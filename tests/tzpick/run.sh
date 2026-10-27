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


# Host test of the time-zone table and its DST arithmetic (P12 W1-C, Q27):
# apps/ipodui/tzpick_core.c, with a generated stand-in for the build's
# lang.h (the table's LANG ids only).
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
T=tests/tzpick
if [ -z "${HOSTCC:-}" ]; then
    HOSTCC=cc
    for v in 19 18 17 16 15 14; do
        if command -v gcc-$v >/dev/null 2>&1; then HOSTCC=gcc-$v; break; fi
    done
fi
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
grep -o 'LANG_IPODUI_TZ_[A-Z0-9_]*' apps/ipodui/tzcities.h | sort -u |
    awk '{ print "#define " $1 " " NR }' > "$TMP/lang.h"
# shellcheck disable=SC2086
if ! "$HOSTCC" -std=gnu99 -O1 -g -Wall -Wextra -Werror ${EXTRA_CFLAGS:-} \
        -I"$TMP" -Iapps -o "$TMP/test_tzpick" \
        "$T/test_tzpick.c" apps/ipodui/tzpick_core.c 2>"$TMP/cc.log"; then
    echo "FAIL: compiling test_tzpick ($HOSTCC)"
    cat "$TMP/cc.log"
    exit 1
fi
if "$TMP/test_tzpick"; then
    echo "ok   time zones (tzpick_core.c: DST rules both hemispheres, other cities, the table)"
else
    echo "FAIL: time zones"
    exit 1
fi
