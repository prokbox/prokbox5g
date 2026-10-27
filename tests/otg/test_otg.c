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

/* Host tests of the On-The-Go store, apps/ipodui/otg.c (docs/phase11-
 * brief.md 3.2, 4.4, section 7 criterion 4), with apps/ipodui/library.c
 * over the fake tagcache of tests/ipodui, real files in a temporary
 * directory and a fake database file name tag file (database_4.tcd):
 *  - add a track, a group row (records then header, count = lines,
 *    commit id), the OTG_MAX cap; nothing touches the playlist;
 *  - open: one read of otg.idx when current; ONE resolve pass after a
 *    commit, after a cut between the records and the header, and none
 *    again for a line that stays unresolved; unresolved rows show the
 *    file name; no database: names only;
 *  - a torn last line (power cut mid-append) is dropped and cut off;
 *  - remove, clear, save ("New Playlist N", first free N), play (a queue
 *    from the paths, from pos);
 *  - otg_check() follows the title and length. GPL v2+ */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "config.h"
#include "tagcache.h"
#include "ipodui/library.h"
#include "ipodui/otg.h"
#include "ipodui/libfile.h"
#include "fake_tagcache.h"

static int failures, checks;

#define CHECK(cond, ...) do { \
        checks++; \
        if (!(cond)) { \
            failures++; \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__); \
            putchar('\n'); \
        } \
    } while (0)

/* -- fakes this test adds ---------------------------------------------- */
static char last_add[256], last_resolve[256], last_open[256], last_play[256];
static int resolves;
static char strip[180000];
static int strip_claims, strip_held;

