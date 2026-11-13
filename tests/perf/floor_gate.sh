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
# Each unit at its own stress ceiling (owner, 2026-11-09, #229): the 32 MB
# unit runs tests/perf/floor-50k.script at 50,000 tracks, the 64 MB unit
# tests/perf/floor-65k.script at 65,000 (tests/perf/stress_size.sh). One
# binary for both (#204): build/sim-ui, the unit's RAM IPODUI_SIM_RAM; a
# run whose log does not say "sim: ram <RAM> MB" fails. It
# fails when the audio file buffer is under the floor (7,500,000 B /
# 39,000,000 B, docs/hardware.md budget rules) at any worst case. It never
# touches build/sim-50k or build/sim-65k's simdisk: each run uses a
# disposable clone (cp -p: the database compares file times); a missing
# disk is made by tests/perf/lib50k.sh / lib65k.sh (the first run builds
# the database, minutes). Exit 0 = every floor held.
#
# FLOOR_K_32 / FLOOR_K_64 (50 or 65) run a unit at the other size, for a
# red run: at 65k the 32 MB unit is under its floor (#229). FLOOR_SOURCE_32
# / FLOOR_SOURCE_64 point a unit at another disk (it needs a db.key).
#
# Two cases per unit (p081-fixture): the STEADY case on a disk this
# firmware primed (tests/perf/ensure65k.sh proves it: key, file formats,
# and a probe boot that changes no database file), and the UPGRADE case,
# "0.8.0 library, first boot": the same floor script (and on the 32 MB
# unit the paused window's wait) on the 0.8.0 release's first build of
# the same library (tests/perf/upgrade080.sh), the current installed files
# copied over it. FLOOR_UPGRADE=0 skips the upgrade case.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
. "$ROOT/tests/perf/stress_size.sh"
RC=0
TMPS=
trap 'for t in $TMPS; do rm -rf "$t"; done' EXIT
# the size of each unit's run: its ceiling, or FLOOR_K_<RAM>
k_for() { eval "_o=\${FLOOR_K_$1:-}"; if [ -n "$_o" ]; then echo "$_o"; else stress_k "$1"; fi; }
for RAM_ in ${*:-32 64}; do
    K=$(k_for "$RAM_") || { echo "floor gate: RAM must be 32 or 64" >&2; exit 2; }
    [ -f "$ROOT/tests/perf/floor-${K}k.script" ] && [ -x "$ROOT/tests/perf/lib${K}k.sh" ] || {
        echo "floor gate: FAIL no floor-${K}k.script / lib${K}k.sh for the ${RAM_} MB unit" >&2; exit 2; }
    eval "SRC=\${FLOOR_SOURCE_$RAM_:-$ROOT/build/sim-${K}k/simdisk}"
    if [ ! -f "$SRC/Synth/.synthetic" ] || [ "$(cat "$SRC/Synth/.synthetic")" != "${K}000" ]; then
        echo "floor gate: building the ${K}k library (tests/perf/lib${K}k.sh)" >&2
        "$ROOT/tests/perf/lib${K}k.sh" >/dev/null 2>&1 || {
            echo "floor gate: FAIL lib${K}k.sh could not build the ${K}k library" >&2
            exit 1; }
    fi
    # the database must be the one this firmware reads (SORTKEY_VERSION and
    # the format defines; tests/perf/ensure65k.sh re-primes a stale one)
    "$ROOT/tests/perf/ensure65k.sh" "$SRC" || {
        echo "floor gate: FAIL the ${K}k database is stale (tests/perf/ensure65k.sh)" >&2
        exit 1; }
    # P16: the re-sort (and a post-USB commit) racing PLAY, unsettled
    FLOOR_SOURCE=$SRC "$ROOT/tests/perf/resort_play_check.sh" "$RAM_" || RC=1
    # #47: a disk sorted by older grouping rules is re-sorted once at its
    # next boot. The measured scripts start from a library in the current
    # order, as on a device after its first boot (tests/perf/settle65k.sh)
    SET=$("$ROOT/tests/perf/settle65k.sh" "$SRC") || exit 1
    [ "$SET" != "$SRC" ] && TMPS="$TMPS $(dirname "$SET")"
    eval "K_$RAM_=$K; SOURCE_$RAM_=\$SET"
    echo "floor gate: ${RAM_} MB unit at ${K}k tracks ($SRC)"
    # p081-fixture: the upgrade case's disk, the 0.8.0 release's first
    # build of the same library (tests/perf/upgrade080.sh; made once)
    if [ "${FLOOR_UPGRADE:-1}" = 1 ]; then
        UP=$("$ROOT/tests/perf/upgrade080.sh" "$K") || {
            echo "floor gate: FAIL no 0.8.0 ${K}k disk for the upgrade case (tests/perf/upgrade080.sh)" >&2
            exit 1; }
        eval "UPSRC_$RAM_=\$UP"
        echo "floor gate: ${RAM_} MB unit upgrade case: 0.8.0 library at ${K}k ($UP)"
    fi
