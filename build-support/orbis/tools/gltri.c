/* gltri - a real triangle: shaders, a vertex buffer, rasterisation, and pixels checked against maths.
 *
 * SPDX-License-Identifier: MIT
 *
 * WHY THIS EXISTS, GIVEN THAT glrun.c ALREADY PASSED. glrun proved the frame gets to the screen:
 * EGL, zink, kopper, the headless swapchain, wsi_orbis, the flip. It proved almost nothing about the
 * GPU, because glClear is a fixed-function path that never compiles a shader and never rasterises a
 * primitive. Everything interesting on this console is on the other side of that line:
 *
 *     GLSL ES -> Mesa's GLSL frontend -> NIR -> zink's nir_to_spirv -> SPIR-V
 *             -> RADV -> ACO -> GFX7 machine code -> the CP -> the rasteriser
 *
 * None of that has ever executed here. ACO on GFX7 is the least-travelled part of it: RADV supports
 * the generation, but a shader arriving as SPIR-V produced by zink from GLSL is not the shape RADV's
 * GFX7 support was exercised with.
 *
 * HOW IT AVOIDS PROVING NOTHING. A triangle that "looks right" is worth as much as a screen that
 * flickered. This checks pixels at coordinates derived from the geometry:
 *
 *   pass 1  a flat colour from a uniform. Inside the triangle the answer is that exact colour and
 *           outside it is the clear colour, both to the byte. No interpolation, so no tolerance is
 *           needed and none is allowed - this pass is the one that says the rasteriser puts the
 *           primitive where the maths says it goes.
 *   pass 2  per-vertex colour through a varying. Now interpolation is the thing under test, so the
 *           check is a tolerance around the value computed from barycentric weights on the host.
 *           A gradient that renders but interpolates wrong passes pass 1 and fails this.
 *
 * ⚠ THE SAMPLE POINTS ARE COMPUTED, NOT EYEBALLED. Each is stated with the NDC coordinate it comes
 * from and whether it is inside, so a failure names a geometric fact rather than a pixel.
 *
 * ⚠ #version 100 ON PURPOSE. The context this asks for is ES 2.0 (EGL_CONTEXT_CLIENT_VERSION 2);
 * glrun observed that the driver hands back ES 3.1 anyway. Writing to the oldest dialect keeps the
 * variable under test the DRIVER rather than the dialect, and it is the dialect libretro cores use.
 */
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <ps4_app.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define W 1920
#define H 1080

/* The triangle, in normalised device coordinates. Deliberately not centred on anything convenient:
 * a symmetric shape can hide a swapped axis. */
static const float verts[] = {
   /*  x      y      r     g     b   */
   -0.8f, -0.8f,   1.0f, 0.0f, 0.0f,
    0.8f, -0.8f,   0.0f, 1.0f, 0.0f,
    0.0f,  0.8f,   0.0f, 0.0f, 1.0f,
};

static const char *vs_src =
   "#version 100\n"
   "attribute vec2 a_pos;\n"
   "attribute vec3 a_col;\n"
   "varying vec3 v_col;\n"
   "void main() {\n"
   "   v_col = a_col;\n"
   "   gl_Position = vec4(a_pos, 0.0, 1.0);\n"
   "}\n";

/* u_flat < 0.5 selects the varying, so one program covers both passes and pass 2 cannot accidentally
 * be testing a different pipeline than pass 1. */
static const char *fs_src =
   "#version 100\n"
   "precision mediump float;\n"
   "varying vec3 v_col;\n"
   "uniform vec3 u_solid;\n"
   "uniform float u_flat;\n"
   "void main() {\n"
   "   gl_FragColor = vec4(u_flat > 0.5 ? u_solid : v_col, 1.0);\n"
   "}\n";

/* Screen pixel from an NDC coordinate. glReadPixels' origin is bottom-left, which is also NDC's
 * orientation, so this is the plain viewport transform with no flip - and the absence of a flip here
 * is exactly what a red/blue-style axis bug would show up as. */
static int px_x(float ndc_x) { return (int)((ndc_x * 0.5f + 0.5f) * (float)W); }
static int px_y(float ndc_y) { return (int)((ndc_y * 0.5f + 0.5f) * (float)H); }

