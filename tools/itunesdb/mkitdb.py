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

"""Minimal iTunesDB writer for an iPod Video 5G/5.5G (Apple firmware 1.3).
Test tool only: one master playlist, audio tracks, no artwork. Stdlib only.
usage: mkitdb.py MOUNT SRC_ALBUM_DIR [--dry]
Copies the album's MP3s to iPod_Control/Music/F00 and writes iPod_Control/iTunes/iTunesDB
(refuses if an iTunesDB already exists)."""
import os, sys, struct, shutil, time, random

MAC_EPOCH = 2082844800
FIXED = {}   # golden mode: fixed time and ids (tests/applefb)
def now_mac(): return FIXED.get("ts", int(time.time()) + MAC_EPOCH)
def rand64(what): return FIXED[what] if what in FIXED else random.getrandbits(64)

# ---- MP3 parsing (ID3v2 text frames, first frame header, Xing) ----
BR = {1: [0,32,40,48,56,64,80,96,112,128,160,192,224,256,320],   # MPEG1 L3
      2: [0,8,16,24,32,40,48,56,64,80,96,112,128,144,160]}       # MPEG2/2.5 L3
SR = {3: [44100,48000,32000], 2: [22050,24000,16000], 0: [11025,12000,8000]}

def synchsafe(b): return (b[0]<<21)|(b[1]<<14)|(b[2]<<7)|b[3]

def id3_text(data, fid):
    if data[:3] != b'ID3': return None
    ver = data[3]; size = synchsafe(data[6:10]); p = 10; end = 10 + size
    while p + 10 <= end:
        f = data[p:p+4]
        n = synchsafe(data[p+4:p+8]) if ver == 4 else struct.unpack('>I', data[p+4:p+8])[0]
        if not f.strip(b'\0') or n <= 0: break
        if f == fid.encode():
            body = data[p+10:p+10+n]; enc = body[0]; t = body[1:]
            if enc == 0: s = t.decode('latin-1')
            elif enc == 1: s = t.decode('utf-16')
            elif enc == 2: s = t.decode('utf-16-be')
            else: s = t.decode('utf-8', 'replace')
            return s.split('\0')[0].strip()
        p += 10 + n
    return None

def mp3_info(path):
    data = open(path, 'rb').read()
    start = 10 + synchsafe(data[6:10]) if data[:3] == b'ID3' else 0
    p = start
    while p < len(data) - 4:
        if data[p] == 0xFF and (data[p+1] & 0xE0) == 0xE0:
            ver = (data[p+1] >> 3) & 3; layer = (data[p+1] >> 1) & 3
            bri = data[p+2] >> 4; sri = (data[p+2] >> 2) & 3
            if layer == 1 and bri not in (0, 15) and sri != 3 and ver != 1:
                break
        p += 1
    else:
        raise ValueError('no MPEG frame: ' + path)
    ver = (data[p+1] >> 3) & 3; bri = data[p+2] >> 4; sri = (data[p+2] >> 2) & 3
    mono = (data[p+3] >> 6) == 3
    kbps = BR[1 if ver == 3 else 2][bri]; sr = SR[ver][sri]
    spf = 1152 if ver == 3 else 576
    side = (17 if mono else 32) if ver == 3 else (9 if mono else 17)
    x = p + 4 + side; vbr = False
    if data[x:x+4] in (b'Xing', b'Info') and struct.unpack('>I', data[x+4:x+8])[0] & 1:
        frames = struct.unpack('>I', data[x+8:x+12])[0]
        ms = int(frames * spf * 1000 / sr); vbr = data[x:x+4] == b'Xing'
        if vbr: kbps = int((len(data) - p) * 8 / ms) if ms else kbps
    else:
        ms = int((len(data) - p) * 8 / kbps) if kbps else 0
    def t(f): return id3_text(data, f)
    trk = t('TRCK') or '0'
    return dict(size=len(data), ms=ms, kbps=kbps, sr=sr, vbr=vbr,
                title=t('TIT2') or os.path.splitext(os.path.basename(path))[0],
                artist=t('TPE1') or '', album=t('TALB') or '', genre=t('TCON') or '',
                track=int(trk.split('/')[0] or 0) if trk.split('/')[0].isdigit() else 0,
                year=int((t('TYER') or t('TDRC') or '0')[:4] or 0) if (t('TYER') or t('TDRC') or '0')[:4].isdigit() else 0)

