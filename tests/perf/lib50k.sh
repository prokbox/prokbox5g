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

# Library timings at 50k tracks in the simulator (Phase 4 review, F9).
#
#   tests/perf/lib50k.sh [tracks] [playing]
#
# Makes build/sim-50k: the build/sim-ui simulator (symlinked) with its own
# simdisk holding only the synthetic library of
# tests/perf/synthetic_library.py (default 50,000 tracks), so the test
# library and the goldens of build/sim-ui are untouched. The first run
# builds the database in the simulator (the shell's silent first build;
# minutes), later runs reuse it. Then runs tests/perf/lib-50k.script and
# prints the "lib open" / "lib play" trace lines (us = host microseconds;
# scripts/check_trace.py multiplies by its SIM_FACTOR for a device
# estimate) and the check_trace verdict. Numbers are recorded in the
# header of apps/ipodui/library.c and in docs/hardware.md.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
N=${1:-50000}
UI=$ROOT/build/sim-ui
B=$ROOT/build/sim-50k
[ -x "$UI/rockboxui" ] || { echo "build build/sim-ui first" >&2; exit 2; }
[ -d "$UI/simdisk/.rockbox/codecs" ] || make -C "$UI" install >/dev/null

mkdir -p "$B/simdisk"
ln -sf "$UI/rockboxui" "$B/rockboxui"
[ -f "$UI/UI256.bmp" ] && ln -sf "$UI/UI256.bmp" "$B/UI256.bmp"
# the installed files, refreshed, but not the database
rsync -a --exclude 'database*' "$UI/simdisk/.rockbox/" "$B/simdisk/.rockbox/"
# the device's playlist size (settings_list.c: 2,000 on PP502x; the sim
# defaults to 10,000), so lib_play/lib_shuffle_all build what it would
echo "max files in playlist: 2000" >>"$B/simdisk/.rockbox/config.cfg"
python3 "$ROOT/tests/perf/synthetic_library.py" "$B/simdisk" "$N"

# "playing" (2nd arg): the pool while audio plays (lib-50k-playing.script)
S=lib-50k
[ "${2:-}" = playing ] && S=lib-50k-playing
SIM_BUILD=$B SHOTS_TIMEOUT=${SHOTS_TIMEOUT:-1800} \
    "$ROOT/scripts/shots.sh" "$ROOT/tests/perf/$S.script" >/dev/null
LOG=$ROOT/out/shots/$S/sim.log
grep -E "wait_db|lib open|lib play|RAM cache|lib pool peak|lib alloc" "$LOG" \
    | sed 's/^ipodui: //'
python3 "$ROOT/scripts/check_trace.py" "$LOG" "$ROOT/tests/perf/$S.script" \
    | grep -E "lib open|FAIL" || true
