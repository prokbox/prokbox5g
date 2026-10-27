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

/* Host test of the BCM2722 job driver (firmware/target/arm/ipod/video/
 * bcmjob-core.c, the device's code) against a chip made of the reference
 * executor (bcmfw/jobs/jref.c) behind a byte-addressed port: the rings,
 * the header and the job block where the image keeps them (hostproto.h,
 * jobs.h), a heartbeat, power, death and restarts.
 *
 * Cases (P19 brief W0-6): a ring round-trip (the words equal jring_put's,
 * the panel equals the executor's), slots with engine 2 and F16w, a cut
 * mid-SCROLL and a death mid-SLIDE resynced to the PP's frame, a full
 * ring answering BUSY at once with the CPU frame after, a dead chip
 * restarted then jobs off after the second failure, a slow job marked on
 * its second miss, and an hour dark with 0 power-ups and no port access.
 *
 * DECODE (P19 19-6, bcmjob_decode() and bcmjob_read(); jmtest's cases on
 * the wire): the chip runs a ring DECODE as the display-only image does,
 * jdec over jdec_ring.c's reader (bcmfw/jobs), on a thread of its own
 * that runs only while the PP thread waits (a baton: each look the
 * reader takes at the ring is one step of the PP's), so the driver's
 * feed meets the BCM's real ring protocol.  The pixels read back equal
 * jdec_run() over the same file and outputs in PP memory (the CPU twin).
 *
 * The visualizer (#126): the chip is built as the display-only image is
 * (jref.h JREF_EXT): it offers OVERLAY when told to, and its jref_ext
 * below checks an upload as jobs_bcm.c's stub does (the length, the F16w,
 * the header's ABI word and load end) before it offers SPECTRUM and BARS
 * (jref_viz.c over jviz.c, the C).  t_overlay drives bjc_ovl_* as
 * bcmjob-dev.c's bcmjob_ovl_put() does: the load, every refusal, the
 * re-upload after each kind of epoch change, and SPECTRUM's reply.
 * Prints FAIL lines, then "bcmjob: N cases, M failed". */

#include <pthread.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hostproto.h"
#include "jref.h"
#include "bcmjob-core.h"
#include "jdec_ring.h"
#include "jstage.h"
#include "jviz.h"
#include "bcmjob_pp.h"      /* the line ops (bcmjob-viz.c) */

/* firmware/export/bcmjob_pp.h's (not on this test's include path) */
bool bcmjob_bars_flip(void);

static int cases, failed;
#define CHECK(c, ...) do { cases++; if (!(c)) { failed++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
    printf("\n"); } } while (0)

#define W 320
#define H 240
#define MEM_BYTES 0x200000
#define FRONT 0xE0000
#define BACK  (FRONT + W * H * 2)
#define BLOCK 0xDE000
#define POOL  0x130000
#define POOL_BYTES 0x10000
#define SCRATCH 0x140000        /* J_CLS_SCRATCH: the ring's one slot */
#define RING_POOL 0x8000
#define THUMBS  0x148000        /* J_CLS_THUMB: DECODE's boxes */
#define THUMBS_BYTES 0x10000
/* the staged upload (jobs rev 0.5): a J_CLS_STRIP pool large enough for a
 * cover, and the window (the BCM's SDRAM; here plain memory past SRAM) */
#define STRIPS  0x160000
#define STRIPS_BYTES 0x40000
#define STAGE   0x1A0000
#define STAGE_BYTES 0x30000

/* ---- the chip ------------------------------------------------------------ */
static uint8_t mem[MEM_BYTES];
static uint16_t panel[W * H], ppfb[W * H];
static struct jref chip;
static bool powered, dead, stalled, lit_, charging_;
static uint32_t gen_no, now_us_, slow_until, restarts, power_calls;
static uint32_t port_ops, e2_bytes, anim[1 + HP_MAX_PAYLOAD], anim_t0, anim_next;
static bool anim_on;
static uint32_t vpu_word = J_VPU_CHECKED | J_VPU_SHIFT;    /* the merged image's */
static uint32_t cmd_words = HP_CMD_WORDS;   /* the command ring's size */
static bool stage_offer;                    /* J_CAPS: J_CAP_STAGED */
static uint32_t stage_owner = JSTAGE_NONE, stage_moves, caps_garbage;

/* ---- the chip's DECODE (the display-only image's, bcmfw/video/jobs_bcm.c)
 * dec_offer: the image has it (J_OPS bit 16); chip_mhz: J_POWER's clock;
 * dec_hang: a DECODE that starts and never ends (the heartbeat live).
 * The decode itself runs on dj.t, handed the baton by the PP thread at
 * each chip_run() and handing it back at each look at the ring. */
static bool dec_offer, dec_hang;
static uint32_t chip_mhz = 150, ring_allocs, dec_row_us;
static uint32_t dec_delay_us, dec_start_at;     /* a DECODE starts this late */
static uint32_t commit_hold_us, commit_at;      /* a COMMIT takes this long */
static uint32_t hold_op, hold_us, hold_at;      /* any op, once */
static uint32_t hold2_op, hold2_us, hold2_at;
static uint32_t thumb_allocs, hit_frees;
static bool hit_given[JREF_SLOTS];              /* handed out as a hit */
#define DROP_TAG 0x777                          /* the shell's own FREEs */
static uint8_t *dwork;                  /* the image's work area */
static bool dec_lent;                   /* the merged overlay lends its work area */
static uint32_t dwork_bytes = J_DEC_WORK_MIN;
static struct jdec_ring dring;
static uint32_t dring_len;              /* a ring DECODE's file bytes */
static struct
{
    pthread_t t;
    bool on, thr, end, kill;
    uint32_t rec[1 + HP_MAX_PAYLOAD], rep[1 + HP_MAX_PAYLOAD], nrep, st;
} dj;
static pthread_mutex_t bmx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t bcv = PTHREAD_COND_INITIALIZER;
static int baton;                       /* 0: the PP's turn, 1: the decode's */

static void baton_to(int who, int me)
{
    pthread_mutex_lock(&bmx);
    baton = who;
    pthread_cond_broadcast(&bcv);
    while (baton != me)
        pthread_cond_wait(&bcv, &bmx);
    pthread_mutex_unlock(&bmx);
}

/* The ring reader's clock: every look at the ring is a step for the PP */
static uint32_t ring_now(void)
{
    baton_to(0, 1);
    if (dj.kill)
        dring.err = 1;          /* the BCM cut: the decode ends at once */
    return now_us_;
}

/* jref's hook, as jobs_bcm.c's decode(): the ring, or a memory slot */
static int chip_decode(const uint8_t *jpg, uint32_t len, const struct jdec_out *out,
                       int n, struct jdec_info *info)
{
    struct jdec_src src;
    struct jdec_mem m;

    if (dring_len)
    {
        dring.buf = (uint8_t *)jpg;
        jdec_ring_src(&src, &dring, dring_len);
    }
    else
        jdec_mem_src(&src, &m, jpg, len);
    if (dec_lent)
    {
        /* dec_ovl.c's work_slot(): the PP's lent slot of this epoch */
        uint32_t k;

        for (k = 0; k < JREF_SLOTS; k++)
        {
            const struct jref_slot *s = &chip.slot[k];

            if (s->epoch == chip.epoch && s->cls == J_CLS_DRIFT &&
                s->key_lo == J_DEC_WORK_KEY && s->key_hi == 0 &&
                (s->state == J_ST_ALLOCATED || s->state == J_ST_READY))
                return jdec_run(&src, out, n, (uint8_t *)s->surf.px, s->bytes, info);
        }
        return JDEC_E_NOMEM;
    }
    return jdec_run(&src, out, n, dwork, dwork_bytes, info);
}

/* jdec's yield (once per MCU row and output row, S3c): with dec_row_us
 * set, each row takes that long on the BCM's clock while the PP runs */
static void chip_yield(void)
{
    uint32_t t = now_us_;

    while (dec_row_us && !dj.kill && now_us_ - t < dec_row_us)
        baton_to(0, 1);
}

static void *dec_main(void *x)
{
    (void)x;
    pthread_mutex_lock(&bmx);
    while (baton != 1)
        pthread_cond_wait(&bcv, &bmx);
    pthread_mutex_unlock(&bmx);
    dj.st = jref_exec(&chip, dj.rec, dj.rep, &dj.nrep);
    pthread_mutex_lock(&bmx);
    dj.end = true;
    baton = 0;
    pthread_cond_broadcast(&bcv);
    pthread_mutex_unlock(&bmx);
    return NULL;
}

static uint32_t *w32(uint32_t a) { return (uint32_t *)(mem + a); }

static void evt_put(uint32_t op, uint32_t tag, const uint32_t *p, uint32_t n)
{
    uint32_t wr = *w32(HP_EVT_WR) & 0xFFFF, i;

    *w32(HP_EVT_RING + 4 * (wr & (HP_EVT_WORDS - 1))) = HP_REC(op, n, tag);
    for (i = 0; i < n; i++)
        *w32(HP_EVT_RING + 4 * ((wr + 1 + i) & (HP_EVT_WORDS - 1))) = p[i];
    *w32(HP_EVT_WR) = HP_ENC((wr + 1 + n) & 0xFFFF);
}

static void publish(void)
{
    *w32(BLOCK + J_EPOCH) = HP_ENC(chip.epoch);
    *w32(BLOCK + J_FENCE) = HP_ENC(chip.fence);
    *w32(BLOCK + J_DONE) = chip.done;
    *w32(BLOCK + J_ERRORS) = chip.errors;
}

static void dec_kill(void);

/* ---- the chip's overlay (the display-only image's stub: jobs_bcm.c) ------
 * ovl_offer: the image offers OVERLAY (J_OPS bit 21, J_OVL); stub_abi: its
 * __vc2_abi (what the stub compares the header's word with); image_abi:
 * the word the PP reads at OVL_ABI_AT (the stub's immediate in the image:
 * the same unless a test makes them differ) */
#define OVL_BASE    0x1D0000
#define OVL_SIZE    0x5000
/* the merged image's region (DECODE's overlay; album-render.md 5.3) and
 * its lent-work-slot pool (J_CLS_DRIFT: 2 x 300 x 300 at J_DRIFT_PITCH) */
#define DEC_BASE    0x1C0000
#define LENT_POOL   0x20000
#define OVL_ABI     0xDD3562B0u
#define OVL_ABI_AT  0x2A06
#define OVL_LINES   (1u << (J_OP_POLYLINE - 0x20) | 1u << (J_OP_SEGMENTS - 0x20) | \
                     1u << (J_OP_PALROW - 0x20))
#define OVL_BITS    (1u << (J_OP_SPECTRUM - 0x20) | 1u << (J_OP_BARS - 0x20) | OVL_LINES)
static bool ovl_offer, ovl_loaded;
static bool ovl_noflip;           /* the overlay before J_BARS_FLIP */
static uint32_t ovl_nolines;      /* line ops whose start-up check failed (bits) */
static uint32_t stub_abi = OVL_ABI, image_abi = OVL_ABI, ovl_cmds;
/* ovl_dec: the image is the merged one, its overlay DECODE's (at
 * DEC_BASE): a load sets jref's decode hook, J_OPS bit 16 and J_CAPS
 * J_CAP_DEC_LENT; its DECODE has no decode_prepare(), so the record's len
 * is the ring's at most and the file's length is J_DEC_LEN; its work area
 * is the J_CLS_DRIFT slot keyed J_DEC_WORK_KEY the PP lends (dec_ovl.c) */
static bool ovl_dec;
/* ovl_as_viz: the merged image's next load is the visualizer's
 * (viz-ovl-m.bin, the same region: visualizer.md 8.5); ovl_viz_in: the
 * region holds it now */
static bool ovl_as_viz, ovl_viz_in;
static uint32_t ovl_base = OVL_BASE, lent_len_seen, lent_rec_len;
static int chip_decode(const uint8_t *jpg, uint32_t len, const struct jdec_out *out,
                       int n, struct jdec_info *info);
#define DEC_BIT     (1u << (J_OP_DECODE - 0x20))
static void dec_ovl_drop(void)
{
    if (!ovl_dec)
        return;
    dec_lent = false;
    chip.decode = NULL;
    *(uint32_t *)(mem + BLOCK + J_OPS) &= ~DEC_BIT;
    *(uint32_t *)(mem + BLOCK + J_CAPS) &= ~J_CAP_DEC_LENT;
}

uint32_t jref_ext(struct jref *j, uint32_t op, const uint32_t *a, uint32_t n,
                  uint32_t *rep, uint32_t *nr)
{
    uint32_t *hdr = (uint32_t *)(mem + ovl_base), s[2];
    struct jref_surf img = { (uint16_t *)(mem + ovl_base), 1, 0, 1 };

    if (op != J_OP_OVERLAY || !ovl_offer)
    {
        if (ovl_dec && !ovl_viz_in)
            return HP_E_UNKNOWN;        /* DECODE is jref's own op */
        if (!ovl_loaded)
            return HP_E_UNKNOWN;
        if (op == J_OP_SPECTRUM)
            return n < J_SPEC_ARGS(1) ? HP_E_INVAL : jref_spectrum(j, a, n, rep, nr, 0);
        if (op == J_OP_BARS)
            return n < J_BARS_ARGS(1) ? HP_E_INVAL : jref_bars(j, a, n, 0);
        if (op == J_OP_POLYLINE && !(ovl_nolines & 1))
            return n < J_POLY_ARGS ? HP_E_INVAL : jref_polyline(j, a, n);
        if (op == J_OP_SEGMENTS && !(ovl_nolines & 2))
            return n < J_SEGS_ARGS(1) ? HP_E_INVAL : jref_segments(j, a, n);
        if (op == J_OP_PALROW && !(ovl_nolines & 4))
            return n < J_PAL_ARGS(1) ? HP_E_INVAL : jref_palrow(j, a, n);
        return HP_E_UNKNOWN;
    }
    ovl_cmds++;
    ovl_loaded = false;                 /* every load drops the ops first */
    ovl_viz_in = false;
    *(uint32_t *)(mem + BLOCK + J_OPS) &= ~OVL_BITS;
    dec_ovl_drop();
    if (n < 3 || a[0] - 7 > OVL_SIZE / 4 - 7)
        return HP_E_INVAL;
    img.h = 2 * a[0];
    jref_f16w(&img, s, s + 1);
    if (s[0] != a[1] || s[1] != a[2])
        return J_E_SUM;
    if (hdr[1] != stub_abi || hdr[3] != ovl_base + 4 * a[0])
        return HP_E_VERSION;
    ovl_loaded = true;
    if (ovl_dec && !ovl_as_viz)
    {
        /* dec_ovl.c's init: the hook, DECODE, the cue to lend */
        dec_lent = true;
        j->decode = chip_decode;
        *(uint32_t *)(mem + BLOCK + J_OPS) |= DEC_BIT;
        *(uint32_t *)(mem + BLOCK + J_CAPS) = J_CAPS_MAGIC | J_CAP_DEC_LENT;
        return HP_OK;
    }
    ovl_viz_in = true;
    /* a line op whose start-up check failed (ovl_nolines) is not offered */
    *(uint32_t *)(mem + BLOCK + J_OPS) |= OVL_BITS &
        ~((uint32_t)ovl_nolines << (J_OP_POLYLINE - 0x20));
    /* an overlay before J_BARS_FLIP (ovl_noflip) leaves its bit clear */
    *(uint32_t *)(mem + BLOCK + J_VPU) &= ~J_VPU_FLIP;
    *(uint32_t *)(mem + BLOCK + J_VPU) |= J_VPU_FFT | J_VPU_BARS |
                                          (ovl_noflip ? 0 : J_VPU_FLIP);
    return HP_OK;
}

/* a cold start of our image: everything lost, the PP's frame first */
static void chip_start(void)
{
    struct jref_surf f = { (uint16_t *)(mem + FRONT), W, H, W };
    struct jref_surf bk = { (uint16_t *)(mem + BACK), W, H, W };
    struct jref_surf p = { panel, W, H, W };

    dec_kill();
    memset(mem, 0, 0x2000);
    memset(mem + HP_CMD_RING, 0, 0x10000);
    jref_init(&chip, f, bk, p);
    jref_pool(&chip, J_CLS_DRIFT, mem + POOL, POOL_BYTES);
    jref_pool(&chip, J_CLS_SCRATCH, mem + SCRATCH, RING_POOL);
    jref_pool(&chip, J_CLS_THUMB, mem + THUMBS, THUMBS_BYTES);
    jref_pool(&chip, J_CLS_STRIP, mem + STRIPS, STRIPS_BYTES);
    stage_owner = JSTAGE_NONE;
    /* an image without the word leaves whatever was there: never the magic */
    *w32(BLOCK + J_CAPS) = stage_offer ? J_CAPS_MAGIC | J_CAP_STAGED : caps_garbage;
    chip.decode = dec_offer ? chip_decode : NULL;
    dring.rd_word = w32(BLOCK + J_DEC_IN_RD);
    dring.wr_word = w32(BLOCK + J_DEC_IN_WR);
    dring.now_us = ring_now;
    dring_len = 0;
    *w32(BLOCK + J_DEC_IN_RD) = HP_ENC(0);
    *w32(BLOCK + J_DEC_IN_WR) = HP_ENC(0);
    *w32(BLOCK + J_CUR_OP) = HP_ENC(0);
    *w32(BLOCK + J_POWER) = chip_mhz << 16 | J_PWR_IDLE_SLEEP;
    *w32(HP_CAPS) = HP_CAP_NULL | HP_CAP_JOBS_2D;
    *w32(HP_CMD_BASE) = HP_CMD_RING;
    *w32(HP_CMD_SIZE) = cmd_words;
    *w32(HP_EVT_BASE) = HP_EVT_RING;
    *w32(HP_EVT_SIZE) = HP_EVT_WORDS;
    *w32(HP_CMD_WR) = *w32(HP_CMD_RD) = HP_ENC(0);
    *w32(HP_EVT_WR) = *w32(HP_EVT_RD) = HP_ENC(0);
    *w32(HP_JOBS) = BLOCK;
    *w32(BLOCK + J_OPS) = 0x0000BFFF |  /* 0x20-0x2D, 0x2F */
                          (dec_offer ? 1u << (J_OP_DECODE - 0x20) : 0);
    *w32(BLOCK + J_VPU) = vpu_word | (dec_offer ? J_VPU_MAC : 0);
    ovl_loaded = false;
    dec_lent = false;
    if (ovl_offer)
    {
        *w32(BLOCK + J_OPS) |= 1u << (J_OP_OVERLAY - 0x20);
        *w32(BLOCK + J_OVL) = ovl_base | OVL_SIZE >> 12;
        memcpy(mem + OVL_ABI_AT, &image_abi, 4);
    }
    if (ovl_dec)
        jref_pool(&chip, J_CLS_DRIFT, mem + LENT_POOL, J_BUDGET_DRIFT);
    *w32(0x1F8) = 0x50420001;
    *w32(HP_VERSION) = HP_VERSION_WORD;
    *w32(BLOCK + J_VERSION) = J_VERSION_WORD;
    memcpy(mem + FRONT, ppfb, sizeof(ppfb));
    memcpy(panel, ppfb, sizeof(ppfb));
    publish();
    anim_on = false;
    dead = stalled = false;
    powered = true;
    gen_no++;
}

/* The cut or a restart: a running decode is ended (its reader errs) */
static void dec_kill(void)
{
    if (!dj.on)
        return;
    if (dj.thr)
    {
        dj.kill = true;
        while (!dj.end)
            baton_to(1, 0);
        pthread_join(dj.t, NULL);
    }
    dj.on = dj.thr = dj.end = dj.kill = false;
    dring_len = 0;
}

/* The decode's reply, as the image's loop posts a job's */
static void dec_finish(void)
{
    pthread_join(dj.t, NULL);
    dj.on = dj.thr = false;
    dring_len = 0;
    if (dj.nrep)
        evt_put(J_OP_DECODE | HP_REPLY, HP_REC_TAG(dj.rec[0]), dj.rep, dj.nrep);
    *w32(BLOCK + J_CUR_OP) = HP_ENC(0);
    publish();
}

/* A ring DECODE record (jobs_bcm.c's decode_prepare()): the ring must be
 * a READY scratch slot of a power of two; jref then sees the slot alone
 * and the ring's bytes.  true: it runs on dj.t */
static bool dec_start(uint32_t *rec)
{
    uint32_t slot = rec[1] & 0xFF;

    dring_len = 0;
    if (HP_REC_LEN(rec[0]) < 2 || !(rec[1] & J_DEC_RING) || !chip.decode)
        return false;
    if (slot >= JREF_SLOTS || !jref_slot_px(&chip, slot) ||
        !jdec_ring_ok(chip.slot[slot].bytes) || chip.slot[slot].cls != J_CLS_SCRATCH ||
        rec[2] < 4)
    {
        rec[1] = 0xEF;
        return false;
    }
    dring.bytes = chip.slot[slot].bytes;
    dring_len = rec[2];
    if (dec_lent)
    {
        /* the PP clipped len to the ring; the file's length is J_DEC_LEN */
        lent_rec_len = rec[2];
        lent_len_seen = dring_len = *w32(BLOCK + J_DEC_LEN);
        if (rec[2] > dring.bytes)
        {
            rec[1] = 0xEF;
            dring_len = 0;
            return false;
        }
    }
    rec[1] = slot;
    if (rec[2] > dring.bytes)
        rec[2] = dring.bytes;
    memcpy(dj.rec, rec, sizeof(dj.rec));
    dj.on = dj.thr = true;
    dj.end = dj.kill = false;
    *w32(BLOCK + J_CUR_OP) = HP_ENC(J_OP_DECODE);
    if (pthread_create(&dj.t, NULL, dec_main, NULL))
        abort();
    return true;
}

/* the image's loop: commands, then the animation's frame */
static void chip_run(void)
{
    uint32_t rec[1 + HP_MAX_PAYLOAD], rep[1 + HP_MAX_PAYLOAD], nrep, st, i;

    if (!powered || dead)
        return;
    *w32(HP_STATE) = ((now_us_ >> 10) & 0xFF) << 8 | HP_ST_IDLE;
    if (dj.on)
    {
        /* a running DECODE holds the loop (S3c: only the heartbeat and
           the LCD contract are served meanwhile, never the ring) */
        if (!dj.thr)
        {
            if (dec_hang)
                return;
            /* a hang let go: the job fails as a stalled ring does */
            dj.on = false;
            rep[0] = J_E_DEC_CORRUPT;
            evt_put(J_OP_DECODE | HP_REPLY, HP_REC_TAG(dj.rec[0]), rep, 1);
            *w32(BLOCK + J_CUR_OP) = HP_ENC(0);
            chip.done++;
            chip.errors++;
            publish();
        }
        else
        {
            baton_to(1, 0);
            if (!dj.end)
                return;
            dec_finish();
        }
    }
    while (!stalled && now_us_ >= slow_until)
    {
        uint32_t rd = *w32(HP_CMD_RD) & 0xFFFF, wr = *w32(HP_CMD_WR) & 0xFFFF, n, op;

        if (rd == wr)
            break;
        rec[0] = *w32(HP_CMD_RING + 4 * (rd & (cmd_words - 1)));
        n = HP_REC_LEN(rec[0]);
        for (i = 0; i < n; i++)
            rec[1 + i] = *w32(HP_CMD_RING + 4 * ((rd + 1 + i) & (cmd_words - 1)));
        *w32(HP_CMD_RD) = HP_ENC((rd + 1 + n) & 0xFFFF);
        op = HP_REC_OP(rec[0]);
        if (op == hold2_op && hold2_us)
        {
            if (!hold2_at)
                hold2_at = now_us_ + hold2_us;
            if (now_us_ < hold2_at)
            {
                *w32(HP_CMD_RD) = HP_ENC(rd);
                return;
            }
            hold2_at = hold2_us = 0;
        }
        if (op == hold_op && hold_us)
        {
            /* this op takes hold_us on the BCM (once) */
            if (!hold_at)
                hold_at = now_us_ + hold_us;
            if (now_us_ < hold_at)
            {
                *w32(HP_CMD_RD) = HP_ENC(rd);
                return;
            }
            hold_at = hold_us = 0;
        }
        if (op == J_OP_FREE && rec[1] < JREF_SLOTS && hit_given[rec[1]] &&
            HP_REC_TAG(rec[0]) != DROP_TAG)
            hit_frees++;        /* the driver freed another holder's slot */
        if (op == J_OP_FREE && rec[1] < JREF_SLOTS)
            hit_given[rec[1]] = false;
        if (op == J_OP_COMMIT && commit_hold_us)
        {
            /* COMMIT reads the whole slot for its F16w: up to ~94 ms */
            if (!commit_at)
                commit_at = now_us_ + commit_hold_us;
            if (now_us_ < commit_at)
            {
                *w32(HP_CMD_RD) = HP_ENC(rd);
                return;
            }
            commit_at = 0;
        }
        if (op == J_OP_DECODE && dec_delay_us)
        {
            /* the BCM still busy with earlier work: the record waits */
            if (!dec_start_at)
                dec_start_at = now_us_ + dec_delay_us;
            if (now_us_ < dec_start_at)
            {
                *w32(HP_CMD_RD) = HP_ENC(rd);
                return;
            }
            dec_start_at = 0;
        }
        if (op == J_OP_DECODE && dec_hang && chip.decode)
        {
            memcpy(dj.rec, rec, sizeof(dj.rec));
            dj.on = true;
            *w32(BLOCK + J_CUR_OP) = HP_ENC(J_OP_DECODE);
            return;
        }
        if (op == J_OP_DECODE && dec_start(rec))
        {
            baton_to(1, 0);
            if (!dj.end)
                return;
            dec_finish();
            continue;
        }
        if (op == HP_OP_HELLO)
        {
            rep[0] = HP_OK;
            rep[1] = HP_VERSION_WORD;
            rep[2] = *w32(HP_CAPS);
            evt_put(op | HP_REPLY, HP_REC_TAG(rec[0]), rep, 3);
            continue;
        }
        if (stage_offer && op == J_OP_COMMIT && n)
        {
            /* jobs_bcm.c's stage_before: the window into the slot */
            uint32_t bytes;
            uint16_t *px = jstage_take(&chip, &stage_owner, rec[1], &bytes);

            if (px)
            {
                memcpy(px, mem + STAGE, bytes);
                stage_moves++;
            }
            st = jref_exec(&chip, rec, rep, &nrep);
            jstage_reply(&chip, rec[1], st, px ? J_HOW_DMA : J_HOW_DIRECT,
                         (const uint16_t *)(mem + STAGE), rep, &nrep);
        }
        else
            st = jref_exec(&chip, rec, rep, &nrep);
        if (st == HP_OK && op == J_OP_ALLOC)
            rep[2] = (uint32_t)((uint8_t *)jref_slot_px(&chip, rep[1]) - mem);
        if (stage_offer && op == J_OP_ALLOC &&
            jstage_wanted(&chip, rec + 1, n, st, rep, nrep, STAGE_BYTES))
            jstage_give(&stage_owner, rep, STAGE);
        if ((op == J_OP_FREE && n && rec[1] == stage_owner) || op == J_OP_RESET)
            stage_owner = JSTAGE_NONE;
        if (op == J_OP_RESET)
            dec_ovl_drop();     /* jobs_bcm.c ovl_drop at the epoch */
        if (st == HP_OK && op == J_OP_ALLOC && rec[1] == J_CLS_SCRATCH)
            ring_allocs++;
        if (st == HP_OK && op == J_OP_ALLOC && rec[1] != J_CLS_SCRATCH)
        {
            thumb_allocs++;
            if (rep[1] < JREF_SLOTS)
                hit_given[rep[1]] = (rep[4] & J_ALLOC_HIT) != 0;
        }
        if (st == HP_OK && op == J_OP_ANIM)
        {
            memcpy(anim, rec, sizeof(anim));
            anim_on = true;
            anim_t0 = anim_next = now_us_;
        }
        if (nrep)
            evt_put(op | HP_REPLY, HP_REC_TAG(rec[0]), rep, nrep);
        publish();
    }
    if (anim_on && now_us_ >= anim_next)
    {
        /* as the BCM's jobs_tick (jobs rev 0.4): the frame into the front
         * frame, then its rect to the panel, the next at its interval */
        const uint32_t *a = anim + 1;
        uint32_t next = jref_anim_frame(&chip, anim, (now_us_ - anim_t0) / 1000);
        uint32_t xy = a[0] == J_ANIM_SLIDE ? a[3] : a[5], r, c;

        for (r = J_Y(xy); r < J_Y(xy) + J_Y(a[4]); r++)
            for (c = J_X(xy); c < J_X(xy) + J_X(a[4]); c++)
                chip.panel.px[r * chip.panel.pitch + c] = chip.front.px[r * chip.front.pitch + c];
        if (!next)
            anim_on = false;
        anim_next += next;
    }
}

