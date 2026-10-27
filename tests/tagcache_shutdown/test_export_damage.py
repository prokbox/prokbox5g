#!/usr/bin/env python3
#***************************************************************************
#               __________                   __    __________
#    Open       \______   \_______    ____  |  | __\______   \  ____  ___  ___
#    Source      |     ___/\_  __ \  /  _ \ |  |/ / |    |  _/ /  _ \ \  \/  /
#    Player      |    |     |  | \/ (  <_> )|    <  |    |   \(  <_> ) >    <
#    Firmware    |____|     |__|     \____/ |__|_ \ |______  / \____/ /__/\_ \
#                                                \/        \/               \/
#
# Custom firmware for the iPod Video, 5th generation (5G and 5.5G).
# Not affiliated with or endorsed by Apple Inc.
#
# Copyright (C) 2026 prokopto-dev
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
# KIND, either express or implied.
#
#***************************************************************************

"""#85 round 8 MED-U1: an export that skips rows with statistics is not
complete.

A filename tag file with a good header but damaged entries (the owed
rebuild after three content failures) used to give "complete, nothing to
export" (2), so the rebuild imported nothing and every row unchanged since
the last rebuild became 0. The actual ui_export_to() runs here over real
files with the tagcache file access replaced: a damaged name with
statistics makes the export 0 (the import ranks the older complete
changelog, which stays untouched), never 2 and never a partial file in
its place; only a library with no statistics gives 2; an I/O failure -1.
"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "apps/tagcache.c").read_text()


def function(signature):
    start = source.index(signature + "\n{")
    depth, end = 1, source.index("{", start) + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


tags = source[source.index("static const unsigned char ui_export_tags[] = {"):
              source.index("};", source.index("static const unsigned char ui_export_tags[] = {")) + 2]
harness = r'''
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define MAX_PATH 260
#define TAGCACHE_BUFSZ 1024
#define HZ 100
#define UNUSED_ATTR __attribute__((unused))
#define TC_CTX_ARG(c) (c),
#define DEBUGF(...) ((void)0)
#define logf(...) ((void)0)
#define ARRAYLEN(a) (sizeof(a) / sizeof((a)[0]))
#define TAGCACHE_FILE_CHANGELOG "database_changelog.txt"
#define UI_EXPORT_CHUNK 1536
#define UI_EXPORT_END "## end\n"
#define STOP_REQUESTED() false
enum { tag_filename, tag_playcount, tag_rating, tag_playtime, tag_lastplayed,
       tag_commitid, tag_lastelapsed, tag_lastoffset, TAG_COUNT };
#define FLAG_DELETED 1
#define FLAG_DIRTYNUM 4
enum e_read_errors { e_SUCCESS, e_SUCCESS_LEN_ZERO, e_ENTRY_SIZEMISMATCH };
struct index_entry { int32_t tag_seek[TAG_COUNT]; int32_t flag; };
struct master_header { struct { int entry_count; } tch; int32_t commitid; };
struct tagcache_header { int32_t magic, datasize, entry_count; };
struct tagfile_entry { int tag_length; };
struct tagcache_context { struct { char db_path[MAX_PATH]; int commit_step;
                                   bool ready; } stat;
                          struct master_header master; };
static struct tagcache_context ctx, *ui_build = &ctx;
#define tc_stat (ui_build->stat)
#define current_tcmh (ui_build->master)
#define NO_INLINE
#define CONFIG_CHARGING 1
#define TIME_BEFORE(a, b) ((long)((a) - (b)) < 0)
static int32_t ui_export_bad = -1;
static bool ui_commit_pending, ui_reload_pending, charger = true;
static long ui_stats_tick;
static int exports_run;
static void run_command_queue(bool f) { (void)f; exports_run++; }
static bool charger_inserted(void) { return charger; }
static bool keep_new;                   /* a power cut before the remove */
static int test_remove(const char *p)
{ return keep_new && strstr(p, ".new") ? 0 : remove(p); }
static int read_lock, write_lock;
static bool ui_import_pending;
static long current_tick, ui_stats_gen = 1, ui_stats_exported;
static void test_sleep(int t) { (void)t; }
#define sleep test_sleep
static int audio_status(void) { return 0; }
static void do_timed_yield(void) {}
static void ui_export_recover(char *name) { (void)name; }
static void itoa_buf(char *b, size_t n, int v) { snprintf(b, n, "%d", v); }
static const char *tagcache_tag_to_str(int t)
{ static const char *n[] = { "filename", "playcount", "rating", "playtime",
  "lastplayed", "commitid", "lastelapsed", "lastoffset" }; return n[t]; }
#define ROWS 4
static struct index_entry rows[ROWS];
static bool damaged[ROWS];
static int cur, fail_lseek, name_fd = -1, datasize = 1000;
#define NAMES 100                       /* row r's name at NAMES + 10 r */
static int open_master_fd(struct tagcache_context *c, struct master_header *h, bool w)
{ (void)c; (void)w; h->tch.entry_count = ROWS; h->commitid = 7; return dup(0); }
static int open_tag_fd(struct tagcache_context *c, struct tagcache_header *h, int t, bool w)
{ (void)c; (void)t; (void)w; h->datasize = datasize; return name_fd = dup(0); }
static off_t test_lseek(int fd, off_t pos, int whence)
{
    (void)whence;
    /* Rockbox's lseek: EINVAL outside the file; fail_lseek: EIO */
    if (fd == name_fd && (pos < 0 || pos > 1012)) { errno = EINVAL; return -1; }
    if (fd == name_fd && fail_lseek) { errno = EIO; return -1; }
    cur = (int)pos;
    return pos;
}
#define lseek(fd, pos, w) test_lseek(fd, pos, w)
static ssize_t read_index_entries(struct tagcache_context *c, int fd,
                                  struct index_entry *e, size_t n)
{
    (void)c; (void)fd;
    int row = (cur - (int)sizeof(struct master_header)) / (int)sizeof(*e);
    memcpy(e, &rows[row], n * sizeof(*e));
    return n * sizeof(*e);
}
static enum e_read_errors read_tagfile_entry_and_tag(struct tagcache_context *c,
    int fd, struct tagfile_entry *fe, char *buf, int n)
{
    (void)c; (void)fd; (void)fe;
    int row = (cur - NAMES) / 10;
    if (damaged[row]) return e_ENTRY_SIZEMISMATCH;
    snprintf(buf, n, "/Music/t%d.mp3", row);
    return e_SUCCESS;
}
static int open_db_fd(struct tagcache_context *c, const char *f, int mode)
{
    char p[2 * MAX_PATH];
    snprintf(p, sizeof(p), "%s/%s", c->stat.db_path, f);
    return open(p, mode, 0666);
}
''' + tags + "\n" + function("static int ui_export_put(char *out, int n, int tag, const char *val)") \
    + "#define remove test_remove\n" \
    + function("static int ui_export_to(char *name, bool wait, bool stoppable)") \
    + "#undef remove\n" \
    + function("static int NO_INLINE ui_export_changelog_ex(bool stoppable)") \
    + function("static int ui_export_changelog(void)") \
    + function("static void ui_export_if_changed(void)") \
    + "#define UI_EXPORT_QUIET (60 * HZ)\n" \
    + function("static void ui_export_idle(void)") + r'''
#undef lseek
static char *slurp(const char *leaf)
{
    static char buf[4096];
    char p[2 * MAX_PATH];
    snprintf(p, sizeof(p), "%s/%s", ctx.stat.db_path, leaf);
    FILE *f = fopen(p, "r");
    if (!f) return NULL;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    return buf;
}
static void reset(void)
{
    memset(rows, 0, sizeof(rows));
    memset(damaged, 0, sizeof(damaged));
    fail_lseek = 0;
    keep_new = false;
    ui_export_bad = -1;
    datasize = 1000;
    for (int r = 0; r < ROWS; r++)
        rows[r].tag_seek[tag_filename] = NAMES + 10 * r;
    char p[2 * MAX_PATH];
    snprintf(p, sizeof(p), "%s/" TAGCACHE_FILE_CHANGELOG, ctx.stat.db_path);
    FILE *f = fopen(p, "w");
    assert(f);
    fputs("## Changelog version 1\n## commit 3\nfilename=\"/old.mp3\" playcount=\"9\" \n" UI_EXPORT_END, f);
    fclose(f);
}
#define OLD_KEPT() (strstr(slurp(TAGCACHE_FILE_CHANGELOG), "/old.mp3") \
                    && !slurp(TAGCACHE_FILE_CHANGELOG ".new"))
int main(int argc, char **argv)
{
    char name[TAGCACHE_BUFSZ];
    (void)argc;
    snprintf(ctx.stat.db_path, MAX_PATH, "%s", argv[1]);
    /* every name readable: a complete export with its commit line */
    reset();
    rows[1].flag = rows[3].flag = FLAG_DIRTYNUM;
    rows[1].tag_seek[tag_playcount] = 5;
    rows[3].tag_seek[tag_playcount] = 6;
    assert(ui_export_to(name, false, false) == 1);
    char *t = slurp(TAGCACHE_FILE_CHANGELOG);
    assert(strstr(t, "## commit 7\n") && strstr(t, "/Music/t1.mp3")
           && strstr(t, "/Music/t3.mp3") && strstr(t, UI_EXPORT_END));
    /* partial damage: one row with statistics unreadable: not complete
     * (0), the older complete changelog stays for the import's ranking */
    reset();
    rows[1].flag = rows[3].flag = FLAG_DIRTYNUM;
    rows[1].tag_seek[tag_playcount] = 5;
    damaged[3] = true;
    assert(ui_export_to(name, false, false) == 0 && OLD_KEPT());
    /* every row with statistics damaged: 0, never "nothing to export" */
    reset();
    rows[2].flag = FLAG_DIRTYNUM;
    damaged[2] = true;
    assert(ui_export_to(name, false, false) == 0 && OLD_KEPT());
    /* a damaged name without statistics (or deleted) changes nothing */
    reset();
    damaged[0] = true;
    rows[2].flag = FLAG_DIRTYNUM | FLAG_DELETED;
    damaged[2] = true;
    assert(ui_export_to(name, false, false) == 2 && OLD_KEPT());
    /* R8a: a name offset outside the name file (below its header, past
     * its data) is damage, not I/O: 0, never a retry loop */
    reset();
    rows[1].flag = FLAG_DIRTYNUM;
    rows[1].tag_seek[tag_filename] = 5000;
    assert(ui_export_to(name, false, false) == 0 && OLD_KEPT());
    reset();
    rows[1].flag = FLAG_DIRTYNUM;
    rows[1].tag_seek[tag_filename] = -4;
    assert(ui_export_to(name, false, false) == 0 && OLD_KEPT());
    /* a damaged header whose datasize exceeds the real file: a seek past
     * the real end (EINVAL) is damage as well, never a retry loop */
    reset();
    datasize = 5000;
    rows[1].flag = FLAG_DIRTYNUM;
    rows[1].tag_seek[tag_filename] = 2000;
    assert(ui_export_to(name, false, false) == 0 && OLD_KEPT());
    /* R8b (1): the export stops at the first damaged row and never writes
     * the end line: a power cut before the .new is removed leaves no
     * complete-looking partial file for recovery or the ranking */
    reset();
    rows[0].flag = rows[2].flag = FLAG_DIRTYNUM;
    rows[0].tag_seek[tag_playcount] = 3;
    damaged[2] = true;
    keep_new = true;
    assert(ui_export_to(name, false, false) == 0);
    t = slurp(TAGCACHE_FILE_CHANGELOG ".new");
    assert(t && !strstr(t, UI_EXPORT_END));
    assert(strstr(slurp(TAGCACHE_FILE_CHANGELOG), "/old.mp3"));
    keep_new = false;
    /* R8b (2): with the names damaged, the idle and post-USB exports
     * leave this library alone until its commit changes (publication,
     * rebuild); a working export clears the mark */
    reset();
    rows[2].flag = FLAG_DIRTYNUM;
    damaged[2] = true;
    tc_stat.ready = true;
    current_tcmh.commitid = 7;
    ui_stats_gen = 5;
    ui_stats_exported = 1;
    exports_run = 0;
    ui_export_if_changed();
    assert(exports_run == 1 && ui_export_bad == 7);
    ui_export_if_changed();
    current_tick = 1000 * HZ;
    ui_export_idle();
    assert(exports_run == 1);
    current_tcmh.commitid = 8;          /* a publication */
    damaged[2] = false;
    ui_export_idle();
    assert(exports_run == 2 && ui_export_bad == -1 && ui_stats_exported == 5);
    /* the name file cannot be positioned: I/O, retried */
    reset();
    rows[1].flag = FLAG_DIRTYNUM;
    fail_lseek = 1;
    assert(ui_export_to(name, false, false) == -1 && OLD_KEPT());
    puts("tagcache export: damaged names with statistics never make an export complete");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="tagcache-export-") as temp:
    c = Path(temp) / "export.c"
    c.write_text(harness)
    exe = Path(temp) / "export"
    subprocess.run([os.environ.get("HOSTCC", "cc"), "-std=gnu99", "-Wall", "-Wextra",
                    "-Werror", "-Wno-unused-function", str(c), "-o", str(exe)], check=True)
    subprocess.run([str(exe), temp], check=True)
