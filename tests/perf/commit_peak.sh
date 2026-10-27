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

# Commit peak memory per phase at 50k tracks (Phase 16-0):
#   tests/perf/commit_peak.sh [32|64 ...]
# Runs tests/perf/commit-peak-50k.script on a disposable clone of
# build/sim-50k/simdisk (tests/perf/lib50k.sh first) and prints the free
# heap timeline interleaved with the tagcache phase lines. A report, not
# a gate (the floor is tests/perf/floor_gate.sh).
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SOURCE=$ROOT/build/sim-50k/simdisk
# #47: measure the steady state, not the one-time re-sort (settle50k.sh)
SETTLED=$("$ROOT/tests/perf/settle50k.sh" "$SOURCE") || exit 1
if [ "$SETTLED" != "$SOURCE" ]; then
    trap 'rm -rf "$(dirname "$SETTLED")"' EXIT
    SOURCE=$SETTLED
fi
for RAM in ${*:-32 64}; do
    case "$RAM" in
        32) BUILD=$ROOT/build/sim-ui32 ;;
        64) BUILD=$ROOT/build/sim-ui ;;
        *) exit 2 ;;
    esac
    RUN=$(mktemp -d "$ROOT/build/commit-peak-$RAM.XXXXXX")
    mkdir -p "$RUN/simdisk"
    cp -cpR "$SOURCE/." "$RUN/simdisk/" 2>/dev/null || cp -pR "$SOURCE/." "$RUN/simdisk/"
    OUT=$ROOT/out/shots/commit-peak-$RAM
    SIM_BUILD=$BUILD SIMDISK=$RUN/simdisk SHOTS_TIMEOUT=${SHOTS_TIMEOUT:-600} \
        "$ROOT/scripts/shots.sh" "$ROOT/tests/perf/commit-peak-50k.script" "$OUT" \
        >"$RUN/shots.out" 2>&1 || echo "commit peak ${RAM} MB: script failed ($RUN/shots.out)" >&2
    echo "== commit peak, ${RAM} MB unit (log $OUT/sim.log)"
    grep -E "floor_tl|floor_watch commit_peak|tagcache: (scan|probe|commit|numeric|RAM cache)|snapshot stress" \
        "$OUT/sim.log" | sed 's/^sim_script: //'
    rm -rf "$RUN"
done
