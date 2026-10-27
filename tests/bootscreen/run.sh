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

# Boot screen round trip (Phase 8a, docs/phase8a-brief.md section 4, step 0):
#  1. art/boot/bootlogo.py --c into a temp root must reproduce the committed
#     firmware/common/bootscreen_data.c and firmware/export/bootscreen_data.h;
#  2. the real firmware/common/bootscreen.c + the committed data, compiled on
#     the host into a 320x240 RGB565 framebuffer, must match BOOTSCREEN_GEN
#     and be pixel-identical to docs/design/logo/wheel3d-medium-screen.png.
# Writes out/bootscreen/boot-bl.png (the bootloader's frame, for the boot
# hand-off sheet). A missing generator, reference or compiler FAILS; this
# test never skips. HOSTCC=... overrides the compiler.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
T=tests/bootscreen
GEN=art/boot/bootlogo.py
REF=docs/design/logo/wheel3d-medium-screen.png
OUT=out/bootscreen

if [ -z "${HOSTCC:-}" ]; then
    HOSTCC=cc
    for v in 19 18 17 16 15 14; do
        if command -v gcc-$v >/dev/null 2>&1; then HOSTCC=gcc-$v; break; fi
    done
fi

for f in "$GEN" "$REF" firmware/common/bootscreen.c \
         firmware/common/bootscreen_data.c firmware/export/bootscreen_data.h; do
    if [ ! -f "$f" ]; then
        echo "FAIL: missing $f"
        exit 1
    fi
done
command -v python3 >/dev/null 2>&1 || { echo "FAIL: no python3"; exit 1; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail=0

# 1. the committed data is what the generator makes today
if ! python3 -I "$GEN" --c "$TMP/gen" >"$TMP/gen.log" 2>&1; then
    echo "FAIL: $GEN --c"; cat "$TMP/gen.log"; exit 1
fi
for f in firmware/common/bootscreen_data.c firmware/export/bootscreen_data.h; do
    if cmp -s "$TMP/gen/$f" "$f"; then
        echo "ok   $f is current ($(cat "$TMP/gen.log"))"
    else
        echo "FAIL: $f differs from $GEN --c output (regenerate and commit)"
        fail=1
    fi
done

# 2. host render of the committed code + data
if ! "$HOSTCC" -std=gnu99 -O1 -Wall -Wextra -Werror ${EXTRA_CFLAGS:-} \
        -DHAVE_IPOD_BOOTSCREEN -DBOOTLOADER \
        -I$T/shim -Ifirmware/export \
        -o "$TMP/test_bootscreen" $T/test_bootscreen.c \
        firmware/common/bootscreen.c firmware/common/bootscreen_data.c \
        2>"$TMP/cc.log"; then
    echo "FAIL: compiling test_bootscreen ($HOSTCC)"; cat "$TMP/cc.log"; exit 1
fi
"$TMP/test_bootscreen" "$TMP/fb.raw" || exit 1
mkdir -p "$OUT"
python3 -I $T/compare.py "$TMP/fb.raw" "$REF" "$OUT/boot-bl.png" || fail=1

[ $fail -eq 0 ] && echo "bootscreen: PASS" || { echo "bootscreen: FAIL"; exit 1; }
