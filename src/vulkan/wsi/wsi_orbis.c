/*
 * Copyright 2026 Mikołaj Mikołajczyk
 * SPDX-License-Identifier: MIT
 *
 * The PS4's scan-out for Mesa's WSI. See wsi_orbis.h for why it exists and why it hangs off the headless
 * platform. Every constant and every call order here is taken from the sequence that already put a picture on
 * the television (Tempest ps4/radv, probe_scanout) rather than derived again.
 */

#include "wsi_orbis.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "util/log.h"
#include "util/os_time.h"
#include "util/macros.h"

/* ⚠ THE SDK IS ON THE CONSOLE'S SIDE OF THE SEAM, AND SO IS THE TELEVISION.
 *
 * ac_orbis_drm.c splits the same way and states the principle: -Dplatforms=orbis selects the ARM, not the
 * TOOLCHAIN, so the host build compiles this file too. It did not - this file included the SDK headers
 * unconditionally, which broke `build.sh --host-orbis` the day it was written and stayed broken for a whole
 * session, because nothing in this port runs that build.
 *
 * The split is AT THE SONY CALLS AND NOWHERE ELSE, so everything below is one version rather than two. The
 * buffer bookkeeping, the pitch arithmetic and the row-by-row copy - the parts a laptop can catch being
 * wrong - run in both builds; what is left for hardware is only "does sceVideoOut agree?".
 */
#if defined(__PS4__)
#include <orbis/VideoOut.h>
#include <orbis/libkernel.h>
#include <orbis/GnmDriver.h>
#else
#include <sys/types.h>

/* Values copied from the SDK headers named above, so the host build compiles the SAME constants the console
 * build passes rather than placeholders that would hide a typo until a flash. */
typedef int32_t OrbisUserServiceUserId;

#define ORBIS_VIDEO_USER_MAIN                      0xFF
#define ORBIS_VIDEO_OUT_BUS_MAIN                   0
#define ORBIS_VIDEO_OUT_FLIP_VSYNC                 1
#define ORBIS_VIDEO_OUT_FLIP_60HZ                  0
#define ORBIS_VIDEO_OUT_TILING_MODE_LINEAR         0x1
#define ORBIS_VIDEO_OUT_ASPECT_RATIO_16_9          0x0
#define ORBIS_VIDEO_OUT_PIXEL_FORMAT_A8B8G8R8_SRGB 0x80002200
#define ORBIS_KERNEL_WC_GARLIC                     0x3

typedef struct {
   uint32_t pixelFormat, tilingMode, aspectRatio, width, height, pitchInPixel;
} OrbisVideoOutBufferAttribute;

typedef struct {
   int32_t numFlipPending;
} OrbisVideoOutFlipStatus;

/* A HOST FLIP SUCCEEDS AND SHOWS NOTHING. Reporting failure instead would make every host swapchain go
 * OUT_OF_DATE, which is an alarm about the platform rather than a fact about the code - and the point of this
 * arm is to let a defect in the code be the only thing that fails. */
static int32_t
sceVideoOutOpen(OrbisUserServiceUserId user, int32_t bus, int32_t index, const void *param)
{
   mesa_logi("wsi/orbis: no video-out off the console - the scan-out is ordinary memory and a flip displays "
             "nothing. Everything up to the flip is the console's own code path.");
   return 1;
}

static int32_t sceVideoOutClose(int32_t handle) { return 0; }

static void
sceVideoOutSetBufferAttribute(void *attr, uint32_t fmt, uint32_t tiling, uint32_t aspect, uint32_t width,
                              uint32_t height, uint32_t pitch_in_pixel)
{
   OrbisVideoOutBufferAttribute *const a = attr;
   a->pixelFormat = fmt;
   a->tilingMode = tiling;
   a->aspectRatio = aspect;
   a->width = width;
   a->height = height;
   a->pitchInPixel = pitch_in_pixel;
}

static int32_t
sceVideoOutRegisterBuffers(int32_t handle, int32_t start, void *const *addrs, int32_t count,
                           const OrbisVideoOutBufferAttribute *attr)
{
   return 0;
}

static int32_t sceVideoOutSetFlipRate(int32_t handle, int32_t rate) { return 0; }

/* Nothing is pending, ever, so wsi_orbis_wait_for_flip_slot returns on its first look rather than spending
 * its two-second budget waiting for a display that is not there. */
static int32_t
sceVideoOutGetFlipStatus(int32_t handle, OrbisVideoOutFlipStatus *st)
{
   st->numFlipPending = 0;
   return 0;
}

static int32_t
sceVideoOutSubmitFlip(int32_t handle, int32_t index, uint32_t mode, int64_t flip_arg)
{
   return 0;
}

/* DIRECT MEMORY BECOMES AN ORDINARY ALIGNED ALLOCATION, and the "physical address" is the allocation's own
 * pointer. That keeps allocate/map/release three separate steps with their three separate failure paths, so
 * the create-and-unwind logic this arm exists to exercise is the same logic in both builds. */
static size_t sceKernelGetDirectMemorySize(void) { return (size_t)1 << 32; }

static int32_t
sceKernelAllocateDirectMemory(off_t search_start, off_t search_end, size_t len, size_t align, int32_t type,
                              off_t *phys)
{
   void *p = NULL;
   if (posix_memalign(&p, align, len) != 0)
      return -1;
   *phys = (off_t)(uintptr_t)p;
   return 0;
}

static int32_t
sceKernelMapDirectMemory(void **addr, size_t len, int32_t prot, int32_t flags, off_t phys, size_t align)
{
   *addr = (void *)(uintptr_t)phys;
   return 0;
}

static int32_t
sceKernelReleaseDirectMemory(off_t phys, size_t len)
{
   free((void *)(uintptr_t)phys);
   return 0;
}

static int32_t sceKernelUsleep(uint32_t usec) { os_time_sleep(usec); return 0; }

static int32_t sceGnmAreSubmitsAllowed(void) { return 1; }
static int32_t sceGnmSubmitDone(void) { return 0; }
#endif

/* ⚠ THE PRESENT PATH TIMED FROM ITS OWN FILE. Counters and accessors live in ac_orbis_drm.c - permanent, and the
 * only place they can live without making the arm depend on the transient diagnostic patch that reports them. Ids
 * must match that patch's enum; 47-50 were verified against it when this was written. */
void orbis_api_count(unsigned id, uint64_t n);
void orbis_api_time(unsigned id, uint64_t ns);

/* ⚠ AND THE KERNEL-CALL CENSUS, WHICH THIS FILE WAS MISSING FROM ENTIRELY - the omission that sent a
 * whole round of this investigation at the wrong call.
 *
 * The census in ac_orbis_drm.c counted every memory-touching sceKernel entry point IN THAT FILE, and
 * the report then attributed a 309 B/frame leak to sceGnmSubmitCommandBuffers because it was the only
 * counter that moved. It was the only counter that EXISTED on a per-frame path: presents run 1:1 with
 * submits in every leaking window, and every sceVideoOut call and every sceKernelUsleep in this file
 * was invisible. A per-present cost fits the same measurements just as well - 650 B x presents matched
 * three consecutive windows to 0.02%.
 *
 * So the present path books into the same counters, through the same shared enum, and the next report
 * can tell a per-submit cost from a per-present one instead of assuming. */
