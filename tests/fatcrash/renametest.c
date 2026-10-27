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
/* tests/fatcrash/renametest: the firmware's file layer (file.c,
 * file_internal.c, fileobj_mgr.c, dir.c, rb_namespace.c over fat.c and
 * disk_cache.c) on a RAM FAT32 image, no crash: does a publish by
 * rename-over leave one directory entry per name?
 *
 *   renametest <img> <case>
 * Cases (each runs N publishes the way artfmt.c artfile_publish() does:
 * rename(dat, prev) over prev, then rename(tmp, dat)):
 *   plain    nothing else open
 *   reader   another fd holds index.dat open across each publish, and
 *            reopens it (or index.prev when index.dat is missing), as the
 *            UI's art reader does (artcache.c reader_sync())
 *   errno    open() of a missing file must leave errno == ENOENT
 *   victimfail  the victim's directory-entry removal fails inside a
 *            rename-over (file_internal.c reaches fat_remove() through
 *            rt_fat_remove() below): the rename must fail with one entry
 *            per name left, the source still under its old name and both
 *            files' bytes intact (same directory, a move, a reader open)
 * Exit 0 when every name appears once and fsck_msdos (run by run.sh)
 * finds nothing; 1 with the first duplicate printed.
 */
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

#define SECTOR 512
static uint8_t *img;
static size_t img_sectors;

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
/* timefuncs: a fixed RTC time */
struct tm *get_time(void)
{
    static struct tm t;
    t.tm_year = 126; t.tm_mon = 9; t.tm_mday = 19; t.tm_hour = 13;
    t.tm_min = 24;
    return &t;
}

void dostime_localtime(time_t t, uint16_t *d, uint16_t *tm)
{ (void)t; *d = (uint16_t)((46 << 9) | (10 << 5) | 19); *tm = (uint16_t)(13 << 11); }
time_t dostime_mktime(uint16_t d, uint16_t t) { (void)d; (void)t; return 0; }

/* file_internal.c is built with -Dfat_remove=rt_fat_remove (run.sh): fail
 * the next rt_fail_dirent removals of directory entries, as an I/O error
 * reading the victim's directory sector would */
static int rt_fail_dirent;
int rt_fat_remove(struct fat_file *file, enum fat_remove_op what)
{
    if ((what & FAT_RM_DIRENTRIES) && rt_fail_dirent > 0)
    {
        rt_fail_dirent--;
        return -3;
    }
    return fat_remove(file, what);
}

static void load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    img = malloc((size_t)n);
    img_sectors = (size_t)n / SECTOR;
    if (fread(img, 1, (size_t)n, f) != (size_t)n) exit(2);
    fclose(f);
}

static void save(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(img, SECTOR, img_sectors, f) != img_sectors) exit(2);
    fclose(f);
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

/* every name in a directory at most once */
static int dup_check(const char *dirpath)
{
    char names[512][64];
    int n = 0, bad = 0;
    DIR *d = rbfs_opendir(dirpath);
    struct dirent *e;
    if (!d) { fprintf(stderr, "opendir %s\n", dirpath); return 1; }
    while ((e = rbfs_readdir(d)))
    {
        for (int i = 0; i < n; i++)
            if (!strcmp(names[i], e->d_name))
            {
                printf("DUPLICATE %s/%s\n", dirpath, e->d_name);
                bad = 1;
            }
        if (n < 512)
            snprintf(names[n++], sizeof(names[0]), "%s", e->d_name);
    }
    rbfs_closedir(d);
    return bad;
}

