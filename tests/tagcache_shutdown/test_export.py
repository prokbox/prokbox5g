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

"""#85 round 5 HIGH-1: every changelog export reads the master on disk.

RAM holds the journal's deltas after the boot replay; a rebuild imports the
changelog and its replay adds those deltas again, so an export from RAM
(the idle export, or a kept one when a pre-rebuild export is skipped) would
count them twice. The master on disk never folds a delta (a candidate takes
absolute frames only). This checks the actual export function's source:
no RAM-cache read is left in it.
"""
from pathlib import Path
import re

source = Path("apps/tagcache.c").read_text()


def body_of(signature):
    start = source.index(signature + "\n{")
    depth, end = 0, source.index("{", start)
    while True:
        depth += {"{": 1, "}": -1}.get(source[end], 0)
        end += 1
        if depth == 0:
            return source[start:end]


body = body_of("static int ui_export_to(char *name, bool wait, bool stoppable)")
bad = re.findall(r"tcramcache|ramcache|indices\[|ram\.hdr", body)
assert not bad, "ui_export_to reads RAM: %s" % bad
assert "read_index_entries" in body and "open_master_fd" in body
# A skipped or failed export reports -1 (busy, stopped, I/O); an
# unreadable master 0; complete (empty included) 1.
assert re.search(r"skipped \(busy\)\\n\"\);\s*return -1;", body)
assert "return !ok ? -1 : skipped ? 0 : 1 + !written;" in body

# Round 6 MED-A: a rebuild imports its own export of the master on disk.
# An export that cannot run keeps the active library (the candidate is
# dropped, the rebuild retried under the begin backoff); the candidate's
# old changelog files go first, and the import's copy from the active
# directory never replaces the rebuild's export (the import reads it).
rebuild = source[source.index("            case Q_REBUILD:"):
                 source.index("            case Q_UPDATE:")]
# Round 7 LOW-c: the request stays (ui_rebuild_again) until a rebuild
# begins and its export ran; a begin that fails backs off.
pos = 0
for s in (
    "ui_rebuild_maint = ev.data & 1;",
    "ui_rebuild_again = 2 | ui_rebuild_maint;",
    "if (!ui_candidate_begin())",
    "if (ui_retry_due(UI_RETRY_BEGIN))",
    "ui_retry_later(UI_RETRY_BEGIN, \"rebuild\", -1);",
    "break;",
    "remove_db_file(ui_build, TAGCACHE_FILE_CHANGELOG);",
    "remove_db_file(ui_build, TAGCACHE_FILE_CHANGELOG \".new\");",
    "int exported = ui_export_changelog();",
    "if (exported < 0)",
    "ui_candidate_abandon(exported);",
    "ui_retry_later(UI_RETRY_BEGIN",
    "break;",
    "ui_candidate_exported = exported > 0;",
    "ui_rebuild_again = 0;",
    "remove_files();",
    "tagcache_build();"):
    pos = rebuild.index(s, pos) + len(s)   # in this order
timeout = source[source.index("            case SYS_TIMEOUT:"):]
assert re.search(r"if \(ui_rebuild_again && ui_retry_due\(UI_RETRY_BEGIN\)\)\s*"
                 r"queue_post\(&tagcache_queue, Q_REBUILD, ui_rebuild_again\);", timeout)
assert rebuild.count("ui_rebuild_again = 0;") == 1   # only once begun
# Round 8: the connect-time re-arm keeps the rebuild's maintenance bit
usb = source[source.index("            case SYS_USB_CONNECTED:"):]
usb = usb[:usb.index("usb_acknowledge(")]
assert "ui_rebuild_again = 2 | ui_rebuild_maint;" in usb
source_open = body_of("static int ui_changelog_open(char *name)")
assert source_open.index("if (ui_build == ui_active || ui_candidate_exported == 1)") \
    < source_open.index("ui_changelog_scan(name)")
assert "clfd = ui_changelog_open(buf);" in body_of("bool tagcache_import_changelog(void)")
# Round 7b HIGH-R7: the input choice is tested by test_changelog.py
# Round 7 LOW-a: only a damaged/absent file is "unreadable" (0); I/O is -1
assert body.count("== -2 || errno == ENOENT ? 0 : -1;") == 2
begin = body_of("static bool ui_candidate_begin(void)")
assert "ui_candidate_exported = 0;" in begin

# LOW-5: load_ramcache never frees RAM a pinned reader may still use
load = body_of("static void load_ramcache(void)")
guard = load.index("tcramcache.handle > 0 && ui_build->readers")
assert "ui_reload_pending = ui_drain_force = true;" in load   # LOW-d
assert guard < load.index("core_free(tcramcache.handle)")
print("tagcache export: changelog exports read the master on disk only; "
      "a rebuild imports its own export or waits; no RAM freed under a reader")
