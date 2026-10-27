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
/* tests/fatcrash: crash-consistency model of firmware/common/fat.c and
 * disk_cache.c (the firmware's own sources, compiled for the host).
 *
 * A FAT32 image is held in RAM. "setup" builds the pre-crash state the
 * device had (.rockbox/artcache with index.dat, index.prev, thumbs.pak, a
 * bundle directory, .rockbox/.resume.cfg + .new + .old), commits, and
 * saves the base image. "record <scenario>" then runs one scenario
 * through the same fat.c calls file.c makes and logs every sector write in
 * order. "apply <k>" writes base + the first k logged writes: the disk as
 * a halt (panic, data abort, hard reset) after the k-th write leaves it.
 * "post" remounts such an image and allocates (what the next boot does),
 * which turns "starts with a free cluster" into a cross-link.
 * run.sh runs fsck_msdos -n on every prefix and tallies what it reports.
 *
 * Scenarios (file.c call order, see the report in
 * docs/handoff-2026-10-19-crash-fat.md):
 *   old-publish  7868773a30 index_write(): index.tmp written + closed, then
 *                rename(index.tmp, index.dat) over the old index.dat
 *                (fat_rename, then the victim's FAT_RM_DIRENTRIES, then its
 *                FAT_RM_DATA at close)
 *   new-publish  2ba025a068 artfile_publish(): rename(index.dat,
 *                index.prev) over index.prev, then rename(index.tmp,
 *                index.dat)
 *   slot         write_slot(): thumbs.pak extended in place by one slot
 *   bundle       a new bundle directory (mkdir artcache/69) + XX.ART.tmp
 *                written, closed, renamed to XX.ART
 *   resume       settings_load()'s rename_temp_file() at boot:
 *                rename(.resume.cfg, .resume.cfg.old) over .old, then
 *                rename(.resume.cfg.new, .resume.cfg)
 *   all          the art builder's commit the device logged at 19:33-34:
 *                bundle + slot + new-publish
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#include "fat.h"
#include "fs_attr.h"
#include "disk_cache.h"
#include "file_internal.h"
#ifdef FATCRASH_FILE_LAYER
/* run.sh's default build: the renames go through file.c's rbfs_rename()
 * (the rename intent record, #91) and "post" runs the real boot recovery,
 * rbfs_rename_intent_boot(), before the next boot's first allocation */
#include "file.h"
#include "fileobj_mgr.h"
#include "rb_namespace.h"
#endif

#define SECTOR 512

static uint8_t *img;
static size_t img_sectors;
static FILE *wlog;
static unsigned long nwrites;

/* ---- the storage driver: RAM image, every write logged ---- */
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
    for (int i = 0; i < count; i++)
    {
        uint32_t s = (uint32_t)(start + i);
        if (wlog)
        {
            fwrite(&s, sizeof(s), 1, wlog);
            fwrite((const uint8_t *)buf + i * SECTOR, SECTOR, 1, wlog);
        }
        nwrites++;
    }
    memcpy(img + start * SECTOR, buf, (size_t)count * SECTOR);
    return 0;
}

/* in-order model: every write lands as issued; a flush has nothing to do */
int storage_flush(void) { return 0; }

void panicf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "PANIC: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(3);
}

/* rbunicode / timefuncs: ASCII names only here */
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

/* timefuncs: a fixed RTC time */
struct tm *get_time(void)
{
    static struct tm t;
    t.tm_year = 126; t.tm_mon = 9; t.tm_mday = 19; t.tm_hour = 13;
    t.tm_min = 24;
    return &t;
}

void dostime_localtime(time_t t, uint16_t *d, uint16_t *tm)
{
    (void)t;
    *d = (uint16_t)(((2026 - 1980) << 9) | (10 << 5) | 18);
    *tm = (uint16_t)((19 << 11) | (34 << 5));
}

#ifdef FATCRASH_FILE_LAYER
time_t dostime_mktime(uint16_t d, uint16_t t) { (void)d; (void)t; return 0; }
#endif

/* ---- image I/O ---- */
static void load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    img = malloc((size_t)n);
    img_sectors = (size_t)n / SECTOR;
    if (fread(img, 1, (size_t)n, f) != (size_t)n) { perror("read"); exit(2); }
    fclose(f);
}

