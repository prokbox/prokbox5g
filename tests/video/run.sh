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

# MP4/M4V demux host tests (#60, #64; docs/research/bcm2722/player.md):
#   - tests/video/mp4demux_test.py: the Python reference against ffprobe
#     and ffmpeg,
#   - tests/video/mp4demux_c_test.py: the C demuxer and sample index
#     (apps/video/) against the reference, byte for byte,
#   - tests/video/test_bufpeek.c: bufpeek_at() in apps/buffering.c,
#   - tests/video/test_vplay.c: the player engine (apps/video/vplay.c),
#   - tests/video/test_vpcore.c: the player's core and the VP client
#     (apps/video/vpcore.c, bcmvp.c) against a model of our image.
# Builds the H.264 corpus first if it is missing (make_corpus.sh, ~2 s).
# Without ffmpeg or ffprobe it is skipped with a note.
# Compiler: the newest Homebrew gcc-NN, else cc; override with HOSTCC=...
set -e
cd "$(dirname "$0")/../.."
# Phase 14a V2/V3: the video classifier, the .vix pass (test_vixpass) and
# the MP4 parser's video fields
VLOG=$(mktemp)
vrc=0
tests/video/vclass_test.sh >"$VLOG" 2>&1 || vrc=$?
grep -v '^ok  ' "$VLOG" || true
rm -f "$VLOG"
[ $vrc -eq 0 ] || { echo "FAIL: tests/video/vclass_test.sh"; exit 1; }
for t in ffmpeg ffprobe; do
    if ! command -v "$t" >/dev/null 2>&1; then
        echo "SKIP mp4demux: $t not installed"
        exit 0
    fi
done
python3 -I tests/video/mp4demux_test.py

if [ -z "${HOSTCC:-}" ]; then
    HOSTCC=cc
    for v in 19 18 17 16 15 14; do
        if command -v gcc-$v >/dev/null 2>&1; then HOSTCC=gcc-$v; break; fi
    done
fi
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
# shellcheck disable=SC2086
if ! "$HOSTCC" -std=gnu99 -O1 -g -Wall -Wextra -Werror ${EXTRA_CFLAGS:-} \
        -Itests/video/shim -Iapps -Iapps/video -o "$TMP/test_mp4demux" \
        tests/video/test_mp4demux.c apps/video/mp4demux.c apps/video/vix.c \
        apps/video/vix_file.c 2>"$TMP/cc.log"; then
    echo "FAIL: compiling test_mp4demux ($HOSTCC)"
    cat "$TMP/cc.log"
    exit 1
fi
"$TMP/test_mp4demux" --selftest
# bufpeek_at() in the real apps/buffering.c, built against the stand-ins
# in tests/video/bufshim: copies of buffering.c/.h, so their quoted
# includes find the stand-ins before the real apps/ headers
cp apps/buffering.c apps/buffering.h "$TMP/"
# shellcheck disable=SC2086
if ! "$HOSTCC" -std=gnu99 -O1 -g -Wall -Wextra -Werror -Wno-unused-function \
        -Wno-unused-variable ${EXTRA_CFLAGS:-} -I"$TMP" -Itests/video/bufshim \
        -o "$TMP/test_bufpeek" tests/video/test_bufpeek.c \
        firmware/common/linked_list.c 2>"$TMP/cc.log"; then
    echo "FAIL: compiling test_bufpeek ($HOSTCC)"
    cat "$TMP/cc.log"
    exit 1
fi
"$TMP/test_bufpeek" "$TMP/bufpeek.bin"

# the video clock gate (vclock_gate.h): the release's 150 MHz by token
# shellcheck disable=SC2086
if ! "$HOSTCC" -std=gnu99 -O1 -g -Wall -Wextra -Werror ${EXTRA_CFLAGS:-} \
        -Iapps -Iapps/video -o "$TMP/test_vclock" tests/video/test_vclock.c \
        2>"$TMP/cc.log"; then
    echo "FAIL: compiling test_vclock ($HOSTCC)"
    cat "$TMP/cc.log"
    exit 1
fi
"$TMP/test_vclock" >"$TMP/vclock.log" || { cat "$TMP/vclock.log"; exit 1; }
grep -E '^(FAIL|info|test_vclock)' "$TMP/vclock.log"

# the first-use video check (vcheck.h): the token and its store, the
# ladder, the stream and its sums against our own decoder, the check
# against the model with its faults, and its time
H264=bcmfw/h264
if ! "$HOSTCC" -std=gnu99 -O1 -g -Wall -Wextra -Werror -U_FORTIFY_SOURCE \
        -D_FORTIFY_SOURCE=0 ${EXTRA_CFLAGS:-} -DVCHECK_HOST -DSAFEFILE_HOST \
        -Iapps -Iapps/video -I$H264 -o "$TMP/test_vcheck" \
        tests/video/test_vcheck.c apps/video/vcheck.c \
        apps/video/vcheck_store.c apps/ipodui/safefile.c \
        apps/video/vbcm_model.c apps/video/bcmvp.c apps/video/vclk.c \
        $H264/h264.c $H264/ps.c $H264/slice.c $H264/mb.c $H264/cavlc.c \
        $H264/mvpred.c $H264/intra.c $H264/idct.c $H264/mc.c $H264/inter.c \
        $H264/deblock.c $H264/tables.c $H264/vlc_tables.c \
        2>"$TMP/cc.log"; then
    echo "FAIL: compiling test_vcheck ($HOSTCC)"
    cat "$TMP/cc.log"
    exit 1
