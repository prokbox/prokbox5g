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
 * Host test of the diagnostics report's scrubber, tail and builder
 * (apps/ipodui/diag_core.c): no path or title survives, system lines do.
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
#include "diag_core.h"

static int fails, checks;

#define CHECK(c) do { checks++; if (!(c)) { \
    printf("  line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static char out[256];

static const char *scrub(const char *in)
{
    diag_scrub_line(in, strlen(in), out, sizeof(out), NULL, 0, NULL);
    return out;
}

static const char *scrub_deny(const char *in, const char *const *deny, int n)
{
    diag_scrub_line(in, strlen(in), out, sizeof(out), deny, n, NULL);
    return out;
}

#define SAME(s)        CHECK(!strcmp(scrub(s), s))
#define GIVES(s, want) do { const char *g_ = scrub(s); \
    if (strcmp(g_, want)) printf("  got  '%s'\n  want '%s'\n", g_, want); \
    CHECK(!strcmp(g_, want)); } while (0)

/* synthetic names: spaces, brackets, punctuation, accents, quotes */
static const char *const titles[] = {
    "Glass Harbor",
    "Velvet Lanterns (Live) [2019]",
    "Quorn & Tallow: Partway",
    "Lowtide Drifters, Vol. 3",
    "Zephyrine \xc3\x9c" "berklang",
    "Marrowind's Orchard",
    "Brisket-Tempo Remixes",
    "Ophelian Nocturnes - Disc 2",
};

/* every word of 4+ letters of every title must be gone */
static void no_word_survives(const char *line, const char *title)
{
    char w[64];
    const char *t = title;

    while (*t)
    {
        size_t n = 0;

        while (*t && (*t == ' ' || *t == '(' || *t == '[' || *t == ','))
            t++;
        while (t[n] && t[n] != ' ' && t[n] != ')' && t[n] != ']'
               && t[n] != ',' && n < sizeof(w) - 1)
            n++;
        memcpy(w, t, n);
        w[n] = '\0';
        if (n >= 4 && strstr(line, w))
        {
            printf("  survived '%s' in '%s'\n", w, line);
            fails++;
        }
        checks++;
        t += n ? n : 1;
    }
}

