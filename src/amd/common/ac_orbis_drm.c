/*
 * Copyright 2026 Mikołaj Mikołajczyk
 * SPDX-License-Identifier: MIT
 *
 * The PS4 arm of ac_linux_drm - a third backend beside amdgpu and virtio.
 *
 * This file REPLACES src/amd/common/ac_linux_drm.c rather than patching it. That file is one entry in
 * src/amd/common/meson.build and its header declares every function out-of-line, so a file of ours
 * providing the same symbols is a drop-in that never fights an upstream rebase.
 *
 * FORTY-EIGHT FUNCTIONS, IN TWO HALVES THAT ARRIVED SEPARATELY. ac_linux_drm.h declares 59, of which 54
 * exist on gfx7 and 48 are actually referenced by this build. The first thirteen are what RADV needs with
 * no winsys at all - enough to enumerate a device, and they are confirmed on real hardware. The other
 * thirty-five arrived the moment winsys/amdgpu/ was built for this platform, because that winsys is what
 * calls them; they are at the bottom of this file. Regenerate either list from tools/linkprobe.sh's output;
 * the linker is the authority, not this comment.
 *
 * EVERY BODY MUST FAIL, and three of these cannot fail by return value. ac_drm_query_has_vm_always_valid
 * returns void and writes into radeon_info; ac_drm_device_deinitialize returns void; and
 * ac_drm_device_get_sync_provider returns a pointer that RADV dereferences without checking. A body that
 * quietly does nothing there is a wrong answer with no error anywhere - which is the failure mode this
 * project has spent the most console flashes on. Each is handled explicitly below.
 */

#include "ac_linux_drm.h"
#include "ac_gpu_info.h"
#include "orbis_tile_tables.h"

#include "util/log.h"
#include "util/macros.h"
#include "util/os_time.h"
#include "util/simple_mtx.h"
#include <pthread.h>
#include <sys/syscall.h>
#include <sys/mman.h>

#include "util/futex.h"
#include "util/u_atomic.h"
#include "util/orbis_api_probe.h"
#include "util/u_sync_provider.h"

#if defined(__PS4__)
/* ⚠ AT THE TOP, WITH THE OTHER HEADERS, and that is the same rule as the file-static state below: these used
 * to live inside the backing-store seam, three hundred lines down, so anything above that point could not call
 * a Sony function. That cost a fifth ordering failure in one day. The dependencies of this file run upwards -
 * the poll yields with sceKernelUsleep, the submit calls sceGnmSubmitCommandBuffers, the arena maps direct
 * memory - so the declarations belong above all of them. */
#include <orbis/GnmDriver.h>
#include <orbis/libkernel.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/resource.h>
#include <string.h>

/* ------------------------------------------------ libkernel's internal memory, per frame, per call
 *
 * ⚠ WHAT THIS IS FOR. Four runs of the RetroArch port have ended with the console repeating
 *
 *     [ScePthread/System] Internal Memory is running out.        technote 235
 *
 * thousands of times until nothing worked. The frontend's watchdog now reads the number behind that
 * message with sceKernelInternalMemoryGetAvailableSize, and it is unambiguous: 14013728 bytes free at
 * startup, 96 bytes free at the failure, and in between a drain of up to 3.7 MB per five seconds -
 * about 12 KB per PRESENTED FRAME - that runs while RetroArch's menu is up and slows to noise while a
 * core is drawing a single textured quad.
 *
 * ⚠ AND EVERY PTHREAD EXPLANATION IS ALREADY DEAD. Across the whole of it pthread_mutex_init,
 * pthread_cond_init, pthread_attr_init and scePthreadCreate all kept succeeding; the category in the
 * message is the logger, not the owner of the pool. vkAllocateMemory is ZERO per frame, so it is not
 * application memory either, and the VA arena is flat at 350 MiB with 114 live ranges, so it is not
 * address space.
 *
 * ⚠ SO THE REMAINING SUSPECT IS THIS FILE'S OWN KERNEL TRAFFIC, and the honest way to test that is to
 * count it rather than to reason about it. Every sceKernel entry point here that creates, maps,
 * protects or releases memory is counted, and the budget report divides the window's counts AND the
 * window's loss of internal memory by the same number of presents. Bytes-lost-per-frame beside
 * calls-made-per-frame is an attribution; either alone is a story.
 *
 * ⚠ THE COUNTING IS A MACRO OVER THE CALL, NOT A WRAPPER FUNCTION, so no call site changes and none
 * can be missed. `(name)(args)` inside the macro does not re-expand, because a function-like macro is
 * only expanded when its name is followed directly by a parenthesis. Checked before relying on it:
 * none of these names is ever used as an address or a cast in this file, only as a call.
 *
 * ⚠ AND THE READING IS SIGNATURE-SAFE. sceKernelInternalMemoryGetAvailableSize is exported by
 * libkernel.so (llvm-nm --dynamic) and declared by no SDK header, so the two plausible Sony shapes -
 * `int f(size_t *out)` and `size_t f(void)` - are both satisfied by calling it with a real pointer to
 * a zeroed scratch and reading BOTH the scratch and the return value. It is weak, so a link that
 * cannot resolve it yields zero rather than a driver that will not build.
 */
#if defined(__PS4__)
static uint64_t orbis_kc[ORBIS_KC_SLOTS];

static const char *const orbis_kc_names[ORBIS_KC_SLOTS] = {
   [ORBIS_KC_DMEM_ALLOC] = "AllocateDirectMemory",
   [ORBIS_KC_DMEM_RELEASE] = "ReleaseDirectMemory",
   [ORBIS_KC_DMEM_MAP] = "MapDirectMemory",
   [ORBIS_KC_FMEM_MAP] = "MapFlexibleMemory",
   [ORBIS_KC_MMAP] = "Mmap",
   [ORBIS_KC_MUNMAP] = "Munmap",
   [ORBIS_KC_MPROTECT] = "Mprotect",
   [ORBIS_KC_SUBMIT] = "SubmitCommandBuffers",
   [ORBIS_KC_BO_ALLOC] = "bo_alloc",
   [ORBIS_KC_BO_FREE] = "bo_free",
   [ORBIS_KC_VA_MAP] = "va_map",
   [ORBIS_KC_VA_UNMAP] = "va_unmap",
   [ORBIS_KC_SYNC_TIMEOUT] = "syncobj_timeout",
   [ORBIS_KC_SYNC_POLL] = "syncobj_poll",
   [ORBIS_KC_USLEEP] = "Usleep",
   [ORBIS_KC_GNM_FLUSH_GARLIC] = "FlushGarlic",
   [ORBIS_KC_GNM_SUBMIT_DONE] = "SubmitDone",
   [ORBIS_KC_GNM_ALLOWED] = "AreSubmitsAllowed",
   [ORBIS_KC_VO_SUBMIT_FLIP] = "VideoOutSubmitFlip",
   [ORBIS_KC_VO_FLIP_STATUS] = "VideoOutGetFlipStatus",
   [ORBIS_KC_VO_REGISTER] = "VideoOutRegisterBuffers",
   [ORBIS_KC_VO_OPEN_CLOSE] = "VideoOutOpen/Close",
};

static inline void
orbis_kc_hit(unsigned slot)
{
   p_atomic_inc(&orbis_kc[slot]);
}

/* The cross-translation-unit door, so wsi_orbis.c books into the same counters and the same report.
   Not static, and declared in util/orbis_api_probe.h beside the enum. */
void
orbis_kc_count(unsigned slot)
{
   if (slot < ORBIS_KC_SLOTS)
      orbis_kc_hit(slot);
}

#define sceKernelUsleep(...) (orbis_kc_hit(ORBIS_KC_USLEEP), (sceKernelUsleep)(__VA_ARGS__))
#define sceGnmFlushGarlic(...)                                                                     \
   (orbis_kc_hit(ORBIS_KC_GNM_FLUSH_GARLIC), (sceGnmFlushGarlic)(__VA_ARGS__))
#define sceGnmSubmitDone(...)                                                                      \
   (orbis_kc_hit(ORBIS_KC_GNM_SUBMIT_DONE), (sceGnmSubmitDone)(__VA_ARGS__))
#define sceGnmAreSubmitsAllowed(...)                                                               \
   (orbis_kc_hit(ORBIS_KC_GNM_ALLOWED), (sceGnmAreSubmitsAllowed)(__VA_ARGS__))
#define sceKernelAllocateDirectMemory(...)                                                         \
   (orbis_kc_hit(ORBIS_KC_DMEM_ALLOC), (sceKernelAllocateDirectMemory)(__VA_ARGS__))
#define sceKernelReleaseDirectMemory(...)                                                          \
   (orbis_kc_hit(ORBIS_KC_DMEM_RELEASE), (sceKernelReleaseDirectMemory)(__VA_ARGS__))
#define sceKernelMapDirectMemory(...)                                                              \
   (orbis_kc_hit(ORBIS_KC_DMEM_MAP), (sceKernelMapDirectMemory)(__VA_ARGS__))
#define sceKernelMapFlexibleMemory(...)                                                            \
   (orbis_kc_hit(ORBIS_KC_FMEM_MAP), (sceKernelMapFlexibleMemory)(__VA_ARGS__))
#define sceKernelMmap(...) (orbis_kc_hit(ORBIS_KC_MMAP), (sceKernelMmap)(__VA_ARGS__))
#define sceKernelMunmap(...) (orbis_kc_hit(ORBIS_KC_MUNMAP), (sceKernelMunmap)(__VA_ARGS__))
#define sceKernelMprotect(...) (orbis_kc_hit(ORBIS_KC_MPROTECT), (sceKernelMprotect)(__VA_ARGS__))
#define sceGnmSubmitCommandBuffers(...)                                                            \
   (orbis_kc_hit(ORBIS_KC_SUBMIT), (sceGnmSubmitCommandBuffers)(__VA_ARGS__))

extern uint64_t sceKernelInternalMemoryGetAvailableSize(void *out, uint64_t out_len, uint64_t zero)
   __attribute__((weak));

/* ⚠ THE ONE CONSUMER THIS DRIVER CANNOT SEE FROM THE INSIDE. Every counter above is a call this file
 * makes on purpose. simple_mtx is not: its contended path is futex_wait, and on this console
 * futex_wait is orbis-compat's _umtx_op shim, which sleeps on a pthread condition variable with a
 * deadline. A cond wait that TIMES OUT is the last unexamined thing on the failing syncobj path, and
 * it is the right shape - allocated on entry, released on the signalled return, never on the expired
 * one. Weak, so a build without the overlay's counters reports zeroes instead of failing to link. */
extern void orbis_umtx_stats(unsigned long long *waits, unsigned long long *timedwaits,
                             unsigned long long *timeouts, unsigned long long *locks)
   __attribute__((weak));

/* The other half of the same argument: two clock reads happen on every one of those waits, and
   146/2 = 73 is the size of a small fixed record. Weak for the same reason. */
extern void orbis_clock_counts(unsigned long long *monotonic, unsigned long long *realtime)
   __attribute__((weak));

static uint64_t
orbis_internal_memory_free(void)
{
   uint64_t scratch[8];
   uint64_t ret;

   if (&sceKernelInternalMemoryGetAvailableSize == NULL)
      return 0;

   memset(scratch, 0, sizeof(scratch));
   ret = sceKernelInternalMemoryGetAvailableSize(scratch, sizeof(scratch), 0);
   return scratch[0] != 0 ? scratch[0] : ret;
}
#endif /* __PS4__ */

/* Once per call site, not once per call: RADV asks these questions at initialisation rates, and a
 * per-call log is thousands of lines nobody reads. The shape mirrors gnmLogOnceFor in the Tempest fork.
 *
 * ⚠ THE ONCE-FLAG IS orbis_budget, WHICH IS DEFINED BELOW THESE MACROS ON PURPOSE. A macro is expanded at
 * its use site, and every use of these three is inside a function far past that definition - so the order
 * reads backwards and compiles forwards. It matters because the flag is shared by whatever threads reach
 * the call site, and these were the last three copies of the hand-written read-then-increment in this file;
 * the regex that converted the other twenty could not see through the line continuations. */
#define ORBIS_DRM_TODO() \
   do {                                                                                            \
      static unsigned _said;                                                                       \
      if (orbis_budget(&_said, 1)) {                                                               \
         mesa_logw("orbis-drm: %s is not implemented yet", __func__);                               \
      }                                                                                            \
   } while (0)

/* A refusal is a decision, not a gap. The log has to distinguish the two or the next reader spends an
 * afternoon implementing something that was never going to exist here. */
#define ORBIS_DRM_REFUSED(why) \
   do {                                                                                            \
      static unsigned _said;                                                                       \
      if (orbis_budget(&_said, 1)) {                                                               \
         mesa_logi("orbis-drm: %s refused - %s", __func__, why); \
      }                                                                                            \
   } while (0)

/* A value this port cannot cite from any oracle. It is reported anyway - refusing would stop the driver
 * dead - but it says so once, by field name, so the RADV_DEBUG=info diff has a companion list of
 * known-unknowns instead of looking uniformly confident. Five of them describe the shader-array topology
 * and are measurable on hardware. */
#define ORBIS_DRM_UNCITED(field, why) \
   do {                                                                                            \
      static unsigned _said;                                                                       \
      if (orbis_budget(&_said, 1)) {                                                               \
         mesa_logw("orbis-drm: %s is UNCITED - %s", field, why); \
      }                                                                                            \
   } while (0)

/* ---------------------------------------------------------------- the virtual address window
 *
 * DEFINED HERE BECAUSE IT IS REPORTED FROM HERE. RADV reads virtual_address_offset/_max out of
 * AMDGPU_INFO_DEV_INFO and then trusts that every address ac_drm_va_range_alloc hands back lands inside
 * them - so the description and the enforcement have to be the same two constants, or they disagree in a way
 * that surfaces as a GPU fault far from its cause.
 *
 * TWO CONSTRAINTS, AND THE FIRST ONE THE CONSOLE CORRECTED:
 *   - amdgpu_sw_info_address32_hi names the ONE 4 GiB window that RADEON_FLAG_32BIT allocations live in, and
 *     it is DERIVED from where the memory actually is - `orbis_va_base >> 32`. Reporting 0 and calling it a
 *     contract was only valid while the window was ours to choose; on hardware the kernel placed the arena at
 *     0x200400000 and reporting 0 made RADV's first shader arena unallocatable.
 *   - address_prt_wa_control_bit is 47, and a NIR pass clears that bit from every SMEM address. It is inert
 *     only while no real address has it set, i.e. while the window stays far below 2^47. The measured arena at
 *     ~8.6 GB is nowhere near it.
 */
/* On the HOST these are the window. On the CONSOLE they are overwritten by the arena the kernel actually
 * gave us, because there the window is not ours to choose - see orbis_arena_setup below. */
#define ORBIS_VA_BASE_DEFAULT 0x200000ull    /* 2 MB: the low pages stay unmapped, so a null VA faults */
#define ORBIS_VA_END_DEFAULT  0x400000000ull /* 16 GB of ADDRESS SPACE, not of memory */
#define ORBIS_VA_32BIT_END    0x100000000ull /* where address32_hi = 0 stops being true */

static uint64_t orbis_va_base = ORBIS_VA_BASE_DEFAULT;
static uint64_t orbis_va_end = ORBIS_VA_END_DEFAULT;

/* ⚠ ASKED, NOT ASSUMED, and a silent -EINVAL is why. radv_amdgpu_bo.c:57 rounds every mapping up with
 * `size = align64(size, getpagesize())`, so the page size is not a detail of ours - it is the granularity
 * RADV has already applied by the time a VA op arrives. This file assumed 4096, which is the host's answer,
 * and on the console a 4 KiB BO therefore arrived asking to map more than it owned. Three places need the
 * real number: the alignment the VA allocator hands out, the size a VA op will accept, and the
 * virtual_address_alignment / gart_page_size this device REPORTS - because RADV aligns to those too. */
/* ⚠ NO os_time_sleep IN A POLL LOOP, AND A HANG IS WHY. (The console arm now has a real yield - see below.) Every wait in this file polls, and until submit #2 on
 * the console none of them had ever actually SLEPT - each one found its value already reached and returned
 * without touching the sleep. The first one that did sleep never came back: the harness stopped inside
 * vkQueueSubmit with the arm's own timing line already printed, in a loop bounded by the monotonic clock, which
 * cannot spin forever. `os_time_sleep` is nanosleep underneath, and on this platform it links, exists and does
 * not return - the fourth SDK function to behave that way after struct stat, std::abs and the C11 mutexes.
 *
 * So these loops are bounded by the CLOCK ALONE. It burns a core while waiting, which for bounded waits in a
 * driver whose alternative is a hung console is the right trade - and if a real scheduler-friendly yield turns
 * up (sceKernelUsleep is the obvious candidate, unverified), this is the one place to put it. */
/* ⚠ A BOUNDED LOG IS STILL A SHARED COUNTER, and this file had eighteen hand-written ones.
 *
 * Every bounded diagnostic here spelled out the same read-test-increment - `if (said < 8) { ++said; ... }` -
 * outside any lock, from entries called on whatever thread the application happens to use. Each race is
 * benign on its own: two threads see the last slot and both spend it, and a line or two extra reaches the
 * log. What is not benign is eighteen copies of a pattern, because the nineteenth gets written from one of
 * them and by then nobody remembers whether it was meant to be safe.
 *
 * The read guard is not redundant with the increment: without it the counter climbs for the life of the
 * process and wraps back through the budget after 2^32 calls, printing the whole quota again hours in. */
static bool
orbis_budget(unsigned *slot, unsigned limit)
{
   return p_atomic_read(slot) < limit && p_atomic_inc_return(slot) <= limit;
}

/* ---------------------------------------------------------------- every wait is BOUNDED
 *
 * ⚠ RADV ASKS FOR INFINITE WAITS AND THIS PLATFORM CANNOT AFFORD TO GRANT THEM.
 * radv_amdgpu_bo.c:1362 passes OS_TIMEOUT_INFINITE to ac_drm_bo_wait_for_idle, and because os_time_sleep does
 * not return here every poll is a SPIN - so an infinite timeout is a core burning forever with no diagnosis.
 * That is what the 1080p scan-out run did: the harness reached vkQueueSubmit, the GPU did not finish, and the
 * process stopped producing output entirely. A hang tells you nothing; a timeout tells you the GPU did not
 * finish, which is the whole difference.
 *
 * ⚠ AND THE ANSWER WAS WRONG FOR FIVE MONTHS: this used to read "no wait in this file exceeds
 * ORBIS_WAIT_CAP_NS", and cutting an UNBOUNDED wait short is not a conservative choice, it is an
 * illegal return value. vkWaitForFences(UINT64_MAX) may not come back VK_TIMEOUT. The cap is now what
 * it should always have been - a period at which a long wait REPORTS ITSELF - and the caller's own
 * deadline is what ends the wait. See orbis_wait_watch below.
 *
 * ONE wait still takes a hard cap: the fence-slot throttle in the submit path, which holds
 * orbis_submit_lock and would otherwise wedge every other thread behind a hung GPU. It calls
 * orbis_deadline directly.
 *
 * Capping was LOUD for the same reason the watchdog is loud - a driver that silently shortens a wait
 * the caller asked for turns a GPU hang into mysterious corruption.
 *
 * FIVE SECONDS because it is far past any legitimate submission on this hardware (the whole 64x64 draw
 * completed in microseconds) and short enough that a human sees a log line rather than a dead console. */
#define ORBIS_WAIT_CAP_NS (5ull * 1000 * 1000 * 1000)

static int64_t
orbis_deadline(uint64_t abs_timeout_ns, const char *what)
{
   const int64_t cap = (int64_t)(os_time_get_nano() + ORBIS_WAIT_CAP_NS);
   if ((int64_t)abs_timeout_ns > cap || abs_timeout_ns == OS_TIMEOUT_INFINITE) {
      static unsigned said;
      if (orbis_budget(&said, 1)) {
         mesa_logw("orbis-drm: capping an unbounded %s wait at %llu s - RADV asked to wait forever and this "
                   "platform polls, so forever means a burning core with no diagnosis",
                   what, (unsigned long long)(ORBIS_WAIT_CAP_NS / 1000000000ull));
      }
      return cap;
   }
   return (int64_t)abs_timeout_ns;
}

/* ⚠ AND FOR A WAIT THE CALLER MEANT TO BE LONGER THAN THE CAP, THE CAP IS A WATCHDOG RATHER THAN AN
 * ANSWER - which is what orbis_deadline turned it into, and that answer is illegal.
 *
 * vkWaitForFences(UINT64_MAX) may not return VK_TIMEOUT: the Vulkan specification says a wait with a
 * timeout of UINT64_MAX waits until the fence signals, full stop. Capping it produced -ETIME, which
 * vk_drm_syncobj.c turns into exactly that VK_TIMEOUT, and a conforming application is entitled to
 * treat it as impossible. The trigger is not exotic either: this port's own logs have >5 s bursts
 * while a level loads, and every wait carries WAIT_FOR_SUBMIT, so a wait entered before its
 * submission has no bound at all.
 *
 * The reason the cap was introduced still stands - "a hang tells you nothing" - but it is a reason
 * to SAY SOMETHING every five seconds, not to lie to the caller. os_time_sleep not returning here is
 * also no longer the situation: orbis_poll_pause yields through sceKernelUsleep now, so a long wait
 * costs a sleeping thread rather than a burning core.
 *
 * The one place that still takes a HARD cap is the fence-slot throttle in the submit path, and that
 * is deliberate: it holds orbis_submit_lock, so waiting forever there wedges every other thread.
 * It calls orbis_deadline directly and says so. */
struct orbis_wait_watch {
   int64_t     caller_deadline; /* what the caller actually asked for, absolute ns */
   int64_t     next_report;
   unsigned    periods;
   const char *what;
};

static void
orbis_wait_begin(struct orbis_wait_watch *w, uint64_t abs_timeout_ns, const char *what)
{
   w->caller_deadline = abs_timeout_ns == OS_TIMEOUT_INFINITE ? INT64_MAX : (int64_t)abs_timeout_ns;
   w->next_report = (int64_t)(os_time_get_nano() + ORBIS_WAIT_CAP_NS);
   w->periods = 0;
   w->what = what;
}

/* true: keep waiting. false: the CALLER's own deadline has passed, so a timeout is the right answer. */
static bool
orbis_wait_continue(struct orbis_wait_watch *w)
{
   const int64_t now = (int64_t)os_time_get_nano();

   if (now >= w->caller_deadline)
      return false;

   if (now >= w->next_report) {
      ++w->periods;
      w->next_report = now + (int64_t)ORBIS_WAIT_CAP_NS;
      mesa_logw("orbis-drm: a %s wait has been running for %llu s and the caller asked for %s - the GPU "
                "has not finished. Still waiting, because returning a timeout the caller did not ask for "
                "would be worse than a slow frame.",
                w->what, (unsigned long long)(w->periods * (ORBIS_WAIT_CAP_NS / 1000000000ull)),
                w->caller_deadline == INT64_MAX ? "no timeout at all" : "longer than this");
   }
   return true;
}

static void
orbis_poll_pause(void)
{
#if defined(__PS4__)
   /* AND THE REAL YIELD IS sceKernelUsleep, WHICH THIS TREE ALREADY RELIES ON. When os_time_sleep turned out
    * not to return here, the fallback was a spin and a note saying sceKernelUsleep was "the obvious unverified
    * candidate". It is not unverified: ps4_idle_forever loops on it forever in every title in ps4/, and the
    * audio probe sleeps three seconds with it. So the poll yields properly instead of burning a core, and 50 us
    * is the same interval os_time_sleep(50) asked for.
    *
    * Worth stating plainly because two SDK sleep paths now disagree: nanosleep behind os_time_sleep does not
    * return, and Sony's own usleep does. */
   sceKernelUsleep(50);
#else
   /* Enough to keep the compiler from eliding the loop and to space out label reads, and nothing more. */
   for (volatile unsigned i = 0; i < 64; i++)
      ;
#endif
}

/* ---------------------------------------------------------------- the trace switch
 *
 * The instruments below found five defects in one day and every one of them was silent by nature: a fence
 * label that never moved, a page that stopped being mapped, a selector dword with an event type in it. They
 * are worth keeping. What is not worth keeping is their COST - one polled the GPU for 10 ms on every submit,
 * another touched memory and queried the kernel on every mapping.
 *
 *     ORBIS_DRM_TRACE=1
 *
 * turns them back on. Off, they compile to a branch on a cached bool. */
static bool
orbis_trace(void)
{
   static int cached = -1;
   if (cached < 0) {
      const char *e = getenv("ORBIS_DRM_TRACE");
      cached = (e && *e && *e != '0') ? 1 : 0;
   }
   return cached == 1;
}

/* How many submissions get the per-stage trace. ORBIS_TRACE_SUBMITS=0 means every one - see the bound's own
 * comment in ac_drm_cs_submit_raw2 for why that stopped being a compile-time 8. */
static uint64_t
orbis_trace_submits(void)
{
   static int64_t cached = -1;
   if (cached < 0) {
      const char *const e = getenv("ORBIS_TRACE_SUBMITS");
      cached = (e != NULL && *e != '\0') ? (int64_t)strtoll(e, NULL, 10) : 8;
      if (cached < 0)
         cached = 8;
   }
   return (uint64_t)cached;
}

static uint64_t
orbis_page_size(void)
{
   static uint64_t cached;
   if (cached)
      return cached;
   const int p = getpagesize();
   /* A defensive floor rather than trust: getpagesize links and is defined here, and this project has been
    * burned by exactly that being insufficient evidence. A zero or nonsensical answer would make every
    * alignment below a no-op. */
   cached = (p >= 4096 && (p & (p - 1)) == 0) ? (uint64_t)p : 4096;
   if ((uint64_t)p != cached)
      mesa_logw("orbis-drm: getpagesize() returned %d - using %" PRIu64 " instead", p, cached);
   return cached;
}

/* ---------------------------------------------------------------- the fence, and it is a GPU write
 *
 * amdgpu's kernel signals a submission's fence for the caller. Nothing does that here, so the arm appends its
 * own end-of-pipe packet to every submit and polls the value it writes. Which is exactly what the Tempest
 * fork's GNM backend already does (gnmdevice.cpp: one monotonic ticket per submit, an EOP writing it into an
 * ONION label the CPU polls) - so this is that mechanism, transcribed, rather than a design.
 *
 * ⚠ TWO EOP EVENTS ARE MANDATORY ON GFX7, and one of them looks redundant. The first drains the engines so
 * the second one's write is trustworthy; the fork established this from two oracles that agree on the
 * sequence and disagree only on the dummy's INT_SEL (radeon's cik.c:3540-3570 field by field, Mesa's
 * ac_cmdbuf_cp.c:518-536 neutering the dummy by its data instead). This follows the radeon shape, which is
 * the one spelled out field by field.
 *
 * Packet layout, from the same place (CIK:1823-1845):
 *   dw0  type-3 header, IT_EVENT_WRITE_EOP, 5 body dwords
 *   dw1  event | EVENT_INDEX(EOP) | TCL1_ACTION_EN | TC_ACTION_EN
 *   dw2  address low, dword-aligned
 *   dw3  address high (16 bits) | DST_SEL | INT_SEL | DATA_SEL
 *   dw4  the value
 *   dw5  zero (the value's high half; unused for a 32-bit write)
 */
#define ORBIS_PM4_TYPE3(op, body) ((3u << 30) | ((((body) - 1) & 0x3FFFu) << 16) | ((uint32_t)(op) << 8))
#define ORBIS_IT_EVENT_WRITE_EOP 0x47
#define ORBIS_IT_WRITE_DATA 0x37
#define ORBIS_EI_EOP 5
#define ORBIS_EV_CACHE_FLUSH_AND_INV_TS 20
#define ORBIS_EOP_TCL1_ACTION_EN (1u << 16)
#define ORBIS_EOP_TC_ACTION_EN (1u << 17)
#define ORBIS_EOP_DST_SEL_MC (0u << 16)
#define ORBIS_EOP_DATA_SEL_32BIT (1u << 29)
#define ORBIS_EOP_INT_SEL_SHIFT 24
/* 2 = "interrupt when the data write is confirmed" (CIK:1845), which is what the fork ships. The dummy raises
 * none, following radeon rather than Mesa. */
#define ORBIS_EOP_INT_SEL 2

/* The ticket every submission is handed, and what the fence label is compared against. Declared here rather
 * than beside ac_drm_cs_submit_raw2 because it IS the fence mechanism, and because ac_drm_bo_wait_for_idle -
 * which sits earlier in this file - needs the last value handed out. */
/* ⚠ ALL FILE-STATIC STATE LIVES UP HERE, AND THAT IS A RULE THIS FILE LEARNED THE HARD WAY. Four separate
 * build failures in one day came from a declaration sitting below its first use: the fence label inside the
 * #if defined(__PS4__) branch, the VA window constants, the submit counter, and this BO table. The cause is
 * structural rather than careless - the file grew by APPENDING sections while its dependencies run the other
 * way: memory needs the fence, the fence needs the arena, the arena needs the page size, and teardown needs
 * all of them. So new state goes here, beside the state it depends on, not beside the functions that use it.
 */

/* AMDGPU_GEM_CREATE_VRAM_CLEARED, spelled out rather than included: this file deliberately does not pull in
 * drm/amdgpu_drm.h, and one bit is a smaller dependency than a kernel header. Value from
 * include/uapi/drm/amdgpu_drm.h (1 << 3), the same header the winsys sets it from. */
#define ORBIS_GEM_CREATE_VRAM_CLEARED (1ull << 3)

struct orbis_bo {
   uint64_t size;
   uint64_t alignment;
   uint32_t preferred_heap; /* AMDGPU_GEM_DOMAIN_VRAM -> GARLIC, GTT -> ONION */
   uint64_t alloc_flags;
   /* The physical allocation: an offset into the pool, which is what both arms deal in. */
   uint64_t phys_offset;
   /* Where ac_drm_bo_va_op_raw last mapped it. Zero until then, and ac_drm_bo_cpu_map says so rather than
    * returning a plausible NULL. */
   void *cpu;
   uint32_t handle;
   struct amdgpu_bo_metadata metadata;
   bool has_metadata;
   /* create_bo_from_user_mem: somebody else's pages, so neither arm may unmap or release them. */
   bool foreign;

   /* ⚠ REAL PHYSICAL BACKING FROM THE OTHER BUS. Zero unless this BO's range was re-backed with GARLIC
    * memory; see orbis_garlic_back(). The address is kept so the arena's own pages can be put back
    * exactly where they were, which is what makes this reversible rather than a one-way door. */
   off_t    garlic_phys;
   uint64_t garlic_addr;
   uint64_t garlic_bytes;
};

/* THE uint32_t HANDLE IS NOT DECORATION. Six functions in this layer take a bo_handle rather than an
 * ac_drm_bo - va_op_raw, va_op_raw2, query_info, set_metadata among them - so the arm needs a handle table,
 * and the winsys gets its handles from ac_drm_bo_export(type_kms). See the note there. */
/* ⚠ A CEILING THE CTS WALKED STRAIGHT INTO. This was a fixed 4096 and
 * dEQP-VK.api.object_management.max_concurrent.command_buffer_primary - a test whose entire purpose
 * is to create as many objects as the driver claims to allow - exhausted it and took the process
 * down with "out of BO slots (4096)".
 *
 * 4096 was never measured against anything; it was a number large enough for a game. The table now
 * grows instead, doubling from that same starting size, because handles are INDICES and stay valid
 * across a realloc while pointers into the array would not. */
#define ORBIS_BO_SLOTS_INITIAL 4096

/* ⚠ WHO IS HOLDING WHAT, and why this had to be built before anything else could be concluded.
 *
 * dEQP-VK.api.object_management.multithreaded_per_thread_device.descriptor_pool hangs. It hung when
 * _umtx_op was ENOSYS, that was fixed, the futex now demonstrably sleeps and wakes - and it hangs
 * still. The ONLY evidence either time was a log that stopped, and a log that stops names nothing.
 *
 * ⚠ AND MAKING THE FUTEX WORK IS ITSELF A SUSPECT. While futex_wait could not block, every
 * contended simple_mtx in Mesa was a spin: a lock taken out of order still made progress, because a
 * spinning thread that is descheduled lets the other one through. A futex that genuinely sleeps
 * turns that same ordering mistake into a permanent, silent park. The repair may have converted a
 * livelock nobody noticed into a deadlock nobody can see.
 *
 * So the arm's eight global locks record who holds them. A deadlock then prints as two locks with
 * different holders and a waiter each, and an acquire count that has stopped moving - which is a
 * NAME rather than a silence.
 *
 * Off by default and gated on the watchdog: the bookkeeping is two atomics per acquisition, which
 * is nothing next to the lock itself, but the submit path takes these often enough that a branch on
 * a cached bool is the honest default.
 */
#define ORBIS_LOCK_SLOTS 16

struct orbis_lock_state {
   void       *mtx;
   const char *name;
   uint64_t    holder;   /* thread, or 0 when free */
   uint64_t    acquires;
   uint32_t    waiters;
};

static struct orbis_lock_state orbis_lock_state[ORBIS_LOCK_SLOTS];

static bool
orbis_lock_watch(void)
{
   static int cached = -1;
   if (cached < 0) {
      const char *e = getenv("ORBIS_WATCHDOG");
      cached = (e != NULL && e[0] != '0') ? 1 : 0;
   }
   return cached != 0;
}

static uint64_t
orbis_self_tid(void)
{
   return (uint64_t)(uintptr_t)pthread_self();
}

/* Lock-free because it is called from inside the lock path: taking a lock to record a lock is how
   an instrument becomes the defect. Claiming a slot races only on first use of each mutex. */
static struct orbis_lock_state *
orbis_lock_slot(void *mtx, const char *name)
{
   for (unsigned i = 0; i < ORBIS_LOCK_SLOTS; i++) {
      void *const seen = p_atomic_read(&orbis_lock_state[i].mtx);
      if (seen == mtx)
         return &orbis_lock_state[i];
      if (seen == NULL && p_atomic_cmpxchg(&orbis_lock_state[i].mtx, (void *)NULL, mtx) == NULL) {
         orbis_lock_state[i].name = name;
         return &orbis_lock_state[i];
      }
   }
   return NULL; /* more locks than slots - the dump says so by omission, which is why it prints the count */
}

static void
orbis_mtx_lock_traced(simple_mtx_t *m, const char *name)
{
   if (!orbis_lock_watch()) {
      (simple_mtx_lock)(m);
      return;
   }

   struct orbis_lock_state *const s = orbis_lock_slot(m, name);
   if (s != NULL)
      p_atomic_inc(&s->waiters);
   (simple_mtx_lock)(m);
   if (s != NULL) {
      p_atomic_dec(&s->waiters);
      p_atomic_set(&s->holder, orbis_self_tid());
      p_atomic_inc(&s->acquires);
   }
}

static void
orbis_mtx_unlock_traced(simple_mtx_t *m)
{
   if (orbis_lock_watch()) {
      struct orbis_lock_state *const s = orbis_lock_slot(m, "?");
      if (s != NULL)
         p_atomic_set(&s->holder, (uint64_t)0);
   }
   (simple_mtx_unlock)(m);
}

/* ⚠ THE PARENTHESES INSIDE THE WRAPPERS ARE LOAD-BEARING: (simple_mtx_lock)(m) does not expand this
   macro, so the wrapper calls the real function instead of itself. */
#define simple_mtx_lock(m)   orbis_mtx_lock_traced((m), #m)
#define simple_mtx_unlock(m) orbis_mtx_unlock_traced((m))

/* ⚠ A HELD LOCK IS NOT A LOCATION, which is where the last run stopped being useful.
 *
 * The dump said orbis_map_lock was held by one thread with ZERO waiters and an acquisition count
 * frozen at 997 across three reports. That is a thread parked INSIDE the critical section - but the
 * section is forty lines long and "somewhere in there" is not an answer. Worse, it is not even
 * distinguishable from a stale record, because nothing else wants the lock and so nothing proves it
 * is really held.
 *
 * A breadcrumb settles both. One global is enough and is race-free by construction: only the holder
 * of orbis_map_lock writes it, and only while holding. If the watchdog prints a step, the lock is
 * genuinely held and the step names where; if it prints "(idle)", the holder record was stale and
 * the arm is not where the process is stuck.
 */
static const char *orbis_map_step = "(idle)";

#define ORBIS_MAP_STEP(s_)                                                                                             \
   do {                                                                                                                \
      if (orbis_lock_watch())                                                                                          \
         p_atomic_set(&orbis_map_step, (s_));                                                                          \
   } while (0)

static bool
orbis_serialise_kernel_mem(void)
{
   static int cached = -1;
   if (cached < 0) {
      const char *const e = getenv("ORBIS_SERIALISE_KERNEL_MEM");
      cached = (e != NULL && e[0] != '0') ? 1 : 0;
   }
   return cached != 0;
}

static simple_mtx_t orbis_kernel_mem_lock = SIMPLE_MTX_INITIALIZER;

/* ⚠ WHICH OF THE THREE, AND WITH WHAT ARGUMENTS. The serialising lock answered its question - the
 * holder of orbis_kernel_mem_lock stops and a second thread waits under it for 24 seconds - but the
 * lock covers map, unmap and protect together, so "a direct-memory call does not return" is as far
 * as it goes. Same breadcrumb as orbis_map_step, for the same reason and with the same guarantee:
 * only the holder writes it, and only while holding.
 *
 * The arguments are in it because the next step after naming the call is reproducing it on its own,
 * and a standalone probe needs the size and the address that did it. */
static char orbis_kmem_step[96] = "(idle)";

static void
orbis_kmem_step_set(const char *what, uint64_t addr, uint64_t size)
{
   if (!orbis_serialise_kernel_mem())
      return;
   snprintf(orbis_kmem_step, sizeof(orbis_kmem_step), "%s 0x%" PRIx64 " + %" PRIu64 " bytes", what, addr,
            size);
}


static simple_mtx_t orbis_bo_lock = SIMPLE_MTX_INITIALIZER;
static struct orbis_bo **orbis_bo_slot;
static uint32_t orbis_bo_slots;

/* Take a slot for `obo` and return its handle, growing the table when it is full. Zero on failure.
 * Called with orbis_bo_lock held. Handles are index+1, so 0 is never valid - DRM's convention. */
static uint32_t
orbis_bo_slot_claim(struct orbis_bo *obo)
{
   for (uint32_t i = 0; i < orbis_bo_slots; i++) {
      if (orbis_bo_slot[i] != NULL)
         continue;
      orbis_bo_slot[i] = obo;
      return i + 1;
   }

   const uint32_t want = orbis_bo_slots ? orbis_bo_slots * 2 : ORBIS_BO_SLOTS_INITIAL;
   struct orbis_bo **grown = realloc(orbis_bo_slot, (size_t)want * sizeof(*grown));
   if (grown == NULL) {
      mesa_loge("orbis-drm: %u BO slots are all taken and the table will not grow to %u", orbis_bo_slots,
                want);
      return 0;
   }

   memset(&grown[orbis_bo_slots], 0, (size_t)(want - orbis_bo_slots) * sizeof(*grown));
   mesa_logi("orbis-drm: BO table grown %u -> %u slots", orbis_bo_slots, want);

   const uint32_t slot = orbis_bo_slots;
   orbis_bo_slot = grown;
   orbis_bo_slots = want;
   orbis_bo_slot[slot] = obo;
   return slot + 1;
}

#define ORBIS_MAX_CONTEXTS 16

static simple_mtx_t orbis_ctx_lock = SIMPLE_MTX_INITIALIZER;
static struct {
   bool taken;
   /* Recorded rather than acted on: kept so that the log can name what was asked for, and so a future
    * mapping onto Sony's own queue priorities has the value it needs. */
   int32_t priority;
} orbis_ctx[ORBIS_MAX_CONTEXTS];

struct amdgpu_va {
   uint64_t base;
   uint64_t size;
   struct amdgpu_va *next; /* sorted by base, so a first-fit walk sees the gaps in order */
   /* Freed as far as RADV is concerned, still held by the allocator until the GPU has passed retire_seq. That is the
    * kernel's reference, kept the only way this arm can keep one. */
   bool     retired;
   uint64_t retire_seq;
};

static simple_mtx_t orbis_va_lock = SIMPLE_MTX_INITIALIZER;
static struct amdgpu_va *orbis_va_list;

/* ⚠ EVERY LIVE MAPPING, SO THAT TWO OF THEM CANNOT SHARE AN ADDRESS UNNOTICED.
 *
 * The defect this exists for: after the heavy GPU-driven frame, the NEXT few submissions arrive with a float at
 * dword 0 of their command stream - 0x4651b10f (13420.26) in one run, 0x4523e4c6 (2622.3) in another, the same value
 * across five submissions of five different sizes, and the same five sizes in both runs. The corruption is in the
 * chunk RADV handed us, before this arm copies it, so something wrote game data over RADV's command buffer.
 *
 * A CPU-side audit of every bound descriptor set found nothing, which narrows it: the descriptors are right when
 * they are written and the damage happens DURING execution. Two live BOs sharing an address does exactly that, and
 * it would explain both faults at once - the game's compute writes landing on a command buffer (bad command), and
 * on a descriptor set (a wild base, then a page fault).
 *
 * Grepping the mapping log for overlapping ranges finds 36 of them, but that proves nothing on its own: BO handles
 * are recycled, so the same range appearing twice is expected when the first BO is gone. Only a mapping that
 * overlaps a LIVE one is a defect, and that needs the arm to remember what is live - which is what this is. */
/* ⚠ THE SECOND CEILING THE SAME TEST FOUND. With the BO table growing,
 * dEQP-VK.api.object_management.max_concurrent.command_buffer_primary stopped killing the process
 * and immediately reported the next fixed number instead: the overlap checker filled at 8192 and
 * announced that it had gone blind. Blind is the dangerous state - it reads as a clean run - so this
 * grows too, by the same doubling. */
#define ORBIS_LIVE_MAPS_INITIAL 8192

struct orbis_live_map {
   uint64_t addr; /* 0 = free slot */
   uint64_t end;
   uint32_t handle;
};

static simple_mtx_t orbis_map_lock = SIMPLE_MTX_INITIALIZER;
static struct orbis_live_map *orbis_live_map;
static unsigned ORBIS_LIVE_MAPS;

/* Called with orbis_map_lock held. Returns a free slot index, growing the table if there is none,
 * or ORBIS_LIVE_MAPS if it cannot grow - the caller then reports going blind, as before. */
static unsigned
orbis_live_map_slot(void)
{
   for (unsigned i = 0; i < ORBIS_LIVE_MAPS; ++i)
      if (orbis_live_map[i].addr == 0)
         return i;

   const unsigned want = ORBIS_LIVE_MAPS ? ORBIS_LIVE_MAPS * 2 : ORBIS_LIVE_MAPS_INITIAL;
   struct orbis_live_map *grown = realloc(orbis_live_map, (size_t)want * sizeof(*grown));
   if (grown == NULL)
      return ORBIS_LIVE_MAPS;

   memset(&grown[ORBIS_LIVE_MAPS], 0, (size_t)(want - ORBIS_LIVE_MAPS) * sizeof(*grown));
   mesa_logi("orbis-drm: live-mapping table grown %u -> %u entries", ORBIS_LIVE_MAPS, want);

   const unsigned slot = ORBIS_LIVE_MAPS;
   orbis_live_map = grown;
   ORBIS_LIVE_MAPS = want;
   return slot;
}

/* ⚠ IS THIS ADDRESS ONE THIS PROCESS MAPPED? The question the hung-stream dump could not answer.
 *
 * RADV's window starts mapped-but-inaccessible and a range becomes real when a BO is mapped into it, so
 * "inside the arena" and "the GPU can touch it" are different facts. The live table is the only record of
 * the second, and until now it was consulted for overlap warnings and never for this.
 *
 * Declared here, beside the table, because that is this file's rule for state; used from the hang dump far
 * below. */
/* ⚠ WHICH MAPPING CONTAINS IT, AND HOW FAR DOES THAT MAPPING REACH. orbis_va_is_mapped answers yes or no
 * about the first few bytes, and that turned out to be the wrong question: a fill shader is handed a base and
 * a length, writes the whole length with no bounds check of its own (its descriptor carries num_records = -1),
 * and the audit was confirming only that the base existed.
 *
 * Returns the containing range's bounds so the caller can print them beside the length the work will use.
 * The comparison is then the reader's to make, which is the right division here - the arm does not know what
 * any particular shader intends to do with a pointer. */
static bool
orbis_va_mapping_of(uint64_t addr, uint64_t *lo, uint64_t *hi, uint32_t *handle)
{
   bool found = false;
   simple_mtx_lock(&orbis_map_lock);
   for (unsigned i = 0; i < ORBIS_LIVE_MAPS && !found; ++i) {
      if (orbis_live_map[i].addr == 0)
         continue;
      if (addr >= orbis_live_map[i].addr && addr < orbis_live_map[i].end) {
         *lo = orbis_live_map[i].addr;
         *hi = orbis_live_map[i].end;
         *handle = orbis_live_map[i].handle;
         found = true;
      }
   }
   simple_mtx_unlock(&orbis_map_lock);
   return found;
}

static bool
orbis_va_is_mapped(uint64_t addr, uint64_t size)
{
   bool found = false;
   simple_mtx_lock(&orbis_map_lock);
   for (unsigned i = 0; i < ORBIS_LIVE_MAPS && !found; ++i) {
      if (orbis_live_map[i].addr == 0)
         continue;
      if (addr >= orbis_live_map[i].addr && addr + size <= orbis_live_map[i].end)
         found = true;
   }
   simple_mtx_unlock(&orbis_map_lock);
   return found;
}

/* ⚠ THE ADDRESSES A COMMAND STREAM HANDS THE COMMAND PROCESSOR, AND WHETHER THEY EXIST.
 *
 * Two packet types carry a 64-bit address the CP dereferences ITSELF, before any shader runs, so a bad one
 * stalls the CP rather than faulting a wave:
 *
 *   PKT3_SET_BASE (0x11)  - where DRAW_INDIRECT reads its arguments from
 *   PKT3_DMA_DATA (0x50)  - source and destination of a CP-driven copy
 *
 * A world load measured 7096 of the first and 2324 of the second in ONE submission, which is why this
 * counts and bounds rather than printing: the interesting output is "n of m were never mapped", followed by
 * the first few, and nothing at all when the answer is none.
 *
 * The walk trusts each packet's own length field, which the same stream's structural walk has already
 * proven consistent end to end - it reached the last dword exactly. */
static void
orbis_audit_hung_addresses(uint64_t seq, const uint32_t *dw, uint32_t n)
{
   uint32_t base_total = 0, base_bad = 0, dma_total = 0, dma_bad = 0, shown = 0;
   uint32_t draw_total = 0, draw_zero = 0, draw_max_verts = 0, draw_max_insts = 0;
   uint32_t sh_total = 0, sh_bad = 0, pred_total = 0, pred_bad = 0, cpw_total = 0, cpw_bad = 0;
   uint64_t last_base = 0, draw_max_va = 0;

   for (uint32_t i = 0; i + 1 < n;) {
      const uint32_t hdr = dw[i];
      if ((hdr >> 30) != 3)
         break;
      const uint32_t cnt = ((hdr >> 16) & 0x3FFF) + 2;
      const uint32_t op = (hdr >> 8) & 0xFF;
      if (i + cnt > n)
         break;

      /* body[0] is the packet's first payload dword, i.e. dw[i + 1]. */
      if (op == 0x11 && cnt >= 4) {
         const uint64_t va = (uint64_t)dw[i + 2] | ((uint64_t)dw[i + 3] << 32);
         ++base_total;
         last_base = va;
         if (va != 0 && !orbis_va_is_mapped(va, 16)) {
            ++base_bad;
            last_base = 0;
            if (shown++ < 8)
               mesa_loge("orbis-drm: submit #%" PRIu64 " SET_BASE at dword %u names 0x%" PRIx64
                         " - NOT MAPPED. DRAW_INDIRECT reads its arguments from there.",
                         seq, i, va);
         }
      } else if (op == 0x24 && cnt >= 5 && last_base != 0) {
         /* ⚠ THE ARGUMENTS THEMSELVES, READ OUT OF THE MEMORY THE CP WOULD HAVE READ THEM FROM.
          *
          * Every address in this stream is mapped - that is the previous measurement - so the remaining way
          * for an indirect draw to wedge the front end is a VALUE. A vertex count of a few billion is not a
          * fault and produces no error anywhere: the geometry engine simply works through it, and from
          * outside that is indistinguishable from a hang.
          *
          * These pages belong to this process and are mapped, so the read is safe. Non-indexed DRAW_INDIRECT
          * takes four dwords at base + data_offset: vertex count, instance count, first vertex, first
          * instance (VkDrawIndirectCommand). */
         const uint64_t args_va = last_base + dw[i + 1];
         if (orbis_va_is_mapped(args_va, 16)) {
            const uint32_t *const a = (const uint32_t *)(uintptr_t)args_va;
            const uint32_t verts = a[0], insts = a[1];
            ++draw_total;
            if (verts == 0)
               ++draw_zero;
            if (verts > draw_max_verts) {
               draw_max_verts = verts;
               draw_max_va = args_va;
            }
            if (insts > draw_max_insts)
               draw_max_insts = insts;
            /* 16 M vertices in one draw is far past anything this world can contain, and a count that large
             * is the shape a stale or never-written argument buffer produces. */
            if ((verts > (1u << 24) || insts > (1u << 16)) && shown++ < 8)
               mesa_loge("orbis-drm: submit #%" PRIu64 " DRAW_INDIRECT at dword %u reads %u vertices x %u "
                         "instances from 0x%" PRIx64 " - the front end will not finish that",
                         seq, i, verts, insts, args_va);
         }
      } else if (op == 0x76 && cnt >= 3) {
         /* ⚠ THIRTY-TWO BIT POINTERS, WHICH IS WHAT THIS DRIVER ACTUALLY EMITS, and the first version of this
          * audit looked for the wrong thing entirely.
          *
          * It searched for 64-bit pairs - a low dword followed by the arena's high dword - and found twenty
          * candidates in 20708 SET_SH_REG packets, which is noise rather than a measurement. The packets carry
          * ONE value each: `00000050 12a54920`. This arm reports address32_hi = 0x2 (derived from the arena,
          * not assumed), so RADV stores descriptor pointers as 32-BIT low halves and the hardware supplies the
          * high half from that constant. 0x12a54920 is the address 0x212a54920.
          *
          * So the reconstruction is the driver's own: splice the value under address32_hi and ask whether the
          * result is a page this process mapped. The CP never reads through these - a wave does, and a
          * faulting wave leaves the submission unfinished exactly the way a stalled CP does.
          *
          * This is the class the port already knows it has, from ac_drm_va_range_free: "a stale descriptor
          * sails through every 'is this address ours' check, because a freed address IS ours". True while the
          * arena stayed mapped for the life of the process; not true since RADV's window was made to start
          * inaccessible and gain access per mapping. The check that could not be written became the cheap one.
          *
          * A value inside the window that is not a pointer will be counted - a size or an offset can land
          * there - which is why only MISSES are reported: a value that resolves to mapped memory says
          * nothing either way, and a value that looks like a pointer into our own window and is NOT mapped is
          * worth a line whatever it turns out to be.
          *
          * body[0] is the register offset; body[1..] are the values. */
         for (uint32_t k = i + 2; k < i + cnt; ++k) {
            const uint64_t va = (orbis_va_base & 0xFFFFFFFF00000000ull) | (uint64_t)dw[k];
            if (va < orbis_va_base || va >= orbis_va_end)
               continue;
            ++sh_total;
            if (!orbis_va_is_mapped(va, 4)) {
               ++sh_bad;
               if (shown++ < 8)
                  mesa_loge("orbis-drm: submit #%" PRIu64 " SET_SH_REG at dword %u loads 0x%08x into shader "
                            "user data - as a 32-bit pointer that is 0x%" PRIx64 ", inside RADV's window and "
                            "NOT MAPPED. A wave reading through it faults.",
                            seq, i, dw[k], va);
            }
         }
      } else if (op == 0x20 && cnt >= 3) {
         /* ⚠ SET_PREDICATION, AND THE AUDIT MISSED IT ON THE FIRST TWO PASSES. The CP READS this address
          * itself to decide whether to skip the draws that follow, so an unmapped one stalls the front end
          * with no shader involved at all - the same class as SET_BASE, and it was simply not in the list.
          *
          * DW1 is the low half; DW2 carries the high byte in [7:0] alongside the predication operation. A
          * body of all zeros is "predication off" and names no address. */
         const uint64_t va = (uint64_t)dw[i + 1] | ((uint64_t)(dw[i + 2] & 0xFF) << 32);
         if (va != 0) {
            ++pred_total;
            if (!orbis_va_is_mapped(va, 8)) {
               ++pred_bad;
               if (shown++ < 8)
                  mesa_loge("orbis-drm: submit #%" PRIu64 " SET_PREDICATION at dword %u reads 0x%" PRIx64
                            " - NOT MAPPED. The CP reads it to decide whether to skip what follows.",
                            seq, i, va);
            }
         }
      } else if ((op == 0x37 || op == 0x40 || op == 0x47) && cnt >= 5) {
         /* WRITE_DATA, COPY_DATA and EVENT_WRITE_EOP each carry a destination the CP writes itself. Few of
          * them - 28, 29 and 2 in the stream this was written for - but they belong in the list for the same
          * reason SET_PREDICATION did: the failure mode is "the audit did not look", and that has now
          * happened twice.
          *
          * The address sits at a different body offset per packet, so each is taken where it lives. Only
          * candidates inside RADV's window are judged; these packets legitimately address the arm's own
          * private slice, which is below the window and never in the live table. */
         const uint32_t at = (op == 0x40) ? i + 5 : i + 2;
         if (at + 1 < i + cnt) {
            const uint64_t va = (uint64_t)dw[at] | ((uint64_t)dw[at + 1] << 32);
            if (va >= orbis_va_base && va < orbis_va_end) {
               ++cpw_total;
               if (!orbis_va_is_mapped(va, 4)) {
                  ++cpw_bad;
                  if (shown++ < 8)
                     mesa_loge("orbis-drm: submit #%" PRIu64 " op 0x%02x at dword %u targets 0x%" PRIx64
                               " - inside RADV's window and NOT MAPPED.", seq, op, i, va);
               }
            }
         }
      } else if (op == 0x50 && cnt >= 7) {
         const uint64_t src = (uint64_t)dw[i + 2] | ((uint64_t)dw[i + 3] << 32);
         const uint64_t dst = (uint64_t)dw[i + 4] | ((uint64_t)dw[i + 5] << 32);
         dma_total += 2;
         if (src != 0 && !orbis_va_is_mapped(src, 4)) {
            ++dma_bad;
            if (shown++ < 8)
               mesa_loge("orbis-drm: submit #%" PRIu64 " DMA_DATA at dword %u reads 0x%" PRIx64
                         " - NOT MAPPED.", seq, i, src);
         }
         if (dst != 0 && !orbis_va_is_mapped(dst, 4)) {
            ++dma_bad;
            if (shown++ < 8)
               mesa_loge("orbis-drm: submit #%" PRIu64 " DMA_DATA at dword %u writes 0x%" PRIx64
                         " - NOT MAPPED.", seq, i, dst);
         }
      }
      i += cnt;
   }

   if (draw_total != 0)
      mesa_logi("orbis-drm: submit #%" PRIu64 " indirect draws: %u read, %u ask for zero vertices; the "
                "largest is %u vertices at 0x%" PRIx64 ", the most instances is %u",
                seq, draw_total, draw_zero, draw_max_verts, draw_max_va, draw_max_insts);

   mesa_logi("orbis-drm: submit #%" PRIu64 " other CP-read addresses: %u/%u SET_PREDICATION and %u/%u "
             "write targets are NOT MAPPED",
             seq, pred_bad, pred_total, cpw_bad, cpw_total);

   /* ⚠ READ THE NEXT NUMBER WITH ITS FALSE POSITIVES IN MIND, and they are not hypothetical: the first run
    * of this check reported thirty misses and every one printed was a round IEEE float - 0x41200000 is 10.0f,
    * 0x44000000 is 512.0f. RADV's window covers the low halves 0x0a404000..0x48400000, which is where small
    * float constants live, and no shape test separates those from a pointer. A non-zero count here is a lead
    * and not a finding until the values are looked at. */
   mesa_logi("orbis-drm: submit #%" PRIu64 " shader user data: %u value(s) resolve into RADV's window as "
             "32-bit pointers, %u of them are NOT MAPPED",
             seq, sh_total, sh_bad);

   mesa_logi("orbis-drm: submit #%" PRIu64 " address audit: %u/%u SET_BASE and %u/%u DMA_DATA addresses are "
             "NOT in any live mapping%s",
             seq, base_bad, base_total, dma_bad, dma_total,
             (base_bad == 0 && dma_bad == 0)
                ? " - so the CP was not sent anywhere this process had not mapped, and the fault is in what "
                  "it READ rather than in where it was told to look"
                : "");
}

/* ⚠ HOW FAR DID THE COMMAND PROCESSOR ACTUALLY GET? The one question static analysis cannot answer, and
 * everything else about the hang has now been answered without it.
 *
 * A submission's end-of-pipe writes the fence label ONCE, at the end, so a stream that stops halfway
 * produces the same silence wherever it stopped. Five audits over the hung stream came back clean - every
 * address the CP dereferences is mapped, every indirect draw is sane, there is no wait packet to block on,
 * and the packet walk reaches the last dword exactly - which leaves "where" as the only thing left to
 * measure, and 157000 dwords is too many to bisect by flashing.
 *
 * So the flattener can staple a WRITE_DATA every N dwords, each writing its own output offset to one word.
 * The last value that lands is the offset the CP reached, and the dump already in the log says which packet
 * lives there.
 *
 * OFF BY DEFAULT AND OFF IN EVERY NORMAL RUN: ORBIS_PROGRESS_MARKS=<dwords> arms it. A marker is five
 * dwords, so 4096 costs about a thousandth of the stream and 40 markers on the submission this exists for.
 * It perturbs nothing else - WRITE_DATA is a CP write with no pipeline sync, unlike the EOP events. */
#if defined(__PS4__)
static volatile uint32_t *orbis_progress_word;
/* ⚠ AND A SECOND WORD, WRITTEN BY THE OTHER END OF THE PIPE. The WRITE_DATA marker is a COMMAND PROCESSOR
 * write: it lands as soon as the CP walks past it, and the CP runs ahead of the work it queues. Measured -
 * the same submission stopped at dword 70992 in one run and 71136 in the next, and both windows end in the
 * same PFP_SYNC_ME / SURFACE_SYNC pair, which are the packets that block the CP until the pipeline drains.
 * So the CP's offset is where it PARKED, not where the defect is.
 *
 * An end-of-pipe event writes only after everything before it has retired. Armed together, the two words
 * bracket the fault: the CP got to A, execution got to B, and whatever never finished is between them. */
static volatile uint32_t *orbis_retired_word;
/* The drain half of the pair needs a destination too, and orbis_fence_scratch is console-only state while
 * this splice is shared by both arms. Its value is never read - only the write matters. */
static volatile uint32_t *orbis_drain_word;
#else
/* ⚠ THE HOST NEEDS ONE TOO, OR THE MARKERS CANNOT BE PROVEN BEFORE A FLASH. Nothing executes on this arm, so
 * the word is never written by a GPU - but orbis_progress_word being non-NULL is what makes the flattener
 * splice the packets at all, and the splicing is exactly what the laptop can check: the stream grows by five
 * dwords per marker and the headers land where they should. Declared HERE rather than beside the fence label,
 * because a static initialiser is a use and this file has lost four builds to that already. */
static uint32_t orbis_host_progress;
static volatile uint32_t *orbis_progress_word = &orbis_host_progress;
static uint32_t orbis_host_retired;
static volatile uint32_t *orbis_retired_word = &orbis_host_retired;
static uint32_t orbis_host_drain;
static volatile uint32_t *orbis_drain_word = &orbis_host_drain;
#endif

/* ⚠ THE EXPENSIVE HALF, AND IT IS A DIFFERENT EXPERIMENT RATHER THAN A BETTER ONE. An end-of-pipe event
 * drains the pipeline, so arming this SERIALISES the stream at every marker - which is precisely what makes
 * the number trustworthy, and precisely what could hide a race. Read the two words together: if execution
 * stops well short of the CP, the work between them is the suspect; if they stop at the same place, the
 * pipeline was never running ahead and the packet at that offset is. */
static bool
orbis_progress_eop(void)
{
   static int cached = -1;
   if (cached < 0) {
      const char *const e = getenv("ORBIS_PROGRESS_EOP");
      cached = (e != NULL && *e != '\0' && *e != '0') ? 1 : 0;
   }
   return cached == 1;
}

/* ⚠ PER-PACKET PRECISION, IN A WINDOW, BECAUSE GLOBAL PER-PACKET DOES NOT FIT.
 *
 * The coarse markers put the stop between two dwords 66 apart, and the packets in that gap include two the
 * CP dereferences memory for - SET_PREDICATION and a COPY_DATA into a register - plus a dispatch. Telling
 * them apart needs a marker between every packet, and every packet costs 17 dwords: over this submission
 * that is 660000 dwords against a 524288-dword staging buffer, so it would simply be refused.
 *
 * ORBIS_PROGRESS_FROM/_TO bound a range where the interval drops to eight dwords - effectively one marker
 * per packet - while the rest of the stream keeps the coarse one. Unset, nothing changes. */
static uint32_t
orbis_progress_from(void)
{
   static int64_t cached = -1;
   if (cached < 0) {
      const char *const e = getenv("ORBIS_PROGRESS_FROM");
      cached = (e != NULL && *e != '\0') ? (int64_t)strtoll(e, NULL, 10) : 0;
   }
   return (uint32_t)cached;
}

static uint32_t
orbis_progress_to(void)
{
   static int64_t cached = -1;
   if (cached < 0) {
      const char *const e = getenv("ORBIS_PROGRESS_TO");
      cached = (e != NULL && *e != '\0') ? (int64_t)strtoll(e, NULL, 10) : 0;
   }
   return (uint32_t)cached;
}

static uint32_t
orbis_progress_marks(void)
{
   static int64_t cached = -1;
   if (cached < 0) {
      const char *const e = getenv("ORBIS_PROGRESS_MARKS");
      cached = (e != NULL && *e != '\0') ? (int64_t)strtoll(e, NULL, 10) : 0;
      if (cached < 64)
         cached = 0; /* Below this the markers outweigh the stream and measure themselves. */
   }
   return (uint32_t)cached;
}

/* ⚠ WATCH A RANGE THAT SHOULD STAY ZERO, AND REPORT THE SUBMISSION AT WHICH IT DOES NOT.
 *
 * Where the hunt is: with the descriptor pool zeroed at creation, the audit still finds real data inside one -
 * `46391d1c 44046408 45f31ab8 45f3647e` at element 1280 of a 3072-entry image array, which decodes as the floats
 * 11847.28, 529.56, 7779.34, 7788.56. World coordinates, written into descriptor memory.
 *
 * Every theory about WHO wrote them has been wrong so far, so this measures WHEN instead, which is a smaller
 * question with a bigger answer: the tail of each pool is checked before every submission, and the first
 * submission at which it stops being zero is printed.
 *
 *   it is already dirty before the first submit  -> the CPU wrote it, and the writer is in RADV or in Tempest
 *   it turns dirty between two submits           -> the GPU wrote it, and the submission number names the frame
 *
 * The tail rather than the whole range, because the front is where RADV legitimately writes descriptors. 4 KiB is
 * enough: an array rounded up to 3072 entries leaves far more than that unwritten. */
#define ORBIS_WATCH_MAX 64

static simple_mtx_t orbis_watch_lock = SIMPLE_MTX_INITIALIZER;
static struct orbis_watch {
   uint64_t va;
   uint64_t size;
   bool     reported;
} orbis_watch[ORBIS_WATCH_MAX];

void
ac_orbis_watch_range(uint64_t va, uint64_t size)
{
   if (getenv("ORBIS_WATCH") == NULL || size < 4096)
      return;
   simple_mtx_lock(&orbis_watch_lock);
   for (unsigned i = 0; i < ORBIS_WATCH_MAX; ++i) {
      if (orbis_watch[i].va == 0) {
         orbis_watch[i].va = va;
         orbis_watch[i].size = size;
         orbis_watch[i].reported = false;
         break;
      }
   }
   simple_mtx_unlock(&orbis_watch_lock);
}

static void
orbis_watch_check(uint64_t seq, const char *when)
{
   if (getenv("ORBIS_WATCH") == NULL)
      return;

   simple_mtx_lock(&orbis_watch_lock);
   for (unsigned i = 0; i < ORBIS_WATCH_MAX; ++i) {
      if (orbis_watch[i].va == 0 || orbis_watch[i].reported)
         continue;
      const uint32_t *const tail = (const uint32_t *)(uintptr_t)(orbis_watch[i].va + orbis_watch[i].size - 4096);
      for (unsigned k = 0; k < 1024; ++k) {
         if (tail[k] == 0)
            continue;
         orbis_watch[i].reported = true;
         mesa_loge("orbis-drm: A WATCHED RANGE STOPPED BEING ZERO - 0x%" PRIx64 " (%" PRIu64
                   " KiB) has 0x%08x at %u dwords into its last 4 KiB, first seen %s submit #%" PRIu64 ".",
                   orbis_watch[i].va, orbis_watch[i].size / 1024, tail[k], k, when, seq);
         break;
      }
   }
   simple_mtx_unlock(&orbis_watch_lock);
}

/* The ring of ranges that have been unmapped, so a mapping that moves into a dead range can name its predecessor.
 * Shares orbis_map_lock with the live table - they are two halves of one question. */
#define ORBIS_DEAD_MAPS 256

static struct orbis_dead_map {
   uint64_t addr;
   uint64_t end;
   uint32_t handle;
} orbis_dead_map[ORBIS_DEAD_MAPS];
static unsigned orbis_dead_at;

static uint64_t orbis_submit_seq_no;

static uint64_t orbis_arena_private;
/* The arena's physical offset and the address the whole of it was first mapped at. Together they turn any address in
 * the window into the physical page behind it - phys = orbis_arena_phys + (va - orbis_arena_map_base) - which is what
 * lets a range be unmapped and mapped again at exactly the same address. */
static uint64_t orbis_arena_phys;
static uint64_t orbis_arena_map_base;
/* Whether RADV's window was handed back after setup. False means the old flat arena, and every stale access stays
 * silent - which is what the whole of 2026-08-11 was spent discovering. */
static bool orbis_arena_unmapped;

/* ⚠ A RANGE MAY NOT LOSE ITS PAGES WHILE THE GPU IS STILL WRITING TO IT, AND THAT IS WHAT amdgpu GUARANTEES FOR US
 * ON LINUX.
 *
 * The fault that proved it, the first WRITE fault of this whole hunt and the reason the arena now protects itself:
 *
 *     BO 42 maps   0x225a00000..0x225e00000   4 MiB, heap 0x2      (line 3056 of the log)
 *     BO 42 UNMAPS 0x225a00000..0x225e00000                        (line 3080, 24 lines later)
 *     # GPU Protection fault. client: TC0, access: Write, addr(VA): 0x225c88000
 *
 * The GPU wrote to BO 42 after BO 42 was destroyed. With the old flat arena that write silently landed in whatever
 * took the address next - which is exactly how bounding boxes ended up inside a descriptor pool, because BO 42's
 * contents ARE bounding boxes.
 *
 * ON amdgpu THIS CANNOT HAPPEN: a submission takes a kernel reference on every BO in its BO list, so userspace may
 * destroy a buffer and the memory stays alive until the fence retires. This arm ignores the BO list entirely - the
 * trace says so on every submission, "BO_HANDLES ... ignored, everything mapped is resident" - so a destroy took
 * effect immediately, mid-flight.
 *
 * So the reference is kept the only way this arm can keep one: a range whose access is being taken away is parked
 * with the sequence number of the last submission handed out, and it only loses access once the fence label has
 * passed that number. Same guarantee, same lifetime, one array and no kernel objects.
 *
 * BOUNDED: if the queue fills, the oldest entry is retired early and SAYS SO - because a silent early retire is the
 * exact bug this exists to prevent. */
#define ORBIS_RETIRE_MAX 4096

static struct orbis_retire {
   uint64_t addr;
   uint64_t size;
   uint64_t seq; /* the submission this range must outlive */
   /* ⚠ AND THE GARLIC CHUNK RIDES WITH IT, because it is the same lifetime question. Taking the
    * rights away was deferred to here from the start; swapping the pages underneath was not, and
    * that is the larger of the two. orbis_garlic_restore maps the arena's own pages back over the
    * range with MAP_FIXED and then releases the GARLIC backing to the system - so a GPU still
    * executing the frame that referenced this BO was reading memory that had already been handed
    * back for the next tenant. vkFreeMemory is allowed to be called while that frame is in flight;
    * this file cites the contract itself where it explains bo_wait_for_idle.
    *
    * Zero garlic_bytes means the entry carries no chunk, which is every entry for a GTT BO. */
   uint64_t garlic_addr;
   uint64_t garlic_bytes;
   int64_t  garlic_phys;
} orbis_retire_q[ORBIS_RETIRE_MAX];
static unsigned orbis_retire_head, orbis_retire_count;
/* ⚠ ONE LOCK FOR THE RING, AND IT USED TO HAVE THREE - WHICH IS THE SAME AS NONE.
 *
 * The push ran under nothing on the default path (it took orbis_kernel_mem_lock only when
 * ORBIS_SERIALISE_KERNEL_MEM was set, and that is off by default), the drain ran under
 * orbis_submit_lock because that is what the submit path happened to hold, and the cancel ran under
 * orbis_map_lock because that is what the map path happened to hold. Three disjoint locks around
 * one ring buffer: two threads in vkFreeMemory could compute the same slot from the same
 * orbis_retire_count and both write it, or both increment it, and the loser's range is either
 * protected twice or never parked at all. Either way orbis_protect_now can take the rights off a
 * range that is live, or leave a freed range readable - and this queue exists to make exactly those
 * two things impossible.
 *
 * ORDER, so this does not become the next deadlock: orbis_submit_lock -> this -> orbis_va_lock.
 * Nothing takes this lock and then reaches for a submit, map or kernel-mem lock. The kernel calls
 * made under it (mprotect, the GARLIC restore) are the ones this file already accepted the cost of;
 * they are serialised against other retire work only, not against mapping. */
static simple_mtx_t orbis_retire_lock = SIMPLE_MTX_INITIALIZER;
/* The label the GPU writes and the CPU polls, and the scratch the mandatory first EOP targets. Separate
 * cache lines, because they are written by the same packet pair microseconds apart.
 *
 * On the host they point at plain memory and the "submit" writes the label itself - so ac_drm_cs_query_fence_
 * status is the SAME code on both arms, and only who moves the label differs. That keeps the laptop loop
 * exercising RADV's real waiting behaviour instead of a shortcut. */
/* ⚠ THE DUMP FOLLOWS IT_INDIRECT_BUFFER, BECAUSE THE FLAT ENTRY CONTAINS ALMOST NO WORK.
 *
 * Measured on the console: a Tempest clear submits 112 dwords, and two of its packets are IT_INDIRECT_BUFFER
 * (0x3F) - one to RADV's per-queue preamble, one to the command buffer itself. RADV grows and links its command
 * streams that way, so the interesting bytes are never in the buffer this arm builds; dumping only the entry
 * shows the envelope and calls it the letter.
 *
 * ONE LEVEL DEEP, and the reason is the hardware's: an IB2 cannot start another IB, so a target's own
 * INDIRECT_BUFFER packets are reported but not followed - a dump that recursed would be describing something
 * the CP will not do.
 *
 * The CHAIN bit is called out by name (bit 20 of the size dword) because it changes what "after this packet"
 * means: a chained IB transfers control and does not return, so anything appended after it - this arm's own
 * end-of-pipe fence, for instance - would never execute. */
/* Shared by both arms: the host build uses the same capacity so a submission that would not fit the console's
 * staging slot does not fit the laptop's dry run either - the refusal is reproducible off-console. */
/* ⚠ 2 MiB, AND 64 KiB WAS A PROBE'S NUMBER MEASURED AGAINST A PROBE'S WORKLOAD.
 *
 * The staging slot holds one submission's whole flattened stream. 64 KiB was chosen when the only client was
 * ps4/radv - a triangle and a dispatch - and it fit everything for weeks. Then the flattener started following
 * RADV's chains correctly and a Gothic frame said what it actually needs:
 *
 *     orbis-drm: submission does not fit the 16384 dword staging buffer (chunk 1 of 3)
 *
 * ~2040 indirect draws with their state is hundreds of kilobytes, and for as long as this buffer was too small the
 * refusal was the whole picture: the world never reached the GPU.
 *
 * 2 MiB x ORBIS_FENCE_CB_SLOTS comes out of the arena's private slice - 32 MiB of a 1 GiB arena - which is a cost
 * worth naming and not worth arguing about. The refusal stays, and it now prints how much was needed, so the next
 * time this bounds a real workload the number comes from the console rather than from an estimate. */
#define ORBIS_FENCE_CB_BYTES (2 * 1024 * 1024)

/* ⚠ FLATTENING, AND IT REPLACES A ONE-LEVEL PATCH THAT COULD ONLY EVER BE A DOWN PAYMENT.
 *
 * RADV grows a command stream as a LINKED LIST: the chunk it hands us ends in an IT_INDIRECT_BUFFER with CHAIN,
 * whose target can end in another one. CHAIN means control TRANSFERS - it does not return - so:
 *
 *   - this arm's end-of-pipe fence, appended after the concatenated chunks, is unreachable. That was measured:
 *     with the bit set, Tempest's submissions after the first were accepted and never executed.
 *   - clearing the bit turns the jump into a call and fixes the FIRST level only. The chain inside the target is
 *     untouched, and following it as a call would put the CP at IB3 depth, which gfx7 does not have. A command
 *     buffer big enough to chain twice - a game's - would then break in a new way.
 *
 * So the whole list is COPIED INTO THE STAGING BUFFER instead: the chain packet is dropped, the target's packets
 * take its place, recursively. That is the same thing this arm already does to RADV's chunk LIST, applied to the
 * chain list, and it leaves one flat IB with no chains and no depth question.
 *
 * PACKETS RATHER THAN BYTES, because a chain must be recognised: the walk needs type-3 headers and counts. If it
 * meets a dword that is not a type-3 header it copies the remainder verbatim and stops walking - a stream this
 * code cannot parse is still a stream the CP can run, and refusing it would be worse than passing it through.
 *
 * THE REST OF AN IB AFTER ITS CHAIN IS DROPPED, and that is the chain's own semantic rather than an optimisation:
 * control never comes back, so those packets were already dead. RADV puts the chain last anyway.
 */
/* ⚠ 4096, AND 16 WAS A PROBE'S NUMBER THAT TRUNCATED A REAL FRAME.
 *
 * This guard exists to stop a CYCLE - a chain that points back into itself would recurse forever - and it was set to
 * a depth no probe could ever reach. Then a Gothic frame arrived: 155046 dwords, and RADV had linked it out of
 * SEVENTEEN OR MORE command buffers. The log said so once, in the frame that then took a GPU page fault:
 *
 *     orbis-drm: chain deeper than 16 - passing the packet through unflattened
 *     # GPU Protection fault. vmid: game process, client: TC1, access: Read
 *     # exception: 0xa0d0c005 (GPU_FAULT_PAGE_FAULT_ASYNC)
 *
 * A cycle is caught just as well at 4096, and no legitimate recording is 4096 IBs deep - the staging buffer's own
 * capacity bounds it long before that. */
#define ORBIS_CHAIN_MAX_DEPTH 4096

/* ⚠ MOVED OUT OF THE __PS4__ SEAM, because building a PM4 packet is not a console operation. It sat in the
 * backing-store arm until the flattener - which both arms run - started emitting one for the retired-side
 * progress marker, and the host build failed to link. Same rule as the diagnostics block earlier: code that
 * is platform-independent does not live inside the platform seam, however it grew there. */
static uint32_t *
orbis_emit_eop(uint32_t *dw, uint64_t va, uint32_t value, uint32_t sel)
{
   *dw++ = ORBIS_PM4_TYPE3(ORBIS_IT_EVENT_WRITE_EOP, 5);
   *dw++ = ORBIS_EV_CACHE_FLUSH_AND_INV_TS | (ORBIS_EI_EOP << 8) | ORBIS_EOP_TCL1_ACTION_EN |
           ORBIS_EOP_TC_ACTION_EN;
   *dw++ = (uint32_t)va & 0xFFFFFFFCu;
   *dw++ = ((uint32_t)(va >> 32) & 0xFFFFu) | sel;
   *dw++ = value;
   *dw++ = 0;
   return dw;
}


static uint32_t *
orbis_copy_ib(uint32_t *dst, const uint32_t *dst_end, const uint32_t *src, uint32_t n, unsigned depth,
              unsigned *chains, const uint32_t *out_base, uint32_t marks, uint32_t *next_mark,
              bool eop)
{
   uint32_t i = 0;
   while (i < n) {
      const uint32_t hdr = src[i];

      if (hdr == 0) {
         /* Zeros are what a padding NOP's body is, and copying them into the stream would hand the CP a run of
          * type-0 writes to register 0. The recording ends here. */
         return dst;
      }

      if ((hdr & 0xC0000000u) != 0xC0000000u) {
         /* Not a type-3 header: stop parsing and pass the tail through untouched. */
         const uint32_t rest = n - i;
         if (dst + rest > dst_end)
            return NULL;
         memcpy(dst, src + i, (size_t)rest * 4);
         return dst + rest;
      }

      const uint32_t op = (hdr >> 8) & 0xFF;
      const uint32_t cnt = ((hdr >> 16) & 0x3FFF) + 1;
      const uint32_t words = 1 + cnt;

      /* ⚠ A NOP'S COUNT IS PADDING, NOT A PACKET, AND TRUSTING IT BROKE THE WHOLE POINT OF THIS FUNCTION.
       *
       * RADV reserves space at the end of a command stream by emitting PKT3(NOP, 0x3fff) - header 0xFFFF1000 - and
       * overwrites those dwords with a chain packet when it links the next CS object. A leftover one therefore
       * claims 16385 dwords inside a buffer of a few hundred, and this walk took it at its word: `i + words > n`
       * sent it down the "packet claims more than the IB holds" path, which copies the remainder VERBATIM and
       * returns.
       *
       * MEASURED, in submission #6023's own flattened stream:
       *
       *     0e00: ... 0000000a ffff1000 c0023f00 232c4000 00000002 00900120
       *                        ^NOP     ^IT_INDIRECT_BUFFER, size dword has bit 20 -> CHAIN=1
       *
       * **A CHAIN PACKET SURVIVED THE FLATTENER**, which is the exact thing this function exists to prevent: a
       * chain transfers control and does not return, so the end-of-pipe fence this port appends after it is
       * unreachable. That defect cost an evening once already and was fixed by writing this walk; a NOP with a
       * nonsense count walked straight around it.
       *
       * So NOP is handled FIRST and its count is clamped to what the buffer holds, and the packet is DROPPED
       * rather than copied - it is padding, and the flattened stream is built fresh with its own padding at the
       * end. Then the walk carries on and sees the chain, which is the whole point. */
      /* ⚠ ONLY THE OVERRUNNING ONE. The first version of this dropped EVERY NOP, and the self-test failed again -
       * "0 dwords out" - because its own markers ARE NOPs carrying magic payloads. Twice now this test has caught
       * this fix being wrong before the console did, which is the entire argument for having it.
       *
       * A NOP whose count fits is a real packet the CP skips: copy it, and the stream stays what RADV built. A NOP
       * whose count runs past the buffer is the padding RADV reserves and overwrites - and copying THAT into the
       * stream is a skip off the end of our own staging slot, into whatever follows it. That one is dropped, and
       * nothing follows it in an IB anyway, which is why doing so costs no reachable packet. */
      if (op == 0x10 /* IT_NOP */ && i + words > n) {
         /* ⚠ SKIP THE HEADER ONLY. NOT the rest of the IB, and getting that wrong hid the whole world.
          *
          * RADV pads an IB's tail with PKT3(NOP, 0x3fff) and, when the stream grows, OVERWRITES THE LAST FOUR DWORDS
          * OF THAT PADDING with the chain packet to the next IB. So a leftover padding NOP is followed, in the same
          * buffer, by the chain that carries the REST OF THE RECORDING.
          *
          * The first version of this fix set `i = n` - "nothing after an overrunning NOP is reachable" - which threw
          * that chain away. The console said so in the clearest possible terms: no crash any more, and no world
          * either. OpenGothic issued ~2040 indirect draws per frame, RADV emitted every one of them, and the stream
          * this port submitted was 664 dwords.
          *
          * ⚠ AND THE SELF-TEST HAD ALREADY CAUGHT THIS. Its first version put a padding NOP immediately before
          * IB_B's chain and failed with "both chains were not followed". I decided the TEST was describing a shape
          * RADV does not emit and moved the NOP to the end of the last IB. The test was right and I edited it into
          * agreeing with me. That case is back, and it is the one that matters.
          *
          * So: drop the padding header, keep walking. If a chain follows, it is found and flattened; if only zeros
          * follow, the loop below ends on them. */
         ++i;
         continue;
      }

      if (i + words > n) {
         /* A packet that claims more than the IB holds. Copy what is there and stop; the alternative is reading
          * past the buffer to describe it. */
         const uint32_t rest = n - i;
         if (dst + rest > dst_end)
            return NULL;
         memcpy(dst, src + i, (size_t)rest * 4);
         return dst + rest;
      }

      if (op == 0x3F /* IT_INDIRECT_BUFFER */ && (src[i + 3] & (1u << 20)) != 0) {
         const uint64_t addr = (uint64_t)src[i + 1] | ((uint64_t)(src[i + 2] & 0xFFFFu) << 32);
         const uint32_t size_dw = src[i + 3] & 0xFFFFFu;

         /* ⚠ ORBIS_SELFTEST_BREAK EXISTS SO THE SELF-TEST CAN BE SEEN TO FAIL. A check that cannot fail is
          * decoration, and this file has already shipped one of those tonight (a strip that logged only when it
          * had done something, so "did nothing" and "never ran" were indistinguishable). With this set, chains
          * are left alone and the self-test must report FAILED - if it still says OK, the test is broken and not
          * the driver. */
         if (getenv("ORBIS_SELFTEST_BREAK") != NULL) {
            /* fall through to copying the packet verbatim, chain and all */
         } else if (depth >= ORBIS_CHAIN_MAX_DEPTH) {
            /* ⚠ AND IT REFUSES NOW RATHER THAN PASSING THE PACKET THROUGH. Emitting an unflattened chain is the one
             * thing this function exists to prevent: control transfers away, the fence appended after it is never
             * reached, and the CP carries on through whatever is at the target. That is not a degraded submission,
             * it is an undefined one - and it cost a GPU page fault to learn the difference. A refused submit is a
             * loud -ENOMEM in the log with the driver still alive. */
            mesa_loge("orbis-drm: chain deeper than %d - REFUSING the submission rather than emitting a chain "
                      "packet the fence cannot survive",
                      ORBIS_CHAIN_MAX_DEPTH);
            return NULL;
         } else if (size_dw == 0 || addr < orbis_va_base || addr + (uint64_t)size_dw * 4 > orbis_va_end) {
            /* The address came out of the stream. Following it blindly would turn this into the crash it exists
             * to prevent, so it is reported and passed through as-is. */
            mesa_logw("orbis-drm: chain target 0x%" PRIx64 " (%u dw) is outside the arena - not flattened", addr,
                      size_dw);
         } else {
            ++*chains;
            /* The chain replaces everything after it in this IB, so this is a tail call rather than a splice. */
            return orbis_copy_ib(dst, dst_end, (const uint32_t *)(uintptr_t)addr, size_dw, depth + 1, chains,
                              out_base, marks, next_mark, eop);
         }
      }

      if (dst + words > dst_end)
         return NULL;

      /* ⚠ THE MARKER GOES AT A PACKET BOUNDARY, WHICH IS WHY IT LIVES HERE. This walk is the only place that
       * knows where one packet ends and the next begins; splicing anywhere else would hand the CP a header
       * mid-packet. Emitted BEFORE the packet it precedes, so the value that lands is the offset of work the
       * CP has finished rather than started. */
      if (marks != 0 && out_base != NULL && orbis_progress_word != NULL &&
          (uint32_t)(dst - out_base) >= *next_mark && dst + 5 + words <= dst_end) {
         const uint64_t va = (uint64_t)(uintptr_t)orbis_progress_word;
         *dst++ = ORBIS_PM4_TYPE3(ORBIS_IT_WRITE_DATA, 4);
         /* DST_SEL=MEMORY(5) at bit 8, WR_CONFIRM at bit 20, ENGINE_SEL=ME(0). Values from Mesa's own
          * generated packet header (amd_cp_packets: S_371_DST_SEL, S_371_WR_CONFIRM, V_371_MEMORY), spelled
          * out because this file deliberately includes no sid.h. */
         *dst++ = (5u << 8) | (1u << 20);
         *dst++ = (uint32_t)va;
         *dst++ = (uint32_t)(va >> 32);
         /* ⚠ THE OFFSET IS THE ONE PAST THIS PACKET, and writing it as `*dst++ = (uint32_t)(dst -
          * out_base)` made that a coin toss: reading dst and modifying it in the same expression
          * with no sequence point between them is undefined, and the build said so
          * (-Wunsequenced). Whichever the compiler picked, the marker was off by a dword half the
          * time it was recompiled. Spelled out: the value is where the CP resumes after this
          * WRITE_DATA, which is what the comment above claims it is. */
         *dst = (uint32_t)((dst + 1) - out_base);
         ++dst;

         /* The retired-side twin, when it is armed. Same offset, written by the end of the pipe instead of
          * by the front, so the pair brackets whatever never finished. */
         if (eop && dst + 12 + words <= dst_end) {
            /* ⚠ TWO EVENTS, AS THE FENCE PATH DOES, AND THE FIRST VERSION EMITTED ONE.
             *
             * This file's own research settles it: "TWO EOP EVENTS ARE MANDATORY ON GFX7 (...) The first
             * drains the engines so the second one's write is trustworthy" - established from two oracles
             * for the fence, and then not applied here. Worse, the single event carried INT_SEL 0, which is
             * exactly the DUMMY selector: the marker was the drain, and its write is the one the port
             * already knows not to trust.
             *
             * The localisation never rested on it - that came from the WRITE_DATA marker, which is an
             * ordinary CP write - but "execution retired through B" did, and B was being read off the wrong
             * half of a pair that was not there. The drain writes to the scratch word the fence uses for the
             * same purpose, and the second writes the offset. */
            const uint64_t rva = (uint64_t)(uintptr_t)orbis_retired_word;
            const uint32_t off = (uint32_t)(dst - out_base);
            dst = orbis_emit_eop(dst, (uint64_t)(uintptr_t)orbis_drain_word, off,
                                 ORBIS_EOP_DST_SEL_MC | (0u << ORBIS_EOP_INT_SEL_SHIFT) |
                                    ORBIS_EOP_DATA_SEL_32BIT);
            dst = orbis_emit_eop(dst, rva, off,
                                 ORBIS_EOP_DST_SEL_MC |
                                    ((uint32_t)ORBIS_EOP_INT_SEL << ORBIS_EOP_INT_SEL_SHIFT) |
                                    ORBIS_EOP_DATA_SEL_32BIT);
         }
         {
            /* Inside the fine window the interval is eight dwords, which is smaller than every packet this
             * stream contains - so each one gets its own marker and the last value names the packet. */
            const uint32_t here = (uint32_t)(dst - out_base);
            const uint32_t fine_lo = orbis_progress_from(), fine_hi = orbis_progress_to();
            const bool fine = fine_hi > fine_lo && here >= fine_lo && here < fine_hi;
            *next_mark = here + (fine ? 8u : marks);
         }
      }

      memcpy(dst, src + i, (size_t)words * 4);
      dst += words;
      i += words;
   }
   return dst;
}

/* Every chunk of one submission, flattened into one buffer. Shared by both arms deliberately: the construction is
 * platform-independent, so the laptop can verify it and the console is left for questions about execution. */
static uint32_t *
orbis_build_flat(uint32_t *cb, uint32_t cb_dwords, void **dcb, uint32_t *dcb_bytes, unsigned n_dcb,
                 uint64_t seq, uint32_t reserve_dw)
{
   const uint32_t *const dst_end = cb + cb_dwords - reserve_dw;
   uint32_t             *dw = cb;
   unsigned              chains = 0;
   const uint32_t        marks = orbis_progress_marks();
   const bool            eop = marks != 0 && orbis_progress_eop() && orbis_retired_word != NULL &&
                                    orbis_drain_word != NULL;
   uint32_t              next_mark = marks;

   for (unsigned i = 0; i < n_dcb; i++) {
      const uint32_t *const before = dw;
      dw = orbis_copy_ib(dw, dst_end, (const uint32_t *)dcb[i], dcb_bytes[i] / 4, 0, &chains, cb, marks,
                         &next_mark, eop);
      if (dw == NULL) {
         /* The produced size is what sizes this buffer, so it is printed: "does not fit" without a number is a
          * refusal nobody can act on. `before - cb` is what fitted; the chunk's own size is a floor on the rest. */
         mesa_loge("orbis-drm: submission does not fit the %u dword staging buffer - chunk %u of %u, %u dwords "
                   "already written, this chunk alone is %u dwords before its chains",
                   cb_dwords, i, n_dcb, (unsigned)(before - cb), dcb_bytes[i] / 4);
         return NULL;
      }
   }

   if (orbis_trace())
      mesa_logi("orbis-drm: submit #%" PRIu64 ": flattened %u chunk(s) and %u chain(s) into %u dwords", seq,
                n_dcb, chains, (unsigned)(dw - cb));
   return dw;
}

/* ⚠ THE FLATTENER HAS A SELF-TEST, AND IT RUNS ON BOTH ARMS AT EVERY BOOT.
 *
 * The code above is the fix for a defect that took an evening of console flashes to find, and its input - a
 * chain of IBs - is something RADV only produces in particular circumstances: forcing 2000 draws through
 * infoprobe on the laptop still produced ONE inline chunk and no chain, because radv_amdgpu_cs_chain links CS
 * OBJECTS rather than growing with size. So waiting for a real chain to test the flattener means the first real
 * exercise is a game, on hardware, with a five-second timeout as the error message.
 *
 * Instead the input is SYNTHESISED: three IBs in the arena, joined by two chains, with marker payloads and dead
 * packets after each chain. Then the flattener's own output is checked - the markers present in order, nothing
 * from after a chain, and not one IT_INDIRECT_BUFFER with CHAIN left. That is a real test of the real function,
 * including its arena bounds check, and it costs ~200 dwords once per boot.
 *
 * IT RUNS BEFORE RADV HAS ALLOCATED ANYTHING, at the end of arena setup, so writing at orbis_va_base cannot
 * disturb a live allocation. */
static void
orbis_selftest_flatten(void)
{
   /* ⚠ ITS OWN BUFFER, AND IT LENDS THE ARM'S VA WINDOW TO IT FOR THE DURATION.
    *
    * The flattener refuses to follow a chain whose target is outside [orbis_va_base, orbis_va_end) - the address
    * comes out of the stream, and dereferencing a wrong one would turn the diagnostic into the crash it prevents.
    * That check is part of what this test is testing, so the test cannot simply bypass it. On the console the
    * window is the arena and these addresses would be real; on the host arm the window is a placeholder and
    * nothing is mapped there, so writing at orbis_va_base would fault.
    *
    * So the test uses a static buffer and points the window at it, restoring both values afterwards. That makes
    * the test identical on both arms and independent of whether the arena exists yet. */
   static uint32_t region[3 * 1024];
   const uint64_t  saved_base = orbis_va_base, saved_end = orbis_va_end;
   orbis_va_base = (uint64_t)(uintptr_t)region;
   orbis_va_end = orbis_va_base + sizeof(region);

   /* Three IBs, 4 KiB apart. Distinct markers so ORDER is checked and not just the count. */
   uint32_t *const ib_a = region;
   uint32_t *const ib_b = region + 1024;
   uint32_t *const ib_c = region + 2048;
   uint32_t       *p;

   /* IB_C: two NOPs, no chain. */
   p = ib_c;
   *p++ = ORBIS_PM4_TYPE3(0x10, 2); *p++ = 0xC0DE0001u; *p++ = 0xC0DE0002u;
   *p++ = ORBIS_PM4_TYPE3(0x10, 2); *p++ = 0xC0DE0003u; *p++ = 0xC0DE0004u;
   /* ⚠ A PADDING NOP AT THE END, WHICH IS WHERE RADV LEAVES ONE. It emits PKT3(NOP, 0x3fff) - header 0xFFFF1000 -
    * to reserve the tail of a command stream and overwrites those dwords when it chains the next CS object. A
    * leftover claims 16385 dwords inside a buffer of a few hundred, and this port used to copy it into the stream
    * it hands Sony's CP: a skip that runs off the end of our own staging slot and into whatever follows.
    *
    * FIRST TRY AT TESTING THIS PUT THE NOP BEFORE IB_B's CHAIN, and this self-test failed with "both chains were
    * not followed" - correctly, because clamping such a NOP to the end of its IB discards the packets after it,
    * chain included. That shape came from misreading a CONCATENATED dump: the `ffff1000` seen on the console was
    * the last dword of one chunk and the chain was the first packet of the NEXT one, and our flattener walks each
    * chunk separately. The test now describes what RADV actually emits.
    *
    * The output must contain no such NOP: it is padding on the way in and the flattened stream builds its own. */
   *p++ = ORBIS_PM4_TYPE3(0x10, 0x3FFF);
   const uint32_t n_c = (uint32_t)(p - ib_c);

   /* IB_B: one NOP, then a PADDING NOP WHOSE COUNT OVERRUNS THE BUFFER, then a chain to IB_C, then a packet that
    * must NOT survive.
    *
    * ⚠ THE PADDING NOP IS HERE BECAUSE ITS ABSENCE LET A REAL DEFECT THROUGH. RADV reserves the tail of a command
    * stream with PKT3(NOP, 0x3fff) and overwrites it when it chains; a leftover one claims 16385 dwords inside a
    * few hundred, and the walk used to take that at face value, copy the remainder verbatim and return - leaving a
    * CHAIN packet in the flattened stream, which is precisely what this function exists to remove. It happened on
    * the console in OpenGothic's submission #6023 and this self-test said OK throughout.
    *
    * A test that passes while the thing it tests is broken is worse than no test. Now the input contains the shape
    * that broke it. */
   p = ib_b;
   *p++ = ORBIS_PM4_TYPE3(0x10, 2); *p++ = 0xB00B0001u; *p++ = 0xB00B0002u;
   /* ⚠ THE PADDING NOP GOES BACK WHERE IT BROKE THINGS: immediately before the chain, which is exactly how RADV
    * leaves it when it overwrites the tail of an IB to link the next one. This test failed on this shape once and I
    * moved the shape instead of fixing the code. */
   *p++ = ORBIS_PM4_TYPE3(0x10, 0x3FFF);
   *p++ = ORBIS_PM4_TYPE3(0x3F, 3);
   *p++ = (uint32_t)(uintptr_t)ib_c;
   *p++ = (uint32_t)(((uint64_t)(uintptr_t)ib_c) >> 32);
   *p++ = (1u << 20) | (1u << 23) | n_c; /* CHAIN | VALID | size */
   *p++ = ORBIS_PM4_TYPE3(0x10, 2); *p++ = 0xDEAD0001u; *p++ = 0xDEAD0002u;
   const uint32_t n_b = (uint32_t)(p - ib_b);

   /* IB_A: one NOP, a chain to IB_B, then another packet that must NOT survive. */
   p = ib_a;
   *p++ = ORBIS_PM4_TYPE3(0x10, 2); *p++ = 0xA5A50001u; *p++ = 0xA5A50002u;
   *p++ = ORBIS_PM4_TYPE3(0x3F, 3);
   *p++ = (uint32_t)(uintptr_t)ib_b;
   *p++ = (uint32_t)(((uint64_t)(uintptr_t)ib_b) >> 32);
   *p++ = (1u << 20) | (1u << 23) | n_b;
   *p++ = ORBIS_PM4_TYPE3(0x10, 2); *p++ = 0xDEAD0003u; *p++ = 0xDEAD0004u;
   const uint32_t n_a = (uint32_t)(p - ib_a);

   uint32_t  out[64];
   void     *chunks[1] = {ib_a};
   uint32_t  sizes[1] = {n_a * 4};
   unsigned  chains = 0;
   uint32_t next_mark = 0;
   uint32_t *end = orbis_copy_ib(out, out + ARRAY_SIZE(out), ib_a, n_a, 0, &chains, NULL, 0, &next_mark,
                                 false);

   const char *fail = NULL;
   if (end == NULL)
      fail = "the flattener refused a stream that fits";
   else if (chains != 2)
      fail = "both chains were not followed";
   else if ((uint32_t)(end - out) != 3 + 3 + (n_c - 1))
      fail = "the flattened size is not the sum of the reachable packets minus the padding NOP";
   else if (out[1] != 0xA5A50001u || out[4] != 0xB00B0001u || out[7] != 0xC0DE0001u || out[10] != 0xC0DE0003u)
      fail = "the markers are missing or out of order";

   if (fail == NULL)
      for (uint32_t i = 0; i < (uint32_t)(end - out); i++) {
         if ((out[i] & 0xFFFF0000u) == 0xDEAD0000u)
            fail = "a packet from after a chain survived";
         if ((out[i] & 0xC0000000u) == 0xC0000000u && ((out[i] >> 8) & 0xFF) == 0x3F)
            fail = "an IT_INDIRECT_BUFFER survived flattening";
         /* A NOP whose count runs past what any buffer here holds. Copying one into the stream is a skip off the
          * end of our staging slot, which is the defect this whole case exists for. */
         if (out[i] == ORBIS_PM4_TYPE3(0x10, 0x3FFF))
            fail = "a padding NOP claiming 16385 dwords survived into the flattened stream";
      }

   (void)chunks;
   (void)sizes;
   if (fail != NULL)
      mesa_loge("orbis-drm: FLATTENER SELF-TEST FAILED - %s (chains %u, %u dwords out)", fail, chains,
                end != NULL ? (unsigned)(end - out) : 0u);
   else
      mesa_logi("orbis-drm: flattener self-test: 2 chains followed, %u dwords, no chain packet left - OK",
                (unsigned)(end - out));

   orbis_va_base = saved_base;
   orbis_va_end = saved_end;
}


/* ⚠ WHAT SHAPE IS THE DRIVER DRAWING IN? Thirty hypotheses have been eliminated and not one of them explained
 * the ONE property that constrains this hardest: the artefacts are EXACTLY a quarter of the screen.
 *
 * Nothing in GFX7's raster hardware works at quarter-screen granularity. PA_SC_RASTER_CONFIG interleaves shader
 * engines every 8 to 64 pixels; a render backend owns tiles, not quadrants; a CMASK block covers 128x128. So a
 * 960x540 region is not something the hardware partitions into - which leaves the possibility that SOFTWARE asked
 * for it, and nobody has looked.
 *
 * This walks the command stream that was submitted anyway and prints every distinct scissor and screen rectangle
 * in it, decoded. Pure observation: it parses a buffer the driver already built and changes nothing.
 *
 *   if a 960x540 rectangle appears        the shape stops being a mystery, and the question becomes which pass
 *                                         owns that rectangle and why it sometimes writes the wrong thing
 *   if no such rectangle appears          then nothing in the frame is quadrant-shaped, and the shape has to come
 *                                         from addressing rather than from rasterisation - which points at
 *                                         surface layout, not at state
 *
 * ⚠ EITHER ANSWER IS WORTH MORE THAN ANOTHER REGISTER DIFF, because the shape is the property that survived every
 * elimination so far.
 *
 * Field layout from gfx7.json: TL_X/BR_X are bits [0,14], TL_Y/BR_Y bits [16,30], WINDOW_OFFSET_DISABLE bit 31.
 * ORBIS_SCAN_SCISSORS=1. */
static void
orbis_scan_scissors(uint64_t seq, const uint32_t *w, uint32_t n)
{
   if (getenv("ORBIS_SCAN_SCISSORS") == NULL)
      return;

   struct rect { uint32_t reg, tl, br; };
   struct rect seen[64];
   unsigned    n_seen = 0;

   /* Every register pair whose value is a screen rectangle. The viewport scissors are a run of sixteen, eight
    * bytes apart, and all of them are worth having: a pass that renders to one quadrant would say so here. */
   for (uint32_t i = 0; i + 1 < n;) {
      const uint32_t hdr = w[i];
      if ((hdr >> 30) != 3) { ++i; continue; }              /* not a type-3 packet */
      const uint32_t count = ((hdr >> 16) & 0x3FFF) + 1;    /* body dwords */
      const uint32_t op = (hdr >> 8) & 0xFF;
      if (op == 0x69 /* IT_SET_CONTEXT_REG */ && count >= 2) {
         const uint32_t base = 0x28000u + (w[i + 1] << 2);
         for (uint32_t k = 0; k + 1 < count - 1; k += 2) {
            const uint32_t reg = base + k * 4;
            /* ⚠ THE VIEWPORT AND THE COLOUR MASK, BECAUSE SCISSORS ALONE MISSED IT.
             *
             * The strip in the corner is not any scissor in the frame: the distinct rectangles are the
             * full 1920x1080, a mip chain of powers of two, and nothing wide-and-short. But a VIEWPORT of
             * the strip's shape would produce the same picture and is invisible to a scissor scan - a pass
             * rendering into a 1920x1080 target through a small viewport puts its output in the corner,
             * depth-tested, so it appears only where geometry stands. Which is exactly what is seen.
             *
             * And the pixels say more: sampled at frame 900, the strip's rows have R and B EXACTLY zero
             * with only green non-zero, while a control row from mid-screen carries all three. That is a
             * write mask, not a brightness. So CB_TARGET_MASK is worth the same line of code.
             *
             * PA_CL_VPORT_XSCALE is 0x282D0 and the six words per viewport repeat every 24 bytes; the
             * rectangle is centre +/- scale. CB_TARGET_MASK is 0x28238, four bits per render target. */
            if (reg >= 0x282D0u && reg < 0x28330u && ((reg - 0x282D0u) % 4) == 0) {
               /* Spelled out rather than pulled from a header: this file includes no Mesa float
                * helpers, and a union is the portable spelling of "these bits are a float". */
               union { uint32_t u; float f; } bits = {.u = w[i + 2 + k]};
               const float f = bits.f;
               static const char *const what[6] = {"XSCALE", "XOFFSET", "YSCALE", "YOFFSET", "ZSCALE",
                                                   "ZOFFSET"};
               mesa_logi("orbis-drm: VIEWPORT %u %-7s = %f", (reg - 0x282D0u) / 24u,
                         what[((reg - 0x282D0u) / 4u) % 6u], f);
            }
            if (reg == 0x28238u)
               mesa_logi("orbis-drm: CB_TARGET_MASK = 0x%08x (four bits per target; 0xf is RGBA, 0x2 is "
                         "GREEN ONLY - which is what the strip's pixels look like)",
                         w[i + 2 + k]);

            const bool is_rect = (reg == 0x28030u) ||                       /* SCREEN_SCISSOR_TL   */
                                 (reg == 0x28204u) ||                       /* WINDOW_SCISSOR_TL   */
                                 (reg == 0x28240u) ||                       /* GENERIC_SCISSOR_TL  */
                                 (reg >= 0x28250u && reg < 0x282D0u &&
                                  ((reg - 0x28250u) % 8) == 0);             /* VPORT_SCISSOR_n_TL  */
            if (!is_rect)
               continue;
            const uint32_t tl = w[i + 2 + k], br = w[i + 3 + k];
            bool dup = false;
            for (unsigned s = 0; s < n_seen; ++s)
               if (seen[s].reg == reg && seen[s].tl == tl && seen[s].br == br) { dup = true; break; }
            if (!dup && n_seen < ARRAY_SIZE(seen)) {
               seen[n_seen].reg = reg; seen[n_seen].tl = tl; seen[n_seen].br = br;
               ++n_seen;
            }
         }
      }
      i += 1 + count;
   }

   if (n_seen == 0)
      return;

   mesa_logi("orbis-drm: SCISSORS submit #%" PRIu64 ": %u distinct rectangle(s)", seq, n_seen);
   for (unsigned s = 0; s < n_seen; ++s) {
      const uint32_t tl = seen[s].tl, br = seen[s].br;
      const unsigned x0 = tl & 0x7FFF, y0 = (tl >> 16) & 0x7FFF;
      const unsigned x1 = br & 0x7FFF, y1 = (br >> 16) & 0x7FFF;
      const unsigned wid = x1 > x0 ? x1 - x0 : 0, hei = y1 > y0 ? y1 - y0 : 0;
      /* 960x540 is a quarter of 1080p, and a half in one axis is worth flagging too - the maintainer has seen
       * the artefact cover half the screen as well as a quarter. */
      const char *note = "";
      if (wid == 960 && hei == 540)       note = "   *** EXACTLY A QUARTER OF 1080p ***";
      else if (wid == 1920 && hei == 540) note = "   *** HALF THE SCREEN, split horizontally ***";
      else if (wid == 960 && hei == 1080) note = "   *** HALF THE SCREEN, split vertically ***";
      else if (wid == 1920 && hei == 1080) note = "   (full frame)";
      mesa_logi("orbis-drm: SCISSORS   [0x%05x] (%u,%u)-(%u,%u)  %ux%u%s", seen[s].reg, x0, y0, x1, y1, wid, hei,
                note);
   }
}

static void
orbis_dump_ib(uint64_t seq, const uint32_t *w, uint32_t n, const char *what, bool follow)
{
   mesa_logi("orbis-drm: submit #%" PRIu64 " %s: %u dwords at %p", seq, what, n, (const void *)w);
   for (uint32_t i = 0; i < n; i += 8) {
      char     line[128];
      unsigned at = 0;
      for (uint32_t j = i; j < n && j < i + 8; ++j)
         at += (unsigned)snprintf(line + at, sizeof(line) - at, "%08x ", w[j]);
      mesa_logi("orbis-drm:   %04x: %s", i * 4, line);
   }

   if (!follow)
      return;

   /* A second pass rather than one interleaved with the hexdump, so the entry reads as the contiguous block the
    * CP sees before anything it points at appears underneath it. */
   for (uint32_t i = 0; i + 3 < n;) {
      const uint32_t hdr = w[i];
      /* Type-3 packets only: 0xC0000000 in the top two bits, opcode in [15:8], count-1 in [29:16]. Anything
       * else means the walk has lost the packet boundary, and continuing would print noise as if it were
       * structure - so it stops and says so. */
      if ((hdr & 0xC0000000u) != 0xC0000000u) {
         mesa_logw("orbis-drm:   dump stopped at dword %u: 0x%08x is not a type-3 header", i, hdr);
         return;
      }
      const uint32_t op = (hdr >> 8) & 0xFF;
      const uint32_t cnt = ((hdr >> 16) & 0x3FFF) + 1;

      if (op == 0x3F /* IT_INDIRECT_BUFFER */ && i + 3 < n) {
         const uint64_t addr = (uint64_t)w[i + 1] | ((uint64_t)(w[i + 2] & 0xFFFFu) << 32);
         const uint32_t size_dw = w[i + 3] & 0xFFFFFu;
         const bool     chain = (w[i + 3] & (1u << 20)) != 0;
         mesa_logi("orbis-drm:   -> IB2 at 0x%" PRIx64 ", %u dwords, chain=%d, flags 0x%03x", addr, size_dw,
                   chain ? 1 : 0, (unsigned)(w[i + 3] >> 20));
         /* Only if it is inside the arena: the address comes out of the stream, and dereferencing something the
          * stream got wrong would turn a diagnostic into the crash it was meant to explain.
          *
          * ⚠ AND THE ARENA BOUNDS ALONE DO NOT ESTABLISH THAT. The pages between orbis_va_base and orbis_va_end
          * start mapped-and-inaccessible and are backed only where RADV has put a BO, so "inside the arena" and
          * "readable" are different statements - the SH-pointer dump below learned that by taking the process
          * down. Below orbis_va_base is this arm's own private slice, which is backed by construction; at or
          * above it, ask the live mapping table. */
         const bool readable = addr >= orbis_arena_private && addr + (uint64_t)size_dw * 4 <= orbis_va_end &&
                               (addr < orbis_va_base || orbis_va_is_mapped(addr, (uint64_t)size_dw * 4));
         if (readable && size_dw != 0) {
            char label[32];
            snprintf(label, sizeof(label), "IB2@%08x", (unsigned)addr);
            orbis_dump_ib(seq, (const uint32_t *)(uintptr_t)addr, size_dw > 256 ? 256 : size_dw, label, false);
            if (size_dw > 256)
               mesa_logi("orbis-drm:   (truncated at 256 of %u dwords)", size_dw);
         } else {
            mesa_logw("orbis-drm:   IB2 target is outside the arena (0x%" PRIx64 "..0x%" PRIx64
                      ") or inside it with nothing backing it - either way the CP would fault here, and this "
                      "dump will not follow it",
                      orbis_arena_private, orbis_va_end);
         }
      }

      i += 1 + cnt;
   }
}

/* ⚠ FOLLOW WHAT THE FRAME POINTS AT, BECAUSE THE ADDRESS IS NEITHER IN THE PACKETS NOR IN MEMORY.
 *
 * Run H closed the two obvious doors. Scanning all 580 MiB of mapped memory for the faulting base in its 64-bit
 * spellings - a plain pointer and a V#'s word pair - found it ZERO times, and the vertex-descriptor check at
 * construction never fired. Every dword hit was `0x0ff00000` sitting inside BO 4, a 128 MiB texture heap, framed
 * by things like `f6d40000 14000001` and `0ff0ff00 0fff00ff`: those are BC block colours and index bytes, and 182
 * hits in 145 million dwords is the rate coincidence predicts.
 *
 * So no descriptor RADV BUILT and no descriptor SITTING IN MEMORY carries that address. What remains is that the
 * GPU reads DESCRIPTORS FROM THE WRONG PLACE - texture bytes interpreted as a T#, whose base field would be a
 * number like `0x0ff00000`, and `0x0ff00000 << 8` is `0xff0000000` to the bit. That also explains the drift: a
 * different byte each frame, a base that looks stable because the top bits of BC colour data are, and offsets
 * that move because the low bits do.
 *
 * THIS PASS TESTS IT DIRECTLY. Every SET_SH_REG in the frame is a user-data write, and user data is where a
 * shader gets its descriptor-set pointers. For each value written, this treats it as a 32-bit pointer - the top
 * half being address32_hi, which is the arena's, exactly as the shader reconstructs it - and dumps the sixteen
 * dwords it points at. Then reading is enough: a descriptor set contains recognisable V#s and T#s, and a texture
 * heap contains BC blocks. One of those two will be there.
 *
 * DEDUPED AND CAPPED, because a Gothic frame sets user data thousands of times and most writes repeat. What the
 * cap dropped is printed rather than left as a silence that reads like completeness. */
/* ⚠ THE CAP IS A KNOB NOW, AND 48 WAS TOO SMALL BY AN ORDER OF MAGNITUDE. One ordinary Gothic submission
 * followed 48 distinct pointers and dropped 118 - so the run covered a quarter of what it looked at, and the
 * pointer that matters could have been in the three quarters it did not print. `ORBIS_FOLLOW_MAX=<n>`. */
/* ⚠ AND 4096 WAS STILL TOO SMALL. The run that used it followed 4096 and DROPPED 3693 - so ~7800 distinct
 * pointers exist in one Gothic world frame and the run covered half of them, which is the same failure the
 * comment above describes at a smaller number. 8192 covers the frame that has been measured; the log still
 * states what it dropped, because a cap that silently fits is indistinguishable from one that silently does
 * not. */
#define ORBIS_FOLLOW_CAP 8192

static void
orbis_follow_sh_pointers(uint64_t seq, const uint32_t *w, uint32_t n)
{
   /* ⚠ THE DEAD-POOL CHECK RUNS WHETHER OR NOT THE VERBOSE DUMP IS ARMED, and the knob now only controls
    * the printing. A use-after-free is not a curiosity to switch on when you happen to suspect it - it is
    * the defect this hunt is left with, it costs one ranged compare per pointer, and the run that needed it
    * is always the run where nobody thought to arm it. The dump is what is expensive; the check is not. */
   const bool verbose = getenv("ORBIS_FOLLOW_SH") != NULL;

   static uint64_t seen[ORBIS_FOLLOW_CAP];
   const char *const cap_s = getenv("ORBIS_FOLLOW_MAX");
   unsigned          cap = cap_s != NULL ? (unsigned)strtoul(cap_s, NULL, 10) : 512;
   if (cap > ORBIS_FOLLOW_CAP)
      cap = ORBIS_FOLLOW_CAP;
   unsigned n_seen = 0, dropped = 0, dup = 0;
   const uint64_t hi = orbis_va_base & 0xffffffff00000000ull;

   /* ⚠ AGGREGATE BY BUFFER, BECAUSE EIGHT THOUSAND HEX BLOCKS ARE NOT AN ANSWER.
    *
    * The question this dump exists for is "does a user-data pointer land somewhere that is not a descriptor
    * set" - and the previous run answered it with 4096 entries of sixteen dwords each for a human to classify.
    * A per-buffer tally makes the shape visible in twenty lines: a descriptor pool referenced by thousands of
    * pointers is expected, a 128 MiB texture heap referenced by twelve is not. */
   struct { uint32_t bo; uint64_t bytes; unsigned hits; } tally[64];
   unsigned n_tally = 0, tally_dropped = 0;

   for (uint32_t i = 0; i + 1 < n;) {
      const uint32_t hdr = w[i];
      if ((hdr & 0xC0000000u) != 0xC0000000u)
         break;
      const uint32_t op = (hdr >> 8) & 0xFF;
      const uint32_t cnt = ((hdr >> 16) & 0x3FFFu) + 1;

      /* IT_SET_SH_REG, and only it: context and uconfig registers are state, not pointers. */
      if (op == 0x76 && cnt >= 2) {
         const uint32_t reg = w[i + 1] & 0xFFFFu;
         for (uint32_t k = 2; k <= cnt && i + k < n; ++k) {
            const uint64_t va = hi | (uint64_t)w[i + k];
            /* ⚠ IN THE ARENA IS NOT THE SAME AS READABLE, AND THIS DEREFERENCED THE DIFFERENCE.
             *
             * The test here used to be `va < orbis_va_base || va + 64 > orbis_va_end` - the ARENA bounds. But
             * the arena is one flat mapping whose pages start mapped-and-inaccessible, and only become backed
             * where RADV has placed a BO. So an address can be squarely inside the arena and belong to nobody,
             * and reading it takes the process down with SIGSEGV - which is exactly what happened on the run
             * this was armed for: the dump reached its fifteenth pointer, printed `SH 0x243 + 1 ->
             * 0x241200000:`, and died before it could print a single dword of it. The klog named the frame
             * (ac_drm_cs_submit_raw2) even though its register dump described the signal handler.
             *
             * orbis_va_is_mapped answers the question the arena bounds only look like they answer, and it has
             * been in this file since the hang audits. Use it. */
            /* ⚠ THE DEAD-POOL CHECK GOES BEFORE THE MAPPED CHECK, and putting it after was a defect the
             * host test caught before any flash. A destroyed pool whose range nothing has claimed yet is
             * NOT in the live-mapping table, so the gate below would `continue` past it and the detector
             * would only ever see the half of the cases where the memory had already been re-let. That is
             * the half that is hardest to argue about, and the other half is the clean one. */
            {
               uint64_t dlo = 0, dhi = 0;
               const char *dwhat = "?";
               if (ac_orbis_va_in_freed_range(va, &dlo, &dhi, &dwhat)) {
                  static unsigned said;
                  if (orbis_budget(&said, 8))
                     mesa_loge("orbis-drm: USE AFTER FREE - submit #%" PRIu64 " binds SH 0x%03x to 0x%" PRIx64
                               ", which is 0x%" PRIx64 " into a %s of 0x%" PRIx64
                               " bytes that was FREED. The arena keeps that address mapped, so every bounds "
                               "check in this driver passes and the shader reads whoever owns the memory now.",
                               seq, reg, va, va - dlo, dwhat, dhi - dlo);
               }
            }

            if (!orbis_va_is_mapped(va, 64))
               continue;


            bool already = false;
            for (unsigned d = 0; d < n_seen; ++d)
               if (seen[d] == va)
                  already = true;
            if (already) {
               ++dup;
               continue;
            }
            if (n_seen == cap) {
               ++dropped;
               continue;
            }
            seen[n_seen++] = va;

            /* ⚠ AND THE FREED-BUFFER CASE IS INSIDE THE DESCRIPTOR, NOT IN THE POINTER.
             *
             * The check above catches a user-data pointer that still names released storage - a descriptor
             * SET whose pool went. It cannot catch a released BUFFER, because freeing a buffer does not
             * touch the set's address: the stale value is the base field of a V# sitting in the set's
             * memory. Reading the pointer's contents is the only place that shape is visible.
             *
             * A V# holds base[31:0] in word 0 and base[47:32] in the low bits of word 1. Sixteen dwords is
             * four of them - enough to see whether a set points at something that has been freed without
             * turning this into a descriptor decoder. A false positive would need a dword pair that both
             * looks like a base AND lands inside a range freed recently enough to still be in the ring,
             * which is a much narrower coincidence than the ones this hunt has already been burned by. */
            {
               const uint32_t *const q = (const uint32_t *)(uintptr_t)va;

               for (unsigned d = 0; d < 4; ++d) {
                  const uint64_t base = (uint64_t)q[d * 4] | ((uint64_t)(q[d * 4 + 1] & 0xFFFFu) << 32);
                  uint64_t blo = 0, bhi = 0;
                  const char *bwhat = "?";

                  if (base == 0 || !ac_orbis_va_in_freed_range(base, &blo, &bhi, &bwhat))
                     continue;

                  static unsigned said_desc;
                  if (orbis_budget(&said_desc, 8))
                     mesa_loge("orbis-drm: USE AFTER FREE - submit #%" PRIu64 " has a descriptor at 0x%" PRIx64
                               " (SH 0x%03x) whose base 0x%" PRIx64 " is 0x%" PRIx64 " into a %s of 0x%" PRIx64
                               " bytes that was FREED. The shader will read it, because this arena never "
                               "unmaps anything.",
                               seq, va, reg, base, base - blo, bwhat, bhi - blo);
               }
            }

            if (!verbose)
               continue;

            /* ⚠ NAME THE BUFFER, BECAUSE THAT IS THE ACTUAL QUESTION. This dump exists to decide whether a
             * user-data pointer lands in a descriptor set or in a texture heap - and sixteen dwords of hex
             * make that a judgement call, while "handle 4, offset 0x1a20 of a 128 MiB BO" does not. */
            const uint32_t *const q = (const uint32_t *)(uintptr_t)va;
            uint64_t lo = 0, mhi = 0;
            uint32_t bo = 0;
            if (orbis_va_mapping_of(va, &lo, &mhi, &bo)) {
               mesa_logi("orbis-drm:   SH 0x%03x + %u -> 0x%" PRIx64 ": BO %u, offset 0x%" PRIx64 " of %" PRIu64
                         " KiB",
                         reg, k - 2, va, bo, va - lo, (mhi - lo) / 1024);
               unsigned t = 0;
               while (t < n_tally && tally[t].bo != bo)
                  ++t;
               if (t < n_tally)
                  ++tally[t].hits;
               else if (n_tally < ARRAY_SIZE(tally)) {
                  tally[n_tally].bo = bo;
                  tally[n_tally].bytes = mhi - lo;
                  tally[n_tally].hits = 1;
                  ++n_tally;
               } else
                  ++tally_dropped;
            } else {
               mesa_logi("orbis-drm:   SH 0x%03x + %u -> 0x%" PRIx64 ":", reg, k - 2, va);
            }
            for (unsigned r = 0; r < 2; ++r)
               mesa_logi("orbis-drm:     %08x %08x %08x %08x %08x %08x %08x %08x", q[r * 8 + 0], q[r * 8 + 1],
                         q[r * 8 + 2], q[r * 8 + 3], q[r * 8 + 4], q[r * 8 + 5], q[r * 8 + 6], q[r * 8 + 7]);
         }
      }

      i += 1 + cnt;
   }

   if (!verbose)
      return;

   mesa_logi("orbis-drm: submit #%" PRIu64 " followed %u distinct SH pointers (%u repeats skipped, %u DROPPED at "
             "the %u cap - raise it if the interesting one is missing)",
             seq, n_seen, dup, dropped, cap);
   for (unsigned t = 0; t < n_tally; ++t)
      mesa_logi("orbis-drm:   BO %-4u %8" PRIu64 " KiB  <- %u user-data pointer(s)", tally[t].bo,
                tally[t].bytes / 1024, tally[t].hits);
   if (tally_dropped != 0)
      mesa_logi("orbis-drm:   (%u more pointer(s) fell in buffers past the %zu-buffer tally)", tally_dropped,
                ARRAY_SIZE(tally));
}

#define ORBIS_FENCE_CB_PAD_DW 8 /* pad_dw:0x7 - the size must be a multiple of 8 dwords */

/* ⚠ ONE PAD, BECAUSE THREE COPIES MEANT ONE OF THEM WAS WRONG FOR THE WHOLE LIFE OF THIS PORT.
 *
 * A type-3 NOP is a header plus its body, so the smallest one is TWO dwords, and ORBIS_PM4_TYPE3 encodes the
 * body count as `body - 1` masked to 14 bits. Ask it for a body of zero and it wraps: the header comes out
 * 0xFFFF1000, a NOP claiming 16384 data dwords, and if that lands as the last dword of a stream the CP is
 * told to skip 16384 dwords past the end of an IB we sized at 2320.
 *
 * The two register-poke IBs both spelled the one-dword case out - a type-2 packet is a single dword with no
 * body, which is exactly what a one-dword hole needs. The SUBMIT path did not, and its own validator had been
 * reporting the consequence on every submission whose stream came out 7 mod 8:
 *
 *     OUR OWN STREAM RUNS PAST ITS END - submit #106 packet at dword 2319
 *     (op 0x10, 16384 data dwords) needs 18704 dwords but the buffer is 2320
 *
 * That was read as a leftover of RADV's reserved-space NOP the flattener had missed, because the header is
 * byte-identical to one. It is not: the flattener never sees this dword, it is written after it returns.
 * `git log -L` puts the block in the arm's first commit, so it predates the progress markers - what the
 * markers changed is the flattened length, and with it WHICH submissions land on 7 mod 8. */
static uint32_t *
orbis_pad_ib(uint32_t *dw, const uint32_t *base)
{
   const unsigned used = (unsigned)(dw - base);
   const unsigned pad = (ORBIS_FENCE_CB_PAD_DW - (used % ORBIS_FENCE_CB_PAD_DW)) % ORBIS_FENCE_CB_PAD_DW;

   if (pad >= 2) {
      *dw++ = ORBIS_PM4_TYPE3(0x10 /* IT_NOP */, pad - 1);
      for (unsigned i = 1; i < pad; i++)
         *dw++ = 0;
   } else if (pad == 1) {
      *dw++ = 0x80000000u; /* type-2: one dword, no body, skipped by the CP */
   }
   return dw;
}

/* ⚠ AND THE PAD HAS ONE TOO, BECAUSE ITS BUG WAS INVISIBLE FOR THE LIFE OF THE PORT.
 *
 * orbis_pad_ib's failure was not a crash or a wrong picture: it wrote one wrong dword at the very end of a
 * stream, on the one submission in eight whose length came out 7 mod 8, and the only thing that ever said so
 * was a validator line that was read as being about something else. So it is checked here, on both arms, at
 * every boot - every starting length modulo 8, each one walked as the CP would walk it.
 *
 * This costs eight iterations of a loop over sixteen dwords and answers the one question that matters: does
 * the stream this arm hands the GPU end EXACTLY where it says it does. */
/* ⚠ ON BOTH ARMS, AND NEXT TO THE SELF-TESTS FOR THE REASON STATED THERE. This block used to sit in
 * the console-only arena setup, so the laptop had never once run it - the very mistake the comment
 * beside those self-tests was written about. Anything that characterises the PLATFORM belongs where
 * both arms reach it, or its first real test is a flash. */
static void
orbis_characterise_clocks(void)
{
/* ⚠ WHICH CLOCK IS clock_gettime(1) ON THIS KERNEL? A numeric-constant question, and this port has already been
 * burned by one: the toolchain ships musl's headers, which carry LINUX's clock ids -
 *
 *     openorbis/include/time.h:86   #define CLOCK_MONOTONIC          1
 *     openorbis/include/time.h:87   #define CLOCK_PROCESS_CPUTIME_ID 2
 *
 * - while the kernel underneath is FreeBSD-derived, where the same numbers mean different clocks. Every timeout in
 * Mesa and in this arm goes through os_time_get_nano(), so if id 1 is a CPU-time clock here, every bounded wait is
 * measuring the wrong thing and stops advancing exactly when the thread blocks.
 *
 * MEASURED RATHER THAN ASSERTED, because the FreeBSD header is not in this project's oracles and I will not quote
 * numbers I cannot cite: sleep a known wall interval and print how far each candidate id moved. A clock that
 * advances by roughly the sleep is a wall clock; one that barely moves is CPU time.
 *
 * 20 ms, once, at device init - cheap enough to leave in permanently, and the answer belongs in every log. */
/* ⚠ AND TWO PHASES, BECAUSE ONE CANNOT TELL A CPU CLOCK FROM A BROKEN ONE.
 *
 * The sleep phase alone says "advanced by roughly the sleep" or "barely moved", and BOTH a CPU-time
 * clock and a clock that is simply not implemented fall in the second bucket. Adding a phase that
 * BURNS the same interval separates them: a CPU-time clock is flat while asleep and advances while
 * spinning; a stopped clock is flat in both.
 *
 * WHY THIS IS WORTH THE EXTRA 20 ms AT BOOT. "Is the CPU waiting or working" is the question the
 * frame budget turns on, and CLOCK_PROCESS_CPUTIME_ID answers it for ALL blocking with one clock
 * read per report - including waits nobody instrumented. The alternative that was tried and reverted
 * (148786b8263) summed hand-placed counters around individual poll loops, and a code review found
 * nine separate reasons its percentage would be wrong: it missed the WSI flip-slot loop entirely, it
 * counted mutex waits as CPU work, it summed across threads against a single-threaded wall clock,
 * and it leaned on orbis_idle_wait_ns - which this project had ALREADY measured as 55x wrong and
 * written a standing instruction not to use. An instrument that can only see waits somebody
 * remembered to wrap is the wrong shape for this question.
 *
 * ids 2 and 3 are what musl's headers call CLOCK_PROCESS_CPUTIME_ID and CLOCK_THREAD_CPUTIME_ID. As
 * with ids 1 and 4 above, the NUMBER is what is being tested here, not the name: the toolchain ships
 * Linux's constants over a FreeBSD-derived kernel, so what each id means is a measurement. */
{
#define ORBIS_CLOCK_IDS 5
   struct timespec t0[ORBIS_CLOCK_IDS], t1[ORBIS_CLOCK_IDS], t2[ORBIS_CLOCK_IDS];
   static const int ids[ORBIS_CLOCK_IDS] = {
      0 /* REALTIME on both */,       1 /* MONOTONIC per musl */,
      2 /* PROCESS_CPUTIME per musl */, 3 /* THREAD_CPUTIME per musl */,
      4 /* MONOTONIC_RAW per musl */
   };
   bool got[ORBIS_CLOCK_IDS];

   for (unsigned i = 0; i < ORBIS_CLOCK_IDS; ++i) {
      got[i] = clock_gettime(ids[i], &t0[i]) == 0;
      if (!got[i])
         t0[i].tv_sec = t0[i].tv_nsec = 0;
   }

   /* Sony's usleep on the console - os_time_sleep does not return there, which orbis_poll_pause documents
    * at length - and the ordinary one on the laptop, so this function runs on both arms. */
#if defined(__PS4__)
   sceKernelUsleep(20 * 1000);
#else
   os_time_sleep(20 * 1000);
#endif

   for (unsigned i = 0; i < ORBIS_CLOCK_IDS; ++i)
      if (!got[i] || clock_gettime(ids[i], &t1[i]) != 0)
         t1[i] = t0[i];

   /* Burn 20 ms rather than sleep it. volatile so the loop survives -O2, and bounded by the wall
    * clock the phase above just identified rather than by an iteration count nobody can calibrate. */
   {
      const uint64_t spin_until = os_time_get_nano() + 20ull * 1000 * 1000;
      volatile uint64_t sink = 0;
      while (os_time_get_nano() < spin_until)
         sink += 1;
      (void)sink;
   }

   for (unsigned i = 0; i < ORBIS_CLOCK_IDS; ++i)
      if (!got[i] || clock_gettime(ids[i], &t2[i]) != 0)
         t2[i] = t1[i];

   for (unsigned i = 0; i < ORBIS_CLOCK_IDS; ++i) {
      const int64_t slept =
         (int64_t)(t1[i].tv_sec - t0[i].tv_sec) * 1000000000ll + (t1[i].tv_nsec - t0[i].tv_nsec);
      const int64_t spun =
         (int64_t)(t2[i].tv_sec - t1[i].tv_sec) * 1000000000ll + (t2[i].tv_nsec - t1[i].tv_nsec);
      const bool moved_asleep = slept > 15000000;
      const bool moved_busy = spun > 15000000;
      const char *verdict;

      if (!got[i])
         verdict = "REFUSED - clock_gettime does not answer for this id";
      else if (moved_asleep && moved_busy)
         verdict = "WALL CLOCK - advances whether the process runs or not";
      else if (!moved_asleep && moved_busy)
         verdict = "CPU TIME - flat while asleep, advances while busy. THIS ANSWERS 'waiting or working'";
      else if (!moved_asleep && !moved_busy)
         verdict = "STOPPED or unimplemented - flat in both phases, useless";
      else
         verdict = "advances only while asleep, which is nonsense - do not use";

      mesa_logi("orbis-drm: clock id %d: %lld us across a 20000 us SLEEP, %lld us across a 20000 us "
                "SPIN - %s",
                ids[i], (long long)(slept / 1000), (long long)(spun / 1000), verdict);
   }
#undef ORBIS_CLOCK_IDS
}

   /* ⚠ AND THE FALLBACK, MEASURED THE SAME WAY, BECAUSE THE CLOCKS ABOVE MAY ALL BE WALL CLOCKS.
    *
    * If no clock id reports CPU time, the question "is the CPU waiting or working" still has an answer -
    * getrusage's ru_utime + ru_stime is CPU time by definition. What is NOT obvious on this platform is
    * whether the call works at all: the toolchain's libc is musl, which issues LINUX syscall numbers,
    * and the kernel underneath is FreeBSD-derived. That mismatch is why os_time_sleep does not return
    * here, and it would silently poison this too.
    *
    * It should nevertheless work, and for a reason worth writing down: libkernel.so exports `getrusage`
    * ITSELF (nm -D shows `T getrusage`), so the link resolves to SONY's implementation rather than
    * musl's syscall stub. The header's `__REDIR(getrusage, __getrusage_time64)` is behind _REDIR_TIME64,
    * which nothing in this SDK defines, so the plain symbol is what is referenced.
    *
    * Same two phases and the same verdicts as the clock ids, because the same two failure modes exist:
    * a value that tracks the wall clock is not CPU time, and a value that never moves is not implemented. */
   {
      struct rusage r0, r1, r2;
      const bool ok0 = getrusage(RUSAGE_SELF, &r0) == 0;

#if defined(__PS4__)
      sceKernelUsleep(20 * 1000);
#else
      os_time_sleep(20 * 1000);
#endif
      const bool ok1 = ok0 && getrusage(RUSAGE_SELF, &r1) == 0;

      {
         const uint64_t spin_until = os_time_get_nano() + 20ull * 1000 * 1000;
         volatile uint64_t sink = 0;
         while (os_time_get_nano() < spin_until)
            sink += 1;
         (void)sink;
      }
      const bool ok2 = ok1 && getrusage(RUSAGE_SELF, &r2) == 0;

      if (!ok2) {
         mesa_logi("orbis-drm: getrusage(RUSAGE_SELF) REFUSED - no CPU-time source from this route");
      } else {
#define ORBIS_RU_US(r) ((int64_t)(r).ru_utime.tv_sec * 1000000ll + (r).ru_utime.tv_usec + \
                        (int64_t)(r).ru_stime.tv_sec * 1000000ll + (r).ru_stime.tv_usec)
         const int64_t slept = ORBIS_RU_US(r1) - ORBIS_RU_US(r0);
         const int64_t spun = ORBIS_RU_US(r2) - ORBIS_RU_US(r1);
         const bool moved_asleep = slept > 15000;
         const bool moved_busy = spun > 15000;

         mesa_logi("orbis-drm: getrusage user+sys: %lld us across a 20000 us SLEEP, %lld us across a "
                   "20000 us SPIN - %s",
                   (long long)slept, (long long)spun,
                   !moved_asleep && moved_busy
                      ? "CPU TIME. THIS ANSWERS 'waiting or working' and needs no clock id"
                      : (moved_asleep && moved_busy ? "tracks the WALL CLOCK - not CPU time"
                                                    : "flat in both phases - not implemented"));
#undef ORBIS_RU_US
      }
   }

}

/* ⚠ THE USE-AFTER-FREE DETECTOR HAS A SELF-TEST, because a detector that cannot be seen to fire is a
 * detector nobody can trust when it stays silent - and this one has already been wrong once.
 *
 * Its first version put the dead-pool comparison AFTER the live-mapping gate, so a destroyed pool whose
 * range nothing had claimed yet was skipped before the comparison ran. A host probe that performs the exact
 * abuse - pool, set, bind, destroy the pool, submit - produced no warning at all, which is what sent me
 * looking. That probe still could not prove the fix, because a command buffer with no dispatch emits no
 * descriptor pointers, so the check is exercised here instead: a synthetic stream that sets a user-data
 * register to an address inside a range this test has just declared destroyed.
 *
 * Runs on both arms at every boot, like the flattener and the pad. */
static void
orbis_selftest_dead_pool(void)
{
   const uint64_t fake_pool = 0xdead0000ull;
   const uint64_t fake_size = 0x1000ull;

   ac_orbis_note_freed_range(fake_pool, fake_size, "self-test range");

   if (!ac_orbis_va_in_freed_range(fake_pool + 0x40, NULL, NULL, NULL)) {
      mesa_loge("orbis-drm: DEAD-POOL SELF-TEST FAILED - an address inside a destroyed pool was not "
                "recognised, so the use-after-free detector cannot fire at all");
      return;
   }
   if (ac_orbis_va_in_freed_range(fake_pool + fake_size, NULL, NULL, NULL)) {
      mesa_loge("orbis-drm: DEAD-POOL SELF-TEST FAILED - the byte one past a destroyed pool was reported "
                "as inside it, so the detector would accuse innocent addresses");
      return;
   }

   mesa_logi("orbis-drm: dead-pool self-test: a destroyed range is recognised and its first byte past the "
             "end is not - OK");
}

/* ⚠ DOES THIS CONSOLE'S KERNEL ACTUALLY HONOUR _umtx_op? Nothing has ever asked.
 *
 * dEQP-VK.api.object_management.multithreaded_per_thread_device.descriptor_pool hangs - measured,
 * not suspected: over 180 seconds the result file and the driver's own log did not change by a
 * single byte. No fault, no kernel message, no timeout. That is what a lost wakeup looks like.
 *
 * Mesa's simple_mtx is built on futexes, and on this port futex_wait/futex_wake reach FreeBSD's
 * _umtx_op through a shim of ours (build-support/orbis/shims/sys/umtx.h, syscall 454). Everything
 * this port has run until today was effectively single-threaded, so an uncontended simple_mtx never
 * called futex_wait once. The first genuinely multithreaded test is the first exercise of that path.
 *
 * ⚠ AND THE ASYMMETRY IS THE WHOLE POINT. If WAIT is refused, a contended lock becomes a spin -
 * wasteful and visible. If WAKE is refused or silently does nothing, the waiter sleeps forever and
 * the process hangs with nothing in any log. The second is what we are looking at, so this has to
 * distinguish them rather than report "the futex works".
 *
 * Bounded: the waiter is given up on after two seconds with a verdict, rather than hanging the
 * caller the way the thing it is diagnosing does. */
/* ⚠ NOT os_time_get_nano(), AND THE REASON IS MEASURED RATHER THAN SUSPECTED.
 *
 * os_time_get_nano() is timespec_get(TIME_MONOTONIC), i.e. CLOCK_MONOTONIC - and what that id means
 * on this console DEPENDS ON THE PROCESS. In OpenGothic it is a wall clock (20745 us across a 20 ms
 * sleep). In the CTS binary it is CPU TIME: flat while asleep, advancing only while running, three
 * runs in a row. Same console, same id, two answers; orbis_characterise_clocks() prints both.
 *
 * That makes os_time_get_nano() useless for timing a thread that SLEEPS, which is precisely what
 * these probes do. It reported "the waiter came back after 0 us" for a waiter that had slept
 * perfectly well, and the verdict built on it was a false failure - a ruler that stops running at
 * the exact moment the thing being measured happens.
 *
 * CLOCK_REALTIME (id 0) measured as a wall clock in BOTH processes, so that is what these use. It
 * can be stepped by a time sync, which over a 200 ms probe is a risk worth taking against a clock
 * that is known to be wrong here.
 */
static uint64_t
orbis_walltime_ns(void)
{
   struct timespec t;
   if (clock_gettime(CLOCK_REALTIME, &t) != 0)
      return os_time_get_nano();
   return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

struct orbis_futex_probe {
   uint32_t word;
   int wait_ret;
   int wait_errno;
   uint64_t waited_ns;
   unsigned wakeups;
   volatile bool finished;
};

static void *
orbis_futex_probe_waiter(void *arg)
{
   struct orbis_futex_probe *const p = arg;
   const uint64_t t0 = orbis_walltime_ns();

   /* ⚠ A SPURIOUS WAKEUP IS NOT A FAILURE, and the first version of this test called one. A futex
    * promises not to LOSE a wakeup; it never promised not to add one, and every correct waiter -
    * Mesa's included - re-checks its condition in a loop. This one does too, so what it measures is
    * the thing that matters: whether the word's change is ever observed. */
   do {
      p->wait_ret = futex_wait(&p->word, 0, NULL);
      p->wait_errno = errno;
      p->wakeups++;
   } while (p->word == 0 && p->wait_ret == 0);
   p->waited_ns = orbis_walltime_ns() - t0;
   p->finished = true;
   return NULL;
}

/* ⚠ THE TIMED PATH IS A SECOND FUTEX AND THE FIRST TEST NEVER TOUCHED IT.
 *
 * futex_wait takes an optional deadline, and the two arms are different code with different failure
 * modes. The untimed one was measured working; the timed one was written on an ASSUMPTION - that
 * FreeBSD's absolute CLOCK_MONOTONIC deadline could be handed to pthread_cond_timedwait unchanged -
 * and the assumption was wrong. A monotonic instant read as a realtime one is decades in the past,
 * so the wait returned ETIMEDOUT immediately and the caller spun.
 *
 * Nothing in RADV calls the timed arm today (only gallium's u_queue does), so this was latent. That
 * is the reason to test it rather than a reason not to: a latent defect in a lock primitive is one
 * caller away from being a live one, and it cost nothing to find because the word never changes.
 *
 * ⚠ BOUNDED, because the failure it looks for is a wait that is too LONG. Getting the conversion
 * wrong in the other direction puts the deadline about fifty-five years out, and a self-test that
 * hangs driver initialisation is worse than the defect. The waiter is given up on after a second. */
#define ORBIS_FUTEX_TIMED_MS 150

struct orbis_futex_timed_probe {
   uint32_t      word;
   int           ret;
   uint64_t      waited_ns;
   volatile bool finished;
};

static void *
orbis_futex_timed_waiter(void *arg)
{
   struct orbis_futex_timed_probe *const p = arg;
   struct timespec deadline;

   clock_gettime(CLOCK_MONOTONIC, &deadline);
   deadline.tv_nsec += ORBIS_FUTEX_TIMED_MS * 1000 * 1000;
   if (deadline.tv_nsec >= 1000000000L) {
      deadline.tv_nsec -= 1000000000L;
      deadline.tv_sec++;
   }

   const uint64_t t0 = orbis_walltime_ns();
   /* Nobody ever changes the word and nobody ever wakes it, so the deadline is the only way out. */
   p->ret       = futex_wait(&p->word, 0, &deadline);
   p->waited_ns = orbis_walltime_ns() - t0;
   p->finished  = true;
   return NULL;
}

/* The shim's own clock probe, read through an extern rather than an include: sys/umtx.h owns a
 * static bucket table and a link-time sentinel that makes a second includer fail, so this file must
 * NOT include it. Only the console build has the shim at all. */
#if defined(__PS4__)
extern int orbis_umtx_cond_clock_probed(void);
extern int64_t orbis_umtx_cond_probe_ms(void);
extern int orbis_umtx_waiter_count(void);
extern int orbis_umtx_waiter_at(int i, uint64_t *thread, const void **addr, uint64_t *since_ns);
extern uint64_t orbis_umtx_now_ns(void);
#endif

static void
orbis_selftest_futex_timed(void)
{
   struct orbis_futex_timed_probe p = {.word = 0};
   pthread_t th;

#if defined(__PS4__)
   /* ⚠ PRINTED BEFORE THE VERDICT, because it is what makes the verdict readable. If the timed wait
    * fails, this line separates "the deadline was converted onto the wrong clock" from "the probe
    * chose the wrong clock to convert onto" - two different repairs that look identical without it.
    * The measured interval is against a 20 ms target. */
   mesa_logi("orbis-drm: futex shim: condition variables measure deadlines against %s "
             "(the probe's own 20 ms wait took %lld ms)",
             orbis_umtx_cond_clock_probed() == CLOCK_MONOTONIC ? "CLOCK_MONOTONIC" : "CLOCK_REALTIME",
             (long long)orbis_umtx_cond_probe_ms());
#endif

   if (pthread_create(&th, NULL, orbis_futex_timed_waiter, &p) != 0) {
      mesa_logw("orbis-drm: futex timed self-test: could not start the waiter thread - test skipped");
      return;
   }

   for (unsigned i = 0; i < 100 && !p.finished; ++i) {
#if defined(__PS4__)
      sceKernelUsleep(10 * 1000);
#else
      os_time_sleep(10 * 1000);
#endif
   }

   if (!p.finished) {
      mesa_loge("orbis-drm: futex timed self-test: FAILED - still asleep one second after a %d ms "
                "deadline. The deadline was converted onto a clock whose numbers are far larger, so "
                "it lies in the distant future and the wait will never end.",
                ORBIS_FUTEX_TIMED_MS);
      pthread_detach(th);
      return;
   }

   pthread_join(th, NULL);

   /* Two thirds of the interval. A wait that comes back a little early is still a wait; one that
      never slept comes back in microseconds, and the two are nowhere near each other. */
   if (p.waited_ns < (uint64_t)ORBIS_FUTEX_TIMED_MS * 2 / 3 * 1000 * 1000) {
      mesa_loge("orbis-drm: futex timed self-test: FAILED - the waiter returned after %llu us against "
                "a %d ms deadline (returned %d). The deadline is being measured against the wrong "
                "clock, so every timed wait expires instantly and its caller spins.",
                (unsigned long long)(p.waited_ns / 1000), ORBIS_FUTEX_TIMED_MS, p.ret);
      return;
   }

   mesa_logi("orbis-drm: futex timed self-test: OK - the waiter slept %llu us against a %d ms "
             "deadline and returned %d (ETIMEDOUT is %d). The deadline reaches the right clock.",
             (unsigned long long)(p.waited_ns / 1000), ORBIS_FUTEX_TIMED_MS, p.ret, ETIMEDOUT);
}

/* ⚠ THE INSTRUMENT FOR A PROCESS THAT HAS STOPPED SAYING ANYTHING.
 *
 * Every other diagnostic in this driver prints when something HAPPENS, so a hang silences all of
 * them at once - and then the only fact available is that the log ended, which is equally consistent
 * with a deadlock, a spin, a wait on the GPU, and a test that is simply slow.
 *
 * This prints on a timer instead. If the counters move, the process is working and nothing is hung.
 * If they are frozen but a lock has a holder and a waiter, the dump names both. If they are frozen
 * and no lock is held, the block is somewhere this driver cannot see - which is also an answer, and
 * a different one.
 *
 *     ORBIS_WATCHDOG=<seconds>
 */
/* ⚠ THE WATCHDOG WENT BLIND IN THE ONE CASE IT WAS BUILT FOR, and it went blind because it shared a
 * stream with the thing it was watching.
 *
 * With the flat arena the run reached test 22 and froze - and this time the watchdog printed its
 * startup line and then NOTHING, while mesa_log's file and dEQP's own trace both stopped growing at
 * the same byte. Every writer is stuck behind one libc FILE lock, which is a pthread mutex the
 * futex shim never sees, and the watchdog is a writer too. An instrument that queues behind its own
 * subject reports nothing at exactly the moment it matters.
 *
 * So it writes with write(2) to a descriptor of its own. No FILE, no buffering, no lock shared with
 * anybody: /data/orbis-watchdog.txt.
 *
 * ⚠ AND IT KEEPS USING mesa_log AS WELL, deliberately. When logging works, having the report in the
 * same file as the driver's own lines is what makes it readable in context; when logging jams, the
 * separate file is the only thing that survives. The two disagreeing is itself a finding - it says
 * the log is the blocked resource. */
static int orbis_watchdog_fd = -1;

static void
orbis_watchdog_say(const char *fmt, ...)
{
   if (orbis_watchdog_fd < 0)
      return;

   char line[512];
   va_list ap;
   va_start(ap, fmt);
   const int n = vsnprintf(line, sizeof(line) - 1, fmt, ap);
   va_end(ap);
   if (n <= 0)
      return;

   const size_t len = (size_t)n < sizeof(line) - 1 ? (size_t)n : sizeof(line) - 2;
   line[len] = '\n';
   /* Return value deliberately ignored: this is the path that runs when everything else is stuck,
      and it has nowhere to report a failure to. */
   (void)!write(orbis_watchdog_fd, line, len + 1);
}

static void *
orbis_watchdog_thread(void *arg)
{
   const unsigned period = (unsigned)(uintptr_t)arg;
   uint64_t last_submits = ~0ull;
   unsigned quiet = 0;

#if defined(__PS4__)
   orbis_watchdog_fd = open("/data/orbis-watchdog.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
   orbis_watchdog_say("watchdog: own descriptor %d, reporting every %u s. This file exists because "
                      "the driver's log is written through a FILE lock that a hang can hold.",
                      orbis_watchdog_fd, period);
#endif

   for (;;) {
#if defined(__PS4__)
      sceKernelUsleep(period * 1000 * 1000);
#else
      os_time_sleep(period * 1000 * 1000);
#endif

      const uint64_t submits = p_atomic_read(&orbis_submit_seq_no);
      quiet = (submits == last_submits) ? quiet + 1 : 0;
      last_submits = submits;

      /* ⚠ THE PRIVATE WRITE GOES FIRST, and getting this backwards would have wasted the whole
         change. Written second, it would sit behind the mesa_log call - and in the case this exists
         for, that call never returns, so the private file would stop at exactly the same moment as
         the log and record nothing extra. First, it keeps advancing through the jam, which is the
         entire point. */
      orbis_watchdog_say("watchdog: %llu submission(s), %u BO slot(s)%s",
                         (unsigned long long)submits, orbis_bo_slots,
                         quiet ? " - NO NEW SUBMISSION SINCE THE LAST REPORT" : "");
      mesa_logi("orbis-drm: watchdog: %llu submission(s), %u BO slot(s)%s",
                (unsigned long long)submits, orbis_bo_slots,
                quiet ? " - NO NEW SUBMISSION SINCE THE LAST REPORT" : "");

      unsigned held = 0;
      for (unsigned i = 0; i < ORBIS_LOCK_SLOTS; i++) {
         struct orbis_lock_state *const s = &orbis_lock_state[i];
         if (p_atomic_read(&s->mtx) == NULL)
            continue;

         const uint64_t holder = p_atomic_read(&s->holder);
         const uint32_t waiters = p_atomic_read(&s->waiters);

         /* A free, uncontended lock is the normal case and printing it every period would bury the
            one line that matters. */
         if (holder == 0 && waiters == 0)
            continue;

         held++;
         orbis_watchdog_say("watchdog:   %s held by thread 0x%llx, %u waiting, %llu acquisition(s)%s%s",
                            s->name, (unsigned long long)holder, waiters, (unsigned long long)s->acquires,
                            (s->mtx == (void *)&orbis_map_lock || s->mtx == (void *)&orbis_kernel_mem_lock)
                               ? " - holder is at: " : "",
                            s->mtx == (void *)&orbis_map_lock       ? p_atomic_read(&orbis_map_step)
                            : s->mtx == (void *)&orbis_kernel_mem_lock ? orbis_kmem_step
                                                                       : "");
         mesa_logw("orbis-drm: watchdog:   %s held by thread 0x%llx, %u waiting, %llu acquisition(s)%s%s",
                   s->name, (unsigned long long)holder, waiters, (unsigned long long)s->acquires,
                   (s->mtx == (void *)&orbis_map_lock || s->mtx == (void *)&orbis_kernel_mem_lock)
                      ? " - holder is at: " : "",
                   s->mtx == (void *)&orbis_map_lock       ? p_atomic_read(&orbis_map_step)
                   : s->mtx == (void *)&orbis_kernel_mem_lock ? orbis_kmem_step
                                                              : "");
      }

      /* ⚠ EVERY CONTENDED simple_mtx IN MESA, not just this driver's eight. The first version of
       * this watchdog could only see the arm's own locks, reported "the block is outside the arm",
       * and stopped there - true, and useless. On this console futex_wait IS the shim, so any
       * thread asleep on any Mesa lock anywhere is inside it and registers itself. */
#if defined(__PS4__)
      unsigned sleepers = 0;
      {
         const int n = orbis_umtx_waiter_count();
         const uint64_t now = orbis_umtx_now_ns();
         for (int i = 0; i < n; i++) {
            uint64_t thread = 0, since = 0;
            const void *addr = NULL;
            if (!orbis_umtx_waiter_at(i, &thread, &addr, &since))
               break;
            sleepers++;
            orbis_watchdog_say("watchdog:   thread 0x%llx asleep in futex_wait on %p for %llu ms",
                               (unsigned long long)thread, addr,
                               (unsigned long long)(since ? (now - since) / 1000000ull : 0));
            mesa_logw("orbis-drm: watchdog:   thread 0x%llx asleep in futex_wait on %p for %llu ms",
                      (unsigned long long)thread, addr,
                      (unsigned long long)(since ? (now - since) / 1000000ull : 0));
         }
      }
#else
      const unsigned sleepers = 0;
#endif

      /* ⚠ TWO OR MORE HELD ACROSS A QUIET PERIOD IS THE SHAPE OF A DEADLOCK, and saying so is the
         whole point - the reader should not have to know this driver's locking to read the dump. */
      if (quiet >= 2 && held >= 2)
         mesa_loge("orbis-drm: watchdog: %u locks held with waiters and nothing has progressed for "
                   "%u period(s) - this is a deadlock, and the lines above name the threads",
                   held, quiet);
      else if (quiet >= 2 && sleepers > 0)
         mesa_loge("orbis-drm: watchdog: nothing has progressed for %u period(s) and %u thread(s) are "
                   "asleep on Mesa locks OUTSIDE this driver - the addresses above are the locks",
                   quiet, sleepers);
      else if (quiet >= 2 && held == 0)
         mesa_logw("orbis-drm: watchdog: nothing has progressed for %u period(s), NO lock of this "
                   "driver is held and NOBODY is asleep in futex_wait - so the block is not a Mesa "
                   "lock at all. That leaves a pthread mutex or condvar this shim never sees, or a "
                   "blocking call outside Mesa entirely.",
                   quiet);
      /* ⚠ THIS CASE PRINTED NOTHING AT ALL in the last run, and it was the case that actually
         happened: exactly one lock held, no waiters, nobody asleep. Three branches that each looked
         complete left the most informative state silent. */
      else if (quiet >= 2 && held >= 1)
         mesa_loge("orbis-drm: watchdog: nothing has progressed for %u period(s) and %u lock(s) of "
                   "this driver are held with NOBODY waiting and NOBODY asleep in futex_wait. The "
                   "holder is blocked INSIDE its critical section on something that is not a Mesa "
                   "lock - the step named above is where.",
                   quiet, held);
   }
   return NULL;
}

static void
orbis_watchdog_start(void)
{
   const char *const e = getenv("ORBIS_WATCHDOG");
   if (e == NULL || e[0] == '0')
      return;

   unsigned period = (unsigned)strtoul(e, NULL, 10);
   if (period == 0)
      period = 5;

   pthread_t th;
   if (pthread_create(&th, NULL, orbis_watchdog_thread, (void *)(uintptr_t)period) != 0) {
      mesa_logw("orbis-drm: watchdog: could not start the thread");
      return;
   }
   pthread_detach(th);
   mesa_logi("orbis-drm: watchdog: reporting every %u s, and the arm's locks are recording their "
             "holders", period);
}

static void
orbis_selftest_futex(void)
{
   struct orbis_futex_probe p = {.word = 0};

   /* Cheapest possible probe first: a WAKE on an address with no waiters must return without error.
    * If the syscall is not answered at all, nothing below would mean anything. */
   /* ⚠ THE FIRST VERSION OF THIS TEST REPORTED OK ON A BROKEN FUTEX, and the numbers in its own
    * message said otherwise: "wait returned 78/errno 78, wake returned 78", with the waiter asleep
    * for 0 us. 78 is ENOSYS, and futex.c returns errno as a POSITIVE value - so the `< 0` check
    * never fired, futex_wait failed instantly instead of sleeping, and the thread finishing was read
    * as success. It checked that the waiter FINISHED rather than that the calls WORKED.
    *
    * Zero is success for both. Anything else is the verdict. */
   /* ⚠ IS IT THE SYSCALL, OR IS IT US CALLING IT? _umtx_op answers ENOSYS here, and that has two very
    * different explanations with two different fixes:
    *
    *   the kernel does not implement it   - nothing to do but replace it, which is what the shim does
    *   raw syscalls from our own text are refused - then the kernel HAS it, retail titles reach it
    *     through libkernel, and the interesting question becomes whether we can too
    *
    * getpid is the control. It is syscall 20 on FreeBSD, it cannot fail, and its answer is checkable
    * against the libc call - so if the raw one disagrees or errors, the problem is the CALL and not
    * the operation. */
   {
      const long raw = syscall(SYS_getpid);
      const pid_t via_libc = getpid();
      mesa_logi("orbis-drm: raw syscall check: syscall(SYS_getpid)=%ld errno=%d, getpid()=%d - %s",
                raw, raw < 0 ? errno : 0, (int)via_libc,
                raw == (long)via_libc ? "raw syscalls DO work, so _umtx_op is genuinely absent"
                                      : "raw syscalls are refused, so _umtx_op may exist behind libkernel");
   }

   const int wake_none = futex_wake(&p.word, 1);
   if (wake_none != 0) {
      mesa_loge("orbis-drm: futex self-test: FAILED - futex_wake on an idle address returned %d (%s). "
                "Every contended lock in Mesa is built on this, so they cannot sleep and cannot be "
                "woken.",
                wake_none, wake_none == 78 ? "ENOSYS: the kernel does not implement it" : "unexpected");
      return;
   }

   pthread_t th;
   if (pthread_create(&th, NULL, orbis_futex_probe_waiter, &p) != 0) {
      mesa_logw("orbis-drm: futex self-test: could not start the waiter thread - test skipped");
      return;
   }

#if defined(__PS4__)
   sceKernelUsleep(200 * 1000);
#else
   os_time_sleep(200 * 1000);
#endif

   p.word = 1;
   const int woke = futex_wake(&p.word, 1);

   for (unsigned i = 0; i < 200 && !p.finished; ++i) {
#if defined(__PS4__)
      sceKernelUsleep(10 * 1000);
#else
      os_time_sleep(10 * 1000);
#endif
   }

   if (!p.finished) {
      mesa_loge("orbis-drm: futex self-test: FAILED - the waiter is still asleep two seconds after "
                "futex_wake returned %d. WAIT sleeps and WAKE does not deliver, which is exactly the "
                "shape of the hang in the multithreaded CTS tests.",
                woke);
      pthread_detach(th);
      return;
   }

   pthread_join(th, NULL);

   /* ⚠ FINISHING IS NOT PASSING. A waiter that never slept also finishes. */
   if (p.wait_ret != 0 || woke != 0) {
      mesa_loge("orbis-drm: futex self-test: FAILED - the waiter returned rather than slept "
                "(wait %d, wake %d, %llu us). A futex that refuses to block turns every contended "
                "lock in Mesa into a spin.",
                p.wait_ret, woke, (unsigned long long)(p.waited_ns / 1000));
      return;
   }

   if (p.waited_ns < 100ull * 1000 * 1000) {
      mesa_loge("orbis-drm: futex self-test: FAILED - the waiter came back after %llu us across %u "
                "wakeup(s) even though the word only changed at 200 ms. It is not blocking, so every "
                "contended lock in Mesa is a spin.",
                (unsigned long long)(p.waited_ns / 1000), p.wakeups);
      return;
   }
   mesa_logi("orbis-drm: futex self-test: OK - the waiter SLEPT %llu us and was woken by the wake "
             "across %u wakeup(s) (wait returned %d/errno %d, wake returned %d). Both halves work; "
             "wakeups beyond the first are addresses sharing a bucket and are harmless.",
             (unsigned long long)(p.waited_ns / 1000), p.wakeups, p.wait_ret, p.wait_errno, woke);
}

static void
orbis_selftest_pad(void)
{
   uint32_t buf[ORBIS_FENCE_CB_PAD_DW * 3];
   const char *fail = NULL;

   for (unsigned used = 1; used <= ORBIS_FENCE_CB_PAD_DW * 2 && fail == NULL; used++) {
      memset(buf, 0, sizeof(buf));
      /* Fill the "already written" part with well-formed one-dword-body NOPs so the walk below starts on a
       * packet boundary; an odd `used` gets a single type-2 to make up the difference. */
      unsigned i = 0;
      while (i + 2 <= used) {
         buf[i] = ORBIS_PM4_TYPE3(0x10, 1);
         buf[i + 1] = 0;
         i += 2;
      }
      if (i < used)
         buf[i++] = 0x80000000u;

      const uint32_t *const end = orbis_pad_ib(buf + used, buf);
      const unsigned        n = (unsigned)(end - buf);

      if (n % ORBIS_FENCE_CB_PAD_DW != 0) {
         fail = "the padded length is not a multiple of the alignment";
         break;
      }

      /* Walk it as the CP does, and as the submit path's validator does. */
      for (unsigned k = 0; k < n;) {
         const uint32_t hdr = buf[k];
         if ((hdr & 0xC0000000u) == 0x80000000u) {
            k++;
            continue;
         }
         if ((hdr & 0xC0000000u) != 0xC0000000u) {
            fail = "a dword that is neither a type-2 nor a type-3 header";
            break;
         }
         const unsigned cnt = ((hdr >> 16) & 0x3FFFu) + 1;
         if (k + 1 + cnt > n) {
            fail = "a packet that runs past the end - this is the 0xFFFF1000 defect";
            break;
         }
         k += 1 + cnt;
      }
   }

   if (fail != NULL)
      mesa_loge("orbis-drm: PAD SELF-TEST FAILED - %s", fail);
   else
      mesa_logi("orbis-drm: pad self-test: every length mod %u pads to a stream that ends exactly at its size "
                "- OK",
                ORBIS_FENCE_CB_PAD_DW);
}

/* ⚠ OUTSIDE THE PLATFORM SEAM, because radv_queue calls it unconditionally and the host link failed when
 * it was not. Same rule this file has learned twice already, for orbis_emit_eop and for the pad: code that
 * both arms reach does not live inside the arm that is not always built.
 *
 * The value radv_emit_tess_factor_ring put in VGT_TF_MEMORY_BASE, remembered so the register ladder's
 * round-trip rung can write the RIGHT number rather than a magic one - a run where the write sticks then
 * leaves the register correct instead of corrupted. Zero until the rings exist. */
static uint32_t orbis_roundtrip_base;

void
ac_orbis_note_tf_base(uint32_t base_shifted)
{
   orbis_roundtrip_base = base_shifted;
}

/* The same idea for the GS rings, and for the same reason: the register ladder is worth far more if it fires
 * AFTER radv_emit_gs_ring_sizes has run, because then rungs 1-2 stop being "what did Sony leave" and become
 * "is the number we programmed actually in the register". Both are zero until a pipeline with a geometry
 * shader has made the queue build a preamble, which is the fact ORBIS_READ_REGS_AT=gs waits on. */
static uint32_t orbis_gs_ring_esgs_shifted;
static uint32_t orbis_gs_ring_gsvs_shifted;

void
ac_orbis_note_gs_ring_sizes(uint32_t esgs_shifted, uint32_t gsvs_shifted)
{
   orbis_gs_ring_esgs_shifted = esgs_shifted;
   orbis_gs_ring_gsvs_shifted = gsvs_shifted;
}

#if defined(__PS4__)
static volatile uint32_t *orbis_fence_label;
static volatile uint32_t *orbis_fence_scratch;
static uint32_t *orbis_fence_cb;
#else
static uint32_t orbis_host_label;
static volatile uint32_t *orbis_fence_label = &orbis_host_label;
#endif

/* ⚠ THE SUBMISSION THE GPU NEVER FINISHED, DUMPED WHILE THERE IS STILL A PROCESS TO DUMP IT.
 *
 * Eight console trips went on bisecting one hang with environment knobs, and six hypotheses died: synchronisation,
 * layering, 3D tiling, uninitialised memory, OpenGothic's HQ fog path, and the vertex index - the last of those
 * disproved per-pixel by a harness rung. What was never available is the thing that would have ended it on the
 * first trip: THE BYTES OF THE SUBMISSION THAT HUNG.
 *
 * ORBIS_DUMP_SUBMIT=<n> cannot do it. The number moves between runs - 6084, 6091, 6142, 8051 - because it depends
 * on how far the world load got, so a fixed n either misses or dumps the wrong one. And Sony's driver kills the
 * process about a second after it notices the graphics core is busy, so nothing that waits for our own five-second
 * fence gets to print anything at all.
 *
 * WHAT IDENTIFIES IT WITHOUT A GUESS: the fence label. Every submission ends with an end-of-pipe packet writing its
 * own sequence number, so `label` is the last one that FINISHED and `label + 1` is the one the GPU is stuck in. A
 * ring of the last few streams therefore always contains it, keyed by nothing but arithmetic.
 *
 * COST: one memcpy of the flattened stream per submission - a few hundred dwords typically, 7136 at the largest
 * measured - and ORBIS_HUNG_RING slots of .bss. Enabled by DEFAULT, and that is deliberate: a hang that produces no
 * dump has cost this project more than half a megabyte of memory ever will. ORBIS_DUMP_HUNG=0 turns it off. */
#define ORBIS_HUNG_RING 8

struct orbis_hung_slot {
   uint64_t seq;
   uint32_t dwords;
   uint32_t stream[ORBIS_FENCE_CB_BYTES / 4];
};
static struct orbis_hung_slot orbis_hung_ring[ORBIS_HUNG_RING];

/* Time spent in the fence-slot throttle since the last periodic sample, and the totals the WSI's idle wait keeps.
 * Plain globals under the submit lock, which is the only thing that touches the first of them. */
static uint64_t orbis_slot_wait_ns;

/* ⚠ THE COUNTERS LIVE IN radv_cmd_buffer.c, which this file does not include - declared rather than shared through
 * a header because the whole instrument is a diagnostic and a header would be a third thing to keep in step. Ids
 * must match that enum's order; 45 and 46 were verified against it when this was written. */
/* ⚠ THE API COUNTERS LIVE HERE, IN THIS REPOSITORY'S OWN FILE, AND THE DEPENDENCY RUNS THAT WAY ROUND ON PURPOSE.
 *
 * They started life inside the diagnostic patch that reports them (0051), because that is where the reporting is.
 * Then this file needed to add to them too - the submit path and the flatten memcpy are ours, not RADV's - and a
 * hard reference from here made the arm unlinkable in exactly the configuration the patch generator requires: a
 * tree with the patch absent. A weak fallback did not resolve it either.
 *
 * So the storage and the two accessors are here, where they are permanent, and the patch declares them extern and
 * keeps only the names table and the once-a-second report. When the diagnostic is eventually deleted, what is left
 * behind is two functions nobody calls rather than a build that does not link.
 *
 * SIZE 64 RATHER THAN THE ENUM'S COUNT, because the enum belongs to the patch: this file must not need to know how
 * many ids exist, only that no id exceeds the array. The patch's report iterates its own ORBIS_API_COUNT. */
uint64_t orbis_api_calls[64];
uint64_t orbis_api_nanos[64];
int      orbis_api_on = -1;

/* Prototypes before the definitions: this tree builds with -Wmissing-prototypes -Werror, and without them the file
 * does not compile - which is worth naming, because the archive then keeps a STALE object and the failure presents
 * as "undefined symbol" at link time instead of as the compile error it is. Two false trails came out of exactly
 * that: a weak-symbol theory and a stale-clone theory, both chasing a link error whose cause was three lines above
 * it in the same log. */
void orbis_api_count(unsigned id, uint64_t n);
void orbis_api_time(unsigned id, uint64_t ns);

void
orbis_api_count(unsigned id, uint64_t n)
{
   if (orbis_api_on < 0)
      orbis_api_on = getenv("ORBIS_COUNT_API") != NULL ? 1 : 0;
   if (orbis_api_on && id < 64)
      orbis_api_calls[id] += n;
}

void
orbis_api_time(unsigned id, uint64_t ns)
{
   if (orbis_api_on == 1 && id < 64)
      orbis_api_nanos[id] += ns;
}

enum { ORBIS_ID_ARM_SUBMIT = 45, ORBIS_ID_ARM_BUILD_FLAT = 46 };

/* ⚠ THE RASTER-CONFIG SLOT, for an A/B that has to happen inside one frame loop. Patch 0053 stashes the address of
 * info->pa_sc_raster_config here at device init; wsi_orbis.c writes through it between presents. Defined in this
 * file rather than in either patch because both of those are transient and this one is not - the same lesson the API
 * counters taught when defining them in a patch made the arm unlinkable without it. */
uint32_t *ac_orbis_raster_slot;
extern uint64_t orbis_idle_wait_ns;

/* ⚠ THE PRESENT'S WAIT FOR THE GPU, MEASURED AT THE CALL SITE RATHER THAN INSIDE THE CALL.
 *
 * orbis_idle_wait_ns exists two lines up and this project's own notes retract it: it reported 3.3 ms a
 * wait where a direct measurement of the SAME call gave 184 ms, with an instruction not to use that
 * line until it is fixed. The direct measurement is the one wsi_orbis.c already takes around
 * ac_orbis_wait_gpu_idle, so these are written from there and this is the pair to trust.
 *
 * Why it is worth its own name: that wait is the frame's SERIALISATION POINT. The present path waits
 * for the whole GPU to go idle, then memcpy's 8100 KiB into the scan-out on the CPU, then flips - so
 * the next frame's CPU work cannot begin until this frame's GPU work has finished. Measuring at half
 * resolution showed CPU time per frame is CONSTANT at ~44 ms while GPU time scales with pixels, and
 * 44 + GPU predicts the observed frame time at both resolutions. That is a serial model, not a
 * pipelined one, and this counter is what will confirm or kill it. */
uint64_t orbis_present_wait_ns;
uint64_t orbis_present_waits;

/* Written by radv_cmd_buffer.c on CmdBeginRendering and CmdDispatchBase; read by the budget line below.
 * See the comment at those call sites for why the count of screenfuls is the number worth having. */
/* ⚠ HOW MUCH OF THE CPU'S 60 ms A FRAME IS OURS?
 *
 * The frame budget measures the whole PROCESS - the title, RADV and this file together - and if the CPU
 * becomes the floor, attributing it by guesswork is how the last three wrong conclusions happened. This
 * is the only part anyone here can bound honestly: wall time inside ac_drm_cs_submit_raw2, end to end,
 * which that function already measures for its own booking.
 *
 * ⚠ AND IT IS A FLOOR ON OUR SHARE, NOT OUR SHARE. RADV does plenty outside this call - building command
 * streams, updating descriptors, compiling nothing at steady state - and none of that is counted here.
 * What this settles is the cheaper question: whether the SUBMISSION path is worth optimising at all. */
uint64_t orbis_submit_cpu_ns;

/* ⚠ EVERY EXIT, NOT ONE OF THEM. The first version of this counter was incremented on the
 * empty-submission path alone, which no frame ever takes, so the budget line reported "0 ms inside
 * this driver's submit path" over 22 windows - a number that looked like a finding and measured
 * nothing. The sibling orbis_api_time() call is gated on ORBIS_COUNT_API and was also off, so the
 * whole of this accounting was dead. Both go through here now. */
static inline int
orbis_submit_return(int ret, uint64_t t0)
{
   const uint64_t d = os_time_get_nano() - t0;

   orbis_api_time(ORBIS_ID_ARM_SUBMIT, d);
   p_atomic_add(&orbis_submit_cpu_ns, d);

   return ret;
}

/* The recording side of the CPU, added when the frame turned out to be CPU-bound with almost none
 * of it in the submit path. orbis_record_ns is Begin-to-End wall time and therefore an UPPER bound
 * on our share of recording: the title's own work between its vkCmd calls falls inside it too. */
/* ⚠ THE VULKAN API BOUNDARY, which is the last place the frame's CPU can be hiding.
 *
 * Submit, recording and draw counts have all come back small, so either the title owns the frame's
 * CPU or an entry point nobody timed does. These seven are timed at their boundary and reported
 * per frame; see src/util/orbis_api_probe.h for why the list is short and high-level. */
uint64_t orbis_api_slot_ns[ORBIS_API_SLOTS];
uint64_t orbis_api_slot_calls[ORBIS_API_SLOTS];

void ac_orbis_note(const char *msg);

/* A line from the application into the driver's log, which is the only stream that reaches the
 * laptop. The title has its own logging and it does not come off the console; rather than build a
 * second channel, let it borrow this one for the handful of facts worth having. */
void
ac_orbis_note(const char *msg)
{
   mesa_logi("orbis-app: %s", msg != NULL ? msg : "(null)");
}

void
ac_orbis_api_account(unsigned slot, uint64_t ns)
{
   if (slot < ORBIS_API_SLOTS) {
      p_atomic_add(&orbis_api_slot_ns[slot], ns);
      p_atomic_inc(&orbis_api_slot_calls[slot]);
   }
}

uint64_t orbis_draws;
uint64_t orbis_desc_binds;
uint64_t orbis_record_ns;
uint64_t orbis_record_spans;

uint64_t orbis_gfx_passes;
uint64_t orbis_gfx_pass_pixels;
uint64_t orbis_dispatches;
uint64_t orbis_dispatch_groups;
extern uint64_t orbis_idle_waits;

static bool
orbis_dump_hung_enabled(void)
{
   static int cached = -1;
   if (cached < 0) {
      const char *const e = getenv("ORBIS_DUMP_HUNG");
      cached = (e != NULL && e[0] == '0' && e[1] == '\0') ? 0 : 1;
   }
   return cached == 1;
}


#if !defined(__PS4__)
/* ⚠ THE HOST ARM'S HALF OF orbis_test_mirror_mapping(), AND IT EXISTS TO REFUSE OUT LOUD.
 *
 * The real ladder is inside the `#if defined(__PS4__)` region below, because every call it makes is
 * Sony's. Without this stub the whole function would simply not exist on the laptop, `--host-orbis`
 * with ORBIS_TEST_MIRROR=1 would print nothing at all, and silence would be read as "the probe found
 * nothing" rather than "the probe was not there". That distinction is the one this project keeps
 * paying for.
 *
 * A green ladder here would in any case be a statement about orbis-compat's shims and not about the
 * console's kernel, which is not the question being asked. */
static void
orbis_test_mirror_mapping(void)
{
   const char *const e = getenv("ORBIS_TEST_MIRROR");
   if (e == NULL || e[0] == '0')
      return;

   mesa_logw("orbis-drm: MIRROR ladder asked for, but this is the HOST arm - sceKernelMapDirectMemory "
             "here is orbis-compat's shim, not Sony's kernel, so any answer would be about the "
             "overlay. NOT RUN. Ask a console.");
}
#endif /* !__PS4__ */

#if defined(__PS4__)
/* ---------------------------------------------------------------- the backing-store seam
 *
 * THE ONLY THING IN THIS FILE THAT DIFFERS BETWEEN THE LAPTOP AND THE CONSOLE, and it is one screen of code
 * rather than a second implementation. Everything above and below it - the VA allocator, the handle table,
 * the ordering of alloc/map, the aliasing - is shared, so the laptop exercises the real logic and only the
 * question "does sceKernel agree?" is left for hardware. That is the distinction this project has spent the
 * most console flashes failing to make.
 *
 * __PS4__ and not MESA_SYSTEM_HAS_KMS_DRM: that macro is 0 in BOTH builds, since -Dplatforms=orbis selects
 * the arm rather than the toolchain. Getting this wrong would make the host build compile console code.
 */

/* ONE ARENA, MAPPED ONCE, AND THE VA WINDOW IS THAT ARENA. This is not the design the host arm has, and the
 * difference is deliberate.
 *
 * The obvious translation is per-BO: sceKernelAllocateDirectMemory in bo_alloc, sceKernelMapDirectMemory at
 * RADV's chosen address in va_op_raw. Two things argue against it for a FIRST flash. Whether this kernel
 * honours a requested address at all is unverified - the flags argument the shipped sample passes is 0 - and
 * this fork has already hung a console once by assuming a mapping call behaved like Linux's (PS4 mmap
 * populates eagerly). Meanwhile the arena shape below is the one GnmAllocator uses, the one the only
 * shipped OpenOrbis sample uses, and the one proven on this hardware.
 *
 * So: reserve and map one arena up front, REPORT ITS ADDRESS RANGE as the VA window, and let the VA allocator
 * hand out addresses inside it. Then bo_alloc has no kernel call to make and va_op_raw has nothing to map -
 * the pages are already there, at the address RADV was given. Which is the platform's own truth: one address
 * space, one mapping, both processors.
 *
 * WHAT THIS COSTS, SAID BEFORE IT IS DISCOVERED:
 *   - ONE BUS. Everything is WB_ONION, so every surface is cached and coherent rather than write-combined.
 *     Correct, and slower than GARLIC would be for GPU-read data. Bus selection is a later step, and doing it
 *     here would mean two disjoint windows where drm_amdgpu_info_device has room for one.
 *   - NO ALIASING. RADEON_FLAG_VM_PAD_1PAGE maps a BO's first page a second time after the buffer; a flat
 *     arena cannot, so that guard page reads its neighbour instead. It is a read-only overrun absorber, so
 *     the consequence is a wrong VALUE in a place nothing should read, not a fault.
 *   - A FIXED CEILING. Address-space exhaustion is a loud message from the VA allocator rather than a growth
 *     attempt, which is the right failure for a first flash.
 */
#define ORBIS_BACKING "direct memory (one ONION arena)"

/* 2 MiB, which is what the shipped sample passes as both the round-up and the alignment argument
 * (OO graphics.cpp:116-119, and gnmallocator.cpp's DirectMemAlign says the true minimum is unknown). */
#define ORBIS_DIRECT_ALIGN 0x200000ull
/* CPU_RW | GPU_RW = 0x03 | 0x30, exactly what the sample passes (OO kernel.h:113-121). */
#define ORBIS_GRAPHICS_PROT 0x33
/* No access at all, for a range that is reserved but must not be readable - the state amdgpu leaves an unmapped VA
 * range in. A GPU read of such a range faults, which is the whole point. */
#define ORBIS_PROT_NONE 0x00
/* ⚠ A LADDER, NOT A CONSTANT, AND A GAME ENGINE IS WHY. 256 MiB was chosen when the only client was a probe:
 * enough for RADV's shader arenas, its rings and a few buffers, and small enough that a failure to reserve it
 * said something about the console rather than about the request.
 *
 * Then Tempest's own Vulkan backend ran here and hit it in six calls. Its device allocator carves pages of
 * DEFAULT_PAGE_SIZE = 128 MiB (Tempest Engine/gapi/deviceallocator.h:17), so an offscreen clear-and-read-back
 * takes one page for the attachment and a second for the read-back staging buffer: 256 MiB against an arena of
 * 256 MiB minus the arm's private slice. Measured on hardware - every stage up to readPixels passed and
 * readPixels returned VK_ERROR_OUT_OF_DEVICE_MEMORY for a SIXTEEN KILOBYTE buffer, because the page behind it
 * did not fit. An engine that pages its heap will always be a multiple of its page size away from the ceiling,
 * so the ceiling has to be well clear of it rather than exactly at it.
 *
 * DESCENDING, AND MEASURED RATHER THAN ASSUMED, because how much direct memory this console will hand a
 * homebrew process is not in any oracle here - and asking for too much is not free: sceKernelMapDirectMemory
 * commits eagerly on this platform, and an over-large request has frozen this console before. So the arm asks
 * for the largest of these that the kernel actually gives, and logs which one it got. ORBIS_ARENA_MIB overrides
 * the whole ladder for an experiment that wants a specific number. */
static const uint64_t orbis_arena_ladder[] = {
   1024ull * 1024 * 1024,
   512ull * 1024 * 1024,
   256ull * 1024 * 1024,
   128ull * 1024 * 1024,
};

/* The refusals rung 5 and rung 6 collect are only worth anything if the reader can tell EACCES from
 * EINVAL without a header open beside the log: the first says the kernel understood the request and
 * declined it on policy, the second says the request was malformed and the policy was never reached.
 * Rung 5a returned 0x8002000d and that distinction was the entire content of the result.
 * Values from orbis/_types/errors.h; anything not listed is printed as its raw code. */
static const char *
orbis_kernel_error_name(int32_t rc)
{
   switch ((uint32_t)rc) {
   case 0x80020001u: return "EPERM - not permitted";
   case 0x80020008u: return "ENOEXEC - not an executable object";
   case 0x80020009u: return "EBADF - bad file descriptor, so the flags word was not anonymous";
   case 0x8002000Cu: return "ENOMEM - out of memory or address space";
   case 0x8002000Du: return "EACCES - understood and refused on policy";
   case 0x8002000Eu: return "EFAULT - bad address argument";
   case 0x80020016u: return "EINVAL - malformed request, policy never reached";
   case 0x8002002Du: return "EOPNOTSUPP - not supported on this object";
   default:          return "not in this file's table";
   }
}

/* ⚠ CAN THE SAME PHYSICAL DIRECT MEMORY BE MAPPED AT TWO LIVE VIRTUAL ADDRESSES AT ONCE?
 * ORBIS_TEST_MIRROR=1, and nothing else in this driver depends on the answer.
 *
 * WHY IT IS ASKED HERE. RetroArch's Beetle PSX HW runs the MIPS INTERPRETER on this console because
 * Lightrec, its dynamic recompiler, wants the emulated machine's RAM visible at several addresses at
 * once - which on Linux it builds with memfd_create/MAP_SHM. `RetroArch/ps4/HANDOFF.md` records that
 * as the wall: "neither exists here". That is true of POSIX shared memory and says nothing about the
 * platform's own direct-memory API, which this file uses on every run:
 *
 *     sceKernelAllocateDirectMemory(..., &phys)          physical pages
 *     sceKernelMapDirectMemory(&va, len, prot, flags, phys, align)
 *
 * Nothing in that shape forbids calling the map a second time with the SAME phys. If it is allowed
 * and the two views are coherent, the wall is not a wall, and the measured cost of the interpreter -
 * one CPU core saturated for a frame in which this driver spends 0.6% and the GPU waits 0 ms - has a
 * route out. If it is refused, the refusal is on the record with a return code instead of an
 * assumption, and nobody spends a day rediscovering it.
 *
 * ⚠ THE RUNGS ARE ORDERED SO THAT EACH ONE IS MEANINGLESS WITHOUT THE ONE BEFORE, and each prints
 * what it measured rather than a verdict. A ladder whose control rung is skipped proves nothing:
 * "the second mapping worked" is not a fact about mirrors if the first mapping never held data.
 *
 *   rung 0   allocate, map once, write a pattern, read it back through THAT SAME address.
 *            The harness itself. A failure here means the rest of the run says nothing.
 *   rung 1   map the same phys AGAIN. Return code and both addresses, and whether they differ -
 *            a kernel that hands back the same VA has not made a mirror.
 *   rung 2   coherence, BOTH WAYS. Write through the first view, read through the second; then
 *            write a different word through the second and read it through the first. One
 *            direction is not enough: a copy-on-map would pass the first and fail the second.
 *   rung 3   how MANY. Lightrec wants about four. Keep mapping until the kernel refuses and say
 *            the number, because "more than one" and "as many as you like" are different answers.
 *   rung 4   independence: unmap one view and check the others still read the pattern. If tearing
 *            one down poisons the rest, the mirrors are not usable however many there were.
 *
 * ⚠ AND IT SAYS WHERE IT RAN. Under --host-orbis this file is built for Linux against the overlay's
 * shims, and those shims are not Sony's kernel. A green ladder there would be a statement about
 * orbis-compat, which is not the question. __ORBIS__ is defined only by the cross build.
 */
static void
orbis_test_mirror_mapping(void)
{
   const char *const e = getenv("ORBIS_TEST_MIRROR");
   if (e == NULL || e[0] == '0')
      return;

   /* One page-aligned 2 MiB block: the platform's own direct-memory granule, and small enough that
    * taking it changes nothing for the title that is about to start. */
   const size_t len = (size_t)ORBIS_DIRECT_ALIGN;
   /* CPU read/write only. The GPU has no part in this question, and asking for GPU rights as well
    * would make a refusal ambiguous between "no mirrors" and "not that protection". */
   const int32_t prot = 0x03;
   enum { ORBIS_MIRROR_MAX = 8 };
   void *view[ORBIS_MIRROR_MAX] = {0};
   unsigned views = 0;
   off_t phys = 0;

   mesa_logi("orbis-drm: MIRROR ladder - %llu KiB of direct memory, prot 0x%02x, align %llu KiB",
             (unsigned long long)(len / 1024), (unsigned)prot,
             (unsigned long long)(ORBIS_DIRECT_ALIGN / 1024));

   int32_t err = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), len,
                                               ORBIS_DIRECT_ALIGN, ORBIS_KERNEL_WB_ONION, &phys);
   if (err != 0) {
      mesa_loge("orbis-drm: MIRROR rung 0 FAILED - sceKernelAllocateDirectMemory -> 0x%08x. "
                "Nothing below this ran; the ladder says nothing about mirrors.", (unsigned)err);
      return;
   }
   mesa_logi("orbis-drm: MIRROR rung 0: phys 0x%llx", (unsigned long long)phys);

   err = sceKernelMapDirectMemory(&view[0], len, prot, 0, phys, ORBIS_DIRECT_ALIGN);
   if (err != 0) {
      mesa_loge("orbis-drm: MIRROR rung 0 FAILED - sceKernelMapDirectMemory -> 0x%08x. The phys was "
                "allocated and could not be mapped even once.", (unsigned)err);
      sceKernelReleaseDirectMemory(phys, len);
      return;
   }
   views = 1;

   /* Two words, far apart, so that a mapping which only shares its first page still fails rung 2. */
   const size_t far = len - orbis_page_size();
   volatile uint32_t *const a0 = (volatile uint32_t *)view[0];
   volatile uint32_t *const a1 = (volatile uint32_t *)((char *)view[0] + far);

   a0[0] = 0xa5c0ffeeu;
   a1[0] = 0x0badf00du;
   if (a0[0] != 0xa5c0ffeeu || a1[0] != 0x0badf00du) {
      mesa_loge("orbis-drm: MIRROR rung 0 FAILED - wrote 0xa5c0ffee/0x0badf00d at %p and %p, read "
                "back 0x%08x/0x%08x. The mapping does not hold data; nothing below is meaningful.",
                (void *)a0, (void *)a1, a0[0], a1[0]);
      goto done;
   }
   mesa_logi("orbis-drm: MIRROR rung 0 OK - view 0 at %p, %llu KiB, holds what was written to it "
             "at offset 0 and at offset %llu KiB",
             view[0], (unsigned long long)(len / 1024), (unsigned long long)(far / 1024));

   /* rung 1 - the actual question. */
   err = sceKernelMapDirectMemory(&view[1], len, prot, 0, phys, ORBIS_DIRECT_ALIGN);
   if (err != 0) {
      mesa_logw("orbis-drm: MIRROR rung 1: the SECOND mapping of phys 0x%llx was REFUSED -> 0x%08x. "
                "One physical range, one virtual view. A dynamic recompiler that needs mirrored RAM "
                "cannot get it this way, and that is now measured rather than assumed.",
                (unsigned long long)phys, (unsigned)err);
      goto done;
   }
   views = 2;
   if (view[1] == view[0]) {
      mesa_logw("orbis-drm: MIRROR rung 1: the second mapping SUCCEEDED but landed at the SAME "
                "address %p. That is not a mirror - the kernel returned the existing view.",
                view[1]);
      goto done;
   }
   mesa_logi("orbis-drm: MIRROR rung 1 OK - view 1 at %p, %lld KiB from view 0 at %p",
             view[1], (long long)(((char *)view[1] - (char *)view[0]) / 1024), view[0]);

   /* rung 2 - coherence, both ways. */
   {
      volatile uint32_t *const b0 = (volatile uint32_t *)view[1];
      volatile uint32_t *const b1 = (volatile uint32_t *)((char *)view[1] + far);

      const uint32_t saw0 = b0[0], saw1 = b1[0];
      if (saw0 != 0xa5c0ffeeu || saw1 != 0x0badf00du) {
         mesa_logw("orbis-drm: MIRROR rung 2 FAILED forwards - view 0 holds 0xa5c0ffee/0x0badf00d, "
                   "view 1 reads 0x%08x/0x%08x. Two mappings of one phys that do not share their "
                   "contents; this is a copy, not a mirror.", saw0, saw1);
         goto done;
      }

      b0[0] = 0x1eaf1eafu;
      b1[0] = 0xdeadbeefu;
      if (a0[0] != 0x1eaf1eafu || a1[0] != 0xdeadbeefu) {
         mesa_logw("orbis-drm: MIRROR rung 2 FAILED backwards - wrote 0x1eaf1eaf/0xdeadbeef through "
                   "view 1, view 0 reads 0x%08x/0x%08x. Forwards worked and backwards did not, which "
                   "is what a copy-on-map looks like and is why this rung is two-sided.",
                   a0[0], a1[0]);
         goto done;
      }
      mesa_logi("orbis-drm: MIRROR rung 2 OK - both directions, at offset 0 and at offset %llu KiB. "
                "The two views are the same memory.", (unsigned long long)(far / 1024));
   }

   /* rung 3 - how many. */
   while (views < ORBIS_MIRROR_MAX) {
      err = sceKernelMapDirectMemory(&view[views], len, prot, 0, phys, ORBIS_DIRECT_ALIGN);
      if (err != 0) {
         mesa_logi("orbis-drm: MIRROR rung 3: mapping %u was refused -> 0x%08x", views, (unsigned)err);
         break;
      }
      views++;
   }
   mesa_logi("orbis-drm: MIRROR rung 3: %u simultaneous view(s) of one phys%s (the ladder stops "
             "asking at %u; Lightrec wants about four)",
             views, views >= ORBIS_MIRROR_MAX ? ", and it did not refuse" : "", ORBIS_MIRROR_MAX);

   /* rung 4 - does tearing one down poison the others. */
   if (views >= 2) {
      void *const dropped = view[views - 1];
      err = sceKernelMunmap(dropped, len);
      if (err != 0) {
         mesa_logw("orbis-drm: MIRROR rung 4: sceKernelMunmap(%p) -> 0x%08x - could not drop one "
                   "view, so independence is untested", dropped, (unsigned)err);
      } else {
         views--;
         if (a0[0] != 0x1eaf1eafu || a1[0] != 0xdeadbeefu) {
            mesa_logw("orbis-drm: MIRROR rung 4 FAILED - after unmapping %p the surviving view reads "
                      "0x%08x/0x%08x instead of 0x1eaf1eaf/0xdeadbeef. Dropping one mirror destroys "
                      "the rest, so they cannot be managed independently.", dropped, a0[0], a1[0]);
            goto done;
         }
         mesa_logi("orbis-drm: MIRROR rung 4 OK - unmapped %p, the remaining %u view(s) still hold "
                   "the pattern", dropped, views);
      }
   }


   /* ⚠ RUNG 5 - THE CODE BUFFER, AND IT IS THE ONE THAT CAN KILL THE PROCESS.
    *
    * Everything above asked whether the emulated machine's RAM can be mirrored. A recompiler also
    * needs somewhere to put the instructions it writes and then RUN them, and rungs 0-4 only ever
    * asked for prot 0x03 - CPU read and write. This asks for 0x05, read and EXECUTE, on the same
    * phys, which is the W^X shape a JIT actually wants: write through the writable view, execute
    * through the executable one, never both rights on one mapping.
    *
    * It is split into three because they fail differently and only the last one is dangerous:
    *
    *   5a  does the kernel GRANT the mapping at all. A return code, and nothing runs.
    *   5b  read the instruction bytes back THROUGH the executable view. Free, non-fatal, and it
    *       separates "the mirror carries the code" from "the page will execute" - two claims that a
    *       single jump would answer together and therefore answer badly.
    *   5c  CALL it. ⚠ If the page is not really executable this does not return an error, it takes
    *       the process down. So it is behind its own value: ORBIS_TEST_MIRROR=exec. With =1 the
    *       ladder stops after 5b and says what to set.
    *
    * ⚠ AND 5a SUCCEEDING IS NOT AN ANSWER. This console's kernel can accept a protection and not
    * honour it - the same lesson the GB_ADDR_CONFIG and tessellation-register work paid for, where
    * a call returned zero and the hardware disagreed. Only 5c is evidence about execution, which is
    * exactly why it is the one that costs something.
    *
    * The stub is six bytes of x86-64 and returns a value nothing else in this process produces:
    *   b8 ee ff c0 00   mov eax, 0x00c0ffee
    *   c3               ret
    * Self-modifying code needs no cache maintenance on x86-64, so a wrong answer here is about
    * mapping rather than about coherency. */
   {
      static const uint8_t stub[] = {0xb8, 0xee, 0xff, 0xc0, 0x00, 0xc3};
      const int32_t prot_rx = 0x01 | 0x04; /* CPU read | CPU execute; VM_PROT_EXECUTE is 0x04 */
      void *xview = NULL;

      const int32_t xerr =
         sceKernelMapDirectMemory(&xview, len, prot_rx, 0, phys, ORBIS_DIRECT_ALIGN);
      if (xerr != 0) {
         mesa_logw("orbis-drm: MIRROR rung 5a: a READ|EXECUTE view of phys 0x%llx was REFUSED -> "
                   "0x%08x. Direct memory cannot hold a recompiler's code buffer this way. The "
                   "remaining route is sceKernelJitCreateSharedMemory and its alias call, which the "
                   "SDK declares as `void f();` - no argument shapes, so that is reverse "
                   "engineering rather than another rung.",
                   (unsigned long long)phys, (unsigned)xerr);
         goto rung5_done;
      }
      mesa_logi("orbis-drm: MIRROR rung 5a: READ|EXECUTE view granted at %p (prot 0x%02x asked). "
                "⚠ A granted protection is not an honoured one - 5c is what tests that.",
                xview, (unsigned)prot_rx);

      /* Write the stub through the WRITABLE view. This is the whole point of the W^X shape: the
       * mapping being written is not the mapping being executed. */
      memcpy(view[0], stub, sizeof(stub));

      const uint8_t *const seen = (const uint8_t *)xview;
      if (memcmp(seen, stub, sizeof(stub)) != 0) {
         mesa_logw("orbis-drm: MIRROR rung 5b FAILED - wrote %02x %02x %02x %02x %02x %02x through "
                   "the writable view, the executable view reads %02x %02x %02x %02x %02x %02x. The "
                   "code never arrives, so whether the page would execute does not matter.",
                   stub[0], stub[1], stub[2], stub[3], stub[4], stub[5],
                   seen[0], seen[1], seen[2], seen[3], seen[4], seen[5]);
         sceKernelMunmap(xview, len);
         goto rung5_done;
      }
      mesa_logi("orbis-drm: MIRROR rung 5b OK - the six instruction bytes written through %p are "
                "readable through %p. The mirror carries code as well as data.", view[0], xview);

      if (e[0] != 'e') {
         mesa_logi("orbis-drm: MIRROR rung 5c NOT RUN - calling into %p is the only test of whether "
                   "the page really executes, and a page that does not takes this process down "
                   "rather than returning an error. Set ORBIS_TEST_MIRROR=exec and run again; "
                   "nothing needs rebuilding or reinstalling, only that one line in the env file.",
                   xview);
         sceKernelMunmap(xview, len);
         goto rung5_done;
      }

      /* ⚠ SAY IT BEFORE JUMPING. If the next line is the last thing in the log, the answer is that
       * the page did not execute - and that is a result rather than a lost run, provided this line
       * reached the file first. */
      mesa_loge("orbis-drm: MIRROR rung 5c: CALLING %p NOW. If this is the last MIRROR line in the "
                "log, the mapping was granted READ|EXECUTE and did not execute, and the process died "
                "on the jump. That is the answer, not a crash to be investigated.", xview);

      uint32_t (*fn)(void);
      memcpy(&fn, &xview, sizeof(fn)); /* ⚠ not a cast: object pointer to function pointer is not one */
      const uint32_t got = fn();

      if (got != 0x00c0ffeeu)
         mesa_logw("orbis-drm: MIRROR rung 5c: the call RETURNED, which means the page executes - "
                   "but it produced 0x%08x instead of 0x00c0ffee. The page is executable and "
                   "something about the bytes or the calling convention is wrong.", got);
      else
         mesa_logi("orbis-drm: MIRROR rung 5c OK - code written through a writable mirror and CALLED "
                   "through an executable one returned 0x00c0ffee. W^X works on direct memory, and "
                   "every mechanism a MIPS recompiler needs from this platform is now measured.");

      sceKernelMunmap(xview, len);
   }
rung5_done:

   /* ⚠ RUNG 6 - CAN THIS PROCESS GET EXECUTABLE PAGES BY ANY ROUTE AT ALL?
    *
    * Rung 5a settled that DIRECT memory will not carry them: EACCES, not EINVAL, so the call was
    * well formed and the kernel said no on policy. That is one route, and the two requirements it
    * was standing in for are separable:
    *
    *   the emulated machine's RAM    must be MIRRORED     - rungs 1-4, and it works
    *   the recompiler's code buffer  must be EXECUTABLE   - and it need NOT be mirrored
    *
    * So the question that is left is smaller than the one rung 5 asked, and this process is not
    * categorically barred from executing memory: its own .text runs. Three routes, none of them
    * direct memory, each with its own policy check:
    *
    *   6a  sceKernelMprotect adding EXEC to a range that is ALREADY mapped read-write. A different
    *       syscall from the one that refused, and this driver already establishes elsewhere that
    *       mprotect on direct memory reaches even the GPU's page tables - so the call works there
    *       for other bits, and only this bit is in question.
    *   6b  sceKernelMapFlexibleMemory. A different pool entirely, with its own accounting and
    *       plausibly its own policy. This is the pool musl's malloc grows into.
    *   6c  sceKernelMmap with MAP_ANON. The plainest form there is.
    *
    * Each is tried at RWX first and at RX second, and BOTH codes are logged. A route that refuses
    * RWX and grants RX is a W^X platform and a perfectly good answer; one that refuses both is a
    * closed door. Collapsing them into one attempt would turn the first case into the second.
    *
    * ⚠ NOTHING HERE EXECUTES. A granted protection is not an honoured one - rung 5's split made
    * that point and it holds here too - so rung 6 collects return codes and addresses only, and
    * names which route to point rung 5c at. Turning the jump on stays a deliberate second run. */
   {
      static const struct {
         const char *name;
         unsigned    which;
      } routes[] = {
         {"6a sceKernelMprotect on an already-mapped RW direct range", 0},
         {"6b sceKernelMapFlexibleMemory", 1},
         {"6c sceKernelMmap MAP_ANON|MAP_PRIVATE", 2},
      };
      /* RWX first, then RX. 0x01 read, 0x02 write, 0x04 execute - VM_PROT_* in the SDK's
       * _types/kernel.h, and the same bits ORBIS_GRAPHICS_PROT's low nibble uses. */
      static const int32_t want[2] = {0x07, 0x05};
      mesa_logi("orbis-drm: MIRROR rung 6c will pass flags 0x%04x for MAP_PRIVATE|MAP_ANON "
                "(FreeBSD wants 0x1002; the SDK's musl header would give 0x0022)",
                (unsigned)(MAP_PRIVATE | MAP_ANON));

      const char *granted_by = NULL;
      void       *granted_at = NULL;
      int32_t     granted_prot = 0;
      unsigned    granted_which = 0;

      for (unsigned r = 0; r < ARRAY_SIZE(routes); r++) {
         for (unsigned p = 0; p < 2; p++) {
            void   *at = NULL;
            int32_t rc = 0;

            switch (routes[r].which) {
            case 0:
               /* view[0] is still mapped read-write from rung 0. Ask for the bit to be added in
                * place, which is what a JIT that allocates first and protects later would do. */
               at = view[0];
               rc = sceKernelMprotect(view[0], len, want[p]);
               break;
            case 1:
               rc = sceKernelMapFlexibleMemory(&at, len, want[p], 0);
               break;
            default:
               /* ⚠ THE SYMBOLS, NOT THE NUMBERS. The first version of this rung passed 0x20 for
                * MAP_ANON, read out of the SDK's own sys/mman.h - which is musl's header and carries
                * LINUX's value. This kernel is FreeBSD-derived and wants 0x1000, so the flags word
                * was not anonymous, the kernel went on to validate fd = -1, and BOTH protections
                * came back EBADF. Identical codes for two different protections is the signature of
                * a call that failed before it ever looked at prot: it read as "executable pages are
                * refused" and was nothing of the kind.
                *
                * orbis-compat/src/orbis_mmap.cpp:53 carries a static_assert that
                * MAP_PRIVATE|MAP_ANON == 0x1002 for exactly this reason. The value is logged below
                * so the next reader does not have to trust that this one got it right. */
               rc = sceKernelMmap(NULL, len, want[p], MAP_PRIVATE | MAP_ANON, -1, 0, &at);
               break;
            }

            if (rc != 0) {
               mesa_logi("orbis-drm: MIRROR rung %s at prot 0x%02x -> 0x%08x (%s)", routes[r].name,
                         (unsigned)want[p], (unsigned)rc, orbis_kernel_error_name(rc));
               continue;
            }

            mesa_logi("orbis-drm: MIRROR rung %s at prot 0x%02x -> GRANTED at %p", routes[r].name,
                      (unsigned)want[p], at);
            if (granted_by == NULL) {
               granted_by = routes[r].name;
               granted_at = at;
               granted_prot = want[p];
               granted_which = routes[r].which;
            } else if (routes[r].which != 0) {
               /* Not the route rung 7 will use, so give the pages straight back. Leaving a flexible
                * or anonymous mapping behind would be a leak in a function that exists to measure. */
               sceKernelMunmap(at, len);
            }
            break; /* this route answered; do not also ask it for the weaker protection */
         }
      }

      if (granted_by == NULL)
         mesa_logw("orbis-drm: MIRROR rung 6: NO route gave this process an executable page - "
                   "direct memory (5a), mprotect, flexible memory and anonymous mmap all refused. "
                   "What is left is sceKernelJitCreateSharedMemory and its alias call, which the "
                   "SDK declares as `void f();` with no argument shapes at all. That is reverse "
                   "engineering of the kind sceGnmSetHsShader has already cost days on, not another "
                   "rung. A recompiler on this console goes through that door or not at all.");
      else
         mesa_logi("orbis-drm: MIRROR rung 6: %s granted prot 0x%02x at %p. ⚠ GRANTED IS NOT "
                   "HONOURED - nothing in rung 6 executed. Rung 7 is what tests that.",
                   granted_by, (unsigned)granted_prot, granted_at);

      /* ⚠ RUNG 7 - RUN SOMETHING OUT OF THE PAGE RUNG 6 WAS GIVEN.
       *
       * Rung 5a established that direct memory cannot be MAPPED executable, and rung 6 that the same
       * pages can be MPROTECTED to it afterwards - a policy applied at map time and not at protect
       * time. That asymmetry is interesting and it is still only a return code. The platform has
       * answered "yes" to a question about permissions twice now and never once to a question about
       * execution, and those are not the same question: this console's kernel has form for accepting
       * a value and not honouring it.
       *
       * Same six bytes as rung 5, same gate. ORBIS_TEST_MIRROR=1 stops after the read-back;
       * =exec calls it and accepts that a page which is not really executable takes the process
       * down rather than returning an error. */
      if (granted_by != NULL) {
         static const uint8_t stub7[] = {0xb8, 0xee, 0xff, 0xc0, 0x00, 0xc3};
         bool writable = (granted_prot & 0x02) != 0;

         /* If the grant was read-execute only, put the write rights back for as long as it takes to
          * place the stub and then take them away again. That IS the W^X dance a recompiler does,
          * so doing it here measures the thing rather than working around it. Only route 6a can:
          * the other two would need their own mprotect and that is a different question. */
         if (!writable && granted_which == 0) {
            if (sceKernelMprotect(granted_at, len, 0x03) == 0) {
               memcpy(granted_at, stub7, sizeof(stub7));
               const int32_t back = sceKernelMprotect(granted_at, len, granted_prot);
               writable = (back == 0);
               mesa_logi("orbis-drm: MIRROR rung 7: W^X dance - RW to write the stub, then back to "
                         "0x%02x -> 0x%08x (%s)", (unsigned)granted_prot, (unsigned)back,
                         orbis_kernel_error_name(back));
            }
         } else if (writable) {
            memcpy(granted_at, stub7, sizeof(stub7));
         }

         if (!writable) {
            mesa_logw("orbis-drm: MIRROR rung 7 NOT RUN - the granted page could not be made "
                      "writable long enough to put six bytes in it, so there is nothing to call.");
         } else if (memcmp(granted_at, stub7, sizeof(stub7)) != 0) {
            const uint8_t *const b = (const uint8_t *)granted_at;
            mesa_logw("orbis-drm: MIRROR rung 7 FAILED - the six bytes did not stay: %02x %02x %02x "
                      "%02x %02x %02x", b[0], b[1], b[2], b[3], b[4], b[5]);
         } else if (e[0] != 'e') {
            mesa_logi("orbis-drm: MIRROR rung 7 READY at %p and NOT CALLED - calling it is the only "
                      "test of whether the page really executes, and a page that does not takes this "
                      "process down rather than returning an error. Set ORBIS_TEST_MIRROR=exec and "
                      "run again; nothing needs rebuilding or reinstalling, only that one line.",
                      granted_at);
         } else {
            /* ⚠ SAY IT BEFORE JUMPING. logger_file() fflushes every message, so this line is on
             * disk before the call. If it is the last MIRROR line in the log, the answer is that the
             * page did not execute - a result, not a crash to be investigated. */
            mesa_loge("orbis-drm: MIRROR rung 7: CALLING %p NOW, from %s at prot 0x%02x. If this is "
                      "the last MIRROR line, that route grants execute and does not honour it.",
                      granted_at, granted_by, (unsigned)granted_prot);

            uint32_t (*fn7)(void);
            memcpy(&fn7, &granted_at, sizeof(fn7));
            const uint32_t got7 = fn7();

            if (got7 != 0x00c0ffeeu)
               mesa_logw("orbis-drm: MIRROR rung 7: the call RETURNED - the page executes - but "
                         "produced 0x%08x instead of 0x00c0ffee.", got7);
            else
               mesa_logi("orbis-drm: MIRROR rung 7 OK - code placed in a page from %s and CALLED "
                         "returned 0x00c0ffee. This process can execute memory it wrote. Together "
                         "with the mirrors of rungs 1-4, every mechanism a MIPS recompiler needs "
                         "from this platform is now measured rather than assumed.",
                         granted_by);
         }

         if (granted_which != 0)
            sceKernelMunmap(granted_at, len);
      }
   }

   mesa_logi("orbis-drm: MIRROR: mirrored direct memory WORKS on this console - %u coherent views of "
             "one physical range, independently unmappable. This is the mechanism Lightrec needs and "
             "the reason it was skipped (memfd_create/MAP_SHM absent) does not apply to it. The "
             "remaining question about PLACEMENT is answered elsewhere in this file: "
             "ORBIS_MAP_FIXED puts a direct-memory mapping at an address of our choosing, on "
             "hardware, every time a BO moves to GARLIC. Rung 5 above is the code buffer.",
             views);

done:
   for (unsigned i = 0; i < views; i++)
      if (view[i] != NULL)
         sceKernelMunmap(view[i], len);
   sceKernelReleaseDirectMemory(phys, len);
   mesa_logi("orbis-drm: MIRROR ladder done - %u view(s) unmapped, phys 0x%llx released",
             views, (unsigned long long)phys);
}


/* THE ARM'S OWN SLICE, TAKEN OFF THE FRONT AND NOT REPORTED. The VA window RADV is told about starts AFTER
 * it, so nothing RADV allocates can land on the arm's fence label or its command buffer - which would be a
 * corruption whose symptom is a fence that never signals. One page for the labels, one for the PM4. */
/* ⚠ A RING OF FENCE COMMAND BUFFERS, NOT ONE, and a console hang is why. The GPU reads a submitted command
 * buffer ASYNCHRONOUSLY, long after the submit call returned and the lock was dropped - so a single shared
 * buffer means submit N+1 rewrites the packets submit N's work has not consumed yet. Measured: submits #1 and
 * #2 returned in 0 us, #3 blocked inside sceGnmSubmitCommandBuffers and never came back, because the CP had
 * been handed a packet that changed under it and the ring filled behind the stall.
 *
 * One slot per outstanding submit, indexed by sequence number, and a slot is only reused once the label proves
 * the GPU has passed the submission that used it. 16 is far more than anything measured (RADV's own limit here
 * is one submission per queue at a time), and running out is a bounded wait rather than corruption. */
#define ORBIS_FENCE_CB_SLOTS 16
/* ⚠ 256 BYTES BECAUSE THAT IS WHAT THIS DEVICE ITSELF REPORTS, and ignoring our own answer stalled the GPU.
 * The console's radeon_info dump reads `IP GFX 7.2 queues:1 align:256 pad_dw:0x7`: an IB starts on a 256-byte
 * boundary and its size is a multiple of 8 dwords. The two EOP packets are 12 dwords - not a multiple of 8 -
 * and the slots were 64 bytes apart, so every fence command buffer violated both.
 *
 * It cost a hang that looked like a queue-depth limit: submit #1's end-of-pipe packet executed and moved the
 * label to 1, submit #2's did NOT (the label stayed 1 across its return), and the third submit then blocked
 * inside sceGnmSubmitCommandBuffers with the ring full behind a CP that had stopped parsing. The first one
 * surviving is what made this look like anything else. */
/* ⚠ ONE DCB ENTRY, AND THE IBs ARE COPIED INTO IT. sceGnmSubmitCommandBuffers takes arrays, so four entries
 * looked like the natural translation of RADV's three IB chunks plus our fence - and it is UNPROVEN on this
 * hardware. The Tempest fork, which is the only thing known to submit successfully here, passes count = 1
 * always (gnmdevice.cpp:530) and builds one flat command buffer with no IT_INDIRECT_BUFFER anywhere.
 *
 * What the console said, across three flashes: submit #1's end-of-pipe packet executed and moved the label to
 * 1, submit #2's did NOT, and the third submit then blocked inside sceGnmSubmitCommandBuffers. Neither the
 * shared fence buffer nor its alignment explained it - both were real bugs, both fixed, same result. Four
 * entries working ONCE is consistent with the CP running past an entry boundary because the pieces happened to
 * be adjacent, and not with four entries being a supported shape.
 *
 * Concatenation is the exact semantic anyway: RADV's three chunks are the preamble, the body and the flush
 * postamble OF ONE SUBMISSION, which amdgpu's kernel runs in order. The cost is a memcpy per submit, which is
 * a known follow-up rather than a hidden one - and the correctness question comes first. */

/* ⚠ THE PRIVATE SLICE IS 32 MiB, NOT THE HANDFUL OF PAGES THESE COMMENTS USED TO CLAIM. Two of them described
 * an earlier design - "64 KiB per slot", and "16 slots of 64 bytes is 1 KiB, comfortably inside" the second
 * page - while ORBIS_FENCE_CB_BYTES has been 2 MiB. Sixteen of those is 32 MiB, and this is the one arithmetic
 * in the file that decides where RADV's window starts: believe the stale figure and the window overlaps the
 * arm's own command buffers, whose symptom is a fence that never signals. */

/* One page for the labels, then the ring of staging command buffers - 16 x 2 MiB. */
#define ORBIS_ARENA_PRIVATE (16384ull + ORBIS_FENCE_CB_SLOTS * ORBIS_FENCE_CB_BYTES)

/* ASK THE KERNEL WHAT IS AT AN ADDRESS, instead of touching it to find out. A touch that faults is fatal and
 * takes the answer with it; sceKernelVirtualQuery returns one.
 *
 * ⚠ THE SDK'S LAYOUT FOR THIS STRUCT IS NOT TRUSTED. OpenOrbis names the first two members unk01/unk02 and
 * this project has already been bitten by one of its structs being wrong in a way nothing detects

 * (struct stat reports st_blocks where st_size belongs). So the raw words are logged
 * alongside the interpretation - if the two disagree, the raw ones are the evidence. */
static void
orbis_virtual_query(uint64_t addr, const char *what)
{
   OrbisKernelVirtualQueryInfo info;
   memset(&info, 0, sizeof(info));

   const int32_t err = sceKernelVirtualQuery((const void *)(uintptr_t)addr, 0, &info, sizeof(info));
   if (err != 0) {
      mesa_logw("orbis-drm: vq %s 0x%" PRIx64 ": NOTHING MAPPED (0x%08x)", what, addr, err);
      return;
   }

   const uint64_t *raw = (const uint64_t *)&info;
   mesa_logi("orbis-drm: vq %s 0x%" PRIx64 ": start 0x%" PRIx64 " end 0x%" PRIx64 " off 0x%" PRIx64
             " direct %u committed %u flexible %u pooled %u name '%.31s'",
             what, addr, (uint64_t)(uintptr_t)info.unk01, (uint64_t)(uintptr_t)info.unk02,
             (uint64_t)info.offset, info.isDirectMemory, info.isCommitted, info.isFlexibleMemory,
             info.isPooledMemory, info.name);
   mesa_logi("orbis-drm: vq %s raw: %016" PRIx64 " %016" PRIx64 " %016" PRIx64 " %016" PRIx64, what, raw[0],
             raw[1], raw[2], raw[3]);
}



/* ⚠ ASK SONY'S OWN DRIVER WHERE IT THINKS THE TESSELLATION FACTOR RING IS.
 *
 * 0xff0000000 has driven this whole hunt and this port has never once obtained it first-hand. It comes
 * from the old tree's notes, which quote sceGnmGetTheTessellationFactorRingBufferBaseAddress() returning
 * it. Every conclusion built on "Sony left that base in a register" therefore rests on a number nobody in
 * this tree has asked for. The register route cannot check it - all three tessellation registers failed a
 * write-then-read round trip in the same packet stream, so their zeros are non-answers - but the SDK
 * exports the query itself.
 *
 * ⚠ THE SIGNATURE IS UNKNOWN AND THAT IS HANDLED RATHER THAN GUESSED. OpenOrbis declares every one of
 * these as `void f()`, which is a linkable symbol and no information at all. Sony's real form is either a
 * value returned or an out-parameter written. Calling it one way and being wrong the other way would mean
 * writing through whatever happened to be in the argument register.
 *
 * So it is called with a VALID pointer and BOTH results are read. If it takes no arguments the extra
 * register is ignored; if it takes an out-parameter it gets somewhere real to write. Neither convention
 * can do damage, and the log prints both so the reader can see which one answered.
 *
 * ORBIS_ASK_GNM=1. */
static void
orbis_ask_gnm_tess_base(void)
{
#if defined(__PS4__)
   if (getenv("ORBIS_ASK_GNM") == NULL)
      return;

   uint64_t out = 0xDEADBEEFDEADBEEFull;
   uint64_t ret;

   {
      /* Both spellings at once. The prototype the SDK gives is empty, so this is the call site's choice
       * and it is made explicit rather than hidden in a header. */
      uint64_t (*const ask)(void *) = (uint64_t (*)(void *))sceGnmGetTheTessellationFactorRingBufferBaseAddress;
      ret = ask(&out);
   }

   mesa_logi("orbis-drm: sceGnmGetTheTessellationFactorRingBufferBaseAddress -> returned 0x%" PRIx64
             ", out-slot 0x%" PRIx64 " (0xdeadbeefdeadbeef means it wrote nothing there). This port has "
             "quoted 0xff0000000 from second-hand notes since the beginning; this is the first time it "
             "has asked.",
             ret, out);

   if (ret == 0xff0000000ull || out == 0xff0000000ull)
      mesa_logi("orbis-drm:   ...and it is 0xff0000000, so Sony's driver does place its factor ring exactly "
                "where the faulting reads land. That makes the reads tessellation traffic, not a stray "
                "descriptor - and our own registers demonstrably work, since changing the offchip count "
                "moved the write faults with the factor ring.");
#endif
}

/* ⚠ WHAT DOES SONY THINK A COMMAND BUFFER MUST BEGIN WITH?
 *
 * This is the question that should have been asked first, and it took until the end of the day to see it.
 *
 * RADV DOES NOT INITIALISE THE GPU. It assumes somebody already did. On Linux that somebody is the amdgpu
 * kernel driver at boot - golden registers, GB_ADDR_CONFIG, the tile tables, the rings, the VM - and RADV
 * writes only what changes per frame, silently relying on the rest being amdgpu's. Here the rest is SONY's.
 * That is not speculation: PA_SC_RASTER_CONFIG reads 0x2a00161a on this part where Bonaire's golden value
 * is 0x16000012, measured through two independent routes in the same run.
 *
 * And the SDK says so in its own vocabulary. sceGnmDrawInitDefaultHardwareState and
 * sceGnmDrawInitToDefaultContextState are not helpers - they are the CONTRACT: a Gnm command buffer begins
 * with a block that puts the hardware into a known state. This port has never emitted one. Every submission
 * it has ever made has run on whatever context the previous owner of the GPU left behind, and between our
 * submissions the previous owner is Sony's compositor.
 *
 * That reconciles the one contradiction this hunt could not resolve. The tessellation WRITES follow our
 * sizes, so our registers do take effect inside the submission that carries them; the READS land on Sony's
 * ring base, as though the state had been restored underneath us. Both at once is what a context we do not
 * own and do not initialise looks like.
 *
 * ⚠ AND THESE TWO CALLS ARE SAFE TO MAKE, which is why this is a dump and not an experiment. They take a
 * buffer and a size and FILL it; they submit nothing and touch no hardware. The signature is one of the few
 * the SDK states outright. So the cost is a memset and a log, and the return is Sony's own list of what a
 * command buffer must set - which can then be diffed against what RADV's preamble emits.
 *
 * The difference between those two lists is the most literal possible answer to "what is different from
 * Linux": it is the registers the kernel sets there and nobody sets here.
 *
 * ORBIS_DUMP_INIT_STATE=1. */
/* ⚠ MAKE SONY EMIT THE STAGE WE HAVE NO ORACLE FOR, AND READ WHAT SHE WROTE.
 *
 * This is a technique rather than a probe, and it is the cheapest source of truth this port has found.
 *
 * Every sceGnm entry point taking (cmdbuf, numdwords) is a PM4 BUILDER: it writes packets into the
 * caller's buffer and the caller advances by numdwords whether or not anything was written. So Sony's own
 * driver can be asked to lay out any pipeline stage, into memory we own, without submitting anything. What
 * comes back is how THIS hardware's vendor configures that stage - and the difference against what RADV
 * emits is a list of registers rather than a theory.
 *
 * The stage that matters is tessellation, because that is where this hunt is stuck and because it is
 * exactly where nobody has an oracle: OpenGothic really does tessellate (shader/materials/main.tesc and
 * .tese, wired through Tempest's VkPipelineTessellationStateCreateInfo), and a reimplementation of this
 * library refuses LS/HS by name for want of ground truth - "no title in the corpus programs
 * VGT_SHADER_STAGES_EN.HS_EN".
 *
 * ⚠ THE REGISTER BLOB IS INVENTED AND THAT IS FINE HERE. sce_gnm_set_hs_shader takes a pointer to an
 * HsStageRegisters this port cannot construct correctly. It does not need to: the question is WHICH
 * REGISTERS the builder writes and in what order, not what values. A zeroed blob answers that, and a
 * recognisable pattern answers it better - a 0xC0DEnnnn dword coming back in the stream says which field
 * of the structure landed in which register, which is the layout itself.
 *
 * Nothing is submitted. The buffer is ours, the blob is ours, and the only output is a log line.
 *
 * ORBIS_ASK_HS_LAYOUT=1. */
static void
orbis_ask_sony_hs_layout(void)
{
#if defined(__PS4__)
   if (getenv("ORBIS_ASK_HS_LAYOUT") == NULL)
      return;

   /* Marked rather than zeroed, so a value appearing in the emitted packets identifies WHICH input dword
    * it came from. 64 dwords is far more than any stage-register structure and costs nothing. */
   static uint32_t regs_in[64];
   static uint32_t buf[512];

   for (unsigned i = 0; i < ARRAY_SIZE(regs_in); ++i)
      regs_in[i] = 0xC0DE0000u | i;

   /* ⚠ A CONTROL BUILDER, BECAUSE "IT REFUSED" HAS TWO CAUSES AND ONLY ONE IS INTERESTING.
    *
    * Both tessellation builders refused every reservation from 1 to 64 dwords. That is either "the
    * argument shape is wrong" - in which case nothing here works and the technique is dead - or "these
    * two validate their register blob and ours is nonsense", which is specific and fixable.
    *
    * SetVsShader separates them. It takes the same (cmdbuf, size, regs, modifier) shape, it is one of the
    * builders a reimplementation of this library DOES emit PM4 for, and nothing about tessellation is
    * special to it. If it accepts, the shape is right and the tessellation pair is rejecting the BLOB. If
    * it refuses too, the shape is wrong and no amount of fixing the blob will help.
    *
    * ⚠ AND THE BLOB IS TRIED TWICE. Sony's builders plausibly validate the shader address inside a stage
    * register structure - SPI_SHADER_PGM_LO_* holds va >> 8 - and 0xC0DE0000 is not an address any of this
    * hardware would accept. So each builder is asked with the marked blob and again with one whose dwords
    * are a REAL arena address shifted, which is what a genuine structure would hold. The marked pass gives
    * the layout when it is accepted; the plausible pass says whether validation was the obstacle. */
   const struct {
      const char *name;
      uint32_t (*fill)(uint32_t *, uint32_t, const uint32_t *, uint32_t);
   } builders[] = {
      {"SetVsShader (control)",
       (uint32_t (*)(uint32_t *, uint32_t, const uint32_t *, uint32_t))sceGnmSetVsShader},
      {"SetHsShader", (uint32_t (*)(uint32_t *, uint32_t, const uint32_t *, uint32_t))sceGnmSetHsShader},
      {"SetLsShader", (uint32_t (*)(uint32_t *, uint32_t, const uint32_t *, uint32_t))sceGnmSetLsShader},
   };

   static uint32_t regs_plausible[64];
   for (unsigned i = 0; i < ARRAY_SIZE(regs_plausible); ++i)
      regs_plausible[i] = (uint32_t)(orbis_arena_private >> 8);

   for (unsigned b = 0; b < ARRAY_SIZE(builders); ++b) {
      /* ⚠ SWEEP THE SIZE, BECAUSE -1 IS WHAT A WRONG ONE LOOKS LIKE.
       *
       * The first attempt passed the whole buffer, 512 dwords, and both builders returned 0xFFFFFFFF - a
       * plain -1. These are fixed-size packet builders and Sony's own gnmx knows the exact reservation for
       * each; a builder handed a size that is not the one it wants has nothing sensible to do but refuse.
       *
       * That number is not knowable from here, so it is swept. A stage-register packet is a handful of
       * SET_SH_REG writes, so 64 is a generous ceiling, and the first size that is ACCEPTED is the answer -
       * it is the reservation the caller is supposed to make.
       *
       * ⚠ AND THE PREVIOUS VERSION ANNOUNCED THE RESULT BEFORE CHECKING IT. It printed "wrote 4294967295
       * dwords", which is -1 read as a length, and only the decode guard downstream stopped it turning
       * into a walk over nonsense. Validate, then claim. */
      uint32_t used = 0;
      unsigned accepted_at = 0;
      bool     marked = true;

      for (unsigned pass = 0; pass < 2 && accepted_at == 0; ++pass) {
         const uint32_t *const blob = pass == 0 ? regs_in : regs_plausible;

         for (unsigned sz = 1; sz <= 64; ++sz) {
            memset(buf, 0, sizeof(buf));
            const uint32_t r = builders[b].fill(buf, sz, blob, 0);
            if (r != 0xFFFFFFFFu && r != 0 && r <= ARRAY_SIZE(buf)) {
               used = r;
               accepted_at = sz;
               marked = (pass == 0);
               break;
            }
         }
      }

      if (accepted_at == 0) {
         mesa_loge("orbis-drm: %s refused every reservation from 1 to 64 dwords, with a marked blob AND "
                   "with one holding a real address - so it is not the size and not blob validation. The "
                   "argument shape is wrong.",
                   builders[b].name);
         continue;
      }

      if (!marked)
         mesa_logi("orbis-drm: %s accepted only the PLAUSIBLE blob, so it validates the register contents - "
                   "the layout below is real but the input-dword mapping is not available this way",
                   builders[b].name);

      mesa_logi("orbis-drm: %s accepted a reservation of %u dwords and wrote %u - this is how SONY lays out "
                "the stage this port has no oracle for",
                builders[b].name, accepted_at, used);

      for (uint32_t k = 0; k < used;) {
         const uint32_t hdr = buf[k];
         if ((hdr & 0xC0000000u) != 0xC0000000u) {
            mesa_logi("orbis-drm:   %s dword %u is 0x%08x, not a type-3 header - stopping", builders[b].name,
                      k, hdr);
            break;
         }
         const uint32_t op = (hdr >> 8) & 0xFFu;
         const uint32_t cnt = ((hdr >> 16) & 0x3FFFu) + 1;
         uint32_t       base = 0;

         switch (op) {
         case 0x69: base = 0x28000u; break;
         case 0x76: base = 0x0b000u; break;
         case 0x79: base = 0x30000u; break;
         case 0x68: base = 0x08000u; break;
         default: break;
         }

         if (base != 0 && cnt >= 2) {
            for (uint32_t r = 0; r + 1 < cnt; ++r) {
               const uint32_t v = buf[k + 2 + r];
               const uint32_t reg = base + ((buf[k + 1] + r) << 2);
               if ((v & 0xFFFF0000u) == 0xC0DE0000u)
                  mesa_logi("orbis-drm:   %s reg 0x%05x <- input dword %u", builders[b].name, reg,
                            v & 0xFFFFu);
               else
                  mesa_logi("orbis-drm:   %s reg 0x%05x = 0x%08x  (a constant Sony sets, not from the blob)",
                            builders[b].name, reg, v);
            }
         } else {
            mesa_logi("orbis-drm:   %s op 0x%02x, %u dwords - sets no register", builders[b].name, op, cnt);
         }
         k += 1 + cnt;
      }
   }
#endif
}

static void
orbis_dump_sony_init_state(void)
{
#if defined(__PS4__)
   if (getenv("ORBIS_DUMP_INIT_STATE") == NULL)
      return;

   /* Sony's own samples size these at a few hundred dwords; 4096 is room to spare and the returned length
    * is what says how much was really written. */
   static uint32_t buf[4096];
   const struct {
      const char *name;
      uint32_t (*fill)(uint32_t *, uint32_t);
   } inits[] = {
      /* Cast because the SDK header declares these as `void f()` like the rest. The real shape is stated
       * plainly in Sony's own documentation and in every reimplementation of it - a buffer, a size, and the
       * number of dwords written - and unlike the debugger family there is nothing to guess: both arguments
       * are ours and neither is an address the callee could dereference wrongly. */
      {"DrawInitDefaultHardwareState", (uint32_t (*)(uint32_t *, uint32_t))sceGnmDrawInitDefaultHardwareState},
      {"DrawInitToDefaultContextState",
       (uint32_t (*)(uint32_t *, uint32_t))sceGnmDrawInitToDefaultContextState},
   };

   for (unsigned i = 0; i < ARRAY_SIZE(inits); ++i) {
      memset(buf, 0, sizeof(buf));
      const uint32_t used = inits[i].fill(buf, ARRAY_SIZE(buf));

      mesa_logi("orbis-drm: %s filled %u dwords - this is what SONY says a command buffer must begin with, "
                "and this port has never emitted any of it",
                inits[i].name, used);

      if (used == 0 || used > ARRAY_SIZE(buf)) {
         mesa_loge("orbis-drm:   ...but %u dwords is not a length this buffer can hold, so the call did not "
                   "do what its signature says and nothing below is trustworthy",
                   used);
         continue;
      }

      /* Decoded rather than hexdumped: a list of REGISTERS is the thing to diff against RADV's preamble,
       * and a wall of dwords is not that. Only the register-setting packets are named; everything else is
       * counted, because "and 14 other packets" is honest where silence is not. */
      unsigned other = 0;
      for (uint32_t k = 0; k < used;) {
         const uint32_t hdr = buf[k];
         if ((hdr & 0xC0000000u) != 0xC0000000u) {
            mesa_logi("orbis-drm:   dword %u is 0x%08x, not a type-3 header - stopping the walk here", k, hdr);
            break;
         }
         const uint32_t op = (hdr >> 8) & 0xFFu;
         const uint32_t cnt = ((hdr >> 16) & 0x3FFFu) + 1;
         uint32_t       base = 0;

         switch (op) {
         case 0x69: base = 0x28000u; break; /* SET_CONTEXT_REG */
         case 0x76: base = 0x0b000u; break; /* SET_SH_REG      */
         case 0x79: base = 0x30000u; break; /* SET_UCONFIG_REG */
         case 0x68: base = 0x08000u; break; /* SET_CONFIG_REG  */
         default: break;
         }

         if (base != 0 && cnt >= 2) {
            for (uint32_t r = 0; r + 1 < cnt; ++r)
               mesa_logi("orbis-drm:   %-30s reg 0x%05x = 0x%08x", inits[i].name,
                         base + ((buf[k + 1] + r) << 2), buf[k + 2 + r]);
         } else {
            ++other;
         }
         k += 1 + cnt;
      }
      if (other != 0)
         mesa_logi("orbis-drm:   %s: and %u packet(s) that set no register (waits, events, clears)",
                   inits[i].name, other);
   }
#endif
}


/* ⚠ ASK THE CHIP, BECAUSE THIS IS THE ONE PLACE THE PORT HAS BEEN GUESSING HARDEST.
 *
 * On Linux this file does not exist: the amdgpu KERNEL reads the ASIC's configuration registers and hands the
 * values to userspace, and ps4-video-archlinux shows how little else has to change for RADV to drive this exact
 * silicon - a handful of numbers. So every number this arm INVENTS is a candidate defect, and the biggest of them
 * is GB_ADDR_CONFIG with the tile-mode tables built to match it: addrlib computes every surface layout from those,
 * and this arm synthesises them from assumptions nobody measured (`Row size assumed 2 KB`).
 *
 * The command processor can read a memory-mapped register into memory: PKT3_COPY_DATA with SRC_SEL=REG and
 * DST_SEL=MEM, which is how a user-mode driver reads registers without a kernel. So the chip can be asked
 * directly, and the guessing stops.
 *
 * ⚠ TWO KINDS OF OFFSET, AND CONFLATING THEM IS WHAT BROKE THIS FUNCTION THE FIRST TIME. gfx7.json and the
 * kernel headers name registers by BYTE offset - GB_ADDR_CONFIG at 39160 = 0x98F8, GB_TILE_MODE0 at 0x9910,
 * GB_MACROTILE_MODE0 at 0x9990, PA_SC_RASTER_CONFIG at 0x28350 - while COPY_DATA's register field is a DWORD
 * INDEX. The table below is in bytes, because that is what the citations are in, and the packet divides by four
 * at the one place it is emitted. Consecutive registers are therefore FOUR apart, not one.
 *
 * THE CONTROL IS A REGISTER THIS FUNCTION WRITES ITSELF. A live status register cannot be a control: any value
 * it returns looks plausible, so it cannot tell "the chip said this" apart from "the CP skipped the read and
 * memory happened to hold that". SCRATCH_REG0 can - it is written with a known pattern in the same packet
 * stream and read back through the same path, so the pattern coming back proves the packet, the addressing and
 * the destination all at once, and anything else invalidates the whole dump.
 *
 * WHAT THE DUMP IS FOR. Every register in it is one amdgpu writes at init and RADV never writes at all, so on
 * Linux it holds a value the kernel chose and here it holds a value SONY's kernel chose. The log prints both
 * and marks the differences, because that comparison is the only honest answer to "how can the same silicon
 * behave differently under two operating systems": it cannot, and it is not the same silicon STATE.
 *
 * ORBIS_READ_REGS=1. One shot, before the first real submission - so what it reads is what Sony left, not what
 * RADV's own preamble is about to set. */
/* ⚠ CAN WE WRITE CONFIG SPACE? The read backdoor is proven; this asks the same question in the other direction,
 * and the answer decides whether "give this GPU the configuration Linux gives it" is a plan or a wish.
 *
 * Mesa writes the one privileged register it needs - SPI_CONFIG_CNTL - through a COPY_DATA back door rather than
 * SET_CONFIG_REG: SRC_SEL=COPY_DATA_IMM(5) -> DST_SEL=COPY_DATA_PERF(4), the value in dword 2 and the register as
 * `reg >> 2` in dword 4 (ac_pm4_set_privileged_reg, ac_pm4.c:352-364). The READ mirror of that - PERF -> GRBM -
 * was measured working on this console, cross-checked against the plain select on PA_SC_RASTER_CONFIG in the same
 * run. So the write direction is the obvious next question and nobody has asked it.
 *
 * ⚠ THE CONTROL IS AN INERT ROUND TRIP, WHICH IS WHY THIS IS SAFE TO RUN. For each register it READS the current
 * value, WRITES THAT SAME VALUE BACK, and reads again. Nothing changes on the GPU no matter what happens, and yet
 * a second read returning the value proves the write path reached the register - because the read path was
 * already proven independently. A write probe that alters behaviour to prove it can write would be a worse
 * experiment and a worse risk.
 *
 * What it CANNOT prove: that a write of a DIFFERENT value takes. A register that ignores writes entirely returns
 * its old value and looks like success. So the probe also writes one register with an INVERTED copy of itself and
 * reads back, then restores it - and it picks GB_TILE_MODE31, which this port's own tile table reports as 0 and
 * addrlib never selects, so the blast radius of the one non-inert write is a table slot nothing reads.
 *
 * ORBIS_WRITE_REGS=1. */
static void
orbis_probe_config_writes(void)
{
   if (getenv("ORBIS_WRITE_REGS") == NULL || orbis_arena_private == 0)
      return;

   /* ⚠ TWO REGISTERS ONLY, AND THAT IS THE SAFE VERSION OF THIS EXPERIMENT.
    *
    * The first draft of this probe wrote a literal 0 to GB_ADDR_CONFIG, TA_CNTL_AUX, DB_DEBUG and PA_SC_ENHANCE
    * on the theory that it was writing "what is already there". It was not - 0 in GB_ADDR_CONFIG destroys every
    * surface address on the part, and if the write mechanism works then that draft would have proved it by
    * breaking the GPU. Deleted rather than guarded.
    *
    *   SCRATCH_REG0   0x30100, USER-CONFIG. The control: a scratch register with no hardware meaning
    *                  (kernel-cik.c:3420, "a legacy interface"), written through the back door and read back
    *                  through the proven one. Pattern returns = the write encoding is right.
    *   GB_TILE_MODE31 0x0998c, CONFIG SPACE and therefore privileged - which is the whole question. This port's
    *                  own tile table reports slot 31 as 0 and addrlib never selects it, so it is the one
    *                  privileged register whose value nothing on this system reads. Written, read back, restored.
    *
    * Those two together answer it: control passing says the encoding is right, the spare slot changing says
    * PRIVILEGED CONFIG SPACE IS WRITABLE FROM THIS RING. Nothing else needs to be touched to learn that, and
    * everything else that could be touched has consequences.
    *
    * ORBIS_WRITE_REGS=1. */
   struct wq { const char *name; uint32_t reg; uint32_t poke; };
   static const struct wq list[] = {
      { "SCRATCH_REG0 (uconfig, control)", 0x30100, 0xC0FFEE02u },
      { "GB_TILE_MODE31 (config, spare)",  0x0998c, 0xA5A5A5A5u },
   };

   volatile uint32_t *const dst  = (volatile uint32_t *)(uintptr_t)(orbis_arena_private + 1024);
   volatile uint32_t *const flag = (volatile uint32_t *)(uintptr_t)(orbis_arena_private + 512);
   uint32_t *const ib = (uint32_t *)(uintptr_t)(orbis_arena_private + 16384);

   for (unsigned i = 0; i < ARRAY_SIZE(list); ++i) {
      const uint32_t reg = list[i].reg;

      /* One submission does read, write, read - so the before and after land in a known order and slot 0 versus
       * slot 1 is the entire verdict. */
      dst[0] = 0xDEAD0001u;
      dst[1] = 0xDEAD0002u;
      *flag = 0;

      uint32_t *dw = ib;
      for (unsigned pass = 0; pass < 2; ++pass) {
         const uint64_t dva = (uint64_t)(uintptr_t)&dst[pass];
         /* read, through the backdoor already proven on this console: PERF -> GRBM */
         *dw++ = ORBIS_PM4_TYPE3(0x40 /* PKT3_COPY_DATA */, 5);
         *dw++ = 4u | (1u << 8) | (1u << 20);
         *dw++ = reg >> 2;
         *dw++ = 0;
         *dw++ = (uint32_t)dva;
         *dw++ = (uint32_t)(dva >> 32);
         if (pass == 0) {
            /* write, through its mirror: IMM -> PERF, value in dword 2 and the register as reg >> 2 in dword 4.
             * Exactly ac_pm4_set_privileged_reg (ac_pm4.c:352-364). */
            *dw++ = ORBIS_PM4_TYPE3(0x40, 5);
            *dw++ = 5u | (4u << 8);
            *dw++ = list[i].poke;
            *dw++ = 0;
            *dw++ = reg >> 2;
            *dw++ = 0;
         }
      }
      dw = orbis_emit_eop(dw, (uint64_t)(uintptr_t)flag, 1u, ORBIS_EOP_DST_SEL_MC | ORBIS_EOP_DATA_SEL_32BIT |
                                                          ((uint32_t)ORBIS_EOP_INT_SEL << ORBIS_EOP_INT_SEL_SHIFT));
      dw = orbis_pad_ib(dw, ib);

      mesa_logi("orbis-drm: WRITE_REGS %u ASKING: %-32s [byte 0x%05x -> dword 0x%05x], poking 0x%08x", i,
                list[i].name, reg, reg >> 2, list[i].poke);

      void    *dcb[1] = {ib};
      uint32_t bytes[1] = {(uint32_t)((dw - ib) * 4)};
      sceGnmFlushGarlic();
      const int32_t err = sceGnmSubmitCommandBuffers(1, dcb, bytes, NULL, NULL);
      if (err != 0) {
         mesa_loge("orbis-drm: WRITE_REGS %u REFUSED at submit: 0x%08x - the probe stops", i, (unsigned)err);
         return;
      }
      for (unsigned spin = 0; spin < 20000 && *flag == 0; ++spin)
         sceKernelUsleep(10);
      if (*flag == 0) {
         mesa_loge("orbis-drm: WRITE_REGS %u NO END-OF-PIPE IN 200 ms - everything above this line stands", i);
         return;
      }

      const uint32_t before = dst[0], after = dst[1];
      if (after == list[i].poke && before != list[i].poke) {
         mesa_logi("orbis-drm: WRITE_REGS %u ⭐ THE WRITE TOOK: %-32s was 0x%08x, poked 0x%08x, reads 0x%08x.%s", i,
                   list[i].name, before, list[i].poke, after,
                   reg == 0x0998cu ? "  PRIVILEGED CONFIG SPACE IS WRITABLE FROM THIS RING." : "");
      } else if (after == before) {
         mesa_logw("orbis-drm: WRITE_REGS %u NO CHANGE: %-32s 0x%08x before and after, poked 0x%08x. Either the "
                   "write never reached the register or the register ignores writes.%s", i, list[i].name, before,
                   list[i].poke,
                   reg == 0x0998cu ? "  If the control above passed, the encoding is right and CONFIG SPACE "
                                     "REFUSES WRITES - which closes branch (A) for good." : "");
      } else {
         mesa_logw("orbis-drm: WRITE_REGS %u UNEXPECTED: %-32s before 0x%08x, poked 0x%08x, after 0x%08x", i,
                   list[i].name, before, list[i].poke, after);
      }

      /* Put it back, so the run leaves nothing behind whichever way it went. */
      if (after != before) {
         uint32_t *r = ib;
         *r++ = ORBIS_PM4_TYPE3(0x40, 5);
         *r++ = 5u | (4u << 8);
         *r++ = before;
         *r++ = 0;
         *r++ = reg >> 2;
         *r++ = 0;
         *r++ = ORBIS_PM4_TYPE3(0x10, 1);
         *r++ = 0;
         void    *rd[1] = {ib};
         uint32_t rb[1] = {(uint32_t)((r - ib) * 4)};
         sceGnmFlushGarlic();
         (void)sceGnmSubmitCommandBuffers(1, rd, rb, NULL, NULL);
         mesa_logi("orbis-drm: WRITE_REGS %u restored %s to 0x%08x", i, list[i].name, before);
      }
   }
}

/* ⚠ DOES THE GS RING SIZE THE VGT WRAPS ON HAVE ANYTHING TO DO WITH THE ONE WE PROGRAM?
 *
 * The geometry rings on this port fail in two ways that no amount of reading the driver has explained: a ring
 * that wraps early and overwrites itself (WRONG IMAGES) and a ring the hardware writes past the end of (GPU
 * FAULT). Eight configurations were tried, the shipped one is a margin rather than a repair, and the two
 * register-side candidates named for this investigation are now BOTH ELIMINATED BY ARITHMETIC:
 *
 *   VGT_GSVS_RING_ITEMSIZE     radv_shader.c:1793-1809 accumulates it term for term with
 *   VGT_GS_VERT_ITEMSIZE[0-3]  si_state_shaders.cpp:565-586, and both drivers emit it the same way. The
 *   VGT_GSVS_RING_OFFSET_1-3   fields are 15 bits (0x7FFF) and the failing shaders need ~192 dwords: the
 *                              multiview GS is `triangles` in, `max_vertices = 16` out, some ten components
 *                              per vertex. Three orders of magnitude below the limit, so nothing truncates.
 *   on-chip versus off-chip    not a variable on this generation. ac_shader_util.c:102 sets ONCHIP only from
 *                              GFX9 up, in SHARED code, so RADV and radeonsi both run GFX7 off-chip and there
 *                              is no second setting to get wrong.
 *
 * ⚠ AND THE 63.999 MB CLAMP IS NOT ENGAGED EITHER, which kills the tidiest remaining theory. That clamp is not
 * a multiple of the per-wave item size, so a clamped ring's last wave slot WOULD straddle the end - but the
 * multiview ring computes to roughly 6 MB before scaling against a 128 MB ceiling. It never reaches it.
 *
 * So every register-side explanation is spent, and what is left is the question this probe asks. The size the
 * VGT wraps on comes from ONE register, written in the queue preamble (radv_queue.c:471-477):
 *
 *     R_030900_VGT_ESGS_RING_SIZE   0x030900, in 256-byte units
 *     R_030904_VGT_GSVS_RING_SIZE   0x030904, in 256-byte units
 *
 * ⚠ THIS CONSOLE IS ALREADY KNOWN TO SWALLOW REGISTER WRITES SILENTLY. All three tessellation registers fail a
 * write-then-read round trip in the same packet stream while SCRATCH_REG0 round trips perfectly, which is why
 * task #21 has no ground truth. If the ring-size registers behave the same way, then the VGT is wrapping on
 * whatever Sony's context state left there, our allocation and its programmed size are unrelated numbers, and
 * that single fact explains EVERYTHING the eight configurations measured: both faults, why padding the
 * ALLOCATION works while scaling the declared SIZE does not fully, and why the requirement came out flat
 * rather than per-view when multiview was the family that exposed it.
 *
 * The test is cheap and the controls are already proven. Both registers live in USER-CONFIG space, in the same
 * 0x30000 window as SCRATCH_REG0 at 0x30100 - the one register on this part known to take a SET_UCONFIG_REG
 * write and read back through the COPY_DATA backdoor. So the mechanism needs no new validation: rung 0 is that
 * same control, and a rung that disagrees with it is about the register and not about the route.
 *
 *   rung 0   SCRATCH_REG0        control. Writes a pattern, reads it back. Known to pass.
 *   rung 1   VGT_ESGS_RING_SIZE  read only - what is in it before anything of ours has run
 *   rung 2   VGT_GSVS_RING_SIZE  read only
 *   rung 3   VGT_ESGS_RING_SIZE  read, SET_UCONFIG_REG write, read - THE ROUND TRIP, then restored
 *   rung 4   VGT_GSVS_RING_SIZE  the same
 *
 * ⚠ RUN THIS IN A DEDICATED SHORT RUN, NOT BUNDLED WITH A SWEEP. The COND_WRITE probe killed the session it
 * shipped in even though every one of its rungs printed. This one restores what it pokes, bounds every spin at
 * 200 ms and stops at the first refusal, but the precedent stands and the answers are worth more than the
 * tests that would be lost around them.
 *
 * The poked pattern is a LEGAL small ring size (0x1234 x 256 = 1.1 MB) rather than a marker word, because a
 * marker in this register is a ring size too and nothing here guarantees no other process draws.
 *
 * ORBIS_GS_RING_REGS=1, ORBIS_GS_RING_REGS_FIRST / _LAST to step over a rung that kills the run. */
static void
orbis_test_gs_ring_regs(void)
{
   if (getenv("ORBIS_GS_RING_REGS") == NULL || orbis_arena_private == 0)
      return;

   struct rung { const char *name; uint32_t reg; bool write; const uint32_t *expect; };
   static const struct rung rungs[] = {
      { "SCRATCH_REG0 (control)",  0x30100, true,  NULL },
      { "VGT_PRIMITIVE_TYPE",      0x30908, false, NULL },
      { "VGT_ESGS_RING_SIZE",      0x30900, false, &orbis_gs_ring_esgs_shifted },
      { "VGT_GSVS_RING_SIZE",      0x30904, false, &orbis_gs_ring_gsvs_shifted },
      { "VGT_ESGS_RING_SIZE",      0x30900, true,  &orbis_gs_ring_esgs_shifted },
      { "VGT_GSVS_RING_SIZE",      0x30904, true,  &orbis_gs_ring_gsvs_shifted },
   };
   /* ⚠ THE POKES ARE OPT-IN NOW, AND RUN 49 IS WHY. Its ladder read zero from both ring registers and read
    * zero back after poking them, and the very next thing the run did was hang inside its FIRST geometry test
    * - one that passes in the shipped configuration. Two readings fit and the run cannot separate them:
    *
    *   the write landed and the read is blind   the ring shrank to 1.1 MB and the first GS draw died on it,
    *                                            which is what the tessellation block already does: written
    *                                            and read back in one stream, it answers 0x00000000
    *   the probe's own submissions did it       COND_WRITE killed the run it shipped in the same way
    *
    * Read-only is the default so the next run can survive to say which. ORBIS_GS_RING_REGS_LAST=5 arms the
    * pokes deliberately. */
   const unsigned last_safe = 3;

   /* ⚠ THE READ-ONLY RUNGS ARE ONLY WORTH SOMETHING AFTER OUR PREAMBLE HAS RUN. Say so rather than let the
    * next reader assume it: with ORBIS_READ_REGS_AT unset the ladder fires before the first submission, and
    * a zero there is Sony's state, not a verdict on ours. */
   if (orbis_gs_ring_gsvs_shifted == 0)
      mesa_logw("orbis-drm: GS_RING_REGS ⚠ radv_emit_gs_ring_sizes has NOT run yet, so rungs 1-2 read Sony's "
                "state and say NOTHING about whether our value lands. Set ORBIS_READ_REGS_AT=gs to fire on "
                "that fact instead. The round trip in rungs 3-4 stands either way.");
   else
      mesa_logi("orbis-drm: GS_RING_REGS this driver programmed ESGS=0x%08x (%u B), GSVS=0x%08x (%u B). Rungs "
                "1-2 are read back against exactly those.",
                orbis_gs_ring_esgs_shifted, orbis_gs_ring_esgs_shifted * 256u, orbis_gs_ring_gsvs_shifted,
                orbis_gs_ring_gsvs_shifted * 256u);
   const uint32_t poke = 0x00001234u;   /* 1.1 MB expressed the way this register expresses sizes */

   unsigned first = 0, last = last_safe;
   {
      const char *const s = getenv("ORBIS_GS_RING_REGS_FIRST");
      const char *const e = getenv("ORBIS_GS_RING_REGS_LAST");
      if (s != NULL) first = (unsigned)strtoul(s, NULL, 0);
      if (e != NULL) last = (unsigned)strtoul(e, NULL, 0);
      if (last >= ARRAY_SIZE(rungs)) last = (unsigned)ARRAY_SIZE(rungs) - 1;
   }

   mesa_logi("orbis-drm: GS_RING_REGS ladder of %u rungs, running [%u, %u]%s. Rung 0 is the SCRATCH_REG0 "
             "control and MUST pass for any later rung to mean anything; rung 1 is VGT_PRIMITIVE_TYPE, which "
             "says whether this read route can see the VGT block AT ALL.",
             (unsigned)ARRAY_SIZE(rungs), first, last,
             last > last_safe ? " - ⚠ INCLUDING THE POKES, which are suspected of killing run 49" : "");

   volatile uint32_t *const dst = (volatile uint32_t *)(uintptr_t)(orbis_arena_private + 1024);
   volatile uint32_t *const flag = (volatile uint32_t *)(uintptr_t)(orbis_arena_private + 512);
   uint32_t *const ib = (uint32_t *)(uintptr_t)(orbis_arena_private + 16384);

   bool control_passed = false;

   for (unsigned i = first; i <= last; ++i) {
      const uint32_t reg = rungs[i].reg;
      const uint32_t udw = (reg - 0x30000u) >> 2;   /* SET_UCONFIG_REG's own encoding, not COPY_DATA's */

      dst[0] = 0xDEAD0001u;
      dst[1] = 0xDEAD0002u;
      *flag = 0;

      /* read, [write, read] - one submission, so before and after land in a known order. */
      uint32_t *dw = ib;
      for (unsigned pass = 0; pass < (rungs[i].write ? 2u : 1u); ++pass) {
         const uint64_t dva = (uint64_t)(uintptr_t)&dst[pass];
         *dw++ = ORBIS_PM4_TYPE3(0x40 /* PKT3_COPY_DATA */, 5);
         *dw++ = 4u /* PERF */ | (1u << 8) /* DST_MEM_GRBM */ | (1u << 20) /* WR_CONFIRM */;
         *dw++ = reg >> 2;
         *dw++ = 0;
         *dw++ = (uint32_t)dva;
         *dw++ = (uint32_t)(dva >> 32);
         if (pass == 0 && rungs[i].write) {
            /* ⚠ WRITTEN THE WAY RADV WRITES IT, which is the entire point: radv_emit_gs_ring_sizes() uses
             * radeon_set_uconfig_reg_seq, so a backdoor write here would test a route the driver never takes. */
            *dw++ = ORBIS_PM4_TYPE3(0x79 /* IT_SET_UCONFIG_REG */, 2);
            *dw++ = udw;
            *dw++ = poke;
         }
      }
      dw = orbis_emit_eop(dw, (uint64_t)(uintptr_t)flag, 1u,
                          ORBIS_EOP_DST_SEL_MC | ORBIS_EOP_DATA_SEL_32BIT |
                             ((uint32_t)ORBIS_EOP_INT_SEL << ORBIS_EOP_INT_SEL_SHIFT));
      dw = orbis_pad_ib(dw, ib);

      /* Said BEFORE the submission: if this rung is the fatal one, the announcement is what survives. */
      mesa_logi("orbis-drm: GS_RING_REGS rung %u ASKING: %-24s [byte 0x%05x -> dword 0x%05x]%s", i, rungs[i].name,
                reg, reg >> 2,
                rungs[i].write ? ", round trip via SET_UCONFIG_REG" : ", read only");

      void *dcb[1] = {ib};
      uint32_t bytes[1] = {(uint32_t)((dw - ib) * 4)};
      sceGnmFlushGarlic();
      const int32_t err = sceGnmSubmitCommandBuffers(1, dcb, bytes, NULL, NULL);
      if (err != 0) {
         mesa_loge("orbis-drm: GS_RING_REGS rung %u REFUSED at submit: 0x%08x - the ladder stops rather than "
                   "guessing what state the ring is in", i, (unsigned)err);
         return;
      }
      for (unsigned spin = 0; spin < 20000 && *flag == 0; ++spin)
         sceKernelUsleep(10);
      if (*flag == 0) {
         mesa_loge("orbis-drm: GS_RING_REGS rung %u NO END-OF-PIPE IN 200 ms - everything above this line "
                   "stands, nothing below it will be asked", i);
         return;
      }

      const uint32_t before = dst[0];

      if (!rungs[i].write && rungs[i].expect == NULL) {
         /* ⚠ THE BLOCK DISCRIMINATOR, AND WITHOUT IT A ZERO IS NOT AN ANSWER. VGT_PRIMITIVE_TYPE sits four
          * bytes from VGT_GSVS_RING_SIZE in the same user-config block, and RADV writes it on every draw
          * (radv_cmd_buffer.c, R_030908). If it reads back a plausible primitive type then this read route
          * CAN see the block and a zero from the ring registers is a real zero. If it reads zero too, the
          * route is blind here exactly as it is blind to the tessellation block, and rungs 2-3 said nothing
          * at all - which is the mistake the tessellation hunt made and wrote down so it would not recur. */
         if (before == 0)
            mesa_logw("orbis-drm: GS_RING_REGS rung %u ⚠ BLIND: %-24s reads 0x00000000, and RADV writes this "
                      "register on every draw. This read route cannot see the VGT block, so ANY zero from the "
                      "ring registers is a NON-ANSWER. Nothing below may be read as a verdict.",
                      i, rungs[i].name);
         else
            mesa_logi("orbis-drm: GS_RING_REGS rung %u ⭐ THE BLOCK IS VISIBLE: %-24s reads 0x%08x (prim type "
                      "%u), so a zero from the ring registers below is a REAL zero.", i, rungs[i].name, before,
                      before & 0x3f);
         continue;
      }

      if (!rungs[i].write) {
         const uint32_t want = *rungs[i].expect;
         if (want == 0) {
            mesa_logi("orbis-drm: GS_RING_REGS rung %u READ: %-24s = 0x%08x (%u B). Nothing of ours has been "
                      "programmed yet, so this is Sony's state and not a verdict.", i, rungs[i].name, before,
                      before * 256u);
         } else if (before == want) {
            mesa_logi("orbis-drm: GS_RING_REGS rung %u ⭐ MATCHES: %-24s = 0x%08x (%u B), which is exactly what "
                      "this driver programmed. The VGT wraps on OUR size and the ring defect is elsewhere.", i,
                      rungs[i].name, before, before * 256u);
         } else {
            mesa_logw("orbis-drm: GS_RING_REGS rung %u ⚠ MISMATCH: %-24s reads 0x%08x (%u B) where we "
                      "programmed 0x%08x (%u B). The size the hardware wraps on is NOT the size we asked for, "
                      "which would explain both ring faults at once.", i, rungs[i].name, before, before * 256u,
                      want, want * 256u);
         }
         continue;
      }

      const uint32_t after = dst[1];
      const bool took = (after == poke && before != poke);

      if (i == 0) {
         control_passed = took;
         if (took)
            mesa_logi("orbis-drm: GS_RING_REGS rung 0 CONTROL PASSED - SET_UCONFIG_REG lands and the backdoor "
                      "reads it back (0x%08x -> 0x%08x). Every later rung is now about the REGISTER.", before,
                      after);
         else
            mesa_loge("orbis-drm: GS_RING_REGS rung 0 ⚠ CONTROL FAILED: before 0x%08x, poked 0x%08x, after "
                      "0x%08x. The MECHANISM is broken in this build and NOTHING below may be read as a "
                      "statement about the ring registers.", before, poke, after);
      } else if (took) {
         mesa_logi("orbis-drm: GS_RING_REGS rung %u ⭐ THE WRITE TOOK: %-24s was 0x%08x, poked 0x%08x, reads "
                   "0x%08x. So the VGT wraps on a size WE control, and the ring defect is not this register.%s",
                   i, rungs[i].name, before, poke, after,
                   control_passed ? "" : "  ⚠ BUT THE CONTROL FAILED, so treat this as unproven.");
      } else if (after == before) {
         mesa_logw("orbis-drm: GS_RING_REGS rung %u ⚠ NO CHANGE: %-24s 0x%08x before and after, poked 0x%08x.%s",
                   i, rungs[i].name, before, poke,
                   control_passed ? "  The control passed in this same run, so the encoding is right and THIS "
                                    "REGISTER DOES NOT TAKE OUR WRITES - which would mean the size the VGT "
                                    "wraps on has never been ours."
                                  : "  The control failed too, so this says nothing yet.");
      } else {
         mesa_logw("orbis-drm: GS_RING_REGS rung %u UNEXPECTED: %-24s before 0x%08x, poked 0x%08x, after 0x%08x",
                   i, rungs[i].name, before, poke, after);
      }

      /* Put it back. A ring size left at 1.1 MB is not a marker word - it is a ring size, and something else
       * on this system may draw with it. */
      if (after != before) {
         uint32_t *r = ib;
         *r++ = ORBIS_PM4_TYPE3(0x79 /* IT_SET_UCONFIG_REG */, 2);
         *r++ = udw;
         *r++ = before;
         *r++ = ORBIS_PM4_TYPE3(0x10 /* PKT3_NOP */, 1);
         *r++ = 0;
         void *rd[1] = {ib};
         uint32_t rb[1] = {(uint32_t)((r - ib) * 4)};
         sceGnmFlushGarlic();
         (void)sceGnmSubmitCommandBuffers(1, rd, rb, NULL, NULL);
         mesa_logi("orbis-drm: GS_RING_REGS rung %u restored %s to 0x%08x", i, rungs[i].name, before);
      }
   }
}

/* Defined next to the GB_ADDR_CONFIG field shifts it decodes, further down this file. Declared here because
 * the register reader above wants it and the shifts do not exist yet at that point. */
static void orbis_log_gb_addr_config_read(uint32_t v);

/* ONE SUBMISSION PER REGISTER, and that is the whole design of this second attempt.
 *
 * The first attempt asked for 71 registers in one command buffer. The submission was ACCEPTED - the log says
 * `READ_REGS submitted 440 dwords for 71 registers -> 0x00000000` - and then the title died before printing a
 * single value, with the step log stopping at `before-vkQueueSubmit`. So the command processor executed the
 * buffer and something in it was fatal, and a batch cannot say WHICH: every value arrives after the whole
 * buffer has run, so one bad register takes all 71 answers with it.
 *
 * One register per submission, each printed as soon as it lands, turns that into a bisect that finishes in one
 * run: the log file on /data holds every register up to the fatal one, and the first missing line names it.
 *
 * The ladder starts below the registers, because "the CP died" and "this register is unreadable" are different
 * claims and the first two rungs separate them:
 *
 *   rung 0   an end-of-pipe packet ALONE, touching no register. If this dies, a standalone command buffer at
 *            this point in init is what is fatal, and nothing here is about registers at all.
 *   rung 1   SET_UCONFIG_REG writes a pattern into SCRATCH_REG0 and COPY_DATA reads it back. If rung 0 lived
 *            and this dies, the read MECHANISM is at fault. If the pattern comes back, the mechanism is proven
 *            and every later death belongs to a specific named register.
 *   rung 2+  one register each, least-known first. GB_ADDR_CONFIG leads because nothing has ever measured it
 *            and five synthesised fields depend on it; the tile tables come last because they are a
 *            cross-check of something the upload rung already confirmed through the texture unit.
 *
 * ORBIS_READ_REGS_FIRST / _LAST bound the ladder by index, so a known-fatal register can be stepped over
 * without a rebuild-and-reflash. The index of every rung is printed with it, for exactly that purpose. */
static void
orbis_read_hw_registers(void)
{
   if (getenv("ORBIS_READ_REGS") == NULL || orbis_arena_private == 0)
      return;

   /* ⚠ THE PRIVILEGE RULE IS NOW MEASURED, AND THE PLAIN READ IS SPENT.
    *
    * Run 3 asked for PA_SC_RASTER_CONFIG (context, dword 0xA0D4) and got 0x2a00161a; asked for
    * VGT_CACHE_INVALIDATION (config, dword 0x2231) and the title died - after run 2 had already died on
    * GB_ADDR_CONFIG (config, dword 0x263E) with the mechanism proven one rung earlier by SCRATCH_REG0
    * (dword 0xC040). Four data points, and AMD's own driver predicts all four from one rule: on Gfx7+ every
    * CONFIG register outside the user-config window - dwords [0xC000, 0xFFFF] - is PRIVILEGED, and a
    * privileged register can be neither read nor written with COPY_DATA's plain register select.
    *
    * SCRATCH_REG0 at dword 0xC040 is INSIDE that window, which is exactly why the control passed. It was
    * chosen for being scratch, and it turned out to prove the rule as well.
    *
    * ⚠ AND MESA WAS MISREAD ON THE WAY HERE. ac_pm4.c:33-49 returns only SPI_CONFIG_CNTL for GFX6-8, and that
    * is not a claim that the rest of config space is unprivileged - it is the list of privileged registers MESA
    * NEEDS TO WRITE, because under amdgpu the kernel wrote the others and Mesa never touches them.
    *
    * THE BACKDOOR IS THE REMAINING ROUTE, and Mesa is the pinned witness for its encoding rather than a
    * second-hand one. ac_pm4_set_privileged_reg (ac_pm4.c:352-364) writes a privileged register with
    * SRC_SEL=COPY_DATA_IMM(5) -> DST_SEL=COPY_DATA_PERF(4) and the register as `reg >> 2`. Reading is the
    * mirror: SRC_SEL=COPY_DATA_PERF(4) -> DST_SEL=COPY_DATA_DST_MEM_GRBM(1), whose own comment in sid.h:108
    * reads "sync across GRBM". Values from src/amd/common/sid.h:99-115.
    *
    *   ORBIS_READ_REGS_MODE=backdoor   the default now, because the plain path is known fatal here
    *   ORBIS_READ_REGS_MODE=reg        the plain path, kept so the fatal result stays reproducible */
   bool backdoor = true;
   {
      const char *const m = getenv("ORBIS_READ_REGS_MODE");
      if (m != NULL && m[0] == 'r')
         backdoor = false;
   }

   /* An entry says what amdgpu would make of the value it finds, so the log can print the DIFFERENCE rather
    * than a column of numbers somebody has to diff by hand. amdgpu's writes are read-modify-write, so the
    * target is expressed the same way: (found & ~clear) | set. `clear = 0xffffffff` is an absolute write, and
    * `set == 0 && clear == 0` means nobody has a target for this register - it is being read to be known. */
   struct regq {
      const char *name;
      uint32_t    reg;   /* BYTE offset, the units gfx7.json and the kernel headers use */
      uint32_t    clear;
      uint32_t    set;
      const char *who;   /* the amdgpu site that writes it, so a surprise has somewhere to be checked */
   };

   static const struct regq queries[] = {
      /* ⚠ THE TESSELLATION REGISTERS, ASKED BEFORE ANYTHING ELSE, BECAUSE THEY ARE THE ONLY ONES HERE THAT
       * HOLD AN ADDRESS.
       *
       * The remaining fault is a texture-cache READ at 0xff0000000 plus a small offset, and everything this
       * driver owns has been eliminated as its source: the frame's ~157000 dwords, all 588 MiB of mapped
       * memory scanned exactly, every user-data pointer followed, descriptor pools audited both ways, and a
       * run that REVOKED the rights on every freed range and still faulted at Sony's base rather than at one
       * of ours. So the value comes from hardware state, and a register is the only place left.
       *
       * But the rest of this table cannot hold it. GB_ADDR_CONFIG, the PA_SC and DB registers, SQ_CONFIG,
       * TCP_CHAN_STEER, the tile tables - they are configuration, not addresses. Reading all seventy-five
       * would not find 0x0ff00000, because there is nowhere in them for it to be. These three are the
       * exception: ac_emit_cp_tess_rings writes them as one USER-CONFIG sequence and the last of them
       * carries a base as `va >> 8`, which is exactly the spelling that makes 0xff0000000 invisible to every
       * memory scan.
       *
       * AND THE QUESTION IS NOT WHETHER WE WRITE THEM - the log proves we do, every boot. It is whether our
       * value is still there LATER. This file's own comment on them: they "are not reset by a submission and
       * hold whatever the last writer left there, across submissions AND across processes". Between our
       * submissions the system compositor drives the same GPU. Read them late - ORBIS_READ_REGS_AT - and
       * VGT_TF_MEMORY_BASE reading 0x0ff00000 when our preamble put our own address there names the defect
       * outright. */
      { "VGT_TF_RING_SIZE",        0x30938, 0, 0, "ac_emit_cp_tess_rings; ours is the factor ring in dwords" },
      { "VGT_HS_OFFCHIP_PARAM",    0x3093c, 0, 0, "ac_emit_cp_tess_rings; ours is 0x100 = 256 buffers" },
      { "VGT_TF_MEMORY_BASE",      0x30940, 0, 0, "ac_emit_cp_tess_rings; ours is our factor ring >> 8. If this "
                                                  "reads 0x0ff00000 it is SONY's and our write did not survive" },

      /* ---- THE PAYLOAD, ASKED FIRST. Five fields of GB_ADDR_CONFIG are SYNTHESISED by
       * orbis_gb_addr_config() and two are flagged there as unmeasured: ROW_SIZE ("a documented assumption")
       * and NUM_SHADER_ENGINES, which says ONE engine while drm_amdgpu_info_device.num_shader_engines says
       * TWO - a contradiction this port has carried since the field was guessed, in a register addrlib uses
       * for macro-tiling. One read settles all five, so it is asked before anything that might be fatal. */
      { "GB_ADDR_CONFIG",          0x98f8, 0,          0,          "cik_gpu_init: BONAIRE golden 0x12010001 + ROW_SIZE from MC_ARB_RAMCFG" },

      /* ---- CONTEXT SPACE, and this is the class already believed readable. RADV writes both itself, so a
       * value that disagrees with what we programmed means the read path is lying - worth knowing before any
       * config-space number is believed. Cheap, so it comes early. */
      { "PA_SC_RASTER_CONFIG",     0x28350, 0, 0, "RADV's own preamble, from info->pa_sc_raster_config" },
      { "PA_SC_RASTER_CONFIG_1",   0x28354, 0, 0, "RADV's own preamble" },

      /* ---- CONFIG SPACE. Nothing in RADV writes any of these on GFX7; amdgpu does, at init.
       *
       * That asymmetry is the whole reason this list exists: under amdgpu these hold values a kernel chose,
       * and under Sony's driver they hold whatever Sony's kernel chose. Same silicon, two configurations -
       * which is the only honest answer to "how can one chip behave two ways depending on the operating
       * system". It does not. It is SET UP two ways.
       *
       * Values are Kaveri's, because Liverpool is a KV-family part (this file's own ORBIS_CHIP=kaveri arm,
       * external_rev 0x61). ⚠ A DIFFERENCE IS NOT AUTOMATICALLY A DEFECT - Liverpool is semi-custom and Sony
       * may have chosen differently on purpose, and several of these are errata switches for bugs a
       * semi-custom part need not have. A difference is a QUESTION, and the register's name says what the
       * question is about. Ordered with the ones whose names touch the observed symptoms first. */
      { "VGT_CACHE_INVALIDATION",  0x88c4, 0xffffffff, 0x000000c2, "cik_gpu_init: VC_AND_TC|AUTO_INVLD_EN(ES_AND_GS_AUTO)" },
      { "PA_SC_ENHANCE",           0x8bf0, 0xffffffff, 0x00000001, "cik_gpu_init: ENABLE_PA_SC_OUT_OF_ORDER" },
      { "PA_SC_FIFO_SIZE",         0x8bcc, 0xffffffff, 0x98184020, "cik_gpu_init: 0x20/0x100/0x30/0x130" },
      { "CB_HW_CONTROL",           0x9a10, 0x00000000, 0x00018208, "cik_gpu_init: |0x18208 (its &~0x10000 is undone by the |)" },
      { "TA_CNTL_AUX",             0x9508, 0xffffffff, 0x00010000, "cik_gpu_init: 0x00010000" },
      { "DB_DEBUG",                0x9830, 0xffffffff, 0x00000000, "cik_gpu_init: 0" },
      { "DB_DEBUG2",               0x9834, 0xf00fffff, 0x00000400, "cik_gpu_init: &~0xf00fffff |0x400" },
      { "DB_DEBUG3",               0x9838, 0x0002021c, 0x00020200, "cik_gpu_init: &~0x0002021c |0x00020200" },
      { "PA_CL_ENHANCE",           0x8a14, 0xffffffff, 0x00000007, "cik_gpu_init: CLIP_VTX_REORDER_ENA|NUM_CLIP_SEQ(3)" },
      { "VGT_GS_VERTEX_REUSE",     0x88d4, 0xffffffff, 0x00000010, "cik_gpu_init: 16" },
      { "PA_SC_FORCE_EOV_MAX_CNTS",0x8b24, 0xffffffff, 0x00ff0fff, "cik_gpu_init: MAX_CLK_CNT(4095)|MAX_REZ_CNT(255)" },
      { "SQ_CONFIG",               0x8c00, 0xffffffff, 0x00000000, "cik_gpu_init: 1 then 0, and it lands on 0" },
      { "SX_DEBUG_1",              0x9060, 0xffffffff, 0x00000020, "cik_gpu_init: 0x20" },
      { "SPI_CONFIG_CNTL_1",       0x913c, 0xffffffff, 0x00000004, "cik_gpu_init: VTX_DONE_DELAY(4)" },
      { "CP_MEQ_THRESHOLDS",       0x8764, 0xffffffff, 0x00006030, "cik_gpu_init: MEQ1_START(0x30)|MEQ2_START(0x60)" },
      { "GC_USER_RB_BACKEND_DISABLE",0x9b7c,0x00ff0000, 0x00fc0000, "spectre_golden_registers" },
      { "TCP_CHAN_STEER_LO",       0xac0c, 0xffffffff, 0x54763210, "spectre_golden_registers" },
      { "TCP_CHAN_STEER_HI",       0xac10, 0,          0,          "not in the Kaveri tables - read to be known" },

      /* ⚠ LAST ON PURPOSE. Mesa marks this one PRIVILEGED on GFX6 through GFX8 - the only register in the
       * whole window that is (ac_pm4.c:45-46) - and it writes it through a COPY_DATA back door rather than a
       * SET_CONFIG_REG. If any single register in this list is going to be refused, it is this one, so it is
       * asked after everything else has already been printed. */
      { "SPI_CONFIG_CNTL",         0x9100, 0x00000000, 0x03000000, "cik_gpu_init: |= 0x03000000  (PRIVILEGED to write)" },
   };

   /* ⚠ THE SET THAT DOES NOT NEED A PRIVILEGED READ, AND IT IS THE BETTER QUESTION.
    *
    * RADV emits PKT3_CLEAR_STATE in its queue preamble (radv_queue.c:703-707) and then DELIBERATELY OMITS
    * initialising nine context registers, on the stated assumption that CLEAR_STATE has set them
    * (ac_cmdbuf.c:246-256, every one inside `if (!info->has_clear_state)`). has_clear_state is true for us:
    * `gfx_level >= GFX7 && gfx_level < GFX11` (ac_gpu_info.c:1101).
    *
    * AND MESA'S OWN COMMENT SAYS THAT ASSUMPTION IS KERNEL-DEPENDENT: "The mere presence of CLEAR_STATE in the
    * IB causes random GPU hangs on GFX6. CLEAR_STATE causes GPU hangs with the radeon kernel driver, so only
    * enable GFX7 CLEAR_STATE on amdgpu." (ac_gpu_info.c:1097-1099). What CLEAR_STATE loads comes from a buffer
    * the KERNEL allocates and points the RLC at - amdgpu does it at cik.c:6617-6620, writing
    * RLC_CLEAR_STATE_DESCRIPTOR_OFFSET and then its own buffer's address and size, and it builds the buffer's
    * contents itself at cik.c:6711-6761. The RLC registers are config space, so on this console that descriptor
    * points at SONY's clear-state buffer and there is no way to look at it.
    *
    * SO THESE NINE REGISTERS HOLD SONY'S CONTEXT DEFAULTS WHILE RADV IS WRITTEN AGAINST AMD'S. That is exactly
    * the shape of defect worth hunting - and unlike config space, CONTEXT SPACE IS READABLE: run 3 read
    * PA_SC_RASTER_CONFIG through the plain select.
    *
    * The first eleven entries are CONTROLS with values RADV writes ITSELF in the same preamble, so this set
    * validates its own read before any conclusion rests on it. A control that comes back wrong means the read
    * happened at the wrong time, not that the register is wrong. */
   static const struct regq clearstate[] = {
      /* ---- controls: RADV writes these, so they must read back exactly (ac_cmdbuf.c:246-330) */
      { "PA_SC_GENERIC_SCISSOR_TL", 0x28240, 0xffffffff, 0x80000000, "RADV writes WINDOW_OFFSET_DISABLE(1)" },
      { "PA_SC_GENERIC_SCISSOR_BR", 0x28244, 0xffffffff, 0x40004000, "RADV writes BR_X(16384)|BR_Y(16384)" },
      { "VGT_HOS_MAX_TESS_LEVEL",   0x28A18, 0xffffffff, 0x42800000, "RADV writes fui(64)" },
      { "VGT_GS_PER_ES",            0x28A54, 0xffffffff, 0x00000080, "RADV writes SI_GS_PER_ES = 128" },
      { "VGT_ES_PER_GS",            0x28A58, 0xffffffff, 0x00000040, "RADV writes 0x40" },
      { "VGT_MAX_VTX_INDX",         0x28400, 0xffffffff, 0xffffffff, "RADV writes ~0" },
      { "VGT_MIN_VTX_INDX",         0x28404, 0xffffffff, 0x00000000, "RADV writes 0" },
      { "VGT_INDX_OFFSET",          0x28408, 0xffffffff, 0x00000000, "RADV writes 0" },
      { "VGT_VERTEX_REUSE_BLK_CNTL",0x28C58, 0xffffffff, 0x0000000e, "RADV writes 14" },
      { "VGT_OUT_DEALLOC_CNTL",     0x28C5C, 0xffffffff, 0x00000010, "RADV writes 16" },
      { "VGT_INSTANCE_STEP_RATE_0", 0x28AA0, 0xffffffff, 0x00000001, "RADV writes 1" },

      /* ---- THE NINE RADV DOES NOT WRITE. The "amdgpu" column is what Mesa's own !has_clear_state branch
       * would have written, so a DIFFER here is Sony's clear state disagreeing with what RADV assumes. */
      { "VGT_HOS_MIN_TESS_LEVEL",   0x28A1C, 0xffffffff, 0x00000000, "OMITTED under has_clear_state; Mesa would write fui(0)" },
      { "PA_CL_NANINF_CNTL",        0x28820, 0xffffffff, 0x00000000, "OMITTED under has_clear_state; Mesa would write 0" },
      { "DB_SRESULTS_COMPARE_ST0",  0x28AC0, 0xffffffff, 0x00000000, "OMITTED under has_clear_state; Mesa would write 0" },
      { "DB_SRESULTS_COMPARE_ST1",  0x28AC4, 0xffffffff, 0x00000000, "OMITTED under has_clear_state; Mesa would write 0" },
      { "DB_PRELOAD_CONTROL",       0x28AC8, 0xffffffff, 0x00000000, "OMITTED under has_clear_state; Mesa would write 0" },
      { "VGT_PRIMITIVEID_RESET",    0x28A8C, 0xffffffff, 0x00000000, "OMITTED under has_clear_state; Mesa would write 0" },
      { "VGT_STRMOUT_BUFFER_CONFIG",0x28B98, 0xffffffff, 0x00000000, "OMITTED under has_clear_state; Mesa would write 0" },
      { "VGT_GS_PER_VS",            0x28A5C, 0xffffffff, 0x00000002, "OMITTED under has_clear_state; Mesa would write 0x2" },
      { "VGT_VTX_CNT_EN",           0x28AB8, 0xffffffff, 0x00000000, "OMITTED under has_clear_state; Mesa would write 0" },
   };

   /* ORBIS_READ_REGS_SET=clearstate asks the context-space question instead of the config-space one. It needs
    * ORBIS_READ_REGS_AT>=2 to mean anything: the state it is about is what the preamble left, and the default
    * read happens BEFORE the first submission. */
   const bool set_clearstate = (getenv("ORBIS_READ_REGS_SET") != NULL &&
                                getenv("ORBIS_READ_REGS_SET")[0] == 'c');

   /* ⚠ THE PRIVILEGE CONCLUSION ONCE RECORDED HERE WAS WRONG, AND THE REPLACEMENT IS NOT PROVEN EITHER.
    *
    * The note that used to sit here said config space is privileged for a user ring, on the evidence of one
    * crash. Two defects in that crash's asking are certain: COPY_DATA takes the register as a DWORD index and
    * every Mesa call site passes `reg >> 2` (ac_pm4.c:359, radv_cmd_buffer.c:5277 and :16543,
    * si_state_streamout.c:284) while this code passed BYTE offsets, and the tile tables were indexed
    * `0x9910 + i` when registers are four bytes apart. Mesa also states the rule outright: GFX6 through GFX8,
    * exactly one privileged register, R_009100_SPI_CONFIG_CNTL, and the rest of [0x8000, 0xB000) written from
    * a user ring with a plain PKT3_SET_CONFIG_REG (ac_pm4.c:45-46, :370-372).
    *
    * BOTH FIXED, AND THE TITLE STILL DIED - submission accepted, no value printed. So the correct statement
    * today is that something in a 71-register command buffer is fatal on this hardware and nobody knows what.
    * The ladder below is built to name it rather than to explain it. */

   /* THE LADDER, ordered least-known first. The named registers above, then the tile-mode tables, then the one
    * privileged register. The tables come late deliberately: they are a CONFIRMATION rather than a discovery -
    * the upload rung already round-trips a CPU pattern through a tiled image and reads it back through the
    * texture unit exactly, for eleven mip levels - so if something fatal lies in the tail, what it costs is a
    * cross-check and not an unknown. And SPI_CONFIG_CNTL is last of all, being the only register Mesa marks
    * privileged on this generation. */
   struct regq list[128];
   unsigned    n_list = 0;
   if (set_clearstate) {
      for (unsigned i = 0; i < ARRAY_SIZE(clearstate) && n_list < ARRAY_SIZE(list); ++i)
         list[n_list++] = clearstate[i];
      mesa_logi("orbis-drm: READ_REGS set=clearstate - %u CONTEXT registers, eleven of them controls RADV wrote "
                "itself. No privileged read anywhere in this set.", n_list);
   } else {
   const unsigned n_named = (unsigned)ARRAY_SIZE(queries);
   for (unsigned i = 0; i + 1 < n_named && n_list < ARRAY_SIZE(list); ++i)
      list[n_list++] = queries[i];
   for (unsigned i = 0; i < 32 && n_list < ARRAY_SIZE(list); ++i) {
      list[n_list].name = "GB_TILE_MODE";
      list[n_list].reg  = 0x9910 + i * 4;   /* four bytes apart, not one */
      list[n_list].clear = 0; list[n_list].set = 0;
      list[n_list].who = "cik_tiling_mode_table_init, 8-pipe branch; diff against orbis_tile_tables.h";
      ++n_list;
   }
   for (unsigned i = 0; i < 16 && n_list < ARRAY_SIZE(list); ++i) {
      list[n_list].name = "GB_MACROTILE_MODE";
      list[n_list].reg  = 0x9990 + i * 4;
      list[n_list].clear = 0; list[n_list].set = 0;
      list[n_list].who = "cik_tiling_mode_table_init, 8-pipe branch; diff against orbis_tile_tables.h";
      ++n_list;
   }
   if (n_list < ARRAY_SIZE(list))
      list[n_list++] = queries[n_named - 1];   /* SPI_CONFIG_CNTL, the privileged one, dead last */
   }

   const unsigned n_rungs = 5 + n_list;
   unsigned first = 0, last = n_rungs - 1;
   {
      const char *const s = getenv("ORBIS_READ_REGS_FIRST");
      const char *const e = getenv("ORBIS_READ_REGS_LAST");
      if (s != NULL) first = (unsigned)strtoul(s, NULL, 0);
      if (e != NULL) last  = (unsigned)strtoul(e, NULL, 0);
      if (last >= n_rungs) last = n_rungs - 1;
   }
   mesa_logi("orbis-drm: READ_REGS ladder of %u rungs, running [%u, %u]. Rung 0 is an EOP alone, rung 1 is the "
             "SCRATCH_REG0 round trip, rungs 2+ are one register each. A MISSING rung names what killed the "
             "title; ORBIS_READ_REGS_FIRST steps over it without a rebuild.", n_rungs, first, last);

   /* The destination and the completion flag live in the private page, past the fence label and its scratch; the
    * IB goes in the staging ring's first slot, which nothing has used yet at this point. */
   volatile uint32_t *const dst = (volatile uint32_t *)(uintptr_t)(orbis_arena_private + 1024);
   volatile uint32_t *const flag = (volatile uint32_t *)(uintptr_t)(orbis_arena_private + 512);
   uint32_t *const ib = (uint32_t *)(uintptr_t)(orbis_arena_private + 16384);

   const uint32_t control_pattern = 0xC0FFEE01u;
   bool           mechanism_proven = false;   /* the plain select works on a non-privileged register */
   bool           backdoor_reads    = false;   /* the backdoor returns something for one too */
   bool           backdoor_trusted  = false;   /* ...and it agrees with the plain select on the same register */
   uint32_t       raster_plain      = 0;
   unsigned       differ = 0, unread = 0;

   /* FOUR RUNGS OF CONTROL BEFORE ANY PRIVILEGED REGISTER, and the fourth one has a KNOWN ANSWER.
    *
    *   0  end-of-pipe alone                       is a standalone submission here survivable
    *   1  SCRATCH_REG0, plain select              the baseline, which run 2 already passed
    *   2  SCRATCH_REG0, backdoor select           does the backdoor work AT ALL, on a register the plain
    *                                              path can also reach
    *   3  PA_SC_RASTER_CONFIG, plain select       0x2a00161a, measured in run 3
    *   4  PA_SC_RASTER_CONFIG, backdoor select    MUST MATCH RUNG 3. This is the strongest control available:
    *                                              the backdoor is judged against a value THIS SAME RUN read
    *                                              through the other route, so agreement rules out both "the
    *                                              backdoor returns stale memory" and "it returns a plausible
    *                                              wrong register".
    *   5+ the register list, via ORBIS_READ_REGS_MODE (backdoor by default) */
   for (unsigned rung = first; rung <= last; ++rung) {
      const bool     is_eop_only = (rung == 0);
      const bool     is_scratch  = (rung == 1 || rung == 2);
      const bool     is_raster   = (rung == 3 || rung == 4);
      const bool     via_backdoor = is_eop_only ? false
                                  : (rung == 1 || rung == 3) ? false
                                  : (rung == 2 || rung == 4) ? true
                                                             : backdoor;
      const unsigned qi          = rung >= 5 ? rung - 5 : 0;
      const uint32_t reg         = is_scratch ? 0x30100u : (is_raster ? 0x28350u : list[qi].reg);
      const char    *const name  = is_eop_only ? "(EOP alone, no register)"
                                 : is_scratch  ? "SCRATCH_REG0 (control)"
                                 : is_raster   ? "PA_SC_RASTER_CONFIG (control)"
                                               : list[qi].name;

      *dst = 0xDEADBEEFu;
      *flag = 0;

      uint32_t *dw = ib;

      /* The control's pattern, written the way RADV writes user-config registers. SET_UCONFIG_REG's body takes
       * (reg - 0x30000) >> 2, which is a different encoding from COPY_DATA's plain dword index - and getting
       * both right in one packet stream is part of what the control proves. */
      if (is_scratch) {
         *dw++ = ORBIS_PM4_TYPE3(0x79 /* IT_SET_UCONFIG_REG */, 2);
         *dw++ = (0x30100u - 0x30000u) >> 2;
         *dw++ = control_pattern;
      }

      /* ⚠ A ROUND TRIP ON THE REGISTER ITSELF, BECAUSE THREE ZEROS ARE NOT AN ANSWER.
       *
       * ⚠ ANSWERED, AND THE ANSWER IS THAT THIS ROUTE IS BLIND. All three round trips FAILED: written and
       * read back in the same packet stream, they returned 0x00000000 - including the base, written with
       * the very value radv_emit_tess_factor_ring had programmed. SCRATCH_REG0 at 0x30100 round trips
       * perfectly through the same write path, so the mechanism works and this register block is what does
       * not answer.
       *
       * So every zero previously read from these three was a NON-ANSWER, and nothing may be concluded from
       * them in either direction - not "our write does not survive", not "Sony's value is back". The rung
       * stays because that conclusion is worth keeping, and because the next person will otherwise read the
       * zeros and believe them.
       *
       * ⚠ AND THE CLAIM THIS PORT RESTS ON IS THEREFORE STILL UNMEASURED: that user-config registers "hold
       * whatever the last writer left there, across submissions AND across processes". It is the entire
       * justification for programming the tessellation registers ourselves. It has never been tested, this
       * instrument cannot test it, and it should be read as an assumption wherever it appears.
       *
       * VGT_TF_RING_SIZE, VGT_HS_OFFCHIP_PARAM and VGT_TF_MEMORY_BASE all read 0x00000000 at submission
       * #6100, through the plain select AND through the backdoor, with the world rendering. That is either
       * "our preamble's writes do not survive" or "these registers do not answer this way and the zero is a
       * non-answer wearing the costume of a result", and nothing in that run separates the two.
       *
       * SCRATCH_REG0 cannot settle it. It proves the 0x30000 BLOCK is reachable, not that 0x3093x is - and
       * the probe is a submission of its own, outside RADV's, so it may simply not be looking at the same
       * context state at all. That possibility undoes the claim this file has carried since the tess rings
       * were first programmed: that user-config registers "hold whatever the last writer left there, across
       * submissions AND across processes".
       *
       * So the ladder writes the register in the same packet stream it reads it from, exactly as it does for
       * SCRATCH_REG0. The pattern is 0xC0FFEE01 for the two that are plain numbers; for the BASE it is the
       * value we actually want there, so a run where the write sticks leaves the register CORRECT rather
       * than corrupted.
       *
       *   the value comes back  -> the register is reachable this way, the zeros before it were real, and
       *                            it is our preamble's write that does not survive
       *   zero comes back       -> all of it was a non-answer, and the question needs a different route
       *
       * ORBIS_REG_ROUNDTRIP=1. */
      const bool roundtrip = rung >= 5 && reg >= 0x30938u && reg <= 0x30940u &&
                             getenv("ORBIS_REG_ROUNDTRIP") != NULL;
      if (roundtrip) {
         const uint32_t want = (reg == 0x30940u) ? orbis_roundtrip_base : control_pattern;
         *dw++ = ORBIS_PM4_TYPE3(0x79 /* IT_SET_UCONFIG_REG */, 2);
         *dw++ = (reg - 0x30000u) >> 2;
         *dw++ = want;
      }

      if (!is_eop_only) {
         const uint64_t dva = (uint64_t)(uintptr_t)dst;
         /* Two routes, one packet shape. The register goes in dwords 2-3 and the destination address in 4-5
          * either way; only the selects change, which is what makes the backdoor testable against the plain
          * path on a register both can reach. */
         const uint32_t src_sel = via_backdoor ? 4u /* COPY_DATA_PERF */    : 0u /* COPY_DATA_REG */;
         const uint32_t dst_sel = via_backdoor ? 1u /* DST_MEM_GRBM, "sync across GRBM" */
                                               : 5u /* COPY_DATA_DST_MEM */;
         *dw++ = ORBIS_PM4_TYPE3(0x40 /* PKT3_COPY_DATA */, 5);
         *dw++ = src_sel | (dst_sel << 8) | (1u << 20) /* WR_CONFIRM */;
         *dw++ = reg >> 2;   /* DWORD index. Mesa: ac_pm4.c:359, radv_cmd_buffer.c:16543 */
         *dw++ = 0;
         *dw++ = (uint32_t)dva;
         *dw++ = (uint32_t)(dva >> 32);
      }

      dw = orbis_emit_eop(dw, (uint64_t)(uintptr_t)flag, 1u, ORBIS_EOP_DST_SEL_MC | ORBIS_EOP_DATA_SEL_32BIT |
                                                          ((uint32_t)ORBIS_EOP_INT_SEL << ORBIS_EOP_INT_SEL_SHIFT));
      /* ONE NOP THAT COVERS THE REMAINDER, not a run of them: a NOP header declares how many dwords follow it,
       * so a sequence of headers each claiming one body dword leaves the CP parsing a body dword as a header. */
      dw = orbis_pad_ib(dw, ib);

      /* SAID BEFORE THE SUBMISSION, NOT AFTER. If this rung is the fatal one there will be no line after it,
       * so the announcement has to be the thing that survives. */
      if (is_eop_only)
         mesa_logi("orbis-drm: READ_REGS rung %2u ASKING: %s", rung, name);
      else
         mesa_logi("orbis-drm: READ_REGS rung %2u ASKING: %-28s [byte 0x%05x -> dword 0x%05x] via %s", rung,
                   name, reg, reg >> 2, via_backdoor ? "BACKDOOR (PERF->GRBM)" : "plain REG select");

      void    *dcb[1] = {ib};
      uint32_t bytes[1] = {(uint32_t)((dw - ib) * 4)};
      sceGnmFlushGarlic();
      const int32_t err = sceGnmSubmitCommandBuffers(1, dcb, bytes, NULL, NULL);
      if (err != 0) {
         mesa_loge("orbis-drm: READ_REGS rung %2u REFUSED at submit: 0x%08x - and a refusal is not a fault, so "
                   "the ladder stops rather than guessing what state the ring is in", rung, (unsigned)err);
         return;
      }

      /* Bounded, and short: a rung that does not complete in 200 ms is a rung the GPU is not going to finish,
       * and hanging here would cost the answers already collected. */
      for (unsigned spin = 0; spin < 20000 && *flag == 0; ++spin)
         sceKernelUsleep(10);

      if (*flag == 0) {
         mesa_loge("orbis-drm: READ_REGS rung %2u %-26s NO END-OF-PIPE IN 200 ms - the GPU took the buffer and "
                   "did not finish it. Everything above this line stands; nothing below it will be asked.",
                   rung, name);
         return;
      }

      if (is_eop_only) {
         mesa_logi("orbis-drm: READ_REGS rung %2u OK - a standalone command buffer runs to end-of-pipe at this "
                   "point in init. So whatever killed the batch was in its CONTENTS, not in the fact of "
                   "submitting one here.", rung);
         continue;
      }

      const uint32_t v = *dst;

      if (roundtrip) {
         const uint32_t want = (reg == 0x30940u) ? orbis_roundtrip_base : control_pattern;
         if (v == want)
            mesa_logi("orbis-drm: READ_REGS rung %2u %-26s ROUND TRIP OK - wrote 0x%08x one packet earlier "
                      "and read it back, so this register IS reachable here and the 0x00000000 it held "
                      "before was real. Our preamble's write is what does not survive.",
                      rung, name, want);
         else
            mesa_loge("orbis-drm: READ_REGS rung %2u %-26s ROUND TRIP FAILED - wrote 0x%08x one packet "
                      "earlier and read back 0x%08x. This register does not answer through this route, so "
                      "every zero read from it is a non-answer rather than a measurement.",
                      rung, name, want, v);
         continue;
      }

      if (is_scratch) {
         const char *const route = via_backdoor ? "BACKDOOR" : "plain";
         if (v == control_pattern) {
            if (!via_backdoor)
               mechanism_proven = true;
            else
               backdoor_reads = true;
            mesa_logi("orbis-drm: READ_REGS rung %2u CONTROL PASSED (%s) - SCRATCH_REG0 read back 0x%08x, the "
                      "pattern written one packet earlier. The packet, the dword addressing and the destination "
                      "are all right on this route.", rung, route, v);
         } else {
            mesa_loge("orbis-drm: READ_REGS rung %2u ⚠ CONTROL FAILED (%s) - SCRATCH_REG0 read back 0x%08x, "
                      "wanted 0x%08x. Nothing read by THIS route below is evidence.", rung, route, v,
                      control_pattern);
         }
         continue;
      }

      if (is_raster) {
         if (!via_backdoor) {
            raster_plain = v;
            mesa_logi("orbis-drm: READ_REGS rung %2u PA_SC_RASTER_CONFIG via plain select = 0x%08x   (run 3 read "
                      "0x2a00161a here; this is the value the backdoor is about to be judged against)", rung, v);
         } else if (raster_plain != 0 && v == raster_plain) {
            backdoor_trusted = true;
            mesa_logi("orbis-drm: READ_REGS rung %2u ⭐ BACKDOOR AGREES - 0x%08x through PERF->GRBM matches "
                      "0x%08x through the plain select, on the same register in the same run. The backdoor is "
                      "not returning stale memory and not returning a different register, so config-space "
                      "values below can be believed.", rung, v, raster_plain);
         } else {
            mesa_loge("orbis-drm: READ_REGS rung %2u ⚠ BACKDOOR DISAGREES - 0x%08x through PERF->GRBM against "
                      "0x%08x through the plain select. THE BACKDOOR IS NOT A READ PATH: every config-space "
                      "value below is whatever this mechanism happens to produce, not what the chip holds.",
                      rung, v, raster_plain);
         }
         continue;
      }

      if (v == 0xDEADBEEFu) {
         ++unread;
         mesa_logw("orbis-drm: READ_REGS rung %2u %-26s NOT WRITTEN - end-of-pipe fired but the fill pattern is "
                   "still there, so the CP completed the buffer while declining this one read   (%s)",
                   rung, name, list[qi].who);
         continue;
      }

      if (list[qi].clear == 0 && list[qi].set == 0) {
         mesa_logi("orbis-drm: READ_REGS rung %2u %-26s sony=0x%08x   (no amdgpu target; %s)", rung, name, v,
                   list[qi].who);
      } else {
         const uint32_t want = (v & ~list[qi].clear) | list[qi].set;
         const bool     same = (want == v);
         if (!same)
            ++differ;
         mesa_logi("orbis-drm: READ_REGS rung %2u %-26s sony=0x%08x amdgpu=0x%08x  %s   (%s)", rung, name, v,
                   want, same ? "same" : "*** DIFFER ***", list[qi].who);
      }

      /* GB_ADDR_CONFIG decoded on the spot, because the point of reading it is the five fields
       * orbis_gb_addr_config() synthesises and a raw hex word does not settle an argument about
       * NUM_SHADER_ENGINES. The decode lives next to the field shifts it uses, further down this file. */
      if (reg == 0x98f8)
         orbis_log_gb_addr_config_read(v);
   }

   mesa_logi("orbis-drm: READ_REGS ladder finished: %u register(s) differ from what amdgpu would set, %u were "
             "declined. plain select %s, backdoor %s, backdoor cross-checked %s", differ, unread,
             mechanism_proven ? "PROVEN" : "unproven",
             backdoor_reads ? "returns data" : "returned nothing usable",
             backdoor_trusted ? "AGAINST THE PLAIN SELECT - the config-space values above are real"
                              : "NOT confirmed - treat the config-space values above as unverified");
}

/* ⚠ THIS RAN TWICE AT ONCE AND ITS GUARD DID NOT NOTICE.
 *
 * The guard used to be `static bool done` set BEFORE the work, with no lock and no atomic. Two
 * threads in vkCreateDevice at the same moment: the first sees false, sets it true and starts
 * taking direct memory and mapping it; the second sees true and RETURNS IMMEDIATELY, on
 * orbis_va_base and orbis_va_end that the first has not written yet. A device then exists whose
 * address window is zero or half-written, and everything it allocates goes somewhere that is not
 * the arena.
 *
 * ⚠ AND IT IS EXACTLY THE PATH THAT SEPARATES THE PASSING TESTS FROM THE HANGING ONE.
 * object_management.multithreaded_per_thread_resources and _shared_resources pass - one device,
 * many threads. _per_thread_device hangs - a device PER THREAD, which is the only test in the
 * suite that calls vkCreateDevice from several threads at once. It also passes on the laptop,
 * because this whole function is inside #if defined(__PS4__) and the host never runs it.
 *
 * So: a proper once-init. The state is set AFTER the work, and a second caller waits for the first
 * to finish rather than racing past it.
 */
static simple_mtx_t orbis_arena_lock = SIMPLE_MTX_INITIALIZER;
static int orbis_arena_state; /* 0 = untouched, 1 = up, -1 = tried and failed */

static int orbis_arena_setup_once(void);

static int
orbis_arena_setup(void)
{
   simple_mtx_lock(&orbis_arena_lock);

   if (orbis_arena_state == 0) {
      const int r = orbis_arena_setup_once();
      /* Written from the result rather than from reaching the end: the body has three exits and two
         of them are failures. */
      orbis_arena_state = (r == 0 && orbis_va_end > orbis_va_base) ? 1 : -1;
   }

   const int ret = orbis_arena_state > 0 ? 0 : -ENOMEM;
   simple_mtx_unlock(&orbis_arena_lock);
   return ret;
}

static int
orbis_arena_setup_once(void)
{

   /* What the console has, said out loud before anything is taken from it. A refusal further down means one of
    * two different things - the pool is smaller than we asked, or it is fragmented - and this number is what
    * tells them apart. */
   const size_t direct_total = sceKernelGetDirectMemorySize();
   /* ⚠ A POSITIVE MARKER, BECAUSE AN ABSENT LOG LINE PROVES NOTHING.
    *
    * A run came back with the CHAIN bit still set and no "cleared CHAIN" line, and there were two explanations
    * with identical evidence: the patch did not fire, or the console ran the previous eboot. Absence of a line
    * cannot tell those apart, so the arm now states which knobs it SAW and when it was compiled - which makes
    * "the old binary ran" a readable fact instead of a suspicion.
    *
    * __DATE__/__TIME__ deliberately: this file is recompiled by every build.sh run (the scratch tree is
    * re-cloned when src/ changes), so the stamp moves whenever the driver does - unlike the title's own stamp,
    * which belongs to the title. */
   mesa_logi("orbis-drm: arm built %s %s; knobs: TRACE=%s DUMP_SUBMIT=%s CMASK=%s TILE_MODE=%s "
             "ARENA_MIB=%s",
             __DATE__, __TIME__,
             getenv("ORBIS_DRM_TRACE") ? getenv("ORBIS_DRM_TRACE") : "unset",
             getenv("ORBIS_DUMP_SUBMIT") ? getenv("ORBIS_DUMP_SUBMIT") : "unset",
             getenv("ORBIS_CMASK") ? getenv("ORBIS_CMASK") : "unset",
             getenv("ORBIS_TILE_MODE") ? getenv("ORBIS_TILE_MODE") : "unset",
             getenv("ORBIS_ARENA_MIB") ? getenv("ORBIS_ARENA_MIB") : "unset");

   mesa_logi("orbis-drm: direct memory pool is %llu MiB",
             (unsigned long long)(direct_total / (1024 * 1024)));

   uint64_t sizes[ARRAY_SIZE(orbis_arena_ladder) + 1];
   unsigned n_sizes = 0;
   const char *const forced = getenv("ORBIS_ARENA_MIB");
   if (forced != NULL) {
      const unsigned long mib = strtoul(forced, NULL, 10);
      if (mib != 0) {
         sizes[n_sizes++] = (uint64_t)mib * 1024 * 1024;
         mesa_logi("orbis-drm: ORBIS_ARENA_MIB=%lu - the ladder is skipped", mib);
      }
   }
   if (n_sizes == 0)
      for (unsigned i = 0; i < ARRAY_SIZE(orbis_arena_ladder); ++i)
         sizes[n_sizes++] = orbis_arena_ladder[i];

   off_t    phys = 0;
   void    *va = NULL;
   uint64_t arena = 0;
   for (unsigned i = 0; i < n_sizes; ++i) {
      const uint64_t want = sizes[i];
      /* ⚠ WHICH BUS THE GPU USES FOR EVERYTHING, and why it is suddenly the first question.
       *
       * This arena has always been WB_ONION - cached and coherent for both processors, correct and
       * chosen deliberately. What it also means is that EVERY GPU access to every surface crosses
       * the coherent bus and snoops the CPU's caches on the way, where GARLIC would bypass them.
       * That is what GARLIC is for on this console.
       *
       * It matters because the SAME OpenGothic build, the same save and the same scene ran at 30 fps
       * on a native GNM backend and runs at 15 here - with two thirds of the frame inside the game's
       * own simulation, code this driver never touches. A driver cannot make someone else's
       * arithmetic slower except through the memory system, and this is the memory system.
       *
       *   onion      (default)  cached, coherent, what has always shipped
       *   wc-garlic             GPU stops snooping; CPU writes are write-combined and CPU reads
       *                         from it become an order of magnitude slower
       *   wb-garlic             GPU stops snooping; the CPU keeps its cache, and coherence becomes
       *                         this driver's problem rather than the hardware's
       *
       * ⚠ NEITHER GARLIC MODE IS SAFE YET. Write-combining needs a store fence before the GPU reads
       * what the CPU wrote, and wb-garlic needs explicit cache maintenance that nothing here does.
       * These are named experiments, not configurations, and the default is unchanged. */
      const char *const bus = getenv("ORBIS_ARENA_BUS");
      const int type = (bus == NULL)                    ? ORBIS_KERNEL_WB_ONION
                       : !strcmp(bus, "wc-garlic")      ? ORBIS_KERNEL_WC_GARLIC
                       : !strcmp(bus, "wb-garlic")      ? ORBIS_KERNEL_WB_GARLIC
                                                        : ORBIS_KERNEL_WB_ONION;

      int32_t err = sceKernelAllocateDirectMemory(0, direct_total, want, ORBIS_DIRECT_ALIGN, type, &phys);
      if (type != ORBIS_KERNEL_WB_ONION && err == 0)
         mesa_logw("orbis-drm: ORBIS_ARENA_BUS=%s - the arena is on the GARLIC bus (type 0x%x). The GPU "
                   "stops snooping the CPU's caches; correctness now depends on fences this driver does "
                   "not yet emit. An experiment.",
                   bus, type);
      else if (bus != NULL && type == ORBIS_KERNEL_WB_ONION)
         mesa_logw("orbis-drm: ORBIS_ARENA_BUS=%s is not a bus I know - staying on WB_ONION", bus);
      if (err != 0) {
         /* Not an error yet: the next rung down is the point of the ladder. It becomes an error when the last
          * rung fails, and then the log carries every refusal that led there. */
         mesa_logi("orbis-drm: sceKernelAllocateDirectMemory(%llu MiB) -> 0x%08x, trying smaller",
                   (unsigned long long)(want / (1024 * 1024)), err);
         continue;
      }

      err = sceKernelMapDirectMemory(&va, want, ORBIS_GRAPHICS_PROT, 0, phys, ORBIS_DIRECT_ALIGN);
      if (err != 0) {
         mesa_logi("orbis-drm: sceKernelMapDirectMemory(%llu MiB) -> 0x%08x, trying smaller",
                   (unsigned long long)(want / (1024 * 1024)), err);
         sceKernelReleaseDirectMemory(phys, want);
         va = NULL;
         continue;
      }

      arena = want;
      break;
   }

   if (arena == 0) {
      mesa_loge("orbis-drm: no arena could be reserved - every size on the ladder was refused");
      return -ENOMEM;
   }

   /* ⚠ THE ARENA MUST BE BIGGER THAN THE SLICE TAKEN OUT OF IT, and ORBIS_ARENA_MIB lets a person ask for one
    * that is not. The ladder's own smallest rung is 128 MiB against a 32 MiB private slice, so this cannot fire
    * by itself - but the knob exists to be turned during a bisect, and `ORBIS_ARENA_MIB=16` would leave
    * orbis_va_base ABOVE orbis_va_end. Nothing downstream checks that: the VA allocator would refuse every
    * request as out of space, and the self-test below would write through orbis_va_end - page, which is inside
    * the arm's own fence ring. Refuse here, naming both numbers, rather than produce a device that is subtly
    * dead. */
   if (arena <= ORBIS_ARENA_PRIVATE + orbis_page_size()) {
      mesa_loge("orbis-drm: an arena of %llu MiB cannot hold the arm's own %llu MiB private slice - nothing "
                "would be left for RADV. Raise ORBIS_ARENA_MIB.",
                (unsigned long long)(arena / (1024 * 1024)),
                (unsigned long long)(ORBIS_ARENA_PRIVATE / (1024 * 1024)));
      sceKernelReleaseDirectMemory(phys, arena);
      return -ENOMEM;
   }

   /* The private slice comes off the front; everything below and everything RADV is told refers to what is
    * left. Deliberately at the FRONT rather than the back: an off-by-one in the VA allocator then collides
    * with our own structures immediately and loudly, rather than at the far end of 256 MiB after hours. */
   orbis_arena_phys = (uint64_t)phys;
   orbis_arena_map_base = (uint64_t)(uintptr_t)va;
   orbis_arena_private = (uint64_t)(uintptr_t)va;
   orbis_va_base = orbis_arena_private + ORBIS_ARENA_PRIVATE;
   orbis_va_end = (uint64_t)(uintptr_t)va + arena;

   /* ⚠ A DECOY AT THE FAULTING ADDRESS, TO TURN A FATAL FAULT INTO AN OBSERVABLE ONE.
    *
    * `ORBIS_DECOY_VA=0xff0000000[:<MiB>]` maps real memory at an address nothing should be reading, fills it with
    * a marker byte, and lets the read succeed. The defect is a GPU page fault on VA 0xff0000000 + up to 0x1c000,
    * read by the texture cache, and it kills the process - so everything downstream of it is unobservable: the
    * frame never finishes, the picture is never drawn, and no later instrument in the same run gets to speak.
    *
    * WHAT IT BUYS, and it is not a fix:
    *   - the frame COMPLETES, so the picture says which draw sampled the decoy. 0x5a fills every byte, so a
    *     texture read through it is a flat mid-grey and a vertex read through it is visible garbage geometry.
    *   - every check and counter later in the run gets to run at all.
    *   - and if the crash simply moves to a different address, that is a finding too: the number is not one wrong
    *     pointer but a class of them.
    *
    * 2 MiB by default, which covers every offset observed (0x2000, 0x11000, 0x15000, 0x16000, 0x1c000) with room
    * to spare. SMALL ON PURPOSE: sceKernelMapDirectMemory COMMITS EAGERLY on this console - an over-large request
    * has frozen the machine before - so this asks for megabytes, never gigabytes.
    *
    * AND IT NEVER FAILS QUIETLY. If the kernel will not put the mapping at the requested address, that is said,
    * with what it gave instead, because a decoy somewhere else is worse than no decoy: the run would look
    * protected and would not be.
    *
    * Deliberately AFTER the arena, so a refusal here cannot cost the arena, and deliberately not registered as a
    * BO: RADV must never allocate from it. It exists to be read by mistake. */
   {
      const char *const decoy = getenv("ORBIS_DECOY_VA");
      if (decoy != NULL) {
         char          *end = NULL;
         const uint64_t at = strtoull(decoy, &end, 0);
         const uint64_t mib = (end != NULL && *end == ':') ? strtoull(end + 1, NULL, 10) : 2;
         const uint64_t len = (mib > 0 ? mib : 1) * 1024 * 1024;
         off_t          dphys = 0;
         int32_t        err = sceKernelAllocateDirectMemory(0, direct_total, len, ORBIS_DIRECT_ALIGN,
                                                           ORBIS_KERNEL_WB_ONION, &dphys);
         if (err != 0) {
            mesa_loge("orbis-drm: DECOY refused physical memory: 0x%08x - the fault will still be fatal", err);
         } else {
            void *dva = (void *)(uintptr_t)at;
            err = sceKernelMapDirectMemory(&dva, len, ORBIS_GRAPHICS_PROT, 0, dphys, ORBIS_DIRECT_ALIGN);
            if (err != 0) {
               mesa_loge("orbis-drm: DECOY could not be mapped at 0x%" PRIx64 ": 0x%08x - the fault will still be "
                         "fatal",
                         at, err);
               sceKernelReleaseDirectMemory(dphys, len);
            } else if ((uint64_t)(uintptr_t)dva != at) {
               mesa_loge("orbis-drm: DECOY landed at 0x%" PRIx64 " and NOT at the requested 0x%" PRIx64
                         " - it protects nothing; treat this run as undecoyed",
                         (uint64_t)(uintptr_t)dva, at);
            } else {
               memset(dva, 0x5a, (size_t)len);
               mesa_logi("orbis-drm: DECOY %llu MiB mapped at 0x%" PRIx64 " and filled with 0x5a - a read that "
                         "would have faulted now returns mid-grey",
                         (unsigned long long)(len / (1024 * 1024)), at);
            }
         }
      }
   }

   /* The window reported here is what is LEFT after the arm's private slice, which is why its base is not the
    * mapping's base - the vq lines a few lines down show both. */
   mesa_logi("orbis-drm: arena %llu MiB, RADV's window 0x%" PRIx64 "..0x%" PRIx64 " (phys 0x%llx)",
             (unsigned long long)(arena / (1024 * 1024)), orbis_va_base, orbis_va_end,
             (unsigned long long)phys);

   /* ⚠ WHERE THE ARENA LANDS IS THE KERNEL'S DECISION, AND IT MEASURED 0x200400000 - so what matters is not
    * whether it is high but whether it is CONTIGUOUS IN ONE 4 GiB WINDOW. amdgpu_sw_info_address32_hi names a
    * single window for every RADEON_FLAG_32BIT allocation, so an arena straddling a 4 GiB boundary can only
    * serve them from one side of it, and the VA allocator clamps to that side. Worth a warning, because the
    * usable 32-bit space then silently becomes smaller than the arena. */
   if ((orbis_va_base >> 32) != ((orbis_va_end - 1) >> 32))
      mesa_logw("orbis-drm: the arena straddles a 4 GiB boundary - only the part in window 0x%x can serve "
                "RADEON_FLAG_32BIT allocations",
                (unsigned)(orbis_va_base >> 32));
   orbis_fence_label = (volatile uint32_t *)(uintptr_t)orbis_arena_private;
   orbis_fence_scratch = (volatile uint32_t *)(uintptr_t)(orbis_arena_private + 64);
   /* The labels sit in the first page; the ring of fence command buffers starts after it and is 32 MiB
    * (ORBIS_FENCE_CB_SLOTS x ORBIS_FENCE_CB_BYTES), which is what ORBIS_ARENA_PRIVATE reserves. */
   orbis_fence_cb = (uint32_t *)(uintptr_t)(orbis_arena_private + 16384);
   /* The base is page-aligned and the stride is a multiple of 256, so every slot starts on the 256-byte
    * boundary the device asks for. Asserted rather than assumed: the alignment is the whole point of the
    * layout, and ignoring the number this device itself reports already cost one flash. */
   static_assert(ORBIS_FENCE_CB_BYTES % 256 == 0, "staging slots must start 256-byte aligned");
   *orbis_fence_label = 0;
   *orbis_fence_scratch = 0;
   /* One word for the progress marker, past the label and the scratch, inside the arm's own slice. */
   orbis_progress_word = (volatile uint32_t *)(uintptr_t)(orbis_arena_private + 128);
   *orbis_progress_word = 0;
   orbis_retired_word = (volatile uint32_t *)(uintptr_t)(orbis_arena_private + 192);
   *orbis_retired_word = 0;
   orbis_drain_word = (volatile uint32_t *)(uintptr_t)(orbis_arena_private + 224);
   *orbis_drain_word = 0;


   mesa_logi("orbis-drm: page size %" PRIu64 " bytes, as getpagesize() reports it", orbis_page_size());
   mesa_logi("orbis-drm: fence label 0x%" PRIx64 ", fence CB 0x%" PRIx64 ", RADV's window starts 0x%" PRIx64,
             (uint64_t)(uintptr_t)orbis_fence_label, (uint64_t)(uintptr_t)orbis_fence_cb, orbis_va_base);

   /* ⚠ A SELF-TEST OF THE ARENA ITSELF, because the first console run left two facts in contradiction: RADV
    * wrote PM4 into IBs inside this arena and vkCreateDevice succeeded, yet the harness's own write to a
    * mapping out of the same arena did not come back. One of those is wrong, and RADV is not in this loop -
    * so if this passes, the arena is writable and the fault is in what the harness was handed; if it faults,
    * the arena mapping is the problem and everything above it is noise.
    *
    * Deliberately at the BASE and at the LAST page: a partially-committed mapping - which is a real
    * possibility on a platform where mmap populates eagerly - passes at one end and fails at the other. */
   volatile uint32_t *probe_lo = (volatile uint32_t *)(uintptr_t)orbis_va_base;
   volatile uint32_t *probe_hi =
      (volatile uint32_t *)(uintptr_t)(orbis_va_end - orbis_page_size());
   mesa_logi("orbis-drm: arena self-test: writing 0x%" PRIx64 " and 0x%" PRIx64,
             (uint64_t)(uintptr_t)probe_lo, (uint64_t)(uintptr_t)probe_hi);
   *probe_lo = 0xA5A50001u;
   *probe_hi = 0xA5A50002u;
   mesa_logi("orbis-drm: arena self-test: read back 0x%08x and 0x%08x - %s", *probe_lo, *probe_hi,
             (*probe_lo == 0xA5A50001u && *probe_hi == 0xA5A50002u) ? "OK" : "MISMATCH");
   /* THE THREE OFFSETS THE LAST RUN MEASURED, and they contradicted each other: +16K, +112K and +192K were
    * writable while +128K faulted "page not present". If the arena is one mapping, all four answers here are
    * the same range. If they are not, the arena is not what sceKernelMapDirectMemory reported. */
   /* ONE query, once, at the base. The four-offset version existed to prove the arena was a single mapping
    * while a hole in it was being hunted; that hole was radv_amdgpu_winsys_bo_destroy's munmap and it is fixed.
    * What stays useful is the kernel's own statement that this range is direct memory and committed. */
   orbis_virtual_query(orbis_va_base, "arena");

   mesa_logi("orbis-drm: address32_hi = 0x%x, derived from the arena rather than assumed",
             (unsigned)(orbis_va_base >> 32));

   /* ⚠ AND NOW GIVE RADV'S WINDOW BACK, SO IT STARTS UNMAPPED - THE WHOLE POINT OF THIS PORT'S REWORK.
    *
    * The maintainer's decision (2026-08-11): behave as Linux behaves, and patch Mesa only where behaving the same is
    * impossible. Everything this hunt found today was one deviation with four faces - the arena stayed mapped for
    * the life of the process, so:
    *
    *   a freed buffer's address kept reading and WRITING fine, instead of faulting
    *   an unwritten descriptor slot held the last tenant's bytes, instead of a kernel's zeros
    *   a stale descriptor sailed through every "is this address ours" check, because a freed address IS ours
    *   and a GPU-side read-only mapping could not be enforced at all
    *
    * On amdgpu, AMDGPU_VA_OP_UNMAP removes pages. A stale access then faults IMMEDIATELY, at the instruction that
    * makes the mistake, and the bug is a line in a log rather than a week of instruments. That is the behaviour to
    * copy, and this console can: sceKernelMprotect exists (libkernel.h:325), so access can be taken away from a
    * range without giving up the address - which is what amdgpu's page tables do and what MAP_FIXED cannot.
    *
    * So the arena is mapped once to establish its address and its physical range, and then RADV's window is
    * unmapped again. From here every VA_OP_MAP maps its own range from the arena's physical memory at exactly the
    * address RADV asked for, and every VA_OP_UNMAP takes it away. The private slice - fence label, scratch, the
    * staging command buffers - stays mapped, because it is this arm's own and nothing hands it out.
    *
    * ORBIS_FLAT_ARENA=1 restores the old behaviour, for a run that needs to compare against every measurement made
    * before today. */
   if (getenv("ORBIS_FLAT_ARENA") != NULL) {
      mesa_logw("orbis-drm: ORBIS_FLAT_ARENA=1 - the whole arena stays mapped, as it did before 2026-08-11. Stale "
                "accesses will be silent again; this is for comparing against older runs, not for playing.");
      return 0;
   }

   /* ⚠ PROTECTED, NOT UNMAPPED - AND THE DIFFERENCE COST A RUN.
    *
    * The first version really did unmap RADV's window. It worked: the window went away, buffers mapped their own
    * ranges, submissions executed. Then OpenGothic could not load a save:
    *
    *     loading error: Cannot find script symbol SELF, OTHER, ITEM, VICTIM, or HERO! Cannot proceed!
    *
    * Not a graphics failure at all - the Daedalus script data was gone. Unmapping the window made that address space
    * FREE, so the title's own allocator was handed part of it for game data, and the next MAP_FIXED for a buffer
    * silently replaced it. MAP_FIXED destroys whatever is at the address; that is what it is for.
    *
    * The old flat arena had this right for a reason nobody had written down: mapping the whole window up front
    * RESERVES the address space. What was wrong was only that the pages stayed ACCESSIBLE.
    *
    * So: keep one mapping, and take the ACCESS away instead. sceKernelMprotect with no access gives what amdgpu
    * gives - a stale read or write faults at the instruction that makes it - while the addresses stay ours and
    * nothing else can move in. It is also far cheaper than map/unmap churn: no kernel object comes or goes. */
   {
      const uint64_t win = orbis_va_end - orbis_va_base;
      const int32_t  err = sceKernelMprotect((void *)(uintptr_t)orbis_va_base, (size_t)win, ORBIS_PROT_NONE);
      if (err != 0) {
         mesa_loge("orbis-drm: could not protect RADV's window 0x%" PRIx64 "..0x%" PRIx64 ": 0x%08x - falling back "
                   "to the flat arena, so stale accesses stay silent",
                   orbis_va_base, orbis_va_end, (unsigned)err);
         return 0;
      }
      orbis_arena_unmapped = true;
      mesa_logi("orbis-drm: RADV's window 0x%" PRIx64 "..0x%" PRIx64 " (%" PRIu64
                " MiB) is mapped but NOT ACCESSIBLE - a buffer gains access when it is mapped and loses it when it "
                "is unmapped, the way amdgpu's page tables behave",
                orbis_va_base, orbis_va_end, win / (1024 * 1024));
   }
   return 0;
}
#else
/* A memfd is a pool of pages addressed by offset and mappable anywhere, which is what direct memory is. So
 * the host arm is a faithful analogue rather than a stub: same ordering, same aliasing, same errors. */
#define ORBIS_BACKING "memfd"
#include <sys/mman.h>
#include <unistd.h>
#endif

struct ac_drm_device {
   /* -1, and stored only so ac_drm_device_get_fd can hand back what it was given. There is no /dev/dri
    * here, and -1 is the right lie precisely because it is not plausible: a syscall on it fails loudly,
    * where a positive number would quietly operate on some unrelated open descriptor. */
   int fd;
   uint32_t major_version;
   uint32_t minor_version;
   /* Owned by the device, the way the amdgpu arm owns its own (ac_linux_drm.c:87 finalizes it in
    * device_deinitialize). Callers that want to keep one past the device's life clone it. */
   struct util_sync_provider *sync;
};

/* ---------------------------------------------------------------- the sync provider
 *
 * Returned as a pointer, and RADV reads a member off it before doing anything else:
 *
 *    info->has_timeline_syncobj = ac_drm_device_get_sync_provider(dev)->timeline_wait != NULL;
 *
 * so returning NULL is a null dereference during device init, not a clean refusal. Instead: a real
 * struct whose five REQUIRED entries fail loudly and whose other nine stay NULL.
 *
 * Leaving timeline_wait NULL is load-bearing and deliberate - it makes RADV not advertise
 * KHR_timeline_semaphore or KHR_present_wait, which is an honest absence rather than a broken promise.
 * The five that must exist cannot be refused because has_syncobj is hardcoded true rather than probed
 * (ac_gpu_info.c: info->has_syncobj = true). See research/02-syncobj.md.
 */

/* THE BACKING STORE IS AN ARRAY OF COUNTERS, AND ON PURPOSE FOR NOW. research/02-syncobj.md's
 * "falsifiable first step, no console" is exactly this: implement the five required entries over plain
 * host memory and let RADV exercise the whole vk_sync layer on the laptop. Only the STORE changes on the
 * console - from this array to the GPU label page GnmDevice::waitFence already polls - and the GPU-side
 * signal is not a provider entry at all, it arrives as CHUNK_ID_SYNCOBJ_OUT on the submit.
 *
 * A counter rather than a flag because that is what a syncobj is: reset means zero, signal means non-zero,
 * and the timeline entries this port leaves NULL are the ones that would need the value itself.
 */
/* ⚠ THE THIRD FIXED TABLE THE CTS HAS WALKED INTO, and the first two were made to grow for exactly
 * this reason. dEQP-VK.synchronization.basic.binary_semaphore.chain - the FIRST test of the family,
 * on its first ever run here - asked for more syncobjs than this and got
 * VK_ERROR_OUT_OF_HOST_MEMORY out of vkCreateFence, with the arm saying "out of syncobj slots
 * (1024)" twice on its way out. dEQP treats a ResourceError as fatal to the SESSION, so 1899 tests
 * ended after one.
 *
 * ⚠ AND IT IS A CEILING, NOT A LEAK. orbis_sync_destroy does `*slot = (struct orbis_sync){0}`,
 * which clears `taken` - I read the destroy path looking for a literal `taken = false`, did not
 * find one, and had already called it a leak before checking what the zeroing did.
 *
 * 1024 was a number large enough for a game, the same way 4096 BO slots and 8192 live mappings
 * were. The table doubles instead. Handles are INDICES here too, so they stay valid across the
 * realloc where pointers into the array would not. */
#define ORBIS_SYNC_SLOTS_INITIAL 1024

/* TWO WAYS TO BE SIGNALLED, AND THE SECOND ONE IS THE POINT OF PHASE 5.
 *
 *   value    the CPU signalled it, which is what vkSignalSemaphore and a reset-then-signal do.
 *   gpu_seq  the GPU signals it, by reaching the end of the submission that carried this handle as
 *            CHUNK_ID_SYNCOBJ_OUT. Non-zero means "signalled once the fence label reaches this number".
 *
 * Before this, a submit signalled its out-syncobjs on the CPU the moment the packets were queued - so
 * anything that waited on one was told the GPU was finished before it had started. Ordering against the CPU
 * happened to be correct because every FENCE wait polls the label; ordering between two submits was not, and
 * that is what every renderer does. */
struct orbis_sync {
   uint64_t value;
   uint64_t gpu_seq;
   bool taken;
};

static simple_mtx_t orbis_sync_lock = SIMPLE_MTX_INITIALIZER;
static struct orbis_sync *orbis_sync_slot;
static uint32_t orbis_sync_slots;

/* Called with orbis_sync_lock held. Returns the index of a free slot, growing the table when every
   one is taken. UINT32_MAX when the allocation fails. */
static uint32_t
orbis_sync_slot_claim(void)
{
   for (uint32_t i = 0; i < orbis_sync_slots; i++)
      if (!orbis_sync_slot[i].taken)
         return i;

   const uint32_t want = orbis_sync_slots ? orbis_sync_slots * 2 : ORBIS_SYNC_SLOTS_INITIAL;
   struct orbis_sync *const grown = realloc(orbis_sync_slot, want * sizeof(*grown));
   if (grown == NULL)
      return UINT32_MAX;

   memset(grown + orbis_sync_slots, 0, (want - orbis_sync_slots) * sizeof(*grown));
   mesa_logi("orbis-drm: syncobj table grown %u -> %u slots", orbis_sync_slots, want);

   const uint32_t first = orbis_sync_slots;
   orbis_sync_slot = grown;
   orbis_sync_slots = want;
   return first;
}

/* Handles are slot + 1, so that 0 is never a valid handle - DRM's convention, and it makes a
 * zero-initialised handle field fail loudly instead of aliasing slot 0. */
static struct orbis_sync *
orbis_sync_get(uint32_t handle)
{
   if (handle == 0 || handle > orbis_sync_slots || !orbis_sync_slot[handle - 1].taken)
      return NULL;
   return &orbis_sync_slot[handle - 1];
}

/* The label is written by the GPU and read here without the lock, which is correct for a single 32-bit word
 * whose only writer is the hardware: a torn read is not possible and a stale one only costs another poll. */
static bool
orbis_sync_reached(const struct orbis_sync *sync)
{
   if (sync->value != 0)
      return true;
   if (sync->gpu_seq == 0)
      return false;
   /* Signed delta, for the same reason ac_drm_cs_query_fence_status uses one: the label is 32 bits and the
    * sequence number is 64. */
   return (int32_t)(*orbis_fence_label - (uint32_t)sync->gpu_seq) >= 0;
}

static int
orbis_sync_create(struct util_sync_provider *p, uint32_t flags, uint32_t *handle)
{
   simple_mtx_lock(&orbis_sync_lock);
   {
      const uint32_t i = orbis_sync_slot_claim();
      if (i == UINT32_MAX) {
         simple_mtx_unlock(&orbis_sync_lock);
         mesa_loge("orbis-drm: could not grow the syncobj table past %u slots", orbis_sync_slots);
         return -ENOMEM;
      }
      orbis_sync_slot[i].taken = true;
      /* DRM_SYNCOBJ_CREATE_SIGNALED is 1 << 0. vk_drm_syncobj_get_type_from_provider creates one this
       * way and then waits on it with a zero timeout, so honouring the flag is what lets RADV advertise
       * CPU_WAIT and WAIT_ANY. */
      orbis_sync_slot[i].value = (flags & 0x1) ? 1 : 0;
      orbis_sync_slot[i].gpu_seq = 0;
      simple_mtx_unlock(&orbis_sync_lock);
      *handle = i + 1;
      return 0;
   }
   simple_mtx_unlock(&orbis_sync_lock);
   mesa_loge("orbis-drm: unreachable: the syncobj table neither grew nor failed");
   return -ENOMEM;
}

static int
orbis_sync_destroy(struct util_sync_provider *p, uint32_t handle)
{
   simple_mtx_lock(&orbis_sync_lock);
   struct orbis_sync *slot = orbis_sync_get(handle);
   if (slot)
      *slot = (struct orbis_sync){0};
   simple_mtx_unlock(&orbis_sync_lock);
   return slot ? 0 : -EINVAL;
}

static int
orbis_sync_signal(struct util_sync_provider *p, const uint32_t *handles, uint32_t handle_count)
{
   simple_mtx_lock(&orbis_sync_lock);
   for (uint32_t i = 0; i < handle_count; i++) {
      struct orbis_sync *slot = orbis_sync_get(handles[i]);
      if (!slot) {
         simple_mtx_unlock(&orbis_sync_lock);
         return -EINVAL;
      }
      /* A CPU signal overrides a pending GPU one: the caller is asserting the object is signalled NOW, and
       * leaving gpu_seq set would let a later poll decide it is not. */
      slot->value = 1;
      slot->gpu_seq = 0;
   }
   simple_mtx_unlock(&orbis_sync_lock);
   return 0;
}

static int
orbis_sync_reset(struct util_sync_provider *p, const uint32_t *handles, uint32_t handle_count)
{
   simple_mtx_lock(&orbis_sync_lock);
   for (uint32_t i = 0; i < handle_count; i++) {
      struct orbis_sync *slot = orbis_sync_get(handles[i]);
      if (!slot) {
         simple_mtx_unlock(&orbis_sync_lock);
         return -EINVAL;
      }
      slot->value = 0;
      slot->gpu_seq = 0;
   }
   simple_mtx_unlock(&orbis_sync_lock);
   return 0;
}

static int
orbis_sync_wait(struct util_sync_provider *p, uint32_t *handles, unsigned num_handles,
                int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled)
{
   /* DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL is 1 << 0. Without it the wait is wait-ANY, and first_signaled is an
    * out-parameter RADV uses to learn WHICH one - easy to leave unwritten and then wonder why wait-any
    * returns the wrong index, which research/02 named as a thing that could surprise. */
   const bool wait_all = flags & 0x1;

   /* ⚠ DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT (1 << 1) IS A MODIFIER, NOT A MODE, and reading it as a mode
    * was a bug this file briefly carried. It means "if this syncobj has no fence attached yet, wait for one
    * rather than refusing" - the wait then still waits for the fence to SIGNAL. vk_drm_syncobj_wait_many
    * sets it on every wait it makes, plain vkWaitForFences included, which is the proof: were it a mode,
    * every fence wait on every platform would return before the GPU had finished.
    *
    * So it is deliberately not consulted here. A syncobj with no fence yet is simply not reached, and this
    * wait polls until it is or the deadline passes - which is the same observable behaviour.
    */

   /* ⚠ TIMEOUTS ARE ABSOLUTE NANOSECONDS in DRM's convention, not a duration. Treating one as a duration
    * turns a deadline that has already passed into a very long sleep - the conversion research/02 warned
    * about. os_time_get_absolute_timeout handles zero and INT64_MAX for us. */
   /* The caller's own deadline is honoured; the cap only decides how often a long wait says so. */
   struct orbis_wait_watch watch;
   orbis_wait_begin(&watch, (uint64_t)timeout_nsec, "syncobj");

   /* ⚠ A DEADLINE THAT WAS ALREADY BEHIND US WHEN WE WERE CALLED IS A POLL, NOT A WAIT, AND THIS
    * DRIVER HAS BEEN CALLING IT A GPU FAILURE.
    *
    * Measured on hardware 2026-08-31: 81 of these a frame, and the caller's deadline was 1344 ns and
    * 5374 ns in the past on entry - microseconds, not the decades a wrong clock base would give. So
    * zink is asking "is it done yet" and expressing it as an absolute deadline of the present
    * instant, which is exactly what timeout_nsec == 0 means and is already handled quietly three
    * screens down. Same label repeatedly, which is what a poll loop looks like.
    *
    * ⚠ AND THIS CHANGES THE ANSWER, NOT THE WORK - said plainly because it would be easy to read as
    * a performance fix and it is not one. A wait whose deadline has passed already returns after a
    * single pass of the loop below; there is no sleep to remove and no call to avoid. What this
    * removes is a wrong diagnosis - "the GPU did not finish" about a caller that never asked it to -
    * and it splits the counter, so the next run can say whether the ~146 bytes this driver loses per
    * expiring wait follows the POLLS or the genuine timeouts. Those are different bugs if they
    * separate. */
   const bool expired_on_entry = (int64_t)os_time_get_nano() >= watch.caller_deadline;

   /* ⚠ THE MEASUREMENT THE FENCE POOL NEEDS. A title exhausted its 32-fence pool and threw "device is
    * lost" while this arm reported the GPU label advancing normally - which means the disagreement is
    * between what a fence WAS given and what this wait is comparing it against. Print both, per handle,
    * bounded, so that "the fence never signalled" and "the fence was never given a sequence" stop looking
    * the same from outside.
    */
   {
      /* ⚠ TWO BUDGETS, because the interesting call is not among the first. Single-handle waits are
       * vkGetFenceStatus and there are hundreds of them before anything else happens; the one that decides
       * whether a fence pool can be recycled is vkWaitForFences over the whole pool, which arrives late and
       * is rare. Bounding by call count alone spent the whole budget before it appeared.
       */
      static unsigned said, said_many;
      const bool many = num_handles > 1;
      if (orbis_trace() && orbis_budget(many ? &said_many : &said, 12)) {
         simple_mtx_lock(&orbis_sync_lock);
         const struct orbis_sync *const s0 = num_handles ? orbis_sync_get(handles[0]) : NULL;
         mesa_logi("orbis-drm: sync wait: %u handle(s), all=%d, timeout=%" PRId64 ", label=%u, "
                   "h0=%u value=%" PRIu64 " gpu_seq=%" PRIu64,
                   num_handles, wait_all ? 1 : 0, timeout_nsec, *orbis_fence_label,
                   num_handles ? handles[0] : 0, s0 ? s0->value : 0, s0 ? s0->gpu_seq : 0);
         simple_mtx_unlock(&orbis_sync_lock);
      }
   }

   for (;;) {
      unsigned signaled = 0;
      simple_mtx_lock(&orbis_sync_lock);
      for (unsigned i = 0; i < num_handles; i++) {
         struct orbis_sync *slot = orbis_sync_get(handles[i]);
         if (!slot) {
            simple_mtx_unlock(&orbis_sync_lock);
            return -EINVAL;
         }
         if (orbis_sync_reached(slot)) {
            signaled++;
            if (!wait_all) {
               if (first_signaled)
                  *first_signaled = i;
               simple_mtx_unlock(&orbis_sync_lock);
               return 0;
            }
         }
      }
      simple_mtx_unlock(&orbis_sync_lock);

      if (wait_all && signaled == num_handles)
         return 0;

      if (timeout_nsec == 0 || !orbis_wait_continue(&watch)) {
         /* ⚠ errno, NOT THE RETURN VALUE, IS WHAT MESA READS - AND GETTING THAT WRONG TURNED EVERY POLL INTO A
          * FATAL ERROR.
          *
          * vk_drm_syncobj.c:238 and :386 both branch on `errno == ETIME` and map anything else to
          * VK_ERROR_UNKNOWN. This returned -ETIME and left errno alone, so a wait that had merely not finished
          * yet came back to the application as VK_ERROR_UNKNOWN - which is the -13 OpenGothic died on, 28 ms into
          * its first frame, while the GPU may simply have still been working. libdrm's convention is errno plus a
          * negative return, and this now follows it.
          *
          * A ZERO TIMEOUT IS A POLL, NOT A FAILURE. vkGetFenceStatus and vkWaitForFences(0) are how an engine
          * asks "is it done"; answering "the GPU did not finish" to that is a diagnosis of nothing, and shouting
          * it once per frame is worse. So the loud line is kept for a real expired deadline and the poll gets a
          * quiet one, bounded.
          *
          * The DIAGNOSIS this used to shout is still correct for a real timeout: on this platform nothing signals
          * from an interrupt, so a deadline that expires means the GPU did not reach the end of a submission. */
         errno = ETIME;
         if (timeout_nsec == 0 || expired_on_entry) {
#if defined(__PS4__)
            orbis_kc_hit(ORBIS_KC_SYNC_POLL);
#endif
            static unsigned polls;
            if (orbis_budget(&polls, 4)) {
               mesa_logi("orbis-drm: syncobj POLL says not yet, label %u - a deadline of now or "
                         "earlier is a question, not a failure", *orbis_fence_label);
            }
         } else {
#if defined(__PS4__)
            orbis_kc_hit(ORBIS_KC_SYNC_TIMEOUT);
#endif
            /* ⚠ LOUD ONCE, NOT EIGHTY-TWO THOUSAND TIMES - AND THE MISSING BUDGET HERE IS ITSELF A
             * FINDING. The poll arm three lines up is bounded with orbis_budget(&polls, 4) and this
             * one never was, on the reasoning that a real expired deadline is worth shouting about.
             * It is, once: measured 2026-08-31, a single glcore session emitted 81974 copies of this
             * line - 99.5% of the whole log - which buries every other line in the file and makes the
             * one channel this console has useless for anything else.
             *
             * ⚠ AND BOUNDING IT IS ALSO THE EXPERIMENT. The same session drained libkernel's internal
             * memory from 14013728 bytes to 96 and killed the process, and this event is the only
             * thing in the run whose rate tracks the drain - BO allocation does not, the arena does
             * not, vkAllocateMemory is zero. Two possibilities remain and they are told apart by this
             * one line: if the cost is in the LOG SINK, bounding it stops the drain; if the cost is in
             * the WAIT, the drain continues while the counter above still reports the true rate.
             * Either answer is worth a run, and the log is worth bounding regardless. */
            static unsigned said_timeout;
            if (orbis_budget(&said_timeout, 8)) {
               /* ⚠ AND WHY IT EXPIRED, NOT ONLY THAT IT DID - the question the counter cannot answer.
                * 81 of these a frame while Usleep runs at 15 a frame means most of them never slept
                * ONCE: orbis_wait_continue found the caller's deadline already behind it on the first
                * pass. That is either a caller asking for no time at all, or a deadline made on a
                * different clock base from the one compared against here - the second is the exact
                * defect orbis-compat's umtx.h was written to fix on the futex path, and nothing has
                * ever checked for it here. The three numbers below tell those apart: a deadline that
                * is microseconds in the past is a caller with no patience, one that is decades in the
                * past is the clock. */
               const int64_t now_ns = (int64_t)os_time_get_nano();
               mesa_logw("orbis-drm: syncobj wait timed out - the GPU did not finish; label %u. "
                         "Caller's deadline %" PRId64 " ns, now %" PRId64 " ns, so it was %" PRId64
                         " ns %s when the wait began (this line is bounded; the true count is the "
                         "BUDGET line's syncobj_timeout counter)",
                         *orbis_fence_label, (int64_t)timeout_nsec, now_ns,
                         now_ns - (int64_t)timeout_nsec,
                         now_ns >= (int64_t)timeout_nsec ? "ALREADY PAST" : "in the future");
            }
         }
         return -ETIME;
      }

      /* Polling, because nothing here signals from an interrupt. On the console this loop becomes
       * GnmDevice::waitFence's, which polls the label page the same way. */
      orbis_poll_pause();
   }
}

/* ⚠ clone AND finalize ARE NOT OPTIONAL, and this file said they were until the amdgpu winsys entered the
 * build. Both are called through the pointer with no NULL check:
 *
 *    radv_amdgpu_winsys.c:204   return p->clone(p);        - vk_device wants a provider of its own
 *    vk_device.c:325            device->sync->finalize(device->sync);
 *
 * So each caller gets a HEAP COPY and finalize frees it, which is exactly the ownership upstream's DRM
 * provider has (drm_syncobj_finalize is free(p), drm_sync_provider_clone builds a fresh one). The slot
 * array behind them is file-static, so every copy sees the same syncobjs - which is correct: on this
 * platform there is one device and one set of them. */
static void
orbis_sync_finalize(struct util_sync_provider *p)
{
   free(p);
}

static struct util_sync_provider *orbis_sync_provider_create(void);

static struct util_sync_provider *
orbis_sync_clone(struct util_sync_provider *p)
{
   return orbis_sync_provider_create();
}

/* Declared before the table that names it. Five ordering failures in this file came from definitions sitting
 * below their first use, and a static initialiser is a use. */
static int orbis_sync_transfer(struct util_sync_provider *p, uint32_t dst_handle, uint64_t dst_point,
                               uint32_t src_handle, uint64_t src_point, uint32_t flags);

static const struct util_sync_provider orbis_sync_provider_template = {
   .create = orbis_sync_create,
   .destroy = orbis_sync_destroy,
   .wait = orbis_sync_wait,
   .reset = orbis_sync_reset,
   .signal = orbis_sync_signal,
   .finalize = orbis_sync_finalize,
   .clone = orbis_sync_clone,
   /* See orbis_sync_transfer_payload: WSI's present path calls this through vk_drm_syncobj_copy_payloads, and a
    * NULL here was a crash with no log rather than a refusal. */
   .transfer = orbis_sync_transfer,
   /* The rest stay NULL on purpose:
    *   timeline_signal, timeline_wait, query            - no timeline semaphores here
    *   handle_to_fd, fd_to_handle,
    *   import_sync_file, export_sync_file               - fd-based cross-process sharing, meaningless
    *                                                      for a single homebrew process
    */
};

/* ⚠ THE PROVIDER NEEDS transfer, AND ITS ABSENCE WAS A JUMP TO ADDRESS ZERO.
 *
 * vk_drm_syncobj_copy_payloads (vk_drm_syncobj.c:655) either transfers payloads - when the device has timeline
 * syncobjs - or falls back to creating a syncobj and doing sync-file import/export. This port has neither
 * timelines nor sync files, so the fallback called through NULL provider entries: the process died inside
 * vkQueuePresentKHR with nothing written, which is what a call through a NULL function pointer looks like here.
 *
 * WSI takes that path on every present: a present has wait semaphores and no command buffers, and
 * wsi_queue_submit2_unordered short-circuits exactly that shape into copy_sync_payloads.
 *
 * So transfer is implemented, with the same rule as ac_drm_cs_syncobj_transfer: copy the LATER of the two
 * payloads, refuse a non-zero timeline point. One shared helper rather than two copies of that decision. */
static int
orbis_sync_transfer_payload(uint32_t dst_handle, uint64_t dst_point, uint32_t src_handle, uint64_t src_point)
{
   if (dst_point != 0 || src_point != 0) {
      ORBIS_DRM_REFUSED("a timeline point in a syncobj transfer - this store is a counter, not a timeline");
      return -ENOSYS;
   }

   simple_mtx_lock(&orbis_sync_lock);
   struct orbis_sync *dst = orbis_sync_get(dst_handle);
   struct orbis_sync *src = orbis_sync_get(src_handle);
   if (dst == NULL || src == NULL) {
      simple_mtx_unlock(&orbis_sync_lock);
      mesa_loge("orbis-drm: syncobj transfer with a bad handle (dst %u, src %u)", dst_handle, src_handle);
      return -EINVAL;
   }
   /* Never backwards: accumulating several waits into one syncobj means "the latest of them", and an assignment
    * would let the second copy undo the first. */
   if (src->value > dst->value)
      dst->value = src->value;
   if (src->gpu_seq > dst->gpu_seq)
      dst->gpu_seq = src->gpu_seq;
   simple_mtx_unlock(&orbis_sync_lock);
   return 0;
}

static int
orbis_sync_transfer(struct util_sync_provider *p, uint32_t dst_handle, uint64_t dst_point, uint32_t src_handle,
                    uint64_t src_point, uint32_t flags)
{
   (void)p;
   (void)flags;
   return orbis_sync_transfer_payload(dst_handle, dst_point, src_handle, src_point);
}

static struct util_sync_provider *
orbis_sync_provider_create(void)
{
   struct util_sync_provider *p = malloc(sizeof(*p));
   if (p)
      *p = orbis_sync_provider_template;
   return p;
}

struct util_sync_provider *
ac_drm_device_get_sync_provider(ac_drm_device *dev)
{
   /* Created once with the device, because RADV reads a member off the result before checking anything:
    *    info->has_timeline_syncobj = ac_drm_device_get_sync_provider(dev)->timeline_wait != NULL;
    * A NULL return there is a null dereference during device init rather than a clean refusal. */
   return dev->sync;
}

/* ---------------------------------------------------------------- device lifetime
 *
 * THE DRM VERSION WE REPORT IS DERIVED, NOT PICKED. ac_gpu_info.c constrains it from both sides:
 *
 *   :1471   assert(info->drm_major == 3);                        -> major must be 3
 *   :1474   if (info->drm_minor < 54) ... return FAIL;           -> minor must be >= 54
 *   :1088   has_gpuvm_fault_query      = drm_minor >= 55;        -> 55 would claim a query we REFUSE
 *   :1169   has_default_zerovram_support = drm_minor >= 59;      -> 59 would claim behaviour we lack
 *
 * So 3.54.0 is the only value that is accepted and switches on nothing this port does not provide. It is
 * the amdgpu interface of kernel 6.6, which is the oldest RADV still talks to.
 */

#define ORBIS_DRM_MAJOR 3
#define ORBIS_DRM_MINOR 54

int
ac_drm_device_initialize(int fd, bool is_virtio, uint32_t *major_version, uint32_t *minor_version,
                         ac_drm_device **device_handle)
{
   if (is_virtio) {
      ORBIS_DRM_REFUSED("virtio-gpu is a different backend of this same layer, not this one");
      return -ENOSYS;
   }

   ac_drm_device *dev = calloc(1, sizeof(*dev));
   if (!dev)
      return -ENOMEM;

   /* Kept as handed in, which is -1 from radv_orbis_winsys_query_info. Nothing here opens a file: a
    * syscall on -1 fails loudly, where a plausible-looking positive number would act on some unrelated
    * open descriptor. */
#if defined(__PS4__)
   /* BEFORE the window is reported, because on this platform the kernel chooses it. ac_drm_query_info's
    * DEV_INFO answer is read straight out of orbis_va_base/_end. */
   if (orbis_arena_setup() != 0) {
      free(dev);
      return -ENOMEM;
   }
#endif

   dev->fd = fd;
   dev->major_version = ORBIS_DRM_MAJOR;
   dev->minor_version = ORBIS_DRM_MINOR;

   dev->sync = orbis_sync_provider_create();
   if (!dev->sync) {
      free(dev);
      return -ENOMEM;
   }

   *major_version = dev->major_version;
   *minor_version = dev->minor_version;
   *device_handle = dev;

   /* The backing store is named in the log because it is the one thing that differs between the laptop and
    * the console, and every later message has to be read in the light of which one is live. */
   /* ⚠ ONCE PER PROCESS, ON BOTH ARMS, AND IN THE SHARED PATH ON PURPOSE. The first version of this call sat in
    * the console-only arena setup, so the laptop never ran it - which is precisely the mistake that sent a
    * question about stream CONSTRUCTION to the console earlier tonight. Anything that checks what this driver
    * BUILDS belongs where both arms reach it. */
   static unsigned selftest_done;
   if (orbis_budget(&selftest_done, 1)) {
      orbis_selftest_flatten();
      orbis_selftest_pad();
      orbis_selftest_dead_pool();
      orbis_selftest_futex();
      /* Separate call rather than a tail of the one above, because that one returns early on every
         failure it finds and the timed arm is an independent claim. */
      orbis_selftest_futex_timed();
      orbis_characterise_clocks();
      orbis_test_mirror_mapping();
      orbis_watchdog_start();
   }

   mesa_logi("orbis-drm: device up, reporting amdgpu interface %u.%u, memory backed by %s",
             dev->major_version, dev->minor_version, ORBIS_BACKING);
   return 0;
}

/* WHAT THIS ARM STILL HOLDS, counted at teardown. Every resource it tracks is in a file-static table, so a
 * leak here is invisible: the device goes away, the slots do not, and the failure is a SECOND device that runs
 * out of them. Reported unconditionally rather than behind the trace switch, because one line at device
 * destruction is cheap and a growing number across two devices is the whole signal. */
static void
orbis_report_residue(void)
{
   unsigned bos = 0, syncs = 0, ctxs = 0;
   uint64_t va_ranges = 0, va_bytes = 0;

   simple_mtx_lock(&orbis_bo_lock);
   for (uint32_t i = 0; i < orbis_bo_slots; i++)
      if (orbis_bo_slot[i])
         bos++;
   simple_mtx_unlock(&orbis_bo_lock);

   simple_mtx_lock(&orbis_sync_lock);
   for (uint32_t i = 0; i < orbis_sync_slots; i++)
      if (orbis_sync_slot[i].taken)
         syncs++;
   simple_mtx_unlock(&orbis_sync_lock);

   simple_mtx_lock(&orbis_ctx_lock);
   for (uint32_t i = 0; i < ORBIS_MAX_CONTEXTS; i++)
      if (orbis_ctx[i].taken)
         ctxs++;
   simple_mtx_unlock(&orbis_ctx_lock);

   simple_mtx_lock(&orbis_va_lock);
   for (const struct amdgpu_va *va = orbis_va_list; va; va = va->next) {
      va_ranges++;
      va_bytes += va->size;
   }
   simple_mtx_unlock(&orbis_va_lock);

   /* ⚠ THE NUMBER IS NOT THE SIGNAL - THE GROWTH IS. Measured over four device create/destroy cycles on the
    * host, the residue is CONSTANT at 2 BOs, 1 syncobj and 2 VA ranges of 16 KiB, and that is correct rather
    * than leaked: RADV's amdgpu winsys is refcounted per device cookie and keeps its own allocations - the
    * null-PRT BO, the fence BO, its VM timeline syncobj - alive across devices. A single report cannot tell
    * that apart from a leak, so this remembers the last one and only complains when it grows. Which is the
    * failure a real application dies of: not residue, but residue per device. */
   static unsigned prev_bos, prev_syncs, prev_ctxs;
   static uint64_t prev_ranges;
   static bool seen;

   /* ⚠ AN EMPTY TEARDOWN IS NOT A BASELINE, and making it one turned this detector into a liar.
    *
    * radv_orbis_winsys_query_info creates a device purely to read radeon_info off it and now
    * deinitialises it again, which reaches here holding nothing at all. Recorded as the previous
    * sample, that zero made the FIRST REAL device's teardown look like growth from nothing, and the
    * gate printed "something is leaking per device" at a run with no leak in it. A detector that
    * cries at the normal case is worse than no detector, because the next real one is ignored.
    *
    * Zero everywhere means this device never got as far as allocating, so it says nothing about
    * whether anything leaks: reported, and then not remembered. */
   const bool empty = bos == 0 && syncs == 0 && ctxs == 0 && va_ranges == 0;

   const bool grew = seen && !empty && (bos > prev_bos || syncs > prev_syncs || ctxs > prev_ctxs ||
                                        va_ranges > prev_ranges);

   if (grew)
      mesa_logw("orbis-drm: RESIDUE GREW across devices - %u BO(s) was %u, %u syncobj(s) was %u, %u ctx was "
                "%u, %" PRIu64 " VA range(s) was %" PRIu64 " - something is leaking per device",
                bos, prev_bos, syncs, prev_syncs, ctxs, prev_ctxs, va_ranges, prev_ranges);
   else
      mesa_logi("orbis-drm: at teardown %u BO(s), %u syncobj(s), %u context(s), %" PRIu64 " VA range(s), %"
                PRIu64 " KiB held - winsys-lifetime, and it did not grow",
                bos, syncs, ctxs, va_ranges, va_bytes / 1024);

   if (!empty) {
      prev_bos = bos;
      prev_syncs = syncs;
      prev_ctxs = ctxs;
      prev_ranges = va_ranges;
      seen = true;
   }
}

void
ac_drm_device_deinitialize(ac_drm_device *dev)
{
   /* Returns void, so a do-nothing body leaks silently - which is why the free lives here from the first
    * version of this file rather than being added when it starts to matter. */
   orbis_report_residue();

   if (dev->sync)
      dev->sync->finalize(dev->sync);
   free(dev);
}

/* ---------------------------------------------------------------- device queries */

/* ---------------------------------------------------------------- AMDGPU_INFO_DEV_INFO
 *
 * The 39 fields ac_gpu_info.c reads off drm_amdgpu_info_device. Regenerate the list with
 *
 *     grep -oE 'device_info(\.|->)[a-z0-9_]+' src/amd/common/ac_gpu_info.c | sort -u
 *
 * Every field below carries its provenance, because this is the phase where a wrong value produces a
 * driver that works and renders subtly wrongly - the class of defect this project has spent the most
 * console flashes on:
 *
 *   MEASURED  established on Liverpool by the Tempest fork, with the citation
 *   DERIVED   forced by RADV's own code; there is no freedom to get it wrong
 *   PLATFORM  a property of the console rather than of a measurement
 *   DECISION  a choice, with its consequence named
 *   UNCITED   not establishable from any oracle in the stash; logged by name at runtime
 */
static int
orbis_query_dev_info(struct drm_amdgpu_info_device *d)
{
   memset(d, 0, sizeof(*d));

   /* --- chip identity. DECISION, and research/04 explains why this one is about inheriting a bug list
    * rather than about resembling a chip: ac_identify_chip maps (family, external_rev) through addrlib's
    * ASICREV macros, and Liverpool is in no arm of that switch. FAMILY_CI + external_rev 21 is what makes
    * RADV say CHIP_BONAIRE, which is what the reference dump in notes/radeon_info-bonaire.txt reports.
    * Consequence: this port inherits Bonaire's errata list, and has_lds_bank_count_16 stays 0. */
   /* ⚠ AND THE LINUX STACK DISAGREES WITH ALL OF IT, which the maintainer's own reference makes plain.
    * ps4-video-archlinux identifies this chip inside `case FAMILY_KV:` - the KAVERI APU family - with an ASIC
    * revision in a range it carves out of SPOOKY:
    *
    *     #define AMDGPU_STARSHA_RANGE  0x61, 0x71        (amdgpu_asic_addr.h, their patch)
    *     identify_chip2(STARSHA, LIVERPOOL);             (ac_gpu_info.c, their patch)
    *
    * and addrlib's own comment says what FAMILY_KV means: "Kaveri APUs: Spectre, Spooky, Kalindi, Godavari".
    * Liverpool IS an APU. Reporting FAMILY_CI makes addrlib and RADV apply the rules of a DISCRETE Sea Islands
    * card to it - which is why this tree carries patches 0010 and 0018, "no tile swizzle" and "no tile mode
    * liverpool cannot name". Those two exist to undo consequences of the identity being wrong, and the honest fix
    * is the identity rather than another exception.
    *
    * ORBIS_CHIP picks it, so this is an A/B and not a belief:
    *
    *   bonaire    FAMILY_CI + rev 21   -> CHIP_BONAIRE. Everything measured so far was measured here.
    *   kaveri     FAMILY_KV + rev 0x61 -> stock Mesa says CHIP_KAVERI and addrlib says SPOOKY, an APU of the
    *                                      RIGHT family. This is what the Linux stack would say with the Liverpool
    *                                      patch absent, and it costs two numbers to try.
    *
    * ⚠ THE VERIFICATION EXISTS ALREADY: ps4/radv's upload and mip rungs round-trip a CPU pattern through tiled
    * surfaces and read it back THROUGH THE TEXTURE UNIT, exactly. If the identity changes and those still pass,
    * the change is safe; if they start failing, the identity is what the tiling was depending on. Either answer
    * is worth more than the guess it replaces. */
   {
      const char *const chip = getenv("ORBIS_CHIP");
      if (chip != NULL && strcmp(chip, "kaveri") == 0) {
         d->family = 125;      /* FAMILY_KV - addrlib's Kaveri APU family, which is what Liverpool is */
         d->external_rev = 0x61; /* the start of ps4-video-archlinux's STARSHA range */
         mesa_logi("orbis-drm: ORBIS_CHIP=kaveri - reporting FAMILY_KV (125) + external_rev 0x61, the family and "
                   "revision range the Linux stack uses for Liverpool");
      } else {
         d->family = 120; /* FAMILY_CI */
         d->external_rev = 21;
         if (chip != NULL)
            mesa_logw("orbis-drm: ORBIS_CHIP=%s is not a value this arm knows - staying on FAMILY_CI/BONAIRE",
                      chip);
      }
   }
   d->chip_rev = 1;
   d->pci_rev = 0;

   ORBIS_DRM_UNCITED("drm_amdgpu_info_device.device_id", "Liverpool's PCI id is in no oracle; "
                                                         "it reaches only radeon_info.pci_id");
   d->device_id = 0x9920;

   /* --- shader array topology. ALL UNCITED, and gnmtune.h:732 says so in the fork's own words:
    * "the fork cannot cite Liverpool's CU/SIMD topology from any oracle in the stash".
    *
    * These cannot be left zero - ac_gpu_info walks cu_bitmap to derive max_good_cu_per_sa, and a zero
    * bitmap means a GPU with no compute units - so a shape is reported and named as a guess.
    * A measurement would replace it: SQ_WAVE_HW_ID carries SE_ID/SH_ID/CU_ID, so a compute
    * kernel that ORs one bit per (SE, SH, CU) enumerates the real topology from the hardware, in exactly
    * the [se][sh] shape cu_bitmap wants. Until it has run, treat every occupancy number RADV derives
    * from these as provisional. */
   /* ⚠ AND THE PUBLISHED SPEC NOW BACKS THESE, which it did not when they were written. Sony's own
    * figures for this APU: "Radeon HD 7850 based graphics engine with 18 Compute Units = 1152 GPU
    * cores", 72 TMUs, 32 ROPs. Every number below falls out of that and agrees:
    *
    *   18 CUs   = 2 engines * 1 array * 9 CUs, which is what this arm writes
    *   72 TMUs  = 4 per CU * 18
    *   32 ROPs  = 4 per backend * 8, and num_rb_pipes = 8 was already MEASURED
    *   HD 7850  = Pitcairn, a TWO shader engine part
    *
    * ⚠ So GB_ADDR_CONFIG's NUM_SHADER_ENGINES field reading ONE is the outlier, not this - that
    * field is a tiling input and is not the arbiter of how many engines the part has. The
    * contradiction this file has carried since the field was guessed resolves in favour of two.
    *
    * ⚠ IT IS STILL NOT A MEASUREMENT. A published figure is a citation, and a citation is worth more
    * than a guess and less than SQ_WAVE_HW_ID enumerating the topology from the silicon. Occupancy
    * numbers RADV derives from these stay provisional. */
   ORBIS_DRM_UNCITED("drm_amdgpu_info_device.num_shader_engines et al",
                     "matches the published 18 CU / 72 TMU / 32 ROP spec, but is not measured from "
                     "the hardware - SQ_WAVE_HW_ID would be");
   /* ⚠ AND THE GEOMETRY STAGE HAS NOW MADE THIS NUMBER TESTABLE, which it never was before.
    *
    * The GSVS ring is sized from max_se (radv_shader_info.c, max_gs_waves = 32 * num_se) and it is
    * measurably too small here: ORBIS_GS_RING_SCALE=2 takes dEQP-VK.geometry.basic from 3 pass /
    * 11 fail to 14 pass / 1 fail, and a scale of 4 does not improve on 2. So the ring needs EXACTLY
    * twice what the formula gives - and with this field at 2, exactly twice is what raising it to 4
    * would produce.
    *
    * That is one of two explanations and they are indistinguishable by scaling the ring, because
    * scaling moves the allocation and the register together:
    *
    *   this field should be 4        the formula is right and its input is wrong
    *   the ring size is per-SE       the formula is wrong; the hardware wants the per-engine size
    *                                 and we hand it the total, which is short by exactly num_se
    *
    * ORBIS_NUM_SE separates them: with the ring left unscaled, a value of 4 fixes geometry only
    * under the first explanation.
    *
    * ⚠ ITS BLAST RADIUS IS WIDE - pa_sc_raster_config, cu_bitmap, every occupancy number RADV
    * derives - so this is a knob for one measurement, not a new default. And note the standing
    * contradiction it sits on: GB_ADDR_CONFIG's own NUM_SHADER_ENGINES field reads ONE. */
   {
      const char *const se = getenv("ORBIS_NUM_SE");
      d->num_shader_engines = se != NULL ? (uint32_t)MAX2(atoi(se), 1) : 2u;
      if (se != NULL)
         mesa_logi("orbis-drm: ORBIS_NUM_SE=%u - AN EXPERIMENT. This field is uncited, "
                   "GB_ADDR_CONFIG says one, and the GSVS ring needs exactly twice what it sizes.",
                   d->num_shader_engines);
   }
   d->num_shader_arrays_per_engine = 1;
   d->num_cu_per_sh = 9;
   d->cu_active_number = d->num_shader_engines * d->num_shader_arrays_per_engine * d->num_cu_per_sh;
   for (unsigned se = 0; se < d->num_shader_engines; se++)
      d->cu_bitmap[se][0] = (1u << d->num_cu_per_sh) - 1;
   /* ⚠ THE LINUX STACK SAYS FOUR. ps4-video-archlinux's radeon winsys puts CHIP_LIVERPOOL in the same arm as
    * BONAIRE and KAVERI: `ws->info.max_tcc_blocks = 4`. This arm has said 8 since it was written, uncited. And
    * sceGnmGetNumTcaUnits() answered 2 on the console, which is the arbiter count rather than the channel count,
    * but it is the same half of the memory system and it does not support 8 either.
    * ORBIS_TCC_BLOCKS=<n> to A/B it; it feeds l2_cache_size and the perf-counter block layout. */
   {
      const char *const tcc = getenv("ORBIS_TCC_BLOCKS");
      d->num_tcc_blocks = tcc != NULL ? (uint32_t)atoi(tcc) : 8u;
      if (tcc != NULL)
         mesa_logi("orbis-drm: ORBIS_TCC_BLOCKS=%u (the Linux stack says 4 for Liverpool; this arm's default is 8)",
                   d->num_tcc_blocks);
   }

   /* --- render backends. MEASURED: gnm-tiling.md H5, "Liverpool has 8 RBs", and rung 2 of the tiling
    * test passing is what confirmed it. Note this differs from Bonaire's 4, so it is one of the diff
    * lines against the reference dump that is CORRECT rather than a defect. */
   d->num_rb_pipes = 8;
   d->enabled_rb_pipes_mask = 0xff;

   /* --- clocks. gpu_counter_freq is the one measured value in this group, and it is measured twice:
    * gnmprof.h:477 says "100 ticks = 1 us, cross-checked against fence-wait", i.e. 100 MHz. RADV turns
    * it into timestampPeriod = 1000000 / clock_crystal_freq = 10 ns. */
   d->gpu_counter_freq = 100000; /* KHz. MEASURED. */
   d->max_engine_clock = 800000; /* KHz. gnmtune.h:138, "Liverpool's ~800 MHz". */
   ORBIS_DRM_UNCITED("drm_amdgpu_info_device.max_memory_clock", "GDDR5 clock not established here");
   d->max_memory_clock = 2750000;

   /* --- memory. DECISION with a consequence worth stating: has_dedicated_vram is
    * !(ids_flags & AMDGPU_IDS_FLAGS_FUSION), and this console has ONE unified GDDR5 pool shared by CPU
    * and GPU. So FUSION is the honest answer, and it is also the field where copying Bonaire - a discrete
    * card - would have produced a driver that believes in dedicated VRAM that does not exist. */
   d->ids_flags = AMDGPU_IDS_FLAGS_FUSION;
   d->vram_type = AMDGPU_VRAM_TYPE_GDDR5; /* PLATFORM */
   d->vram_bit_width = 256;               /* PLATFORM */

   /* --- the virtual address space. OURS to define, since nothing here maps GPU pages independently of
    * CPU ones: the two share one address space, which is why this group is simpler than on a PC.
    * virtual_address_alignment and gart_page_size are DERIVED - RADV aligns allocations to them.
    *
    * The constants live at the top of the VA section below rather than here, because THE ALLOCATOR AND THIS
    * REPORT MUST AGREE and a window described in one place and enforced in another is exactly the kind of
    * disagreement that shows up as a GPU fault far from its cause. */
   d->virtual_address_offset = orbis_va_base;
   d->virtual_address_max = orbis_va_end;
   d->virtual_address_alignment = (uint32_t)orbis_page_size();
   d->pte_fragment_size = 0x200000;
   d->gart_page_size = (uint32_t)orbis_page_size();
   /* NO HIGH RANGE, and that is load-bearing rather than a placeholder: RADV asks for AMDGPU_VA_RANGE_HIGH
    * on EVERY buffer (radv_amdgpu_bo.c's va_flags), so the arm's va_range_alloc has to know that honouring
    * the flag literally is impossible here - which is only consistent because these two are zero. It is
    * also what keeps bit 47 free for the PRT workaround. */
   d->high_va_offset = 0;
   d->high_va_max = 0;

   /* --- gfx7 shader-engine constants. wave_front_size and num_shader_visible_vgprs are DERIVED: 64 and
    * 256 are GCN invariants that ACO itself assumes for this generation. The rest are UNCITED and only
    * bound geometry-pipeline scratch structures. */
   d->wave_front_size = 64;
   d->num_shader_visible_vgprs = 256;
   d->num_hw_gfx_contexts = 8;
   d->gc_double_offchip_lds_buf = 1;
   ORBIS_DRM_UNCITED("drm_amdgpu_info_device geometry limits",
                     "ce_ram_size, gs_*_depth and max_gs_waves_per_vgt are Bonaire-shaped guesses");
   d->ce_ram_size = 32768;
   d->gs_vgt_table_depth = 32;
   d->gs_prim_buffer_depth = 1792;
   d->max_gs_waves_per_vgt = 16;

   /* --- there is no PCI bus, so there is no link to describe. Zero rather than a plausible number: RADV
    * only prints the derived bandwidth. */
   d->pcie_gen = 0;
   d->pcie_num_lanes = 0;

   /* --- everything else stays zero, and for the GFX11+ group that is the RIGHT answer rather than a gap:
    * tcp_cache_size, num_sqc_per_wgp, sqc_*_cache_size, gl1c/gl2c_cache_size, mall_size,
    * enabled_rb_pipes_mask_hi, userq_ip_mask, the shadow and csa sizes, and gfx10's
    * pa_sc_tile_steering_override. */
   return 0;
}

/* AMDGPU_INFO_MEMORY, AND IT MUST AGREE WITH ac_drm_query_heap_info - the two describe the same memory through
 * different doors, and a disagreement between them is a driver that advertises more than it can allocate.
 *
 * THE CEILING IS THE ADDRESS WINDOW, on both arms and for the same reason: nothing can be used that cannot be
 * mapped, and the window is what the VA allocator hands out from. On the console the window IS the arena, so
 * this is also the physical truth. It used to report a flat 4 GB, which was the flattering answer - an
 * application told about 4 GB and given 256 MiB fails at allocation time with nothing to read.
 *
 * ONE POOL REPORTED THREE TIMES follows from IDS_FLAGS_FUSION: there is no separate VRAM, and no
 * CPU-inaccessible part of it. Dividing the arena between the heaps would make the smaller share the real
 * ceiling for no reason. */
static int
orbis_query_memory(struct drm_amdgpu_memory_info *m)
{
   memset(m, 0, sizeof(*m));

   const uint64_t pool = orbis_va_end - orbis_va_base;
   m->vram.total_heap_size = pool;
   m->cpu_accessible_vram.total_heap_size = pool;
   m->gtt.total_heap_size = pool;
   return 0;
}

int
ac_drm_query_info(ac_drm_device *dev, unsigned info_id, unsigned size, void *value)
{
   if (!dev || !value)
      return -EINVAL;

   switch (info_id) {
   case AMDGPU_INFO_DEV_INFO:
      if (size < sizeof(struct drm_amdgpu_info_device))
         return -EINVAL;
      return orbis_query_dev_info(value);

   case AMDGPU_INFO_MEMORY:
      if (size < sizeof(struct drm_amdgpu_memory_info))
         return -EINVAL;
      return orbis_query_memory(value);

   case AMDGPU_INFO_MAX_IBS:
      /* Refusable by design: ac_gpu_info treats failure here as "the kernel cannot say" and substitutes
       * 50 per IP type, which is its own documented estimate. Answering with a number of our own would
       * replace a considered fallback with a guess. */
      ORBIS_DRM_REFUSED("RADV substitutes its own estimate of 50 when this is unavailable");
      return -ENOSYS;

   default:
      mesa_logw("orbis-drm: %s: unhandled info_id 0x%x", __func__, info_id);
      return -ENOSYS;
   }
}

/* ---------------------------------------------------------------- GB_ADDR_CONFIG and MC_ARB_RAMCFG
 *
 * Field positions from oracles/mesa/.../registers/gfx7.json's GB_ADDR_CONFIG. Every value below is either
 * MEASURED on Liverpool or marked uncited, and the measured ones come from the tiling work
 * (backlog/docs/gnm-tiling.md), not from the drm-shim's bonaire entry - Bonaire is gfx7 but it is not this
 * chip, and copying its topology is how a driver ends up working while rendering subtly wrongly.
 */
#define ORBIS_GB_NUM_PIPES_SHIFT               0   /* [0,2]   1 << field                            */
#define ORBIS_GB_PIPE_INTERLEAVE_SHIFT         4   /* [4,6]   256 << field                          */
#define ORBIS_GB_BANK_INTERLEAVE_SHIFT         8   /* [8,10]  1 << field                            */
#define ORBIS_GB_NUM_SHADER_ENGINES_SHIFT     12   /* [12,13] 1 << field                            */
#define ORBIS_GB_SHADER_ENGINE_TILE_SIZE_SHIFT 16  /* [16,18]                                       */
/* ⚠ ROW SIZE, AND THE ONE RULE ABOUT IT: THE TABLE AND THE REGISTER MUST COME FROM HERE, BOTH OF THEM.
 *
 * The kernel's CIK arm assumes a 2 KB memory row and nothing has ever measured Liverpool's. It is not a
 * decorative field: TILE_SPLIT is derived from it, TILE_SPLIT sets the macro-tile geometry, and on GFX7 the
 * texture unit derives EVERY MIP LEVEL's address from that geometry because a T# carries no per-level
 * offsets (Tempest ps4/mipmap). Getting it wrong therefore does not shift a texel - it puts a QUARTER-AREA
 * region of some other level's content on screen, which is what the console shows in motion.
 *
 * `ORBIS_ROW_SIZE=2|4` (KB). Both the register field and the tile tables read it through this one function,
 * because a table built for 2 KB under a register that says 4 KB would be worse than either alone.
 */
static unsigned
orbis_row_size_kb(void)
{
   static unsigned cached;
   if (cached != 0)
      return cached;
   const char *const s = getenv("ORBIS_ROW_SIZE");
   const unsigned    kb = (s != NULL) ? (unsigned)atoi(s) : 2u;
   cached = (kb == 4) ? 4u : 2u;
   mesa_logi("orbis-drm: row size %u KB - GB_ADDR_CONFIG.ROW_SIZE=%u and the %u KB tile tables, from one knob "
             "(ORBIS_ROW_SIZE)", cached, cached == 4 ? 2u : 1u, cached);
   return cached;
}

#define ORBIS_GB_ROW_SIZE_SHIFT               28   /* [28,29] addrlib: 0=1KB 1=2KB 2=4KB            */

static uint32_t
orbis_gb_addr_config(void)
{
   /* 8 pipes: MEASURED. gnm-tiling.md H5 - "Liverpool takes the 8-pipe branch of the CIK tile table" -
    * confirmed by rung 2 of the tiling test passing on the console. */
   uint32_t v = 3u << ORBIS_GB_NUM_PIPES_SHIFT;

   /* 256 B pipe interleave: MEASURED (H7). Also asserted by ac_fill_tiling_info, so a wrong value here
    * trips an assert during init rather than rendering wrongly - the good failure. */
   v |= 0u << ORBIS_GB_PIPE_INTERLEAVE_SHIFT;

   /* Bank interleave 1: MEASURED (H7). */
   v |= 0u << ORBIS_GB_BANK_INTERLEAVE_SHIFT;

   /* 2 KB row size: the fork's H7, and the kernel's own default arm of the row-size switch. addrlib
    * decodes 1 as ADDR_ROWSIZE_2KB (siaddrlib.cpp DecodeGbRegs). ⚠ H7 records this as UNTOUCHED rather
    * than measured - the clamp it feeds has been slack at every element size run so far - so it is a
    * documented assumption, not a measurement. */
   v |= (orbis_row_size_kb() == 4 ? 2u : 1u) << ORBIS_GB_ROW_SIZE_SHIFT;

   /* NOT CITABLE. gnmtune.h:732 says so in the fork's own words. Reported as one shader engine of tile
    * size 32 - Bonaire's shape - because addrlib's CIK path does not read either field: the fork's own
    * tiling implementation reproduces addrlib's CIK addresses on hardware while knowing neither. If that
    * inference is ever wrong, it is wrong for 2D addressing, which rung 2 of the tiling test covers. */
   /* ⚠ THIS FIELD SAYS ONE SHADER ENGINE AND drm_amdgpu_info_device.num_shader_engines SAYS TWO, and both are read
    * by the same driver. The field is 1 << value, so 0 here means ONE engine while the device info says two - a
    * contradiction this port has carried since the field was first guessed at.
    *
    * It matters for screen-space work: addrlib uses this for macro-tiling, RADV uses num_shader_engines for the
    * raster config, and the maintainer's artefacts are quarter-of-the-screen shaped.
    *
    * ORBIS_GB_SE=<n> sets it, so the A/B is an env file: 0 keeps today's behaviour, 1 makes it say two engines and
    * agree with the device info. Tiling currently WORKS, so this is deliberately not changed by default - a wrong
    * value here corrupts surfaces rather than merely misplacing them. */
   ORBIS_DRM_UNCITED("GB_ADDR_CONFIG.NUM_SHADER_ENGINES",
                     "shader-array topology; SQ_WAVE_HW_ID would measure it");
   {
      /* ⚠ MASKED TO THE FIELD, because the knob went into it unchecked and the paragraph above says what that
       * costs: "a wrong value here corrupts surfaces rather than merely misplacing them". The field is two
       * bits at [12,13]. ORBIS_GB_SE=16 shifts into SE_TILE_SIZE at [16,18], which the line below then ORs 2
       * into - so the register would describe a shader-engine tile size nobody chose, from a knob whose whole
       * purpose is to change ONE field and observe the difference. An out-of-range value is a typo in an env
       * file, so it is named rather than quietly truncated. */
      static unsigned cached_se = UINT_MAX;
      if (cached_se == UINT_MAX) {
         const char *const se = getenv("ORBIS_GB_SE");
         const unsigned    want = (se != NULL) ? (unsigned)strtoul(se, NULL, 0) : 0u;
         cached_se = want & 3u;
         if (want != cached_se)
            mesa_logw("orbis-drm: ORBIS_GB_SE=%u does not fit NUM_SHADER_ENGINES' two bits - using %u. The "
                      "field is 1 << value, so 0 means one engine and 1 means two.",
                      want, cached_se);
      }
      v |= cached_se << ORBIS_GB_NUM_SHADER_ENGINES_SHIFT;
   }
   v |= 2u << ORBIS_GB_SHADER_ENGINE_TILE_SIZE_SHIFT;

   return v;
}

/* What the chip actually holds, decoded field by field and printed next to what this arm synthesises. Two of
 * those fields are flagged in orbis_gb_addr_config() as not measured - ROW_SIZE ("a documented assumption")
 * and NUM_SHADER_ENGINES, which claims ONE engine while drm_amdgpu_info_device.num_shader_engines claims TWO.
 * A single read settles both, and the log says outright which guesses it retires. */
static void
orbis_log_gb_addr_config_read(uint32_t v)
{
   const unsigned f_pipes = (v >> ORBIS_GB_NUM_PIPES_SHIFT) & 7u;
   const unsigned f_intlv = (v >> ORBIS_GB_PIPE_INTERLEAVE_SHIFT) & 7u;
   const unsigned f_bank  = (v >> ORBIS_GB_BANK_INTERLEAVE_SHIFT) & 7u;
   const unsigned f_se    = (v >> ORBIS_GB_NUM_SHADER_ENGINES_SHIFT) & 3u;
   const unsigned f_setsz = (v >> ORBIS_GB_SHADER_ENGINE_TILE_SIZE_SHIFT) & 7u;
   const unsigned f_row   = (v >> ORBIS_GB_ROW_SIZE_SHIFT) & 3u;
   const uint32_t ours    = orbis_gb_addr_config();

   mesa_logi("orbis-drm: READ_REGS GB_ADDR_CONFIG=0x%08x FROM THE CHIP: NUM_PIPES=%u(%u pipes) "
             "PIPE_INTERLEAVE=%u(%u B) BANK_INTERLEAVE=%u(%u) NUM_SHADER_ENGINES=%u(%u SE) "
             "SE_TILE_SIZE=%u ROW_SIZE=%u(%u KB)",
             v, f_pipes, 1u << f_pipes, f_intlv, 256u << f_intlv, f_bank, 1u << f_bank,
             f_se, 1u << f_se, f_setsz, f_row, f_row == 0 ? 1u : (f_row == 1 ? 2u : 4u));
   mesa_logi("orbis-drm: READ_REGS   this arm SYNTHESISES 0x%08x%s", ours,
             ours == v ? " - IDENTICAL, every field of it was guessed right" : "");
   if (ours == v)
      return;

   /* Field by field, because "the words differ" does not say which guess was wrong, and each of these fields
    * feeds a different part of addrlib. */
   if (((ours >> ORBIS_GB_NUM_PIPES_SHIFT) & 7u) != f_pipes)
      mesa_logw("orbis-drm: READ_REGS   NUM_PIPES ours=%u chip=%u - the tile-table branch is chosen by this",
                (ours >> ORBIS_GB_NUM_PIPES_SHIFT) & 7u, f_pipes);
   if (((ours >> ORBIS_GB_PIPE_INTERLEAVE_SHIFT) & 7u) != f_intlv)
      mesa_logw("orbis-drm: READ_REGS   PIPE_INTERLEAVE ours=%u chip=%u",
                (ours >> ORBIS_GB_PIPE_INTERLEAVE_SHIFT) & 7u, f_intlv);
   if (((ours >> ORBIS_GB_BANK_INTERLEAVE_SHIFT) & 7u) != f_bank)
      mesa_logw("orbis-drm: READ_REGS   BANK_INTERLEAVE ours=%u chip=%u",
                (ours >> ORBIS_GB_BANK_INTERLEAVE_SHIFT) & 7u, f_bank);
   if (((ours >> ORBIS_GB_NUM_SHADER_ENGINES_SHIFT) & 3u) != f_se)
      mesa_logw("orbis-drm: READ_REGS   NUM_SHADER_ENGINES ours=%u(%u SE) chip=%u(%u SE) - MEASURED NOW. This "
                "field was a guess that contradicted the device info, addrlib uses it for macro-tiling, and "
                "the artefacts it was suspected of are quarter-of-the-screen shaped.",
                (ours >> ORBIS_GB_NUM_SHADER_ENGINES_SHIFT) & 3u,
                1u << ((ours >> ORBIS_GB_NUM_SHADER_ENGINES_SHIFT) & 3u), f_se, 1u << f_se);
   if (((ours >> ORBIS_GB_SHADER_ENGINE_TILE_SIZE_SHIFT) & 7u) != f_setsz)
      mesa_logw("orbis-drm: READ_REGS   SE_TILE_SIZE ours=%u chip=%u",
                (ours >> ORBIS_GB_SHADER_ENGINE_TILE_SIZE_SHIFT) & 7u, f_setsz);
   if (((ours >> ORBIS_GB_ROW_SIZE_SHIFT) & 3u) != f_row)
      mesa_logw("orbis-drm: READ_REGS   ROW_SIZE ours=%u chip=%u - MEASURED NOW, and it picks BOTH the register "
                "field and which tile table orbis_tile_tables.h hands out (ORBIS_ROW_SIZE was the knob for it)",
                (ours >> ORBIS_GB_ROW_SIZE_SHIFT) & 3u, f_row);
}

static uint32_t
orbis_mc_arb_ramcfg(void)
{
   /* THIS ONE IS LOAD-BEARING, and it is easy to mistake for decoration. ac_surface.c feeds it straight
    * into addrlib:
    *
    *    regValue.noOfBanks = info->mc_arb_ramcfg & 0x3;
    *    regValue.noOfRanks = (info->mc_arb_ramcfg & 0x4) >> 2;
    *
    * and addrlib decodes noOfBanks 0/1/2 as 4/8/16 banks (siaddrlib.cpp DecodeGbRegs). The fork's
    * macro-tile table carries NUM_BANKS = 16, so 2 is the value that agrees with what was measured.
    * A different one changes where every 2D-tiled texel lands. */
   uint32_t v = 2u; /* noOfBanks = 2 -> 16 banks. MEASURED. */

   /* noOfRanks: 0 is one rank. Not citable, and not reached by the addressing the fork verified. */
   ORBIS_DRM_UNCITED("MC_ARB_RAMCFG.NOOFRANKS", "one rank assumed; addrlib's CIK path does not read it");
   return v;
}

/* Defined further down, next to the query helpers whose output it prints. */
static void orbis_dump_shim_device(void);

int
ac_drm_query_gpu_info(ac_drm_device *dev, struct amdgpu_gpu_info *info)
{
   if (!dev || !info)
      return -EINVAL;

   memset(info, 0, sizeof(*info));

   /* ac_fill_tiling_info reads exactly four fields of this struct - checked with
    * `grep -oE 'amdinfo(\.|->)[a-z0-9_]+' ac_gpu_info.c` - so the rest staying zero is not a gap. */
   info->gb_addr_cfg = orbis_gb_addr_config();
   info->mc_arb_ramcfg = orbis_mc_arb_ramcfg();

   /* The tables themselves: generated from the kernel's cik_tiling_mode_table_init() 8-pipe branch by
    * tools/gen-tile-tables.py, with the bit positions cross-checked between gfx7.json and kernel-cikd.h.
    * ac_fill_tiling_info memcpy's both arrays wholesale, and reads PIPE_CONFIG out of slot
    * CIK_TILE_MODE_COLOR_2D (14) to derive num_tile_pipes - which lands on 8 because that slot carries
    * ADDR_SURF_P8_32x32_16x16, the value the console confirmed. */
   /* ⚠ BOTH VARIANTS ASSERTED, NOT JUST ONE. orbis_gb_tile_mode is a #define for the 2 KB table
    * (orbis_tile_tables.h:144), so these two lines used to check the size of the table the 4 KB arm does NOT
    * copy - while that arm memcpy's sizeof(orbis_gb_tile_mode_4kb) into the same destination. Same dimensions
    * today; the assert exists so that stops being something a reader has to go and confirm. */
   STATIC_ASSERT(sizeof(info->gb_tile_mode) == sizeof(orbis_gb_tile_mode_2kb));
   STATIC_ASSERT(sizeof(info->gb_macro_tile_mode) == sizeof(orbis_gb_macro_tile_mode_2kb));
   STATIC_ASSERT(sizeof(info->gb_tile_mode) == sizeof(orbis_gb_tile_mode_4kb));
   STATIC_ASSERT(sizeof(info->gb_macro_tile_mode) == sizeof(orbis_gb_macro_tile_mode_4kb));
   if (orbis_row_size_kb() == 4) {
      memcpy(info->gb_tile_mode, orbis_gb_tile_mode_4kb, sizeof(orbis_gb_tile_mode_4kb));
      memcpy(info->gb_macro_tile_mode, orbis_gb_macro_tile_mode_4kb, sizeof(orbis_gb_macro_tile_mode_4kb));
   } else {
      memcpy(info->gb_tile_mode, orbis_gb_tile_mode_2kb, sizeof(orbis_gb_tile_mode_2kb));
      memcpy(info->gb_macro_tile_mode, orbis_gb_macro_tile_mode_2kb, sizeof(orbis_gb_macro_tile_mode_2kb));
   }

   /* Last, because by here every number it prints has been computed by the code above rather than restated. */
   orbis_dump_shim_device();

   return 0;
}

/* ⚠ EXPORT THIS DEVICE INTO MESA'S OWN DRM-SHIM, so that STOCK, UNPATCHED RADV can be run against it on Linux.
 *
 * The question this exists to answer is the one the maintainer has asked from the start: does our glue produce
 * the same output as the Linux stack? Every attempt to answer it by reading the console's registers has now hit
 * the wall that config space is privileged there. This goes the other way round instead - it takes the device
 * DESCRIPTION out of here and gives it to a Linux userspace that has no PS4 in it at all.
 *
 * Mesa ships `src/amd/drm-shim/libamdgpu_noop_drm_shim.so`: LD_PRELOAD it, set AMDGPU_GPU_ID=<name>, and every
 * amdgpu ioctl is answered from a table in `src/amd/common/amdgpu_devices.c` while the submit ioctl does
 * nothing. Its oldest entry is Polaris - GFX8 - so there is no GFX7 part in it and one has to be added. The
 * table's shape is exactly what this file already computes: a `drm_amdgpu_info_device`, a memory info, and a
 * list of (dword register, instance, value) triples answering AMDGPU_INFO_READ_MMR_REG.
 *
 * WHICH REGISTERS, AND WHY THOSE. ac_linux_drm.c:705-750 is the whole query sequence RADV makes on a pre-GFX9
 * part, so the list below is not a guess about what to supply - it is that sequence, transcribed:
 *
 *     0x263d  per shader engine   CC_RB_BACKEND_DISABLE; RADV keeps bits 16-23 as backend_disable[se]
 *     0xa0d4  per shader engine   PA_SC_RASTER_CONFIG
 *     0xa0d5  per shader engine   PA_SC_RASTER_CONFIG_1
 *     0x263e  broadcast           GB_ADDR_CONFIG
 *     0x2644  broadcast, 32       GB_TILE_MODE0..31
 *     0x2664  broadcast, 16       GB_MACROTILE_MODE0..15
 *     0x09d8  broadcast           MC_ARB_RAMCFG
 *
 * ⚠ AND THAT LIST IS THE SAME SET CONFIG SPACE WOULD NOT GIVE UP. GB_ADDR_CONFIG and the tile tables are exactly
 * what ORBIS_READ_REGS died trying to read. So the shim is not a convenience: it is the only place where the
 * numbers this port synthesises can be handed to a driver that also has a working kernel, a real allocator and
 * a real command-stream dumper.
 *
 * RASTER CONFIG IS THE ONE VALUE HERE THAT IS NOT SYNTHESISED. It was read off the console - ps4/radv run 3,
 * PA_SC_RASTER_CONFIG = 0x2a00161a and _1 = 0, before RADV's preamble had run, so it is Sony's. The shim
 * therefore gets the real value while the console driver gets whatever ac_get_raster_config derives from the
 * family, which is Bonaire's 0x16000012. That difference is already known and measured (Tempest
 * ps4/opengothic/run-bk), and a shim run reproducing it is a POSITIVE sign - the export is faithful enough to
 * show a difference we independently established.
 *
 * ORBIS_DUMP_SHIM_DEVICE=1 prints the entry. Run it on the HOST build - build.sh --host-orbis compiles this
 * arm for Linux - and paste the output into amdgpu_devices.c as a patch.
 */
static void
orbis_dump_shim_device(void)
{
   if (getenv("ORBIS_DUMP_SHIM_DEVICE") == NULL)
      return;

   struct drm_amdgpu_info_device d;
   struct drm_amdgpu_memory_info m;
   if (orbis_query_dev_info(&d) != 0 || orbis_query_memory(&m) != 0) {
      mesa_loge("orbis-drm: SHIM_DEVICE could not query itself");
      return;
   }

   const uint32_t *tile  = (orbis_row_size_kb() == 4) ? orbis_gb_tile_mode_4kb : orbis_gb_tile_mode_2kb;
   const uint32_t *macro = (orbis_row_size_kb() == 4) ? orbis_gb_macro_tile_mode_4kb
                                                      : orbis_gb_macro_tile_mode_2kb;

   /* MEASURED on the console, ps4/radv run 3, before RADV's preamble had run - so Sony's, not ours. */
   const uint32_t sony_raster_config   = 0x2a00161a;
   const uint32_t sony_raster_config_1 = 0x00000000;

   mesa_logi("orbis-drm: ==== BEGIN amdgpu_devices.c ENTRY (paste into Mesa, then AMDGPU_GPU_ID=liverpool) ====");
   mesa_logi("   {");
   mesa_logi("      .name = \"liverpool\",");
   /* CHIP_LIVERPOOL now exists as a family of its own (amd_family.h), so the shim entry names it rather than
    * borrowing Bonaire's - the whole point of the export is that the shim describes THIS part. */
   mesa_logi("      .radeon_family = CHIP_LIVERPOOL,");
   mesa_logi("      .hw_ip_gfx = { .hw_ip_version_major = 7, .ib_start_alignment = 32,");
   mesa_logi("                     .ib_size_alignment = 32, .available_rings = 0x1, },");
   mesa_logi("      .hw_ip_compute = { .hw_ip_version_major = 7, .ib_start_alignment = 32,");
   mesa_logi("                         .ib_size_alignment = 32, .available_rings = 0xf, },");
   mesa_logi("      .mmr_regs = {");

   unsigned triples = 0;
   for (unsigned se = 0; se < d.num_shader_engines; ++se) {
      const uint32_t instance = (se << AMDGPU_INFO_MMR_SE_INDEX_SHIFT) |
                                (AMDGPU_INFO_MMR_SH_INDEX_MASK << AMDGPU_INFO_MMR_SH_INDEX_SHIFT);
      /* CC_RB_BACKEND_DISABLE: RADV keeps bits 16-23 as the DISABLED mask, and every render backend on this
       * part is enabled (enabled_rb_pipes_mask = 0xff), so the disabled mask is zero. */
      mesa_logi("         0x263d, 0x%08x, 0x00000000,   /* CC_RB_BACKEND_DISABLE, SE%u - nothing harvested */",
                instance, se);
      mesa_logi("         0xa0d4, 0x%08x, 0x%08x,   /* PA_SC_RASTER_CONFIG, SE%u - READ OFF THE CONSOLE */",
                instance, sony_raster_config, se);
      mesa_logi("         0xa0d5, 0x%08x, 0x%08x,   /* PA_SC_RASTER_CONFIG_1, SE%u */",
                instance, sony_raster_config_1, se);
      triples += 3;
   }
   mesa_logi("         0x263e, 0xffffffff, 0x%08x,   /* GB_ADDR_CONFIG */", orbis_gb_addr_config());
   mesa_logi("         0x09d8, 0xffffffff, 0x%08x,   /* MC_ARB_RAMCFG */", orbis_mc_arb_ramcfg());
   triples += 2;
   for (unsigned i = 0; i < 32; ++i) {
      mesa_logi("         0x%04x, 0xffffffff, 0x%08x,   /* GB_TILE_MODE%u */", 0x2644 + i, tile[i], i);
      ++triples;
   }
   for (unsigned i = 0; i < 16; ++i) {
      mesa_logi("         0x%04x, 0xffffffff, 0x%08x,   /* GB_MACROTILE_MODE%u */", 0x2664 + i, macro[i], i);
      ++triples;
   }
   mesa_logi("      },");
   mesa_logi("      .mmr_reg_count = %u,", triples);

   mesa_logi("      .dev = {");
   mesa_logi("         .device_id = 0x%x, .external_rev = 0x%x, .pci_rev = 0x%x, .family = %u,",
             d.device_id, d.external_rev, d.pci_rev, d.family);
   mesa_logi("         .num_shader_engines = %u, .num_shader_arrays_per_engine = %u,",
             d.num_shader_engines, d.num_shader_arrays_per_engine);
   mesa_logi("         .gpu_counter_freq = %u, .max_engine_clock = %llu, .max_memory_clock = %llu,",
             d.gpu_counter_freq, (unsigned long long)d.max_engine_clock,
             (unsigned long long)d.max_memory_clock);
   mesa_logi("         .cu_active_number = %u, .cu_ao_mask = 0x%x,", d.cu_active_number, d.cu_ao_mask);
   for (unsigned se = 0; se < 4; ++se)
      for (unsigned sh = 0; sh < 4; ++sh)
         if (d.cu_bitmap[se][sh] != 0)
            mesa_logi("         .cu_bitmap[%u][%u] = 0x%x,", se, sh, d.cu_bitmap[se][sh]);
   mesa_logi("         .enabled_rb_pipes_mask = 0x%x, .num_rb_pipes = %u, .num_hw_gfx_contexts = %u,",
             d.enabled_rb_pipes_mask, d.num_rb_pipes, d.num_hw_gfx_contexts);
   mesa_logi("         .ids_flags = 0x%llx, .virtual_address_offset = 0x%llx,",
             (unsigned long long)d.ids_flags, (unsigned long long)d.virtual_address_offset);
   mesa_logi("         .virtual_address_max = 0x%llx, .virtual_address_alignment = %u,",
             (unsigned long long)d.virtual_address_max, d.virtual_address_alignment);
   mesa_logi("         .pte_fragment_size = %u, .gart_page_size = %u,", d.pte_fragment_size, d.gart_page_size);
   mesa_logi("         .ce_ram_size = %u, .vram_type = %u, .vram_bit_width = %u,",
             d.ce_ram_size, d.vram_type, d.vram_bit_width);
   mesa_logi("         .num_shader_visible_vgprs = %u, .num_cu_per_sh = %u, .num_tcc_blocks = %u,",
             d.num_shader_visible_vgprs, d.num_cu_per_sh, d.num_tcc_blocks);
   mesa_logi("         .gs_vgt_table_depth = %u, .gs_prim_buffer_depth = %u, .max_gs_waves_per_vgt = %u,",
             d.gs_vgt_table_depth, d.gs_prim_buffer_depth, d.max_gs_waves_per_vgt);
   mesa_logi("         .gc_double_offchip_lds_buf = %u, .wave_front_size = %u,",
             d.gc_double_offchip_lds_buf, d.wave_front_size);
   mesa_logi("         .high_va_offset = 0x%llx, .high_va_max = 0x%llx,",
             (unsigned long long)d.high_va_offset, (unsigned long long)d.high_va_max);
   mesa_logi("      },");
   mesa_logi("      .mem = {");
   mesa_logi("         .gtt = { .total_heap_size = 0x%llx, .usable_heap_size = 0x%llx, },",
             (unsigned long long)m.gtt.total_heap_size, (unsigned long long)m.gtt.usable_heap_size);
   mesa_logi("         .vram = { .total_heap_size = 0x%llx, .usable_heap_size = 0x%llx, },",
             (unsigned long long)m.vram.total_heap_size, (unsigned long long)m.vram.usable_heap_size);
   mesa_logi("         .cpu_accessible_vram = { .total_heap_size = 0x%llx, .usable_heap_size = 0x%llx, },",
             (unsigned long long)m.cpu_accessible_vram.total_heap_size,
             (unsigned long long)m.cpu_accessible_vram.usable_heap_size);
   mesa_logi("      },");
   mesa_logi("   },");
   mesa_logi("orbis-drm: ==== END amdgpu_devices.c ENTRY (%u register triples) ====", triples);
}

int
ac_drm_query_hw_ip_count(ac_drm_device *dev, unsigned type, uint32_t *count)
{
   if (!dev || !count)
      return -EINVAL;
   if (type != AMDGPU_HW_IP_GFX)
      return -ENOSYS; /* the caller ignores failure here and leaves num_instances at 0 */
   *count = 1;
   return 0;
}

int
ac_drm_query_hw_ip_info(ac_drm_device *dev, unsigned type, unsigned ip_instance,
                        struct drm_amdgpu_info_hw_ip *info)
{
   if (!dev || !info)
      return -EINVAL;

   memset(info, 0, sizeof(*info));

   /* THIS IS WHERE THE NUMBER OF RINGS IS DECIDED, and the caller makes it easy: ac_query_gpu_info's loop
    * does `if (r) continue;` per IP type, so refusing a type leaves its num_queues at zero, and
    * radv_physical_device.c gates the compute queue family on
    * info.ip[AMD_IP_COMPUTE].num_queues > 0. One ring, and the second one is declined rather than
    * missing. See research/01-submission.md: Sony's compute queues go through sceGnmMapComputeQueue and
    * sceGnmDingDong, a different door from the DCB/CCB submit this port will use, and Vulkan permits a
    * driver to expose a single queue family that services graphics and compute both. */
   if (type != AMDGPU_HW_IP_GFX) {
      if (type == AMDGPU_HW_IP_COMPUTE)
         ORBIS_DRM_REFUSED("one ring on purpose - Sony's compute queues are a different door; "
                           "see research/01-submission.md");
      return -ENOSYS;
   }

   /* GFX IP 7.2, the version Bonaire reports for this generation - taken as the SHAPE of a gfx7 answer,
    * which is what the reference dump is for. ip_discovery_version stays 0 because gfx6-8 do not set it,
    * and ac_fill_hw_ip_info reads the major/minor pair instead when it is zero. */
   info->hw_ip_version_major = 7;
   info->hw_ip_version_minor = 2;

   /* One ring. num_queues is util_bitcount(available_rings). */
   info->available_rings = 0x1;

   /* 32 B, per research/01. The value barely matters: ac_fill_hw_ip_info takes
    * MAX3(ib_start_alignment, ib_size_alignment, 256), so 256 wins regardless - the kernel is documented
    * as reporting wrong limits for some queues and Mesa rounds them all up. */
   info->ib_start_alignment = 32;
   info->ib_size_alignment = 32;

   return 0;
}

int
ac_drm_query_firmware_version(ac_drm_device *dev, unsigned fw_type, unsigned ip_instance,
                              unsigned index, uint32_t *version, uint32_t *feature)
{
   if (!dev || !version || !feature)
      return -EINVAL;

   /* ZERO, AND ON THIS GENERATION THAT IS INERT RATHER THAN MERELY CAUTIOUS. Sony's firmware carries
    * Sony's version numbers; they are not on AMD's scale and comparing them to it would be meaningless.
    * So the question is what RADV does with the values, and on gfx7 the answer is nothing - every
    * consumer in the tree is gated on a later generation:
    *
    *   has_taskmesh_indirect0_bug  gfx_level == GFX10_3 && mec_fw_version < 100   (ac_gpu_info.c:1036)
    *   me_fw_feature comparisons   gfx_level >= GFX8                              (:1135, :1142, :1144)
    *   pfp_fw_version >= 2680      gfx_level == GFX12                             (ac_cmdbuf_cp.c:327)
    *   EVENT_WRITE_ZPASS           gfx_level >= GFX11                             (radv_query.c:100)
    *   ac_cmdbuf.h:325 / radv_dgc  take their gfx_level < GFX9 arm regardless
    *
    * If that ever stops being true, a workaround keyed on "old firmware" switches ON at zero, which is
    * the safe direction to be wrong in. */
   *version = 0;
   *feature = 0;
   return 0;
}

int
ac_drm_query_sw_info(ac_drm_device *dev, enum amdgpu_sw_info info, void *value)
{
   if (!dev || !value)
      return -EINVAL;

   switch (info) {
   case amdgpu_sw_info_address32_hi:
      /* DERIVED FROM WHERE THE MEMORY ACTUALLY IS, and the console is what taught this file the difference.
       *
       * RADV builds 64-bit addresses for its 32-bit-addressable allocations by packing a 32-bit offset with
       * this constant (radv_dgc.c: nir_pack_64_2x32_split(lo, info->address32_hi)), so the value NAMES which
       * 4 GiB window those allocations live in. This file used to report 0 and call that a contract phase 3
       * had to keep - reasoning that only held while the window was ours to choose.
       *
       * It is not. On hardware sceKernelMapDirectMemory put the arena at 0x200400000, and reporting 0 then
       * meant every RADEON_FLAG_32BIT request had to come from a 4 GiB window containing none of our memory:
       * vkCreateDevice failed with OUT_OF_DEVICE_MEMORY on RADV's first shader arena. Reporting the arena's
       * own high half instead makes the whole arena 32-bit-addressable, which is what the field is for. */
      *(uint32_t *)value = (uint32_t)(orbis_va_base >> 32);
      return 0;

   case amdgpu_sw_info_address_prt_wa_control_bit:
      /* ⚠ THIS ONE IS ASKED FOR ON THIS GENERATION, and I first wrote that it was not. gfx7 HAS the
       * has_smem_with_null_prt_bug - the reference dump reports it 1 for Bonaire - and ac_gpu_info queries
       * this bit precisely when that flag is set, treating failure as fatal.
       *
       * It is a BIT INDEX, not a boolean, and ac_nir_fixup_smem_loads_null_prt CLEARS it on every SMEM
       * address: nir_iand_imm(addr, ~(1ull << bit)). It also asserts the index is >= 32.
       *
       * 47 is the canonical GCN split between the low and high halves of a 48-bit VA, which is what the
       * bit means on hardware. Here it is additionally INERT, and that is the property to preserve: the VA
       * window this port reports tops out at 0x400000000 (34 bits), so no valid address ever sets bit 47,
       * clearing it never changes one, and ACO folds the mask away.
       *
       * THE INVARIANT: the allocator must never hand out a VA with bit 47 set. Trivially true for a 34-bit
       * space, and phase 3 has to keep it true if that window ever grows. */
      *(uint8_t *)value = 47;
      return 0;

   default:
      mesa_logw("orbis-drm: %s: unhandled sw_info %d", __func__, (int)info);
      return -ENOSYS;
   }
}

void
ac_drm_query_has_vm_always_valid(ac_drm_device *dev, struct radeon_info *info)
{
   /* Returns void AND writes into radeon_info, so a do-nothing body leaves the field at whatever it
    * happened to be - a wrong answer with no error anywhere, which is why this one could never be a stub.
    *
    * TRUE, and the reason is a property of the platform rather than a guess: everything this port maps is
    * resident for the lifetime of the mapping. There is no eviction, no per-submit residency list and no
    * page fault path - which is also why AMDGPU_CHUNK_ID_BO_HANDLES carries no information here
    * (research/01-submission.md). That is exactly what the flag asserts. */
   if (!dev || !info)
      return;
   info->has_vm_always_valid = true;
}

const char *
ac_drm_get_marketing_name(ac_drm_device *device)
{
   /* Not "AMD Radeon ..." - this is a semi-custom part and the name should say so. Returning a real
    * string rather than NULL because a NULL here shows up as an empty device name in every tool that
    * lists devices, which is confusing in exactly the phase where device enumeration is what is being
    * debugged. */
   return "PlayStation 4 Liverpool (RADV ORBIS)";
}

/* ---------------------------------------------------------------- refusals
 *
 * Three of the thirteen will never be implemented, and saying so in the log is the point: it stops the
 * next reader from treating an absence as an unfinished task.
 */

int
ac_drm_query_pci_bus_info(ac_drm_device *dev, struct radeon_info *info)
{
   ORBIS_DRM_REFUSED("there is no PCI bus to enumerate on this platform");
   return -ENOSYS;
}

int
ac_drm_query_video_caps_info(ac_drm_device *dev, unsigned cap_type, unsigned size, void *value)
{
   ORBIS_DRM_REFUSED("built with -Dvideo-codecs=, so there are no codecs to describe");
   return -ENOSYS;
}

int
ac_drm_query_uq_fw_area_info(ac_drm_device *dev, unsigned type, unsigned ip_instance,
                             struct drm_amdgpu_info_uq_metadata *info)
{
   ORBIS_DRM_REFUSED("user queues are GFX11+ and this is gfx7");
   return -ENOSYS;
}

/* ================================================================ the winsys half
 *
 * THIRTY-FIVE MORE FUNCTIONS, AND THE LINKER NAMED EVERY ONE. Building winsys/amdgpu/ for this platform -
 * rather than writing a PS4 winsys - turns those 3890 lines of BO management, command-stream building, IB
 * chaining and submission into callers of this file. The exact list came out of tools/linkprobe.sh once the
 * winsys was in the archive; it is not a grep's inference:
 *
 *     grep -oE "undefined symbol: ac_drm_[a-z0-9_]+" build.log | sed 's/.*: //' | sort -u    -> 35
 *
 * The point of stubbing all 35 at once is that the archive LINKS again, and a linking archive is what makes
 * the next question answerable: vkCreateDevice can run, and the log names the FIRST function it actually
 * needs rather than the first one somebody guessed. That is the loop that got phase 2 through thirteen
 * functions without a console, and it needs no console here either.
 *
 * ⚠ AND THE FIRST THING IT ALREADY FOUND: this file's sync provider had clone and finalize NULL, with a
 * comment saying clone was "only needed when a provider is duplicated". radv_amdgpu_winsys.c:204 duplicates
 * it, through the pointer, unchecked. Nothing was wrong with the reasoning - the caller simply did not exist
 * in the build yet. See the provider section above.
 */

/* ---------------------------------------------------------------- device identity, and both are real */

int
ac_drm_device_get_fd(ac_drm_device *dev)
{
   /* -1, deliberately. RADV stores this in ws->fd and only ever compares it or hands it to code that is
    * compiled out here. */
   return dev->fd;
}

uintptr_t
ac_drm_device_get_cookie(ac_drm_device *dev)
{
   /* A key, not a descriptor: radv_amdgpu_alloc_tracker_acquire() uses it to find the per-device allocation
    * tracker, so all it has to be is stable and unique per device. The device pointer is both. */
   return (uintptr_t)dev;
}

/* ---------------------------------------------------------------- submission bookkeeping, real
 *
 * Pure struct packing with no OS underneath, so it is the amdgpu arm's own body: the fence chunk carries
 * the BO holding the fence and a byte offset, and the offset the caller passes is in QWORDS. Getting that
 * multiplication wrong writes the end-of-pipe value four bytes into a neighbouring slot, which is the kind
 * of defect that looks like a GPU hang.
 */
void
ac_drm_cs_chunk_fence_info_to_data(uint32_t bo_handle, uint64_t offset,
                                   struct drm_amdgpu_cs_chunk_data *data)
{
   data->fence_data.handle = bo_handle;
   data->fence_data.offset = offset * sizeof(uint64_t);
}

/* ---------------------------------------------------------------- syncobjs, real over the same slots
 *
 * The provider's five entries and these two are the SAME objects - RADV creates syncobjs through both doors
 * (vk_sync goes through the provider, the winsys' internal queue syncobjs come here) and then waits on them
 * through one. Two separate pools would deadlock in a way that looks like a lost GPU.
 */

int
ac_drm_cs_create_syncobj2(ac_drm_device *dev, uint32_t flags, uint32_t *handle)
{
   return orbis_sync_create(dev->sync, flags, handle);
}

int
ac_drm_cs_destroy_syncobj(ac_drm_device *dev, uint32_t handle)
{
   return orbis_sync_destroy(dev->sync, handle);
}

/* ---------------------------------------------------------------- memory, BO and VA - phase 3
 *
 * Every right-hand side already exists in the Tempest fork (sceKernelAllocateDirectMemory, GnmAllocator's
 * arenas, GARLIC/ONION already distinguished), and this platform is EASIER than a PC here because the CPU
 * and the GPU share one address space - there is no separate GPU VA to map into.
 *
 * ⚠ TWO CONTRACTS PHASE 2 ALREADY PROMISED RADV, AND BOTH BREAK SILENTLY:
 *   1. amdgpu_sw_info_address32_hi is reported as 0, so every 32-bit-addressable allocation must live in
 *      the low 4 GB.
 *   2. address_prt_wa_control_bit is 47, and it is inert only while the VA window stays 34 bits. A NIR pass
 *      clears that bit from every SMEM address, so a wider window makes it clear a live one.
 */

/* ---------------------------------------------------------------- buffer objects
 *
 * THE LAYER SPLITS PHYSICAL MEMORY FROM ITS MAPPING, AND SO DOES THIS PLATFORM. That is the reason
 * ac_linux_drm fits a console at all, and it decides the shape of everything below:
 *
 *    ac_drm_bo_alloc      reserves PHYSICAL memory and nothing else   -> sceKernelAllocateDirectMemory
 *    ac_drm_bo_va_op_raw  maps a range of it AT AN ADDRESS RADV CHOSE -> sceKernelMapDirectMemory
 *    ac_drm_bo_cpu_map    hands back the address that mapping produced
 *
 * The winsys calls them in exactly that order (radv_amdgpu_bo.c: va_range_alloc, bo_alloc, export, va_op) and
 * the middle one is where CPU and GPU addresses become the same number - which on this hardware they are.
 * A design that allocated and mapped in one step would have to invent a GPU VA afterwards, and RADV has
 * already chosen one by then.
 *
 * THE HOST ARM IS A FAITHFUL ANALOGUE, NOT A FAKE. A memfd is a pool of pages addressed by offset, mapped
 * wherever asked - which is what direct memory is - so the same code runs on the laptop with the same
 * ordering, the same aliasing (RADEON_FLAG_VM_PAD_1PAGE maps one page at two addresses) and the same failure
 * modes. What it does NOT test is whether sceKernel agrees, and that distinction is the point of having a
 * seam here rather than a second implementation.
 */


/* The host pool: one memfd grown as needed, offsets bumped. No reuse of freed offsets yet - a bump allocator
 * is honest about being one, and 34 bits of address space is the constraint that matters first. */
#if !defined(__PS4__)
static int orbis_pool_fd = -1;
static uint64_t orbis_pool_used;
#endif

static struct orbis_bo *
orbis_bo_from_handle(uint32_t handle)
{
   if (handle == 0 || handle > orbis_bo_slots)
      return NULL;
   return orbis_bo_slot[handle - 1];
}

/* Reserve physical memory. Returns 0 and sets bo->phys_offset, or a negative errno. */
static int
orbis_backing_alloc(struct orbis_bo *bo)
{
#if defined(__PS4__)
   /* NOTHING TO DO: the arena is already allocated and already mapped, so the BO's pages are whichever bytes
    * of it the VA allocator hands out. phys_offset stays 0 because there is no second address space to
    * translate into - which is the whole reason this platform is simpler here than a PC. */
   return 0;
#else
   if (orbis_pool_fd < 0) {
      orbis_pool_fd = memfd_create("orbis-drm-pool", MFD_CLOEXEC);
      if (orbis_pool_fd < 0)
         return -errno;
   }

   const uint64_t align = MAX2(bo->alignment, 4096);
   const uint64_t offset = (orbis_pool_used + align - 1) & ~(align - 1);
   if (ftruncate(orbis_pool_fd, offset + bo->size) < 0)
      return -errno;

   orbis_pool_used = offset + bo->size;
   bo->phys_offset = offset;
   return 0;
#endif
}

static void
orbis_backing_free(struct orbis_bo *bo)
{
#if defined(__PS4__)
   /* sceKernelReleaseDirectMemory. Nothing to do until orbis_backing_alloc exists. */
#else
   /* The memfd keeps the pages; the bump allocator does not reclaim. Deliberate for now, and it is a leak
    * that shows up as address-space exhaustion with a loud message rather than as corruption. */
#endif
}

/* Map size bytes of the BO, starting at bo_offset within it, at the address RADV chose.
 *
 * ⚠ AMDGPU_VM_PAGE_* DESCRIBE THE GPU'S PAGE TABLE, NOT THE CPU'S MAPPING, and collapsing the two cost a
 * SIGSEGV that read like a driver bug. RADV marks an IB read-only (RADEON_FLAG_READ_ONLY) because the CP only
 * ever READS it - and on amdgpu that is a GPU permission, while the CPU keeps writing the same pages through
 * a separate mapping the GEM_MMAP ioctl produced. Here there is only ONE mapping, so honouring the flag made
 * the command stream read-only for its own writer:
 *
 *     0x200000-0x201000  rw-s   the fence BO
 *     0x201000-0x215000  r--s   the IB - and RADV faulted writing PM4 into it
 *
 * So the mapping is always writable and the GPU-side intent is NOT enforced. That is a real loss, not a
 * shrug: on amdgpu a GPU write to a read-only IB faults, and here it would corrupt silently. This platform
 * has one page table shared by both processors, so there is nowhere to record the distinction. */
#if defined(__PS4__)
/* FreeBSD's MAP_FIXED, which is what Sony's mapper takes: place this mapping AT the address given,
 * replacing whatever was there. Spelled out because the SDK headers do not name it. */
#define ORBIS_MAP_FIXED 0x0010

/* ⚠ THE TWO BUSES, AND WHICH BUFFERS BELONG ON WHICH.
 *
 * Every surface this driver has ever allocated lives in one WB_ONION arena: cached and coherent, so
 * every GPU access to it crosses the bus the CPUs are on and snoops their caches. GARLIC exists on
 * this console so that GPU traffic does not, and a native GNM backend would have used it without
 * thinking about it.
 *
 * It cost nothing visible while only one core ran. Giving OpenGothic's worker pool its six cores
 * made it measurable in one step: animation fell from 27.2 ms to 9.5, and the GPU's own time grew
 * by about 15 ms at the same instant, so the frame got WORSE - 61.6 ms to 77.2. The CPUs and the
 * GPU are fighting over the same bus.
 *
 * So a VRAM-domain BO gets REAL PHYSICAL BACKING from the GARLIC pool, mapped over the address RADV
 * already chose. The arena stops being memory and becomes what it should always have been: an
 * address-space reservation whose pages can be replaced.
 *
 * ⚠ WHAT THIS DOES NOT DO. GTT-domain buffers stay on ONION, which is right - they are the ones the
 * CPU reads and writes, and CPU reads from GARLIC are an order of magnitude slower. Nothing here
 * decides which domain RADV asks for; if RADV puts host-visible uploads in VRAM on this part, they
 * will land on the write-combined bus and need the store fence the submit path now emits.
 *
 * Returns true if the range was re-backed. Reversible: the arena's own page for any address is
 * arithmetic, so orbis_garlic_restore puts it back exactly. */
/* ⚠ ON BY DEFAULT, and the measurement is why. One run, same save, same scene, one variable:
 *
 *                        ONION only      VRAM on GARLIC
 *     app:animation         9.5 ms           5.1 ms
 *     app:world tick        9.1 ms           5.4 ms
 *     app:Renderer::draw   10.1 ms           8.2 ms
 *     app:submit+present   48.1 ms           2.8 ms
 *     ------------------------------------------------
 *     frame                77.2 ms          21.9 ms      15 fps -> 46 fps
 *     GPU wait                56%               0%
 *     cores burned           1.12             2.20
 *
 * The GPU's wait vanished, which was expected. What was not expected is that the game's own CPU
 * code got 40% faster - world tick and animation both - and so did the scan-out memcpy, from
 * 2130 MB/s to 3572. That is the size of the effect: coherent GPU traffic was not merely competing
 * for bandwidth, it was invalidating the CPUs' caches continuously.
 *
 * ORBIS_VRAM_GARLIC=0 turns it off, for comparing against every measurement taken before today. */
static bool orbis_vram_garlic_on(void)
{
   static int on = -1;
   if (on < 0) {
      const char *const s = getenv("ORBIS_VRAM_GARLIC");
      on = (s != NULL && s[0] == '0') ? 0 : 1;
      if (on == 0)
         mesa_logw("orbis-drm: ORBIS_VRAM_GARLIC=0 - every surface is back on the cache-coherent bus, "
                   "which cost this title two thirds of its frame rate. For comparison runs only.");
   }
   return on == 1;
}

static bool
orbis_garlic_back(struct orbis_bo *bo, uint64_t start, uint64_t bytes)
{
   if (!orbis_vram_garlic_on() || bo == NULL || bo->foreign)
      return false;
   if (!(bo->preferred_heap & AMDGPU_GEM_DOMAIN_VRAM))
      return false;
   /* One range per BO. RADV maps a BO once; a second map would need a list, and a list nobody has
    * exercised is worse than a refusal that says so. */
   if (bo->garlic_bytes != 0)
      return false;

   off_t gphys = 0;
   int32_t err = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), bytes,
                                               orbis_page_size(), ORBIS_KERNEL_WC_GARLIC, &gphys);
   if (err != 0) {
      static unsigned said;
      if (orbis_budget(&said, 4))
         mesa_logw("orbis-drm: no GARLIC for %llu KiB -> 0x%08x - this BO stays on the coherent bus",
                   (unsigned long long)(bytes / 1024), (unsigned)err);
      return false;
   }

   void *at = (void *)(uintptr_t)start;
   err = sceKernelMapDirectMemory(&at, bytes, ORBIS_GRAPHICS_PROT, ORBIS_MAP_FIXED, gphys,
                                  orbis_page_size());
   if (err != 0 || (uintptr_t)at != start) {
      /* ⚠ IF THIS EVER FIRES, THE ARENA'S OWN PAGES MAY NO LONGER BE AT THAT ADDRESS. Say so loudly:
       * a partially replaced mapping is a wrong picture at best and a fault at worst. */
      mesa_loge("orbis-drm: GARLIC map at 0x%" PRIx64 " (%llu KiB) -> 0x%08x, landed at %p",
                start, (unsigned long long)(bytes / 1024), (unsigned)err, at);
      sceKernelReleaseDirectMemory(gphys, bytes);
      return false;
   }

   bo->garlic_phys = gphys;
   bo->garlic_addr = start;
   bo->garlic_bytes = bytes;

   static unsigned said;
   if (orbis_budget(&said, 4))
      mesa_logi("orbis-drm: BO %u (%llu KiB, heap 0x%x) is on GARLIC at 0x%" PRIx64
                " - off the CPU's bus",
                bo->handle, (unsigned long long)(bytes / 1024), bo->preferred_heap, start);
   return true;
}

/* Put the arena's own pages back over a range and give the GARLIC chunk to the system.
 *
 * ⚠ THIS IS A TEARDOWN AND IT IS NOT SAFE WHILE THE GPU IS STILL WORKING ON THE RANGE. The pages
 * the GPU is reading stop being the pages that are there, and the physical memory behind them is
 * available to the next caller of sceKernelAllocateDirectMemory - so a frame in flight reads
 * whatever moves in. Every caller must therefore be one of:
 *
 *   - the retire queue, which runs it only once the fence label has passed the newest submission
 *     handed out. That is the normal path and it is why the queue carries the chunk at all;
 *   - a MAP over the same range, which is re-letting the address THIS INSTANT. Deferring is not
 *     available there and amdgpu has the same window: new page-table entries take effect at once;
 *   - ac_drm_bo_free on a BO the winsys never unmapped, which is already outside the contract.
 *
 * Split out of orbis_garlic_restore so the queue can run it from an entry rather than from a
 * struct orbis_bo, which by then may have been freed. */
static void
orbis_garlic_put_back(uint64_t garlic_addr, uint64_t garlic_bytes, off_t garlic_phys)
{
   if (garlic_bytes == 0)
      return;

   void *at = (void *)(uintptr_t)garlic_addr;
   const off_t phys = (off_t)(orbis_arena_phys + (garlic_addr - orbis_arena_map_base));
   const int32_t err = sceKernelMapDirectMemory(&at, garlic_bytes, ORBIS_GRAPHICS_PROT,
                                                ORBIS_MAP_FIXED, phys, orbis_page_size());
   if (err != 0)
      mesa_loge("orbis-drm: could not put the arena back at 0x%" PRIx64 " -> 0x%08x - that range is "
                "now a hole and the next BO to land there will fault",
                garlic_addr, (unsigned)err);

   sceKernelReleaseDirectMemory(garlic_phys, garlic_bytes);
}

/* Detach the chunk from the BO without touching the mapping. The caller owns it afterwards - either
 * to park on the retire queue or to put back at once. */
static void
orbis_garlic_take(struct orbis_bo *bo, uint64_t *addr, uint64_t *bytes, off_t *phys)
{
   *addr = 0;
   *bytes = 0;
   *phys = 0;
   if (bo == NULL || bo->garlic_bytes == 0)
      return;

   *addr = bo->garlic_addr;
   *bytes = bo->garlic_bytes;
   *phys = bo->garlic_phys;
   bo->garlic_phys = 0;
   bo->garlic_addr = 0;
   bo->garlic_bytes = 0;
}

/* Put the arena's own pages back where they were, NOW. See orbis_garlic_put_back for when that is
 * allowed; the deferred path is orbis_backing_unmap parking the chunk on the retire queue. */
static void
orbis_garlic_restore(struct orbis_bo *bo)
{
   uint64_t addr, bytes;
   off_t phys;

   orbis_garlic_take(bo, &addr, &bytes, &phys);
   orbis_garlic_put_back(addr, bytes, phys);
}
#endif

/* ⚠ ARE THIS CONSOLE'S DIRECT-MEMORY CALLS SAFE TO MAKE CONCURRENTLY? Nobody has ever asked, and
 * until this week nothing could: the title is one thread through this path.
 *
 * dEQP-VK.api.object_management.multithreaded_per_thread_device.device_memory_small hangs. The
 * watchdog is unambiguous for 132 periods - eleven minutes - and it is the branch that names an
 * absence rather than a culprit:
 *
 *     NO lock of this driver is held and NOBODY is asleep in futex_wait - so the block is not a
 *     Mesa lock at all
 *
 * ⚠ AND THE ARM TAKES THESE CALLS OUT OF EVERY LOCK ON PURPOSE. The unmap path says so in its own
 * comment: "a kernel call under orbis_map_lock is a stall every other thread pays for". That was
 * the right call for a title that never has two threads here, and it is exactly what a test with a
 * device per thread does not do.
 *
 * So this knob serialises them, and it is a QUESTION rather than a fix:
 *
 *     ORBIS_SERIALISE_KERNEL_MEM=1
 *
 *   the hang goes away  -> this platform's direct-memory calls cannot be made concurrently, which
 *                          is a platform fact worth knowing and worth paying a lock for
 *   the hang stays      -> they are not where it blocks, and one more place is eliminated
 *
 * Off by default until a run answers, because a global lock on the allocation path is a real cost
 * to the title and it should not be paid for a suspicion.
 */
static int
orbis_backing_map_locked(struct orbis_bo *bo, uint64_t bo_offset, uint64_t size, uint64_t addr, bool gpu_read_only)
{
   if (gpu_read_only) {
      static unsigned said;
      if (orbis_budget(&said, 1)) {
         mesa_logi("orbis-drm: GPU-side read-only mappings are not enforceable here - one shared page table");
      }
   }

#if defined(__PS4__)
   /* Two things are still checked rather than assumed, because a silently-wrong range here is a GPU read of
    * somebody else's memory. */
   if (addr < orbis_va_base || addr + size > orbis_va_end) {
      mesa_loge("orbis-drm: VA op outside the arena: 0x%" PRIx64 " + %" PRIu64, addr, size);
      return -EINVAL;
   }
   if (bo_offset != 0) {
      /* A second mapping of part of a BO at a different address - RADEON_FLAG_VM_PAD_1PAGE's guard page. This arena
       * cannot alias one physical page at two addresses, so the pad page gets the arena's own memory at that
       * address: a read-only overrun absorber that reads a wrong value rather than faulting, which is what the pad
       * page is for. RADV reserves the address, so nothing else can be there. */
      static unsigned said;
      if (orbis_budget(&said, 1)) {
         mesa_logi("orbis-drm: a pad page maps the arena's own memory at that address, not an alias of page 0");
      }
   }

   if (!orbis_arena_unmapped)
      return 0; /* the old flat arena: everything is mapped already */

   /* ⚠ MAP THIS RANGE FOR REAL, THE WAY amdgpu DOES. phys is arithmetic because the arena is one contiguous
    * physical allocation mapped at one contiguous address, so the page behind any address in the window is known
    * exactly - which is what makes unmapping reversible.
    *
    * Alignment is the PAGE, not ORBIS_DIRECT_ALIGN: a range RADV asks for is page-aligned and can sit anywhere,
    * while the 2 MiB alignment the arena itself was allocated with would refuse most of them. */
   {
      /* ⚠ NO SLACK. I granted one page past every mapping for a run, on the claim that Linux absorbs small
       * overruns because VA ranges are packed - and the maintainer was right to call that unfounded. What the code
       * actually says (radv_buffer.c:274-281):
       *
       *     "Pad the BO with an extra VM page to mitigate OOB access from SMEM instructions."
       *     if (device->compiler_info.key.mitigate_smem_oob && !is_internal) flags |= RADEON_FLAG_VM_PAD_1PAGE;
       *     if (pdev->info.has_smem_partial_oob_access_bug && !is_internal)  flags |= RADEON_FLAG_VM_PAD_1PAGE;
       *
       * and (ac_gpu_info.c:1064):
       *
       *     info->has_smem_partial_oob_access_bug = info->gfx_level == GFX9 && ...
       *
       * So the pad page is a GFX9-only hardware-bug mitigation of exactly one page, not a general absorber, and this
       * chip is GFX7 - RADV is CORRECT not to ask for one here. Granting slack was leniency Linux does not have, and
       * it did not help: the writes are ~2 MiB past a live buffer, at addresses no BO was ever given. Those
       * are real out-of-bounds stores, and this protection is what makes them visible. */
      const uint64_t page = orbis_page_size();
      const uint64_t start = addr & ~(page - 1);
      /* ⚠ `ORBIS_SLACK_PAGES=<n>` GRANTS n PAGES PAST THIS MAPPING, AND IT IS AN INSTRUMENT, NOT A FIX.
       *
       * Default 0: strict, which is what caught the real defect. But strict also means the title dies on the loading
       * screen, and the maintainer's answer to "describe the picture" was the right one - there is no picture to
       * describe when it crashes before the world appears.
       *
       * The decoy taught this lesson already: a fault that kills the process hides everything behind it, and making it
       * survivable is what turns an invisible defect into an observable one. So the slack is available on demand, for
       * runs whose deliverable is what the SCREEN looks like - comparing textures between two GB_ADDR_CONFIG values,
       * say, which cannot be done from a log at all.
       *
       * Labelled, off by default, and NOT the correctness fix I earlier justified with an invented claim about how
       * Linux packs allocations. */
      const char *const slack_s = getenv("ORBIS_SLACK_PAGES");
      const uint64_t    slack = slack_s != NULL ? strtoull(slack_s, NULL, 10) * page : 0;
      const uint64_t bytes = ((addr + size + page - 1) & ~(page - 1)) - start + slack;
      const off_t    phys = (off_t)(orbis_arena_phys + (start - orbis_arena_map_base));
      void          *at = (void *)(uintptr_t)start;

      /* ⚠ A MAP CANCELS ANY PARKED TEARDOWN OF THE SAME RANGE, AND FORGETTING THAT PRODUCED SEVEN WRITE FAULTS.
       *
       * The retire queue defers taking access away until the GPU has passed the last submission handed out. But a
       * range can be mapped again in the meantime, and the console showed exactly that:
       *
       *     line   209:  BO 19 maps   0x21d200000..0x225200000     128 MiB, heap 0x2
       *     line 23780:  BO 19 UNMAPS 0x21d200000..0x225204000
       *     line 23782:  BO 19 maps   0x21d200000..0x225200000     <- alive again, two lines later
       *
       * The parked entry then fired and revoked access to a range that was live, and every write into BO 19 faulted -
       * seven of them, at 0x221568000, 0x2215b8000, 0x221598000, 0x2219a0000, 0x2219a8000. My own bug, and the
       * mechanism was a deferred teardown with no way to cancel.
       *
       * amdgpu has no such window: a MAP writes the page tables, and nothing deferred can undo it afterwards. So a
       * grant drops every parked entry it overlaps. */
      /* ⚠ AND A CANCELLED ENTRY MAY BE HOLDING A GARLIC CHUNK, which is the one thing a cancel
       * cannot simply drop. The chunk is a real mapping over this address and real physical memory
       * on loan; leaving it parked would have orbis_garlic_back allocate a SECOND chunk over the
       * same range, and the parked restore would later map the arena back over the live one and
       * leak the loan. So the chunk goes back HERE, synchronously - which is allowed for the same
       * reason the mprotect grant below is: the address is being re-let this instant, so there is
       * nothing left to defer for.
       *
       * One chunk per pass, outside the lock, and the lock is orbis_retire_lock rather than
       * orbis_map_lock - see the ring's own comment for why it may not borrow whichever lock the
       * caller happens to be holding. The loop re-scans until nothing overlaps. */
      for (;;) {
         uint64_t g_addr = 0, g_bytes = 0;
         int64_t  g_phys = 0;

         simple_mtx_lock(&orbis_retire_lock);
         for (unsigned i = 0; i < orbis_retire_count; ++i) {
            struct orbis_retire *const r = &orbis_retire_q[(orbis_retire_head + i) % ORBIS_RETIRE_MAX];
            const bool hits_range = r->size != 0 && start < r->addr + r->size && r->addr < start + bytes;
            const bool hits_chunk = r->garlic_bytes != 0 && start < r->garlic_addr + r->garlic_bytes &&
                                    r->garlic_addr < start + bytes;
            if (!hits_range && !hits_chunk)
               continue;

            r->size = 0; /* cancelled: this range is live again */
            if (r->garlic_bytes != 0) {
               g_addr = r->garlic_addr;
               g_bytes = r->garlic_bytes;
               g_phys = r->garlic_phys;
               r->garlic_addr = 0;
               r->garlic_bytes = 0;
               r->garlic_phys = 0;
               break; /* out of the scan, so the kernel call happens without the lock */
            }
         }
         simple_mtx_unlock(&orbis_retire_lock);

         if (g_bytes == 0)
            break;
         orbis_garlic_put_back(g_addr, g_bytes, (off_t)g_phys);
      }

      const int32_t err = sceKernelMprotect((const void *)(uintptr_t)start, (size_t)bytes, ORBIS_GRAPHICS_PROT);
      if (err != 0) {
         mesa_loge("orbis-drm: granting access to 0x%" PRIx64 "..0x%" PRIx64 " failed: 0x%08x", start, start + bytes,
                   (unsigned)err);
         return -ENOMEM;
      }
      /* ⚠ TRACED, BECAUSE I HAVE BEEN REASONING ABOUT THE TIMING OF MY OWN MECHANISM WITH NO RECORD OF IT. Three
       * runs in a row I explained a write fault by guessing when a range lost access relative to the unmap and to
       * the submissions in flight. `ORBIS_TRACE_PROT=1` prints it, so the next explanation is read rather than
       * constructed. */
      if (getenv("ORBIS_TRACE_PROT") != NULL)
         mesa_logi("orbis-prot: GRANT 0x%" PRIx64 "..0x%" PRIx64 " at submit #%" PRIu64 " (label %u)", start,
                   start + bytes, orbis_submit_seq_no, *orbis_fence_label);

      /* After the grant, not before: the mapping this replaces has to exist and be reachable first,
       * so that a refusal leaves the range in the state it was already in. */
      orbis_garlic_back(bo, start, bytes);
   }
   return 0;
#else
   const int prot = PROT_READ | PROT_WRITE;
   /* MAP_FIXED_NOREPLACE, NOT MAP_FIXED: plain MAP_FIXED would silently unmap whatever the process already
    * had at that address, which for a VA window this low could be the binary itself. Failing loudly turns
    * "our window overlaps the host process" into a message instead of a corrupted run - and if it ever
    * fires, the fix is to move ORBIS_VA_BASE, not to weaken this flag. */
   void *p = mmap((void *)(uintptr_t)addr, size, prot, MAP_SHARED | MAP_FIXED_NOREPLACE, orbis_pool_fd,
                  bo->phys_offset + bo_offset);
   if (p == MAP_FAILED)
      return -errno;
   if ((uintptr_t)p != addr) {
      munmap(p, size);
      mesa_loge("orbis-drm: the pool would not map at 0x%" PRIx64 " - move ORBIS_VA_BASE", addr);
      return -ENOMEM;
   }
   return 0;
#endif
}

/* Takes access away from a range for real. Called only once the GPU can no longer be writing to it. */
static void
orbis_protect_now(uint64_t addr, uint64_t size)
{
#if defined(__PS4__)
   if (getenv("ORBIS_TRACE_PROT") != NULL)
      mesa_logi("orbis-prot: REVOKE 0x%" PRIx64 "..0x%" PRIx64 " (label %u, last submit #%" PRIu64 ")", addr,
                addr + size, *orbis_fence_label, orbis_submit_seq_no);
   const uint64_t page = orbis_page_size();
   const uint64_t start = addr & ~(page - 1);
   const uint64_t bytes = ((addr + size + page - 1) & ~(page - 1)) - start;
   const int32_t  err = sceKernelMprotect((const void *)(uintptr_t)start, (size_t)bytes, ORBIS_PROT_NONE);
   if (err != 0) {
      static unsigned said;
      if (orbis_budget(&said, 8)) {
         mesa_logw("orbis-drm: taking access away from 0x%" PRIx64 "..0x%" PRIx64 " failed: 0x%08x - that range stays "
                   "writable, so a stale access to it will be silent",
                   start, start + bytes, (unsigned)err);
      }
   }
#else
   (void)addr;
   (void)size;
#endif
}

/* Retires everything the GPU has finished with. Called from the submit path and from every unmap, so the queue drains
 * without a thread of its own. */
static void
orbis_retire_ranges(void)
{
#if defined(__PS4__)
   if (!orbis_arena_unmapped)
      return;
   /* ⚠ THE GPU MUST HAVE CAUGHT UP WITH EVERYTHING, NOT WITH THE SUBMISSION THAT WAS CURRENT WHEN THE RANGE WAS
    * FREED. The trace is what settled this, after three runs of me reasoning about it:
    *
    *     PARK   0x21d200000..0x225204000 behind submit #5982 (label 5980)
    *     REVOKE 0x21d200000..0x225204000 (label 5982, last submit #5983)
    *
    * The park was honoured exactly - the label reached 5982 - and submission #5983 was already in flight, and #5983
    * writes into that range. Freeing a buffer bounds nothing about later submissions: they can still hold its address
    * in a descriptor or a pointer captured earlier, which is the same class of stale reference as the reads.
    *
    * So the rule is the conservative one: a range is released only once the GPU has reached the LATEST submission
    * handed out, measured at drain time rather than at free time. That is cheap here rather than expensive, because
    * the present path already waits for GPU idle every frame - so ranges retire about once per frame, which is as
    * often as anything needs them back. */
   const uint32_t label = *orbis_fence_label;
   const uint32_t newest = (uint32_t)orbis_submit_seq_no;
   if ((int32_t)(label - newest) < 0)
      return; /* the GPU is still behind: nothing may be released yet */

   /* ⚠ A "DRAIN ONLY WHEN THE FENCE MOVED" SKIP WENT HERE AND WAS REVERTED, and the reasoning that
    * put it there is worth keeping so it is not reinvented.
    *
    * The observation was sound: with ZERO submissions label and newest are both 0, so the guard
    * above never fires and the drain runs on EVERY unmap. That is real, it is what the CTS walked
    * into, and it is why one syscall per range mattered.
    *
    * The fix was not. Skipping the drain until the fence moves means a range that is freed and
    * re-let between two fences never has its rights taken away in between - so a stale reference
    * reads the new tenant SILENTLY instead of faulting. I argued at the time that delaying
    * protection "goes in the safe direction". It does not: this protection is a DETECTOR, and
    * making a detector fire less often converts a loud failure into a quiet one. The title came
    * back with the artefacts it had before, and zero GPU faults - which is exactly that signature.
    *
    * The per-unmap cost is real and still wants solving, but not by protecting less. Coalescing
    * below is the half that was safe: it changes how many syscalls cover a set of pages, never
    * which pages end up protected. */

   /* Ranges whose owner is gone and whose last submission has retired: now they may be reused. Walked under the VA
    * lock, and the list is short - a Gothic boot keeps ~150 live ranges. */
   simple_mtx_lock(&orbis_va_lock);
   for (struct amdgpu_va **link = &orbis_va_list; *link;) {
      struct amdgpu_va *const va = *link;
      if (va->retired) { /* the GPU has caught up with everything, checked above */
         *link = va->next;
         free(va);
         continue;
      }
      link = &va->next;
   }
   simple_mtx_unlock(&orbis_va_lock);

   /* ⚠ ONE SYSCALL PER RANGE WAS THE OTHER HALF, and task #20 predicted it when the scheme was
    * built: "cost: one syscall per freed range, on a path that runs often. Measure before
    * shipping." It was never measured, and the CTS measured it the hard way.
    *
    * Ranges are parked in the order they are freed, and RADV frees out of a suballocator, so
    * consecutive entries are very often adjacent or touching. Merging them costs a comparison and
    * saves a syscall each time. Anything left unmerged is one call as before, so this is never
    * worse. */
   uint64_t run_addr = 0, run_end = 0;

   simple_mtx_lock(&orbis_retire_lock);
   while (orbis_retire_count != 0) {
      const struct orbis_retire *const r = &orbis_retire_q[orbis_retire_head];
      /* Every parked range was parked behind a submission no newer than the newest, and the newest has retired, so
       * the queue drains whole. The comparison stays as an assertion of that reasoning rather than as a filter. */
      if ((int32_t)(label - (uint32_t)r->seq) < 0)
         break;

      /* ⚠ THE CHUNK GOES BACK BEFORE THE PROTECT, NOT AFTER, and the order is load-bearing: putting
       * the arena's pages back is a fresh MAP_FIXED mapping with ORBIS_GRAPHICS_PROT, so it hands
       * the rights back to a range this loop is about to take them away from. Done in this order
       * the protect lands on the arena's own pages and stands; done in the other, the revoke is
       * silently undone and the detector stops detecting.
       *
       * A coalesced run cannot interleave the two wrongly. Runs only merge ranges that touch, so a
       * chunk restored here belongs either to the run being built - whose protect is issued after
       * this entry is popped - or to a run that does not overlap it at all. */
      orbis_garlic_put_back(r->garlic_addr, r->garlic_bytes, (off_t)r->garlic_phys);

      if (r->size != 0) {
         if (run_end != 0 && r->addr <= run_end && r->addr + r->size >= run_addr) {
            /* Touches or overlaps the run being built: widen it rather than issuing a call. */
            if (r->addr < run_addr)
               run_addr = r->addr;
            if (r->addr + r->size > run_end)
               run_end = r->addr + r->size;
         } else {
            if (run_end != 0)
               orbis_protect_now(run_addr, run_end - run_addr);
            run_addr = r->addr;
            run_end = r->addr + r->size;
         }
      }

      orbis_retire_head = (orbis_retire_head + 1) % ORBIS_RETIRE_MAX;
      --orbis_retire_count;
   }

   if (run_end != 0)
      orbis_protect_now(run_addr, run_end - run_addr);
   simple_mtx_unlock(&orbis_retire_lock);
#endif
}

static int
orbis_backing_unmap_locked(uint64_t addr, uint64_t size, struct orbis_bo *bo)
{
#if defined(__PS4__)
   if (!orbis_arena_unmapped) {
      /* The old flat arena: an unmap is bookkeeping the VA allocator already did, and there is no
       * retire queue to park anything on - so a chunk still attached here has to go back now. */
      orbis_garlic_restore(bo);
      return 0;
   }

   /* ⚠ UNMAP FOR REAL, WHICH IS THE WHOLE POINT. The old comment here said a hole "no later allocation could fill" -
    * that was true only while nothing could map a range back, and orbis_backing_map does now. A hole is exactly what
    * is wanted: a stale read or write into it FAULTS, at the instruction that makes the mistake, instead of quietly
    * finding the next tenant's data. */
   {
      const uint64_t page = orbis_page_size();
      const uint64_t start = addr & ~(page - 1);
      const uint64_t bytes = ((addr + size + page - 1) & ~(page - 1)) - start;
      /* Park it behind the last submission handed out, the way a kernel reference would, and drain whatever the GPU
       * has already finished with. */
      orbis_retire_ranges();
      /* Taken after the drain, not around it: orbis_retire_ranges takes this lock itself and
       * simple_mtx is not recursive. */
      simple_mtx_lock(&orbis_retire_lock);
      if (orbis_retire_count == ORBIS_RETIRE_MAX) {
         static unsigned said;
         if (orbis_budget(&said, 4)) {
            mesa_logw("orbis-drm: the retire queue is full at %u ranges - retiring the oldest EARLY, which is exactly "
                      "the use-after-free this queue exists to prevent. Raise ORBIS_RETIRE_MAX.",
                      (unsigned)ORBIS_RETIRE_MAX);
         }
         const struct orbis_retire *const old_r = &orbis_retire_q[orbis_retire_head];
         /* Same order as the drain: the pages come back first, then the rights go away. */
         orbis_garlic_put_back(old_r->garlic_addr, old_r->garlic_bytes, (off_t)old_r->garlic_phys);
         if (old_r->size != 0)
            orbis_protect_now(old_r->addr, old_r->size);
         orbis_retire_head = (orbis_retire_head + 1) % ORBIS_RETIRE_MAX;
         --orbis_retire_count;
      }
      if (getenv("ORBIS_TRACE_PROT") != NULL)
         mesa_logi("orbis-prot: PARK 0x%" PRIx64 "..0x%" PRIx64 " behind submit #%" PRIu64 " (label %u)", start,
                   start + bytes, orbis_submit_seq_no, *orbis_fence_label);
      const unsigned slot = (orbis_retire_head + orbis_retire_count) % ORBIS_RETIRE_MAX;
      orbis_retire_q[slot].addr = start;
      orbis_retire_q[slot].size = bytes;
      orbis_retire_q[slot].seq = orbis_submit_seq_no;
      /* The chunk is taken off the BO here rather than restored: the BO can be freed the moment
       * this returns, and the queue must not hold a pointer to it. Zeroes for a BO that has none. */
      {
         uint64_t g_addr, g_bytes;
         off_t    g_phys;
         orbis_garlic_take(bo, &g_addr, &g_bytes, &g_phys);
         orbis_retire_q[slot].garlic_addr = g_addr;
         orbis_retire_q[slot].garlic_bytes = g_bytes;
         orbis_retire_q[slot].garlic_phys = (int64_t)g_phys;
      }
      ++orbis_retire_count;
      simple_mtx_unlock(&orbis_retire_lock);
      return 0;
   }
#else
   return munmap((void *)(uintptr_t)addr, size) ? -errno : 0;
#endif
}

int
ac_drm_bo_alloc(ac_drm_device *dev, struct amdgpu_bo_alloc_request *alloc_buffer, ac_drm_bo *bo)
{
#if defined(__PS4__)
   orbis_kc_hit(ORBIS_KC_BO_ALLOC);
#endif
   struct orbis_bo *obo = calloc(1, sizeof(*obo));
   if (!obo)
      return -ENOMEM;

   obo->size = alloc_buffer->alloc_size;
   obo->alignment = alloc_buffer->phys_alignment;
   obo->preferred_heap = alloc_buffer->preferred_heap;
   obo->alloc_flags = alloc_buffer->flags;

   int r = orbis_backing_alloc(obo);
   if (r) {
      free(obo);
      return r;
   }

   simple_mtx_lock(&orbis_bo_lock);
   obo->handle = orbis_bo_slot_claim(obo);
   simple_mtx_unlock(&orbis_bo_lock);
   if (obo->handle != 0) {
      bo->abo = obo;
      return 0;
   }

   orbis_backing_free(obo);
   free(obo);
   return -ENOMEM;
}

int
ac_drm_bo_free(ac_drm_device *dev, ac_drm_bo bo)
{
#if defined(__PS4__)
   orbis_kc_hit(ORBIS_KC_BO_FREE);
#endif
   struct orbis_bo *obo = bo.abo;
   if (!obo)
      return -EINVAL;

   simple_mtx_lock(&orbis_bo_lock);
   if (obo->handle)
      orbis_bo_slot[obo->handle - 1] = NULL;
   simple_mtx_unlock(&orbis_bo_lock);

   /* The MAPPING is the winsys' to undo through va_op_raw(UNMAP) before it gets here; this releases the
    * PHYSICAL memory. Doing both would unmap ranges the winsys still believes it owns. */
#if defined(__PS4__)
   /* ⚠ EXCEPT THE GARLIC CHUNK, which is physical memory this arm allocated and nobody else knows
    * about. If the winsys freed the BO without unmapping it, that chunk would be lost for the life
    * of the process and the address would be left as a hole. Restoring is idempotent - it does
    * nothing when the UNMAP already ran, which is the normal path. */
   orbis_garlic_restore(obo);
#endif
   if (!obo->foreign)
      orbis_backing_free(obo);
   free(obo);
   return 0;
}

int
ac_drm_bo_cpu_map(ac_drm_device *dev, ac_drm_bo bo, void **cpu)
{
   /* NOT optional on this platform the way it is on amdgpu: radv_amdgpu_winsys_bo_map reaches the kernel
    * through a DRM_AMDGPU_GEM_MMAP ioctl there, and patches/0007 routes it here instead - so this is the
    * only door to a mapped BO.
    *
    * There is nothing to map: va_op_raw already did it, at the address RADV picked. Returning that address
    * is the whole body, because CPU and GPU share it. */
   struct orbis_bo *obo = bo.abo;
   if (!obo)
      return -EINVAL;

   if (!obo->cpu) {
      /* A BO asked for by the CPU before anything mapped it. Refusing is right - inventing a second mapping
       * at a different address would give the CPU and the GPU two views of one buffer. */
      mesa_logw("orbis-drm: cpu_map on a BO with no VA mapping yet (handle %u)", obo->handle);
      return -EINVAL;
   }

   /* *cpu carries a REQUESTED address on the way in (the virtio arm documents this), and RADV only uses that
    * for its replace path. Honouring it silently would hand back memory other than this BO's. */
   if (*cpu && *cpu != obo->cpu) {
      mesa_logw("orbis-drm: cpu_map at a fixed address is not supported here");
      return -EINVAL;
   }

   *cpu = obo->cpu;
   return 0;
}

int
ac_drm_bo_cpu_unmap(ac_drm_device *dev, ac_drm_bo bo)
{
   /* A no-op that SUCCEEDS, and the reason is the same as cpu_map's: the mapping belongs to the VA op, not to
    * this call. Tearing it down here would unmap memory the GPU is still addressing. */
   return bo.abo ? 0 : -EINVAL;
}

int
ac_drm_bo_query_info(ac_drm_device *dev, uint32_t bo_handle, struct amdgpu_bo_info *info)
{
   struct orbis_bo *obo = orbis_bo_from_handle(bo_handle);
   if (!obo)
      return -EINVAL;

   memset(info, 0, sizeof(*info));
   info->alloc_size = obo->size;
   info->phys_alignment = obo->alignment;
   info->preferred_heap = obo->preferred_heap;
   info->alloc_flags = obo->alloc_flags;
   if (obo->has_metadata)
      info->metadata = obo->metadata;
   return 0;
}

int
ac_drm_bo_set_metadata(ac_drm_device *dev, uint32_t bo_handle, struct amdgpu_bo_metadata *info)
{
   /* Tiling and format description for a shared surface. Nothing on this platform reads it back yet - there
    * is no other process to hand a surface to - but recording it is cheap and query_info promises it. */
   struct orbis_bo *obo = orbis_bo_from_handle(bo_handle);
   if (!obo)
      return -EINVAL;

   obo->metadata = *info;
   obo->has_metadata = true;
   return 0;
}

/* THE VA OPS ARE THE MAPPING, and both spellings land here. raw2 adds a timeline syncobj so the caller can
 * wait for the page-table update; there is no asynchronous page-table update here - the mapping is done when
 * the call returns - so the extra arguments are accepted and ignored, which is stronger than honouring them
 * would be. */
/* ⚠ TAKE THE RIGHTS OFF A FREED RANGE, WHICH THIS PLATFORM CAN ACTUALLY DO.
 *
 * MEASURED, not assumed (by a three-rung harness, deleted 2026-08-22 once it had answered): sceKernelMprotect reaches the
 * GPU's page tables. Rung 0 proved the harness reads the page, rung 1 took the rights off 0x20a204000 and the
 * query confirmed prot 0x0 where 0x33 had been, rung 2's copy delivered nothing, and the klog of the same
 * second carried a GPU protection fault at that exact address from client CPG - the command processor running
 * the copy. So amdgpu's teardown is REPRODUCIBLE here, not merely imitable by holding addresses back.
 *
 * That matters because every defect this port has left has one shape: an address released, still referenced,
 * and still readable, because a flat arena never unmaps. With the rights removed, a stale access faults AT THE
 * POINT OF USE with its address in the klog, instead of silently reading whoever moved in.
 *
 * ⚠ WHOLE PAGES ONLY, AND THE PAGE HERE IS 16 KiB. Revoking rights on a page that a NEIGHBOUR also occupies
 * would kill a live buffer, so the revoke covers only the pages entirely inside the range - align the start up,
 * the end down - and a range smaller than a page protects nothing at all. That is not a flaw to hide: RADV
 * allocates plenty below 16 KiB, so the log states how many ranges were skipped for it. A partial scheme
 * reported as a total one is the failure mode this file keeps paying for.
 *
 * The restore is the mirror image and deliberately WIDER - align the start down, the end up - because a page
 * this range shares with a neighbour may have been revoked when that neighbour died, and the new tenant needs
 * all of what it touches.
 *
 * ORBIS_PROTECT_FREED=1. Off by default: it converts a silent corruption into a dead title, which is the
 * point, but it is a behaviour change and the run that wants it should say so. */
static bool
orbis_protect_freed_enabled(void)
{
   static int on = -1;
   if (on < 0) {
      const char *const e = getenv("ORBIS_PROTECT_FREED");
      on = (e != NULL && *e != '\0' && *e != '0');
      if (on)
         mesa_logi("orbis-drm: freed ranges will have their rights REVOKED (ORBIS_PROTECT_FREED). A stale GPU "
                   "access now faults where it is made rather than reading the next tenant - measured to work "
                   "on this console, see the MPROTECT ladder.");
   }
   return on != 0;
}

static void
orbis_protect_range_locked(uint64_t addr, uint64_t size, bool give_back)
{
   if (!orbis_protect_freed_enabled() || size == 0)
      return;

   /* The arm's own slice is never RADV's to free, and revoking rights on the fence label or the staging ring
    * would break the driver rather than catch a bug in it. */
   if (addr < orbis_va_base)
      return;

   const uint64_t page = orbis_page_size();
   const uint64_t lo = give_back ? (addr & ~(page - 1)) : ((addr + page - 1) & ~(page - 1));
   const uint64_t hi = give_back ? ((addr + size + page - 1) & ~(page - 1)) : ((addr + size) & ~(page - 1));

   if (hi <= lo) {
      static unsigned too_small;
      if (!give_back && orbis_budget(&too_small, 4))
         mesa_logi("orbis-drm: a freed range of %" PRIu64 " bytes at 0x%" PRIx64 " covers no whole %" PRIu64
                   " KiB page, so its rights cannot be taken away - allocations this small share a page with a "
                   "neighbour and are outside this scheme",
                   size, addr, page / 1024);
      return;
   }

#if !defined(__PS4__)
   /* ⚠ THE HOST ARM CANNOT DO THIS, AND SAYS SO RATHER THAN LOOKING LIKE IT DID. There is no shared page
    * table with a GPU on a laptop and no sceKernelMprotect to call; a silent no-op here would let a host run
    * print every other line of this scheme and imply it was in force. */
   {
      static unsigned said;
      if (orbis_budget(&said, 1))
         mesa_logi("orbis-drm: ORBIS_PROTECT_FREED has no effect on the host arm - the revoke is a console "
                   "call. Freed ranges stay readable here.");
   }
   (void)lo;
   (void)hi;
#else
   const int32_t err =
      sceKernelMprotect((const void *)(uintptr_t)lo, (size_t)(hi - lo),
                        give_back ? ORBIS_GRAPHICS_PROT : ORBIS_PROT_NONE);
   if (err != 0) {
      static unsigned failed;
      if (orbis_budget(&failed, 4))
         mesa_loge("orbis-drm: sceKernelMprotect(0x%" PRIx64 "..0x%" PRIx64 ", %s) -> 0x%08x. The scheme is "
                   "not doing what the rest of this log implies it is.",
                   lo, hi, give_back ? "restore" : "revoke", (unsigned)err);
   }
#endif
}

/* The three wrappers. One lock for all of them, because the question is whether this console's
   direct-memory machinery tolerates concurrency at all, not which of the three calls is the one. */
static int
orbis_backing_map(struct orbis_bo *bo, uint64_t bo_offset, uint64_t size, uint64_t addr, bool gpu_read_only)
{
   if (!orbis_serialise_kernel_mem())
      return orbis_backing_map_locked(bo, bo_offset, size, addr, gpu_read_only);

   simple_mtx_lock(&orbis_kernel_mem_lock);
   orbis_kmem_step_set("map", addr, size);
   const int r = orbis_backing_map_locked(bo, bo_offset, size, addr, gpu_read_only);
   orbis_kmem_step_set("(idle) after map", addr, size);
   simple_mtx_unlock(&orbis_kernel_mem_lock);
   return r;
}

static int
orbis_backing_unmap(uint64_t addr, uint64_t size, struct orbis_bo *bo)
{
   if (!orbis_serialise_kernel_mem())
      return orbis_backing_unmap_locked(addr, size, bo);

   simple_mtx_lock(&orbis_kernel_mem_lock);
   orbis_kmem_step_set("unmap", addr, size);
   const int r = orbis_backing_unmap_locked(addr, size, bo);
   orbis_kmem_step_set("(idle) after unmap", addr, size);
   simple_mtx_unlock(&orbis_kernel_mem_lock);
   return r;
}

static void
orbis_protect_range(uint64_t addr, uint64_t size, bool give_back)
{
   if (!orbis_serialise_kernel_mem()) {
      orbis_protect_range_locked(addr, size, give_back);
      return;
   }

   simple_mtx_lock(&orbis_kernel_mem_lock);
   orbis_kmem_step_set(give_back ? "protect (give back)" : "protect (revoke)", addr, size);
   orbis_protect_range_locked(addr, size, give_back);
   orbis_kmem_step_set("(idle) after protect", addr, size);
   simple_mtx_unlock(&orbis_kernel_mem_lock);
}

static int
orbis_bo_va_op(uint32_t bo_handle, uint64_t offset, uint64_t size, uint64_t addr, uint64_t flags, uint32_t ops)
{
   const bool gpu_read_only = !(flags & AMDGPU_VM_PAGE_WRITEABLE);

   switch (ops) {
   case AMDGPU_VA_OP_MAP: {
      struct orbis_bo *obo = orbis_bo_from_handle(bo_handle);
#if defined(__PS4__)
      orbis_kc_hit(ORBIS_KC_VA_MAP);
#endif

      /* Give the rights back before anything can touch the range. Wider than the revoke on purpose: a page
       * this buffer shares with a neighbour may have been revoked when that neighbour died. */
      orbis_protect_range(addr, size, true);

      if (!obo) {
         mesa_loge("orbis-drm: VA map of unknown BO handle %u", bo_handle);
         return -EINVAL;
      }
      /* ROUNDED THE WAY THE CALLER ROUNDED. radv_amdgpu_bo.c aligns size up to getpagesize() before calling,
       * so a BO of 4 KiB legitimately asks to map one page - which is 16 KiB here. Comparing against the raw
       * size refused that, silently, and cost a console flash: vkCreateDevice returned
       * OUT_OF_DEVICE_MEMORY with nothing in the log between the VA allocation and the failure. */
      const uint64_t page = orbis_page_size();
      const uint64_t owned = (obo->size + page - 1) & ~(page - 1);
      if (offset + size > owned) {
         mesa_loge("orbis-drm: VA map past the BO: offset %" PRIu64 " + %" PRIu64 " > %" PRIu64
                   " (size %" PRIu64 ", page %" PRIu64 ")",
                   offset, size, owned, obo->size, page);
         return -EINVAL;
      }

      int r = orbis_backing_map(obo, offset, size, addr, gpu_read_only);
      if (r)
         return r;

      /* The overlap check, and it is deliberately not behind a knob: a knob would have been off during every run
       * that needed it. It costs one walk of a table that a Gothic boot keeps at ~150 entries. */
      {
         simple_mtx_lock(&orbis_map_lock);
         ORBIS_MAP_STEP("map: scanning the live table for overlaps");
         unsigned slot = ORBIS_LIVE_MAPS;
         for (unsigned i = 0; i < ORBIS_LIVE_MAPS; ++i) {
            if (orbis_live_map[i].addr == 0) {
               if (slot == ORBIS_LIVE_MAPS)
                  slot = i;
               continue;
            }
            if (addr < orbis_live_map[i].end && orbis_live_map[i].addr < addr + size) {
               /* ⚠ THE SAME BO AT THE SAME RANGE IS NOT TWO BUFFERS SHARING MEMORY, and the first version of this
                * check reported eight of them as if it were. It is either RADV mapping a BO twice, or - far more
                * likely - this table never being told about the UNMAP, because removal below demanded an EXACT
                * (addr, end) match and an unmap of a different length leaves the entry behind forever.
                *
                * Kept as its own message rather than dropped: a stale entry means the OVERLAP CHECK IS BLIND from
                * then on, which is worse than a false positive and reads the same as a clean run. */
               if (orbis_live_map[i].handle == bo_handle && orbis_live_map[i].addr == addr &&
                   orbis_live_map[i].end == addr + size) {
                  static unsigned resaid;
                  if (orbis_budget(&resaid, 4)) {
                     mesa_logw("orbis-drm: BO %u is mapped AGAIN at the same 0x%" PRIx64 "..0x%" PRIx64
                               " with no unmap in between - bookkeeping, not a collision",
                               bo_handle, addr, addr + size);
                  }
                  continue;
               }
               static unsigned said;
               if (orbis_budget(&said, 8)) {
                  mesa_loge("orbis-drm: TWO LIVE MAPPINGS OVERLAP - BO %u wants 0x%" PRIx64 "..0x%" PRIx64
                            " (offset %" PRIu64 ") and BO %u already holds 0x%" PRIx64 "..0x%" PRIx64
                            ". Whichever writes second destroys the other's contents.",
                            bo_handle, addr, addr + size, offset, orbis_live_map[i].handle,
                            orbis_live_map[i].addr, orbis_live_map[i].end);
               }
            }
         }
         /* ⚠ AND DID THIS MAPPING MOVE INTO A DEAD RANGE? That is the shape run AF measured: a pool mapped inside
          * the range a 4 MiB buffer used to hold, with the buffer already unmapped so the live-overlap check saw
          * nothing. Naming the predecessor turns "some CPU writer" into "whatever owned that BO". */
         for (unsigned d = 0; d < ORBIS_DEAD_MAPS; ++d) {
            if (orbis_dead_map[d].addr == 0)
               continue;
            if (addr < orbis_dead_map[d].end && orbis_dead_map[d].addr < addr + size) {
               /* ⚠ A BUFFER RE-ENTERING ITS OWN OLD RANGE IS NOT A CHANGE OF TENANT, and the live-overlap
                * check twenty lines above already learned this - it grew the same carve-out after reporting
                * eight bookkeeping events as collisions. This loop did not, so a Gothic boot spent the whole
                * budget of sixteen on BO 4 re-mapping inside BO 4's own freed range, and a real predecessor
                * arriving afterwards would have been reported nowhere. A blind check reads exactly like a
                * clean one.
                *
                * Its own budget rather than silence, because the two are not certainly the same buffer: a
                * handle is recycled after ac_drm_bo_free, so an identical number can be a different BO. That
                * is a real if unlikely case and it should not vanish - only stop drowning the other one. */
               if (orbis_dead_map[d].handle == bo_handle) {
                  static unsigned self_reused;
                  if (orbis_budget(&self_reused, 4)) {
                     mesa_logi("orbis-drm: BO %u maps 0x%" PRIx64 "..0x%" PRIx64
                               " back inside the range it held itself (0x%" PRIx64 "..0x%" PRIx64
                               ") - bookkeeping, unless this handle was recycled onto a different buffer.",
                               bo_handle, addr, addr + size, orbis_dead_map[d].addr, orbis_dead_map[d].end);
                  }
                  break;
               }
               static unsigned reused;
               if (orbis_budget(&reused, 16)) {
                  mesa_logi("orbis-drm: BO %u maps 0x%" PRIx64 "..0x%" PRIx64
                            " INTO A RANGE BO %u HELD until it was unmapped (0x%" PRIx64 "..0x%" PRIx64
                            "). Anything still writing through the old pointer lands in the new tenant.",
                            bo_handle, addr, addr + size, orbis_dead_map[d].handle, orbis_dead_map[d].addr,
                            orbis_dead_map[d].end);
               }
               break;
            }
         }

         /* ⚠ THE SEARCH ABOVE WALKS THE WHOLE TABLE FOR OVERLAPS, so it also finds a free slot on
          * the way; only when it finds none does the table need to grow, and growing must happen
          * after the walk rather than during it. */
         ORBIS_MAP_STEP("map: growing the live table");
         if (slot == ORBIS_LIVE_MAPS)
            slot = orbis_live_map_slot();

         if (slot < ORBIS_LIVE_MAPS) {
            orbis_live_map[slot].addr = addr;
            orbis_live_map[slot].end = addr + size;
            orbis_live_map[slot].handle = bo_handle;
         } else {
            static unsigned said;
            if (orbis_budget(&said, 1)) {
               mesa_logw("orbis-drm: the live-mapping table is full at %u entries - overlaps are no longer being "
                         "checked, which is a silence rather than a clean bill of health",
                         (unsigned)ORBIS_LIVE_MAPS);
            }
         }
         ORBIS_MAP_STEP("(idle)");
         simple_mtx_unlock(&orbis_map_lock);
      }

      /* ⚠ AMDGPU_GEM_CREATE_VRAM_CLEARED IS HONOURED HERE, AND UNTIL NOW IT WAS SILENTLY IGNORED.
       *
       * ac_drm_bo_alloc records the flag into obo->alloc_flags and nothing ever read it, so RADV_DEBUG=zerovram -
       * which exists exactly to answer "was the input garbage" - ran a whole console trip and changed NOTHING while
       * appearing to work: the title logged the knob, RADV set the flag, and this arm dropped it. A knob that can
       * silently not fire must say that it did not, and this one could not even say that.
       *
       * THE MAPPING IS WHERE IT HAS TO HAPPEN. ac_drm_bo_alloc only reserves physical memory; there is no address
       * to write to until the VA op. Here the range is inside the arena and CPU-visible by construction - one
       * ONION mapping for both processors - so a memset is the whole implementation.
       *
       * OFFSET 0 ONLY: a second mapping of the same BO is RADEON_FLAG_VM_PAD_1PAGE's guard page, and zeroing
       * through it would clear the neighbour it aliases rather than anything of ours.
       *
       * RADV also relies on this for correctness, not only for debugging: it passes RADEON_FLAG_ZERO_VRAM for
       * buffers it expects to read back as zero (radv_amdgpu_bo.c:441,634). Dropping the flag is a wrong VALUE
       * bug that only shows up as something else. */
      /* ⚠ ZERO EVERYTHING, BECAUSE THIS ARENA IS THE ONE THING NO DESKTOP DRIVER HAS.
       *
       * `ORBIS_ZERO_ALL=1` memsets every BO at its first mapping. On Linux a fresh GEM object is backed by pages
       * the kernel has zeroed; here every BO is a slice of ONE arena that is handed out, freed and handed out
       * again, so a "new" buffer holds whatever the last one left in it. Any place the Vulkan spec permits an
       * application to leave memory unwritten therefore behaves differently on this port than on the hardware the
       * application was tested against - and it behaves differently in the direction of a GPU fault.
       *
       * WHICH IS EXACTLY WHERE THE HUNT ARRIVED. `-bl 0` - OpenGothic's bindless switch - made the fault vanish
       * completely: the world renders continuously with no protection fault in the klog at all. Bindless here
       * means descriptor arrays declared with VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT (Tempest's
       * vsetlayoutcache.cpp:52), and PARTIALLY_BOUND is precisely the permission to leave a descriptor slot
       * unwritten. A shader that reads an unwritten slot gets whatever bytes are there: on a desktop driver zeros,
       * which decode as a null descriptor and return nothing; here arena garbage, which decodes as a texture
       * descriptor with a nonsense base. That accounts for every property of this defect - the scattered addresses
       * (0xff0000000, 0x26d872000, 0x31578c000, 0x3bcdf87000, spread over 240 GiB), the address appearing in no
       * packet, no descriptor RADV BUILDS ever failing its bounds check, and the value not sitting anywhere in
       * memory as an address.
       *
       * A KNOB AND NOT A DEFAULT, for now: this is a measurement first. It costs a memset of every allocation, and
       * if it removes the fault while bindless is ON, then this arena needs zeroing as a matter of correctness
       * rather than debugging, and the cost belongs in a decision rather than in an environment variable. */
      /* ⚠ `!obo->cpu` AND NOT `offset == 0`, AND GETTING THAT WRONG PRODUCED A BAD-COMMAND FAULT.
       *
       * RADEON_FLAG_VM_PAD_1PAGE maps offset 0 of the SAME BO a second time, at the address just after the
       * buffer. Both mappings therefore have offset 0, so `offset == 0` is true for the pad page too - and
       * zeroing through it writes `size` zero bytes over WHATEVER LIVES AFTER THE BUFFER, which is the next
       * allocation. With the knob on, OpenGothic died with exception 0xa0d0c006 GPU_FAULT_BAD_COMMAND and a
       * kernel PM4 tail of an IT_INDIRECT_BUFFER followed by 0x465c3b30, a dword that is not a packet header:
       * the CP had entered an IB whose middle had been zeroed out from underneath it.
       *
       * The condition that means "the first mapping" is `!obo->cpu`, which is what the line further down uses
       * to decide where the BO lives. The warning is a few paragraphs below, written before this knob existed
       * and then not applied to it. */
      if (getenv("ORBIS_ZERO_ALL") != NULL && offset == 0 && obo->cpu == NULL) {
         static uint64_t zeroed_bytes;
         static unsigned zeroed_bos;
         memset((void *)(uintptr_t)addr, 0, (size_t)size);
         zeroed_bytes += size;
         ++zeroed_bos;
         if (zeroed_bos <= 2 || (zeroed_bos % 256) == 0)
            mesa_logi("orbis-drm: ORBIS_ZERO_ALL - zeroed %u BOs, %" PRIu64 " MiB so far (this arena recycles its "
                      "memory, so nothing else zeroes it)",
                      zeroed_bos, zeroed_bytes / (1024 * 1024));
      } else if ((obo->alloc_flags & ORBIS_GEM_CREATE_VRAM_CLEARED) && offset == 0 && obo->cpu == NULL) {
         memset((void *)(uintptr_t)addr, 0, (size_t)size);
         static unsigned said;
         if (orbis_budget(&said, 4)) {
            mesa_logi("orbis-drm: zeroing %" PRIu64 " KiB at 0x%" PRIx64
                      " - the BO asked for VRAM_CLEARED (RADV_DEBUG=zerovram or a buffer RADV reads back as zero)",
                      size / 1024, addr);
         }
      }

      /* The BO's own address is the mapping of its offset 0. RADEON_FLAG_VM_PAD_1PAGE maps page 0 a second
       * time after the buffer, so only the first mapping defines where the BO lives. */
      if (offset == 0 && !obo->cpu)
         obo->cpu = (void *)(uintptr_t)addr;

      /* ⚠ WHICH BO OWNS AN ADDRESS. `ORBIS_TRACE_BOS=1` prints every mapping's range once, and the reason is a
       * fault report that names an address and nothing else: `Protection fault addr(VA): 0xff00xxxxx` is only
       * actionable next to a list saying which buffer ends where. With the list, an address either lands in a BO -
       * and then the log says which, and how far past its end - or it lands in no BO at all, which is a different
       * bug from a wrong offset.
       *
       * Not folded into ORBIS_DRM_TRACE: RADV maps thousands of BOs in a Gothic boot and this is one line each. */
      if (getenv("ORBIS_TRACE_BOS") != NULL)
         mesa_logi("orbis-drm: BO %u maps 0x%" PRIx64 "..0x%" PRIx64 " (offset %" PRIu64 ", %" PRIu64
                   " KiB, heap 0x%x, flags 0x%" PRIx64 ")",
                   bo_handle, addr, addr + size, offset, size / 1024, obo->preferred_heap, obo->alloc_flags);
      return 0;
   }
   case AMDGPU_VA_OP_UNMAP: {
#if defined(__PS4__)
      orbis_kc_hit(ORBIS_KC_VA_UNMAP);
#endif
      /* Forget it here, so a later mapping of the same range is not reported as an overlap. An UNMAP this arm
       * treats as a no-op for the BACKING is still a real event for the bookkeeping. */
      simple_mtx_lock(&orbis_map_lock);
      ORBIS_MAP_STEP("unmap: recording the dead range");
      /* ⚠ REMEMBER THE DEAD, BECAUSE THE LIVE-OVERLAP CHECK IS BLIND TO THIS BY DESIGN.
       *
       * What run AF showed, all in one log: the corrupt set is a pool at 0x225d14000 (295008 bytes, BO 105, 304 KiB,
       * heap 0x6) and that address ALSO lies inside BO 42 - 4 MiB, heap 0x2, 0x225a00000..0x225e00000. The live-
       * overlap check never fired, which means BO 42 was already unmapped when the pool was mapped over its range.
       * So a buffer full of bounding boxes died, a descriptor pool moved into its address, and writes kept arriving.
       *
       * On Linux that write faults: freeing the buffer unmaps the pages. Here the arena never unmaps, so it lands in
       * the pool and the GPU builds descriptors out of world coordinates.
       *
       * This records what died where, so the next MAP into a dead range can name its predecessor - which turns "some
       * CPU writer" into "whatever owned BO 42". */
      /* ⚠ NO LOCK HERE, AND TAKING ONE FROZE THE CONSOLE. orbis_map_lock is already held by the line above, and
       * simple_mtx is not recursive - so the first unmap deadlocked the thread that does the rendering, which the
       * console showed as a black screen at 0 fps. The lock is taken once, at the top of this case, and everything
       * that touches either table belongs under it without asking again. */
      {
         if (getenv("ORBIS_TRACE_BOS") != NULL)
            mesa_logi("orbis-drm: BO %u UNMAPS 0x%" PRIx64 "..0x%" PRIx64, bo_handle, addr, addr + size);
         orbis_dead_map[orbis_dead_at % ORBIS_DEAD_MAPS].addr = addr;
         orbis_dead_map[orbis_dead_at % ORBIS_DEAD_MAPS].end = addr + size;
         orbis_dead_map[orbis_dead_at % ORBIS_DEAD_MAPS].handle = bo_handle;
         ++orbis_dead_at;
      }

      /* Anything the unmapped range COVERS OR TOUCHES, not only an exact match: RADV does not have to unmap with
       * the same length it mapped with, and an exact-match rule leaves entries behind forever - which silently
       * turns the overlap check off. */
      ORBIS_MAP_STEP("unmap: clearing live entries the range covers");
      for (unsigned i = 0; i < ORBIS_LIVE_MAPS; ++i)
         if (orbis_live_map[i].addr != 0 && orbis_live_map[i].addr < addr + size &&
             addr < orbis_live_map[i].end)
            orbis_live_map[i].addr = 0;
      ORBIS_MAP_STEP("(idle)");
      simple_mtx_unlock(&orbis_map_lock);

      /* After the tables are updated and the lock is dropped: a kernel call under orbis_map_lock is a stall
       * every other thread pays for, and nothing here needs the tables to stay still. */
      orbis_protect_range(addr, size, false);

      /* ⚠ THE GARLIC CHUNK IS PARKED HERE, NOT RESTORED HERE, and it used to be restored right at
       * the top of this case. That was a synchronous sceKernelMapDirectMemory(MAP_FIXED) plus a
       * sceKernelReleaseDirectMemory on memory the GPU could still be reading: vkFreeMemory during
       * the frame that used the texture is legal Vulkan, and this file quotes the contract where it
       * explains why bo_wait_for_idle waits for the last submission rather than for one BO. Only
       * the mprotect revoke was deferred, which protected the addresses and not the pages behind
       * them. orbis_backing_unmap now hands the chunk to the same retire entry as the revoke, so
       * both happen once the fence label has passed. */
      return orbis_backing_unmap(addr, size, orbis_bo_from_handle(bo_handle));
   }
   case AMDGPU_VA_OP_CLEAR:
      /* Unmapping a PRT range back to "nothing there". Sparse residency is not wired up yet, and a silent
       * success here would let a sparse buffer look bound when it is not. */
      ORBIS_DRM_TODO();
      return -ENOSYS;
   case AMDGPU_VA_OP_REPLACE:
      ORBIS_DRM_TODO();
      return -ENOSYS;
   default:
      return -EINVAL;
   }
}

int
ac_drm_bo_wait_for_idle(ac_drm_device *dev, ac_drm_bo bo, uint64_t timeout_ns, bool *busy)
{
   /* COARSE ON PURPOSE, AND CONSERVATIVE IN THE SAFE DIRECTION. Per-BO residency tracking would mean recording
    * which submissions touched which buffer; this waits for the LAST submission instead, which is a superset
    * of what any single BO can be waiting for. So it never reports idle too early - it over-waits, which costs
    * time and cannot cause corruption. The opposite trade would be a use-after-free the GPU commits. */
   if (!bo.abo)
      return -EINVAL;

   static unsigned said;
   if (orbis_budget(&said, 1)) {
      mesa_logi("orbis-drm: bo_wait_for_idle waits for the last submission, not for this BO - coarse and "
                "conservative");
   }

   const uint64_t seq = orbis_submit_seq_no;
   if (seq == 0) {
      /* Nothing was ever submitted, so nothing can be using it. */
      *busy = false;
      return 0;
   }

   uint32_t expired = 0;
   const int r = ac_drm_cs_query_fence_status(dev, 0, AMDGPU_HW_IP_GFX, 0, 0, seq, timeout_ns, 0, &expired);
   if (r)
      return r;
   *busy = !expired;
   return 0;
}

int
ac_drm_create_bo_from_user_mem(ac_drm_device *dev, void *cpu, uint64_t size, ac_drm_bo *bo)
{
   /* ⚠ THE GPU CAN ONLY SEE THE ARENA, and that is the whole content of this function. amdgpu's userptr pins
    * arbitrary process pages and maps them into the GPU's own address space; there is no second address space
    * here and no way to make pages the GPU was never given visible to it. So a pointer inside the arena is
    * wrappable and anything else is not.
    *
    * Refusing loudly matters more than usual: radeon_info reports has_userptr = 1, derived by ac_gpu_info from
    * the DRM version rather than probed, so RADV believes this works. A silent success would hand the GPU an
    * address it cannot read, and the symptom would be a shader reading zeroes. */
   const uint64_t va = (uint64_t)(uintptr_t)cpu;

   if (va < orbis_va_base || va + size > orbis_va_end) {
      mesa_logw("orbis-drm: user memory at 0x%" PRIx64 " (%" PRIu64 " bytes) is outside the arena - the GPU "
                "cannot address it",
                va, size);
      return -EINVAL;
   }

   struct orbis_bo *obo = calloc(1, sizeof(*obo));
   if (!obo)
      return -ENOMEM;

   obo->size = size;
   obo->alignment = orbis_page_size();
   obo->preferred_heap = AMDGPU_GEM_DOMAIN_GTT;
   obo->cpu = cpu;
   /* Somebody else's pages: neither arm may release or unmap them. */
   obo->foreign = true;

   simple_mtx_lock(&orbis_bo_lock);
   obo->handle = orbis_bo_slot_claim(obo);
   simple_mtx_unlock(&orbis_bo_lock);
   if (obo->handle != 0) {
      bo->abo = obo;
      return 0;
   }

   free(obo);
   return -ENOMEM;
}

/* ---------------------------------------------------------------- the VA allocator, and it is REAL
 *
 * PURE BOOKKEEPING, WHICH IS WHY IT IS FINISHED WHILE ITS NEIGHBOURS ARE STUBS. Reserving an address range
 * touches no hardware and calls no sceKernel entry, so unlike ac_drm_bo_alloc this needs no host/console
 * seam and no console to test: the laptop exercises the same code the PS4 will run. That split - VA is ours,
 * MEMORY is the platform's - is the useful line through phase 3.
 *
 * The window itself is defined near the top of this file, beside the report that promises it.
 */

/* struct amdgpu_va is libdrm's opaque handle type and ac_linux_drm.h forward-declares it, so the arm gets to
 * say what it contains. It owns its range because ac_drm_va_range_free takes NO device argument - the freeing
 * side has nothing else to reach through, which is worth knowing before writing the allocator rather than
 * after. */

static uint64_t
orbis_align_up(uint64_t v, uint64_t a)
{
   return a > 1 ? (v + a - 1) & ~(a - 1) : v;
}

/* ---------------------------------------------------------------- WHAT THE ARENA ACTUALLY LOOKS LIKE
 *
 * ⚠ "out of device VA" USED TO SAY ONLY THAT IT HAPPENED, AND THAT IS NOT ENOUGH TO ACT ON. Three
 * different faults produce the identical line and they need three different fixes:
 *
 *   a LEAK          - live bytes climb monotonically and never come back down
 *   FRAGMENTATION   - live bytes are flat, but the largest free gap shrinks below the request while
 *                     plenty of free space remains spread across many gaps
 *   CAPACITY        - live bytes are flat, the free space is genuinely nearly zero, and the title
 *                     simply wants more than the window holds
 *
 * The three are told apart by four numbers - live bytes, free bytes, the largest gap, and the number
 * of gaps - so they are printed on the failure AND every five seconds, because a single sample at the
 * moment of death cannot show a trend and the trend is the whole answer.
 *
 * ⚠ THE WINDOW IS NOT 16 GB. ORBIS_VA_BASE_DEFAULT/ORBIS_VA_END_DEFAULT at the top of this file are
 * overwritten during arena setup: the window is the direct-memory arena minus this arm's private
 * slice, which the ladder makes 1024 MiB at best and the log reports as "RADV's window ... (991 MiB)".
 * The report prints the window size with every sample so no reader has to remember which is in force.
 *
 * A parked range - retired by RADV, still held here until the GPU passes the last submission handed
 * out - is counted separately, because "the retire queue is not draining" and "the title is holding
 * memory" are different faults and both show as live bytes.
 *
 * COST: one walk of a list of a few hundred entries, five times a minute plus once per failure. */
struct orbis_va_stats {
   uint64_t ranges;        /* entries in the sorted list */
   uint64_t retired;       /* of those, freed by RADV and parked until the GPU passes them */
   uint64_t retired_bytes;
   uint64_t used;          /* bytes the list covers, each range's own trailing gap included */
   uint64_t largest_range;
   uint64_t gaps;          /* separate runs of free address space */
   uint64_t free_bytes;
   uint64_t largest_gap;
   /* Live ranges by size class, so a leak names the size that is leaking. The classes are powers of
    * four from 64 KiB, which is coarse enough to stay one line and fine enough to separate a
    * descriptor pool from a render target. */
   uint64_t bucket_n[7];
   uint64_t bucket_bytes[7];
};

static uint64_t orbis_va_allocs, orbis_va_frees;

static void
orbis_va_snapshot(struct orbis_va_stats *s)
{
   memset(s, 0, sizeof(*s));

   simple_mtx_lock(&orbis_va_lock);
   uint64_t cursor = orbis_va_base;
   for (const struct amdgpu_va *va = orbis_va_list; va; va = va->next) {
      s->ranges++;
      s->used += va->size;
      if (va->size > s->largest_range)
         s->largest_range = va->size;
      if (va->retired) {
         s->retired++;
         s->retired_bytes += va->size;
      }
      if (va->base > cursor) {
         const uint64_t gap = va->base - cursor;
         s->gaps++;
         s->free_bytes += gap;
         if (gap > s->largest_gap)
            s->largest_gap = gap;
      }
      if (va->base + va->size > cursor)
         cursor = va->base + va->size;

      unsigned b = 0;
      for (uint64_t edge = 64ull * 1024; b < 6 && va->size > edge; edge *= 4)
         b++;
      s->bucket_n[b]++;
      s->bucket_bytes[b] += va->size;
   }
   simple_mtx_unlock(&orbis_va_lock);

   /* The tail of the window is a gap like any other, and it is the one first-fit reaches last. */
   if (orbis_va_end > cursor) {
      const uint64_t gap = orbis_va_end - cursor;
      s->gaps++;
      s->free_bytes += gap;
      if (gap > s->largest_gap)
         s->largest_gap = gap;
   }
}

static void
orbis_va_report(const char *why)
{
   struct orbis_va_stats s;
   orbis_va_snapshot(&s);

   const uint64_t window = orbis_va_end > orbis_va_base ? orbis_va_end - orbis_va_base : 0;

   mesa_logi("orbis-drm: ARENA %s: %" PRIu64 " live range(s) holding %" PRIu64 " KiB of a %" PRIu64
             " KiB window; free %" PRIu64 " KiB in %" PRIu64 " gap(s), LARGEST GAP %" PRIu64
             " KiB; largest live range %" PRIu64 " KiB; %" PRIu64 " range(s) (%" PRIu64
             " KiB) are parked for the GPU and %u entr(ies) sit on the retire queue; %" PRIu64
             " alloc(s) and %" PRIu64 " free(s) since start, so %" PRIu64 " are outstanding",
             why, s.ranges, s.used / 1024, window / 1024, s.free_bytes / 1024, s.gaps,
             s.largest_gap / 1024, s.largest_range / 1024, s.retired, s.retired_bytes / 1024,
             orbis_retire_count, orbis_va_allocs, orbis_va_frees,
             orbis_va_allocs - orbis_va_frees);

   mesa_logi("orbis-drm: ARENA %s by size: <=64K %" PRIu64 " (%" PRIu64 " KiB), <=256K %" PRIu64
             " (%" PRIu64 " KiB), <=1M %" PRIu64 " (%" PRIu64 " KiB), <=4M %" PRIu64 " (%" PRIu64
             " KiB), <=16M %" PRIu64 " (%" PRIu64 " KiB), <=64M %" PRIu64 " (%" PRIu64
             " KiB), >64M %" PRIu64 " (%" PRIu64 " KiB)",
             why, s.bucket_n[0], s.bucket_bytes[0] / 1024, s.bucket_n[1], s.bucket_bytes[1] / 1024,
             s.bucket_n[2], s.bucket_bytes[2] / 1024, s.bucket_n[3], s.bucket_bytes[3] / 1024,
             s.bucket_n[4], s.bucket_bytes[4] / 1024, s.bucket_n[5], s.bucket_bytes[5] / 1024,
             s.bucket_n[6], s.bucket_bytes[6] / 1024);

   /* ⚠ AND WHO. A size class is not a culprit; a REPEATED EXACT SIZE is, because RADV asks for a VA
    * range of exactly align64(bo_size, page) + pad, so one allocation site produces one number. The
    * failing request in the swanstation run was 2113536 bytes - a 2 MiB buffer plus RADV's pad page -
    * and if four hundred ranges of exactly 2113536 bytes are live, the leak is named without any
    * further instrument. Sixty-four distinct sizes are tracked and the six largest totals printed;
    * anything past that is lumped, which is honest rather than silent. */
   {
      struct { uint64_t size, n; } tally[64];
      unsigned distinct = 0;
      uint64_t spilled_n = 0;

      simple_mtx_lock(&orbis_va_lock);
      for (const struct amdgpu_va *va = orbis_va_list; va; va = va->next) {
         unsigned i = 0;
         while (i < distinct && tally[i].size != va->size)
            i++;
         if (i < distinct) {
            tally[i].n++;
         } else if (distinct < ARRAY_SIZE(tally)) {
            tally[distinct].size = va->size;
            tally[distinct].n = 1;
            distinct++;
         } else {
            spilled_n++;
         }
      }
      simple_mtx_unlock(&orbis_va_lock);

      char line[512];
      int n = 0;
      for (unsigned shown = 0; shown < 6 && n < (int)sizeof(line) - 1; shown++) {
         unsigned best = distinct;
         uint64_t best_bytes = 0;
         for (unsigned i = 0; i < distinct; i++) {
            if (tally[i].n == 0)
               continue;
            if (tally[i].size * tally[i].n > best_bytes) {
               best_bytes = tally[i].size * tally[i].n;
               best = i;
            }
         }
         if (best == distinct)
            break;
         n += snprintf(line + n, sizeof(line) - n, "%s%" PRIu64 " B x%" PRIu64 " = %" PRIu64 " KiB",
                       n ? ", " : "", tally[best].size, tally[best].n, best_bytes / 1024);
         tally[best].n = 0;
      }

      mesa_logi("orbis-drm: ARENA %s biggest holders by exact size: %s%s", why, n ? line : "(none)",
                spilled_n ? " (and more than 64 distinct sizes, so the tail is not shown)" : "");
   }
}

int
ac_drm_va_range_alloc(ac_drm_device *dev, enum amdgpu_gpu_va_range va_range_type, uint64_t size,
                      uint64_t va_base_alignment, uint64_t va_base_required,
                      uint64_t *va_base_allocated, amdgpu_va_handle *va_range_handle, uint64_t flags)
{
   if (!size)
      return -EINVAL;

   /* AMDGPU_VA_RANGE_HIGH arrives on EVERY call and is deliberately ignored: this device reports
    * high_va_offset = high_va_max = 0, so there is no second window to serve it from. Saying so once beats
    * either pretending or failing - libdrm itself falls back to the low range when the high one is empty. */
   if (flags & AMDGPU_VA_RANGE_HIGH) {
      static unsigned said;
      if (orbis_budget(&said, 1)) {
         mesa_logi("orbis-drm: AMDGPU_VA_RANGE_HIGH ignored - this device reports no high VA window");
      }
   }

   /* THE PAGE SIZE IS A FLOOR ON THE ALIGNMENT, not a formality: RADV rounds a mapping up to it, so two VA
    * ranges closer together than one page would be mapped on top of each other. */
   const uint64_t align = MAX2(va_base_alignment, orbis_page_size());
   /* 32-bit requests are a HARD constraint, not a preference: address32_hi is reported as 0. */
   /* A 32-bit request must land in the 4 GiB window amdgpu_sw_info_address32_hi names, which is the one the
    * arena's base lives in - not "below 4 GiB", which was only true while this port chose its own window. */
    const uint64_t hi_window_end = ((orbis_va_base >> 32) + 1) << 32;
   const uint64_t limit = (flags & AMDGPU_VA_RANGE_32_BIT) ? MIN2(hi_window_end, orbis_va_end) : orbis_va_end;

   struct amdgpu_va *va = calloc(1, sizeof(*va));
   if (!va)
      return -ENOMEM;
   va->size = size;

   simple_mtx_lock(&orbis_va_lock);

   /* ⚠ NEVER HAND BACK AN ADDRESS, TO ASK WHETHER RECYCLING THEM IS WHAT MAKES THIS PORT RANDOM.
    *
    * Two runs of the SHIPPED configuration, back to back, same 802 cases, same driver, same env:
    *
    *   run A   binding_model 580 Pass   geometry 179 Pass / 0 Fail    multiview 3 Pass / 21 Fail
    *   run B   binding_model 579 FAIL   geometry 148 Pass / 31 Fail   multiview 3 Pass / 21 Fail
    *
    * 613 tests went Pass -> Fail with nothing changed. The driver's own log cannot tell the runs apart: the
    * same seven ring sizes to the byte, the same sixteen aliasing warnings, no VA errors in either.
    *
    * ⚠ AND MULTIVIEW IS THE PROOF THAT COUNTING HIDES IT. Both runs report 3 Pass / 21 Fail - and the THREE
    * ARE DIFFERENT TESTS EACH TIME. Six distinct cases passed across two runs, three per run. The count looks
    * like a stable measurement; the identity is a coin toss. Every number this port has reasoned from - "63
    * failures", "78/78", "180/181", the eight ring configurations - is a count.
    *
    * The suspect has been on the open list all along: this allocator is first-fit over a sorted list, and a
    * retired range is removed from that list, so its address is available again immediately. A buffer still
    * being written through a stale pointer then lands in whatever moved in.
    *
    * ORBIS_VA_NO_REUSE=1 keeps a high-water mark and allocates only above it, so no address is ever handed
    * out twice in a process. If the flakiness goes away, address recycling IS the mechanism and the fix is a
    * real quarantine rather than another multiplier somewhere else.
    *
    * ⚠ IT WILL EXHAUST THE ARENA ON A LONG RUN, and that is expected rather than a failure of the experiment:
    * a 1 GiB window cannot serve a CTS sweep without reuse. "out of device VA" below says so plainly, and a
    * run that gets far enough to answer the question before it is still an answer. */
   static int no_reuse = -1;
   static uint64_t va_high_water;
   if (no_reuse < 0) {
      const char *const e = getenv("ORBIS_VA_NO_REUSE");
      no_reuse = (e != NULL && *e != '\0' && *e != '0') ? 1 : 0;
      if (no_reuse)
         mesa_logi("orbis-drm: ORBIS_VA_NO_REUSE - no virtual address will be handed out twice. This asks "
                   "whether recycling them is what makes this port's results random; expect the arena to run "
                   "out on a long run, which is the cost of the answer rather than a fault.");
   }

   struct amdgpu_va **link = &orbis_va_list;
   uint64_t candidate = orbis_align_up(MAX2(va_base_required, orbis_va_base), align);

   /* The high-water mark never applies to a replay address, which must be satisfied exactly or refused. */
   if (no_reuse && !va_base_required)
      candidate = orbis_align_up(MAX2(candidate, va_high_water), align);

   if (va_base_required) {
      /* A replay address (VK_KHR_buffer_device_address's capture/replay). It is satisfied EXACTLY or not at
       * all - moving it would make a captured trace address different memory than it captured. */
      if (va_base_required < orbis_va_base || va_base_required + size > limit ||
          (va_base_required & (align - 1))) {
         simple_mtx_unlock(&orbis_va_lock);
         free(va);
         return -EINVAL;
      }
      candidate = va_base_required;
   }

   for (;;) {
      while (*link && (*link)->base + (*link)->size <= candidate)
         link = &(*link)->next;

      const bool clash = *link && candidate + size > (*link)->base;
      if (!clash)
         break;

      if (va_base_required) {
         /* The exact address is taken. */
         simple_mtx_unlock(&orbis_va_lock);
         free(va);
         return -EINVAL;
      }
      candidate = orbis_align_up((*link)->base + (*link)->size, align);
      link = &(*link)->next;
   }

   if (candidate + size > limit) {
      simple_mtx_unlock(&orbis_va_lock);
      free(va);
      mesa_loge("orbis-drm: out of %s VA for %" PRIu64 " bytes%s",
                (flags & AMDGPU_VA_RANGE_32_BIT) ? "32-bit" : "device", size,
                no_reuse ? " - with ORBIS_VA_NO_REUSE set, so this is the window being consumed rather than "
                           "fragmented, and everything measured before this line still stands"
                         : "");
      /* ⚠ AND WHY. The error alone repeated 183 times in the swanstation run and said nothing that the
       * first line had not: the state of the arena is what separates a leak from fragmentation from a
       * window that is simply too small, and it has to be printed HERE because after the failure the
       * title tears down and the evidence goes with it. Budgeted, because the title retries this
       * allocation every frame and eight samples are as good as two hundred. */
      {
         static unsigned said;
         if (orbis_budget(&said, 8))
            orbis_va_report("AT THE FAILURE");
      }
      return -ENOMEM;
   }

   /* Raise the mark before the range is committed, so a failure above cannot leave it half-advanced. */
   if (no_reuse)
      va_high_water = candidate + size;

   /* ⚠ A PAGE OF SLACK AFTER EVERY RANGE, BECAUSE RADV'S PAD PAGE CANNOT ALIAS HERE.
    *
    * RADEON_FLAG_VM_PAD_1PAGE makes RADV map offset 0 of a buffer A SECOND TIME, at the address just past the
    * buffer. It is an overrun absorber: a shader that reads or writes slightly past the end lands on a real page,
    * and on Linux that page IS page 0 of the same buffer, so the access is harmless to everything else.
    *
    * This arena is ONE flat mapping and cannot alias, so a write through that page does NOT come back to the
    * buffer - it lands on the arena bytes after it.
    *
    * ⚠ AND THAT IS NOT THE DEFECT I FIRST WROTE HERE. I claimed the overrun reaches the NEXT ALLOCATION; it does
    * not. radv_amdgpu_bo_va_size reserves `align64(bo_size, page) + page`, so the pad page's address is inside the
    * BO's OWN VA reservation and no other buffer can be there. The slack below is therefore belt-and-braces rather
    * than a fix, and it is kept only because it is one page against a 1 GiB arena and it makes the invariant
    * explicit instead of inherited from another file's arithmetic.
    *
    * Which is one mechanism for every symptom left in this hunt:
    *   - a descriptor pool with float data in it from element 1280 on: `46391d1c 44046408 45f31ab8 45f3647e`
    *     decodes as 11847.28, 529.56, 7779.34, 7788.56 - world coordinates, written past the end of whatever
    *     allocation sits before that pool
    *   - a float at dword 0 of a command stream, in the submissions right after the GPU-driven frame
    *   - and then scattered unmapped reads, because the GPU builds descriptors out of those floats
    *
    * So: never place two ranges adjacent. One page of slack means an overrun writes into nothing, exactly as the
    * pad page intends, and the cost is a page per allocation - 16 KiB against a 1 GiB arena, times the ~150
    * allocations a Gothic boot makes.
    *
    * THE GAP IS RESERVED, NOT MERELY SKIPPED: it is part of this range's recorded size, so the first-fit walk below
    * cannot hand it to anybody, and freeing this range returns the gap with it. */
   {
      const char *const off = getenv("ORBIS_NO_VA_GAP");
      const uint64_t    gap = off != NULL ? 0 : orbis_page_size();
      if (gap != 0 && candidate + size + gap <= limit) {
         va->size = size + gap;
         static unsigned said;
         if (orbis_budget(&said, 1)) {
            mesa_logi("orbis-drm: reserving one %" PRIu64 " KiB page of slack after every VA range - RADV's pad "
                      "page cannot alias in a flat arena, so without it an overrun writes into the next "
                      "allocation (ORBIS_NO_VA_GAP=1 disables it)",
                      gap / 1024);
         }
      }
   }

   va->base = candidate;
   va->next = *link;
   *link = va;
   orbis_va_allocs++; /* under the VA lock, so it needs no atomic of its own */
   simple_mtx_unlock(&orbis_va_lock);

   *va_base_allocated = va->base;
   *va_range_handle = va;
   return 0;
}

int
ac_drm_va_range_free(amdgpu_va_handle va_range_handle)
{
   if (!va_range_handle)
      return 0; /* RADV's error paths free unconditionally; libdrm tolerates NULL the same way. */

   /* Counted at the DOOR rather than at the release, because the two differ by exactly the ranges this
    * arm is still holding for the GPU - and that difference is the number that says whether the retire
    * queue is draining. */
   p_atomic_inc(&orbis_va_frees);

   /* ⚠ POISON AND QUARANTINE, AND THIS IS WHERE THIS PORT DIFFERS FROM EVERY LINUX ONE.
    *
    * The maintainer's observation is the whole reason this exists: OpenGothic with bindless works on Linux, and the
    * difference cannot be RADV, because it is the same RADV. On Linux, freeing a VA range UNMAPS the pages: a stale
    * descriptor pointing there either faults or reads a hole, so a use-after-free is loud or harmless. Here the
    * arena stays mapped for the life of the process and a freed range is handed to the next allocation immediately -
    * so a stale descriptor silently reads, or WRITES, whatever moved in. If the new tenant is RADV's command buffer,
    * a stale storage-buffer write puts game data at dword 0 of a command stream, which is exactly the corruption
    * this hunt is chasing: a float, at dword 0, in the five submissions after the GPU-driven frame.
    *
    * Every bounds check in this port is blind to that, and necessarily so: a stale address IS inside the arena.
    *
    *   ORBIS_VA_POISON=1        fill a freed range with 0xdeadbeef. A stale read then builds a descriptor whose base
    *                            is 0xdeadbeef00 (T#) or 0xbeefdeadbeef (V#), so the FAULT ADDRESS ITSELF names the
    *                            defect - no further instrument needed.
    *   ORBIS_VA_QUARANTINE=<n>  do not hand a freed range back for n more frees. Without this, poison is erased by
    *                            the next tenant within microseconds; with it, the range stays poisoned long enough
    *                            for a stale access to hit it.
    *
    * BOUNDED IN BOTH DIRECTIONS, and it says when it gives up: the quarantine also has a MiB budget, because this
    * arena is 1 GiB and a Gothic world already uses ~580 MiB of it. Holding too much would turn a diagnostic into
    * an out-of-memory, and a silent early release would make a negative result meaningless. */
   {
      /* ⚠ CAPPED, BECAUSE POISONING EVERYTHING STALLED THE GPU. RADV frees 128 MiB ranges, and a memset that size
       * on every free is enough CPU work to turn the run into `0xa0d0c007
       * GPU_FAULT_SUBMITDONE_TIMEOUT_IN_RUN` - the first attempt did exactly that and measured nothing.
       *
       * The first 256 KiB is enough for what this catches: a stale descriptor points at a buffer's BASE, so poison
       * near the start is what a stale access lands on. `ORBIS_VA_POISON_KIB` moves the line. */
      const char *const poison = getenv("ORBIS_VA_POISON");
      if (poison != NULL && va_range_handle->size != 0 &&
          va_range_handle->base >= orbis_va_base && va_range_handle->base + va_range_handle->size <= orbis_va_end) {
         const char *const kib_s = getenv("ORBIS_VA_POISON_KIB");
         const uint64_t    limit = (kib_s != NULL ? strtoull(kib_s, NULL, 10) : 256) * 1024;
         const uint64_t    bytes = va_range_handle->size < limit ? va_range_handle->size : limit;
         uint32_t *const   p = (uint32_t *)(uintptr_t)va_range_handle->base;
         for (uint64_t i = 0; i < bytes / 4; ++i)
            p[i] = 0xdeadbeefu;
      }
   }

   /* ⚠ THE RANGE MAY NOT BE REUSED WHILE THE GPU CAN STILL BE TOUCHING IT, AND DEFERRING ONLY THE PROTECTION WAS
    * HALF A FIX.
    *
    * What amdgpu does, read off RADV's own destroy path rather than guessed at (radv_amdgpu_bo.c:406-408):
    *
    *     radv_amdgpu_bo_va_op(..., AMDGPU_VA_OP_UNMAP);
    *     ac_drm_bo_free(ws->dev, bo->bo);
    *
    * NO WAIT. No fence check, no wait_for_idle. RADV unmaps and frees the instant the application destroys a buffer,
    * because on Linux the kernel holds a reference from every submission that named the BO and the memory outlives
    * the free. That is the contract, and it is the whole contract.
    *
    * The retire queue added earlier defers taking ACCESS away, which stops the dying GPU work from faulting. It does
    * not stop the ALLOCATOR from handing the same range to the next buffer - and then the new tenant's data is
    * overwritten by the old owner's in-flight writes, silently, exactly as before. So the range itself is parked too:
    * it stays in the allocated list, invisible to first-fit, until the fence label passes the last submission handed
    * out.
    *
    * One queue, one rule, two effects: no reuse and no access until the GPU has moved on. */
   if (orbis_arena_unmapped) {
      simple_mtx_lock(&orbis_va_lock);
      va_range_handle->retire_seq = orbis_submit_seq_no;
      va_range_handle->retired = true;
      simple_mtx_unlock(&orbis_va_lock);
      orbis_retire_ranges();
      return 0;
   }

   {
      const char *const q = getenv("ORBIS_VA_QUARANTINE");
      const unsigned    depth = q != NULL ? (unsigned)strtoul(q, NULL, 10) : 0;
      const char *const mib_s = getenv("ORBIS_VA_QUARANTINE_MIB");
      const uint64_t    byte_cap = (mib_s != NULL ? strtoull(mib_s, NULL, 10) : 32) * 1024 * 1024;

      /* ⚠ THE BYTE BUDGET IS CHECKED ON EVERY FREE, AND CHECKING IT LATE COST A RUN. The first version only looked
       * at the budget once the FIFO was full, so it held 64 ranges whatever their size - and RADV allocates 128 MiB
       * ranges. The title died with `out of device VA for 134234112 bytes` and the run measured nothing.
       *
       * A range bigger than the whole budget is never quarantined at all: holding one would starve the arena on its
       * own, and the descriptors worth catching stale are small buffers rather than 128 MiB heaps. Said once, because
       * "the big ones are not covered" changes what a clean run means. */
      if (depth > 0) {
         static struct amdgpu_va *pending[256];
         static uint64_t          pending_bytes;
         static unsigned          head, count;
         const unsigned           cap = depth > 256 ? 256 : depth;

         if (va_range_handle->size > byte_cap / 2) {
            static unsigned said;
            if (orbis_budget(&said, 1)) {
               mesa_logw("orbis-drm: VA quarantine SKIPS ranges over %" PRIu64 " MiB (this one is %" PRIu64
                         " MiB) - stale accesses to big allocations are not covered by this run",
                         (byte_cap / 2) / (1024 * 1024), va_range_handle->size / (1024 * 1024));
            }
         } else {
            simple_mtx_lock(&orbis_va_lock);
            pending[(head + count) % 256] = va_range_handle;
            pending_bytes += va_range_handle->size;
            ++count;

            /* Release from the head until BOTH caps are satisfied. Releasing means unlinking for real, which is
             * what lets the allocator hand the range out again. */
            while (count > cap || pending_bytes > byte_cap) {
               struct amdgpu_va *const old = pending[head];
               head = (head + 1) % 256;
               --count;
               pending_bytes -= old->size;

               struct amdgpu_va **plink = &orbis_va_list;
               while (*plink && *plink != old)
                  plink = &(*plink)->next;
               if (*plink != NULL)
                  *plink = old->next;
               simple_mtx_unlock(&orbis_va_lock);
               free(old);
               simple_mtx_lock(&orbis_va_lock);
            }
            const uint64_t held = pending_bytes;
            const unsigned n = count;
            simple_mtx_unlock(&orbis_va_lock);

            static uint64_t said;
            if ((said++ % 4096) == 0)
               mesa_logi("orbis-drm: VA quarantine holds %u range(s), %" PRIu64 " KiB (caps: %u ranges, %" PRIu64
                         " MiB)",
                         n, held / 1024, cap, byte_cap / (1024 * 1024));
            return 0;
         }
      }
   }

   simple_mtx_lock(&orbis_va_lock);
   struct amdgpu_va **link = &orbis_va_list;
   while (*link && *link != va_range_handle)
      link = &(*link)->next;

   const bool found = *link != NULL;
   if (found)
      *link = va_range_handle->next;
   simple_mtx_unlock(&orbis_va_lock);

   if (!found) {
      /* Freeing a range that is not in the list means the handle is stale or foreign, and continuing would
       * corrupt the list. Loud, and no free. */
      mesa_loge("orbis-drm: ac_drm_va_range_free on an unknown handle %p", (void *)va_range_handle);
      return -EINVAL;
   }

   free(va_range_handle);
   return 0;
}

int
ac_drm_bo_va_op_raw(ac_drm_device *dev, uint32_t bo_handle, uint64_t offset, uint64_t size, uint64_t addr,
                    uint64_t flags, uint32_t ops)
{
   return orbis_bo_va_op(bo_handle, offset, size, addr, flags, ops);
}

int
ac_drm_bo_va_op_raw2(ac_drm_device *dev, uint32_t bo_handle, uint64_t offset, uint64_t size, uint64_t addr,
                     uint64_t flags, uint32_t ops, uint32_t vm_timeline_syncobj_out,
                     uint64_t vm_timeline_point, uint64_t input_fence_syncobj_handles,
                     uint32_t num_syncobj_handles)
{
   int r = orbis_bo_va_op(bo_handle, offset, size, addr, flags, ops);
   if (r)
      return r;

   /* The point of raw2 is that the caller can WAIT for the page-table update. Ours is already done, so the
    * timeline point is signalled immediately - and it must be signalled, or the winsys waits forever on an
    * update that already happened. */
   if (vm_timeline_syncobj_out)
      orbis_sync_signal(NULL, &vm_timeline_syncobj_out, 1);
   return 0;
}

int
ac_drm_query_heap_info(ac_drm_device *dev, uint32_t heap, uint32_t flags, struct amdgpu_heap_info *info)
{
   /* WHAT THIS DEVICE CAN ACTUALLY ALLOCATE, which is the arena and not the console's memory. Reporting the
    * physical size would be the flattering answer and the wrong one: an application told about 4 GB and given
    * a 256 MiB arena fails at allocation time with no explanation, which is the failure mode this port keeps
    * choosing against.
    *
    * ONE POOL, TWO NAMES. VRAM and GTT are the same memory here - IDS_FLAGS_FUSION, one unified GDDR5 pool -
    * so both heaps describe the same arena rather than dividing it. Dividing it would make the smaller half
    * the real ceiling for no reason. */
   memset(info, 0, sizeof(*info));

   simple_mtx_lock(&orbis_va_lock);
   uint64_t used = 0;
   uint64_t largest = 0;
   uint64_t cursor = orbis_va_base;
   for (const struct amdgpu_va *va = orbis_va_list; va; va = va->next) {
      used += va->size;
      if (va->base > cursor && va->base - cursor > largest)
         largest = va->base - cursor;
      cursor = va->base + va->size;
   }
   if (orbis_va_end > cursor && orbis_va_end - cursor > largest)
      largest = orbis_va_end - cursor;
   simple_mtx_unlock(&orbis_va_lock);

   info->heap_size = orbis_va_end - orbis_va_base;
   info->heap_usage = used;
   info->max_allocation = largest;
   return 0;
}

/* ⚠ WAIT FOR THE GPU BEFORE THE CPU READS PIXELS IT WROTE. PHOTOGRAPHED, NOT SUSPECTED.
 *
 * The menu showed a black region whose boundary was a STAIRCASE of rectangles with finely serrated edges, appearing
 * on every screen change and resolving after a few seconds. That shape is not geometry: a PREFIX of a surface in
 * TILE order maps to exactly that staircase in screen space, macro-tiles making the steps and micro-tiles the
 * serration. It is a copy that read the image while the GPU was still filling it.
 *
 * WHY NOTHING WAITED. wsi_common_headless's present is a no-op upstream - the platform exists to render into
 * nothing - so no fence wait was ever needed there. This port put a REAL CPU copy into that no-op
 * (wsi_common_headless.c, HAVE_ORBIS_PLATFORM) and inherited the absence of the wait with it. The out-syncobjs are
 * honest, the app's fences are honest; the copy simply asks nobody.
 *
 * ONE QUEUE, ONE LABEL, so the guarantee is arithmetic: when the label reaches the last sequence number handed out,
 * everything submitted has retired. No per-BO tracking, no residency question.
 *
 * BOUNDED AND LOUD like every other wait in this file: it returns false with a line rather than spinning forever,
 * because a frame that presents stale pixels is better than a console that stops. */
/* How long this port has spent waiting for the GPU on the CPU's behalf, and how often. Read by the periodic submit
 * line, because "29 submissions per second" needs a breakdown before anything is changed to improve it. */
uint64_t orbis_idle_wait_ns;
uint64_t orbis_idle_waits;

/* ⚠ IS THIS ADDRESS EVEN OURS? Asked where descriptors are BUILT, not where the GPU faults on them.
 *
 * A world frame took a GPU page fault reading VA 0x0000000ff0000000 - the texture cache, on a read - and that address
 * appears nowhere in the frame's 155664 dwords of PM4, so it lives in a descriptor in memory. `RADV_DEBUG=zerovram`
 * moved the offset and left the base, so it is written deliberately rather than being uninitialised.
 *
 * One arena, one window, so "ours" is a comparison. Exported so that ac_descriptors.c can refuse to build a
 * descriptor pointing outside it - a wrong address caught at construction names the surface; the same address caught
 * by the GPU names nothing. */
bool
ac_orbis_va_is_ours(uint64_t va, uint64_t size)
{
   return va >= orbis_va_base && va + size <= orbis_va_end;
}

/* ⚠ IS THIS ADDRESS STILL MAPPED TO ANYTHING? A different question from ac_orbis_va_is_ours, and the one the hunt
 * now turns on.
 *
 * Where it stands: the CPU writes coherent bounding boxes into descriptor memory (run AA proved the writer is the CPU
 * - twelve watched ranges dirtied before a submission, none across GPU execution), the binding fits its set (run AB),
 * and no two live mappings overlap (run U). The only shape left that satisfies all three is a descriptor set whose
 * POOL WAS DESTROYED while the command buffer still references it: the memory is then handed to the next allocation,
 * the CPU fills that allocation with real data, and both the audit and the shader read it as descriptors.
 *
 * On Linux that is invisible-to-fatal by luck rather than benign: freeing the pool unmaps its pages, so the stale
 * read faults or reads a hole. Here the arena stays mapped and the read silently succeeds against somebody else's
 * data.
 *
 * The live-mapping table already exists for the overlap check, so this is a walk of it rather than new bookkeeping. */
/* ⚠ WHO HELD THIS ADDRESS BEFORE? The last question this hunt needs answered in ONE line.
 *
 * Measured in one log (run AG): 30 of 28 descriptor pools are created inside ranges that some BO had unmapped. That
 * is not a defect by itself - reuse is what an allocator does - but run AA proved the writer of the bounding boxes
 * found in descriptor memory is the CPU, and a stale pointer into a freed buffer is the only CPU writer that can
 * reach a live pool's memory here.
 *
 * Cross-referencing the audit's set VA against the unmap lines works but is arithmetic done afterwards, and this
 * hunt has already published one conclusion built from two different boots. So the audit asks directly, and the
 * finding arrives as one self-contained line. */
/* ⚠ A SNAPSHOT OF WHAT IS MAPPED, so a caller can ask about thousands of descriptors without walking this table
 * thousands of times.
 *
 * The question that needs it: every arena-bounds check in this port asks "is this address ours", and a STALE
 * descriptor - one pointing at a buffer that has been freed - passes every one of them, because the arena keeps that
 * address mapped forever. The maintainer saw the consequence directly: the world drew for a few frames with its
 * triangles scrambled, which is what a vertex descriptor pointing at somebody else's live memory looks like. It only
 * becomes a fault when the bytes there happen to decode as a wild base.
 *
 * So the audit needs "is this address in a LIVE mapping", per descriptor, cheaply. One copy per audit pass. */
unsigned
ac_orbis_live_snapshot(uint64_t *addrs, uint64_t *ends, unsigned max)
{
   unsigned n = 0;
   simple_mtx_lock(&orbis_map_lock);
   for (unsigned i = 0; i < ORBIS_LIVE_MAPS && n < max; ++i) {
      if (orbis_live_map[i].addr == 0)
         continue;
      addrs[n] = orbis_live_map[i].addr;
      ends[n] = orbis_live_map[i].end;
      ++n;
   }
   simple_mtx_unlock(&orbis_map_lock);
   return n;
}

uint32_t
ac_orbis_previous_owner(uint64_t va, uint64_t *start, uint64_t *end)
{
   uint32_t handle = 0;
   simple_mtx_lock(&orbis_map_lock);
   /* Newest first, so the most recent tenant wins when a range has been recycled more than once. */
   for (unsigned i = 0; i < ORBIS_DEAD_MAPS; ++i) {
      const unsigned d = (orbis_dead_at - 1 - i) % ORBIS_DEAD_MAPS;
      if (orbis_dead_map[d].addr == 0)
         continue;
      if (va >= orbis_dead_map[d].addr && va < orbis_dead_map[d].end) {
         handle = orbis_dead_map[d].handle;
         if (start)
            *start = orbis_dead_map[d].addr;
         if (end)
            *end = orbis_dead_map[d].end;
         break;
      }
   }
   simple_mtx_unlock(&orbis_map_lock);
   return handle;
}

bool
ac_orbis_va_is_live(uint64_t va, uint64_t size)
{
   bool live = false;
   simple_mtx_lock(&orbis_map_lock);
   for (unsigned i = 0; i < ORBIS_LIVE_MAPS; ++i) {
      if (orbis_live_map[i].addr == 0)
         continue;
      if (va >= orbis_live_map[i].addr && va + size <= orbis_live_map[i].end) {
         live = true;
         break;
      }
   }
   simple_mtx_unlock(&orbis_map_lock);
   return live;
}

/* ⚠ THE ASSERT THAT HAS BEEN SILENTLY PASSING ALL HUNT, PUT BACK AS A LOG.
 *
 * `ac_cmdbuf_emit_32bit_pointer` writes only base[31:0] to a user-data SGPR and the shader rebuilds the top half
 * from address32_hi, which is 0x2 here. Its only guard is
 *
 *     assert((va) == 0 || ((va) >> 32) == (info)->address32_hi);
 *
 * and a release build compiles that away. So an address whose top half is NOT 0x2 is truncated in silence, and the
 * shader then reads 0x2<garbage> - an address that looks structurally plausible and is mapped by nothing.
 *
 * THAT IS THE SHAPE THE DECOY EXPOSED. With 2 MiB mapped at 0xff0000000 the world drew for a few seconds at 10 fps
 * and then faulted at 0x26d872000 and 0x26d1d7000, both high half 0x2, both far past the arena's end of
 * 0x248400000. A garbage low dword under a correct high half is exactly what this truncation produces, and it is
 * the first hypothesis in this hunt that predicts the ADDRESSES rather than accommodating them.
 *
 * Checked for both spellings: a truncated 32-bit pointer and a full 64-bit one. Once per distinct address, with
 * the reconstructed value, because what the shader will read is not what was passed in.
 *
 * ⚠ THE RECONSTRUCTION IS THE POINT OF THE MESSAGE. Logging the va that was passed would name a value nobody
 * dereferences; the fault will name `(address32_hi << 32) | (va & 0xffffffff)`, and matching the two is how this
 * line gets connected to a klog. */
void
ac_orbis_check_pointer(const char *what, uint32_t sh_offset, uint64_t va, bool is_32bit)
{
   if (va == 0)
      return;

   const uint64_t hi = orbis_va_base & 0xffffffff00000000ull;
   const uint64_t seen_by_shader = is_32bit ? (hi | (va & 0xffffffffull)) : va;

   if (ac_orbis_va_is_ours(seen_by_shader, 4) && (!is_32bit || (va >> 32) == (hi >> 32)))
      return;

   static uint64_t said[8];
   static unsigned n_said;
   for (unsigned i = 0; i < n_said; ++i)
      if (said[i] == va)
         return;
   if (n_said < ARRAY_SIZE(said))
      said[n_said++] = va;

   mesa_loge("orbis: %s POINTER THE SHADER CANNOT USE - SH 0x%03x got 0x%" PRIx64
             "; the shader will read 0x%" PRIx64 ", and the arena is 0x%" PRIx64 "..0x%" PRIx64
             ". This is the truncation ac_cmdbuf_emit_32bit_pointer's assert is compiled out of.",
             what, sh_offset, va, seen_by_shader, orbis_va_base, orbis_va_end);
}

/* ⚠ THE GRAVEYARD: DESCRIPTOR POOLS THAT HAVE BEEN DESTROYED, AND WHERE THEY USED TO LIVE.
 *
 * This exists because every bounds test in this port is the wrong test for the defect that is left. They all
 * ask "is this address inside the arena", and the arena maps every address for the life of the process - so a
 * descriptor set whose pool was freed passes every one of them. That is measurable, not theoretical:
 * ac_orbis_check_pointer has fired ZERO times across this entire hunt while the GPU keeps faulting on a
 * descriptor read at 0xff0000000, an address that appears in no packet, in no descriptor this driver builds,
 * and in no mapped memory except as scattered image data at the rate coincidence predicts.
 *
 * On Linux this bug is caught by accident: freeing a pool unmaps its pages, so the stale read faults or reads
 * a hole. Here it silently succeeds against whatever owns that memory now - which is why it shows up as
 * scrambled geometry that gets worse the longer the title runs, and only becomes a fault when the bytes there
 * happen to decode as a wild base.
 *
 * A ring, not a list: pools are created and destroyed continuously and only the recent ones can still be
 * referenced by a command buffer in flight. When it wraps it says so, because a graveyard that silently
 * forgets is a check that silently stops working. */
#define ORBIS_DEAD_POOLS 256

static struct {
   uint64_t addr, end;
   const char *what;
} orbis_dead_pool[ORBIS_DEAD_POOLS];
static unsigned orbis_dead_pool_next;
static unsigned orbis_dead_pool_wrapped;
static simple_mtx_t orbis_dead_pool_lock = SIMPLE_MTX_INITIALIZER;

/* ⚠ THE SAME GRAVEYARD SERVES BOTH, and the tag is what makes a finding readable.
 *
 * Descriptor pools were the first suspect and were ELIMINATED on hardware: both halves of that audit ran,
 * both were seen to fire on the laptop, neither fired on the console, and the stamp array never overflowed.
 * So the released storage behind the bad descriptor is not a pool - which leaves the class one level up,
 * ordinary buffer objects freed while something still refers to them.
 *
 * One ring, one tag per entry, because two rings would be two things to keep in step and the question is
 * identical: was this address released? */
void
ac_orbis_note_freed_range(uint64_t va, uint64_t size, const char *what)
{
   if (va == 0 || size == 0)
      return;

   simple_mtx_lock(&orbis_dead_pool_lock);
   orbis_dead_pool[orbis_dead_pool_next].addr = va;
   orbis_dead_pool[orbis_dead_pool_next].end = va + size;
   orbis_dead_pool[orbis_dead_pool_next].what = what;
   orbis_dead_pool_next = (orbis_dead_pool_next + 1) % ORBIS_DEAD_POOLS;
   if (orbis_dead_pool_next == 0 && !orbis_dead_pool_wrapped) {
      orbis_dead_pool_wrapped = 1;
      mesa_logi("orbis-drm: the destroyed-pool ring wrapped at %u entries - older pools are no longer "
                "checked, so a miss from here on is not proof of absence",
                ORBIS_DEAD_POOLS);
   }
   simple_mtx_unlock(&orbis_dead_pool_lock);
}

bool
ac_orbis_va_in_freed_range(uint64_t va, uint64_t *lo, uint64_t *hi, const char **what)
{
   bool found = false;

   simple_mtx_lock(&orbis_dead_pool_lock);
   for (unsigned i = 0; i < ORBIS_DEAD_POOLS; ++i) {
      if (orbis_dead_pool[i].end == 0 || va < orbis_dead_pool[i].addr || va >= orbis_dead_pool[i].end)
         continue;
      if (lo != NULL)
         *lo = orbis_dead_pool[i].addr;
      if (hi != NULL)
         *hi = orbis_dead_pool[i].end;
      if (what != NULL)
         *what = orbis_dead_pool[i].what;
      found = true;
      break;
   }
   simple_mtx_unlock(&orbis_dead_pool_lock);
   return found;
}

/* ⚠ WHO HOLDS THAT POINTER? THE ARENA IS SEARCHED FOR IT, BECAUSE THE PM4 STREAM DOES NOT CONTAIN IT.
 *
 * The open defect: a world frame faults reading VA 0xff0000000-ish, the base is stable across runs, the offset
 * drifts, the address appears in none of the frame's 155664 dwords, and ac_orbis_va_is_ours refuses nothing - so
 * no descriptor RADV BUILT carries it. That leaves one possibility: the value is DATA, written into a buffer by
 * the CPU or by a shader, and read as a descriptor or as an address later.
 *
 * So this reads memory instead of packets. Every mapped BO is scanned for the dword, and a hit is reported as
 * "handle H, offset O" - which turns "an address from nowhere" into a named buffer at a named offset. That is the
 * one fact this hunt does not have.
 *
 * PATTERNS, PLURAL, AND A MASK, because a 40-bit address has several 32-bit spellings: a V# holds base[31:0] in
 * word 0, a T# holds base >> 8, and the drifting low bits must be ignorable. `ORBIS_SCAN_VA=0xff000000,0x0ff00000`
 * tests both forms in the same pass, and `:<mask>` on a value compares only the bits that matter -
 * `0x0ff00000:0xfff00000` finds the T# form whatever its offset. Several patterns rather than several runs
 * because a scan is expensive and a console trip is more so.
 *
 * WHEN: every 512th submission by default, `ORBIS_SCAN_EVERY=<n>` to change it, `ORBIS_SCAN_AT=<n>` for exactly
 * one. The faulting frame's number moves between runs (5884, 5902, 6000, 6084) so a hand-picked number is a
 * guess; a stride costs a dozen scans across a boot and cannot miss by being off by two.
 *
 * BOUNDED AND LOUD: `ORBIS_SCAN_MAX_MIB` (256 by default) caps how much is read per scan, and what the cap
 * SKIPPED is logged with it. A silent cap would read as "the value is not in memory" when the truth is "the
 * value is not in the first 256 MiB this walk happened to reach".
 */
static void
orbis_scan_for_va(uint64_t seq)
{
   /* Either spelling turns the scan on; ORBIS_SCAN_VA64 alone is the common case now that exact 64-bit patterns
    * are what identify a descriptor. */
   const char *const spec = getenv("ORBIS_SCAN_VA");
   if (spec == NULL && getenv("ORBIS_SCAN_VA64") == NULL)
      return;

   const char *const at_s = getenv("ORBIS_SCAN_AT");
   const char *const every_s = getenv("ORBIS_SCAN_EVERY");
   const uint64_t    at = at_s != NULL ? strtoull(at_s, NULL, 0) : 0;
   const uint64_t    every = every_s != NULL ? strtoull(every_s, NULL, 10) : 512;
   if (at != 0) {
      if (seq != at)
         return;
   } else if (every == 0 || seq % every != 0) {
      return;
   }

#define ORBIS_SCAN_PATTERNS 8
   uint32_t want[ORBIS_SCAN_PATTERNS], mask[ORBIS_SCAN_PATTERNS];
   unsigned n_pat = 0;
   for (const char *c = spec != NULL ? spec : ""; *c != '\0' && n_pat < ORBIS_SCAN_PATTERNS;) {
      char *end = NULL;
      const uint32_t v = (uint32_t)strtoull(c, &end, 0);
      if (end == c)
         break;
      uint32_t m = 0xffffffffu;
      if (*end == ':') {
         m = (uint32_t)strtoull(end + 1, &end, 0);
         /* A mask of zero matches everything, which would print the whole arena. Almost certainly a typo. */
         if (m == 0) {
            mesa_logw("orbis-drm: ORBIS_SCAN_VA pattern %u has mask 0 - it would match every dword; ignored",
                      n_pat);
            goto next;
         }
      }
      want[n_pat] = v & m;
      mask[n_pat] = m;
      ++n_pat;
   next:
      while (*end == ',' || *end == ' ')
         ++end;
      c = end;
   }
   if (n_pat == 0 && getenv("ORBIS_SCAN_VA64") == NULL) {
      mesa_logw("orbis-drm: ORBIS_SCAN_VA=\"%s\" parsed to no patterns - nothing scanned", spec);
      return;
   }

   /* ⚠ 64-BIT PATTERNS, BECAUSE THE FIRST RUN OF THIS SCAN ANSWERED WITH NOISE. Two dword patterns with the low
    * bits masked off produced 15051 hits in a 128 MiB upload buffer: `0xf00xxxxx` and `0x0ff0xxxx` are ordinary
    * numbers, and a mask loose enough to absorb a drifting offset is loose enough to match anything. The exact
    * value is what identifies a descriptor, and for that the pattern has to be as wide as the field.
    *
    * `ORBIS_SCAN_VA64=<qword>[:<mask>]` reads two consecutive dwords at every 4-byte position, so it catches BOTH
    * spellings of a 48-bit base in one pass: a plain 64-bit pointer, and a V#'s word pair, where word 0 is
    * base[31:0] and word 1 holds base[47:32] in its low 16 bits with stride and flags above. Masking with
    * 0x0000ffffffffffff keeps exactly the base and ignores what shares word 1:
    *
    *     ORBIS_SCAN_VA64=0xff0000000:0x0000ffffffffffff
    *
    * A T# needs no 64-bit form - it holds base >> 8, which fits a dword - so ORBIS_SCAN_VA=0x0ff00000 EXACTLY,
    * with no mask, is that half of the question. */
   uint64_t want64[4], mask64[4];
   unsigned n_pat64 = 0;
   const char *const spec64 = getenv("ORBIS_SCAN_VA64");
   for (const char *c = spec64 != NULL ? spec64 : ""; *c != '\0' && n_pat64 < 4;) {
      char *end = NULL;
      const uint64_t v = strtoull(c, &end, 0);
      if (end == c)
         break;
      uint64_t m = ~0ull;
      if (*end == ':')
         m = strtoull(end + 1, &end, 0);
      if (m == 0) {
         mesa_logw("orbis-drm: ORBIS_SCAN_VA64 pattern %u has mask 0 - ignored", n_pat64);
      } else {
         want64[n_pat64] = v & m;
         mask64[n_pat64] = m;
         ++n_pat64;
      }
      while (*end == ',' || *end == ' ')
         ++end;
      c = end;
   }

   const char *const max_s = getenv("ORBIS_SCAN_MAX_MIB");
   const uint64_t    budget = (max_s != NULL ? strtoull(max_s, NULL, 10) : 256) * 1024 * 1024;
   const char *const print_s = getenv("ORBIS_SCAN_PRINT");
   const unsigned    print_max = print_s != NULL ? (unsigned)strtoul(print_s, NULL, 10) : 32;

   const uint64_t t0 = os_time_get_nano();
   uint64_t scanned = 0, skipped = 0, hits = 0;
   unsigned bos = 0, bos_skipped = 0, printed = 0;

   simple_mtx_lock(&orbis_bo_lock);
   for (unsigned h = 0; h < orbis_bo_slots; ++h) {
      const struct orbis_bo *const bo = orbis_bo_slot[h];
      /* ->cpu is set by the VA op, so an unmapped BO has no address to read and nothing the GPU can reach
       * either. Foreign pages are somebody else's mapping and are still readable - and still a candidate. */
      if (bo == NULL || bo->cpu == NULL || bo->size < 4)
         continue;
      if (scanned + bo->size > budget) {
         skipped += bo->size;
         ++bos_skipped;
         continue;
      }
      ++bos;
      scanned += bo->size;

      const uint32_t *const p = (const uint32_t *)bo->cpu;
      const uint64_t        n = bo->size / 4;
      for (uint64_t i = 0; i < n; ++i) {
         const uint32_t d = p[i];
         int            hit = -1; /* pattern index, negated by 100 for the 64-bit set */

         for (unsigned k = 0; k < n_pat && hit < 0; ++k)
            if ((d & mask[k]) == want[k])
               hit = (int)k;
         /* The pair, and only where a pair exists: the last dword of a BO has no successor. */
         if (hit < 0 && i + 1 < n) {
            const uint64_t q = ((uint64_t)p[i + 1] << 32) | d;
            for (unsigned k = 0; k < n_pat64 && hit < 0; ++k)
               if ((q & mask64[k]) == want64[k])
                  hit = 100 + (int)k;
         }
         if (hit < 0)
            continue;

         ++hits;
         if (printed >= print_max)
            continue;
         ++printed;
         /* ⚠ THE NEIGHBOURHOOD, NOT THE DWORD. A hit on its own says an address is in memory; the eight dwords
          * around it say WHAT it is. A V# is four dwords on a 16-byte boundary - stride and num_records in words
          * 1 and 2 identify it immediately - a T# is eight, and a plain pointer array has neither. Reading that
          * by hand is how every other descriptor in this hunt was identified, so the scan prints what the hand
          * needs instead of a number that needs another run to explain. */
         const uint64_t ctx = (i & ~3ull) >= 4 ? (i & ~3ull) - 4 : 0;
         mesa_logi("orbis-drm: SCAN hit pattern %s%u at BO handle %u offset %" PRIu64 " (VA 0x%" PRIx64
                   ", BO %" PRIu64 " KiB, heap 0x%x, flags 0x%" PRIx64 ")",
                   hit >= 100 ? "64:" : "dw:", (unsigned)(hit >= 100 ? hit - 100 : hit), bo->handle, i * 4,
                   (uint64_t)(uintptr_t)bo->cpu + i * 4, bo->size / 1024, bo->preferred_heap, bo->alloc_flags);
         mesa_logi("orbis-drm:   dw[%" PRIu64 "..] %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x",
                   ctx, ctx + 0 < n ? p[ctx + 0] : 0, ctx + 1 < n ? p[ctx + 1] : 0, ctx + 2 < n ? p[ctx + 2] : 0,
                   ctx + 3 < n ? p[ctx + 3] : 0, ctx + 4 < n ? p[ctx + 4] : 0, ctx + 5 < n ? p[ctx + 5] : 0,
                   ctx + 6 < n ? p[ctx + 6] : 0, ctx + 7 < n ? p[ctx + 7] : 0, ctx + 8 < n ? p[ctx + 8] : 0,
                   ctx + 9 < n ? p[ctx + 9] : 0, ctx + 10 < n ? p[ctx + 10] : 0, ctx + 11 < n ? p[ctx + 11] : 0);
      }
   }
   simple_mtx_unlock(&orbis_bo_lock);

   mesa_logi("orbis-drm: SCAN at submit #%" PRIu64 ": %" PRIu64 " hits (%u printed) in %u BOs, %" PRIu64
             " MiB read in %" PRIu64 " ms; SKIPPED %u BOs / %" PRIu64 " MiB over the %" PRIu64
             " MiB budget (ORBIS_SCAN_MAX_MIB); %" PRIu64 " MiB is mapped in total, so that is the budget that "
             "covers everything",
             seq, hits, printed, bos, scanned / (1024 * 1024), (os_time_get_nano() - t0) / 1000000,
             bos_skipped, skipped / (1024 * 1024), budget / (1024 * 1024),
             (scanned + skipped) / (1024 * 1024));
#undef ORBIS_SCAN_PATTERNS
}

/* ⚠ WHAT HAS BEEN SUBMITTED SO FAR, so a caller can wait for THIS frame rather than for the newest one.
 *
 * ac_orbis_wait_gpu_idle below waits for orbis_submit_seq_no as it stands WHEN IT IS CALLED. That was
 * exactly right while the present copied immediately: the newest submission was the frame being
 * presented. It stopped being right the moment the present was deferred by a frame - by then the next
 * frame has also been submitted, so "wait for the newest" waits for the frame AFTER the one being
 * copied, and the deferral buys nothing at all. Measured: 71% of the frame spent waiting, before and
 * after, to the percentage point.
 *
 * So the WSI records this at present time and waits for that value later. */
uint64_t
ac_orbis_gpu_seq_now(void)
{
   return orbis_submit_seq_no;
}

bool
ac_orbis_wait_gpu_seq(uint64_t want, uint64_t timeout_ns)
{
   const uint64_t entered = os_time_get_nano();
   const int64_t  deadline = (int64_t)(entered + timeout_ns);

   while ((int32_t)(*orbis_fence_label - (uint32_t)want) < 0) {
      if (os_time_get_nano() >= (uint64_t)deadline) {
         static unsigned said;
         if (orbis_budget(&said, 4)) {
            mesa_logw("orbis-drm: waited %llu ms for the GPU to reach submission #%" PRIu64
                      " and the label is %u - presenting whatever is in the image",
                      (unsigned long long)(timeout_ns / 1000000), want, *orbis_fence_label);
         }
         return false;
      }
      orbis_poll_pause();
   }
   return true;
}

bool
ac_orbis_wait_gpu_idle(uint64_t timeout_ns)
{
   const uint64_t want = orbis_submit_seq_no;
   const uint64_t entered = os_time_get_nano();
   const int64_t  deadline = (int64_t)(entered + timeout_ns);
   ++orbis_idle_waits;

   while ((int32_t)(*orbis_fence_label - (uint32_t)want) < 0) {
      if (os_time_get_nano() >= (uint64_t)deadline) {
         static unsigned said;
         if (orbis_budget(&said, 4)) {
            mesa_logw("orbis-drm: waited %llu ms for the GPU to reach submission #%" PRIu64
                      " and the label is %u - presenting whatever is in the image",
                      (unsigned long long)(timeout_ns / 1000000), want, *orbis_fence_label);
         }
         orbis_idle_wait_ns += os_time_get_nano() - entered;
         return false;
      }
      orbis_poll_pause();
   }
   orbis_idle_wait_ns += os_time_get_nano() - entered;
   return true;
}

/* ---------------------------------------------------------------- submission and contexts - phase 4 */

/* A CONTEXT HERE IS A NAME, NOT A RESOURCE, and that is the honest description rather than a shortcut.
 * On amdgpu a context is a kernel object carrying its own ring assignments, priority and reset state; Sony
 * exposes ONE submission path, so there is nothing underneath to create. What RADV actually needs from the
 * value is that it is non-zero, unique, and accepted again by submit and fence-status - so it is an index
 * into a small table, the same shape as the syncobj slots above.
 *
 * BOUNDED AND SMALL ON PURPOSE. RADV creates one per radeon_winsys_ctx, which is per queue family in
 * practice; a fixed table means running out says so instead of growing silently, and 16 is far past what one
 * device asks for. */

int
ac_drm_cs_ctx_create2(ac_drm_device *dev, uint32_t priority, uint32_t *ctx_id)
{
   /* Signed, because AMDGPU_CTX_PRIORITY_* are negative below NORMAL - VERY_LOW is -1023 - and the
    * parameter's uint32_t is the kernel ABI's spelling of the same bits. Reading it unsigned would make
    * every low priority look enormous. */
   const int32_t prio = (int32_t)priority;

   /* ⚠ REFUSED, NOT ACCEPTED-AND-IGNORED, AND THE CTS IS WHY.
    *
    * This used to log "not honoured" and hand back a context anyway. RADV probes each priority by
    * CREATING one - radv_amdgpu_winsys.c:366, radv_amdgpu_ctx_is_priority_permitted - and puts the
    * ones that succeed into global_priority_mask, which VK_EXT_global_priority_query then reports to
    * the application as the priorities it may ask for. Saying yes to all four made this driver
    * advertise REALTIME on a console with one submission path and no priority at all.
    *
    * dEQP-VK.api.device_init.create_device_global_priority_khr.basic is what found it: it took the
    * process down, after 4323 tests with no failures at all. A driver that quietly under-delivers on
    * its own API is worse than one that refuses, and this project has a written position on that.
    *
    * NORMAL is the truth here and everything else is a refusal. -EINVAL rather than -EPERM because
    * this is not a permission the kernel could grant with different credentials; there is nowhere
    * for a priority to go. */
   if (prio != 0 /* AMDGPU_CTX_PRIORITY_NORMAL */) {
      static unsigned said;
      if (orbis_budget(&said, 4)) {
         mesa_logi("orbis-drm: context priority %d refused - this console has one submission path, so "
                   "NORMAL is the only priority that exists and claiming otherwise is how "
                   "VK_EXT_global_priority_query starts lying",
                   prio);
      }
      return -EINVAL;
   }

   simple_mtx_lock(&orbis_ctx_lock);
   for (uint32_t i = 0; i < ORBIS_MAX_CONTEXTS; i++) {
      if (orbis_ctx[i].taken)
         continue;
      orbis_ctx[i].taken = true;
      orbis_ctx[i].priority = prio;
      simple_mtx_unlock(&orbis_ctx_lock);
      /* index + 1, so 0 is never a valid handle - DRM's convention, and it makes a zero-initialised
       * ctx_handle fail loudly rather than address context 0. */
      *ctx_id = i + 1;
      return 0;
   }
   simple_mtx_unlock(&orbis_ctx_lock);

   mesa_loge("orbis-drm: out of contexts (%d)", ORBIS_MAX_CONTEXTS);
   return -ENOMEM;
}

int
ac_drm_cs_ctx_free(ac_drm_device *dev, uint32_t ctx_id)
{
   if (ctx_id == 0 || ctx_id > ORBIS_MAX_CONTEXTS)
      return -EINVAL;

   simple_mtx_lock(&orbis_ctx_lock);
   const bool was_taken = orbis_ctx[ctx_id - 1].taken;
   orbis_ctx[ctx_id - 1].taken = false;
   simple_mtx_unlock(&orbis_ctx_lock);

   /* Freeing a context that was never created is a bug in the caller, not a no-op to absorb quietly. */
   return was_taken ? 0 : -EINVAL;
}

int
ac_drm_cs_ctx_stable_pstate(ac_drm_device *dev, uint32_t ctx_id, uint32_t op, uint32_t flags,
                            uint32_t *out_flags)
{
   /* Pinning the GPU to a fixed clock so that profile timings are comparable. RADV asks for it only around
    * SQTT capture (radv_amdgpu_ctx_set_pstate), so refusing costs stable timings in a trace and nothing in
    * rendering. If it ever matters, the fork's own gnmtune.h is where this platform's clock knobs live. */
   ORBIS_DRM_REFUSED("stable pstate is a profiling knob; no equivalent is exposed here");
   return -ENOSYS;
}

/* ---------------------------------------------------------------- submission: DECODE AND LOG
 *
 * THE WHOLE PORT IS IN THIS ONE FUNCTION, and it takes the KERNEL's raw ABI structures - so this layer
 * abstracts the TRANSPORT and not the MODEL. The arm has to implement amdgpu's submission SEMANTICS on top
 * of Sony's rather than swap one call for another.
 *
 * ⚠ THIS VERSION SUBMITS NOTHING, ON PURPOSE, and the log says so on every call. PLAN.md's phase 4 step 1:
 * walk the chunk array, print what arrived, and measure the shape of a real submission - how many IBs, in
 * what order, chained or not, which chunk kinds RADV actually uses - rather than assuming it from reading.
 * The next step turns IB chunks into sceGnmSubmitCommandBuffers' arrays, and by then the shape is known.
 *
 * ⚠ AND IT REPORTS SUCCESS, WHICH IS A LIE THAT HAS TO BE PAID FOR. RADV waits on the fences a submit
 * promises, so returning failure stops the loop here while returning success without signalling hangs it.
 * The out-syncobjs are therefore signalled immediately and ac_drm_cs_query_fence_status reports every fence
 * expired - which means ANY TEST THAT READS BACK GPU RESULTS WILL SEE STALE MEMORY AND CALL IT DONE. That is
 * acceptable while the goal is to observe the chunks and nothing else; it is not acceptable one line further.
 */
static simple_mtx_t orbis_submit_lock = SIMPLE_MTX_INITIALIZER;

/* RADV's own bound is MAX_RINGS_PER_TYPE-independent: it chains within a CS and submits preamble, body and
 * postamble as separate chunks, so three is the floor and a few more is the realistic ceiling. 16 is far past
 * anything measured, and exceeding it is a loud refusal rather than a silent truncation. */
#define ORBIS_MAX_IBS_PER_SUBMIT 16

static const char *
orbis_chunk_name(uint32_t id)
{
   switch (id) {
   case AMDGPU_CHUNK_ID_IB: return "IB";
   case AMDGPU_CHUNK_ID_FENCE: return "FENCE";
   case AMDGPU_CHUNK_ID_DEPENDENCIES: return "DEPENDENCIES";
   case AMDGPU_CHUNK_ID_SYNCOBJ_IN: return "SYNCOBJ_IN";
   case AMDGPU_CHUNK_ID_SYNCOBJ_OUT: return "SYNCOBJ_OUT";
   case AMDGPU_CHUNK_ID_BO_HANDLES: return "BO_HANDLES";
   case AMDGPU_CHUNK_ID_SCHEDULED_DEPENDENCIES: return "SCHEDULED_DEPENDENCIES";
   case AMDGPU_CHUNK_ID_SYNCOBJ_TIMELINE_WAIT: return "SYNCOBJ_TIMELINE_WAIT";
   case AMDGPU_CHUNK_ID_SYNCOBJ_TIMELINE_SIGNAL: return "SYNCOBJ_TIMELINE_SIGNAL";
   case AMDGPU_CHUNK_ID_CP_GFX_SHADOW: return "CP_GFX_SHADOW";
   default: return "?";
   }
}

int
ac_drm_cs_submit_raw2(ac_drm_device *dev, uint32_t ctx_id, uint32_t bo_list_handle, int num_chunks,
                      struct drm_amdgpu_cs_chunk *chunks, uint64_t *seq_no)
{
   /* End to end, because Sony's call and the three waits inside are already measured and small - so whatever else
    * this function costs shows up as the difference. */
   const uint64_t orbis_submit_t0 = os_time_get_nano();
   orbis_api_count(ORBIS_ID_ARM_SUBMIT, 1);
   /* How many tickets have been handed out before this call - a relaxed read for the hang detector below, which
    * runs before the lock on purpose. The ticket itself is taken further down; see the note there. */
   const uint64_t queued = orbis_submit_seq_no;

   /* STAGE MARKERS, and they exist because two runs could only say "somewhere in the submit". Every stage of
    * this function logs once per submit for the first few, mesa_log flushes per line, so the LAST line printed
    * names the stage that blocked. Cheaper than another hypothesis: three of mine died on this one bug. */
   /* ⚠ AND THE BOUND IS A KNOB NOW, because a run died on submission 36 and the bound was 8.
    *
    * OpenGothic reached its frame loop and the log stops immediately after "#36: flattened" - so the death is in
    * the Sony call or in what follows it, and every line that would name which was suppressed by this bound. The
    * first eight submissions of a boot are no longer the interesting ones.
    *
    * ORBIS_TRACE_SUBMITS=<n> traces the first n; 0 traces every submission, which at 30-odd lines each is a large
    * log and exactly what a hang 36 submits in needs. Default stays 8 so a normal run is readable. */
   const uint64_t stage_limit = orbis_trace_submits();
#define ORBIS_SUBMIT_STAGE(what)                                                                   \
   do {                                                                                            \
      if (orbis_trace() && (stage_limit == 0 || orbis_submit_seq_no <= stage_limit))                \
         mesa_logi("orbis-drm: submit #%" PRIu64 " stage: %s", orbis_submit_seq_no, what);          \
   } while (0)

   /* ⚠ IS THE PREVIOUS SUBMISSION STILL RUNNING? Asked here, on the way IN, because this is the last moment this
    * process reliably gets: the hang has already happened, Sony's driver is already counting, and the kill comes
    * about a second later.
    *
    * The label lags a submission or two in normal operation - measured, and read as a defect once already - so a
    * single lagging read means nothing. What means something is a label that does not move at all while
    * submissions keep arriving. Four consecutive submits with an unchanged label is far outside the normal lag and
    * far inside the second the kernel gives us.
    *
    * Then `label + 1` names the submission that never wrote its end-of-pipe, the ring holds its bytes, and the
    * dump is everything four runs of guessing from PM4 headers could not produce: that draw's own registers. */
   if (orbis_dump_hung_enabled() && queued > 2) {
      static uint32_t last_label;
      static unsigned stall;
      static bool     reported;
      const uint32_t  label = *orbis_fence_label;

      if (label == last_label)
         ++stall;
      else
         stall = 0;
      last_label = label;

      if (stall >= 4 && !reported) {
         const uint64_t              hung = (uint64_t)label + 1;
         struct orbis_hung_slot *const hs = &orbis_hung_ring[hung % ORBIS_HUNG_RING];
         reported = true;
         mesa_logw("orbis-drm: THE GPU HAS NOT FINISHED submit #%" PRIu64 " - the fence label has been stuck at %u "
                   "for %u submissions while %" PRIu64 " were queued. Dumping the stream it is stuck in.",
                   hung, label, stall, queued + 1 - hung);
         if (hs->seq == hung && hs->dwords > 0) {
            orbis_dump_ib(hung, hs->stream, hs->dwords, "HUNG", true);

            /* ⚠ EVERY ADDRESS THE CP WAS TOLD TO READ, CHECKED AGAINST WHAT THIS PROCESS ACTUALLY MAPPED.
             *
             * A hung stream of 157000 dwords narrows to nothing by eye, and the answer that mattered turned
             * out to be one the dump alone cannot give: "inside the arena" is not "mapped". Since RADV's
             * window starts inaccessible and a range gains access only when a BO is mapped into it, an
             * address can be perfectly in-range and still be a page the GPU faults on - which is exactly the
             * failure that leaves a submission unfinished with nothing else wrong.
             *
             * Offline analysis of the last dump got this far and stopped here: all 7096 SET_BASE addresses
             * and all 4648 DMA_DATA addresses were inside the arena, and the log carried no record of the
             * live mappings to check them against. This is that record, and the check, in the same run. */
            orbis_audit_hung_addresses(hung, hs->stream, hs->dwords);

            /* ⚠ AND WHERE THE COMMAND PROCESSOR ACTUALLY STOPPED, when ORBIS_PROGRESS_MARKS armed the
             * markers. The dump above is the same stream, so the offset names the packet directly: count
             * dwords into the HUNG dump and read what is there. Zero means the CP never reached the first
             * marker, which is itself an answer - it stopped in the first few thousand dwords. */
            if (orbis_progress_marks() != 0 && orbis_progress_word != NULL)
               mesa_logw("orbis-drm: submit #%" PRIu64 " the COMMAND PROCESSOR reached dword %u of %u (%u%%) "
                         "before it stopped; EXECUTION retired through dword %u. The CP runs ahead, so the "
                         "work that never finished is between those two.",
                         hung, *orbis_progress_word, hs->dwords,
                         hs->dwords ? (unsigned)((uint64_t)*orbis_progress_word * 100 / hs->dwords) : 0,
                         orbis_retired_word != NULL ? *orbis_retired_word : 0);

            /* ⚠ AND THE SHADER'S OWN INSTRUCTIONS, because "a compute dispatch hangs" is not yet an identification.
             *
             * The first hung stream was 184 dwords with no draw in it: one IT_DISPATCH_DIRECT of 7x1x1 groups, 64
             * threads with a 21-thread partial group, and a CP DMA prefetching 96 bytes - so the shader is about
             * two dozen instructions. Which two dozen decides everything, and nothing outside the GPU knows: RADV
             * compiles a hundred meta shaders and this address is in the first arena, so it is one of THOSE rather
             * than one of the game's.
             *
             * COMPUTE_PGM_LO is SET_SH_REG offset 0x20C and holds the address shifted right by 8, so the stream
             * carries it and the arena is CPU-mapped - the ISA is simply there to be read. Scanned rather than
             * assumed: the packet's position moves with the stream, so this walks type-3 packets and takes the last
             * write to 0x20C, which is the one the dispatch used.
             *
             * 32 dwords, which is more than the 24 the prefetch implies and cheap to over-read inside our own
             * arena. */
            /* ⚠ AND THE ONE BEFORE IT, because a stream that reads clean is evidence about the WRONG submission.
             *
             * #6843's 184 dwords were decoded packet by packet and every one of them is right: both descriptor
             * addresses inside the arena, the size used, and a fifteen-instruction copy shader that cannot loop.
             * A GPU that faults on a correct dispatch was already damaged when it arrived - so the interesting
             * stream is the one that ran BEFORE, and the ring already holds it.
             *
             * label is the last submission that FINISHED, so it is the last one whose end-of-pipe wrote - the last
             * thing known to have completed, and the last chance to have left the GPU in a state the next dispatch
             * dies on. */
            {
               struct orbis_hung_slot *const prev = &orbis_hung_ring[label % ORBIS_HUNG_RING];
               if (label > 0 && prev->seq == (uint64_t)label && prev->dwords > 0) {
                  mesa_logi("orbis-drm: and submit #%u, the last one that FINISHED, follows - a correct dispatch "
                            "that faults was damaged before it arrived",
                            label);
                  orbis_dump_ib((uint64_t)label, prev->stream, prev->dwords, "PREV", true);
               }
            }

            /* ⚠ THE DISPATCH THE MARKER POINTS AT, NOT THE LAST ONE IN THE STREAM.
             *
             * The scan below used to take the final write to COMPUTE_PGM_LO anywhere in the submission,
             * which is a measurement when there is one dispatch and a guess when there are thirty-six. With
             * ORBIS_PROGRESS_MARKS armed, the progress word says which dword the command processor reached,
             * so the shader that matters is the one whose address was IN EFFECT at that point - and on the
             * run this was written for those two answers differed: 0x20a49cb00 from the old scan against
             * 0x20a4c0b00 from the marker, and only the second is where it stopped.
             *
             * Bounded to the progress offset when there is one, and to the whole stream when there is not,
             * so the knob changes the precision and never the behaviour. */
            const uint32_t scan_to =
               (orbis_progress_marks() != 0 && orbis_progress_word != NULL && *orbis_progress_word != 0 &&
                *orbis_progress_word < hs->dwords)
                  ? *orbis_progress_word + 64 /* the dispatch sits just past the marker */
                  : hs->dwords;

            /* ⚠ WHICH SHADER, AND THE FIRST VERSION ASKED ONLY ABOUT COMPUTE. That was right when the
             * hung stream held one dispatch and nothing else; it is wrong here. The brackets put the stop at
             * a DRAW_INDIRECT, and a draw runs the graphics stages - so a line naming COMPUTE_PGM_LO was
             * reporting a shader with no connection to the packet that hung, for four runs.
             *
             * All six program registers are tracked to the progress offset and whichever are live get
             * dumped. SH offsets, from the register addresses divided by four:
             *
             *   0x008  SPI_SHADER_PGM_LO_PS   0x00B020      0x048  SPI_SHADER_PGM_LO_VS   0x00B120
             *   0x088  SPI_SHADER_PGM_LO_GS   0x00B220      0x0C8  SPI_SHADER_PGM_LO_ES   0x00B320
             *   0x108  SPI_SHADER_PGM_LO_HS   0x00B420      0x20C  COMPUTE_PGM_LO         0x00B830
             *
             * Each holds the address shifted right by 8, and the arena is CPU-mapped, so the ISA is simply
             * there to be read. */
            struct { const char *name; uint32_t sh; uint64_t pgm; } stages[] = {
               {"PS", 0x008, 0}, {"VS", 0x048, 0}, {"GS", 0x088, 0},
               {"ES", 0x0C8, 0}, {"HS", 0x108, 0}, {"CS", 0x20C, 0},
            };

            for (uint32_t i = 0; i + 1 < scan_to && i + 1 < hs->dwords;) {
               const uint32_t hdr = hs->stream[i];
               if ((hdr >> 30) != 3)
                  break;
               const uint32_t cnt = ((hdr >> 16) & 0x3FFF) + 2;
               const uint32_t op = (hdr >> 8) & 0xFF;
               if (op == 0x76 && cnt >= 3 && i + cnt <= hs->dwords) {
                  const uint32_t base = hs->stream[i + 1];
                  for (uint32_t k = i + 2; k < i + cnt; ++k) {
                     const uint32_t reg = base + (k - i - 2);
                     for (unsigned st = 0; st < ARRAY_SIZE(stages); ++st)
                        if (reg == stages[st].sh)
                           stages[st].pgm = (uint64_t)hs->stream[k] << 8;
                  }
               }
               i += cnt;
            }

            /* ⚠ THE POINTERS THAT DISPATCH WAS HANDED, WITH THE ROOM BEHIND THEM.
             *
             * The brackets put the stop on one DISPATCH_DIRECT, and the same shader with the same initiator
             * and the same thread count completes six times earlier in the same stream at smaller grids. So
             * the shape is not the question; the arguments are. RADV's fill_buffer meta takes a base in user
             * data 2:3 and a length in user data 4, and its own descriptor carries num_records = -1, which
             * means the store is NOT clamped by the hardware - the length is the only bound there is.
             *
             * Printing the containing mapping's extent beside the length makes "the fill runs off the end of
             * its buffer" a fact or not a fact, in one line, instead of an inference from three numbers in
             * three different places. */
            {
               uint32_t ud[8] = {0};
               bool have_ud = false;
               for (uint32_t i = 0; i + 1 < scan_to && i + 1 < hs->dwords;) {
                  const uint32_t hdr = hs->stream[i];
                  if ((hdr >> 30) != 3)
                     break;
                  const uint32_t cnt = ((hdr >> 16) & 0x3FFF) + 2;
                  if (((hdr >> 8) & 0xFF) == 0x76 && cnt >= 3 && i + cnt <= hs->dwords) {
                     const uint32_t base = hs->stream[i + 1];
                     for (uint32_t k = i + 2; k < i + cnt; ++k) {
                        const uint32_t reg = base + (k - i - 2);
                        if (reg >= 0x240 && reg < 0x248) {
                           ud[reg - 0x240] = hs->stream[k];
                           have_ud = true;
                        }
                     }
                  }
                  i += cnt;
               }

               if (have_ud) {
                  const uint64_t bufva = (uint64_t)ud[2] | ((uint64_t)ud[3] << 32);
                  uint64_t lo = 0, hi = 0;
                  uint32_t bo = 0;
                  mesa_logi("orbis-drm: submit #%" PRIu64 " compute user data at the stop: "
                            "0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x",
                            hung, ud[0], ud[1], ud[2], ud[3], ud[4], ud[5]);
                  if (orbis_va_mapping_of(bufva, &lo, &hi, &bo)) {
                     mesa_logi("orbis-drm: submit #%" PRIu64 " its buffer 0x%" PRIx64 " + %u bytes lives in "
                               "BO %u's mapping 0x%" PRIx64 "..0x%" PRIx64 " - %s",
                               hung, bufva, ud[4], bo, lo, hi,
                               bufva + ud[4] <= hi ? "the write fits"
                                                   : "THE WRITE RUNS PAST THE END OF THE MAPPING");
                  } else if (bufva != 0) {
                     mesa_loge("orbis-drm: submit #%" PRIu64 " its buffer 0x%" PRIx64 " is in NO live mapping",
                               hung, bufva);
                  }
               }
            }

            unsigned dumped = 0;
            for (unsigned st = 0; st < ARRAY_SIZE(stages); ++st) {
               const uint64_t pgm = stages[st].pgm;
               if (pgm == 0)
                  continue;
               if (pgm < orbis_va_base || pgm + 512 > orbis_va_end) {
                  mesa_logw("orbis-drm: %s program address 0x%" PRIx64 " is OUTSIDE the arena [0x%" PRIx64
                            ", 0x%" PRIx64 ") - that alone would explain the hang",
                            stages[st].name, pgm, orbis_va_base, orbis_va_end);
                  continue;
               }
               mesa_logi("orbis-drm: submit #%" PRIu64 " was running %s at 0x%" PRIx64
                         " where it stopped - its first 128 dwords follow",
                         hung, stages[st].name, pgm);
               orbis_dump_ib(hung, (const uint32_t *)(uintptr_t)pgm, 128, stages[st].name, false);
               ++dumped;
            }
            if (dumped == 0)
               mesa_logi("orbis-drm: no shader program address was set before the stop");
         }
         else
            mesa_logw("orbis-drm: submit #%" PRIu64 " is no longer in the %d-slot ring (it holds #%" PRIu64
                      ") - raise ORBIS_HUNG_RING",
                      hung, ORBIS_HUNG_RING, hs->seq);
      }
   }

   /* ⚠ ONE SUBMIT AT A TIME, and the lock is not paranoia: the fence command buffer is a single shared
    * allocation, so two threads building it concurrently would each submit the other's sequence number. RADV
    * submits from whichever thread called vkQueueSubmit. */
   /* THE LABEL AT ENTRY, AND IT SPLITS THE REMAINING POSSIBILITIES IN TWO. Submits #1 and #2 return in 0 us and
    * #3 blocks inside sceGnmSubmitCommandBuffers forever - with the fence command buffer now a ring, so it is
    * not an overwrite. Either the GPU retired the first two, in which case Sony's submission queue has a depth
    * a flip is meant to drain and three outstanding submits exceed it; or the label is still 1, in which case
    * the GPU stalled on submit #2's stream and the ring filled behind it. One number decides which. */
   if (orbis_trace() && orbis_submit_seq_no < 8)
      mesa_logi("orbis-drm: submit #%" PRIu64 " entering, fence label reads %u", orbis_submit_seq_no,
                *orbis_fence_label);

   ORBIS_SUBMIT_STAGE("entered");
   simple_mtx_lock(&orbis_submit_lock);
   ORBIS_SUBMIT_STAGE("took the submit lock");

   /* +1 for the arm's own fence command buffer, which goes LAST so that its end-of-pipe event follows every
    * IB in the submission. */
   void *dcb[ORBIS_MAX_IBS_PER_SUBMIT + 1];
   uint32_t dcb_bytes[ORBIS_MAX_IBS_PER_SUBMIT + 1];
   unsigned n_dcb = 0;

   for (int i = 0; i < num_chunks; i++) {
      const struct drm_amdgpu_cs_chunk *c = &chunks[i];
      const void *data = (const void *)(uintptr_t)c->chunk_data;

      switch (c->chunk_id) {
      case AMDGPU_CHUNK_ID_IB: {
         const struct drm_amdgpu_cs_chunk_ib *ib = data;

         /* ONE DCB ENTRY PER IB CHUNK, and an empty command buffer already produces THREE of them - RADV's
          * preamble, the buffer itself and its flush postamble. Measured on both machines, which is why this
          * is an array rather than the single pair the Tempest fork's own submit path has ever passed. */
         if (ib->ip_type != AMDGPU_HW_IP_GFX) {
            /* This device reports one GFX ring and zero compute queues, so anything else is a bug upstream of
             * here rather than a case to handle. */
            mesa_loge("orbis-drm: IB for ip_type %u - only GFX exists here", ib->ip_type);
            simple_mtx_unlock(&orbis_submit_lock);
            { return orbis_submit_return(-EINVAL, orbis_submit_t0); }
         }
         if (n_dcb >= ORBIS_MAX_IBS_PER_SUBMIT) {
            mesa_loge("orbis-drm: more than %d IBs in one submit", ORBIS_MAX_IBS_PER_SUBMIT);
            simple_mtx_unlock(&orbis_submit_lock);
            { return orbis_submit_return(-EINVAL, orbis_submit_t0); }
         }
         /* Recorded rather than submitted: the concatenation happens below, into one buffer. */
         dcb[n_dcb] = (void *)(uintptr_t)ib->va_start;
         dcb_bytes[n_dcb] = ib->ib_bytes;
         ++n_dcb;
         break;
      }
      /* ⚠ THESE FOUR LINES RAN ON EVERY SUBMISSION, INSIDE THE SUBMIT LOCK.
       *
       * They are what the chunk decoding was written with, and they never came back out. At the 84
       * submissions a second this port measures, a submission carrying an IB, a FENCE, both SYNCOBJ
       * chunks and BO_HANDLES puts 200 to 350 lines a second through the log - and every one of them
       * is flushed to storage while orbis_submit_lock is held, so every other thread waiting to
       * submit pays for it. This same function already moved its budget report out from under the
       * lock for exactly that reason.
       *
       * Behind orbis_trace() with the rest of the file's diagnostics. Nothing here is a warning; it
       * is a transcript of what the caller passed, which matters while decoding and not afterwards. */
      case AMDGPU_CHUNK_ID_FENCE: {
         /* Where the end-of-pipe value lands: a BO handle and a byte offset, written by
          * ac_drm_cs_chunk_fence_info_to_data above. */
         const uint32_t *w = data;
         if (orbis_trace())
            mesa_logi("orbis-drm:   [%d] FENCE bo_handle %u offset %u", i, w[0], w[1]);
         break;
      }
      case AMDGPU_CHUNK_ID_SYNCOBJ_IN:
      case AMDGPU_CHUNK_ID_SYNCOBJ_OUT: {
         const uint32_t n = c->length_dw / (sizeof(struct drm_amdgpu_cs_chunk_sem) / 4);
         if (orbis_trace())
            mesa_logi("orbis-drm:   [%d] %s x%u", i, orbis_chunk_name(c->chunk_id), n);
         break;
      }
      case AMDGPU_CHUNK_ID_BO_HANDLES:
         /* IGNORED, and not because it is hard: everything mapped is resident here, so there is no
          * per-submit residency list to honour. research/01-submission.md. */
         if (orbis_trace())
            mesa_logi("orbis-drm:   [%d] BO_HANDLES %u dw - ignored, everything mapped is resident", i,
                      c->length_dw);
         break;
      default:
         /* A chunk this arm does not decode. Bounded rather than traced: an unknown chunk is worth
          * knowing about once even in a run with tracing off. */
         {
            static unsigned said;
            if (orbis_budget(&said, 4))
               mesa_logi("orbis-drm:   [%d] %s (%u) %u dw", i, orbis_chunk_name(c->chunk_id), c->chunk_id,
                         c->length_dw);
         }
         break;
      }
   }

   ORBIS_SUBMIT_STAGE("chunks decoded");

   /* ⚠ THE TICKET IS TAKEN HERE, UNDER THE LOCK AND PAST VALIDATION, AND IT USED TO BE TAKEN AT ENTRY.
    *
    * Two things were wrong with that, and this file states the invariant itself twenty lines below: "the fence
    * still has to move or every wait on this sequence number hangs".
    *
    * A ticket taken before the chunk loop is STRANDED by that loop's two refusals - an IB for a non-GFX ring,
    * and more IBs than the array holds.  Both return without ever writing the label, so the counter runs ahead
    * of the fence permanently, and the next ac_orbis_wait_gpu_idle waits for a sequence nothing will write and
    * reports a GPU that did not stall.  In a port diagnosed entirely by reading these logs, a fabricated stall
    * is worse than the error it follows.
    *
    * And the lock's own comment says why order matters: "two threads building it concurrently would each submit
    * the other's sequence number".  A ticket handed out before the lock can be handed out in one order and
    * submitted in another, which walks the label BACKWARDS - and the whole fence is a signed comparison against
    * that label.
    *
    * Everything that can still fail after this point gives the ticket back explicitly. */
   const uint64_t seq = ++orbis_submit_seq_no;

   /* ---------------- the WAITS, and one ring is what makes them cheap
    *
    * On amdgpu the kernel takes CHUNK_ID_SYNCOBJ_IN and holds the submission until those objects signal. There
    * is no such kernel here, and there is also only ONE in-order ring - which does most of the work for us:
    * a syncobj signalled by an EARLIER submission is already ordered, because the GPU cannot reach this
    * submission's packets before it has finished that one's. Nothing has to be emitted for those.
    *
    * ⚠ WHAT IS LEFT IS A CPU BLOCK, AND IT IS A REAL LIMITATION rather than a shortcut. A wait on a syncobj
    * that nothing has signalled and no submission owns cannot be expressed as a GPU-side wait yet - a real one
    * would be a WAIT_REG_MEM packet on the label - so the arm blocks here before submitting. That is correct
    * and it serialises the CPU against the GPU, which a WAIT_REG_MEM would not. Bounded, and loud, because a
    * wait that never ends looks exactly like a hung driver. */
   for (int i = 0; i < num_chunks; i++) {
      if (chunks[i].chunk_id != AMDGPU_CHUNK_ID_SYNCOBJ_IN)
         continue;
      const struct drm_amdgpu_cs_chunk_sem *sems = (const void *)(uintptr_t)chunks[i].chunk_data;
      const uint32_t n = chunks[i].length_dw / (sizeof(*sems) / 4);
      for (uint32_t j = 0; j < n; j++) {
         simple_mtx_lock(&orbis_sync_lock);
         struct orbis_sync *slot = orbis_sync_get(sems[j].handle);
         const bool known = slot != NULL;
         const bool done = known && orbis_sync_reached(slot);
         /* Owned by a submission that has already been handed to the GPU: in-order, so implicitly satisfied. */
         const bool ordered = known && slot->gpu_seq != 0 && slot->gpu_seq <= seq;
         simple_mtx_unlock(&orbis_sync_lock);

         if (!known) {
            mesa_logw("orbis-drm: SYNCOBJ_IN names unknown handle %u", sems[j].handle);
            continue;
         }
         if (done || ordered)
            continue;

         static unsigned said;
         if (orbis_budget(&said, 1)) {
            mesa_logi("orbis-drm: waiting on the CPU for a syncobj no submission owns - correct, and it "
                      "serialises this thread against the GPU");
         }
         uint32_t h = sems[j].handle;
         uint32_t first = 0;
         /* One second, absolute, through the provider's own wait so there is one implementation of "reached".
          * A timeout is reported and the submission proceeds: refusing it would turn a lost signal into a
          * permanently dead queue, and proceeding turns it into a visible ordering bug. */
         if (orbis_sync_wait(NULL, &h, 1, os_time_get_absolute_timeout(1000000000ull), 0x1, &first) != 0)
            mesa_logw("orbis-drm: SYNCOBJ_IN %u never signalled - submitting anyway, ordering is now wrong",
                      h);
      }
   }

   /* ⚠ THE STORE FENCE THE GARLIC EXPERIMENT NEEDS. On write-combined memory the CPU's writes sit in
    * the combining buffers until something drains them, and the command stream this is about to hand
    * the GPU was written by the CPU. Unconditional because it costs a handful of cycles once per
    * submission - 84 times a second here - and getting it wrong only shows up as corruption. */
   __builtin_ia32_sfence();

   ORBIS_SUBMIT_STAGE("waits resolved");

   if (n_dcb == 0) {
      /* Nothing to run, but the fence still has to move or every wait on this sequence number hangs. */
      *orbis_fence_label = (uint32_t)seq;
      simple_mtx_unlock(&orbis_submit_lock);
      *seq_no = seq;
      { return orbis_submit_return(0, orbis_submit_t0); }
   }

#if !defined(__PS4__)
   /* THE HOST ARM SUBMITS NOTHING, and says so on every call. There is no GPU behind this build; what it does
    * exercise - and the reason it is worth having - is the whole path down to here, the chunk decode, and
    * RADV's waiting behaviour once the label moves. Anything reading back GPU RESULTS on the host is reading
    * memory the CPU wrote. */
   mesa_logi("orbis-drm: SUBMIT #%" PRIu64 " ctx %u, %u IB chunk(s) - host arm, nothing is executed", seq,
             ctx_id, n_dcb);

   /* ⚠ THE DUMP BELONGS ON THIS ARM TOO, AND LEAVING IT OUT SENT A QUESTION TO THE CONSOLE THAT BELONGED HERE.
    *
    * A stream is built by platform-independent RADV code; this arm just never executes it. So every question of
    * the form "what does RADV emit for this" - which packets, which registers, whether an IB carries the CHAIN
    * bit - is answerable on the laptop, at no flash. The console is for questions about EXECUTION. Having the
    * dump only on the PS4 side blurred that line, and a flash was spent on the wrong side of it.
    *
    * Per chunk rather than one flat entry, because that is what this arm has: the PS4 side concatenates the
    * chunks into its own staging buffer, and here they are still RADV's. */
   {
      const char *const dump = getenv("ORBIS_DUMP_SUBMIT");
      const uint64_t    want = dump != NULL ? strtoull(dump, NULL, 10) : UINT64_MAX;
      if (dump != NULL && (want == 0 || want == seq)) {
         for (unsigned i = 0; i < n_dcb; i++) {
            char label[32];
            snprintf(label, sizeof(label), "chunk %u/%u", i, n_dcb);
            orbis_dump_ib(seq, (const uint32_t *)dcb[i], dcb_bytes[i] / 4, label, true);
         }

         /* ⚠ THE FLATTENED ENTRY, BUILT HERE TOO EVEN THOUGH NOTHING WILL RUN IT. This is the SAME
          * orbis_build_flat the console submits, so the laptop verifies the construction that took a whole
          * evening of flashes to get right: whether chains were found and followed, whether the packet walk
          * stayed in sync, and how many dwords came out. Questions about EXECUTION still need the console;
          * questions about what we BUILD do not, and forgetting that cost a flash. */
         static uint32_t host_flat[ORBIS_FENCE_CB_BYTES / 4];
         /* timed: the flatten is the one thing in this path nobody has measured */
         const uint64_t orbis_flat_t0 = os_time_get_nano();
         uint32_t *const end = orbis_build_flat(host_flat, ARRAY_SIZE(host_flat), dcb, dcb_bytes, n_dcb, seq, 32);

         /* ⚠ THE AUDITS RUN ON THE LAPTOP TOO, and until now they did not. Both of them - the released-range
          * check on user-data pointers and the released-base check inside descriptors - lived only in the
          * console arm, so the only way to see either of them fire was to flash. That is the exact shape
          * this port keeps paying for: a diagnostic whose silence cannot be distinguished from absence.
          *
          * They read the stream and the memory it points at, both of which the host arm has. */
         for (unsigned i = 0; i < n_dcb; i++)
            orbis_follow_sh_pointers(seq, (const uint32_t *)dcb[i], dcb_bytes[i] / 4);
         orbis_api_time(ORBIS_ID_ARM_BUILD_FLAT, os_time_get_nano() - orbis_flat_t0);
         orbis_api_count(ORBIS_ID_ARM_BUILD_FLAT, 1);
         if (end != NULL)
            orbis_dump_ib(seq, host_flat, (uint32_t)(end - host_flat), "flattened", true);
      }
   }

   *orbis_fence_label = (uint32_t)seq;
   simple_mtx_unlock(&orbis_submit_lock);
#else
   /* THE ARM'S OWN COMMAND BUFFER, LAST. Two EOP events, the first draining so the second is trustworthy. The
    * scratch target takes seq-1 and raises no interrupt; the label takes seq and does. */
   {
      const uint32_t sel_dummy = ORBIS_EOP_DST_SEL_MC | (0u << ORBIS_EOP_INT_SEL_SHIFT) |
                                 ORBIS_EOP_DATA_SEL_32BIT;
      const uint32_t sel = ORBIS_EOP_DST_SEL_MC | (ORBIS_EOP_INT_SEL << ORBIS_EOP_INT_SEL_SHIFT) |
                           ORBIS_EOP_DATA_SEL_32BIT;
      /* THE SLOT THIS SUBMISSION OWNS. Before writing it, make sure the submission that used it last is done -
       * otherwise this is the same overwrite-under-the-GPU bug, just 16 submits later instead of one. */
      const unsigned slot = (unsigned)((seq - 1) % ORBIS_FENCE_CB_SLOTS);
      if (seq > ORBIS_FENCE_CB_SLOTS) {
         const uint32_t owner = (uint32_t)(seq - ORBIS_FENCE_CB_SLOTS);
         const uint64_t slot_t0 = os_time_get_nano();
         /* ⚠ AND THIS WAIT GETS THE SAME DEADLINE AS EVERY OTHER ONE IN THIS FILE, which it did not.
          * It was the one unbounded spin left: a GPU that hangs on submission K stops the label, and
          * the submitter of K + ORBIS_FENCE_CB_SLOTS spins here FOREVER while holding
          * orbis_submit_lock - so every later vkQueueSubmit blocks on the mutex behind it and the
          * console has to be pulled from the wall. That is precisely the outcome ORBIS_WAIT_CAP_NS
          * exists to prevent, stated at the top of this file: "a hang tells you nothing; a timeout
          * tells you the GPU did not finish".
          *
          * The report names the numbers, because "the GPU hung" is not actionable and "the label
          * stopped at N while slot S was owed to N+16" is. */
         const int64_t slot_deadline = orbis_deadline(OS_TIMEOUT_INFINITE, "fence-slot");
         bool slot_free = true;
         while ((int32_t)(*orbis_fence_label - owner) < 0) {
            if (os_time_get_nano() >= (uint64_t)slot_deadline) {
               slot_free = false;
               break;
            }
            orbis_poll_pause();
         }
         orbis_slot_wait_ns += os_time_get_nano() - slot_t0;

         if (!slot_free) {
            mesa_loge("orbis-drm: fence slot %u is still owed to submission %u after %llu s - the label "
                      "is at %u and the newest submission handed out is %llu. The GPU has not retired "
                      "%u submission(s); this is a hang, and waiting longer would hold the submit lock "
                      "against every other thread.",
                      slot, owner, (unsigned long long)(ORBIS_WAIT_CAP_NS / 1000000000ull),
                      *orbis_fence_label, (unsigned long long)orbis_submit_seq_no,
                      (unsigned)(owner - *orbis_fence_label));
            /* The ticket goes back the same way the builder's failure returns it: nothing was
             * submitted, so nothing will ever write this sequence number, and the counter must not
             * be left ahead of the fence. Still under the lock, so no other thread took one. */
            --orbis_submit_seq_no;
            simple_mtx_unlock(&orbis_submit_lock);
            { return orbis_submit_return(-ETIME, orbis_submit_t0); }
         }
      }

      uint32_t *cb = orbis_fence_cb + slot * (ORBIS_FENCE_CB_BYTES / 4);

      /* RADV's chunks FLATTENED, then our end-of-pipe pair - so the fence is written after every packet of the
       * submission, which is what makes it a fence at all. 32 dwords held back for those two packets and the size
       * padding; the builder refuses rather than overrunning the slot. */
      /* timed: the flatten is the one thing in this path nobody has measured */
      const uint64_t orbis_flat_t0 = os_time_get_nano();
      uint32_t *dw = orbis_build_flat(cb, ORBIS_FENCE_CB_BYTES / 4, dcb, dcb_bytes, n_dcb, seq, 32);
      orbis_api_time(ORBIS_ID_ARM_BUILD_FLAT, os_time_get_nano() - orbis_flat_t0);
      orbis_api_count(ORBIS_ID_ARM_BUILD_FLAT, 1);
      if (dw == NULL) {
         /* Nothing was submitted, so nothing will ever write this sequence number. Give the ticket back rather
          * than leave the counter ahead of the fence for the rest of the process - still under the lock, so no
          * other thread can have taken one in between. */
         --orbis_submit_seq_no;
         simple_mtx_unlock(&orbis_submit_lock);
         { return orbis_submit_return(-ENOMEM, orbis_submit_t0); }
      }

      /* ⚠ TRUNCATION, AND IT IS A BISECTION TOOL RATHER THAN A FEATURE. ORBIS_TRUNCATE=<seq>:<dwords> cuts one
       * submission's stream short, then appends the fence as usual - so a submission the GPU refuses whole can be
       * asked which HALF it refuses.
       *
       * It exists because guessing stopped paying. Four hypotheses about one stalled submission died in a row:
       * the IB-chain limit, HTILE, a generated tile-table entry, and OpenGothic's fog-LUT pass. Each cost a
       * console trip. A bisection costs the same per step and CANNOT be wrong, because the fence still reports:
       * if the truncated stream completes, everything removed is a candidate and everything kept is cleared.
       *
       * THE CUT MUST LAND ON A PACKET BOUNDARY. Nothing here can check that - a dword is a dword - so the caller
       * takes an offset read out of a dump, and the log states what was dropped so a wrong cut is visible as
       * nonsense rather than as a result. */
      /* ⚠ PUT THE HARDWARE IN A KNOWN STATE FIRST, WHICH THIS PORT HAS NEVER DONE.
       *
       * RADV does not initialise the GPU; it assumes somebody already has. On Linux that is amdgpu at
       * boot. Here it is Sony, and sceGnmDrawInitDefaultHardwareState is the block Sony's own API says a
       * command buffer begins with. Dumped from the console it is 256 dwords and 39 registers, and among
       * them is GRBM_GFX_INDEX = 0xe0000000 - full broadcast across shader engines, shader arrays and
       * instances.
       *
       * That last one is the reason this is worth a flash rather than a note. GRBM_GFX_INDEX decides WHICH
       * shader engines subsequent register writes reach. RADV touches it in exactly one place,
       * ac_write_harvested_raster_configs, and otherwise assumes broadcast because under amdgpu it always
       * is. If Sony leaves it aimed at one engine, every register this driver writes lands on half the
       * hardware and the other half keeps whatever was there - which is the shape of a fault that follows
       * our sizes for one kind of traffic and ignores them for another.
       *
       * Prepended to the flattened stream, before RADV's own preamble, so RADV's values win wherever the
       * two disagree. Once per submission rather than once per boot: the whole point is that the state
       * does not survive between them.
       *
       * ORBIS_INIT_HW_STATE=1. Off by default - it is 256 dwords of somebody else's register values and
       * shipping it on a hunch is what this file spends its comments warning against. */
      {
         static int init_state = -1;
         if (init_state < 0) {
            const char *const e = getenv("ORBIS_INIT_HW_STATE");
            init_state = (e != NULL && *e != '\0' && *e != '0');
         }
         if (init_state) {
            static uint32_t sony_init[512];
            static uint32_t sony_init_dw;
            static bool     asked;

            if (!asked) {
               asked = true;
               sony_init_dw = ((uint32_t (*)(uint32_t *, uint32_t))sceGnmDrawInitDefaultHardwareState)(
                  sony_init, ARRAY_SIZE(sony_init));
               mesa_logi("orbis-drm: ORBIS_INIT_HW_STATE - Sony's default hardware state is %u dwords and "
                         "will be prepended to every submission. RADV's preamble follows it, so RADV wins "
                         "wherever the two set the same register.",
                         sony_init_dw);
            }

            if (sony_init_dw > 0 && sony_init_dw <= ARRAY_SIZE(sony_init) &&
                (uint32_t)(dw - cb) + sony_init_dw + 64 < ORBIS_FENCE_CB_BYTES / 4) {
               /* Room is checked against the whole buffer rather than assumed: this runs before the fence
                * packets are appended and shifting the stream by 256 dwords must not be what overflows it. */
               memmove(cb + sony_init_dw, cb, (size_t)(dw - cb) * 4);
               memcpy(cb, sony_init, (size_t)sony_init_dw * 4);
               dw += sony_init_dw;
            }
         }
      }

      /* ⚠ BISECTING BY SIZE, AND SNAPPING TO A PACKET BOUNDARY, because the plain knob below has two traps
       * that would each waste console trips.
       *
       * ORBIS_TRUNCATE takes a sequence NUMBER, and the interesting submission does not have a stable one -
       * it has been 5874, 5909, 5924, 5957, 5963, 5965, 6000, 6065 across the runs of this hunt. A bisection
       * needs a dozen runs to agree on which submission they are cutting, so it takes a THRESHOLD instead,
       * exactly as ORBIS_DUMP_BIG does: the world frame is ~157000 dwords and everything else in a boot is a
       * few hundred.
       *
       * And its own comment says "THE CUT MUST LAND ON A PACKET BOUNDARY. Nothing here can check that" - so a
       * mis-cut produces a malformed stream and an answer about nothing. This walks the packets and cuts at
       * the last boundary at or before the request, then says where it actually cut. A bisection whose steps
       * silently mean something else is worse than no bisection.
       *
       *     ORBIS_TRUNCATE_BIG=<threshold>:<keep_dwords>
       */
      {
         const char *const big_trunc = getenv("ORBIS_TRUNCATE_BIG");
         static bool       big_done;

         if (big_trunc != NULL && !big_done) {
            char                    *colon = NULL;
            const unsigned long long threshold = strtoull(big_trunc, &colon, 10);
            const unsigned long      keep = (colon != NULL && *colon == ':') ? strtoul(colon + 1, NULL, 10) : 0;
            const uint32_t           have = (uint32_t)(dw - cb);

            if (threshold > 0 && have > threshold && keep > 0 && keep < have) {
               uint32_t boundary = 0, i = 0;

               while (i < have) {
                  const uint32_t hdr = cb[i];
                  uint32_t       words;

                  if ((hdr & 0xC0000000u) == 0x80000000u)
                     words = 1; /* type-2: one dword, no body */
                  else if ((hdr & 0xC0000000u) == 0xC0000000u)
                     words = 2 + ((hdr >> 16) & 0x3FFFu);
                  else
                     break; /* not a header we can walk - stop where the walk stops */

                  if (i + words > keep)
                     break;
                  i += words;
                  boundary = i;
               }

               big_done = true;
               if (boundary > 0) {
                  mesa_logw("orbis-drm: submit #%" PRIu64 " is the first over %llu dwords - TRUNCATED to %u of "
                            "%u dwords (asked for %lu; snapped back to the packet boundary), %u dropped. The "
                            "fence is still appended, so if this completes, everything dropped is a suspect "
                            "and everything kept is cleared.",
                            seq, threshold, boundary, have, keep, have - boundary);
                  dw = cb + boundary;
               } else {
                  mesa_loge("orbis-drm: ORBIS_TRUNCATE_BIG found no packet boundary at or before %lu dwords - "
                            "nothing truncated, and this run measures the untruncated frame",
                            keep);
               }
            }
         }
      }

      {
         const char *const trunc = getenv("ORBIS_TRUNCATE");
         if (trunc != NULL) {
            char    *colon = NULL;
            const unsigned long long want_seq = strtoull(trunc, &colon, 10);
            const unsigned long      keep = (colon != NULL && *colon == ':') ? strtoul(colon + 1, NULL, 10) : 0;
            if (want_seq == seq && keep > 0 && keep < (unsigned long)(dw - cb)) {
               mesa_logw("orbis-drm: submit #%" PRIu64 ": TRUNCATED to %lu of %u dwords - %u dropped", seq, keep,
                         (unsigned)(dw - cb), (unsigned)((dw - cb) - keep));
               dw = cb + keep;
            }
         }
      }

      /* ⚠ THE CHAIN BIT, AND THIS ARM'S CONSTRUCTION IS WRONG WITHOUT DEALING WITH IT.
       *
       * Our fence goes at the END of the concatenated chunks. An IT_INDIRECT_BUFFER with CHAIN (bit 20 of its
       * size dword) transfers control and does NOT return, so anything after it - including that fence - is
       * unreachable. The kernel's amdgpu path does not have this problem because it appends its fence to the
       * LAST IB of the chain rather than after the first.
       *
       * MEASURED, both sides: RADV's submissions for ps4/radv carry their command stream INLINE in a chunk and
       * contain no chained IB (host arm, infoprobe: `chunk 1/3: 40 dwords` with no IB2 in it), and ten of them in
       * a row execute on the console. Tempest's submissions reference the command buffer through
       * `IB2 ... 304 dwords, chain=1`, and the second one never executes. Chained IBs appear exactly in the
       * failing case and in no working one.
       *
       * ⚠ AND IT DOES NOT EXPLAIN EVERYTHING, which is why this is an experiment and not a fix. Tempest's FIRST
       * submission has the same chained IB and its fence demonstrably ran (label 0 -> 1). So either the CP
       * returns from a chain entered at IB2 level - in which case this changes nothing and the cause is
       * elsewhere - or that first submission was luck. Clearing the bit makes the jump a call that returns, so
       * the fence is reached deterministically either way, which is the correct construction for a stream we
       * append to regardless of what the measurement says.
       *
       * Env-gated so the run that tests it has ONE variable, and so ps4/radv stays an untouched control. */

      /* ⚠ sel_dummy ALONE. This call site used to OR ORBIS_EV_CACHE_FLUSH_AND_INV_TS in as well, and the hex
       * dump of the stream is what caught it: the selector dword read 20000016 where it should read 20000002.
       * 0x14 is the event type, which orbis_emit_eop already puts in dword 1 - the fork's helper takes the
       * event as a separate argument (eventWriteEop(va, value, event, sel)) and I collapsed four parameters
       * into three while still passing four values.
       *
       * The consequence was not a wrong flag but a WILD ADDRESS: the selector dword carries the high 16 bits
       * of the target, so 0x14 | 0x2 made the dummy write to 0x16_00400040 instead of 0x2_00400040. Submit #1's
       * real EOP still landed - the label reached 1 - and the dummy's write then faulted the GPU, which is why
       * every submission after the first was accepted and never ran. Six hypotheses died on that symptom. */
      dw = orbis_emit_eop(dw, (uint64_t)(uintptr_t)orbis_fence_scratch, (uint32_t)seq - 1, sel_dummy);
      dw = orbis_emit_eop(dw, (uint64_t)(uintptr_t)orbis_fence_label, (uint32_t)seq, sel);

      /* PADDED TO THE SIZE ALIGNMENT WE OURSELVES REPORT. RADV pads its own IBs for the same reason
       * (ac_gpu_info's ib_pad_dw_mask), and this buffer is an IB like any other - being ours does not exempt
       * it. This call site is the one that had its own copy of the arithmetic and got the one-dword case
       * wrong; see orbis_pad_ib. */
      dw = orbis_pad_ib(dw, cb);

      /* ONE entry, replacing the list. */
      dcb[0] = cb;
      dcb_bytes[0] = (uint32_t)((dw - cb) * 4);
      n_dcb = 1;

      /* Into the ring, so that if THIS is the submission that hangs, the next one can print it. Copied rather
       * than referenced: the staging slot it lives in is reused ORBIS_FENCE_CB_SLOTS submissions later, and by the
       * time a hang is noticed the original would be somebody else's stream. */
      if (orbis_dump_hung_enabled()) {
         struct orbis_hung_slot *const hs = &orbis_hung_ring[seq % ORBIS_HUNG_RING];
         hs->dwords = (uint32_t)(dw - cb);
         memcpy(hs->stream, cb, (size_t)hs->dwords * 4);
         /* Last, so a reader can never see a size that does not match the sequence number. */
         hs->seq = seq;
      }
   }

   ORBIS_SUBMIT_STAGE("fence packet built, calling Sony");

   /* ⚠ THE STREAM ITSELF, ON DEMAND. ORBIS_DUMP_SUBMIT=<n> hexdumps submission n's whole flat DCB entry, and
    * n=0 dumps every one of them.
    *
    * This existed once as scratch code and was deleted, and deleting it was the mistake: a one-dword selector
    * error survived SIX hypotheses about queue depth, alignment and padding, and the hex dump found it in two
    * minutes. "Dump the bytes before the third hypothesis" is the rule this port keeps re-learning, so the
    * facility is now permanent and behind an environment variable rather than a patch someone has to remember
    * to re-apply.
    *
    * The dump is what the CP will actually read - AFTER the IBs were memcpy'd in and AFTER the end-of-pipe
    * packets were appended - so a stream that looks right here and still does not execute says the problem is
    * not in the bytes. */
   /* ⚠ WALK OUR OWN STREAM BEFORE THE CP DOES. Always on, because the fault it catches is the one the console
    * reports as GPU_FAULT_BAD_COMMAND and the console cannot say WHERE.
    *
    * Twice now the title has died with exception 0xa0d0c006 and a kernel PM4 tail that is almost identical
    * between runs:
    *
    *     0x4651b10f  c0003f00  c0008b00  c0001700  c0044700     (run P)
    *     0x465c3b30  c0003f00  c0008b00  c0001700  c0044700     (run N)
    *
    * `0xc0044700` is EXACTLY the header this arm writes for its end-of-pipe fence - ORBIS_PM4_TYPE3(0x47, 5) - so
    * the kernel is quoting our own packet. The three that follow are not ours: our stream ends with two EOPs and a
    * type-3 NOP pad. And `0xc0003f00` is an INDIRECT_BUFFER with a count of 0, which is malformed - a real one
    * carries three data dwords. Malformed headers repeating identically across runs is not corrupted memory; it is
    * the CP reading the same wrong PLACE.
    *
    * So the question is whether the stream we hand over is well formed to its last dword, and that is answerable
    * here rather than on the console: walk it as the CP would. Every type-3 header, every count, and the last
    * packet must end EXACTLY at the size we report. A packet that runs one dword past the end means the CP keeps
    * fetching into whatever follows - which is precisely the observed shape.
    *
    * CHEAP AND UNCONDITIONAL: one pass over a stream this arm just memcpy'd anyway, and it prints nothing when the
    * stream is right. A validator behind a knob is a validator that is off during the run that needed it. */
   {
      const uint32_t *const w = (const uint32_t *)dcb[0];
      const uint32_t        n = dcb_bytes[0] / 4;
      static unsigned       said;
      uint32_t              i = 0;

      while (i < n) {
         const uint32_t hdr = w[i];
         /* A type-2 packet is one dword with no body - the CP skips it. This arm emits exactly one, as the
          * one-dword tail pad, so the walk has to know the shape or it reports its own padding as garbage. */
         if ((hdr & 0xC0000000u) == 0x80000000u) {
            ++i;
            continue;
         }
         if ((hdr & 0xC0000000u) != 0xC0000000u) {
            if (said < 8) {
               ++said;
               mesa_loge("orbis-drm: OUR OWN STREAM IS MALFORMED - submit #%" PRIu64 " dword %u of %u is 0x%08x, "
                         "which is not a type-3 header. The CP will treat everything after it as garbage.",
                         seq, i, n, hdr);
            }
            break;
         }
         const uint32_t cnt = ((hdr >> 16) & 0x3FFFu) + 1;
         if (i + 1 + cnt > n) {
            if (said < 8) {
               ++said;
               mesa_loge("orbis-drm: OUR OWN STREAM RUNS PAST ITS END - submit #%" PRIu64 " packet at dword %u "
                         "(op 0x%02x, %u data dwords) needs %u dwords but the buffer is %u. The CP will fetch "
                         "past the buffer.",
                         seq, i, (hdr >> 8) & 0xFFu, cnt, i + 1 + cnt, n);
            }
            break;
         }
         i += 1 + cnt;
      }

      /* ⚠ THE LAST EIGHT DWORDS, ON DEMAND. `ORBIS_TAIL=<n>` prints them for every nth submission. When the CP
       * dies just after our fence, the bytes that follow the fence are the evidence, and a hexdump of a whole
       * 9000-dword frame is the wrong tool for eight dwords. */
      const char *const tail = getenv("ORBIS_TAIL");
      const uint64_t    every = tail != NULL ? strtoull(tail, NULL, 10) : 0;
      if (every != 0 && (seq % every) == 0 && n >= 8)
         mesa_logi("orbis-drm: submit #%" PRIu64 " tail (%u dw): %08x %08x %08x %08x %08x %08x %08x %08x", seq, n,
                   w[n - 8], w[n - 7], w[n - 6], w[n - 5], w[n - 4], w[n - 3], w[n - 2], w[n - 1]);
   }

   /* ⚠ CHECKED ON BOTH SIDES OF EXECUTION, AND ONE SIDE ALONE PROVED NOTHING.
    *
    * The first version of this watch reported "first seen before submit #5832" and I wrote that a transition between
    * two submissions means the GPU wrote it. That is wrong: the CPU also works between submissions - it records
    * command buffers and calls vkUpdateDescriptorSets - so "dirty by submit N" says only that it happened at some
    * point before N, by either processor. The instrument could not answer the question it was built for.
    *
    * With ORBIS_WATCH_STRICT it is checked twice: here, after all CPU work for this submission, and again after the
    * GPU has actually executed it. Clean before and dirty after is the GPU, with no room left for interpretation.
    * It costs a wait per submission, which is the price of an answer rather than another guess. */
   /* Anything the GPU has finished with can lose its pages now - the retire queue drains from here and from every
    * unmap, so it needs no thread of its own. */
   orbis_retire_ranges();

   /* ⚠ DOES THIS SUBMISSION POINT AT A BUFFER THAT IS ALREADY DEAD? Asked BEFORE the GPU runs it, which is the only
    * moment at which the answer is actionable.
    *
    * The remaining faults all have one shape: a buffer is destroyed early - BO 42 at submit #570, BO 43 at #571, both
    * heap 0x2 flags 0x85, 4 and 8 MiB, the shape of load-time staging - and something writes into its range THOUSANDS
    * of submissions later. The retire rule is satisfied many times over by then, so this is not work in flight: an
    * address captured long ago is still being used.
    *
    * Every arena-bounds check in this port is blind to it, because a dead address IS inside the arena. But this arm now
    * remembers what died where, and every user-data pointer a submission programs is in its own PM4 - so the two can be
    * put together here and the culprit named before it faults.
    *
    * SET_SH_REG only, and 32-bit pointers reconstructed the way the shader will: high half from the arena, low half
    * from the register. Bounded to 8 reports, because if it fires at all it will fire every frame. */
   if (getenv("ORBIS_CHECK_DEAD_PTRS") != NULL) {
      const uint32_t *const w = (const uint32_t *)dcb[0];
      const uint32_t        n = dcb_bytes[0] / 4;
      const uint64_t        hi = orbis_va_base & 0xffffffff00000000ull;
      static unsigned       said;

      for (uint32_t i = 0; i + 1 < n && said < 8;) {
         const uint32_t hdr = w[i];
         if ((hdr & 0xC0000000u) != 0xC0000000u)
            break;
         const uint32_t op = (hdr >> 8) & 0xFFu;
         const uint32_t cnt = ((hdr >> 16) & 0x3FFFu) + 1;

         if (op == 0x76 /* IT_SET_SH_REG */ && cnt >= 2) {
            const uint32_t reg = w[i + 1] & 0xFFFFu;
            for (uint32_t k = 2; k <= cnt && i + k < n && said < 8; ++k) {
               const uint64_t va = hi | (uint64_t)w[i + k];
               uint64_t       ds = 0, de = 0;
               const uint32_t prev = ac_orbis_previous_owner(va, &ds, &de);
               if (prev == 0 || ac_orbis_va_is_live(va, 4))
                  continue;
               ++said;
               mesa_loge("orbis-drm: SUBMISSION #%" PRIu64 " POINTS AT A DEAD BUFFER - SH 0x%03x + %u carries 0x%"
                         PRIx64 ", which was BO %u's (0x%" PRIx64 "..0x%" PRIx64 ") until it was unmapped. The shader "
                         "will read or write there.",
                         seq, reg, k - 2, va, prev, ds, de);
            }
         }
         i += 1 + cnt;
      }
   }

   orbis_watch_check(seq, "before");

   /* Before the bytes go to the GPU: what does MEMORY say. Nothing unless ORBIS_SCAN_VA is set. */
   orbis_scan_for_va(seq);

   {
         /* ⚠ DUMP BY SIZE, BECAUSE THE INTERESTING SUBMISSION HAS NO STABLE NUMBER. `ORBIS_DUMP_BIG=<dwords>` dumps the
    * FIRST submission whose flattened stream exceeds that many dwords, once per boot.
    *
    * A Gothic world frame is 155646 dwords and everything else in a boot is a few hundred, so "the big one" is a
    * precise description and its sequence number is not: it moved between every run of this hunt (5884, 5902).
    * That frame now takes a GPU page fault at VA 0x0000000ff0000000, read by the texture cache, and the only way
    * to find which descriptor carries that base is to read the stream that set it up. */
   /* ON ITS OWN GATE AND ON EVERY SUBMISSION. Hanging it off the hex dumper would have limited it to the two or
    * three submissions ORBIS_TRACE_SUBMITS prints, and the rectangle we are looking for may only be programmed by
    * one pass of one frame. It deduplicates internally, so a frame that programs the same scissor two hundred
    * times prints one line. */
   orbis_scan_scissors(seq, (const uint32_t *)dcb[0], dcb_bytes[0] / 4);

   {
      const char *const big = getenv("ORBIS_DUMP_BIG");
      static bool       done;
      const unsigned    threshold = big != NULL ? (unsigned)strtoul(big, NULL, 10) : 0;
      if (threshold > 0 && !done && dcb_bytes[0] / 4 > threshold) {
         done = true;
         mesa_logi("orbis-drm: submit #%" PRIu64 " is the first over %u dwords - dumping it whole", seq,
                   threshold);
         orbis_dump_ib(seq, (const uint32_t *)dcb[0], dcb_bytes[0] / 4, "BIG", true);
         orbis_follow_sh_pointers(seq, (const uint32_t *)dcb[0], dcb_bytes[0] / 4);
      }
   }

   /* ⚠ A RANGE, BECAUSE A FRAME IS NOT ONE SUBMISSION. `ORBIS_DUMP_SUBMIT=<n>[:<count>]` dumps count submissions
       * starting at n; n=0 still means every one of them.
       *
       * The reason is the current question: OpenGothic is RUNNING - the maintainer walked the character around by
       * ear - and the 3D scene is not drawn. In the steady state the driver sees one repeating 647-dword submission
       * per frame, which for a Gothic frame is nothing, so the thing to read is that frame's packets. A single
       * number cannot span it, and the sequence numbers shift between runs so a hand-picked pair is a guess. */
      const char *const dump = getenv("ORBIS_DUMP_SUBMIT");
      char             *colon = NULL;
      const uint64_t    want = dump != NULL ? strtoull(dump, &colon, 10) : UINT64_MAX;
      const uint64_t    count =
        (colon != NULL && *colon == ':') ? strtoull(colon + 1, NULL, 10) : 1;
      /* ⚠ BRACES, AND THEIR ABSENCE COST A RUN. Appending the follow call under a braceless `if` put it OUTSIDE
       * the condition, so it fired on all 5877 submissions of a boot instead of on the one dumped frame - 9.5 MiB
       * of log, and the 48-pointer cap silently dropping 118 pointers per submission. The run was still readable,
       * which is the dangerous part: a diagnostic that runs too often looks like one that works. */
      if (dump != NULL && (want == 0 || (seq >= want && seq < want + (count > 0 ? count : 1)))) {
         orbis_dump_ib(seq, (const uint32_t *)dcb[0], dcb_bytes[0] / 4, "stream", true);
         orbis_follow_sh_pointers(seq, (const uint32_t *)dcb[0], dcb_bytes[0] / 4);
      }
   }

   /* ⚠ FLUSH BEFORE SUBMITTING, and this is taken from the only code known to submit repeatedly on this
    * hardware. GnmDevice's own probe does exactly this - sceGnmFlushGarlic() immediately before
    * sceGnmSubmitCommandBuffers (gnmdevice.cpp:492) - and this arm did it never.
    *
    * The CPU writes the command stream and the CP reads it, which is precisely the shape that works once and
    * then stops: whatever path carries those bytes to memory has no reason to have completed by the time the
    * GPU looks. The arena is WB_ONION, so in theory it is coherent and this is unnecessary - and "in theory
    * coherent" is the assumption that has cost this port five defects today. */
   /* One shot, here rather than at arena setup, because this is the first point where the submission path is
    * known to work: whatever this function needs, the submission below needs too. */
   {
      /* WHICH SUBMISSION TO READ ON, AND IT IS NOT A DETAIL. Before the first one, the registers hold what
       * SONY left - which is the question for config space. After it, they hold what SONY's CLEAR_STATE left
       * plus what RADV's preamble wrote - which is the question for the nine registers RADV omits. Reading the
       * second question before the first submission would find Sony's untouched state and prove nothing about
       * what RADV runs against. ORBIS_READ_REGS_AT=2 is the smallest value that answers it. */
      static unsigned submits_seen;
      static bool     asked;
      ++submits_seen;
      unsigned read_at = 1;
      {
         /* ⚠ FIRE ON A FACT, NOT ON A COUNTER. ORBIS_READ_REGS_AT=tess waits until this driver has actually
          * programmed the tessellation registers, which happens lazily at the first frame that tessellates.
          *
          * A number cannot express that. The world frame has landed at submission 5874, 5909, 5957, 5965,
          * 6000 and 6017 across the runs of this hunt, and the point at which the title dies moves too - a
          * threshold of 6100 missed entirely, because that run stopped at 6062. Guessing the number is a
          * coin toss per flash, and the thing being asked about is "after we wrote them", which
          * orbis_roundtrip_base states exactly: it is zero until radv_emit_tess_factor_ring has run. */
         const char *const s = getenv("ORBIS_READ_REGS_AT");
         if (s != NULL && s[0] == 't') {
            /* Wait for the fact rather than for a number: zero until radv_emit_tess_factor_ring has run. */
            read_at = (orbis_roundtrip_base != 0) ? submits_seen : UINT_MAX;
         } else if (s != NULL && s[0] == 'g') {
            /* ⚠ THE SAME RULE FOR THE GS RINGS, and it is what makes the read-only rungs worth anything. Zero
             * until radv_emit_gs_ring_sizes has run, which happens the first time a pipeline with a geometry
             * shader forces the queue to rebuild its preamble. Before that the registers hold Sony's state and
             * "does our value land" is not even askable.
             *
             * ⚠ AND THE FACT ALONE IS NOT ENOUGH - run 49 fired at submission #1, because the preamble is
             * built before it is sent. Nothing had EXECUTED yet, so VGT_PRIMITIVE_TYPE could not act as the
             * block discriminator either. Waiting for a third submission means a draw has actually run. */
            read_at = (orbis_gs_ring_gsvs_shifted != 0 && submits_seen >= 3) ? submits_seen : UINT_MAX;
         } else if (s != NULL) {
            const unsigned v = (unsigned)strtoul(s, NULL, 0);
            if (v >= 1)
               read_at = v;
         }
      }
      if (!asked && submits_seen >= read_at) {
         asked = true;
         mesa_logi("orbis-drm: READ_REGS firing before submission #%u (ORBIS_READ_REGS_AT=%u)", submits_seen,
                   read_at);
         orbis_ask_gnm_tess_base();
         orbis_dump_sony_init_state();
         orbis_ask_sony_hs_layout();
         orbis_read_hw_registers();
         orbis_probe_config_writes();
         orbis_test_gs_ring_regs();
      }
   }

   sceGnmFlushGarlic();

   const uint64_t t0 = os_time_get_nano();
   const int32_t err = sceGnmSubmitCommandBuffers(n_dcb, dcb, dcb_bytes, NULL, NULL);
   const uint64_t t1 = os_time_get_nano();
   if (getenv("ORBIS_WATCH_STRICT") != NULL && err == 0) {
      /* Bounded, like every wait in this file: if the GPU does not get there, the check is skipped rather than the
       * process hanging, and the wait itself says so. */
      ac_orbis_wait_gpu_idle(2ull * 1000 * 1000 * 1000);
      orbis_watch_check(seq, "AFTER THE GPU EXECUTED");
   }

   /* ⚠ SERIALISE EVERYTHING, TO ASK WHETHER SYNCHRONISATION IS THE DEFECT AT ALL.
    *
    * The artefacts need MOTION - standing still produces none, and that has been true across every run of this
    * investigation. Standing still also means the data uploaded each frame is IDENTICAL to the last frame's, so a
    * read-before-write or a buffer recycled too early produces the CORRECT result anyway. Motion changes the data
    * and the same race becomes visible. Nothing else tried so far explains the motion dependence that cleanly.
    *
    * And this is the layer where the port is not stock Mesa. Under amdgpu the KERNEL serialises submissions on a
    * queue and signals the fences RADV hands the application; here sceGnmSubmitCommandBuffers and this file's own
    * fence code do it. If a fence signals before the GPU is finished, the application recycles a buffer the GPU is
    * still reading - which is exactly "a quad misrenders, intermittently, only when the contents change".
    *
    * ⚠ WHAT THE EARLIER ELIMINATIONS DID NOT COVER: RADV_DEBUG=syncshaders is about ordering INSIDE the GPU, and
    * the present path's wait is about the END of a frame. Neither touches CPU writes against GPU reads within a
    * frame, or a buffer reused between frames.
    *
    * ORBIS_SERIALISE=1 waits for the GPU to go idle after EVERY submission, so nothing can be in flight while the
    * CPU prepares the next thing. It destroys the frame rate on purpose - that is not a cost here, because the
    * question is a yes or no:
    *
    *   the artefacts GO      the defect is synchronisation, in this arm's fences or in the submit path, and the
    *                         search moves off configuration entirely
    *   the artefacts STAY    synchronisation is eliminated AS A CLASS, which is worth as much: nothing that can be
    *                         fixed by waiting is responsible
    *
    * ⚠ AND IT IS NOT A FIX EITHER WAY. Serialising is a diagnosis; the repair would be to make the fences correct,
    * which is a different piece of work in a known place. */
   if (getenv("ORBIS_SERIALISE") != NULL && err == 0) {
      static unsigned said;
      if (orbis_budget(&said, 1)) {
         mesa_logi("orbis-drm: ORBIS_SERIALISE - waiting for GPU idle after EVERY submission. The frame rate will "
                   "collapse; that is the point. Artefacts going means synchronisation, staying means it is not.");
      }
      if (!ac_orbis_wait_gpu_idle(2ull * 1000 * 1000 * 1000))
         mesa_logw("orbis-drm: SERIALISE submit #%" PRIu64 " did not go idle in 2 s - the wait gave up, so this "
                   "submission was NOT serialised and the run is compromised from here on", seq);
   }
   if (err != 0) {
      mesa_loge("orbis-drm: sceGnmSubmitCommandBuffers(%u) failed: 0x%08x", n_dcb, err);
      /* The GPU never received this stream, so its end-of-pipe will never fire. Same reasoning as the builder's
       * refusal above: hand the ticket back so the fence label and the counter stay in step. */
      --orbis_submit_seq_no;
      simple_mtx_unlock(&orbis_submit_lock);
      { return orbis_submit_return(-EIO, orbis_submit_t0); }
   }

   /* ⚠ sceGnmSubmitDone IS A PER-FRAME CALL, AND THIS PORT HAS NO FRAMES. Sony's own header says what it costs:
    * "Checks if any draw or compute submission will stall sceGnmSubmitDone(). Returns 1 if it won't stall"
    * (GnmDriver.h:14-16) - a predictor exists because the call blocks. The Tempest fork calls it after every
    * submit and that is correct there, because its submit IS a frame and ends in a flip.
    *
    * Here three submits with no flip measured 0 ms, 557 ms and then no return at all. So it is called ONLY when
    * the driver says it will not stall, and skipping it is logged. The failure mode that buys: if unprocessed
    * work then never completes, the fence poll reports a bounded timeout instead of the process hanging - a
    * diagnosable answer rather than a power-cycle. Which of the two is actually needed is what this measures. */
   /* ⚠ NOT CALLED, AND THAT IS THE EXPERIMENT THIS BUILD EXISTS FOR. Five explanations for "the first submit
    * executes and the second never does" have been measured and killed: submitDone stalling, a shared fence
    * buffer, its alignment, its padding, and a multi-entry DCB. Every one was a real bug except the last, and
    * none changed the result.
    *
    * What was never tested is NOT CALLING sceGnmSubmitDone. Sony's own description is "tell the driver that all
    * command buffers were submitted FOR THIS FRAME" - a frame boundary, not a submit boundary. If it closes a
    * frame that only a flip can retire, then the first frame executes, everything after it queues, and the
    * third submit blocks on a full queue. That is exactly the observed shape, and it is the one hypothesis
    * consistent with the first submit always working.
    *
    * The failure mode if this is wrong is bounded and readable: nothing executes at all, so the harness's first
    * vkWaitForFences reports a one-second timeout instead of the process hanging. */
   const int32_t allowed = sceGnmAreSubmitsAllowed();
   const int32_t done = 0;
   const bool skipped = true;
   const uint64_t t2 = os_time_get_nano();

   /* The first few, with timings, because "it hung somewhere in the submit" was all the last run could say. Bound
    * shared with the stage markers, so ORBIS_TRACE_SUBMITS moves both: a submit that stops mid-flight must be able
    * to say whether Sony's call came back, and this is the line that says it. */
   /* ⚠ SAMPLED MID-RUN, BECAUSE THE FIRST EIGHT SUBMISSIONS OF A BOOT ARE NOT WHERE THE TIME GOES.
    *
    * A world load measured 7074 submissions in 242 s - 29 per second, about 34 ms each - while the loading screen
    * is one quad. Three things in this path can account for that: the Sony call itself, the fence-slot throttle
    * that waits for the GPU every ORBIS_FENCE_CB_SLOTS submissions, and the scan-out copy's wait for GPU idle. This
    * line reports all three every 512th submission, which is a dozen lines per load rather than a log nobody reads.
    *
    * Cumulative rather than per-submission for the wait, because it happens on the WSI thread and not here - a
    * total and a count are what divide into an average. */
   if (orbis_trace() && (seq % 512) == 0) {
      mesa_logi("orbis-drm: submit #%" PRIu64 " sample: %u dwords, Sony %" PRIu64 " us, slot-throttle %" PRIu64
                " us, label lag %d; present idle-waits %" PRIu64 " totalling %" PRIu64 " ms",
                seq, dcb_bytes[0] / 4, (t1 - t0) / 1000, orbis_slot_wait_ns / 1000,
                (int)(seq - *orbis_fence_label), orbis_idle_waits, orbis_idle_wait_ns / 1000000);
      orbis_slot_wait_ns = 0;
   }

   if (orbis_trace() && (stage_limit == 0 || seq <= stage_limit)) {
      mesa_logi("orbis-drm: submit #%" PRIu64 ": %u DCB entry, %u bytes, submit %" PRIu64
                " us, submits_allowed %d, submitDone %s%" PRIu64 " us",
                seq, n_dcb, dcb_bytes[0], (t1 - t0) / 1000, allowed, skipped ? "NOT CALLED, " : "",
                (t2 - t1) / 1000);
      /* ⚠ NO WAITING HERE ANY MORE. This used to poll the label for 10 ms so each submit could say whether the
       * GPU had taken it - which is exactly what found the wild-address EOP, and which also SERIALISES every
       * submission against the GPU. That is diagnosis cost, not driver behaviour, so it is gone rather than
       * gated: the label is reported as it stands, and anything that needs to know waits through
       * ac_drm_cs_query_fence_status like RADV does. */
      /* ⚠ THE LABEL HERE IS READ MICROSECONDS AFTER THE SUBMIT AND USUALLY LAGS, which is not a failure and
       * read like one: the 1080p draw reported "label reads 5" for submission 6 while the GPU went on to
       * render it correctly and put the picture on the television. Submissions 1-5 were small enough to finish
       * inside the gap and looked consistent, which made the lag look like a defect the moment the work got
       * bigger. It is a snapshot, and it says so now. */
      mesa_logi("orbis-drm: submit #%" PRIu64 " returned, label was %u at that instant (it lags - the wait is "
                "what settles it)",
                seq, *orbis_fence_label);
   }

   simple_mtx_unlock(&orbis_submit_lock);
#endif

   /* ⚠ THE FRAME BUDGET: HOW MUCH CPU DOES THIS PROCESS ACTUALLY BURN?
    *
    * 8 fps is 125 ms a frame and this port has never attributed 85% of it to anything. The first
    * question is whether the CPU is working or waiting, and getrusage answers it for EVERY kind of
    * blocking - poll loops, mutexes, the WSI flip throttle, anything added later - because ru_utime +
    * ru_stime is CPU time by definition. Measured on this console: 27 us across a 20 ms sleep, 20016 us
    * across a 20 ms spin.
    *
    * ⚠ REPORTED AS CORES, NOT AS "BLOCKED", AND THAT MATTERS. RUSAGE_SELF sums CPU across ALL THREADS
    * while the wall clock is one timeline, so `wall - cpu` is not "time spent blocked" - with two busy
    * threads it is NEGATIVE. The honest quantity is cpu/wall: the average number of cores this process
    * kept busy. A previous attempt at this budget did exactly that subtraction across threads and a
    * review caught it; the arithmetic is the finding, so it is stated in the units it is true in.
    *
    * HOW TO READ IT. Well under 1.0 core at 8 fps means the CPU is idle and the GPU is the bottleneck.
    * Around or above 1.0 means a thread is saturated and the CPU is - the title, RADV, or this file.
    *
    * A WINDOW, NOT A TOTAL. Each line covers the interval since the previous one, so a change in the
    * middle of a session is visible; a since-boot average buries it and converges by construction.
    *
    * COST: one clock read per submission on the fast path, and one getrusage every five seconds. Not
    * under the trace gate, because profiling runs turn the trace off. Outside the submit lock, because
    * mesa_logi flushes to console storage and doing that inside the lock would stall every other
    * submitting thread - inside the very window being measured. Both arms, so the laptop can check it. */
   {
      static simple_mtx_t budget_lock = SIMPLE_MTX_INITIALIZER;
      static uint64_t next_ns, win_ns, win_cpu_us, win_seq, win_pw_ns, win_pw_n;
      static uint64_t win_passes, win_px, win_disp, win_groups, win_sub_ns;
      static uint64_t win_rec_ns, win_draws, win_dbinds, win_spans;
      static uint64_t win_api_ns[ORBIS_API_SLOTS], win_api_calls[ORBIS_API_SLOTS];
#if defined(__PS4__)
      static uint64_t win_kc[ORBIS_KC_SLOTS];
      static uint64_t win_ifree;
      static unsigned long long win_fw, win_ftw, win_fto, win_flk, win_cm, win_cr;
#endif

      const uint64_t now = os_time_get_nano();

      if (now >= next_ns) {
         simple_mtx_lock(&budget_lock);
         if (now >= next_ns) {
            struct rusage ru;
            const bool ok = getrusage(RUSAGE_SELF, &ru) == 0;
            const uint64_t cpu_us = ok ? (uint64_t)ru.ru_utime.tv_sec * 1000000ull + ru.ru_utime.tv_usec +
                                            (uint64_t)ru.ru_stime.tv_sec * 1000000ull + ru.ru_stime.tv_usec
                                       : 0;

            if (win_ns != 0 && ok) {
               const uint64_t wall_us = (now - win_ns) / 1000;
               const uint64_t used_us = cpu_us - win_cpu_us;
               const uint64_t pw_us = (orbis_present_wait_ns - win_pw_ns) / 1000;
               const uint64_t pw_n = orbis_present_waits - win_pw_n;
               const uint64_t np = orbis_present_waits - win_pw_n;
               const uint64_t passes = orbis_gfx_passes - win_passes;
               const uint64_t px = orbis_gfx_pass_pixels - win_px;
               const uint64_t disp = orbis_dispatches - win_disp;
               const uint64_t groups = orbis_dispatch_groups - win_groups;
               const uint64_t sub_us = (orbis_submit_cpu_ns - win_sub_ns) / 1000;
               const uint64_t rec_us = (orbis_record_ns - win_rec_ns) / 1000;
               const uint64_t draws = orbis_draws - win_draws;
               const uint64_t dbinds = orbis_desc_binds - win_dbinds;
               const uint64_t spans = orbis_record_spans - win_spans;

               mesa_logi("orbis-drm: BUDGET over %" PRIu64 " ms and %" PRIu64 " submissions: this process "
                         "burned %" PRIu64 " ms of CPU, i.e. %" PRIu64 ".%02" PRIu64 " cores busy on "
                         "average, of which %" PRIu64 " ms was inside this driver's submit path; and it "
                         "waited %" PRIu64 " ms for the GPU across %" PRIu64
                         " presents, which is %" PRIu64 "%% of the window.",
                         wall_us / 1000, seq - win_seq, used_us / 1000,
                         wall_us ? used_us / wall_us : 0,
                         wall_us ? (used_us * 100 / wall_us) % 100 : 0, sub_us / 1000,
                         pw_us / 1000, pw_n, wall_us ? pw_us * 100 / wall_us : 0);

               /* ⚠ SCREENFULS, which is the unit that makes 103 ms of GPU either reasonable or absurd.
                * A 1920x1080 pass is one screenful; the count is how many times a frame every pixel is
                * shaded, and it is the difference between "this title draws too much" and "the shaders
                * are wrong". */
               mesa_logi("orbis-drm: and per frame: %" PRIu64 " render passes covering %" PRIu64
                         ".%02" PRIu64 " screenfuls of 1920x1080, plus %" PRIu64 " dispatches of %" PRIu64
                         " workgroups",
                         np ? passes / np : 0, np ? px / np / 2073600 : 0,
                         np ? (px / np * 100 / 2073600) % 100 : 0, np ? disp / np : 0,
                         np ? groups / np : 0);

               /* ⚠ THE RECORDING SPAN IS AN UPPER BOUND ON OUR SHARE, the submit path a lower one.
                * Between them sits everything RADV does per draw; outside them sits the title. If the
                * span is a small part of the frame the CPU is going somewhere neither counter sees. */
               mesa_logi("orbis-drm: and recording: %" PRIu64 " draws and %" PRIu64 " descriptor-set binds "
                         "a frame, inside %" PRIu64 " command buffers whose Begin-to-End spans total %" PRIu64
                         " us a frame - an upper bound on this driver's share, against %" PRIu64
                         " us of submit which is a lower one",
                         np ? draws / np : 0, np ? dbinds / np : 0, np ? spans / np : 0,
                         np ? rec_us / np : 0, np ? sub_us / np : 0);

               /* Microseconds per frame inside each entry point, and how many calls. Everything the
                * title spends OUTSIDE these is its own. */
               {
                  static const char *const names[ORBIS_API_SLOTS] = {
                     [ORBIS_API_SUBMIT] = "QueueSubmit2",
                     [ORBIS_API_PRESENT] = "QueuePresentKHR",
                     [ORBIS_API_ACQUIRE] = "AcquireNextImage2",
                     [ORBIS_API_WAIT_FENCE] = "WaitForFences",
                     [ORBIS_API_DESC_UPDATE] = "UpdateDescriptorSets",
                     [ORBIS_API_DESC_ALLOC] = "AllocateDescriptorSets",
                     [ORBIS_API_MEM_ALLOC] = "AllocateMemory",
                     [ORBIS_API_FENCE_STATUS] = "GetFenceStatus",
                     [ORBIS_APP_TICK] = "app:world tick",
                     [ORBIS_APP_ANIM] = "app:animation",
                     [ORBIS_APP_CAMERA] = "app:camera",
                     [ORBIS_APP_UI] = "app:ui",
                     [ORBIS_APP_DRAW] = "app:Renderer::draw",
                     [ORBIS_APP_PRESENT] = "app:submit+present",
                  };
                  char line[1024];
                  int n = 0;
                  uint64_t total_us = 0;

                  for (unsigned i = 0; i < ORBIS_API_SLOTS && n < (int)sizeof(line) - 1; i++) {
                     const uint64_t us = (orbis_api_slot_ns[i] - win_api_ns[i]) / 1000;
                     const uint64_t calls = orbis_api_slot_calls[i] - win_api_calls[i];

                     /* The gap between the driver's slots and the application's is real, and a slot
                      * nothing ever called says nothing worth a line's width. */
                     if (names[i] == NULL || calls == 0)
                        continue;

                     total_us += us;
                     n += snprintf(line + n, sizeof(line) - n, "%s%s %" PRIu64 " us in %" PRIu64 " calls",
                                   n ? ", " : "", names[i], np ? us / np : 0, np ? calls / np : 0);
                  }

                  mesa_logi("orbis-drm: and per frame inside the Vulkan API: %s - %" PRIu64
                            " us a frame in all of them, against a frame of %" PRIu64 " us",
                            line, np ? total_us / np : 0, np ? wall_us / np : 0);
               }

               /* ⚠ AND THE ARENA, EVERY WINDOW, BECAUSE ONE SAMPLE AT THE MOMENT OF DEATH IS NOT A
                * DIAGNOSIS. swanstation ran for three and a half minutes and then failed to allocate
                * 2 MiB; whether the live bytes had been climbing the whole time (a leak), or had been
                * flat while the largest gap collapsed (fragmentation), or had been flat and high from
                * the start (a window that is simply too small) cannot be read off the failure alone.
                * A line every five seconds turns the whole run into the trend that answers it, at the
                * cost of one walk of a few hundred list entries. */
               orbis_va_report("now");

#if defined(__PS4__)
               /* ⚠ THE ATTRIBUTION LINE. Bytes of libkernel internal memory lost this window,
                * divided by the same present count everything else on these lines is divided by,
                * beside the number of each kernel call that could have consumed them. A leak that
                * divides evenly by one of these counts names its own call site; one that divides by
                * none of them says the consumer is inside libkernel or inside a facility this file
                * does not make, and the next step is the frontend's gated
                * sceKernelInternalHeapPrintBacktraceWithModuleInfo rather than more counting here. */
               {
                  const uint64_t ifree = orbis_internal_memory_free();
                  char kline[512];
                  int kn = 0;

                  for (unsigned i = 0; i < ORBIS_KC_SLOTS && kn < (int)sizeof(kline) - 1; i++) {
                     const uint64_t calls = orbis_kc[i] - win_kc[i];
                     if (calls == 0)
                        continue;
                     kn += snprintf(kline + kn, sizeof(kline) - kn, "%s%s %" PRIu64 " (%" PRIu64
                                    "/frame)", kn ? ", " : "", orbis_kc_names[i], calls,
                                    np ? calls / np : 0);
                  }

                  unsigned long long fw = 0, ftw = 0, fto = 0, flk = 0;
                  unsigned long long cm = 0, cr = 0;
                  if (&orbis_umtx_stats != NULL)
                     orbis_umtx_stats(&fw, &ftw, &fto, &flk);
                  if (&orbis_clock_counts != NULL)
                     orbis_clock_counts(&cm, &cr);

                  if (ifree == 0) {
                     mesa_logw("orbis-drm: libkernel internal memory is UNREADABLE - "
                               "sceKernelInternalMemoryGetAvailableSize did not resolve, so the "
                               "attribution below has no left-hand side. Kernel calls this window: %s",
                               kn ? kline : "none");
                  } else {
                     const int64_t lost = (int64_t)win_ifree - (int64_t)ifree;
                     mesa_logi("orbis-drm: libkernel internal memory %" PRIu64 " bytes free, %" PRId64
                               " lost this window = %" PRId64 " bytes a frame, against kernel calls: %s"
                               "; futex shim this window: %llu wait(s), %llu cond-timedwait(s), "
                               "%llu TIMED OUT, %llu bucket lock(s); clock_gettime this window: "
                               "%llu monotonic, %llu realtime",
                               ifree, win_ifree ? lost : 0,
                               (win_ifree && np) ? lost / (int64_t)np : 0, kn ? kline : "none",
                               fw - win_fw, ftw - win_ftw, fto - win_fto, flk - win_flk,
                               cm - win_cm, cr - win_cr);
                  }
                  win_ifree = ifree;
                  win_fw = fw; win_ftw = ftw; win_fto = fto; win_flk = flk;
                  win_cm = cm; win_cr = cr;
                  for (unsigned i = 0; i < ORBIS_KC_SLOTS; i++)
                     win_kc[i] = orbis_kc[i];
               }
#endif
            }

#if defined(__PS4__)
            /* Seeded on the FIRST window, which prints nothing, so the first line that does print
             * carries a real difference rather than the whole of startup. */
            if (win_ifree == 0) {
               win_ifree = orbis_internal_memory_free();
               if (&orbis_umtx_stats != NULL)
                  orbis_umtx_stats(&win_fw, &win_ftw, &win_fto, &win_flk);
               if (&orbis_clock_counts != NULL)
                  orbis_clock_counts(&win_cm, &win_cr);
               for (unsigned i = 0; i < ORBIS_KC_SLOTS; i++)
                  win_kc[i] = orbis_kc[i];
            }
#endif
            win_ns = now;
            win_cpu_us = cpu_us;
            win_seq = seq;
            win_pw_ns = orbis_present_wait_ns;
            win_pw_n = orbis_present_waits;
            win_passes = orbis_gfx_passes;
            win_px = orbis_gfx_pass_pixels;
            win_disp = orbis_dispatches;
            win_groups = orbis_dispatch_groups;
            win_sub_ns = orbis_submit_cpu_ns;
            win_rec_ns = orbis_record_ns;
            win_draws = orbis_draws;
            win_dbinds = orbis_desc_binds;
            win_spans = orbis_record_spans;
            for (unsigned i = 0; i < ORBIS_API_SLOTS; i++) {
               win_api_ns[i] = orbis_api_slot_ns[i];
               win_api_calls[i] = orbis_api_slot_calls[i];
            }
            next_ns = now + 5ull * 1000 * 1000 * 1000;
         }
         simple_mtx_unlock(&budget_lock);
      }
   }

   /* THE OUT-SYNCOBJS ARE THE GPU'S TO SIGNAL, and attaching this submission's sequence number is what says
    * so. They become signalled when the fence label reaches it - i.e. when the end-of-pipe packet appended
    * above executes - rather than when these packets were queued. */
   for (int i = 0; i < num_chunks; i++) {
      if (chunks[i].chunk_id != AMDGPU_CHUNK_ID_SYNCOBJ_OUT)
         continue;
      const struct drm_amdgpu_cs_chunk_sem *sems = (const void *)(uintptr_t)chunks[i].chunk_data;
      const uint32_t n = chunks[i].length_dw / (sizeof(*sems) / 4);
      simple_mtx_lock(&orbis_sync_lock);
      for (uint32_t j = 0; j < n; j++) {
         struct orbis_sync *slot = orbis_sync_get(sems[j].handle);
         if (!slot) {
            mesa_logw("orbis-drm: SYNCOBJ_OUT names unknown handle %u", sems[j].handle);
            continue;
         }
         slot->value = 0;
         slot->gpu_seq = seq;
      }
      simple_mtx_unlock(&orbis_sync_lock);
   }

   ORBIS_SUBMIT_STAGE("out-syncobjs attached, returning");
   *seq_no = seq;
   { return orbis_submit_return(0, orbis_submit_t0); }
#undef ORBIS_SUBMIT_STAGE
}

int
ac_drm_cs_query_fence_status(ac_drm_device *dev, uint32_t ctx_id, uint32_t ip_type, uint32_t ip_instance,
                             uint32_t ring, uint64_t fence_seq_no, uint64_t timeout_ns, uint64_t flags,
                             uint32_t *expired)
{
   /* THE GPU'S OWN WRITE, POLLED. orbis_fence_label is what the end-of-pipe packet appended to every submit
    * writes, so this is a real completion test rather than a promise.
    *
    * ⚠ SIGNED DELTA, not `>=`. The label is 32 bits and the sequence number is 64; comparing the truncated
    * values directly makes the four-billionth submit look like it completed before the first. The fork's
    * fence poll uses the same signed-difference test for the same reason. */
   const uint32_t want = (uint32_t)fence_seq_no;
   const bool reached = (int32_t)(*orbis_fence_label - want) >= 0;
   if (reached) {
      *expired = 1;
      return 0;
   }

   /* NOT REACHED YET, WHICH IS THE INTERESTING CASE. If RADV is blocked for half a second and then forever,
    * this is one of the two places it can be doing it, and the values say which fence and how far behind the
    * GPU is. Bounded log, because a busy frame polls this constantly. */
   static unsigned said_wait;
   if (orbis_trace() && said_wait < 8) {
      ++said_wait;
      mesa_logi("orbis-drm: fence wait: want %u, label %u, timeout %" PRIu64 " ns, flags 0x%" PRIx64, want,
                *orbis_fence_label, timeout_ns, flags);
   }

   /* AMDGPU_QUERY_FENCE_TIMEOUT_IS_ABSOLUTE is bit 0. Without it the value is a duration, and treating one as
    * the other is how a 1 ms wait becomes a wait until the heat death of the console. */
   struct orbis_wait_watch watch;
   orbis_wait_begin(&watch, (flags & 0x1) ? timeout_ns : os_time_get_absolute_timeout(timeout_ns), "fence");

   for (;;) {
      if ((int32_t)(*orbis_fence_label - want) >= 0) {
         *expired = 1;
         return 0;
      }
      if (timeout_ns == 0 || !orbis_wait_continue(&watch)) {
         if (timeout_ns != 0)
            mesa_logw("orbis-drm: fence %u never reached - label is %u, the GPU did not finish",
                      (uint32_t)fence_seq_no, *orbis_fence_label);
         *expired = 0;
         return 0; /* Not an error: "not yet" is a legitimate answer to a bounded wait. */
      }
      orbis_poll_pause();
   }
}

/* ---------------------------------------------------------------- refusals the winsys brought with it
 *
 * Each of these is a DECISION, and the consequence is stated here rather than discovered inside
 * vkCreateDevice.
 */

/* ⚠ NOT ONLY ABOUT SHARING, AND REFUSING IT WHOLESALE WAS WRONG. radv_amdgpu_bo.c:649 calls this with
 * amdgpu_bo_handle_type_kms under an assert(!r) to get the uint32_t handle it then passes to va_op_raw,
 * query_info and set_metadata - the handle table's whole reason to exist. A KMS handle is this process's own
 * name for its own buffer; only the dma-buf and flink spellings leave the process. Same lesson as
 * ac_drm_cs_syncobj_import_sync_file below: a name with "export" in it can serve an internal need. */
int
ac_drm_bo_export(ac_drm_device *dev, ac_drm_bo bo, enum amdgpu_bo_handle_type type,
                 uint32_t *shared_handle)
{
   struct orbis_bo *obo = bo.abo;
   if (!obo)
      return -EINVAL;

   switch (type) {
   case amdgpu_bo_handle_type_kms:
   case amdgpu_bo_handle_type_kms_noimport:
      *shared_handle = obo->handle;
      return 0;
   default:
      ORBIS_DRM_REFUSED("only KMS handles exist here - dma-buf and flink leave the process");
      return -ENOSYS;
   }
}

int
ac_drm_bo_import(ac_drm_device *dev, enum amdgpu_bo_handle_type type, uint32_t shared_handle,
                 struct ac_drm_bo_import_result *output)
{
   ORBIS_DRM_REFUSED("VK_KHR_external_memory has no meaning for a single homebrew process");
   return -ENOSYS;
}

int
ac_drm_cs_syncobj_export_sync_file(ac_drm_device *dev, uint32_t syncobj, int *sync_file_fd)
{
   ORBIS_DRM_REFUSED("there are no sync_file descriptors on this platform");
   return -ENOSYS;
}

int
ac_drm_cs_syncobj_export_sync_file2(ac_drm_device *dev, uint32_t syncobj, uint64_t point, uint32_t flags,
                                    int *sync_file_fd)
{
   ORBIS_DRM_REFUSED("there are no sync_file descriptors on this platform");
   return -ENOSYS;
}

int
ac_drm_cs_syncobj_import_sync_file(ac_drm_device *dev, uint32_t syncobj, int sync_file_fd)
{
   /* ⚠ THIS ONE IS NOT ONLY ABOUT SHARING, and that is worth knowing before the first hang.
    * radv_amdgpu_cs.c uses the export/accumulate/import trio to fold several waits into ONE queue syncobj
    * for its own internal ordering - not to hand a fence to another process. Refusing it makes that path
    * return VK_ERROR_DEVICE_LOST, so if a submit ever dies there, the fix is to give the winsys an
    * equivalent over the label page rather than to implement sync_file. */
   ORBIS_DRM_REFUSED("there are no sync_file descriptors on this platform - see the note above");
   return -ENOSYS;
}

int
ac_drm_cs_syncobj_timeline_wait(ac_drm_device *dev, uint32_t *handles, uint64_t *points,
                                unsigned num_handles, int64_t timeout_nsec, unsigned flags,
                                uint32_t *first_signaled)
{
   ORBIS_DRM_REFUSED("timeline semaphores are not advertised - the provider's timeline_wait is NULL");
   return -ENOSYS;
}

int
ac_drm_cs_syncobj_query2(ac_drm_device *dev, uint32_t *handles, uint64_t *points, unsigned num_handles,
                         uint32_t flags)
{
   ORBIS_DRM_REFUSED("timeline semaphores are not advertised, so there is no point value to report");
   return -ENOSYS;
}

int
ac_drm_cs_syncobj_transfer(ac_drm_device *dev, uint32_t dst_handle, uint64_t dst_point,
                           uint32_t src_handle, uint64_t src_point, uint32_t flags)
{
   /* ⚠ IMPLEMENTED FOR THE BINARY CASE, BECAUSE PRESENTATION NEEDS IT AND THIS MODEL MAKES IT TRIVIAL.
    *
    * A syncobj here is {value, gpu_seq}: signalled either by the CPU or once the fence label reaches gpu_seq.
    * "Transfer a payload" is therefore a copy of those two numbers - which is exactly what
    * radv_amdgpu_cs_submit_zero wants when it accumulates a present's waits into the queue's syncobj and then
    * signals the present's semaphores from it.
    *
    * It used to be refused as "a timeline operation", and that was wrong twice over: RADV uses it for BINARY
    * syncobjs too (radv_amdgpu_cs.c:1227), and the refusal made vkQueuePresentKHR return DEVICE_LOST - measured,
    * on the first frame the engine ever tried to present.
    *
    * MAX RATHER THAN ASSIGN: a syncobj must never go backwards. Accumulating several waits into one queue
    * syncobj means "wait for the latest of them", and an assignment would let the second copy undo the first.
    *
    * A NON-ZERO POINT IS STILL REFUSED. Timeline points need the value itself, this store keeps a counter, and
    * pretending otherwise would make a wait return early - which is a corruption, not a missing feature. */
   (void)flags;
   return orbis_sync_transfer_payload(dst_handle, dst_point, src_handle, src_point);
}


int
ac_drm_vm_reserve_vmid(ac_drm_device *dev, uint32_t flags)
{
   ORBIS_DRM_REFUSED("there are no VMIDs to reserve - this platform exposes one address space");
   return -ENOSYS;
}

int
ac_drm_vm_unreserve_vmid(ac_drm_device *dev, uint32_t flags)
{
   ORBIS_DRM_REFUSED("there are no VMIDs to reserve - this platform exposes one address space");
   return -ENOSYS;
}

int
ac_drm_query_sensor_info(ac_drm_device *dev, unsigned sensor_type, unsigned size, void *value)
{
   /* Clocks and temperature. RADV asks only through radeon_winsys_query_value, which serves
    * VK_EXT_memory_budget and the debug overlay - nothing that affects rendering. */
   ORBIS_DRM_REFUSED("no sensor path is citable on this platform");
   return -ENOSYS;
}

int
ac_drm_read_mm_registers(ac_drm_device *dev, unsigned dword_offset, unsigned count, uint32_t instance,
                         uint32_t flags, uint32_t *values)
{
   /* Only RADV's hang report reads MMIO (radv_amdgpu_winsys_read_registers), and a hang report that says
    * "registers unavailable" is more honest than one printing the drm-shim's Bonaire dump as if it had been
    * read from this GPU. */
   ORBIS_DRM_REFUSED("MMIO reads are not exposed here; only the hang report wants them");
   return -ENOSYS;
}
