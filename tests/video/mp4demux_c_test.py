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
# The C demuxer (apps/video/mp4demux.c, driven by test_mp4demux.c) against
# the Python reference (mp4demux.py, itself checked against ffprobe and
# ffmpeg by mp4demux_test.py).  For each corpus file, and for rewritten
# variants that reach the rarer paths (co64, an edit shift, ctts v1 with
# negative offsets, no stss, one stsc entry per chunk, a first stsc entry
# past chunk 1), at several scratch sizes:
#   - the sample table (offset, size, PTS us, key) is identical,
#   - the Annex B stream is byte-identical, from the start and after a seek,
#   - with --index, the .vix file is byte-identical.
# Files the reference refuses, the C code refuses too.
#
#   python3 -I tests/video/mp4demux_c_test.py BINARY
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

import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mp4demux  # noqa: E402
import mp4demux_test as mt  # noqa: E402

OUT = os.path.join(mt.ROOT, 'build', 'video', 'mp4c')
# the smallest is the iterator's minimum (six 64-byte windows) plus one
# index record, so windows refill every 16 entries and records go out
# one at a time
SCRATCH = [396, 6 * 1024, 1 << 20]
fails = 0


def check(cond, what):
    global fails
    print('%s %s' % ('ok  ' if cond else 'FAIL', what))
    if not cond:
        fails += 1


def read(path):
    with open(path, 'rb') as f:
        return f.read()


def run_py(src, args):
    return subprocess.run([sys.executable, '-I', os.path.join(HERE, 'mp4demux.py'), src] + args,
                          capture_output=True)


def run_c(binary, src, args):
    return subprocess.run([binary, src] + args, capture_output=True)


