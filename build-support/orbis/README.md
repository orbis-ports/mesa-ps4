# RADV for the PS4 (OpenOrbis)

Everything needed to cross-build Mesa's RADV for `x86_64-pc-freebsd12-elf` and link it into a PS4
executable: a meson cross file, seven shim headers, probes, and `build.sh`.

```sh
build-support/orbis/build.sh                 # cross-build for the console
build-support/orbis/build.sh --host-orbis    # same driver + our arm, as a laptop ICD - the dev loop
```

Toolchain: OpenOrbis v0.5.4 at `~/.local/opt/openorbis`, or `$OO_PS4_TOOLCHAIN`. Build deps come from
`nix develop nixpkgs#mesa`.

## Status

**The driver compiles in full for `x86_64-pc-freebsd12-elf` and links into a PS4 executable.**

    libvulkan_radeon.a    34.3 MB, zero compile errors
    link probe            26.4 MB, ELF64 / DYN / x86-64, interp /libexec/ld-elf.so.1

That is the shape an OpenOrbis eboot is, so the driver can physically ship. What it has not done yet is
present a frame: WSI over `sceVideoOut` is the one batch still outstanding.

| batch | what | state |
|---|---|---|
| 00 | this scaffolding | landed |
| 01 | `util/futex.c`, `util/detect_os.h` | landed |
| 02 | `-Dplatforms=orbis`, and `MESA_SYSTEM_HAS_KMS_DRM` for the `_WIN32` guards in `src/amd/` | landed |
| 03 | `vk_drm_syncobj.c` decoupled from libdrm | landed |
| 03b | `CHIP_LIVERPOOL` / `CHIP_GLADIUS` as real families | landed |
| 04 | `winsys/amdgpu/` off the one DRM call it makes | landed |
| 05 | `ac_orbis_drm.c` — the `ac_drm_*` arm | landed |
| 06 | `radv_orbis_winsys.c` and the enumeration hook | landed |
| 07 | `wsi_orbis.c` over `sceVideoOut` | pending |

Carried across but **not** yet re-applied, each needing to be confirmed against a log rather than taken
on faith - see `~/src-ps4/ps4-mesa-docs/docs/` for what the earlier effort measured:

* the VA guard page, reserved with a hardcoded 4096 while mappings round to `getpagesize()`
* `PA_SC_RASTER_CONFIG`, now answered by the family entry rather than an override — worth re-measuring
* `PA_CL_NANINF_CNTL`, which `PKT3_CLEAR_STATE` was measured leaving uninitialised on this console
* the tile swizzle, disabled as a workaround at a cost of 145 ms of GPU per frame
* `amdgpu_devices.c`'s Liverpool entry, which is for the host drm-shim rather than the console

## Why this builds at all: two findings

**One.** The PS4's target triple **is** `x86_64-pc-freebsd12-elf`, clang defines `__FreeBSD__ 12` for it,
and Mesa supports FreeBSD — so meson's own arms and Mesa's build plumbing need no invention.

> **The PS4 is a FreeBSD *kernel* with a musl *libc*, and nobody assumes that combination.**

`__FreeBSD__` is defined while none of FreeBSD's libc interfaces exist: no `sys/umtx.h`, no
`machine/cpu.h`, no `struct kinfo_file`, no `KERN_PROC_ARGS`; and `cpu_set_t` is present but spelled the
Linux way and hidden behind `_GNU_SOURCE`. So the two halves of "FreeBSD" get opposite answers, and
`orbis-compat/cmake/orbis.ini.in` spells out which is which — raw `__FreeBSD__` is kept and load-bearing for the futex,
`DETECT_OS_FREEBSD` is suppressed because that is what `util/` uses to reach for FreeBSD's *libc*.

**Two, and it is the one that made the build work.** Mesa spells "there is no DRM kernel interface" as
`_WIN32` and `with_platform_windows`, but the predicate already exists as a 0/1 C macro,
`MESA_SYSTEM_HAS_KMS_DRM`, already 0 on exactly the platforms where `_WIN32` is defined. The earlier
effort spent six rounds *faking* libdrm rather than setting that switch; setting it took 97 failed objects
and 244 errors to zero. Account in `~/src-ps4/ps4-mesa-docs/docs/research/03-the-platform-predicate.md`.

