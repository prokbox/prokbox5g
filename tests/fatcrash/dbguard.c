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

/* tests/fatcrash/dbguard (p081-fatdb): dbgen's prepare must never remove
 * or truncate through a damaged chain. On a RAM FAT32 image with the
 * 5.5G's geometry (fsreplay.c's), with the real fat.c, file layer,
 * apps/ipodui/dbgen.c (built as on the target) and apps/chaincheck.c: a
 * scratch generation is seeded like the device's gen2 (2026-11-09), one
 * chain is damaged in the FAT, and dbgen_prepare_empty() / dbgen_prepare() run.
 *
 *   dbguard        every case with the guard; exit 0 when all hold
 *   dbguard off    the same without it (ops.chain NULL): the control,
 *                  which must fail the harm cases (run.sh checks)
 *
 * Harm is what a remove through the chain frees that it does not own: a
 * file outside the generation (keep.dat) or the generation directory's
 * own cluster. A damaged directory must be set aside (database_aside0),
 * never cleared, and a healthy one cleared as before (no false alarm). */
#define main fsreplay_main
#include "fsreplay.c"
#undef main
#include "fs_defines.h"
#include "../../apps/ipodui/dbgen.h"
#include "../../apps/chaincheck.h"

/* dbgen.c on the target calls these; the paths dbgen_prepare*() take
 * never reach them */
int safefile_remove(const char *path, const char *keep)
{ (void)path; (void)keep; return -1; }

#define ROOT ROCKBOX_DIR
#define GEN ROOT "/database_gen0"
#define KEEP ROOT "/keep/keep.dat"
#define KEEP_SIZE 40000

static const struct dbgen_file files[] = {
    { "database_idx.tcd", false, true },
    { "database_0.tcd", true, true }, { "database_1.tcd", true, true },
    { "database_2.tcd", true, true }, { "database_3.tcd", true, true },
    { "database_4.tcd", true, true }, { "database_5.tcd", true, true },
    { "database_6.tcd", true, true }, { "database_7.tcd", true, true },
    { "database_8.tcd", true, true }, { "database_12.tcd", true, true },
    { "database_lib.dat", true, false },
};
static const long sizes[] = { 171480, 8972, 8996, 5660, 59796, 164875, 3132,
                              30316, 5444, 59252, 8972, 37048 };

static enum dbgen_result validate(const char *path, int32_t commitid,
    int32_t entries, uint32_t *sum, int32_t *found, void *opaque)
{
    (void)path; (void)commitid; (void)entries; (void)opaque;
    *sum = 0;
    *found = -1;
    return DBGEN_CORRUPT;
}

static void put(const char *path, long k, char fill)
{
    static char b[4096];
    int fd = rbfs_open(path, O_WRONLY | O_CREAT | O_TRUNC);
    memset(b, fill, sizeof(b));
    while (fd >= 0 && k > 0)
    {
        long c = k > 4096 ? 4096 : k;
        rbfs_write(fd, b, c);
        k -= c;
    }
    if (fd >= 0)
        rbfs_close(fd);
}

/* a file's or a folder's first cluster */
static long first_cluster(const char *path, unsigned int type)
{
    struct path_component_info ci;
    struct filestr_base s;
    file_internal_lock_WRITER();
    int rc = open_stream_internal(path, type | FF_PROBE | FF_INFO, &s, &ci);
    file_internal_unlock_WRITER();
    return rc > 0 ? (long)ci.info.fatfile.firstcluster : -1;
}

/* the FAT32 entry of cluster c in every FAT copy, straight on the image
 * (the volume unmounted: nothing cached) */
static uint32_t *fat_at(unsigned f, long c)
{
    unsigned b = img[11] | img[12] << 8, rsvd = img[14] | img[15] << 8;
    uint32_t fatsz;
    memcpy(&fatsz, img + 36, 4);
    return (uint32_t *)(img + (size_t)(rsvd + f * fatsz) * b + 4 * c);
}
static void set_fat(long c, uint32_t v)
{
    for (unsigned f = 0; f < img[16]; f++)
        *fat_at(f, c) = v;
}

