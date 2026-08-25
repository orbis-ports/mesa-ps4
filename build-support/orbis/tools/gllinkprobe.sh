#!/usr/bin/env bash
# Link a minimal PS4 executable against the built OpenGL ES stack.
#
#   tools/gllinkprobe.sh <build-dir> <sdk> [out]
#
# WHY THIS IS A SEPARATE PROBE. linkprobe.sh links libvulkan_radeon.a alone, which is the shape a title
# that uses Vulkan directly has. The GL stack is four archives that only make sense together -
# libEGL.a, libGLESv2.a, libgallium-<ver>.a (zink + the mesa state tracker + the DRI frontend) and the
# driver underneath them - and the interesting failures are BETWEEN them: a DRI entry point that
# with_dri2 dropped, an entry-point library still expecting a shared glapi, an EGL platform whose
# implementation was never compiled. None of those are visible from `ar`.
set -euo pipefail

BUILD="${1:?usage: gllinkprobe.sh <build-dir> <sdk> [out]}"
SDK="${2:?usage: gllinkprobe.sh <build-dir> <sdk> [out]}"
OUT="${3:-${BUILD}/gllinkprobe.elf}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TREE="$(cd "${ROOT}/../.." && pwd)"

ORBIS_COMPAT="${ORBIS_COMPAT_DIR:-${HOME}/src-ps4/orbis-compat}"
[[ -f "${ORBIS_COMPAT}/build/liborbis-compat.a" ]] || {
  echo "gllinkprobe: no ${ORBIS_COMPAT}/build/liborbis-compat.a - build orbis-compat first" >&2; exit 2; }

# The gallium archive is versioned, so it is found rather than named.
GALLIUM="$(echo "${BUILD}"/src/gallium/targets/dri/libgallium-*.a)"
for a in "${BUILD}/src/egl/libEGL.a" "${BUILD}/src/mesa/glapi/es2api/libGLESv2.a" \
         "${GALLIUM}" "${BUILD}/src/amd/vulkan/libvulkan_radeon.a"; do
  [[ -f "${a}" ]] || { echo "gllinkprobe: no ${a}" >&2; exit 2; }
done

clang --target=x86_64-pc-freebsd12-elf --sysroot="${SDK}" -fPIC \
      -isysroot "${SDK}" -isystem "${ORBIS_COMPAT}/include" -isystem "${SDK}/include" \
      -I"${TREE}/include" \
      -c "${ROOT}/tools/glprobe.c" -o "${BUILD}/glprobe.o"

# ⚠ THIS PROBE MUST LINK THE WAY A TITLE LINKS, AND FOR ONE DAY IT DID NOT. It used to use
# --start-group alone and the SDK's own link.x, with a comment arguing that --whole-archive would
# pull in members no GL title reaches. Both halves of that were wrong in the same way: they optimised
# the probe for SIZE, and a probe's only job is to FAIL WHEN A TITLE WOULD.
#
# What the console then showed, on a binary this probe had passed:
#
#   * with --start-group alone, Mesa's generated dispatch tables reference their entry points WEAKLY.
#     A weak undefined reference does not extract an archive member - it resolves to zero. The
#     executable linked with `U vk_common_GetPhysicalDeviceProperties2` still undefined, every such
#     table slot NULL, and vk_tramp_GetPhysicalDeviceProperties2 did `jmpq *%rax` with rax = 0.
#   * with the SDK's link.x, .tdata.* is an orphan section (Mesa builds with -fdata-sections), lld
#     placed it ahead of .data.rel.ro, and the ALIGN(0x4000) that starts the RW segment stopped
#     starting anything. The console's loader refused the file outright: "segment #1 is not page
#     aligned", surfacing as "Cannot start the application" with nothing in any log.
#
# So: the corrected linker script, and --whole-archive on the ICD, exactly as glrun.sh does.
# --allow-multiple-definition comes with the latter because libgallium carries its own copy of parts
# of the Vulkan runtime - zink is a Vulkan client - so vk_enum_to_str.c ends up in both archives.
#
# ⚠ NO COMMENTS INSIDE THE CONTINUED COMMAND BELOW - a `#` between backslashes ends it silently.
clang --target=x86_64-pc-freebsd12-elf --sysroot="${SDK}" \
      -nostdlib -fuse-ld=lld -pie -Wl,-m,elf_x86_64 \
      -Wl,--script="${ORBIS_COMPAT}/cmake/orbis-tls.ld" -Wl,--eh-frame-hdr -Wl,--no-rosegment \
      -Wl,--error-limit=0 -Wl,--allow-multiple-definition \
      "${BUILD}/glprobe.o" \
      -Wl,--start-group \
      "${BUILD}/src/egl/libEGL.a" \
      "${BUILD}/src/mesa/glapi/es2api/libGLESv2.a" \
      "${GALLIUM}" \
      -Wl,--whole-archive "${BUILD}/src/amd/vulkan/libvulkan_radeon.a" -Wl,--no-whole-archive \
      -Wl,--end-group \
      -L"${ORBIS_COMPAT}/build" -lorbis-compat \
      -L"${SDK}/lib" -lc -lkernel -lc++ -lSceGnmDriver -lSceVideoOut "${SDK}/lib/crt1.o" \
      -o "${OUT}"

echo "gllinkprobe: linked $(stat -c%s "${OUT}") bytes -> ${OUT}"
