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
/* tests/fatcrash/fsreplay: replays a simulator session's file operations
 * (IPODUI_FSTRACE, uisimulator/common/filesystem-sim.c) on the firmware's
 * whole file layer over a RAM FAT32 image with the 5.5G's geometry
 * (2048-byte BPB sectors, 16 KiB clusters), with the target-only one-
 * sector records (.bootbatt, .rename_intent) the simulator never makes.
 * A panicf() (e.g. fat_closewrite()'s "Updating size on empty dir entry",
 * the 2026-10-23 device freeze) fails the run.
 *
 *   fsreplay <trace> [spc] [bps]   replay; exit 0 when nothing panicked
 *   fsreplay count <trace> [spc] [bps]
 *                                  replay with the reads (IPODUI_FSTRACE_READS)
 *                                  and print the storage commands of each
 *                                  "mark" span (#245, tests/perf/commit_trace.sh)
 *   fsreplay records [spc] [bps]   the record files are a sector long
 *   fsreplay emptyslot [spc] [bps] a close on an emptied entry fails soft
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <time.h>
#include <setjmp.h>
#include <unistd.h>
#include <sys/wait.h>
#include "config.h"
#include "file.h"
#include "dir.h"
#include "fat.h"
#include "disk_cache.h"
#include "file_internal.h"
#include "fileobj_mgr.h"
#include "rb_namespace.h"
#include "filesystem-native.h"

#define SECTOR 512
static uint8_t *img;
static size_t img_sectors;
static unsigned bps = 2048;

/* count mode (#245): the storage commands and 512-byte sectors the
 * marked thread's file calls cost, and its calls, per marked span */
enum { CN_RD, CN_WR, CN_RDSEC, CN_WRSEC, CN_API_RD, CN_API_WR, CN_API_OPEN,
       CN_N };
static int cn_mode, cn_counting;
static long cn_now[CN_N];
/* and per file (the path the trace opened the fd with) */
#define CN_FILES 32
static struct { char path[64]; long v[CN_N]; } cn_file[CN_FILES];
static int cn_nfiles, cn_cur = -1;
static int cn_fdfile[64];

int storage_read_sectors(unsigned long start, int count, void *buf)
{
    if (cn_counting)
    {
        cn_now[CN_RD]++;
        cn_now[CN_RDSEC] += count;
        if (cn_cur >= 0)
        {
            cn_file[cn_cur].v[CN_RD]++;
            cn_file[cn_cur].v[CN_RDSEC] += count;
        }
    }
    if (start + count > img_sectors) return -1;
    memcpy(buf, img + start * SECTOR, (size_t)count * SECTOR);
    return 0;
}

/* cut mode (#177): every storage write counted; at wr_limit the power
 * goes: the write that would be number wr_limit + 1 never lands, and the
 * replay stops there (longjmp: nothing after it runs, so no later read
 * sees a sector that a dropped write left stale) */
static long wr_count, wr_limit = -1;
static int wr_cut_armed;
static jmp_buf wr_cut_jmp;

/* two undo logs of 512-byte sectors (first old content per entry):
 * pend: written since the last storage_flush() (the drive cache's
 * contents, FSREPLAY_REORDER); jr: written since jr_on (a check's own
 * writes, undone to try the next cache outcome on the same cut) */
struct undo { unsigned long sec; uint8_t old[SECTOR]; };
static struct undo *pend, *jr;
static size_t pend_n, pend_cap, jr_n, jr_cap;
static int pend_on, jr_on;

static void undo_add(struct undo **u, size_t *n, size_t *cap,
                     unsigned long sec)
{
    if (*n == *cap)
    {
        *cap = *cap ? *cap * 2 : 4096;
        *u = realloc(*u, *cap * sizeof(**u));
        if (!*u) exit(2);
    }
    (*u)[*n].sec = sec;
    memcpy((*u)[*n].old, img + sec * SECTOR, SECTOR);
    (*n)++;
}

static void journal(unsigned long sec)
{
    if (pend_on)
        undo_add(&pend, &pend_n, &pend_cap, sec);
    if (jr_on)
        undo_add(&jr, &jr_n, &jr_cap, sec);
}

/* undo in reverse: each sector back to its oldest logged content */
static void undo_all(struct undo *u, size_t n)
{
    while (n--)
        memcpy(img + u[n].sec * SECTOR, u[n].old, SECTOR);
}

/* dbguard (p081-fatdb): a drive that acknowledges writes to the sectors
 * [drop_lo, drop_hi) and keeps none of them (the stale-sector model) */
static unsigned long drop_lo, drop_hi;
static long drop_hits;

