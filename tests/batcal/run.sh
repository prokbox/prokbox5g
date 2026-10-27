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

# Host test of automatic battery calibration (tracker #70):
# apps/ipodui/batcal_core.c, which has no Rockbox calls, plus the replay
# of a synthetic power.csv-style trace (replay.csv) through the recorder.
# Compiler: the newest Homebrew gcc-NN, else cc; override with HOSTCC=...
# A real power.csv can be replayed locally (never commit it):
#   tests/batcal/run.sh --replay /path/to/power.csv
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
T=tests/batcal

if [ -z "${HOSTCC:-}" ]; then
    HOSTCC=cc
    for v in 19 18 17 16 15 14; do
        if command -v gcc-$v >/dev/null 2>&1; then HOSTCC=gcc-$v; break; fi
    done
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/inc"
cp firmware/include/crc32.h "$TMP/inc/"     # alone: firmware/include
                                            # shadows libc headers
# shellcheck disable=SC2086
if ! "$HOSTCC" -std=gnu99 -O1 -g -Wall -Wextra -Werror ${EXTRA_CFLAGS:-} \
        -Iapps -I"$TMP/inc" -o "$TMP/test_batcal" \
        "$T/test_batcal.c" apps/ipodui/batcal_core.c -lm 2>"$TMP/cc.log"; then
    echo "FAIL: compiling test_batcal ($HOSTCC)"
    cat "$TMP/cc.log"
    exit 1
fi
if [ "${1:-}" = "--replay" ]; then
    "$TMP/test_batcal" --replay "$2"
    exit $?
fi
fail=0
if "$TMP/test_batcal"; then
    echo "ok   battery calibration (batcal_core.c: fit, sanity, decimation, blend, states, files)"
else
    echo "FAIL: battery calibration"
    fail=1
fi
if "$TMP/test_batcal" --replay "$T/replay.csv"; then
    echo "ok   battery calibration replay (replay.csv: runs open and close where expected)"
else
    echo "FAIL: battery calibration replay"
    fail=1
fi
exit $fail
