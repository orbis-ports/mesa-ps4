#!/usr/bin/env bash
# Cross-build Mesa's RADV for the PS4 (OpenOrbis).
#
#   build-support/orbis/build.sh [--sdk <openorbis>] [--work <dir>] [--host-too] [--host-orbis]
#                               [--gl] [--regress] [--mingw]
#
# The scaffolding used to be a separate repo (orbis-mesa) beside a driver fork. It is one tree now: this
# script builds the checkout it lives in, so there is no --mesa argument and no way to point it at the
# wrong tree.
#
# WHAT THIS PRODUCES: libvulkan_radeon.a for x86_64-pc-freebsd12-elf. The archive is complete except for
# the ac_drm_* arm - run ps4-mesa-docs/notes/README.md's regeneration command against it to see which symbols are still
# missing.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TREE="$(git -C "${ROOT}" rev-parse --show-toplevel)"
SDK="${OO_PS4_TOOLCHAIN:-${HOME}/.local/opt/openorbis}"
WORK="${HOME}/.cache/orbis-mesa"
HOST_TOO=no
HOST_ORBIS=no
GL=no
REGRESS=no
MINGW=no

while [[ $# -gt 0 ]]; do
  case "$1" in
    --sdk)  SDK="$2";  shift 2 ;;
    --work) WORK="$2"; shift 2 ;;
    --host-too) HOST_TOO=yes; shift ;;
    --host-orbis) HOST_ORBIS=yes; shift ;;
    --gl) GL=yes; shift ;;
    --regress) REGRESS=yes; shift ;;
    --mingw) MINGW=yes; shift ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

die() { echo "orbis: $*" >&2; exit 1; }

[[ -d "${TREE}/src/amd/vulkan" ]] || die "no RADV at ${TREE} - this is not a Mesa checkout"
[[ -f "${SDK}/link.x" ]]          || die "no OpenOrbis toolchain at ${SDK} (--sdk <path>)"
command -v nix > /dev/null         || die "nix is required: Mesa's build deps come from 'nix develop nixpkgs#mesa'"

# ⚠ NOT FATAL, because the port lands in batches and the first four legitimately predate the arm. It was
# fatal in the old repo only to catch --mesa pointing at stock Mesa, and that argument is gone.
[[ -f "${TREE}/src/amd/common/ac_orbis_drm.c" ]] || \
  echo "== note: no ac_orbis_drm.c yet - the ac_drm_* arm lands in batch 05, so expect undefined symbols"

CROSS="${WORK}/cross"

# ⚠ NOT OPTIONAL, AND IT REFUSES RATHER THAN BUILDING WITHOUT IT. orbis-compat carries corrections
# this SDK needs; a build that silently omits them is a build against declarations known to be wrong.
ORBIS_COMPAT="${ORBIS_COMPAT_DIR:-${HOME}/src-ps4/orbis-compat}"
[[ -f "${ORBIS_COMPAT}/include/bits/alltypes.h" ]] || die \
  "orbis-compat not found at ${ORBIS_COMPAT} - set ORBIS_COMPAT_DIR"

# ---------------------------------------------------------------- the cross prefix
#
# The seven headers that used to be copied from ${ROOT}/shims are GONE FROM THIS TREE. They live in
# orbis-compat, which every consumer of this SDK already needs for reasons no build shim can cover
# (the pthread type sizes), and the cross file below puts its include directory first. Two copies of
# a header are two things to keep in step; this build.sh used to have a check whose whole job was to
# notice when they drifted.
#
# The libdrm shims that preceded them are GONE for a different reason: with -Dplatforms=orbis nothing
# includes xf86drm.h or libdrm/amdgpu.h at all. Verified with `ninja -t deps` - zero references.
#
# What is left here is a prefix that exists to hold NOTHING. An empty but EXISTING pkgconfig dir:
# pointing PKG_CONFIG_LIBDIR at a path that does not exist leaves pkg-config's behaviour ambiguous,
# while an empty real directory unambiguously finds nothing.
echo "== cross prefix -> ${CROSS}"
rm -rf "${CROSS}"
mkdir -p "${CROSS}/lib/pkgconfig"

# ---------------------------------------------------------------- the cross file
echo "== meson cross file"
sed -e "s|@OO_PS4_TOOLCHAIN@|${SDK}|g" -e "s|@ORBIS_CROSS@|${CROSS}|g" -e "s|@ORBIS_COMPAT@|${ORBIS_COMPAT}|g" \
    "${ORBIS_COMPAT}/cmake/orbis.ini.in" > "${CROSS}/orbis.ini"

