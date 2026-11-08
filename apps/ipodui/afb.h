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
 * iPod UI shell: the Apple fallback (#137): after a USB session, Apple
 * firmware 1.3's music database lists the user's Apple-playable songs
 * through shared-cluster links, never copies. Developer builds only,
 * behind one developer switch (default off; it expires at the owner's
 * device trial: folded into the default or removed). The job is part of
 * the MOD_XINDEX module (afb.c); resident here are the switch, the park
 * on the USB ack path, and the "links may be live" flag (afb_hook.c).
 * Design: docs/research/apple-fallback-links.md, itunes-mode.md.
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
#ifndef IPODUI_AFB_H
#define IPODUI_AFB_H

#include <stdbool.h>
#include "config.h"

/* our files, in the hidden folder */
#define AFB_DIR     ROCKBOX_DIR "/afb"
#define AFB_ON      AFB_DIR "/on"           /* the developer switch */
#define AFB_STATE   AFB_DIR "/state"        /* struct afb_stfile */
#define AFB_TABLE   AFB_DIR "/links.tbl"    /* afb_tbl_hdr + afb_rec... */

#ifdef HAVE_IPOD_UI_DEVMODE
/* the switch (read once, lazily: two file_exists()) */
bool afb_on(void);
/* Developer > Experiments: sets the switch and runs the job now (it
 * makes or drops the links); UI thread */
void afb_set(bool on);
/* the job has work: the switch is on, or a state is left to undo */
bool afb_wanted(void);
/* USB: before the UI thread's ack (the disk is still ours), the link
 * folder goes, so the host never sees a link. Reads only when there is
 * nothing to park. Logged, never flushed here. */
void afb_usb_park(void);
/* the job's end: links live (true) or none */
void afb_live(bool live);

struct xidx_job;
/* the job (MOD_XINDEX's ops->afb; when: enum afb_when). XIDX_JOB_* */
int afb_job(struct xidx_job *j, int when);
#else
static inline bool afb_wanted(void) { return false; }
static inline void afb_usb_park(void) {}
#endif

#endif /* IPODUI_AFB_H */
