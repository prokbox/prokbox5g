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

/*
 * Host test of #144 P0: the UAC1 source descriptors (uac1_source.h)
 * against blobs written out by hand from the UAC 1.0 specification, the
 * packet sizer, and the ARC isochronous dQH/dTD rules (usb-arc-iso.h).
 */
#include <stdio.h>
#include <string.h>
#include "uac1_source.h"
#include "usb-arc-iso.h"

static int fails, checks;

#define CHECK(c) do { checks++; if (!(c)) { fails++; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* The 5G's shape: interfaces 0/1, EP1 IN, 192 bytes, full speed, nine
 * rates 8 to 48 kHz.  Each line is one descriptor. */
static const uint8_t want_fs9[] = {
    0x09, 0x04, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00,   /* AC if */
    0x09, 0x24, 0x01, 0x00, 0x01, 0x1e, 0x00, 0x01, 0x01,   /* header */
    0x0c, 0x24, 0x02, 0x01, 0x01, 0x02, 0x00, 0x02,         /* IT mic */
    0x03, 0x00, 0x00, 0x00,
    0x09, 0x24, 0x03, 0x02, 0x01, 0x01, 0x00, 0x01, 0x00,   /* OT USB */
    0x09, 0x04, 0x01, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,   /* AS alt 0 */
    0x09, 0x04, 0x01, 0x01, 0x01, 0x01, 0x02, 0x00, 0x00,   /* AS alt 1 */
    0x07, 0x24, 0x01, 0x02, 0x01, 0x01, 0x00,               /* AS general */
    0x23, 0x24, 0x02, 0x01, 0x02, 0x02, 0x10, 0x09,         /* Type I */
    0x40, 0x1f, 0x00,   0x11, 0x2b, 0x00,   0xe0, 0x2e, 0x00,
    0x80, 0x3e, 0x00,   0x22, 0x56, 0x00,   0xc0, 0x5d, 0x00,
    0x00, 0x7d, 0x00,   0x44, 0xac, 0x00,   0x80, 0xbb, 0x00,
    0x09, 0x05, 0x81, 0x01, 0xc0, 0x00, 0x01, 0x00, 0x00,   /* EP1 IN iso */
    0x07, 0x25, 0x01, 0x01, 0x00, 0x00, 0x00,               /* CS EP */
};

/* the bench probe's: 48 kHz only, high speed, interfaces 2/3, EP2 IN */
static const uint8_t want_hs1[] = {
    0x09, 0x04, 0x02, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00,
    0x09, 0x24, 0x01, 0x00, 0x01, 0x1e, 0x00, 0x01, 0x03,
    0x0c, 0x24, 0x02, 0x01, 0x01, 0x02, 0x00, 0x02, 0x03, 0x00, 0x00, 0x00,
    0x09, 0x24, 0x03, 0x02, 0x01, 0x01, 0x00, 0x01, 0x00,
    0x09, 0x04, 0x03, 0x00, 0x00, 0x01, 0x02, 0x00, 0x00,
    0x09, 0x04, 0x03, 0x01, 0x01, 0x01, 0x02, 0x00, 0x00,
    0x07, 0x24, 0x01, 0x02, 0x01, 0x01, 0x00,
    0x0b, 0x24, 0x02, 0x01, 0x02, 0x02, 0x10, 0x01, 0x80, 0xbb, 0x00,
    0x09, 0x05, 0x82, 0x01, 0xc0, 0x00, 0x04, 0x00, 0x00,
    0x07, 0x25, 0x01, 0x01, 0x00, 0x00, 0x00,
};

static void dump_diff(const uint8_t *got, const uint8_t *want, int n)
{
    int i;

    for (i = 0; i < n; i++)
        if (got[i] != want[i])
        {
            printf("  first difference at byte %d: got %02x want %02x\n",
                   i, got[i], want[i]);
            return;
        }
}

