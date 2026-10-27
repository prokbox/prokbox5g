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
/* tests/fatcrash/dirburst: R1 (#107) directory burst reads in fat.c, over
 * the firmware's whole file layer on a RAM FAT32 image. run.sh builds it
 * twice, with bursts and with -DFAT_NO_DIR_BURST (upstream's one sector
 * per command), and compares the two outputs line for line.
 *
 *   dirburst lib <tracks> <spc>   a library-shaped volume (Music/Artist/
 *       Album/track, LFN names, one large flat folder with deleted
 *       entries); prints the image digest after building it (the writes
 *       must not depend on the reads), then the names, entry count and
 *       storage commands of (1) a recursive walk, as the scan's check_dir
 *       does, and (2) an open of every track by path, as the post-USB
 *       deleted-file pass does
 *   dirburst mut <seed> <spc>     creates, removes and renames entries of
 *       a directory, and recycles a subdirectory's clusters as file data,
 *       while that directory is being enumerated, sometimes with the disk
 *       cache emptied so slots are the only copy. Fails on a name returned
 *       twice in one pass, a name returned after its removal, or a fresh
 *       listing that differs from the model; prints every returned name's
 *       running digest for run.sh to compare across the two builds
 * Exit 0 when every check holds.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <time.h>
#include "config.h"
#include "file.h"
#include "dir.h"
#include "fat.h"
#include "disk_cache.h"
#include "file_internal.h"
#include "fileobj_mgr.h"
#include "rb_namespace.h"

#define SECTOR 512
#define SINGLES 1200   /* the flat folder: 1,200 names, ~9 clusters of 16 KiB */
static uint8_t *img;
static size_t img_sectors;
static unsigned long rd_cmds, rd_secs, wr_top;
static unsigned bps = 512;          /* BPB bytes per sector (argv[4]) */
static long fail_reads;             /* >0: fail that many reads ... */
static unsigned long fail_lo, fail_hi; /* ... touching [lo, hi) */
static int fail_min;                /* ... of at least this many sectors */

int storage_read_sectors(unsigned long start, int count, void *buf)
{
    if (start + count > img_sectors) return -1;
    if (fail_reads > 0 && count >= fail_min &&
        start < fail_hi && fail_lo < start + count)
    {
        fail_reads--;
        return -5;                  /* an I/O error, as ata.c returns */
    }
    rd_cmds++;
    rd_secs += (unsigned long)count;
    memcpy(buf, img + start * SECTOR, (size_t)count * SECTOR);
    return 0;
}

int storage_write_sectors(unsigned long start, int count, const void *buf)
{
    if (start + count > img_sectors) return -1;
    if (start + count > wr_top) wr_top = start + count;
    memcpy(img + start * SECTOR, buf, (size_t)count * SECTOR);
    return 0;
}

/* in-order model: every write lands as issued; a flush has nothing to do */
int storage_flush(void) { return 0; }

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
    t.tm_year = 126; t.tm_mon = 9; t.tm_mday = 20; t.tm_hour = 9;
    return &t;
}
void dostime_localtime(time_t t, uint16_t *d, uint16_t *tm)
{ (void)t; *d = (uint16_t)((46 << 9) | (10 << 5) | 20); *tm = 9 << 11; }
time_t dostime_mktime(uint16_t d, uint16_t t) { (void)d; (void)t; return 0; }

/* FAT32, no partition table, 512 B storage sectors and bps-byte BPB
 * sectors (2048 is the 5.5G's on-disk layout: fat.c scales the BPB by
 * secmult = 4), spc storage sectors per cluster, at least FAT32's 65,525
 * clusters (calloc: untouched pages stay unmapped) */
