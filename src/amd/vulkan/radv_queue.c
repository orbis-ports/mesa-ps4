/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 *
 * based in part on anv driver which is:
 * Copyright © 2015 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "util/u_atomic.h"
#include "radv_queue.h"
#include "tools/radv_debug_hang.h"
#include "tools/radv_debug_nir.h"
#include "tools/radv_rmv.h"
#include "radv_buffer.h"
#include "radv_cp_reg_shadowing.h"
#include "radv_cs.h"
#ifdef HAVE_ORBIS_PLATFORM
#include "radv_descriptor_pool.h"
#include "ac_linux_drm.h"
#endif
#include "radv_device_memory.h"
#include "radv_image.h"
#include "vk_common_entrypoints.h"
#include "vk_semaphore.h"
#include "vk_sync.h"

#include "ac_cmdbuf.h"
#include "ac_debug.h"
#include "ac_descriptors.h"

#ifdef HAVE_ORBIS_PLATFORM
/* The tess ring watermark's state. File-static because the fill happens where the ring is created and the
 * read happens between submissions, and threading it through either signature would put a diagnostic into
 * an interface. 0x5a is the arm's own decoy byte - one poison value across this port, so a stray pattern in
 * a dump is recognisable wherever it turns up. */
#define ORBIS_TESS_RING_POISON 0x5a
static uint8_t *orbis_tess_ring_map;
static uint64_t orbis_tess_ring_size;
static uint32_t orbis_tess_ring_sized_at; /* what ac_gpu_info computed, for the ratio */
#endif


enum radeon_ctx_priority
radv_get_queue_global_priority(const VkDeviceQueueGlobalPriorityCreateInfo *pObj)
{
   /* Default to MEDIUM when a specific global priority isn't requested */
   if (!pObj)
      return RADEON_CTX_PRIORITY_MEDIUM;

   return vk_to_radeon_priority(pObj->globalPriority);
}

static VkResult
radv_sparse_buffer_bind_memory(struct radv_device *device, const VkSparseBufferMemoryBindInfo *bind)
{
   VK_FROM_HANDLE(radv_buffer, buffer, bind->buffer);
   VkResult result = VK_SUCCESS;

   struct radv_device_memory *mem = NULL;
   VkDeviceSize resourceOffset = 0;
   VkDeviceSize size = 0;
   VkDeviceSize memoryOffset = 0;
   for (uint32_t i = 0; i < bind->bindCount; ++i) {
      struct radv_device_memory *cur_mem = NULL;

      if (bind->pBinds[i].memory != VK_NULL_HANDLE)
         cur_mem = radv_device_memory_from_handle(bind->pBinds[i].memory);
      if (i && mem == cur_mem) {
         if (mem) {
            if (bind->pBinds[i].resourceOffset == resourceOffset + size &&
                bind->pBinds[i].memoryOffset == memoryOffset + size) {
               size += bind->pBinds[i].size;
               continue;
            }
         } else {
            if (bind->pBinds[i].resourceOffset == resourceOffset + size) {
               size += bind->pBinds[i].size;
               continue;
            }
         }
      }
      if (size) {
         result = radv_bo_virtual_bind(device, &buffer->vk.base, buffer->bo, resourceOffset, size, mem ? mem->bo : NULL,
                                       memoryOffset);
         if (result != VK_SUCCESS)
            return result;
      }
      mem = cur_mem;
      resourceOffset = bind->pBinds[i].resourceOffset;
      size = bind->pBinds[i].size;
      memoryOffset = bind->pBinds[i].memoryOffset;
   }
   if (size) {
      result = radv_bo_virtual_bind(device, &buffer->vk.base, buffer->bo, resourceOffset, size, mem ? mem->bo : NULL,
                                    memoryOffset);
   }

   return result;
}

static VkResult
radv_sparse_image_opaque_bind_memory(struct radv_device *device, const VkSparseImageOpaqueMemoryBindInfo *bind)
{
   VK_FROM_HANDLE(radv_image, image, bind->image);
   VkResult result;

   for (uint32_t i = 0; i < bind->bindCount; ++i) {
      struct radv_device_memory *mem = NULL;

      if (bind->pBinds[i].memory != VK_NULL_HANDLE)
         mem = radv_device_memory_from_handle(bind->pBinds[i].memory);

      result = radv_bo_virtual_bind(device, &image->vk.base, image->bindings[0].bo, bind->pBinds[i].resourceOffset,
                                    bind->pBinds[i].size, mem ? mem->bo : NULL, bind->pBinds[i].memoryOffset);
      if (result != VK_SUCCESS)
         return result;
   }

   return VK_SUCCESS;
}

static VkResult
radv_sparse_image_bind_memory(struct radv_device *device, const VkSparseImageMemoryBindInfo *bind)
{
   VK_FROM_HANDLE(radv_image, image, bind->image);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radeon_surf *surface = &image->planes[0].surface;
   uint32_t bs = vk_format_get_blocksize(image->vk.format);
   VkResult result;

   for (uint32_t i = 0; i < bind->bindCount; ++i) {
      struct radv_device_memory *mem = NULL;
      uint64_t offset, depth_pitch;
      uint32_t pitch;
      uint64_t mem_offset = bind->pBinds[i].memoryOffset;
      const uint32_t layer = bind->pBinds[i].subresource.arrayLayer;
      const uint32_t level = bind->pBinds[i].subresource.mipLevel;

      VkExtent3D bind_extent = bind->pBinds[i].extent;
      bind_extent.width = DIV_ROUND_UP(bind_extent.width, vk_format_get_blockwidth(image->vk.format));
      bind_extent.height = DIV_ROUND_UP(bind_extent.height, vk_format_get_blockheight(image->vk.format));

      VkOffset3D bind_offset = bind->pBinds[i].offset;
      bind_offset.x /= vk_format_get_blockwidth(image->vk.format);
      bind_offset.y /= vk_format_get_blockheight(image->vk.format);

      if (bind->pBinds[i].memory != VK_NULL_HANDLE)
         mem = radv_device_memory_from_handle(bind->pBinds[i].memory);

      if (pdev->info.gfx_level >= GFX9) {
         offset = surface->u.gfx9.surf_slice_size * layer + surface->u.gfx9.prt_level_offset[level];
         pitch = surface->u.gfx9.prt_level_pitch[level];
         depth_pitch = surface->u.gfx9.surf_slice_size;
      } else {
         depth_pitch = surface->u.legacy.level[level].slice_size_dw * 4;
         offset = (uint64_t)surface->u.legacy.level[level].offset_256B * 256 + depth_pitch * layer;
         pitch = surface->u.legacy.level[level].nblk_x;
      }

      offset +=
         bind_offset.z * depth_pitch + ((uint64_t)bind_offset.y * pitch * surface->prt_tile_depth +
                                        (uint64_t)bind_offset.x * surface->prt_tile_height * surface->prt_tile_depth) *
                                          bs;

      uint32_t aligned_extent_width = align(bind_extent.width, surface->prt_tile_width);
      uint32_t aligned_extent_height = align(bind_extent.height, surface->prt_tile_height);
      uint32_t aligned_extent_depth = align(bind_extent.depth, surface->prt_tile_depth);

      bool whole_subres = (bind_extent.height <= surface->prt_tile_height || aligned_extent_width == pitch) &&
                          (bind_extent.depth <= surface->prt_tile_depth ||
                           (uint64_t)aligned_extent_width * aligned_extent_height * bs == depth_pitch);

      if (whole_subres) {
         uint64_t size = (uint64_t)aligned_extent_width * aligned_extent_height * aligned_extent_depth * bs;
         result = radv_bo_virtual_bind(device, &image->vk.base, image->bindings[0].bo, offset, size,
                                       mem ? mem->bo : NULL, mem_offset);
         if (result != VK_SUCCESS)
            return result;
      } else {
         uint32_t img_y_increment = pitch * bs * surface->prt_tile_depth;
         uint32_t mem_y_increment = aligned_extent_width * bs * surface->prt_tile_depth;
         uint64_t mem_z_increment = (uint64_t)aligned_extent_width * aligned_extent_height * bs;
         uint64_t size = mem_y_increment * surface->prt_tile_height;
         for (unsigned z = 0; z < bind_extent.depth;
              z += surface->prt_tile_depth, offset += depth_pitch * surface->prt_tile_depth) {
            for (unsigned y = 0; y < bind_extent.height; y += surface->prt_tile_height) {
               uint64_t bo_offset = offset + (uint64_t)img_y_increment * y;

               result = radv_bo_virtual_bind(device, &image->vk.base, image->bindings[0].bo, bo_offset, size,
                                             mem ? mem->bo : NULL,
                                             mem_offset + (uint64_t)mem_y_increment * y + mem_z_increment * z);
               if (result != VK_SUCCESS)
                  return result;
            }
         }
      }
   }

   return VK_SUCCESS;
}

static VkResult
radv_queue_submit_bind_sparse_memory(struct radv_device *device, struct vk_queue_submit *submission)
{
   for (uint32_t i = 0; i < submission->buffer_bind_count; ++i) {
      VkResult result = radv_sparse_buffer_bind_memory(device, submission->buffer_binds + i);
      if (result != VK_SUCCESS)
         return result;
   }

   for (uint32_t i = 0; i < submission->image_opaque_bind_count; ++i) {
      VkResult result = radv_sparse_image_opaque_bind_memory(device, submission->image_opaque_binds + i);
      if (result != VK_SUCCESS)
         return result;
   }

   for (uint32_t i = 0; i < submission->image_bind_count; ++i) {
      VkResult result = radv_sparse_image_bind_memory(device, submission->image_binds + i);
      if (result != VK_SUCCESS)
         return result;
   }

   return VK_SUCCESS;
}

static VkResult
radv_queue_submit_empty(struct radv_queue *queue, struct vk_queue_submit *submission)
{
   struct radv_device *device = radv_queue_device(queue);
   struct radeon_winsys_ctx *ctx = queue->hw_ctx;
   struct radv_winsys_submit_info submit = {
      .ip_type = radv_queue_ring(queue),
      .queue_index = queue->vk.index_in_family,
      .secure = submission->is_protected,
   };

   return device->ws->cs_submit(ctx, &submit, submission->wait_count, submission->waits, submission->signal_count,
                                submission->signals);
}

static void
radv_set_ring_buffer(const struct radv_physical_device *pdev, struct radeon_winsys_bo *bo, uint32_t offset,
                     uint32_t ring_size, bool add_tid, bool swizzle_enable, bool oob_select_raw, uint32_t element_size,
                     uint32_t index_stride, uint32_t desc[4])
{
   const uint8_t oob_select = oob_select_raw ? V_008F0C_OOB_SELECT_RAW : V_008F0C_OOB_SELECT_DISABLED;
   const uint64_t va = radv_buffer_get_va(bo) + offset;
   const struct ac_buffer_state ac_state = {
      .va = va,
      .size = ring_size,
      .format = PIPE_FORMAT_R32_FLOAT,
      .swizzle =
         {
            PIPE_SWIZZLE_X,
            PIPE_SWIZZLE_Y,
            PIPE_SWIZZLE_Z,
            PIPE_SWIZZLE_W,
         },
      .swizzle_enable = swizzle_enable,
      .element_size = element_size,
      .index_stride = index_stride,
      .add_tid = add_tid,
      .gfx10_oob_select = oob_select,
      .has_desc_resource_level = pdev->info.compiler_info.has_desc_resource_level,
   };

   ac_build_buffer_descriptor(pdev->info.gfx_level, &ac_state, desc);
}

static void
radv_fill_shader_rings(struct radv_device *device, uint32_t *desc, struct radeon_winsys_bo *scratch_bo,
                       uint32_t esgs_ring_size, struct radeon_winsys_bo *esgs_ring_bo, uint32_t gsvs_ring_size,
                       struct radeon_winsys_bo *gsvs_ring_bo, struct radeon_winsys_bo *tess_rings_bo,
                       struct radeon_winsys_bo *task_rings_bo, struct radeon_winsys_bo *mesh_scratch_ring_bo,
                       struct radeon_winsys_bo *ge_rings_bo)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);

   if (scratch_bo) {
      uint64_t scratch_va = radv_buffer_get_va(scratch_bo);
      uint32_t rsrc1 = S_008F04_BASE_ADDRESS_HI(scratch_va >> 32);

      if (pdev->info.gfx_level >= GFX11)
         rsrc1 |= S_008F04_SWIZZLE_ENABLE_GFX11(1);
      else
         rsrc1 |= S_008F04_SWIZZLE_ENABLE_GFX6(1);

      desc[0] = scratch_va;
      desc[1] = rsrc1;
   }

   desc += 4;

   if (esgs_ring_bo) {
      /* stride 0, num records - size, add tid, swizzle, elsize4,
         index stride 64 */
      radv_set_ring_buffer(pdev, esgs_ring_bo, 0, esgs_ring_size, true, true, false, 1, 3, &desc[0]);

      /* GS entry for ES->GS ring */
      /* stride 0, num records - size, elsize0,
         index stride 0 */
      radv_set_ring_buffer(pdev, esgs_ring_bo, 0, esgs_ring_size, false, false, false, 0, 0, &desc[4]);
   }

   desc += 8;

   if (gsvs_ring_bo) {
      /* VS entry for GS->VS ring */
      /* stride 0, num records - size, elsize0,
         index stride 0 */
      radv_set_ring_buffer(pdev, gsvs_ring_bo, 0, gsvs_ring_size, false, false, false, 0, 0, &desc[0]);

      /* stride gsvs_itemsize, num records 64
         elsize 4, index stride 16 */
      /* shader will patch stride and desc[2] */
      radv_set_ring_buffer(pdev, gsvs_ring_bo, 0, 0, true, true, false, 1, 1, &desc[4]);
   }

   desc += 8;

   if (tess_rings_bo) {
      radv_set_ring_buffer(pdev, tess_rings_bo, pdev->info.tess_offchip_ring_size, pdev->info.tess_factor_ring_size,
                           false, false, true, 0, 0, &desc[0]);

      radv_set_ring_buffer(pdev, tess_rings_bo, 0, pdev->info.tess_offchip_ring_size, false, false, true, 0, 0,
                           &desc[4]);
   }

   desc += 8;

   if (task_rings_bo) {
      radv_set_ring_buffer(pdev, task_rings_bo, pdev->task_info.draw_ring_offset,
                           pdev->task_info.num_entries * AC_TASK_DRAW_ENTRY_BYTES, false, false, false, 0, 0, &desc[0]);

      radv_set_ring_buffer(pdev, task_rings_bo, pdev->task_info.payload_ring_offset,
                           pdev->task_info.num_entries * pdev->task_info.payload_entry_size, false, false, false, 0, 0,
                           &desc[4]);
   }

   desc += 8;

   if (mesh_scratch_ring_bo) {
      radv_set_ring_buffer(pdev, mesh_scratch_ring_bo, 0, AC_MESH_SCRATCH_NUM_ENTRIES * AC_MESH_SCRATCH_ENTRY_BYTES,
                           false, false, false, 0, 0, &desc[0]);
   }

   desc += 4;

   if (ge_rings_bo) {
      assert(pdev->info.gfx_level >= GFX11);

      ac_build_attr_ring_descriptor(pdev->info.gfx_level, pdev->info.compiler_info.has_desc_resource_level,
                                    radv_buffer_get_va(ge_rings_bo), pdev->info.total_attribute_pos_prim_ring_size, 0,
                                    &desc[0]);
   }

   desc += 4;

   /* add sample positions after all rings */
   memcpy(desc, device->sample_locations_1x, 8);
   desc += 2;
   memcpy(desc, device->sample_locations_2x, 16);
   desc += 4;
   memcpy(desc, device->sample_locations_4x, 32);
   desc += 8;
   memcpy(desc, device->sample_locations_8x, 64);
}

