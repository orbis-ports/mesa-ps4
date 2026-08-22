/*
 * Copyright 2012 Advanced Micro Devices, Inc.
 * Copyright 2024 Valve Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "ac_cmdbuf.h"
#include "ac_cmdbuf_cp.h"
#include "ac_gpu_info.h"
#include "ac_shader_util.h"

#include "amd_family.h"
#include "sid.h"

#include "util/log.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

void
ac_emit_cp_indirect_buffer(struct ac_cmdbuf *cs, uint64_t va, uint32_t cdw,
                           enum ac_cp_indirect_buffer_flags flags,
                           bool predicate)
{
   uint32_t dword2_flags = 0;

   if (flags & AC_CP_INDIRECT_BUFFER_CHAIN)
      dword2_flags |= S_3F3_CHAIN(1);
   if (flags & AC_CP_INDIRECT_BUFFER_VALID)
      dword2_flags |= S_3F3_VALID(1);

   ac_cmdbuf_begin(cs);
   ac_cmdbuf_emit(PKT3(PKT3_INDIRECT_BUFFER, 2, predicate));
   ac_cmdbuf_emit(va);
   ac_cmdbuf_emit(va >> 32);
   ac_cmdbuf_emit(cdw | dword2_flags);
   ac_cmdbuf_end();
}

void
ac_emit_cp_cond_exec(struct ac_cmdbuf *cs, enum amd_gfx_level gfx_level,
                     uint64_t va, uint32_t count)
{
   /* EXEC_COUNT is bits 13:0 - AMD's PM4 tables in src/amd/packets say so for the PFP and the MEC
    * form alike. A count past 16383 does not skip further, it skips count & 0x3fff dwords and leaves
    * the CP resuming inside a packet, so this is a wedge rather than a wrong result. Caught here
    * because the caller that overflowed it computed the number correctly and only the field was too
    * small to hold it. */
   assert(count <= 0x3FFF);

   ac_cmdbuf_begin(cs);
   if (gfx_level >= GFX7) {
      ac_cmdbuf_emit(PKT3(PKT3_COND_EXEC, 3, 0));
      ac_cmdbuf_emit(va);
      ac_cmdbuf_emit(va >> 32);
      ac_cmdbuf_emit(0);
      ac_cmdbuf_emit(count);
   } else {
      ac_cmdbuf_emit(PKT3(PKT3_COND_EXEC, 2, 0));
      ac_cmdbuf_emit(va);
      ac_cmdbuf_emit(va >> 32);
      ac_cmdbuf_emit(count);
   }
   ac_cmdbuf_end();
}

void
ac_emit_cp_write_data_head(struct ac_cmdbuf *cs, uint32_t engine_sel,
                           uint32_t dst_sel, uint64_t va, uint32_t size,
                           bool predicate)
{
   ac_cmdbuf_begin(cs);
   ac_cmdbuf_emit(PKT3(PKT3_WRITE_DATA, 2 + size, predicate));
   ac_cmdbuf_emit(S_371_DST_SEL(dst_sel) |
                  S_371_WR_CONFIRM(1) |
                  S_371_ENGINE_SEL(engine_sel));
   ac_cmdbuf_emit(va);
   ac_cmdbuf_emit(va >> 32);
   ac_cmdbuf_end();
}

void
ac_emit_cp_write_data(struct ac_cmdbuf *cs, uint32_t engine_sel,
                      uint32_t dst_sel, uint64_t va, uint32_t size,
                      const uint32_t *data, bool predicate)
{
   ac_emit_cp_write_data_head(cs, engine_sel, dst_sel, va, size, predicate);
   ac_cmdbuf_begin(cs);
   ac_cmdbuf_emit_array(data, size);
   ac_cmdbuf_end();
}

void
ac_emit_cp_write_data_imm(struct ac_cmdbuf *cs, unsigned engine_sel,
                          uint64_t va, uint32_t value)
{
   ac_emit_cp_write_data(cs, engine_sel, V_371_MEMORY, va, 1, &value, false);
}

void
ac_emit_cp_wait_mem(struct ac_cmdbuf *cs, uint64_t va, uint32_t ref,
                    uint32_t mask, unsigned flags)
{
   ac_cmdbuf_begin(cs);
   ac_cmdbuf_emit(PKT3(PKT3_WAIT_REG_MEM, 5, 0));
   ac_cmdbuf_emit(WAIT_REG_MEM_MEM_SPACE(1) | flags);
   ac_cmdbuf_emit(va);
   ac_cmdbuf_emit(va >> 32);
   ac_cmdbuf_emit(ref);  /* reference value */
   ac_cmdbuf_emit(mask); /* mask */
   ac_cmdbuf_emit(4);    /* poll interval */
   ac_cmdbuf_end();
}

static bool
is_ts_event(unsigned event_type)
{
   return event_type == V_028A90_CACHE_FLUSH_TS ||
          event_type == V_028A90_CACHE_FLUSH_AND_INV_TS_EVENT ||
          event_type == V_028A90_BOTTOM_OF_PIPE_TS ||
          event_type == V_028A90_FLUSH_AND_INV_DB_DATA_TS ||
          event_type == V_028A90_FLUSH_AND_INV_CB_DATA_TS;
}

