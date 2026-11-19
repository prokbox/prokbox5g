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

/* #352: the per-family EQ preset table parser (apps/ipodui/eqtable_core.c)
 *
 *  1. family:  44.1/48 kHz (and 0, unknown) are base, 88.2/96 kHz hi.
 *  2. good:    the test fixture tests/ui/fixtures/eqtable-hi.txt (the sim
 *              script's hi table) parses, with comments, blank lines and
 *              CRLF; every base preset written with eqlab_format() into a
 *              table round-trips to the same bands (the slot line format
 *              holds the compiled-in table exactly).
 *  3. bad:     a malformed preset line (gain, Hz, width, junk, too long,
 *              a NUL) is EQT_LINE with its line number.
 *  4. short:   21 lines, or a file cut mid-line, is refused; 23 is long.
 *  5. header:  version 2, family base asked as hi, n=21, garbage, empty
 *              and oversize files are refused, each with its own code.
 *
 * argv[1]: the fixture's path. */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ipodui/eqtable_core.h"
#include "ipodui/eqlab_core.h"
#include "ipodui/eq_hw.h"

static int fails, checks;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; \
    printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static struct dac_eq_band tab[EQTABLE_PRESETS][DAC_EQ_BANDS];

static int parse(const char *s, int family, int *line)
{
    return eqtable_parse(s, (int)strlen(s), family, tab, line);
}

/* the 22 base presets as a table text, header hdr */
static void base_text(char *buf, size_t len, const char *hdr, int lines,
                      const char *eol)
{
    size_t o = (size_t)snprintf(buf, len, "%s%s", hdr, eol);
    int p;

    for (p = 0; p < lines; p++)
    {
        char l[EQLAB_TEXT_LEN];
        struct dac_eq_band b[DAC_EQ_BANDS];
        int i;

        for (i = 0; i < DAC_EQ_BANDS; i++)
        {
            const struct eq_hw_band *h =
                &eq_hw_presets[p % EQ_HW_PRESETS].band[i];
            b[i].gain = h->gain;
            b[i].freq = h->freq;
            b[i].wide = eqlab_has_width(i) ? h->wide : 0;
        }
        eqlab_format(b, l, sizeof(l));
        o += (size_t)snprintf(buf + o, len - o, "%s%s", l, eol);
    }
}

static bool same_as_base(void)
{
    int p, i;

    for (p = 0; p < EQTABLE_PRESETS; p++)
        for (i = 0; i < DAC_EQ_BANDS; i++)
        {
            const struct eq_hw_band *h = &eq_hw_presets[p].band[i];

            if (tab[p][i].gain != h->gain || tab[p][i].freq != h->freq
                || tab[p][i].wide != (eqlab_has_width(i) ? h->wide : 0))
                return false;
        }
    return true;
}

static void test_family(void)
{
    CHECK(eqtable_family_of(0) == EQTABLE_BASE, "0 Hz is base");
    CHECK(eqtable_family_of(44100) == EQTABLE_BASE, "44.1 kHz is base");
    CHECK(eqtable_family_of(48000) == EQTABLE_BASE, "48 kHz is base");
    CHECK(eqtable_family_of(88200) == EQTABLE_HI, "88.2 kHz is hi");
    CHECK(eqtable_family_of(96000) == EQTABLE_HI, "96 kHz is hi");
    CHECK(!strcmp(eqtable_family_name(EQTABLE_HI), "hi"), "name hi");
    CHECK(!strcmp(eqtable_family_name(EQTABLE_BASE), "base"), "name base");
}

static void test_fixture(const char *path)
{
    static char buf[EQTABLE_FILE_MAX + 1];
    FILE *f = fopen(path, "rb");
    int n, line, err;

    CHECK(f != NULL, "fixture %s opens", path);
    if (!f)
        return;
    n = (int)fread(buf, 1, sizeof(buf), f);
    fclose(f);
    CHECK(n > 0 && n <= EQTABLE_FILE_MAX, "fixture size %d", n);
    memset(tab, 0x55, sizeof(tab));
    err = eqtable_parse(buf, n, EQTABLE_HI, tab, &line);
    CHECK(err == EQT_OK, "fixture parses (%s at line %d)",
          eqtable_err_name(err), line);
    /* its first preset: Acoustic, every band one code down */
    CHECK(tab[0][0].gain == 5 && tab[0][0].freq == 2 && tab[0][3].gain == 7
          && tab[0][3].freq == 2 && tab[0][3].wide == 1,
          "fixture preset 1 = +5@135 .. +7@3200w");
    CHECK(!same_as_base(), "the fixture differs from the base table");
    /* as a base file it is the wrong family */
    err = eqtable_parse(buf, n, EQTABLE_BASE, tab, &line);
    CHECK(err == EQT_FAMILY, "fixture asked as base: %s",
          eqtable_err_name(err));
}

