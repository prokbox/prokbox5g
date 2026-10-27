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

#ifndef _USB_BUF_ALLOC_H_
#define _USB_BUF_ALLOC_H_

/* ProkBox (HAVE_IPOD_UI): the USB storage buffer, taken by the USB thread
 * at SET_CONFIGURATION (usb_storage_init_connection()). That runs before
 * usb_request_exclusive_storage() broadcasts SYS_USB_CONNECTED, so the
 * audio buffer still holds all the free memory, and a paused queue
 * refuses another thread's shrink (playback.c shrink_callback(), #88: the
 * bytes come later, from a posted give). Upstream panicked on that OOM
 * (device, 2026-10-26: USB plugged in on a paused Now Playing).
 *
 * Disk mode stops playback anyway (the audio thread stops it at
 * SYS_USB_CONNECTED, as the stock firmware does), so the stop comes
 * first here, outside any buflib walk: the shrink then takes the plain
 * stopped path. A failure is retried a few times (another thread's give
 * may be on its way), then reported to the caller and logged, never a
 * panic: the cable then charges without offering the disk.
 *
 * The decisions are pure (usb_buf_alloc_core(), host-tested in
 * tests/usbbuf); usb_buf_alloc() wires them to audio_status(),
 * audio_stop(), core_alloc_ex() and sleep(). */

#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>

#define USB_BUF_TRIES   10          /* allocation attempts in all */
#define USB_BUF_WAIT_MS 100         /* between attempts: about 1 s in all */

struct usb_buf_ops {
    bool (*active)(void *ctx);      /* playing or paused */
    void (*stop)(void *ctx);        /* stops it, synchronously */
    int (*alloc)(void *ctx);        /* handle > 0, or <= 0: no memory */
    void (*wait)(void *ctx);        /* USB_BUF_WAIT_MS */
    long (*now_ms)(void *ctx);      /* a millisecond clock (the log) */
    void (*log)(void *ctx, const char *fmt, ...)
        __attribute__((format(printf, 2, 3)));
    void *ctx;
};

/* Stop playback if it is active, then allocate, retrying up to 'tries'
 * times in all. Returns the handle (> 0) or the last failure (<= 0). */
int usb_buf_alloc_core(const struct usb_buf_ops *o, int tries);

#if !defined(USB_BUF_HOST)
struct buflib_callbacks;
/* The USB thread's buffer: 'who' names the caller in the log. */
int usb_buf_alloc(size_t size, struct buflib_callbacks *ops,
                  const char *who);
/* The shell's log: one line per call, from the USB thread; NULL to stop */
void usb_buf_set_log(void (*log)(const char *line));
#endif

#endif /* _USB_BUF_ALLOC_H_ */