static unsigned long data_start;    /* storage sector of cluster 2 */
static void mkfs(unsigned spc)
{
    const unsigned k = bps / SECTOR;    /* storage sectors per BPB sector */
    const unsigned rsvd = 32, nfats = 2, bspc = spc / k;
    unsigned long clusters = 66000, fatsz = (clusters * 4 + 8 + bps - 1) / bps;
    unsigned long nsect = rsvd + nfats * fatsz + clusters * bspc; /* BPB */
    if (!bspc) { fprintf(stderr, "spc < bps/512\n"); exit(2); }
    img_sectors = nsect * k;
    data_start = (rsvd + nfats * fatsz) * k;
    img = calloc(img_sectors, SECTOR);
    if (!img) exit(2);
    uint8_t *b = img;
    b[0] = 0xeb; b[1] = 0x58; b[2] = 0x90;
    memcpy(b + 3, "DIRBURST", 8);
    b[11] = (uint8_t)bps; b[12] = (uint8_t)(bps >> 8);
    b[13] = (uint8_t)bspc; b[14] = rsvd; b[16] = nfats;
    b[21] = 0xf8; b[24] = 63; b[26] = 16;
    memcpy(b + 32, &(uint32_t){ (uint32_t)nsect }, 4);
    memcpy(b + 36, &(uint32_t){ (uint32_t)fatsz }, 4);
    memcpy(b + 44, &(uint32_t){ 2 }, 4);
    b[48] = 1; b[50] = 6; b[64] = 0x80; b[66] = 0x29;
    memcpy(b + 71, "DIRBURST   ", 11);
    memcpy(b + 82, "FAT32   ", 8);
    b[510] = 0x55; b[511] = 0xaa;
    memcpy(img + 6 * bps, b, SECTOR);
    b = img + bps;
    memcpy(b, "RRaA", 4);
    memcpy(b + 484, "rrAa", 4);
    memcpy(b + 488, &(uint32_t){ (uint32_t)(clusters - 1) }, 4);
    memcpy(b + 492, &(uint32_t){ 3 }, 4);
    b[510] = 0x55; b[511] = 0xaa;
    for (unsigned f = 0; f < nfats; f++)
    {
        uint32_t *fat = (uint32_t *)(img + (rsvd + f * fatsz) * bps);
        fat[0] = 0x0ffffff8; fat[1] = 0x0fffffff; fat[2] = 0x0fffffff;
    }
}

static void mount(void)
{
    filesystem_init();
    fat_init();
    if (fat_mount(0) != 0) { fprintf(stderr, "mount\n"); exit(2); }
    root_mount_path("/", NSITEM_HIDDEN);
    root_mount_path("/", NSITEM_CONTENTS);
}

static void put(const char *path, size_t n, int fill)
{
    static char buf[16384];
    int fd = rbfs_open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) { fprintf(stderr, "create %s: %d\n", path, errno); exit(2); }
    memset(buf, fill, sizeof(buf));
    while (n)
    {
        size_t k = n < sizeof(buf) ? n : sizeof(buf);
        if (rbfs_write(fd, buf, k) != (ssize_t)k) exit(2);
        n -= k;
    }
    if (rbfs_close(fd) < 0) exit(2);
}

static uint32_t fnv(uint32_t h, const void *p, size_t n)
{
    const uint8_t *b = p;
    while (n--)
        h = (h ^ *b++) * 16777619u;
    return h;
}

/* the disk cache written back and emptied: slots are the only copies */
static void dc_empty(void)
{
    dc_lock_cache();
    dc_commit_all();
    dc_discard_all();
    dc_unlock_cache();
}

/* ---- lib ---- */
static const char *words[] = { "Midnight", "Velvet", "Harbour", "Signal",
    "Paper", "Lanterns", "Northern", "Echoes", "Static", "Gardens",
    "Silver", "Machine", "Hollow", "Summer", "Engines", "Quiet" };

static void title(char *out, size_t n, unsigned seed, int nwords)
{
    size_t k = 0;
    out[0] = 0;
    for (int i = 0; i < nwords; i++)
    {
        seed = seed * 1103515245u + 12345u;
        k += (size_t)snprintf(out + k, n - k, "%s%s", i ? " " : "",
                              words[(seed >> 16) % 16]);
    }
}

struct walk { unsigned long entries, dirs; uint32_t h; };

static void walk(const char *path, struct walk *w, int depth)
{
    DIR *d = rbfs_opendir(path);
    struct dirent *e;
    if (!d) { fprintf(stderr, "opendir %s\n", path); exit(2); }
    w->dirs++;
    while ((e = rbfs_readdir(d)))
    {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        w->entries++;
        w->h = fnv(w->h, e->d_name, strlen(e->d_name) + 1);
        if ((e->info.attr & ATTR_DIRECTORY) && depth < 8)
        {
            char sub[MAX_PATH];
            snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name);
            walk(sub, w, depth + 1);
        }
    }
    rbfs_closedir(d);
}