static void unmount_all(void)
{
    root_unmount_volume();
    fileobj_mgr_unmount();
    fat_unmount();
}

static long last_cluster(const char *path)
{
    uint32_t runs[64];
    int n = 0;
    if (chain_check(path, runs, 32, &n) != CHAIN_OK || n <= 0)
        return -1;
    return (long)(runs[2 * (n - 1)] + runs[2 * (n - 1) + 1] - 1);
}

static bool keep_whole(void)
{
    uint32_t runs[64];
    int n = 0;
    if (chain_check(KEEP, runs, 32, &n) != CHAIN_OK)
        return false;
    int fd = rbfs_open(KEEP, O_RDONLY);
    char b[4096];
    long got = 0, r;
    bool same = fd >= 0;
    while (same && (r = rbfs_read(fd, b, sizeof(b))) > 0)
    {
        for (long i = 0; i < r; i++)
            same &= b[i] == 'K';
        got += r;
    }
    if (fd >= 0)
        rbfs_close(fd);
    return same && got == KEEP_SIZE;
}

static int exists(const char *path, unsigned int type)
{
    return first_cluster(path, type) >= 0;
}

static int declared_left(void)
{
    int n = 0;
    for (unsigned i = 0; i < ARRAYLEN(files); i++)
    {
        char p[MAX_PATH];
        snprintf(p, sizeof(p), GEN "/%s", files[i].name);
        n += exists(p, FF_FILE);
    }
    return n;
}

enum damage { D_NONE, D_INTO_KEEP, D_INTO_DIR, D_CUT, D_SHARED, D_LONG,
              D_ALIAS, D_DROP_REMOVES, D_DROP_TEMP };
static const char *const dname[] = {
    "healthy", "idx tail into another folder's file",
    "idx tail into the generation's own folder cluster",
    "database_0 cut (first cluster free)",
    "idx tail into database_0 (two files, one chain)",
    "database_lib.dat a cluster past its size (into keep.dat)",
    "database_1's entry on database_0's cluster (each whole)",
    "the drive keeps no folder write during the removes",
    "the drive keeps no folder write during the temp's create" };

/* the log (ops->note and the chain hook's line), as the device's */
static char notes[16][64];
static int nnotes, longest;
static void take(const char *line)
{
    int n = (int)strlen(line);
    if (n > longest)
        longest = n;
    if (nnotes < 16)
        snprintf(notes[nnotes++], sizeof(notes[0]), "%s", line);
}
static int noted(const char *what)
{
    int k = 0;
    for (int i = 0; i < nnotes && i < 16; i++)
        k += strstr(notes[i], what) != NULL;
    return k;
}
static int chain_logged(const char *path, uint32_t *runs, int maxruns,
                        int *nruns)
{
    struct chain_report r;
    char line[56], full[64];
    int rc = chain_check_why(path, runs, maxruns, nruns, &r);
    if (rc == CHAIN_DAMAGED || rc == CHAIN_UNREADABLE)
    {
        /* the device's line: ui_log("db", "gen chain %s", ...) */
        snprintf(full, sizeof(full), "gen chain %s",
                 chain_report_line(line, sizeof(line), path, &r));
        take(full);
    }
    return rc;
}
static void no_drop(void) {}   /* the "cache" control: blind read-backs */

/* the generation folder's sectors (512-byte storage units) */
static void folder_sectors(long dir, unsigned long *lo, unsigned long *hi)
{
    unsigned b = img[11] | img[12] << 8, rsvd = img[14] | img[15] << 8;
    uint32_t fatsz;
    memcpy(&fatsz, img + 36, 4);
    unsigned long first = rsvd + img[16] * fatsz + (dir - 2) * img[13];
    *lo = first * (b / SECTOR);
    *hi = (first + img[13]) * (b / SECTOR);
}