#include "util/orbis_api_probe.h"

#if defined(__PS4__)
#define sceKernelUsleep(...) (orbis_kc_count(ORBIS_KC_USLEEP), (sceKernelUsleep)(__VA_ARGS__))
#define sceVideoOutSubmitFlip(...)                                                                    \
   (orbis_kc_count(ORBIS_KC_VO_SUBMIT_FLIP), (sceVideoOutSubmitFlip)(__VA_ARGS__))
#define sceVideoOutGetFlipStatus(...)                                                                 \
   (orbis_kc_count(ORBIS_KC_VO_FLIP_STATUS), (sceVideoOutGetFlipStatus)(__VA_ARGS__))
#define sceVideoOutRegisterBuffers(...)                                                               \
   (orbis_kc_count(ORBIS_KC_VO_REGISTER), (sceVideoOutRegisterBuffers)(__VA_ARGS__))
#define sceVideoOutOpen(...) (orbis_kc_count(ORBIS_KC_VO_OPEN_CLOSE), (sceVideoOutOpen)(__VA_ARGS__))
#define sceVideoOutClose(...) (orbis_kc_count(ORBIS_KC_VO_OPEN_CLOSE), (sceVideoOutClose)(__VA_ARGS__))
#define sceGnmSubmitDone(...)                                                                         \
   (orbis_kc_count(ORBIS_KC_GNM_SUBMIT_DONE), (sceGnmSubmitDone)(__VA_ARGS__))
#define sceGnmAreSubmitsAllowed(...)                                                                  \
   (orbis_kc_count(ORBIS_KC_GNM_ALLOWED), (sceGnmAreSubmitsAllowed)(__VA_ARGS__))
#define sceKernelAllocateDirectMemory(...)                                                            \
   (orbis_kc_count(ORBIS_KC_DMEM_ALLOC), (sceKernelAllocateDirectMemory)(__VA_ARGS__))
#define sceKernelMapDirectMemory(...)                                                                 \
   (orbis_kc_count(ORBIS_KC_DMEM_MAP), (sceKernelMapDirectMemory)(__VA_ARGS__))
#define sceKernelReleaseDirectMemory(...)                                                             \
   (orbis_kc_count(ORBIS_KC_DMEM_RELEASE), (sceKernelReleaseDirectMemory)(__VA_ARGS__))
#endif
enum { ORBIS_ID_WSI_PRESENT = 47, ORBIS_ID_WSI_GPU_IDLE = 48, ORBIS_ID_WSI_COPY = 49,
       ORBIS_ID_WSI_FLIP_WAIT = 50, ORBIS_ID_AB_A = 51, ORBIS_ID_AB_B = 52 };

/* ⚠ ONE PLACE, BECAUSE A FUNCTION WITH SEVEN EXITS HAS SEVEN CHANCES TO FORGET. Both timed functions below
 * book their duration on every path, and each path spelled the same three statements out again - so a return
 * added later reads as ordinary code while silently dropping the measurement, and the counter that goes quiet
 * looks like a path that stopped running. */
#define ORBIS_BOOK_RETURN(id, t0, v)                                                                          \
   do {                                                                                                       \
      orbis_api_time((id), os_time_get_nano() - (t0));                                                         \
      orbis_api_count((id), 1);                                                                               \
      return (v);                                                                                             \
   } while (0)

/* ⚠ THE A/B THAT CANNOT BE CONFOUNDED BY THE CAMERA. Two runs comparing raster-config values were voided because
 * GPU time depends on what is on screen: the SAME value measured 184 ms and 145 ms in two runs - 27% apart - while
 * the effect under test was 14%. Inside one run the measurement is precise to 0.4%, and the only fixed-camera scene
 * in the game (the main menu) is vsync-bound at 60 fps with 4.5 ms of GPU work, so it cannot host the test either.
 *
 * So the two values alternate BETWEEN FRAMES of the same scene from the same viewpoint, and each frame's GPU wait is
 * booked to whichever value was live. Same geometry, same camera, interleaved - the confound is removed by the
 * design rather than by asking someone to stand still twice.
 *
 * ORBIS_RASTER_AB=<hexA>:<hexB> arms it. Unset, nothing is written and the slot is never touched. */
extern uint32_t *ac_orbis_raster_slot;

static bool     orbis_ab_live;
static unsigned orbis_ab_bucket;

static bool
orbis_raster_ab(unsigned *bucket)
{
   static int      armed = -1;
   static uint32_t a, b;
   static unsigned phase;

   if (armed < 0) {
      const char *const e = getenv("ORBIS_RASTER_AB");
      armed = 0;
      if (e != NULL && ac_orbis_raster_slot != NULL) {
         char *endp = NULL;
         a = (uint32_t)strtoul(e, &endp, 0);
         if (endp != NULL && *endp == ':') {
            b = (uint32_t)strtoul(endp + 1, NULL, 0);
            armed = 1;
            mesa_logw("wsi/orbis: RASTER A/B armed - alternating PA_SC_RASTER_CONFIG between 0x%08x and 0x%08x every "
                      "frame, and booking each frame's GPU wait to the value that was live. Same scene, same camera, "
                      "interleaved: this is the comparison two separate runs could not make.", a, b);
         } else {
            mesa_logw("wsi/orbis: ORBIS_RASTER_AB wants <hexA>:<hexB> - ignoring \"%s\"", e);
         }
      }
   }
   if (armed != 1)
      return false;

   phase ^= 1u;
   *ac_orbis_raster_slot = phase ? a : b;
   *bucket = phase ? ORBIS_ID_AB_A : ORBIS_ID_AB_B;
   return true;
}

/* 2 MiB, which is what the shipped OpenOrbis sample passes as both the round-up and the alignment argument for
 * direct memory, and what the fork's own allocator uses. */
#define WSI_ORBIS_DIRECT_ALIGN 0x200000ull
/* CPU_RW | GPU_RW, the same protection the driver's arena is mapped with. */
#define WSI_ORBIS_PROT 0x33
/* Sony's own limit on a single RegisterBuffers call is well above this; the cap exists so a swapchain asking for
 * something absurd is refused with a number in the log rather than by an allocator. */
#define WSI_ORBIS_MAX_BUFFERS 8

