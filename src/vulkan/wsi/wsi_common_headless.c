/*
 * Copyright 2021 Red Hat, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/** VK_EXT_headless_surface */

#ifdef HAVE_ORBIS_PLATFORM
#include "wsi_orbis.h"
#define WSI_ORBIS_FORCE_CPU_IMAGES true
#else
#define WSI_ORBIS_FORCE_CPU_IMAGES false
#endif

#include "util/macros.h"
#include "util/timespec.h"
#include "vk_util.h"
#include "vk_instance.h"
#include "vk_physical_device.h"
#include "wsi_common_entrypoints.h"
#include "wsi_common_private.h"

#include "drm-uapi/drm_fourcc.h"

struct wsi_headless {
   struct wsi_interface base;
};

static VkResult
wsi_headless_surface_get_support(VkIcdSurfaceBase *surface,
                                 struct wsi_device *wsi_device,
                                 uint32_t queueFamilyIndex,
                                 VkBool32* pSupported)
{
   *pSupported = true;

   return VK_SUCCESS;
}

static const VkPresentModeKHR present_modes[] = {
   VK_PRESENT_MODE_MAILBOX_KHR,
   VK_PRESENT_MODE_FIFO_KHR,
};

static VkResult
wsi_headless_surface_get_capabilities(VkIcdSurfaceBase *surface,
                                      struct wsi_device *wsi_device,
                                      VkSurfaceCapabilities2KHR* caps)
{
   /* For true mailbox mode, we need at least 4 images:
    *  1) One to scan out from
    *  2) One to have queued for scan-out
    *  3) One to be currently held by the Wayland compositor
    *  4) One to render to
    */
   caps->surfaceCapabilities.minImageCount = 4;
   /* There is no real maximum */
   caps->surfaceCapabilities.maxImageCount = 0;

   caps->surfaceCapabilities.currentExtent = (VkExtent2D) { -1, -1 };
   caps->surfaceCapabilities.minImageExtent = (VkExtent2D) { 1, 1 };
   caps->surfaceCapabilities.maxImageExtent = (VkExtent2D) {
      wsi_device->maxImageDimension2D,
      wsi_device->maxImageDimension2D,
   };

   caps->surfaceCapabilities.supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
   caps->surfaceCapabilities.currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
   caps->surfaceCapabilities.maxImageArrayLayers = 1;

   caps->surfaceCapabilities.supportedCompositeAlpha =
      VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR |
      VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;

   VkImageUsageFlags image_usage = wsi_caps_get_image_usage();

   VK_FROM_HANDLE(vk_physical_device, pdevice, wsi_device->pdevice);
   if (pdevice->supported_extensions.EXT_attachment_feedback_loop_layout)
      image_usage |= VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;

   VkSwapchainFlagsSurfaceCapabilitiesEXT *surface_caps = vk_find_struct(caps, SWAPCHAIN_FLAGS_SURFACE_CAPABILITIES_EXT);
   if (surface_caps && pdevice->supported_extensions.EXT_multisampled_render_to_swapchain)
      surface_caps->swapchainSupportedFlags |= VK_SWAPCHAIN_CREATE_MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_BIT_EXT;

   VkImageUsageFlags2CreateInfoKHR *usage2 =
      vk_find_struct(caps->pNext, IMAGE_USAGE_FLAGS_2_CREATE_INFO_KHR);
   if (usage2) {
      usage2->usage = image_usage;
   } else {
      caps->surfaceCapabilities.supportedUsageFlags = image_usage;
   }

   return VK_SUCCESS;
}

static VkResult
wsi_headless_surface_get_capabilities2(VkIcdSurfaceBase *surface,
                                       struct wsi_device *wsi_device,
                                       const void *info_next,
                                       VkSurfaceCapabilities2KHR* caps)
{
   assert(caps->sType == VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR);

   const VkSurfacePresentModeKHR *present_mode =
      vk_find_struct_const(info_next, SURFACE_PRESENT_MODE_EXT);

   VkResult result =
      wsi_headless_surface_get_capabilities(surface, wsi_device,
                                      caps);

   vk_foreach_struct(ext, caps->pNext) {
      switch (ext->sType) {
      case VK_STRUCTURE_TYPE_SURFACE_PROTECTED_CAPABILITIES_KHR: {
         VkSurfaceProtectedCapabilitiesKHR *protected = (void *)ext;
         protected->supportsProtected = VK_FALSE;
         break;
      }
      case VK_STRUCTURE_TYPE_SURFACE_PRESENT_SCALING_CAPABILITIES_KHR: {
         /* Unsupported */
         VkSurfacePresentScalingCapabilitiesKHR *scaling = (void *)ext;
         scaling->supportedPresentScaling = 0;
         scaling->supportedPresentGravityX = 0;
         scaling->supportedPresentGravityY = 0;
         scaling->minScaledImageExtent = caps->surfaceCapabilities.minImageExtent;
         scaling->maxScaledImageExtent = caps->surfaceCapabilities.maxImageExtent;
         break;
      }
      case VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_COMPATIBILITY_KHR: {
         /* Unsupported */
         VkSurfacePresentModeCompatibilityKHR *compat = (void *)ext;
         if (compat->pPresentModes == NULL) {
            if (!present_mode) {
               wsi_common_vk_warn_once("Use of VkSurfacePresentModeCompatibilityKHR "
                                       "without a VkSurfacePresentModeKHR set. This is an "
                                       "application bug.\n");
            }
            compat->presentModeCount = 1;
         } else if (compat->presentModeCount) {
            assert(present_mode);
            compat->presentModeCount = 1;
            compat->pPresentModes[0] = present_mode->presentMode;
         }
         break;
      }

      case VK_STRUCTURE_TYPE_PRESENT_TIMING_SURFACE_CAPABILITIES_EXT: {
         VkPresentTimingSurfaceCapabilitiesEXT *wait = (void *)ext;

         wait->presentStageQueries = 0;
         wait->presentTimingSupported = VK_FALSE;
         wait->presentAtAbsoluteTimeSupported = VK_FALSE;
         wait->presentAtRelativeTimeSupported = VK_FALSE;
         break;
      }

      default:
         /* Ignored */
         break;
      }
   }

   return result;
}

