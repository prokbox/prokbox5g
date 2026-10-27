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

# Host test of the unlit clock face core (Phase 8a U2): apps/ipodui/
# unlit_core.c's state machine (docs/phase8a-brief.md §3.5, Revision 1
# M1/M2) and its painting, which must equal the art generator's PNGs in
# docs/design/unlit/ pixel for pixel (compare.py). The glyph tables
# (apps/ipodui/unlit_glyphs.h) come from tests/unlit/gen_glyphs.py; set
# UNLIT_REGEN=1 to also check that they are current (about 20 s).
# Compiler: the newest Homebrew gcc-NN, else cc; override with HOSTCC=...
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
T=tests/unlit

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
        -I$T/stub -Iapps -o "$TMP/test_unlit" \
        "$T/test_unlit.c" apps/ipodui/unlit_core.c 2>"$TMP/cc.log"; then
    echo "FAIL: compiling test_unlit ($HOSTCC)"
    cat "$TMP/cc.log"
    exit 1
fi
if ! "$TMP/test_unlit" "$TMP"; then
    echo "FAIL: unlit core"
    exit 1
fi
echo "ok   unlit state machine + minute patch (unlit_core.c)"
python3 -I $T/compare.py "$TMP" || { echo "FAIL: unlit pixels"; exit 1; }
if [ -n "${UNLIT_REGEN:-}" ]; then
    if python3 -I $T/gen_glyphs.py "$TMP/unlit_glyphs.h" >/dev/null &&
            cmp -s "$TMP/unlit_glyphs.h" apps/ipodui/unlit_glyphs.h; then
        echo "ok   unlit_glyphs.h is current"
    else
        echo "FAIL: apps/ipodui/unlit_glyphs.h is stale"
        exit 1
    fi
fi
