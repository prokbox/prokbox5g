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

# Build the public release zips (docs/release-plan.md section 3.3).
#
# usage: VERSION=0.8.0 scripts/release_build.sh RAM [OUTDIR]
#   RAM     64 or 32
#   OUTDIR  where the zips go (default out/)
#
# Builds ProkBox5g-$VERSION-{stock,modded}-{flash,hdd}-${RAM}MB.zip in
# build/rel-<variant>-<storage>/. Run it inside the release toolchain
# container (ci/toolchain/Dockerfile): .github/workflows/release.yml does,
# and so does the local side of scripts/release_compare.py, so both sides
# build with the same compiler and host tools.
#
# Shape, variants and checks: scripts/release_flags.sh. Public variants
# (owner, 2026-10-26):
#   stock   stock battery, 30-pin dock, no haptics
#   modded  the owner's mods: 3000 mAh, haptics, no dock (USB-C)
# each for flash and hard-drive storage (runtime detection, #79, is a
# later roadmap item).
set -eu

RAM=${1:?usage: VERSION=x.y.z $0 RAM [OUTDIR]}
OUT=${2:-out}
: "${VERSION:?set VERSION (the tag without its v)}"
export VERSION
case "$RAM" in 64|32) ;; *) echo "RAM must be 64 or 32" >&2; exit 2 ;; esac

. "$(dirname "$0")/release_flags.sh"
COMMON="$RELEASE_SHAPE --compiler-prefix=arm-none-eabi-"
STOCK=$RELEASE_STOCK
MODDED=$RELEASE_MODDED
JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)

echo "toolchain: $(arm-none-eabi-gcc --version | head -1)"
mkdir -p "$OUT"

# The ProkBox start-up program (bootloader), the same for every variant; the
# guide's install step adds it with ipodpatcher -a from the zip's top level.
boot="build/rel-boot-$RAM"
rm -rf "$boot" && mkdir -p "$boot"
(cd "$boot" && ../../tools/configure $RELEASE_BOOT --compiler-prefix=arm-none-eabi- \
    </dev/null >configure.log)
make -C "$boot" -j"$JOBS" >"$boot/make.log" 2>&1 || { tail -30 "$boot/make.log"; exit 1; }
[ -f "$boot/bootloader-ipodvideo.ipod" ] || { echo "no bootloader-ipodvideo.ipod" >&2; exit 1; }
for variant in stock modded; do
    if [ $variant = stock ]; then vargs=$STOCK; else vargs=$MODDED; fi
    for storage in flash hdd; do
        dir="build/rel-$variant-$storage-$RAM"
        rm -rf "$dir" && mkdir -p "$dir"
        (cd "$dir" && ../../tools/configure $COMMON $vargs \
            --storage=$storage --ram=$RAM </dev/null >configure.log)
        grep IPOD_VARIANT "$dir/autoconf.h"
        make -C "$dir" -j"$JOBS" >"$dir/make.log" 2>&1 \
            || { tail -30 "$dir/make.log"; exit 1; }
        make -C "$dir" zip >"$dir/zip.log" 2>&1 \
            || { tail -30 "$dir/zip.log"; exit 1; }
        # Developer mode is compiled out of a release (scripts/validate.sh, fw-rel).
        if strings "$dir/rockbox.ipod" | grep -qi developer; then
            echo "$variant-$storage-$RAM: release image contains developer strings" >&2
            exit 1
        fi
        zipout="$OUT/ProkBox5g-$VERSION-$variant-$storage-${RAM}MB.zip"
        cp "$dir/rockbox.zip" "$zipout"
        zip -q -X -j "$zipout" "$boot/bootloader-ipodvideo.ipod"
        # The shipping shape (scripts/release_flags.sh): every required file
        # in the zip, and the resident image under the cap.
        list=$(unzip -Z1 "$zipout")
        for f in $RELEASE_REQUIRED; do
            if ! printf '%s\n' "$list" | grep -qx "$f"; then
                echo "$variant-$storage-$RAM: release zip lacks $f" >&2
                exit 1
            fi
        done
        # No empty language file (a header-only .lng is 10 B: no on-screen text)
        if unzip -Z -l "$zipout" '.rockbox/langs/*.lng' | awk '$NF ~ /\.lng$/ && $4+0 < 1000 {bad=1; print "  " $NF " " $4 " B"} END {exit !bad}' >&2; then
            echo "$variant-$storage-$RAM: an empty language file in the zip" >&2
            exit 1
        fi
        end=$(arm-none-eabi-nm "$dir/rockbox.elf" | awk '$3=="_end" {print $1}')
        end=$(printf '%d' "0x$end")
        if [ "$end" -gt "$RELEASE_END_MAX" ]; then
            echo "$variant-$storage-$RAM: _end $end B over the $RELEASE_END_MAX B resident cap" >&2
            exit 1
        fi
        echo "$variant-$storage-$RAM: _end $end B (cap $RELEASE_END_MAX)"
    done
done
ls -l "$OUT"/ProkBox5g-"$VERSION"-*-"${RAM}"MB.zip