# ⚠ THE LIBRARY HALF OF orbis-compat IS NOW IN THE TEMPLATE, AND THIS BLOCK ONLY CHECKS THAT IT IS.
#
# It used to inject it here with sed, under a comment saying its permanent home was
# orbis-compat/cmake/orbis.ini.in and that whoever moved it should delete this block. It has been moved;
# this is the deletion, minus the two assertions, which are worth keeping for the reason below.
#
# meson decides HAVE_SYSCONF with cc.has_function('sysconf'), which is a LINK test. orbis-compat's
# <unistd.h> makes sysconf a macro for orbis_sysconf (so that _SC_NPROCESSORS_ONLN can answer six rather
# than the one musl reports on this console), and orbis_sysconf lives in liborbis-compat.a. With only the
# overlay's include path on the line, that test links against a declaration whose definition is nowhere:
#
#     ld.lld: error: undefined symbol: orbis_sysconf
#     Checking for function "sysconf" : NO
#
# HAVE_SYSCONF then goes unset, and util/os_misc.c - which every build in this tree compiles - falls
# through its BSD chain into `#error Unsupported *BSD` and `#error unexpected platform in os_sysinfo.c`.
# The failure names a *BSD problem and is really a missing -l, three hundred files from the cause. That
# is why this is checked here rather than left to be discovered: the overlay is a build-time dependency
# of the CONFIGURE CHECKS, not just of the title, and nothing else in this script would say so.
[[ -f "${ORBIS_COMPAT}/build/liborbis-compat.a" ]] || die \
  "no ${ORBIS_COMPAT}/build/liborbis-compat.a - build orbis-compat before Mesa (it is a link-time dependency of the configure checks, not just of the title)"
grep -q -- "-lorbis-compat" "${CROSS}/orbis.ini" || die \
  "the generated cross file has no -lorbis-compat - orbis-compat/cmake/orbis.ini.in has lost the library half of the overlay again; see the comment above for what that breaks and how it presents"

# ---------------------------------------------------------------- which driver is this
#
# What identifies this build is a git revision plus whatever is uncommitted, and both are printed because
# a build log that cannot name its own source has cost this port a day more than once.
#
# ⚠ RADV_BUILD_ID_OVERRIDE (passed as -Dradv-build-id further down) IS THE SHADER CACHE UUID, so it has to
# change whenever the driver changes - including for uncommitted edits, which is the normal state during a
# session. Hence HEAD plus a hash of the working diff plus a hash of the untracked files that meson compiles.
MESA_HEAD="$(git -C "${TREE}" rev-parse HEAD 2> /dev/null || echo unknown)"
MESA_REF="$(git -C "${TREE}" describe --tags --always --dirty 2> /dev/null || echo unknown)"
MESA_BRANCH="$(git -C "${TREE}" rev-parse --abbrev-ref HEAD 2> /dev/null || echo unknown)"
# --no-ext-diff and -U0: a user's diff.external or a wider context would change the hash without the driver
# changing. Untracked sources count too - a new .c that meson picks up is part of the driver.
WORK_SHA="$( { git -C "${TREE}" diff --no-ext-diff -U0 HEAD;
               git -C "${TREE}" ls-files --others --exclude-standard -z -- '*.c' '*.h' '*.build' \
                 | (cd "${TREE}" && xargs -0 --no-run-if-empty sha256sum); } | sha256sum | cut -d' ' -f1)"
SERIES_SHA="$(printf '%s-%s' "${MESA_HEAD}" "${WORK_SHA}" | sha256sum | cut -d' ' -f1)"

DIRTY_N="$(git -C "${TREE}" status --porcelain 2> /dev/null | grep -c '' || true)"
echo "== tree ${TREE}"
echo "   ${MESA_BRANCH} @ ${MESA_REF}  ($(git -C "${TREE}" log -1 --format=%s 2> /dev/null || echo '?'))"
echo "   uncommitted: ${DIRTY_N} path(s)"
# ⚠ A DETACHED HEAD IS SAID OUT LOUD. Forgetting you are on one and then editing files is how a day's work
# gets lost to a later checkout, so it is not left to be noticed.
[[ "${MESA_BRANCH}" == "HEAD" ]] && \
  echo "   ⚠ DETACHED HEAD - commit or branch before editing anything here."

# ---------------------------------------------------------------- configure and build
#
# default_library=static because the PS4 has no ICD loader: RADV gets linked into the title, and our own
# link line is an executable's (crt1.o, -pie), so a .so link fails on an undefined `main`.
COMMON_OPTS=(
  -Dvulkan-drivers=amd
  -Dgallium-drivers=
  -Dglx=disabled -Degl=disabled -Dgbm=disabled
  -Dopengl=false -Dgles1=disabled -Dgles2=disabled
  -Dvideo-codecs=
  -Dllvm=disabled
  -Dbuildtype=release
)

# ⚠ -Dplatforms MUST NOT be shared between the two builds. The orbis build wants 'orbis', which switches
# the DRM path OFF; the host build wants NO platform but libdrm ON, because the whole point of the host
# build is to run RADV against the amdgpu drm-shim. Passing 'orbis' to the host build makes dep_libdrm a
# null_dep, so the shim compiles without libdrm's own -I and dies on xf86drm.h's #include <drm.h>.
# -Dzlib=disabled because zlib is a HOST library here. Mesa's zlib is a 'feature' option defaulting to
# enabled, and with allow_fallback it reaches for a wrap when the system copy is gone - so this has to be
# said explicitly rather than left to detection failing gracefully. Consequence: HAVE_ZLIB and
# HAVE_COMPRESSION are unset, which costs the shader disk cache's compression. There is no disk cache on
# this console.
# ...and -Dshader-cache=disabled follows from it: Mesa errors out with "Shader Cache requires compression"
# when both zlib and zstd are off.
# -Dradv-build-id, and THIS ONE WAS FOUND ON HARDWARE. RADV derives its pipeline-cache UUID from the
# driver's ELF build-id, and when there is none it walks program headers looking for a NT_GNU_BUILD_ID
# note: disk_cache_get_function_identifier -> build_id_find_nhdr_for_addr. Our eboot has no such note
# (llvm-readelf -n: zero), and on the PS4 that walk SIGSEGVs inside libkernel - the first console run died
# there, in radv_device_get_cache_uuid, three calls before the radeon_info dump.
#
# RADV_BUILD_ID_OVERRIDE is upstream's own escape hatch for exactly this, taken two lines earlier in the
# same function, so no patch is involved. The value is derived from the build's real inputs - the Mesa
# commit and the working tree - so the cache UUID changes when the driver does.
ORBIS_BUILD_ID="$(printf '%s-%s' "${MESA_HEAD}" "${SERIES_SHA}" | sha256sum | cut -c1-40)"
ORBIS_OPTS=(-Dplatforms=orbis -Dzlib=disabled -Dshader-cache=disabled
            "-Dradv-build-id=${ORBIS_BUILD_ID}")
