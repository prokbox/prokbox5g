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

# Run on cloned, disposable 50k simdisks, never the golden or source disk.
# Build sim-persist32/64 with --ipodui --ram=32/64 before invoking this.
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SOURCE=${SNAPSHOT_SOURCE:-$ROOT/build/sim-50k/simdisk}
[ "$(cat "$SOURCE/Synth/.synthetic")" = 50000 ] || {
    echo 'Run tests/perf/lib50k.sh first (requires a built 50k database).' >&2
    exit 2
}
# #47: measure the steady state, not the one-time re-sort (settle50k.sh)
SETTLED=$("$ROOT/tests/perf/settle50k.sh" "$SOURCE")
if [ "$SETTLED" != "$SOURCE" ]; then
    trap 'rm -rf "$(dirname "$SETTLED")"' EXIT
    SOURCE=$SETTLED
fi
for RAM in ${*:-32 64}; do
    case "$RAM" in 32|64) ;; *) exit 2 ;; esac
    BUILD=$ROOT/build/sim-persist$RAM
    [ -x "$BUILD/rockboxui" ] || exit 2
    RUN=$(mktemp -d "$ROOT/build/snapshot-memory-$RAM.XXXXXX")
    mkdir -p "$RUN/simdisk"
    cp -cR "$SOURCE/." "$RUN/simdisk/"
    SIM_BUILD=$BUILD SIMDISK=$RUN/simdisk SHOTS_TIMEOUT=360 \
        "$ROOT/scripts/shots.sh" "$ROOT/tests/perf/snapshot-50k.script" \
        "$ROOT/out/shots/snapshot-memory-$RAM"
    rg 'snapshot stress|expect_snapshot_stress' \
        "$ROOT/out/shots/snapshot-memory-$RAM/sim.log"
    echo "Retained disposable test disk: $RUN/simdisk"
done
