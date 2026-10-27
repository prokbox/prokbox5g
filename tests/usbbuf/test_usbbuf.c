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

/* Host test of usb_buf_alloc_core() (firmware/common/usb_buf_alloc.c):
 * the USB storage buffer at SET_CONFIGURATION never panics. Device case
 * (2026-10-26): paused, the audio buffer refuses the shrink until the
 * queue stops; the core must stop it first and then allocate. */
#ifndef USB_BUF_HOST
#define USB_BUF_HOST
#endif
#include "usb_buf_alloc.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

struct fake {
    bool playing;       /* playing or paused */
    bool refuse_active; /* a paused queue refuses (playback.c #88) */
    int fail_n;         /* allocations that fail even when stopped */
    int stops, allocs, waits;
    int order_stop_at;  /* allocs seen when stop ran */
    char last[128];
};

static bool f_active(void *c) { return ((struct fake *)c)->playing; }

static void f_stop(void *c)
{
    struct fake *f = c;
    f->stops++;
    f->order_stop_at = f->allocs;
    f->playing = false;
}

static int f_alloc(void *c)
{
    struct fake *f = c;
    f->allocs++;
    if (f->playing && f->refuse_active)
        return -1;
    if (f->fail_n > 0)
    {
        f->fail_n--;
        return -2;
    }
    return 7;
}

static void f_wait(void *c) { ((struct fake *)c)->waits++; }
static long f_now(void *c) { return ((struct fake *)c)->stops * 30L; }

static void f_log(void *c, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static void f_log(void *c, const char *fmt, ...)
{
    struct fake *f = c;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(f->last, sizeof f->last, fmt, ap);
    va_end(ap);
}

static int fails;
#define CHECK(c) do { if (!(c)) { \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static int run(struct fake *f, int tries)
{
    struct usb_buf_ops o = { f_active, f_stop, f_alloc, f_wait, f_now, f_log, f };
    return usb_buf_alloc_core(&o, tries);
}

int main(void)
{
    struct fake f;

    /* the device case: paused, the shrink refused while active */
    memset(&f, 0, sizeof f);
    f.playing = f.refuse_active = true;
    CHECK(run(&f, USB_BUF_TRIES) == 7);
    CHECK(f.stops == 1 && f.order_stop_at == 0);    /* stop came first */
    CHECK(f.allocs == 1 && f.waits == 0);
    CHECK(!strcmp(f.last, "alloc stop playback ms=30"));

    /* stopped: no stop, one allocation, no log line */
    memset(&f, 0, sizeof f);
    CHECK(run(&f, USB_BUF_TRIES) == 7);
    CHECK(f.stops == 0 && f.allocs == 1 && f.waits == 0 && !f.last[0]);

    /* a give on its way: two failures, then the bytes */
    memset(&f, 0, sizeof f);
    f.fail_n = 2;
    CHECK(run(&f, USB_BUF_TRIES) == 7);
    CHECK(f.allocs == 3 && f.waits == 2);
    CHECK(!strcmp(f.last, "alloc ok tries=3"));

    /* no memory at all: bounded, the failure returned, never a panic */
    memset(&f, 0, sizeof f);
    f.playing = true;
    f.fail_n = 1000;
    CHECK(run(&f, USB_BUF_TRIES) == -2);
    CHECK(f.stops == 1 && f.allocs == USB_BUF_TRIES
          && f.waits == USB_BUF_TRIES - 1);
    CHECK(strstr(f.last, "alloc FAILED tries=10 rc=-2") == f.last);

    /* the bound: about a second in all */
    CHECK((USB_BUF_TRIES - 1) * USB_BUF_WAIT_MS <= 1000);

    if (fails)
        return 1;
    return 0;
}
