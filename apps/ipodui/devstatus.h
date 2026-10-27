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

/* The Developer menu's Status screen, the bench verdicts its rows show,
 * the speed governor's switch and the bench session's own switches
 * (devmode menu re-layout, owner 2026-10-26; docs/design/devmode.md
 * section 3).  Developer builds only (HAVE_IPOD_UI_DEVMODE). */
#ifndef _IPODUI_DEVSTATUS_H_
#define _IPODUI_DEVSTATUS_H_

#include <stdbool.h>
#include <stddef.h>
#include "config.h"

#ifdef HAVE_IPOD_UI_DEVMODE
/* Developer > Status: one read-only list, what is on and off in plain
 * words; a row away from its default ends in "(changed)".  Reads the
 * newest bench results file of this build once, on entry. */
int devstatus_show(void);

/* The newest bench results file of this build, read once (Status and
 * Experiments call it on entry; never at boot, never while browsing) */
void devstatus_bench_load(void);
/* A step's last result in it, as LANG text ("bench: pass"), or NULL when
 * the step has none (no file, or the step didn't run) */
const char *devstatus_verdict(const char *step_id);
/* text + "  [bench: pass]" into buf when the step has a verdict */
char *devstatus_with_verdict(const char *text, const char *step_id,
                             char *buf, size_t len);

#if defined(HAVE_PP_GOV) && defined(HAVE_PLL_IDLE)
/* The speed governor's switch for the next start, from and to its marker
 * files (clockpol.h; clockpol_init() reads them unchanged) */
enum devgov { DEVGOV_OFF = 0, DEVGOV_STAGE1, DEVGOV_LOG, DEVGOV_APPLY,
              DEVGOV_N };
enum devgov devgov_get(void);
void devgov_set(enum devgov g);
/* apply would be refused at the next start: the UI on the 2nd core */
bool devgov_refused(void);
const char *devgov_name(enum devgov g);
#endif

#ifdef HAVE_BCMFW_DEVTEST
/* The bench session's switches (bcmjobs.on today): set at a new
 * session's start, held across its restarts, undone at its end.  What it
 * set is listed in a file in the bench folder, so a restart keeps it and
 * the undo is exact.  begin(): a new session only (never on resume); a
 * stale list from an earlier session is undone first.  Returns true when
 * the session must stop for one restart first (it said so on screen).
 * mode: enum bench_mode. */
bool devswitch_bench_begin(int mode);
void devswitch_bench_end(void);
/* Boot (developer builds): a list left by a session that is no longer
 * open (ended, or another build's) is undone, so a switch never leaks
 * into normal use.  One file_exists() when there is none. */
void devswitch_boot(void);
/* The session holds a switch now (Status) */
bool devswitch_held(void);
#endif
#endif /* HAVE_IPOD_UI_DEVMODE */

#endif /* _IPODUI_DEVSTATUS_H_ */
