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

# Host test of #324: no BCM2722 host-port access while the chip is
# unpowered (GPO32 bit 14 clear), and the port lock's waits bounded.
# Extracts the REAL accessors (no copy kept here) from lcd-video.c,
# bcm-devtest-hal.c and e2-port.c, runs them against the register model of
# test_bcmgate.c (the port is a page that faults on any load or store while
# unpowered), in two shapes: the release's (HAVE_BCMFW_L1: bounded waits)
# and the plain shell firmware's (stock waits), then each mutant in
# spec_bcmgate.py, which must turn it red.  BCMGATE_SRC=DIR reads the
# three files from another tree; BCMGATE_OLD=1 expects that tree red (the
# red proof against the tree before the gate).  Compiler: the newest
# Homebrew gcc-NN, else cc; HOSTCC overrides.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
T=tests/bcmgate
SRC=${BCMGATE_SRC:-firmware/target/arm/ipod/video}
LV=$SRC/lcd-video.c
HAL=$SRC/bcm-devtest-hal.c
E2=$SRC/e2-port.c
OLD=${BCMGATE_OLD:-0}
if [ -z "${HOSTCC:-}" ]; then
    HOSTCC=cc
    for v in 19 18 17 16 15 14; do
        if command -v gcc-$v >/dev/null 2>&1; then HOSTCC=gcc-$v; break; fi
    done
fi
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
fail=0
nparts=0

# put LINE...: literal lines into the parts
put() { printf '%s\n' "$@" >>"$TMP/parts.c"; }

# got NAME: the piece just extracted is not empty; appended to the parts
got() {
    if [ ! -s "$TMP/part.c" ]; then
        echo "FAIL: $1: the extraction produced nothing"; fail=1; return 1
    fi
    cat "$TMP/part.c" >>"$TMP/parts.c"
}

# take NAME FILE START_RE [NTH]: START_RE's line through the next "}" at
# column 0; START_RE must match exactly once (NTH: take the NTH of several)
take() {
    n=$(grep -c -E "$3" "$2")
    if [ -z "${4:-}" ]; then ok=$([ "$n" = 1 ] && echo 1); else ok=$([ "$n" -ge "$4" ] && echo 1); fi
    if [ -z "$ok" ]; then
        if [ "$OLD" = 1 ]; then put "/* $1: absent */"; return; fi
        echo "FAIL: $1: $n lines match in $2 (the extraction must see the real code)"
        fail=1; return
    fi
    RE="$3" NTH="${4:-1}" awk \
        '!on && $0 ~ ENVIRON["RE"] { k++; if (k == ENVIRON["NTH"] + 0) on = 1 }
         on { print } on && /^}/ { exit }' "$2" >"$TMP/part.c"
    got "$1" || return
    echo >>"$TMP/parts.c"
    nparts=$((nparts + 1))
}

# range NAME FILE START_RE END_RE: START_RE's line through the "}" that
# closes the function END_RE starts
range() {
    if [ "$(grep -c -E "$3" "$2")" != 1 ] || [ "$(grep -c -E "$4" "$2")" != 1 ]; then
        if [ "$OLD" = 1 ]; then put "/* $1: absent */"; return; fi
        echo "FAIL: $1: the range is not in $2"; fail=1; return
    fi
    A="$3" B="$4" awk \
        '!on && $0 ~ ENVIRON["A"] { on = 1 } on { print }
         on && $0 ~ ENVIRON["B"] { e = 1 } e && /^}/ { exit }' "$2" >"$TMP/part.c"
    got "$1" || return
    echo >>"$TMP/parts.c"
    nparts=$((nparts + 1))
}

# macro NAME FILE RE: one #define line (and its continuation lines)
macro() {
    if [ "$(grep -c -E "$3" "$2")" != 1 ]; then
        if [ "$OLD" = 1 ]; then return; fi
        echo "FAIL: $1: not one line in $2"; fail=1; return
    fi
    RE="$3" awk '!on && $0 ~ ENVIRON["RE"] { on = 1 }
        on { print; if (substr($0, length($0)) != "\\") exit }' "$2" >"$TMP/part.c"
    got "$1"
}

: >"$TMP/parts.c"
# lcd-video.c: the gate's record and macros, the primitives, the tick,
# the update paths, the bounded lock
macro BCM_PORT_OK  "$LV"  '^#define BCM_PORT_OK[(]fn[)] bcm_port_on[(]fn[)]$'
macro BCM_DOORBELL "$LV"  '^#define BCM_DOORBELL[(]fn[)] \\$'
macro E2_PORT_OK   "$E2"  '^#define E2_PORT_OK[(]fn[)] bcm_port_on[(]fn[)]$'
if [ "$OLD" = 1 ]; then    # the tree before the gate: its plain forms
    put '#ifndef BCM_PORT_OK' '#define BCM_PORT_OK(fn) true' '#endif'
    put '#ifndef BCM_DOORBELL' '#define BCM_DOORBELL(fn) (BCM_CONTROL = 0x31)' '#endif'
    put '#ifndef E2_PORT_OK' '#define E2_PORT_OK(fn) true' '#endif'
