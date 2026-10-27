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

/* Host generator for firmware/common/nfc_table.h and the NFC golden corpus
 * (docs/phase9-brief.md 3.3.2, Rev 1 H4). Everything comes from the in-tree
 * lib/utf8proc data; nothing is downloaded.
 *
 *   cc -O1 -w -Ilib/utf8proc -o gen_nfc tools/gen_nfc.c
 *   ./gen_nfc table > firmware/common/nfc_table.h   (split on stderr)
 *   ./gen_nfc cases > nfc_cases.txt                 (tests/unicode/run.sh)
 *
 * The pair table holds exactly the (starter, second) pairs that utf8proc's
 * compose loop (utf8proc_normalize_utf32) can find: the starter's CSR row,
 * the second's comb_issecond flag and the row's sorted early-out are all
 * replayed here, so a pair utf8proc can't reach is not emitted. Hangul is
 * algorithmic in both and never in the table (asserted).
 *
 * Case lines: "<flag> <hex bytes>". B = output must be byte-identical to
 * the utf8proc reference (unicode.c's call); N = nfc only, string must be
 * unchanged (>= 2048 code points: the reference overruns normbuf there). */

/* The ProkBox banner (docs/templates/prokbox-header.txt), at the top of
 * every file this generator writes. */
#define PROKBOX_BANNER \
    "/***************************************************************************\n" \
    " *              __________                   __    __________\n" \
    " *   Open       \\______   \\_______    ____  |  | __\\______   \\  ____  ___  ___\n" \
    " *   Source      |     ___/\\_  __ \\  /  _ \\ |  |/ / |    |  _/ /  _ \\ \\  \\/  /\n" \
    " *   Player      |    |     |  | \\/ (  <_> )|    <  |    |   \\(  <_> ) >    <\n" \
    " *   Firmware    |____|     |__|     \\____/ |__|_ \\ |______  / \\____/ /__/\\_ \\\n" \
    " *                                               \\/        \\/               \\/\n" \
    " *\n" \
    " * Custom firmware for the iPod Video, 5th generation (5G and 5.5G).\n" \
    " * Not affiliated with or endorsed by Apple Inc.\n" \
    " *\n" \
    " * Copyright (C) 2026 prokopto-dev\n" \
    " *\n" \
    " * This program is free software; you can redistribute it and/or\n" \
    " * modify it under the terms of the GNU General Public License\n" \
    " * as published by the Free Software Foundation; either version 2\n" \
    " * of the License, or (at your option) any later version.\n" \
    " *\n" \
    " * This software is distributed on an \"AS IS\" basis, WITHOUT WARRANTY OF ANY\n" \
    " * KIND, either express or implied.\n" \
    " *\n" \
    " ****************************************************************************/\n"

#include "utf8proc.c"   /* static tables + unsafe_get_property() */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NFC_TABLE_MAX 8192

#define HS UTF8PROC_HANGUL_SBASE
#define HL UTF8PROC_HANGUL_LBASE
#define HV UTF8PROC_HANGUL_VBASE
#define HT UTF8PROC_HANGUL_TBASE

static int is_surrogate(int cp) { return cp >= 0xd800 && cp < 0xe000; }

static int ccc(int cp)
{
    return is_surrogate(cp) ? 0 : unsafe_get_property(cp)->combining_class;
}

static int enc_len(int cp)
{
    return cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
}

static void die(const char *msg, int a, int b)
{
    fprintf(stderr, "gen_nfc: %s (%04X %04X)\n", msg, a, b);
    exit(1);
}

/* Replays utf8proc's table lookup for (starter, second); 0 = not found. */
static int utf8proc_pair(int starter, int second)
{
    const utf8proc_property_t *p1 = unsafe_get_property(starter);
    const utf8proc_property_t *p2 = unsafe_get_property(second);
    int idx = p1->comb_index, len = p1->comb_length, off;
    if (idx >= 0x3FF || !p2->comb_issecond || len == 0)
        return 0;
    if (second > utf8proc_combinations_second[idx + len - 1])
        return 0;
    for (off = 0; off < len; off++) {
        int s = utf8proc_combinations_second[idx + off];
        if (second < s)
            return 0;
        if (second == s)
            return utf8proc_combinations_combined[idx + off];
    }
    return 0;
}

