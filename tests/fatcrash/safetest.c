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
/* tests/fatcrash/safetest: the safe replace (apps/ipodui/safefile.c, #91)
 * and every site's file sequence, over the firmware's own file layer
 * (file.c, file_internal.c, fileobj_mgr.c, dir.c over fat.c and
 * disk_cache.c) on a RAM FAT32 image.
 *
 *   safetest <img> [site...]
 *
 * For each site: a dry run counts its sector writes N; then for every
 * k = 0..N a power cut after write k (later writes never reach the
 * image), a reboot (the cache dropped, a fresh mount), the site's boot
 * recovery, and the check: every name once in its directory, the
 * destination holding the old or the new bytes exactly (or, where the
 * site allows it, absent with the complete new file as the only
 * residue), no temporary or prev left where recovery runs. Then a
 * victim-removal failure (the j-th directory-entry removal fails, as the
 * device's -338 EIO) at every remove of the sequence: the same check
 * after recovery, and a clean retry.
 *
 * After every cut and recovery, a chain check of the raw image (its own
 * FAT32 walker, not the firmware's): every entry's chain starts on an
 * allocated cluster and no cluster belongs to two chains. A recovery that
 * removes one of two names left on one chain (#91 H1) fails it. Strict:
 * any failed check fails the run. SAFETEST_SEEDS="1 2 ..." repeats every
 * site with random sectors preloaded into the disk cache first, so the
 * site's sectors take other cache slots (dc_commit_all() writes in slot
 * order).
 *
 * The site sequences restate the call sites (dbgen.c finish_publish,
 * dbjournal.c salvage/create_empty/compact_finish, otg.c idx/list/save,
 * playlist.c set_current/save control/save playlist, ab_store.c rewrite,
 * artfmt.c artfile_publish)
 * with their file names; the replace and recovery are the real code. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include "config.h"
#include "file.h"
#include "dir.h"
#include "fat.h"
#include "disk_cache.h"
#include "file_internal.h"
#include "fileobj_mgr.h"
#include "rb_namespace.h"
#include "../../apps/ipodui/safefile.h"
#include "../../apps/ipodui/cfgsafe.h"

#define SECTOR 512
static uint8_t *img, *base;
static size_t img_sectors;
static uint8_t *dirty;              /* sectors written since the base */
static size_t *dlist, dcount;
static long cut_at = -1, nwrites;

/* SAFETEST_REORDER: the drive's volatile write cache. A write lands in
 * the cache (later reads see it); storage_flush() lands every cached
 * write; at a power cut each cached sector lands or is lost on its own
 * (cut_variant: 0 all, 1 none, 2.. a seeded random subset), so only the
 * orders fat.c and file.c enforce with a flush (fat_barrier) hold */
static int reorder, cut_variant = -1;
static unsigned cut_seed;
static uint32_t *pidx;              /* sector -> cached slot + 1 */
static size_t *psec, pcount, pcap;
static uint8_t *pdata;
static unsigned long nflush;

static void land(size_t sec, const void *buf)
{
    if (!dirty[sec])
    {
        dirty[sec] = 1;
        dlist[dcount++] = sec;
    }
    memcpy(img + sec * SECTOR, buf, SECTOR);
}

static void stage(size_t sec, const void *buf)
{
    if (!pidx[sec])
    {
        if (pcount == pcap)
        {
            pcap = pcap ? pcap * 2 : 256;
            psec = realloc(psec, pcap * sizeof(*psec));
            pdata = realloc(pdata, pcap * SECTOR);
            if (!psec || !pdata) exit(2);
        }
        psec[pcount] = sec;
        pidx[sec] = (uint32_t)++pcount;
    }
    memcpy(pdata + (size_t)(pidx[sec] - 1) * SECTOR, buf, SECTOR);
}

/* the cached writes land: all of them, or (at a cut) the variant's set */
static void settle(int variant, unsigned seed)
{
    srand(seed);
    for (size_t i = 0; i < pcount; i++)
    {
        int lands = variant <= 0 ? 1 : variant == 1 ? 0 : rand() & 1;
        if (lands)
            land(psec[i], pdata + i * SECTOR);
        pidx[psec[i]] = 0;
    }
    pcount = 0;
}

int storage_read_sectors(unsigned long start, int count, void *buf)
{
    if (start + count > img_sectors) return -1;
    for (int i = 0; i < count; i++)
    {
        const uint8_t *src = reorder && pidx[start + i]
            ? pdata + (size_t)(pidx[start + i] - 1) * SECTOR
            : img + (start + i) * SECTOR;
        memcpy((uint8_t *)buf + (size_t)i * SECTOR, src, SECTOR);
    }
    return 0;
}

int storage_write_sectors(unsigned long start, int count, const void *buf)
{
    if (start + count > img_sectors) return -1;
    nwrites++;
    if (getenv("SAFETEST_WLOG"))
        printf("  write %ld: sector %lu x%d\n", nwrites, start, count);
    if (cut_at >= 0 && nwrites > cut_at)
        return 0;                   /* the power is gone: nothing lands */
    for (int i = 0; i < count; i++)
    {
        const uint8_t *b = (const uint8_t *)buf + (size_t)i * SECTOR;
        if (reorder)
            stage(start + i, b);
        else
            land(start + i, b);
    }
    return 0;
}

int storage_flush(void)
{
    nflush++;
    if (getenv("SAFETEST_WLOG"))
        printf("  flush after write %ld\n", nwrites);
    /* the power went with write cut_at: no later command completes (the
     * flush just after it landing everything is cut variant 0) */
    if (reorder && !(cut_at >= 0 && nwrites >= cut_at))
        settle(0, 0);
    return 0;
}

void panicf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(3);
}

unsigned char *utf8encode(unsigned long ucs, unsigned char *utf8)
{ *utf8++ = (unsigned char)(ucs < 0x80 ? ucs : '_'); return utf8; }
const unsigned char *utf8decode(const unsigned char *utf8, ucschar_t *ucs)
{ *ucs = *utf8; return utf8 + 1; }
unsigned long utf16len_utf8(const unsigned char *utf8)
{ return strlen((const char *)utf8); }
struct tm *get_time(void)
{
    static struct tm t;
    t.tm_year = 126; t.tm_mon = 9; t.tm_mday = 20; t.tm_hour = 13;
    return &t;
}
void dostime_localtime(time_t t, uint16_t *d, uint16_t *tm)
{ (void)t; *d = (uint16_t)((46 << 9) | (10 << 5) | 20); *tm = (uint16_t)(13 << 11); }
time_t dostime_mktime(uint16_t d, uint16_t t) { (void)d; (void)t; return 0; }

/* file_internal.c is built with -Dfat_remove=rt_fat_remove (run.sh): the
 * rt_fail_at-th directory-entry removal from now fails, as an I/O error
 * reading the victim's directory sector would */
static int rt_fail_at = -1, rt_dirent_calls;
int rt_fat_remove(struct fat_file *file, enum fat_remove_op what)
{
    if (what & FAT_RM_DIRENTRIES)
    {
        if (rt_dirent_calls++ == rt_fail_at)
            return -3;
    }
    return fat_remove(file, what);
}

/* safefile.c is built with -Drbfs_remove_alias=rt_no_alias for the
 * negative control (run.sh): recovery then removes aliases outright */
int rt_no_alias(const char *path, const char *keep);
int rt_no_alias(const char *path, const char *keep)
{ (void)path; (void)keep; return 0; }

/* SAFETEST_NETONLY (run.sh, file.c built -DRBFS_INTENT_NO_RESOLVE): the
 * boot leaves a cut rename's two names, so only the chain checks count:
 * remove() and O_TRUNC alone must never free a shared chain */
static int netonly;

/* ---- the chain check: the raw image, independent of fat.c ---------------- */
static uint32_t rsvd, spc, fatsz, nfats, rootclus, nclus;
static uint8_t *owned;

