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

/* The device defaults the shell must ship (#142), kept apart from the table
 * in settings_list.c so a host test can read them (tests/defaults). The
 * simulator keeps its own backlight default (0, Always On: the scripts
 * and screenshots depend on it). */
#ifndef SETTINGS_DEFAULTS_H
#define SETTINGS_DEFAULTS_H

/* Backlight timer, seconds (0 = Always On). The 7G default is 10 s; 15 s is
 * not in the shell's list (stock values only). */
#ifdef SIMULATOR
#define DEFAULT_BACKLIGHT_TIMEOUT 0
#elif defined(HAVE_IPOD_UI)
#define DEFAULT_BACKLIGHT_TIMEOUT 10
#else
#define DEFAULT_BACKLIGHT_TIMEOUT 15
#endif

/* Clicker: On out of the box, as the 7G (docs/design/changes-from-7g.md).
 * The shell's On is both halves: the audio click (any level > 0) and the
 * piezo/haptic line (keyclick_hardware); see screens/settings.c.
 * Owner-delegated default, reversible (#142). */
#ifdef HAVE_IPOD_UI
#define DEFAULT_KEYCLICK            2
#define DEFAULT_KEYCLICK_HARDWARE   true
#else
#define DEFAULT_KEYCLICK            0
#define DEFAULT_KEYCLICK_HARDWARE   false
#endif

#endif /* SETTINGS_DEFAULTS_H */
