/* glrun - the first OpenGL frame on the console, and the log that says what happened.
 *
 * SPDX-License-Identifier: MIT
 *
 * WHY THIS EXISTS. Everything about OpenGL on this port has been proven on a laptop and nowhere else:
 * the timeline-semaphore emulation against real GFX11 silicon, kopper's headless swapchain against a
 * real 780M, the whole build to a linked 33 MB executable. None of that touched a PS4. The one thing
 * a laptop provably CANNOT do is execute GPU work through this driver - build-hostorbis's submit path
 * is a logging stub with no engine behind it, measured: vkCmdFillBuffer leaves memory untouched there.
 *
 * So this is the first program whose output is evidence. It clears to a known colour, reads the pixel
 * back, and says whether the two match. A matching pixel is the entire claim: it means the GL frontend,
 * zink, kopper, the headless swapchain, wsi_orbis and the GPU all did their part.
 *
 * ⚠ THE FIRST LINE THAT MATTERS IS NOT OURS. wsi_orbis announces which presentation path it took:
 *
 *     scan-out up - WxH pitch P, N swapchain buffer(s), A8B8G8R8_SRGB linear   <- zero copy
 *     scan-out up - ... N GARLIC buffer(s) ... one full-screen copy per frame  <- the fallback
 *     video-out refused the swapchain's own images (0x........)                <- and why
 *
 * Which of those appears settles a question no host rig can answer: whether sceVideoOut accepts the
 * swapchain's own images. Run with MESA_LOG_LEVEL=info and read it.
 *
 * ⚠ THE WINDOW HANDLE IS (EGLNativeWindowType)1 AND IT HAS TO BE NON-ZERO. There is no window on this
 * console - platform_orbis ignores the value entirely - but _eglCreateWindowSurfaceCommon rejects a
 * NULL native window with EGL_BAD_NATIVE_WINDOW before the platform is ever consulted, so "pass
 * nothing" is not available. Any non-zero token works; the core then enforces one window surface per
 * token, which is right when there is exactly one scan-out.
 */
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <ps4_app.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef GLRUN_FRAMES
#define GLRUN_FRAMES 60
#endif

/* Six colours, cycled. Distinct in every channel so a readback that matches by accident is unlikely,
 * and none of them is black - a zero readback is the failure this probe is most likely to see, and it
 * must never be confusable with a frame that legitimately cleared to zero. */
static const struct {
   uint8_t r, g, b;
} colours[] = {
   {  64, 128, 191 }, { 255,   0, 128 }, {   0, 255,  64 },
   { 128, 128,   0 }, { 191,  64, 255 }, {  32,  32, 255 },
};

static const char *
egl_err(void)
{
   switch (eglGetError()) {
   case EGL_SUCCESS:             return "EGL_SUCCESS";
   case EGL_NOT_INITIALIZED:     return "EGL_NOT_INITIALIZED";
   case EGL_BAD_ACCESS:          return "EGL_BAD_ACCESS";
   case EGL_BAD_ALLOC:           return "EGL_BAD_ALLOC";
   case EGL_BAD_ATTRIBUTE:       return "EGL_BAD_ATTRIBUTE";
   case EGL_BAD_CONFIG:          return "EGL_BAD_CONFIG";
   case EGL_BAD_CONTEXT:         return "EGL_BAD_CONTEXT";
   case EGL_BAD_DISPLAY:         return "EGL_BAD_DISPLAY";
   case EGL_BAD_MATCH:           return "EGL_BAD_MATCH";
   case EGL_BAD_NATIVE_WINDOW:   return "EGL_BAD_NATIVE_WINDOW";
   case EGL_BAD_PARAMETER:       return "EGL_BAD_PARAMETER";
   case EGL_BAD_SURFACE:         return "EGL_BAD_SURFACE";
   case EGL_CONTEXT_LOST:        return "EGL_CONTEXT_LOST";
   default:                      return "EGL_<unknown>";
   }
}

/* A string that may be NULL, for a log line that must not be. glGetString returning NULL is itself a
 * finding - it means the context is not current or the enum is unsupported - so it is reported rather
 * than papered over with "". */
static const char *
str_or(const GLubyte *s, const char *fallback)
{
   return s != NULL ? (const char *)s : fallback;
}

