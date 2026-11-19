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

/* 0.9.0 package G (#235): the library glyph file's writer and checker
 * (apps/ipodui/libglyph.c), the reader's rank map, and the sparse font
 * form's rank (firmware/export/font_sparse.h), on the host against a
 * synthetic sparse font. Numbers that are the format are literals here. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <unistd.h>
#include <sys/stat.h>
#include "font_sparse.h"
#include "libglyph.h"

static int fails, checks;
#define CK(cond, ...) do { checks++; if (!(cond)) { fails++; \
        printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
        printf("\n"); } } while (0)

const char *libglyph_font_path;
static char dir[256], font[300];

int safefile_replace(const char *tmp, const char *dest, const char *prev)
{
    (void)prev;
    return rename(tmp, dest);
}

#define STRIDE 163                      /* the 14 px list companion's */
#define NFONT  80
/* the font's glyphs: 60 ideographs from U+4E00, kana, Hangul, one
 * compatibility ideograph, two full-width forms */
static uint32_t font_cps[NFONT];

static uint8_t rec_byte(uint32_t cp, int i, int salt)
{
    return (uint8_t)(cp * 7 + i * 13 + salt);
}

static void make_font(int salt)
{
    static uint8_t map[FONT_SPARSE_BITMAP];
    uint16_t cnt[FONT_SPARSE_BLOCKS];
    uint8_t hdr[36], rec[STRIDE];
    struct font_sparse_sub sub;
    FILE *f;
    int i, b, n = 0;

    memset(map, 0, sizeof(map));
    for (i = 0; i < NFONT; i++)
    {
        b = font_sparse_bit(font_cps[i]);
        map[b >> 3] |= 1 << (b & 7);
    }
    for (b = 0; b < FONT_SPARSE_BLOCKS; b++)
    {
        int k;
        cnt[b] = (uint16_t)n;
        for (k = b * 256; k < b * 256 + 256 && k < FONT_SPARSE_BITS; k++)
            n += (map[k >> 3] >> (k & 7)) & 1;
    }
    memset(hdr, 0, sizeof(hdr));
    memcpy(hdr, "RB12", 4);
    hdr[4] = 14; hdr[6] = 23; hdr[8] = 18;
    hdr[10] = 1; hdr[11] = 1;           /* depth 1 | FONT_SPARSE_DEPTH */
    sub.magic = FONT_SPARSE_MAGIC;
    sub.count = NFONT;
    sub.stride = STRIDE;
    sub.font_id = 0x1234 + salt;
    f = fopen(font, "wb");
    fwrite(hdr, 1, 36, f);
    fwrite(&sub, 1, sizeof(sub), f);
    fwrite(map, 1, sizeof(map), f);
    fwrite(cnt, 2, FONT_SPARSE_BLOCKS, f);
    for (b = 0; b < FONT_SPARSE_BITS; b++)
    {
        uint32_t cp;
        if (!((map[b >> 3] >> (b & 7)) & 1))
            continue;
        cp = b < 42916 ? 0x3000 + b : b < 43428 ? 0xF900 + b - 42916
                                                : 0xFF00 + b - 43428;
        for (i = 0; i < STRIDE; i++)
            rec[i] = rec_byte(cp, i, salt);
        fwrite(rec, 1, STRIDE, f);
    }
    fclose(f);
}

static long fsize(const char *p)
{
    struct stat st;
    return stat(p, &st) ? -1 : (long)st.st_size;
}

static uint8_t *slurp(const char *p, long *n)
{
    FILE *f = fopen(p, "rb");
    uint8_t *b;

    *n = fsize(p);
    if (!f || *n < 0)
        return NULL;
    b = malloc(*n);
    if (fread(b, 1, *n, f) != (size_t)*n)
        *n = -1;
    fclose(f);
    return b;
}

static void rank(void)
{
    static uint8_t map[FONT_SPARSE_BITMAP];
    uint16_t cnt[FONT_SPARSE_BLOCKS];
    int b, n = 0, bad = 0;

    srand(7);
    for (b = 0; b < FONT_SPARSE_BITS; b++)
        if (rand() % 3 == 0)
            map[b >> 3] |= 1 << (b & 7);
    for (b = 0; b < FONT_SPARSE_BITS; b++)
    {
        if (!(b & 255))
            cnt[b >> 8] = (uint16_t)n;
        if (font_sparse_has(map, b) && font_sparse_rank(map, cnt, b) != n)
            bad++;
        n += font_sparse_has(map, b);
    }
    CK(!bad, "rank wrong at %d bits", bad);
    CK(font_sparse_bit(0x2FFF) < 0 && font_sparse_bit(0x3000) == 0
       && font_sparse_bit(0xD7A3) == 42915 && font_sparse_bit(0xD7A4) < 0
       && font_sparse_bit(0xF900) == 42916 && font_sparse_bit(0xFAFF) == 43427
       && font_sparse_bit(0xFF00) == 43428 && font_sparse_bit(0xFFEF) == 43667
       && font_sparse_bit(0xFFF0) < 0, "windows");
    CK(font_sparse_crc(0, "123456789", 9) == 0xCBF43926u, "crc-32 check");
    CK(FONT_SPARSE_DATA == 5856 && FONT_SPARSE_INDEX == 5804, "layout");
}

