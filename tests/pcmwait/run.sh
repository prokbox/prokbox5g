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

# Host test of #312: the PCM driver's waits are bounded with the codec held
# in standby, and a start wakes the codec first.  Extracts the REAL code
# (no copy kept here) from firmware/target/arm/pp/pcm-pp.c (the pcm-wait
# block, dma_tx_stop, pq_stop, sink_stop_pcm) and firmware/pcm.c
# (pcm_play_data), runs it against the register model of test_pcmwait.c,
# then each mutant in spec_pcmwait.py, which must turn it red.
# PCMWAIT_PP / PCMWAIT_PCM point at other sources (the red proof against an
# older tree: PCMWAIT_OLD=1 allows the missing pcm-wait block and expects
# red).  Compiler: the newest Homebrew gcc-NN, else cc; HOSTCC overrides.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
T=tests/pcmwait
PP=${PCMWAIT_PP:-firmware/target/arm/pp/pcm-pp.c}
PCMC=${PCMWAIT_PCM:-firmware/pcm.c}
OLD=${PCMWAIT_OLD:-0}
if [ -z "${HOSTCC:-}" ]; then
    HOSTCC=cc
    for v in 19 18 17 16 15 14; do
        if command -v gcc-$v >/dev/null 2>&1; then HOSTCC=gcc-$v; break; fi
    done
fi
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail=0

# take NAME FILE START_RE: START_RE's one line through the next "}" at
# column 0; an empty or ambiguous result fails the test
take() {
    n=$(grep -c -E "$3" "$2")
    if [ "$n" != 1 ]; then
        echo "FAIL: $1: $n lines match in $2 (the extraction must see the real code)"
        fail=1; return
    fi
    awk -v re="$3" '!on && $0 ~ re { on = 1 } on { print } on && /^}/ { exit }' \
        "$2" >>"$TMP/parts.c"
    echo >>"$TMP/parts.c"
}

: >"$TMP/parts.c"
awk '/pcm-wait: begin/ { on = 1; next } /pcm-wait: end/ { on = 0 } on' \
    "$PP" >"$TMP/block.c"
if grep -q 'pcm_wait_bounded' "$TMP/block.c"; then
    cat "$TMP/block.c" >>"$TMP/parts.c"
elif [ "$OLD" != 1 ]; then
    echo "FAIL: the pcm-wait block not found in $PP"; fail=1
fi
take dma_tx_stop   "$PP"   '^static void dma_tx_stop[(]void[)]$'
take pq_stop       "$PP"   '^static void pq_stop[(]void[)]$'
take sink_stop_pcm "$PP"   '^static void sink_stop_pcm[(]void[)]$'
take pcm_play_data "$PCMC" '^void pcm_play_data[(]pcm_play_callback_type get_more,$'

# the real register bits (pp5020.h) and the fault record (pcm_sink.h)
awk '/^#define (IIS_TX_FREE_MASK|IIS_TX_IS_EMPTY|IIS_TXCLR|DMA_CMD_START|DMA_CMD_INTR|DMA_STATUS_BUSY|DMA_STATUS_INTR)[ (]/ \
         { on = 1 }
     on { print; if (substr($0, length($0)) != "\\") on = 0 }' \
    firmware/export/pp5020.h >"$TMP/pp_defs.h"
{ grep -E '^#define PCM_WAIT_(FIFO|CH0|CH1) ' firmware/export/pcm_sink.h
  awk '/^struct pcm_wait_fault$/ { on = 1 } on { print } on && /^};/ { exit }' \
      firmware/export/pcm_sink.h; } >"$TMP/pcm_wait_fault.h"
[ "$(grep -c '^#define' "$TMP/pp_defs.h")" = 7 ] \
    || { echo "FAIL: pp5020.h's I2S/DMA bits: $(grep -c '^#define' "$TMP/pp_defs.h") of 7"; fail=1; }
grep -q 'unsigned long count;' "$TMP/pcm_wait_fault.h" \
    || { echo "FAIL: struct pcm_wait_fault not found in pcm_sink.h"; fail=1; }
[ $fail -eq 0 ] || exit 1

# build NAME [MUTANT]
build() {
    mkdir -p "$TMP/$1"
    cp "$TMP/pp_defs.h" "$TMP/pcm_wait_fault.h" "$TMP/$1/"
    python3 tests/fencepcm/srcpatch.py "$TMP/parts.c" \
        "$TMP/$1/pcmwait_parts.c" $T/spec_pcmwait.py ${2:-} || return 2
    "$HOSTCC" -std=gnu99 -O1 -g -Wall -Wextra -Werror -Wno-unused-function \
        -Wno-unused-parameter -Wno-cast-function-type ${EXTRA_CFLAGS:-} \
        -I"$TMP/$1" -o "$TMP/$1/t" $T/test_pcmwait.c 2>"$TMP/cc.log" \
        || { cat "$TMP/cc.log"; return 2; }
}

if ! build base; then
    echo "FAIL: compiling test_pcmwait ($HOSTCC)"; exit 1
fi
"$TMP/base/t" >"$TMP/out" 2>&1
cat "$TMP/out"
if [ "$OLD" = 1 ]; then
    # the red proof: an older tree must fail with a verdict line
    if grep -q '^VERDICT: FAIL' "$TMP/out"; then
        echo "red  (as expected for $PP / $PCMC)"; exit 0
    fi
    echo "FAIL: the older source did not go red"; exit 1
fi
grep -q '^VERDICT: PASS$' "$TMP/out" \
    || { echo "FAIL: PCM waits bounded with the codec in standby (#312)"; exit 1; }
echo "ok   PCM waits bounded in DAC standby and with the I2S gated; start wakes the codec (#312)"

[ "${PCMWAIT_MUTANTS:-1}" = 0 ] && exit 0
rc=0; n=0; red=0
for m in $(python3 tests/fencepcm/srcpatch.py --list $T/spec_pcmwait.py); do
    n=$((n + 1))
    if ! build "mut_$m" "$m"; then
        echo "FAIL: mutant $m does not apply or build"; rc=1; continue
    fi
    "$TMP/mut_$m/t" >"$TMP/mout" 2>&1
    v=$(grep '^VERDICT:' "$TMP/mout" | head -1)
    case "$v" in
        "VERDICT: FAIL"*) red=$((red + 1)); echo "     red  $m: ${v#VERDICT: FAIL }" ;;
        "VERDICT: PASS") echo "FAIL: mutant $m survived (the test cannot see it)"; rc=1 ;;
        *) echo "FAIL: mutant $m ended without a verdict line"; tail -3 "$TMP/mout"; rc=1 ;;
    esac
done
echo "     mutants: $red of $n red"
[ "$red" -eq "$n" ] && [ "$n" -gt 0 ] && [ $rc -eq 0 ] \
    && echo "ok   PCM wait mutants ($n of $n red)"
exit $rc
