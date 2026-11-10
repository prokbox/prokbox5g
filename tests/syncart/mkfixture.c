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

/* Synthetic cover database + song database from our own writers (#225),
 * for host tests and the perf fixtures. Nothing here comes from a real
 * device: the pixels are a test pattern, the ids a counter.
 *
 *   mkfixture ROOT < LIST
 *       LIST: one track per line, "PATH<TAB>GROUP" (PATH like
 *       /iPod_Control/Music/F23/ABCD.mp3). Tracks with the same GROUP
 *       share one stored cover (one image per format, every song's
 *       record naming the same offsets); GROUP "-" (or none): no cover.
 *       Writes ROOT/iPod_Control/iTunes/iTunesDB and
 *       ROOT/iPod_Control/Artwork/{ArtworkDB,F1028_1.ithmb,F1029_1.ithmb}.
 *       Options before ROOT: --no-link (a short 0xf4-byte song header
 *       with no +352 field: the dbid join only, as older writers),
 *       --first-id N (first image id, default 100), --mirror (every
 *       picture mirrored left to right, r5 from W - 1 - x: a re-sync).
 *   Test code only (fixwrite.c): never linked into the firmware.
 *   mkfixture --ref G W H OUT
 *       the pattern of the G-th cover group (0-based, in first-seen
 *       order) at W x H, RGB565 LE, row-major: a reference image.
 *
 * Pattern of group g at (x, y) in a W x H image:
 *   r5 = x * 31 / (W - 1), g6 = y * 63 / (H - 1), b5 = (g * 7 + 3) & 31,
 *   pixel = r5 << 11 | g6 << 5 | b5, stored little-endian.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "fixwrite.h"

#define FMT_SMALL 1028                  /* 100 x 100 [public-doc] */
#define FMT_LARGE 1029                  /* 200 x 200 */

static const struct fx_fmt fmts[2] = {
    { FMT_SMALL, 100, 100 }, { FMT_LARGE, 200, 200 },
};

static void mkdirs(const char *root)
{
    char b[1024];

    mkdir(root, 0777);
    snprintf(b, sizeof(b), "%s/iPod_Control", root);
    mkdir(b, 0777);
    snprintf(b, sizeof(b), "%s/iPod_Control/iTunes", root);
    mkdir(b, 0777);
    snprintf(b, sizeof(b), "%s/iPod_Control/Artwork", root);
    mkdir(b, 0777);
}

struct track {
    char *path;
    int group;                  /* -1 none */
};

int main(int argc, char **argv)
{
    static char line[2048];
    char **names = NULL, buf[1024];
    struct track *tr = NULL;
    int nt = 0, ng = 0, cap = 0, gcap = 0, a = 1;
    uint32_t first_id = 100, *ids;
    int link = 1;
    FILE *f;
    struct fx_art aw;
    struct fx_db dw;
    uint8_t *img;

    if (argc == 6 && !strcmp(argv[1], "--ref"))
    {
        unsigned w = atoi(argv[3]), h = atoi(argv[4]);

        img = malloc(w * h * 2);
        fx_pattern(img, atoi(argv[2]), w, h);
        f = fopen(argv[5], "wb");
        if (!f || fwrite(img, 1, w * h * 2, f) != w * h * 2)
            return 1;
        fclose(f);
        return 0;
    }
    for (; a < argc - 1; a++)
    {
        if (!strcmp(argv[a], "--mirror"))
            fx_mirror = 1;
        else if (!strcmp(argv[a], "--no-link"))
            link = 0;
        else if (!strcmp(argv[a], "--first-id") && a + 2 < argc)
            first_id = strtoul(argv[++a], NULL, 0);
        else
            break;
    }
    if (a != argc - 1)
    {
        fprintf(stderr, "usage: mkfixture [--mirror] [--no-link] "
                "[--first-id N] ROOT"
                " < LIST\n       mkfixture --ref G W H OUT\n");
        return 2;
    }
    while (fgets(line, sizeof(line), stdin))
    {
        char *tab = strchr(line, '\t'), *nl = strchr(line, '\n');
        int g = -1;

        if (nl)
            *nl = '\0';
        if (!line[0])
            continue;
        if (tab)
            *tab++ = '\0';
        if (tab && *tab && strcmp(tab, "-"))
        {
            for (g = 0; g < ng && strcmp(names[g], tab); g++)
                ;
            if (g == ng)
            {
                if (ng == gcap)
                    names = realloc(names, (gcap = gcap * 2 + 64)
                                           * sizeof(*names));
                names[ng++] = strdup(tab);
            }
        }
        if (nt == cap)
            tr = realloc(tr, (cap = cap * 2 + 256) * sizeof(*tr));
        tr[nt].path = strdup(line);
        tr[nt++].group = g;
    }
    mkdirs(argv[a]);
    /* the image files: one cover per group, in first-seen order */
    img = malloc(200 * 200 * 2);
    for (int i = 0; i < 2; i++)
    {
        snprintf(buf, sizeof(buf), "%s/iPod_Control/Artwork/F%lu_1.ithmb",
                 argv[a], (unsigned long)fmts[i].id);
        if (!(f = fopen(buf, "wb")))
            return 1;
        for (int g = 0; g < ng; g++)
        {
            size_t n = (size_t)fmts[i].w * fmts[i].h * 2;

            fx_pattern(img, g, fmts[i].w, fmts[i].h);
            if (fwrite(img, 1, n, f) != n)
                return 1;
        }
        fclose(f);
    }
    /* the cover database: one record per song with a cover, each naming
     * its group's stored image */
    snprintf(buf, sizeof(buf), "%s/iPod_Control/Artwork/ArtworkDB", argv[a]);
    if (!(f = fopen(buf, "wb")))
        return 1;
    ids = calloc(nt + 1, sizeof(*ids));
    fx_art_begin(&aw, f, fmts, 2, first_id);
    for (int i = 0; i < nt; i++)
    {
        uint32_t off[2];

        if (tr[i].group < 0)
            continue;
        off[0] = (uint32_t)tr[i].group * 20000;
        off[1] = (uint32_t)tr[i].group * 80000;
        ids[i] = fx_art_add(&aw, 0x1000000000000000ull + i, 30000 + i, off,
                            NULL);
    }
    fx_art_end(&aw);
    if (fclose(f))
        return 1;
    /* the song database, joined by dbid (and +352 unless --no-link) */
    snprintf(buf, sizeof(buf), "%s/iPod_Control/iTunes/iTunesDB", argv[a]);
    if (!(f = fopen(buf, "wb")))
        return 1;
    fx_db_begin(&dw, f, link ? 0x184 : 0xf4, 0);
    for (int i = 0; i < nt; i++)
    {
        struct fx_song s = { tr[i].path, 0x1000000000000000ull + i,
                             ids[i], ids[i] ? 30000 + i : 0 };

        fx_db_add(&dw, &s);
    }
    fx_db_end(&dw);
    if (fclose(f))
        return 1;
    printf("mkfixture: %d songs, %d covers\n", nt, ng);
    return 0;
}
