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

# Art builder source reads, before and after the bulk read (art-bulk-1019):
# builds tests/artread/artread.c against the core JPEG decoder twice (BASE's
# apps/recorder and the working tree's), with the shell simulator's flags,
# decodes a large and a typical cover (plain, inside a FLAC and inside an
# M4B, at the container's picture offset) and prints the read() calls, the
# bytes and the modelled storage commands per decode. Fails if the decoded
# pixels differ between the two builds.
#   SIMDIR  a configured shell simulator build (default build/sim-ui)
#   BASE    the "before" commit (default 91fead48e1)
# Needs gcc-16 (or HOSTCC), python3 with PIL, and ffmpeg.
set -eu
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
SIMDIR=${SIMDIR:-$ROOT/build/sim-ui}
BASE=${BASE:-91fead48e1}
HOSTCC=${HOSTCC:-gcc-16}
T=$ROOT/tests/artread
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

[ -f "$SIMDIR/autoconf.h" ] || { echo "SKIP: no simulator build at $SIMDIR"; exit 0; }
SIMROOT=$(sed -n 's/^export ROOTDIR=//p' "$SIMDIR/Makefile")
FL=$(cd "$SIMDIR" && make -n -B V=1 "$SIMDIR/apps/recorder/jpeg_load.o" 2>/dev/null \
     | grep -- '-c .*jpeg_load.c' | head -1 \
     | sed -E -e 's#^.*(gcc|cc)(-[0-9]+)? ##' -e 's# -MMD -MP -c .*##' \
              -e 's#-DTARGET_NAME=[^ ]*##' \
              -e "s#-I$SIMROOT/(firmware|apps|lib|uisimulator)#-I$ROOT/\\1#g")
[ -n "$FL" ] || { echo "FAIL: no compile line from $SIMDIR"; exit 1; }

mkdir -p "$TMP/before/apps/recorder"
for f in jpeg_load.c jpeg_load.h jpeg_common.h resize.c resize.h bmp.c bmp.h; do
    git -C "$ROOT" show "$BASE:apps/recorder/$f" > "$TMP/before/apps/recorder/$f"
done
build() {   # dir-of-apps/recorder out
    for f in jpeg_load resize bmp; do
        # shellcheck disable=SC2086
        "$HOSTCC" -I"$1" $FL -w -c "$1/$f.c" -o "$TMP/$2-$f.o"
    done
    "$HOSTCC" -O1 -w -o "$TMP/$2.bin" "$T/artread.c" "$TMP/$2-jpeg_load.o" \
        "$TMP/$2-resize.o" "$TMP/$2-bmp.o"
}
build "$TMP/before/apps/recorder" before
build "$ROOT/apps/recorder" after

python3 - "$TMP" <<'EOF'
import sys, random
from PIL import Image
d = sys.argv[1]
random.seed(7)
def cover(name, side, q):
    im = Image.new('RGB', (side, side))
    px = im.load()
    for y in range(side):
        for x in range(side):
            px[x, y] = ((x * 255 // side) ^ random.randrange(64),
                        (y * 255 // side) ^ random.randrange(64),
                        ((x + y) * 127 // side) ^ random.randrange(64))
    im.save(f'{d}/{name}.jpg', quality=q, progressive=False, optimize=False)
cover('large', 2400, 97)
cover('typical', 600, 90)
EOF
for c in large typical; do
    ffmpeg -loglevel error -f lavfi -t 1 -i anullsrc=r=44100:cl=stereo \
        -i "$TMP/$c.jpg" -map 0:a -map 1:v -c:a flac -c:v copy \
        -disposition:v attached_pic "$TMP/$c.flac"
    ffmpeg -loglevel error -f lavfi -t 1 -i anullsrc=r=44100:cl=stereo \
        -i "$TMP/$c.jpg" -map 0:a -map 1:v -c:a aac -c:v copy \
        -disposition:v attached_pic -f mp4 "$TMP/$c.m4b"
done

fail=0
for c in large typical; do
    for k in jpg flac m4b; do
        set -- $(python3 -c "
import sys
j = open('$TMP/$c.jpg','rb').read(); f = open('$TMP/$c.$k','rb').read()
print(f.find(j), len(j))")
        off=$1 len=$2
        [ "$off" -ge 0 ] || { echo "FAIL: $c.$k: picture not found"; exit 1; }
        for v in before after; do
            r=$("$TMP/$v.bin" "$TMP/$c.$k" "$off" "$len" 142 "$TMP/$v.$c.$k.raw" | tail -1)
            echo "$c.$k off=$off len=$len $v: $r"
        done
        if cmp -s "$TMP/before.$c.$k.raw" "$TMP/after.$c.$k.raw"; then
            echo "ok   $c.$k: decoded pixels identical"
        else
            echo "FAIL: $c.$k: decoded pixels differ"; fail=1
        fi
    done
done
exit $fail
