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

# Host test of the PP5022 cache drop (#290; 0ebecb0db6): the REAL cache
# section of firmware/target/arm/pp/system-pp502x.c, sliced out verbatim
# (tests/fencepcm/srcpatch.py + spec_cache.py), against recording register
# and CPSR hooks (test_fencecache.c). Then mutation mode: each mutant in
# spec_cache.py deletes one fence or changes one mask or window, and must
# turn the test red with its verdict line; a mutant that survives, fails
# to build or crashes without a verdict fails here.
# Compiler: the newest Homebrew gcc-NN, else cc; override with HOSTCC=...
# FENCE_MUTANTS=0 skips the mutants.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
T=tests/fencecache
SRC=firmware/target/arm/pp/system-pp502x.c

if [ -z "${HOSTCC:-}" ]; then
    HOSTCC=cc
    for v in 19 18 17 16 15 14; do
        if command -v gcc-$v >/dev/null 2>&1; then HOSTCC=gcc-$v; break; fi
    done
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/src"

# build NAME [MUTANT]
build() {
    rm -f "$TMP/src/cache_fence.c" "$TMP/$1"
    python3 tests/fencepcm/srcpatch.py $SRC "$TMP/src/cache_fence.c" \
        $T/spec_cache.py ${2:-} || return 2
    # shellcheck disable=SC2086
    "$HOSTCC" -std=gnu99 -O1 -g -Wall -Wextra -Werror \
        -Wno-unused-function -Wno-unused-but-set-variable ${EXTRA_CFLAGS:-} \
        -I"$TMP/src" -I$T/stub -o "$TMP/$1" $T/test_fencecache.c \
        2>"$TMP/cc.log" || { cat "$TMP/cc.log"; return 2; }
}

rc=0
if ! build base; then
    echo "FAIL: compiling test_fencecache ($HOSTCC)"
    exit 1
fi
if "$TMP/base" >"$TMP/out" 2>&1 && grep -q '^VERDICT: PASS$' "$TMP/out"; then
    grep -v '^VERDICT' "$TMP/out"
    echo "ok   cache drop (real system-pp502x.c: write-back before the status rewrite, IRQ masked throughout, the COP's FIQ open only between 64-line blocks and only if open at the call; CPU and COP, every entry mask)"
else
    cat "$TMP/out"
    echo "FAIL: cache drop"
    exit 1
fi

[ "${FENCE_MUTANTS:-1}" = 0 ] && exit 0
n=0; red=0
for m in $(python3 tests/fencepcm/srcpatch.py --list $T/spec_cache.py); do
    n=$((n + 1))
    if ! build "mut_$m" "$m"; then
        echo "FAIL: mutant $m does not apply or build"
        rc=1; continue
    fi
    "$TMP/mut_$m" >"$TMP/mout" 2>&1
    v=$(grep '^VERDICT:' "$TMP/mout" | head -1)
    case "$v" in
        "VERDICT: FAIL"*) red=$((red + 1))
            echo "     red  $m: ${v#VERDICT: }" ;;
        "VERDICT: PASS") echo "FAIL: mutant $m survived (the test cannot see it)"
            rc=1 ;;
        *) echo "FAIL: mutant $m ended without a verdict line"
            tail -5 "$TMP/mout"; rc=1 ;;
    esac
done
echo "     mutants: $red of $n red"
[ "$red" -eq "$n" ] && [ "$n" -gt 0 ] && [ $rc -eq 0 ] \
    && echo "ok   cache drop mutants ($n of $n red)"
exit $rc
