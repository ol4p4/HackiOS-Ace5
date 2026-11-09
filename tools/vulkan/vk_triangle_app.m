/*
 * vk_triangle_app: a Vulkan frame in a CoreAnimation drawable, on the panel.
 *
 * WHY IT EXISTS. The Turnip gate (tools/vulkan/turnip_test.c) passed steps 1-12
 * without ever touching the window server: every surface it drew into was one it
 * had made itself, and every frame was read back in the same process. Steps 10
 * and 12 played the compositor's side by hand. The one link nobody has measured
 * is the real one -- a drawable CoreAnimation owns, drawn into by Turnip, and
 * taken to the panel by the image queue. This program is that link.
 *
 * THE CHAIN, and where each half is already proven:
 *
 *   CAMetalLayer nextDrawable          triangle_app --layer, on this port
 *   the drawable's IOSurface           triangle_app reads it out of the private
 *                                      structure, validated against the texture
 *                                      and the device it already holds
 *   IOSurface id -> VkImage import     turnip_test step 11 (the id IS the "fd",
 *                                      turnip-darwin-surface-import.patch)
 *   the draw                           turnip_test steps 8/11 (ir3, the CP)
 *   IOSurfaceLock/Unlock (seed bump)   turnip_test step 12's rule: a writer must
 *                                      announce itself or readers see the last
 *                                      frame
 *   -[CAMetalDrawable present]         this program; presentation never goes
 *                                      through a device -- the surface is
 *                                      registered in the layer's CAImageQueue
 *                                      and the window server takes it from there
 *
 * WHAT IS DRAWN. A green clear with a red triangle, in BGRA. The Metal
 * application (src/triangle_app.m) draws blue with an orange triangle, so a
 * screenshot cannot confuse the two paths: this program's picture is the only
 * one that looks like this. Green and red are also opposite in every channel
 * pair, so a channel swap anywhere between the shader and the panel is seen
 * before it is counted.
 *
 * TWO MODES, one binary, because a boot is expensive:
 *
 *   vk_triangle_app --layer   no UIKit: a bare CAMetalLayer of the panel's size,
 *                             three frames, and every check that does not need
 *                             a window. Runs from the job list, where its output
 *                             is certainly in the log.
 *   vk_triangle_app           the application: UIApplicationMain, a window whose
 *                             view's layer is a CAMetalLayer, a frame every half
 *                             second, so a screenshot can be taken whenever.
 *
 * THE DEVICE. A CAMetalLayer needs a Metal device to allocate drawables from;
 * this program never draws with it. The two roads are src/triangle_app.m's and
 * are taken in its order: Metal's own discovery if it answers, otherwise the
 * RedwoodMetal bundle's -initWithAcceleratorPort: + MTLAddDevice.
 */
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurfaceRef.h>
#import <IOKit/IOKitLib.h>
#import <objc/message.h>
#import <objc/runtime.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <vulkan/vulkan.h>

#include "triangle_spirv.h"

/* The ICD this port builds, and the two Metal constants triangle_app uses (they
 * are not exported, so they are reached by their distance from one that is). */
#define RWVK_ICD_PATH "/redwood/lib/libvulkan_freedreno.dylib"
#define RWVK_BUNDLE "/redwood/lib/RedwoodMetal.bundle"
#define RWVK_MTL_CREATE_DEVICE_UNSLID 0x181a34b3cUL
#define RWVK_MTL_ADD_DEVICE_UNSLID    0x181a1875cUL

/* _CAMetalDrawablePrivate, the two fields this file validates before it believes
 * the third (src/triangle_app.m carries the encoding and docs/metal.md 10.46 the
 * instructions). A layout that moved makes the measurement absent, not wrong. */
#define RWVK_PRIV_SURFACE 0x40
#define RWVK_PRIV_TEXTURE 0x48
#define RWVK_PRIV_DEVICE  0x50

/* The ICD's own door (vulkan/vk_icd.h has it, not vulkan.h). */
typedef PFN_vkVoidFunction (*icd_gipa_t)(VkInstance instance, const char *name);

enum { RWVK_LAYER_W = 1125, RWVK_LAYER_H = 2436, RWVK_ENTRIES = 8 };
enum { RWVK_LAYER_FRAMES = 3, RWVK_APP_PERIOD_MS = 500 };

/* The BGRA bytes the attachment is expected to end up holding. */
static const unsigned char rwvk_clear_bgra[4]    = { 0, 255, 0, 255 };   /* green */
static const unsigned char rwvk_triangle_bgra[4] = { 0, 0, 255, 255 };   /* red */

/* ------------------------------------------------------------------ saying things */

static int rwvk_console = -1;

static void
rwvk_open_console(void)
{
	rwvk_console = open("/dev/console", O_WRONLY);
	if (rwvk_console >= 0) {
		(void)dup2(rwvk_console, 1);
		(void)dup2(rwvk_console, 2);
	}
	setvbuf(stdout, NULL, _IONBF, 0);
}

static void
rwvk_say(const char *format, ...)
{
	char line[1024];
	va_list arguments;
	int n;

	va_start(arguments, format);
	n = vsnprintf(line, sizeof(line) - 2, format, arguments);
	va_end(arguments);
	if (n < 0)
		return;
	if (n > (int)sizeof(line) - 2)
		n = (int)sizeof(line) - 2;
	line[n++] = '\n';
	(void)write(rwvk_console >= 0 ? rwvk_console : 2, line, (size_t)n);
}

static void
rwvk_step(const char *what)
{
	rwvk_say("vktriangle: step: %s", what);
}

/* --------------------------------------------------------------------------- the device */

/*
 * A Metal device, for the drawables only. Same two roads and the same reasons as
 * src/triangle_app.m's rwt_make_device: discovery if it answers, otherwise the
 * bundle and MTLAddDevice, because a layer asks an inherited selector of the
 * device and only registration fills the ivars behind it.
 */