enum mode { M_FULL, M_OFF, M_NODROP };

/* 0 holds, 1 fails */
static int one(enum damage dmg, enum mode mode, bool clone)
{
    static char scratch[8192];
    char out[DBGEN_PATH_MAX];
    bool guard = mode != M_OFF;
    struct dbgen_ops ops = { files, ARRAYLEN(files), validate, NULL, NULL,
                             guard ? chain_logged : NULL, take,
                             mode == M_FULL ? chain_cache_drop
                             : mode == M_NODROP ? no_drop : NULL };

    nnotes = longest = 0;
    drop_lo = drop_hi = 0;
    drop_hits = 0;
    free(img);
    mkfs(32);
    mount();
    rbfs_mkdir(ROOT);
    rbfs_rename_intent_boot();
    rbfs_mkdir(ROOT "/keep");
    put(KEEP, KEEP_SIZE, 'K');
    rbfs_mkdir(GEN);
    put(GEN "/database_changelog.txt", 115954, 'c');
    for (unsigned i = 0; i < ARRAYLEN(files); i++)
    {
        char p[MAX_PATH];
        snprintf(p, sizeof(p), GEN "/%s", files[i].name);
        put(p, sizes[i], 'a' + i);
    }
    long idx_last = last_cluster(GEN "/database_idx.tcd");
    long lib_last = last_cluster(GEN "/database_lib.dat");
    long db0 = first_cluster(GEN "/database_0.tcd", FF_FILE);
    long keep = first_cluster(KEEP, FF_FILE);
    long dir = first_cluster(GEN, FF_DIR);
    unmount_all();
    if (idx_last < 2 || lib_last < 2 || db0 < 2 || keep < 2 || dir < 2)
    {
        printf("  setup failed\n");
        return 1;
    }
    switch (dmg)
    {
    case D_NONE: case D_DROP_REMOVES: case D_DROP_TEMP: break;
    case D_INTO_KEEP: set_fat(idx_last, (uint32_t)keep); break;
    case D_INTO_DIR: set_fat(idx_last, (uint32_t)dir); break;
    case D_CUT: set_fat(db0, 0); break;
    case D_SHARED: set_fat(idx_last, (uint32_t)db0); break;
    case D_LONG: set_fat(lib_last, (uint32_t)keep); break;
    case D_ALIAS:
        /* database_1's short entry (the fourth DATABASE*.TCD alias made:
         * DATABA~2TCD) names database_0's one cluster: each chain alone is
         * whole, only the comparison sees one cluster under two files */
        for (size_t o = 0; o + 32 <= img_sectors * SECTOR; o += 32)
            if (!memcmp(img + o, "DATABA~2TCD", 11))
            {
                img[o + 20] = (uint8_t)(db0 >> 16);
                img[o + 21] = (uint8_t)(db0 >> 24);
                img[o + 26] = (uint8_t)db0;
                img[o + 27] = (uint8_t)(db0 >> 8);
            }
        break;
    }
    mount();

    /* the active library is elsewhere (the root: a legacy one) */
    if (dmg == D_DROP_REMOVES)
        folder_sectors(dir, &drop_lo, &drop_hi);
    enum dbgen_result rc = clone
        ? dbgen_prepare(ROOT, ROOT, out, scratch, sizeof(scratch), &ops)
        : dbgen_prepare_empty(ROOT, ROOT, out, scratch, sizeof(scratch),
                              &ops);
    drop_lo = drop_hi = 0;
    enum dbgen_result rc1 = rc;

    /* a scan's temp file in the prepared folder, read back (dbgen's, as
     * the tagcache's scan does it) */
    enum dbgen_result rb = DBGEN_OK;
    bool temp_ok = rc != DBGEN_OK;      /* no temp when prepare stopped */
    if (rc == DBGEN_OK)
    {
        char t[MAX_PATH];
        snprintf(t, sizeof(t), "%s/database_tmp.tcd", out);
        if (dmg == D_DROP_TEMP)
            folder_sectors(dir, &drop_lo, &drop_hi);
        put(t, 733894, 'T');
        drop_lo = drop_hi = 0;
        rb = dbgen_readback_size(ROOT, t, 733894, &ops);
        if (rb == DBGEN_OK)
        {
            /* the old code's view (the cache): the temp is there */
            int fd = rbfs_open(t, O_RDONLY);
            temp_ok = fd >= 0 && rbfs_ffilesize(fd) == 733894;
            if (fd >= 0)
                rbfs_close(fd);
        }
        else
            temp_ok = true;             /* caught: nothing trusted */
    }
    bool caught = rc1 == DBGEN_READBACK || rb == DBGEN_READBACK;
    /* the next pass after a read-back mismatch: the folder set aside, as
     * it was, and the generation made afresh */
    if (caught)
    {
        unmount_all();                  /* (the remount a reboot does) */
        mount();
        rc = clone
            ? dbgen_prepare(ROOT, ROOT, out, scratch, sizeof(scratch), &ops)
            : dbgen_prepare_empty(ROOT, ROOT, out, scratch, sizeof(scratch),
                                  &ops);
    }

    bool keep_ok = keep_whole();
    /* the generation folder's cluster still allocated (the image's FAT,
     * after the commit) */
    unmount_all();
    uint32_t dirent_fat = *fat_at(0, dir) & 0x0fffffff;
    mount();
    bool dir_ok = dirent_fat != 0;
    bool aside = exists(ROOT "/database_aside0", FF_DIR);
    int left = declared_left();

    bool dropping = dmg == D_DROP_REMOVES || dmg == D_DROP_TEMP;
    bool harmed = !keep_ok || !dir_ok || !temp_ok;
    bool want_aside = guard && dmg != D_NONE;
    /* the log: nothing on a clean run; a chain line on damage; a
     * read-back line when the drive dropped a folder write; every line
     * under 60 characters */
    bool log_ok = longest < 60
        && (dmg == D_NONE ? nnotes == 0 : true)
        && (!guard || dmg == D_NONE || dropping || dmg == D_ALIAS
            || noted("gen chain ") == 1)
        && (!dropping || noted("dir readback") >= 1)
        && (!want_aside || noted("gen aside0") == 1);
    bool bad = harmed || rc != DBGEN_OK || aside != want_aside
               || (dropping && !caught) || left != 0 || !log_ok;
    printf("  %-6s %-7s %-56s rc=%d/%d keep=%s dir=%s temp=%s aside=%d "
           "left=%d log=%d%s %s\n",
           mode == M_FULL ? "guard" : mode == M_OFF ? "none" : "cache",
           clone ? "prepare" : "empty", dname[dmg], rc1, rb,
           keep_ok ? "ok" : "HARMED", dir_ok ? "ok" : "FREED",
           temp_ok ? "ok" : "LOST", aside, left, nnotes,
           log_ok ? "" : "(BAD)", bad ? "FAIL" : "ok");
    if (getenv("DBGUARD_VERBOSE"))
        for (int i = 0; i < nnotes && i < 16; i++)
            printf("    | %s\n", notes[i]);
    unmount_all();
    return bad;
}

int main(int argc, char **argv)
{
    enum mode mode = argc > 1 && !strcmp(argv[1], "off") ? M_OFF
                   : argc > 1 && !strcmp(argv[1], "cache") ? M_NODROP
                   : M_FULL;
    int fails = 0, cases = 0;
    for (int clone = 0; clone < 2; clone++)
        for (int d = D_NONE; d <= D_DROP_TEMP; d++)
        {
            fails += one((enum damage)d, mode, clone);
            cases++;
        }
    printf("dbguard %s: %d cases, %d failed\n",
           mode == M_FULL ? "guard" : mode == M_OFF ? "control (no guard)"
           : "control (read-back through the cache)", cases, fails);
    return cases && !fails ? 0 : 1;
}
