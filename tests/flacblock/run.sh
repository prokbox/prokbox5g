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

# #281: FLAC streams with large blocks. Rockbox's decoder refused any
# stream whose STREAMINFO maximum block size was over 4608 samples (8192
# on the host), with no message: the track skipped in ~25 ms. ffmpeg writes
# 16384-sample blocks at 176.4/192 kHz. lib/rbcodec/codecs/flac.c now takes
# every block size the format allows (up to 65535), the buffers sized per
# stream from the codec buffer.
#
# For 16/44.1, 24/96 and 24/192 stereo, with blocks of 4096, 16384 and
# 65535 samples (ffmpeg -frame_size), warble's raw codec output must equal
# the encoder's input sample for sample, and the fixture must really have
# that block size (STREAMINFO). A reference decode (flac -d when installed,
# else ffmpeg's decoder) must give the input back too; ffmpeg 9 decodes no
# 65535-sample frame at all, so without flac those three get no reference
# (noted), only the input.
#
# Device memory: WARBLE_CODEC_BUF gives the codec only what the device's
# codec slot leaves free with the COP's stack reserved: 1,048,576 B
# (CODEC_SIZE) - 5,856 B (flac.codec's image, fw-ship at #281) - 12,288 B
# (COP_CODEC_STACK) = 1,030,432 B. The 24/192, 65535-sample stream must
# decode in it with the frame-size fields of STREAMINFO zeroed (the worst
# case: the frame copy sized for verbatim frames), and must be refused,
# with "flac rejected blocksize=65535 why=memory", with 1 KiB less than it
# needs.
#
# Needs ffmpeg (the encoder) and a built warble (scripts/test.sh).
set -e
cd "$(dirname "$0")/../.."

WARBLE=${WARBLE:-}
if [ -z "$WARBLE" ]; then
    for w in build/warble/warble.*; do
        [ -x "$w" ] && WARBLE=$w && break
    done
fi
[ -n "$WARBLE" ] && [ -x "$WARBLE" ] || {
    echo "warble not found; build it first (scripts/test.sh)" >&2; exit 1; }
command -v ffmpeg >/dev/null 2>&1 || {
    echo "flacblock: FAILED: ffmpeg not found (the fixtures' encoder)" >&2
    exit 1; }

DEVICE_FREE=1030432
T=build/flacblock
rm -rf "$T"
mkdir -p "$T"
FB="python3 tests/flacblock/fb.py"
fail=0
checked=0
if command -v flac >/dev/null 2>&1; then REF=flac; else REF=ffmpeg; fi
echo "flacblock: reference decoder: $REF"

ok()  { echo "ok   $*"; }
bad() { echo "FAIL $*"; fail=1; }

for fmt in 16:44100 24:96000 24:192000; do
    bits=${fmt%%:*}
    rate=${fmt#*:}
    src=$T/src_${bits}_$rate.raw
    # 3 blocks of 65535 and a short last one
    $FB gen "$src" "$bits" 200000
    [ "$bits" = 16 ] && pcm=s16le || pcm=s24le
    for bs in 4096 16384 65535; do
        name=${bits}_${rate}_$bs
        f=$T/$name.flac
        ffmpeg -nostdin -v error -y -f $pcm -ar "$rate" -ac 2 -i "$src" \
            -c:a flac -frame_size "$bs" "$f"
        info=$($FB info "$f")
        case "$info" in
            "bs=$bs bps=$bits rate=$rate ch=2 "*) ;;
            *) bad "$name: the fixture is not what it should be: $info"
               continue ;;
        esac
        if "$WARBLE" -r "$f" "$T/$name.out" 2>"$T/$name.log" \
                && r=$($FB same "$src" "$bits" "$T/$name.out"); then
            ok "$name: $r ($info)"
        else
            bad "$name: $(cat "$T/$name.log" | tr '\n' ' ') ${r:-}"
        fi
        if [ $REF = flac ]; then
            flac -s -d -f --force-raw-format --endian=little --sign=signed \
                -o "$T/$name.ref" "$f"
        else
            ffmpeg -nostdin -v error -y -i "$f" -f $pcm "$T/$name.ref"
        fi
        if r=$($FB samepcm "$src" "$T/$name.ref"); then
            ok "$name reference: $r"
        elif [ $REF = ffmpeg ] && [ "$bs" = 65535 ] && [ ! -s "$T/$name.ref" ]; then
            # ffmpeg 9's FLAC parser finds no frame this large (it decodes
            # nothing): no reference here; libFLAC's flac -d reads them
            echo "note $name reference: not checked (ffmpeg reads no 65535-sample frame; install flac)"
        else
            bad "$name reference: $r"
        fi
        checked=$((checked + 1))
    done
done

# a seek before the first frame (as a resume does) lands on the exact
# sample inside a large block: the output is the input from there on
for name in 16_44100_65535 24_192000_16384 24_192000_65535; do
    bits=${name%%_*}
    rate=${name#*_}; rate=${rate%%_*}
    if "$WARBLE" -r -c seek=700 "$T/$name.flac" "$T/$name.seek" 2>"$T/$name.seeklog" \
            && r=$($FB same "$T/src_${bits}_$rate.raw" "$bits" "$T/$name.seek" \
                   $((700 * rate / 1000))); then
        ok "$name seek 700 ms: $r"
    else
        bad "$name seek 700 ms: $(cat "$T/$name.seeklog" | tr '\n' ' ') ${r:-}"
    fi
done

# the device's codec memory: the worst case fits, and 1 KiB less refuses
f=$T/24_192000_65535.flac
z=$T/24_192000_65535_noframes.flac
$FB zeroframes "$f" "$z"
need=$($FB need "$z")
echo "     24/192, 65535-sample blocks, no frame sizes: needs $need B of $DEVICE_FREE"
if WARBLE_CODEC_BUF=$DEVICE_FREE "$WARBLE" -r "$z" "$T/dev.out" 2>"$T/dev.log" \
        && r=$($FB same "$T/src_24_192000.raw" 24 "$T/dev.out"); then
    ok "device codec memory: decodes in $DEVICE_FREE B: $r"
else
    bad "device codec memory: $(cat "$T/dev.log" | tr '\n' ' ') ${r:-}"
fi
less=$((need - 1024))
WARBLE_CODEC_BUF=$less "$WARBLE" -r "$z" "$T/less.out" 2>"$T/less.log" || true
if grep -q "flac rejected blocksize=65535 why=memory" "$T/less.log" \
        && [ ! -s "$T/less.out" ]; then
    ok "1 KiB short ($less B): refused, logged"
else
    bad "1 KiB short ($less B): not refused as it should be: $(cat "$T/less.log" | tr '\n' ' ')"
fi

echo "flacblock: $checked streams checked"
[ "$checked" = 9 ] || bad "expected 9 streams, checked $checked"
[ $fail -eq 0 ] && echo "flacblock: all passed" || { echo "flacblock: FAILED"; exit 1; }
