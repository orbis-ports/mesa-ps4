/*
 * Copyright 2026 Mikołaj Mikołajczyk
 * SPDX-License-Identifier: MIT
 *
 * kopperprobe - drive a GL swapchain through a VK_EXT_headless_surface, and prove it by the pixels.
 *
 * WHY THIS EXISTS. The presentation design for this port is
 *
 *     GL swapchain -> kopper -> vkCreateHeadlessSurfaceEXT -> VK_EXT_headless_surface -> wsi_orbis
 *       -> sceVideoOut flip
 *
 * and every arrow in it was an ASSUMPTION. The middle one was the dangerous assumption: kopper - zink's
 * GL-side swapchain manager - only ever created xcb, wayland and win32 surfaces. Nothing upstream has
 * ever asked it for a headless one, so nothing upstream had ever found out whether it can. If it could
 * not, the whole design was wrong, and the cost of learning that after someone had written a PS4 EGL
 * platform is a great deal larger than the cost of learning it here.
 *
 * ⚠ THIS PROBE ONLY MEANS ANYTHING AGAINST A PATCHED MESA. It needs two things that are not upstream:
 *
 *   1. The KOPPER_HEADLESS arm in src/gallium/drivers/zink/zink_kopper.c - the REAL change, ~45 lines,
 *      an enum value plus the ten cdt->type sites that branch on it.
 *   2. A loader that actually asks for a headless surface. There is no such loader, so the probe is
 *      driven by ZINK_PROBE_HEADLESS in src/egl/drivers/dri2/platform_x11.c, which swaps the sType and
 *      changes nothing else. That hunk is a TEST HARNESS AND MUST NOT SHIP; the console wants a real
 *      PS4 EGL platform that fills in VkHeadlessSurfaceCreateInfoEXT because that is what it presents
 *      through.
 *
 * WHY AN X11 WINDOW, ON A PROBE ABOUT NOT HAVING A WINDOW SYSTEM. Because the alternative could not
 * answer the question. Surfaceless EGL implements only create_pbuffer_surface, and eglSwapBuffers on a
 * pbuffer is a no-op by specification - so it never presents, and present is the entire question. The
 * X11 window is a vehicle for obtaining a window-type EGL surface with a real swap loop. NOTHING IS
 * DISPLAYED IN THE HEADLESS CASE: the headless WSI backend consumes the presented image. That is also
 * why this probe checks pixels rather than looking at the screen - the screen is not the evidence.
 *
 * WHY SIX FRAMES AND SIX DIFFERENT COLOURS. wsi_common_headless.c reports minImageCount = 4. One frame
 * would prove only that a single image can be rendered into; four would sit exactly on the boundary.
 * Six distinct colours, each cleared, read back, then swapped, prove that acquire/present genuinely
 * ROTATES through the swapchain and hands back a different image rather than the same one repeatedly.
 * A stale image shows up immediately as the previous frame's colour.
 *
 * WHAT IT MEASURED, so the next person does not have to re-run it to know:
 *
 *   - GFX11 (AMD 780M, system RADV), headless: all six readbacks exact. Kopper drives a headless
 *     swapchain. Under VK_LAYER_KHRONOS_validation it produces the same single VUID
 *     (VUID-vkQueueSubmit-pSignalSemaphores-00067) as the unmodified xcb path, i.e. the headless arm
 *     introduces no new validation error.
 *   - Orbis winsys (build.sh --host-orbis ICD, plus ZINK_PROBE_NO_TIMELINE): the whole chain executed -
 *     kopper created the headless surface, wsi_orbis brought its scan-out up at 4 buffers, and six
 *     flips went through. Pixels read back zero, which is the RIG, not the design: that ICD is backed
 *     by memfd with a no-op submit, so glClear never writes anything on this host. Every step that does
 *     not require a GPU to execute succeeded.
 *
 * HOW TO RUN IT. Needs a zink build and its devenv; see the meson devenv notes in the probe report, or:
 *
 *     export LD_LIBRARY_PATH=<build>/src/gbm:<build>/src/egl:<build>/src/glx:<build>/src/gallium/targets/dri
 *     export __EGL_VENDOR_LIBRARY_FILENAMES=<build>/src/egl/devenv_glvnd_egl.json
 *     export MESA_LOADER_DRIVER_OVERRIDE=zink DISPLAY=:0
 *
 *     kopperprobe                          # control: ordinary xcb swapchain, must pass
 *     ZINK_PROBE_HEADLESS=1 kopperprobe    # the question: headless swapchain
 *
 * Run the control FIRST. It is the same binary and the same expected pixels, so if it fails the
 * headless result means nothing - exactly the discipline the rest of this port's probes follow.
 *
 *     cc -O1 -o kopperprobe kopperprobe.c -lEGL -lGLESv2 -lX11
 */
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <X11/Xlib.h>

#include <stdio.h>
#include <stdlib.h>

static int fail = 0;

/* Read one pixel of the back buffer and hold it against what was cleared. This is the whole proof:
 * a headless swapchain image is an ordinary VkImage, so getting the right colour out of it means the
 * GL frontend, kopper's acquire, the render and the swapchain image all agree. */
