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
 *   fsreplay records [spc] [bps]   the record files are a sector long
 *   fsreplay emptyslot [spc] [bps] a close on an emptied entry fails soft
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
#include "filesystem-native.h"

#define SECTOR 512
static uint8_t *img;
static size_t img_sectors;
static unsigned bps = 2048;

int storage_read_sectors(unsigned long start, int count, void *buf)
{
    if (start + count > img_sectors) return -1;
    memcpy(buf, img + start * SECTOR, (size_t)count * SECTOR);
    return 0;
}

int storage_write_sectors(unsigned long start, int count, const void *buf)
{
    if (start + count > img_sectors) return -1;
    memcpy(img + start * SECTOR, buf, (size_t)count * SECTOR);
    return 0;
}

int storage_flush(void) { return 0; }

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
static int replay(const char *trace)
{
    FILE *f = fopen(trace, "r");
    if (!f) { perror(trace); return 2; }
    int map[64];
    for (int i = 0; i < 64; i++) map[i] = -1;
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
        if (!strcmp(v[0], "open") && n >= 4)
        {
            if (!ours(v[2]) || tfd < 0 || tfd >= 64)
                continue;
            if (map[tfd] >= 0)
                rbfs_close(map[tfd]);
            map[tfd] = rbfs_open(v[2], oflag_of(strtoul(v[3], NULL, 16)));
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
    fclose(f);
    for (int i = 0; i < 64; i++)
        if (map[i] >= 0)
            rbfs_close(map[i]);
    printf("replayed %ld operations, no panic\n", ops);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: fsreplay <trace>|records [spc] [bps]\n");
        return 2;
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