static int lib(unsigned long tracks, unsigned spc)
{
    char p[MAX_PATH], a[96], b[96], t[96];
    static char (*paths)[160];
    unsigned long npaths = 0, artists = tracks / 20 ? tracks / 20 : 1;
    paths = malloc((tracks + 4000) * sizeof(*paths));
    mkfs(spc);
    mount();
    rbfs_mkdir("/Music");
    for (unsigned long ar = 0; ar < artists; ar++)
    {
        title(a, sizeof(a), (unsigned)ar * 7 + 1, 2 + ar % 3);
        snprintf(p, sizeof(p), "/Music/%s %04lu", a, ar);
        rbfs_mkdir(p);
        for (int al = 0; al < 2; al++)
        {
            title(b, sizeof(b), (unsigned)(ar * 13 + al), 3 + al);
            snprintf(p, sizeof(p), "/Music/%s %04lu/%s (%d)", a, ar, b,
                     1990 + (int)(ar % 30));
            rbfs_mkdir(p);
            for (int tr = 1; tr <= 10; tr++)
            {
                title(t, sizeof(t), (unsigned)(ar * 31 + al * 11 + tr),
                      2 + tr % 4);
                snprintf(paths[npaths], sizeof(paths[0]),
                         "%s/%02d %s.flac", p, tr, t);
                put(paths[npaths++], 0, 0);
            }
        }
    }
    /* one large flat folder: LFN runs cross sectors and clusters; every
     * seventh file removed again (deleted entries inside the runs) */
    rbfs_mkdir("/Music/Singles");
    for (int i = 0; i < SINGLES; i++)
    {
        title(t, sizeof(t), (unsigned)i * 977u, 2 + i % 6);
        snprintf(paths[npaths], sizeof(paths[0]), "/Music/Singles/%s %04d.mp3",
                 t, i);
        put(paths[npaths++], 0, 0);
    }
    for (unsigned long i = npaths - SINGLES; i < npaths; i += 7)
    {
        rbfs_remove(paths[i]);
        paths[i][0] = 0;
    }
    dc_empty();
    printf("image digest %08x\n", fnv(2166136261u, img, wr_top * SECTOR));

    /* (1) the scan's walk, cold */
    struct walk w = { 0, 0, 2166136261u };
    dc_empty();
    rd_cmds = rd_secs = 0;
    clock_t c0 = clock();
    walk("/Music", &w, 0);
    double ms = (double)(clock() - c0) * 1000.0 / CLOCKS_PER_SEC;
    printf("walk: %lu dirs %lu entries names %08x\n", w.dirs, w.entries, w.h);
    fprintf(stderr, "walk: %lu commands, %lu sectors, host %.0f ms\n",
            rd_cmds, rd_secs, ms);
    printf("stat walk_cmds %lu walk_secs %lu\n", rd_cmds, rd_secs);

    /* (2) the deleted-file pass: open every track by its path */
    unsigned long found = 0, missing = 0;
    dc_empty();
    rd_cmds = rd_secs = 0;
    c0 = clock();
    for (unsigned long i = 0; i < npaths; i++)
    {
        if (!paths[i][0])
            continue;
        int fd = rbfs_open(paths[i], O_RDONLY);
        if (fd >= 0) { found++; rbfs_close(fd); } else missing++;
    }
    ms = (double)(clock() - c0) * 1000.0 / CLOCKS_PER_SEC;
    printf("opens: %lu found %lu missing\n", found, missing);
    fprintf(stderr, "opens: %lu commands, %lu sectors, host %.0f ms\n",
            rd_cmds, rd_secs, ms);
    printf("stat open_cmds %lu open_secs %lu\n", rd_cmds, rd_secs);
    return missing != 0;
}

/* ---- mut ---- */
#define MAXN 4096
static char names[MAXN][80];
static bool alive[MAXN];
static int nnames;
static unsigned rnd_state;
static unsigned rnd(void)
{
    rnd_state = rnd_state * 1103515245u + 12345u;
    return (rnd_state >> 8) & 0xffffff;
}

