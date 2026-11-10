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

# The one port lock's nesting (firmware/target/arm/ipod/video/
# bcm-port-lock.h) against a model of the tick block and the mutex.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
T=tests/bcmportlock

if [ -z "${HOSTCC:-}" ]; then
    HOSTCC=cc
    for v in 19 18 17 16 15 14; do
        if command -v gcc-$v >/dev/null 2>&1; then HOSTCC=gcc-$v; break; fi
    done
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
if ! "$HOSTCC" -std=gnu99 -O1 -g -Wall -Wextra -Werror ${EXTRA_CFLAGS:-} \
        -Ifirmware/target/arm/ipod/video -o "$TMP/test_bcmportlock" "$T/test_bcmportlock.c" \
        2>"$TMP/cc.log"; then
    echo "FAIL: compiling test_bcmportlock ($HOSTCC)"
    cat "$TMP/cc.log"
    exit 1
fi
if "$TMP/test_bcmportlock" >"$TMP/out.log"; then
    echo "ok   one port lock nesting: $(tail -1 "$TMP/out.log") (bcm-port-lock.h)"
else
    echo "FAIL: one port lock nesting"
    cat "$TMP/out.log"
    exit 1
fi
