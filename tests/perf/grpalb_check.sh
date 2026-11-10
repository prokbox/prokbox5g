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

# #230: every composer's and every genre's precomputed album range against
# the scan, in the simulator (library.c lib_artalb_check(),
# PROKBOX_ARTALB_CHECK), and the open timing line (lib_openlog.h).
#
#   tests/perf/grpalb_check.sh          the collision fixture
#   tests/perf/grpalb_check.sh 50k      also build/sim-50k (tests/perf/
#                                       lib50k.sh made it): the check, then
#                                       a timing run without it
#
# 1. The collision fixture (tests/perf/artalb_fixture.py, its own simdisk in
#    build/sim-artalb): case and accent variants of composers and genres,
#    names sharing a sort key, untagged tracks, albums split between
#    composers and genres.
# Each run must log "lib cmpalb check composers=N" and "lib genalb check
# genres=N" with N > 0, slices = N and bad=0, both "lib open albums" from
# src=slice, no "unassigned" or "no composers/genres albums" line from the
# precompute, and timing lines ("lib <ms>ms n=<rows> <cat>[/<filter>]
# <src>") of at most 54 characters. Prints PASS or FAIL lines and the
# counts compared; exits 1 on any FAIL.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
UI=$ROOT/build/sim-ui
B=$ROOT/build/sim-artalb
[ -x "$UI/rockboxui" ] || { echo "build build/sim-ui first" >&2; exit 2; }
[ -d "$UI/simdisk/.rockbox/codecs" ] || make -C "$UI" install >/dev/null
fail=0

group() {   # log, check name (cmpalb|genalb), word (composers|genres), label
    line=$(grep "lib $2 check $3=" "$1" | tail -1)
    g=$(echo "$line" | sed -n "s/.* $3=\([0-9]*\).*/\1/p")
    a=$(echo "$line" | sed -n 's/.*albums=\([0-9]*\).*/\1/p')
    s=$(echo "$line" | sed -n 's/.*slices=\([0-9]*\).*/\1/p')
    b=$(echo "$line" | sed -n 's/.*bad=\([0-9]*\).*/\1/p')
    if [ -n "$g" ] && [ "$g" -gt 0 ] && [ "$g" = "$s" ] && [ "$b" = 0 ]; then
        echo "PASS $4: $g $3 compared ($a album rows), every one from its range, bad=0"
    else
        echo "FAIL $4: '${line:-no $2 check line}' (want $3 > 0, slices = $3, bad=0)"
        fail=1
    fi
}

opens() {   # log, label, want-check (1: the check ran in this boot)
    sl=$(grep -c "lib open albums .*src=slice" "$1" || true)
    sc=$(grep -c "lib open albums .*src=scan" "$1" || true)
    un=$(grep -cE "unassigned|lib precompute no (composers|genres|group) albums" "$1" || true)
    if [ "$sl" -ge 2 ] && [ "$sc" = 0 ] && [ "$un" = 0 ]; then
        echo "PASS $2: $sl album opens from src=slice, none scanned"
    else
        echo "FAIL $2: slice=$sl scan=$sc precompute-misses=$un (want >= 2, 0, 0)"
        fail=1
    fi
    grep "lib open albums" "$1" | sed 's/^ipodui: /  /'
    # the device's timing line: numbers first, at most 54 characters
    tl=$(grep -E "^ipodui: lib [0-9]+ms n=[0-9]+ [a-z]+(/[a-z]+)? (file|slice|scan)$" "$1" \
         | sed 's/^ipodui: //' || true)
    n=$(printf '%s\n' "$tl" | grep -c . || true)
    long=$(printf '%s\n' "$tl" | awk 'length($0) > 54' | wc -l | tr -d ' ')
    if [ "$n" -gt 0 ] && [ "$long" = 0 ]; then
        echo "PASS $2: $n timing lines, none over 54 characters"
        printf '%s\n' "$tl" | sed 's/^/  /'
    else
        echo "FAIL $2: $n timing lines, $long over 54 characters"
        fail=1
    fi
}

mkdir -p "$B/simdisk"
ln -sf "$UI/rockboxui" "$B/rockboxui"
[ -f "$UI/UI256.bmp" ] && ln -sf "$UI/UI256.bmp" "$B/UI256.bmp"
rsync -a --exclude 'database*' "$UI/simdisk/.rockbox/" "$B/simdisk/.rockbox/"
python3 "$ROOT/tests/perf/artalb_fixture.py" "$B/simdisk" >/dev/null
PROKBOX_ARTALB_CHECK=1 SIM_BUILD=$B "$ROOT/scripts/shots.sh" \
    "$ROOT/tests/perf/grpalb-slices.script" >/dev/null 2>&1 || true
L=$ROOT/out/shots/grpalb-slices/sim.log
group "$L" cmpalb composers "collision fixture"
group "$L" genalb genres "collision fixture"
opens "$L" "collision fixture"

if [ "${1:-}" = 50k ]; then
    B=$ROOT/build/sim-50k
    [ -d "$B/simdisk/Synth" ] || { echo "FAIL 50k: no build/sim-50k (tests/perf/lib50k.sh)"; exit 1; }
    ln -sf "$UI/rockboxui" "$B/rockboxui"
    [ -f "$UI/UI256.bmp" ] && ln -sf "$UI/UI256.bmp" "$B/UI256.bmp"
    L=$ROOT/out/shots/grpalb-50k/sim.log
    PROKBOX_ARTALB_CHECK=1 SIM_BUILD=$B SHOTS_TIMEOUT=${SHOTS_TIMEOUT:-1800} \
        "$ROOT/scripts/shots.sh" "$ROOT/tests/perf/grpalb-50k.script" \
        >/dev/null 2>&1 || true
    cp "$L" "$ROOT/out/shots/grpalb-50k/sim-check.log"
    group "$L" cmpalb composers "50k"
    group "$L" genalb genres "50k"
    # the timings, without the check's extra opens
    SIM_BUILD=$B SHOTS_TIMEOUT=${SHOTS_TIMEOUT:-1800} \
        "$ROOT/scripts/shots.sh" "$ROOT/tests/perf/grpalb-50k.script" \
        >/dev/null 2>&1 || true
    opens "$L" "50k timing"
    grep -E "lib precompute (done|group albums)" "$ROOT/out/shots/grpalb-50k/sim-check.log" \
        | sed 's/^ipodui: /  check run: /' || true
fi
exit $fail
