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
 * H.264 decoder: slice header (7.3.3), picture start and finish, the
 * decoded picture buffer for Baseline: reference list initialisation and
 * modification for P slices (8.2.4), sliding-window and the short-term
 * memory_management_control_operations (8.2.5).  Long-term references and
 * frame_num gaps are refused cleanly.
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

#define MAX_MODS (H264_MAX_REFLIST + 1)

struct refmod {
    int n;
    uint8_t idc[MAX_MODS];
    uint32_t val[MAX_MODS];
};

static H264_NOINLINE int parse_header(struct h264_dec *d, struct h264_bits *b,
                        struct h264_slice_hdr *sh, struct refmod *mods)
{
    const struct h264_pps *pps;
    const struct h264_sps *sps;
    uint32_t v;
    int32_t sv;

    sh->first_mb = bs_ue(b);
    v = bs_ue(b);
    if (v > 9)
        return H264_ERR(3, H264_E_BITSTREAM);
    v %= 5;
    if (v != 0 && v != 2)
        return H264_REFUSE(3);       /* B, SP, SI */
    sh->type = v;
    v = bs_ue(b);
    if (v >= H264_MAX_PPS || !d->pps[v].valid)
        return H264_ERR(3, H264_E_STATE);
    sh->pps_id = v;
    pps = &d->pps[v];
    sps = &d->sps[pps->sps_id];
    if (!sps->valid)
        return H264_ERR(3, H264_E_STATE);
    if (sh->first_mb >= sps->mb_w * sps->mb_h)
        return H264_ERR(3, H264_E_BITSTREAM);
    sh->frame_num = bs_get(b, sps->log2_max_frame_num);
    if (sh->idr)
    {
        sh->idr_pic_id = bs_ue(b);
        if (sh->frame_num)
            return H264_ERR(3, H264_E_BITSTREAM);
    }
    sh->poc_lsb = 0;
    if (sps->poc_type == 0)
    {
        sh->poc_lsb = bs_get(b, sps->log2_max_poc_lsb);
        if (pps->bottom_field_pic_order_present)
            bs_se(b);
    }
    else if (sps->poc_type == 1 && !sps->delta_pic_order_always_zero)
    {
        bs_se(b);
        if (pps->bottom_field_pic_order_present)
            bs_se(b);
    }
    if (pps->redundant_pic_cnt_present && bs_ue(b))
        return 1;                        /* a redundant slice: ignored */
    sh->num_ref_idx_active = pps->num_ref_idx_default;
    mods->n = 0;
    if (sh->type == 0)
    {
        if (bs_get1(b))                  /* num_ref_idx_active_override */
        {
            v = bs_ue(b);
            if (v > 31)
                return H264_ERR(3, H264_E_BITSTREAM);
            sh->num_ref_idx_active = v + 1;
        }
        if (bs_get1(b))                  /* ref_pic_list_modification_flag */
        {
            for (;;)
            {
                v = bs_ue(b);
                if (v == 3)
                    break;
                if (v > 2 || mods->n >= MAX_MODS || bs_overrun(b))
                    return H264_ERR(3, H264_E_BITSTREAM);
                if (v == 2)
                    return H264_REFUSE(3);   /* long-term */
                mods->idc[mods->n] = v;
                mods->val[mods->n] = bs_ue(b);
                mods->n++;
            }
        }
    }
    sh->adaptive_marking = 0;
    sh->nmmco = 0;
    sh->long_term_reference = 0;
    if (sh->nal_ref_idc)
    {
        if (sh->idr)
        {
            bs_get1(b);                  /* no_output_of_prior_pics_flag */
            if (bs_get1(b))
                return H264_REFUSE(3);   /* long_term_reference_flag */
        }
        else if ((sh->adaptive_marking = bs_get1(b)))
        {
            for (;;)
            {
                struct h264_mmco *op;
                v = bs_ue(b);
                if (v == 0)
                    break;
                if (v > 6 || sh->nmmco >= H264_MAX_MMCO || bs_overrun(b))
                    return H264_ERR(3, H264_E_BITSTREAM);
                if (v != 1 && v != 5)
                    return H264_REFUSE(3);   /* long-term operations */
                op = &sh->mmco[sh->nmmco++];
                op->op = v;
                op->arg = v == 1 ? bs_ue(b) : 0;
            }
        }
    }
    sv = bs_se(b);
    sv += pps->init_qp;
    if (sv < 0 || sv > 51)
        return H264_ERR(3, H264_E_BITSTREAM);
    sh->qp = sv;
    sh->dbk_idc = 0;
    sh->alpha_off = sh->beta_off = 0;
    if (pps->deblocking_filter_control_present)
    {
        v = bs_ue(b);
        if (v > 2)
            return H264_ERR(3, H264_E_BITSTREAM);
        sh->dbk_idc = v;
        if (v != 1)
        {
            sv = bs_se(b);
            if (sv < -6 || sv > 6)
                return H264_ERR(3, H264_E_BITSTREAM);
            sh->alpha_off = sv * 2;
            sv = bs_se(b);
            if (sv < -6 || sv > 6)
                return H264_ERR(3, H264_E_BITSTREAM);
            sh->beta_off = sv * 2;
        }
    }
    if (bs_overrun(b) || b->err)
        return H264_ERR(3, H264_E_BITSTREAM);
    return 0;
}

