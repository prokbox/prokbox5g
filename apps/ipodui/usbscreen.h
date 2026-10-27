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
 * iPod UI shell: the native USB screen (Phase 8a contract,
 * docs/phase8a-brief.md §3.2, §4.3; Revision 1 H1, M4, L4)
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
#ifndef _IPODUI_USBSCREEN_H_
#define _IPODUI_USBSCREEN_H_

#include <stdbool.h>

#define UI_USB_FPS      10              /* arrows, while I/O and lit */
#define UI_USB_FRAMES   12              /* 30-degree steps (U2: 6 over 180) */
#define UI_USB_IO_HOLD  (HZ/2)          /* I/O counts as active this long */
#define UI_USB_DISC_D   92              /* generator constants, mirrored (U1:
                                           photo + usb.png, not 80) */
#define UI_USB_DISC_CX  160
#define UI_USB_DISC_CY  99
#define UI_USB_BAND_Y   (UI_USB_DISC_CY - UI_USB_DISC_D / 2)   /* 53 */
#define UI_USB_BAND_H   UI_USB_DISC_D                         /* 92 rows */

enum ui_usb_glyph { UI_USB_GLYPH_DOCK30, UI_USB_GLYPH_USBC };
#ifdef HAVE_IPOD_DOCK30
#define UI_USB_IDLE_GLYPH UI_USB_GLYPH_DOCK30
#else
#define UI_USB_IDLE_GLYPH UI_USB_GLYPH_USBC
#endif

struct ui_usb_state {
    signed char frame;      /* identity: arrows frame on screen, -1 = idle glyph */
    bool io;                /* identity: I/O state last drawn */
    bool static_valid;      /* validity: header/body/text are in the framebuffer */
    bool frames_valid;      /* validity: the strip holds this entry's frames
                               (false if the claim was lost or never made) */
};

/* Called by apps/gui/usb_screen.c after usb_acknowledge(); UI thread.
 * Returns on SYS_USB_DISCONNECTED. Calls backlight_on() at entry (H1).
 * Boosts only for the entry compose and releases before blocking.
 * 'early' is informational; the loop checks ui_shell_started() for fonts
 * and the strip (static art, no text, no animation if not).
 * HID (L4): never calls get_hid_usb_action(); usb_hid is off by default
 * (hook H1b), and a developer who turns it on gets keys the shell ignores. */
void ui_usb_screen_loop(bool early);

/* -- sustained transfers (#58, owner) ------------------------------------
 * The arrows spun almost all the time a Mac had the disk mounted: its
 * background reads (Spotlight, fseventsd, Finder) touch the disk every
 * second or so. They now show only for a sustained transfer: the disk
 * busy in at least UI_USB_IO_MIN of the last UI_USB_IO_WINDOW slots of
 * UI_USB_IO_SLOT (~1 s of I/O within 1.5 s; at USB 2.0 rates that is
 * megabytes, where a background read is a few sectors). Once on, they stay
 * on until the disk has been quiet for UI_USB_IO_HOLD (0.5 s, as before),
 * and the window starts empty again after that. The storage driver keeps
 * no byte count, so "busy" is per slot: an access since the last sample
 * (storage_last_disk_activity()). Sampled at the slot rate only while
 * something is in the window or the arrows are on; else the loop polls at
 * UI_USB_IO_HOLD as before. Pure (host-tested in tests/usbio). */
#define UI_USB_IO_SLOT      (HZ / 10)   /* 100 ms */
#define UI_USB_IO_WINDOW    15          /* slots: 1.5 s */
#define UI_USB_IO_MIN       10          /* busy slots for "sustained" (a
                                           short burst on a slot boundary
                                           counts in two) */

struct ui_usb_io {
    unsigned short busy;    /* the last UI_USB_IO_WINDOW slots, newest
                               in bit 0 */
    long sample;            /* tick of the last sample */
    long last;              /* the last activity seen (valid: seen) */
    bool seen;
    bool on;                /* the arrows */
};

static inline int ui_usb_io_bits(unsigned v)
{
    int n = 0;

    for (; v; v &= v - 1)
        n++;
    return n;
}

/* Starts empty at 'now' (no I/O counted from before). */
static inline void ui_usb_io_init(struct ui_usb_io *s, long now)
{
    s->busy = 0;
    s->sample = now;
    s->last = now;
    s->seen = false;
    s->on = false;
}