static int find_name(const char *n)
{
    for (int i = 0; i < nnames; i++)
        if (alive[i] && !strcmp(names[i], n))
            return i;
    return -1;
}

static void new_name(char *out)
{
    char t[64];
    title(t, sizeof(t), rnd(), 1 + rnd() % 6);  /* 1..6 words: 1..4 LFN */
    snprintf(out, 80, "%s %05d", t, nnames);
}

static int add_file(void)
{
    if (nnames >= MAXN) return -1;
    char p[MAX_PATH];
    new_name(names[nnames]);
    snprintf(p, sizeof(p), "/D/%s", names[nnames]);
    put(p, rnd() % 3 ? 0 : 1 + rnd() % 3000, 'a' + nnames % 26);
    alive[nnames] = true;
    return nnames++;
}

static int pick_alive(void)
{
    int live = 0;
    for (int i = 0; i < nnames; i++) live += alive[i];
    if (!live) return -1;
    int k = (int)(rnd() % (unsigned)live);
    for (int i = 0; i < nnames; i++)
        if (alive[i] && k-- == 0) return i;
    return -1;
}

static uint32_t mut_h = 2166136261u;
static int bad;

/* one mutation of /D (and the subdirectory recycling); returns its kind */
static void mutate(void)
{
    char p[MAX_PATH], q[MAX_PATH];
    int i, op = (int)(rnd() % 8);
    switch (op)
    {
    case 0: case 1:                     /* create */
        add_file();
        break;
    case 2: case 3:                     /* remove */
        if ((i = pick_alive()) < 0) break;
        snprintf(p, sizeof(p), "/D/%s", names[i]);
        if (rbfs_remove(p) != 0) { printf("remove %s failed\n", p); bad = 1; }
        alive[i] = false;
        break;
    case 4: case 5:                     /* rename within /D */
        if ((i = pick_alive()) < 0 || nnames >= MAXN) break;
        snprintf(p, sizeof(p), "/D/%s", names[i]);
        new_name(names[nnames]);
        snprintf(q, sizeof(q), "/D/%s", names[nnames]);
        if (rbfs_rename(p, q) != 0) { printf("rename failed\n"); bad = 1; }
        alive[i] = false;
        alive[nnames++] = true;
        break;
    case 6:                             /* a subdirectory's clusters */
        rbfs_mkdir("/E/sub");           /* become a file's data */
        for (int k = 0; k < 40; k++)
        {
            snprintf(p, sizeof(p), "/E/sub/entry with a long name %03d", k);
            put(p, 0, 0);
        }
        dc_empty();
        {   /* read it through the slots */
            struct walk w = { 0, 0, 2166136261u };
            walk("/E/sub", &w, 8);
            if (w.entries != 40)
            { printf("/E/sub lists %lu, want 40\n", w.entries); bad = 1; }
        }
        for (int k = 0; k < 40; k++)
        {
            snprintf(p, sizeof(p), "/E/sub/entry with a long name %03d", k);
            rbfs_remove(p);
        }
        rbfs_rmdir("/E/sub");
        put("/E/blob", 64 * 1024, 0x41);   /* likely the freed clusters */
        rbfs_mkdir("/E/sub");               /* and a directory again */
        {
            struct walk w = { 0, 0, 2166136261u };
            walk("/E/sub", &w, 8);
            if (w.entries != 0)
            { printf("new /E/sub lists %lu, want 0\n", w.entries); bad = 1; }
        }
        rbfs_rmdir("/E/sub");
        rbfs_remove("/E/blob");
        break;
    case 7:                             /* the disk cache emptied */
        dc_empty();
        break;
    }
}

static int fresh_check(int pass)
{
    static unsigned char seen[MAXN];
    memset(seen, 0, sizeof(seen));
    DIR *d = rbfs_opendir("/D");
    struct dirent *e;
    int fails = 0;
    while ((e = rbfs_readdir(d)))
    {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        int i = find_name(e->d_name);
        if (i < 0) { printf("pass %d: fresh listing has stale %s\n", pass,
                            e->d_name); fails++; continue; }
        if (seen[i]++) { printf("pass %d: fresh listing has %s twice\n",
                                pass, e->d_name); fails++; }
    }
    rbfs_closedir(d);
    for (int i = 0; i < nnames; i++)
        if (alive[i] && !seen[i])
        { printf("pass %d: fresh listing lacks %s\n", pass, names[i]);
          fails++; }
    return fails;
}

