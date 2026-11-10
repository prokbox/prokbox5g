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

# Shuffle Songs start time on the 50k library (0.8.1 counter fix):
#
#   tests/perf/shuf_start50k.sh [SIM_BIN_DIR] [OUT_NAME]
#
# Runs tests/perf/shuf-start-50k.script with the simulator binary in
# SIM_BIN_DIR (default build/sim-ui; a saved "before" binary works too)
# on an APFS clone of build/sim-50k's disk (keyed by ensure50k.sh,
# settled by settle50k.sh), and prints each start's "shuf: start songs
# ok ... us= cpu=" line (host microseconds) and the medians. Exit 1 when
# fewer than 5 starts were traced.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BIN=${1:-$ROOT/build/sim-ui}
NAME=${2:-shuf-start-50k}
[ -x "$BIN/rockboxui" ] || { echo "shuf_start50k: no $BIN/rockboxui" >&2; exit 2; }
"$ROOT/tests/perf/ensure50k.sh" || exit 1
SRC=$("$ROOT/tests/perf/settle50k.sh" "$ROOT/build/sim-50k/simdisk" --in-place) || exit 1
DIR=$(mktemp -d "$ROOT/build/shufstart.XXXXXX")
trap 'rm -rf "$DIR"' EXIT
mkdir -p "$DIR/sb/simdisk"
cp "$BIN/rockboxui" "$DIR/sb/rockboxui"
[ -f "$ROOT/build/sim-ui/UI256.bmp" ] && ln -sf "$ROOT/build/sim-ui/UI256.bmp" "$DIR/sb/UI256.bmp"
cp -cpR "$SRC/." "$DIR/sb/simdisk/" 2>/dev/null || cp -pR "$SRC/." "$DIR/sb/simdisk/"
OUT=$ROOT/out/shots/$NAME
SIM_BUILD=$DIR/sb SHOTS_TIMEOUT=${SHOTS_TIMEOUT:-1800} \
    "$ROOT/scripts/shots.sh" "$ROOT/tests/perf/shuf-start-50k.script" "$OUT" \
    >"$DIR/shots.out" 2>&1
LINES=$(grep -o 'shuf: start songs ok.*' "$OUT/sim.log")
N=$(printf '%s\n' "$LINES" | grep -c 'us=')
printf '%s\n' "$LINES"
med() {
    printf '%s\n' "$LINES" | sed -n "s/.* $1=\([0-9]*\).*/\1/p" | sort -n \
        | awk '{a[NR] = $1} END {print a[int((NR + 1) / 2)]}'
}
echo "shuf_start50k: starts=$N median us=$(med us) cpu=$(med cpu) ($BIN)"
[ "$N" -ge 5 ] || { echo "shuf_start50k: FAIL $N starts traced, want 5 ($OUT/sim.log)" >&2; exit 1; }