/* ---- the port ------------------------------------------------------------ */
/* S10's two-core model (t_cross_core, below): its state */
#define XPX_BYTES (142 * 160 * 2 + 64)
static uint8_t piece_mem[BCMJOB_DEC_PIECE] __attribute__((aligned(16)));
static uint8_t piece_ram[BCMJOB_DEC_PIECE] __attribute__((aligned(16)));
static uint8_t pxv[XPX_BYTES] __attribute__((aligned(16)));    /* the view */
static uint8_t pxr[XPX_BYTES] __attribute__((aligned(16)));    /* memory */
static bool px_dirty[XPX_BYTES / 16];
static struct bjc_req xq;
static pthread_mutex_t xmx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t xcv = PTHREAD_COND_INITIALIZER;
static pthread_t x_caller;
static struct
{
    bool on, model;
    bool drop_publish, drop_acquire;    /* the negative controls */
    uint32_t rnd, one_in, jump_in, jump_us;
    int turn;                   /* 0 the owner's, 1 the caller's */
    bool done;                  /* the caller's scenario returned */
    bool stall_arm, stalled;    /* the owner frozen inside a pass */
    uint32_t owner_off;         /* passes the owner skips first */
    bool piece_live;
    /* counters */
    uint32_t points, switches, publishes, acquires, aliases, unmodeled;
    uint32_t cached_reads, dead_reads, dma_alias, wrong_core;
    uint32_t ack_waits, served;
    /* the neighbours of px's span in its edge lines */
    uint32_t nb_lo, nb_hi;      /* byte offsets in pxv (none: 0) */
    uint8_t nb_val;
} xc;

static bool in_buf(const void *p, const uint8_t *base, uint32_t n)
{
    return (const uint8_t *)p >= base && (const uint8_t *)p < base + n;
}
static void xc_point(void);     /* every port call is a schedule point */
static uint32_t xc_rd_us;       /* a slow port while the model runs: a
                                   read takes several passes */

static int lock_depth;
static bool due_now;             /* the owner's pass has a frame due */
static uint32_t due_writes, due_reads, art_port;
static pthread_t art_tid;
static bool art_running;
static uint32_t disc_gen, cut_evt, stale_evt;
static void evt_check(uint32_t a)
{
    if (a != HP_EVT_WR && a != HP_EVT_RD)
        return;
    if (!powered)
        cut_evt++;              /* the event ring of a cut BCM */
    else if (gen_no != disc_gen)
        stale_evt++;            /* a restarted one's, before discovery */
}
/* The port moves whole words at the address rounded down (CR1 [silicon]
 * 2026-10-25: a block written from a mid-word address lands from the
 * word's start); half_addr counts the writes that asked for a mid-word
 * start, which the driver must never send */
static uint32_t half_addr, lone_tail;
static uint32_t port_addr(uint32_t a, bool write)
{
    if ((a & 2) && write)
        half_addr++;
    return a & ~3u;
}
static int p_rd(uint32_t a, uint32_t *v)
{
    a = port_addr(a, false);
    port_ops++;
    now_us_ += xc_rd_us;
    xc_point();
    if (a == HP_VERSION)
        disc_gen = gen_no;      /* the driver's discovery starts */
    evt_check(a);
    if (art_running && pthread_equal(pthread_self(), art_tid))
        art_port++;             /* the art thread never touches the port */
    if (due_now && a >= THUMBS && a < THUMBS + THUMBS_BYTES)
        due_reads++;
    chip_run();
    memcpy(v, mem + a, 4);      /* a is rounded down to a word (port_addr) */
    return 0;
}
static int p_wr(uint32_t a, uint32_t v)
{
    a = port_addr(a, true);
    port_ops++;
    xc_point();
    evt_check(a);
    memcpy(mem + a, &v, 4);
    return 0;
}
static uint32_t e2_any, e2_pieces, ring_unlocked;
static uint32_t port_us;                /* a slow port: us per block write */
static uint32_t restart_at_block;       /* a restart at this block write */
static void p_restart(void);
static int p_wr_block(uint32_t a, const uint16_t *s, uint32_t n)
{
    a = port_addr(a, true);
    port_ops++;
    xc_point();
    now_us_ += port_us;
    if (restart_at_block && !--restart_at_block)
        p_restart();
    if (a >= SCRATCH && a < SCRATCH + RING_POOL && lock_depth <= 0)
        ring_unlocked++;
    if (a >= SCRATCH && a < SCRATCH + RING_POOL && due_now)
        due_writes++;
    if (art_running && pthread_equal(pthread_self(), art_tid))
        art_port++;
    if (xc.model && in_buf(s, piece_mem, sizeof(piece_mem)))
        xc.cached_reads++;      /* the owner's CPU through its own cache */
    /* a lone trailing halfword is lost [silicon, sitting 2 ALGNP
       PLH1/3/5]: an odd count lands all but its last halfword (the
       device's dt_wr_block merges it by a read-modify-write instead;
       this stub models the bare port, so the driver must never ask) */
    if (n & 1)
    {
        lone_tail++;
        n--;
    }
    memcpy(mem + a, s, 2 * n);
    return 0;
}
static bool tear_upload;        /* the next upload lands one bit wrong */

static uint32_t p_upload(uint32_t a, const uint16_t *s, uint32_t bytes)
{
    a = port_addr(a, true);
    port_ops++;
    now_us_ += port_us;
    if (bytes & 3)              /* engine 2 moves whole words only */
    {
        lone_tail++;
        bytes &= ~3u;
    }
    memcpy(mem + a, s, bytes);
    if (tear_upload && bytes)
    {
        mem[a + bytes - 1] ^= 0x01;
        tear_upload = false;
    }
    e2_bytes += bytes;
    return bytes;
}
static void p_doorbell(void) { }
static void p_lock(void) { lock_depth++; }
static void p_unlock(void) { lock_depth--; }
static uint32_t p_now_us(void) { return now_us_; }
static uint32_t p_now_ms(void) { return now_us_ / 1000; }
static void p_yield(void) { now_us_ += 50; chip_run(); }
static uint32_t p_gen(void) { return powered ? gen_no : 0; }
static bool p_lit(void) { return lit_; }
static int p_push(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    uint32_t r, c;

    if (!powered)
        return -1;
    for (r = y; r < y + h; r++)
        for (c = x; c < x + w; c++)
        {
            ((uint16_t *)(mem + FRONT))[r * W + c] = ppfb[r * W + c];
            panel[r * W + c] = ppfb[r * W + c];
        }
    return 0;
}
static void p_restart(void) { restarts++; now_us_ += 400000; chip_start(); }
static int p_power(bool up)
{
    power_calls++;
    if (up)
        chip_start();
    else
    {
        dec_kill();
        powered = false;
    }
    return 0;
}
static bool p_charging(void) { return charging_; }
/* engine 2 for a DECODE ring piece (the lock held) */
static uint32_t p_upload_any(uint32_t a, const void *s, uint32_t bytes)
{
    a = port_addr(a, true);
    port_ops++;
    xc_point();
    if (lock_depth <= 0)
        ring_unlocked++;
    if (due_now)
        due_writes++;
    if (art_running && pthread_equal(pthread_self(), art_tid))
        art_port++;
    if (xc.model && in_buf(s, piece_mem, sizeof(piece_mem)))
    {
        if (!xc.piece_live)
            xc.dead_reads++;
        s = piece_ram + ((const uint8_t *)s - piece_mem);   /* engine 2
                                                               reads memory */
    }
    else if (xc.model && in_buf(s, piece_ram, sizeof(piece_ram)))
        xc.dma_alias++;
    memcpy(mem + a, s, bytes);
    e2_any += bytes;
    e2_pieces++;
    return bytes;
}
static int bufs_out;
static bool no_buf;
static void *p_dec_buf(bool get)
{
    if (!get)
    {
        bufs_out--;
        xc.piece_live = false;
        return NULL;
    }
    if (no_buf)
        return NULL;
    bufs_out++;
    xc.piece_live = true;
    return piece_mem;
}

static const struct bjc_port port = {
    p_rd, p_wr, p_wr_block, p_upload, p_doorbell, p_lock, p_unlock, p_now_us,
    p_now_ms, p_yield, p_gen, p_lit, p_push, p_restart, p_power, p_charging,
    p_upload_any, p_dec_buf,
};

/* ---- trace and helpers ----------------------------------------------------- */
static char lines[256][112];
static int nlines;
static struct bjc_slot slots[BJC_SLOTS];
static uint32_t epoch_seen, epoch_calls;

static void tr(const char *l)
{
    if (nlines < 256)
        snprintf(lines[nlines++], sizeof(lines[0]), "%s", l);
}

static int logged(const char *prefix)
{
    int i;

    for (i = 0; i < nlines; i++)
        if (!strncmp(lines[i], prefix, strlen(prefix)))
            return 1;
    return 0;
}

static void on_epoch(uint32_t e) { epoch_seen = e; epoch_calls++; }

static void fresh(void)
{
    uint32_t i;

    for (i = 0; i < W * H; i++)
        ppfb[i] = (uint16_t)(i * 2654435761u >> 16);
    nlines = 0;
    restarts = 0;
    lit_ = true;
    chip_start();
    bjc_init(&port, slots, tr);
    bjc_verbose(true);
    bcmjob_on_epoch(on_epoch);
}

static bool panel_is_ppfb(void) { return !memcmp(panel, ppfb, sizeof(ppfb)); }

/* ---- cases ----------------------------------------------------------------- */

static void t_round_trip(void)
{
    uint32_t fill[4] = { J_SURF_FRONT, J_XY(10, 20), J_XY(40, 8), 0xF800 };
    uint32_t pres[4] = { J_SURF_FRONT, J_XY(0, 0), J_XY(W, H), 0 };
    uint32_t seq, wr0, i, ref_w[64];
    struct jring ref = { ref_w, 64, 0, 0 };
    static uint16_t cf[W * H], cp[W * H];
    struct jref cpu;
    uint32_t rec[1 + HP_MAX_PAYLOAD], rep[1 + HP_MAX_PAYLOAD], n;

    fresh();
    CHECK(bcmjob_have(J_OP_FILL), "have FILL after discovery");
    CHECK(logged("bcm power state=C1 why=lit"), "power line");
    CHECK(logged("bcmjob up epoch="), "up line");
    CHECK(logged("bcmjob vpu=0x3 expect=0x3") &&
          !logged("bcmjob vpu=0x3 expect=0x3 why="), "J_VPU logged at discovery");
    CHECK(lock_depth == 0, "lock balanced %d", lock_depth);
    wr0 = *w32(HP_CMD_WR) & 0xFFFF;
    stalled = true;             /* look at the words before the chip runs */
    CHECK(bcmjob_submit(J_OP_FILL, 7, fill, 4) == BCMJOB_OK, "submit FILL");
    CHECK(bcmjob_submit(J_OP_PRESENT, 8, pres, 4) == BCMJOB_OK, "submit PRESENT");
    jring_put(&ref, J_OP_FILL, 7, fill, 4);
    jring_put(&ref, J_OP_PRESENT, 8, pres, 4);
    for (i = 0; i < ref.wr; i++)
        CHECK(*w32(HP_CMD_RING + 4 * ((wr0 + i) & (HP_CMD_WORDS - 1))) == ref_w[i],
              "ring word %u: %08x vs %08x", i,
              *w32(HP_CMD_RING + 4 * ((wr0 + i) & (HP_CMD_WORDS - 1))), ref_w[i]);
    stalled = false;
    CHECK(bcmjob_fence(&seq) == BCMJOB_OK, "fence");
    CHECK(bcmjob_wait(seq, J_T_FENCE_BASE + J_T_DRAW + J_T_PRESENT) == BCMJOB_OK,
          "wait");
    CHECK(logged("bcmjob submit op=0x25 tag=7 n=4"), "submit line");
    CHECK(logged("bcmjob fence seq="), "fence line");
    /* the CPU path's frame from the same records */
    memcpy(cf, ppfb, sizeof(cf));
    memcpy(cp, ppfb, sizeof(cp));
    jref_init(&cpu, (struct jref_surf){ cf, W, H, W }, (struct jref_surf){ 0, 0, 0, 0 },
              (struct jref_surf){ cp, W, H, W });
    rec[0] = HP_REC(J_OP_FILL, 4, 7);
    memcpy(rec + 1, fill, 16);
    jref_exec(&cpu, rec, rep, &n);
    rec[0] = HP_REC(J_OP_PRESENT, 4, 8);
    memcpy(rec + 1, pres, 16);
    jref_exec(&cpu, rec, rep, &n);
    CHECK(!memcmp(cp, panel, sizeof(cp)), "panel equals the CPU path's");
}

/* A torn upload: the BCM's COMMIT answers J_E_SUM, and the fault trace
 * names the status, the slot and its size (bench 2026-10-25) */
static void t_commit_status(void)
{
    static uint16_t pic[32 * 32];
    struct bcmjob_slot s;
    uint32_t seq, i;
    char want[80];

    fresh();
    for (i = 0; i < 32 * 32; i++)
        pic[i] = (uint16_t)(i * 5 + 1);
    CHECK(bcmjob_slot(J_CLS_DRIFT, 32, 32, 0x77, 0x88, &s) == BCMJOB_OK, "ALLOC");
    tear_upload = true;
    CHECK(bcmjob_upload(&s, pic) == BCMJOB_OK, "the upload is sent");
    CHECK(bcmjob_fence(&seq) == BCMJOB_OK, "fence");
    bcmjob_wait(seq, 200);
    snprintf(want, sizeof(want), "bcmjob fault op=0x22 st=%d why=failed slot=%lu 32x32",
             J_E_SUM, (unsigned long)s.slot);
    CHECK(logged(want), "the COMMIT's status and slot traced: %s", want);
    CHECK(bcmjob_have(J_OP_COMMIT), "a J_E_SUM is a normal outcome: not marked");
}

static void t_slots(void)
{
    static uint16_t pic[32 * 32];
    struct bcmjob_slot s, s2;
    uint32_t blit[6], pres[4] = { J_SURF_FRONT, J_XY(0, 0), J_XY(32, 32), 0 };
    uint32_t seq, i, r, ok = 1;

    fresh();
    for (i = 0; i < 32 * 32; i++)
        pic[i] = (uint16_t)(i * 7 + 3);
    CHECK(bcmjob_slot(J_CLS_DRIFT, 32, 32, 0x1234, 0x5678, &s) == BCMJOB_OK, "ALLOC");
    CHECK(!s.hit && s.epoch == bcmjob_epoch(), "a miss in this epoch");
    e2_bytes = 0;
    CHECK(bcmjob_upload(&s, pic) == BCMJOB_OK, "upload");
    CHECK(e2_bytes == 32 * 32 * 2, "engine 2 moved it (%u)", e2_bytes);
    blit[0] = s.slot; blit[1] = 0; blit[2] = J_XY(32, 32);
    blit[3] = J_SURF_FRONT; blit[4] = 0; blit[5] = J_BLIT_OPAQUE;
    CHECK(bcmjob_submit(J_OP_BLIT, 1, blit, 6) == BCMJOB_OK, "BLIT");
    CHECK(bcmjob_submit(J_OP_PRESENT, 2, pres, 4) == BCMJOB_OK, "PRESENT");
    CHECK(bcmjob_fence(&seq) == BCMJOB_OK && bcmjob_wait(seq, 100) == BCMJOB_OK,
          "fenced");
    for (r = 0; r < 32; r++)
        ok &= !memcmp(&panel[r * W], &pic[r * 32], 64);
    CHECK(ok, "the slot's pixels on the panel (COMMIT's F16w accepted)");
    CHECK(!logged("bcmjob fault"), "no faults");
    CHECK(bcmjob_slot(J_CLS_DRIFT, 32, 32, 0x1234, 0x5678, &s2) == BCMJOB_OK &&
          s2.hit && s2.slot == s.slot, "the same key hits");
    /* 8 rows: the CPU's copy, row by row at the slot's pitch */
    e2_bytes = 0;
    CHECK(bcmjob_slot(J_CLS_DRIFT, 31, 8, 9, 9, &s2) == BCMJOB_OK &&
          bcmjob_upload(&s2, pic) == BCMJOB_OK && e2_bytes == 0, "small: the CPU");
    CHECK(bcmjob_fence(&seq) == BCMJOB_OK && bcmjob_wait(seq, 100) == BCMJOB_OK &&
          !logged("bcmjob fault"), "odd width committed");
}

/* Odd widths (CR1 [silicon] 2026-10-25, 301 x 7 in a strip slot at pitch
 * 602 bytes: every other row started mid-word and landed one pixel
 * early).  Every class: the image now rounds every pitch to 16 pixels, so
 * no row starts mid-word; every pixel lands where it belongs and COMMIT
 * accepts the sum.  Then an older image's pitch (2 x w, odd in words):
 * the driver still never asks the port for a mid-word start (write_row
 * merges the odd first and last pixels) and every pixel lands */
static void t_odd_width(void)
{
    static const uint16_t dims[][2] = { { 301, 7 }, { 3, 5 }, { 1, 3 }, { 37, 2 } };
    static const uint8_t cls[] = { J_CLS_DRIFT, J_CLS_STRIP, J_CLS_THUMB, J_CLS_SCRATCH };
    static uint16_t pic[301 * 7];
    struct bcmjob_slot s;
    uint32_t seq, i, k, c, x, y, bad, old;

    for (old = 0; old < 2; old++)
        for (c = 0; c < sizeof(cls); c++)
            for (k = 0; k < sizeof(dims) / sizeof(dims[0]); k++)
            {
                uint32_t w = dims[k][0], h = dims[k][1], pitch;

                fresh();
                for (i = 0; i < w * h; i++)
                    pic[i] = (uint16_t)(0x8000 + i * 0x2f + k);
                CHECK(bcmjob_slot(cls[c], w, h, 0x0dd0 + k, c, &s) == BCMJOB_OK,
                      "class %u %ux%u: ALLOC", cls[c], w, h);
                if (old)
                    slots[s.slot].pitch = 2 * w;    /* an older image's reply */
                pitch = slots[s.slot].pitch;
                CHECK(old || !(pitch & 31), "class %u %ux%u: pitch %u a multiple "
                      "of 32 bytes", cls[c], w, h, pitch);
                half_addr = 0;
                CHECK(bcmjob_upload(&s, pic) == BCMJOB_OK, "class %u %ux%u: upload",
                      cls[c], w, h);
                CHECK(half_addr == 0, "class %u %ux%u%s: no port write from mid-word "
                      "(%u)", cls[c], w, h, old ? " (old pitch)" : "", half_addr);
                bad = 0;
                for (y = 0; y < h; y++)
                    for (x = 0; x < w; x++)
                    {
                        uint32_t a = slots[s.slot].addr + y * pitch + 2 * x;

                        bad += *(uint16_t *)(mem + a) != pic[y * w + x];
                    }
                CHECK(bad == 0, "class %u %ux%u%s: every pixel where it belongs "
                      "(%u wrong)", cls[c], w, h, old ? " (old pitch)" : "", bad);
                if (!old)
                    CHECK(bcmjob_fence(&seq) == BCMJOB_OK &&
                          bcmjob_wait(seq, 100) == BCMJOB_OK && !logged("bcmjob fault"),
                          "class %u %ux%u: COMMIT accepts the sum", cls[c], w, h);
            }
}

static void t_cut_mid_scroll(void)
{
    uint32_t scroll[4] = { J_SURF_FRONT, J_XY(0, 40), J_XY(W, 160), (uint32_t)-16 & 0xFFFF };
    uint32_t seq, e0;

    fresh();
    CHECK(bcmjob_have(J_OP_SCROLL), "up");
    e0 = bcmjob_epoch();
    stalled = true;
    CHECK(bcmjob_submit(J_OP_SCROLL, 3, scroll, 4) == BCMJOB_OK, "SCROLL queued");
    CHECK(bcmjob_fence(&seq) == BCMJOB_OK, "fence");
    powered = false;            /* cut before it ran */
    CHECK(bcmjob_wait(seq, 50) == BCMJOB_OFF, "OFF after the cut");
    CHECK(bcmjob_resync(0, 0, W, H) == BCMJOB_OFF, "no resync while cut");
    CHECK(logged("bcm power state=C0"), "C0 line");
    chip_start();               /* lit again: a new power-up */
    CHECK(bcmjob_resync(0, 0, W, H) == BCMJOB_OK, "resync");
    CHECK(panel_is_ppfb(), "the panel equals the PP framebuffer");
    CHECK(epoch_calls >= 1 && epoch_seen > e0, "on_epoch with a new epoch");
}

static void t_death_mid_slide(void)
{
    uint32_t a[8 + 25], k, seq, e0 = 0;

    fresh();
    CHECK(bcmjob_have(J_OP_ANIM), "up");
    e0 = bcmjob_epoch();
    a[0] = J_ANIM_SLIDE; a[1] = J_SURF_FRONT; a[2] = J_SURF_BACK;
    a[3] = J_XY(0, 22); a[4] = J_XY(W, 200); a[5] = 0; a[6] = 33; a[7] = 10;
    for (k = 0; k < 5; k++)
        a[8 + k] = (32 * (2 * k)) | (uint32_t)(32 * (2 * k + 1)) << 16;
    CHECK(bcmjob_submit(J_OP_ANIM, 5, a, 13) == BCMJOB_OK, "SLIDE");
    for (k = 0; k < 2000; k++)
        p_yield();              /* 100 ms: three frames */
    CHECK(anim_on && !panel_is_ppfb(), "the slide is moving");
    dead = true;
    CHECK(bcmjob_fence(&seq) == BCMJOB_OK, "fence");
    CHECK(bcmjob_wait(seq, 20) == BCMJOB_TIMEOUT, "TIMEOUT");
    CHECK(restarts == 1 || restarts > 0, "restarted");
    CHECK(logged("bcmjob fault op=0x00 why=dead"), "the death logged");
    CHECK(panel_is_ppfb(), "the final frame (the PP's) on the panel");
    CHECK(bcmjob_have(J_OP_FILL), "jobs again after one failure");
    CHECK(bcmjob_epoch() > e0 && epoch_seen == bcmjob_epoch(), "new epoch");
}

static void t_ring_full(void)
{
    uint32_t fill[4] = { J_SURF_FRONT, J_XY(0, 0), J_XY(8, 8), 0x1F };
    uint32_t k, t, seq;
    int r = BCMJOB_OK;
    static uint16_t cf[W * H];
    struct jref cpu;
    uint32_t rec[5], rep[1 + HP_MAX_PAYLOAD], n;

    fresh();
    CHECK(bcmjob_have(J_OP_FILL), "up");
    bjc_verbose(false);         /* 410 submit lines */
    stalled = true;
    for (k = 0; k < 1000 && r == BCMJOB_OK; k++)
        r = bcmjob_submit(J_OP_FILL, k, fill, 4);
    CHECK(r == BCMJOB_BUSY && k == HP_CMD_WORDS / 5 + 1, "BUSY after %u", k);
    t = now_us_;
    CHECK(bcmjob_submit(J_OP_FILL, 0, fill, 4) == BCMJOB_BUSY && now_us_ == t,
          "BUSY at once, no wait");
    CHECK(logged("bcmjob fault op=0x25 why=busy"), "busy line");
    /* the frame's CPU path, then one push */
    memcpy(cf, ppfb, sizeof(cf));
    jref_init(&cpu, (struct jref_surf){ cf, W, H, W }, (struct jref_surf){ 0, 0, 0, 0 },
              (struct jref_surf){ cf, W, H, W });
    rec[0] = HP_REC(J_OP_FILL, 4, 0);
    memcpy(rec + 1, fill, 16);
    jref_exec(&cpu, rec, rep, &n);
    memcpy(ppfb, cf, sizeof(cf));
    stalled = false;
    CHECK(bcmjob_fence(&seq) == BCMJOB_OK && bcmjob_wait(seq, 500) == BCMJOB_OK,
          "the queue drains");
    CHECK(bcmjob_resync(0, 0, W, H) == BCMJOB_OK && panel_is_ppfb(),
          "resync: the panel equals the CPU frame");
}

static void t_dead_twice(void)
{
    uint32_t seq;

    fresh();
    restarts = 0;
    CHECK(bcmjob_have(J_OP_FILL), "up");
    dead = true;
    CHECK(bcmjob_fence(&seq) == BCMJOB_OK && bcmjob_wait(seq, 10) == BCMJOB_TIMEOUT,
          "first death");
    CHECK(restarts == 1 && bcmjob_have(J_OP_FILL), "restarted, jobs on");
    dead = true;
    CHECK(bcmjob_fence(&seq) == BCMJOB_OK && bcmjob_wait(seq, 10) == BCMJOB_TIMEOUT,
          "second death");
    CHECK(restarts == 2, "the panel restarted again (%u)", restarts);
    CHECK(!bcmjob_have(J_OP_FILL) && bjc_state() == 3, "jobs off for the boot");
    CHECK(bcmjob_submit(J_OP_FILL, 0, &seq, 0) == BCMJOB_OFF, "submit: OFF");
    CHECK(logged("bcmjob off why=second failure"), "off line");
}

static void t_slow(void)
{
    uint32_t fill[4] = { J_SURF_FRONT, J_XY(0, 0), J_XY(8, 8), 0x1F }, seq;

    fresh();
    CHECK(bcmjob_have(J_OP_FILL), "up");
    slow_until = now_us_ + 100000;      /* the BCM busy 100 ms, alive */
    CHECK(bcmjob_submit(J_OP_FILL, 0, fill, 4) == BCMJOB_OK, "FILL");
    CHECK(bcmjob_fence(&seq) == BCMJOB_OK && bcmjob_wait(seq, J_T_DRAW) == BCMJOB_TIMEOUT,
          "a missed deadline");
    CHECK(restarts == 0 && bcmjob_have(J_OP_FILL), "slow, alive: kept, not cut");
    CHECK(logged("bcmjob fault op=0x25 why=timeout"), "timeout line names FILL");
    slow_until = now_us_ + 100000;
    CHECK(bcmjob_submit(J_OP_FILL, 0, fill, 4) == BCMJOB_OK, "FILL again");
    CHECK(bcmjob_fence(&seq) == BCMJOB_OK && bcmjob_wait(seq, J_T_DRAW) == BCMJOB_TIMEOUT,
          "missed again");
    CHECK(!bcmjob_have(J_OP_FILL) && bcmjob_have(J_OP_BLIT),
          "FILL takes its CPU path for the boot, BLIT still goes");
    slow_until = 0;
}