/* Barycentric weights of an NDC point against the triangle above. Used to predict what pass 2 must
 * produce, on the host, from the same numbers the GPU is given. */
static void
bary(float x, float y, float *w0, float *w1, float *w2)
{
   const float x0 = verts[0], y0 = verts[1];
   const float x1 = verts[5], y1 = verts[6];
   const float x2 = verts[10], y2 = verts[11];
   const float d = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
   *w0 = ((y1 - y2) * (x - x2) + (x2 - x1) * (y - y2)) / d;
   *w1 = ((y2 - y0) * (x - x2) + (x0 - x2) * (y - y2)) / d;
   *w2 = 1.0f - *w0 - *w1;
}

static const struct {
   const char *name;
   float ndc_x, ndc_y;
   int inside;
} points[] = {
   { "centroid",      0.00f, -0.267f, 1 },
   { "low-centre",    0.00f, -0.60f,  1 },
   { "near-apex",     0.00f,  0.60f,  1 },
   { "left-inner",   -0.35f, -0.40f,  1 },
   { "right-inner",   0.35f, -0.40f,  1 },
   { "top-left",     -0.90f,  0.90f,  0 },
   { "top-right",     0.90f,  0.90f,  0 },
   { "below-edge",    0.00f, -0.95f,  0 },
   { "far-left",     -0.95f,  0.00f,  0 },
};
#define NPOINTS (int)(sizeof(points) / sizeof(points[0]))

static GLuint
compile(GLenum type, const char *src, const char *what)
{
   GLuint sh = glCreateShader(type);
   glShaderSource(sh, 1, &src, NULL);
   glCompileShader(sh);

   GLint ok = 0;
   glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);

   /* ⚠ THE LOG IS READ ON SUCCESS TOO. A shader that compiles with a warning is a shader whose
    * warning nobody will ever see again; on a target where this compiler has never run, that line is
    * worth the two calls it costs. */
   GLint len = 0;
   glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &len);
   if (len > 1) {
      char *log = malloc((size_t)len + 1);
      if (log != NULL) {
         glGetShaderInfoLog(sh, len, NULL, log);
         ps4_log("%s info log: %s", what, log);
         free(log);
      }
   }
   ps4_log("%s compile: %s", what, ok ? "OK" : "FAILED");
   return ok ? sh : 0;
}

/* One sampled pixel against one expectation, with a tolerance the caller chooses: 0 for the flat
 * pass, non-zero for the interpolated one. Returns 1 on match. */
static int
check(const char *pass, const char *name, int x, int y,
      const uint8_t *got, const uint8_t *want, int tol)
{
   const int dr = abs((int)got[0] - (int)want[0]);
   const int dg = abs((int)got[1] - (int)want[1]);
   const int db = abs((int)got[2] - (int)want[2]);
   const int ok = dr <= tol && dg <= tol && db <= tol;

   if (!ok)
      ps4_log("%s %-12s (%4d,%4d) got %3u %3u %3u  want %3u %3u %3u  (tol %d)  MISMATCH",
              pass, name, x, y, got[0], got[1], got[2], want[0], want[1], want[2], tol);
   return ok;
}