static id<MTLDevice>
rwvk_make_device(void)
{
	id<MTLDevice> found = MTLCreateSystemDefaultDevice();

	if (found != nil) {
		rwvk_say("vktriangle: device: Metal's own discovery gave %s",
		    class_getName(object_getClass(found)));
		return [found retain];
	}
	rwvk_say("vktriangle: device: discovery gave nil, building one from the bundle");

	NSBundle *bundle = [NSBundle bundleWithPath:
	    [NSString stringWithUTF8String:RWVK_BUNDLE]];
	if (bundle == nil) {
		rwvk_say("vktriangle: NOT MEASURED: no bundle at %s", RWVK_BUNDLE);
		return nil;
	}
	Class cls = [bundle classNamed:@"RedwoodMetalDevice"];
	if (cls == Nil)
		cls = [bundle principalClass];
	if (cls == Nil) {
		rwvk_say("vktriangle: NOT MEASURED: the bundle gave up no class");
		return nil;
	}
	io_service_t service = IOServiceGetMatchingService(0,
	    IOServiceMatching("IOAcceleratorES"));
	if (service == IO_OBJECT_NULL) {
		rwvk_say("vktriangle: NOT MEASURED: no IOAcceleratorES service");
		return nil;
	}
	id device = ((id (*)(id, SEL, io_service_t))objc_msgSend)([cls alloc],
	    sel_getUid("initWithAcceleratorPort:"), service);
	if (device == nil) {
		rwvk_say("vktriangle: NOT MEASURED: -initWithAcceleratorPort: gave nil");
		return nil;
	}
	uintptr_t slide = (uintptr_t)&MTLCreateSystemDefaultDevice -
	    RWVK_MTL_CREATE_DEVICE_UNSLID;
	void (*add_device)(id) = (void (*)(id))(slide + RWVK_MTL_ADD_DEVICE_UNSLID);
	@try {
		add_device(device);
	} @catch (NSException *exception) {
		rwvk_say("vktriangle: FAILED: MTLAddDevice threw %s: %s",
		    [[exception name] UTF8String], [[exception reason] UTF8String]);
		return nil;
	}
	id<MTLDevice> system = MTLCreateSystemDefaultDevice();
	if (system == nil) {
		rwvk_say("vktriangle: FAILED: MTLAddDevice took the device and Metal still "
		    "has none");
		return nil;
	}
	rwvk_say("vktriangle: device: built in this process, %s",
	    class_getName(object_getClass(system)));
	return [system retain];
}

/* ----------------------------------------------------- the IOSurface behind the drawable */

/*
 * The drawable's own IOSurface, through CAMetalDrawable's one ivar and validated
 * exactly as src/triangle_app.m does it: the structure is believed only after the
 * texture and the device it holds turn out to be the two this program already has.
 * The driver's texture answers nil to -iosurface (redwoodmetal.m), so there is no
 * way to ask Metal.
 */
static IOSurfaceRef
rwvk_surface_of_drawable(id drawable, id texture, id device)
{
	unsigned count = 0;
	Ivar *ivars = class_copyIvarList(object_getClass(drawable), &count);
	IOSurfaceRef surface = NULL;

	for (unsigned i = 0; i < count && surface == NULL; i++) {
		const char *type = ivar_getTypeEncoding(ivars[i]);
		char *private;

		if (type == NULL || strstr(type, "_CAMetalDrawablePrivate") == NULL)
			continue;
		private = *(char **)((char *)drawable + ivar_getOffset(ivars[i]));
		if (private == NULL)
			continue;
		if (*(id *)(private + RWVK_PRIV_TEXTURE) != texture) {
			rwvk_say("vktriangle: surface: %s holds a structure whose texture at "
			    "+%#x is not this drawable's, so its layout has moved",
			    ivar_getName(ivars[i]), RWVK_PRIV_TEXTURE);
			continue;
		}
		if (*(id *)(private + RWVK_PRIV_DEVICE) != device) {
			rwvk_say("vktriangle: surface: %s holds the right texture at +%#x and "
			    "the wrong device at +%#x, so the layout is half right",
			    ivar_getName(ivars[i]), RWVK_PRIV_TEXTURE, RWVK_PRIV_DEVICE);
			continue;
		}
		surface = *(IOSurfaceRef *)(private + RWVK_PRIV_SURFACE);
	}
	free(ivars);
	return surface;
}

/* --------------------------------------------------------------------------- the Vulkan side */

struct rwvk_entry {
	uint32_t sid;
	uint32_t width, height;
	VkImage image;
	VkDeviceMemory memory;
	VkImageView view;
	VkFramebuffer framebuffer;
};

struct rwvk {
	VkInstance instance;
	VkPhysicalDevice physical;
	VkDevice device;
	VkQueue queue;
	uint32_t family;
	VkCommandPool pool;
	VkCommandBuffer commands;
	VkFence fence;
	VkRenderPass pass;
	VkPipelineLayout layout;
	VkPipeline pipeline;
	VkShaderModule vertex, fragment;
	VkPhysicalDeviceMemoryProperties memory;
	struct rwvk_entry entries[RWVK_ENTRIES];
	unsigned entries_used;
	unsigned frames, failures;
};

/* The process's one Vulkan context, its Metal device and the queue the present
 * rides. Declared here because rwvk_frame uses the queue: the drawable's own
 * -present is not enough on this port (the first on-panel run stayed black). */
static struct rwvk rwvk_vulkan;
static id<MTLDevice> rwvk_metal;
static id<MTLCommandQueue> rwvk_metal_queue;

static PFN_vkGetInstanceProcAddr rwvk_gipa_instance;
static PFN_vkGetDeviceProcAddr rwvk_gdpa_device;

#define RWVK_GIPA(instance, name, var) do { \
	(var) = (__typeof__(var))rwvk_gipa_instance((instance), (name)); \
	if ((var) == NULL) { rwvk_say("vktriangle: FAILED: no %s", (name)); return -1; } \
} while (0)
#define RWVK_GDPA(device, name, var) do { \
	(var) = (__typeof__(var))rwvk_gdpa_device((device), (name)); \
	if ((var) == NULL) { rwvk_say("vktriangle: FAILED: no %s", (name)); return -1; } \
} while (0)

