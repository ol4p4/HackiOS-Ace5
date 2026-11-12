/*
 * wsi_redwood.m: the drawable half of a swapchain over CAMetalLayer.
 * See wsi_redwood.h for the contract and the two measured rules it carries.
 */
#import "wsi_redwood.h"

#import <Foundation/Foundation.h>
#import <objc/runtime.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* _CAMetalDrawablePrivate, the two fields this file validates before it
 * believes the third (src/triangle_app.m carries the encoding and the
 * instructions). A layout that moved makes the measurement absent, not wrong. */
#define RW_WSI_PRIV_SURFACE 0x40
#define RW_WSI_PRIV_TEXTURE 0x48
#define RW_WSI_PRIV_DEVICE  0x50

struct rw_wsi {
	CAMetalLayer *layer;
	id<MTLDevice> device;
	id<MTLCommandQueue> queue;
	unsigned presents;
};

/* Present waits for the command buffer by default; 0 is the pacing experiment
 * (see wsi_redwood.h). */
static int rw_wsi_wait_present = 1;

void
rw_wsi_wait_for_present(int enable)
{
	rw_wsi_wait_present = enable != 0;
}

static void
rw_wsi_why(char *why, size_t size, const char *format, ...)
{
	va_list arguments;

	if (why == NULL || size == 0)
		return;
	va_start(arguments, format);
	vsnprintf(why, size, format, arguments);
	va_end(arguments);
}

/*
 * The drawable's own IOSurface, through CAMetalDrawable's one ivar and
 * validated before it is believed: the structure is trusted only after the
 * texture and the device it holds turn out to be the two this layer already
 * has. The driver's texture answers nil to -iosurface (redwoodmetal.m), so
 * there is no way to ask Metal.
 */
static IOSurfaceRef
rw_wsi_surface_of_drawable(id drawable, id texture, id device)
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
		if (*(id *)(private + RW_WSI_PRIV_TEXTURE) != texture)
			continue;
		if (*(id *)(private + RW_WSI_PRIV_DEVICE) != device)
			continue;
		surface = *(IOSurfaceRef *)(private + RW_WSI_PRIV_SURFACE);
	}
	free(ivars);
	return surface;
}

int
rw_wsi_open(struct rw_wsi **out, CAMetalLayer *layer, id<MTLDevice> device,
    id<MTLCommandQueue> queue, char *why, size_t why_size)
{
	struct rw_wsi *wsi;

	if (out == NULL || layer == nil || device == nil || queue == nil) {
		rw_wsi_why(why, why_size, "layer, device and queue are all required");
		return -1;
	}
	wsi = calloc(1, sizeof(*wsi));
	if (wsi == NULL) {
		rw_wsi_why(why, why_size, "no memory for the WSI layer");
		return -1;
	}
	wsi->layer = [layer retain];
	wsi->device = [device retain];
	wsi->queue = [queue retain];
	*out = wsi;
	return 0;
}

int
rw_wsi_acquire(struct rw_wsi *wsi, struct rw_wsi_image *image, char *why,
    size_t why_size)
{
	id<CAMetalDrawable> drawable;
	id<MTLTexture> texture;
	IOSurfaceRef surface;

	if (wsi == NULL || image == NULL) {
		rw_wsi_why(why, why_size, "no layer");
		return -1;
	}
	memset(image, 0, sizeof(*image));
	drawable = [wsi->layer nextDrawable];
	if (drawable == nil) {
		rw_wsi_why(why, why_size, "nextDrawable gave nil (a pool of %lu still in "
		    "flight?)", (unsigned long)[wsi->layer maximumDrawableCount]);
		return -1;
	}
	texture = [drawable texture];
	if (texture == nil) {
		rw_wsi_why(why, why_size, "the drawable's texture is nil");
		return -1;
	}
	surface = rw_wsi_surface_of_drawable(drawable, texture, wsi->device);
	if (surface == NULL) {
		rw_wsi_why(why, why_size, "the drawable's IOSurface could not be reached "
		    "(the private layout moved?)");
		return -1;
	}
	if (IOSurfaceGetBaseAddress(surface) == NULL ||
	    IOSurfaceGetWidth(surface) != [texture width] ||
	    IOSurfaceGetHeight(surface) != [texture height] ||
	    IOSurfaceGetBytesPerRow(surface) < (size_t)[texture width] * 4) {
		rw_wsi_why(why, why_size, "the surface does not describe the drawable "
		    "(%ux%u texture, %zux%zu surface)", (unsigned)[texture width],
		    (unsigned)[texture height], IOSurfaceGetWidth(surface),
		    IOSurfaceGetHeight(surface));
		return -1;
	}
	image->drawable = drawable;
	image->surface = surface;
	image->sid = (uint32_t)IOSurfaceGetID(surface);
	image->width = (uint32_t)[texture width];
	image->height = (uint32_t)[texture height];
	image->stride = IOSurfaceGetBytesPerRow(surface);
	image->pixel_format = (uint32_t)IOSurfaceGetPixelFormat(surface);
	return 0;
}

int
rw_wsi_present(struct rw_wsi *wsi, struct rw_wsi_image *image, char *why,
    size_t why_size)
{
	id<MTLCommandBuffer> commit;

	if (wsi == NULL || image == NULL || image->drawable == nil ||
	    image->surface == NULL) {
		rw_wsi_why(why, why_size, "nothing acquired to present");
		return -1;
	}
	/* THE SEED RULE (turnip_test step 12): the kernel invalidates this
	 * processor's cached lines of the surface only when its seed moves, so
	 * whoever wrote must say so or readers see the previous frame. */
	if (IOSurfaceLock(image->surface, 0, NULL) == 0)
		IOSurfaceUnlock(image->surface, 0, NULL);

	/* The present that reaches the panel: Apple's presentDrawable: on a
	 * command buffer, delivered by -commit. The drawable's own -present did
	 * not (measured 2026-11-09: the window stayed black while the surface
	 * held the right pixels). The buffer carries no encoder, so the driver's
	 * -commit has nothing to draw. */
	commit = [wsi->queue commandBuffer];
	if (commit == nil) {
		rw_wsi_why(why, why_size, "the queue gave no command buffer");
		return -1;
	}
	[commit presentDrawable:image->drawable];
	[commit commit];
	if (rw_wsi_wait_present)
		[commit waitUntilCompleted];
	wsi->presents++;
	image->drawable = nil;
	image->surface = NULL;
	return 0;
}

void
rw_wsi_close(struct rw_wsi *wsi)
{
	if (wsi == NULL)
		return;
	[wsi->layer release];
	[wsi->device release];
	[wsi->queue release];
	free(wsi);
}
