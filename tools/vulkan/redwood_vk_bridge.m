/*
 * The Vulkan half of the CA bridge probe: one VkDevice, one render pass, and a
 * clear into a CoreAnimation drawable's IOSurface.
 *
 * Read wsi_redwood.h first: acquire/present are its contract, and the import
 * below is the sequence step 11 (turnip_test) and step 13 (vk_triangle_app)
 * measured -- external DMA_BUF memory whose "fd" is the IOSurface id, linear
 * BGRA, no copy. The import is duplicated here rather than shared with the app
 * so the proven app path is not edited for the probe.
 */
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurfaceRef.h>

#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <objc/runtime.h>
#include <vulkan/vulkan.h>

#include "redwood_vk_bridge.h"
#include "wsi_redwood.h"

#define RWB_ICD "/redwood/lib/libvulkan_freedreno.dylib"
#define RWB_ENTRIES 8

struct rwb_entry {
	uint32_t sid;
	VkImage image;
	VkDeviceMemory memory;
	VkImageView view;
	VkFramebuffer framebuffer;
};

struct rwb {
	void *icd;

	VkInstance instance;
	VkPhysicalDevice physical;
	VkDevice device;
	VkQueue queue;
	uint32_t family;

	VkRenderPass pass;
	VkCommandPool pool;
	VkCommandBuffer command;
	VkFence fence;

	struct rwb_entry entries[RWB_ENTRIES];
	unsigned entries_used;

	PFN_vkDestroyInstance destroy_instance;
	PFN_vkDestroyDevice destroy_device;
	PFN_vkGetDeviceQueue get_device_queue;
	PFN_vkCreateImage create_image;
	PFN_vkDestroyImage destroy_image;
	PFN_vkGetImageMemoryRequirements image_memory_requirements;
	PFN_vkAllocateMemory allocate_memory;
	PFN_vkFreeMemory free_memory;
	PFN_vkBindImageMemory bind_image_memory;
	PFN_vkCreateImageView create_image_view;
	PFN_vkDestroyImageView destroy_image_view;
	PFN_vkCreateRenderPass create_render_pass;
	PFN_vkDestroyRenderPass destroy_render_pass;
	PFN_vkCreateFramebuffer create_framebuffer;
	PFN_vkDestroyFramebuffer destroy_framebuffer;
	PFN_vkCreateCommandPool create_command_pool;
	PFN_vkDestroyCommandPool destroy_command_pool;
	PFN_vkAllocateCommandBuffers allocate_command_buffers;
	PFN_vkResetCommandPool reset_command_pool;
	PFN_vkBeginCommandBuffer begin_command_buffer;
	PFN_vkEndCommandBuffer end_command_buffer;
	PFN_vkCmdBeginRenderPass cmd_begin_render_pass;
	PFN_vkCmdEndRenderPass cmd_end_render_pass;
	PFN_vkQueueSubmit queue_submit;
	PFN_vkCreateFence create_fence;
	PFN_vkDestroyFence destroy_fence;
	PFN_vkWaitForFences wait_for_fences;
	PFN_vkResetFences reset_fences;
	PFN_vkDeviceWaitIdle device_wait_idle;
};

static void
rwb_fail(char *why, size_t why_size, const char *what)
{
	if (why && why_size)
		snprintf(why, why_size, "%s", what);
}

/*
 * The PFN_ names are camelCase (PFN_vkGetDeviceQueue); the struct's fields are
 * snaked, because the macro below pastes the field name. One typedef per call
 * keeps both spellings honest.
 */
