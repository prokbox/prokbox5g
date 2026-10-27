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

/* Tiny assertion helpers shared by the tests/npscreen host tests. */
#ifndef NP_TEST_CHECK_H
#define NP_TEST_CHECK_H
#include <stdio.h>

static int check_fails;
#define CHECK(c) do { if (!(c)) { check_fails++; \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
    } } while (0)
#define CHECK_EQ(a, b) do { long long _a = (long long)(a); \
    long long _b = (long long)(b); \
    if (_a != _b) { check_fails++; \
    fprintf(stderr, "%s:%d: %s == %lld, expected %s == %lld\n", __FILE__, \
            __LINE__, #a, _a, #b, _b); } } while (0)
#define CHECK_DONE(name) do { if (check_fails) { \
    fprintf(stderr, "%s: %d check(s) failed\n", name, check_fails); \
    return 1; } return 0; } while (0)
#endif
