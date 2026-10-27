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
 * bcmfw panel tables: power-on and power-off for the four LCD panels
 * the iPod Video ships with (panel ID = GPIO51 + 2 * GPIO50).
 *
 * The register numbers, values, delays and their order are hardware
 * facts recorded in docs/research/bcm2722/hardware.md ("Power
 * sequences"); the record format and the code are ours.  Each panel
 * has an init part that both directions run first, then its own on or
 * off part.  The power-on prologue (interface, reset pulse, bus sync)
 * and the final reset-low are code in lcd.c.
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
#include "bcmfw.h"

#define R PS_REG

/* ---- Panel 0: MIPI DCS controller, one byte per parameter ----------- */

static const uint16_t p0_init[] =
{
    PS_LUT,                             /* 0x2D: 5/6/5-bit colour table */
    PS_DCS(0x36, 1), 0x00,              /* memory access control */
    PS_DCS(0x26, 1), 0x01,              /* gamma curve 1 */
    PS_DCS(0x3A, 1), 0x05,              /* 16 bits per pixel */
    PS_DCS(0x30, 4), 0x00, 0x00, 0x00, 0xEF,    /* partial area 0-239 */
    PS_CMD(0x11), PS_WAIT(5),           /* sleep out */
    PS_DCS(0x2A, 4), 0x00, 0x00, 0x01, 0x3F,    /* columns 0-319 */
    PS_DCS(0x2B, 4), 0x00, 0x00, 0x00, 0xEF,    /* rows 0-239 */
    PS_END
};

static const uint16_t p0_on[] =
{
    PS_TIMING(0),
    PS_CMD(0x29),                       /* display on */
    PS_DCS(0x35, 1), 0x00,              /* tearing-effect line on */
    PS_END
};

static const uint16_t p0_off[] =
{
    PS_CMD(0x34),                       /* tearing-effect line off */
    PS_CMD(0x28),                       /* display off */
    PS_CMD(0x10), PS_WAIT(50),          /* sleep in */
    PS_END
};

/* ---- Panels 1 and 3: 16-bit index/value, 0x2xx register map ---------- */

static const uint16_t p1_init_m1[] =
{
    R(0x001, 0x0100), R(0x002, 0x2500), R(0x003, 0x02B0), R(0x004, 0x0000),
    R(0x008, 0x0404), R(0x009, 0x0000), R(0x00B, 0x0000), R(0x00D, 0x0010),
    R(0x010, 0x0013), R(0x011, 0x2600), R(0x012, 0x2200), R(0x013, 0x0802),
    R(0x014, 0x080C), R(0x015, 0x0816), R(0x016, 0x2200), R(0x031, 0x0000),
    R(0x080, 0x0031),
    PS_END
};

static const uint16_t p1_init_m2[] =
{
    R(0x001, 0x0110), R(0x002, 0x1500), R(0x003, 0x02B0), R(0x004, 0x0000),
    R(0x008, 0x0404), R(0x009, 0x0000), R(0x00B, 0x0000), R(0x00D, 0x0010),
    R(0x010, 0x0013), R(0x011, 0x2600), R(0x012, 0x2200), R(0x013, 0x0802),
    R(0x014, 0x080C), R(0x015, 0x0816), R(0x016, 0x2200), R(0x031, 0x0000),
    R(0x080, 0x0031),
    PS_END
};

/* Gamma block shared by panel 1's variants: registers base + 0..13 */
#define P1_GAMMA(b) \
    R((b) + 0x0, 0x0705), R((b) + 0x1, 0x0006), R((b) + 0x2, 0x0302), \
    R((b) + 0x3, 0x0303), R((b) + 0x4, 0x0300), R((b) + 0x5, 0x0103), \
    R((b) + 0x6, 0x001F), R((b) + 0x7, 0x0705), R((b) + 0x8, 0x0006), \
    R((b) + 0x9, 0x0302), R((b) + 0xA, 0x0303), R((b) + 0xB, 0x0300), \
    R((b) + 0xC, 0x0203), R((b) + 0xD, 0x001F)

/* Display-on tail shared by panel 1's variants (r103 differs) */
#define P1_DISPLAY_ON \
    R(0x007, 0x0021), R(0x102, 0x02BD), PS_WAIT(120), \
    R(0x200, 0x0000), R(0x201, 0x0000), R(0x007, 0x0031), \
    R(0x030, 0x001F), PS_WAIT(17), R(0x030, 0x03FF), R(0x007, 0x0133)

static const uint16_t p1_on_m1[] =
{
    P1_GAMMA(0x300),
    PS_TIMING(1),
    R(0x400, 0x001D), R(0x401, 0x0001), R(0x007, 0x0001), R(0x205, 0x0060),
    R(0x110, 0x0001), R(0x100, 0x15B0), R(0x101, 0x0020), R(0x102, 0x029D),
    R(0x103, 0x0E5B), R(0x105, 0x0103), R(0x106, 0x0103),
    P1_DISPLAY_ON,
    PS_END
};

static const uint16_t p1_on_m2[] =
{
    P1_GAMMA(0x300), P1_GAMMA(0x310),
    R(0x320, 0x0707), R(0x321, 0x0006), R(0x322, 0x0300), R(0x323, 0x0303),
    R(0x324, 0x0303), R(0x325, 0x0107), R(0x326, 0x001F), R(0x327, 0x0707),
    R(0x328, 0x0006), R(0x329, 0x0300), R(0x32A, 0x0303), R(0x32B, 0x0303),
    R(0x32C, 0x0107), R(0x32D, 0x001F),
    PS_TIMING(1),
    R(0x400, 0x001D), R(0x401, 0x0001), R(0x205, 0x0060), R(0x007, 0x0001),
    R(0x110, 0x0001), R(0x100, 0x15B0), R(0x101, 0x0020), R(0x102, 0x029D),
    R(0x103, 0x1158), R(0x105, 0x0103), R(0x106, 0x0103),
    P1_DISPLAY_ON,
    PS_END
};

static const uint16_t p1_off[] =
{
    R(0x007, 0x0032), PS_WAIT(51),
    R(0x007, 0x0022), R(0x030, 0x0040), PS_WAIT(17),
    R(0x007, 0x0020), R(0x030, 0x0000),
    R(0x100, 0x0580), R(0x101, 0x0060), R(0x102, 0x020D), R(0x007, 0x0000),
    PS_WAIT(17),
    R(0x100, 0x0700), R(0x100, 0x15B4),
    PS_END
};

static const uint16_t p3_init[] =
{
    R(0x008, 0x0808), R(0x010, 0x0013), R(0x011, 0x0300), R(0x012, 0x0101),
    R(0x013, 0x0A03), R(0x014, 0x0A0E), R(0x015, 0x0A19), R(0x016, 0x2402),
    R(0x018, 0x0001), R(0x090, 0x0021), R(0x00D, 0x0010), R(0x003, 0x0230),
    PS_END
};

/* Gamma: 14 values for registers base + 0..13 */
#define GAMMA14(b, v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, va, vb, vc, vd) \
    R((b) + 0x0, v0), R((b) + 0x1, v1), R((b) + 0x2, v2), R((b) + 0x3, v3), \
    R((b) + 0x4, v4), R((b) + 0x5, v5), R((b) + 0x6, v6), R((b) + 0x7, v7), \
    R((b) + 0x8, v8), R((b) + 0x9, v9), R((b) + 0xA, va), R((b) + 0xB, vb), \
    R((b) + 0xC, vc), R((b) + 0xD, vd)

#define P3_POWER(r103, r106, r102) \
    PS_TIMING(1), \
    R(0x400, 0x001D), R(0x401, 0x0001), R(0x205, 0x0060), R(0x007, 0x0001), \
    R(0x031, 0x0071), R(0x110, 0x0001), R(0x100, 0x17B0), R(0x101, 0x0220), \
    R(0x102, 0x009B), R(0x103, r103), R(0x105, 0x0103), R(0x106, r106), \
    R(0x007, 0x0021), R(0x102, r102), R(0x001, 0x0110), R(0x002, 0x0500), \
    R(0x007, 0x0031), R(0x030, 0x0007), PS_WAIT(17), \
    R(0x030, 0x03FF), PS_WAIT(34), R(0x007, 0x0072), PS_WAIT(85), \
    R(0x007, 0x0173)

static const uint16_t p3_on_m1[] =
{
    GAMMA14(0x300, 0x0707, 0x0207, 0x0700, 0x0303, 0x0301, 0x0707, 0x1F07,
            0x0606, 0x0507, 0x0701, 0x0303, 0x0302, 0x0706, 0x1F19),
    GAMMA14(0x310, 0x0707, 0x0207, 0x0700, 0x0303, 0x0301, 0x0707, 0x1F07,
            0x0606, 0x0507, 0x0701, 0x0303, 0x0302, 0x0706, 0x1F19),
    GAMMA14(0x320, 0x0607, 0x0206, 0x0700, 0x0302, 0x0302, 0x0707, 0x1A13,
            0x0606, 0x0507, 0x0702, 0x0303, 0x0303, 0x0707, 0x1F1F),
    P3_POWER(0x1000, 0x0104, 0x00BB),
    PS_END
};

static const uint16_t p3_on_m2[] =
{
    GAMMA14(0x300, 0x0707, 0x0002, 0x0603, 0x0303, 0x0100, 0x0307, 0x1719,
            0x0707, 0x0002, 0x0603, 0x0303, 0x0100, 0x0307, 0x1719),
    GAMMA14(0x310, 0x0707, 0x0204, 0x0605, 0x0303, 0x0200, 0x0307, 0x1719,
            0x0707, 0x0204, 0x0605, 0x0303, 0x0200, 0x0307, 0x1719),
    GAMMA14(0x320, 0x0607, 0x0001, 0x0701, 0x0303, 0x0300, 0x0707, 0x1F19,
            0x0607, 0x0001, 0x0701, 0x0303, 0x0300, 0x0707, 0x1F19),
    P3_POWER(0x1200, 0x0105, 0x00BD),
    PS_END
};

static const uint16_t p3_off[] =
{
    R(0x007, 0x0172), R(0x030, 0x03FF), PS_WAIT(51),
    R(0x007, 0x0120), R(0x030, 0x0000), R(0x100, 0x0780), R(0x007, 0x0000),
    R(0x101, 0x0260), R(0x102, 0x00A9), PS_WAIT(17),
    R(0x100, 0x0700), R(0x100, 0x0704),
    PS_END
};

/* ---- Panel 2: 16-bit index/value, 0x2x/0x4x register map ------------- */

static const uint16_t p2_init[] =
{
    R(0x01, 0x011D), R(0x02, 0x0300), R(0x03, 0x1030), R(0x07, 0x0004),
    R(0x08, 0x0202), R(0x0C, 0x0000),
    PS_END
};

static const uint16_t p2_on[] =
{
    R(0x30, 0x0000), R(0x31, 0x0000), R(0x32, 0x0000), R(0x33, 0x0000),
    R(0x34, 0x0000), R(0x35, 0x0000), R(0x36, 0x0707), R(0x37, 0x0000),
    R(0x38, 0x1807), R(0x39, 0x1E08),
    PS_TIMING(1),
    R(0x07, 0x0004), R(0x13, 0x0000), PS_WAIT(10),
    R(0x11, 0x2202), R(0x0B, 0xE283), R(0x10, 0x2D50), R(0x13, 0x0040),
    PS_WAIT(50),
    R(0x13, 0x0060), PS_WAIT(50),
    R(0x14, 0xA404), R(0x10, 0x3250), R(0x13, 0x0070), R(0x71, 0x0317),
    PS_WAIT(150),
    R(0x44, 0x013F), R(0x45, 0x0000), R(0x46, 0xEF00), R(0x09, 0x0009),
    R(0x07, 0x0016), PS_WAIT(50),
    R(0x07, 0x0017), R(0x20, 0x0000), R(0x21, 0x0000),
    PS_END
};

static const uint16_t p2_off[] =
{
    R(0x0B, 0x6C04), R(0x07, 0x0016), PS_WAIT(50),
    R(0x09, 0x0001), R(0x07, 0x0004), R(0x10, 0x0250), R(0x13, 0x0060),
    PS_WAIT(50),
    R(0x13, 0x0040), PS_WAIT(50),
    R(0x13, 0x0000), PS_WAIT(200),
    R(0x10, 0x0000),
    PS_END
};

/* ---- Bus timing ---------------------------------------------------------
 * A timing word holds four cycle counts, each one less than the cycles
 * needed to cover a duration in ns: bits 29:24 (a, from n0), 21:16 (c,
 * from n2), 14:8 (d, from n3 less what rounding a and c up already
 * added) and 6:0 (b, from n1); bit 30 = 16-bit bus.  The formula is
 * Apple's (@c87c), the nanoseconds are each panel's table (hardware.md,
 * "Bus timing"); the arithmetic below is ours.  With
 * BCMFW_LCD_BUS_KHZ = 150000 it gives Apple's words exactly (hello_test
 * checks it for every panel). */
#define TW_POS(x)       ((x) > 0 ? (x) : 0)
#define TW_CYC(k, ns)   TW_POS(((ns) * (k) + 999999) / 1000000 - 1)
/* ns a rounded-up field already spends beyond its own n */
#define TW_OVER(k, ns)  ((TW_CYC(k, ns) + 1) * 1000000 / (k) - (ns))
#define TWK(k, n0, n1, n2, n3) (0x40000000 \
    | (TW_CYC(k, n0) & 63) << 24 | (TW_CYC(k, n2) & 63) << 16 \
    | (TW_CYC(k, TW_POS((n3) - TW_OVER(k, n2) - TW_OVER(k, n0))) & 127) << 8 \
    | (TW_CYC(k, n1) & 127))
