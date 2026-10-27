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

# Host test of the PP5022 band governor (docs/design/pp-governor.md
# section 10.1): firmware/pgov_core.c's rows, then the simulator's
# regression table at fixed parameters against sim_expected.txt (a policy
# change that moves it fails until the table is regenerated with a reason:
# tests/ppgov/run.sh --update).  The A/B grid: build/ppgov/sim --grid.
# Compiler: the newest Homebrew gcc-NN, else cc; override with HOSTCC=...
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
T=tests/ppgov

if [ -z "${HOSTCC:-}" ]; then
    HOSTCC=cc
    for v in 19 18 17 16 15 14; do
        if command -v gcc-$v >/dev/null 2>&1; then HOSTCC=gcc-$v; break; fi
    done
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
# -ffp-contract=off: the same table on every host (no fused multiply-add)
CF="-std=gnu99 -O1 -g -Wall -Wextra -Werror -ffp-contract=off -DPGOV_HOST -Ifirmware/export"
# shellcheck disable=SC2086
if ! "$HOSTCC" $CF ${EXTRA_CFLAGS:-} -o "$TMP/test_pgov" \
        "$T/test_pgov.c" firmware/pgov_core.c 2>"$TMP/cc.log" ||
   ! "$HOSTCC" $CF ${EXTRA_CFLAGS:-} -o "$TMP/sim" \
        "$T/sim.c" firmware/pgov_core.c 2>>"$TMP/cc.log"; then
    echo "FAIL: compiling the governor tests ($HOSTCC)"
    cat "$TMP/cc.log"
    exit 1
fi
if "$TMP/test_pgov"; then
    echo "ok   band governor policy (pgov_core.c: selector, voltage layer, record, tables)"
else
    echo "FAIL: band governor policy"
    exit 1
fi
"$TMP/sim" >"$TMP/sim.txt" || { echo "FAIL: governor simulator"; exit 1; }
if [ "${1:-}" = --update ]; then
    cp "$TMP/sim.txt" "$T/sim_expected.txt"
    echo "updated $T/sim_expected.txt"
elif diff -u "$T/sim_expected.txt" "$TMP/sim.txt" >"$TMP/sim.diff"; then
    echo "ok   band governor simulator (regression table unchanged)"
else
    echo "FAIL: the governor simulator's table moved:"
    head -40 "$TMP/sim.diff"
    exit 1
fi