static void t_dark_hour(void)
{
    uint32_t ops, k;

    fresh();
    restarts = 0;
    CHECK(bcmjob_have(J_OP_FILL), "up");
    lit_ = false;
    powered = false;            /* dark means cut */
    bjc_dark(true);
    ops = port_ops;
    power_calls = 0;
    for (k = 0; k < 3600; k++)
    {
        now_us_ += 1000000;
        CHECK(!bcmjob_have(J_OP_FILL) || k, "C0: no jobs");
        if (k % 600 == 0)
            CHECK(bcmjob_submit(J_OP_FILL, 0, &k, 0) == BCMJOB_OFF, "C0: OFF");
        bcmjob_dark_ups(BCM_WANT_N);
    }
    now_us_ += 1000;
    CHECK(port_ops == ops, "no port access in C0 (%u)", port_ops - ops);
    CHECK(bcmjob_dark_ups(0) == 0 && power_calls == 0, "0 dark power-ups");
    CHECK(logged("bcm power ups dark=0 decode=0 game=0 video=0"), "the hourly line");
    CHECK(bcm_power_want(BCM_WANT_DECODE, 1000) == BCMJOB_REFUSED, "decode refused on battery");
    charging_ = true;
    CHECK(bcm_power_want(BCM_WANT_DECODE, 1000) == BCMJOB_OK &&
          bcmjob_dark_ups(BCM_WANT_DECODE) == 1 && powered, "on the charger: up");
    now_us_ += 2000000;
    CHECK(bcmjob_dark_ups(0) == 1 && !powered, "the hold ran out: cut again");
    charging_ = false;
    bjc_dark(false);
}


/* P19 S0 (MF-2): the driver's state has one writer, the ring owner; a
 * suspend from another thread is a request the owner applies and
 * acknowledges, with a bounded wait */
static struct bjc_req q;
static bool as_owner, owner_answers;
static int kicks, acks, waits, mode_calls;
static bool mode_jobs;

static bool o_is_owner(void) { return as_owner; }
static void o_kick(void) { kicks++; }
static void o_ack(void) { acks++; }
static uint32_t mode_gen_seen;
static void o_mode(bool jobs) { mode_calls++; mode_jobs = jobs; mode_gen_seen = p_gen(); }
static int o_wait(uint32_t ms)
{
    waits++;
    (void)ms;
    if (!owner_answers)
        return -1;              /* the owner never ran: the time ran out */
    as_owner = true;            /* the owner's queue: SYS_BCMJOB_REQ */
    bjc_apply();
    as_owner = false;
    return 0;
}

static const struct bjc_owner owner_ops = {
    &q, o_is_owner, o_kick, o_wait, o_ack, o_mode, NULL, NULL,
    NULL, NULL, NULL, NULL,
};

static void t_requests(void)
{
    uint32_t e0;

    fresh();
    memset(&q, 0, sizeof(q));
    kicks = acks = waits = mode_calls = 0;
    bjc_set_owner(&owner_ops);
    as_owner = true;
    CHECK(bcmjob_have(J_OP_FILL) && mode_calls == 0,
          "the owner: jobs, none in flight: job mode stays off");
    e0 = bcmjob_epoch();

    /* the owner's own suspend: at once, no kick, no wait */
    CHECK(bjc_suspend_request(true, 10) == BCMJOB_OK && kicks == 0 &&
          waits == 0 && acks == 1, "owner: applied inline");
    CHECK(!bcmjob_have(J_OP_FILL) && !mode_jobs, "owner: suspended, mode off");
    CHECK(bjc_suspend_request(false, 10) == BCMJOB_OK && bcmjob_have(J_OP_FILL)
          && bcmjob_epoch() != e0 && !mode_jobs, "owner: resumed, a new epoch");

    /* another thread, the owner answers: kicked, applied by the owner */
    as_owner = false;
    owner_answers = true;
    CHECK(bjc_suspend_request(true, 10) == BCMJOB_OK && kicks == 1 &&
          waits == 1 && (q.done & 1), "requester: acknowledged");
    as_owner = true;
    CHECK(!bcmjob_have(J_OP_FILL), "requester: the owner sees it suspended");
    as_owner = false;
    CHECK(bjc_suspend_request(false, 10) == BCMJOB_OK && kicks == 2,
          "requester: resume acknowledged");
    as_owner = true;
    CHECK(bcmjob_have(J_OP_FILL), "resumed");

    /* another thread, the owner never answers: refused, withdrawn */
    as_owner = false;
    owner_answers = false;
    CHECK(bjc_suspend_request(true, 10) == BCMJOB_TIMEOUT,
          "requester: no acknowledgement, refused");
    CHECK(logged("bcmjob suspend on=1 ack timeout"), "the timeout line");
    as_owner = true;
    CHECK(bcmjob_have(J_OP_FILL) && !(q.done & 1),
          "withdrawn: the owner's next call keeps jobs on");

    /* the dark record: the backlight thread writes it, the owner reads */
    as_owner = false;
    bjc_dark(true);
    CHECK(q.dark == 1 && !(q.dseq & 1) && q.dseq == 2, "dark: one seqlock write");
    now_us_ += 5000000;
    bjc_dark(false);
    CHECK(q.dark == 0 && q.total >= 5000 && q.dseq == 4, "dark: 5 s counted");
    as_owner = true;
    bjc_set_owner(NULL);
}


/* S0 review M1/M2: job mode only while jobs are in flight, and told again
 * for every power-up (the device keys it to bcmjob_gen) */
static void t_mode(void)
{
    uint32_t fill[4] = { J_SURF_FRONT, J_XY(0, 0), J_XY(8, 8), 0x1F }, seq;
    int calls;
    struct bcmjob_slot sl;

    fresh();
    memset(&q, 0, sizeof(q));
    kicks = acks = waits = mode_calls = 0;
    mode_jobs = false;
    bjc_set_owner(&owner_ops);
    as_owner = true;
    CHECK(bcmjob_have(J_OP_FILL) && mode_calls == 0, "up, idle: mode off");
    CHECK(bcmjob_submit(J_OP_FILL, 0, fill, 4) == BCMJOB_OK && mode_calls == 1
          && mode_jobs && mode_gen_seen == gen_no, "a job in flight: on");
    CHECK(bcmjob_fence(&seq) == BCMJOB_OK && mode_jobs, "fenced: still on");
    CHECK(bcmjob_wait(seq, J_T_FENCE_BASE + J_T_DRAW) == BCMJOB_OK &&
          mode_calls == 2 && !mode_jobs, "the last fence done: off (M2)");
    /* a hang between batches: the tick's check is back (mode off) */
    CHECK(bcmjob_have(J_OP_FILL) && !mode_jobs, "idle after a batch: off");

    /* M1: a power cycle with a job in flight */
    CHECK(bcmjob_submit(J_OP_FILL, 0, fill, 4) == BCMJOB_OK && mode_jobs,
          "in flight again");
    calls = mode_calls;
    gen_no++;                   /* a new power-up (bcm_init) */
    CHECK(bcmjob_have(J_OP_FILL) && !mode_jobs && mode_calls == calls + 1,
          "new power-up: the old ring's jobs are gone, mode off");
    CHECK(bcmjob_submit(J_OP_FILL, 0, fill, 4) == BCMJOB_OK && mode_jobs &&
          mode_gen_seen == gen_no && mode_calls >= 2,
          "told again for the new power-up (M1, gen %u)", gen_no);
    /* a reply (ALLOC) comes in ring order: everything before it ran */
    CHECK(bcmjob_slot(J_CLS_DRIFT, 8, 8, 1, 2, &sl) == BCMJOB_OK && !mode_jobs,
          "a call's reply: drained, off");
    bjc_set_owner(NULL);
}

/* J_VPU (jobs.h 0x44): read once per BCM start, logged against what the
 * image's J_OPS lead to expect; a failed check is a slower image, not a
 * fault */
static int count_lines(const char *prefix)
{
    int i, n = 0;

    for (i = 0; i < nlines; i++)
        n += !strncmp(lines[i], prefix, strlen(prefix));
    return n;
}

static void t_vpu(void)
{
    uint32_t fill[4] = { J_SURF_FRONT, J_XY(0, 0), J_XY(8, 8), 0x1F }, seq, ops;

    CHECK(bjc_vpu_expected(0x0000BFFF) == 3, "no DECODE: 3");
    CHECK(bjc_vpu_expected(0x0000BFFF | 1u << (J_OP_DECODE - 0x20)) == 7,
          "DECODE: 7");
    CHECK(bjc_vpu_expected(0) == 3, "no ops: 3");

    fresh();
    CHECK(bcmjob_have(J_OP_FILL) && bcmjob_have(J_OP_BLIT), "up");
    CHECK(count_lines("bcmjob vpu=") == 1, "once per start (%d)",
          count_lines("bcmjob vpu="));
    powered = false;            /* cut: no read while down */
    ops = port_ops;
    CHECK(!bcmjob_have(J_OP_FILL) && port_ops == ops, "down: no port access");
    chip_start();               /* the next start: logged again */
    CHECK(bcmjob_have(J_OP_FILL) && count_lines("bcmjob vpu=") == 2,
          "again at the next start");

    vpu_word = J_VPU_CHECKED;   /* the lane shift's check failed */
    fresh();
    CHECK(bcmjob_have(J_OP_FILL), "degraded: jobs still on");
    CHECK(logged("bcmjob vpu=0x1 expect=0x3 why=degraded"), "degraded line");
    CHECK(!logged("bcmjob fault"), "not a fault");
    CHECK(bcmjob_submit(J_OP_FILL, 0, fill, 4) == BCMJOB_OK &&
          bcmjob_fence(&seq) == BCMJOB_OK &&
          bcmjob_wait(seq, J_T_FENCE_BASE + J_T_DRAW) == BCMJOB_OK, "jobs run");

    vpu_word = 0;               /* an image before the word */
    fresh();
    CHECK(bcmjob_have(J_OP_FILL) && logged("bcmjob vpu=0x0 expect=0x3 why=unchecked"),
          "unchecked line");

    vpu_word = J_VPU_CHECKED | J_VPU_SHIFT | J_VPU_MAC;
    chip_start();               /* the display-only image: it decodes */
    *w32(BLOCK + J_OPS) |= 1u << (J_OP_DECODE - 0x20);
    nlines = 0;
    CHECK(bcmjob_have(J_OP_FILL) && logged("bcmjob vpu=0x7 expect=0x7") &&
          !logged("bcmjob vpu=0x7 expect=0x7 why="), "the decoding image: 7");
    vpu_word = J_VPU_CHECKED | J_VPU_SHIFT;
}

/* ---- bcmjob_resync after the queued jobs (jobs.md section 6) --------------- */
static void t_resync_order(void)
{
    uint32_t scroll[4] = { J_SURF_FRONT, J_XY(0, 40), J_XY(W, 160), (uint32_t)-16 & 0xFFFF };
    uint32_t pres[4] = { J_SURF_FRONT, J_XY(0, 0), J_XY(W, H), 0 }, seq, k, t0;
    int r = BCMJOB_OK;

    /* a SCROLL queued, then PRESENTs until one is BUSY; the BCM gets to
       them 5 ms later: the resync waits for them, so FRONT ends equal to
       the PP's frame */
    fresh();
    CHECK(bcmjob_have(J_OP_SCROLL), "up");
    stalled = true;
    CHECK(bcmjob_submit(J_OP_SCROLL, 1, scroll, 4) == BCMJOB_OK, "SCROLL queued");
    for (k = 0; k < 2 * HP_CMD_WORDS && r == BCMJOB_OK; k++)
        r = bcmjob_submit(J_OP_PRESENT, 2, pres, 4);
    CHECK(r == BCMJOB_BUSY, "a BUSY PRESENT");
    stalled = false;
    slow_until = now_us_ + 5000;
    nlines = 0;
    t0 = now_us_;
    CHECK(bcmjob_resync(0, 0, W, H) == BCMJOB_OK, "resync");
    t0 = now_us_ - t0;
    CHECK(bcmjob_fence(&seq) == BCMJOB_OK && bcmjob_wait(seq, 100) == BCMJOB_OK, "fence");
    CHECK(!memcmp(mem + FRONT, ppfb, sizeof(ppfb)) && panel_is_ppfb(),
          "FRONT and the panel equal the PP's frame after a queued SCROLL");
    CHECK(t0 >= 5000 && !logged("bcmjob resync forced"), "waited %u us for the queue", t0);
    slow_until = 0;

    /* the BCM never gets to them: the push goes anyway at the bound,
       logged (truth wins; the shell resyncs again after its next fence) */
    fresh();
    CHECK(bcmjob_have(J_OP_SCROLL), "up");
    stalled = true;
    CHECK(bcmjob_submit(J_OP_SCROLL, 1, scroll, 4) == BCMJOB_OK, "SCROLL queued");
    nlines = 0;
    t0 = now_us_;
    CHECK(bcmjob_resync(0, 0, W, H) == BCMJOB_OK, "resync");
    t0 = now_us_ - t0;
    CHECK(logged("bcmjob resync forced why=timeout") &&
          !memcmp(mem + FRONT, ppfb, sizeof(ppfb)) && panel_is_ppfb() &&
          t0 <= 1000u * (J_T_FENCE_BASE + J_T_PRESENT) + 1000,
          "the timeout path: pushed after %u us", t0);
    /* an epoch change while it waits: pushed at once, logged */
    fresh();
    CHECK(bcmjob_have(J_OP_SCROLL), "up");
    stalled = true;
    CHECK(bcmjob_submit(J_OP_SCROLL, 1, scroll, 4) == BCMJOB_OK, "SCROLL queued");
    {
        uint32_t e = chip.epoch + 1;

        evt_put(J_EV_EPOCH, 0, &e, 1);
    }
    nlines = 0;
    CHECK(bcmjob_resync(0, 0, W, H) == BCMJOB_OK && logged("bcmjob resync forced why=epoch") &&
          !memcmp(mem + FRONT, ppfb, sizeof(ppfb)), "an epoch change: pushed");
    stalled = false;
}

#ifdef HAVE_BCMFW_DEVTEST
/* The bench's forced job failure (bjc_inject_fail): the job goes out
 * asking for a reply and its HP_OK reply reads as failed, through the
 * real reply path: logged, counted, and the second counted failure in a
 * boot marks the op (bcmjob.h BCMJOB_FAIL_MARK) */
static void t_inject(void)
{
    uint32_t fill[4] = { J_SURF_FRONT, J_XY(0, 0), J_XY(8, 8), 0xF800 };
    uint32_t seq, wr0;
    struct bcmjob_slot sl;

    fresh();
    CHECK(bcmjob_have(J_OP_FILL), "up");
    bjc_inject_fail(J_OP_FILL, 1);
    CHECK(logged("bcmjob inject op=0x25 n=1"), "inject line");
    wr0 = *w32(HP_CMD_WR) & 0xFFFF;
    stalled = true;
    CHECK(bcmjob_submit(J_OP_FILL, 5, fill, 4) == BCMJOB_OK, "FILL");
    CHECK(HP_REC_TAG(*w32(HP_CMD_RING + 4 * (wr0 & (HP_CMD_WORDS - 1)))) ==
          (5 | J_TAG_REPLY), "sent asking for a reply");
    stalled = false;
    CHECK(bcmjob_fence(&seq) == BCMJOB_OK &&
          bcmjob_wait(seq, J_T_FENCE_BASE + J_T_DRAW) == BCMJOB_OK,
          "the fence still retires");
    CHECK(((uint16_t *)(mem + FRONT))[0] == 0xF800,
          "the BCM ran it (only the status lies)");
    CHECK(count_lines("bcmjob fault op=0x25 st=2 why=failed") == 1, "first failure logged");
    CHECK(bcmjob_have(J_OP_FILL), "one failure: retried, not marked");

    /* disarmed after n: the next FILL is fine */
    CHECK(bcmjob_submit(J_OP_FILL, 6, fill, 4) == BCMJOB_OK &&
          bcmjob_fence(&seq) == BCMJOB_OK &&
          bcmjob_wait(seq, J_T_FENCE_BASE + J_T_DRAW) == BCMJOB_OK &&
          count_lines("bcmjob fault op=0x25 st=2 why=failed") == 1, "n=1: one only");

    /* n = 0 disarms */
    bjc_inject_fail(J_OP_FILL, 3);
    bjc_inject_fail(J_OP_FILL, 0);
    CHECK(bcmjob_submit(J_OP_FILL, 7, fill, 4) == BCMJOB_OK &&
          bcmjob_fence(&seq) == BCMJOB_OK &&
          bcmjob_wait(seq, J_T_FENCE_BASE + J_T_DRAW) == BCMJOB_OK &&
          count_lines("bcmjob fault op=0x25 st=2 why=failed") == 1, "disarmed");

    /* the second counted failure this boot (across a restart) marks it */
    p_restart();
    bjc_inject_fail(J_OP_FILL, 1);
    CHECK(bcmjob_submit(J_OP_FILL, 8, fill, 4) == BCMJOB_OK &&
          bcmjob_fence(&seq) == BCMJOB_OK &&
          bcmjob_wait(seq, J_T_FENCE_BASE + J_T_DRAW) == BCMJOB_OK, "FILL again");
    CHECK(count_lines("bcmjob fault op=0x25 st=2 why=failed") == 2, "second failure");
    CHECK(!bcmjob_have(J_OP_FILL) && bcmjob_have(J_OP_BLIT),
          "FILL marked failed: its CPU path; BLIT still goes");
    CHECK(bcmjob_submit(J_OP_FILL, 9, fill, 4) == BCMJOB_NOJOB, "submit: NOJOB");

    /* a call's reply: the caller gets the error; not counted */
    bjc_inject_fail(J_OP_ALLOC, 1);
    CHECK(bcmjob_slot(J_CLS_DRIFT, 8, 8, 1, 2, &sl) == BCMJOB_FAILED,
          "ALLOC: the caller sees FAILED");
    CHECK(bcmjob_slot(J_CLS_DRIFT, 8, 8, 1, 2, &sl) == BCMJOB_OK &&
          bcmjob_have(J_OP_ALLOC), "ALLOC again: fine, not marked");
    CHECK(restarts == 1, "never a restart of its own (%u)", restarts);
}
#endif

/* ---- DECODE (bcmjob_decode(), bcmjob_read(); P19 19-6) -------------------- */

static uint8_t *jpg, *big, *bad[3], *twork;
static uint32_t jpg_len, big_len;
static uint16_t RB[142 * 142], TW[J_DEC_MAX][142 * 142];
static uint32_t bad_pieces, big_pieces;

/* The source (the shell's over its fd): counts starts and fetches, checks
 * each piece (at most 8 KB, whole 64-byte units but the file's last),
 * fails past fail_at, takes 2 s once past slow_at, runs hook per fetch */
struct tsrc
{
    const uint8_t *p;
    uint32_t len, pos, starts, fetches, fail_at, slow_at;
    void (*hook)(struct tsrc *t);
};

static int ts_start(void *c)
{
    struct tsrc *t = c;

    t->pos = 0;
    t->starts++;
    return 0;
}

static long ts_fetch(void *c, void *buf, uint32_t n)
{
    struct tsrc *t = c;

    t->fetches++;
    if (t->hook)
        t->hook(t);
    if (n > BCMJOB_DEC_PIECE)
        big_pieces++;
    if (n % J_DEC_UNIT && t->pos + n != t->len)
        bad_pieces++;
    if (t->fail_at && t->pos + n > t->fail_at)
        return -1;
    if (t->slow_at && t->pos + n > t->slow_at)
    {
        t->slow_at = 0;
        now_us_ += 2000000;     /* a spin-up the BCM won't wait for */
    }
    if (n > t->len - t->pos)
        n = t->len - t->pos;
    memcpy(buf, t->p + t->pos, n);
    t->pos += n;
    return n;
}

static int ts_skip(void *c, uint32_t n)
{
    struct tsrc *t = c;

    if (n > t->len - t->pos)
        return -1;
    t->pos += n;
    return 0;
}

static struct jdec_src ts_src(struct tsrc *t, const uint8_t *p, uint32_t len)
{
    struct jdec_src src = { len, ts_start, ts_fetch, ts_skip, t };

    memset(t, 0, sizeof(*t));
    t->p = p;
    t->len = len;
    return src;
}

static struct bcmjob_dec_out dout(uint16_t dw, uint16_t dh, uint16_t cx, uint16_t cy,
                                  uint16_t cw, uint16_t ch, uint32_t key)
{
    struct bcmjob_dec_out o;

    memset(&o, 0, sizeof(o));
    o.dw = dw; o.dh = dh; o.cx = cx; o.cy = cy; o.cw = cw; o.ch = ch;
    o.cls = J_CLS_THUMB;
    o.key_lo = key;
    o.key_hi = 0xA1000000u;
    return o;
}

/* The CPU twin: jdec_run() over the same file and outputs, in PP memory */
static int twin(const uint8_t *p, uint32_t len, const struct bcmjob_dec_out *o, int n)
{
    struct jdec_src src;
    struct jdec_mem m;
    struct jdec_out j[J_DEC_MAX];
    int k;

    for (k = 0; k < n; k++)
    {
        j[k].dw = o[k].dw; j[k].dh = o[k].dh; j[k].cx = o[k].cx; j[k].cy = o[k].cy;
        j[k].cw = o[k].cw; j[k].ch = o[k].ch;
        j[k].px = TW[k];
        j[k].pitch = o[k].cw;
    }
    jdec_mem_src(&src, &m, p, len);
    return jdec_run(&src, j, n, twork, J_DEC_WORK_MIN, NULL);
}

/* A slot read back in bands of 37 rows into RB: its F16w */
static int read_sum(uint32_t slot, uint32_t cw, uint32_t ch, uint32_t *a, uint32_t *b)
{
    uint32_t y, r, band = 37;

    *a = *b = 0;
    for (y = 0; y < ch; y += band)
    {
        uint32_t h = ch - y < band ? ch - y : band;

        if (bcmjob_read(slot, 0, y, cw, h, RB + y * cw, cw) != BCMJOB_OK)
            return 0;
        for (r = 0; r < h; r++)
            j_f16w(a, b, RB + (y + r) * cw, cw);
    }
    return 1;
}

/* A DECODE record of n outputs was submitted (since nlines = 0) */
static int sent_n(int n)
{
    char want[24];
    int i;

    snprintf(want, sizeof(want), " n=%d", J_DEC_ARGS(n));
    for (i = 0; i < nlines; i++)
        if (!strncmp(lines[i], "bcmjob submit op=0x30", 21) && strstr(lines[i], want))
            return 1;
    return 0;
}

/* Every output read back in bands of 37 rows: its F16w the BCM's, its
 * pixels the twin's */
static int same_as_twin(const struct bcmjob_dec_res *res, const struct bcmjob_dec_out *o,
                        int n, const uint8_t *p, uint32_t len)
{
    int k;

    if (twin(p, len, o, n) != JDEC_OK)
        return 0;
    for (k = 0; k < n; k++)
    {
        uint32_t a = 0, b = 0, y, r, band = 37;

        for (y = 0; y < o[k].ch; y += band)
        {
            uint32_t h = o[k].ch - y < band ? o[k].ch - y : band;

            if (bcmjob_read(res->out[k].slot, 0, y, o[k].cw, h, RB + y * o[k].cw,
                            o[k].cw) != BCMJOB_OK)
                return 0;
            for (r = 0; r < h; r++)
                j_f16w(&a, &b, RB + (y + r) * o[k].cw, o[k].cw);
        }
        if (a != res->out[k].a || b != res->out[k].b ||
            memcmp(RB, TW[k], 2u * o[k].cw * o[k].ch))
            return 0;
    }
    return 1;
}

static uint32_t cancel_after;           /* fetches before the hook says stop */
static int busy_inside = 1;             /* a second decode's answer, inside */
static struct tsrc *cur_ts;
static const struct bcmjob_dec_out *cur_o;

static int rect_mid = 1;                /* upload_rect's answer mid-decode */
static bool cancel_cb(void *ctx)
{
    struct bcmjob_dec_res r2;
    struct bcmjob_dec_req q2;
    struct tsrc t2;
    struct jdec_src s2;

    (void)ctx;
    if (busy_inside == 1)
    {
        /* one decode in flight: a second caller is BUSY at once */
        s2 = ts_src(&t2, jpg, jpg_len);
        memset(&q2, 0, sizeof(q2));
        q2.src = &s2;
        q2.out = cur_o;
        q2.n = 1;
        busy_inside = bcmjob_decode(&q2, &r2);
    }
    if (rect_mid == BCMJOB_OK && cur_ts->fetches)
    {
        static uint16_t px8[8 * 8];

        rect_mid = bcmjob_upload_rect(J_SURF_BACK, 0, 0, 8, 8, px8, 8);
    }
    return cancel_after && cur_ts->fetches >= cancel_after;
}

static int decode1(const uint8_t *p, uint32_t len, const struct bcmjob_dec_out *o, int n,
                   uint32_t flags, uint32_t ms, bool cancel, struct bcmjob_dec_res *res,
                   struct tsrc *ts)
{
    struct jdec_src src = ts_src(ts, p, len);
    struct bcmjob_dec_req q;

    memset(&q, 0, sizeof(q));
    q.src = &src;
    q.out = o;
    q.n = n;
    q.flags = flags;
    q.ms = ms;
    q.cancel = cancel ? cancel_cb : NULL;
    cur_ts = ts;
    cur_o = o;
    return bcmjob_decode(&q, res);
}

static void dec_fresh(void)
{
    dec_offer = true;
    dec_hang = false;
    dec_row_us = 0;
    ring_allocs = 0;
    fresh();
}

/* let the chip run what is queued (FREEs behind a job) */
static void settle(void)
{
    uint32_t seq;

    if (bcmjob_fence(&seq) == BCMJOB_OK)
        bcmjob_wait(seq, 200);
}

static int slot_state(uint32_t slot)
{
    return slot < JREF_SLOTS ? chip.slot[slot].state : -1;
}

/* The shell is done with a render's slots (album-render.md section 6) */
static void drop(const struct bcmjob_dec_res *res, int n)
{
    uint32_t p[1];
    int k;

    for (k = 0; k < n; k++)
    {
        p[0] = res->out[k].slot;
        bcmjob_submit(J_OP_FREE, DROP_TAG, p, 1);
    }
    settle();
}

static uint8_t *load(const char *path, uint32_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *p = malloc(200000);
    size_t n = f ? fread(p, 1, 200000, f) : 0;

    if (f)
        fclose(f);
    *len = (uint32_t)n;
    return p;
}

static uint32_t find_sof(const uint8_t *p, uint32_t len)
{
    uint32_t i;

    for (i = 2; i + 1 < len; i++)
        if (p[i] == 0xFF && p[i + 1] == 0xC0)
            return i + 1;
    return 0;
}

static void hook_cut(struct tsrc *t)
{
    if (t->fetches == 3)
        p_power(false);         /* the backlight's cut, mid-stream */
}

static void hook_restart(struct tsrc *t)
{
    if (t->fetches == 3)
        p_restart();
}

static void hook_epoch_event(struct tsrc *t)
{
    uint32_t k;

    if (t->fetches != 3)
        return;
    /* the BCM's own epoch move (a video OPEN took the pools) */
    for (k = 0; k < JREF_SLOTS; k++)
        chip.slot[k].state = J_ST_EMPTY;
    chip.epoch++;
    evt_put(J_EV_EPOCH, 0, &chip.epoch, 1);
    publish();
}

static void hook_dead(struct tsrc *t)
{
    if (t->fetches == 3)
        dead = true;
}


/* ---- DECODE across threads: the art thread and the ring owner -------------
 * The device's shape (S0, MF-2): the caller (here a pthread, "art") posts
 * the call and only fetches pieces, polls its hook and waits; the owner
 * (main) runs every wire step in its passes.  The two run one at a time,
 * handing over at the art thread's wait and the end of the owner's pass,
 * as on one core of a cooperative scheduler. */
static struct bjc_req tq;
static pthread_t owner_tid;
static pthread_mutex_t tmx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t tcv = PTHREAD_COND_INITIALIZER;
static int turn;                        /* 0: the owner's, 1: the art's */
static bool art_done, owner_kicked, kick_only;
static void (*ev_every)(uint32_t pass);
static uint32_t drop_kicks, stop_after, max_pass_us;
static uint32_t art_waits;

static void turn_to(int who, int me)
{
    pthread_mutex_lock(&tmx);
    turn = who;
    pthread_cond_broadcast(&tcv);
    while (turn != me)
        pthread_cond_wait(&tcv, &tmx);
    pthread_mutex_unlock(&tmx);
}

