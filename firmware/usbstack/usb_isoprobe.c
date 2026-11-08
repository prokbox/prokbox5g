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
 * #144 P1: the isochronous bench probe (developer bench build only).
 * A UAC1 source with Apple's 5G shape (uac1_source.h), 48 kHz only: one
 * isochronous IN packet of 192 bytes per millisecond, re-armed from the
 * completion interrupt.  docs/research/usb-digital-audio.md §7.
 */
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "string.h"
#include "usb_ch9.h"
#include "usb_core.h"
#include "usb_drv.h"
#include "usb_class_driver.h"
#include "usb_isoprobe.h"
#include "uac1_source.h"
#include "usb-arc-iso.h"

#define PKT         UAC1_SRC_MPS            /* 48 frames x 4 bytes */
#define RATE        48000
#define UAC_SET_CUR 0x01
#define UAC_GET_CUR 0x81
#define UAC_SF_CTL  0x0100                  /* sampling frequency control */

static struct usb_class_driver_ep_allocation ep_allocs[1] = {
    { .type = USB_ENDPOINT_XFER_ISOC, .dir = DIR_IN, .mps = PKT },
};

static bool armed;
static unsigned variant;
static int if_ac;
static struct usb_isoprobe_counters ct;
static uint32_t fr_last;
static int slot;                        /* B: the next ping-pong slot */

/* The two packets: one millisecond of a 1 kHz tone at -12 dBFS (exactly
 * one cycle, so every packet is the same) and one of silence.  Static,
 * written once through the uncached alias, read only by the controller. */
static uint8_t pkt_buf[2][PKT] __attribute__((aligned(32)));
#define PKT_TONE    ((void *)UNCACHED_ADDR(&pkt_buf[0][0]))
#define PKT_SILENT  ((void *)UNCACHED_ADDR(&pkt_buf[1][0]))

/* a quarter of the sine, 13 points of 48 */
static const int16_t quarter[13] = {
    0, 1069, 2120, 3135, 4096, 4987, 5793, 6499, 7094, 7568, 7913, 8122, 8192
};

static void fill_packets(void)
{
    uint8_t *t = PKT_TONE, *s = PKT_SILENT;
    int k, v;

    commit_discard_dcache();    /* no dirty line may land on them later */
    for (k = 0; k < 48; k++)
    {
        v = k <= 12 ? quarter[k] : k <= 24 ? quarter[24 - k]
          : k <= 36 ? -quarter[k - 24] : -quarter[48 - k];
        t[4 * k] = t[4 * k + 2] = v & 0xff;           /* left, right */
        t[4 * k + 1] = t[4 * k + 3] = (v >> 8) & 0xff;
    }
    memset(s, 0, PKT);
}

void usb_isoprobe_arm(bool on, unsigned var)
{
    if (on)
        fill_packets();
    variant = var;
    usb_drv_iso_set_variant(on ? var : 0);
    armed = on;
}

bool usb_isoprobe_armed(void)
{
    return armed;
}

void usb_isoprobe_get(struct usb_isoprobe_counters *out)
{
    int oldlevel = disable_irq_save();
    *out = ct;
    restore_irq(oldlevel);
}

static void *next_packet(void)
{
    return isoprobe_tone_now(USEC_TIMER - ct.open_us) ? PKT_TONE
                                                       : PKT_SILENT;
}

/* Hands the next packet to the controller; IRQ context or not */
static void arm_packet(void)
{
    int rc;

    if (variant & USB_ISO_VAR_B)
    {
        rc = usb_drv_iso_pp_queue(ep_allocs[0].ep, slot, next_packet(), PKT);
        slot ^= 1;
    }
    else
        rc = usb_drv_send_nonblocking(ep_allocs[0].ep, next_packet(), PKT);
    if (rc < 0)
        ct.prime_fail++;
}

static void stream(bool on)
{
    int oldlevel = disable_irq_save();
    bool was = ct.streaming;

    ct.streaming = on;
    if (on && !was)
    {
        ct.open_us = USEC_TIMER;
        fr_last = usb_drv_iso_frindex();
        slot = 0;
        arm_packet();
        if (variant & USB_ISO_VAR_B)
            arm_packet();               /* two in flight */
    }
    restore_irq(oldlevel);
    if (!on && was)
    {
        if (variant & USB_ISO_VAR_B)
            usb_drv_iso_pp_stop(ep_allocs[0].ep);
        else
            usb_drv_reset_endpoint(ep_allocs[0].ep, true);
    }
}

