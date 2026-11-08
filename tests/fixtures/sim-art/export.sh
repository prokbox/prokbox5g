#!/bin/bash
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

# Encodes the simulator test library's cover images and the video
# fixtures' posters (synthetic art only:
# procedural patterns and gradients, no third-party or owner media) into
# <dir>, with SHA256SUMS. This is the ONLY place they are encoded: the
# generators (tests/ui/setup_library.py, tests/ui/setup_music.sh) copy the
# committed files byte for byte, because an mjpeg encode differs between
# ffmpeg builds (the Mac's 9.0.2 and Fedora's 8.1.3 wrote different bytes
# for the same cover) and the goldens show every pixel of it.
#
# Run it on the machine whose goldens are canonical, then commit:
#   tests/fixtures/sim-art/export.sh tests/fixtures/sim-art
# New bytes mean new goldens for every shot that shows art.
# Needs ffmpeg and jpegtran or cjpeg (libjpeg-turbo); refuses without them.
set -eu
[ -n "${1:-}" ] || { echo "usage: $0 <dir>" >&2; exit 2; }
OUT=$1
ROOT=$(cd "$(dirname "$0")/../../.." && pwd -P)
command -v ffmpeg >/dev/null || { echo "export.sh: ffmpeg not found" >&2; exit 1; }
command -v jpegtran >/dev/null || command -v cjpeg >/dev/null \
    || { echo "export.sh: jpegtran or cjpeg (libjpeg-turbo) not found" >&2; exit 1; }
mkdir -p "$OUT"
python3 "$ROOT/tests/ui/setup_library.py" --export-art "$OUT"
# setup_music.sh's ring cover (was encoded there until the art was committed)
IMG_EXACT="-fflags +bitexact -flags:v +bitexact -sws_flags bicubic+bitexact+accurate_rnd+full_chroma_int"
ffmpeg -nostdin -v error -y -f lavfi -i "gradients=s=240x240:c0=0x1d3557:c1=0xe76f51:x0=0:y0=0:x1=240:y1=240:d=1:seed=1" \
    -vf "geq=r='r(X,Y)*(1-0.35*lt(mod(hypot(X-120,Y-120),40),12))':g='g(X,Y)*(1-0.35*lt(mod(hypot(X-120,Y-120),40),12))':b='b(X,Y)*(1-0.35*lt(mod(hypot(X-120,Y-120),40),12))',format=yuvj420p" \
    -frames:v 1 -c:v mjpeg -q:v 4 $IMG_EXACT "$OUT/music-cover.jpg"
# tests/fixtures/videos/mkvideos.sh's posters (were encoded there): 200x300
# gradients with rings, one colour pair each
poster() { # <out.jpg> <c0> <c1> <seed>
    ffmpeg -nostdin -v error -y -f lavfi -i "gradients=s=200x300:c0=$2:c1=$3:x0=0:y0=0:x1=200:y1=300:d=1:seed=$4" \
        -vf "geq=r='r(X,Y)*(1-0.35*lt(mod(hypot(X-100,Y-150),40),12))':g='g(X,Y)*(1-0.35*lt(mod(hypot(X-100,Y-150),40),12))':b='b(X,Y)*(1-0.35*lt(mod(hypot(X-100,Y-150),40),12))',format=yuvj420p" \
        -frames:v 1 -q:v 3 $IMG_EXACT -c:v mjpeg -f image2 "$1"
}
poster "$OUT/p_film.jpg"  0x264653 0xe9c46a 11
poster "$OUT/p_late.jpg"  0x6a040f 0xffba08 12
poster "$OUT/p_showa.jpg" 0x03045e 0x90e0ef 13
rm -f "$OUT"/*.ppm
cd "$OUT"
names="cover0.jpg cover1.jpg cover2.jpg cover3.jpg cover4.jpg cover5.jpg cover_nonsquare.jpg cover.png cover_progressive.jpg music-cover.jpg p_film.jpg p_late.jpg p_showa.jpg"
for n in $names; do [ -s "$n" ] || { echo "export.sh: $n not written" >&2; exit 1; }; done
if command -v sha256sum >/dev/null 2>&1; then sha256sum $names; else shasum -a 256 $names; fi > SHA256SUMS
echo "export.sh: $(echo $names | wc -w | tr -d ' ') images and SHA256SUMS in $OUT ($(ffmpeg -version | head -1 | cut -d' ' -f1-3))"
