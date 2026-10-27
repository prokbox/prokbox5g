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

# Host test of the video chip clock governor (P19 PW4, vclk.h):
# the floors, the load governor, the silicon FEED traces.
# Compiler: the newest Homebrew gcc-NN, else cc; override with HOSTCC=...
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
T=tests/vclk

if [ -z "${HOSTCC:-}" ]; then
    HOSTCC=cc
    for v in 19 18 17 16 15 14; do
        if command -v gcc-$v >/dev/null 2>&1; then HOSTCC=gcc-$v; break; fi
    done
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
# shellcheck disable=SC2086
if ! "$HOSTCC" -std=gnu99 -O1 -g -Wall -Wextra -Werror ${EXTRA_CFLAGS:-} \
        -Iapps -o "$TMP/test_vclk" \
        "$T/test_vclk.c" apps/video/vclk.c 2>"$TMP/cc.log"; then
    echo "FAIL: compiling test_vclk ($HOSTCC)"
    cat "$TMP/cc.log"
    exit 1
fi
if "$TMP/test_vclk" > "$TMP/out.txt"; then
    cat "$TMP/out.txt"
    echo "ok   video clock governor (vclk.c: the band selector and the in-band policies, the video floor on the silicon traces, misses, seeks, hysteresis, dwell, failures)"
else
    cat "$TMP/out.txt"
    echo "FAIL: video clock governor"
    exit 1
fi