static void save(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(img, SECTOR, img_sectors, f) != img_sectors)
    { perror(path); exit(2); }
    fclose(f);
}

/* a minimal FAT32 format (no partition table): 512 B sectors, 2 sectors
 * per cluster, 32 reserved sectors, 2 FATs, root at cluster 2, FAT[1]
 * clean (0x0fffffff) */
static void mkfs(const char *path, unsigned long nsect)
{
    const unsigned rsvd = 32, spc = 2, nfats = 2;
    unsigned long clusters = nsect / spc, fatsz;
    uint8_t *b;

    img_sectors = nsect;
    img = calloc(nsect, SECTOR);
    fatsz = (clusters * 4 + SECTOR - 1) / SECTOR;
    clusters = (nsect - rsvd - nfats * fatsz) / spc;
    b = img;
    b[0] = 0xeb; b[1] = 0x58; b[2] = 0x90;
    memcpy(b + 3, "FATCRASH", 8);
    b[11] = 0x00; b[12] = 0x02;                 /* 512 B/sector */
    b[13] = spc;
    b[14] = rsvd; b[15] = 0;
    b[16] = nfats;
    b[21] = 0xf8;
    b[24] = 63; b[26] = 16;
    memcpy(b + 32, &(uint32_t){ (uint32_t)nsect }, 4);
    memcpy(b + 36, &(uint32_t){ (uint32_t)fatsz }, 4);
    memcpy(b + 44, &(uint32_t){ 2 }, 4);        /* root cluster */
    b[48] = 1;                                  /* FSInfo sector */
    b[50] = 6;                                  /* backup boot sector */
    b[64] = 0x80; b[66] = 0x29;
    memcpy(b + 71, "FATCRASH   ", 11);
    memcpy(b + 82, "FAT32   ", 8);
    b[510] = 0x55; b[511] = 0xaa;
    memcpy(img + 6 * SECTOR, b, SECTOR);
    b = img + SECTOR;                           /* FSInfo */
    memcpy(b, "RRaA", 4);
    memcpy(b + 484, "rrAa", 4);
    memcpy(b + 488, &(uint32_t){ (uint32_t)(clusters - 1) }, 4);
    memcpy(b + 492, &(uint32_t){ 3 }, 4);
    b[510] = 0x55; b[511] = 0xaa;
    for (unsigned f = 0; f < nfats; f++)
    {
        uint32_t *fat = (uint32_t *)(img + (rsvd + f * fatsz) * SECTOR);
        fat[0] = 0x0ffffff8;
        fat[1] = 0x0fffffff;
        fat[2] = 0x0fffffff;                    /* root directory */
    }
    save(path);
}

static void mount(void)
{
#ifdef FATCRASH_FILE_LAYER
    filesystem_init();
#else
    dc_init();
#endif
    fat_init();
    if (fat_mount(0) < 0) { fprintf(stderr, "mount failed\n"); exit(2); }
#ifdef FATCRASH_FILE_LAYER
    root_mount_path("/", NSITEM_HIDDEN);
    root_mount_path("/", NSITEM_CONTENTS);
#endif
}

/* ---- file.c-shaped helpers over fat.c ---- */
static struct fat_file root;
static struct fat_file rbdir, artdir;
/* the source's name and a bundle directory's path, for the file layer */
static const char *src_name = "";
static const char *bundle_path = "/.rockbox/artcache/69";


static void must(int rc, const char *what)
{
    if (rc < 0) { fprintf(stderr, "%s failed: %d\n", what, rc); exit(2); }
}

/* look a name up in a directory (setup state is rebuilt by name) */
static bool lookup(struct fat_file *dir, const char *name, struct fat_file *out,
                   uint32_t *size)
{
    struct fat_filestr ds;
    struct fat_dirscan_info scan;
    struct fat_direntry ent;
    static uint8_t cbuf[SECTOR];
    struct filestr_cache cache;

    memset(&cache, 0, sizeof(cache));
    cache.buffer = cbuf;
    cache.sector = (sector_t)-1;        /* file_cache_reset(): nothing cached */
    fat_filestr_init(&ds, dir);
    fat_rewinddir(&scan);
    while (fat_readdir(&ds, &scan, &cache, &ent) > 0)
    {
        if (!ent.name[0])
            break;
        if (!strcmp((const char *)ent.name, name))
        {
            fat_open(dir, ent.firstcluster, out);
            out->dircluster = dir->firstcluster;
            out->e = scan;
            if (size)
                *size = ent.filesize;
            return true;
        }
    }
    return false;
}

