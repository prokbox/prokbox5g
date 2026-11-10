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

/* tests/walkcache: the walk lookup cache (#243, file_internal.c) over the
 * firmware's whole file layer on a RAM FAT32 image (the volume and
 * storage model of tests/fatcrash/dirburst.c, included below).
 *
 *   walkcache unit <spc> [bps]   hits and misses, invalidation by rmdir,
 *       rename, a file in a removed folder's place, and USB (unmount, the
 *       host's image, remount), LRU eviction, no room, nesting, long
 *       names. Prints one line per case, "ok" or what went wrong.
 *   walkcache lib <tracks> <spc> [bps]   a library-shaped volume, then the
 *       scan's access pattern (tagcache.c check_dir(): an opendir by path
 *       per folder, its two database.(un)ignore probes, and per track an
 *       open by path, a read, its size and a .cue probe, and per folder
 *       its entry as a lookup returns it), then an open of
 *       every track (the deleted-file pass), then a folder-only walk (the
 *       boot digest's shape). Prints a digest of every
 *       result (names, attributes, sizes, return codes, errno, bytes)
 *       that must not depend on the cache, and, on stderr, the directory
 *       entries the lookups decoded and the host time.
 *
 * run.sh builds it with the cache, without it (-DFILE_NO_WALK_CACHE,
 * upstream's lookups) and once per negative control (-DWALKCACHE_BREAK=<mask>:
 * 1 creates, 2 removes, 4 mounts do not invalidate; 8 never a hit; 16 no
 * LRU), and checks that each control fails the case that guards it.
 * Exit 0 when every check holds.
 */
#define DIRBURST_NO_MAIN
#include "dirburst.c"
#include "core_alloc.h"     /* fatcrash_core_room */

#ifdef FILE_WALK_CACHE
extern unsigned long walk_cache_hits, walk_cache_misses;
#else   /* file.h's file_walk_cache() does nothing */
static unsigned long walk_cache_hits, walk_cache_misses;
#endif

/* file_internal.c is built with -Dfat_readdir=wc_fat_readdir: every
 * directory entry a lookup or a listing decodes */
static unsigned long decoded;
int wc_fat_readdir(struct fat_filestr *dirstr, struct fat_dirscan_info *scan,
                   struct filestr_cache *cachep, struct fat_direntry *entry)
{
    int rc = fat_readdir(dirstr, scan, cachep, entry);
    if (rc > 0)
        decoded++;
    return rc;
}

static int fails;
static const char *cur;
#define CHECK(c, ...) do { if (!(c)) { printf("%s: FAIL ", cur); \
    printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static void reset_stats(void)
{
    walk_cache_hits = walk_cache_misses = 0;
}

static bool isdir(const char *p)
{
    DIR *d = rbfs_opendir(p);
    if (!d)
        return false;
    rbfs_closedir(d);
    return true;
}

static void mk(const char *p)
{
    if (rbfs_mkdir(p) != 0) { fprintf(stderr, "mkdir %s\n", p); exit(2); }
}

/* hits: a second lookup of a path's folders is served from the cache */
static void t_hits(void)
{
    cur = "hits";
    mk("/Music"); mk("/Music/Artist"); mk("/Music/Artist/Album");
    put("/Music/Artist/Album/01 Song.mp3", 10, 'a');
    file_walk_cache(true);
    reset_stats();
    CHECK(isdir("/Music/Artist/Album"), "first lookup");
    CHECK(walk_cache_misses == 3 && walk_cache_hits == 0,
          "first: %lu misses %lu hits, want 3 0", walk_cache_misses,
          walk_cache_hits);
    reset_stats();
    unsigned long d0 = decoded;
    CHECK(isdir("/Music/Artist/Album"), "second lookup");
    CHECK(walk_cache_hits == 3 && walk_cache_misses == 0,
          "second: %lu hits %lu misses, want 3 0", walk_cache_hits,
          walk_cache_misses);
    CHECK(decoded - d0 == 0, "second decoded %lu entries", decoded - d0);
    reset_stats();
    CHECK(isdir("/MUSIC/artist/ALBUM"), "other case");
    CHECK(walk_cache_hits == 3, "other case: %lu hits", walk_cache_hits);
    reset_stats();
    CHECK(rbfs_file_exists("/Music/Artist/Album/01 Song.mp3"), "file");
    CHECK(!rbfs_file_exists("/Music/Artist/Album/02 None.mp3"), "missing");
    CHECK(walk_cache_hits == 6 && walk_cache_misses == 2,
          "file probes: %lu hits %lu misses, want 6 2 (files scan)",
          walk_cache_hits, walk_cache_misses);
    file_walk_cache(false);
    reset_stats();
    CHECK(isdir("/Music/Artist/Album"), "after off");
    CHECK(walk_cache_hits == 0, "off: %lu hits", walk_cache_hits);
}

