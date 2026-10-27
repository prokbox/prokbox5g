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

# The audio-buffer floor gate (Phase 16-A, #105):
#
#   tests/perf/floor_gate.sh [32|64 ...]      (default: 32 64)
#
# Runs tests/perf/floor-50k.script at 50,000 tracks on the 32 MB
# (build/sim-ui32, --ram=32) and 64 MB (build/sim-ui, the default) shell
# simulators and fails when the audio file buffer is under the floor
# (7,500,000 B / 39,000,000 B, docs/hardware.md budget rules) at any
# worst case. It never touches build/sim-50k/simdisk: each run uses a
# disposable clone of it (cp -p: the database compares file times) (made by tests/perf/lib50k.sh when missing: the
# first run builds the database, minutes). Exit 0 = every floor held.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SOURCE=${FLOOR_SOURCE:-$ROOT/build/sim-50k/simdisk}
RC=0
if [ ! -f "$SOURCE/Synth/.synthetic" ] || [ "$(cat "$SOURCE/Synth/.synthetic")" != 50000 ]; then
    echo "floor gate: building the 50k library (tests/perf/lib50k.sh)" >&2
    "$ROOT/tests/perf/lib50k.sh" >/dev/null 2>&1 || {
        echo "floor gate: FAIL lib50k.sh could not build the 50k library" >&2
        exit 1; }
fi
# #47: a disk sorted by older grouping rules is re-sorted once at its next
# boot. The measured scripts start from a library in the current order, as
# on a device after its first boot (tests/perf/settle50k.sh); the re-sort
# racing PLAY is tests/perf/resort_play_check.sh (run first, below).
# P16: the re-sort (and a post-USB commit) racing PLAY, unsettled
FLOOR_SOURCE=$SOURCE "$ROOT/tests/perf/resort_play_check.sh" ${*:-32 64} || RC=1
SETTLED=$("$ROOT/tests/perf/settle50k.sh" "$SOURCE") || exit 1
if [ "$SETTLED" != "$SOURCE" ]; then
    trap 'rm -rf "$(dirname "$SETTLED")"' EXIT
    SOURCE=$SETTLED
fi
# P12: the module arena at its real size (the largest MOD_ARENA_SIZE in
# apps/ipodui/modload.h across the release and developer builds, which
# tools/modpak.py prints and enforces) replaces the 64 KB placeholder; the
# resident build's Extra-open transient (mod_work_alloc, brief 3.1) is
# FLOOR_EXTRA_WORK (default 32768, the largest Class A work block).
# FLOOR_ARENA overrides; a header with no readable size keeps 65536.
ARENA=${FLOOR_ARENA:-$(python3 "$ROOT/tests/perf/arena_size.py" "$ROOT/apps/ipodui/modload.h")}
EXTRA_WORK=${FLOOR_EXTRA_WORK:-32768}
# The release line's resident BCM image (--bcmfw-merged: apps/video/
# bcmres.c reads bcmvideo.bin once at boot and pins it for good; 214,356 B
# today, capped at BCM_RESIDENT_BYTES): the plain shell simulators have no
# BCM, so a stand-in core_alloc of the cap, in slot 4, taken before the
# first play and held all run.  FLOOR_BCM_RES overrides (0: none).
BCM_RES=${FLOOR_BCM_RES:-$(sed -n 's/^#define BCM_RESIDENT_BYTES[[:space:]]*\(0x[0-9a-fA-F]*\).*/\1/p' \
    "$ROOT/firmware/export/config/ipodvideo.h" | head -1)}