#if BCMFW_LCD_BUS_KHZ
#define TWA(n0, n1, n2, n3, apple) TWK(BCMFW_LCD_BUS_KHZ, n0, n1, n2, n3)
#else
#define TWA(n0, n1, n2, n3, apple) (apple)
#endif

/* {ours, ours, ours}, {Apple's}, then the sweep's three words per clock:
 * one descriptor's timing, from its three nanosecond quadruples */
#define SWEEP1(k, p0, p1, p2, p3, q0, q1, q2, q3, r0, r1, r2, r3) \
    { TWK(k, p0, p1, p2, p3), TWK(k, q0, q1, q2, q3), TWK(k, r0, r1, r2, r3) }
#define TIMING(w0, w1a, w1b, a0, a1a, a1b) \
    TWA(w0, a0), { TWA(w1a, a1a), TWA(w1b, a1b) }, a0, { a1a, a1b }, \
    { SWEEP1(BCMFW_SWEEP_KHZ_0, w0, w1a, w1b), \
      SWEEP1(BCMFW_SWEEP_KHZ_1, w0, w1a, w1b), \
      SWEEP1(BCMFW_SWEEP_KHZ_2, w0, w1a, w1b) }

/* panel 0: 10/45/90/25, then 10/35/10/55 */
#define P0_TIMING TIMING(NS(10, 45, 90, 25), NS(10, 35, 10, 55), NS(10, 35, 10, 55), \
                         0x410D0206, 0x41010705, 0x41010705)
