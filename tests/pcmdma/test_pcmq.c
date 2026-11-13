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

/* The two-channel PCM DMA queue (0.8.3 fifo96, pcm-pp.c "pcm-queue").
 *
 * Runs the REAL queue block of firmware/target/arm/pp/pcm-pp.c (pq_arm,
 * pq_fill, pq_start, pq_fiq: run.sh extracts the lines between its
 * "pcm-queue: begin" and "pcm-queue: end" markers) against:
 *  - an engine model of the property the boot probe proves on silicon:
 *    two channels on one request are served one after the other, in the
 *    order they were started; reading a status acknowledges;
 *  - a 16-word I2S FIFO the engine refills when 4 or more are free, and
 *    a codec that takes one word a tick;
 *  - the real mixer's double buffer's index logic (pcm_mixer.c: the
 *    callback hands out buf[idx]; PCM_DMAST_STARTED flips idx and mixes
 *    into the other), each mix taking MIX ticks while the engine runs on,
 *    and every word stamped with a sequence number;
 *  - a FIQ that runs LAT ticks after a completion (fixed or random).
 *
 * Asserts: the codec receives every stamped word once, in order; the
 * mixer never writes a buffer the engine still holds; the FIFO never runs
 * dry between the first word and the end while the FIQ's latency plus the
 * mix fit inside one piece.  And the arms that must fail (red runs, in
 * the same binary): a latency longer than a piece must show dry counts
 * and underruns; an engine that serves both channels at once (what the
 * boot probe refuses) must show order errors. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IBSS_ATTR
#define ICODE_ATTR
#define NUM_CORES 1
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
static unsigned long uncached_base;
#define UNCACHED_BASE_ADDR uncached_base
#define UNCACHED_ADDR(a) (a)
static void commit_dcache(void) {}
#include "dma_defs_q.h"         /* DMA_CMD_*, DMA_STATUS_*, DMA_REQ_IIS */

enum pcm_dma_status { PCM_DMAST_ERR_DMA = -1, PCM_DMAST_OK = 0,
                      PCM_DMAST_STARTED = 1 };
bool pcm_play_dma_complete_callback(enum pcm_dma_status status,
                                    const void **addr, size_t *size);
static enum pcm_dma_status pcm_play_dma_status_callback(enum pcm_dma_status s);

/* ---- the engine model ---- */
struct chan { unsigned long cmd, status, ram, shadow_cmd;
              size_t left; int busy, intr, order; };
static struct chan ch[2];
static int start_seq, rr_engine, lifo_engine, nofiq1;
static unsigned long junk;

/* A START written since the last look begins that channel's turn, in the
 * order the channels were started.  Every register access looks first,
 * so two arms in a row are seen in their order */
static void engine_sync(void)
{
    for (int c = 0; c < 2; c++)
    {
        if ((ch[c].cmd & DMA_CMD_START) && ch[c].cmd != ch[c].shadow_cmd)
        {
            ch[c].busy = 1;
            ch[c].intr = 0;
            ch[c].left = (ch[c].cmd & 0xffff) + 4;
            ch[c].order = ++start_seq;
        }
        else if (!(ch[c].cmd & DMA_CMD_START))
            ch[c].busy = 0;
        ch[c].shadow_cmd = ch[c].cmd;
    }
}

static volatile unsigned long *pq_reg(int c, int off)
{
    engine_sync();
    switch (off)
    {
    case 0x00:
        return &ch[c].cmd;
    case 0x04:
        /* a read: busy, interrupt (acknowledged by the read), what is
         * left as the size field counts it */
        ch[c].status = ((unsigned long)ch[c].busy << 31) |
                       ((unsigned long)ch[c].intr << 30) |
                       (ch[c].busy ? (ch[c].left - 4) & 0xffff
                                   : ch[c].cmd & 0xffff);
        ch[c].intr = 0;
        return &ch[c].status;
    case 0x10:
        return &ch[c].ram;
    }
    return &junk;
}
#define PQ_REG(c, off) (*pq_reg((c), (off)))

/* ---- what the queue block expects ---- */
struct dma_data
{
    union { unsigned long addr; const void *p_r; };
    size_t size;
    int locked;
    int state;
};
static struct dma_data dma_play_data;
static uint16_t max_dma_chunk_size;
static long pieces, dry, odd, slack_min, late_max;
#define PCM_PP_PIECE_DONE() (pieces++)
#define PQ_STAT_PIECE()     do { } while (0)
#define PQ_STAT_DRY()       (dry++)
#define PQ_STAT_ODD()       (odd++)
#define PQ_STAT_LATE(b)     do { if ((long)(b) / 4 > late_max) \
                                     late_max = (long)(b) / 4; } while (0)