static void test_good(void)
{
    static char buf[EQTABLE_FILE_MAX * 2];
    int err, line;

    base_text(buf, sizeof(buf), "eqtable 1 family=hi n=22", 22, "\n");
    memset(tab, 0x55, sizeof(tab));
    err = parse(buf, EQTABLE_HI, &line);
    CHECK(err == EQT_OK, "base presets as text (%s at %d)",
          eqtable_err_name(err), line);
    CHECK(same_as_base(), "base presets round-trip through the line format");
    CHECK(strlen(buf) < 1500, "22 full lines are %u B, well under the cap",
          (unsigned)strlen(buf));

    base_text(buf, sizeof(buf), "# a comment first\r\n\r\n"
              "eqtable 1 family=hi n=22", 22, "\r\n");
    err = parse(buf, EQTABLE_HI, &line);
    CHECK(err == EQT_OK && same_as_base(), "CRLF, comment and blank lines "
          "(%s at %d)", eqtable_err_name(err), line);

    /* no newline after the last line */
    base_text(buf, sizeof(buf), "eqtable 1 family=base n=22", 22, "\n");
    buf[strlen(buf) - 1] = '\0';
    err = parse(buf, EQTABLE_BASE, &line);
    CHECK(err == EQT_OK, "no final newline (%s at %d)",
          eqtable_err_name(err), line);
}

static void test_bad_lines(void)
{
    static const struct { const char *line; const char *why; } bad[] = {
        { "eq1 +13@80 +0@300 +0@850 +0@2400 +0@6900", "gain over 12" },
        { "eq1 +0@81 +0@300 +0@850 +0@2400 +0@6900", "Hz not a code" },
        { "eq1 +0@80w +0@300 +0@850 +0@2400 +0@6900", "wide shelf" },
        { "eq1 +0@80 +0@300 +0@850 +0@2400", "four bands" },
        { "eq1 +0@80 +0@300 +0@850 +0@2400 +0@6900 x", "trailing junk" },
        { "eq2 +0@80 +0@300 +0@850 +0@2400 +0@6900", "other line tag" },
        { "eq1 +0@80 +0@300 +0@850 +0@2400 +0@6900                        "
          "                      ", "longer than a slot line" },
    };
    static char buf[EQTABLE_FILE_MAX * 2];
    unsigned k;

    for (k = 0; k < sizeof(bad) / sizeof(bad[0]); k++)
    {
        int err, line;
        char *p;

        base_text(buf, sizeof(buf), "eqtable 1 family=hi n=22", 22, "\n");
        /* replace preset 5 (file line 6) */
        p = buf;
        for (int i = 0; i < 5; i++)
            p = strchr(p, '\n') + 1;
        {
            char rest[EQTABLE_FILE_MAX * 2];

            strcpy(rest, strchr(p, '\n'));
            sprintf(p, "%s%s", bad[k].line, rest);
        }
        err = parse(buf, EQTABLE_HI, &line);
        CHECK(err == EQT_LINE && line == 6, "%s: %s at line %d", bad[k].why,
              eqtable_err_name(err), line);
    }

    /* a NUL inside a line, the valid start before it */
    {
        int err, line, n;

        base_text(buf, sizeof(buf), "eqtable 1 family=hi n=22", 22, "\n");
        n = (int)strlen(buf);
        *(strchr(strchr(buf, '\n') + 1, '\n') - 3) = '\0';
        err = eqtable_parse(buf, n, EQTABLE_HI, tab, &line);
        CHECK(err == EQT_LINE && line == 2, "NUL in a line: %s at %d",
              eqtable_err_name(err), line);
    }
}

