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

/* Host-test stand-in for apps/ipodui/fe_be.h (P19): the UI/back-end
 * boundary is not under test here; every request runs inline. */
#ifndef FE_BE_H
#define FE_BE_H
#include <stdbool.h>
#include <stdint.h>
enum be_kind { BE_NOP = 0, BE_LOG_FLUSH = 6 };
static inline bool fe_be_route(void) { return false; }
static inline void fe_be_log_kick(void) {}
static inline bool fe_be_is_service_thread(void) { return true; }
static inline intptr_t be_call(enum be_kind k, uint32_t f, uint32_t *a, int n)
{ (void)k; (void)f; (void)a; (void)n; return 0; }
#endif
