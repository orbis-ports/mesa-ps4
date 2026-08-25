#!/usr/bin/env bash
# Build glrun.c into a PS4 eboot.bin - the first OpenGL frame this port has ever attempted on hardware.
#
#   build-support/orbis/tools/glrun.sh <gl-build-dir> <sdk> [out-dir]
#
# SPDX-License-Identifier: MIT
#
# ⚠ THIS IS gllinkprobe.sh's LINK LINE PLUS A LOG CHANNEL, and it is deliberately a separate script
# rather than a flag on that one. gllinkprobe answers "do the archives resolve"; it links a program
# nobody runs, and it must stay that cheap so --gl can run it on every build. This one produces
# something that goes on a console, which means it also needs orbis-compat's optional/ log targets and
# -lSceNet, and those are exactly what the overlay refuses to put in liborbis-compat.a so that Mesa and
# the CTS do not inherit a network dependency for having asked for a working mmap.
#
# ⚠ NO vkloader THUNKS, AND THAT IS NOT AN OVERSIGHT. Every other consumer of this driver generates a
# thunk per Vulkan entry point it references, because it calls vkCreateInstance and vkCmdDraw itself.
# This program never mentions Vulkan: zink resolves the whole API through vk_icdGetInstanceProcAddr,
# which is a definition inside libvulkan_radeon.a, so the ONE symbol it needs statically is already in
# the archive on the line below. gen.py has nothing to emit for a GL title.
set -euo pipefail

BUILD="${1:?usage: glrun.sh <gl-build-dir> <sdk> [out-dir] [source.c]}"
SDK="${2:?usage: glrun.sh <gl-build-dir> <sdk> [out-dir] [source.c]}"
# ⚠ THE SOURCE IS AN ARGUMENT because the link line is the hard-won part, not the program. glrun.c
# proved the frame reaches the screen; gltri.c exercises the half of the stack that glClear cannot
# touch. Both want the same four archives, the same whole-archive rule and the same linker script,
# and a second copy of that would drift from this one within a week.
SRC="${4:-glrun.c}"
OUT_DIR="${3:-${BUILD}/${SRC%.c}}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TREE="$(cd "${ROOT}/../.." && pwd)"
ORBIS_COMPAT="${ORBIS_COMPAT_DIR:-${HOME}/src-ps4/orbis-compat}"

NETLOG_HOST="${PS4_NETLOG_HOST:-192.168.100.1}"
NETLOG_PORT="${PS4_NETLOG_PORT:-18194}"

die() { echo "glrun: $*" >&2; exit 2; }

[[ -f "${ORBIS_COMPAT}/build/liborbis-compat.a" ]] || \
  die "no ${ORBIS_COMPAT}/build/liborbis-compat.a - build orbis-compat first"
[[ -f "${ORBIS_COMPAT}/optional/ps4_app.cpp" ]] || \
  die "no ${ORBIS_COMPAT}/optional/ps4_app.cpp - this needs the overlay's opt-in log channel"

GALLIUM="$(echo "${BUILD}"/src/gallium/targets/dri/libgallium-*.a)"
for a in "${BUILD}/src/egl/libEGL.a" "${BUILD}/src/mesa/glapi/es2api/libGLESv2.a" \
      "${BUILD}/src/mesa/glapi/es1api/libGLESv1_CM.a" \
         "${GALLIUM}" "${BUILD}/src/amd/vulkan/libvulkan_radeon.a"; do
  [[ -f "${a}" ]] || die "no ${a} - run ps4/build.sh --gl first"
done

mkdir -p "${OUT_DIR}"

CFLAGS=(--target=x86_64-pc-freebsd12-elf --sysroot="${SDK}" -fPIC -funwind-tables
        -D__PS4__ -DPS4 -D__ORBIS__ -D_BSD_SOURCE=1
        -isysroot "${SDK}" -isystem "${ORBIS_COMPAT}/include" -isystem "${SDK}/include"
        -I"${TREE}/include" -O2)

echo "== compile"
clang "${CFLAGS[@]}" -c "${ROOT}/tools/${SRC}" -o "${OUT_DIR}/app.o"