#define PQ_STAT_RESTART()   do { } while (0)
#define PQ_STAT_SLACK(st)   do { long s_ = (long)((st) & 0xfffc) / 4 + 1; \
                                 if (s_ < slack_min) slack_min = s_; } while (0)
#define PQ_STAT_FELL()      (fell++)
static long fell;
/* the one-channel path the queue falls back to: channel 0's registers */
#define DMA0_CMD      PQ_REG(0, 0x00)
#define DMA0_STATUS   PQ_REG(0, 0x04)
#define DMA0_RAM_ADDR PQ_REG(0, 0x10)
#define PCM_PP_ARMED(size) do { (void)(size); } while (0)
/* #312: pq_stop()'s bounded wait lives outside the queue block (the
 * pcm-wait block; tests/pcmwait runs the real one): here, poll this
 * model's channels until idle, a bounded number of looks */
#define PCM_WAIT_CH0 1
#define PCM_WAIT_CH1 2
static void pcm_wait_bounded(unsigned int what)
{
    for (int n = 0; n < 1000; n++)
    {
        unsigned long st = 0;

        if (what & PCM_WAIT_CH0)
            st |= PQ_REG(0, 0x04);
        if (what & PCM_WAIT_CH1)
            st |= PQ_REG(1, 0x04);
        if (!(st & (DMA_STATUS_BUSY | DMA_STATUS_INTR)))
            return;
    }
}
#include "dma_play_config.h"    /* the real DMA_PLAY_CONFIG */
#include "dma_tx_buf_prepare.c" /* the real dma_tx_buf_prepare() */
#include "dma_tx_start.c"       /* the real one-channel dma_tx_start() */
#include "pcmq_parts.c"         /* the real queue block */
#include "fiq_playback.c"       /* the real one-channel FIQ (stock form) */

/* ---- the codec, the FIFO and time ---- */
#define FIFO 16
static uint32_t fifo[FIFO];
static int fifo_n, started_audio, stopped;
static uint32_t expect_seq;
static long underruns, bad_order, ticks, words_out;

static void tick(void)
{
    ticks++;
    if (fifo_n > 0)
    {
        uint32_t w = fifo[0];

        memmove(fifo, fifo + 1, (size_t)(fifo_n - 1) * 4);
        fifo_n--;
        if (w != expect_seq && bad_order++ < 3)
            printf("  order: got word %u, expected %u (tick %ld)\n",
                   (unsigned)w, (unsigned)expect_seq, ticks);
        expect_seq = w + 1;
        words_out++;
        started_audio = 1;
    }
    else if (started_audio && !stopped)
        underruns++;     /* the codec found the FIFO empty: a click */
    /* the request: 4 or more free; the engine fills the FIFO */
    engine_sync();
    if (FIFO - fifo_n >= 4)
    {
        static int flip;

        while (fifo_n < FIFO)
        {
            int c = -1;

            for (int i = 0; i < 2; i++)
                if (ch[i].busy && ch[i].left &&
                    (c < 0 || (lifo_engine ? ch[i].order > ch[c].order
                                           : ch[i].order < ch[c].order)))
                    c = i;
            if (c < 0)
                break;
            /* the refused engine: the other busy channel takes every
             * second word */
            if (rr_engine && ch[c ^ 1].busy && ch[c ^ 1].left && (flip ^= 1))
                c ^= 1;
            fifo[fifo_n++] = *(const uint32_t *)ch[c].ram;
            ch[c].ram += 4;
            ch[c].left -= 4;
            if (ch[c].left == 0)
            {
                ch[c].busy = 0;
                ch[c].intr = 1;
                ch[c].cmd &= ~DMA_CMD_START;    /* single: it stops */
                ch[c].shadow_cmd = ch[c].cmd;
            }
        }
    }
}

/* ---- the mixer: the real double buffer's index logic ---- */
#define FRAME_BYTES 1024
static uint32_t buf[2][FRAME_BYTES / 4];
static int idx, handed, nbuf = 200;
static uint32_t stamp;
static long conflicts, mix_ticks;

static int hw_holds(int b)
{
    unsigned long lo = (unsigned long)buf[b], hi = lo + FRAME_BYTES;

    for (int c = 0; c < 2; c++)
        if (ch[c].busy && ch[c].ram >= lo && ch[c].ram < hi)
            return 1;
    return 0;
}