HOST_OPTS=(-Dplatforms=)

# ⚠ A CHANGED CROSS FILE MUST WIPE THE BUILD DIR. meson caches dependency lookups, so after fixing the
# cross file to stop finding the HOST's zlib and libelf, a reconfigure still reported
# "Dependency zlib found: YES (cached)" - the fix looked like it had not worked when in fact it had not
# been tested. The stamp is the cross file's own hash.
CROSS_STAMP="${TREE}/.orbis-cross-stamp"
CROSS_SHA="$(sha256sum "${CROSS}/orbis.ini" | cut -d' ' -f1)"
#
# ⚠ THE STAMP IS WRITTEN AFTER CONFIGURE SUCCEEDS, not here. Written here, a failed meson setup left the
# build directory wiped and the stamp already claiming the new cross file - so the next run skipped the
# wipe and handed --reconfigure to a directory that had never been configured, which is the stale
# dependency cache this stamp exists to prevent.
if [[ ! -f "${CROSS_STAMP}" || "$(cat "${CROSS_STAMP}")" != "${CROSS_SHA}" ]]; then
  echo "== cross file changed - wiping build-orbis so meson re-runs its dependency lookups"
  rm -rf "${TREE}/build-orbis"
  rm -f "${CROSS_STAMP}"
fi

echo "== configure (orbis)"
cd "${TREE}"
# PKG_CONFIG_PATH is CLEARED and PKG_CONFIG_LIBDIR points at an empty dir on purpose: nix's devShell sets
# the former, it OVERRIDES the cross file's own pkg_config_libdir, and six rounds of this port silently
# used the HOST libdrm, spirv-tools, libunwind and valgrind. The tell was std::__cxx11 symbols - libstdc++
# ABI - in a build that links libc++.
# --reconfigure when the directory already exists: meson refuses a plain setup on a configured tree, and
# the options do change between runs now that the build-id is derived from the inputs.
ORBIS_SETUP=()
[[ -d build-orbis ]] && ORBIS_SETUP=(--reconfigure)
nix develop nixpkgs#mesa --command env PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR="${CROSS}/lib/pkgconfig" \
  meson setup build-orbis "${ORBIS_SETUP[@]}" --cross-file "${CROSS}/orbis.ini" \
  -Ddefault_library=static -Dwrap_mode=nodownload \
  -Dzstd=disabled -Dvalgrind=disabled -Dlmsensors=disabled -Dlibunwind=disabled \
  "${COMMON_OPTS[@]}" "${ORBIS_OPTS[@]}"

# Only now, with a configured directory to go with it. set -e means a failed setup never reaches this.
echo "${CROSS_SHA}" > "${CROSS_STAMP}"

# ⚠ ASKED OF THE LOG, NOT ASSUMED FROM THE EXIT STATUS. meson reports success for a configure in which
# every link-based probe failed, and the one that matters here is sysconf: see the cross-file note above
# for why a missing -lorbis-compat turns into "#error Unsupported *BSD" three hundred files later. This
# is two seconds and it names the cause instead of the symptom.
grep -q 'Checking for function "sysconf" : YES' "${TREE}/build-orbis/meson-logs/meson-log.txt" || die \
  "configure did not find sysconf - HAVE_SYSCONF will be unset and util/os_misc.c will fail with '#error Unsupported *BSD'. Check that ${ORBIS_COMPAT}/build/liborbis-compat.a is current."

echo "== build (orbis)"
# -k 0 rather than stopping at the first error: the whole error list is the work list, and one error at
# a time turns a morning into a week.
mkdir -p "${WORK}"
BUILDLOG="${WORK}/build-orbis.log"
nix develop nixpkgs#mesa --command ninja -C build-orbis -k 0 2>&1 | tee "${BUILDLOG}" || true

echo
echo "== orbis result"
# Counted from the BUILD LOG, not from .ninja_log: .ninja_log is a timing database and never contains the
# word "error", so grepping it reported 0 no matter how badly the build had gone.
echo "   failed targets: $(grep -c '^FAILED:' "${BUILDLOG}" || true)"
# Anchored on file:line:col so that meson's own "supports -Wno-error: YES" lines do not count as errors.
#
# ⚠ "fatal error:" IS an error, and leaving it out of the pattern is not cosmetic: a missing header is
# reported that way, so a build whose every target died on one #include printed "compile errors: 0"
# beside "failed targets: 92" and read as a link problem.
echo "   compile errors: $(grep -cE '[.](c|cc|cpp|h|hpp):[0-9]+:[0-9]+: (fatal )?error:' "${BUILDLOG}" || true)"
ls -la build-orbis/src/amd/vulkan/libvulkan_radeon.a 2> /dev/null || \
  echo "   no libvulkan_radeon.a - see ${BUILDLOG}"