/* 8.2.4.1: FrameNumWrap and PicNum of the short-term references */
static void number_refs(struct h264_dec *d, int frame_num)
{
    int k;
    for (k = 0; k < d->nframes; k++)
    {
        struct h264_frame *f = &d->frames[k];
        if (f->ref != REF_SHORT || f == d->cur)
            continue;
        f->frame_num_wrap = f->frame_num > frame_num
                          ? f->frame_num - d->max_frame_num : f->frame_num;
        f->pic_num = f->frame_num_wrap;
    }
}

/* 8.2.4.2.1 and 8.2.4.3: RefPicList0 for a P slice */
static H264_NOINLINE int build_reflist(struct h264_dec *d, const struct refmod *mods)
{
    struct h264_frame *list[H264_MAX_FRAMES];
    int n = 0, k, j, size = d->sh.num_ref_idx_active;
    int curr_pic_num = d->sh.frame_num, pred = curr_pic_num;

    number_refs(d, d->sh.frame_num);
    for (k = 0; k < d->nframes; k++)
    {
        struct h264_frame *f = &d->frames[k];
        if (f->ref != REF_SHORT || f == d->cur)
            continue;
        for (j = n; j > 0 && list[j - 1]->pic_num < f->pic_num; j--)
            list[j] = list[j - 1];       /* descending PicNum */
        list[j] = f;
        n++;
    }
    if (n == 0)
        return H264_ERR(3, H264_E_STATE);
    for (k = 0; k < H264_MAX_REFLIST; k++)
        d->reflist[k] = k < n ? list[k] : NULL;

    /* modifications (short-term only) */
    for (k = 0; k < mods->n; k++)
    {
        int diff = (int)mods->val[k] + 1, pic_num, idx, out;
        struct h264_frame *f = NULL;
        if ((uint32_t)diff > (uint32_t)d->max_frame_num || k >= size)
            return H264_ERR(3, H264_E_BITSTREAM);
        if (mods->idc[k] == 0)
        {
            pred -= diff;
            if (pred < 0)
                pred += d->max_frame_num;
        }
        else
        {
            pred += diff;
            if (pred >= d->max_frame_num)
                pred -= d->max_frame_num;
        }
        pic_num = pred > curr_pic_num ? pred - d->max_frame_num : pred;
        for (j = 0; j < n; j++)
            if (list[j]->pic_num == pic_num)
                f = list[j];
        if (!f)
            return H264_ERR(3, H264_E_STATE);
        /* insert at k, then drop the later duplicate */
        for (idx = size; idx > k; idx--)
            d->reflist[idx] = d->reflist[idx - 1];
        d->reflist[k] = f;
        for (idx = out = k + 1; idx <= size; idx++)
            if (d->reflist[idx] != f)
                d->reflist[out++] = d->reflist[idx];
        for (; out <= size && out < H264_MAX_REFLIST; out++)
            d->reflist[out] = NULL;
    }
    for (k = size; k < H264_MAX_REFLIST; k++)
        d->reflist[k] = NULL;
    d->nref = size;
    return 0;
}