struct wsi_orbis_scanout {
   int32_t  video;
   uint32_t width, height, count;
   uint64_t bytes;   /* per buffer, aligned */
   void    *map[WSI_ORBIS_MAX_BUFFERS];
   off_t    phys[WSI_ORBIS_MAX_BUFFERS];
   uint64_t flips;
   /* ⚠ HOW MANY IMAGES SOMEBODY ELSE IS HOLDING while the display works, which is the caller's
    * business and not something this file may assume. See wsi_orbis_scanout_reserve. */
   uint32_t reserve;
   /* ⚠ WHOSE MEMORY IS BEING SCANNED OUT. False when the swapchain's own images are registered
    * directly - then there is nothing to copy and nothing here to release, because RADV owns those
    * pages. See the note on wsi_orbis_scanout_create. */
   bool     owns_buffers;
};

static void
wsi_orbis_release(struct wsi_orbis_scanout *so)
{
   for (uint32_t i = 0; i < WSI_ORBIS_MAX_BUFFERS; i++) {
      if (so->map[i] != NULL && so->owns_buffers)
         sceKernelReleaseDirectMemory(so->phys[i], so->bytes);
      so->map[i] = NULL;
   }
   if (so->video >= 0)
      sceVideoOutClose(so->video);
   so->video = -1;
}

/* ⚠ THE COPY IS THE LAST THING BETWEEN THE FRAME AND THE TELEVISION, and it costs 2.3 ms of a
 * 21.9 ms frame - over a tenth of the frame to move 8100 KiB that the GPU has already written.
 *
 * It exists because this WSI allocated its own scan-out buffers and the swapchain's images lived
 * somewhere else. They need not: the images are LINEAR and CPU-mapped already (wants_linear), and
 * this arm's memory is direct memory, which is what the display controller scans out of. So when
 * the caller passes their addresses, they are registered with video-out directly and the present
 * becomes a flip.
 *
 * `addrs == NULL` keeps the old behaviour, and it is not dead code: video-out may refuse an
 * address for alignment or for memory type, and a swapchain that cannot reach the display must fall
 * back to something that works rather than fail. The caller tries zero-copy first and this way
 * second, which is why the whole open/attribute/register sequence is one function.
 *
 * `pitch_px` is the image's OWN row pitch in pixels, which need not equal the width - video-out
 * takes it as a separate argument, so an image RADV padded is registered as it is rather than
 * copied to remove the padding. */
struct wsi_orbis_scanout *
wsi_orbis_scanout_create(uint32_t width, uint32_t height, uint32_t count, void *const *addrs,
                         uint32_t pitch_px)
{
   if (count == 0 || count > WSI_ORBIS_MAX_BUFFERS) {
      mesa_loge("wsi/orbis: %u swapchain images is outside 1..%d", count, WSI_ORBIS_MAX_BUFFERS);
      return NULL;
   }

   struct wsi_orbis_scanout *so = calloc(1, sizeof(*so));
   if (so == NULL)
      return NULL;
   so->video = -1;
   so->width = width;
   so->height = height;
   so->count = count;
   /* The registered buffer's pitch is the width: that is what the attribute below states, so the copy in
    * _present must produce exactly that regardless of what pitch the image has. */
   so->bytes = ((uint64_t)width * height * 4 + WSI_ORBIS_DIRECT_ALIGN - 1) & ~(WSI_ORBIS_DIRECT_ALIGN - 1);

   so->video = sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN, ORBIS_VIDEO_OUT_BUS_MAIN, 0, NULL);
   if (so->video < 0) {
      mesa_loge("wsi/orbis: sceVideoOutOpen -> %d", (int)so->video);
      goto fail;
   }

   /* ⚠ GARLIC, AND NOT THE DRIVER'S ARENA. The arena is one WB_ONION carve-out, and no oracle in this project
    * says video-out accepts ONION; the fork allocates its scan-out buffers from Garlic and reaches a display.
    * Physically contiguous and 2 MiB aligned for the same reason. */
   void *own[WSI_ORBIS_MAX_BUFFERS];
   so->owns_buffers = addrs == NULL;
   for (uint32_t i = 0; so->owns_buffers && i < count; i++) {
      int32_t err = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), so->bytes,
                                                  WSI_ORBIS_DIRECT_ALIGN, ORBIS_KERNEL_WC_GARLIC, &so->phys[i]);
      if (err != 0) {
         mesa_loge("wsi/orbis: sceKernelAllocateDirectMemory(%llu KiB GARLIC) for image %u -> 0x%08x",
                   (unsigned long long)(so->bytes / 1024), i, (unsigned)err);
         goto fail;
      }
      err = sceKernelMapDirectMemory(&so->map[i], so->bytes, WSI_ORBIS_PROT, 0, so->phys[i],
                                     WSI_ORBIS_DIRECT_ALIGN);
      if (err != 0) {
         mesa_loge("wsi/orbis: sceKernelMapDirectMemory for image %u -> 0x%08x", i, (unsigned)err);
         sceKernelReleaseDirectMemory(so->phys[i], so->bytes);
         so->map[i] = NULL;
         goto fail;
      }
      /* Cleared, so the first flip of a swapchain the application has not drawn into yet shows black rather than
       * whatever the previous title left in that memory. */
      memset(so->map[i], 0, (size_t)so->bytes);
      own[i] = so->map[i];
   }

   if (!so->owns_buffers) {
      /* Remembered for the present path, which needs to know there is nothing to copy. Not released
       * on destroy - RADV owns these pages and frees them with the images. */
      for (uint32_t i = 0; i < count; i++)
         so->map[i] = addrs[i];
   }

   OrbisVideoOutBufferAttribute attr;
   memset(&attr, 0, sizeof(attr));
   /* A8B8G8R8_SRGB and LINEAR: the display controller's own format, and the reason the swapchain format is
    * constrained to an SRGB one - rendering UNORM into an SRGB scan-out shows a picture with the wrong gamma,
    * which is the kind of "nearly right" this project does not accept as a pass. */
   sceVideoOutSetBufferAttribute(&attr, ORBIS_VIDEO_OUT_PIXEL_FORMAT_A8B8G8R8_SRGB,
                                 ORBIS_VIDEO_OUT_TILING_MODE_LINEAR, ORBIS_VIDEO_OUT_ASPECT_RATIO_16_9,
                                 width, height, pitch_px != 0 ? pitch_px : width);

   int32_t err = sceVideoOutRegisterBuffers(so->video, 0, so->owns_buffers ? own : (void *const *)addrs,
                                            count, &attr);
   if (err < 0) {
      /* Not an error line when the caller still has a fallback to try: the addresses it offered may
       * simply not be scan-out-able, which is a fact about this console rather than a fault. */
      if (so->owns_buffers)
         mesa_loge("wsi/orbis: sceVideoOutRegisterBuffers(%u) -> 0x%08x", count, (unsigned)err);
      else
         mesa_logi("wsi/orbis: video-out refused the swapchain's own images (0x%08x) - falling back to "
                   "buffers of its own and a copy per frame",
                   (unsigned)err);
      goto fail;
   }
   err = sceVideoOutSetFlipRate(so->video, ORBIS_VIDEO_OUT_FLIP_60HZ);
   if (err < 0)
      mesa_logw("wsi/orbis: sceVideoOutSetFlipRate -> 0x%08x (continuing; the flip rate is not correctness)",
                (unsigned)err);

   mesa_logi("wsi/orbis: scan-out up - %ux%u pitch %u, %u %s buffer(s), A8B8G8R8_SRGB linear%s", width, height,
             pitch_px != 0 ? pitch_px : width, count, so->owns_buffers ? "GARLIC" : "swapchain",
             so->owns_buffers ? " - one full-screen copy per frame" : " - ZERO COPY, the flip shows what "
                                                                     "the GPU rendered into");
   return so;