# ---------------------------------------------------------------- the link probe
#
# An archive is not a driver until something links it. This step is what caught 21 undefined symbols from
# the HOST's zlib and libelf, which a configure reporting success had hidden. It is cheap and it fails
# loudly, so it runs every time.
if [[ -f build-orbis/src/amd/vulkan/libvulkan_radeon.a ]]; then
  echo
  echo "== link probe (a PS4 executable against the driver)"
  "${ROOT}/tools/linkprobe.sh" "${TREE}/build-orbis" "${SDK}" || \
    echo "   LINK PROBE FAILED - the archive is not self-contained; read the undefined symbols above"
fi

# ---------------------------------------------------------------- the host build, optionally
#
# The same Mesa, built for the HOST with only ACO (llvm=disabled) and the drm-shim. That is what lets
# tools/acoprobe.c compile a real .sprv for gfx7 with no PS4 in the loop: AMDGPU_GPU_ID=bonaire is a
# complete CHIP_BONAIRE entry in the shim, which is gfx7/CIK - the generation this console is.
if [[ "${HOST_TOO}" == "yes" ]]; then
  echo "== configure + build (host, RADV+ACO+drm-shim)"
  # --reconfigure once the directory exists, as the orbis build does: meson refuses a plain setup on a
  # configured tree, and under set -e that aborts the whole script on the second run.
  HOST_SETUP=()
  [[ -d build-host ]] && HOST_SETUP=(--reconfigure)
  nix develop nixpkgs#mesa --command meson setup build-host "${HOST_SETUP[@]}" -Dtools=drm-shim \
    "${COMMON_OPTS[@]}" "${HOST_OPTS[@]}"
  nix develop nixpkgs#mesa --command ninja -C build-host
  echo
  echo "   to compile one module for gfx7 and dump its ISA:"
  echo "     cc -o acoprobe ${ROOT}/tools/acoprobe.c -lvulkan"
  echo "     LD_PRELOAD=${TREE}/build-host/src/amd/drm-shim/libamdgpu_noop_drm_shim.so \\"
  echo "     AMDGPU_GPU_ID=bonaire \\"
  echo "     VK_DRIVER_FILES=${TREE}/build-host/src/amd/vulkan/radeon_devenv_icd.x86_64.json \\"
  echo "     RADV_DEBUG=asm ./acoprobe <module>.sprv"
  echo
  echo "   to dump the whole radeon_info struct for gfx7 - the reference for phase 2:"
  echo "     ... RADV_DEBUG=info vulkaninfo --summary       # RADV_DEBUG, not AMD_DEBUG"
  echo "   captured already: ~/src-ps4/ps4-mesa-docs/notes/radeon_info-bonaire.txt (165 fields)"
fi

