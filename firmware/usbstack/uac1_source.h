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
 * USB Audio Class 1.0 streaming source (the iPod as an audio input to the
 * host): the descriptor builder and the packet sizer, pure so
 * tests/usbaudio checks them byte for byte on the host (#144 P0).
 *
 * The shape follows what the 5G's own firmware enumerates, read from its
 * descriptor bytes (docs/research/usb-digital-audio.md §1): one Audio
 * Control interface, a microphone input terminal feeding a USB streaming
 * output terminal; one Audio Streaming interface with alt 0 (no endpoint)
 * and alt 1 (one isochronous IN endpoint, synchronisation none, 16-bit
 * stereo PCM, discrete rates, sampling-frequency control).  Written from
 * the UAC 1.0 specification; no bytes are copied.
 */
#ifndef UAC1_SOURCE_H
#define UAC1_SOURCE_H

#include <stdint.h>
#include <stdbool.h>

#define UAC1_MAX_RATES      9
#define UAC1_FRAME_BYTES    4               /* 16-bit stereo */
/* Apple's wMaxPacketSize: 48 kHz x 4 bytes in 1 ms */
#define UAC1_SRC_MPS        192

/* the descriptors' length for n rates */
#define UAC1_SRC_LEN(n)     (9 + 9 + 12 + 9 + 9 + 9 + 7 + (8 + 3 * (n)) + 9 + 7)

struct uac1_src_cfg {
    uint8_t if_ac;          /* Audio Control interface; streaming is +1 */
    uint8_t ep_in;          /* endpoint address, with the IN bit */
    bool high_speed;        /* bInterval 4 (8 microframes), else 1 */
    uint16_t mps;           /* wMaxPacketSize */
    uint8_t nrates;         /* 1..UAC1_MAX_RATES */
    uint32_t rates[UAC1_MAX_RATES];
};

static inline uint8_t *uac1_put(uint8_t *p, const uint8_t *b, int n)
{
    while (n-- > 0)
        *p++ = *b++;
    return p;
}

/* Writes the interface-level descriptors (no configuration descriptor:
 * usb_core writes that) to dest.  Returns the length, or -1 for a bad
 * rate count. */
static inline int uac1_src_build(uint8_t *dest, const struct uac1_src_cfg *c)
{
    uint8_t *p = dest;
    const uint8_t if_as = c->if_ac + 1;
    const uint16_t ac_total = 9 + 12 + 9;
    int i;

    if (c->nrates < 1 || c->nrates > UAC1_MAX_RATES)
        return -1;
    {
        /* standard AC interface: class audio (1), subclass control (1) */
        const uint8_t ac_if[9] = { 9, 4, c->if_ac, 0, 0, 1, 1, 0, 0 };
        /* class AC header: bcdADC 1.00, one streaming interface */
        const uint8_t ac_hdr[9] = { 9, 0x24, 1, 0x00, 0x01,
                                    ac_total & 0xff, ac_total >> 8, 1, if_as };
        /* input terminal 1: microphone (0x0201), 2 channels, L+R */
        const uint8_t it[12] = { 12, 0x24, 2, 1, 0x01, 0x02, 0, 2,
                                 0x03, 0x00, 0, 0 };
        /* output terminal 2: USB streaming (0x0101), fed by terminal 1 */
        const uint8_t ot[9] = { 9, 0x24, 3, 2, 0x01, 0x01, 0, 1, 0 };
        /* standard AS interface, alt 0 (no endpoint) and alt 1 (one) */
        const uint8_t as0[9] = { 9, 4, if_as, 0, 0, 1, 2, 0, 0 };
        const uint8_t as1[9] = { 9, 4, if_as, 1, 1, 1, 2, 0, 0 };
        /* class AS general: linked to terminal 2, delay 1, PCM */
        const uint8_t as_gen[7] = { 7, 0x24, 1, 2, 1, 0x01, 0x00 };
        /* Type I format: 2 channels, 2 bytes, 16 bits, n discrete rates */
        const uint8_t fmt[8] = { (uint8_t)(8 + 3 * c->nrates), 0x24, 2, 1,
                                 2, 2, 16, c->nrates };
        /* standard audio endpoint (9 bytes in UAC1): isochronous, sync
         * none; 1 ms at either speed */
        const uint8_t ep[9] = { 9, 5, c->ep_in, 0x01,
                                c->mps & 0xff, c->mps >> 8,
                                c->high_speed ? 4 : 1, 0, 0 };
        /* class endpoint: sampling-frequency control, no lock delay */
        const uint8_t cs_ep[7] = { 7, 0x25, 1, 0x01, 0, 0, 0 };

        p = uac1_put(p, ac_if, 9);
        p = uac1_put(p, ac_hdr, 9);
        p = uac1_put(p, it, 12);
        p = uac1_put(p, ot, 9);
        p = uac1_put(p, as0, 9);
        p = uac1_put(p, as1, 9);
        p = uac1_put(p, as_gen, 7);
        p = uac1_put(p, fmt, 8);
        for (i = 0; i < c->nrates; i++)
        {
            *p++ = c->rates[i] & 0xff;
            *p++ = c->rates[i] >> 8 & 0xff;
            *p++ = c->rates[i] >> 16 & 0xff;
        }
        p = uac1_put(p, ep, 9);
        p = uac1_put(p, cs_ep, 7);
    }
    return (int)(p - dest);
}

/* Is rate one of the configured ones? (SET_CUR is refused otherwise) */
static inline bool uac1_src_rate_ok(const struct uac1_src_cfg *c,
                                    uint32_t rate)
{
    int i;

    for (i = 0; i < c->nrates; i++)
        if (c->rates[i] == rate)
            return true;
    return false;
}

/* The packet sizer.  The host's frames are the clock: one packet per
 * millisecond, so rate / 1000 frames each, the remainder carried.  44.1
 * kHz gives 44 nine times and 45 once in every ten packets, exactly
 * 44,100 per 1,000; 48 kHz gives 48 every time.  No drift against the
 * host can arise (research §2.3). */
struct uac1_pacer {
    uint32_t rate;
    uint32_t acc;           /* remainder, in 1/1000 sample */
};

static inline void uac1_pacer_init(struct uac1_pacer *p, uint32_t rate)
{
    p->rate = rate;
    p->acc = 0;
}

/* sample frames (4 bytes each) for the next packet */
static inline unsigned uac1_pacer_next(struct uac1_pacer *p)
{
    unsigned n;

    p->acc += p->rate;
    n = p->acc / 1000;
    p->acc -= n * 1000;
    return n;
}

#endif /* UAC1_SOURCE_H */
