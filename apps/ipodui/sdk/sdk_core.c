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

/* The SDK's pure rules (sdk_core.h) */
#include <string.h>
#include "ipodui/sdk/sdk_core.h"

/* -- the exit chord ------------------------------------------------------ */
int sdk_chord_feed(struct sdk_chord *ch, bool sel, bool prev,
                   uint32_t now_ms, uint32_t hold_ms)
{
    int r = 0;

    switch (ch->state)
    {
        case SDK_CH_IDLE:
            if (!(sel && prev))
                return 0;
            ch->state = SDK_CH_HELD;
            ch->t0 = now_ms;
            r = SDK_CHR_ACTIVE | SDK_CHR_BEGIN;
            break;
        case SDK_CH_HELD:
            r = SDK_CHR_ACTIVE;
            if (!(sel && prev))
            {
                /* let go early: nothing happens, swallow to both up */
                ch->state = (sel || prev) ? SDK_CH_SWALLOW : SDK_CH_IDLE;
                return r;
            }
            if (now_ms - ch->t0 >= hold_ms)
            {
                ch->state = SDK_CH_SWALLOW;     /* fires once */
                r |= SDK_CHR_EXIT;
            }
            break;
        default:                                /* SDK_CH_SWALLOW */
            r = SDK_CHR_ACTIVE;
            if (!sel && !prev)
                ch->state = SDK_CH_IDLE;
            break;
    }
    return r;
}

int sdk_chord_due(const struct sdk_chord *ch, uint32_t now_ms,
                  uint32_t hold_ms)
{
    uint32_t d;

    if (ch->state != SDK_CH_HELD)
        return -1;
    d = now_ms - ch->t0;
    return d >= hold_ms ? 0 : (int)(hold_ms - d);
}

/* -- dirty bands ----------------------------------------------------------- */
static void band_join(struct sdk_bands *b, int i, int j)
{
    int y0 = b->y[i] < b->y[j] ? b->y[i] : b->y[j];
    int e0 = b->y[i] + b->h[i], e1 = b->y[j] + b->h[j];
    int k;

    b->y[i] = (int16_t)y0;
    b->h[i] = (int16_t)((e0 > e1 ? e0 : e1) - y0);
    for (k = j; k + 1 < b->n; k++)
    {
        b->y[k] = b->y[k + 1];
        b->h[k] = b->h[k + 1];
    }
    b->n--;
}

void sdk_bands_add(struct sdk_bands *b, int y, int h, int top, int bottom)
{
    int i, j, bi = -1, bj = -1, gap = 0x7fff;

    if (y < top)
    {
        h -= top - y;
        y = top;
    }
    if (y + h > bottom)
        h = bottom - y;
    if (h <= 0)
        return;
    if (b->n < SDK_BANDS_MAX)
    {
        b->y[b->n] = (int16_t)y;
        b->h[b->n] = (int16_t)h;
        b->n++;
    }
    else
    {
        /* every slot taken: the new mark joins its nearest band */
        for (i = 0; i < b->n; i++)
        {
            int d = y > b->y[i] + b->h[i] ? y - (b->y[i] + b->h[i])
                  : b->y[i] > y + h ? b->y[i] - (y + h) : 0;
            if (d < gap)
            {
                gap = d;
                bi = i;
            }
        }
        if (y < b->y[bi])
        {
            b->h[bi] = (int16_t)(b->y[bi] + b->h[bi] - y);
            b->y[bi] = (int16_t)y;
        }
        if (y + h > b->y[bi] + b->h[bi])
            b->h[bi] = (int16_t)(y + h - b->y[bi]);
    }
    /* join overlapping or touching bands until none are left */
    for (;;)
    {
        bi = bj = -1;
        for (i = 0; i < b->n && bi < 0; i++)
            for (j = i + 1; j < b->n; j++)
                if (b->y[i] <= b->y[j] + b->h[j]
                    && b->y[j] <= b->y[i] + b->h[i])
                {
                    bi = i;
                    bj = j;
                    break;
                }
        if (bi < 0)
            break;
        band_join(b, bi, bj);
    }
}

int sdk_bands_rows(const struct sdk_bands *b)
{
    int i, n = 0;

    for (i = 0; i < b->n; i++)
        n += b->h[i];
    return n;
}


/* -- slices --------------------------------------------------------------- */
int sdk_slice_judge(uint32_t used_us, uint32_t budget_us, uint8_t *viols)
{
    if (used_us <= budget_us)
        return SDK_J_OK;
    if (used_us > budget_us * 10u || *viols >= 1)
    {
        if (*viols < 0xff)
            (*viols)++;
        return SDK_J_KILL;
    }
    (*viols)++;
    return SDK_J_VIOL;
}

/* -- rate limits ---------------------------------------------------------- */
bool sdk_rate_take(struct sdk_rate *r, uint32_t now_ms, uint32_t window_ms,
                   uint8_t per)
{
    if (now_ms - r->t0 >= window_ms)
    {
        r->t0 = now_ms;
        r->n = 0;
    }
    if (r->n >= per)
        return false;
    r->n++;
    return true;
}

/* -- the absolute wheel, polled -------------------------------------------- */
int sdk_ww_poll(struct sdk_wwatch *w, bool lit, int pos, uint32_t touches)
{
    int r = 0;
    bool t = pos >= 0;

    if (lit)
    {
        if (w->dark)
            r |= SDK_WW_WAKE | SDK_WW_LIT;
        if (t || w->touched || touches != w->touches)
            r |= SDK_WW_WAKE;
    }
    w->dark = !lit;
    w->touched = t && lit;
    w->touches = touches;
    return r;
}

int sdk_wheel_norm(int raw)
{
    return raw >= 0 && raw < SDK_WHEEL_POSITIONS ? raw : -1;
}
