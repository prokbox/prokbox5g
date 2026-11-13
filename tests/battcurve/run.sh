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

# Host test of the fitted cell's capacity, sag and curve
# (apps/ipodui/battcurve.c; one-build brief WP3): pinned values for
# every answer on both RAM sizes and both storage rows, each one a table
# batcal's own sanity rule accepts (batcal_core.c bc_sane()).
# Compiler: the newest Homebrew gcc-NN, else cc; override with HOSTCC=...
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
T=tests/battcurve

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
        -Iapps -I"$TMP/inc" -o "$TMP/test_battcurve" \
        "$T/test_battcurve.c" apps/ipodui/battcurve.c \
        apps/ipodui/batcal_core.c 2>"$TMP/cc.log"; then
    echo "FAIL: compiling test_battcurve ($HOSTCC)"
    cat "$TMP/cc.log"
    exit 1
fi
out=$("$TMP/test_battcurve" 2>&1)
rc=$?
case "$out" in
    *"battcurve: "*" checks, 0 failed"*) ;;
    *) rc=1 ;;
esac
if [ $rc -eq 0 ]; then
    echo "ok   battery cell (battcurve.c: capacity, sag, scaled curve; $(echo "$out" | tail -1))"
else
    echo "FAIL: battery cell"
    echo "$out"
fi
exit $rc
