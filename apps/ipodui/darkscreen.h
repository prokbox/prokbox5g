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
 * iPod UI shell
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
#ifndef IPODUI_DARKSCREEN_H
#define IPODUI_DARKSCREEN_H

/* The dark full-screen look (Volume Limit, Date and Time, About,
 * Brightness, Charging; Screen Lock later) lives in two places:
 *  - header.h: UI_HEADER_DARK. A screen sets s->header_w to it in
 *    enter(); the main loop (ui_main.c draw_dirty()) then draws the dark
 *    header with the screen's title, keeps it across passes and slides
 *    the whole screen when the header kind changes.
 *  - usbscreen.h: ui_dark_body() paints the body gradient (the USB
 *    screen's art) below UI_DARK_HDR_H; ui_dark_header() is the main
 *    loop's call.
 * This header only forwards to them. */
#include "ipodui/header.h"
#include "ipodui/usbscreen.h"

#endif /* IPODUI_DARKSCREEN_H */
