#!/usr/bin/env python3
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

"""p090-osr128 (#439): the b17 row "128x at 96 kHz"'s test piece, ours.

A 96 kHz, 24-bit stereo piece of about 28 s that ends in the state where
the owner heard a faint crackle at Hi-Res 128x (the silent tail of a 16/96
track, 2026-11-16):

  0-18 s   a slow tone sequence (a C-major arpeggio and its fifths, soft
           attack, three harmonics, a little stereo spread), about -14.5 dBFS peak;
  18-24 s  the last chord fading exponentially towards -130 dBFS: the 24-bit
           samples step down through the codec's smallest codes, undithered,
           so they reach exact zero on their own (the first 1024 zeros come
           at about 23.6 s);
  to 28 s  exact digital silence (1024 zeros engage the DAC's AMUTE, so the
           AMUTE arm hears its mute here).

Deterministic (no randomness), stdlib only; writes WAV and, when the `flac`
tool is on PATH, FLAC next to it.  Prints the sample count, the peak and
where the first run of 1024 zeros starts, so the row can check the file.

    python3 tests/fixtures/make_tail96_fixture.py build/b17/tail96
"""
import math
import os
import shutil
import struct
import subprocess
import sys
import wave

RATE = 96000
TONE_S, FADE_S, ZERO_S = 18.0, 6.0, 4.0
PEAK_DB = -12.0
FADE_END_DB = -130.0
FULL = (1 << 23) - 1

# C4 E4 G4 C5 G4 E4, then the fifths, each note 1.5 s
NOTES = [261.63, 329.63, 392.00, 523.25, 392.00, 329.63,
         392.00, 493.88, 587.33, 783.99, 587.33, 493.88]
CHORD = [261.63, 329.63, 392.00, 523.25]


def voice(f, t):
    return (math.sin(2 * math.pi * f * t) + 0.35 * math.sin(4 * math.pi * f * t)
            + 0.12 * math.sin(6 * math.pi * f * t)) / 1.47


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "tail96"
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    amp = 10 ** (PEAK_DB / 20)
    n_tone = int(TONE_S * RATE)
    n_fade = int(FADE_S * RATE)
    n_zero = int(ZERO_S * RATE)
    note_n = int(1.5 * RATE)
    frames = bytearray()
    peak = 0
    first_zero_run = None
    zrun = 0
    total = n_tone + n_fade + n_zero
    for i in range(total):
        t = i / RATE
        if i < n_tone:
            k = i // note_n
            pos = (i % note_n) / RATE
            env = min(1.0, pos / 0.08) * math.exp(-pos * 0.9)
            s = env * voice(NOTES[k % len(NOTES)], t)
            l, r = s * 1.0, s * 0.85
        elif i < n_tone + n_fade:
            x = (i - n_tone) / n_fade
            g = 10 ** ((FADE_END_DB * x) / 20)
            s = g * sum(voice(f, t) for f in CHORD) / len(CHORD) * 0.6
            l, r = s * 0.9, s
        else:
            l = r = 0.0
        li = int(round(l * amp * FULL))
        ri = int(round(r * amp * FULL))
        peak = max(peak, abs(li), abs(ri))
        if li == 0 and ri == 0:
            zrun += 1
            if zrun == 1024 and first_zero_run is None:
                first_zero_run = i - 1023
        else:
            zrun = 0
        frames += struct.pack("<i", li)[:3] + struct.pack("<i", ri)[:3]
    with wave.open(out + ".wav", "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(3)
        w.setframerate(RATE)
        w.writeframes(bytes(frames))
    print("tail96: %d frames (%.1f s), peak %.1f dBFS, first 1024 zeros at "
          "%.3f s" % (total, total / RATE, 20 * math.log10(peak / FULL),
                      (first_zero_run or 0) / RATE))
    if shutil.which("flac"):
        subprocess.run(["flac", "-s", "-f", "-8", "-o", out + ".flac",
                        out + ".wav"], check=True)
        print("tail96: wrote %s.flac" % out)


if __name__ == "__main__":
    main()
