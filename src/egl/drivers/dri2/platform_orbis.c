/*
 * Copyright 2026 Mikołaj Mikołajczyk
 * SPDX-License-Identifier: MIT
 *
 * EGL for the PlayStation 4 (Orbis).
 *
 * WHY THIS FILE EXISTS AT ALL. Everything else in the OpenGL path was already in the tree by the time
 * this was written: zink resolves Vulkan without dlopen, kopper knows how to drive a
 * VK_EXT_headless_surface swapchain, and wsi_orbis turns a present into an sceVideoOut flip. What was
 * missing was the first link in that chain - something for a title to call. This is it:
 *
 *     eglSwapBuffers -> kopper -> vkQueuePresentKHR -> VK_EXT_headless_surface -> wsi_orbis -> flip
 *
 * It is modelled on platform_surfaceless.c, with two differences that are the whole point:
 *
 *   1. NO DRM, ANYWHERE. surfaceless_probe_device() walks _eglGlobal.DeviceList for _EGL_DEVICE_DRM
 *      entries and then calls loader_open_device()/loader_get_driver_for_fd()/drmGetNodeTypeFromFd().
 *      None of that exists on this console - there is no /dev/dri, no libdrm and no device node - so
 *      the entire probe loop is gone, replaced by the three facts it would have been looking for:
 *      fd == -1, driver "zink", the kopper loader extensions.
 *
 *   2. REAL WINDOW SURFACES. surfaceless implements only create_pbuffer_surface, and eglSwapBuffers on
 *      a pbuffer is a no-op - copying that would defeat the purpose. Here a window surface is a
 *      headless Vulkan surface that the WSI scans out, so eglSwapBuffers really does put a frame on
 *      the television.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/macros.h"
#include "util/os_misc.h"

#include "egl_dri2.h"
#include "eglglobals.h"
#include "kopper_interface.h"
#include "mesa_interface.h"
#include "dri_util.h"
#include "dri_screen.h"

/* ⚠ THE SCAN-OUT FORMAT IS FIXED AND NOTHING DOWNSTREAM WILL COMPLAIN ABOUT GETTING IT WRONG.
 *
 * wsi_orbis registers its buffers as ORBIS_VIDEO_OUT_PIXEL_FORMAT_A8B8G8R8_SRGB - a hardcoded
 * attribute, not derived from pCreateInfo->imageFormat - and presentation is a straight copy or a
 * flip with no swizzle anywhere. A swapchain created with a B-first format therefore produces a
 * picture with red and blue exchanged, and it produces it silently: there is no validation, no
 * fallback and no error return on that path. wsi_common_headless.c already restricts the surface's
 * advertised formats to R8G8B8A8 UNORM/SRGB for exactly this reason; this list is the same decision
 * one layer up, because zink hands vkCreateSwapchainKHR whatever pipe_format the EGLConfig carries
 * (zink_kopper.c: `cswap->scci.imageFormat = cdt->formats[0]`, from the drawable's gl_config) and
 * never asks the surface what it supports.
 *
 * So: R8G8B8A8 only. The BGRA configs that dri_fill_in_modes also produces are deliberately not
 * exposed - not sorted last, not deprioritised, simply absent - because an EGLConfig this platform
 * never returns is an EGLConfig that can never end up on the display.
 *
 * UNORM and SRGB are both here and they collapse into ONE EGLConfig each: dri2_add_config() folds the
 * sRGB variant into conf->dri_config[double_buffer][1], and EGL_GL_COLORSPACE on the surface picks
 * between them. That is the ordinary Mesa arrangement and it needs nothing from us.
 */
static const enum pipe_format orbis_visuals[] = {
   PIPE_FORMAT_RGBA8888_UNORM,
   PIPE_FORMAT_RGBA8888_SRGB,
};

/* The console has exactly one scan-out and this port has only ever configured it for 1080p.
 *
 * ⚠ THE MODE IS NOT QUERIED HERE, AND THAT IS NOT AN OVERSIGHT. The video-out handle belongs to
 * wsi_orbis, which opens it in wsi_orbis_scanout_create() when the swapchain appears - i.e. AFTER this
 * function has already had to answer. Opening a second one from EGL to ask about the mode would mean
 * two owners of the same device for the lifetime of the display. The direction of travel is the other
 * way round anyway: wsi_orbis takes its geometry from the swapchain extent, so what EGL says here is
 * what the scan-out is configured to, rather than the reverse.
 *
 * ORBIS_EGL_MODE=WxH is a bring-up lever in the convention of the ORBIS_WSI_* knobs next door. The
 * first console run of this file is the first time any of this is executed at all, and being able to
 * try 1280x720 without a rebuild is worth six lines.
 */
