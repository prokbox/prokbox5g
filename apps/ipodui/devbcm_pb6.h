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
 * PB-6's pure logic (docs/bcm2722-reference/brief-map-peripherals.md,
 * "PB-6"; bench-p19.md, card PB-6): the colour-bar BMP that Apple's NOR
 * vmcs is asked to show on the TV, the command words and their completion
 * rule, the timeouts, and what the owner's answer means.  The device half
 * is devbcm_pb6() in devbcm_probes.c and bcm_devtest_tv_pattern() in the
 * devtest HAL; tests/pb6probe/ checks this on the host.
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
#ifndef _IPODUI_DEVBCM_PB6_H_
#define _IPODUI_DEVBCM_PB6_H_

#include <stdbool.h>
#include <stdint.h>

/* ---- Timeouts ---------------------------------------------------------
 * The spec's bound on each command's completion, and the hold the owner
 * watches the TV for (SELECT ends it early). */
#define PB6_CMD_US      2000000
#define PB6_HOLD_MS     10000

/* ---- Apple's NOR vmcs command words ------------------------------------
 * lcd-video.c's BCM_CMD encoding (the index in the low half, its
 * complement in the high half), mirrored here because the firmware's
 * names live in a .c file: TV_PALBMP 2, TV_NTSCBMP 3, TV_MVOFF 14.  The
 * command goes to host 0x1F8; the BMP to host 0xC020_0000 (BCMA_TV_BMPDATA). */
#define PB6_CMD(x)      ((~(uint32_t)(x) << 16) | (uint32_t)(x))
#define PB6_IDX_PALBMP  2
#define PB6_IDX_NTSCBMP 3
#define PB6_IDX_MVOFF   14
#define PB6_BMP_HOST    0xC0200000u

static inline uint32_t pb6_bmp_cmd(bool pal)
{
    return PB6_CMD(pal ? PB6_IDX_PALBMP : PB6_IDX_NTSCBMP);
}

/* The completion rule of lcd-video.c's bcm_command(): the word at 0x1F8
 * reads back as something other than the command and the busy 0xFFFF */
static inline bool pb6_cmd_done(uint32_t cmd, uint32_t status)
{
    return status != cmd && status != 0xFFFF;
}

/* The log's cmd= field: "ack" only when every port access went through
 * and the word changed within the bound; a dead port, or the word still
 * the command at the bound, is "timeout" */
enum pb6_cmd
{
    PB6_ACK = 0,
    PB6_TIMEOUT,
};

static inline enum pb6_cmd pb6_cmd_verdict(bool port_ok, uint32_t cmd,
                                           uint32_t status, uint32_t us)
{
    return port_ok && us <= PB6_CMD_US && pb6_cmd_done(cmd, status)
         ? PB6_ACK : PB6_TIMEOUT;
}

static inline const char *pb6_cmd_name(enum pb6_cmd c)
{
    return c == PB6_ACK ? "ack" : "timeout";
}

/* ---- The BMP -----------------------------------------------------------
 * The format Apple's vmcs expects is unknown (the command names say BMP;
 * nothing says which kind), so this is the most ordinary one, the form a
 * minimal decoder is most likely to accept:
 *   - a 14-byte BITMAPFILEHEADER ("BM", the file size, 0, 0, bfOffBits
 *     54) and a 40-byte BITMAPINFOHEADER (the classic Windows 3 header,
 *     not the V4/V5 ones: biSize 40, 320 x 240, one plane, 24 bits,
 *     BI_RGB = 0, biSizeImage 230400, 2835 px/m both ways (72 dpi), no
 *     palette); the pixels start right after, at byte 54;
 *   - bottom-up (biHeight positive, the default; a top-down BMP needs a
 *     negative height that older decoders reject), so the first row in
 *     the file is the bottom of the picture;
 *   - B, G, R byte order per pixel (BMP's own); a row is 960 bytes, a
 *     multiple of 4, so no row padding to get wrong.
 * The picture: eight vertical bars 40 px wide at 75 % (white, yellow,
 * cyan, green, magenta, red, blue, black), the composite-safe levels of
 * the usual test pattern, identical on every row, so even an upside-down
 * or mirrored reading still shows bars (mirrored: in reverse order).
 * The buffer holds the file padded to a multiple of 4 bytes (the port
 * moves 32-bit words); the pad bytes are 0 and not counted in the file
 * size. */
#define PB6_W           320
#define PB6_H           240
#define PB6_BPP         24
#define PB6_HDR         54
#define PB6_ROW         (PB6_W * 3)
#define PB6_PIXELS      (PB6_ROW * PB6_H)
#define PB6_FILE        (PB6_HDR + PB6_PIXELS)         /* 230454 */
#define PB6_BUF         ((PB6_FILE + 3) & ~3)          /* 230456 */
#define PB6_BARS        8
#define PB6_LEVEL       191                             /* 75 % of 255 */

/* The bar under column x as 0xRRGGBB */
static inline uint32_t pb6_bar_rgb(int x)
{
    /* bit 2 red, bit 1 green, bit 0 blue, white first, black last */
    static const uint8_t mask[PB6_BARS] = { 7, 6, 3, 2, 5, 4, 1, 0 };
    int b = x / (PB6_W / PB6_BARS);
    uint32_t m = mask[b < 0 ? 0 : b >= PB6_BARS ? PB6_BARS - 1 : b];

    return (m & 4 ? (uint32_t)PB6_LEVEL << 16 : 0) |
           (m & 2 ? (uint32_t)PB6_LEVEL << 8 : 0) |
           (m & 1 ? (uint32_t)PB6_LEVEL : 0);
}

