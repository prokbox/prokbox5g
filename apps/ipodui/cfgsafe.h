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

#ifndef IPODUI_CFGSAFE_H
#define IPODUI_CFGSAFE_H

#include <stdbool.h>

/* The settings files' crash-safe save and boot recovery (settings.c,
 * HAVE_IPOD_UI; tests/fatcrash safetest sites cfg-*).
 *
 * Device, build 207956fb84 (2026-10-26): config.cfg, config.cfg.new and
 * config.cfg.old all 0 bytes. The stock scheme wrote .new with O_TRUNC
 * (FAT records the size only at close: a cut leaves 0 bytes), and the
 * next boot renamed whatever .new held over config.cfg, the good file
 * going to .old and, a boot later, removed. Three cut saves lost all.
 *
 * Here:
 *   save    the caller writes tmp completely, fsyncs and closes it;
 *           cfgsafe_commit() checks it is whole, then: remove old (only
 *           its name when it shares file's chain), rename file -> old
 *           (only when file is whole: an empty one is removed instead),
 *           rename tmp -> file. Nothing is truncated or removed before
 *           the new file is complete; a cut at any point leaves file
 *           whole, or file missing with old whole and tmp whole.
 *   boot    cfgsafe_recover(): a stock-scheme .new (legacy) that is whole
 *           is promoted as the stock boot would, an empty or torn one is
 *           removed, never promoted. Our tmp is never rolled forward
 *           (a cut before its rename loses that one change, not the
 *           file; safefile.h L1). Then, file missing or empty: old back
 *           in its place when whole.
 * "Whole": more than 0 bytes, at most CFGSAFE_MAX, the last byte '\n'
 * (every settings line ends "\r\n"; a cut file is 0 bytes on FAT). */
#define CFGSAFE_MAX     65536

/* recovery's report (a bit set) for the caller's log */
#define CFGSAFE_R_LEGACY    1u  /* a whole stock .new promoted */
#define CFGSAFE_R_DROPPED   2u  /* an empty or torn .new / tmp removed */
#define CFGSAFE_R_OLD       4u  /* file missing or empty: old put back */
#define CFGSAFE_R_EMPTY     8u  /* file empty, no whole old: defaults */

bool cfgsafe_valid(const char *path);
/* 0, or -1 with file untouched or (a cut) recoverable; tmp is removed
 * when it is not whole */
int cfgsafe_commit(const char *tmp, const char *file, const char *old);
unsigned cfgsafe_recover(const char *file, const char *tmp,
                         const char *legacy, const char *old);

#endif /* IPODUI_CFGSAFE_H */
