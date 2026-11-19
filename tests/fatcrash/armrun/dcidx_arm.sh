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

#
# tests/fatcrash/armrun/dcidx_arm.sh: build tests/fatcrash/dcidx.c with the
# firmware's compiler and its code-generation flags (arm-none-eabi-gcc, -Os,
# -mcpu=arm7tdmi, ARM state) as a bare program, and run it under
# qemu-system-arm with semihosting.  The #355 fold only shows in the 32-bit
# target compile, so this is the run that can go red on the old macro.
# Usage: dcidx_arm.sh <shim-include-dir> <out-dir> [disk_cache.c dir] [-O..]
# Prints dcidx's lines; exit 0 PASS, 1 FAIL, 2 could not build or run
# (the verdict line "dcidx: PASS" is required for 0).
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../../.." && pwd)
inc=$1; out=$2; src=${3:-$top/firmware/common}; opt=${4:--Os}
mkdir -p "$out"
cc=arm-none-eabi-gcc
qemu=qemu-system-arm
command -v $cc >/dev/null 2>&1 || { echo "dcidx-arm: no $cc"; exit 2; }
command -v $qemu >/dev/null 2>&1 || { echo "dcidx-arm: no $qemu"; exit 2; }
# the firmware's GCCOPTS code-generation part (tools/configure, PP5022)
flags="$opt -std=gnu11 -ffreestanding -nostdlib -funit-at-a-time \
 -fno-delete-null-pointer-checks -fno-strict-overflow -fno-common \
 -mcpu=arm7tdmi -marm -fomit-frame-pointer -mthumb-interwork -w"
elf="$out/dcidx-arm$opt.elf"
# shellcheck disable=SC2086
if ! $cc $flags -I"$here" -I"$src" -I"$inc" -I"$top/firmware/libc/include" \
        -include "$here/armrt.h" -T "$here/armrt.ld" -o "$elf" \
        "$top/tests/fatcrash/dcidx.c" "$top/firmware/common/linked_list.c" \
        "$here/armrt.c" -lgcc >"$out/dcidx-arm$opt.log" 2>&1; then
    echo "dcidx-arm: build failed"; head -20 "$out/dcidx-arm$opt.log"; exit 2
fi
log="$out/dcidx-arm$opt.run"
rm -f "$log"
$qemu -M versatilepb -m 64M -nographic -monitor none -serial null \
      -semihosting -kernel "$elf" >"$log" 2>&1 &
pid=$!
n=0
while kill -0 $pid 2>/dev/null && [ $n -lt 60 ]; do sleep 1; n=$((n + 1)); done
if kill -0 $pid 2>/dev/null; then
    kill $pid; echo "dcidx-arm: qemu timed out"; cat "$log"; exit 2
fi
wait $pid; rc=$?
cat "$log"
if grep -q '^dcidx: PASS' "$log" && [ $rc = 0 ]; then exit 0; fi
if grep -q '^dcidx: FAIL' "$log"; then exit 1; fi
echo "dcidx-arm: no verdict (qemu rc=$rc)"; exit 2
