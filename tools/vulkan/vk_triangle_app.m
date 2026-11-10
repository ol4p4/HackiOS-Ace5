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
#include <mach/mach_time.h>

#include <vulkan/vulkan.h>

#include "triangle_spirv.h"
#include "composite_spirv.h"
#include "wsi_redwood.h"

/* The ICD this port builds, and the two Metal constants triangle_app uses (they
 * are not exported, so they are reached by their distance from one that is). */
#define RWVK_ICD_PATH "/redwood/lib/libvulkan_freedreno.dylib"
#define RWVK_BUNDLE "/redwood/lib/RedwoodMetal.bundle"
#define RWVK_MTL_CREATE_DEVICE_UNSLID 0x181a34b3cUL
#define RWVK_MTL_ADD_DEVICE_UNSLID    0x181a1875cUL

/* The ICD's own door (vulkan/vk_icd.h has it, not vulkan.h). */
typedef PFN_vkVoidFunction (*icd_gipa_t)(VkInstance instance, const char *name);

enum { RWVK_LAYER_W = 1125, RWVK_LAYER_H = 2436, RWVK_ENTRIES = 8 };

/* Which road the composite's source takes. 1: sample the imported LINEAR image
 * directly -- the zero-copy question this boot answers. 0: copy it once into a
 * tiled image (step 15's working road). */
#define RWVK_SAMPLE_LINEAR 1
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

static int rwvk_steps;	/* --steps: the entry trace. Off by default: the extra
			 * writes change the timing of the import/present path, and
			 * the frame-4 fault did not reproduce with them on. */

static void
rwvk_step(const char *what)
{
	if (!rwvk_steps)
		return;
	rwvk_say("vktriangle: step: %s", what);
}

/* The XNU here fails corpse capture, so a SIGSEGV in the driver dies in silence
 * (the first composite boot: "exited due to SIGSEGV" and nothing else). Print
 * the fault address and the registers against the ICD's load base, as
 * tools/vulkan/turnip_test.c does. */
static void *rwvk_fault_base;

static void
rwvk_fault_handler(int sig, siginfo_t *info, void *uctx)
{
	unsigned long long *regs = (unsigned long long *)uctx;

	rwvk_say("vktriangle: FAULT sig=%d addr=%p libbase=%p", sig, info->si_addr,
	    rwvk_fault_base);
	for (int i = 0; i < 34; i++)
		rwvk_say("vktriangle:   uctx[%02d] = %#018llx%s", i, regs[i],
		    (unsigned long long)regs[i] > (unsigned long long)rwvk_fault_base &&
		    (unsigned long long)regs[i] < (unsigned long long)rwvk_fault_base + 0x1000000
		        ? "  <-- in the ICD" : "");
	_exit(99);
}

static void
rwvk_install_fault_handler(void *library)
{
	struct sigaction action = { .sa_sigaction = rwvk_fault_handler,
	    .sa_flags = SA_SIGINFO };
	Dl_info info;

	sigaction(SIGSEGV, &action, NULL);
	sigaction(SIGBUS, &action, NULL);
	if (library != NULL && dladdr(dlsym(library, "vk_icdGetInstanceProcAddr"), &info) &&
	    info.dli_fbase != NULL)
		rwvk_fault_base = info.dli_fbase;
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

/* The drawable half -- nextDrawable, the surface by id, the seed bump, the
 * present -- lives in wsi_redwood.m; this program is its first client. */

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

	/* The source half of the composite (step 15): an IOSurface this program
	 * fills, imported as a linear VkImage, copied once into a tiled image, and
	 * sampled from there. The import is the bridge's read side: the pixels
	 * come from surface memory, not from a buffer this program owns. */
	IOSurfaceRef source_surface;
	VkImage source_linear;
	VkDeviceMemory source_linear_memory;
	VkImage source_tiled;
	VkDeviceMemory source_tiled_memory;
	VkImageView source_view;

	/* The upper layer (step 16): a red wash whose alpha runs left to right. */
	IOSurfaceRef overlay_surface;
	VkImage overlay_image;
	VkDeviceMemory overlay_memory;
	VkImageView overlay_view;
	VkSampler sampler;
	VkDescriptorSetLayout set_layout;
	VkDescriptorPool descriptor_pool;
	VkDescriptorSet descriptor_set;
	unsigned source_width, source_height;
};

/* The process's one Vulkan context, its Metal device and the queue the present
 * rides. Declared here because rwvk_frame uses the queue: the drawable's own
 * -present is not enough on this port (the first on-panel run stayed black). */
