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

# Host test of the shell's error/splash log (Phase 8a U2): apps/ipodui/
# uilog.c against host fakes of the file and event calls (tests/uilog/stub):
# the persistent idle flush (Revision 1 H3), overflow, rotation, a line
# logged during a flush, repeat collapse (M3) and a write error.
# Compiler: the newest Homebrew gcc-NN, else cc; override with HOSTCC=...
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
T=tests/uilog

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
        -DROCKBOX_DIR="\"$TMP\"" -I$T/stub -Iapps -o "$TMP/test_uilog" \
        "$T/test_uilog.c" apps/ipodui/uilog.c 2>"$TMP/cc.log"; then
    echo "FAIL: compiling test_uilog ($HOSTCC)"
    cat "$TMP/cc.log"
    exit 1
fi
if "$TMP/test_uilog"; then
    echo "ok   ui log (uilog.c: ring, idle flush, overflow, rotation, repeats)"
else
    echo "FAIL: ui log"
    exit 1
fi
# the boot ring (UI_LOG_BOOT_LINES): the same file, its own scenario
if ! "$HOSTCC" -std=gnu99 -O1 -g -Wall -Wextra -Werror ${EXTRA_CFLAGS:-} \
        -DT_BOOT_RING -DROCKBOX_DIR="\"$TMP/b\"" -I$T/stub -Iapps \
        -o "$TMP/test_uilog_boot" "$T/test_uilog.c" apps/ipodui/uilog.c \
        2>"$TMP/cc.log"; then
    echo "FAIL: compiling test_uilog (boot ring)"
    cat "$TMP/cc.log"
    exit 1
fi
mkdir -p "$TMP/b"
if "$TMP/test_uilog_boot"; then
    echo "ok   ui log: the boot ring (64 lines, kept across a bench session)"
else
    echo "FAIL: ui log: the boot ring"
    exit 1
fi