/* panels 1 and 3: 10/170/5/285, then 0/50/5/145 and 0/65/5/30 */
#define P13_TIMING TIMING(NS(10, 170, 5, 285), NS(0, 50, 5, 145), NS(0, 65, 5, 30), \
                          0x41002A19, 0x40001407, 0x40000309)
/* panel 2: 10/250/10/240, then 10/40/10/50 */
#define P2_TIMING TIMING(NS(10, 250, 10, 240), NS(10, 40, 10, 50), NS(10, 40, 10, 50), \
                         0x41012325, 0x41010605, 0x41010605)
#define NS(n0, n1, n2, n3) n0, n1, n2, n3

/* ---- Descriptors ------------------------------------------------------- */

static const struct panel panels[4][2] =
{
    {   /* 0 */
        { p0_init, p0_on, p0_off, P0_TIMING,
          110, WIN_DCS },
        { p0_init, p0_on, p0_off, P0_TIMING,
          110, WIN_DCS },
    },
    {   /* 1 */
        { p1_init_m1, p1_on_m1, p1_off, P13_TIMING,
          110, WIN_R2XX },
        { p1_init_m2, p1_on_m2, p1_off, P13_TIMING,
          110, WIN_R2XX },
    },
    {   /* 2 */
        { p2_init, p2_on, p2_off, P2_TIMING,
          120, WIN_R4X },
        { p2_init, p2_on, p2_off, P2_TIMING,
          120, WIN_R4X },
    },
    {   /* 3 */
        { p3_init, p3_on_m1, p3_off, P13_TIMING,
          110, WIN_R2XX },
        { p3_init, p3_on_m2, p3_off, P13_TIMING,
          110, WIN_R2XX },
    },
};

