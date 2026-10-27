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

"""Compile the production tagcache journal caller over actual journal files.

Only platform services are replaced: the queue and compaction implementation
are extracted verbatim. This is not a scheduling or device timing model.
"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / "apps/tagcache.c").read_text()
caller = source[source.index("static bool ui_journal_ready("):
                source.index("\n#endif\n\nstatic void command_queue_sync_callback", source.index("static bool ui_journal_ready("))]
queue = source[source.index("#ifdef UI_COMMIT\n/* A numeric update's RAM write"):
               source.index("\nlong tagcache_increase_serial", source.index("static void queue_command(int cmd,"))]


def function(signature):
    start = source.index(signature + "\n{")
    depth, end = 1, source.index("{", start) + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


# R-H1: the boot replay's per-row skip and its completion, verbatim
replay = (function("static enum dbjournal_result ui_stats_apply(void *opaque, const char *filename,\n    uint32_t tag, int32_t absolute, uint64_t sequence)")
          + function("static void ui_stats_replay_end(bool done, int masterfd)")
          + function("static bool ui_stats_replay_boot(struct dbjournal *snapshot, bool ok,\n                                 long loaded)"))
with tempfile.TemporaryDirectory(prefix="journal-recovery-") as temp:
    out = Path(temp)
    (out / "recovery_actual.inc").write_text(caller + "\n" + queue)
    (out / "replay_actual.inc").write_text(replay)
    executable = out / "recovery"
    # the safe replace (#91), its renames through the journal's fault hooks
    safefile = out / "safefile.o"
    subprocess.run([os.environ.get("HOSTCC", "cc"), "-std=gnu99", "-O1",
                    "-g", "-Wall", "-Wextra", "-Werror", "-DSAFEFILE_HOST",
                    "-Drename=safefile_test_rename", "-c",
                    "apps/ipodui/safefile.c", "-o", str(safefile)],
                    cwd=root, check=True)
    sanitizer = (["-fsanitize=undefined", "-fno-sanitize-recover=all"]
                 if os.environ.get("JOURNAL_RECOVERY_UBSAN") == "1" else [])
    subprocess.run([os.environ.get("HOSTCC", "cc"), "-std=gnu99", "-O1",
                    "-g", "-Wall", "-Wextra", "-Werror", "-DUI_COMMIT",
                    "-DDBJOURNAL_HOST", "-DDBJOURNAL_MAX_BYTES=4096",
                    "-I" + temp, "-Iapps/ipodui",
                    *sanitizer,
                    "tests/dbjournal/test_recovery.c", "apps/ipodui/dbjournal.c",
                    str(safefile),
                    "-o", str(executable)], cwd=root, check=True)
    subprocess.run([str(executable), temp], cwd=root, check=True)
    big = out / "recovery_big"
    subprocess.run([os.environ.get("HOSTCC", "cc"), "-std=gnu99", "-O1",
                    "-g", "-Wall", "-Wextra", "-Werror", "-DUI_COMMIT",
                    "-DDBJOURNAL_HOST", "-DDBJOURNAL_MAX_BYTES=1048576",
                    "-DRECOVERY_BIG", "-I" + temp, "-Iapps/ipodui",
                    *sanitizer,
                    "tests/dbjournal/test_recovery.c", "apps/ipodui/dbjournal.c",
                    str(safefile),
                    "-o", str(big)], cwd=root, check=True)
    for leaf in ("database_stats.jnl", "database_stats.prev", "database_stats.tmp"):
        try:
            os.unlink(os.path.join(temp, leaf))
        except FileNotFoundError:
            pass
    subprocess.run([str(big), temp], cwd=root, check=True)