static void mix_into(int b)
{
    if (hw_holds(b) && conflicts++ < 3)
        printf("  the mixer wrote buffer %d while the engine held it\n", b);
    for (int i = 0; i < FRAME_BYTES / 4; i++)
        buf[b][i] = stamp++;
    for (long t = 0; t < mix_ticks; t++)
        tick();                 /* the mix takes time; the engine runs on */
}

static enum pcm_dma_status pcm_play_dma_status_callback(enum pcm_dma_status s)
{
    if (s == PCM_DMAST_STARTED)
    {
        idx ^= 1;
        mix_into(idx);
    }
    return s;
}

bool pcm_play_dma_complete_callback(enum pcm_dma_status status,
                                    const void **addr, size_t *size)
{
    (void)status;
    if (handed >= nbuf)
    {
        /* the PCM layer stops the PCM (pq_stop) */
        stopped = 1;
        if (pq_on)
            pq_stop();
        else
            ch[0].cmd = 0;
        engine_sync();
        pq_n = 0;
        dma_play_data.state = 0;
        return false;
    }
    handed++;
    *addr = buf[idx];
    *size = FRAME_BYTES;
    return true;
}

/* ---- one run ---- */
struct run
{
    const char *name;
    unsigned int chunk;
    long lat, lat_rand, mix;
    int rr, want_clean;
    int first_prompt;   /* the first FIQ on time (the start's half pieces) */
    int fault;          /* 1: channel 1 raises no FIQ; 2: the engine serves
                           the newest first (odd); 3: a stop mid-play */
};

static long run_one(const struct run *r, long *o_dry, long *o_und,
                    long *o_bad, long *o_conf)
{
    int first;

    memset(ch, 0, sizeof(ch));
    memset(&dma_play_data, 0, sizeof(dma_play_data));
    fifo_n = started_audio = stopped = 0;
    expect_seq = stamp = 0;
    underruns = bad_order = ticks = words_out = conflicts = 0;
    pieces = dry = late_max = 0;
    slack_min = 1L << 30;
    start_seq = 0;
    rr_engine = r->rr;
    nofiq1 = r->fault == 1;
    lifo_engine = r->fault == 2;
    fell = odd = 0;
    pq_on = 1;                  /* what a proved probe sets */
    pq_fail = 0;
    max_dma_chunk_size = (uint16_t)r->chunk;
    mix_ticks = r->mix;
    handed = 0;
    srand(12345);

    /* mixer_start_pcm(): two STARTED mixes, the first one plays */
    idx = 0;
    idx ^= 1;
    mix_into(idx);
    first = idx;
    idx ^= 1;
    mix_into(idx);
    dma_play_data.p_r = buf[first];
    dma_play_data.size = FRAME_BYTES;
    dma_play_data.addr = dma_tx_buf_prepare(dma_play_data.p_r);
    dma_play_data.state = 1;
    pq_start();
    engine_sync();

    for (long guard = 0, nfiq = 0; guard < 4000000; guard++)
    {
        if (r->fault == 3 && handed == nbuf / 2 && pq_n)
        {
            /* a stop mid-play (pcm_play_stop): nothing may play after */
            long w0;

            pq_stop();
            stopped = 1;
            w0 = words_out;
            for (int t = 0; t < 600; t++)
                tick();
            if (ch[0].busy || ch[1].busy || ch[0].intr || ch[1].intr ||
                words_out > w0 + FIFO || pq_n)
            {
                printf("FAIL: %s: the engine ran on after the stop\n",
                       r->name);
                bad_order += 1000;
            }
            break;
        }
        if (ch[0].intr || (ch[1].intr && !nofiq1))
        {
            long lat = r->lat + (r->lat_rand ? rand() % (r->lat_rand + 1) : 0);

            if (nfiq++ == 0 && r->first_prompt)
                lat = 0;

            for (long t = 0; t < lat; t++)
                tick();
            if (pq_on)
                pq_fiq();
            else
                fiq_playback();         /* the one-channel path */
            if (!dma_play_data.state && handed >= nbuf)
                break;
        }
        else
            tick();
    }
    for (int t = 0; t < 64; t++)
        tick();
    *o_dry = dry;
    *o_und = underruns;
    *o_bad = bad_order;
    *o_conf = conflicts;
    printf("%-33s chunk %4u lat %3ld+%-3ld mix %2ld: %4ld pieces, %ld words, "
           "dry %ld, underrun ticks %ld, order errors %ld, mixer conflicts "
           "%ld, slack min %ld words, late max %ld words\n",
           r->name, r->chunk, r->lat, r->lat_rand, r->mix, pieces, words_out,
           dry, underruns, bad_order, conflicts,
           slack_min == (1L << 30) ? -1 : slack_min, late_max);
    return words_out;
}