const struct panel *panel_get(int id, int model)
{
    return &panels[id & 3][model == 2];
}

#if BCMFW_LCD_STANDBY
/* ---- Light standby (stage 5s): display off, power circuits left on ----
 *
 * Our own cut of each panel's scripts above [hypothesis]: the off script
 * up to the display-off writes, without the power-circuit registers
 * (0x100-0x102 on panel 3; sleep-in 0x10 on panel 0); the wake is the
 * display-on ramp at the end of the on script, with the same waits.
 * Panels 1 and 2 have none (deep standby only). */

static const uint16_t p0_light_off[] =
{
    PS_CMD(0x34),                       /* tearing-effect line off */
    PS_CMD(0x28),                       /* display off */
    PS_END
};

static const uint16_t p0_light_on[] =
{
    PS_CMD(0x29),                       /* display on */
    PS_DCS(0x35, 1), 0x00,              /* tearing-effect line on */
    PS_END
};

static const uint16_t p3_light_off[] =
{
    R(0x007, 0x0172), R(0x030, 0x03FF), PS_WAIT(51),
    R(0x007, 0x0120), R(0x030, 0x0000), R(0x007, 0x0000),
    PS_END
};

static const uint16_t p3_light_on[] =
{
    R(0x007, 0x0021), R(0x007, 0x0031), R(0x030, 0x0007), PS_WAIT(17),
    R(0x030, 0x03FF), PS_WAIT(34), R(0x007, 0x0072), PS_WAIT(85),
    R(0x007, 0x0173),
    PS_END
};

const uint16_t *panel_light(int id, int on)
{
    switch (id & 3)
    {
    case 0:
        return on ? p0_light_on : p0_light_off;
    case 3:
        return on ? p3_light_on : p3_light_off;
    default:
        return 0;
    }
}
#endif
