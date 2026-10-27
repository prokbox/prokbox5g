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
 * iPod UI shell: the UDMA4 experiment on the flash adapter (Fit-1 C7,
 * docs/fit1-brief.md section A). Developer builds on flash units only
 * (HAVE_ATA_UDMA_FORCE); everything is a no-op elsewhere.
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
#ifndef _IPODUI_UDMATEST_H_
#define _IPODUI_UDMATEST_H_

#include <stdbool.h>
#include <stddef.h>

#ifdef HAVE_ATA_UDMA_FORCE
/* Boot (UI thread): forces UDMA4 if the developer switch is on */
void udma_boot(void);
/* Main loop: a fallback the driver made is persisted (the switch goes
 * off) and logged */
void udma_service(void);
/* The developer rows */
bool udma_is_on(void);
void udma_set(bool on);
/* The write-and-verify test (UI thread, playback stopped): writes a
 * scratch file in the system folder, reads it back at the negotiated
 * mode, then UDMA_TEST_PASSES times at UDMA4, compares every byte,
 * deletes it. Never touches anything else. Returns 0 pass, < 0 fail or
 * refused; the one-line result is in udma_test_result(). */
int udma_test(void);
const char *udma_test_result(void);
#else
static inline void udma_boot(void) {}
static inline void udma_service(void) {}
#endif

#endif /* _IPODUI_UDMATEST_H_ */
