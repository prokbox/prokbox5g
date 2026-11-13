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


# The stress measurement scripts' settle step (#47; the 50k and 65k disks):
#
#   tests/perf/settle65k.sh SOURCE_SIMDISK [--in-place]
#
# A disk whose tag files predate the current grouping rules (no
# .rockbox/database_order.dat, or one older than UI_GROUPING_VERSION in
# apps/ipodui/sortkey.h) is re-sorted once at its next boot: an empty
# commit and a publication. The measurement scripts (floor_gate.sh,
# commit_peak.sh, snapshot-memory.sh, import65k.sh) measure the steady
# state, as on a device after its first boot, so they settle first:
# one boot of tests/perf/floor-settle.script on a clone (or on the
# disk itself with --in-place). Prints the settled disk's path (SOURCE
# itself when it is current); the clone lives under build/ and the caller
# removes it. SETTLE_BUILD picks the simulator (default build/sim-ui; the
# re-sort does not depend on the unit's RAM). Exit 1 when the boot did not re-sort.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SOURCE=$1
GV=$(sed -n 's/^#define UI_GROUPING_VERSION \([0-9]*\).*/\1/p' "$ROOT/apps/ipodui/sortkey.h")
MARK=.rockbox/database_order.dat
mark() { od -An -tu4 "$1/$MARK" 2>/dev/null | tr -d ' '; }
if [ -z "$GV" ] || [ "$(mark "$SOURCE")" = "$GV" ]; then
    echo "$SOURCE"
    exit 0
fi
SB=${SETTLE_BUILD:-$ROOT/build/sim-ui}
if [ "${2:-}" = --in-place ]; then
    DISK=$SOURCE
    LOG=$ROOT/build/settle65k-inplace.out
else
    DIR=$(mktemp -d "$ROOT/build/settle65k.XXXXXX")
    DISK=$DIR/simdisk
    LOG=$DIR/shots.out
    mkdir -p "$DISK"
    cp -cpR "$SOURCE/." "$DISK/" 2>/dev/null || cp -pR "$SOURCE/." "$DISK/"
fi
SIM_BUILD=$SB SIMDISK=$DISK SHOTS_TIMEOUT=${SHOTS_TIMEOUT:-600} \
    "$ROOT/scripts/shots.sh" "$ROOT/tests/perf/floor-settle.script" \
    "$ROOT/out/shots/floor-settle" >"$LOG" 2>&1
if [ "$(mark "$DISK")" != "$GV" ]; then
    echo "settle65k: FAIL the settle boot did not re-sort $SOURCE ($ROOT/out/shots/floor-settle/sim.log)" >&2
    exit 1
fi
echo "settle65k: stress disk re-sorted once for grouping v$GV (settle boot)" >&2
echo "$DISK"
