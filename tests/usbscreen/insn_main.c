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

/* Bare-metal ARM workloads for tests/usbscreen/insn.sh: dynamic
 * instruction counts of the USB screen's pixel work (usbscreen.c's pure
 * part, -mcpu=arm7tdmi -Os as in the firmware) under qemu-system-arm +
 * the risk tool's TCG plugin. WL 0 is the empty baseline. Not counted:
 * the header and the two text lines (Rockbox fonts), lcd_update_rect
 * (the LCD bus). memcpy is tests/npscreen/insn_libc.c's word copy, an
 * upper bound of the firmware's ldm/stm one. */
#include <stddef.h>
void *memcpy(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
#define USB_PURE_ONLY
#include "usbscreen.c"

#ifndef WL
#define WL 0
#endif

static usb_px fb[320 * 240];
static usb_px strip[USB_STRIP_BYTES / 2];
volatile int sink;

int main(void)
{
    int y;

    (void)y;
#if WL == 1     /* the entry compose: disc, idle, 3 maps, 6 frames */
    usb_compose_all(strip, 1);
#elif WL == 2   /* the static body: 218 dithered rows */
    for (y = USB_HDR_H; y < 240; y++)
        usb_body_row(fb + y * 320, y, 320);
#elif WL == 3   /* one arrow frame: 80 rows x 160 B into the framebuffer */
    for (y = 0; y < USB_N; y++)
        memcpy(fb + (USB_BOX_Y + y) * 320 + USB_BOX_X, strip + y * USB_N,
               USB_N * sizeof(usb_px));
#elif WL == 4   /* no strip: the idle disc drawn straight into the fb */
    usb_draw_disc(fb + USB_BOX_Y * 320 + USB_BOX_X, 320);
    usb_idle_glyph(fb + USB_BOX_Y * 320 + USB_BOX_X, 320, 1);
#elif WL == 5   /* one arrow coverage map (of the three) */
    usb_arrow_alpha(1, (unsigned char *)strip);
#endif
    sink = fb[0] + strip[0];
    return 0;
}
