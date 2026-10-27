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
 * Host test: module pack, relocation, build ID, arena fit (#106)
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
/* Usage: test_modload [fletcher_hex] [pak core_id_hex a_bin b_bin ...]
 *  - pure checks always run;
 *  - fletcher_hex: modpak.py's Fletcher-32 of the 1000-byte pattern below;
 *  - pak ...: a real modules.pak, the core's patched build ID, and per
 *    module (pack order) its .text linked at the base and at base + 1 MB:
 *    settling a module at base + 1 MB must give the second link exactly. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ipodui/modload_core.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void *slurp(const char *path, long *n)
{
    FILE *f = fopen(path, "rb");
    void *p;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    fseek(f, 0, SEEK_SET);
    p = malloc(*n + 16);
    if (fread(p, 1, *n, f) != (size_t)*n)
        *n = -1;
    fclose(f);
    return p;
}

static void pure(void)
{
    uint32_t img[8] = { 0x03000010, 7, 0x03000004, 9, 0, 0, 0, 0 };
    uint16_t rel[2] = { 0, 2 }, bad[2] = { 0, 8 };
    struct modpak_hdr h;
    struct mod_span sp[3] = { { 0, 100 }, { 256, 64 }, { 0, 0 } };
    uint8_t buf[256];
    struct modpak_ent e;

    /* relocation: only the listed words, by the delta */
    CHECK(mod_relocate(img, sizeof(img), rel, 2, 0x100) == MODPAK_OK);
    CHECK(img[0] == 0x03000110 && img[1] == 7 && img[2] == 0x03000104
          && img[3] == 9);
    /* an index outside the image changes nothing */
    CHECK(mod_relocate(img, 16, bad, 2, 0x100) == MODPAK_ERELOC);
    CHECK(img[0] == 0x03000110);
    /* two relocations back = identity */
    CHECK(mod_relocate(img, sizeof(img), rel, 2, -0x100) == MODPAK_OK);
    CHECK(img[0] == 0x03000010 && img[2] == 0x03000004);

    /* settle: relocs move behind .bss, .bss zeroed, words fixed */
    memset(buf, 0xee, sizeof(buf));
    memset(&e, 0, sizeof(e));
    e.image = 16; e.bss_off = 16; e.bss = 20; e.nreloc = 2;
    memcpy(buf, img, 16);
    memcpy(buf + 16, rel, 4);
    CHECK(modpak_read_bytes(&e) == 20);
    CHECK(modpak_reloc_at(&e) == 36);
    CHECK(modpak_footprint(&e) == 48);
    CHECK(mod_settle(buf, &e, 0x03000000, 0x00123400) == MODPAK_OK);
    CHECK(((uint32_t *)buf)[0] == 0x00123410);
    CHECK(((uint32_t *)buf)[2] == 0x00123404);
    CHECK(((uint32_t *)buf)[1] == 7);
    { int i, z = 1; for (i = 16; i < 36; i++) z &= buf[i] == 0; CHECK(z); }
    CHECK(((uint16_t *)(buf + 36))[0] == 0 && ((uint16_t *)(buf + 36))[1] == 2);

    /* header checks: the build ID refuses another build */
    memset(&h, 0, sizeof(h));
    h.magic = MODPAK_MAGIC; h.version = MODPAK_VERSION; h.count = 1;
    h.build_id = 0x1234abcd; h.link_base = 0x03000000; h.total = 4096 + 20;
    h.ent[0].offset = 4096; h.ent[0].image = 16; h.ent[0].bss_off = 16;
    h.ent[0].nreloc = 2; h.ent[0].nexport = 1;
    CHECK(modpak_check(&h, 4116, 0x1234abcd) == MODPAK_OK);
    CHECK(modpak_check(&h, 4116, 0x1234abce) == MODPAK_EBUILD);
    CHECK(modpak_check(&h, 4115, 0x1234abcd) == MODPAK_EBOUNDS);
    h.ent[0].offset = 4000;
    CHECK(modpak_check(&h, 4116, 0x1234abcd) == MODPAK_EBOUNDS);
    h.ent[0].offset = 4096; h.magic ^= 1;
    CHECK(modpak_check(&h, 4116, 0x1234abcd) == MODPAK_EMAGIC);
    /* P12: a v1 pack (32 B entries) is refused by version */
    h.magic ^= 1; h.version = 1;
    CHECK(modpak_check(&h, 4116, 0x1234abcd) == MODPAK_EVERSION);
    h.version = 3;
    CHECK(modpak_check(&h, 4116, 0x1234abcd) == MODPAK_EVERSION);
    h.version = MODPAK_VERSION;
    CHECK(modpak_find(&h, 0) == &h.ent[0] && modpak_find(&h, 1) == NULL);

    /* first fit, MOD_ALIGN steps */
    CHECK(mod_fit(sp, 2, 1024, 100) == 112);
    CHECK(mod_fit(sp, 2, 1024, 200) == 320);
    CHECK(mod_fit(sp, 2, 400, 200) == -1);
    CHECK(mod_fit(sp, 0, 64, 64) == 0);
}