static bool t_is_owner(void) { return pthread_equal(pthread_self(), owner_tid); }
static void t_kick(void) { owner_kicked = true; }
static void t_art_wait(uint32_t ms) { (void)ms; art_waits++; turn_to(0, 1); }
static void t_art_wake(void) { }

static const struct bjc_owner towner = {
    &tq, t_is_owner, t_kick, NULL, NULL, NULL, t_art_wait, t_art_wake,
    NULL, NULL, NULL, NULL,
};

/* What the art thread runs: a decode, then (OK) its outputs read back in
 * bands of 37 rows into RT with their F16w */
static struct
{
    const uint8_t *p;
    uint32_t len, flags, ms, cancel_at;
    const struct bcmjob_dec_out *o;
    int n, r, rd;
    struct bcmjob_dec_res res;
    struct tsrc ts;
    uint32_t a[J_DEC_MAX], b[J_DEC_MAX];
} at;
static uint16_t RT[J_DEC_MAX][142 * 142];

static bool at_cancel(void *ctx)
{
    (void)ctx;
    return at.cancel_at && at.ts.fetches >= at.cancel_at;
}

static void *art_main(void *x)
{
    struct jdec_src src = ts_src(&at.ts, at.p, at.len);
    struct bcmjob_dec_req q;
    int k;

    (void)x;
    pthread_mutex_lock(&tmx);
    while (turn != 1)
        pthread_cond_wait(&tcv, &tmx);
    pthread_mutex_unlock(&tmx);
    memset(&q, 0, sizeof(q));
    q.src = &src;
    q.out = at.o;
    q.n = at.n;
    q.flags = at.flags;
    q.ms = at.ms;
    q.cancel = at_cancel;
    at.r = bcmjob_decode(&q, &at.res);
    at.rd = BCMJOB_OK;
    for (k = 0; at.r == BCMJOB_OK && k < at.n && at.rd == BCMJOB_OK; k++)
    {
        uint32_t y, rr, band = 37, cw = at.o[k].cw, ch = at.o[k].ch;

        at.a[k] = at.b[k] = 0;
        for (y = 0; y < ch && at.rd == BCMJOB_OK; y += band)
        {
            uint32_t h = ch - y < band ? ch - y : band;

            at.rd = bcmjob_read(at.res.out[k].slot, 0, y, cw, h, RT[k] + y * cw, cw);
            for (rr = 0; rr < h; rr++)
                j_f16w(&at.a[k], &at.b[k], RT[k] + (y + rr) * cw, cw);
        }
    }
    pthread_mutex_lock(&tmx);
    art_done = true;
    turn = 0;
    pthread_cond_broadcast(&tcv);
    pthread_mutex_unlock(&tmx);
    return NULL;
}

/* The owner's loop: a pass every gap_us (none if !passes), with a frame
 * due on the passes due() says, an event at pass ev_at; then the art
 * thread's turn.  Returns the passes run. */
static uint32_t run_threads(uint32_t gap_us, bool passes, bool (*due)(uint32_t pass),
                            uint32_t ev_at, void (*ev)(void))
{
    uint32_t pass = 0, t;

    owner_tid = pthread_self();
    art_done = false;
    max_pass_us = 0;
    turn = 0;
    if (pthread_create(&art_tid, NULL, art_main, NULL))
        abort();
    art_running = true;
    while (!art_done && pass < 200000)
    {
        turn_to(1, 0);          /* the art thread until it waits */
        if (art_done)
            break;
        now_us_ += gap_us;
        chip_run();             /* the BCM's own time */
        pass++;
        if (ev && pass == ev_at)
            ev();
        if (ev_every)
            ev_every(pass);
        due_now = due && due(pass);
        if (passes && stop_after && pass > stop_after)
            passes = false;     /* the owner stops answering */
        if (passes && kick_only)
        {
            /* the owner's queue: only a kick brings a pass; a loop that
               swallows SYS_BCMJOB_REQ drops it */
            bool k = owner_kicked;

            owner_kicked = false;
            if (k && drop_kicks && tq.o_take == tq.a_seq)
                drop_kicks--;
            else if (k)
            {
                t = now_us_;
                bjc_apply();
                if (now_us_ - t > max_pass_us)
                    max_pass_us = now_us_ - t;
            }
        }
        else if (passes)
        {
            owner_kicked = false;
            t = now_us_;
            bjc_apply();        /* the owner's queue: SYS_BCMJOB_REQ */
            if (now_us_ - t > max_pass_us)
                max_pass_us = now_us_ - t;
        }
        due_now = false;
    }
    pthread_join(art_tid, NULL);
    art_running = false;
    return pass;
}

static bool t_due(void) { return due_now; }

static void thread_fresh(void)
{
    dec_fresh();
    memset(&tq, 0, sizeof(tq));
    owner_tid = pthread_self();
    bjc_set_owner(&towner);
    bjc_frame_due(t_due);
    ev_every = NULL;
    kick_only = false;
    drop_kicks = stop_after = 0;
    memset(&at, 0, sizeof(at));
    due_writes = due_reads = art_port = 0;
}

static bool due_odd(uint32_t pass) { return pass & 1; }
static bool due_mostly(uint32_t pass) { return pass % 4 != 0; }
static void ev_cut(void) { p_power(false); }
static void ev_restart(void) { p_restart(); }
static void ev_suspend(void) { bjc_suspend_request(true, 0); }

/* The art thread's outputs equal the twin's, their sums the BCM's */
static int art_same(const uint8_t *p, uint32_t len)
{
    int k;

    if (twin(p, len, at.o, at.n) != JDEC_OK)
        return 0;
    for (k = 0; k < at.n; k++)
        if (at.a[k] != at.res.out[k].a || at.b[k] != at.res.out[k].b ||
            memcmp(RT[k], TW[k], 2u * at.o[k].cw * at.o[k].ch))
            return 0;
    return 1;
}

/* Owner decision: the owner's calls during a DECODE answer BUSY, never
 * counted; the calls that don't touch the BCM still work */
static int dur_slot, dur_fill, dur_fence, dur_resync, dur_upload;
static bool dur_have, dur_epoch;
static uint32_t dur_e0, dur_seq0;
static int dur_present, dur_wait;
static void ev_owner_calls(void)
{
    static uint16_t px[8 * 8];
    uint32_t fill[4] = { J_SURF_BACK, 0, J_XY(8, 8), 0x1F }, seq;
    uint32_t pres[4] = { J_SURF_FRONT, 0, J_XY(W, H), 0 };
    struct bcmjob_slot sl;

    int k;

    for (k = 0; k < 2; k++)     /* twice: a counted BUSY would mark them */
    {
        dur_slot = bcmjob_slot(J_CLS_THUMB, 8, 8, 0x99, 0, &sl);
        dur_fill = bcmjob_submit(J_OP_FILL, 0, fill, 4);
    }
    dur_fence = bcmjob_fence(&seq);
    dur_resync = bcmjob_resync(0, 0, W, H);
    sl.slot = 0;
    sl.hit = false;
    sl.epoch = bcmjob_epoch();
    dur_upload = bcmjob_upload(&sl, px);
    dur_present = bcmjob_submit(J_OP_PRESENT, 0, pres, 4);
    dur_wait = bcmjob_wait(dur_seq0, 50);
    dur_have = bcmjob_have(J_OP_FILL);
    dur_epoch = bcmjob_epoch() == dur_e0;
}

/* re-review 2's owner: after the output ALLOC ran (its reply not yet
   collected), a SCROLL and a PRESENT on FRONT; two passes later (the
   record in), a resync */
static int win_state, win_resync;
static bool win_held;
/* a DECODE record on the command ring the chip hasn't taken yet */
static bool dec_live_now(void)
{
    uint32_t rd = *w32(HP_CMD_RD) & 0xFFFF, wr = *w32(HP_CMD_WR) & 0xFFFF;

    while (rd != wr)
    {
        uint32_t h = *w32(HP_CMD_RING + 4 * (rd & (cmd_words - 1)));

        if (HP_REC_OP(h) == J_OP_DECODE)
            return true;
        rd = (rd + 1 + HP_REC_LEN(h)) & 0xFFFF;
    }
    return dj.on;
}
static uint32_t win_us, win_pass;
static void ev_window(uint32_t pass)
{
    uint32_t scroll[4] = { J_SURF_FRONT, J_XY(0, 40), J_XY(W, 160), (uint32_t)-16 & 0xFFFF };
    uint32_t pres[4] = { J_SURF_FRONT, J_XY(0, 0), J_XY(W, H), 0 }, t;

    if (win_state == 0 && hold_at)  /* the output ALLOC posted, not run */
    {
        if (bcmjob_submit(J_OP_SCROLL, 1, scroll, 4) == BCMJOB_OK &&
            bcmjob_submit(J_OP_PRESENT, 2, pres, 4) == BCMJOB_OK)
            win_state = 1;
        else
            win_state = -1;
        win_pass = pass;
    }
    else if (win_state == 1 && !hold_at)    /* the ALLOC ran */
    {
        win_state = 2;
        win_pass = pass;
    }
    else if (win_state == 2 && pass >= win_pass + 2)    /* the record in */
    {
        win_held = dec_live_now();
        t = now_us_;
        win_resync = bcmjob_resync(0, 0, W, H);
        win_us = now_us_ - t;
        win_state = 3;
    }
}

/* re-review 4: SYS_USB_CONNECTED reaching the owner's default handler */
static uint32_t usb_at;
static void ev_usb(void)
{
    usb_at = now_us_;
    bjc_decode_stop();
}

static void t_decode_threads(void)
{
    struct bcmjob_dec_out o[2];
    uint32_t passes, k, t0;
    int r;

    /* the art thread's decode and read-back, every wire step the owner's;
       a frame due on every other pass: no piece written, no row read then */
    thread_fresh();
    o[0] = dout(142, 80, 0, 0, 142, 80, 0x8001);
    at.p = big; at.len = big_len; at.o = o; at.n = 1;
    e2_pieces = 0;
    passes = run_threads(1000, true, due_odd, 0, NULL);
    CHECK(at.r == BCMJOB_OK && at.rd == BCMJOB_OK && at.res.passes == 2 &&
          at.res.bytes == 2 * big_len && art_same(big, big_len),
          "threads: %d read %d, passes %u bytes %u", at.r, at.rd, at.res.passes,
          at.res.bytes);
    CHECK(!art_port && !due_writes && !due_reads && !ring_unlocked && lock_depth == 0,
          "threads: art port %u, due writes %u, due reads %u", art_port, due_writes,
          due_reads);
    /* one piece a pass at most: 26 pieces over two passes of the file */
    CHECK(passes >= 2 * ((big_len + BCMJOB_DEC_PIECE - 1) / BCMJOB_DEC_PIECE),
          "one piece a pass: %u passes for %u pieces", passes, e2_pieces);
    printf("info: threads: %u owner passes, %u engine-2 pieces, %u art waits\n",
           passes, e2_pieces, art_waits);
    /* the owner's own calls mid-decode: BUSY ("busy-decode"), never
       counted; have and epoch still answer; all of them work after */
    thread_fresh();
    nlines = 0;
    o[0] = dout(142, 142, 0, 0, 142, 142, 0x8010);
    at.p = big; at.len = big_len; at.o = o; at.n = 1;
    dur_e0 = bcmjob_epoch();
    CHECK(bcmjob_fence(&dur_seq0) == BCMJOB_OK, "a fence before the decode");
    run_threads(1000, true, NULL, 10, ev_owner_calls);
    CHECK(at.r == BCMJOB_OK && art_same(big, big_len), "the decode beside them: %d", at.r);
    CHECK(dur_slot == BCMJOB_BUSY && dur_fill == BCMJOB_BUSY && dur_fence == BCMJOB_BUSY &&
          dur_resync == BCMJOB_OK && dur_upload == BCMJOB_BUSY && dur_have && dur_epoch &&
          dur_present == BCMJOB_BUSY && dur_wait == BCMJOB_OK &&
          logged("bcmjob fault op=0x21 why=busy-decode") &&
          logged("bcmjob fault op=0x25 why=busy-decode"),
          "owner calls mid-decode: slot %d fill %d fence %d resync %d upload %d", dur_slot,
          dur_fill, dur_fence, dur_resync, dur_upload);
    {
        struct bcmjob_slot sl;
        uint32_t seq;

        CHECK(bcmjob_have(J_OP_ALLOC) && bcmjob_have(J_OP_COMMIT) && bcmjob_have(J_OP_FILL) &&
              bcmjob_have(J_OP_FENCE) && bcmjob_have(J_OP_DECODE) &&
              !logged("why=timeout") && !logged("why=failed"),
              "nothing marked slow or failed through busy-decode");
        CHECK(bcmjob_slot(J_CLS_THUMB, 8, 8, 0x99, 0, &sl) == BCMJOB_OK &&
              bcmjob_fence(&seq) == BCMJOB_OK && bcmjob_wait(seq, 100) == BCMJOB_OK &&
              bcmjob_resync(0, 0, W, H) == BCMJOB_OK, "after the decode they work");
    }

    /* a frame due on 3 passes of 4: slower, still exact */
    thread_fresh();
    o[0] = dout(142, 142, 0, 0, 142, 142, 0x8002);
    at.p = big; at.len = big_len; at.o = o; at.n = 1; at.ms = 20000;
    run_threads(1000, true, due_mostly, 0, NULL);
    CHECK(at.r == BCMJOB_OK && !due_writes && art_same(big, big_len),
          "frames mostly due: %d, due writes %u", at.r, due_writes);

    /* a slow owner (a pass every 200 ms): still the right answer */
    thread_fresh();
    o[0] = dout(142, 142, 0, 0, 142, 142, 0x8003);
    at.p = jpg; at.len = jpg_len; at.o = o; at.n = 1;
    run_threads(200000, true, NULL, 0, NULL);
    CHECK(at.r == BCMJOB_OK && art_same(jpg, jpg_len), "a slow owner: %d", at.r);
    thread_fresh();
    o[0] = dout(142, 142, 0, 0, 142, 142, 0x8004);
    at.p = big; at.len = big_len; at.o = o; at.n = 1; at.ms = 30000;
    run_threads(100000, true, NULL, 0, NULL);
    CHECK(at.r == BCMJOB_OK && !at.res.stalled && art_same(big, big_len),
          "a slow owner, a long file: %d stalled %d", at.r, at.res.stalled);

    /* every end releases the art thread: a cancel (ABORTED), the deadline,
       a cut, a restart, a suspend (the CPU path's verdict) */
    thread_fresh();
    o[0] = dout(142, 142, 0, 0, 142, 142, 0x8100);
    at.p = big; at.len = big_len; at.o = o; at.n = 1; at.cancel_at = 3;
    t0 = now_us_;
    run_threads(1000, true, NULL, 0, NULL);
    settle();
    CHECK(at.r == BCMJOB_ABORTED && bcmjob_dec_verdict(at.r, &at.res) == BCMJOB_DEC_DEFER &&
          slot_state(at.res.out[0].slot) == J_ST_EMPTY && now_us_ - t0 < 200000,
          "threads: a cancel: %d, %u us", at.r, now_us_ - t0);
    thread_fresh();
    dec_hang = true;
    o[0] = dout(142, 142, 0, 0, 142, 142, 0x8101);
    at.p = jpg; at.len = jpg_len; at.o = o; at.n = 1; at.ms = 30;
    run_threads(1000, true, NULL, 0, NULL);
    dec_hang = false;
    CHECK(at.r == BCMJOB_TIMEOUT && bcmjob_dec_verdict(at.r, &at.res) == BCMJOB_DEC_TWIN,
          "threads: the deadline: %d", at.r);
    {
        static void (*const evs[])(void) = { ev_cut, ev_restart, ev_suspend };

        for (k = 0; k < 3; k++)
        {
            thread_fresh();
            o[0] = dout(142, 142, 0, 0, 142, 142, 0x8200 + k);
            at.p = big; at.len = big_len; at.o = o; at.n = 1;
            run_threads(1000, true, NULL, 12, evs[k]);
            CHECK(at.r == BCMJOB_OFF && bcmjob_dec_verdict(at.r, &at.res) == BCMJOB_DEC_DEFER,
                  "threads: end %u mid-decode: %d", k, at.r);
            bjc_suspend_request(false, 0);
        }
    }
    /* a kick swallowed after the decode was taken (a modal loop, the USB
       screen, a cleared queue): posted again when stale, the decode ends
       as usual */
    thread_fresh();
    kick_only = true;
    drop_kicks = 1;
    o[0] = dout(142, 142, 0, 0, 142, 142, 0x8400);
    at.p = big; at.len = big_len; at.o = o; at.n = 1;
    t0 = now_us_;
    run_threads(1000, true, NULL, 0, NULL);
    CHECK(at.r == BCMJOB_OK && drop_kicks == 0 && now_us_ - t0 < 2000000 &&
          art_same(big, big_len), "a dropped kick: %d after %u us", at.r, now_us_ - t0);

    /* the owner stops answering after taking it: the caller leaves at
       its own bound (TIMEOUT, the CPU path), never hangs; the owner ends
       it unseen at its next pass, and the next call goes */
    thread_fresh();
    stop_after = 5;
    o[0] = dout(142, 142, 0, 0, 142, 142, 0x8410);
    at.p = big; at.len = big_len; at.o = o; at.n = 1; at.ms = 200;
    t0 = now_us_;
    run_threads(1000, true, NULL, 0, NULL);
    t0 = now_us_ - t0;
    CHECK(at.r == BCMJOB_TIMEOUT && bcmjob_dec_verdict(at.r, &at.res) == BCMJOB_DEC_TWIN &&
          t0 >= 200000 && t0 < 200000 + 3000000 + 50000 && tq.a_quit == tq.a_seq,
          "the owner gone: %d after %u us", at.r, t0);
    bjc_apply();                /* the owner back: it ends the left call */
    CHECK(tq.o_seq == tq.a_seq && logged("bcmjob decode done st=-4"), "ended unseen");
    settle();
    stop_after = 0;
    o[0] = dout(142, 142, 0, 0, 142, 142, 0x8411);
    at.ms = 0;
    memset(&at.res, 0, sizeof(at.res));
    run_threads(1000, true, NULL, 0, NULL);
    CHECK(at.r == BCMJOB_OK && art_same(big, big_len), "the next decode: %d", at.r);

    /* a COMMIT that takes 30 ms on the BCM: collected in a later pass,
       no pass waits for it */
    thread_fresh();
    commit_hold_us = 30000;     /* inside its deadline (45 ms for 142 x 142) */
    o[0] = dout(142, 142, 0, 0, 142, 142, 0x8420);
    at.p = jpg; at.len = jpg_len; at.o = o; at.n = 1;
    run_threads(1000, true, NULL, 0, NULL);
    commit_hold_us = 0;
    CHECK(at.r == BCMJOB_OK && max_pass_us < 2000 && art_same(jpg, jpg_len),
          "a 30 ms COMMIT: %d, the longest pass %u us", at.r, max_pass_us);

    /* re-review 1: the caller left and its leave kick was dropped; with no
       bjc_apply by hand, the owner's own next call ends the left call,
       and nothing stays busy-decode */
    thread_fresh();
    kick_only = true;
    stop_after = 5;
    o[0] = dout(142, 142, 0, 0, 142, 142, 0x8500);
    at.p = big; at.len = big_len; at.o = o; at.n = 1; at.ms = 200;
    run_threads(1000, true, NULL, 0, NULL);
    owner_kicked = false;       /* the leave kick: swallowed */
    stop_after = 0;
    CHECK(at.r == BCMJOB_TIMEOUT && tq.o_seq != tq.a_seq, "left: %d", at.r);
    {
        uint32_t fill[4] = { J_SURF_BACK, 0, J_XY(8, 8), 0x1F }, tries;

        for (tries = 0, r = BCMJOB_BUSY; r == BCMJOB_BUSY && tries < 100; tries++)
        {
            r = bcmjob_submit(J_OP_FILL, 0, fill, 4);
            if (r == BCMJOB_BUSY)
                p_yield();
        }
        CHECK(r == BCMJOB_OK && tq.o_seq == tq.a_seq && tries < 50,
              "a dropped leave kick: %d after %u tries", r, tries);
    }
    settle();

    /* re-review 2: a resync in the window between the record and the
       jobs queued ahead of it (a SCROLL and a PRESENT the owner put
       between the ALLOC's reply and its next pass; the SCROLL takes
       30 ms): it waits for them, then pushes; FRONT stays the PP's */
    thread_fresh();
    o[0] = dout(55, 55, 0, 0, 55, 55, 0x85F0);
    at.p = jpg; at.len = jpg_len; at.o = o; at.n = 1;
    run_threads(1000, true, NULL, 0, NULL);     /* the ring this epoch */
    hold_op = J_OP_ALLOC;                       /* the output's: 10 ms */
    hold_us = 10000;
    hold2_op = J_OP_SCROLL;
    hold2_us = 30000;
    win_state = 0;
    ev_every = ev_window;
    o[0] = dout(142, 142, 0, 0, 142, 142, 0x8600);
    memset(&at.res, 0, sizeof(at.res));
    nlines = 0;
    run_threads(1000, true, NULL, 0, NULL);
    ev_every = NULL;
    settle();
    CHECK(at.r == BCMJOB_OK && win_state == 3 && win_resync == BCMJOB_OK &&
          win_held && win_us >= 10000 && win_us < 60000 && !logged("bcmjob resync forced") &&
          !memcmp(mem + FRONT, ppfb, sizeof(ppfb)),
          "a resync in the window: %d after %u us (state %d), decode %d", win_resync,
          win_us, win_state, at.r);

    /* re-review 3: a decode never frees a hit output; a late ALLOC hit is
       not freed */
    thread_fresh();
    hit_frees = 0;
    o[0] = dout(55, 55, 0, 0, 55, 55, 0x8700);
    o[1] = dout(48, 40, 4, 0, 40, 40, 0x8701);
    at.p = jpg; at.len = jpg_len; at.o = o; at.n = 1;
    run_threads(1000, true, NULL, 0, NULL);     /* o[0] READY: the cache's */
    CHECK(at.r == BCMJOB_OK, "the held slot: %d", at.r);
    k = at.res.out[0].slot;
    memset(&at.res, 0, sizeof(at.res));
    at.p = bad[1]; at.n = 2;                    /* a hit and a miss, CORRUPT */
    run_threads(1000, true, NULL, 0, NULL);
    settle();
    CHECK(at.r == BCMJOB_FAILED && at.res.out[0].hit && at.res.out[0].slot == k &&
          slot_state(k) == J_ST_READY && !hit_frees &&
          slot_state(at.res.out[1].slot) == J_ST_EMPTY,
          "a failed decode with a hit output: %d, hit frees %u", at.r, hit_frees);
    {
        struct bcmjob_slot sl;

        slow_until = now_us_ + 40000;           /* the ALLOC's reply late */
        r = bcmjob_slot(J_CLS_THUMB, 55, 55, 0x8700, 0xA1000000u, &sl);
        slow_until = 0;
        settle();
        CHECK(r == BCMJOB_TIMEOUT && slot_state(k) == J_ST_READY && !hit_frees,
              "a late ALLOC hit: %d, slot %d, hit frees %u", r, slot_state(k), hit_frees);
    }

    /* re-review 4: USB (or a shutdown) mid-decode: the call ends at once,
       ABORTED (DEFER), the art thread free within 200 ms; a hit output
       stays; and a call posted but not taken yet ends the same */
    thread_fresh();
    hit_frees = 0;
    o[0] = dout(55, 55, 0, 0, 55, 55, 0x8800);
    at.p = big; at.len = big_len; at.o = o; at.n = 1;
    run_threads(1000, true, NULL, 0, NULL);     /* o[0] READY */
    k = at.res.out[0].slot;
    o[1] = dout(48, 40, 4, 0, 40, 40, 0x8801);
    memset(&at.res, 0, sizeof(at.res));
    at.n = 2;
    usb_at = 0;
    run_threads(1000, true, NULL, 10, ev_usb);
    settle();
    CHECK(at.r == BCMJOB_ABORTED && bcmjob_dec_verdict(at.r, &at.res) == BCMJOB_DEC_DEFER &&
          now_us_ - usb_at <= 200000 && slot_state(k) == J_ST_READY && !hit_frees,
          "USB mid-decode: %d, %u us after it", at.r, now_us_ - usb_at);
    thread_fresh();
    o[0] = dout(142, 142, 0, 0, 142, 142, 0x8802);
    at.p = jpg; at.len = jpg_len; at.o = o; at.n = 1;
    usb_at = 0;
    run_threads(1000, false, NULL, 3, ev_usb);  /* the owner never passed */
    CHECK(at.r == BCMJOB_ABORTED && now_us_ - usb_at <= 200000,
          "USB with the call posted: %d", at.r);
    /* the cap: q->ms == 0 and the owner gone: the caller leaves at the
       cap plus its margin, not 14 s */
    thread_fresh();
    stop_after = 3;
    o[0] = dout(142, 142, 0, 0, 142, 142, 0x8803);
    at.p = big; at.len = big_len; at.o = o; at.n = 1; at.ms = 0;
    t0 = now_us_;
    run_threads(1000, true, NULL, 0, NULL);
    t0 = now_us_ - t0;
    stop_after = 0;
    CHECK(at.r == BCMJOB_TIMEOUT && t0 >= 1000u * BCMJOB_DEC_CAP_MS &&
          t0 <= 1000u * (BCMJOB_DEC_CAP_MS + 1000) + 50000,
          "the caller's bound with ms 0: %u us", t0);
    bjc_apply();
    settle();

    /* an owner that never runs: the call is withdrawn, OFF */
    thread_fresh();
    o[0] = dout(142, 142, 0, 0, 142, 142, 0x8300);
    at.p = jpg; at.len = jpg_len; at.o = o; at.n = 1;
    run_threads(10000, false, NULL, 0, NULL);
    CHECK(at.r == BCMJOB_OFF && tq.a_quit == tq.a_seq && tq.o_take != tq.a_seq,
          "no owner: %d", at.r);
    /* the next call goes */
    o[0] = dout(142, 142, 0, 0, 142, 142, 0x8301);
    memset(&at.res, 0, sizeof(at.res));
    at.o = o;
    run_threads(1000, true, NULL, 0, NULL);
    CHECK(at.r == BCMJOB_OK && art_same(jpg, jpg_len), "after a withdrawal: %d", at.r);
    bjc_set_owner(NULL);
}

/* ---- S10: the hand-off across cores ----------------------------------------
 * The caller (a pthread) and the owner (main) as two cores, each with its
 * own data cache, modelled for the two buffers the owner touches:
 *  - each buffer has the caller's view (the real array: what its core's
 *    cache holds) and the memory behind it (a second array);
 *  - publish writes the caller's whole lines of a range back to memory;
 *    acquire writes back the lines another thread of the caller's core
 *    dirtied meanwhile (whole lines, as a cache does), then reloads every
 *    line of the range from memory;
 *  - the owner's alias (uncached) is the memory; engine 2 (upload_any)
 *    reads memory; the owner's CPU reading the caller's cached address
 *    (wr_block of the view) is a fault, served stale.
 * A hand-off that skips a step leaves the wrong bytes, so the twin's
 * pixels check every one; the counters say which.  The two threads take a
 * baton at random points: every BJC_FENCE(), every port call, every hook,
 * so each give-up lands at random against the owner's take, its pieces
 * and its end.  Time jumps on the caller's side force the give-ups. */
static uint32_t xc_rand(void)
{
    xc.rnd ^= xc.rnd << 13;
    xc.rnd ^= xc.rnd >> 17;
    xc.rnd ^= xc.rnd << 5;
    return xc.rnd;
}

/* The caller is waiting for its acknowledgement: no time jumps then (a
 * pass holds it a row's time, not seconds; the frozen owner has its own
 * case) */