static PFN_vkDestroyInstance pDestroyInstance;
static PFN_vkEnumeratePhysicalDevices pEnumeratePhysicalDevices;
static PFN_vkGetPhysicalDeviceProperties pGetPhysicalDeviceProperties;
static PFN_vkGetPhysicalDeviceQueueFamilyProperties pGetQueueFamilyProperties;
static PFN_vkGetPhysicalDeviceMemoryProperties pGetPhysicalDeviceMemoryProperties;
static PFN_vkCreateDevice pCreateDevice;
static PFN_vkDestroyDevice pDestroyDevice;
static PFN_vkGetDeviceQueue pGetDeviceQueue;
static PFN_vkCreateCommandPool pCreateCommandPool;
static PFN_vkDestroyCommandPool pDestroyCommandPool;
static PFN_vkAllocateCommandBuffers pAllocateCommandBuffers;
static PFN_vkResetCommandBuffer pResetCommandBuffer;
static PFN_vkBeginCommandBuffer pBeginCommandBuffer;
static PFN_vkEndCommandBuffer pEndCommandBuffer;
static PFN_vkCreateFence pCreateFence;
static PFN_vkDestroyFence pDestroyFence;
static PFN_vkWaitForFences pWaitForFences;
static PFN_vkResetFences pResetFences;
static PFN_vkQueueSubmit pQueueSubmit;
static PFN_vkCreateShaderModule pCreateShaderModule;
static PFN_vkDestroyShaderModule pDestroyShaderModule;
static PFN_vkCreateImageView pCreateImageView;
static PFN_vkDestroyImageView pDestroyImageView;
static PFN_vkCreateRenderPass pCreateRenderPass;
static PFN_vkDestroyRenderPass pDestroyRenderPass;
static PFN_vkCreateFramebuffer pCreateFramebuffer;
static PFN_vkDestroyFramebuffer pDestroyFramebuffer;
static PFN_vkCreatePipelineLayout pCreatePipelineLayout;
static PFN_vkDestroyPipelineLayout pDestroyPipelineLayout;
static PFN_vkCreateGraphicsPipelines pCreateGraphicsPipelines;
static PFN_vkDestroyPipeline pDestroyPipeline;
static PFN_vkCreateImage pCreateImage;
static PFN_vkDestroyImage pDestroyImage;
static PFN_vkGetImageMemoryRequirements pGetImageMemoryRequirements;
static PFN_vkBindImageMemory pBindImageMemory;
static PFN_vkAllocateMemory pAllocateMemory;
static PFN_vkFreeMemory pFreeMemory;
static PFN_vkCmdBeginRenderPass pCmdBeginRenderPass;
static PFN_vkCmdEndRenderPass pCmdEndRenderPass;
static PFN_vkCmdBindPipeline pCmdBindPipeline;
static PFN_vkCmdDraw pCmdDraw;
static PFN_vkCmdSetViewport pCmdSetViewport;
static PFN_vkCmdSetScissor pCmdSetScissor;

/* The clear colour changes every eight frames, so two screenshots taken apart
 * are two different pictures and a still frame is told from a live present. */
static void
rwvk_clear_for(unsigned frame, VkClearValue *clear)
{
	if ((frame / 8) & 1) {
		/* cyan: the second picture, still nowhere near the Metal app's colours */
		clear->color.float32[0] = 0.0f;
		clear->color.float32[1] = 1.0f;
		clear->color.float32[2] = 1.0f;
		clear->color.float32[3] = 1.0f;
	} else {
		clear->color.float32[0] = 0.0f;
		clear->color.float32[1] = 1.0f;
		clear->color.float32[2] = 0.0f;
		clear->color.float32[3] = 1.0f;
	}
}

