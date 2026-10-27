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

/* Compact canonical composition for utf8_normalize() (Phase 9, brief 3.3.2).
 * Contract: firmware/include/nfc.h. Byte-identical to unicode.c's utf8proc
 * call (decompose with UTF8PROC_NULLTERM, then reencode with COMPOSE|STABLE)
 * on every case tests/unicode/run.sh generates. No static state: callers on
 * different threads each work in their own string. */
#include <stdint.h>
#include <string.h>
#include "nfc.h"
#include "nfc_table.h"

_Static_assert(sizeof(nfc_pairs16) + sizeof(nfc_pairs32) +
               sizeof(nfc_ccc_rng) + sizeof(nfc_ccc_val) +
               sizeof(nfc_ccc_page) + sizeof(nfc_second_page) <= 8192,
               "NFC tables over their 8 KB budget (brief 3.3.2)");

#define NFC_MAX_CPS   2048      /* utf8proc was given 2047 slots */
#define NFC_FIRST_CCC 0x300     /* gen_nfc asserts: no class or second below */

#define H_SBASE 0xAC00
#define H_LBASE 0x1100
#define H_VBASE 0x1161
#define H_TBASE 0x11A7
#define H_LCOUNT 19
#define H_VCOUNT 21
#define H_TCOUNT 28
#define H_SCOUNT 11172

/* BMP page filter: 0 = no code point of this page is in the set */
#define PAGE_HIT(map, cp) \
    ((cp) >= 0x10000 || ((map)[(cp) >> 11] >> (((cp) >> 8) & 7) & 1))

unsigned char nfc_ccc(unsigned long cp)
{
    int lo = 0, hi = NFC_CCC_RANGES - 1;
    if (cp < NFC_FIRST_CCC || !PAGE_HIT(nfc_ccc_page, cp))
        return 0;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        unsigned long first = nfc_ccc_rng[mid] >> 8;
        if (cp < first)
            hi = mid - 1;
        else if (cp >= first + (nfc_ccc_rng[mid] & 0xff))
            lo = mid + 1;
        else
            return nfc_ccc_val[mid];
    }
    return 0;
}

unsigned long nfc_compose(unsigned long starter, unsigned long second)
{
    /* Hangul first, in utf8proc's order: L+V, then LV+T */
    unsigned long l = starter - H_LBASE, v = second - H_VBASE;
    unsigned long s = starter - H_SBASE, t = second - H_TBASE;
    if (l < H_LCOUNT && v < H_VCOUNT)
        return H_SBASE + (l * H_VCOUNT + v) * H_TCOUNT;
    if (s < H_SCOUNT && (s % H_TCOUNT) == 0 && t - 1 < H_TCOUNT - 1)
        return starter + t;

    if (second < NFC_FIRST_CCC)
        return 0;
    if (starter < 0x10000 && second < 0x10000) {
        uint32_t key = (uint32_t)(starter << 16 | second);
        int lo = 0, hi = NFC_PAIRS16 - 1;
        while (lo <= hi) {
            int mid = (lo + hi) >> 1;
            uint32_t k = (uint32_t)nfc_pairs16[mid][0] << 16 |
                         nfc_pairs16[mid][1];
            if (key < k)
                hi = mid - 1;
            else if (key > k)
                lo = mid + 1;
            else
                return nfc_pairs16[mid][2];
        }
    }
    /* the few pairs with a supplementary starter, second or result */
    for (int i = 0; i < NFC_PAIRS32; i++) {
        if (nfc_pairs32[i][0] == starter && nfc_pairs32[i][1] == second)
            return nfc_pairs32[i][2];
    }
    return 0;
}

/* Strict UTF-8 decode, the same acceptance as utf8proc_iterate(): leads
 * C2..F4, real continuation bytes, no overlongs, no surrogates, nothing above
 * U+10FFFF. Never reads past the terminating NUL. Returns the length, or 0
 * for an invalid sequence. */
