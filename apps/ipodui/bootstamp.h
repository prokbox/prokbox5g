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
 * iPod UI shell: the no-change boot stamp and the boot window
 * (Phase 16-2, #89 and #87)
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
#ifndef _IPODUI_BOOTSTAMP_H_
#define _IPODUI_BOOTSTAMP_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* docs/phase16-brief.md 3.2 and 4.2 (Revision 1, H1). A 32 B stamp
 * holds a digest of the music tree's directory entries and vouches for
 * the library as of the last completed walk (or a boot that trusted the
 * stamp before it). The next boot reads it and removes it at once (a
 * crash later in that boot finds none unless that boot writes its own),
 * and skips the library's probe walk only when the stamp is sound, the
 * active library is the one it names, fewer than
 * BOOT_STAMP_WALK_EVERY - 1 boots were skipped in a row, and a fresh
 * digest (directory reads only, no file opens) matches.
 *
 * Writes (tagcache.c): the remove at boot; the write once the library
 * has settled after a completed walk or a trusted boot, with the disk
 * already up (ui_boot_stamp_settle(), npfix), and at a clean shutdown or
 * a sleep entry; its removal at the start of any library walk, at the
 * first disk wake after a soft sleep, and (npfix H1) on the tagcache
 * thread at a USB connect, before its ack (a UI-thread write at USB
 * screen entry is still never made).
 *
 * Owner decision 2026-10-23 (npfix, supersedes P16 Q3's forced walk
 * after an unclean end): the settle write and the removal at USB connect
 * before the ack stay; a crash without a USB session no longer forces a
 * walk; the 10th-boot backstop stays.
 *
 * The rule that remains: a crash keeps the last stamp written, so the
 * next boot skips the walk exactly as after a clean shutdown when no
 * walk and no USB session began since that write. A change to the music
 * folders made while this firmware is not running (another firmware's
 * disk mode) is seen only through the directory digest, and the
 * BOOT_STAMP_WALK_EVERY-th boot walks regardless. */

#define BOOT_STAMP_MAGIC        0x54534250u  /* "PBST" */
#define BOOT_STAMP_VERSION      2u   /* P18: last_walk_rtc; a v1 stamp
                                         * reads as torn (one walk) */
#define BOOT_STAMP_CLEAN        0x0001u      /* flags: a clean shutdown */
#define BOOT_STAMP_WALK_EVERY   10           /* Q3: every 10th boot walks */
#define BOOT_DIGEST_CAP_MS      1000         /* slower: walk (never "clean and
                                              * no USB" alone, until the
                                              * device's 'boot digest ms='
                                              * line says otherwise) */
#define BOOT_HOLD_MS            3000         /* Q4: the boot screen's hold */

struct boot_stamp {            /* the hidden folder's boot_stamp.dat */
    uint32_t magic, version;
    uint32_t fsinfo_free;               /* FAT FSInfo at the write (log
                                         * only: our own later writes move
                                         * it, the digest is the detector) */
    uint32_t last_walk_rtc;             /* v2 (P18 Q11): the RTC (s since
                                         * 2000) of the last completed walk
                                         * this stamp vouches by; 0 none */
    uint32_t dir_digest;                /* boot_digest over the music tree */
    uint32_t db_generation;             /* the active library's commit id */
    uint16_t boots_skipped;             /* walks skipped in a row before it */
    uint16_t flags;                     /* BOOT_STAMP_CLEAN */
    uint32_t crc;                       /* boot_crc32 over the bytes above */
};

/* -- pure half (bootstamp_core.c; host test tests/bootstamp) ------------- */

/* Why a boot walks or not; only the first skips the walk */
enum boot_why {
    BOOT_WHY_TRUST = 0,     /* stamp sound and the digest matches */
    BOOT_WHY_SLOW,          /* digest over BOOT_DIGEST_CAP_MS: walk */
    BOOT_WHY_NOSTAMP,       /* none: first boot, an unclean end, a USB
                             * session or an unsettled library at shutdown */
    BOOT_WHY_TORN,          /* wrong size, magic, version or CRC */
    BOOT_WHY_UNCLEAN,       /* sound, but not marked clean */
    BOOT_WHY_TENTH,         /* Q3: the forced walk */
    BOOT_WHY_GEN,           /* another library generation is active */
    BOOT_WHY_DIGEST,        /* the tree changed (added, removed, renamed,
                             * resized, retimed or moved) */
    BOOT_WHY_DIGEST_FAIL,   /* the digest walk could not finish */
    BOOT_WHY_USB,           /* a USB session this boot before the check */
    BOOT_WHY_DB,            /* the library needs a scan anyway */
    BOOT_WHY_DAYS,          /* P18 Q11: SLEEP_WALK_DAYS since the last walk */
    BOOT_WHY__N
};

enum boot_digest_state {
    BOOT_DIGEST_MATCH = 0,
    BOOT_DIGEST_DIFF,
    BOOT_DIGEST_SLOW,       /* stopped at the cap */
    BOOT_DIGEST_FAIL,       /* an open failed, a path too long, an event */
};

