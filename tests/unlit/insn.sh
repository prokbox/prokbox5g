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

# Dynamic ARM instruction counts for the unlit face's per-event work
# (Phase 8a U2; lean rule 8), measured with qemu-system-arm and the risk
# tool's TCG plugin (scripts/risk/qemu-harness, built by
# scripts/risk/arm_insn_count.sh into build/risk/qemu/libinsn.dylib).
#
#   tests/unlit/insn.sh
#
# Builds tests/unlit/insn_main.c per workload (WL 0 = baseline) with
# apps/ipodui/unlit_core.c at -mcpu=arm7tdmi -Os, twice: FILL=1 stores
# every rect pixel with a plain 16-bit loop (upper bound of lcd_fillrect),
# FILL=0 counts the core alone. A measurement, not a gate: exits 0 when a
# tool is missing. Not run by scripts/test.sh (not named run.sh).
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
B=build/unlit-insn
H=scripts/risk/qemu-harness
PLUGIN=build/risk/qemu/libinsn.dylib
CC=arm-none-eabi-gcc
REPS=8
mkdir -p "$B"

command -v "$CC" >/dev/null 2>&1 || { echo "no $CC"; exit 0; }
command -v qemu-system-arm >/dev/null 2>&1 || { echo "no qemu-system-arm"; exit 0; }
if [ ! -f "$PLUGIN" ]; then
    echo "building the TCG plugin via scripts/risk/arm_insn_count.sh"
    scripts/risk/arm_insn_count.sh >/dev/null 2>&1
fi
[ -f "$PLUGIN" ] || { echo "no $PLUGIN (see scripts/risk/arm_insn_count.sh)"; exit 0; }

CF="-mcpu=arm7tdmi -Os -ffreestanding -fno-builtin -Itests/unlit/insn_inc -Itests/unlit/stub -Iapps"
$CC -mcpu=arm7tdmi -c "$H/start.S" -o "$B/start.o" || exit 0
$CC $CF -c tests/npscreen/insn_libc.c -o "$B/libc_shim.o" || exit 0
$CC $CF -c apps/ipodui/unlit_core.c -o "$B/unlit_core.o" || exit 0

count() {
    wl=$1 fill=$2
    $CC $CF -DWL="$wl" -DFILL="$fill" -c tests/unlit/insn_main.c \
            -o "$B/main$wl-$fill.o" \
        && $CC -mcpu=arm7tdmi -nostdlib -ffreestanding -Wl,-T,"$H/link.ld" \
            "$B/start.o" "$B/main$wl-$fill.o" "$B/unlit_core.o" \
            "$B/libc_shim.o" -lgcc -o "$B/wl$wl-$fill.elf" 2>/dev/null \
        || { echo "x"; return; }
    timeout 60 qemu-system-arm -M versatilepb -cpu arm926 -nographic \
        -semihosting -plugin "./$PLUGIN" -kernel "$B/wl$wl-$fill.elf" 2>&1 \
        | sed -n 's/^INSN_COUNT //p' | tail -1
}

for fill in 0 1; do
    base=$(count 0 $fill)
    echo "FILL=$fill baseline (setup) $base instructions"
    for wl in 1:minute_one_cell 2:minute_row_recentre 3:status_row \
              4:first_show 5:state_event; do
        n=${wl%%:*}
        name=${wl#*:}
        c=$(count "$n" $fill)
        if [ -n "$c" ] && [ "$c" != x ] && [ -n "$base" ]; then
            echo "  $name: $(( (c - base) / REPS )) instructions per event"
        else
            echo "  $name: not measured"
        fi
    done
done