static void
check(const char *what, int frame, int r, int g, int b, int a)
{
   unsigned char px[4] = {0, 0, 0, 0};
   glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
   const int ok = px[0] == r && px[1] == g && px[2] == b && px[3] == a;
   printf("kopperprobe:   frame %d %-12s readback = %3u %3u %3u %3u  expect %3d %3d %3d %3d  %s\n",
          frame, what, px[0], px[1], px[2], px[3], r, g, b, a, ok ? "MATCH" : "*** MISMATCH ***");
   fflush(stdout);
   if (!ok)
      fail = 1;
}

int
main(void)
{
   Display *xdpy = XOpenDisplay(NULL);
   if (!xdpy) {
      printf("kopperprobe: XOpenDisplay failed - this probe needs an X display for its vehicle window\n");
      return 1;
   }
   Window win = XCreateSimpleWindow(xdpy, DefaultRootWindow(xdpy), 0, 0, 64, 64, 0, 0, 0);
   XMapWindow(xdpy, win);
   XSync(xdpy, False);
   printf("kopperprobe: x11 window 0x%lx mapped (vehicle only; nothing is displayed when headless)\n",
          (unsigned long)win);

   EGLDisplay dpy = eglGetDisplay((EGLNativeDisplayType)xdpy);
   EGLint maj, min;
   if (!eglInitialize(dpy, &maj, &min)) {
      printf("kopperprobe: eglInitialize failed\n");
      return 1;
   }
   printf("kopperprobe: egl %d.%d\n", maj, min);
   eglBindAPI(EGL_OPENGL_ES_API);

   /* EGL_WINDOW_BIT is load-bearing: a pbuffer config would give us a surface whose eglSwapBuffers is
    * a specified no-op, and then the probe would pass without ever presenting anything. */
   const EGLint cfg_attrs[] = {
      EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
      EGL_RED_SIZE,        8,
      EGL_GREEN_SIZE,      8,
      EGL_BLUE_SIZE,       8,
      EGL_NONE
   };
   EGLConfig cfg;
   EGLint n;
   if (!eglChooseConfig(dpy, cfg_attrs, &cfg, 1, &n) || n < 1) {
      printf("kopperprobe: eglChooseConfig found no window-capable config\n");
      return 1;
   }

   EGLSurface surf = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)win, NULL);
   if (surf == EGL_NO_SURFACE) {
      printf("kopperprobe: eglCreateWindowSurface failed, egl error 0x%x\n", eglGetError());
      return 1;
   }
   printf("kopperprobe: window surface created\n");

   const EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
   EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
   if (ctx == EGL_NO_CONTEXT) {
      printf("kopperprobe: eglCreateContext failed, egl error 0x%x\n", eglGetError());
      return 1;
   }
   if (!eglMakeCurrent(dpy, surf, surf, ctx)) {
      printf("kopperprobe: eglMakeCurrent failed, egl error 0x%x\n", eglGetError());
      return 1;
   }
   printf("kopperprobe: context current\n");
   printf("kopperprobe: GL_VERSION  = %s\n", (const char *)glGetString(GL_VERSION));
   printf("kopperprobe: GL_RENDERER = %s\n", (const char *)glGetString(GL_RENDERER));
   fflush(stdout);

   /* Six colours chosen so every channel differs between consecutive frames - a swapchain that handed
    * back a stale image would show the previous row of this table, not a near miss. The expected bytes
    * are the float clear colours rounded the way an 8-bit UNORM target rounds them. */
   static const float colour[6][4] = {
      { 0.25f, 0.50f, 0.75f, 1.0f }, { 1.00f, 0.00f, 0.50f, 1.0f }, { 0.00f, 1.00f, 0.25f, 1.0f },
      { 0.50f, 0.50f, 0.00f, 1.0f }, { 0.75f, 0.25f, 1.00f, 1.0f }, { 0.00f, 0.00f, 1.00f, 1.0f },
   };
   static const int expect[6][4] = {
      {  64, 128, 191, 255 }, { 255,   0, 128, 255 }, {   0, 255,  64, 255 },
      { 128, 128,   0, 255 }, { 191,  64, 255, 255 }, {   0,   0, 255, 255 },
   };

   for (int f = 0; f < 6; f++) {
      glClearColor(colour[f][0], colour[f][1], colour[f][2], colour[f][3]);
      glClear(GL_COLOR_BUFFER_BIT);
      /* glFinish before the readback so a mismatch means the wrong image, not an unfinished one. */
      glFinish();
      check("post-clear", f, expect[f][0], expect[f][1], expect[f][2], expect[f][3]);

      if (!eglSwapBuffers(dpy, surf)) {
         printf("kopperprobe: eglSwapBuffers failed on frame %d, egl error 0x%x\n", f, eglGetError());
         return 1;
      }
      printf("kopperprobe:   frame %d swapped OK\n", f);
      fflush(stdout);
   }

   eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
   eglDestroySurface(dpy, surf);
   eglDestroyContext(dpy, ctx);
   eglTerminate(dpy);

   printf("kopperprobe: %s\n", fail ? "PIXEL MISMATCH - the swapchain did not hand back what we rendered"
                                    : "OK - six frames rendered, read back and presented");
   return fail;
}
