/*
 * Copyright 2026 Mikołaj Mikołajczyk
 * SPDX-License-Identifier: MIT
 *
 * The device-query half of the PS4 winsys.
 *
 * WHY THIS FILE EXISTS AT ALL. RADV's physical-device creation calls exactly one function to fill
 * radeon_info - radv_amdgpu_winsys_query_info() - and that function lives in winsys/amdgpu/, which
 * -Dplatforms=orbis drops from the build. So with the winsys gone, NOTHING calls the thirteen ac_drm_*
 * queries in ac_orbis_drm.c: they were dead code until this file connected them. That was the first of
 * the three planning gaps found before implementation started.
 *
 * The amdgpu original is ~45 lines and only two of them are actually about DRM: opening a file
 * descriptor, and deriving the vk_sync type from it. Everything else - ac_drm_device_initialize,
 * ac_query_gpu_info, the video/SDMA/compute queue clamps - is platform-neutral, which is why this is a
 * sibling rather than a rewrite.
 */

#include "ac_gpu_info.h"
#include "ac_linux_drm.h"
#include "radv_radeon_winsys.h"
#include "tools/radv_debug.h"

/* ⚠ struct radeon_winsys_info is declared in the AMDGPU winsys's public header, even though it is a
 * generic structure and that winsys is not built here. The header itself is fine to include - the platform
 * exclusion drops the .c files, not the tree - and radv_physical_device.c includes it on the DRM arm for
 * the same reason. Moving the struct to radv_radeon_winsys.h would be tidier and is upstream's call. */
#include "winsys/amdgpu/radv_amdgpu_winsys_public.h"

#include "vk_drm_syncobj.h"
#include "vk_log.h"

#include "util/log.h"
#include "util/macros.h"

#include <string.h>

#define ORBIS_MAX_RINGS_PER_TYPE 8

VkResult radv_orbis_winsys_query_info(uint64_t debug_flags, struct radeon_winsys_info *info);

VkResult
radv_orbis_winsys_query_info(uint64_t debug_flags, struct radeon_winsys_info *info)
{
   uint32_t drm_major = 0, drm_minor = 0;
   ac_drm_device *dev = NULL;

   memset(info, 0, sizeof(*info));

   /* The fd is -1 and stays -1. ac_drm_device_initialize's PS4 arm does not open anything - there is no
    * /dev/dri - it allocates our own device object. Passing -1 rather than a fake positive number is
    * deliberate: if any code path ever uses this value as a real descriptor, a syscall on -1 fails loudly
    * instead of operating on whatever file happened to be open. */
   if (ac_drm_device_initialize(-1, false, &drm_major, &drm_minor, &dev)) {
      mesa_loge("radv/orbis: ac_drm_device_initialize failed");
      return VK_ERROR_INITIALIZATION_FAILED;
   }

   info->base.drm_major = drm_major;
   info->base.drm_minor = drm_minor;
   info->base.is_virtio = false;

   /* This is the call that reaches every ac_drm_query_* body. It stops at the first failure, which is
    * exactly the property phase 2 wants: each run gets one query further, and the log names the next one
    * to write. "Implement in the order RADV trips over them" stops being advice and becomes mechanical. */
   /* require_pci_bus_info = FALSE, and that argument is the whole reason ac_drm_query_pci_bus_info may be
    * refused. The amdgpu winsys passes true, and copying it verbatim made the very first query in
    * ac_query_gpu_info fatal: info->pci.valid = (query == 0), and with require_pci_bus_info the driver
    * returns AC_QUERY_GPU_INFO_FAIL right there (ac_gpu_info.c:1465). Passing false leaves pci.valid
    * false, which is the truth - there is no PCI bus on this machine to describe. */
   enum ac_query_gpu_info_result r =
      ac_query_gpu_info(-1, dev, &info->base, false, !(debug_flags & RADV_DEBUG_NO_CACHE_COMPAT));
   if (r != AC_QUERY_GPU_INFO_SUCCESS) {
      ac_drm_device_deinitialize(dev);
      return r == AC_QUERY_GPU_INFO_FAIL ? VK_ERROR_INITIALIZATION_FAILED : VK_ERROR_INCOMPATIBLE_DRIVER;
   }

   /* Verbatim from the amdgpu winsys, and neither clamp is about DRM. The first is a hardware erratum
    * comment worth keeping even though this build has no video queues; the second is RADV's own array
    * bound. */
   for (enum amd_ip_type ip_type = AMD_IP_UVD; ip_type <= AMD_IP_VCN_ENC; ip_type++)
      info->base.max_submitted_ibs[ip_type] = 1;

   /* The amdgpu winsys clamps these to MAX_RINGS_PER_TYPE, which is its own array bound
    * (winsys/amdgpu/radv_amdgpu_cs.h:21) and guards against a KERNEL reporting more rings than RADV can
    * track. Here we ARE the kernel - the numbers come out of our own ac_drm_query_hw_ip_info - so the
    * clamp is kept as a backstop with a local constant rather than by including that winsys's private
    * header. It must not exceed whatever ring arrays the PS4 winsys eventually carries. */
   info->base.ip[AMD_IP_SDMA].num_queues = MIN2(info->base.ip[AMD_IP_SDMA].num_queues, ORBIS_MAX_RINGS_PER_TYPE);
   info->base.ip[AMD_IP_COMPUTE].num_queues =
      MIN2(info->base.ip[AMD_IP_COMPUTE].num_queues, ORBIS_MAX_RINGS_PER_TYPE);

   /* THE ONE LINE THAT WOULD HAVE BEEN A SUBSYSTEM, AND IS NOT.
    *
    * The amdgpu winsys says vk_drm_syncobj_get_type(fd), which builds the whole vk_sync implementation -
    * fences, binary semaphores, timeline emulation - on top of DRM syncobj ioctls. There is no DRM here,
    * so that looked like a layer this port had to write.
    *
    * It does not: vk_drm_syncobj_get_type_from_provider() takes a util_sync_provider instead of a file
    * descriptor, and Intel's anv and freedreno's virtio backend already use it that way. Our provider is
    * the one ac_drm_device_get_sync_provider returns, so the entire vk_sync layer comes for free and
    * phase 5's five counter operations are the only thing left to write underneath it. */
   info->syncobj_sync_type = vk_drm_syncobj_get_type_from_provider(ac_drm_device_get_sync_provider(dev));

   /* ⚠ AND THE PROBE DEVICE IS FINISHED WITH HERE, WHICH THE SUCCESS PATH USED TO FORGET. The failure
    * path above deinitialises and this one did not, so every enumeration leaked one ac_drm_device and
    * its sync provider. Nothing outside this function refers to `dev`: radeon_info is a value, and
    * vk_drm_syncobj_get_type_from_provider returns a struct of function pointers that keeps no
    * reference to the provider it was probed with - it creates a syncobj, reads the capabilities off
    * it and destroys it again.
    *
    * Deinitialising does NOT tear the arena down - orbis_arena_setup is a once-guard and
    * ac_drm_device_deinitialize does not undo it - so the real device this enumeration leads to still
    * finds the address space where it left it. */
   ac_drm_device_deinitialize(dev);

   return VK_SUCCESS;
}
