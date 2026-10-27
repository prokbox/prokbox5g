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

"""#85 round 7b HIGH-R7: a candidate imports the newest changelog there is.

The journal is sparse (values that differ from the master, plus folded
rows): rows unchanged since an export live only in that export and the
masters. With the master unreadable, importing nothing would zero them.
This compiles the actual ui_changelog_stamp()/ui_changelog_open() over
real files: the newest complete export (its "## commit" line) of every
generation directory and the root wins, never the candidate's own
scratch directory, and any changelog beats none.
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


export = function("static int ui_export_to(char *name, bool wait, bool stoppable)")
assert '"## Changelog version 1\\n## commit %ld\\n"' in export
assert "(long)mh.commitid" in export

harness = r'''
#include <assert.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define MAX_PATH 260
#define TAGCACHE_BUFSZ 1024
#define DBGEN_GENERATIONS 4
#define TAGCACHE_FILE_CHANGELOG "database_changelog.txt"
#define UI_EXPORT_END "## end\n"
#define MAX(a, b) ((a) > (b) ? (a) : (b))
struct tagcache_context { struct { char db_path[MAX_PATH]; bool ready; } stat;
                          struct { long commitid; } master; };
static struct tagcache_context active, build;
static struct tagcache_context *ui_active = &active, *ui_build = &build;
#define tc_stat (ui_build->stat)
static int ui_candidate_exported;
static char ui_db_root[MAX_PATH];
static int open_db_fd(struct tagcache_context *c, const char *f, int mode)
{
    char p[2 * MAX_PATH];
    snprintf(p, sizeof(p), "%s/%s", c->stat.db_path, f);
    return open(p, mode);
}
''' + function("static long ui_changelog_stamp(const char *path, char *buf)") \
    + function("static long ui_changelog_scan(char *name)") \
    + function("static int ui_changelog_open(char *name)") + r'''
static void put_as(const char *dir, const char *leaf, const char *text)
{
    char p[2 * MAX_PATH];
    snprintf(p, sizeof(p), "%s/%s/%s", ui_db_root, dir, leaf);
    if (!text) { unlink(p); return; }
    FILE *f = fopen(p, "w");
    assert(f && fputs(text, f) >= 0 && fclose(f) == 0);
}
#define put(dir, text) put_as(dir, TAGCACHE_FILE_CHANGELOG, text)
#define put_new(dir, text) put_as(dir, TAGCACHE_FILE_CHANGELOG ".new", text)
/* which value the import would read for the track */
static int chosen(void)
{
    char name[2048];
    int fd = ui_changelog_open(name);
    if (fd < 0)
        return -1;
    char buf[512];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    buf[n > 0 ? n : 0] = 0;
    char *v = strstr(buf, "playcount=\"");
    return v ? atoi(v + 11) : 0;
}
#define GEN(n) "## Changelog version 1\n## commit " #n "\nfilename=\"/a.mp3\" playcount=\"" #n "0\" \n## end\n"
int main(int argc, char **argv)
{
    (void)argc;
    snprintf(ui_db_root, sizeof(ui_db_root), "%s", argv[1]);
    snprintf(active.stat.db_path, MAX_PATH, "%s/database_gen1", ui_db_root);
    snprintf(build.stat.db_path, MAX_PATH, "%s/database_gen2", ui_db_root);
    /* nothing anywhere: no input (the journal alone) */
    assert(chosen() == -1);
    /* the root's old export (complete, no commit line) beats nothing:
     * the unreadable-master case keeps its values, not zeros */
    put(".", "## Changelog version 1\nfilename=\"/a.mp3\" playcount=\"5\" \n## end\n");
    assert(chosen() == 5);
    /* a stock export (no end line) loses to a complete one... */
    put("database_gen3", "## Changelog version 1\nfilename=\"/a.mp3\" playcount=\"4\" \n");
    assert(chosen() == 5);
    put(".", NULL);
    assert(chosen() == 4);              /* ... but beats nothing */
    put(".", "## Changelog version 1\nfilename=\"/a.mp3\" playcount=\"5\" \n## end\n");
    /* a stamped export of another generation is newer than the root's */
    put("database_gen0", GEN(3));
    assert(chosen() == 30);
    /* the newest of several; the candidate's own (scratch) never */
    put("database_gen3", GEN(6));
    put("database_gen2", GEN(9));
    assert(chosen() == 60);
    /* a torn newer one (no end line) never wins over a complete one */
    put("database_gen0", "## Changelog version 1\n## commit 8\nfilename=\"/a.mp3\" playcount=\"80\" \n");
    assert(chosen() == 60);
    /* equal commits: the active directory first */
    put("database_gen1", "## Changelog version 1\n## commit 6\nfilename=\"/a.mp3\" playcount=\"61\" \n## end\n");
    assert(chosen() == 61);
    /* the rebuild's own export is the input when it ran */
    ui_candidate_exported = 1;
    assert(chosen() == 90);
    /* R7b-1b: the owed fresh rebuild's export of the active master is the
     * input, whatever stamps other directories carry */
    ui_candidate_exported = 2;
    assert(chosen() == 61);
    /* ... and none when that readable master had nothing to export */
    ui_candidate_exported = 3;
    assert(chosen() == -1);
    ui_candidate_exported = 0;

    /* R7b-1c: with the active library loaded at commit 6, an export above
     * it (an abandoned candidate's, gen0: 8) never beats the active
     * library's own at 6, but beats everything older (the root's) */
    active.stat.ready = true;
    active.master.commitid = 6;
    put("database_gen0", GEN(8));
    assert(chosen() == 61);
    put("database_gen1", NULL);
    put("database_gen3", GEN(4));       /* an older in-line export */
    assert(chosen() == 40);
    put("database_gen3", NULL);
    assert(chosen() == 80);             /* still above the unstamped root */
    /* a format change: the active master did not load (commit 0): the
     * old line's newest export wins (and seeds the new line above it) */
    active.stat.ready = false;
    active.master.commitid = 0;
    put("database_gen3", GEN(7));
    {
        char name[2048];
        assert(ui_changelog_scan(name) == 8);   /* the highest, any rank */
    }
    assert(chosen() == 80);
    /* LOW-7c: a complete .new an export left unrenamed counts (its
     * stamp); an incomplete one never does */
    put("database_gen0", NULL);
    put_new("database_gen3", GEN(8));
    assert(chosen() == 80);
    put_new("database_gen3", "## Changelog version 1\n## commit 12\nfilename=\"/a.mp3\" playcount=\"120\" \n");
    assert(chosen() == 70);
    puts("tagcache changelog: the newest complete changelog of every directory is imported");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="tagcache-changelog-") as temp:
    for d in ("database_gen0", "database_gen1", "database_gen2", "database_gen3"):
        os.mkdir(os.path.join(temp, d))
    c = Path(temp) / "changelog.c"
    c.write_text(harness)
    exe = Path(temp) / "changelog"
    subprocess.run([os.environ.get("HOSTCC", "cc"), "-std=gnu99", "-Wall", "-Wextra",
                    "-Werror", str(c), "-o", str(exe)], check=True)
    subprocess.run([str(exe), temp], check=True)