/* ⚠ ONE LIST, BECAUSE TWO COPIES OF IT ALREADY DIVERGED AND THE PICTURE WENT WRONG.
 *
 * The Orbis restriction below was added to get_formats and not to get_formats2, and
 * vkGetPhysicalDeviceSurfaceFormats2KHR goes straight to the second one (wsi_common.c) - so anything
 * using the KHR_get_surface_capabilities2 entry point, which is dxvk and every recent engine, was
 * still offered B8G8R8A8 and presented with red and blue swapped. Returning the list from one place
 * is what stops that happening again, and it is why this is a table rather than two blocks of
 * vk_outarray_append_typed.
 *
 * ⚠ TWO FORMATS ON THIS CONSOLE, UNORM FIRST, AND THE ORDER IS THE WHOLE POINT.
 *
 * The display consumes sRGB-encoded bytes either way; what differs is who encodes them. An engine that writes
 * linear values wants the SRGB format so the hardware encodes; one that tonemaps to display values itself
 * wants UNORM so nothing touches them again. Both are legitimate, so both are offered.
 *
 * Offering SRGB alone was measured wrong on a television: an engine asking for a UNORM format and falling
 * back to availableFormats[0] was handed an SRGB image while writing values it had already gamma-encoded, and
 * the hardware encoded them a second time. Twice-encoded reads as blown highlights - the kind of "nearly
 * right" that looks like a game bug rather than a driver one.
 *
 * R8G8B8A8 rather than B8G8R8A8 in both encodings: the registered scan-out buffer is A8B8G8R8, which names
 * channels from the most significant byte down, so its low byte is R - R8G8B8A8 in Vulkan's little-endian
 * naming. Offering the B-first spelling would swap red and blue against that buffer.
 */
static const VkFormat *
wsi_headless_surface_formats(const struct wsi_device *wsi_device, uint32_t *count)
{
#ifdef HAVE_ORBIS_PLATFORM
   static const VkFormat orbis_formats[] = {
      VK_FORMAT_R8G8B8A8_UNORM,
      VK_FORMAT_R8G8B8A8_SRGB,
   };

   (void)wsi_device;
   *count = ARRAY_SIZE(orbis_formats);
   return orbis_formats;
#else
   static const VkFormat bgra_first[] = {
      VK_FORMAT_B8G8R8A8_UNORM,
      VK_FORMAT_R8G8B8A8_UNORM,
      VK_FORMAT_B8G8R8A8_SRGB,
      VK_FORMAT_R8G8B8A8_SRGB,
   };
   static const VkFormat rgba_first[] = {
      VK_FORMAT_R8G8B8A8_UNORM,
      VK_FORMAT_B8G8R8A8_UNORM,
      VK_FORMAT_R8G8B8A8_SRGB,
      VK_FORMAT_B8G8R8A8_SRGB,
   };

   *count = ARRAY_SIZE(rgba_first);
   return wsi_device->force_bgra8_unorm_first ? bgra_first : rgba_first;
#endif
}

static VkResult
wsi_headless_surface_get_formats(VkIcdSurfaceBase *icd_surface,
                                 struct wsi_device *wsi_device,
                                 uint32_t* pSurfaceFormatCount,
                                 VkSurfaceFormatKHR* pSurfaceFormats)
{
   VK_OUTARRAY_MAKE_TYPED(VkSurfaceFormatKHR, out, pSurfaceFormats, pSurfaceFormatCount);

   uint32_t n = 0;
   const VkFormat *const formats = wsi_headless_surface_formats(wsi_device, &n);

   for (uint32_t i = 0; i < n; i++) {
      vk_outarray_append_typed(VkSurfaceFormatKHR, &out, out_fmt) {
         out_fmt->format = formats[i];
         out_fmt->colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
      }
   }

   return vk_outarray_status(&out);
}

static VkResult
wsi_headless_surface_get_formats2(VkIcdSurfaceBase *icd_surface,
                                  struct wsi_device *wsi_device,
                                  const void *info_next,
                                  uint32_t* pSurfaceFormatCount,
                                  VkSurfaceFormat2KHR* pSurfaceFormats)
{
   VK_OUTARRAY_MAKE_TYPED(VkSurfaceFormat2KHR, out, pSurfaceFormats, pSurfaceFormatCount);

   uint32_t n = 0;
   const VkFormat *const formats = wsi_headless_surface_formats(wsi_device, &n);

   for (uint32_t i = 0; i < n; i++) {
      vk_outarray_append_typed(VkSurfaceFormat2KHR, &out, out_fmt) {
         out_fmt->surfaceFormat.format = formats[i];
         out_fmt->surfaceFormat.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
      }
   }

   return vk_outarray_status(&out);
}

