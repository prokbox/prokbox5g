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

# Commit peak memory per phase at each unit's stress ceiling (Phase 16-0;
# #229: 32 MB at 50k tracks, 64 MB at 65k):
#   tests/perf/commit_peak.sh [32|64 ...]
# Runs tests/perf/commit-peak.script on a disposable clone of the unit's
# disk (build/sim-50k or build/sim-65k; tests/perf/ensure65k.sh primes a
# stale one) and prints the free heap timeline interleaved with the
# tagcache phase lines. A report, not a gate (the floor is
# tests/perf/floor_gate.sh).
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$ROOT/tests/perf/stress_size.sh"
TMPS=
trap 'for t in $TMPS; do rm -rf "$t"; done' EXIT
for RAM in ${*:-32 64}; do
    case "$RAM" in 32|64) ;; *) exit 2 ;; esac
    # one binary for both units (#204): the unit's RAM is IPODUI_SIM_RAM
    BUILD=$ROOT/build/sim-ui
    export IPODUI_SIM_RAM=$RAM
    SOURCE=$(stress_disk "$RAM")
    "$ROOT/tests/perf/ensure65k.sh" "$SOURCE" || exit 1
    # #47: measure the steady state, not the one-time re-sort (settle65k.sh)
    SETTLED=$("$ROOT/tests/perf/settle65k.sh" "$SOURCE") || exit 1
    if [ "$SETTLED" != "$SOURCE" ]; then
        TMPS="$TMPS $(dirname "$SETTLED")"
        SOURCE=$SETTLED
    fi
    RUN=$(mktemp -d "$ROOT/build/commit-peak-$RAM.XXXXXX")
    mkdir -p "$RUN/simdisk"
    cp -cpR "$SOURCE/." "$RUN/simdisk/" 2>/dev/null || cp -pR "$SOURCE/." "$RUN/simdisk/"
    OUT=$ROOT/out/shots/commit-peak-$RAM
    SIM_BUILD=$BUILD SIMDISK=$RUN/simdisk SHOTS_TIMEOUT=${SHOTS_TIMEOUT:-600} \
        "$ROOT/scripts/shots.sh" "$ROOT/tests/perf/commit-peak.script" "$OUT" \
        >"$RUN/shots.out" 2>&1 || echo "commit peak ${RAM} MB: script failed ($RUN/shots.out)" >&2
    echo "== commit peak, ${RAM} MB unit at $(stress_k "$RAM")k (log $OUT/sim.log)"
    grep -q "^sim: ram $RAM MB" "$OUT/sim.log" \
        || echo "commit peak ${RAM} MB: the simulator did not run as a ${RAM} MB unit ($OUT/sim.log)" >&2
    grep -E "floor_tl|floor_watch commit_peak|tagcache: (scan|probe|commit|numeric|RAM cache)|snapshot stress" \
        "$OUT/sim.log" | sed 's/^sim_script: //'
    rm -rf "$RUN"
done