static bool x_giving_up(void)
{
    return xq.a_seq && xq.a_quit == xq.a_seq && xq.o_seq != xq.a_seq;
}

static int xc_me(void)
{
    return pthread_equal(pthread_self(), x_caller) ? 1 : 0;
}

/* Hand the baton to the other side and wait for it back */
static void xc_switch(int me)
{
    pthread_mutex_lock(&xmx);
    if (xc.done)
    {
        pthread_mutex_unlock(&xmx);
        return;                 /* the caller is gone: the owner runs on */
    }
    xc.turn = !me;
    xc.switches++;
    pthread_cond_broadcast(&xcv);
    while (xc.turn != me)
        pthread_cond_wait(&xcv, &xmx);
    pthread_mutex_unlock(&xmx);
}

static void xc_point(void)
{
    int me;

    if (!xc.on || xc.done)
        return;
    me = xc_me();
    xc.points++;
    if (me == 1 && xc.jump_in && !x_giving_up() && xc_rand() % xc.jump_in == 0)
        now_us_ += xc.jump_us;  /* the caller's bound passes here */
    if (me == 1 && xc.stalled)
        return;                 /* the owner is frozen: nobody to hand to */
    if (me == 0 && xc.stall_arm && xq.o_busy && xq.o_busy == xq.a_seq &&
        xq.o_take == xq.a_seq)
    {
        /* frozen inside a pass of the call until the caller returned */
        xc.stall_arm = false;
        xc.stalled = true;
        xc_switch(0);
        return;
    }
    if (xc_rand() % xc.one_in == 0)
        xc_switch(me);
}

void bjc_test_fence(void)
{
    __sync_synchronize();
    xc_point();
}

static bool x_cross(void) { return xc.model; }

static void x_publish(const void *p, uint32_t n)
{
    uintptr_t a, e;

    xc.publishes++;
    if (xc_me() != 1)
        xc.wrong_core++;
    xc_point();
    if (xc.drop_publish)
        return;
    a = (uintptr_t)p & ~(uintptr_t)15;
    e = ((uintptr_t)p + n + 15) & ~(uintptr_t)15;
    for (; a < e; a += 16)
        if (in_buf((void *)a, piece_mem, sizeof(piece_mem)))
            memcpy(piece_ram + (a - (uintptr_t)piece_mem), (void *)a, 16);
        else if (in_buf((void *)a, pxv, sizeof(pxv)))
        {
            memcpy(pxr + (a - (uintptr_t)pxv), (void *)a, 16);
            px_dirty[(a - (uintptr_t)pxv) / 16] = false;
        }
        else
            xc.unmodeled++;
}

static void x_acquire(void *p, uint32_t n)
{
    uintptr_t a, e;

    xc.acquires++;
    if (xc_me() != 1)
        xc.wrong_core++;
    xc_point();
    if (xc.drop_acquire)
        return;
    a = (uintptr_t)p & ~(uintptr_t)15;
    e = ((uintptr_t)p + n + 15) & ~(uintptr_t)15;
    for (; a < e; a += 16)
        if (in_buf((void *)a, pxv, sizeof(pxv)))
        {
            uint32_t o = a - (uintptr_t)pxv;

            if (px_dirty[o / 16])
                memcpy(pxr + o, pxv + o, 16);   /* the dirty line first */
            memcpy(pxv + o, pxr + o, 16);
            px_dirty[o / 16] = false;
        }
        else
            xc.unmodeled++;
}

static void *x_uncached(void *p)
{
    xc.aliases++;
    if (xc_me() != 0)
        xc.wrong_core++;
    xc_point();
    if (in_buf(p, piece_mem, sizeof(piece_mem)))
    {
        if (!xc.piece_live)
            xc.dead_reads++;
        return piece_ram + ((uint8_t *)p - piece_mem);
    }
    if (in_buf(p, pxv, sizeof(pxv)))
        return pxr + ((uint8_t *)p - pxv);
    xc.unmodeled++;
    return p;
}

/* The caller's wait: time passes, another thread of its core writes the
 * neighbours of px's span (its edge lines, dirty now), the owner runs */
static void x_art_wait(uint32_t ms)
{
    if (!xq.o_busy || xc.stalled)
        now_us_ += 1000u * ms;  /* the wait's time is the owner's pass's
                                   own when it is inside one */
    if (xc.jump_in && !xc.stalled && !x_giving_up() && xc_rand() % xc.jump_in == 0)
        now_us_ += xc.jump_us;  /* a bound passes between the owner's passes */
    if (xq.a_quit && xq.a_quit == xq.a_seq && xq.o_busy == xq.a_seq)
        xc.ack_waits++;         /* a give-up waiting on a pass */
    if (xc.nb_lo || xc.nb_hi)
    {
        xc.nb_val++;
        if (xc.nb_lo)
        {
            pxv[xc.nb_lo] = xc.nb_val;
            px_dirty[xc.nb_lo / 16] = true;
        }
        if (xc.nb_hi)
        {
            pxv[xc.nb_hi] = xc.nb_val;
            px_dirty[xc.nb_hi / 16] = true;
        }
    }
    if (!xc.stalled)
        xc_switch(1);
}

static void x_kick(void) { }
static bool x_is_owner(void) { return xc_me() == 0; }
static void x_art_wake(void) { }

static const struct bjc_owner xowner = {
    &xq, x_is_owner, x_kick, NULL, NULL, NULL, x_art_wait, x_art_wake,
    x_cross, x_publish, x_acquire, x_uncached,
};

/* The caller's scenario: a decode (res in a canary frame), then (OK) the
 * output read back whole into px (odd offset and pitch: both edge lines
 * shared with neighbours), checked against the twin */
static struct
{
    const uint8_t *p;
    uint32_t len, ms;
    struct bcmjob_dec_out o;
    int r, rd, busy2;
    bool px_ok, dec_quit, rd_quit;  /* the call was given up on */
    uint32_t px_off, pitch, span;
    struct bcmjob_dec_res res;  /* copied out of the frame */
    struct { uint32_t a[8]; struct bcmjob_dec_res res; uint32_t b[8]; } fr;
    struct tsrc ts;
} xs;

static void *x_main(void *x)
{
    struct jdec_src src = ts_src(&xs.ts, xs.p, xs.len);
    struct bcmjob_dec_req q;

    (void)x;
    pthread_mutex_lock(&xmx);
    while (xc.turn != 1)
        pthread_cond_wait(&xcv, &xmx);
    pthread_mutex_unlock(&xmx);
    memset(&q, 0, sizeof(q));
    q.src = &src;
    q.out = &xs.o;
    q.n = 1;
    q.ms = xs.ms;
    xs.r = bcmjob_decode(&q, &xs.fr.res);
    xs.dec_quit = xq.a_quit && xq.a_quit == xq.a_seq;
    xs.res = xs.fr.res;
    if (xc.stalled)
    {
        struct bcmjob_dec_res tmp;

        xs.busy2 = bcmjob_decode(&q, &tmp);     /* still frozen: BUSY */
    }
    memset(&xs.fr.res, 0x5A, sizeof(xs.fr.res));    /* the frame is dead */
    xs.rd = -100;
    xs.px_ok = false;
    if (xs.r == BCMJOB_OK)
    {
        uint16_t *px = (uint16_t *)(void *)(pxv + xs.px_off);
        uint32_t y;

        xs.rd = bcmjob_read(xs.res.out[0].slot, 0, 0, xs.o.cw, xs.o.ch, px, xs.pitch);
        xs.rd_quit = xq.a_quit && xq.a_quit == xq.a_seq;
        if (xs.rd == BCMJOB_OK)
        {
            xs.px_ok = true;
            for (y = 0; y < xs.o.ch; y++)
                if (memcmp(px + y * xs.pitch, TW[0] + y * xs.o.cw, 2u * xs.o.cw))
                    xs.px_ok = false;
        }
        /* the rows are the caller's again: anything the owner writes
           from here lands in memory over this */
        memset(pxv + xs.px_off, 0x77, xs.span);
        memset(pxr + xs.px_off, 0x77, xs.span);
    }
    pthread_mutex_lock(&xmx);
    xc.done = true;
    xc.stalled = false;
    xc.turn = 0;
    pthread_cond_broadcast(&xcv);
    pthread_mutex_unlock(&xmx);
    return NULL;
}

/* One run: the caller's scenario against the owner's passes.  Then the
 * owner runs on until idle, and every canary is checked */
static void x_run(const char *what, uint32_t gap_us)
{
    uint32_t pass = 0, k, late = 0;
    bool frame_ok = true;

    xc.done = false;
    xc.turn = 0;
    xc.on = true;
    if (pthread_create(&x_caller, NULL, x_main, NULL))
        abort();
    art_running = true;
    art_tid = x_caller;
    while (!xc.done && pass < 400000)
    {
        xc_switch(0);
        if (xc.done)
            break;
        now_us_ += gap_us;
        chip_run();
        pass++;
        if (pass > xc.owner_off)
            bjc_apply();
    }
    pthread_join(x_caller, NULL);
    art_running = false;
    xc.on = false;
    /* the owner on alone: whatever it still runs must leave the caller's
       memory alone */
    for (k = 0; k < 4000 && (xq.o_seq != xq.a_seq || xq.o_busy); k++)
    {
        now_us_ += 1000;
        chip_run();
        bjc_apply();
    }
    bjc_apply();
    for (k = 0; k < xs.span; k++)
        late += pxr[xs.px_off + k] != 0x77 && xs.r == BCMJOB_OK;
    for (k = 0; k < 8; k++)
        frame_ok = frame_ok && xs.fr.a[k] == 0xA5A5A5A5u && xs.fr.b[k] == 0xA5A5A5A5u;
    {
        const uint8_t *z = (const uint8_t *)&xs.fr.res;

        for (k = 0; k < sizeof(xs.fr.res); k++)
            frame_ok = frame_ok && z[k] == 0x5A;
    }
    CHECK(frame_ok && !late && !xc.dead_reads && !xc.cached_reads && !xc.unmodeled &&
          !xc.dma_alias && !xc.wrong_core && !art_port && lock_depth == 0 &&
          xq.o_seq == xq.a_seq,
          "%s: frame %d, late px writes %u, dead reads %u, cached reads %u, "
          "unmodeled %u, dma alias %u, wrong core %u, art port %u, idle %d",
          what, frame_ok, late, xc.dead_reads, xc.cached_reads, xc.unmodeled,
          xc.dma_alias, xc.wrong_core, art_port, xq.o_seq == xq.a_seq);
}

/* A fresh run's set-up: the chip, the owner, the buffers and canaries */
static void x_fresh(const uint8_t *p, uint32_t len, uint32_t ms, uint32_t key)
{
    uint32_t k;

    dec_fresh();
    memset(&xq, 0, sizeof(xq));
    bjc_set_owner(&xowner);
    bjc_frame_due(NULL);
    xc.done = xc.stalled = xc.stall_arm = false;
    xc.owner_off = 0;
    xc.dead_reads = xc.cached_reads = xc.unmodeled = xc.dma_alias = 0;
    xc.wrong_core = 0;
    art_port = 0;
    memset(&xs, 0, sizeof(xs));
    xs.p = p;
    xs.len = len;
    xs.ms = ms;
    xs.o = dout(48, 40, 0, 0, 48, 40, key);
    xs.px_off = 16 * 3 + 6;     /* mid-line: the first edge line shared */
    xs.pitch = xs.o.cw + 3;     /* the end mid-line too */
    xs.span = 2 * ((xs.o.ch - 1) * xs.pitch + xs.o.cw);
    for (k = 0; k < 8; k++)
        xs.fr.a[k] = xs.fr.b[k] = 0xA5A5A5A5u;
    for (k = 0; k < sizeof(pxv); k++)
        pxv[k] = pxr[k] = (uint8_t)(k * 7);
    memset(px_dirty, 0, sizeof(px_dirty));
    memcpy(piece_ram, piece_mem, sizeof(piece_ram));
    /* the neighbours in the span's edge lines */
    xc.nb_lo = xs.px_off - 1;
    xc.nb_hi = xs.px_off + xs.span;
    xc.nb_val = 0;
}

static bool x_neighbours_ok(void)
{
    return pxv[xc.nb_lo] == xc.nb_val && pxv[xc.nb_hi] == xc.nb_val;
}

static void t_cross_core(void)
{
    uint32_t i, n_ok = 0, n_left = 0, n_off = 0, n_raced = 0, n_read_left = 0;
    uint32_t pub0, acq0, served0;

    CHECK(twin(jpg, jpg_len, &(struct bcmjob_dec_out){ 48, 40, 0, 0, 48, 40, 0, 0, 0 }, 1)
          == JDEC_OK, "the twin of the cross-core output");

    /* 1. Nothing goes wrong: across cores, the pixels and the edges right
       through the model, every piece published, every read acquired */
    x_fresh(big, big_len, 0, 0x9001);
    xc.model = true;
    xc.one_in = 3;
    xc.rnd = 12345;
    xc.jump_in = 0;
    pub0 = xc.publishes;
    acq0 = xc.acquires;
    x_run("cross: plain", 1000);
    CHECK(xs.r == BCMJOB_OK && xs.rd == BCMJOB_OK && xs.px_ok && x_neighbours_ok() &&
          xc.publishes - pub0 >= xs.ts.fetches + 1 && xc.acquires - acq0 == 1 &&
          xc.aliases > 0,
          "cross: decode %d read %d px %d neighbours %d, %u publishes for %u pieces, "
          "%u acquires", xs.r, xs.rd, xs.px_ok, x_neighbours_ok(),
          xc.publishes - pub0, xs.ts.fetches, xc.acquires - acq0);
    printf("info: cross: %u schedule points, %u switches, %u pieces, %u aliases\n",
           xc.points, xc.switches, xs.ts.fetches, xc.aliases);

    /* 2. The negative controls: the model sees a hand-off left out */
    x_fresh(jpg, jpg_len, 0, 0x9002);
    xc.drop_publish = true;
    x_run("control: no publish", 1000);
    xc.drop_publish = false;
    CHECK(xs.r != BCMJOB_OK || xs.rd != BCMJOB_OK || !xs.px_ok,
          "without publish the model must fail it: %d %d %d", xs.r, xs.rd, xs.px_ok);
    x_fresh(jpg, jpg_len, 0, 0x9003);
    xc.drop_acquire = true;
    x_run("control: no acquire", 1000);
    xc.drop_acquire = false;
    CHECK(xs.r == BCMJOB_OK && xs.rd == BCMJOB_OK && !xs.px_ok,
          "without acquire the rows must come out stale: %d %d %d", xs.r, xs.rd,
          xs.px_ok);

    /* 3. One core (cross false): no hand-off step runs, same answer */
    x_fresh(big, big_len, 0, 0x9004);
    xc.model = false;
    pub0 = xc.publishes;
    acq0 = xc.acquires;
    i = xc.aliases;
    x_run("one core", 1000);
    CHECK(xs.r == BCMJOB_OK && xs.rd == BCMJOB_OK && xs.px_ok &&
          xc.publishes == pub0 && xc.acquires == acq0 && xc.aliases == i,
          "one core: %d %d %d, hand-off calls %u %u %u", xs.r, xs.rd, xs.px_ok,
          xc.publishes - pub0, xc.acquires - acq0, xc.aliases - i);
    xc.model = true;

    /* 4. The races, randomised: give-ups (time jumps) against the take,
       the pieces, the rows and the answer; some runs with the owner
       absent at first (withdrawn before the take) */
    served0 = 0;
    for (i = 0; i < 240; i++)
    {
        bool big_file = i & 1;

        x_fresh(big_file ? big : jpg, big_file ? big_len : jpg_len,
                (i % 3) ? 60 : 0, 0x9100 + i);
        xc.rnd = 0x9E3779B9u * (i + 1);
        xc.one_in = 2 + i % 4;
        xc.jump_in = 20 + (i % 7) * 15;
        xc.jump_us = (i % 5 == 0) ? 600000 : 3000000;
        xc.owner_off = i % 6 == 0 ? 2000 : 0;
        xc_rd_us = 20;          /* 48 x 40 rows: several passes */
        x_run("race", 1000);
        xc_rd_us = 0;
        if (xs.r == BCMJOB_OK)
        {
            n_ok++;
            /* given up on, and the answer came first: it stands */
            n_raced += xs.dec_quit + (xs.rd == BCMJOB_OK && xs.rd_quit);
            CHECK(xs.res.out[0].slot < JREF_SLOTS && xs.res.epoch == bcmjob_epoch(),
                  "race %u: an OK answer whole", i);
            if (xs.rd == BCMJOB_OK)
                CHECK(xs.px_ok && x_neighbours_ok(), "race %u: rows %d neighbours %d",
                      i, xs.px_ok, x_neighbours_ok());
            else if (xs.rd == BCMJOB_TIMEOUT || xs.rd == BCMJOB_OFF)
                n_read_left++;
        }
        else if (xs.r == BCMJOB_TIMEOUT)
            n_left++;
        else if (xs.r == BCMJOB_OFF)
            n_off++;
        CHECK(bufs_out == 0 && !bjc_leaks(), "race %u: buffers out %d, leaks %u", i,
              bufs_out, bjc_leaks());
        served0 += xs.ts.fetches;
    }
    printf("info: cross races: %u ok (%u with a give-up), %u left, %u withdrawn, "
           "%u reads left, %u give-ups waited on a pass, %u pieces\n",
           n_ok, n_raced, n_left, n_off, n_read_left, xc.ack_waits, served0);
    CHECK(n_ok && n_left && n_off && n_read_left && xc.ack_waits && n_raced,
          "every race reached: ok %u left %u withdrawn %u read given up %u "
          "ack-waits %u answer-won %u", n_ok, n_left, n_off, n_read_left,
          xc.ack_waits, n_raced);

    /* 5. The owner frozen inside a pass: no acknowledgement within
       BJC_ACK_MS.  The caller returns TIMEOUT, traced loud, the piece
       buffer kept pinned, and its next call is BUSY while the owner stays
       frozen; once the owner acknowledges, the next call gives the buffer
       back and decodes again */
    x_fresh(big, big_len, 50, 0x9300);
    xc.one_in = 1000000;
    xc.jump_in = 0;
    xc.stall_arm = true;
    nlines = 0;
    x_run("leak", 1000);
    CHECK(xs.r == BCMJOB_TIMEOUT && xs.busy2 == BCMJOB_BUSY && bjc_leaks() == 1 &&
          bufs_out == 1 && logged("bcmjob withdraw unacked kind=decode"),
          "leak: %d, then %d, leaks %u, buffers out %d", xs.r, xs.busy2, bjc_leaks(),
          bufs_out);
    xs.p = jpg;
    xs.len = jpg_len;
    xs.o = dout(48, 40, 0, 0, 48, 40, 0x9301);
    xs.ms = 0;
    x_run("after the leak", 1000);
    CHECK(xs.r == BCMJOB_OK && xs.rd == BCMJOB_OK && xs.px_ok && bufs_out == 0 &&
          logged("bcmjob withdraw acked late"),
          "after the leak: %d %d %d, buffers out %d", xs.r, xs.rd, xs.px_ok, bufs_out);
    xc.model = false;
    bjc_set_owner(NULL);
}

/* ---- DECODE as the merged image's overlay (album-render.md 5.3) ------------
 * The merged image offers OVERLAY at its own region (DEC_BASE) and no
 * DECODE; dec-ovl.bin adds it, and J_CAP_DEC_LENT: each decode's work
 * area is a J_CLS_DRIFT slot keyed J_DEC_WORK_KEY the driver ALLOCs behind
 * the outputs and FREEs after, the file's length in J_DEC_LEN and the
 * record's len clipped to the ring.  The visualizer's record (another
 * image's region) is never due here. */
static int logged_has(const char *prefix, const char *part);
static uint32_t ovl_file[2048];         /* (the overlay's helpers: below) */
static uint32_t fnv1a(const void *p, uint32_t n);
static void ovl_make(uint32_t abi);
static int ovl_load_k(int kind, const struct bjc_ovl_rec *rec, uint32_t fnv_xor);

static bool work_slot_live(void)
{
    uint32_t k;

    for (k = 0; k < JREF_SLOTS; k++)
        if (chip.slot[k].epoch == chip.epoch && chip.slot[k].cls == J_CLS_DRIFT &&
            chip.slot[k].key_lo == J_DEC_WORK_KEY && chip.slot[k].state != J_ST_EMPTY)
            return true;
    return false;
}

static void t_dec_lent(void)
{
    struct bcmjob_dec_res res;
    struct bcmjob_dec_out o[1];
    struct bcmjob_slot cov, resd;
    struct bjc_ovl_rec rec, vrec;
    struct tsrc ts;
    uint32_t p[1], ring_bytes = BCMJOB_DEC_RING_W * BCMJOB_DEC_RING_W * 2;
    int r;

    ovl_offer = ovl_dec = true;
    ovl_base = DEC_BASE;
    stub_abi = image_abi = OVL_ABI;
    dec_offer = dec_hang = false;
    dec_row_us = 0;
    ring_allocs = 0;
    fresh();
    ovl_make(OVL_ABI);
    rec.bytes = sizeof(ovl_file);
    rec.fnv = fnv1a(ovl_file, sizeof(ovl_file));
    rec.abi = OVL_ABI;
    rec.abi_at = OVL_ABI_AT;
    rec.base = DEC_BASE;
    vrec = rec;
    vrec.base = OVL_BASE;               /* the display-only image's region */
    bjc_ovl_set(BJC_OVL_VIZ, &vrec);
    bjc_ovl_set(BJC_OVL_DEC, &rec);
    CHECK(!bcmjob_have(J_OP_DECODE) && !bjc_dec_lends(), "merged: no DECODE before the load");
    CHECK(bjc_ovl_due(BJC_OVL_DEC) && !bjc_ovl_due(BJC_OVL_VIZ),
          "the merged image's region: DECODE's overlay due, the visualizer's never");
    o[0] = dout(142, 142, 0, 0, 142, 142, 0x2001);
    r = decode1(jpg, jpg_len, o, 1, 0, 0, false, &res, &ts);
    CHECK(r != BCMJOB_OK, "no DECODE before the load: %d", r);

    /* the load: DECODE offered, the work slot lent from now on */
    nlines = 0;
    CHECK(ovl_load_k(BJC_OVL_DEC, &rec, 0) == BCMJOB_OK && ovl_loaded, "the load");
    CHECK(bcmjob_have(J_OP_DECODE) && bjc_dec_lends() && !bjc_ovl_due(BJC_OVL_DEC) &&
          logged("bcmjob overlay load k=1 bytes=8192") &&
          logged_has("bcmjob overlay ok k=1", "lent=1"), "DECODE offered, lent");
    CHECK(!memcmp(mem + DEC_BASE, ovl_file, sizeof(ovl_file)), "the region holds the file");

    /* the pool full (the drift's cover and the resident): the work slot's
       ALLOC fails, the call answers FAILED (the CPU decodes), no slot kept */
    CHECK(bcmjob_slot(J_CLS_DRIFT, 300, 300, 0xC0, 0, &cov) == BCMJOB_OK &&
          bcmjob_slot(J_CLS_DRIFT, 300, 300, 0xC1, 0, &resd) == BCMJOB_OK, "the pool full");
    nlines = 0;
    r = decode1(jpg, jpg_len, o, 1, 0, 0, false, &res, &ts);
    settle();
    CHECK(r == BCMJOB_FAILED && res.err == J_E_NOSLOT && logged("bcmjob decode why=nowork") &&
          !work_slot_live() && !logged("bcmjob submit op=0x30"),
          "no room to lend: %d err %u", r, res.err);
    CHECK(slot_state(res.out[0].slot) == J_ST_EMPTY, "the output freed after the failure");
    /* the drift's cover parked (the shell's dj_cover_park()) */
    p[0] = cov.slot;
    CHECK(bcmjob_submit(J_OP_FREE, 0, p, 1) == BCMJOB_OK, "the cover parked");
    settle();
    nlines = 0;
    lent_len_seen = lent_rec_len = 0;
    r = decode1(jpg, jpg_len, o, 1, 0, 0, false, &res, &ts);
    CHECK(r == BCMJOB_OK && same_as_twin(&res, o, 1, jpg, jpg_len),
          "the NP box on the lent slot: %d err %u", r, res.err);
    CHECK(lent_len_seen == jpg_len && lent_rec_len == jpg_len,
          "J_DEC_LEN %u, the record's len %u (file %u)", lent_len_seen, lent_rec_len, jpg_len);
    settle();
    CHECK(!work_slot_live(), "the work slot freed behind the DECODE");
    drop(&res, 1);
    /* a file longer than the ring: the record's len clipped, J_DEC_LEN
       the file's, the bytes streamed through the ring all the same */
    o[0] = dout(142, 80, 0, 0, 142, 80, 0x2002);
    lent_len_seen = lent_rec_len = 0;
    r = decode1(big, big_len, o, 1, 0, 0, false, &res, &ts);
    CHECK(r == BCMJOB_OK && same_as_twin(&res, o, 1, big, big_len) &&
          lent_len_seen == big_len && lent_rec_len == ring_bytes,
          "a 101 KB file: %d err %u, J_DEC_LEN %u, len %u", r, res.err, lent_len_seen,
          lent_rec_len);
    settle();
    CHECK(!work_slot_live(), "the work slot freed again");
    drop(&res, 1);
    /* an output in the same pool (the panel's class) beside the work
       slot, once the resident left */
    p[0] = resd.slot;
    CHECK(bcmjob_submit(J_OP_FREE, 0, p, 1) == BCMJOB_OK, "the resident dropped");
    settle();
    o[0] = dout(142, 142, 0, 0, 142, 142, 0x2003);
    o[0].cls = J_CLS_DRIFT;
    r = decode1(jpg, jpg_len, o, 1, 0, 0, false, &res, &ts);
    CHECK(r == BCMJOB_OK && same_as_twin(&res, o, 1, jpg, jpg_len) &&
          slot_state(res.out[0].slot) == J_ST_READY,
          "a J_CLS_DRIFT output beside the lent slot: %d err %u", r, res.err);
    settle();
    CHECK(!work_slot_live(), "freed; the output kept");
    drop(&res, 1);

    /* a RESET drops DECODE and the lending; the load is due again */
    CHECK(bcmjob_submit(J_OP_RESET, 0, NULL, 0) == BCMJOB_OK && !bcmjob_have(J_OP_DECODE) &&
          !bjc_dec_lends() && bjc_ovl_due(BJC_OVL_DEC), "a RESET: gone, due");
    settle();
    CHECK(ovl_load_k(BJC_OVL_DEC, &rec, 0) == BCMJOB_OK && bcmjob_have(J_OP_DECODE) &&
          bjc_dec_lends(), "reloaded");
    /* the BCM lost it unseen (a DECODE answers HP_E_UNKNOWN): gone, not
       a failure, due again, the CPU decodes meanwhile */
    dec_ovl_drop();
    o[0] = dout(142, 142, 0, 0, 142, 142, 0x2004);
    nlines = 0;
    r = decode1(jpg, jpg_len, o, 1, 0, 0, false, &res, &ts);
    settle();
    CHECK(r == BCMJOB_FAILED && logged("bcmjob overlay gone op=0x30") &&
          !bcmjob_have(J_OP_DECODE) && !bjc_dec_lends() && bjc_ovl_due(BJC_OVL_DEC) &&
          !work_slot_live(), "gone: %d", r);
    CHECK(ovl_load_k(BJC_OVL_DEC, &rec, 0) == BCMJOB_OK, "loaded again");
    /* a restart (a new epoch): due again, and the visualizer's never */
    p_restart();
    CHECK(!bcmjob_have(J_OP_DECODE) && bjc_ovl_due(BJC_OVL_DEC) &&
          !bjc_ovl_due(BJC_OVL_VIZ), "after a restart: gone, due");
    CHECK(lock_depth == 0 && bufs_out == 0, "lock %d, buffers %d", lock_depth, bufs_out);
    ovl_offer = ovl_dec = false;
    ovl_base = OVL_BASE;
    dec_lent = false;
}