int main(void)
{
    char in[200];
    unsigned hits;
    size_t k;

    /* system lines pass untouched */
    SAME("123 maint job=art ok=1 ms=40 play_during=0 audio=1234");
    SAME("5 usb cmdstat op:fail/stall/rst 0/0/0");
    SAME("7 art stack 40% of 2048 at drift");
    SAME("8 boot variant flash-64-3000-usbc-haptic");
    SAME("boot build=0123abcd-261026 rtc=2026-10-26 12:00:00");
    SAME("p panic pc=00012345 lr=00023456 sp=40001000");
    SAME("p panic kind=0 count=1 thread=main");
    SAME("9 video clock: 150 MHz (verified image)");
    SAME("10 art pub 3 st=1 e=0 fs=2/9");
    SAME("11 m0 disk wake by=tagcache play=1 t=40");
    SAME("12 audio give 12 want=100 audio=3->4 ms=7 pos=1");
    SAME("log dropped=3");

    /* paths */
    GIVES("13 usb marker /Music/Glass Harbor/x failed",
          "13 usb marker <path>");
    GIVES("14 video start /Videos/My Clip.mp4: 120 samples, 3 keys",
          "14 video start <path>: 120 samples, 3 keys");
    GIVES("15 video no index for /Videos/a.b/Clip (2).M4V",
          "15 video no index for <path>");
    GIVES("16 x open=/Audiobooks/Book One.m4b rc=-2",
          "16 x open=<path> rc=-2");
    GIVES("17 bench switch /.rockbox/bench/x.on on",
          "17 bench switch <path>");
    /* names with an extension, no path */
    GIVES("18 art pub Velvet Lanterns (Live).jpg st=1 e=0",
          "18 art <path> st=1 e=0");
    GIVES("19 db at=Lowtide Drifters, Vol. 3.flac n=2",
          "19 db at=<path> n=2");
    /* keys and quotes */
    GIVES("20 np title=Glass Harbor len=3", "20 np title=<title>");
    GIVES("21 np artist=\"Quorn & Tallow\" n=1", "21 np artist=<title>");
    GIVES("22 iap playlist_create failed \"Partway\" rc=1",
          "22 iap playlist_create failed <title> rc=1");
    GIVES("23 ab path=/Audiobooks/x", "23 ab path=<path>");
    GIVES("24 ab mark \"unclosed name", "24 ab mark <title>");
    /* the system folder's name, any case */
    GIVES("25 x saw ROCKBOX here", "25 x saw <sys> here");
    GIVES("26 x saw .rockbox/x.cfg here", "26 x saw .<sys>/x.cfg here");
    /* non-ASCII left over: the message goes */
    GIVES("27 lib now Zephyrine \xc3\x9c" "berklang", "27 lib <title>");
    GIVES("28 lib bad \x01 byte", "28 lib <title>");
    /* a deny list (the playing track's names) */
    {
        static const char *const deny[] = { "Glass Harbor", "Hi", NULL };
        const char *g = scrub_deny("29 lib saw glass harbor twice Hi",
                                   deny, 3);
        CHECK(!strcmp(g, "29 lib saw <title> twice Hi"));
    }
    /* counted */
    hits = 0;
    strcpy(in, "30 x title=a /b \"c\"");
    diag_scrub_line(in, strlen(in), out, sizeof(out), NULL, 0, &hits);
    CHECK(hits == 1);
    hits = 0;
    strcpy(in, "31 x \"c\" /b");
    diag_scrub_line(in, strlen(in), out, sizeof(out), NULL, 0, &hits);
    CHECK(hits == 2);
    /* never overruns a small output */
    {
        char small[16];

        memset(small, 'Z', sizeof(small));
        diag_scrub_line("32 x 0123456789abcdefghij", 25, small, 12, NULL, 0,
                        NULL);
        CHECK(strlen(small) == 11 && small[12] == 'Z');
    }

    /* the property: no word of any synthetic title, in any form a log
     * line has used, survives */
    for (k = 0; k < sizeof(titles) / sizeof(titles[0]); k++)
    {
        static const char *const forms[] = {
            "40 art pub %s.jpg st=1 e=0",
            "41 video start /Videos/%s/x.mp4: 120 samples",
            "42 np title=%s",
            "43 usb marker /%s/x",
            "44 tag msg \"%s\" failed",
            "45 db playing %s.m4b at=3",
            "p panic msg open /Music/%s/01 x.flac failed",
            "46 ab book=%s pos=3",
            "47 x %s.FLAC",
            "48 x dir=%s",
            " tail open %s.opus",
            "*np lyrics %s.lrc",
        };
        const char *const deny[] = { titles[k] };

        for (size_t f = 0; f < sizeof(forms) / sizeof(forms[0]); f++)
        {
            snprintf(in, sizeof(in), forms[f], titles[k]);
            no_word_survives(scrub(in), titles[k]);
        }
        /* a bare name only the deny list can know */
        snprintf(in, sizeof(in), "49 lib now %s here", titles[k]);
        no_word_survives(scrub_deny(in, deny, 1), titles[k]);
    }

    /* the tail */
    {
        const char *t = "a\nbb\nccc\ndddd\n";
        size_t n = strlen(t);

        CHECK(diag_tail(t, n, true, 2) == 5);       /* "ccc" */
        CHECK(diag_tail(t, n, true, 9) == 0);
        CHECK(diag_tail(t, n, false, 9) == 2);      /* partial "a" skipped */
        CHECK(diag_tail(t, n, true, 0) == n);
        CHECK(diag_tail("abc", 3, false, 2) == 3);  /* no whole line */
        CHECK(diag_tail("x\nyz", 4, true, 1) == 2); /* no final '\n' */
    }

    /* the builder: bounded, NUL ended, lines scrubbed */
    {
        char buf[48];
        struct diag_buf b;

        diag_init(&b, buf, sizeof(buf));
        diag_printf(&b, "v=%d\n", 7);
        CHECK(!strcmp(buf, "v=7\n") && !b.full);
        diag_put_lines(&b, "  ", "1 a /p/q\r\n\n2 b ok", 17, NULL, 0, NULL);
        CHECK(!strcmp(buf, "v=7\n  1 a <path>\n  2 b ok\n"));
        diag_printf(&b, "%s", "0123456789012345678901234567890123456789");
        CHECK(b.full && b.len == sizeof(buf) - 1 && strlen(buf) == b.len);
    }

    if (fails)
    {
        printf("diag: %d of %d checks failed\n", fails, checks);
        return 1;
    }
    return 0;
}
