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

# The FLAC decoder as the device runs it, under qemu-system-arm: the
# decoder objects of a firmware build (lib/rbcodec/codecs/libffmpegFLAC,
# decoder.o and arm.o, rebuilt here from the current sources) linked into a
# bare-metal driver (tests/flacarm/flac_main.c) that decodes a stream and
# writes its PCM.
#
#   1. bit-exactness: for every stream below (flac, ffmpeg and synth.py
#      made, block sizes up to 65535), the output must be
#      byte-identical to the reference decoder's (flac -d), and the streams
#      together must use every LPC order 1..32 on both filter paths (the
#      32-bit one and the 64-bit "wide" one; the bound that picks between
#      them, from both sides of its edge; the wide filter's order 2..4
#      register windows with every leftover count), fixed orders 0..4, verbatim
#      and constant subframes, escaped Rice partitions, wasted bits and all
#      four channel assignments (flac --analyze; a gate that inspected
#      nothing must not pass).
#   2. cycles: the ARM7TDMI estimate per decoded stereo sample, decoder
#      only (the driver's own functions are drv_* and main, subtracted), and
#      per 4096-sample frame (tests/resample/cyc_plugin.c with exact=on:
#      real multiplier m from Rs, failed conditions 1 cycle; zero-wait
#      memory, so IRAM or cache hits everywhere).
#
#   tests/flacarm/run.sh [FWDIR]      FWDIR: a configured firmware build
#                                     (default build/fw-ship)
#   PROFILE=1  also print the per-symbol profile of each stream (PROFILE=pc:
#              and keep per-address cycles in build/flacarm/NAME/qemu.log)
#   ONLY=name  run one stream
#
# Exits 1 on any mismatch or missing coverage; exits 0 with a note when
# arm-none-eabi-gcc, qemu-system-arm (with qemu-plugin.h) or flac is missing.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT" || exit 1
FW=${1:-build/fw-ship}
case $FW in /*) ;; *) FW=$ROOT/$FW ;; esac
B=$ROOT/build/flacarm
H=scripts/risk/qemu-harness
CC=arm-none-eabi-gcc
mkdir -p "$B"
for t in $CC qemu-system-arm flac; do
    command -v $t >/dev/null 2>&1 || { echo "note: no $t; FLAC ARM checks skipped"; exit 0; }
done
if [ ! -f "$FW/Makefile" ]; then
    # scripts/test.sh calls this without an argument; a fresh tree has no
    # firmware build yet (scripts/validate.sh makes build/fw-ship)
    [ -z "${1:-}" ] && { echo "note: no firmware build in $FW; FLAC ARM checks skipped"; exit 0; }
    echo "FAIL no firmware build in $FW (configure one)"; exit 1
fi
V=$(qemu-system-arm --version | sed -n 's/.*version \([0-9.]*\).*/\1/p;q')
PH=""
for p in /opt/homebrew/Cellar/qemu/"$V"/include /usr/local/Cellar/qemu/"$V"/include /usr/include/qemu; do
    [ -f "$p/qemu-plugin.h" ] && PH=$p && break
done
[ -n "$PH" ] || { echo "note: no qemu-plugin.h; FLAC ARM checks skipped"; exit 0; }
# shellcheck disable=SC2046
cc -shared -fPIC -O2 -undefined dynamic_lookup -I"$PH" \
   $(pkg-config --cflags glib-2.0 2>/dev/null) \
   tests/resample/cyc_plugin.c -o "$B/cyc.dylib" || exit 1

# the decoder, built by the firmware's own rules and flags
L=$FW/lib/rbcodec/codecs/libffmpegFLAC
# Through the library's target, never the object's: codecs.make gives the
# library its -O2 as a target-specific variable, which an object made on its
# own does not inherit (it would be measured at the global -Os).
rm -f "$L/decoder.o" "$L/arm.o"
make -C "$FW" V=1 "$FW/lib/rbcodec/codecs/libffmpegFLAC.a" >"$B/fwmake.log" 2>&1 \
    || { echo "FAIL building the decoder in $FW:"; tail -20 "$B/fwmake.log"; exit 1; }