/**
 * ⚠ THIS HARDWARE WRITES PAST THE RING SIZE IT WAS TOLD, so the allocation has to be bigger than the
 * register. Two runs of the same 349 tests, differing in nothing else:
 *
 *   VGT_GSVS_RING_SIZE = S, buffer S     GPU Protection fault, client TC0, access Write,
 *                                        "Unmapped page access" - the geometry shader off the end
 *   VGT_GSVS_RING_SIZE = S, buffer 16*S  349 / 349
 *
 * The register carries the SAME value in both, so this is the allocation and nothing else. RADV
 * computes one number and uses it for both, which is right everywhere amdgpu runs; here the declared
 * size is fine and the buffer behind it is not.
 *
 * ⚠ AND THE PADDING IS A MARGIN, NOT A DERIVATION. Four blanket multipliers were spent moving this
 * boundary before the two were separated - two carried geometry.basic, four carried multiview, and
 * neither carried this - so the honest reading is that nothing here predicts how far past the end
 * the hardware goes. ORBIS_GS_RING_PAD is the knob for the next family that outgrows it, and its
 * existence is the admission that one may.
 */
static uint32_t
radv_orbis_gs_ring_bytes(uint32_t size, bool is_esgs)
{
#ifdef HAVE_ORBIS_PLATFORM
   /* ⚠ AND THE SLACK IS BOUNDED, BECAUSE THE FIRST VERSION OF IT WAS NOT AND COST A RUN.
    *
    * A bare multiplier here compounds with the size scale and lands on a value that is already
    * clamped to the hardware maximum - MIN2(gsvs_ring_size, 63.999 MB * num_se) upstream - so 16x
    * of that asked for 2147442688 bytes, the arena said "out of device VA", the device was lost and
    * 328 tests failed behind it. The overrun this absorbs was measured on SMALL rings; nothing says
    * it grows with the ring, and everything says the arena will not survive assuming it does. */
   static int pad = -1;
   static int esgs_pad = -1;
   static int cap_mb = -1;

   if (pad < 0) {
      const char *const e = getenv("ORBIS_GS_RING_PAD");
      const char *const c = getenv("ORBIS_GS_RING_PAD_CAP_MB");
      pad = (e != NULL && *e != '\0') ? MAX2(atoi(e), 1) : 16;
      cap_mb = (c != NULL && *c != '\0') ? MAX2(atoi(c), 0) : 512;

      /* ⚠ A SEPARATE PAD FOR THE ES-TO-GS RING, BECAUSE ONE PAD FOR BOTH CANNOT ASK THE QUESTION.
       *
       * Run 53 raised ORBIS_GS_RING_PAD to 32 to test the one term the elimination had left - the ESGS
       * allocation - and never got to test it: the GSVS rings went to 160 MB, 300 MB and 512 MB, the arena
       * ran out of device VA, BOs were handed addresses that live BOs still held, and the two geometry.basic
       * tests failed with difference 17883.6 and 21818.5 - the SAME numbers run 51 produced by starving the
       * declared size. Two very different causes, one indistinguishable picture, which is what an experiment
       * looks like when it never establishes its own condition.
       *
       * The ESGS rings are small - 512 KB to 1.5 MB requested against GSVS's 5 MB to 64 MB - so padding them
       * hard costs tens of megabytes while padding GSVS hard costs the device. Splitting the knob is what
       * makes the remaining term reachable at all.
       *
       * ORBIS_GS_RING_PAD_ESGS, defaulting to whatever ORBIS_GS_RING_PAD is, so nothing changes unless it
       * is asked for. */
      const char *const ee = getenv("ORBIS_GS_RING_PAD_ESGS");
      esgs_pad = (ee != NULL && *ee != '\0') ? MAX2(atoi(ee), 1) : pad;

      mesa_logi("radv/orbis: GS ring allocations padded %dx (ESGS %dx), TOTAL capped at %d MiB - this CP "
                "writes past the ring size it is given, and an uncapped pad loses the device instead.",
                pad, esgs_pad, cap_mb);
   }

   const int this_pad = is_esgs ? esgs_pad : pad;

   /* ⚠ THE CEILING IS ON THE TOTAL, NOT THE SLACK, because the arena is what breaks and the arena
    * sees a total. Capping the slack was guessed at three times - 64 MiB starved the binding-model
    * geometry tests, uncapped asked 2 GB and lost the device - while the log carries what this arena
    * actually survived:
    *
    *   301,989,888 B  allocated, run completed
    *   671,086,080 B  allocated, device lost shortly after
    *
    * So 256 MiB is a number with a measurement under it rather than a fourth guess. A ring larger
    * than the cap on its own is never shrunk: that would trade a fault for a wrong picture. */
   const uint64_t slack = (uint64_t)size * (uint32_t)(this_pad - 1);
   const uint64_t ceiling = MAX2((uint64_t)size, (uint64_t)cap_mb * 1024 * 1024);
   const uint64_t total = MIN2(MIN2((uint64_t)size + slack, ceiling), (uint64_t)UINT32_MAX);

   /* ⚠ PRINT THE NUMBERS, BECAUSE THREE RUNS WERE SPENT MOVING MULTIPLIERS WITHOUT KNOWING ONE.
    * Every configuration tried so far fixed one family and broke another - a 64 MiB cap starves the
    * binding-model geometry tests while an uncapped pad asks for 2 GB and loses the device - and
    * none of that reasoning had a single real ring size in it. Once per distinct size, so a run
    * carries the magnitudes instead of the next guess. */
   {
      static uint32_t seen[8];
      static unsigned n;
      bool known = false;
      for (unsigned i = 0; i < n; i++)
         known |= seen[i] == size;
      if (!known) {
         if (n < ARRAY_SIZE(seen))
            seen[n++] = size;
         mesa_logi("radv/orbis: GS ring %u B requested -> %" PRIu64 " B allocated (slack %" PRIu64
                   " B%s)",
                   size, total, slack, total == ceiling && slack ? ", AT THE CAP" : "");
      }
   }

   return (uint32_t)total;
#else
   return size;
#endif
}

static void
radv_emit_gs_ring_sizes(struct radv_device *device, struct radv_cmd_stream *cs, struct radeon_winsys_bo *esgs_ring_bo,
                        uint32_t esgs_ring_size, struct radeon_winsys_bo *gsvs_ring_bo, uint32_t gsvs_ring_size)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);

   if (!esgs_ring_bo && !gsvs_ring_bo)
      return;

   if (esgs_ring_bo)
      radv_cs_add_buffer(device->ws, cs->b, esgs_ring_bo);

   if (gsvs_ring_bo)
      radv_cs_add_buffer(device->ws, cs->b, gsvs_ring_bo);

   uint32_t esgs_reg = esgs_ring_size >> 8;
   uint32_t gsvs_reg = gsvs_ring_size >> 8;

#ifdef HAVE_ORBIS_PLATFORM
   /* ⚠ DOES THIS REGISTER REACH THE VGT AT ALL? ASK BY ITS EFFECT, BECAUSE READING IT IS IMPOSSIBLE.
    *
    * Run 50 settled the reading question and settled it against us: VGT_PRIMITIVE_TYPE, four bytes from
    * VGT_GSVS_RING_SIZE and written by this driver on every draw, reads back 0x00000000 after two draws have
    * executed. The COPY_DATA route is BLIND TO THE WHOLE VGT USER-CONFIG BLOCK - which is also, at last, why
    * the three tessellation registers answer zero. One mechanism, two mysteries; and it means every zero this
    * port has read from that block is a non-answer rather than a measurement.
    *
    * So stop reading and watch what happens instead. This divides ONLY THE REGISTER, leaving the allocation
    * and every other term alone, which turns reachability into an image comparison:
    *
    *   the images get worse    the register reaches the VGT, the ring wraps where we tell it to, and the ring
    *                           defect is about the VALUE we compute
    *   nothing changes at all  the register is inert on this part, the VGT wraps on something we do not
    *                           control, and no multiplier was ever going to fix it
    *
    * ⚠ DIVIDE, DO NOT MULTIPLY. A larger declared size than the allocation is how the hardware gets sent past
    * the end of a buffer, which is the fault that kills the device. Shrinking is the safe direction and it is
    * the sensitive one: the baseline already passes geometry.basic.output_10 and output_128.
    *
    * ORBIS_GS_RING_REG_DIV=8 is the experiment; unset is the shipped behaviour. */
   {
      static int div = -1;
      if (div < 0) {
         const char *const e = getenv("ORBIS_GS_RING_REG_DIV");
         div = (e != NULL && *e != '\0') ? MAX2(atoi(e), 1) : 1;
      }
      if (div > 1) {
         const uint32_t esgs_was = esgs_reg, gsvs_was = gsvs_reg;
         /* Never to zero: a zero-sized ring is a different experiment and probably an immediate hang. */
         esgs_reg = MAX2(esgs_reg / (uint32_t)div, 1u);
         gsvs_reg = MAX2(gsvs_reg / (uint32_t)div, 1u);
         mesa_logi("radv/orbis: ORBIS_GS_RING_REG_DIV=%d - DECLARING ESGS 0x%08x -> 0x%08x, GSVS 0x%08x -> "
                   "0x%08x while the allocations stay as they are. If the geometry images do not change, this "
                   "register does not reach the VGT.",
                   div, esgs_was, esgs_reg, gsvs_was, gsvs_reg);
      }
   }

   /* ⚠ THE OTHER DIRECTION, AND IT SEPARATES THE TWO FAULTS FOR THE FIRST TIME.
    *
    * Run 51 answered the reachability question: dividing the declared size by eight took
    * geometry.basic.output_10 and output_128 from Pass to image differences of 17883 and 21818 against a
    * threshold of 0.0015. The register reaches the VGT and the VGT wraps where we tell it to.
    *
    * That makes ORBIS_GS_RING_SCALE the wrong instrument for what remains, because it moves the DECLARED size
    * and the ALLOCATION together - which is exactly why eight configurations never separated
    *
    *     the ring wraps too early    -> it overwrites itself   -> WRONG IMAGES
    *     the allocation is short     -> something writes past  -> GPU FAULT
    *
    * This moves only the first. If multiview's images come right, the declared size was what was short and
    * the allocation was never the point; if nothing changes, the declaration was already adequate and the
    * fault lives in the allocation.
    *
    * ⚠ AND IT IS CLAMPED TO WHAT WAS ACTUALLY ALLOCATED, which is the whole reason it is safe to raise a
    * declared ring size at all. radv_orbis_gs_ring_bytes() is deterministic, so the padded allocation can be
    * recomputed here exactly; declaring more than that is how the hardware gets sent past the end of a buffer
    * and is refused rather than trusted to the caller. With the shipped 16x padding, MUL=2 still leaves the
    * declaration eight times inside the buffer.
    *
    * ORBIS_GS_RING_REG_MUL=2. */
   {
      static int mul = -1;
      if (mul < 0) {
         const char *const e = getenv("ORBIS_GS_RING_REG_MUL");
         mul = (e != NULL && *e != '\0') ? MAX2(atoi(e), 1) : 1;
      }
      if (mul > 1) {
         const uint32_t esgs_cap = (uint32_t)(radv_orbis_gs_ring_bytes(esgs_ring_size, true) >> 8);
         const uint32_t gsvs_cap = (uint32_t)(radv_orbis_gs_ring_bytes(gsvs_ring_size, false) >> 8);
         const uint32_t esgs_was = esgs_reg, gsvs_was = gsvs_reg;

         esgs_reg = MIN2(esgs_reg * (uint32_t)mul, esgs_cap);
         gsvs_reg = MIN2(gsvs_reg * (uint32_t)mul, gsvs_cap);

         mesa_logi("radv/orbis: ORBIS_GS_RING_REG_MUL=%d - DECLARING ESGS 0x%08x -> 0x%08x (allocation allows "
                   "0x%08x), GSVS 0x%08x -> 0x%08x (allows 0x%08x). The ALLOCATIONS are untouched, so this "
                   "asks whether the DECLARED size was what multiview was short of.%s",
                   mul, esgs_was, esgs_reg, esgs_cap, gsvs_was, gsvs_reg, gsvs_cap,
                   (esgs_reg == esgs_cap || gsvs_reg == gsvs_cap)
                      ? "  ⚠ ONE OF THEM HIT THE ALLOCATION CEILING - the factor asked for is not the factor "
                        "applied, and a null result means less than it looks."
                      : "");
      }
   }
#endif

   /* ⚠ THIS USED TO SIT OUTSIDE THE PLATFORM SEAM, saying "the note is a no-op elsewhere". It is not:
    * ac_orbis_note_gs_ring_sizes is declared in ac_linux_drm.h, which this file includes only under
    * HAVE_ORBIS_PLATFORM, and defined only in ac_orbis_drm.c, which only -Dplatforms=orbis builds. So
    * every other configuration of this tree failed on -Werror=implicit-function-declaration, and would
    * have failed at the link even with a prototype.
    *
    * The instinct behind it was sound - a call site inside an #ifdef has broken the host link twice in
    * this file's history - but the cure for that is a stub that really exists, and there is none. The
    * other four ac_orbis_note_* call sites in RADV are all inside the seam already; this one was the
    * odd one out rather than the pattern. */
#ifdef HAVE_ORBIS_PLATFORM
   ac_orbis_note_gs_ring_sizes(esgs_reg, gsvs_reg);
#endif

   radeon_begin(cs);

   if (pdev->info.gfx_level >= GFX7) {
      radeon_set_uconfig_reg_seq(R_030900_VGT_ESGS_RING_SIZE, 2);
      radeon_emit(esgs_reg);
      radeon_emit(gsvs_reg);
   } else {
      radeon_set_config_reg_seq(R_0088C8_VGT_ESGS_RING_SIZE, 2);
      radeon_emit(esgs_reg);
      radeon_emit(gsvs_reg);
   }

   radeon_end();
}

static void
radv_emit_tess_factor_ring(struct radv_device *device, struct radv_cmd_stream *cs,
                           struct radeon_winsys_bo *tess_rings_bo)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   uint64_t va;

   if (!tess_rings_bo)
      return;

   va = radv_buffer_get_va(tess_rings_bo);

   radv_cs_add_buffer(device->ws, cs->b, tess_rings_bo);

#ifdef HAVE_ORBIS_PLATFORM
   /* ⚠ THE LINE THAT MAKES THE FIX CHECKABLE, and without it a run that programmed the rings and a run that
    * inherited Sony's look identical. A log carrying this sentence took the registers from a BO of ours; a
    * log without it left 0xff0000000 in place. It is also the base the remaining write faults have to be
    * compared against, so it prints the extent rather than just the address.
    *
    * ac_emit_cp_tess_rings puts the FACTOR ring at va + tess_offchip_ring_size and writes va >> 8, so both
    * numbers below are what the register will actually carry. */
   {
      /* p_atomic rather than a plain flag: this runs on whatever thread built the preamble, and the arm
       * spent a commit today removing eighteen copies of the racy version. */
      static unsigned said;
      if (p_atomic_read(&said) < 1 && p_atomic_inc_return(&said) <= 1) {
         mesa_logi("radv/orbis: tess rings are OURS - 0x%" PRIx64 "..0x%" PRIx64 " (offchip %u B, factor %u B, "
                   "total %u B); VGT_TF_MEMORY_BASE <- 0x%" PRIx64 ", replacing Sony's 0x0ff00000",
                   va, va + pdev->info.total_tess_ring_size, pdev->info.tess_offchip_ring_size,
                   pdev->info.tess_factor_ring_size, pdev->info.total_tess_ring_size,
                   (va + pdev->info.tess_offchip_ring_size) >> 8);

         /* ⚠ THE NUMBERS THE OVERRUN IS READ AGAINST. A write fault inside this ring is only interpretable
          * against the buffer count and the stride: the fault at 0x212a50000 meant "offchip buffer 74" only
          * because 32768 and 72 were known. Printing them turns the next klog address into an index without a
          * spreadsheet, and states what went into OFFCHIP_BUFFERING so a run cannot be mistaken for one that
          * carried the other value. */
         const unsigned stride = pdev->info.hs_offchip_workgroup_dw_size * 4;
         mesa_logi("radv/orbis: offchip stride %u B, %u buffer(s) allocated, %u shader engine(s); "
                   "VGT_HS_OFFCHIP_PARAM <- 0x%08x. A write fault at ring+N*%u is offchip buffer N.",
                   stride, stride ? pdev->info.tess_offchip_ring_size / stride : 0, pdev->info.max_se,
                   pdev->info.hs_offchip_param, stride);
      }
   }