typedef PFN_vkGetDeviceQueue PFN_get_device_queue;
typedef PFN_vkCreateImage PFN_create_image;
typedef PFN_vkDestroyImage PFN_destroy_image;
typedef PFN_vkGetImageMemoryRequirements PFN_image_memory_requirements;
typedef PFN_vkAllocateMemory PFN_allocate_memory;
typedef PFN_vkFreeMemory PFN_free_memory;
typedef PFN_vkBindImageMemory PFN_bind_image_memory;
typedef PFN_vkCreateImageView PFN_create_image_view;
typedef PFN_vkDestroyImageView PFN_destroy_image_view;
typedef PFN_vkCreateRenderPass PFN_create_render_pass;
typedef PFN_vkDestroyRenderPass PFN_destroy_render_pass;
typedef PFN_vkCreateFramebuffer PFN_create_framebuffer;
typedef PFN_vkDestroyFramebuffer PFN_destroy_framebuffer;
typedef PFN_vkCreateCommandPool PFN_create_command_pool;
typedef PFN_vkDestroyCommandPool PFN_destroy_command_pool;
typedef PFN_vkAllocateCommandBuffers PFN_allocate_command_buffers;
typedef PFN_vkResetCommandPool PFN_reset_command_pool;
typedef PFN_vkBeginCommandBuffer PFN_begin_command_buffer;
typedef PFN_vkEndCommandBuffer PFN_end_command_buffer;
typedef PFN_vkCmdBeginRenderPass PFN_cmd_begin_render_pass;
typedef PFN_vkCmdEndRenderPass PFN_cmd_end_render_pass;
typedef PFN_vkQueueSubmit PFN_queue_submit;
typedef PFN_vkCreateFence PFN_create_fence;
typedef PFN_vkDestroyFence PFN_destroy_fence;
typedef PFN_vkWaitForFences PFN_wait_for_fences;
typedef PFN_vkResetFences PFN_reset_fences;
typedef PFN_vkDeviceWaitIdle PFN_device_wait_idle;
typedef PFN_vkDestroyDevice PFN_destroy_device;

/*
 * vkGetDeviceProcAddr wants the real Vulkan name; the struct fields are snaked.
 * One string per field, next to the typedef above, so RWB_LOAD stays one line.
 */
#define RWB_FN_get_device_queue "vkGetDeviceQueue"
#define RWB_FN_create_image "vkCreateImage"
#define RWB_FN_destroy_image "vkDestroyImage"
#define RWB_FN_image_memory_requirements "vkGetImageMemoryRequirements"
#define RWB_FN_allocate_memory "vkAllocateMemory"
#define RWB_FN_free_memory "vkFreeMemory"
#define RWB_FN_bind_image_memory "vkBindImageMemory"
#define RWB_FN_create_image_view "vkCreateImageView"
#define RWB_FN_destroy_image_view "vkDestroyImageView"
#define RWB_FN_create_render_pass "vkCreateRenderPass"
#define RWB_FN_destroy_render_pass "vkDestroyRenderPass"
#define RWB_FN_create_framebuffer "vkCreateFramebuffer"
#define RWB_FN_destroy_framebuffer "vkDestroyFramebuffer"
#define RWB_FN_create_command_pool "vkCreateCommandPool"
#define RWB_FN_destroy_command_pool "vkDestroyCommandPool"
#define RWB_FN_allocate_command_buffers "vkAllocateCommandBuffers"
#define RWB_FN_reset_command_pool "vkResetCommandPool"
#define RWB_FN_begin_command_buffer "vkBeginCommandBuffer"
#define RWB_FN_end_command_buffer "vkEndCommandBuffer"
#define RWB_FN_cmd_begin_render_pass "vkCmdBeginRenderPass"
#define RWB_FN_cmd_end_render_pass "vkCmdEndRenderPass"
#define RWB_FN_queue_submit "vkQueueSubmit"
#define RWB_FN_create_fence "vkCreateFence"
#define RWB_FN_destroy_fence "vkDestroyFence"
#define RWB_FN_wait_for_fences "vkWaitForFences"
#define RWB_FN_reset_fences "vkResetFences"
#define RWB_FN_device_wait_idle "vkDeviceWaitIdle"
#define RWB_FN_destroy_device "vkDestroyDevice"