cp "$B/fwmake.log" "$B/flags.txt"
olev=$(python3 -c '
import sys
t = open(sys.argv[1]).read().replace("\\\n", " ")
for l in t.splitlines():
    w = l.split()
    if "-c" in w and any(x.endswith("libffmpegFLAC/decoder.c") for x in w):
        print([x for x in w if x[:2] == "-O"][-1])
        break' "$B/fwmake.log")
[ "$olev" = -O2 ] || { echo "FAIL decoder.c built with ${olev:-no -O flag}, not the codec's -O2"; exit 1; }
# the driver, with the decoder's compile flags (struct layouts must agree)
python3 - "$B/flags.txt" "$ROOT/tests/flacarm/flac_main.c" "$B/drv.o" >"$B/drvcc.sh" <<'EOF' || { echo "FAIL no compile line for decoder.c in $FW"; exit 1; }
import shlex, sys
txt = open(sys.argv[1]).read().replace("\\\n", " ")
for line in txt.splitlines():
    if "libffmpegFLAC/decoder.c" in line and "-c" in line.split():
        a = shlex.split(line)
        a = [x for x in a if not x.endswith("ccache")]
        out, i = [], 0
        while i < len(a):
            if a[i] in ("-c", "-o", "-MF", "-MT"):
                i += 2
                continue
            if a[i] in ("-MMD", "-MP"):
                i += 1
                continue
            out.append(a[i])
            i += 1
        out += ["-c", sys.argv[2], "-o", sys.argv[3]]
        print(" ".join(shlex.quote(x) for x in out))
        sys.exit(0)
sys.exit(1)
EOF
sh "$B/drvcc.sh" || { echo "FAIL compiling the driver"; exit 1; }
$CC -mcpu=arm7tdmi -c "$H/start.S" -o "$B/start.o" || exit 1
$CC -mcpu=arm7tdmi -O2 -ffreestanding -fno-builtin -c tests/npscreen/insn_libc.c -o "$B/libc.o" || exit 1
$CC -mcpu=arm7tdmi -nostdlib -ffreestanding -Wl,-T,"$H/link.ld" -Wl,--no-warn-rwx-segments \
    "$B/start.o" "$B/drv.o" "$L/decoder.o" "$L/arm.o" "$B/libc.o" -lgcc \
    -o "$B/flac.elf" || { echo "FAIL linking"; exit 1; }

fail=0
checked=0
# arm.S's helpers that flac -d cannot judge, against their C (flac_main.c)
mkdir -p "$B/selftest"
: >"$B/selftest/selftest"
st=$(cd "$B/selftest" && timeout 60 qemu-system-arm -M versatilepb -cpu arm926 -nographic \
    -semihosting -kernel "$B/flac.elf" </dev/null 2>&1)
case "$st" in
    *"RANGE ok"*) echo "ok   flac_range_or_arm == C on 400 arrays in and out of range" ;;
    *) echo "FAIL flac_range_or_arm self-test: $(echo "$st" | head -2)"; fail=1 ;;
esac
bad() { echo "FAIL $*"; fail=1; }

# name bits rate seconds kind encoder-options
STREAMS='s24_192 24 192000 0.75 music -8
s24_96 24 96000 1 music -8
s16_44 16 44100 2 music -8
s16_192 16 192000 0.75 music -8
s24_192_l32 24 192000 0.5 music -8 --lax -l 32
s24_96_l32p 24 96000 0.5 music -8 --lax -l 32 -p
s16_44_l32 16 44100 1 music -8 --lax -l 32
s16_44_q15 16 44100 1 music -8 --lax -l 32 -q 15
s24_192_b16k 24 192000 0.5 music -8 -b 16384
s16_44_b16k 16 44100 1 music -8 --lax -b 16384
s24_96_m5 24 96000 0.5 music -5
s16_44_m0 16 44100 1 music -0
s24_96_noise 24 96000 0.5 noise -8
s16_44_noise 16 44100 1 noise -8
s24_192_quiet 24 192000 0.25 quiet -8
s24_192_l3 24 192000 0.5 music -8 -l 3
ff24_192_16k 24 192000 0.5 ffmpeg 16384
ff24_192_64k 24 192000 1.1 ffmpeg 65535
ff24_96_4k 24 96000 0.5 ffmpeg 4096
ff16_44 16 44100 1 ffmpeg 4608
syn24 24 96000 0 synth 1152 64
syn16 16 44100 0 synth 1152 64
syn24_4608 24 96000 0 synth 4608 12
syn24_var 24 96000 0 synth 1152 120 var
syn29 29 96000 0 synth 1152 24'
[ -n "${ONLY:-}" ] && STREAMS=$(echo "$STREAMS" | grep "^$ONLY ")

