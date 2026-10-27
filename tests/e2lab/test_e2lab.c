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
 * Host test of the second DMA engine lab's pure logic
 * (firmware/target/arm/ipod/video/bcm-e2lab.h): the patterns can't pass
 * "nothing happened", the readback's classes, the guards, Apple's command
 * words (dma.md section 6.3: 0xCC00_FFFC for a full piece), piece
 * splitting and the target bounds.
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
#include <stdio.h>
#include <string.h>
#include "bcm-e2lab.h"

static int fails;

#define CHECK(c) do { if (!(c)) { fails++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#define N E2LAB_MAX_WORDS
static uint32_t buf[N];

static void tally(struct e2_tally *t, const uint32_t *w, uint32_t n,
                  uint32_t seed)
{
    uint32_t i;

    e2_tally_init(t);
    for (i = 0; i < n; i++)
        e2_tally_add(t, w[i], seed, i, n);
}

static void patterns(void)
{
    static const uint32_t seeds[] = { 1, 0x5EE, 0xFFF, 0x800 };
    uint32_t k, i;

    for (k = 0; k < sizeof(seeds) / sizeof(seeds[0]); k++)
    {
        uint32_t s = seeds[k];

        for (i = 0; i < N; i++)
        {
            uint32_t S = e2_pat(E2_PAT_S, s, i), T = e2_pat(E2_PAT_T, s, i);
            uint32_t G = e2_pat(E2_PAT_G, s, i & 31);

            /* no word of S equals T or G at the same index, in either half */
            CHECK((S >> 16) != (T >> 16) && (S & 0xFFFF) != (T & 0xFFFF));
            CHECK(S != G);
            /* S names its index; a neighbour never looks right */
            if (i)
                CHECK(S != e2_pat(E2_PAT_S, s, i - 1));
            if (i + 1 < N)
                CHECK(S != e2_pat(E2_PAT_S, s, i + 1));
        }
        /* another run's S never reads as this run's */
        CHECK(e2_pat(E2_PAT_S, s, 7) != e2_pat(E2_PAT_S, s ^ 1, 7));
    }
    for (i = 0; i < 100000; i += 7)
        CHECK(e2_seed(i * 2654435761u) != 0 && e2_seed(i) <= 0xFFF);
    CHECK(e2_seed(0) != 0);
}

static void classes(void)
{
    struct e2_tally t;
    uint32_t seed = 0x123, n = 1024, i;

    /* nothing happened: every word untouched, never a pass */
    e2_fill(buf, n, E2_PAT_T, seed, 0);
    tally(&t, buf, n, seed);
    CHECK(!e2_tally_pass(&t) && t.untouched == n && t.first_bad == 0);
    CHECK(!e2_tally_partial(&t) && !e2_tally_fixed_dst(&t));

    /* all landed */
    e2_fill(buf, n, E2_PAT_S, seed, 0);
    tally(&t, buf, n, seed);
    CHECK(e2_tally_pass(&t) && t.first_bad == -1 && t.last_ok == (int)n - 1);

    /* but not S of another run (S8: data already there) */
    e2_fill(buf, n, E2_PAT_S, seed + 1, 0);
    tally(&t, buf, n, seed);
    CHECK(!e2_tally_pass(&t) && t.ok == 0 && t.other == n);

    /* S32/S33: the first word missing, the rest shifted one place down */
    e2_fill(buf, n, E2_PAT_T, seed, 0);
    for (i = 0; i + 1 < n; i++)
        buf[i] = e2_pat(E2_PAT_S, seed, i + 1);
    tally(&t, buf, n, seed);
    CHECK(!e2_tally_pass(&t) && e2_tally_partial(&t));
    CHECK(t.shifted == n - 1 && t.first_d == -1 && t.untouched == 1);

    /* word 0 of the block untouched, the rest in place */
    e2_fill(buf, n, E2_PAT_S, seed, 0);
    buf[0] = e2_pat(E2_PAT_T, seed, 0);
    tally(&t, buf, n, seed);
    CHECK(e2_tally_partial(&t) && t.first_bad == 0 && t.ok == n - 1);

    /* a fixed destination: every write to word 0, the last one stays */
    e2_fill(buf, n, E2_PAT_T, seed, 0);
    buf[0] = e2_pat(E2_PAT_S, seed, n - 1);
    tally(&t, buf, n, seed);
    CHECK(e2_tally_fixed_dst(&t) && t.w0_j == (int)n - 1);
    CHECK(!e2_tally_pass(&t));

    /* half a word landed (a 16-bit port eating the high halves) */
    e2_fill(buf, n, E2_PAT_T, seed, 0);
    for (i = 0; i < n; i++)
        buf[i] = (buf[i] & 0xFFFF0000u) |
                 (e2_pat(E2_PAT_S, seed, i) & 0xFFFF);
    tally(&t, buf, n, seed);
    CHECK(t.other == n && t.first_other_i == 0 && !e2_tally_pass(&t));

    /* guards */
    {
        uint32_t gb[E2LAB_GUARD_WORDS], ga[E2LAB_GUARD_WORDS];

        e2_fill(gb, E2LAB_GUARD_WORDS, E2_PAT_G, seed, 0);
        e2_fill(ga, E2LAB_GUARD_WORDS, E2_PAT_G, seed, E2LAB_GUARD_WORDS);
        CHECK(e2_guard_hits(gb, ga, seed) == 0);
        ga[15] ^= 1;
        gb[0] = e2_pat(E2_PAT_S, seed, 0);
        CHECK(e2_guard_hits(gb, ga, seed) == 2);
        /* a guard word read in the target counts as guard-like */
        e2_fill(buf, n, E2_PAT_S, seed, 0);
        buf[5] = e2_pat(E2_PAT_G, seed, 3);
        tally(&t, buf, n, seed);
        CHECK(t.guardlike == 1 && t.first_bad == 5);
    }
}

