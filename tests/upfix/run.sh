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

# tests/upfix: the upstream fixes taken in on fix-upstream-batch, through
# the real parsers and codecs (build/warble, made by scripts/test.sh).
# Fixtures: crafted at run time by gen.py from committed ones
# (tests/fixtures/upfix/base.m4a: ffmpeg 9.0.2 AAC, 1 s 440 Hz mono 44.1 kHz,
# ffprobe 9.0.2: aac, 44100 Hz, 1.000000 s, 45 frames; the chapter fixtures as
# tests/audiobook uses them).
#
# Each case line: kind|file|arg|arg
#   dec      FILE REF          decodes (-r) to the same bytes as REF
#   meta     FILE LINE         -m prints LINE (exactly)
#   chaps    FILE REF          -m's chapter lines equal REF's
#   seek     FILE MS REF REFMS seeked decodes equal (bytes)
#   seekn    FILE MS BYTES     the decode seeked to MS gives BYTES bytes
#   decn     FILE BYTES        the decode gives BYTES bytes
#   quick    FILE              -m finishes inside 10 s
#   ok       FILE              -m and the decode end without a signal or
#                              a timeout (a refusal is fine)
#   len      FILE FIELD N      -m's FIELD value is N bytes long
#   reject   FILE              -m refuses it (exit 1, as ffprobe does) and
#                              the decode ends without a signal
#   refuse   FILE [TEXT]       the codec refuses it ("codec error"),
#                              no signal, its log saying TEXT (where it
#                              refused: the host's divide by zero gives
#                              0 where the PP5022's traps and restarts)
# long.m4a: base.m4a's 45 frames each stretched to 2^27 samples (50.7 min)
# by its stts, about 38 h in all, so the track passes 2^32 samples (27 h);
# frame 30 starts at 91304577 ms (27.9 h), frame 20 at 60869718 ms. A
# seek just inside one decodes the frames from it on: 15 (or 25) x 1024
# samples x 8 bytes (-r: 2 channels x 32 bits).
#
#   tests/upfix/run.sh
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT" || exit 2
W=$(ls build/warble/warble.* 2>/dev/null | head -1)
[ -x "$W" ] || { echo "FAIL: no build/warble/warble.* (run scripts/test.sh)" >&2; exit 2; }
OUT=build/upfix
rm -rf "$OUT"
python3 -I tests/upfix/gen.py tests/fixtures "$OUT" >/dev/null || {
    echo "FAIL: gen.py" >&2; exit 2; }
F=tests/fixtures
# #281 with upstream a9319e71c4's resync: a FLAC with no frame that decodes
# (a header, then 64 KiB of zeros) is a codec error, not a track that ended
python3 -I tests/fixtures/unplayable/make_unplayable.py "$OUT/unplayable" >/dev/null || {
    echo "FAIL: make_unplayable.py" >&2; exit 2; }
# Pictures (gen.py newpos prints the three offsets below): picback.flac
# (back cover, then front: upstream cb36cae2bc keeps the front, at 220),
# picpng.flac (PNG front, then JPEG front: the JPEG, at 173), picneg.flac
# (a PICTURE refused for its negative MIME length: upstream 5d3b008151
# skips it and the file still parses and plays; before, the whole file
# failed to parse and never played), apicunsync.mp3 (an
# ID3v2.3 tag unsynchronised as a whole, UTF-16 description: upstream
# 619144481a puts the picture at its first byte, 68)
TO="timeout 10"
WARBLE_ART=1        # -m prints the embedded picture's place and size
export WARBLE_ART

