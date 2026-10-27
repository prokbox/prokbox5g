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

# Bit-perfect host test helper (Phase 15, wave 2a): writes noise WAVs and
# compares the 16-bit stereo WAVs warble writes. Standard library only.
#
#   bp.py gen OUT.wav RATE BITS SECONDS SEED
#   bp.py same A.wav B.wav          exit 0 if the sample data is identical
#   bp.py differ A.wav B.wav PCT    exit 0 if at least PCT % of samples differ
#   bp.py round24 IN24.wav OUT.wav  plain round-to-nearest 24 -> 16 bits
#   bp.py mixed IN.wav AUTO.wav DITH.wav
#                                   timestretch: AUTO keeps the copied
#                                   stretches of IN exact (DITH does not)
#                                   and is not an exact copy throughout
import random
import struct
import sys


def read_wav(path):
    """Return (rate, channels, bits, raw data bytes)."""
    with open(path, 'rb') as f:
        buf = f.read()
    if buf[0:4] != b'RIFF' or buf[8:12] != b'WAVE':
        sys.exit('%s: not a WAV file' % path)
    pos, fmt, data = 12, None, None
    while pos + 8 <= len(buf):
        cid, size = buf[pos:pos + 4], struct.unpack('<I', buf[pos + 4:pos + 8])[0]
        body = buf[pos + 8:pos + 8 + size]
        if cid == b'fmt ':
            fmt = struct.unpack('<HHIIHH', body[:16])
        elif cid == b'data':
            data = body
        pos += 8 + size + (size & 1)
    if fmt is None or data is None:
        sys.exit('%s: no fmt or data chunk' % path)
    return fmt[2], fmt[1], fmt[5], data


def samples16(path):
    rate, ch, bits, data = read_wav(path)
    if bits != 16 or ch != 2:
        sys.exit('%s: expected 16-bit stereo, got %d-bit %d ch' % (path, bits, ch))
    n = len(data) // 2
    return rate, struct.unpack('<%dh' % n, data[:n * 2])


def write_wav(path, rate, bits, frames):
    """frames: flat list of interleaved stereo ints."""
    width = bits // 8
    out = bytearray()
    for v in frames:
        out += (v & ((1 << bits) - 1)).to_bytes(width, 'little')
    hdr = b'RIFF' + struct.pack('<I', 36 + len(out)) + b'WAVE'
    hdr += b'fmt ' + struct.pack('<IHHIIHH', 16, 1, 2, rate,
                                 rate * 2 * width, 2 * width, bits)
    hdr += b'data' + struct.pack('<I', len(out))
    with open(path, 'wb') as f:
        f.write(hdr + out)


def gen(path, rate, bits, seconds, seed):
    rng = random.Random(seed)
    # full scale for 16 bits (exact values never clip); half scale for
    # 24 bits so the dither noise can't reach the clip
    peak = (1 << 15) - 1 if bits == 16 else 1 << 22
    frames = [rng.randint(-peak, peak) for _ in range(int(rate * seconds) * 2)]
    write_wav(path, rate, bits, frames)


def main():
    cmd = sys.argv[1]
    if cmd == 'gen':
        gen(sys.argv[2], int(sys.argv[3]), int(sys.argv[4]),
            float(sys.argv[5]), int(sys.argv[6]))
    elif cmd == 'same':
        ra, a = samples16(sys.argv[2])
        rb, b = samples16(sys.argv[3])
        if ra != rb:
            print('rate %d != %d' % (ra, rb))
            return 1
        if len(a) != len(b):
            print('length %d != %d samples' % (len(a), len(b)))
            return 1
        bad = sum(1 for x, y in zip(a, b) if x != y)
        if bad:
            first = next(i for i, (x, y) in enumerate(zip(a, b)) if x != y)
            print('%d of %d samples differ (first at %d)' % (bad, len(a), first))
            return 1
        print('identical: %d samples at %d Hz' % (len(a), ra))
    elif cmd == 'differ':
        _, a = samples16(sys.argv[2])
        _, b = samples16(sys.argv[3])
        n = min(len(a), len(b))
        bad = sum(1 for x, y in zip(a[:n], b[:n]) if x != y)
        pct = 100.0 * bad / max(n, 1)
        print('%.1f %% of %d samples differ' % (pct, n))
        return 0 if n and pct >= float(sys.argv[4]) else 1
    elif cmd == 'mixed':
        _, src = samples16(sys.argv[2])
        _, auto = samples16(sys.argv[3])
        _, dith = samples16(sys.argv[4])
        # 4-frame windows found verbatim in the input: copied stretches
        grams = set(tuple(src[j:j + 8]) for j in range(0, len(src) - 7, 2))

        def exact_pct(out):
            wins = range(0, len(out) - 7, 2)
            hit = sum(1 for k in wins if tuple(out[k:k + 8]) in grams)
            return 100.0 * hit / max(len(wins), 1)
        pa, pd = exact_pct(auto), exact_pct(dith)
        print('windows copied exactly: %.1f %% (forced dither %.1f %%)'
              % (pa, pd))
        return 0 if 20 <= pa <= 90 and pd < 10 else 1
    elif cmd == 'round24':
        rate, ch, bits, data = read_wav(sys.argv[2])
        assert bits == 24 and ch == 2
        vals = [int.from_bytes(data[i:i + 3], 'little', signed=True)
                for i in range(0, len(data) - 2, 3)]
        write_wav(sys.argv[3], rate, 16,
                  [max(-32768, min(32767, (v + 128) >> 8)) for v in vals])
    else:
        sys.exit('unknown command ' + cmd)
    return 0


if __name__ == '__main__':
    sys.exit(main())