static int mut(unsigned seed, unsigned spc)
{
    rnd_state = seed;
    mkfs(spc);
    mount();
    rbfs_mkdir("/D");
    rbfs_mkdir("/E");
    for (int i = 0; i < 400; i++)
        add_file();
    for (int i = 0; i < 400; i += 5)    /* deleted entries to start with */
    {
        char p[MAX_PATH];
        snprintf(p, sizeof(p), "/D/%s", names[i]);
        rbfs_remove(p);
        alive[i] = false;
    }
    for (int pass = 0; pass < 60 && !bad; pass++)
    {
        static unsigned char seen[MAXN];
        memset(seen, 0, sizeof(seen));
        if (pass % 3 == 0)
            dc_empty();
        DIR *d = rbfs_opendir("/D");
        struct dirent *e;
        int every = 1 + (int)(rnd() % 12);
        int n = 0;
        while ((e = rbfs_readdir(d)))
        {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            mut_h = fnv(mut_h, e->d_name, strlen(e->d_name) + 1);
            int i = find_name(e->d_name);
            if (i < 0)
            {
                printf("pass %d: returned %s after its removal\n", pass,
                       e->d_name);
                bad = 1;
            }
            else if (seen[i]++)
            {
                printf("pass %d: returned %s twice\n", pass, e->d_name);
                bad = 1;
            }
            if (++n % every == 0)
                mutate();
        }
        rbfs_closedir(d);
        if (fresh_check(pass))
            bad = 1;
        if (pass % 4 == 3)
        {
            dc_empty();
            if (fresh_check(pass))
                bad = 1;
        }
    }
    printf("mut seed %u spc %u: %d names made, listing digest %08x, %s\n",
           seed, spc, nnames, mut_h, bad ? "FAIL" : "ok");
    return bad;
}

/* ---- fault: read errors on a burst ---- */
static int list_d(char out[][80], int max, int *errors)
{
    int n = 0, guard = 0;
    DIR *d = rbfs_opendir("/D");
    if (!d) { printf("opendir /D failed\n"); return -1; }
    *errors = 0;
    while (guard++ < 100000)
    {
        errno = 0;
        struct dirent *e = rbfs_readdir(d);
        if (!e)
        {
            if (errno == 0)
                break;              /* end of the folder */
            ++*errors;              /* an I/O error: call again */
            continue;
        }
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (n < max)
            snprintf(out[n++], 80, "%s", e->d_name);
    }
    rbfs_closedir(d);
    return n;
}

static void make_d(void)
{
    rbfs_mkdir("/D");                   /* the first allocation: cluster 3 */
    rbfs_mkdir("/E");
    for (int i = 0; i < 400; i++)
        add_file();
    for (int i = 0; i < 400; i += 5)
    {
        char p[MAX_PATH];
        snprintf(p, sizeof(p), "/D/%s", names[i]);
        rbfs_remove(p);
        alive[i] = false;
    }
    dc_empty();
}

static char ref[MAXN][80], got[MAXN][80];

