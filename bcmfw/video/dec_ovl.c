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

/* DECODE as an overlay for the merged image (MERGED IMAGE, next re-verify
 * batch; docs/research/bcm2722/album-render.md section 5.3, the panel's
 * end state).  The merged image keeps jref's DECODE op (JREF_HOOKS implies
 * JREF_DECODE) with its hook unset, so jref answers HP_E_UNKNOWN until
 * this overlay's init sets the hook; the loader drops the hook again at
 * every load and epoch (jobs_bcm.c ovl_drop).  What the display-only image
 * has resident (jobs_bcm.c's DECODE section) is here, with two
 * differences the merged image's memory asks for:
 *  - the work area is a slot the PP lends: a J_CLS_DRIFT slot of this
 *    epoch keyed J_DEC_WORK_KEY (the drift's own cover's place; the PP
 *    uploads that cover again after), 182,400 B at 300 x 300.  The merged
 *    image has no fixed area in UI mode (memory-contract.md section 2);
 *  - the merged image has no decode_prepare() (no room), so jref checks
 *    len against the ring's bytes as for any slot: the PP sends len
 *    clipped to the ring and the file's own length in J_DEC_LEN.
 * Linked at the region's base against the image's map; its one call into
 * the image is bcmfw_service() (build.py DEC_OVL_IMPORTS). */
#include <stdint.h>
#include "config.h"
#include "hw.h"
#include "bcmfw.h"
#include "hostproto.h"
#include "jref.h"
#include "jdec.h"
#include "jdec_ring.h"
#include "jdec_vpu.h"

extern char __vc2_abi[], __data_end[], __bss_start[], __bss_end[];
static uint32_t init(struct jref *j);
static uint32_t exec(struct jref *j, uint32_t op, const uint32_t *a, uint32_t n,
                     uint32_t *rep, uint32_t *nr);

/* the header, first at the base (.vectors): build.py checks its words */
const struct jref_ovl dec_ovl __attribute__((section(".vectors"))) =
{
    J_OVL_MAGIC, (uint32_t)__vc2_abi, (uint32_t)&dec_ovl, (uint32_t)__data_end,
    (uint32_t)__bss_end, init, exec
};

#define JBW(off)            MB(BCMFW_JOBS_BLOCK + (off))
#define DECODE_SERVICE_US   2000

static struct jref *jr;
static struct jdec_ring ring;

static uint32_t now_us(void)
{
    return REG(TIMER_CLO);
}

/* S3c: the heartbeat, the LCD command word and the running animation
 * between MCU rows, never the ring (as jobs_bcm.c's decode_yield) */
static void dec_yield(void)
{
    static uint32_t last;
    uint32_t t = now_us();

    if (t - last < DECODE_SERVICE_US)
        return;
    last = t;
    bcmfw_service();
}

/* The slot of this epoch whose pixels start at px, or none */
static const struct jref_slot *slot_at(const void *px)
{
    uint32_t k;

    for (k = 0; k < JREF_SLOTS; k++)
        if (jr->slot[k].epoch == jr->epoch &&
            jr->slot[k].state != J_ST_EMPTY && jr->slot[k].surf.px == px)
            return &jr->slot[k];
    return 0;
}

/* The work slot the PP lent this epoch, or none */
static const struct jref_slot *work_slot(void)
{
    uint32_t k;

    for (k = 0; k < JREF_SLOTS; k++)
    {
        const struct jref_slot *s = &jr->slot[k];

        if (s->epoch == jr->epoch && s->cls == J_CLS_DRIFT &&
            s->key_lo == J_DEC_WORK_KEY && s->key_hi == 0 &&
            (s->state == J_ST_ALLOCATED || s->state == J_ST_READY))
            return s;
    }
    return 0;
}

/* jref's decode hook: the source is the ring (J_DEC_LEN, the file's
 * length) or a slot holding the whole file (J_DEC_LEN 0) */
static int decode(const uint8_t *jpg, uint32_t len, const struct jdec_out *out,
                  int n, struct jdec_info *info)
{
    const struct jref_slot *w = work_slot(), *src = slot_at(jpg);
    uint32_t flen = JBW(J_DEC_LEN);
    struct jdec_src s;
    struct jdec_mem m;

    if (!w)
        return JDEC_E_NOMEM;            /* nothing lent: HP_E_NOMEM */
    if (flen)
    {
        if (!src || src->cls != J_CLS_SCRATCH || !jdec_ring_ok(src->bytes))
            return JDEC_E_RANGE;
        ring.buf = (uint8_t *)jpg;
        ring.bytes = src->bytes;
        jdec_ring_src(&s, &ring, flen);
    }
    else
        jdec_mem_src(&s, &m, jpg, len);
    return jdec_run(&s, out, n, w->surf.px, w->bytes, info);
}

/* Once a load is checked: the .bss cleared, the ring's words, the vector
 * unit's checks (J_VPU_MAC, J_VPU_YUV), the hook; DECODE offered */
static uint32_t init(struct jref *j)
{
    uint32_t *p;

    for (p = (uint32_t *)__bss_start; p < (uint32_t *)__bss_end; p++)
        *p = 0;
    jr = j;
    ring.rd_word = &JBW(J_DEC_IN_RD);
    ring.wr_word = &JBW(J_DEC_IN_WR);
    ring.now_us = now_us;
    JBW(J_DEC_IN_WR) = HP_ENC(0);
    JBW(J_DEC_IN_RD) = HP_ENC(0);
    jdec_yield = dec_yield;
    JBW(J_VPU) |= jdec_vpu_init(BCMFW_JDEC_VPU_IDCT, BCMFW_JOBS_MAC_FAIL) *
                  J_VPU_MAC;
    JBW(J_VPU) |= jdec_vpu_yuv() * J_VPU_YUV;
    j->decode = decode;
    /* the PP's cue: lend a work slot, write J_DEC_LEN (jobs.h) */
    JBW(J_CAPS) = J_CAPS_MAGIC | J_CAP_DEC_LENT |
                  (J_CAPS_VALID(JBW(J_CAPS)) ? JBW(J_CAPS) & 0xFFFF : 0);
    return 1u << (J_OP_DECODE - 0x20);
}

/* DECODE is jref's own op (the hook is this overlay's): nothing else
 * comes here */
static uint32_t exec(struct jref *j, uint32_t op, const uint32_t *a, uint32_t n,
                     uint32_t *rep, uint32_t *nr)
{
    (void)j; (void)op; (void)a; (void)n; (void)rep; (void)nr;
    return HP_E_UNKNOWN;
}
