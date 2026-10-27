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

# Host test of the no-change boot stamp (Phase 16-2, #89):
# apps/ipodui/bootstamp_core.c: the stamp CRC and torn writes, every
# trust/walk decision, and the directory digest over a synthetic tree.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
if [ -z "${HOSTCC:-}" ]; then
    HOSTCC=cc
    for v in 19 18 17 16 15 14; do
        if command -v gcc-$v >/dev/null 2>&1; then HOSTCC=gcc-$v; break; fi
    done
fi
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
if ! "$HOSTCC" -std=gnu99 -O1 -g -Wall -Wextra -Werror -Iapps \
        -o "$TMP/test_bootstamp" tests/bootstamp/test_bootstamp.c \
        -DBOOTSTAMP_HOST_TEST apps/ipodui/bootstamp_core.c 2>"$TMP/cc.log"; then
    echo "FAIL: compiling test_bootstamp ($HOSTCC)"
    cat "$TMP/cc.log"
    exit 1
fi
"$TMP/test_bootstamp" || { echo "FAIL: boot stamp"; exit 1; }
echo "ok   boot stamp CRC, torn writes, decision, digest (bootstamp_core.c)"
