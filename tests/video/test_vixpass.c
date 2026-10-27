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
 * Host test: apps/ipodui/vixpass_core.c, the video index pass's decisions
 * (Phase 14a V3, docs/phase14a-brief.md 3.4). The ledger's identity,
 * header, sum, sort and chunked merge; the copies' records (end review
 * M2a); the take-over of an index whose file kept its name; the .vix
 * names; and a model of vixpass_run()'s pass over a fake disk: fresh,
 * steady, renamed and deleted videos (no orphan left: M2b), a rebuilt
 * database, a pass that waits half way, and two records of one index.
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
#include "vixpass_core.h"

static int fails;

#define CHECK(name, cond) do { \
    if (!(cond)) { printf("FAIL %s: %s (line %d)\n", name, #cond, __LINE__); \
                   fails++; } } while (0)

/* ---- the fake disk: the .vix names present -------------------------- */
#define MAXN 64
static uint64_t disk[MAXN];
static int ndisk;

static void disk_add(uint64_t n)
{
    if (!vpc_has_name64(disk, ndisk, n) && ndisk < MAXN)
        disk[ndisk++] = n;
}

static void disk_del(uint64_t n)
{
    int i;

    for (i = 0; i < ndisk; i++)
        if (disk[i] == n)
        {
            disk[i] = disk[--ndisk];
            return;
        }
}

/* ---- a model of vixpass_run() (not torn) ---------------------------- */
struct vid {
    uint32_t id, mtime;
    uint64_t file;          /* the name vix_name() gives it; 0: can't open */
};

struct res {
    int kept, reused, ondisk, built, removed, written, waited;
};

static struct vixpass_ent led[MAXN];
static int nled;

/* 'resolves' < 0: no wait; else the pass waits before resolve #resolves */
static struct res pass(const struct vid *v, int nv, int resolves)
{
    struct vixpass_ent old[MAXN], cur[MAXN], buf[2];
    int32_t pend[MAXN];
    uint8_t used[MAXN / 8] = { 0 };
    struct res r = { 0, 0, 0, 0, 0, 0, 0 };
    int i, nold = nled, ncur = 0, nkeep = 0, done = 0;
    bool dirty = false;

    memcpy(old, led, sizeof(old));
    for (i = 0; i < nv; i++)
    {
        int k = vpc_match(old, nold, used, v[i].id, v[i].mtime);

        cur[ncur].id = v[i].id;
        cur[ncur].mtime = v[i].mtime;
        cur[ncur].name = k >= 0 ? old[k].name : 0;
        pend[ncur] = k >= 0 ? -1 : i;
        r.kept += k >= 0;
        ncur++;
    }
    for (i = 0; !r.waited && i < ncur; i++)
    {
        const struct vid *e;

        if (pend[i] < 0)
            continue;
        if (resolves >= 0 && done == resolves)
        {
            r.waited = 1;
            break;
        }
        dirty = true;
        e = &v[pend[i]];
        pend[i] = -1;
        done++;
        cur[i].name = e->file;
        if (!e->file)
            continue;
        if (vpc_claim_name(old, nold, used, e->file) >= 0)
            r.reused++;
        else if (vpc_has_name64(disk, ndisk, e->file))
            r.ondisk++;
        else
        {
            disk_add(e->file);
            r.built++;
        }
    }
    for (i = 0; !r.waited && i < nold; i++)
        if (!VPC_USED(used, i) && old[i].name)
        {
            if (!vpc_has_name(cur, ncur, old[i].name))
            {
                disk_del(old[i].name);
                r.removed++;
            }
            dirty = true;
        }
    if (r.waited && dirty)
        nkeep = vpc_keep_unused(old, used, nold);
    if (dirty)
    {
        int n, ia = 0, ib = 0, c;

        vpc_sort(cur, pend, ncur);
        n = vpc_keep_resolved(cur, pend, ncur);
        nled = 0;
        while ((c = vpc_merge(cur, n, &ia, old, nkeep, &ib, buf, 2)) > 0)
        {
            memcpy(&led[nled], buf, c * sizeof(buf[0]));
            nled += c;
        }
        r.written = 1;
    }
    return r;
}

static bool led_sorted(void)
{
    int i;

    for (i = 1; i < nled; i++)
        if (vpc_cmp(&led[i - 1], led[i].id, led[i].mtime) > 0)
            return false;
    return true;
}

/* the disk holds exactly the live videos' indexes */
static bool no_orphans(const struct vid *v, int nv)
{
    int i, live = 0;

    for (i = 0; i < nv; i++)
        if (v[i].file)
        {
            if (!vpc_has_name64(disk, ndisk, v[i].file))
                return false;
            live++;
        }
    for (i = 0; i < nv; i++)        /* copies of one file count once */
    {
        int j;

        for (j = 0; j < i; j++)
            if (v[j].file && v[j].file == v[i].file)
            {
                live--;
                break;
            }
    }
    return ndisk == live;
}

