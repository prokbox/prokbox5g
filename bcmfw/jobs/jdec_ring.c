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

/* jdec's source over the input ring (jdec_ring.h) */

#include <stddef.h>
#include <stdint.h>
#include "hostproto.h"
#include "jobs.h"
#include "jdec_ring.h"

void *memcpy(void *dst, const void *src, size_t n);    /* lib/string.c */

#define UNITS(r)    ((r)->bytes / J_DEC_UNIT)

int jdec_ring_ok(uint32_t bytes)
{
    return bytes >= 4096 && bytes <= 128 * 1024 && !(bytes & (bytes - 1));
}

static void publish(struct jdec_ring *r)
{
    *r->rd_word = HP_ENC(J_DEC_IDX(r->pass, r->done));
}

static uint32_t next_pass(uint32_t p)
{
    return p + 1 < J_DEC_PASSES ? p + 1 : 0;
}

/* A new pass: the PP starts the file again at ring byte 0 */
static void new_pass(struct jdec_ring *r)
{
    r->pass = next_pass(r->pass);
    r->pos = r->have = r->done = 0;
    publish(r);
}

/* The PP's word, if it is whole and names this pass: more units */
static void poll_wr(struct jdec_ring *r)
{
    uint32_t w = *r->wr_word, d;

    if (!HP_VALID(w))
        return;                 /* half-written: next time */
    w &= 0xFFFF;
    if (w == J_DEC_ABORT)
    {
        r->err = 1;
        return;
    }
    if (J_DEC_PASS(w) != r->pass)
        return;                 /* still the old pass, or not started */
    d = (J_DEC_UNITS(w) - r->have) & 0xFFF;
    if (r->have + d - r->done <= UNITS(r))  /* never more than a ring ahead */
        r->have += d;
}

/* Bytes of the file readable now from pos (0 while waiting); waits up to
 * J_DEC_STALL_MS for the first */
static uint32_t avail(struct jdec_ring *r)
{
    uint32_t t0 = r->now_us(), top;

    for (;;)
    {
        if (r->err)
            return 0;
        top = r->have * J_DEC_UNIT;
        if (top > r->len)
            top = r->len;
        if (top > r->pos)
            return top - r->pos;
        if (r->pos >= r->len)
            return 0;
        if (r->now_us() - t0 > J_DEC_STALL_MS * 1000u)
        {
            r->err = 1;
            return 0;
        }
        poll_wr(r);
    }
}

/* pos moved: the units behind it are the PP's again */
static void consumed(struct jdec_ring *r)
{
    uint32_t u = r->pos / J_DEC_UNIT;

    if (r->pos >= r->len)
        u = (r->len + J_DEC_UNIT - 1) / J_DEC_UNIT;
    if (u > r->done)
    {
        r->done = u;
        publish(r);
    }
}

static int ring_start(void *ctx)
{
    struct jdec_ring *r = ctx;

    if (r->err)
        return -1;
    if (r->used++ && r->pos)
    {
        if (r->len <= r->bytes)
            r->pos = 0;         /* the whole file is still in the ring */
        else
            new_pass(r);
    }
    return 0;
}

static long ring_fetch(void *ctx, void *buf, uint32_t n)
{
    struct jdec_ring *r = ctx;
    uint8_t *d = buf;
    uint32_t got = 0, k, at;

    while (got < n)
    {
        k = avail(r);
        if (!k)
            break;
        at = r->pos & (r->bytes - 1);
        if (k > n - got)
            k = n - got;
        if (k > r->bytes - at)
            k = r->bytes - at;
        memcpy(d + got, r->buf + at, k);
        got += k;
        r->pos += k;
        consumed(r);
    }
    return r->err ? -1 : (long)got;
}

static int ring_skip(void *ctx, uint32_t n)
{
    struct jdec_ring *r = ctx;
    uint32_t k;

    while (n)
    {
        k = avail(r);
        if (!k)
            return -1;
        if (k > n)
            k = n;
        r->pos += k;
        n -= k;
        consumed(r);
    }
    return 0;
}

void jdec_ring_src(struct jdec_src *src, struct jdec_ring *r, uint32_t len)
{
    uint32_t w = *r->rd_word;

    r->pass = HP_VALID(w) ? J_DEC_PASS(w & 0xFFFF) % J_DEC_PASSES : 0;
    r->len = len;
    r->used = 0;
    r->err = 0;
    /* the PP writes nothing until it sees the new pass: its word from the
       last job (an abort, or a pass that comes round again) is cleared */
    *r->wr_word = HP_ENC(J_DEC_IDLE);
    new_pass(r);
    src->len = len;
    src->start = ring_start;
    src->fetch = ring_fetch;
    src->skip = ring_skip;
    src->ctx = r;
}
