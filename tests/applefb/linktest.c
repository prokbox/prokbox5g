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
 * Host test for the Apple fallback's steps on the volume (#137): the
 * firmware's own fat.c, file layer and apps/ipodui/afb_fat.c on a FAT32
 * image in RAM (tests/fatcrash's shim). The firmware's half of the
 * tests/fatlink rehearsal:
 *   - a link reads the original's bytes; free space is counted once;
 *   - remove() of a link drops its entry only (the guard in
 *     file_internal.c): the original stays whole; the guard is the link
 *     folder's only (any case): a read-only + hidden file elsewhere, or
 *     a plain file inside, frees its chain as upstream does;
 *   - the park drops the link folder, no chain freed;
 *   - FREED (a host deleted an unlocked link): rechained from the runs;
 *   - EMPTIED (a disk checker cut the original): restored;
 *   - a chain that is not whole is never linked.
 * Saves live.img (one live link) and parked.img (after the park) for
 * run.sh's fsck_msdos -n (the independent oracle).
 *   linktest <mkfs'd image> <outdir>
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
#include "../../apps/ipodui/afb_fat.h"

#define SECTOR 512
static uint8_t *img;
static size_t img_sectors;
static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
    printf("\n"); } } while (0)

int storage_read_sectors(unsigned long start, int count, void *buf)
{
    if (start + count > img_sectors)
        return -1;
    memcpy(buf, img + start * SECTOR, (size_t)count * SECTOR);
    return 0;
}

int storage_write_sectors(unsigned long start, int count, const void *buf)
{
    if (start + count > img_sectors)
        return -1;
    memcpy(img + start * SECTOR, buf, (size_t)count * SECTOR);
    return 0;
}

int storage_flush(void) { return 0; }

void panicf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(3);
}

/* rbunicode / timefuncs, as tests/fatcrash: ASCII names only here */
unsigned char *utf8encode(unsigned long ucs, unsigned char *utf8)
{
    *utf8++ = (unsigned char)(ucs < 0x80 ? ucs : '_');
    return utf8;
}

const unsigned char *utf8decode(const unsigned char *utf8, ucschar_t *ucs)
{
    *ucs = *utf8;
    return utf8 + 1;
}

unsigned long utf16len_utf8(const unsigned char *utf8)
{
    return strlen((const char *)utf8);
}

struct tm *get_time(void)
{
    static struct tm t;
    t.tm_year = 126; t.tm_mon = 10; t.tm_mday = 5; t.tm_hour = 15;
    return &t;
}

void dostime_localtime(time_t t, uint16_t *d, uint16_t *tm)
{
    (void)t;
    *d = (uint16_t)(((2026 - 1980) << 9) | (11 << 5) | 5);
    *tm = (uint16_t)((15 << 11) | (0 << 5));
}
time_t dostime_mktime(uint16_t d, uint16_t t) { (void)d; (void)t; return 0; }

static void mount(void)
{
    filesystem_init();
    fat_init();
    if (fat_mount(0) != 0) { fprintf(stderr, "mount\n"); exit(2); }
    root_mount_path("/", NSITEM_HIDDEN);
    root_mount_path("/", NSITEM_CONTENTS);
}

static void unmount(void)
{
    root_unmount_volume();
    fileobj_mgr_unmount();
    fat_unmount();
}

static void save(const char *dir, const char *name)
{
    char p[512];
    FILE *f;

    snprintf(p, sizeof(p), "%s/%s", dir, name);
    f = fopen(p, "wb");
    if (!f || fwrite(img, SECTOR, img_sectors, f) != img_sectors)
    { perror(p); exit(2); }
    fclose(f);
}

/* the image's FAT (first copy) and its geometry, for a host's edits */
static uint32_t rsvd, nfats, fatsz;
static void geometry(void)
{
    rsvd = img[14] | img[15] << 8;
    nfats = img[16];
    memcpy(&fatsz, img + 36, 4);
}

static void host_free_chain(const uint32_t *runs, int n)
{
    for (int i = 0; i < n; i++)
        for (uint32_t k = 0; k < runs[2 * i + 1]; k++)
            for (uint32_t f = 0; f < nfats; f++)
                memset(img + (rsvd + f * fatsz) * SECTOR
                       + 4 * (runs[2 * i] + k), 0, 4);
}