/* P12 pack v2 (docs/phase12-brief.md 3.1): the 44 B entry, 16 modules,
 * the class labels, hidden-if-unrunnable, the module header */
static void v2(void)
{
    static struct modpak_hdr h;
    struct mod_desc d;
    struct modpak_ent *e;
    uint32_t off = MODPAK_INDEX, i;

    CHECK(sizeof(struct modpak_ent) == 44);
    CHECK(sizeof(struct modpak_hdr) == 32 + 16 * 44);
    CHECK(MODPAK_MAX == 16 && MOD_IDX_STAGE >= sizeof(struct modpak_hdr));

    /* a full index: 16 Class A shell modules, one per 2 KB */
    memset(&h, 0, sizeof(h));
    h.magic = MODPAK_MAGIC; h.version = MODPAK_VERSION; h.count = MODPAK_MAX;
    h.build_id = 0x0badcafe; h.link_base = 0x03000000;
    for (i = 0; i < MODPAK_MAX; i++, off += MODPAK_ALIGN)
    {
        e = &h.ent[i];
        e->id = i; e->offset = off; e->image = 64; e->bss_off = 64;
        e->nexport = 2; e->exit_policy = MOD_EXIT_IMMEDIATE;
    }
    h.total = off - MODPAK_ALIGN + 64;
    CHECK(modpak_check(&h, h.total, 0x0badcafe) == MODPAK_OK);
    CHECK(modpak_find(&h, 15) == &h.ent[15]);
    h.count = MODPAK_MAX + 1;
    CHECK(modpak_check(&h, h.total, 0x0badcafe) == MODPAK_EBOUNDS);
    h.count = MODPAK_MAX;

    /* labels in range; Class A is always a 32 MB module */
    e = &h.ent[3];
    e->mclass = MOD_CLASS__N;
    CHECK(modpak_check(&h, h.total, 0x0badcafe) == MODPAK_EBOUNDS);
    e->mclass = MOD_CLASS_A; e->min_ram = MOD_RAM_64;
    CHECK(modpak_check(&h, h.total, 0x0badcafe) == MODPAK_EBOUNDS);
    e->mclass = MOD_CLASS_B;            /* a 64 MB Class B game: valid */
    CHECK(modpak_check(&h, h.total, 0x0badcafe) == MODPAK_OK);
    e->kind = MOD_KIND__N;
    CHECK(modpak_check(&h, h.total, 0x0badcafe) == MODPAK_EBOUNDS);
    e->kind = MOD_KIND_SHELL; e->exit_policy = MOD_EXIT__N;
    CHECK(modpak_check(&h, h.total, 0x0badcafe) == MODPAK_EBOUNDS);
    e->exit_policy = MOD_EXIT_IMMEDIATE;

    /* hidden, never an error: a 64 MB module on a 32 MB probe; a class
     * this firmware does not build; an SDK module of another ABI */
    CHECK(mod_why_hidden(e, MOD_RAM_32, MOD_CLASSES_BUILT) == MOD_WHY_RAM);
    CHECK(mod_why_hidden(e, MOD_RAM_64, MOD_CLASSES_BUILT)
          == MOD_WHY_CLASS);
    CHECK(mod_why_hidden(e, MOD_RAM_64, MOD_CLASSES_BUILT
                         | (1u << MOD_CLASS_B)) == MOD_WHY_OK);
    CHECK(mod_why_hidden(&h.ent[0], MOD_RAM_32, MOD_CLASSES_BUILT)
          == MOD_WHY_OK);
    e = &h.ent[4];
    e->kind = MOD_KIND_SDK; e->abi_major = SDK_ABI_MAJOR;
    e->abi_minor = SDK_ABI_MINOR;
    CHECK(mod_why_hidden(e, MOD_RAM_32, MOD_CLASSES_BUILT) == MOD_WHY_OK);
    e->abi_minor = SDK_ABI_MINOR + 1;   /* built for a newer core */
    CHECK(mod_why_hidden(e, MOD_RAM_32, MOD_CLASSES_BUILT) == MOD_WHY_ABI);
    e->abi_minor = 0; e->abi_major = SDK_ABI_MAJOR + 1;
    CHECK(mod_why_hidden(e, MOD_RAM_32, MOD_CLASSES_BUILT) == MOD_WHY_ABI);
    e->abi_major = SDK_ABI_MAJOR;

    /* the module header (export slot 0) against its pack entry */
    memset(&d, 0, sizeof(d));
    d.magic = MOD_HDR_MAGIC; d.mclass = e->mclass; d.min_ram = e->min_ram;
    d.kind = e->kind; d.exit_policy = e->exit_policy;
    d.abi_major = e->abi_major; d.abi_minor = e->abi_minor;
    CHECK(mod_desc_check(&d, e) == MODPAK_OK);
    d.work_kb = 4;                      /* the manifest says 0 */
    CHECK(mod_desc_check(&d, e) == MODPAK_EHDR);
    d.work_kb = 0; d.magic ^= 1;
    CHECK(mod_desc_check(&d, e) == MODPAK_EHDR);
    d.magic ^= 1; d.mclass = MOD_CLASS_C;
    CHECK(mod_desc_check(&d, e) == MODPAK_EHDR);
    d.mclass = e->mclass; d.exit_policy = e->exit_policy = MOD_EXIT_GRACEFUL;
    d.exit_ms = SDK_EXIT_MS_MAX;
    CHECK(mod_desc_check(&d, e) == MODPAK_OK);
    d.exit_ms = SDK_EXIT_MS_MAX + 1;    /* over the Q19 ceiling */
    CHECK(mod_desc_check(&d, e) == MODPAK_EHDR);
    d.exit_ms = 0; d.exit_policy = e->exit_policy = MOD_EXIT_IMMEDIATE;
    /* a page-load rate (W3-K): Class A, 1..MOD_PAGES_MAX */
    CHECK(e->mclass == MOD_CLASS_A);
    d.pages = 8;
    CHECK(mod_desc_check(&d, e) == MODPAK_OK);
    d.pages = MOD_PAGES_MAX;
    CHECK(mod_desc_check(&d, e) == MODPAK_OK);
    d.pages = MOD_PAGES_MAX + 1;
    CHECK(mod_desc_check(&d, e) == MODPAK_EHDR);
    d.pages = 8; d.mclass = e->mclass = MOD_CLASS_B;
    CHECK(mod_desc_check(&d, e) == MODPAK_EHDR);
    d.pages = 0;
    CHECK(mod_desc_check(&d, e) == MODPAK_OK);
    d.mclass = e->mclass = MOD_CLASS_A;
}

