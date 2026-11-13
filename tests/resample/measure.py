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

# Resampler measurements through warble's real DSP (P19 19-13,
# docs/design/resample-plan.md). Needs numpy.
#
#   measure.py gen OUT.wav RATE CHANNELS SECONDS F1[,F2...]
#       24-bit WAV; each of the n tones at 0.9/n of full scale
#   measure.py check IN_RATE OUT.wav F1[,F2...] KIND PASS_DB REJECT_DB RES_DB
#       KIND "down": every input tone above 20 kHz must alias at least
#       REJECT_DB below its own level; tones at or below 20 kHz are the
#       passband. KIND "up": images (k*IN_RATE +/- f) must be REJECT_DB
#       down; every tone is passband. Passband tones must come out within
#       +/- PASS_DB of their input level. Also checks the residual (output
#       minus the fitted tones) as a glitch detector: at most -RES_DB
#       relative to one tone (0 = report only).
#       KIND "frac" (any ratio, #271): passband tones are those at or below
#       min(20 kHz, 0.45 of the lower rate); every other tone, and every
#       k * IN_RATE / 8 +/- f of every tone, folded into the output band,
#       is a spur when it lands in 20 Hz..min(20 kHz, output Nyquist) away
#       from the passband tones. That covers images and aliases of every
#       stage (the decimators before the FIR run at IN_RATE / 2^n).
#       Prints one line; exit 1 on failure. REJECT_DB 0 = report only.
import struct
import sys

import numpy as np

LEVEL = 0.25  # set per file: 0.9 / number of tones


def write_wav(path, rate, data):  # data: (n, ch) float in [-1, 1)
    n, ch = data.shape
    pcm = np.clip(np.round(data * 8388608.0), -8388608, 8388607).astype('<i4')
    raw = b''.join(int(v).to_bytes(3, 'little', signed=True)
                   for v in pcm.reshape(-1))
    with open(path, 'wb') as f:
        f.write(b'RIFF' + struct.pack('<I', 36 + len(raw)) + b'WAVE')
        f.write(b'fmt ' + struct.pack('<IHHIIHH', 16, 1, ch, rate,
                                      rate * ch * 3, ch * 3, 24))
        f.write(b'data' + struct.pack('<I', len(raw)) + raw)


def read_wav(path):
    buf = open(path, 'rb').read()
    pos, fmt, data = 12, None, None
    while pos + 8 <= len(buf):
        cid = buf[pos:pos + 4]
        size = struct.unpack('<I', buf[pos + 4:pos + 8])[0]
        if cid == b'fmt ':
            fmt = struct.unpack('<HHIIHH', buf[pos + 8:pos + 24])
        elif cid == b'data':
            data = buf[pos + 8:pos + 8 + size]
        pos += 8 + size + (size & 1)
    ch, rate, bits = fmt[1], fmt[2], fmt[5]
    assert bits in (16, 32)
    if bits == 16:
        x = np.frombuffer(data, '<i2').astype(float) / 32768.0
    else:
        x = np.frombuffer(data, '<i4').astype(float) / 2147483648.0
    return rate, x.reshape(-1, ch)


def tone_amp(x, rate, f):
    """Amplitude of the tone at f (Hz): windowed DFT, 4-term
    Blackman-Harris, power summed over +/- 4 bins."""
    n = len(x)
    k = np.arange(n)
    w = (0.35875 - 0.48829 * np.cos(2 * np.pi * k / n)
         + 0.14128 * np.cos(4 * np.pi * k / n)
         - 0.01168 * np.cos(6 * np.pi * k / n))
    t = k / float(rate)
    p = 0.0
    df = rate / float(n)
    for j in range(-4, 5):
        fj = f + j * df
        z = np.sum(x * w * np.exp(-2j * np.pi * fj * t))
        p += abs(z) ** 2
    # the window's power gain, summed the same way, is sum(w^2) * n
    return np.sqrt(p / (np.sum(w * w) * n)) * 2


def db(a):
    return 20 * np.log10(max(a, 1e-12))


