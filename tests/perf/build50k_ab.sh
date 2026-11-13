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

# The 50k library build time in the simulator, before/after (one-build
# WP5, the simplified performance gate; out/briefs/one-build-brief.md
# section 8):
#
#   tests/perf/build50k_ab.sh A_SIM_DIR B_SIM_DIR [ROUNDS]
#
# A_SIM_DIR and B_SIM_DIR are configured, built simulator dirs (e.g. the
# baseline's build/sim-ui and this tree's). Each round runs A then B (ABAB
# ... ROUNDS times, default 2), every run on a fresh APFS clone of one
# synthetic 50,000-track disk with no database
# (tests/perf/synthetic_library.py) plus that arm's installed .rockbox, so
# every run builds the whole library from nothing. A run is
# tests/perf/build-50k.script: boot, "wait_db" until the database is
# ready. Prints one line per run:
#   run <n> arm=<A|B> wait_db_ms=<ms> storage=<solid|rotating>
# then each arm's median and the B/A ratio. The default sim disk is the
# flash model, so every run of an arm in NEED_SOLID (default "B", the tree
# under test; a baseline before #79 has no such line) must log "unit
# storage=solid" (the brief's flash arm); a run without it, or any run
# without "wait_db ready", fails (exit 1).
# Host load shows in the spread: alternate arms, and report every run.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
A=${1:?usage: $0 A_SIM_DIR B_SIM_DIR [ROUNDS]}
B=${2:?usage: $0 A_SIM_DIR B_SIM_DIR [ROUNDS]}
ROUNDS=${3:-2}
N=50000
W=$ROOT/build/tmp/build50k
mkdir -p "$W"
for d in "$A" "$B"; do
    [ -x "$d/rockboxui" ] || { echo "no simulator in $d" >&2; exit 2; }
    [ -d "$d/simdisk/.rockbox/codecs" ] || make -C "$d" install >/dev/null || exit 2
done
# one synthetic music tree for every run (idempotent, deterministic)
python3 "$ROOT/tests/perf/synthetic_library.py" "$W/pristine" "$N" >/dev/null || exit 2

clone() { # src dst
    rm -rf "$2"
    if ! { [ "$(uname)" = Darwin ] && /bin/cp -c -R "$1" "$2" 2>/dev/null; }; then
        cp -pR "$1" "$2" || exit 2
    fi
}

rc=0
n=0
: >"$W/results.txt"
for r in $(seq 1 "$ROUNDS"); do
    for arm in A B; do
        n=$((n + 1))
        if [ $arm = A ]; then src=$A; else src=$B; fi
        run=$W/run-$n
        rm -rf "$run"
        mkdir -p "$run"
        clone "$W/pristine" "$run/simdisk"
        rsync -a --exclude 'database*' "$src/simdisk/.rockbox/" "$run/simdisk/.rockbox/"
        # the device's playlist size, as tests/perf/lib65k.sh sets it
        echo "max files in playlist: 2000" >>"$run/simdisk/.rockbox/config.cfg"
        ln -sf "$src/rockboxui" "$run/rockboxui"
        [ -f "$src/UI256.bmp" ] && ln -sf "$src/UI256.bmp" "$run/UI256.bmp"
        SIM_BUILD=$run SHOTS_TIMEOUT=${SHOTS_TIMEOUT:-2400} \
            "$ROOT/scripts/shots.sh" "$ROOT/tests/perf/build-50k.script" \
            "$run/out" >"$run/shots.log" 2>&1
        log=$run/out/sim.log
        ms=$(sed -n 's/.*wait_db ready after \([0-9]*\) ms.*/\1/p' "$log" 2>/dev/null | head -1)
        st=$(sed -n 's/.*unit storage=\([a-z]*\).*/\1/p' "$log" 2>/dev/null | head -1)
        echo "run $n arm=$arm wait_db_ms=${ms:-none} storage=${st:-none}" | tee -a "$W/results.txt"
        if [ -z "$ms" ]; then
            echo "FAIL run $n: no 'wait_db ready' ($log)"
            rc=1
        fi
        case " ${NEED_SOLID:-B} " in
            *" $arm "*) if [ "$st" != solid ]; then
                            echo "FAIL run $n: arm $arm did not log unit storage=solid ($log)"
                            rc=1
                        fi ;;
        esac
        rm -rf "$run/simdisk"      # the clone; keep the log
    done
done
python3 - "$W/results.txt" <<'EOF'
import re, sys, statistics
v = {'A': [], 'B': []}
for l in open(sys.argv[1]):
    m = re.search(r'arm=(\w) wait_db_ms=(\d+)', l)
    if m:
        v[m.group(1)].append(int(m.group(2)))
for k in 'AB':
    if v[k]:
        print('arm %s: n=%d median %d ms (min %d, max %d)' % (
            k, len(v[k]), statistics.median(v[k]), min(v[k]), max(v[k])))
if v['A'] and v['B']:
    print('B/A median ratio %.3f' % (statistics.median(v['B']) / statistics.median(v['A'])))
EOF
exit $rc
