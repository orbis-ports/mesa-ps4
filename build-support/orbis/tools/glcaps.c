/* glcaps - what GL version this driver actually gives on the console, asked properly.
 *
 * SPDX-License-Identifier: MIT
 *
 * WHY THIS EXISTS. glrun and gltri both reported "OpenGL ES 3.1", and that number has been quoted
 * since. It is not a ceiling: both ask for EGL_CONTEXT_CLIENT_VERSION 2 and get whatever the driver
 * considers adequate for that request. Nobody has ever asked this driver for its best.
 *
 * The caps measured on the laptop say GL 4.6 / GLSL 4.60 / GLES 3.2, on the Orbis winsys and on a
 * GFX7 forced through drm-shim alike. Those rigs execute no GPU work, so what they read is the
 * driver's advertisement, not silicon. This asks the silicon.
 *
 * ⚠ IT ASKS DOWNWARDS AND REPORTS EVERY ANSWER, rather than asking once and printing the result. A
 * single failed request tells you nothing about where the ceiling is; the first version that
 * succeeds, with the failures above it named, is the ceiling.
 *
 * ⚠ AND IT ASKS FOR DESKTOP GL SEPARATELY. EGL advertises EGL_CLIENT_APIS = "OpenGL OpenGL_ES" here,
 * but the build exports no desktop entry points at all - glx and glvnd are both off, so there is no
 * libGL to link. Whether a desktop CONTEXT can be created is a different question from whether a
 * title could call anything on it, and the answer decides whether RetroArch's glcore driver is
 * reachable at all. Everything below goes through eglGetProcAddress for exactly that reason.
 */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <ps4_app.h>
#include <stddef.h>
#include <stdint.h>

#ifndef EGL_OPENGL_ES3_BIT_KHR
#define EGL_OPENGL_ES3_BIT_KHR 0x00000040
#endif
/* ES3 tokens, spelled out rather than pulled from <GLES3/gl3.h>: this probe links the ES2 entry
 * points like every other consumer here, and the one indexed query it wants comes through
 * eglGetProcAddress anyway. */
#ifndef GL_NUM_EXTENSIONS
#define GL_NUM_EXTENSIONS 0x821D
#endif

typedef const GLubyte *(*PFN_glGetStringi)(GLenum, GLuint);

static EGLDisplay dpy;
static EGLSurface surf = EGL_NO_SURFACE;

static const char *
s_or(const GLubyte *s)
{
   return s != NULL ? (const char *)s : "(null)";
}

/* One attempt at one API and version. Reports what came back, or why not. Returns 1 on success. */
static int
try_context(EGLenum api, const char *api_name, int major, int minor, EGLint renderable_bit)
{
   const EGLint cfg_attrs[] = {
      EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
      EGL_RENDERABLE_TYPE, renderable_bit,
      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
      EGL_NONE
   };
   EGLConfig cfg = NULL;
   EGLint n = 0;
   if (!eglChooseConfig(dpy, cfg_attrs, &cfg, 1, &n) || n < 1) {
      ps4_log("%-14s %d.%d  no config with that renderable type", api_name, major, minor);
      return 0;
   }

   if (!eglBindAPI(api)) {
      ps4_log("%-14s %d.%d  eglBindAPI refused (0x%04x)", api_name, major, minor, eglGetError());
      return 0;
   }

   /* EGL 1.5 spelling. EGL_CONTEXT_MAJOR_VERSION is the same token as the old
    * EGL_CONTEXT_CLIENT_VERSION, so this is also correct against a 1.4 implementation - the minor is
    * the part that needs 1.5, and the console reported EGL 1.5. */
   const EGLint ctx_attrs[] = {
      EGL_CONTEXT_MAJOR_VERSION, major,
      EGL_CONTEXT_MINOR_VERSION, minor,
      EGL_NONE
   };
   EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
   if (ctx == EGL_NO_CONTEXT) {
      ps4_log("%-14s %d.%d  eglCreateContext refused (0x%04x)", api_name, major, minor, eglGetError());
      return 0;
   }
   if (!eglMakeCurrent(dpy, surf, surf, ctx)) {
      ps4_log("%-14s %d.%d  eglMakeCurrent refused (0x%04x)", api_name, major, minor, eglGetError());
      eglDestroyContext(dpy, ctx);
      return 0;
   }

   /* ⚠ THE STRING IS THE ANSWER, NOT THE REQUEST. A driver may hand back a context newer than asked
    * for, and this one already did once - glrun asked for ES 2 and got ES 3.1. */
   ps4_log("%-14s %d.%d  OK -> %s | %s", api_name, major, minor,
           s_or(glGetString(GL_VERSION)), s_or(glGetString(GL_SHADING_LANGUAGE_VERSION)));

   /* Extension count, via the ES3/GL3 indexed form when it is there. Fetched through
    * eglGetProcAddress because nothing in this build statically exports it. */
   PFN_glGetStringi getstringi = (PFN_glGetStringi)eglGetProcAddress("glGetStringi");
   if (getstringi != NULL) {
      GLint next = 0;
      glGetIntegerv(GL_NUM_EXTENSIONS, &next);
      if (glGetError() == GL_NO_ERROR && next > 0)
         ps4_log("               %d extensions", (int)next);
   } else {
      const GLubyte *e = glGetString(GL_EXTENSIONS);
      if (e != NULL) {
         int count = 1;
         for (const char *p = (const char *)e; *p != '\0'; p++)
            if (*p == ' ') count++;
         ps4_log("               ~%d extensions (string form)", count);
      }
   }

   eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
   eglDestroyContext(dpy, ctx);
   return 1;
}

