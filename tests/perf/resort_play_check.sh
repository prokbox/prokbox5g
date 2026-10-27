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


# P16 gate fix: PLAY between a stopped commit and its candidate's
# publication (tests/perf/resort-play-50k.script), on a 50k disk without
# the #47 marker (no settle boot), for each memory size:
#
#   tests/perf/resort_play_check.sh [32|64 ...]     (default: 32 64)
#
# The first boot's re-sort and a post-USB update each commit stopped, PLAY
# follows at once (wait_commit), and PLAY also comes during the re-sort's
# publication (commit_hold 100). Pass: each candidate gives its RAM cache
# back while audio plays ("db cand ram yield", three times; the one in
# the publication with its prepared lookup, "stats=1"), publishes at the
# stop ("db resort done ok=1", generations 2 and 3), and the audio buffer
# stays >= the floor (7,500,000 / 39,000,000 B) throughout.
#
# 32 MB also runs tests/perf/resort-pause-50k.script: the yielded
# candidate in a paused window whose reload does not fit. Pass: no
# master index open in the window beyond one ("expect_db_opens ok"), no
# commit take there, the window's regrow not held off (no refused take
# left owed), published at the stop, the floor kept.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SOURCE=${FLOOR_SOURCE:-$ROOT/build/sim-50k/simdisk}
RC=0
for RAM in ${*:-32 64}; do
    case "$RAM" in
        32) BUILD=$ROOT/build/sim-ui32 ;;
        64) BUILD=$ROOT/build/sim-ui ;;
        *) echo "resort play: RAM must be 32 or 64" >&2; exit 2 ;;
    esac
    RUN=$(mktemp -d "$ROOT/build/resort-play-$RAM.XXXXXX")
    mkdir -p "$RUN/simdisk"
    cp -cpR "$SOURCE/." "$RUN/simdisk/" 2>/dev/null || cp -pR "$SOURCE/." "$RUN/simdisk/"
    rm -f "$RUN/simdisk/.rockbox/database_order.dat"
    OUT=$ROOT/out/shots/resort-play-$RAM
    SIM_BUILD=$BUILD SIMDISK=$RUN/simdisk SHOTS_TIMEOUT=${SHOTS_TIMEOUT:-600} \
        "$ROOT/scripts/shots.sh" "$ROOT/tests/perf/resort-play-50k.script" "$OUT" \
        >"$RUN/shots.out" 2>&1
    ST=$?
    L=$OUT/sim.log
    y=$(grep -c "db cand ram yield" "$L" 2>/dev/null)
    d=$(grep -c "db resort done ok=1" "$L" 2>/dev/null)
    g=$(grep -c "db gen 3 " "$L" 2>/dev/null)
    p=$(sed -n '/commit_release/,$p' "$L" 2>/dev/null | grep -c "db cand ram yield .* stats=1")
    lo=$(grep -c "expect_floor failed" "$L" 2>/dev/null)
    min=$(grep "expect_floor ok min over all_cases" "$L" 2>/dev/null | tail -1)
    if [ $ST -eq 0 ] && [ "$y" -ge 3 ] && [ "$p" -ge 1 ] && [ "$d" = 1 ] \
       && [ "$g" -ge 1 ] && [ "$lo" = 0 ] && [ -n "$min" ]; then
        echo "resort play: ok ${RAM} MB (yields=$y, one in publication; published; ${min#sim_script: })"
        rm -rf "$RUN"
    else
        echo "resort play: FAIL ${RAM} MB: exit=$ST yields=$y publish_yields=$p resort_done=$d gen3=$g low=$lo min='${min}' ($L, $RUN/shots.out)" >&2
        RC=1
    fi
    [ "$RAM" = 32 ] || continue
    RUN=$(mktemp -d "$ROOT/build/resort-pause-$RAM.XXXXXX")
    mkdir -p "$RUN/simdisk"
    cp -cpR "$SOURCE/." "$RUN/simdisk/" 2>/dev/null || cp -pR "$SOURCE/." "$RUN/simdisk/"
    rm -f "$RUN/simdisk/.rockbox/database_order.dat"
    OUT=$ROOT/out/shots/resort-pause-$RAM
    SIM_BUILD=$BUILD SIMDISK=$RUN/simdisk SHOTS_TIMEOUT=${SHOTS_TIMEOUT:-600} \
        "$ROOT/scripts/shots.sh" "$ROOT/tests/perf/resort-pause-50k.script" "$OUT" \
        >"$RUN/shots.out" 2>&1
    ST=$?
    L=$OUT/sim.log
    y=$(grep -c "db cand ram yield" "$L" 2>/dev/null)
    o=$(grep "expect_db_opens ok" "$L" 2>/dev/null | tail -1)
    t=$(sed -n '/db_opens_mark/,/expect_db_opens/p' "$L" 2>/dev/null | grep -c "maint take job=commit")
    r=$(grep -c "maint take job=regrow" "$L" 2>/dev/null)
    d=$(grep -c "db resort done ok=1" "$L" 2>/dev/null)
    lo=$(grep -c "expect_floor failed" "$L" 2>/dev/null)
    min=$(grep "expect_floor ok min over all_cases" "$L" 2>/dev/null | tail -1)
    if [ $ST -eq 0 ] && [ "$y" -ge 1 ] && [ -n "$o" ] && [ "$t" = 0 ] \
       && [ "$r" -ge 1 ] && [ "$d" = 1 ] && [ "$lo" = 0 ] && [ -n "$min" ]; then
        echo "resort pause: ok ${RAM} MB (${o#sim_script: }; ${min#sim_script: })"
        rm -rf "$RUN"
    else
        echo "resort pause: FAIL ${RAM} MB: exit=$ST yields=$y opens='${o}' takes=$t regrows=$r resort_done=$d low=$lo min='${min}' ($L, $RUN/shots.out)" >&2
        RC=1
    fi
done
exit $RC