static int
rwvk_open(struct rwvk *v)
{
	void *library = dlopen(RWVK_ICD_PATH, RTLD_NOW);
	icd_gipa_t gipa;
	VkResult result;
	VkApplicationInfo app = {
	    .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
	    .pApplicationName = "vk_triangle_app",
	    .applicationVersion = 1,
	    .pEngineName = "redwood",
	    .engineVersion = 1,
	    .apiVersion = VK_API_VERSION_1_0,
	};
	VkInstanceCreateInfo instanceInfo = {
	    .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
	    .pApplicationInfo = &app,
	};
	uint32_t count = 0, families = 0;

	memset(v, 0, sizeof(*v));
	if (library == NULL) {
		rwvk_say("vktriangle: FAILED: %s: %s", RWVK_ICD_PATH, dlerror());
		return -1;
	}
	gipa = (icd_gipa_t)dlsym(library, "vk_icdGetInstanceProcAddr");
	if (gipa == NULL) {
		rwvk_say("vktriangle: FAILED: no vk_icdGetInstanceProcAddr");
		return -1;
	}
	PFN_vkCreateInstance createInstance = (PFN_vkCreateInstance)gipa(NULL,
	    "vkCreateInstance");
	if (createInstance == NULL) {
		rwvk_say("vktriangle: FAILED: no vkCreateInstance through the ICD");
		return -1;
	}
	result = createInstance(&instanceInfo, NULL, &v->instance);
	if (result != VK_SUCCESS) {
		rwvk_say("vktriangle: FAILED: vkCreateInstance %d", result);
		return -1;
	}
	rwvk_gipa_instance = (PFN_vkGetInstanceProcAddr)gipa(v->instance,
	    "vkGetInstanceProcAddr");
	if (rwvk_gipa_instance == NULL) {
		rwvk_say("vktriangle: FAILED: no vkGetInstanceProcAddr");
		return -1;
	}
	RWVK_GIPA(v->instance, "vkDestroyInstance", pDestroyInstance);
	RWVK_GIPA(v->instance, "vkEnumeratePhysicalDevices", pEnumeratePhysicalDevices);
	RWVK_GIPA(v->instance, "vkGetPhysicalDeviceProperties", pGetPhysicalDeviceProperties);
	RWVK_GIPA(v->instance, "vkGetPhysicalDeviceQueueFamilyProperties", pGetQueueFamilyProperties);
	RWVK_GIPA(v->instance, "vkGetPhysicalDeviceMemoryProperties", pGetPhysicalDeviceMemoryProperties);
	RWVK_GIPA(v->instance, "vkCreateDevice", pCreateDevice);

	if (pEnumeratePhysicalDevices(v->instance, &count, NULL) != VK_SUCCESS || count == 0) {
		rwvk_say("vktriangle: FAILED: no physical device");
		return -1;
	}
	VkPhysicalDevice *devices = calloc(count, sizeof(*devices));
	if (devices == NULL)
		return -1;
	if (pEnumeratePhysicalDevices(v->instance, &count, devices) != VK_SUCCESS) {
		free(devices);
		return -1;
	}
	v->physical = devices[0];
	{
		VkPhysicalDeviceProperties properties;
		pGetPhysicalDeviceProperties(v->physical, &properties);
		rwvk_say("vktriangle: device 0: \"%s\", vendor %#x, API %u.%u.%u",
		    properties.deviceName, properties.vendorID,
		    VK_API_VERSION_MAJOR(properties.apiVersion),
		    VK_API_VERSION_MINOR(properties.apiVersion),
		    VK_API_VERSION_PATCH(properties.apiVersion));
	}
	free(devices);

	pGetQueueFamilyProperties(v->physical, &families, NULL);
	VkQueueFamilyProperties *queues = calloc(families ? families : 1, sizeof(*queues));
	if (queues == NULL)
		return -1;
	pGetQueueFamilyProperties(v->physical, &families, queues);
	for (uint32_t i = 0; i < families; i++) {
		if ((queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && queues[i].queueCount > 0) {
			v->family = i;
			break;
		}
	}
	free(queues);
	if (v->family == UINT32_MAX && families > 0) {
		/* the first family that exists is the fallback */
		v->family = 0;
	}
	if (families == 0) {
		rwvk_say("vktriangle: FAILED: no queue family");
		return -1;
	}

	float priority = 1.0f;
	VkDeviceQueueCreateInfo queueInfo = {
	    .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
	    .queueFamilyIndex = v->family,
	    .queueCount = 1,
	    .pQueuePriorities = &priority,
	};
	VkDeviceCreateInfo deviceInfo = {
	    .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
	    .queueCreateInfoCount = 1,
	    .pQueueCreateInfos = &queueInfo,
	};
	if (pCreateDevice(v->physical, &deviceInfo, NULL, &v->device) != VK_SUCCESS) {
		rwvk_say("vktriangle: FAILED: vkCreateDevice");
		return -1;
	}
	rwvk_gdpa_device = (PFN_vkGetDeviceProcAddr)rwvk_gipa_instance(v->instance,
	    "vkGetDeviceProcAddr");
	if (rwvk_gdpa_device == NULL) {
		rwvk_say("vktriangle: FAILED: no vkGetDeviceProcAddr");
		return -1;
	}
	RWVK_GDPA(v->device, "vkDestroyDevice", pDestroyDevice);
	RWVK_GDPA(v->device, "vkGetDeviceQueue", pGetDeviceQueue);
	RWVK_GDPA(v->device, "vkCreateCommandPool", pCreateCommandPool);
	RWVK_GDPA(v->device, "vkDestroyCommandPool", pDestroyCommandPool);
	RWVK_GDPA(v->device, "vkAllocateCommandBuffers", pAllocateCommandBuffers);
	RWVK_GDPA(v->device, "vkResetCommandBuffer", pResetCommandBuffer);
	RWVK_GDPA(v->device, "vkBeginCommandBuffer", pBeginCommandBuffer);
	RWVK_GDPA(v->device, "vkEndCommandBuffer", pEndCommandBuffer);
	RWVK_GDPA(v->device, "vkCreateFence", pCreateFence);
	RWVK_GDPA(v->device, "vkDestroyFence", pDestroyFence);
	RWVK_GDPA(v->device, "vkWaitForFences", pWaitForFences);
	RWVK_GDPA(v->device, "vkResetFences", pResetFences);
	RWVK_GDPA(v->device, "vkQueueSubmit", pQueueSubmit);
	RWVK_GDPA(v->device, "vkCreateShaderModule", pCreateShaderModule);
	RWVK_GDPA(v->device, "vkDestroyShaderModule", pDestroyShaderModule);
	RWVK_GDPA(v->device, "vkCreateImageView", pCreateImageView);
	RWVK_GDPA(v->device, "vkDestroyImageView", pDestroyImageView);
	RWVK_GDPA(v->device, "vkCreateRenderPass", pCreateRenderPass);
	RWVK_GDPA(v->device, "vkDestroyRenderPass", pDestroyRenderPass);
	RWVK_GDPA(v->device, "vkCreateFramebuffer", pCreateFramebuffer);
	RWVK_GDPA(v->device, "vkDestroyFramebuffer", pDestroyFramebuffer);
	RWVK_GDPA(v->device, "vkCreatePipelineLayout", pCreatePipelineLayout);
	RWVK_GDPA(v->device, "vkDestroyPipelineLayout", pDestroyPipelineLayout);
	RWVK_GDPA(v->device, "vkCreateGraphicsPipelines", pCreateGraphicsPipelines);
	RWVK_GDPA(v->device, "vkDestroyPipeline", pDestroyPipeline);
	RWVK_GDPA(v->device, "vkCreateImage", pCreateImage);
	RWVK_GDPA(v->device, "vkDestroyImage", pDestroyImage);
	RWVK_GDPA(v->device, "vkGetImageMemoryRequirements", pGetImageMemoryRequirements);
	RWVK_GDPA(v->device, "vkBindImageMemory", pBindImageMemory);
	RWVK_GDPA(v->device, "vkAllocateMemory", pAllocateMemory);
	RWVK_GDPA(v->device, "vkFreeMemory", pFreeMemory);
	RWVK_GDPA(v->device, "vkCmdBeginRenderPass", pCmdBeginRenderPass);
	RWVK_GDPA(v->device, "vkCmdEndRenderPass", pCmdEndRenderPass);
	RWVK_GDPA(v->device, "vkCmdBindPipeline", pCmdBindPipeline);
	RWVK_GDPA(v->device, "vkCmdDraw", pCmdDraw);
	RWVK_GDPA(v->device, "vkCmdSetViewport", pCmdSetViewport);
	RWVK_GDPA(v->device, "vkCmdSetScissor", pCmdSetScissor);

	pGetDeviceQueue(v->device, v->family, 0, &v->queue);
	pGetPhysicalDeviceMemoryProperties(v->physical, &v->memory);

	VkCommandPoolCreateInfo poolInfo = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
	    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
	    .queueFamilyIndex = v->family,
	};
	if (pCreateCommandPool(v->device, &poolInfo, NULL, &v->pool) != VK_SUCCESS) {
		rwvk_say("vktriangle: FAILED: vkCreateCommandPool");
		return -1;
	}
	VkCommandBufferAllocateInfo commandsInfo = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
	    .commandPool = v->pool,
	    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
	    .commandBufferCount = 1,
	};
	if (pAllocateCommandBuffers(v->device, &commandsInfo, &v->commands) != VK_SUCCESS) {
		rwvk_say("vktriangle: FAILED: vkAllocateCommandBuffers");
		return -1;
	}
	VkFenceCreateInfo fenceInfo = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	if (pCreateFence(v->device, &fenceInfo, NULL, &v->fence) != VK_SUCCESS) {
		rwvk_say("vktriangle: FAILED: vkCreateFence");
		return -1;
	}

	/* One BGRA8 colour attachment, cleared and stored: the drawable's format. */
	VkAttachmentDescription attachment = {
	    .format = VK_FORMAT_B8G8R8A8_UNORM,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
	    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
	    .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
	    .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	    .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	};
	VkAttachmentReference reference = {
	    .attachment = 0,
	    .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	};
	VkSubpassDescription subpass = {
	    .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
	    .colorAttachmentCount = 1,
	    .pColorAttachments = &reference,
	};
	VkRenderPassCreateInfo passInfo = {
	    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
	    .attachmentCount = 1,
	    .pAttachments = &attachment,
	    .subpassCount = 1,
	    .pSubpasses = &subpass,
	};
	if (pCreateRenderPass(v->device, &passInfo, NULL, &v->pass) != VK_SUCCESS) {
		rwvk_say("vktriangle: FAILED: vkCreateRenderPass");
		return -1;
	}

	VkShaderModuleCreateInfo vertexInfo = {
	    .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
	    .codeSize = sizeof(triangle_vert_spirv),
	    .pCode = triangle_vert_spirv,
	};
	VkShaderModuleCreateInfo fragmentInfo = {
	    .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
	    .codeSize = sizeof(triangle_frag_spirv),
	    .pCode = triangle_frag_spirv,
	};
	if (pCreateShaderModule(v->device, &vertexInfo, NULL, &v->vertex) != VK_SUCCESS ||
	    pCreateShaderModule(v->device, &fragmentInfo, NULL, &v->fragment) != VK_SUCCESS) {
		rwvk_say("vktriangle: FAILED: vkCreateShaderModule");
		return -1;
	}
	VkPipelineShaderStageCreateInfo stages[2] = {
	    { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	      .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = v->vertex, .pName = "main" },
	    { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	      .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = v->fragment, .pName = "main" },
	};
	VkPipelineVertexInputStateCreateInfo vertexInput = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
	};
	VkPipelineInputAssemblyStateCreateInfo inputAssembly = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
	    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
	};
	/* The viewport is the drawable's size and is different every mode: dynamic,
	 * so one pipeline serves all of them. */
	VkDynamicState dynamicStates[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dynamic = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
	    .dynamicStateCount = 2,
	    .pDynamicStates = dynamicStates,
	};
	VkViewport viewport = { 0.0f, 0.0f, (float)RWVK_LAYER_W, (float)RWVK_LAYER_H, 0.0f, 1.0f };
	VkRect2D scissor = { { 0, 0 }, { RWVK_LAYER_W, RWVK_LAYER_H } };
	VkPipelineViewportStateCreateInfo viewportState = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
	    .viewportCount = 1,
	    .pViewports = &viewport,
	    .scissorCount = 1,
	    .pScissors = &scissor,
	};
	VkPipelineRasterizationStateCreateInfo raster = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
	    .polygonMode = VK_POLYGON_MODE_FILL,
	    .cullMode = VK_CULL_MODE_NONE,
	    .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
	    .lineWidth = 1.0f,
	};
	VkPipelineMultisampleStateCreateInfo multisample = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
	    .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
	};
	VkPipelineColorBlendAttachmentState blendAttachment = {
	    .blendEnable = VK_FALSE,
	    .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
	                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
	};
	VkPipelineColorBlendStateCreateInfo blend = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
	    .attachmentCount = 1,
	    .pAttachments = &blendAttachment,
	};
	VkPipelineLayoutCreateInfo layoutInfo = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
	};
	if (pCreatePipelineLayout(v->device, &layoutInfo, NULL, &v->layout) != VK_SUCCESS) {
		rwvk_say("vktriangle: FAILED: vkCreatePipelineLayout");
		return -1;
	}
	VkGraphicsPipelineCreateInfo pipelineInfo = {
	    .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
	    .stageCount = 2,
	    .pStages = stages,
	    .pVertexInputState = &vertexInput,
	    .pInputAssemblyState = &inputAssembly,
	    .pViewportState = &viewportState,
	    .pRasterizationState = &raster,
	    .pMultisampleState = &multisample,
	    .pColorBlendState = &blend,
	    .pDynamicState = &dynamic,
	    .layout = v->layout,
	    .renderPass = v->pass,
	    .subpass = 0,
	};
	result = pCreateGraphicsPipelines(v->device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL,
	    &v->pipeline);
	if (result != VK_SUCCESS) {
		rwvk_say("vktriangle: FAILED: vkCreateGraphicsPipelines %d", result);
		return -1;
	}
	rwvk_say("vktriangle: Vulkan up: %s, our B8G8R8A8 pass, the red-triangle pipeline",
	    "Turnip");
	return 0;
}

