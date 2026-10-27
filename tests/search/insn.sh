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

# Dynamic ARM instruction counts of srch_match() per haystack byte
# (docs/phase11-brief.md section 7 criterion 3: <= 6 instructions per
# byte), measured with qemu-system-arm and the risk tool's TCG plugin
# (scripts/risk/qemu-harness; scripts/risk/arm_insn_count.sh builds
# build/risk/qemu/libinsn.dylib). Compiles apps/ipodui/search_core.c at
# -mcpu=arm7tdmi -Os as the firmware does, runs tests/search/insn_main.c
# per workload (WL 0 = baseline) and prints instructions per byte and per
# title, and the per-keystroke estimate over the 1.28 MB haystack of 50k
# titles plus the group labels (section 6 item 3). A measurement, not a
# gate: exits 0 when a tool is missing. Not run by scripts/test.sh.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
B=build/search-insn
H=scripts/risk/qemu-harness
PLUGIN=build/risk/qemu/libinsn.dylib
CC=arm-none-eabi-gcc
REPS=4
mkdir -p "$B"

command -v "$CC" >/dev/null 2>&1 || { echo "no $CC"; exit 0; }
command -v qemu-system-arm >/dev/null 2>&1 || { echo "no qemu-system-arm"; exit 0; }
if [ ! -f "$PLUGIN" ]; then
    echo "building the TCG plugin via scripts/risk/arm_insn_count.sh"
    scripts/risk/arm_insn_count.sh >/dev/null 2>&1
fi
[ -f "$PLUGIN" ] || { echo "no $PLUGIN (see scripts/risk/arm_insn_count.sh)"; exit 0; }

CF="-mcpu=arm7tdmi -Os -ffreestanding -fno-builtin -Itests/search/insn_inc -Iapps/ipodui"
$CC -mcpu=arm7tdmi -c "$H/start.S" -o "$B/start.o" || exit 0
$CC $CF -c tests/npscreen/insn_libc.c -o "$B/libc_shim.o" || exit 0
$CC $CF -c apps/ipodui/search_core.c -o "$B/search_core.o" || exit 0
$CC $CF -c apps/ipodui/sortkey.c -o "$B/sortkey.o" || exit 0

count() {
    wl=$1
    $CC $CF -DWL="$wl" -c tests/search/insn_main.c -o "$B/main$wl.o" \
        && $CC -mcpu=arm7tdmi -nostdlib -ffreestanding -Wl,-T,"$H/link.ld" \
            "$B/start.o" "$B/main$wl.o" "$B/search_core.o" "$B/sortkey.o" \
            "$B/libc_shim.o" -lgcc -o "$B/wl$wl.elf" 2>/dev/null \
        || { echo "x"; return; }
    timeout 60 qemu-system-arm -M versatilepb -cpu arm926 -nographic \
        -semihosting -plugin "./$PLUGIN" -kernel "$B/wl$wl.elf" 2>&1 \
        | sed -n 's/^INSN_COUNT //p' | tail -1
}

# corpus bytes (each title with its NUL) and titles, from insn_main.c
stats=$(sed -n '/^static const char \*const titles/,/^};/p' \
        tests/search/insn_main.c | grep -o '"[^"]*"' \
        | LC_ALL=C awk '{ n += length($0) - 1; t++ } END { print n, t }')
nb=${stats% *}
nt=${stats#* }
base=$(count 0)
echo "corpus: $nt titles, $nb bytes (x$REPS); baseline $base instructions"
c3=
for wl in 1:first_char_h 2:refine_ha 3:rare_q 4:two_words; do
    n=${wl%%:*}
    name=${wl#*:}
    c=$(count "$n")
    if [ -n "$c" ] && [ "$c" != x ] && [ -n "$base" ]; then
        d=$((c - base))
        [ "$n" = 3 ] && c3=$c
        echo "$name: $(( d / REPS / nt )) instructions per title," \
             "$(( d * 100 / REPS / nb )) / 100 per byte;" \
             "1.28 MB keystroke: ~$(( d * 1280 / REPS / nb / 1000 )) M instructions"
    else
        echo "$name: not measured"
    fi
done
# the marginal cost per byte: each title doubled adds (bytes - titles)
# bytes (the NULs are counted once; the joining space adds one per title)
c5=$(count 5)
if [ -n "$c5" ] && [ "$c5" != x ] && [ -n "$c3" ]; then
    echo "hot loop (doubled titles, rare_q): $(( (c5 - c3) * 100 / REPS / nb ))" \
         "/ 100 instructions per added byte;" \
         "fixed per call: $(( ((c3 - base) / REPS - (c5 - c3) / REPS) / nt ))" \
         "instructions per title"
fi