fi
range bpg            "$LV" '^static volatile uint32_t bpg_n XCORE_BSS;' \
                           '^static inline void bcm_port_powered_up[(]void[)]$'
take bcm_port_wait   "$LV" '^static bool bcm_port_wait[(]volatile unsigned short [*]reg, unsigned mask, bool set[)]$'
take bcm_write_addr  "$LV" '^static inline void bcm_write_addr[(]unsigned address[)]$'
take bcm_write32     "$LV" '^static inline void bcm_write32[(]unsigned address, unsigned value[)]$'
take bcm_read32      "$LV" '^static inline unsigned bcm_read32[(]unsigned address[)]$'
take continue_awake  "$LV" '^static void continue_lcd_awake[(]void[)]$'
take lcd_tick        "$LV" '^static void lcd_tick[(]void[)]$'
take lcd_block_tick  "$LV" '^static inline void lcd_block_tick[(]void[)]$'
take lcd_unblock     "$LV" '^static void lcd_unblock_and_update[(]void[)]$' 1
take lock_tmo        "$LV" '^static int lcd_port_lock_tmo[(]long t[)]$'
take lock_bounded    "$LV" '^static int lcd_port_lock_bounded[(]int k[)]$'
put '#ifdef HAVE_FE_FB_HANDOFF'
take lcd_update_rect "$LV" '^static void lcd_update_rect_x[(]int x, int y, int width, int height,$'
take lcd_blit_yuv    "$LV" '^void lcd_blit_yuv[(]unsigned char [*] const src[[]3[]],$'
take bcm_command     "$LV" '^static void bcm_command[(]unsigned cmd[)]$'
# bcm-devtest-hal.c: the device test's and the player's port layer
take dt_until        "$HAL" '^static bool dt_until[(]volatile unsigned short [*]reg, unsigned mask, bool set[)]$'
take dt_wait         "$HAL" '^static bool dt_wait[(]volatile unsigned short [*]reg, unsigned mask,$'
take dt_rd           "$HAL" '^static int dt_rd[(]uint32_t addr, uint32_t [*]val[)]$'
take dt_wr           "$HAL" '^static int dt_wr[(]uint32_t addr, uint32_t val[)]$'
take bpb_addr        "$HAL" '^static inline int bpb_addr[(]uint32_t a[)]$'
take bpb_words       "$HAL" '^static inline void bpb_words[(]const uint16_t [*]s, uint32_t n[)]$'
take bpb_word        "$HAL" '^static inline void bpb_word[(]uint32_t v[)]$'
take bpb_rd          "$HAL" '^static inline int bpb_rd[(]uint32_t a, uint32_t [*]v[)]$'
take bpb_wr          "$HAL" '^static inline int bpb_wr[(]uint32_t a, uint32_t v[)]$'
put '#include "bcm-port-block.h"'
take dt_wr_block     "$HAL" '^static int dt_wr_block[(]uint32_t addr, const uint16_t [*]src, uint32_t n[)]$'
take dt_ppdma        "$HAL" '^static int dt_ppdma[(]const void [*]src, uint32_t bytes[)]$'
take dt_fb_upload    "$HAL" '^static int dt_fb_upload[(]uint32_t addr, uint32_t off, uint32_t bytes, int how[)]$'
take dt_upload_fb    "$HAL" '^static int dt_upload_fb[(]void[)]$'
take dt_doorbell     "$HAL" '^static void dt_doorbell[(]void[)]$'
take dt_pa_addr      "$HAL" '^static int dt_pa_addr[(]uint32_t addr, unsigned how[)]$'
take dt_pa_cpu       "$HAL" '^static int dt_pa_cpu[(]uint32_t addr, const uint16_t [*]src, uint32_t n,$'
take port_raw        "$HAL" '^void bcm_video_port_raw[(]uint16_t out[[]10[]][)]$'
take alt_until       "$HAL" '^static bool alt_until[(]volatile unsigned short [*]reg, unsigned mask[)]$'
take alt_count       "$HAL" '^static uint32_t alt_count[(]uint32_t v, uint32_t want_lo, uint32_t want_hi[)]$'
take alt_lab         "$HAL" '^int bcm_video_alt_lab[(]struct bcm_alt_result [*]r[)]$'
take video_rd        "$HAL" '^int bcm_video_rd[(]uint32_t addr, uint32_t [*]val[)]$'
take video_wr        "$HAL" '^int bcm_video_wr[(]uint32_t addr, uint32_t val[)]$'
take video_wr_block  "$HAL" '^int bcm_video_wr_block[(]uint32_t addr, const uint16_t [*]src, uint32_t n[)]$'
take vwr_block       "$HAL" '^static int vwr_block[(]uint32_t addr, const uint16_t [*]src, uint32_t n[)]$'
take wr_variant      "$HAL" '^int bcm_video_wr_variant[(]uint32_t addr, const uint16_t [*]src, uint32_t n,$'
# e2-port.c: engine 2 into the port
take e2port_claim    "$E2" '^bool e2port_claim[(]enum e2port_owner who[)]$'
take e2port_release  "$E2" '^void e2port_release[(]enum e2port_owner who[)]$'
take e2port_fits     "$E2" '^bool e2port_fits[(]const void [*]src, uint32_t bytes[)]$'
take port_ready      "$E2" '^static bool port_ready[(]void[)]$'
take e2_port_ready   "$E2" '^bool e2port_port_ready[(]void[)]$'
take e2port_start    "$E2" '^int e2port_start[(]uint32_t bcm_addr, const void [*]src, uint32_t bytes[)]$'
take e2port_busy     "$E2" '^bool e2port_busy[(]void[)]$'
take e2port_stop     "$E2" '^void e2port_stop[(]void[)]$'
# the bound the lock cases expect (the test defines the same literal)
[ "$OLD" = 1 ] || grep -q '^#define LCD_PORT_LOCK_T (2 [*] HZ) ' "$LV" \
    || { echo "FAIL: LCD_PORT_LOCK_T is not (2 * HZ) in $LV"; fail=1; }
