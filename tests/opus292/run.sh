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

# #292: Opus. With build/warble's real codec and metadata parser, against
# ffmpeg (its libopus encoder makes the files, its decoder is the
# reference):
#  1. length: the decode is exactly as long as the reference's for files of
#     one audio page (a 0.3 s mono clip: upstream found no audio at all in
#     those, its page search took only header type 0 and its packet loop
#     stopped at the gap the skipped comment pages leave), two pages and
#     many: the stream's last packet is decoded (upstream stopped in front
#     of it) and trimmed to the last page's granule position (RFC 7845 4.4);
#     and the samples are the reference's (SNR >= 40 dB, aligned);
#  2. no "codec error" at the end of a track (upstream returned one after
#     every Opus file);
#  3. Opus in a .ogg file is found by its content and decodes exactly as
#     the same file named .opus;
#  4. R128_TRACK_GAIN / R128_ALBUM_GAIN (Q7.8 dB to -23 LUFS) become
#     ReplayGain levels + 5 dB, pinned: -2560 -> -5 dB (level -20480 in
#     Q19.12), 512 -> +7 dB (28672); they win over REPLAYGAIN_* in either
#     order; REPLAYGAIN_* alone still works (-1.00 dB -> -4096); an R128 tag
#     in a FLAC file is ignored.
#   WARBLEDIR  a built warble (default build/warble; scripts/test.sh builds it)
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT" || exit 1
WARBLEDIR=${WARBLEDIR:-$ROOT/build/warble}
W=$WARBLEDIR/warble.ipodvideo
[ -x "$W" ] || { echo "FAIL: no warble at $W"; exit 1; }
command -v ffmpeg >/dev/null 2>&1 || { echo "FAIL: opus292 needs ffmpeg"; exit 1; }
ffmpeg -hide_banner -encoders 2>/dev/null | grep -q libopus \
    || { echo "FAIL: opus292 needs ffmpeg with libopus"; exit 1; }
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

fail=0
checked=0
bad() { echo "FAIL opus292: $*"; fail=1; }

mk() {      # NAME SECONDS CHANNELS [ffmpeg options...]: a two-tone signal
    n=$1 s=$2 c=$3
    shift 3
    ffmpeg -nostdin -v error -y -f lavfi \
        -i "aevalsrc=0.3*sin(440*2*PI*t)+0.2*sin(1250*2*PI*t):s=48000:d=$s" \
        -ac "$c" -c:a libopus -b:a 96k "$@" "$TMP/$n.opus"
}

# 1, 2: lengths and samples
length() {  # NAME
    rm -f "$TMP/$1.wav"
    "$W" -c outrate=0 "$TMP/$1.opus" "$TMP/$1.wav" >"$TMP/$1.log" 2>&1
    if grep -q "codec error" "$TMP/$1.log"; then
        bad "$1: the codec reported an error"
        return
    fi
    ffmpeg -nostdin -v error -y -i "$TMP/$1.opus" -f wav -acodec pcm_s16le "$TMP/$1.ref.wav" \
        || { bad "$1: ffmpeg decode"; return; }
    if r=$(python3 -I - "$TMP/$1.wav" "$TMP/$1.ref.wav" 2>&1 <<'EOF'
import math, struct, sys, wave
def pcm(p):
    w = wave.open(p)
    n = w.getnframes() * w.getnchannels()
    return (w.getnframes(), w.getnchannels(),
            struct.unpack('<%dh' % n, w.readframes(w.getnframes())))
try:
    na, ca, a = pcm(sys.argv[1])
except Exception as e:
    sys.exit('no decode (%s)' % e)
nb, cb, b = pcm(sys.argv[2])
if ca == 2 and cb == 1:     # warble writes a mono stream as two channels
    a = a[0::2]
if na != nb:
    sys.exit('%d samples, the reference %d' % (na, nb))
sig = sum(x * x for x in b) / len(b)
err = sum((x - y) ** 2 for x, y in zip(a, b)) / len(b)
if math.sqrt(sig) < 1000:
    sys.exit('the reference is near silence')
snr = 10 * math.log10(sig / err) if err else 99.0
if snr < 40:
    sys.exit('SNR %.1f dB < 40 against the reference' % snr)
print('%d samples, as the reference; SNR %.1f dB' % (na, snr))
EOF
    ); then
        echo "     $1: $r"
        checked=$((checked + 1))
    else
        bad "$1: $r"
    fi
}

