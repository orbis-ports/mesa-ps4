/*
 * Copyright 2026 Mikołaj Mikołajczyk
 * SPDX-License-Identifier: MIT
 *
 * THE PS4's SCAN-OUT, BEHIND FOUR CALLS, so that sceVideoOut never appears in a file Mesa owns.
 *
 * WHY THIS EXISTS AT ALL. This console has no DRM, no dma-buf and no compositor, so none of Mesa's WSI platforms
 * describes it. What it has is sceVideoOut: a handle, N registered buffers of GARLIC memory, and a flip. That
 * whole sequence is already PROVEN on hardware - ps4/radv's scan-out probe renders 1080p, carries it into a
 * registered buffer and flips, and the television shows the fragment shader's own gradient - so this file is a
 * transcription of a measured sequence rather than a new idea.
 *
 * WHY IT HANGS OFF wsi_common_headless.c. VK_EXT_headless_surface already exists, RADV already advertises it
 * (radv_instance.c:241), and its swapchain skeleton already does acquire/release/present bookkeeping with a
 * CPU-mapped image. Adding a new surface extension instead would mean touching Mesa's XML and its generated
 * entry points - a large change to get the same VkSwapchainKHR the engine actually asks for.
 *
 * WHAT IT COSTS TODAY: one CPU copy per frame, image -> registered buffer, because the swapchain image lives in
 * the driver's WB_ONION arena and no oracle here says video-out accepts ONION. The zero-copy shape is to
 * allocate the swapchain images IN the registered GARLIC buffers, which needs
 * ac_drm_create_bo_from_user_mem to accept memory outside the arena. Deliberately second: first a picture the
 * engine put there, then the fast path.
 */

#ifndef WSI_ORBIS_H
#define WSI_ORBIS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct wsi_orbis_scanout;

/* Opens video-out and registers `count` linear GARLIC buffers of `width` x `height`. Returns NULL and logs the
 * call that refused; a swapchain that cannot be displayed must fail loudly at creation rather than present into
 * nothing. */
/* `addrs` are the swapchain's own images, registered directly so the present is a flip and nothing
 * is copied; NULL asks this code to allocate GARLIC buffers of its own, which is the fallback when
 * video-out refuses them. `pitch_px` is the image's row pitch in pixels, 0 meaning "the width". */
struct wsi_orbis_scanout *wsi_orbis_scanout_create(uint32_t width, uint32_t height, uint32_t count,
                                                   void *const *addrs, uint32_t pitch_px);

/* Copies one image into its registered buffer and flips. `src_pitch` is the image's own row pitch, which WSI
 * chooses and which need not equal width*4 - copying row by row rather than in one memcpy is what makes that
 * difference harmless instead of a diagonal picture. */
/* gpu_seq is the submission this frame's rendering ended at, taken with ac_orbis_gpu_seq_now() when the
 * frame was deferred. Zero means "whatever is newest", which is only correct when the caller presents
 * the frame it has just submitted. */
bool wsi_orbis_scanout_present(struct wsi_orbis_scanout *so, uint32_t index, const void *src,
                               uint32_t src_pitch, uint64_t gpu_seq);

/* Declares how many images the caller holds while the display works - one for the application, plus
 * one more if presents are deferred - and returns the smallest image count that can work with that.
 * A swapchain smaller than the returned value must not register its own images. */
uint32_t wsi_orbis_scanout_reserve(struct wsi_orbis_scanout *so, uint32_t reserve);

/* Flips taken by the display but not yet shown. With the swapchain's own images registered, those
 * buffers and the one on screen are all still the display's and must not be handed back. */
uint32_t wsi_orbis_flips_pending(struct wsi_orbis_scanout *so);

void wsi_orbis_scanout_destroy(struct wsi_orbis_scanout *so);

#ifdef __cplusplus
}
#endif

#endif /* WSI_ORBIS_H */