static H264_NOINLINE int start_picture(struct h264_dec *d)
{
    const struct h264_pps *pps = &d->pps[d->sh.pps_id];
    const struct h264_sps *sps = &d->sps[pps->sps_id];
    int k, r;

    if (d->sh.idr)
    {
        r = h264_layout(d, sps);
        if (r)
            return r;
        d->active_sps = sps;
        for (k = 0; k < d->nframes; k++)
            d->frames[k].ref = REF_NONE;
        d->prev_ref_frame_num = 0;
    }
    else
    {
        if (!d->active_sps || sps != d->active_sps || !d->nframes ||
            sps->mb_w != d->mb_w || sps->mb_h != d->mb_h ||
            sps->num_ref_frames != d->layout_refs)
            return H264_ERR(3, H264_E_STATE);         /* no IDR yet, or an SPS change */
        if (d->sh.frame_num != d->prev_ref_frame_num &&
            d->sh.frame_num != (d->prev_ref_frame_num + 1) % d->max_frame_num)
            return H264_REFUSE(3);   /* gaps in frame_num */
    }
    d->max_frame_num = 1 << sps->log2_max_frame_num;
    d->cur = NULL;
    for (k = 0; k < d->nframes; k++)
        if (d->frames[k].ref == REF_NONE)
        {
            d->cur = &d->frames[k];
            break;
        }
    if (!d->cur)
        return H264_ERR(3, H264_E_STATE);             /* DPB full: bad marking */
    d->cur->frame_num = d->sh.frame_num;
    d->cur->ref = REF_NONE;
    for (k = 0; k < d->mbs; k++)
        d->mbi[k].slice = 0xffff;
    d->first_sh = d->sh;
    d->mbs_done = 0;
    d->slice_num = 0;
    d->in_picture = 1;
    return 0;
}

/* Fill the macroblocks no slice covered: copy the most recent reference,
 * or mid-grey.  They are left out of the loop filter. */
static H264_NOINLINE int conceal(struct h264_dec *d)
{
    const struct h264_frame *src = NULL;
    int k, n = 0, best = -(1 << 30), row;
    for (k = 0; k < d->nframes; k++)
    {
        struct h264_frame *f = &d->frames[k];
        if (f->ref == REF_SHORT && f != d->cur && f->frame_num_wrap > best)
        {
            best = f->frame_num_wrap;
            src = f;
        }
    }
    for (k = 0; k < d->mbs; k++)
    {
        struct h264_mbinfo *mi = &d->mbi[k];
        int x = k % d->mb_w, y = k / d->mb_w, w = d->ls;    /* stride */
        if (mi->slice != 0xffff)
            continue;
        n++;
        for (row = 0; row < 16; row++)
        {
            int o = (y * 16 + row) * w + x * 16;
            if (src)
                memcpy(d->cur->y + o, src->y + o, 16);
            else
                memset(d->cur->y + o, 128, 16);
        }
        for (row = 0; row < 8; row++)
        {
            int o = (y * 8 + row) * (w / 2) + x * 8;
            if (src)
            {
                memcpy(d->cur->u + o, src->u + o, 8);
                memcpy(d->cur->v + o, src->v + o, 8);
            }
            else
            {
                memset(d->cur->u + o, 128, 8);
                memset(d->cur->v + o, 128, 8);
            }
        }
        memset(mi, 0, sizeof(*mi));
        mi->flags = MBF_INTRA;
        mi->dbk_idc = 1;
        memset(mi->ref, -1, 4);
        memset(mi->refpic, 0xff, 4);
        memset(d->mvs[k], 0, sizeof(d->mvs[k]));
    }
    return n;
}

/* 8.2.5: reference marking of the finished picture */
static H264_NOINLINE void mark(struct h264_dec *d)
{
    const struct h264_slice_hdr *sh = &d->first_sh;
    struct h264_frame *cur = d->cur;
    int k, j, mmco5 = 0;

    if (!sh->nal_ref_idc)
        return;
    number_refs(d, cur->frame_num);
    if (sh->idr)
        ;                                /* refs were cleared at the start */
    else if (sh->adaptive_marking)
    {
        for (j = 0; j < sh->nmmco; j++)
        {
            const struct h264_mmco *op = &sh->mmco[j];
            if (op->op == 1)
            {
                int pic_num = cur->frame_num - (int)(op->arg + 1);
                for (k = 0; k < d->nframes; k++)
                    if (d->frames[k].ref == REF_SHORT && &d->frames[k] != cur
                        && d->frames[k].pic_num == pic_num)
                        d->frames[k].ref = REF_NONE;
            }
            else if (op->op == 5)
            {
                for (k = 0; k < d->nframes; k++)
                    if (&d->frames[k] != cur)
                        d->frames[k].ref = REF_NONE;
                mmco5 = 1;
            }
        }
    }
    else
    {
        /* sliding window (8.2.5.3) */
        int max = d->active_sps->num_ref_frames;
        if (max < 1)
            max = 1;
        for (;;)
        {
            struct h264_frame *old = NULL;
            int n = 0;
            for (k = 0; k < d->nframes; k++)
            {
                struct h264_frame *f = &d->frames[k];
                if (f->ref != REF_SHORT || f == cur)
                    continue;
                n++;
                if (!old || f->frame_num_wrap < old->frame_num_wrap)
                    old = f;
            }
            if (n < max)
                break;
            old->ref = REF_NONE;
        }
    }
    /* the DPB holds at most num_ref_frames (+ the current picture):
     * a stream whose MMCOs keep more is cut back the same way */
    for (;;)
    {
        struct h264_frame *old = NULL;
        int n = 0;
        for (k = 0; k < d->nframes; k++)
        {
            struct h264_frame *f = &d->frames[k];
            if (f->ref != REF_SHORT || f == cur)
                continue;
            n++;
            if (!old || f->frame_num_wrap < old->frame_num_wrap)
                old = f;
        }
        if (n < d->nframes - 1)
            break;
        old->ref = REF_NONE;
    }
    cur->ref = REF_SHORT;
    if (mmco5)
        cur->frame_num = 0;
    d->prev_ref_frame_num = cur->frame_num;
}