#define RWB_LOAD(dev, name) \
	do { \
		bridge->name = (PFN_##name)gdpa(bridge->device, RWB_FN_##name); \
		if (bridge->name == NULL) { \
			rwb_fail(why, why_size, "the ICD has no " RWB_FN_##name); \
			goto failed; \
		} \
	} while (0)

struct rwb *
rwb_open(id<MTLDevice> device, char *why, size_t why_size)
{
	typedef PFN_vkVoidFunction (*icd_gipa_t)(VkInstance, const char *);
	icd_gipa_t gipa;
	PFN_vkCreateInstance create_instance;
	PFN_vkEnumeratePhysicalDevices enumerate;
	PFN_vkGetPhysicalDeviceQueueFamilyProperties families;
	PFN_vkCreateDevice create_device;
	PFN_vkGetDeviceProcAddr gdpa;
	struct rwb *bridge;
	VkApplicationInfo application;
	VkInstanceCreateInfo instance_info;
	uint32_t count = 1, family_count = 0, chosen = UINT32_MAX;
	VkDeviceCreateInfo device_info;
	VkDeviceQueueCreateInfo queue_info;
	float priority = 1.0f;
	VkAttachmentDescription attachment;
	VkAttachmentReference colour;
	VkSubpassDescription subpass;
	VkRenderPassCreateInfo pass_info;
	VkCommandPoolCreateInfo pool_info;
	VkCommandBufferAllocateInfo command_info;
	VkFenceCreateInfo fence_info;

	if (why && why_size)
		why[0] = '\0';
	bridge = calloc(1, sizeof(*bridge));
	if (bridge == NULL) {
		rwb_fail(why, why_size, "no memory for the bridge");
		return NULL;
	}

	bridge->icd = dlopen(RWB_ICD, RTLD_NOW | RTLD_LOCAL);
	if (bridge->icd == NULL) {
		rwb_fail(why, why_size, dlerror() ?: "the ICD did not load");
		goto failed;
	}
	gipa = (icd_gipa_t)dlsym(bridge->icd, "vk_icdGetInstanceProcAddr");
	if (gipa == NULL) {
		rwb_fail(why, why_size, "the ICD has no vk_icdGetInstanceProcAddr");
		goto failed;
	}
	/* Only vkCreateInstance is a NULL-instance (global) lookup in this ICD;
	 * the rest are instance commands and come back NULL when asked with a
	 * NULL instance. Resolve them after the instance exists, as
	 * vk_triangle_app.m:425-436 and turnip_test.c:270 do. */
	create_instance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
	if (create_instance == NULL) {
		rwb_fail(why, why_size, "the ICD gave no vkCreateInstance");
		goto failed;
	}

	memset(&application, 0, sizeof(application));
	application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	application.pApplicationName = "redwood-vk-bridge";
	application.apiVersion = VK_API_VERSION_1_0;
	memset(&instance_info, 0, sizeof(instance_info));
	instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	instance_info.pApplicationInfo = &application;
	if (create_instance(&instance_info, NULL, &bridge->instance) != VK_SUCCESS) {
		rwb_fail(why, why_size, "vkCreateInstance failed");
		goto failed;
	}
	bridge->destroy_instance = (PFN_vkDestroyInstance)
	    gipa(bridge->instance, "vkDestroyInstance");
	enumerate = (PFN_vkEnumeratePhysicalDevices)
	    gipa(bridge->instance, "vkEnumeratePhysicalDevices");
	families = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)
	    gipa(bridge->instance, "vkGetPhysicalDeviceQueueFamilyProperties");
	create_device = (PFN_vkCreateDevice)
	    gipa(bridge->instance, "vkCreateDevice");
	if (enumerate == NULL || families == NULL || create_device == NULL) {
		rwb_fail(why, why_size, "the ICD did not give the instance entry points");
		goto failed;
	}

	if (enumerate(bridge->instance, &count, &bridge->physical) != VK_SUCCESS || count < 1) {
		rwb_fail(why, why_size, "no physical device");
		goto failed;
	}
	families(bridge->physical, &family_count, NULL);
	{
		VkQueueFamilyProperties *list = calloc(family_count ?: 1, sizeof(*list));

		if (list == NULL) {
			rwb_fail(why, why_size, "no memory for the queue families");
			goto failed;
		}
		families(bridge->physical, &family_count, list);
		for (uint32_t i = 0; i < family_count; i++) {
			if (list[i].queueCount > 0 &&
			    (list[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
				chosen = i;
				break;
			}
		}
		free(list);
	}
	if (chosen == UINT32_MAX) {
		rwb_fail(why, why_size, "no graphics queue family");
		goto failed;
	}
	bridge->family = chosen;

	memset(&queue_info, 0, sizeof(queue_info));
	queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	queue_info.queueFamilyIndex = chosen;
	queue_info.queueCount = 1;
	queue_info.pQueuePriorities = &priority;
	memset(&device_info, 0, sizeof(device_info));
	device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	device_info.queueCreateInfoCount = 1;
	device_info.pQueueCreateInfos = &queue_info;
	if (create_device(bridge->physical, &device_info, NULL, &bridge->device) != VK_SUCCESS) {
		rwb_fail(why, why_size, "vkCreateDevice failed");
		goto failed;
	}
	gdpa = (PFN_vkGetDeviceProcAddr)gipa(bridge->instance, "vkGetDeviceProcAddr");
	if (gdpa == NULL) {
		rwb_fail(why, why_size, "the ICD has no vkGetDeviceProcAddr");
		goto failed;
	}

	RWB_LOAD(device, get_device_queue);
	RWB_LOAD(device, create_image);
	RWB_LOAD(device, destroy_image);
	RWB_LOAD(device, image_memory_requirements);
	RWB_LOAD(device, allocate_memory);
	RWB_LOAD(device, free_memory);
	RWB_LOAD(device, bind_image_memory);
	RWB_LOAD(device, create_image_view);
	RWB_LOAD(device, destroy_image_view);
	RWB_LOAD(device, create_render_pass);
	RWB_LOAD(device, destroy_render_pass);
	RWB_LOAD(device, create_framebuffer);
	RWB_LOAD(device, destroy_framebuffer);
	RWB_LOAD(device, create_command_pool);
	RWB_LOAD(device, destroy_command_pool);
	RWB_LOAD(device, allocate_command_buffers);
	RWB_LOAD(device, reset_command_pool);
	RWB_LOAD(device, begin_command_buffer);
	RWB_LOAD(device, end_command_buffer);
	RWB_LOAD(device, cmd_begin_render_pass);
	RWB_LOAD(device, cmd_end_render_pass);
	RWB_LOAD(device, queue_submit);
	RWB_LOAD(device, create_fence);
	RWB_LOAD(device, destroy_fence);
	RWB_LOAD(device, wait_for_fences);
	RWB_LOAD(device, reset_fences);
	RWB_LOAD(device, device_wait_idle);
	RWB_LOAD(device, destroy_device);

	bridge->get_device_queue(bridge->device, bridge->family, 0, &bridge->queue);

	memset(&attachment, 0, sizeof(attachment));
	attachment.format = VK_FORMAT_B8G8R8A8_UNORM;
	attachment.samples = VK_SAMPLE_COUNT_1_BIT;
	attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	memset(&colour, 0, sizeof(colour));
	colour.attachment = 0;
	colour.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	memset(&subpass, 0, sizeof(subpass));
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &colour;
	memset(&pass_info, 0, sizeof(pass_info));
	pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	pass_info.attachmentCount = 1;
	pass_info.pAttachments = &attachment;
	pass_info.subpassCount = 1;
	pass_info.pSubpasses = &subpass;
	if (bridge->create_render_pass(bridge->device, &pass_info, NULL, &bridge->pass) !=
	    VK_SUCCESS) {
		rwb_fail(why, why_size, "vkCreateRenderPass failed");
		goto failed;
	}

	memset(&pool_info, 0, sizeof(pool_info));
	pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pool_info.queueFamilyIndex = bridge->family;
	if (bridge->create_command_pool(bridge->device, &pool_info, NULL, &bridge->pool) !=
	    VK_SUCCESS) {
		rwb_fail(why, why_size, "vkCreateCommandPool failed");
		goto failed;
	}
	memset(&command_info, 0, sizeof(command_info));
	command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	command_info.commandPool = bridge->pool;
	command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	command_info.commandBufferCount = 1;
	if (bridge->allocate_command_buffers(bridge->device, &command_info, &bridge->command) !=
	    VK_SUCCESS) {
		rwb_fail(why, why_size, "vkAllocateCommandBuffers failed");
		goto failed;
	}
	memset(&fence_info, 0, sizeof(fence_info));
	fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
	if (bridge->create_fence(bridge->device, &fence_info, NULL, &bridge->fence) !=
	    VK_SUCCESS) {
		rwb_fail(why, why_size, "vkCreateFence failed");
		goto failed;
	}

	fprintf(stderr, "vkbridge: bridge open, device %s\n",
	    class_getName(object_getClass(device)));
	return bridge;

failed:
	rwb_close(bridge);
	return NULL;
}

/*
 * The kernel's answer about a surface (kRedwoodSurface 0x7e, struct
 * redwood_surface in RedwoodGPU.cpp). Read-only describe: handle = 0xffffffff
 * asks for the numbers and allocates nothing.
 */
struct rwb_kernel_surface {
	uint32_t surface;
	uint32_t handle;
	uint64_t size;
	uint32_t offset, rowBytes;
	uint32_t width, height;
	uint32_t pixelFormat, planes;
};

#define RWB_KERNEL_DESCRIBE 0xffffffffu
#define RWB_REQUEST_SURFACE 0x7eu

static void
rwb_describe_surface(uint32_t sid, IOSurfaceRef surface)
{
	io_service_t service = IOServiceGetMatchingService(0,
	    IOServiceMatching("RedwoodGPU"));
	struct rwb_kernel_surface question, answer;
	io_connect_t connection = 0;
	size_t size = sizeof(answer);
	kern_return_t result;

	if (service == IO_OBJECT_NULL) {
		fprintf(stderr, "vkbridge: describe: no RedwoodGPU service\n");
		return;
	}
	if (IOServiceOpen(service, mach_task_self(), 0, &connection) != KERN_SUCCESS) {
		fprintf(stderr, "vkbridge: describe: RedwoodGPU refused a user client\n");
		IOObjectRelease(service);
		return;
	}
	memset(&question, 0, sizeof(question));
	question.surface = sid;
	question.handle = RWB_KERNEL_DESCRIBE;
	memset(&answer, 0, sizeof(answer));
	result = IOConnectCallStructMethod(connection, RWB_REQUEST_SURFACE, &question,
	    sizeof(question), &answer, &size);
	fprintf(stderr, "vkbridge: describe sid %u: kr %#x, size %llu offset %#x "
	    "rowBytes %u, %ux%u fmt %#x planes %u\n",
	    sid, result, (unsigned long long)answer.size, answer.offset,
	    answer.rowBytes, answer.width, answer.height, answer.pixelFormat,
	    answer.planes);
	fprintf(stderr, "vkbridge: iosurface sid %u: %zux%zu, rowBytes %zu, alloc %zu, "
	    "base %p\n", sid, IOSurfaceGetWidth(surface), IOSurfaceGetHeight(surface),
	    IOSurfaceGetBytesPerRow(surface), IOSurfaceGetAllocSize(surface),
	    IOSurfaceGetBaseAddress(surface));
	IOServiceClose(connection);
	IOObjectRelease(service);
}

static void
rwb_entry_reset(struct rwb *bridge, struct rwb_entry *entry)
{
	if (entry->framebuffer && bridge->destroy_framebuffer)
		bridge->destroy_framebuffer(bridge->device, entry->framebuffer, NULL);
	if (entry->view && bridge->destroy_image_view)
		bridge->destroy_image_view(bridge->device, entry->view, NULL);
	if (entry->image && bridge->destroy_image)
		bridge->destroy_image(bridge->device, entry->image, NULL);
	if (entry->memory && bridge->free_memory)
		bridge->free_memory(bridge->device, entry->memory, NULL);
	memset(entry, 0, sizeof(*entry));
}

static struct rwb_entry *
rwb_entry(struct rwb *bridge, const struct rw_wsi_image *image, char *why,
    size_t why_size)
{
	struct rwb_entry *entry;
	VkExternalMemoryImageCreateInfo external;
	VkImageCreateInfo create;
	VkMemoryRequirements requirements;
	VkImportMemoryFdInfoKHR import;
	VkMemoryAllocateInfo allocate;
	VkImageViewCreateInfo view;
	VkFramebufferCreateInfo framebuffer;
	uint32_t type = UINT32_MAX;

	for (unsigned i = 0; i < bridge->entries_used; i++)
		if (bridge->entries[i].sid == image->sid)
			return &bridge->entries[i];
	if (bridge->entries_used == RWB_ENTRIES) {
		rwb_fail(why, why_size, "more drawable surfaces than the bridge caches");
		return NULL;
	}
	entry = &bridge->entries[bridge->entries_used];

	memset(&external, 0, sizeof(external));
	external.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
	external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
	memset(&create, 0, sizeof(create));
	create.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	create.pNext = &external;
	create.imageType = VK_IMAGE_TYPE_2D;
	create.format = VK_FORMAT_B8G8R8A8_UNORM;
	create.extent.width = image->width;
	create.extent.height = image->height;
	create.extent.depth = 1;
	create.mipLevels = 1;
	create.arrayLayers = 1;
	create.samples = VK_SAMPLE_COUNT_1_BIT;
	create.tiling = VK_IMAGE_TILING_LINEAR;
	create.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	create.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	create.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	if (bridge->create_image(bridge->device, &create, NULL, &entry->image) != VK_SUCCESS) {
		rwb_fail(why, why_size, "vkCreateImage over the drawable failed");
		rwb_entry_reset(bridge, entry);
		return NULL;
	}
	bridge->image_memory_requirements(bridge->device, entry->image, &requirements);
	fprintf(stderr, "vkbridge: image %ux%u requirements size %llu align %llu "
	    "bits %#x\n", image->width, image->height,
	    (unsigned long long)requirements.size,
	    (unsigned long long)requirements.alignment, requirements.memoryTypeBits);
	rwb_describe_surface(image->sid, image->surface);

	/*
	 * The app's rule (vk_triangle_app.m) prefers a non-host-visible type and
	 * then accepts any allowed one; every heap here is host-visible, so the
	 * second pass is the one that answers. Take the lowest allowed type.
	 */
	for (uint32_t i = 0; i < 32; i++) {
		if (requirements.memoryTypeBits & (1u << i)) {
			type = i;
			break;
		}
	}
	if (type == UINT32_MAX) {
		rwb_fail(why, why_size, "the drawable's image allows no memory type");
		rwb_entry_reset(bridge, entry);
		return NULL;
	}
	memset(&import, 0, sizeof(import));
	import.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
	import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
	import.fd = (int)image->sid;
	memset(&allocate, 0, sizeof(allocate));
	allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
	allocate.pNext = &import;
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = type;
	if (bridge->allocate_memory(bridge->device, &allocate, NULL, &entry->memory) !=
	    VK_SUCCESS) {
		rwb_fail(why, why_size, "vkAllocateMemory importing the surface failed");
		rwb_entry_reset(bridge, entry);
		return NULL;
	}
	if (bridge->bind_image_memory(bridge->device, entry->image, entry->memory, 0) !=
	    VK_SUCCESS) {
		rwb_fail(why, why_size, "vkBindImageMemory failed");
		rwb_entry_reset(bridge, entry);
		return NULL;
	}

	memset(&view, 0, sizeof(view));
	view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	view.image = entry->image;
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.format = VK_FORMAT_B8G8R8A8_UNORM;
	view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	view.subresourceRange.levelCount = 1;
	view.subresourceRange.layerCount = 1;
	if (bridge->create_image_view(bridge->device, &view, NULL, &entry->view) !=
	    VK_SUCCESS) {
		rwb_fail(why, why_size, "vkCreateImageView failed");
		rwb_entry_reset(bridge, entry);
		return NULL;
	}
	memset(&framebuffer, 0, sizeof(framebuffer));
	framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	framebuffer.renderPass = bridge->pass;
	framebuffer.attachmentCount = 1;
	framebuffer.pAttachments = &entry->view;
	framebuffer.width = image->width;
	framebuffer.height = image->height;
	framebuffer.layers = 1;
	if (bridge->create_framebuffer(bridge->device, &framebuffer, NULL,
	    &entry->framebuffer) != VK_SUCCESS) {
		rwb_fail(why, why_size, "vkCreateFramebuffer failed");
		rwb_entry_reset(bridge, entry);
		return NULL;
	}

	entry->sid = image->sid;
	bridge->entries_used++;
	fprintf(stderr, "vkbridge: surface %u imported, %ux%u stride %zu\n",
	    image->sid, image->width, image->height, image->stride);
	/*
	 * A marker for the external-compositor probe (userland/src/lookup_test.c):
	 * another process asks IOSurfaceLookup for this id. /tmp is writable from
	 * SpringBoard and from the job list.
	 */
	{
		FILE *marker = fopen("/tmp/vkbridge.sid", "w");

		if (marker != NULL) {
			fprintf(marker, "%u\n", image->sid);
			fclose(marker);
		}
	}
	return entry;
}

int
rwb_present_clear(struct rwb *bridge, void *wsi, uint32_t bgra, char *why,
    size_t why_size)
{
	struct rw_wsi_image image;
	struct rwb_entry *entry;
	VkCommandBufferBeginInfo begin;
	VkClearValue clear;
	VkRenderPassBeginInfo pass;
	VkSubmitInfo submit;
	VkResult result;

	if (why && why_size)
		why[0] = '\0';
	if (rw_wsi_acquire((struct rw_wsi *)wsi, &image, why, why_size) != 0)
		return -1;
	entry = rwb_entry(bridge, &image, why, why_size);
	if (entry == NULL) {
		/* The drawable is already ours; give it back so CA can reuse it. */
		rw_wsi_present((struct rw_wsi *)wsi, &image, why, why_size);
		return -1;
	}
	bridge->reset_command_pool(bridge->device, bridge->pool, 0);
	memset(&begin, 0, sizeof(begin));
	begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	if (bridge->begin_command_buffer(bridge->command, &begin) != VK_SUCCESS) {
		rwb_fail(why, why_size, "vkBeginCommandBuffer failed");
		return -1;
	}
	memset(&clear, 0, sizeof(clear));
	clear.color.float32[0] = ((bgra >> 16) & 0xff) / 255.0f;   /* R */
	clear.color.float32[1] = ((bgra >> 8) & 0xff) / 255.0f;    /* G */
	clear.color.float32[2] = (bgra & 0xff) / 255.0f;           /* B */
	clear.color.float32[3] = ((bgra >> 24) & 0xff) / 255.0f;   /* A */
	memset(&pass, 0, sizeof(pass));
	pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
	pass.renderPass = bridge->pass;
	pass.framebuffer = entry->framebuffer;
	pass.renderArea.extent.width = image.width;
	pass.renderArea.extent.height = image.height;
	pass.clearValueCount = 1;
	pass.pClearValues = &clear;
	bridge->cmd_begin_render_pass(bridge->command, &pass, VK_SUBPASS_CONTENTS_INLINE);
	bridge->cmd_end_render_pass(bridge->command);
	if (bridge->end_command_buffer(bridge->command) != VK_SUCCESS) {
		rwb_fail(why, why_size, "vkEndCommandBuffer failed");
		return -1;
	}
	memset(&submit, 0, sizeof(submit));
	submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &bridge->command;
	result = bridge->queue_submit(bridge->queue, 1, &submit, bridge->fence);
	if (result != VK_SUCCESS) {
		rwb_fail(why, why_size, "vkQueueSubmit failed");
		return -1;
	}
	result = bridge->wait_for_fences(bridge->device, 1, &bridge->fence, VK_TRUE, 1000000000ull);
	if (result != VK_SUCCESS) {
		rwb_fail(why, why_size, "the clear did not finish in 1 s");
		return -1;
	}
	bridge->reset_fences(bridge->device, 1, &bridge->fence);
	if (rw_wsi_present((struct rw_wsi *)wsi, &image, why, why_size) != 0)
		return -1;
	return 0;
}

void
rwb_close(struct rwb *bridge)
{
	if (bridge == NULL)
		return;
	if (bridge->device != NULL && bridge->device_wait_idle != NULL)
		bridge->device_wait_idle(bridge->device);
	for (unsigned i = 0; i < bridge->entries_used; i++)
		rwb_entry_reset(bridge, &bridge->entries[i]);
	if (bridge->fence && bridge->destroy_fence)
		bridge->destroy_fence(bridge->device, bridge->fence, NULL);
	if (bridge->pool && bridge->destroy_command_pool)
		bridge->destroy_command_pool(bridge->device, bridge->pool, NULL);
	if (bridge->pass && bridge->destroy_render_pass)
		bridge->destroy_render_pass(bridge->device, bridge->pass, NULL);
	if (bridge->device && bridge->destroy_device)
		bridge->destroy_device(bridge->device, NULL);
	if (bridge->instance && bridge->destroy_instance)
		bridge->destroy_instance(bridge->instance, NULL);
	if (bridge->icd)
		dlclose(bridge->icd);
	free(bridge);
}
