/*
 * Copyright (c) 2016, Alliance for Open Media. All rights reserved
 *
 * This source code is subject to the terms of the BSD 2 Clause License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at www.aomedia.org/license/software. If the Alliance for Open
 * Media Patent License 1.0 was not distributed with this source code in the
 * PATENTS file, you can obtain it at www.aomedia.org/license/patent.

 */

#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>

#include "config/aom_dsp_rtcd.h"
#include "config/av1_rtcd.h"

#include "aom_dsp/aom_dsp_common.h"
#include "aom_dsp/blend.h"
#include "aom_mem/aom_mem.h"
#include "aom_ports/aom_timer.h"
#include "aom_ports/mem.h"
#include "aom_ports/system_state.h"

#include "av1/encoder/model_rd.h"
#include "av1/common/mvref_common.h"
#include "av1/common/pred_common.h"
#include "av1/common/reconinter.h"
#include "av1/common/reconintra.h"

#include "av1/encoder/encodemv.h"
#include "av1/encoder/rdopt.h"
#include "av1/encoder/reconinter_enc.h"

extern int g_pick_inter_mode_cnt;
/*!\cond */
typedef struct {
  uint8_t *data;
  int stride;
  int in_use;
} PRED_BUFFER;

typedef struct {
  PRED_BUFFER *best_pred;
  PREDICTION_MODE best_mode;
  TX_SIZE best_tx_size;
#if CONFIG_NEW_REF_SIGNALING
  MV_REFERENCE_FRAME_NRS best_ref_frame;
#else
  MV_REFERENCE_FRAME best_ref_frame;
#endif  // CONFIG_NEW_REF_SIGNALING
  uint8_t best_mode_skip_txfm;
  uint8_t best_mode_initial_skip_flag;
#if CONFIG_REMOVE_DUAL_FILTER
  InterpFilter best_pred_filter;
#else
  int_interpfilters best_pred_filter;
#endif  // CONFIG_REMOVE_DUAL_FILTER
} BEST_PICKMODE;

typedef struct {
  MV_REFERENCE_FRAME ref_frame;
  PREDICTION_MODE pred_mode;
} REF_MODE;
/*!\endcond */

static const int pos_shift_16x16[4][4] = {
  { 9, 10, 13, 14 }, { 11, 12, 15, 16 }, { 17, 18, 21, 22 }, { 19, 20, 23, 24 }
};

#if CONFIG_NEW_INTER_MODES
#define RT_INTER_MODES 6

#if CONFIG_NEW_REF_SIGNALING
static const REF_MODE ref_mode_set[RT_INTER_MODES] = {
  { 0, NEARMV }, { 0, NEWMV },  { 1, NEARMV },
  { 1, NEWMV },  { 2, NEARMV }, { 2, NEWMV }
};
#else
static const REF_MODE ref_mode_set[RT_INTER_MODES] = {
  { LAST_FRAME, NEARMV },  { LAST_FRAME, NEWMV },    { GOLDEN_FRAME, NEARMV },
  { GOLDEN_FRAME, NEWMV }, { ALTREF_FRAME, NEARMV }, { ALTREF_FRAME, NEWMV }
};
static const THR_MODES mode_idx[REF_FRAMES][4] = {
  { THR_DC, THR_V_PRED, THR_H_PRED, THR_SMOOTH },
  { THR_NEARMV, THR_NEARMV, THR_GLOBALMV, THR_NEWMV },
  { THR_NEARG, THR_NEARG, THR_GLOBALMV, THR_NEWG },
  { THR_NEARA, THR_NEARA, THR_GLOBALMV, THR_NEWA },
};
#endif  // CONFIG_NEW_REF_SIGNALING
#else
#define RT_INTER_MODES 9
#if CONFIG_NEW_REF_SIGNALING
static const REF_MODE ref_mode_set[RT_INTER_MODES] = {
  { 0, NEARESTMV }, { 0, NEARMV }, { 0, NEWMV },
  { 1, NEARESTMV }, { 1, NEARMV }, { 1, NEWMV },
  { 2, NEARESTMV }, { 2, NEARMV }, { 2, NEWMV }
};
#else
static const REF_MODE ref_mode_set[RT_INTER_MODES] = {
  { LAST_FRAME, NEARESTMV },   { LAST_FRAME, NEARMV },
  { LAST_FRAME, NEWMV },       { GOLDEN_FRAME, NEARESTMV },
  { GOLDEN_FRAME, NEARMV },    { GOLDEN_FRAME, NEWMV },
  { ALTREF_FRAME, NEARESTMV }, { ALTREF_FRAME, NEARMV },
  { ALTREF_FRAME, NEWMV }
};
static const THR_MODES mode_idx[REF_FRAMES][4] = {
  { THR_DC, THR_V_PRED, THR_H_PRED, THR_SMOOTH },
  { THR_NEARESTMV, THR_NEARMV, THR_GLOBALMV, THR_NEWMV },
  { THR_NEARESTL2, THR_NEARL2, THR_GLOBALL2, THR_NEWL2 },
  { THR_NEARESTL3, THR_NEARL3, THR_GLOBALL3, THR_NEWL3 },
  { THR_NEARESTG, THR_NEARG, THR_GLOBALMV, THR_NEWG },
};
#endif  // CONFIG_NEW_REF_SIGNALING
#endif  // CONFIG_NEW_INTER_MODES

static const PREDICTION_MODE intra_mode_list[] = { DC_PRED, V_PRED, H_PRED,
                                                   SMOOTH_PRED };
