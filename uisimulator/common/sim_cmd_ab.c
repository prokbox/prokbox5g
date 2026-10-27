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
 * Phase 6 (Audiobooks) sim script commands: power-cut injection into
 * books.dat / books.pak / books.idx, store and pak inspection, a memory
 * mark for "no pool growth while playing". Host-side file access only (the
 * files live under the sim disk); the layouts are read from the frozen
 * contract headers apps/ipodui/ab_store.h and ab_book.h, mirrored here as
 * constants so the stock sim links without any apps/ipodui file.
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
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "core_alloc.h"
#include "filesystem-sim.h"
#include "sim_cmd_ab.h"

/* ab_store.h: 512 B sectors, header in sector 0 and the last sector,
 * 32 B records, 16 per sector; flags' low byte = state, high = check. */
#define AB_SECTOR        512
#define AB_REC           32
#define AB_PER_SECT      16
#define AB_F_STARTED     0x0001
#define AB_F_FINISHED    0x0002
#define AB_F_SEEDED      0x0004
/* ab_store_hdr: magic 4, version 2, rec_size 2, seq 4, count 4, ... */
#define HDR_COUNT_OFF    12
#define AB_STORE_MAGIC   0x42415049u     /* "IPAB" (ab_store.h) */
/* ab_idx_hdr: magic 4, version 2, rec_size 2, generation 4, count 4,
 * pak_size 4, dead_bytes 4 */
#define IDX_COUNT_OFF    12
#define IDX_PAK_OFF      16
#define IDX_GEN_OFF      8

#define DIR "/.rockbox/artcache/"

static const char *ab_name(const char *what)
{
    if (!strcmp(what, "store"))   return DIR "books.dat";
    if (!strcmp(what, "tmp"))     return DIR "books.tmp";
    if (!strcmp(what, "pak"))     return DIR "books.pak";
    if (!strcmp(what, "idx"))     return DIR "books.idx";
    if (!strcmp(what, "pak_tmp")) return DIR "books.pak.tmp";
    if (!strcmp(what, "idx_tmp")) return DIR "books.idx.tmp";
    return NULL;
}

static bool os_path(char *buf, size_t n, const char *sim)
{
    return sim_get_os_path(buf, sim, n) >= 0;
}

static long file_size(const char *os)
{
    struct stat st;
    return stat(os, &st) ? -1 : (long)st.st_size;
}

static bool zero_range(const char *os, long off, long len)
{
    FILE *f = fopen(os, "r+b");
    char z[AB_SECTOR];

    if (!f)
        return false;
    memset(z, 0, sizeof(z));
    if (len > (long)sizeof(z))
        len = sizeof(z);
    if (fseek(f, off, SEEK_SET) == 0)
        fwrite(z, 1, len, f);
    fclose(f);
    return true;
}

/* one injection; 'kind' as in the README table. Returns true if applied. */
static bool cut(const char *kind, long arg)
{
    char a[1024], b[1024];
    long sz;

    if (!strcmp(kind, "store_tmp"))         /* books.dat removed, books.tmp left */
    {
        return os_path(a, sizeof(a), DIR "books.dat")
            && os_path(b, sizeof(b), DIR "books.tmp")
            && rename(a, b) == 0;
    }
    if (!strncmp(kind, "store_", 6) || !strncmp(kind, "pak_", 4)
        || !strncmp(kind, "idx_", 4))
    {
        const char *what = kind[0] == 's' ? "store"
                         : kind[0] == 'p' ? "pak" : "idx";
        const char *op = strchr(kind, '_') + 1;

        if (!os_path(a, sizeof(a), ab_name(what)))
            return false;
        sz = file_size(a);
        if (!strcmp(op, "drop"))
            return remove(a) == 0;
        if (sz < 0)
            return false;
        if (!strcmp(op, "tear"))            /* half a sector of zeros */
            return zero_range(a, (arg ? arg : sz / AB_SECTOR / 2) * AB_SECTOR,
                              AB_SECTOR / 2);
        if (!strcmp(op, "hdr0"))            /* first header lost */
            return zero_range(a, 0, AB_SECTOR);
        if (!strcmp(op, "hdr_both"))        /* both copies lost */
            return zero_range(a, 0, AB_SECTOR)
                && zero_range(a, sz - AB_SECTOR, AB_SECTOR);
        if (!strcmp(op, "trunc"))
            return truncate(a, (sz / 2) / AB_SECTOR * AB_SECTOR) == 0;
        if (!strcmp(op, "junk"))
        {
            FILE *f = fopen(a, "r+b");
            long i;

            if (!f)
                return false;
            for (i = 0; i < sz; i++)
                fputc((int)((i * 131 + 7) & 0xff), f);
            fclose(f);
            return true;
        }
        if (!strcmp(op, "flip"))            /* one byte in the header */
            return zero_range(a, 8, 4);
    }
    if (!strcmp(kind, "pak_tmp_left"))      /* a compaction died mid-copy */
    {
        FILE *f;

        if (!os_path(a, sizeof(a), DIR "books.pak.tmp"))
            return false;
        f = fopen(a, "wb");
        if (!f)
            return false;
        fwrite("torn compaction", 1, 15, f);
        fclose(f);
        return true;
    }
    return false;
}