/* the file holds n bytes of 'fill' and nothing else */
static int holds(const char *path, size_t n, int fill)
{
    char b[512];
    size_t got = 0;
    ssize_t k;
    int fd = rbfs_open(path, O_RDONLY);
    if (fd < 0)
    {
        printf("%s: missing (errno=%d)\n", path, errno);
        return 0;
    }
    while ((k = rbfs_read(fd, b, sizeof(b))) > 0)
    {
        for (ssize_t i = 0; i < k; i++)
            if (b[i] != fill)
            {
                printf("%s: byte %zu is %02x, want %02x\n", path, got + i,
                       (unsigned char)b[i], fill);
                rbfs_close(fd);
                return 0;
            }
        got += (size_t)k;
    }
    rbfs_close(fd);
    if (got != n)
        printf("%s: %zu bytes, want %zu\n", path, got, n);
    return got == n;
}

#define D   "/.rockbox/artcache"
#define DAT D "/index.dat"
#define TMP D "/index.tmp"
#define PRV D "/index.prev"

static int overwrites;               /* renames onto an existing name */

static int rn(const char *a, const char *b)
{
    if (rbfs_file_exists(b))
        overwrites++;
    return rbfs_rename(a, b);
}

static int publish(void)
{
    /* apps/ipodui/artfmt.c artfile_publish() (2026-10-19: prev removed
     * first while the primary exists, so no rename lands on a name) */
    if (!getenv("RENAMETEST_OLD_PUBLISH")     /* 2ba025a068 order */
        && rbfs_file_exists(DAT) && rbfs_remove(PRV) != 0 && errno != ENOENT)
        return -1;
    bool moved = rn(DAT, PRV) == 0;
    if (!moved && errno != ENOENT)
        return -1;
    if (rn(TMP, DAT) == 0)
        return 0;
    if (moved)
        rn(PRV, DAT);
    return -1;
}

