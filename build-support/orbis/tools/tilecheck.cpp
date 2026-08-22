/*
 * Copyright 2026 Mikołaj Mikołajczyk
 * SPDX-License-Identifier: MIT
 *
 * TWO INDEPENDENT IMPLEMENTATIONS OF CIK TILING, CROSS-CHECKED ON THE LAPTOP.
 *
 * The Tempest fork's gnmtiler.cpp computes texel addresses for gfx7 and PASSES ON THIS CONSOLE - rung 2 of
 * its tiling test renders correctly, which makes it a hardware-blessed reference rather than a second
 * opinion. AMD's addrlib computes the same addresses and is what RADV will use for every image it creates.
 *
 * Nobody has ever compared them. That is the gap this closes, and it matters more than it sounds:
 *
 *   1. THE TILE TABLES ARE GENERATED AND UNVERIFIED. tools/gen-tile-tables.py emits 48 register words from
 *      two oracles that agree on every bit position - which proves the transcription, not the VALUES. Those
 *      words are what addrlib decodes to place every 2D-tiled texel. If one is wrong, RADV renders subtly
 *      wrongly: the exact defect class this project has spent the most flashes on, and the one a smoke test
 *      cannot see.
 *   2. THE TWO DISAGREE STRUCTURALLY OR NOT AT ALL. Tiling is a bijection onto the slice; an off-by-one in a
 *      bank or pipe term moves a whole family of texels, so a mismatch shows up in the first few hundred
 *      comparisons or never.
 *
 * WHAT IS COMPARED, and geometry is checked separately from addressing because they can disagree
 * independently - a surface can be laid out at the wrong pitch and then addressed self-consistently inside
 * it, which looks correct until something else reads the same memory:
 *
 *   geometry     the fork's aligned pitch/height against addrlib's
 *   addressing   the byte offset of each texel, on geometry both agree on
 *
 * The addrlib side goes through tileIndex, which is the path the real driver takes - so this exercises the
 * generated tables rather than bypassing them with an explicit ADDR_TILEINFO.
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include "gnmtiler.h"
/* The tiler is Tempest's, so its names are in Tempest's namespace. Pulled in wholesale because this file is
 * a test of that namespace and nothing else. */
using namespace Tempest::Detail;

extern "C" {
#include "addrinterface.h"
/* amdgpu_id.h, for the engine constant. addrinterface.h does not declare it - ac_surface.c gets it from
 * Mesa's own amd_family.h, which drags in far more than a standalone tool needs. */
#define CIASICIDGFXENGINE_SOUTHERNISLAND 0x0000000A
}

/* Liverpool, as ac_orbis_drm.c reports it. Any change there has to change here, and a disagreement between
 * the two is exactly what this tool exists to catch - so the values are spelled out rather than included. */
#define LIVERPOOL_GB_ADDR_CONFIG 0x10020003u
#define LIVERPOOL_MC_ARB_RAMCFG 2u /* noOfBanks = 2 -> 16 banks, MEASURED (gnm-tiling.md) */
#define LIVERPOOL_RB_MASK 0xffu    /* 8 RBs, MEASURED */
#define FAMILY_CI 120u             /* amdgpu_family.h; Bonaire/Liverpool are CI */
#define LIVERPOOL_CHIP_EXTERNAL_REV 21u

#include "orbis_tile_tables.h"

static void *ADDR_API alloc_sys_mem(const ADDR_ALLOCSYSMEM_INPUT *in)
{
   return malloc(in->sizeInBytes);
}

static ADDR_E_RETURNCODE ADDR_API free_sys_mem(const ADDR_FREESYSMEM_INPUT *in)
{
   free(in->pVirtAddr);
   return ADDR_OK;
}