/* The image over one drawable's surface, made once per surface id: a layer
 * rotates through two or three drawables, so the cache is a handful of entries
 * and a surface never changes size under one id. */
static struct rwvk_entry *
rwvk_entry_for(struct rwvk *v, IOSurfaceRef surface, uint32_t width, uint32_t height)
{
	uint32_t sid = (uint32_t)IOSurfaceGetID(surface);

	for (unsigned i = 0; i < v->entries_used; i++) {
		if (v->entries[i].sid == sid)
			return &v->entries[i];
	}
	if (v->entries_used >= RWVK_ENTRIES) {
		rwvk_say("vktriangle: FAILED: more than %u drawable surfaces in rotation",
		    RWVK_ENTRIES);
		return NULL;
	}
	struct rwvk_entry *entry = &v->entries[v->entries_used];
	VkExternalMemoryImageCreateInfo external = {
	    .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
	    .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
	};
	VkImageCreateInfo imageInfo = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	    .pNext = &external,
	    .imageType = VK_IMAGE_TYPE_2D,
	    /* the drawable's surface is BGRA in memory; declaring anything else
	     * puts a channel swap into the pixels (turnip_test step 11) */
	    .format = VK_FORMAT_B8G8R8A8_UNORM,
	    .extent = { width, height, 1 },
	    .mipLevels = 1,
	    .arrayLayers = 1,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .tiling = VK_IMAGE_TILING_LINEAR,
	    .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
	    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	VkMemoryRequirements requirements;
	uint32_t type = UINT32_MAX;

	memset(entry, 0, sizeof(*entry));
	entry->sid = sid;
	entry->width = width;
	entry->height = height;
	if (pCreateImage(v->device, &imageInfo, NULL, &entry->image) != VK_SUCCESS) {
		rwvk_say("vktriangle: FAILED: vkCreateImage over surface %u", sid);
		return NULL;
	}
	pGetImageMemoryRequirements(v->device, entry->image, &requirements);
	for (uint32_t i = 0; i < v->memory.memoryTypeCount; i++) {
		if ((requirements.memoryTypeBits & (1u << i)) &&
		    !(v->memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
			type = i;
			break;
		}
	}
	if (type == UINT32_MAX) {
		/* every heap on this driver is host-visible system memory */
		for (uint32_t i = 0; i < v->memory.memoryTypeCount; i++) {
			if (requirements.memoryTypeBits & (1u << i)) {
				type = i;
				break;
			}
		}
	}
	if (type == UINT32_MAX) {
		rwvk_say("vktriangle: FAILED: no memory type for surface %u", sid);
		return NULL;
	}
	VkImportMemoryFdInfoKHR import = {
	    .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
	    .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
	    /* the port's "fd" for a surface is its id (turnip-darwin-surface-import) */
	    .fd = (int)sid,
	};
	VkMemoryAllocateInfo allocate = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .pNext = &import,
	    .allocationSize = requirements.size,
	    .memoryTypeIndex = type,
	};
	if (pAllocateMemory(v->device, &allocate, NULL, &entry->memory) != VK_SUCCESS) {
		rwvk_say("vktriangle: FAILED: vkAllocateMemory importing surface %u", sid);
		return NULL;
	}
	if (pBindImageMemory(v->device, entry->image, entry->memory, 0) != VK_SUCCESS) {
		rwvk_say("vktriangle: FAILED: vkBindImageMemory for surface %u", sid);
		return NULL;
	}
	VkImageViewCreateInfo viewInfo = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
	    .image = entry->image,
	    .viewType = VK_IMAGE_VIEW_TYPE_2D,
	    .format = VK_FORMAT_B8G8R8A8_UNORM,
	    .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
	};
	if (pCreateImageView(v->device, &viewInfo, NULL, &entry->view) != VK_SUCCESS) {
		rwvk_say("vktriangle: FAILED: vkCreateImageView for surface %u", sid);
		return NULL;
	}
	VkFramebufferCreateInfo framebufferInfo = {
	    .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
	    .renderPass = v->pass,
	    .attachmentCount = 1,
	    .pAttachments = &entry->view,
	    .width = width,
	    .height = height,
	    .layers = 1,
	};
	if (pCreateFramebuffer(v->device, &framebufferInfo, NULL, &entry->framebuffer) != VK_SUCCESS) {
		rwvk_say("vktriangle: FAILED: vkCreateFramebuffer for surface %u", sid);
		return NULL;
	}
	v->entries_used++;
	rwvk_say("vktriangle: surface %u imported: %ux%u, %zu bytes a row, %llu bytes of "
	    "memory, type %u", sid, width, height, IOSurfaceGetBytesPerRow(surface),
	    (unsigned long long)requirements.size, type);
	return entry;
}