#define ID(t, seek) vpc_id(t, "Album", 60000, seek)

static void scenarios(void)
{
    struct vid v[8];
    struct res r;
    int f0 = fails;

    nled = 0;
    ndisk = 0;
    v[0] = (struct vid){ ID("A", 100), 11, 0xa1 };
    v[1] = (struct vid){ ID("B", 200), 22, 0xb1 };
    v[2] = (struct vid){ ID("C", 300), 33, 0xc1 };

    r = pass(v, 3, -1);
    CHECK("fresh", r.built == 3 && r.removed == 0 && r.written && nled == 3);
    CHECK("fresh", led_sorted() && no_orphans(v, 3));

    r = pass(v, 3, -1);
    CHECK("steady", r.kept == 3 && r.built == 0 && !r.written);

    /* B renamed: a new entry (new filename seek), a new index name */
    v[1] = (struct vid){ ID("B", 400), 22, 0xb2 };
    r = pass(v, 3, -1);
    CHECK("rename", r.built == 1 && r.removed == 1 && nled == 3);
    CHECK("rename", !vpc_has_name64(disk, ndisk, 0xb1) && no_orphans(v, 3));

    /* C deleted */
    r = pass(v, 2, -1);
    CHECK("delete", r.removed == 1 && r.built == 0 && nled == 2);
    CHECK("delete", no_orphans(v, 2));

    /* the database rebuilt: new seeks, same files: taken over */
    v[0].id = ID("A", 7);
    v[1].id = ID("B", 8);
    r = pass(v, 2, -1);
    CHECK("rebuilt db", r.reused == 2 && r.built == 0 && r.removed == 0);
    CHECK("rebuilt db", nled == 2 && no_orphans(v, 2));
    r = pass(v, 2, -1);
    CHECK("rebuilt db steady", r.kept == 2 && !r.written);

    /* two renames, the pass waits after the first: nothing removed, the
     * old records kept in the ledger; the next pass removes both */
    v[0] = (struct vid){ ID("A", 9), 11, 0xa2 };
    v[1] = (struct vid){ ID("B", 10), 22, 0xb3 };
    r = pass(v, 2, 0);
    CHECK("wait at once", r.waited && !r.written && r.removed == 0);
    r = pass(v, 2, 1);
    CHECK("wait half", r.waited && r.written && r.built == 1);
    CHECK("wait half", r.removed == 0 && nled == 3 && led_sorted());
    CHECK("wait half", vpc_has_name(led, nled, 0xa1)
                       && vpc_has_name(led, nled, 0xb2));
    r = pass(v, 2, -1);
    CHECK("after wait", r.built == 1 && r.removed == 2 && nled == 2);
    CHECK("after wait", no_orphans(v, 2));

    /* a video that cannot be opened: recorded (name 0), not retried */
    v[2] = (struct vid){ ID("D", 11), 44, 0 };
    r = pass(v, 3, -1);
    CHECK("unopenable", r.built == 0 && nled == 3 && r.written);
    r = pass(v, 3, -1);
    CHECK("unopenable steady", r.kept == 3 && !r.written);

    /* copies of one identity (same tags, seek and mtime: duplicate
     * database entries of one file) share nothing: one record each, so a
     * steady pass neither rebuilds nor removes (M2a) */
    v[3] = v[0];
    r = pass(v, 4, -1);
    CHECK("copies", r.ondisk == 1 && r.built == 0 && r.removed == 0);
    CHECK("copies", nled == 4);
    r = pass(v, 4, -1);
    CHECK("copies steady", r.kept == 4 && !r.written && r.removed == 0);
    /* one copy goes: its record is unused, but a live record names the
     * same index, which stays */
    r = pass(v, 3, -1);
    CHECK("copy gone", r.removed == 0 && r.written && nled == 3);
    CHECK("copy gone", vpc_has_name64(disk, ndisk, 0xa2) && no_orphans(v, 3));

    if (fails == f0)
        printf("ok   pass model (fresh, steady, rename, delete, rebuilt db, "
               "wait, unopenable, copies)\n");
}

