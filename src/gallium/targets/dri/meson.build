# Copyright © 2017 Dylan Baker
# Copyright © 2018 Intel Corporation
# SPDX-License-Identifier: MIT

# TODO: support non-static targets
# Static targets are always enabled in autotools (unless you modify
# configure.ac)

gallium_dri_c_args = []
gallium_dri_ld_args = [cc.get_supported_link_arguments('-Wl,--default-symver')]
gallium_dri_link_depends = []
gallium_dri_drivers = []
gallium_dri_link_with = []
gallium_dri_link_whole = []

if with_gallium_va
  gallium_dri_link_with += [libgalliumvlwinsys]
  gallium_dri_link_whole += [libva_st]
endif

if host_machine.cpu_family() == 'x86' and not with_glx_read_only_text
  gallium_dri_ld_args += [cc.get_supported_link_arguments('-Wl,--no-error-rwx-segments')]
endif

dri_sym = configure_file(input : 'dri.sym.in', output : 'dri.sym', configuration : sym_config)

# ⚠ BOTH OF THESE DESCRIBE A DYNAMIC SYMBOL TABLE, which an archive does not have. Guarded on
# with_loadable_modules rather than left to be ignored: ld.lld accepts --version-script on a link that
# has no dynamic symbols and silently does nothing, and meson would still add dri.sym to link_depends,
# so the build would carry a rule whose output nothing reads.
if with_loadable_modules
  if with_ld_version_script
    gallium_dri_ld_args += ['-Wl,--version-script', join_paths(meson.current_build_dir(), 'dri.sym')]
    gallium_dri_link_depends += dri_sym
  endif
  if with_ld_dynamic_list
    gallium_dri_ld_args += ['-Wl,--dynamic-list', join_paths(meson.current_source_dir(), '../dri.dyn')]
    gallium_dri_link_depends += files('../dri.dyn')
  endif
endif

if get_option('unversion-libgallium') or with_platform_android
  libgallium_name = 'gallium_dri'
else
  libgallium_name = 'gallium-@0@'.format(meson.project_version())
endif

# ⚠ TWO CALLS RATHER THAN ONE, for the same reasons src/amd/vulkan/meson.build gives: library() would
# make this the only target in the tree answering to -Ddefault_library, and build_target(target_type:)
# raises the project's Meson floor. The arguments common to both are hoisted; link_args, link_depends,
# name_suffix and the megadriver symlinks describe a shared object and nothing else.
gallium_dri_sources = files('dri_target.c')
gallium_dri_includes = [
  inc_include, inc_src, inc_mesa, inc_gallium, inc_gallium_aux, inc_util, inc_gallium_drivers,
  inc_gallium_winsys, include_directories('../../frontends/dri'),
]
gallium_dri_link_with_all = [
  libmesa, libgalliumvl,
  libgallium, libglapi, libpipe_loader_static, libws_null, libwsw, libswdri,
  libswkmsdri, gallium_dri_link_with
]
gallium_dri_link_whole_all = [libdri, gallium_dri_link_whole]
gallium_dri_deps = [
  dep_libdrm, dep_llvm, dep_thread, idep_xmlconfig, idep_mesautil,
  driver_swrast, driver_r300, driver_r600, driver_radeonsi, driver_nouveau,
  driver_kmsro, driver_v3d, driver_vc4, driver_freedreno, driver_etnaviv,
  driver_tegra, driver_i915, driver_svga, driver_virgl,
  driver_panfrost, driver_iris, driver_lima, driver_zink, driver_d3d12,
  driver_asahi, driver_crocus, driver_rocket, driver_ethosu
]

if with_loadable_modules
  libgallium_dri = shared_library(
    libgallium_name,
    gallium_dri_sources,
    include_directories : gallium_dri_includes,
    gnu_symbol_visibility : 'hidden',
    link_args : [ld_args_build_id, ld_args_gc_sections, gallium_dri_ld_args],
    link_depends : gallium_dri_link_depends,
    link_with : gallium_dri_link_with_all,
    link_whole : gallium_dri_link_whole_all,
    dependencies : gallium_dri_deps,
    install : true,
    name_suffix : libname_suffix,
  )
else
  libgallium_dri = static_library(
    libgallium_name,
    gallium_dri_sources,
    include_directories : gallium_dri_includes,
    gnu_symbol_visibility : 'hidden',
    link_with : gallium_dri_link_with_all,
    link_whole : gallium_dri_link_whole_all,
    dependencies : gallium_dri_deps,
    # ⚠ install, and NOT because anyone installs it - the same trick libvulkan_radeon uses. Meson builds
    # an uninstalled static library as a THIN archive, an index of absolute paths to loose objects that
    # absorbs nothing from link_with. Marking it installed produces a real archive with the component
    # static libraries flattened in, which is what "the driver is linked into the title" needs.
    install : true,
  )
endif

shared_glapi_lib = libgallium_dri

if with_gallium_va
  va_drivers = []
  foreach d : [[with_gallium_r600, 'r600'],
              [with_gallium_radeonsi, 'radeonsi'],
              [with_gallium_nouveau, 'nouveau'],
              [with_gallium_virgl, 'virtio_gpu'],
              [with_gallium_d3d12_video, 'd3d12']]
    if d[0]
      name = '@0@_drv_video.@1@'.format(d[1], libname_suffix)
      va_drivers += name

      if prog_ln.found()
        custom_target(
          name,
          output : name,
          command : [prog_ln, '-sf', libgallium_dri.full_path(), '@OUTPUT@'],
          build_by_default : true,
        )
      endif
    endif
  endforeach

  devenv.set('LIBVA_DRIVERS_PATH', meson.current_build_dir())

  if va_drivers.length() > 0
    meson.add_install_script(
      install_megadrivers,
      libgallium_dri.full_path(),
      va_drivers_path,
      va_drivers,
      '--megadriver-libdir', get_option('libdir'),
      install_tag : 'runtime',
    )
  endif
endif