/*
 * One frame: draw into the surface, announce the write, present it, then read it
 * back through the surface the display will read. The read is the honest check
 * (two views of the same pages), the present is what puts it on the panel.
 */
static int
rwvk_frame(struct rwvk *v, CAMetalLayer *layer, int loud)
{
	@autoreleasepool {
		if (loud)
			rwvk_step("nextDrawable");
		id<CAMetalDrawable> drawable = [layer nextDrawable];
		if (drawable == nil) {
			v->failures++;
			rwvk_say("vktriangle: frame %u: nextDrawable gave nil (pool of %lu "
			    "still in flight?)", v->frames + 1,
			    (unsigned long)[layer maximumDrawableCount]);
			return -1;
		}
		id<MTLTexture> texture = [drawable texture];
		if (texture == nil) {
			v->failures++;
			rwvk_say("vktriangle: frame %u: the drawable's texture is nil",
			    v->frames + 1);
			return -1;
		}
		uint32_t width = (uint32_t)[texture width];
		uint32_t height = (uint32_t)[texture height];
		IOSurfaceRef surface = rwvk_surface_of_drawable(drawable, texture,
		    layer.device);

		v->frames++;
		if (surface == NULL) {
			v->failures++;
			rwvk_say("vktriangle: frame %u: the drawable's IOSurface could not be "
			    "reached", v->frames);
			return -1;
		}
		if (loud)
			rwvk_say("vktriangle: frame %u: drawable %s, texture %s, %ux%u, "
			    "surface id %u, stride %zu, format %#x", v->frames,
			    class_getName(object_getClass(drawable)),
			    class_getName(object_getClass(texture)), width, height,
			    (unsigned)IOSurfaceGetID(surface),
			    IOSurfaceGetBytesPerRow(surface),
			    (unsigned)IOSurfaceGetPixelFormat(surface));
		if (width == 0 || height == 0) {
			v->failures++;
			return -1;
		}
		struct rwvk_entry *entry = rwvk_entry_for(v, surface, width, height);
		if (entry == NULL) {
			v->failures++;
			return -1;
		}
		if (pResetCommandBuffer(v->commands, 0) != VK_SUCCESS ||
		    pBeginCommandBuffer(v->commands,
			&(VkCommandBufferBeginInfo){ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }) != VK_SUCCESS) {
			v->failures++;
			rwvk_say("vktriangle: frame %u: the command buffer would not begin",
			    v->frames);
			return -1;
		}
		VkClearValue clear;
		rwvk_clear_for(v->frames, &clear);
		VkRenderPassBeginInfo begin = {
		    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		    .renderPass = v->pass,
		    .framebuffer = entry->framebuffer,
		    .renderArea = { .offset = { 0, 0 }, .extent = { width, height } },
		    .clearValueCount = 1,
		    .pClearValues = &clear,
		};
		VkViewport viewport = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
		VkRect2D scissor = { { 0, 0 }, { width, height } };
		pCmdBeginRenderPass(v->commands, &begin, VK_SUBPASS_CONTENTS_INLINE);
		pCmdBindPipeline(v->commands, VK_PIPELINE_BIND_POINT_GRAPHICS, v->pipeline);
		pCmdSetViewport(v->commands, 0, 1, &viewport);
		pCmdSetScissor(v->commands, 0, 1, &scissor);
		pCmdDraw(v->commands, 3, 1, 0, 0);
		pCmdEndRenderPass(v->commands);
		if (pEndCommandBuffer(v->commands) != VK_SUCCESS) {
			v->failures++;
			return -1;
		}
		VkSubmitInfo submit = {
		    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		    .commandBufferCount = 1,
		    .pCommandBuffers = &v->commands,
		};
		if (pQueueSubmit(v->queue, 1, &submit, v->fence) != VK_SUCCESS) {
			v->failures++;
			rwvk_say("vktriangle: frame %u: vkQueueSubmit failed", v->frames);
			return -1;
		}
		if (pWaitForFences(v->device, 1, &v->fence, VK_TRUE, 3000000000ull) != VK_SUCCESS) {
			v->failures++;
			rwvk_say("vktriangle: frame %u: the fence never signalled (3 s)",
			    v->frames);
			return -1;
		}
		pResetFences(v->device, 1, &v->fence);

		/* THE SEED RULE (turnip_test step 12): the kernel invalidates this
		 * processor's cached lines of a surface only when its seed moves, so
		 * whoever wrote must say so or readers see the previous frame. */
		if (IOSurfaceLock(surface, 0, NULL) == 0)
			IOSurfaceUnlock(surface, 0, NULL);

		/* Presentation. The drawable's own -present is not enough on this
		 * port: the first on-panel run left the window black while the
		 * surface held the right pixels (screenshot 20261109-202913). Metal's
		 * path is Apple's -[MTLCommandBuffer presentDrawable:] and a commit:
		 * the present is a scheduled handler that -commit delivers
		 * (src/triangle_app.m says the same). The buffer carries no encoder,
		 * so the driver's -commit has nothing to draw. */
		if (loud)
			rwvk_step("-[MTLCommandBuffer presentDrawable:] + commit");
		id<MTLCommandBuffer> commit = [rwvk_metal_queue commandBuffer];
		if (commit == nil) {
			v->failures++;
			rwvk_say("vktriangle: frame %u: the queue gave no command buffer",
			    v->frames);
			return -1;
		}
		[commit presentDrawable:drawable];
		[commit commit];
		[commit waitUntilCompleted];

		/* Read back through the surface, the same pages the panel reads. */
		if (IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL) == 0) {
			const uint8_t *bytes = IOSurfaceGetBaseAddress(surface);
			size_t stride = IOSurfaceGetBytesPerRow(surface);
			const uint8_t *corner = bytes + stride + 4;          /* (1,1) */
			const uint8_t *middle = bytes + (size_t)(height / 2) * stride + (width / 2) * 4;
			/* the clear alternates every eight frames (rwvk_clear_for), and the
			 * corner check knows which picture this frame asked for */
			int cyan = ((v->frames / 8) & 1) != 0;
			int corner_ok = corner[2] < 40 && corner[1] > 200 &&
			    (cyan ? corner[0] > 200 : corner[0] < 40);
			int middle_ok = middle[2] > 200 && middle[0] < 40 && middle[1] < 40;

			IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
			if (loud)
				rwvk_say("vktriangle: frame %u: corner BGRA %u %u %u %u, "
				    "middle BGRA %u %u %u %u", v->frames,
				    corner[0], corner[1], corner[2], corner[3],
				    middle[0], middle[1], middle[2], middle[3]);
			if (!corner_ok || !middle_ok) {
				v->failures++;
				rwvk_say("vktriangle: frame %u: FAILED: the drawable does not "
				    "hold the draw (corner %s, middle %s)", v->frames,
				    corner_ok ? "ok" : "wrong", middle_ok ? "ok" : "wrong");
				return -1;
			}
		} else {
			rwvk_say("vktriangle: frame %u: surface readback lock failed", v->frames);
			v->failures++;
			return -1;
		}
		return 0;
	}
}

