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

BUILD="${1:?usage: linkprobe.sh <build-dir> <sdk> [out]}"
SDK="${2:?usage: linkprobe.sh <build-dir> <sdk> [out]}"
OUT="${3:-${BUILD}/linkprobe.elf}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

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
      -Wl,--script="${SDK}/link.x" -Wl,--eh-frame-hdr -Wl,--no-rosegment \
      -Wl,--error-limit=0 \
      "${BUILD}/linkprobe.o" \
      -Wl,--whole-archive "${A}" -Wl,--no-whole-archive \
      -L"${SDK}/lib" -lc -lkernel -lc++ -lSceGnmDriver -lSceVideoOut "${SDK}/lib/crt1.o" \
      -o "${OUT}"

echo "linkprobe: linked $(stat -c%s "${OUT}") bytes -> ${OUT}"