static void test_short_long(void)
{
    static char buf[EQTABLE_FILE_MAX * 2];
    int err, line, n;

    base_text(buf, sizeof(buf), "eqtable 1 family=hi n=22", 21, "\n");
    err = parse(buf, EQTABLE_HI, &line);
    CHECK(err == EQT_SHORT, "21 lines: %s", eqtable_err_name(err));

    base_text(buf, sizeof(buf), "eqtable 1 family=hi n=22", 23, "\n");
    err = parse(buf, EQTABLE_HI, &line);
    CHECK(err == EQT_LONG && line == 24, "23 lines: %s at %d",
          eqtable_err_name(err), line);

    /* cut mid-way through the last line */
    base_text(buf, sizeof(buf), "eqtable 1 family=hi n=22", 22, "\n");
    n = (int)strlen(buf) - 12;
    err = eqtable_parse(buf, n, EQTABLE_HI, tab, &line);
    CHECK(err == EQT_LINE && line == 23, "cut mid-line: %s at %d",
          eqtable_err_name(err), line);

    /* header only */
    err = parse("eqtable 1 family=hi n=22\n", EQTABLE_HI, &line);
    CHECK(err == EQT_SHORT, "header only: %s", eqtable_err_name(err));
}

static void test_header(void)
{
    static const struct { const char *hdr; int fam; int err; } h[] = {
        { "eqtable 2 family=hi n=22", EQTABLE_HI, EQT_VERSION },
        { "eqtable 0 family=hi n=22", EQTABLE_HI, EQT_VERSION },
        { "eqtable 1 family=base n=22", EQTABLE_HI, EQT_FAMILY },
        { "eqtable 1 family=hi n=22", EQTABLE_BASE, EQT_FAMILY },
        { "eqtable 1 family=hi n=21", EQTABLE_HI, EQT_COUNT },
        { "eqtable 1 family=hi n=23", EQTABLE_HI, EQT_COUNT },
        { "eqtable 1 family=low n=22", EQTABLE_HI, EQT_HEADER },
        { "eqtable 1 family=hi", EQTABLE_HI, EQT_HEADER },
        { "eqtable 1 family=hi n=22 x", EQTABLE_HI, EQT_HEADER },
        { "eqtable x family=hi n=22", EQTABLE_HI, EQT_HEADER },
        { "EQTABLE 1 family=hi n=22", EQTABLE_HI, EQT_HEADER },
        { "eq1 +0@80 +0@300 +0@850 +0@2400 +0@6900", EQTABLE_HI, EQT_HEADER },
    };
    static char buf[EQTABLE_FILE_MAX * 2];
    unsigned k;
    int err, line;

    for (k = 0; k < sizeof(h) / sizeof(h[0]); k++)
    {
        base_text(buf, sizeof(buf), h[k].hdr, 22, "\n");
        err = parse(buf, h[k].fam, &line);
        CHECK(err == h[k].err && line == 1, "header \"%s\" as %s: %s "
              "(want %s) at %d", h[k].hdr, eqtable_family_name(h[k].fam),
              eqtable_err_name(err), eqtable_err_name(h[k].err), line);
    }
    err = parse("", EQTABLE_HI, &line);
    CHECK(err == EQT_SIZE, "empty: %s", eqtable_err_name(err));
    err = parse("# only a comment\n\n", EQTABLE_HI, &line);
    CHECK(err == EQT_HEADER, "no header: %s", eqtable_err_name(err));
    memset(buf, '#', EQTABLE_FILE_MAX + 1);
    err = eqtable_parse(buf, EQTABLE_FILE_MAX + 1, EQTABLE_HI, tab, &line);
    CHECK(err == EQT_SIZE, "oversize: %s", eqtable_err_name(err));
}

int main(int argc, char **argv)
{
    test_family();
    test_fixture(argc > 1 ? argv[1] : "tests/ui/fixtures/eqtable-hi.txt");
    test_good();
    test_bad_lines();
    test_short_long();
    test_header();
    if (fails)
    {
        printf("FAIL: eqtable %d of %d checks\n", fails, checks);
        return 1;
    }
    printf("PASS: eqtable %d checks\n", checks);
    return 0;
}