static void
orbis_get_scanout_size(int *width, int *height)
{
   const char *mode = os_get_option("ORBIS_EGL_MODE");
   int w = 0, h = 0;

   if (mode && sscanf(mode, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
      *width = w;
      *height = h;
      return;
   }

   *width = 1920;
   *height = 1080;
}

/* ⚠ THIS IS THE FUNCTION THAT MAKES THE WHOLE CHAIN HEADLESS. Everything kopper does downstream
 * branches on info->bos.sType and nothing else: init_dt_type() maps it to KOPPER_HEADLESS,
 * kopper_CreateSurface() calls vkCreateHeadlessSurfaceEXT(), and dri_drawable::is_window becomes true
 * because the sType is non-zero. Leave the storage as calloc left it - as platform_surfaceless does,
 * having no SetSurfaceCreateInfo hook at all - and every drawable is a pbuffer forever.
 *
 * VkHeadlessSurfaceCreateInfoEXT is {sType, pNext, flags}, so it fits in the create-info storage with
 * room to spare; the static_assert below says so at compile time the way platform_x11.c does for xcb.
 */
static_assert(sizeof(struct kopper_vk_surface_create_storage) >=
                 sizeof(VkHeadlessSurfaceCreateInfoEXT),
              "VkHeadlessSurfaceCreateInfoEXT does not fit kopper's create-info storage");

static void
orbis_kopper_set_surface_create_info(void *_draw, struct kopper_loader_info *ci)
{
   struct dri2_egl_surface *dri2_surf = _draw;
   VkHeadlessSurfaceCreateInfoEXT *headless =
      (VkHeadlessSurfaceCreateInfoEXT *)&ci->bos;

   /* Pbuffers must stay pbuffers. kopper reads is_window as `!isPixmap && sType != 0`, so leaving the
    * sType at zero here is what keeps a pbuffer off the swapchain path - the same effect
    * kopper_pbuffer_loader_extension gets by supplying no hook at all. */
   if (dri2_surf->base.Type != EGL_WINDOW_BIT)
      return;

   headless->sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT;
   headless->pNext = NULL;
   headless->flags = 0;

   /* No compositor, so no composite alpha: the scan-out is the bottom of the stack and its alpha
    * channel is never read by anything. has_alpha = 0 makes zink_kopper ask for
    * VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, which is the truthful request even though wsi_orbis ignores
    * the field entirely. present_opaque is a Wayland concept and stays false. */
   ci->has_alpha = 0;
   ci->present_opaque = false;

   /* Ask for FIFO from the first swapchain rather than after the first eglSwapInterval. Interval 0
    * would send zink_kopper_set_present_mode_for_interval() looking for IMMEDIATE and then MAILBOX,
    * neither of which this backend offers, and it would land on FIFO anyway - but by the long way
    * round, and MAILBOX is no longer even advertised (see wsi_common_headless.c). The flip is
    * ORBIS_VIDEO_OUT_FLIP_VSYNC and wsi_orbis_wait_for_flip_slot() blocks the producer, so FIFO is
    * not a preference here, it is a description. */
   ci->initial_swap_interval = 1;
}

static void
orbis_kopper_get_drawable_info(struct dri_drawable *draw, int *w, int *h,
                               void *loaderPrivate)
{
   struct dri2_egl_surface *dri2_surf = loaderPrivate;

   /* ⚠ THE LOADER IS THE SOURCE OF TRUTH FOR SIZE ON THIS PLATFORM, and this is the only place it
    * speaks. zink_kopper_update() re-queries the surface's capabilities for KOPPER_X11 only; every
    * other type - headless included - trusts what GetDrawableInfo returns. A resolution change would
    * therefore have to arrive here, by changing the EGLSurface's dimensions, and not from the
    * swapchain going out of date. */
   *w = dri2_surf->base.Width;
   *h = dri2_surf->base.Height;
}

static const __DRIkopperLoaderExtension orbis_kopper_loader_extension = {
   .base = {__DRI_KOPPER_LOADER, 1},

   .SetSurfaceCreateInfo = orbis_kopper_set_surface_create_info,
   .GetDrawableInfo = orbis_kopper_get_drawable_info,
};

/* No __DRIimageLoaderExtension on purpose: kopper_allocate_textures() takes the loader-image path
 * whenever screen->image.loader is set, and then the swapchain images it would otherwise create for
 * itself never happen. image_lookup_extension is unrelated - it is EGLImage lookup - and is what every
 * other platform passes here. */
static const __DRIextension *orbis_kopper_loader_extensions[] = {
   &orbis_kopper_loader_extension.base,
   &image_lookup_extension.base,
   NULL,
};

static _EGLSurface *
dri2_orbis_create_surface(_EGLDisplay *disp, EGLint type, _EGLConfig *conf,
                          void *native_window, const EGLint *attrib_list)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_config *dri2_conf = dri2_egl_config(conf);
   struct dri2_egl_surface *dri2_surf;
   const struct dri_config *config;

   dri2_surf = calloc(1, sizeof *dri2_surf);
   if (!dri2_surf) {
      _eglError(EGL_BAD_ALLOC, "eglCreateSurface");
      return NULL;
   }

   if (!dri2_init_surface(&dri2_surf->base, disp, type, conf, attrib_list,
                          false, native_window))
      goto cleanup_surface;

   if (type == EGL_WINDOW_BIT) {
      /* ⚠ WHAT HAPPENS TO NativeWindowType: NOTHING, AND IT MUST STILL NOT BE NULL.
       *
       * A "window" on this console is the video-out scan-out itself. There is no handle a title could
       * obtain and pass in - sceVideoOutOpen() is called by wsi_orbis, after this point, on behalf of
       * the swapchain - so the parameter is recorded on the _EGLSurface (for
       * _eglNativeSurfaceAlreadyUsed) and otherwise ignored.
       *
       * It cannot simply be documented as "pass NULL", because _eglCreateWindowSurfaceCommon() in the
       * EGL core rejects a NULL native_window with EGL_BAD_NATIVE_WINDOW BEFORE the platform is
       * consulted. The contract for a title is therefore: pass any non-zero value, conventionally
       * (EGLNativeWindowType)1.
       *
       * The pleasant side effect is that the core's _eglNativeSurfaceAlreadyUsed() check then enforces
       * one window surface per distinct value - and since there is exactly one scan-out, a title that
       * reuses the same token twice gets EGL_BAD_ALLOC instead of two EGLSurfaces fighting over the
       * display. That is the behaviour we would have had to write by hand.
       */
      orbis_get_scanout_size(&dri2_surf->base.Width, &dri2_surf->base.Height);
   }

   config = dri2_get_dri_config(dri2_conf, type, dri2_surf->base.GLColorspace);
   if (!config) {
      _eglError(EGL_BAD_MATCH,
                "Unsupported surfacetype/colorspace configuration");
      goto cleanup_surface;
   }

   if (!dri2_create_drawable(dri2_dpy, config, dri2_surf, dri2_surf))
      goto cleanup_surface;

   return &dri2_surf->base;

cleanup_surface:
   free(dri2_surf);
   return NULL;
}

