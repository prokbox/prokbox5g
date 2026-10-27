/***************************************************************************
 *              __________                   __    __________
 *   Open       \______   \_______    ____  |  | __\______   \  ____  ___  ___
 *   Source      |     ___/\_  __ \  /  _ \ |  |/ / |    |  _/ /  _ \ \  \/  /
 *   Player      |    |     |  | \/ (  <_> )|    <  |    |   \(  <_> ) >    <
 *   Firmware    |____|     |__|     \____/ |__|_ \ |______  / \____/ /__/\_ \
 *                                               \/        \/               \/
 *
 * Custom firmware for the iPod Video, 5th generation (5G and 5.5G).
 * Not affiliated with or endorsed by Apple Inc.
 *
 * H.264 decoder: sequence and picture parameter sets (7.3.2.1, 7.3.2.2).
 * Anything outside our subset (frame-only 4:2:0 8-bit CAVLC, no FMO,
 * no weighted prediction, no 8x8 transform or scaling matrices) is
 * refused with H264_E_UNSUPPORTED.
 *
 * Copyright (C) 2026 prokopto-dev
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#include "h264_int.h"

static int more_rbsp_data(const struct h264_dec *d, const struct h264_bits *b)
{
    return bs_tell(b) < d->rbsp_bits;
}

H264_COLD int h264_parse_sps(struct h264_dec *d, struct h264_bits *b)
{
    struct h264_sps s;
    uint32_t id, v, w, h;
    int k, n;

    memset(&s, 0, sizeof(s));
    s.profile_idc = bs_get(b, 8);
    bs_get(b, 8);                        /* constraint flags, reserved */
    s.level_idc = bs_get(b, 8);
    id = bs_ue(b);
    if (id >= H264_MAX_SPS)
        return H264_ERR(2, H264_E_BITSTREAM);
    switch (s.profile_idc)
    {
    case 100: case 110: case 122: case 244: case 44:
    case 83: case 86: case 118: case 128: case 138: case 139:
    case 134: case 135:
        if (bs_ue(b) != 1)               /* chroma_format_idc */
            return H264_REFUSE(2);
        if (bs_ue(b) || bs_ue(b))        /* bit depths */
            return H264_REFUSE(2);
        bs_get1(b);                      /* lossless qpprime_y_zero */
        if (bs_get1(b))                  /* seq_scaling_matrix_present */
            return H264_REFUSE(2);
        break;
    }
    v = bs_ue(b);
    if (v > 12)
        return H264_ERR(2, H264_E_BITSTREAM);
    s.log2_max_frame_num = v + 4;
    v = bs_ue(b);
    if (v > 2)
        return H264_ERR(2, H264_E_BITSTREAM);
    s.poc_type = v;
    if (v == 0)
    {
        v = bs_ue(b);
        if (v > 12)
            return H264_ERR(2, H264_E_BITSTREAM);
        s.log2_max_poc_lsb = v + 4;
    }
    else if (v == 1)
    {
        s.delta_pic_order_always_zero = bs_get1(b);
        bs_se(b);                        /* offset_for_non_ref_pic */
        bs_se(b);                        /* offset_for_top_to_bottom_field */
        n = bs_ue(b);
        if (n > 255)
            return H264_ERR(2, H264_E_BITSTREAM);
        for (k = 0; k < n && !bs_overrun(b); k++)
            bs_se(b);                    /* offset_for_ref_frame */
    }
    v = bs_ue(b);
    if (v > H264_MAX_REF_FRAMES)
        return H264_REFUSE(2);
    s.num_ref_frames = v;
    s.gaps_allowed = bs_get1(b);
    w = bs_ue(b) + 1;
    h = bs_ue(b) + 1;
    if (!bs_get1(b))                     /* frame_mbs_only_flag */
        return H264_REFUSE(2);       /* interlaced */
    if (w > H264_MAX_MB_W || h > H264_MAX_MB_H || !w || !h)
        return H264_REFUSE(2);
    s.mb_w = w;
    s.mb_h = h;
    bs_get1(b);                          /* direct_8x8_inference_flag */
    if (bs_get1(b))                      /* frame_cropping_flag */
    {
        uint32_t l = bs_ue(b), r = bs_ue(b), t = bs_ue(b), bo = bs_ue(b);
        /* 4:2:0 frame: CropUnitX = CropUnitY = 2 */
        if ((l + r) * 2 >= w * 16 || (t + bo) * 2 >= h * 16)
            return H264_ERR(2, H264_E_BITSTREAM);
        s.crop_l = l * 2;
        s.crop_r = r * 2;
        s.crop_t = t * 2;
        s.crop_b = bo * 2;
    }
    /* vui_parameters: nothing in them changes decoding for us */
    if (bs_overrun(b) || b->err)
        return H264_ERR(2, H264_E_BITSTREAM);
    s.valid = 1;
    /* An SPS in use may only change at an IDR picture: the new one is
     * kept and activated there (slice.c). */
    d->sps[id] = s;
    return H264_OK;
}

H264_COLD int h264_parse_pps(struct h264_dec *d, struct h264_bits *b)
{
    struct h264_pps p;
    uint32_t id, v;
    int32_t sv;

    memset(&p, 0, sizeof(p));
    id = bs_ue(b);
    if (id >= H264_MAX_PPS)
        return H264_REFUSE(2);
    v = bs_ue(b);
    if (v >= H264_MAX_SPS)
        return H264_ERR(2, H264_E_BITSTREAM);
    p.sps_id = v;
    if (bs_get1(b))                      /* entropy_coding_mode_flag */
        return H264_REFUSE(2);       /* CABAC */
    p.bottom_field_pic_order_present = bs_get1(b);
    if (bs_ue(b))                        /* num_slice_groups_minus1 */
        return H264_REFUSE(2);       /* FMO */
    v = bs_ue(b);
    if (v > 31)
        return H264_ERR(2, H264_E_BITSTREAM);
    p.num_ref_idx_default = v + 1;
    if (bs_ue(b) > 31)                   /* num_ref_idx_l1_default */
        return H264_ERR(2, H264_E_BITSTREAM);
    if (bs_get1(b))                      /* weighted_pred_flag */
        return H264_REFUSE(2);
    if (bs_get(b, 2))                    /* weighted_bipred_idc */
        return H264_REFUSE(2);
    sv = bs_se(b);
    if (sv < -26 || sv > 25)
        return H264_ERR(2, H264_E_BITSTREAM);
    p.init_qp = 26 + sv;
    sv = bs_se(b);                       /* pic_init_qs: SP/SI only */
    if (sv < -26 || sv > 25)
        return H264_ERR(2, H264_E_BITSTREAM);
    sv = bs_se(b);
    if (sv < -12 || sv > 12)
        return H264_ERR(2, H264_E_BITSTREAM);
    p.chroma_qp_offset = sv;
    p.deblocking_filter_control_present = bs_get1(b);
    p.constrained_intra_pred = bs_get1(b);
    p.redundant_pic_cnt_present = bs_get1(b);
    if (more_rbsp_data(d, b))
    {
        if (bs_get1(b))                  /* transform_8x8_mode_flag */
            return H264_REFUSE(2);
        if (bs_get1(b))                  /* pic_scaling_matrix_present */
            return H264_REFUSE(2);
        if (bs_se(b) != p.chroma_qp_offset)
            return H264_REFUSE(2);   /* second_chroma_qp_index_offset */
    }
    if (bs_overrun(b) || b->err)
        return H264_ERR(2, H264_E_BITSTREAM);
    p.valid = 1;
    d->pps[id] = p;
    return H264_OK;
}