int main(int argc, char **argv)
{
    int bad = 0, reader = -1;
    if (argc < 3) return 2;
    load(argv[1]);
    filesystem_init();
    fat_init();
    if (fat_mount(0) != 0) { fprintf(stderr, "mount\n"); return 2; }
    /* dircache_redirect.h volume_onmount_internal(), one volume */
    root_mount_path("/", NSITEM_HIDDEN);
    root_mount_path("/", NSITEM_CONTENTS);

    rbfs_mkdir("/.rockbox");
    rbfs_mkdir(D);
    for (int i = 0; i < 40; i++)        /* the bundle directories */
    {
        char p[64];
        snprintf(p, sizeof(p), D "/%02X", i);
        rbfs_mkdir(p);
    }
    if (!strcmp(argv[2], "errno"))
    {
        errno = 0;
        int fd = rbfs_open(D "/missing.dat", O_RDONLY);
        printf("open(missing) = %d errno=%d (%s)\n", fd, errno,
               errno == ENOENT ? "ENOENT" : errno == EBADF ? "EBADF" : "?");
        return (fd < 0 && errno == ENOENT) ? 0 : 1;
    }
    put(PRV, 4608, 'p');
    put(DAT, 5120, 'i');
    put(D "/thumbs.pak", 65536, 't');
    if (!strcmp(argv[2], "victimfail"))
    {
        int rc, fd;
        /* 1: same directory, nothing else open */
        rt_fail_dirent = 1;
        rc = rbfs_rename(DAT, PRV);
        printf("rename over, victim removal failing: rc=%d errno=%d\n",
               rc, errno);
#ifdef HAVE_IPOD_UI
        struct rbfs_rename_fault rf;
        rbfs_rename_fault_take(&rf);
        printf("fault rc=%d undo=%d\n", rf.rc, rf.undo);
        /* undo 1: the source got its old name back; 0: the rename
         * intent build removes the victim first, so nothing to undo */
        bad |= rf.rc >= 0 || rf.undo < 0;
#endif
        bad |= rc >= 0;
        bad |= dup_check(D);
        bad |= !holds(DAT, 5120, 'i') || !holds(PRV, 4608, 'p');
        /* 2: a move into another directory, onto an existing name */
        put(D "/00/move.tmp", 3000, 'm');
        put(D "/move.dat", 2000, 'o');
        rt_fail_dirent = 1;
        rc = rbfs_rename(D "/00/move.tmp", D "/move.dat");
        printf("move over, victim removal failing: rc=%d\n", rc);
        bad |= rc >= 0;
        bad |= dup_check(D) | dup_check(D "/00");
        bad |= !holds(D "/00/move.tmp", 3000, 'm')
               || !holds(D "/move.dat", 2000, 'o');
        /* 3: a reader holds the source open across the failed rename */
        fd = rbfs_open(DAT, O_RDONLY);
        rt_fail_dirent = 1;
        rc = rbfs_rename(DAT, PRV);
        printf("rename over, source open: rc=%d\n", rc);
        bad |= rc >= 0 || fd < 0;
        if (fd >= 0)
        {
            char b[16];
            bad |= rbfs_read(fd, b, sizeof(b)) != (ssize_t)sizeof(b)
                   || b[0] != 'i';
            rbfs_close(fd);
        }
        bad |= dup_check(D);
        bad |= !holds(DAT, 5120, 'i') || !holds(PRV, 4608, 'p');
        /* then the same rename with no fault succeeds, one entry each */
        rc = rbfs_rename(DAT, PRV);
        printf("rename over, no fault: rc=%d\n", rc);
        bad |= rc != 0 || dup_check(D) || rbfs_file_exists(DAT)
               || !holds(PRV, 5120, 'i');
        save(argv[1]);
        printf("%s\n", bad ? "FAIL" : "ok");
        return bad;
    }
    if (!strcmp(argv[2], "fuzz"))
    {
        /* random readers on index.dat / index.prev opened and closed
         * between publishes (UI reader, builder's pass_begin, GC walk) */
        int fds[4] = { -1, -1, -1, -1 };
        srand(argc > 3 ? atoi(argv[3]) : 1);
        for (int g = 0; g < 400 && !bad; g++)
        {
            for (int i = 0; i < 4; i++)
            {
                int r = rand() % 4;
                if (r == 0 && fds[i] < 0)
                    fds[i] = rbfs_open(rand() % 2 ? DAT : PRV, O_RDONLY);
                else if (r == 1 && fds[i] >= 0)
                {
                    rbfs_close(fds[i]);
                    fds[i] = -1;
                }
            }
            put(TMP, 8032, 'a' + g % 26);
            if (rand() % 3 == 0 && fds[0] >= 0)
            {   /* a reader mid-publish (yield inside the I/O) */
                char b[64];
                rbfs_read(fds[0], b, sizeof(b));
            }
            if (publish() < 0)
                printf("publish %d failed errno=%d\n", g, errno);
            bad = dup_check(D);
            if (bad)
                printf("after publish %d\n", g);
        }
        for (int i = 0; i < 4; i++)     /* a removed file's data goes at */
            if (fds[i] >= 0)            /* its last close */
                rbfs_close(fds[i]);
        save(argv[1]);
        if (overwrites)
            printf("FAIL: %d renames onto an existing name\n", overwrites);
        return bad || overwrites;
    }
    for (int g = 0; g < 60 && !bad; g++)
    {
        if (!strcmp(argv[2], "reader"))
        {
            if (reader < 0)
                reader = rbfs_open(DAT, O_RDONLY);
            if (reader < 0)
                reader = rbfs_open(PRV, O_RDONLY);
        }
        put(TMP, 8032, 'a' + g % 26);
        if (publish() < 0)
            printf("publish %d failed errno=%d\n", g, errno);
        if (!strcmp(argv[2], "reader") && (g % 3) == 2 && reader >= 0)
        {
            rbfs_close(reader);         /* reader_sync(): reopen next time */
            reader = -1;
        }
        bad = dup_check(D);
        if (bad)
            printf("after publish %d\n", g);
    }
    if (reader >= 0)
        rbfs_close(reader);
    save(argv[1]);
    if (overwrites)
        printf("FAIL: %d renames onto an existing name\n", overwrites);
    return bad || overwrites;
}
