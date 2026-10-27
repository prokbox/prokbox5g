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

# Chapter-parsing golden tests.
#
# Runs warble's metadata-only mode (-m) on each fixture, keeps the
# audiobook/chapter section of its output and diffs it against
# tests/audiobook/golden/<fixture>.txt. Exits non-zero on any mismatch.
#
# Needs a built warble (build/warble, see scripts/test.sh); override the
# binary with WARBLE=/path/to/warble.<model>.
#
# Golden files were checked against `ffprobe -show_chapters <fixture>`
# (titles are truncated to 63 bytes on a UTF-8 boundary, like the firmware).
# Only regenerate them on purpose: UPDATE_GOLDEN=1 tests/audiobook/run.sh
set -u

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"

if [ -z "${WARBLE:-}" ]; then
    for w in build/warble/warble.*; do
        [ -x "$w" ] && WARBLE=$w && break
    done
fi
if [ -z "${WARBLE:-}" ] || [ ! -x "$WARBLE" ]; then
    echo "warble not found; build it first (scripts/test.sh or make -C build/warble)" >&2
    exit 2
fi

GOLDEN=tests/audiobook/golden
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

fail=0
# The metadata-extras fixtures (tests/fixtures/make_meta_fixtures.py) also
# cover ID3 CHAP chapters, UTF-16 titles, lyrics and per-chapter art; their
# goldens were checked against ffprobe (-show_chapters, format tags, and
# -show_packets for the image track) and the generator's image CRCs.
# The chapter-capacity fixtures (tests/fixtures/make_chapcap_fixtures.py,
# #48) check that all 648 chapters are read (ffmpeg's 'chpl' cut at 255
# loses to its QuickTime track; an mp4v2 'chpl' with a 32-bit count; 300
# ID3 CHAP frames) and that 1100 stop cleanly at AB_MAX_CHAPTERS (1024);
# checked against ffprobe -show_chapters (the mp4v2 one against
# chapters_648.m4b, since ffmpeg reads only the low byte of its count).
# P6: chapters_300_v24.mp3 is chapters_300.mp3 as ffmpeg's ID3v2.4 (its
# CTOC size is not synchsafe, #50; same golden lines); chapters_400_long
# and chapters_450_utf8 check the packed pool's fair share (no cut when
# the text track replaces a 'chpl'; equal UTF-8-safe cuts past the pool).
# Their chapter lines were checked with tests/audiobook/ffprobe_chapters.py
# (ffprobe + the pool rules in Python, independent of warble).
FIXTURES="nero_chapters.m4b apple_chapters.m4b plain.mp3
          id3v24_chapters.mp3 id3v23_unsync.mp3 rich_chapters.m4b
          chapters_648.m4b chapters_648_mp4v2.m4b chapters_1100.m4b
          chapters_300.mp3 chapters_300_v24.mp3 chapters_400_long.m4b
          chapters_450_utf8.mp3"
for fixture in $FIXTURES; do
    name=${fixture%.*}
    out="$TMP/$name.txt"
    if ! "$WARBLE" -m "tests/fixtures/$fixture" >"$TMP/raw.txt" 2>"$TMP/err.txt"; then
        echo "FAIL $fixture: warble exited non-zero" >&2
        cat "$TMP/err.txt" >&2
        fail=1
        continue
    fi
    # the chapter section starts at "Audiobook:" and runs to the end
    sed -n '/^Audiobook:/,$p' "$TMP/raw.txt" >"$out"

    if [ "${UPDATE_GOLDEN:-0}" = 1 ]; then
        cp "$out" "$GOLDEN/$name.txt"
        echo "updated $GOLDEN/$name.txt"
    elif [ ! -f "$GOLDEN/$name.txt" ]; then
        echo "FAIL $fixture: no golden; output:" >&2
        cat "$out" >&2
        fail=1
    elif diff -u "$GOLDEN/$name.txt" "$out"; then
        echo "PASS $fixture"
    else
        echo "FAIL $fixture" >&2
        fail=1
    fi
done

# Now Playing's "N of M" and snap scrub (apps/ipodui/np_scrub.c) on the
# real 648-chapter parse, in a chapter table of the shell's size.
if [ -z "${HOSTCC:-}" ]; then
    HOSTCC=cc
    for v in 19 18 17 16 15 14; do
        if command -v gcc-$v >/dev/null 2>&1; then HOSTCC=gcc-$v; break; fi
    done
fi
if ! "$HOSTCC" -std=gnu99 -O1 -Wall -Wextra -Werror -DWARBLE \
        -Ilib/rbcodec/metadata -Iapps/ipodui -o "$TMP/scrub_real" \
        tests/audiobook/test_scrub_real.c apps/ipodui/np_scrub.c \
        lib/rbcodec/metadata/abpack.c \
        2>"$TMP/cc.log"; then
    echo "FAIL: compiling test_scrub_real ($HOSTCC)" >&2
    cat "$TMP/cc.log" >&2
    fail=1
elif "$WARBLE" -m tests/fixtures/chapters_648.m4b 2>/dev/null \
        | "$TMP/scrub_real" 648 73 "73 of 648"; then
    echo "PASS np_scrub on chapters_648.m4b"
else
    echo "FAIL np_scrub on chapters_648.m4b" >&2
    fail=1
fi

exit $fail
