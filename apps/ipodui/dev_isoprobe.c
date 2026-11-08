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
 * #144 P1: Developer > Advanced > Labs > USB audio probe (bench build only,
 * HAVE_USB_ISOPROBE).  While this screen is open the next cable session
 * is charge-only USB with the isochronous probe instead of mass storage;
 * leaving the screen switches it off (the switch expires with the screen).
 * Counters live on screen and in RAM; ipodui.log gets them only after the
 * cable is out and the controller is down.  Bench sheet:
 * docs/research/usb-isoprobe-bench.md.
 */
#include "config.h"

#ifdef HAVE_USB_ISOPROBE
#include <stdio.h>
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "usb.h"
#include "usb_drv.h"
#include "usb_isoprobe.h"
#include "audio.h"
#include "settings.h"
#include "action.h"
#include "gui/list.h"
#include "ipodui/uilog.h"
#include "ipodui/dev_isoprobe.h"

enum { ST_PLUG, ST_LIVE, ST_SETTLE, ST_LOGGED };

static const unsigned variants[] = {
    0, USB_ISO_VAR_A, USB_ISO_VAR_B, USB_ISO_VAR_C,
    USB_ISO_VAR_A | USB_ISO_VAR_B | USB_ISO_VAR_C,
};
static const char *const variant_name[] = {
    "baseline", "A (TR_ERR kept)", "B (ping-pong)", "C (bulk deinit)",
    "A+B+C",
};

static struct {
    int state;
    int var;                    /* index into variants[] */
    long out_tick;              /* the cable came out */
    long rate_tick;
    uint32_t rate_done, per_s;
    struct usb_isoprobe_counters c;
    struct usb_drv_iso_stats d;
    struct usb_drv_stats s;
} ip;

static void ip_sample(void)
{
    usb_isoprobe_get(&ip.c);
    usb_drv_iso_get_stats(&ip.d);
    usb_drv_get_stats(&ip.s);
    if (TIME_AFTER(current_tick, ip.rate_tick + HZ))
    {
        ip.per_s = (ip.c.done - ip.rate_done) * HZ /
                   (current_tick - ip.rate_tick);
        ip.rate_done = ip.c.done;
        ip.rate_tick = current_tick;
    }
}

static void ip_log(void)
{
    ui_log("isoprobe", "variant=%s", variant_name[ip.var]);
    ui_log("isoprobe", "id=%08lx dci=%08lx dcc=%08lx eps=%lu",
           (unsigned long)ip.d.id, (unsigned long)ip.d.dciversion,
           (unsigned long)ip.d.dccparams,
           (unsigned long)(ip.d.dccparams & 0x1f));
    ui_log("isoprobe", "portsc=%08lx epctrl=%08lx",
           (unsigned long)ip.d.portsc, (unsigned long)ip.d.epctrl);
    ui_log("isoprobe", "cfg=%lu alt1=%lu alt0=%lu rate=%lu cur=%lu bad=%lu",
           (unsigned long)ip.c.set_config, (unsigned long)ip.c.alt1,
           (unsigned long)ip.c.alt0, (unsigned long)ip.c.rate,
           (unsigned long)ip.c.set_cur, (unsigned long)ip.c.bad_req);
    ui_log("isoprobe", "done=%lu err=%lu refused=%lu disc=%lu",
           (unsigned long)ip.c.done, (unsigned long)ip.c.err,
           (unsigned long)ip.c.prime_fail, (unsigned long)ip.c.disconnects);
    ui_log("isoprobe", "fr=%lu..%lu odd=%lu",
           (unsigned long)ip.c.fr_min, (unsigned long)ip.c.fr_max,
           (unsigned long)ip.c.fr_odd);
    ui_log("isoprobe", "td done=%lu txn=%lu buf=%lu halt=%lu flush=%lu",
           (unsigned long)ip.d.done, (unsigned long)ip.d.txn_err,
           (unsigned long)ip.d.buf_err, (unsigned long)ip.d.halted,
           (unsigned long)ip.d.flushed);
    ui_log("isoprobe", "pp app=%lu prime=%lu busy=%lu atdtw=%lu",
           (unsigned long)ip.d.pp_append, (unsigned long)ip.d.pp_prime,
           (unsigned long)ip.d.pp_busy, (unsigned long)ip.d.pp_atdtw_to);
    ui_log("isoprobe", "prime to=%lu miss=%lu stale=%lu rst=%lu",
           (unsigned long)ip.s.prime_to, (unsigned long)ip.s.prime_miss,
           (unsigned long)ip.s.prime_stale, (unsigned long)ip.s.rst_real);
    ui_log("isoprobe", "irq n=%lu max=%luus handler max=%luus",
           (unsigned long)ip.d.irq_n, (unsigned long)ip.d.irq_max_us,
           (unsigned long)ip.c.handler_max_us);
    ui_log_hold(false);
    ui_log_flush();
}

#define IP_ROWS 12