static uint32_t rd32(const uint8_t *p)
{ return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t fat_at(uint32_t c)
{ return rd32(img + rsvd * SECTOR + (size_t)c * 4) & 0x0fffffff; }
static uint8_t *clus(uint32_t c)
{ return img + (rsvd + nfats * fatsz + (size_t)(c - 2) * spc) * SECTOR; }

static int own_chain(uint32_t c, const char *what)
{
    for (uint32_t n = 0; c < 0x0ffffff8; n++)
    {
        if (c < 2 || c >= nclus + 2 || n > nclus)
        {
            printf("CHAIN %s: cluster %u out of range\n", what, c);
            return 1;
        }
        uint32_t next = fat_at(c);
        if (next == 0)
        {
            printf("CHAIN %s: %s free cluster %u\n", what,
                   n ? "runs into" : "starts with", c);
            return 1;
        }
        if (owned[c])
        {
            /* the safety net alone leaves a cut rename's two names: one
             * whole chain under both is tolerated there, nothing else */
            if (netonly && !n && owned[c] == 2)
                return 0;
            printf("CHAIN %s: cluster %u shared (cross-linked)\n", what, c);
            return 1;
        }
        owned[c] = n ? 1 : 2;
        c = next;
    }
    return 0;
}

static int walk_dir(uint32_t dclus, const char *dpath, int depth)
{
    int bad = 0;
    for (uint32_t c = dclus, n = 0; c >= 2 && c < 0x0ffffff8 && n <= nclus;
         c = fat_at(c), n++)
    {
        uint8_t *e = clus(c);
        for (uint32_t i = 0; i < spc * SECTOR / 32; i++, e += 32)
        {
            if (e[0] == 0)
                return bad;
            if (e[0] == 0xe5 || (e[11] & 0x3f) == 0x0f || e[0] == '.')
                continue;
            uint32_t start = (uint32_t)(e[20] | e[21] << 8) << 16
                           | (uint32_t)(e[26] | e[27] << 8);
            char what[300];
            snprintf(what, sizeof(what), "%s/%.11s", dpath, (char *)e);
            if (!start)
                continue;
            bad |= own_chain(start, what);
            if ((e[11] & 0x10) && !bad)
            {
                /* its ".." names this directory (0 for the root) */
                const uint8_t *dd = clus(start) + 32;
                uint32_t up = (uint32_t)(dd[20] | dd[21] << 8) << 16
                            | (uint32_t)(dd[26] | dd[27] << 8);
                if (memcmp(dd, "..         ", 11)
                    || up != (dclus == rootclus ? 0 : dclus))
                {
                    printf("DOTDOT %s: \"..\" names cluster %u, not %u\n",
                           what, up, dclus == rootclus ? 0 : dclus);
                    bad = 1;
                }
            }
            if ((e[11] & 0x10) && depth < 8 && !bad)
                bad |= walk_dir(start, what, depth + 1);
        }
    }
    return bad;
}

static int chain_check(const char *when)
{
    const uint8_t *b = img;
    rsvd = b[14] | b[15] << 8;
    spc = b[13];
    nfats = b[16];
    fatsz = rd32(b + 36);
    rootclus = rd32(b + 44);
    nclus = (uint32_t)((img_sectors - rsvd - nfats * fatsz) / spc);
    memset(owned, 0, nclus + 2);
    int bad = own_chain(rootclus, "/");
    if (!bad)
        bad = walk_dir(rootclus, "", 0);
    if (bad)
        printf("chain check failed %s\n", when);
    return bad;
}

/* the disk cache's slots taken by random sectors first (seeded) */
static unsigned preload_seed;
static void preload(void)
{
    if (!preload_seed)
        return;
    srand(preload_seed);
    for (int i = 0, n = rand() % 64; i < n; i++)
    {
        unsigned int flags;
        unsigned long sec = (unsigned long)rand() % img_sectors;
        dc_lock_cache();
        void *b = dc_cache_probe(sec, &flags);
        if (!flags)
            memcpy(b, img + sec * SECTOR, SECTOR);
        dc_unlock_cache();
    }
}

static int skip_boot;                /* a host's view: no boot recovery */
static void mount(void)
{
    filesystem_init();
    fat_init();
    if (fat_mount(0) != 0) { fprintf(stderr, "mount\n"); exit(2); }
    root_mount_path("/", NSITEM_HIDDEN);
    root_mount_path("/", NSITEM_CONTENTS);
    if (!skip_boot)
        rbfs_rename_intent_boot();  /* main.c: before any app opens a file */
}

static void powercut(void)          /* the cache is lost, not written */
{
    /* the drive's cache: all of it lands unless this is a cut (a clean
     * stop flushes it) */
    if (reorder)
        settle(cut_variant, cut_seed);
    root_unmount_volume();
    fileobj_mgr_unmount();
    fat_unmount();
}

static void restore(void)
{
    for (size_t i = 0; i < dcount; i++)
    {
        memcpy(img + dlist[i] * SECTOR, base + dlist[i] * SECTOR, SECTOR);
        dirty[dlist[i]] = 0;
    }
    dcount = 0;
}

/* sizes by fill: old 'a', the pair's other 'b', new 'N' */
static size_t size_of(int fill)
{ return fill == 'L' ? 70000 : fill == 'N' ? 5120 : fill == 'b' ? 3500
       : 4608; }

static void put(const char *path, int fill)
{
    static char buf[16384];
    size_t n = size_of(fill);
    int fd = rbfs_open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) { fprintf(stderr, "create %s: %d\n", path, errno); exit(2); }
    memset(buf, fill, sizeof(buf));
    for (size_t k; n; n -= k)
    {
        k = n < sizeof(buf) ? n : sizeof(buf);
        if (rbfs_write(fd, buf, k) != (ssize_t)k) exit(2);
    }
    rbfs_fsync(fd);
    if (rbfs_close(fd) < 0) exit(2);
}

/* what a file holds: its fill char when exactly one of ours, 0 when
 * missing, -1 when anything else (torn, mixed, wrong size) */
static int content(const char *path)
{
    char b[512];
    size_t got = 0;
    ssize_t k;
    int fill = -2;
    int fd = rbfs_open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    while ((k = rbfs_read(fd, b, sizeof(b))) > 0)
    {
        for (ssize_t i = 0; i < k; i++)
        {
            if (fill == -2)
                fill = (unsigned char)b[i];
            if (b[i] != fill)
                fill = -1;
        }
        got += (size_t)k;
    }
    rbfs_close(fd);
    if (fill < 0 || got != size_of(fill))
        return -1;
    return fill;
}

static int exists(const char *p) { return rbfs_file_exists(p); }

/* every name in a directory at most once */
static int dup_check(const char *dirpath)
{
    char names[256][64];
    int n = 0, bad = 0;
    DIR *d = rbfs_opendir(dirpath);
    struct dirent *e;
    if (!d) return 0;
    while ((e = rbfs_readdir(d)))
    {
        for (int i = 0; i < n; i++)
            if (!strcmp(names[i], e->d_name))
            {
                printf("DUPLICATE %s/%s\n", dirpath, e->d_name);
                bad = 1;
            }
        if (n < 256)
            snprintf(names[n++], sizeof(names[0]), "%s", e->d_name);
    }
    rbfs_closedir(d);
    return bad;
}

/* ---- the sites --------------------------------------------------------- */

#define RB   "/.rockbox"
#define DB   RB                      /* tagcache_db_path default */
#define OTGD RB "/otg"
#define ART  RB "/artcache"
#define PL   "/Playlists"
#define CTL  RB "/.playlist_control"

struct site {
    const char *name;
    void (*setup)(void);             /* the state before the replace */
    int  (*run)(void);               /* the sequence: 0 ok, -1 failed */
    void (*recover)(void);           /* its boot recovery */
    int  (*check)(int faulted);      /* after recovery: 0 ok */
    const char *dirs[3];
    void (*next)(void);              /* the site's next save (may be NULL) */
};



static int expect(const char *what, const char *path, const char *allowed)
{
    int c = content(path);
    char want = c == 0 ? '0' : c < 0 ? '?' : (char)c;
    if (!strchr(allowed, want))
    {
        printf("%s: %s holds '%c', allowed \"%s\"\n", what, path, want,
               allowed);
        return 1;
    }
    return 0;
}
static int absent(const char *what, const char *path)
{
    if (!exists(path)) return 0;
    printf("%s: %s left behind\n", what, path);
    return 1;
}

/* dbgen.c dbgen_finish_publish(): the stale slot goes, then the rename;
 * dbgen_recover(): the slot temporaries go */
#define M0  DB "/database_manifest0.dat"
#define M0T DB "/database_manifest0.tmp"
#define M1  DB "/database_manifest1.dat"
#define M1T DB "/database_manifest1.tmp"
static void gen_setup(void) { put(M0, 'a'); put(M1, 'b'); }
static int gen_run(void)
{
    put(M0T, 'N');
    if (rbfs_remove(M0) < 0 && errno != ENOENT) return -1;
    return rbfs_rename(M0T, M0);
}
static void gen_recover(void)
{ safefile_remove(M0T, M0); safefile_remove(M1T, M1); }
static int gen_check(int f)
{
    (void)f;   /* the other manifest is the recovery: always intact */
    return expect("gen", M1, "b") | expect("gen", M0, "aN0")
         | absent("gen", M0T);
}