static void
rwvk_close(struct rwvk *v)
{
	for (unsigned i = 0; i < v->entries_used; i++) {
		if (v->entries[i].framebuffer)
			pDestroyFramebuffer(v->device, v->entries[i].framebuffer, NULL);
		if (v->entries[i].view)
			pDestroyImageView(v->device, v->entries[i].view, NULL);
		if (v->entries[i].image)
			pDestroyImage(v->device, v->entries[i].image, NULL);
		if (v->entries[i].memory)
			pFreeMemory(v->device, v->entries[i].memory, NULL);
	}
	if (v->pipeline)
		pDestroyPipeline(v->device, v->pipeline, NULL);
	if (v->layout)
		pDestroyPipelineLayout(v->device, v->layout, NULL);
	if (v->vertex)
		pDestroyShaderModule(v->device, v->vertex, NULL);
	if (v->fragment)
		pDestroyShaderModule(v->device, v->fragment, NULL);
	if (v->pass)
		pDestroyRenderPass(v->device, v->pass, NULL);
	if (v->fence)
		pDestroyFence(v->device, v->fence, NULL);
	if (v->pool)
		pDestroyCommandPool(v->device, v->pool, NULL);
	if (v->device)
		pDestroyDevice(v->device, NULL);
	if (v->instance)
		pDestroyInstance(v->instance, NULL);
}