static int decode(const unsigned char *p, unsigned long *cp)
{
    unsigned c = p[0];
    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    if (c - 0xc2 > 0xf4 - 0xc2 || (p[1] & 0xc0) != 0x80)
        return 0;
    if (c < 0xe0) {
        *cp = (c & 0x1f) << 6 | (p[1] & 0x3f);
        return 2;
    }
    if ((p[2] & 0xc0) != 0x80)
        return 0;
    if (c < 0xf0) {
        if ((c == 0xe0 && p[1] < 0xa0) || (c == 0xed && p[1] > 0x9f))
            return 0;
        *cp = (c & 0x0f) << 12 | (p[1] & 0x3f) << 6 | (p[2] & 0x3f);
        return 3;
    }
    if ((p[3] & 0xc0) != 0x80 || (c == 0xf0 && p[1] < 0x90) ||
        (c == 0xf4 && p[1] > 0x8f))
        return 0;
    *cp = (unsigned long)(c & 0x07) << 18 | (p[1] & 0x3f) << 12 |
          (p[2] & 0x3f) << 6 | (p[3] & 0x3f);
    return 4;
}

static int enc_len(unsigned long cp)
{
    return cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
}

static void encode(unsigned long cp, unsigned char *d)
{
    if (cp < 0x80) {
        d[0] = cp;
    } else if (cp < 0x800) {
        d[0] = 0xc0 | cp >> 6;
        d[1] = 0x80 | (cp & 0x3f);
    } else if (cp < 0x10000) {
        d[0] = 0xe0 | cp >> 12;
        d[1] = 0x80 | (cp >> 6 & 0x3f);
        d[2] = 0x80 | (cp & 0x3f);
    } else {
        d[0] = 0xf0 | cp >> 18;
        d[1] = 0x80 | (cp >> 12 & 0x3f);
        d[2] = 0x80 | (cp >> 6 & 0x3f);
        d[3] = 0x80 | (cp & 0x3f);
    }
}

void nfc_normalize(char *string)
{
    unsigned char *r, *w, *st = NULL;
    unsigned long cp, st_cp = 0;
    int n = 0, len, st_len = 0, max_ccc = -1;
    int maybe = 0;

    if (!string)
        return;

    /* Pass 1: validate and count. Text in which no code point can be a
     * second (ASCII, Latin-1, CJK, ...: page filter) can't compose: done. */
    for (r = (unsigned char *)string; *r; r += len, n++) {
        if (*r < 0x80) {
            len = 1;
            continue;
        }
        len = decode(r, &cp);
        if (!len)
            return;                     /* invalid UTF-8: unchanged */
        if (PAGE_HIT(nfc_second_page, cp))
            maybe = 1;
    }
    if (!maybe || n >= NFC_MAX_CPS)
        return;

    /* Pass 2: utf8proc's compose loop, in place. The write pointer never
     * passes the read pointer: a composite is never longer than its two
     * parts (gen_nfc asserts it), so a starter that grows (A + U+030A ->
     * U+00C5) only shifts the marks written after it into consumed bytes. */
    for (r = w = (unsigned char *)string; *r; ) {
        int ccc;
        len = decode(r, &cp);
        r += len;
        ccc = nfc_ccc(cp);
        if (st && ccc > max_ccc && PAGE_HIT(nfc_second_page, cp)) {
            unsigned long c = nfc_compose(st_cp, cp);
            if (c) {
                int c_len = enc_len(c);
                if (c_len != st_len) {
                    memmove(st + c_len, st + st_len, w - (st + st_len));
                    w += c_len - st_len;
                    st_len = c_len;
                }
                encode(c, st);
                st_cp = c;
                continue;               /* the second is consumed */
            }
        }
        if (w != r - len)
            memmove(w, r - len, len);
        if (ccc) {
            if (ccc > max_ccc)
                max_ccc = ccc;
        } else {
            st = w;
            st_len = len;
            st_cp = cp;
            max_ccc = -1;
        }
        w += len;
    }
    *w = '\0';
}