/* set read-only + hidden on the 8.3 entry name11 ("NAME    EXT"); the
 * volume unmounted: the file layer has no word for attributes */
static int set_rh(const char *name11)
{
    int n = 0;

    for (size_t o = 0; o + 32 <= img_sectors * SECTOR; o += 32)
        if (!memcmp(img + o, name11, 11) && !(img[o + 11] & 0x10))
            img[o + 11] |= 0x01 | 0x02, n++;
    return n;
}

static uint8_t data[200000];
static uint8_t back[200000 + 16];

static void put(const char *path, const uint8_t *b, size_t n)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0 || write(fd, b, n) != (ssize_t)n || close(fd) < 0)
    { fprintf(stderr, "put %s\n", path); exit(2); }
}

static bool same(const char *path, const uint8_t *b, size_t n)
{
    int fd = open(path, O_RDONLY);
    ssize_t got;

    if (fd < 0)
        return false;
    got = read(fd, back, sizeof(back));
    close(fd);
    return got == (ssize_t)n && !memcmp(back, b, n);
}

static uint32_t free_clusters(void)
{
    unsigned long fc = 0, nf = 0, cl = 0;

    fat_recalc_free(IF_MV(0));
    fat_get_fsinfo(IF_MV(0,) &fc, &nf, &cl);
    return fc;
}

#define ORIG "/Music/Peaches!/01 Song One.mp3"
#define LINK AFB_LINK_DIR "/P0000001.MP3"

