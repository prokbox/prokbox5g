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
 * Host test: the no-change boot stamp (Phase 16-2, #89)
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
#include <string.h>
#include "ipodui/bootstamp.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
    printf("\n"); } } while (0)

static struct boot_stamp good(void)
{
    struct boot_stamp s;

    memset(&s, 0, sizeof(s));
    s.fsinfo_free = 123456;
    s.last_walk_rtc = 846000000u;     /* 2026-10 */
    s.dir_digest = 0xdeadbeef;
    s.db_generation = 42;
    s.boots_skipped = 3;
    s.flags = BOOT_STAMP_CLEAN;
    boot_stamp_seal(&s);
    return s;
}

static void test_crc(void)
{
    /* the standard CRC-32 check value */
    CHECK(boot_crc32(0, "123456789", 9) == 0xcbf43926u, "crc %08x",
          boot_crc32(0, "123456789", 9));
    /* chained equals one pass */
    CHECK(boot_crc32(boot_crc32(0, "1234", 4), "56789", 5) == 0xcbf43926u,
          "chained");
}

static void test_roundtrip_torn(void)
{
    struct boot_stamp s = good(), out;
    unsigned char buf[sizeof(s) + 1];
    size_t i;
    int b;

    memcpy(buf, &s, sizeof(s));
    CHECK(boot_stamp_check(buf, sizeof(s), &out), "round trip");
    CHECK(!memcmp(&s, &out, sizeof(s)), "round trip bytes");
    /* torn writes: every short length, and a longer file */
    for (i = 0; i <= sizeof(s) + 1; i++)
        if (i != sizeof(s))
            CHECK(!boot_stamp_check(buf, i, NULL), "length %zu", i);
    CHECK(!boot_stamp_check(NULL, sizeof(s), NULL), "null");
    /* a torn sector: zeros from any byte on; every single bit flip */
    for (i = 0; i < sizeof(s); i++)
    {
        unsigned char t[sizeof(s)];
        memcpy(t, &s, sizeof(s));
        memset(t + i, 0, sizeof(s) - i);
        CHECK(!boot_stamp_check(t, sizeof(s), NULL), "zeroed from %zu", i);
        for (b = 0; b < 8; b++)
        {
            memcpy(t, &s, sizeof(s));
            t[i] ^= 1u << b;
            CHECK(!boot_stamp_check(t, sizeof(s), NULL), "flip %zu.%d", i, b);
        }
    }
    /* another version or magic, even with a matching CRC */
    out = s;
    out.version = BOOT_STAMP_VERSION + 1;
    out.crc = boot_crc32(0, &out, offsetof(struct boot_stamp, crc));
    CHECK(!boot_stamp_check(&out, sizeof(out), NULL), "version");
    out = s;
    out.magic ^= 1;
    out.crc = boot_crc32(0, &out, offsetof(struct boot_stamp, crc));
    CHECK(!boot_stamp_check(&out, sizeof(out), NULL), "magic");
    /* P18: a v1 stamp (fsinfo_next where last_walk_rtc now is, the same
     * 32 B and the same CRC rule) is refused, so its FAT cluster number
     * is never read as a walk time: the first boot after the update
     * walks once ("torn") and writes a v2 stamp */
    out = s;
    out.version = 1;
    out.last_walk_rtc = 0x2000;
    out.crc = boot_crc32(0, &out, offsetof(struct boot_stamp, crc));
    CHECK(!boot_stamp_check(&out, sizeof(out), NULL), "v1 refused");
    CHECK(offsetof(struct boot_stamp, last_walk_rtc) == 12, "v2 layout");
    CHECK(!strcmp(boot_why_name(BOOT_WHY_DAYS), "days"), "days name");
}

