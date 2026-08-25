/**************************************************************************
 *
 * Copyright 2008 VMware, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

#ifndef __eglmesaext_h_
#define __eglmesaext_h_

#ifdef __cplusplus
extern "C" {
#endif

#include <EGL/eglplatform.h>

/* remnant of EGL_NOK_swap_region kept for compatibility because of a non-standard type name */
typedef EGLBoolean (EGLAPIENTRYP PFNEGLSWAPBUFFERSREGIONNOK) (EGLDisplay dpy, EGLSurface surface, EGLint numRects, const EGLint* rects);

#ifndef EGL_MESA_configless_context
#define EGL_MESA_configless_context 1
#define EGL_NO_CONFIG_MESA			EGL_CAST(EGLConfig,0)
#endif

#ifndef EGL_MESA_platform_orbis
#define EGL_MESA_platform_orbis 1
/* ⚠ NOT A KHRONOS-REGISTERED ENUM. This value is local to this fork.
 *
 * There is no registered EGL platform for the PlayStation 4 and there will not be one, so a value had
 * to be picked. 0x31E0 is the first slot after the block Mesa's own platforms occupy (GBM 0x31D7,
 * XCB 0x31DC, surfaceless 0x31DD) and is unused by every header in include/EGL. If it ever collides
 * with something Khronos allocates later, this define is the single place to change: the enum is
 * consumed once, in _eglGetPlatformDisplayCommon().
 *
 * Titles do not normally need it. A homebrew title has nothing to pass to eglGetPlatformDisplay, so
 * the expected call is eglGetDisplay(EGL_DEFAULT_DISPLAY), which resolves to this platform through the
 * build-time native platform. */
#define EGL_PLATFORM_ORBIS_MESA 0x31E0
#endif /* EGL_MESA_platform_orbis */

#ifndef EGL_MESA_drm_image_formats
#define EGL_MESA_drm_image_formats 1
#define EGL_DRM_BUFFER_FORMAT_ARGB2101010_MESA  0x3290
#define EGL_DRM_BUFFER_FORMAT_ARGB1555_MESA     0x3291
#define EGL_DRM_BUFFER_FORMAT_RGB565_MESA       0x3292
#endif /* EGL_MESA_drm_image_formats */

#ifdef __cplusplus
}
#endif

#endif
