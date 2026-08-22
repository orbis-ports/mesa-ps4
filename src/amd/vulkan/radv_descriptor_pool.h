/*
 * Copyright © 2016 Bas Nieuwenhuizen
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef RADV_DESCRIPTOR_POOL_H
#define RADV_DESCRIPTOR_POOL_H

#include "vk_object.h"

#include <vulkan/vulkan.h>

#include "util/list.h"
#include "util/vma.h"

/* The vma heap reserves 0 to mean NULL; we have to offset by some amount to ensure we can allocate
 * the entire BO without hitting zero. The actual amount doesn't matter.
 */
#define RADV_POOL_HEAP_OFFSET 32

struct radv_descriptor_set;

struct radv_descriptor_pool {
   struct vk_object_base base;
   struct radeon_winsys_bo *bo;
   uint8_t *host_bo;
   uint8_t *mapped_ptr;
   uint64_t current_offset;
   uint64_t size;

   uint8_t *host_memory_base;
   uint8_t *host_memory_ptr;
   uint8_t *host_memory_end;

   struct list_head sets;

   struct util_vma_heap bo_heap;

   uint32_t entry_count;
   uint32_t max_entry_count;

#ifdef HAVE_ORBIS_PLATFORM
   /* ⚠ A GENERATION, BECAUSE ADDRESSES ALONE CANNOT TELL A STALE BINDING FROM A FRESH ONE.
    *
    * vkResetDescriptorPool frees every set and hands the SAME addresses straight back out, so a command
    * buffer recorded before the reset and one recorded after it bind byte-identical values. From the PM4
    * stream - which is all the arm sees - they are indistinguishable, and a range-based check would accuse
    * every legitimate re-binding.
    *
    * This is bumped whenever the pool's storage is invalidated (reset, or destruction). A command buffer
    * stamps the value it saw at record time, and the submit path compares. A mismatch is a set being used
    * after its storage was released, stated without interpretation.
    *
    * Only on this platform, because only here does the memory stay mapped afterwards: on Linux the pages go
    * and the bug is a fault at the point of use. */
   uint32_t orbis_generation;
#endif
};

#ifdef HAVE_ORBIS_PLATFORM
/* Whether a pool pointer still refers to a live pool. See the definition for why a stamp needs it. */
bool radv_orbis_pool_is_live(const struct radv_descriptor_pool *pool);
#endif

VK_DEFINE_NONDISP_HANDLE_CASTS(radv_descriptor_pool, base, VkDescriptorPool, VK_OBJECT_TYPE_DESCRIPTOR_POOL)

#endif /* RADV_DESCRIPTOR_POOL_H */