int main(void)
{
    /* The real sink_set_freq's pieces: 96 kHz 24-bit 3000 B, 96 kHz 16-bit
     * 1500, 44.1 kHz 16-bit 688, 8 kHz 124.  A 1 KB frame is 256 words:
     * one piece at 3000 and 1500, 172 + 84 words at 688.  One tick is one
     * FIFO word (5.2 us at 96 kHz 24-bit, 22.7 us at 44.1 kHz 16-bit) */
    /* The owner's 24/192 run saw FIQs ~200 us late: 38 words at 96 kHz
     * 24-bit, where one channel lasts 16.  A piece is 256 words there, the
     * start's two halves 128 */
    static const struct run runs[] = {
        { "queue, prompt FIQ",               3000,   0,   0,  0, 0, 1, 0, 0 },
        { "queue, 96k/24: every FIQ 200 late",3000, 200,   0, 40, 0, 1, 1, 0 },
        { "queue, 96k/24: random 0-120",     3000,   0, 120, 40, 0, 1, 0, 0 },
        { "queue, 96k/16: random 0-120",     1500,   0, 120, 40, 0, 1, 0, 0 },
        { "queue, 44.1k/16: random 0-60",     688,   0,  60, 10, 0, 1, 0, 0 },
        { "queue, 8k: random 0-20",           124,   0,  20,  4, 0, 1, 0, 0 },
        /* the arms that must fail */
        { "RED: FIQ later than a piece",     3000, 300,   0,  0, 0, 0, 0, 0 },
        { "RED: engine serves both at once", 3000,   0,   0,  0, 1, 0, 0, 0 },
        /* faults the queue must survive (review of p083-fifo96): it falls
         * back to one channel for the session */
        { "fault: channel 1 raises no FIQ",  3000,   0,   0, 40, 0, 0, 0, 1 },
        { "fault: 44.1k, ch 1 no FIQ",        688,   0,   0, 10, 0, 0, 0, 1 },
        { "fault: newest piece served first",3000,   0,   0, 40, 0, 0, 0, 2 },
        { "stop mid-play",                   3000,   0,  60, 10, 0, 0, 0, 3 },
    };
    const size_t n = sizeof(runs) / sizeof(runs[0]);
    long checked = 0;
    int fail = 0;

    for (size_t i = 0; i < n; i++)
    {
        const struct run *r = &runs[i];
        long d, u, b, c;
        long w = run_one(r, &d, &u, &b, &c);
        bool clean = d == 0 && u == 0 && b == 0 && c == 0;

        checked += w;
        if (r->fault)
        {
            /* fell back once, after at most two dry hand-overs; then the
             * one-channel path plays every word in order */
            /* the odd engine played pieces out of order (and the mixer
             * may have refilled one it held) before it was caught */
            if (r->fault != 3 && (pq_on || fell != 1 || d > 3 ||
                                  u > 128 ||
                                  (r->fault == 1 ? b != 0 : b > 2 * 256) ||
                                  (r->fault == 1 ? c != 0 : c > 1) ||
                                  w < (long)nbuf * FRAME_BYTES / 4 - 512))
            {
                printf("FAIL: %s: no clean fallback (queue %s, fell %ld, "
                       "dry %ld, underrun %ld, order %ld, conflicts %ld)\n",
                       r->name, pq_on ? "still on" : "off", fell, d, u, b, c);
                fail = 1;
            }
            if (r->fault == 3 && (b || c || u))
            {
                printf("FAIL: %s: not clean before the stop\n", r->name);
                fail = 1;
            }
            continue;
        }
        if (w < (long)nbuf * FRAME_BYTES / 4)
        {
            printf("FAIL: %s: only %ld words reached the codec\n", r->name, w);
            fail = 1;
        }
        if (r->want_clean && !clean)
        {
            printf("FAIL: %s: not clean\n", r->name);
            fail = 1;
        }
        if (!r->want_clean && clean)
        {
            printf("FAIL: %s: the check did not fire (it cannot fail)\n",
                   r->name);
            fail = 1;
        }
        if (!r->want_clean && r->lat > 0 && (d == 0 || u == 0))
        {
            printf("FAIL: %s: a FIQ later than a piece without a dry count "
                   "and an underrun\n", r->name);
            fail = 1;
        }
        if (!r->want_clean && r->rr && b == 0)
        {
            printf("FAIL: %s: interleaved words not seen\n", r->name);
            fail = 1;
        }
    }
    printf("pcmq: %zu runs, %ld words checked\n", n, checked);
    if (checked == 0)
    {
        printf("FAIL: nothing checked\n");
        return 1;
    }
    return fail;
}