#if !CONFIG_NEW_REF_SIGNALING
static INLINE int mode_offset(const PREDICTION_MODE mode) {
#if CONFIG_NEW_INTER_MODES
  if (mode >= NEARMV) {
#else
  if (mode >= NEARESTMV) {
#endif  // CONFIG_NEW_INTER_MODES
    return INTER_OFFSET(mode);
  } else {
    switch (mode) {
      case DC_PRED: return 0;
      case V_PRED: return 1;
      case H_PRED: return 2;
      case SMOOTH_PRED: return 3;
      default: assert(0); return -1;
    }
  }
}
#endif  // !CONFIG_NEW_REF_SIGNALING

#if CONFIG_NEW_INTER_MODES
enum {
  INTER_NEW = (1 << NEWMV),
  INTER_NEAR = (1 << NEARMV),
  INTER_NEAR_NEW = (1 << NEARMV) | (1 << NEWMV),
};
#else
enum {
  //  INTER_ALL = (1 << NEARESTMV) | (1 << NEARMV) | (1 << NEWMV),
  INTER_NEAREST = (1 << NEARESTMV),
  INTER_NEAREST_NEW = (1 << NEARESTMV) | (1 << NEWMV),
  INTER_NEAREST_NEAR = (1 << NEARESTMV) | (1 << NEARMV),
  INTER_NEAR_NEW = (1 << NEARMV) | (1 << NEWMV),
};
#endif  // CONFIG_NEW_INTER_MODES

static INLINE void init_best_pickmode(BEST_PICKMODE *bp) {
#if CONFIG_NEW_INTER_MODES
  bp->best_mode = NEARMV;
#else
  bp->best_mode = NEARESTMV;
#endif  // CONFIG_NEW_INTER_MODES
#if CONFIG_NEW_REF_SIGNALING
  bp->best_ref_frame = 0;
#else
  bp->best_ref_frame = LAST_FRAME;
#endif  // CONFIG_NEW_REF_SIGNALING
  bp->best_tx_size = TX_8X8;
#if CONFIG_REMOVE_DUAL_FILTER
  bp->best_pred_filter = EIGHTTAP_REGULAR;
#else
  bp->best_pred_filter = av1_broadcast_interp_filter(EIGHTTAP_REGULAR);
#endif  // CONFIG_REMOVE_DUAL_FILTER
  bp->best_mode_skip_txfm = 0;
  bp->best_mode_initial_skip_flag = 0;
  bp->best_pred = NULL;
}

/*!\brief Runs Motion Estimation for a specific block and specific ref frame.
 *
 * \ingroup nonrd_mode_search
 * \callgraph
 * \callergraph
 * Finds the best Motion Vector by running Motion Estimation for a specific
 * block and a specific reference frame. Exits early if RDCost of Full Pel part
 * exceeds best RD Cost fund so far
 * \param[in]    cpi                      Top-level encoder structure
 * \param[in]    x                        Pointer to structure holding all the
 *                                        data for the current macroblock
 * \param[in]    bsize                    Current block size
 * \param[in]    mi_row                   Row index in 4x4 units
 * \param[in]    mi_col                   Column index in 4x4 units
 * \param[in]    tmp_mv                   Pointer to best found New MV
 * \param[in]    rate_mv                  Pointer to Rate of the best new MV
 * \param[in]    best_rd_sofar            RD Cost of the best mode found so far
 * \param[in]    use_base_mv              Flag, indicating that tmp_mv holds
 *                                        specific MV to start the search with
 *
 * \return Returns 0 if ME was terminated after Full Pel Search because too
 * high RD Cost. Otherwise returns 1. Best New MV is placed into \c tmp_mv.
 * Rate estimation for this vector is placed to \c rate_mv
 */
static int combined_motion_search(AV1_COMP *cpi, MACROBLOCK *x,
                                  BLOCK_SIZE bsize, int mi_row, int mi_col,
                                  int_mv *tmp_mv, int *rate_mv,
                                  int64_t best_rd_sofar, int use_base_mv) {
  MACROBLOCKD *xd = &x->e_mbd;
  const AV1_COMMON *cm = &cpi->common;
  const int num_planes = av1_num_planes(cm);
  MB_MODE_INFO *mi = xd->mi[0];
  struct buf_2d backup_yv12[MAX_MB_PLANE] = { { 0, 0, 0, 0, 0 } };
  int step_param = (cpi->sf.rt_sf.fullpel_search_step_param)
                       ? cpi->sf.rt_sf.fullpel_search_step_param
                       : cpi->mv_search_params.mv_step_param;
  FULLPEL_MV start_mv;
#if CONFIG_NEW_REF_SIGNALING
  const int ref_nrs = mi->ref_frame_nrs[0];
#else
  const int ref = mi->ref_frame[0];
#endif  // CONFIG_NEW_REF_SIGNALING
  const MV ref_mv = av1_get_ref_mv(x, mi->ref_mv_idx).as_mv;
  MV center_mv;
  int dis;
  int rv = 0;
  int cost_list[5];
  int search_subpel = 1;
  const YV12_BUFFER_CONFIG *scaled_ref_frame =
#if CONFIG_NEW_REF_SIGNALING
      av1_get_scaled_ref_frame(cpi, ref_nrs);
#else
      av1_get_scaled_ref_frame(cpi, ref);
#endif  // CONFIG_NEW_REF_SIGNALING

  if (scaled_ref_frame) {
    int i;
    // Swap out the reference frame for a version that's been scaled to
    // match the resolution of the current frame, allowing the existing
    // motion search code to be used without additional modifications.
    for (i = 0; i < MAX_MB_PLANE; i++) backup_yv12[i] = xd->plane[i].pre[0];
    av1_setup_pre_planes(xd, 0, scaled_ref_frame, mi_row, mi_col, NULL,
                         num_planes, &mi->chroma_ref_info);
  }

  start_mv = get_fullmv_from_mv(&ref_mv);

  if (!use_base_mv)
    center_mv = ref_mv;
  else
    center_mv = tmp_mv->as_mv;
  const search_site_config *src_search_sites =
      cpi->mv_search_params.search_site_cfg[SS_CFG_SRC];
  MvSubpelPrecision max_mv_precision = mi->max_mv_precision;
  FULLPEL_MOTION_SEARCH_PARAMS full_ms_params;
  av1_make_default_fullpel_ms_params(&full_ms_params, cpi, x, bsize, &center_mv,
                                     max_mv_precision, src_search_sites,
                                     /*fine_search_interval=*/0);

  av1_full_pixel_search(start_mv, &full_ms_params, step_param,
                        cond_cost_list(cpi, cost_list), &tmp_mv->as_fullmv,
                        NULL);

  // calculate the bit cost on motion vector
  MV mvp_full = get_mv_from_fullmv(&tmp_mv->as_fullmv);

  *rate_mv = av1_mv_bit_cost(&mvp_full, &ref_mv, max_mv_precision, &x->mv_costs,
                             MV_COST_WEIGHT);

  // TODO(kyslov) Account for Rate Mode!
  rv = !(RDCOST(x->rdmult, (*rate_mv), 0) > best_rd_sofar);

  if (rv && search_subpel) {
    SUBPEL_MOTION_SEARCH_PARAMS ms_params;
    av1_make_default_subpel_ms_params(&ms_params, cpi, x, bsize, &ref_mv,
                                      max_mv_precision, cost_list);
    MV subpel_start_mv = get_mv_from_fullmv(&tmp_mv->as_fullmv);
    cpi->mv_search_params.find_fractional_mv_step(
        xd, cm, &ms_params, subpel_start_mv, &tmp_mv->as_mv, &dis,
#if CONFIG_NEW_REF_SIGNALING
        &x->pred_sse[ref_nrs],
#else
        &x->pred_sse[ref],
#endif  // CONFIG_NEW_REF_SIGNALING
        NULL);

    *rate_mv = av1_mv_bit_cost(&tmp_mv->as_mv, &ref_mv, max_mv_precision,
                               &x->mv_costs, MV_COST_WEIGHT);
  }

  if (scaled_ref_frame) {
    int i;
    for (i = 0; i < MAX_MB_PLANE; i++) xd->plane[i].pre[0] = backup_yv12[i];
  }
  return rv;
}

/*!\brief Searches for the best New Motion Vector.
 *
 * \ingroup nonrd_mode_search
 * \callgraph
 * \callergraph
 * Finds the best Motion Vector by doing Motion Estimation. Uses reduced
 * complexity ME for non-LAST frames or calls \c combined_motion_search
 * for LAST reference frame
 * \param[in]    cpi                      Top-level encoder structure
 * \param[in]    x                        Pointer to structure holding all the
 *                                        data for the current macroblock
 * \param[in]    frame_mv                 Array that holds MVs for all modes
 *                                        and ref frames
 * \param[in]    ref_frame                Reference freme for which to find
 *                                        the best New MVs
 * \param[in]    gf_temporal_ref          Flag, indicating temporal reference
 *                                        for GOLDEN frame
 * \param[in]    bsize                    Current block size
 * \param[in]    mi_row                   Row index in 4x4 units
 * \param[in]    mi_col                   Column index in 4x4 units
 * \param[in]    rate_mv                  Pointer to Rate of the best new MV
 * \param[in]    best_rdc                 Pointer to the RD Cost for the best
 *                                        mode found so far
 *
 * \return Returns -1 if the search was not done, otherwise returns 0.
 * Best New MV is placed into \c frame_mv array, Rate estimation for this
 * vector is placed to \c rate_mv
 */
static int search_new_mv(AV1_COMP *cpi, MACROBLOCK *x,
                         int_mv frame_mv[][REF_FRAMES],
#if CONFIG_NEW_REF_SIGNALING
                         MV_REFERENCE_FRAME ref_frame,
#else
                         MV_REFERENCE_FRAME ref_frame,
#endif  // CONFIG_NEW_REF_SIGNALING
                         int gf_temporal_ref, BLOCK_SIZE bsize, int mi_row,
                         int mi_col, int *rate_mv, RD_STATS *best_rdc) {
  MACROBLOCKD *const xd = &x->e_mbd;
  MB_MODE_INFO *const mi = xd->mi[0];
  AV1_COMMON *cm = &cpi->common;
  MvSubpelPrecision max_mv_precision = mi->max_mv_precision;
#if CONFIG_NEW_REF_SIGNALING
  const MV_REFERENCE_FRAME_NRS last_frame = get_closest_pastcur_ref_index(cm);
  if (ref_frame != last_frame && cpi->oxcf.rc_cfg.mode == AOM_CBR &&
      gf_temporal_ref) {
#else
  if (ref_frame > LAST_FRAME && cpi->oxcf.rc_cfg.mode == AOM_CBR &&
      gf_temporal_ref) {
#endif  // CONFIG_NEW_REF_SIGNALING
    int tmp_sad;
    int dis;
    int cost_list[5] = { INT_MAX, INT_MAX, INT_MAX, INT_MAX, INT_MAX };

    if (bsize < BLOCK_16X16) return -1;

    tmp_sad = av1_int_pro_motion_estimation(
        cpi, x, bsize, mi_row, mi_col,
        &x->mbmi_ext->ref_mv_stack[ref_frame][0].this_mv.as_mv);

#if CONFIG_NEW_REF_SIGNALING
    if (tmp_sad > x->pred_mv_sad[last_frame]) return -1;
#else
    if (tmp_sad > x->pred_mv_sad[LAST_FRAME]) return -1;
#endif  // CONFIG_NEW_REF_SIGNALING

    frame_mv[NEWMV][ref_frame].as_int = mi->mv[0].as_int;
    int_mv best_mv = mi->mv[0];
    best_mv.as_mv.row >>= 3;
    best_mv.as_mv.col >>= 3;
    MV ref_mv = av1_get_ref_mv(x, 0).as_mv;

    *rate_mv = av1_mv_bit_cost(&frame_mv[NEWMV][ref_frame].as_mv, &ref_mv,
                               max_mv_precision, &x->mv_costs, MV_COST_WEIGHT);
    frame_mv[NEWMV][ref_frame].as_mv.row >>= 3;
    frame_mv[NEWMV][ref_frame].as_mv.col >>= 3;

    SUBPEL_MOTION_SEARCH_PARAMS ms_params;
    av1_make_default_subpel_ms_params(&ms_params, cpi, x, bsize, &ref_mv,
                                      max_mv_precision, cost_list);
    MV start_mv = get_mv_from_fullmv(&best_mv.as_fullmv);
    cpi->mv_search_params.find_fractional_mv_step(xd, cm, &ms_params, start_mv,
                                                  &best_mv.as_mv, &dis,
#if CONFIG_NEW_REF_SIGNALING
                                                  &x->pred_sse[ref_frame],
#else
                                                  &x->pred_sse[ref_frame],
#endif  // CONFIG_NEW_REF_SIGNALING
                                                  NULL);
    frame_mv[NEWMV][ref_frame].as_int = best_mv.as_int;
  } else if (!combined_motion_search(cpi, x, bsize, mi_row, mi_col,
                                     &frame_mv[NEWMV][ref_frame], rate_mv,
                                     best_rdc->rdcost, 0)) {
    return -1;
  }

  return 0;
}

#if CONFIG_NEW_REF_SIGNALING
/*!\brief Finds predicted motion vectors for a block.
 *
 * \ingroup nonrd_mode_search
 * \callgraph
 * \callergraph
 * Finds predicted motion vectors for a block from a certain reference frame.
 * First, it fills reference MV stack, then picks the test from the stack and
 * predicts the final MV for a block for each mode.
 * \param[in]    cpi                      Top-level encoder structure
 * \param[in]    x                        Pointer to structure holding all the
 *                                        data for the current macroblock
 * \param[in]    ref_frame                Reference freme for which to find
 *                                        ref MVs
 * \param[in]    frame_mv                 Predicted MVs for a block
 * \param[in]    tile_data                Pointer to struct holding adaptive
 *                                        data/contexts/models for the tile
 *                                        during encoding
 * \param[in]    yv12_mb                  Buffer to hold predicted block
 * \param[in]    bsize                    Current block size
 * \param[in]    force_skip_low_temp_var  Flag indicating possible mode search
 *                                        prune for low temporal variace  block
 *
 * \return Nothing is returned. Instead, predicted MVs are placed into
 * \c frame_mv array
 */
#else
/*!\brief Finds predicted motion vectors for a block.
 *
 * \ingroup nonrd_mode_search
 * \callgraph
 * \callergraph
 * Finds predicted motion vectors for a block from a certain reference frame.
 * First, it fills reference MV stack, then picks the test from the stack and
 * predicts the final MV for a block for each mode.
 * \param[in]    cpi                      Top-level encoder structure
 * \param[in]    x                        Pointer to structure holding all the
 *                                        data for the current macroblock
 * \param[in]    ref_frame                Reference freme for which to find
 *                                        ref MVs
 * \param[in]    frame_mv                 Predicted MVs for a block
 * \param[in]    tile_data                Pointer to struct holding adaptive
 *                                        data/contexts/models for the tile
 *                                        during encoding
 * \param[in]    yv12_mb                  Buffer to hold predicted block
 * \param[in]    bsize                    Current block size
 * \param[in]    force_skip_low_temp_var  Flag indicating possible mode search
 *                                        prune for low temporal variace  block
 *
 * \return Nothing is returned. Instead, predicted MVs are placed into
 * \c frame_mv array
 */
#endif  // CONFIG_NEW_REF_SIGNALING
static INLINE void find_predictors(
    AV1_COMP *cpi, MACROBLOCK *x,
#if CONFIG_NEW_REF_SIGNALING
    MV_REFERENCE_FRAME_NRS ref_frame,
    int_mv frame_mv[MB_MODE_COUNT][REF_FRAMES_NRS],
#else
    MV_REFERENCE_FRAME ref_frame, int_mv frame_mv[MB_MODE_COUNT][REF_FRAMES],
#endif  // CONFIG_NEW_REF_SIGNALING
    TileDataEnc *tile_data, struct buf_2d yv12_mb[8][MAX_MB_PLANE],
    BLOCK_SIZE bsize, int force_skip_low_temp_var) {
  AV1_COMMON *const cm = &cpi->common;
  MACROBLOCKD *const xd = &x->e_mbd;
  MB_MODE_INFO *const mbmi = xd->mi[0];
  MB_MODE_INFO_EXT *const mbmi_ext = x->mbmi_ext;
  const int num_planes = av1_num_planes(cm);
  (void)tile_data;

#if CONFIG_NEW_REF_SIGNALING
  const YV12_BUFFER_CONFIG *yv12 = get_ref_frame_yv12_buf_nrs(cm, ref_frame);
  x->pred_mv_sad[ref_frame] = INT_MAX;
  frame_mv[NEWMV][ref_frame].as_int = INVALID_MV;
#else
  const YV12_BUFFER_CONFIG *yv12 = get_ref_frame_yv12_buf(cm, ref_frame);
  x->pred_mv_sad[ref_frame] = INT_MAX;
  frame_mv[NEWMV][ref_frame].as_int = INVALID_MV;
#endif  // CONFIG_NEW_REF_SIGNALING
  // TODO(kyslov) this needs various further optimizations. to be continued..
  assert(yv12 != NULL);
  if (yv12 != NULL) {
#if CONFIG_NEW_REF_SIGNALING
    const struct scale_factors *const sf =
        get_ref_scale_factors_const_nrs(cm, ref_frame);
    av1_setup_pred_block(xd, yv12_mb[ref_frame], yv12, sf, sf, num_planes);
#else
    const struct scale_factors *const sf =
        get_ref_scale_factors_const(cm, ref_frame);
    av1_setup_pred_block(xd, yv12_mb[ref_frame], yv12, sf, sf, num_planes);
#endif  // CONFIG_NEW_REF_SIGNALING
#if CONFIG_NEW_REF_SIGNALING
    av1_find_mv_refs_nrs(cm, xd, mbmi, ref_frame, mbmi_ext->ref_mv_count,
                         xd->ref_mv_stack, xd->weight, NULL,
                         mbmi_ext->global_mvs_nrs, mbmi_ext->mode_context);
    av1_copy_usable_ref_mv_stack_and_weight(xd, mbmi_ext, ref_frame);
#else
    av1_find_mv_refs(cm, xd, mbmi, ref_frame, mbmi_ext->ref_mv_count,
                     xd->ref_mv_stack, xd->weight, NULL, mbmi_ext->global_mvs,
                     mbmi_ext->mode_context);
    av1_copy_usable_ref_mv_stack_and_weight(xd, mbmi_ext, ref_frame);
#endif  // CONFIG_NEW_REF_SIGNALING
        // TODO(Ravi): Populate mbmi_ext->ref_mv_stack[ref_frame][4] and
        // mbmi_ext->weight[ref_frame][4] inside av1_find_mv_refs.
#if CONFIG_NEW_INTER_MODES
    frame_mv[NEARMV][ref_frame] = av1_find_best_ref_mv_from_stack(
        mbmi_ext, ref_frame, cm->features.fr_mv_precision);
#else
    av1_find_best_ref_mvs_from_stack(
        mbmi_ext, ref_frame, &frame_mv[NEARESTMV][ref_frame],
        &frame_mv[NEARMV][ref_frame], cm->features.fr_mv_precision);
#endif  // CONFIG_NEW_INTER_MODES

    // Early exit for non-LAST frame if force_skip_low_temp_var is set.
#if CONFIG_NEW_REF_SIGNALING
    if (!av1_is_scaled(sf) && bsize >= BLOCK_8X8 &&
        !(force_skip_low_temp_var && ref_frame > 1)) {
      av1_mv_pred(cpi, x, yv12_mb[ref_frame][0].buf, yv12->y_stride, ref_frame,
                  bsize);
    }
#else
    if (!av1_is_scaled(sf) && bsize >= BLOCK_8X8 &&
        !(force_skip_low_temp_var && ref_frame != LAST_FRAME)) {
      av1_mv_pred(cpi, x, yv12_mb[ref_frame][0].buf, yv12->y_stride, ref_frame,
                  bsize);
    }
#endif  // CONFIG_NEW_REF_SIGNALING
  }
  av1_count_overlappable_neighbors(cm, xd);
  mbmi->num_proj_ref = 1;
}

static void estimate_single_ref_frame_costs(const AV1_COMMON *cm,
                                            const MACROBLOCKD *xd,
                                            const ModeCosts *mode_costs,
                                            int segment_id,
                                            unsigned int *ref_costs_single) {
#if CONFIG_NEW_REF_SIGNALING
  (void)cm;
  (void)segment_id;
  int seg_ref_active = 0;
#else
  int seg_ref_active =
      segfeature_active(&cm->seg, segment_id, SEG_LVL_REF_FRAME);
#endif  // CONFIG_NEW_REF_SIGNALING
  if (seg_ref_active) {
    memset(ref_costs_single, 0, REF_FRAMES * sizeof(*ref_costs_single));
  } else {
#if CONFIG_NEW_REF_SIGNALING
    int intra_inter_ctx = av1_get_intra_inter_context(xd);
    ref_costs_single[INTRA_FRAME_INDEX_NRS] =
        mode_costs->intra_inter_cost[intra_inter_ctx][0];
    unsigned int base_cost = mode_costs->intra_inter_cost[intra_inter_ctx][1];
    for (int i = 0; i < INTER_REFS_PER_FRAME_NRS; ++i)
      ref_costs_single[i] = base_cost;

    const int n_refs = cm->new_ref_frame_data.n_total_refs;
    for (int i = 0; i < n_refs; i++) {
      for (int j = 0; j <= AOMMIN(i, n_refs - 2); j++) {
        aom_cdf_prob ctx = av1_get_ref_pred_context_nrs(xd, j, n_refs);
        const int bit = i == j;
        ref_costs_single[i] += mode_costs->single_ref_cost[ctx][j][bit];
      }
    }
    for (int i = n_refs; i < INTER_REFS_PER_FRAME_NRS; i++)
      ref_costs_single[i] = INT_MAX;
#else
    int intra_inter_ctx = av1_get_intra_inter_context(xd);
    ref_costs_single[INTRA_FRAME] =
        mode_costs->intra_inter_cost[intra_inter_ctx][0];
    unsigned int base_cost = mode_costs->intra_inter_cost[intra_inter_ctx][1];

    for (int i = LAST_FRAME; i <= ALTREF_FRAME; ++i)
      ref_costs_single[i] = base_cost;

    const int ctx_p1 = av1_get_pred_context_single_ref_p1(xd);
    const int ctx_p2 = av1_get_pred_context_single_ref_p2(xd);
    const int ctx_p3 = av1_get_pred_context_single_ref_p3(xd);
    const int ctx_p4 = av1_get_pred_context_single_ref_p4(xd);
    const int ctx_p5 = av1_get_pred_context_single_ref_p5(xd);
    const int ctx_p6 = av1_get_pred_context_single_ref_p6(xd);

    // Determine cost of a single ref frame, where frame types are represented
    // by a tree:
    // Level 0: add cost whether this ref is a forward or backward ref
    ref_costs_single[LAST_FRAME] += mode_costs->single_ref_cost[ctx_p1][0][0];
    ref_costs_single[LAST2_FRAME] += mode_costs->single_ref_cost[ctx_p1][0][0];
    ref_costs_single[LAST3_FRAME] += mode_costs->single_ref_cost[ctx_p1][0][0];
    ref_costs_single[GOLDEN_FRAME] += mode_costs->single_ref_cost[ctx_p1][0][0];
    ref_costs_single[BWDREF_FRAME] += mode_costs->single_ref_cost[ctx_p1][0][1];
    ref_costs_single[ALTREF2_FRAME] +=
        mode_costs->single_ref_cost[ctx_p1][0][1];
    ref_costs_single[ALTREF_FRAME] += mode_costs->single_ref_cost[ctx_p1][0][1];

    // Level 1: if this ref is forward ref,
    // add cost whether it is last/last2 or last3/golden
    ref_costs_single[LAST_FRAME] += mode_costs->single_ref_cost[ctx_p3][2][0];
    ref_costs_single[LAST2_FRAME] += mode_costs->single_ref_cost[ctx_p3][2][0];
    ref_costs_single[LAST3_FRAME] += mode_costs->single_ref_cost[ctx_p3][2][1];
    ref_costs_single[GOLDEN_FRAME] += mode_costs->single_ref_cost[ctx_p3][2][1];

    // Level 1: if this ref is backward ref
    // then add cost whether this ref is altref or backward ref
    ref_costs_single[BWDREF_FRAME] += mode_costs->single_ref_cost[ctx_p2][1][0];
    ref_costs_single[ALTREF2_FRAME] +=
        mode_costs->single_ref_cost[ctx_p2][1][0];
    ref_costs_single[ALTREF_FRAME] += mode_costs->single_ref_cost[ctx_p2][1][1];

    // Level 2: further add cost whether this ref is last or last2
    ref_costs_single[LAST_FRAME] += mode_costs->single_ref_cost[ctx_p4][3][0];
    ref_costs_single[LAST2_FRAME] += mode_costs->single_ref_cost[ctx_p4][3][1];

    // Level 2: last3 or golden
    ref_costs_single[LAST3_FRAME] += mode_costs->single_ref_cost[ctx_p5][4][0];
    ref_costs_single[GOLDEN_FRAME] += mode_costs->single_ref_cost[ctx_p5][4][1];

    // Level 2: bwdref or altref2
    ref_costs_single[BWDREF_FRAME] += mode_costs->single_ref_cost[ctx_p6][5][0];
    ref_costs_single[ALTREF2_FRAME] +=
        mode_costs->single_ref_cost[ctx_p6][5][1];
#endif  // CONFIG_NEW_REF_SIGNALING
  }
}

static void estimate_comp_ref_frame_costs(
    const AV1_COMMON *cm, const MACROBLOCKD *xd, const ModeCosts *mode_costs,
    int segment_id,
#if CONFIG_NEW_REF_SIGNALING
    unsigned int (*ref_costs_comp)[REF_FRAMES_NRS]) {
#else
    unsigned int (*ref_costs_comp)[REF_FRAMES]) {
#endif  // CONFIG_NEW_REF_SIGNALING
#if CONFIG_NEW_REF_SIGNALING
  (void)segment_id;
  const int seg_ref_active = 0;
#else
  const int seg_ref_active =
      segfeature_active(&cm->seg, segment_id, SEG_LVL_REF_FRAME);
#endif  // CONFIG_NEW_REF_SIGNALING
  if (seg_ref_active) {
    for (int ref_frame = 0; ref_frame < REF_FRAMES; ++ref_frame)
      memset(ref_costs_comp[ref_frame], 0,
             REF_FRAMES * sizeof((*ref_costs_comp)[0]));
  } else {
#if CONFIG_NEW_REF_SIGNALING
    int intra_inter_ctx = av1_get_intra_inter_context(xd);
    unsigned int base_cost = mode_costs->intra_inter_cost[intra_inter_ctx][1];
    if (cm->current_frame.reference_mode != SINGLE_REFERENCE) {
      for (int i = 0; i < REF_FRAMES_NRS; i++)
        for (int j = 0; j < REF_FRAMES_NRS; j++) ref_costs_comp[i][j] = INT_MAX;

      const int n_refs = cm->new_ref_frame_data.n_total_refs;
      for (int i = 0; i < n_refs - 1; i++) {
        int prev_cost = base_cost;
        for (int j = 0; j < n_refs; j++) {
          if (j <= i) {
            // Keep track of the cost to encode the first reference
            aom_cdf_prob ctx = av1_get_ref_pred_context_nrs(xd, j, n_refs);
            const int bit = i == j;
            prev_cost += mode_costs->compound_ref_cost[ctx][0][j][bit];
          } else {
            // Assign the cost of signaling both references
            ref_costs_comp[i][j] = prev_cost;
            if (j < n_refs - 1) {
              aom_cdf_prob ctx = av1_get_ref_pred_context_nrs(xd, j, n_refs);
              ref_costs_comp[i][j] +=
                  mode_costs->compound_ref_cost[ctx][1][j - 1][1];
              // Maintain the cost of sending a 0 bit for the 2nd reference to
              // be used in the next iteration.
              prev_cost += mode_costs->compound_ref_cost[ctx][1][j - 1][0];
            }
          }
        }
      }
    } else {
      for (int ref0 = 0; ref0 < REF_FRAMES_NRS; ++ref0) {
        for (int ref1 = ref0 + 1; ref1 < REF_FRAMES_NRS; ++ref1)
          ref_costs_comp[ref0][ref1] = 512;
      }
    }
#else
    int intra_inter_ctx = av1_get_intra_inter_context(xd);
    unsigned int base_cost = mode_costs->intra_inter_cost[intra_inter_ctx][1];

    if (cm->current_frame.reference_mode != SINGLE_REFERENCE) {
      // Similar to single ref, determine cost of compound ref frames.
      // cost_compound_refs = cost_first_ref + cost_second_ref
      const int bwdref_comp_ctx_p = av1_get_pred_context_comp_bwdref_p(xd);
      const int bwdref_comp_ctx_p1 = av1_get_pred_context_comp_bwdref_p1(xd);
      const int ref_comp_ctx_p = av1_get_pred_context_comp_ref_p(xd);
      const int ref_comp_ctx_p1 = av1_get_pred_context_comp_ref_p1(xd);
      const int ref_comp_ctx_p2 = av1_get_pred_context_comp_ref_p2(xd);

      const int comp_ref_type_ctx = av1_get_comp_reference_type_context(xd);
      unsigned int ref_bicomp_costs[REF_FRAMES] = { 0 };

      ref_bicomp_costs[LAST_FRAME] = ref_bicomp_costs[LAST2_FRAME] =
          ref_bicomp_costs[LAST3_FRAME] = ref_bicomp_costs[GOLDEN_FRAME] =
              base_cost + mode_costs->comp_ref_type_cost[comp_ref_type_ctx][1];
      ref_bicomp_costs[BWDREF_FRAME] = ref_bicomp_costs[ALTREF2_FRAME] = 0;
      ref_bicomp_costs[ALTREF_FRAME] = 0;

      // cost of first ref frame
      ref_bicomp_costs[LAST_FRAME] +=
          mode_costs->comp_ref_cost[ref_comp_ctx_p][0][0];
      ref_bicomp_costs[LAST2_FRAME] +=
          mode_costs->comp_ref_cost[ref_comp_ctx_p][0][0];
      ref_bicomp_costs[LAST3_FRAME] +=
          mode_costs->comp_ref_cost[ref_comp_ctx_p][0][1];
      ref_bicomp_costs[GOLDEN_FRAME] +=
          mode_costs->comp_ref_cost[ref_comp_ctx_p][0][1];

      ref_bicomp_costs[LAST_FRAME] +=
          mode_costs->comp_ref_cost[ref_comp_ctx_p1][1][0];
      ref_bicomp_costs[LAST2_FRAME] +=
          mode_costs->comp_ref_cost[ref_comp_ctx_p1][1][1];

      ref_bicomp_costs[LAST3_FRAME] +=
          mode_costs->comp_ref_cost[ref_comp_ctx_p2][2][0];
      ref_bicomp_costs[GOLDEN_FRAME] +=
          mode_costs->comp_ref_cost[ref_comp_ctx_p2][2][1];

      // cost of second ref frame
      ref_bicomp_costs[BWDREF_FRAME] +=
          mode_costs->comp_bwdref_cost[bwdref_comp_ctx_p][0][0];
      ref_bicomp_costs[ALTREF2_FRAME] +=
          mode_costs->comp_bwdref_cost[bwdref_comp_ctx_p][0][0];
      ref_bicomp_costs[ALTREF_FRAME] +=
          mode_costs->comp_bwdref_cost[bwdref_comp_ctx_p][0][1];

      ref_bicomp_costs[BWDREF_FRAME] +=
          mode_costs->comp_bwdref_cost[bwdref_comp_ctx_p1][1][0];
      ref_bicomp_costs[ALTREF2_FRAME] +=
          mode_costs->comp_bwdref_cost[bwdref_comp_ctx_p1][1][1];

      // cost: if one ref frame is forward ref, the other ref is backward ref
      for (int ref0 = LAST_FRAME; ref0 <= GOLDEN_FRAME; ++ref0) {
        for (int ref1 = BWDREF_FRAME; ref1 <= ALTREF_FRAME; ++ref1) {
          ref_costs_comp[ref0][ref1] =
              ref_bicomp_costs[ref0] + ref_bicomp_costs[ref1];
        }
      }

      // cost: if both ref frames are the same side.
      const int uni_comp_ref_ctx_p = av1_get_pred_context_uni_comp_ref_p(xd);
      const int uni_comp_ref_ctx_p1 = av1_get_pred_context_uni_comp_ref_p1(xd);
      const int uni_comp_ref_ctx_p2 = av1_get_pred_context_uni_comp_ref_p2(xd);
      ref_costs_comp[LAST_FRAME][LAST2_FRAME] =
          base_cost + mode_costs->comp_ref_type_cost[comp_ref_type_ctx][0] +
          mode_costs->uni_comp_ref_cost[uni_comp_ref_ctx_p][0][0] +
          mode_costs->uni_comp_ref_cost[uni_comp_ref_ctx_p1][1][0];
      ref_costs_comp[LAST_FRAME][LAST3_FRAME] =
          base_cost + mode_costs->comp_ref_type_cost[comp_ref_type_ctx][0] +
          mode_costs->uni_comp_ref_cost[uni_comp_ref_ctx_p][0][0] +
          mode_costs->uni_comp_ref_cost[uni_comp_ref_ctx_p1][1][1] +
          mode_costs->uni_comp_ref_cost[uni_comp_ref_ctx_p2][2][0];
      ref_costs_comp[LAST_FRAME][GOLDEN_FRAME] =
          base_cost + mode_costs->comp_ref_type_cost[comp_ref_type_ctx][0] +
          mode_costs->uni_comp_ref_cost[uni_comp_ref_ctx_p][0][0] +
          mode_costs->uni_comp_ref_cost[uni_comp_ref_ctx_p1][1][1] +
          mode_costs->uni_comp_ref_cost[uni_comp_ref_ctx_p2][2][1];
      ref_costs_comp[BWDREF_FRAME][ALTREF_FRAME] =
          base_cost + mode_costs->comp_ref_type_cost[comp_ref_type_ctx][0] +
          mode_costs->uni_comp_ref_cost[uni_comp_ref_ctx_p][0][1];
    } else {
      for (int ref0 = LAST_FRAME; ref0 <= GOLDEN_FRAME; ++ref0) {
        for (int ref1 = BWDREF_FRAME; ref1 <= ALTREF_FRAME; ++ref1)
          ref_costs_comp[ref0][ref1] = 512;
      }
      ref_costs_comp[LAST_FRAME][LAST2_FRAME] = 512;
      ref_costs_comp[LAST_FRAME][LAST3_FRAME] = 512;
      ref_costs_comp[LAST_FRAME][GOLDEN_FRAME] = 512;
      ref_costs_comp[BWDREF_FRAME][ALTREF_FRAME] = 512;
    }
#endif  // CONFIG_NEW_REF_SIGNALING
  }
}

static TX_SIZE calculate_tx_size(const AV1_COMP *const cpi, BLOCK_SIZE bsize,
                                 MACROBLOCK *const x, unsigned int var,
                                 unsigned int sse) {
  MACROBLOCKD *const xd = &x->e_mbd;
  TX_SIZE tx_size;
  const TxfmSearchParams *txfm_params = &x->txfm_search_params;
  if (txfm_params->tx_mode_search_type == TX_MODE_SELECT) {
    if (sse > (var << 2))
      tx_size =
          AOMMIN(max_txsize_lookup[bsize],
                 tx_mode_to_biggest_tx_size[txfm_params->tx_mode_search_type]);
    else
      tx_size = TX_8X8;

    if (cpi->oxcf.q_cfg.aq_mode == CYCLIC_REFRESH_AQ &&
        cyclic_refresh_segment_id_boosted(xd->mi[0]->segment_id))
      tx_size = TX_8X8;
    else if (tx_size > TX_16X16)
      tx_size = TX_16X16;
  } else {
    tx_size =
        AOMMIN(max_txsize_lookup[bsize],
               tx_mode_to_biggest_tx_size[txfm_params->tx_mode_search_type]);
  }

  if (txfm_params->tx_mode_search_type != ONLY_4X4 && bsize > BLOCK_32X32)
    tx_size = TX_16X16;

  return AOMMIN(tx_size, TX_16X16);
}

static const uint8_t b_width_log2_lookup[BLOCK_SIZES] = { 0, 0, 1, 1, 1, 2,
                                                          2, 2, 3, 3, 3, 4,
                                                          4, 4, 5, 5 };
static const uint8_t b_height_log2_lookup[BLOCK_SIZES] = { 0, 1, 0, 1, 2, 1,
                                                           2, 3, 2, 3, 4, 3,
                                                           4, 5, 4, 5 };

static void block_variance(const uint8_t *src, int src_stride,
                           const uint8_t *ref, int ref_stride, int w, int h,
                           unsigned int *sse, int *sum, int block_size,
                           uint32_t *sse8x8, int *sum8x8, uint32_t *var8x8) {
  int i, j, k = 0;

  *sse = 0;
  *sum = 0;

  for (i = 0; i < h; i += block_size) {
    for (j = 0; j < w; j += block_size) {
      aom_get8x8var(src + src_stride * i + j, src_stride,
                    ref + ref_stride * i + j, ref_stride, &sse8x8[k],
                    &sum8x8[k]);
      *sse += sse8x8[k];
      *sum += sum8x8[k];
      var8x8[k] = sse8x8[k] - (uint32_t)(((int64_t)sum8x8[k] * sum8x8[k]) >> 6);
      k++;
    }
  }
}

static void calculate_variance(int bw, int bh, TX_SIZE tx_size,
                               unsigned int *sse_i, int *sum_i,
                               unsigned int *var_o, unsigned int *sse_o,
                               int *sum_o) {
  const BLOCK_SIZE unit_size = txsize_to_bsize[tx_size];
  const int nw = 1 << (bw - b_width_log2_lookup[unit_size]);
  const int nh = 1 << (bh - b_height_log2_lookup[unit_size]);
  int i, j, k = 0;

  for (i = 0; i < nh; i += 2) {
    for (j = 0; j < nw; j += 2) {
      sse_o[k] = sse_i[i * nw + j] + sse_i[i * nw + j + 1] +
                 sse_i[(i + 1) * nw + j] + sse_i[(i + 1) * nw + j + 1];
      sum_o[k] = sum_i[i * nw + j] + sum_i[i * nw + j + 1] +
                 sum_i[(i + 1) * nw + j] + sum_i[(i + 1) * nw + j + 1];
      var_o[k] = sse_o[k] - (uint32_t)(((int64_t)sum_o[k] * sum_o[k]) >>
                                       (b_width_log2_lookup[unit_size] +
                                        b_height_log2_lookup[unit_size] + 6));
      k++;
    }
  }
}

// Adjust the ac_thr according to speed, width, height and normalized sum
static int ac_thr_factor(const int speed, const int width, const int height,
                         const int norm_sum) {
  if (speed >= 8 && norm_sum < 5) {
    if (width <= 640 && height <= 480)
      return 4;
    else
      return 2;
  }
  return 1;
}

static void model_skip_for_sb_y_large(AV1_COMP *cpi, BLOCK_SIZE bsize,
                                      int mi_row, int mi_col, MACROBLOCK *x,
                                      MACROBLOCKD *xd, RD_STATS *rd_stats,
                                      int *early_term, int calculate_rd) {
  // Note our transform coeffs are 8 times an orthogonal transform.
  // Hence quantizer step is also 8 times. To get effective quantizer
  // we need to divide by 8 before sending to modeling function.
  unsigned int sse;
  struct macroblock_plane *const p = &x->plane[0];
  struct macroblockd_plane *const pd = &xd->plane[0];
#if CONFIG_EXTQUANT
  const uint32_t dc_quant =
      ROUND_POWER_OF_TWO(p->dequant_QTX[0], QUANT_TABLE_BITS);
  const uint32_t ac_quant =
      ROUND_POWER_OF_TWO(p->dequant_QTX[1], QUANT_TABLE_BITS);
#else
  const uint32_t dc_quant = p->dequant_QTX[0];
  const uint32_t ac_quant = p->dequant_QTX[1];
#endif
  const int64_t dc_thr = dc_quant * dc_quant >> 6;
  int64_t ac_thr = ac_quant * ac_quant >> 6;
  unsigned int var;
  int sum;

  const int bw = b_width_log2_lookup[bsize];
  const int bh = b_height_log2_lookup[bsize];
  const int num8x8 = 1 << (bw + bh - 2);
  unsigned int sse8x8[256] = { 0 };
  int sum8x8[256] = { 0 };
  unsigned int var8x8[256] = { 0 };
  TX_SIZE tx_size;
  int k;
  // Calculate variance for whole partition, and also save 8x8 blocks' variance
  // to be used in following transform skipping test.
  block_variance(p->src.buf, p->src.stride, pd->dst.buf, pd->dst.stride,
                 4 << bw, 4 << bh, &sse, &sum, 8, sse8x8, sum8x8, var8x8);
  var = sse - (unsigned int)(((int64_t)sum * sum) >> (bw + bh + 4));

  rd_stats->sse = sse;

  ac_thr *= ac_thr_factor(cpi->oxcf.speed, cpi->common.width,
                          cpi->common.height, abs(sum) >> (bw + bh));

  tx_size = calculate_tx_size(cpi, bsize, x, var, sse);
  // The code below for setting skip flag assumes tranform size of at least 8x8,
  // so force this lower limit on transform.
  if (tx_size < TX_8X8) tx_size = TX_8X8;
  xd->mi[0]->tx_size = tx_size;

  // Evaluate if the partition block is a skippable block in Y plane.
  {
    unsigned int sse16x16[64] = { 0 };
    int sum16x16[64] = { 0 };
    unsigned int var16x16[64] = { 0 };
    const int num16x16 = num8x8 >> 2;

    unsigned int sse32x32[16] = { 0 };
    int sum32x32[16] = { 0 };
    unsigned int var32x32[16] = { 0 };
    const int num32x32 = num8x8 >> 4;

    int ac_test = 1;
    int dc_test = 1;
    const int num = (tx_size == TX_8X8)
                        ? num8x8
                        : ((tx_size == TX_16X16) ? num16x16 : num32x32);
    const unsigned int *sse_tx =
        (tx_size == TX_8X8) ? sse8x8
                            : ((tx_size == TX_16X16) ? sse16x16 : sse32x32);
    const unsigned int *var_tx =
        (tx_size == TX_8X8) ? var8x8
                            : ((tx_size == TX_16X16) ? var16x16 : var32x32);

    // Calculate variance if tx_size > TX_8X8
    if (tx_size >= TX_16X16)
      calculate_variance(bw, bh, TX_8X8, sse8x8, sum8x8, var16x16, sse16x16,
                         sum16x16);
    if (tx_size == TX_32X32)
      calculate_variance(bw, bh, TX_16X16, sse16x16, sum16x16, var32x32,
                         sse32x32, sum32x32);

    // Skipping test
    *early_term = 0;
    for (k = 0; k < num; k++)
      // Check if all ac coefficients can be quantized to zero.
      if (!(var_tx[k] < ac_thr || var == 0)) {
        ac_test = 0;
        break;
      }

    for (k = 0; k < num; k++)
      // Check if dc coefficient can be quantized to zero.
      if (!(sse_tx[k] - var_tx[k] < dc_thr || sse == var)) {
        dc_test = 0;
        break;
      }

    if (ac_test && dc_test) {
      int skip_uv[2] = { 0 };
      unsigned int var_uv[2];
      unsigned int sse_uv[2];
      AV1_COMMON *const cm = &cpi->common;
      // Transform skipping test in UV planes.
      for (int i = 1; i <= 2; i++) {
        int j = i - 1;
        skip_uv[j] = 1;
        if (x->color_sensitivity[j]) {
          skip_uv[j] = 0;
          struct macroblock_plane *const puv = &x->plane[i];
          struct macroblockd_plane *const puvd = &xd->plane[i];
          const BLOCK_SIZE uv_bsize = get_plane_block_size(
              bsize, puvd->subsampling_x, puvd->subsampling_y);
          // Adjust these thresholds for UV.
          const int64_t uv_dc_thr =
              (puv->dequant_QTX[0] * puv->dequant_QTX[0]) >> 3;
          const int64_t uv_ac_thr =
              (puv->dequant_QTX[1] * puv->dequant_QTX[1]) >> 3;
          av1_enc_build_inter_predictor(cm, xd, mi_row, mi_col, NULL, bsize, i,
                                        i);
          var_uv[j] = cpi->fn_ptr[uv_bsize].vf(puv->src.buf, puv->src.stride,
                                               puvd->dst.buf, puvd->dst.stride,
                                               &sse_uv[j]);
          if ((var_uv[j] < uv_ac_thr || var_uv[j] == 0) &&
              (sse_uv[j] - var_uv[j] < uv_dc_thr || sse_uv[j] == var_uv[j]))
            skip_uv[j] = 1;
          else
            break;
        }
      }
      if (skip_uv[0] & skip_uv[1]) {
        *early_term = 1;
      }
    }
  }
  if (calculate_rd) {
    if (!*early_term) {
      const int bwide = block_size_wide[bsize];
      const int bhigh = block_size_high[bsize];

      model_rd_with_curvfit(cpi, x, bsize, AOM_PLANE_Y, sse, bwide * bhigh,
                            &rd_stats->rate, &rd_stats->dist);
    }

    if (*early_term) {
      rd_stats->rate = 0;
      rd_stats->dist = sse << 4;
    }
  }
}

static void model_rd_for_sb_y(const AV1_COMP *const cpi, BLOCK_SIZE bsize,
                              MACROBLOCK *x, MACROBLOCKD *xd,
                              RD_STATS *rd_stats, int calculate_rd) {
  // Note our transform coeffs are 8 times an orthogonal transform.
  // Hence quantizer step is also 8 times. To get effective quantizer
  // we need to divide by 8 before sending to modeling function.
#if CONFIG_NEW_REF_SIGNALING
  const int ref = xd->mi[0]->ref_frame_nrs[0];
#else
  const int ref = xd->mi[0]->ref_frame[0];
#endif  // CONFIG_NEW_REF_SIGNALING

  assert(bsize < BLOCK_SIZES_ALL);

  struct macroblock_plane *const p = &x->plane[0];
  struct macroblockd_plane *const pd = &xd->plane[0];
  unsigned int sse;
  int rate;
  int64_t dist;

  unsigned int var = cpi->fn_ptr[bsize].vf(p->src.buf, p->src.stride,
                                           pd->dst.buf, pd->dst.stride, &sse);
  xd->mi[0]->tx_size = calculate_tx_size(cpi, bsize, x, var, sse);

  if (calculate_rd) {
    const int bwide = block_size_wide[bsize];
    const int bhigh = block_size_high[bsize];
    model_rd_with_curvfit(cpi, x, bsize, AOM_PLANE_Y, sse, bwide * bhigh, &rate,
                          &dist);
  } else {
    rate = INT_MAX;  // this will be overwritten later with block_yrd
    dist = INT_MAX;
  }
  rd_stats->sse = sse;
  x->pred_sse[ref] = (unsigned int)AOMMIN(sse, UINT_MAX);

  assert(rate >= 0);

  rd_stats->skip_txfm = (rate == 0);
  rate = AOMMIN(rate, INT_MAX);
  rd_stats->rate = rate;
  rd_stats->dist = dist;
}

/*!\brief Calculates RD Cost using Hadamard transform.
 *
 * \ingroup nonrd_mode_search
 * \callgraph
 * \callergraph
 * Calculates RD Cost using Hadamard transform. For low bit depth this function
 * uses low-precision set of functions (16-bit) and 32 bit for high bit depth
 * \param[in]    cpi            Top-level encoder structure
 * \param[in]    x              Pointer to structure holding all the data for
                                the current macroblock
 * \param[in]    mi_row         Row index in 4x4 units
 * \param[in]    mi_col         Column index in 4x4 units
 * \param[in]    this_rdc       Pointer to calculated RD Cost
 * \param[in]    skippable      Pointer to a flag indicating possible tx skip
 * \param[in]    bsize          Current block size
 * \param[in]    tx_size        Transform size
 *
 * \return Nothing is returned. Instead, calculated RD cost is placed to
 * \c this_rdc. \c skippable flag is set if there is no non-zero quantized
 * coefficients for Hadamard transform
 */
static void block_yrd(AV1_COMP *cpi, MACROBLOCK *x, int mi_row, int mi_col,
                      RD_STATS *this_rdc, int *skippable, BLOCK_SIZE bsize,
                      TX_SIZE tx_size) {
  MACROBLOCKD *xd = &x->e_mbd;
  const struct macroblockd_plane *pd = &xd->plane[0];
  struct macroblock_plane *const p = &x->plane[0];
  const int num_4x4_w = mi_size_wide[bsize];
  const int num_4x4_h = mi_size_high[bsize];
  const int step = 1 << (tx_size << 1);
  const int block_step = (1 << tx_size);
  int block = 0;
  const int max_blocks_wide =
      num_4x4_w + (xd->mb_to_right_edge >= 0 ? 0 : xd->mb_to_right_edge >> 5);
  const int max_blocks_high =
      num_4x4_h + (xd->mb_to_bottom_edge >= 0 ? 0 : xd->mb_to_bottom_edge >> 5);
  int eob_cost = 0;
  const int bw = 4 * num_4x4_w;
  const int bh = 4 * num_4x4_h;

  (void)mi_row;
  (void)mi_col;
  (void)cpi;

  if (xd->cur_buf->flags & YV12_FLAG_HIGHBITDEPTH) {
    aom_highbd_subtract_block(bh, bw, p->src_diff, bw, p->src.buf,
                              p->src.stride, pd->dst.buf, pd->dst.stride,
                              x->e_mbd.bd);
  } else {
    aom_subtract_block(bh, bw, p->src_diff, bw, p->src.buf, p->src.stride,
                       pd->dst.buf, pd->dst.stride);
  }

  *skippable = 1;
  // Keep track of the row and column of the blocks we use so that we know
  // if we are in the unrestricted motion border.
  for (int r = 0; r < max_blocks_high; r += block_step) {
    for (int c = 0; c < num_4x4_w; c += block_step) {
      if (c < max_blocks_wide) {
        const SCAN_ORDER *const scan_order = &av1_default_scan_orders[tx_size];
        const int block_offset = BLOCK_OFFSET(block);
        tran_low_t *const coeff = p->coeff + block_offset;
        tran_low_t *const qcoeff = p->qcoeff + block_offset;
        tran_low_t *const dqcoeff = p->dqcoeff + block_offset;
        uint16_t *const eob = &p->eobs[block];
        const int diff_stride = bw;
        const int16_t *src_diff;
        src_diff = &p->src_diff[(r * diff_stride + c) << 2];

        switch (tx_size) {
          case TX_64X64:
            assert(0);  // Not implemented
            break;
          case TX_32X32:
            assert(0);  // Not used
            break;
          case TX_16X16:
            aom_hadamard_16x16(src_diff, diff_stride, coeff);
            av1_quantize_fp(coeff, 16 * 16, p->zbin_QTX, p->round_fp_QTX,
                            p->quant_fp_QTX, p->quant_shift_QTX, qcoeff,
                            dqcoeff, p->dequant_QTX, eob, scan_order->scan,
                            scan_order->iscan);
            break;
          case TX_8X8:
            aom_hadamard_8x8(src_diff, diff_stride, coeff);
            av1_quantize_fp(coeff, 8 * 8, p->zbin_QTX, p->round_fp_QTX,
                            p->quant_fp_QTX, p->quant_shift_QTX, qcoeff,
                            dqcoeff, p->dequant_QTX, eob, scan_order->scan,
                            scan_order->iscan);
            break;
          default:
            assert(tx_size == TX_4X4);
            aom_fdct4x4(src_diff, coeff, diff_stride);
            av1_quantize_fp(coeff, 4 * 4, p->zbin_QTX, p->round_fp_QTX,
                            p->quant_fp_QTX, p->quant_shift_QTX, qcoeff,
                            dqcoeff, p->dequant_QTX, eob, scan_order->scan,
                            scan_order->iscan);
            break;
        }
        assert(*eob <= 1024);
        *skippable &= (*eob == 0);
        eob_cost += 1;
      }
      block += step;
    }
  }
  this_rdc->skip_txfm = *skippable;
  this_rdc->rate = 0;
  if (this_rdc->sse < INT64_MAX) {
    this_rdc->sse = (this_rdc->sse << 6) >> 2;
    if (*skippable) {
      this_rdc->dist = this_rdc->sse;
      return;
    }
  }

  block = 0;
  this_rdc->dist = 0;
  for (int r = 0; r < max_blocks_high; r += block_step) {
    for (int c = 0; c < num_4x4_w; c += block_step) {
      if (c < max_blocks_wide) {
        const int block_offset = BLOCK_OFFSET(block);
        uint16_t *const eob = &p->eobs[block];
        int64_t dummy;
        tran_low_t *const coeff = p->coeff + block_offset;
        tran_low_t *const qcoeff = p->qcoeff + block_offset;
        tran_low_t *const dqcoeff = p->dqcoeff + block_offset;

        if (*eob == 1)
          this_rdc->rate += (int)abs(qcoeff[0]);
        else if (*eob > 1)
          this_rdc->rate += aom_satd(qcoeff, step << 4);

        this_rdc->dist +=
            av1_block_error(coeff, dqcoeff, step << 4, &dummy) >> 2;
      }
      block += step;
    }
  }

  // If skippable is set, rate gets clobbered later.
  this_rdc->rate <<= (2 + AV1_PROB_COST_SHIFT);
  this_rdc->rate += (eob_cost << AV1_PROB_COST_SHIFT);
}

static INLINE void init_mbmi(MB_MODE_INFO *mbmi, PREDICTION_MODE pred_mode,
#if CONFIG_NEW_REF_SIGNALING
                             MV_REFERENCE_FRAME_NRS ref_frame0,
                             MV_REFERENCE_FRAME_NRS ref_frame1,
#else
                             MV_REFERENCE_FRAME ref_frame0,
                             MV_REFERENCE_FRAME ref_frame1,
#endif  // CONFIG_NEW_REF_SIGNALING
                             const AV1_COMMON *cm, const SB_INFO *sbi) {
  PALETTE_MODE_INFO *const pmi = &mbmi->palette_mode_info;
  mbmi->ref_mv_idx = 0;
  mbmi->mode = pred_mode;
  mbmi->uv_mode = UV_DC_PRED;
#if CONFIG_NEW_REF_SIGNALING
  mbmi->ref_frame_nrs[0] = ref_frame0;
  mbmi->ref_frame_nrs[1] = ref_frame1;
#else
  mbmi->ref_frame[0] = ref_frame0;
  mbmi->ref_frame[1] = ref_frame1;
#endif  // CONFIG_NEW_REF_SIGNALING
  pmi->palette_size[0] = 0;
  pmi->palette_size[1] = 0;
  mbmi->filter_intra_mode_info.use_filter_intra = 0;
  mbmi->mv[0].as_int = mbmi->mv[1].as_int = 0;
  mbmi->motion_mode = SIMPLE_TRANSLATION;
  mbmi->num_proj_ref = 1;
  mbmi->interintra_mode = 0;
  set_default_interp_filters(mbmi, cm->features.interp_filter);
  av1_set_default_mbmi_mv_precision(mbmi, sbi);
}

#if CONFIG_INTERNAL_STATS && !CONFIG_NEW_REF_SIGNALING
static void store_coding_context(MACROBLOCK *x, PICK_MODE_CONTEXT *ctx,
                                 int mode_index) {
#else
static void store_coding_context(MACROBLOCK *x, PICK_MODE_CONTEXT *ctx) {
#endif  // CONFIG_INTERNAL_STATS && !CONFIG_NEW_REF_SIGNALING
  MACROBLOCKD *const xd = &x->e_mbd;
  TxfmSearchInfo *txfm_info = &x->txfm_search_info;

  // Take a snapshot of the coding context so it can be
  // restored if we decide to encode this way
  ctx->rd_stats.skip_txfm = txfm_info->skip_txfm;

  memset(ctx->blk_skip, 0, sizeof(ctx->blk_skip[0]) * ctx->num_4x4_blk);
  memset(ctx->tx_type_map, DCT_DCT,
         sizeof(ctx->tx_type_map[0]) * ctx->num_4x4_blk);
  ctx->skippable = txfm_info->skip_txfm;
#if CONFIG_INTERNAL_STATS && !CONFIG_NEW_REF_SIGNALING
  ctx->best_mode_index = mode_index;
#endif  // CONFIG_INTERNAL_STATS && !CONFIG_NEW_REF_SIGNALING
  ctx->mic = *xd->mi[0];
  ctx->skippable = txfm_info->skip_txfm;
  av1_copy_mbmi_ext_to_mbmi_ext_frame(
      &ctx->mbmi_ext_best, x->mbmi_ext,
#if CONFIG_NEW_REF_SIGNALING
      av1_ref_frame_type_nrs(xd->mi[0]->ref_frame_nrs)
#else
      av1_ref_frame_type(xd->mi[0]->ref_frame)
#endif  // CONFIG_NEW_REF_SIGNALING
  );
  ctx->comp_pred_diff = 0;
  ctx->hybrid_pred_diff = 0;
  ctx->single_pred_diff = 0;
}

static int get_pred_buffer(PRED_BUFFER *p, int len) {
  for (int i = 0; i < len; i++) {
    if (!p[i].in_use) {
      p[i].in_use = 1;
      return i;
    }
  }
  return -1;
}

static void free_pred_buffer(PRED_BUFFER *p) {
  if (p != NULL) p->in_use = 0;
}

static int cost_mv_ref(const ModeCosts *const mode_costs, PREDICTION_MODE mode,
                       int16_t mode_context) {
  if (is_inter_compound_mode(mode)) {
    return mode_costs
        ->inter_compound_mode_cost[mode_context][INTER_COMPOUND_OFFSET(mode)];
  }

  assert(is_inter_mode(mode));

#if CONFIG_NEW_INTER_MODES
  const int16_t ismode_ctx = inter_single_mode_ctx(mode_context);
  return mode_costs
      ->inter_single_mode_cost[ismode_ctx][mode - SINGLE_INTER_MODE_START];
#else
  int mode_cost = 0;
  int16_t mode_ctx = mode_context & NEWMV_CTX_MASK;

  if (mode == NEWMV) {
    mode_cost = mode_costs->newmv_mode_cost[mode_ctx][0];
    return mode_cost;
  } else {
    mode_cost = mode_costs->newmv_mode_cost[mode_ctx][1];
    mode_ctx = (mode_context >> GLOBALMV_OFFSET) & GLOBALMV_CTX_MASK;

    if (mode == GLOBALMV) {
      mode_cost += mode_costs->zeromv_mode_cost[mode_ctx][0];
      return mode_cost;
    } else {
      mode_cost += mode_costs->zeromv_mode_cost[mode_ctx][1];
      mode_ctx = (mode_context >> REFMV_OFFSET) & REFMV_CTX_MASK;
      mode_cost += mode_costs->refmv_mode_cost[mode_ctx][mode != NEARESTMV];
      return mode_cost;
    }
  }
#endif  // CONFIG_NEW_INTER_MODES
}

static void newmv_diff_bias(MACROBLOCKD *xd, PREDICTION_MODE this_mode,
                            RD_STATS *this_rdc, BLOCK_SIZE bsize, int mv_row,
                            int mv_col, int speed, uint32_t spatial_variance) {
  // Bias against MVs associated with NEWMV mode that are very different from
  // top/left neighbors.
  if (this_mode == NEWMV) {
    int al_mv_average_row;
    int al_mv_average_col;
    int left_row, left_col;
    int row_diff, col_diff;
    int above_mv_valid = 0;
    int left_mv_valid = 0;
    int above_row = 0;
    int above_col = 0;

    if (xd->above_mbmi) {
      above_mv_valid = xd->above_mbmi->mv[0].as_int != INVALID_MV;
      above_row = xd->above_mbmi->mv[0].as_mv.row;
      above_col = xd->above_mbmi->mv[0].as_mv.col;
    }
    if (xd->left_mbmi) {
      left_mv_valid = xd->left_mbmi->mv[0].as_int != INVALID_MV;
      left_row = xd->left_mbmi->mv[0].as_mv.row;
      left_col = xd->left_mbmi->mv[0].as_mv.col;
    }
    if (above_mv_valid && left_mv_valid) {
      al_mv_average_row = (above_row + left_row + 1) >> 1;
      al_mv_average_col = (above_col + left_col + 1) >> 1;
    } else if (above_mv_valid) {
      al_mv_average_row = above_row;
      al_mv_average_col = above_col;
    } else if (left_mv_valid) {
      al_mv_average_row = left_row;
      al_mv_average_col = left_col;
    } else {
      al_mv_average_row = al_mv_average_col = 0;
    }
    row_diff = al_mv_average_row - mv_row;
    col_diff = al_mv_average_col - mv_col;
    if (row_diff > 80 || row_diff < -80 || col_diff > 80 || col_diff < -80) {
      if (bsize >= BLOCK_32X32)
        this_rdc->rdcost = this_rdc->rdcost << 1;
      else
        this_rdc->rdcost = 5 * this_rdc->rdcost >> 2;
    }
  } else {
    // Bias for speed >= 8 for low spatial variance.
    if (speed >= 8 && spatial_variance < 150 &&
        (mv_row > 64 || mv_row < -64 || mv_col > 64 || mv_col < -64))
      this_rdc->rdcost = 5 * this_rdc->rdcost >> 2;
  }
}

static void model_rd_for_sb_uv(AV1_COMP *cpi, BLOCK_SIZE plane_bsize,
                               MACROBLOCK *x, MACROBLOCKD *xd,
                               RD_STATS *this_rdc, int64_t *sse_y,
                               int start_plane, int stop_plane) {
  // Note our transform coeffs are 8 times an orthogonal transform.
  // Hence quantizer step is also 8 times. To get effective quantizer
  // we need to divide by 8 before sending to modeling function.
  unsigned int sse;
  int rate;
  int64_t dist;
  int i;
  int64_t tot_sse = *sse_y;

  this_rdc->rate = 0;
  this_rdc->dist = 0;
  this_rdc->skip_txfm = 0;

  for (i = start_plane; i <= stop_plane; ++i) {
    struct macroblock_plane *const p = &x->plane[i];
    struct macroblockd_plane *const pd = &xd->plane[i];
#if CONFIG_EXTQUANT
    const uint32_t dc_quant =
        ROUND_POWER_OF_TWO(p->dequant_QTX[0], QUANT_TABLE_BITS);
    const uint32_t ac_quant =
        ROUND_POWER_OF_TWO(p->dequant_QTX[1], QUANT_TABLE_BITS);
#else
    const uint32_t dc_quant = p->dequant_QTX[0];
    const uint32_t ac_quant = p->dequant_QTX[1];
#endif
    const BLOCK_SIZE bs = plane_bsize;
    unsigned int var;
    if (!x->color_sensitivity[i - 1]) continue;

    var = cpi->fn_ptr[bs].vf(p->src.buf, p->src.stride, pd->dst.buf,
                             pd->dst.stride, &sse);
    assert(sse >= var);
    tot_sse += sse;

    av1_model_rd_from_var_lapndz(sse - var, num_pels_log2_lookup[bs],
                                 dc_quant >> 3, &rate, &dist);

    this_rdc->rate += rate >> 1;
    this_rdc->dist += dist << 3;

    av1_model_rd_from_var_lapndz(var, num_pels_log2_lookup[bs], ac_quant >> 3,
                                 &rate, &dist);

    this_rdc->rate += rate;
    this_rdc->dist += dist << 4;
  }

  if (this_rdc->rate == 0) {
    this_rdc->skip_txfm = 1;
  }

  if (RDCOST(x->rdmult, this_rdc->rate, this_rdc->dist) >=
      RDCOST(x->rdmult, 0, tot_sse << 4)) {
    this_rdc->rate = 0;
    this_rdc->dist = tot_sse << 4;
    this_rdc->skip_txfm = 1;
  }

  *sse_y = tot_sse;
}

/*!\cond */
struct estimate_block_intra_args {
  AV1_COMP *cpi;
  MACROBLOCK *x;
  PREDICTION_MODE mode;
  int skippable;
  RD_STATS *rdc;
};
/*!\endcond */

/*!\brief Estimation of RD cost of an intra mode for Non-RD optimized case.
 *
 * \ingroup nonrd_mode_search
 * \callgraph
 * \callergraph
 * Calculates RD Cost for an intra mode for a single TX block using Hadamard
 * transform.
 * \param[in]    plane          Color plane
 * \param[in]    block          Index of a TX block in a prediction block
 * \param[in]    row            Row of a current TX block
 * \param[in]    col            Column of a current TX block
 * \param[in]    plane_bsize    Block size of a current prediction block
 * \param[in]    tx_size        Transform size
 * \param[in]    arg            Pointer to a structure that holds paramaters
 *                              for intra mode search
 *
 * \return Nothing is returned. Instead, best mode and RD Cost of the best mode
 * are set in \c args->rdc and \c args->mode
 */
static void estimate_block_intra(int plane, int block, int row, int col,
                                 BLOCK_SIZE plane_bsize, TX_SIZE tx_size,
                                 void *arg) {
  struct estimate_block_intra_args *const args = arg;
  AV1_COMP *const cpi = args->cpi;
  AV1_COMMON *const cm = &cpi->common;
  MACROBLOCK *const x = args->x;
  MACROBLOCKD *const xd = &x->e_mbd;
  struct macroblock_plane *const p = &x->plane[plane];
  struct macroblockd_plane *const pd = &xd->plane[plane];
  const BLOCK_SIZE bsize_tx = txsize_to_bsize[tx_size];
  uint8_t *const src_buf_base = p->src.buf;
  uint8_t *const dst_buf_base = pd->dst.buf;
  const int64_t src_stride = p->src.stride;
  const int64_t dst_stride = pd->dst.stride;
  RD_STATS this_rdc;

  (void)block;

  p->src.buf = &src_buf_base[4 * (row * src_stride + col)];
  pd->dst.buf = &dst_buf_base[4 * (row * dst_stride + col)];

  av1_predict_intra_block_facade(cm, xd, plane, col, row, tx_size);
  av1_invalid_rd_stats(&this_rdc);

  if (plane == 0) {
    block_yrd(cpi, x, 0, 0, &this_rdc, &args->skippable, bsize_tx,
              AOMMIN(tx_size, TX_16X16));
  } else {
    int64_t sse = 0;
    model_rd_for_sb_uv(cpi, plane_bsize, x, xd, &this_rdc, &sse, plane, plane);
  }

  p->src.buf = src_buf_base;
  pd->dst.buf = dst_buf_base;
  args->rdc->rate += this_rdc.rate;
  args->rdc->dist += this_rdc.dist;
}

static INLINE void update_thresh_freq_fact(
    AV1_COMP *cpi, MACROBLOCK *x, BLOCK_SIZE bsize,
#if CONFIG_NEW_REF_SIGNALING
    MV_REFERENCE_FRAME_NRS ref_frame,
#else
    MV_REFERENCE_FRAME ref_frame,
#endif  // CONFIG_NEW_REF_SIGNALING
    PREDICTION_MODE mode,
#if CONFIG_NEW_REF_SIGNALING
    MV_REFERENCE_FRAME_NRS best_ref_frame,
#else
    MV_REFERENCE_FRAME best_ref_frame,
#endif  // CONFIG_NEW_REF_SIGNALING
    PREDICTION_MODE best_mode) {
#if CONFIG_NEW_REF_SIGNALING
  int *freq_fact = &x->thresh_freq_fact[bsize][mode];
  if (ref_frame == best_ref_frame && mode == best_mode) {
#else
  THR_MODES thr_mode_idx = mode_idx[ref_frame][mode_offset(mode)];
  THR_MODES best_mode_idx = mode_idx[best_ref_frame][mode_offset(best_mode)];
  int *freq_fact = &x->thresh_freq_fact[bsize][thr_mode_idx];
  if (thr_mode_idx == best_mode_idx) {
#endif  // CONFIG_NEW_REF_SIGNALING
    *freq_fact -= (*freq_fact >> 4);
  } else {
    *freq_fact =
        AOMMIN(*freq_fact + RD_THRESH_INC,
               cpi->sf.inter_sf.adaptive_rd_thresh * RD_THRESH_MAX_FACT);
  }
}

static INLINE int get_force_skip_low_temp_var_small_sb(uint8_t *variance_low,
                                                       int mi_row, int mi_col,
                                                       BLOCK_SIZE bsize) {
  // Relative indices of MB inside the superblock.
  const int mi_x = mi_row & 0xF;
  const int mi_y = mi_col & 0xF;
  // Relative indices of 16x16 block inside the superblock.
  const int i = mi_x >> 2;
  const int j = mi_y >> 2;
  int force_skip_low_temp_var = 0;
  // Set force_skip_low_temp_var based on the block size and block offset.
  switch (bsize) {
    case BLOCK_64X64: force_skip_low_temp_var = variance_low[0]; break;
    case BLOCK_64X32:
      if (!mi_y && !mi_x) {
        force_skip_low_temp_var = variance_low[1];
      } else if (!mi_y && mi_x) {
        force_skip_low_temp_var = variance_low[2];
      }
      break;
    case BLOCK_32X64:
      if (!mi_y && !mi_x) {
        force_skip_low_temp_var = variance_low[3];
      } else if (mi_y && !mi_x) {
        force_skip_low_temp_var = variance_low[4];
      }
      break;
    case BLOCK_32X32:
      if (!mi_y && !mi_x) {
        force_skip_low_temp_var = variance_low[5];
      } else if (mi_y && !mi_x) {
        force_skip_low_temp_var = variance_low[6];
      } else if (!mi_y && mi_x) {
        force_skip_low_temp_var = variance_low[7];
      } else if (mi_y && mi_x) {
        force_skip_low_temp_var = variance_low[8];
      }
      break;
    case BLOCK_32X16:
    case BLOCK_16X32:
    case BLOCK_16X16:
      force_skip_low_temp_var = variance_low[pos_shift_16x16[i][j]];
      break;
    default: break;
  }

  return force_skip_low_temp_var;
}

static INLINE int get_force_skip_low_temp_var(uint8_t *variance_low, int mi_row,
                                              int mi_col, BLOCK_SIZE bsize) {
  int force_skip_low_temp_var = 0;
  int x, y;
  x = (mi_col & 0x1F) >> 4;
  // y = (mi_row & 0x1F) >> 4;
  // const int idx64 = (y << 1) + x;
  y = (mi_row & 0x17) >> 3;
  const int idx64 = y + x;

  x = (mi_col & 0xF) >> 3;
  // y = (mi_row & 0xF) >> 3;
  // const int idx32 = (y << 1) + x;
  y = (mi_row & 0xB) >> 2;
  const int idx32 = y + x;

  x = (mi_col & 0x7) >> 2;
  // y = (mi_row & 0x7) >> 2;
  // const int idx16 = (y << 1) + x;
  y = (mi_row & 0x5) >> 1;
  const int idx16 = y + x;
  // Set force_skip_low_temp_var based on the block size and block offset.
  switch (bsize) {
    case BLOCK_128X128: force_skip_low_temp_var = variance_low[0]; break;
    case BLOCK_128X64:
      assert((mi_col & 0x1F) == 0);
      force_skip_low_temp_var = variance_low[1 + ((mi_row & 0x1F) != 0)];
      break;
    case BLOCK_64X128:
      assert((mi_row & 0x1F) == 0);
      force_skip_low_temp_var = variance_low[3 + ((mi_col & 0x1F) != 0)];
      break;
    case BLOCK_64X64:
      // Location of this 64x64 block inside the 128x128 superblock
      force_skip_low_temp_var = variance_low[5 + idx64];
      break;
    case BLOCK_64X32:
      x = (mi_col & 0x1F) >> 4;
      y = (mi_row & 0x1F) >> 3;
      /*
      .---------------.---------------.
      | x=0,y=0,idx=0 | x=0,y=0,idx=2 |
      :---------------+---------------:
      | x=0,y=1,idx=1 | x=1,y=1,idx=3 |
      :---------------+---------------:
      | x=0,y=2,idx=4 | x=1,y=2,idx=6 |
      :---------------+---------------:
      | x=0,y=3,idx=5 | x=1,y=3,idx=7 |
      '---------------'---------------'
      */
      const int idx64x32 = (x << 1) + (y % 2) + ((y >> 1) << 2);
      force_skip_low_temp_var = variance_low[9 + idx64x32];
      break;
    case BLOCK_32X64:
      x = (mi_col & 0x1F) >> 3;
      y = (mi_row & 0x1F) >> 4;
      const int idx32x64 = (y << 2) + x;
      force_skip_low_temp_var = variance_low[17 + idx32x64];
      break;
    case BLOCK_32X32:
      force_skip_low_temp_var = variance_low[25 + (idx64 << 2) + idx32];
      break;
    case BLOCK_32X16:
    case BLOCK_16X32:
    case BLOCK_16X16:
      force_skip_low_temp_var =
          variance_low[41 + (idx64 << 4) + (idx32 << 2) + idx16];
      break;
    default: break;
  }
  return force_skip_low_temp_var;
}

#define FILTER_SEARCH_SIZE 2
/*!\brief Searches for the best intrpolation filter
 *
 * \ingroup nonrd_mode_search
 * \callgraph
 * \callergraph
 * Iterates through subset of possible interpolation filters (currently
 * only EIGHTTAP_REGULAR and EIGTHTAP_SMOOTH in both directions) and selects
 * the one that gives lowest RD cost. RD cost is calculated using curvfit model
 *
 * \param[in]    cpi                  Top-level encoder structure
 * \param[in]    x                    Pointer to structure holding all the
 *                                    data for the current macroblock
 * \param[in]    this_rdc             Pointer to calculated RD Cost
 * \param[in]    mi_row               Row index in 4x4 units
 * \param[in]    mi_col               Column index in 4x4 units
 * \param[in]    tmp                  Pointer to a temporary buffer for
 *                                    prediction re-use
 * \param[in]    bsize                Current block size
 * \param[in]    reuse_inter_pred     Flag, indicating prediction re-use
 * \param[out]   this_mode_pred       Pointer to store prediction buffer
 *                                    for prediction re-use
 * \param[out]   this_early_term      Flag, indicating that transform can be
 *                                    skipped
 * \param[in]    use_model_yrd_large  Flag, indicating special logic to handle
 *                                    large blocks
 *
 * \return Nothing is returned. Instead, calculated RD cost is placed to
 * \c this_rdc and best filter is placed to \c mi->interp_filters. In case
 * \c reuse_inter_pred flag is set, this function also ouputs
 * \c this_mode_pred. Also \c this_early_temp is set if transform can be
 * skipped
 */
static void search_filter_ref(AV1_COMP *cpi, MACROBLOCK *x, RD_STATS *this_rdc,
                              int mi_row, int mi_col, PRED_BUFFER *tmp,
                              BLOCK_SIZE bsize, int reuse_inter_pred,
                              PRED_BUFFER **this_mode_pred,
                              int *this_early_term, int use_model_yrd_large) {
  AV1_COMMON *const cm = &cpi->common;
  MACROBLOCKD *const xd = &x->e_mbd;
  struct macroblockd_plane *const pd = &xd->plane[0];
  MB_MODE_INFO *const mi = xd->mi[0];
  const int bw = block_size_wide[bsize];
  RD_STATS pf_rd_stats[FILTER_SEARCH_SIZE] = { 0 };
  TX_SIZE pf_tx_size[FILTER_SEARCH_SIZE] = { 0 };
  PRED_BUFFER *current_pred = *this_mode_pred;
  int best_skip = 0;
  int best_early_term = 0;
  int64_t best_cost = INT64_MAX;
  int best_filter_index = -1;
  InterpFilter filters[FILTER_SEARCH_SIZE] = { EIGHTTAP_REGULAR,
                                               EIGHTTAP_SMOOTH };
  for (int i = 0; i < FILTER_SEARCH_SIZE; ++i) {
    int64_t cost;
    InterpFilter filter = filters[i];
#if CONFIG_REMOVE_DUAL_FILTER
    mi->interp_fltr = filter;
#else
    mi->interp_filters = av1_broadcast_interp_filter(filter);
#endif  // CONFIG_REMOVE_DUAL_FILTER
    av1_enc_build_inter_predictor_y(xd, mi_row, mi_col);
    if (use_model_yrd_large)
      model_skip_for_sb_y_large(cpi, bsize, mi_row, mi_col, x, xd,
                                &pf_rd_stats[i], this_early_term, 1);
    else
      model_rd_for_sb_y(cpi, bsize, x, xd, &pf_rd_stats[i], 1);
    pf_rd_stats[i].rate +=
        av1_get_switchable_rate(x, xd,
#if !CONFIG_REMOVE_DUAL_FILTER
                                cm->seq_params.enable_dual_filter,
#endif  // !CONFIG_REMOVE_DUAL_FILTER
                                cm->features.interp_filter);
    cost = RDCOST(x->rdmult, pf_rd_stats[i].rate, pf_rd_stats[i].dist);
    pf_tx_size[i] = mi->tx_size;
    if (cost < best_cost) {
      best_filter_index = i;
      best_cost = cost;
      best_skip = pf_rd_stats[i].skip_txfm;
      best_early_term = *this_early_term;
      if (reuse_inter_pred) {
        if (*this_mode_pred != current_pred) {
          free_pred_buffer(*this_mode_pred);
          *this_mode_pred = current_pred;
        }
        current_pred = &tmp[get_pred_buffer(tmp, 3)];
        pd->dst.buf = current_pred->data;
        pd->dst.stride = bw;
      }
    }
  }
  assert(best_filter_index >= 0 && best_filter_index < FILTER_SEARCH_SIZE);
  if (reuse_inter_pred && *this_mode_pred != current_pred)
    free_pred_buffer(current_pred);

#if CONFIG_REMOVE_DUAL_FILTER
  mi->interp_fltr = filters[best_filter_index];
#else
  mi->interp_filters = av1_broadcast_interp_filter(filters[best_filter_index]);
#endif  // CONFIG_REMOVE_DUAL_FILTER
  mi->tx_size = pf_tx_size[best_filter_index];
  this_rdc->rate = pf_rd_stats[best_filter_index].rate;
  this_rdc->dist = pf_rd_stats[best_filter_index].dist;
  this_rdc->sse = pf_rd_stats[best_filter_index].sse;
  this_rdc->skip_txfm = (best_skip || best_early_term);
  *this_early_term = best_early_term;
  if (reuse_inter_pred) {
    pd->dst.buf = (*this_mode_pred)->data;
    pd->dst.stride = (*this_mode_pred)->stride;
  } else if (best_filter_index < FILTER_SEARCH_SIZE - 1) {
    av1_enc_build_inter_predictor_y(xd, mi_row, mi_col);
  }
}

#define COLLECT_PICK_MODE_STAT 0

#if COLLECT_PICK_MODE_STAT
typedef struct _mode_search_stat {
  int32_t num_blocks[BLOCK_SIZES];
  int64_t avg_block_times[BLOCK_SIZES];
  int32_t num_searches[BLOCK_SIZES][MB_MODE_COUNT];
  int32_t num_nonskipped_searches[BLOCK_SIZES][MB_MODE_COUNT];
  int64_t search_times[BLOCK_SIZES][MB_MODE_COUNT];
  int64_t nonskipped_search_times[BLOCK_SIZES][MB_MODE_COUNT];
  struct aom_usec_timer timer1;
  struct aom_usec_timer timer2;
} mode_search_stat;
#endif  // COLLECT_PICK_MODE_STAT

static void compute_intra_yprediction(const AV1_COMMON *cm,
                                      PREDICTION_MODE mode, BLOCK_SIZE bsize,
                                      MACROBLOCK *x, MACROBLOCKD *xd) {
  struct macroblockd_plane *const pd = &xd->plane[0];
  struct macroblock_plane *const p = &x->plane[0];
  uint8_t *const src_buf_base = p->src.buf;
  uint8_t *const dst_buf_base = pd->dst.buf;
  const int src_stride = p->src.stride;
  const int dst_stride = pd->dst.stride;
  int plane = 0;
  int row, col;
  // block and transform sizes, in number of 4x4 blocks log 2 ("*_b")
  // 4x4=0, 8x8=2, 16x16=4, 32x32=6, 64x64=8
  // transform size varies per plane, look it up in a common way.
  const TX_SIZE tx_size = max_txsize_lookup[bsize];
  const BLOCK_SIZE plane_bsize =
      get_plane_block_size(bsize, pd->subsampling_x, pd->subsampling_y);
  // If mb_to_right_edge is < 0 we are in a situation in which
  // the current block size extends into the UMV and we won't
  // visit the sub blocks that are wholly within the UMV.
  const int max_blocks_wide = max_block_wide(xd, plane_bsize, plane);
  const int max_blocks_high = max_block_high(xd, plane_bsize, plane);
  // Keep track of the row and column of the blocks we use so that we know
  // if we are in the unrestricted motion border.
  for (row = 0; row < max_blocks_high; row += (1 << tx_size)) {
    // Skip visiting the sub blocks that are wholly within the UMV.
    for (col = 0; col < max_blocks_wide; col += (1 << tx_size)) {
      p->src.buf = &src_buf_base[4 * (row * (int64_t)src_stride + col)];
      pd->dst.buf = &dst_buf_base[4 * (row * (int64_t)dst_stride + col)];
      av1_predict_intra_block(
          cm, xd, block_size_wide[bsize], block_size_high[bsize], tx_size, mode,
          0, 0, FILTER_INTRA_MODES,
#if CONFIG_DERIVED_INTRA_MODE
          0,
#endif  // CONFIG_DERIVED_INTRA_MODE
          pd->dst.buf, dst_stride, pd->dst.buf, dst_stride, 0, 0, plane);
    }
  }
  p->src.buf = src_buf_base;
  pd->dst.buf = dst_buf_base;
}

void av1_nonrd_pick_intra_mode(AV1_COMP *cpi, MACROBLOCK *x, RD_STATS *rd_cost,
                               BLOCK_SIZE bsize, PICK_MODE_CONTEXT *ctx) {
  AV1_COMMON *const cm = &cpi->common;
  MACROBLOCKD *const xd = &x->e_mbd;
  MB_MODE_INFO *const mi = xd->mi[0];
  RD_STATS this_rdc, best_rdc;
  struct estimate_block_intra_args args = { cpi, x, DC_PRED, 1, 0 };
  const TxfmSearchParams *txfm_params = &x->txfm_search_params;
  const TX_SIZE intra_tx_size =
      AOMMIN(max_txsize_lookup[bsize],
             tx_mode_to_biggest_tx_size[txfm_params->tx_mode_search_type]);
  int *bmode_costs;
  const MB_MODE_INFO *above_mi = xd->above_mbmi;
  const MB_MODE_INFO *left_mi = xd->left_mbmi;
  const PREDICTION_MODE A = av1_above_block_mode(above_mi);
  const PREDICTION_MODE L = av1_left_block_mode(left_mi);
  bmode_costs = x->mode_costs.y_mode_costs[A][L];

  av1_invalid_rd_stats(&best_rdc);
  av1_invalid_rd_stats(&this_rdc);

#if CONFIG_NEW_REF_SIGNALING
  init_mbmi(mi, DC_PRED, INTRA_FRAME_NRS, INVALID_IDX, cm, xd->sbi);
#else
  init_mbmi(mi, DC_PRED, INTRA_FRAME, NONE_FRAME, cm, xd->sbi);
#endif  // CONFIG_NEW_REF_SIGNALING
  mi->mv[0].as_int = mi->mv[1].as_int = INVALID_MV;

  // Change the limit of this loop to add other intra prediction
  // mode tests.
  for (int i = 0; i < 4; ++i) {
    PREDICTION_MODE this_mode = intra_mode_list[i];
    this_rdc.dist = this_rdc.rate = 0;
    args.mode = this_mode;
    args.skippable = 1;
    args.rdc = &this_rdc;
    mi->tx_size = intra_tx_size;
    av1_foreach_transformed_block_in_plane(xd, bsize, 0, estimate_block_intra,
                                           &args);
    if (args.skippable) {
      this_rdc.rate = av1_cost_symbol(av1_get_skip_txfm_cdf(xd)[1]);
    } else {
      this_rdc.rate += av1_cost_symbol(av1_get_skip_txfm_cdf(xd)[0]);
    }
    this_rdc.rate += bmode_costs[this_mode];
    this_rdc.rdcost = RDCOST(x->rdmult, this_rdc.rate, this_rdc.dist);

    if (this_rdc.rdcost < best_rdc.rdcost) {
      best_rdc = this_rdc;
      mi->mode = this_mode;
    }
  }

  *rd_cost = best_rdc;

#if CONFIG_INTERNAL_STATS && !CONFIG_NEW_REF_SIGNALING
  store_coding_context(x, ctx, mi->mode);
#else
  store_coding_context(x, ctx);
#endif  // CONFIG_INTERNAL_STATS && !CONFIG_NEW_REF_SIGNALING
}

static AOM_INLINE int is_same_gf_and_last_scale(AV1_COMMON *cm) {
  struct scale_factors *const sf_last = get_ref_scale_factors(cm, LAST_FRAME);
  struct scale_factors *const sf_golden =
      get_ref_scale_factors(cm, GOLDEN_FRAME);
  return ((sf_last->x_scale_fp == sf_golden->x_scale_fp) &&
          (sf_last->y_scale_fp == sf_golden->y_scale_fp));
}

#if CONFIG_NEW_REF_SIGNALING
static AOM_INLINE void get_ref_frame_use_mask(AV1_COMP *cpi, MACROBLOCK *x,
                                              MB_MODE_INFO *mi, int mi_row,
                                              int mi_col, int bsize,
                                              int gf_temporal_ref,
                                              int use_ref_frame[],
                                              int *force_skip_low_temp_var) {
  AV1_COMMON *const cm = &cpi->common;
  const int is_small_sb = (cm->seq_params.sb_size == BLOCK_64X64);

  int use_alt_ref_frame = cpi->sf.rt_sf.use_nonrd_altref_frame;
  int use_golden_ref_frame = 1;
  int golden_frame = cm->new_ref_frame_data.past_refs[0];
  int altref_frame = cm->new_ref_frame_data.past_refs[0];

  use_ref_frame[0] = 1;  // we never skip 0

  if (cpi->rc.frames_since_golden == 0 && gf_temporal_ref) {
    use_golden_ref_frame = 0;
  }

  if (cpi->sf.rt_sf.short_circuit_low_temp_var &&
      x->nonrd_prune_ref_frame_search) {
    if (is_small_sb)
      *force_skip_low_temp_var = get_force_skip_low_temp_var_small_sb(
          &x->part_search_info.variance_low[0], mi_row, mi_col, bsize);
    else
      *force_skip_low_temp_var = get_force_skip_low_temp_var(
          &x->part_search_info.variance_low[0], mi_row, mi_col, bsize);
    // If force_skip_low_temp_var is set, skip golden reference.
    if (*force_skip_low_temp_var) {
      use_golden_ref_frame = 0;
      use_alt_ref_frame = 0;
    }
  }
  (void)mi;

  use_alt_ref_frame = cpi->common.ref_frame_flags_nrs & (1 << altref_frame)
                          ? use_alt_ref_frame
                          : 0;
  use_golden_ref_frame = cpi->common.ref_frame_flags & (1 << golden_frame)
                             ? use_golden_ref_frame
                             : 0;

  use_ref_frame[altref_frame] = use_alt_ref_frame;
  use_ref_frame[golden_frame] = use_golden_ref_frame;
}
#else
static AOM_INLINE void get_ref_frame_use_mask(AV1_COMP *cpi, MACROBLOCK *x,
                                              MB_MODE_INFO *mi, int mi_row,
                                              int mi_col, int bsize,
                                              int gf_temporal_ref,
                                              int use_ref_frame[],
                                              int *force_skip_low_temp_var) {
  AV1_COMMON *const cm = &cpi->common;
  const int is_small_sb = (cm->seq_params.sb_size == BLOCK_64X64);

  int use_alt_ref_frame = cpi->sf.rt_sf.use_nonrd_altref_frame;
  int use_golden_ref_frame = 1;

  use_ref_frame[LAST_FRAME] = 1;  // we never skip LAST

  if (cpi->rc.frames_since_golden == 0 && gf_temporal_ref) {
    use_golden_ref_frame = 0;
  }

  if (cpi->sf.rt_sf.short_circuit_low_temp_var &&
      x->nonrd_prune_ref_frame_search) {
    if (is_small_sb)
      *force_skip_low_temp_var = get_force_skip_low_temp_var_small_sb(
          &x->part_search_info.variance_low[0], mi_row, mi_col, bsize);
    else
      *force_skip_low_temp_var = get_force_skip_low_temp_var(
          &x->part_search_info.variance_low[0], mi_row, mi_col, bsize);
    // If force_skip_low_temp_var is set, skip golden reference.
    if (*force_skip_low_temp_var) {
      use_golden_ref_frame = 0;
      use_alt_ref_frame = 0;
    }
  }

  const struct segmentation *const seg = &cm->seg;
  if (segfeature_active(seg, mi->segment_id, SEG_LVL_REF_FRAME) &&
      get_segdata(seg, mi->segment_id, SEG_LVL_REF_FRAME) == GOLDEN_FRAME) {
    use_golden_ref_frame = 1;
    use_alt_ref_frame = 0;
  }

  use_alt_ref_frame =
      cpi->common.ref_frame_flags & AOM_ALT_FLAG ? use_alt_ref_frame : 0;
  use_golden_ref_frame =
      cpi->common.ref_frame_flags & AOM_GOLD_FLAG ? use_golden_ref_frame : 0;

  use_ref_frame[ALTREF_FRAME] = use_alt_ref_frame;
  use_ref_frame[GOLDEN_FRAME] = use_golden_ref_frame;
}
#endif  // CONFIG_NEW_REF_SIGNALING

/*!\brief Estimates best intra mode for inter mode search
 *
 * \ingroup nonrd_mode_search
 * \callgraph
 * \callergraph
 *
 * Using heuristics based on best inter mode, block size, and other decides
 * whether to check intra modes. If so, estimates and selects best intra mode
 * from the reduced set of intra modes (max 4 intra modes checked)
 *
 * \param[in]    cpi                      Top-level encoder structure
 * \param[in]    x                        Pointer to structure holding all the
 *                                        data for the current macroblock
 * \param[in]    bsize                    Current block size
 * \param[in]    use_modeled_non_rd_cost  Flag, indicating usage of curvfit
 *                                        model for RD cost
 * \param[in]    best_early_term          Flag, indicating that TX for the
 *                                        best inter mode was skipped
 * \param[in]    ref_cost_intra           Cost of signalling intra mode
 * \param[in]    reuse_prediction         Flag, indicating prediction re-use
 * \param[in]    orig_dst                 Original destination buffer
 * \param[in]    tmp_buffers              Pointer to a temporary buffers for
 *                                        prediction re-use
 * \param[out]   this_mode_pred           Pointer to store prediction buffer
 *                                        for prediction re-use
 * \param[in]    best_rdc                 Pointer to RD cost for the best
 *                                        selected intra mode
 * \param[in]    best_pickmode            Pointer to a structure containing
 *                                        best mode picked so far
 *
 * \return Nothing is returned. Instead, calculated RD cost is placed to
 * \c best_rdc and best selected mode is placed to \c best_pickmode
 */
static void estimate_intra_mode(
    AV1_COMP *cpi, MACROBLOCK *x, BLOCK_SIZE bsize, int use_modeled_non_rd_cost,
    int best_early_term, unsigned int ref_cost_intra, int reuse_prediction,
    struct buf_2d *orig_dst, PRED_BUFFER *tmp_buffers,
    PRED_BUFFER **this_mode_pred, RD_STATS *best_rdc,
    BEST_PICKMODE *best_pickmode) {
  AV1_COMMON *const cm = &cpi->common;
  MACROBLOCKD *const xd = &x->e_mbd;
  MB_MODE_INFO *const mi = xd->mi[0];
  const TxfmSearchParams *txfm_params = &x->txfm_search_params;
  const unsigned char segment_id = mi->segment_id;
  const int *const rd_threshes = cpi->rd.threshes[segment_id][bsize];
  const int *const rd_thresh_freq_fact = x->thresh_freq_fact[bsize];
  const int mi_row = xd->mi_row;
  const int mi_col = xd->mi_col;
  struct macroblockd_plane *const pd = &xd->plane[0];

  const CommonQuantParams *quant_params = &cm->quant_params;

  RD_STATS this_rdc;

  int intra_cost_penalty = av1_get_intra_cost_penalty(
      quant_params->base_qindex, quant_params->y_dc_delta_q,
#if CONFIG_EXTQUANT
      cm->seq_params.base_y_dc_delta_q,
#endif  // CONFIG_EXTQUANT
      cm->seq_params.bit_depth);
  int64_t inter_mode_thresh = RDCOST(x->rdmult, intra_cost_penalty, 0);
  int perform_intra_pred = cpi->sf.rt_sf.check_intra_pred_nonrd;

  int do_early_exit_rdthresh = 1;

  uint32_t spatial_var_thresh = 50;
  int motion_thresh = 32;
  // Adjust thresholds to make intra mode likely tested if the other
  // references (golden, alt) are skipped/not checked.
  if (cpi->sf.rt_sf.use_nonrd_altref_frame == 0 &&
      cpi->sf.rt_sf.nonrd_prune_ref_frame_search > 0) {
    spatial_var_thresh = 150;
    motion_thresh = 0;
  }

  // Some adjustments to checking intra mode based on source variance.
  if (x->source_variance < spatial_var_thresh) {
    // If the best inter mode is large motion or non-LAST ref reduce intra cost
    // penalty, so intra mode is more likely tested.
#if CONFIG_NEW_REF_SIGNALING
    if (best_pickmode->best_ref_frame != get_closest_pastcur_ref_index(cm) ||
#else
    if (best_pickmode->best_ref_frame != LAST_FRAME ||
#endif  // CONFIG_NEW_REF_SIGNALING
        abs(mi->mv[0].as_mv.row) >= motion_thresh ||
        abs(mi->mv[0].as_mv.col) >= motion_thresh) {
      intra_cost_penalty = intra_cost_penalty >> 2;
      inter_mode_thresh = RDCOST(x->rdmult, intra_cost_penalty, 0);
      do_early_exit_rdthresh = 0;
    }
    // For big blocks worth checking intra (since only DC will be checked),
    // even if best_early_term is set.
    if (bsize >= BLOCK_32X32) best_early_term = 0;
  } else if (cpi->sf.rt_sf.source_metrics_sb_nonrd &&
             x->content_state_sb == kLowSad) {
    perform_intra_pred = 0;
  }

  if (cpi->sf.rt_sf.skip_intra_pred_if_tx_skip && best_rdc->skip_txfm &&
      best_pickmode->best_mode_initial_skip_flag) {
    perform_intra_pred = 0;
  }

  if (!(best_rdc->rdcost == INT64_MAX ||
        (perform_intra_pred && !best_early_term &&
         best_rdc->rdcost > inter_mode_thresh &&
         bsize <= cpi->sf.part_sf.max_intra_bsize))) {
    return;
  }

  struct estimate_block_intra_args args = { cpi, x, DC_PRED, 1, 0 };
  TX_SIZE intra_tx_size = AOMMIN(
      AOMMIN(max_txsize_lookup[bsize],
             tx_mode_to_biggest_tx_size[txfm_params->tx_mode_search_type]),
      TX_16X16);

  PRED_BUFFER *const best_pred = best_pickmode->best_pred;
  if (reuse_prediction && best_pred != NULL) {
    const int bh = block_size_high[bsize];
    const int bw = block_size_wide[bsize];
    if (best_pred->data == orig_dst->buf) {
      *this_mode_pred = &tmp_buffers[get_pred_buffer(tmp_buffers, 3)];
      aom_convolve_copy(best_pred->data, best_pred->stride,
                        (*this_mode_pred)->data, (*this_mode_pred)->stride, bw,
                        bh);
      best_pickmode->best_pred = *this_mode_pred;
    }
  }
  pd->dst = *orig_dst;

  for (int i = 0; i < 4; ++i) {
    const PREDICTION_MODE this_mode = intra_mode_list[i];
#if CONFIG_NEW_REF_SIGNALING
    const int mode_rd_thresh = rd_threshes[this_mode];
#else
    const THR_MODES mode_index = mode_idx[INTRA_FRAME][mode_offset(this_mode)];
    const int mode_rd_thresh = rd_threshes[mode_index];
#endif  // CONFIG_NEW_REF_SIGNALING

    // Only check DC for blocks >= 32X32.
    if (this_mode > 0 &&
        (bsize >= BLOCK_32X32 || cpi->sf.rt_sf.nonrd_intra_dc_only))
      continue;

    if (rd_less_than_thresh(best_rdc->rdcost, mode_rd_thresh,
#if CONFIG_NEW_REF_SIGNALING
                            rd_thresh_freq_fact[this_mode]) &&
#else
                            rd_thresh_freq_fact[mode_index]) &&
#endif  // CONFIG_NEW_REF_SIGNALING
        (do_early_exit_rdthresh || this_mode == SMOOTH_PRED)) {
      continue;
    }
    const BLOCK_SIZE uv_bsize = get_plane_block_size(
        bsize, xd->plane[1].subsampling_x, xd->plane[1].subsampling_y);

    mi->mode = this_mode;
#if CONFIG_NEW_REF_SIGNALING
    mi->ref_frame_nrs[0] = INTRA_FRAME_NRS;
    mi->ref_frame_nrs[1] = INVALID_IDX;
#else
    mi->ref_frame[0] = INTRA_FRAME;
    mi->ref_frame[1] = NONE_FRAME;
#endif  // CONFIG_NEW_REF_SIGNALING

    av1_invalid_rd_stats(&this_rdc);
    args.mode = this_mode;
    args.skippable = 1;
    args.rdc = &this_rdc;
    mi->tx_size = intra_tx_size;
    compute_intra_yprediction(cm, this_mode, bsize, x, xd);
    // Look into selecting tx_size here, based on prediction residual.
    if (use_modeled_non_rd_cost)
      model_rd_for_sb_y(cpi, bsize, x, xd, &this_rdc, 1);
    else
      block_yrd(cpi, x, mi_row, mi_col, &this_rdc, &args.skippable, bsize,
                mi->tx_size);
    // TODO(kyslov@) Need to account for skippable
    if (x->color_sensitivity[0]) {
      av1_foreach_transformed_block_in_plane(xd, uv_bsize, 1,
                                             estimate_block_intra, &args);
    }
    if (x->color_sensitivity[1]) {
      av1_foreach_transformed_block_in_plane(xd, uv_bsize, 2,
                                             estimate_block_intra, &args);
    }

    int mode_cost = 0;
    if (av1_is_directional_mode(this_mode) && av1_use_angle_delta(bsize)) {
      mode_cost +=
          x->mode_costs.angle_delta_cost[this_mode - V_PRED]
                                        [MAX_ANGLE_DELTA +
                                         mi->angle_delta[PLANE_TYPE_Y]];
    }
    if (this_mode == DC_PRED && av1_filter_intra_allowed_bsize(cm, bsize)) {
      mode_cost += x->mode_costs.filter_intra_cost[bsize][0];
    }
    this_rdc.rate += ref_cost_intra;
    this_rdc.rate += intra_cost_penalty;
    this_rdc.rate += mode_cost;
    this_rdc.rdcost = RDCOST(x->rdmult, this_rdc.rate, this_rdc.dist);

    if (this_rdc.rdcost < best_rdc->rdcost) {
      *best_rdc = this_rdc;
      best_pickmode->best_mode = this_mode;
      best_pickmode->best_tx_size = mi->tx_size;
#if CONFIG_NEW_REF_SIGNALING
      best_pickmode->best_ref_frame = INTRA_FRAME_NRS;
#else
      best_pickmode->best_ref_frame = INTRA_FRAME;
#endif  // CONFIG_NEW_REF_SIGNALING
      mi->uv_mode = this_mode;
      mi->mv[0].as_int = INVALID_MV;
      mi->mv[1].as_int = INVALID_MV;
    }
  }
  mi->tx_size = best_pickmode->best_tx_size;
}

static AOM_INLINE int is_filter_search_enabled(const AV1_COMP *cpi, int mi_row,
                                               int mi_col, BLOCK_SIZE bsize) {
  const AV1_COMMON *const cm = &cpi->common;
  int enable_filter_search = 0;

  if (cpi->sf.rt_sf.use_nonrd_filter_search) {
    enable_filter_search = 1;
    if (cpi->sf.interp_sf.cb_pred_filter_search) {
      const int bsl = mi_size_wide_log2[bsize];
      enable_filter_search =
          (((mi_row + mi_col) >> bsl) +
           get_chessboard_index(cm->current_frame.frame_number)) &
          0x1;
    }
  }
  return enable_filter_search;
}

static AOM_INLINE int skip_mode_by_threshold(
    PREDICTION_MODE mode, MV_REFERENCE_FRAME ref_frame, int_mv mv,
    int frames_since_golden, const int *const rd_threshes,
    const int *const rd_thresh_freq_fact, int64_t best_cost, int best_skip) {
  int skip_this_mode = 0;
#if CONFIG_NEW_REF_SIGNALING
  int mode_rd_thresh = best_skip ? rd_threshes[mode] << 1 : rd_threshes[mode];
#else
  const THR_MODES mode_index = mode_idx[ref_frame][INTER_OFFSET(mode)];
  int mode_rd_thresh =
      best_skip ? rd_threshes[mode_index] << 1 : rd_threshes[mode_index];
#endif  // CONFIG_NEW_REF_SIGNALING

  // Increase mode_rd_thresh value for non-LAST for improved encoding
  // speed
  if (ref_frame != LAST_FRAME) {
    mode_rd_thresh = mode_rd_thresh << 1;
    if (ref_frame == GOLDEN_FRAME && frames_since_golden > 4)
      mode_rd_thresh = mode_rd_thresh << 1;
  }

#if CONFIG_NEW_REF_SIGNALING
  if (rd_less_than_thresh(best_cost, mode_rd_thresh, rd_thresh_freq_fact[mode]))
#else
  if (rd_less_than_thresh(best_cost, mode_rd_thresh,
                          rd_thresh_freq_fact[mode_index]))
#endif  // CONFIG_NEW_REF_SIGNALING
    if (mv.as_int != 0) skip_this_mode = 1;

  return skip_this_mode;
}

static AOM_INLINE int skip_mode_by_low_temp(PREDICTION_MODE mode,
                                            MV_REFERENCE_FRAME ref_frame,
                                            BLOCK_SIZE bsize,
                                            int content_state_sb, int_mv mv,
                                            int force_skip_low_temp_var) {
  // Skip non-zeromv mode search for non-LAST frame if force_skip_low_temp_var
  // is set. If nearestmv for golden frame is 0, zeromv mode will be skipped
  // later.
  if (force_skip_low_temp_var && ref_frame != LAST_FRAME && mv.as_int != 0) {
    return 1;
  }

  if (content_state_sb != kHighSad && bsize >= BLOCK_64X64 &&
      force_skip_low_temp_var && mode == NEWMV) {
    return 1;
  }
  return 0;
}

static AOM_INLINE int skip_mode_by_bsize_and_ref_frame(
    const AV1_COMMON *const cm, PREDICTION_MODE mode,
#if CONFIG_NEW_REF_SIGNALING
    MV_REFERENCE_FRAME_NRS ref_frame,
#else
    MV_REFERENCE_FRAME ref_frame,
#endif  // CONFIG_NEW_REF_SIGNALING
    BLOCK_SIZE bsize, int extra_prune, unsigned int sse_zeromv_norm) {
  const unsigned int thresh_skip_golden = 500;

#if CONFIG_NEW_REF_SIGNALING
  const MV_REFERENCE_FRAME_NRS last_frame = get_closest_pastcur_ref_index(cm);
  if (ref_frame != last_frame && sse_zeromv_norm < thresh_skip_golden &&
      mode == NEWMV)
    return 1;

  if (bsize == BLOCK_128X128 && mode == NEWMV) return 1;

  // Skip testing non-LAST if this flag is set.
  if (extra_prune) {
    if (extra_prune > 1 && ref_frame != last_frame &&
        (bsize > BLOCK_64X64 || (bsize > BLOCK_16X16 && mode == NEWMV)))
      return 1;

    if (ref_frame != last_frame && mode == NEARMV) return 1;
  }
#else
  (void)cm;
  if (ref_frame != LAST_FRAME && sse_zeromv_norm < thresh_skip_golden &&
      mode == NEWMV)
    return 1;

  if (bsize == BLOCK_128X128 && mode == NEWMV) return 1;

  // Skip testing non-LAST if this flag is set.
  if (extra_prune) {
    if (extra_prune > 1 && ref_frame != LAST_FRAME &&
        (bsize > BLOCK_64X64 || (bsize > BLOCK_16X16 && mode == NEWMV)))
      return 1;

    if (ref_frame != LAST_FRAME && mode == NEARMV) return 1;
  }
#endif  // CONFIG_NEW_REF_SIGNALING
  return 0;
}

void av1_nonrd_pick_inter_mode_sb(AV1_COMP *cpi, TileDataEnc *tile_data,
                                  MACROBLOCK *x, RD_STATS *rd_cost,
                                  BLOCK_SIZE bsize, PICK_MODE_CONTEXT *ctx) {
  AV1_COMMON *const cm = &cpi->common;
  MACROBLOCKD *const xd = &x->e_mbd;
  MB_MODE_INFO *const mi = xd->mi[0];
  struct macroblockd_plane *const pd = &xd->plane[0];

  BEST_PICKMODE best_pickmode;
#if COLLECT_PICK_MODE_STAT
  static mode_search_stat ms_stat;
#endif
#if CONFIG_NEW_REF_SIGNALING
  MV_REFERENCE_FRAME_NRS ref_frame_nrs;
  int_mv frame_mv[MB_MODE_COUNT][REF_FRAMES_NRS];
  uint8_t mode_checked[MB_MODE_COUNT][REF_FRAMES_NRS];
  struct buf_2d yv12_mb[REF_FRAMES_NRS][MAX_MB_PLANE];
#else
  MV_REFERENCE_FRAME ref_frame;
  int_mv frame_mv[MB_MODE_COUNT][REF_FRAMES];
  uint8_t mode_checked[MB_MODE_COUNT][REF_FRAMES];
  struct buf_2d yv12_mb[REF_FRAMES][MAX_MB_PLANE];
#endif  // CONFIG_NEW_REF_SIGNALING
  RD_STATS this_rdc, best_rdc;
  const unsigned char segment_id = mi->segment_id;
  const int *const rd_threshes = cpi->rd.threshes[segment_id][bsize];
  const int *const rd_thresh_freq_fact = x->thresh_freq_fact[bsize];
  const InterpFilter filter_ref = cm->features.interp_filter;
  int best_early_term = 0;
#if CONFIG_NEW_REF_SIGNALING
  unsigned int ref_costs_single[REF_FRAMES_NRS];
  unsigned int ref_costs_comp[REF_FRAMES_NRS][REF_FRAMES_NRS];
  int use_ref_frame_mask[REF_FRAMES_NRS] = { 0 };
#else
  unsigned int ref_costs_single[REF_FRAMES],
      ref_costs_comp[REF_FRAMES][REF_FRAMES];
  int use_ref_frame_mask[REF_FRAMES] = { 0 };
#endif  // CONFIG_NEW_REF_SIGNALING
  int force_skip_low_temp_var = 0;
  unsigned int sse_zeromv_norm = UINT_MAX;
  int num_inter_modes = RT_INTER_MODES;
  PRED_BUFFER tmp[4];
  DECLARE_ALIGNED(16, uint8_t, pred_buf[3 * 128 * 128]);
  PRED_BUFFER *this_mode_pred = NULL;
  const int reuse_inter_pred =
      cpi->sf.rt_sf.reuse_inter_pred_nonrd && cm->seq_params.bit_depth == 8;
  const int bh = block_size_high[bsize];
  const int bw = block_size_wide[bsize];
  const int pixels_in_block = bh * bw;
  struct buf_2d orig_dst = pd->dst;
  const CommonQuantParams *quant_params = &cm->quant_params;
  const TxfmSearchParams *txfm_params = &x->txfm_search_params;
  TxfmSearchInfo *txfm_info = &x->txfm_search_info;
#if COLLECT_PICK_MODE_STAT
  aom_usec_timer_start(&ms_stat.timer2);
#endif
  const InterpFilter default_interp_filter = EIGHTTAP_REGULAR;
  int64_t thresh_sad_pred = INT64_MAX;
  const int mi_row = xd->mi_row;
  const int mi_col = xd->mi_col;

  init_best_pickmode(&best_pickmode);

#if CONFIG_NEW_REF_SIGNALING
  av1_collect_neighbors_ref_counts_nrs(cm, xd);
#else
  av1_collect_neighbors_ref_counts(xd);
#endif  // CONFIG_NEW_REF_SIGNALING

  const ModeCosts *mode_costs = &x->mode_costs;

  estimate_single_ref_frame_costs(cm, xd, mode_costs, segment_id,
                                  ref_costs_single);
  if (cpi->sf.rt_sf.use_comp_ref_nonrd)
    estimate_comp_ref_frame_costs(cm, xd, mode_costs, segment_id,
                                  ref_costs_comp);

  memset(&mode_checked[0][0], 0, MB_MODE_COUNT * REF_FRAMES);
  if (reuse_inter_pred) {
    for (int i = 0; i < 3; i++) {
      tmp[i].data = &pred_buf[pixels_in_block * i];
      tmp[i].stride = bw;
      tmp[i].in_use = 0;
    }
    tmp[3].data = pd->dst.buf;
    tmp[3].stride = pd->dst.stride;
    tmp[3].in_use = 0;
  }

  txfm_info->skip_txfm = 0;

  // initialize mode decisions
  av1_invalid_rd_stats(&best_rdc);
  av1_invalid_rd_stats(&this_rdc);
  av1_invalid_rd_stats(rd_cost);
  mi->sb_type = bsize;
#if CONFIG_NEW_REF_SIGNALING
  mi->ref_frame_nrs[0] = INVALID_IDX;
  mi->ref_frame_nrs[1] = INVALID_IDX;
#else
  mi->ref_frame[0] = NONE_FRAME;
  mi->ref_frame[1] = NONE_FRAME;
#endif  // CONFIG_NEW_REF_SIGNALING

  const int gf_temporal_ref = is_same_gf_and_last_scale(cm);

  get_ref_frame_use_mask(cpi, x, mi, mi_row, mi_col, bsize, gf_temporal_ref,
                         use_ref_frame_mask, &force_skip_low_temp_var);

#if CONFIG_NEW_REF_SIGNALING
  for (MV_REFERENCE_FRAME_NRS ref_frame_iter_nrs = 0;
       ref_frame_iter_nrs < cm->new_ref_frame_data.n_total_refs;
       ++ref_frame_iter_nrs) {
    if (use_ref_frame_mask[ref_frame_iter_nrs]) {
      find_predictors(cpi, x, ref_frame_iter_nrs, frame_mv, tile_data, yv12_mb,
                      bsize, force_skip_low_temp_var);
    }
  }
#else
  for (MV_REFERENCE_FRAME ref_frame_iter = LAST_FRAME;
       ref_frame_iter <= ALTREF_FRAME; ++ref_frame_iter) {
    if (use_ref_frame_mask[ref_frame_iter]) {
      find_predictors(cpi, x, ref_frame_iter, frame_mv, tile_data, yv12_mb,
                      bsize, force_skip_low_temp_var);
    }
  }
#endif  // CONFIG_NEW_REF_SIGNALING

#if CONFIG_NEW_REF_SIGNALING
  const MV_REFERENCE_FRAME_NRS last_frame = get_closest_pastcur_ref_index(cm);
  const MV_REFERENCE_FRAME_NRS golden_frame =
      cm->new_ref_frame_data.past_refs[0];
  thresh_sad_pred = ((int64_t)x->pred_mv_sad[last_frame]) << 1;
#else
  thresh_sad_pred = ((int64_t)x->pred_mv_sad[LAST_FRAME]) << 1;
#endif  // CONFIG_NEW_REF_SIGNALING
  // Increase threshold for less agressive pruning.
  if (cpi->sf.rt_sf.nonrd_prune_ref_frame_search == 1)
#if CONFIG_NEW_REF_SIGNALING
    thresh_sad_pred += (x->pred_mv_sad[last_frame] >> 2);
#else
    thresh_sad_pred += (x->pred_mv_sad[LAST_FRAME] >> 2);
#endif  // CONFIG_NEW_REF_SIGNALING

  const int large_block = bsize >= BLOCK_32X32;
  const int use_model_yrd_large =
      cpi->oxcf.rc_cfg.mode == AOM_CBR && large_block &&
      !cyclic_refresh_segment_id_boosted(xd->mi[0]->segment_id) &&
      quant_params->base_qindex && cm->seq_params.bit_depth == 8;

  const int enable_filter_search =
      is_filter_search_enabled(cpi, mi_row, mi_col, bsize);

  // TODO(marpan): Look into reducing these conditions. For now constrain
  // it to avoid significant bdrate loss.
  const int use_modeled_non_rd_cost =
      (cpi->sf.rt_sf.use_modeled_non_rd_cost &&
       quant_params->base_qindex > 120 && x->source_variance > 100 &&
       bsize <= BLOCK_16X16 && x->content_state_sb != kLowVarHighSumdiff &&
       x->content_state_sb != kHighSad);

#if COLLECT_PICK_MODE_STAT
  ms_stat.num_blocks[bsize]++;
#endif
#if CONFIG_NEW_REF_SIGNALING
  init_mbmi(mi, DC_PRED, INVALID_IDX, INVALID_IDX, cm, xd->sbi);
#else
  init_mbmi(mi, DC_PRED, NONE_FRAME, NONE_FRAME, cm, xd->sbi);
#endif  // CONFIG_NEW_REF_SIGNALING
  mi->tx_size = AOMMIN(
      AOMMIN(max_txsize_lookup[bsize],
             tx_mode_to_biggest_tx_size[txfm_params->tx_mode_search_type]),
      TX_16X16);

  for (int idx = 0; idx < num_inter_modes; ++idx) {
#if !CONFIG_NEW_REF_SIGNALING
    const struct segmentation *const seg = &cm->seg;
#endif  // !CONFIG_NEW_REF_SIGNALING

    int rate_mv = 0;
    int is_skippable;
    int this_early_term = 0;
    int skip_this_mv = 0;
    PREDICTION_MODE this_mode;
    MB_MODE_INFO_EXT *const mbmi_ext = x->mbmi_ext;
    RD_STATS nonskip_rdc;
    av1_invalid_rd_stats(&nonskip_rdc);

    this_mode = ref_mode_set[idx].pred_mode;
#if CONFIG_OPTFLOW_REFINEMENT
    // Optical modes only enabled with enable_order_hint
    if (this_mode > NEW_NEWMV &&
        !cm->seq_params.order_hint_info.enable_order_hint)
      continue;
#endif  // CONFIG_OPTFLOW_REFINEMENT
#if CONFIG_NEW_REF_SIGNALING
    ref_frame_nrs = ref_mode_set[idx].ref_frame;
#else
    ref_frame = ref_mode_set[idx].ref_frame;
#endif  // CONFIG_NEW_REF_SIGNALING

#if COLLECT_PICK_MODE_STAT
    aom_usec_timer_start(&ms_stat.timer1);
    ms_stat.num_searches[bsize][this_mode]++;
#endif
    mi->mode = this_mode;
#if CONFIG_NEW_REF_SIGNALING
    mi->ref_frame_nrs[0] = ref_frame_nrs;
    if (!use_ref_frame_mask[ref_frame_nrs]) continue;
#else
    mi->ref_frame[0] = ref_frame;
    if (!use_ref_frame_mask[ref_frame]) continue;
#endif  // CONFIG_NEW_REF_SIGNALING

#if CONFIG_NEW_REF_SIGNALING
    // Skip non-zero motion for SVC if skip_nonzeromv_ref is set.
    if (cpi->use_svc && frame_mv[this_mode][ref_frame_nrs].as_int != 0) {
      if (ref_frame_nrs == last_frame && cpi->svc.skip_nonzeromv_last)
        continue;
      else if (ref_frame_nrs == golden_frame && cpi->svc.skip_nonzeromv_gf)
        continue;
    }
#else
    // Skip non-zero motion for SVC if skip_nonzeromv_ref is set.
    if (cpi->use_svc && frame_mv[this_mode][ref_frame].as_int != 0) {
      if (ref_frame == LAST_FRAME && cpi->svc.skip_nonzeromv_last)
        continue;
      else if (ref_frame == GOLDEN_FRAME && cpi->svc.skip_nonzeromv_gf)
        continue;
    }
#endif  // CONFIG_NEW_REF_SIGNALING

#if CONFIG_NEW_REF_SIGNALING
    if (skip_mode_by_bsize_and_ref_frame(cm, this_mode, ref_frame_nrs, bsize,
                                         x->nonrd_prune_ref_frame_search,
                                         sse_zeromv_norm))
      continue;

    if (skip_mode_by_low_temp(
            this_mode, ref_frame_nrs, bsize, x->content_state_sb,
            frame_mv[this_mode][ref_frame_nrs], force_skip_low_temp_var))
      continue;
#else
    // If the segment reference frame feature is enabled then do nothing if the
    // current ref frame is not allowed.
    if (segfeature_active(seg, segment_id, SEG_LVL_REF_FRAME) &&
        get_segdata(seg, segment_id, SEG_LVL_REF_FRAME) != (int)ref_frame)
      continue;

    if (skip_mode_by_bsize_and_ref_frame(cm, this_mode, ref_frame, bsize,
                                         x->nonrd_prune_ref_frame_search,
                                         sse_zeromv_norm))
      continue;

    if (skip_mode_by_low_temp(this_mode, ref_frame, bsize, x->content_state_sb,
                              frame_mv[this_mode][ref_frame],
                              force_skip_low_temp_var))
      continue;
#endif  // CONFIG_NEW_REF_SIGNALING

      // Disable this drop out case if the ref frame segment level feature is
      // enabled for this segment. This is to prevent the possibility that we
      // end up unable to pick any mode.
#if CONFIG_NEW_REF_SIGNALING
    // Check for skipping GOLDEN and ALTREF based pred_mv_sad.
    if (cpi->sf.rt_sf.nonrd_prune_ref_frame_search > 0 &&
        x->pred_mv_sad[ref_frame_nrs] != INT_MAX &&
        ref_frame_nrs != last_frame) {
      if ((int64_t)(x->pred_mv_sad[ref_frame_nrs]) > thresh_sad_pred) continue;
    }

    if (skip_mode_by_threshold(
            this_mode, ref_frame_nrs, frame_mv[this_mode][ref_frame_nrs],
            cpi->rc.frames_since_golden, rd_threshes, rd_thresh_freq_fact,
            best_rdc.rdcost, best_pickmode.best_mode_skip_txfm))
      continue;

    // Select prediction reference frames.
    for (int i = 0; i < MAX_MB_PLANE; i++) {
      xd->plane[i].pre[0] = yv12_mb[ref_frame_nrs][i];
    }
#else
    if (!segfeature_active(seg, segment_id, SEG_LVL_REF_FRAME)) {
      // Check for skipping GOLDEN and ALTREF based pred_mv_sad.
      if (cpi->sf.rt_sf.nonrd_prune_ref_frame_search > 0 &&
          x->pred_mv_sad[ref_frame] != INT_MAX && ref_frame != LAST_FRAME) {
        if ((int64_t)(x->pred_mv_sad[ref_frame]) > thresh_sad_pred) continue;
      }
    }

    if (skip_mode_by_threshold(
            this_mode, ref_frame, frame_mv[this_mode][ref_frame],
            cpi->rc.frames_since_golden, rd_threshes, rd_thresh_freq_fact,
            best_rdc.rdcost, best_pickmode.best_mode_skip_txfm))
      continue;

    // Select prediction reference frames.
    for (int i = 0; i < MAX_MB_PLANE; i++) {
      xd->plane[i].pre[0] = yv12_mb[ref_frame][i];
    }
#endif  // CONFIG_NEW_REF_SIGNALING
#if CONFIG_NEW_REF_SIGNALING
    mi->ref_frame_nrs[0] = ref_frame_nrs;
    mi->ref_frame_nrs[1] = INVALID_IDX;
#else
    mi->ref_frame[0] = ref_frame;
    mi->ref_frame[1] = NONE_FRAME;
#endif  // CONFIG_NEW_REF_SIGNALING
#if CONFIG_NEW_REF_SIGNALING
    set_ref_ptrs_nrs(cm, xd, mi->ref_frame_nrs[0], INVALID_IDX);
#else
    set_ref_ptrs(cm, xd, mi->ref_frame[0], NONE_FRAME);
#endif  // CONFIG_NEW_REF_SIGNALING

    if (this_mode == NEWMV) {
      if (search_new_mv(cpi, x, frame_mv,
#if CONFIG_NEW_REF_SIGNALING
                        ref_frame_nrs,
#else
                        ref_frame,
#endif  // CONFIG_NEW_REF_SIGNALING
                        gf_temporal_ref, bsize, mi_row, mi_col, &rate_mv,
                        &best_rdc))
        continue;
    }

#if CONFIG_NEW_INTER_MODES
    for (PREDICTION_MODE inter_mv_mode = NEARMV; inter_mv_mode <= NEWMV;
#else
    for (PREDICTION_MODE inter_mv_mode = NEARESTMV; inter_mv_mode <= NEWMV;
#endif  // CONFIG_NEW_INTER_MODES
         inter_mv_mode++) {
      if (inter_mv_mode == this_mode) continue;
#if CONFIG_NEW_REF_SIGNALING
      if (mode_checked[inter_mv_mode][ref_frame_nrs] &&
          frame_mv[this_mode][ref_frame_nrs].as_int ==
              frame_mv[inter_mv_mode][ref_frame_nrs].as_int) {
#else
      if (mode_checked[inter_mv_mode][ref_frame] &&
          frame_mv[this_mode][ref_frame].as_int ==
              frame_mv[inter_mv_mode][ref_frame].as_int) {
#endif  // CONFIG_NEW_REF_SIGNALING
        skip_this_mv = 1;
        break;
      }
    }

    if (skip_this_mv) continue;

    mi->mode = this_mode;
#if CONFIG_NEW_REF_SIGNALING
    mi->mv[0].as_int = frame_mv[this_mode][ref_frame_nrs].as_int;
#else
    mi->mv[0].as_int = frame_mv[this_mode][ref_frame].as_int;
#endif  // CONFIG_NEW_REF_SIGNALING
    mi->mv[1].as_int = 0;
    if (reuse_inter_pred) {
      if (!this_mode_pred) {
        this_mode_pred = &tmp[3];
      } else {
        this_mode_pred = &tmp[get_pred_buffer(tmp, 3)];
        pd->dst.buf = this_mode_pred->data;
        pd->dst.stride = bw;
      }
    }
#if COLLECT_PICK_MODE_STAT
    ms_stat.num_nonskipped_searches[bsize][this_mode]++;
#endif
    if (enable_filter_search &&
        ((mi->mv[0].as_mv.row & 0x07) || (mi->mv[0].as_mv.col & 0x07)) &&
#if CONFIG_NEW_REF_SIGNALING
        (ref_frame_nrs == last_frame || !x->nonrd_prune_ref_frame_search)) {
#else
        (ref_frame == LAST_FRAME || !x->nonrd_prune_ref_frame_search)) {
#endif  // CONFIG_NEW_REF_SIGNALING
      search_filter_ref(cpi, x, &this_rdc, mi_row, mi_col, tmp, bsize,
                        reuse_inter_pred, &this_mode_pred, &this_early_term,
                        use_model_yrd_large);
    } else {
#if CONFIG_REMOVE_DUAL_FILTER
      mi->interp_fltr =
          (filter_ref == SWITCHABLE) ? default_interp_filter : filter_ref;
#else
      mi->interp_filters =
          (filter_ref == SWITCHABLE)
              ? av1_broadcast_interp_filter(default_interp_filter)
              : av1_broadcast_interp_filter(filter_ref);
#endif  // CONFIG_REMOVE_DUAL_FILTER
      av1_enc_build_inter_predictor_y(xd, mi_row, mi_col);
      if (use_model_yrd_large) {
        model_skip_for_sb_y_large(cpi, bsize, mi_row, mi_col, x, xd, &this_rdc,
                                  &this_early_term, use_modeled_non_rd_cost);
      } else {
        model_rd_for_sb_y(cpi, bsize, x, xd, &this_rdc,
                          use_modeled_non_rd_cost);
      }
    }

#if CONFIG_NEW_REF_SIGNALING
    if (ref_frame_nrs == last_frame &&
        frame_mv[this_mode][ref_frame_nrs].as_int == 0) {
#else
    if (ref_frame == LAST_FRAME && frame_mv[this_mode][ref_frame].as_int == 0) {
#endif  // CONFIG_NEW_REF_SIGNALING
      sse_zeromv_norm =
          (unsigned int)(this_rdc.sse >> (b_width_log2_lookup[bsize] +
                                          b_height_log2_lookup[bsize]));
    }

    const int skip_ctx = av1_get_skip_txfm_context(xd);
    const int skip_txfm_cost = mode_costs->skip_txfm_cost[skip_ctx][1];
    const int no_skip_txfm_cost = mode_costs->skip_txfm_cost[skip_ctx][0];
    if (this_early_term) {
      this_rdc.skip_txfm = 1;
      this_rdc.rate = skip_txfm_cost;
      this_rdc.dist = this_rdc.sse << 4;
    } else {
      if (use_modeled_non_rd_cost) {
        if (this_rdc.skip_txfm) {
          this_rdc.rate = skip_txfm_cost;
        } else {
          this_rdc.rate += no_skip_txfm_cost;
        }
      } else {
        block_yrd(cpi, x, mi_row, mi_col, &this_rdc, &is_skippable, bsize,
                  mi->tx_size);
        if (this_rdc.skip_txfm ||
            RDCOST(x->rdmult, this_rdc.rate, this_rdc.dist) >=
                RDCOST(x->rdmult, 0, this_rdc.sse)) {
          if (!this_rdc.skip_txfm) {
            // Need to store "real" rdc for possible furure use if UV rdc
            // disallows tx skip
            nonskip_rdc = this_rdc;
            nonskip_rdc.rate += no_skip_txfm_cost;
          }
          this_rdc.rate = skip_txfm_cost;
          this_rdc.skip_txfm = 1;
          this_rdc.dist = this_rdc.sse;
        } else {
          this_rdc.rate += no_skip_txfm_cost;
        }
      }
      if ((x->color_sensitivity[0] || x->color_sensitivity[1])) {
        RD_STATS rdc_uv;
        const BLOCK_SIZE uv_bsize = get_plane_block_size(
            bsize, xd->plane[1].subsampling_x, xd->plane[1].subsampling_y);
        if (x->color_sensitivity[0]) {
          av1_enc_build_inter_predictor(cm, xd, mi_row, mi_col, NULL, bsize,
                                        AOM_PLANE_U, AOM_PLANE_U);
        }
        if (x->color_sensitivity[1]) {
          av1_enc_build_inter_predictor(cm, xd, mi_row, mi_col, NULL, bsize,
                                        AOM_PLANE_V, AOM_PLANE_V);
        }
        model_rd_for_sb_uv(cpi, uv_bsize, x, xd, &rdc_uv, &this_rdc.sse, 1, 2);
        // Restore Y rdc if UV rdc disallows txfm skip
        if (this_rdc.skip_txfm && !rdc_uv.skip_txfm &&
            nonskip_rdc.rate != INT_MAX)
          this_rdc = nonskip_rdc;
        this_rdc.rate += rdc_uv.rate;
        this_rdc.dist += rdc_uv.dist;
        this_rdc.skip_txfm = this_rdc.skip_txfm && rdc_uv.skip_txfm;
      }
    }

    // TODO(kyslov) account for UV prediction cost
    this_rdc.rate += rate_mv;
    const int16_t mode_ctx =
#if CONFIG_NEW_REF_SIGNALING
        av1_mode_context_analyzer(mbmi_ext->mode_context, mi->ref_frame_nrs);
#else
        av1_mode_context_analyzer(mbmi_ext->mode_context, mi->ref_frame);
#endif  // CONFIG_NEW_REF_SIGNALING
    this_rdc.rate += cost_mv_ref(mode_costs, this_mode, mode_ctx);

#if CONFIG_NEW_REF_SIGNALING
    this_rdc.rate += ref_costs_single[ref_frame_nrs];
#else
    this_rdc.rate += ref_costs_single[ref_frame];
#endif  // CONFIG_NEW_REF_SIGNALING

    this_rdc.rdcost = RDCOST(x->rdmult, this_rdc.rate, this_rdc.dist);
    if (cpi->oxcf.rc_cfg.mode == AOM_CBR) {
      newmv_diff_bias(xd, this_mode, &this_rdc, bsize,
#if CONFIG_NEW_REF_SIGNALING
                      frame_mv[this_mode][ref_frame_nrs].as_mv.row,
                      frame_mv[this_mode][ref_frame_nrs].as_mv.col,
#else
                      frame_mv[this_mode][ref_frame].as_mv.row,
                      frame_mv[this_mode][ref_frame].as_mv.col,
#endif  // CONFIG_NEW_REF_SIGNALING
                      cpi->speed, x->source_variance);
    }

#if CONFIG_NEW_REF_SIGNALING
    mode_checked[this_mode][ref_frame_nrs] = 1;
#else
    mode_checked[this_mode][ref_frame] = 1;
#endif  // CONFIG_NEW_REF_SIGNALING
#if COLLECT_PICK_MODE_STAT
    aom_usec_timer_mark(&ms_stat.timer1);
    ms_stat.nonskipped_search_times[bsize][this_mode] +=
        aom_usec_timer_elapsed(&ms_stat.timer1);
#endif
    if (this_rdc.rdcost < best_rdc.rdcost) {
      best_rdc = this_rdc;
      best_early_term = this_early_term;
      best_pickmode.best_mode = this_mode;
#if CONFIG_REMOVE_DUAL_FILTER
      best_pickmode.best_pred_filter = mi->interp_fltr;
#else
      best_pickmode.best_pred_filter = mi->interp_filters;
#endif  // CONFIG_REMOVE_DUAL_FILTER
      best_pickmode.best_tx_size = mi->tx_size;
#if CONFIG_NEW_REF_SIGNALING
      best_pickmode.best_ref_frame = ref_frame_nrs;
#else
      best_pickmode.best_ref_frame = ref_frame;
#endif  // CONFIG_NEW_REF_SIGNALING
      best_pickmode.best_mode_skip_txfm = this_rdc.skip_txfm;
      best_pickmode.best_mode_initial_skip_flag =
          (nonskip_rdc.rate == INT_MAX && this_rdc.skip_txfm);

      if (reuse_inter_pred) {
        free_pred_buffer(best_pickmode.best_pred);
        best_pickmode.best_pred = this_mode_pred;
      }
    } else {
      if (reuse_inter_pred) free_pred_buffer(this_mode_pred);
    }
    if (best_early_term && idx > 0) {
      txfm_info->skip_txfm = 1;
      break;
    }
  }

  mi->mode = best_pickmode.best_mode;
#if CONFIG_REMOVE_DUAL_FILTER
  mi->interp_fltr = best_pickmode.best_pred_filter;
#else
  mi->interp_filters = best_pickmode.best_pred_filter;
#endif  // CONFIG_REMOVE_DUAL_FILTER
  mi->tx_size = best_pickmode.best_tx_size;
  memset(mi->inter_tx_size, mi->tx_size, sizeof(mi->inter_tx_size));
#if CONFIG_NEW_REF_SIGNALING
  mi->ref_frame_nrs[0] = best_pickmode.best_ref_frame;
#else
  mi->ref_frame[0] = best_pickmode.best_ref_frame;
#endif  // CONFIG_NEW_REF_SIGNALING
  mi->mv[0].as_int =
      frame_mv[best_pickmode.best_mode][best_pickmode.best_ref_frame].as_int;

  // Perform intra prediction search, if the best SAD is above a certain
  // threshold.
  mi->angle_delta[PLANE_TYPE_Y] = 0;
  mi->angle_delta[PLANE_TYPE_UV] = 0;
  mi->filter_intra_mode_info.use_filter_intra = 0;

#if CONFIG_NEW_REF_SIGNALING
  estimate_intra_mode(cpi, x, bsize, use_modeled_non_rd_cost, best_early_term,
                      ref_costs_single[INTRA_FRAME_INDEX_NRS], reuse_inter_pred,
                      &orig_dst, tmp, &this_mode_pred, &best_rdc,
                      &best_pickmode);
#else
  estimate_intra_mode(cpi, x, bsize, use_modeled_non_rd_cost, best_early_term,
                      ref_costs_single[INTRA_FRAME], reuse_inter_pred,
                      &orig_dst, tmp, &this_mode_pred, &best_rdc,
                      &best_pickmode);
#endif  // CONFIG_NEW_REF_SIGNALING

  pd->dst = orig_dst;
  mi->mode = best_pickmode.best_mode;
#if CONFIG_NEW_REF_SIGNALING
  mi->ref_frame_nrs[0] = best_pickmode.best_ref_frame;
#else
  mi->ref_frame[0] = best_pickmode.best_ref_frame;
#endif  // CONFIG_NEW_REF_SIGNALING
  txfm_info->skip_txfm = best_rdc.skip_txfm;

  if (!is_inter_block(mi)) {
#if CONFIG_REMOVE_DUAL_FILTER
    mi->interp_fltr = SWITCHABLE_FILTERS;
#else
    mi->interp_filters = av1_broadcast_interp_filter(SWITCHABLE_FILTERS);
#endif  // CONFIG_REMOVE_DUAL_FILTER
  }

  if (reuse_inter_pred && best_pickmode.best_pred != NULL) {
    PRED_BUFFER *const best_pred = best_pickmode.best_pred;
    if (best_pred->data != orig_dst.buf && is_inter_mode(mi->mode)) {
      aom_convolve_copy(best_pred->data, best_pred->stride, pd->dst.buf,
                        pd->dst.stride, bw, bh);
    }
  }
  if (cpi->sf.inter_sf.adaptive_rd_thresh) {
#if CONFIG_NEW_REF_SIGNALING
    if (best_pickmode.best_ref_frame == INTRA_FRAME_NRS) {
#else
    if (best_pickmode.best_ref_frame == INTRA_FRAME) {
#endif  // CONFIG_NEW_REF_SIGNALING
      // Only consider the modes that are included in the intra_mode_list.
      int intra_modes = sizeof(intra_mode_list) / sizeof(PREDICTION_MODE);
      for (int i = 0; i < intra_modes; i++) {
        update_thresh_freq_fact(cpi, x, bsize,
#if CONFIG_NEW_REF_SIGNALING
                                INTRA_FRAME_NRS,
#else
                                INTRA_FRAME,
#endif  // CONFIG_NEW_REF_SIGNALING
                                intra_mode_list[i],
                                best_pickmode.best_ref_frame, mi->mode);
      }
    } else {
      PREDICTION_MODE this_mode;
#if CONFIG_NEW_INTER_MODES
      for (this_mode = NEARMV; this_mode <= NEWMV; ++this_mode) {
#else
      for (this_mode = NEARESTMV; this_mode <= NEWMV; ++this_mode) {
#endif  // CONFIG_NEW_INTER_MODES
        update_thresh_freq_fact(cpi, x, bsize, best_pickmode.best_ref_frame,
                                this_mode, best_pickmode.best_ref_frame,
                                mi->mode);
      }
    }
  }

#if CONFIG_INTERNAL_STATS && !CONFIG_NEW_REF_SIGNALING
  store_coding_context(x, ctx, mi->mode);
#else
  store_coding_context(x, ctx);
#endif  // CONFIG_INTERNAL_STATS && !CONFIG_NEW_REF_SIGNALING
#if COLLECT_PICK_MODE_STAT
  aom_usec_timer_mark(&ms_stat.timer2);
  ms_stat.avg_block_times[bsize] += aom_usec_timer_elapsed(&ms_stat.timer2);
  //
  if ((mi_row + mi_size_high[bsize] >= (cpi->common.mi_params.mi_rows)) &&
      (mi_col + mi_size_wide[bsize] >= (cpi->common.mi_params.mi_cols))) {
    int i, j;
    PREDICTION_MODE used_modes[3] = { NEARESTMV, NEARMV, NEWMV };
    BLOCK_SIZE bss[5] = { BLOCK_8X8, BLOCK_16X16, BLOCK_32X32, BLOCK_64X64,
                          BLOCK_128X128 };
    int64_t total_time = 0l;
    int32_t total_blocks = 0;

    printf("\n");
    for (i = 0; i < 5; i++) {
      printf("BS(%d) Num %d, Avg_time %f: ", bss[i], ms_stat.num_blocks[bss[i]],
             ms_stat.num_blocks[bss[i]] > 0
                 ? (float)ms_stat.avg_block_times[bss[i]] /
                       ms_stat.num_blocks[bss[i]]
                 : 0);
      total_time += ms_stat.avg_block_times[bss[i]];
      total_blocks += ms_stat.num_blocks[bss[i]];
      for (j = 0; j < 3; j++) {
        printf("Mode %d, %d/%d tps %f ", used_modes[j],
               ms_stat.num_nonskipped_searches[bss[i]][used_modes[j]],
               ms_stat.num_searches[bss[i]][used_modes[j]],
               ms_stat.num_nonskipped_searches[bss[i]][used_modes[j]] > 0
                   ? (float)ms_stat
                             .nonskipped_search_times[bss[i]][used_modes[j]] /
                         ms_stat.num_nonskipped_searches[bss[i]][used_modes[j]]
                   : 0l);
      }
      printf("\n");
    }
    printf("Total time = %ld. Total blocks = %d\n", total_time, total_blocks);
  }
  //
#endif  // COLLECT_PICK_MODE_STAT
  *rd_cost = best_rdc;
}