static struct rwvk rwvk_vulkan;
static struct rw_wsi *rwvk_wsi;
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
static PFN_vkGetPhysicalDeviceFormatProperties pGetPhysicalDeviceFormatProperties;
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
static PFN_vkCmdBindDescriptorSets pCmdBindDescriptorSets;
static PFN_vkCmdPipelineBarrier pCmdPipelineBarrier;
static PFN_vkCmdCopyImage pCmdCopyImage;
static PFN_vkCreateSampler pCreateSampler;
static PFN_vkDestroySampler pDestroySampler;
static PFN_vkCreateDescriptorSetLayout pCreateDescriptorSetLayout;
static PFN_vkDestroyDescriptorSetLayout pDestroyDescriptorSetLayout;
static PFN_vkCreateDescriptorPool pCreateDescriptorPool;
static PFN_vkDestroyDescriptorPool pDestroyDescriptorPool;
static PFN_vkAllocateDescriptorSets pAllocateDescriptorSets;
static PFN_vkUpdateDescriptorSets pUpdateDescriptorSets;

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

	rwvk_install_fault_handler(library);
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
	RWVK_GIPA(v->instance, "vkGetPhysicalDeviceFormatProperties", pGetPhysicalDeviceFormatProperties);
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
	RWVK_GDPA(v->device, "vkCmdBindDescriptorSets", pCmdBindDescriptorSets);
	RWVK_GDPA(v->device, "vkCmdPipelineBarrier", pCmdPipelineBarrier);
	RWVK_GDPA(v->device, "vkCmdCopyImage", pCmdCopyImage);
	RWVK_GDPA(v->device, "vkCreateSampler", pCreateSampler);
	RWVK_GDPA(v->device, "vkDestroySampler", pDestroySampler);
	RWVK_GDPA(v->device, "vkCreateDescriptorSetLayout", pCreateDescriptorSetLayout);
	RWVK_GDPA(v->device, "vkDestroyDescriptorSetLayout", pDestroyDescriptorSetLayout);
	RWVK_GDPA(v->device, "vkCreateDescriptorPool", pCreateDescriptorPool);
	RWVK_GDPA(v->device, "vkDestroyDescriptorPool", pDestroyDescriptorPool);
	RWVK_GDPA(v->device, "vkAllocateDescriptorSets", pAllocateDescriptorSets);
	RWVK_GDPA(v->device, "vkUpdateDescriptorSets", pUpdateDescriptorSets);

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
	    .codeSize = sizeof(composite_vert_spirv),
	    .pCode = composite_vert_spirv,
	};
	VkShaderModuleCreateInfo fragmentInfo = {
	    .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
	    .codeSize = sizeof(composite2_frag_spirv),
	    .pCode = composite2_frag_spirv,
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
	/* The single descriptor the composite samples through: a combined image
	 * sampler in set 0, binding 0, read by the fragment stage. */
	VkSamplerCreateInfo samplerInfo = {
	    .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
	    .magFilter = VK_FILTER_NEAREST,
	    .minFilter = VK_FILTER_NEAREST,
	    .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
	    .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	    .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	    .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	    .maxLod = 0.0f,
	};
	if (pCreateSampler(v->device, &samplerInfo, NULL, &v->sampler) != VK_SUCCESS) {
		rwvk_say("vktriangle: FAILED: vkCreateSampler");
		return -1;
	}
	VkDescriptorSetLayoutBinding sourceBindings[2] = {
	    { .binding = 0,
	      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	      .descriptorCount = 1,
	      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
	    { .binding = 1,
	      .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	      .descriptorCount = 1,
	      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
	};
	VkDescriptorSetLayoutCreateInfo setInfo = {
	    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
	    .bindingCount = 2,
	    .pBindings = sourceBindings,
	};
	if (pCreateDescriptorSetLayout(v->device, &setInfo, NULL, &v->set_layout) != VK_SUCCESS) {
		rwvk_say("vktriangle: FAILED: vkCreateDescriptorSetLayout");
		return -1;
	}
	VkPipelineLayoutCreateInfo layoutInfo = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
	    .setLayoutCount = 1,
	    .pSetLayouts = &v->set_layout,
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
	rwvk_say("vktriangle: Vulkan up: %s, our B8G8R8A8 pass, the sampled-source "
	    "composite pipeline", "Turnip");
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
	rwvk_step("entry: vkCreateImage over the drawable");
	entry->sid = sid;
	entry->width = width;
	entry->height = height;
	if (pCreateImage(v->device, &imageInfo, NULL, &entry->image) != VK_SUCCESS) {
		rwvk_say("vktriangle: FAILED: vkCreateImage over surface %u", sid);
		return NULL;
	}
	rwvk_step("entry: memory requirements");
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
	rwvk_step("entry: import and allocate the surface's memory");
	if (pAllocateMemory(v->device, &allocate, NULL, &entry->memory) != VK_SUCCESS) {
		rwvk_say("vktriangle: FAILED: vkAllocateMemory importing surface %u", sid);
		return NULL;
	}
	if (pBindImageMemory(v->device, entry->image, entry->memory, 0) != VK_SUCCESS) {
		rwvk_say("vktriangle: FAILED: vkBindImageMemory for surface %u", sid);
		return NULL;
	}
	rwvk_step("entry: view and framebuffer");
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

/* ------------------------------------------------- the source half of the composite */

/*
 * A surface this program fills with vertical blue/yellow stripes, imported by
 * its id and sampled where it lies. This is the bridge's read side: the pixels
 * come out of IOSurface memory through the same import Turnip uses for a
 * drawable, and the two colours (blue 0,0,255 and yellow 255,255,0, opposite in
 * every channel) make a channel swap or a stride mistake countable instead of
 * plausible. Stripes, not a checker: the blend check reads them back against an
 * expected value, and a pattern that changes with x alone cannot be wrong about
 * which end of the surface the sampler calls v = 0.
 *
 * The copy is deliberate for this step: whether a LINEAR image can be sampled
 * directly is the next question, and a copy into the driver's own tiling keeps
 * this step's failure about the import and the sampler, not about tiling.
 */
static int
rwvk_make_source(struct rwvk *v)
{
	const uint32_t size = 256, cell = 32;
	CFMutableDictionaryRef props;
	CFNumberRef number;
	int value;
	IOSurfaceRef surface;
	VkImageCreateInfo linearInfo, tiledInfo;
	VkExternalMemoryImageCreateInfo external = {
	    .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
	    .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
	};
	VkMemoryRequirements requirements;
	uint32_t type;
	VkImportMemoryFdInfoKHR import = {
	    .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
	    .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
	};
	VkMemoryAllocateInfo allocate = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	VkImageMemoryBarrier barriers[2] = {
	    { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	      .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
	      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	      .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } },
	    { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	      .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	      .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	      .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	      .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } },
	};
	VkImageMemoryBarrier ready = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
	    .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
	};
	VkImageCopy copy = {
	    .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
	    .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
	    .extent = { size, size, 1 },
	};
	VkSubmitInfo submit = {
	    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
	    .commandBufferCount = 1,
	    .pCommandBuffers = &v->commands,
	};
	VkImageViewCreateInfo viewInfo = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	VkDescriptorPoolSize poolSize = {
	    .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	    .descriptorCount = 2,
	};
	VkDescriptorPoolCreateInfo poolInfo = {
	    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
	    .maxSets = 1,
	    .poolSizeCount = 1,
	    .pPoolSizes = &poolSize,
	};
	VkDescriptorSetAllocateInfo setAllocate = {
	    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
	    .descriptorSetCount = 1,
	    .pSetLayouts = &v->set_layout,
	};
	VkDescriptorImageInfo imageInfo = {
	    .sampler = v->sampler,
	    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};
	VkWriteDescriptorSet write = {
	    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	    .descriptorCount = 1,
	    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	    .pImageInfo = &imageInfo,
	};

	rwvk_step("source: create the checker surface");
	{
		VkFormatProperties format;
		pGetPhysicalDeviceFormatProperties(v->physical, VK_FORMAT_B8G8R8A8_UNORM, &format);
		rwvk_say("vktriangle: source: B8G8R8A8 linear tiling %#x (sampled %s), "
		    "optimal tiling %#x (sampled %s)", format.linearTilingFeatures,
		    (format.linearTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)
		        ? "yes" : "NO",
		    format.optimalTilingFeatures,
		    (format.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)
		        ? "yes" : "NO");
	}
	/* The surface, filled with the checker and announced (the seed rule). */
	props = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	value = (int)size;
	number = CFNumberCreate(NULL, kCFNumberIntType, &value);
	CFDictionarySetValue(props, kIOSurfaceWidth, number); CFRelease(number);
	CFDictionarySetValue(props, kIOSurfaceHeight, number); CFRelease(number);
	value = 4;
	number = CFNumberCreate(NULL, kCFNumberIntType, &value);
	CFDictionarySetValue(props, kIOSurfaceBytesPerElement, number); CFRelease(number);
	value = (int)size * 4;
	number = CFNumberCreate(NULL, kCFNumberIntType, &value);
	CFDictionarySetValue(props, kIOSurfaceBytesPerRow, number); CFRelease(number);
	value = 0x42475241;                     /* 'BGRA' */
	number = CFNumberCreate(NULL, kCFNumberIntType, &value);
	CFDictionarySetValue(props, kIOSurfacePixelFormat, number); CFRelease(number);
	surface = IOSurfaceCreate(props);
	CFRelease(props);
	if (surface == NULL) {
		rwvk_say("vktriangle: FAILED: no source IOSurface");
		return -1;
	}
	if (IOSurfaceLock(surface, 0, NULL) != 0) {
		rwvk_say("vktriangle: FAILED: the source surface would not lock for writing");
		return -1;
	}
	{
		uint8_t *base = IOSurfaceGetBaseAddress(surface);
		size_t stride = IOSurfaceGetBytesPerRow(surface);

		for (uint32_t y = 0; y < size; y++) {
			for (uint32_t x = 0; x < size; x++) {
				uint8_t *p = base + (size_t)y * stride + (size_t)x * 4;
				/* vertical stripes, not a checker: the blend check must
				 * not depend on which end of the surface the sampler calls
				 * v = 0, and a colour that changes with x alone cannot. */
				if ((x / cell) & 1) {
					p[0] = 0; p[1] = 255; p[2] = 255;   /* yellow, BGRA */
				} else {
					p[0] = 255; p[1] = 0; p[2] = 0;      /* blue, BGRA */
				}
				p[3] = 255;
			}
		}
	}
	IOSurfaceUnlock(surface, 0, NULL);
	v->source_surface = surface;
	v->source_width = v->source_height = size;

	rwvk_step("source: import the surface as a linear VkImage");
	/* The imported linear image. */
	linearInfo = (VkImageCreateInfo){
	    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	    .pNext = &external,
	    .imageType = VK_IMAGE_TYPE_2D,
	    .format = VK_FORMAT_B8G8R8A8_UNORM,
	    .extent = { size, size, 1 },
	    .mipLevels = 1,
	    .arrayLayers = 1,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .tiling = VK_IMAGE_TILING_LINEAR,
	    .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
	    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	if (pCreateImage(v->device, &linearInfo, NULL, &v->source_linear) != VK_SUCCESS) {
		rwvk_say("vktriangle: FAILED: vkCreateImage(source, linear)");
		return -1;
	}
	pGetImageMemoryRequirements(v->device, v->source_linear, &requirements);
	type = UINT32_MAX;
	for (uint32_t i = 0; i < v->memory.memoryTypeCount; i++) {
		if ((requirements.memoryTypeBits & (1u << i)) &&
		    !(v->memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
			type = i;
			break;
		}
	}
	for (uint32_t i = 0; i < v->memory.memoryTypeCount && type == UINT32_MAX; i++) {
		if (requirements.memoryTypeBits & (1u << i))
			type = i;
	}
	if (type == UINT32_MAX) {
		rwvk_say("vktriangle: FAILED: no memory type for the source import");
		return -1;
	}
	import.fd = (int)IOSurfaceGetID(surface);
	allocate.pNext = &import;
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = type;
	if (pAllocateMemory(v->device, &allocate, NULL, &v->source_linear_memory) != VK_SUCCESS ||
	    pBindImageMemory(v->device, v->source_linear, v->source_linear_memory, 0) != VK_SUCCESS) {
		rwvk_say("vktriangle: FAILED: the source import did not bind (id %u)",
		    (unsigned)IOSurfaceGetID(surface));
		return -1;
	}

	rwvk_step("source: create the tiled image");
#if !RWVK_SAMPLE_LINEAR
	/* The tiled image the sampler reads. */
	tiledInfo = linearInfo;
	tiledInfo.pNext = NULL;
	tiledInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	tiledInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	if (pCreateImage(v->device, &tiledInfo, NULL, &v->source_tiled) != VK_SUCCESS) {
		rwvk_say("vktriangle: FAILED: vkCreateImage(source, tiled)");
		return -1;
	}
	pGetImageMemoryRequirements(v->device, v->source_tiled, &requirements);
	type = UINT32_MAX;
	for (uint32_t i = 0; i < v->memory.memoryTypeCount; i++) {
		if ((requirements.memoryTypeBits & (1u << i)) &&
		    !(v->memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
			type = i;
			break;
		}
	}
	for (uint32_t i = 0; i < v->memory.memoryTypeCount && type == UINT32_MAX; i++) {
		if (requirements.memoryTypeBits & (1u << i))
			type = i;
	}
	if (type == UINT32_MAX) {
		rwvk_say("vktriangle: FAILED: no memory type for the tiled source");
		return -1;
	}
	allocate.pNext = NULL;
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = type;
	if (pAllocateMemory(v->device, &allocate, NULL, &v->source_tiled_memory) != VK_SUCCESS ||
	    pBindImageMemory(v->device, v->source_tiled, v->source_tiled_memory, 0) != VK_SUCCESS) {
		rwvk_say("vktriangle: FAILED: the tiled source did not bind");
		return -1;
	}

	rwvk_step("source: copy import -> tiled, then to SHADER_READ_ONLY");
	/* One command: import -> tiled, then the tiled image ready to sample. */
	barriers[0].image = v->source_linear;
	barriers[1].image = v->source_tiled;
	ready.image = v->source_tiled;
	if (pResetCommandBuffer(v->commands, 0) != VK_SUCCESS ||
	    pBeginCommandBuffer(v->commands, &(VkCommandBufferBeginInfo){
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }) != VK_SUCCESS)
		return -1;
	pCmdPipelineBarrier(v->commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
	    VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, barriers);
	pCmdCopyImage(v->commands, v->source_linear, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	    v->source_tiled, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
	pCmdPipelineBarrier(v->commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
	    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &ready);
	if (pEndCommandBuffer(v->commands) != VK_SUCCESS)
		return -1;
	if (pQueueSubmit(v->queue, 1, &submit, v->fence) != VK_SUCCESS ||
	    pWaitForFences(v->device, 1, &v->fence, VK_TRUE, 3000000000ull) != VK_SUCCESS) {
		rwvk_say("vktriangle: FAILED: the source copy never completed");
		return -1;
	}
	pResetFences(v->device, 1, &v->fence);

	rwvk_step("source: view, descriptor set, update");
#else
	/* The zero-copy road: the imported linear image straight to sampled. No
	 * tiled copy, so what the sampler reads IS the surface's memory. */
	rwvk_step("source: linear image straight to SHADER_READ_ONLY (no copy)");
	{
		VkImageMemoryBarrier straight = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		    .srcAccessMask = 0,
		    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
		    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		    .image = v->source_linear,
		    .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
		};

		if (pResetCommandBuffer(v->commands, 0) != VK_SUCCESS ||
		    pBeginCommandBuffer(v->commands, &(VkCommandBufferBeginInfo){
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }) != VK_SUCCESS)
			return -1;
		pCmdPipelineBarrier(v->commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1,
		    &straight);
		if (pEndCommandBuffer(v->commands) != VK_SUCCESS)
			return -1;
		if (pQueueSubmit(v->queue, 1, &submit, v->fence) != VK_SUCCESS ||
		    pWaitForFences(v->device, 1, &v->fence, VK_TRUE, 3000000000ull) != VK_SUCCESS) {
			rwvk_say("vktriangle: FAILED: the linear image never became sampled");
			return -1;
		}
		pResetFences(v->device, 1, &v->fence);
	}
#endif
	viewInfo.image = RWVK_SAMPLE_LINEAR ? v->source_linear : v->source_tiled;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
	viewInfo.subresourceRange = (VkImageSubresourceRange){
	    VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	if (pCreateImageView(v->device, &viewInfo, NULL, &v->source_view) != VK_SUCCESS) {
		rwvk_say("vktriangle: FAILED: vkCreateImageView(source)");
		return -1;
	}
	if (pCreateDescriptorPool(v->device, &poolInfo, NULL, &v->descriptor_pool) != VK_SUCCESS) {
		rwvk_say("vktriangle: FAILED: vkCreateDescriptorPool");
		return -1;
	}
	/* The pool is named HERE and not in the initializer at the top: the pool
	 * does not exist until this line, and the first device run of the
	 * composite crashed in vkAllocateDescriptorSets on exactly that -- a NULL
	 * descriptorPool (fault addr 0x58). */
	setAllocate.descriptorPool = v->descriptor_pool;
	if (pAllocateDescriptorSets(v->device, &setAllocate, &v->descriptor_set) != VK_SUCCESS) {
		rwvk_say("vktriangle: FAILED: vkAllocateDescriptorSets");
		return -1;
	}
	imageInfo.imageView = v->source_view;
	write.dstSet = v->descriptor_set;
	write.dstBinding = 0;
	pUpdateDescriptorSets(v->device, 1, &write, 0, NULL);
	rwvk_say("vktriangle: source: surface id %u, %ux%u %#x, imported linear, %s, "
	    "sampled through a nearest sampler (vertical stripes)",
	    (unsigned)IOSurfaceGetID(surface), size, size,
	    (unsigned)IOSurfaceGetPixelFormat(surface),
	    RWVK_SAMPLE_LINEAR ? "sampled where it lies (no copy)"
	                       : "copied once into a tiled image");
	return 0;
}


/*
 * The upper layer of the composite: a red wash whose alpha runs 0 at the left
 * edge to 255 at the right. Imported the same zero-copy way as the under layer
 * and bound to set 0, binding 1; the blend itself is the fragment shader's.
 */
static int
rwvk_make_overlay(struct rwvk *v)
{
	const uint32_t size = 256;
	CFMutableDictionaryRef props;
	CFNumberRef number;
	int value;
	IOSurfaceRef surface;
	VkExternalMemoryImageCreateInfo external = {
	    .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
	    .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
	};
	VkImageCreateInfo info = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	    .pNext = &external,
	    .imageType = VK_IMAGE_TYPE_2D,
	    .format = VK_FORMAT_B8G8R8A8_UNORM,
	    .extent = { size, size, 1 },
	    .mipLevels = 1,
	    .arrayLayers = 1,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .tiling = VK_IMAGE_TILING_LINEAR,
	    .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
	    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	VkMemoryRequirements requirements;
	uint32_t type = UINT32_MAX;
	VkImportMemoryFdInfoKHR import = {
	    .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
	    .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
	};
	VkMemoryAllocateInfo allocate = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
	VkImageMemoryBarrier ready = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
	    .srcAccessMask = 0,
	    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
	    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	    .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
	    .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
	};
	VkSubmitInfo submit = {
	    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
	    .commandBufferCount = 1,
	    .pCommandBuffers = &v->commands,
	};
	VkImageViewCreateInfo viewInfo = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
	VkDescriptorImageInfo imageInfo = {
	    .sampler = v->sampler,
	    .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};
	VkWriteDescriptorSet write = {
	    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
	    .dstSet = v->descriptor_set,
	    .dstBinding = 1,
	    .descriptorCount = 1,
	    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
	    .pImageInfo = &imageInfo,
	};

	props = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks,
	    &kCFTypeDictionaryValueCallBacks);
	value = (int)size;
	number = CFNumberCreate(NULL, kCFNumberIntType, &value);
	CFDictionarySetValue(props, kIOSurfaceWidth, number); CFRelease(number);
	CFDictionarySetValue(props, kIOSurfaceHeight, number); CFRelease(number);
	value = 4;
	number = CFNumberCreate(NULL, kCFNumberIntType, &value);
	CFDictionarySetValue(props, kIOSurfaceBytesPerElement, number); CFRelease(number);
	value = (int)size * 4;
	number = CFNumberCreate(NULL, kCFNumberIntType, &value);
	CFDictionarySetValue(props, kIOSurfaceBytesPerRow, number); CFRelease(number);
	value = 0x42475241;
	number = CFNumberCreate(NULL, kCFNumberIntType, &value);
	CFDictionarySetValue(props, kIOSurfacePixelFormat, number); CFRelease(number);
	surface = IOSurfaceCreate(props);
	CFRelease(props);
	if (surface == NULL || IOSurfaceLock(surface, 0, NULL) != 0) {
		rwvk_say("vktriangle: FAILED: no overlay surface");
		return -1;
	}
	{
		uint8_t *base = IOSurfaceGetBaseAddress(surface);
		size_t stride = IOSurfaceGetBytesPerRow(surface);

		for (uint32_t y = 0; y < size; y++) {
			for (uint32_t x = 0; x < size; x++) {
				uint8_t *p = base + (size_t)y * stride + (size_t)x * 4;

				p[0] = 0; p[1] = 0; p[2] = 255;    /* red, BGRA */
				p[3] = (uint8_t)x;                  /* alpha 0..255 left to right */
			}
		}
	}
	IOSurfaceUnlock(surface, 0, NULL);
	v->overlay_surface = surface;

	if (pCreateImage(v->device, &info, NULL, &v->overlay_image) != VK_SUCCESS) {
		rwvk_say("vktriangle: FAILED: vkCreateImage(overlay)");
		return -1;
	}
	pGetImageMemoryRequirements(v->device, v->overlay_image, &requirements);
	for (uint32_t i = 0; i < v->memory.memoryTypeCount; i++) {
		if ((requirements.memoryTypeBits & (1u << i)) &&
		    !(v->memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
			type = i;
			break;
		}
	}
	for (uint32_t i = 0; i < v->memory.memoryTypeCount && type == UINT32_MAX; i++) {
		if (requirements.memoryTypeBits & (1u << i))
			type = i;
	}
	if (type == UINT32_MAX) {
		rwvk_say("vktriangle: FAILED: no memory type for the overlay");
		return -1;
	}
	import.fd = (int)IOSurfaceGetID(surface);
	allocate.pNext = &import;
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = type;
	if (pAllocateMemory(v->device, &allocate, NULL, &v->overlay_memory) != VK_SUCCESS ||
	    pBindImageMemory(v->device, v->overlay_image, v->overlay_memory, 0) != VK_SUCCESS) {
		rwvk_say("vktriangle: FAILED: the overlay import did not bind");
		return -1;
	}
	ready.image = v->overlay_image;
	if (pResetCommandBuffer(v->commands, 0) != VK_SUCCESS ||
	    pBeginCommandBuffer(v->commands, &(VkCommandBufferBeginInfo){
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }) != VK_SUCCESS)
		return -1;
	pCmdPipelineBarrier(v->commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
	    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &ready);
	if (pEndCommandBuffer(v->commands) != VK_SUCCESS)
		return -1;
	if (pQueueSubmit(v->queue, 1, &submit, v->fence) != VK_SUCCESS ||
	    pWaitForFences(v->device, 1, &v->fence, VK_TRUE, 3000000000ull) != VK_SUCCESS) {
		rwvk_say("vktriangle: FAILED: the overlay never became sampled");
		return -1;
	}
	pResetFences(v->device, 1, &v->fence);

	viewInfo.image = v->overlay_image;
	viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
	viewInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
	viewInfo.subresourceRange = (VkImageSubresourceRange){
	    VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	if (pCreateImageView(v->device, &viewInfo, NULL, &v->overlay_view) != VK_SUCCESS) {
		rwvk_say("vktriangle: FAILED: vkCreateImageView(overlay)");
		return -1;
	}
	imageInfo.imageView = v->overlay_view;
	pUpdateDescriptorSets(v->device, 1, &write, 0, NULL);
	rwvk_say("vktriangle: overlay: surface id %u, %ux%u %#x, red with alpha 0..255 "
	    "left to right, sampled where it lies",
	    (unsigned)IOSurfaceGetID(surface), size, size,
	    (unsigned)IOSurfaceGetPixelFormat(surface));
	return 0;
}

/*
 * One frame: draw into the surface, announce the write, present it, then read it
 * back through the surface the display will read. The read is the honest check
 * (two views of the same pages), the present is what puts it on the panel.
 */
static int
rwvk_frame(struct rwvk *v, int loud)
{
	@autoreleasepool {
		struct rw_wsi_image image;
		char why[192];

		if (loud)
			rwvk_step("rw_wsi_acquire (nextDrawable + the surface by id)");
		if (rw_wsi_acquire(rwvk_wsi, &image, why, sizeof(why)) != 0) {
			v->failures++;
			rwvk_say("vktriangle: frame %u: %s", v->frames + 1, why);
			return -1;
		}
		IOSurfaceRef surface = image.surface;
		uint32_t width = image.width;
		uint32_t height = image.height;

		v->frames++;
		if (loud)
			rwvk_say("vktriangle: frame %u: surface id %u, %ux%u, stride %zu, "
			    "format %#x", v->frames, image.sid, width, height,
			    image.stride, image.pixel_format);
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
		if (loud)
			rwvk_step("frame: begin the pass and draw");
		VkViewport viewport = { 0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f };
		VkRect2D scissor = { { 0, 0 }, { width, height } };
		pCmdBeginRenderPass(v->commands, &begin, VK_SUBPASS_CONTENTS_INLINE);
		pCmdBindPipeline(v->commands, VK_PIPELINE_BIND_POINT_GRAPHICS, v->pipeline);
		pCmdBindDescriptorSets(v->commands, VK_PIPELINE_BIND_POINT_GRAPHICS, v->layout,
		    0, 1, &v->descriptor_set, 0, NULL);
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
		if (loud)
			rwvk_step("frame: submit and wait");
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

		/* The WSI half: the seed rule and the present live in wsi_redwood,
		 * not here -- they belong to the window system, not to a client. */
		if (loud)
			rwvk_step("rw_wsi_present (seed bump + presentDrawable + commit)");
		if (rw_wsi_present(rwvk_wsi, &image, why, sizeof(why)) != 0) {
			v->failures++;
			rwvk_say("vktriangle: frame %u: %s", v->frames, why);
			return -1;
		}

		/* Read back through the surface, the same pages the panel reads. The
		 * check is the blend's: for every sampled pixel the expected byte is
		 * computed from the same inputs the shader had -- the under checker at
		 * that texel, and the overlay's alpha at that column -- and compared
		 * within a byte or two of rounding. A compositor that dropped the
		 * overlay, the under layer, or the alpha is counted, not looked at. */
		if (IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL) == 0) {
			const uint8_t *bytes = IOSurfaceGetBaseAddress(surface);
			size_t stride = IOSurfaceGetBytesPerRow(surface);
			unsigned good = 0, wrong = 0, red_edge = 0, under_edge = 0;

			for (uint32_t y = 0; y < height; y += 7) {
				for (uint32_t x = 0; x < width; x += 7) {
					const uint8_t *p = bytes + (size_t)y * stride + (size_t)x * 4;
					double u = (x + 0.5) / width;
					double v = 1.0 - (y + 0.5) / height;
					int cx = (int)(u * 256);
					int cy = (int)(v * 256);
					int stripe;
					double a, under_r, under_g, under_b, out_r, out_g, out_b;

					if (cx > 255) cx = 255;
					if (cy > 255) cy = 255;
					(void)cy;
					stripe = (cx / 32) & 1;
					under_r = stripe ? 1.0 : 0.0;    /* yellow or blue */
					under_g = stripe ? 1.0 : 0.0;
					under_b = stripe ? 0.0 : 1.0;
					a = (cx > 255 ? 255 : cx) / 255.0;
					out_r = a + (1.0 - a) * under_r;
					out_g = (1.0 - a) * under_g;
					out_b = (1.0 - a) * under_b;
					if (abs((int)p[0] - (int)(out_b * 255.0 + 0.5)) <= 8 &&
					    abs((int)p[1] - (int)(out_g * 255.0 + 0.5)) <= 8 &&
					    abs((int)p[2] - (int)(out_r * 255.0 + 0.5)) <= 8)
						good++;
					else
						wrong++;
					if (a > 0.98)
						red_edge++;
					if (a < 0.02)
						under_edge++;
				}
			}
			IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
			if (loud)
				rwvk_say("vktriangle: frame %u: blend grid good %u, wrong %u "
				    "(pure-over %u, pure-under %u)", v->frames, good, wrong,
				    red_edge, under_edge);
			if (wrong * 50 > (good + wrong) || red_edge == 0 || under_edge == 0) {
				v->failures++;
				rwvk_say("vktriangle: frame %u: FAILED: the drawable does not hold "
				    "the blend (good %u, wrong %u, pure-over %u, pure-under %u)",
				    v->frames, good, wrong, red_edge, under_edge);
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
	if (v->source_view)
		pDestroyImageView(v->device, v->source_view, NULL);
	if (v->descriptor_pool)
		pDestroyDescriptorPool(v->device, v->descriptor_pool, NULL);
	if (v->set_layout)
		pDestroyDescriptorSetLayout(v->device, v->set_layout, NULL);
	if (v->sampler)
		pDestroySampler(v->device, v->sampler, NULL);
	if (v->source_tiled)
		pDestroyImage(v->device, v->source_tiled, NULL);
	if (v->source_tiled_memory)
		pFreeMemory(v->device, v->source_tiled_memory, NULL);
	if (v->source_linear)
		pDestroyImage(v->device, v->source_linear, NULL);
	if (v->source_linear_memory)
		pFreeMemory(v->device, v->source_linear_memory, NULL);
	if (v->source_surface)
		CFRelease(v->source_surface);
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

/* --------------------------------------------------------------------------- pacing */

static int
rwvk_compare_double(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return x < y ? -1 : x > y ? 1 : 0;
}

static void
rwvk_stats(const char *name, double *values, unsigned count)
{
	double sum = 0;

	if (count == 0)
		return;
	qsort(values, count, sizeof(*values), rwvk_compare_double);
	for (unsigned i = 0; i < count; i++)
		sum += values[i];
	rwvk_say("vktriangle: BENCH %s: min %.3f, median %.3f, mean %.3f, max %.3f",
	    name, values[0], values[count / 2], sum / count, values[count - 1]);
}

/*
 * Present as fast as the window system takes frames, and say how fast that is.
 * No timer: the loop is acquire -> draw -> present, back to back, so the
 * interval between one present and the next is CA's own cadence (a display
 * link throttles it; nothing here sleeps). The failure that matters is a nil
 * nextDrawable part-way through -- the pool of drawables does not come back
 * when presents are not completing -- and it is reported by frame number.
 */
static void
rwvk_bench(struct rwvk *v, const char *where, unsigned frames)
{
	mach_timebase_info_data_t timebase;
	double *frame_us = calloc(frames, sizeof(double));
	double *gap_ms = calloc(frames, sizeof(double));
	unsigned n = 0, counted = 0;
	uint64_t previous = 0;

	if (frame_us == NULL || gap_ms == NULL) {
		free(frame_us);
		free(gap_ms);
		return;
	}
	mach_timebase_info(&timebase);
	for (; n < frames; n++) {
		uint64_t start = mach_absolute_time();
		int failed = rwvk_frame(v, n < 3) != 0;
		uint64_t end = mach_absolute_time();

		if (failed)
			break;
		frame_us[counted] = (double)(end - start) * timebase.numer / timebase.denom / 1000.0;
		if (previous != 0)
			gap_ms[counted] = (double)(end - previous) * timebase.numer /
			    timebase.denom / 1000000.0;
		else
			gap_ms[counted] = 0.0;
		previous = end;
		counted++;
	}
	rwvk_say("vktriangle: BENCH %s: %u of %u frames, %u failure(s)", where, counted,
	    frames, v->failures);
	rwvk_stats("frame (draw+readback, us)", frame_us, counted);
	rwvk_stats("present interval (ms, 0 = first)", gap_ms + 1, counted > 1 ? counted - 1 : 0);
	if (counted > 1) {
		double elapsed = 0;

		for (unsigned i = 1; i < counted; i++)
			elapsed += gap_ms[i];
		rwvk_say("vktriangle: BENCH %s: %.1f fps over the window (%.1f ms/frame)",
		    where, (counted - 1) * 1000.0 / elapsed, elapsed / (counted - 1));
	}
	free(frame_us);
	free(gap_ms);
}

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
	{
		char why[192];

		if (rw_wsi_open(&rwvk_wsi, layer, rwvk_metal, rwvk_metal_queue, why,
		    sizeof(why)) != 0) {
			rwvk_say("vktriangle: FAILED: %s", why);
			return;
		}
	}
	/* The first pacing measurement: present back to back, no timer, then the
	 * half-second loop that leaves the picture on the panel. */
	rwvk_bench(&rwvk_vulkan, "app", 240);
	rwvk_timer = [NSTimer scheduledTimerWithTimeInterval:RWVK_APP_PERIOD_MS / 1000.0
	    repeats:YES block:^(NSTimer *timer) {
		int loud = rwvk_app_frames < 3 || (rwvk_app_frames % 10) == 0;

		rwvk_app_frames++;
		rwvk_frame(&rwvk_vulkan, loud);
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
	if (rwvk_make_source(&rwvk_vulkan) != 0) {
		rwvk_say("vktriangle: FAILED: no source to composite");
		return YES;
	}
	if (rwvk_make_overlay(&rwvk_vulkan) != 0) {
		rwvk_say("vktriangle: FAILED: no overlay to composite");
		return YES;
	}
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
	if (rwvk_make_source(&rwvk_vulkan) != 0) {
		rwvk_say("vktriangle: FAILED: no source to composite");
		return 1;
	}
	if (rwvk_make_overlay(&rwvk_vulkan) != 0) {
		rwvk_say("vktriangle: FAILED: no overlay to composite");
		return 1;
	}
	layer = [[CAMetalLayer alloc] init];
	layer.device = rwvk_metal;
	layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
	layer.framebufferOnly = NO;
	layer.contentsScale = 1.0;
	layer.frame = CGRectMake(0, 0, RWVK_LAYER_W, RWVK_LAYER_H);
	layer.drawableSize = CGSizeMake(RWVK_LAYER_W, RWVK_LAYER_H);
	rwvk_say("vktriangle: layer mode: a bare CAMetalLayer, %.0fx%.0f",
	    layer.drawableSize.width, layer.drawableSize.height);
	{
		char why[192];

		if (rw_wsi_open(&rwvk_wsi, layer, rwvk_metal, rwvk_metal_queue, why,
		    sizeof(why)) != 0) {
			rwvk_say("vktriangle: FAILED: %s", why);
			return 1;
		}
	}
	for (unsigned i = 0; i < RWVK_LAYER_FRAMES; i++)
		failures += rwvk_frame(&rwvk_vulkan, 1) != 0;
	/* The same loop as the app's, in a process with no window: the gap to
	 * compare against, when the app's cadence is read. */
	rwvk_bench(&rwvk_vulkan, "layer", 120);

	rwvk_say("vktriangle: layer mode: %u frame(s), %u failure(s): %s",
	    rwvk_vulkan.frames, rwvk_vulkan.failures,
	    rwvk_vulkan.failures == 0 && rwvk_vulkan.frames >= RWVK_LAYER_FRAMES
	        ? "PASS" : "FAIL");
	rw_wsi_close(rwvk_wsi);
	rwvk_wsi = NULL;
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
		if (strcmp(argv[i], "--steps") == 0)
			rwvk_steps = 1;
	}
	rwvk_open_console();
	return UIApplicationMain(argc, argv, nil, NSStringFromClass([RWVKDelegate class]));
}
