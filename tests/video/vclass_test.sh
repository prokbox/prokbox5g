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

# Phase 14a V2 host tests (docs/phase14a-brief.md 3.2, A3), run by
# tests/video/run.sh:
#   - test_vclass: apps/ipodui/vclass_core.c, every 3.2 rule (no ffmpeg);
#   - test_vixpass: apps/ipodui/vixpass_core.c, the .vix pass's ledger and
#     its keep / reuse / orphan decisions (3.4; no ffmpeg);
#   - test_mp4meta: the MP4 parser's video fields on synthetic fixtures
#     (tests/video/mp4meta_fixtures.sh), built with warble's configuration
#     plus -DHAVE_BCMFW_VIDEO against the real lib/rbcodec/metadata sources;
#     needs build/warble (scripts/test.sh builds it) and ffmpeg/ffprobe,
#     else it is skipped with a note.
# Compiler: the newest Homebrew gcc-NN, else cc; override with HOSTCC=...
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
if [ -z "${HOSTCC:-}" ]; then
    HOSTCC=cc
    for v in 19 18 17 16 15 14; do
        if command -v gcc-$v >/dev/null 2>&1; then HOSTCC=gcc-$v; break; fi
    done
fi
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# --- test_vclass ---------------------------------------------------------
# shellcheck disable=SC2086
if ! "$HOSTCC" -std=gnu99 -O1 -g -Wall -Wextra -Werror ${EXTRA_CFLAGS:-} \
        -Iapps/ipodui -o "$TMP/test_vclass" tests/video/test_vclass.c \
        apps/ipodui/vclass_core.c 2>"$TMP/cc.log"; then
    echo "FAIL: compiling test_vclass ($HOSTCC)"
    cat "$TMP/cc.log"
    exit 1
fi
"$TMP/test_vclass"

# --- test_vixpass --------------------------------------------------------
# shellcheck disable=SC2086
if ! "$HOSTCC" -std=gnu99 -O1 -g -Wall -Wextra -Werror ${EXTRA_CFLAGS:-} \
        -Iapps/ipodui -o "$TMP/test_vixpass" tests/video/test_vixpass.c \
        apps/ipodui/vixpass_core.c 2>"$TMP/cc.log"; then
    echo "FAIL: compiling test_vixpass ($HOSTCC)"
    cat "$TMP/cc.log"
    exit 1
fi
"$TMP/test_vixpass"

# --- test_mp4meta --------------------------------------------------------
for t in ffmpeg ffprobe python3; do
    if ! command -v "$t" >/dev/null 2>&1; then
        echo "SKIP test_mp4meta: $t not installed"
        exit 0
    fi
done
W=$ROOT/build/warble
if [ ! -f "$W/lib/librbcodec.a" ]; then
    echo "SKIP test_mp4meta: build/warble not built (scripts/test.sh)"
    exit 0
fi
FX=$ROOT/build/video/meta
if [ ! -f "$FX/mpeg4.m4v" ] || [ "$FX/high.m4v" -ot tests/video/mp4meta_fixtures.sh ]; then
    sh tests/video/mp4meta_fixtures.sh "$FX"
fi

# warble's own compile line (lib/rbcodec/test/warble.make), plus the flag;
# every metadata source is rebuilt so struct mp3entry has one layout
INC="-I$W -Ilib/fixedpoint -Ilib/rbcodec/test -Iapps -Iapps/gui
     -Ifirmware/kernel/include -Ifirmware/export -Ifirmware/include
     -Ifirmware/target/hosted -Ifirmware/target/hosted/sdl -Ilib/tlsf/src
     -Ilib/rbcodec -Ilib/rbcodec/codecs -Ilib/rbcodec/dsp
     -Ilib/rbcodec/metadata"
DEF="-DROCKBOX -DMEMORYSIZE=64 -DIPOD_VIDEO -DTARGET_ID=15
     -DTARGET_NAME=\"ipodvideo\" -DDEBUG -DWARBLE -D__PCTOOL__ -D_FILE_H_
     -DHAVE_BCMFW_VIDEO"
