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
 * #85 library boot recovery sim commands; see sim_cmd_dbrec.h. They damage
 * only the simulator's test disk.
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "config.h"
#include "system.h"
#include "kernel.h"
#include "sim_cmd_dbrec.h"

#if defined(HAVE_IPOD_UI) && defined(HAVE_TC_RAMCACHE) && defined(HAVE_TAGCACHE)
#include "file.h"
#include "crc32.h"
#include "tagcache.h"

/* tagcache.c (SIMULATOR && UI_COMMIT): retries, prepares, active entries,
 * statistics recovering, fresh generation pending */
extern void sim_tagcache_recovery_state(long state[5]);
extern const char *sim_tagcache_selected(void);   /* this boot's choice */
extern long sim_tagcache_master_opens(void);      /* master index opens */
static long opens_mark;
extern int sim_open_audio_db_count(void);         /* filesystem-sim.c */
static int audio_db_mark;

#define DB "/.rockbox"
#define MARK DB "/dbrec.mark"
#define MANIFEST_BYTES 304

static const char *const gen_files[] = {
    "database_idx.tcd", "database_0.tcd", "database_1.tcd", "database_2.tcd",
    "database_3.tcd", "database_4.tcd", "database_5.tcd", "database_6.tcd",
    "database_7.tcd", "database_8.tcd", "database_12.tcd", "database_lib.dat"
};

struct mark {
    long count;
    uint32_t manifest[2];           /* fingerprints of the manifest files */
    uint64_t seq[2];
    int gen[2];                     /* -1: not an intact manifest */
    uint32_t dir[4];                /* gen0..3 */
    uint32_t root;                  /* root idx + 0..3 */
};

static void unesc(char *p)
{
    char *r = p, *w = p;

    while (*r)
    {
        if (r[0] == '%' && r[1] == '2' && r[2] == '0')
        {
            *w++ = ' ';
            r += 3;
        }
        else
            *w++ = *r++;
    }
    *w = '\0';
}

static uint32_t fnv(uint32_t h, const void *p, size_t n)
{
    const unsigned char *b = p;

    while (n--)
        h = (h ^ *b++) * 16777619u;
    return h;
}

static uint32_t file_fp(uint32_t h, const char *path)
{
    unsigned char buf[4096];
    ssize_t n;
    int fd = open(path, O_RDONLY);

    if (fd < 0)
        return fnv(h, "missing", 7);
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        h = fnv(h, buf, n);
    close(fd);
    return fnv(h, "end", 3);
}

static uint32_t dir_fp(const char *dir, int files)
{
    char path[MAX_PATH];
    uint32_t h = 2166136261u;

    for (int i = 0; i < files; i++)
    {
        snprintf(path, sizeof(path), "%s/%s", dir, gen_files[i]);
        h = file_fp(h, path);
    }
    return h;
}

static void gen_dir(char *out, size_t size, int gen)
{
    snprintf(out, size, DB "/database_gen%d", gen);
}

static void manifest_path(char *out, size_t size, int slot)
{
    snprintf(out, size, DB "/database_manifest%d.dat", slot);
}

/* Intact manifest of this version: its gen, else -1 */
static int manifest_read(int slot, unsigned char m[MANIFEST_BYTES])
{
    char path[MAX_PATH];
    uint32_t magic, version, gen, sum, saved;
    int fd;
    bool ok;

    manifest_path(path, sizeof(path), slot);
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    ok = read(fd, m, MANIFEST_BYTES) == MANIFEST_BYTES;
    close(fd);
    if (!ok)
        return -1;
    memcpy(&magic, m, 4);
    memcpy(&version, m + 4, 4);
    memcpy(&gen, m + 16, 4);
    memcpy(&saved, m + 296, 4);
    memset(m + 296, 0, 4);
    sum = ~crc_32r(m, MANIFEST_BYTES, ~0u);
    memcpy(m + 296, &saved, 4);
    return magic == 0x47445049u && version == 1 && sum == saved && gen <= 3
           ? (int)gen : -1;
}

static long active_count(void)
{
    long st[5];

    sim_tagcache_recovery_state(st);
    return st[2];
}

