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

/* Host stub: the event classes apps/appevents.h needs (buffering.h) */
#ifndef STUB_EVENTS_H
#define STUB_EVENTS_H
#define EVENT_CLASS_DISK       0x0100
#define EVENT_CLASS_PLAYBACK   0x0200
#define EVENT_CLASS_BUFFERING  0x0400
#define EVENT_CLASS_GUI        0x0800
#define EVENT_CLASS_RECORDING  0x1000
#define EVENT_CLASS_LCD        0x2000
#define EVENT_CLASS_VOICE      0x4000
#define EVENT_CLASS_SYSTEM     0x8000
#endif
