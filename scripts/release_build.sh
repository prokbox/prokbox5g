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

# Build the public release zip (docs/release-plan.md section 3.3).
#
# usage: scripts/release_build.sh [OUTDIR]
#   OUTDIR  where the zip goes (default out/)
# The version comes from git (scripts/version.sh: the tag at HEAD, or a
# dev string off a tag). VERSION=x.y.z may override it (release_tree.py
# does, for its export check), but at a release tag it must equal the tag,
# and RELEASE_BUILD=1 (CI) refuses to build anywhere but a clean release tag.
#
# Builds ProkBox5g-$VERSION.zip in build/rel-fw/ (the bootloader in
# build/rel-boot/). Run it inside the release toolchain container
# (ci/toolchain/Dockerfile): .github/workflows/release.yml does, and so
# does the local side of scripts/release_compare.py, so both sides build
# with the same compiler and host tools.
#
# Shape and checks: scripts/release_flags.sh. One image for every unit
# since 0.8.3 (owner, 2026-11-11): RAM and storage are read at start-up,
# the battery is asked once at first start, iAP is resident (#204, #79,
# #205, #236). Until 0.8.2 this built {stock,modded} x {flash,hdd} for a
# RAM argument; an old caller's "64" is refused, not taken as OUTDIR.
set -eu

case "${1:-}" in
    32|64) echo "release_build.sh takes no RAM argument since 0.8.3 (one image for 32 and 64 MB units)" >&2; exit 2 ;;
esac
OUT=${1:-out}
HERE=$(cd "$(dirname "$0")" && pwd)
SRCV=$("$HERE/version.sh" "$HERE/..") || {
    [ -n "${VERSION:-}" ] || { echo "release_build: cannot compute the version" >&2; exit 2; }
    SRCV=$VERSION; }
TAGV=$("$HERE/version.sh" --tag-only "$HERE/.." 2>/dev/null) || TAGV=
if [ -n "$TAGV" ] && [ -n "${VERSION:-}" ] && [ "$VERSION" != "$TAGV" ]; then
    echo "release_build: REFUSED: VERSION=$VERSION disagrees with the tag at HEAD (v$TAGV)" >&2; exit 2
fi
if [ "${RELEASE_BUILD:-0}" = 1 ] && [ -z "$TAGV" ]; then
    echo "release_build: REFUSED: RELEASE_BUILD=1 but HEAD is not at a clean release tag ($SRCV)" >&2; exit 2
fi
VERSION=${VERSION:-$SRCV}
# Only the shown version carries it. VERSION is the Rockbox build-id override
# (tools/version.sh, rockbox-info "Build:"): never leave it set for make.
PROKBOX_VERSION=$VERSION
export PROKBOX_VERSION
unset VERSION
. "$(dirname "$0")/release_flags.sh"
COMMON="$RELEASE_SHAPE --compiler-prefix=arm-none-eabi-"
JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)

echo "toolchain: $(arm-none-eabi-gcc --version | head -1)"
mkdir -p "$OUT"

# The ProkBox start-up program (bootloader); the guide's install step adds
# it with ipodpatcher -a from the zip's top level.
boot="build/rel-boot"
rm -rf "$boot" && mkdir -p "$boot"
(cd "$boot" && ../../tools/configure $RELEASE_BOOT --compiler-prefix=arm-none-eabi- \
    </dev/null >configure.log)
make -C "$boot" -j"$JOBS" >"$boot/make.log" 2>&1 || { tail -30 "$boot/make.log"; exit 1; }
[ -f "$boot/bootloader-ipodvideo.ipod" ] || { echo "no bootloader-ipodvideo.ipod" >&2; exit 1; }

dir="build/rel-fw"
rm -rf "$dir" && mkdir -p "$dir"
(cd "$dir" && ../../tools/configure $COMMON </dev/null >configure.log)
make -C "$dir" -j"$JOBS" >"$dir/make.log" 2>&1 \
    || { tail -30 "$dir/make.log"; exit 1; }
make -C "$dir" zip >"$dir/zip.log" 2>&1 \
    || { tail -30 "$dir/zip.log"; exit 1; }
# Developer mode is compiled out of a release (scripts/validate.sh, fw-rel).
if strings "$dir/rockbox.ipod" | grep -qi developer; then
    echo "release image contains developer strings" >&2
    exit 1
fi
zipout="$OUT/ProkBox5g-$PROKBOX_VERSION.zip"
cp "$dir/rockbox.zip" "$zipout"
zip -q -X -j "$zipout" "$boot/bootloader-ipodvideo.ipod"
# The shipping shape (scripts/release_flags.sh): every required file in the
# zip, and the resident image under the cap.
list=$(unzip -Z1 "$zipout")
for f in $RELEASE_REQUIRED; do
    if ! printf '%s\n' "$list" | grep -qx "$f"; then
        echo "release zip lacks $f" >&2
        exit 1
    fi
done
# No empty language file (a header-only .lng is 10 B: no on-screen text)
if unzip -Z -l "$zipout" '.rockbox/langs/*.lng' | awk '$NF ~ /\.lng$/ && $4+0 < 1000 {bad=1; print "  " $NF " " $4 " B"} END {exit !bad}' >&2; then
    echo "an empty language file in the zip" >&2
    exit 1
fi
end=$(arm-none-eabi-nm "$dir/rockbox.elf" | awk '$3=="_end" {print $1}')
end=$(printf '%d' "0x$end")
if [ "$end" -gt "$RELEASE_END_MAX" ]; then
    echo "_end $end B over the $RELEASE_END_MAX B resident cap" >&2
    exit 1
fi
echo "ProkBox5g-$PROKBOX_VERSION: _end $end B (cap $RELEASE_END_MAX)"
ls -l "$zipout"