/* This will wait or insert into the pipeline a wait for a previous
 * RELEASE_MEM PWS event.
 *
 * "event_type" must be the same as the RELEASE_MEM PWS event.
 *
 * "stage_sel" determines when the waiting happens. It can be CP_PFP, CP_ME,
 * PRE_SHADER, PRE_DEPTH, or PRE_PIX_SHADER, allowing to wait later in the
 * pipeline instead of completely idling the hw at the frontend.
 *
 * "gcr_cntl" must be 0 if not waiting in PFP or ME. When waiting later in the
 * pipeline, any cache flushes must be part of RELEASE_MEM, not ACQUIRE_MEM.
 *
 * "distance" determines how many RELEASE_MEM PWS events ago it should wait
 * for, minus one (starting from 0). There are 3 event types: PS_DONE,
 * CS_DONE, and TS events. The distance counter increments separately for each
 * type, so 0 with PS_DONE means wait for the last PS_DONE event, while 0 with
 * *_TS means wait for the last TS event (even if it's a different TS event
 * because all TS events share the same counter).
 *
 * PRE_SHADER waits before the first shader that has IMAGE_OP=1, while
 * PRE_PIX_SHADER waits before PS if it has IMAGE_OP=1 (IMAGE_OP should really
 * be called SYNC_ENABLE) PRE_DEPTH waits before depth/stencil tests.
 *
 * PRE_COLOR also exists but shouldn't be used because it can hang. It's
 * recommended to use PRE_PIX_SHADER instead, which means all PS that have
 * color exports with enabled color buffers, non-zero colormask, and non-zero
 * sample mask must have IMAGE_OP=1 to enable the sync before PS.
 *
 * Waiting for a PWS fence that was generated by a previous IB is valid, but
 * if there is an IB from another process in between and that IB also inserted
 * a PWS fence, the hw will wait for the newer fence instead because the PWS
 * counter was incremented.
 */
void
ac_emit_cp_acquire_mem_pws(struct ac_cmdbuf *cs, ASSERTED enum amd_gfx_level gfx_level,
                           ASSERTED enum amd_ip_type ip_type, uint32_t event_type,
                           uint32_t stage_sel, uint32_t count,
                           uint32_t gcr_cntl)
{
   assert(gfx_level >= GFX11 && ip_type == AMD_IP_GFX);

   const bool ts = is_ts_event(event_type);
   const bool ps_done = event_type == V_028A90_PS_DONE;
   const bool cs_done = event_type == V_028A90_CS_DONE;
   const uint32_t counter_sel = ts ? V_581B_TS_SELECT : ps_done ? V_581B_PS_SELECT : V_581B_CS_SELECT;

   assert((int)ts + (int)cs_done + (int)ps_done == 1);
   assert(!gcr_cntl || stage_sel == V_581B_CP_PFP || stage_sel == V_581B_CP_ME);
   assert(stage_sel != V_581B_PRE_COLOR);

   ac_cmdbuf_begin(cs);
   ac_cmdbuf_emit(PKT3(PKT3_ACQUIRE_MEM, 6, 0));
   ac_cmdbuf_emit(S_581B_PWS_STAGE_SEL(stage_sel) |
                  S_581B_PWS_COUNTER_SEL(counter_sel) |
                  S_581B_PWS_ENA2(1) |
                  S_581B_PWS_COUNT(count));
   ac_cmdbuf_emit(0xffffffff); /* GCR_SIZE */
   ac_cmdbuf_emit(0x01ffffff); /* GCR_SIZE_HI */
   ac_cmdbuf_emit(0);          /* GCR_BASE_LO */
   ac_cmdbuf_emit(0);          /* GCR_BASE_HI */
   ac_cmdbuf_emit(S_586B_PWS_ENA(1));
   ac_cmdbuf_emit(gcr_cntl); /* GCR_CNTL (this has no effect if PWS_STAGE_SEL isn't PFP or ME) */
   ac_cmdbuf_end();
}

/* Insert CS_DONE, PS_DONE, or a *_TS event into the pipeline, which will
 * signal after the work indicated by the event is complete, which optionally
 * includes flushing caches using "gcr_cntl" after the completion of the work.
 * *_TS events are always signaled at the end of the pipeline, while CS_DONE
 * and PS_DONE are signaled when those shaders finish. This call only inserts
 * the event into the pipeline. It doesn't wait for anything and it doesn't
 * execute anything immediately. The only way to wait for the event completion
 * is to call si_cp_acquire_mem_pws with the same "event_type".
 */