# ---------------------------------------------------------------- the host build OF OUR ARM
#
# THE DEVELOPMENT LOOP, and it needs neither a console nor a harness. -Dplatforms=orbis selects the no-DRM
# arm and our ac_drm_* implementation; it says nothing about the TOOLCHAIN. So the same driver, including
# our arm, builds for the laptop as an ordinary ICD and runs under the system Vulkan loader. RADV
# enumerates, reaches ac_query_gpu_info, and the log names the first query not implemented yet:
#
#     radv: info: orbis: creating the one physical device.
#     MESA: info: orbis-drm: device up, reporting amdgpu interface 3.54
#     MESA: warning: orbis-drm: ac_drm_query_gpu_info is not implemented yet
#
# Implement the one it names, rebuild, run again, get the next name.
if [[ "${HOST_ORBIS}" == "yes" ]]; then
  # ⚠ THE ONE PIECE OF THE PORT THIS GATE COULD NOT SEE. Everything below runs the driver on Linux,
  # where futex.c takes the __linux__ arm and calls the kernel's real futex - so orbis-compat's
  # sys/umtx.h, which every contended lock in Mesa goes through ON THE CONSOLE, saw nothing here.
  # Its last defect (an absolute CLOCK_MONOTONIC deadline handed to a CLOCK_REALTIME condition
  # variable, so every timed wait expired instantly) reproduces on this laptop perfectly well; it
  # was simply never asked. This asks, before anything is built.
  echo
  echo "== the futex shim, driven the way futex.c drives it"
  cc -O1 -o "${WORK}/umtxcheck" "${ORBIS_COMPAT}/test/umtxcheck.c" -I"${ORBIS_COMPAT}/include" -lpthread
  "${WORK}/umtxcheck" || die "the futex shim is broken - see the FAIL lines above. Every contended lock in Mesa is built on it."

  echo
  echo "== configure + build (host, OUR ARM)"
  cd "${TREE}"
  # --reconfigure for the same reason as build-host. NOT silenced: the previous version sent this to
  # /dev/null, so the second run died with set -e and no visible reason at all.
  HOSTORBIS_SETUP=()
  [[ -d build-hostorbis ]] && HOSTORBIS_SETUP=(--reconfigure)
  # ⚠ -Dbuildtype AFTER COMMON_OPTS, which carries -Dbuildtype=release. Meson takes the last of a
  # duplicated option, so passing it first made the build whose entire purpose is the debug loop a
  # release build - NDEBUG set and every assert compiled out.
  nix develop nixpkgs#mesa --command meson setup build-hostorbis "${HOSTORBIS_SETUP[@]}" \
    -Dzlib=disabled -Dshader-cache=disabled \
    "${COMMON_OPTS[@]}" "${ORBIS_OPTS[@]}" -Dbuildtype=debugoptimized
  nix develop nixpkgs#mesa --command ninja -C build-hostorbis

  # infoprobe rather than vulkaninfo: vulkaninfo builds an AppGpu per device, which calls vkCreateDevice -
  # and until there is a winsys that fails and takes RADV's cleanup path down through
  # radv_destroy_shader_arenas on a device whose arenas were never created. The process dies mid-write,
  # which truncated the RADV_DEBUG=info dump and hid 57 of its 165 fields.
  echo
  echo "== build the info probe"
  LOADER="$(nix build --no-link --print-out-paths nixpkgs#vulkan-loader 2>/dev/null | head -1)"
  # -I tools, for shaders/pattern_spv.h - the ONE copy of the probe shader, which Tempest's ps4/radv
  # includes from here too rather than keeping a second generated array.
  cc -o "${WORK}/infoprobe" "${ROOT}/tools/infoprobe.c" \
     -I"${ROOT}/tools" -I"${TREE}/include" -L"${LOADER}/lib" -lvulkan -Wl,-rpath,"${LOADER}/lib"

  # ⚠ THE PROBE'S EXIT STATUS, WHICH USED TO BE THROWN AWAY. `| grep ... || true` reports the GREP's status,
  # and `|| true` discards even that - so the first run of this loop ever made printed a Mesa assertion
  # failure, dumped core, and finished with status 0. A loop whose whole purpose is to catch things must not
  # swallow the one signal that says something was caught. Not fatal - a probe that dies on the first
  # unimplemented call is the NORMAL state of this loop - but named, loudly, at the point it happened.
  probe() {
    # ⚠ NO PIPE, AND THAT IS THE WHOLE POINT. Reading the probe's status through a pipe took two attempts and
    # both were wrong: `local rc=${PIPESTATUS[0]}` measures `local` (always 0), and with `set -o pipefail`
    # already on, `... | grep ... || true` runs the `true` and PIPESTATUS becomes 0 anyway. Both reported
    # "exited cleanly" about a probe that had just dumped core - the exact failure this function exists to
    # stop. A file has no such subtlety, and it keeps the UNFILTERED log for the next question.
    local what rc log
    what="$1"; shift
    log="${WORK}/$(echo "${what}" | tr -c 'a-zA-Z0-9' '-').log"
    rc=0
    env VK_DRIVER_FILES="${TREE}/build-hostorbis/src/amd/vulkan/radeon_devenv_icd.x86_64.json" \
        MESA_LOG_LEVEL=info "$@" > "${log}" 2>&1 || rc=$?
    grep -E "infoprobe|dispatch|draw|orbis|amdgpu:|radv:|Assertion|assert" "${log}" || true
    if [[ ${rc} -ne 0 ]]; then
      # 128+n is a signal; SIGABRT is 134, which is what a failed assert() looks like from here.
      echo "   !! ${what} exited ${rc}$([[ ${rc} -ge 128 ]] && echo " (signal $((rc - 128)))")"
      HOSTORBIS_PROBE_FAILED=yes
    fi
  }
  HOSTORBIS_PROBE_FAILED=no

  echo
  echo "== run it - the log names the first query that is not implemented yet"
  probe "the query probe" env RADV_DEBUG=startup "${WORK}/infoprobe"

  echo
  echo "== the whole radeon_info struct, for the diff against ~/src-ps4/ps4-mesa-docs/notes/radeon_info-bonaire.txt"
  env VK_DRIVER_FILES="${TREE}/build-hostorbis/src/amd/vulkan/radeon_devenv_icd.x86_64.json" \
      RADV_DEBUG=info MESA_LOG_LEVEL=info "${WORK}/infoprobe" > "${WORK}/radeon_info-orbis.txt" 2>&1 || true
  echo "   ${WORK}/radeon_info-orbis.txt ($(grep -cE '^ +[a-z_0-9]+ = ' "${WORK}/radeon_info-orbis.txt" || true) fields)"

  # AND THE SAME LOOP ONE LAYER DEEPER, now that winsys/amdgpu/ is built for this platform: vkCreateDevice
  # goes through radv_create_winsys into the winsys' own ac_drm_* calls, so the log names the first
  # function of the memory/submit/syncobj groups that RADV actually needs - in ITS order, not the plan's.
  echo
  echo "== vkCreateDevice - the log names the first winsys-side function that is not written yet"
  probe "the create-device probe" env RADV_DEBUG=startup "${WORK}/infoprobe" --create-device

  # ⚠ AND THE PRESENT PATH, WHICH IS NOT REACHABLE FROM infoprobe. A present with wait semaphores and no
  # command buffers is short-circuited by WSI into vk_device_copy_semaphore_payloads, and that is where a
  # title died on the console with an empty log - through TWO different NULL entries of this arm's sync
  # provider, found weeks apart. Nothing else in this tree exercises it, so nothing else could have caught
  # either one; presentprobe reproduced the second in one run on this machine.
  echo
  echo "== build the present probe"
  cc -o "${WORK}/presentprobe" "${ROOT}/tools/presentprobe.c" \
     -I"${TREE}/include" -L"${LOADER}/lib" -lvulkan -Wl,-rpath,"${LOADER}/lib"

  echo
  echo "== one swapchain present, all the way to the flip"
  probe "the present probe" "${WORK}/presentprobe"

  # ⚠ AND THE TWO PATHS A SUCCESSFUL PRESENT NEVER TOUCHES. Both are error handling in
  # wsi_common_headless.c, both were wrong for the whole life of this port, both were found by reading
  # and fixed without a test - and the Vulkan CTS marks WSI NotSupported on this target, so nothing else
  # in the world was ever going to run them.
  #
  # --images 3 is not a detail. With four images the WSI registers the swapchain's OWN images with
  # video-out and a present copies nothing, so a present that reads a freed image reads it with a pointer
  # nobody dereferences and the run is quiet. Three images puts the copy back, and then the teardown
  # really does memcpy 8100 KiB out of the image - which is what the destroy used to do AFTER freeing it.
  echo
  echo "== a swapchain destroyed with a frame still owed to the display, 8 times"
  probe "the create/destroy probe" "${WORK}/presentprobe" --create-destroy 8 --images 3

  echo
  echo "== presents whose flips fail - does vkAcquireNextImageKHR starve?"
  probe "the starvation probe" env ORBIS_WSI_FAIL_FLIP=1 "${WORK}/presentprobe" --starve 40

  # A poll and an expired wait are different answers, and this WSI gave the same one for both. The layer
  # about to sit on this acquire (zink/kopper) reads the difference, and reads VK_NOT_READY from a wait
  # as a swapchain that has stopped working.
  echo
  echo "== what an empty swapchain says: VK_NOT_READY to a poll, VK_TIMEOUT to an expired wait"
  probe "the acquire-timeout probe" "${WORK}/presentprobe" --acquire-timeout


  echo
  if [[ "${HOSTORBIS_PROBE_FAILED}" == "yes" ]]; then
    echo "== host-orbis result: a probe did NOT exit cleanly - see the !! lines above"
  else
    echo "== host-orbis result: every probe exited cleanly"
  fi
