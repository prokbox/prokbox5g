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

# Generate the Phase 14a (#64) video fixtures, deterministically.
#
#   tests/fixtures/videos/mkvideos.sh [--install <simdisk>] [<outdir>]
#
# Writes <outdir>/tree/ (default build/video-fixtures/tree), a ready-made
# disk layout, plus <outdir>/SHA256SUMS. --install then copies the tree into
# <simdisk> (which must be under build/), keeping the pinned mtimes.
# Idempotent: a stamp holding VERSION and the hash of this script and of the
# source clip makes a rerun exit at once. Nothing here is committed; the
# only input is the CC BY clip bcmfw/video/stage6-real.mp4 (Sintel trailer
# excerpt, credited in README.md). Posters are our own synthetic JPEGs.
#
# Converter: docs/phase14a-brief.md 3.1 writes these with
# scripts/prokbox-convert.py --bitexact-test. Its tag/kind options were not
# frozen when this was written, so the plain ffmpeg commands with the
# converter's parameters are used (the same ones). The source clip already
# is in the device profile (320x240, H.264 Baseline L1.3, one slice, no
# B-frames, AAC-LC), so those files remux with -c copy, which is bit-exact
# and fast; only the High-profile file is encoded (libx264, one thread,
# bitexact flags). If scripts/prokbox-convert.py exists it is used
# afterwards only to --check one output (a note, never a failure).
#
#   Movies/Short Film.m4v            stik 9, poster (covr), 3 chapters at 0, 4, 8 s
#   Movies/Late Chapter.m4v          stik 9, sidecar poster, first chapter at 2 s
#   Movies/High Profile.m4v          High profile, B-frames (audio-only fallback)
#   TV Shows/Show A/Season 1|2/...   tagged: stik 10, tvsh, tvsn, tves (2 x 2), S1E1 has a poster
#   TV Shows/Show B/Season 1/...     untagged (folder rule only), 2 episodes
#   Misc/Show.C.S01E02.m4v           untagged, filename rule
#   Misc/S01E03.m4v                  untagged, nameless show
#   Misc/Audio First.m4v             audio trak first (-map 1:a -map 0:v)
#   Podcasts/Pod One/Video Episode.m4v + Audio Episode.m4a   genre Podcast
#   Music/Fixture Singer/Audio Only/Audio Only.mp4     audio only: stays a song
#   Music/Fixture Author/Image Book/Image Book.m4b     mjpeg chapter-image trak: stays a book
#
# Requires ffmpeg (libx264, mjpeg) and ffprobe.
set -eu

VERSION=1
INSTALL=""
OUT=""
while [ $# -gt 0 ]; do
    case "$1" in
        --install) INSTALL=$2; shift 2 ;;
        -*) echo "usage: $0 [--install <simdisk>] [<outdir>]" >&2; exit 2 ;;
        *) OUT=$1; shift ;;
    esac
done
ROOT=$(cd "$(dirname "$0")/../../.." && pwd -P)
[ -n "$OUT" ] || OUT=$ROOT/build/video-fixtures
SRC=$ROOT/bcmfw/video/stage6-real.mp4
[ -f "$SRC" ] || { echo "mkvideos: missing $SRC" >&2; exit 1; }

# Only ever write inside the repo's gitignored build/ tree.
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd -P)
case "$OUT/" in
    "$ROOT/build/"*) ;;
    *) echo "mkvideos: refusing to write outside $ROOT/build/: $OUT" >&2; exit 2 ;;
esac

hash_tool() { command -v sha256sum >/dev/null 2>&1 && sha256sum || shasum -a 256; }
KEY=$( { echo "v$VERSION"; cat "$0" "$SRC"; } | hash_tool | awk '{print $1}')
TREE=$OUT/tree
STAMP=$OUT/.stamp

install_tree() {
    [ -n "$INSTALL" ] || return 0
    case "$(cd "$INSTALL" && pwd -P)/" in
        "$ROOT/build/"*) ;;
        *) echo "mkvideos: refusing to install outside $ROOT/build/: $INSTALL" >&2; exit 2 ;;
    esac
    # contents only, never deletes; -p keeps the pinned mtimes
    cp -pR "$TREE"/. "$INSTALL"/
}