void
ac_emit_cp_release_mem_pws(struct ac_cmdbuf *cs, ASSERTED enum amd_gfx_level gfx_level,
                           ASSERTED enum amd_ip_type ip_type, uint32_t event_type,
                           uint32_t gcr_cntl)
{
   assert(gfx_level >= GFX11 && ip_type == AMD_IP_GFX);
   /* Only GFX12+ supports GCR ops with PS_DONE & CS_DONE in RELEASE_MEM. */
   assert(gfx_level >= GFX12 || !gcr_cntl || (event_type != V_028A90_PS_DONE &&
                                              event_type != V_028A90_CS_DONE));

   /* Extract GCR_CNTL fields because the encoding is different in RELEASE_MEM. */
   assert(G_587_GLI_INV(gcr_cntl) == 0);
   assert(gfx_level >= GFX12 || G_587_GL1_RANGE(gcr_cntl) == 0);
   const uint32_t glm_wb = G_587_GLM_WB(gcr_cntl);
   const uint32_t glm_inv = G_587_GLM_INV(gcr_cntl);
   const uint32_t glk_wb = G_587_GLK_WB(gcr_cntl);
   const uint32_t glk_inv = G_587_GLK_INV(gcr_cntl);
   const uint32_t glv_inv = G_587_GLV_INV(gcr_cntl);
   const uint32_t gl1_inv = G_587_GL1_INV(gcr_cntl);
   assert(G_587_GL2_US(gcr_cntl) == 0);
   assert(G_587_GL2_RANGE(gcr_cntl) == 0);
   assert(G_587_GL2_DISCARD(gcr_cntl) == 0);
   const uint32_t gl2_inv = G_587_GL2_INV(gcr_cntl);
   const uint32_t gl2_wb = G_587_GL2_WB(gcr_cntl);
   const uint32_t gcr_seq = G_587_SEQ(gcr_cntl);
   const bool ts = is_ts_event(event_type);

   ac_cmdbuf_begin(cs);
   ac_cmdbuf_emit(PKT3(PKT3_RELEASE_MEM, 6, 0));
   ac_cmdbuf_emit(S_491_EVENT_TYPE(event_type) |
                   S_491_EVENT_INDEX(ts ? 5 : 6) |
                   (gfx_level >= GFX12 ? 0 : S_491_GLM_WB(glm_wb) | S_491_GLM_INV(glm_inv) | S_491_GL1_INV(gl1_inv)) |
                   S_491_GLV_INV(glv_inv) |
                   S_491_GL2_INV(gl2_inv) |
                   S_491_GL2_WB(gl2_wb) |
                   S_491_SEQ(gcr_seq) |
                   S_491_GLK_WB(glk_wb) |
                   S_491_GLK_INV(glk_inv) |
                   S_491_PWS_ENABLE(1));
   ac_cmdbuf_emit(0); /* DST_SEL, INT_SEL, DATA_SEL */
   ac_cmdbuf_emit(0); /* ADDRESS_LO */
   ac_cmdbuf_emit(0); /* ADDRESS_HI */
   ac_cmdbuf_emit(0); /* DATA_LO */
   ac_cmdbuf_emit(0); /* DATA_HI */
   ac_cmdbuf_emit(0); /* INT_CTXID */
   ac_cmdbuf_end();
}

void
ac_emit_cp_copy_data(struct ac_cmdbuf *cs, uint32_t src_sel, uint32_t dst_sel,
                     uint64_t src_va, uint64_t dst_va,
                     enum ac_cp_copy_data_flags flags, bool predicate)
{
   uint32_t dword0 = COPY_DATA_SRC_SEL(src_sel) |
                     COPY_DATA_DST_SEL(dst_sel);

   if (flags & AC_CP_COPY_DATA_WR_CONFIRM)
      dword0 |= COPY_DATA_WR_CONFIRM;
   if (flags & AC_CP_COPY_DATA_COUNT_SEL)
      dword0 |= COPY_DATA_COUNT_SEL;
   if (flags & AC_CP_COPY_DATA_ENGINE_PFP) {
      /* COPY_DATA shouldn't set registers in PFP because that would execute
       * out-of-order with SET register packets that are executed by ME.
       */
      assert(src_sel != COPY_DATA_REG && dst_sel != COPY_DATA_REG);
      dword0 |= COPY_DATA_ENGINE_PFP;
   }

   ac_cmdbuf_begin(cs);
   ac_cmdbuf_emit(PKT3(PKT3_COPY_DATA, 4, predicate));
   ac_cmdbuf_emit(dword0);
   ac_cmdbuf_emit(src_va);
   ac_cmdbuf_emit(src_va >> 32);
   ac_cmdbuf_emit(dst_va);
   ac_cmdbuf_emit(dst_va >> 32);
   ac_cmdbuf_end();
}

void
ac_emit_cp_pfp_sync_me(struct ac_cmdbuf *cs, bool predicate)
{
   ac_cmdbuf_begin(cs);
   ac_cmdbuf_emit(PKT3(PKT3_PFP_SYNC_ME, 0, predicate));
   ac_cmdbuf_emit(0);
   ac_cmdbuf_end();
}

