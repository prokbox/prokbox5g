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

# Resampler host tests (P19 19-13, docs/design/resample-plan.md):
#   1. the coefficient header equals a fresh tests/resample/hb_design.py run;
#   2. ARM half-band code == C reference, bit for bit, under qemu
#      (tests/resample/arm.sh; skipped with a note without the tools), plus
#      its instruction and cycle counts;
#   3. every converted ratio through warble's real DSP: multitone in,
#      passband flatness and alias/image rejection out (measure.py), with
#      today's Hermite (rsq=0) printed beside it for comparison;
#   4. mono, and the time-stretch chain in front of the resampler;
#   5. golden output hashes (tests/resample/golden.txt). After a deliberate
#      change, regenerate with: tests/resample/run.sh --update-golden
# Needs build/warble (scripts/test.sh) and python3 with numpy.
set -u
cd "$(dirname "$0")/../.."
WB=""
for w in build/warble/warble.*; do [ -x "$w" ] && WB=$w && break; done
[ -n "$WB" ] || { echo "warble not found; build it first (scripts/test.sh)"; exit 1; }
python3 -c "import numpy" 2>/dev/null || { echo "note: numpy missing; resample measurements skipped"; exit 0; }
T=build/resample
rm -rf "$T"; mkdir -p "$T"
M="python3 tests/resample/measure.py"
fail=0
UPDATE=0; [ "${1:-}" = --update-golden ] && UPDATE=1

# 1. coefficient tables
python3 tests/resample/hb_design.py >"$T/coefs.txt"
sed -n '/^\/\* tight, 44/,/^#endif/p' lib/rbcodec/dsp/resample_hb_coefs.h \
    | grep -v '^$' | grep -v '^#endif' >"$T/coefs_h.txt"
grep -v '^$' "$T/coefs.txt" >"$T/coefs_new.txt"
if cmp -s "$T/coefs_h.txt" "$T/coefs_new.txt"; then
    echo "ok   resample_hb_coefs.h equals a fresh hb_design.py run"
else
    echo "FAIL resample_hb_coefs.h differs from hb_design.py"; fail=1
fi

# 2. ARM == C
sh tests/resample/arm.sh || fail=1

# 2b. buffer boundaries: resample.c's worker, one block vs irregular blocks
I="-Ibuild/warble -Ilib/fixedpoint -Ilib/rbcodec/test -Iapps \
-Ifirmware/kernel/include -Ifirmware/export -Ifirmware/include \
-Ifirmware/target/hosted -Ifirmware/target/hosted/sdl -Ilib/rbcodec \
-Ilib/rbcodec/codecs -Ilib/rbcodec/dsp -Ilib/rbcodec/metadata"
# shellcheck disable=SC2086
if ${CC:-cc} $I -DROCKBOX -DMEMORYSIZE=64 -DIPOD_VIDEO -DTARGET_ID=15 \
       -DWARBLE -D__PCTOOL__ -std=gnu99 -O2 -Wall -Wno-pointer-sign \
       tests/resample/unit.c lib/rbcodec/dsp/resample_hb.c \
       -o "$T/unit" 2>"$T/unit.log" && "$T/unit" >"$T/unit.txt"; then
    echo "ok   block boundaries: $(grep -c '^ok' "$T/unit.txt") ratio/channel cases identical in one block and in 1..37-sample blocks"
else
    echo "FAIL block boundaries:"; cat "$T/unit.log" "$T/unit.txt" 2>/dev/null | grep -v '^ok' | head; fail=1
fi