static int hangul_applies(int starter, int second)
{
    int l = starter - HL, v = second - HV, s = starter - HS, t = second - HT;
    if (l >= 0 && l < UTF8PROC_HANGUL_LCOUNT &&
        v >= 0 && v < UTF8PROC_HANGUL_VCOUNT)
        return 1;
    return s >= 0 && s < UTF8PROC_HANGUL_SCOUNT &&
           (s % UTF8PROC_HANGUL_TCOUNT) == 0 &&
           t > 0 && t < UTF8PROC_HANGUL_TCOUNT;
}

struct pair { int a, b, r; };
static struct pair pairs[4096];
static int npairs;

static void collect_pairs(void)
{
    int cp, off;
    for (cp = 0; cp < 0x110000; cp++) {
        const utf8proc_property_t *p;
        if (is_surrogate(cp))
            continue;
        p = unsafe_get_property(cp);
        if (p->comb_index >= 0x3FF)
            continue;
        for (off = 0; off < p->comb_length; off++) {
            int s = utf8proc_combinations_second[p->comb_index + off];
            int r = utf8proc_pair(cp, s);
            if (!r)
                continue;               /* unreachable row entry */
            if (hangul_applies(cp, s))
                die("pair shadowed by Hangul", cp, s);
            if (s < 0x300)
                die("second below U+0300 (breaks the fast path)", cp, s);
            if (enc_len(r) > enc_len(cp) + enc_len(s))
                die("composite longer than its parts", cp, s);
            if (npairs && pairs[npairs - 1].a == cp && pairs[npairs - 1].b >= s)
                die("row not strictly sorted", cp, s);
            if (npairs >= (int)(sizeof pairs / sizeof pairs[0]))
                die("too many pairs", cp, s);
            pairs[npairs].a = cp;
            pairs[npairs].b = s;
            pairs[npairs].r = r;
            npairs++;
        }
    }
}

static int is_bmp_pair(const struct pair *p)
{
    return p->a < 0x10000 && p->b < 0x10000 && p->r < 0x10000;
}

