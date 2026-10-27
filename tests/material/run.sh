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

# Host test of the shell's material kit (Phase 8b brief 3.1, 7):
#   - apps/ipodui/material_data.h equals a fresh run of art/materials.py,
#     and apps/ipodui/header_tiles.h one of apps/ipodui/art/header.py;
#   - apps/ipodui/material.c: every table equals its key evaluation, the
#     gels deepen then brighten, the header and the shadow fall off, and
#     Now Playing's gel, groove and reflection alphas equal the FROZEN
#     golden captured before the move (np_gel_golden.h, review M7).
# Pure C (no Rockbox headers). Compiler: the newest Homebrew gcc-NN, else
# cc; override with HOSTCC=...
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
T=tests/material
S=apps/ipodui

if [ -z "${HOSTCC:-}" ]; then
    HOSTCC=cc
    for v in 19 18 17 16 15 14; do
        if command -v gcc-$v >/dev/null 2>&1; then HOSTCC=gcc-$v; break; fi
    done
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail=0

if python3 -I art/materials.py "$TMP/material_data.h" >/dev/null &&
        cmp -s "$TMP/material_data.h" $S/material_data.h; then
    echo "ok   material_data.h is current (art/materials.py)"
else
    echo "FAIL: apps/ipodui/material_data.h is stale: python3 -I art/materials.py"
    fail=1
fi

if python3 -I $S/art/header.py "$TMP/header_tiles.h" >/dev/null &&
        cmp -s "$TMP/header_tiles.h" $S/header_tiles.h; then
    echo "ok   header_tiles.h is current (apps/ipodui/art/header.py)"
else
    echo "FAIL: apps/ipodui/header_tiles.h is stale: python3 -I apps/ipodui/art/header.py"
    fail=1
fi

# 8b-3: the panel art (stdlib only) and the calendar digits (Pillow and
# the source font, which is not in the repository: the header records its
# sha256; skipped with a note when either is missing or differs)
if python3 -I $S/art/panelart.py > "$TMP/panelart.h" &&
        cmp -s "$TMP/panelart.h" $S/panelart.h; then
    echo "ok   panelart.h is current (apps/ipodui/art/panelart.py)"
else
    echo "FAIL: apps/ipodui/panelart.h is stale: python3 -I apps/ipodui/art/panelart.py > apps/ipodui/panelart.h"
    fail=1
fi
# the Extras rows' icons (owner, 2026-10-24; stdlib only)
if python3 -I $S/art/extrasart.py > "$TMP/extrasart.h" &&
        cmp -s "$TMP/extrasart.h" $S/extrasart.h; then
    echo "ok   extrasart.h is current (apps/ipodui/art/extrasart.py)"
else
    echo "FAIL: apps/ipodui/extrasart.h is stale: python3 -I apps/ipodui/art/extrasart.py > apps/ipodui/extrasart.h"
    fail=1
fi
OTF=${HEROS_BOLD:-}
if [ -z "$OTF" ]; then
    COMMON=$(git rev-parse --git-common-dir 2>/dev/null || echo .git)
    OTF="${COMMON%/.git*}/out/fonts-src/heros/texgyreheros-bold.otf"
    case "$COMMON" in /*) ;; *) OTF="out/fonts-src/heros/texgyreheros-bold.otf" ;; esac
fi
WANT=$(sed -n 's/.* sha256 \([0-9a-f]*\).*/\1/p' $S/caldigits.h)
if [ ! -f "$OTF" ]; then
    echo "note caldigits.h not checked: no source font (set HEROS_BOLD=<texgyreheros-bold.otf>)"
elif ! python3 -c 'import PIL' 2>/dev/null; then
    echo "note caldigits.h not checked: no Pillow"
elif [ "$(shasum -a 256 "$OTF" | cut -d' ' -f1)" != "$WANT" ]; then
    echo "note caldigits.h not checked: $OTF is not the recorded source font"
elif python3 -I $S/art/caldigits.py "$OTF" "$TMP/caldigits.h" >/dev/null &&
        cmp -s "$TMP/caldigits.h" $S/caldigits.h; then
    echo "ok   caldigits.h is current (apps/ipodui/art/caldigits.py)"
else
    echo "FAIL: apps/ipodui/caldigits.h is stale: python3 apps/ipodui/art/caldigits.py <texgyreheros-bold.otf>"
    fail=1
fi

# shellcheck disable=SC2086
if ! "$HOSTCC" -std=gnu99 -O1 -g -Wall -Wextra -Werror ${EXTRA_CFLAGS:-} \
        -I$S -I$T -o "$TMP/test_material" \
        $T/test_material.c $S/material.c 2>"$TMP/cc.log"; then
    echo "FAIL: compiling test_material ($HOSTCC)"
    cat "$TMP/cc.log"
    exit 1
fi
if "$TMP/test_material"; then
    echo "ok   material kit (material.c)"
else
    echo "FAIL: material kit"
    fail=1
fi
exit $fail