static int fault(unsigned spc)      /* spc >= 4 */
{
    int nref, n, errs, fails = 0;
    rnd_state = 7;
    mkfs(spc);
    mount();
    make_d();
    nref = list_d(ref, MAXN, &errs);

    /* A: the first burst fails once; the single-sector retry serves it */
    dc_empty();
    fail_reads = 1; fail_min = 2; fail_lo = 0; fail_hi = img_sectors;
    n = list_d(got, MAXN, &errs);
    if (fail_reads || errs || n != nref || memcmp(ref, got, n * 80))
    { printf("fault A: %d of %d names, %d errors, fault %s\n", n, nref, errs,
             fail_reads ? "unused" : "used"); fails++; }

    /* B: one of the folder's sectors 1..3 (cluster 3; an item's entries
     * may straddle it) fails on its next three reads (bursts over it and
     * single-sector retries): readdir reports at most one error, then the
     * listing goes on with no entry lost or repeated */
    for (unsigned sec = 1; sec < 4; sec++)
    {
        unsigned long x = data_start + spc + sec;
        dc_empty();
        fail_reads = 3; fail_min = 1; fail_lo = x; fail_hi = x + 1;
        n = list_d(got, MAXN, &errs);
        if (fail_reads || errs > 1 || n != nref ||
            memcmp(ref, got, n * 80))
        { printf("fault B sector %u: %d of %d names, %d errors\n", sec, n, nref,
                 errs);
          for (int i = 0; i < n; i++)
              if (i >= nref || strcmp(ref[i], got[i]))
              { printf("  first difference at %d: got %s, want %s\n", i, got[i],
                       i < nref ? ref[i] : "(end)"); break; }
          fails++; }
    }
    fail_reads = 0;
    printf("fault spc %u bps %u: %d names, %s\n", spc, bps, nref,
           fails ? "FAIL" : "ok");
    return fails != 0;
}

/* ---- usb: unmount, the host writes, remount, mid-enumeration ---- */
static int usb(unsigned spc)
{
    int fails = 0;
    rnd_state = 11;
    mkfs(spc);
    mount();
    make_d();
    /* the volume as the host will put it back */
    unsigned long snap_top = wr_top;
    uint8_t *snap = malloc(snap_top * SECTOR);
    memcpy(snap, img, snap_top * SECTOR);
    bool alive_a[MAXN];
    memcpy(alive_a, alive, sizeof(alive_a));
    int nnames_a = nnames;

    DIR *d = rbfs_opendir("/D");
    for (int i = 0; i < 150 && rbfs_readdir(d); i++)
        ;                               /* slots now hold /D */
    for (int i = 0; i < 50; i++)        /* the device changes /D ... */
    {
        int k = pick_alive();
        char p[MAX_PATH];
        snprintf(p, sizeof(p), "/D/%s", names[k]);
        rbfs_remove(p);
        alive[k] = false;
        add_file();
    }
    for (int i = 0; i < 100 && rbfs_readdir(d); i++)
        ;                               /* ... and reads its new sectors */

    /* USB: disk_unmount_all() as usb.c does, the host rewrites the
     * volume, then the remount */
    root_unmount_volume();
    fileobj_mgr_unmount();
    fat_unmount();
    memcpy(img, snap, snap_top * SECTOR);
    if (wr_top > snap_top)
        memset(img + snap_top * SECTOR, 0, (wr_top - snap_top) * SECTOR);
    if (fat_mount(0) != 0) { printf("remount failed\n"); return 1; }
    root_mount_path("/", NSITEM_HIDDEN);
    root_mount_path("/", NSITEM_CONTENTS);

    errno = 0;
    if (rbfs_readdir(d))
    { printf("usb: a handle from before the unmount still reads\n");
      fails++; }
    rbfs_closedir(d);

    memcpy(alive, alive_a, sizeof(alive_a));
    nnames = nnames_a;
    fails += fresh_check(0);
    printf("usb spc %u bps %u: %s\n", spc, bps, fails ? "FAIL" : "ok");
    free(snap);
    return fails != 0;
}

int main(int argc, char **argv)
{
    if (argc == 5)
        bps = (unsigned)atoi(argv[4]);
    if (argc >= 4 && !strcmp(argv[1], "fault"))
        return fault((unsigned)atoi(argv[3]));
    if (argc >= 4 && !strcmp(argv[1], "usb"))
        return usb((unsigned)atoi(argv[3]));
    if (argc >= 4 && !strcmp(argv[1], "lib"))
        return lib(strtoul(argv[2], NULL, 0), (unsigned)atoi(argv[3]));
    if (argc >= 4 && !strcmp(argv[1], "mut"))
        return mut((unsigned)atoi(argv[2]), (unsigned)atoi(argv[3]));
    fprintf(stderr, "usage: dirburst lib <tracks> <spc> [bps] | "
            "mut <seed> <spc> [bps] | fault - <spc> [bps] | usb - <spc> [bps]\n");
    return 2;
}
