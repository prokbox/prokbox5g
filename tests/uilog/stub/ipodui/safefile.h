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

/* host stub for tests/uilog: the two calls uilog.c's rotation makes, over
 * the host file system (no shared chains there: aliases never exist) */
#ifndef STUB_UILOG_SAFEFILE_H
#define STUB_UILOG_SAFEFILE_H
#include <stdbool.h>
extern int access(const char *, int);
extern int unlink(const char *);
static inline bool file_exists(const char *p) { return access(p, 0) == 0; }
static inline int safefile_drop_alias(const char *p, const char *keep)
{ (void)p; (void)keep; return 0; }
static inline bool safefile_remove(const char *p, const char *keep)
{ (void)keep; return unlink(p) == 0 || !file_exists(p); }
#endif