/* dbjournal.c: salvage's merge onto the newest name (safefile_replace),
 * create_empty (no destination yet), compact_finish (the whole stage
 * under its own name, the inactive goes, then the rename, L4);
 * dbjournal_recover() and open's roll-forward */
#define JP  DB "/database_stats.jnl"
#define JV  DB "/database_stats.prev"
#define JT  DB "/database_stats.tmp"
#define JPO DB "/database_stats.jnl.old"
#define JVO DB "/database_stats.prev.old"
#define JPN DB "/database_stats.jnl.new"
#define JVN DB "/database_stats.prev.new"
/* dbjournal_open()'s settle_stage(): a stage that went whole under its own
 * name (here: holds 'N' exactly; there: newer than both checkpoints) takes
 * its checkpoint's name, any other goes */
static void jnl_settle(const char *stage, const char *dest)
{
    if (!exists(stage)) return;
    if (content(stage) == 'N')
    {
        if (safefile_remove(dest, stage)) rbfs_rename(stage, dest);
    }
    else
        safefile_remove(stage, dest);
}
static void jnl_recover(void)           /* dbjournal_recover(), open */
{
    int tmp_ok = 1;
    if (safefile_drop_alias(JT, JPN) < 0 || safefile_drop_alias(JT, JVN) < 0)
        tmp_ok = 0;
    safefile_drop_alias(JPN, JP);
    safefile_drop_alias(JVN, JV);
    safefile_recover(JP, NULL, JPO);
    if (safefile_drop_alias(JT, JP) < 0)
        tmp_ok = 0;
    safefile_recover(JV, tmp_ok ? JT : NULL, JVO);
    jnl_settle(JPN, JP);
    jnl_settle(JVN, JV);
}
static void merge_setup(void) { put(JP, 'a'); put(JV, 'b'); }
static int merge_run(void) { put(JT, 'N'); return safefile_replace(JT, JP, JPO); }
static int merge_check(int f)
{
    (void)f;
    return expect("merge", JP, "aN") | expect("merge", JV, "b")
         | absent("merge", JT) | absent("merge", JPO) | absent("merge", JVO);
}
static void create_setup(void) { }
static int create_run(void) { put(JT, 'N'); return safefile_replace(JT, JP, JPO); }
static int create_check(int f)
{
    (void)f;
    return expect("create", JP, "N0") | absent("create", JT)
         | absent("create", JPO);
}
static int compact_run(void)
{
    put(JT, 'N');           /* live is the primary: the inactive is .prev */
    if (!safefile_remove(JVN, JV) || rbfs_rename(JT, JVN) < 0
        || !safefile_remove(JV, JVN) || rbfs_rename(JVN, JV) < 0)
    {
        safefile_remove(JVN, JV);
        return -1;
    }
    return 0;
}
static int compact_check(int f)
{
    /* L4: a cut never loses the inactive checkpoint's place: the old one
     * or the whole stage (a failed removal: the old one, or none) */
    return expect("compact", JP, "a") | expect("compact", JV, f ? "bN0" : "bN")
         | absent("compact", JT) | absent("compact", JVN);
}

/* otg.c idx_write_all(), otg_remove(): safefile_replace; otg_save(): a
 * free name only; recovery at the first sync */
#define OI   OTGD "/otg.idx"
#define OIT  OTGD "/otg.itmp"
#define OIO  OTGD "/otg.iold"
#define OL   OTGD "/otg.m3u8"
#define OLT  OTGD "/otg.tmp"
#define OLO  OTGD "/otg.old"
#define OST  PL "/.otgsave.tmp"
#define OSP  PL "/New Playlist 2.m3u8"
static void otg_recover(void)
{
    safefile_recover(OL, OLT, OLO);
    safefile_recover(OI, OIT, OIO);
    safefile_remove_in(OST, PL);
}
static void otg_setup(void)
{
    put(OI, 'a'); put(OL, 'a');
    rbfs_mkdir(PL "/Road Trips");       /* a user's folder: M1 */
    put(PL "/New Playlist 1.m3u8", 'b');
}
static int idx_run(void) { put(OIT, 'N'); return safefile_replace(OIT, OI, OIO); }
static int idx_check(int f)
{
    (void)f;
    return expect("otg idx", OI, "aN") | expect("otg idx", OL, "a")
         | absent("otg idx", OIT) | absent("otg idx", OIO);
}
static int list_run(void) { put(OLT, 'N'); return safefile_replace(OLT, OL, OLO); }
static int list_check(int f)
{
    (void)f;
    return expect("otg list", OL, "aN") | expect("otg list", OI, "a")
         | absent("otg list", OLT) | absent("otg list", OLO);
}
static int save_run(void)
{
    safefile_remove_in(OST, PL);        /* before the O_TRUNC (M1) */
    put(OST, 'N');
    if (exists(OSP)) return -1;
    return rbfs_rename(OST, OSP);
}
static int save_check(int f)
{
    (void)f;
    return expect("otg save", OSP, "N0") | absent("otg save", OST)
         | expect("otg save", PL "/New Playlist 1.m3u8", "b");
}

/* playlist.c: playlist_set_current() (the working control file, .tmp,
 * takes the current one's place) and the save's control swap (_temp);
 * both: on failure no control file, as upstream; recovery at the top
 * of playlist_resume() */
#define CTLT CTL "_temp"
#define CTLW CTL ".tmp"
#define CTLP CTL "_prev"
static void ctl_setup(void) { put(CTL, 'a'); }
static void ctl_recover(void)   /* playlist_init(), playlist_resume() */
{
    safefile_remove(CTLW, CTL);
    safefile_recover(CTL, CTLT, CTLP);
}
static void ctl_failed(const char *tmp)  /* control_replace_failed() */
{
    if (safefile_remove(CTLP, CTL))
        safefile_remove(CTL, tmp);
}
static int setcur_run(void)
{
    put(CTLW, 'N');
    if (safefile_replace(CTLW, CTL, CTLP) < 0)
    {
        ctl_failed(CTLW);
        return -1;
    }
    return 0;
}
static int swap_run(void)
{
    put(CTLT, 'N');
    if (safefile_replace(CTLT, CTL, CTLP) < 0)
    {
        ctl_failed(CTLT);
        return -1;
    }
    return 0;
}
static int ctl_check(int f)
{
    /* a failed replace leaves no control file (upstream's meaning) */
    return expect("control", CTL, f ? "aN0" : "aN") | absent("control", CTLT)
         | absent("control", CTLP) | absent("control", CTLW);
}

/* playlist.c pl_save_playlist(): a residue temp goes (alias-aware), the
 * destination is recorded, the old file goes, then the rename and the
 * marker; playlist_init() finishes a cut save from the marker, and
 * /Playlists' stray temporaries are deleted by safefile_recover_dir(),
 * never rolled forward (L1); user folders are never scanned */
#define PLI  RB "/.playlist_save"
#define PLS  PL "/Road Trip.m3u8"
#define PLST PLS "_temp"
#define USR  "/Music/Road Trip.m3u8"
#define USRT USR "_temp"
static void pls_setup(void) { put(PLS, 'a'); put(USR, 'a'); put(PL "/Other.m3u", 'b'); }
static int pls_save(const char *dest, const char *tmp)
{
    if (!safefile_remove(tmp, dest)) return -1;
    put(tmp, 'N');
    if (!safefile_intent_set(PLI, dest, (long)size_of('N'))) return -1;
    if (rbfs_remove(dest) < 0 && exists(dest)) return -1;
    if (rbfs_rename(tmp, dest) < 0) return -1;
    rbfs_remove(PLI);
    return 0;
}
static void pls_recover(void)
{
    safefile_intent_recover(PLI, "_temp");
    safefile_recover_dir(PL, "_temp", PLI);
}
static int pls_run(void) { return pls_save(PLS, PLST); }
static int pls_check(int f)
{
    (void)f;
    return expect("save", PLS, "aN") | absent("save", PLST)
         | expect("save", PL "/Other.m3u", "b") | absent("save", PLI);
}
static int usr_run(void) { return pls_save(USR, USRT); }
static int usr_check(int f)
{
    /* the marker finishes a cut save: old or new. A temp cut before the
     * marker was written (or a failed removal, f) stays beside the old
     * file, on its own chain, for the next save to drop: user folders are
     * never scanned (M1) */
    (void)f;
    return expect("save (user folder)", USR, "aN")
         | (content(USR) == 'a' ? 0 : absent("save (user folder)", USRT))
         | absent("save (user folder)", PLI);
}