/* rmdir: the removed folder's slot must never answer again */
static void t_rmdir(void)
{
    cur = "rmdir";
    mk("/D"); mk("/D/X");
    file_walk_cache(true);
    CHECK(isdir("/D/X"), "before");
    CHECK(isdir("/D/X"), "before (hit)");
    CHECK(rbfs_rmdir("/D/X") == 0, "rmdir");
    errno = 0;
    CHECK(!isdir("/D/X") && errno == ENOENT, "after rmdir: %s",
          isdir("/D/X") ? "still opens" : "wrong errno");
    file_walk_cache(false);
}

/* rename: the old name gone, the new one and a name reused */
static void t_rename(void)
{
    cur = "rename";
    mk("/R"); mk("/R/A"); mk("/R/B");
    put("/R/A/from-a", 1, 'a');
    put("/R/B/from-b", 1, 'b');
    file_walk_cache(true);
    CHECK(rbfs_file_exists("/R/A/from-a") && rbfs_file_exists("/R/B/from-b"),
          "before");
    CHECK(rbfs_rename("/R/A", "/R/T") == 0, "rename A T");
    CHECK(!isdir("/R/A"), "old name still opens");
    CHECK(rbfs_file_exists("/R/T/from-a"), "new name");
    CHECK(rbfs_rename("/R/B", "/R/A") == 0, "rename B A");
    CHECK(rbfs_file_exists("/R/A/from-b") && !rbfs_file_exists("/R/A/from-a"),
          "the reused name is the other folder");
    file_walk_cache(false);
}

/* create in a removed folder's place: a file now, then a new folder */
static void t_create(void)
{
    cur = "create";
    mk("/C"); mk("/C/N");
    put("/C/N/old", 1, 'o');
    file_walk_cache(true);
    CHECK(rbfs_file_exists("/C/N/old"), "before");
    CHECK(rbfs_remove("/C/N/old") == 0 && rbfs_rmdir("/C/N") == 0, "rmdir");
    /* a file of that name: a stale slot makes this a folder (EISDIR) */
    int fd = rbfs_open("/C/N", O_WRONLY | O_CREAT | O_TRUNC);
    CHECK(fd >= 0, "create a file in its place: errno %d", errno);
    if (fd < 0)
        return;
    CHECK(rbfs_write(fd, "fffff", 5) == 5 && rbfs_close(fd) == 0, "write");
    errno = 0;
    CHECK(!isdir("/C/N") && errno == ENOTDIR, "a file: %s",
          isdir("/C/N") ? "opens as a folder" : "wrong errno");
    CHECK(rbfs_remove("/C/N") == 0, "remove file");
    mk("/C/N");                           /* a new folder, empty */
    CHECK(!rbfs_file_exists("/C/N/old"), "new folder holds the old file");
    put("/C/N/new", 1, 'n');
    CHECK(rbfs_file_exists("/C/N/new"), "new folder's file");
    file_walk_cache(false);
}

/* USB: unmount, the host puts back an image without /U/X, remount */
static void t_usb(void)
{
    cur = "usb";
    mk("/U");
    dc_empty();
    unsigned long top = wr_top;
    uint8_t *snap = malloc(top * SECTOR);
    memcpy(snap, img, top * SECTOR);
    mk("/U/X");
    put("/U/X/f", 1, 'x');
    file_walk_cache(true);
    CHECK(rbfs_file_exists("/U/X/f"), "before");
    CHECK(rbfs_file_exists("/U/X/f"), "before (hit)");
    dc_empty();
    root_unmount_volume();
    fileobj_mgr_unmount();
    fat_unmount();
    memcpy(img, snap, top * SECTOR);
    if (wr_top > top)
        memset(img + top * SECTOR, 0, (wr_top - top) * SECTOR);
    if (fat_mount(0) != 0) { printf("remount failed\n"); exit(2); }
    root_mount_path("/", NSITEM_HIDDEN);
    root_mount_path("/", NSITEM_CONTENTS);
    errno = 0;
    CHECK(!isdir("/U/X") && errno == ENOENT, "after USB: %s",
          isdir("/U/X") ? "the host's removed folder opens" : "wrong errno");
    file_walk_cache(false);
    free(snap);
}