#endif

#ifdef HAVE_ORBIS_PLATFORM
   /* ⚠ MOVE OUR OWN RING ON PURPOSE, BECAUSE THE TWO KINDS OF TRAFFIC DISAGREE ABOUT WHERE IT IS.
    *
    * Measured, both halves: the WRITES follow our base - correcting the offchip count from 72 to 256 moved
    * the factor ring from 0x212a40000 to 0x213000000 and the write faults moved with it, keeping their
    * offsets from the new base. The READS do not - they land at 0xff0000000 plus small offsets no matter
    * what we program, and sceGnmGetTheTessellationFactorRingBufferBaseAddress() answers 0xff0000000 in this
    * very process, first-hand rather than quoted.
    *
    * Two rings, then. But "the reads ignore our base" is an inference from two fixes that each moved one
    * thing; nothing has moved the base ON ITS OWN, with every other number held still. ORBIS_TF_BASE_BIAS
    * does exactly that: it adds a known offset to the address we program, changing nothing else.
    *
    *   write faults shift by the bias   -> writes use our base, confirmed by a deliberate move rather than
    *                                       by a side effect of a sizing change
    *   read faults stay at 0xff0000000  -> the reads genuinely never see our base, and the question becomes
    *                                       which register or descriptor still carries Sony's
    *   read faults shift too            -> they DO use our base, and 0xff0000000 was reached some other way
    *
    * ⚠ THE RING STAYS WHERE IT WAS ALLOCATED. Only the ADDRESS PROGRAMMED moves, so the hardware is pointed
    * at memory we own but did not intend for it - which is the whole point, and why this is a diagnostic
    * with a knob rather than a default. */
   uint64_t orbis_tf_va = va + pdev->info.tess_offchip_ring_size;
   {
      const char *const bias_s = getenv("ORBIS_TF_BASE_BIAS");
      const uint64_t bias = bias_s != NULL ? strtoull(bias_s, NULL, 0) : 0;
      if (bias != 0) {
         orbis_tf_va += bias;
         mesa_logw("radv/orbis: ORBIS_TF_BASE_BIAS moves the programmed factor-ring base by 0x%" PRIx64
                   " to 0x%" PRIx64 " while the allocation stays put. Tessellation output goes somewhere we "
                   "own but did not mean; the point is only whether the fault addresses follow.",
                   bias, orbis_tf_va);
      }
   }
   ac_orbis_note_tf_base((uint32_t)(orbis_tf_va >> 8));
#endif

   ac_emit_cp_tess_rings(cs->b, &pdev->info, va);
}

static VkResult
radv_initialise_task_control_buffer(struct radv_device *device, struct radeon_winsys_bo *task_rings_bo)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   uint32_t *ptr = (uint32_t *)radv_buffer_map(device->ws, task_rings_bo);
   if (!ptr)
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   const uint32_t num_entries = pdev->task_info.num_entries;
   const uint64_t task_va = radv_buffer_get_va(task_rings_bo);
   const uint64_t task_draw_ring_va = task_va + pdev->task_info.draw_ring_offset;
   assert((task_draw_ring_va & 0xFFFFFF00) == (task_draw_ring_va & 0xFFFFFFFF));

   /* 64-bit write_ptr */
   ptr[0] = num_entries;
   ptr[1] = 0;
   /* 64-bit read_ptr */
   ptr[2] = num_entries;
   ptr[3] = 0;
   /* 64-bit dealloc_ptr */
   ptr[4] = num_entries;
   ptr[5] = 0;
   /* num_entries */
   ptr[6] = num_entries;
   /* 64-bit draw ring address */
   ptr[7] = task_draw_ring_va;
   ptr[8] = task_draw_ring_va >> 32;

   device->ws->buffer_unmap(device->ws, task_rings_bo, false);
   return VK_SUCCESS;
}

static void
radv_emit_task_rings(struct radv_device *device, struct radv_cmd_stream *cs, struct radeon_winsys_bo *task_rings_bo,
                     bool compute)
{
   if (!task_rings_bo)
      return;

   const uint64_t task_ctrlbuf_va = radv_buffer_get_va(task_rings_bo);
   assert(util_is_aligned(task_ctrlbuf_va, 256));
   radv_cs_add_buffer(device->ws, cs->b, task_rings_bo);

   radeon_begin(cs);

   /* Tell the GPU where the task control buffer is. */
   radeon_emit(PKT3(PKT3_DISPATCH_TASK_STATE_INIT, 1, 0) | PKT3_SHADER_TYPE_S(!!compute));
   /* bits [31:8]: control buffer address lo, bits[7:0]: reserved (set to zero) */
   radeon_emit(task_ctrlbuf_va & 0xFFFFFF00);
   /* bits [31:0]: control buffer address hi */
   radeon_emit(task_ctrlbuf_va >> 32);

   radeon_end();
}

static void
radv_emit_graphics_scratch(struct radv_device *device, struct radv_cmd_stream *cs, uint32_t size_per_wave,
                           uint32_t waves, struct radeon_winsys_bo *scratch_bo)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radeon_info *gpu_info = &pdev->info;
   uint32_t tmpring_size;

   if (!scratch_bo)
      return;

   const uint64_t va = radv_buffer_get_va(scratch_bo);

   ac_get_scratch_tmpring_size(gpu_info, waves, size_per_wave, &tmpring_size);

   radv_cs_add_buffer(device->ws, cs->b, scratch_bo);

   ac_emit_cp_gfx_scratch(cs->b, pdev->info.gfx_level, va, tmpring_size);
}

static void
radv_emit_compute_scratch(struct radv_device *device, struct radv_cmd_stream *cs, uint32_t size_per_wave,
                          uint32_t waves, struct radeon_winsys_bo *compute_scratch_bo)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const struct radeon_info *gpu_info = &pdev->info;
   uint32_t tmpring_size;
   uint64_t scratch_va;

   /* Ensure there is always a mapped BO in s[0:1] for the SMEM OOB mitigation */
   if (!compute_scratch_bo && device->compiler_info.key.mitigate_smem_oob)
      compute_scratch_bo = device->zero_bo;

   if (!compute_scratch_bo)
      return;

   scratch_va = radv_buffer_get_va(compute_scratch_bo);

   ac_get_scratch_tmpring_size(gpu_info, waves, size_per_wave, &tmpring_size);

   radv_cs_add_buffer(device->ws, cs->b, compute_scratch_bo);

   radeon_begin(cs);

   if (gpu_info->gfx_level >= GFX11) {
      assert(!device->compiler_info.key.mitigate_smem_oob);

      radeon_set_sh_reg_seq(R_00B840_COMPUTE_DISPATCH_SCRATCH_BASE_LO, 2);
      radeon_emit(scratch_va >> 8);
      radeon_emit(scratch_va >> 40);
   } else {
      uint32_t rsrc1 = S_008F04_BASE_ADDRESS_HI(scratch_va >> 32) | S_008F04_SWIZZLE_ENABLE_GFX6(1);

      radeon_set_sh_reg_seq(R_00B900_COMPUTE_USER_DATA_0, 2);
      radeon_emit(scratch_va);
      radeon_emit(rsrc1);
   }

   radeon_set_sh_reg(R_00B860_COMPUTE_TMPRING_SIZE, tmpring_size);

   radeon_end();
}

static void
radv_emit_compute_shader_pointers(struct radv_device *device, struct radv_cmd_stream *cs,
                                  struct radeon_winsys_bo *descriptor_bo)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);

   if (!descriptor_bo)
      return;

   uint64_t va = radv_buffer_get_va(descriptor_bo);
   radv_cs_add_buffer(device->ws, cs->b, descriptor_bo);

   /* Compute shader user data 0-1 have the scratch pointer (unlike GFX shaders),
    * so emit the descriptor pointer to user data 2-3 instead (task_ring_offsets arg).
    */
   radeon_begin(cs);
   if (pdev->info.gfx_level >= GFX11)
      radeon_emit_64bit_pointer(R_00B900_COMPUTE_USER_DATA_0, va);
   else
      radeon_emit_64bit_pointer(R_00B908_COMPUTE_USER_DATA_2, va);
   radeon_end();
}

static void
radv_emit_graphics_shader_pointers(struct radv_device *device, struct radv_cmd_stream *cs,
                                   struct radeon_winsys_bo *descriptor_bo)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   uint64_t va;

   /* Ensure there is always a mapped BO in s[0:1] for the SMEM OOB mitigation */
   if (!descriptor_bo && device->compiler_info.key.mitigate_smem_oob)
      descriptor_bo = device->zero_bo;

   if (!descriptor_bo)
      return;

   va = radv_buffer_get_va(descriptor_bo);

   radv_cs_add_buffer(device->ws, cs->b, descriptor_bo);

   radeon_begin(cs);

   if (pdev->info.gfx_level >= GFX12) {
      uint32_t regs[] = {R_00B030_SPI_SHADER_USER_DATA_PS_0, R_00B410_SPI_SHADER_PGM_LO_HS,
                         R_00B210_SPI_SHADER_PGM_LO_GS};

      for (int i = 0; i < ARRAY_SIZE(regs); ++i) {
         radeon_emit_64bit_pointer(regs[i], va);
      }
   } else if (pdev->info.gfx_level >= GFX11) {
      uint32_t regs[] = {R_00B030_SPI_SHADER_USER_DATA_PS_0, R_00B420_SPI_SHADER_PGM_LO_HS,
                         R_00B220_SPI_SHADER_PGM_LO_GS};

      for (int i = 0; i < ARRAY_SIZE(regs); ++i) {
         radeon_emit_64bit_pointer(regs[i], va);
      }
   } else if (pdev->info.gfx_level >= GFX10) {
      uint32_t regs[] = {R_00B030_SPI_SHADER_USER_DATA_PS_0, R_00B130_SPI_SHADER_USER_DATA_VS_0,
                         R_00B208_SPI_SHADER_USER_DATA_ADDR_LO_GS, R_00B408_SPI_SHADER_USER_DATA_ADDR_LO_HS};

      for (int i = 0; i < ARRAY_SIZE(regs); ++i) {
         radeon_emit_64bit_pointer(regs[i], va);
      }
   } else if (pdev->info.gfx_level == GFX9) {
      uint32_t regs[] = {R_00B030_SPI_SHADER_USER_DATA_PS_0, R_00B130_SPI_SHADER_USER_DATA_VS_0,
                         R_00B208_SPI_SHADER_USER_DATA_ADDR_LO_GS, R_00B408_SPI_SHADER_USER_DATA_ADDR_LO_HS};

      for (int i = 0; i < ARRAY_SIZE(regs); ++i) {
         radeon_emit_64bit_pointer(regs[i], va);
      }
   } else {
      uint32_t regs[] = {R_00B030_SPI_SHADER_USER_DATA_PS_0, R_00B130_SPI_SHADER_USER_DATA_VS_0,
                         R_00B230_SPI_SHADER_USER_DATA_GS_0, R_00B330_SPI_SHADER_USER_DATA_ES_0,
                         R_00B430_SPI_SHADER_USER_DATA_HS_0, R_00B530_SPI_SHADER_USER_DATA_LS_0};

      for (int i = 0; i < ARRAY_SIZE(regs); ++i) {
         radeon_emit_64bit_pointer(regs[i], va);
      }
   }

   radeon_end();
}

static void
radv_emit_ge_rings(struct radv_device *device, struct radv_cmd_stream *cs, struct radeon_winsys_bo *ge_rings_bo)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   uint64_t va;

   if (!ge_rings_bo)
      return;

   va = radv_buffer_get_va(ge_rings_bo);

   radv_cs_add_buffer(device->ws, cs->b, ge_rings_bo);

   /* We must wait for idle using an EOP event before changing the attribute ring registers. Use the
    * bottom-of-pipe EOP event, but increment the PWS counter instead of writing memory.
    */
   ac_emit_cp_release_mem_pws(cs->b, pdev->info.gfx_level, AMD_IP_GFX, V_028A90_BOTTOM_OF_PIPE_TS, 0);

   /* Wait for the PWS counter. */
   ac_emit_cp_acquire_mem_pws(cs->b, pdev->info.gfx_level, AMD_IP_GFX, V_028A90_BOTTOM_OF_PIPE_TS, V_581B_CP_ME, 0, 0);

   ac_emit_cp_gfx11_ge_rings(cs->b, &pdev->info, va, pdev->gfx12_hiz_wa == RADV_GFX12_HIZ_WA_PARTIAL);
}

static void
radv_emit_compute(struct radv_device *device, struct radv_cmd_stream *cs, bool is_compute_queue)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const uint64_t border_color_va = device->border_color_data.bo ? radv_buffer_get_va(device->border_color_data.bo) : 0;

   struct ac_pm4_state *pm4 = ac_pm4_create_sized(&pdev->info, false, 64, is_compute_queue);
   if (!pm4)
      return;

   const struct ac_preamble_state preamble_state = {
      .border_color_va = border_color_va,
      .gfx11 =
         {
            .compute_dispatch_interleave = 64,
         },
   };

   ac_init_compute_preamble_state(&preamble_state, pm4);

   ac_pm4_set_reg(pm4, R_00B810_COMPUTE_START_X, 0);
   ac_pm4_set_reg(pm4, R_00B814_COMPUTE_START_Y, 0);
   ac_pm4_set_reg(pm4, R_00B818_COMPUTE_START_Z, 0);

   if (pdev->info.gfx_level == GFX8 && device->tma_bo) {
      uint64_t tba_va, tma_va;

      tba_va = radv_shader_get_va(device->trap_handler_shader);
      tma_va = radv_buffer_get_va(device->tma_bo);

      ac_pm4_set_reg(pm4, R_00B838_COMPUTE_TBA_LO, tba_va >> 8);
      ac_pm4_set_reg(pm4, R_00B83C_COMPUTE_TBA_HI, tba_va >> 40);
      ac_pm4_set_reg(pm4, R_00B840_COMPUTE_TMA_LO, tma_va >> 8);
      ac_pm4_set_reg(pm4, R_00B844_COMPUTE_TMA_HI, tma_va >> 40);
   }

   if (pdev->info.gfx_level >= GFX12) {
      if (is_compute_queue) {
         ac_pm4_set_reg(pm4, R_00B8BC_COMPUTE_DISPATCH_INTERLEAVE,
                        S_00B8BC_INTERLEAVE_1D(preamble_state.gfx11.compute_dispatch_interleave));
      } else {
         ac_pm4_set_reg_custom(pm4, R_00B8BC_COMPUTE_DISPATCH_INTERLEAVE - SI_SH_REG_OFFSET,
                               S_00B8BC_INTERLEAVE_1D(preamble_state.gfx11.compute_dispatch_interleave),
                               PKT3_SET_SH_REG_INDEX, 2);
      }
   }

   ac_pm4_finalize(pm4);
   ac_pm4_emit_commands(cs->b, pm4);
   ac_pm4_free_state(pm4);
}

/* 12.4 fixed-point */
static unsigned
radv_pack_float_12p4(float x)
{
   return x <= 0 ? 0 : x >= 4096 ? 0xffff : x * 16;
}

