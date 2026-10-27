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

/* host stub for tests/search and tests/otg: the trace goes to the test's
 * test_trace() (which keeps the lines it checks), and the list strip
 * otg.c borrows (test_otg.c) */
#ifndef STUB_ANIM_H
#define STUB_ANIM_H
#include <stdbool.h>
void stub_trace(const char *fmt, ...);  /* fake_tagcache.c (unused here) */
void test_trace(const char *fmt, ...);
#define UPDATE_TRACE(fmt, ...) test_trace(fmt, __VA_ARGS__)
#define UI_STRIP_SIZE 180000
void *ui_strip_claim(int owner, bool *kept);
void ui_strip_put(void);
int  ui_strip_new_owner(void);
#endif
