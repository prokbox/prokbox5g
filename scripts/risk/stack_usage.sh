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

# Risk tool 3: worst-case call-chain stack depth (docs/risk-tooling.md).
#
#   scripts/risk/stack_usage.sh
#
# Builds fw-ui (--target=22 --type=n --ipodui --compiler-prefix=arm-none-eabi-)
# into a SCRATCH dir, build/risk-stack-fw (never build/fw-ui), with gcc's
# -fstack-usage -fcallgraph-info=su,da added to the generated (gitignored)
# Makefile's GCCOPTS: one .ci file per translation unit, each function's
# own worst-case stack frame size plus its direct call graph. Then
# scripts/risk/lib/stack_depth.py walks that graph from each thread's
# entry function (grep'd below from its create_thread() call site, so
# the stack-size side of the comparison tracks the real source, not a
# copied-in number) to the deepest call chain, and flags any thread over
# ~70% of its declared stack.
#
# Exit status is always 0 (a report, not a gate).
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
B=build/risk-stack-fw
step() { printf '\n== %s\n' "$*"; }

step "1. build/risk-stack-fw (scratch fw-ui, -fstack-usage -fcallgraph-info=su,da)"
if [ ! -f "$B/Makefile" ]; then
    mkdir -p "$B"
    (cd "$B" && ../../tools/configure --target=22 --type=n --ipodui \
        --compiler-prefix=arm-none-eabi- </dev/null \
        > configure.log 2>&1)
fi
if ! grep -q 'fcallgraph-info' "$B/Makefile" 2>/dev/null; then
    sed -i.bak 's/^export GCCOPTS=.*/& -fstack-usage -fcallgraph-info=su,da/' \
        "$B/Makefile"
fi
if make -C "$B" -j10 > "$B/make.log" 2>&1; then
    echo "ok   $B built ($(find "$B" -name '*.ci' | wc -l | tr -d ' ') .ci files)"
else
    echo "FAIL: $B did not build; see $B/make.log"
    exit 0
fi

step "2. thread entry points and their declared stack sizes"
# DEFAULT_STACK_SIZE for this target: firmware/asm/arm/thread-classic.h,
# the final "#else ... 0x400 ... #endif" (PLATFORM_NATIVE, i.e. not
# PLATFORM_HOSTED/DX50/DX90 -- ipodvideo is native). Read, not computed:
# a plain grep can't tell which #else/#endif nesting level applies.
DEFAULT_STACK_SIZE=0x400
echo "DEFAULT_STACK_SIZE = $DEFAULT_STACK_SIZE (firmware/asm/arm/thread-classic.h: the PLATFORM_NATIVE branch -- verify by hand if that file's #ifdef nesting changes)"

# main/UI thread: apps/ipodui runs on it directly (apps/main.c calls
# ipodui_main(); no create_thread() for the shell itself). Its stack is
# the linker-script .stack section, firmware/target/arm/pp/app-pp.lds.
MAIN_STACK=$(awk '/\.stack \(NOLOAD\)/{f=1} f && /\+= 0x/{print $3; exit}' \
    firmware/target/arm/pp/app-pp.lds | tr -d ';')
MAIN_STACK=${MAIN_STACK:-0x2000}
echo "main/UI thread (ipodui_main, apps/main.c):   stack = $MAIN_STACK (app-pp.lds .stack)"

report_thread() { # label, entry-fn, stack-expr (as C arithmetic text)
    printf '%-38s stack = %s\n' "$1 ($2)" "$3"
}
report_thread "tagcache thread"   tagcache_thread   "DEFAULT_STACK_SIZE + 0x4000 (apps/tagcache.c)"
report_thread "audio thread"      audio_thread      "DEFAULT_STACK_SIZE + 0x1000 (apps/audio_thread.c)"
report_thread "codec thread"      codec_thread      "DEFAULT_STACK_SIZE + 0x2000 + WORKAROUND_FS13060(0) (apps/codec_thread.c)"
report_thread "buffering thread"  buffering_thread  "DEFAULT_STACK_SIZE + 0x2000 (apps/buffering.c)"
report_thread "playlist thread"   dc_thread_playlist "DEFAULT_STACK_SIZE + 0x800 (apps/playlist.c)"
report_thread "piezo thread"      piezo_thread      "DEFAULT_STACK_SIZE (firmware/target/arm/ipod/piezo.c)"
echo "(art builder thread: not present on main yet -- Phase 4b, in"
echo " progress on other branches as of 2026-10-06; add it here, same"
echo " pattern, once apps/ipodui/art/*.c creates one)"

step "3. worst-case call-chain depth (scripts/risk/lib/stack_depth.py)"
python3 scripts/risk/lib/stack_depth.py "$B" \
    ipodui_main tagcache_thread audio_thread codec_thread \
    buffering_thread dc_thread_playlist piezo_thread
exit 0
