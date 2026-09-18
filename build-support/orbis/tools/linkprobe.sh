#!/usr/bin/env bash
# Link a minimal PS4 executable against the built RADV archive.
#
#   tools/linkprobe.sh <build-dir> <sdk> [out]
#
# WHY THIS IS A BUILD STEP. The archive being self-contained was an assumption for as long as the build
# stopped at `ar`, and two measurements of it disagreed - the shared-library link said three undefined
# symbols, `nm` over the archive suggested 244 more. Linking settles it, and settling it once is worth
# less than keeping it settled: the 21 symbols this probe found the first time (zlib and libelf, both the
# HOST's, reached through meson's cmake fallback and lld's default library paths) were invisible to a
# configure that reported success.
set -euo pipefail

# Portable file size. macOS stat has no -c: `stat: illegal option -- c`, and the message goes to
# stderr while the substitution yields the empty string, so the line still prints and reads as a
# successful step with a blank number. GNU first, BSD second; both are exact.
orbis_size() { stat -c%s "$1" 2>/dev/null || stat -f%z "$1"; }

BUILD="${1:?usage: linkprobe.sh <build-dir> <sdk> [out]}"
SDK="${2:?usage: linkprobe.sh <build-dir> <sdk> [out]}"
OUT="${3:-${BUILD}/linkprobe.elf}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# ⚠ liborbis-compat.a IS PART OF THE LINK LINE NOW, and leaving it out did not merely weaken this probe -
# it made the probe disagree with the build. orbis-compat's <unistd.h> does `#define sysconf orbis_sysconf`
# and puts orbis_sysconf in this archive, so util/os_misc.c now emits a call to it; without the archive the
# probe stops on "undefined symbol: orbis_sysconf" for an object that is perfectly correct. Every title
# that links this driver links orbis-compat too - the overlay is not optional - so the probe's link line
# has to look like a title's.
ORBIS_COMPAT="${ORBIS_COMPAT_DIR:-${HOME}/src-ps4/orbis-compat}"

# ⚠ THE LINKER SCRIPT IS THE KIT'S SINCE 2026-09-18. cmake/ moved to
# orbis-ports/orbis-porting-kit; the overlay keeps include/ and the archive, both of which this
# probe still takes from ORBIS_COMPAT. The fallback is the overlay, which carried the script until
# then. Without this the probe fails with "cannot find linker script" and reports it as an archive
# that is not self-contained, which is a diagnosis about the wrong thing entirely.
ORBIS_KIT="${ORBIS_KIT_DIR:-}"
[[ -n "${ORBIS_KIT}" && -f "${ORBIS_KIT}/cmake/orbis-tls.ld" ]] || ORBIS_KIT="${ORBIS_COMPAT}"

[[ -f "${ORBIS_COMPAT}/build/liborbis-compat.a" ]] || {
  echo "linkprobe: no ${ORBIS_COMPAT}/build/liborbis-compat.a - build orbis-compat first" >&2; exit 2; }

A="${BUILD}/src/amd/vulkan/libvulkan_radeon.a"
[[ -f "${A}" ]] || { echo "linkprobe: no ${A}" >&2; exit 2; }

# ⚠ ONLY libvulkan_radeon.a, and that is not an oversight. meson merges addrlib and amd_common into it,
# and they ALSO exist as their own archives - so passing all three with --whole-archive produces 1139
# duplicate-symbol errors that look like a broken build and are nothing of the kind.
#
# --whole-archive because "the driver is linked into the title" is the target shape: nothing later will
# dlopen the members the linker decided were unreachable.
clang --target=x86_64-pc-freebsd12-elf --sysroot="${SDK}" -fPIC \
      -isysroot "${SDK}" -isystem "${SDK}/include" \
      -c "${ROOT}/tools/linkprobe.c" -o "${BUILD}/linkprobe.o"

# -lSceVideoOut is as much a driver dependency as -lSceGnmDriver now: the WSI arm (src/vulkan/wsi/wsi_orbis.c)
# opens video-out, registers scan-out buffers and flips, so a title linking this archive needs it too. This probe
# is where a missing one becomes a build failure here rather than in somebody else's tree - which is exactly how
# it was caught.
#
# ⚠ NO COMMENTS INSIDE THE CONTINUED COMMAND BELOW. A `#` line between backslashes ends the command, and the rest
# of the flags become a separate no-op - which briefly made the archive look like it had lost sceGnm* symbols.
clang --target=x86_64-pc-freebsd12-elf --sysroot="${SDK}" \
      -nostdlib -fuse-ld=lld -pie -Wl,-m,elf_x86_64 \
      -Wl,--script="${ORBIS_KIT}/cmake/orbis-tls.ld" -Wl,--eh-frame-hdr -Wl,--no-rosegment \
      -Wl,--error-limit=0 \
      "${BUILD}/linkprobe.o" \
      -Wl,--whole-archive "${A}" -Wl,--no-whole-archive "${BUILD}"/subprojects/zlib-*/libz.a \
      -L"${ORBIS_COMPAT}/build" -lorbis-compat \
      -L"${SDK}/lib" -lc -lkernel -lc++ -lSceGnmDriver -lSceVideoOut "${SDK}/lib/crt1.o" \
      -o "${OUT}"

echo "linkprobe: linked $(orbis_size "${OUT}") bytes -> ${OUT}"