static const char *ip_row(int row, void *data, char *buf, size_t len)
{
    static const char *const state[] = {
        "Plug the cable into the Mac", "Connected: record now",
        "Cable out: shutting down", "Logged. Plug in again or MENU" };

    (void)data;
    switch (row)
    {
    case 0:
        return (char *)state[ip.state];
    case 1:
        snprintf(buf, len, "Driver: %s%s", variant_name[ip.var],
                 ip.state == ST_PLUG ? " (SELECT)" : "");
        break;
    case 2:
        snprintf(buf, len, "Core %08lx dci %08lx", (unsigned long)ip.d.id,
                 (unsigned long)ip.d.dciversion);
        break;
    case 3:
        snprintf(buf, len, "Endpoints %lu, %s speed",
                 (unsigned long)(ip.d.dccparams & 0x1f),
                 !ip.d.portsc ? "-" : (ip.d.portsc >> 26 & 3) == 2 ? "high"
                 : "full");
        break;
    case 4:
        snprintf(buf, len, "Host: cfg %lu alt1 %lu rate %lu",
                 (unsigned long)ip.c.set_config, (unsigned long)ip.c.alt1,
                 (unsigned long)ip.c.rate);
        break;
    case 5:
        snprintf(buf, len, "Packets/s %lu, total %lu",
                 (unsigned long)ip.per_s, (unsigned long)ip.c.done);
        break;
    case 6:
        snprintf(buf, len, "Frame step %lu..%lu, odd %lu",
                 (unsigned long)ip.c.fr_min, (unsigned long)ip.c.fr_max,
                 (unsigned long)ip.c.fr_odd);
        break;
    case 7:
        snprintf(buf, len, "dTD txn %lu buf %lu halt %lu fl %lu",
                 (unsigned long)ip.d.txn_err, (unsigned long)ip.d.buf_err,
                 (unsigned long)ip.d.halted, (unsigned long)ip.d.flushed);
        break;
    case 8:
        snprintf(buf, len, "Prime refused %lu, timeout %lu",
                 (unsigned long)ip.c.prime_fail,
                 (unsigned long)ip.s.prime_to);
        break;
    case 9:
        snprintf(buf, len, "IRQ max %lu us, handler %lu us",
                 (unsigned long)ip.d.irq_max_us,
                 (unsigned long)ip.c.handler_max_us);
        break;
    case 10:
        if (!ip.c.streaming)
            return "Sound: not streaming";
        return isoprobe_tone_now(USEC_TIMER - ip.c.open_us)
               ? "Sound: 1 kHz tone (20 s)" : "Sound: silence (10 s)";
    default:
        return ip.state == ST_PLUG || ip.state == ST_LOGGED
               ? "MENU: leave (probe off)" : "Unplug before leaving";
    }
    return buf;
}

static int ip_action(int action, struct gui_synclist *lists)
{
    (void)lists;
    ip_sample();
    switch (ip.state)
    {
    case ST_PLUG:
    case ST_LOGGED:
        if (usb_inserted())
        {
            ip.state = ST_LIVE;
            ui_log_hold(true);          /* nothing written while it runs */
        }
        break;
    case ST_LIVE:
        if (!usb_inserted())
        {
            ip.state = ST_SETTLE;
            ip.out_tick = current_tick;
        }
        break;
    case ST_SETTLE:
        /* the USB thread takes the stack down within milliseconds of the
         * cable going; a second leaves it well behind */
        if (usb_inserted())
            ip.state = ST_LIVE;
        else if (TIME_AFTER(current_tick, ip.out_tick + HZ))
        {
            ip_sample();
            ip_log();
            ip.state = ST_LOGGED;
        }
        break;
    }
    if (action == ACTION_STD_OK)
    {
        if (ip.state == ST_PLUG)
        {
            ip.var = (ip.var + 1) % (int)ARRAYLEN(variants);
            usb_isoprobe_arm(true, variants[ip.var]);
        }
        return ACTION_REDRAW;
    }
    if (action == ACTION_STD_CANCEL &&
        (ip.state == ST_LIVE || ip.state == ST_SETTLE))
        return ACTION_REDRAW;           /* unplug first */
    if (action == ACTION_NONE)
        return ACTION_REDRAW;
    return action;
}

int dev_isoprobe_run(void)
{
    struct simplelist_info info;

    memset(&ip, 0, sizeof(ip));
    ip.rate_tick = current_tick;
    audio_stop();                       /* no disk, no audio thread work */
    usb_set_mode(USB_MODE_CHARGE);      /* a cable in must not take the disk */
    usb_isoprobe_arm(true, variants[0]);
    ip.state = usb_inserted() ? ST_LOGGED : ST_PLUG;    /* an old cable */
    ui_log("isoprobe", "open");

    simplelist_info_init(&info, "USB audio probe", IP_ROWS, NULL);
    info.get_name = ip_row;
    info.action_callback = ip_action;
    info.timeout = HZ / 4;
    info.speak_onshow = false;
    simplelist_show_list(&info);

    usb_isoprobe_arm(false, 0);         /* the switch expires here */
    usb_set_mode(global_settings.usb_mode);
    ui_log_hold(false);
    return 0;
}
#endif /* HAVE_USB_ISOPROBE */