int h264_finish_picture(struct h264_dec *d)
{
    const struct h264_sps *sps = d->active_sps;
    struct h264_frame *f = d->cur;
    int n = 0, w = d->ls;                    /* the luma stride */

    if (!d->in_picture)
        return H264_OK;
    d->in_picture = 0;
    if (d->mbs_done < d->mbs)
        n = conceal(d);
    h264_deblock_picture(d);
    mark(d);
    d->out.stride_y = w;
    d->out.stride_c = w / 2;
    d->out.y = f->y + sps->crop_t * w + sps->crop_l;
    d->out.u = f->u + (sps->crop_t / 2) * (w / 2) + sps->crop_l / 2;
    d->out.v = f->v + (sps->crop_t / 2) * (w / 2) + sps->crop_l / 2;
    d->out.width = d->mb_w * 16 - sps->crop_l - sps->crop_r;
    d->out.height = d->mb_h * 16 - sps->crop_t - sps->crop_b;
    d->out.frame_num = d->first_sh.frame_num;
    d->out.idr = d->first_sh.idr;
    d->out.concealed = n;
    d->stats.concealed += n;
    d->stats.pictures++;
    return H264_FRAME;
}

int h264_decode_slice(struct h264_dec *d, struct h264_bits *b, int nal_type,
                      int nal_ref_idc)
{
    struct h264_slice_hdr sh;
    struct refmod mods;
    int r;

    sh.idr = nal_type == 5;
    sh.nal_ref_idc = nal_ref_idc;
    if (sh.idr && !nal_ref_idc)
        return H264_ERR(3, H264_E_BITSTREAM);
    r = parse_header(d, b, &sh, &mods);
    if (r == 1)
        return H264_OK;                  /* redundant slice */
    if (r < 0)
        return r;

    /* first slice of a new picture? (7.4.1.2.4, the parts that matter
     * without fields) */
    if (d->in_picture &&
        (sh.first_mb == 0 || sh.frame_num != d->first_sh.frame_num ||
         sh.idr != d->first_sh.idr ||
         (sh.idr && sh.idr_pic_id != d->first_sh.idr_pic_id) ||
         sh.poc_lsb != d->first_sh.poc_lsb ||
         !sh.nal_ref_idc != !d->first_sh.nal_ref_idc))
        return h264_finish_picture(d) == H264_FRAME
               ? H264_FRAME_AGAIN : H264_OK;
    d->sh = sh;
    if (!d->in_picture)
    {
        r = start_picture(d);
        if (r < 0)
            return r;
    }
    else if (++d->slice_num >= 0xffff)
        return H264_ERR(3, H264_E_BITSTREAM);
    d->pps_cur = &d->pps[sh.pps_id];
    if (d->pps_cur->sps_id != (int)(d->active_sps - d->sps))
        return H264_ERR(3, H264_E_STATE);
    if (sh.type == 0)
    {
        r = build_reflist(d, &mods);
        if (r < 0)
            return r;
    }
    else
        d->nref = 0;
    d->stats.slices++;
    r = h264_decode_slice_data(d, b);
    if (d->mbs_done >= d->mbs)
    {
        int f = h264_finish_picture(d);
        if (r < 0)
            d->stats.errors++;           /* counted; the picture is out */
        return f;
    }
    return r < 0 ? r : H264_OK;
}