static EGLBoolean
dri2_orbis_swap_interval(_EGLDisplay *disp, _EGLSurface *surf, EGLint interval)
{
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(surf);

   /* ⚠ SAID ONCE, BECAUSE THE ANSWER TO eglSwapInterval() HERE IS ALWAYS 1 AND SILENCE IS THE WRONG
    * WAY TO SAY SO. dri2_setup_screen() leaves min = max = default = 1 on this platform (see the
    * note in dri2_initialize_orbis), so the core clamps whatever the application asked for before it
    * ever reaches this function - an application that calls eglSwapInterval(0) expecting to run
    * unthrottled gets EGL_TRUE and no hint that nothing changed.
    *
    * It cannot be honoured by anything below us: the flip is submitted with
    * ORBIS_VIDEO_OUT_FLIP_VSYNC, the rate is pinned by sceVideoOutSetFlipRate, and
    * wsi_orbis_wait_for_flip_slot() blocks the producer once it runs ahead. So the value is not
    * being ignored out of laziness; there is no mechanism to ignore it WITH. One line in the log is
    * what stands between that and somebody spending a day on a frame limiter that was never theirs.
    */
   static bool said = false;
   if (!said) {
      said = true;
      _eglLog(_EGL_INFO, "orbis-egl: swap interval is pinned at 1 - the flip is vsync-locked and the "
                         "WSI throttles the producer, so eglSwapInterval() cannot change the rate");
   }

   kopperSetSwapInterval(dri2_surf->dri_drawable, interval);

   return EGL_TRUE;
}