cat >"$OUT/cases" <<EOF
meta|$OUT/m4a64.m4a|Title: Upfix
meta|$OUT/m4a64.m4a|File size without headers: 6267
chaps|$OUT/apple64.m4b|$F/apple_chapters.m4b
chaps|$OUT/nero64.m4b|$F/nero_chapters.m4b
meta|$OUT/clamp.m4a|Frequency: 44100 Hz
meta|$OUT/clamp.m4a|Title: Upfix
dec|$OUT/m4a64.m4a|$F/upfix/base.m4a
dec|$OUT/apple64.m4b|$F/apple_chapters.m4b
dec|$OUT/gap.m4a|$F/upfix/base.m4a
dec|$OUT/co64.m4a|$F/upfix/base.m4a
seek|$OUT/co64.m4a|500|$F/upfix/base.m4a|500
seekn|$OUT/long.m4a|91305577|122880
seekn|$OUT/long.m4a|60870718|204800
meta|$OUT/picok.flac|Album art: type 3 at 88, 86 bytes
ok|$OUT/picneg.flac
ok|$OUT/infoneg.wav
meta|$OUT/infoneg.wav|Title: Wave Title
ok|$OUT/infolong.wav
meta|$OUT/infolong.wav|Title: Wave Title
len|$OUT/infolong.wav|Comment|1786
meta|$OUT/id3first.aiff|Title: Aiff Id3 Title
meta|$OUT/textlong.aiff|Artist: Aiff Author
len|$OUT/textlong.aiff|Title|1500
len|$OUT/textlong.aiff|Comment|286
meta|$OUT/cjk.vgm|Album: Game
meta|$OUT/cjk.vgm|Artist: Author
meta|$OUT/cjk.vgm|Title: 音楽音楽音楽音楽音楽音楽音
meta|$OUT/apic16.mp3|Album art: type 3 at 67, 86 bytes
meta|$OUT/apic16e.mp3|Album art: type 3 at 57, 86 bytes
quick|$OUT/sttsbig.m4a
meta|$OUT/sttsbig.m4a|Number of samples: 45124
ok|$OUT/sttsbig.m4a
refuse|$OUT/pcm4.wav|bitspersample 4 < 8
ok|$OUT/pcmseek.vgm
reject|$OUT/sirate0.flac
reject|$OUT/sibig.flac
dec|$OUT/varblock.flac|$F/f1/sort.flac
seek|$OUT/varblock.flac|1000|$F/f1/sort.flac|1000
seek|$OUT/varblock.flac|1700|$F/f1/sort.flac|1700
decn|$OUT/badframe.flac|59904
refuse|$OUT/unplayable/bad1.flac|codec error
meta|$OUT/picback.flac|Album art: type 3 at 220, 86 bytes
meta|$OUT/picpng.flac|Album art: type 3 at 173, 86 bytes
meta|$OUT/picneg.flac|Frequency: 8000 Hz
dec|$OUT/picneg.flac|$F/f1/sort.flac
meta|$OUT/apicunsync.mp3|Album art: type 3 at 68, 86 bytes
EOF
N_WANT=46