static int emit_table(void)
{
    int i, cp, n16 = 0, n32 = 0, nrng = 0, col;
    int rfirst[2048], rlen[2048], rval[2048];

    for (cp = 0; cp < 0x110000; ) {
        int c = ccc(cp), start = cp;
        if (!c) { cp++; continue; }
        if (cp < 0x300)
            die("non-zero ccc below U+0300 (breaks the fast path)", cp, c);
        while (cp < 0x110000 && ccc(cp) == c && cp - start < 255)
            cp++;
        if (nrng >= 2048)
            die("too many ccc ranges", start, c);
        rfirst[nrng] = start; rlen[nrng] = cp - start; rval[nrng] = c;
        nrng++;
    }
    for (i = 0; i < npairs; i++) {
        if (is_bmp_pair(&pairs[i])) n16++; else n32++;
    }

    printf(PROKBOX_BANNER "\n");
    printf("/* Generated by tools/gen_nfc.c from lib/utf8proc (Unicode %s,\n"
           " * utf8proc %s). Do not edit; regenerate with\n"
           " *   ./gen_nfc table > firmware/common/nfc_table.h\n"
           " * Pairs: every (starter, second) utf8proc's compose loop can find,\n"
           " * sorted by starter then second; Hangul is algorithmic (nfc.c).\n"
           " * Classes: ranges of non-zero canonical combining class,\n"
           " * first << 8 | length, with the class in nfc_ccc_val[]. */\n\n",
           utf8proc_unicode_version(), utf8proc_version());
    printf("#define NFC_PAIRS16 %d\n#define NFC_PAIRS32 %d\n"
           "#define NFC_CCC_RANGES %d\n\n", n16, n32, nrng);

    printf("static const uint16_t nfc_pairs16[NFC_PAIRS16][3] = {\n");
    for (i = 0, col = 0; i < npairs; i++) {
        if (!is_bmp_pair(&pairs[i])) continue;
        printf("%s{0x%04X,0x%04X,0x%04X},", col == 0 ? "    " : "",
               pairs[i].a, pairs[i].b, pairs[i].r);
        if (++col == 4) { printf("\n"); col = 0; }
    }
    printf("%s};\n\n", col ? "\n" : "");

    printf("static const uint32_t nfc_pairs32[NFC_PAIRS32][3] = {\n");
    for (i = 0, col = 0; i < npairs; i++) {
        if (is_bmp_pair(&pairs[i])) continue;
        printf("%s{0x%05X,0x%05X,0x%05X},", col == 0 ? "    " : "",
               pairs[i].a, pairs[i].b, pairs[i].r);
        if (++col == 3) { printf("\n"); col = 0; }
    }
    printf("%s};\n\n", col ? "\n" : "");

    /* BMP page filters (bit p = page p, U+pp00..U+ppFF): pages holding a
     * non-zero class, and pages holding a second or a Hangul V/T. A clear
     * bit lets nfc.c skip the searches; code points above the BMP always
     * take the search. */
    {
        unsigned char ccc_page[32] = { 0 }, sec_page[32] = { 0 };
        for (i = 0; i < nrng; i++) {
            int a = rfirst[i], b = rfirst[i] + rlen[i] - 1, pg;
            for (pg = a >> 8; pg <= b >> 8 && pg < 256; pg++)
                ccc_page[pg >> 3] |= 1 << (pg & 7);
        }
        for (i = 0; i < npairs; i++)
            if (pairs[i].b < 0x10000)
                sec_page[pairs[i].b >> 11] |= 1 << ((pairs[i].b >> 8) & 7);
        for (cp = HV; cp < HV + UTF8PROC_HANGUL_VCOUNT; cp++)
            sec_page[cp >> 11] |= 1 << ((cp >> 8) & 7);
        for (cp = HT + 1; cp < HT + UTF8PROC_HANGUL_TCOUNT; cp++)
            sec_page[cp >> 11] |= 1 << ((cp >> 8) & 7);
        printf("static const uint8_t nfc_ccc_page[32] = {\n");
        for (i = 0; i < 32; i++)
            printf("%s0x%02X,%s", i % 16 ? "" : "    ", ccc_page[i],
                   i % 16 == 15 ? "\n" : "");
        printf("};\n\nstatic const uint8_t nfc_second_page[32] = {\n");
        for (i = 0; i < 32; i++)
            printf("%s0x%02X,%s", i % 16 ? "" : "    ", sec_page[i],
                   i % 16 == 15 ? "\n" : "");
        printf("};\n\n");
    }

    printf("static const uint32_t nfc_ccc_rng[NFC_CCC_RANGES] = {\n");
    for (i = 0; i < nrng; i++)
        printf("%s0x%07X,%s", i % 6 ? "" : "    ",
               (unsigned)(rfirst[i] << 8 | rlen[i]), i % 6 == 5 ? "\n" : "");
    printf("%s};\n\n", nrng % 6 ? "\n" : "");

    printf("static const uint8_t nfc_ccc_val[NFC_CCC_RANGES] = {\n");
    for (i = 0; i < nrng; i++)
        printf("%s%3d,%s", i % 12 ? "" : "    ", rval[i],
               i % 12 == 11 ? "\n" : "");
    printf("%s};\n", nrng % 12 ? "\n" : "");

    {
        int b16 = n16 * 6, b32 = n32 * 12, brng = nrng * 5 + 64;
        fprintf(stderr, "gen_nfc: pairs %d BMP (%d B) + %d other (%d B), "
                "ccc %d ranges + page filters (%d B), total %d B of %d\n",
                n16, b16, n32, b32, nrng, brng, b16 + b32 + brng,
                NFC_TABLE_MAX);
        if (b16 + b32 + brng > NFC_TABLE_MAX)
            die("tables over budget", b16 + b32 + brng, NFC_TABLE_MAX);
    }
    return 0;
}

/* ---- corpus ---------------------------------------------------------- */

static unsigned long ncases;

