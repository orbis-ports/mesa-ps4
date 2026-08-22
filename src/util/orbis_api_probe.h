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
