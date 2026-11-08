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
 * ARC/ChipIdea device controller: the isochronous rules for queue heads
 * (dQH), transfer descriptors (dTD) and endpoint control, as pure
 * functions so tests/usbaudio runs them on the host (#144 P0).
 *
 * Read from the core's manual and Linux's chipidea UDC [external]; none is
 * proven on the PP5022 yet (docs/research/usb-digital-audio.md §2).  Only
 * the bench probe (HAVE_USB_ISOPROBE) uses them on the device so far.
 */
#ifndef USB_ARC_ISO_H
#define USB_ARC_ISO_H

#include <stdint.h>
#include <stdbool.h>

/* endpoint types, as in ENDPTCTRL and the USB endpoint descriptor */
#define ARC_EP_CONTROL      0u
#define ARC_EP_ISO          1u
#define ARC_EP_BULK         2u
#define ARC_EP_INT          3u

/* dQH word 0 */
#define ARC_QH_MULT_SHIFT   30
#define ARC_QH_ZLT_OFF      0x20000000u     /* zero-length termination off */
#define ARC_QH_MPS_SHIFT    16
#define ARC_QH_MPS_MASK     0x07ff0000u
/* dTD word 1 */
#define ARC_TD_MULTO_SHIFT  10
#define ARC_TD_MULTO_MASK   0x00000c00u
/* the dTD status bits (also in usb-arc-reset.h) */
#define ARC_ISO_TD_HALTED   0x40u
#define ARC_ISO_TD_BUF_ERR  0x20u
#define ARC_ISO_TD_TXN_ERR  0x08u
/* ENDPTCTRL fields per direction */
#define ARC_EPC_RX_TYPE_SHIFT   2
#define ARC_EPC_TX_TYPE_SHIFT   18
#define ARC_EPC_RX_ENABLE       0x00000080u
#define ARC_EPC_TX_ENABLE       0x00800000u

/* The largest isochronous max packet: 1024 at high speed, 1023 at full
 * speed (USB 2.0 §5.6.3).  The driver's default of 1024 is wrong on a
 * full-speed link, which our unit has shown (usb.md §5). */
static inline int arc_iso_max_mps(bool high_speed)
{
    return high_speed ? 1024 : 1023;
}

static inline int arc_iso_clamp_mps(int mps, bool high_speed)
{
    int max = arc_iso_max_mps(high_speed);

    return mps > max ? max : mps;
}

/* dQH word 0 for an endpoint.  Isochronous IN: Mult = 3, the largest, and
 * each dTD's MultO gives its real packet count (Linux does this).
 * Isochronous OUT: Mult = packets per (micro)frame.  Others: Mult = 0. */
static inline uint32_t arc_qh_caps(unsigned type, bool dir_in, int mps,
                                   int out_packets)
{
    uint32_t w = (uint32_t)mps << ARC_QH_MPS_SHIFT & ARC_QH_MPS_MASK;

    w |= ARC_QH_ZLT_OFF;
    if (type == ARC_EP_ISO)
        w |= (uint32_t)(dir_in ? 3 : out_packets & 3) << ARC_QH_MULT_SHIFT;
    return w;
}

/* Packets one isochronous IN dTD of len bytes takes: ceil(len / mps), a
 * zero-length packet counting as one.  0 when it needs more than 3, the
 * most one (micro)frame carries. */
static inline unsigned arc_iso_td_multo(int len, int mps)
{
    int n;

    if (len < 0 || mps <= 0)
        return 0;
    n = len ? (len + mps - 1) / mps : 1;
    return n > 3 ? 0 : (unsigned)n;
}

/* An isochronous request is refused when longer than mult x maxpacket */
static inline bool arc_iso_len_ok(int len, int mult, int mps)
{
    return len >= 0 && mult >= 1 && mult <= 3 && len <= mult * mps;
}

/* dTD word 1 for one isochronous IN packet group: length, IOC, ACTIVE and
 * MultO.  0 when the length does not fit. */
static inline uint32_t arc_iso_td_token(int len, int mps)
{
    unsigned m = arc_iso_td_multo(len, mps);

    if (!m)
        return 0;
    return (uint32_t)len << 16 | 0x8000u /* IOC */ |
           m << ARC_TD_MULTO_SHIFT | 0x80u /* ACTIVE */;
}

/* The error bits of a retired dTD that end the stream.  On an isochronous
 * endpoint a transaction error is a missed (micro)frame: the host simply
 * got no packet that time, and the next dTD goes on.  Linux treats it so;
 * our driver used to flush the endpoint for it. */
static inline uint32_t arc_iso_td_fatal(uint32_t sts, bool iso)
{
    uint32_t err = sts & (ARC_ISO_TD_HALTED | ARC_ISO_TD_BUF_ERR |
                          ARC_ISO_TD_TXN_ERR);

    return iso ? err & ~ARC_ISO_TD_TXN_ERR : err;
}

static inline bool arc_iso_td_missed(uint32_t sts, bool iso)
{
    return iso && (sts & ARC_ISO_TD_TXN_ERR);
}

/* ENDPTCTRL after disabling one direction.  The other direction keeps
 * working, so the disabled one is left as bulk: the manual's caution
 * (32.9.5.18) is that a direction left as control makes the active
 * direction's data PID tracking undefined.  Upstream wrote type 0. */
static inline uint32_t arc_epctrl_deinit(uint32_t ctrl, bool dir_in)
{
    if (dir_in)
        return (ctrl & ~(ARC_EPC_TX_ENABLE | 3u << ARC_EPC_TX_TYPE_SHIFT)) |
               ARC_EP_BULK << ARC_EPC_TX_TYPE_SHIFT;
    return (ctrl & ~(ARC_EPC_RX_ENABLE | 3u << ARC_EPC_RX_TYPE_SHIFT)) |
           ARC_EP_BULK << ARC_EPC_RX_TYPE_SHIFT;
}

/* the type a direction of ENDPTCTRL holds */
static inline unsigned arc_epctrl_type(uint32_t ctrl, bool dir_in)
{
    return (ctrl >> (dir_in ? ARC_EPC_TX_TYPE_SHIFT
                            : ARC_EPC_RX_TYPE_SHIFT)) & 3u;
}

/* FRINDEX steps between two completions (14-bit counter, microframes;
 * one millisecond is 8 at either speed) */
static inline unsigned arc_frindex_delta(uint32_t now, uint32_t then)
{
    return (now - then) & 0x3fffu;
}

#endif /* USB_ARC_ISO_H */