def ff_info(path):
    import subprocess, json as _j
    out = subprocess.run(['ffprobe', '-v', 'quiet', '-print_format', 'json', '-show_format', '-show_streams', path],
                         capture_output=True, text=True, check=True).stdout
    d = _j.loads(out); f = d['format']; tags = {k.lower(): v for k, v in f.get('tags', {}).items()}
    a = next(st for st in d['streams'] if st.get('codec_type') == 'audio')
    trk = (tags.get('track') or '0').split('/')[0]
    yr = (tags.get('date') or '0')[:4]
    return dict(size=os.path.getsize(path), ms=int(float(f['duration']) * 1000),
                kbps=int(int(f.get('bit_rate', '0')) / 1000), sr=int(a.get('sample_rate', 44100)), vbr=False,
                title=tags.get('title') or os.path.splitext(os.path.basename(path))[0],
                artist=tags.get('artist', ''), album=tags.get('album', ''), genre=tags.get('genre', ''),
                track=int(trk) if trk.isdigit() else 0, year=int(yr) if yr.isdigit() else 0)

# ---- chunk builders (little-endian) ----
def pad(b, n): return b + b'\0' * (n - len(b))

def mhod_str(typ, s):
    u = s.encode('utf-16-le')
    body = struct.pack('<IIII', 1, len(u), 0, 0) + u
    return struct.pack('<4sIIIII', b'mhod', 0x18, 0x18 + len(body), typ, 0, 0) + body

def mhod_pos(pos):
    return pad(struct.pack('<4sIIIIII', b'mhod', 0x18, 0x2c, 100, 0, 0, pos), 0x2c)

def mhit(tid, dbid, t, location):
    mh = [mhod_str(1, t['title']), mhod_str(2, location), mhod_str(6, t.get('kind', 'MPEG audio file'))]
    if t['album']: mh.append(mhod_str(3, t['album']))
    if t['artist']: mh.append(mhod_str(4, t['artist']))
    if t['genre']: mh.append(mhod_str(5, t['genre']))
    body = b''.join(mh)
    h = bytearray(0x184)
    struct.pack_into('<4sIIIII', h, 0, b'mhit', 0x184, 0x184 + len(body), len(mh), tid, 1)
    struct.pack_into('<4s', h, 24, t.get('ftype', b' 3PM'))   # filetype as LE u32 ('MP3 ', 'M4B ')
    if t.get('ftype', b' 3PM') == b' 3PM':
        h[28] = 1 if t['vbr'] else 0; h[29] = 1          # type1 (VBR), type2 (MP3)
    ts = now_mac()
    struct.pack_into('<IIIIIIII', h, 32, ts, t['size'], t['ms'], t['track'], 0, t['year'], t['kbps'], t['sr'] << 16)
    struct.pack_into('<I', h, 104, ts)                   # date added
    struct.pack_into('<Q', h, 112, dbid)                 # dbid
    struct.pack_into('<I', h, 0xd0, t.get('media', 1))   # mediatype: 1 audio, 8 audiobook
    return bytes(h) + body

def mhsd(typ, child):
    return struct.pack('<4sIII', b'mhsd', 0x60, 0x60 + len(child), typ).ljust(0x60, b'\0') + child

def build(tracks, name):
    items = b''.join(mhit(t['id'], t['dbid'], t, t['loc']) for t in tracks)
    mhlt = struct.pack('<4sII', b'mhlt', 0x5c, len(tracks)).ljust(0x5c, b'\0') + items
    mhips = b''
    for i, t in enumerate(tracks):
        ip = struct.pack('<4sIIIIIII', b'mhip', 0x4c, 0x4c + 0x2c, 1, 0, 0, t['id'], now_mac()).ljust(0x4c, b'\0')
        mhips += ip + mhod_pos(i + 1)
    title = mhod_str(1, name)
    hy = bytearray(0x6c)
    struct.pack_into('<4sIIIII', hy, 0, b'mhyp', 0x6c, 0x6c + len(title) + len(mhips), 1, len(tracks), 1)
    struct.pack_into("<IQ", hy, 24, now_mac(), rand64("pl_id"))
    mhyp = bytes(hy) + title + mhips
    mhlp = struct.pack('<4sII', b'mhlp', 0x5c, 1).ljust(0x5c, b'\0') + mhyp
    sets = mhsd(1, mhlt) + mhsd(2, mhlp)
    hb = bytearray(0x68)
    struct.pack_into('<4sIIIII', hb, 0, b'mhbd', 0x68, 0x68 + len(sets), 1, 0x13, 2)
    struct.pack_into("<Q", hb, 24, rand64("db_id"))
    struct.pack_into('<H', hb, 32, 2)
    return bytes(hb) + sets

