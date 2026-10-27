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
# Checks tests/video/mp4demux.py against ffprobe and ffmpeg (#60, #64).
# Muxes corpus clips (tests/video/make_corpus.sh) with a synthetic AAC track
# into MP4/M4V the way iPod-era files are laid out (moov first, moov last,
# co64), then for each file:
#   - the sample table (offset, size, PTS, key) equals ffprobe's packets,
#   - the Annex B stream it writes decodes, with ffmpeg, to the exact frames
#     of the corpus reference (.yuv),
#   - a seek from the middle starts on a key sample and decodes to the
#     reference's frames from there,
#   - the sidecar index round-trips.
# Media stays under build/video/mp4 (never committed).
#
#   python3 -I tests/video/mp4demux_test.py
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

import hashlib
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import mp4demux  # noqa: E402

CORPUS = os.path.join(ROOT, 'build', 'video', 'corpus')
OUT = os.path.join(ROOT, 'build', 'video', 'mp4')
CLIPS = ['d_ipod_768k', 'b_ippp_gop10', 'g_crop', 'e_vga', 'f_slices']
FPS = 30

fails = 0


def check(cond, what):
    global fails
    print('%s %s' % ('ok  ' if cond else 'FAIL', what))
    if not cond:
        fails += 1


def run(*args):
    return subprocess.run(args, check=True, capture_output=True).stdout


def size_of(clip):
    out = run('ffprobe', '-v', 'error', '-select_streams', 'v', '-show_entries',
              'stream=width,height', '-of', 'csv=p=0',
              os.path.join(CORPUS, clip + '.264')).decode().strip()
    w, h = out.split(',')[:2]
    return int(w), int(h)


def mux(clip, name, extra, frames):
    """The clip plus a synthetic AAC track as long as the video, interleaved
    the way ffmpeg (and iTunes) write them."""
    src = os.path.join(CORPUS, clip + '.264')
    dst = os.path.join(OUT, name)
    if not os.path.exists(dst) or os.path.getmtime(dst) < os.path.getmtime(src):
        run('ffmpeg', '-v', 'error', '-y', '-framerate', str(FPS), '-i', src,
            '-f', 'lavfi', '-t', '%.4f' % (frames / FPS),
            '-i', 'sine=frequency=440:sample_rate=44100',
            '-map', '0:v', '-map', '1:a', '-c:v', 'copy',
            '-c:a', 'aac', '-b:a', '128k', *extra, dst)
    return dst


def to_co64(path, dst):
    """Rewrite a moov-last file's stco boxes as co64 (mdat offsets unchanged)."""
    buf = bytearray(open(path, 'rb').read())
    top = list(mp4demux.boxes(buf, 0, len(buf)))
    mv = [b for b in top if b[0] == b'moov'][0]
    md = [b for b in top if b[0] == b'mdat'][0]
    assert md[1] < mv[1], 'needs moov after mdat'

    def rebuild(s, e):
        out = b''
        for typ, ps, pe in mp4demux.boxes(buf, s, e):
            if typ in mp4demux.CONTAINERS:
                body = rebuild(ps, pe)
            elif typ == b'stco':
                n = struct.unpack_from('>I', buf, ps + 4)[0]
                offs = struct.unpack_from('>%dI' % n, buf, ps + 8)
                body = bytes(buf[ps:ps + 8]) + struct.pack('>%dQ' % n, *offs)
                typ = b'co64'
            else:
                body = bytes(buf[ps:pe])
            out += struct.pack('>I4s', 8 + len(body), typ) + body
        return out

    moov = rebuild(mv[1], mv[2])
    data = bytes(buf[:mv[1] - 8]) + struct.pack('>I4s', 8 + len(moov), b'moov') + moov
    open(dst, 'wb').write(data)
    return dst


def ffprobe_packets(path):
    out = run('ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_entries',
              'packet=pts_time,pos,size,flags', '-of', 'csv=p=0', path).decode()
    pk = []
    for line in out.split():
        f = line.split(',')
        pts, size, pos, flags = f[0], f[1], f[2], f[3]   # ffprobe's order
        pk.append((int(pos), int(size), round(float(pts) * 1e6), 'K' in flags))
    return pk


def decode_md5s(stream, w, h):
    raw = subprocess.run(['ffmpeg', '-v', 'error', '-f', 'h264', '-i', '-',
                          '-f', 'rawvideo', '-pix_fmt', 'yuv420p', '-'],
                         input=stream, check=True, capture_output=True).stdout
    fs = w * h * 3 // 2
    return [hashlib.md5(raw[i:i + fs]).hexdigest() for i in range(0, len(raw), fs)]