static ADDR_HANDLE
make_addrlib()
{
   ADDR_CREATE_INPUT in = {};
   ADDR_CREATE_OUTPUT out = {};

   in.size = sizeof(in);
   out.size = sizeof(out);

   in.chipFamily = FAMILY_CI;
   in.chipRevision = LIVERPOOL_CHIP_EXTERNAL_REV;
   in.chipEngine = CIASICIDGFXENGINE_SOUTHERNISLAND;

   in.regValue.gbAddrConfig = LIVERPOOL_GB_ADDR_CONFIG;
   in.regValue.noOfBanks = LIVERPOOL_MC_ARB_RAMCFG & 0x3;
   in.regValue.noOfRanks = (LIVERPOOL_MC_ARB_RAMCFG & 0x4) >> 2;
   in.regValue.backendDisables = LIVERPOOL_RB_MASK;
   in.regValue.pTileConfig = (UINT_32 *)orbis_gb_tile_mode;
   in.regValue.noOfEntries = 32;
   in.regValue.pMacroTileConfig = (UINT_32 *)orbis_gb_macro_tile_mode;
   in.regValue.noOfMacroEntries = 16;

   /* useTileIndex, so the lookup goes through the generated tables - the point of the exercise. */
   in.createFlags.useTileIndex = 1;
   in.createFlags.useHtileSliceAlign = 1;

   in.callbacks.allocSysMem = alloc_sys_mem;
   in.callbacks.freeSysMem = free_sys_mem;
   in.callbacks.debugPrint = 0;

   if (AddrCreate(&in, &out) != ADDR_OK)
      return nullptr;
   return out.hLib;
}

static AddrTileMode
addr_mode(GnmTiling t)
{
   switch (t) {
   case GnmTiling::Linear: return ADDR_TM_LINEAR_ALIGNED;
   case GnmTiling::Tiled1D: return ADDR_TM_1D_TILED_THIN1;
   case GnmTiling::Tiled2D: return ADDR_TM_2D_TILED_THIN1;
   }
   abort();
}

static AddrTileType
addr_type(GnmMicroTile m)
{
   switch (m) {
   case GnmMicroTile::Display: return ADDR_DISPLAYABLE;
   case GnmMicroTile::Thin: return ADDR_NON_DISPLAYABLE;
   case GnmMicroTile::Depth: return ADDR_DEPTH_SAMPLE_ORDER;
   }
   abort();
}

static const char *
mode_name(GnmTiling t)
{
   switch (t) {
   case GnmTiling::Linear: return "linear";
   case GnmTiling::Tiled1D: return "1D";
   case GnmTiling::Tiled2D: return "2D";
   }
   return "?";
}

static const char *
micro_name(GnmMicroTile m)
{
   switch (m) {
   case GnmMicroTile::Display: return "display";
   case GnmMicroTile::Thin: return "thin";
   case GnmMicroTile::Depth: return "depth";
   }
   return "?";
}

struct Stats {
   unsigned surfaces = 0;
   unsigned geom_ok = 0;
   unsigned geom_policy = 0;
   unsigned geom_bad = 0;
   unsigned addr_checked = 0;
   unsigned addr_bad = 0;
   unsigned skipped = 0;
   /* ⚠ COVERAGE, BECAUSE "NOTHING DISAGREED" AND "NOTHING WAS COMPARED" ARE THE SAME NUMBER OTHERWISE.
    * The verdict used to be geom_bad == 0 && addr_bad == 0, which a run that compared not one address
    * satisfies perfectly - and this is the tool whose PASS was cited as the reason to trust the fork's
    * tiler. These two say how much of the sweep actually happened, and the verdict now requires it. */
   unsigned addr_surfaces = 0; /* surfaces that produced at least one address comparison */
   unsigned addr_aborted = 0;  /* surfaces abandoned part-way because addrlib refused a coordinate */
};

/* One surface. Returns false if addrlib refused it - which is information rather than failure: a
 * combination addrlib rejects is one RADV will never ask the fork's tiler for either. */
