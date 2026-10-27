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

# Dynamic ARM instruction counts for Now Playing's per-event work
# (docs/phase5-brief.md §7 item 3), measured with qemu-system-arm and the
# risk tool's TCG plugin (scripts/risk/qemu-harness, built by
# scripts/risk/arm_insn_count.sh into build/risk/qemu/libinsn.dylib).
#
#   tests/npscreen/insn.sh
#
# Builds tests/npscreen/insn_main.c once per workload (WL 0 = baseline)
# with apps/ipodui/np_{draw,state,scrub,lyrics}.c at -mcpu=arm7tdmi -Os
# into build/npscreen-insn/ and prints instructions per event. A
# measurement, not a gate: exits 0 when a tool is missing.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
B=build/npscreen-insn
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

CF="-mcpu=arm7tdmi -Os -ffreestanding -fno-builtin -Iapps/ipodui -Ilib/rbcodec/metadata -Itests/npscreen/insn_inc -DHAVE_IPOD_UI"
$CC -mcpu=arm7tdmi -c "$H/start.S" -o "$B/start.o" || exit 0
$CC $CF -c tests/npscreen/insn_libc.c -o "$B/libc_shim.o" || exit 0
for f in np_draw np_state np_scrub np_lyrics ab_book_core; do
    $CC $CF -c "apps/ipodui/$f.c" -o "$B/$f.o" || exit 0
done
$CC $CF -c lib/rbcodec/metadata/abpack.c -o "$B/abpack.o" || exit 0

count() {
    wl=$1
    $CC $CF -DWL="$wl" -c tests/npscreen/insn_main.c -o "$B/main$wl.o" \
        && $CC -mcpu=arm7tdmi -nostdlib -ffreestanding -Wl,-T,"$H/link.ld" \
            "$B/start.o" "$B/main$wl.o" "$B/np_draw.o" "$B/np_state.o" \
            "$B/np_scrub.o" "$B/np_lyrics.o" "$B/ab_book_core.o" \
            "$B/abpack.o" "$B/libc_shim.o" -lgcc \
            -o "$B/wl$wl.elf" 2>/dev/null \
        || { echo "x"; return; }
    timeout 60 qemu-system-arm -M versatilepb -cpu arm926 -nographic \
        -semihosting -plugin "./$PLUGIN" -kernel "$B/wl$wl.elf" 2>&1 \
        | sed -n 's/^INSN_COUNT //p' | tail -1
}

base=$(count 0)
echo "baseline (setup) $base instructions"
for wl in 1:art_slant_reflection 2:placeholder_image 3:tick_1hz_audiobook \
          4:scrub_event 5:volume_event 6:marquee_step 7:rating_event \
          8:lyrics_step_move 9:lyrics_wrap_8k 10:chapter_ticks_648 \
          11:row_slide_frame 12:scrub_event_fast_snap \
          13:book_scrub_fast_snap_1024 14:book_ticks_scale \
          15:tick_1hz_book 16:qw2_trackchange_blit \
          17:qw2_placeholder_decode 18:qw2_builder_compose; do
    n=${wl%%:*}
    name=${wl#*:}
    c=$(count "$n")
    if [ -n "$c" ] && [ "$c" != x ] && [ -n "$base" ]; then
        echo "$name: $(( (c - base) / REPS )) instructions per event"
    else
        echo "$name: not measured"
    fi
done