int
main(void)
{
   ps4_app_init("glrun", "gl-first-frame");

   ps4_log("start - EGL/GLES2 over zink over RADV, %d frames", GLRUN_FRAMES);

   EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
   if (dpy == EGL_NO_DISPLAY) {
      ps4_log_fatal("eglGetDisplay returned EGL_NO_DISPLAY (%s)", egl_err());
      ps4_idle_forever("no display");
      return 1;
   }

   EGLint major = -1, minor = -1;
   if (!eglInitialize(dpy, &major, &minor)) {
      ps4_log_fatal("eglInitialize failed (%s)", egl_err());
      ps4_idle_forever("no init");
      return 1;
   }
   ps4_log("eglInitialize -> EGL %d.%d", (int)major, (int)minor);
   ps4_log("EGL_VENDOR      = %s", eglQueryString(dpy, EGL_VENDOR));
   ps4_log("EGL_VERSION     = %s", eglQueryString(dpy, EGL_VERSION));
   ps4_log("EGL_CLIENT_APIS = %s", eglQueryString(dpy, EGL_CLIENT_APIS));

   /* No EGL_ALPHA_SIZE, no depth, no stencil: this probe draws nothing but a clear, and every
    * attribute asked for is one more way for eglChooseConfig to come back empty on the first run of a
    * platform that has never run. Depth belongs in the next probe, not this one. */
   static const EGLint cfg_attrs[] = {
      EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_RED_SIZE,        8,
      EGL_GREEN_SIZE,      8,
      EGL_BLUE_SIZE,       8,
      EGL_NONE
   };
   EGLConfig cfg = NULL;
   EGLint n_cfg = 0;
   if (!eglChooseConfig(dpy, cfg_attrs, &cfg, 1, &n_cfg) || n_cfg < 1) {
      ps4_log_fatal("eglChooseConfig gave %d configs (%s)", (int)n_cfg, egl_err());
      ps4_idle_forever("no config");
      return 1;
   }
   EGLint cfg_id = -1, cfg_r = -1, cfg_g = -1, cfg_b = -1, cfg_a = -1;
   eglGetConfigAttrib(dpy, cfg, EGL_CONFIG_ID, &cfg_id);
   eglGetConfigAttrib(dpy, cfg, EGL_RED_SIZE, &cfg_r);
   eglGetConfigAttrib(dpy, cfg, EGL_GREEN_SIZE, &cfg_g);
   eglGetConfigAttrib(dpy, cfg, EGL_BLUE_SIZE, &cfg_b);
   eglGetConfigAttrib(dpy, cfg, EGL_ALPHA_SIZE, &cfg_a);
   ps4_log("config id %d, RGBA %d%d%d%d, %d matched", (int)cfg_id,
           (int)cfg_r, (int)cfg_g, (int)cfg_b, (int)cfg_a, (int)n_cfg);

   /* See the header note: non-zero and otherwise ignored. */
   EGLSurface surf = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)1, NULL);
   if (surf == EGL_NO_SURFACE) {
      ps4_log_fatal("eglCreateWindowSurface failed (%s)", egl_err());
      ps4_idle_forever("no surface");
      return 1;
   }
   EGLint sw = -1, sh = -1;
   eglQuerySurface(dpy, surf, EGL_WIDTH, &sw);
   eglQuerySurface(dpy, surf, EGL_HEIGHT, &sh);
   ps4_log("window surface %dx%d", (int)sw, (int)sh);

   if (!eglBindAPI(EGL_OPENGL_ES_API)) {
      ps4_log_fatal("eglBindAPI(ES) failed (%s)", egl_err());
      ps4_idle_forever("no api");
      return 1;
   }

   static const EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
   EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
   if (ctx == EGL_NO_CONTEXT) {
      ps4_log_fatal("eglCreateContext failed (%s)", egl_err());
      ps4_idle_forever("no context");
      return 1;
   }
   if (!eglMakeCurrent(dpy, surf, surf, ctx)) {
      ps4_log_fatal("eglMakeCurrent failed (%s)", egl_err());
      ps4_idle_forever("not current");
      return 1;
   }
   ps4_log("context current");

   /* The four strings that identify what is actually rendering. GL_RENDERER is the one to read: it
    * should name zink over RADV over Liverpool, which is the whole stack in one line. */
   ps4_log("GL_VENDOR   = %s", str_or(glGetString(GL_VENDOR), "(null)"));
   ps4_log("GL_RENDERER = %s", str_or(glGetString(GL_RENDERER), "(null)"));
   ps4_log("GL_VERSION  = %s", str_or(glGetString(GL_VERSION), "(null)"));
   ps4_log("GLSL        = %s", str_or(glGetString(GL_SHADING_LANGUAGE_VERSION), "(null)"));

   unsigned matched = 0, mismatched = 0, swap_failed = 0;

   for (int i = 0; i < GLRUN_FRAMES; i++) {
      const unsigned c = (unsigned)i % (sizeof(colours) / sizeof(colours[0]));
      const uint8_t want_r = colours[c].r, want_g = colours[c].g, want_b = colours[c].b;

      glClearColor((float)want_r / 255.0f, (float)want_g / 255.0f, (float)want_b / 255.0f, 1.0f);
      glClear(GL_COLOR_BUFFER_BIT);

      /* ⚠ READ BACK BEFORE THE SWAP, and finish first. After eglSwapBuffers the back buffer is a
       * different image and the answer would be about whichever one the swapchain handed us next.
       * glFinish rather than glFlush because the readback has to observe completed work, and this is
       * a probe - a stall per frame costs nothing and removes a whole class of doubt. */
      glFinish();

      uint8_t px[4] = { 0, 0, 0, 0 };
      glReadPixels(sw / 2, sh / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);

      const GLenum gl_err = glGetError();
      const int ok = px[0] == want_r && px[1] == want_g && px[2] == want_b;
      if (ok)
         matched++;
      else
         mismatched++;

      /* ⚠ THE FIRST FRAME AND EVERY MISMATCH GO TO KLOG TOO. ps4_log_frame is UDP-only and safe in a
       * loop; ps4_log also writes klog, which costs 8-15 ms a line on this console. A run where the
       * network never came up still has to be able to say what frame 0 did, and a mismatch is worth
       * the stall wherever it happens. The rest of the frames are UDP only. */
      if (i == 0 || !ok) {
         ps4_log("frame %d  readback = %3u %3u %3u %3u  want %3u %3u %3u  %s%s",
                 i, px[0], px[1], px[2], px[3], want_r, want_g, want_b,
                 ok ? "MATCH" : "MISMATCH",
                 gl_err != GL_NO_ERROR ? "  (glGetError set)" : "");
         /* Named separately because a red/blue swap is the specific failure this port has a standing
          * hypothesis about: the scan-out attribute is hardcoded A8B8G8R8_SRGB and nothing validates
          * the swapchain format against it, so a B-first config produces exactly this and no error. */
         if (!ok && px[0] == want_b && px[2] == want_r)
            ps4_log("frame %d  ^ red and blue are exchanged - the swapchain format is B-first", i);
      } else {
         ps4_log_frame("frame %d  readback = %3u %3u %3u  MATCH", i, px[0], px[1], px[2]);
      }

      if (!eglSwapBuffers(dpy, surf)) {
         swap_failed++;
         ps4_log("frame %d  eglSwapBuffers FAILED (%s)", i, egl_err());
      }
   }

   ps4_log("RESULT: %u matched, %u mismatched, %u swap failure(s) over %d frame(s)",
           matched, mismatched, swap_failed, GLRUN_FRAMES);
   ps4_log("VERDICT: %s",
           (matched == GLRUN_FRAMES && swap_failed == 0) ? "OK - OpenGL renders and presents"
           : matched > 0            ? "PARTIAL - see the mismatched frames above"
                                    : "FAIL - nothing rendered correctly");

   /* Teardown is part of the test, not tidiness: wsi_headless_swapchain_destroy is where two of the
    * fifteen review findings lived, and its console behaviour has never been observed. The log lines
    * after this point - "scan-out down after N flip(s)" - are the ones that say it survived. */
   eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
   eglDestroyContext(dpy, ctx);
   eglDestroySurface(dpy, surf);
   eglTerminate(dpy);
   ps4_log("teardown returned");

   ps4_idle_forever("glrun done");
   return 0;
}