static void map(void)
{
    uint16_t cps[500], perm[500];
    int i, bad = 0;

    for (i = 0; i < 500; i++)
        cps[i] = (uint16_t)(0x3000 + (i * 7919) % 30000);
    libglyph_map_sort(cps, perm, 500);
    for (i = 1; i < 500; i++)
        if (cps[perm[i - 1]] >= cps[perm[i]])
            bad++;
    CK(!bad, "sort: %d out of order", bad);
    for (i = 0; i < 500; i++)
        if (libglyph_map_find(cps, perm, 500, cps[i]) != i)
            bad++;
    CK(!bad, "find: %d wrong", bad);
    CK(libglyph_map_find(cps, perm, 500, 0x2FFF) == -1, "absent found");
    CK(libglyph_map_find(cps, perm, 0, 0x3000) == -1, "empty");
}

/* writes the library: returns end()'s count */
static int write_lib(const struct libglyph_stamp *st, const char **s, int n,
                     bool commit)
{
    static uint32_t work[(LIBGLYPH_WORK_MIN + 3) / 4];
    struct libglyph_writer w;
    int i;

    if (libglyph_begin(&w, dir, st, work, sizeof(work)) != LIBGLYPH_OK)
        return -100;
    for (i = 0; i < n; i++)
        if (libglyph_add(&w, s[i]) != LIBGLYPH_OK)
            return -101;
    return libglyph_end(&w, commit);
}