static VkResult
wsi_headless_surface_get_present_modes(VkIcdSurfaceBase *surface,
                                       struct wsi_device *wsi_device,
                                       uint32_t* pPresentModeCount,
                                       VkPresentModeKHR* pPresentModes)
{
   if (pPresentModes == NULL) {
      *pPresentModeCount = ARRAY_SIZE(present_modes);
      return VK_SUCCESS;
   }

   *pPresentModeCount = MIN2(*pPresentModeCount, ARRAY_SIZE(present_modes));
   typed_memcpy(pPresentModes, present_modes, *pPresentModeCount);

   if (*pPresentModeCount < ARRAY_SIZE(present_modes))
      return VK_INCOMPLETE;
   else
      return VK_SUCCESS;
}

static VkResult
wsi_headless_surface_get_present_rectangles(VkIcdSurfaceBase *surface,
                                            struct wsi_device *wsi_device,
                                            uint32_t* pRectCount,
                                            VkRect2D* pRects)
{
   VK_OUTARRAY_MAKE_TYPED(VkRect2D, out, pRects, pRectCount);

   vk_outarray_append_typed(VkRect2D, &out, rect) {
      /* We don't know a size so just return the usual "I don't know." */
      *rect = (VkRect2D) {
         .offset = { 0, 0 },
         .extent = { UINT32_MAX, UINT32_MAX },
      };
   }

   return vk_outarray_status(&out);
}

struct wsi_headless_image {
   struct wsi_image base;

   /* whether the host side ownership is taken by the app or the display */
   bool busy_on_host;

   /* whether the image may still be worked on by the device
    *
    * The headless display does not involve a presentation queue to wait for
    * the gpu out-fence. To let the app acquire the most likely idle image, we
    * use a second boolean to steer the app to acquire all the swapchain images
    * in a loop.
    */
   bool busy_on_device;
};

struct wsi_headless_swapchain {
   struct wsi_swapchain base;
#ifdef HAVE_ORBIS_PLATFORM
   /* ⚠ THIS PLATFORM PRESENTS FOR REAL. Everything else here is unchanged headless bookkeeping; the console
    * additionally owns registered scan-out buffers and a video-out handle, and its queue_present copies the image
    * into one and flips. See wsi_orbis.h for why the display path lives behind four calls.
    */
   struct wsi_orbis_scanout *scanout;

   /* ⚠ THE FRAME WHOSE COPY AND FLIP HAVE NOT HAPPENED YET, or -1.
    *
    * MEASURED: the present's wait for the GPU is 72% of the frame - 103 ms of a 143 ms frame at 1080p,
    * stable over 37 consecutive five-second windows - while the CPU burns 49 ms that does not change
    * with resolution. 103 + 49 predicts the frame time at two different resolutions, so the CPU and the
    * GPU are SERIALISED rather than pipelined, and the serialisation point is this file calling
    * wsi_orbis_scanout_present, which waits for the GPU before it copies.
    *
    * Deferring that call by one present is what lets them overlap: the application records and submits
    * frame N+1 while the GPU is still drawing frame N, and only then does anyone wait. Arithmetic says
    * the frame becomes max(cpu, gpu) instead of cpu + gpu - 143 ms to 103 ms at 1080p, and 71 ms to
    * 43 ms at half resolution, where the gain is larger because the GPU side is cheaper.
    *
    * ⚠ AND THE IMAGE MUST STAY BUSY UNTIL ITS COPY HAPPENS. queue_present used to clear busy_on_host
    * immediately, which is correct only when the copy is done before it returns. With the copy deferred,
    * releasing the image would let acquire_next_image hand it straight back to the application, which
    * would render into pixels this file has not read yet. The flag is now cleared where the copy
    * completes, not where the present is requested. */
   int32_t orbis_pending;
   uint64_t orbis_pending_seq;
   /* ⚠ THE IMAGE THE TELEVISION IS SHOWING, which is not free even though its flip is long gone.
    *
    * Only meaningful when the scan-out registered the swapchain's own images. While this file copied
    * into buffers of its own, a flipped image was free the instant the copy returned - the copy was a
    * snapshot. With the images registered directly there is no snapshot: a consumed flip is still on
    * screen until the next one replaces it, and handing that image back is the application rendering
    * into what the display is scanning out. The maintainer saw it as tearing and black bands in the
    * main menu, where a frame is nearly free and the title laps the display; the world never tore
    * because 15 ms of GPU a frame kept it behind. */
   int32_t orbis_onscreen;
   bool orbis_zero_copy;
   /* ⚠ ONE HELD IMAGE WAS NOT ENOUGH, and the menu said so twice.
    *
    * The display takes a QUEUE of flips - up to count-1 of them, which is what the throttle allows -
    * so at any moment the buffers it owns are every queued flip PLUS the one on screen, not just the
    * last one flipped. Holding only the newest released the rest while they were still waiting their
    * turn, and the application drew into them.
    *
    * So the flipped indices are kept in order and released from the front once the display can no
    * longer be holding them. */
   int32_t orbis_flip_q[8];
   uint32_t orbis_flip_n;
#endif
   struct wsi_headless_image images[0];
};
VK_DEFINE_NONDISP_HANDLE_CASTS(wsi_headless_swapchain, base.base, VkSwapchainKHR,
                               VK_OBJECT_TYPE_SWAPCHAIN_KHR)

