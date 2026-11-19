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
 * The library's version stamp (apps/ipodui/libstamp.h) and the manifest
 * check that uses it (apps/ipodui/dbgen.c, DBGEN_STALE): host test.
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
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "dbgen.h"
#include "libstamp.h"

static unsigned checks;
#define CHECK(c) do { checks++; if (!(c)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

/* -- the manifest's layout (dbgen.c, struct manifest; pinned here) ------- */
#define M_SIZE      304
#define M_VERSION   4
#define M_LIB       264
#define M_SUM       296

struct master { int32_t commitid, entries; uint32_t structural, stats; };
static unsigned char work[16384];
static const struct dbgen_file files[] = {
    { "database_idx.tcd", false, true },
    { "database_0.tcd", true, true },
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
    int fd = open(p, O_RDONLY);
    if (fd < 0) return 0;
    ssize_t got = read(fd, data, n); assert(got >= 0 && close(fd) == 0);
    return (size_t)got;
}
static void fill(const char *dir, int id)
{
    struct master m = { id, 3, 0x12340000u + id, 100 };
    unsigned char strings[4096];
    put(dir, files[0].name, &m, sizeof(m));
    memset(strings, id, sizeof(strings));
    put(dir, files[1].name, strings, sizeof(strings));
}
static enum dbgen_result validate(const char *dir, int32_t id, int32_t entries,
                                  uint32_t *sum, int32_t *found, void *opaque)
{
    struct master m;
    (void)opaque;
    if (get(dir, files[0].name, &m, sizeof(m)) != sizeof(m))
        return DBGEN_CORRUPT;
    if ((id >= 0 && id != m.commitid) || (entries >= 0 && entries != m.entries))
        return DBGEN_CORRUPT;
    *sum = m.structural;
    *found = m.commitid;
    return DBGEN_OK;
}
static bool cancel(void *opaque) { (void)opaque; return false; }

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
/* bytes into a manifest at offset, its CRC sealed again (intact) */
static void patch(const char *root, const char *name, size_t off,
                  const void *bytes, size_t n)
{
    unsigned char m[M_SIZE];
    assert(get(root, name, m, sizeof(m)) == sizeof(m));
    memcpy(m + off, bytes, n);
    memset(m + M_SUM, 0, 4);
    uint32_t sum = crc32_iso(m, sizeof(m));
    memcpy(m + M_SUM, &sum, 4);
    put(root, name, m, sizeof(m));
}
static void manifest_stamp(const char *root, const char *name,
                           struct libstamp *s)
{
    unsigned char m[M_SIZE];
    assert(get(root, name, m, sizeof(m)) == sizeof(m));
    memcpy(s, m + M_LIB, sizeof(*s));
}

