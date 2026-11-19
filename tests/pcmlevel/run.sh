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

# apps/pcmbuf.c data_level() counts bytes at the frame's real size, so a
# 24-bit track (8-byte frames) prefills the full time, not half of it
# (0.9.0 integration, package B's finding). The real function, sliced out
# of pcmbuf.c, is checked in the shell's build and in stock's; a mutant
# that puts back the 4-byte count must go red.
#
#   tests/pcmlevel/run.sh
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT" || exit 2
T=tests/pcmlevel
SP=tests/fencepcm/srcpatch.py

if [ -z "${HOSTCC:-}" ]; then
    HOSTCC=cc
    for v in 19 18 17 16 15 14; do
        if command -v gcc-$v >/dev/null 2>&1; then HOSTCC=gcc-$v; break; fi
    done
fi
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# build NAME DEFINES [MUTANT]
build() {
    mkdir -p "$TMP/$1"
    python3 $SP apps/pcmbuf.c "$TMP/$1/level_slice.c" $T/spec_level.py \
        ${3:-} || return 2
    # shellcheck disable=SC2086
    "$HOSTCC" -std=gnu99 -O1 -Wall -Wextra -Werror $2 -I"$TMP/$1" \
        -o "$TMP/$1/t" $T/test_pcmlevel.c 2>"$TMP/$1/cc.log" \
        || { cat "$TMP/$1/cc.log"; return 2; }
}

# run NAME: the verdict line must be there
run() {
    "$TMP/$1/t" >"$TMP/$1/out" 2>&1
    run_rc=$?
    grep -q '^VERDICT: ' "$TMP/$1/out" || { cat "$TMP/$1/out"; return 2; }
    [ $run_rc -eq 0 ] && grep -q '^VERDICT: PASS' "$TMP/$1/out"
}

rc=0
for cfg in shell stock; do
    d=""; [ $cfg = shell ] && d="-DHAVE_PCM_24BIT_OUT"
    if ! build $cfg "$d"; then echo "FAIL: building the $cfg check"; exit 1; fi
    if run $cfg; then
        echo "ok   pcmbuf data_level, $cfg build: $(sed -n 's/^VERDICT: PASS //p' "$TMP/$cfg/out")"
    else
        cat "$TMP/$cfg/out"; echo "FAIL: pcmbuf data_level, $cfg build"; rc=1
    fi
done

# the mutant (24-bit build only): must build, run, and fail
build mut "-DHAVE_PCM_24BIT_OUT" four_byte_frames \
    || { echo "FAIL: mutant four_byte_frames does not apply or build"; exit 1; }
run mut; m=$?
if [ $m -eq 1 ]; then
    echo "ok   mutant four_byte_frames is red ($(grep -c '^FAIL' "$TMP/mut/out") checks)"
else
    cat "$TMP/mut/out"; echo "FAIL: mutant four_byte_frames survived (or crashed: $m)"; rc=1
fi
exit $rc