static struct wsi_image *
wsi_headless_swapchain_get_wsi_image(struct wsi_swapchain *wsi_chain,
                                     uint32_t image_index)
{
   struct wsi_headless_swapchain *chain =
      (struct wsi_headless_swapchain *)wsi_chain;
   return &chain->images[image_index].base;
}

static VkResult
wsi_headless_swapchain_release_images(struct wsi_swapchain *wsi_chain,
                                      uint32_t count, const uint32_t *indices)
{
   struct wsi_headless_swapchain *chain =
      (struct wsi_headless_swapchain *)wsi_chain;

   for (uint32_t i = 0; i < count; i++) {
      uint32_t index = indices[i];
      assert(index < chain->base.image_count);
      chain->images[index].busy_on_device = false;
      chain->images[index].busy_on_host = false;
#ifdef HAVE_ORBIS_PLATFORM
      /* The application is taking this image back before its deferred copy happened. Forget the copy
       * rather than perform it: the contents are about to be undefined anyway, and a pending index that
       * outlives its meaning would flip a frame the application had already reclaimed. */
      if (chain->orbis_pending == (int32_t)index)
         chain->orbis_pending = -1;
      /* Same for the image on screen: the application has taken it back, so this file must stop
       * holding it. What the display shows until the next flip is then undefined, which is what
       * releasing an image means. */
      for (uint32_t q = 0; q < chain->orbis_flip_n; q++)
         if (chain->orbis_flip_q[q] == (int32_t)index)
            chain->orbis_flip_q[q] = -1;
#endif
   }

   return VK_SUCCESS;
}

static VkResult
wsi_headless_swapchain_acquire_next_image(struct wsi_swapchain *wsi_chain,
                                          const VkAcquireNextImageInfoKHR *info,
                                          uint32_t *image_index)
{
   struct wsi_headless_swapchain *chain =
      (struct wsi_headless_swapchain *)wsi_chain;
   struct timespec start_time, end_time;
   struct timespec rel_timeout;

   timespec_from_nsec(&rel_timeout, info->timeout);

   clock_gettime(CLOCK_MONOTONIC, &start_time);
   timespec_add(&end_time, &rel_timeout, &start_time);

   while (1) {
      /* Try to find a free image. */
      for (uint32_t i = 0; i < chain->base.image_count; i++) {
         if (!chain->images[i].busy_on_host) {
            if (chain->images[i].busy_on_device) {
               /* simple trick to avoid the just presented image */
               chain->images[i].busy_on_device = false;
               continue;
            }

            /* We found a non-busy image */
            *image_index = i;
            chain->images[i].busy_on_host = true;
            chain->images[i].busy_on_device = true;
            return VK_SUCCESS;
         }
      }

      /* Check for timeout. */
      struct timespec current_time;
      clock_gettime(CLOCK_MONOTONIC, &current_time);
      if (timespec_after(&current_time, &end_time)) {
#ifdef HAVE_ORBIS_PLATFORM
         /* ⚠ THE FAILURE MODE OF REGISTERING THE SWAPCHAIN'S OWN IMAGES, said out loud rather than
          * left as a stall. If every image is spoken for - the display holding some, this file
          * holding the rest - the application waits for one that cannot come back, and from the
          * outside that is a frozen title with an empty log.
          *
          * The arithmetic that prevents it is in wsi_orbis and derived from the image count, so this
          * should be unreachable. It is here because "should be" is what the next title will test,
          * and a driver that only works for the swapchain shape it was written against is not a
          * driver. If this ever fires, the reservation is wrong for that application, not for this
          * one. */
         if (chain->orbis_zero_copy) {
            static bool said;
            if (!said) {
               mesa_logw("wsi/orbis: no free image after %llu ns with %u images, %u held for the "
                         "display - the zero-copy reservation is too small for this swapchain. Set "
                         "ORBIS_WSI_NO_DEFER=1 to give one back, or report the image count.",
                         (unsigned long long)info->timeout, chain->base.image_count, chain->orbis_flip_n);
               said = true;
            }
         }
#endif
         return VK_NOT_READY;
      }
   }
}

#ifdef HAVE_ORBIS_PLATFORM
/* ⚠ ONE ANSWER, TWO CALLERS. Whether presents are deferred decides both how queue_present behaves
 * and how many images the swapchain must have, and the two must not be able to disagree - which is
 * what a second getenv() in the second place would allow. Announced once, on the first call. */
static bool
wsi_headless_orbis_defers(void)
{
   static int defer = -1;
   if (defer < 0) {
      const char *const e = getenv("ORBIS_WSI_NO_DEFER");
      defer = (e != NULL && *e != '\0' && *e != '0') ? 0 : 1;
      mesa_logi("wsi/orbis: present is %s - the GPU wait is %s",
                defer ? "DEFERRED by one frame" : "immediate (ORBIS_WSI_NO_DEFER)",
                defer ? "overlapped with the next frame's recording" : "on the critical path");
   }
   return defer == 1;
}

