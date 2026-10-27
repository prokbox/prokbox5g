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

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
int parse(const uint8_t *d, uint32_t n);
uint32_t dec_ref(int, int); uint32_t dec_fast(int, int);
int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb"); static uint8_t buf[8 << 20];
    size_t n = fread(buf, 1, sizeof buf, f);
    int ky = atoi(argv[2]), kc = atoi(argv[3]), i;
    if (parse(buf, n)) { puts("parse"); return 1; }
    clock_t t0 = clock(); uint32_t a = 0, b = 0;
    for (i = 0; i < 5; i++) a = dec_ref(ky, kc);
    clock_t t1 = clock();
    for (i = 0; i < 5; i++) b = dec_fast(ky, kc);
    clock_t t2 = clock();
    printf("%s k %d/%d: ref %08x %.1f ms  fast %08x %.1f ms  %s  x%.2f\n", argv[1], ky, kc, a,
           (t1 - t0) * 200.0 / CLOCKS_PER_SEC, b, (t2 - t1) * 200.0 / CLOCKS_PER_SEC,
           a == b ? "SAME" : "DIFF", (double)(t1 - t0) / (t2 - t1));
    return a != b;
}