mk clip 0.3 1 && length clip || bad "clip: encoder"
mk two 1.013 2 && length two || bad "two: encoder"
mk many 3.71 2 && length many || bad "many: encoder"
mk f60 2.2 2 -frame_duration 60 && length f60 || bad "f60: encoder"

# 3: .ogg
if [ -s "$TMP/two.wav" ]; then
    cp "$TMP/two.opus" "$TMP/two_ogg.ogg"
    "$W" -m "$TMP/two_ogg.ogg" >"$TMP/ogg.meta" 2>&1
    "$W" -c outrate=0 "$TMP/two_ogg.ogg" "$TMP/ogg.wav" >"$TMP/ogg.log" 2>&1
    if ! grep -q "^Codec: Opus" "$TMP/ogg.meta"; then
        bad ".ogg: not recognised as Opus ($(grep '^Codec' "$TMP/ogg.meta"))"
    elif grep -q "codec error" "$TMP/ogg.log" || ! cmp -s "$TMP/ogg.wav" "$TMP/two.wav"; then
        bad ".ogg: does not decode as the same file named .opus"
    else
        echo "     .ogg: Opus by its content, the same PCM as .opus"
        checked=$((checked + 1))
    fi
fi

# 4: loudness tags
gain() {    # NAME EXPECTED-TRACK-LINE EXPECTED-ALBUM-LINE (or -)
    "$W" -m "$TMP/$1" >"$TMP/$1.meta" 2>&1
    t=$(grep '^Track gain:' "$TMP/$1.meta" | sed 's/ gain=.*//')
    a=$(grep '^Album gain:' "$TMP/$1.meta" | sed 's/ gain=.*//')
    if [ "$t" = "$2" ] && [ "$a" = "$3" ]; then
        echo "     $1: ${t:-no track gain}; ${a:-no album gain}"
        checked=$((checked + 1))
    else
        bad "$1: got '$t' / '$a', want '$2' / '$3'"
    fi
}
mk r128 0.3 2 -metadata R128_TRACK_GAIN=-2560 -metadata R128_ALBUM_GAIN=512 \
    && gain r128.opus "Track gain: level=-20480" "Album gain: level=28672"
mk rgfirst 0.3 2 -metadata REPLAYGAIN_TRACK_GAIN="-1.00 dB" -metadata R128_TRACK_GAIN=-2560 \
    && gain rgfirst.opus "Track gain: level=-20480" ""
mk r128first 0.3 2 -metadata R128_TRACK_GAIN=-2560 -metadata REPLAYGAIN_TRACK_GAIN="-1.00 dB" \
    && gain r128first.opus "Track gain: level=-20480" ""
mk rgonly 0.3 2 -metadata REPLAYGAIN_TRACK_GAIN="-1.00 dB" \
    && gain rgonly.opus "Track gain: level=-4096" ""
ffmpeg -nostdin -v error -y -f lavfi -i "sine=frequency=440:duration=0.3:sample_rate=44100" \
    -c:a flac -metadata R128_TRACK_GAIN=-2560 "$TMP/r128.flac" \
    && gain r128.flac "" ""
# the tags really are in the files (not a vacuous pass)
for f in r128.opus rgfirst.opus r128.flac; do
    ffprobe -v error -show_entries format_tags:stream_tags "$TMP/$f" 2>/dev/null | grep -qi "R128_TRACK_GAIN=-2560" \
        || bad "$f: the fixture has no R128_TRACK_GAIN"
done

[ "$checked" = 10 ] || bad "$checked of 10 checks ran to a verdict"
[ $fail = 0 ] || exit 1
echo "ok   Opus: exact length and end trim, one-page files, .ogg, R128 loudness tags (tests/opus292)"
