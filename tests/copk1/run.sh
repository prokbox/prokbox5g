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

# Host test of K-1, the parked COP (firmware/export/cop_park.h): the tick
# rule in a model of the COP's scheduler, and upstream's core_sleep() /
# core_wake() handshake on two threads with no tick.
# Compiler: the newest Homebrew gcc-NN, else cc; override with HOSTCC=...
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
T=tests/copk1

if [ -z "${HOSTCC:-}" ]; then
    HOSTCC=cc
    for v in 19 18 17 16 15 14; do
        if command -v gcc-$v >/dev/null 2>&1; then HOSTCC=gcc-$v; break; fi
    done
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
# shellcheck disable=SC2086
if ! "$HOSTCC" -std=gnu11 -O1 -g -Wall -Wextra -Werror -pthread \
        ${EXTRA_CFLAGS:-} -Ifirmware/export -o "$TMP/test_copk1" \
        "$T/test_copk1.c" 2>"$TMP/cc.log"; then
    echo "FAIL: compiling test_copk1 ($HOSTCC)"
    cat "$TMP/cc.log"
    exit 1
fi
if "$TMP/test_copk1"; then
    echo "ok   K-1 parked COP (cop_park.h: posts and timeouts as with the pulse, no lost wake-up in the handshake)"
else
    echo "FAIL: K-1 parked COP"
    exit 1
fi