void test_trace(const char *fmt, ...)
{
    char line[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (!strncmp(line, "otg add", 7))
        strcpy(last_add, line);
    else if (!strncmp(line, "otg resolve", 11))
    {
        strcpy(last_resolve, line);
        resolves++;
    }
    else if (!strncmp(line, "otg open", 8))
        strcpy(last_open, line);
    else if (!strncmp(line, "otg play", 8))
        strcpy(last_play, line);
    if (getenv("TEST_LIBRARY_TRACE"))
        puts(line);
}

void *ui_strip_claim(int owner, bool *kept)
{
    (void)owner;
    *kept = false;
    strip_claims++;
    strip_held++;
    return strip;
}

void ui_strip_put(void)
{
    strip_held--;
}

int ui_strip_new_owner(void)
{
    return 7;
}

/* -- fixture ------------------------------------------------------------ */
#define E(t, a, al, aa, g, n) \
    { t, a, al, aa, g, NULL, "/Music/x.mp3", 1, n, 100000, 0, 0, 0, false, 0 }

static struct fake_entry lib[] = {
    /* 0 */ E("One", "Ann", "First", "Ann", "Rock", 1),
    /* 1 */ E("Two", "Ann", "First", "Ann", "Rock", 2),
    /* 2 */ E("Three", "Ann", "First", "Ann", "Rock", 3),
    /* 3 */ E("Four", "Bob", "Second", "Bob", "Pop", 1),
    /* 4 */ E("Five", "Bob", "Second", "Bob", "Pop", 2),
};
#define LIB_N (int)(sizeof(lib) / sizeof(lib[0]))

/* the database's file name tag file, as tagcache.c writes it: what the
 * fake's tagcache_retrieve() gives ("/e<idx><path>") */
static void write_tagfile(const char *dir, int n)
{
    char path[300], name[64];
    FILE *f;
    int32_t hdr[3] = { 0x54434801, 0, 0 };
    int i;

    snprintf(path, sizeof(path), "%s/database_%d.tcd", dir, tag_filename);
    f = fopen(path, "wb");
    hdr[2] = n;
    fwrite(hdr, sizeof(hdr), 1, f);
    for (i = 0; i < n; i++)
    {
        int32_t e[2];
        int len;

        snprintf(name, sizeof(name), "/e%d%s", i, lib[i].path);
        len = strlen(name) + 1;
        e[0] = len + (8 - (len + 8) % 8) % 8;
        e[1] = i;
        fwrite(e, sizeof(e), 1, f);
        fwrite(name, len, 1, f);
        fwrite("XXXXXXXX", e[0] - len, 1, f);
    }
    fclose(f);
}

static long file_read(const char *path, char *buf, long size)
{
    FILE *f = fopen(path, "rb");
    long n;

    if (!f)
        return -1;
    n = fread(buf, 1, size - 1, f);
    buf[n] = '\0';
    fclose(f);
    return n;
}

static int file_lines(const char *path)
{
    static char buf[200000];
    long n = file_read(path, buf, sizeof(buf)), i;
    int lines = 0;

    for (i = 0; i < n; i++)
        lines += buf[i] == '\n';
    return n < 0 ? -1 : lines;
}

static bool idx_get(struct otg_idx_hdr *h, struct otg_rec *r, int max)
{
    FILE *f = fopen(OTG_IDX, "rb");
    bool ok;

    if (!f)
        return false;
    ok = fread(h, sizeof(*h), 1, f) == 1;
    if (ok && r)
        fread(r, sizeof(*r), max, f);
    fclose(f);
    return ok;
}

static void append_raw(const char *path, const char *text)
{
    FILE *f = fopen(path, "ab");
    fputs(text, f);
    fclose(f);
}

#define LIST OTG_DIR "/" OTG_LIST

static const char *label(int v, int i)
{
    static char buf[128];
    return lib_label(v, i, buf, sizeof(buf));
}

static void test_store(const char *dir)
{
    struct otg_idx_hdr h;
    struct otg_rec r[8];
    char name[64], buf[4096];
    int v, albums, songs, n, pl_n;

    mkdir("rb", 0777);
    fake_db_load(lib, LIB_N);
    write_tagfile(dir, LIB_N);
    CHECK(otg_count() == 0, "empty count %d", otg_count());

    /* a track, then an album (Second: 2 tracks) */
    pl_n = fake_pl_n;
    CHECK(otg_add_track(1 + 2) == 1, "add a track");
    CHECK(strstr(last_add, "n=1 reads=1 "), "add trace: %s", last_add);
    CHECK(file_lines(LIST) == 1, "1 line");
    albums = lib_open(LIB_ALBUMS, NULL);
    CHECK(lib_count(albums) == 2 && !strcmp(label(albums, 1), "Second"),
          "albums");
    CHECK(otg_add_row(albums, 1) == 2, "add an album");
    CHECK(strstr(last_add, "n=2 reads=2 writes=3"), "album trace: %s",
          last_add);
    CHECK(otg_count() == 3 && file_lines(LIST) == 3, "3 lines");
    CHECK(fake_pl_n == pl_n, "adding touched the playlist");
    CHECK(idx_get(&h, r, 8) && h.magic == OTG_MAGIC && h.count == 3
          && h.commitid == tagcache_ui_commitid() && r[0].idx == 2
          && r[1].idx == 3 && r[2].idx == 4 && r[1].check == otg_check(3),
          "idx: count %u", h.count);
    CHECK(strip_held == 0, "strip not put back");

    /* open: current, no resolve, one read of the index */
    v = otg_open_view();
    CHECK(v > 0 && lib_count(v) == 3 && lib_id(v, 0) == 2
          && !strcmp(label(v, 1), "Four") && resolves == 0,
          "open: %d rows, %d resolves", lib_count(v), resolves);
    CHECK(strstr(last_open, "unresolved=0 reads=1 writes=0"), "open: %s",
          last_open);
    lib_close(v);

    /* play from row 1: the paths, never the store file */
    CHECK(otg_play(1) == 0 && fake_pl_n == 3 && fake_pl[0] == 2
          && fake_pl[1] == 3 && fake_pl[2] == 4 && fake_pl_start == 1,
          "play: n=%d start=%d", fake_pl_n, fake_pl_start);
    CHECK(strstr(last_play, "n=3 pos=1 reads="), "play trace: %s",
          last_play);
    /* lib_play() of a LIB_OTG view is otg_play() */
    v = otg_open_view();
    CHECK(lib_play(v, 2, false) == 0 && fake_pl_start == 2, "lib_play");
    lib_close(v);

    /* a commit: ONE resolve pass at the next open */
    fake_db_commit();
    resolves = 0;
    v = otg_open_view();
    CHECK(resolves == 1 && strstr(last_resolve, "n=3 unresolved=0"),
          "resolve after commit: %s", last_resolve);
    CHECK(v > 0 && lib_id(v, 2) == 4, "resolved rows");
    CHECK(idx_get(&h, NULL, 0) && h.commitid == tagcache_ui_commitid(),
          "idx commit id after resolve");
    lib_close(v);
    v = otg_open_view();
    CHECK(resolves == 1, "resolved twice");
    lib_close(v);

    /* a line not in the database: unresolved once, its file name shown;
     * the list changed under us (USB), so the index is out of step */
    append_raw(LIST, "/Music/Gone Away.flac\n");
    v = otg_open_view();
    CHECK(resolves == 2 && strstr(last_resolve, "n=4 unresolved=1"),
          "resolve with a missing file: %s", last_resolve);
    CHECK(lib_count(v) == 4 && !strcmp(label(v, 3), "Gone Away")
          && lib_id(v, 3) == -2, "unresolved row: \"%s\" id %d",
          label(v, 3), lib_id(v, 3));
    {
        char sub[64];
        const char *s = lib_sublabel(v, 3, sub, sizeof(sub));
        CHECK(s && !*s, "unresolved second line");
    }
    lib_close(v);
    v = otg_open_view();
    CHECK(resolves == 2 && lib_count(v) == 4
          && !strcmp(label(v, 3), "Gone Away"),
          "an unresolved line re-resolved without a commit");
    lib_close(v);

    /* a torn last line (cut mid-append): dropped, cut off */
    append_raw(LIST, "/Music/partial");
    v = otg_open_view();
    CHECK(lib_count(v) == 4 && file_lines(LIST) == 4
          && file_read(LIST, buf, sizeof(buf)) > 0
          && !strstr(buf, "partial"), "torn line: %d rows", lib_count(v));
    lib_close(v);

    /* a list edited over USB with no final newline (the last name exists): the last entry is kept and ended with a newline */
    append_raw(LIST, OTG_IDX);   /* a name that exists */
    {
        int resolves_before = resolves;

        v = otg_open_view();
        CHECK(lib_count(v) == 5 && file_lines(LIST) == 5
          && file_read(LIST, buf, sizeof(buf)) > 0
          && strstr(buf, OTG_IDX "\n"), "hand-edited tail kept: %d",
          lib_count(v));
        lib_close(v);
        CHECK(otg_remove(4) == 0 && otg_count() == 4,
              "hand-edited tail removed");
        resolves = resolves_before;     /* the later checks count passes */
    }

    /* cut between the records and the header: one record too many */
    {
        FILE *f = fopen(OTG_IDX, "ab");
        struct otg_rec x = { 1, 0 };
        fwrite(&x, sizeof(x), 1, f);
        fclose(f);
    }
    otg_count();
    v = otg_open_view();
    CHECK(resolves == 3 && lib_count(v) == 4 && lib_id(v, 1) == 3,
          "torn index: %d resolves", resolves);
    lib_close(v);

    /* remove row 1 ("Four"): list and index stay in step, no resolve */
    CHECK(otg_remove(1) == 0 && otg_count() == 3 && file_lines(LIST) == 3,
          "remove");
    v = otg_open_view();
    CHECK(resolves == 3 && lib_count(v) == 3 && lib_id(v, 0) == 2
          && lib_id(v, 1) == 4 && !strcmp(label(v, 2), "Gone Away"),
          "after remove: %d resolves", resolves);
    lib_close(v);

    /* save: New Playlist 1, then 2; the store is emptied */
    mkdir(OTG_PL_DIR, 0777);
    n = otg_save(name, sizeof(name));
    CHECK(n == 3 && !strcmp(name, "New Playlist 1")
          && file_lines(OTG_PL_DIR "/New Playlist 1.m3u8") == 3
          && otg_count() == 0 && file_lines(LIST) == 0, "save: %d %s", n,
          name);
    CHECK(otg_add_track(1) == 1 && otg_save(name, sizeof(name)) == 1
          && !strcmp(name, "New Playlist 2"), "second save: %s", name);
    CHECK(otg_save(name, sizeof(name)) < 0, "saving an empty store");

    /* clear */
    CHECK(otg_add_track(2) == 1 && otg_clear() == 0 && otg_count() == 0
          && idx_get(&h, NULL, 0) && h.count == 0, "clear");

    /* no database: names only, no resolve */
    CHECK(otg_add_track(1) == 1, "add");
    fake_db_commit();
    fake_db_usable(false);
    v = otg_open_view();
    CHECK(v > 0 && lib_count(v) == 1 && lib_id(v, 0) == -2
          && !strcmp(label(v, 0), "x") && resolves == 3,
          "no database: \"%s\"", label(v, 0));
    lib_close(v);
    fake_db_usable(true);

    /* the cap */
    songs = lib_open(LIB_SONGS, NULL);
    for (n = 0; otg_count() < OTG_MAX && n < 3000; n++)
        otg_add_row(songs, 0);
    CHECK(otg_count() == OTG_MAX && otg_add_row(songs, 0) == 0
          && strstr(last_add, "full=1"), "cap: %d", otg_count());
    CHECK(file_lines(LIST) == OTG_MAX && idx_get(&h, NULL, 0)
          && h.count == OTG_MAX, "cap files");
    CHECK(otg_play(OTG_MAX - 1) == 0 && fake_pl_n == OTG_MAX
          && fake_pl_start == OTG_MAX - 1, "play %d", fake_pl_n);
    lib_close(songs);
    lib_close(albums);
    CHECK(strip_held == 0, "strip held: %d", strip_held);

    /* otg_check: title and length */
    {
        uint32_t c = otg_check(0);
        fake_db_set_stat(0, tag_length, 123);
        CHECK(otg_check(0) != c, "check ignores the length");
    }
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";

    fake_db_dir(dir);
    test_store(dir);
    printf("   %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
