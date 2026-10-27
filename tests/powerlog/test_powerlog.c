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
 * Host test of the developer power log's ring and CSV formatting
 * (apps/ipodui/powerlog_core.c, tracker #10): row layout, the header,
 * push/pop order, overflow with the dropped count, mark tags (sanitised,
 * cut, reused slots), and the longest line.
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
#include "ipodui/powerlog_core.h"

static int fails;

#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
    fails++; } } while (0)
#define CHECK_STR(a, b) do { if (strcmp((a), (b))) { \
    fprintf(stderr, "%s:%d: got \"%s\"\n   expected \"%s\"\n", \
            __FILE__, __LINE__, (a), (b)); fails++; } } while (0)

static struct pl_ring ring;

static struct pl_row row_at(uint32_t secs)
{
    struct pl_row w;
    memset(&w, 0, sizeof(w));
    w.secs = secs;
    w.mv = 3700;
    w.pct = 50;
    /* Fit-1 M0: as sample() leaves them (no measurement) */
    w.idle_pct = w.cop_idle_pct = PL_UNKNOWN;
    w.cpu1_slot = w.cpu1_pct = w.cpu2_slot = w.cpu2_pct = PL_UNKNOWN;
    w.disk_by = PL_UNKNOWN;
    return w;
}

/* Pops one row as powerlog.c's flush does: copy, format, advance */
static int pop(char *buf, size_t size)
{
    struct pl_row w;

    if (pl_ring_pending(&ring) == 0)
        return 0;
    w = ring.rows[ring.tail % PL_ROWS];
    ring.tail++;
    return pl_format_row(buf, size, &w, pl_ring_tag(&ring, w.mark), NULL);
}

static void test_layout(void)
{
    int commas = 0;
    const char *p;

    CHECK(sizeof(struct pl_row) == 36);
    CHECK(sizeof(struct pl_ring) <= 7200);
    for (p = pl_csv_header; *p; p++)
        commas += *p == ',';
    CHECK(commas == 29);                    /* 27 columns */
    /* the M0 columns come after mark: old column numbers keep working */
    CHECK(strstr(pl_csv_header, ",dropped,mark,idle_pct,") != NULL);
    CHECK(pl_csv_header[strlen(pl_csv_header) - 1] == '\n');
    CHECK(!strncmp(pl_csv_header, "s,mv,pct,", 9));
}

static void test_format(void)
{
    char buf[PL_LINE_MAX];
    struct pl_row w = row_at(86400);
    int n;

    w.mv = 3712;
    w.pct = 54;
    w.flags = PL_F_CHARGER | PL_F_PLAYING | PL_F_DAC_ALLOW;
    w.boost_pct = 12;
    w.bl_pct = 0;
    w.lcd_pct = 3;
    w.disk_pct = 1;
    w.dac_sleeps = 7;
    w.dac_wakes = 6;
    w.vol = -30;
    n = pl_format_row(buf, sizeof(buf), &w, "", NULL);
    CHECK_STR(buf, "86400,3712,54,1,0,1,12,0,3,1,1,0,7,6,-30,0,,-1,-1,-,-1,-,-1,0,0,0,-,0,0,0\n");
    CHECK(n == (int)strlen(buf));

    /* paused wins over playing; unknown level is -1 */
    w = row_at(5);
    w.pct = 255;
    w.flags = PL_F_PLAYING | PL_F_PAUSED | PL_F_DAC_SB | PL_F_CHARGING;
    pl_format_row(buf, sizeof(buf), &w, "ab", NULL);
    CHECK_STR(buf, "5,3700,-1,0,1,2,0,0,0,0,0,1,0,0,0,0,ab,-1,-1,-,-1,-,-1,0,0,0,-,0,0,0\n");

    /* the M0 fields, with names (a comma in a name becomes '_') */
    {
        static const char *const names[3] = { "codec", "audio,x", NULL };
        w = row_at(7);
        w.idle_pct = 93;
        w.cop_idle_pct = 100;
        w.cpu1_slot = 3;
        w.cpu1_pct = 5;
        w.cpu2_slot = 4;
        w.cpu2_pct = 1;
        w.disk_cmds = 12;
        w.disk_kb = 1536;
        w.disk_wakes = 2;
        w.disk_by = 6;
        pl_format_row(buf, sizeof(buf), &w, "", names);
        CHECK_STR(buf, "7,3700,50,0,0,0,0,0,0,0,0,0,0,0,0,0,"
                       ",93,100,codec,5,audio_x,1,12,1536,2,-,0,0,0\n");
    }

    /* the longest line fits PL_LINE_MAX */
    w.secs = 4294967295u;
    w.mv = 65535;
    w.pct = 100;
    w.boost_pct = w.bl_pct = w.lcd_pct = w.disk_pct = 100;
    w.dac_sleeps = w.dac_wakes = w.dropped = 65535;
    w.vol = -128;
    w.idle_pct = w.cop_idle_pct = w.cpu1_pct = w.cpu2_pct = 100;
    w.cpu1_slot = w.cpu2_slot = w.disk_by = 1;
    w.disk_cmds = w.disk_kb = 65535;
    w.disk_wakes = 255;
    w.stor_on_pct = 100;
    w.stor_ons = 255;
    w.stor_ons_total = 65535;
    {
        static const char *const names[3] = { "123456789012345678",
            "123456789012345678", "123456789012345678" };
        n = pl_format_row(buf, sizeof(buf), &w, "123456789012345678901",
                          names);
    }
    CHECK(n < PL_LINE_MAX && buf[n - 1] == '\n');
    CHECK(strstr(buf, ",123456789012345678901,") != NULL);
    CHECK(strstr(buf, ",123456789012345,100,255,65535\n") != NULL);

    /* a short buffer cuts but keeps the newline */
    n = pl_format_row(buf, 12, &w, "", NULL);
    CHECK(n == 11 && buf[10] == '\n' && buf[11] == '\0');
}