def main():
    a = sys.argv
    if a[1] == 'gen':
        path, rate, ch, secs = a[2], int(a[3]), int(a[4]), float(a[5])
        fs = [float(v) for v in a[6].split(',')]
        level = 0.9 / len(fs)
        t = np.arange(int(rate * secs)) / float(rate)
        x = sum(level * np.sin(2 * np.pi * f * t + 0.3 * i)
                for i, f in enumerate(fs))
        write_wav(path, rate, np.repeat(x[:, None], ch, axis=1))
        return
    fin, path = int(a[2]), a[3]
    fs = [float(v) for v in a[4].split(',')]
    kind, pass_db, rej_db, res_db = a[5], float(a[6]), float(a[7]), float(a[8])
    global LEVEL
    LEVEL = 0.9 / len(fs)
    rate, y = read_wav(path)
    skip = rate // 4                        # settle
    n = min(1 << 15, len(y) - skip)
    worst_pass, worst_rej, worst_res = 0.0, -999.0, -999.0
    for c in range(y.shape[1]):
        x = y[skip:skip + n, c]
        if kind == 'frac':
            edge = min(20000.0, 0.45 * min(fin, rate))
            passband = [f for f in fs if f <= edge]
            # tones both rates carry (beyond the passband, in a stage's
            # transition band) are content, not spurs: fitted, not checked
            legit = [f for f in fs if f < min(fin, rate) / 2.0]
        else:
            passband = [f for f in fs if kind == 'up' or f <= 20000]
            legit = passband
        for f in passband:
            g = db(tone_amp(x, rate, f) / LEVEL)
            worst_pass = max(worst_pass, abs(g))
        spurs = []
        top = min(20000.0, rate / 2.0 - 20)
        for f in fs if kind == 'frac' else []:
            base = fin / 8.0
            for k in range(0, int(4 * max(fin, rate) / base) + 1):
                for im in ((f,) if k == 0 else (k * base - f, k * base + f)):
                    al = abs(im - round(im / rate) * rate)
                    if k == 0 and f in legit:
                        continue
                    if 20 < al < top and min(abs(al - p) for p in legit) > 30:
                        spurs.append(al)
        spurs = sorted(set(round(v, 3) for v in spurs))
        for f in fs if kind != 'frac' else []:
            if kind == 'down' and f > 20000:
                al = abs(f - round(f / rate) * rate)
                if al <= 20000:
                    spurs.append(al)
            elif kind == 'up':
                k = 1
                while k * fin - f < rate / 2.0:
                    for im in (k * fin - f, k * fin + f):
                        if 20 < im < rate / 2.0 - 20:
                            spurs.append(im)
                    k += 1
        for s in spurs:
            worst_rej = max(worst_rej, db(tone_amp(x, rate, s) / LEVEL))
        # residual: least-squares fit of the passband tones, then RMS
        t = np.arange(n) / float(rate)
        cols = []
        for f in legit:
            cols += [np.sin(2 * np.pi * f * t), np.cos(2 * np.pi * f * t)]
        m = np.array(cols).T
        coef, *_ = np.linalg.lstsq(m, x, rcond=None)
        r = x - m @ coef
        for s in spurs:  # the spurs are measured above; leave them out
            ms = np.array([np.sin(2 * np.pi * s * t),
                           np.cos(2 * np.pi * s * t)]).T
            cs, *_ = np.linalg.lstsq(ms, r, rcond=None)
            r = r - ms @ cs
        worst_res = max(worst_res, db(np.sqrt(np.mean(r * r)) * np.sqrt(2) / LEVEL))
    ok = (worst_pass <= pass_db and (rej_db == 0 or worst_rej <= -rej_db)
          and (res_db == 0 or worst_res <= -res_db))
    print('passband %.3f dB (<= %.2f), worst %s %.1f dB%s, residual %.1f dB'
          % (worst_pass, pass_db, 'alias' if kind == 'down' else 'image',
             worst_rej, '' if rej_db == 0 else ' (<= -%.0f)' % rej_db,
             worst_res))
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
