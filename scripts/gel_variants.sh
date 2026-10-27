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

# gel-variants (fidelity audit row 12): build the simulator once per
# selection-bar / battery candidate (UI_MAT_SEL_VARIANT 1 A, 2 B, 3 C;
# art/materials.py SEL_C), shoot tests/ui/ipodui-gel-variants.script with
# each, then compose the owner comparison sheet (scripts/gel_sheet.py).
#
#   scripts/gel_variants.sh [SIM_BUILD]     # default build/sim-ui
#
# One build dir: only material.c and header.c read the switch, so their
# objects are dropped and make reruns with the define appended to the
# configured EXTRA_DEFINES. The dir is left built as variant B (the
# default). Output: out/shots/gel-{A,B,C}/ and out/review/gel-variants/.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
B=${1:-build/sim-ui}
DEFS=$(sed -n 's/^export EXTRA_DEFINES=//p' "$B/Makefile")
[ -n "$DEFS" ] || { echo "gel_variants: $B is not configured" >&2; exit 1; }

build() {
    find "$B/apps/ipodui" \( -name 'material.o' -o -name 'header.o' \) -delete
    make -C "$B" -j8 EXTRA_DEFINES="$DEFS -DUI_MAT_SEL_VARIANT=$1" \
        > "$B/make-gel$1.log" 2>&1 ||
        { echo "gel_variants: build $1 failed ($B/make-gel$1.log)" >&2; exit 1; }
}

for v in 1:A 2:B 3:C; do
    n=${v%%:*}; name=${v#*:}
    build "$n"
    SIM_BUILD="$B" scripts/shots.sh tests/ui/ipodui-gel-variants.script \
        "out/shots/gel-$name" >/dev/null
    echo "gel_variants: variant $name shot (out/shots/gel-$name)"
done
build 2
python3 scripts/gel_sheet.py