void
ac_emit_cp_set_predication(struct ac_cmdbuf *cs, enum amd_gfx_level gfx_level,
                           uint64_t va, uint32_t op)
{
   ac_cmdbuf_begin(cs);
   if (gfx_level >= GFX9) {
      ac_cmdbuf_emit(PKT3(PKT3_SET_PREDICATION, 2, 0));
      ac_cmdbuf_emit(op);
      ac_cmdbuf_emit(va);
      ac_cmdbuf_emit(va >> 32);
   } else {
      ac_cmdbuf_emit(PKT3(PKT3_SET_PREDICATION, 1, 0));
      ac_cmdbuf_emit(va);
      ac_cmdbuf_emit(op | ((va >> 32) & 0xFF));
   }
   ac_cmdbuf_end();
}

void
ac_emit_cp_gfx11_ge_rings(struct ac_cmdbuf *cs, const struct radeon_info *info,
                         uint64_t attr_ring_va, bool enable_gfx12_partial_hiz_wa)
{
   assert(info->gfx_level >= GFX11);
   assert((attr_ring_va >> 32) == info->address32_hi);

   ac_cmdbuf_begin(cs);

   ac_cmdbuf_set_ucfg_reg_seq(R_031110_SPI_GS_THROTTLE_CNTL1, 4);
   ac_cmdbuf_emit(0x12355123);
   ac_cmdbuf_emit(0x1544D);
   ac_cmdbuf_emit(attr_ring_va >> 16);
   ac_cmdbuf_emit(S_03111C_MEM_SIZE((info->attribute_ring_size_per_se >> 16) - 1) |
                  S_03111C_BIG_PAGE(info->discardable_allows_big_page) |
                  S_03111C_L1_POLICY(1));

   if (info->gfx_level >= GFX12) {
      const uint64_t pos_va = attr_ring_va + info->pos_ring_offset;
      const uint64_t prim_va = attr_ring_va + info->prim_ring_offset;

      /* When one of these 4 registers is updated, all 4 must be updated. */
      ac_cmdbuf_set_ucfg_reg_seq(R_0309A0_GE_POS_RING_BASE, 4);
      ac_cmdbuf_emit(pos_va >> 16);
      ac_cmdbuf_emit(S_0309A4_MEM_SIZE(info->pos_ring_size_per_se >> 5));
      ac_cmdbuf_emit(prim_va >> 16);
      ac_cmdbuf_emit(S_0309AC_MEM_SIZE(info->prim_ring_size_per_se >> 5) |
                     S_0309AC_SCOPE(gfx12_scope_device) |
                     S_0309AC_PAF_TEMPORAL(gfx12_store_high_temporal_stay_dirty) |
                     S_0309AC_PAB_TEMPORAL(gfx12_load_last_use_discard) |
                     S_0309AC_SPEC_DATA_READ(gfx12_spec_read_auto) |
                     S_0309AC_FORCE_SE_SCOPE(1) |
                     S_0309AC_PAB_NOFILL(1));

      if (info->gfx_level == GFX12 && info->pfp_fw_version >= 2680) {
         /* Mitigate the HiZ GPU hang by increasing a timeout when
          * BOTTOM_OF_PIPE_TS is used as the workaround. This must be emitted
          * when the gfx queue is idle.
          */
         const uint32_t timeout = enable_gfx12_partial_hiz_wa ? 0xfff : 0;

         ac_cmdbuf_emit(PKT3(PKT3_UPDATE_DB_SUMMARIZER_TIMEOUT, 0, 0));
         ac_cmdbuf_emit(S_EF1_SUMM_CNTL_EVICT_TIMEOUT(timeout));
      }
   }

   ac_cmdbuf_end();
}

