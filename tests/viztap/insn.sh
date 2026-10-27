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

# Dynamic ARM instruction counts for the visualizer's PCM tap and its CPU
# fallback (visualizer.md 11.5), measured as tests/usbscreen/insn.sh is:
# qemu-system-arm with the risk tool's TCG plugin. A measurement, not a
# gate (scripts/test.sh runs only run.sh): exits 0 when a tool is missing.
#   tests/viztap/insn.sh [-Os|-O2]
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
OPT=${1:--O2}
B=build/viztap-insn
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

CF="-mcpu=arm7tdmi $OPT -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns -Itests/viztap/armstub -Iapps/ipodui -Ibcmfw/jobs"
$CC -mcpu=arm7tdmi -c "$H/start.S" -o "$B/start.o" || exit 0
# the firmware's own memcpy (IRAM, ldm/stm), which the tap's writer calls
$CC -mcpu=arm7tdmi -Itests/viztap/armstub -c firmware/asm/arm/memcpy.S \
    -o "$B/memcpy.o" || exit 0
$CC $CF -c apps/ipodui/viz_tap_core.c -o "$B/core.o" || exit 0
$CC $CF -c bcmfw/jobs/jviz.c -o "$B/jviz.o" || exit 0

count() {
    $CC $CF -DWL="$1" -c tests/viztap/insn_main.c -o "$B/main$1.o" \
        && $CC -mcpu=arm7tdmi -nostdlib -ffreestanding -Wl,-T,"$H/link.ld" \
            "$B/start.o" "$B/main$1.o" "$B/core.o" "$B/jviz.o" \
            "$B/memcpy.o" -lgcc -o "$B/wl$1.elf" 2>/dev/null \
        || { echo "x"; return; }
    timeout 120 qemu-system-arm -M versatilepb -cpu arm926 -nographic \
        -semihosting -plugin "./$PLUGIN" -kernel "$B/wl$1.elf" 2>&1 \
        | sed -n 's/^INSN_COUNT //p' | tail -1
}

base=$(count 0)
echo "$OPT: baseline (setup) $base instructions"
for wl in 1:write_one_mixer_frame_256 2:read_512 3:read_512_norm \
          4:cpu_fallback_spectrum_512_24bands \
          5:cpu_fallback_spectrum_256_12bands; do
    n=${wl%%:*}
    name=${wl#*:}
    c=$(count "$n")
    if [ -n "$c" ] && [ "$c" != x ] && [ -n "$base" ]; then
        echo "$name: $(( c - base )) instructions"
    else
        echo "$name: not measured"
    fi
done
