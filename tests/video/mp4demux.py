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
# Host prototype of the PP-side MP4/M4V demuxer for our BCM2722 video path
# (#60, #64; docs/research/bcm2722/player.md).  It does what the Rockbox
# driver will do, in the same order, so its output is the reference for it:
#
#   1. walk the boxes once (moov first or last; fragmented files refused),
#   2. pick the first 'vide' track with an avc1 sample entry, take avcC,
#   3. flatten stsz/stco|co64/stsc/stts/ctts/stss/elst into one sample table
#      (file offset, size, PTS in us, key) -- the sync-time sidecar index,
#   4. write Annex B packets: avcC's SPS/PPS in front of the first packet,
#      each length prefix replaced by a start code.
#
#   mp4demux.py FILE.mp4 [--annexb OUT.264] [--table OUT.csv] [--index OUT.vix]
#
# Pure Python, no dependencies.  Validated against ffprobe/ffmpeg by
# tests/video/mp4demux_test.py.
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

import argparse
import struct
import sys

CONTAINERS = {b'moov', b'trak', b'mdia', b'minf', b'stbl', b'edts', b'dinf'}


class DemuxError(Exception):
    pass


def boxes(buf, start, end):
    """Yield (type, payload start, payload end) for each box in buf[start:end]."""
    p = start
    while p + 8 <= end:
        size, typ = struct.unpack_from('>I4s', buf, p)
        hdr = 8
        if size == 1:
            if p + 16 > end:
                raise DemuxError('truncated largesize box')
            size = struct.unpack_from('>Q', buf, p + 8)[0]
            hdr = 16
        elif size == 0:
            size = end - p
        if size < hdr or p + size > end:
            raise DemuxError('bad box %r size %d at %d' % (typ, size, p))
        yield typ, p + hdr, p + size
        p += size


def find(buf, start, end, path):
    """The payload range of the first box along path (a list of types)."""
    for typ, s, e in boxes(buf, start, end):
        if typ == path[0]:
            return (s, e) if len(path) == 1 else find(buf, s, e, path[1:])
    return None


def full(buf, s):
    """Skip a full box's version/flags; return (version, payload start)."""
    return buf[s], s + 4


class Track:
    pass


def parse_trak(buf, s, e):
    t = Track()
    t.handler = None
    hd = find(buf, s, e, [b'mdia', b'hdlr'])
    if hd:
        t.handler = bytes(buf[hd[0] + 8:hd[0] + 12])
    md = find(buf, s, e, [b'mdia', b'mdhd'])
    v, p = full(buf, md[0])
    if v == 1:
        t.timescale, t.duration = struct.unpack_from('>IQ', buf, p + 16)
    else:
        t.timescale, t.duration = struct.unpack_from('>II', buf, p + 8)
    st = find(buf, s, e, [b'mdia', b'minf', b'stbl'])
    if not st:
        raise DemuxError('track without stbl')
    t.stbl = {typ: (ps, pe) for typ, ps, pe in boxes(buf, st[0], st[1])}
    t.elst = []
    el = find(buf, s, e, [b'edts', b'elst'])
    if el:
        v, p = full(buf, el[0])
        n = struct.unpack_from('>I', buf, p)[0]
        p += 4
        for _ in range(n):
            if v == 1:
                dur, mt = struct.unpack_from('>Qq', buf, p)
                p += 16
            else:
                dur, mt = struct.unpack_from('>Ii', buf, p)
                p += 8
            p += 4                      # media rate
            t.elst.append((dur, mt))
    return t


def avc_entry(buf, t):
    """The avc1 sample entry: (width, height, avcC bytes) or None."""
    sd = t.stbl.get(b'stsd')
    if not sd:
        return None
    _, p = full(buf, sd[0])
    for typ, s, e in boxes(buf, p + 4, sd[1]):
        if typ not in (b'avc1', b'avc3'):
            continue
        w, h = struct.unpack_from('>HH', buf, s + 24)
        # VisualSampleEntry is 78 bytes, then child boxes
        for ct, cs, ce in boxes(buf, s + 78, e):
            if ct == b'avcC':
                return w, h, bytes(buf[cs:ce])
    return None


def parse_avcc(c):
    """avcC -> (NAL length size, [SPS], [PPS])."""
    if len(c) < 7 or c[0] != 1:
        raise DemuxError('bad avcC')
    lsize = (c[4] & 3) + 1
    p = 5
    out = []
    for count_mask in (0x1f, 0xff):
        n = c[p] & count_mask
        p += 1
        sets = []
        for _ in range(n):
            ln = struct.unpack_from('>H', c, p)[0]
            sets.append(c[p + 2:p + 2 + ln])
            p += 2 + ln
        out.append(sets)
    return lsize, out[0], out[1]


