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

# Self-test of scripts/zeroed_init_check.py (#313), the gate that fails on
# an initialiser placed in a section the boot zeroes (.ncbss: SHAREDBSS,
# NOCACHEBSS, XCORE_BSS; .ibss: IBSS).  ARM objects built here with the
# firmware's compiler and the firmware's attribute spellings:
#  - red: the old wmcodec_gate_mask line (a non-zero value in .ncbss), a
#    pointer initialiser (zero bytes plus a relocation) and an IBSS one;
#  - green: zero initialisers there, non-zero ones in .ncdata/.idata;
#  - no input: exit 2, never a pass.
# Exits 0 with a note when arm-none-eabi-gcc is missing (as tests/flacarm).
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT" || exit 1
CC=arm-none-eabi-gcc
command -v $CC >/dev/null 2>&1 || { echo "note: no $CC; zeroed-section gate self-test skipped"; exit 0; }
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/red" "$TMP/green" "$TMP/empty"
cat >"$TMP/attrs.h" <<'EOF'
#define SHAREDBSS_ATTR  __attribute__((section(".ncbss"),nocommon))
#define SHAREDDATA_ATTR __attribute__((section(".ncdata"),nocommon))
#define IBSS_ATTR       __attribute__((section(".ibss"),nocommon))
#define IDATA_ATTR      __attribute__((section(".idata"),nocommon))
EOF
cat >"$TMP/red/gate.c" <<'EOF'
#include "attrs.h"
unsigned long wmcodec_gate_mask SHAREDBSS_ATTR = 0x00000800 | 0x00000002;
static unsigned long gate_bits SHAREDBSS_ATTR;
unsigned long *gate_ptr SHAREDBSS_ATTR = &gate_bits;
int fiq_count IBSS_ATTR = 3;
EOF
cat >"$TMP/green/gate.c" <<'EOF'
#include "attrs.h"
unsigned long wmcodec_gate_mask SHAREDDATA_ATTR = 0x00000800 | 0x00000002;
static unsigned long gate_bits SHAREDBSS_ATTR = 0;
unsigned long *gate_ptr SHAREDBSS_ATTR = 0;
unsigned long *gate_ptr2 SHAREDDATA_ATTR = &gate_bits;
int fiq_count IDATA_ATTR = 3;
int fiq_zero IBSS_ATTR;
EOF
for d in red green; do
    $CC -c -O2 -I"$TMP" -o "$TMP/$d/gate.o" "$TMP/$d/gate.c" \
        || { echo "FAIL: compiling the $d object"; exit 1; }
done
rc=0
python3 scripts/zeroed_init_check.py "$TMP/red" >"$TMP/red.out" 2>&1
r=$?
n=$(grep -c 'initialiser (' "$TMP/red.out")
if [ $r -eq 1 ] && [ "$n" = 3 ] \
   && grep -q 'wmcodec_gate_mask in .ncbss' "$TMP/red.out" \
   && grep -q 'gate_ptr in .ncbss holds an address' "$TMP/red.out" \
   && grep -q 'fiq_count in .ibss' "$TMP/red.out"; then
    echo "     red  the old gate-mask line, a pointer and an IBSS initialiser (3 found)"
else
    cat "$TMP/red.out"; echo "FAIL: the gate did not catch all 3 (exit $r, $n found)"; rc=1
fi
python3 scripts/zeroed_init_check.py "$TMP/green" >"$TMP/green.out" 2>&1
r=$?
if [ $r -eq 0 ] && grep -q '1 objects, 2 zeroed sections' "$TMP/green.out"; then
    echo "     green zero initialisers in .ncbss/.ibss, values in .ncdata/.idata"
else
    cat "$TMP/green.out"; echo "FAIL: the clean object did not pass (exit $r)"; rc=1
fi
python3 scripts/zeroed_init_check.py "$TMP/empty" >"$TMP/empty.out" 2>&1
r=$?
if [ $r -eq 2 ] && grep -q 'FAILED TO RUN' "$TMP/empty.out"; then
    echo "     no input: exit 2"
else
    cat "$TMP/empty.out"; echo "FAIL: an empty scan did not refuse (exit $r)"; rc=1
fi
[ $rc -eq 0 ] && echo "ok   zeroed-section initialiser gate (#313): red, green and no-input arms"
exit $rc