/* L1: leftovers of other saves. A temp in /Playlists with no marker is
 * stray and goes (never resurrected) and a user's playlist beside one is
 * untouched; a marker whose recorded size does not match its temp (not
 * that save's) never rolls the temp into a free name */
#define STRAY PL "/Gone.m3u_temp"
#define KEPT  PL "/Kept.m3u8"
static void stray_setup(void)
{
    int fd;
    put(STRAY, 'N');
    put(KEPT, 'a');
    put(KEPT "_temp", 'N');
    put(USRT, 'N');                 /* USR itself missing */
    fd = rbfs_open(PLI, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0 || rbfs_write(fd, USR "\n77\n", sizeof(USR) + 3)
                  != (ssize_t)sizeof(USR) + 3) exit(2);
    rbfs_close(fd);
}
static int stray_run(void) { return 0; }
static int stray_check(int f)
{
    (void)f;
    return absent("stray", STRAY) | absent("stray", PL "/Gone.m3u")
         | expect("stray", KEPT, "a") | absent("stray", KEPT "_temp")
         | absent("stray", USR) | absent("stray", PLI);
}

/* ab_store.c rewrite(): safefile_replace; open_recover() at load */
#define BD ART "/books.dat"
#define BT ART "/books.tmp"
#define BO ART "/books.old"
static void ab_setup(void) { put(BD, 'a'); }
static int ab_run(void)
{
    put(BT, 'N');
    if (safefile_replace(BT, BD, BO) < 0)
    {
        rbfs_remove(BT);
        return -1;
    }
    return 0;
}
static void ab_recover(void)
{
    if (!exists(BD))
        safefile_recover(BD, NULL, BO);
    if (exists(BD))
    {
        safefile_remove(BT, BD);
        safefile_remove(BO, BD);
    }
    else if (exists(BT))
        rbfs_rename(BT, BD);        /* before #91: tmp whole, dat removed */
}
static int ab_check(int f)
{
    (void)f;
    return expect("ab", BD, "aN") | absent("ab", BT) | absent("ab", BO);
}

/* artcache.c pool_build() / pool_save() (#100): a cover written IN PLACE
 * into a free slot of panelpool.pak (here an append: the file grows),
 * fsynced, then panelpool.idx replaced by safefile_replace(); recovery:
 * safefile_recover() once per boot. The index the boot finds names a
 * slot whose bytes are all on disk, and slot 0 (named by the old index)
 * is never touched. */
#define PK  ART "/panelpool.pak"
#define PI  ART "/panelpool.idx"
#define PIT ART "/panelpool.idx.tmp"
#define PIP ART "/panelpool.idx.prev"
#define PSLOT 4608                   /* size_of('a'): slot 0 is put() */
static void pool_setup(void) { put(PK, 'a'); put(PI, 'a'); }
static int pool_run(void)
{
    static char buf[PSLOT];
    int fd = rbfs_open(PK, O_WRONLY);
    if (fd < 0) return -1;
    memset(buf, 'N', sizeof(buf));
    if (rbfs_lseek(fd, PSLOT, SEEK_SET) != PSLOT
        || rbfs_write(fd, buf, PSLOT) != PSLOT || rbfs_fsync(fd) < 0)
    {
        rbfs_close(fd);
        return -1;
    }
    if (rbfs_close(fd) < 0) return -1;
    if (!safefile_remove(PIT, PI)) return -1;
    put(PIT, 'N');
    return safefile_replace(PIT, PI, PIP) < 0 ? -1 : 0;
}
static void pool_recover(void) { safefile_recover(PI, PIT, PIP); }
static int slot_is(int slot, int fill)
{
    char b[512];
    size_t left = PSLOT;
    int fd = rbfs_open(PK, O_RDONLY), ok = fd >= 0;
    if (ok && rbfs_lseek(fd, (off_t)slot * PSLOT, SEEK_SET)
              != (off_t)slot * PSLOT)
        ok = 0;
    while (ok && left)
    {
        ssize_t k = rbfs_read(fd, b, left < sizeof(b) ? left : sizeof(b));
        if (k <= 0) { ok = 0; break; }
        for (ssize_t i = 0; i < k; i++)
            if (b[i] != fill) ok = 0;
        left -= (size_t)k;
    }
    if (fd >= 0) rbfs_close(fd);
    return ok;
}
static int pool_check(int f)
{
    int c = content(PI);
    (void)f;
    if (c != 'a' && c != 'N')
    {
        printf("pool: %s holds '%c'\n", PI, c == 0 ? '0' : c < 0 ? '?' : c);
        return 1;
    }
    if (!slot_is(0, 'a'))
    {
        printf("pool: slot 0 (the old index's) changed\n");
        return 1;
    }
    if (c == 'N' && !slot_is(1, 'N'))
    {
        printf("pool: the new index names a slot not on disk\n");
        return 1;
    }
    return absent("pool", PIT);
}

/* artfmt.c artfile_publish() (index, panels, books.idx and every bundle),
 * its temp written after artfile_tmp_clear(); recovery: artfile_recover()
 * once per boot, then pass_begin()'s index recovery (a missing index is
 * prev's copy, else a complete temp's, by artfile_replace()) */
#define AI ART "/index.dat"
#define AT ART "/index.tmp"
#define AP ART "/index.prev"
static void art_setup(void) { put(AI, 'a'); put(AP, 'b'); }
static int art_replace(const char *tmp, const char *path)
{
    if (!safefile_remove(path, tmp)) return -1;
    return rbfs_rename(tmp, path);
}
static int art_run(void)
{
    int moved;
    if (!safefile_remove(AT, AI)) return -1;        /* artfile_tmp_clear */
    put(AT, 'N');
    if (exists(AI) && !safefile_remove(AP, AI)) return -1;
    moved = rbfs_rename(AI, AP) == 0;
    if (!moved && errno != ENOENT) return -1;
    if (rbfs_rename(AT, AI) == 0) return 0;
    if (moved) rbfs_rename(AP, AI);
    return -1;
}
static void art_recover(void)
{
    int c;
    safefile_drop_alias(AT, AI);                     /* artfile_recover */
    safefile_drop_alias(AP, AI);
    c = content(AI);
    if (c == 'a' || c == 'N') return;
    c = content(AP);
    if (c == 'a' || c == 'b' || c == 'N')
        art_replace(AP, AI);
    else if (content(AT) == 'N')
        art_replace(AT, AI);
}
static int art_check(int f)
{
    (void)f;
    return expect("art", AI, "aN");
}

/* uilog.c rotate() (this boot's roll of the logs): the live file and
 * .1 to .5 hold distinct contents; each moves one name up, highest first,
 * never onto a taken name; only the oldest (.5) is removed, once .4 is
 * there to replace it. Recovery is the next boot's rotation opening with
 * its alias pass. After a cut every generation a..f must still be there
 * exactly once, whole and in age order (g, the oldest, may be gone). */
#define LG  RB "/ipodui.log"
#define LG_KEEP 5
static const char lg_fill[LG_KEEP + 1] = { 'a', 'c', 'd', 'e', 'f', 'g' };
static void lg_name(char *o, int g)
{ if (g) sprintf(o, "%s.%d", LG, g); else strcpy(o, LG); }
static void lg_setup(void)
{
    char n[64];
    for (int g = 0; g <= LG_KEEP; g++) { lg_name(n, g); put(n, lg_fill[g]); }
}
static void lg_alias_pass(void)
{
    char f[64], t[64];
    for (int g = 0; g < LG_KEEP; g++)
    {
        lg_name(f, g); lg_name(t, g + 1);
        if (exists(f) && exists(t)) safefile_drop_alias(t, f);
    }
}
static int lg_run(void)
{
    char f[64], t[64];
    lg_alias_pass();
    for (int g = LG_KEEP - 1; g >= 0; g--)
    {
        lg_name(f, g); lg_name(t, g + 1);
        if (!exists(f)) continue;
        if (g + 1 == LG_KEEP && exists(t) && !safefile_remove(t, f)) continue;
        if (!exists(t) && rbfs_rename(f, t) < 0) return -1;
    }
    return 0;
}
static void lg_recover(void) { }
static int lg_check(int f)
{
    int bad = 0, seen[LG_KEEP + 1] = { 0 }, last = -1;
    char n[64];
    (void)f;
    lg_alias_pass();            /* the next boot's rotation, first step */
    for (int g = 0; g <= LG_KEEP; g++)
    {
        int c, i;
        lg_name(n, g);
        if (!exists(n)) continue;
        c = content(n);
        for (i = 0; i <= LG_KEEP && lg_fill[i] != c; i++) ;
        if (i > LG_KEEP) { printf("uilog: %s holds '%c'\n", n, c > 0 ? c : '?'); bad = 1; continue; }
        if (seen[i]++ || i < last) { printf("uilog: %s duplicate or out of order\n", n); bad = 1; }
        last = i;
    }
    for (int i = 0; i < LG_KEEP; i++)
        if (!seen[i]) { printf("uilog: generation '%c' lost\n", lg_fill[i]); bad = 1; }
    return bad;
}
static void lg_next(void)
{
    put(LG, 'a');               /* a new boot's file, then its rotation */
    lg_run();
}

