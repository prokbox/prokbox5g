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

# Progressive cases for jdec (docs/research/bcm2722/jpeg-spike.md section
# 9): each progressive file with its baseline twin, encoded by cjpeg from
# the same source picture at the same quality and sampling, so the two
# hold the same coefficients and jdec must give the same pixels for both.
# The sources are make_corpus.sh's (build/jpeg/src, only read); the files
# go to build/jpeg-prog (or $1).  Plus: two scan scripts of our own (DC
# alone per component, uneven bands, successive approximation), restart
# intervals, a truncated file, and one past JPEG_PROG_MAX (refused).
# Usage: tests/jpeg/make_prog.sh [outdir]
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
SRC=$ROOT/build/jpeg/src
OUT=${1:-$ROOT/build/jpeg-prog}
mkdir -p "$OUT"
TMP=$(mktemp -d "$OUT/tmp.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
pair() {    # name src args...: p_<name>.jpg and b_<name>.jpg
    n=$1; s=$2; shift 2
    cjpeg -progressive "$@" -outfile "$OUT/p_$n.jpg" "$SRC/$s.ppm"
    cjpeg -baseline "$@" -outfile "$OUT/b_$n.jpg" "$SRC/$s.ppm"
}
pair t600_420 t600 -quality 85 -sample 2x2
pair t600_444 t600 -quality 85 -sample 1x1
pair t600_grey t600 -quality 85 -grayscale
pair h333x500_422 h333x500 -quality 85 -sample 2x1
pair t600_rst t600 -quality 85 -sample 2x2 -restart 3B
pair t142_420 t142 -quality 95 -sample 2x2
pair t1000_420 t1000 -quality 85 -sample 2x2
pair t1500_420 t1500 -quality 85 -sample 2x2
pair t3000_420 t3000 -quality 85 -sample 2x2
# scan scripts: interleaved DC with successive approximation and uneven
# AC bands; then DC per component (non-interleaved) and a refinement
# band that straddles the 1/4 and 1/2 scales' kept coefficients
cat > "$TMP/a.scans" <<'S'
0 1 2: 0 0 0 2;
0: 1 2 0 1;
0: 3 9 0 2;
0: 10 63 0 1;
1: 1 63 0 1;
2: 1 63 0 0;
0 1 2: 0 0 2 1;
0: 3 9 2 1;
0 1 2: 0 0 1 0;
0: 1 63 1 0;
1: 1 63 1 0;
S
cat > "$TMP/b.scans" <<'S'
0: 0 0 0 0;
2: 0 0 0 0;
1: 0 0 0 0;
0: 1 2 0 0;
0: 3 20 0 1;
2: 1 63 0 0;
0: 21 63 0 0;
1: 1 63 0 0;
0: 3 20 1 0;
S
cjpeg -quality 85 -sample 2x2 -scans "$TMP/a.scans" -outfile "$OUT/p_t600_sa.jpg" "$SRC/t600.ppm"
cjpeg -quality 85 -sample 2x2 -scans "$TMP/b.scans" -outfile "$OUT/p_t1000_sb.jpg" "$SRC/t1000.ppm"
cjpeg -quality 85 -sample 2x2 -scans "$TMP/b.scans" -outfile "$OUT/p_t1500_sb.jpg" "$SRC/t1500.ppm"
cp "$OUT/b_t600_420.jpg" "$OUT/b_t600_sa.jpg"
cp "$OUT/b_t1000_420.jpg" "$OUT/b_t1000_sb.jpg"
cp "$OUT/b_t1500_420.jpg" "$OUT/b_t1500_sb.jpg"
# truncated at two thirds: the scans that came, then TRUNCATED
n=$(wc -c < "$OUT/p_t600_420.jpg")
head -c $((n * 2 / 3)) "$OUT/p_t600_420.jpg" > "$OUT/x_t600_trunc.jpg"
# past the cap: 4000 px 4:2:0 at 1/8 keeps 2.4 MB of coefficients
ffmpeg -loglevel error -y -f lavfi -i "testsrc2=size=4000x4000" -frames:v 1 \
    -pix_fmt rgb24 "$TMP/t4000.ppm"
cjpeg -progressive -quality 50 -sample 2x2 -outfile "$OUT/x_t4000_big.jpg" "$TMP/t4000.ppm"
(cd "$OUT" && shasum -a 256 *.jpg) > "$OUT/prog.sha256"
echo "progressive cases: $(ls "$OUT"/*.jpg | wc -l | tr -d ' ') files in $OUT"
