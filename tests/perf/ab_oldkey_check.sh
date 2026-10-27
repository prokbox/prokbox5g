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

# #47: a multi-file book whose art key changed with the shared case folding
# ("ÉTÉ Multi" by "ÉMILE Auteur") still opens with its chapters right after
# the upgrade. Run 1 (tests/perf/ab-oldkey-1.script) boots as the firmware
# before #47 (PROKBOX_SIM_ASCII_ARTKEY) and files the book in books.pak;
# run 2 (ab-oldkey-2.script) is the upgrade boot: the page must hit the old
# key ("ab pak hit-old"), with no miss. A copy of build/sim-ui's disk in
# build/sim-abold keeps the test library of the other scripts untouched.
#
#   tests/perf/ab_oldkey_check.sh
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
UI=$ROOT/build/sim-ui
B=$ROOT/build/sim-abold
[ -x "$UI/rockboxui" ] || { echo "build build/sim-ui first" >&2; exit 2; }
rm -rf "$B"
mkdir -p "$B"
ln -sf "$UI/rockboxui" "$B/rockboxui"
[ -f "$UI/UI256.bmp" ] && ln -sf "$UI/UI256.bmp" "$B/UI256.bmp"
rsync -a "$UI/simdisk/" "$B/simdisk/"
SIM_BUILD=$B "$ROOT/scripts/shots.sh" "$ROOT/tests/perf/ab-oldkey-1.script" \
    >/dev/null 2>&1 || { echo "FAIL run 1"; exit 1; }
L=$ROOT/out/shots/ab-oldkey-2/sim.log
if SIM_BUILD=$B "$ROOT/scripts/shots.sh" \
        "$ROOT/tests/perf/ab-oldkey-2.script" >/dev/null 2>&1 \
   && grep -q "lib precompute done" "$L" \
   && grep -q "ab pak hit-old" "$L" && ! grep -q "ab pak miss" "$L"; then
    echo "PASS book chapters after the upgrade: $(grep -o 'ab pak hit-old[^ ]* book=[0-9a-f]*' "$ROOT/out/shots/ab-oldkey-2/sim.log" | head -1)"
    rm -rf "$B"
else
    echo "FAIL run 2 (out/shots/ab-oldkey-2/sim.log)"
    exit 1
fi
