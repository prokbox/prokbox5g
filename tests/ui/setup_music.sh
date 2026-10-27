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

# Generate a small tagged "mini library" for simulator UI tests.
#
#   tests/ui/setup_music.sh <simdisk-dir>
#
# Writes into <simdisk-dir>/Music/ (normally $SIM_BUILD/simdisk, which lives
# under the gitignored build/). Idempotent: a stamp file records the library
# version, and if it matches and every file exists the script exits at once.
# Bump VERSION whenever the contents below change.
#
#   Music/Tone Collective/Sine Studies/       3 x MP3, embedded cover art
#       01 Concert A.mp3          45 s   440 Hz
#       02 Low E.mp3              30 s   165 Hz
#       03 Warble.mp3             25 s   vibrato tone
#   Music/Square Wave Quartet/Low Frequencies/   2 x MP3, no art
#       01 Hum.mp3                60 s   110 Hz + harmonics
#       02 Beeps.mp3              20 s   pulsed 880 Hz
#   Music/Fixture Author/Mini Audiobook/
#       Mini Audiobook.m4b        48 s   4 chapters (Nero + QuickTime),
#                                        genre Audiobook, embedded cover art
#
# Audio recipes follow tests/fixtures/make_fixtures.sh (lavfi tones, mono
# 22.05 kHz, 32k MP3 / 16k AAC). The cover is an original abstract image
# drawn with lavfi; no third-party art.
#
# Requires ffmpeg (with libmp3lame).
set -eu

VERSION=3   # 2: bitexact cover encode (IMG_EXACT, as setup_library.py 8)
            # 3: pinned mtimes (as setup_library.py 9)
[ -n "${1:-}" ] || { echo "usage: $0 <simdisk-dir>" >&2; exit 2; }
DISK=$1
# Only ever write inside the repo's gitignored build/ tree.
ROOT=$(cd "$(dirname "$0")/../.." && pwd -P)
PARENT=$(cd "$(dirname "$DISK")" 2>/dev/null && pwd -P) \
    || { echo "setup_music: no such directory: $(dirname "$DISK")" >&2; exit 2; }
case "$PARENT/$(basename "$DISK")/" in
    "$ROOT/build/"*) ;;
    *) echo "setup_music: refusing to write outside $ROOT/build/: $DISK" >&2; exit 2;;
esac
mkdir -p "$DISK"
MUSIC="$DISK/Music"
STAMP="$MUSIC/.mini-library"

A1="$MUSIC/Tone Collective/Sine Studies"
A2="$MUSIC/Square Wave Quartet/Low Frequencies"
A3="$MUSIC/Fixture Author/Mini Audiobook"
FILES="$A1/01 Concert A.mp3
$A1/02 Low E.mp3
$A1/03 Warble.mp3
$A2/01 Hum.mp3
$A2/02 Beeps.mp3
$A3/Mini Audiobook.m4b"