def compare(binary, src, index=True):
    tag = os.path.basename(src)
    m = mp4demux.Movie(memoryview(read(src)))
    mid = m.samples[len(m.samples) // 2][2]
    for seek in (None, mid):
        sa = ['--seek-us', str(seek)] if seek is not None else []
        py = run_py(src, ['--table', OUT + '/py.csv', '--annexb', OUT + '/py.264'] + sa +
                    (['--index', OUT + '/py.vix'] if index else []))
        if py.returncode:
            check(False, '%s: reference failed: %s' % (tag, py.stderr.decode().strip()))
            return
        for sc in SCRATCH:
            c = run_c(binary, src, ['--table', OUT + '/c.csv', '--annexb', OUT + '/c.264',
                                    '--scratch', str(sc)] + sa +
                      (['--index', OUT + '/c.vix'] if index else []))
            what = '%s%s, scratch %d' % (tag, ', seek %d us' % seek if seek is not None else '', sc)
            if c.returncode:
                check(False, '%s: C failed: %s' % (what, c.stderr.decode().strip()))
                continue
            same = [read(OUT + '/py.csv') == read(OUT + '/c.csv'),
                    read(OUT + '/py.264') == read(OUT + '/c.264')]
            if index:
                same.append(read(OUT + '/py.vix') == read(OUT + '/c.vix'))
            check(all(same), '%s: table, Annex B%s identical (%s)' % (
                what, ', .vix' if index else '', c.stdout.decode().strip().split('; ')[-1]))
        if index and seek is None:
            # the library update's way: vix_build_file() into a directory
            d = os.path.join(OUT, 'idx')
            c = run_c(binary, src, ['--build', d])
            line = c.stdout.decode().strip().split('\n')[-1]
            built = line.split(' ')[1].rstrip(',') if line.startswith('built ') else None
            check(c.returncode == 0 and built and read(built) == read(OUT + '/py.vix')
                  and not os.path.exists(built + '.tmp'),
                  '%s: vix_build_file() writes the same index (%s)' % (
                      tag, line.replace(OUT + '/', '')))
            c = run_c(binary, '--check-index', [OUT + '/py.vix', src])
            check(c.returncode == 0, '%s: read back: %s' % (
                tag, (c.stdout.decode().strip() or c.stderr.decode().strip()).split('\n')[-1]
                .replace(OUT + '/', '')))


# --- rewritten variants: the moov of a moov-last file, mdat untouched ------

def rewrite(src, dst, fn):
    """fn(path, typ, body) -> body, None to drop, or (body, [extra boxes])."""
    buf = bytearray(read(src))
    top = list(mp4demux.boxes(buf, 0, len(buf)))
    mv = [b for b in top if b[0] == b'moov'][0]
    assert [b for b in top if b[0] == b'mdat'][0][1] < mv[1], 'needs moov last'
    video = [None]

    def walk(s, e, path):
        out = b''
        for typ, ps, pe in mp4demux.boxes(buf, s, e):
            p = path + [typ]
            if typ == b'trak':
                hd = mp4demux.find(buf, ps, pe, [b'mdia', b'hdlr'])
                is_v = bytes(buf[hd[0] + 8:hd[0] + 12]) == b'vide'
                video[0] = is_v
            if typ in mp4demux.CONTAINERS:
                body = walk(ps, pe, p)
            else:
                body = bytes(buf[ps:pe])
            extra = []
            if video[0] and b'trak' in p:
                r = fn(p, typ, body)
                if r is None:
                    continue
                if isinstance(r, tuple):
                    body, extra = r
                else:
                    body = r
            out += struct.pack('>I4s', 8 + len(body), typ) + body
            for t, b in extra:
                out += struct.pack('>I4s', 8 + len(b), t) + b
        return out

    moov = walk(mv[1], mv[2], [])
    with open(dst, 'wb') as f:
        f.write(bytes(buf[:mv[1] - 8]) + struct.pack('>I4s', 8 + len(moov), b'moov') + moov)
    return dst


def stsc_entries(body):
    n = struct.unpack_from('>I', body, 4)[0]
    return [struct.unpack_from('>III', body, 8 + 12 * i) for i in range(n)]


def variants(late, n_chunks_hint=None):
    out = []
    name = os.path.splitext(os.path.basename(late))[0]

    def v(tag, fn, index=True):
        out.append((rewrite(late, os.path.join(OUT, '%s_%s.mp4' % (name, tag)), fn), index))

    # an edit shift: the first edit's media_time (replaces or adds elst).
    # The first PTS goes negative (floored, like the reference's //), which
    # the index cannot hold: both refuse --index, so it is compared without
    def shift(p, typ, body):
        elst = struct.pack('>IIIii', 0, 1, 0, 1234, 0x10000)
        if typ == b'edts':
            return struct.pack('>I4s', 8 + len(elst), b'elst') + elst
        if typ == b'tkhd' and not has_edts[0]:
            return body, [(b'edts', struct.pack('>I4s', 8 + len(elst), b'elst') + elst)]
        return body
    has_edts = [mp4demux.find(read(late), 0, len(read(late)), [b'moov', b'trak', b'edts']) is not None]
    v('shift', shift, index=False)

    # ctts version 1 with negative offsets (PTS no longer monotonic: the
    # reference and the C code must still agree sample for sample)
    def ctts(p, typ, body):
        if typ == b'stts':
            n = sum(struct.unpack_from('>I', body, 8 + 8 * i)[0]
                    for i in range(struct.unpack_from('>I', body, 4)[0]))
            runs = [(1, 4096), (1, -512), (2, 0)] * (n // 4) + [(1, 0)] * (n % 4)
            ct = struct.pack('>II', 1 << 24, len(runs)) + b''.join(
                struct.pack('>Ii', c, o) for c, o in runs)
            return body, [(b'ctts', ct)]
        if typ == b'ctts':
            return None
        return body
    v('ctts1', ctts)

    # no stss: every sample is a key sample
    v('nostss', lambda p, typ, body: None if typ == b'stss' else body)

    # stsc as one entry per chunk, and with chunk 1 holding no samples
    # (the first entry starts at chunk 2, its samples moved one chunk on)
    def per_chunk(p, typ, body):
        if typ != b'stsc':
            return body
        e = stsc_entries(body)
        nch = n_chunks[0]
        rows = []
        for i, (first, per, d) in enumerate(e):
            last = e[i + 1][0] - 1 if i + 1 < len(e) else nch
            rows += [(c, per, d) for c in range(first, last + 1)]
        return struct.pack('>II', 0, len(rows)) + b''.join(struct.pack('>III', *r) for r in rows)
    n_chunks = [None]
    st = mp4demux.Movie(memoryview(read(late))).video.stbl
    buf = read(late)
    key = b'stco' if b'stco' in st else b'co64'
    n_chunks[0] = struct.unpack_from('>I', buf, st[key][0] + 4)[0]
    v('stscall', per_chunk)

    def skip_first(p, typ, body):
        if typ in (b'stco', b'co64'):
            n = struct.unpack_from('>I', body, 4)[0]
            w = 8 if typ == b'co64' else 4
            first = body[8:8 + w]       # a duplicate chunk 1 that holds nothing
            return body[:4] + struct.pack('>I', n + 1) + first + body[8:]
        if typ == b'stsc':
            e = stsc_entries(body)
            return struct.pack('>II', 0, len(e)) + b''.join(
                struct.pack('>III', f + 1, per, d) for f, per, d in e)
        return body
    v('skip1', skip_first)
    return out


def refusals(binary):
    good = None
    for f in os.listdir(os.path.join(mt.ROOT, 'build', 'video', 'mp4')):
        if f.endswith('.mp4') and 'co64' not in f:
            good = os.path.join(mt.ROOT, 'build', 'video', 'mp4', f)
            break
    cases = {
        'fragmented': b'\0\0\0\x08ftyp' + b'\0\0\0\x08moof',
        'no moov': b'\0\0\0\x08ftyp' + b'\0\0\0\x10mdat' + b'\0' * 8,
        'truncated box': b'\0\0\0\x08ftyp' + b'\0\0\x10\x00moov' + b'\0' * 8,
    }
    for tag, data in cases.items():
        path = os.path.join(OUT, 'bad.mp4')
        with open(path, 'wb') as f:
            f.write(data)
        py = run_py(path, [])
        c = run_c(binary, path, [])
        check(py.returncode != 0 and c.returncode != 0, 'refused by both: %s (%s)' % (
            tag, c.stderr.decode().strip()))
    if good:
        def empty(p, typ, body):
            elst = struct.pack('>IIIii', 0, 1, 0, -1, 0x10000)
            if typ == b'edts':
                return struct.pack('>I4s', 8 + len(elst), b'elst') + elst
            if typ == b'tkhd':
                return body, ([] if had[0] else [(b'edts', struct.pack('>I4s', 8 + len(elst), b'elst') + elst)])
            return body
        had = [mp4demux.find(read(good), 0, len(read(good)), [b'moov', b'trak', b'edts']) is not None]
        path = rewrite(good, os.path.join(OUT, 'emptyedit.mp4'), empty)
        py, c = run_py(path, []), run_c(binary, path, [])
        check(py.returncode != 0 and c.returncode != 0, 'refused by both: empty edit (%s)' %
              c.stderr.decode().strip())
        noavc = rewrite(good, os.path.join(OUT, 'noavcc.mp4'),
                        lambda p, typ, body: body.replace(b'avcC', b'avcX') if typ == b'stsd' else body)
        py, c = run_py(noavc, []), run_c(binary, noavc, [])
        check(py.returncode != 0 and c.returncode != 0, 'refused by both: no avcC (%s)' %
              c.stderr.decode().strip())


def main():
    binary = sys.argv[1]
    if not all(os.path.exists(os.path.join(mt.CORPUS, c + '.264')) for c in mt.CLIPS):
        subprocess.run([os.path.join(HERE, 'make_corpus.sh')], check=True, cwd=mt.ROOT)
    os.makedirs(mt.OUT, exist_ok=True)
    os.makedirs(OUT, exist_ok=True)
    index = '--no-index' not in sys.argv
    for clip in mt.CLIPS:
        w, h = mt.size_of(clip)
        frames = len(mt.ref_md5s(clip, w, h))
        faststart = mt.mux(clip, clip + '.m4v', ['-movflags', '+faststart'], frames)
        late = mt.mux(clip, clip + '.mp4', [], frames)
        compare(binary, faststart, index)
        compare(binary, late, index)
        if clip in ('d_ipod_768k', 'b_ippp_gop10'):
            compare(binary, mt.to_co64(late, os.path.join(OUT, clip + '_co64.mp4')), index)
            for p, ix in variants(late):
                compare(binary, p, index and ix)
                if index and not ix:
                    py = run_py(p, ['--index', OUT + '/py.vix'])
                    c = run_c(binary, p, ['--index', OUT + '/c.vix'])
                    check(py.returncode != 0 and c.returncode != 0,
                          '%s: a negative PTS: --index refused by both' % os.path.basename(p))
    refusals(binary)
    print('mp4demux C: %d failures' % fails)
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