# ⚠ THE STAMP IS THE FIRST QUESTION AFTER AN INSTALL, and this project has paid for not being able to
# answer it: three CTS packages built on three different days were all exactly the same size, so "the
# file changed" proves nothing. PS4_APP_STAMP is what the title prints before anything else runs.
STAMP="$(git -C "${TREE}" describe --tags --always --dirty 2> /dev/null || echo unknown)"

# ps4_app.cpp and orbis_netlog.cpp are C++, and are compiled here rather than linked from the overlay
# because they are NOT in liborbis-compat.a - see optional/CMakeLists.txt for why that is deliberate.
#
# ⚠ THE INCLUDE ORDER AND THE PREFIX ARE BOTH LOAD-BEARING, AND THIS SCRIPT GOT THEM WRONG FIRST TIME.
# Copied verbatim from CMAKE_CXX_FLAGS_INIT in orbis-compat/cmake/ps4-openorbis.cmake, whose comment
# block is the only place either is explained:
#
#   * libc++'s directory FIRST. Its C-header wrappers #include_next the platform header and add the
#     C++ overloads; with the C directory ahead of it, <cmath>'s `using ::abs;` finds only musl's
#     `int abs(int)` and std::abs on a float silently truncates. Measured at 37 call sites in
#     OpenGothic. orbis-compat goes BETWEEN libc++ and the SDK, not first.
#   * -include orbis_prefix.h, which is <stddef.h> and <stdint.h> and nothing else. The SDK's headers
#     are not self-contained - orbis/Net.h names size_t without including <stddef.h> - and without the
#     prefix this script failed on exactly that. Not -include stdlib.h: that is what caused the abs
#     truncation above, and it covers fewer of the SDK's headers (16 failures vs 7 across all 189).
CXXFLAGS=(--target=x86_64-pc-freebsd12-elf --sysroot="${SDK}" -fPIC -funwind-tables
          -D__PS4__ -DPS4 -D__ORBIS__ -D_BSD_SOURCE=1 -O2
          -isysroot "${SDK}"
          -isystem "${SDK}/include/c++/v1"
          -isystem "${ORBIS_COMPAT}/include"
          -isystem "${SDK}/include"
          -include orbis_prefix.h
          -DNETLOG_HOST="\"${NETLOG_HOST}\"" -DNETLOG_PORT="${NETLOG_PORT}"
          -DPS4_APP_STAMP="\"${STAMP}\"")
clang++ "${CXXFLAGS[@]}" -c "${ORBIS_COMPAT}/optional/ps4_app.cpp"      -o "${OUT_DIR}/ps4_app.o"
clang++ "${CXXFLAGS[@]}" -c "${ORBIS_COMPAT}/optional/orbis_netlog.cpp" -o "${OUT_DIR}/orbis_netlog.o"