static void test_ring(void)
{
    char buf[PL_LINE_MAX];
    struct pl_row w;
    uint32_t i;

    pl_ring_reset(&ring);
    CHECK(pl_ring_pending(&ring) == 0);
    CHECK(pop(buf, sizeof(buf)) == 0);
    for (i = 0; i < 3; i++)
    {
        w = row_at(i * 30);
        pl_ring_push(&ring, &w);
    }
    CHECK(pl_ring_pending(&ring) == 3);
    pop(buf, sizeof(buf));
    CHECK(!strncmp(buf, "0,", 2));
    pop(buf, sizeof(buf));
    CHECK(!strncmp(buf, "30,", 3));
    pop(buf, sizeof(buf));
    CHECK(!strncmp(buf, "60,", 3));
    CHECK(pl_ring_pending(&ring) == 0);

    /* overflow: PL_ROWS + 5 rows; the 5 oldest are lost and counted */
    pl_ring_reset(&ring);
    for (i = 0; i < PL_ROWS + 5; i++)
    {
        w = row_at(i);
        pl_ring_push(&ring, &w);
    }
    CHECK(pl_ring_pending(&ring) == PL_ROWS);
    CHECK(ring.dropped == 5);
    pop(buf, sizeof(buf));
    /* the oldest kept row is secs 5, pushed before any drop */
    CHECK(!strncmp(buf, "5,", 2));
    CHECK(strstr(buf, ",0,,-1,-1,-,-1,-,-1,0,0,0,-,0,0,0\n") != NULL);
    while (pl_ring_pending(&ring) > 1)
        pop(buf, sizeof(buf));
    pop(buf, sizeof(buf));
    {
        char want[16];
        snprintf(want, sizeof(want), "%d,", PL_ROWS + 4);
        CHECK(!strncmp(buf, want, strlen(want)));
    }
    CHECK(strstr(buf, ",5,,-1,-1,-,-1,-,-1,0,0,0,-,0,0,0\n") != NULL);

    /* the counters keep going past 2^32 rows (head - tail stays right) */
    pl_ring_reset(&ring);
    ring.head = ring.tail = 0xfffffffeu;
    for (i = 0; i < 4; i++)
    {
        w = row_at(i);
        pl_ring_push(&ring, &w);
    }
    CHECK(pl_ring_pending(&ring) == 4);
    pop(buf, sizeof(buf));
    CHECK(!strncmp(buf, "0,", 2));
}

static void test_marks(void)
{
    char buf[PL_LINE_MAX];
    struct pl_row w;
    uint8_t ids[PL_MARKS + 2];
    int i;

    pl_ring_reset(&ring);
    w = row_at(10);
    w.mark = pl_ring_mark(&ring, "phase-a, \"lcd\"\n");
    CHECK(w.mark == 1);
    pl_ring_push(&ring, &w);
    w = row_at(11);
    pl_ring_push(&ring, &w);
    pop(buf, sizeof(buf));
    CHECK_STR(buf, "10,3700,50,0,0,0,0,0,0,0,0,0,0,0,0,0,phase-a_ _lcd__,-1,-1,-,-1,-,-1,0,0,0,-,0,0,0\n");
    pop(buf, sizeof(buf));
    CHECK_STR(buf, "11,3700,50,0,0,0,0,0,0,0,0,0,0,0,0,0,,-1,-1,-,-1,-,-1,0,0,0,-,0,0,0\n");

    /* cut to 21 characters */
    pl_ring_mark(&ring, "0123456789abcdefghijKLMNOP");
    CHECK_STR(pl_ring_tag(&ring, 2), "0123456789abcdefghijK");

    /* a reused slot reads "?" */
    pl_ring_reset(&ring);
    for (i = 0; i < PL_MARKS + 2; i++)
    {
        char t[8];
        snprintf(t, sizeof(t), "m%d", i);
        ids[i] = pl_ring_mark(&ring, t);
    }
    CHECK_STR(pl_ring_tag(&ring, ids[0]), "?");
    CHECK_STR(pl_ring_tag(&ring, ids[1]), "?");
    CHECK_STR(pl_ring_tag(&ring, ids[2]), "m2");
    CHECK_STR(pl_ring_tag(&ring, ids[PL_MARKS + 1]), "m9");
    CHECK_STR(pl_ring_tag(&ring, 0), "");

    /* ids wrap 255 -> 1, never 0 */
    ring.mark_no = 254;
    CHECK(pl_ring_mark(&ring, "x") == 255);
    CHECK(pl_ring_mark(&ring, "y") == 1);
    CHECK_STR(pl_ring_tag(&ring, 1), "y");
}

static void test_acc(void)
{
    CHECK(pl_acc_pct(0, 0) == 0);
    CHECK(pl_acc_pct(1, 120) == 1);         /* 0.83 rounds up */
    CHECK(pl_acc_pct(120, 120) == 100);
    CHECK(pl_acc_pct(1, 20) == 5);
}

int main(void)
{
    test_layout();
    test_format();
    test_ring();
    test_marks();
    test_acc();
    if (fails)
        fprintf(stderr, "%d check(s) failed\n", fails);
    return fails ? 1 : 0;
}