static void put_utf8(int cp)
{
    if (cp < 0x80) printf("%02x", cp);
    else if (cp < 0x800) printf("%02x%02x", 0xC0 | cp >> 6, 0x80 | (cp & 63));
    else if (cp < 0x10000)
        printf("%02x%02x%02x", 0xE0 | cp >> 12, 0x80 | (cp >> 6 & 63),
               0x80 | (cp & 63));
    else
        printf("%02x%02x%02x%02x", 0xF0 | cp >> 18, 0x80 | (cp >> 12 & 63),
               0x80 | (cp >> 6 & 63), 0x80 | (cp & 63));
}

static void case_cps(char flag, const int *cps, int n)
{
    int i;
    printf("%c ", flag);
    for (i = 0; i < n; i++)
        put_utf8(cps[i]);
    printf("\n");
    ncases++;
}

#define CASE(...) do { int c_[] = { __VA_ARGS__ }; \
    case_cps('B', c_, (int)(sizeof c_ / sizeof c_[0])); } while (0)

static void case_bytes(const char *hex)
{
    printf("B %s\n", hex);
    ncases++;
}

/* One representative mark per class (the first and the last code point of
 * each class, so two marks per class where the class has more than one). */
static int cls_first[256], cls_last[256];

static void collect_classes(void)
{
    int cp;
    for (cp = 0; cp < 0x110000; cp++) {
        int c = ccc(cp);
        if (!c) continue;
        if (!cls_first[c]) cls_first[c] = cp;
        cls_last[c] = cp;
    }
}

static unsigned long rng_state = 0x9e3779b9UL;
static unsigned rnd(unsigned n)
{
    rng_state = rng_state * 1103515245UL + 12345UL;
    return (unsigned)((rng_state >> 8) & 0xFFFFFF) % n;
}

