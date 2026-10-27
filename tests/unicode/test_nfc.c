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

/* NFC golden (docs/phase9-brief.md 3.3.2, Rev 1 H4): every case from
 * `gen_nfc cases` goes through the stock path (unicode.c's utf8proc call,
 * reproduced line for line below against the in-tree lib/utf8proc) and
 * through firmware/common/nfc.c; any byte difference fails. "N" cases
 * (>= 2048 code points) run nfc.c only and must come back unchanged.
 * Also checks that nfc.c never writes past the string's terminator. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "utf8proc.h"
#include "nfc.h"

/* unicode.c, HAVE_UTF8_NORMALIZE without IPODUI_TRIM_UTF8PROC */
static utf8proc_int32_t normbuf[2048];
static void ref_normalize(char *string)
{
    utf8proc_ssize_t result, orig;
    if (!string || !*string)
        return;
    orig = strlen(string);
    result = utf8proc_decompose((const utf8proc_uint8_t *)string, 0, normbuf,
                                sizeof(normbuf)/4 - 1, UTF8PROC_NULLTERM);
    if (result > 0) {
        result = utf8proc_reencode(normbuf, result, UTF8PROC_NULLTERM |
                                   UTF8PROC_COMPOSE | UTF8PROC_STABLE);
        if (result > 0 && result <= orig && strcmp((char *)normbuf, string))
            strcpy(string, (char *)normbuf);
    }
}

static int hexval(int c)
{
    return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10;
}

static void dump(const char *tag, const char *s)
{
    fprintf(stderr, "  %s:", tag);
    for (; *s; s++)
        fprintf(stderr, " %02x", (unsigned char)*s);
    fprintf(stderr, "\n");
}

#define GUARD 16

int main(int argc, char **argv)
{
    static char line[40000], in[20000], a[20000 + GUARD], b[20000 + GUARD];
    unsigned long n_b = 0, n_n = 0, changed = 0, fails = 0, lineno = 0;
    FILE *f;

    if (argc != 2 || !(f = fopen(argv[1], "r"))) {
        fprintf(stderr, "usage: %s nfc_cases.txt\n", argv[0]);
        return 2;
    }
    while (fgets(line, sizeof line, f)) {
        size_t len = 0, i;
        char *h = line + 2;
        lineno++;
        if ((line[0] != 'B' && line[0] != 'N') || line[1] != ' ') {
            fprintf(stderr, "bad line %lu\n", lineno);
            return 2;
        }
        while (h[0] && h[0] != '\n' && h[1])
            in[len++] = (char)(hexval(h[0]) << 4 | hexval(h[1])), h += 2;
        in[len] = 0;
        if (strlen(in) != len) {
            fprintf(stderr, "line %lu: NUL inside a case\n", lineno);
            return 2;
        }

        memset(b, 0x5a, len + GUARD);
        memcpy(b, in, len + 1);
        nfc_normalize(b);
        for (i = len + 1; i < len + GUARD; i++)
            if (b[i] != 0x5a) {
                fprintf(stderr, "FAIL line %lu: nfc.c wrote past the "
                        "terminator\n", lineno);
                fails++;
                break;
            }

        if (line[0] == 'N') {
            n_n++;
            if (strcmp(b, in)) {
                fprintf(stderr, "FAIL line %lu: >= 2048 code points must "
                        "be unchanged\n", lineno);
                fails++;
            }
            continue;
        }
        n_b++;
        memcpy(a, in, len + 1);
        ref_normalize(a);
        if (strcmp(a, in))
            changed++;
        if (strcmp(a, b)) {
            if (fails < 20) {
                fprintf(stderr, "FAIL line %lu: differs from utf8proc\n",
                        lineno);
                if (len < 200) {
                    dump("in ", in);
                    dump("ref", a);
                    dump("nfc", b);
                }
            }
            fails++;
        }
    }
    fclose(f);
    printf("nfc golden: %lu byte-identity cases (%lu changed by "
           "composition), %lu unchanged-only cases, %lu failures\n",
           n_b, changed, n_n, fails);
    return fails ? 1 : 0;
}