static _EGLSurface *
dri2_orbis_create_window_surface(_EGLDisplay *disp, _EGLConfig *conf,
                                 void *native_window,
                                 const EGLint *attrib_list)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   _EGLSurface *surf;

   surf = dri2_orbis_create_surface(disp, EGL_WINDOW_BIT, conf, native_window,
                                    attrib_list);
   if (surf) {
      surf->SwapInterval = 1;
      dri2_orbis_swap_interval(disp, surf, dri2_dpy->default_swap_interval);
   }

   return surf;
}

static _EGLSurface *
dri2_orbis_create_pbuffer_surface(_EGLDisplay *disp, _EGLConfig *conf,
                                  const EGLint *attrib_list)
{
   return dri2_orbis_create_surface(disp, EGL_PBUFFER_BIT, conf, NULL,
                                    attrib_list);
}

static EGLBoolean
dri2_orbis_destroy_surface(_EGLDisplay *disp, _EGLSurface *surf)
{
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(surf);

   driDestroyDrawable(dri2_surf->dri_drawable);

   dri2_fini_surface(surf);
   free(dri2_surf);

   return EGL_TRUE;
}

static EGLBoolean
dri2_orbis_swap_buffers_with_damage(_EGLDisplay *disp, _EGLSurface *draw,
                                    const EGLint *rects, EGLint numRects)
{
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(draw);

   if (numRects)
      kopperSwapBuffersWithDamage(dri2_surf->dri_drawable,
                                  __DRI2_FLUSH_CONTEXT |
                                     __DRI2_FLUSH_INVALIDATE_ANCILLARY,
                                  numRects, rects);
   else
      kopperSwapBuffers(dri2_surf->dri_drawable,
                        __DRI2_FLUSH_CONTEXT |
                           __DRI2_FLUSH_INVALIDATE_ANCILLARY);

   /* platform_x11.c follows the swap with kopperQuerySurfaceSize(), because an X11 window can be
    * resized under the drawable and the swapchain finds out first. Nothing of the sort can happen
    * here: kopperQuerySurfaceSize() returns dri_drawable::w/h, which for a headless drawable is
    * whatever orbis_kopper_get_drawable_info() last reported, which is this surface's own dimensions.
    * The call would be a round trip through our own answer. */
   return EGL_TRUE;
}

static EGLBoolean
dri2_orbis_swap_buffers(_EGLDisplay *disp, _EGLSurface *draw)
{
   return dri2_orbis_swap_buffers_with_damage(disp, draw, NULL, 0);
}

static EGLint
dri2_orbis_query_buffer_age(_EGLDisplay *disp, _EGLSurface *surf)
{
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(surf);

   return kopperQueryBufferAge(dri2_surf->dri_drawable);
}

static const struct dri2_egl_display_vtbl dri2_orbis_display_vtbl = {
   .create_window_surface = dri2_orbis_create_window_surface,
   .create_pbuffer_surface = dri2_orbis_create_pbuffer_surface,
   .destroy_surface = dri2_orbis_destroy_surface,
   .create_image = dri2_create_image_khr,
   .swap_interval = dri2_orbis_swap_interval,
   .swap_buffers = dri2_orbis_swap_buffers,
   .swap_buffers_with_damage = dri2_orbis_swap_buffers_with_damage,
   .query_buffer_age = dri2_orbis_query_buffer_age,
   .get_dri_drawable = dri2_surface_get_dri_drawable,

   /* ⚠ get_sync_values IS DELIBERATELY NULL AND CHROMIUM_sync_control IS DELIBERATELY UNSET.
    *
    * platform_x11.c sets disp->Extensions.CHROMIUM_sync_control and supplies no get_sync_values, which
    * works there only because the DRI3 path (platform_x11_dri3.c) is the one that fills the hook in.
    * There is no equivalent for kopper: kopperGetSyncValues() talks to the X server's Present
    * extension directly, its entire body sits inside #ifdef VK_USE_PLATFORM_XCB_KHR, and that define
    * comes from with_platform_x11 - so in a -Dplatforms=orbis build the function compiles to
    * `return 0;`.
    *
    * A real MSC would need three layers this console does not have: wsi_orbis has no vblank source
    * (so->flips counts flip SUBMISSIONS, which is SBC semantics, and sceVideoOutGetFlipStatus reports
    * only numFlipPending); no Vulkan extension carries one here (VK_GOOGLE_display_timing,
    * VK_EXT_display_control and VK_KHR_present_wait are all absent from this ICD); and zink has no
    * entry point for it. Advertising the extension and returning zeros would be worse than not
    * advertising it - an application that trusts EGL_CHROMIUM_sync_control paces itself on the
    * numbers. Leave both unset. */
};