/* A file with Rockbox's lseek rule: no seek past the end (file.c) */
struct rbfile { uint8_t data[8192]; uint32_t size, pos; int seeks_refused; };
/* W1-F: the index compacted into mod_rt[] (brief 3.1, M1), and back */
static void rt(void)
{
    struct modpak_ent e, back;
    struct mod_rt r;

    CHECK(sizeof(struct mod_rt) == 40);
    CHECK(MODPAK_MAX * sizeof(struct mod_rt) == 640);
    memset(&e, 0, sizeof(e));
    e.id = 5; e.offset = 6144; e.image = 6112; e.bss_off = 6112;
    e.bss = 260; e.nreloc = 93; e.nexport = 4; e.fletcher = 0x12345678;
    e.mclass = MOD_CLASS_B; e.min_ram = MOD_RAM_64; e.abi_major = 0;
    e.abi_minor = 1; e.work_kb = 4; e.kind = MOD_KIND_SDK;
    e.exit_policy = MOD_EXIT_GRACEFUL;
    memset(&r, 0xa5, sizeof(r));        /* runtime fields are kept */
    CHECK(mod_rt_fill(&r, &e) == MODPAK_OK);
    CHECK(r.refs == 0xa5 && r.lru == 0xa5 && r.state == 0xa5
          && r.arena_off == 0xa5a5a5a5u);
    mod_rt_ent(&r, &back);
    CHECK(memcmp(&e, &back, sizeof(e)) == 0);
    CHECK(mod_rt_footprint(&r) == modpak_footprint(&e));
    /* what the compact table cannot hold reads as absent */
    e.bss = 0x10000;
    CHECK(mod_rt_fill(&r, &e) == MODPAK_EBOUNDS);
    e.bss = 260; e.bss_off = 6116;      /* .bss not right after the image */
    CHECK(mod_rt_fill(&r, &e) == MODPAK_EBOUNDS);
    e.bss_off = 6112; e.nreloc = 0x10000;
    CHECK(mod_rt_fill(&r, &e) == MODPAK_EBOUNDS);
}