void
radv_emit_graphics(struct radv_device *device, struct radv_cmd_stream *cs)
{
   struct radv_physical_device *pdev = radv_device_physical(device);
   const uint64_t border_color_va = device->border_color_data.bo ? radv_buffer_get_va(device->border_color_data.bo) : 0;
   bool has_clear_state = pdev->info.has_clear_state;
   int i;

   struct ac_pm4_state *pm4 = ac_pm4_create_sized(&pdev->info, false, 512, false);
   if (!pm4)
      return;

   if (!device->uses_shadow_regs) {
      ac_pm4_cmd_add(pm4, PKT3(PKT3_CONTEXT_CONTROL, 1, 0));
      ac_pm4_cmd_add(pm4, S_281_UPDATE_LOAD_ENABLES(1));
      ac_pm4_cmd_add(pm4, S_282_UPDATE_SHADOW_ENABLES(1));

      if (has_clear_state) {
         ac_pm4_cmd_add(pm4, PKT3(PKT3_CLEAR_STATE, 0, 0));
         ac_pm4_cmd_add(pm4, 0);
      }
   }

   const struct ac_preamble_state preamble_state = {
      .border_color_va = border_color_va,
   };

   ac_init_graphics_preamble_state(&preamble_state, pm4);

   if (!has_clear_state) {
      for (i = 0; i < 16; i++) {
         ac_pm4_set_reg(pm4, R_0282D0_PA_SC_VPORT_ZMIN_0 + i * 8, 0);
         ac_pm4_set_reg(pm4, R_0282D4_PA_SC_VPORT_ZMAX_0 + i * 8, fui(1.0));
      }
   }

   if (!has_clear_state) {
      ac_pm4_set_reg(pm4, R_028230_PA_SC_EDGERULE, 0xAAAAAAAA);
   }

   if (pdev->info.gfx_level <= GFX8)
      ac_pm4_set_reg(pm4, R_00B324_SPI_SHADER_PGM_HI_ES, S_00B324_MEM_BASE(pdev->info.address32_hi >> 8));

   if (pdev->info.gfx_level < GFX11)
      ac_pm4_set_reg(pm4, R_00B124_SPI_SHADER_PGM_HI_VS, S_00B124_MEM_BASE(pdev->info.address32_hi >> 8));

   if (pdev->info.gfx_level >= GFX10) {
      /* Vulkan doesn't support user edge flags and it also doesn't
       * need to prevent drawing lines on internal edges of
       * decomposed primitives (such as quads) with polygon mode = lines.
       */
      unsigned vertex_reuse_depth = pdev->info.gfx_level >= GFX10_3 ? 30 : 0;
      ac_pm4_set_reg(pm4, R_028838_PA_CL_NGG_CNTL,
                     S_028838_INDEX_BUF_EDGE_FLAG_ENA(0) | S_028838_VERTEX_REUSE_DEPTH(vertex_reuse_depth));
   }

   unsigned tmp = (unsigned)(1.0 * 8.0);
   ac_pm4_set_reg(pm4, R_028A00_PA_SU_POINT_SIZE, S_028A00_HEIGHT(tmp) | S_028A00_WIDTH(tmp));
   ac_pm4_set_reg(pm4, R_028A04_PA_SU_POINT_MINMAX,
                  S_028A04_MIN_SIZE(radv_pack_float_12p4(0)) | S_028A04_MAX_SIZE(radv_pack_float_12p4(8191.875 / 2)));

   /* Enable the Polaris small primitive filter control.
    * XXX: There is possibly an issue when MSAA is off (see RadeonSI
    * has_msaa_sample_loc_bug). But this doesn't seem to regress anything,
    * and AMDVLK doesn't have a workaround as well.
    */
   if (pdev->info.family >= CHIP_POLARIS10) {
      unsigned small_prim_filter_cntl = S_028830_SMALL_PRIM_FILTER_ENABLE(1) |
                                        /* Workaround for a hw line bug. */
                                        S_028830_LINE_FILTER_DISABLE(pdev->info.family <= CHIP_POLARIS12) |
                                        S_028830_SC_1XMSAA_COMPATIBLE_DISABLE(pdev->info.gfx_level >= GFX10);

      ac_pm4_set_reg(pm4, R_028830_PA_SU_SMALL_PRIM_FILTER_CNTL, small_prim_filter_cntl);
   }

   if (pdev->info.gfx_level >= GFX12) {
      ac_pm4_set_reg(pm4, R_028644_SPI_INTERP_CONTROL_0,
                     S_0286D4_FLAT_SHADE_ENA(1) | S_0286D4_PNT_SPRITE_ENA(1) |
                        S_0286D4_PNT_SPRITE_OVRD_X(V_0286D4_SPI_PNT_SPRITE_SEL_S) |
                        S_0286D4_PNT_SPRITE_OVRD_Y(V_0286D4_SPI_PNT_SPRITE_SEL_T) |
                        S_0286D4_PNT_SPRITE_OVRD_Z(V_0286D4_SPI_PNT_SPRITE_SEL_0) |
                        S_0286D4_PNT_SPRITE_OVRD_W(V_0286D4_SPI_PNT_SPRITE_SEL_1) |
                        S_0286D4_PNT_SPRITE_TOP_1(0)); /* vulkan is top to bottom - 1.0 at bottom */
   } else {
      ac_pm4_set_reg(pm4, R_0286D4_SPI_INTERP_CONTROL_0,
                     S_0286D4_FLAT_SHADE_ENA(1) | S_0286D4_PNT_SPRITE_ENA(1) |
                        S_0286D4_PNT_SPRITE_OVRD_X(V_0286D4_SPI_PNT_SPRITE_SEL_S) |
                        S_0286D4_PNT_SPRITE_OVRD_Y(V_0286D4_SPI_PNT_SPRITE_SEL_T) |
                        S_0286D4_PNT_SPRITE_OVRD_Z(V_0286D4_SPI_PNT_SPRITE_SEL_0) |
                        S_0286D4_PNT_SPRITE_OVRD_W(V_0286D4_SPI_PNT_SPRITE_SEL_1) |
                        S_0286D4_PNT_SPRITE_TOP_1(0)); /* vulkan is top to bottom - 1.0 at bottom */
   }

   ac_pm4_set_reg(pm4, R_028BE4_PA_SU_VTX_CNTL,
                  S_028BE4_PIX_CENTER(1) | S_028BE4_ROUND_MODE(V_028BE4_X_ROUND_TO_EVEN) |
                     S_028BE4_QUANT_MODE(V_028BE4_X_16_8_FIXED_POINT_1_256TH));

   if (pdev->info.gfx_level >= GFX12) {
      ac_pm4_set_reg(pm4, R_028814_PA_CL_VTE_CNTL,
                     S_028818_VTX_W0_FMT(1) | S_028818_VPORT_X_SCALE_ENA(1) | S_028818_VPORT_X_OFFSET_ENA(1) |
                        S_028818_VPORT_Y_SCALE_ENA(1) | S_028818_VPORT_Y_OFFSET_ENA(1) | S_028818_VPORT_Z_SCALE_ENA(1) |
                        S_028818_VPORT_Z_OFFSET_ENA(1));
   } else {
      ac_pm4_set_reg(pm4, R_028818_PA_CL_VTE_CNTL,
                     S_028818_VTX_W0_FMT(1) | S_028818_VPORT_X_SCALE_ENA(1) | S_028818_VPORT_X_OFFSET_ENA(1) |
                        S_028818_VPORT_Y_SCALE_ENA(1) | S_028818_VPORT_Y_OFFSET_ENA(1) | S_028818_VPORT_Z_SCALE_ENA(1) |
                        S_028818_VPORT_Z_OFFSET_ENA(1));
   }

   if (pdev->info.gfx_level == GFX8 && device->tma_bo) {
      uint64_t tba_va, tma_va;

      tba_va = radv_shader_get_va(device->trap_handler_shader);
      tma_va = radv_buffer_get_va(device->tma_bo);

      uint32_t regs[] = {R_00B000_SPI_SHADER_TBA_LO_PS, R_00B100_SPI_SHADER_TBA_LO_VS, R_00B200_SPI_SHADER_TBA_LO_GS,
                         R_00B300_SPI_SHADER_TBA_LO_ES, R_00B400_SPI_SHADER_TBA_LO_HS, R_00B500_SPI_SHADER_TBA_LO_LS};

      for (i = 0; i < ARRAY_SIZE(regs); ++i) {
         ac_pm4_set_reg(pm4, regs[i] + 0, tba_va >> 8);
         ac_pm4_set_reg(pm4, regs[i] + 4, tba_va >> 40);
         ac_pm4_set_reg(pm4, regs[i] + 8, tma_va >> 8);
         ac_pm4_set_reg(pm4, regs[i] + 12, tma_va >> 40);
      }
   }

   ac_pm4_set_reg(pm4, R_028828_PA_SU_LINE_STIPPLE_SCALE, 0x3f800000);

   if (pdev->info.gfx_level >= GFX12) {
      ac_pm4_set_reg(pm4, R_028000_DB_RENDER_CONTROL, 0);
   }

   if (pdev->info.family >= CHIP_NAVI31 && pdev->info.family <= CHIP_STRIX1) {
      /* Disable SINGLE clear codes on GFX11 (including first GFX11.5 rev) to workaround a hw bug
       * with DCC. */
      ac_pm4_set_reg(pm4, R_028424_CB_FDCC_CONTROL, S_028424_DISABLE_CONSTANT_ENCODE_SINGLE(1));
   }

   ac_pm4_finalize(pm4);
   ac_pm4_emit_commands(cs->b, pm4);
   ac_pm4_free_state(pm4);

   radv_emit_compute(device, cs, false);
}

static void
radv_init_graphics_state(struct radv_cmd_stream *cs, struct radv_device *device)
{
   if (device->gfx_init) {
      struct radeon_winsys *ws = device->ws;

      ws->cs_execute_ib(cs->b, device->gfx_init, 0, device->gfx_init_size_dw & 0xffff, false);

      radv_cs_add_buffer(device->ws, cs->b, device->gfx_init);
   } else {
      radv_emit_graphics(device, cs);
   }
}

