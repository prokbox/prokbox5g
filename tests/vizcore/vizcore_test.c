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

/* Host test of apps/ipodui/viz_core.c (#126). */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "viz_core.h"
#include "jviz.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
    printf("\n"); } } while (0)

/* n samples of a sine (amplitude a, f Hz at rate) in split order */
static void sine_split(int16_t *out, unsigned n, double f, double a,
                       unsigned rate)
{
    unsigned i;

    for (i = 0; i < n; i++)
    {
        int16_t v = (int16_t)lrint(a * sin(2 * M_PI * f * i / rate));
        out[(i & 1) ? n / 2 + i / 2 : i / 2] = v;
    }
}

static void bands_of(uint16_t *l2, const int16_t *pcm, unsigned n,
                     const uint16_t *e, unsigned nb)
{
    static uint32_t p[JVIZ_M_MAX];

    jviz_power_ref(p, pcm, n);
    jviz_bands(l2, p, n / 2, e, nb);
}

static void test_edges(void)
{
    static const unsigned rates[] = { 8000, 22050, 32000, 44100, 48000 };
    static const unsigned ns[] = { 256, 512 };
    uint16_t e[VIZ_BANDS_MAX + 1];
    unsigned r, k, i, nb;

    for (r = 0; r < 5; r++)
        for (k = 0; k < 2; k++)
        {
            nb = viz_edges(e, VIZ_BANDS, ns[k], rates[r]);
            CHECK(nb >= 1 && nb <= VIZ_BANDS, "nb %u", nb);
            CHECK(e[0] >= 1, "no DC bin (rate %u n %u)", rates[r], ns[k]);
            CHECK(e[nb] <= ns[k] / 2, "top %u", e[nb]);
            for (i = 0; i < nb; i++)
                CHECK(e[i] < e[i + 1], "edge %u: %u %u (rate %u n %u)", i,
                      e[i], e[i + 1], rates[r], ns[k]);
        }
    nb = viz_edges(e, VIZ_BANDS, 512, 44100);
    CHECK(nb == VIZ_BANDS, "24 bands at 512");
    /* the last edge near 16 kHz: bin 186 of 256 at 86 Hz a bin */
    CHECK(e[nb] >= 180 && e[nb] <= 192, "top edge %u", e[nb]);
    printf("     edges 512@44.1k: lo %u .. hi %u\n", e[0], e[nb]);
    nb = viz_edges(e, VIZ_BANDS, 256, 44100);
    CHECK(nb == VIZ_BANDS, "24 bands at 256 (%u)", nb);
}

/* A sine lands in its band, heights stay in the rect, silence falls, the
 * cap holds and then falls, and a quiet copy of a signal draws the same
 * picture after the gain settles (auto-gain) */
static void test_dyn(void)
{
    int16_t pcm[512];
    uint16_t e[VIZ_BANDS_MAX + 1], l2[VIZ_BANDS_MAX];
    uint8_t h[VIZ_BANDS_MAX], pk[VIZ_BANDS_MAX], h_loud[VIZ_BANDS_MAX];
    struct viz_dyn d;
    unsigned nb = viz_edges(e, VIZ_BANDS, 512, 44100), i, f, best = 0;
    unsigned held;

    viz_dyn_init(&d);
    sine_split(pcm, 512, 1000.0, 20000.0, 44100);
    bands_of(l2, pcm, 512, e, nb);
    for (f = 0; f < 30; f++)
        viz_dyn_step(&d, l2, nb, 0, 150, 33, h, pk);
    for (i = 0; i < nb; i++)
    {
        CHECK(h[i] <= 150 && pk[i] <= 150, "in the rect");
        if (h[i] > h[best])
            best = i;
    }
    CHECK(e[best] * 44100 / 512 <= 1000 + 90
          && e[best + 1] * 44100 / 512 >= 1000 - 90,
          "1 kHz in band %u (%u..%u)", best, e[best], e[best + 1]);
    CHECK(h[best] >= 140, "full height %u", h[best]);
    CHECK(pk[best] >= h[best], "cap at the top");
    /* neighbours lifted, far bands low */
    CHECK(best > 0 && h[best - 1] >= h[best] * 2 / 3 - 2, "lift");
    memcpy(h_loud, h, sizeof(h));

    /* silence: the bar falls, the cap holds 0.5 s then falls too */
    viz_dyn_step(&d, l2, nb, 1, 150, 33, h, pk);
    CHECK(h[best] < h_loud[best] && pk[best] >= h_loud[best] - 1,
          "release, cap held (%u %u)", h[best], pk[best]);
    for (f = 0, held = 0; f < 120; f++)
    {
        viz_dyn_step(&d, l2, nb, 1, 150, 33, h, pk);
        if (pk[best] >= h_loud[best] - 1)
            held++;
    }
    CHECK(held >= 12 && held <= 17, "held %u frames (0.5 s)", held);
    CHECK(h[best] == 0 && pk[best] == 0, "fallen %u %u", h[best], pk[best]);

    /* -30 dB: after the gain falls (3 dB/s: 10 s), the same picture */
    sine_split(pcm, 512, 1000.0, 20000.0 / 31.6, 44100);
    bands_of(l2, pcm, 512, e, nb);
    for (f = 0; f < 400; f++)
        viz_dyn_step(&d, l2, nb, 0, 150, 33, h, pk);
    CHECK(h[best] + 4 >= h_loud[best], "auto-gain %u vs %u", h[best],
          h_loud[best]);

    /* 15 fps steps reach the same place */
    viz_dyn_init(&d);
    for (f = 0; f < 15; f++)
        viz_dyn_step(&d, l2, nb, 0, 150, 66, h, pk);
    CHECK(h[best] > 100, "15 fps attack %u", h[best]);
}

static void test_mirror(void)
{
    unsigned b, r;

    viz_mirror_split(160, &b, &r);
    CHECK(b + r + 1 == 160 && r == 48, "split %u %u", b, r);
    CHECK(viz_fade565(0xFFFF, 0xFFFF) == 0xFFFF, "white stays");
    CHECK(viz_fade565(0x0000, 0xFFFF) == ((19 << 11) | (39 << 5) | 19),
          "black fades %04x", viz_fade565(0, 0xFFFF));
    CHECK(viz_style_shown(VIZ_STYLE_BARS) && !viz_style_shown(VIZ_STYLE_AURA)
          && !viz_style_shown(-1) && !viz_style_shown(VIZ_STYLES), "shown");
}

int main(void)
{
    test_edges();
    test_dyn();
    test_mirror();
    if (fails)
    {
        printf("FAIL: vizcore %d of %d\n", fails, checks);
        return 1;
    }
    printf("ok   vizcore (%d checks: edges, gain, attack/release, lift, "
           "peak hold, mirror)\n", checks);
    return 0;
}
