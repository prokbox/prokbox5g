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

# #47: the one-time re-sort of an unchanged library (tagcache.c
# ui_resort_owed()). A copy of build/sim-ui's disk without the marker
# (.rockbox/database_order.dat) boots three times (tests/perf/resort-once.script):
# the first boot must log "resort owed" and "resort done ok=1" once; later
# boots neither, with no commit, and the third trusts its stamp.
#
#   tests/perf/resort_once_check.sh
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
UI=$ROOT/build/sim-ui
B=$ROOT/build/sim-resort
[ -x "$UI/rockboxui" ] || { echo "build build/sim-ui first" >&2; exit 2; }
rm -rf "$B"
mkdir -p "$B"
ln -sf "$UI/rockboxui" "$B/rockboxui"
[ -f "$UI/UI256.bmp" ] && ln -sf "$UI/UI256.bmp" "$B/UI256.bmp"
rsync -a "$UI/simdisk/" "$B/simdisk/"
rm -f "$B/simdisk/.rockbox/database_order.dat"
SIM_BUILD=$B "$ROOT/scripts/shots.sh" "$ROOT/tests/perf/resort-once.script" \
    >/dev/null 2>&1 || true
L=$ROOT/out/shots/resort-once/sim.log
# the log of each boot: up to and after the "reboot" marker line
first=$(sed -n '1,/sim_script: reboot/p' "$L")
later=$(sed -n '/sim_script: reboot/,$p' "$L")
o1=$(echo "$first" | grep -c "db resort owed" || true)
d1=$(echo "$first" | grep -c "db resort done ok=1" || true)
o2=$(echo "$later" | grep -c "db resort" || true)
c2=$(echo "$later" | grep -c "db commit" || true)
w3=$(echo "$later" | grep -c "stamp walk=0 why=trust" || true)
if [ "$o1" = 1 ] && [ "$d1" = 1 ] && [ "$o2" = 0 ] && [ "$c2" = 0 ] \
   && [ "$w3" -ge 1 ] && [ -f "$B/simdisk/.rockbox/database_order.dat" ]; then
    echo "PASS re-sorted once; later boots: no re-sort, no commit, then stamp trusted"
    rm -rf "$B"
else
    echo "FAIL owed=$o1 done=$d1 later=$o2 commits=$c2 trust=$w3 ($L)"
    exit 1
fi
