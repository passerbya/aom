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

#ifndef AV1_COMMON_CFL_H_
#define AV1_COMMON_CFL_H_

#include "av1/common/blockd.h"

typedef void (*cfl_subsample_lbd_fn)(const uint8_t *input, int input_stride,
                                     int16_t *output_q3, int width, int height);

typedef void (*cfl_subsample_hbd_fn)(const uint16_t *input, int input_stride,
                                     int16_t *output_q3, int width, int height);

typedef void (*cfl_predict_lbd_fn)(const int16_t *pred_buf_q3, uint8_t *dst,
                                   int dst_stride, TX_SIZE tx_size,
                                   int alpha_q3);

typedef void (*cfl_predict_hbd_fn)(const int16_t *pred_buf_q3, uint16_t *dst,
                                   int dst_stride, TX_SIZE tx_size,
                                   int alpha_q3, int bd);

static INLINE CFL_ALLOWED_TYPE is_cfl_allowed(const MB_MODE_INFO *mbmi) {
  const BLOCK_SIZE bsize = mbmi->sb_type;
  assert(bsize < BLOCK_SIZES_ALL);
#if CONFIG_EXT_PARTITION_TYPES && CONFIG_RECT_TX_EXT_INTRA
  return (CFL_ALLOWED_TYPE)(block_size_wide[bsize] <= 32 &&
                            block_size_high[bsize] <= 32);
#else
  return (CFL_ALLOWED_TYPE)(bsize <= CFL_MAX_BLOCK_SIZE);
#endif  // CONFIG_EXT_PARTITION_TYPES && CONFIG_RECT_TX_EXT_INTRA
}

static INLINE int get_scaled_luma_q0(int alpha_q3, int16_t pred_buf_q3) {
  int scaled_luma_q6 = alpha_q3 * pred_buf_q3;
  return ROUND_POWER_OF_TWO_SIGNED(scaled_luma_q6, 6);
}

static INLINE CFL_PRED_TYPE get_cfl_pred_type(PLANE_TYPE plane) {
  assert(plane > 0);
  return (CFL_PRED_TYPE)(plane - 1);
}

void cfl_predict_block(MACROBLOCKD *const xd, uint8_t *dst, int dst_stride,
                       TX_SIZE tx_size, int plane);

void cfl_store_block(MACROBLOCKD *const xd, BLOCK_SIZE bsize, TX_SIZE tx_size);

void cfl_store_tx(MACROBLOCKD *const xd, int row, int col, TX_SIZE tx_size,
                  BLOCK_SIZE bsize);

void cfl_store_dc_pred(MACROBLOCKD *const xd, const uint8_t *input,
                       CFL_PRED_TYPE pred_plane, int width);

void cfl_load_dc_pred(MACROBLOCKD *const xd, uint8_t *dst, int dst_stride,
                      TX_SIZE tx_size, CFL_PRED_TYPE pred_plane);

// TODO(ltrudeau) Remove this when HBD 420 SIMD is added
void cfl_luma_subsampling_420_hbd_c(const uint16_t *input, int input_stride,
                                    int16_t *output_q3, int width, int height);

// TODO(ltrudeau) Remove this when HBD 422 SIMD is added
void cfl_luma_subsampling_422_hbd_c(const uint16_t *input, int input_stride,
                                    int16_t *output_q3, int width, int height);

// TODO(ltrudeau) Remove this when HBD 444 SIMD is added
void cfl_luma_subsampling_444_hbd_c(const uint16_t *input, int input_stride,
                                    int16_t *output_q3, int width, int height);

// TODO(ltrudeau) Remove this when LBD 422 SIMD is added
void cfl_luma_subsampling_422_lbd_c(const uint8_t *input, int input_stride,
                                    int16_t *output_q3, int width, int height);
// TODO(ltrudeau) Remove this when LBD 444 SIMD is added
void cfl_luma_subsampling_444_lbd_c(const uint8_t *input, int input_stride,
                                    int16_t *output_q3, int width, int height);

#define CFL_GET_SUBSAMPLE_FUNCTION(arch)                                   \
  cfl_subsample_lbd_fn get_subsample_lbd_fn_##arch(int sub_x, int sub_y) { \
    if (sub_x == 1)                                                        \
      return (sub_y == 1) ? cfl_luma_subsampling_420_lbd_##arch            \
                          : cfl_luma_subsampling_422_lbd_c;                \
    return cfl_luma_subsampling_444_lbd_c;                                 \
  }                                                                        \
  cfl_subsample_hbd_fn get_subsample_hbd_fn_##arch(int sub_x, int sub_y) { \
    if (sub_x == 1)                                                        \
      return (sub_y == 1) ? cfl_luma_subsampling_420_hbd_c                 \
                          : cfl_luma_subsampling_422_hbd_c;                \
    return cfl_luma_subsampling_444_hbd_c;                                 \
  }

#endif  // AV1_COMMON_CFL_H_