static long rb_seek(void *h, uint32_t off)
{
    struct rbfile *f = h;

    if (off > f->size)
    {
        f->seeks_refused++;
        return -1;
    }
    f->pos = off;
    return off;
}
static long rb_write(void *h, const void *p, uint32_t n)
{
    struct rbfile *f = h;

    if (f->pos + n > sizeof(f->data))
        return -1;
    memcpy(f->data + f->pos, p, n);
    f->pos += n;
    if (f->pos > f->size)
        f->size = f->pos;
    return n;
}

static void file_put(void)
{
    static const struct mod_fileio io = { rb_seek, rb_write };
    static struct rbfile f;
    uint32_t size = 0, magic = 0x50414e53u, i;
    uint8_t slot[300];
    int ok;

    memset(&f, 0xee, sizeof(f.data));
    f.size = f.pos = 0;
    f.seeks_refused = 0;
    memset(slot, 0x5a, sizeof(slot));
    /* a new snap.pak: magic, then a slot at 2048: the gap fills with zeros
     * in order, never a seek past the end */
    CHECK(mod_file_put(&io, &f, &size, 0, &magic, 4) == 0);
    CHECK(mod_file_put(&io, &f, &size, 2048, slot, sizeof(slot)) == 0);
    CHECK(f.seeks_refused == 0 && size == 2048 + sizeof(slot)
          && f.size == size);
    for (ok = 1, i = 4; i < 2048; i++)
        ok &= f.data[i] == 0;
    CHECK(ok);
    CHECK(memcmp(f.data, &magic, 4) == 0 && f.data[2048] == 0x5a);
    /* in place: no growth */
    CHECK(mod_file_put(&io, &f, &size, 4, slot, 8) == 0 && size == 2348);
    /* a raw seek past the end is what Rockbox refuses */
    CHECK(rb_seek(&f, 9000) < 0);
}

