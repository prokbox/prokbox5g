#!/bin/bash
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

# Where a boot's library steps spend their time and storage commands
# (p081-bootload, 2026-11-10).
#
#   tests/perf/boot_trace.sh [tracks] [label]   (default 50000 run)
#
# Boots a clone of build/sim-<k>k/simdisk (a built library: tests/perf/
# lib50k.sh, ensure50k.sh; never booted itself, so every run starts from
# the same files) on build/sim-ui (or BOOT_TRACE_SIM) twice:
#   1. plain: the devmode "boottrace" lines of apps/tagcache.c, per step
#      from the tagcache thread's start to its first RAM load's end
#      (recover, select, init, ..., load, precomp): the wall ms since the
#      start, the step's own thread CPU ms (the host's) and file calls;
#      the "boot window" line; and the sha256 of the RAM image the load
#      made (BOOT_TRACE_RAMIMG: the byte-identity check of two builds'
#      loads; compare the "ramimg" lines);
#   2. traced (IPODUI_FSTRACE + IPODUI_FSTRACE_READS): the session
#      replayed on the firmware's own file.c and fat.c (tests/fatcrash/
#      fsreplay "count", 2048-byte sectors, 16 KiB clusters as the 5.5G):
#      each step's storage commands as the device's file layer issues
#      them (rd_cmds, 512-byte sectors, api_rd).
#   BOOT_TRACE_ENV="A=1 B=2": extra environment for both runs.
# Results: build/boottrace/<tracks>-<label>.txt; exit 1 on a FAIL line.
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
N=${1:-50000}
LABEL=${2:-run}
K=$((N / 1000))
UI=${BOOT_TRACE_SIM:-$ROOT/build/sim-ui}
SRC=$ROOT/build/sim-${K}k/simdisk
B=$ROOT/build/sim-boottrace-$K-$LABEL
OUT=$ROOT/build/boottrace
RES=$OUT/$N-$LABEL.txt
[ -x "$UI/rockboxui" ] || { echo "build build/sim-ui first" >&2; exit 2; }
ls "$SRC"/.rockbox/database_gen*/database_idx.tcd >/dev/null 2>&1 \
    || { echo "no built library under $SRC (tests/perf/lib50k.sh)" >&2; exit 2; }
mkdir -p "$OUT"

# the counter, built as tests/fatcrash/powercut.sh builds fsreplay
FC=$OUT/fatcrash
mkdir -p "$FC/inc"
fs="$ROOT/firmware/common"
for d in firmware/export firmware/include firmware/kernel/include; do
    cp "$ROOT/$d"/*.h "$FC/inc/"
done
cp "$ROOT/tests/fatcrash/shim/"*.h "$FC/inc/"
cc -std=gnu99 -O1 -w -I"$FC/inc" -DMAX_VIRT_SECTOR_SIZE=2048 \
   -DDEFAULT_VIRT_SECTOR_SIZE=2048 -o "$FC/fsreplay" \
   "$ROOT/tests/fatcrash/fsreplay.c" "$fs/fat.c" "$fs/disk_cache.c" \
   "$fs/linked_list.c" "$fs/file.c" "$fs/file_internal.c" \
   "$fs/fileobj_mgr.c" "$fs/dir.c" "$fs/pathfuncs.c" "$fs/rb_namespace.c"

S=$OUT/boot_trace.script
printf '%s\n' "# timeout: 600" "wait 3000" "wait_db 300000" "wait 3000" \
    "shot end" "quit" >"$S"

fresh() {
    rm -rf "$B"
    mkdir -p "$B"
    ln -sf "$UI/rockboxui" "$B/rockboxui"
    if [ -f "$UI/UI256.bmp" ]; then ln -sf "$UI/UI256.bmp" "$B/UI256.bmp"; fi
    # a clone (APFS: no data copied), the build's own files over it
    /bin/cp -cRp "$SRC" "$B/simdisk" 2>/dev/null || cp -Rp "$SRC" "$B/simdisk"
    for f in "$UI"/simdisk/.rockbox/*; do
        case "${f##*/}" in database*|config.cfg) continue ;; esac
        rm -rf "$B/simdisk/.rockbox/${f##*/}"
        cp -Rp "$f" "$B/simdisk/.rockbox/"
    done
}

run() { # name, extra env...
    local name=$1 rc=0
    shift
    fresh
    # shellcheck disable=SC2086
    env $BOOT_TRACE_ENV "$@" SIM_BUILD="$B" SHOTS_TIMEOUT=600 \
        "$ROOT/scripts/shots.sh" "$S" "$OUT/shots-$N-$LABEL-$name" \
        >"$OUT/$N-$LABEL-$name.shots.out" 2>&1 || rc=$?
    cp "$OUT/shots-$N-$LABEL-$name/sim.log" "$OUT/$N-$LABEL-$name.sim.log"
    echo "$rc"
}

{
    echo "== boot trace, $N tracks, label $LABEL," \
         "$(git -C "$ROOT" rev-parse --short HEAD)$(git -C "$ROOT" diff --quiet || echo +dirty)" \
         "${BOOT_TRACE_ENV:+env $BOOT_TRACE_ENV}"
    IMG=$OUT/$N-$LABEL.ramimg
    rm -f "$IMG"
    rc=$(run plain BOOT_TRACE_RAMIMG="$IMG")
    LOG=$OUT/$N-$LABEL-plain.sim.log
    echo "-- plain run (shots rc=$rc)"
    [ "$rc" = 0 ] || echo "FAIL: the plain run's session failed (rc=$rc)"
    grep -E "boottrace|boot window|gen select|gen verify|RAM cache [0-9]" "$LOG" \
        | sed -E 's/^.*(boottrace|boot window|gen select|gen verify|RAM cache)/\1/' || true
    grep -q "boottrace load" "$LOG" || echo "FAIL: no boottrace load line (no RAM load?)"
    if [ -s "$IMG" ]; then
        echo "ramimg $(wc -c <"$IMG" | tr -d ' ') bytes sha256 $(shasum -a 256 "$IMG" | cut -c1-16)"
    else
        echo "FAIL: no RAM image written ($IMG)"
    fi
    if [ "${BOOT_TRACE_COUNT:-1}" != 0 ]; then
        T=$OUT/$N-$LABEL.trace
        rm -f "$T"
        rc=$(run traced IPODUI_FSTRACE="$T" IPODUI_FSTRACE_READS=1)
        echo "-- traced run (shots rc=$rc): the device's file layer (fsreplay count)"
        "$FC/fsreplay" count "$T" 32 2048 || echo "FAIL: fsreplay count rc=$?"
        rm -f "$T"
    fi
} 2>&1 | tee "$RES"
if grep -q '^FAIL' "$RES"; then
    exit 1
fi