/* The merged image's one region, two overlays (visualizer.md 8.5): the
 * visualizer's and DECODE's both due; a load of either evicts the other
 * ("bcmjob overlay evict"), whose load is due again at once, swap after
 * swap (no try spent by an eviction) */
static void t_ovl_swap(void)
{
    struct bjc_ovl_rec rec, vrec;
    int k;

    ovl_offer = ovl_dec = true;
    ovl_base = DEC_BASE;
    stub_abi = image_abi = OVL_ABI;
    dec_offer = false;
    fresh();
    ovl_make(OVL_ABI);
    rec.bytes = sizeof(ovl_file);
    rec.fnv = fnv1a(ovl_file, sizeof(ovl_file));
    rec.abi = OVL_ABI;
    rec.abi_at = OVL_ABI_AT;
    rec.base = DEC_BASE;
    vrec = rec;                         /* viz-ovl-m.bin: the same region */
    bjc_ovl_set(BJC_OVL_VIZ, &vrec);
    bjc_ovl_set(BJC_OVL_DEC, &rec);
    CHECK(bjc_ovl_region() == DEC_BASE, "the region: %lx", (unsigned long)bjc_ovl_region());
    CHECK(bjc_ovl_due(BJC_OVL_DEC) && bjc_ovl_due(BJC_OVL_VIZ), "both due in the region");
    ovl_as_viz = true;
    CHECK(ovl_load_k(BJC_OVL_VIZ, &vrec, 0) == BCMJOB_OK && bcmjob_have(J_OP_SPECTRUM) &&
          bcmjob_have(J_OP_BARS) && !bcmjob_have(J_OP_DECODE) && !bjc_ovl_due(BJC_OVL_VIZ) &&
          bjc_ovl_due(BJC_OVL_DEC), "the visualizer's loaded, DECODE's still due");
    for (k = 0; k < 3; k++)
    {
        ovl_as_viz = false;
        nlines = 0;
        CHECK(ovl_load_k(BJC_OVL_DEC, &rec, 0) == BCMJOB_OK && bcmjob_have(J_OP_DECODE) &&
              bjc_dec_lends() && !bcmjob_have(J_OP_SPECTRUM) && !bcmjob_have(J_OP_BARS) &&
              bjc_ovl_due(BJC_OVL_VIZ) && !bjc_ovl_due(BJC_OVL_DEC) &&
              logged("bcmjob overlay evict k=0 by=1"), "swap %d: DECODE in, the visualizer out "
              "and due", k);
        ovl_as_viz = true;
        nlines = 0;
        CHECK(ovl_load_k(BJC_OVL_VIZ, &vrec, 0) == BCMJOB_OK && bcmjob_have(J_OP_SPECTRUM) &&
              !bcmjob_have(J_OP_DECODE) && !bjc_dec_lends() && bjc_ovl_due(BJC_OVL_DEC) &&
              !bjc_ovl_due(BJC_OVL_VIZ) && logged("bcmjob overlay evict k=1 by=0"),
              "swap %d: the visualizer in, DECODE out and due", k);
    }
    CHECK(lock_depth == 0 && bufs_out == 0, "lock %d, buffers %d", lock_depth, bufs_out);
    ovl_offer = ovl_dec = ovl_as_viz = ovl_viz_in = false;
    ovl_base = OVL_BASE;
    dec_lent = false;
}

static void t_decode(void)
{
    struct bcmjob_dec_res res;
    struct bcmjob_dec_out o[3];
    struct tsrc ts;
    uint32_t k, e, d, t0, ops, sof, slot;
    int r;

    jpg = load("tests/ui/fixtures/sig-cover.jpg", &jpg_len);
    CHECK(jpg_len == 1130, "the fixture: %u bytes", jpg_len);
    /* a 101 KB file: two 50 KB APP15 segments after SOI, so the probe and
       the decode each stream it through the 32 KB ring (a pass each, the
       ring wrapping) */
    big = malloc(200000);
    k = 0;
    big[k++] = 0xFF;
    big[k++] = 0xD8;
    for (d = 0; d < 2; d++)
    {
        big[k++] = 0xFF;
        big[k++] = 0xEF;
        big[k++] = 50000 >> 8;
        big[k++] = 50000 & 0xFF;
        for (e = 0; e < 49998; e++)
            big[k++] = (uint8_t)(e * 131 + d);
    }
    memcpy(big + k, jpg + 2, jpg_len - 2);
    big_len = k + jpg_len - 2;
    /* the source's verdicts: arithmetic coding (UNSUPPORTED), a
       progressive marker on baseline data (CORRUPT), the end cut
       (TRUNCATED) */
    sof = find_sof(jpg, jpg_len);
    for (k = 0; k < 3; k++)
    {
        bad[k] = malloc(jpg_len);
        memcpy(bad[k], jpg, jpg_len);
    }
    bad[0][sof] = 0xC9;
    bad[1][sof] = 0xC2;
    dwork = malloc(J_DEC_WORK_MIN);
    twork = malloc(J_DEC_WORK_MIN);

    /* the NP box from a file the ring holds: one pass, the twin's pixels */
    dec_fresh();
    CHECK(bcmjob_have(J_OP_DECODE), "the image offers DECODE");
    o[0] = dout(142, 142, 0, 0, 142, 142, 0x1001);
    e2_any = ring_unlocked = bad_pieces = big_pieces = 0;
    r = decode1(jpg, jpg_len, o, 1, 0, 0, false, &res, &ts);
    CHECK(r == BCMJOB_OK && res.sw == 120 && res.sh == 120 && res.passes == 1 &&
          res.bytes == jpg_len && !res.out[0].hit && res.epoch == bcmjob_epoch() &&
          slot_state(res.out[0].slot) == J_ST_READY && ts.starts == 1 && ring_allocs == 1,
          "the NP box: %d err %u, %ux%u, passes %u, bytes %u, starts %u", r, res.err,
          res.sw, res.sh, res.passes, res.bytes, ts.starts);
    CHECK(same_as_twin(&res, o, 1, jpg, jpg_len), "the NP box differs from jdec_run's");
    CHECK(bcmjob_dec_verdict(r, &res) == JDEC_OK, "verdict");
    CHECK(logged("bcmjob decode len=1130 n=1") && logged("bcmjob submit op=0x30") &&
          logged("bcmjob decode done st=0 err=0 passes=1 bytes=1130"), "decode traces");
    CHECK(lock_depth == 0 && bufs_out == 0, "lock %d, buffers %d", lock_depth, bufs_out);
    /* the same album again this epoch: a hit, nothing streamed */
    nlines = 0;
    r = decode1(jpg, jpg_len, o, 1, 0, 0, false, &res, &ts);
    CHECK(r == BCMJOB_OK && res.out[0].hit && res.passes == 0 && ts.starts == 0 &&
          !res.sw && !logged("bcmjob submit op=0x30") && same_as_twin(&res, o, 1, jpg, jpg_len),
          "a second render of the same album: %d hit %d passes %u", r, res.out[0].hit,
          res.passes);
    drop(&res, 1);

    /* a file longer than the ring: a pass each for the probe and the
       decode, the bytes again in order; the 16:9 box; engine 2 */
    o[0] = dout(142, 80, 0, 0, 142, 80, 0x1002);
    r = decode1(big, big_len, o, 1, 0, 0, false, &res, &ts);
    CHECK(r == BCMJOB_OK && res.passes == 2 && res.bytes == 2 * big_len && ts.starts == 2 &&
          ring_allocs == 1 && same_as_twin(&res, o, 1, big, big_len),
          "a 101 KB file: %d err %u passes %u bytes %u starts %u", r, res.err, res.passes,
          res.bytes, ts.starts);
    CHECK(e2_any > 0 && !ring_unlocked && !bad_pieces && !big_pieces,
          "ring pieces: engine 2 %u B, unlocked %u, bad %u, big %u", e2_any, ring_unlocked,
          bad_pieces, big_pieces);
    drop(&res, 1);

    /* a DECODE that starts 5 ms after its record (the BCM on earlier
       work): the pass the word shows meanwhile is the last job's, never
       served */
    o[0] = dout(142, 142, 0, 0, 142, 142, 0x1006);
    dec_delay_us = 5000;
    r = decode1(big, big_len, o, 1, 0, 0, false, &res, &ts);
    dec_delay_us = 0;
    CHECK(r == BCMJOB_OK && res.passes == 2 && ts.starts == 2 && res.bytes == 2 * big_len &&
          same_as_twin(&res, o, 1, big, big_len),
          "a late start: %d passes %u starts %u", r, res.passes, ts.starts);
    drop(&res, 1);

    /* the thumbnail: a window of the fill */
    o[1] = dout(83, 55, 14, 0, 55, 55, 0x1003);
    r = decode1(jpg, jpg_len, o + 1, 1, 0, 0, false, &res, &ts);
    CHECK(r == BCMJOB_OK && same_as_twin(&res, o + 1, 1, jpg, jpg_len),
          "the thumbnail's window: %d", r);
    drop(&res, 1);
    /* two outputs in one job: equal to jdec_run() given the same set */
    o[0] = dout(55, 55, 0, 0, 55, 55, 0x1004);
    o[1] = dout(48, 40, 4, 0, 40, 40, 0x1004);
    o[1].key_hi ^= 0x01000000;
    r = decode1(big, big_len, o, 2, 0, 0, false, &res, &ts);
    CHECK(r == BCMJOB_OK && same_as_twin(&res, o, 2, big, big_len),
          "two outputs: %d err %u", r, res.err);
    drop(&res, 2);

    /* n > 1 with hits (bcmjob.h): the DECODE gets the misses only, each
       equal to jdec_run() given that reduced set; a hit is never written
       or freed */
    {
        static uint16_t pat[142 * 142];
        struct bcmjob_dec_out set[J_DEC_MAX], four[4];
        struct bcmjob_slot sl;
        uint32_t ha, hb, a2, b2, i, ms;
        int rnd;

        for (i = 0; i < 142 * 142; i++)
            pat[i] = (uint16_t)(i * 2654435761u >> 13);
        /* all three miss: the twin given the full set */
        o[0] = dout(142, 142, 0, 0, 142, 142, 0x1007);
        o[1] = dout(48, 40, 4, 0, 40, 40, 0x1007);
        o[2] = dout(30, 30, 0, 0, 30, 30, 0x1007);
        o[1].key_hi ^= 0x01000000;
        o[2].key_hi ^= 0x02000000;
        nlines = 0;
        hit_frees = 0;
        r = decode1(jpg, jpg_len, o, 3, 0, 0, false, &res, &ts);
        CHECK(r == BCMJOB_OK && res.miss_n == 3 && res.miss[0] == 0 && res.miss[1] == 1 &&
              res.miss[2] == 2 && sent_n(3) && same_as_twin(&res, o, 3, jpg, jpg_len),
              "three misses: %d err %u, misses %u", r, res.err, res.miss_n);
        /* the same three again: every one a hit, nothing sent */
        nlines = 0;
        r = decode1(jpg, jpg_len, o, 3, 0, 0, false, &res, &ts);
        CHECK(r == BCMJOB_OK && res.out[0].hit && res.out[1].hit && res.out[2].hit &&
              !res.miss_n && !res.passes && !ts.starts && !logged("bcmjob submit op=0x30") &&
              same_as_twin(&res, o, 3, jpg, jpg_len),
              "three hits: %d, misses %u, passes %u", r, res.miss_n, res.passes);
        drop(&res, 3);

        /* one hit of three, the largest (the primary), holding its own
           pixels (an upload): the DECODE gets two outputs, each the
           twin's given those two (the 48 x 40 the primary now: half
           scale), and the hit's pixels and sums are as before */
        o[0].key_lo = o[1].key_lo = o[2].key_lo = 0x1008;
        CHECK(bcmjob_slot(J_CLS_THUMB, 142, 142, 0x1008, o[0].key_hi, &sl) == BCMJOB_OK &&
              !sl.hit && bcmjob_upload(&sl, pat) == BCMJOB_OK &&
              read_sum(sl.slot, 142, 142, &ha, &hb), "the held box");
        nlines = 0;
        r = decode1(jpg, jpg_len, o, 3, 0, 0, false, &res, &ts);
        CHECK(r == BCMJOB_OK && res.out[0].hit && res.out[0].slot == sl.slot &&
              !res.out[1].hit && !res.out[2].hit && res.miss_n == 2 && res.miss[0] == 1 &&
              res.miss[1] == 2 && sent_n(2) && !sent_n(3) &&
              bcmjob_dec_set(o, &res, set) == 2 && set[0].dw == 48,
              "one hit of three: %d err %u, misses %u", r, res.err, res.miss_n);
        {
            struct bcmjob_dec_res r2 = res;

            r2.out[0] = res.out[1];     /* the misses' slots, in set order */
            r2.out[1] = res.out[2];
            CHECK(same_as_twin(&r2, set, 2, jpg, jpg_len),
                  "the misses differ from the twin given the reduced set");
        }
        CHECK(read_sum(sl.slot, 142, 142, &a2, &b2) && a2 == ha && b2 == hb &&
              res.out[0].a == ha && res.out[0].b == hb && !memcmp(RB, pat, sizeof(pat)) &&
              slot_state(sl.slot) == J_ST_READY && !hit_frees,
              "the hit's pixels changed: %08x %08x, was %08x %08x", a2, b2, ha, hb);
        /* the full set's 48 x 40 differs from the reduced set's: a driver
           that sent the hit too would fail above */
        CHECK(twin(jpg, jpg_len, o, 3) == JDEC_OK && memcpy(RB, TW[1], 2 * 40 * 40) &&
              twin(jpg, jpg_len, set, 2) == JDEC_OK && memcmp(RB, TW[0], 2 * 40 * 40),
              "the reduced set's 48 x 40 is the full set's");
        drop(&res, 3);

        /* a failed and a cancelled decode with a hit: the hit READY with
           its pixels and sums, never freed; the miss freed */
        o[0].key_lo = o[1].key_lo = 0x1009;
        CHECK(bcmjob_slot(J_CLS_THUMB, 142, 142, 0x1009, o[0].key_hi, &sl) == BCMJOB_OK &&
              bcmjob_upload(&sl, pat) == BCMJOB_OK && read_sum(sl.slot, 142, 142, &ha, &hb),
              "the held box");
        busy_inside = 0;
        for (rnd = 0; rnd < 2; rnd++)
        {
            cancel_after = rnd ? 3 : 0;
            r = rnd ? decode1(big, big_len, o, 2, 0, 0, true, &res, &ts) :
                      decode1(bad[1], jpg_len, o, 2, 0, 0, false, &res, &ts);
            cancel_after = 0;
            ms = res.out[1].slot;
            settle();
            CHECK(r == (rnd ? BCMJOB_ABORTED : BCMJOB_FAILED) && res.out[0].hit &&
                  res.out[0].slot == sl.slot && res.miss_n == 1 && res.miss[0] == 1 &&
                  sent_n(1) && slot_state(sl.slot) == J_ST_READY &&
                  slot_state(ms) == J_ST_EMPTY && !hit_frees &&
                  read_sum(sl.slot, 142, 142, &a2, &b2) && a2 == ha && b2 == hb,
                  "a %s decode with a hit: %d, states %d %d, sums %08x/%08x, hit frees %u",
                  rnd ? "cancelled" : "failed", r, slot_state(sl.slot), slot_state(ms), a2,
                  ha, hit_frees);
        }
        busy_inside = 1;
        res.out[0].slot = sl.slot;
        drop(&res, 1);

        /* the work check is on the reduced set: the 16:9 art set's sizes
           (184,428 B, the minimum; small windows, the test's pool) and a
           fourth output over it.  The fourth a hit: the rest decoded.  All
           four misses: NOMEM once the ALLOCs answer, no DECODE */
        four[0] = dout(142, 80, 0, 0, 142, 80, 0x100A);
        four[1] = dout(98, 55, 21, 0, 55, 55, 0x100A);
        four[2] = dout(534, 300, 117, 0, 40, 40, 0x100A);
        four[3] = dout(300, 300, 0, 0, 40, 40, 0x100A);
        for (i = 1; i < 4; i++)
            four[i].key_hi ^= i << 24;
        CHECK(bcmjob_dec_work(four, 3) == J_DEC_WORK_MIN &&
              bcmjob_dec_work(four, 4) > J_DEC_WORK_MIN, "the fourth's work: %u %u",
              bcmjob_dec_work(four, 3), bcmjob_dec_work(four, 4));
        nlines = 0;
        e = bcmjob_errors();
        r = decode1(jpg, jpg_len, four, 4, 0, 0, false, &res, &ts);
        settle();
        CHECK(r == BCMJOB_FAILED && res.err == HP_E_NOMEM && res.miss_n == 4 &&
              !logged("bcmjob submit op=0x30") && ts.starts == 0 &&
              slot_state(res.out[0].slot) == J_ST_EMPTY &&
              slot_state(res.out[3].slot) == J_ST_EMPTY && bcmjob_errors() == e &&
              bcmjob_have(J_OP_DECODE), "four misses over the work area: %d err %u", r,
              res.err);
        r = decode1(jpg, jpg_len, four + 3, 1, 0, 0, false, &res, &ts);
        ms = res.out[0].slot;
        CHECK(r == BCMJOB_OK, "the fourth alone: %d", r);
        nlines = 0;
        r = decode1(jpg, jpg_len, four, 4, 0, 0, false, &res, &ts);
        CHECK(r == BCMJOB_OK && res.out[3].hit && res.out[3].slot == ms && res.miss_n == 3 &&
              sent_n(3) && same_as_twin(&res, four, 3, jpg, jpg_len) && !hit_frees,
              "fits once the hit is dropped: %d err %u", r, res.err);
        drop(&res, 4);
    }

    /* past the work area alone (a 16:1 cover's fill, 4,800 x 300), or
       outside the picture: never sent */
    nlines = 0;
    ops = port_ops;
    o[0] = dout(142, 142, 0, 0, 142, 142, 0x1005);
    o[1] = dout(4800, 300, 0, 0, 55, 55, 0x1005);
    o[1].key_hi ^= 0x01000000;
    r = decode1(jpg, jpg_len, o + 1, 1, 0, 0, false, &res, &ts);
    CHECK(bcmjob_dec_work(o + 1, 1) > J_DEC_WORK_MIN && r == BCMJOB_FAILED &&
          res.err == HP_E_NOMEM && ts.starts == 0 &&
          bcmjob_dec_verdict(r, &res) == BCMJOB_DEC_TWIN, "a panorama's fill: %d err %u",
          r, res.err);
    o[0].cx = 1;
    r = decode1(jpg, jpg_len, o, 1, 0, 0, false, &res, &ts);
    CHECK(r == BCMJOB_FAILED && res.err == J_E_RANGE, "a window past dw: %d", r);
    o[0].cx = 0;
    r = decode1(jpg, jpg_len, o, 0, 0, 0, false, &res, &ts);
    CHECK(r == BCMJOB_FAILED && res.err == J_E_RANGE, "n = 0: %d", r);
    CHECK(port_ops == ops && !logged("bcmjob decode"), "refused before any traffic");
    /* the panorama with the box (which fits alone): the work is the set
       sent's, known once the ALLOCs answer; NOMEM then, no DECODE, the
       misses freed, nothing counted */
    e = bcmjob_errors();
    r = decode1(jpg, jpg_len, o, 2, 0, 0, false, &res, &ts);
    settle();
    CHECK(r == BCMJOB_FAILED && res.err == HP_E_NOMEM && res.miss_n == 2 && ts.starts == 0 &&
          !logged("bcmjob submit op=0x30") && slot_state(res.out[0].slot) == J_ST_EMPTY &&
          slot_state(res.out[1].slot) == J_ST_EMPTY && bcmjob_errors() == e &&
          bcmjob_have(J_OP_DECODE) && bcmjob_dec_verdict(r, &res) == BCMJOB_DEC_TWIN,
          "the box and the panorama: %d err %u", r, res.err);

    /* the source's verdicts, the twin's; never counted toward the failed
       mark; the outputs freed behind the job */
    {
        static const struct { uint32_t err; int verdict; } v[] =
        {
            { J_E_DEC_UNSUPPORTED, BCMJOB_DEC_TWIN },
            { J_E_DEC_CORRUPT,     JDEC_E_CORRUPT },
            { J_E_DEC_TRUNCATED,   JDEC_E_TRUNCATED },
        };
        uint32_t rnd, len;

        for (rnd = 0; rnd < 2; rnd++)
            for (k = 0; k < 3; k++)
            {
                len = k == 2 ? jpg_len - 200 : jpg_len;
                o[0] = dout(142, 142, 0, 0, 142, 142, 0x2000 + k);
                r = decode1(bad[k], len, o, 1, 0, 0, false, &res, &ts);
                slot = res.out[0].slot;
                settle();
                CHECK(r == BCMJOB_FAILED && res.err == v[k].err && !res.stalled &&
                      !res.read_err && bcmjob_dec_verdict(r, &res) == v[k].verdict &&
                      (v[k].verdict == BCMJOB_DEC_TWIN ||
                       twin(bad[k], len, o, 1) == v[k].verdict) &&
                      slot_state(slot) == J_ST_EMPTY,
                      "source %u: %d err %u, verdict %d, slot %d", k, r, res.err,
                      bcmjob_dec_verdict(r, &res), slot_state(slot));
            }
        CHECK(twin(bad[0], jpg_len, o, 1) == JDEC_E_UNSUPPORTED, "the twin: UNSUPPORTED");
        CHECK(bcmjob_have(J_OP_DECODE), "the source's errors marked DECODE failed");
    }

    /* the BCM's own failure (an image whose work area came out short):
       counted, marked on the second; the twin's to do meanwhile */
    dwork_bytes = 100000;
    o[0] = dout(142, 142, 0, 0, 142, 142, 0x2050);
    r = decode1(jpg, jpg_len, o, 1, 0, 0, false, &res, &ts);
    CHECK(r == BCMJOB_FAILED && res.err == HP_E_NOMEM && bcmjob_have(J_OP_DECODE) &&
          bcmjob_dec_verdict(r, &res) == BCMJOB_DEC_TWIN &&
          logged("bcmjob fault op=0x30 st=6 why=failed"), "a NOMEM from the BCM: %d err %u", r,
          res.err);
    r = decode1(jpg, jpg_len, o, 1, 0, 0, false, &res, &ts);
    CHECK(r == BCMJOB_FAILED && !bcmjob_have(J_OP_DECODE) &&
          decode1(jpg, jpg_len, o, 1, 0, 0, false, &res, &ts) == BCMJOB_NOJOB,
          "the second marks DECODE failed: %d", r);
    dwork_bytes = J_DEC_WORK_MIN;
    dec_fresh();

    /* the PP's read fails: J_DEC_ABORT, CORRUPT as the twin's read error */
    o[0] = dout(142, 142, 0, 0, 142, 142, 0x2100);
    {
        struct jdec_src src = ts_src(&ts, big, big_len);
        struct bcmjob_dec_req q;

        memset(&q, 0, sizeof(q));
        ts.fail_at = 50000;
        q.src = &src;
        q.out = o;
        q.n = 1;
        t0 = now_us_;
        r = bcmjob_decode(&q, &res);
        t0 = now_us_ - t0;
        slot = res.out[0].slot;
        settle();
        CHECK(r == BCMJOB_FAILED && res.err == J_E_DEC_CORRUPT && res.read_err &&
              !res.stalled && res.bytes < 50000 &&
              bcmjob_dec_verdict(r, &res) == JDEC_E_CORRUPT && slot_state(slot) == J_ST_EMPTY &&
              t0 < 100000, "a read error: %d err %u read_err %d stalled %d, %u us", r,
              res.err, res.read_err, res.stalled, t0);
        /* a read that takes 2 s: the BCM gives up; not the file's verdict */
        src = ts_src(&ts, big, big_len);
        ts.slow_at = 40000;
        q.ms = 20000;           /* the stall, not the deadline */
        r = bcmjob_decode(&q, &res);
        q.ms = 0;
        CHECK(r == BCMJOB_FAILED && res.err == J_E_DEC_CORRUPT && res.stalled &&
              !res.read_err && bcmjob_dec_verdict(r, &res) == BCMJOB_DEC_TWIN,
              "a stall: %d err %u stalled %d", r, res.err, res.stalled);
        settle();
    }

    /* called off while streaming: J_DEC_ABORT, the job ends on the BCM,
       ABORTED, the outputs go; a second caller meanwhile is BUSY */
    o[0] = dout(142, 142, 0, 0, 142, 142, 0x3000);
    nlines = 0;
    cancel_after = 3;
    busy_inside = 1;
    t0 = now_us_;
    r = decode1(big, big_len, o, 1, 0, 0, true, &res, &ts);
    t0 = now_us_ - t0;
    cancel_after = 0;
    slot = res.out[0].slot;
    settle();
    CHECK(r == BCMJOB_ABORTED && res.bytes < big_len && slot_state(slot) == J_ST_EMPTY &&
          bcmjob_dec_verdict(r, &res) == BCMJOB_DEC_DEFER &&
          logged("bcmjob fault op=0x30 why=aborted") && bcmjob_have(J_OP_DECODE) &&
          t0 < 100000, "a cancel mid-stream: %d, %u bytes, %u us (the BCM told at once)",
          r, res.bytes, t0);
    CHECK(busy_inside == BCMJOB_BUSY && logged("bcmjob fault op=0x30 why=busy"),
          "a second decode in flight: %d", busy_inside);
    /* called off once the whole file is in the ring: it runs to its end
       (each row 1 ms on the BCM), waited for, and is dropped */
    o[0] = dout(142, 142, 0, 0, 142, 142, 0x3001);
    dec_row_us = 1000;
    cancel_after = 1;
    d = chip.done;
    e = bcmjob_errors();
    r = decode1(jpg, jpg_len, o, 1, 0, 0, true, &res, &ts);
    cancel_after = 0;
    dec_row_us = 0;
    slot = res.out[0].slot;
    settle();
    CHECK(r == BCMJOB_ABORTED && res.bytes == jpg_len && chip.done == d + 4 &&
          bcmjob_errors() == e && slot_state(slot) == J_ST_EMPTY,
          "a cancel after the stream: %d, done +%u (ALLOC, DECODE, FREE, FENCE), errors +%u", r,
          chip.done - d, bcmjob_errors() - e);
    o[0] = dout(142, 142, 0, 0, 142, 142, 0x3002);
    r = decode1(big, big_len, o, 1, 0, 0, false, &res, &ts);
    CHECK(r == BCMJOB_OK && same_as_twin(&res, o, 1, big, big_len), "after a cancel: %d", r);
    drop(&res, 1);

    /* the epoch moves mid-decode (a cut, a restart, the BCM's event): OFF,
       the slots gone; the next decode allocates the ring again.  The
       driver never touches the event ring of a cut BCM, nor of a
       restarted one before its discovery */
    cut_evt = stale_evt = 0;
    {
        static void (*const hooks[])(struct tsrc *) = { hook_cut, hook_restart,
                                                        hook_epoch_event };
        struct jdec_src src;
        struct bcmjob_dec_req q;

        for (k = 0; k < 3; k++)
        {
            e = bcmjob_epoch();
            d = ring_allocs;
            o[0] = dout(142, 142, 0, 0, 142, 142, 0x4000 + k);
            src = ts_src(&ts, big, big_len);
            ts.hook = hooks[k];
            memset(&q, 0, sizeof(q));
            q.src = &src;
            q.out = o;
            q.n = 1;
            r = bcmjob_decode(&q, &res);
            CHECK(r == BCMJOB_OFF && bcmjob_epoch() != e && epoch_seen == bcmjob_epoch() &&
                  bcmjob_dec_verdict(r, &res) == BCMJOB_DEC_DEFER,
                  "epoch change %u mid-decode: %d", k, r);
            if (k == 0)
            {
                p_power(true);  /* lit again */
                CHECK(bcmjob_have(J_OP_DECODE), "up after the cut");
            }
            r = decode1(big, big_len, o, 1, 0, 0, false, &res, &ts);
            CHECK(r == BCMJOB_OK && !res.out[0].hit && ring_allocs == d + 1 &&
                  same_as_twin(&res, o, 1, big, big_len),
                  "a decode after epoch change %u: %d (ring allocs %u)", k, r,
                  ring_allocs - d);
            drop(&res, 1);
        }
        CHECK(!cut_evt && !stale_evt, "the event ring of a cut BCM %u, of a restarted one %u",
              cut_evt, stale_evt);
        /* a dead BCM mid-decode: past the deadline, restarted */
        restarts = 0;
        o[0] = dout(142, 142, 0, 0, 142, 142, 0x4100);
        src = ts_src(&ts, big, big_len);
        ts.hook = hook_dead;
        q.ms = 50;
        r = bcmjob_decode(&q, &res);
        CHECK(r == BCMJOB_TIMEOUT && restarts == 1 && logged("bcmjob fault op=0x00 why=dead"),
              "dead mid-decode: %d, restarts %u", r, restarts);
    }

    /* the deadline: a DECODE that never ends with the heartbeat live is
       slow, marked on its second miss; its FREEs run after it */
    dec_fresh();
    nlines = 0;
    dec_hang = true;
    o[0] = dout(142, 142, 0, 0, 142, 142, 0x6000);
    r = decode1(jpg, jpg_len, o, 1, 0, 30, false, &res, &ts);
    slot = res.out[0].slot;
    CHECK(r == BCMJOB_TIMEOUT && restarts == 0 && bcmjob_have(J_OP_DECODE) &&
          logged("bcmjob fault op=0x30 why=timeout") &&
          bcmjob_dec_verdict(r, &res) == BCMJOB_DEC_TWIN, "a slow decode: %d", r);
    CHECK(slot_state(slot) == J_ST_ALLOCATED, "freed while the job may write it");
    dec_hang = false;
    settle();
    CHECK(slot_state(slot) == J_ST_EMPTY, "the FREE behind the job: %d", slot_state(slot));
    dec_hang = true;
    r = decode1(jpg, jpg_len, o, 1, 0, 30, false, &res, &ts);
    dec_hang = false;
    CHECK(r == BCMJOB_TIMEOUT && !bcmjob_have(J_OP_DECODE) &&
          decode1(jpg, jpg_len, o, 1, 0, 0, false, &res, &ts) == BCMJOB_NOJOB,
          "a slow decode not marked on its second miss: %d", r);
    /* the default deadline: BCMJOB_T_DECODE at J_POWER's clock, capped at
       BCMJOB_DEC_CAP_MS (27 MHz: 11.1 s uncapped) */
    for (k = 0; k < 2; k++)
    {
        uint32_t want;

        chip_mhz = k ? 27 : 150;
        dec_fresh();
        dec_hang = true;
        t0 = now_us_;
        r = decode1(jpg, jpg_len, o, 1, 0, 0, false, &res, &ts);
        t0 = (now_us_ - t0) / 1000;
        want = BCMJOB_T_DECODE(jpg_len, chip_mhz);
        if (want > BCMJOB_DEC_CAP_MS)
            want = BCMJOB_DEC_CAP_MS;
        CHECK(r == BCMJOB_TIMEOUT && t0 >= want && t0 < want + 50 &&
              (k == 0 || want == BCMJOB_DEC_CAP_MS),
              "the deadline at %u MHz: %u ms", chip_mhz, t0);
        dec_hang = false;
    }
    chip_mhz = 150;

    /* a wait that runs out behind a DECODE still holding the BCM (past
       its own deadline and the busy-decode window) blames nobody: the
       DECODE is marked only by its own deadline */
    dec_fresh();
    dec_hang = true;
    o[0] = dout(142, 142, 0, 0, 142, 142, 0x6100);
    r = decode1(jpg, jpg_len, o, 1, 0, 30, false, &res, &ts);
    CHECK(r == BCMJOB_TIMEOUT && bcmjob_have(J_OP_DECODE), "a slow decode once: %d", r);
    now_us_ += 2000u * J_DEC_STALL_MS + 1000;   /* the window gone */
    {
        struct bcmjob_slot sl;

        nlines = 0;
        r = bcmjob_slot(J_CLS_THUMB, 8, 8, 0x6101, 0, &sl);
        CHECK(r == BCMJOB_TIMEOUT && bcmjob_have(J_OP_DECODE) && bcmjob_have(J_OP_ALLOC),
              "an ALLOC behind the DECODE: %d, DECODE %d ALLOC %d", r,
              bcmjob_have(J_OP_DECODE), bcmjob_have(J_OP_ALLOC));
        /* the resync waits for the jobs ahead of the held DECODE only:
           no wait for the DECODE itself, nothing forced */
        nlines = 0;
        t0 = now_us_;
        r = bcmjob_resync(0, 0, W, H);
        t0 = now_us_ - t0;
        CHECK(r == BCMJOB_OK && t0 < 10000 && !logged("bcmjob resync forced") &&
              !memcmp(mem + FRONT, ppfb, sizeof(ppfb)),
              "a resync behind a held DECODE: %d after %u us", r, t0);
    }
    dec_hang = false;
    settle();

    /* an ALLOC whose wait ran out: its late reply frees the slot */
    dec_fresh();
    {
        struct bcmjob_slot sl;
        uint32_t k2, left = 0;

        CHECK(bcmjob_have(J_OP_ALLOC), "up");
        nlines = 0;
        slow_until = now_us_ + 40000;
        r = bcmjob_slot(J_CLS_THUMB, 8, 8, 0x6200, 0, &sl);
        slow_until = 0;
        settle();
        for (k2 = 0; k2 < JREF_SLOTS; k2++)
            left += chip.slot[k2].key_lo == 0x6200 && chip.slot[k2].state != J_ST_EMPTY;
        CHECK(r == BCMJOB_TIMEOUT && !left && logged("bcmjob free late alloc"),
              "a late ALLOC: %d, %u left", r, left);
    }

    /* power, D-3: dark on battery REFUSED (nothing powered); dark on the
       charger lit-only (NO_WAKE) OFF; else it powers the BCM for the job,
       holds it for the read-back, and lets it go */
    dec_fresh();
    lit_ = false;
    p_power(false);
    bjc_dark(true);
    d = power_calls;
    nlines = 0;
    o[0] = dout(142, 142, 0, 0, 142, 142, 0x5000);
    r = decode1(jpg, jpg_len, o, 1, 0, 0, false, &res, &ts);
    CHECK(r == BCMJOB_REFUSED && power_calls == d && !powered && ts.starts == 0 &&
          logged("bcmjob fault op=0x30 why=refused") &&
          bcmjob_dec_verdict(r, &res) == BCMJOB_DEC_TWIN, "dark on battery: %d", r);
    charging_ = true;
    r = decode1(jpg, jpg_len, o, 1, BCMJOB_DEC_NO_WAKE, 0, false, &res, &ts);
    CHECK(r == BCMJOB_OFF && power_calls == d && !powered && bcmjob_dark_ups(0) == 0,
          "dark, the charger, lit only: %d", r);
    /* a decode never powers the BCM (owner, 2026-10-24): OFF, which the
       verdict maps to DEFER (retry when lit) */
    r = decode1(jpg, jpg_len, o, 1, 0, 0, false, &res, &ts);
    CHECK(r == BCMJOB_OFF && !powered && power_calls == d && bcmjob_dark_ups(0) == 0 &&
          bcmjob_dec_verdict(r, &res) == BCMJOB_DEC_DEFER,
          "dark on the charger, not up: %d", r);
    /* up already (another reason's hold): it runs, and powers nothing */
    CHECK(bcm_power_want(BCM_WANT_GAME, 60000) == BCMJOB_OK && powered, "a hold");
    d = bcmjob_dark_ups(0);
    r = decode1(jpg, jpg_len, o, 1, 0, 0, false, &res, &ts);
    CHECK(r == BCMJOB_OK && bcmjob_dark_ups(0) == d && bcmjob_dark_ups(BCM_WANT_DECODE) == 0 &&
          same_as_twin(&res, o, 1, jpg, jpg_len), "dark, up for a hold: %d", r);
    bcm_power_release(BCM_WANT_GAME);
    CHECK(!powered, "the hold's release cuts it");
    charging_ = false;
    lit_ = true;
    bjc_dark(false);

    /* the image lacks DECODE (the merged image) */
    dec_offer = false;
    fresh();
    o[0] = dout(142, 142, 0, 0, 142, 142, 0x5100);
    r = decode1(jpg, jpg_len, o, 1, 0, 0, false, &res, &ts);
    CHECK(r == BCMJOB_NOJOB && !bcmjob_have(J_OP_DECODE) &&
          bcmjob_dec_verdict(r, &res) == BCMJOB_DEC_TWIN, "no DECODE in the image: %d", r);

    /* a command ring too small for the record: BUSY, the output freed */
    cmd_words = 8;
    dec_fresh();
    r = decode1(jpg, jpg_len, o, 1, 0, 0, false, &res, &ts);
    slot = res.out[0].slot;
    settle();
    CHECK(r == BCMJOB_BUSY && slot_state(slot) == J_ST_EMPTY && bufs_out == 0,
          "ring full: %d, slot %d", r, slot_state(slot));
    cmd_words = HP_CMD_WORDS;

    /* the output's pool full: FAILED, J_E_NOSLOT, the twin's to do */
    dec_fresh();
    {
        struct bcmjob_slot sl;

        for (k = 0; k < 8 && bcmjob_slot(J_CLS_THUMB, 100, 100, 0x77 + k, 0, &sl) == BCMJOB_OK; k++)
            ;
    }
    r = decode1(jpg, jpg_len, o, 1, 0, 0, false, &res, &ts);
    CHECK(r == BCMJOB_FAILED && res.err == J_E_NOSLOT && bcmjob_have(J_OP_DECODE) &&
          bcmjob_dec_verdict(r, &res) == BCMJOB_DEC_TWIN, "no slot: %d err %u", r, res.err);
    /* no piece buffer on the PP: NOMEM before any traffic */
    dec_fresh();
    no_buf = true;
    ops = port_ops;
    r = decode1(jpg, jpg_len, o, 1, 0, 0, false, &res, &ts);
    no_buf = false;
    CHECK(r == BCMJOB_FAILED && res.err == HP_E_NOMEM && port_ops == ops && bufs_out == 0,
          "no piece buffer: %d err %u", r, res.err);

    /* bcmjob_read: any rect, any band; FAILED outside; OFF in another
       epoch */
    o[0] = dout(142, 142, 0, 0, 142, 142, 0x7000);
    r = decode1(jpg, jpg_len, o, 1, 0, 0, false, &res, &ts);
    CHECK(r == BCMJOB_OK && twin(jpg, jpg_len, o, 1) == JDEC_OK, "a box to read: %d", r);
    {
        static uint16_t px[60 * 20];
        uint32_t y, ok = 1;

        for (k = 0; k < sizeof(px) / 2; k++)
            px[k] = 0xDEAD;
        CHECK(bcmjob_read(res.out[0].slot, 3, 5, 51, 20, px, 60) == BCMJOB_OK, "a rect");
        for (y = 0; y < 20; y++)
            ok &= !memcmp(px + y * 60, TW[0] + (5 + y) * 142 + 3, 2 * 51) &&
                  px[y * 60 + 51] == 0xDEAD;
        CHECK(ok, "an odd rect equals the twin's, the pitch kept");
        CHECK(bcmjob_read(res.out[0].slot, 100, 0, 43, 1, px, 60) == BCMJOB_FAILED &&
              bcmjob_read(res.out[0].slot, 0, 140, 10, 3, px, 60) == BCMJOB_FAILED &&
              bcmjob_read(res.out[0].slot, 0, 0, 61, 1, px, 60) == BCMJOB_FAILED &&
              bcmjob_read(BJC_SLOTS, 0, 0, 1, 1, px, 60) == BCMJOB_FAILED,
              "rects outside, not a slot: FAILED");
        p_restart();
        CHECK(bcmjob_read(res.out[0].slot, 0, 0, 10, 1, px, 60) == BCMJOB_OFF,
              "another epoch: OFF");
    }
    t_decode_threads();
    t_cross_core();
    CHECK(lock_depth == 0 && bufs_out == 0, "after decodes: lock %d, buffers %d", lock_depth,
          bufs_out);
    t_dec_lent();
    t_ovl_swap();
    dec_offer = false;
    fresh();
    for (k = 0; k < 3; k++)
        free(bad[k]);
    free(jpg);
    free(big);
    free(dwork);
    free(twork);
}

