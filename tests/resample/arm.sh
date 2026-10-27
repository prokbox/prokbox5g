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

# ARM half-band stages under qemu-system-arm (P19 19-13): bit-exactness of
# resample_hb_arm.S against the C reference (a failure exits 1), then the
# executed instruction count and the ARM7TDMI cycle estimate per sample for
# each stage (tests/resample/cyc_plugin.c). Exits 0 with a note when
# arm-none-eabi-gcc or qemu-system-arm (with its plugin header) is missing.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
B=build/resample-arm
H=scripts/risk/qemu-harness
CC=arm-none-eabi-gcc
mkdir -p "$B"
command -v "$CC" >/dev/null 2>&1 || { echo "note: no $CC; ARM checks skipped"; exit 0; }
command -v qemu-system-arm >/dev/null 2>&1 || { echo "note: no qemu-system-arm; ARM checks skipped"; exit 0; }
V=$(qemu-system-arm --version | sed -n 's/.*version \([0-9.]*\).*/\1/p;q')
PH=""
for p in /opt/homebrew/Cellar/qemu/"$V"/include /usr/local/Cellar/qemu/"$V"/include; do
    [ -f "$p/qemu-plugin.h" ] && PH=$p && break
done
[ -n "$PH" ] || { echo "note: no qemu-plugin.h; ARM checks skipped"; exit 0; }
# shellcheck disable=SC2046
cc -shared -fPIC -O2 -undefined dynamic_lookup -I"$PH" \
   $(pkg-config --cflags glib-2.0 2>/dev/null) \
   tests/resample/cyc_plugin.c -o "$B/cyc.dylib" || exit 1
CF="-mcpu=arm7tdmi -O2 -ffreestanding -fno-builtin -DRESAMPLE_HB_STANDALONE -Ilib/rbcodec/dsp -Wall -Wextra"
$CC -mcpu=arm7tdmi -c "$H/start.S" -o "$B/start.o" || exit 1
$CC $CF -c lib/rbcodec/dsp/resample_hb.c -o "$B/hb_c.o" || exit 1
$CC -mcpu=arm7tdmi -DRESAMPLE_HB_STANDALONE -c lib/rbcodec/dsp/resample_hb_arm.S -o "$B/hb_arm.o" || exit 1
$CC $CF -c tests/npscreen/insn_libc.c -o "$B/libc.o" || exit 1

run() { # run WL [extra objects/flags] -> qemu stdout+stderr
    rwl=$1; shift
    $CC $CF -DWL="$rwl" ${HFLAG:-} -c tests/resample/arm_main.c -o "$B/m$rwl.o" &&
    $CC -mcpu=arm7tdmi -nostdlib -ffreestanding -Wl,-T,"$H/link.ld" -Wl,--no-warn-rwx-segments \
        "$B/start.o" "$B/m$rwl.o" "$B/hb_c.o" "$B/hb_arm.o" "$B/libc.o" "$@" \
        -lgcc -o "$B/wl$rwl.elf" || return 1
    timeout 120 qemu-system-arm -M versatilepb -cpu arm926 -nographic \
        -semihosting -plugin "./$B/cyc.dylib" -kernel "$B/wl$rwl.elf" 2>&1
}

out=$(run 0)
case "$out" in
    *"ARM==C ok"*) echo "ok   ARM == C, bit for bit: $(echo "$out" | sed -n 's/.*ARM==C ok \([0-9a-f]*\).*/\1/p')";;
    *) echo "FAIL ARM vs C: $out"; exit 1;;
esac

NS=4032
base=$(run 99)
bi=$(echo "$base" | sed -n 's/^INSN_COUNT //p')
bc=$(echo "$base" | sed -n 's/^CYC_COUNT //p')
echo "per output sample, per channel (ARM7TDMI model; $NS-sample blocks of 96):"
printf '  %-22s %10s %10s %10s\n' stage "ARM insns" "ARM cyc" "C cyc"
for wl in 1:"x2 hb_t44 (4+3)" 2:"x2 hb_t48 (3+3)" 3:"x2 hb_r (2+1)" \
          4:"/2 hb_t44 (4+3)" 5:"/2 hb_t48 (3+3)" 6:"/2 hb_r (2+1)"; do
    n=${wl%%:*}; name=${wl#*:}
    a=$(run "$n"); c=$(run $((n + 100)))
    ai=$(echo "$a" | sed -n 's/^INSN_COUNT //p')
    ac=$(echo "$a" | sed -n 's/^CYC_COUNT //p')
    cc_=$(echo "$c" | sed -n 's/^CYC_COUNT //p')
    outs=$NS; [ "$n" -le 3 ] && outs=$((2 * NS))
    printf '  %-22s %10s %10s %10s\n' "$name" \
        "$(echo "scale=1; ($ai - $bi) / $outs" | bc)" \
        "$(echo "scale=1; ($ac - $bc) / $outs" | bc)" \
        "$(echo "scale=1; ($cc_ - $bc) / $outs" | bc)"
done

# Upstream Hermite for comparison: the ARM code from a firmware build
DA=""
for d in build/fw-ui-mod build/fw-ui build/fw; do
    [ -f "$d/lib/rbcodec/dsp/dsp_arm.o" ] && DA=$d/lib/rbcodec/dsp/dsp_arm.o && break
done
if [ -z "$DA" ]; then
    echo "note: no firmware build with dsp_arm.o; Hermite figures skipped"
    exit 0
fi
printf '  %-22s %10s %10s\n' "Hermite ($DA)" "ARM insns" "ARM cyc"
for wl in 7:"x2" 8:"/2" 9:"4:3 (x3 path)" 10:"2:3 (x6 path, 3:2)" \
          11:"8:3 (3:2 rsq=2)" 12:"x4" 13:"/4" 14:"x3" 15:"x6"; do
    n=${wl%%:*}; name=${wl#*:}
    a=$(HFLAG=-DHERMITE=1 run "$n" "$DA")
    outs=$(printf '%d' "0x$(echo "$a" | sed -n 's/^OUT \([0-9a-f]*\).*/\1/p')")
    ai=$(echo "$a" | sed -n 's/^INSN_COUNT //p')
    ac=$(echo "$a" | sed -n 's/^CYC_COUNT //p')
    printf '  %-22s %10s %10s\n' "$name" \
        "$(echo "scale=1; ($ai - $bi) / $outs" | bc)" \
        "$(echo "scale=1; ($ac - $bc) / $outs" | bc)"
done