static bool
check_surface(ADDR_HANDLE lib, Stats &st, GnmTiling tiling, GnmMicroTile micro, uint32_t bpe, uint32_t w,
              uint32_t h, uint32_t slices, bool verbose)
{
   /* ⚠ A COMBINATION THE HARDWARE FORBIDS, WHICH addrlib REPORTS BY ASSERTING RATHER THAN BY RETURNING.
    *
    * egbaddrlib.cpp:1335: a tiled surface at 128 bits per element, or a thick tile mode, cannot use the
    * DISPLAYABLE micro-tile type. AddrComputeSurfaceInfo accepts it anyway, so the sweep got as far as the
    * address stage and then died on ADDR_ASSERT with no output at all - which is how this tool stood while
    * being cited as the reason to trust the fork's tiler.
    *
    * The earlier PASS came from a build with addrlib's asserts compiled out, where these calls return
    * whatever the unsupported path computes. Those values were compared and counted as agreement.
    *
    * Refused here, by name, rather than left to addrlib: RADV will never ask for this either. */
   if (micro == GnmMicroTile::Display && tiling != GnmTiling::Linear && bpe * 8 >= 128) {
      st.skipped++;
      if (verbose)
         printf("  skip  %-7s %-7s bpe %u %ux%ux%u - 128-bit displayable tiling does not exist on CIK\n",
                mode_name(tiling), micro_name(micro), bpe, w, h, slices);
      return false;
   }

   const GnmTileLevel lvl = gnmTileLevel(tiling, micro, bpe, w, h, slices);
   const uint32_t tile_index = gnmTileIndex(lvl.tiling, lvl.micro, bpe);

   /* ---- geometry, through AddrComputeSurfaceInfo */
   ADDR_COMPUTE_SURFACE_INFO_INPUT sin = {};
   ADDR_COMPUTE_SURFACE_INFO_OUTPUT sout = {};
   ADDR_TILEINFO tile_info = {};
   sin.size = sizeof(sin);
   sout.size = sizeof(sout);
   sout.pTileInfo = &tile_info;

   sin.tileMode = addr_mode(lvl.tiling);
   sin.format = ADDR_FMT_INVALID; /* bpp is given directly; format only matters for compressed paths */
   sin.bpp = bpe * 8;
   sin.numSamples = 1;
   sin.numFrags = 1;
   sin.width = w;
   sin.height = h;
   sin.numSlices = slices;
   sin.numMipLevels = 1;
   sin.tileIndex = (INT_32)tile_index;
   sin.pTileInfo = &tile_info;
   sin.tileType = addr_type(lvl.micro);
   if (lvl.micro == GnmMicroTile::Depth)
      sin.flags.depth = 1;

   if (AddrComputeSurfaceInfo(lib, &sin, &sout) != ADDR_OK) {
      st.skipped++;
      if (verbose)
         printf("  skip  %-7s %-7s bpe %u %ux%ux%u - addrlib refused the surface\n", mode_name(tiling),
                micro_name(micro), bpe, w, h, slices);
      return false;
   }

   st.surfaces++;

   /* GEOMETRY IS A POLICY QUESTION, AND THE ONLY WRONG ANSWER IS AN ILLEGAL ONE.
    *
    * addrlib is authoritative for RADV by construction - RADV asks it, allocates what it says and programs the
    * T# from it. The fork's tiler is authoritative for the GNM backend, and it renders correctly on this
    * console. So the two choosing DIFFERENT pitches is not by itself a defect: a larger pitch wastes memory
    * and a smaller one is fine as long as it still satisfies the hardware's alignment. What would be a real
    * defect is a pitch that does NOT.
    *
    * addrlib reports the requirement separately from its choice - pitchAlign and heightAlign - so that is what
    * the fork's numbers are checked against. This distinguishes "padded differently" from "padded wrongly",
    * which the first version of this tool conflated and would have reported 2665 failures for. */
   const bool geom_same = sout.pitch == lvl.pitch && sout.height == lvl.height;
   const bool pitch_legal = lvl.pitch >= w && sout.pitchAlign && (lvl.pitch % sout.pitchAlign) == 0;
   const bool height_legal = lvl.height >= h && sout.heightAlign && (lvl.height % sout.heightAlign) == 0;

   if (geom_same) {
      st.geom_ok++;
   } else if (pitch_legal && height_legal) {
      st.geom_policy++;
      if (verbose)
         printf("  pad   %-7s %-7s bpe %u %ux%ux%u: fork %ux%u, addrlib %ux%u (both legal)\n",
                mode_name(tiling), micro_name(micro), bpe, w, h, slices, lvl.pitch, lvl.height, sout.pitch,
                sout.height);
   } else {
      st.geom_bad++;
      printf("  GEOM  %-7s %-7s bpe %u %ux%ux%u: fork %ux%u ILLEGAL, addrlib %ux%u, align %ux%u\n",
             mode_name(tiling), micro_name(micro), bpe, w, h, slices, lvl.pitch, lvl.height, sout.pitch,
             sout.height, sout.pitchAlign, sout.heightAlign);
   }

   /* ---- addressing, ON ADDRLIB'S GEOMETRY IN BOTH CASES
    *
    * ⚠ THIS USED TO SKIP SURFACES WHOSE GEOMETRY DIFFERED, AND THAT WAS THE WRONG TEST. Padding policy and
    * the address function are separate questions: a surface can be padded to a different pitch and still be
    * addressed identically inside it. Skipping meant the only cases where the two implementations disagreed
    * about anything were also the only cases whose ADDRESSING was never compared - the exact opposite of what
    * an audit should do.
    *
    * So the fork's level is overridden with addrlib's pitch and height. That is legitimate rather than a
    * fudge: a GnmTileLevel is the geometry plus the mode, gnmTileOffset is a pure function of both, and on
    * the real hardware the pitch comes from the T# - whatever value the allocator chose. Asking "given this
    * pitch, where does texel (x,y) live" is the question the texture unit asks. */
   GnmTileLevel probe = lvl;
   probe.pitch = sout.pitch;
   probe.height = sout.height;

   /* Every texel of a small surface, a stride through a large one: the interesting failures are periodic in
    * the tile, so a stride that is coprime with the tile dimensions samples every phase. */
   const uint32_t total = probe.pitch * probe.height;
   const uint32_t step = total > 4096 ? 7 : 1;

   unsigned bad_here = 0;
   const unsigned checked_before = st.addr_checked;
   for (uint32_t s = 0; s < slices; s++) {
      for (uint32_t i = 0; i < total; i += step) {
         const uint32_t x = i % probe.pitch;
         const uint32_t y = i / probe.pitch;

         ADDR_COMPUTE_SURFACE_ADDRFROMCOORD_INPUT ain = {};
         ADDR_COMPUTE_SURFACE_ADDRFROMCOORD_OUTPUT aout = {};
         ain.size = sizeof(ain);
         aout.size = sizeof(aout);

         ain.x = x;
         ain.y = y;
         ain.slice = s;
         ain.sample = 0;
         ain.bpp = bpe * 8;
         ain.pitch = sout.pitch;
         ain.height = sout.height;
         ain.numSlices = slices;
         ain.numSamples = 1;
         ain.numFrags = 1;
         ain.tileMode = addr_mode(lvl.tiling);
         ain.isDepth = lvl.micro == GnmMicroTile::Depth;
         ain.tileType = addr_type(lvl.micro);
         ain.pTileInfo = &tile_info;
         ain.tileIndex = (INT_32)tile_index;

         if (AddrComputeSurfaceAddrFromCoord(lib, &ain, &aout) != ADDR_OK) {
            /* Counted apart from a refused SURFACE: this one abandons a sweep that had already started, so
             * the surface is partly compared rather than not compared, and lumping the two together hid
             * exactly how much of the run was real. */
            st.skipped++;
            st.addr_aborted++;
            if (st.addr_checked > checked_before)
               st.addr_surfaces++;
            return true;
         }

         const uint64_t mine = gnmTileOffset(probe, x, y, s);
         st.addr_checked++;
         if (mine != aout.addr) {
            st.addr_bad++;
            if (bad_here < 4)
               printf("  ADDR  %-7s %-7s bpe %u %ux%ux%u (%u,%u,%u): fork 0x%llx, addrlib 0x%llx\n",
                      mode_name(tiling), micro_name(micro), bpe, w, h, slices, x, y, s,
                      (unsigned long long)mine, (unsigned long long)aout.addr);
            if (++bad_here == 4)
               printf("  ADDR  ... further differences in this surface not printed\n");
         }
      }
   }
   if (st.addr_checked > checked_before)
      st.addr_surfaces++;
   return true;
}