int
main(void)
{
   ps4_app_init("glcaps", "gl-ceiling");

   dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
   EGLint major = 0, minor = 0;
   if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &major, &minor)) {
      ps4_log_fatal("no EGL display");
      ps4_idle_forever("no egl");
      return 1;
   }
   ps4_log("EGL %d.%d, client APIs: %s", (int)major, (int)minor,
           eglQueryString(dpy, EGL_CLIENT_APIS));
   ps4_log("EGL extensions: %s", eglQueryString(dpy, EGL_EXTENSIONS));

   /* One surface, reused by every context: the question is about contexts, and a fresh surface per
    * attempt would put swapchain creation in the way of the answer. */
   const EGLint cfg_attrs[] = {
      EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE
   };
   EGLConfig cfg = NULL;
   EGLint n = 0;
   eglChooseConfig(dpy, cfg_attrs, &cfg, 1, &n);
   surf = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)1, NULL);
   if (surf == EGL_NO_SURFACE) {
      ps4_log_fatal("no window surface (0x%04x)", eglGetError());
      ps4_idle_forever("no surface");
      return 1;
   }

   ps4_log("--- OpenGL ES, highest first ---");
   int es_best_major = 0, es_best_minor = 0;
   static const struct { int mj, mn; EGLint bit; } es[] = {
      { 3, 2, EGL_OPENGL_ES3_BIT_KHR }, { 3, 1, EGL_OPENGL_ES3_BIT_KHR },
      { 3, 0, EGL_OPENGL_ES3_BIT_KHR }, { 2, 0, EGL_OPENGL_ES2_BIT },
   };
   for (unsigned i = 0; i < sizeof(es) / sizeof(es[0]); i++) {
      if (try_context(EGL_OPENGL_ES_API, "OpenGL ES", es[i].mj, es[i].mn, es[i].bit) &&
          es_best_major == 0) {
         es_best_major = es[i].mj;
         es_best_minor = es[i].mn;
      }
   }

   ps4_log("--- desktop OpenGL ---");
   int gl_ok = 0;
   static const struct { int mj, mn; } gl[] = { { 4, 6 }, { 4, 5 }, { 3, 3 }, { 3, 2 }, { 2, 1 } };
   for (unsigned i = 0; i < sizeof(gl) / sizeof(gl[0]); i++)
      if (try_context(EGL_OPENGL_API, "OpenGL", gl[i].mj, gl[i].mn, EGL_OPENGL_BIT) && !gl_ok)
         gl_ok = 1;

   ps4_log("CEILING: OpenGL ES %d.%d; desktop GL context %s",
           es_best_major, es_best_minor, gl_ok ? "CREATED" : "not available");
   ps4_log("⚠ a desktop context being creatable is not the same as being usable: this build exports "
           "no desktop entry points (glx and glvnd are both off), so they would have to come from "
           "eglGetProcAddress");

   eglDestroySurface(dpy, surf);
   eglTerminate(dpy);
   ps4_idle_forever("glcaps done");
   return 0;
}
