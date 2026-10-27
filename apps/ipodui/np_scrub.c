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
 * iPod UI shell: Now Playing scrub maths
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
/* Pure C on purpose: no Rockbox headers (host-tested by tests/nowplaying/).
 * Every chapter lookup is a linear scan of the start column (no sort, no
 * buffer): 648 chapters are ~650 compares per wheel event or per chapter
 * change, a few us on the PP5022 at 30 MHz. */
#include "np_scrub.h"

unsigned long np_scrub_step_ms(unsigned velocity)
{
    unsigned shift = 0;

    if (velocity >= NP_SCRUB_ACCEL_V)
    {
        shift = 1 + (velocity - NP_SCRUB_ACCEL_V) / NP_SCRUB_ACCEL_DV;
        if (shift > NP_SCRUB_ACCEL_MAX)
            shift = NP_SCRUB_ACCEL_MAX;
    }
    return (unsigned long)NP_SCRUB_BASE_MS << shift;
}

/* smallest base + start in (pos, best), or best if none */
static unsigned long next_start(const struct np_chapters *ch,
                                unsigned long base, unsigned long pos,
                                unsigned long best)
{
    int i;

    for (i = 0; i < ch->count; i++)
    {
        unsigned long s = base + np_chapter_start(ch, i);
        if (s > pos && s < best)
            best = s;
    }
    return best;
}

/* largest base + start in (best, pos) (strictly before pos), or best */
static unsigned long prev_start(const struct np_chapters *ch,
                                unsigned long base, unsigned long pos,
                                unsigned long best)
{
    int i;

    for (i = 0; i < ch->count; i++)
    {
        unsigned long s = base + np_chapter_start(ch, i);
        if (s < pos && s > best)
            best = s;
    }
    return best;
}

unsigned long np_scrub_wheel(const struct np_scrub_ctx *ctx,
                             unsigned long pos_ms, int clicks,
                             unsigned velocity)
{
    unsigned long len = ctx->length_ms;
    unsigned long pos = pos_ms;

    if (len == 0 || clicks == 0)
        return pos > len ? len : pos;
    if (pos > len)
        pos = len;

    if (ctx->snap && (ctx->ch.count > 0 || ctx->sub.count > 0)
        && velocity >= NP_SCRUB_SNAP_V)
    {
        int n = clicks < 0 ? -clicks : clicks;
        while (n-- > 0)
        {
            /* the union of both sets (sub is empty but in a multi-file
             * book: the same loop as Phase 5 otherwise) */
            unsigned long np = clicks > 0
                ? next_start(&ctx->sub, ctx->sub_base, pos,
                             next_start(&ctx->ch, 0, pos, len))
                : prev_start(&ctx->sub, ctx->sub_base, pos,
                             prev_start(&ctx->ch, 0, pos, 0));
            if (np == pos)
                break;              /* at an end */
            pos = np;
        }
        return pos;
    }

    {
        unsigned long step = np_scrub_step_ms(velocity);
        unsigned long n = clicks < 0 ? (unsigned long)-(long)clicks
                                     : (unsigned long)clicks;
        /* n * step can overflow 32 bits only for absurd inputs;
         * saturate instead */
        unsigned long d = (n > 0xffffffffUL / step) ? 0xffffffffUL
                                                    : n * step;
        if (clicks > 0)
            pos = (d >= len - pos) ? len : pos + d;
        else
            pos = (d >= pos) ? 0 : pos - d;
    }
    return pos;
}

int np_chapter_at(const struct np_chapters *ch, unsigned long pos_ms,
                  int *arr_idx)
{
    int i, best = -1, first = -1, rank = 0;
    unsigned long bs = 0, fs = 0;

    if (ch->count <= 0)
    {
        if (arr_idx)
            *arr_idx = -1;
        return -1;
    }
    /* latest start <= pos (first in array order on ties), and the earliest
     * start overall for positions before every start */
    for (i = 0; i < ch->count; i++)
    {
        unsigned long s = np_chapter_start(ch, i);
        if (s <= pos_ms && (best < 0 || s > bs))
        {
            best = i;
            bs = s;
        }
        if (first < 0 || s < fs)
        {
            first = i;
            fs = s;
        }
    }
    if (best < 0)
    {
        best = first;
        bs = fs;
    }
    /* rank in time order: starts before it, plus equal starts earlier in
     * the array */
    for (i = 0; i < ch->count; i++)
    {
        unsigned long s = np_chapter_start(ch, i);
        if (s < bs || (s == bs && i < best))
            rank++;
    }
    if (arr_idx)
        *arr_idx = best;
    return rank;
}

int np_bar_x(unsigned long pos_ms, unsigned long length_ms, int w)
{
    if (length_ms == 0 || w <= 0)
        return 0;
    if (pos_ms >= length_ms)
        return w;
    /* pos * w must fit 32 bits: an 11 h book is 4.2e7 ms, x 320 px is not.
     * Drop low bits of both instead of 64-bit maths (no __aeabi_uldivmod
     * on the ARM7TDMI); the error is far below a pixel. */
    while (length_ms > 0xffffffffUL / (unsigned)w)
    {
        length_ms >>= 1;
        pos_ms >>= 1;
    }
    return (int)(pos_ms * (unsigned)w / length_ms);
}

int np_chapter_ticks(const struct np_chapters *ch, unsigned long length_ms,
                     int w, uint8_t *bits)
{
    int i, n = 0;

    if (w <= 0)
        return 0;
    for (i = 0; i < (w + 7) / 8; i++)
        bits[i] = 0;
    if (length_ms == 0)
        return 0;
    for (i = 0; i < ch->count; i++)
    {
        unsigned long s = np_chapter_start(ch, i);
        int x;
        uint8_t m;

        if (s == 0 || s >= length_ms)
            continue;
        x = np_bar_x(s, length_ms, w);
        if (x <= 0 || x >= w)
            continue;               /* would sit on the bar's ends */
        m = (uint8_t)(1u << (x & 7));
        if (!(bits[x >> 3] & m))
        {
            bits[x >> 3] |= m;
            n++;
        }
    }
    return n;
}

int np_chapter_step(const struct np_chapters *ch, unsigned long pos_ms,
                    int hint)
{
    int i = hint, n = ch->count;

    if (n <= 0)
        return -1;
    if (i < 0)
        i = 0;
    if (i >= n)
        i = n - 1;
    while (i > 0 && np_chapter_start(ch, i) > pos_ms)
        i--;
    while (i + 1 < n && np_chapter_start(ch, i + 1) <= pos_ms)
        i++;
    while (i > 0 && np_chapter_start(ch, i - 1) == np_chapter_start(ch, i))
        i--;
    return i;
}