static inline void pb6_le16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static inline void pb6_le32(uint8_t *p, uint32_t v)
{
    pb6_le16(p, v);
    pb6_le16(p + 2, v >> 16);
}

/* Fill buf (cap bytes) with the BMP; its padded length back, 0 when cap
 * is too small */
static inline uint32_t pb6_bmp_build(uint8_t *buf, uint32_t cap)
{
    uint8_t *row = buf + PB6_HDR;
    uint32_t i;
    int x, y;

    if (!buf || cap < PB6_BUF)
        return 0;
    for (i = 0; i < PB6_HDR; i++)
        buf[i] = 0;
    buf[0] = 'B';
    buf[1] = 'M';
    pb6_le32(buf + 2, PB6_FILE);            /* bfSize */
    pb6_le32(buf + 10, PB6_HDR);            /* bfOffBits */
    pb6_le32(buf + 14, 40);                 /* biSize */
    pb6_le32(buf + 18, PB6_W);              /* biWidth */
    pb6_le32(buf + 22, PB6_H);              /* biHeight > 0: bottom-up */
    pb6_le16(buf + 26, 1);                  /* biPlanes */
    pb6_le16(buf + 28, PB6_BPP);            /* biBitCount */
    pb6_le32(buf + 30, 0);                  /* biCompression: BI_RGB */
    pb6_le32(buf + 34, PB6_PIXELS);         /* biSizeImage */
    pb6_le32(buf + 38, 2835);               /* biXPelsPerMeter */
    pb6_le32(buf + 42, 2835);               /* biYPelsPerMeter */
    /* biClrUsed, biClrImportant: 0 */
    for (x = 0; x < PB6_W; x++)
    {
        uint32_t rgb = pb6_bar_rgb(x);

        row[3 * x] = (uint8_t)rgb;              /* B */
        row[3 * x + 1] = (uint8_t)(rgb >> 8);   /* G */
        row[3 * x + 2] = (uint8_t)(rgb >> 16);  /* R */
    }
    for (y = 1; y < PB6_H; y++)
        for (i = 0; i < PB6_ROW; i++)
            row[y * PB6_ROW + i] = row[i];
    for (i = PB6_FILE; i < PB6_BUF; i++)
        buf[i] = 0;
    return PB6_BUF;
}

/* The first two little-endian words read back from 0xC020_0000 match
 * the header written: "BM" and the size's low half, then its high half
 * and the reserved word's low half */
static inline bool pb6_head_ok(uint32_t w0, uint32_t w1)
{
    return w0 == ((uint32_t)'B' | (uint32_t)'M' << 8 |
                  (PB6_FILE & 0xFFFFu) << 16) &&
           w1 == (uint32_t)(PB6_FILE >> 16);
}

/* ---- The owner's answer and what it means ------------------------------ */
enum pb6_saw
{
    PB6_SAW_BARS = 0,   /* colour bars */
    PB6_SAW_GREY,       /* a steady grey, black or blank picture */
    PB6_SAW_NOTHING,    /* the TV says no signal */
    PB6_SAW_NOISE,      /* snow, rolling or torn picture */
    PB6_SAWS,
};

static inline const char *pb6_saw_name(int saw)
{
    static const char *const s[PB6_SAWS] = { "bars", "grey", "nothing",
                                             "noise" };
    return saw >= 0 && saw < PB6_SAWS ? s[saw] : "none";
}

enum pb6_verdict
{
    PB6_V_VIDEO = 0,    /* bars: the jack carries video; PB-8 worth running */
    PB6_V_SIGNAL,       /* grey or noise: a signal reaches the TV, the BMP
                           guess (or the standard) is wrong */
    PB6_V_UNCLEAR,      /* nothing: the jack, the BMP or the command */
    PB6_V_NOCMD,        /* the command never completed: Apple's vmcs
                           didn't take it; nothing learnt about the jack */
    PB6_V_NONE,         /* no answer from the owner */
};

static inline enum pb6_verdict pb6_verdict(enum pb6_cmd cmd, int saw)
{
    if (saw == PB6_SAW_BARS)
        return PB6_V_VIDEO;     /* a picture is proof, whatever the word */
    if (saw == PB6_SAW_GREY || saw == PB6_SAW_NOISE)
        return PB6_V_SIGNAL;
    if (cmd != PB6_ACK)
        return PB6_V_NOCMD;
    if (saw == PB6_SAW_NOTHING)
        return PB6_V_UNCLEAR;
    return PB6_V_NONE;
}

static inline const char *pb6_verdict_name(enum pb6_verdict v)
{
    static const char *const s[] = {
        "jack carries video; PB-8 worth running",
        "signal reaches the TV; BMP or standard wrong; try the other "
            "standard, then PB-8",
        "unclear (jack, BMP or standard); try the other standard, then PB-8",
        "command not taken; nothing learnt about the jack",
        "no answer",
    };
    return (unsigned)v < sizeof(s) / sizeof(s[0]) ? s[v] : "?";
}

#endif /* _IPODUI_DEVBCM_PB6_H_ */