int storage_write_sectors(unsigned long start, int count, const void *buf)
{
    if (cn_counting)
    {
        cn_now[CN_WR]++;
        cn_now[CN_WRSEC] += count;
        if (cn_cur >= 0)
        {
            cn_file[cn_cur].v[CN_WR]++;
            cn_file[cn_cur].v[CN_WRSEC] += count;
        }
    }
    if (start + count > img_sectors) return -1;
    if (start < drop_hi && drop_lo < start + count)
    {
        drop_hits++;
        for (int i = 0; i < count; i++)     /* the rest of a run lands */
            if (start + i < drop_lo || start + i >= drop_hi)
                memcpy(img + (start + i) * SECTOR,
                       (const uint8_t *)buf + (size_t)i * SECTOR, SECTOR);
        return 0;
    }
    if (wr_limit >= 0 && wr_count >= wr_limit)
    {
        if (wr_cut_armed)
        {
            wr_cut_armed = 0;
            longjmp(wr_cut_jmp, 1);
        }
        return 0;                   /* the power is gone: nothing lands */
    }
    wr_count++;
    for (int i = 0; i < count; i++)
        journal(start + i);
    memcpy(img + start * SECTOR, buf, (size_t)count * SECTOR);
    return 0;
}

/* FSREPLAY_REORDER (as safetest's SAFETEST_REORDER): the drive's volatile
 * write cache. A sector written since the last flush (fat_barrier()) has
 * either landed or not at a cut, each on its own; only the orders the
 * file layer enforces with flushes hold. */
int storage_flush(void)
{
    pend_n = 0;
    return 0;
}

#ifdef MAX_VIRT_SECTOR_SIZE
/* the 5.5G: 512-byte storage sectors under 2048-byte BPB sectors */
int disk_get_log_sector_size(void) { return SECTOR; }
#endif

static long lineno;
void panicf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "PANIC (trace line %ld): ", lineno);
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
    t.tm_year = 126; t.tm_mon = 9; t.tm_mday = 23; t.tm_hour = 9;
    return &t;
}
void dostime_localtime(time_t t, uint16_t *d, uint16_t *tm)
{ (void)t; *d = (uint16_t)((46 << 9) | (10 << 5) | 23); *tm = 9 << 11; }
time_t dostime_mktime(uint16_t d, uint16_t t) { (void)d; (void)t; return 0; }