static VkResult
radv_update_preamble_cs(struct radv_queue_state *queue, struct radv_device *device,
                        const struct radv_queue_ring_info *needs, bool secure)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radeon_winsys *ws = device->ws;
   struct radeon_winsys_bo *scratch_bo = queue->scratch_bo;
   struct radeon_winsys_bo *descriptor_bo = queue->descriptor_bo;
   struct radeon_winsys_bo *compute_scratch_bo = queue->compute_scratch_bo;
   struct radeon_winsys_bo *esgs_ring_bo = queue->esgs_ring_bo;
   struct radeon_winsys_bo *gsvs_ring_bo = queue->gsvs_ring_bo;
   struct radeon_winsys_bo *tess_rings_bo = queue->tess_rings_bo;
   struct radeon_winsys_bo *task_rings_bo = queue->task_rings_bo;
   struct radeon_winsys_bo *mesh_scratch_ring_bo = queue->mesh_scratch_ring_bo;
   struct radeon_winsys_bo *ge_rings_bo = queue->ge_rings_bo;
   struct radeon_winsys_bo *gds_bo = queue->gds_bo;
   struct radeon_winsys_bo *gds_oa_bo = queue->gds_oa_bo;
   struct radv_cmd_stream *dest_cs[3] = {0};
   const uint32_t ring_bo_flags = RADEON_FLAG_NO_CPU_ACCESS | RADEON_FLAG_NO_INTERPROCESS_SHARING;
   const uint32_t ring_bo_flags_tmz = ring_bo_flags | (secure ? RADEON_FLAG_ENCRYPTED : 0);
   VkResult result = VK_SUCCESS;

   const bool add_sample_positions = !queue->ring_info.sample_positions && needs->sample_positions;
   const uint32_t scratch_size = needs->scratch_size_per_wave * needs->scratch_waves;
   const uint32_t queue_scratch_size = queue->ring_info.scratch_size_per_wave * queue->ring_info.scratch_waves;

   if (scratch_size > queue_scratch_size) {
      result = radv_bo_create(device, NULL, scratch_size, 4096, RADEON_DOMAIN_VRAM, ring_bo_flags,
                              RADV_BO_PRIORITY_SCRATCH, 0, true, &scratch_bo);
      if (result != VK_SUCCESS)
         goto fail;
      radv_rmv_log_command_buffer_bo_create(device, scratch_bo, 0, 0, scratch_size);
   }

   const uint32_t compute_scratch_size = needs->compute_scratch_size_per_wave * needs->compute_scratch_waves;
   const uint32_t compute_queue_scratch_size =
      queue->ring_info.compute_scratch_size_per_wave * queue->ring_info.compute_scratch_waves;
   if (compute_scratch_size > compute_queue_scratch_size) {
      result = radv_bo_create(device, NULL, compute_scratch_size, 4096, RADEON_DOMAIN_VRAM, ring_bo_flags_tmz,
                              RADV_BO_PRIORITY_SCRATCH, 0, true, &compute_scratch_bo);
      if (result != VK_SUCCESS)
         goto fail;
      radv_rmv_log_command_buffer_bo_create(device, compute_scratch_bo, 0, 0, compute_scratch_size);
   }

   if (needs->esgs_ring_size > queue->ring_info.esgs_ring_size) {
      result = radv_bo_create(device, NULL, radv_orbis_gs_ring_bytes(needs->esgs_ring_size, true), 4096,
                              RADEON_DOMAIN_VRAM, ring_bo_flags, RADV_BO_PRIORITY_SCRATCH, 0, true, &esgs_ring_bo);
      if (result != VK_SUCCESS)
         goto fail;
      radv_rmv_log_command_buffer_bo_create(device, esgs_ring_bo, 0, 0, needs->esgs_ring_size);
   }

   if (needs->gsvs_ring_size > queue->ring_info.gsvs_ring_size) {
      result = radv_bo_create(device, NULL, radv_orbis_gs_ring_bytes(needs->gsvs_ring_size, false), 4096,
                              RADEON_DOMAIN_VRAM, ring_bo_flags, RADV_BO_PRIORITY_SCRATCH, 0, true, &gsvs_ring_bo);
      if (result != VK_SUCCESS)
         goto fail;
      radv_rmv_log_command_buffer_bo_create(device, gsvs_ring_bo, 0, 0, needs->gsvs_ring_size);
   }

   if (!queue->ring_info.tess_rings && needs->tess_rings) {
      uint64_t tess_bo_size = pdev->info.total_tess_ring_size;
#ifdef HAVE_ORBIS_PLATFORM
      /* ⚠ A KNOB FOR AN OPEN QUESTION, DEFAULTING TO THE UNMODIFIED SIZE.
       *
       * With the rings finally programmed from a BO of ours, the earlier effort measured a GPU WRITE fault on
       * the page immediately after a ring - an offset into the ring of 74 x 32768 where the allocation was
       * sized for 72 workgroups. Two explanations fit and it did not separate them:
       *
       *   the sizing is short   - every term of it (max_good_cu_per_sa x max_sa_per_se x max_se,
       *                           ac_gpu_info.c) comes from CU topology this port's own arm marks UNCITED
       *   the count is wrong    - OFFCHIP_BUFFERING may be read per-SE by this part, where Mesa divides by
       *                           max_se only from GFX11 up (ac_cmdbuf_cp.c)
       *
       * That effort left the multiplier at FOUR by default, which turns an unanswered question into shipped
       * behaviour. It defaults to 1 here: the ring fix and the ring SIZE are two variables, and the first run
       * should move one. ORBIS_TESS_RING_SCALE=4 arms the experiment from the env file without a rebuild,
       * which is the whole point of it being a knob.
       *
       * It grows the ALLOCATION only. hs_offchip_param still states the same workgroup count and the ring
       * descriptors carry the same sizes, so the extra pages sit past the factor ring - where the overrun
       * lands - and the two candidates separate on the next run with no further instrument:
       *
       *   the fault disappears        -> the sizing is short, and the topology guess is what makes it short
       *   the fault stays at end+page -> the hardware was told the wrong workgroup count
       */
      {
         const char *const scale_env = getenv("ORBIS_TESS_RING_SCALE");
         unsigned scale = (scale_env != NULL) ? (unsigned)atoi(scale_env) : 1u;
         if (scale < 1 || scale > 64)
            scale = 1;
         if (scale != 1) {
            tess_bo_size *= scale;
            mesa_logi("radv/orbis: ORBIS_TESS_RING_SCALE=%u - allocating %" PRIu64 " bytes for a ring "
                      "ac_gpu_info sized at %u. Every register is left as computed, so this tests the SIZE "
                      "and nothing else.",
                      scale, tess_bo_size, pdev->info.total_tess_ring_size);
         }
      }
#endif
      result = radv_bo_create(device, NULL, tess_bo_size, 256, RADEON_DOMAIN_VRAM, ring_bo_flags_tmz,
                              RADV_BO_PRIORITY_SCRATCH, 0, true, &tess_rings_bo);
      if (result != VK_SUCCESS)
         goto fail;

#ifdef HAVE_ORBIS_PLATFORM
      /* ⚠ HOW BIG DOES THE RING ACTUALLY NEED TO BE? A MULTIPLIER IS NOT AN ANSWER.
       *
       * ORBIS_TESS_RING_SCALE=4 stopped the write faults, which established that the ring ac_gpu_info sizes
       * is SHORTER than the hardware uses - both register readings of those faults had already been tested
       * and killed. What it did not establish is by how much, and shipping x4 as a default would import that
       * open question as a fact.
       *
       * So the ring is filled with a pattern at creation and the highest byte the GPU disturbed is reported.
       * That converts "four times is enough" into a number, in one run, with no console bisection - and the
       * number is what a real fix needs, because the honest form of this is a corrected size term rather
       * than a multiplier.
       *
       * ORBIS_TESS_RING_WATERMARK=<n> reports after every nth submission. Off by default: the scan reads the
       * whole ring, which is 9 MiB at scale 4. */
      if (getenv("ORBIS_TESS_RING_WATERMARK") != NULL) {
         uint8_t *const map = (uint8_t *)radv_buffer_map(device->ws, tess_rings_bo);
         if (map != NULL) {
            memset(map, ORBIS_TESS_RING_POISON, tess_bo_size);
            orbis_tess_ring_map = map;
            orbis_tess_ring_size = tess_bo_size;
            orbis_tess_ring_sized_at = pdev->info.total_tess_ring_size;
            mesa_logi("radv/orbis: tess ring filled with 0x%02x for the watermark - anything not that byte "
                      "afterwards was written by the GPU",
                      ORBIS_TESS_RING_POISON);
         } else {
            mesa_logw("radv/orbis: tess ring would not map - no watermark this boot");
         }
      }
#endif
      radv_rmv_log_command_buffer_bo_create(device, tess_rings_bo, 0, 0, tess_bo_size);
   }

   if (!queue->ring_info.task_rings && needs->task_rings) {
      assert(pdev->info.gfx_level >= GFX10_3);

      /* We write the control buffer from the CPU, so need to grant CPU access to the BO.
       * The draw ring needs to be zero-initialized otherwise the ready bits will be incorrect.
       */
      uint32_t task_rings_bo_flags =
         RADEON_FLAG_CPU_ACCESS | RADEON_FLAG_NO_INTERPROCESS_SHARING | RADEON_FLAG_ZERO_VRAM;

      result = radv_bo_create(device, NULL, pdev->task_info.bo_size_bytes, 256, RADEON_DOMAIN_VRAM, task_rings_bo_flags,
                              RADV_BO_PRIORITY_SCRATCH, 0, true, &task_rings_bo);
      if (result != VK_SUCCESS)
         goto fail;
      radv_rmv_log_command_buffer_bo_create(device, task_rings_bo, 0, 0, pdev->task_info.bo_size_bytes);

      result = radv_initialise_task_control_buffer(device, task_rings_bo);
      if (result != VK_SUCCESS)
         goto fail;
   }

   if (!queue->ring_info.mesh_scratch_ring && needs->mesh_scratch_ring) {
      assert(pdev->info.gfx_level >= GFX10_3);
      result =
         radv_bo_create(device, NULL, AC_MESH_SCRATCH_NUM_ENTRIES * AC_MESH_SCRATCH_ENTRY_BYTES, 256,
                        RADEON_DOMAIN_VRAM, ring_bo_flags, RADV_BO_PRIORITY_SCRATCH, 0, true, &mesh_scratch_ring_bo);

      if (result != VK_SUCCESS)
         goto fail;
      radv_rmv_log_command_buffer_bo_create(device, mesh_scratch_ring_bo, 0, 0,
                                            AC_MESH_SCRATCH_NUM_ENTRIES * AC_MESH_SCRATCH_ENTRY_BYTES);
   }

   if (!queue->ring_info.ge_rings && needs->ge_rings) {
      assert(pdev->info.gfx_level >= GFX11);
      result = radv_bo_create(device, NULL, pdev->info.total_attribute_pos_prim_ring_size, 2 * 1024 * 1024 /* 2MiB */,
                              RADEON_DOMAIN_VRAM, RADEON_FLAG_32BIT | RADEON_FLAG_DISCARDABLE | ring_bo_flags_tmz,
                              RADV_BO_PRIORITY_SCRATCH, 0, true, &ge_rings_bo);
      if (result != VK_SUCCESS)
         goto fail;
      radv_rmv_log_command_buffer_bo_create(device, ge_rings_bo, 0, 0, pdev->info.total_attribute_pos_prim_ring_size);
   }

   if (!queue->ring_info.gds && needs->gds) {
      assert(pdev->info.gfx_level == GFX10 || pdev->info.gfx_level == GFX10_3);

      /* 4 streamout GDS counters.
       * We need 256B (64 dw) of GDS, otherwise streamout hangs.
       */
      result = radv_bo_create(device, NULL, 256, 4, RADEON_DOMAIN_GDS, ring_bo_flags, RADV_BO_PRIORITY_SCRATCH, 0, true,
                              &gds_bo);
      if (result != VK_SUCCESS)
         goto fail;

      /* Add the GDS BO to our global BO list to prevent the kernel to emit a GDS switch and reset
       * the state when a compute queue is used.
       */
      result = device->ws->buffer_make_resident(ws, gds_bo, true);
      if (result != VK_SUCCESS)
         goto fail;
   }

   if (!queue->ring_info.gds_oa && needs->gds_oa) {
      assert(pdev->info.gfx_level >= GFX10 && pdev->info.gfx_level < GFX12);

      result = radv_bo_create(device, NULL, 1, 1, RADEON_DOMAIN_OA, ring_bo_flags, RADV_BO_PRIORITY_SCRATCH, 0, true,
                              &gds_oa_bo);
      if (result != VK_SUCCESS)
         goto fail;

      /* Add the GDS OA BO to our global BO list to prevent the kernel to emit a GDS switch and
       * reset the state when a compute queue is used.
       */
      result = device->ws->buffer_make_resident(ws, gds_oa_bo, true);
      if (result != VK_SUCCESS)
         goto fail;
   }

   /* Re-initialize the descriptor BO when any ring BOs changed.
    *
    * Additionally, make sure to create the descriptor BO for the compute queue
    * when it uses the task shader rings. The task rings BO is shared between the
    * GFX and compute queues and already initialized here.
    */
   if ((queue->qf == RADV_QUEUE_COMPUTE && !descriptor_bo && task_rings_bo) || scratch_bo != queue->scratch_bo ||
       esgs_ring_bo != queue->esgs_ring_bo || gsvs_ring_bo != queue->gsvs_ring_bo ||
       tess_rings_bo != queue->tess_rings_bo || task_rings_bo != queue->task_rings_bo ||
       mesh_scratch_ring_bo != queue->mesh_scratch_ring_bo || ge_rings_bo != queue->ge_rings_bo ||
       add_sample_positions) {
      const uint32_t size = 304;

      result = radv_bo_create(device, NULL, size, 4096, RADEON_DOMAIN_VRAM,
                              RADEON_FLAG_CPU_ACCESS | RADEON_FLAG_NO_INTERPROCESS_SHARING | RADEON_FLAG_READ_ONLY,
                              RADV_BO_PRIORITY_DESCRIPTOR, 0, true, &descriptor_bo);
      if (result != VK_SUCCESS)
         goto fail;
   }

   if (descriptor_bo != queue->descriptor_bo) {
      uint32_t *map = (uint32_t *)radv_buffer_map(ws, descriptor_bo);
      if (!map) {
         result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
         goto fail;
      }

      radv_fill_shader_rings(device, map, scratch_bo, needs->esgs_ring_size, esgs_ring_bo, needs->gsvs_ring_size,
                             gsvs_ring_bo, tess_rings_bo, task_rings_bo, mesh_scratch_ring_bo, ge_rings_bo);

      ws->buffer_unmap(ws, descriptor_bo, false);
   }

   const enum amd_ip_type hw_ip = radv_queue_family_to_ring(pdev, queue->qf);

   for (int i = 0; i < 3; ++i) {
      enum rgp_flush_bits sqtt_flush_bits = 0;
      struct radv_cmd_stream *cs = NULL;

      result = radv_create_cmd_stream(device, hw_ip, false, &cs);
      if (result != VK_SUCCESS)
         goto fail;

      radeon_check_space(ws, cs->b, 512);
      dest_cs[i] = cs;

      if (scratch_bo)
         radv_cs_add_buffer(ws, cs->b, scratch_bo);

      /* Emit initial configuration. */
      switch (queue->qf) {
      case RADV_QUEUE_GENERAL:
         if (queue->uses_shadow_regs)
            radv_emit_shadow_regs_preamble(cs, device, queue);
         radv_init_graphics_state(cs, device);

         if (esgs_ring_bo || gsvs_ring_bo || tess_rings_bo || task_rings_bo) {
            radeon_begin(cs);
            radeon_event_write(V_028A90_VS_PARTIAL_FLUSH);
            radeon_event_write(V_028A90_VGT_FLUSH);
            radeon_end();
         }

         if (mesh_scratch_ring_bo)
            radv_cs_add_buffer(device->ws, cs->b, mesh_scratch_ring_bo);

         radv_emit_gs_ring_sizes(device, cs, esgs_ring_bo, needs->esgs_ring_size, gsvs_ring_bo, needs->gsvs_ring_size);
         radv_emit_tess_factor_ring(device, cs, tess_rings_bo);
         radv_emit_task_rings(device, cs, task_rings_bo, false);
         radv_emit_ge_rings(device, cs, ge_rings_bo);
         radv_emit_graphics_shader_pointers(device, cs, descriptor_bo);
         radv_emit_compute_scratch(device, cs, needs->compute_scratch_size_per_wave, needs->compute_scratch_waves,
                                   compute_scratch_bo);
         radv_emit_graphics_scratch(device, cs, needs->scratch_size_per_wave, needs->scratch_waves, scratch_bo);
         break;
      case RADV_QUEUE_COMPUTE:
         radv_emit_compute(device, cs, true);

         if (task_rings_bo) {
            radeon_begin(cs);
            radeon_event_write(V_028A90_CS_PARTIAL_FLUSH);
            radeon_end();
         }

         radv_emit_task_rings(device, cs, task_rings_bo, true);
         radv_emit_compute_shader_pointers(device, cs, descriptor_bo);
         radv_emit_compute_scratch(device, cs, needs->compute_scratch_size_per_wave, needs->compute_scratch_waves,
                                   compute_scratch_bo);
         break;
      default:
         break;
      }

      if (i < 2) {
         /* The two initial preambles have a cache flush at the beginning. */
         const enum amd_gfx_level gfx_level = pdev->info.gfx_level;
         enum radv_cmd_flush_bits flush_bits = RADV_CMD_FLAG_INV_ICACHE | RADV_CMD_FLAG_INV_SCACHE |
                                               RADV_CMD_FLAG_INV_VCACHE | RADV_CMD_FLAG_INV_L2 |
                                               RADV_CMD_FLAG_START_PIPELINE_STATS;

         if (i == 0) {
            /* The full flush preamble should also wait for previous shader work to finish. */
            flush_bits |= RADV_CMD_FLAG_CS_PARTIAL_FLUSH;
            if (queue->qf == RADV_QUEUE_GENERAL)
               flush_bits |= RADV_CMD_FLAG_PS_PARTIAL_FLUSH;
         }

         radv_cs_emit_cache_flush(ws, cs, gfx_level, NULL, 0, flush_bits, &sqtt_flush_bits, 0);
      }

      result = radv_finalize_cmd_stream(device, cs);
      if (result != VK_SUCCESS)
         goto fail;
   }

   if (queue->initial_full_flush_preamble_cs)
      radv_destroy_cmd_stream(device, queue->initial_full_flush_preamble_cs);

   if (queue->initial_preamble_cs)
      radv_destroy_cmd_stream(device, queue->initial_preamble_cs);

   if (queue->continue_preamble_cs)
      radv_destroy_cmd_stream(device, queue->continue_preamble_cs);

   queue->initial_full_flush_preamble_cs = dest_cs[0];
   queue->initial_preamble_cs = dest_cs[1];
   queue->continue_preamble_cs = dest_cs[2];

   if (scratch_bo != queue->scratch_bo) {
      if (queue->scratch_bo) {
         radv_rmv_log_command_buffer_bo_destroy(device, queue->scratch_bo);
         radv_bo_destroy(device, NULL, queue->scratch_bo);
      }
      queue->scratch_bo = scratch_bo;
   }

   if (compute_scratch_bo != queue->compute_scratch_bo) {
      if (queue->compute_scratch_bo) {
         radv_rmv_log_command_buffer_bo_destroy(device, queue->compute_scratch_bo);
         radv_bo_destroy(device, NULL, queue->compute_scratch_bo);
      }
      queue->compute_scratch_bo = compute_scratch_bo;
   }

   if (esgs_ring_bo != queue->esgs_ring_bo) {
      if (queue->esgs_ring_bo) {
         radv_rmv_log_command_buffer_bo_destroy(device, queue->esgs_ring_bo);
         radv_bo_destroy(device, NULL, queue->esgs_ring_bo);
      }
      queue->esgs_ring_bo = esgs_ring_bo;
   }

   if (gsvs_ring_bo != queue->gsvs_ring_bo) {
      if (queue->gsvs_ring_bo) {
         radv_rmv_log_command_buffer_bo_destroy(device, queue->gsvs_ring_bo);
         radv_bo_destroy(device, NULL, queue->gsvs_ring_bo);
      }
      queue->gsvs_ring_bo = gsvs_ring_bo;
   }

   if (descriptor_bo != queue->descriptor_bo) {
      if (queue->descriptor_bo)
         radv_bo_destroy(device, NULL, queue->descriptor_bo);
      queue->descriptor_bo = descriptor_bo;
   }

   queue->tess_rings_bo = tess_rings_bo;
   queue->task_rings_bo = task_rings_bo;
   queue->mesh_scratch_ring_bo = mesh_scratch_ring_bo;
   queue->ge_rings_bo = ge_rings_bo;
   queue->gds_bo = gds_bo;
   queue->gds_oa_bo = gds_oa_bo;
   queue->ring_info = *needs;
   return VK_SUCCESS;
fail:
   for (int i = 0; i < ARRAY_SIZE(dest_cs); ++i)
      if (dest_cs[i])
         radv_destroy_cmd_stream(device, dest_cs[i]);
   if (descriptor_bo && descriptor_bo != queue->descriptor_bo)
      radv_bo_destroy(device, NULL, descriptor_bo);
   if (scratch_bo && scratch_bo != queue->scratch_bo)
      radv_bo_destroy(device, NULL, scratch_bo);
   if (compute_scratch_bo && compute_scratch_bo != queue->compute_scratch_bo)
      radv_bo_destroy(device, NULL, compute_scratch_bo);
   if (esgs_ring_bo && esgs_ring_bo != queue->esgs_ring_bo)
      radv_bo_destroy(device, NULL, esgs_ring_bo);
   if (gsvs_ring_bo && gsvs_ring_bo != queue->gsvs_ring_bo)
      radv_bo_destroy(device, NULL, gsvs_ring_bo);
   if (tess_rings_bo && tess_rings_bo != queue->tess_rings_bo)
      radv_bo_destroy(device, NULL, tess_rings_bo);
   if (task_rings_bo && task_rings_bo != queue->task_rings_bo)
      radv_bo_destroy(device, NULL, task_rings_bo);
   if (ge_rings_bo && ge_rings_bo != queue->ge_rings_bo)
      radv_bo_destroy(device, NULL, ge_rings_bo);
   if (gds_bo && gds_bo != queue->gds_bo) {
      ws->buffer_make_resident(ws, queue->gds_bo, false);
      radv_bo_destroy(device, NULL, gds_bo);
   }
   if (gds_oa_bo && gds_oa_bo != queue->gds_oa_bo) {
      ws->buffer_make_resident(ws, queue->gds_oa_bo, false);
      radv_bo_destroy(device, NULL, gds_oa_bo);
   }

   return vk_error(device, result);
}