# 3. ratios: name fin fout channels kind pass_db reject_db residual_db
#    tones [rsq]. residual_db 0 skips the glitch check: the Hermite step's
#    16.16 phase step makes x3/x6/3:2 run a few ppm off pitch (as upstream
#    does for every non-integer ratio), which a whole-file fit reads as
#    residual.
: >"$T/hash.txt"
case_() {
    name=$1 fin=$2 fout=$3 ch=$4 kind=$5 pdb=$6 rdb=$7 res=$8 tones=$9
    q=${10:-1}
    $M gen "$T/$name.wav" "$fin" "$ch" 1.5 "$tones"
    "$WB" -c "outrate=$fout:rsq=$q" "$T/$name.wav" "$T/$name.out.wav" 2>>"$T/warble.log"
    "$WB" -c "outrate=$fout:rsq=0" "$T/$name.wav" "$T/$name.h.wav" 2>>"$T/warble.log"
    if r=$($M check "$fin" "$T/$name.out.wav" "$tones" "$kind" "$pdb" "$rdb" "$res"); then
        echo "ok   $name: $r"
    else
        echo "FAIL $name: $r"; fail=1
    fi
    echo "     (Hermite: $($M check "$fin" "$T/$name.h.wav" "$tones" "$kind" 99 0 0))"
    echo "$name $(tail -c +45 "$T/$name.out.wav" | shasum | cut -c1-16)" >>"$T/hash.txt"
}
# hi-res decimation: aliases must stay 88 dB down (designs: 91 / 96 dB)
case_ d2_882 88200 44100 2 down 0.1 88 65 1000,10000,20000,24200,30000,40100
case_ d2_96  96000 48000 2 down 0.1 88 65 1000,10000,20000,28500,35000,46300
case_ d4_1764 176400 44100 2 down 0.1 88 65 1000,10000,20000,24200,60000,85000
case_ d4_192 192000 48000 2 down 0.1 88 65 1000,10000,20000,28500,50000,90500
# low-rate interpolation: images 88 dB down (whole band flat to 0.45 fin)
case_ u2_2205 22050 44100 2 up 0.1 88 65 1000,5000,9000,10000
case_ u2_24  24000 48000 2 up 0.1 88 65 1000,5000,9500,10800
case_ u4_11025 11025 44100 2 up 0.1 88 65 500,2500,4500,5000
case_ u4_12  12000 48000 2 up 0.1 88 65 500,2500,4500,5400
# x3 / x6 (speech rates): x4 half-bands then Hermite, images 40 dB down
case_ u3_16  16000 48000 2 up 0.1 40 0 500,2000,4000,6500
case_ u6_8   8000 48000 2 up 0.1 40 0 300,1000,2000,3200
# 3:2 is Hermite by default (rsq=1); rsq=2 measures the x2 + Hermite path
# (Hermite 4:3 at 64 kHz droops 0.25 dB at 13 kHz)
case_ r32_q2 32000 48000 2 up 0.3 40 0 1000,5000,10000,13000 2
# 4. mono (the usual audiobook) and time-stretch before the resampler
case_ u2_mono 22050 44100 1 up 0.1 88 65 1000,5000,9000
# mono 24-bit WAV blocks are an odd number of samples: the /2 tail carries
case_ d2_mono 96000 48000 1 down 0.1 88 65 1000,10000,20000,28500,35000,46300
$M gen "$T/ts.wav" 22050 1 1.5 1000,4000
"$WB" -c "outrate=44100:tempo=1.5" "$T/ts.wav" "$T/ts.out.wav" 2>>"$T/warble.log"
n=$(( $(wc -c <"$T/ts.out.wav") / 4 ))
if [ "$n" -gt 40000 ] && [ "$n" -lt 48000 ]; then
    echo "ok   time-stretch 1.5x then x2: $n frames (1.5 s * 44100 / 1.5 = 44100)"
else
    echo "FAIL time-stretch 1.5x then x2: $n frames"; fail=1
fi

# 5. goldens
if [ $UPDATE = 1 ]; then
    cp "$T/hash.txt" tests/resample/golden.txt
    echo "golden.txt updated"
elif cmp -s "$T/hash.txt" tests/resample/golden.txt; then
    echo "ok   outputs match tests/resample/golden.txt"
else
    echo "FAIL outputs differ from tests/resample/golden.txt:"
    diff tests/resample/golden.txt "$T/hash.txt" | head; fail=1
fi

[ $fail -eq 0 ] && echo "resample: all passed" || { echo "resample: FAILED"; exit 1; }
