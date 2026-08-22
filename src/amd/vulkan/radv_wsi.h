/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 *
 * based in part on anv driver which is:
 * Copyright © 2015 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef RADV_WSI_H
#define RADV_WSI_H

#include "radv_physical_device.h"

/* ⚠ THE PLATFORM BELONGS IN THIS LIST, and its absence is an upstream gap rather than a special case for us.
 *
 * This macro gates KHR_surface, KHR_get_surface_capabilities2 and KHR_swapchain. EXT_headless_surface, mean-
 * while, is advertised whenever the build is not Windows - so a headless-only build offers a surface you can
 * create and then no swapchain to create FOR it, and vkCreateInstance fails the moment an application asks for
 * VK_KHR_surface. That combination was clearly not meant to be reachable; here it is the only one there is.
 *
 * Measured, and twice: a title's Vulkan init threw at construction with vkCreateInstance refusing
 * VK_KHR_surface, while the driver's own log stayed completely empty - because nothing had got far enough to
 * write a line.
 */
#if defined(VK_USE_PLATFORM_WAYLAND_KHR) || defined(VK_USE_PLATFORM_XCB_KHR) || defined(VK_USE_PLATFORM_XLIB_KHR) ||   \
   defined(VK_USE_PLATFORM_DISPLAY_KHR) || defined(HAVE_ORBIS_PLATFORM)
#define RADV_USE_WSI_PLATFORM
#endif

VkResult radv_init_wsi(struct radv_physical_device *pdev);

void radv_finish_wsi(struct radv_physical_device *pdev);

#endif /* RADV_WSI_H */