## Layout

| path | what |
|---|---|
| ~~`cross/`~~ | **moved 2026-08-21** to `orbis-compat/cmake/orbis.ini.in`, beside `ps4-openorbis.cmake`. The two are the same job for two build systems and were drifting apart in separate repositories; `build.sh` reads it from there |
| ~~`shims/`~~ | **gone.** The seven headers OpenOrbis does not ship live in `orbis-compat`, which every component of this port already needs; the cross file puts its include directory first |
| ~~`cts/`~~ | **moved 2026-08-21.** The run configuration and `qpa-status.py` are in the CTS fork at `targets/orbis/`; the account of what the CTS found is `ps4-mesa-docs/cts.md` |
| `tools/linkprobe.{c,sh}` | links a minimal PS4 executable against the driver. An archive is not a driver until something links it |
| `tools/infoprobe.c` | enumerates and dumps `radeon_info` without `vkCreateDevice`, so the dump is not truncated by a cleanup path |
| `tools/acoprobe.c` | creates one compute pipeline from a `.sprv`, so ACO compiles for a GPU that is not in the machine |
| `tools/tilecheck.{cpp,sh}` | tiling and surface-layout probe. Kept for an open item: point it at a layered MSAA image - multiview + multisample halts a CTS run and was never investigated |
| `tools/gen-tile-tables.py` | ⚠ **generates `src/amd/common/orbis_tile_tables.h`**, which is a driver source. Not a probe; do not remove it as one |
| ~~`notes/`~~, ~~`docs/`~~ | **moved 2026-08-21** to `~/src-ps4/ps4-mesa-docs/`. Neither is about building; both are the record of the port |

⚠ `ps4-mesa-docs/docs/PLAN.md` and `TODO.md` were written against the pre-fork tree. Their **findings**
hold and their function lists are still the work list; their **"DONE" markers describe that tree, not
this one** — this tree's progress is the batch table above.

⚠ `PLAN.md` §8 and HANDOFF's "upstreamable list" are **dead text**: on 2026-08-21 the decision was made
that nothing from this port goes to Mesa. That does not cover ZenKit or OpenGothic, which are separate
projects with open pull requests.

## Cross-build hygiene: three doors the host gets in through

Each produced a build that looked like it was cross-compiling and was not. Same mistake three times, and
the third was found only by linking:

1. **`PKG_CONFIG_PATH`.** nix's devShell sets it, and it **overrides the cross file's own
   `pkg_config_libdir`**. Six rounds silently used the host's libdrm, spirv-tools, libunwind and valgrind.
   The tell was `std::__cxx11::` symbols — libstdc++ ABI — in a build that links libc++. `build.sh` clears
   it and points `PKG_CONFIG_LIBDIR` at an **empty but existing** directory, because a path that does not
   exist leaves pkg-config's behaviour ambiguous.
2. **meson's cmake fallback.** `dependency()` tries cmake package files when pkg-config finds nothing, and
   nix carries cmake configs for host libraries. That is how zlib was reported `found: YES 1.3.1.zlib-ng`
   for a PS4 build whose own pkg-config correctly said no. The cross file sets `cmake = 'false'`.
3. **`-nostdlib` does not remove the default library SEARCH PATHS.** `cc.find_library('elf')` resolved to
   `/usr/lib/libelf.so` — the host's, for an `x86_64-pc-freebsd12-elf` target — so Mesa compiled its
   libelf-dependent RGP code and the archive then wanted 21 symbols nothing could supply. Fixed with
   `--sysroot` on both the compile and link lines.

**Any `dependency()` or `cc.find_library()` in a Mesa cross build is a place the host can leak in, and the
symptom is a *successful* configure.** The link is what catches it, which is why `build.sh` performs one.

## A changed cross file must wipe the build directory

meson caches dependency lookups. After fixing the cross file so the host's zlib and libelf were no longer
visible, a reconfigure still printed `Dependency zlib found: YES (cached)` — the fix looked broken when it
had simply not been tested. `build.sh` stamps the cross file's hash and wipes `build-orbis` when it changes.

## Licence

MIT, matching Mesa. shadPS4, GPCS4 and fpPS4 are **not** sources for anything here — see the clean-room
note in the Tempest fork's `AGENTS.md`.
