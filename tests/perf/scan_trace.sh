#!/bin/sh
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

# Where a no-change library scan spends its time (Phase 16-0, 2026-10-20).
#
#   tests/perf/scan_trace.sh [tracks]       (default 1451, the owner's volume)
#
# Makes build/sim-scantrace: the build/sim-ui simulator with a synthetic
# library of <tracks> (tests/perf/synthetic_library.py), a "cover.jpg" and
# a "._" sidecar of a cover in every album folder (the device volume has
# 418 folders and 3,801 entries for 1,451 tracks), boots it twice and
# prints the second boot's scan trace: the devmode "scantrace" log lines
# of apps/tagcache.c (SCAN_TRACE) for the no-change probe and for a
# no-op update. Per stage: ms (host time in the sim: counts are the
# transferable numbers), storage commands (device only; the sim has no
# ATA layer, so 0), folders, entries, audio files, failed ignore-file
# lookups, filename-table lookups and tag-file reads.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
N=${1:-1451}
UI=$ROOT/build/sim-ui
B=$ROOT/build/sim-scantrace
[ -x "$UI/rockboxui" ] || { echo "build build/sim-ui first" >&2; exit 2; }
[ -d "$UI/simdisk/.rockbox/codecs" ] || make -C "$UI" install >/dev/null
rm -rf "$B"
mkdir -p "$B/simdisk"
ln -sf "$UI/rockboxui" "$B/rockboxui"
[ -f "$UI/UI256.bmp" ] && ln -sf "$UI/UI256.bmp" "$B/UI256.bmp"
rsync -a --exclude 'database*' "$UI/simdisk/.rockbox/" "$B/simdisk/.rockbox/"
python3 "$ROOT/tests/perf/synthetic_library.py" "$B/simdisk" "$N" >/dev/null
python3 - "$B/simdisk/Synth" <<'EOF2'
import os, struct, sys
hdr = struct.pack('>II16sH', 0x00051607, 0x00020000, b'Mac OS X        ', 2)
side = hdr + b'\0' * (4096 - len(hdr))
for dp, dn, fn in os.walk(sys.argv[1]):
    if any(f.endswith('.mp3') for f in fn):
        open(os.path.join(dp, 'cover.jpg'), 'wb').write(b'\xff\xd8' + b'\0' * 2000)
        open(os.path.join(dp, '._cover.jpg'), 'wb').write(side)
EOF2
S=$B/scan_trace.script
cat >"$S" <<'EOF2'
# timeout: 900
wait 3000
wait_db 600000
wait 20000
reboot
wait 3000
wait_db 600000
wait 20000
update_db_wait 600000
wait 3000
shot end
quit
EOF2
rc=0
SIM_BUILD=$B SHOTS_TIMEOUT=900 "$ROOT/scripts/shots.sh" "$S" >"$B/shots.out" 2>&1 || rc=$?
LOG=$ROOT/out/shots/scan_trace/sim.log
echo "== scan trace, $N tracks (shots rc=$rc; log $LOG)"
grep -E "tagcache: (scan|probe) (done|stopped)|scantrace|db scan ms" "$LOG" | sed 's/^ipodui: //' || true
find "$B/simdisk/Synth" -type d | wc -l | sed 's/^ */folders on disk: /'