static void need(struct fat_file *dir, const char *name, struct fat_file *out,
                 uint32_t *size)
{
    if (!lookup(dir, name, out, size))
    { fprintf(stderr, "missing %s\n", name); exit(2); }
    src_name = name;
}

static void create(struct fat_file *dir, const char *name, uint8_t attr,
                   struct fat_file *out)
{
    must(fat_create_file(dir, name, attr, out, NULL), name);
}

/* write 'bytes' at sector 'at' (extending as needed), then close: what
 * file.c's write() + close() (fsync_internal -> fat_closewrite) do */
static void write_close(struct fat_file *f, unsigned long at, uint32_t bytes,
                        uint32_t newsize, uint8_t fill)
{
    struct fat_filestr s;
    static uint8_t buf[64 * SECTOR];
    unsigned long n = (bytes + SECTOR - 1) / SECTOR;

    memset(buf, fill, sizeof(buf));
    fat_filestr_init(&s, f);
    if (at)
        must(fat_seek(&s, at), "seek");
    while (n)
    {
        unsigned long k = n > 64 ? 64 : n;
        if (fat_readwrite(&s, k, buf, true) != (long)k)
        { fprintf(stderr, "write failed\n"); exit(2); }
        n -= k;
    }
    /* file.c passes its dir-entry scratch (times come from it) */
    static struct fat_direntry fe;
    memset(&fe, 0, sizeof(fe));
    must(fat_closewrite(&s, newsize, &fe), "closewrite");
}

/* file.c rbfs_rename(): fat_rename(), then for an overwrite the victim's
 * remove_stream_internal() (dir entries) and close_stream_internal()
 * (data, last reference) */
static void rename_over(struct fat_file *dir, struct fat_file *src,
                        const char *newname)
{
#ifdef FATCRASH_FILE_LAYER
    /* the file layer by path: src's name is looked up again */
    char oldp[300], newp[300];
    const char *dirp = dir == &artdir ? "/.rockbox/artcache"
                     : dir == &rbdir ? "/.rockbox" : NULL;
    if (!dirp)
        dirp = bundle_path;
    snprintf(newp, sizeof(newp), "%s/%s", dirp, newname);
    snprintf(oldp, sizeof(oldp), "%s/%s", dirp, src_name);
    (void)src;
    if (rbfs_rename(oldp, newp) < 0)
    { fprintf(stderr, "rename %s failed\n", oldp); exit(2); }
    return;
#endif
    struct fat_file victim;
    bool over = lookup(dir, newname, &victim, NULL);

    must(fat_rename(dir, src, (const unsigned char *)newname), "rename");
    if (over)
    {
        must(fat_remove(&victim, FAT_RM_DIRENTRIES), "rm dirent");
        must(fat_remove(&victim, FAT_RM_DATA), "rm data");
    }
}

/* ---- the device's pre-crash state ---- */

static void open_dirs(void)
{
    must(fat_open_rootdir(&root), "root");
    need(&root, ".rockbox", &rbdir, NULL);
    need(&rbdir, "artcache", &artdir, NULL);
}

static void setup(void)
{
    struct fat_file f, d;

    must(fat_open_rootdir(&root), "root");
    create(&root, ".rockbox", ATTR_DIRECTORY, &rbdir);
    create(&rbdir, "artcache", ATTR_DIRECTORY, &artdir);
    /* some unrelated user files first, so art clusters sit mid-volume */
    create(&root, "Music", ATTR_DIRECTORY, &d);
    for (int i = 0; i < 6; i++)
    {
        char n[32];
        snprintf(n, sizeof(n), "track%02d.flac", i);
        create(&d, n, 0, &f);
        write_close(&f, 0, 200 * 1024, 200 * 1024, 0x40 + i);
    }
    /* the bundle directories (151 on the device): enough entries that a
     * new name lands in another directory sector than the old one */
    for (int i = 0; i < 40; i++)
    {
        char n[8];
        snprintf(n, sizeof(n), "%02X", i);
        create(&artdir, n, ATTR_DIRECTORY, &d);
    }
    create(&rbdir, ".resume.cfg.old", 0, &f);
    write_close(&f, 0, 600, 600, 'o');
    create(&rbdir, ".resume.cfg", 0, &f);
    write_close(&f, 0, 610, 610, 'r');
    create(&rbdir, ".resume.cfg.new", 0, &f);
    write_close(&f, 0, 620, 620, 'n');
    create(&artdir, "index.prev", 0, &f);
    write_close(&f, 0, 4608, 4608, 'p');
    create(&artdir, "index.dat", 0, &f);
    write_close(&f, 0, 5120, 5120, 'i');
    create(&artdir, "thumbs.pak", 0, &f);
    write_close(&f, 0, 1300 * 1024 / 1024 * 1024, 1300 * 1024, 't');
    create(&artdir, "78", ATTR_DIRECTORY, &d);
    create(&d, "78BF7B39.ART", 0, &f);
    write_close(&f, 0, 51750, 51750, 'a');
}