static void take(struct mark *mk, bool count)
{
    char path[MAX_PATH];
    unsigned char m[MANIFEST_BYTES];

    if (count)
        mk->count = active_count();
    for (int i = 0; i < 2; i++)
    {
        manifest_path(path, sizeof(path), i);
        mk->manifest[i] = file_fp(2166136261u, path);
        mk->gen[i] = manifest_read(i, m);
        memcpy(&mk->seq[i], m + 8, 8);
        if (mk->gen[i] < 0)
            mk->seq[i] = 0;
    }
    for (int g = 0; g < 4; g++)
    {
        gen_dir(path, sizeof(path), g);
        mk->dir[g] = dir_fp(path, ARRAYLEN(gen_files));
    }
    mk->root = dir_fp(DB, 5);
}

static bool mark_load(struct mark *mk)
{
    int fd = open(MARK, O_RDONLY);
    bool ok = fd >= 0 && read(fd, mk, sizeof(*mk)) == sizeof(*mk);

    if (fd >= 0)
        close(fd);
    return ok;
}

static void mark_save(const struct mark *mk)
{
    int fd = open(MARK, O_WRONLY | O_CREAT | O_TRUNC, 0666);

    if (fd >= 0)
    {
        write(fd, mk, sizeof(*mk));
        close(fd);
    }
}

static int newest_slot(const struct mark *mk)
{
    if (mk->gen[0] < 0)
        return mk->gen[1] < 0 ? -1 : 1;
    if (mk->gen[1] < 0)
        return 0;
    return mk->seq[1] > mk->seq[0] ? 1 : 0;
}

static bool flip(const char *path, long offset)
{
    unsigned char b;
    int fd = open(path, O_RDWR);
    bool ok = fd >= 0;

    if (ok && offset < 0)
        offset = lseek(fd, 0, SEEK_END) / 2;
    ok = ok && lseek(fd, offset, SEEK_SET) == offset && read(fd, &b, 1) == 1;
    b ^= 0x40;
    ok = ok && lseek(fd, offset, SEEK_SET) == offset && write(fd, &b, 1) == 1;
    if (fd >= 0)
        close(fd);
    return ok;
}

static bool corrupt_gen(int gen)
{
    char path[MAX_PATH];

    snprintf(path, sizeof(path), DB "/database_gen%d/database_3.tcd", gen);
    return gen >= 0 && flip(path, -1);
}

static bool copy(const char *from, const char *to)
{
    unsigned char buf[4096];
    ssize_t n;
    int in = open(from, O_RDONLY);
    int out = in >= 0 ? open(to, O_WRONLY | O_CREAT | O_TRUNC, 0666) : -1;
    bool ok = out >= 0;

    while (ok && (n = read(in, buf, sizeof(buf))) > 0)
        ok = write(out, buf, n) == n;
    if (in >= 0)
        close(in);
    if (out >= 0)
        close(out);
    return ok;
}

static bool partial_legacy(char *track, const char *rating)
{
    const char *active = tagcache_ui_db_path_ctx(tagcache_context_active());
    char from[MAX_PATH], to[MAX_PATH], line[MAX_PATH + 64];
    bool ok = true;
    int fd;

    unesc(track);
    for (int i = 0; i < (int)ARRAYLEN(gen_files); i++)
    {
        snprintf(to, sizeof(to), DB "/%s", gen_files[i]);
        if (i < 5)
        {
            snprintf(from, sizeof(from), "%s/%s", active, gen_files[i]);
            ok = ok && strcmp(active, DB) && copy(from, to);
        }
        else
            remove(to);
    }
    remove(DB "/database_manifest0.dat");
    remove(DB "/database_manifest1.dat");
    remove(DB "/database_stats.jnl");
    remove(DB "/database_stats.prev");
    remove(DB "/database_stats.tmp");
    remove(DB "/database_stats.bad");
    fd = open(DB "/database_changelog.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    snprintf(line, sizeof(line), "filename=\"%s\" rating=\"%s\" \n## end\n",
             track, rating);
    ok = ok && fd >= 0 && write(fd, line, strlen(line)) == (ssize_t)strlen(line);
    if (fd >= 0)
        close(fd);
    return ok;
}