int
main(void)
{
   ps4_app_init("gltri", "gl-triangle");
   ps4_log("start - a triangle with shaders, %dx%d", W, H);

   EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
   EGLint major = 0, minor = 0;
   if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, &major, &minor)) {
      ps4_log_fatal("no EGL display");
      ps4_idle_forever("no egl");
      return 1;
   }

   static const EGLint cfg_attrs[] = {
      EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
      EGL_NONE
   };
   EGLConfig cfg = NULL;
   EGLint n = 0;
   if (!eglChooseConfig(dpy, cfg_attrs, &cfg, 1, &n) || n < 1) {
      ps4_log_fatal("no config");
      ps4_idle_forever("no config");
      return 1;
   }

   /* Non-zero and ignored - see glrun.c's header for why it cannot be NULL. */
   EGLSurface surf = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)1, NULL);
   eglBindAPI(EGL_OPENGL_ES_API);
   static const EGLint ctx_attrs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
   EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
   if (surf == EGL_NO_SURFACE || ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, surf, surf, ctx)) {
      ps4_log_fatal("no context/surface");
      ps4_idle_forever("no context");
      return 1;
   }

   ps4_log("GL_RENDERER = %s", (const char *)glGetString(GL_RENDERER));
   ps4_log("GL_VERSION  = %s", (const char *)glGetString(GL_VERSION));
   ps4_log("GLSL        = %s", (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION));

   GLint v = 0;
   glGetIntegerv(GL_MAX_TEXTURE_SIZE, &v);              ps4_log("MAX_TEXTURE_SIZE      = %d", v);
   glGetIntegerv(GL_MAX_VERTEX_ATTRIBS, &v);            ps4_log("MAX_VERTEX_ATTRIBS    = %d", v);
   glGetIntegerv(GL_MAX_VARYING_VECTORS, &v);           ps4_log("MAX_VARYING_VECTORS   = %d", v);
   glGetIntegerv(GL_MAX_FRAGMENT_UNIFORM_VECTORS, &v);  ps4_log("MAX_FRAG_UNIFORM_VECS = %d", v);

   /* ---------------------------------------------------------------- the shaders */
   GLuint vs = compile(GL_VERTEX_SHADER, vs_src, "vertex shader");
   GLuint fs = compile(GL_FRAGMENT_SHADER, fs_src, "fragment shader");
   if (vs == 0 || fs == 0) {
      ps4_log_fatal("VERDICT: FAIL - shader compilation");
      ps4_idle_forever("no shaders");
      return 1;
   }

   GLuint prog = glCreateProgram();
   glAttachShader(prog, vs);
   glAttachShader(prog, fs);
   /* Bound explicitly rather than queried, so a driver that hands out different indices cannot
    * silently swap position and colour. */
   glBindAttribLocation(prog, 0, "a_pos");
   glBindAttribLocation(prog, 1, "a_col");
   glLinkProgram(prog);

   GLint linked = 0;
   glGetProgramiv(prog, GL_LINK_STATUS, &linked);
   GLint loglen = 0;
   glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &loglen);
   if (loglen > 1) {
      char *log = malloc((size_t)loglen + 1);
      if (log != NULL) {
         glGetProgramInfoLog(prog, loglen, NULL, log);
         ps4_log("link info log: %s", log);
         free(log);
      }
   }
   ps4_log("program link: %s", linked ? "OK" : "FAILED");
   if (!linked) {
      ps4_log_fatal("VERDICT: FAIL - program link");
      ps4_idle_forever("no program");
      return 1;
   }

   const GLint u_solid = glGetUniformLocation(prog, "u_solid");
   const GLint u_flat = glGetUniformLocation(prog, "u_flat");
   ps4_log("uniform locations: u_solid=%d u_flat=%d", (int)u_solid, (int)u_flat);

   /* ---------------------------------------------------------------- the vertex buffer */
   GLuint vbo = 0;
   glGenBuffers(1, &vbo);
   glBindBuffer(GL_ARRAY_BUFFER, vbo);
   glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (const void *)0);
   glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (const void *)(2 * sizeof(float)));
   glEnableVertexAttribArray(0);
   glEnableVertexAttribArray(1);

   glViewport(0, 0, W, H);
   glUseProgram(prog);

   GLenum err = glGetError();
   ps4_log("setup glGetError = 0x%04x %s", (unsigned)err, err == GL_NO_ERROR ? "(none)" : "");

   /* ---------------------------------------------------------------- pass 1: flat colour */
   const uint8_t clear[3] = { 16, 16, 24 };
   const uint8_t solid[3] = { 240, 160, 32 };

   glClearColor(clear[0] / 255.0f, clear[1] / 255.0f, clear[2] / 255.0f, 1.0f);
   glClear(GL_COLOR_BUFFER_BIT);
   glUniform1f(u_flat, 1.0f);
   glUniform3f(u_solid, solid[0] / 255.0f, solid[1] / 255.0f, solid[2] / 255.0f);
   glDrawArrays(GL_TRIANGLES, 0, 3);
   glFinish();

   int p1_ok = 0;
   for (int i = 0; i < NPOINTS; i++) {
      const int x = px_x(points[i].ndc_x), y = px_y(points[i].ndc_y);
      uint8_t got[4] = { 0, 0, 0, 0 };
      glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, got);
      p1_ok += check("pass1", points[i].name, x, y, got,
                     points[i].inside ? solid : clear, 0);
   }
   ps4_log("pass 1 (flat uniform, exact): %d/%d points", p1_ok, NPOINTS);
   eglSwapBuffers(dpy, surf);

   /* ---------------------------------------------------------------- pass 2: interpolation */
   glClear(GL_COLOR_BUFFER_BIT);
   glUniform1f(u_flat, 0.0f);
   glDrawArrays(GL_TRIANGLES, 0, 3);
   glFinish();

   /* mediump in the fragment shader plus the barycentric maths being done twice in different
    * arithmetic is worth a few LSBs; 6/255 is loose enough not to be flaky and far tighter than any
    * real interpolation bug. */
   const int tol = 6;
   int p2_ok = 0, p2_n = 0;
   for (int i = 0; i < NPOINTS; i++) {
      const int x = px_x(points[i].ndc_x), y = px_y(points[i].ndc_y);
      uint8_t got[4] = { 0, 0, 0, 0 };
      glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, got);

      uint8_t want[3];
      if (points[i].inside) {
         float w0, w1, w2;
         bary(points[i].ndc_x, points[i].ndc_y, &w0, &w1, &w2);
         for (int c = 0; c < 3; c++) {
            const float f = w0 * verts[2 + c] + w1 * verts[7 + c] + w2 * verts[12 + c];
            want[c] = (uint8_t)(fminf(fmaxf(f, 0.0f), 1.0f) * 255.0f + 0.5f);
         }
      } else {
         memcpy(want, clear, sizeof(want));
      }
      p2_n++;
      p2_ok += check("pass2", points[i].name, x, y, got, want, points[i].inside ? tol : 0);
   }
   ps4_log("pass 2 (varying, tol %d inside / exact outside): %d/%d points", tol, p2_ok, p2_n);
   eglSwapBuffers(dpy, surf);

   /* ---------------------------------------------------------------- a few animated frames */
   int frames_ok = 0;
   const int frames = 30;
   for (int i = 0; i < frames; i++) {
      const float t = (float)i / (float)frames;
      glClearColor(clear[0] / 255.0f, clear[1] / 255.0f, clear[2] / 255.0f, 1.0f);
      glClear(GL_COLOR_BUFFER_BIT);
      glUniform1f(u_flat, 1.0f);
      glUniform3f(u_solid, t, 1.0f - t, 0.5f);
      glDrawArrays(GL_TRIANGLES, 0, 3);
      glFinish();

      uint8_t got[4] = { 0, 0, 0, 0 };
      glReadPixels(px_x(0.0f), px_y(-0.267f), 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, got);
      const uint8_t want[3] = { (uint8_t)(t * 255.0f + 0.5f),
                                (uint8_t)((1.0f - t) * 255.0f + 0.5f),
                                (uint8_t)(0.5f * 255.0f + 0.5f) };
      if (check("anim ", "centroid", px_x(0.0f), px_y(-0.267f), got, want, 2))
         frames_ok++;

      if (!eglSwapBuffers(dpy, surf))
         ps4_log("frame %d: eglSwapBuffers failed", i);
   }
   ps4_log("animated frames: %d/%d centroid samples correct", frames_ok, frames);

   err = glGetError();
   ps4_log("final glGetError = 0x%04x %s", (unsigned)err, err == GL_NO_ERROR ? "(none)" : "");

   const int all = (p1_ok == NPOINTS) && (p2_ok == p2_n) && (frames_ok == frames) && (err == GL_NO_ERROR);
   ps4_log("VERDICT: %s", all ? "OK - shaders compile, the triangle rasterises, interpolation is correct"
                              : "FAIL - see the MISMATCH lines above");

   glDeleteBuffers(1, &vbo);
   glDeleteProgram(prog);
   glDeleteShader(vs);
   glDeleteShader(fs);
   eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
   eglDestroyContext(dpy, ctx);
   eglDestroySurface(dpy, surf);
   eglTerminate(dpy);
   ps4_log("teardown returned");

   ps4_idle_forever("gltri done");
   return 0;
}
