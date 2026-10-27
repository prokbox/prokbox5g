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

# Regenerate the small audio fixtures used by tests/audiobook/run.sh.
#
#   nero_chapters.m4b   AAC in MP4 with only a Nero 'chpl' chapter list
#   apple_chapters.m4b  AAC in MP4 with only a QuickTime 'chap' text track
#   plain.mp3           MP3 with ID3 tags and no chapters
#
# ffmpeg's mp4/ipod muxers write *both* chapter formats by default.
# `-movflags disable_chpl` drops the Nero list, leaving the QuickTime track.
# There is no switch to drop the QuickTime track, so for the Nero fixture the
# chapter 'trak' and the audio track's 'tref' are renamed to 'free' (atoms
# every parser skips), leaving only 'chpl'.
#
# Requires ffmpeg (with libmp3lame) and python3. Run from anywhere.
set -eu

cd "$(dirname "$0")"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

FF="ffmpeg -nostdin -v error -y"
BITEXACT="-fflags +bitexact -flags:a +bitexact"
AAC="-c:a aac -b:a 16k -ac 1 -ar 22050"

# Chapter lists (ffmetadata). The Apple list has two equal-length chapters in
# a row (coalesced into a single 'stts' run) and a title longer than the
# 63-byte limit; both lists include non-ASCII UTF-8.
cat > "$TMP/nero.txt" <<'EOF'
;FFMETADATA1
title=Nero Test Book
artist=Fixture Author
album=Nero Test Book
[CHAPTER]
TIMEBASE=1/1000
START=0
END=2500
title=Intro
[CHAPTER]
TIMEBASE=1/1000
START=2500
END=5000
title=Part 1 – Beginnings
[CHAPTER]
TIMEBASE=1/1000
START=5000
END=7250
title=Part 2
[CHAPTER]
TIMEBASE=1/1000
START=7250
END=8000
title=Credits
EOF

cat > "$TMP/apple.txt" <<'EOF'
;FFMETADATA1
title=Apple Test Book
artist=Fixture Author
album=Apple Test Book
[CHAPTER]
TIMEBASE=1/1000
START=0
END=2000
title=Prologue
[CHAPTER]
TIMEBASE=1/1000
START=2000
END=4000
title=Chapter One: Über Café
[CHAPTER]
TIMEBASE=1/1000
START=4000
END=5500
title=Chapter Two
[CHAPTER]
TIMEBASE=1/1000
START=5500
END=8000
title=Chapter Three, which has a deliberately long title to test truncation
EOF

tone() { echo "-f lavfi -i sine=frequency=$1:duration=$2"; }

# Nero-only: mp4 muxer, then hide the QuickTime chapter track
$FF $(tone 440 8) -i "$TMP/nero.txt" -map 0:a -map_metadata 1 -map_chapters 1 \
    $AAC $BITEXACT -f mp4 "$TMP/nero_both.m4b"
python3 - "$TMP/nero_both.m4b" nero_chapters.m4b <<'EOF'
import struct, sys
d = bytearray(open(sys.argv[1], 'rb').read())

def atoms(off, end):
    while off + 8 <= end:
        size, typ = struct.unpack('>I4s', d[off:off + 8])
        if size < 8:
            raise SystemExit('unexpected atom size')
        yield off, size, typ
        off += size

def find(off, end, typ):
    return [(o, s) for o, s, t in atoms(off, end) if t == typ]

def free(off):
    d[off + 4:off + 8] = b'free'

moov, msize = find(0, len(d), b'moov')[0]
for trak, tsize in find(moov + 8, moov + msize, b'trak'):
    mdia, dsize = find(trak + 8, trak + tsize, b'mdia')[0]
    hdlr, _ = find(mdia + 8, mdia + dsize, b'hdlr')[0]
    handler = bytes(d[hdlr + 16:hdlr + 20])
    if handler == b'text':
        free(trak)
    for tref, _ in find(trak + 8, trak + tsize, b'tref'):
        free(tref)
if b'chpl' not in d:
    raise SystemExit('no chpl atom written')
open(sys.argv[2], 'wb').write(d)
EOF

# QuickTime-only: ipod muxer without the Nero list
$FF $(tone 330 8) -i "$TMP/apple.txt" -map 0:a -map_metadata 1 -map_chapters 1 \
    $AAC $BITEXACT -movflags disable_chpl -f ipod apple_chapters.m4b

# Plain MP3, no chapters
$FF $(tone 550 3) -map_chapters -1 -c:a libmp3lame -b:a 32k -ac 1 -ar 22050 \
    $BITEXACT -metadata title="Plain Song" -metadata artist="Fixture Artist" \
    -id3v2_version 3 plain.mp3

ls -l nero_chapters.m4b apple_chapters.m4b plain.mp3
