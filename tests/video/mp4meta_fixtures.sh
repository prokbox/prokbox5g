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

# Fixtures for test_mp4meta (Phase 14a V2, docs/phase14a-brief.md 3.2, A3):
# tiny synthetic MP4/M4V files (lavfi testsrc2 + sine, 3 s, 64x48), made
# with ffmpeg into the directory given (default build/video/meta). Nothing
# is committed: the expected values are the tags written here and ffprobe's
# own durations, read by tests/video/run.sh.
#
#   movie.m4v    avc1 + AAC, stik 9, 3 chapters (first at 0), a 'pcst'
#                patched in over '(c)grp' (ffmpeg can't write 'pcst'; H1)
#   late.m4v     stik 6, 2 chapters, the first at 2 s (lead_trim must be 0)
#   tv.m4v       stik 10, tvsh / tvsn / tves
#   afirst.m4v   the audio trak first (-map 1:a -map 0:v), tvsh only
#   audio.mp4    AAC only: not a video
#   high.m4v     High profile avc1: still a video (the player falls back)
#   mpeg4.m4v    MPEG-4 Part 2 ('mp4v'): a video (never music), not
#                playable (never listed in Videos; #85 B-M3)
set -e
OUT=${1:-build/video/meta}
mkdir -p "$OUT"
cd "$OUT"

V="-f lavfi -i testsrc2=size=64x48:rate=10:duration=3"
A="-f lavfi -i sine=frequency=440:sample_rate=44100:duration=3"
ENC="-c:a aac -b:a 64k -ac 2 -ar 44100 -fflags +bitexact -flags:v +bitexact
     -flags:a +bitexact"
BASE="-c:v libx264 -profile:v baseline -pix_fmt yuv420p $ENC"

cat >ch3.txt <<'EOF'
;FFMETADATA1
[CHAPTER]
TIMEBASE=1/1000
START=0
END=1000
title=One
[CHAPTER]
TIMEBASE=1/1000
START=1000
END=2000
title=Two
[CHAPTER]
TIMEBASE=1/1000
START=2000
END=3000
title=Three
EOF
cat >chlate.txt <<'EOF'
;FFMETADATA1
[CHAPTER]
TIMEBASE=1/1000
START=2000
END=2500
title=Late
[CHAPTER]
TIMEBASE=1/1000
START=2500
END=3000
title=Later
EOF

# shellcheck disable=SC2086
{
ffmpeg -v error -y $V $A -i ch3.txt -map 0:v -map 1:a -map_chapters 2 $BASE \
    -metadata title="A Movie" -metadata media_type=9 -metadata grouping=X \
    -movflags +faststart movie.m4v
ffmpeg -v error -y $V $A -i chlate.txt -map 0:v -map 1:a -map_chapters 2 \
    $BASE -metadata media_type=6 late.m4v
ffmpeg -v error -y $V $A -map 0:v -map 1:a $BASE -metadata media_type=10 \
    -metadata show="Show A" -metadata season_number=2 \
    -metadata episode_sort=3 -metadata title="Ep" tv.m4v
ffmpeg -v error -y $V $A -map 1:a -map 0:v $BASE -metadata show="Show B" \
    afirst.m4v
ffmpeg -v error -y $A -map 0:a $ENC audio.mp4
ffmpeg -v error -y $V $A -map 0:v -map 1:a -c:v libx264 -profile:v high \
    -pix_fmt yuv420p $ENC high.m4v
ffmpeg -v error -y $V $A -map 0:v -map 1:a -c:v mpeg4 $ENC mpeg4.m4v
}

# '(c)grp' with a 1-byte UTF-8 "X" and 'pcst' with a 1-byte int are both
# 25-byte atoms: retype it in place (type 1 -> 21, "X" -> 1)
python3 -I - movie.m4v <<'EOF'
import sys
p = sys.argv[1]
b = bytearray(open(p, 'rb').read())
old = b'\x00\x00\x00\x19\xa9grp\x00\x00\x00\x11data\x00\x00\x00\x01\x00\x00\x00\x00X'
new = b'\x00\x00\x00\x19pcst\x00\x00\x00\x11data\x00\x00\x00\x15\x00\x00\x00\x00\x01'
i = b.find(old)
if i < 0 or b.find(old, i + 1) >= 0:
    sys.exit('mp4meta_fixtures: no unique (c)grp atom to patch')
b[i:i + len(old)] = new
open(p, 'wb').write(b)
EOF