int main(int argc, char **argv)
{
    struct libstamp ours, s;
    char buf[64];

    CHECK(argc == 2);
    /* -- the numbers, pinned (a bump updates them here, same change) -- */
    CHECK(sizeof(struct libstamp) == 32);
    CHECK(LIBSTAMP_DB_MAGIC == 0x544348a3);
    CHECK(SORTKEY_VERSION == 3);
    CHECK(LIBSTAMP_SCHEMA == 1);
    CHECK(DBGEN_MAX_FILES == 28);

    libstamp_ours(&ours, "0.9.0-test+g0123456789abcdef-dirty");
    CHECK(ours.db_magic == 0x544348a3 && ours.sortkey == 3 && ours.schema == 1);
    CHECK(ours.fw[LIBSTAMP_FW_LEN - 1] == '\0');            /* truncated */
    CHECK(!strncmp(ours.fw, "0.9.0-test+g0123456789ab", LIBSTAMP_FW_LEN - 1));

    /* -- the check: equal, each field, missing, corrupt -- */
    s = ours;
    CHECK(libstamp_check(&s, &ours) == LIBSTAMP_OK);
    strcpy(s.fw, "0.1.0");                  /* logs only: never compared */
    CHECK(libstamp_check(&s, &ours) == LIBSTAMP_OK);
    s = ours; s.db_magic = 0x544348a2;      /* an older format */
    CHECK(libstamp_check(&s, &ours) == LIBSTAMP_DB_FORMAT);
    s = ours; s.db_magic = 0x544348a4;      /* a newer one (a downgrade) */
    CHECK(libstamp_check(&s, &ours) == LIBSTAMP_DB_FORMAT);
    s = ours; s.sortkey = 2;
    CHECK(libstamp_check(&s, &ours) == LIBSTAMP_SORTKEY);
    s = ours; s.sortkey = 4;
    CHECK(libstamp_check(&s, &ours) == LIBSTAMP_SORTKEY);
    s = ours; s.schema = 2;
    CHECK(libstamp_check(&s, &ours) == LIBSTAMP_SCHEMA_DIFF);
    memset(&s, 0, sizeof(s));
    CHECK(libstamp_check(&s, &ours) == LIBSTAMP_MISSING);
    CHECK(libstamp_check(NULL, &ours) == LIBSTAMP_MISSING);
    s = ours; s.db_magic = 0xdeadbeef;
    CHECK(libstamp_check(&s, &ours) == LIBSTAMP_CORRUPT);
    s = ours; s.schema = 0;
    CHECK(libstamp_check(&s, &ours) == LIBSTAMP_CORRUPT);
    s = ours; s.sortkey = 0;
    CHECK(libstamp_check(&s, &ours) == LIBSTAMP_CORRUPT);
    s = ours; memset(s.fw, 'A', sizeof(s.fw));  /* no terminator */
    CHECK(libstamp_check(&s, &ours) == LIBSTAMP_CORRUPT);
    CHECK(!strcmp(libstamp_why_name(LIBSTAMP_OK), "none"));
    CHECK(!strcmp(libstamp_why_name(LIBSTAMP_MISSING), "missing"));
    CHECK(!strcmp(libstamp_why_name(LIBSTAMP_CORRUPT), "corrupt"));
    CHECK(!strcmp(libstamp_why_name(LIBSTAMP_DB_FORMAT), "format"));
    CHECK(!strcmp(libstamp_why_name(LIBSTAMP_SORTKEY), "sortkey"));
    CHECK(!strcmp(libstamp_why_name(LIBSTAMP_SCHEMA_DIFF), "schema"));
    CHECK(!strcmp(libstamp_format(&ours, buf, sizeof(buf)), "a3/k3/s1"));
    CHECK(!strcmp(libstamp_fw(&ours, buf, sizeof(buf)),
                  "0.9.0-test+g0123456789a"));
    s = ours; s.db_magic = 0xdeadbeef;
    CHECK(!strcmp(libstamp_format(&s, buf, sizeof(buf)), "xdeadbeef"));
    memset(&s, 0, sizeof(s));
    CHECK(!strcmp(libstamp_format(&s, buf, sizeof(buf)), "none"));
    CHECK(!strcmp(libstamp_fw(&s, buf, sizeof(buf)), "-"));
    /* the longest owed line fits a log line (64 bytes with "db ") */
    s = ours; s.db_magic = 0x544348ff; s.sortkey = 65535; s.schema = 65535;
    {
        char a[24], b[24], line[128];
        snprintf(line, sizeof(line), "db upgrade owed from=%s to=%s why=%s",
                 libstamp_format(&s, a, sizeof(a)),
                 libstamp_format(&ours, b, sizeof(b)),
                 libstamp_why_name(LIBSTAMP_SCHEMA_DIFF));
        CHECK(strlen(line) < 64);
    }
    /* any bytes: a verdict, never OK unless equal, never past the buffer */
    srand(1);
    for (int n = 0; n < 200000; n++)
    {
        unsigned char *b = (unsigned char *)&s;
        for (size_t i = 0; i < sizeof(s); i++) b[i] = (unsigned char)rand();
        if (n & 1) s.db_magic = LIBSTAMP_DB_FAMILY | (s.db_magic & 0xff);
        enum libstamp_why w = libstamp_check(&s, &ours);
        CHECK(w >= LIBSTAMP_OK && w <= LIBSTAMP_SCHEMA_DIFF);
        CHECK(w != LIBSTAMP_OK || (s.db_magic == ours.db_magic
              && s.sortkey == ours.sortkey && s.schema == ours.schema));
        libstamp_format(&s, buf, sizeof(buf));
        CHECK(strlen(buf) < sizeof(buf));
        libstamp_fw(&s, buf, sizeof(buf));
        CHECK(strlen(buf) <= LIBSTAMP_FW_LEN);
    }

    /* -- the manifest: written, checked before any content is read -- */
    char root[DBGEN_PATH_MAX], out[DBGEN_PATH_MAX], cand[DBGEN_PATH_MAX];
    char gen0[DBGEN_PATH_MAX];
    struct libstamp found, other;
    struct dbgen_ops ops = { files, 2, validate, cancel, NULL, NULL, NULL,
                             NULL, NULL, &ours, &found };
    const char *m0 = "database_manifest0.dat";
    path(root, argv[1], "root"); CHECK(mkdir(root, 0777) == 0);
    path(gen0, root, "database_gen0");
    CHECK(dbgen_select(root, out, work, sizeof(work), &ops) == DBGEN_LEGACY);
    CHECK(dbgen_prepare(root, root, cand, work, sizeof(work), &ops) == DBGEN_OK);
    CHECK(!strcmp(cand, gen0));
    fill(cand, 1);
    CHECK(dbgen_publish(root, cand, 1, 3, work, sizeof(work), &ops) == DBGEN_OK);
    manifest_stamp(root, m0, &s);
    CHECK(!memcmp(&s, &ours, sizeof(s)));       /* the stamp is written */
    CHECK(dbgen_select(root, out, work, sizeof(work), &ops) == DBGEN_OK);
    CHECK(!strcmp(out, gen0));
    CHECK(dbgen_select_quick(root, out, work, sizeof(work), &ops) == DBGEN_OK);

    /* each field this firmware differs in: STALE, the directory named, the
     * stamp found reported; quick refuses it too */
    static const struct { int field; int value; enum libstamp_why why; } d[] = {
        { 0, 0x544348a4, LIBSTAMP_DB_FORMAT },
        { 1, 4, LIBSTAMP_SORTKEY },
        { 2, 2, LIBSTAMP_SCHEMA_DIFF },
    };
    for (unsigned i = 0; i < sizeof(d) / sizeof(d[0]); i++)
    {
        other = ours;
        if (d[i].field == 0) other.db_magic = (uint32_t)d[i].value;
        if (d[i].field == 1) other.sortkey = (uint16_t)d[i].value;
        if (d[i].field == 2) other.schema = (uint16_t)d[i].value;
        ops.stamp = &other;
        memset(&found, 0, sizeof(found));
        CHECK(dbgen_select(root, out, work, sizeof(work), &ops) == DBGEN_STALE);
        CHECK(!strcmp(out, gen0));
        CHECK(!memcmp(&found, &ours, sizeof(found)));
        CHECK(libstamp_check(&found, &other) == d[i].why);
        CHECK(dbgen_select_quick(root, out, work, sizeof(work), &ops) == DBGEN_STALE);
        CHECK(out[0] == '\0');
    }
    ops.stamp = &ours;

    /* a version-1 manifest (before the stamp): read as one with none */
    {
        uint32_t v1 = 1;
        unsigned char zero[32] = { 0 };
        patch(root, m0, M_VERSION, &v1, 4);
        patch(root, m0, M_LIB, zero, sizeof(zero));
        memset(&found, 0xee, sizeof(found));
        CHECK(dbgen_select(root, out, work, sizeof(work), &ops) == DBGEN_STALE);
        CHECK(!strcmp(out, gen0));
        CHECK(libstamp_check(&found, &ours) == LIBSTAMP_MISSING);
        /* the old firmware's view (no stamp checked): selected as before */
        ops.stamp = NULL;
        CHECK(dbgen_select(root, out, work, sizeof(work), &ops) == DBGEN_OK);
        CHECK(!strcmp(out, gen0));
        ops.stamp = &ours;
        /* a version-1 manifest with bytes where the stamp would be: still
         * none (they are not a stamp) */
        unsigned char junk[32];
        memset(junk, 0x5a, sizeof(junk));
        patch(root, m0, M_LIB, junk, sizeof(junk));
        CHECK(dbgen_select(root, out, work, sizeof(work), &ops) == DBGEN_STALE);
        CHECK(libstamp_check(&found, &ours) == LIBSTAMP_MISSING);
        uint32_t v2 = 2;
        patch(root, m0, M_VERSION, &v2, 4);
    }
    /* a corrupt stamp in an intact version-2 manifest: STALE, corrupt */
    {
        uint32_t bad = 0xdeadbeef;
        patch(root, m0, M_LIB, &bad, 4);
        CHECK(dbgen_select(root, out, work, sizeof(work), &ops) == DBGEN_STALE);
        CHECK(libstamp_check(&found, &ours) == LIBSTAMP_CORRUPT);
        /* never salvaged past, and its directory stays reserved: a fresh
         * candidate goes elsewhere and gen0 keeps every byte */
        unsigned char before[4096], after[4096];
        size_t nb = get(gen0, files[1].name, before, sizeof(before));
        CHECK(dbgen_prepare_empty(root, root, cand, work, sizeof(work), &ops)
              == DBGEN_OK);
        CHECK(strcmp(cand, gen0) != 0);
        CHECK(get(gen0, files[1].name, after, sizeof(after)) == nb
              && !memcmp(before, after, nb));
        /* the rebuild publishes this firmware's stamp: selected */
        fill(cand, 2);
        CHECK(dbgen_publish(root, cand, 2, 3, work, sizeof(work), &ops) == DBGEN_OK);
        CHECK(dbgen_select(root, out, work, sizeof(work), &ops) == DBGEN_OK);
        CHECK(!strcmp(out, cand));
    }
    printf("ok libstamp: %u checks\n", checks);
    return 0;
}
