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

# The art builder's no-change pass at 50k tracks (#90):
#
#   tests/perf/artpass50k.sh [build dir]     (default build/sim-ui)
#
# Clones FLOOR_SOURCE (default build/sim-50k/simdisk, made by
# tests/perf/lib50k.sh) and runs tests/perf/art-pass-50k.script on it;
# prints the pass lines. The clone is removed afterwards.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SOURCE=${FLOOR_SOURCE:-$ROOT/build/sim-50k/simdisk}
BUILD=${1:-$ROOT/build/sim-ui}
[ -d "$SOURCE/Synth" ] || { echo "artpass50k: no 50k disk at $SOURCE" >&2; exit 2; }
RUN=$(mktemp -d "$ROOT/build/artpass50k.XXXXXX")
mkdir -p "$RUN/simdisk"
/bin/cp -cpR "$SOURCE/." "$RUN/simdisk/" 2>/dev/null || cp -pR "$SOURCE/." "$RUN/simdisk/"
OUT=$ROOT/out/shots/art-pass-50k
SIM_BUILD=$BUILD SIMDISK=$RUN/simdisk SHOTS_TIMEOUT=${SHOTS_TIMEOUT:-1800} \
    "$ROOT/scripts/shots.sh" "$ROOT/tests/perf/art-pass-50k.script" "$OUT" \
    >"$RUN/shots.out" 2>&1
ST=$?
grep -E "art build (start|done)|art sig" "$OUT/sim.log" | sed 's/^ipodui: //'
[ $ST -eq 0 ] && rm -rf "$RUN" || echo "artpass50k: script failed ($RUN/shots.out)" >&2
exit $ST