/* artcache.c panel_save() -> artfile_publish() of panels.pak (tmp, fsync,
 * prev, rename: the art-publish sequence on the panels names) and
 * panel_commit(): the 792-byte header rewritten IN PLACE at offset 0 (two
 * 512-byte sectors), fsynced; a header cut between its sectors fails its
 * CRC and readers use panels.pak.prev (panel_load()). What a cut may leave:
 * the file's size and every byte past the header untouched, each header
 * sector whole-old or whole-new, prev intact. */
#define PP  ART "/panels.pak"
#define PPT ART "/panels.pak.tmp"
#define PPP ART "/panels.pak.prev"
#define PHDR 792
static void pp_setup(void) { put(PP, 'a'); put(PPP, 'b'); }
static int pp_run(void)
{
    int moved;
    if (!safefile_remove(PPT, PP)) return -1;
    put(PPT, 'N');
    if (exists(PP) && !safefile_remove(PPP, PP)) return -1;
    moved = rbfs_rename(PP, PPP) == 0;
    if (!moved && errno != ENOENT) return -1;
    if (rbfs_rename(PPT, PP) == 0) return 0;
    if (moved) rbfs_rename(PPP, PP);
    return -1;
}
static void pp_recover(void)
{
    int c;
    safefile_drop_alias(PPT, PP);
    safefile_drop_alias(PPP, PP);
    c = content(PP);
    if (c == 'a' || c == 'N') return;
    c = content(PPP);
    if (c == 'a' || c == 'b' || c == 'N') art_replace(PPP, PP);
    else if (content(PPT) == 'N') art_replace(PPT, PP);
}
static int pp_check(int f) { (void)f; return expect("panels", PP, "aN"); }
static int ph_run(void)
{
    char buf[PHDR];
    int fd = rbfs_open(PP, O_WRONLY);
    if (fd < 0) return -1;
    memset(buf, 'N', sizeof(buf));
    if (rbfs_write(fd, buf, PHDR) != PHDR || rbfs_fsync(fd) < 0)
    { rbfs_close(fd); return -1; }
    return rbfs_close(fd) < 0 ? -1 : 0;
}
static void ph_recover(void) { }
static int ph_check(int f)
{
    char b[512];
    int fd = rbfs_open(PP, O_RDONLY), bad = 0;
    off_t off = 0;
    ssize_t k;
    (void)f;
    if (fd < 0) { printf("panels: unreadable\n"); return 1; }
    while ((k = rbfs_read(fd, b, sizeof(b))) > 0)
    {
        for (ssize_t i = 0; i < k; i++)
        {
            off_t o = off + i;
            int sec_lo = o < 512, in_hdr = o < PHDR;
            /* sector 0 and the header's tail in sector 1: each uniform,
             * a or N; the body stays a */
            static char first[2];
            if (o == 0 || o == 512) first[o == 512] = b[i];
            if (in_hdr)
            {
                if (b[i] != 'a' && b[i] != 'N') bad = 1;
                if (b[i] != first[!sec_lo]) bad = 1;
            }
            else if (b[i] != 'a') bad = 1;
        }
        off += k;
    }
    rbfs_close(fd);
    if (bad || off != (off_t)size_of('a'))
    { printf("panels: header/body torn (size %ld)\n", (long)off); return 1; }
    return expect("panels", PPP, "b");
}

/* ---- plain renames (#91 re-review): sites with no recovery of their
 * own. The rename intent record (file.c) is resolved at every boot
 * (mount() above); then the site's own next boot step and next save, and
 * the chain check after each. Without the record (the nointent control)
 * a later remove() or O_TRUNC of one of two names frees the other's
 * chain. */

/* settings.c rename_temp_file() in settings_load(): .new -> the file, the
 * file -> .old (removed first in the shell build); the next boot repeats
 * it; the next save writes .new (O_TRUNC) for the boot after */
#define SC  RB "/.resume.cfg"
#define SCN SC ".new"
#define SCO SC ".old"
static void rename_temp_file(const char *tmp, const char *file,
                             const char *old)
{
    if (exists(tmp))
    {
        if (old && exists(file))
        {
            rbfs_remove(old);
            rbfs_rename(file, old);
        }
        rbfs_rename(tmp, file);
    }
}
static void cfg_setup(void) { put(SC, 'a'); put(SCO, 'b'); put(SCN, 'N'); }
static int cfg_run(void) { rename_temp_file(SCN, SC, SCO); return 0; }
static void cfg_recover(void) { rename_temp_file(SCN, SC, SCO); }
static int cfg_check(int f)
{
    /* a failed victim removal (f): settings ignore errors, as upstream */
    return expect("settings", SC, "N") | expect("settings", SCO, f ? "ab" : "a")
         | absent("settings", SCN);
}
static void cfg_next(void)
{
    put(SCN, 'b');
    rename_temp_file(SCN, SC, SCO);
}

/* cfgsafe (apps/ipodui/cfgsafe.c, settings.c in the shell build): the
 * save writes config.cfg.tmp whole (O_TRUNC, fsync, close) and
 * cfgsafe_commit() renames it over config.cfg, the old file to .old; the
 * boot's cfgsafe_recover() promotes a whole stock .new, drops an empty
 * or torn one and our tmp, and puts .old back over a missing or empty
 * file. The settings must never be lost: config.cfg holds the old or
 * the new settings exactly after every cut. A settings file's bytes:
 * fill, then '\n' (whole only with the final newline). */
#define CF   RB "/config.cfg"
#define CFT  CF ".tmp"
#define CFN  CF ".new"
#define CFO  CF ".old"
static void cput(const char *path, int fill)
{
    static char buf[16384];
    size_t n = size_of(fill);
    int fd = rbfs_open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) { fprintf(stderr, "create %s: %d\n", path, errno); exit(2); }
    memset(buf, fill, sizeof(buf));
    buf[(n - 1) % sizeof(buf)] = '\n';
    for (size_t k, at = 0; at < n; at += k)
    {
        k = n - at < sizeof(buf) ? n - at : sizeof(buf);
        if (rbfs_write(fd, buf, k) != (ssize_t)k) exit(2);
    }
    rbfs_fsync(fd);
    if (rbfs_close(fd) < 0) exit(2);
}
static void cempty(const char *path)   /* the device's 0-byte files */
{
    int fd = rbfs_open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0 || rbfs_close(fd) < 0) exit(2);
}
/* 0 missing, 'E' empty, the fill when exactly one of ours, -1 else */
static int ccontent(const char *path)
{
    char b[512];
    size_t got = 0;
    ssize_t k;
    int fill = -2, last = 0;
    int fd = rbfs_open(path, O_RDONLY);
    if (fd < 0)
        return 0;
    while ((k = rbfs_read(fd, b, sizeof(b))) > 0)
    {
        for (ssize_t i = 0; i < k; i++)
        {
            if (last == '\n')
                fill = -1;              /* bytes after the newline */
            last = (unsigned char)b[i];
            if (last == '\n')
                continue;
            if (fill == -2)
                fill = last;
            if (last != fill)
                fill = -1;
        }
        got += (size_t)k;
    }
    rbfs_close(fd);
    if (!got)
        return 'E';
    if (fill < 0 || last != '\n' || got != size_of(fill))
        return -1;
    return fill;
}
static int cexpect(const char *what, const char *path, const char *allowed)
{
    int c = ccontent(path);
    char want = c == 0 ? '0' : c < 0 ? '?' : (char)c;
    if (!strchr(allowed, want))
    {
        printf("%s: %s holds '%c', allowed \"%s\"\n", what, path, want,
               allowed);
        return 1;
    }
    return 0;
}
static void cfgs_recover(void) { cfgsafe_recover(CF, CFT, CFN, CFO); }
static int cfgs_save(int fill)          /* settings_write_safe() */
{
    cput(CFT, fill);
    return cfgsafe_commit(CFT, CF, CFO);
}
/* a save over a good file (and a good .old) */
static void cfgs_setup(void) { cput(CF, 'a'); cput(CFO, 'b'); }
static int cfgs_run(void) { return cfgs_save('N'); }
static int cfgs_check(int f)
{
    (void)f;
    return cexpect("cfg-save", CF, "aN") | absent("cfg-save", CFT);
}
static void cfgs_next(void) { cfgs_save('b'); }
/* the first save of a unit: no file yet. A cut may leave none (the
 * defaults, as before the save), never an empty or torn one */