static void writer(void)
{
    static uint32_t work[(LIBGLYPH_WORK_MIN + 3) / 4];
    struct libglyph_writer w;
    struct libglyph_stamp st = { 42, 1234 }, other;
    struct libglyph_hdr h;
    char dest[400], tmp[400];
    const char *strs[80];
    char store[80][32];
    uint32_t order[NFONT];
    int i, n, norder = 0, bad;
    long len;
    uint8_t *f;

    snprintf(dest, sizeof(dest), "%s/database_glyph.dat", dir);
    snprintf(tmp, sizeof(tmp), "%s/database_glyph.dat.tmp", dir);
    CK(sizeof(struct libglyph_hdr) == 36 && LIBGLYPH_WORK_MIN == 15360
       && LIBGLYPH_PAGE == 4096, "contract sizes");
    CK(libglyph_begin(&w, dir, &st, work, LIBGLYPH_WORK_MIN - 1)
       == LIBGLYPH_ERROR, "short work buffer accepted");
    CK(libglyph_begin(&w, dir, &st, (char *)work + 1, LIBGLYPH_WORK_MIN)
       == LIBGLYPH_ERROR, "misaligned work buffer accepted");

    /* no list font: begin fails, add/end refuse, nothing on disk */
    libglyph_font_path = "/nonexistent/font.fnt";
    CK(libglyph_begin(&w, dir, &st, work, sizeof(work)) == LIBGLYPH_ERROR,
       "begin without a font");
    CK(libglyph_add(&w, "\xe4\xb8\x80") == LIBGLYPH_ERROR
       && libglyph_end(&w, true) < 0, "add/end after a failed begin");
    CK(fsize(dest) < 0 && fsize(tmp) < 0, "file left without a font");
    libglyph_font_path = font;
    CK(libglyph_check(dir, &st, &h) == LIBGLYPH_MISSING, "missing");

    /* a Latin library: a header page, no glyphs, checks OK */
    strs[0] = "Latin"; strs[1] = "Caf\xc3\xa9"; strs[2] = NULL; strs[3] = "";
    CK(write_lib(&st, strs, 4, true) == 0, "latin count");
    CK(fsize(dest) == 4096 && fsize(tmp) < 0, "latin file %ld", fsize(dest));
    CK(libglyph_check(dir, &st, &h) == LIBGLYPH_OK && h.count == 0,
       "latin check");

    /* a CJK library: first appearances across 3 pages, duplicates, glyphs
     * the font lacks (U+3000, U+4E70), a broken byte, Latin around them */
    n = 0;
    strs[n++] = "\xe4\xb8\x80\xe3\x81\x82 Latin";             /* 一あ */
    strs[n++] = "x \xe4\xb8\x80\xe3\x80\x80\xe4\xb9\xb0";     /* 一　买 */
    strs[n++] = "\xea\xb0\x80\xe3\x81\x84\xff\xe4\xb8\x81";   /* 가い?丁 */
    strs[n++] = "\xef\xbc\xa1\xef\xa4\xa9";                   /* ＡF929 */
    order[norder++] = 0x4E00; order[norder++] = 0x3042;
    order[norder++] = 0xAC00; order[norder++] = 0x3044;
    order[norder++] = 0x4E01; order[norder++] = 0xFF21;
    order[norder++] = 0xF929;
    for (i = 59; i >= 2; i--)                       /* the rest, reversed */
    {
        uint32_t c = 0x4E00 + i;
        snprintf(store[i], sizeof(store[i]), "%c%c%c Song", 0xE0 | (c >> 12),
                 0x80 | ((c >> 6) & 63), 0x80 | (c & 63));
        strs[n++] = store[i];
        order[norder++] = c;
    }
    CK(write_lib(&st, strs, n, true) == norder, "cjk count");
    f = slurp(dest, &len);
    /* per page (4096 / (163 + 2)) = 24 records: 65 glyphs in 3 pages */
    CK(len == 4096 * 4 + 132, "cjk size %ld", len);
    CK(libglyph_check(dir, &st, &h) == LIBGLYPH_OK && h.count == 65
       && h.stride == STRIDE && h.font_id == 0x1234 && h.commitid == 42
       && h.entries == 1234 && h.flags == 0, "cjk check");
    bad = 0;
    for (i = 0; f && i < norder; i++)
    {
        int p = i / 24, k = i % 24, j;
        const uint8_t *page = f + 4096 * (1 + p);
        const uint8_t *tab = f + 4096 * 4;
        if (((const uint16_t *)page)[k] != order[i]
            || ((const uint16_t *)tab)[i] != order[i])
            bad++;
        for (j = 0; j < STRIDE; j++)
            if (page[48 + k * STRIDE + j] != rec_byte(order[i], j, 0))
            {
                bad++;
                break;
            }
    }
    CK(f && !bad, "cjk records/order: %d wrong", bad);
    free(f);

    /* stale: another commit, another entry count, a damaged header, a
     * short file, another font */
    other = st; other.commitid = 43;
    CK(libglyph_check(dir, &other, &h) == LIBGLYPH_STALE, "commit");
    other = st; other.entries = 1235;
    CK(libglyph_check(dir, &other, &h) == LIBGLYPH_STALE, "entries");
    make_font(1);
    CK(libglyph_check(dir, &st, &h) == LIBGLYPH_STALE, "font id");
    make_font(0);
    CK(libglyph_check(dir, &st, &h) == LIBGLYPH_OK, "font back");
    {
        FILE *fp = fopen(dest, "r+b");
        CK(fp != NULL, "no file to damage");
        if (fp)
        {
            fseek(fp, 28, SEEK_SET);
            fputc(0x55, fp);    /* flags: only the sum notices */
            fclose(fp);
        }
    }
    CK(libglyph_check(dir, &st, &h) == LIBGLYPH_STALE, "sum");
    CK(write_lib(&st, strs, n, true) == norder, "rewrite");
    CK(truncate(dest, 4096 * 4) == 0
       && libglyph_check(dir, &st, &h) == LIBGLYPH_STALE, "short");

    /* a dropped commit leaves the published file and no .tmp */
    CK(write_lib(&st, strs, n, true) == norder, "rewrite 2");
    other = st; other.commitid = 99;
    CK(write_lib(&other, strs, 1, false) == 0, "drop count");
    CK(fsize(tmp) < 0 && libglyph_check(dir, &st, &h) == LIBGLYPH_OK,
       "drop kept the old file, no tmp");
}

int main(void)
{
    char tmpl[] = "/tmp/libglyph.XXXXXX";
    int i = 0;
    uint32_t c;

    for (c = 0x4E00; c < 0x4E00 + 60; c++)
        font_cps[i++] = c;
    font_cps[i++] = 0x3042; font_cps[i++] = 0x3044; font_cps[i++] = 0x3046;
    font_cps[i++] = 0xAC00; font_cps[i++] = 0xAC01; font_cps[i++] = 0xD7A3;
    font_cps[i++] = 0xF929; font_cps[i++] = 0xFF21; font_cps[i++] = 0xFF71;
    while (i < NFONT)
        font_cps[i] = 0xAC10 + i, i++;
    if (!mkdtemp(tmpl))
        return 2;
    snprintf(dir, sizeof(dir), "%s", tmpl);
    snprintf(font, sizeof(font), "%s/14-ShellCJK-Bold.fnt", tmpl);
    make_font(0);
    rank();
    map();
    writer();
    {
        char cmd[400];
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", tmpl);
        if (system(cmd))
            fails++;
    }
    printf("   p090 libglyph: %d checks, %d failures\n", checks, fails);
    return fails != 0;
}
