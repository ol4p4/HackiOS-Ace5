/*
 * Can a compositor process read the layer list CA submitted? The framebuffer
 * driver now keeps a snapshot of every swap it takes (kRedwoodSurface's
 * neighbour: selector 83 on IOMobileFramebufferUserClient), and this tool
 * dumps it ten times, a second apart, from the job list — the compositor will
 * do the same, and turn the surfaces into a picture.
 *
 * The struct is field for field the kernel's (fb_swap_snapshot in
 * iokit/Kernel/MiniIOMobileFramebuffer.cpp).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct fb_swap_layer {
	uint32_t sid, enabled;
	int32_t src[4], dst[4];         /* in the surface: x,y,w,h; on the display: x,y,w,h */
};

struct fb_swap_snapshot {
	uint32_t swap_id;
	uint32_t seq;
	uint32_t layers_set, layers_cleared;
	struct fb_swap_layer layer[4];
};

int
main(void)
{
	io_service_t service = IOServiceGetMatchingService(0,
	    IOServiceMatching("IOMobileFramebuffer"));
	io_connect_t connection;

	if (service == 0) {
		printf("swaplist: no IOMobileFramebuffer service\n");
		return 1;
	}
	if (IOServiceOpen(service, mach_task_self(), 0, &connection) != KERN_SUCCESS) {
		printf("swaplist: IOServiceOpen failed\n");
		IOObjectRelease(service);
		return 1;
	}
	for (int i = 0; i < 10; i++) {
		struct fb_swap_snapshot snap;
		size_t size = sizeof(snap);
		kern_return_t result;

		memset(&snap, 0, sizeof(snap));
		result = IOConnectCallStructMethod(connection, 83, NULL, 0, &snap, &size);
		if (result != KERN_SUCCESS) {
			printf("swaplist: selector 83 -> %#x\n", result);
			break;
		}
		printf("swaplist: seq %u swap %u set %#x cleared %#x\n",
		    snap.seq, snap.swap_id, snap.layers_set, snap.layers_cleared);
		for (int layer = 0; layer < 4; layer++) {
			const struct fb_swap_layer *entry = &snap.layer[layer];

			printf("swaplist:   layer %d: sid %u en %u src %d,%d %dx%d dst %d,%d %dx%d\n",
			    layer, entry->sid, entry->enabled,
			    entry->src[0], entry->src[1], entry->src[2], entry->src[3],
			    entry->dst[0], entry->dst[1], entry->dst[2], entry->dst[3]);
		}
		sleep(1);
	}
	IOServiceClose(connection);
	IOObjectRelease(service);
	return 0;
}
