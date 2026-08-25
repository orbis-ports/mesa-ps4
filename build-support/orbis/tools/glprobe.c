/* Link a minimal PS4 executable against the built OpenGL ES stack.
 *
 * This is the GL sibling of linkprobe.c, and it exists for the same reason: an archive is not a driver
 * until something links it. The calls below are chosen to drag in the whole chain rather than to run -
 * eglInitialize reaches the EGL platform code, dri2_create_screen and the DRI frontend; glClear and
 * glGetString reach the GLES entry-point library and, through it, the mapi dispatch that libgallium
 * carries. Nothing here is executed by the build; the link is the test.
 *
 * ⚠ THE SEQUENCE IS THE FAZA-4 SEQUENCE, NOT AN ARBITRARY ONE. eglInitialize alone would leave
 * platform_orbis.c's surface, context and swap paths - which is most of the file - unreferenced, and
 * an unreferenced object file in an archive is an object file the linker never has to resolve. Every
 * call below therefore corresponds to one step of the gate this arm is meant to prove:
 *
 *     eglGetDisplay -> eglInitialize -> eglChooseConfig -> eglCreateWindowSurface
 *                   -> eglCreateContext -> eglMakeCurrent -> glClear -> eglSwapBuffers
 *
 * ⚠ THE WINDOW ARGUMENT IS DELIBERATELY NON-NULL. _eglCreateWindowSurfaceCommon() rejects a NULL
 * native_window with EGL_BAD_NATIVE_WINDOW before the platform is ever consulted, so "there is no
 * window handle on this console" has to be spelled "pass any non-zero token"; platform_orbis.c ignores
 * the value and takes its size from the video mode. This probe uses 1, which is the convention.
 */
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES/gl.h>
#include <stdio.h>

int main(void)
{
   EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
   EGLint maj = 0, min = 0, num_configs = 0;
   EGLConfig config;
   EGLSurface surf;
   EGLContext ctx;

   static const EGLint config_attribs[] = {
      EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_RED_SIZE,        8,
      EGL_GREEN_SIZE,      8,
      EGL_BLUE_SIZE,       8,
      EGL_ALPHA_SIZE,      8,
      EGL_DEPTH_SIZE,      24,
      EGL_NONE,
   };
   static const EGLint context_attribs[] = {
      EGL_CONTEXT_CLIENT_VERSION, 2,
      EGL_NONE,
   };

   if (dpy == EGL_NO_DISPLAY)
      return 1;
   if (!eglInitialize(dpy, &maj, &min))
      return 2;

   printf("EGL %d.%d %s\n", maj, min, eglQueryString(dpy, EGL_VENDOR));

   if (!eglChooseConfig(dpy, config_attribs, &config, 1, &num_configs) ||
       num_configs < 1)
      return 3;

   surf = eglCreateWindowSurface(dpy, config, (EGLNativeWindowType)1, NULL);
   if (surf == EGL_NO_SURFACE)
      return 4;

   eglBindAPI(EGL_OPENGL_ES_API);
   ctx = eglCreateContext(dpy, config, EGL_NO_CONTEXT, context_attribs);
   if (ctx == EGL_NO_CONTEXT)
      return 5;

   if (!eglMakeCurrent(dpy, surf, surf, ctx))
      return 6;

   printf("GL %s / %s\n", (const char *)glGetString(GL_VERSION),
          (const char *)glGetString(GL_RENDERER));

   glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
   eglSwapInterval(dpy, 1);
   if (!eglSwapBuffers(dpy, surf))
      return 7;

   /* ⚠ THE ES1 CALLS ARE HERE TO BE LINKED, NOT TO BE RUN, AND THE ARCHIVE WAS LISTED WITHOUT THEM
    * FOR ONE BUILD. libGLESv1_CM.a sits inside --start-group, so the linker pulls only the members
    * something references; with nothing in this file naming an ES1 entry point, the archive was on
    * the line and never opened, and the probe proved exactly nothing about it. A gate that names a
    * library it does not exercise is worse than one that does not mention it - it reads as coverage.
    *
    * Guarded on the current API so a run of this probe cannot end up calling fixed-function entry
    * points against an ES2 context; the link is what matters and it happens either way. */
   if (eglQueryAPI() == EGL_OPENGL_ES_API) {
      glAlphaFunc(GL_GREATER, 0.0f);
      glLoadIdentity();
      glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
      glOrthof(-1.0f, 1.0f, -1.0f, 1.0f, -1.0f, 1.0f);
   }

   eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
   eglDestroyContext(dpy, ctx);
   eglDestroySurface(dpy, surf);
   eglTerminate(dpy);
   return 0;
}