/* The newest checkpoint file (highest header base) and its last full frame */
static bool corrupt_journal(void)
{
    static const char *const names[2] = {
        DB "/database_stats.jnl", DB "/database_stats.prev" };
    uint64_t best = 0;
    int pick = -1;

    for (int i = 0; i < 2; i++)
    {
        unsigned char h[24];
        uint64_t base;
        int fd = open(names[i], O_RDONLY);
        if (fd < 0)
            continue;
        if (read(fd, h, sizeof(h)) == (ssize_t)sizeof(h))
        {
            memcpy(&base, h + 8, 8);
            if (pick < 0 || base > best)
            {
                best = base;
                pick = i;
            }
        }
        close(fd);
    }
    if (pick < 0)
        return false;
    int fd = open(names[pick], O_RDONLY);
    long size = fd >= 0 ? lseek(fd, 0, SEEK_END) : 0, pos = 24, last = -1;
    while (fd >= 0 && pos + 40 <= size)
    {
        uint32_t bytes;
        if (lseek(fd, pos + 8, SEEK_SET) != pos + 8
            || read(fd, &bytes, 4) != 4 || bytes < 40 || pos + bytes > size)
            break;
        last = pos;
        pos += bytes;
    }
    if (fd >= 0)
        close(fd);
    fprintf(stderr, "sim_script: dbrec journal %s frame at %ld\n",
            names[pick], last);
    return last >= 0 && flip(names[pick], last + 41);
}

static void verdict(const char *what, bool ok, int lineno, const char *detail)
{
    if (ok)
        fprintf(stderr, "sim_script: dbrec expect_%s ok %s\n", what, detail);
    else
        fprintf(stderr, "sim_script:%d: dbrec expect_%s failed %s\n", lineno,
                what, detail);
}

static bool expect_active(const struct mark *mk, const char *which,
                          const char *active)
{
    char path[MAX_PATH];
    int newest = newest_slot(mk), gen = -1;

    if (!strcmp(which, "newest") && newest >= 0)
        gen = mk->gen[newest];
    else if (!strcmp(which, "older") && newest >= 0)
        gen = mk->gen[newest ^ 1];
    else if (!strncmp(which, "gen", 3))
        gen = atoi(which + 3);
    else if (!strcmp(which, "unreferenced"))
    {
        for (int g = 0; g < 4; g++)
        {
            gen_dir(path, sizeof(path), g);
            if (!strcmp(path, active))
                return g != mk->gen[0] && g != mk->gen[1];
        }
        return false;
    }
    if (gen < 0)
        return false;
    gen_dir(path, sizeof(path), gen);
    return !strcmp(path, active);
}

static bool expect_unchanged(const struct mark *mk, const char *which)
{
    struct mark now;
    int newest = newest_slot(mk);

    take(&now, false);
    if (!strcmp(which, "root"))
        return now.root == mk->root;
    if (!strcmp(which, "manifests"))
        return now.manifest[0] == mk->manifest[0]
            && now.manifest[1] == mk->manifest[1];
    if (!strcmp(which, "older"))
        return newest >= 0 && mk->gen[newest ^ 1] >= 0
            && now.dir[mk->gen[newest ^ 1]] == mk->dir[mk->gen[newest ^ 1]]
            && now.manifest[newest ^ 1] == mk->manifest[newest ^ 1];
    if (!strcmp(which, "referenced"))
    {
        /* every directory a manifest slot referenced at the mark is
         * never rewritten while that slot still holds its old manifest
         * (intact or foreign); once a valid manifest in the slot names
         * another directory (a salvaged copy or a new build published),
         * the old one may be reused, e.g. by the boot scan's clone */
        bool any = false;
        for (int i = 0; i < 2; i++)
            if (mk->gen[i] >= 0)
            {
                any = true;
                bool superseded = now.gen[i] >= 0 && now.gen[i] != mk->gen[i];
                if (!superseded && now.dir[mk->gen[i]] != mk->dir[mk->gen[i]])
                    return false;
            }
        return any;
    }
    return false;
}