static void units(void)
{
    struct vixpass_ent e[6], out[6], buf[2];
    int32_t pend[6] = { 5, -1, 3, -1, -1, 0 };
    uint8_t used[1] = { 0 };
    struct vpc_hdr h = { VPC_MAGIC, VPC_VERSION, 4, 0 };
    uint64_t n;
    int f0 = fails, ia, ib, c, m, i;

    /* identity: the filename seek and every tag count; NULL == "" */
    CHECK("id", vpc_id("T", "A", 1, 5) == vpc_id("T", "A", 1, 5));
    CHECK("id", vpc_id("T", "A", 1, 5) != vpc_id("T", "A", 1, 6));
    CHECK("id", vpc_id("T", "A", 1, 5) != vpc_id("T", "A", 2, 5));
    CHECK("id", vpc_id("TA", "", 1, 5) != vpc_id("T", "A", 1, 5));
    CHECK("id", vpc_id(NULL, NULL, 1, 5) == vpc_id("", "", 1, 5));

    /* header */
    CHECK("hdr", vpc_hdr_ok(&h, 4) && !vpc_hdr_ok(&h, 3));
    h.version = 1;
    CHECK("hdr old version", !vpc_hdr_ok(&h, 4));
    h.version = VPC_VERSION;
    h.magic ^= 1;
    CHECK("hdr magic", !vpc_hdr_ok(&h, 4));

    /* sort keeps pend alongside; keep_resolved keeps order */
    for (i = 0; i < 6; i++)
        e[i] = (struct vixpass_ent){ (uint32_t)(60 - i * 10) % 35,
                                     (uint32_t)i, (uint64_t)(100 + i) };
    vpc_sort(e, pend, 6);
    for (i = 1; i < 6; i++)
        CHECK("sort", vpc_cmp(&e[i - 1], e[i].id, e[i].mtime) < 0);
    for (i = 0; i < 6; i++)
    {
        int orig = (int)(e[i].name - 100);
        static const int32_t p0[6] = { 5, -1, 3, -1, -1, 0 };

        CHECK("sort pend", pend[i] == p0[orig]);
    }
    m = vpc_keep_resolved(e, pend, 6);
    CHECK("keep resolved", m == 3);
    for (i = 1; i < m; i++)
        CHECK("keep resolved order",
              vpc_cmp(&e[i - 1], e[i].id, e[i].mtime) < 0);

    /* merge in chunks == a sorted union; the sum chains over chunks */
    {
        struct vixpass_ent a[3] = { { 1, 0, 1 }, { 3, 0, 3 }, { 5, 0, 5 } };
        struct vixpass_ent b[2] = { { 2, 0, 2 }, { 6, 0, 6 } };
        uint32_t s = VPC_FNV_INIT;

        ia = ib = m = 0;
        while ((c = vpc_merge(a, 3, &ia, b, 2, &ib, buf, 2)) > 0)
        {
            s = vpc_sum(s, buf, c);
            memcpy(&out[m], buf, c * sizeof(buf[0]));
            m += c;
        }
        CHECK("merge", m == 5);
        for (i = 0; i < m; i++)
        {
            static const uint32_t want[5] = { 1, 2, 3, 5, 6 };

            CHECK("merge order", out[i].id == want[i]);
        }
        CHECK("sum chunks", s == vpc_sum(VPC_FNV_INIT, out, 5));
        ia = ib = 0;
        CHECK("merge empty", vpc_merge(a, 0, &ia, b, 0, &ib, buf, 2) == 0);
    }

    /* match: copies take one record each, then none is left */
    {
        struct vixpass_ent o[4] = { { 1, 1, 10 }, { 2, 2, 20 },
                                    { 2, 2, 21 }, { 3, 3, 30 } };
        int k1 = vpc_match(o, 4, used, 2, 2), k2 = vpc_match(o, 4, used, 2, 2);

        CHECK("match copies", k1 >= 1 && k1 <= 2 && k2 >= 1 && k2 <= 2
                              && k1 != k2);
        CHECK("match copies", vpc_match(o, 4, used, 2, 2) == -1);
        CHECK("match mtime", vpc_match(o, 4, used, 3, 4) == -1);
        CHECK("match", vpc_match(o, 4, used, 3, 3) == 3);
        /* claim: by name, unused only, never name 0 */
        CHECK("claim", vpc_claim_name(o, 4, used, 10) == 0);
        CHECK("claim used", vpc_claim_name(o, 4, used, 10) == -1);
        CHECK("claim used", vpc_claim_name(o, 4, used, 20) == -1);
        CHECK("claim zero", vpc_claim_name(o, 4, used, 0) == -1);
        memset(used, 0, sizeof(used));
        VPC_MARK(used, 1);
        o[3].name = 0;
        CHECK("keep unused", vpc_keep_unused(o, used, 4) == 2
                             && o[0].name == 10 && o[1].name == 21);
    }

    /* .vix names */
    CHECK("hex", vpc_hex_name("0123456789abcdef", &n)
                 && n == 0x0123456789abcdefull);
    CHECK("hex upper", !vpc_hex_name("0123456789ABCDEF", &n));
    CHECK("dir name", vpc_dir_name("00000000000000ff.vix", &n) && n == 0xff);
    CHECK("dir name", !vpc_dir_name("00000000000000ff.vix.tmp", &n));
    CHECK("dir name", !vpc_dir_name("vix.dat", &n));
    CHECK("dir name", !vpc_dir_name("0000000000000ff.vixx", &n));
    CHECK("dir name", !vpc_dir_name("00000000000000fg.vix", &n));

    if (fails == f0)
        printf("ok   units (id, header, sort, merge, sum, match, claim, "
               "names)\n");
}

int main(void)
{
    units();
    scenarios();
    printf("test_vixpass: %d failure(s)\n", fails);
    return fails ? 1 : 0;
}
