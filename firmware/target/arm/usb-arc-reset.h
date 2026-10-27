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
 * ARC/ChipIdea device controller: what a USB reset interrupt (USBSTS.URI)
 * means, and the compact form of the registers the device log keeps for
 * each one (ProkBox, USB resets). Pure, so tests/usb/reset_test.c runs it
 * on the host.
 *
 * A URI is the controller's own verdict that the bus was reset, and on a
 * high-speed link it needs more than 3 ms without a microframe SOF, which
 * a working host never leaves. So nearly every URI is real. The one case
 * treated as not real: the port no longer shows the reset (PORTSC.PR
 * clear) and the device still holds the address it was given (the
 * controller did not clear DEVICEADDR). Then nothing was torn down in the
 * hardware, and tearing storage down in software would only make the
 * device deaf to the host that still talks to that address. If such a
 * URI was in fact a real reset caught late, the host's next request goes
 * to address 0, gets no answer, and the host resets again; that reset
 * shows PR and takes the full path. The cost of a wrong guess is one
 * extra host retry, never a lost device.
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
#ifndef USB_ARC_RESET_H
#define USB_ARC_RESET_H

#include <stdint.h>

/* PORTSC1 bits (ARC device mode) */
#define ARC_PS_CCS          0x00000001u     /* current connect status */
#define ARC_PS_PE           0x00000004u     /* port enabled (high speed) */
#define ARC_PS_SUSP         0x00000080u     /* suspended */
#define ARC_PS_PR           0x00000100u     /* port in reset */
#define ARC_PS_LS_MASK      0x00000c00u     /* line state */
#define ARC_PS_LS_SHIFT     10
#define ARC_PS_SPD_MASK     0x0c000000u     /* 0 full, 1 low, 2 high */
#define ARC_PS_SPD_SHIFT    26
#define ARC_ADDR_SHIFT      25              /* DEVICEADDR[31:25] */

enum arc_reset_kind {
    ARC_RST_PR = 0,         /* the port is in reset: real */
    ARC_RST_ADDR0,          /* the controller cleared our address: real */
    ARC_RST_UNADDR,         /* not addressed yet: nothing to lose, real */
    ARC_RST_SPURIOUS,       /* PR clear, our address intact: not real */
};

/* portsc: PORTSC1 at the interrupt; deviceaddr: DEVICEADDR at the
 * interrupt, before the driver clears it; assigned: the address the
 * driver last set (0: none) */
static inline enum arc_reset_kind arc_reset_classify(uint32_t portsc,
                                                     uint32_t deviceaddr,
                                                     unsigned assigned)
{
    unsigned hw = (deviceaddr >> ARC_ADDR_SHIFT) & 0x7f;

    if (portsc & ARC_PS_PR)
        return ARC_RST_PR;
    if (assigned == 0)
        return ARC_RST_UNADDR;
    if (hw != assigned)
        return ARC_RST_ADDR0;
    return ARC_RST_SPURIOUS;
}

static inline int arc_reset_is_real(enum arc_reset_kind k)
{
    return k != ARC_RST_SPURIOUS;
}

/* PORTSC1 in one byte for the log: bit 0 PR, 1 SUSP, 2 PE, 3 CCS,
 * 4-5 line state (0 SE0, 1 J, 2 K), 6-7 speed (0 full, 1 low, 2 high) */
static inline uint8_t arc_portsc_byte(uint32_t portsc)
{
    return (uint8_t)(((portsc & ARC_PS_PR) ? 0x01 : 0) |
                     ((portsc & ARC_PS_SUSP) ? 0x02 : 0) |
                     ((portsc & ARC_PS_PE) ? 0x04 : 0) |
                     ((portsc & ARC_PS_CCS) ? 0x08 : 0) |
                     (((portsc & ARC_PS_LS_MASK) >> ARC_PS_LS_SHIFT) << 4) |
                     (((portsc & ARC_PS_SPD_MASK) >> ARC_PS_SPD_SHIFT) << 6));
}

/* ENDPTSTATUS/ENDPTPRIME in one byte: OUT endpoints 0-3 in bits 0-3,
 * IN endpoints 0-3 in bits 4-7 */
static inline uint8_t arc_ep_byte(uint32_t ep)
{
    return (uint8_t)((ep & 0x0f) | ((ep >> 12) & 0xf0));
}

/* the gap since the previous reset, in four buckets for the log:
 * < 100 ms (one reset seen twice, or a storm), < 1 s, < 10 s, longer */
static inline int arc_gap_bucket(uint32_t ms)
{
    return ms < 100 ? 0 : ms < 1000 ? 1 : ms < 10000 ? 2 : 3;
}

/* ---- completions, dTD errors, lengths (usb-drv-arc.c) ---- */

/* dTD status bits that end a chain early: the controller halts the
 * endpoint and the dTDs behind the failed one stay ACTIVE for ever */
#define ARC_TD_HALTED       0x40u
#define ARC_TD_BUF_ERR      0x20u
#define ARC_TD_TXN_ERR      0x08u
#define ARC_TD_ERR_MASK     (ARC_TD_HALTED | ARC_TD_BUF_ERR | ARC_TD_TXN_ERR)

/* ENDPTCOMPLETE bits to clear: the pipes whose chain really finished.
 * A pipe still ACTIVE keeps its bit, so its completion is delivered when
 * the last dTD retires instead of being cleared and lost. */
static inline uint32_t arc_complete_clear(uint32_t seen, uint32_t unfinished)
{
    return seen & ~unfinished;
}

/* the error bits of one dTD status byte (0: none) */
static inline uint32_t arc_td_error(uint32_t sts)
{
    return sts & ARC_TD_ERR_MASK;
}

/* dTDs a transfer of len bytes needs at per_td bytes each, or -1 when it
 * does not fit in max_tds (negative length: -1 too) */
static inline int arc_tds_needed(int len, int per_td, int max_tds)
{
    int n;

    if (len < 0)
        return -1;
    n = len ? (len + per_td - 1) / per_td : 1;
    return n > max_tds ? -1 : n;
}

#endif /* USB_ARC_RESET_H */