fi

# ---------------------------------------------------------------- the OpenGL arm, optionally
#
# ⚠ THIS IS A GATE, NOT A PRODUCT. Nothing on the console uses OpenGL yet; what this arm protects is the
# fact that the SAME meson tree has to answer two very different questions - "Vulkan only, no gallium" and
# "gallium + EGL + GLES on a platform with no DRM and no dynamic loader" - and the second one exercises
# code paths the first never reaches. Four of them broke the moment they were first compiled:
#
#   meson.build             with_dri is gated on KMS/DRM, with kgsl and darwin as the two zink-without-DRM
#                           exceptions. Orbis is the third. Without it: "EGL requires DRI, Haiku, Windows
#                           or Android".
#   frontends/dri           dri2.c was compiled only for with_dri2, which every previous with_dri build
#                           also had. It holds dri_create_image, dri_interop_*, dri_set_damage_region and
#                           nine more that the whole frontend calls unconditionally.
#   the three GL targets    libgallium_dri, libEGL and libGLESv2 were hard shared_library(). There is no
#                           dlopen on this console and the link line is an executable's, so a .so link
#                           dies on an undefined `main` - or, here, on 60 duplicate symbols between
#                           libc.a's C11 threads and Mesa's.
#   util/futex.h            the SDK's libc++ __config_site does `#undef __FreeBSD__`, so C and C++ in the
#                           SAME BUILD disagreed about UTIL_FUTEX_SUPPORTED - which changes the size of
#                           simple_mtx_t and struct util_queue_fence. That one is an ABI split, and it was
#                           only ever going to be found by a build with enough C++ in it to trip over.
#
# It builds and it LINKS; it does not run. See ps4-mesa-docs for what is still missing at runtime
# (zink's util_dl_open of libvulkan.so.1, kopper's per-platform surface arms, and an EGL platform that is
# not 'surfaceless').
if [[ "${GL}" == "yes" ]]; then
  echo
  echo "== configure (orbis, gallium + EGL + GLES2 via zink)"
  cd "${TREE}"
  GL_SETUP=()
  [[ -d build-orbis-gl ]] && GL_SETUP=(--reconfigure)
  # Deliberately NOT COMMON_OPTS: that array is the Vulkan-only answer, and half of this arm's value is
  # that the two option sets are written out separately and can be compared.
  # -Dgallium-drivers=zink needs no meson change - the empty orbis default sits inside the 'auto' branch.
  # -Dgles1=disabled because GLESv1 wants the fixed-function state tracker for no benefit here.
  nix develop nixpkgs#mesa --command env PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR="${CROSS}/lib/pkgconfig" \
    meson setup build-orbis-gl "${GL_SETUP[@]}" --cross-file "${CROSS}/orbis.ini" \
    -Ddefault_library=static -Dwrap_mode=nodownload \
    -Dzstd=disabled -Dvalgrind=disabled -Dlmsensors=disabled -Dlibunwind=disabled \
    -Dvulkan-drivers=amd -Dgallium-drivers=zink \
    -Dopengl=true -Dgles1=disabled -Dgles2=enabled \
    -Degl=enabled -Dglx=disabled -Dgbm=disabled -Dglvnd=disabled \
    -Dllvm=disabled -Dvideo-codecs= -Dbuildtype=release \
    "${ORBIS_OPTS[@]}"

  echo "== build (orbis, gallium)"
  GLLOG="${WORK}/build-orbis-gl.log"
  nix develop nixpkgs#mesa --command ninja -C build-orbis-gl -k 0 2>&1 | tee "${GLLOG}" || true
  echo
  echo "== orbis gallium result"
  echo "   failed targets: $(grep -c '^FAILED:' "${GLLOG}" || true)"
  echo "   compile errors: $(grep -cE '[.](c|cc|cpp|h|hpp):[0-9]+:[0-9]+: (fatal )?error:' "${GLLOG}" || true)"
  grep -q '^FAILED:' "${GLLOG}" && die "the gallium arm did not build - see ${GLLOG}"

  # ⚠ THE LINK IS THE POINT. The Vulkan arm learned this the expensive way (21 undefined symbols behind a
  # configure that reported success); the GL arm has four archives instead of one and so has four times
  # the surface for the same mistake.
  echo
  echo "== GL link probe (a PS4 executable against EGL + GLESv2 + gallium + RADV)"
  "${ROOT}/tools/gllinkprobe.sh" "${TREE}/build-orbis-gl" "${SDK}" || \
    die "GL LINK PROBE FAILED - the archives are not self-contained; read the undefined symbols above"