/* ---- scenarios ---- */
static void sc_index_tmp(void)
{
    struct fat_file t;
    /* artidx_write_merged(): O_CREAT|O_TRUNC, header + records, the real
     * header rewritten at 0, close */
    create(&artdir, "index.tmp", 0, &t);
    write_close(&t, 0, 5632, 5632, 'T');
}

static void sc_old_publish(void)
{
    struct fat_file t;
    sc_index_tmp();
    need(&artdir, "index.tmp", &t, NULL);
    rename_over(&artdir, &t, "index.dat");
}

static void sc_new_publish(void)
{
    struct fat_file t, cur;
    sc_index_tmp();
    need(&artdir, "index.dat", &cur, NULL);
    rename_over(&artdir, &cur, "index.prev");
    need(&artdir, "index.tmp", &t, NULL);
    rename_over(&artdir, &t, "index.dat");
}

static void sc_slot(void)
{
    struct fat_file p;
    uint32_t size;
    need(&artdir, "thumbs.pak", &p, &size);
    /* one 55x55 RGB565 slot (6,050 B) appended at the end */
    write_close(&p, size / SECTOR, 6144, size + 6144, 'S');
}

static void sc_bundle(void)
{
    struct fat_file d, t;
    create(&artdir, "69", ATTR_DIRECTORY, &d);
    create(&d, "69ABCDEF.ART.tmp", 0, &t);
    write_close(&t, 0, 51750, 51750, 'B');
    need(&d, "69ABCDEF.ART.tmp", &t, NULL);
    rename_over(&d, &t, "69ABCDEF.ART");
}

static void sc_resume(void)
{
    struct fat_file cur, nw;
    need(&rbdir, ".resume.cfg", &cur, NULL);
    rename_over(&rbdir, &cur, ".resume.cfg.old");
    need(&rbdir, ".resume.cfg.new", &nw, NULL);
    rename_over(&rbdir, &nw, ".resume.cfg");
}

/* the next boot: allocate from the FSInfo hint, as any first write does */
static void post(void)
{
    struct fat_file f;
#ifdef FATCRASH_FILE_LAYER
    rbfs_rename_intent_boot();
#endif
    open_dirs();
    create(&rbdir, "after.bin", 0, &f);
    write_close(&f, 0, 64 * 1024, 64 * 1024, 'Z');
}

/* "aliases <img>": entries sharing a start cluster, read off the raw
 * image. A pair with equal size and attributes is one file under two
 * names, what a halt inside a rename leaves (fat.c FAT_ORDERED_DIRENTRIES;
 * the shell's recovery drops the second name, tests/fatcrash/safetest).
 * Prints "alias <pairs> other <pairs>"; run.sh tags such images "alias"
 * instead of "crosslink" when no other pair shares a cluster. */