[ $fail -eq 0 ] || exit 1
[ "$OLD" = 1 ] || [ $nparts -eq 47 ] \
    || { echo "FAIL: $nparts of 47 parts extracted"; exit 1; }

# build NAME SHAPE [MUTANT]: SHAPE l1 (the release's bounded waits) or
# plain (the shell firmware without our image: stock waits)
build() {
    mkdir -p "$TMP/$1"
    cp "$SRC/bcm-port-block.h" "$TMP/$1/" 2>/dev/null \
        || cp firmware/target/arm/ipod/video/bcm-port-block.h "$TMP/$1/"
    cp firmware/target/arm/ipod/video/bcm-port-gate.h "$TMP/$1/"
    python3 tests/fencepcm/srcpatch.py "$TMP/parts.c" \
        "$TMP/$1/bcmgate_parts.c" $T/spec_bcmgate.py ${3:-} || return 2
    d=""
    [ "$2" = l1 ] && d="-DHAVE_BCMFW_L1"
    [ "$OLD" = 1 ] && d="$d -DBCMGATE_OLD"
    "$HOSTCC" -std=gnu99 -O1 -g -Wall -Wextra -Werror -Wno-unused-function \
        -Wno-unused-parameter -Wno-unused-variable -Wno-unused-but-set-variable \
        $d ${EXTRA_CFLAGS:-} -I"$TMP/$1" -o "$TMP/$1/t" $T/test_bcmgate.c \
        2>"$TMP/cc.log" || { cat "$TMP/cc.log"; return 2; }
}

for shape in l1 plain; do
    if ! build "base_$shape" $shape; then
        echo "FAIL: compiling test_bcmgate ($shape, $HOSTCC)"; exit 1
    fi
    "$TMP/base_$shape/t" >"$TMP/out_$shape" 2>&1
    sed "s/^/[$shape] /" "$TMP/out_$shape"
    if [ "$OLD" = 1 ]; then
        grep -q '^VERDICT: FAIL' "$TMP/out_$shape" \
            || { echo "FAIL: the older source did not go red ($shape)"; exit 1; }
        continue
    fi
    grep -q '^VERDICT: PASS$' "$TMP/out_$shape" \
        || { echo "FAIL: the BCM host port gated on its power ($shape; #324)"; exit 1; }
done
if [ "$OLD" = 1 ]; then echo "red  (as expected for $SRC)"; exit 0; fi
echo "ok   no host-port access with the BCM unpowered; port-lock waits bounded (#324)"

[ "${BCMGATE_MUTANTS:-1}" = 0 ] && exit 0
rc=0; n=0; red=0
for m in $(python3 tests/fencepcm/srcpatch.py --list $T/spec_bcmgate.py); do
    n=$((n + 1))
    if ! build "mut_$m" l1 "$m"; then
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
    && echo "ok   port gate mutants ($n of $n red)"
exit $rc
