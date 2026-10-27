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
"""Sanity check of the host reference against ffmpeg's float AAC decoder.

The two decoders are not bit-exact (libfaad is fixed point), so this only
asks that libfaad's output is the same signal.  At the best alignment
(libfaad withholds the priming AU's output; ffmpeg emits it) either the
signal-to-difference ratio is at least MIN_SNR dB, or, where the encoder
used perceptual noise substitution (PNS: each decoder makes its own
noise, so the waveforms differ by design), the level of every 1024-frame
block agrees: median difference at most ENV_MEDIAN dB, 95th percentile at
most ENV_P95 dB.  Catches a build that is consistent with itself but
wrong (e.g. a wrong output scale or a broken config).

Usage: refcheck.py BUILD_DIR CLIP_DIR      (BUILD_DIR/NAME.s16 vs
                                           CLIP_DIR/NAME.ref.s16)
"""
import array
import math
import os
import sys

MIN_SNR = 40.0
ENV_MEDIAN = 0.5
ENV_P95 = 2.0


def s16(path):
    a = array.array('h')
    with open(path, 'rb') as f:
        a.frombytes(f.read())
    if sys.byteorder != 'little':
        a.byteswap()
    return a


def snr(ours, ref):
    """(best dB, lag in frames): ours[i] ~ ref[i + lag] (stereo frames)."""
    best = (-1e9, 0)
    for lag in (0, 1024, 2048, -1024):
        a = ours[2 * max(0, -lag):]
        b = ref[2 * max(0, lag):]
        n = min(len(a), len(b))
        if n < 8192:
            continue
        err = sum((x - y) * (x - y) for x, y in zip(a[:n], b[:n]))
        sig = sum(y * y for y in b[:n])
        db = 10 * math.log10(sig / err) if err else 200.0
        best = max(best, (db, lag))
    return best


def envelope(ours, ref, lag):
    """(median, 95th percentile) of |level difference| in dB over the
    1024-frame blocks above -60 dBFS."""
    a, b = ours[2 * max(0, -lag):], ref[2 * max(0, lag):]
    n = min(len(a), len(b)) // 2048 * 2048
    diffs = []
    for k in range(0, n, 2048):
        ea = sum(x * x for x in a[k:k + 2048]) / 2048.0
        eb = sum(y * y for y in b[k:k + 2048]) / 2048.0
        if eb > 32.8 ** 2 and ea > 0:
            diffs.append(abs(10 * math.log10(ea / eb)))
    if not diffs:
        return 99.0, 99.0
    diffs.sort()
    return diffs[len(diffs) // 2], diffs[int(len(diffs) * 0.95)]


def main(argv):
    out, clips = argv
    bad = 0
    for f in sorted(os.listdir(clips)):
        if not f.endswith('.au'):
            continue
        name = f[:-3]
        ours = s16(os.path.join(out, name + '.s16'))
        ref = s16(os.path.join(clips, name + '.ref.s16'))
        db, lag = snr(ours, ref)
        med, p95 = envelope(ours, ref, lag)
        ok = db >= MIN_SNR or (med <= ENV_MEDIAN and p95 <= ENV_P95)
        bad += not ok
        print('%-20s vs ffmpeg: SNR %5.1f dB, block levels within %.2f dB '
              '(median) / %.2f dB (95%%), lag %d frames: %s' % (
                  name, db, med, p95, lag, 'ok' if ok else 'FAIL'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
