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

# Host test of the drive power policy (#114, P18 A3):
# firmware/common/stor_pwr_core.c, which has no Rockbox calls.
# Compiler: the newest Homebrew gcc-NN, else cc; override with HOSTCC=...
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
T=tests/storpwr

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
        -Ifirmware/export -o "$TMP/test_storpwr" \
        "$T/test_storpwr.c" firmware/common/stor_pwr_core.c 2>"$TMP/cc.log"; then
    echo "FAIL: compiling test_storpwr ($HOSTCC)"
    cat "$TMP/cc.log"
    exit 1
fi
if "$TMP/test_storpwr"; then
    echo "ok   drive power policy (stor_pwr_core.c: hold, backoff, events, refill margin, failed power-ups, cut safety)"
else
    echo "FAIL: drive power policy"
    exit 1
fi