/* Record that `index` has been flipped, and release whatever the display can no longer be holding:
 * the queued flips plus the one on screen are the newest pending+1 entries, and everything older is
 * free. Called only when the swapchain's own images are registered. */
static void
wsi_headless_orbis_retire_flips(struct wsi_headless_swapchain *chain, uint32_t index)
{
   if (chain->orbis_flip_n < ARRAY_SIZE(chain->orbis_flip_q)) {
      chain->orbis_flip_q[chain->orbis_flip_n++] = (int32_t)index;
   } else {
      /* Cannot happen while the queue is at least as long as the image count, and if it ever does,
       * dropping the OLDEST is the safe end to drop: it is the one most likely already shown. */
      memmove(&chain->orbis_flip_q[0], &chain->orbis_flip_q[1],
              (ARRAY_SIZE(chain->orbis_flip_q) - 1) * sizeof(chain->orbis_flip_q[0]));
      chain->orbis_flip_q[ARRAY_SIZE(chain->orbis_flip_q) - 1] = (int32_t)index;
   }

   const uint32_t keep = wsi_orbis_flips_pending(chain->scanout) + 1;
   while (chain->orbis_flip_n > keep) {
      const int32_t done = chain->orbis_flip_q[0];
      memmove(&chain->orbis_flip_q[0], &chain->orbis_flip_q[1],
              (chain->orbis_flip_n - 1) * sizeof(chain->orbis_flip_q[0]));
      chain->orbis_flip_n--;
      if (done >= 0)
         chain->images[done].busy_on_host = false;
   }
}
#endif

static VkResult
wsi_headless_swapchain_queue_present(struct wsi_swapchain *wsi_chain,
                                     uint32_t image_index,
                                     uint64_t present_id,
                                     const VkPresentRegionKHR *damage)
{
   struct wsi_headless_swapchain *chain =
      (struct wsi_headless_swapchain *)wsi_chain;

   assert(image_index < chain->base.image_count);

#ifndef HAVE_ORBIS_PLATFORM
   chain->images[image_index].busy_on_host = false;
#endif

#ifdef HAVE_ORBIS_PLATFORM
   /* ⚠ ONE BOUNDED LINE, AND IT EARNS ITS PLACE. Everything below this point is silent on success, so
    * without it "the title never presented" and "the present path ran and the picture went nowhere" are the
    * same empty log - which has already cost this port one blind run. Bounded to the first few frames so it
    * cannot become per-frame noise.
    */
   {
      static unsigned said;
      if (said < 4) {
         ++said;
         mesa_logi("wsi/orbis: queue_present entered - image %u, scanout %s", image_index,
                   chain->scanout != NULL ? "present" : "ABSENT");
      }
   }
#endif

#ifdef HAVE_ORBIS_PLATFORM
   /* The image is LINEAR and CPU-mapped because this platform sets wsi_device->wants_linear, so cpu_map is the
    * pixels and row_pitches[0] is their stride. A failed flip is reported as OUT_OF_DATE rather than swallowed:
    * an application that never learns presentation stopped working will draw forever into nothing.
    */
   /* ⚠ EVERY WAY OUT OF THIS FUNCTION MUST EITHER HAND THE IMAGE ON OR GIVE IT BACK, and three of
    * them used to do neither. Upstream clears busy_on_host at the top of queue_present, which this
    * arm had to take out because a deferred present still owns the image; what replaced it released
    * the image only on the paths that succeeded. A failed flip - and flips DO fail here, which is
    * why the scan-out slot has a timeout at all - therefore consumed one image for the life of the
    * swapchain, and after image_count failures vkAcquireNextImageKHR has nothing left to return and
    * the title stops with no error anywhere. Same for the scanout == NULL case, which skipped the
    * whole block and never released anything.
    *
    * So: one exit, and handed_on says whether somebody else is now responsible for the image -
    * the deferred slot, or the zero-copy flip queue. */
   VkResult orbis_result = VK_SUCCESS;
   bool handed_on = false;

   if (chain->scanout != NULL) {
      const struct wsi_image *img = &chain->images[image_index].base;

      /* The one thing this cannot assume, and the first thing the copy would touch. Without the check, a wrong
       * wants_linear leaves "queue_present was never called" and "the copy faulted" indistinguishable - both are
       * a dead process and an empty log.
       */
      if (img->cpu_map == NULL) {
         mesa_loge("wsi/orbis: image %u has no CPU mapping - wants_linear did not take effect", image_index);
         orbis_result = VK_ERROR_OUT_OF_DATE_KHR;
         goto orbis_done;
      }
      /* ⚠ ONE PRESENT LATE, ON PURPOSE - see orbis_pending above. This call waits for the GPU, copies
       * and flips; doing it for the PREVIOUS frame means the application has already recorded and
       * submitted this one, so the wait absorbs only what the GPU still had left rather than all of it.
       *
       * ORBIS_WSI_NO_DEFER=1 goes back to presenting immediately. Kept because this changes the display
       * path, and a display path that goes wrong is the one failure a log cannot describe. */
      const bool defer = wsi_headless_orbis_defers();

      if (!defer) {
         if (!wsi_orbis_scanout_present(chain->scanout, image_index, img->cpu_map,
                                        (uint32_t)img->row_pitches[0], 0)) {
            orbis_result = VK_ERROR_OUT_OF_DATE_KHR;
            goto orbis_done;
         }
         if (chain->orbis_zero_copy) {
            wsi_headless_orbis_retire_flips(chain, image_index);
            handed_on = true; /* the flip queue releases it when the display is done with it */
         }
      } else {
         if (chain->orbis_pending >= 0) {
            const struct wsi_image *prev = &chain->images[chain->orbis_pending].base;
            const bool ok = wsi_orbis_scanout_present(chain->scanout, (uint32_t)chain->orbis_pending,
                                                      prev->cpu_map, (uint32_t)prev->row_pitches[0],
                                                      chain->orbis_pending_seq);
            if (chain->orbis_zero_copy) {
               wsi_headless_orbis_retire_flips(chain, (uint32_t)chain->orbis_pending);
            } else {
               chain->images[chain->orbis_pending].busy_on_host = false;
            }
            chain->orbis_pending = -1;
            if (!ok) {
               orbis_result = VK_ERROR_OUT_OF_DATE_KHR;
               goto orbis_done;
            }
         }
         chain->orbis_pending = (int32_t)image_index;
         handed_on = true; /* released when this frame is finally presented, or at destroy */
         /* Everything this frame needed has been submitted by now, so this is the sequence its
          * rendering ends at. Taken here rather than at flush time, which is the whole point.
          *
          * Declared locally for the same reason ac_orbis_wait_gpu_idle is in wsi_orbis.c: this is the
          * port's own seam between the WSI and the submission path, and pulling ac_linux_drm.h into a
          * generic WSI file would imply a much wider contract than one counter. */
         { extern uint64_t ac_orbis_gpu_seq_now(void);
           chain->orbis_pending_seq = ac_orbis_gpu_seq_now(); }
      }
   }

orbis_done:
   if (!handed_on)
      chain->images[image_index].busy_on_host = false;
   return orbis_result;
#endif

   return VK_SUCCESS;
}