static void fletcher(const char *hex)
{
    uint8_t p[1001];
    int i;

    for (i = 0; i < 1001; i++)
        p[i] = (uint8_t)(i * 7 + 3);
    /* an odd length pads a zero byte */
    CHECK(mod_fletcher32(p, 1001) == (uint32_t)strtoul(hex, NULL, 16));
    CHECK(mod_fletcher32(p, 1000) != mod_fletcher32(p, 1001));
}

static void real_pak(int argc, char **argv)
{
    long n, na, nb;
    uint8_t *pak = slurp(argv[0], &n);
    const struct modpak_hdr *h = (const void *)pak;
    uint32_t core = (uint32_t)strtoul(argv[1], NULL, 16);
    int i;

    CHECK(pak && n > MODPAK_INDEX);
    if (!pak)
        return;
    CHECK(modpak_check(h, (uint32_t)n, core) == MODPAK_OK);
    CHECK(modpak_check(h, (uint32_t)n, core ^ 1) == MODPAK_EBUILD);
    for (i = 0; i < h->count && 2 + 2 * i + 1 < argc; i++)
    {
        const struct modpak_ent *e = &h->ent[i];
        uint8_t *a = slurp(argv[2 + 2 * i], &na), *b = slurp(argv[3 + 2 * i], &nb);
        uint8_t *run = calloc(1, modpak_footprint(e) + 16);

        CHECK(a && b && (uint32_t)na == e->image && (uint32_t)nb == e->image);
        if (!a || !b)
            continue;
        CHECK(mod_fletcher32(pak + e->offset, modpak_read_bytes(e))
              == e->fletcher);
        CHECK(memcmp(pak + e->offset, a, e->image) == 0);
        memcpy(run, pak + e->offset, modpak_read_bytes(e));
        CHECK(mod_settle(run, e, h->link_base, h->link_base + 0x100000)
              == MODPAK_OK);
        CHECK(memcmp(run, b, e->image) == 0);
        /* P12: slot 0 is the module header, equal to the entry */
        {
            uint32_t at = ((const uint32_t *)(const void *)b)[0]
                          - (h->link_base + 0x100000);

            CHECK(e->nexport >= 2 && at + sizeof(struct mod_desc) <= e->image);
            if (at + sizeof(struct mod_desc) <= e->image)
            {
                struct mod_desc d;

                memcpy(&d, run + at, sizeof(d));
                CHECK(mod_desc_check(&d, e) == MODPAK_OK);
            }
        }
        printf("ok   module %u: %u B, %u relocations, settled = linked at "
               "+1 MB\n", e->id, e->image, e->nreloc);
        free(a); free(b); free(run);
    }
    free(pak);
}

/* -- H1: the room policy (mod_room_find, mod_room_possible) ------------- */
/* A model arena: modules with a place and an age (MOD_AGE_* or >= 0),
 * working blocks as extra spans, an LRU eviction log and a compaction
 * that slides idle modules down as modload.c's compact() does. Sizes are
 * the P12 integration's check.ok figures. */
enum { T_SET, T_NOTES, T_CONTACTS, T_XINDEX, T_GAME, T_N };
#define F_SET      5600u
#define F_NOTES    6640u
#define W_NOTES    16384u
#define F_CONTACTS 5680u
#define F_XINDEX   24720u
#define F_GAME     7168u                /* solitaire */
#define W_GAME     12288u
#define USE_REL    (33792u - MOD_IDX_STAGE)     /* the new arenas */
#define USE_DEV    (39936u - MOD_IDX_STAGE)
#define USE_DEV_OLD (35840u - MOD_IDX_STAGE)    /* before H1 */
#define USE_REL_OLD (29696u - MOD_IDX_STAGE)

static struct tmod { uint32_t off, len; int placed, age; } tm[T_N];
static struct mod_span twork[2];
static int ntwork, tlog[T_N], nevict;

static bool t_span(void *ctx, int i, struct mod_span *sp)
{
    (void)ctx;
    if (!tm[i].placed)
        return false;
    sp->off = tm[i].off;
    sp->len = tm[i].len;
    return true;
}

static int t_age(void *ctx, int i)
{
    (void)ctx;
    return tm[i].age;
}

