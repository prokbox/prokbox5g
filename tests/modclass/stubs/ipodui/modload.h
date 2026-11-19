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

/* tests/modclass: the module loader's names the accountant uses */
#ifndef TEST_MODLOAD_H
#define TEST_MODLOAD_H
#include <stdint.h>
enum mod_id { MOD_NOTES = 1 };
enum { MOD_CLASS_A = 0, MOD_CLASS_B = 1 };
#define MOD_HDR_MAGIC 0x4d4f4448u
#define MOD_SLOT_DESC 0
struct mod_rt { int mclass; };
struct mod_desc { uint32_t magic; uint8_t pages; };
const struct mod_rt *mod_rt_get(enum mod_id id);
const char *mod_name(int id);
const void *mod_export_now(enum mod_id id, int slot);
#endif
