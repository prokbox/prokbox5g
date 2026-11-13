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

# Opus start cost (p083-codecload): an Opus track with embedded art took
# 15 s to start on the device. opus.c found the comment header and then the
# first audio page by reading the file one byte per read_filebuf() call, and
# a codec on the COP makes every call a cross-core round trip: ~118,000 of
# them for this fixture's ~110 KB header. The scan now looks at buffered
# blocks in place (request_buffer/advance_buffer, no call on the COP while
# the bytes are buffered).
#
# Fixture: tests/fixtures/opusstart/tone_art.opus (make_opus_start_fixture.py):
# 2 s of two tones, behind art_2page.opus's two-page comment header.
# Checks, with build/warble's real codec (WARBLE_CI_STATS counts the codec's
# input calls up to its first output):
#  1. the start makes at most 200 input calls in all, and at most 16
#     read_filebuf/seek_buffer calls (the ones that always cross cores);
#     the byte-wise scan made 115,993 reads and 2,145 seeks;
#  2. a start at a byte offset inside the comment header (a resume's offset)
#     and inside the audio also stays under those limits;
#  3. the decode is the file's audio: against ffmpeg's decode of the same
#     file, same length and SNR >= 40 dB, and the reference is not silence.
#   WARBLEDIR  a built warble (default build/warble; scripts/test.sh builds it)
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
WARBLEDIR=${WARBLEDIR:-$ROOT/build/warble}
W=$WARBLEDIR/warble.ipodvideo
F=$ROOT/tests/fixtures/opusstart/tone_art.opus
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

[ -x "$W" ] || { echo "FAIL: no warble at $W"; exit 1; }
command -v ffmpeg >/dev/null 2>&1 || { echo "FAIL: opusstart needs ffmpeg (the reference decode)"; exit 1; }
[ -s "$F" ] || { echo "FAIL: no fixture $F"; exit 1; }

fail=0
# calls CONFIG LABEL: the first-output counts, checked against the limits
calls() {
    if ! WARBLE_CI_STATS=1 "$W" -c "$1" "$F" "$TMP/out.wav" 2>"$TMP/err.log"; then
        echo "FAIL: opusstart: warble failed ($2)"; cat "$TMP/err.log"; fail=1; return
    fi
    line=$(grep '^ci first-out ' "$TMP/err.log")
    [ -n "$line" ] || { echo "FAIL: opusstart: no call counts ($2)"; fail=1; return; }
    set -- "$2" $(echo "$line" | sed -E 's/.*read=([0-9]+) req=([0-9]+) adv=([0-9]+) seek=([0-9]+).*/\1 \2 \3 \4/')
    tot=$(( $2 + $3 + $4 + $5 )); cross=$(( $2 + $5 ))
    if [ $(( $2 + $3 + $4 + $5 )) -eq 0 ]; then
        echo "FAIL: opusstart: no input calls before the first output ($1): nothing measured"; fail=1
    elif [ "$tot" -gt 200 ] || [ "$cross" -gt 16 ]; then
        echo "FAIL: opusstart: $1: $tot input calls before the first output ($cross read/seek; limits 200 and 16)"; fail=1
    else
        echo "     $1: $tot input calls to the first output ($cross read/seek)"
    fi
}

calls outrate=0 "a start"
cp "$TMP/out.wav" "$TMP/start.wav"
calls outrate=0:offset=60000 "an offset in the comment header"
calls outrate=0:offset=120000 "an offset in the audio"

if ! ffmpeg -loglevel error -y -i "$F" -map 0:a -f wav -acodec pcm_s16le "$TMP/ref.wav"; then
    echo "FAIL: opusstart: ffmpeg could not decode the fixture"; exit 1
fi
if ! python3 -I - "$TMP/start.wav" "$TMP/ref.wav" <<'EOF'
import math, struct, sys, wave
def pcm(p):
    w = wave.open(p)
    assert w.getsampwidth() == 2 and w.getnchannels() == 2, p
    return w.getframerate(), struct.unpack('<%dh' % (w.getnframes() * 2),
                                           w.readframes(w.getnframes()))
r1, a = pcm(sys.argv[1])
r2, b = pcm(sys.argv[2])
if r1 != 48000 or r2 != 48000:
    sys.exit('rates %d/%d, not 48000' % (r1, r2))
if abs(len(a) - len(b)) > 2 * 960:      # one 20 ms frame
    sys.exit('lengths %d/%d frames' % (len(a) // 2, len(b) // 2))
n = min(len(a), len(b))
sig = sum(x * x for x in b[:n]) / n
err = sum((x - y) ** 2 for x, y in zip(a[:n], b[:n])) / n
if math.sqrt(sig) < 1000:
    sys.exit('the reference is near silence (rms %.0f)' % math.sqrt(sig))
snr = 10 * math.log10(sig / err) if err else 99.0
print('     decode vs ffmpeg: %d frames, SNR %.1f dB, reference rms %.0f'
      % (n // 2, snr, math.sqrt(sig)))
if snr < 40:
    sys.exit('SNR %.1f dB < 40' % snr)
EOF
then
    echo "FAIL: opusstart: the decode is not the file's audio"; fail=1
fi

[ $fail -eq 0 ] || exit 1
echo "ok   Opus start: the header scans read buffered blocks, not one call per byte (tests/opusstart)"