#define MAX_DEFER 8
static struct { char kind[32]; long arg; } defer[MAX_DEFER];
static int ndefer;
static bool atexit_set;

static void run_deferred(void)
{
    int i;

    for (i = 0; i < ndefer; i++)
        fprintf(stderr, "sim_script: ab_power_cut exit %s %s\n", defer[i].kind,
                cut(defer[i].kind, defer[i].arg) ? "applied" : "no file");
}

/* books.idx header words: generation, count, pak_size; false = no index */
static bool idx_read(uint32_t *gen, uint32_t *cnt, uint32_t *pak)
{
    char a[1024];
    unsigned char h[32];
    FILE *f = os_path(a, sizeof(a), DIR "books.idx") ? fopen(a, "rb") : NULL;
    bool ok;

    *gen = *cnt = *pak = 0;
    if (!f)
        return false;
    ok = fread(h, 1, 32, f) == 32;
    if (ok)
    {
        memcpy(gen, h + IDX_GEN_OFF, 4);
        memcpy(cnt, h + IDX_COUNT_OFF, 4);
        memcpy(pak, h + IDX_PAK_OFF, 4);
    }
    fclose(f);
    return ok;
}

static uint32_t gen_mark = (uint32_t)-1, pak_mark;

static char copy_src[1024], copy_dst[1024];

static void run_copy(void)
{
    FILE *i = fopen(copy_src, "rb"), *o = i ? fopen(copy_dst, "wb") : NULL;
    char buf[4096];
    size_t n;

    while (o && (n = fread(buf, 1, sizeof(buf), i)) > 0)
        fwrite(buf, 1, n, o);
    if (i) fclose(i);
    if (o) fclose(o);
    fprintf(stderr, "sim_script: ab_copy_at_exit %s\n", o ? "done" : "failed");
}

static size_t mem_mark = (size_t)-1;

/* the store's state counts, read straight from books.dat; -1 on no file.
 * own: records seeded from the database's stats (a missing store's
 * migration, AB_REC_F_SEEDED) are left out of every count */
static int store_counts(int *started, int *finished, bool own)
{
    char a[1024];
    FILE *f;
    unsigned char sec[AB_SECTOR];
    uint32_t count = 0;
    long n = 0, i, r;

    *started = *finished = 0;
    if (!os_path(a, sizeof(a), DIR "books.dat") || !(f = fopen(a, "rb")))
        return -1;
    if (fread(sec, 1, AB_SECTOR, f) == AB_SECTOR)
    {
        uint32_t magic;

        memcpy(&magic, sec, 4);
        if (magic != AB_STORE_MAGIC     /* copy 0 lost: the last sector's */
            && (fseek(f, -AB_SECTOR, SEEK_END)
                || fread(sec, 1, AB_SECTOR, f) != AB_SECTOR))
            memset(sec, 0, sizeof(sec));
        memcpy(&magic, sec, 4);
        if (magic == AB_STORE_MAGIC)
            memcpy(&count, sec + HDR_COUNT_OFF, 4);
    }
    if (count > 2048)
        count = 0;
    for (i = 0; i < (long)count; i += AB_PER_SECT)
    {
        if (fseek(f, (1 + i / AB_PER_SECT) * AB_SECTOR, SEEK_SET)
            || fread(sec, 1, AB_SECTOR, f) != AB_SECTOR)
            break;
        for (r = 0; r < AB_PER_SECT && i + r < (long)count; r++)
        {
            uint16_t flags;

            memcpy(&flags, sec + r * AB_REC + 14, 2);
            if (!(flags >> 8))                  /* check byte 0: bad */
                continue;
            if (own && (flags & AB_F_SEEDED))
                continue;
            n++;
            if (flags & AB_F_FINISHED)
                (*finished)++;
            else if (flags & AB_F_STARTED)
                (*started)++;
        }
    }
    fclose(f);
    return (int)n;
}