fi

# ---------------------------------------------------------------- the regression gate
#
# ⚠ EVERY MESON EDIT FOR THIS PORT TOUCHES A SHARED TREE, AND THREE BUILD BREAKS HAVE ALREADY REACHED
# review that way: changes made for the console that broke ordinary builds nobody on this project
# compiles. meson.build, src/egl/meson.build, src/gallium/targets/dri/meson.build and the two glapi
# entry-point libraries are all read by every Mesa configuration in the world; a with_platform_orbis
# guard that is subtly wrong is invisible here and obvious to anyone else.
#
# Two configurations, because they fail differently:
#
#   linux, gallium+zink+EGL   the one that shares code with the console arm. It is built TWICE, with and
#                             without glvnd, because -Dglvnd=enabled skips src/mesa/glapi/es1api and
#                             es2api entirely (src/meson.build gates those on `not with_glvnd`) - so the
#                             glvnd build cannot see a mistake in either of them, and nix's devShell
#                             detects glvnd by default.
#   linux, Vulkan only        the existing build-host configuration, which is what --host-too builds and
#                             what the development loop runs against the drm-shim.
#
# Deliberately -Dplatforms= : the point is the shared meson logic, not X11 or wayland, and requiring
# those makes the gate depend on what the devShell happens to carry.
if [[ "${REGRESS}" == "yes" ]]; then
  echo
  echo "== regression gate: plain Linux, gallium + zink + EGL (glvnd as detected)"
  cd "${TREE}"
  REG_SETUP=()
  [[ -d build-regress-gl ]] && REG_SETUP=(--reconfigure)
  nix develop nixpkgs#mesa --command meson setup build-regress-gl "${REG_SETUP[@]}" \
    -Dplatforms= -Dvulkan-drivers=amd -Dgallium-drivers=zink \
    -Dopengl=true -Dgles1=enabled -Dgles2=enabled -Degl=enabled -Dglx=disabled -Dgbm=disabled \
    -Dllvm=disabled -Dvideo-codecs= -Dbuildtype=release
  nix develop nixpkgs#mesa --command ninja -C build-regress-gl \
    || die "the plain-Linux gallium build is broken - an orbis change reached a shared meson file"

  echo
  echo "== regression gate: the same, with -Dglvnd=disabled (this is what builds es1api/es2api)"
  nix develop nixpkgs#mesa --command meson setup build-regress-gl --reconfigure \
    -Dplatforms= -Dvulkan-drivers=amd -Dgallium-drivers=zink \
    -Dopengl=true -Dgles1=enabled -Dgles2=enabled -Degl=enabled -Dglx=disabled -Dgbm=disabled \
    -Dglvnd=disabled -Dllvm=disabled -Dvideo-codecs= -Dbuildtype=release
  nix develop nixpkgs#mesa --command ninja -C build-regress-gl \
    || die "the plain-Linux gallium build without glvnd is broken - check src/mesa/glapi/es{1,2}api and src/egl"

  echo
  echo "== regression gate: plain Linux, Vulkan only (the build-host configuration)"
  REG_VK_SETUP=()
  [[ -d build-regress-vk ]] && REG_VK_SETUP=(--reconfigure)
  nix develop nixpkgs#mesa --command meson setup build-regress-vk "${REG_VK_SETUP[@]}" -Dtools=drm-shim \
    "${COMMON_OPTS[@]}" "${HOST_OPTS[@]}"
  nix develop nixpkgs#mesa --command ninja -C build-regress-vk \
    || die "the plain-Linux Vulkan-only build is broken - this is the configuration --host-too uses"

  echo "   both plain-Linux configurations build."
fi

