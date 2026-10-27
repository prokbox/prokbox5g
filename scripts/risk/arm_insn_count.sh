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

# Risk tool 1: ARM instruction counts / device-time estimate for the
# iPod UI shell's pure, host-testable modules (docs/risk-tooling.md).
#
#   scripts/risk/arm_insn_count.sh
#
# What it does, in order (every build goes under build/risk/, never
# build/fw-ui or build/sim-ui):
#   1. Cross-compiles apps/ipodui/{sortkey,scroll,library}.c (unmodified)
#      with arm-none-eabi-gcc -mcpu=arm7tdmi -Os and reports each hot
#      function's static ARM instruction count (scripts/risk/lib/
#      objdump_count.py) -- a live regression signal for the hand-traced
#      constants in scripts/risk/lib/device_time.py and docs/risk-tooling.md.
#   2. Builds and runs the host-side drivers (scripts/risk/drivers/*.c)
#      that call the SAME real source at realistic library scale
#      (docs/hardware.md Planning scale: 40,000 tracks, 4,000 albums,
#      1,250 artists), measuring average input length, HASH_GROUP rate,
#      real qsort comparator call counts (apps/ipodui/library.c's
#      lib_open), and host ("sim") timing.
#   3. If qemu-system-arm is present, builds a tiny bare-metal ARM
#      harness (build/risk/qemu/) for ui_sortkey()/ui_scroll_step() and
#      measures a REAL dynamic instruction count with a from-scratch TCG
#      plugin (build/risk/qemu/libinsn.c) under ARM semihosting exit.
#      If a Python venv with `unicorn` is present (or can be created --
#      see below), the same harness is also run under Unicorn as an
#      independent cross-check. Either or both may be skipped; the
#      report says which, and falls back to the hand-traced model in
#      scripts/risk/lib/device_time.py (documented, dated, not hidden).
#   4. scripts/risk/lib/device_time.py combines all of this into a
#      device-time table and the "corrected factor" vs the project's
#      existing uncalibrated x400 (docs/process.md).
#
# Exit status is always 0 (this is a measurement/report tool, not a
# pass/fail gate); missing optional tools are reported, not fatal.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
B=build/risk
mkdir -p "$B/arm" "$B/host" "$B/qemu" "$B/out"
FW="-Ifirmware -Ifirmware/export -Ifirmware/drivers -Ifirmware/include \
-Ifirmware/kernel/include -Ifirmware/libc/include"
AI="-Iapps -Iapps/ipodui"
LIBSTUB="-Itests/ipodui/libstub -Itests/ipodui"
ARMCC="arm-none-eabi-gcc"
note() { printf '\nNOTE: %s\n' "$*"; }
step() { printf '\n== %s\n' "$*"; }

step "toolchain"
if ! command -v "$ARMCC" >/dev/null 2>&1; then
    echo "FAIL: $ARMCC not found."
    echo "  brew install arm-none-eabi-gcc"
    exit 0
fi
"$ARMCC" --version | head -1

step "1. static ARM instruction counts (regression signal)"
# shellcheck disable=SC2086
"$ARMCC" -mcpu=arm7tdmi -Os -ffreestanding -fno-builtin $FW $AI \
    -c apps/ipodui/sortkey.c -o "$B/arm/sortkey.o" \
    && python3 scripts/risk/lib/objdump_count.py "$B/arm/sortkey.o" \
    || echo "FAIL: cross-compiling apps/ipodui/sortkey.c for ARM"
# shellcheck disable=SC2086
"$ARMCC" -mcpu=arm7tdmi -Os -ffreestanding -fno-builtin $AI \
    -c apps/ipodui/scroll.c -o "$B/arm/scroll.o" \
    && python3 scripts/risk/lib/objdump_count.py "$B/arm/scroll.o" \
    || echo "FAIL: cross-compiling apps/ipodui/scroll.c for ARM"
mkdir -p "$B/arm-shim"
cat > "$B/arm-shim/strings.h" <<'EOF'
#ifndef RISK_SHIM_STRINGS_H
#define RISK_SHIM_STRINGS_H
#include <string.h>
int strcasecmp(const char *s1, const char *s2);
int strncasecmp(const char *s1, const char *s2, size_t n);
size_t strlcpy(char *dst, const char *src, size_t size);
size_t strlcat(char *dst, const char *src, size_t size);
#endif
EOF
# shellcheck disable=SC2086
"$ARMCC" -mcpu=arm7tdmi -Os -ffreestanding -fno-builtin \
    $LIBSTUB $AI -I"$B/arm-shim" -Ifirmware/include -Ifirmware/libc/include \
    -c apps/ipodui/library.c -o "$B/arm/library.o" \
    && python3 scripts/risk/lib/objdump_count.py "$B/arm/library.o" \
        cmp_rec cmp_labels cmp_track_no key_prefix \
    || echo "FAIL: cross-compiling apps/ipodui/library.c for ARM (libstub)"

