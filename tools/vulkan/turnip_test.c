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
 *
 * Step 8 is the stage the gate stops short of: it answers whether Turnip's
 * pipeline and draw paths (ir3 compilation, the render pass, the CP's own draw
 * packets) run on this Adreno, not just its command submission.
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
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

#define STEP(n, what) do { step = (n); printf("turnip_test: step %d: %s\n", (n), (what)); fflush(stdout); } while (0)
#define FAIL(n, what, r) do { \
	printf("turnip_test: FAILED at step %d (%s): %s\n", (n), (what), result_name(r)); \
	fflush(stdout); failed_at = (n); goto out; \
} while (0)
#define CHECK(n, what, r) do { VkResult _r = (r); if (_r != VK_SUCCESS) FAIL((n), (what), _r); } while (0)

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
	VkBuffer buffer = VK_NULL_HANDLE;
	VkDeviceMemory bufferMemory = VK_NULL_HANDLE;
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
		pUnmapMemory(device, bufferMemory);
		if (drawFence != VK_NULL_HANDLE)
			pDestroyFence(device, drawFence, NULL);
	}
	printf("turnip_test: step 8: the triangle reached the image and read back\n");

	printf("turnip_test: TURNIP GATE: PASS (steps 1-8)\n");
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
		pDestroyDevice(device, NULL);
	}
	free(devices);
	if (instance != VK_NULL_HANDLE && pDestroyInstance != NULL)
		pDestroyInstance(instance, NULL);
	if (library != NULL)
		dlclose(library);
	if (failed_at != 0)
		printf("turnip_test: TURNIP GATE: FAIL at step %d\n", failed_at);
	return result;
}
