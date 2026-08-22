/*
 * Copyright © 2021 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef AC_DRM_FOURCC_H
#define AC_DRM_FOURCC_H

#if !MESA_SYSTEM_HAS_KMS_DRM
#include <stdint.h>
/* uint64_t rather than a local `typedef uint64_t __u64`, which is what this arm used to carry.
 *
 * The typedef could not be written correctly. Unconditionally, it collides with Linux's own __u64 as
 * soon as anything pulls in <linux/types.h> - which happens on a HOST build of a no-DRM platform's arm,
 * the way such a driver is exercised without the device present. Guarded on __linux__, it tests the
 * wrong thing: whether __u64 has ALREADY been defined depends on include order, not on the platform, so
 * the next includer to sort differently gets "unknown type name '__u64'". There is no third spelling.
 * Since the name is only ever used to widen a constant, the type it aliases will do.
 */
#define DRM_FORMAT_MOD_VENDOR_NONE    0
#define DRM_FORMAT_MOD_VENDOR_AMD     0x02
#define DRM_FORMAT_RESERVED	      ((1ULL << 56) - 1)
#define fourcc_mod_code(vendor, val) \
	((((uint64_t)DRM_FORMAT_MOD_VENDOR_## vendor) << 56) | ((val) & 0x00ffffffffffffffULL))
#define DRM_FORMAT_MOD_INVALID	fourcc_mod_code(NONE, DRM_FORMAT_RESERVED)
#define DRM_FORMAT_MOD_LINEAR	fourcc_mod_code(NONE, 0)
#define AMD_FMT_MOD fourcc_mod_code(AMD, 0)
#define IS_AMD_FMT_MOD(val) (((val) >> 56) == DRM_FORMAT_MOD_VENDOR_AMD)
#define AMD_FMT_MOD_TILE_VER_GFX9 1
#define AMD_FMT_MOD_TILE_VER_GFX10 2
#define AMD_FMT_MOD_TILE_VER_GFX10_RBPLUS 3
#define AMD_FMT_MOD_TILE_VER_GFX11 4
#define AMD_FMT_MOD_TILE_VER_GFX12 5
#define AMD_FMT_MOD_TILE_GFX9_64K_S 9
#define AMD_FMT_MOD_TILE_GFX9_64K_D 10
#define AMD_FMT_MOD_TILE_GFX9_4K_D_X 22
#define AMD_FMT_MOD_TILE_GFX9_64K_S_X 25
#define AMD_FMT_MOD_TILE_GFX9_64K_D_X 26
#define AMD_FMT_MOD_TILE_GFX9_64K_R_X 27
#define AMD_FMT_MOD_TILE_GFX11_256K_R_X 31
#define AMD_FMT_MOD_TILE_GFX12_256B_2D 1
#define AMD_FMT_MOD_TILE_GFX12_4K_2D 2
#define AMD_FMT_MOD_TILE_GFX12_64K_2D 3
#define AMD_FMT_MOD_TILE_GFX12_256K_2D 4
#define AMD_FMT_MOD_DCC_BLOCK_64B 0
#define AMD_FMT_MOD_DCC_BLOCK_128B 1
#define AMD_FMT_MOD_DCC_BLOCK_256B 2
#define AMD_FMT_MOD_TILE_VERSION_SHIFT 0
#define AMD_FMT_MOD_TILE_VERSION_MASK 0xFF
#define AMD_FMT_MOD_TILE_SHIFT 8
#define AMD_FMT_MOD_TILE_MASK 0x1F
#define AMD_FMT_MOD_DCC_SHIFT 13
#define AMD_FMT_MOD_DCC_MASK 0x1
#define AMD_FMT_MOD_DCC_RETILE_SHIFT 14
#define AMD_FMT_MOD_DCC_RETILE_MASK 0x1
#define AMD_FMT_MOD_DCC_PIPE_ALIGN_SHIFT 15
#define AMD_FMT_MOD_DCC_PIPE_ALIGN_MASK 0x1
#define AMD_FMT_MOD_DCC_INDEPENDENT_64B_SHIFT 16
#define AMD_FMT_MOD_DCC_INDEPENDENT_64B_MASK 0x1
#define AMD_FMT_MOD_DCC_INDEPENDENT_128B_SHIFT 17
#define AMD_FMT_MOD_DCC_INDEPENDENT_128B_MASK 0x1
#define AMD_FMT_MOD_DCC_MAX_COMPRESSED_BLOCK_SHIFT 18
#define AMD_FMT_MOD_DCC_MAX_COMPRESSED_BLOCK_MASK 0x3
#define AMD_FMT_MOD_DCC_CONSTANT_ENCODE_SHIFT 20
#define AMD_FMT_MOD_PIPE_XOR_BITS_SHIFT 21
#define AMD_FMT_MOD_BANK_XOR_BITS_SHIFT 24
#define AMD_FMT_MOD_PACKERS_SHIFT 27 /* aliases with BANK_XOR_BITS */
#define AMD_FMT_MOD_RB_SHIFT 30
#define AMD_FMT_MOD_RB_MASK 0x7
#define AMD_FMT_MOD_PIPE_SHIFT 33
#define AMD_FMT_MOD_SET(field, value) \
	((uint64_t)(value) << AMD_FMT_MOD_##field##_SHIFT)
#define AMD_FMT_MOD_GET(field, value) \
	(((value) >> AMD_FMT_MOD_##field##_SHIFT) & AMD_FMT_MOD_##field##_MASK)
#else
#include "drm-uapi/drm_fourcc.h"
#endif

#endif /* AC_DRM_FOURCC_H */