static VkResult
wsi_headless_swapchain_wait_for_present(struct wsi_swapchain *wsi_chain,
                                        uint64_t waitValue,
                                        uint64_t timeout)
{
   return wsi_swapchain_wait_for_present_semaphore(
      wsi_chain, waitValue, timeout);
}

static VkResult
wsi_headless_swapchain_destroy(struct wsi_swapchain *wsi_chain,
                               const VkAllocationCallbacks *pAllocator)
{
   struct wsi_headless_swapchain *chain =
      (struct wsi_headless_swapchain *)wsi_chain;

#ifdef HAVE_ORBIS_PLATFORM
   /* ⚠ THE LAST FRAME IS STILL PENDING, and dropping it would leave the television showing the one
    * before it - and, worse, would tear down the scan-out buffers while a copy into them was still
    * owed. A swapchain is destroyed on every resolution change, not only at exit.
    *
    * ⚠ AND ALL OF THIS RUNS BEFORE THE IMAGES ARE DESTROYED, which it did not. The loop below frees
    * every image - Unmap then Free, leaving cpu_map pointing at memory that is gone, because
    * wsi_destroy_image does not clear it - and the flush then read prev->cpu_map from a destroyed
    * image. The `prev->cpu_map != NULL` guard passed on the dangling pointer, so the copy path
    * memcpy'd out of unmapped memory and the zero-copy path flipped a freed buffer that the display
    * then kept scanning. Present first, take the scan-out down second, free the images last: after
    * wsi_orbis_scanout_destroy the display holds nothing of ours. */
   if (chain->orbis_pending >= 0 && chain->scanout != NULL) {
      const struct wsi_image *prev = &chain->images[chain->orbis_pending].base;
      if (prev->cpu_map != NULL)
         wsi_orbis_scanout_present(chain->scanout, (uint32_t)chain->orbis_pending, prev->cpu_map,
                                   (uint32_t)prev->row_pitches[0], chain->orbis_pending_seq);
      chain->images[chain->orbis_pending].busy_on_host = false;
      chain->orbis_pending = -1;
   }
   for (uint32_t q = 0; q < chain->orbis_flip_n; q++)
      if (chain->orbis_flip_q[q] >= 0)
         chain->images[chain->orbis_flip_q[q]].busy_on_host = false;
   chain->orbis_flip_n = 0;

   wsi_orbis_scanout_destroy(chain->scanout);
   chain->scanout = NULL;
#endif

   for (uint32_t i = 0; i < chain->base.image_count; i++) {
      if (chain->images[i].base.image != VK_NULL_HANDLE)
         wsi_destroy_image(&chain->base, &chain->images[i].base);
   }

   wsi_swapchain_finish(&chain->base);

   vk_free(pAllocator, chain);

   return VK_SUCCESS;
}