# ---------------------------------------------------------------- the Windows arm
#
# ⚠ NOTHING IN THIS PROJECT SHIPS FOR WINDOWS, WHICH IS EXACTLY WHY THIS EXISTS. RADV carries #ifdef
# _WIN32 arms that this port has had to edit - radv_physical_device.c's device-creation path and
# ac_gpu_info.c's sync-provider probe both grew a Windows case while being changed for Orbis - and a
# review found a defect in one of them that no build in this repository could have caught. It was
# "fixed" once already without ever being compiled; the second attempt did not compile either
# (`struct radeon_winsys_info winsys_info;` is an incomplete type on Windows, because the header that
# defines it is excluded there). Five minutes of mingw is cheaper than that loop.
#
# mingw rather than MSVC because it runs on this machine. Upstream's Windows CI is clang-cl and there are
# differences - see the -mbmi note below - but the PREPROCESSOR arms are what this gate is about, and
# those are the same either way.
if [[ "${MINGW}" == "yes" ]]; then
  echo
  echo "== windows arm: mingw-w64 cross build of RADV"
  # ⚠ grep -v '-man$', AND IT IS NOT COSMETIC. `nix build --print-out-paths` prints EVERY output of the
  # derivation, one per line and not in a documented order; the gcc wrapper has two, and a plain
  # `head -1` picked the -man one. meson then reported "Unknown compiler(s)" about a path that exists,
  # which reads like a broken toolchain and is a broken shell pipeline.
  MINGW_PREFIX="$(nix build --no-link --print-out-paths 'nixpkgs#pkgsCross.mingwW64.buildPackages.gcc' | grep -v -- '-man$' | head -1)"
  [[ -x "${MINGW_PREFIX}/bin/x86_64-w64-mingw32-gcc" ]] || die \
    "no mingw gcc under ${MINGW_PREFIX} - nixpkgs' output layout for pkgsCross.mingwW64 has changed"
  # ⚠ THREE NIX PATHS, AND ALL THREE ARE NEEDED. Outside its own stdenv the mingw gcc wrapper gets none
  # of the flags nixpkgs would normally hand it, so:
  #   mcfgthreads      gcc 15's default threading model here. Without its lib, EVERY link probe meson
  #                    runs fails on "cannot find -lmcfgthread" and configure dies on -latomic; without
  #                    its headers, every C++ file fails on <mcfgthread/gthr.h>.
  #   directx-headers  src/vulkan/wsi/wsi_common_win32.cpp includes directx/d3d12.h.
  MCF="$(nix build --no-link --print-out-paths 'nixpkgs#pkgsCross.mingwW64.windows.mcfgthreads' | grep -v -- '-dev$' | head -1)"
  MCF_DEV="$(nix build --no-link --print-out-paths 'nixpkgs#pkgsCross.mingwW64.windows.mcfgthreads.dev' | head -1)"
  DXH="$(nix build --no-link --print-out-paths 'nixpkgs#pkgsCross.mingwW64.directx-headers' | head -1)"
  [[ -f "${MCF}/lib/libmcfgthread.a" && -f "${MCF_DEV}/include/mcfgthread/gthr.h" && -f "${DXH}/include/directx/d3d12.h" ]] || die \
    "mingw support libraries not where expected (mcfgthread lib/headers, directx-headers) - see the paths above"

  MINGW_INI="${WORK}/mingw.ini"
  mkdir -p "${WORK}"
  # ⚠ -mbmi/-mbmi2/-mlzcnt IS A GCC-ONLY WORKAROUND AND NOT A MESA BUG. addrlib's addrcommon.h calls
  # ::_tzcnt_u32 unguarded; clang-cl accepts that, gcc refuses to inline an always_inline intrinsic
  # whose target feature is off ("target specific option mismatch"). Upstream builds this file with
  # clang, so the flag belongs to this cross file rather than to the tree.
  cat > "${MINGW_INI}" <<INI
[binaries]
c = '${MINGW_PREFIX}/bin/x86_64-w64-mingw32-gcc'
cpp = '${MINGW_PREFIX}/bin/x86_64-w64-mingw32-g++'
ar = '${MINGW_PREFIX}/bin/x86_64-w64-mingw32-ar'
strip = '${MINGW_PREFIX}/bin/x86_64-w64-mingw32-strip'
windres = '${MINGW_PREFIX}/bin/x86_64-w64-mingw32-windres'
pkg-config = 'false'
cmake = 'false'

[host_machine]
system = 'windows'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'

[built-in options]
c_args = ['-I${MCF_DEV}/include','-I${DXH}/include']
cpp_args = ['-mbmi','-mbmi2','-mlzcnt','-I${MCF_DEV}/include','-I${DXH}/include']
c_link_args = ['-L${MCF}/lib']
cpp_link_args = ['-L${MCF}/lib']

[properties]
needs_exe_wrapper = true
INI

  cd "${TREE}"
  # No --reconfigure arm: cross-file [built-in options] are read on the FIRST setup only, so a changed
  # mingw.ini silently does nothing on a reconfigure. This directory is cheap; wipe it.
  rm -rf build-mingw
  nix develop nixpkgs#mesa --command env PKG_CONFIG_PATH= PKG_CONFIG_LIBDIR="${CROSS}/lib/pkgconfig" \
    meson setup build-mingw --cross-file "${MINGW_INI}" \
    -Dvulkan-drivers=amd -Dgallium-drivers= -Dplatforms=windows \
    -Dopengl=false -Dgles1=disabled -Dgles2=disabled -Degl=disabled -Dglx=disabled -Dgbm=disabled \
    -Dllvm=disabled -Dvideo-codecs= -Dzstd=disabled -Dzlib=disabled -Dshader-cache=disabled \
    -Dwrap_mode=nodownload -Dbuildtype=release
  nix develop nixpkgs#mesa --command ninja -C build-mingw \
    || die "the Windows arm does not build - this is the gap that let a never-compiled _WIN32 fix ship twice"
  echo "   vulkan_radeon.dll: $(stat -c%s build-mingw/src/amd/vulkan/vulkan_radeon.dll) bytes"
fi
