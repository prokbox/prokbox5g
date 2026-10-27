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

/* Host shim for tests/fatcrash: the subset of config.h that fat.c,
 * disk_cache.c and their headers read. Mirrors the shell build on the
 * iPod Video (FAT32 only, one volume, one drive, 512 B sectors). */
#ifndef FATCRASH_CONFIG_H
#define FATCRASH_CONFIG_H
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#define PLATFORM_NATIVE  (1 << 0)
#define PLATFORM_HOSTED  (1 << 1)
#define CONFIG_PLATFORM  PLATFORM_NATIVE
#define MEMORYSIZE       64
#define HAVE_IPOD_UI     1
#define CONFIG_RTC       1   /* the 5.5G has the PCF50605 RTC */
#define NUM_CORES        1
#define CONFIG_STORAGE   0
#define HAVE_STORAGE_FLUSH 1  /* fat.c's barrier calls storage_flush() */
#define CACHEALIGN_ATTR
#define CACHEALIGN_BITS  2
#define USED_ATTR
#define NORETURN_ATTR __attribute__((noreturn))
#define ATTRIBUTE_PRINTF(a,b) __attribute__((format(printf,a,b)))
#define INIT_ATTR
#define IF_MV(x...)
#define IF_MV_NONVOID(x...) void
#define IF_MV_VOL(x...) 0
#define IF_MD(x...)
#define IF_MD_NONVOID(x...) void
#define CHECK_VOL(x) 1
#define CHECK_DRV(x) 1
#define NUM_VOLUMES 1
#define NUM_DRIVES 1
#endif
#ifndef FATCRASH_CONFIG_TAIL
#define FATCRASH_CONFIG_TAIL
/* as firmware/export/config.h on a 32/64 MB iPod: fat.c then decodes into an
   aligned ucschar_t, not through a uint16_t* (a misaligned store) */
#define UNICODE32
#define ucschar_t unsigned int
#define NO_INLINE __attribute__((noinline))
#define YEAR 2026
#define MONTH 10
#define DAY 18
#endif
