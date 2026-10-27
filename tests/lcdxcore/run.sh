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

# Host test of the pixel hand-off across cores (BCM MF-4, P19 19-9):
# firmware/target/arm/ipod/video/lcd-fb-xcore.c, included as built into a
# two-core cache-incoherence model.  The COP draws, the CPU pushes, at
# seeded random interleavings: the CPU push of the COP's frame (the wake
# frame), vplayer's overlay, power_off's take.  Each is clean over 300
# seeds, and each negative control (one write-back, fence, drop or
# uncached read left out) is caught.
# Compiler: the newest Homebrew gcc-NN, else cc; override with HOSTCC=...
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
T=tests/lcdxcore

if [ -z "${HOSTCC:-}" ]; then
    HOSTCC=cc
    for v in 19 18 17 16 15 14; do
        if command -v gcc-$v >/dev/null 2>&1; then HOSTCC=gcc-$v; break; fi
    done
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
CF="-std=gnu99 -O1 -g -Wall -Wextra -Werror -pthread ${EXTRA_CFLAGS:-}"
# shellcheck disable=SC2086
if ! "$HOSTCC" $CF -o "$TMP/test_lcdxcore" "$T/test_lcdxcore.c" \
        2>"$TMP/cc.log"; then
    echo "FAIL: compiling test_lcdxcore ($HOSTCC)"; cat "$TMP/cc.log"; exit 1
fi
if "$TMP/test_lcdxcore" >"$TMP/out.log"; then
    echo "ok   pixel hand-off (lcd-fb-xcore.c: two incoherent cores, clean; omissions caught)"
    exit 0
fi
cat "$TMP/out.log"
echo "FAIL: pixel hand-off (lcd-fb-xcore.c)"
exit 1