/* --------------------------------------------------------------------------- the application */

@interface RWVKView : UIView
@end

@implementation RWVKView
+ (Class)layerClass
{
	return [CAMetalLayer class];
}
@end

@interface RWVKController : UIViewController
@end

static CAMetalLayer *rwvk_layer;
static NSTimer *rwvk_timer;
static unsigned rwvk_app_frames;

@implementation RWVKController

/* The view's class IS the program: a UIViewController's default view is a plain
 * UIView with a CALayer, and -setDevice: on that is an unrecognized selector
 * (measured on the first run: RedwoodVK[39] died in startFrames). RWVKView's
 * +layerClass is what makes self.view.layer a CAMetalLayer. */
- (void)loadView
{
	self.view = [[RWVKView alloc] initWithFrame:[UIScreen mainScreen].bounds];
}

- (void)viewDidLoad
{
	[super viewDidLoad];
	/* UIKit installs the view's layer as the window's backing layer, and the
	 * drawable's size is the layer's size times its content scale. */
}

- (void)viewDidAppear:(BOOL)animated
{
	[super viewDidAppear:animated];
	[self startFrames];
}

- (void)startFrames
{
	if (rwvk_timer != nil)
		return;
	CAMetalLayer *layer = (CAMetalLayer *)self.view.layer;

	rwvk_layer = layer;
	layer.device = rwvk_metal;
	layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
	/* drawableSize, contentsScale and framebufferOnly are left exactly as
	 * src/triangle_app.m leaves them: CA works the drawable out as the layer's
	 * size x its content scale, and on this port that is 375x812 for this
	 * window (docs/ACE5.md, "20261023-145230"). Forcing 1125x2436 here is what
	 * kept the second on-panel run black while every readback was right. */
	rwvk_say("vktriangle: app: the window is up, layer %s, bounds %.0fx%.0f at scale "
	    "%.1f (drawableSize %.0fx%.0f, CA's to choose)", class_getName(object_getClass(layer)),
	    layer.bounds.size.width, layer.bounds.size.height, layer.contentsScale,
	    layer.drawableSize.width, layer.drawableSize.height);
	rwvk_timer = [NSTimer scheduledTimerWithTimeInterval:RWVK_APP_PERIOD_MS / 1000.0
	    repeats:YES block:^(NSTimer *timer) {
		int loud = rwvk_app_frames < 3 || (rwvk_app_frames % 10) == 0;

		rwvk_app_frames++;
		rwvk_frame(&rwvk_vulkan, rwvk_layer, loud);
		if (loud)
			rwvk_say("vktriangle: app: frame %u done, %u failure(s) so far",
			    rwvk_app_frames, rwvk_vulkan.failures);
	}];
}

@end

@interface RWVKDelegate : NSObject <UIApplicationDelegate>
@property (nonatomic, strong) UIWindow *window;
@end

@implementation RWVKDelegate

- (BOOL)application:(UIApplication *)application
    didFinishLaunchingWithOptions:(NSDictionary *)options
{
	rwvk_say("vktriangle: app: didFinishLaunchingWithOptions");
	rwvk_open_console();
	rwvk_metal = rwvk_make_device();
	if (rwvk_metal == nil) {
		rwvk_say("vktriangle: FAILED: no Metal device, so no drawable to draw into");
		return YES;
	}
	rwvk_metal_queue = [rwvk_metal newCommandQueue];
	if (rwvk_metal_queue == nil) {
		rwvk_say("vktriangle: FAILED: the device gave no command queue, so no present");
		return YES;
	}
	if (rwvk_open(&rwvk_vulkan) != 0)
		return YES;
	self.window = [[UIWindow alloc] initWithFrame:[UIScreen mainScreen].bounds];
	self.window.rootViewController = [[RWVKController alloc] init];
	[self.window makeKeyAndVisible];
	return YES;
}

@end

/* --------------------------------------------------------------------------- the layer mode */

/*
 * No UIKit: a bare CAMetalLayer of the panel's size, a few frames, and every
 * check that does not need a window. This is the mode whose output is certainly
 * in the log, because the job list runs it.
 */
static int
rwvk_layer_mode(void)
{
	CAMetalLayer *layer;
	int failures = 0;

	rwvk_open_console();
	rwvk_metal = rwvk_make_device();
	if (rwvk_metal == nil) {
		rwvk_say("vktriangle: FAILED: no Metal device");
		return 1;
	}
	rwvk_metal_queue = [rwvk_metal newCommandQueue];
	if (rwvk_metal_queue == nil) {
		rwvk_say("vktriangle: FAILED: the device gave no command queue, so no present");
		return 1;
	}
	if (rwvk_open(&rwvk_vulkan) != 0)
		return 1;
	layer = [[CAMetalLayer alloc] init];
	layer.device = rwvk_metal;
	layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
	layer.framebufferOnly = NO;
	layer.contentsScale = 1.0;
	layer.frame = CGRectMake(0, 0, RWVK_LAYER_W, RWVK_LAYER_H);
	layer.drawableSize = CGSizeMake(RWVK_LAYER_W, RWVK_LAYER_H);
	rwvk_say("vktriangle: layer mode: a bare CAMetalLayer, %.0fx%.0f",
	    layer.drawableSize.width, layer.drawableSize.height);
	for (unsigned i = 0; i < RWVK_LAYER_FRAMES; i++)
		failures += rwvk_frame(&rwvk_vulkan, layer, 1) != 0;

	rwvk_say("vktriangle: layer mode: %u frame(s), %u failure(s): %s",
	    rwvk_vulkan.frames, rwvk_vulkan.failures,
	    rwvk_vulkan.failures == 0 && rwvk_vulkan.frames == RWVK_LAYER_FRAMES
	        ? "PASS" : "FAIL");
	rwvk_close(&rwvk_vulkan);
	return failures != 0 || rwvk_vulkan.failures != 0;
}

int
main(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--layer") == 0) {
			int status = rwvk_layer_mode();
			rwvk_say("vktriangle: LAYER MODE: %s", status == 0 ? "PASS" : "FAIL");
			return status;
		}
	}
	rwvk_open_console();
	return UIApplicationMain(argc, argv, nil, NSStringFromClass([RWVKDelegate class]));
}