/* eviction: 40 folders cycled through 16 slots; the one looked up
 * between each of them stays (least recently used goes first) */
static void t_evict(void)
{
    char p[64];
    cur = "evict";
    mk("/E"); mk("/E/hot");
    for (int i = 0; i < 40; i++)
    {
        snprintf(p, sizeof(p), "/E/d%02d", i);
        mk(p);
        snprintf(p, sizeof(p), "/E/d%02d/m%02d", i, i);
        put(p, 1, 'm');
    }
    file_walk_cache(true);
    CHECK(isdir("/E/hot"), "hot");
    unsigned long hot_miss = 0;
    for (int round = 0; round < 2; round++)
        for (int i = 0; i < 40; i++)
        {
            snprintf(p, sizeof(p), "/E/d%02d/m%02d", i, i);
            CHECK(rbfs_file_exists(p), "%s", p);
            reset_stats();
            CHECK(isdir("/E/hot"), "hot");
            hot_miss += walk_cache_misses;
        }
    CHECK(hot_miss == 0, "the hot folder missed %lu times", hot_miss);
    reset_stats();
    CHECK(isdir("/E/d00"), "d00");     /* 39 folders since: evicted */
    CHECK(walk_cache_misses == 1 && walk_cache_hits == 1,
          "d00 after 39 others: %lu misses %lu hits, want 1 1",
          walk_cache_misses, walk_cache_hits);
    file_walk_cache(false);
}

/* no free memory: no cache, same answers; nesting; long names */
static void t_misc(void)
{
    char longname[200], p[260];
    cur = "misc";
    mk("/M"); mk("/M/S");
    fatcrash_core_room = 0;
    file_walk_cache(true);
    reset_stats();
    CHECK(isdir("/M/S") && isdir("/M/S"), "no room");
    CHECK(walk_cache_hits + walk_cache_misses == 0, "no room: a cache");
    file_walk_cache(false);
    fatcrash_core_room = 1 << 20;

    file_walk_cache(true);
    file_walk_cache(true);
    file_walk_cache(false);             /* one user left */
    reset_stats();
    CHECK(isdir("/M/S") && isdir("/M/S"), "nested");
    CHECK(walk_cache_hits >= 2, "nested: %lu hits", walk_cache_hits);
    file_walk_cache(false);
    file_walk_cache(false);             /* unbalanced: ignored */
    reset_stats();
    CHECK(isdir("/M/S"), "after off");
    CHECK(walk_cache_hits == 0, "after the last off: %lu hits",
          walk_cache_hits);

    memset(longname, 'L', 150);
    longname[150] = 0;
    snprintf(p, sizeof(p), "/M/%s", longname);
    mk(p);
    file_walk_cache(true);
    reset_stats();
    CHECK(isdir(p) && isdir(p), "long name");
    CHECK(walk_cache_hits == 1 && walk_cache_misses == 3,
          "long name: %lu hits %lu misses, want 1 3 (/M kept, it scans)",
          walk_cache_hits, walk_cache_misses);
    file_walk_cache(false);
}

/* listed: a folder kept from a listing (readdir) answers a lookup with
 * exactly the entry a scan finds (its position, attributes, size) */