void
ac_emit_cp_tess_rings(struct ac_cmdbuf *cs, const struct radeon_info *info,
                      uint64_t attr_ring_va)
{
   uint64_t va = attr_ring_va + info->tess_offchip_ring_size;

   /* ⚠ THE DIAGNOSTIC BIAS IS APPLIED HERE, WHERE THE REGISTER IS ACTUALLY BUILT. Setting it anywhere else
    * would move the number this driver remembers and leave the number the hardware receives alone, which is
    * the difference between an experiment and a log line about one. See radv_emit_tess_factor_ring for why
    * the factor ring's base is being moved on purpose. */
   if (info->family == CHIP_LIVERPOOL || info->family == CHIP_GLADIUS) {
      const char *const bias_s = getenv("ORBIS_TF_BASE_BIAS");
      if (bias_s != NULL)
         va += strtoull(bias_s, NULL, 0);
   }
   uint32_t tf_ring_size = info->tess_factor_ring_size / 4;

   if (info->gfx_level >= GFX11) {
      /* TF_RING_SIZE is per SE on GFX11. */
      tf_ring_size /= info->max_se;
   } else if (info->family == CHIP_LIVERPOOL || info->family == CHIP_GLADIUS) {
      /* ⚠ AND ON THESE TWO PARTS IT APPEARS TO BE PER SE AS WELL. CANDIDATE, NOT ESTABLISHED.
       *
       * MEASURED on a PlayStation 4: a GPU write fault at VA 0x212a50000 against a tess ring at
       * 0x212800000, whose offchip region is 2359296 B and whose factor ring therefore begins at
       * 0x212a40000 and is 55296 B long, ending at 0x212a4d800. The fault is 0x10000 into the factor
       * ring - past its end, and inside where a SECOND per-engine copy of it would sit.
       *
       * The competing reading of the same address is an offchip overrun: 74 * 32768 is 0x250000, which
       * hits the same byte exactly. That one was tried first and its prediction failed - programming
       * OFFCHIP_BUFFERING per SE left the fault at the same address (see ac_gpu_info.c). This candidate
       * has not been tried, and it explains why writing the full size overruns: the VGT gives each
       * engine a region of TF_RING_SIZE and the second engine's starts where the allocation ends.
       *
       * ⚠ IT RESTS ON max_se = 2, which comes from CU topology this port's arm marks UNCITED.
       * ORBIS_TF_RING_PER_SE=0 restores the undivided size, which is the control for the run. */
      /* ⚠ AND THIS PREDICTION FAILED TOO, so it is off by default and kept only as an experiment.
       *
       * With TF_RING_SIZE carrying 6912 dwords - 27648 B, one engine's worth - and the read at Sony's base
       * masked out of the way so the write was observable at all, the writes landed at 0x212a50000,
       * 0x212a51000 and 0x212a52000: offsets 0x10000, 0x11000 and 0x12000 into a factor ring the register
       * now describes as 0x6c00 long. They are not bounded by TF_RING_SIZE, and halving it moved them
       * nowhere. The same measurement had already killed the OFFCHIP_BUFFERING reading.
       *
       * Both readings of that address are therefore dead, and what is left is the third possibility the
       * earlier effort wrote down and never got to test: the ring is simply SHORTER than the hardware uses,
       * because every term of its size comes from CU topology this port marks UNCITED. That is an
       * ALLOCATION question, and ORBIS_TESS_RING_SCALE is the knob for it. */
      const char *const on = getenv("ORBIS_TF_RING_PER_SE");
      const bool        per_se = on != NULL && strcmp(on, "0") != 0;
      static bool       said;

      if (per_se)
         tf_ring_size /= info->max_se;

      /* Stated once, because a run that carries the division and a run that does not are otherwise
       * indistinguishable in the log - and the whole point of the knob is to compare them. */
      if (!said) {
         said = true;
         mesa_logi("orbis: VGT_TF_RING_SIZE <- %u dwords (%u B) %s; factor ring 0x%" PRIx64 "..0x%" PRIx64
                   ", %u shader engine(s)",
                   tf_ring_size, tf_ring_size * 4,
                   per_se ? "PER SHADER ENGINE - divided by max_se (ORBIS_TF_RING_PER_SE, an experiment "
                            "whose prediction already failed once)"
                          : "undivided, as upstream",
                   va, va + info->tess_factor_ring_size, info->max_se);
      }
   }

   /* ⚠ C_030938_SIZE IS THE WRONG MASK TO ASSERT AGAINST, and it let a real overflow through.
    *
    * The S_/C_ pair is generated from the widest definition of the field - 17 bits, because GFX11
    * widened it - so C_030938_SIZE is 0xFFFE0000 and a value of 0x10000 passes on a part whose
    * field is 16 bits wide. Mesa's own register database has the per-generation truth:
    * bits [0,15] for GFX6 through GFX10, bits [0,16] from GFX11. A PlayStation 4 asking for 256
    * tess factor workgroups produced exactly 0x10000, and the register would have been programmed
    * with zero.
    *
    * Clamped rather than only asserted, because an assert is compiled out of a release build and
    * the hardware's own truncation is silent - a ring size of zero looks like a driver that never
    * set the register. */
   {
      const uint32_t tf_size_max = info->gfx_level >= GFX11 ? 0x1FFFF : 0xFFFF;

      if (tf_ring_size > tf_size_max) {
         static bool clamped;
         if (!clamped) {
            clamped = true;
            mesa_logw("amd: VGT_TF_RING_SIZE %u dwords does not fit the %u-bit SIZE field on this "
                      "generation - clamped to %u. The tess factor ring is sized wrong upstream of "
                      "here; the hardware would otherwise have taken the low bits.",
                      tf_ring_size, info->gfx_level >= GFX11 ? 17 : 16, tf_size_max);
         }
         tf_ring_size = tf_size_max;
      }
      assert(tf_ring_size <= tf_size_max);
   }

   ac_cmdbuf_begin(cs);

   if (info->gfx_level >= GFX7) {
      ac_cmdbuf_set_ucfg_reg_seq(R_030938_VGT_TF_RING_SIZE, 3);
      ac_cmdbuf_emit(S_030938_SIZE(tf_ring_size));
      ac_cmdbuf_emit(info->hs_offchip_param);
      ac_cmdbuf_emit(va >> 8);

      if (info->gfx_level >= GFX12) {
         ac_cmdbuf_set_ucfg_reg(R_03099C_VGT_TF_MEMORY_BASE_HI, S_03099C_BASE_HI(va >> 40));
      } else if (info->gfx_level >= GFX10) {
         ac_cmdbuf_set_ucfg_reg(R_030984_VGT_TF_MEMORY_BASE_HI, S_030984_BASE_HI(va >> 40));
      } else if (info->gfx_level == GFX9) {
         ac_cmdbuf_set_ucfg_reg(R_030944_VGT_TF_MEMORY_BASE_HI, S_030944_BASE_HI(va >> 40));
      }
   } else {
      ac_cmdbuf_set_cfg_reg(R_008988_VGT_TF_RING_SIZE, S_008988_SIZE(tf_ring_size));
      ac_cmdbuf_set_cfg_reg(R_0089B8_VGT_TF_MEMORY_BASE, va >> 8);
      ac_cmdbuf_set_cfg_reg(R_0089B0_VGT_HS_OFFCHIP_PARAM, info->hs_offchip_param);
   }

   ac_cmdbuf_end();
}

