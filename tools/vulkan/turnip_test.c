/*
 * turnip_test: does Mesa's freedreno Vulkan driver (Turnip) run on this port's
 * Adreno, through the kernel's RedwoodGPU service?
 *
 * This is the gate for the "real driver" route (RUNBOOK.md, 2026-10-28): UniXNU
 * runs Metal through metal2vulkan on Turnip, and our kernel already speaks the
 * msm DRM interface Turnip speaks (tools/mesa/redwood-drm stands in for libdrm,
 * exactly as it does for the GL driver). So the question is not whether the
 * ABI matches -- it is whether Turnip's a7xx code paths accept what our A7XX
 * driver (FD750, chip_id 0x43051401 in Mesa's table) actually provides.
 *
 * The steps say where it stops, each with the VkResult that stopped it:
 *
 *   1. the ICD loads and hands out vkCreateInstance (no Vulkan loader: the
 *      library is the ICD, and vk_icdGetInstanceProcAddr is its door)
 *   2. vkCreateInstance
 *   3. physical devices, with the properties (name, driver, API version)
 *   4. vkCreateDevice and a queue
 *   5. host-visible memory: allocated, mapped, written, read back
 *   6. an image with color-attachment usage, bound to memory
 *   7. an empty command buffer submitted; the fence reaches signalled
 *   8. a triangle drawn into that image, copied back through a host-visible
 *      buffer, and the pixels compared with the draw's own colour
 *   9. the same pixels in an IOSurface, in the BGRA order the rest of the
 *      system reads: the surface half of a swapchain
 *  10. a two-image swapchain: a second render target drawn over a different
 *      clear and presented as the compositor's second IOSurface, both
 *      surfaces then verified -- the acquire/render/present rotation in
 *      miniature, with this program playing the window server
 *
 * Step 8 is the stage the gate stops short of: it answers whether Turnip's
 * pipeline and draw paths (ir3 compilation, the render pass, the CP's own draw
 * packets) run on this Adreno, not just its command submission. Step 9 answers
 * whether what Turnip drew can leave the driver's own BOs and become a surface
 * the window server, the 2D engine and CoreAnimation can take.
 */
#include <dlfcn.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurfaceRef.h>
#include <vulkan/vulkan.h>

#include "triangle_spirv.h"

#define ICD_PATH "/redwood/lib/libvulkan_freedreno.dylib"

/* The ICD's own door (vulkan/vk_icd.h has it, not vulkan.h): what a loader
 * would call first, and what this program calls instead of being a loader. */
typedef PFN_vkVoidFunction (*icd_gipa_t)(VkInstance instance, const char *name);

/* The entry points this program asks for, filled through the ICD's own
 * vk_icdGetInstanceProcAddr. Typedefs come from the SDK's vulkan headers. */
static PFN_vkGetInstanceProcAddr   getInstanceProcAddr;
static PFN_vkCreateInstance        pCreateInstance;
static PFN_vkDestroyInstance       pDestroyInstance;
static PFN_vkEnumeratePhysicalDevices pEnumeratePhysicalDevices;
static PFN_vkGetPhysicalDeviceProperties pGetPhysicalDeviceProperties;
static PFN_vkGetPhysicalDeviceQueueFamilyProperties pGetQueueFamilyProperties;
static PFN_vkCreateDevice          pCreateDevice;
static PFN_vkDestroyDevice         pDestroyDevice;
static PFN_vkGetDeviceProcAddr     pGetDeviceProcAddr;
static PFN_vkAllocateMemory        pAllocateMemory;
static PFN_vkFreeMemory            pFreeMemory;
static PFN_vkMapMemory             pMapMemory;
static PFN_vkUnmapMemory           pUnmapMemory;
static PFN_vkCreateImage           pCreateImage;
static PFN_vkDestroyImage          pDestroyImage;
static PFN_vkGetImageMemoryRequirements pGetImageMemoryRequirements;
static PFN_vkBindImageMemory       pBindImageMemory;
static PFN_vkCreateCommandPool     pCreateCommandPool;
static PFN_vkDestroyCommandPool    pDestroyCommandPool;
static PFN_vkAllocateCommandBuffers pAllocateCommandBuffers;
static PFN_vkBeginCommandBuffer    pBeginCommandBuffer;
static PFN_vkEndCommandBuffer      pEndCommandBuffer;
static PFN_vkCreateFence           pCreateFence;
static PFN_vkDestroyFence          pDestroyFence;
static PFN_vkGetFenceStatus        pGetFenceStatus;
static PFN_vkGetDeviceQueue        pGetDeviceQueue;
static PFN_vkQueueSubmit           pQueueSubmit;
static PFN_vkDeviceWaitIdle        pDeviceWaitIdle;
static PFN_vkCreateShaderModule    pCreateShaderModule;
static PFN_vkDestroyShaderModule   pDestroyShaderModule;
static PFN_vkCreateImageView       pCreateImageView;
static PFN_vkDestroyImageView      pDestroyImageView;
static PFN_vkCreateRenderPass      pCreateRenderPass;
static PFN_vkDestroyRenderPass     pDestroyRenderPass;
static PFN_vkCreateFramebuffer     pCreateFramebuffer;
static PFN_vkDestroyFramebuffer    pDestroyFramebuffer;
static PFN_vkCreatePipelineLayout  pCreatePipelineLayout;
static PFN_vkDestroyPipelineLayout pDestroyPipelineLayout;
static PFN_vkCreateGraphicsPipelines pCreateGraphicsPipelines;
static PFN_vkDestroyPipeline       pDestroyPipeline;
static PFN_vkCreateBuffer          pCreateBuffer;
static PFN_vkDestroyBuffer         pDestroyBuffer;
static PFN_vkGetBufferMemoryRequirements pGetBufferMemoryRequirements;
static PFN_vkBindBufferMemory      pBindBufferMemory;
static PFN_vkCmdBeginRenderPass    pCmdBeginRenderPass;
static PFN_vkCmdEndRenderPass      pCmdEndRenderPass;
static PFN_vkCmdBindPipeline        pCmdBindPipeline;
static PFN_vkCmdDraw               pCmdDraw;
static PFN_vkCmdCopyImageToBuffer  pCmdCopyImageToBuffer;

static const char *
result_name(VkResult r)
{
	switch (r) {
	case VK_SUCCESS: return "VK_SUCCESS";
	case VK_NOT_READY: return "VK_NOT_READY";
	case VK_TIMEOUT: return "VK_TIMEOUT";
	case VK_EVENT_SET: return "VK_EVENT_SET";
	case VK_INCOMPLETE: return "VK_INCOMPLETE";
	case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
	case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
	case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
	case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
	case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
	case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
	case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
	case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
	case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
	case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
	case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
	case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
	case VK_ERROR_OUT_OF_POOL_MEMORY: return "VK_ERROR_OUT_OF_POOL_MEMORY";
	case VK_ERROR_INVALID_EXTERNAL_HANDLE: return "VK_ERROR_INVALID_EXTERNAL_HANDLE";
	case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
	default: return "VK_ERROR_<other>";
	}
}

static int failed_at, step;

/* The XNU here fails corpse capture ("Failed to send exception
 * EXC_CORPSE_NOTIFY"), so a SIGSEGV in the driver dies in silence. Print the
 * fault address and PC ourselves: a small address is a NULL deref, and the PC
 * against the ICD's load base names the function. */
static void *
fault_library_base;

static void
fault_handler(int sig, siginfo_t *info, void *uctx)
{
	unsigned long long *regs = (unsigned long long *)uctx;

	printf("turnip_test: FAULT sig=%d addr=%p libbase=%p\n", sig, info->si_addr,
	    fault_library_base);
	for (int i = 0; i < 24; i++)
		printf("turnip_test:   uctx[%02d] = %#018llx%s\n", i, regs[i],
		    ((unsigned long long)regs[i] > (unsigned long long)fault_library_base &&
		     (unsigned long long)regs[i] < (unsigned long long)fault_library_base + 0x1000000)
		        ? "  <-- in the ICD" : "");
	fflush(stdout);
	_exit(99);
}

#define STEP(n, what) do { step = (n); printf("turnip_test: step %d: %s\n", (n), (what)); fflush(stdout); } while (0)
#define FAIL(n, what, r) do { \
	printf("turnip_test: FAILED at step %d (%s): %s\n", (n), (what), result_name(r)); \
	fflush(stdout); failed_at = (n); goto out; \
} while (0)
#define CHECK(n, what, r) do { VkResult _r = (r); if (_r != VK_SUCCESS) FAIL((n), (what), _r); } while (0)

/* A 256x256 BGRA IOSurface: the buffer a window server hands its clients and
 * reads back. Both swapchain surfaces come from here. */