static void t_listed(void)
{
    static struct path_component_info ref[40];
    static char names[40][96];
    char p[MAX_PATH];
    int n = 0;
    cur = "listed";
    mk("/L");
    for (int i = 0; i < 12; i++)        /* fewer than the slots */
    {
        char t[64];
        title(t, sizeof(t), (unsigned)i * 41u + 3u, 1 + i % 5);
        snprintf(names[n], sizeof(names[0]), "%s %02d", t, i);
        snprintf(p, sizeof(p), "/L/%s", names[n++]);
        mk(p);
        if (i % 3 == 0)                 /* files between the folders */
        {
            snprintf(p, sizeof(p), "/L/file %02d.mp3", i);
            put(p, 3, 'f');
        }
    }
    strcpy(names[n++], "SHORT");        /* fits 8.3 */
    mk("/L/SHORT");
    for (int pass = 0; pass < 2; pass++)
    {
        if (pass)
        {
            file_walk_cache(true);
            DIR *d = rbfs_opendir("/L");
            while (rbfs_readdir(d))
                ;
            rbfs_closedir(d);
            reset_stats();
        }
        for (int i = 0; i < n; i++)
        {
            struct filestr_base st;
            struct path_component_info ci;
            memset(&ci, 0, sizeof(ci));
            snprintf(p, sizeof(p), "/L/%s", names[i]);
            file_internal_lock_WRITER();
            int rc = open_stream_internal(p, FF_ANYTYPE | FF_PROBE | FF_INFO,
                                          &st, &ci);
            file_internal_unlock_WRITER();
            CHECK(rc > 0, "%s: rc %d", names[i], rc);
            if (!pass)
                ref[i] = ci;
            else
                CHECK(!memcmp(&ref[i].info.fatfile, &ci.info.fatfile,
                              sizeof(ci.info.fatfile))
                      && ref[i].attr == ci.attr
                      && ref[i].filesize == ci.filesize,
                      "%s: entry %u/%u cluster %ld, scan says %u/%u %ld",
                      names[i], ci.info.fatfile.e.entry,
                      ci.info.fatfile.e.entries, ci.info.fatfile.firstcluster,
                      ref[i].info.fatfile.e.entry,
                      ref[i].info.fatfile.e.entries,
                      ref[i].info.fatfile.firstcluster);
        }
    }
    /* each lookup: /L, then the folder from the listing (the file layer
     * writes a long name for SHORT too; a short-only, OEM, entry comes
     * from other writers only and is never kept from a listing) */
    CHECK(walk_cache_hits == 2 * 13 && walk_cache_misses == 0,
          "listed folders: %lu hits %lu misses, want 26 0", walk_cache_hits,
          walk_cache_misses);
    file_walk_cache(false);
}

static int unit(unsigned spc)
{
    mkfs(spc);
    mount();
    t_hits(); t_rmdir(); t_rename(); t_create(); t_usb(); t_evict();
    t_misc(); t_listed();
    printf("unit spc %u bps %u: %s\n", spc, bps, fails ? "FAIL" : "ok");
    return fails != 0;
}

/* ---- lib: the scan's pattern, digested ---- */
static uint32_t dg = 2166136261u;
static void dg_int(long v) { dg = fnv(dg, &v, sizeof(v)); }
static void dg_str(const char *s) { dg = fnv(dg, s, strlen(s) + 1); }

static unsigned long nfiles, ndirs;
/* as tagcache.c check_dir() since p081-bootwalk: a subfolder opened from
 * its parent's entry (opendir_child()), the top one by path */
static void scan(DIR *parent, const struct dirent *pe, char *path,
                 size_t len)
{
    DIR *d = parent ? rbfs_opendir_child(parent, pe, path)
                    : rbfs_opendir(path);
    dg_int(d != NULL);
    if (!d)
        return;
    ndirs++;
    char probe[MAX_PATH];
    snprintf(probe, sizeof(probe), "%s/database.ignore", path);
    dg_int(rbfs_file_exists(probe));
    snprintf(probe, sizeof(probe), "%s/database.unignore", path);
    dg_int(rbfs_file_exists(probe));
    struct dirent *e;
    while ((e = rbfs_readdir(d)))
    {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        dg_str(e->d_name);
        dg_int(e->info.attr);
        dg_int((long)e->info.size);
        size_t n = (size_t)snprintf(path + len, MAX_PATH - len, "/%s",
                                    e->d_name);
        if (e->info.attr & ATTR_DIRECTORY)
        {
            /* the folder's entry as a lookup gives it (its position too) */
            struct filestr_base st;
            struct path_component_info ci;
            memset(&ci, 0, sizeof(ci));
            file_internal_lock_WRITER();
            dg_int(open_stream_internal(path, FF_DIR | FF_PROBE | FF_INFO,
                                        &st, &ci));
            file_internal_unlock_WRITER();
            dg = fnv(dg, &ci.info.fatfile, sizeof(ci.info.fatfile));
            dg_int(ci.attr);
            dg_int((long)ci.filesize);
            scan(d, e, path, len + n);
        }
        else
        {
            nfiles++;
            unsigned char buf[64];
            int fd = rbfs_open(path, O_RDONLY);
            dg_int(fd >= 0);
            if (fd >= 0)
            {
                ssize_t r = rbfs_read(fd, buf, sizeof(buf));
                dg_int(r);
                if (r > 0)
                    dg = fnv(dg, buf, (size_t)r);
                dg_int(rbfs_ffilesize(fd));
                rbfs_close(fd);
            }
            char *dot = strrchr(path + len, '.');
            if (dot)
            {
                strcpy(dot, ".cue");
                errno = 0;
                dg_int(rbfs_file_exists(path));
                dg_int(errno);
            }
        }
        path[len] = 0;
    }
    rbfs_closedir(d);
}