/* as dirburst.c: FAT32, no partition table, bps-byte BPB sectors */
static void mkfs(unsigned spc)
{
    const unsigned k = bps / SECTOR;
    const unsigned rsvd = 32, nfats = 2, bspc = spc / k;
    unsigned long clusters = 66000, fatsz = (clusters * 4 + 8 + bps - 1) / bps;
    unsigned long nsect = rsvd + nfats * fatsz + clusters * bspc;
    if (!bspc) { fprintf(stderr, "spc < bps/512\n"); exit(2); }
    img_sectors = nsect * k;
    img = calloc(img_sectors, SECTOR);
    if (!img) exit(2);
    uint8_t *b = img;
    b[0] = 0xeb; b[1] = 0x58; b[2] = 0x90;
    memcpy(b + 3, "FSREPLAY", 8);
    b[11] = (uint8_t)bps; b[12] = (uint8_t)(bps >> 8);
    b[13] = (uint8_t)bspc; b[14] = rsvd; b[16] = nfats;
    b[21] = 0xf8; b[24] = 63; b[26] = 16;
    memcpy(b + 32, &(uint32_t){ (uint32_t)nsect }, 4);
    memcpy(b + 36, &(uint32_t){ (uint32_t)fatsz }, 4);
    memcpy(b + 44, &(uint32_t){ 2 }, 4);
    b[48] = 1; b[50] = 6; b[64] = 0x80; b[66] = 0x29;
    memcpy(b + 71, "FSREPLAY   ", 11);
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

static long size_of(const char *path)
{
    int fd = rbfs_open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    long n = rbfs_ffilesize(fd);
    rbfs_close(fd);
    return n;
}

/* the records a first boot makes (bootbatt.c latch_create(), file.c
 * rbfs_rename_intent_boot()): each must hold one whole sector, or every
 * later record_io() refuses it (-2) and bootbatt repairs it forever */
static int records(void)
{
    int bad = 0;
    rbfs_mkdir(ROCKBOX_DIR);
    /* first boot: both made */
    rbfs_rename_intent_boot();
    rbfs_record_create(ROCKBOX_DIR "/.bootbatt");
    /* the device after the 2048-byte sector bug: 0-byte records, made
     * good at the next boot (intent) and by the latch repair (bootbatt.c
     * latch_repair(): remove, then create) */
    rbfs_remove(ROCKBOX_DIR "/.rename_intent");
    rbfs_remove(ROCKBOX_DIR "/.bootbatt");
    rbfs_close(rbfs_open(ROCKBOX_DIR "/.rename_intent", O_WRONLY | O_CREAT));
    rbfs_close(rbfs_open(ROCKBOX_DIR "/.bootbatt", O_WRONLY | O_CREAT));
    rbfs_rename_intent_boot();
    rbfs_remove(ROCKBOX_DIR "/.bootbatt");
    rbfs_record_create(ROCKBOX_DIR "/.bootbatt");
    const char *rec[] = { ROCKBOX_DIR "/.rename_intent",
                          ROCKBOX_DIR "/.bootbatt" };
    for (int i = 0; i < 2; i++)
    {
        long n = size_of(rec[i]);
        char data[16] = "record";
        int w = rbfs_record_write(rec[i], data, sizeof(data));
        printf("%s size=%ld write=%d\n", rec[i], n, w);
        if (n <= 0 || w != (int)sizeof(data))
            bad++;
    }
    return bad ? 1 : 0;
}

/* fat.c's empty-slot case (FAT_SOFT_EMPTY_SLOT): a file open for
 * writing whose short entry was cleared under it (on the image, the disk
 * cache dropped) closes with an error, no panic, the case recorded, the
 * descriptor freed and the volume still usable */
#ifdef FSREPLAY_NO_SLOT_FAULT    /* run.sh's control: an old fat.c */
unsigned long fat_slot_fault_get(struct fat_slot_fault *out)
{
    memset(out, 0, sizeof(*out));
    return 0;
}
#endif
static int emptyslot(void)
{
    rbfs_mkdir(ROCKBOX_DIR);
    int fd = rbfs_open(ROCKBOX_DIR "/SLOTTEST.DAT", O_WRONLY | O_CREAT);
    char d[100];
    memset(d, 'd', sizeof(d));
    if (fd < 0 || rbfs_write(fd, d, sizeof(d)) != sizeof(d)
        || rbfs_fsync(fd) < 0)
        return 2;
    dc_lock_cache();
    dc_commit_all();
    dc_discard_all();
    dc_unlock_cache();
    int hits = 0;
    for (size_t i = 0; i + 32 <= img_sectors * SECTOR; i += 32)
        if (!memcmp(img + i, "SLOTTESTDAT", 11))
        {
            img[i] = 0xe5;
            hits++;
        }
    rbfs_write(fd, d, sizeof(d));
    int rc = rbfs_close(fd);
    struct fat_slot_fault sf;
    fat_slot_fault_get(&sf);
    printf("emptyslot hits=%d close=%d count=%lu entry=%lu this=%02x "
           "from_disk=%d\n", hits, rc, sf.count,
           sf.entry, sf.raw[1][0], (int)sf.from_disk);
    int fds[8], ok = 1;
    for (int i = 0; i < 8; i++)
    {
        char p[64];
        snprintf(p, sizeof(p), ROCKBOX_DIR "/after%d.dat", i);
        fds[i] = rbfs_open(p, O_WRONLY | O_CREAT);
        ok &= fds[i] >= 0 && rbfs_write(fds[i], d, 10) == 10;
    }
    for (int i = 0; i < 8; i++)
        ok &= fds[i] < 0 || rbfs_close(fds[i]) == 0;
    return hits == 1 && rc < 0 && sf.count == 1
           && sf.raw[1][0] == 0xe5 && ok ? 0 : 1;
}

/* host (macOS) open flags in the trace -> the firmware's */
static int oflag_of(unsigned long h)
{
    int f = (h & 3) == 1 ? O_WRONLY : (h & 3) == 2 ? O_RDWR : O_RDONLY;
    if (h & 0x0008) f |= O_APPEND;
    if (h & 0x0200) f |= O_CREAT;
    if (h & 0x0400) f |= O_TRUNC;
    if (h & 0x0800) f |= O_EXCL;
    return f;
}

static int ours(const char *path)
{
    return !strncmp(path, ROCKBOX_DIR "/", sizeof(ROCKBOX_DIR));
}

/* replay: fds the trace names map to ours; files outside the hidden
 * folder (the music) are not on the image and are skipped */
/* cut mode: the storage write count at the last two "point" lines and at
 * the "powercut" line, and the cut point's name */
static long pt_last_wr = -1, pt_prev_wr = -1, pc_wr = -1;
static char pc_name[160];
static int boots;

static long replay_f(FILE *f, int *map, int cut);
/* count mode: the spans are named by the "mark" lines (each mark closes
 * the span since the one before; "begin" opens the first), on the marked
 * thread only, until "end"; the totals print at the end, one line per
 * name */
#define CN_SPANS 32
static char cn_thread[16];
static int cn_active;
static struct { char name[16]; long v[CN_N]; } cn_span[CN_SPANS];
static int cn_nspans;
static long cn_last[CN_N];

static void cn_mark(const char *name, const char *thread)
{
    int i, k;

    if (!strcmp(name, "begin"))
    {
        snprintf(cn_thread, sizeof(cn_thread), "%s", thread);
        cn_active = 1;
        memcpy(cn_last, cn_now, sizeof(cn_last));
        return;
    }
    if (!cn_active || strcmp(thread, cn_thread))
        return;
    if (!strcmp(name, "end"))
    {
        cn_active = 0;      /* the spans are over: nothing more counts */
        return;
    }
    for (i = 0; i < cn_nspans && strcmp(cn_span[i].name, name); i++)
        ;
    if (i == cn_nspans)
    {
        if (cn_nspans == CN_SPANS)
            return;
        snprintf(cn_span[cn_nspans++].name, sizeof(cn_span[0].name), "%s",
                 name);
    }
    for (k = 0; k < CN_N; k++)
        cn_span[i].v[k] += cn_now[k] - cn_last[k];
    memcpy(cn_last, cn_now, sizeof(cn_last));
}

static void cn_print(void)
{
    long t[CN_N] = { 0 };
    int i, k;

    printf("%-8s %8s %8s %9s %9s %8s %8s %6s\n", "span", "rd_cmds",
           "wr_cmds", "rd_sect", "wr_sect", "api_rd", "api_wr", "opens");
    for (i = 0; i < cn_nspans; i++)
    {
        printf("%-8s %8ld %8ld %9ld %9ld %8ld %8ld %6ld\n", cn_span[i].name,
               cn_span[i].v[CN_RD], cn_span[i].v[CN_WR],
               cn_span[i].v[CN_RDSEC], cn_span[i].v[CN_WRSEC],
               cn_span[i].v[CN_API_RD], cn_span[i].v[CN_API_WR],
               cn_span[i].v[CN_API_OPEN]);
        for (k = 0; k < CN_N; k++)
            t[k] += cn_span[i].v[k];
    }
    printf("%-8s %8ld %8ld %9ld %9ld %8ld %8ld %6ld\n", "total",
           t[CN_RD], t[CN_WR], t[CN_RDSEC], t[CN_WRSEC], t[CN_API_RD],
           t[CN_API_WR], t[CN_API_OPEN]);
    printf("per file (the marked spans):\n");
    for (i = 0; i < cn_nfiles; i++)
        if (cn_file[i].v[CN_RD] || cn_file[i].v[CN_WR]
            || cn_file[i].v[CN_API_RD] || cn_file[i].v[CN_API_WR])
            printf("  %-28s %8ld %8ld %9ld %9ld %8ld %8ld\n", cn_file[i].path,
               cn_file[i].v[CN_RD], cn_file[i].v[CN_WR],
               cn_file[i].v[CN_RDSEC], cn_file[i].v[CN_WRSEC],
               cn_file[i].v[CN_API_RD], cn_file[i].v[CN_API_WR]);
}

static int cn_file_of(const char *path)
{
    const char *b = strrchr(path, '/');
    int i;

    b = b ? b + 1 : path;
    for (i = 0; i < cn_nfiles && strcmp(cn_file[i].path, b); i++)
        ;
    if (i == cn_nfiles)
    {
        if (cn_nfiles == CN_FILES)
            return -1;
        snprintf(cn_file[cn_nfiles++].path, sizeof(cn_file[0].path), "%s", b);
    }
    return i;
}

static int replay(const char *trace)
{
    FILE *f = fopen(trace, "r");
    if (!f) { perror(trace); return 2; }
    int map[64];
    for (int i = 0; i < 64; i++) map[i] = -1;
    long ops = replay_f(f, map, 0);
    fclose(f);
    for (int i = 0; i < 64; i++)
        if (map[i] >= 0)
            rbfs_close(map[i]);
    printf("replayed %ld operations, no panic\n", ops);
    if (cn_mode)
    {
        if (!cn_nspans)
        {
            printf("fsreplay count: no marked span in the trace\n");
            return 2;
        }
        cn_print();
    }
    return 0;
}

/* replays from f's position; cut: stops at a "powercut" line and returns
 * -1 there, else the number of operations at the end of the file */
static long replay_f(FILE *f, int *map, int cut)
{
    static char buf[65536];
    memset(buf, 'x', sizeof(buf));
    char line[2048];
    long ops = 0;
    /* FSREPLAY_INJECT=<line>: the boot guard's latch repair (bootbatt.c
     * latch_repair(): remove, record create, record write) runs just
     * before that trace line, as the UI thread's bootbatt_service() may */
    const char *inj = getenv("FSREPLAY_INJECT");
    long inject = inj ? atol(inj) : -1;
    while (fgets(line, sizeof(line), f))
    {
        char *v[6] = { 0 };
        int n = 0;
        lineno++;
        if (lineno == inject && getenv("FSREPLAY_INJECT_OPS"))
        {
            /* another thread's operations at this point: every open,
             * close, remove and rename holds the file layer's writer
             * lock, so any interleaving is some order of whole ops */
            FILE *g = fopen(getenv("FSREPLAY_INJECT_OPS"), "r");
            long save = lineno;
            if (g)
            {
                int gmap[64];
                for (int i = 0; i < 64; i++) gmap[i] = -1;
                char gl[2048];
                while (fgets(gl, sizeof(gl), g))
                {
                    char *w[6] = { 0 };
                    int m = 0;
                    gl[strcspn(gl, "\n")] = 0;
                    for (char *q = strtok(gl, "\t"); q && m < 6;
                         q = strtok(NULL, "\t"))
                        w[m++] = q;
                    if (m < 2) continue;
                    int gfd = atoi(w[1]) & 63;
                    if (!strcmp(w[0], "open") && m >= 4)
                        gmap[gfd] = rbfs_open(w[2],
                                        oflag_of(strtoul(w[3], NULL, 16)));
                    else if (!strcmp(w[0], "write") && gmap[gfd] >= 0)
                        rbfs_write(gmap[gfd], buf, atol(w[2]) % 4096);
                    else if (!strcmp(w[0], "fsync") && gmap[gfd] >= 0)
                        rbfs_fsync(gmap[gfd]);
                    else if (!strcmp(w[0], "close") && gmap[gfd] >= 0)
                    {
                        rbfs_close(gmap[gfd]);
                        gmap[gfd] = -1;
                    }
                    else if (!strcmp(w[0], "remove") && m >= 3)
                        rbfs_remove(w[2]);
                    else if (!strcmp(w[0], "rename") && m >= 4)
                        rbfs_rename(w[2], w[3]);
                }
                fclose(g);
            }
            lineno = save;
        }
        else if (lineno == inject)
        {
            char data[16] = "r";
            rbfs_remove(ROCKBOX_DIR "/.bootbatt");
            rbfs_record_create(ROCKBOX_DIR "/.bootbatt");
            rbfs_record_write(ROCKBOX_DIR "/.bootbatt", data, sizeof(data));
        }
        line[strcspn(line, "\n")] = 0;
        for (char *p = strtok(line, "\t"); p && n < 6; p = strtok(NULL, "\t"))
            v[n++] = p;
        if (n < 2)
            continue;
        int tfd = atoi(v[1]);
        if (cn_mode)
        {
            cn_counting = cn_active && !strcmp(v[n - 1], cn_thread);
            cn_cur = -1;
            if (!strcmp(v[0], "open") && n >= 4 && tfd >= 0 && tfd < 64)
                cn_fdfile[tfd] = cn_cur = ours(v[2]) ? cn_file_of(v[2])
                                                     : -1;
            else if (tfd >= 0 && tfd < 64 && strcmp(v[0], "mark"))
                cn_cur = cn_fdfile[tfd];
            if (cn_cur >= 0 && cn_counting)
            {
                if (!strcmp(v[0], "read"))
                    cn_file[cn_cur].v[CN_API_RD]++;
                else if (!strcmp(v[0], "write"))
                    cn_file[cn_cur].v[CN_API_WR]++;
            }
            if (!strcmp(v[0], "mark") && n >= 4)
            {
                cn_mark(v[2], v[n - 1]);
                continue;
            }
            if (cn_counting && !strcmp(v[0], "open"))
                cn_now[CN_API_OPEN]++;
            else if (cn_counting && !strcmp(v[0], "write"))
                cn_now[CN_API_WR]++;
            else if (!strcmp(v[0], "read") && n >= 4)
            {
                if (cn_counting)
                    cn_now[CN_API_RD]++;
                if (tfd >= 0 && tfd < 64 && map[tfd] >= 0)
                {
                    long k = atol(v[2]);
                    while (k > 0)
                    {
                        long c = k < (long)sizeof(buf) ? k
                                                       : (long)sizeof(buf);
                        if (rbfs_read(map[tfd], buf, c) <= 0)
                            break;
                        k -= c;
                    }
                }
                continue;
            }
        }
        if (!strcmp(v[0], "boot"))
        {
            /* a new boot of the simulator (IPODUI_FSTRACE_APPEND): the
             * last one's descriptors are gone */
            for (int i = 0; i < 64; i++)
                if (map[i] >= 0)
                {
                    rbfs_close(map[i]);
                    map[i] = -1;
                }
            boots++;
            continue;
        }
        if (!strcmp(v[0], "point") && n >= 3)
        {
            pt_prev_wr = pt_last_wr;
            pt_last_wr = wr_count;
            continue;
        }
        if (!strcmp(v[0], "powercut") && n >= 3)
        {
            if (!cut)
                continue;
            pc_wr = wr_count;
            snprintf(pc_name, sizeof(pc_name), "%s %s", v[2],
                     n >= 4 ? v[3] : "");
            return -1;
        }
        if (!strcmp(v[0], "seed") && boots > 1)
            continue;   /* a later boot's seeds are the host's files, which
                           a power cut never touched: the image rules */
        if (!strcmp(v[0], "open") && n >= 4)
        {
            if (!ours(v[2]) || tfd < 0 || tfd >= 64)
                continue;
            if (map[tfd] >= 0)
                rbfs_close(map[tfd]);
            map[tfd] = rbfs_open(v[2], oflag_of(strtoul(v[3], NULL, 16)));
            if (map[tfd] < 0 && getenv("FSREPLAY_VERBOSE"))
                printf("  | line %ld: open %s %s failed (errno %d)\n",
                       lineno, v[2], v[3], errno);
            ops++;
        }
        else if (!strcmp(v[0], "close"))
        {
            if (tfd >= 0 && tfd < 64 && map[tfd] >= 0)
            {
                rbfs_close(map[tfd]);
                map[tfd] = -1;
                ops++;
            }
        }
        else if (!strcmp(v[0], "write") && n >= 3)
        {
            if (tfd >= 0 && tfd < 64 && map[tfd] >= 0)
            {
                long k = atol(v[2]);
                while (k > 0)
                {
                    long c = k < (long)sizeof(buf) ? k : (long)sizeof(buf);
                    rbfs_write(map[tfd], buf, c);
                    k -= c;
                }
                ops++;
            }
        }
        else if (!strcmp(v[0], "lseek") && n >= 4)
        {
            if (tfd >= 0 && tfd < 64 && map[tfd] >= 0)
                rbfs_lseek(map[tfd], atol(v[2]), atoi(v[3]));
        }
        else if (!strcmp(v[0], "ftruncate") && n >= 3)
        {
            if (tfd >= 0 && tfd < 64 && map[tfd] >= 0)
                rbfs_ftruncate(map[tfd], atol(v[2]));
        }
        else if (!strcmp(v[0], "fsync"))
        {
            if (tfd >= 0 && tfd < 64 && map[tfd] >= 0)
            {
                rbfs_fsync(map[tfd]);
                ops++;
            }
        }
        else if (!strcmp(v[0], "remove") && n >= 3 && ours(v[2]))
        {
            rbfs_remove(v[2]);
            ops++;
        }
        else if (!strcmp(v[0], "rename") && n >= 4 && ours(v[2]))
        {
            rbfs_rename(v[2], v[3]);
            ops++;
        }
        else if (!strcmp(v[0], "mkdir") && n >= 3 && ours(v[2]))
            rbfs_mkdir(v[2]);
        else if (!strcmp(v[0], "rmdir") && n >= 3 && ours(v[2]))
            rbfs_rmdir(v[2]);
        else if (!strcmp(v[0], "seed") && n >= 3)
        {
            /* a file the session starts with: "seed <path> <size>", or a
             * folder: "seed <path> -1" */
            long k = atol(v[2]);
            if (k < 0)
                rbfs_mkdir(v[1]);
            else
            {
                int fd = rbfs_open(v[1], O_WRONLY | O_CREAT | O_TRUNC);
                while (fd >= 0 && k > 0)
                {
                    long c = k < (long)sizeof(buf) ? k : (long)sizeof(buf);
                    rbfs_write(fd, buf, c);
                    k -= c;
                }
                if (fd >= 0)
                    rbfs_close(fd);
            }
        }
        else if (!strcmp(v[0], "record") && n >= 3)
        {
            /* a target-only one-sector record op: create|write|remove */
            char data[16] = "r";
            if (!strcmp(v[1], "create"))
                rbfs_record_create(v[2]);
            else if (!strcmp(v[1], "write"))
                rbfs_record_write(v[2], data, sizeof(data));
            else if (!strcmp(v[1], "remove"))
                rbfs_remove(v[2]);
            ops++;
        }
    }
    return ops;
}

/* ---- cut mode (#177, sim-fidelity S2) ---------------------------------- *
 * fsreplay cut <trace> [spc] [bps]: the trace of a simulator session that
 * a "powercut at <point>" ended. The power is cut after every storage
 * write since the point before that one (the window the code ran between
 * the two), each time on a fresh image: then the drive comes back, the
 * file layer mounts and runs its boot recovery, and the image must pass
 * a read-only fsck (fsck_msdos -n, else fsck.fat -n) with no chain fault
 * and every file in the hidden folder must read to its size; then the
 * trace's next boot (if any) is replayed on it and both checks again. */
static const char *fsck_tool;
static char img_path[1100];
static long files_checked;

static int cut_map[64];             /* the cut session's descriptors */
static void power_off(void)
{
    /* nothing more lands: the descriptors go (their closes write
     * nothing), unmount drops the disk cache unwritten */
    wr_limit = wr_count;
    wr_cut_armed = 0;
    for (int i = 0; i < 64; i++)
        if (cut_map[i] >= 0)
        {
            rbfs_close(cut_map[i]);
            cut_map[i] = -1;
        }
    root_unmount_volume();
    fileobj_mgr_unmount();
    fat_unmount();
    wr_limit = -1;
}

static void boot_image(void)
{
    mount();
    rbfs_rename_intent_boot();      /* main.c: before any app opens a file */
}

static int dump_image(void)
{
    FILE *o = fopen(img_path, "wb");
    static const uint8_t zero[65536];
    size_t total = img_sectors * SECTOR;

    if (!o)
        return -1;
    for (size_t off = 0; off < total; off += sizeof(zero))
    {
        size_t n = total - off < sizeof(zero) ? total - off : sizeof(zero);
        if (!memcmp(img + off, zero, n))
            fseek(o, (long)(off + n), SEEK_SET);   /* a hole */
        else
        {
            fseek(o, (long)off, SEEK_SET);
            fwrite(img + off, 1, n, o);
        }
    }
    if (fseek(o, (long)total - 1, SEEK_SET) == 0)
        fputc(img[total - 1], o);
    return fclose(o);
}

/* fsck -n: the faults a reader trips on fail (a chain shorter than the
 * size, a free or cross-linked cluster, a duplicate or broken name);
 * leaks (lost clusters, a chain longer than the size, FSInfo's count)
 * are counted but pass: they read fine and cost space only */
static int fsck_image(const char *label, int *leaks)
{
    char cmd[1300], l[512];
    int bad = 0;

    *leaks = 0;
    if (dump_image() != 0)
    {
        printf("  %s: cannot write the image\n", label);
        return 1;
    }
    snprintf(cmd, sizeof(cmd), "%s -n '%s' 2>&1", fsck_tool, img_path);
    FILE *p = popen(cmd, "r");
    if (!p)
    {
        printf("  %s: fsck failed to run\n", label);
        return 1;
    }
    while (fgets(l, sizeof(l), p))
    {
        l[strcspn(l, "\n")] = 0;
        if (getenv("FSREPLAY_VERBOSE") && atoi(getenv("FSREPLAY_VERBOSE")) > 1)
            printf("  | %s\n", l);
        if (strstr(l, "too few clusters") || strstr(l, "too short")
            || strstr(l, "starts with free") || strstr(l, "cross-linked")
            || strstr(l, "out of range") || strstr(l, "uplicate")
            || strstr(l, "free cluster")
            || strstr(l, "bad cluster") || strstr(l, "Bad cluster")
            || strstr(l, "should at most be"))   /* size past the chain */
        {
            printf("  %s: fsck: %s\n", label, l);
            bad++;
        }
        else if (strstr(l, "ost cluster") || strstr(l, "rphan")
                 || strstr(l, "FSInfo") || strstr(l, "Lost"))
            (*leaks)++;
        else if (strstr(l, "Unable to write"))
            ;   /* -n: fsck's refusal to apply the fix it just asked for */
        else if (strstr(l, "nvalid long"))
        {
            /* fat.c writes a new name's long entries before its short
             * entry (FAT_ORDERED_DIRENTRIES, never the 8.3 alias alone):
             * a cut between leaves long entries whose checksum names no
             * short entry, which fsck pins on the next file. Readers skip
             * them by checksum; that file still reads (read_all()) */
            printf("  %s: fsck (orphan long-name entries): %s\n", label, l);
            (*leaks)++;
        }
    }
    int st = pclose(p);
    if (st == -1 || (WIFEXITED(st) && WEXITSTATUS(st) == 127))
    {
        printf("  %s: fsck did not run (status %d)\n", label, st);
        return 1;
    }
    return bad;
}

/* every file under 'dir' read to its size through the firmware's layer */
static int read_all(const char *dir, const char *label, int depth)
{
    static char rb[65536];
    int bad = 0;
    DIR *d = rbfs_opendir(dir);
    struct dirent *e;

    if (!d)
    {
        printf("  %s: cannot list %s\n", label, dir);
        return 1;
    }
    while ((e = rbfs_readdir(d)))
    {
        char p[MAX_PATH];

        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        if (e->info.attr & ATTR_DIRECTORY)
        {
            if (depth < 6)
                bad += read_all(p, label, depth + 1);
            continue;
        }
        int fd = rbfs_open(p, O_RDONLY);
        long size = fd >= 0 ? (long)rbfs_ffilesize(fd) : -1, got = 0;
        ssize_t r = 0;
        while (fd >= 0 && (r = rbfs_read(fd, rb, sizeof(rb))) > 0)
            got += r;
        if (fd >= 0)
            rbfs_close(fd);
        files_checked++;
        if (getenv("FSREPLAY_VERBOSE") && atoi(getenv("FSREPLAY_VERBOSE")) > 1)
            printf("  | read %s %ld of %ld\n", p, got, size);
        if (fd < 0 || r < 0 || got != size)
        {
            printf("  %s: UNREADABLE %s: read %ld of %ld (rc %ld)\n", label,
                   p, got, size, (long)r);
            bad++;
        }
    }
    rbfs_closedir(d);
    return bad;
}

static int check_image(const char *label, long k)
{
    int leaks, bad = fsck_image(label, &leaks);

    bad += read_all(ROCKBOX_DIR, label, 0);
    if (bad || getenv("FSREPLAY_VERBOSE"))
        printf("cut k=%ld %s: %s (%d faults, %d leak lines)\n", k, label,
               bad ? "FAIL" : "ok", bad, leaks);
    return bad;
}

static unsigned cut_spc;
static void fresh_image(void)
{
    static bool mounted;

    if (mounted)
        power_off();                /* the last run's cache goes too */
    mounted = true;
    free(img);
    img = NULL;
    mkfs(cut_spc);
    pend_n = jr_n = 0;
    jr_on = 0;
    pend_on = getenv("FSREPLAY_REORDER") != NULL;
    wr_count = 0;
    wr_limit = -1;
    pt_last_wr = pt_prev_wr = pc_wr = -1;
    boots = 0;
    mount();
    rbfs_mkdir(ROCKBOX_DIR);
    rbfs_rename_intent_boot();
}

static int cutmode(const char *trace, unsigned spc)
{
    int *map = cut_map;
    FILE *f;

    for (int i = 0; i < 64; i++)
        map[i] = -1;

    cut_spc = spc;
    if (!access("/sbin/fsck_msdos", X_OK))
        fsck_tool = "/sbin/fsck_msdos";
    else if (!system("command -v fsck.fat >/dev/null 2>&1"))
        fsck_tool = "fsck.fat";
    else
    {
        fprintf(stderr, "fsreplay cut: neither fsck_msdos nor fsck.fat "
                "found: cannot check\n");
        return 2;
    }
    snprintf(img_path, sizeof(img_path), "%s.img", trace);

    /* the dry run: where the window starts and ends */
    fresh_image();
    f = fopen(trace, "r");
    if (!f) { perror(trace); return 2; }
    for (int i = 0; i < 64; i++) map[i] = -1;
    long r = replay_f(f, map, 1);
    fclose(f);
    if (r != -1)
    {
        printf("fsreplay cut: no powercut line in %s (the point was never "
               "reached): nothing checked\n", trace);
        return 2;
    }
    long from = pt_prev_wr >= 0 ? pt_prev_wr : 0, to = pc_wr;
    char name[160];
    snprintf(name, sizeof(name), "%s", pc_name);
    long span = to - from, stride = span > 300 ? span / 300 + 1 : 1;
    int fails = 0, cuts = 0;
    files_checked = 0;
    printf("fsreplay cut %s: writes %ld..%ld (%ld since the point before; "
           "stride %ld), tool %s\n", name, from, to, span, stride,
           fsck_tool);
    for (long k = from; k <= to; k = (k == to) ? to + 1
                                     : (k + stride > to ? to : k + stride))
    {
        int bad;

        fresh_image();
        f = fopen(trace, "r");
        for (int i = 0; i < 64; i++) map[i] = -1;
        wr_limit = k;
        wr_cut_armed = k < to;
        if (!setjmp(wr_cut_jmp))
            replay_f(f, map, 1);
        /* the power is gone: after write k, or at the point itself */
        if (k < to)
        {
            /* the longjmp left the replay mid-line: skip to the cut */
            char l[2048];
            while (fgets(l, sizeof(l), f) && strncmp(l, "powercut\t", 9))
                ;
        }
        pend_on = 0;
        power_off();
        bad = 0;
        /* FSREPLAY_REORDER: the drive cache's other outcomes first (none
         * of the unflushed sectors landed, 4 random subsets), each undone
         * after its check; then all landed, which the next boot follows */
        for (int v = getenv("FSREPLAY_REORDER") ? 1 : 0; v >= 0;
             v = (v == 0) ? -1 : (v == 5 ? 0 : v + 1))
        {
            char label[48];

            jr_n = 0;
            jr_on = v != 0;
            for (size_t i = pend_n; v && i--; )
            {
                unsigned long s = pend[i].sec;
                unsigned h = (unsigned)(s * 2654435761u) ^ (unsigned)v * 40503u;

                if (v == 1 || ((h >> 9) & 1))
                {
                    undo_add(&jr, &jr_n, &jr_cap, s);
                    memcpy(img + s * SECTOR, pend[i].old, SECTOR);
                }
            }
            boot_image();
            snprintf(label, sizeof(label), "after the cut (cache %d)", v);
            bad += check_image(v ? label : "after the cut", k);
            if (v)
            {
                power_off();
                jr_on = 0;
                undo_all(jr, jr_n);
            }
        }
        /* the next boot (if the trace has one): its operations on the
         * image the cut left, then a clean stop */
        for (int i = 0; i < 64; i++) map[i] = -1;
        replay_f(f, map, 0);
        fclose(f);
        for (int i = 0; i < 64; i++)
            if (map[i] >= 0)
            {
                rbfs_close(map[i]);
                map[i] = -1;
            }
        dc_lock_cache();
        dc_commit_all();
        dc_unlock_cache();
        bad += check_image("after the next boot", k);
        cuts++;
        fails += bad != 0;
    }
    remove(img_path);
    printf("fsreplay cut %s: %d cut points, %d failed, %ld file reads\n",
           name, cuts, fails, files_checked);
    if (!files_checked)
    {
        printf("fsreplay cut: no file was read: nothing checked\n");
        return 2;
    }
    return fails ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: fsreplay <trace>|records [spc] [bps]\n");
        return 2;
    }
    if (!strcmp(argv[1], "cut") && argc > 2)
    {
        if (argc > 4)
            bps = (unsigned)atoi(argv[4]);
        return cutmode(argv[2], argc > 3 ? (unsigned)atoi(argv[3]) : 32);
    }
    if (!strcmp(argv[1], "count") && argc > 2)
    {
        /* fsreplay count <trace> [spc] [bps] (#245) */
        cn_mode = 1;
        for (int i = 0; i < 64; i++)
            cn_fdfile[i] = -1;
        argv++;
        argc--;
    }
    unsigned spc = argc > 2 ? (unsigned)atoi(argv[2]) : 32;
    if (argc > 3)
        bps = (unsigned)atoi(argv[3]);
    mkfs(spc);
    mount();
    if (!strcmp(argv[1], "records"))
        return records();
    if (!strcmp(argv[1], "emptyslot"))
        return emptyslot();
    rbfs_mkdir(ROCKBOX_DIR);
    rbfs_rename_intent_boot();
    return replay(argv[1]);
}
