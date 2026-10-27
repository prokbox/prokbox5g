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

# Round trip of the native USB screen's art (Phase 8a, §3.2):
#   1. art/usb/usbscreen.py regenerates the tables and PNGs into a temp
#      dir; the committed apps/ipodui/usbscreen_data.h and
#      docs/design/usb/*.png must be byte-identical (the generator is the
#      source of truth).
#   2. apps/ipodui/usbscreen.c's pure part, built on the host, composes
#      the strip as the firmware does; every screen must match the
#      reference PNGs pixel for pixel (check.py).
# A missing generator, compiler or file fails; nothing skips.
# Compiler: the newest Homebrew gcc-NN, else cc; override with HOSTCC=...
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
T=tests/usbscreen

if [ -z "${HOSTCC:-}" ]; then
    HOSTCC=cc
    for v in 19 18 17 16 15 14; do
        if command -v gcc-$v >/dev/null 2>&1; then HOSTCC=gcc-$v; break; fi
    done
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail=0

mkdir -p "$TMP/png" "$TMP/raw"
if ! python3 art/usb/usbscreen.py --c "$TMP/usbscreen_data.h" \
        --png "$TMP/png" >"$TMP/gen.log" 2>&1; then
    echo "FAIL generator"; cat "$TMP/gen.log"; exit 1
fi
if cmp -s "$TMP/usbscreen_data.h" apps/ipodui/usbscreen_data.h; then
    echo "ok   usbscreen_data.h matches the generator"
else
    echo "FAIL apps/ipodui/usbscreen_data.h is stale: rerun art/usb/usbscreen.py"
    fail=1
fi
for p in "$TMP"/png/*.png; do
    b=$(basename "$p")
    if ! cmp -s "$p" "docs/design/usb/$b"; then
        echo "FAIL docs/design/usb/$b differs from the generator"
        fail=1
    fi
done

if ! "$HOSTCC" -std=gnu99 -O1 -Wall -Wextra -Werror -Iapps/ipodui \
        -o "$TMP/t" $T/test_usbscreen.c 2>"$TMP/cc.log"; then
    echo "FAIL compile"; cat "$TMP/cc.log"; exit 1
fi
"$TMP/t" "$TMP/raw" || { echo "FAIL test_usbscreen"; exit 1; }
python3 $T/check.py "$TMP/raw" docs/design/usb || fail=1

[ $fail -eq 0 ] && echo "usbscreen: PASS" || { echo "usbscreen: FAIL"; exit 1; }
