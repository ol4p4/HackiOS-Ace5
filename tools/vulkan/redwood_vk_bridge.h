/*
 * redwood_vk_bridge: the Vulkan half of the first CA bridge probe.
 *
 * The window-system half is wsi_redwood (acquire a CAMetalLayer drawable, bump
 * its seed, present it). This half owns the VkDevice and turns the drawable's
 * IOSurface id into a VkImage, then renders into it. It deliberately draws only
 * a clear: the probe answers "can a process inside CoreAnimation's own window
 * server get a drawable, import it into Turnip and put pixels on the panel",
 * not "can we composite a desktop yet".
 *
 * The import sequence is the one step 11/13 measured (linear BGRA, DMA_BUF fd =
 * IOSurface id, patched Turnip + redwood-drm shim). Do not "improve" it here:
 * a tiled copy and an RGBA declaration are both measured wrong on this port.
 */
#ifndef REDWOOD_VK_BRIDGE_H
#define REDWOOD_VK_BRIDGE_H

#import <Metal/Metal.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct rwb;

/* 0, or -1 with a sentence in `why`. `device` is only asked for its memory
 * properties; the drawable and present stay with the WSI. */
struct rwb *rwb_open(id<MTLDevice> device, char *why, size_t why_size);

/* Acquire one drawable from `wsi`, import it, clear it to `bgra`, submit, wait,
 * then present through the WSI. 0, or -1 with a sentence in `why`. */
int rwb_present_clear(struct rwb *bridge, void *wsi, uint32_t bgra, char *why,
    size_t why_size);

void rwb_close(struct rwb *bridge);

#ifdef __cplusplus
}
#endif

#endif /* REDWOOD_VK_BRIDGE_H */