static void commands(void)
{
    uint32_t v;

    /* init clears 31, 30, 28, 25, 24, [23:16]; sets 26; keeps the rest */
    v = e2_cmd_init(0xFFFFFFFFu);
    CHECK(v == (0xFFFFFFFFu & ~(0x80000000u | 0x40000000u | 0x10000000u |
                                0x02000000u | 0x01000000u | 0x00FF0000u)));
    CHECK(e2_cmd_init(0) == 0x04000000u);
    /* Apple's full piece: 0xCC00_FFFC */
    v = e2_cmd_piece(e2_cmd_init(0), 65536, true) | E2_CMD_INTR |
        E2_CMD_START;
    CHECK(v == 0xCC00FFFCu);
    CHECK(e2_cmd_piece(e2_cmd_init(0), 256, true) == 0x0C0000FCu);
    CHECK(e2_cmd_piece(0x0C00FFFCu, 4096, false) == 0x04000FFCu);
    CHECK(e2_cfg_code(0xFFFFFFFFu, 2) == 0xFAFFFFFFu);
    CHECK(e2_cfg_code(0, 4) == 0x04000000u);
}

static void pieces(void)
{
    uint32_t done, n, k;

    CHECK(e2_bulk(255) == 0 && e2_bulk(1000) == 768 && e2_bulk(65536) == 65536);
    /* 153,600 bytes (a frame): 64 + 64 + 22 KB, nothing lost */
    for (done = 0, k = 0; (n = e2_piece_len(153600, done)); done += n, k++)
        CHECK(n <= E2LAB_PIECE_MAX && !(n & 255));
    CHECK(done == 153600 && k == 3);
    CHECK(e2_piece_len(256, 256) == 0 && e2_piece_len(256, 512) == 0);

    /* targets: the window only, the whole length, never 0x7xxx_xxxx */
    CHECK(e2_target_ok(E2LAB_WIN_HELD, 65536, E2LAB_WIN_HELD,
                       E2LAB_WIN_HELD + E2LAB_WIN_SIZE));
    CHECK(!e2_target_ok(E2LAB_WIN_HELD + 4, 65536, E2LAB_WIN_HELD,
                        E2LAB_WIN_HELD + E2LAB_WIN_SIZE));
    CHECK(!e2_target_ok(E2LAB_WIN_HELD - 4, 8, E2LAB_WIN_HELD,
                        E2LAB_WIN_HELD + E2LAB_WIN_SIZE));
    CHECK(!e2_target_ok(E2LAB_WIN_HELD, 65540, E2LAB_WIN_HELD, 0xFFFFFFF0u));
    CHECK(!e2_target_ok(E2LAB_WIN_HELD + 2, 4, E2LAB_WIN_HELD,
                        E2LAB_WIN_HELD + E2LAB_WIN_SIZE));
    CHECK(!e2_target_ok(E2LAB_WIN_HELD, 6, E2LAB_WIN_HELD,
                        E2LAB_WIN_HELD + E2LAB_WIN_SIZE));
    CHECK(!e2_target_ok(0x70000000u, 4, 0, 0xFFFFFFF0u));
    CHECK(!e2_target_ok(0x6FFFFFFCu, 8, 0, 0xFFFFFFF0u));
    CHECK(e2_target_ok(E2LAB_WIN_SDRAM, 256, E2LAB_WIN_SDRAM,
                       E2LAB_WIN_SDRAM + E2LAB_WIN_SIZE));
    /* the windows themselves, with their guards, stay inside SRAM */
    CHECK(E2LAB_WIN_IDLE - 64 >= 0x12A800 &&
          E2LAB_WIN_IDLE + E2LAB_WIN_SIZE + 64 <= 0x140000);
}

int main(void)
{
    patterns();
    classes();
    commands();
    pieces();
    if (fails)
        printf("%d failed\n", fails);
    return fails != 0;
}
