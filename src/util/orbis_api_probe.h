/*
 * SPDX-License-Identifier: MIT
 *
 * ⚠ WHERE DOES THE FRAME'S CPU GO, when it is demonstrably not in the driver's own paths?
 *
 * The frame is CPU-bound and three counters have already excluded the obvious places: the submit
 * path costs 0.3 ms a frame, command-buffer recording under 1 ms, and the title issues only 86
 * draws. That leaves tens of milliseconds a frame with no owner, and the difference between "the
 * title's own work" and "a Vulkan entry point nobody timed" decides who has to fix it.
 *
 * So this times a handful of high-level entry points - submit, present, acquire, fence waits,
 * descriptor updates, allocations - at their API boundary. Not per draw: these run a few times a
 * frame at most, so the two clock reads are free at this scale.
 *
 * The cleanup attribute is what makes it one line at the top of a function rather than an edit at
 * every return, and these functions have many.
 */

#ifndef ORBIS_API_PROBE_H
#define ORBIS_API_PROBE_H

#ifdef __cplusplus
extern "C" {
#endif

enum orbis_api_slot {
   ORBIS_API_SUBMIT = 0,
   ORBIS_API_PRESENT,
   ORBIS_API_ACQUIRE,
   ORBIS_API_WAIT_FENCE,
   ORBIS_API_DESC_UPDATE,
   ORBIS_API_DESC_ALLOC,
   ORBIS_API_MEM_ALLOC,
   /* ⚠ THE POLL. OpenGothic's frame begins with a zero-timeout fence wait and RETURNS if the GPU is
    * not done, so the application re-enters render() and yields in a loop. Tempest turns that into
    * vkGetFenceStatus, which no other counter here sees. If this one comes back with thousands of
    * calls a frame, the process is spinning rather than working, and every "CPU-bound" reading taken
    * so far has been measuring a busy-wait. */
   ORBIS_API_FENCE_STATUS,

   /* ⚠ SLOTS THE APPLICATION FILLS IN, and the reason they exist.
    *
    * The driver's share of the frame is now bounded at about 4 ms and the poll turned out not to be
    * a spin, so the remaining ~45 ms is genuine work above the Vulkan boundary. Splitting it needs
    * a measurement inside the title - but the title already links this driver, so it can call
    * ac_orbis_api_account() directly and its phases land in the same per-frame line as everything
    * else, on the same clock, in the same window.
    *
    * ⚠ THE NUMBERS ARE PART OF THE INTERFACE. OpenGothic hardcodes them (mainwindow.cpp); changing
    * them here without changing them there silently mislabels the report rather than failing. */
   ORBIS_APP_TICK = 16,   /* game update: world simulation alone */
   ORBIS_APP_UI = 17,     /* UI meshes and paint events */
   ORBIS_APP_DRAW = 18,   /* Renderer::draw - Tempest building the frame's commands */
   ORBIS_APP_PRESENT = 19,/* Device::submit and Device::present as the title sees them */
   ORBIS_APP_ANIM = 20,   /* updateAnimation - the bones */
   ORBIS_APP_CAMERA = 21, /* tickCamera */

   ORBIS_API_SLOTS,
};

/* ⚠ THE KERNEL-CALL CENSUS, AND IT LIVES HERE BECAUSE THE FIRST VERSION OF IT LIED BY OMISSION.
 *
 * Counting every memory-touching sceKernel entry point in ac_orbis_drm.c produced windows whose only
 * non-zero counter was SubmitCommandBuffers, and that was read - by me - as "the leak is per submit".
 * It is not what the data said. It said "the leak is per something that happens once per submit, AMONG
 * THE CALLS THAT WERE COUNTED", and the calls that were NOT counted include every sceKernel and
 * sceVideoOut entry point in wsi_orbis.c and sceKernelUsleep everywhere. Presents run 1:1 with submits
 * in every leaking window, so a per-present cost fits the same numbers exactly as well - measured:
 * 650 B x presents fits w11-w13 to 0.02%, 309 B x submits fits them to 0.3%. The census could not tell
 * them apart and did not say so.
 *
 * ⚠ SO THE ENUM IS SHARED RATHER THAN DUPLICATED. The precedent above it - ids 47-52 written out by
 * hand in wsi_orbis.c with a comment saying they "were verified against it when this was written" - is
 * exactly the drift this avoids: two files agreeing by inspection until one of them changes. */
enum orbis_kc_slot {
   ORBIS_KC_DMEM_ALLOC = 0,
   ORBIS_KC_DMEM_RELEASE,
   ORBIS_KC_DMEM_MAP,
   ORBIS_KC_FMEM_MAP,
   ORBIS_KC_MMAP,
   ORBIS_KC_MUNMAP,
   ORBIS_KC_MPROTECT,
   ORBIS_KC_SUBMIT,
   ORBIS_KC_BO_ALLOC,
   ORBIS_KC_BO_FREE,
   ORBIS_KC_VA_MAP,
   ORBIS_KC_VA_UNMAP,
   ORBIS_KC_SYNC_TIMEOUT,
   ORBIS_KC_SYNC_POLL,

   /* ⚠ EVERYTHING BELOW THIS LINE IS WHAT THE FIRST CENSUS COULD NOT SEE.
    *
    * usleep first, because it is on BOTH hot paths and is the highest-frequency libkernel call in the
    * process: orbis_poll_pause sleeps 50 us per iteration of every fence poll, and
    * wsi_orbis_wait_for_flip_slot sleeps 500 us per iteration while present blocks ~15 ms of every
    * 16.7 ms frame. Neither was counted, and either would divide into the measured constants. */
   ORBIS_KC_USLEEP,
   ORBIS_KC_GNM_FLUSH_GARLIC,
   ORBIS_KC_GNM_SUBMIT_DONE,
   ORBIS_KC_GNM_ALLOWED,
   ORBIS_KC_VO_SUBMIT_FLIP,
   ORBIS_KC_VO_FLIP_STATUS,
   ORBIS_KC_VO_REGISTER,
   ORBIS_KC_VO_OPEN_CLOSE,

   ORBIS_KC_SLOTS,
};

/* Defined in ac_orbis_drm.c, which owns the counters and the report that divides them by presents. */
void orbis_kc_count(unsigned slot);

/* ⚠ THE FRAME LEDGER, AND IT REPLACES A MEASUREMENT AIMED AT THE WRONG AXIS.
 *
 * A bisection that sampled around POLLS was built on a coefficient - 73.0 bytes per syncobj poll,
 * exact to 0.03% - that turned out to be a coincidence of a stable poll rate. Measured 2026-08-31:
 * the poll rate fell 2.7x between two runs (162 -> 60 a frame) and the loss barely moved
 * (11863 -> 10224 bytes A FRAME). Fitting both points gives ~9260 bytes per frame plus ~16 per poll,
 * which is two unknowns from two measurements and therefore a description rather than a result - but
 * the axis is settled: the big term is PER FRAME.
 *
 * So the meter is read at named points around the frame instead, and each segment is bounded by ONE
 * call, because a segment containing four candidates names none of them. The segment between the
 * present's exit and the next submission's entry is the frontend, zink and RADV's recording - the
 * "outside our code entirely" case, which no instrument in this port has ever weighed. */
enum orbis_ledger_id {
   ORBIS_LG_PRESENT_ENTER = 0,   /* .. to after the GPU-idle wait */
   ORBIS_LG_AFTER_GPU_IDLE,      /* .. to after sceGnmSubmitDone */
   ORBIS_LG_AFTER_SUBMIT_DONE,   /* .. to after the flip-slot wait */
   ORBIS_LG_AFTER_FLIP_SLOT,     /* .. to after the scan-out copy */
   ORBIS_LG_AFTER_COPY,          /* .. to after sceVideoOutSubmitFlip */
   ORBIS_LG_PRESENT_EXIT,        /* .. to the next submission: everything above this driver */
   ORBIS_LG_SUBMIT_ENTER,        /* .. to after sceGnmFlushGarlic */
   ORBIS_LG_AFTER_FLUSH_GARLIC,  /* .. to after sceGnmSubmitCommandBuffers */
   ORBIS_LG_AFTER_GNM_SUBMIT,    /* .. to the submission's return */
   ORBIS_LG_SUBMIT_EXIT,         /* .. to whatever comes next */
   ORBIS_LG_IDS,
};

void orbis_ledger_mark(unsigned id);

#ifdef HAVE_ORBIS_PLATFORM

#include <stdint.h>
#include "util/os_time.h"

void ac_orbis_api_account(unsigned slot, uint64_t ns);

struct orbis_api_probe {
   unsigned slot;
   uint64_t t0;
};

static inline void
orbis_api_probe_end(struct orbis_api_probe *p)
{
   ac_orbis_api_account(p->slot, os_time_get_nano() - p->t0);
}

#define ORBIS_API_PROBE(slot_)                                                                                         \
   struct orbis_api_probe orbis_probe_##slot_ __attribute__((cleanup(orbis_api_probe_end))) = {(slot_),                \
                                                                                               os_time_get_nano()}

#else

#define ORBIS_API_PROBE(slot_)                                                                                         \
   do {                                                                                                                \
   } while (0)

#endif

#ifdef __cplusplus
}
#endif

#endif /* ORBIS_API_PROBE_H */
