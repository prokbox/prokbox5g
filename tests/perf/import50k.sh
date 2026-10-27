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

# Changelog import timing at 50k tracks in the simulator (Phase 4 second
# review, N1).
#
#   tests/perf/import50k.sh [lines] [--shuffle | --realistic]
#
# Needs build/sim-50k with its database (run tests/perf/lib50k.sh first).
# Writes a changelog of 'lines' entries (default: every track) in index
# order, as the shell exports it (--shuffle: random order, the worst case;
# --realistic: 5% moved to the end in album blocks, 2% deleted files),
# runs tests/perf/import-50k.script and prints the import time (host ms;
# the device estimate is in docs/hardware.md). SIM_BUILD=<dir> runs another
# sim build on the same disk (e.g. one built from older sources, for a
# before/after comparison).
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
B=$ROOT/build/sim-50k
SIM=${SIM_BUILD:-$B}
[ -f "$B/simdisk/.rockbox/database_idx.tcd" ] \
    || { echo "run tests/perf/lib50k.sh first" >&2; exit 2; }
# #47: this script works on the disk itself; re-sort it once first so the
# import is timed against the steady state (tests/perf/settle50k.sh)
"$ROOT/tests/perf/settle50k.sh" "$B/simdisk" --in-place >/dev/null
python3 "$ROOT/tests/perf/make_changelog.py" "$B/simdisk" "$@"
case "$SIM" in /*) ;; *) SIM="$ROOT/$SIM" ;; esac
if [ "$SIM" != "$B" ]; then
    mkdir -p "$SIM-50k"
    ln -sf "$SIM/rockboxui" "$SIM-50k/rockboxui"
    ln -sfn "$B/simdisk" "$SIM-50k/simdisk"
    SIM="$SIM-50k"
fi
SIM_BUILD=$SIM SHOTS_TIMEOUT=${SHOTS_TIMEOUT:-3600} \
    "$ROOT/scripts/shots.sh" "$ROOT/tests/perf/import-50k.script" \
    >/dev/null 2>&1 || true
grep -E "import_changelog|changelog import" \
    "$ROOT/out/shots/import-50k/sim.log"
