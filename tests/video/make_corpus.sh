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
# H.264 Baseline test corpus for our VideoCore decoder (bcmfw/h264, #60).
# Synthetic content only (ffmpeg lavfi sources), encoded with libx264,
# Baseline profile, CAVLC.  Reference output is ffmpeg's own decode as raw
# yuv420p.  Media goes to build/video/corpus (never committed).
#
#   tests/video/make_corpus.sh          build what is missing
#   tests/video/make_corpus.sh --force  rebuild everything
#   tests/video/make_corpus.sh --check  compare with tests/video/corpus.sha256
#
# The list of clips (name, size, frames, source, x264 options) is below;
# tests/video/README.md says what each one is for.
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
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT="$ROOT/build/video/corpus"
FFMPEG=${FFMPEG:-/opt/homebrew/bin/ffmpeg}
SUMS="$ROOT/tests/video/corpus.sha256"
FORCE=0
CHECK=0
case "$1" in
    --force) FORCE=1 ;;
    --check) CHECK=1 ;;
    "") ;;
    *) echo "usage: $0 [--force|--check]" >&2; exit 2 ;;
esac
[ -x "$FFMPEG" ] || { echo "make_corpus: ffmpeg not found ($FFMPEG)" >&2; exit 2; }
mkdir -p "$OUT"

# clip NAME WxH FRAMES FPS "LAVFI SOURCE" "FFMPEG/X264 OPTIONS"
clip() {
    name=$1 size=$2 frames=$3 fps=$4 src=$5 opts=$6
    if [ $FORCE -eq 0 ] && [ -f "$OUT/$name.264" ] && [ -f "$OUT/$name.yuv" ]; then
        return
    fi
    # shellcheck disable=SC2086
    "$FFMPEG" -hide_banner -loglevel error -y -f lavfi -i "$src" \
        -frames:v "$frames" -r "$fps" -s "$size" -pix_fmt yuv420p \
        -c:v libx264 -profile:v baseline -coder 0 $opts \
        -bsf:v h264_mp4toannexb -f h264 "$OUT/$name.264"
    "$FFMPEG" -hide_banner -loglevel error -y -i "$OUT/$name.264" \
        -fps_mode passthrough -f rawvideo -pix_fmt yuv420p "$OUT/$name.yuv"
    echo "$name: $(wc -c <"$OUT/$name.264" | tr -d ' ') bytes, $frames frames"
}

MOVE="testsrc2=size=320x240:rate=30,rotate=a=0.15*t:c=black"
PAN="testsrc2=size=480x360:rate=30,scroll=h=0.0031:v=0.0017,crop=320:240"
MANDEL="mandelbrot=size=320x240:rate=30"
BARS="smptebars=size=320x240:rate=30,scroll=h=0.0041"

# (a) I-only, several QPs
for qp in 8 20 32 44 51; do
    clip "a_ionly_qp$qp" 320x240 4 30 "$MOVE" "-x264-params keyint=1:qp=$qp"
done
clip a_ionly_mandel 320x240 4 30 "$MANDEL" "-x264-params keyint=1:qp=24"
# (b) IPPP, short GOP; several reference frames
clip b_ippp_gop10 320x240 30 30 "$PAN" "-x264-params keyint=10:min-keyint=10:crf=23:ref=3"
clip b_ippp_mandel 320x240 20 30 "$MANDEL" "-x264-params keyint=12:crf=20:ref=2"
# (c) --tune fastdecode (no deblocking)
clip c_fastdecode 320x240 30 30 "$MOVE" "-tune fastdecode -x264-params keyint=15:crf=22"
# (d) the iPod spec: 768 kbit/s, 30 fps, about 2 s, level 3.0
clip d_ipod_768k 320x240 60 30 "$PAN" "-level 3.0 -b:v 768k -maxrate 768k -bufsize 2000k -x264-params keyint=30"
# (e) 640x480, for the scaling work later
clip e_vga 640x480 10 30 "testsrc2=size=640x480:rate=30,rotate=a=0.1*t:c=black" "-x264-params keyint=5:crf=24"
# (f) multiple slices, deblocking offsets, constrained intra prediction
clip f_slices 320x240 12 30 "$BARS" "-x264-params keyint=6:crf=26:slices=4:deblock=-2,2:constrained-intra=1:ref=2"
# (h) every partition size down to 4x4, four references, mixed refs
clip h_partitions 320x240 16 30 "$PAN" "-x264-params keyint=16:crf=18:ref=4:partitions=all:mixed-refs=1:subme=9:me=umh"
# (g) cropped size (not a multiple of 16), single reference
clip g_crop 176x120 10 30 "$MOVE" "-x264-params keyint=8:crf=30:ref=1"

if [ $CHECK -eq 1 ]; then
    (cd "$OUT" && shasum -a 256 -c "$SUMS")
fi