static int libwalk(unsigned long tracks, unsigned spc)
{
    char p[MAX_PATH], a[96], b[96], t[96];
    unsigned long artists = tracks / 20 ? tracks / 20 : 1, n = 0;
    static char (*paths)[160];
    paths = malloc(tracks * sizeof(*paths));
    mkfs(spc);
    mount();
    mk("/Music");
    for (unsigned long ar = 0; ar < artists; ar++)
    {
        title(a, sizeof(a), (unsigned)ar * 7 + 1, 2 + ar % 3);
        snprintf(p, sizeof(p), "/Music/%s %04lu", a, ar);
        mk(p);
        if (ar % 50 == 0)               /* a few artists have one */
        {
            snprintf(p, sizeof(p), "/Music/%s %04lu/database.ignore", a, ar);
            put(p, 0, 0);
        }
        for (int al = 0; al < 2; al++)
        {
            title(b, sizeof(b), (unsigned)(ar * 13 + al), 3 + al);
            snprintf(p, sizeof(p), "/Music/%s %04lu/%s (%d)", a, ar, b,
                     1990 + (int)(ar % 30));
            mk(p);
            for (int tr = 1; tr <= 10 && n < tracks; tr++)
            {
                title(t, sizeof(t), (unsigned)(ar * 31 + al * 11 + tr),
                      2 + tr % 4);
                snprintf(paths[n], sizeof(paths[0]), "%s/%02d %s.%s", p, tr,
                         t, tr == 3 ? "cue" : "flac");
                put(paths[n++], 40 + (size_t)(tr * 7), 'A' + tr);
            }
        }
    }
    dc_empty();
    printf("image digest %08x\n", fnv(2166136261u, img, wr_top * SECTOR));

    file_walk_cache(true);
    char path[MAX_PATH] = "/Music";
    decoded = 0;
    reset_stats();
    clock_t c0 = clock();
    scan(NULL, NULL, path, strlen(path));
    double ms = (double)(clock() - c0) * 1000.0 / CLOCKS_PER_SEC;
    file_walk_cache(false);
    printf("scan: %lu dirs %lu files digest %08x\n", ndirs, nfiles, dg);
    fprintf(stderr, "scan: decoded %lu entries, %lu hits %lu misses, "
            "host %.0f ms\n", decoded, walk_cache_hits, walk_cache_misses,
            ms);
    printf("stat scan_decoded %lu scan_ms %.0f\n", decoded, ms);

    file_walk_cache(true);
    decoded = 0;
    unsigned long found = 0;
    c0 = clock();
    for (unsigned long i = 0; i < n; i++)
    {
        int fd = rbfs_open(paths[i], O_RDONLY);
        if (fd >= 0) { found++; rbfs_close(fd); }
    }
    ms = (double)(clock() - c0) * 1000.0 / CLOCKS_PER_SEC;
    file_walk_cache(false);
    printf("opens: %lu of %lu found\n", found, n);
    printf("stat open_decoded %lu open_ms %.0f\n", decoded, ms);

    /* the boot digest's shape (bootstamp.c walk_dir()): an opendir by
     * path per folder and its listing, no file opened */
    struct walk w = { 0, 0, 2166136261u };
    file_walk_cache(true);
    decoded = 0;
    c0 = clock();
    walk("/Music", &w, 0);
    ms = (double)(clock() - c0) * 1000.0 / CLOCKS_PER_SEC;
    file_walk_cache(false);
    printf("dirwalk: %lu dirs %lu entries names %08x\n", w.dirs, w.entries,
           w.h);
    printf("stat dirwalk_decoded %lu dirwalk_ms %.0f\n", decoded, ms);
    return found != n;
}

int main(int argc, char **argv)
{
    if (argc >= 4 && !strcmp(argv[1], "lib"))
    {
        if (argc == 5)
            bps = (unsigned)atoi(argv[4]);
        return libwalk(strtoul(argv[2], NULL, 0), (unsigned)atoi(argv[3]));
    }
    if (argc >= 3 && !strcmp(argv[1], "unit"))
    {
        if (argc == 4)
            bps = (unsigned)atoi(argv[3]);
        return unit((unsigned)atoi(argv[2]));
    }
    fprintf(stderr, "usage: walkcache unit <spc> [bps] | "
            "lib <tracks> <spc> [bps]\n");
    return 2;
}