static int t_extra(void *ctx, struct mod_span *sp, int max)
{
    int i;

    (void)ctx;
    for (i = 0; i < ntwork && i < max; i++)
        sp[i] = twork[i];
    return i;
}

static void t_evict(void *ctx, int i)
{
    (void)ctx;
    tm[i].placed = 0;
    tlog[nevict++] = i;
}

static int t_spans_but(struct mod_span *sp, int skip)
{
    int i, n = 0;

    for (i = 0; i < T_N; i++)
        if (i != skip && tm[i].placed)
        {
            sp[n].off = tm[i].off;
            sp[n].len = tm[i].len;
            n++;
        }
    for (i = 0; i < ntwork; i++)
        sp[n++] = twork[i];
    return n;
}

static void t_compact(void *ctx)
{
    (void)ctx;
    for (;;)
    {
        struct mod_span sp[MOD_ROOM_SPANS];
        int i, lo = -1;
        long to = -1;

        for (i = 0; i < T_N; i++)
        {
            long t;

            if (!tm[i].placed || tm[i].age < 0
                || (lo >= 0 && tm[lo].off < tm[i].off))
                continue;
            t = mod_fit(sp, t_spans_but(sp, i), 1u << 20, tm[i].len);
            if (t >= 0 && (uint32_t)t < tm[i].off)
            {
                lo = i;
                to = t;
            }
        }
        if (lo < 0)
            return;
        tm[lo].off = (uint32_t)to;
    }
}

static const struct mod_room troom = {
    .n = T_N, .ctx = NULL, .span = t_span, .age = t_age, .extra = t_extra,
    .evict = t_evict, .compact = t_compact,
};

static void t_reset(void)
{
    memset(tm, 0, sizeof(tm));
    ntwork = nevict = 0;
}

static void t_put(int i, uint32_t off, uint32_t len, int age)
{
    tm[i].off = off;
    tm[i].len = len;
    tm[i].placed = 1;
    tm[i].age = age;
}