static void cfgf_setup(void) { }
static int cfgf_check(int f)
{
    (void)f;
    return cexpect("cfg-first", CF, "0N") | absent("cfg-first", CFT);
}
/* the boot over the device's state (2026-10-26): the last good file, an
 * empty stock .new from a cut save; the stock boot made the empty one
 * config.cfg */
static void cfgl_setup(void) { cput(CF, 'a'); cput(CFO, 'b'); cempty(CFN); }
static int cfgl_run(void) { cfgs_recover(); return 0; }
static int cfgl_check(int f)
{
    (void)f;
    return cexpect("cfg-legacy", CF, "a") | absent("cfg-legacy", CFN);
}
/* a whole stock .new (an orderly save of the previous build): promoted,
 * as the stock boot would */
static void cfgn_setup(void) { cput(CF, 'a'); cput(CFN, 'N'); }
static int cfgn_check(int f)
{
    return cexpect("cfg-legacy-new", CF, f ? "aN" : "N")
         | absent("cfg-legacy-new", CFN);
}
/* config.cfg itself empty (what the stock boot left), .old whole: back */
static void cfge_setup(void) { cempty(CF); cempty(CFN); cput(CFO, 'a'); }
static int cfge_check(int f)
{
    (void)f;
    return cexpect("cfg-empty", CF, "a") | absent("cfg-empty", CFN);
}
/* all three empty (the device): nothing to restore; the boot leaves no
 * empty file to be promoted later, and the next save is whole */
static void cfgz_setup(void) { cempty(CF); cempty(CFN); cempty(CFO); }
static int cfgz_check(int f)
{
    (void)f;
    return cexpect("cfg-all-empty", CF, "0E") | absent("cfg-all-empty", CFN);
}

/* write a temp, remove the file, rename: tagcache.c's changelog export
 * (.new), bookmark.c, batcal.c put_file(), vix_file.c; the next export
 * truncates the temp (O_TRUNC) and does it again */
#define CL  DB "/changelog.txt"
#define CLN CL ".new"
static void cl_setup(void) { put(CL, 'a'); }
static int cl_run(void)
{
    put(CLN, 'N');
    rbfs_remove(CL);
    return rbfs_rename(CLN, CL);
}
static void cl_recover(void) { }
static int cl_check(int f)
{
    (void)f;
    return expect("changelog", CL, "aN0")
         | (content(CL) == 'N' ? absent("changelog", CLN) : 0);
}
static void cl_next(void)
{
    put(CLN, 'b');
    rbfs_remove(CL);
    rbfs_rename(CLN, CL);
}

/* fileop.c: the user moves a file into another folder; next, the user
 * deletes it where the browser lists it first and copies a new file in */
#define MV  "/Music/Song.mp3"
#define MVD "/Music/Sub/Song.mp3"
static void mvf_setup(void) { rbfs_mkdir("/Music/Sub"); put(MV, 'a'); }
static int mvf_run(void) { return rbfs_rename(MV, MVD); }
static int mvf_check(int f)
{
    int a = content(MV), b = content(MVD);
    (void)f;
    if ((a == 'a') + (b == 'a') != 1 || (a && a != 'a') || (b && b != 'a'))
    {
        printf("move: old '%c' new '%c', want the file under one name\n",
               a > 0 ? a : '0', b > 0 ? b : '0');
        return 1;
    }
    return 0;
}
static void mvf_next(void)
{
    rbfs_remove(exists(MV) ? MV : MVD);
    put("/Music/Sub/Next.mp3", 'N');
}

/* fileop.c: the user moves a folder into another folder; next, a
 * duplicate listing is emptied and removed, a file added to the album */
#define AD  "/Music/Album"
#define ADD "/Other/Album"
static int is_dir(const char *p)
{
    DIR *d = rbfs_opendir(p);
    if (d) rbfs_closedir(d);
    return d != NULL;
}
static void mvd_setup(void)
{
    rbfs_mkdir("/Other");
    rbfs_mkdir(AD);
    put(AD "/t1.mp3", 'a');
    put(AD "/t2.mp3", 'b');
}
static int mvd_run(void) { return rbfs_rename(AD, ADD); }
static int mvd_check(int f)
{
    int a = is_dir(AD), b = is_dir(ADD);
    const char *d = b ? ADD : AD;
    char p1[64], p2[64];
    (void)f;
    if (a + b != 1)
    {
        printf("dir move: old %d new %d, want one\n", a, b);
        return 1;
    }
    snprintf(p1, sizeof(p1), "%s/t1.mp3", d);
    snprintf(p2, sizeof(p2), "%s/t2.mp3", d);
    return expect("dir move", p1, "a") | expect("dir move", p2, "b");
}
static void mvd_next(void)
{
    if (is_dir(AD) && is_dir(ADD))
    {
        rbfs_remove(AD "/t1.mp3");
        rbfs_remove(AD "/t2.mp3");
        rbfs_rmdir(AD);
    }
    put(is_dir(ADD) ? ADD "/t3.mp3" : AD "/t3.mp3", 'N');
}

/* fileop.c paste with overwrite: a rename onto an existing name (the
 * victim goes first, file.c); next, the source name is removed if it
 * is still listed and the destination rewritten (O_TRUNC) */
#define OW  RB "/ow.dat"
#define OWS RB "/ow.src"
static void ow_setup(void) { put(OW, 'b'); put(OWS, 'a'); }
static int ow_run(void) { return rbfs_rename(OWS, OW); }
static int ow_check(int f)
{
    int d = content(OW), src = content(OWS);
    (void)f;
    if ((d == 'a' && src == 0) || (d == 'b' && src == 'a')
        || (d == 0 && src == 'a'))
        return 0;
    printf("overwrite: dest '%c' src '%c'\n", d > 0 ? d : '0',
           src > 0 ? src : '0');
    return 1;
}
static void ow_next(void)
{
    if (exists(OWS)) rbfs_remove(OWS);
    put(OW, 'N');
}

/* #91 H1: the same, with a free gap earlier in the directory, so the new
 * name lands in another sector than the victim's cleared entry: a drive
 * cache that lands the new name but loses the victim's clear would leave
 * two entries named ow2.dat (file.c's barrier after the victim) */
#define OW2  RB "/ow2.dat"
#define OW2S RB "/ow2.src"
static void ow2_setup(void)
{
    char p[64];
    /* the gap's two slots inside one sector (no long-name barrier) */
    put(RB "/ow2pre.dat", 'b');
    put(RB "/ow2gap.dat", 'b');
    for (int i = 0; i < 10; i++)
    {
        snprintf(p, sizeof(p), RB "/ow2fill%02d.dat", i);
        put(p, 'b');
    }
    put(OW2, 'b');
    put(OW2S, 'a');
    rbfs_remove(RB "/ow2gap.dat");
}
static int ow2_run(void) { return rbfs_rename(OW2S, OW2); }
static int ow2_check(int f)
{
    int d = content(OW2), src = content(OW2S);
    (void)f;
    if ((d == 'a' && src == 0) || (d == 'b' && src == 'a')
        || (d == 0 && src == 'a'))
        return 0;
    printf("overwrite-far: dest '%c' src '%c'\n", d > 0 ? d : '0',
           src > 0 ? src : '0');
    return 1;
}
static void ow2_next(void)
{
    if (exists(OW2S)) rbfs_remove(OW2S);
    put(OW2, 'N');
}

/* M2: a directory grows into a cluster that held other bytes (here:
 * entries named JUNKnnnn.DAT in every free cluster); a cut must never
 * make them part of the directory (fat.c zeroes before it links) */
