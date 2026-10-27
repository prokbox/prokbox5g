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
 * Host test of PB-6's pure logic (apps/ipodui/devbcm_pb6.h): the
 * BMP file and info headers, the bars, the padding, the command words
 * against lcd-video.c's encoding, the completion rule and the verdicts.
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
#include <stdlib.h>
#include <string.h>
#include "devbcm_pb6.h"

/* lcd-video.c's encoding, copied as written there */
#define BCM_CMD(x) ((~((unsigned long)x) << 16) | ((unsigned long)x))

static int fails;

#define CHECK(c) do { if (!(c)) { fails++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint32_t rd16(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8;
}

static uint32_t rd32(const uint8_t *p)
{
    return rd16(p) | rd16(p + 2) << 16;
}

static void t_bmp(void)
{
    uint8_t *b = malloc(PB6_BUF + 16);
    uint32_t n, x, y;
    /* white, yellow, cyan, green, magenta, red, blue, black at 75 % */
    static const uint8_t want[8][3] = {     /* R, G, B */
        { 191, 191, 191 }, { 191, 191, 0 }, { 0, 191, 191 }, { 0, 191, 0 },
        { 191, 0, 191 }, { 191, 0, 0 }, { 0, 0, 191 }, { 0, 0, 0 },
    };

    CHECK(PB6_FILE == 230454 && PB6_BUF == 230456 && PB6_BUF % 4 == 0);
    CHECK(PB6_ROW % 4 == 0);                /* no row padding */
    CHECK(pb6_bmp_build(NULL, PB6_BUF) == 0);
    CHECK(pb6_bmp_build(b, PB6_BUF - 1) == 0);
    memset(b, 0xA5, PB6_BUF + 16);
    n = pb6_bmp_build(b, PB6_BUF + 16);
    CHECK(n == PB6_BUF);
    CHECK(b[PB6_BUF] == 0xA5);              /* nothing past the buffer */

    /* BITMAPFILEHEADER */
    CHECK(b[0] == 'B' && b[1] == 'M');
    CHECK(rd32(b + 2) == PB6_FILE);
    CHECK(rd32(b + 6) == 0);
    CHECK(rd32(b + 10) == 54);
    /* BITMAPINFOHEADER: 40 bytes, bottom-up, 24 bpp, BI_RGB */
    CHECK(rd32(b + 14) == 40);
    CHECK(rd32(b + 18) == 320);
    CHECK((int32_t)rd32(b + 22) == 240);    /* positive: bottom-up */
    CHECK(rd16(b + 26) == 1 && rd16(b + 28) == 24);
    CHECK(rd32(b + 30) == 0);
    CHECK(rd32(b + 34) == 320 * 240 * 3);
    CHECK(rd32(b + 38) == 2835 && rd32(b + 42) == 2835);
    CHECK(rd32(b + 46) == 0 && rd32(b + 50) == 0);
    CHECK(14 + 40 == PB6_HDR);

    /* every row the same eight bars, B G R per pixel */
    for (y = 0; y < PB6_H; y++)
        for (x = 0; x < PB6_W; x++)
        {
            const uint8_t *p = b + PB6_HDR + y * PB6_ROW + 3 * x;
            const uint8_t *w = want[x / 40];

            if (p[2] != w[0] || p[1] != w[1] || p[0] != w[2])
            {
                fails++;
                printf("FAIL pixel %u,%u\n", (unsigned)x, (unsigned)y);
                y = PB6_H;
                break;
            }
        }
    /* the pad bytes are zero and outside the file size */
    CHECK(b[PB6_FILE] == 0 && b[PB6_FILE + 1] == 0);

    /* the read-back check against the header as written */
    CHECK(pb6_head_ok(rd32(b), rd32(b + 4)));
    CHECK(!pb6_head_ok(rd32(b) ^ 1, rd32(b + 4)));
    CHECK(!pb6_head_ok(rd32(b), rd32(b + 4) + 1));
    CHECK(!pb6_head_ok(0xFFFFFFFF, 0xFFFFFFFF));    /* a dead port */
    free(b);
}