static int emit_cases(void)
{
    int i, c, l, v, t, cp;
    unsigned long base;

    /* 1. every pair: bare, then with one intervening mark of every class
     *    (blocked when that class >= the second's, unblocked otherwise),
     *    then with two marks of the same class, and followed by a starter */
    base = ncases;
    for (i = 0; i < npairs; i++) {
        int a = pairs[i].a, b = pairs[i].b;
        CASE(a, b);
        CASE(0x41, a, b, 0x42);
        CASE(a, b, b);
        for (c = 1; c < 256; c++) {
            if (!cls_first[c]) continue;
            CASE(a, cls_first[c], b);
            if (cls_last[c] != cls_first[c])
                CASE(a, cls_last[c], b);
            CASE(a, b, cls_first[c]);
        }
    }
    fprintf(stderr, "gen_nfc: cases pairs x classes %lu\n", ncases - base);

    /* 2. chains: a composite followed by every second it composes with */
    base = ncases;
    for (i = 0; i < npairs; i++) {
        int r = pairs[i].r, j;
        for (j = 0; j < npairs; j++)
            if (pairs[j].a == r)
                CASE(pairs[i].a, pairs[i].b, pairs[j].b);
    }
    fprintf(stderr, "gen_nfc: cases chains %lu\n", ncases - base);

    /* 3. Hangul: every L+V, L+V+T (T index 0..27, i.e. all 11,172 syllables
     *    in decomposed form), every LV+T, and non-composing neighbours */
    base = ncases;
    for (l = 0; l < UTF8PROC_HANGUL_LCOUNT; l++)
        for (v = 0; v < UTF8PROC_HANGUL_VCOUNT; v++) {
            int lv = HS + (l * UTF8PROC_HANGUL_VCOUNT + v) *
                     UTF8PROC_HANGUL_TCOUNT;
            CASE(HL + l, HV + v);
            for (t = 1; t < UTF8PROC_HANGUL_TCOUNT; t++) {
                CASE(HL + l, HV + v, HT + t);
                CASE(lv, HT + t);
            }
            CASE(lv, HT);                       /* T index 0: no compose */
            CASE(lv + 1, HT + 1);               /* LVT + T */
            CASE(HL + l, 0x0301, HV + v);       /* blocked by a mark */
            CASE(lv, 0x0301, HT + 1);
        }
    for (cp = 0x1100; cp < 0x1200; cp++) {      /* all jamo neighbours */
        CASE(cp, cp);
        CASE(HL, cp);
        CASE(HS, cp);
        CASE(cp, HV);
        CASE(cp, HT + 1);
    }
    CASE(0x115A, HV); CASE(0x115F, HV); CASE(HL + 19, HV);
    CASE(HL, HV + 21); CASE(HS + 11171, HT + 1); CASE(HS - 1, HT + 1);
    CASE(HS + 11172, HT + 1); CASE(HS, HT + 28);
    fprintf(stderr, "gen_nfc: cases Hangul %lu\n", ncases - base);

    /* 4. every canonical decomposition: the composed character itself and
     *    its full canonical decomposition (covers exclusions such as
     *    U+0958 <- U+0915 U+093C, singletons such as U+212B, U+2126,
     *    U+0340, ccc-0 seconds, non-BMP pairs, multi-mark NFD) */
    base = ncases;
    for (cp = 0; cp < 0x110000; cp++) {
        const utf8proc_property_t *p;
        utf8proc_int32_t buf[32];
        utf8proc_int32_t lb = 0;
        utf8proc_ssize_t n;
        int k, d[34];
        if (is_surrogate(cp)) continue;
        p = unsafe_get_property(cp);
        if (p->decomp_seqindex == UINT16_MAX || p->decomp_type)
            continue;
        n = utf8proc_decompose_char(cp, buf, 32, UTF8PROC_DECOMPOSE, &lb);
        if (n < 1 || n > 32)
            die("decompose failed", cp, (int)n);
        CASE(cp);
        for (k = 0; k < n; k++) d[k] = buf[k];
        case_cps('B', d, (int)n);
        d[n] = 0x0301;                          /* plus a trailing mark */
        case_cps('B', d, (int)n + 1);
        d[0] = 0x61; for (k = 0; k < n; k++) d[k + 1] = buf[k];
        case_cps('B', d, (int)n + 1);           /* after another starter */
    }
    fprintf(stderr, "gen_nfc: cases decompositions %lu\n", ncases - base);

    /* 5. named edges from the brief */
    base = ncases;
    CASE(0x0B47, 0x0B3E); CASE(0x0B47, 0x0301, 0x0B3E);
    CASE(0x0DD9, 0x0DCF); CASE(0x0DD9, 0x0DCA, 0x0DCF);
    CASE(0x0915, 0x093C); CASE(0x212B); CASE(0x2126); CASE(0x0340);
    CASE(0x0041, 0x0340); CASE(0x11099, 0x110BA); CASE(0x11099, 0x0301,
    0x110BA); CASE(0x110BA); CASE(0x0301); CASE(0x0301, 0x0041, 0x0301);
    CASE(0x0041, 0x0323, 0x0302); CASE(0x0041, 0x0302, 0x0323);
    CASE(0x0041, 0x0316, 0x0301); CASE(0x0041, 0x0301, 0x0316, 0x0301);
    CASE(0x1F600); CASE(0x10FFFF, 0x0301); CASE(0xFFFD, 0x0301);
    fprintf(stderr, "gen_nfc: cases named edges %lu\n", ncases - base);

    /* 6. invalid UTF-8: truncated, overlong, surrogates, > U+10FFFF, stray
     *    continuation, bad lead bytes, each alone and after valid text */
    base = ncases;
    {
        static const char *bad[] = {
            "c3", "e2", "e282", "f0", "f09f", "f09f98", "c328", "e228a1",
            "e282 28", "f0289f98", "c080", "c1bf", "e080af", "e09f80",
            "f08080af", "f08f8080", "eda080", "edbfbf", "edb080", "f4908080",
            "f5808080", "f8888080", "fc8080808080", "fe", "ff", "80", "bf",
            "41cc", "41cc80cc", "80cc81", "41cc81ff", "e1848541e185", NULL };
        static const char *ok_prefix[] = { "", "41cc8a", "e18480e185a1", NULL };
        int b, pz;
        for (b = 0; bad[b]; b++)
            for (pz = 0; ok_prefix[pz]; pz++) {
                char hex[64]; const char *s; char *o = hex;
                o += sprintf(o, "%s", ok_prefix[pz]);
                for (s = bad[b]; *s; s++) if (*s != ' ') *o++ = *s;
                *o = 0;
                case_bytes(hex);
            }
    }
    fprintf(stderr, "gen_nfc: cases invalid %lu\n", ncases - base);

    /* 7. length limit: 2,046 and 2,047 code points must match (with
     *    compositions in them); 2,048 and more must stay unchanged (N) */
    base = ncases;
    {
        static int buf[4000];
        static const int lens[] = { 2046, 2047, 2048, 2049, 3000 };
        int k, n, pat;
        for (pat = 0; pat < 3; pat++)
            for (k = 0; k < 5; k++) {
                n = lens[k];
                for (i = 0; i < n; i++) {
                    if (pat == 0) buf[i] = (i & 1) ? 0x030A : 0x41;
                    else if (pat == 1) buf[i] = (i % 3 == 0) ? HL :
                                                (i % 3 == 1) ? HV : HT + 1;
                    else buf[i] = (i == n - 1) ? 0x0301 : 0x65;
                }
                case_cps(n < 2048 ? 'B' : 'N', buf, n);
            }
    }
    fprintf(stderr, "gen_nfc: cases length %lu\n", ncases - base);

    /* 8. random sequences from a pool of starters, every second, a mark of
     *    every class, jamo and ASCII (deterministic LCG) */
    base = ncases;
    {
        static int pool[2048];
        int np = 0, k, n;
        for (i = 0; i < npairs; i++) {
            pool[np++] = pairs[i].a;
            if (i == 0 || pairs[i].b != pairs[i - 1].b) pool[np++] = pairs[i].b;
        }
        for (c = 1; c < 256; c++) if (cls_first[c]) pool[np++] = cls_first[c];
        pool[np++] = HL; pool[np++] = HV + 3; pool[np++] = HT + 5;
        pool[np++] = HS; pool[np++] = 0x41; pool[np++] = 0x20;
        for (k = 0; k < 40000; k++) {
            int buf[24];
            n = 1 + (int)rnd(23);
            for (i = 0; i < n; i++) buf[i] = pool[rnd((unsigned)np)];
            case_cps('B', buf, n);
        }
    }
    fprintf(stderr, "gen_nfc: cases random %lu\n", ncases - base);

    /* 9. mixed-script smoke set: Latin, Greek, Cyrillic in NFD and NFC,
     *    CJK, emoji with modifiers and ZWJ */
    base = ncases;
    CASE(0x42, 0x65, 0x301, 0x6C, 0x61, 0x20, 0x46, 0x6C, 0x65, 0x301, 0x63,
         0x6B);
    CASE(0x42, 0xE9, 0x6C, 0x61, 0x20, 0x46, 0x6C, 0xE9, 0x63, 0x6B);
    CASE(0x3B1, 0x313, 0x301, 0x345, 0x3BB, 0x3C6, 0x3B1);
    CASE(0x1F84, 0x3BB, 0x3C6, 0x3B1);
    CASE(0x418, 0x306, 0x43E, 0x301, 0x439);
    CASE(0x6771, 0x4EAC, 0x20, 0x30AB, 0x3099, 0x30CF, 0x309A);
    CASE(0x1F44D, 0x1F3FD, 0x20, 0x1F468, 0x200D, 0x1F469, 0x200D, 0x1F467);
    CASE(0x53, 0x69, 0x67, 0x75, 0x72, 0x20, 0x52, 0x6F, 0x301, 0x73);
    CASE(0x41);
    fprintf(stderr, "gen_nfc: cases smoke %lu\n", ncases - base);

    fprintf(stderr, "gen_nfc: cases total %lu\n", ncases);
    return 0;
}

int main(int argc, char **argv)
{
    collect_pairs();
    collect_classes();
    if (argc == 2 && !strcmp(argv[1], "table"))
        return emit_table();
    if (argc == 2 && !strcmp(argv[1], "cases"))
        return emit_cases();
    fprintf(stderr, "usage: %s table|cases\n", argv[0]);
    return 2;
}
