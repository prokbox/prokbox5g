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

"""#281 FLAC block-size test helper (tests/flacblock/run.sh).

    fb.py gen OUT.raw BITS N          stereo little-endian PCM, N frames
    fb.py info IN.flac                prints "bs=<max> bps= rate= ch=
                                      maxframe= seekpoints="
    fb.py zeroframes IN.flac OUT.flac STREAMINFO without frame sizes
    fb.py same SRC.raw BITS OUT.raw [FRAME]
                                      OUT is warble -r output (int32,
                                      interleaved, << (29 - BITS)): exit 0
                                      when every sample equals SRC (from
                                      stereo frame FRAME on: a seek)
    fb.py samepcm SRC.raw REF.raw     byte-identical reference decode
    fb.py need IN.flac                bytes flac.c takes from the codec
                                      buffer for this stream
"""
import struct
import sys


def gen(path, bits, n):
    # deterministic: a sine pair plus LCG noise, the right channel partly
    # correlated (side and mid-side coding) and partly not (verbatim)
    import math
    full = (1 << (bits - 1)) - 1
    x = 12345
    out = bytearray()
    w = bits // 8
    for i in range(n):
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        noise = ((x >> 8) % 2001 - 1000) / 1000.0
        s = math.sin(i * 0.031) * 0.5
        left = int((s + 0.25 * noise) * full)
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        noise2 = ((x >> 8) % 2001 - 1000) / 1000.0
        right = int((s * 0.8 + (0.4 if i % 50000 < 25000 else 0.02) * noise2)
                    * full)
        for v in (left, right):
            v = max(-full - 1, min(full, v))
            out += (v & ((1 << bits) - 1)).to_bytes(w, "little")
    open(path, "wb").write(out)


def streaminfo(data):
    assert data[:4] == b"fLaC", "not a FLAC file"
    p = 4
    si = None
    npts = 0
    while True:
        last = data[p] & 0x80
        typ = data[p] & 0x7F
        ln = int.from_bytes(data[p + 1:p + 4], "big")
        if typ == 0:
            si = (p + 4, data[p + 4:p + 4 + ln])
        elif typ == 3:
            npts = ln // 18
        p += 4 + ln
        if last:
            break
    off, b = si
    v = int.from_bytes(b[10:18], "big")
    return {
        "off": off,
        "minbs": int.from_bytes(b[0:2], "big"),
        "bs": int.from_bytes(b[2:4], "big"),
        "maxframe": int.from_bytes(b[7:10], "big"),
        "rate": v >> 44,
        "ch": ((v >> 41) & 7) + 1,
        "bps": ((v >> 36) & 31) + 1,
        "seekpoints": npts,
    }


def align16(n):
    return (n + 15) & ~15


def need(si):
    # mirrors lib/rbcodec/codecs/flac.c flac_alloc() and the SEEKTABLE
    # allocation (MAX_BLOCKSIZE 8192 on the host, 4608 on the device: a
    # stream over it takes every channel from the codec buffer)
    bs, ch, bps = si["bs"], si["ch"], si["bps"]
    worst = (bs * ch * (bps + 1) + 7) // 8 + 64
    frame = si["maxframe"] if 0 < si["maxframe"] < worst else worst
    return (ch * align16(bs * 4) + align16(frame)
            + align16(min(si["seekpoints"], 5000) * 12))


def main():
    cmd = sys.argv[1]
    if cmd == "gen":
        gen(sys.argv[2], int(sys.argv[3]), int(sys.argv[4]))
    elif cmd == "info":
        si = streaminfo(open(sys.argv[2], "rb").read())
        print("bs=%d bps=%d rate=%d ch=%d maxframe=%d seekpoints=%d"
              % (si["bs"], si["bps"], si["rate"], si["ch"], si["maxframe"],
                 si["seekpoints"]))
    elif cmd == "zeroframes":
        d = bytearray(open(sys.argv[2], "rb").read())
        si = streaminfo(d)
        d[si["off"] + 4:si["off"] + 10] = bytes(6)
        open(sys.argv[3], "wb").write(d)
    elif cmd == "need":
        print(need(streaminfo(open(sys.argv[2], "rb").read())))
    elif cmd == "same":
        src = open(sys.argv[2], "rb").read()
        bits = int(sys.argv[3])
        out = open(sys.argv[4], "rb").read()
        w = bits // 8
        if len(sys.argv) > 5:           # output from stereo frame N on
            src = src[int(sys.argv[5]) * 2 * w:]
        n = len(src) // w
        if len(out) != n * 4:
            print("length %d samples, want %d" % (len(out) // 4, n))
            return 1
        shift = 29 - bits
        got = struct.unpack("<%di" % n, out)
        for i in range(n):
            want = int.from_bytes(src[i * w:i * w + w], "little", signed=True)
            if got[i] != want << shift:
                print("sample %d: %d, want %d" % (i, got[i] >> shift, want))
                return 1
        print("%d samples identical" % n)
    elif cmd == "samepcm":
        a = open(sys.argv[2], "rb").read()
        b = open(sys.argv[3], "rb").read()
        if a != b:
            print("reference decode differs (%d vs %d bytes)" % (len(a), len(b)))
            return 1
        print("reference decode identical (%d bytes)" % len(a))
    return 0


if __name__ == "__main__":
    sys.exit(main())