static IOSurfaceRef
make_bgra_surface(void)
{
	CFMutableDictionaryRef props = CFDictionaryCreateMutable(NULL, 0,
	    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFNumberRef value;
	IOSurfaceRef created;
	int number;

	if (props == NULL)
		return NULL;
	number = 256;
	value = CFNumberCreate(NULL, kCFNumberIntType, &number);
	CFDictionarySetValue(props, kIOSurfaceWidth, value);
	CFDictionarySetValue(props, kIOSurfaceHeight, value);
	CFRelease(value);
	number = 4;
	value = CFNumberCreate(NULL, kCFNumberIntType, &number);
	CFDictionarySetValue(props, kIOSurfaceBytesPerElement, value);
	CFRelease(value);
	number = 256 * 4;
	value = CFNumberCreate(NULL, kCFNumberIntType, &number);
	CFDictionarySetValue(props, kIOSurfaceBytesPerRow, value);
	CFRelease(value);
	number = 0x42475241;                    /* 'BGRA' */
	value = CFNumberCreate(NULL, kCFNumberIntType, &number);
	CFDictionarySetValue(props, kIOSurfacePixelFormat, value);
	CFRelease(value);
	created = IOSurfaceCreate(props);
	CFRelease(props);
	return created;
}

int
main(void)
{
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice *devices = NULL;
	uint32_t count = 0, family = UINT32_MAX;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkImage image = VK_NULL_HANDLE;
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBuffer commands = VK_NULL_HANDLE;
	VkFence fence = VK_NULL_HANDLE;
	VkImageView view = VK_NULL_HANDLE;
	VkRenderPass pass = VK_NULL_HANDLE;
	VkFramebuffer framebuffer = VK_NULL_HANDLE;
	VkPipelineLayout layout = VK_NULL_HANDLE;
	VkPipeline pipeline = VK_NULL_HANDLE;
	VkShaderModule vertShader = VK_NULL_HANDLE, fragShader = VK_NULL_HANDLE;
	VkShaderModule fragShader2 = VK_NULL_HANDLE;
	VkPipeline pipeline2 = VK_NULL_HANDLE;
	VkBuffer buffer = VK_NULL_HANDLE;
	VkDeviceMemory bufferMemory = VK_NULL_HANDLE;
	IOSurfaceRef surface = NULL;
	uint8_t *rendered = NULL;
	VkImage image2 = VK_NULL_HANDLE;
	VkDeviceMemory imageMemory2 = VK_NULL_HANDLE;
	VkImageView view2 = VK_NULL_HANDLE;
	VkFramebuffer framebuffer2 = VK_NULL_HANDLE;
	VkFence fence2 = VK_NULL_HANDLE;
	IOSurfaceRef surface2 = NULL;
	uint8_t *rendered2 = NULL;
	void *library = NULL, *mapped = NULL;
	int result = 1;

	/* A device the driver skips is skipped in silence: vk_startup_errorf says
	 * why only when Mesa's debug output asks for it, and this build is
	 * b_ndebug. The gate always wants the reason. */
	setenv("TU_DEBUG", "startup", 1);
	setenv("REDWOOD_DRM_TRACE", "1", 1);

	printf("turnip_test: opening the ICD %s\n", ICD_PATH);
	library = dlopen(ICD_PATH, RTLD_NOW);
	if (library == NULL) {
		printf("turnip_test: FAILED: %s: %s\n", ICD_PATH, dlerror());
		return 1;
	}
	{
		struct sigaction sa = { .sa_sigaction = fault_handler, .sa_flags = SA_SIGINFO };
		sigaction(SIGSEGV, &sa, NULL);
		sigaction(SIGBUS, &sa, NULL);
		Dl_info di;
		void *inside = dlsym(library, "vk_icdGetInstanceProcAddr");
		if (inside != NULL && dladdr(inside, &di) && di.dli_fbase)
			fault_library_base = di.dli_fbase;
		printf("turnip_test: ICD base %p\n", fault_library_base);
		fflush(stdout);
	}

	/* No Vulkan loader on this port: the ICD's own door is what a loader would
	 * open, and this program is the loader's two lines. */
	icd_gipa_t gipa = (icd_gipa_t)dlsym(library, "vk_icdGetInstanceProcAddr");
	if (gipa == NULL) {
		printf("turnip_test: FAILED: the library has no vk_icdGetInstanceProcAddr: %s\n", dlerror());
		return 1;
	}
	pCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
	if (pCreateInstance == NULL)
		FAIL(1, "vkCreateInstance through the ICD", VK_ERROR_INITIALIZATION_FAILED);
	printf("turnip_test: step 1: the ICD answers vk_icdGetInstanceProcAddr\n");

	STEP(2, "vkCreateInstance");
	VkApplicationInfo app = {
	    .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
	    .pApplicationName = "turnip_test",
	    .applicationVersion = 1,
	    .pEngineName = "redwood",
	    .engineVersion = 1,
	    .apiVersion = VK_API_VERSION_1_0,
	};
	VkInstanceCreateInfo instanceInfo = {
	    .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
	    .pApplicationInfo = &app,
	    .enabledExtensionCount = 0,
	    .ppEnabledExtensionNames = NULL,
	};
	CHECK(2, "vkCreateInstance", pCreateInstance(&instanceInfo, NULL, &instance));

	/* From here on, everything is asked of the instance the usual way. */
	getInstanceProcAddr = (PFN_vkGetInstanceProcAddr)gipa(instance, "vkGetInstanceProcAddr");
	if (getInstanceProcAddr == NULL)
		FAIL(2, "vkGetInstanceProcAddr", VK_ERROR_INITIALIZATION_FAILED);
#define GIPA(name, var) do { \
	(var) = (__typeof__(var))getInstanceProcAddr(instance, name); \
	if ((var) == NULL) { printf("turnip_test: FAILED: no %s\n", name); failed_at = step; goto out; } \
} while (0)
	GIPA("vkDestroyInstance", pDestroyInstance);
	GIPA("vkEnumeratePhysicalDevices", pEnumeratePhysicalDevices);
	GIPA("vkGetPhysicalDeviceProperties", pGetPhysicalDeviceProperties);
	GIPA("vkGetPhysicalDeviceQueueFamilyProperties", pGetQueueFamilyProperties);
	GIPA("vkCreateDevice", pCreateDevice);

	STEP(3, "physical devices");
	CHECK(3, "vkEnumeratePhysicalDevices(count)", pEnumeratePhysicalDevices(instance, &count, NULL));
	if (count == 0) {
		printf("turnip_test: FAILED at step 3: the ICD has no physical device\n");
		failed_at = 3;
		goto out;
	}
	devices = calloc(count, sizeof(*devices));
	if (devices == NULL)
		FAIL(3, "calloc", VK_ERROR_OUT_OF_HOST_MEMORY);
	CHECK(3, "vkEnumeratePhysicalDevices(list)", pEnumeratePhysicalDevices(instance, &count, devices));
	for (uint32_t i = 0; i < count; i++) {
		VkPhysicalDeviceProperties p;

		pGetPhysicalDeviceProperties(devices[i], &p);
		/* VkPhysicalDeviceProperties has no driverID in newer headers (the
		 * driver's identity moved to VK_KHR_driver_properties): the version. */
		printf("turnip_test: device %u: \"%s\", type %d, vendor %#x, driver version %u.%u.%u, API %u.%u.%u\n",
		    i, p.deviceName, p.deviceType, p.vendorID,
		    VK_API_VERSION_MAJOR(p.driverVersion), VK_API_VERSION_MINOR(p.driverVersion),
		    VK_API_VERSION_PATCH(p.driverVersion), VK_API_VERSION_MAJOR(p.apiVersion),
		    VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion));
	}
	if (count == 0)
		FAIL(3, "no device after enumeration", VK_ERROR_INITIALIZATION_FAILED);

	/* The first queue family with graphics, which on Turnip is family 0 on
	 * every generation so far, but ask rather than assume. */
	uint32_t families = 0;
	pGetQueueFamilyProperties(devices[0], &families, NULL);
	VkQueueFamilyProperties *queues = calloc(families ? families : 1, sizeof(*queues));
	if (queues == NULL)
		FAIL(3, "calloc(queue families)", VK_ERROR_OUT_OF_HOST_MEMORY);
	pGetQueueFamilyProperties(devices[0], &families, queues);
	for (uint32_t i = 0; i < families; i++) {
		printf("turnip_test: queue family %u: %u queue(s), flags %#x\n", i, queues[i].queueCount,
		    queues[i].queueFlags);
		if ((queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && queues[i].queueCount > 0) {
			family = i;
			break;
		}
	}
	free(queues);
	if (family == UINT32_MAX) {
		printf("turnip_test: FAILED at step 3: no graphics queue family\n");
		failed_at = 3;
		goto out;
	}

	STEP(4, "vkCreateDevice and a queue");
	float priority = 1.0f;
	VkDeviceQueueCreateInfo queueInfo = {
	    .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
	    .queueFamilyIndex = family,
	    .queueCount = 1,
	    .pQueuePriorities = &priority,
	};
	VkDeviceCreateInfo deviceInfo = {
	    .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
	    .queueCreateInfoCount = 1,
	    .pQueueCreateInfos = &queueInfo,
	};
	CHECK(4, "vkCreateDevice", pCreateDevice(devices[0], &deviceInfo, NULL, &device));
	pGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)getInstanceProcAddr(instance, "vkGetDeviceProcAddr");
	if (pGetDeviceProcAddr == NULL)
		FAIL(4, "vkGetDeviceProcAddr", VK_ERROR_INITIALIZATION_FAILED);