fail:
   wsi_orbis_release(so);
   free(so);
   return NULL;
}

/* ⚠ WHAT THE CALLER MUST KEEP, so this file can bound the display's queue without knowing anything
 * about the caller's own scheme. `reserve` is the number of images the caller holds while the
 * display works: one for the application to draw into, plus one more if presents are deferred.
 *
 * Returns the smallest image count that can work - the display needs the one on screen and the one
 * being flipped, so it is reserve + 2. A swapchain smaller than that cannot register its own images
 * at all, and the caller must fall back to copying rather than deadlock on acquire. */
uint32_t
wsi_orbis_scanout_reserve(struct wsi_orbis_scanout *so, uint32_t reserve)
{
   if (so != NULL)
      so->reserve = reserve;
   return reserve + 2;
}

/* How many flips the display has taken but not yet shown. The caller needs it to know which images
 * are still the display's: the queued ones plus the one on screen. Returns 0 if the status cannot be
 * read, which makes the caller hold only the newest - the safe direction is holding fewer only when
 * nothing can be known, and that case already logs. */
uint32_t
wsi_orbis_flips_pending(struct wsi_orbis_scanout *so)
{
   if (so == NULL || so->video < 0)
      return 0;

   OrbisVideoOutFlipStatus st;
   memset(&st, 0, sizeof(st));
   if (sceVideoOutGetFlipStatus(so->video, &st) != 0)
      return 0;
   return st.numFlipPending > 0 ? (uint32_t)st.numFlipPending : 0;
}

/* ⚠ THE DISPLAY HAS A QUEUE AND IT IS SHORTER THAN A FRAME LOOP. Measured: 21 flips went out in about 110 ms -
 * one every 5 ms against a 16.7 ms vsync - and then presentation started failing. Nothing in the Vulkan path
 * throttles on the DISPLAY: the engine's fence gates on GPU work, and this arm's present returns as soon as the
 * flip is submitted.
 *
 * So it waits for a slot. At most count-1 flips outstanding, which keeps the display from being overtaken.
 *
 * ⚠ THAT IS NOT THE SAME AS KEEPING A BUFFER FROM BEING OVERWRITTEN, and this comment used to claim it was.
 * A flip CONSUMED from the queue is still ON SCREEN until the next one replaces it, so "no flips pending"
 * means one buffer is being scanned out right now, not that all of them are free. While this code owned the
 * scan-out buffers the distinction did not matter - the copy was a snapshot - but with the swapchain's own
 * images registered it is the whole problem, and it belongs where images are handed back to the application
 * rather than here. See orbis_onscreen in wsi_common_headless.c.
 *
 * BOUNDED AND LOUD, like every other wait in this port: if the display stops consuming flips, this returns after
 * two seconds with a line naming the counter rather than hanging the title forever. sceKernelUsleep because
 * nanosleep does not return on this platform - a fact this project paid for once already. */
#define WSI_ORBIS_THROTTLE_NS (2ull * 1000 * 1000 * 1000)

static bool
wsi_orbis_wait_for_flip_slot(struct wsi_orbis_scanout *so)
{
   /* Timed from inside, because the function has several exits and a wrapper at each call site would be two places
    * to keep in step. The count is per call, so ms/s divided by calls/s is the average wait for a flip slot - which
    * is the number that says whether this path is waiting for the display or doing work. */
   const uint64_t orbis_fs_t0 = os_time_get_nano();

   /* ⚠ DERIVED, NOT TUNED - and the difference decides whether this is a driver or a fixture for one
    * title. A first version bounded the queue at count-3 because the swapchain in front of it had
    * five images; another application with three would have deadlocked on a number that fitted
    * nobody but OpenGothic.
    *
    * The invariant is what to write down instead. While the swapchain's own images are registered,
    * every image is in exactly one of four places:
    *
    *     P   queued flips the display has taken but not yet shown
    *     1   the image on screen
    *     1   the flip about to be submitted, which joins the queue
    *     R   whatever the caller holds - the deferred present, and one for the application to draw
    *         into, which it declares through wsi_orbis_scanout_reserve
    *
    * They must fit: P + 2 + R <= count. So the queue may be P <= count - 2 - R deep, and the loop
    * below proceeds while numFlipPending < count - 1 - R.
    *
    * With the copy there is no such arithmetic: the copy is a snapshot, a flipped buffer is free the
    * moment it returns, and count-1 only keeps the display from being overtaken. */
   const int32_t  reserve = so->owns_buffers ? 0 : (int32_t)so->reserve;
   const int32_t  limit = MAX2((int32_t)so->count - 1 - reserve, 1);
   const uint64_t deadline = os_time_get_nano() + WSI_ORBIS_THROTTLE_NS;

   for (;;) {
      OrbisVideoOutFlipStatus st;
      memset(&st, 0, sizeof(st));
      const int32_t err = sceVideoOutGetFlipStatus(so->video, &st);
      if (err != 0) {
         /* Cannot measure the queue: go ahead rather than stall. A flip that then fails is reported by the caller,
          * which is a better failure than a frame loop that stops for a diagnostic. */
         if (so->flips == 0)
            mesa_logw("wsi/orbis: sceVideoOutGetFlipStatus -> 0x%08x, presenting without throttle", (unsigned)err);
         ORBIS_BOOK_RETURN(ORBIS_ID_WSI_FLIP_WAIT, orbis_fs_t0, true);
      }
      if (st.numFlipPending < limit)
         ORBIS_BOOK_RETURN(ORBIS_ID_WSI_FLIP_WAIT, orbis_fs_t0, true);
      if (os_time_get_nano() >= deadline) {
         mesa_loge("wsi/orbis: %d flip(s) still pending after 2 s - the display is not consuming them",
                   st.numFlipPending);
         ORBIS_BOOK_RETURN(ORBIS_ID_WSI_FLIP_WAIT, orbis_fs_t0, false);
      }
      sceKernelUsleep(500);
   }
}

