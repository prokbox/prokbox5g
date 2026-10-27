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

/* iPod UI shell: the sync-time indexer's driver (resident, xindex.c) and
 * the MOD_XINDEX module's export (screens/xindex.c). P12 W1-N,
 * docs/phase12-brief.md 3.5 and 8 rows 6, 10, 13. */
#ifndef IPODUI_XINDEX_H
#define IPODUI_XINDEX_H

#include <stdbool.h>
#include <stdint.h>
#include "rbpaths.h"
#include "ipodui/xindex_core.h"

/* In the hidden system folder (never named on screen) */
#define XNOTES_IDX      ROCKBOX_DIR "/notes.idx"
#define XNOTES_TMP      ROCKBOX_DIR "/notes.idx.tmp"
#define XNOTES_PREV     ROCKBOX_DIR "/notes.idx.old"
/* Contacts (W3-K): the same three, one index per source folder */
#define XCONTACTS_IDX   ROCKBOX_DIR "/contacts.idx"
#define XCONTACTS_TMP   ROCKBOX_DIR "/contacts.idx.tmp"
#define XCONTACTS_PREV  ROCKBOX_DIR "/contacts.idx.old"
/* W3-C: the Calendars index (xical_core.h) */
#define XCAL_IDX        ROCKBOX_DIR "/calendars.idx"
#define XCAL_TMP        ROCKBOX_DIR "/calendars.idx.tmp"
#define XCAL_PREV       ROCKBOX_DIR "/calendars.idx.old"
/* A build owed when the session ended: a boot the stamp trusts still
 * checks the folders (one lookup in the hidden folder, no walk) */
#define XIDX_OWE        ROCKBOX_DIR "/xindex.owe"

/* The sync moments (UI thread). XIDX_AT_USB: the post-USB moment's end
 * (usbbusy.c, #84); XIDX_AT_BOOT: the boot window (bootstamp.c, #87),
 * where a boot the P16 stamp trusts as clean skips the signature check
 * (nothing could have changed the folders). Stopped, or paused with
 * P16's window open: one maint_take(MAINT_XIDX, XIDX_BUILD_BLK) block,
 * the module pinned (mod_pin), the build on the storage worker
 * (storage_post_job; inline where there is none: the simulator and the
 * resident target build). Playing, or the take refused: owed (logged
 * "xidx owed"), the last good index stays. */
enum xidx_when { XIDX_AT_USB = 0, XIDX_AT_BOOT = 1 };
void xindex_sync(enum xidx_when when);
/* UI thread, the header's passes (usbbusy.c): an owed build runs at the
 * next stop or paused window, never under the Notes screens */
void xindex_poll(void);
/* L1: a soft-sleep wake (sleep.c): the month check runs at the next poll,
 * so the Calendars window rolls after a sleep across a month's end */
void xindex_wake(void);
/* Any thread: the job stops at its next entry (an alarm's ring wins,
 * docs/phase12-brief.md 8 row 10); it is owed again */
void xindex_cancel(void);
bool xindex_busy(void);
/* Bumped when a new index is published: an open Notes screen reloads
 * (identity is the header's sig, validity its crc) */
unsigned xindex_seq(void);

/* A paged list's storage policy (docs/storage-choices.md "Paged
 * lists"), resident so a Class A screen never calls the storage driver.
 * xindex_browse_keep(): an input (true) or a tick (false) of a paged
 * list whose index is not all in RAM: for XIDX_BROWSE_HOLD after the
 * last input, while the screen is lit, the drive's idle timer is reset
 * (storage_spin(): it never wakes a sleeping drive), so a page load
 * while browsing finds it awake on a hard drive as on flash.
 * True while the hold runs (the screen's tick comes back each second).
 * xindex_disk_warm(): a read now costs no spin-up or power-up (a
 * prefetch only runs then). */
#define XIDX_BROWSE_HOLD (30 * HZ)
bool xindex_browse_keep(bool input);
bool xindex_disk_warm(void);

/* -- the MOD_XINDEX module (export slot MOD_SLOT_FIRST) ------------------ */
struct xidx_job {
    uint8_t *blk;                       /* the take: XIDX_BUILD_BLK bytes */
    uint32_t blk_len;
    volatile bool *stop;                /* xindex_cancel() */
    /* out */
    int32_t  n;                         /* entries indexed */
    int32_t  bytes;                     /* the index file */
    uint8_t  cut;                       /* a cap left entries out */
    uint8_t  made_dir;                  /* /Notes created (Q12) */
};
enum { XIDX_JOB_BUILT = 0, XIDX_JOB_FRESH = 1, XIDX_JOB_FAIL = -1,
       XIDX_JOB_STOPPED = -2 };
struct xidx_ops {
    /* The Notes index: walks /Notes and its folders (one level), compares
     * the signature with notes.idx, rebuilds it when it differs or does
     * not check. Storage thread (or inline); XIDX_JOB_* */
    int (*notes)(struct xidx_job *j);
    /* The Contacts index (W3-K): the same job for /Contacts's .vcf files
     * (flat); the block is reused, its results (n, bytes, cut, made_dir)
     * are the Contacts', read by the driver between the two calls */
    int (*contacts)(struct xidx_job *j);
    /* W3-C: the Calendars index: walks /Calendars (one level, created if
     * missing), rebuilds calendars.idx when the signature (files, home
     * city, month) differs or it does not check. NULL: not built */
    int (*cal)(struct xidx_job *j);
};

#endif /* IPODUI_XINDEX_H */