if [ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$KEY" ] && [ -d "$TREE/Movies" ]; then
    install_tree; exit 0
fi

command -v ffmpeg >/dev/null && command -v ffprobe >/dev/null \
    || { echo "mkvideos: ffmpeg/ffprobe not found" >&2; exit 1; }

WORK=$OUT/.work
rm -rf "$TREE" "$WORK"; mkdir -p "$TREE" "$WORK"
trap 'rm -rf "$WORK"' EXIT

FF="ffmpeg -nostdin -v error -y"
EXACT="-fflags +bitexact -flags:v +bitexact -flags:a +bitexact -map_metadata -1"
IMG_EXACT="-fflags +bitexact -flags:v +bitexact -sws_flags bicubic+bitexact+accurate_rnd+full_chroma_int"

# Posters: 200x300 baseline JFIF JPEGs (<= 480x720), gradients with rings,
# a different pair of colours each; all drawn with lavfi.
poster() { # <out.jpg> <c0> <c1> <seed>
    $FF -f lavfi -i "gradients=s=200x300:c0=$2:c1=$3:x0=0:y0=0:x1=200:y1=300:d=1:seed=$4" \
        -vf "geq=r='r(X,Y)*(1-0.35*lt(mod(hypot(X-100,Y-150),40),12))':g='g(X,Y)*(1-0.35*lt(mod(hypot(X-100,Y-150),40),12))':b='b(X,Y)*(1-0.35*lt(mod(hypot(X-100,Y-150),40),12))',format=yuvj420p" \
        -frames:v 1 -q:v 3 $IMG_EXACT -c:v mjpeg -f image2 "$1"
}
poster "$WORK/p_film.jpg"  0x264653 0xe9c46a 11
poster "$WORK/p_late.jpg"  0x6a040f 0xffba08 12
poster "$WORK/p_showa.jpg" 0x03045e 0x90e0ef 13

# Chapters, ffmetadata (ms timebase): chapfile <out> <start:end:title>...
chapfile() {
    out=$1; shift
    printf ';FFMETADATA1\n' >"$out"
    for c in "$@"; do
        s=${c%%:*}; r=${c#*:}; e=${r%%:*}; t=${r#*:}
        printf '[CHAPTER]\nTIMEBASE=1/1000\nSTART=%s\nEND=%s\ntitle=%s\n' "$s" "$e" "$t" >>"$out"
    done
}
chapfile "$WORK/ch_film.txt" 0:4000:Opening 4000:8000:Middle 8000:13000:Ending
chapfile "$WORK/ch_late.txt" 2000:8000:Second 8000:13000:Third

# mk <out> <ffmpeg input/map/metadata args...>. Always -f ipod (an .m4v
# name would otherwise pick the raw H.264 muxer); FMT=mp4 for the mjpeg
# book, which the ipod muxer refuses as a plain stream.
mk() {
    out=$1; shift
    mkdir -p "$(dirname "$out")"
    $FF "$@" $EXACT -movflags +faststart -f "${FMT:-ipod}" "$out"
}
T6=12   # seconds kept for TV and podcast episodes (keyframes every 2 s)

# Movies
mk "$TREE/Movies/Short Film.m4v" -i "$SRC" -i "$WORK/p_film.jpg" -i "$WORK/ch_film.txt" \
    -map 0:v -map 0:a -map 1:v -map_chapters 2 -c copy -disposition:v:1 attached_pic \
    -metadata title="Short Film" -metadata media_type=9 -metadata artist="Fixture Author"
mk "$TREE/Movies/Late Chapter.m4v" -i "$SRC" -i "$WORK/ch_late.txt" \
    -map 0:v -map 0:a -map_chapters 1 -c copy \
    -metadata title="Late Chapter" -metadata media_type=9
cp "$WORK/p_late.jpg" "$TREE/Movies/Late Chapter.jpg"
# High profile with B-frames (the device cannot decode it: audio fallback)
mk "$TREE/Movies/High Profile.m4v" -i "$SRC" -t 5 \
    -c:v libx264 -profile:v high -preset medium -threads 1 \
    -x264-params ref=3:bframes=2:threads=1:sliced-threads=0 -pix_fmt yuv420p -b:v 600k -g 48 \
    -c:a copy -metadata title="High Profile" -metadata media_type=9

# TV: tagged show, 2 seasons x 2 episodes
for s in 1 2; do for e in 1 2; do
    f="$TREE/TV Shows/Show A/Season $s/Show A S0${s}E0${e}.m4v"
    if [ $s = 1 ] && [ $e = 1 ]; then
        mk "$f" -i "$SRC" -i "$WORK/p_showa.jpg" -t $T6 -map 0:v -map 0:a -map 1:v -c copy \
            -disposition:v:1 attached_pic \
            -metadata title="Pilot" -metadata media_type=10 -metadata show="Show A" \
            -metadata season_number=$s -metadata episode_sort=$e -metadata episode_id="S0${s}E0${e}"
    else
        mk "$f" -i "$SRC" -t $T6 -map 0:v -map 0:a -c copy \
            -metadata title="Episode $s.$e" -metadata media_type=10 -metadata show="Show A" \
            -metadata season_number=$s -metadata episode_sort=$e -metadata episode_id="S0${s}E0${e}"
    fi
done; done
# TV: untagged, folder only (no title, no stik, no tvsh)
mk "$TREE/TV Shows/Show B/Season 1/Show B S01E01.m4v" -i "$SRC" -t $T6 -map 0:v -map 0:a -c copy
mk "$TREE/TV Shows/Show B/Season 1/Show B S01E02.m4v" -i "$SRC" -t $T6 -map 0:v -map 0:a -c copy
# TV: filename rules only, outside any category folder
mk "$TREE/Misc/Show.C.S01E02.m4v" -i "$SRC" -t $T6 -map 0:v -map 0:a -c copy
mk "$TREE/Misc/S01E03.m4v" -i "$SRC" -t $T6 -map 0:v -map 0:a -c copy
# audio trak first (H4: the length must be the audio's)
mk "$TREE/Misc/Audio First.m4v" -i "$SRC" -i "$SRC" -map 1:a -map 0:v -c copy \
    -metadata title="Audio First" -metadata media_type=9

# Podcasts: a video episode beside an audio one
mk "$TREE/Podcasts/Pod One/Video Episode.m4v" -i "$SRC" -t $T6 -map 0:v -map 0:a -c copy \
    -metadata title="Video Episode" -metadata album="Pod One" -metadata genre=Podcast
mk "$TREE/Podcasts/Pod One/Audio Episode.m4a" -i "$SRC" -t $T6 -vn -c:a copy \
    -metadata title="Audio Episode" -metadata album="Pod One" -metadata genre=Podcast
# audio-only .mp4 (a song)
# (its own artist: Fixture Author is an audiobook author in the test
# manifest, tests/ui/library_manifest.txt, so never a music artist)
mk "$TREE/Music/Fixture Singer/Audio Only/Audio Only.mp4" -i "$SRC" -vn -c:a copy \
    -metadata title="Audio Only" -metadata artist="Fixture Singer" -metadata album="Audio Only"
# m4b with an mjpeg chapter-image trak (a book: mjpeg is not avc1)
mkdir -p "$TREE/Music/Fixture Author/Image Book"
FMT=mp4 mk "$TREE/Music/Fixture Author/Image Book/Image Book.m4b" \
    -f lavfi -i "gradients=s=160x120:c0=0x1d3557:c1=0xe76f51:x0=0:y0=0:x1=160:y1=120:d=13:r=1:seed=5" \
    -i "$SRC" -map 1:a -map 0:v \
    -c:v mjpeg -q:v 5 -c:a copy -shortest \
    -metadata title="Image Book" -metadata artist="Fixture Author" \
    -metadata album="Image Book" -metadata genre=Audiobook

# Pin every mtime (the .vix ledger and the database key on them)
find "$TREE" -exec touch -t 202601010000 {} +

# Manifest of hashes (reproducibility check)
( cd "$TREE" && find . -type f | sort | while IFS= read -r f; do
    h=$(hash_tool <"$f" | awk '{print $1}'); printf '%s  %s\n' "$h" "$f"; done ) >"$OUT/SHA256SUMS"

if [ -f "$ROOT/scripts/prokbox-convert.py" ]; then
    python3 -I "$ROOT/scripts/prokbox-convert.py" --check "$TREE/Movies/Short Film.m4v" \
        >"$OUT/convert-check.txt" 2>&1 \
        || echo "note mkvideos: converter --check reported problems (see $OUT/convert-check.txt)" >&2
fi
echo "$KEY" >"$STAMP"
install_tree
