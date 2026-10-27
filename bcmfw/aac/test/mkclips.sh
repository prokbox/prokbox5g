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
# AAC-LC test clips for bcmfw/aac (#60).  Synthetic sources only (ffmpeg
# lavfi), encoded with ffmpeg's own AAC encoder and with Apple's
# AudioToolbox encoder (aac_at, macOS), as ADTS, then split into our .au
# files (ASC + raw access units, adts2au.py).  ffmpeg's float decode of
# each clip (.ref.s16, stereo) is kept for a sanity check of the level
# of agreement; the bit-exact reference is our host build of libfaad.
# Media goes to build/aac/clips (never committed).
#
#   bcmfw/aac/test/mkclips.sh           build what is missing
#   bcmfw/aac/test/mkclips.sh --force   rebuild everything
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
set -e
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
OUT="$ROOT/build/aac/clips"
FFMPEG=${FFMPEG:-/opt/homebrew/bin/ffmpeg}
FORCE=0
[ "$1" = "--force" ] && FORCE=1
[ -x "$FFMPEG" ] || { echo "mkclips: ffmpeg not found ($FFMPEG)" >&2; exit 2; }
mkdir -p "$OUT"

# ref IN.aac CHANNELS OUT.s16: ffmpeg's decode as stereo; mono is copied to
# both channels as aac_decode does (ffmpeg's own upmix is 3 dB down)
ref() {
    if [ "$2" = 1 ]; then up="pan=stereo|c0=c0|c1=c0"; else up="anull"; fi
    "$FFMPEG" -hide_banner -loglevel error -y -i "$1" -af "$up" -ac 2 \
        -f s16le "$3"
}

# clip NAME RATE CHANNELS ENCODER BITRATE "LAVFI SOURCE" [EXTRA FILTER]
clip() {
    name=$1 rate=$2 ch=$3 enc=$4 br=$5 src=$6
    [ $FORCE -eq 0 ] && [ -f "$OUT/$name.au" ] && return
    "$FFMPEG" -hide_banner -loglevel error -y -filter_complex "$src" \
        -ar "$rate" -ac "$ch" -t 5 -c:a "$enc" -b:a "$br" \
        -f adts "$OUT/$name.aac"
    ref "$OUT/$name.aac" "$ch" "$OUT/$name.ref.s16"
    python3 -I "$ROOT/bcmfw/aac/test/adts2au.py" "$OUT/$name.aac" "$OUT/$name.au"
}

# a pure tone per channel (440 Hz left, 660 Hz right)
clip sine_st128 44100 2 aac 128k \
    "aevalsrc=0.5*sin(2*PI*440*t)|0.5*sin(2*PI*660*t):s=44100:d=5"
# pink noise, independent per channel: dense spectrum, every band coded
clip pink_st128 44100 2 aac 128k \
    "anoisesrc=c=pink:a=0.4:seed=1:r=44100:d=5[a];anoisesrc=c=pink:a=0.4:seed=2:r=44100:d=5[b];[a][b]amerge=inputs=2"
# white noise, mono, 64 kbit/s: PNS and the single-channel path
clip white_mono64 44100 1 aac 64k \
    "anoisesrc=c=white:a=0.3:seed=3:r=44100:d=5"
# music-like: a swept tone with harmonics and a slow tremolo, panned
clip chirp_st128 44100 2 aac 128k \
    "aevalsrc=(0.3*sin(2*PI*(110*t+60*t*t))+0.15*sin(4*PI*(110*t+60*t*t))+0.08*sin(6*PI*(110*t+60*t*t)))*(0.7+0.3*sin(2*PI*3*t))|(0.3*sin(2*PI*(220*t+40*t*t))+0.1*sin(2*PI*(660*t+120*t*t)))*(0.7+0.3*cos(2*PI*2*t)):s=44100:d=5"
# castanets-like: sharp noise bursts four times a second over a quiet
# tone, 48 kHz: short windows (EIGHT_SHORT_SEQUENCE) and the 48 kHz tables
clip clicks_st48k 48000 2 aac 128k \
    "aevalsrc=if(lt(mod(t\,0.25)\,0.02)\,0.8*(random(0)*2-1)*exp(-mod(t\,0.25)*300)\,0)+0.05*sin(2*PI*300*t)|if(lt(mod(t+0.125\,0.25)\,0.02)\,0.8*(random(1)*2-1)*exp(-mod(t+0.125\,0.25)*300)\,0)+0.05*sin(2*PI*450*t):s=48000:d=5"
# stage 8c's soundtrack: a 440 Hz bed with a 20 ms 1 kHz beep each second
clip stage8c_st128 44100 2 aac 128k \
    "aevalsrc=if(lt(mod(t\,1)\,0.02)\,0.3*sin(2*PI*1000*t)\,0.1*sin(2*PI*440*t)):s=44100:c=stereo:d=5"
# Apple's AudioToolbox encoder (what iTunes made): music-like, and mono 32 kHz
clip chirp_at128 44100 2 aac_at 128k \
    "aevalsrc=(0.3*sin(2*PI*(110*t+60*t*t))+0.15*sin(4*PI*(110*t+60*t*t)))*(0.7+0.3*sin(2*PI*3*t))+if(lt(mod(t\,0.5)\,0.01)\,0.5*(random(0)*2-1)\,0)|0.3*sin(2*PI*(220*t+40*t*t))+if(lt(mod(t+0.25\,0.5)\,0.01)\,0.5*(random(1)*2-1)\,0):s=44100:d=5"
clip voice_at_mono32k 32000 1 aac_at 64k \
    "aevalsrc=0.4*sin(2*PI*(150+30*sin(2*PI*4*t))*t)*(0.5+0.5*sin(2*PI*1.5*t))+0.03*(random(0)*2-1):s=32000:d=5"
# the video tests' soundtrack (tests/video/run.sh makes long_ipod.mp4 with
# this source: lavfi's mono sine at 128 kbit/s)
clip sine_mono128 44100 1 aac 128k "sine=frequency=440:sample_rate=44100:d=5"
# the video corpus' soundtrack itself, when the corpus has been built
LONG="$ROOT/build/video/mp4/long_ipod.mp4"
if [ -f "$LONG" ] && { [ $FORCE -eq 1 ] || [ ! -f "$OUT/long_ipod.au" ]; }; then
    "$FFMPEG" -hide_banner -loglevel error -y -i "$LONG" -vn -c:a copy -t 10 \
        -f adts "$OUT/long_ipod.aac"
    ref "$OUT/long_ipod.aac" 1 "$OUT/long_ipod.ref.s16"
    python3 -I "$ROOT/bcmfw/aac/test/adts2au.py" "$OUT/long_ipod.aac" "$OUT/long_ipod.au"
fi
