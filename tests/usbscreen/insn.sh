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

# Dynamic ARM instruction counts for the USB screen's pixel work
# (docs/phase8a-brief.md §3.2, lean rule 8), measured like
# tests/npscreen/insn.sh: qemu-system-arm + the risk tool's TCG plugin
# (build/risk/qemu/libinsn.dylib via scripts/risk/arm_insn_count.sh).
#
#   tests/usbscreen/insn.sh
#
# A measurement, not a gate (not run by scripts/test.sh, which only runs
# run.sh): exits 0 when a tool is missing.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
B=build/usbscreen-insn
H=scripts/risk/qemu-harness
PLUGIN=build/risk/qemu/libinsn.dylib
CC=arm-none-eabi-gcc
mkdir -p "$B"

command -v "$CC" >/dev/null 2>&1 || { echo "no $CC"; exit 0; }
command -v qemu-system-arm >/dev/null 2>&1 || { echo "no qemu-system-arm"; exit 0; }
if [ ! -f "$PLUGIN" ]; then
    echo "building the TCG plugin via scripts/risk/arm_insn_count.sh"
    scripts/risk/arm_insn_count.sh >/dev/null 2>&1
fi
[ -f "$PLUGIN" ] || { echo "no $PLUGIN (see scripts/risk/arm_insn_count.sh)"; exit 0; }

CF="-mcpu=arm7tdmi -Os -ffreestanding -fno-builtin -Iapps/ipodui"
$CC -mcpu=arm7tdmi -c "$H/start.S" -o "$B/start.o" || exit 0
$CC $CF -c tests/npscreen/insn_libc.c -o "$B/libc_shim.o" || exit 0

count() {
    wl=$1
    $CC $CF -DWL="$wl" -c tests/usbscreen/insn_main.c -o "$B/main$wl.o" \
        && $CC -mcpu=arm7tdmi -nostdlib -ffreestanding -Wl,-T,"$H/link.ld" \
            "$B/start.o" "$B/main$wl.o" "$B/libc_shim.o" -lgcc \
            -o "$B/wl$wl.elf" 2>/dev/null \
        || { echo "x"; return; }
    timeout 120 qemu-system-arm -M versatilepb -cpu arm926 -nographic \
        -semihosting -plugin "./$PLUGIN" -kernel "$B/wl$wl.elf" 2>&1 \
        | sed -n 's/^INSN_COUNT //p' | tail -1
}

base=$(count 0)
echo "baseline (setup) $base instructions"
for wl in 1:entry_compose 2:static_body 3:arrow_frame_blit \
          4:idle_disc_direct 5:one_arrow_map; do
    n=${wl%%:*}
    name=${wl#*:}
    c=$(count "$n")
    if [ -n "$c" ] && [ "$c" != x ] && [ -n "$base" ]; then
        echo "$name: $(( c - base )) instructions"
    else
        echo "$name: not measured"
    fi
done
