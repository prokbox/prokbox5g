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
# The device test's stage 6 clip (bcmfw/video/stage6-clip.mp4): 8 s of
# ffmpeg's own test pattern with its seconds counter, 320x240 at 30 fps,
# H.264 Baseline (x264, one thread, bit-exact flags: the same bytes each
# run) and a 440 Hz AAC tone.  Entirely synthetic: never a frame of
# anyone's media.  buildzip.pl copies it to .rockbox/bcmtest/clip.mp4 in
# --bcmfw-devtest --bcmfw-video builds.
#
# Usage: bcmfw/video/mkclip.sh [out.mp4]
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
OUT=${1:-$(dirname "$0")/stage6-clip.mp4}
ffmpeg -v error -y \
    -f lavfi -i "testsrc=size=320x240:rate=30:duration=8" \
    -f lavfi -i "sine=frequency=440:sample_rate=44100:duration=8" \
    -map 0:v -map 1:a \
    -c:v libx264 -profile:v baseline -level 1.3 -preset medium -threads 1 \
    -b:v 250k -maxrate 400k -bufsize 400k -g 30 -bf 0 -pix_fmt yuv420p \
    -c:a aac -b:a 64k -ac 1 -movflags +faststart -map_metadata -1 \
    -fflags +bitexact -flags:v +bitexact -flags:a +bitexact "$OUT"