/* "N" exact, "-1" any, "geN" at least N */
static bool want_ok(const char *a, int v)
{
    if (a[0] == 'g' && a[1] == 'e')
        return v >= atoi(a + 2);
    return atoi(a) < 0 || atoi(a) == v;
}

bool sim_cmd_ab(const char *cmd, char *argv[], int argc, int lineno)
{
    if (!strcmp(cmd, "ab_power_cut") && argc >= 3)
    {
        long arg = argc >= 4 ? atol(argv[3]) : 0;

        if (!strcmp(argv[1], "now"))
            fprintf(stderr, "sim_script: ab_power_cut now %s %s\n", argv[2],
                    cut(argv[2], arg) ? "applied" : "no file");
        else if (ndefer < MAX_DEFER)
        {
            /* "exit": applied after the clean shutdown's own flush, i.e.
             * what the disk holds when the power goes after the last write */
            strlcpy(defer[ndefer].kind, argv[2], sizeof(defer[0].kind));
            defer[ndefer++].arg = arg;
            if (!atexit_set)
            {
                atexit(run_deferred);
                atexit_set = true;
            }
            fprintf(stderr, "sim_script: ab_power_cut exit %s armed\n", argv[2]);
        }
        return true;
    }
    if (!strcmp(cmd, "ab_copy_at_exit") && argc >= 3)
    {
        /* installs a host file over a sim file after the clean shutdown's
         * own flush (the disk a next boot finds) */
        const char *dir = getenv("SIM_SCRIPT_DIR");

        snprintf(copy_src, sizeof(copy_src), "%s/%s", dir ? dir : ".", argv[1]);
        if (os_path(copy_dst, sizeof(copy_dst), argv[2]))
        {
            atexit(run_copy);
            fprintf(stderr, "sim_script: ab_copy_at_exit %s armed\n", argv[2]);
        }
        return true;
    }
    if (!strcmp(cmd, "ab_pak_mark"))
    {
        uint32_t c;

        if (!idx_read(&gen_mark, &c, &pak_mark))
            gen_mark = 0;
        fprintf(stderr, "sim_script: ab_pak_mark generation %lu\n",
                (unsigned long)gen_mark);
        return true;
    }
    if (!strcmp(cmd, "expect_ab_pak_same") || !strcmp(cmd, "expect_ab_pak_changed"))
    {
        /* same: the index generation does not move for <ms>; changed: it
         * moves within <ms> (books.pak built or rebuilt) */
        bool same = !strcmp(cmd, "expect_ab_pak_same");
        long ms = argc >= 2 ? atol(argv[1]) : 3000, waited = 0;
        uint32_t g = 0, c, k;
        bool moved = false;

        while (waited < ms)
        {
            idx_read(&g, &c, &k);
            if (gen_mark != (uint32_t)-1 && g != gen_mark)
            {
                moved = true;
                if (!same)
                    break;
            }
            sleep(HZ / 20);
            waited += 50;
        }
        if (same != moved && gen_mark != (uint32_t)-1)
            fprintf(stderr, "sim_script: %s ok: generation %lu (mark %lu)\n",
                    cmd, (unsigned long)g, (unsigned long)gen_mark);
        else
            fprintf(stderr, "sim_script:%d: %s failed: generation %lu, mark "
                    "%ld\n", lineno, cmd, (unsigned long)g, (long)gen_mark);
        return true;
    }
    if (!strcmp(cmd, "ab_touch") && argc >= 2)
    {
        char a[1024];
        struct timeval tv[2];

        gettimeofday(&tv[0], NULL);
        tv[0].tv_sec += 3600;           /* an hour ahead: surely newer */
        tv[1] = tv[0];
        if (os_path(a, sizeof(a), argv[1]) && utimes(a, tv) == 0)
            fprintf(stderr, "sim_script: ab_touch %s\n", argv[1]);
        else
            fprintf(stderr, "sim_script:%d: ab_touch failed: %s\n", lineno,
                    argv[1]);
        return true;
    }
    if (!strcmp(cmd, "ab_mark") && argc >= 2)
    {
        fprintf(stderr, "sim_script: ab_mark %s\n", argv[1]);
        return true;
    }
    if (!strcmp(cmd, "ab_mem_mark"))
    {
        mem_mark = core_available();
        fprintf(stderr, "sim_script: ab_mem_mark %lu\n", (unsigned long)mem_mark);
        return true;
    }
    if (!strcmp(cmd, "expect_ab_mem_same"))
    {
        size_t now = core_available();

        /* no allocation survived since the mark: the pool did not grow */
        if (mem_mark != (size_t)-1 && now >= mem_mark)
            fprintf(stderr, "sim_script: expect_ab_mem_same ok: %lu >= %lu\n",
                    (unsigned long)now, (unsigned long)mem_mark);
        else
            fprintf(stderr, "sim_script:%d: expect_ab_mem_same failed: "
                    "core_available %lu, mark %ld (grew by %ld B, or no mark)\n",
                    lineno, (unsigned long)now, (long)mem_mark,
                    (long)mem_mark - (long)now);
        return true;
    }
    if ((!strcmp(cmd, "expect_ab_store") || !strcmp(cmd, "expect_ab_store_own"))
        && argc >= 3)
    {
        bool own = cmd[15] == '_';
        int s = 0, f = 0, n = -1;
        long wait = argc >= 4 ? atol(argv[3]) : 2000, waited = 0;

        while (waited <= wait)
        {
            n = store_counts(&s, &f, own);
            if (n >= 0 && want_ok(argv[1], s) && want_ok(argv[2], f))
                break;
            sleep(HZ / 20);
            waited += 50;
        }
        if (n >= 0 && want_ok(argv[1], s) && want_ok(argv[2], f))
            fprintf(stderr, "sim_script: %s ok: started %d "
                    "finished %d\n", cmd, s, f);
        else
            fprintf(stderr, "sim_script:%d: %s failed: started %d "
                    "finished %d (valid records %d, -1 = no books.dat); "
                    "wanted %s %s\n", lineno, cmd, s, f, n, argv[1], argv[2]);
        return true;
    }
    if (!strcmp(cmd, "expect_ab_file") && argc >= 3)
    {
        const char *nm = ab_name(argv[1]);
        bool want = !strcmp(argv[2], "present");
        long wait = argc >= 4 ? atol(argv[3]) : 1000, waited = 0;
        char a[1024];
        bool have = false;

        while (nm && os_path(a, sizeof(a), nm))
        {
            have = file_size(a) >= 0;
            if (have == want || waited >= wait)
                break;
            sleep(HZ / 20);
            waited += 50;
        }
        if (nm && have == want)
            fprintf(stderr, "sim_script: expect_ab_file ok: %s %s\n", argv[1],
                    argv[2]);
        else
            fprintf(stderr, "sim_script:%d: expect_ab_file failed: %s is %s, "
                    "want %s\n", lineno, argv[1], have ? "present" : "absent",
                    argv[2]);
        return true;
    }
    if ((!strcmp(cmd, "expect_ab_pak") || !strcmp(cmd, "ab_pak_stat")))
    {
        long want = argc >= 2 ? atol(argv[1]) : 0;
        long wait = argc >= 3 ? atol(argv[2]) : 3000, waited = 0;
        uint32_t cnt = 0, pak = 0;
        bool stat = !strcmp(cmd, "ab_pak_stat");
        char a[1024];

        for (;;)
        {
            FILE *f = os_path(a, sizeof(a), DIR "books.idx")
                    ? fopen(a, "rb") : NULL;
            unsigned char h[32];

            cnt = pak = 0;
            if (f)
            {
                if (fread(h, 1, 32, f) == 32)
                {
                    memcpy(&cnt, h + IDX_COUNT_OFF, 4);
                    memcpy(&pak, h + IDX_PAK_OFF, 4);
                }
                fclose(f);
            }
            if (stat || (long)cnt >= want || waited >= wait)
                break;
            sleep(HZ / 20);
            waited += 50;
        }
        if (stat)
            fprintf(stderr, "sim_script: ab_pak_stat books=%lu pak_size=%lu\n",
                    (unsigned long)cnt, (unsigned long)pak);
        else if ((long)cnt >= want)
            fprintf(stderr, "sim_script: expect_ab_pak ok: %lu books in "
                    "books.idx\n", (unsigned long)cnt);
        else
            fprintf(stderr, "sim_script:%d: expect_ab_pak failed: %lu books "
                    "in books.idx, want >= %ld\n", lineno, (unsigned long)cnt,
                    want);
        return true;
    }
    return false;
}
