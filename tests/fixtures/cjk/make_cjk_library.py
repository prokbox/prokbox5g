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

"""A library tagged in Japanese, Chinese and Korean (#235, package G).

    tests/fixtures/cjk/make_cjk_library.py <outdir> [tracks]

Without 'tracks': the shot fixture, 24 songs in 8 albums (Japanese kana
and kanji, Simplified and Traditional Chinese, Korean Hangul, one Latin
album to sit beside them), names as real libraries carry them, with
full-width forms and a CP932 name kanji (髙). Written as
<outdir>/<n>.mp3, flat, so a script copies them with one line each.

With 'tracks' (e.g. 10000): the timing library, <outdir>/<artist>/
<album>/<n>.mp3, 10 songs an album, 40 an artist, names drawn from the
same pools (3,000+ distinct glyphs across the set). Deterministic;
idempotent (a stamp holds the count).

ID3v2.3 with UTF-16 text frames (what iTunes and MusicBee write for CJK
text), behind the frames of tests/ui/fixtures/refresh.mp3 (1.07 s): once
per timing song, SHOT_REPEAT times (about 32 s) per shot song, so a Now
Playing shot taken seconds after SELECT still shows the song selected,
not one the queue moved on to (p090-playfix: the 1 s songs read as a
wrong-song bug)."""
import os
import random
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))
FIXTURE = os.path.join(ROOT, 'tests', 'ui', 'fixtures', 'refresh.mp3')

# (album artist, album, [titles])
SHOT = [
    ('宇多田ヒカル', '初恋', ['あなた', '誓い', '大空で抱きしめて']),
    ('椎名林檎', '無罪モラトリアム', ['正しい街', '歌舞伎町の女王', 'ここでキスして。']),
    ('髙橋優', 'ＢＥＳＴ　ＡＬＢＵＭ', ['福笑い', '明日はきっといい日になる', '虹']),
    ('ゆず', 'ゆずえん', ['夏色', '少年', 'からっぽ']),
    ('周杰伦', '叶惠美', ['晴天', '以父之名', '东风破']),
    ('張學友', '吻別', ['吻別', '一千個傷心的理由', '分手總要在雨天']),
    ('아이유', '꽃갈피', ['나의 옛날이야기', '꽃', '사랑이 지나가면']),
    ('Aurora Lane', 'Northern Songs', ['Amber Sky', 'Glacier', 'Winter Light']),
]

KANA = [chr(c) for c in range(0x3041, 0x3094)] + \
       [chr(c) for c in range(0x30A1, 0x30F5)]
HANGUL = [chr(0xAC00 + i) for i in range(0, 11172, 7)]
KANJI_TEXT = ('日本語中文漢字東京大阪京都名古屋北海道沖縄海山川空雨雪風花月星光'
              '夜朝昼春夏秋冬愛恋心夢歌声音楽曲世界時代未来過去今日明日昨夜'
              '少年少女青春物語旅道街家族友達人生命運希望涙笑顔約束記憶永遠'
              '銀河宇宙地球太陽火水木金土天国神様王子姫騎士魔法剣龍虎鳥魚森')


def frame(fid, text):
    data = b'\x01' + text.encode('utf-16')          # UTF-16 with BOM
    return fid.encode() + struct.pack('>I', len(data)) + b'\0\0' + data


def id3(tags):
    body = b''.join(frame(k, v) for k, v in tags if v)
    n = len(body)
    size = bytes([(n >> 21) & 127, (n >> 14) & 127, (n >> 7) & 127, n & 127])
    return b'ID3\x03\x00\x00' + size + body


def audio_frames():
    d = open(FIXTURE, 'rb').read()
    if d[:3] == b'ID3':
        n = (d[6] << 21) | (d[7] << 14) | (d[8] << 7) | d[9]
        d = d[10 + n:]
    return d


SHOT_REPEAT = 30


def write(path, artist, album, title, track, repeat=1):
    with open(path, 'wb') as f:
        f.write(id3([('TIT2', title), ('TPE1', artist), ('TALB', album),
                     ('TPE2', artist), ('TRCK', str(track)), ('TPOS', '1'),
                     ('TCON', 'Pop')]))
        f.write(AUDIO * repeat)


def name(rng):
    k = rng.random()
    n = rng.choice((2, 3, 4, 5, 6))
    if k < 0.45:
        pool = KANJI_TEXT if rng.random() < 0.5 else KANA
        return ''.join(rng.choice(pool) for _ in range(n))
    if k < 0.75:
        return ''.join(rng.choice(KANA) for _ in range(n))
    return ''.join(rng.choice(HANGUL) for _ in range(n))


def main():
    global AUDIO
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    out = sys.argv[1]
    total = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    stamp = os.path.join(out, '.cjk-library')
    want = '%d v1' % total if total else '0 v2 x%d' % SHOT_REPEAT
    if os.path.exists(stamp) and open(stamp).read().strip() == want:
        return
    os.makedirs(out, exist_ok=True)
    AUDIO = audio_frames()
    if not total:
        n = 0
        for artist, album, titles in SHOT:
            for t, title in enumerate(titles, 1):
                n += 1
                write(os.path.join(out, '%02d.mp3' % n), artist, album,
                      title, t, SHOT_REPEAT)
    else:
        rng = random.Random(235)
        artists = [name(rng) for _ in range(total // 40 + 1)]
        n = album = 0
        while n < total:
            artist = artists[album // 4]
            title_album = name(rng)
            folder = os.path.join(out, 'a%04d' % (album // 4), 'b%05d' % album)
            os.makedirs(folder, exist_ok=True)
            for t in range(1, 11):
                if n >= total:
                    break
                n += 1
                write(os.path.join(folder, '%02d.mp3' % t), artist,
                      title_album, name(rng), t)
            album += 1
    open(stamp, 'w').write(want + '\n')
    print('cjk library: %d songs in %s' % (n, out))


if __name__ == '__main__':
    main()