static void test_decision(void)
{
    struct boot_stamp s = good();
    unsigned k;

    CHECK(boot_stamp_precheck(NULL, false, 42) == BOOT_WHY_NOSTAMP, "none");
    CHECK(boot_stamp_precheck(&s, false, 42) == BOOT_WHY_NOSTAMP, "have=0");
    CHECK(boot_stamp_precheck(&s, true, 42) == BOOT_WHY_TRUST, "trust");
    CHECK(boot_stamp_precheck(&s, true, 43) == BOOT_WHY_GEN, "generation");
    s.flags = 0;
    CHECK(boot_stamp_precheck(&s, true, 42) == BOOT_WHY_UNCLEAN, "unclean");
    s = good();
    /* walk, 9 skips, walk: skipped 0..8 trust, 9 and up walk */
    for (k = 0; k < 20; k++)
    {
        s.boots_skipped = k;
        enum boot_why w = boot_stamp_precheck(&s, true, 42);
        CHECK(k + 1 < BOOT_STAMP_WALK_EVERY ? w == BOOT_WHY_TRUST
                                            : w == BOOT_WHY_TENTH,
              "skipped %u: %s", k, boot_why_name(w));
    }
    s.boots_skipped = 0xffff;
    CHECK(boot_stamp_precheck(&s, true, 42) == BOOT_WHY_TENTH, "saturated");
    /* the digest's verdicts */
    CHECK(boot_stamp_verdict(BOOT_DIGEST_MATCH) == BOOT_WHY_TRUST, "match");
    CHECK(boot_stamp_verdict(BOOT_DIGEST_SLOW) == BOOT_WHY_SLOW, "slow");
    CHECK(boot_stamp_verdict(BOOT_DIGEST_DIFF) == BOOT_WHY_DIGEST, "diff");
    CHECK(boot_stamp_verdict(BOOT_DIGEST_FAIL) == BOOT_WHY_DIGEST_FAIL,
          "fail");
    CHECK(boot_why_trusted(BOOT_WHY_TRUST), "trusted");
    CHECK(!boot_why_trusted(BOOT_WHY_SLOW), "slow walks (H1)");
    CHECK(!boot_why_deleted_pass(BOOT_WHY_TRUST), "trust: no pass");
    CHECK(!boot_why_deleted_pass(BOOT_WHY_DB), "db: its own pass");
    for (k = BOOT_WHY_SLOW; k < BOOT_WHY__N; k++)
    {
        CHECK(!boot_why_trusted((enum boot_why)k), "walks %u", k);
        if (k != BOOT_WHY_DB)
            CHECK(boot_why_deleted_pass((enum boot_why)k), "pass %u", k);
        CHECK(strcmp(boot_why_name((enum boot_why)k), "?"), "name %u", k);
    }
}

/* a synthetic tree, listed depth first: depth, name, dir, size, mtime,
 * cluster; depth changes end folders (a leave marker each) */
struct ent { int depth; const char *name; int dir; uint32_t size, mtime, cl; };

static const struct ent base[] = {
    { 0, "Music", 1, 0, 100, 3 },
    { 1, "Artist", 1, 0, 101, 4 },
    { 2, "01 One.mp3", 0, 4000000, 200, 10 },
    { 2, "02 Two.mp3", 0, 5000000, 201, 20 },
    { 1, "Other", 1, 0, 102, 5 },
    { 2, "03 Three.flac", 0, 9000000, 202, 30 },
    { 0, "Playlists", 1, 0, 103, 6 },
    { 0, "", 0, 0, 0, 0 },                  /* end */
};

static uint32_t digest(const struct ent *t, long *n)
{
    struct boot_digest d;
    int depth = 0;

    boot_digest_begin(&d);
    for (; t->name[0]; t++)
    {
        while (depth > t->depth)
        {
            boot_digest_leave(&d);
            depth--;
        }
        boot_digest_entry(&d, t->name, t->dir, t->size, t->mtime, t->cl);
        depth = t->dir ? t->depth + 1 : t->depth;
    }
    while (depth-- >= 0)
        boot_digest_leave(&d);
    if (n)
        *n = d.entries;
    return d.crc;
}

static void test_digest(void)
{
    struct ent t[9];
    long n;
    const uint32_t d0 = digest(base, &n);

    CHECK(n == 7, "entries %ld", n);
    CHECK(digest(base, NULL) == d0, "stable");
#define VARY(what, stmt) do { memcpy(t, base, sizeof(base)); t[8] = base[7]; stmt; \
        CHECK(digest(t, NULL) != d0, "%s", what); } while (0)
    VARY("renamed", t[2].name = "01 Uno.mp3");
    VARY("resized", t[3].size++);
    VARY("retimed", t[5].mtime++);
    VARY("cluster", t[2].cl = 99);
    VARY("dir bit", t[6].dir = 0);
    VARY("added", (memmove(&t[4], &t[3], 4 * sizeof(t[0])),
                   t[3] = (struct ent){ 2, "02a New.mp3", 0, 1, 300, 40 }));
    VARY("removed", memmove(&t[3], &t[4], 4 * sizeof(t[0])));
    /* moved to the other folder: same entries, other structure */
    VARY("moved", (t[3] = base[4], t[4] = base[5],
                   t[5] = (struct ent){ 2, "02 Two.mp3", 0, 5000000, 201, 20 }));
    /* a name split differently never collides */
    {
        struct boot_digest a, b;
        boot_digest_begin(&a);
        boot_digest_begin(&b);
        boot_digest_entry(&a, "ab", 0, 1, 1, 1);
        boot_digest_entry(&b, "a", 0, 1, 1, 1);
        CHECK(a.crc != b.crc, "split");
        boot_digest_leave(&a);
        boot_digest_begin(&a);
        boot_digest_leave(&a);
        CHECK(a.crc != b.crc && a.dirs == 1, "leave");
    }
    /* a directory's size is ignored (FAT keeps 0; a host may not) */
    memcpy(t, base, sizeof(base));
    t[1].size = 4096;
    CHECK(digest(t, NULL) == d0, "dir size ignored");
}

int main(void)
{
    test_crc();
    test_roundtrip_torn();
    test_decision();
    test_digest();
    if (fails)
        printf("%d failure(s)\n", fails);
    return fails != 0;
}