static int aliases(void)
{
    const uint8_t *b = img;
    uint32_t rsvd = b[14] | b[15] << 8, spc = b[13], nfats = b[16];
    uint32_t fatsz, root, nclus;
    memcpy(&fatsz, b + 36, 4);
    memcpy(&root, b + 44, 4);
    nclus = (uint32_t)((img_sectors - rsvd - nfats * fatsz) / spc);
    struct ent { uint32_t start, size; uint8_t attr; } *e = NULL;
    size_t ne = 0, cap = 0;
    uint32_t dirs[256];
    int nd = 0, alias = 0, other = 0;
    dirs[nd++] = root;
    for (int d = 0; d < nd; d++)
    {
        for (uint32_t c = dirs[d], n = 0; c >= 2 && c < 0x0ffffff8 && n <= nclus;
             n++)
        {
            const uint8_t *p = img + (rsvd + nfats * fatsz
                                      + (size_t)(c - 2) * spc) * SECTOR;
            for (uint32_t i = 0; i < spc * SECTOR / 32; i++, p += 32)
            {
                if (p[0] == 0)
                    goto next_dir;
                if (p[0] == 0xe5 || (p[11] & 0x3f) == 0x0f || p[0] == '.')
                    continue;
                uint32_t st = (uint32_t)(p[20] | p[21] << 8) << 16
                            | (uint32_t)(p[26] | p[27] << 8);
                if (!st)
                    continue;
                if (ne == cap)
                    e = realloc(e, (cap = cap ? cap * 2 : 256) * sizeof(*e));
                e[ne].start = st;
                memcpy(&e[ne].size, p + 28, 4);
                e[ne].attr = p[11];
                ne++;
                if ((p[11] & 0x10) && nd < 256)
                    dirs[nd++] = st;
            }
            uint32_t v;
            memcpy(&v, img + rsvd * SECTOR + (size_t)c * 4, 4);
            c = v & 0x0fffffff;
        }
    next_dir:;
    }
    for (size_t i = 0; i < ne; i++)
        for (size_t j = i + 1; j < ne; j++)
            if (e[i].start == e[j].start)
            {
                if (e[i].size == e[j].size && e[i].attr == e[j].attr)
                    alias++;
                else
                    other++;
            }
    printf("alias %d other %d\n", alias, other);
    free(e);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: fatcrash setup <img> | record <img> <log> "
                "<scenario> | apply <base> <log> <k> <out> | post <img>\n");
        return 2;
    }
    if (!strcmp(argv[1], "mkfs") && argc == 4)
    {
        mkfs(argv[2], strtoul(argv[3], NULL, 0));
        return 0;
    }
    if (!strcmp(argv[1], "aliases"))
    {
        load(argv[2]);
        return aliases();
    }
    if (!strcmp(argv[1], "setup"))
    {
        load(argv[2]);
        mount();
        setup();
#ifdef FATCRASH_FILE_LAYER
        rbfs_rename_intent_boot();      /* the first boot makes the record */
#endif
        save(argv[2]);
        return 0;
    }
    if (!strcmp(argv[1], "record") && argc == 5)
    {
        const char *sc = argv[4];
        load(argv[2]);
        mount();
        open_dirs();
        if (getenv("FATCRASH_SEED"))
        {
            /* vary which cache slots the scenario's sectors get (the
             * commit writes in slot order): pre-load random sectors */
            unsigned seed = (unsigned)strtoul(getenv("FATCRASH_SEED"), NULL, 0);
            srand(seed);
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
        wlog = fopen(argv[3], "wb");
        nwrites = 0;
        if (!strcmp(sc, "old-publish")) sc_old_publish();
        else if (!strcmp(sc, "new-publish")) sc_new_publish();
        else if (!strcmp(sc, "slot")) sc_slot();
        else if (!strcmp(sc, "bundle")) sc_bundle();
        else if (!strcmp(sc, "resume")) sc_resume();
        else if (!strcmp(sc, "all")) { sc_bundle(); sc_slot(); sc_new_publish(); }
        else { fprintf(stderr, "unknown scenario %s\n", sc); return 2; }
        fclose(wlog);
        printf("%lu\n", nwrites);
        return 0;
    }
    if (!strcmp(argv[1], "apply") && argc == 6)
    {
        FILE *f;
        unsigned long k = strtoul(argv[4], NULL, 0);
        uint32_t s;
        uint8_t buf[SECTOR];
        load(argv[2]);
        f = fopen(argv[3], "rb");
        if (!f) { perror(argv[3]); return 2; }
        for (unsigned long i = 0; i < k; i++)
        {
            if (fread(&s, sizeof(s), 1, f) != 1 || fread(buf, SECTOR, 1, f) != 1)
                break;
            memcpy(img + (size_t)s * SECTOR, buf, SECTOR);
        }
        fclose(f);
        save(argv[5]);
        return 0;
    }
    if (!strcmp(argv[1], "post"))
    {
        load(argv[2]);
        mount();
        post();
        save(argv[2]);
        return 0;
    }
    return 2;
}