/* ---- bcmjob_upload_rect (19-4, jobs rev 0.4): the PP's pixels into BACK ---- */
static bool not_owner(void) { return false; }
static const struct bjc_owner other_owner = {
    NULL, not_owner, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
};

static void t_upload_rect(void)
{
    static uint16_t pic[W * H], before[W * H];
    uint16_t *back = (uint16_t *)(mem + BJC_BACK_ADDR);
    uint32_t i, y, ok = 1, ops;
    int r;

    CHECK(BJC_BACK_ADDR == BACK, "BACK's address: the memory contract's");
    fresh();
    for (i = 0; i < W * H; i++)
        pic[i] = (uint16_t)(i * 40503u >> 7);
    /* the whole frame: engine 2 in 16-row bands */
    e2_bytes = 0;
    r = bcmjob_upload_rect(J_SURF_BACK, 0, 0, W, H, pic, W);
    CHECK(r == BCMJOB_OK && !memcmp(back, pic, sizeof(pic)) && e2_bytes == W * H * 2 &&
          lock_depth == 0, "the whole frame: %d, engine 2 %u B", r, e2_bytes);
    /* an odd rect and pitch: its pixels, nothing beside them */
    memcpy(before, back, sizeof(before));
    r = bcmjob_upload_rect(J_SURF_BACK, 3, 5, 51, 20, pic + 7, 61);
    for (y = 0; y < 20; y++)
        ok &= !memcmp(back + (5 + y) * W + 3, pic + 7 + y * 61, 51 * 2) &&
              back[(5 + y) * W + 2] == before[(5 + y) * W + 2] &&
              back[(5 + y) * W + 54] == before[(5 + y) * W + 54];
    ok &= !memcmp(back, before, 5 * W * 2) &&
          !memcmp(back + 25 * W, before + 25 * W, (H - 25) * W * 2);
    CHECK(r == BCMJOB_OK && ok, "an odd rect: %d", r);
    /* outside the frame, another surface: FAILED */
    CHECK(bcmjob_upload_rect(J_SURF_BACK, 300, 0, 21, 1, pic, W) == BCMJOB_FAILED &&
          bcmjob_upload_rect(J_SURF_BACK, 0, 239, 1, 2, pic, W) == BCMJOB_FAILED &&
          bcmjob_upload_rect(J_SURF_FRONT, 0, 0, 1, 1, pic, W) == BCMJOB_FAILED &&
          bcmjob_upload_rect(J_SURF_BACK, 0, 0, 20, 2, pic, 19) == BCMJOB_FAILED,
          "rects outside, FRONT, pitch < w: FAILED");
    /* a restart between pieces: OFF (the epoch moved) */
    restart_at_block = 3;
    r = bcmjob_upload_rect(J_SURF_BACK, 1, 0, 100, 200, pic, W);
    restart_at_block = 0;
    CHECK(r == BCMJOB_OFF && bcmjob_epoch() == epoch_seen, "a restart mid-rect: %d", r);
    /* suspended, cut: OFF, nothing written */
    bjc_suspend_request(true, 0);
    ops = port_ops;
    CHECK(bcmjob_upload_rect(J_SURF_BACK, 0, 0, 8, 8, pic, W) == BCMJOB_OFF &&
          port_ops == ops, "suspended: OFF");
    bjc_suspend_request(false, 0);
    p_power(false);
    CHECK(bcmjob_upload_rect(J_SURF_BACK, 0, 0, 8, 8, pic, W) == BCMJOB_OFF, "cut: OFF");
    p_power(true);
    /* another thread: never the port */
    bjc_set_owner(&other_owner);
    CHECK(bcmjob_upload_rect(J_SURF_BACK, 0, 0, 8, 8, pic, W) == BCMJOB_FAILED,
          "not the ring owner: FAILED");
    bjc_set_owner(NULL);
    /* a slow port: the call stops at its budget (BUSY, the frame's CPU
       path) instead of holding the UI for the whole rect */
    CHECK(bcmjob_have(J_OP_FILL), "up again");
    nlines = 0;
    port_us = 1500;
    i = now_us_;
    r = bcmjob_upload_rect(J_SURF_BACK, 0, 0, W, H, pic, W);
    i = now_us_ - i;
    port_us = 0;
    CHECK(r == BCMJOB_BUSY && i <= BJC_RECT_US && logged("bcmjob upload_rect deferred"),
          "a slow port: %d after %u us", r, i);
    CHECK(bcmjob_upload_rect(J_SURF_BACK, 0, 0, W, H, pic, W) == BCMJOB_OK &&
          !memcmp(back, pic, sizeof(pic)), "the next frame's upload goes");
    /* a DECODE in flight has the port's time: BUSY, uncounted */
    dec_offer = true;
    fresh();
    {
        struct bcmjob_dec_out o = dout(142, 142, 0, 0, 142, 142, 0x9000);
        struct bcmjob_dec_res res;
        struct tsrc ts;

        jpg = load("tests/ui/fixtures/sig-cover.jpg", &jpg_len);
        dwork = malloc(J_DEC_WORK_MIN);
        twork = malloc(J_DEC_WORK_MIN);
        rect_mid = BCMJOB_OK;
        nlines = 0;
        r = decode1(jpg, jpg_len, &o, 1, 0, 0, true, &res, &ts);
        CHECK(r == BCMJOB_OK && rect_mid == BCMJOB_BUSY &&
              logged("bcmjob fault op=0x00 why=busy-decode") &&
              bcmjob_upload_rect(J_SURF_BACK, 0, 0, 8, 8, pic, W) == BCMJOB_OK,
              "upload_rect during a decode: %d, after %d", rect_mid, r);
        free(jpg);
        free(dwork);
        free(twork);
    }
    dec_offer = false;
    CHECK(lock_depth == 0, "lock %d", lock_depth);
}

/* ---- the staged upload (jobs rev 0.5, candidate images; #67) ---------- */

static bool slot_holds(uint32_t slot, const uint16_t *pic, uint32_t w, uint32_t h)
{
    const uint16_t *px = jref_slot_px(&chip, slot);
    uint32_t r;

    if (!px || chip.slot[slot].state != J_ST_READY)
        return false;
    for (r = 0; r < h; r++)
        if (memcmp(px + r * chip.slot[slot].surf.pitch, pic + r * w, 2 * w))
            return false;
    return true;
}

static int logged_has(const char *prefix, const char *part)
{
    int i;

    for (i = 0; i < nlines; i++)
        if (!strncmp(lines[i], prefix, strlen(prefix)) && strstr(lines[i], part))
            return 1;
    return 0;
}

static void t_staged(void)
{
    static const uint32_t cases_[][3] = {
        { J_CLS_STRIP, 300, 300 }, { J_CLS_STRIP, 400, 160 }, { J_CLS_STRIP, 301, 7 },
        { J_CLS_DRIFT, 8, 1 }, { J_CLS_DRIFT, 301, 7 }, { J_CLS_DRIFT, 100, 100 },
        { J_CLS_THUMB, 55, 55 }, { J_CLS_SCRATCH, 128, 128 }, { J_CLS_STRIP, 1, 1 },
        { J_CLS_STRIP, 3, 17 }, { J_CLS_STRIP, 320, 16 },
    };
    /* pic covers the largest upload below (the 400 x 300 direct slot) */
    static uint16_t pic[400 * 300], pic2[300 * 300];
    struct bcmjob_slot s, s2;
    uint32_t i, k, seq, e2;
    char want[160];

    for (i = 0; i < 400 * 300; i++)
        pic[i] = (uint16_t)(i * 2654435761u >> 13);
    for (i = 0; i < 300 * 300; i++)
        pic2[i] = (uint16_t)(i * 40503u + 7);
    /* an image without the form: J_CAPS without its magic is no cap, and
       ALLOC goes out as rev 0.4's (five words), the slot's own address */
    stage_offer = false;
    caps_garbage = 0x00010001;
    fresh();
    CHECK(bcmjob_slot(J_CLS_STRIP, 64, 16, 0x5701, 0, &s) == BCMJOB_OK &&
          !logged("bcmjob caps=") &&
          !slots[s.slot].staged && slots[s.slot].addr >= STRIPS &&
          slots[s.slot].addr < STRIPS + STRIPS_BYTES, "rev 0.4 image: the slot");
    CHECK(bcmjob_upload(&s, pic) == BCMJOB_OK && bcmjob_fence(&seq) == BCMJOB_OK &&
          bcmjob_wait(seq, 200) == BCMJOB_OK && slot_holds(s.slot, pic, 64, 16),
          "rev 0.4 image: direct upload");
    caps_garbage = 0;

    stage_offer = true;
    fresh();
    for (k = 0; k < sizeof(cases_) / sizeof(cases_[0]); k++)
    {
        uint32_t cls = cases_[k][0], w = cases_[k][1], h = cases_[k][2];

        e2 = e2_bytes;
        stage_moves = 0;
        CHECK(bcmjob_slot(cls, w, h, 0x5800 + k, 1, &s) == BCMJOB_OK && !s.hit,
              "staged ALLOC %u: %ux%u", cls, w, h);
        CHECK(slots[s.slot].staged && slots[s.slot].addr == STAGE &&
              slots[s.slot].pitch == 2 * J_SLOT_PITCH(cls, w),
              "%ux%u: the window (%08x, pitch %u)", w, h, slots[s.slot].addr,
              slots[s.slot].pitch);
        CHECK(bcmjob_upload(&s, pic) == BCMJOB_OK, "%ux%u: upload", w, h);
        CHECK(bcmjob_fence(&seq) == BCMJOB_OK && bcmjob_wait(seq, 300) == BCMJOB_OK,
              "%ux%u: fenced", w, h);
        CHECK(stage_moves == 1 && slot_holds(s.slot, pic, w, h),
              "%ux%u: the slot holds the pixels (moved %u)", w, h, stage_moves);
        CHECK((h >= 16 && J_SLOT_PITCH(cls, w) == w && !((w * h) & 1)) == (e2_bytes > e2),
              "%ux%u: engine 2 exactly for the whole-run uploads", w, h);
        CHECK(!logged("bcmjob fault") && !logged("bcmjob alloc"), "%ux%u: no faults", w, h);
        CHECK(logged("bcmjob caps=0x1 staged"), "the caps line");
        CHECK(bcmjob_slot(cls, w, h, 0x5800 + k, 1, &s2) == BCMJOB_OK && s2.hit &&
              !slots[s2.slot].staged && slots[s2.slot].addr != STAGE,
              "%ux%u: a hit names the slot itself", w, h);
        bcmjob_submit(J_OP_FREE, DROP_TAG, &s.slot, 1);
    }
    /* a slot past the window: rev 0.4's answer, logged */
    CHECK(bcmjob_slot(J_CLS_STRIP, 400, 300, 0x5900, 1, &s) == BCMJOB_OK &&
          !slots[s.slot].staged && slots[s.slot].addr != STAGE &&
          logged("bcmjob alloc slot="), "a slot larger than the window: direct");
    CHECK(bcmjob_upload(&s, pic) == BCMJOB_OK && bcmjob_fence(&seq) == BCMJOB_OK &&
          bcmjob_wait(seq, 300) == BCMJOB_OK && slot_holds(s.slot, pic, 300, 1),
          "... and its direct upload");
    bcmjob_submit(J_OP_FREE, DROP_TAG, &s.slot, 1);

    /* an epoch change mid-upload (the BCM's RESET between ALLOC and
       COMMIT): COMMIT's J_E_SLOT is logged with its status, not counted,
       and the next staged upload works */
    fresh();
    CHECK(bcmjob_slot(J_CLS_STRIP, 64, 32, 0x5A01, 0, &s) == BCMJOB_OK &&
          slots[s.slot].staged, "staged ALLOC before the RESET");
    CHECK(bcmjob_submit(J_OP_RESET, 0, NULL, 0) == BCMJOB_OK, "RESET");
    CHECK(bcmjob_upload(&s, pic) == BCMJOB_OK && bcmjob_fence(&seq) == BCMJOB_OK &&
          bcmjob_wait(seq, 300) == BCMJOB_OK, "the upload goes, the COMMIT fails");
    snprintf(want, sizeof(want), "st=%d why=failed", J_E_SLOT);
    CHECK(logged_has("bcmjob fault op=0x22 ", want), "J_E_SLOT logged");
    CHECK(bcmjob_slot(J_CLS_STRIP, 64, 32, 0x5A02, 0, &s) == BCMJOB_OK &&
          slots[s.slot].staged && bcmjob_upload(&s, pic2) == BCMJOB_OK &&
          bcmjob_fence(&seq) == BCMJOB_OK && bcmjob_wait(seq, 300) == BCMJOB_OK &&
          slot_holds(s.slot, pic2, 64, 32), "staged again after the epoch change");
    CHECK(bcmjob_have(J_OP_COMMIT), "COMMIT not marked failed");
    /* ... and a power cut mid-upload: the window's owner gone with the
       image, the next power-up's staged upload works */
    fresh();
    CHECK(bcmjob_slot(J_CLS_STRIP, 301, 7, 0x5A03, 0, &s) == BCMJOB_OK, "ALLOC");
    restart_at_block = 3;
    CHECK(bcmjob_upload(&s, pic) != BCMJOB_OK || bcmjob_fence(&seq) != BCMJOB_OK ||
          bcmjob_wait(seq, 300) != BCMJOB_OK || !slot_holds(s.slot, pic, 301, 7),
          "a restart mid-upload: not committed");
    restart_at_block = 0;
    CHECK(bcmjob_slot(J_CLS_STRIP, 301, 7, 0x5A04, 0, &s) == BCMJOB_OK &&
          slots[s.slot].staged && bcmjob_upload(&s, pic2) == BCMJOB_OK &&
          bcmjob_fence(&seq) == BCMJOB_OK && bcmjob_wait(seq, 300) == BCMJOB_OK &&
          slot_holds(s.slot, pic2, 301, 7), "staged after the restart");

    /* the sum mismatch: a window word lost after the PP wrote it.  The
       J_E_SUM line has the PP's sums, the BCM's (the torn pixels'), the
       slot's first word, how (the window) and the window's B: equal to the
       BCM's, so the upload is to blame, not the move */
    fresh();
    CHECK(bcmjob_slot(J_CLS_STRIP, 300, 300, 0x5B01, 0, &s) == BCMJOB_OK &&
          slots[s.slot].staged, "staged cover");
    stalled = true;
    CHECK(bcmjob_upload(&s, pic) == BCMJOB_OK, "upload");
    {
        uint32_t ppa = 0, ppb = 0, ba = 0, bb = 0;
        uint16_t *win = (uint16_t *)(mem + STAGE);

        uint32_t r, wp = slots[s.slot].pitch / 2;

        win[4321] ^= 0x8001;
        j_f16w(&ppa, &ppb, pic, 300 * 300);
        for (r = 0; r < 300; r++)           /* the window at the slot's pitch */
            j_f16w(&ba, &bb, win + r * wp, 300);
        stalled = false;
        CHECK(bcmjob_fence(&seq) == BCMJOB_OK && bcmjob_wait(seq, 300) == BCMJOB_OK,
              "fenced");
        snprintf(want, sizeof(want), "st=%d why=failed", J_E_SUM);
        CHECK(logged_has("bcmjob fault op=0x22 ", want), "the J_E_SUM fault");
        snprintf(want, sizeof(want), "bcmjob sum slot=%u pp=%08x,%08x bcm=%08x,%08x "
                 "w0=%08x how=%d win=%08x", s.slot, ppa, ppb, ba, bb,
                 pic[0] | (uint32_t)pic[1] << 16, J_HOW_DMA, bb);
        CHECK(logged(want), "the J_E_SUM line: %s", want);
    }
    CHECK(bcmjob_have(J_OP_COMMIT), "J_E_SUM not counted toward the failed mark");
    stage_offer = false;
}

