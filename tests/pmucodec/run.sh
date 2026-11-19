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

# Host test (codec supply, D1REGC1): the real pcf50605_init() sliced out of
# firmware/drivers/pcf50605.c.
#  1. shell build (HAVE_IPOD_UI, IPOD_VIDEO): must write 0xf5 (3.0 V);
#  2. stock build: must still write 0xf0 (stock identity);
#  3. mutant: the shell build with the old 0xf0 must turn test 1 red;
#  4. no other file writes D1REGC1 by name (the devbench cycles it by number
#     and puts back the code it read; its sleep form keeps the voltage code).
# SRC=... overrides the driver (used for the red run on the old source).
# Compiler: newest Homebrew gcc-NN else cc; HOSTCC=... overrides.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
SRC=${SRC:-firmware/drivers/pcf50605.c}
if [ -z "${HOSTCC:-}" ]; then
    HOSTCC=cc
    for v in 19 18 17 16 15 14; do
        command -v gcc-$v >/dev/null 2>&1 && { HOSTCC=gcc-$v; break; }
    done
fi
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
awk '/^void pcf50605_init\(void\)/{p=1} p' "$SRC" >"$TMP/real.c"
if [ ! -s "$TMP/real.c" ] || ! grep -q PCF5060X_D1REGC1 "$TMP/real.c"; then
    echo "pmucodec: FAIL: slice empty"; exit 1
fi
run() {  # name expect defines slice
    cp "$4" "$TMP/init_slice.c"
    # shellcheck disable=SC2086
    "$HOSTCC" -std=gnu99 -Wall -Wextra -Werror -DEXPECT_D1=$2 $3 -I"$TMP" \
        -o "$TMP/$1" tests/pmucodec/test_pmucodec.c 2>"$TMP/cc.log" \
        || { cat "$TMP/cc.log"; echo "pmucodec: FAIL: $1 did not build"; return 2; }
    "$TMP/$1"
}
rc=0
run shell 0xf5 "-DIPOD_VIDEO -DHAVE_IPOD_UI" "$TMP/real.c" || rc=1
run stock 0xf0 "-DIPOD_VIDEO" "$TMP/real.c" || rc=1
# mutant: the shell build's write put back to 2.5 V must be caught
sed 's/PCF5060X_D1REGC1, 0xf5/PCF5060X_D1REGC1, 0xf0/' "$TMP/real.c" >"$TMP/mut.c"
if cmp -s "$TMP/real.c" "$TMP/mut.c"; then
    echo "pmucodec: mutant changed nothing (expected only on the old source)"
fi
if run mutant 0xf5 "-DIPOD_VIDEO -DHAVE_IPOD_UI" "$TMP/mut.c" >"$TMP/mut.out" 2>&1; then
    echo "pmucodec: FAIL: the 2.5 V mutant passed"; rc=1
elif grep -q "pmucodec: FAIL: D1REGC1" "$TMP/mut.out"; then
    echo "pmucodec: mutant red, as it must be"
else
    cat "$TMP/mut.out"; echo "pmucodec: FAIL: mutant red for the wrong reason"; rc=1
fi
# other write sites by name (the driver, pcf50606's own table and the define are fine)
n=$(git grep -n "PCF5060X_D1REGC1" -- firmware apps bootloader |
    grep -v "^firmware/drivers/pcf50605.c\|^firmware/drivers/pcf50606.c\|^firmware/export/pcf5060x.h" |
    wc -l | tr -d ' ')
if [ "$n" = 0 ]; then
    echo "pmucodec: no other D1REGC1 user by name (0 found)"
else
    echo "pmucodec: FAIL: $n other D1REGC1 users:"
    git grep -n PCF5060X_D1REGC1 -- firmware apps bootloader; rc=1
fi
[ $rc -eq 0 ] && echo "pmucodec: ALL PASSED"
exit $rc