static void t_descriptors(void)
{
    static const uint32_t apple_rates[9] = {
        8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000 };
    static const uint8_t ep5g[9] = { 9, 5, 0x81, 1, 0xc0, 0, 1, 0, 0 };
    struct uac1_src_cfg c;
    uint8_t buf[256];
    int n, i, pos, total;

    memset(&c, 0, sizeof(c));
    c.if_ac = 0;
    c.ep_in = 0x81;
    c.high_speed = false;
    c.mps = UAC1_SRC_MPS;
    c.nrates = 9;
    memcpy(c.rates, apple_rates, sizeof(apple_rates));
    memset(buf, 0xee, sizeof(buf));
    n = uac1_src_build(buf, &c);
    CHECK(n == (int)sizeof(want_fs9));
    CHECK(n == UAC1_SRC_LEN(9));
    CHECK(n > 0 && memcmp(buf, want_fs9, sizeof(want_fs9)) == 0);
    if (n > 0 && memcmp(buf, want_fs9, sizeof(want_fs9)))
        dump_diff(buf, want_fs9, sizeof(want_fs9));
    CHECK(n > 0 && buf[n] == 0xee);             /* no overrun */

    /* the endpoint descriptor is the one read from the 5G's firmware */
    CHECK(n > 16 && memcmp(buf + n - 16, ep5g, 9) == 0);
    /* every descriptor's bLength walks exactly to the end, and the AC
     * header's wTotalLength covers header + IT + OT */
    for (pos = 0, i = 0; pos < n && buf[pos]; i++)
        pos += buf[pos];
    CHECK(pos == n && i == 10);
    total = buf[9 + 5] | buf[9 + 6] << 8;
    CHECK(total == 9 + 12 + 9);

    memset(&c, 0, sizeof(c));
    c.if_ac = 2;
    c.ep_in = 0x82;
    c.high_speed = true;
    c.mps = UAC1_SRC_MPS;
    c.nrates = 1;
    c.rates[0] = 48000;
    n = uac1_src_build(buf, &c);
    CHECK(n == (int)sizeof(want_hs1) && n == UAC1_SRC_LEN(1));
    CHECK(n > 0 && memcmp(buf, want_hs1, sizeof(want_hs1)) == 0);
    if (n > 0 && memcmp(buf, want_hs1, sizeof(want_hs1)))
        dump_diff(buf, want_hs1, sizeof(want_hs1));
    CHECK(uac1_src_rate_ok(&c, 48000) && !uac1_src_rate_ok(&c, 44100));

    c.nrates = 0;
    CHECK(uac1_src_build(buf, &c) == -1);
    c.nrates = 10;
    CHECK(uac1_src_build(buf, &c) == -1);
}

static void t_pacer(void)
{
    static const uint32_t r[] = { 8000, 11025, 12000, 16000, 22050,
                                  24000, 32000, 44100, 48000 };
    struct uac1_pacer p;
    unsigned n, sum = 0, n44 = 0, n45 = 0, other = 0, max_run = 0, run = 0;
    unsigned k;
    int i;

    uac1_pacer_init(&p, 44100);
    for (i = 0; i < 1000; i++)
    {
        n = uac1_pacer_next(&p);
        sum += n;
        if (n == 44)
        {
            n44++;
            if (++run > max_run)
                max_run = run;
        }
        else if (n == 45)
        {
            n45++;
            run = 0;
        }
        else
            other++;
        CHECK(n * UAC1_FRAME_BYTES <= UAC1_SRC_MPS);
    }
    CHECK(sum == 44100);
    CHECK(n44 == 900 && n45 == 100 && other == 0);
    CHECK(max_run == 9);                    /* 44 nine times, then 45 */
    CHECK(p.acc == 0);
    /* an hour stays exact: no drift */
    for (i = 0; i < 3600 * 1000 - 1000; i++)
        sum += uac1_pacer_next(&p);
    CHECK(sum == 44100u * 3600);

    uac1_pacer_init(&p, 48000);
    for (i = 0, sum = 0, other = 0; i < 1000; i++)
    {
        n = uac1_pacer_next(&p);
        sum += n;
        other += n != 48;
    }
    CHECK(sum == 48000 && other == 0);
    CHECK(48 * UAC1_FRAME_BYTES == UAC1_SRC_MPS);

    /* every rate of the list sums exactly over a second */
    for (k = 0; k < sizeof(r) / sizeof(r[0]); k++)
    {
        uac1_pacer_init(&p, r[k]);
        for (i = 0, sum = 0; i < 1000; i++)
            sum += uac1_pacer_next(&p);
        CHECK(sum == r[k]);
    }
}

