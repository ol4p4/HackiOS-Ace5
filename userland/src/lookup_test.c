/*
 * Can a process that is not SpringBoard look up a CoreAnimation surface by
 * id? The external-compositor idea stands or falls on this: SpringBoard's layer
 * surfaces are made by CA in SpringBoard, and the app/compositor must take a
 * reference to them through IOSurfaceLookup before Turnip can import the id.
 *
 * The id comes from /tmp/vkbridge.sid, written by the vkbridge overlay when it
 * imports a new drawable (rwb_entry). Run from the job list, after the overlay
 * has started.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurfaceRef.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
main(void)
{
	FILE *file;
	unsigned sid = 0;
	IOSurfaceRef surface;

	file = fopen("/tmp/vkbridge.sid", "r");
	if (file == NULL) {
		printf("lookup: no /tmp/vkbridge.sid (%s)\n", strerror(errno));
		return 1;
	}
	if (fscanf(file, "%u", &sid) != 1) {
		fclose(file);
		printf("lookup: /tmp/vkbridge.sid holds no number\n");
		return 1;
	}
	fclose(file);
	printf("lookup: asking for sid %u\n", sid);
	surface = IOSurfaceLookup((IOSurfaceID)sid);
	if (surface == NULL) {
		printf("lookup: sid %u -> NULL (%s)\n", sid, strerror(errno));
		return 1;
	}
	printf("lookup: sid %u -> %zux%zu, rowBytes %zu, alloc %zu, fmt %#x, "
	    "base %p\n", sid, IOSurfaceGetWidth(surface), IOSurfaceGetHeight(surface),
	    IOSurfaceGetBytesPerRow(surface), IOSurfaceGetAllocSize(surface),
	    (unsigned)IOSurfaceGetPixelFormat(surface), IOSurfaceGetBaseAddress(surface));
	CFRelease(surface);
	return 0;
}