static void room(void)
{
    long at;

    /* the review's case: a build has finished, the idle indexer stays at
     * 0 and Notes (held) sits above it; its 16 KB block found no room in
     * the old developer arena (9,072 B free). Now the indexer is evicted
     * for the block, which lands where it was. */
    t_reset();
    t_put(T_XINDEX, 0, F_XINDEX, 4);
    t_put(T_NOTES, F_XINDEX, F_NOTES, MOD_AGE_HELD);
    at = mod_room_find(&troom, USE_DEV_OLD, W_NOTES, T_NOTES);
    CHECK(at == 0);
    CHECK(nevict == 1 && tlog[0] == T_XINDEX && tm[T_NOTES].placed);

    /* a build running (the indexer pinned: it yields): Notes' image and
     * block together don't fit beside it in the release arena; nothing is
     * evicted, the caller is told to ask the build to stop */
    t_reset();
    t_put(T_XINDEX, 0, F_XINDEX, MOD_AGE_YIELD);
    at = mod_room_find(&troom, USE_REL, F_NOTES + W_NOTES, T_NOTES);
    CHECK(at == MOD_ROOM_YIELD && nevict == 0 && tm[T_XINDEX].placed);
    CHECK(mod_room_possible(&troom, USE_REL, F_NOTES + W_NOTES, T_NOTES)
          == (int)MOD_ROOM_YIELD);
    /* the old release arena could not even take Notes' image beside it */
    CHECK(mod_room_find(&troom, USE_REL_OLD, F_NOTES, T_NOTES)
          == MOD_ROOM_YIELD);
    /* the build stopped: the indexer is idle, evicted, Notes goes in */
    tm[T_XINDEX].age = 0;
    at = mod_room_find(&troom, USE_REL, F_NOTES + W_NOTES, T_NOTES);
    CHECK(at == 0 && nevict == 1 && tlog[0] == T_XINDEX);

    /* a game after a build (developer arena): the game pinned, its 12 KB
     * block refused before ("init refused nomem"); now made */
    t_reset();
    t_put(T_XINDEX, 0, F_XINDEX, 7);
    t_put(T_GAME, F_XINDEX, F_GAME, MOD_AGE_HELD);
    at = mod_room_find(&troom, USE_DEV_OLD, W_GAME, T_GAME);
    CHECK(at == 0 && nevict == 1 && tlog[0] == T_XINDEX);

    /* least recently used first, and only as many as needed */
    t_reset();
    t_put(T_SET, 0, F_SET, 9);
    t_put(T_CONTACTS, F_SET, F_CONTACTS, 3);
    at = mod_room_find(&troom, USE_REL, 20000, -1);
    CHECK(at == (long)(F_SET + F_CONTACTS) && nevict == 0);
    t_reset();
    t_put(T_SET, 0, F_SET, 9);
    t_put(T_CONTACTS, F_SET, F_CONTACTS, 3);
    at = mod_room_find(&troom, USE_REL, 22000, -1);
    CHECK(nevict == 1 && tlog[0] == T_SET && tm[T_CONTACTS].placed);
    CHECK(at == (long)F_CONTACTS && tm[T_CONTACTS].off == 0);

    /* never the asking module, even when it is the oldest */
    t_reset();
    t_put(T_NOTES, 0, F_NOTES, 50);
    t_put(T_XINDEX, F_NOTES, F_XINDEX, 1);
    at = mod_room_find(&troom, USE_REL, W_NOTES, T_NOTES);
    CHECK(at >= 0 && nevict == 1 && tlog[0] == T_XINDEX
          && tm[T_NOTES].placed);

    /* held and pinned tenants and live blocks are never evicted; with no
     * room even then, nothing is evicted at all */
    t_reset();
    t_put(T_GAME, 0, F_GAME, MOD_AGE_HELD);
    twork[ntwork].off = 7168;
    twork[ntwork++].len = W_GAME;
    t_put(T_SET, F_GAME + W_GAME, F_SET, 2);
    at = mod_room_find(&troom, USE_REL, F_NOTES + W_NOTES, T_NOTES);
    CHECK(at == MOD_ROOM_NONE && nevict == 0 && tm[T_SET].placed);
    CHECK(mod_room_possible(&troom, USE_REL, F_NOTES + W_NOTES, T_NOTES)
          == 0);
    /* but an idle one goes for a request that fits once it has */
    at = mod_room_find(&troom, USE_REL, 12000, T_NOTES);
    CHECK(at == (long)(F_GAME + W_GAME) && nevict == 1 && tlog[0] == T_SET);

    /* no evict callback (the simulator's static modules): no room */
    {
        struct mod_room r = troom;

        t_reset();
        t_put(T_SET, 0, F_SET, 1);
        r.evict = NULL;
        CHECK(mod_room_find(&r, F_SET + 100, 200, -1) == MOD_ROOM_NONE);
    }

    /* the sizing rule (modload.h): the largest tenant fits, alone, in the
     * modules' part of each new arena with 25 % headroom */
    CHECK((F_XINDEX + MOD_IDX_STAGE) * 5 / 4 <= USE_REL + MOD_IDX_STAGE);
    CHECK((F_NOTES + W_NOTES + MOD_IDX_STAGE) * 5 / 4
          <= USE_REL + MOD_IDX_STAGE);
    CHECK((F_XINDEX + 4512u + MOD_IDX_STAGE) * 5 / 4
          <= USE_DEV + MOD_IDX_STAGE);
}

int main(int argc, char **argv)
{
    pure();
    v2();
    rt();
    file_put();
    room();
    if (argc > 1)
        fletcher(argv[1]);
    if (argc > 3)
        real_pak(argc - 2, argv + 2);
    if (fails)
        return 1;
    printf("ok   module loader core (relocation, settle, pack v2 checks, "
           "classes, hidden, module header, mod_rt, build ID, fit, Fletcher-32, "
           "file growth, room policy)\n");
    return 0;
}
