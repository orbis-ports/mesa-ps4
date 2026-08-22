/*
 * Copyright 2026 Mikołaj Mikołajczyk
 * SPDX-License-Identifier: MIT
 *
 * The link probe: does RADV actually link into a PS4 executable?
 *
 * WHY THIS EXISTS AS A BUILD STEP AND NOT A ONE-OFF. libvulkan_radeon.a is 35 MB and has to end up inside
 * an eboot beside Tempest's musl and libc++. Whether it can was an ASSUMPTION for as long as the build
 * stopped at the archive, and two measurements of it disagreed: the shared-library link reported three
 * undefined symbols, while `nm` over the archive suggested 244 more. The archive won that argument by
 * being linked - it is self-contained - but the way to keep it true is to link it every build, not to
 * remember that it was true once.
 *
 * It references exactly one symbol. vk_icdGetInstanceProcAddr is the loader-less entry point: there is no
 * ICD loader on this console, so a title calls this directly rather than dlopen()ing a .so and asking for
 * vkGetInstanceProcAddr. Referencing it is therefore both the smallest possible probe and the real entry
 * this port will use.
 *
 * LINK IT WITH --whole-archive. That is not a trick to make the test harder; it is the target shape. A
 * driver linked into a title must have all of itself present, because nothing later will dlopen the parts
 * the linker dropped.
 */

void *vk_icdGetInstanceProcAddr(void *instance, const char *name);

int
main(void)
{
   /* Non-zero exit if the entry point resolved to nothing, so the probe is meaningful if it is ever
    * actually run on hardware rather than only linked. */
   return vk_icdGetInstanceProcAddr(0, "vkCreateInstance") ? 0 : 1;
}