def u32s(buf, rng, skip=0, fmt='>I'):
    _, p = full(buf, rng[0])
    n = struct.unpack_from('>I', buf, p + skip)[0]
    p += skip + 4
    return n, p


def sample_table(buf, t):
    """Flatten the stbl into [(offset, size, dts, pts, key)] in media ticks."""
    sb = t.stbl
    # sizes
    _, p = full(buf, sb[b'stsz'][0])
    fixed, n = struct.unpack_from('>II', buf, p)
    sizes = [fixed] * n if fixed else list(struct.unpack_from('>%dI' % n, buf, p + 8))
    # chunk offsets
    if b'stco' in sb:
        _, p = full(buf, sb[b'stco'][0])
        nc = struct.unpack_from('>I', buf, p)[0]
        chunks = list(struct.unpack_from('>%dI' % nc, buf, p + 4))
    elif b'co64' in sb:
        _, p = full(buf, sb[b'co64'][0])
        nc = struct.unpack_from('>I', buf, p)[0]
        chunks = list(struct.unpack_from('>%dQ' % nc, buf, p + 4))
    else:
        raise DemuxError('no stco/co64')
    # sample-to-chunk
    _, p = full(buf, sb[b'stsc'][0])
    ns = struct.unpack_from('>I', buf, p)[0]
    stsc = [struct.unpack_from('>III', buf, p + 4 + 12 * i) for i in range(ns)]
    offsets = []
    for i, (first, per, _desc) in enumerate(stsc):
        last = stsc[i + 1][0] - 1 if i + 1 < ns else len(chunks)
        for c in range(first - 1, last):
            off = chunks[c]
            for _ in range(per):
                k = len(offsets)
                if k >= n:
                    break
                offsets.append(off)
                off += sizes[k]
    if len(offsets) != n:
        raise DemuxError('stsc covers %d of %d samples' % (len(offsets), n))
    # decode times
    _, p = full(buf, sb[b'stts'][0])
    ne = struct.unpack_from('>I', buf, p)[0]
    dts, d = [], 0
    for i in range(ne):
        cnt, delta = struct.unpack_from('>II', buf, p + 4 + 8 * i)
        for _ in range(cnt):
            dts.append(d)
            d += delta
    dts = dts[:n]
    # composition offsets (Baseline has none; accepted anyway)
    cts = [0] * n
    if b'ctts' in sb:
        v, p = full(buf, sb[b'ctts'][0])
        ne = struct.unpack_from('>I', buf, p)[0]
        k = 0
        for i in range(ne):
            cnt, off = struct.unpack_from('>Ii' if v else '>II', buf, p + 4 + 8 * i)
            for _ in range(cnt):
                if k < n:
                    cts[k] = off
                k += 1
    # sync samples: no stss means every sample is a sync sample
    key = [True] * n
    if b'stss' in sb:
        _, p = full(buf, sb[b'stss'][0])
        ne = struct.unpack_from('>I', buf, p)[0]
        key = [False] * n
        for i in struct.unpack_from('>%dI' % ne, buf, p + 4):
            if 1 <= i <= n:
                key[i - 1] = True
    return [(offsets[i], sizes[i], dts[i], dts[i] + cts[i], key[i]) for i in range(n)]


def edit_shift(t):
    """Media ticks to subtract: the first non-empty edit's media_time (ffmpeg
    does the same for a single edit; empty edits add a presentation delay,
    which iPod-era files don't use, so it is refused)."""
    shift = 0
    for dur, mt in t.elst:
        if mt == -1:
            raise DemuxError('empty edit (presentation delay) not supported')
        shift = mt
        break
    return shift