printf '%-14s %6s %8s %12s %9s %12s\n' stream frames samples "dec cycles" "cyc/samp" "cyc/4096fr"
echo "$STREAMS" | while read -r name bits rate secs kind opts; do
    d=$B/$name
    mkdir -p "$d"
    rm -f "$d/ok"
    src=$d/src.raw
    if [ "$kind" = ffmpeg ]; then
        # ffmpeg's own encoder (its block sizes, predictor choices and
        # 16-bit wide-filter streams), from the music signal
        command -v ffmpeg >/dev/null 2>&1 || { bad "$name: ffmpeg not found"; continue; }
        [ -s "$src" ] || python3 tests/flacarm/gen.py "$src" "$bits" "$rate" "$secs" music
        [ "$bits" = 16 ] && pcm=s16le || pcm=s24le
        ffmpeg -nostdin -v error -y -f $pcm -ar "$rate" -ac 2 -i "$src" \
            -c:a flac -frame_size "$opts" "$d/in.flac" </dev/null \
            || { bad "$name: ffmpeg encode"; continue; }
    elif [ "$kind" = synth ]; then
        # BLOCKSIZE FRAMES [var], seeded with the bit depth
        # shellcheck disable=SC2086
        set -- $opts
        python3 tests/flacarm/synth.py "$d/in.flac" "$bits" "$1" "$2" "$bits" ${3:-} \
            || { bad "$name: synth.py"; continue; }
    else
        [ -s "$src" ] || python3 tests/flacarm/gen.py "$src" "$bits" "$rate" "$secs" "$kind"
        # shellcheck disable=SC2086
        flac -s -f $opts --force-raw-format --endian=little --sign=signed \
            --channels=2 --bps="$bits" --sample-rate="$rate" -o "$d/in.flac" "$src" \
            || { bad "$name: encoder"; continue; }
    fi
    case $bits in
    8|16|24|32)
        flac -s -f -d --force-raw-format --endian=little --sign=signed \
            -o "$d/ref.raw" "$d/in.flac" 2>"$d/ref.log" || { bad "$name: reference decode"; continue; } ;;
    *)  # flac's raw output takes only whole bytes: go through its WAV
        flac -s -f -d -o "$d/ref.wav" "$d/in.flac" 2>"$d/ref.log" \
            && python3 tests/flacarm/wav2raw.py "$d/ref.wav" "$d/ref.raw" "$bits" \
            || { bad "$name: reference decode"; continue; } ;;
    esac
    flac -s -f -a -o "$d/in.ana" "$d/in.flac" 2>/dev/null || { bad "$name: analyze"; continue; }
    rm -f "$d/out.raw"
    out=$(cd "$d" && timeout 300 qemu-system-arm -M versatilepb -cpu arm926 -nographic \
        -semihosting -plugin "$B/cyc.dylib,arg=exact=on,arg=prof=$([ "${PROFILE:-}" = pc ] && echo pc || echo on)" \
        -kernel "$B/flac.elf" </dev/null 2>&1)
    echo "$out" >"$d/qemu.log"
    case "$out" in
        *"FRAMES "*) ;;
        *) bad "$name: $(echo "$out" | grep -E 'ERROR|cyc_plugin' | head -2)"; continue ;;
    esac
    if ! cmp -s "$d/out.raw" "$d/ref.raw"; then
        bad "$name: output differs from flac -d ($(cmp "$d/out.raw" "$d/ref.raw" 2>&1 | head -1))"
        continue
    fi
    frames=$(echo "$out" | sed -n 's/.*FRAMES \([0-9]*\) SAMPLES.*/\1/p')
    samples=$(echo "$out" | sed -n 's/.*SAMPLES \([0-9]*\).*/\1/p')
    total=$(echo "$out" | sed -n 's/^CYC_COUNT //p')
    drv=$(echo "$out" | awk '$1 == "SYM" && ($4 ~ /^drv_/ || $4 == "main" || $4 == "_start" || $4 == "hang" || $4 ~ /^mem/) {s += $2} END {print s + 0}')
    decc=$((total - drv))
    printf '%-14s %6s %8s %12s %9s %12s\n' "$name" "$frames" "$samples" "$decc" \
        "$(echo "scale=1; $decc / $samples" | bc)" \
        "$(echo "scale=0; $decc * 4096 / $samples" | bc)"
    if [ -n "${PROFILE:-}" ]; then
        echo "$out" | awk -v t="$decc" '$1 == "SYM" && $4 !~ /^drv_/ && $4 != "main" && $4 !~ /^mem/ {printf "      %-28s %10d %5.1f%%\n", $4, $2, 100 * $2 / t}' | sort -k2 -n -r | head -25
    fi
    echo ok >"$d/ok"
done
# the while loop ran in a subshell: count its verdicts from the files
for n in $(echo "$STREAMS" | awk '{print $1}'); do
    if [ -f "$B/$n/ok" ]; then checked=$((checked + 1)); rm -f "$B/$n/ok"; else fail=1; fi
done
want=$(echo "$STREAMS" | grep -c .)
echo "flacarm: $checked of $want streams bit-identical to flac -d"
[ "$checked" = "$want" ] || fail=1

if [ -z "${ONLY:-}" ]; then
    # coverage, from the encoder's own analysis of the streams just checked
    # only this run's streams: one dropped from the list leaves its
    # directory (and in.ana) behind, which must not count
    # shellcheck disable=SC2046
    python3 tests/flacarm/coverage.py $(for n in $(echo "$STREAMS" | awk '{print $1}'); do
        echo "$B/$n/in.ana"; done) || fail=1
fi
[ $fail = 0 ] && echo "flacarm: all passed" || { echo "flacarm: FAILED"; exit 1; }