int main(int argc, char **argv)
{
    struct afb_probe o, l;
    FILE *f;
    long n;
    uint32_t free0, free1;

    if (argc != 3)
        return 2;
    f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    img = malloc(n);
    img_sectors = n / SECTOR;
    if (fread(img, 1, n, f) != (size_t)n) return 2;
    fclose(f);
    geometry();
    srand(137);
    for (size_t i = 0; i < sizeof(data); i++)
        data[i] = rand();

    mount();
    mkdir("/Music");
    mkdir("/Music/Peaches!");
    mkdir("/iPod_Control");
    mkdir(AFB_MUSIC_DIR);
    mkdir(AFB_LINK_DIR);
    put(ORIG, data, sizeof(data));
    free0 = free_clusters();

    /* probe: a whole chain, its runs */
    CHECK(afb_vol_probe(ORIG, &o, true) == 0 && o.exists
          && o.size == sizeof(data) && o.healthy == 1 && o.nruns >= 1,
          "probe orig exists=%d size=%u healthy=%d nruns=%d", o.exists,
          o.size, o.healthy, o.nruns);

    /* a link reads the original's bytes; space is counted once */
    CHECK(afb_vol_link("P0000001.MP3", o.fst, o.size) == 0, "link");
    CHECK(same(LINK, data, sizeof(data)), "link reads the original");
    free1 = free_clusters();
    CHECK(free1 == free0, "free space counted once (%u vs %u)", free1,
          free0);
    CHECK(afb_vol_probe(LINK, &l, false) == 0 && l.exists && l.fst == o.fst
          && l.size == o.size, "link entry");
    unmount();
    save(argv[2], "live.img");
    mount();

    /* remove() of a link: its entry only (the file layer's guard) */
    CHECK(remove(LINK) == 0, "remove link");
    CHECK(!file_exists(LINK), "link gone");
    CHECK(same(ORIG, data, sizeof(data)), "orig whole after remove(link)");
    CHECK(afb_vol_probe(ORIG, &o, true) == 0 && o.healthy == 1,
          "orig chain whole after remove(link)");
    CHECK(free_clusters() == free0, "nothing freed by remove(link)");

    /* the guard is the link folder's only: a read-only + hidden file
     * anywhere else removes as upstream (its chain freed) */
    {
        uint32_t f1, f2;

        f1 = free_clusters();
        put("/Music/RHFILE.BIN", data, 20000);
        put(AFB_LINK_DIR "/PLAIN.BIN", data, 20000);
        unmount();
        CHECK(set_rh("RHFILE  BIN") == 1, "RH set outside the folder");
        mount();
        f2 = free_clusters();
        CHECK(f2 < f1, "the two files hold clusters");
        CHECK(remove("/Music/RHFILE.BIN") == 0, "remove RH file outside");
        CHECK(free_clusters() > f2, "RH outside the folder: chain freed");
        /* inside the folder, a plain (not RH) file frees as upstream */
        CHECK(remove(AFB_LINK_DIR "/PLAIN.BIN") == 0, "remove plain inside");
        CHECK(free_clusters() == f1, "plain inside: chain freed (%u vs %u)",
              free_clusters(), f1);
        /* inside the folder, matched case-insensitively: entry only */
        CHECK(afb_vol_link("P0000003.MP3", o.fst, o.size) == 0, "link 3");
        CHECK(remove("/IPOD_CONTROL/music/pkb/P0000003.MP3") == 0,
              "remove link, other case");
        CHECK(!file_exists(AFB_LINK_DIR "/P0000003.MP3"), "link 3 gone");
        CHECK(free_clusters() == f1, "link inside, any case: none freed");
        CHECK(afb_vol_probe(ORIG, &o, true) == 0 && o.healthy == 1,
              "orig chain whole after the case check");
    }

    /* the park: the folder and every link in it; no chain freed */
    CHECK(afb_vol_link("P0000001.MP3", o.fst, o.size) == 0, "relink");
    CHECK(afb_vol_link("P0000002.MP3", o.fst, o.size) == 0, "2nd link");
    CHECK(afb_vol_park() == 0, "park");
    CHECK(!dir_exists(AFB_LINK_DIR), "link folder gone");
    CHECK(afb_vol_park() == 1, "park again: nothing");
    CHECK(same(ORIG, data, sizeof(data)), "orig whole after the park");
    CHECK(afb_vol_probe(ORIG, &o, true) == 0 && o.healthy == 1,
          "orig chain whole after the park");
    unmount();
    save(argv[2], "parked.img");

    /* FREED: a host deleted an unlocked link and so freed the chain */
    host_free_chain(o.runs, o.nruns);
    mount();
    {
        struct afb_probe b;

        CHECK(afb_vol_probe(ORIG, &b, true) == 0 && b.healthy == 0,
              "freed chain is not whole (%d)", b.healthy);
        CHECK(afb_vol_runs_free(o.runs, o.nruns) == 1, "runs free");
        CHECK(afb_vol_rechain(o.runs, o.nruns) == 0, "rechain");
        CHECK(afb_vol_probe(ORIG, &b, true) == 0 && b.healthy == 1,
              "chain whole again");
        CHECK(same(ORIG, data, sizeof(data)), "orig whole after rechain");
        CHECK(afb_vol_rechain(o.runs, o.nruns) < 0,
              "never rechain onto clusters in use");
    }

    /* EMPTIED: a disk checker cut the original to nothing */
    CHECK(afb_vol_set(ORIG, 0, 0) == 0, "empty (as fsck -y)");
    {
        struct afb_probe b;

        CHECK(afb_vol_probe(ORIG, &b, true) == 0 && b.fst == 0 && b.size == 0,
              "emptied");
    }
    CHECK(afb_vol_set(ORIG, o.fst, o.size) == 0, "restore");
    CHECK(same(ORIG, data, sizeof(data)), "orig whole after restore");

    /* a chain shorter than its size is never linked (probe says so) */
    {
        struct afb_probe b;
        uint32_t last = o.runs[2 * (o.nruns - 1)]
                        + o.runs[2 * (o.nruns - 1) + 1] - 1;

        unmount();
        for (uint32_t fc = 0; fc < nfats; fc++)
            memset(img + (rsvd + fc * fatsz) * SECTOR + 4 * (last - 1), 0xff,
                   3), img[(rsvd + fc * fatsz) * SECTOR + 4 * (last - 1) + 3]
                       = 0x0f;
        mount();
        CHECK(afb_vol_probe(ORIG, &b, true) == 0 && b.healthy == 0,
              "short chain not whole");
        unmount();
    }
    printf("applefb links: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
