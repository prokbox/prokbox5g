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

# Host test of mpa.c's synthesis switch (P19 19-1c iii, review M1): the
# inline synthesis the shell runs by default (decoding pinned to the CPU)
# against upstream's single-core path and the second-core thread, PCM
# hashed per run. Needs ffmpeg for the test MP3 (skipped without it).
# Compiler: the newest Homebrew gcc-NN, else cc; override with HOSTCC=...
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
T=tests/mpainline
L=lib/rbcodec/codecs/libmad

if [ -z "${HOSTCC:-}" ]; then
    HOSTCC=cc
    for v in 19 18 17 16 15 14; do
        if command -v gcc-$v >/dev/null 2>&1; then HOSTCC=gcc-$v; break; fi
    done
fi
if ! command -v ffmpeg >/dev/null 2>&1; then
    echo "skip mp3 synthesis switch (no ffmpeg for the test MP3)"
    exit 0
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
# 4 s, 44.1 kHz stereo, CBR 128 kb/s, no tags: a tone over pink noise
ffmpeg -nostdin -v error -y -f lavfi -i "sine=frequency=440:sample_rate=44100,aformat=channel_layouts=stereo[a];anoisesrc=color=pink:amplitude=0.05:sample_rate=44100:seed=7,aformat=channel_layouts=stereo[n];[a][n]amix=inputs=2:normalize=0" \
    -t 4 -c:a libmp3lame -b:a 128k -id3v2_version 0 -write_xing 0 \
    "$TMP/t.mp3" || { echo "FAIL: making the test MP3"; exit 1; }

# The harness itself is warning-free; mpa.c and libmad build as upstream
# wrote them (their sign warnings are not ours to fix here)
for c in 2 1; do
    # shellcheck disable=SC2086
    if ! "$HOSTCC" -std=gnu99 -O1 -g -Wall -Wextra -Werror -fsyntax-only \
            -DSTUB_CORES=$c -I$T/stub -Ilib/rbcodec -Ilib/rbcodec/codecs \
            "$T/test_mpainline.c" 2>"$TMP/cc.log" \
       || ! "$HOSTCC" -std=gnu99 -O1 -g -w ${EXTRA_CFLAGS:-} \
            -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -DSTUB_CORES=$c \
            -DNDEBUG -DHAVE_LIMITS_H -DHAVE_ASSERT_H \
            -I$T/stub -Ilib/rbcodec -Ilib/rbcodec/codecs -I$L \
            -o "$TMP/mpa$c" "$T/test_mpainline.c" lib/rbcodec/codecs/mpa.c \
            $L/bit.c $L/frame.c $L/huffman.c $L/layer12.c $L/layer3.c \
            $L/stream.c $L/synth.c -lpthread 2>>"$TMP/cc.log"; then
        echo "FAIL: compiling test_mpainline (cores=$c, $HOSTCC)"
        cat "$TMP/cc.log"
        exit 1
    fi
done

run() { "$TMP/mpa$1" "$TMP/t.mp3" "$2" ${3:-}; }
fail=0
want() { # label, got, expected
    if [ "$2" != "$3" ]; then
        echo "  $1: got '$2', want '$3'"
        fail=1
    fi
}
plain=$(run 1 0 | cut -d' ' -f1 | cut -d: -f2-)
inl=$(run 2 0 | cut -d' ' -f1)
thr=$(run 2 1 | cut -d' ' -f1)
want "inline = plain" "${inl#I:}" "$plain"
want "pair threaded,inline" "$(run 2 10 | cut -d' ' -f1-2)" "$thr I:$plain"
want "pair inline,threaded" "$(run 2 01 | cut -d' ' -f1-2)" "I:$plain $thr"
want "pair inline,inline" "$(run 2 00 | cut -d' ' -f1-2)" "I:$plain I:$plain"
pseek=$(run 1 0 1500 | cut -d' ' -f1 | cut -d: -f2-)
want "seek inline = plain" "$(run 2 0 1500 | cut -d' ' -f1)" "I:$pseek"
# Upstream's thread reads synth.pcm.length before the thread writes it,
# so each run start (and each seek) loses one 1152-sample frame there;
# the inline path, like the single-core one, keeps it
n_plain=${plain#*:}
n_thr=${thr##*:}
want "threaded frame" "$((n_plain - n_thr))" 1152
want "the thread was made" "$(run 2 1 | cut -d' ' -f2)" "threads=1"
if [ $fail = 0 ]; then
    echo "ok   mp3 synthesis switch (mpa.c inline = single-core path bit-exact: one run, a seek, gapless pairs both ways with the thread; ${n_plain} samples)"
else
    echo "FAIL: mp3 synthesis switch"
    exit 1
fi