/* ⚠ FRAME AGAINST FRAME, BECAUSE THE ARTEFACT IS TRANSIENT AND THE SCENE IS NOT.
 *
 * Two metrics before this one measured the wrong thing, and both errors were mine. The first counted how BLACK each
 * quadrant was - the artefacts are not black. The second counted reversals of the horizontal derivative in the upper
 * third, meaning to catch banding in a smooth sky, and it scored 194 on the MENU and 180 in the world: it was
 * measuring foliage and UI text, not banding, and could not tell the two apart.
 *
 * What is actually known about the artefacts is temporal, not spatial: THEY APPEAR AND VANISH EVERY ONE OR TWO
 * FRAMES. So with a still camera consecutive frames must be IDENTICAL, and any pixel that differs is the artefact -
 * no model of what a sky looks like, nothing to calibrate, and scene detail contributes exactly zero because detail
 * does not move when the camera does not.
 *
 * That is the same principle ps4/radv's rungs are built on (compare identical work against itself), applied in the
 * game where the defect actually lives.
 *
 * ⚠ IT REQUIRES A STILL CAMERA AND SAYS SO IN EVERY LINE. Motion makes every pixel differ legitimately, and then the
 * number means nothing - which is why the line reports the changed fraction rather than a verdict: 0.0% is a clean
 * pair of frames, 99% is a moving camera, and a few percent while standing still is the artefact with a size.
 *
 * ORBIS_FRAME_STATS=1. The comparison band is the upper third, where both remaining artefacts live, and it costs one
 * stored copy of it. */
static void
wsi_orbis_frame_stats(const uint8_t *fb, uint32_t width, uint32_t height, uint32_t pitch)
{
   static int enabled = -1;
   if (enabled < 0)
      enabled = getenv("ORBIS_FRAME_STATS") != NULL ? 1 : 0;
   if (enabled == 0)
      return;

   const uint32_t y0 = height / 8, y1 = height / 2;
   const size_t   band_bytes = (size_t)(y1 - y0) * pitch;
   static uint8_t *prev;
   static size_t   prev_bytes;
   if (prev == NULL || prev_bytes != band_bytes) {
      free(prev);
      prev = malloc(band_bytes);
      prev_bytes = band_bytes;
      if (prev == NULL)
         return;
      memcpy(prev, fb + (size_t)y0 * pitch, band_bytes);
      mesa_logi("wsi/orbis: frame comparison armed on rows %u..%u (%zu KiB); STAND STILL and the changed fraction is "
                "the artefact - a moving camera changes everything and means nothing",
                y0, y1, band_bytes / 1024);
      return;
   }

   const uint8_t *const now = fb + (size_t)y0 * pitch;
   uint64_t changed = 0;
   const size_t px = band_bytes / 4;
   for (size_t i = 0; i < px; ++i)
      if (memcmp(prev + i * 4, now + i * 4, 4) != 0)
         ++changed;
   memcpy(prev, now, band_bytes);

   const unsigned permille = px != 0 ? (unsigned)(changed * 1000 / px) : 0;

   static uint64_t frames, sum, worst, still_frames, still_sum;
   ++frames;
   sum += permille;
   if (permille > worst)
      worst = permille;
   /* A frame is treated as "camera still" when almost nothing changed - under five percent - and those are the only
    * frames whose average means anything. Both averages are printed so the reader can see how many frames were
    * actually usable rather than trusting one number. */
   if (permille < 50) {
      ++still_frames;
      still_sum += permille;
   }
   if ((frames % 32) == 0)
      mesa_logi("wsi/orbis: frame-to-frame change %u/1000 now, mean %llu, worst %llu over %llu frames; STILL frames "
                "%llu with mean %llu/1000 - on a still camera anything above zero is the artefact",
                permille, (unsigned long long)(sum / frames), (unsigned long long)worst,
                (unsigned long long)frames, (unsigned long long)still_frames,
                (unsigned long long)(still_frames != 0 ? still_sum / still_frames : 0));
}

/* ⚠ FLIPS FAIL ON THIS PLATFORM, AND NOTHING IN THIS TREE HAS EVER MADE ONE FAIL ON PURPOSE.
 *
 * They fail for real: 21 flips went out in 110 ms and presentation stopped, which is the measurement
 * wsi_orbis_wait_for_flip_slot exists because of. The WSI above has a whole exit path for that case -
 * it must give the image back rather than keep it, or vkAcquireNextImageKHR runs out of images and
 * the title stops with an empty log - and that path was reached exactly once, on a console, by
 * accident, and has never been reached since. A path that only runs when the display misbehaves is a
 * path that is never tested, and this one was WRONG for the whole life of the port.
 *
 * So it can be asked for: ORBIS_WSI_FAIL_FLIP=N reports failure on every Nth flip, counting the flips
 * this function submits. N=1 fails every one. The failure is injected AT sceVideoOutSubmitFlip and
 * nowhere earlier, because that is where the real one happens - everything before it (the GPU wait,
 * the copy, the throttle) must still run, or the injected failure would be a different event from the
 * one it stands in for.
 *
 * Off unless the variable is set, read once, and announced once - the same shape as every other knob
 * in this file. */
static bool
wsi_orbis_fail_this_flip(uint64_t flip)
{
   static int64_t every = -1;

   if (every < 0) {
      const char *const e = getenv("ORBIS_WSI_FAIL_FLIP");
      every = 0;
      if (e != NULL && *e != '\0') {
         every = (int64_t)strtoll(e, NULL, 10);
         if (every > 0)
            mesa_logw("wsi/orbis: ORBIS_WSI_FAIL_FLIP - every %lld%s flip will be REPORTED AS FAILED. "
                      "The picture is meaningless in this run; what is being tested is what the WSI "
                      "does with an image whose flip did not happen.",
                      (long long)every, every == 1 ? "" : "th");
         else
            every = 0;
      }
   }

   if (every <= 0 || flip == 0 || (flip % (uint64_t)every) != 0)
      return false;

   mesa_logi("wsi/orbis: ORBIS_WSI_FAIL_FLIP - failing flip %llu", (unsigned long long)flip);
   return true;
}

