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

# Host test of the 0.8.3 co-location's write lease (#290): the REAL
# apps/pcmbuf.c (copied beside the stubs with two scheduling-point
# #defines, srcpatch.py + spec_pcmbuf.py) under a two-cache model and
# seeded CPU/COP/FIQ interleavings (test_fencepcm.c). Then mutation mode:
# each mutant in spec_pcmbuf.py deletes one fence or reorders one publish
# in the copy, and must turn the test red with its verdict line; a mutant
# that survives, fails to build or crashes without a verdict fails here.
# Compiler: the newest Homebrew gcc-NN, else cc; override with HOSTCC=...
# FENCE_SEEDS (default 200) seeds per depth; FENCE_MUTANTS=0 skips them.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
T=tests/fencepcm
SEEDS=${FENCE_SEEDS:-200}

if [ -z "${HOSTCC:-}" ]; then
    HOSTCC=cc
    for v in 19 18 17 16 15 14; do
        if command -v gcc-$v >/dev/null 2>&1; then HOSTCC=gcc-$v; break; fi
    done
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/src/ipodui"
# the real headers pcmbuf.c takes from its own directory
cp apps/pcmbuf.h apps/codec_place_core.h firmware/export/pcm_xcore.h "$TMP/src/"
cp apps/ipodui/fadecurve.h "$TMP/src/ipodui/"

# build NAME [MUTANT]: the instrumented copy (and the mutant) and the test
build() {
    rm -f "$TMP/src/pcmbuf_fence.c" "$TMP/$1"
    python3 $T/srcpatch.py apps/pcmbuf.c "$TMP/src/pcmbuf_fence.c" \
        $T/spec_pcmbuf.py ${2:-} || return 2
    # shellcheck disable=SC2086
    "$HOSTCC" -std=gnu99 -O1 -g -Wall -Wextra -Werror -Wno-unused-function \
        -Wno-unused-variable -Wno-unused-but-set-variable -Wno-type-limits \
        ${EXTRA_CFLAGS:-} -I"$TMP/src" -I$T/stub -o "$TMP/$1" \
        $T/test_fencepcm.c apps/codec_place_core.c -lpthread \
        2>"$TMP/cc.log" || { cat "$TMP/cc.log"; return 2; }
}

rc=0
if ! build base; then
    echo "FAIL: compiling test_fencepcm ($HOSTCC)"
    exit 1
fi
for d in 16 24; do
    if "$TMP/base" "$SEEDS" $d >"$TMP/out" 2>&1 \
       && grep -q '^VERDICT: PASS$' "$TMP/out"; then
        grep -v '^VERDICT' "$TMP/out"
    else
        cat "$TMP/out"
        echo "FAIL: pcmbuf write lease, depth $d"
        rc=1
    fi
done
[ $rc -eq 0 ] && echo "ok   pcmbuf write lease (real pcmbuf.c: grant, window, commit, release, reclaim under a two-cache model, CPU/COP/FIQ interleavings, 16- and 24-bit)"

[ "${FENCE_MUTANTS:-1}" = 0 ] && exit $rc
n=0; red=0
for m in $(python3 $T/srcpatch.py --list $T/spec_pcmbuf.py); do
    n=$((n + 1))
    if ! build "mut_$m" "$m"; then
        echo "FAIL: mutant $m does not apply or build"
        rc=1; continue
    fi
    # red in either depth counts; the verdict line must be there
    v=""
    for d in 16 24; do
        "$TMP/mut_$m" "$SEEDS" $d >"$TMP/mout" 2>&1
        if grep -q '^VERDICT: FAIL' "$TMP/mout"; then
            v=$(grep '^VERDICT: FAIL' "$TMP/mout" | head -1); break
        fi
        if ! grep -q '^VERDICT: PASS$' "$TMP/mout"; then
            v="CRASH"; break
        fi
    done
    case "$v" in
        "VERDICT: FAIL"*) red=$((red + 1))
            echo "     red  $m: ${v#VERDICT: }" ;;
        CRASH) echo "FAIL: mutant $m ended without a verdict line"
            tail -5 "$TMP/mout"; rc=1 ;;
        *) echo "FAIL: mutant $m survived (the test cannot see it)"; rc=1 ;;
    esac
done
echo "     mutants: $red of $n red"
[ "$red" -eq "$n" ] && [ "$n" -gt 0 ] && [ $rc -eq 0 ] \
    && echo "ok   pcmbuf write lease mutants ($n of $n red)"
exit $rc