static VkResult
radv_update_preambles(struct radv_queue_state *queue, struct radv_device *device,
                      struct vk_command_buffer *const *cmd_buffers, uint32_t cmd_buffer_count, bool *use_perf_counters,
                      bool *has_follower, bool secure)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);

   if (queue->qf != RADV_QUEUE_GENERAL && queue->qf != RADV_QUEUE_COMPUTE) {
      for (uint32_t j = 0; j < cmd_buffer_count; j++) {
         struct radv_cmd_buffer *cmd_buffer = container_of(cmd_buffers[j], struct radv_cmd_buffer, vk);

         *has_follower |= !!cmd_buffer->gang.cs;
      }

      return VK_SUCCESS;
   }

   /* Figure out the needs of the current submission.
    * Start by copying the queue's current info.
    * This is done because we only allow two possible behaviours for these buffers:
    * - Grow when the newly needed amount is larger than what we had
    * - Allocate the max size and reuse it, but don't free it until the queue is destroyed
    */
   struct radv_queue_ring_info needs = queue->ring_info;
   *use_perf_counters = false;
   *has_follower = false;

   for (uint32_t j = 0; j < cmd_buffer_count; j++) {
      struct radv_cmd_buffer *cmd_buffer = container_of(cmd_buffers[j], struct radv_cmd_buffer, vk);

      needs.scratch_size_per_wave = MAX2(needs.scratch_size_per_wave, cmd_buffer->queue_state.scratch_size_per_wave_needed);
      needs.scratch_waves = MAX2(needs.scratch_waves, cmd_buffer->queue_state.scratch_waves_wanted);
      needs.compute_scratch_size_per_wave =
         MAX2(needs.compute_scratch_size_per_wave, cmd_buffer->queue_state.compute_scratch_size_per_wave_needed);
      needs.compute_scratch_waves = MAX2(needs.compute_scratch_waves, cmd_buffer->queue_state.compute_scratch_waves_wanted);
      needs.esgs_ring_size = MAX2(needs.esgs_ring_size, cmd_buffer->queue_state.esgs_ring_size_needed);
      needs.gsvs_ring_size = MAX2(needs.gsvs_ring_size, cmd_buffer->queue_state.gsvs_ring_size_needed);
      needs.tess_rings |= cmd_buffer->queue_state.tess_rings_needed;
      needs.task_rings |= cmd_buffer->queue_state.task_rings_needed;
      needs.mesh_scratch_ring |= cmd_buffer->queue_state.mesh_scratch_ring_needed;
      needs.gds |= cmd_buffer->queue_state.gds_needed;
      needs.gds_oa |= cmd_buffer->queue_state.gds_oa_needed;
      needs.sample_positions |= cmd_buffer->queue_state.sample_positions_needed;
      *use_perf_counters |= cmd_buffer->queue_state.uses_perf_counters;
      *has_follower |= !!cmd_buffer->gang.cs;
   }

#ifdef HAVE_ORBIS_PLATFORM
   /* ⚠ THE TESSELLATION RINGS ARE ALLOCATED WHETHER OR NOT ANYTHING TESSELLATES, because on this console the
    * alternative is inheriting Sony's.
    *
    * VGT_TF_RING_SIZE, VGT_HS_OFFCHIP_PARAM and VGT_TF_MEMORY_BASE are written for GFX7 through
    * ac_cmdbuf_set_ucfg_reg_seq (ac_cmdbuf_cp.c:358) - USER-CONFIG, not context state, so they are not reset
    * by a submission and hold whatever the last writer left there, across submissions AND across processes.
    * RADV writes them only from radv_emit_tess_factor_ring(), which returns immediately when there is no tess
    * ring BO - so a title that never tessellates never writes them at all. Under amdgpu that is harmless,
    * because the kernel never hands a process a GPU whose registers point outside its address space. Here the
    * last writer was libSceGnmDriver:
    *
    *   sceGnmGetTheTessellationFactorRingBufferBaseAddress()  ->  0xff0000000
    *   sceGnmGetOffChipTessellationBufferSize()               ->  0x800000
    *
    * WHY THAT ADDRESS APPEARS IN NO BUFFER, NO DESCRIPTOR AND NO PACKET OF OURS: the register takes va >> 8,
    * so the only number that exists anywhere is 0x0ff00000, and it lives in a REGISTER rather than in memory.
    * An exact 64-bit scan over 580 MiB of mapped memory found 0xff0000000 zero times, and that is why.
    *
    * It also predicts the shape of the hang measured here. The factor ring sits at base + offchip_size, so
    * 0xff0000000 + 0x800000 = 0xff0800000 - which is exactly where ORBIS_DECOY_VA=0xff0000000:8 stops. The
    * decoy covers Sony's offchip buffer and not one byte of the factor ring, so reads through it succeed and
    * writes to the factor ring land in nothing.
    *
    * So this is a lifetime rule of the same shape as the buffer one, one level down: state this driver did
    * not program is state it does not own.
    *
    * The GS rings need no equivalent: on GFX6-8 their addresses travel in ring descriptors written to user
    * data (radv_set_ring_buffer), not in a register that outlives the submission.
    *
    * ⚠ WHAT IS DELIBERATELY NOT TAKEN FROM THE EARLIER EFFORT: it also multiplies the ring ALLOCATION by four
    * by default (ORBIS_TESS_RING_SCALE), after seeing a write fault one page past a correctly programmed
    * ring. Its own comment lists two competing explanations - a short sizing from UNCITED CU topology, or
    * OFFCHIP_BUFFERING being read per-SE - and says the next run separates them. That run has not happened,
    * so the knob is an open question rather than a fix, and defaulting it on would import the question as a
    * fact. If the fault reappears with the rings ours, THAT is the measurement which decides it.
    */
   needs.tess_rings = true;
#endif

   /* Sanitize scratch size information. */
   needs.scratch_waves =
      needs.scratch_size_per_wave ? MIN2(needs.scratch_waves, UINT32_MAX / needs.scratch_size_per_wave) : 0;
   needs.compute_scratch_waves =
      needs.compute_scratch_size_per_wave
         ? MIN2(needs.compute_scratch_waves, UINT32_MAX / needs.compute_scratch_size_per_wave)
         : 0;

   /* Compute the optimal scratch wavesize. */
   needs.scratch_size_per_wave = ac_compute_scratch_wavesize(&pdev->info, needs.scratch_size_per_wave);
   needs.compute_scratch_size_per_wave = ac_compute_scratch_wavesize(&pdev->info, needs.compute_scratch_size_per_wave);

   if (pdev->info.gfx_level >= GFX11 && queue->qf == RADV_QUEUE_GENERAL) {
      needs.ge_rings = true;
   }

   /* Return early if we already match these needs.
    * Note that it's not possible for any of the needed values to be less
    * than what the queue already had, because we only ever increase the allocated size.
    */
   if (queue->initial_full_flush_preamble_cs && queue->ring_info.scratch_size_per_wave == needs.scratch_size_per_wave &&
       queue->ring_info.scratch_waves == needs.scratch_waves &&
       queue->ring_info.compute_scratch_size_per_wave == needs.compute_scratch_size_per_wave &&
       queue->ring_info.compute_scratch_waves == needs.compute_scratch_waves &&
       queue->ring_info.esgs_ring_size == needs.esgs_ring_size &&
       queue->ring_info.gsvs_ring_size == needs.gsvs_ring_size && queue->ring_info.tess_rings == needs.tess_rings &&
       queue->ring_info.task_rings == needs.task_rings &&
       queue->ring_info.mesh_scratch_ring == needs.mesh_scratch_ring && queue->ring_info.ge_rings == needs.ge_rings &&
       queue->ring_info.gds == needs.gds && queue->ring_info.gds_oa == needs.gds_oa &&
       queue->ring_info.sample_positions == needs.sample_positions)
      return VK_SUCCESS;

   return radv_update_preamble_cs(queue, device, &needs, secure);
}

/**
 * Creates a postamble CS that executes cache flush commands
 * that we can use at the end of each submission.
 *
 * GFX6:
 * The kernel uses an EVENT_WRITE_EOP packet for signalling the
 * fence after each submission. Due to a firmware bug, this
 * packet can't wait for L2 writeback to finish, even with
 * the INV_L2 bit set, because it doesn't properly program
 * the CP_COHER_SIZE register.
 * As a workaround, the kernel uses a SURFACE_SYNC before the
 * fence. That has the side effect of flushing the L2 cache
 * while shaders are still in flight, which is suboptimal,
 * and is also incorrect as the shader may still write L2.
 *
 * GFX7:
 * The kernel uses an EVENT_WRITE_EOP packet for signalling the
 * fence after each submission. Due to a firmware bug, this
 * packet can't wait for L2 writeback to finish. As a workaround,
 * the kernel emits the EVENT_WRITE_EOP packet twice, but that
 * may just reduce the likelihood of the issue and may not fully
 * mitigate it.
 * We see random hangs on Hawaii which can be solved by
 * an L2 cache flush at the end of each submission.
 * Let's also wait for shaders to finish to avoid flushing
 * the cache while shaders are still in flight.
 *
 * Until we find a better way to mitigate the problems in the kernel,
 * let's wait for shaders to finish and then do a full flush
 * of all caches to be sure.
 */
static VkResult
radv_create_flush_postamble(struct radv_queue *queue)
{
   const struct radv_device *device = radv_queue_device(queue);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const enum amd_ip_type ip = radv_queue_family_to_ring(pdev, queue->state.qf);
   const bool is_mec = ip == AMD_IP_COMPUTE && pdev->info.gfx_level >= GFX7;
   struct radeon_winsys *ws = device->ws;
   struct radv_cmd_stream *cs;
   VkResult result;

   result = radv_create_cmd_stream(device, ip, false, &cs);
   if (result != VK_SUCCESS)
      return result;

   radeon_check_space(ws, cs->b, 256);

   ac_cmdbuf_begin(cs->b);
   if (ip == AMD_IP_GFX) {
      ac_cmdbuf_event_write(V_028A90_VS_PARTIAL_FLUSH);
      ac_cmdbuf_event_write(V_028A90_PS_PARTIAL_FLUSH);
      ac_cmdbuf_event_write(V_028A90_FLUSH_AND_INV_CB_META);
      ac_cmdbuf_event_write(V_028A90_FLUSH_AND_INV_DB_META);
   }
   ac_cmdbuf_event_write(V_028A90_CS_PARTIAL_FLUSH);
   ac_cmdbuf_end();

   if (!is_mec)
      ac_emit_cp_pfp_sync_me(cs->b, false);

   uint32_t cp_coher_cntl =
      S_0085F0_TC_ACTION_ENA(1) |
      S_0085F0_TCL1_ACTION_ENA(1);

   if (ip == AMD_IP_GFX)
      cp_coher_cntl |=
         S_0085F0_CB_ACTION_ENA(1) |
         S_0085F0_CB0_DEST_BASE_ENA(1) |
         S_0085F0_CB1_DEST_BASE_ENA(1) |
         S_0085F0_CB2_DEST_BASE_ENA(1) |
         S_0085F0_CB3_DEST_BASE_ENA(1) |
         S_0085F0_CB4_DEST_BASE_ENA(1) |
         S_0085F0_CB5_DEST_BASE_ENA(1) |
         S_0085F0_CB6_DEST_BASE_ENA(1) |
         S_0085F0_CB7_DEST_BASE_ENA(1) |
         S_0085F0_DB_ACTION_ENA(1) |
         S_0085F0_DB_DEST_BASE_ENA(1);

   ac_emit_cp_acquire_mem(cs->b, pdev->info.gfx_level, cs->hw_ip, V_581A_PREFETCH_PARSER, cp_coher_cntl);

   result = radv_finalize_cmd_stream(device, cs);
   if (result != VK_SUCCESS) {
      radv_destroy_cmd_stream(device, cs);
      return result;
   }

   queue->state.flush_postamble_cs = cs;
   return VK_SUCCESS;
}

static VkResult
radv_create_gang_wait_preambles_postambles(struct radv_queue *queue)
{
   struct radv_device *device = radv_queue_device(queue);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const enum amd_ip_type ip = radv_queue_family_to_ring(pdev, queue->state.qf);

   if (queue->gang_sem_bo)
      return VK_SUCCESS;

   VkResult r = VK_SUCCESS;
   struct radeon_winsys *ws = device->ws;
   struct radeon_winsys_bo *gang_sem_bo = NULL;
   enum radeon_bo_flag gang_sem_bo_flags = RADEON_FLAG_NO_INTERPROCESS_SHARING | RADEON_FLAG_ZERO_VRAM;

   /* When the "gang leader" is SDMA, we need to ensure that the gang semaphores BO
    * is coherent between SDMA and CP. To achieve this, we need bypass the L2 cache
    * when either SDMA or CP are connected to L2.
    *
    * GFX6-8:
    *   neither CP nor SDMA are connected to L2
    * GFX9:
    *   CP is connected to L2, SDMA isn't connected to L2
    * GFX10:
    *   CP is connected to L2
    *   SDMA is connected to L2 but there are possible hw bugs with
    *   cache_policy=BYPASS which is currently the default configuration set by SDMA0_UTCL1_PAGE
    * GFX10.3-11.5:
    *   CP is connected to L2
    *   SDMA is connected to L2, and an alternative solution would be to
    *   configure the cache policy in SDMA packets (ie. with CVP=1) directly to overwrite the
    *   default behavior, but it doesn't seem worth it.
    * GFX12:
    *   neither CP nor SDMA are connected to L2
    */
   if (ip == AMD_IP_SDMA && pdev->info.gfx_level >= GFX9 && pdev->info.gfx_level < GFX12)
      gang_sem_bo_flags |= RADEON_FLAG_GL2_BYPASS;

   /* Gang semaphores BO.
    * DWORD 0: used in preambles, gang leader writes, gang members wait.
    * DWORD 1: used in postambles, gang leader waits, gang members write.
    */
   r = radv_bo_create(device, NULL, 8, 4, RADEON_DOMAIN_VRAM, gang_sem_bo_flags, RADV_BO_PRIORITY_SCRATCH, 0, true,
                      &gang_sem_bo);
   if (r != VK_SUCCESS)
      return r;

   struct radv_cmd_stream *leader_pre_cs = NULL, *leader_post_cs = NULL;
   struct radv_cmd_stream *ace_pre_cs = NULL, *ace_post_cs = NULL;

   r = radv_create_cmd_stream(device, ip, false, &leader_pre_cs);
   if (r != VK_SUCCESS)
      goto fail;

   r = radv_create_cmd_stream(device, ip, false, &leader_post_cs);
   if (r != VK_SUCCESS)
      goto fail;

   r = radv_create_cmd_stream(device, AMD_IP_COMPUTE, false, &ace_pre_cs);
   if (r != VK_SUCCESS)
      goto fail;

   r = radv_create_cmd_stream(device, AMD_IP_COMPUTE, false, &ace_post_cs);
   if (r != VK_SUCCESS)
      goto fail;

   radeon_check_space(ws, leader_pre_cs->b, 256);
   radeon_check_space(ws, leader_post_cs->b, 256);
   radeon_check_space(ws, ace_pre_cs->b, 256);
   radeon_check_space(ws, ace_post_cs->b, 256);

   radv_cs_add_buffer(ws, leader_pre_cs->b, gang_sem_bo);
   radv_cs_add_buffer(ws, leader_post_cs->b, gang_sem_bo);
   radv_cs_add_buffer(ws, ace_pre_cs->b, gang_sem_bo);
   radv_cs_add_buffer(ws, ace_post_cs->b, gang_sem_bo);

   const uint64_t ace_wait_va = radv_buffer_get_va(gang_sem_bo);
   const uint64_t leader_wait_va = ace_wait_va + 4;
   const uint32_t zero = 0;
   const uint32_t one = 1;

   /* Preambles for gang submission.
    * Make gang members wait until the gang leader starts.
    * Userspace is required to emit this wait to make sure it behaves correctly
    * in a multi-process environment, because task shader dispatches are not
    * meant to be executed on multiple compute engines at the same time.
    */
   radv_cp_wait_mem(ace_pre_cs, WAIT_REG_MEM_GREATER_OR_EQUAL, ace_wait_va, 1, 0xffffffff);
   radv_cs_write_data(device, ace_pre_cs, V_371_MICRO_ENGINE, ace_wait_va, 1, &zero, false);
   radv_cs_write_data(device, leader_pre_cs, V_371_MICRO_ENGINE, ace_wait_va, 1, &one, false);
   /* Create postambles for gang submission.
    * This ensures that the gang leader waits for the whole gang,
    * which is necessary because the kernel signals the userspace fence
    * as soon as the gang leader is done, which may lead to bugs because the
    * same command buffers could be submitted again while still being executed.
    */
   radv_cp_wait_mem(leader_post_cs, WAIT_REG_MEM_GREATER_OR_EQUAL, leader_wait_va, 1, 0xffffffff);
   radv_cs_write_data(device, leader_post_cs, V_371_MICRO_ENGINE, leader_wait_va, 1, &zero, false);
   radv_cs_emit_write_event_eop(ace_post_cs, pdev->info.gfx_level, V_028A90_BOTTOM_OF_PIPE_TS, 0, EOP_DST_SEL_MEM,
                                EOP_INT_SEL_SEND_DATA_AFTER_WR_CONFIRM, EOP_DATA_SEL_VALUE_32BIT, leader_wait_va, 1, 0);

   r = radv_finalize_cmd_stream(device, leader_pre_cs);
   if (r != VK_SUCCESS)
      goto fail;
   r = radv_finalize_cmd_stream(device, leader_post_cs);
   if (r != VK_SUCCESS)
      goto fail;
   r = radv_finalize_cmd_stream(device, ace_pre_cs);
   if (r != VK_SUCCESS)
      goto fail;
   r = radv_finalize_cmd_stream(device, ace_post_cs);
   if (r != VK_SUCCESS)
      goto fail;

   queue->gang_sem_bo = gang_sem_bo;
   queue->state.gang_wait_preamble_cs = leader_pre_cs;
   queue->state.gang_wait_postamble_cs = leader_post_cs;
   queue->follower_state->gang_wait_preamble_cs = ace_pre_cs;
   queue->follower_state->gang_wait_postamble_cs = ace_post_cs;

   return VK_SUCCESS;

fail:
   if (leader_pre_cs)
      radv_destroy_cmd_stream(device, leader_pre_cs);
   if (leader_post_cs)
      radv_destroy_cmd_stream(device, leader_post_cs);
   if (ace_pre_cs)
      radv_destroy_cmd_stream(device, ace_pre_cs);
   if (ace_post_cs)
      radv_destroy_cmd_stream(device, ace_post_cs);
   if (gang_sem_bo)
      radv_bo_destroy(device, &queue->vk.base, gang_sem_bo);

   return r;
}