bool
wsi_orbis_scanout_present(struct wsi_orbis_scanout *so, uint32_t index, const void *src,
                          uint32_t src_pitch, uint64_t gpu_seq)
{
   const uint64_t orbis_pr_t0 = os_time_get_nano();
   if (so == NULL || index >= so->count || src == NULL)
      ORBIS_BOOK_RETURN(ORBIS_ID_WSI_PRESENT, orbis_pr_t0, false);

   /* ⚠ THE FRAME LEDGER'S SEGMENT BOUNDARIES. See enum orbis_ledger_id in util/orbis_api_probe.h:
    * the leak is now known to be PER FRAME (~10 KB) rather than per poll, and each mark below bounds
    * exactly one candidate call so that a positive result names one rather than four. */
   orbis_ledger_mark(ORBIS_LG_PRESENT_ENTER);

   /* ⚠ THE GPU FIRST. This copy reads pixels the GPU is writing, and until this line it did not ask whether the
    * writing had finished - which is what put a staircase of tiles over half the menu on every screen change. See
    * ac_orbis_wait_gpu_idle for the photograph and the reasoning; declared here rather than in a header because it
    * is this port's own seam between the WSI and the submission path, and a header would imply a wider contract
    * than one function that waits for one label.
    *
    * Two seconds: far past any frame this hardware can produce, and short enough that a stuck GPU shows a stale
    * frame with a line in the log instead of freezing the title. */
   {
      extern bool ac_orbis_wait_gpu_idle(uint64_t timeout_ns);
      extern bool ac_orbis_wait_gpu_seq(uint64_t seq, uint64_t timeout_ns);
      { const uint64_t t = os_time_get_nano();
        /* ⚠ THIS FRAME, NOT THE NEWEST ONE. With the present deferred by a frame the next frame has
         * already been submitted, so waiting for "idle" waits for the frame AFTER the one about to be
         * copied - which is why deferring alone changed the frame time by nothing at all. */
        if (gpu_seq != 0)
           ac_orbis_wait_gpu_seq(gpu_seq, 2ull * 1000 * 1000 * 1000);
        else
           ac_orbis_wait_gpu_idle(2ull * 1000 * 1000 * 1000);
        const uint64_t d = os_time_get_nano() - t;
        orbis_ledger_mark(ORBIS_LG_AFTER_GPU_IDLE);
        orbis_api_time(ORBIS_ID_WSI_GPU_IDLE, d);
        orbis_api_count(ORBIS_ID_WSI_GPU_IDLE, 1);
        /* ⚠ AND UNCONDITIONALLY, because the two lines above only store under ORBIS_COUNT_API and
         * nothing in this tree ever prints what they collect - the reporter stayed in the old one.
         * This wait is the frame's serialisation point: everything after it on the CPU, including the
         * 8100 KiB scan-out copy, happens only once the GPU has gone fully idle. The submit path's
         * budget line reports these as a window delta. */
        { extern uint64_t orbis_present_wait_ns, orbis_present_waits;
          orbis_present_wait_ns += d;
          orbis_present_waits += 1; }
        /* ⚠ BOOKED TO THE VALUE THAT RENDERED THIS FRAME, which is the one set BEFORE it was recorded - so the flip
         * below happens after the booking, never before it. */
        if (orbis_ab_live) {
           orbis_api_time(orbis_ab_bucket, d);
           orbis_api_count(orbis_ab_bucket, 1);
        }
        { unsigned nb = 0;
          if (orbis_raster_ab(&nb)) { orbis_ab_live = true; orbis_ab_bucket = nb; } } }
   }

   /* ⚠ THE FRAME BOUNDARY SONY'S DRIVER HAS NEVER BEEN TOLD ABOUT.
    *
    * `sceGnmSubmitDone` means, in Sony's own words as quoted in ac_orbis_drm.c, "tell the driver that all command
    * buffers were submitted FOR THIS FRAME". This port calls it NOWHERE: the arm's submit path has
    * `const bool skipped = true` and a comment explaining why - calling it per submit measured 0 ms, then 557 ms,
    * then never returned. That was the right conclusion from the wrong place. A per-submit call is not what the API
    * describes, and this port issues ten to twenty submissions per frame.
    *
    * THE GNM BACKEND IN THIS TREE CALLS IT AFTER EVERY SUBMIT AND RAN THIS GAME AT 30 FPS - because there its
    * submit IS a frame and ends in a flip (`GnmDevice::submitRaw`, "Required after every submit"). So the contract
    * is per FRAME, both renderers agree on that, and only one of them has ever honoured it.
    *
    * Here is the frame boundary, and it is the one place the call cannot stall: `ac_orbis_wait_gpu_idle` above has
    * already established that the GPU has finished everything submitted. The 557 ms was work in flight; there is
    * none left here.
    *
    * ORBIS_SUBMIT_DONE=1 turns it on, because a call this port has never made once should be measured rather than
    * assumed - and `sceGnmAreSubmitsAllowed` is asked first, so a stall is reported instead of suffered. */
   {
      static int enabled = -1;
      if (enabled < 0)
         enabled = getenv("ORBIS_SUBMIT_DONE") != NULL ? 1 : 0;
      if (enabled == 1) {
         const int32_t allowed = sceGnmAreSubmitsAllowed();
         const uint64_t t0 = os_time_get_nano();
         const int32_t rc = sceGnmSubmitDone();
         const uint64_t took = (os_time_get_nano() - t0) / 1000;
         static unsigned said;
         static uint64_t total_us, calls;
         total_us += took;
         ++calls;
         if (said < 8 || (calls % 256) == 0) {
            ++said;
            mesa_logi("wsi/orbis: sceGnmSubmitDone at the frame boundary -> 0x%08x in %llu us (submits allowed said "
                      "%d; mean %llu us over %llu calls)", (unsigned)rc, (unsigned long long)took, (int)allowed,
                      (unsigned long long)(total_us / calls), (unsigned long long)calls);
         }
      }
   }

   orbis_ledger_mark(ORBIS_LG_AFTER_SUBMIT_DONE);

   /* ⚠ BREADCRUMBS FOR THE FIRST FEW FRAMES, because the run this exists for died with no line at all: the log
    * ended after the frame's submission returned, and "we never got here" and "we died inside the copy" looked
    * identical. Bounded to four frames - at 60 Hz an unbounded line per frame IS the log. */
   const bool trace = so->flips < 4;
   if (trace)
      mesa_logi("wsi/orbis: present entered - index %u, src %p, src pitch %u, dst pitch %u, %ux%u", index, src,
                src_pitch, so->width * 4, so->width, so->height);

   if (!wsi_orbis_wait_for_flip_slot(so))
      ORBIS_BOOK_RETURN(ORBIS_ID_WSI_PRESENT, orbis_pr_t0, false);
   orbis_ledger_mark(ORBIS_LG_AFTER_FLIP_SLOT);

   /* ⚠ ROW BY ROW WHEN THE PITCHES DIFFER. WSI picks the image's row pitch and it need not be width*4 - RADV
    * aligns a linear image's pitch to the hardware's requirement. One flat memcpy would then shift every row
    * and produce a sheared picture, which looks like a tiling bug and is not one. */
   const uint32_t dst_pitch = so->width * 4;
   uint8_t *const dst = (uint8_t *)so->map[index];
   /* ⚠ TIME THIS, BECAUSE IT IS THE LEADING SUSPECT FOR THE FRAME RATE AND NOBODY HAS EVER MEASURED IT.
    *
    * Eight megabytes, CPU, every frame, into memory the display controller owns - and CPU writes to scan-out memory
    * on this console are nothing like writes to ordinary RAM. At 150 MB/s that is 53 ms a frame on its own, which is
    * the difference between five frames a second and twenty.
    *
    * This port already knows the fix and deferred it deliberately (ps4/radv/main.cpp: "the zero-copy shape is to
    * register the scan-out buffer itself as the copy target... First the falsifiable step, then the fast one"). The
    * falsifiable step has been done for weeks. What was never done is putting a number on the cost, and everything
    * else about the frame rate has now been eliminated: the GPU idles 3.2 ms a frame, Sony's submit costs
    * microseconds, and turning every diagnostic off changed nothing. */
   /* ⚠ WHAT DOES THIS COPY COST BESIDES ITS OWN 3.4 ms? It reads 8100 KiB and writes it somewhere
    * else, every frame. This console's L2 is 2 MiB per cluster, so the copy evicts FOUR TIMES the
    * whole cache - and the game's simulation, which runs next and holds two thirds of the frame,
    * starts every frame with nothing cached.
    *
    * ORBIS_WSI_NO_COPY=1 skips it. The picture stops updating, which is the point: what is being
    * measured is what the rest of the frame costs when the cache is not being flushed under it. If
    * the simulation gets faster with the picture frozen, the copy is charging far more than the
    * 3.4 ms it is booked for, and the zero-copy present is worth more than it looked. */
   const bool no_copy = !so->owns_buffers || getenv("ORBIS_WSI_NO_COPY") != NULL;
   const uint64_t copy_t0 = os_time_get_nano();
   if (!so->owns_buffers) {
      /* Zero copy: the image the GPU rendered into IS the registered buffer. Nothing to move, and
       * `src` is that same memory - the caller passes it because the fallback path needs it. */
   } else if (no_copy) {
      static bool said;
      if (!said) {
         mesa_logw("wsi/orbis: ORBIS_WSI_NO_COPY - the scan-out copy is SKIPPED. The picture is now "
                   "whatever was there before; only the timings mean anything in this run.");
         said = true;
      }
   } else if (src_pitch == dst_pitch) {
      memcpy(dst, src, (size_t)dst_pitch * so->height);
   } else {
      for (uint32_t y = 0; y < so->height; y++)
         memcpy(dst + (size_t)y * dst_pitch, (const uint8_t *)src + (size_t)y * src_pitch, dst_pitch);
   }

   /* ⚠ IS THE ARTEFACT IN THE IMAGE WE COPY, OR ONLY IN WHAT WE SHOW?
    *
    * A narrow rectangle sits at the TOP LEFT of the final picture - the start of the buffer - with a
    * white-to-green gradient, semi-transparent over the 3D, appearing when the camera looks down. The old
    * tree never had it; this one has had it since the first frame it ever drew, so it is ours.
    *
    * "Top left of the final image" has exactly two causes and they need different fixes: either the strip
    * is already in the swapchain image RADV handed us, or something disturbs the scan-out buffer around
    * our copy. Guessing between them is what this comparison exists to avoid.
    *
    * So the first rows are compared after the copy. dst must equal src byte for byte; this memcpy is the
    * only writer we know of. The first mismatch is reported with both values and its pixel coordinates.
    *
    *   no mismatch      -> the strip is IN the rendered image. WSI is innocent and the search moves up to
    *                       whoever wrote those pixels - or to whatever else shares that memory
    *   a mismatch       -> the scan-out buffer is being disturbed, and the offset says where it starts
    *
    * ORBIS_WSI_VERIFY=<rows>, off by default: it reads back what was just written, which is a cost paid
    * per frame and only worth it while this question is open. */
   /* ⚠ TURN "WHITE AND GREEN" INTO NUMBERS, BECAUSE THE COLOUR IS THE ONLY CLUE LEFT.
    *
    * What is known: the strip is in the image RADV handed us, not in the copy (verified byte for byte over
    * its rows). No live mapping overlaps the swapchain images, and none of the sixteen re-let ranges comes
    * near them - the nearest ends at 0x21b274000 where the images start at 0x21b400000. So neither
    * aliasing nor a stale range explains it.
    *
    * What the maintainer sees does: the strip's content CORRELATES WITH THE SCENE, column by column. A
    * tree in view makes that part of the strip white; open ground makes it green. That is not uninitialised
    * memory and not a constant - it is data derived from the same frame, sitting at the image's origin.
    *
    * A wide, short band of scene-derived data is the shape of a REDUCTION: a hierarchical depth level, an
    * occlusion buffer, something computed at low resolution. Whether that is what it is can be read off the
    * bytes: colour data, depth floats and a packed HiZ do not look alike.
    *
    * So the first pixels of a few rows are printed, with a row from the middle of the screen as a control -
    * a row that is definitely ordinary colour. Comparing the two is the whole point; one row of hex on its
    * own says nothing.
    *
    * ORBIS_WSI_DUMP_BAR=1, once. */
   if (getenv("ORBIS_WSI_DUMP_BAR") != NULL) {
      /* ⚠ NOT THE FIRST FRAME. The first version fired once, on the first present, which is the black
       * loading screen - every pixel came back near zero, including the control, and the sample said
       * nothing at all. ORBIS_WSI_DUMP_BAR is the frame NUMBER to sample, so it can be aimed at a frame
       * that has a world in it. */
      static uint64_t frames;
      static bool     said;
      const uint64_t  want = strtoull(getenv("ORBIS_WSI_DUMP_BAR"), NULL, 10);

      ++frames;
      if (!said && frames >= (want != 0 ? want : 600)) {
         said = true;
         mesa_logi("wsi/orbis: sampling the strip at frame %" PRIu64, frames);
         const uint32_t rows[] = {0, 4, 12, 24, 540};

         for (unsigned r = 0; r < ARRAY_SIZE(rows); ++r) {
            const uint32_t *const px = (const uint32_t *)((const uint8_t *)src + (size_t)rows[r] * src_pitch);
            mesa_logi("wsi/orbis: row %4u px[0..7] = %08x %08x %08x %08x %08x %08x %08x %08x%s", rows[r],
                      px[0], px[1], px[2], px[3], px[4], px[5], px[6], px[7],
                      rows[r] == 540 ? "   <- control, ordinary colour" : "");
            mesa_logi("wsi/orbis: row %4u px[400..407] = %08x %08x %08x %08x %08x %08x %08x %08x", rows[r],
                      px[400], px[401], px[402], px[403], px[404], px[405], px[406], px[407]);
         }
      }
   }

   /* ⚠ DOES THE ENGINE RENDER THOSE PIXELS AT ALL?
    *
    * The scan-out matches the image byte for byte over the rows the strip lives in, so presentation is
    * faithful and the green bar is in what RADV handed us. The application is the same one the old tree
    * ran without this artefact, which leaves two possibilities: something else writes into the swapchain
    * image's memory, or the engine never covers those pixels and we are showing whatever was there.
    *
    * Poison separates them. The image is filled with a known pattern AFTER it has been presented, so the
    * next frame starts from it. Vulkan says the contents of an acquired swapchain image are undefined, so
    * this is legal as well as cheap - an application may not rely on what was there.
    *
    *   the bar turns magenta -> the engine does NOT write those pixels, and the bar is uninitialised
    *                            memory showing through. The question becomes why this layout leaves a hole
    *                            where the old one did not.
    *   the bar stays green   -> something WRITES green there, and it is not the engine's frame. An
    *                            overlapping allocation is then the first suspect, and the arm already
    *                            warns about ranges being re-let.
    *
    * 0xFFFF00FF in A8B8G8R8 is magenta: no scene this game renders is that colour, which is the entire
    * requirement for a poison value.
    *
    * ORBIS_WSI_POISON=1. */
   if (getenv("ORBIS_WSI_POISON") != NULL) {
      uint32_t *const p = (uint32_t *)src;
      const size_t    n = (size_t)src_pitch / 4 * so->height;
      static bool     said;

      for (size_t i = 0; i < n; i++)
         p[i] = 0xFFFF00FFu;

      if (!said) {
         said = true;
         mesa_logi("wsi/orbis: the presented image is poisoned magenta afterwards - anything still magenta "
                   "next frame is a pixel the engine did not write");
      }
   }

   {
      const char *const verify = getenv("ORBIS_WSI_VERIFY");
      const uint32_t    rows = verify != NULL ? (uint32_t)strtoul(verify, NULL, 10) : 0;

      if (rows != 0) {
         const uint32_t n = rows < so->height ? rows : so->height;
         static unsigned said;

         for (uint32_t y = 0; y < n && said < 4; y++) {
            const uint8_t *const d = (const uint8_t *)dst + (size_t)y * dst_pitch;
            const uint8_t *const q = (const uint8_t *)src + (size_t)y * src_pitch;

            for (uint32_t x = 0; x < dst_pitch; x++) {
               if (d[x] == q[x])
                  continue;
               ++said;
               mesa_loge("wsi/orbis: SCAN-OUT DIFFERS FROM THE IMAGE at row %u byte %u (pixel x=%u) - "
                         "scan-out 0x%02x, image 0x%02x. The copy is the only writer this code knows of, "
                         "so something else is reaching the scan-out buffer.",
                         y, x, x / 4, d[x], q[x]);
               break;
            }
         }
         if (said == 0)
            mesa_logi("wsi/orbis: scan-out matches the image over the first %u rows - the strip is in what "
                      "RADV rendered, not in what this code shows",
                      n);
      }
   }

   {
      const uint64_t us = (os_time_get_nano() - copy_t0) / 1000;
      static uint64_t total_us, calls, worst_us;
      total_us += us;
      ++calls;
      if (us > worst_us)
         worst_us = us;
      if (calls <= 4 || (calls % 64) == 0)
         mesa_logi("wsi/orbis: the scan-out copy took %llu us for %u KiB (mean %llu us, worst %llu us over %llu "
                   "frames) - %llu MB/s",
                   (unsigned long long)us, (unsigned)((uint64_t)dst_pitch * so->height / 1024),
                   (unsigned long long)(total_us / calls), (unsigned long long)worst_us,
                   (unsigned long long)calls,
                   us != 0 ? (unsigned long long)((uint64_t)dst_pitch * so->height / us) : 0ull);
   }

   /* ⚠ THE SOURCE, NOT THE DESTINATION, and the numbers are identical either way. `dst` is the GARLIC
    * scan-out buffer - write-combined memory the display controller owns, where a CPU READ is an order of
    * magnitude slower than one from ordinary RAM. This function reads the comparison band twice per frame
    * (compare, then store), so pointing it at `dst` made an opt-in diagnostic cost more than the copy it was
    * measuring alongside. `src` holds the same pixels in the driver's cached ONION arena. */
   wsi_orbis_frame_stats(src, so->width, so->height, src_pitch);

   if (trace)
      mesa_logi("wsi/orbis: present copied %u KiB, flipping index %u",
                (unsigned)((uint64_t)dst_pitch * so->height / 1024), index);

   orbis_ledger_mark(ORBIS_LG_AFTER_COPY);
   int32_t err = sceVideoOutSubmitFlip(so->video, (int32_t)index, ORBIS_VIDEO_OUT_FLIP_VSYNC,
                                       (int64_t)++so->flips);
   orbis_ledger_mark(ORBIS_LG_PRESENT_EXIT);
   /* After the call, not instead of it: the flip really is submitted and the display really does take
    * it, so the state this leaves behind is the state a genuine late failure leaves behind. Only the
    * answer handed back to the WSI is forced. */
   if (err == 0 && wsi_orbis_fail_this_flip(so->flips))
      err = -1;
   if (trace)
      mesa_logi("wsi/orbis: present flip %llu -> 0x%08x", (unsigned long long)so->flips, (unsigned)err);
   if (err != 0) {
      /* ⚠ THE FIRST EIGHT, NOT "the first and every 600th". The old rule logged flip 1 and then nothing until 600,
       * and the flip that actually failed was around the 21st - so the error that ended presentation was invisible
       * while four successful flips were traced in detail. Bounded still, because a failure every frame would be
       * the whole log. */
      static unsigned said;
      if (said < 8) {
         ++said;
         mesa_loge("wsi/orbis: sceVideoOutSubmitFlip(index %u, flip %llu) -> 0x%08x", index,
                   (unsigned long long)so->flips, (unsigned)err);
      }
      ORBIS_BOOK_RETURN(ORBIS_ID_WSI_PRESENT, orbis_pr_t0, false);
   }
   ORBIS_BOOK_RETURN(ORBIS_ID_WSI_PRESENT, orbis_pr_t0, true);
}