void
ac_emit_cp_gfx_scratch(struct ac_cmdbuf *cs, enum amd_gfx_level gfx_level,
                       uint64_t va, uint32_t size)
{
   ac_cmdbuf_begin(cs);

   if (gfx_level >= GFX11) {
      ac_cmdbuf_set_ctx_reg_seq(R_0286E8_SPI_TMPRING_SIZE, 3);
      ac_cmdbuf_emit(size);
      ac_cmdbuf_emit(va >> 8);
      ac_cmdbuf_emit(va >> 40);
   } else {
      ac_cmdbuf_set_ctx_reg(R_0286E8_SPI_TMPRING_SIZE, size);
   }

   ac_cmdbuf_end();
}

/* Execute plain ACQUIRE_MEM that just flushes caches. This optionally waits
 * for idle on older chips. "engine" determines whether to sync in PFP or ME.
 */
void
ac_emit_cp_acquire_mem(struct ac_cmdbuf *cs, enum amd_gfx_level gfx_level,
                       enum amd_ip_type ip_type, uint32_t engine,
                       uint32_t gcr_cntl)
{
   assert(ip_type != AMD_IP_GFX ||
          (engine == V_581A_PREFETCH_PARSER || engine == V_581A_MICRO_ENGINE));
   assert(gcr_cntl);

   ac_cmdbuf_begin(cs);

   if (gfx_level >= GFX10) {
      /* ACQUIRE_MEM in PFP is implemented as ACQUIRE_MEM in ME + PFP_SYNC_ME. */
      const uint32_t engine_flag =
         ip_type == AMD_IP_GFX ? S_581A_ENGINE_SEL(engine) : 0;
      const uint32_t coher_size_hi =
         gfx_level >= GFX11 && ip_type == AMD_IP_GFX ? 0xffffff : 0xff;

      /* Flush caches. This doesn't wait for idle. */
      ac_cmdbuf_emit(PKT3(PKT3_ACQUIRE_MEM, 6, 0));
      ac_cmdbuf_emit(engine_flag);   /* which engine to use */
      ac_cmdbuf_emit(0xffffffff);    /* CP_COHER_SIZE */
      ac_cmdbuf_emit(coher_size_hi); /* CP_COHER_SIZE_HI */
      ac_cmdbuf_emit(0);             /* CP_COHER_BASE */
      ac_cmdbuf_emit(0);             /* CP_COHER_BASE_HI */
      ac_cmdbuf_emit(0x0000000A);    /* POLL_INTERVAL */
      ac_cmdbuf_emit(gcr_cntl);      /* GCR_CNTL */
   } else {
      const bool is_mec = gfx_level >= GFX7 && ip_type == AMD_IP_COMPUTE;

      if (gfx_level == GFX9 || is_mec) {
         /* Flush caches and wait for the caches to assert idle. */
         ac_cmdbuf_emit(PKT3(PKT3_ACQUIRE_MEM, 5, 0) | PKT3_SHADER_TYPE_S(is_mec));
         ac_cmdbuf_emit(gcr_cntl);      /* CP_COHER_CNTL */
         ac_cmdbuf_emit(0xffffffff);    /* CP_COHER_SIZE */
         ac_cmdbuf_emit(0x000000ff);    /* CP_COHER_SIZE_HI */
         ac_cmdbuf_emit(0);             /* CP_COHER_BASE */
         ac_cmdbuf_emit(0);             /* CP_COHER_BASE_HI */
         ac_cmdbuf_emit(0x0000000A);    /* POLL_INTERVAL */
      } else {
         /* ACQUIRE_MEM is only required on the compute ring. */
         ac_cmdbuf_emit(PKT3(PKT3_SURFACE_SYNC, 3, 0));
         ac_cmdbuf_emit(gcr_cntl);      /* CP_COHER_CNTL */
         ac_cmdbuf_emit(0xffffffff);    /* CP_COHER_SIZE */
         ac_cmdbuf_emit(0);             /* CP_COHER_BASE */
         ac_cmdbuf_emit(0x0000000A);    /* POLL_INTERVAL */
      }
   }

   ac_cmdbuf_end();
}

