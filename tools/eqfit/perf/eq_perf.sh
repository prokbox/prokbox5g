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

# The software EQ's CPU cost (docs/phase15-brief.md section 3.1, lean
# rule 8): dynamic ARM instruction count for 1 s of 44.1 kHz stereo
# through the 10-band EQ plus its precut, using the firmware's own
# lib/rbcodec/dsp/dsp_arm.o from build/fw-ui and the risk tools' qemu
# instruction-count plugin (scripts/risk/arm_insn_count.sh step 3).
#   tools/eqfit/perf/eq_perf.sh
# Needs build/fw-ui (the build skill), arm-none-eabi-gcc and Homebrew qemu.
set -eu
cd "$(dirname "$0")/../../.."
B=build/eqfit/perf
H=scripts/risk/qemu-harness
OBJ=build/fw-ui/lib/rbcodec/dsp/dsp_arm.o
mkdir -p "$B"
[ -f "$OBJ" ] || { echo "FAIL: $OBJ missing: build build/fw-ui first"; exit 1; }
command -v qemu-system-arm >/dev/null || { echo "FAIL: no qemu-system-arm"; exit 1; }

PLUGIN=build/risk/qemu/libinsn.dylib
if [ ! -f "$PLUGIN" ]; then
    V=$(qemu-system-arm --version | sed -n 's/.*version \([0-9.]*\).*/\1/p;q')
    PH=/opt/homebrew/Cellar/qemu/$V/include
    mkdir -p build/risk/qemu
    # shellcheck disable=SC2046
    cc -shared -fPIC -O2 -undefined dynamic_lookup -I"$PH" \
        $(pkg-config --cflags glib-2.0) "$H/libinsn.c" -o "$PLUGIN"
fi

cat > "$B/link.ld" <<'EOF'
ENTRY(_start)
SECTIONS
{
    . = 0x00000000;
    .text : { *(.text*) *(.icode*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(.bss*) }
}
EOF
arm-none-eabi-gcc -mcpu=arm7tdmi -c "$H/start.S" -o "$B/start.o"

run() {     # bands pga -> instruction count
    arm-none-eabi-gcc -mcpu=arm7tdmi -O2 -ffreestanding -nostdlib \
        -DBANDS="$1" -DPGA="$2" -Wl,-T,"$B/link.ld" "$B/start.o" \
        tools/eqfit/perf/eq_main.c "$OBJ" -lgcc -Wl,--no-warn-rwx-segments \
        -o "$B/eq_$1_$2.elf"
    timeout 120 qemu-system-arm -M versatilepb -cpu arm926 -nographic \
        -semihosting -plugin "./$PLUGIN" -kernel "$B/eq_$1_$2.elf" 2>&1 |
        sed -n 's/.*INSN_COUNT[^0-9]*\([0-9][0-9]*\).*/\1/p' | head -1
}
base=$(run 0 0)
pga=$(run 0 1)
eq10=$(run 10 1)
eq1=$(run 1 0)
echo "baseline (boot, 345 blocks, no work): $base instructions"
awk -v b="$base" -v p="$pga" -v e="$eq10" -v o="$eq1" 'BEGIN {
    band = o - b; pg = p - b; tot = e - b;
    printf "one band, 1 s 44.1 kHz stereo:     %d instructions (%.2f per sample)\n", band, band / 88200
    printf "precut PGA pass:                   %d instructions\n", pg
    printf "10 bands + precut, 1 s:            %d instructions = %.1f MIPS\n", tot, tot / 1e6
    # ARM7TDMI cycles per filter sample from the .fp_loop body: smull
    # (m=4: 6 cycles) + 4 smlal (7 each) + ldr 3 + str 2 + 3 mov/subs 1 +
    # taken branch 3 = 46 (IRAM, no wait states); PGA: ldm 4 + 2 smull
    # (m=3: 5) + 4 alu + stm 3 + subs + branch 3 = ~25 per pair
    cyc = 46 * 88200 * 10 + 12.5 * 88200
    printf "ARM7TDMI cycle estimate:           %.1f MHz of the PP5022 core\n", cyc / 1e6
}'
