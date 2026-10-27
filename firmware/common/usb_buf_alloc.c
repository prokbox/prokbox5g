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

/* see firmware/export/usb_buf_alloc.h */
#include "usb_buf_alloc.h"

int usb_buf_alloc_core(const struct usb_buf_ops *o, int tries)
{
    int h = 0, i;

    if (o->active(o->ctx))
    {
        /* the host is about to own the disk: playback ends here, as the
         * audio thread would end it at SYS_USB_CONNECTED a moment later */
        long t0 = o->now_ms(o->ctx);

        o->stop(o->ctx);
        o->log(o->ctx, "alloc stop playback ms=%ld", o->now_ms(o->ctx) - t0);
    }
    for (i = 0; i < tries; i++)
    {
        if (i)
            o->wait(o->ctx);
        h = o->alloc(o->ctx);
        if (h > 0)
        {
            if (i)
                o->log(o->ctx, "alloc ok tries=%d", i + 1);
            return h;
        }
    }
    o->log(o->ctx, "alloc FAILED tries=%d rc=%d: no disk mode", tries, h);
    return h;
}

#if !defined(USB_BUF_HOST)
#include "config.h"
#include "kernel.h"
#include "audio.h"
#include "core_alloc.h"
#include <stdio.h>

struct usb_buf_req {
    size_t size;
    struct buflib_callbacks *ops;
    const char *who;
};

static void (*volatile shell_log)(const char *line);

void usb_buf_set_log(void (*log)(const char *line))
{
    shell_log = log;
}

static bool buf_active(void *ctx)
{
    (void)ctx;
    return audio_status() != 0;
}

static void buf_stop(void *ctx)
{
    (void)ctx;
    audio_stop();                   /* Q_AUDIO_STOP, sent: it waits */
}

static int buf_alloc(void *ctx)
{
    struct usb_buf_req *r = ctx;
    return core_alloc_ex(r->size, r->ops);
}

static void buf_wait(void *ctx)
{
    (void)ctx;
    sleep(USB_BUF_WAIT_MS * HZ / 1000);
}

static long buf_now_ms(void *ctx)
{
    (void)ctx;
    return current_tick * (1000 / HZ);
}

static void buf_log(void *ctx, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static void buf_log(void *ctx, const char *fmt, ...)
{
    struct usb_buf_req *r = ctx;
    void (*log)(const char *) = shell_log;
    char line[96];
    va_list ap;
    int n;

    if (!log)
        return;
    n = snprintf(line, sizeof line, "%s ", r->who);
    if (n < 0 || n >= (int)sizeof line)
        return;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof line - n, fmt, ap);
    va_end(ap);
    log(line);
}

int usb_buf_alloc(size_t size, struct buflib_callbacks *ops, const char *who)
{
    struct usb_buf_req r = { size, ops, who };
    const struct usb_buf_ops o = {
        buf_active, buf_stop, buf_alloc, buf_wait, buf_now_ms, buf_log, &r
    };

    return usb_buf_alloc_core(&o, USB_BUF_TRIES);
}
#endif /* !USB_BUF_HOST */