bool sim_cmd_dbrec(const char *cmd, char *argv[], int argc, int lineno)
{
    struct mark mk;
    char detail[MAX_PATH + 64];
    long st[5];
    bool ok;

    /* P16 gate fix: "db_opens_mark" remembers the master index opens so
     * far; "expect_db_opens <max>": at most 'max' since the mark (a
     * paused window deciding a reload's fit must not touch the disk):
     * "expect_db_opens ok n=N" / "... failed" */
    if (!strcmp(cmd, "db_opens_mark"))
    {
        opens_mark = sim_tagcache_master_opens();
        fprintf(stderr, "sim_script: db_opens_mark %ld\n", opens_mark);
        return true;
    }
    if (!strcmp(cmd, "expect_db_opens") && argc >= 2)
    {
        long n = sim_tagcache_master_opens() - opens_mark;

        if (n <= atol(argv[1]))
            fprintf(stderr, "sim_script: expect_db_opens ok n=%ld\n", n);
        else
            fprintf(stderr, "sim_script:%d: expect_db_opens failed: n=%ld "
                    "max=%s\n", lineno, n, argv[1]);
        return true;
    }
    if (strcmp(cmd, "dbrec") || argc < 2)
        return false;
    const char *sub = argv[1];
    memset(&mk, 0, sizeof(mk));
    bool have = mark_load(&mk);
    sim_tagcache_recovery_state(st);
    snprintf(detail, sizeof(detail), "(active=%s n=%ld retries=%ld "
             "prepares=%ld recovering=%ld fresh=%ld)",
             tagcache_ui_db_path_ctx(tagcache_context_active()), st[2], st[0],
             st[1], st[3], st[4]);

    if (!strcmp(sub, "mark") || !strcmp(sub, "mark_files"))
    {
        take(&mk, !strcmp(sub, "mark") || !have);
        mark_save(&mk);
        fprintf(stderr, "sim_script: dbrec %s n=%ld gens=%d/%d seq=%lu/%lu\n",
                sub, mk.count, mk.gen[0], mk.gen[1],
                (unsigned long)mk.seq[0], (unsigned long)mk.seq[1]);
        return true;
    }
    if (!strcmp(sub, "corrupt_newest") || !strcmp(sub, "corrupt_referenced")
        || !strcmp(sub, "truncate_newest_manifest")
        || !strcmp(sub, "foreign_manifests") || !strcmp(sub, "corrupt_journal")
        || !strcmp(sub, "partial_legacy") || !strcmp(sub, "set_rating"))
    {
        int newest = newest_slot(&mk);
        char path[MAX_PATH];

        if (!strcmp(sub, "corrupt_newest"))
            ok = have && newest >= 0 && corrupt_gen(mk.gen[newest]);
        else if (!strcmp(sub, "corrupt_referenced"))
            ok = have && corrupt_gen(mk.gen[0]) && corrupt_gen(mk.gen[1]);
        else if (!strcmp(sub, "truncate_newest_manifest"))
        {
            manifest_path(path, sizeof(path), newest);
            int fd = newest >= 0 ? open(path, O_RDWR) : -1;
            ok = fd >= 0 && ftruncate(fd, 100) == 0;
            if (fd >= 0)
                close(fd);
        }
        else if (!strcmp(sub, "foreign_manifests"))
        {
            ok = true;
            for (int i = 0; i < 2; i++)
            {
                unsigned char m[MANIFEST_BYTES];
                uint32_t version = 2, sum;
                ok = ok && manifest_read(i, m) >= 0;
                memcpy(m + 4, &version, 4);
                memset(m + 296, 0, 4);
                sum = ~crc_32r(m, MANIFEST_BYTES, ~0u);
                memcpy(m + 296, &sum, 4);
                manifest_path(path, sizeof(path), i);
                int fd = ok ? open(path, O_WRONLY | O_TRUNC) : -1;
                ok = fd >= 0 && write(fd, m, sizeof(m)) == sizeof(m);
                if (fd >= 0)
                    close(fd);
            }
        }
        else if (!strcmp(sub, "corrupt_journal"))
            ok = corrupt_journal();
        else if (!strcmp(sub, "partial_legacy"))
            ok = argc >= 4 && partial_legacy(argv[2], argv[3]);
        else
        {
            ok = argc >= 4;
            if (ok)
            {
                unsigned char tag = tag_rating;
                long value = atol(argv[3]);
                unesc(argv[2]);
                ok = tagcache_ui_update_stats(argv[2], -1, 0, 1, &tag, &value);
            }
        }
        if (ok)
            fprintf(stderr, "sim_script: dbrec %s done\n", sub);
        else    /* a setup failure must fail the run, too */
            fprintf(stderr, "sim_script:%d: dbrec expect_%s failed\n",
                    lineno, sub);
        return true;
    }
    if (!strcmp(sub, "expect_not_updating"))
    {
        long ms = argc >= 3 ? atol(argv[2]) : 30000, t = 0;
        while (t < ms && (tagcache_ui_busy() || !tagcache_is_usable()))
        {
            sleep(HZ / 10);
            t += 100;
        }
        verdict("not_updating", !tagcache_ui_busy() && tagcache_is_usable(),
                lineno, detail);
        return true;
    }
    if (!strcmp(sub, "expect_published"))
    {
        long ms = argc >= 3 ? atol(argv[2]) : 30000, t = 0;
        for (;;)
        {
            const char *active =
                tagcache_ui_db_path_ctx(tagcache_context_active());
            unsigned char m[MANIFEST_BYTES];
            char path[MAX_PATH];
            ok = false;
            for (int i = 0; i < 2 && !ok; i++)
            {
                int g = manifest_read(i, m);
                gen_dir(path, sizeof(path), g);
                ok = g >= 0 && !strcmp(path, active);
            }
            if (ok || t >= ms)
                break;
            sleep(HZ / 10);
            t += 100;
        }
        verdict("published", ok, lineno, detail);
        return true;
    }
    if (!strcmp(sub, "expect_journal_bad"))
    {
        int fd = open(DB "/database_stats.bad", O_RDONLY);
        verdict("journal_bad", fd >= 0, lineno, detail);
        if (fd >= 0)
            close(fd);
        return true;
    }
    if (!strcmp(sub, "expect_retries") && argc >= 3)
    {
        verdict("retries", st[0] <= atol(argv[2]), lineno, detail);
        return true;
    }
    if (!strcmp(sub, "expect_prepares") && argc >= 3)
    {
        verdict("prepares", st[1] <= atol(argv[2]), lineno, detail);
        return true;
    }
    if (!strcmp(sub, "audio_opens_mark"))
    {
        /* B-H1: a track change's statistics open no database file on the
         * audio thread (they bind from RAM or go to the tagcache thread) */
        audio_db_mark = sim_open_audio_db_count();
        fprintf(stderr, "sim_script: dbrec audio_opens_mark %d\n",
                audio_db_mark);
    }
    else if (!strcmp(sub, "expect_audio_db_opens") && argc >= 3)
    {
        int n = sim_open_audio_db_count() - audio_db_mark;
        snprintf(detail, sizeof(detail), "(audio_db_opens=%d)", n);
        verdict("audio_db_opens", n <= atoi(argv[2]), lineno, detail);
    }
    else if (!strcmp(sub, "expect_playcount_ge") && argc >= 4)
    {
        struct tagcache_search tcs;
        long v = -2;
        unesc(argv[2]);
        if (tagcache_find_index(&tcs, argv[2]))
        {
            v = tagcache_get_numeric(&tcs, tag_playcount);
            tagcache_search_finish(&tcs);
        }
        snprintf(detail, sizeof(detail), "(playcount=%ld)", v);
        verdict("playcount_ge", v >= atol(argv[3]), lineno, detail);
    }
    if (!strcmp(sub, "audio_opens_mark") || !strcmp(sub, "expect_audio_db_opens")
        || !strcmp(sub, "expect_playcount_ge"))
        return true;            /* these need no mark (a fresh disk has none) */
    if (!have)
    {
        fprintf(stderr, "sim_script:%d: dbrec expect_mark failed: no mark\n",
                lineno);
        return true;
    }
    if ((!strcmp(sub, "expect_active") || !strcmp(sub, "expect_selected"))
        && argc >= 3)
    {
        /* selected: this boot's choice; active: polled up to [ms] */
        bool selected = !strcmp(sub, "expect_selected");
        long ms = argc >= 4 ? atol(argv[3]) : 0, t = 0;
        while (!(ok = expect_active(&mk, argv[2], selected
                ? sim_tagcache_selected()
                : tagcache_ui_db_path_ctx(tagcache_context_active())))
               && t < ms)
        {
            sleep(HZ / 10);
            t += 100;
        }
        snprintf(detail + strlen(detail), sizeof(detail) - strlen(detail),
                 " selected=%s", sim_tagcache_selected());
        verdict(selected ? "selected" : "active", ok, lineno, detail);
    }
    else if (!strcmp(sub, "expect_unchanged") && argc >= 3)
        verdict("unchanged", expect_unchanged(&mk, argv[2]), lineno, argv[2]);
    else if (!strcmp(sub, "expect_slots_changed") && argc >= 3)
    {
        struct mark now;
        take(&now, false);
        int n = (now.manifest[0] != mk.manifest[0])
              + (now.manifest[1] != mk.manifest[1]);
        verdict("slots_changed", n == atoi(argv[2]), lineno, detail);
    }
    else if (!strcmp(sub, "expect_count_ge"))
        verdict("count_ge", st[2] >= mk.count, lineno, detail);
    else
        fprintf(stderr, "sim_script:%d: bad dbrec command '%s'\n", lineno, sub);
    return true;
}

#else
bool sim_cmd_dbrec(const char *cmd, char *argv[], int argc, int lineno)
{
    (void)cmd; (void)argv; (void)argc; (void)lineno;
    return false;
}
#endif