fail=0
n=0
raw() { $TO "$W" -r ${2:+-c "seek=$2"} "$1" "$3" >/dev/null 2>&1; }
while IFS='|' read -r kind f a b c; do
    n=$((n + 1))
    k=$OUT/case$n
    case $kind in
    dec)
        raw "$f" "" "$k.out" && raw "$a" "" "$k.ref" && [ -s "$k.ref" ] \
            && cmp -s "$k.out" "$k.ref" \
            || { echo "FAIL dec $f: not $a's $(wc -c <"$k.ref" 2>/dev/null) bytes ($(wc -c <"$k.out" 2>/dev/null))" >&2; fail=1; }
        ;;
    meta)
        $TO "$W" -m "$f" >"$k.m" 2>/dev/null
        grep -qxF "$a" "$k.m" || {
            echo "FAIL meta $f: want '$a'; got: $(grep "^${a%%: *}: " "$k.m" || echo none)" >&2; fail=1; }
        ;;
    chaps)
        $TO "$W" -m "$f" 2>/dev/null | grep '^Chapter' >"$k.m"
        $TO "$W" -m "$a" 2>/dev/null | grep '^Chapter' >"$k.r"
        [ "$(grep -c '^Chapter [0-9]' "$k.r")" -gt 1 ] && cmp -s "$k.m" "$k.r" || {
            echo "FAIL chaps $f: $(grep -c '^Chapter [0-9]' "$k.m") chapters, want $a's $(grep -c '^Chapter [0-9]' "$k.r")" >&2; fail=1; }
        ;;
    seek)
        raw "$f" "$a" "$k.out" && raw "$b" "$c" "$k.ref" && [ -s "$k.ref" ] \
            && cmp -s "$k.out" "$k.ref" \
            || { echo "FAIL seek $f at $a ms: $(wc -c <"$k.out" 2>/dev/null) bytes, want $b at $c ms: $(wc -c <"$k.ref" 2>/dev/null)" >&2; fail=1; }
        ;;
    decn)
        raw "$f" "" "$k.out"
        [ "$(wc -c <"$k.out" 2>/dev/null | tr -d ' ')" = "$a" ] || {
            echo "FAIL decn $f: $(wc -c <"$k.out" 2>/dev/null | tr -d ' ') bytes, want $a" >&2; fail=1; }
        ;;
    seekn)
        raw "$f" "$a" "$k.out"
        [ "$(wc -c <"$k.out" 2>/dev/null | tr -d ' ')" = "$b" ] || {
            echo "FAIL seekn $f at $a ms: $(wc -c <"$k.out" 2>/dev/null | tr -d ' ') bytes, want $b" >&2; fail=1; }
        ;;
    ok)
        $TO "$W" -m "$f" >/dev/null 2>&1; rc=$?
        $TO "$W" -r "$f" "$k.out" >/dev/null 2>&1; rc2=$?
        [ "$rc" -lt 124 ] && [ "$rc2" -lt 124 ] || {
            echo "FAIL ok $f: -m exit $rc, decode exit $rc2 (124 timeout, 128+ a signal)" >&2; fail=1; }
        ;;
    len)
        $TO "$W" -m "$f" >"$k.m" 2>/dev/null
        got=$(grep "^$a: " "$k.m" | head -1 | sed "s/^$a: //" | tr -d '\n' | wc -c | tr -d ' ')
        [ "$got" = "$b" ] || { echo "FAIL len $f: $a is $got bytes, want $b" >&2; fail=1; }
        ;;
    reject)
        $TO "$W" -m "$f" >"$k.m" 2>&1; rc=$?
        $TO "$W" -r "$f" "$k.out" >/dev/null 2>&1; rc2=$?
        [ "$rc" = 1 ] && [ "$rc2" -lt 124 ] || {
            echo "FAIL reject $f: -m exit $rc (want 1: $(grep -E '^Frequency' "$k.m")), decode exit $rc2" >&2; fail=1; }
        ;;
    refuse)
        $TO "$W" -r "$f" "$k.out" >/dev/null 2>"$k.err"; rc=$?
        [ "$rc" -lt 124 ] && grep -q 'codec error' "$k.err" \
            && grep -qF "$a" "$k.err" || {
            echo "FAIL refuse $f: exit $rc; want 'codec error' and '$a', got: $(grep -i error "$k.err" | tr '\n' ' ')" >&2; fail=1; }
        ;;
    quick)
        $TO "$W" -m "$f" >/dev/null 2>&1; rc=$?
        [ "$rc" != 124 ] || { echo "FAIL quick $f: -m still running after 10 s" >&2; fail=1; }
        ;;
    *) echo "FAIL: unknown case kind '$kind'" >&2; fail=1 ;;
    esac
done <"$OUT/cases"
[ "$n" -eq "$N_WANT" ] || { echo "FAIL: checked $n of $N_WANT cases" >&2; exit 1; }
[ "$fail" = 0 ] || exit 1
echo "ok   upfix: $n cases"