#define GR "/Grow"
static void grow_setup(void)
{
    rbfs_mkdir(GR);
    powercut();
    chain_check("before the junk");     /* the geometry */
    for (uint32_t c = 2; c < nclus + 2; c++)
    {
        if (fat_at(c))
            continue;
        uint8_t *e = clus(c);
        for (uint32_t i = 0; i < spc * SECTOR / 32; i++, e += 32)
        {
            char n[12];
            snprintf(n, sizeof(n), "JUNK%04uDAT", (c * 7 + i) % 10000);
            memset(e, 0, 32);
            memcpy(e, n, 11);
            e[11] = 0x20;
        }
    }
    mount();
}
static int grow_run(void)
{
    for (int i = 0; i < 24; i++)
    {
        char p[32];
        snprintf(p, sizeof(p), GR "/g%02d.dat", i);
        int fd = rbfs_open(p, O_WRONLY | O_CREAT);
        if (fd < 0)
            return -1;
        rbfs_close(fd);
    }
    return 0;
}
static int grow_check(int f)
{
    DIR *d = rbfs_opendir(GR);
    struct dirent *e;
    int bad = 0;
    (void)f;
    while (d && (e = rbfs_readdir(d)))
        if (e->d_name[0] != '.'
            && (strncmp(e->d_name, "g", 1) || strlen(e->d_name) != 7))
        {
            printf("grow: %s/%s is not ours\n", GR, e->d_name);
            bad = 1;
            break;
        }
    if (d) rbfs_closedir(d);
    return bad;
}

/* #91 L: the record after a USB session (removed or truncated by the
 * host: made again), and a folder move cut after its record whose new
 * name the host then gave to a folder of its own: the boot never moves
 * the old folder into it or replaces it */
static off_t fsize(const char *p)
{
    int fd = rbfs_open(p, O_RDONLY);
    off_t n = fd >= 0 ? rbfs_lseek(fd, 0, SEEK_END) : -1;
    if (fd >= 0) rbfs_close(fd);
    return n;
}
static int intent_l_checks(void)
{
    int bad = 0, fd;
    restore();
    mount();
    rbfs_remove(RB "/.rename_intent");
    rbfs_rename_intent_usb_exit();
    if (fsize(RB "/.rename_intent") != SECTOR)
    {
        printf("intent-l: record not made again after the host removed it\n");
        bad = 1;
    }
    fd = rbfs_open(RB "/.rename_intent", O_WRONLY | O_TRUNC);
    if (fd >= 0) rbfs_close(fd);
    rbfs_rename_intent_usb_exit();
    if (fsize(RB "/.rename_intent") != SECTOR)
    {
        printf("intent-l: record not made again after the host truncated it\n");
        bad = 1;
    }
    mvd_setup();
    powercut();
    mount();
    cut_at = 1;                     /* the record lands, nothing else */
    nwrites = 0;
    rbfs_rename(AD, ADD);
    cut_at = -1;
    powercut();
    skip_boot = 1;                  /* the host's session */
    mount();
    if (!is_dir(AD) || is_dir(ADD))
    {
        printf("intent-l: the cut move is not where the test needs it\n");
        bad = 1;
    }
    rbfs_mkdir(ADD);                /* empty: a rename could replace it */
    powercut();
    skip_boot = 0;
    mount();                        /* the record is resolved */
    if (!is_dir(AD) || !is_dir(ADD) || exists(ADD "/t1.mp3")
        || !exists(AD "/t1.mp3"))
    {
        printf("intent-l: the boot moved a folder into the host's one\n");
        bad = 1;
    }
    bad |= dup_check("/Music") | dup_check("/Other");
    powercut();
    bad |= chain_check("after the host's folder and the boot");
    printf("intent-l: %s\n", bad ? "FAIL" : "ok");
    return bad;
}

/* #91 M1: a file truncated to empty (ftruncate, then close), and an
 * abandoned O_TRUNC temp (the next export opens the last one's leftover
 * and gives up before writing; over O_TRUNC_THRESH, so the chain is cut
 * at open). Closing at size 0 frees the first cluster: the entry must
 * stop naming it first, or a cut leaves the entry on a free cluster.
 * A cut after the trailing clusters are freed leaves the old size on a
 * shorter chain ('?': torn, fsck trims it), as any cut shrink does; the
 * chain check is what must hold */