# ---- verify by re-parse ----
def verify(db, n):
    assert db[:4] == b'mhbd' and struct.unpack_from('<I', db, 8)[0] == len(db)
    def walk(off, end, depth=0):
        while off < end:
            tag = db[off:off+4]; hl, tl = struct.unpack_from('<II', db, off + 4)
            assert tag in (b'mhsd', b'mhlt', b'mhit', b'mhod', b'mhlp', b'mhyp', b'mhip'), (tag, off)
            if tag in (b'mhlt', b'mhlp'):          # lists: header length only, children follow
                off += hl; continue
            assert off + tl <= end, (tag, off, tl, end)
            if tag != b'mhod': walk(off + hl, off + tl, depth + 1)
            off += tl
    walk(0x68, len(db))
    assert db.count(b'mhit') == n and db.count(b'mhip') == n
    return True

def to_loc(mount, path):
    rel = os.path.relpath(path, mount)
    assert not rel.startswith('..'), path
    return ':' + rel.replace('/', ':')

def add(tracks, path, mount, book=False):
    low = path.lower()
    if low.endswith('.mp3'):
        t = mp3_info(path)
    else:
        t = ff_info(path)
        t['ftype'] = b' B4M' if low.endswith('.m4b') else b' A4M'
        t['kind'] = 'AAC audio file'
    if book: t['media'] = 8
    t['id'] = 100 + len(tracks); t['dbid'] = random.getrandbits(64); t['loc'] = to_loc(mount, path)
    tracks.append(t)
    print('%-5s %-38.38s %-20.20s %7d ms  %s' % ('BOOK' if book else '', t['title'], t['artist'], t['ms'], t['loc']))

def golden(spec_path, out_path):
    """The reference for the firmware's C writer (apps/ipodui/afb_core.c,
    tests/applefb): a JSON spec fixes the time, the ids and each track's
    tags, so both writers must produce the same bytes."""
    import json
    spec = json.load(open(spec_path, encoding='utf-8'))
    FIXED.update(ts=spec['ts'], db_id=spec['db_id'], pl_id=spec['pl_id'])
    tracks = []
    for i, t in enumerate(spec['tracks']):
        k = t.get('kind', 'mp3')
        d = dict(size=t['size'], ms=t['ms'], kbps=t['kbps'], sr=t['sr'], vbr=bool(t.get('vbr', 0)),
                 title=t['title'], artist=t.get('artist', ''), album=t.get('album', ''),
                 genre=t.get('genre', ''), track=t.get('track', 0), year=t.get('year', 0),
                 id=spec['first_id'] + i, dbid=t['dbid'], loc=t['loc'])
        if k != 'mp3':
            d['ftype'] = {'m4a': b' A4M', 'm4b': b' B4M', 'wav': b' VAW', 'aif': b'FFIA'}[k]
            d['kind'] = {'m4a': 'AAC audio file', 'm4b': 'AAC audio file',
                         'wav': 'WAV audio file', 'aif': 'AIFF audio file'}[k]
        if k == 'm4b': d['media'] = 8
        tracks.append(d)
    db = build(tracks, spec.get('name', 'ProkBox'))
    verify(db, len(tracks))
    open(out_path, 'wb').write(db)
    print('golden %d bytes, %d tracks' % (len(db), len(tracks)))

def main():
    # usage: mkitdb.py MOUNT [--dry] [--replace] [--album DIR]... [--book FILE]...
    #        mkitdb.py --golden SPEC.json OUT
    if sys.argv[1:2] == ['--golden']:
        return golden(sys.argv[2], sys.argv[3])
    a = sys.argv[1:]; mount = a[0]; dry = '--dry' in a; replace = '--replace' in a
    tracks = []
    for i, x in enumerate(a):
        if x == '--album':
            d = a[i + 1]
            for f in sorted(os.listdir(d)):
                if f.lower().endswith(('.mp3', '.m4a')) and not f.startswith('._'):
                    add(tracks, os.path.join(d, f), mount)
        if x == '--book':
            add(tracks, a[i + 1], mount, book=True)
    db = build(tracks, 'ProkBox')
    verify(db, len(tracks))
    print('db %d bytes, %d tracks, verified' % (len(db), len(tracks)))
    if dry: open('iTunesDB.dry', 'wb').write(db); return
    dbp = os.path.join(mount, 'iPod_Control', 'iTunes', 'iTunesDB')
    if os.path.exists(dbp):
        if not replace: sys.exit('refusing: iTunesDB exists (use --replace)')
        shutil.copyfile(dbp, dbp + '.bak')
    tmp = dbp + '.new'
    with open(tmp, 'wb') as fh: fh.write(db); fh.flush(); os.fsync(fh.fileno())
    assert open(tmp, 'rb').read() == db
    os.replace(tmp, dbp)
    print('written', dbp)

if __name__ == '__main__': main()