echo "== link"
# --start-group around the four Mesa archives: they reference each other in both directions (EGL calls
# into gallium, gallium's zink calls back into the ICD), and a single pass leaves undefined symbols.
#
# ⚠ --whole-archive ON THE ICD IS NOT OPTIONAL, AND OMITTING IT PRODUCES A BINARY THAT LAUNCHES AND
# THEN JUMPS TO ADDRESS ZERO. This script's first version left it off, reasoning that pulling every
# member of a 36 MB archive would bloat an eboot with code no GL title reaches. That reasoning is
# sound about SIZE and wrong about CORRECTNESS.
#
# Mesa's dispatch tables are built from generated entrypoint tables whose entries are WEAK references
# - radv_*, wsi_* and vk_common_* for the same entry point, so that whichever layer implements it
# wins and the rest fall away. A weak undefined reference does NOT cause a linker to extract an
# archive member; it resolves to zero. In a shared build every member is present anyway and the
# mechanism is invisible. In a static link without --whole-archive the definitions are simply never
# pulled, every table slot for them is NULL, and the failure surfaces at the first CALL:
#
#     rip: 0000000000000000, reason: page fault (user read instruction, page not present)
#     backtrace: radv_init_wsi+0x55 <- wsi_device_init <- vk_tramp_GetPhysicalDeviceProperties2+0xc
#
# `llvm-nm` on the eboot said it plainly: `U vk_common_GetPhysicalDeviceProperties2`, undefined in a
# fully linked executable. linkprobe.sh and RetroArch's Makefile.orbis both use --whole-archive here;
# this script is the odd one out and was wrong to be.
#
# Only the ICD needs it: the GL side is reached through ordinary strong references from glrun.c.
#
# ⚠ AND --allow-multiple-definition COMES WITH IT, for a reason particular to a GL build. linkprobe.sh
# and RetroArch link the ICD whole-archive and hit no duplicates, because they link nothing else.
# libgallium carries its own copy of parts of the Vulkan runtime - zink is a Vulkan client, so
# vk_enum_to_str.c and friends are compiled into it as well as into the ICD. Pulling every ICD member
# therefore presents the linker with two identical definitions of e.g. vk_ObjectType_to_ObjectName.
# They are the same generated code from the same source, so which one wins does not matter; what
# matters is that the alternative - leaving --whole-archive off - is the NULL dispatch table above.
clang --target=x86_64-pc-freebsd12-elf --sysroot="${SDK}" \
      -nostdlib -fuse-ld=lld -pie -Wl,-m,elf_x86_64 \
      -Wl,--script="${ORBIS_COMPAT}/cmake/orbis-tls.ld" -Wl,--eh-frame-hdr -Wl,--no-rosegment \
      -Wl,--error-limit=0 -Wl,--allow-multiple-definition \
      "${OUT_DIR}/app.o" "${OUT_DIR}/ps4_app.o" "${OUT_DIR}/orbis_netlog.o" \
      -Wl,--start-group \
      "${BUILD}/src/egl/libEGL.a" \
      "${BUILD}/src/mesa/glapi/es2api/libGLESv2.a" \
      "${BUILD}/src/mesa/glapi/es1api/libGLESv1_CM.a" \
      "${GALLIUM}" \
      -Wl,--whole-archive "${BUILD}/src/amd/vulkan/libvulkan_radeon.a" -Wl,--no-whole-archive \
      -Wl,--end-group \
      -L"${ORBIS_COMPAT}/build" -lorbis-compat \
      -L"${SDK}/lib" -lc -lkernel -lc++ -lSceNet -lSceGnmDriver -lSceVideoOut \
      "${SDK}/lib/crt1.o" \
      -o "${OUT_DIR}/app.elf"

# ⚠ THE LINKER'S OUTPUT IS NOT AN eboot.bin, AND SKIPPING THIS STEP COSTS A CONSOLE ROUND TRIP.
# A plain ELF named eboot.bin installs, appears on the home screen, and then fails to launch with
# "Cannot start the application" and NOTHING in klog - the loader rejects it before any of our code
# runs, so there is no log line to find and no crash to read. The console needs a fake-signed SELF,
# which is what create-fself makes. Recipe is the one in ps4-mesa-docs/cts.md; the PAID is the value
# every homebrew SELF in this project uses.
echo "== fself"
# ⚠ REMOVED FIRST, BECAUSE A STALE eboot.bin MADE THE CHECK BELOW LIE. create-fself writes nothing and
# exits non-zero when it cannot resolve a symbol; with a previous run's file still there, the existence
# test passed, the script reported success, and what went to the console was the last good build.
rm -f "${OUT_DIR}/eboot.bin"
OO_PS4_TOOLCHAIN="${SDK}" "${SDK}/bin/linux/create-fself" \
  -in="${OUT_DIR}/app.elf" -out="${OUT_DIR}/app.oelf" \
  --eboot "${OUT_DIR}/eboot.bin" --paid 0x3800000000000011 > /dev/null

[[ -f "${OUT_DIR}/eboot.bin" ]] || die "create-fself produced no eboot.bin"

echo "${SRC%.c}: eboot.bin is $(stat -c%s "${OUT_DIR}/eboot.bin") bytes  (stamp ${STAMP})"
echo "       (linker output was $(stat -c%s "${OUT_DIR}/app.elf") bytes of plain ELF)"
echo "       netlog -> ${NETLOG_HOST}:${NETLOG_PORT}"
echo "       next: scripts/ps4/make-pkg.sh --eboot ${OUT_DIR}/eboot.bin --out-dir ${OUT_DIR}/pkg ..."