static void t_cmd(void)
{
    uint32_t k;

    /* the same words as lcd-video.c's BCMCMD_* */
    for (k = 0; k < 16; k++)
        CHECK(PB6_CMD(k) == (uint32_t)BCM_CMD(k));
    CHECK(PB6_CMD(PB6_IDX_PALBMP) == 0xFFFD0002u);
    CHECK(PB6_CMD(PB6_IDX_NTSCBMP) == 0xFFFC0003u);
    CHECK(PB6_CMD(PB6_IDX_MVOFF) == 0xFFF1000Eu);
    CHECK(pb6_bmp_cmd(true) == PB6_CMD(2));
    CHECK(pb6_bmp_cmd(false) == PB6_CMD(3));
    CHECK(PB6_BMP_HOST == 0xC0200000u);

    /* completion: neither the command nor the busy word */
    CHECK(!pb6_cmd_done(PB6_CMD(3), PB6_CMD(3)));
    CHECK(!pb6_cmd_done(PB6_CMD(3), 0xFFFF));
    CHECK(pb6_cmd_done(PB6_CMD(3), 0));
    CHECK(pb6_cmd_done(PB6_CMD(3), 3));

    CHECK(pb6_cmd_verdict(true, PB6_CMD(3), 0, 1500) == PB6_ACK);
    CHECK(pb6_cmd_verdict(true, PB6_CMD(3), 0, PB6_CMD_US) == PB6_ACK);
    CHECK(pb6_cmd_verdict(true, PB6_CMD(3), 0, PB6_CMD_US + 1) ==
          PB6_TIMEOUT);
    CHECK(pb6_cmd_verdict(true, PB6_CMD(3), PB6_CMD(3), 2000100) ==
          PB6_TIMEOUT);
    CHECK(pb6_cmd_verdict(false, PB6_CMD(3), 0, 100) == PB6_TIMEOUT);
    CHECK(!strcmp(pb6_cmd_name(PB6_ACK), "ack"));
    CHECK(!strcmp(pb6_cmd_name(PB6_TIMEOUT), "timeout"));

    /* the spec's bounds */
    CHECK(PB6_CMD_US == 2000000 && PB6_HOLD_MS == 10000);
}

static void t_verdict(void)
{
    CHECK(!strcmp(pb6_saw_name(PB6_SAW_BARS), "bars"));
    CHECK(!strcmp(pb6_saw_name(PB6_SAW_GREY), "grey"));
    CHECK(!strcmp(pb6_saw_name(PB6_SAW_NOTHING), "nothing"));
    CHECK(!strcmp(pb6_saw_name(PB6_SAW_NOISE), "noise"));
    CHECK(!strcmp(pb6_saw_name(-1), "none"));
    CHECK(!strcmp(pb6_saw_name(PB6_SAWS), "none"));

    CHECK(pb6_verdict(PB6_ACK, PB6_SAW_BARS) == PB6_V_VIDEO);
    CHECK(pb6_verdict(PB6_TIMEOUT, PB6_SAW_BARS) == PB6_V_VIDEO);
    CHECK(pb6_verdict(PB6_ACK, PB6_SAW_GREY) == PB6_V_SIGNAL);
    CHECK(pb6_verdict(PB6_ACK, PB6_SAW_NOISE) == PB6_V_SIGNAL);
    CHECK(pb6_verdict(PB6_TIMEOUT, PB6_SAW_GREY) == PB6_V_SIGNAL);
    CHECK(pb6_verdict(PB6_ACK, PB6_SAW_NOTHING) == PB6_V_UNCLEAR);
    CHECK(pb6_verdict(PB6_TIMEOUT, PB6_SAW_NOTHING) == PB6_V_NOCMD);
    CHECK(pb6_verdict(PB6_TIMEOUT, -1) == PB6_V_NOCMD);
    CHECK(pb6_verdict(PB6_ACK, -1) == PB6_V_NONE);
    CHECK(strstr(pb6_verdict_name(PB6_V_VIDEO), "PB-8") != NULL);
    CHECK(!strcmp(pb6_verdict_name((enum pb6_verdict)99), "?"));
}

int main(void)
{
    t_bmp();
    t_cmd();
    t_verdict();
    if (fails)
        printf("%d failure(s)\n", fails);
    return fails != 0;
}