class Movie:
    def __init__(self, buf):
        self.buf = buf
        top = {typ: (s, e) for typ, s, e in boxes(buf, 0, len(buf))}
        if b'moof' in top or (b'moov' in top and find(buf, *top[b'moov'], [b'mvex'])):
            raise DemuxError('fragmented MP4 not supported')
        if b'moov' not in top:
            raise DemuxError('no moov')
        self.video = None
        self.audio = None
        for typ, s, e in boxes(buf, *top[b'moov']):
            if typ != b'trak':
                continue
            t = parse_trak(buf, s, e)
            if t.handler == b'vide' and self.video is None:
                a = avc_entry(buf, t)
                if a:
                    t.width, t.height, t.avcc = a
                    self.video = t
            elif t.handler == b'soun' and self.audio is None:
                self.audio = t
        if not self.video:
            raise DemuxError('no H.264 video track')
        v = self.video
        v.lsize, v.sps, v.pps = parse_avcc(v.avcc)
        shift = edit_shift(v)
        ts = v.timescale
        # PTS in microseconds, as the FEED record carries it
        self.samples = [(o, s, (pts - shift) * 1000000 // ts, k)
                        for o, s, _dts, pts, k in sample_table(buf, v)]

    def params(self):
        sc = b'\0\0\0\1'
        return b''.join(sc + x for x in self.video.sps + self.video.pps)

    def packet(self, i, with_params=False):
        """Sample i as Annex B: each length prefix becomes a start code."""
        o, s, _, _ = self.samples[i]
        data = self.buf[o:o + s]
        ls = self.video.lsize
        out = [self.params()] if with_params else []
        p = 0
        while p + ls <= len(data):
            n = int.from_bytes(data[p:p + ls], 'big')
            p += ls
            if p + n > len(data):
                raise DemuxError('sample %d: NAL runs past the sample' % i)
            out.append(b'\0\0\0\1' + bytes(data[p:p + n]))
            p += n
        if p != len(data):
            raise DemuxError('sample %d: %d trailing bytes' % (i, len(data) - p))
        return b''.join(out)

    def key_before(self, pts_us):
        """Seek: the last key sample at or before pts_us (stss)."""
        best = 0
        for i, (_, _, pts, k) in enumerate(self.samples):
            if pts > pts_us:
                break
            if k:
                best = i
        return best


# The sidecar index prototype (player.md, "Sample index"): a 32-byte header,
# then one 12-byte record per sample, little-endian like the PP.
#   header: 'VIX1', version 1, sample count, PTS units per second (1000),
#           width | height << 16, avcC offset in the file, avcC length, 0
#   record: file offset bits 0-31,
#           size (bits 0-23) | flags << 24 (bit 0 key, bits 4-7 = offset
#           bits 32-35, so files up to 64 GB),
#           PTS in ms (a u32 in us would wrap after 71 minutes)
VIX_MAGIC = b'VIX1'


def write_index(m, path, avcc_off):
    v = m.video
    recs = []
    for o, s, pts, k in m.samples:
        if s >= 1 << 24 or o >= 1 << 36:
            raise DemuxError('sample too large for the index')
        flags = (1 if k else 0) | ((o >> 32) << 4)
        recs.append(struct.pack('<III', o & 0xffffffff, s | flags << 24, pts // 1000))
    hdr = struct.pack('<4sIIIIIII', VIX_MAGIC, 1, len(recs), 1000,
                      v.width | v.height << 16, avcc_off, len(v.avcc), 0)
    with open(path, 'wb') as f:
        f.write(hdr)
        f.write(b''.join(recs))
    return len(hdr) + 12 * len(recs)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('file')
    ap.add_argument('--annexb')
    ap.add_argument('--table')
    ap.add_argument('--index')
    ap.add_argument('--seek-us', type=int,
                    help='start at the key sample at or before this time')
    a = ap.parse_args()
    with open(a.file, 'rb') as f:
        buf = memoryview(f.read())
    try:
        m = Movie(buf)
    except DemuxError as e:
        print('mp4demux: %s' % e, file=sys.stderr)
        return 2
    v = m.video
    first = m.key_before(a.seek_us) if a.seek_us is not None else 0
    print('video %dx%d, timescale %d, %d samples, %d key, NAL length %d, '
          'SPS %d PPS %d, edit shift %d; audio %s' % (
              v.width, v.height, v.timescale, len(m.samples),
              sum(1 for x in m.samples if x[3]), v.lsize, len(v.sps), len(v.pps),
              edit_shift(v), 'yes' if m.audio else 'no'))
    if a.table:
        with open(a.table, 'w') as f:
            for o, s, pts, k in m.samples:
                f.write('%d,%d,%d,%d\n' % (o, s, pts, k))
    if a.annexb:
        with open(a.annexb, 'wb') as f:
            for i in range(first, len(m.samples)):
                f.write(m.packet(i, with_params=(i == first)))
    if a.index:
        off = bytes(buf).find(b'avcC')
        print('index: %d bytes' % write_index(m, a.index, off + 4 if off >= 0 else 0))
    return 0


if __name__ == '__main__':
    sys.exit(main())