/* Configs. dri2_add_pbuffer_configs_for_visuals() cannot be used: it filters on dri2_pbuffer_visuals[],
 * which is a BGRA-ordered list, and it tags everything EGL_PBUFFER_BIT only - the two things this
 * platform must not do. This loop is the same shape with the two decisions reversed. */
static void
dri2_orbis_add_configs(_EGLDisplay *disp)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);

   for (unsigned i = 0; dri2_dpy->driver_configs[i] != NULL; i++) {
      const struct gl_config *gl_config =
         (const struct gl_config *)dri2_dpy->driver_configs[i];
      bool wanted = false;

      for (unsigned v = 0; v < ARRAY_SIZE(orbis_visuals); v++)
         wanted |= gl_config->color_format == orbis_visuals[v];

      if (!wanted)
         continue;

      /* No multisample to start with. The msaa configs would work - kopper allocates
       * drawable->msaa_textures and resolves into the swapchain image - but every extra config is
       * another way for eglChooseConfig() to hand back something that has never been run on this
       * hardware, and the first console bring-up wants the smallest possible set. */
      if (gl_config->samples > 0)
         continue;

      dri2_add_config(disp, dri2_dpy->driver_configs[i],
                      EGL_WINDOW_BIT | EGL_PBUFFER_BIT, NULL);
   }
}

EGLBoolean
dri2_initialize_orbis(_EGLDisplay *disp)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);

   /* There is no device to probe, no node to open and no driver to guess: this build has exactly one
    * gallium driver linked into it and exactly one Vulkan driver under that. fd_render_gpu stays -1,
    * which is also what dri2_create_screen() will pass to driCreateNewScreen3() and what
    * kopper_update_drawable_info() checks for (`screen->fd == -1`) before letting kopper own the
    * drawable's size. */
   dri2_dpy->fd_render_gpu = -1;
   dri2_dpy->fd_display_gpu = -1;

   dri2_dpy->driver_name = strdup("zink");
   if (!dri2_dpy->driver_name)
      return _eglError(EGL_NOT_INITIALIZED, "orbis-egl: out of memory");

   /* Sets dri2_dpy->kopper from the driver name. It can still come back false, via
    * LIBGL_KOPPER_DISABLE, and there is no second path to fall back to here - no swrast, no
    * llvmpipe, nothing. Fail cleanly and say why rather than proceeding into a screen that cannot
    * present: Probe B recorded that the no-WSI configuration dies by SIGNAL rather than by error,
    * and this is the place to make that impossible. */
   dri2_detect_swrast_kopper(disp);
   if (!dri2_dpy->kopper)
      return _eglError(EGL_NOT_INITIALIZED,
                       "orbis-egl: zink/kopper is the only driver on this "
                       "platform and it was disabled");

   dri2_dpy->loader_extensions = orbis_kopper_loader_extensions;

   if (!dri2_create_screen(disp))
      return _eglError(EGL_NOT_INITIALIZED, "orbis-egl: failed to create screen");

   /* The only _EGLDevice this build can produce is the software one: _eglRefreshDeviceList() enumerates
    * DRM devices and there are none, so _eglGlobal.DeviceList has a single entry. Handing it to
    * disp->Device is a label for EGL_EXT_device_query, not a claim about how anything renders - the
    * rendering is RADV on Liverpool either way. */
   if (!dri2_setup_device(disp, EGL_TRUE))
      return _eglError(EGL_NOT_INITIALIZED, "orbis-egl: failed to set up EGLDevice");

   dri2_setup_screen(disp);

   /* Swap interval is left at the dri2_setup_screen() default of min = max = default = 1, and
    * eglSwapInterval() therefore clamps every request to 1. That is the honest answer rather than a
    * missing feature: the flip is submitted with ORBIS_VIDEO_OUT_FLIP_VSYNC at a rate pinned by
    * sceVideoOutSetFlipRate, and wsi_orbis_wait_for_flip_slot() blocks the producer once it gets
    * ahead. Interval 0 cannot be honoured by anything below us, so it is not offered. */

   dri2_orbis_add_configs(disp);

   /* Fill vtbl last to prevent accidentally calling virtual function during
    * initialization.
    */
   dri2_dpy->vtbl = &dri2_orbis_display_vtbl;

   return EGL_TRUE;
}