SDLCF=$(sdl2-config --cflags 2>/dev/null || true)
SDLLD=$(sdl2-config --libs 2>/dev/null || true)
objs=
for f in lib/rbcodec/metadata/*.c tests/video/test_mp4meta.c; do
    o=$TMP/$(basename "$f" .c).o
    # shellcheck disable=SC2086
    if ! "$HOSTCC" -std=gnu99 -O1 -g -Wno-pointer-sign -Wno-override-init \
            -Wimplicit-fallthrough=0 $INC $DEF $SDLCF -c "$f" -o "$o" \
            2>"$TMP/cc.log"; then
        echo "FAIL: compiling $f for test_mp4meta ($HOSTCC)"
        cat "$TMP/cc.log"
        exit 1
    fi
    objs="$objs $o"
done
# shellcheck disable=SC2086
"$HOSTCC" -o "$TMP/test_mp4meta" $objs "$W"/firmware/common/*.o \
    "$W/lib/librbcodec.a" "$W/lib/libfixedpoint.a" "$W/lib/libtlsf.a" \
    $SDLLD -lm

# an audio duration in ms as ffprobe reports it (edit list applied), and
# the audio trak's sample count (its packet durations summed: the stts sum)
adur() {
    ffprobe -v error -select_streams a:0 -show_entries stream=duration \
        -of csv=p=0 "$1" | awk '{ printf "%d", $1 * 1000 + 0.5 }'
}
asamples() {
    ffprobe -v error -select_streams a:0 -show_entries packet=duration \
        -of csv=p=0 "$1" | awk '{ s += $1 } END { printf "%d", s }'
}
# one AAC frame, 1024 samples at 44.1 kHz, is 23.2 ms
FRAME=23

M=$TMP/test_mp4meta
rc=0
$M "$FX/movie.m4v" video=1 playable=1 podcast=1 stik=9 audiobook=0 chapters=3 \
    lead_trim=0 tvsh=- tvsn=0 tves=0 || rc=1
$M "$FX/late.m4v" video=1 stik=6 audiobook=0 chapters=2 lead_trim=0 || rc=1
$M "$FX/tv.m4v" video=1 podcast=0 stik=10 tvsh="Show A" tvsn=2 tves=3 \
    audiobook=0 chapters=0 || rc=1
for f in afirst.m4v tv.m4v audio.mp4; do
    $M "$FX/$f" samples="$(asamples "$FX/$f")" \
        length="$(adur "$FX/$f")~$FRAME" || rc=1
done
$M "$FX/afirst.m4v" video=1 tvsh="Show B" stik=0 || rc=1
$M "$FX/audio.mp4" video=0 audiobook=0 tvsh=- || rc=1
$M "$FX/high.m4v" video=1 playable=1 || rc=1
# MPEG-4 Part 2: leaves Music, but the H.264 player can't play it (B-M3)
$M "$FX/mpeg4.m4v" video=1 playable=0 audiobook=0 || rc=1
$M "$FX/audio.mp4" playable=0 || rc=1
# an m4b with a 'jpeg' chapter-image trak (hdlr 'vide') stays a book
$M tests/fixtures/rich_chapters.m4b video=0 audiobook=1 || rc=1
$M tests/fixtures/nero_chapters.m4b video=0 audiobook=1 || rc=1
# ... and so does one whose chapter images ffmpeg stored as 'mp4v' with a
# JPEG decoder (esds object type 0x6C): the gate's Image Book fixture
IMGBOOK="build/video-fixtures/tree/Music/Fixture Author/Image Book/Image Book.m4b"
if [ -f "$IMGBOOK" ]; then
    $M "$IMGBOOK" video=0 audiobook=1 || rc=1
    # Its esds patched: ES_ID 4 (a 4 before the DecoderConfig tag) is still
    # a still; objectType 0x20 (MPEG-4 video) is a video; a descriptor run
    # of 0xff bytes (no valid tags or lengths) is not a still and reads
    # nothing past the window; a box before the esds is walked past.
    python3 - "$IMGBOOK" "$TMP" <<'PYEOF' || rc=1
import sys
d = open(sys.argv[1], 'rb').read()
e = d.find(b'esds', d.find(b'mp4v'))
assert e > 0 and d[e + 8] == 3, 'no esds in the Image Book'
def put(name, at, data):
    b = bytearray(d); b[at:at + len(data)] = data
    open(sys.argv[2] + '/' + name, 'wb').write(b)
put('esid4.m4b', e + 13, b'\x00\x04')
put('obj20.m4b', e + 21, b'\x20')
put('junk.m4b', e + 9, b'\xff' * 35)
# a 'pasp' box before the esds (other muxers): every enclosing box grows
# by 16 and, the moov being before the mdat, so does every chunk offset
import struct
ins, pasp = e - 4, struct.pack('>I4sII', 16, b'pasp', 1, 1)
b = bytearray(d)
def fix(off, end):
    while off + 8 <= end:
        sz, t = struct.unpack('>I4s', d[off:off + 8])
        if sz < 8:
            return
        if t == b'stco':
            for i in range(struct.unpack('>I', d[off + 12:off + 16])[0]):
                o = off + 16 + 4 * i
                v = struct.unpack('>I', d[o:o + 4])[0]
                b[o:o + 4] = struct.pack('>I', v + 16)
        if off < ins < off + sz:
            b[off:off + 4] = struct.pack('>I', sz + 16)
            if t == b'stsd':
                fix(off + 16, off + sz)
        if t in (b'moov', b'trak', b'mdia', b'minf', b'stbl'):
            fix(off + 8, off + sz)
        off += sz
fix(0, len(d))
open(sys.argv[2] + '/pasp.m4b', 'wb').write(b[:ins] + pasp + b[ins:])
PYEOF
    $M "$TMP/esid4.m4b" video=0 audiobook=1 || rc=1
    $M "$TMP/obj20.m4b" video=1 audiobook=0 || rc=1
    $M "$TMP/junk.m4b" video=1 || rc=1
    $M "$TMP/pasp.m4b" video=0 audiobook=1 || rc=1
else
    echo "note mp4v chapter-image book: no $IMGBOOK (tests/fixtures/videos/mkvideos.sh)"
fi
$M --copy "$FX/tv.m4v" || rc=1
# Classification must also work in the regular shell, with NO video player
# enabled. Rebuild all sources for the changed mp3entry layout, not just MP4.
DEF_PLAIN="-DROCKBOX -DMEMORYSIZE=64 -DIPOD_VIDEO -DTARGET_ID=15
     -DTARGET_NAME=\"ipodvideo\" -DDEBUG -DWARBLE -D__PCTOOL__ -D_FILE_H_
     -DHAVE_IPOD_UI"
objs=
for f in lib/rbcodec/metadata/*.c tests/video/test_mp4meta.c firmware/common/nfc.c; do
    o=$TMP/plain_$(basename "$f" .c).o
    # shellcheck disable=SC2086
    if ! "$HOSTCC" -std=gnu99 -O1 -g -Wno-pointer-sign -Wno-override-init \
            -Wimplicit-fallthrough=0 $INC $DEF_PLAIN $SDLCF -c "$f" -o "$o" \
            2>"$TMP/cc.log"; then
        echo "FAIL: compiling plain shell video classification: $f"
        cat "$TMP/cc.log"
        exit 1
    fi
    objs="$objs $o"
done
# shellcheck disable=SC2086
"$HOSTCC" -o "$TMP/test_plain_mp4meta" $objs "$W"/firmware/common/*.o \
    "$W/lib/librbcodec.a" "$W/lib/libfixedpoint.a" "$W/lib/libtlsf.a" \
    $SDLLD -lm
M=$TMP/test_plain_mp4meta
$M "$FX/movie.m4v" video=1 audiobook=0 || rc=1
$M "$FX/afirst.m4v" video=1 audiobook=0 || rc=1
$M "$FX/audio.mp4" video=0 audiobook=0 || rc=1
$M "$FX/mpeg4.m4v" video=1 audiobook=0 || rc=1
$M tests/fixtures/rich_chapters.m4b video=0 audiobook=1 || rc=1
$M tests/fixtures/nero_chapters.m4b video=0 audiobook=1 || rc=1
exit $rc