static VkResult
wsi_headless_surface_create_swapchain(VkIcdSurfaceBase *icd_surface,
                                      VkDevice device,
                                      struct wsi_device *wsi_device,
                                      const VkSwapchainCreateInfoKHR* pCreateInfo,
                                      const VkAllocationCallbacks* pAllocator,
                                      struct wsi_swapchain **swapchain_out)
{
   struct wsi_headless_swapchain *chain;
   VkResult result;

   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR);

   int num_images = pCreateInfo->minImageCount;

   size_t size = sizeof(*chain) + num_images * sizeof(chain->images[0]);
   chain = vk_zalloc(pAllocator, size, 8, VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (chain == NULL)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   VkDrmFormatModifierPropertiesListEXT mod_list = {
      .sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT,
   };
   VkFormatProperties2 props = {
      .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
      .pNext = &mod_list,
   };
   if (!wsi_device->sw && wsi_device->supports_modifiers) {
      wsi_device->GetPhysicalDeviceFormatProperties2(
         wsi_device->pdevice, pCreateInfo->imageFormat, &props);
      assert(mod_list.drmFormatModifierCount > 0);
   }

   STACK_ARRAY(VkDrmFormatModifierPropertiesEXT, mod_props,
               mod_list.drmFormatModifierCount);
   STACK_ARRAY(uint64_t, mods, mod_list.drmFormatModifierCount);

   if (mod_list.drmFormatModifierCount > 0) {
      mod_list.pDrmFormatModifierProperties = mod_props;
      wsi_device->GetPhysicalDeviceFormatProperties2(
         wsi_device->pdevice, pCreateInfo->imageFormat, &props);

      for (uint32_t i = 0; i < mod_list.drmFormatModifierCount; i++)
         mods[i] = mod_props[i].drmFormatModifier;
   }

   struct wsi_base_image_params *image_params = NULL;
   struct wsi_cpu_image_params cpu_params;
   struct wsi_drm_image_params drm_params;
   /* ⚠ THE CPU PATH IS NOT OPTIONAL ON THIS PLATFORM, and `sw` is the wrong switch for saying so: the console has
    * a real GPU, so wsi_device->sw is false, but it has no DRM, no dma-buf and no format modifiers - and the
    * other branch asks for all three. With wants_linear set, the CPU path gives a LINEAR image that is directly
    * CPU-mapped, which is exactly what a scan-out buffer needs and what queue_present reads.
    */
   if (wsi_device->sw || WSI_ORBIS_FORCE_CPU_IMAGES) {
      cpu_params = (struct wsi_cpu_image_params) {
         .base.image_type = WSI_IMAGE_TYPE_CPU,
      };
      image_params = &cpu_params.base;
   } else {
      drm_params = (struct wsi_drm_image_params) {
         .base.image_type = WSI_IMAGE_TYPE_DRM,
         .same_gpu = true,
         .num_modifier_lists = mod_list.drmFormatModifierCount > 0 ? 1 : 0,
         .num_modifiers = &mod_list.drmFormatModifierCount,
         .modifiers = (const uint64_t **)&mods,
      };
      image_params = &drm_params.base;
   }

   result = wsi_swapchain_init(wsi_device, &chain->base, device,
                               pCreateInfo, image_params, pAllocator);

   STACK_ARRAY_FINISH(mods);
   STACK_ARRAY_FINISH(mod_props);

   if (result != VK_SUCCESS)
      goto fail_free_chain;

   chain->base.destroy = wsi_headless_swapchain_destroy;
   chain->base.get_wsi_image = wsi_headless_swapchain_get_wsi_image;
   chain->base.acquire_next_image = wsi_headless_swapchain_acquire_next_image;
   chain->base.release_images = wsi_headless_swapchain_release_images;
   chain->base.queue_present = wsi_headless_swapchain_queue_present;
#ifdef HAVE_ORBIS_PLATFORM
   /* No frame is waiting to be copied yet. -1 rather than 0, because 0 is a valid image index and a
    * zeroed field would flip the first frame's contents before anything had rendered into them.
    *
    * ⚠ THE SCAN-OUT IS SET UP AFTER THE IMAGES NOW, not before, because it registers them. */
   chain->orbis_pending = -1;
   chain->orbis_onscreen = -1;
   chain->orbis_flip_n = 0;
   chain->orbis_zero_copy = false;
#endif

   chain->base.wait_for_present = wsi_headless_swapchain_wait_for_present;
   chain->base.present_mode = wsi_swapchain_get_present_mode(wsi_device, pCreateInfo);
   chain->base.image_count = num_images;

   uint32_t image = 0;
   for (; image < chain->base.image_count; image++) {
      result = wsi_create_image(&chain->base, &chain->base.image_info,
                                &chain->images[image].base);
      if (result != VK_SUCCESS)
         goto fail_destroy_images;

      chain->images[image].busy_on_host = false;
      chain->images[image].busy_on_device = false;
   }

#ifdef HAVE_ORBIS_PLATFORM
   /* ⚠ REGISTER THE IMAGES THEMSELVES, so the present is a flip rather than 8100 KiB of memcpy.
    *
    * The copy was costing 2.3 ms of a 21.9 ms frame. It only ever existed because this WSI allocated
    * its own scan-out buffers while the swapchain's images lived elsewhere - and they need not: the
    * images are linear and CPU-mapped already, and this port's memory is direct memory, which is what
    * the display controller scans out of.
    *
    * Every image must be mapped and share one pitch; video-out registers one attribute for all of
    * them. If any of that does not hold, or if video-out refuses the addresses, the fallback below
    * allocates buffers of its own and the copy comes back - slower, and correct. */
   {
      void *addrs[8];
      uint32_t pitch_px = 0;
      bool direct = chain->base.image_count <= ARRAY_SIZE(addrs);

      for (uint32_t i = 0; direct && i < chain->base.image_count; i++) {
         const struct wsi_image *img = &chain->images[i].base;
         const uint32_t p = (uint32_t)(img->row_pitches[0] / 4);

         if (img->cpu_map == NULL || (img->row_pitches[0] % 4) != 0 || p < pCreateInfo->imageExtent.width)
            direct = false;
         else if (pitch_px != 0 && p != pitch_px)
            direct = false;
         else {
            pitch_px = p;
            addrs[i] = img->cpu_map;
         }
      }

      if (direct)
         chain->scanout = wsi_orbis_scanout_create(pCreateInfo->imageExtent.width,
                                                   pCreateInfo->imageExtent.height,
                                                   chain->base.image_count, addrs, pitch_px);

      /* ⚠ ASK, DO NOT ASSUME, HOW MANY IMAGES THIS NEEDS.
       *
       * Registering the swapchain's own images means the display owns some of them at all times, so
       * a swapchain that is too small deadlocks in acquire_next_image rather than tearing - and how
       * small is too small depends on what THIS file holds, not on what OpenGothic happens to ask
       * for. It holds one for the application to draw into and, while presents are deferred, one
       * more; wsi_orbis turns that into the smallest count that can work.
       *
       * A first version wrote "three" here because the swapchain in front of it had five images.
       * That is how a driver becomes a fixture for one title. */
      if (chain->scanout != NULL) {
         const uint32_t reserve = 1 + (wsi_headless_orbis_defers() ? 1u : 0u);
         const uint32_t need = wsi_orbis_scanout_reserve(chain->scanout, reserve);

         if (chain->base.image_count < need) {
            mesa_logi("wsi/orbis: %u swapchain images is too few for zero copy - the display holds the "
                      "one on screen and the one being flipped, and this WSI holds %u more, so %u is "
                      "the floor. Falling back to a copy per frame.",
                      chain->base.image_count, reserve, need);
            wsi_orbis_scanout_destroy(chain->scanout);
            chain->scanout = NULL;
         }
      }
      chain->orbis_zero_copy = chain->scanout != NULL;
      if (chain->scanout == NULL)
         chain->scanout = wsi_orbis_scanout_create(pCreateInfo->imageExtent.width,
                                                   pCreateInfo->imageExtent.height,
                                                   chain->base.image_count, NULL, 0);
      if (chain->scanout == NULL) {
         /* Refused both ways, with the failing call already named in the log by wsi_orbis. A swapchain
          * that cannot reach the display must not pretend to be one. */
         result = VK_ERROR_INITIALIZATION_FAILED;
         goto fail_destroy_images;
      }
   }
#endif

   *swapchain_out = &chain->base;

   return VK_SUCCESS;

fail_destroy_images:
   for (uint32_t i = 0; i < image; i++)
      wsi_destroy_image(&chain->base, &chain->images[i].base);
   wsi_swapchain_finish(&chain->base);
fail_free_chain:
   vk_free(pAllocator, chain);

   return result;
}

