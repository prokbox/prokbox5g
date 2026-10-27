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

# Host test of the Calendars index (P12 W3-C, docs/phase12-brief.md 3.5):
# apps/ipodui/xical_core.c (the .ics parser, the recurrence expander, the
# cap and the layout) with xindex_core.c and tzpick_core.c's DST rules,
# against tests/xical/fixtures.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
T=tests/xical
if [ -z "${HOSTCC:-}" ]; then
    HOSTCC=cc
    for v in 19 18 17 16 15 14; do
        if command -v gcc-$v >/dev/null 2>&1; then HOSTCC=gcc-$v; break; fi
    done
fi
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/inc"
cp firmware/include/crc32.h "$TMP/inc/"   # alone: firmware/include
                                            # shadows libc headers
grep -o 'LANG_IPODUI_TZ_[A-Z0-9_]*' apps/ipodui/tzcities.h | sort -u |
    awk '{ print "#define " $1 " " NR }' > "$TMP/inc/lang.h"
# shellcheck disable=SC2086
if ! "$HOSTCC" -std=gnu99 -O1 -g -Wall -Wextra -Werror ${EXTRA_CFLAGS:-} \
        -Iapps -I"$TMP/inc" -o "$TMP/t" "$T/test_xical.c" \
        apps/ipodui/xical_core.c apps/ipodui/xical_read.c apps/ipodui/xindex_core.c \
        apps/ipodui/tzpick_core.c 2>"$TMP/cc.log"; then
    echo "FAIL: compiling test_xical ($HOSTCC)"
    cat "$TMP/cc.log"
    exit 1
fi
if "$TMP/t"; then
    echo "ok   calendars index (xical_core.c: DST-crossing weekly, all-day, TZID vs UTC, folded lines, COUNT/UNTIL/EXDATE/overrides, the 2,048 cap)"
else
    echo "FAIL: calendars index"
    exit 1
fi