done
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
# plus the overlays' session piece buffer (art_bcm.c art_bcm_ovl_buf_init():
# BCMJOB_DEC_PIECE + XCORE_LINE, taken at the BCM's init and kept, so no
# overlay load takes memory from playing audio), with the BCM image's stand-in
OVL_PIECE=$(sed -n 's/^#define BCMJOB_DEC_PIECE[[:space:]]*\([0-9]*\).*/\1/p' "$ROOT/bcmfw/jobs/bcmjob.h" | head -1)
OVL_BUF=$(( ${FLOOR_OVL_BUF:-$(( ${OVL_PIECE:-0} + 16 ))} ))
[ "$BCM_RES" -gt 0 ] && [ "${OVL_PIECE:-0}" -gt 0 ] || [ -n "${FLOOR_OVL_BUF:-}" ] \
    || { echo "floor gate: FAIL could not read BCMJOB_DEC_PIECE (bcmfw/jobs/bcmjob.h)" >&2; exit 2; }
[ "$BCM_RES" -gt 0 ] && BCM_RES=$((BCM_RES + OVL_BUF))
echo "floor gate: module arena $ARENA B (apps/ipodui/modload.h MOD_ARENA_SIZE), Extra-open transient $EXTRA_WORK B, BCM $BCM_RES B (BCM_RESIDENT_BYTES + the overlay session buffer $OVL_BUF B)"
# FLOOR_PARALLEL=1: the 32 MB and 64 MB halves run at the same time (each has
# its own simulator build and its own disposable clone of the library; their
# output is printed in RAM order afterwards). Default: one after the other.
run_ram() { # RAM -> stdout/stderr as before; exit status = this half's RC
  local RC=0
  RAM=$1
  while :; do
    case "$RAM" in
        32) FLOOR=7500000 ;;
        64) FLOOR=39000000 ;;
        *) echo "floor gate: RAM must be 32 or 64" >&2; return 2 ;;
    esac
    BUILD=$ROOT/build/sim-ui
    export IPODUI_SIM_RAM=$RAM
    eval "K=\$K_$RAM; SOURCE=\$SOURCE_$RAM"
    if [ ! -x "$BUILD/rockboxui" ]; then
        echo "floor gate: FAIL $BUILD/rockboxui missing (build it: --ipodui)" >&2
        RC=1; break
    fi
    RUN=$(mktemp -d "$ROOT/build/floor-gate-$RAM.XXXXXX")
    mkdir -p "$RUN/simdisk"
    cp -cpR "$SOURCE/." "$RUN/simdisk/" 2>/dev/null || cp -pR "$SOURCE/." "$RUN/simdisk/"
    OUT=$ROOT/out/shots/floor-$RAM
    sed -e "s/^\(floor_reserve [0-3]\) 65536\$/\1 $ARENA/" \
        -e "s/^\(floor_reserve 3\) 32768\$/\1 $EXTRA_WORK/" \
        "$ROOT/tests/perf/floor-${K}k.script" >"$RUN/floor-${K}k.script"
    if [ "$BCM_RES" -gt 0 ]; then
        # before the first wait: the shell is up, nothing has played yet
        awk -v r="$BCM_RES" '!done && /^wait / {print "floor_reserve 4 " r; done=1} {print}' \
            "$RUN/floor-${K}k.script" >"$RUN/floor-${K}k.script.t" \
            && mv "$RUN/floor-${K}k.script.t" "$RUN/floor-${K}k.script"
        grep -q "^floor_reserve 4 $BCM_RES\$" "$RUN/floor-${K}k.script" || {
            echo "floor gate: FAIL could not put the resident BCM image into the script" >&2
            RC=1; break; }
    fi
    grep -q "^floor_reserve 0 $ARENA\$" "$RUN/floor-${K}k.script" || {
        echo "floor gate: FAIL could not put the arena size $ARENA into the script" >&2
        RC=1; break; }
    SIM_BUILD=$BUILD SIMDISK=$RUN/simdisk SHOTS_TIMEOUT=${SHOTS_TIMEOUT:-600} \
        "$ROOT/scripts/shots.sh" "$RUN/floor-${K}k.script" "$OUT" \
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
        "$ROOT/scripts/shots.sh" "$ROOT/tests/perf/resume-shrink.script" "$RUN2" \
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
    if [ "$RAM" = 64 ] && [ $ST -eq 0 ]; then
        if W_=$(window309 "$LOG" "$FLOOR"); then
            echo "floor gate: 64 MB $W_"
        else
            WAITBAD="${WAITBAD:-$W_}"
        fi
    fi
    # the run must have been the unit it claims (IPODUI_SIM_RAM): every
    # boot's "sim: ram" line, and the floor lines' ram_mb
    RAMBAD=
    LOGS_="$LOG $RUN2/sim.log"
    [ "$RAM" = 32 ] && LOGS_="$LOGS_ $RUN3/sim.log"
    for L_ in $LOGS_; do
        R_=$(sed -n 's/^sim: ram \([0-9]*\) MB$/\1/p' "$L_" 2>/dev/null | sort -u | tr '\n' ' ')
        [ "$R_" = "$RAM " ] \
            || RAMBAD="${RAMBAD:-$L_ ran as '${R_:-no sim: ram line}', not a ${RAM} MB unit}"
    done
    grep -E "ram_mb=[0-9]+" "$LOG" 2>/dev/null | grep -vq "ram_mb=$RAM " \
        && RAMBAD="${RAMBAD:-floor lines with another ram_mb than $RAM ($LOG)}"
    if [ -n "$RAMBAD" ]; then
        echo "floor gate: FAIL ${RAM} MB unit: $RAMBAD (IPODUI_SIM_RAM)" >&2
        RC=1
    elif [ -n "$WAITBAD" ]; then
        echo "floor gate: FAIL ${RAM} MB unit: paused window floor rule: $WAITBAD" >&2
        RC=1
    elif [ $ST -ne 0 ] || [ -z "$MINLINE" ]; then
        echo "floor gate: FAIL ${RAM} MB unit at ${K}k: the floor script failed (exit $ST; $LOG, $RUN/shots.out)" >&2
        tail -5 "$RUN/shots.out" >&2
        RC=1
    else
        echo "floor gate: ok ${RAM} MB unit at ${K}k, all worst cases >= $FLOOR B"
    fi
    # p081-fixture: the upgrade case, "0.8.0 library, first boot of this
    # firmware" (every user's first 0.8.1 boot): the same worst cases (and
    # on the 32 MB unit the paused window's wait) on a clone of the 0.8.0
    # disk, whose first boot here rewrites the library file, publishes a
    # new generation and writes the first boot stamp while they run. The
    # current installed files go over the clone's (an upgrade replaces them
    # and keeps the database).
    if [ "${FLOOR_UPGRADE:-1}" = 1 ]; then
        eval "UPSRC=\$UPSRC_$RAM"
        if ! upgrade_case "$UPSRC"; then
            RC=1
        else
            echo "floor gate: ok ${RAM} MB unit upgrade case (0.8.0 library, first boot) at ${K}k, all worst cases >= $FLOOR B"
        fi
    fi
    [ $RC -eq 0 ] && rm -rf "$RUN"
    break
  done
  return $RC
}
# #309, the 64 MB unit's paused window (floor-65k.script): between
# "floor before_window" and "floor window_commit" the commit is first
# refused for the floor, retried exactly once after the regrow, and taken
# after that retry; the buffer right after it clears the floor. Prints
# the window's line with its margin; 0 = the rule held, else the reason
# (one line).
window309() { # LOG FLOOR
    awk -v floor="$2" '
        /sim_script: floor before_window / { w = 1; next }
        w && /ipodui: maint job=commit wait floor/ { if (!wt) wt = ++n }
        w && /ipodui: maint retry after regrow job=commit/ { retries++; if (!rt) rt = ++n }
        w && /ipodui: maint take job=commit ok=1/ { if (!tk) tk = ++n }
        w && /sim_script: floor window_commit / {
            for (i = 1; i <= NF; i++) if ($i ~ /^audio=/) a = substr($i, 7) + 0
            done = 1; exit }
        END {
            if (!done) { print "no \"floor window_commit\" after \"floor before_window\""; exit 1 }
            if (!wt) { print "no \"maint job=commit wait floor\" in the window (never refused: the #309 path did not run)"; exit 1 }
            if (retries != 1) { print "\"maint retry after regrow job=commit\" " retries + 0 " times in the window, not once"; exit 1 }
            if (!(wt < rt && rt < tk)) { print "the window order is not: wait floor, retry after regrow, commit take"; exit 1 }
            if (a < floor) { print "window_commit audio=" a " B under the floor " floor " B"; exit 1 }
            printf "paused window (#309): refused, retried once after the regrow, committed paused; audio=%d B, floor=%d B, margin=%d B\n", a, floor, a - floor
        }' "$1"
}
# a clone of the 0.8.0 disk with this build's installed files
upgrade_clone() { # SOURCE DEST
    rm -rf "$2"; mkdir -p "$2"
    cp -cpR "$1/." "$2/" 2>/dev/null || cp -pR "$1/." "$2/"
    for it in codecs rocks fonts langs themes wps icons backdrops eqs codepages docs \
              viewers.config tagnavi.config rockbox-info.txt; do
        [ -e "$BUILD/simdisk/.rockbox/$it" ] || continue
        rm -rf "$2/.rockbox/$it"
        cp -cpR "$BUILD/simdisk/.rockbox/$it" "$2/.rockbox/$it" 2>/dev/null \
            || cp -pR "$BUILD/simdisk/.rockbox/$it" "$2/.rockbox/$it"
    done
    [ -d "$2/.rockbox/codecs" ] && [ -f "$2/.rockbox/rockbox-info.txt" ]
}
# the upgrade case's copy of a script (into $RUN, as deep as tests/perf/,
# so its ../../ paths still resolve): its first wait_db becomes
# wait_db_settled. On the 0.8.0 disk wait_db returns when the first boot's
# commit ends, before its candidate publishes; a PLAY in that gap (a slow
# or loaded host: the scan ends after the script's opening wait) makes the
# candidate yield its RAM cache and leaves the session's buffer sized
# with both caches held, a different case (gate run relauto, 2026-11-12).
# The measured case starts after the boot's publication, every run.
upgrade_script() { # SCRIPT DEST
    sed '1,/^wait_db /s/^wait_db /wait_db_settled /' "$1" >"$2" \
        && grep -q '^wait_db_settled ' "$2" && ! grep -q '^wait_db ' "$2" \
        || { echo "floor gate: FAIL could not make $1 wait for the boot's publication (no single wait_db line?)" >&2; return 1; }
}
# 0 = the boot's publication freed the retired generation's RAM cache
# before the first play (p081-floor); else the reason
upgrade_order() { # LOG
    awk '/ipodui: db retired ram free gen=/ { if (!play) { ok = 1; exit } }
         /ipodui: lib play / { play = 1 }
         END { exit ok ? 0 : 1 }' "$1"
}
upgrade_case() { # SOURCE; uses RAM K BUILD RUN FLOOR; 0 = the floor held
    local UB ST4 OUT4 LOG4 MIN4
    UB=
    upgrade_clone "$1" "$RUN/simdisk-up" \
        || { echo "floor gate: FAIL ${RAM} MB upgrade case: could not install this build on the 0.8.0 clone" >&2; return 1; }
    [ ! -f "$RUN/simdisk-up/.rockbox/boot_stamp.dat" ] \
        || { echo "floor gate: FAIL ${RAM} MB upgrade case: the 0.8.0 disk has a boot stamp (not 0.8.0's first build?)" >&2; return 1; }
    upgrade_script "$RUN/floor-${K}k.script" "$RUN/floor-${K}k-up.script" || return 1
    OUT4=$ROOT/out/shots/floor-upgrade-$RAM
    SIM_BUILD=$BUILD SIMDISK=$RUN/simdisk-up SHOTS_TIMEOUT=${SHOTS_TIMEOUT:-600} \
        "$ROOT/scripts/shots.sh" "$RUN/floor-${K}k-up.script" "$OUT4" \
        >"$RUN/shots-up.out" 2>&1
    ST4=$?
    LOG4=$OUT4/sim.log
    upgrade_order "$LOG4" \
        || UB="no 'db retired ram free' before the first 'lib play' in $LOG4 (the boot's publication did not free 0.8.0's RAM cache before playback)"
    grep -E "sim_script: (floor |floor_watch end|floor_reserve )|expect_floor|ipodui: maint (take|job)" "$LOG4" 2>/dev/null \
        | sed 's/^sim_script: /upgrade: /'
    MIN4=$(grep -E "floor_watch all_cases" "$LOG4" | tail -1)
    if [ "$RAM" = 64 ] && [ "$ST4" -eq 0 ]; then
        if W_=$(window309 "$LOG4" "$FLOOR"); then
            echo "floor gate: 64 MB upgrade $W_"
        else
            UB="${UB:-paused window floor rule: $W_}"
        fi
    fi
    # the case must really be an upgrade: this firmware published over
    # 0.8.0's library in the run
    [ -f "$RUN/simdisk-up/.rockbox/boot_stamp.dat" ] \
        || UB="no boot stamp after the first boot (not an upgrade run?)"
    if [ "$RAM" = 32 ]; then
        upgrade_clone "$1" "$RUN/simdisk-up3" \
            || UB="${UB:-could not install this build on the second 0.8.0 clone}"
        upgrade_script "$ROOT/tests/perf/paused-floor-wait-50k.script" \
            "$RUN/paused-floor-wait-50k-up.script" || return 1
        SIM_BUILD=$BUILD SIMDISK=$RUN/simdisk-up3 SHOTS_TIMEOUT=${SHOTS_TIMEOUT:-600} \
            "$ROOT/scripts/shots.sh" "$RUN/paused-floor-wait-50k-up.script" \
            "$ROOT/out/shots/paused-floor-wait-upgrade-$RAM" >"$RUN/shots-up3.out" 2>&1 \
            || UB="${UB:-the wait script failed ($RUN/shots-up3.out)}"
        L5=$ROOT/out/shots/paused-floor-wait-upgrade-$RAM/sim.log
        upgrade_order "$L5" \
            || UB="${UB:-no 'db retired ram free' before the first 'lib play' in $L5}"
        grep -E "maint (job=commit wait|take)|sim_script: floor (shrunk|window)" "$L5" 2>/dev/null \
            | sed 's/^sim_script: //; s/^ipodui: /upgrade wait: /'
        grep -q "maint job=commit wait floor" "$L5" 2>/dev/null \
            || UB="${UB:-no 'maint job=commit wait floor' line}"
        ! grep -q "maint take job=commit" "$L5" 2>/dev/null \
            || UB="${UB:-the window took memory below the floor}"
    fi
    if [ $ST4 -ne 0 ] || [ -z "$MIN4" ]; then
        echo "floor gate: FAIL ${RAM} MB unit upgrade case (0.8.0 library, first boot) at ${K}k: the floor script failed (exit $ST4; $LOG4, $RUN/shots-up.out)" >&2
        tail -5 "$RUN/shots-up.out" >&2
        return 1
    fi
    # an upgrade costs the session no audio buffer against the steady case
    # (p081-floor: a library published at boot kept the retired
    # generation's RAM cache, 6.9 MB at 50k, all session; 7.9 MB at the
    # 32 MB paused window, still above the floor, so the floor alone does
    # not see it). UPGRADE_SLACK B of noise allowed (default 256 KiB).
    SL=${UPGRADE_SLACK:-262144}
    num() { sed -n "s/.* $1=\([0-9][0-9]*\).*/\1/p" | tail -1; }
    S_MIN=$(grep -E "floor_watch all_cases" "$LOG" | num min)
    U_MIN=$(printf '%s\n' "$MIN4" | num min)
    if [ -z "$S_MIN" ] || [ -z "$U_MIN" ]; then
        UB="${UB:-no all_cases minimum to compare (steady $S_MIN, upgrade $U_MIN)}"
    elif [ "$U_MIN" -lt $((S_MIN - SL)) ]; then
        UB="${UB:-all_cases minimum $U_MIN B, $((S_MIN - U_MIN)) B under the steady case $S_MIN B}"
    fi
    echo "floor gate: ${RAM} MB upgrade vs steady: all_cases min $U_MIN / $S_MIN B"
    if [ "$RAM" = 32 ]; then
        S_W=$(grep "maint job=commit wait floor" "$ROOT/out/shots/paused-floor-wait-$RAM/sim.log" 2>/dev/null | num audio)
        U_W=$(grep "maint job=commit wait floor" "$L5" 2>/dev/null | num audio)
        if [ -z "$S_W" ] || [ -z "$U_W" ]; then
            UB="${UB:-no paused-window audio to compare (steady $S_W, upgrade $U_W)}"
        elif [ "$U_W" -lt $((S_W - SL)) ]; then
            UB="${UB:-paused window audio $U_W B, $((S_W - U_W)) B under the steady case $S_W B}"
        fi
        echo "floor gate: ${RAM} MB upgrade vs steady: paused window audio $U_W / $S_W B"
    fi
    if [ -n "$UB" ]; then
        echo "floor gate: FAIL ${RAM} MB unit upgrade case (0.8.0 library, first boot): $UB" >&2
        return 1
    fi
    return 0
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