#define GDPA(name, var) do { \
	(var) = (__typeof__(var))pGetDeviceProcAddr(device, name); \
	if ((var) == NULL) { printf("turnip_test: FAILED: no %s\n", name); failed_at = step; goto out; } \
} while (0)
	GDPA("vkDestroyDevice", pDestroyDevice);
	GDPA("vkGetDeviceQueue", pGetDeviceQueue);
	GDPA("vkAllocateMemory", pAllocateMemory);
	GDPA("vkFreeMemory", pFreeMemory);
	GDPA("vkMapMemory", pMapMemory);
	GDPA("vkUnmapMemory", pUnmapMemory);
	GDPA("vkCreateImage", pCreateImage);
	GDPA("vkDestroyImage", pDestroyImage);
	GDPA("vkGetImageMemoryRequirements", pGetImageMemoryRequirements);
	GDPA("vkBindImageMemory", pBindImageMemory);
	GDPA("vkCreateCommandPool", pCreateCommandPool);
	GDPA("vkDestroyCommandPool", pDestroyCommandPool);
	GDPA("vkAllocateCommandBuffers", pAllocateCommandBuffers);
	GDPA("vkBeginCommandBuffer", pBeginCommandBuffer);
	GDPA("vkEndCommandBuffer", pEndCommandBuffer);
	GDPA("vkCreateFence", pCreateFence);
	GDPA("vkDestroyFence", pDestroyFence);
	GDPA("vkGetFenceStatus", pGetFenceStatus);
	GDPA("vkQueueSubmit", pQueueSubmit);
	GDPA("vkDeviceWaitIdle", pDeviceWaitIdle);
	GDPA("vkCreateShaderModule", pCreateShaderModule);
	GDPA("vkDestroyShaderModule", pDestroyShaderModule);
	GDPA("vkCreateImageView", pCreateImageView);
	GDPA("vkDestroyImageView", pDestroyImageView);
	GDPA("vkCreateRenderPass", pCreateRenderPass);
	GDPA("vkDestroyRenderPass", pDestroyRenderPass);
	GDPA("vkCreateFramebuffer", pCreateFramebuffer);
	GDPA("vkDestroyFramebuffer", pDestroyFramebuffer);
	GDPA("vkCreatePipelineLayout", pCreatePipelineLayout);
	GDPA("vkDestroyPipelineLayout", pDestroyPipelineLayout);
	GDPA("vkCreateGraphicsPipelines", pCreateGraphicsPipelines);
	GDPA("vkDestroyPipeline", pDestroyPipeline);
	GDPA("vkCreateBuffer", pCreateBuffer);
	GDPA("vkDestroyBuffer", pDestroyBuffer);
	GDPA("vkGetBufferMemoryRequirements", pGetBufferMemoryRequirements);
	GDPA("vkBindBufferMemory", pBindBufferMemory);
	GDPA("vkCmdBeginRenderPass", pCmdBeginRenderPass);
	GDPA("vkCmdEndRenderPass", pCmdEndRenderPass);
	GDPA("vkCmdBindPipeline", pCmdBindPipeline);
	GDPA("vkCmdDraw", pCmdDraw);
	GDPA("vkCmdCopyImageToBuffer", pCmdCopyImageToBuffer);
	pGetDeviceQueue(device, family, 0, &queue);

	STEP(5, "host-visible memory: allocate, map, write, read back");
	/* Not every generation offers a HOST_VISIBLE heap through Turnip; take the
	 * first type that is both host-visible and coherent, and say which. */
	VkPhysicalDeviceMemoryProperties memoryProperties;
	PFN_vkGetPhysicalDeviceMemoryProperties getMemoryProperties =
	    (PFN_vkGetPhysicalDeviceMemoryProperties)getInstanceProcAddr(instance,
	        "vkGetPhysicalDeviceMemoryProperties");
	if (getMemoryProperties == NULL)
		FAIL(5, "vkGetPhysicalDeviceMemoryProperties", VK_ERROR_INITIALIZATION_FAILED);
	getMemoryProperties(devices[0], &memoryProperties);
	uint32_t memoryType = UINT32_MAX;
	for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; i++) {
		VkMemoryPropertyFlags f = memoryProperties.memoryTypes[i].propertyFlags;

		printf("turnip_test: memory type %u: heap %u, flags %#x (host visible %s, coherent %s)\n", i,
		    memoryProperties.memoryTypes[i].heapIndex, f, (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? "yes" : "no",
		    (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ? "yes" : "no");
		if (memoryType == UINT32_MAX && (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
			memoryType = i;
	}
	if (memoryType == UINT32_MAX) {
		printf("turnip_test: FAILED at step 5: no host-visible memory type\n");
		failed_at = 5;
		goto out;
	}
	VkMemoryAllocateInfo allocate = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .allocationSize = 4096,
	    .memoryTypeIndex = memoryType,
	};
	printf("turnip_test:   allocating 4096 bytes of type %u\n", memoryType);
	fflush(stdout);
	CHECK(5, "vkAllocateMemory(4096, host visible)", pAllocateMemory(device, &allocate, NULL, &memory));
	CHECK(5, "vkMapMemory", pMapMemory(device, memory, 0, 4096, 0, &mapped));
	printf("turnip_test:   mapped at %p\n", mapped);
	fflush(stdout);
	((uint32_t *)mapped)[0] = 0x50495255u;      /* "URIP" */
	((uint32_t *)mapped)[1] = 0x4e495255u;      /* "URIN" */
	printf("turnip_test:   wrote %08x %08x, reading back before unmap: %08x %08x\n",
	    ((uint32_t *)mapped)[0], ((uint32_t *)mapped)[1],
	    ((uint32_t *)mapped)[0], ((uint32_t *)mapped)[1]);
	fflush(stdout);
	printf("turnip_test:   unmapping\n");
	fflush(stdout);
	pUnmapMemory(device, memory);
	printf("turnip_test:   unmapped, mapping again\n");
	fflush(stdout);
	CHECK(5, "vkMapMemory(again)", pMapMemory(device, memory, 0, 4096, 0, &mapped));
	printf("turnip_test:   mapped again at %p\n", mapped);
	fflush(stdout);
	if (((uint32_t *)mapped)[0] != 0x50495255u || ((uint32_t *)mapped)[1] != 0x4e495255u) {
		printf("turnip_test: FAILED at step 5: the memory did not keep what was written (%#x %#x)\n",
		    ((uint32_t *)mapped)[0], ((uint32_t *)mapped)[1]);
		failed_at = 5;
		goto out;
	}
	printf("turnip_test: step 5: 4096 bytes written and read back the same\n");

	STEP(6, "an image bound to memory");
	VkImageCreateInfo imageInfo = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
	    .imageType = VK_IMAGE_TYPE_2D,
	    .format = VK_FORMAT_R8G8B8A8_UNORM,
	    .extent = { 256, 256, 1 },
	    .mipLevels = 1,
	    .arrayLayers = 1,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .tiling = VK_IMAGE_TILING_OPTIMAL,
	    .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
	             VK_IMAGE_USAGE_TRANSFER_DST_BIT,
	    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	CHECK(6, "vkCreateImage(256x256 RGBA8, color attachment)", pCreateImage(device, &imageInfo, NULL, &image));
	VkMemoryRequirements requirements;
	pGetImageMemoryRequirements(device, image, &requirements);
	printf("turnip_test: image needs %llu bytes, alignment %llu, memory type bits %#x\n",
	    (unsigned long long)requirements.size, (unsigned long long)requirements.alignment,
	    requirements.memoryTypeBits);
	uint32_t imageType = UINT32_MAX;
	for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; i++) {
		if ((requirements.memoryTypeBits & (1u << i)) &&
		    !(memoryProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
			imageType = i;
			break;
		}
	}
	if (imageType == UINT32_MAX) {
		for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; i++) {
			if (requirements.memoryTypeBits & (1u << i)) {
				imageType = i;
				break;
			}
		}
	}
	if (imageType == UINT32_MAX) {
		printf("turnip_test: FAILED at step 6: no memory type for the image\n");
		failed_at = 6;
		goto out;
	}
	VkMemoryAllocateInfo imageAllocate = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .allocationSize = requirements.size,
	    .memoryTypeIndex = imageType,
	};
	VkDeviceMemory imageMemory = VK_NULL_HANDLE;
	CHECK(6, "vkAllocateMemory(image)", pAllocateMemory(device, &imageAllocate, NULL, &imageMemory));
	CHECK(6, "vkBindImageMemory", pBindImageMemory(device, image, imageMemory, 0));
	printf("turnip_test: step 6: the image is backed by memory type %u\n", imageType);

	STEP(7, "an empty command buffer submitted, the fence signalled");
	VkCommandPoolCreateInfo poolInfo = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
	    .queueFamilyIndex = family,
	};
	CHECK(7, "vkCreateCommandPool", pCreateCommandPool(device, &poolInfo, NULL, &pool));
	VkCommandBufferAllocateInfo bufferInfo = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
	    .commandPool = pool,
	    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
	    .commandBufferCount = 1,
	};
	CHECK(7, "vkAllocateCommandBuffers", pAllocateCommandBuffers(device, &bufferInfo, &commands));
	VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	CHECK(7, "vkBeginCommandBuffer", pBeginCommandBuffer(commands, &begin));
	CHECK(7, "vkEndCommandBuffer", pEndCommandBuffer(commands));
	VkFenceCreateInfo fenceInfo = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	CHECK(7, "vkCreateFence", pCreateFence(device, &fenceInfo, NULL, &fence));
	VkSubmitInfo submit = {
	    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
	    .commandBufferCount = 1,
	    .pCommandBuffers = &commands,
	};
	CHECK(7, "vkQueueSubmit", pQueueSubmit(queue, 1, &submit, fence));
	/* Poll rather than vkWaitForFences: a stuck GPU is a result here, and the
	 * three-second timeout is generous against a ring the kernel polls at 5 kHz. */
	VkResult waited = VK_NOT_READY;
	for (int i = 0; i < 300; i++) {
		waited = pGetFenceStatus(device, fence);
		if (waited == VK_SUCCESS)
			break;
		usleep(10 * 1000);
	}
	if (waited != VK_SUCCESS)
		FAIL(7, "the submitted fence never signalled (3 s)", waited);
	printf("turnip_test: step 7: the submission completed\n");

	STEP(8, "a triangle drawn, copied back and compared");
	/* The one image the gate already made is the render target. Turnip's own
	 * BOs are used throughout: nothing is imported, so the fd-bo/pitch/last-level
	 * fixes K90's rebase-26 needed for external buffers are not yet in play. */
	VkImageViewCreateInfo viewInfo = {
	    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
	    .image = image,
	    .viewType = VK_IMAGE_VIEW_TYPE_2D,
	    .format = VK_FORMAT_R8G8B8A8_UNORM,
	    .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
	};
	CHECK(8, "vkCreateImageView", pCreateImageView(device, &viewInfo, NULL, &view));

	VkAttachmentDescription colorAttachment = {
	    .format = VK_FORMAT_R8G8B8A8_UNORM,
	    .samples = VK_SAMPLE_COUNT_1_BIT,
	    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
	    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
	    .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
	    .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
	    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	    /* the copy after the pass wants it in TRANSFER_SRC: doing it as the pass's
	     * final layout saves a barrier of our own */
	    .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	};
	VkAttachmentReference colorReference = {
	    .attachment = 0,
	    .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	};
	VkSubpassDescription subpass = {
	    .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
	    .colorAttachmentCount = 1,
	    .pColorAttachments = &colorReference,
	};
	VkRenderPassCreateInfo passInfo = {
	    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
	    .attachmentCount = 1,
	    .pAttachments = &colorAttachment,
	    .subpassCount = 1,
	    .pSubpasses = &subpass,
	};
	CHECK(8, "vkCreateRenderPass", pCreateRenderPass(device, &passInfo, NULL, &pass));

	VkFramebufferCreateInfo framebufferInfo = {
	    .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
	    .renderPass = pass,
	    .attachmentCount = 1,
	    .pAttachments = &view,
	    .width = 256,
	    .height = 256,
	    .layers = 1,
	};
	CHECK(8, "vkCreateFramebuffer", pCreateFramebuffer(device, &framebufferInfo, NULL, &framebuffer));

	VkShaderModuleCreateInfo vertInfo = {
	    .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
	    .codeSize = sizeof(triangle_vert_spirv),
	    .pCode = triangle_vert_spirv,
	};
	CHECK(8, "vkCreateShaderModule(vertex)", pCreateShaderModule(device, &vertInfo, NULL, &vertShader));
	VkShaderModuleCreateInfo fragInfo = {
	    .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
	    .codeSize = sizeof(triangle_frag_spirv),
	    .pCode = triangle_frag_spirv,
	};
	CHECK(8, "vkCreateShaderModule(fragment)", pCreateShaderModule(device, &fragInfo, NULL, &fragShader));

	VkPipelineShaderStageCreateInfo stages[2] = {
	    { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	      .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vertShader, .pName = "main" },
	    { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	      .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fragShader, .pName = "main" },
	};
	VkPipelineVertexInputStateCreateInfo vertexInput = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
	};
	VkPipelineInputAssemblyStateCreateInfo inputAssembly = {
	    .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
	    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
	};
	VkViewport viewport = { 0.0f, 0.0f, 256.0f, 256.0f, 0.0f, 1.0f };
	VkRect2D scissor = { { 0, 0 }, { 256, 256 } };
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
	CHECK(8, "vkCreatePipelineLayout", pCreatePipelineLayout(device, &layoutInfo, NULL, &layout));
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
	    .layout = layout,
	    .renderPass = pass,
	    .subpass = 0,
	};
	CHECK(8, "vkCreateGraphicsPipelines", pCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &pipeline));

	/* The swapchain step's second pipeline: the same draw with a green fragment
	 * shader, so frame 1 differs from frame 0 without depending on the clear
	 * colour (the a7xx generic-clear path currently loses the clear's RGB). */
	VkShaderModuleCreateInfo fragGreenInfo = {
	    .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
	    .codeSize = sizeof(triangle_green_frag_spirv),
	    .pCode = triangle_green_frag_spirv,
	};
	CHECK(10, "vkCreateShaderModule(fragment, green)", pCreateShaderModule(device, &fragGreenInfo, NULL, &fragShader2));
	VkPipelineShaderStageCreateInfo greenStages[2] = {
	    stages[0],
	    { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
	      .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fragShader2, .pName = "main" },
	};
	pipelineInfo.pStages = greenStages;
	CHECK(10, "vkCreateGraphicsPipelines(green)", pCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, NULL, &pipeline2));

	/* the readback buffer: the whole 256x256 image, RGBA8 */
	const VkDeviceSize bufferSize = 256 * 256 * 4;
	VkBufferCreateInfo readbackInfo = {
	    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
	    .size = bufferSize,
	    .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	CHECK(8, "vkCreateBuffer(readback)", pCreateBuffer(device, &readbackInfo, NULL, &buffer));
	VkMemoryRequirements bufferRequirements;
	pGetBufferMemoryRequirements(device, buffer, &bufferRequirements);
	uint32_t readbackType = UINT32_MAX;
	for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; i++) {
		VkMemoryPropertyFlags f = memoryProperties.memoryTypes[i].propertyFlags;

		if ((bufferRequirements.memoryTypeBits & (1u << i)) &&
		    (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
			readbackType = i;
			break;
		}
	}
	if (readbackType == UINT32_MAX) {
		for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; i++) {
			if ((bufferRequirements.memoryTypeBits & (1u << i)) &&
			    (memoryProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
				readbackType = i;
				break;
			}
		}
	}
	if (readbackType == UINT32_MAX) {
		printf("turnip_test: FAILED at step 8: no host-visible memory type for the readback buffer\n");
		failed_at = 8;
		goto out;
	}
	VkMemoryAllocateInfo bufferAllocate = {
	    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
	    .allocationSize = bufferRequirements.size,
	    .memoryTypeIndex = readbackType,
	};
	CHECK(8, "vkAllocateMemory(readback)", pAllocateMemory(device, &bufferAllocate, NULL, &bufferMemory));
	CHECK(8, "vkBindBufferMemory", pBindBufferMemory(device, buffer, bufferMemory, 0));

	VkCommandBuffer drawCommands = VK_NULL_HANDLE;
	VkCommandBufferAllocateInfo drawBufferInfo = {
	    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
	    .commandPool = pool,
	    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
	    .commandBufferCount = 1,
	};
	CHECK(8, "vkAllocateCommandBuffers(draw)", pAllocateCommandBuffers(device, &drawBufferInfo, &drawCommands));
	VkCommandBufferBeginInfo drawBegin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	CHECK(8, "vkBeginCommandBuffer(draw)", pBeginCommandBuffer(drawCommands, &drawBegin));
	VkClearValue clear = { .color = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } } };
	VkRenderPassBeginInfo passBegin = {
	    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
	    .renderPass = pass,
	    .framebuffer = framebuffer,
	    .renderArea = { { 0, 0 }, { 256, 256 } },
	    .clearValueCount = 1,
	    .pClearValues = &clear,
	};
	pCmdBeginRenderPass(drawCommands, &passBegin, VK_SUBPASS_CONTENTS_INLINE);
	pCmdBindPipeline(drawCommands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	pCmdDraw(drawCommands, 3, 1, 0, 0);
	pCmdEndRenderPass(drawCommands);
	VkBufferImageCopy region = {
	    .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
	    .imageExtent = { 256, 256, 1 },
	};
	pCmdCopyImageToBuffer(drawCommands, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
	CHECK(8, "vkEndCommandBuffer(draw)", pEndCommandBuffer(drawCommands));

	VkFence drawFence = VK_NULL_HANDLE;
	VkFenceCreateInfo drawFenceInfo = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	CHECK(8, "vkCreateFence(draw)", pCreateFence(device, &drawFenceInfo, NULL, &drawFence));
	VkSubmitInfo drawSubmit = {
	    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
	    .commandBufferCount = 1,
	    .pCommandBuffers = &drawCommands,
	};
	CHECK(8, "vkQueueSubmit(draw)", pQueueSubmit(queue, 1, &drawSubmit, drawFence));
	VkResult drawn = VK_NOT_READY;
	for (int i = 0; i < 300; i++) {
		drawn = pGetFenceStatus(device, drawFence);
		if (drawn == VK_SUCCESS)
			break;
		usleep(10 * 1000);
	}
	if (drawn != VK_SUCCESS)
		FAIL(8, "the draw's fence never signalled (3 s)", drawn);
	CHECK(8, "vkMapMemory(readback)", pMapMemory(device, bufferMemory, 0, VK_WHOLE_SIZE, 0, &mapped));
	{
		const uint8_t *pixels = mapped;
		size_t corner = (0 * 256 + 0) * 4;
		size_t middle = (128 * 256 + 128) * 4;

		printf("turnip_test:   corner (%u %u %u %u), middle (%u %u %u %u)\n",
		    pixels[corner + 0], pixels[corner + 1], pixels[corner + 2], pixels[corner + 3],
		    pixels[middle + 0], pixels[middle + 1], pixels[middle + 2], pixels[middle + 3]);
		fflush(stdout);
		/* the clear is opaque black; the fragment shader is pure red. A middle
		 * that is red and a corner that is black can only be the draw. */
		if (!(pixels[middle + 0] > 200 && pixels[middle + 1] < 40 && pixels[middle + 2] < 40)) {
			printf("turnip_test: FAILED at step 8: the middle is not the draw's colour\n");
			failed_at = 8;
			if (drawFence != VK_NULL_HANDLE)
				pDestroyFence(device, drawFence, NULL);
			goto out;
		}
		if (pixels[corner + 0] > 40 || pixels[corner + 1] > 40 || pixels[corner + 2] > 40) {
			printf("turnip_test: FAILED at step 8: the corner is not the clear colour\n");
			failed_at = 8;
			if (drawFence != VK_NULL_HANDLE)
				pDestroyFence(device, drawFence, NULL);
			goto out;
		}
		rendered = malloc(bufferSize);
		if (rendered == NULL)
			FAIL(8, "malloc(rendered)", VK_ERROR_OUT_OF_HOST_MEMORY);
		memcpy(rendered, mapped, bufferSize);
		pUnmapMemory(device, bufferMemory);
		if (drawFence != VK_NULL_HANDLE)
			pDestroyFence(device, drawFence, NULL);
	}
	printf("turnip_test: step 8: the triangle reached the image and read back\n");

	STEP(9, "the same pixels in an IOSurface (BGRA), read back from it");
	/* A swapchain image has to be something the rest of the system can take: an
	 * IOSurface is what the window server's layers, the 2D engine and
	 * CoreAnimation all read. Turnip's BOs are not IOSurfaces, so the first WSI
	 * step is the honest one: draw as before, then put the pixels into a surface
	 * in the byte order everything else expects (BGRA, alpha last). A zero-copy
	 * import of a surface's memory as a BO is the later, faster step. */
	{
		CFMutableDictionaryRef props = CFDictionaryCreateMutable(NULL, 0,
		    &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
		CFNumberRef value;
		int number = 256;

		if (props == NULL)
			FAIL(9, "CFDictionaryCreateMutable", VK_ERROR_OUT_OF_HOST_MEMORY);
		value = CFNumberCreate(NULL, kCFNumberIntType, &number);
		CFDictionarySetValue(props, kIOSurfaceWidth, value);
		CFDictionarySetValue(props, kIOSurfaceHeight, value);
		CFRelease(value);
		number = 4;
		value = CFNumberCreate(NULL, kCFNumberIntType, &number);
		CFDictionarySetValue(props, kIOSurfaceBytesPerElement, value);
		CFRelease(value);
		number = 256 * 4;
		value = CFNumberCreate(NULL, kCFNumberIntType, &number);
		CFDictionarySetValue(props, kIOSurfaceBytesPerRow, value);
		CFRelease(value);
		number = 0x42475241;                    /* 'BGRA' */
		value = CFNumberCreate(NULL, kCFNumberIntType, &number);
		CFDictionarySetValue(props, kIOSurfacePixelFormat, value);
		CFRelease(value);
		surface = IOSurfaceCreate(props);
		CFRelease(props);
		if (surface == NULL)
			FAIL(9, "IOSurfaceCreate(256x256 BGRA)", VK_ERROR_OUT_OF_HOST_MEMORY);

		uint32_t width = (uint32_t)IOSurfaceGetWidth(surface);
		uint32_t height = (uint32_t)IOSurfaceGetHeight(surface);
		size_t stride = IOSurfaceGetBytesPerRow(surface);
		uint8_t *bytes;

		printf("turnip_test:   surface %ux%u, stride %zu, id %u\n", width, height, stride, IOSurfaceGetID(surface));
		fflush(stdout);
		if (IOSurfaceLock(surface, 0, NULL) != 0)
			FAIL(9, "IOSurfaceLock(write)", VK_ERROR_MEMORY_MAP_FAILED);
		bytes = IOSurfaceGetBaseAddress(surface);
		for (uint32_t y = 0; y < height && y < 256; y++) {
			const uint8_t *src = rendered + (size_t)y * 256 * 4;
			uint8_t *dst = bytes + (size_t)y * stride;

			for (uint32_t x = 0; x < width && x < 256; x++) {
				dst[x * 4 + 0] = src[x * 4 + 2];         /* B */
				dst[x * 4 + 1] = src[x * 4 + 1];         /* G */
				dst[x * 4 + 2] = src[x * 4 + 0];         /* R */
				dst[x * 4 + 3] = src[x * 4 + 3];         /* A */
			}
		}
		IOSurfaceUnlock(surface, 0, NULL);

		/* and back out of the surface: what a consumer would read */
		if (IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL) != 0)
			FAIL(9, "IOSurfaceLock(read)", VK_ERROR_MEMORY_MAP_FAILED);
		bytes = IOSurfaceGetBaseAddress(surface);
		{
			const uint8_t *middle = bytes + (size_t)128 * stride + 128 * 4;
			const uint8_t *corner = bytes;

			printf("turnip_test:   surface corner (%u %u %u %u), middle (%u %u %u %u)\n",
			    corner[0], corner[1], corner[2], corner[3], middle[0], middle[1], middle[2], middle[3]);
			fflush(stdout);
			if (!(middle[0] < 40 && middle[1] < 40 && middle[2] > 200)) {
				IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
				printf("turnip_test: FAILED at step 9: the surface's middle is not the draw's colour\n");
				failed_at = 9;
				goto out;
			}
			if (corner[0] > 40 || corner[1] > 40 || corner[2] > 40) {
				IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
				printf("turnip_test: FAILED at step 9: the surface's corner is not the clear colour\n");
				failed_at = 9;
				goto out;
			}
		}
		IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
	}
	printf("turnip_test: step 9: the triangle is in an IOSurface, BGRA, read back\n");

	STEP(10, "a two-image swapchain: acquire, render, present, the compositor's surfaces verified");
	/* The WSI contract in miniature, with the gate playing both sides. The
	 * compositor owns two IOSurfaces; the client renders each acquired image
	 * and presents it in order. Frame 0 is the step-8 draw, already in the
	 * first surface. Frame 1 re-renders the same pipeline into a second image
	 * over a grey clear -- a stale or swapped present shows up immediately.
	 * Turnip's BOs are still not IOSurfaces, so each present is the honest
	 * copy, as in step 9. */
	{
		VkImageCreateInfo imageInfo = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		    .imageType = VK_IMAGE_TYPE_2D,
		    .format = VK_FORMAT_R8G8B8A8_UNORM,
		    .extent = { 256, 256, 1 },
		    .mipLevels = 1,
		    .arrayLayers = 1,
		    .samples = VK_SAMPLE_COUNT_1_BIT,
		    .tiling = VK_IMAGE_TILING_OPTIMAL,
		    .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
		    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		};
		VkImageViewCreateInfo viewInfo = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		    .image = image2,
		    .viewType = VK_IMAGE_VIEW_TYPE_2D,
		    .format = VK_FORMAT_R8G8B8A8_UNORM,
		    .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
		};
		VkFramebufferCreateInfo framebufferInfo = {
		    .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		    .renderPass = pass,
		    .attachmentCount = 1,
		    .pAttachments = &view2,
		    .width = 256,
		    .height = 256,
		    .layers = 1,
		};
		VkCommandBufferAllocateInfo commandsInfo = {
		    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		    .commandPool = pool,
		    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		    .commandBufferCount = 1,
		};
		VkFenceCreateInfo fenceInfo = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
		VkCommandBuffer commands2 = VK_NULL_HANDLE;
		VkMemoryRequirements requirements2;
		uint32_t type2 = UINT32_MAX;
		VkClearValue clear = { .color = { .float32 = { 0.0f, 1.0f, 0.0f, 1.0f } } };
		VkRenderPassBeginInfo begin = {
		    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		    .renderPass = pass,
		    .framebuffer = framebuffer2,
		    .renderArea = { .offset = { 0, 0 }, .extent = { 256, 256 } },
		    .clearValueCount = 1,
		    .pClearValues = &clear,
		};
		VkSubmitInfo submit = {
		    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		    .commandBufferCount = 1,
		    .pCommandBuffers = &commands2,
		};
		VkBufferImageCopy region = {
		    .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
		    .imageExtent = { 256, 256, 1 },
		};
		VkMemoryAllocateInfo allocate2 = {
		    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		    .allocationSize = 0,
		    .memoryTypeIndex = 0,
		};
		size_t frameBytes = 256ull * 256 * 4;
		uint8_t *mapped2 = NULL, *bytes2 = NULL;
		int drawn;

		CHECK(10, "vkCreateImage(swapchain image 1)", pCreateImage(device, &imageInfo, NULL, &image2));
		pGetImageMemoryRequirements(device, image2, &requirements2);
		printf("turnip_test: image 2 needs %llu bytes, alignment %llu, memory type bits %#x\n",
		    (unsigned long long)requirements2.size, (unsigned long long)requirements2.alignment,
		    requirements2.memoryTypeBits);
		for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; i++) {
			if ((requirements2.memoryTypeBits & (1u << i)) &&
			    !(memoryProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
				type2 = i;
				break;
			}
		}
		if (type2 == UINT32_MAX) {
			for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; i++) {
				if (requirements2.memoryTypeBits & (1u << i)) {
					type2 = i;
					break;
				}
			}
		}
		if (type2 == UINT32_MAX)
			FAIL(10, "no memory type for the second image", VK_ERROR_OUT_OF_DEVICE_MEMORY);
		allocate2.allocationSize = requirements2.size;
		allocate2.memoryTypeIndex = type2;
		CHECK(10, "vkAllocateMemory(swapchain image 1)", pAllocateMemory(device, &allocate2, NULL, &imageMemory2));;
		fflush(stdout);
		CHECK(10, "vkBindImageMemory(swapchain image 1)", pBindImageMemory(device, image2, imageMemory2, 0));;
		fflush(stdout);
		viewInfo.image = image2;   /* the struct captured image2's NULL at declaration */
		CHECK(10, "vkCreateImageView(swapchain image 1)", pCreateImageView(device, &viewInfo, NULL, &view2));;
		fflush(stdout);
		CHECK(10, "vkCreateFramebuffer(swapchain image 1)", pCreateFramebuffer(device, &framebufferInfo, NULL, &framebuffer2));;
		begin.framebuffer = framebuffer2;   /* same trap as viewInfo: the struct captured the NULL handle */
		fflush(stdout);
		CHECK(10, "vkAllocateCommandBuffers(swapchain image 1)", pAllocateCommandBuffers(device, &commandsInfo, &commands2));;
		fflush(stdout);
		CHECK(10, "vkCreateFence(swapchain image 1)", pCreateFence(device, &fenceInfo, NULL, &fence2));;
		fflush(stdout);

		/* acquire: the rotation's second image, rendered over the clear, with
		 * the green pipeline so the frame differs from frame 0. */
		CHECK(10, "vkBeginCommandBuffer(swapchain image 1)", pBeginCommandBuffer(commands2,
		    &(VkCommandBufferBeginInfo){ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }));
		printf("turnip_test:   the second command buffer began\n");
		fflush(stdout);
		VkClearValue clear2 = { .color = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } } };
		VkRenderPassBeginInfo begin2 = {
		    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		    .renderPass = pass,
		    .framebuffer = framebuffer2,
		    .renderArea = { .offset = { 0, 0 }, .extent = { 256, 256 } },
		    .clearValueCount = 1,
		    .pClearValues = &clear2,
		};
		pCmdBeginRenderPass(commands2, &begin2, VK_SUBPASS_CONTENTS_INLINE);
		pCmdBindPipeline(commands2, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline2);
		pCmdDraw(commands2, 3, 1, 0, 0);
		pCmdEndRenderPass(commands2);
		pCmdCopyImageToBuffer(commands2, image2, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
		CHECK(10, "vkEndCommandBuffer(swapchain image 1)", pEndCommandBuffer(commands2));
		printf("turnip_test:   the second command buffer recorded\n");
		fflush(stdout);
		CHECK(10, "vkQueueSubmit(swapchain image 1)", pQueueSubmit(queue, 1, &submit, fence2));
		printf("turnip_test:   the second draw is submitted, waiting for its fence\n");
		fflush(stdout);
		printf("turnip_test:   the second draw is submitted, waiting for its fence\n");
		fflush(stdout);
		drawn = VK_NOT_READY;
		for (unsigned i = 0; i < 300 && drawn != VK_SUCCESS; i++) {
			drawn = pGetFenceStatus(device, fence2);
			if (drawn == VK_SUCCESS)
				break;
			usleep(10 * 1000);
		}
		if (drawn != VK_SUCCESS)
			FAIL(10, "the second draw's fence never signalled (3 s)", drawn);
		printf("turnip_test:   the second draw's fence signalled\n");
		fflush(stdout);

		CHECK(10, "vkMapMemory(swapchain image 1)", pMapMemory(device, bufferMemory, 0, bufferSize, 0, (void **)&mapped2));
		rendered2 = malloc(frameBytes);
		if (rendered2 == NULL)
			FAIL(10, "malloc(rendered2)", VK_ERROR_OUT_OF_HOST_MEMORY);
		memcpy(rendered2, mapped2, frameBytes);
		pUnmapMemory(device, bufferMemory);
		printf("turnip_test:   the second frame read back\n");
		fflush(stdout);

		/* present: the frame lands in the compositor's second IOSurface, in
		 * the same BGRA order as step 9's present. */
		surface2 = make_bgra_surface();
		if (surface2 == NULL)
			FAIL(10, "IOSurfaceCreate(swapchain surface 1)", VK_ERROR_OUT_OF_HOST_MEMORY);
		printf("turnip_test:   surface 256x256, stride %zu, id %u\n",
		    IOSurfaceGetBytesPerRow(surface2), IOSurfaceGetID(surface2));
		fflush(stdout);
		if (IOSurfaceLock(surface2, 0, NULL) != 0)
			FAIL(10, "IOSurfaceLock(write)", VK_ERROR_MEMORY_MAP_FAILED);
		printf("turnip_test:   the second surface locked for the present copy\n");
		fflush(stdout);
		bytes2 = IOSurfaceGetBaseAddress(surface2);
		for (uint32_t y = 0; y < 256; y++) {
			const uint8_t *src = rendered2 + (size_t)y * 256 * 4;
			uint8_t *dst = bytes2 + (size_t)y * IOSurfaceGetBytesPerRow(surface2);

			for (uint32_t x = 0; x < 256; x++) {
				dst[x * 4 + 0] = src[x * 4 + 2];         /* B */
				dst[x * 4 + 1] = src[x * 4 + 1];         /* G */
				dst[x * 4 + 2] = src[x * 4 + 0];         /* R */
				dst[x * 4 + 3] = src[x * 4 + 3];         /* A */
			}
		}
		IOSurfaceUnlock(surface2, 0, NULL);
		printf("turnip_test: step 10: frame 1 rendered, presented as surface id %u\n", IOSurfaceGetID(surface2));
		fflush(stdout);

		/* the compositor reads both surfaces back: frame 0 is black with the
		 * red triangle, frame 1 grey with it. A swapped or stale present
		 * fails here. Offsets: stride is 1024, the middle pixel is
		 * (128,128) = 128*1024 + 128*4, the corner probe is pixel (1,0). */
		const uint32_t mid = 128 * 1024 + 128 * 4, corner = 4;
		const uint8_t *px;

		if (IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL) != 0)
			FAIL(10, "IOSurfaceLock(read, surface 0)", VK_ERROR_MEMORY_MAP_FAILED);
		px = IOSurfaceGetBaseAddress(surface);
		if (px[mid + 2] < 200 || px[mid + 0] > 40 || px[mid + 1] > 40)
			FAIL(10, "surface 0's middle is not frame 0's red", VK_ERROR_UNKNOWN);
		if (px[corner + 0] > 40 || px[corner + 1] > 40 || px[corner + 2] > 40)
			FAIL(10, "surface 0's corner is not frame 0's black", VK_ERROR_UNKNOWN);
		IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);

		if (IOSurfaceLock(surface2, kIOSurfaceLockReadOnly, NULL) != 0)
			FAIL(10, "IOSurfaceLock(read, surface 1)", VK_ERROR_MEMORY_MAP_FAILED);
		px = IOSurfaceGetBaseAddress(surface2);
		if (px[mid + 1] < 200 || px[mid + 0] > 40 || px[mid + 2] > 40)
			FAIL(10, "surface 1's middle is not the green triangle", VK_ERROR_UNKNOWN);
		if (px[corner + 0] > 40 && px[corner + 1] > 40 && px[corner + 2] > 40) {
			printf("turnip_test:   surface 1 corner (1,0) BGRA = %u %u %u %u, middle BGRA = %u %u %u %u\n",
			    px[corner + 0], px[corner + 1], px[corner + 2], px[corner + 3],
			    px[mid + 0], px[mid + 1], px[mid + 2], px[mid + 3]);
			fflush(stdout);
			FAIL(10, "surface 1's corner is not frame 1's green", VK_ERROR_UNKNOWN);
		}
		IOSurfaceUnlock(surface2, kIOSurfaceLockReadOnly, NULL);

		printf("turnip_test: step 10: both presents landed, in order, and the compositor read them\n");

		/* Round-2 probe: the a7xx generic clear loses RGB (green clears to
		 * (0,0,0,255), grey to black). Clear with channels that cannot be
		 * confused -- distinct values, and an alpha of 0 in the first and last
		 * -- and print the raw bytes. Which lane survives says whether the
		 * clear runs at all, writes alpha only, or misreads the packed value.
		 *
		 * 0.25/0.5/0.75/0.0  packed RGBA8 = 0x00C08040, DW1..3 = 0
		 * 0/0/0/1            packed       = 0xFF000000
		 * 1/0/0/0            packed       = 0x000000FF
		 *
		 * "clear works" reads (64 128 191 0), (0 0 0 255), (255 0 0 0).
		 * "alpha only"  reads (0 0 0 0), (0 0 0 255), (0 0 0 0).
		 * "four floats, DW0 -> alpha, clamped" reads the same as alpha-only
		 * except the middle one, which stays (0 0 0 255). */
		{
			static const VkClearValue probes[3] = {
			    { .color = { .float32 = { 0.25f, 0.5f, 0.75f, 0.0f } } },
			    { .color = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } } },
			    { .color = { .float32 = { 1.0f, 0.0f, 0.0f, 0.0f } } },
			};

			for (unsigned p = 0; p < 3; p++) {
				VkCommandBuffer commands3 = VK_NULL_HANDLE;
				VkFence fence3 = VK_NULL_HANDLE;
				VkFenceCreateInfo fenceInfo3 = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
				VkRenderPassBeginInfo probeBegin = {
				    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
				    .renderPass = pass,
				    .framebuffer = framebuffer2,
				    .renderArea = { .offset = { 0, 0 }, .extent = { 256, 256 } },
				    .clearValueCount = 1,
				    .pClearValues = &probes[p],
				};
				VkSubmitInfo probeSubmit = {
				    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
				    .commandBufferCount = 1,
				    .pCommandBuffers = &commands3,
				};
				uint8_t *probeBytes = NULL;
				int done = VK_NOT_READY;

				CHECK(10, "vkAllocateCommandBuffers(clear probe)", pAllocateCommandBuffers(device, &commandsInfo, &commands3));
				CHECK(10, "vkCreateFence(clear probe)", pCreateFence(device, &fenceInfo3, NULL, &fence3));
				CHECK(10, "vkBeginCommandBuffer(clear probe)", pBeginCommandBuffer(commands3,
				    &(VkCommandBufferBeginInfo){ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }));
				pCmdBeginRenderPass(commands3, &probeBegin, VK_SUBPASS_CONTENTS_INLINE);
				pCmdEndRenderPass(commands3);
				pCmdCopyImageToBuffer(commands3, image2, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
				CHECK(10, "vkEndCommandBuffer(clear probe)", pEndCommandBuffer(commands3));
				CHECK(10, "vkQueueSubmit(clear probe)", pQueueSubmit(queue, 1, &probeSubmit, fence3));
				for (unsigned i = 0; i < 300 && done != VK_SUCCESS; i++) {
					done = pGetFenceStatus(device, fence3);
					if (done == VK_SUCCESS)
						break;
					usleep(10 * 1000);
				}
				if (done != VK_SUCCESS)
					FAIL(10, "the clear probe's fence never signalled (3 s)", done);
				CHECK(10, "vkMapMemory(clear probe)", pMapMemory(device, bufferMemory, 0, bufferSize, 0, (void **)&probeBytes));
				printf("turnip_test: probe %u: clear (%.2f %.2f %.2f %.2f) reads back RGBA %u %u %u %u, mid %u %u %u %u\n",
				    p, probes[p].color.float32[0], probes[p].color.float32[1],
				    probes[p].color.float32[2], probes[p].color.float32[3],
				    probeBytes[0], probeBytes[1], probeBytes[2], probeBytes[3],
				    probeBytes[mid + 0], probeBytes[mid + 1], probeBytes[mid + 2], probeBytes[mid + 3]);
				fflush(stdout);
				pUnmapMemory(device, bufferMemory);
			}
		}
	}

	STEP(11, "an IOSurface imported as a Vulkan image: the draw lands in the compositor's memory, no copy");
	/* The step the GPU line turns on: the compositor's surface IS the render
	 * target. The kernel's RedwoodGPU already hands out an IOSurface's pages as
	 * a GPU buffer (kRedwoodSurface, 0x7e), and this port's "fd" is the surface
	 * id (drmPrimeFDToHandle in the shim). So: a linear VkImage with an
	 * external-memory handle, memory imported by that id, render into it, then
	 * read the IOSurface directly -- no vkCmdCopyImageToBuffer, no frame copy.
	 * A linear import also exercises the K90 transplant items (pitch tolerance,
	 * last-level padding) for the first time. */
	{
		IOSurfaceRef surface3 = make_bgra_surface();
		VkExternalMemoryImageCreateInfo external = {
		    .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
		    .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
		};
		VkImageCreateInfo imageInfo3 = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		    .pNext = &external,
		    .imageType = VK_IMAGE_TYPE_2D,
		    /* the compositor's surfaces are BGRA in memory: the CPU-copy
		     * presents swapped R/B by hand, a zero-copy import must declare
		     * the surface's real order or the swap lands in the pixels */
		    .format = VK_FORMAT_B8G8R8A8_UNORM,
		    .extent = { 256, 256, 1 },
		    .mipLevels = 1,
		    .arrayLayers = 1,
		    .samples = VK_SAMPLE_COUNT_1_BIT,
		    .tiling = VK_IMAGE_TILING_LINEAR,
		    .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
		    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		};
		VkImage image3 = VK_NULL_HANDLE;
		VkDeviceMemory memory3 = VK_NULL_HANDLE;
		VkImageView view3 = VK_NULL_HANDLE;
		VkFramebuffer fb3 = VK_NULL_HANDLE;
		VkCommandBuffer cmd3 = VK_NULL_HANDLE;
		VkFence fence3 = VK_NULL_HANDLE;
		VkMemoryRequirements req3 = { 0 };
		uint32_t type3 = UINT32_MAX;
		VkImportMemoryFdInfoKHR import = { 0 };
		VkMemoryAllocateInfo allocate3 = { 0 };
		int done3 = VK_NOT_READY;

		if (surface3 == NULL)
			FAIL(11, "IOSurfaceCreate(import target)", VK_ERROR_OUT_OF_HOST_MEMORY);
		printf("turnip_test:   surface 256x256, stride %zu, id %u (the import target)\n",
		    IOSurfaceGetBytesPerRow(surface3), IOSurfaceGetID(surface3));
		fflush(stdout);

		CHECK(11, "vkCreateImage(imported surface)", pCreateImage(device, &imageInfo3, NULL, &image3));
		pGetImageMemoryRequirements(device, image3, &req3);
		printf("turnip_test:   the imported image needs %llu bytes, alignment %llu, type bits %#x\n",
		    (unsigned long long)req3.size, (unsigned long long)req3.alignment, req3.memoryTypeBits);
		for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; i++) {
			if ((req3.memoryTypeBits & (1u << i)) &&
			    !(memoryProperties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
				type3 = i;
				break;
			}
		}
		if (type3 == UINT32_MAX) {
			/* every heap on this driver is host-visible system memory */
			for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; i++) {
				if (req3.memoryTypeBits & (1u << i)) {
					type3 = i;
					break;
				}
			}
		}
		if (type3 == UINT32_MAX)
			FAIL(11, "no memory type for the imported image", VK_ERROR_OUT_OF_DEVICE_MEMORY);
		import.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
		import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
		import.fd = (int)IOSurfaceGetID(surface3);
		allocate3.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocate3.pNext = &import;
		allocate3.allocationSize = req3.size;
		allocate3.memoryTypeIndex = type3;
		CHECK(11, "vkAllocateMemory(import the surface's memory)", pAllocateMemory(device, &allocate3, NULL, &memory3));
		fflush(stdout);
		CHECK(11, "vkBindImageMemory(imported surface)", pBindImageMemory(device, image3, memory3, 0));
		fflush(stdout);

		VkImageViewCreateInfo viewInfo3 = {
		    .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		    .image = image3,
		    .viewType = VK_IMAGE_VIEW_TYPE_2D,
		    .format = VK_FORMAT_B8G8R8A8_UNORM,
		    .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
		};
		CHECK(11, "vkCreateImageView(imported surface)", pCreateImageView(device, &viewInfo3, NULL, &view3));
		VkFramebufferCreateInfo fbInfo3 = {
		    .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		    .renderPass = pass,
		    .attachmentCount = 1,
		    .pAttachments = &view3,
		    .width = 256,
		    .height = 256,
		    .layers = 1,
		};
		CHECK(11, "vkCreateFramebuffer(imported surface)", pCreateFramebuffer(device, &fbInfo3, NULL, &fb3));
		fflush(stdout);

		VkCommandBufferAllocateInfo cmdInfo3 = {
		    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		    .commandPool = pool,
		    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		    .commandBufferCount = 1,
		};
		CHECK(11, "vkAllocateCommandBuffers(imported surface)", pAllocateCommandBuffers(device, &cmdInfo3, &cmd3));
		VkFenceCreateInfo fenceInfo3 = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
		CHECK(11, "vkCreateFence(imported surface)", pCreateFence(device, &fenceInfo3, NULL, &fence3));

		VkClearValue clear3 = { .color = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } } };
		VkRenderPassBeginInfo begin3 = {
		    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		    .renderPass = pass,
		    .framebuffer = fb3,
		    .renderArea = { .offset = { 0, 0 }, .extent = { 256, 256 } },
		    .clearValueCount = 1,
		    .pClearValues = &clear3,
		};
		CHECK(11, "vkBeginCommandBuffer(imported surface)", pBeginCommandBuffer(cmd3,
		    &(VkCommandBufferBeginInfo){ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }));
		pCmdBeginRenderPass(cmd3, &begin3, VK_SUBPASS_CONTENTS_INLINE);
		pCmdBindPipeline(cmd3, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
		pCmdDraw(cmd3, 3, 1, 0, 0);
		pCmdEndRenderPass(cmd3);
		CHECK(11, "vkEndCommandBuffer(imported surface)", pEndCommandBuffer(cmd3));
		VkSubmitInfo submit3 = {
		    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		    .commandBufferCount = 1,
		    .pCommandBuffers = &cmd3,
		};
		CHECK(11, "vkQueueSubmit(imported surface)", pQueueSubmit(queue, 1, &submit3, fence3));
		for (unsigned i = 0; i < 300 && done3 != VK_SUCCESS; i++) {
			done3 = pGetFenceStatus(device, fence3);
			if (done3 == VK_SUCCESS)
				break;
			usleep(10 * 1000);
		}
		if (done3 != VK_SUCCESS)
			FAIL(11, "the imported-surface draw's fence never signalled (3 s)", done3);

		/* No copy: read the compositor's own surface, which the GPU wrote. */
		if (IOSurfaceLock(surface3, kIOSurfaceLockReadOnly, NULL) != 0)
			FAIL(11, "IOSurfaceLock(read, imported surface)", VK_ERROR_MEMORY_MAP_FAILED);
		{
			const uint8_t *bytes3 = IOSurfaceGetBaseAddress(surface3);
			size_t stride3 = IOSurfaceGetBytesPerRow(surface3);
			const uint8_t *corner3 = bytes3 + 4;                     /* pixel (1,0), outside the triangle */
			const uint8_t *middle3 = bytes3 + (size_t)128 * stride3 + 128 * 4;

			printf("turnip_test:   the surface the GPU wrote: corner BGRA %u %u %u %u, middle BGRA %u %u %u %u\n",
			    corner3[0], corner3[1], corner3[2], corner3[3],
			    middle3[0], middle3[1], middle3[2], middle3[3]);
			fflush(stdout);
			IOSurfaceUnlock(surface3, kIOSurfaceLockReadOnly, NULL);
			if (!(middle3[2] > 200 && middle3[1] < 40 && middle3[0] < 40)) {
				printf("turnip_test: FAILED at step 11: the imported surface's middle is not the draw's colour\n");
				failed_at = 11;
				goto out;
			}
			if (corner3[0] > 40 || corner3[1] > 40 || corner3[2] > 40) {
				printf("turnip_test: FAILED at step 11: the imported surface's corner is not the clear colour\n");
				failed_at = 11;
				goto out;
			}
		}
		printf("turnip_test: step 11: the draw is in the compositor's surface, imported by its id, no copy\n");

		/* Step 12 rides the same import as a present loop. Phase A: 32 frames,
		 * each record -> submit -> wait -> read of the compositor's surface,
		 * timed (a swapchain that waits every frame). Phase B: 32 more
		 * submitted back to back and only the last fence waited (queue depth).
		 * Each frame's clear carries its index in a channel -- red for phase A,
		 * green for phase B -- so the surface itself says which frame landed
		 * last: a dropped or reordered present fails the corner check. */
		STEP(12, "a present loop: 32 timed frames into the imported surface, then 32 pipelined");
		{
			enum { FRAMES = 32 };
			VkCommandBuffer ca[FRAMES], cb[FRAMES];
			VkFence fa[FRAMES], fb[FRAMES];
			double t12[FRAMES], pipelined;
			struct timespec ta, tb;
			VkCommandBufferAllocateInfo ai12 = {
			    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			    .commandPool = pool,
			    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			    .commandBufferCount = FRAMES,
			};
			VkFenceCreateInfo fi12 = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };

			CHECK(12, "vkAllocateCommandBuffers(present loop A)", pAllocateCommandBuffers(device, &ai12, ca));
			CHECK(12, "vkAllocateCommandBuffers(present loop B)", pAllocateCommandBuffers(device, &ai12, cb));
			for (unsigned i = 0; i < FRAMES; i++) {
				/* one index step = 8/255, exact in 8 bits */
				VkClearValue cva = { .color = { .float32 = { (float)(i * 8) / 255.0f, 0.0f, 0.0f, 1.0f } } };
				VkClearValue cvb = { .color = { .float32 = { 0.0f, (float)(i * 8) / 255.0f, 0.0f, 1.0f } } };
				VkRenderPassBeginInfo ba = {
				    .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
				    .renderPass = pass,
				    .framebuffer = fb3,
				    .renderArea = { .offset = { 0, 0 }, .extent = { 256, 256 } },
				    .clearValueCount = 1,
				    .pClearValues = &cva,
				};
				VkRenderPassBeginInfo bb = ba;

				bb.pClearValues = &cvb;
				CHECK(12, "vkCreateFence(present loop)", pCreateFence(device, &fi12, NULL, &fa[i]));
				CHECK(12, "vkCreateFence(present loop)", pCreateFence(device, &fi12, NULL, &fb[i]));
				CHECK(12, "vkBeginCommandBuffer(present loop A)", pBeginCommandBuffer(ca[i],
				    &(VkCommandBufferBeginInfo){ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }));
				pCmdBeginRenderPass(ca[i], &ba, VK_SUBPASS_CONTENTS_INLINE);
				pCmdBindPipeline(ca[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
				pCmdDraw(ca[i], 3, 1, 0, 0);
				pCmdEndRenderPass(ca[i]);
				CHECK(12, "vkEndCommandBuffer(present loop A)", pEndCommandBuffer(ca[i]));

				CHECK(12, "vkBeginCommandBuffer(present loop B)", pBeginCommandBuffer(cb[i],
				    &(VkCommandBufferBeginInfo){ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }));
				pCmdBeginRenderPass(cb[i], &bb, VK_SUBPASS_CONTENTS_INLINE);
				pCmdBindPipeline(cb[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
				pCmdDraw(cb[i], 3, 1, 0, 0);
				pCmdEndRenderPass(cb[i]);
				CHECK(12, "vkEndCommandBuffer(present loop B)", pEndCommandBuffer(cb[i]));
			}

			/* phase A: wait every frame, and read the surface each time */
			for (unsigned i = 0; i < FRAMES; i++) {
				VkSubmitInfo si = {
				    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
				    .commandBufferCount = 1,
				    .pCommandBuffers = &ca[i],
				};
				int done = VK_NOT_READY;
				unsigned want = i * 8, got;
				const uint8_t *px;

				clock_gettime(CLOCK_MONOTONIC, &ta);
				CHECK(12, "vkQueueSubmit(present loop A)", pQueueSubmit(queue, 1, &si, fa[i]));
				for (unsigned k = 0; k < 3000 && done != VK_SUCCESS; k++) {
					done = pGetFenceStatus(device, fa[i]);
					if (done == VK_SUCCESS)
						break;
					usleep(1000);
				}
				clock_gettime(CLOCK_MONOTONIC, &tb);
				if (done != VK_SUCCESS)
					FAIL(12, "a present-loop fence never signalled (3 s)", done);
				t12[i] = ((tb.tv_sec - ta.tv_sec) * 1e3) + ((tb.tv_nsec - ta.tv_nsec) / 1e6);

				/* the port's rule: whoever had the GPU draw into a surface
				 * says so (a write lock/unlock moves the seed), or the kernel
				 * keeps this processor's stale cache lines of it. eagl.c does
				 * the same; a WSI layer is where this belongs eventually. */
				if (IOSurfaceLock(surface3, 0, NULL) == 0)
					IOSurfaceUnlock(surface3, 0, NULL);
				if (IOSurfaceLock(surface3, kIOSurfaceLockReadOnly, NULL) != 0)
					FAIL(12, "IOSurfaceLock(present loop read)", VK_ERROR_MEMORY_MAP_FAILED);
				px = IOSurfaceGetBaseAddress(surface3);
				got = px[2];                      /* BGRA: red byte */
				IOSurfaceUnlock(surface3, kIOSurfaceLockReadOnly, NULL);
				if (got + 2 < want || got > want + 2) {
					printf("turnip_test:   frame %u: surface red %u, expected %u\n", i, got, want);
					fflush(stdout);
					FAIL(12, "a present-loop frame did not land in the surface", VK_ERROR_UNKNOWN);
				}
			}

			/* phase B: submit all, wait the last (the queue is in order) */
			{
				int done = VK_NOT_READY;
				unsigned got, want = (FRAMES - 1) * 8;

				clock_gettime(CLOCK_MONOTONIC, &ta);
				for (unsigned i = 0; i < FRAMES; i++) {
					VkSubmitInfo si = {
					    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
					    .commandBufferCount = 1,
					    .pCommandBuffers = &cb[i],
					};
					CHECK(12, "vkQueueSubmit(present loop B)", pQueueSubmit(queue, 1, &si, fb[i]));
				}
				for (unsigned k = 0; k < 3000 && done != VK_SUCCESS; k++) {
					done = pGetFenceStatus(device, fb[FRAMES - 1]);
					if (done == VK_SUCCESS)
						break;
					usleep(1000);
				}
				clock_gettime(CLOCK_MONOTONIC, &tb);
				if (done != VK_SUCCESS)
					FAIL(12, "the pipelined present's last fence never signalled (3 s)", done);
				pipelined = ((tb.tv_sec - ta.tv_sec) * 1e3) + ((tb.tv_nsec - ta.tv_nsec) / 1e6);

				if (IOSurfaceLock(surface3, 0, NULL) == 0)
					IOSurfaceUnlock(surface3, 0, NULL);
				if (IOSurfaceLock(surface3, kIOSurfaceLockReadOnly, NULL) != 0)
					FAIL(12, "IOSurfaceLock(pipelined read)", VK_ERROR_MEMORY_MAP_FAILED);
				{
					const uint8_t *px = IOSurfaceGetBaseAddress(surface3);

					got = px[1];                  /* BGRA: green byte */
					IOSurfaceUnlock(surface3, kIOSurfaceLockReadOnly, NULL);
				}
				if (got + 2 < want || got > want + 2) {
					printf("turnip_test:   the last pipelined frame: surface green %u, expected %u\n", got, want);
					fflush(stdout);
					FAIL(12, "the last pipelined frame is not the one in the surface", VK_ERROR_UNKNOWN);
				}
			}

			/* min / median / max of phase A */
			{
				double sorted[FRAMES], sum = 0.0;

				memcpy(sorted, t12, sizeof(t12));
				for (unsigned i = 1; i < FRAMES; i++) {
					double v = sorted[i];
					unsigned j = i;

					while (j > 0 && sorted[j - 1] > v) {
						sorted[j] = sorted[j - 1];
						j--;
					}
					sorted[j] = v;
				}
				for (unsigned i = 0; i < FRAMES; i++)
					sum += t12[i];
				printf("turnip_test:   %d frames, wait each: min %.2f ms, median %.2f, max %.2f, mean %.2f\n",
				    FRAMES, sorted[0], sorted[FRAMES / 2], sorted[FRAMES - 1], sum / FRAMES);
				printf("turnip_test:   %d frames pipelined: %.2f ms total, %.2f ms/frame\n",
				    FRAMES, pipelined, pipelined / FRAMES);
				fflush(stdout);
			}
		}
		printf("turnip_test: step 12: the present loop ran, every frame landed in the surface in order\n");
	}

	printf("turnip_test: TURNIP GATE: PASS (steps 1-12)\n");
	result = 0;
out:
	if (device != VK_NULL_HANDLE) {
		/* Not after a fence that never signalled: waiting for idle there is
		 * waiting for the same stuck GPU a second time. */
		if (failed_at != 7 && failed_at != 8)
			pDeviceWaitIdle(device);
		if (fence != VK_NULL_HANDLE)
			pDestroyFence(device, fence, NULL);
		if (pipeline != VK_NULL_HANDLE)
			pDestroyPipeline(device, pipeline, NULL);
		if (pipeline2 != VK_NULL_HANDLE)
			pDestroyPipeline(device, pipeline2, NULL);
		if (fragShader2 != VK_NULL_HANDLE)
			pDestroyShaderModule(device, fragShader2, NULL);
		if (layout != VK_NULL_HANDLE)
			pDestroyPipelineLayout(device, layout, NULL);
		if (framebuffer != VK_NULL_HANDLE)
			pDestroyFramebuffer(device, framebuffer, NULL);
		if (pass != VK_NULL_HANDLE)
			pDestroyRenderPass(device, pass, NULL);
		if (vertShader != VK_NULL_HANDLE)
			pDestroyShaderModule(device, vertShader, NULL);
		if (fragShader != VK_NULL_HANDLE)
			pDestroyShaderModule(device, fragShader, NULL);
		if (view != VK_NULL_HANDLE)
			pDestroyImageView(device, view, NULL);
		if (buffer != VK_NULL_HANDLE)
			pDestroyBuffer(device, buffer, NULL);
		if (bufferMemory != VK_NULL_HANDLE)
			pFreeMemory(device, bufferMemory, NULL);
		if (pool != VK_NULL_HANDLE)
			pDestroyCommandPool(device, pool, NULL);
		if (image != VK_NULL_HANDLE)
			pDestroyImage(device, image, NULL);
		if (memory != VK_NULL_HANDLE)
			pFreeMemory(device, memory, NULL);
		if (fence2 != VK_NULL_HANDLE)
			pDestroyFence(device, fence2, NULL);
		if (framebuffer2 != VK_NULL_HANDLE)
			pDestroyFramebuffer(device, framebuffer2, NULL);
		if (view2 != VK_NULL_HANDLE)
			pDestroyImageView(device, view2, NULL);
		if (image2 != VK_NULL_HANDLE)
			pDestroyImage(device, image2, NULL);
		if (imageMemory2 != VK_NULL_HANDLE)
			pFreeMemory(device, imageMemory2, NULL);
		pDestroyDevice(device, NULL);
	}
	free(rendered);
	free(rendered2);
	if (surface != NULL)
		CFRelease(surface);
	if (surface2 != NULL)
		CFRelease(surface2);
	free(devices);
	if (instance != VK_NULL_HANDLE && pDestroyInstance != NULL)
		pDestroyInstance(instance, NULL);
	if (library != NULL)
		dlclose(library);
	if (failed_at != 0)
		printf("turnip_test: TURNIP GATE: FAIL at step %d\n", failed_at);
	return result;
}