static bool isoprobe_fast_complete(int ep, int dir, int status, int length)
{
    uint32_t t0 = USEC_TIMER, fr, d;

    (void)ep; (void)dir; (void)length;
    if (!ct.streaming)
        return true;
    fr = usb_drv_iso_frindex();
    d = arc_frindex_delta(fr, fr_last);
    fr_last = fr;
    if (ct.done)
    {
        if (!ct.fr_min || d < ct.fr_min)
            ct.fr_min = d;
        if (d > ct.fr_max)
            ct.fr_max = d;
        if (d != 8)
            ct.fr_odd++;
    }
    ct.done++;
    if (status)
        ct.err++;
    arm_packet();
    t0 = USEC_TIMER - t0;
    if (t0 > ct.handler_max_us)
        ct.handler_max_us = t0;
    return true;
}

static int isoprobe_set_first_interface(int interface)
{
    if_ac = interface;
    return interface + 2;
}

static int isoprobe_get_config_descriptor(unsigned char *dest, int max_pkt)
{
    struct uac1_src_cfg c;

    memset(&c, 0, sizeof(c));
    c.if_ac = if_ac;
    c.ep_in = ep_allocs[0].ep;
    c.high_speed = max_pkt == 512;
    c.mps = PKT;
    c.nrates = 1;
    c.rates[0] = RATE;
    return uac1_src_build(dest, &c);
}

static int isoprobe_init_connection(void)
{
    ct.set_config++;
    return 0;
}

static void isoprobe_disconnect(void)
{
    stream(false);
    ct.disconnects++;
}

static int isoprobe_set_interface(int interface, int alt)
{
    if (interface != if_ac + 1 || alt > 1)
        return interface == if_ac && alt == 0 ? 0 : -1;
    if (alt)
        ct.alt1++;
    else
        ct.alt0++;
    stream(alt == 1);
    return 0;
}

static int isoprobe_get_interface(int interface)
{
    return interface == if_ac + 1 && ct.streaming ? 1 : 0;
}

static bool isoprobe_control_request(struct usb_ctrlrequest *req,
                                     uint8_t *reqdata, size_t reqdata_size)
{
    uint32_t rate;

    (void)reqdata_size;
    if ((req->bRequestType & USB_RECIP_MASK) != USB_RECIP_ENDPOINT ||
        (req->bRequestType & USB_TYPE_MASK) != USB_TYPE_CLASS ||
        req->wValue != UAC_SF_CTL || req->wLength != 3)
    {
        ct.bad_req++;
        return false;                   /* usb_core stalls it */
    }
    if (req->bRequest == UAC_SET_CUR)
    {
        rate = reqdata[0] | reqdata[1] << 8 | (uint32_t)reqdata[2] << 16;
        ct.rate = rate;
        ct.set_cur++;
        usb_core_control_response(rate == RATE ? USB_CONTROL_ACK
                                               : USB_CONTROL_STALL, NULL, 0);
        return true;
    }
    if (req->bRequest == UAC_GET_CUR)
    {
        reqdata[0] = RATE & 0xff;
        reqdata[1] = RATE >> 8 & 0xff;
        reqdata[2] = RATE >> 16 & 0xff;
        usb_core_control_response(USB_CONTROL_ACK, reqdata, 3);
        return true;
    }
    ct.bad_req++;
    return false;
}

static void isoprobe_init(void)
{
    /* a new cable session: the counters start again (usb_core_init) */
    memset(&ct, 0, sizeof(ct));
}

struct usb_class_driver usb_cdrv_isoprobe = {
    .needs_exclusive_storage = false,
    .needs_cpu_boost = false,           /* measured at the idle clock */
    .config = 1,
    .ep_allocs_size = 1,
    .ep_allocs = ep_allocs,
    .set_first_interface = isoprobe_set_first_interface,
    .get_config_descriptor = isoprobe_get_config_descriptor,
    .init_connection = isoprobe_init_connection,
    .init = isoprobe_init,
    .disconnect = isoprobe_disconnect,
    .fast_transfer_complete = isoprobe_fast_complete,
    .control_request = isoprobe_control_request,
    .set_interface = isoprobe_set_interface,
    .get_interface = isoprobe_get_interface,
};