static bool
radv_queue_init_follower_state(struct radv_queue *queue)
{
   if (queue->follower_state)
      return true;

   queue->follower_state = calloc(1, sizeof(struct radv_queue_state));
   if (!queue->follower_state)
      return false;

   queue->follower_state->qf = RADV_QUEUE_COMPUTE;
   return true;
}

static VkResult
radv_update_gang_preambles(struct radv_queue *queue, bool secure)
{
   struct radv_device *device = radv_queue_device(queue);

   if (!radv_queue_init_follower_state(queue))
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   VkResult r = VK_SUCCESS;

   /* Copy task rings state.
    * Task shaders that are submitted on the ACE queue need to share
    * their ring buffers with the mesh shaders on the GFX queue.
    */
   queue->follower_state->ring_info.task_rings = queue->state.ring_info.task_rings;
   queue->follower_state->task_rings_bo = queue->state.task_rings_bo;

   /* Copy some needed states from the parent queue state.
    * These can only increase so it's okay to copy them as-is without checking.
    * Note, task shaders use the scratch size from their graphics pipeline.
    */
   struct radv_queue_ring_info needs = queue->follower_state->ring_info;
   needs.compute_scratch_size_per_wave = queue->state.ring_info.scratch_size_per_wave;
   needs.compute_scratch_waves = queue->state.ring_info.scratch_waves;
   needs.task_rings = queue->state.ring_info.task_rings;

   r = radv_update_preamble_cs(queue->follower_state, device, &needs, secure);
   if (r != VK_SUCCESS)
      return r;

   r = radv_create_gang_wait_preambles_postambles(queue);
   if (r != VK_SUCCESS)
      return r;

   return VK_SUCCESS;
}

static struct radv_cmd_stream *
radv_create_perf_counter_lock_cs(struct radv_device *device, unsigned pass, bool unlock)
{
   struct radv_cmd_stream **cs_ref = &device->perf_counter_lock_cs[pass * 2 + (unlock ? 1 : 0)];
   struct radv_cmd_stream *cs;
   VkResult result;

   if (*cs_ref)
      return *cs_ref;

   result = radv_create_cmd_stream(device, AMD_IP_GFX, false, &cs);
   if (result != VK_SUCCESS)
      return NULL;

   ASSERTED unsigned cdw = radeon_check_space(device->ws, cs->b, 21);

   radv_cs_add_buffer(device->ws, cs->b, device->perf_counter_bo);

   if (!unlock) {
      uint64_t mutex_va = radv_buffer_get_va(device->perf_counter_bo) + PERF_CTR_BO_LOCK_OFFSET;

      ac_emit_cp_atomic_mem(cs->b, V_1E1_GL2_OP_ATOMIC_CMPSWAP_32, V_1E1_LOOP_UNTIL_COMPARE_SATISFIED, mutex_va, 1, 0);
   }

   uint64_t va = radv_buffer_get_va(device->perf_counter_bo) + PERF_CTR_BO_PASS_OFFSET;
   uint64_t unset_va = va + (unlock ? 8 * pass : 0);
   uint64_t set_va = va + (unlock ? 0 : 8 * pass);

   ac_emit_cp_copy_data(cs->b, COPY_DATA_IMM, COPY_DATA_DST_MEM, 0, unset_va,
                        AC_CP_COPY_DATA_COUNT_SEL | AC_CP_COPY_DATA_WR_CONFIRM, false);

   ac_emit_cp_copy_data(cs->b, COPY_DATA_IMM, COPY_DATA_DST_MEM, 1, set_va,
                        AC_CP_COPY_DATA_COUNT_SEL | AC_CP_COPY_DATA_WR_CONFIRM, false);

   if (unlock) {
      uint64_t mutex_va = radv_buffer_get_va(device->perf_counter_bo) + PERF_CTR_BO_LOCK_OFFSET;

      ac_emit_cp_copy_data(cs->b, COPY_DATA_IMM, COPY_DATA_DST_MEM, 0, mutex_va,
                           AC_CP_COPY_DATA_COUNT_SEL | AC_CP_COPY_DATA_WR_CONFIRM, false);
   }

   assert(cs->b->cdw <= cdw);

   result = radv_finalize_cmd_stream(device, cs);
   if (result != VK_SUCCESS) {
      radv_destroy_cmd_stream(device, cs);
      return NULL;
   }

   /* All the casts are to avoid MSVC errors around pointer truncation in a non-taken
    * alternative.
    */
   if (p_atomic_cmpxchg((uintptr_t *)cs_ref, 0, (uintptr_t)cs) != 0) {
      radv_destroy_cmd_stream(device, cs);
   }

   return *cs_ref;
}

static void
radv_get_shader_upload_sync_wait(struct radv_device *device, uint64_t shader_upload_seq,
                                 struct vk_sync_wait *out_sync_wait)
{
   struct vk_semaphore *semaphore = vk_semaphore_from_handle(device->shader_upload_sem);
   struct vk_sync *sync = vk_semaphore_get_active_sync(semaphore);
   *out_sync_wait = (struct vk_sync_wait){
      .sync = sync,
      .wait_value = shader_upload_seq,
      .stage_mask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
   };
}

static VkResult
radv_queue_submit_normal(struct radv_queue *queue, struct vk_queue_submit *submission)
{
   struct radv_device *device = radv_queue_device(queue);
   struct radeon_winsys_ctx *ctx = queue->hw_ctx;
   bool use_ace = false;
   bool use_perf_counters = false;
   VkResult result;
   uint64_t shader_upload_seq = 0;
   uint32_t wait_count = submission->wait_count;
   struct vk_sync_wait *waits = submission->waits;

   result = radv_update_preambles(&queue->state, device, submission->command_buffers, submission->command_buffer_count,
                                  &use_perf_counters, &use_ace, submission->is_protected);
   if (result != VK_SUCCESS)
      return result;

   if (use_ace) {
      result = radv_update_gang_preambles(queue, submission->is_protected);
      if (result != VK_SUCCESS)
         return result;
   }

#ifdef HAVE_ORBIS_PLATFORM
   /* ⚠ THE OTHER HALF OF THE USE-AFTER-FREE AUDIT, and the half a command stream cannot supply.
    *
    * The arm already checks every user-data pointer against the ranges of DESTROYED descriptor pools and
    * has found nothing. What it structurally cannot see is vkResetDescriptorPool: that frees every set and
    * hands the same addresses straight back out, so a binding recorded before the reset and one recorded
    * after it are byte-identical in PM4.
    *
    * Comparing the pool's generation at record time with its generation now separates them exactly. A pool
    * that has vanished entirely is separated by the live registry, without dereferencing a freed pointer. */
   for (uint32_t i = 0; i < submission->command_buffer_count; i++) {
      const struct radv_cmd_buffer *const cb =
         container_of(submission->command_buffers[i], struct radv_cmd_buffer, vk);
      static unsigned said;

      if (cb->orbis_pool_stamps_dropped != 0 && said < 8) {
         said++;
         mesa_logw("radv/orbis: a command buffer binds sets from more descriptor pools than the audit "
                   "records (%u dropped) - the check below covers a subset",
                   cb->orbis_pool_stamps_dropped);
      }

      for (unsigned k = 0; k < cb->orbis_pool_stamps && said < 8; k++) {
         const struct radv_descriptor_pool *const pool = cb->orbis_pool_stamp[k].pool;

         if (!radv_orbis_pool_is_live(pool)) {
            said++;
            mesa_loge("radv/orbis: USE AFTER FREE - a command buffer in this submission binds a descriptor "
                      "set from a pool that has been DESTROYED. On this platform its memory stays mapped, "
                      "so every bounds check passes and the shader reads whoever owns it now.");
         } else if (pool->orbis_generation != cb->orbis_pool_stamp[k].generation) {
            said++;
            mesa_loge("radv/orbis: USE AFTER FREE - a command buffer in this submission binds a descriptor "
                      "set from a pool that has been RESET since the binding was recorded (generation %u "
                      "at record time, %u now). The addresses are identical, which is why nothing in the "
                      "command stream could show this.",
                      cb->orbis_pool_stamp[k].generation, pool->orbis_generation);
         }
      }
   }
#endif

   const unsigned cmd_buffer_count = submission->command_buffer_count;
   const unsigned max_cs_submission = radv_device_fault_detection_enabled(device) ? 1 : cmd_buffer_count;
   const unsigned cs_array_size = (use_ace ? 2 : 1) * MIN2(max_cs_submission, cmd_buffer_count);

   struct ac_cmdbuf **cs_array = malloc(sizeof(struct ac_cmdbuf *) * cs_array_size);
   if (!cs_array)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   if (radv_device_fault_detection_enabled(device))
      simple_mtx_lock(&device->trace_mtx);

   for (uint32_t j = 0; j < submission->command_buffer_count; j++) {
      struct radv_cmd_buffer *cmd_buffer = (struct radv_cmd_buffer *)submission->command_buffers[j];
      shader_upload_seq = MAX2(shader_upload_seq, cmd_buffer->queue_state.shader_upload_seq);
   }

   if (shader_upload_seq > queue->last_shader_upload_seq) {
      /* Patch the wait array to add waiting for referenced shaders to upload. */
      struct vk_sync_wait *new_waits = malloc(sizeof(struct vk_sync_wait) * (wait_count + 1));
      if (!new_waits) {
         result = VK_ERROR_OUT_OF_HOST_MEMORY;
         goto fail;
      }

      memcpy(new_waits, submission->waits, sizeof(struct vk_sync_wait) * submission->wait_count);
      radv_get_shader_upload_sync_wait(device, shader_upload_seq, &new_waits[submission->wait_count]);

      waits = new_waits;
      wait_count += 1;
   }

   /* For fences on the same queue/vm amdgpu doesn't wait till all processing is finished
    * before starting the next cmdbuffer, so we need to do it here.
    */
   const bool need_wait = wait_count > 0;
   unsigned num_initial_preambles = 0;
   unsigned num_continue_preambles = 0;
   unsigned num_postambles = 0;
   struct ac_cmdbuf *initial_preambles[5] = {0};
   struct ac_cmdbuf *continue_preambles[5] = {0};
   struct ac_cmdbuf *postambles[4] = {0};

   if (queue->state.qf == RADV_QUEUE_GENERAL || queue->state.qf == RADV_QUEUE_COMPUTE) {
      initial_preambles[num_initial_preambles++] =
         need_wait ? queue->state.initial_full_flush_preamble_cs->b : queue->state.initial_preamble_cs->b;

      continue_preambles[num_continue_preambles++] = queue->state.continue_preamble_cs->b;

      if (use_perf_counters) {
         /* RADV only supports perf counters on the GFX queue currently. */
         assert(queue->state.qf == RADV_QUEUE_GENERAL);

         /* Create the lock/unlock CS. */
         struct radv_cmd_stream *perf_ctr_lock_cs =
            radv_create_perf_counter_lock_cs(device, submission->perf_pass_index, false);
         struct radv_cmd_stream *perf_ctr_unlock_cs =
            radv_create_perf_counter_lock_cs(device, submission->perf_pass_index, true);

         if (!perf_ctr_lock_cs || !perf_ctr_unlock_cs) {
            result = VK_ERROR_OUT_OF_HOST_MEMORY;
            goto fail;
         }

         initial_preambles[num_initial_preambles++] = perf_ctr_lock_cs->b;
         continue_preambles[num_continue_preambles++] = perf_ctr_lock_cs->b;
         postambles[num_postambles++] = perf_ctr_unlock_cs->b;
      }
   }

   if (queue->state.flush_postamble_cs) {
      postambles[num_postambles++] = queue->state.flush_postamble_cs->b;
   }

   const unsigned num_1q_initial_preambles = num_initial_preambles;
   const unsigned num_1q_continue_preambles = num_continue_preambles;
   const unsigned num_1q_postambles = num_postambles;

   if (use_ace) {
      initial_preambles[num_initial_preambles++] = queue->state.gang_wait_preamble_cs->b;
      initial_preambles[num_initial_preambles++] = queue->follower_state->gang_wait_preamble_cs->b;
      initial_preambles[num_initial_preambles++] = need_wait ? queue->follower_state->initial_full_flush_preamble_cs->b
                                                             : queue->follower_state->initial_preamble_cs->b;

      continue_preambles[num_continue_preambles++] = queue->state.gang_wait_preamble_cs->b;
      continue_preambles[num_continue_preambles++] = queue->follower_state->gang_wait_preamble_cs->b;
      continue_preambles[num_continue_preambles++] = queue->follower_state->continue_preamble_cs->b;

      postambles[num_postambles++] = queue->follower_state->gang_wait_postamble_cs->b;
      postambles[num_postambles++] = queue->state.gang_wait_postamble_cs->b;
   }

   struct radv_winsys_submit_info submit = {
      .ip_type = radv_queue_ring(queue),
      .queue_index = queue->vk.index_in_family,
      .cs_array = cs_array,
      .cs_count = 0,
      .initial_preamble_count = num_1q_initial_preambles,
      .continue_preamble_count = num_1q_continue_preambles,
      .postamble_count = num_1q_postambles,
      .initial_preamble_cs = initial_preambles,
      .continue_preamble_cs = continue_preambles,
      .postamble_cs = postambles,
      .uses_shadow_regs = queue->state.uses_shadow_regs,
      .secure = submission->is_protected,
   };

   for (uint32_t j = 0, advance; j < cmd_buffer_count; j += advance) {
      advance = MIN2(max_cs_submission, cmd_buffer_count - j);
      const bool last_submit = j + advance == cmd_buffer_count;
      bool submit_ace = false;
      unsigned num_submitted_cs = 0;

      if (radv_device_fault_detection_enabled(device))
         device->trace_data->primary_id = 0;

      struct ac_cmdbuf *chainable = NULL;
      struct ac_cmdbuf *chainable_ace = NULL;

      /* Add CS from submitted command buffers. */
      for (unsigned c = 0; c < advance; ++c) {
         struct radv_cmd_buffer *cmd_buffer = (struct radv_cmd_buffer *)submission->command_buffers[j + c];
         assert(cmd_buffer->vk.level == VK_COMMAND_BUFFER_LEVEL_PRIMARY);
         const bool can_chain_next = !(cmd_buffer->usage_flags & VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT);
         struct radv_cmd_stream *cs = cmd_buffer->cs;
         struct radv_cmd_stream *ace_cs = cmd_buffer->gang.cs;

         /* Follower needs to be before the gang leader because the last CS must match the queue's IP type. */
         if (ace_cs) {
            device->ws->cs_unchain(ace_cs->b);
            if (!chainable_ace || !device->ws->cs_chain(chainable_ace, ace_cs->b, false)) {
               cs_array[num_submitted_cs++] = ace_cs->b;

               /* Prevent chaining the gang leader when the follower couldn't be chained.
                * Otherwise, they would be in the wrong order.
                */
               chainable = NULL;
            }

            chainable_ace = can_chain_next ? ace_cs->b : NULL;
            submit_ace = true;
         }

         device->ws->cs_unchain(cs->b);
         if (!chainable || !device->ws->cs_chain(chainable, cs->b, queue->state.uses_shadow_regs)) {
            /* don't submit empty command buffers to the kernel. */
            if ((radv_queue_ring(queue) != AMD_IP_VCN_ENC && radv_queue_ring(queue) != AMD_IP_UVD) || cs->b->cdw != 0)
               cs_array[num_submitted_cs++] = cs->b;
         }

         chainable = can_chain_next ? cs->b : NULL;
      }

      submit.cs_count = num_submitted_cs;
      submit.initial_preamble_count = submit_ace ? num_initial_preambles : num_1q_initial_preambles;
      submit.continue_preamble_count = submit_ace ? num_continue_preambles : num_1q_continue_preambles;
      submit.postamble_count = submit_ace ? num_postambles : num_1q_postambles;

      result = device->ws->cs_submit(ctx, &submit, j == 0 ? wait_count : 0, waits,
                                     last_submit ? submission->signal_count : 0, submission->signals);

      if (result != VK_SUCCESS)
         goto fail;

      if (radv_device_fault_detection_enabled(device)) {
         result = radv_check_gpu_hangs(queue, &submit);
      }

      if (device->tma_bo) {
         radv_check_trap_handler(queue);
      }

      initial_preambles[0] = queue->state.initial_preamble_cs ? queue->state.initial_preamble_cs->b : NULL;
      initial_preambles[1] = !use_ace ? NULL : queue->follower_state->initial_preamble_cs->b;
   }

   queue->last_shader_upload_seq = MAX2(queue->last_shader_upload_seq, shader_upload_seq);

   radv_dump_printf_data(device, stderr, true);

fail:
   free(cs_array);
   if (waits != submission->waits)
      free(waits);
   if (radv_device_fault_detection_enabled(device))
      simple_mtx_unlock(&device->trace_mtx);

   return result;
}