/* One look at 'now': 'act' is the driver's last activity tick (act_ok:
 * there was any). Returns the arrows' state. Call at any rate; slots
 * advance by elapsed time (missed slots count idle, the newest takes any
 * access since the last sample). */
static inline bool ui_usb_io_step(struct ui_usb_io *s, long now, long act,
                                  bool act_ok)
{
    long el = now - s->sample;
    bool hit = act_ok && (long)(act - s->sample) > 0;

    if (el >= UI_USB_IO_SLOT)
    {
        long n = el / UI_USB_IO_SLOT;

        s->busy = n >= UI_USB_IO_WINDOW ? 0
                  : (unsigned short)(s->busy << n);
        if (hit)
            s->busy |= 1;
        s->busy &= (1u << UI_USB_IO_WINDOW) - 1;
        s->sample += n * UI_USB_IO_SLOT;
    }
    if (act_ok)
    {
        s->last = act;
        s->seen = true;
    }
    if (!s->seen || (long)(now - s->last) >= UI_USB_IO_HOLD)
    {
        if (s->on)
            s->busy = 0;            /* quiet: start the window again */
        s->on = false;
    }
    else if (ui_usb_io_bits(s->busy) >= UI_USB_IO_MIN)
        s->on = true;
    return s->on;
}

/* Ticks until the next useful look: a slot while anything is in the
 * window or the arrows are on, else UI_USB_IO_HOLD. */
static inline long ui_usb_io_wake(const struct ui_usb_io *s, long now)
{
    long next;

    if (!s->on && !s->busy)
        return UI_USB_IO_HOLD;
    next = s->sample + UI_USB_IO_SLOT - now;
    return next > 0 ? next : 1;
}

/* The arrows' state now (the screen's own detector, stepped) */
bool ui_usb_io_active(void);
/* true only while ui_usb_screen_loop() runs: the unlit "skip usb" test
 * (Revision 1 M4), never usb_inserted() */
bool ui_usb_screen_active(void);
/* ui_main.c (U2): true once the shell's fonts and strip are reserved */
bool ui_shell_started(void);
/* P11: how many times the USB screen has run since boot (a screen that
 * must leave after a USB session compares it, e.g. Charging) */
unsigned ui_usb_sessions(void);

/* A computer leaves the disk alone (owner, 2026-10-11): at boot, create
 * the markers macOS honours if they're missing: /.metadata_never_index
 * (no Spotlight index of the library over USB) and /.fseventsd/no_log
 * (no change log). Two tiny files, written once. */
void ui_usb_host_markers(void);

/* -- P11 (U2b): the dark chrome of the pushed dark screens -------------
 * About, Brightness, Charging (and U2a's Volume Limit) are ordinary
 * stack screens with header_w = UI_HEADER_DARK (header.h): the main loop
 * then draws this header instead of the light one, and the screen's
 * draw() paints its body with ui_dark_body(). Same art as the USB screen
 * (usbscreen_data.h), no extra tables. */
#define UI_DARK_HDR_H   22              /* = USB_HDR_H */
/* Rows y .. y+h-1 of the body gradient, full width, straight into the
 * framebuffer (no push; ~1.4k instructions per row). Rows above
 * UI_DARK_HDR_H repeat the first body row (headerless screens). */
void ui_dark_body(int y, int h);
/* The dark header with 'title' centred and the polled battery (bolt or
 * plug while on power). Draws when 'force' or when the title, battery,
 * charging or power state differs from its last draw; returns true if
 * it drew (the caller pushes rows 0..UI_DARK_HDR_H-1). No push. */
bool ui_dark_header(const char *title, bool force);

/* ui_main.c (U2b): a sideways page slide INSIDE the top screen (About's
 * pages). After its input() returns, the main loop captures rows y ..
 * y+h-1 (full width), redraws the screen (draw() with UI_DIRTY_BODY,
 * which must draw the new page) and slides the new band in from the
 * right (dir +1) or the left (-1), 190 ms ease-out, through the strip
 * like a push. Without the strip the new page just shows. */
void ui_slide_band(int dir, int y, int h);

#endif /* _IPODUI_USBSCREEN_H_ */
