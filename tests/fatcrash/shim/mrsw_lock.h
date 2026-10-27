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

#ifndef FATCRASH_MRSW_H
#define FATCRASH_MRSW_H
struct mrsw_lock { int x; };
static inline void mrsw_init(struct mrsw_lock *m) { (void)m; }
static inline void mrsw_read_acquire(struct mrsw_lock *m) { (void)m; }
static inline void mrsw_read_release(struct mrsw_lock *m) { (void)m; }
static inline void mrsw_write_acquire(struct mrsw_lock *m) { (void)m; }
static inline void mrsw_write_release(struct mrsw_lock *m) { (void)m; }
#endif