static void t_arc_rules(void)
{
    uint32_t w, ctrl;

    /* max packet: 1024 high speed, 1023 full speed */
    CHECK(arc_iso_max_mps(true) == 1024 && arc_iso_max_mps(false) == 1023);
    CHECK(arc_iso_clamp_mps(1024, false) == 1023);
    CHECK(arc_iso_clamp_mps(1024, true) == 1024);
    CHECK(arc_iso_clamp_mps(192, false) == 192);

    /* dQH: isochronous IN Mult = 3, OUT the packet count, bulk 0 */
    w = arc_qh_caps(ARC_EP_ISO, true, 192, 0);
    CHECK(w >> ARC_QH_MULT_SHIFT == 3);
    CHECK((w & ARC_QH_MPS_MASK) >> ARC_QH_MPS_SHIFT == 192);
    CHECK(w & ARC_QH_ZLT_OFF);
    w = arc_qh_caps(ARC_EP_ISO, false, 192, 1);
    CHECK(w >> ARC_QH_MULT_SHIFT == 1);
    w = arc_qh_caps(ARC_EP_BULK, true, 512, 0);
    CHECK(w >> ARC_QH_MULT_SHIFT == 0 &&
          (w & ARC_QH_MPS_MASK) >> ARC_QH_MPS_SHIFT == 512);
    w = arc_qh_caps(ARC_EP_ISO, true, 1024, 0);
    CHECK((w & ARC_QH_MPS_MASK) >> ARC_QH_MPS_SHIFT == 1024);

    /* dTD MultO: the real packet count of each request */
    CHECK(arc_iso_td_multo(192, 192) == 1);
    CHECK(arc_iso_td_multo(176, 192) == 1);
    CHECK(arc_iso_td_multo(0, 192) == 1);          /* a zero-length packet */
    CHECK(arc_iso_td_multo(193, 192) == 2);
    CHECK(arc_iso_td_multo(3 * 1024, 1024) == 3);
    CHECK(arc_iso_td_multo(3 * 1024 + 1, 1024) == 0);
    CHECK(arc_iso_td_multo(-1, 192) == 0);
    w = arc_iso_td_token(192, 192);
    CHECK((w & ARC_TD_MULTO_MASK) >> ARC_TD_MULTO_SHIFT == 1);
    CHECK(w >> 16 == 192 && (w & 0x8000u) && (w & 0x80u));
    CHECK(arc_iso_td_token(4000, 1024) == 0);

    /* length <= mult x maxpacket */
    CHECK(arc_iso_len_ok(192, 1, 192));
    CHECK(!arc_iso_len_ok(193, 1, 192));
    CHECK(arc_iso_len_ok(3072, 3, 1024));
    CHECK(!arc_iso_len_ok(3073, 3, 1024));
    CHECK(!arc_iso_len_ok(100, 0, 192) && !arc_iso_len_ok(100, 4, 192));

    /* TR_ERR on isochronous is a missed packet, not fatal */
    CHECK(arc_iso_td_fatal(ARC_ISO_TD_TXN_ERR, true) == 0);
    CHECK(arc_iso_td_missed(ARC_ISO_TD_TXN_ERR, true));
    CHECK(arc_iso_td_fatal(ARC_ISO_TD_TXN_ERR, false) == ARC_ISO_TD_TXN_ERR);
    CHECK(!arc_iso_td_missed(ARC_ISO_TD_TXN_ERR, false));
    CHECK(arc_iso_td_fatal(ARC_ISO_TD_TXN_ERR | ARC_ISO_TD_BUF_ERR, true) ==
          ARC_ISO_TD_BUF_ERR);
    CHECK(arc_iso_td_fatal(ARC_ISO_TD_HALTED, true) == ARC_ISO_TD_HALTED);
    CHECK(arc_iso_td_fatal(0x80u /* active */, true) == 0);

    /* deinit: the disabled direction becomes bulk, never control; the
     * other direction is untouched */
    ctrl = ARC_EPC_TX_ENABLE | ARC_EP_ISO << ARC_EPC_TX_TYPE_SHIFT |
           ARC_EPC_RX_ENABLE | ARC_EP_ISO << ARC_EPC_RX_TYPE_SHIFT;
    w = arc_epctrl_deinit(ctrl, false);
    CHECK(arc_epctrl_type(w, false) == ARC_EP_BULK);
    CHECK(!(w & ARC_EPC_RX_ENABLE));
    CHECK(arc_epctrl_type(w, true) == ARC_EP_ISO && (w & ARC_EPC_TX_ENABLE));
    w = arc_epctrl_deinit(ctrl, true);
    CHECK(arc_epctrl_type(w, true) == ARC_EP_BULK && !(w & ARC_EPC_TX_ENABLE));
    CHECK(arc_epctrl_type(w, false) == ARC_EP_ISO && (w & ARC_EPC_RX_ENABLE));
    /* from a control-typed register too (the reset value) */
    w = arc_epctrl_deinit(0, true);
    CHECK(arc_epctrl_type(w, true) != ARC_EP_CONTROL);

    /* FRINDEX: 8 microframes per millisecond, across the wrap */
    CHECK(arc_frindex_delta(16, 8) == 8);
    CHECK(arc_frindex_delta(4, 0x3ffc) == 8);
}

int main(void)
{
    t_descriptors();
    t_pacer();
    t_arc_rules();
    printf("usbaudio: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