/* ---- the visualizer: SPECTRUM's reply and the overlay load (#126) --------- */
#define OVL_WORDS 2048                  /* the fake overlay: 8 KB loaded */
static uint32_t ovl_file[OVL_WORDS];
static uint32_t ovl_piece_buf[640];     /* the caller's scratch: 2.5 KB */

static uint32_t fnv1a(const void *p, uint32_t n)
{
    const uint8_t *b = p;
    uint32_t v = 2166136261u;

    while (n--)
        v = (v ^ *b++) * 16777619u;
    return v;
}

static void ovl_make(uint32_t abi)
{
    uint32_t i;

    for (i = 0; i < OVL_WORDS; i++)
        ovl_file[i] = i * 2654435761u ^ 0x5A5A1234;
    ovl_file[0] = J_OVL_MAGIC;
    ovl_file[1] = abi;
    ovl_file[2] = ovl_base;
    ovl_file[3] = ovl_base + 4 * OVL_WORDS;
    ovl_file[4] = ovl_base + 4 * OVL_WORDS + 512;    /* its .bss */
}

/* bcmjob-dev.c bcmjob_ovl_read() and _put()'s steps over the file in memory: the
 * first piece's header to begin, every piece summed and written, end */
/* ... each call of begin and end timed (the clock's us across the call:
 * a call never waits for the BCM, sitting 4c) and asked again after a
 * yield while it answers BJC_OVL_AGAIN (the owner's next pass) */
static uint32_t ovl_agains, ovl_call_max;

static int ovl_call(int end, int kind, uint32_t size, uint32_t fnv, uint32_t a,
                    uint32_t b2)
{
    uint32_t t0;
    int r;

    for (;;)
    {
        t0 = now_us_;
        r = end ? bjc_ovl_end(kind, fnv, a, b2)
                : bjc_ovl_begin(kind, ovl_piece_buf, size);
        if (now_us_ - t0 > ovl_call_max)
            ovl_call_max = now_us_ - t0;
        if (r != BJC_OVL_AGAIN)
            return r;
        ovl_agains++;
        p_yield();
    }
}

static int ovl_load_k(int kind, const struct bjc_ovl_rec *rec, uint32_t fnv_xor)
{
    const uint8_t *f = (const uint8_t *)ovl_file;
    uint32_t size = sizeof(ovl_file), off = 0, n, a = 0, b2 = 0, fnv = 2166136261u, k;
    int r;

    n = sizeof(ovl_piece_buf) < size ? sizeof(ovl_piece_buf) : size;
    memcpy(ovl_piece_buf, f, n);
    bjc_ovl_set(kind, rec);
    r = ovl_call(0, kind, size, 0, 0, 0);
    while (r == BCMJOB_OK)
    {
        for (k = 0; k < n; k++)
            fnv = (fnv ^ ((const uint8_t *)ovl_piece_buf)[k]) * 16777619u;
        j_f16w(&a, &b2, (const uint16_t *)ovl_piece_buf, n / 2);
        r = bjc_ovl_piece(kind, off, ovl_piece_buf, n);
        off += n;
        if (r != BCMJOB_OK || off >= size)
            break;
        n = size - off < sizeof(ovl_piece_buf) ? size - off : sizeof(ovl_piece_buf);
        memcpy(ovl_piece_buf, f + off, n);
    }
    if (r == BCMJOB_OK)
        r = ovl_call(1, kind, size, fnv ^ fnv_xor, a, b2);
    return r;
}

static int ovl_load(const struct bjc_ovl_rec *rec, uint32_t fnv_xor)
{
    return ovl_load_k(BJC_OVL_VIZ, rec, fnv_xor);
}

/* the C's levels for the same window and bands (jviz.c: the CPU path) */
static void spectrum_ref(const int16_t *pcm, uint32_t n, const uint16_t *e, uint32_t nb,
                         uint16_t *l2)
{
    static uint32_t p[JVIZ_M_MAX];

    jviz_power_ref(p, pcm, n);
    jviz_bands(l2, p, n / 2, e, nb);
}

static void ovl_drop_chip(void)
{
    ovl_loaded = false;
    *w32(BLOCK + J_OPS) &= ~OVL_BITS;
}

/* The line ops' PP side (bcmjob-viz.c): each wrapper's record, through
 * the ring into the chip's overlay, draws what jviz.h draws with the same
 * values (the shell's CPU fallback), pixel for pixel; sl: the window */
static void t_lines(const struct bcmjob_slot *sl)
{
    static uint16_t want[W * H], lut[256];
    uint8_t ln[48], pk[48], lv[24];
    struct bcmjob_slot ls;
    uint32_t i, seq;
    uint16_t *back = (uint16_t *)(mem + BACK);
    struct bcmjob_poly pq = { J_SURF_BACK, 4, 6, 300, 198, sl->slot, 0, 1, 320, 0, 240,
                              256, 0xF81F, 3 };
    struct bcmjob_segs sq = { J_SURF_BACK, 0, 0, W, H, W / 2, H / 2, 48, 0, 1, 3,
                              J_SEG_CAP | J_SEG_RING | J_SEG_ROUND, 30, 85, 26, 3, 1,
                              0x4A7F, 0xDF1F, 0xFFFF, 0x2104, ln, pk };
    struct bcmjob_palrow rq = { J_SURF_BACK, 0, 0, W, 4, 0, 24, lv };
    struct jviz_poly jp = { chip.slot[sl->slot].surf.px, 512, 320, 0, 1, 256, 240, 3, 0,
                            0xF81F };
    struct jviz_segs js = { 48, 0, 1, 3, J_SEG_CAP | J_SEG_RING | J_SEG_ROUND, 30, 85, 26,
                            3, 1, W / 2, H / 2, 0x4A7F, 0xDF1F, 0xFFFF, 0x2104, ln, pk };
    struct jviz_pal jr = { lut, lv, 24 };

    for (i = 0; i < 256; i++)
        lut[i] = (uint16_t)(i * 0x0101 ^ 0x1234);
    for (i = 0; i < 48; i++)
    {
        ln[i] = (uint8_t)(i * 37 % 97);
        pk[i] = (uint8_t)(ln[i] + i % 7);
        if (i < 24)
            lv[i] = (uint8_t)(i * 11);
    }
    CHECK(bcmjob_have_polyline() && bcmjob_have_segments() && bcmjob_have_palrow(),
          "the line ops offered after the load");
    CHECK(bcmjob_slot(J_CLS_STRIP, 16, 16, 0x4C555430, 0, &ls) == BCMJOB_OK &&
          bcmjob_upload(&ls, lut) == BCMJOB_OK, "the LUT's slot");
    rq.lut = ls.slot;
    memcpy(want, back, sizeof(want));
    CHECK(jviz_poly_draw(want + 6 * W + 4, W, 300, 198, &jp) == 0 &&
          jviz_segs_draw(want, W, W, H, &js) == 0 && jviz_pal_draw(want, W, W, 4, &jr) == 0,
          "the CPU's frame");
    CHECK(bcmjob_viz_polyline(&pq) == BCMJOB_OK && bcmjob_viz_segments(&sq) == BCMJOB_OK &&
          bcmjob_viz_palrow(&rq) == BCMJOB_OK && bcmjob_fence(&seq) == BCMJOB_OK &&
          bcmjob_wait(seq, 100) == BCMJOB_OK, "POLYLINE, SEGMENTS, PALROW queued and run");
    CHECK(!logged("bcmjob fault op=0x36") && !logged("bcmjob fault op=0x37") &&
          !logged("bcmjob fault op=0x38"), "no faults");
    CHECK(!memcmp(want, back, sizeof(want)), "the BCM's frame equals the CPU's");
    sq.n = 49;
    CHECK(bcmjob_viz_segments(&sq) == BCMJOB_FAILED, "49 segments refused on the PP");
    CHECK(logged_has("bcmjob overlay ok", "lines=PSR"), "the ok line lists the line ops");
}

static void t_overlay(void)
{
    static int16_t pcm[512];
    uint16_t edges[25], l2[24], ref[24];
    struct bcmjob_slot sl;
    struct bjc_ovl_rec rec;
    uint32_t i, cmds, ep, seq, bars[J_BARS_ARGS(4)] = { 0 };
    char want[64];

    ovl_make(OVL_ABI);
    rec.bytes = sizeof(ovl_file);
    rec.fnv = fnv1a(ovl_file, sizeof(ovl_file));
    rec.abi = OVL_ABI;
    rec.abi_at = OVL_ABI_AT;
    rec.base = OVL_BASE;
    for (i = 0; i < 512; i++)
        pcm[i] = (int16_t)(12000 * ((int)(i * 37 % 64) - 32) / 32 + (i & 1 ? 900 : -900));
    for (i = 0; i <= 24; i++)
        edges[i] = (uint16_t)(1 + i * 10);

    /* the merged image: no OVERLAY, nothing to load, SPECTRUM refused */
    ovl_offer = false;
    fresh();
    CHECK(bcmjob_have(J_OP_FILL) && !bjc_ovl_due(BJC_OVL_VIZ), "no OVERLAY: no load due");
    bjc_ovl_set(BJC_OVL_VIZ, &rec);
    CHECK(bjc_ovl_begin(BJC_OVL_VIZ, ovl_file, sizeof(ovl_file)) == BCMJOB_NOJOB &&
          !logged("bcmjob overlay"), "begin refused without a line");
    CHECK(bcmjob_spectrum(0, 512, edges, 24, l2) == BCMJOB_NOJOB, "SPECTRUM: NOJOB");

    /* the display-only image: the load, J_OPS read back, SPECTRUM's reply */
    ovl_offer = true;
    stub_abi = image_abi = OVL_ABI;
    fresh();
    bjc_ovl_set(BJC_OVL_VIZ, &rec);
    CHECK(bjc_ovl_due(BJC_OVL_VIZ) && !bcmjob_have(J_OP_SPECTRUM), "OVERLAY offered: a load is due");
    e2_bytes = 0;
    cmds = ovl_cmds;
    CHECK(ovl_load(&rec, 0) == BCMJOB_OK, "the load");
    CHECK(ovl_cmds == cmds + 1 && ovl_loaded, "one OVERLAY, accepted");
    CHECK(!memcmp(mem + OVL_BASE, ovl_file, sizeof(ovl_file)), "the region holds the file");
    CHECK(e2_bytes >= sizeof(ovl_file) / 2, "engine 2 moved its whole pieces (%u)", e2_bytes);
    CHECK(bcmjob_have(J_OP_SPECTRUM) && bcmjob_have(J_OP_BARS) && !bjc_ovl_due(BJC_OVL_VIZ),
          "J_OPS read back: SPECTRUM and BARS");
    CHECK(logged("bcmjob overlay load k=0 bytes=8192") && logged("bcmjob overlay ok ops="),
          "load and ok lines");
    for (i = 0, ep = 0; (int)i < nlines; i++)
        ep |= !strncmp(lines[i], "bcmjob overlay ok", 17) && strstr(lines[i], " flip") != 0;
    CHECK(bcmjob_bars_flip() && ep, "J_VPU_FLIP read back: BARS flips, the ok line says so");
    CHECK(lock_depth == 0, "the lock balanced");
    CHECK(bcmjob_slot(J_CLS_STRIP, 512, 1, 0x56495a30, 0, &sl) == BCMJOB_OK &&
          bcmjob_upload(&sl, (const uint16_t *)pcm) == BCMJOB_OK, "the window's slot");
    CHECK(bcmjob_spectrum(sl.slot, 512, edges, 24, l2) == BCMJOB_OK, "SPECTRUM 512");
    spectrum_ref(pcm, 512, edges, 24, ref);
    CHECK(!memcmp(l2, ref, sizeof(ref)), "its 24 levels equal the C's");
    CHECK(bcmjob_spectrum(sl.slot, 256, edges, 12, l2) == BCMJOB_OK, "SPECTRUM 256");
    spectrum_ref(pcm, 256, edges, 12, ref);
    CHECK(!memcmp(l2, ref, 12 * 2), "its 12 levels equal the C's");
    bars[0] = J_SURF_BACK;
    bars[1] = J_XY(0, 0);
    bars[2] = J_XY(64, 32);
    bars[3] = 4 | 6 << 8 | 2 << 16 | 2u << 24;
    bars[4] = 0x001F | 0xF800u << 16;
    bars[5] = 0x07E0 | 0xFFFFu << 16;
    bars[6] = J_BARS_BG | J_BARS_CAP;
    bars[7] = 5 | 30 << 8 | 0 << 16 | 32u << 24;
    bars[8] = 9 | 31 << 8 | 1 << 16 | 32u << 24;
    CHECK(bcmjob_submit(J_OP_BARS, 0, bars, J_BARS_ARGS(4)) == BCMJOB_OK &&
          bcmjob_fence(&seq) == BCMJOB_OK && bcmjob_wait(seq, 100) == BCMJOB_OK &&
          !logged("bcmjob fault op=0x34"), "BARS runs in the overlay");

    bars[6] |= J_BARS_FLIP;
    CHECK(bcmjob_submit(J_OP_BARS, 0, bars, J_BARS_ARGS(4)) == BCMJOB_OK &&
          bcmjob_fence(&seq) == BCMJOB_OK && bcmjob_wait(seq, 100) == BCMJOB_OK &&
          !logged("bcmjob fault op=0x34"), "flipped BARS runs in the overlay");
    bars[6] &= ~J_BARS_FLIP;
    t_lines(&sl);

    /* the epoch: a restart (cut, our image again) drops the ops; due again,
       and the re-upload restores them */
    p_restart();
    CHECK(!bcmjob_have(J_OP_SPECTRUM) && bjc_ovl_due(BJC_OVL_VIZ), "after a restart: gone, due");
    CHECK(!bcmjob_bars_flip(), "after a restart: no flip (no BARS)");
    /* an overlay before the flag: BARS, no flip (the shell's CPU Mirror) */
    ovl_noflip = true;
    CHECK(ovl_load(&rec, 0) == BCMJOB_OK && bcmjob_have(J_OP_BARS) && !bcmjob_bars_flip(),
          "an overlay before J_BARS_FLIP: BARS without the flip");
    ovl_noflip = false;
    /* SEGMENTS' start-up check failed: the rest offered, the overlay whole
       (no reload), the ok line says which (the shell's Radial on the CPU) */
    p_restart();
    ovl_nolines = 2;
    CHECK(ovl_load(&rec, 0) == BCMJOB_OK && bcmjob_have_polyline() &&
          !bcmjob_have_segments() && bcmjob_have_palrow() && !bjc_ovl_due(BJC_OVL_VIZ) &&
          logged_has("bcmjob overlay ok", "lines=P-R"), "SEGMENTS not offered: POLYLINE and PALROW still are");
    ovl_nolines = 0;
    p_restart();
    CHECK(ovl_load(&rec, 0) == BCMJOB_OK && bcmjob_have(J_OP_SPECTRUM),
          "re-uploaded after the restart");
    /* a mode change (J_EV_EPOCH): the BCM drops them; drained, due again */
    ep = bcmjob_epoch();
    ovl_drop_chip();
    chip.epoch++;
    evt_put(J_EV_EPOCH, 0, &chip.epoch, 1);
    publish();
    CHECK(bcmjob_fence(&seq) == BCMJOB_OK && bcmjob_wait(seq, 50) == BCMJOB_OK, "drained");
    CHECK(bcmjob_epoch() != ep && !bcmjob_have(J_OP_SPECTRUM) && bjc_ovl_due(BJC_OVL_VIZ),
          "after J_EV_EPOCH: gone, due");
    CHECK(ovl_load(&rec, 0) == BCMJOB_OK && bcmjob_have(J_OP_BARS), "re-uploaded after it");
    /* the PP's own RESET drops them too */
    CHECK(bcmjob_submit(J_OP_RESET, 0, NULL, 0) == BCMJOB_OK && !bcmjob_have(J_OP_SPECTRUM),
          "a RESET put: gone");
    ovl_drop_chip();
    CHECK(bjc_ovl_due(BJC_OVL_VIZ) && ovl_load(&rec, 0) == BCMJOB_OK, "re-uploaded after a RESET");
    /* the BCM dropped them before the PP drained the event: the op's
       HP_E_UNKNOWN is not a failure, and the ops go until the next load */
    ovl_drop_chip();
    CHECK(bcmjob_spectrum(sl.slot, 512, edges, 24, l2) == BCMJOB_FAILED, "SPECTRUM refused");
    CHECK(logged("bcmjob overlay gone op=0x33") && !bcmjob_have(J_OP_SPECTRUM) &&
          bjc_ovl_due(BJC_OVL_VIZ), "gone, not counted, due");
    CHECK(ovl_load(&rec, 0) == BCMJOB_OK, "loaded again");
    ovl_drop_chip();
    CHECK(bcmjob_spectrum(sl.slot, 512, edges, 24, l2) == BCMJOB_FAILED &&
          ovl_load(&rec, 0) == BCMJOB_OK &&
          bcmjob_slot(J_CLS_STRIP, 512, 1, 0x56495a31, 0, &sl) == BCMJOB_OK &&
          bcmjob_upload(&sl, (const uint16_t *)pcm) == BCMJOB_OK &&
          bcmjob_spectrum(sl.slot, 512, edges, 24, l2) == BCMJOB_OK,
          "twice gone: never marked failed");

    /* a torn upload (J_E_SUM): one more try this epoch, which succeeds */
    p_restart();
    nlines = 0;
    tear_upload = true;
    CHECK(ovl_load(&rec, 0) == BCMJOB_FAILED && !ovl_loaded, "torn: refused");
    snprintf(want, sizeof(want), "bcmjob overlay refused st=%d why=sum", J_E_SUM);
    CHECK(logged(want), "the sum line");
    CHECK(bjc_ovl_due(BJC_OVL_VIZ) && ovl_load(&rec, 0) == BCMJOB_OK, "retried once: loaded");
    /* torn twice: no third try this epoch; the next epoch tries again */
    p_restart();
    tear_upload = true;
    CHECK(ovl_load(&rec, 0) == BCMJOB_FAILED, "torn");
    tear_upload = true;
    CHECK(ovl_load(&rec, 0) == BCMJOB_FAILED && !bjc_ovl_due(BJC_OVL_VIZ), "torn again: none due");
    p_restart();
    CHECK(bjc_ovl_due(BJC_OVL_VIZ) && ovl_load(&rec, 0) == BCMJOB_OK, "the next epoch loads");

    /* the running image is another build (its own ABI word, read in
       place): refused before any byte is written, this epoch only */
    image_abi = OVL_ABI ^ 0x40;
    p_restart();
    nlines = 0;
    cmds = ovl_cmds;
    memset(mem + OVL_BASE, 0, sizeof(ovl_file));
    CHECK(ovl_load(&rec, 0) == BCMJOB_FAILED && ovl_cmds == cmds &&
          logged("bcmjob overlay refused why=image"), "another image: refused, no OVERLAY");
    CHECK(fnv1a(mem + OVL_BASE, 64) == fnv1a((uint8_t[64]){ 0 }, 64), "the region untouched");
    CHECK(!bjc_ovl_due(BJC_OVL_VIZ), "not again this epoch");
    image_abi = OVL_ABI;
    p_restart();
    CHECK(ovl_load(&rec, 0) == BCMJOB_OK, "the right image: loaded");

    /* the file isn't the build's: its ABI word against the record */
    p_restart();
    nlines = 0;
    cmds = ovl_cmds;
    ovl_make(OVL_ABI ^ 0x1000);
    CHECK(ovl_load(&rec, 0) == BCMJOB_FAILED && ovl_cmds == cmds &&
          logged("bcmjob overlay refused why=file"), "another file: refused");
    ovl_make(OVL_ABI);
    p_restart();
    CHECK(!bjc_ovl_due(BJC_OVL_VIZ), "a bad file: never again this boot");

    /* the bytes streamed aren't the record's (FNV-1a): no OVERLAY */
    fresh();
    cmds = ovl_cmds;
    CHECK(ovl_load(&rec, 0x1) == BCMJOB_FAILED && ovl_cmds == cmds &&
          logged("bcmjob overlay refused why=file"), "the FNV: refused, no OVERLAY");

    /* the BCM's own check: its stub's ABI differs (the PP's record can't
       see it: abi_at 0): HP_E_VERSION, never retried */
    stub_abi = OVL_ABI ^ 0x2;
    fresh();
    rec.abi_at = 0;
    snprintf(want, sizeof(want), "bcmjob overlay refused st=%d why=version", HP_E_VERSION);
    CHECK(ovl_load(&rec, 0) == BCMJOB_FAILED && logged(want) && !ovl_loaded,
          "HP_E_VERSION: refused");
    p_restart();
    CHECK(!bjc_ovl_due(BJC_OVL_VIZ) && !bcmjob_have(J_OP_SPECTRUM), "never retried this boot");
    stub_abi = OVL_ABI;
    rec.abi_at = OVL_ABI_AT;

    /* a header outside the region (J_OVL): refused, this epoch */
    fresh();
    ovl_file[4] = OVL_BASE + OVL_SIZE + 4;
    rec.fnv = fnv1a(ovl_file, sizeof(ovl_file));
    CHECK(ovl_load(&rec, 0) == BCMJOB_FAILED && logged("bcmjob overlay refused why=region"),
          "past the region: refused");
    ovl_make(OVL_ABI);
    rec.fnv = fnv1a(ovl_file, sizeof(ovl_file));

    /* a job in flight is retired before the region is written */
    fresh();
    {
        uint32_t fill[4] = { J_SURF_FRONT, J_XY(0, 0), J_XY(8, 8), 0x1F };

        CHECK(bcmjob_submit(J_OP_FILL, 0, fill, 4) == BCMJOB_OK, "a FILL in flight");
        nlines = 0;
        CHECK(ovl_load(&rec, 0) == BCMJOB_OK && logged("bcmjob fence seq="),
              "fenced first, then loaded");
    }

    /* sitting 4c: no call waits for the BCM.  OVERLAY takes 30 ms there:
       the end answers AGAIN until its reply, each call short */
    fresh();
    ovl_agains = ovl_call_max = 0;
    hold_op = J_OP_OVERLAY;
    hold_us = 30000;
    nlines = 0;
    CHECK(ovl_load(&rec, 0) == BCMJOB_OK && ovl_loaded && bcmjob_have(J_OP_SPECTRUM),
          "a slow OVERLAY: loaded");
    CHECK(ovl_agains >= 100 && ovl_call_max < 2000,
          "its reply polled, never waited for (AGAIN %u, longest call %u us)",
          ovl_agains, ovl_call_max);
    CHECK(logged("bcmjob overlay ok ops=") && !logged("bcmjob overlay refused"),
          "the ok line, as the blocking call's");
    /* ... the begin's FENCE behind a slow job in flight: AGAIN, then the
       load line once it retired */
    fresh();
    {
        uint32_t fill[4] = { J_SURF_FRONT, J_XY(0, 0), J_XY(8, 8), 0x1F };

        hold_op = J_OP_FILL;
        hold_us = 20000;
        CHECK(bcmjob_submit(J_OP_FILL, 0, fill, 4) == BCMJOB_OK, "a slow FILL in flight");
        nlines = 0;
        bjc_ovl_set(BJC_OVL_VIZ, &rec);
        memcpy(ovl_piece_buf, ovl_file, sizeof(ovl_piece_buf));
        CHECK(bjc_ovl_begin(BJC_OVL_VIZ, ovl_piece_buf, sizeof(ovl_file)) == BJC_OVL_AGAIN &&
              !logged("bcmjob overlay load"), "begin: the FENCE posted, AGAIN, no load yet");
        ovl_agains = ovl_call_max = 0;
        CHECK(ovl_load(&rec, 0) == BCMJOB_OK && ovl_agains > 0 && ovl_call_max < 2000 &&
              logged("bcmjob overlay load k=0"), "asked again: the FENCE retired, loaded");
    }
    /* an end never asked again (the caller withdrew), then a new epoch:
       the next load begins afresh, and no reply box is lost (three in a
       row, then the owner's own calls still have theirs) */
    for (i = 0; i < 3; i++)
    {
        uint32_t fnv = 2166136261u, a = 0, b2 = 0, k, off;

        p_restart();
        hold_op = J_OP_OVERLAY;
        hold_us = 30000;
        hold_at = 0;
        bjc_ovl_set(BJC_OVL_VIZ, &rec);
        memcpy(ovl_piece_buf, ovl_file, sizeof(ovl_piece_buf));
        CHECK(ovl_call(0, BJC_OVL_VIZ, sizeof(ovl_file), 0, 0, 0) == BCMJOB_OK, "begun");
        for (off = 0; off < sizeof(ovl_file); off += 4096)
        {
            for (k = 0; k < 4096; k++)
                fnv = (fnv ^ ((const uint8_t *)ovl_file)[off + k]) * 16777619u;
            j_f16w(&a, &b2, (const uint16_t *)((const uint8_t *)ovl_file + off), 2048);
            bjc_ovl_piece(BJC_OVL_VIZ, off, (const uint8_t *)ovl_file + off, 4096);
        }
        k = (uint32_t)bjc_ovl_end(BJC_OVL_VIZ, fnv, a, b2);
        CHECK(k == BJC_OVL_AGAIN, "the end posted: AGAIN (%d, try %u)", (int)k, i);
    }
    hold_op = hold_us = hold_at = 0;
    p_restart();
    nlines = 0;
    CHECK(bjc_ovl_end(BJC_OVL_VIZ, 0, 0, 0) == BCMJOB_OFF, "asked after the epoch: OFF");
    CHECK(ovl_load(&rec, 0) == BCMJOB_OK && bcmjob_have(J_OP_SPECTRUM), "the next load");
    CHECK(bcmjob_slot(J_CLS_STRIP, 512, 1, 0x56495a32, 0, &sl) == BCMJOB_OK &&
          bcmjob_slot(J_CLS_STRIP, 256, 1, 0x56495a33, 0, &sl) == BCMJOB_OK,
          "the owner's calls: their reply boxes all there");
    /* OVERLAY past its bound (J_T_FENCE_BASE + J_T_OVERLAY): TIMEOUT, the
       blocking call's line, not again this epoch */
    p_restart();
    hold_op = J_OP_OVERLAY;
    hold_us = 1000 * (J_T_FENCE_BASE + J_T_OVERLAY) + 50000;
    hold_at = 0;
    nlines = 0;
    snprintf(want, sizeof(want), "bcmjob overlay refused why=wire rc=%d", BCMJOB_TIMEOUT);
    ep = (uint32_t)ovl_load(&rec, 0);
    CHECK((int)ep == BCMJOB_TIMEOUT && logged(want) && !bjc_ovl_due(BJC_OVL_VIZ),
          "a slow OVERLAY past its bound: TIMEOUT (%d)", (int)ep);
    hold_op = hold_us = hold_at = 0;
    p_restart();
    CHECK(lock_depth == 0, "the lock balanced (%d)", lock_depth);
    ovl_offer = false;
}

int main(void)
{
    t_round_trip();
    t_commit_status();
    t_slots();
    t_odd_width();
    t_cut_mid_scroll();
    t_death_mid_slide();
    t_ring_full();
    t_dead_twice();
    t_slow();
    t_dark_hour();
    t_requests();
    t_mode();
    t_vpu();
    jdec_yield = chip_yield;
    t_decode();
    t_upload_rect();
    t_resync_order();
    t_staged();
    t_overlay();
#ifdef HAVE_BCMFW_DEVTEST
    t_inject();
#endif
    CHECK(lock_depth == 0, "the port lock balanced (%d)", lock_depth);
    CHECK(lone_tail == 0, "no port block with a lone trailing halfword (%u)",
          lone_tail);
    printf("bcmjob: %d cases, %d failed\n", cases, failed);
    return failed != 0;
}