BCM_RES=$((${BCM_RES:-0}))
echo "floor gate: module arena $ARENA B (apps/ipodui/modload.h MOD_ARENA_SIZE), Extra-open transient $EXTRA_WORK B, resident BCM image $BCM_RES B (BCM_RESIDENT_BYTES)"
# FLOOR_PARALLEL=1: the 32 MB and 64 MB halves run at the same time (each has
# its own simulator build and its own disposable clone of the library; their
# output is printed in RAM order afterwards). Default: one after the other.
run_ram() { # RAM -> stdout/stderr as before; exit status = this half's RC
  local RC=0
  RAM=$1
  while :; do
    case "$RAM" in
        32) BUILD=$ROOT/build/sim-ui32; FLOOR=7500000 ;;
        64) BUILD=$ROOT/build/sim-ui;   FLOOR=39000000 ;;
        *) echo "floor gate: RAM must be 32 or 64" >&2; return 2 ;;
    esac
    if [ ! -x "$BUILD/rockboxui" ]; then
        echo "floor gate: FAIL $BUILD/rockboxui missing (build it: --ipodui --ram=$RAM)" >&2
        RC=1; break
    fi
    RUN=$(mktemp -d "$ROOT/build/floor-gate-$RAM.XXXXXX")
    mkdir -p "$RUN/simdisk"
    cp -cpR "$SOURCE/." "$RUN/simdisk/" 2>/dev/null || cp -pR "$SOURCE/." "$RUN/simdisk/"
    OUT=$ROOT/out/shots/floor-$RAM
    sed -e "s/^\(floor_reserve [0-3]\) 65536\$/\1 $ARENA/" \
        -e "s/^\(floor_reserve 3\) 32768\$/\1 $EXTRA_WORK/" \
        "$ROOT/tests/perf/floor-50k.script" >"$RUN/floor-50k.script"
    if [ "$BCM_RES" -gt 0 ]; then
        # before the first wait: the shell is up, nothing has played yet
        awk -v r="$BCM_RES" '!done && /^wait / {print "floor_reserve 4 " r; done=1} {print}' \
            "$RUN/floor-50k.script" >"$RUN/floor-50k.script.t" \
            && mv "$RUN/floor-50k.script.t" "$RUN/floor-50k.script"
        grep -q "^floor_reserve 4 $BCM_RES\$" "$RUN/floor-50k.script" || {
            echo "floor gate: FAIL could not put the resident BCM image into the script" >&2
            RC=1; break; }
    fi
    grep -q "^floor_reserve 0 $ARENA\$" "$RUN/floor-50k.script" || {
        echo "floor gate: FAIL could not put the arena size $ARENA into the script" >&2
        RC=1; break; }
    SIM_BUILD=$BUILD SIMDISK=$RUN/simdisk SHOTS_TIMEOUT=${SHOTS_TIMEOUT:-600} \
        "$ROOT/scripts/shots.sh" "$RUN/floor-50k.script" "$OUT" \
        >"$RUN/shots.out" 2>&1
    ST=$?
    LOG=$OUT/sim.log
    grep -E "sim_script: (floor |floor_watch end|floor_reserve )|expect_floor|ipodui: maint (take|job)" "$LOG" 2>/dev/null \
        | sed 's/^sim_script: //'
    MINLINE=$(grep -E "floor_watch all_cases" "$LOG" | tail -1)
    # 16-0: the resume-after-shrink stand-in (its floor lines are notes: the
    # window's own floor rule is #88's, script ipodui-p16-paused-floor)
    RUN2=$ROOT/out/shots/resume-shrink-$RAM
    rm -rf "$RUN/simdisk2"; mkdir -p "$RUN/simdisk2"
    cp -cpR "$SOURCE/." "$RUN/simdisk2/" 2>/dev/null || cp -pR "$SOURCE/." "$RUN/simdisk2/"
    SIM_BUILD=$BUILD SIMDISK=$RUN/simdisk2 SHOTS_TIMEOUT=${SHOTS_TIMEOUT:-600} \
        "$ROOT/scripts/shots.sh" "$ROOT/tests/perf/resume-shrink-50k.script" "$RUN2" \
        >"$RUN/shots2.out" 2>&1 || true
    grep -E "tap2pcm|sim_script: floor (before|after)" "$RUN2/sim.log" 2>/dev/null \
        | sed 's/^sim_script: //; s/^ipodui: /resume: /'
    # 16-1 (#88, Q11): on the 32 MB unit a commit too big for the floor
    # makes the paused window wait: it refuses and takes nothing
    WAITBAD=
    if [ "$RAM" = 32 ]; then
        RUN3=$ROOT/out/shots/paused-floor-wait-$RAM
        rm -rf "$RUN/simdisk3"; mkdir -p "$RUN/simdisk3"
        cp -cpR "$SOURCE/." "$RUN/simdisk3/" 2>/dev/null || cp -pR "$SOURCE/." "$RUN/simdisk3/"
        SIM_BUILD=$BUILD SIMDISK=$RUN/simdisk3 SHOTS_TIMEOUT=${SHOTS_TIMEOUT:-600} \
            "$ROOT/scripts/shots.sh" "$ROOT/tests/perf/paused-floor-wait-50k.script" "$RUN3" \
            >"$RUN/shots3.out" 2>&1 || WAITBAD="the wait script failed ($RUN/shots3.out)"
        grep -E "maint (job=commit wait|take)|sim_script: floor (shrunk|window)" "$RUN3/sim.log" 2>/dev/null \
            | sed 's/^sim_script: //; s/^ipodui: /wait: /'
        grep -q "maint job=commit wait floor" "$RUN3/sim.log" 2>/dev/null \
            || WAITBAD="${WAITBAD:-no 'maint job=commit wait floor' line}"
        ! grep -q "maint take job=commit" "$RUN3/sim.log" 2>/dev/null \
            || WAITBAD="${WAITBAD:-the window took memory below the floor}"
    fi
    if [ -n "$WAITBAD" ]; then
        echo "floor gate: FAIL ${RAM} MB unit: paused window floor rule: $WAITBAD" >&2
        RC=1
    elif [ $ST -ne 0 ] || [ -z "$MINLINE" ]; then
        echo "floor gate: FAIL ${RAM} MB unit: the floor script failed (exit $ST; $LOG, $RUN/shots.out)" >&2
        tail -5 "$RUN/shots.out" >&2
        RC=1
    else
        echo "floor gate: ok ${RAM} MB unit, all worst cases >= $FLOOR B"
        rm -rf "$RUN"
    fi
    break
  done
  return $RC
}
if [ "${FLOOR_PARALLEL:-0}" = 1 ]; then
    FT=$(mktemp -d "$ROOT/build/floor-par.XXXXXX")
    for RAM_ in ${*:-32 64}; do
        ( run_ram "$RAM_" >"$FT/$RAM_.out" 2>&1; echo $? >"$FT/$RAM_.rc" ) &
    done
    wait
    for RAM_ in ${*:-32 64}; do
        cat "$FT/$RAM_.out"
        [ "$(cat "$FT/$RAM_.rc" 2>/dev/null || echo 1)" = 0 ] || RC=1
    done
    rm -rf "$FT"
else
    for RAM_ in ${*:-32 64}; do run_ram "$RAM_" || RC=1; done
fi
exit $RC
