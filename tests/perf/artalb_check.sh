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

# #47: every artist's precomputed album range against the scan, in the
# simulator (library.c lib_artalb_check(), PROKBOX_ARTALB_CHECK).
#
#   tests/perf/artalb_check.sh
#
# 1. The test library (build/sim-ui, tests/ui/ipodui-p16-artist-open.script).
# 2. The collision fixture (tests/perf/artalb_fixture.py, its own simdisk in
#    build/sim-artalb): accented and case-variant artists, a run of four
#    Artists rows on one sort key, Extended-B, Greek and Cyrillic case pairs.
# Each run must log "lib artalb check" with slices = artists and bad=0,
# every "lib open albums" from src=slice, and no "unassigned" line from
# the precompute. Prints PASS or FAIL lines; exits 1 on any FAIL.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
UI=$ROOT/build/sim-ui
B=$ROOT/build/sim-artalb
[ -x "$UI/rockboxui" ] || { echo "build build/sim-ui first" >&2; exit 2; }
[ -d "$UI/simdisk/.rockbox/codecs" ] || make -C "$UI" install >/dev/null
export PROKBOX_ARTALB_CHECK=1
fail=0

check() {   # log, want-artists (empty: any)
    line=$(grep "lib artalb check" "$1" | tail -1)
    a=$(echo "$line" | sed -n 's/.*artists=\([0-9]*\).*/\1/p')
    s=$(echo "$line" | sed -n 's/.*slices=\([0-9]*\).*/\1/p')
    b=$(echo "$line" | sed -n 's/.*bad=\([0-9]*\).*/\1/p')
    scans=$(grep -c "lib open albums .*src=scan" "$1" || true)
    un=$(grep -c "unassigned" "$1" || true)
    if [ -n "$a" ] && [ "$a" = "$s" ] && [ "$b" = 0 ] && [ "$scans" = 0 ] \
       && [ "$un" = 0 ] && { [ -z "$2" ] || [ "$a" = "$2" ]; }; then
        echo "PASS $3: $line"
    else
        echo "FAIL $3: '${line:-no check line}' scans=$scans unassigned=$un want artists=${2:-any}"
        fail=1
    fi
}

SIM_BUILD=$UI "$ROOT/scripts/shots.sh" \
    "$ROOT/tests/ui/ipodui-p16-artist-open.script" >/dev/null 2>&1 || true
check "$ROOT/out/shots/ipodui-p16-artist-open/sim.log" "" "test library"

mkdir -p "$B/simdisk"
ln -sf "$UI/rockboxui" "$B/rockboxui"
[ -f "$UI/UI256.bmp" ] && ln -sf "$UI/UI256.bmp" "$B/UI256.bmp"
rsync -a --exclude 'database*' "$UI/simdisk/.rockbox/" "$B/simdisk/.rockbox/"
python3 "$ROOT/tests/perf/artalb_fixture.py" "$B/simdisk" >/dev/null
SIM_BUILD=$B "$ROOT/scripts/shots.sh" \
    "$ROOT/tests/perf/artist-slices.script" >/dev/null 2>&1 || true
check "$ROOT/out/shots/artist-slices/sim.log" 10 "collision fixture"
grep -q "lib open artists n=10 " "$ROOT/out/shots/artist-slices/sim.log" \
    && echo "PASS collision fixture: 10 Artists rows" \
    || { echo "FAIL collision fixture: Artists rows ($(grep -o 'lib open artists n=[0-9]*' "$ROOT/out/shots/artist-slices/sim.log" | tail -1))"; fail=1; }
exit $fail
