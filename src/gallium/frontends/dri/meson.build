# Copyright © 2017 Dylan Baker
# SPDX-License-Identifier: MIT

inc_st_dri = include_directories('.')
deps_for_libdri = []

files_libdri = files(
  'dri_context.c',
  'dri_context.h',
  'dri_drawable.c',
  'dri_drawable.h',
  'dri_helpers.c',
  'dri_helpers.h',
  'dri_query_renderer.c',
  'dri_query_renderer.h',
  'dri_screen.c',
  'dri_screen.h',
  'dri_util.c',
  'dri_util.h',
)

if with_platform_x11
  deps_for_libdri += dep_xcb
  if with_dri_platform == 'drm' or with_dri_platform == 'pseudo-drm'
    deps_for_libdri += [dep_xcb_dri3, dep_xcb_present, dep_xcb_sync,
                        dep_xshmfence, dep_xcb_xfixes]
    files_libdri += files('loader_dri3_helper.c')
  endif
endif

if with_dri
  files_libdri += files('drisw.c')
endif

# ⚠ with_dri TOO, not just with_dri2. The name is historical: dri2.c holds this frontend's whole
# image/interop/blob-cache implementation (dri_create_image, dri_interop_*, dri_set_damage_region,
# dri_set_blob_cache_funcs, dri2_init_drawable...), which dri_util.c, dri_drawable.c, dri_helpers.c,
# drisw.c, kopper.c and src/egl's egl_dri2.c all call unconditionally. Every configuration that has
# had with_dri so far also had with_dri2 - with_dri_platform is 'drm', 'apple' or 'pseudo-drm' in all
# of them - so a with_dri build without dri2.c had never been linked until this one, and it fails
# with 12 undefined symbols. The DRM-specific parts of the file are guarded inside it by HAVE_LIBDRM.
if with_dri2 or with_dri
  files_libdri += files('dri2.c')
endif

libdri_c_args = []
if with_gallium_zink and not with_platform_android
  if with_glvnd
    libdri_c_args += '-DKOPPER_LIB_NAMES="libEGL_@0@ and libGLX_@0@"'.format(glvnd_vendor_name)
  else
    libdri_c_args += '-DKOPPER_LIB_NAMES="libEGL and libGLX"'
  endif
  files_libdri += files('kopper.c')
  deps_for_libdri += vulkan_wsi_deps
else
  files_libdri += files('kopper_stubs.c')
endif

libdri = static_library(
  'dri',
  [ files_libdri, sha1_h ],
  include_directories : [
    inc_include, inc_util, inc_mesa, inc_src, inc_gallium,
    inc_gallium_aux, inc_util, inc_gallium_drivers, inc_loader,
  ],
  c_args : [libdri_c_args],
  gnu_symbol_visibility : 'hidden',
  dependencies : [
    dep_libdrm,
    idep_mesautil,
    deps_for_libdri,
  ],
  link_with : [libloader],
)

if with_dri
  dri_req_private = []
  if dep_libdrm.found()
    dri_req_private = ['libdrm >= @0@'.format(_drm_ver)]
  endif

  pkg.generate(
    name : 'dri',
    filebase : 'dri',
    description : 'Direct Rendering Infrastructure',
    version : meson.project_version(),
    variables : ['dridriverdir=@0@'.format(dri_drivers_path)],
    requires_private : dri_req_private,
  )
endif
