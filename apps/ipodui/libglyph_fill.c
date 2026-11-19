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

/* iPod UI shell: the library glyph file as the list font's glyph source
 * (0.9.0, #235; the draw side of libglyph.h)
 *
 * One movable block (pinned for its life: the COP draws too), allocated
 * when the theme attaches the file (shell start, or the first quiet
 * moment after a commit, theme.c): the file's code-point table (record
 * order), its sorted permutation, and one 4 KB page. A list font miss on
 * a glyph the table holds reads that glyph's page, one aligned 4 KB read,
 * and puts all of its records into the glyph cache: the glyphs of the
 * next ~20 rows, in the order the lists first show them. A glyph past the
 * resident cap, or not in the file, is read from the font as before. */
#include "config.h"
#include <stdio.h>
#include <string.h>
#include "file.h"
#include "font.h"
#include "core_alloc.h"
#include "ipodui/uilog.h"
#define LGLOG(...) ui_log("libglyph", __VA_ARGS__)
#include "libglyph.h"

static int lg_handle = -1;
static int lg_n;                /* resident table entries */
static int lg_count;            /* records in the file */
static int lg_stride, lg_per_page;
static char lg_path[300];        /* "<generation dir>/" LIBGLYPH_NAME */

static bool lg_fill(struct font *pf, ucschar_t c)
{
    uint16_t *cps;
    uint8_t *page;
    int k, p, i, fd, n;
    bool ok;

    if (lg_handle <= 0)
        return false;
    cps = core_get_data(lg_handle);
    k = libglyph_map_find(cps, cps + lg_n, lg_n, c);
    if (k < 0)
        return false;
    p = k / lg_per_page;
    page = (uint8_t *)(cps + 2 * lg_n);
    fd = open(lg_path, O_RDONLY);
    ok = fd >= 0
        && lseek(fd, LIBGLYPH_PAGE * (1 + (off_t)p), SEEK_SET)
           == LIBGLYPH_PAGE * (1 + (off_t)p)
        && read(fd, page, LIBGLYPH_PAGE) > 0;
    if (fd >= 0)
        close(fd);
    if (!ok)
    {
        LGLOG("page %d read failed", p);
        return false;
    }
    n = MIN(lg_per_page, lg_count - p * lg_per_page);
    for (i = 0; i < n; i++)
        font_fill_put(pf, ((const uint16_t *)page)[i],
                      page + 2 * lg_per_page + i * lg_stride);
    return true;
}

int libglyph_attach(const char *dir, const struct libglyph_stamp *want,
                    int comp_id, int max_glyphs, size_t *bytes)
{
    struct libglyph_hdr h;
    uint16_t *cps;
    size_t need;
    long at;
    int fd;
    bool ok;

    *bytes = 0;
    font_set_fill(comp_id, NULL);
    if (lg_handle > 0)
        core_free(lg_handle);
    lg_handle = -1;
    if (!dir || comp_id < 0
        || libglyph_check(dir, want, &h) != LIBGLYPH_OK || !h.count
        || (int)h.stride != font_sparse_stride(comp_id))
        return 0;
    lg_count = h.count;
    lg_n = MIN((int)h.count, max_glyphs);
    lg_stride = h.stride;
    lg_per_page = LIBGLYPH_PAGE / (h.stride + 2);
    need = (size_t)lg_n * 4 + LIBGLYPH_PAGE;
    lg_handle = core_alloc(need);
    if (lg_handle <= 0)
    {
        lg_handle = -1;
        LGLOG("no room for %ld B", (long)need);
        return 0;
    }
    snprintf(lg_path, sizeof(lg_path), "%s/%s", dir, LIBGLYPH_NAME);
    cps = core_get_data_pinned(lg_handle);      /* kept: the COP draws */
    at = LIBGLYPH_PAGE * (1 + (long)((h.count + lg_per_page - 1)
                                      / lg_per_page));
    fd = open(lg_path, O_RDONLY);
    ok = fd >= 0 && lseek(fd, at, SEEK_SET) == at
        && read(fd, cps, lg_n * 2) == lg_n * 2;
    if (fd >= 0)
        close(fd);
    if (!ok)
    {
        core_free(lg_handle);
        lg_handle = -1;
        LGLOG("table read failed");
        return 0;
    }
    libglyph_map_sort(cps, cps + lg_n, lg_n);
    font_set_fill(comp_id, lg_fill);
    *bytes = need;
    LGLOG("attached n=%d of %d", lg_n, lg_count);
    return lg_count;
}