uptodate() {
    [ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$VERSION" ] || return 1
    echo "$FILES" | while IFS= read -r f; do
        [ -f "$f" ] || exit 1
    done
}
uptodate && exit 0

command -v ffmpeg >/dev/null || { echo "setup_music: ffmpeg not found" >&2; exit 1; }

TMP=$(mktemp -d "$ROOT/build/.setup_music.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
FF="ffmpeg -nostdin -v error -y"
BITEXACT="-fflags +bitexact -flags:a +bitexact"
# image encode: no "Lavc<version>" JPEG comment, SIMD-independent swscale
IMG_EXACT="-fflags +bitexact -flags:v +bitexact -sws_flags bicubic+bitexact+accurate_rnd+full_chroma_int"
MP3="-c:a libmp3lame -b:a 32k -ac 1 -ar 22050 -id3v2_version 3"
AAC="-c:a aac -b:a 16k -ac 1 -ar 22050"

rm -rf "$A1" "$A2" "$A3"
mkdir -p "$A1" "$A2" "$A3"

# Cover: diagonal two-tone gradient with concentric rings (240x240 baseline
# JPEG, which is what the Rockbox album art decoder handles best).
$FF -f lavfi -i "gradients=s=240x240:c0=0x1d3557:c1=0xe76f51:x0=0:y0=0:x1=240:y1=240:d=1:seed=1" \
    -vf "geq=r='r(X,Y)*(1-0.35*lt(mod(hypot(X-120,Y-120),40),12))':g='g(X,Y)*(1-0.35*lt(mod(hypot(X-120,Y-120),40),12))':b='b(X,Y)*(1-0.35*lt(mod(hypot(X-120,Y-120),40),12))',format=yuvj420p" \
    -frames:v 1 -c:v mjpeg -q:v 4 $IMG_EXACT "$TMP/cover.jpg"

# mp3 <out> <lavfi-audio> <seconds> <title> <artist> <album> <track> [art]
mp3() {
    out=$1; src=$2; dur=$3; title=$4; artist=$5; album=$6; track=$7
    if [ -n "${8:-}" ]; then
        $FF -f lavfi -i "$src" -i "$8" -t "$dur" -map 0:a -map 1:v \
            $MP3 -c:v copy -disposition:v attached_pic \
            -metadata:s:v title="Album cover" -metadata:s:v comment="Cover (front)" \
            $BITEXACT -metadata title="$title" -metadata artist="$artist" \
            -metadata album_artist="$artist" -metadata album="$album" \
            -metadata track="$track" -metadata genre="Electronic" \
            -metadata date=2026 "$out"
    else
        $FF -f lavfi -i "$src" -t "$dur" -map 0:a \
            $MP3 $BITEXACT -metadata title="$title" -metadata artist="$artist" \
            -metadata album_artist="$artist" -metadata album="$album" \
            -metadata track="$track" -metadata genre="Ambient" \
            -metadata date=2025 "$out"
    fi
}

ART="Tone Collective"; ALB="Sine Studies"
mp3 "$A1/01 Concert A.mp3" "sine=frequency=440:sample_rate=22050" 45 \
    "Concert A" "$ART" "$ALB" 1/3 "$TMP/cover.jpg"
mp3 "$A1/02 Low E.mp3" "sine=frequency=165:sample_rate=22050" 30 \
    "Low E" "$ART" "$ALB" 2/3 "$TMP/cover.jpg"
mp3 "$A1/03 Warble.mp3" \
    "aevalsrc=0.5*sin(2*PI*(660*t+20*sin(2*PI*3*t))):s=22050" 25 \
    "Warble" "$ART" "$ALB" 3/3 "$TMP/cover.jpg"

ART="Square Wave Quartet"; ALB="Low Frequencies"
mp3 "$A2/01 Hum.mp3" \
    "aevalsrc=0.4*sin(2*PI*110*t)+0.2*sin(2*PI*220*t)+0.1*sin(2*PI*330*t):s=22050" \
    60 "Hum" "$ART" "$ALB" 1/2
mp3 "$A2/02 Beeps.mp3" \
    "aevalsrc=0.5*sin(2*PI*880*t)*lt(mod(t\,1)\,0.3):s=22050" 20 \
    "Beeps" "$ART" "$ALB" 2/2

# Audiobook with chapters; the ipod muxer writes both the Nero 'chpl' list
# and the QuickTime chapter track (see tests/fixtures/make_fixtures.sh).
cat > "$TMP/book.txt" <<'EOF'
;FFMETADATA1
title=Mini Audiobook
artist=Fixture Author
album_artist=Fixture Author
album=Mini Audiobook
genre=Audiobook
date=2026
[CHAPTER]
TIMEBASE=1/1000
START=0
END=8000
title=Opening Credits
[CHAPTER]
TIMEBASE=1/1000
START=8000
END=22000
title=Chapter 1: The Tone
[CHAPTER]
TIMEBASE=1/1000
START=22000
END=38000
title=Chapter 2: The Warble
[CHAPTER]
TIMEBASE=1/1000
START=38000
END=48000
title=End Credits
EOF
$FF -f lavfi -i "sine=frequency=330:sample_rate=22050:duration=48" \
    -i "$TMP/book.txt" -i "$TMP/cover.jpg" \
    -map 0:a -map 2:v -map_metadata 1 -map_chapters 1 \
    $AAC -c:v copy -disposition:v attached_pic $BITEXACT \
    -f ipod "$A3/Mini Audiobook.m4b"

# Pinned mtimes (2026-01-01 00:00:00 UTC on, 10 s apart in sorted path
# order, before setup_library.py's range): the shell's panel set picks the
# "newest albums" by tag_mtime, so generation wall-clock times would make
# it differ per fresh disk.
python3 -c '
import os, sys
fs = sorted(os.path.join(r, n) for r, _, ns in os.walk(sys.argv[1]) for n in ns)
for i, f in enumerate(fs):
    os.utime(f, (1767225600 + 10 * i,) * 2)
' "$MUSIC"
echo "$VERSION" > "$STAMP"
echo "setup_music: mini library written to $MUSIC" >&2
