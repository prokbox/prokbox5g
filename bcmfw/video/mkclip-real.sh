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
# The device test's second stage 6 clip (bcmfw/video/stage6-real.mp4):
# real footage with fast motion and cuts, so frame pacing and A/V sync
# can be judged on more than a test pattern.  13 s (15.0-28.0 s) of the
# Sintel trailer, (c) Blender Foundation, durian.blender.org, licensed
# CC BY 3.0 (https://creativecommons.org/licenses/by/3.0/); credited in
# README.md and tests/video/README.md.  The letterboxed picture (854x364
# at y 58) centre-cropped to 4:3 so every pixel is footage, scaled
# to 320x240, 24 fps, H.264 Baseline (x264, one thread, bit-exact flags),
# AAC mono.  buildzip.pl copies it to .rockbox/bcmtest/clip2.mp4.
#
# The source is never committed.  Fetch it into a scratch directory of
# its own and pass its path; the script checks its hash first:
#   https://download.blender.org/durian/trailer/sintel_trailer-480p.mp4
#
# Usage: bcmfw/video/mkclip-real.sh sintel_trailer-480p.mp4 [out.mp4]
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
SRC=$1
OUT=${2:-$(dirname "$0")/stage6-real.mp4}
SHA=b670602fa00934ca27c4351bb0efe7ea7a07fae57284e44226025eeed7c51254
[ -f "$SRC" ] || { echo "usage: $0 sintel_trailer-480p.mp4 [out.mp4]"; exit 1; }
[ "$(shasum -a 256 "$SRC" | cut -d' ' -f1)" = "$SHA" ] \
    || { echo "mkclip-real: $SRC is not the expected trailer"; exit 1; }
ffmpeg -v error -y -ss 15 -t 13 -i "$SRC" \
    -vf "crop=484:364:185:58,scale=320:240:flags=lanczos,fps=24" \
    -c:v libx264 -profile:v baseline -level 1.3 -preset medium -threads 1 \
    -b:v 400k -maxrate 600k -bufsize 600k -g 48 -bf 0 -pix_fmt yuv420p \
    -c:a aac -b:a 64k -ac 1 -ar 44100 -movflags +faststart \
    -map_metadata -1 -metadata:s:v handler_name= -metadata:s:a handler_name= \
    -fflags +bitexact -flags:v +bitexact -flags:a +bitexact "$OUT"