#define TE  RB "/resume.dat"
static int empty_or(const char *what, const char *p, const char *allowed)
{
    return fsize(p) == 0 ? 0 : expect(what, p, allowed);
}
static void te_setup(void) { put(TE, 'a'); }
static int te_run(void)
{
    int fd = rbfs_open(TE, O_WRONLY);
    if (fd < 0)
        return -1;
    int rc = rbfs_ftruncate(fd, 0);
    if (rbfs_close(fd) < 0)
        rc = -1;
    return rc < 0 ? -1 : 0;
}
static int te_check(int f) { (void)f; return empty_or("truncate", TE, "a?"); }
static void te_next(void) { put(TE, 'b'); }
static void ot_setup(void) { put(CL, 'a'); put(CLN, 'L'); }
static int ot_run(void)
{
    int fd = rbfs_open(CLN, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return -1;
    return rbfs_close(fd) < 0 ? -1 : 0;
}
static int ot_check(int f)
{
    (void)f;
    return empty_or("o_trunc", CLN, "L?") | expect("o_trunc", CL, "a");
}

static const struct site sites[] = {
    { "trunc-empty",  te_setup,     te_run,      cl_recover,  te_check,     { RB }, te_next },
    { "otrunc-temp",  ot_setup,     ot_run,      cl_recover,  ot_check,     { DB }, cl_next },
    { "dir-grow",     grow_setup,   grow_run,    cl_recover,  grow_check,   { GR } },
    { "settings",     cfg_setup,    cfg_run,     cfg_recover, cfg_check,    { RB }, cfg_next },
    { "cfg-save",     cfgs_setup,   cfgs_run,    cfgs_recover, cfgs_check,  { RB }, cfgs_next },
    { "cfg-first",    cfgf_setup,   cfgs_run,    cfgs_recover, cfgf_check,  { RB }, cfgs_next },
    { "cfg-legacy",   cfgl_setup,   cfgl_run,    cfgs_recover, cfgl_check,  { RB }, cfgs_next },
    { "cfg-legacy-new", cfgn_setup, cfgl_run,    cfgs_recover, cfgn_check,  { RB }, cfgs_next },
    { "cfg-empty",    cfge_setup,   cfgl_run,    cfgs_recover, cfge_check,  { RB }, cfgs_next },
    { "cfg-all-empty",cfgz_setup,   cfgl_run,    cfgs_recover, cfgz_check,  { RB }, cfgs_next },
    { "changelog",    cl_setup,     cl_run,      cl_recover,  cl_check,     { DB }, cl_next },
    { "move-file",    mvf_setup,    mvf_run,     cl_recover,  mvf_check,    { "/Music", "/Music/Sub" }, mvf_next },
    { "move-dir",     mvd_setup,    mvd_run,     cl_recover,  mvd_check,    { "/Music", "/Other" }, mvd_next },
    { "overwrite",    ow_setup,     ow_run,      cl_recover,  ow_check,     { RB }, ow_next },
    { "overwrite-far",ow2_setup,    ow2_run,     cl_recover,  ow2_check,    { RB }, ow2_next },
    { "dbgen",        gen_setup,    gen_run,     gen_recover, gen_check,    { DB } },
    { "jnl-merge",    merge_setup,  merge_run,   jnl_recover, merge_check,  { DB } },
    { "jnl-create",   create_setup, create_run,  jnl_recover, create_check, { DB } },
    { "jnl-compact",  merge_setup,  compact_run, jnl_recover, compact_check,{ DB } },
    { "otg-idx",      otg_setup,    idx_run,     otg_recover, idx_check,    { OTGD, PL } },
    { "otg-list",     otg_setup,    list_run,    otg_recover, list_check,   { OTGD, PL } },
    { "otg-save",     otg_setup,    save_run,    otg_recover, save_check,   { OTGD, PL } },
    { "pl-setcurrent",ctl_setup,    setcur_run,  ctl_recover, ctl_check,    { RB } },
    { "pl-ctlswap",   ctl_setup,    swap_run,    ctl_recover, ctl_check,    { RB } },
    { "pl-save",      pls_setup,    pls_run,     pls_recover, pls_check,    { PL, "/Music", RB } },
    { "pl-save-user", pls_setup,    usr_run,     pls_recover, usr_check,    { PL, "/Music", RB } },
    { "pl-stray",     stray_setup,  stray_run,   pls_recover, stray_check,  { PL, "/Music", RB } },
    { "art-publish",  art_setup,    art_run,     art_recover, art_check,    { ART } },
    { "uilog-rotate", lg_setup,     lg_run,      lg_recover,  lg_check,     { RB }, lg_next },
    { "panels-publish", pp_setup,   pp_run,      pp_recover,  pp_check,     { ART } },
    { "panels-header", pp_setup,    ph_run,      ph_recover,  ph_check,     { ART } },
    { "ab-store",     ab_setup,     ab_run,      ab_recover,  ab_check,     { ART } },
    { "pool-slot",    pool_setup,   pool_run,    pool_recover, pool_check,  { ART } },
};

static int verify(const struct site *s, int faulted)
{
    int bad = netonly ? 0 : s->check(faulted);
    for (int i = 0; i < 3 && s->dirs[i]; i++)
        bad |= dup_check(s->dirs[i]);
    return bad;
}

static int run_site(const struct site *s)
{
    int bad = 0, removes, kmax, nvar = reorder ? 6 : 1;
    unsigned long flushes;

    /* the base: the site's old state on top of the common layout */
    restore();
    mount();
    s->setup();
    powercut();                       /* every write landed (no cut) */
    memcpy(base, img, img_sectors * SECTOR);
    for (size_t i = 0; i < dcount; i++)
        dirty[dlist[i]] = 0;
    dcount = 0;

    /* dry run: N writes, R directory-entry removals */
    mount();
    preload();
    nwrites = 0;
    nflush = 0;
    rt_dirent_calls = 0;
    if (s->run() != 0)
    {
        printf("%s: the sequence fails with no fault\n", s->name);
        return 1;
    }
    kmax = (int)nwrites;
    flushes = nflush;
    removes = rt_dirent_calls;
    powercut();
    mount();
    s->recover();
    bad |= verify(s, 0);
    powercut();
    bad |= chain_check("after the uncut sequence");

    for (int k = 0; k <= kmax && !bad; k++)
    for (int v = 0; v < nvar && !bad; v++)
    {
        restore();
        mount();
        preload();
        cut_at = k;
        nwrites = 0;
        s->run();
        cut_at = -1;
        /* reorder: the drive's unflushed writes, all / none / subsets */
        cut_variant = reorder ? v : -1;
        cut_seed = (unsigned)k * 7919u + (unsigned)v * 104729u + preload_seed;
        powercut();                    /* reboot */
        cut_variant = -1;
        mount();
        if (getenv("SAFETEST_TRACE"))
        {
            DIR *d = rbfs_opendir(s->dirs[0]);
            struct dirent *e;
            printf("k=%d:", k);
            while (d && (e = rbfs_readdir(d)))
                if (getenv("SAFETEST_TRACE_ALL") || strncmp(e->d_name, "filler", 6))
                    printf(" %s", e->d_name);
            if (d) rbfs_closedir(d);
            printf("\n");
        }
        s->recover();
        bad |= verify(s, 0);
        powercut();
        if (!bad)
        {
            /* the recovered image, and once more after the next boot's
             * allocations: a freed chain still named would be reused */
            bad |= chain_check("after recovery");
            mount();
            put(RB "/post.dat", 'b');
            bad |= verify(s, 0);
            rbfs_remove(RB "/post.dat");
            powercut();
            bad |= chain_check("after the next allocation");
            if (!bad && s->next)
            {
                /* the site's next save, then its next boot step */
                mount();
                s->next();
                powercut();
                bad |= chain_check("after the next save");
                mount();
                s->recover();
                put(RB "/post.dat", 'b');
                rbfs_remove(RB "/post.dat");
                for (int i = 0; i < 3 && s->dirs[i]; i++)
                    bad |= dup_check(s->dirs[i]);
                powercut();
                bad |= chain_check("after the next save's boot");
            }
        }
        if (bad)
            printf("%s: power cut after write %d of %d (seed %u%s%d)\n",
                   s->name, k, kmax, preload_seed,
                   reorder ? ", drive cache variant " : "", reorder ? v : 0);
    }
    for (int j = 0; j < removes && !bad; j++)
    {
        restore();
        mount();
        preload();
        rt_dirent_calls = 0;
        rt_fail_at = j;
        s->run();
        rt_fail_at = -1;
        powercut();
        mount();
        s->recover();
        if (verify(s, 1))
        {
            printf("%s: removal %d of %d failing (EIO)\n", s->name, j, removes);
            bad = 1;
        }
        /* then a clean retry succeeds with one entry per name (otg-save
         * would pick the next free name: not retried) */
        if (!bad && s->run != save_run && s->run() != 0)
        {
            printf("%s: retry after the failed removal %d fails\n", s->name, j);
            bad = 1;
        }
        powercut();
        bad |= chain_check("after a failed removal and the retry");
    }
    printf("%s seed %u: %d cut points, %d failing removals, %lu flushes"
           "%s: %s\n", s->name, preload_seed, kmax + 1, removes, flushes,
           reorder ? " (drive cache reorders)" : "", bad ? "FAIL" : "ok");
    return bad;
}

int main(int argc, char **argv)
{
    int bad = 0;
    if (argc < 2) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    img = malloc((size_t)n);
    base = malloc((size_t)n);
    img_sectors = (size_t)n / SECTOR;
    dirty = calloc(img_sectors, 1);
    dlist = malloc(img_sectors * sizeof(*dlist));
    owned = calloc(img_sectors + 2, 1);
    if (!img || !base || !dirty || !dlist || !owned
        || fread(img, 1, (size_t)n, f) != (size_t)n) return 2;
    fclose(f);

    /* the common layout */
    mount();
    rbfs_mkdir(RB);
    rbfs_mkdir(OTGD);
    rbfs_mkdir(ART);
    rbfs_mkdir(PL);
    rbfs_mkdir("/Music");
    for (int i = 0; i < 20; i++)        /* fuller directories */
    {
        char p[64];
        snprintf(p, sizeof(p), RB "/filler%02d.dat", i);
        put(p, 'b');
        snprintf(p, sizeof(p), "/Music/track %02d.mp3", i);
        put(p, 'b');
    }
    powercut();
    memcpy(base, img, img_sectors * SECTOR);
    dcount = 0;
    memset(dirty, 0, img_sectors);
    uint8_t *layout = malloc(img_sectors * SECTOR);
    if (!layout) return 2;
    memcpy(layout, img, img_sectors * SECTOR);

    netonly = getenv("SAFETEST_NETONLY") != NULL;
    reorder = getenv("SAFETEST_REORDER") != NULL;
    pidx = calloc(img_sectors, sizeof(*pidx));
    if (!pidx) return 2;
    unsigned seeds[33] = { 0 };
    int nseeds = 1;
    const char *se = getenv("SAFETEST_SEEDS");
    for (char *end; se && *se && nseeds < 33; se = end)
    {
        unsigned long v = strtoul(se, &end, 0);
        if (end == se)
            break;
        if (v)
            seeds[nseeds++] = (unsigned)v;
    }
    if (chain_check("on the fresh layout"))
        return 1;
    for (int sd = 0; sd < nseeds; sd++)
    for (size_t i = 0; i < sizeof(sites) / sizeof(sites[0]); i++)
    {
        int want = argc == 2;
        for (int a = 2; a < argc; a++)
            want |= !strcmp(argv[a], sites[i].name);
        if (!want)
            continue;
        preload_seed = seeds[sd];
        memcpy(img, layout, img_sectors * SECTOR);
        memcpy(base, layout, img_sectors * SECTOR);
        dcount = 0;
        memset(dirty, 0, img_sectors);
        bad |= run_site(&sites[i]);
    }
    int want_l = argc == 2;
    for (int a = 2; a < argc; a++)
        want_l |= !strcmp(argv[a], "intent-l");
    if (want_l && !reorder && !netonly)
    {
        memcpy(img, layout, img_sectors * SECTOR);
        memcpy(base, layout, img_sectors * SECTOR);
        dcount = 0;
        memset(dirty, 0, img_sectors);
        bad |= intent_l_checks();
    }
    /* the last image back to the file, for fsck_msdos (run.sh) */
    f = fopen(argv[1], "wb");
    if (!f || fwrite(img, SECTOR, img_sectors, f) != img_sectors) return 2;
    fclose(f);
    printf("%s\n", bad ? "FAIL" : "ok");
    return bad;
}