def ref_md5s(clip, w, h):
    raw = open(os.path.join(CORPUS, clip + '.yuv'), 'rb').read()
    fs = w * h * 3 // 2
    return [hashlib.md5(raw[i:i + fs]).hexdigest() for i in range(0, len(raw), fs)]


def test_file(clip, path, w, h):
    tag = os.path.basename(path)
    m = mp4demux.Movie(memoryview(open(path, 'rb').read()))
    v = m.video
    check((v.width, v.height) == (w, h) and m.audio is not None,
          '%s: avc1 %dx%d, audio track present' % (tag, v.width, v.height))
    probe = ffprobe_packets(path)
    ours = [(o, s, pts, k) for o, s, pts, k in m.samples]
    same = len(probe) == len(ours) and all(
        a[0] == b[0] and a[1] == b[1] and abs(a[2] - b[2]) <= 1 and a[3] == b[3]
        for a, b in zip(ours, probe))
    if not same:
        bad = next((i for i, (a, b) in enumerate(zip(ours, probe)) if a != b), None)
        print('     first difference at %s: ours %s ffprobe %s' % (
            bad, ours[bad] if bad is not None else '-', probe[bad] if bad is not None else '-'))
    check(same, '%s: %d samples (offset, size, PTS us, key) match ffprobe' % (tag, len(ours)))
    ref = ref_md5s(clip, w, h)
    stream = b''.join(m.packet(i, with_params=(i == 0)) for i in range(len(m.samples)))
    got = decode_md5s(stream, w, h)
    check(got == ref, '%s: Annex B decodes to the reference, %d/%d frames' % (
        tag, sum(a == b for a, b in zip(got, ref)), len(ref)))
    mid = m.samples[len(m.samples) // 2][2]
    k = m.key_before(mid)
    ok = m.samples[k][3] and m.samples[k][2] <= mid
    seek = b''.join(m.packet(i, with_params=(i == k)) for i in range(k, len(m.samples)))
    got = decode_md5s(seek, w, h)
    check(ok and got == ref[k:], '%s: seek to %d us starts at key sample %d, %d frames exact' % (
        tag, mid, k, len(got)))
    ix = os.path.join(OUT, tag + '.vix')
    n = mp4demux.write_index(m, ix, 0)
    raw = open(ix, 'rb').read()
    magic, ver, cnt, unit = struct.unpack_from('<4sIII', raw, 0)
    back = []
    for i in range(cnt):
        lo, sf, ms = struct.unpack_from('<III', raw, 32 + 12 * i)
        fl = sf >> 24
        back.append((lo | (fl >> 4) << 32, sf & 0xffffff, ms, bool(fl & 1)))
    want = [(o, s, pts // 1000, kk) for o, s, pts, kk in m.samples]
    check(magic == b'VIX1' and n == 32 + 12 * cnt and back == want,
          '%s: index %d bytes (12 per sample) round-trips' % (tag, n))


def test_errors():
    try:
        mp4demux.Movie(memoryview(b'\0\0\0\x08ftyp' + b'\0\0\0\x08moof'))
        check(False, 'fragmented file refused')
    except mp4demux.DemuxError as e:
        check('fragmented' in str(e), 'fragmented file refused (%s)' % e)
    lsize, sps, pps = mp4demux.parse_avcc(bytes([1, 66, 0, 30, 0xfd, 0xe1, 0, 2, 0x67, 1, 1, 0, 1, 0x68]))
    check((lsize, sps, pps) == (2, [b'\x67\x01'], [b'\x68']), 'avcC with 2-byte NAL lengths parsed')


def main():
    if not all(os.path.exists(os.path.join(CORPUS, c + '.264')) for c in CLIPS):
        subprocess.run([os.path.join(HERE, 'make_corpus.sh')], check=True, cwd=ROOT)
    os.makedirs(OUT, exist_ok=True)
    for clip in CLIPS:
        w, h = size_of(clip)
        frames = len(ref_md5s(clip, w, h))
        faststart = mux(clip, clip + '.m4v', ['-movflags', '+faststart'], frames)
        late = mux(clip, clip + '.mp4', [], frames)
        test_file(clip, faststart, w, h)
        test_file(clip, late, w, h)
        if clip == 'd_ipod_768k':
            test_file(clip, to_co64(late, os.path.join(OUT, clip + '_co64.mp4')), w, h)
    test_errors()
    print('mp4demux: %d failures' % fails)
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
