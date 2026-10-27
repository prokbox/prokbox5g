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
 * Database generation host tests
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
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "dbgen.h"

static unsigned checks;
#define CHECK(c) do { checks++; if (!(c)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

struct master { int32_t commitid, entries; uint32_t structural, stats; };
static unsigned char work[16384]; /* Host-only caller buffer. */
static bool stop;
static int cancel_after = -1;
static enum dbgen_result validation_fault;
static char io_dir[DBGEN_PATH_MAX];      /* this directory reads as IO */
static const struct dbgen_file files[] = {
    { "database_idx.tcd", false, true },
    { "database_0.tcd", true, true },
    { "database_lib.dat", true, false }
};

static void path(char *out, const char *dir, const char *file)
{ assert(snprintf(out, DBGEN_PATH_MAX, "%s/%s", dir, file) < DBGEN_PATH_MAX); }
static void put(const char *dir, const char *file, const void *data, size_t n)
{
    char p[DBGEN_PATH_MAX]; path(p, dir, file);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    assert(fd >= 0 && write(fd, data, n) == (ssize_t)n && close(fd) == 0);
}
static size_t get(const char *dir, const char *file, void *data, size_t n)
{
    char p[DBGEN_PATH_MAX]; path(p, dir, file);
    int fd = open(p, O_RDONLY); assert(fd >= 0);
    ssize_t got = read(fd, data, n); assert(got >= 0 && close(fd) == 0);
    return (size_t)got;
}
static void fill(const char *dir, int id)
{
    struct master m = { id, 3, 0x12340000u + id, 100 };
    put(dir, files[0].name, &m, sizeof(m));
    unsigned char strings[32768];
    memset(strings, id, sizeof(strings));
    put(dir, files[1].name, strings, sizeof(strings));
    put(dir, files[2].name, &m, sizeof(m));
}
static bool cancel(void *opaque)
{
    (void)opaque;
    if (stop) return true;
    if (cancel_after == 0) return true;
    if (cancel_after > 0) cancel_after--;
    return false;
}
static uint32_t fingerprint(const char *dir)
{
    uint32_t sum = 2166136261u;
    unsigned char data[32768];
    for (unsigned i = 0; i < sizeof(files) / sizeof(files[0]); i++)
    {
        size_t n = get(dir, files[i].name, data, sizeof(data));
        sum = (sum ^ (uint32_t)n) * 16777619u;
        for (size_t b = 0; b < n; b++) sum = (sum ^ data[b]) * 16777619u;
    }
    return sum;
}
static enum dbgen_result validate(const char *dir, int32_t id, int32_t entries,
                                  uint32_t *sum, int32_t *found, void *opaque)
{
    (void)opaque;
    if (validation_fault) return validation_fault;
    if (io_dir[0] && !strcmp(dir, io_dir)) return DBGEN_IO;
    struct master m, lib;
    char p[DBGEN_PATH_MAX];
    path(p, dir, files[0].name);
    int fd = open(p, O_RDONLY);
    if (fd < 0) return DBGEN_MISSING;
    ssize_t n = read(fd, &m, sizeof(m)); close(fd);
    if (n != sizeof(m)) return DBGEN_CORRUPT;
    path(p, dir, files[2].name);
    fd = open(p, O_RDONLY);
    if (fd < 0) return DBGEN_MISSING;
    n = read(fd, &lib, sizeof(lib)); close(fd);
    if (n != sizeof(lib) || (id >= 0 && id != m.commitid)
        || (entries >= 0 && entries != m.entries)
        || lib.commitid != m.commitid || lib.entries != m.entries)
        return DBGEN_CORRUPT;
    *sum = m.structural;
    *found = m.commitid;
    return DBGEN_OK;
}
/* Rewrite one manifest field and re-seal its CRC (an intact foreign one). */
static uint32_t crc32_iso(const unsigned char *p, size_t n)
{
    uint32_t c = ~0u;
    while (n--)
    {
        c ^= *p++;
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & -(c & 1));
    }
    return ~c;
}
static void reseal(const char *root, const char *name, size_t offset, uint32_t value)
{
    unsigned char m[304];
    assert(get(root, name, m, sizeof(m)) == sizeof(m));
    memcpy(m + offset, &value, 4);
    memset(m + 296, 0, 4);
    uint32_t sum = crc32_iso(m, sizeof(m));
    memcpy(m + 296, &sum, 4);
    put(root, name, m, sizeof(m));
}
static struct dbgen_ops ops = { files, 3, validate, cancel, NULL };

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    char root[DBGEN_PATH_MAX], active[DBGEN_PATH_MAX], candidate[DBGEN_PATH_MAX];
    char gen0[DBGEN_PATH_MAX], gen1[DBGEN_PATH_MAX], gen2[DBGEN_PATH_MAX];
    path(root, argv[1], "root"); CHECK(mkdir(root, 0777) == 0);
    path(gen0, root, "database_gen0"); path(gen1, root, "database_gen1");
    path(gen2, root, "database_gen2");

    CHECK(dbgen_select(root, active, work, sizeof(work), &ops) == DBGEN_LEGACY);
    CHECK(!strcmp(active, root));
    CHECK(dbgen_prepare(root, active, candidate, work, sizeof(work), &ops) == DBGEN_OK);
    CHECK(!strcmp(candidate, gen0));
    fill(candidate, 1);
    CHECK(dbgen_publish(root, candidate, 1, 3, work, sizeof(work), &ops) == DBGEN_OK);
    CHECK(dbgen_select(root, active, work, sizeof(work), &ops) == DBGEN_OK);
    CHECK(!strcmp(active, gen0));
    CHECK(dbgen_prepare(root, active, candidate, work, sizeof(work), &ops) == DBGEN_OK);
    CHECK(!strcmp(candidate, gen1));
    char p[DBGEN_PATH_MAX]; path(p, candidate, files[2].name);
    CHECK(access(p, F_OK) != 0); /* Old precompute must not leak into candidate. */
    fill(candidate, 2);
    CHECK(dbgen_publish(root, candidate, 2, 3, work, sizeof(work), &ops) == DBGEN_OK);
    CHECK(dbgen_select(root, active, work, sizeof(work), &ops) == DBGEN_OK);
    CHECK(!strcmp(active, gen1));

    struct master m = { 2, 3, 0x12340002, 9999 };
    put(gen1, files[0].name, &m, sizeof(m));
    CHECK(dbgen_select(root, active, work, sizeof(work), &ops) == DBGEN_OK);
    CHECK(!strcmp(active, gen1)); /* Mutable statistics do not invalidate. */
    m.structural++;
    put(gen1, files[0].name, &m, sizeof(m));
    CHECK(dbgen_select(root, active, work, sizeof(work), &ops) == DBGEN_OK);
    CHECK(!strcmp(active, gen0)); /* Structural master corruption does. */
    fill(gen1, 2);
    put(gen1, files[1].name, "bad", 3);
    CHECK(dbgen_select(root, active, work, sizeof(work), &ops) == DBGEN_OK);
    CHECK(!strcmp(active, gen0));
    fill(gen1, 2);
    put(gen1, files[2].name, &m, sizeof(m)); m.commitid++;
    put(gen1, files[2].name, &m, sizeof(m));
    CHECK(dbgen_select(root, active, work, sizeof(work), &ops) == DBGEN_OK);
    CHECK(!strcmp(active, gen0)); /* No mixed-generation precompute. */
    fill(gen1, 2);

    unsigned char manifests[2][1024]; size_t lens[2];
    lens[0] = get(root, "database_manifest0.dat", manifests[0], sizeof(manifests[0]));
    lens[1] = get(root, "database_manifest1.dat", manifests[1], sizeof(manifests[1]));
    put(root, "database_manifest1.dat", "torn", 4);
    CHECK(dbgen_select(root, active, work, sizeof(work), &ops) == DBGEN_OK);
    CHECK(!strcmp(active, gen0));
    put(root, "database_manifest0.dat", "torn", 4);
    /* Both torn: salvage the newest structurally valid generation, not
     * an empty library and never the stale legacy root. */
    CHECK(dbgen_select(root, active, work, sizeof(work), &ops) == DBGEN_SALVAGED);
    CHECK(!strcmp(active, gen1));
    put(root, "database_manifest0.dat", manifests[0], lens[0]);
    put(root, "database_manifest1.dat", manifests[1], lens[1]);
    CHECK(dbgen_select(root, active, work, sizeof(work), &ops) == DBGEN_OK);

    stop = true;
    CHECK(dbgen_prepare(root, active, candidate, work, sizeof(work), &ops) == DBGEN_CANCELLED);
    stop = false;
    CHECK(dbgen_prepare(root, active, candidate, work, sizeof(work), &ops) == DBGEN_OK);
    CHECK(!strcmp(candidate, gen2));
    fill(candidate, 3);
    CHECK(dbgen_publish(root, gen1, 2, 3, work, sizeof(work), &ops) == DBGEN_BUSY);
    validation_fault = DBGEN_IO;
    CHECK(dbgen_publish(root, candidate, 3, 3, work, sizeof(work), &ops) == DBGEN_IO);
    validation_fault = 0;

    /* Interrupt every operation through publication. Fault masks enumerate
     * single deterministic failure positions, including >one bulk chunk. */
    unsigned injected = 0;
    for (unsigned op = 0; op < DBGEN_TEST_NOPS; op++)
    for (unsigned nth = 0; nth < 35; nth++)
    {
        dbgen_test_fail(op, UINT64_C(1) << nth, op == DBGEN_TEST_WRITE ? ENOSPC : EIO);
        enum dbgen_result rc = dbgen_publish(root, candidate, 3, 3, work, sizeof(work), &ops);
        dbgen_test_reset();
        CHECK(rc == DBGEN_OK || rc == DBGEN_IO);
        CHECK(dbgen_select(root, active, work, sizeof(work), &ops) == DBGEN_OK);
        CHECK(!strcmp(active, gen1) || !strcmp(active, gen2));
        /* Both retained generations remain byte-valid after all failures. */
        put(root, "database_manifest0.dat", manifests[0], lens[0]);
        put(root, "database_manifest1.dat", manifests[1], lens[1]);
        CHECK(dbgen_select(root, active, work, sizeof(work), &ops) == DBGEN_OK);
        CHECK(!strcmp(active, gen1));
        if (rc != DBGEN_OK) injected++;
    }

    /* Copy failures never write either retained generation or publish. */
    for (unsigned op = 0; op < DBGEN_TEST_NOPS; op++)
    {
        dbgen_test_fail(op, UINT64_C(1), op == DBGEN_TEST_WRITE ? ENOSPC : EIO);
        enum dbgen_result rc = dbgen_prepare(root, active, candidate, work, sizeof(work), &ops);
        dbgen_test_reset();
        CHECK(rc == DBGEN_OK || rc == DBGEN_IO);
        CHECK(dbgen_select(root, active, work, sizeof(work), &ops) == DBGEN_OK);
        CHECK(!strcmp(active, gen1));
    }
    dbgen_test_fail(DBGEN_TEST_OPEN, 3, EIO);
    CHECK(dbgen_select(root, active, work, sizeof(work), &ops) == DBGEN_IO);
    dbgen_test_reset();
    CHECK(!*active);
    CHECK(dbgen_prepare(root, gen1, candidate, work, 100, &ops) == DBGEN_INVALID);

    /* If newest content becomes invalid, publication must replace THAT
     * manifest, not the older manifest that still provides valid recovery. */
    CHECK(dbgen_prepare(root, gen1, candidate, work, sizeof(work), &ops) == DBGEN_OK);
    fill(candidate, 3);
    put(gen1, files[1].name, "bad", 3);
    CHECK(dbgen_publish(root, candidate, 3, 3, work, sizeof(work), &ops) == DBGEN_OK);
    unsigned char check[1024];
    CHECK(get(root, "database_manifest0.dat", check, sizeof(check)) == lens[0]);
    CHECK(!memcmp(check, manifests[0], lens[0]));
    put(root, "database_manifest1.dat", "torn", 4);
    CHECK(dbgen_select(root, active, work, sizeof(work), &ops) == DBGEN_OK);
    CHECK(!strcmp(active, gen0));

    /* Bulk staging does not activate the candidate. Final publication is
     * a single rename; every expensive read/flush has already completed. */
    CHECK(dbgen_prepare(root, active, candidate, work, sizeof(work), &ops) == DBGEN_OK);
    fill(candidate, 4);
    struct dbgen_publication publication;
    CHECK(dbgen_stage_publish(root, candidate, 4, 3, work, sizeof(work),
                              &ops, &publication) == DBGEN_OK);
    CHECK(dbgen_select(root, active, work, sizeof(work), &ops) == DBGEN_OK);
    CHECK(!strcmp(active, gen0));
    dbgen_test_fail(DBGEN_TEST_READ, 1, EIO);
    dbgen_test_fail(DBGEN_TEST_FLUSH, 1, EIO);
    CHECK(dbgen_finish_publish(&publication) == DBGEN_OK);
    dbgen_test_reset();
    CHECK(dbgen_finish_publish(&publication) == DBGEN_INVALID);
    CHECK(dbgen_select(root, active, work, sizeof(work), &ops) == DBGEN_OK);
    CHECK(!strcmp(active, candidate));

    /* A partial legacy database is not a fresh database. */
    char partial[DBGEN_PATH_MAX]; path(partial, argv[1], "partial");
    CHECK(mkdir(partial, 0777) == 0);
    put(partial, files[1].name, "orphan", 6);
    CHECK(dbgen_prepare(partial, partial, candidate, work, sizeof(work), &ops) == DBGEN_MISSING);
    CHECK(!*candidate);
    CHECK(dbgen_prepare_empty(partial, partial, candidate, work, sizeof(work), &ops) == DBGEN_OK);
    unsigned char orphan[6];
    CHECK(get(partial, files[1].name, orphan, sizeof(orphan)) == sizeof(orphan));
    CHECK(!memcmp(orphan, "orphan", sizeof(orphan)));

    /* Both manifests are intact, but neither retained generation validates.
     * Explicit fresh recovery may clear only the unreferenced third; preserve
     * every retained byte and both manifests for non-destructive salvage. */
    put(root, "database_manifest0.dat", manifests[0], lens[0]);
    put(root, "database_manifest1.dat", manifests[1], lens[1]);
    fill(gen0, 1); fill(gen1, 2);
    put(gen0, files[1].name, "bad-zero", 8);
    put(gen1, files[1].name, "bad-one", 7);
    uint32_t retained[2] = { fingerprint(gen0), fingerprint(gen1) };
    CHECK(dbgen_select(root, active, work, sizeof(work), &ops) == DBGEN_CORRUPT);
    CHECK(!*active);
    put(gen2, "unrelated.keep", "keep", 4);
    unsigned fresh_faults = 0;
    for (unsigned op = 0; op < DBGEN_TEST_NOPS; op++)
    for (unsigned nth = 0; nth < 8; nth++)
    {
        fill(gen2, 88);
        dbgen_test_fail(op, UINT64_C(1) << nth, EIO);
        enum dbgen_result rc = dbgen_prepare_empty(root, root, candidate,
                                                   work, sizeof(work), &ops);
        dbgen_test_reset();
        CHECK(rc == DBGEN_OK || rc == DBGEN_IO);
        CHECK(fingerprint(gen0) == retained[0] && fingerprint(gen1) == retained[1]);
        CHECK(get(root, "database_manifest0.dat", check, sizeof(check)) == lens[0]);
        CHECK(!memcmp(check, manifests[0], lens[0]));
        CHECK(get(root, "database_manifest1.dat", check, sizeof(check)) == lens[1]);
        CHECK(!memcmp(check, manifests[1], lens[1]));
        if (rc == DBGEN_OK)
        {
            CHECK(!strcmp(candidate, gen2));
            for (unsigned i = 0; i < ops.count; i++)
            { path(p, candidate, files[i].name); CHECK(access(p, F_OK) != 0 && errno == ENOENT); }
        }
        else { CHECK(!*candidate); fresh_faults++; }
        CHECK(get(gen2, "unrelated.keep", check, sizeof(check)) == 4);
        CHECK(!memcmp(check, "keep", 4));
    }
    for (cancel_after = 0; cancel_after < 4;)
    {
        int boundary = cancel_after;
        CHECK(dbgen_prepare_empty(root, root, candidate, work, sizeof(work), &ops) == DBGEN_CANCELLED);
        CHECK(!*candidate);
        CHECK(fingerprint(gen0) == retained[0] && fingerprint(gen1) == retained[1]);
        cancel_after = boundary + 1;
    }
    cancel_after = -1;
    /* All three normal directories taken: only the recovery directory. */
    CHECK(dbgen_prepare_empty(root, gen2, candidate, work, sizeof(work), &ops) == DBGEN_OK);
    CHECK(!strncmp(candidate, root, strlen(root))
          && !strcmp(candidate + strlen(root), "/database_gen3"));

    /* A torn reference cannot establish safe ownership: no directory with
     * content is recycled. With every normal directory occupied, the fresh
     * build uses only the dedicated recovery directory. */
    fill(gen2, 88);
    uint32_t scratch_before = fingerprint(gen2);
    put(root, "database_manifest0.dat", "torn", 4);
    char gen3[DBGEN_PATH_MAX]; path(gen3, root, "database_gen3");
    CHECK(dbgen_prepare_empty(root, root, candidate, work, sizeof(work), &ops) == DBGEN_OK);
    CHECK(!strcmp(candidate, gen3));
    CHECK(fingerprint(gen2) == scratch_before);
    CHECK(fingerprint(gen0) == retained[0] && fingerprint(gen1) == retained[1]);
    /* A directory with no declared files holds nothing to protect. */
    for (unsigned i = 0; i < ops.count; i++)
    { path(p, gen2, files[i].name); CHECK(unlink(p) == 0); }
    CHECK(dbgen_prepare_empty(root, root, candidate, work, sizeof(work), &ops) == DBGEN_OK);
    CHECK(!strcmp(candidate, gen2));
    /* Recovery publication replaces the torn slot, never the intact one. */
    CHECK(dbgen_prepare_empty(root, gen2, candidate, work, sizeof(work), &ops) == DBGEN_OK);
    CHECK(!strcmp(candidate, gen3));
    fill(gen3, 77);
    CHECK(dbgen_publish(root, gen3, 77, 3, work, sizeof(work), &ops) == DBGEN_OK);
    CHECK(get(root, "database_manifest1.dat", check, sizeof(check)) == lens[1]);
    CHECK(!memcmp(check, manifests[1], lens[1]));
    CHECK(dbgen_select(root, active, work, sizeof(work), &ops) == DBGEN_OK);
    CHECK(!strcmp(active, gen3));
    CHECK(fingerprint(gen0) == retained[0] && fingerprint(gen1) == retained[1]);
    put(root, "database_manifest0.dat", manifests[0], lens[0]);

    CHECK(dbgen_prepare_empty(root, root, candidate, work, sizeof(work), &ops) == DBGEN_OK);
    CHECK(!strcmp(candidate, gen2));
    CHECK(dbgen_select(root, active, work, sizeof(work), &ops) == DBGEN_CORRUPT);
    fill(candidate, 99);
    CHECK(dbgen_publish(root, candidate, 99, 3, work, sizeof(work), &ops) == DBGEN_OK);
    CHECK(dbgen_select(root, active, work, sizeof(work), &ops) == DBGEN_OK);
    CHECK(!strcmp(active, gen2));
    CHECK(fingerprint(gen0) == retained[0] && fingerprint(gen1) == retained[1]);

    /* An intact manifest of another version still reserves its directory:
     * neither prepare nor fresh recovery may recycle it. */
    fill(gen0, 1); fill(gen1, 2); fill(gen2, 3);
    put(root, "database_manifest0.dat", manifests[0], lens[0]);
    put(root, "database_manifest1.dat", manifests[1], lens[1]);
    CHECK(dbgen_select(root, active, work, sizeof(work), &ops) == DBGEN_OK);
    CHECK(!strcmp(active, gen1));
    reseal(root, "database_manifest1.dat", 4, 2);   /* version 2 */
    uint32_t foreign_dir = fingerprint(gen1);
    CHECK(dbgen_select(root, active, work, sizeof(work), &ops) == DBGEN_OK);
    CHECK(!strcmp(active, gen0));
    CHECK(dbgen_prepare(root, active, candidate, work, sizeof(work), &ops) == DBGEN_OK);
    CHECK(!strcmp(candidate, gen2));
    CHECK(dbgen_prepare_empty(root, active, candidate, work, sizeof(work), &ops) == DBGEN_OK);
    CHECK(!strcmp(candidate, gen2));
    CHECK(fingerprint(gen1) == foreign_dir);
    /* A foreign slot is replaced before the only valid generation. */
    fill(gen2, 3);
    CHECK(dbgen_publish(root, gen2, 3, 3, work, sizeof(work), &ops) == DBGEN_OK);
    CHECK(get(root, "database_manifest0.dat", check, sizeof(check)) == lens[0]);
    CHECK(!memcmp(check, manifests[0], lens[0]));
    CHECK(dbgen_select(root, active, work, sizeof(work), &ops) == DBGEN_OK);
    CHECK(!strcmp(active, gen2));

    /* Neither manifest usable (both foreign, then both torn): salvage the
     * newest structurally valid directory instead of an empty library.
     * Nothing is written by selection. */
    put(root, "database_manifest0.dat", manifests[0], lens[0]);
    put(root, "database_manifest1.dat", manifests[1], lens[1]);
    reseal(root, "database_manifest0.dat", 4, 2);
    reseal(root, "database_manifest1.dat", 4, 2);
    for (unsigned i = 0; i < ops.count; i++)
    { path(p, gen3, files[i].name); unlink(p); }
    uint32_t dirs[3] = { fingerprint(gen0), fingerprint(gen1), fingerprint(gen2) };
    CHECK(dbgen_select(root, active, work, sizeof(work), &ops) == DBGEN_SALVAGED);
    CHECK(!strcmp(active, gen2));          /* highest commit ID: 3 */
    CHECK(dbgen_prepare(root, active, candidate, work, sizeof(work), &ops) == DBGEN_OK);
    CHECK(!strcmp(candidate, gen3));       /* gen0/gen1 stay foreign-reserved */
    CHECK(fingerprint(gen0) == dirs[0] && fingerprint(gen1) == dirs[1]
          && fingerprint(gen2) == dirs[2]);
    {
        /* Of two foreign slots, the newer one is replaced. */
        unsigned char f[2][304]; uint64_t seq[2];
        for (int i = 0; i < 2; i++)
        {
            CHECK(get(root, i ? "database_manifest1.dat" : "database_manifest0.dat",
                      f[i], sizeof(f[i])) == sizeof(f[i]));
            memcpy(&seq[i], f[i] + 8, 8);
        }
        unsigned keep = seq[1] > seq[0] ? 0 : 1;
        fill(gen3, 4);
        CHECK(dbgen_publish(root, gen3, 4, 3, work, sizeof(work), &ops) == DBGEN_OK);
        CHECK(get(root, keep ? "database_manifest1.dat" : "database_manifest0.dat",
                  check, sizeof(check)) == sizeof(f[keep]));
        CHECK(!memcmp(check, f[keep], sizeof(f[keep])));
        CHECK(dbgen_select(root, active, work, sizeof(work), &ops) == DBGEN_OK);
        CHECK(!strcmp(active, gen3));
        put(root, keep ? "database_manifest0.dat" : "database_manifest1.dat",
            f[!keep], sizeof(f[!keep]));
    }
    put(root, "database_manifest0.dat", "torn", 4);
    put(root, "database_manifest1.dat", "torn-too", 8);
    put(gen2, files[1].name, "bad", 3);      /* not structurally checked */
    put(gen2, files[2].name, "bad", 3);      /* structurally invalid */
    for (unsigned i = 0; i < ops.count; i++)
    { path(p, gen3, files[i].name); unlink(p); }
    CHECK(dbgen_select(root, active, work, sizeof(work), &ops) == DBGEN_SALVAGED);
    CHECK(!strcmp(active, gen1));
    validation_fault = DBGEN_CORRUPT;
    CHECK(dbgen_select(root, active, work, sizeof(work), &ops) == DBGEN_CORRUPT);
    CHECK(!*active);
    validation_fault = 0;

    /* R-M1: a directory unreadable during salvage may hold the newest
     * library: defer (IO), never salvage past it. */
    snprintf(io_dir, sizeof(io_dir), "%s", gen0);
    CHECK(dbgen_select(root, active, work, sizeof(work), &ops) == DBGEN_IO);
    CHECK(!*active);
    io_dir[0] = 0;
    CHECK(dbgen_select(root, active, work, sizeof(work), &ops) == DBGEN_SALVAGED);
    CHECK(!strcmp(active, gen1));
    /* R-M1/R-M2: with no intact manifest every directory with content may
     * be referenced; prepare (like prepare_empty) takes only an empty one,
     * gen3 included (an ordinary generation once published), else defers. */
    fill(gen3, 5);
    uint32_t torn_dirs[4] = { fingerprint(gen0), fingerprint(gen1),
                              fingerprint(gen2), fingerprint(gen3) };
    /* M1: every directory occupied: never deferred for good. The oldest
     * is recycled: the damaged gen2 first, never the salvaged active. */
    CHECK(dbgen_prepare_empty(root, active, candidate, work, sizeof(work), &ops) == DBGEN_OK);
    CHECK(!strcmp(candidate, gen2));
    CHECK(fingerprint(gen0) == torn_dirs[0] && fingerprint(gen1) == torn_dirs[1]
          && fingerprint(gen3) == torn_dirs[3]);
    fill(gen2, 9);              /* valid now: the lowest commit ID goes */
    CHECK(dbgen_prepare(root, active, candidate, work, sizeof(work), &ops) == DBGEN_OK);
    CHECK(!strcmp(candidate, gen0));
    CHECK(fingerprint(gen1) == torn_dirs[1] && fingerprint(gen3) == torn_dirs[3]);
    fill(gen0, 1);
    fill(gen2, 3); put(gen2, files[1].name, "bad", 3); put(gen2, files[2].name, "bad", 3);
    torn_dirs[0] = fingerprint(gen0); torn_dirs[2] = fingerprint(gen2);
    for (unsigned i = 0; i < ops.count; i++)
    { path(p, gen2, files[i].name); unlink(p); }
    CHECK(dbgen_prepare(root, active, candidate, work, sizeof(work), &ops) == DBGEN_OK);
    CHECK(!strcmp(candidate, gen2));
    CHECK(fingerprint(gen0) == torn_dirs[0] && fingerprint(gen3) == torn_dirs[3]);
    /* Foreign manifests are intact: they reserve their own directories and
     * are not torn, so a directory with content no manifest names is still
     * scratch (no deferral loop when every directory has content). */
    fill(gen0, 1); fill(gen1, 2); fill(gen2, 3); fill(gen3, 5);
    put(root, "database_manifest0.dat", manifests[0], lens[0]);
    put(root, "database_manifest1.dat", manifests[1], lens[1]);
    reseal(root, "database_manifest0.dat", 4, 2);
    reseal(root, "database_manifest1.dat", 4, 2);
    CHECK(dbgen_select(root, active, work, sizeof(work), &ops) == DBGEN_SALVAGED);
    CHECK(dbgen_prepare(root, active, candidate, work, sizeof(work), &ops) == DBGEN_OK);
    CHECK(strcmp(candidate, active) && strcmp(candidate, gen0)
          && strcmp(candidate, gen1));

    /* M-2: commit IDs stay monotonic (a fresh build starts above the
     * selected library), so "lowest commit" is "oldest"; the torn recycle
     * with nothing active takes the oldest, never the newest valid one. */
    fill(gen0, 50); fill(gen1, 60); fill(gen2, 55); fill(gen3, 51);
    put(root, "database_manifest0.dat", "torn", 4);
    put(root, "database_manifest1.dat", "torn-too", 8);
    uint32_t newest = fingerprint(gen1);
    CHECK(dbgen_prepare_empty(root, root, candidate, work, sizeof(work), &ops) == DBGEN_OK);
    CHECK(!strcmp(candidate, gen0) && fingerprint(gen1) == newest);
    fill(gen0, 52);
    CHECK(dbgen_prepare_empty(root, root, candidate, work, sizeof(work), &ops) == DBGEN_OK);
    CHECK(!strcmp(candidate, gen3) && fingerprint(gen1) == newest);

    printf("ok dbgen: %u checks; %u failing publication positions; %u fresh-prepare failures; retained generation recovery\n",
           checks, injected, fresh_faults);
    return 0;
}