int
main(int argc, char **argv)
{
   const bool verbose = argc > 1 && !strcmp(argv[1], "-v");

   ADDR_HANDLE lib = make_addrlib();
   if (!lib) {
      fprintf(stderr, "tilecheck: AddrCreate failed - the tile tables or the chip identity are wrong\n");
      return 1;
   }

   Stats st;

   /* The bytes-per-element values a real surface uses, and the extents that exercise the alignment rules:
    * powers of two, one below and one above, and sizes that are not multiples of a macro tile. */
   const uint32_t bpes[] = {1, 2, 4, 8, 16};
   const uint32_t sizes[] = {1, 7, 8, 15, 16, 17, 32, 63, 64, 65, 128, 255, 256, 257, 512};

   const GnmTiling tilings[] = {GnmTiling::Linear, GnmTiling::Tiled1D, GnmTiling::Tiled2D};
   const GnmMicroTile micros[] = {GnmMicroTile::Display, GnmMicroTile::Thin, GnmMicroTile::Depth};

   for (GnmTiling t : tilings)
      for (GnmMicroTile m : micros)
         for (uint32_t bpe : bpes)
            for (uint32_t w : sizes)
               for (uint32_t h : sizes)
                  check_surface(lib, st, t, m, bpe, w, h, 1, verbose);

   /* Arrays, because the slice term is its own arm in both implementations. */
   for (GnmTiling t : tilings)
      for (uint32_t bpe : bpes)
         for (uint32_t slices : {2u, 3u, 6u})
            check_surface(lib, st, t, GnmMicroTile::Thin, bpe, 64, 64, slices, verbose);

   AddrDestroy(lib);

   printf("\ntilecheck: %u surfaces, %u geometry identical, %u padded differently but LEGALLY, %u ILLEGAL\n",
          st.surfaces, st.geom_ok, st.geom_policy, st.geom_bad);
   printf("tilecheck: %u addresses compared over %u of %u surfaces, %u MISMATCH (%u surfaces abandoned "
          "part-way)\n",
          st.addr_checked, st.addr_surfaces, st.surfaces, st.addr_bad, st.addr_aborted);
   printf("tilecheck: %u combinations addrlib refused (not a failure - RADV cannot ask for them either)\n",
          st.skipped);

   /* ⚠ COVERAGE IS PART OF THE VERDICT. Without the last two terms this printed PASS for a run that compared
    * nothing at all - every surface refused, every counter zero, "no mismatches" trivially true. A tool
    * asked whether two implementations AGREE has said nothing until it has made them disagree somewhere or
    * confirmed a real number of cases; half the surfaces is a floor, not a target. */
   const unsigned floor_surfaces = st.surfaces / 2;
   const bool covered = st.addr_checked > 0 && st.addr_surfaces > floor_surfaces;
   const bool ok = st.geom_bad == 0 && st.addr_bad == 0 && covered;
   if (!covered)
      printf("tilecheck: NOT ENOUGH WAS COMPARED to say anything - %u of %u surfaces reached the address "
             "sweep, and a verdict needs more than %u\n",
             st.addr_surfaces, st.surfaces, floor_surfaces);
   printf("tilecheck: %s\n", ok ? "PASS - two independent CIK implementations agree" : "FAIL");
   return ok ? 0 : 1;
}
