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

# Stitch a "record" command's BMP frames into a small H.264 MP4, for the
# #38 (docs/phase-nav-brief.md) owner-round review videos.
#
#   scripts/make_video.sh <frames_dir> <fps> <out.mp4>
#
# <frames_dir> is a "<name>_frames" directory a sim script's "record <ms>
# <fps> <name>" command wrote (uisimulator/common/sim_cmd_record.c), e.g.
# out/shots/ipodui-nav-drift-review/drift-review_frames. <fps> must match
# the fps the script recorded at (the frame files carry no timing of
# their own). Typical use, after scripts/shots.sh ran the capture script:
#
#   scripts/shots.sh tests/ui/ipodui-nav-drift-review.script
#   scripts/make_video.sh \
#       out/shots/ipodui-nav-drift-review/drift-review_frames 20 \
#       out/shots/drift-review.mp4
#
# Output: 320x240 (the LCD's native size: no scaling), H.264 (yuv420p,
# baseline-compatible), faststart for easy preview. A short (20-30 s)
# clip at this size is a few hundred KB to a few MB, comfortably under
# "a few MB at most" (docs/phase-nav-brief.md, Revision 2).
set -e

frames=$1
fps=$2
out=$3

if [ -z "$frames" ] || [ -z "$fps" ] || [ -z "$out" ]; then
    echo "usage: $0 <frames_dir> <fps> <out.mp4>" >&2
    exit 2
fi
[ -d "$frames" ] || { echo "no such directory: $frames" >&2; exit 2; }
first=$(ls "$frames"/frame*.bmp 2>/dev/null | sort | head -n 1)
[ -n "$first" ] || { echo "no frame*.bmp files in $frames" >&2; exit 2; }
command -v ffmpeg >/dev/null 2>&1 || { echo "ffmpeg not found" >&2; exit 2; }

mkdir -p "$(dirname "$out")"
ffmpeg -y -loglevel error -framerate "$fps" -i "$frames/frame%05d.bmp" \
    -c:v libx264 -pix_fmt yuv420p -crf 23 -movflags +faststart "$out"

n=$(ls "$frames"/frame*.bmp 2>/dev/null | wc -l | tr -d ' ')
size=$(du -h "$out" | cut -f1)
echo "make_video: $out ($n frames at $fps fps, $size)"