VkResult
wsi_headless_init_wsi(struct wsi_device *wsi_device,
                      const VkAllocationCallbacks *alloc,
                      VkPhysicalDevice physical_device)
{
   struct wsi_headless *wsi;
   VkResult result;

   wsi = vk_alloc(alloc, sizeof(*wsi), 8,
                   VK_SYSTEM_ALLOCATION_SCOPE_INSTANCE);
   if (!wsi) {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;
      goto fail;
   }

   wsi->base.get_support = wsi_headless_surface_get_support;
   wsi->base.get_capabilities2 = wsi_headless_surface_get_capabilities2;
   wsi->base.get_formats = wsi_headless_surface_get_formats;
   wsi->base.get_formats2 = wsi_headless_surface_get_formats2;
   wsi->base.get_present_modes = wsi_headless_surface_get_present_modes;
   wsi->base.get_present_rectangles = wsi_headless_surface_get_present_rectangles;
   wsi->base.create_swapchain = wsi_headless_surface_create_swapchain;

   wsi_device->wsi[VK_ICD_WSI_PLATFORM_HEADLESS] = &wsi->base;

   return VK_SUCCESS;

fail:
   wsi_device->wsi[VK_ICD_WSI_PLATFORM_HEADLESS] = NULL;

   return result;
}

void
wsi_headless_finish_wsi(struct wsi_device *wsi_device,
                        const VkAllocationCallbacks *alloc)
{
   struct wsi_headless *wsi =
      (struct wsi_headless *)wsi_device->wsi[VK_ICD_WSI_PLATFORM_HEADLESS];
   if (!wsi)
      return;

   vk_free(alloc, wsi);
}

VkResult wsi_CreateHeadlessSurfaceEXT(
    VkInstance                                  _instance,
    const VkHeadlessSurfaceCreateInfoEXT*       pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkSurfaceKHR*                               pSurface)
{
   VK_FROM_HANDLE(vk_instance, instance, _instance);
   VkIcdSurfaceHeadless *surface;

   surface = vk_alloc2(&instance->alloc, pAllocator, sizeof *surface, 8,
                       VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (surface == NULL)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   surface->base.platform = VK_ICD_WSI_PLATFORM_HEADLESS;

   *pSurface = VkIcdSurfaceBase_to_handle(&surface->base);
   return VK_SUCCESS;
}