void
wsi_orbis_scanout_destroy(struct wsi_orbis_scanout *so)
{
   if (so == NULL)
      return;

   /* ⚠ WAIT FOR THE QUEUE TO EMPTY WHEN THE BUFFERS ARE NOT OURS. With the swapchain's own images
    * registered, every buffer the display still holds is memory the caller is about to free. A flip
    * that has been taken but not yet shown is exactly that case, and this file already says so
    * where it counts flips pending.
    *
    * Bounded, and for the same reason as the flip-slot wait: two seconds with a line naming the
    * counter beats hanging a teardown forever. Closing video-out is what actually stops the scan-out
    * - whether sceVideoOutClose is synchronous has never been measured here - so this drain is the
    * part that can be justified rather than assumed. */
   if (!so->owns_buffers && so->video >= 0) {
      const uint64_t deadline = os_time_get_nano() + WSI_ORBIS_THROTTLE_NS;
      uint32_t pending;
      while ((pending = wsi_orbis_flips_pending(so)) > 0) {
         if (os_time_get_nano() >= deadline) {
            mesa_logw("wsi/orbis: %u flip(s) still pending after 2 s at teardown - the display may "
                      "still be scanning buffers this swapchain is about to free",
                      pending);
            break;
         }
         sceKernelUsleep(500);
      }
   }

   mesa_logi("wsi/orbis: scan-out down after %llu flip(s)", (unsigned long long)so->flips);
   wsi_orbis_release(so);
   free(so);
}