/* A running digest over a depth-first listing: every entry's name,
 * directory bit, size, mtime and first cluster, and a marker at the end
 * of each directory (so a move between folders changes it too) */
struct boot_digest {
    uint32_t crc;
    long dirs, entries;
};

uint32_t boot_crc32(uint32_t crc, const void *p, size_t n);
void boot_stamp_seal(struct boot_stamp *s);
/* exactly sizeof(struct boot_stamp) bytes, magic, version and CRC */
bool boot_stamp_check(const void *buf, size_t len, struct boot_stamp *out);
/* the decision before the digest: BOOT_WHY_TRUST means "digest it" */
enum boot_why boot_stamp_precheck(const struct boot_stamp *s, bool have,
                                  uint32_t generation);
enum boot_why boot_stamp_verdict(enum boot_digest_state st);
static inline bool boot_why_trusted(enum boot_why w)
{
    return w == BOOT_WHY_TRUST;
}
/* H2: a boot walk for this reason also runs the deleted-file pass (the
 * walk itself never sees a file that is gone); DB has its own owed pass */
static inline bool boot_why_deleted_pass(enum boot_why w)
{
    return w != BOOT_WHY_TRUST && w != BOOT_WHY_DB && w < BOOT_WHY__N;
}
const char *boot_why_name(enum boot_why w);

void boot_digest_begin(struct boot_digest *d);
void boot_digest_entry(struct boot_digest *d, const char *name, bool dir,
                       uint32_t size, uint32_t mtime, uint32_t cluster);
void boot_digest_leave(struct boot_digest *d);  /* end of one directory */

/* -- device half (bootstamp.c) ------------------------------------------- */
#ifndef BOOTSTAMP_HOST_TEST

/* a struct dirent's first cluster: native FAT only (filesystem-native.h,
 * a ProkBox field); the simulator's host files have none */
#if defined(SIMULATOR) || defined(APPLICATION)
#define BOOT_DIRENT_CLUSTER(e) 0u
#else
#define BOOT_DIRENT_CLUSTER(e) ((uint32_t)(e)->info.fstclus)
#endif

/* tagcache_init(), before the tagcache thread starts: read the stamp and
 * remove it (spoil). A remove that fails truncates it instead. */
void boot_stamp_spoil(void);
/* the stamp read by boot_stamp_spoil(); false: none or unsound */
bool boot_stamp_saved(struct boot_stamp *out, enum boot_why *why);
/* The digest walk (tagcache thread, boosted by the caller): the roots in
 * order, the hidden folder and "._" sidecars left out, as the library walk
 * leaves them. path: the caller's buffer. stop(): an event wants the
 * thread (USB, power-off). */
enum boot_digest_state boot_stamp_digest(struct boot_digest *d,
                                         const char *const roots[],
                                         char *path, size_t pathsz,
                                         long deadline, bool (*stop)(void));
/* tagcache_shutdown(), after the statistics flush, only when the library
 * is settled and no USB session happened since the last completed walk
 * (the caller decides): write the stamp. false: not written. */
bool boot_stamp_write(uint32_t digest, uint32_t generation,
                      unsigned boots_skipped, uint32_t last_walk_rtc,
                      bool settle);     /* npfix: logged "stamp settle=" */
/* M1: the USB exit after a power-off that USB superseded (the stamp was
 * written): remove it before the post-USB scan */
void boot_stamp_remove(void);
/* npfix: the same, for 'why' (a walk starting over a settled stamp) */
void boot_stamp_drop(const char *why);
/* tagcache thread: this boot's decision (once), and each walk's start */
void boot_stamp_decided(enum boot_why why);
void boot_stamp_walk_started(void);

/* #87: the boot window. ipodui_main(), after the shell's reservations and
 * before the first draw: the boot screen stays up, boosted, until the
 * library's boot check is done, at most BOOT_HOLD_MS, handing over early
 * when a walk could not finish inside the hold, or at a key or USB. The
 * hooks for the panel-set rotation (#100) and the module snapshot batch
 * (#106) run here too. P18 (#86) calls it again at wake. */
void maint_boot_window(void);

#ifdef SIMULATOR
/* a crash or a power cut from here on: nothing more is written, so no
 * clean shutdown's stamp; a stamp already on disk (the settle's) stays,
 * as on the device */
void sim_boot_stamp_unclean(void);
/* test setup: the next boot finds no stamp (removed now, none written
 * after), as after a cut before anything settled */
void sim_boot_stamp_nostamp(void);
/* this boot's decision: -1 not yet, 0 skipped, 1 walked; walks: scans
 * run this boot; why: the reason's name */
int sim_boot_stamp_walk(int *walks, const char **why);
#endif
#endif /* BOOTSTAMP_HOST_TEST */

#endif /* _IPODUI_BOOTSTAMP_H_ */
