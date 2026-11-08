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
 * #144 P1: the isochronous bench probe, a tiny UAC1 source class driver
 * (developer bench build only, HAVE_USB_ISOPROBE).  No audio thread, no
 * allocator, no DSP: two static uncached packets, a 1 kHz tone, then
 * silence.  docs/research/usb-digital-audio.md §7.
 */
#ifndef USB_ISOPROBE_H
#define USB_ISOPROBE_H

#include <stdint.h>
#include <stdbool.h>

/* From the stream's open, in a repeating cycle: the tone, then silence
 * (QuickTime opens the stream for its level meter before recording, so
 * any 60 s recording holds both) */
#define ISOPROBE_CYCLE_MS   30000
#define ISOPROBE_TONE_MS    20000
static inline bool isoprobe_tone_now(uint32_t since_open_us)
{
    return since_open_us % (ISOPROBE_CYCLE_MS * 1000u) <
           ISOPROBE_TONE_MS * 1000u;
}

struct usb_isoprobe_counters {
    uint32_t set_config;        /* SET_CONFIGURATION reaching the probe */
    uint32_t disconnects;       /* the config dropped (or the cable out) */
    uint32_t alt1, alt0;        /* SET_INTERFACE on the streaming interface */
    uint32_t rate;              /* the last SET_CUR sampling frequency */
    uint32_t set_cur, bad_req;  /* SET_CURs taken / requests refused */
    uint32_t done;              /* IN completions since the stream opened */
    uint32_t err;               /* ... with a non-zero status */
    uint32_t prime_fail;        /* re-arms the driver refused */
    uint32_t fr_min, fr_max;    /* FRINDEX steps between completions */
    uint32_t fr_odd;            /* steps other than 8 (one millisecond) */
    uint32_t open_us;           /* USEC_TIMER at the stream's open */
    uint32_t handler_max_us;    /* the longest completion handler */
    bool streaming;
};

/* Arms the probe for the next cable session (charge-only USB, no mass
 * storage) with the driver variants (USB_ISO_VAR_*); false disarms. */
void usb_isoprobe_arm(bool on, unsigned variant);
bool usb_isoprobe_armed(void);
void usb_isoprobe_get(struct usb_isoprobe_counters *out);

struct usb_class_driver;
extern struct usb_class_driver usb_cdrv_isoprobe;

#endif /* USB_ISOPROBE_H */
