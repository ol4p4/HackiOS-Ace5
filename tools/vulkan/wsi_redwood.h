/*
 * wsi_redwood: the port's window-system half of a swapchain, over CAMetalLayer.
 *
 * WHY IT IS A FILE OF ITS OWN. Step 13 (tools/vulkan/vk_triangle_app.m) put a
 * Vulkan frame on the panel by doing four things around the draw: take a
 * drawable from the layer, reach the drawable's IOSurface, announce the write
 * (the seed rule), and present the drawable. Those four are the WSI half of a
 * swapchain and they belong to the window system, not to any one client: the
 * next client (a WSI proper in the ICD, a CA bridge, another application) must
 * get the same four, with the same rules. The Vulkan half -- importing the
 * surface by its id as a VkImage -- stays with the client, because it needs
 * that client's VkDevice.
 *
 * THE CONTRACT, in the order a client uses it:
 *
 *   rw_wsi_open     the layer, its device and its queue are bound; nothing is
 *                   allocated until the first acquire.
 *   rw_wsi_acquire  one drawable from CoreAnimation: -nextDrawable, its
 *                   IOSurface (through CAMetalDrawable's private structure,
 *                   validated against the texture and the device), its size,
 *                   stride and id. The surface belongs to this client until
 *                   present; nothing else writes it.
 *   rw_wsi_present  THE SEED BUMP (IOSurfaceLock/Unlock, turnip_test step 12's
 *                   rule -- the kernel invalidates this processor's cached
 *                   lines of a surface only when its seed moves), then the
 *                   present through Apple's -[MTLCommandBuffer
 *                   presentDrawable:] + -commit, which is the path that
 *                   reaches the panel (the drawable's own -present did not;
 *                   measured 2026-11-09).
 *
 * WHAT IT DELIBERATELY DOES NOT DO: pick a size, a format or a pixel layout.
 * CA gives the drawable's size and this layer reports it; the drawable's
 * surface is BGRA (0x42475241) and the client must declare that when it
 * imports. A layer that guessed would hide the exact mistake step 11 cost a
 * boot to find.
 */
#ifndef WSI_REDWOOD_H
#define WSI_REDWOOD_H

#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurfaceRef.h>

#include <stddef.h>
#include <stdint.h>

struct rw_wsi;

struct rw_wsi_image {
	id<CAMetalDrawable> drawable;   /* owned by CA; valid until present */
	IOSurfaceRef surface;           /* the drawable's own surface, borrowed */
	uint32_t sid;                   /* its IOSurface id: the client's "fd" */
	uint32_t width, height;         /* the drawable's, read back not assumed */
	size_t stride;                  /* IOSurfaceGetBytesPerRow */
	uint32_t pixel_format;          /* IOSurfaceGetPixelFormat ('BGRA') */
};

/* 0, or -1 with a sentence in `why`. */
int rw_wsi_open(struct rw_wsi **wsi, CAMetalLayer *layer, id<MTLDevice> device,
    id<MTLCommandQueue> queue, char *why, size_t why_size);
int rw_wsi_acquire(struct rw_wsi *wsi, struct rw_wsi_image *image, char *why,
    size_t why_size);
int rw_wsi_present(struct rw_wsi *wsi, struct rw_wsi_image *image, char *why,
    size_t why_size);
void rw_wsi_close(struct rw_wsi *wsi);

#endif