step "2. host drivers (realistic library scale)"
: > "$B/out/driver.log"
# shellcheck disable=SC2086
cc -O1 -std=gnu99 -Wall -Wextra $AI scripts/risk/drivers/sortkey_drv.c \
    apps/ipodui/sortkey.c -o "$B/host/sortkey_drv" \
    && for role in title:40000 album:4000 artist:1250; do
        n=${role#*:}; r=${role%:*}
        "$B/host/sortkey_drv" "$n" "$r" >> "$B/out/driver.log"
    done \
    || echo "FAIL: building/running sortkey_drv"
# shellcheck disable=SC2086
cc -O1 -std=gnu99 -Wall -Wextra $AI scripts/risk/drivers/scroll_drv.c \
    apps/ipodui/scroll.c -o "$B/host/scroll_drv" \
    && "$B/host/scroll_drv" 10000 >> "$B/out/driver.log" \
    || echo "FAIL: building/running scroll_drv"
# shellcheck disable=SC2086
cc -O1 -std=gnu99 -Wall -Wextra $LIBSTUB $AI \
    scripts/risk/drivers/library_sort_drv.c apps/ipodui/library.c \
    apps/ipodui/sortkey.c tests/ipodui/fake_tagcache.c \
    -o "$B/host/library_sort_drv" \
    && "$B/host/library_sort_drv" 40000 4000 >> "$B/out/driver.log" \
    || echo "FAIL: building/running library_sort_drv"
cat "$B/out/driver.log"

step "3. dynamic ARM instruction counts (qemu-system-arm / Unicorn)"
RISK_SORTKEY_INSNS_PER_CALL=
RISK_SCROLL_INSNS_PER_GLIDE=
H=scripts/risk/qemu-harness

# the bare-metal ELF harnesses (apps/ipodui/{sortkey,scroll}.c,
# unmodified, linked against H/start.S + H/libc_shim.c -- see
# docs/risk-tooling.md "dynamic instruction counts" for why the real
# firmware/asm/arm/{memcpy,memmove}.S aren't used here)
# shellcheck disable=SC2086
"$ARMCC" -mcpu=arm7tdmi -c "$H/start.S" -o "$B/qemu/start.o" \
    && "$ARMCC" -mcpu=arm7tdmi -Os -ffreestanding -fno-builtin $AI \
        -c "$H/sortkey_main.c" -o "$B/qemu/sortkey_main.o" \
    && "$ARMCC" -mcpu=arm7tdmi -Os -ffreestanding -fno-builtin $AI \
        -c "$H/scroll_main.c" -o "$B/qemu/scroll_main.o" \
    && "$ARMCC" -mcpu=arm7tdmi -Os -ffreestanding -fno-builtin $FW $AI \
        -c apps/ipodui/sortkey.c -o "$B/qemu/sortkey.o" \
    && "$ARMCC" -mcpu=arm7tdmi -Os -ffreestanding -fno-builtin $AI \
        -c apps/ipodui/scroll.c -o "$B/qemu/scroll.o" \
    && "$ARMCC" -mcpu=arm7tdmi -Os -ffreestanding -fno-builtin \
        -c "$H/libc_shim.c" -o "$B/qemu/libc_shim.o" \
    && "$ARMCC" -mcpu=arm7tdmi -nostdlib -ffreestanding \
        -Wl,-T,"$H/link.ld" "$B/qemu/start.o" "$B/qemu/sortkey_main.o" \
        "$B/qemu/sortkey.o" "$B/qemu/libc_shim.o" -lgcc \
        -o "$B/qemu/sortkey.elf" 2>/dev/null \
    && "$ARMCC" -mcpu=arm7tdmi -nostdlib -ffreestanding \
        -Wl,-T,"$H/link.ld" "$B/qemu/start.o" "$B/qemu/scroll_main.o" \
        "$B/qemu/scroll.o" -lgcc -o "$B/qemu/scroll.elf" 2>/dev/null \
    && arm-none-eabi-objcopy -O binary "$B/qemu/sortkey.elf" "$B/qemu/sortkey.bin" \
    && arm-none-eabi-objcopy -O binary "$B/qemu/scroll.elf" "$B/qemu/scroll.bin" \
    || echo "FAIL: building the qemu/Unicorn bare-metal harness"

HAVE_QEMU=0
command -v qemu-system-arm >/dev/null 2>&1 && HAVE_QEMU=1
if [ $HAVE_QEMU = 0 ]; then
    note "qemu-system-arm not found: brew install qemu
  (ships qemu-system-arm on macOS; no arm-linux user-mode qemu-arm and
  no prebuilt TCG plugin -- this tool builds its own, see below)."
fi

QEMU_OK=0
if [ $HAVE_QEMU = 1 ] && [ -f "$B/qemu/sortkey.elf" ]; then
    QEMU_VER=$(qemu-system-arm --version | sed -n 's/.*version \([0-9.]*\).*/\1/p;q')
    PLUGIN_H=""
    for p in /opt/homebrew/Cellar/qemu/"$QEMU_VER"/include/qemu-plugin.h \
             /usr/local/Cellar/qemu/"$QEMU_VER"/include/qemu-plugin.h; do
        [ -f "$p" ] && PLUGIN_H=$p && break
    done
    if [ -z "$PLUGIN_H" ]; then
        note "qemu-plugin.h not found for qemu $QEMU_VER (no Homebrew
  qemu-plugin.h at the expected Cellar path) -- cannot build the TCG
  instruction-count plugin. Falling back to Unicorn / the hand-traced
  model."
    elif [ ! -f "$B/qemu/libinsn.dylib" ]; then
        GLIB_CFLAGS=$(pkg-config --cflags glib-2.0 2>/dev/null || true)
        # shellcheck disable=SC2086
        cc -shared -fPIC -O2 -undefined dynamic_lookup \
            -I"$(dirname "$PLUGIN_H")" $GLIB_CFLAGS \
            "$H/libinsn.c" -o "$B/qemu/libinsn.dylib" 2>"$B/qemu/plugin.log" \
            || note "could not build the TCG plugin (see $B/qemu/plugin.log)."
    fi
    if [ -f "$B/qemu/libinsn.dylib" ]; then
        SK_LOG=$(timeout 20 qemu-system-arm -M versatilepb -cpu arm926 \
            -nographic -semihosting -plugin "./$B/qemu/libinsn.dylib" \
            -kernel "$B/qemu/sortkey.elf" 2>&1)
        SC_LOG=$(timeout 20 qemu-system-arm -M versatilepb -cpu arm926 \
            -nographic -semihosting -plugin "./$B/qemu/libinsn.dylib" \
            -kernel "$B/qemu/scroll.elf" 2>&1)
        SK_N=$(echo "$SK_LOG" | sed -n 's/INSN_COUNT //p')
        SC_N=$(echo "$SC_LOG" | sed -n 's/INSN_COUNT //p')
        if [ -n "$SK_N" ] && [ -n "$SC_N" ]; then
            echo "qemu-system-arm + TCG plugin: sortkey=$SK_N insns/2000 calls, scroll=$SC_N insns/500 glides"
            RISK_SORTKEY_INSNS_PER_CALL=$(echo "$SK_N" | awk '{print $1/2000}')
            RISK_SCROLL_INSNS_PER_GLIDE=$(echo "$SC_N" | awk '{print $1/500}')
            QEMU_OK=1
        else
            note "qemu-system-arm ran but did not print INSN_COUNT; see:
$SK_LOG
$SC_LOG"
        fi
    fi
fi

if [ ! -x out/risk-venv/bin/python3 ]; then
    note "out/risk-venv (Unicorn) not set up; creating it (pip install
  unicorn, local to the repo's gitignored out/ -- not system-wide):"
    mkdir -p out
    python3 -m venv out/risk-venv 2>/dev/null \
        && out/risk-venv/bin/pip install --quiet unicorn 2>/dev/null \
        || note "could not create out/risk-venv / install unicorn (offline?)."
fi
if [ -x out/risk-venv/bin/python3 ] \
        && [ -f "$B/qemu/sortkey.bin" ] && [ -f "$B/qemu/scroll.bin" ]; then
    UC_SK=$(out/risk-venv/bin/python3 scripts/risk/lib/unicorn_count.py \
        "$B/qemu/sortkey.bin" 2>&1 | sed -n 's/UNICORN_INSN_COUNT //p')
    UC_SC=$(out/risk-venv/bin/python3 scripts/risk/lib/unicorn_count.py \
        "$B/qemu/scroll.bin" 2>&1 | sed -n 's/UNICORN_INSN_COUNT //p')
    if [ -n "$UC_SK" ] && [ -n "$UC_SC" ]; then
        echo "Unicorn (independent cross-check): sortkey=$UC_SK insns/2000 calls, scroll=$UC_SC insns/500 glides"
        if [ $QEMU_OK = 0 ]; then
            RISK_SORTKEY_INSNS_PER_CALL=$(echo "$UC_SK" | awk '{print $1/2000}')
            RISK_SCROLL_INSNS_PER_GLIDE=$(echo "$UC_SC" | awk '{print $1/500}')
        fi
    fi
else
    note "out/risk-venv (Unicorn) not set up: python3 -m venv out/risk-venv &&
  out/risk-venv/bin/pip install unicorn"
fi

step "4. device-time report"
export RISK_SORTKEY_INSNS_PER_CALL RISK_SCROLL_INSNS_PER_GLIDE
python3 scripts/risk/lib/device_time.py "$B/out/driver.log"
exit 0
