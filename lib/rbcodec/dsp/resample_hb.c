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

/* C reference for the half-band stages in resample_hb.h (P19 19-13). The
 * ARM build uses resample_hb_arm.S instead; tests/resample checks the two
 * agree bit for bit. */
#ifndef RESAMPLE_HB_STANDALONE
#include "rbcodecconfig.h"
#include "platform.h"
#endif
#include "resample_hb.h"

#if defined(RESAMPLE_HB_STANDALONE) || !defined(CPU_ARM) || \
    defined(CPU_ARM_MICRO)

#ifdef RESAMPLE_HB_STANDALONE
#define HB_UP2   hb_up2_c
#define HB_DOWN2 hb_down2_c
#else
#define HB_UP2   hb_up2
#define HB_DOWN2 hb_down2
#endif

/* One chain of k sections on one sample; s = the chain's k+1 states */
static inline int32_t hb_chain(int32_t x, int32_t *s, const int32_t *a,
                               int k)
{
    for (int i = 0; i < k; i++)
    {
        int32_t d = (int32_t)((uint32_t)x - (uint32_t)s[i + 1]);
        int32_t hi = (int32_t)(((int64_t)a[i] * d) >> 32);
        int32_t y = (int32_t)((uint32_t)s[i] + ((uint32_t)hi << 1));
        s[i] = x;
        x = y;
    }
    s[k] = x;
    return x;
}

void HB_UP2(int32_t *dst, const int32_t *src, int n, int32_t *st,
            const struct hb_coefs *c)
{
    for (int i = 0; i < n; i++)
    {
        int32_t x = src[i];
        dst[2*i]     = hb_chain(x, st, c->c, c->k0);
        dst[2*i + 1] = hb_chain(x, st + HB_MAX_SECT + 1,
                                c->c + HB_MAX_SECT, c->k1);
    }
}

void HB_DOWN2(int32_t *dst, const int32_t *src, int n, int32_t *st,
              const struct hb_coefs *c)
{
    for (int i = 0; i < n; i++)
    {
        /* the newer sample of the pair feeds chain 0 */
        int32_t y0 = hb_chain(src[2*i + 1], st, c->c, c->k0);
        int32_t y1 = hb_chain(src[2*i], st + HB_MAX_SECT + 1,
                              c->c + HB_MAX_SECT, c->k1);
        dst[i] = (int32_t)((uint32_t)y0 + (uint32_t)y1) >> 1;
    }
}

#endif /* C reference */
