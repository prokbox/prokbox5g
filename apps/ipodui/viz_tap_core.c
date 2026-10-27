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

/* The visualizer's PCM tap, the pure part: viz_tap_core.h */

#include <stddef.h>
#include <string.h>
#include "viz_tap_core.h"

/* The compiler must not move the copy's loads past the second read of
 * head (the ARM7 itself is in order; on the host this is enough for the
 * test's interleavings, which are explicit) */
#define VT_BARRIER() __asm__ __volatile__("" ::: "memory")
/* tests/viztap: the writer running in the middle of a read (an FIQ) */
#ifndef VT_MIDREAD
#define VT_MIDREAD(r) do { } while (0)
#endif

uint32_t viz_ring_frames(uint32_t n, uint32_t dec, uint32_t fmax)
{
    uint32_t need = n * dec + 2 * fmax, f = 1;

    while (f < need)
        f <<= 1;
    return f;
}

void viz_ring_init(struct viz_ring *r, uint32_t *buf, uint32_t frames,
                   uint32_t fmax)
{
    uint32_t i;

    r->buf = buf;
    r->mask = frames - 1;
    r->fmax = fmax;
    r->w = 0;
    r->head = 0;
    for (i = 0; i < frames; i++)
        buf[i] = 0;
}

void viz_ring_write(struct viz_ring *r, const uint32_t *src, uint32_t frames)
{
    uint32_t w = r->w, at = w & r->mask, run = r->mask + 1 - at;

    r->head = w;            /* everything before this frame plays first */
    if (frames > r->fmax)
        frames = r->fmax;   /* never past what the ring was sized for */
    r->w = w + frames;
    if (run > frames)
        run = frames;
    memcpy(r->buf + at, src, run * 4);  /* the firmware's: IRAM, ldm/stm */
    if (frames > run)                   /* the wrap */
        memcpy(r->buf, src + run, (frames - run) * 4);
}

uint32_t viz_tap_dec(uint32_t rate)
{
    return rate > 128000 ? 4 : rate > 64000 ? 2 : 1;
}

/* One sample from dec frames starting at frame index p (signed: before
 * the first frame written reads as silence) */
static inline int32_t vt_sample(const struct viz_ring *r, int32_t p,
                                uint32_t dec, uint32_t sh)
{
    int32_t s = 0;
    uint32_t j;

    for (j = 0; j < dec; j++, p++)
    {
        uint32_t v;
        if (p < 0)
            continue;
        v = r->buf[(uint32_t)p & r->mask];
        s += (int32_t)(int16_t)(v & 0xffff) + (int32_t)(int16_t)(v >> 16);
    }
    return s >> sh;         /* floored: (L + R) / 2, averaged over dec */
}

/* The device's case (dec 1, a full window): both samples of a pair from
 * one pass, the extremes kept on the way; returns the peak |x| */
static int32_t vt_read_pairs(const struct viz_ring *r, uint32_t p,
                             int16_t *ev, int16_t *od, uint32_t half)
{
    const uint32_t *buf = r->buf;
    uint32_t mask = r->mask, k;
    int32_t hi = 0, lo = 0;

    for (k = 0; k < half; k++, p += 2)
    {
        uint32_t a = buf[p & mask], b = buf[(p + 1) & mask];
        int32_t x = ((int32_t)(int16_t)a + ((int32_t)a >> 16)) >> 1;
        int32_t y = ((int32_t)(int16_t)b + ((int32_t)b >> 16)) >> 1;
        ev[k] = (int16_t)x;
        od[k] = (int16_t)y;
        if (x > hi) hi = x;
        if (x < lo) lo = x;
        if (y > hi) hi = y;
        if (y < lo) lo = y;
    }
    return -lo > hi ? -lo : hi;
}

unsigned viz_ring_read(const struct viz_ring *r, int16_t *out, uint32_t n,
                       uint32_t dec, uint32_t last_head, unsigned opts,
                       struct viz_tap_frame *f)
{
    uint32_t need = n * dec, half = n / 2, size = r->mask + 1;
    uint32_t sh = 1 + (dec >= 4 ? 2 : dec >= 2 ? 1 : 0);
    uint32_t h1, h2, k, tries;
    unsigned flags = 0;
    int32_t peak = 0;

    for (tries = 0; ; tries++)
    {
        int32_t base;
        h1 = r->head;
        VT_BARRIER();
        base = (int32_t)(h1 - need);
        if (h1 < need)
            base = -(int32_t)(need - h1);
        if (dec == 1 && h1 >= need)
            peak = vt_read_pairs(r, h1 - need, out, out + half, half);
        else
        {
            for (k = 0; k < half; k++)
            {
                int32_t p = base + (int32_t)(2 * k * dec);
                out[k] = (int16_t)vt_sample(r, p, dec, sh);
                out[half + k] = (int16_t)vt_sample(r, p + (int32_t)dec,
                                                   dec, sh);
            }
            peak = -1;      /* found below */
        }
        VT_MIDREAD(r);
        VT_BARRIER();
        h2 = r->head;
        /* the writer is at most fmax past h2: did it reach our oldest? */
        if ((h2 - h1) + r->fmax + need <= size)
            break;
        if (tries)
        {
            for (k = 0; k < n; k++)
                out[k] = 0;
            flags |= VIZ_TAP_TORN;
            peak = 0;
            break;
        }
    }
    if (h1 < need)
        flags |= VIZ_TAP_UNDERRUN;
    if (h1 == last_head)
        flags |= VIZ_TAP_STALE;

    if (peak < 0)           /* the general path: scan for it */
    {
        peak = 0;
        for (k = 0; k < n; k++)
        {
            int32_t a = out[k] < 0 ? -(int32_t)out[k] : out[k];
            if (a > peak)
                peak = a;
        }
    }
    if (peak > 32767)
        peak = 32767;       /* -32768 */
    if (peak <= VIZ_TAP_SILENT_PEAK)
        flags |= VIZ_TAP_SILENT;

    f->head = h1;
    f->peak = (int16_t)peak;
    f->shift = 0;
    if ((opts & VIZ_RD_NORM) && !(flags & VIZ_TAP_SILENT))
    {
        uint32_t s = 0;
        while (s < VIZ_TAP_NORM_MAX && (peak << (s + 1)) <= 32767)
            s++;
        if (s)
            for (k = 0; k < n; k++)
                out[k] = (int16_t)(out[k] * (1 << s));
        f->shift = (uint8_t)s;
    }
    return flags;
}