static void
radv_report_gpuvm_fault(struct radv_device *device)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct radv_winsys_gpuvm_fault_info fault_info = {0};

   if (!radv_vm_fault_occurred(device, &fault_info))
      return;

   fprintf(stderr, "radv: GPUVM fault detected at address 0x%08" PRIx64 ".\n", fault_info.addr);
   ac_print_gpuvm_fault_status(stderr, pdev->info.gfx_level, fault_info.status);
}

static VkResult
radv_queue_sparse_submit(struct vk_queue *vqueue, struct vk_queue_submit *submission)
{
   struct radv_queue *queue = (struct radv_queue *)vqueue;
   struct radv_device *device = radv_queue_device(queue);
   VkResult result;

   result = radv_queue_submit_bind_sparse_memory(device, submission);
   if (result != VK_SUCCESS)
      goto fail;

   /* We do a CPU wait here, in part to avoid more winsys mechanisms. In the likely kernel explicit
    * sync mechanism, we'd need to do a CPU wait anyway. Haven't seen this be a perf issue yet, but
    * we have to make sure the queue always has its submission thread enabled. */
   result = vk_sync_wait_many(&device->vk, submission->wait_count, submission->waits, 0, UINT64_MAX);
   if (result != VK_SUCCESS)
      goto fail;

   /* Ignore all the commandbuffers. They're necessarily empty anyway. */

   for (unsigned i = 0; i < submission->signal_count; ++i) {
      result = vk_sync_signal(&device->vk, submission->signals[i].sync, submission->signals[i].signal_value);
      if (result != VK_SUCCESS)
         goto fail;
   }

fail:
   if (result != VK_SUCCESS) {
      /* When something bad happened during the submission, such as
       * an out of memory issue, it might be hard to recover from
       * this inconsistent state. To avoid this sort of problem, we
       * assume that we are in a really bad situation and return
       * VK_ERROR_DEVICE_LOST to ensure the clients do not attempt
       * to submit the same job again to this device.
       */
      radv_report_gpuvm_fault(device);
      result = vk_device_set_lost(&device->vk, "vkQueueSubmit() failed");
   }
   return result;
}

static VkResult
radv_queue_submit(struct vk_queue *vqueue, struct vk_queue_submit *submission)
{
#ifdef HAVE_ORBIS_PLATFORM
   /* The watermark, read from the CPU between submissions. It runs backwards from the end, so a ring the
    * hardware barely touches costs a few pages of reading rather than the whole 9 MiB. */
   if (orbis_tess_ring_map != NULL) {
      static uint64_t nth;
      const uint64_t  every = strtoull(getenv("ORBIS_TESS_RING_WATERMARK"), NULL, 10);

      if (every != 0 && (++nth % every) == 0) {
         static uint64_t high_water;
         uint64_t        i = orbis_tess_ring_size;

         while (i > 0 && orbis_tess_ring_map[i - 1] == ORBIS_TESS_RING_POISON)
            --i;

         if (i > high_water) {
            high_water = i;
            mesa_logi("radv/orbis: tess ring watermark %" PRIu64 " B of %" PRIu64 " allocated - ac_gpu_info "
                      "sizes it at %u, so the hardware wants %.2fx that. A ring of %" PRIu64
                      " B would have held this frame.",
                      i, orbis_tess_ring_size, orbis_tess_ring_sized_at,
                      (double)i / (double)orbis_tess_ring_sized_at, i);
         }
      }
   }
#endif
   struct radv_queue *queue = (struct radv_queue *)vqueue;
   struct radv_device *device = radv_queue_device(queue);

   VkResult result = radv_queue_submit_bind_sparse_memory(device, submission);
   if (result != VK_SUCCESS)
      goto fail;

   if (!submission->command_buffer_count && !submission->wait_count && !submission->signal_count)
      return VK_SUCCESS;

   if (!submission->command_buffer_count) {
      result = radv_queue_submit_empty(queue, submission);
   } else {
      result = radv_queue_submit_normal(queue, submission);
   }

fail:
   if (result != VK_SUCCESS) {
      /* When something bad happened during the submission, such as
       * an out of memory issue, it might be hard to recover from
       * this inconsistent state. To avoid this sort of problem, we
       * assume that we are in a really bad situation and return
       * VK_ERROR_DEVICE_LOST to ensure the clients do not attempt
       * to submit the same job again to this device.
       */
      radv_report_gpuvm_fault(device);
      result = vk_device_set_lost(&device->vk, "vkQueueSubmit() failed");
   }
   return result;
}

bool
radv_queue_internal_submit(struct radv_queue *queue, struct ac_cmdbuf *cs)
{
   struct radv_device *device = radv_queue_device(queue);
   struct radeon_winsys_ctx *ctx = queue->hw_ctx;
   struct radv_winsys_submit_info submit = {
      .ip_type = radv_queue_ring(queue),
      .queue_index = queue->vk.index_in_family,
      .cs_array = &cs,
      .cs_count = 1,
   };

   VkResult result = device->ws->cs_submit(ctx, &submit, 0, NULL, 0, NULL);
   if (result != VK_SUCCESS)
      return false;

   return true;
}

int
radv_queue_init(struct radv_device *device, struct radv_queue *queue, int idx,
                const VkDeviceQueueCreateInfo *create_info,
                const VkDeviceQueueGlobalPriorityCreateInfo *global_priority)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);

   queue->priority = radv_get_queue_global_priority(global_priority);
   queue->state.qf = vk_queue_to_radv(pdev, create_info->queueFamilyIndex);

   if (queue->state.qf == RADV_QUEUE_VIDEO_ENC && device->hw_vcn_enc_ctx)
      queue->hw_ctx = device->hw_vcn_enc_ctx;
   else
      queue->hw_ctx = device->hw_ctx[queue->priority];

   VkResult result = vk_queue_init(&queue->vk, &device->vk, create_info, idx);
   if (result != VK_SUCCESS)
      return result;

   queue->state.uses_shadow_regs = device->uses_shadow_regs && queue->state.qf == RADV_QUEUE_GENERAL;
   if (queue->state.uses_shadow_regs) {
      result = radv_create_shadow_regs_preamble(device, &queue->state);
      if (result != VK_SUCCESS)
         goto fail;
      result = radv_init_shadowed_regs_buffer_state(device, queue);
      if (result != VK_SUCCESS)
         goto fail;
   }

   if (pdev->info.gfx_level <= GFX7 &&
       (queue->state.qf == RADV_QUEUE_GENERAL || queue->state.qf == RADV_QUEUE_COMPUTE)) {
      result = radv_create_flush_postamble(queue);
      if (result != VK_SUCCESS)
         goto fail;
   }

   if (queue->state.qf == RADV_QUEUE_SPARSE) {
      queue->vk.driver_submit = radv_queue_sparse_submit;
      vk_queue_enable_submit_thread(&queue->vk);
   } else {
      queue->vk.driver_submit = radv_queue_submit;
   }

   if (device->utrace.context) {
      VkCommandPoolCreateInfo pool_info = {
         .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
         .queueFamilyIndex = queue->vk.queue_family_index,
      };

      result =
         vk_common_CreateCommandPool(radv_device_to_handle(device), &pool_info, NULL, &queue->utrace_command_pool);
      if (result != VK_SUCCESS)
         goto fail;
   }

   /* Use internal synchronization for UVD < 6.3 to prevent a possible
    * race between application submissions and destroying the video session.
    */
   if (pdev->info.family < CHIP_POLARIS10 && queue->state.qf == RADV_QUEUE_VIDEO_DEC)
      queue->vk.internally_synchronized = true;

   return VK_SUCCESS;
fail:
   vk_queue_finish(&queue->vk);
   return result;
}

static void
radv_queue_state_finish(struct radv_queue_state *queue, struct radv_device *device)
{
   radv_destroy_shadow_regs_preamble(device, queue, device->ws);
   if (queue->initial_full_flush_preamble_cs)
      radv_destroy_cmd_stream(device, queue->initial_full_flush_preamble_cs);
   if (queue->initial_preamble_cs)
      radv_destroy_cmd_stream(device, queue->initial_preamble_cs);
   if (queue->continue_preamble_cs)
      radv_destroy_cmd_stream(device, queue->continue_preamble_cs);
   if (queue->gang_wait_preamble_cs)
      radv_destroy_cmd_stream(device, queue->gang_wait_preamble_cs);
   if (queue->gang_wait_postamble_cs)
      radv_destroy_cmd_stream(device, queue->gang_wait_postamble_cs);
   if (queue->flush_postamble_cs)
      radv_destroy_cmd_stream(device, queue->flush_postamble_cs);
   if (queue->descriptor_bo)
      radv_bo_destroy(device, NULL, queue->descriptor_bo);
   if (queue->scratch_bo) {
      radv_rmv_log_command_buffer_bo_destroy(device, queue->scratch_bo);
      radv_bo_destroy(device, NULL, queue->scratch_bo);
   }
   if (queue->esgs_ring_bo) {
      radv_rmv_log_command_buffer_bo_destroy(device, queue->esgs_ring_bo);
      radv_bo_destroy(device, NULL, queue->esgs_ring_bo);
   }
   if (queue->gsvs_ring_bo) {
      radv_rmv_log_command_buffer_bo_destroy(device, queue->gsvs_ring_bo);
      radv_bo_destroy(device, NULL, queue->gsvs_ring_bo);
   }
   if (queue->tess_rings_bo) {
      radv_rmv_log_command_buffer_bo_destroy(device, queue->tess_rings_bo);
      radv_bo_destroy(device, NULL, queue->tess_rings_bo);
   }
   if (queue->task_rings_bo) {
      radv_rmv_log_command_buffer_bo_destroy(device, queue->task_rings_bo);
      radv_bo_destroy(device, NULL, queue->task_rings_bo);
   }
   if (queue->mesh_scratch_ring_bo) {
      radv_rmv_log_command_buffer_bo_destroy(device, queue->mesh_scratch_ring_bo);
      radv_bo_destroy(device, NULL, queue->mesh_scratch_ring_bo);
   }
   if (queue->ge_rings_bo) {
      radv_rmv_log_command_buffer_bo_destroy(device, queue->ge_rings_bo);
      radv_bo_destroy(device, NULL, queue->ge_rings_bo);
   }
   if (queue->gds_bo) {
      device->ws->buffer_make_resident(device->ws, queue->gds_bo, false);
      radv_bo_destroy(device, NULL, queue->gds_bo);
   }
   if (queue->gds_oa_bo) {
      device->ws->buffer_make_resident(device->ws, queue->gds_oa_bo, false);
      radv_bo_destroy(device, NULL, queue->gds_oa_bo);
   }
   if (queue->compute_scratch_bo) {
      radv_rmv_log_command_buffer_bo_destroy(device, queue->compute_scratch_bo);
      radv_bo_destroy(device, NULL, queue->compute_scratch_bo);
   }
}

void
radv_queue_finish(struct radv_queue *queue)
{
   struct radv_device *device = radv_queue_device(queue);

   vk_common_DestroyCommandPool(radv_device_to_handle(device), queue->utrace_command_pool, NULL);

   if (queue->follower_state) {
      /* Prevent double free */
      queue->follower_state->task_rings_bo = NULL;

      /* Clean up the internal ACE queue state. */
      radv_queue_state_finish(queue->follower_state, device);
      free(queue->follower_state);
   }

   if (queue->gang_sem_bo)
      radv_bo_destroy(device, &queue->vk.base, queue->gang_sem_bo);

   radv_queue_state_finish(&queue->state, device);
   vk_queue_finish(&queue->vk);
}

enum amd_ip_type
radv_queue_ring(const struct radv_queue *queue)
{
   struct radv_device *device = radv_queue_device(queue);
   const struct radv_physical_device *pdev = radv_device_physical(device);
   return radv_queue_family_to_ring(pdev, queue->state.qf);
}

enum amd_ip_type
radv_queue_family_to_ring(const struct radv_physical_device *pdev, enum radv_queue_family f)
{
   switch (f) {
   case RADV_QUEUE_GENERAL:
      return AMD_IP_GFX;
   case RADV_QUEUE_COMPUTE:
      return AMD_IP_COMPUTE;
   case RADV_QUEUE_TRANSFER:
      return AMD_IP_SDMA;
   case RADV_QUEUE_VIDEO_DEC:
      return pdev->vid_decode_ip;
   case RADV_QUEUE_VIDEO_ENC:
      return AMD_IP_VCN_ENC;
   default:
      UNREACHABLE("Unknown queue family");
   }
}