void
ac_emit_cp_release_mem(struct ac_cmdbuf *cs, enum amd_gfx_level gfx_level,
                       enum amd_ip_type ip_type, uint32_t event,
                       uint32_t event_flags, uint32_t dst_sel,
                       uint32_t int_sel, uint32_t data_sel, uint64_t va,
                       uint32_t new_fence, uint64_t eop_bug_va)
{
   /* Only GFX12+ supports GCR ops with PS_DONE & CS_DONE in RELEASE_MEM. */
   assert(gfx_level >= GFX12 || !event_flags || (event != V_028A90_PS_DONE &&
                                                 event != V_028A90_CS_DONE));

   const bool is_mec = gfx_level >= GFX7 && ip_type == AMD_IP_COMPUTE;

   /* GFX7 CP DMA: any use of CP_DMA.DST_SEL=TC must be avoided when EOS packets are used.
    * Use DST_SEL=MC instead. For prefetch, use SRC_SEL=TC and DST_SEL=MC.
    * Maybe related to waCpDmaHangMcTcAckDrop in PAL.
    */
   if (gfx_level == GFX7 && (event == V_028A90_CS_DONE || event == V_028A90_PS_DONE))
      event = V_028A90_BOTTOM_OF_PIPE_TS;

   const uint32_t op = EVENT_TYPE(event) |
                       EVENT_INDEX(event == V_028A90_CS_DONE || event == V_028A90_PS_DONE ? 6 : 5) |
                       event_flags;
   const uint32_t sel = EOP_DST_SEL(dst_sel) |
                        EOP_INT_SEL(int_sel) |
                        EOP_DATA_SEL(data_sel);

   ac_cmdbuf_begin(cs);

   if (gfx_level >= GFX9 || is_mec) {
      /* A ZPASS_DONE or PIXEL_STAT_DUMP_EVENT (of the DB occlusion counters)
       * must immediately precede every timestamp event to prevent a GPU hang
       * on GFX9.
       */
      if (gfx_level == GFX9 && !is_mec && eop_bug_va) {
         ac_cmdbuf_emit(PKT3(PKT3_EVENT_WRITE, 2, 0));
         ac_cmdbuf_emit(EVENT_TYPE(V_028A90_ZPASS_DONE) | EVENT_INDEX(1));
         ac_cmdbuf_emit(eop_bug_va);
         ac_cmdbuf_emit(eop_bug_va >> 32);
      }

      ac_cmdbuf_emit(PKT3(PKT3_RELEASE_MEM, gfx_level >= GFX9 ? 6 : 5, false));
      ac_cmdbuf_emit(op);
      ac_cmdbuf_emit(sel);
      ac_cmdbuf_emit(va);        /* address lo */
      ac_cmdbuf_emit(va >> 32);  /* address hi */
      ac_cmdbuf_emit(new_fence); /* immediate data lo */
      ac_cmdbuf_emit(0);         /* immediate data hi */
      if (gfx_level >= GFX9)
         ac_cmdbuf_emit(0); /* unused */
   } else {
      /* On GFX6, EOS events are always emitted with EVENT_WRITE_EOS.
       * On GFX7+, EOS events are emitted with EVENT_WRITE_EOS on the graphics
       * queue, and with RELEASE_MEM on the compute queue.
       */
      if (event == V_028B9C_CS_DONE || event == V_028B9C_PS_DONE) {
         assert(event_flags == 0 && dst_sel == EOP_DST_SEL_MEM && data_sel == EOP_DATA_SEL_VALUE_32BIT);

         ac_cmdbuf_emit(PKT3(PKT3_EVENT_WRITE_EOS, 3, false));
         ac_cmdbuf_emit(op);
         ac_cmdbuf_emit(va);
         ac_cmdbuf_emit(((va >> 32) & 0xffff) |
                        EOS_DATA_SEL(EOS_DATA_SEL_VALUE_32BIT));
         ac_cmdbuf_emit(new_fence);
      } else {
         if (gfx_level == GFX7 || gfx_level == GFX8) {
            /* Two EOP events are required to make all engines go idle (and
             * optional cache flushes executed) before the timestamp is
             * written.
             */
            ac_cmdbuf_emit(PKT3(PKT3_EVENT_WRITE_EOP, 4, false));
            ac_cmdbuf_emit(op);
            ac_cmdbuf_emit(eop_bug_va);
            ac_cmdbuf_emit(((eop_bug_va >> 32) & 0xffff) | sel);
            ac_cmdbuf_emit(0); /* immediate data */
            ac_cmdbuf_emit(0); /* unused */
         }

         ac_cmdbuf_emit(PKT3(PKT3_EVENT_WRITE_EOP, 4, false));
         ac_cmdbuf_emit(op);
         ac_cmdbuf_emit(va);
         ac_cmdbuf_emit(((va >> 32) & 0xffff) | sel);
         ac_cmdbuf_emit(new_fence); /* immediate data */
         ac_cmdbuf_emit(0);         /* unused */
      }
   }

   ac_cmdbuf_end();
}