fi
mkdir -p "$TMP/vcheck"
"$TMP/test_vcheck" bcmfw/video/vcheck.bin "$TMP/vcheck" >"$TMP/vcheck.log" \
    || { cat "$TMP/vcheck.log"; exit 1; }
grep -E '^(FAIL|info|test_vcheck)' "$TMP/vcheck.log"

# the pace row's arithmetic (vtrace.c: devbench vfeed)
# shellcheck disable=SC2086
if ! "$HOSTCC" -std=gnu99 -O1 -g -Wall -Wextra -Werror ${EXTRA_CFLAGS:-} \
        -Iapps -Iapps/video -o "$TMP/test_vpace" tests/video/test_vpace.c \
        apps/video/vtrace.c 2>"$TMP/cc.log"; then
    echo "FAIL: compiling test_vpace ($HOSTCC)"
    cat "$TMP/cc.log"
    exit 1
fi
"$TMP/test_vpace" >"$TMP/vpace.log" || { cat "$TMP/vpace.log"; exit 1; }
grep -E '^(FAIL|test_vpace)' "$TMP/vpace.log"

# the player engine against a model BCM and audio clock, the last case on
# a corpus clip's own index
# shellcheck disable=SC2086
if ! "$HOSTCC" -std=gnu99 -O1 -g -Wall -Wextra -Werror ${EXTRA_CFLAGS:-} \
        -Iapps -Iapps/video -o "$TMP/test_vplay" tests/video/test_vplay.c \
        apps/video/vplay.c apps/video/vix.c apps/video/mp4demux.c apps/video/vtrace.c \
        2>"$TMP/cc.log"; then
    echo "FAIL: compiling test_vplay ($HOSTCC)"
    cat "$TMP/cc.log"
    exit 1
fi
"$TMP/test_mp4demux" build/video/mp4/d_ipod_768k.mp4 --index "$TMP/clip.vix" \
    >/dev/null
# stage 6's film clip at silicon's measured pace (sitting 2)
"$TMP/test_mp4demux" bcmfw/video/stage6-real.mp4 --index "$TMP/film.vix" >/dev/null
"$TMP/test_vplay" "$TMP/clip.vix" "$TMP/film.vix"

# the player's core (vpcore.c) with the VP client (bcmvp.c) against the
# model of our image (vbcm_model.c): scenarios on a 12 s clip (the iPod
# clip six times over, with a sine soundtrack), then the corpus files
# shellcheck disable=SC2086
if ! "$HOSTCC" -std=gnu99 -O1 -g -Wall -Wextra -Werror ${EXTRA_CFLAGS:-} \
        -Iapps -Iapps/video -o "$TMP/test_vpcore" tests/video/test_vpcore.c \
        apps/video/vpcore.c apps/video/bcmvp.c apps/video/vbcm_model.c \
        apps/video/vplay.c apps/video/vix.c apps/video/mp4demux.c apps/video/vtrace.c \
        apps/video/vclk.c 2>"$TMP/cc.log"; then
    echo "FAIL: compiling test_vpcore ($HOSTCC)"
    cat "$TMP/cc.log"
    exit 1
fi
LONG=build/video/mp4/long_ipod.mp4
if [ ! -f "$LONG" ] || [ "$LONG" -ot build/video/corpus/d_ipod_768k.264 ]; then
    for i in 1 2 3 4 5 6; do cat build/video/corpus/d_ipod_768k.264; done \
        >"$TMP/long.264"
    ffmpeg -v error -y -framerate 30 -i "$TMP/long.264" -f lavfi -t 12 \
        -i sine=frequency=440:sample_rate=44100 -map 0:v -map 1:a -c:v copy \
        -c:a aac -b:a 128k "$LONG"
fi
"$TMP/test_vpcore" "$LONG" build/video/mp4/b_ippp_gop10.mp4 \
    build/video/mp4/e_vga.mp4 build/video/mp4/f_slices.m4v \
    build/video/mp4/d_ipod_768k_co64.mp4 >"$TMP/vpcore.log" 2>&1 || {
    cat "$TMP/vpcore.log"; exit 1; }
grep -v '^ok  ' "$TMP/vpcore.log" | grep -v '^     ' || true

# the ok lines run to hundreds: print only the failures and the total
rc=0
python3 -I tests/video/mp4demux_c_test.py "$TMP/test_mp4demux" \
    >"$TMP/c.log" 2>&1 || rc=$?
grep -v '^ok  ' "$TMP/c.log" || true
exit $rc