void
ac_emit_cp_atomic_mem(struct ac_cmdbuf *cs, uint32_t atomic_op,
                      uint32_t atomic_cmd, uint64_t va, uint64_t data,
                      uint64_t compare_data)
{
   ac_cmdbuf_begin(cs);
   ac_cmdbuf_emit(PKT3(PKT3_ATOMIC_MEM, 7, 0));
   ac_cmdbuf_emit(S_1E1_ATOMIC(atomic_op) |
                  S_1E1_COMMAND(atomic_cmd));
   ac_cmdbuf_emit(va);                    /* addr lo */
   ac_cmdbuf_emit(va >> 32);              /* addr hi */
   ac_cmdbuf_emit(data);                  /* data lo */
   ac_cmdbuf_emit(data >> 32);            /* data hi */
   ac_cmdbuf_emit(compare_data);          /* compare data lo */
   ac_cmdbuf_emit(compare_data >> 32);    /* compare data hi */
   ac_cmdbuf_emit(10);                    /* loop interval */
   ac_cmdbuf_end();
}

void
ac_emit_cp_nop(struct ac_cmdbuf *cs, uint32_t value)
{
   ac_cmdbuf_begin(cs);
   ac_cmdbuf_emit(PKT3(PKT3_NOP, 0, 0));
   ac_cmdbuf_emit(value);
   ac_cmdbuf_end();
}

void
ac_emit_cp_load_context_reg_index(struct ac_cmdbuf *cs, uint32_t reg,
                                  uint32_t reg_count, uint64_t va,
                                  bool predicate)
{
   assert(reg_count);

   ac_cmdbuf_begin(cs);
   ac_cmdbuf_emit(PKT3(PKT3_LOAD_CONTEXT_REG_INDEX, 3, predicate));
   ac_cmdbuf_emit(va);
   ac_cmdbuf_emit(va >> 32);
   ac_cmdbuf_emit((reg - SI_CONTEXT_REG_OFFSET) >> 2);
   ac_cmdbuf_emit(reg_count); /* in DWORDS */
   ac_cmdbuf_end();
}

void
ac_emit_cp_inhibit_clockgating(struct ac_cmdbuf *cs, enum amd_gfx_level gfx_level,
                               bool inhibit)
{
   if (gfx_level >= GFX11)
      return; /* not needed */

   ac_cmdbuf_begin(cs);
   if (gfx_level >= GFX10) {
      ac_cmdbuf_set_ucfg_reg(R_037390_RLC_PERFMON_CLK_CNTL,
                             S_037390_PERFMON_CLOCK_STATE(inhibit));
   } else if (gfx_level >= GFX8) {
      ac_cmdbuf_set_ucfg_reg(R_0372FC_RLC_PERFMON_CLK_CNTL,
                             S_0372FC_PERFMON_CLOCK_STATE(inhibit));
   }
   ac_cmdbuf_end();
}

void
ac_emit_cp_spi_config_cntl(struct ac_cmdbuf *cs, enum amd_gfx_level gfx_level,
                           bool enable)
{
   ac_cmdbuf_begin(cs);
   if (gfx_level >= GFX12) {
      ac_cmdbuf_set_ucfg_reg(R_031120_SPI_SQG_EVENT_CTL,
                             S_031120_ENABLE_SQG_TOP_EVENTS(enable) |
                             S_031120_ENABLE_SQG_BOP_EVENTS(enable));
   } else if (gfx_level >= GFX9) {
      uint32_t spi_config_cntl = S_031100_GPR_WRITE_PRIORITY(0x2c688) |
                                 S_031100_EXP_PRIORITY_ORDER(3) |
                                 S_031100_ENABLE_SQG_TOP_EVENTS(enable) |
                                 S_031100_ENABLE_SQG_BOP_EVENTS(enable);

      if (gfx_level >= GFX10)
         spi_config_cntl |= S_031100_PS_PKR_PRIORITY_CNTL(3);

      ac_cmdbuf_set_ucfg_reg(R_031100_SPI_CONFIG_CNTL, spi_config_cntl);
   } else {
      /* SPI_CONFIG_CNTL is a protected register on GFX6-GFX8. */
      ac_cmdbuf_set_privileged_cfg_reg(R_009100_SPI_CONFIG_CNTL,
                                       S_009100_ENABLE_SQG_TOP_EVENTS(enable) |
                                       S_009100_ENABLE_SQG_BOP_EVENTS(enable));
   }
   ac_cmdbuf_end();
}

void
ac_emit_cp_update_windowed_counters(struct ac_cmdbuf *cs, const struct radeon_info *info,
                                    enum amd_ip_type ip_type, bool enable)
{
   ac_cmdbuf_begin(cs);
   if (ip_type == AMD_IP_GFX) {
      if (enable) {
         ac_cmdbuf_event_write(V_028A90_PERFCOUNTER_START);
      } else if (!info->never_send_perfcounter_stop) {
         ac_cmdbuf_event_write(V_028A90_PERFCOUNTER_STOP);
      }
   }
   ac_cmdbuf_set_sh_reg(R_00B82C_COMPUTE_PERFCOUNT_ENABLE,
                        S_00B82C_PERFCOUNT_ENABLE(enable));
   ac_cmdbuf_end();
}
