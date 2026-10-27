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
"""Generate bcmfw/h264/vlc_tables.c: the CAVLC code tables of ITU-T H.264
(Tables 9-5, 9-7, 9-8, 9-9 and 9-10) as lookup tables, precomputed here
rather than at run time.

Every table is first checked: it must be prefix-free, and its Kraft sum
must be exactly 1 (a complete code) or below 1 with the unused words the
standard leaves out.  Then each code is split as (leading zeros, the bits
after the first one).  For each leading-zero count lz the table keeps the
longest suffix length s(lz) and 2^s(lz) entries.  The top lz + 1 + s bits
of the stream are 2^s + suffix, so one shift gives an index; a decode is

    w = next 32 bits; lz = min(clz(w | 1), LZMAX); r = row[lz]
    e = tab[(r >> 5) + (w >> (r & 31))]

with row = base << 5 | (31 - lz - s), base = offset - 2^s, and
e = length << 8 | value.  The last row (lz = LZMAX) keeps 2^(s+1)
entries with base = offset, because w may have more leading zeros there:
its lower half is the all-zeros code where a table has one, and unused
otherwise.  Unused words decode as length 1, value 255, which every
caller rejects as out of range.  Padding at the start keeps every base
non-negative.

run_before for zerosLeft 1..6 (codes of at most 3 bits) is also a direct
table, h264_rb3[zerosLeft - 1][next 3 bits] = length << 4 | run.  So is
total_zeros (codes of at most 9 bits; batch 4): h264_tzd, one table per
TotalCoeff indexed by its longest code's length in bits, with the vlc
tables' entry format (emit_tzd).

Usage: bcmfw/h264/gen_tables.py [--check]   (--check: verify, write nothing)
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

# ---- Table 9-5, coeff_token: lengths and code values, index 4*TotalCoeff
# + TrailingOnes, for 0<=nC<2, 2<=nC<4, 4<=nC<8.  (nC >= 8 is a 6-bit
# fixed-length code, handled in C.)
CT_LEN = [
    [1, 0, 0, 0, 6, 2, 0, 0, 8, 6, 3, 0, 9, 8, 7, 5, 10, 9, 8, 6,
     11, 10, 9, 7, 13, 11, 10, 8, 13, 13, 11, 9, 13, 13, 13, 10,
     14, 14, 13, 11, 14, 14, 14, 13, 15, 15, 14, 14, 15, 15, 15, 14,
     16, 15, 15, 15, 16, 16, 16, 15, 16, 16, 16, 16, 16, 16, 16, 16],
    [2, 0, 0, 0, 6, 2, 0, 0, 6, 5, 3, 0, 7, 6, 6, 4, 8, 6, 6, 4,
     8, 7, 7, 5, 9, 8, 8, 6, 11, 9, 9, 6, 11, 11, 11, 7,
     12, 11, 11, 9, 12, 12, 12, 11, 12, 12, 12, 11, 13, 13, 13, 12,
     13, 13, 13, 13, 13, 14, 13, 13, 14, 14, 14, 13, 14, 14, 14, 14],
    [4, 0, 0, 0, 6, 4, 0, 0, 6, 5, 4, 0, 6, 5, 5, 4, 7, 5, 5, 4,
     7, 5, 5, 4, 7, 6, 6, 4, 7, 6, 6, 4, 8, 7, 7, 5,
     8, 8, 7, 6, 9, 8, 8, 7, 9, 9, 8, 8, 9, 9, 9, 8,
     10, 9, 9, 9, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10],
]
CT_BITS = [
    [1, 0, 0, 0, 5, 1, 0, 0, 7, 4, 1, 0, 7, 6, 5, 3, 7, 6, 5, 3,
     7, 6, 5, 4, 15, 6, 5, 4, 11, 14, 5, 4, 8, 10, 13, 4,
     15, 14, 9, 4, 11, 10, 13, 12, 15, 14, 9, 12, 11, 10, 13, 8,
     15, 1, 9, 12, 11, 14, 13, 8, 7, 10, 9, 12, 4, 6, 5, 8],
    [3, 0, 0, 0, 11, 2, 0, 0, 7, 7, 3, 0, 7, 10, 9, 5, 7, 6, 5, 4,
     4, 6, 5, 6, 7, 6, 5, 8, 15, 6, 5, 4, 11, 14, 13, 4,
     15, 10, 9, 4, 11, 14, 13, 12, 8, 10, 9, 8, 15, 14, 13, 12,
     11, 10, 9, 12, 7, 11, 6, 8, 9, 8, 10, 1, 7, 6, 5, 4],
    [15, 0, 0, 0, 15, 14, 0, 0, 11, 15, 13, 0, 8, 12, 14, 12, 15, 10, 11, 11,
     11, 8, 9, 10, 9, 14, 13, 9, 8, 10, 9, 8, 15, 14, 13, 13,
     11, 14, 10, 12, 15, 10, 13, 12, 11, 14, 9, 12, 8, 10, 13, 8,
     13, 7, 9, 12, 9, 12, 11, 10, 5, 8, 7, 6, 1, 4, 3, 2],
]
# chroma DC (nC == -1, 4:2:0), index 4*TotalCoeff + TrailingOnes, 0..4
CDC_LEN = [2, 0, 0, 0, 6, 1, 0, 0, 6, 6, 3, 0, 6, 7, 7, 6, 6, 8, 8, 7]
CDC_BITS = [1, 0, 0, 0, 7, 1, 0, 0, 4, 6, 1, 0, 3, 3, 2, 5, 2, 3, 2, 0]

# ---- Tables 9-7 and 9-8, total_zeros for 4x4 blocks, TotalCoeff 1..15
TZ_LEN = [
    [1, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 9],
    [3, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 6, 6, 6, 6],
    [4, 3, 3, 3, 4, 4, 3, 3, 4, 5, 5, 6, 5, 6],
    [5, 3, 4, 4, 3, 3, 3, 4, 3, 4, 5, 5, 5],
    [4, 4, 4, 3, 3, 3, 3, 3, 4, 5, 4, 5],
    [6, 5, 3, 3, 3, 3, 3, 3, 4, 3, 6],
    [6, 5, 3, 3, 3, 2, 3, 4, 3, 6],
    [6, 4, 5, 3, 2, 2, 3, 3, 6],
    [6, 6, 4, 2, 2, 3, 2, 5],
    [5, 5, 3, 2, 2, 2, 4],
    [4, 4, 3, 3, 1, 3],
    [4, 4, 2, 1, 3],
    [3, 3, 1, 2],
    [2, 2, 1],
    [1, 1],
]
TZ_BITS = [
    [1, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 1],
    [7, 6, 5, 4, 3, 5, 4, 3, 2, 3, 2, 3, 2, 1, 0],
    [5, 7, 6, 5, 4, 3, 4, 3, 2, 3, 2, 1, 1, 0],
    [3, 7, 5, 4, 6, 5, 4, 3, 3, 2, 2, 1, 0],
    [5, 4, 3, 7, 6, 5, 4, 3, 2, 1, 1, 0],
    [1, 1, 7, 6, 5, 4, 3, 2, 1, 1, 0],
    [1, 1, 5, 4, 3, 3, 2, 1, 1, 0],
    [1, 1, 1, 3, 3, 2, 2, 1, 0],
    [1, 0, 1, 3, 2, 1, 1, 1],
    [1, 0, 1, 3, 2, 1, 1],
    [0, 1, 1, 2, 1, 3],
    [0, 1, 1, 1, 1],
    [0, 1, 1, 1],
    [0, 1, 1],
    [0, 1],
]
# Table 9-9 (a), total_zeros for chroma DC 2x2, TotalCoeff 1..3
TZC_LEN = [[1, 2, 3, 3], [1, 2, 2], [1, 1]]
TZC_BITS = [[1, 1, 1, 0], [1, 1, 0], [1, 0]]
# Table 9-10, run_before, zerosLeft 1..6 and > 6
RB_LEN = [
    [1, 1],
    [1, 2, 2],
    [2, 2, 2, 2],
    [2, 2, 2, 3, 3],
    [2, 2, 3, 3, 3, 3],
    [2, 3, 3, 3, 3, 3, 3],
    [3, 3, 3, 3, 3, 3, 3, 4, 5, 6, 7, 8, 9, 10, 11],
]
RB_BITS = [
    [1, 0],
    [1, 1, 0],
    [3, 2, 1, 0],
    [3, 2, 1, 1, 0],
    [3, 2, 3, 2, 1, 0],
    [3, 0, 1, 3, 2, 5, 4],
    [7, 6, 5, 4, 3, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1],
]


def codes(lens, bits, skip_zero_len=True):
    """[(code string, value)] from parallel length/value lists."""
    out = []
    for v, (n, b) in enumerate(zip(lens, bits)):
        if n == 0 and skip_zero_len:
            continue
        out.append((format(b, '0%db' % n), v))
    return out


def check(name, cw, complete=True):
    """Prefix-free, and Kraft sum 1 (complete) or < 1."""
    words = [c for c, _ in cw]
    for i, a in enumerate(words):
        for j, b in enumerate(words):
            if i != j and b.startswith(a):
                raise SystemExit('%s: %s is a prefix of %s' % (name, a, b))
    k = sum(2.0 ** -len(c) for c in words)
    if complete and abs(k - 1.0) > 1e-12:
        raise SystemExit('%s: Kraft sum %r, expected 1' % (name, k))
    if k > 1.0 + 1e-12:
        raise SystemExit('%s: Kraft sum %r > 1' % (name, k))
    return k


INVALID = 1 << 8 | 0xff


def build(cw):
    """(lzmax, rows (base << 5 | shift), entries) as described above."""
    zero = [(c, v) for c, v in cw if '1' not in c]
    rest = [(c, v) for c, v in cw if '1' in c]
    lzmax = max(c.index('1') for c, _ in rest) if rest else 0
    if zero:
        assert len(zero) == 1
        lzmax = len(zero[0][0])
    rows, entries = [], []
    for lz in range(lzmax + 1):
        mine = [(c[lz + 1:], v, len(c)) for c, v in rest if c.index('1') == lz]
        if zero and lz == lzmax:
            # w >> (31 - lzmax) is 0 or 1: both are the all-zeros code
            rows.append((len(entries), 31 - lz))
            entries += [len(zero[0][0]) << 8 | zero[0][1]] * 2
            continue
        s = max((len(suf) for suf, _, _ in mine), default=0)
        tab = [INVALID] * (1 << s)
        filled = [False] * (1 << s)
        for suf, v, n in mine:
            pad = s - len(suf)
            base = int(suf, 2) << pad if suf else 0
            for k in range(1 << pad):
                assert not filled[base + k]
                filled[base + k] = True
                tab[base + k] = n << 8 | v
        if lz == lzmax:          # more leading zeros: the lower half
            rows.append((len(entries), 31 - lz - s))
            entries += [INVALID] * (1 << s) + tab
        else:
            rows.append((len(entries) - (1 << s), 31 - lz - s))
            entries += tab
    pad = max(0, -min(b for b, _ in rows))
    rows = [(b + pad, sh) for b, sh in rows]
    entries = [INVALID] * pad + entries
    assert all(0 <= b < 2048 and 0 <= sh < 32 for b, sh in rows)
    return lzmax, [b << 5 | sh for b, sh in rows], entries


def decode_model(lzmax, rows, entries, bits):
    """Python model of the C decode, for the self-check."""
    w = int((bits + '0' * 32)[:32], 2)
    lz = 32 - (w | 1).bit_length()
    lz = min(lz, lzmax)
    r = rows[lz]
    e = entries[(r >> 5) + (w >> (r & 31))]
    return None if e == INVALID else (e >> 8, e & 0xff)


def emit_rb3(out):
    """run_before, zerosLeft 1..6: [zl - 1][3 bits] = length << 4 | run."""
    tab = []
    for zl in range(6):
        cw = codes(RB_LEN[zl], RB_BITS[zl], False)
        check('run_before%d' % (zl + 1), cw, True)
        row = [None] * 8
        for c, v in cw:
            assert len(c) <= 3
            pad = 3 - len(c)
            for k in range(1 << pad):
                i = int(c, 2) << pad | k
                assert row[i] is None
                row[i] = len(c) << 4 | v
        assert None not in row        # complete codes
        for c, v in cw:               # the model decodes every code
            for tail in ('000', '111', '010'):
                i = int((c + tail)[:3], 2)
                if (row[i] >> 4, row[i] & 15) != (len(c), v):
                    raise SystemExit('rb3 %d: %s' % (zl + 1, c))
        tab.append(row)
    out.append('/* run_before, zerosLeft 1..6, by the next 3 bits: '
               'length << 4 | run_before */')
    out.append('const uint8_t h264_rb3[6][8] = {')
    for row in tab:
        out.append('    { ' + ', '.join('0x%02x' % e for e in row) + ' },')
    out.append('};')
    out.append('')
    return 48


def emit_tzd(out):
    """total_zeros read directly (batch 4): for TotalCoeff t (luma 1..15,
    then chroma DC 1..3) the next L(t) bits (its longest code) index
    h264_tzd at h264_tzd_row[t - 1] >> 5, the row's low 5 bits being
    32 - L(t).  An entry is length << 8 | total_zeros; the unused word is
    INVALID (as in the vlc tables)."""
    sets = [(TZ_LEN[t], TZ_BITS[t], 'total_zeros%d' % (t + 1), t != 0)
            for t in range(15)]
    sets += [(TZC_LEN[t], TZC_BITS[t], 'total_zeros_cdc%d' % (t + 1), True)
             for t in range(3)]
    rows, data = [], []
    for lens, bits, name, complete in sets:
        cw = codes(lens, bits, False)
        check(name, cw, complete)
        L = max(len(c) for c, _ in cw)
        assert L <= 9 and len(data) < (1 << 27)
        tab = [None] * (1 << L)
        for c, v in cw:
            assert v < 16 and len(c) < 16
            pad = L - len(c)
            for k in range(1 << pad):
                i = int(c, 2) << pad | k
                assert tab[i] is None
                tab[i] = len(c) << 8 | v
        # the model: every code, any tail, decodes to itself
        for c, v in cw:
            for tail in ('', '1' * 9, '0' * 9, '01' * 5):
                i = int((c + tail + '0' * 9)[:L], 2)
                if (tab[i] >> 8, tab[i] & 0xff) != (len(c), v):
                    raise SystemExit('tzd %s: %s' % (name, c))
        if complete:
            assert None not in tab
        tab = [INVALID if e is None else e for e in tab]
        rows.append(len(data) << 5 | (32 - L))
        data += tab
    out.append('/* total_zeros by TotalCoeff t, read directly (luma t = 1..15, '
               'then chroma DC\n * t = 1..3): the entry at h264_tzd_row[t - 1] '
               '>> 5 plus w >> (row & 31)\n * is length << 8 | total_zeros '
               '(length 1, value 255: the unused word) */')
    out.append('const uint16_t h264_tzd[%d] = {' % len(data))
    for i in range(0, len(data), 10):
        out.append('    ' + ', '.join('0x%04x' % e for e in data[i:i + 10]) + ',')
    out.append('};')
    out.append('const uint32_t h264_tzd_row[18] = {')
    for i in range(0, 18, 6):
        out.append('    ' + ', '.join('0x%04x' % r for r in rows[i:i + 6]) + ',')
    out.append('};')
    out.append('')
    return 2 * len(data) + 4 * 18


def emit(out, name, cw, complete=True):
    k = check(name, cw, complete)
    lzmax, rows, entries = build(cw)
    for c, v in cw:            # every code decodes to itself
        for tail in ('', '1' * 20, '0' * 20, '0101' * 5, '1', '01'):
            got = decode_model(lzmax, rows, entries, c + tail)
            if got != (len(c), v):
                raise SystemExit('%s: %s decodes to %r' % (name, c, got))
    # every bit string of the longest code's length: its code, or unused
    maxlen = max(len(c) for c, _ in cw)
    bycode = {c: v for c, v in cw}
    for x in range(1 << maxlen):
        bits = format(x, '0%db' % maxlen)
        want = next(((len(c), v) for c, v in bycode.items()
                     if bits.startswith(c)), None)
        if decode_model(lzmax, rows, entries, bits) != want:
            raise SystemExit('%s: %s decodes wrongly' % (name, bits))
    out.append('/* %s: %d codes, Kraft sum %s */' % (
        name, len(cw), '1' if abs(k - 1) < 1e-12 else '%.6f' % k))
    out.append('static const uint16_t %s_e[%d] = {' % (name, len(entries)))
    for i in range(0, len(entries), 10):
        out.append('    ' + ', '.join('0x%04x' % e for e in entries[i:i + 10]) + ',')
    out.append('};')
    out.append('const struct h264_vlc %s = { %d, {%s}, %s_e };' % (
        name, lzmax, ', '.join('0x%04x' % r for r in rows), name))
    out.append('')
    return len(entries), lzmax


def main(argv):
    out = []
    total = 0
    maxlz = 0
    tables = []
    for t in range(3):
        # one unused word each (2^-15, 2^-13, 2^-10 of the code space)
        tables.append(('h264_vlc_coeff_token%d' % t, codes(CT_LEN[t], CT_BITS[t]),
                       False))
    tables.append(('h264_vlc_coeff_token_cdc', codes(CDC_LEN, CDC_BITS), True))
    # total_zeros: the direct tables of emit_tzd
    # zerosLeft 1..6 are the direct table h264_rb3 (emit_rb3)
    tables.append(('h264_vlc_run_before7', codes(RB_LEN[6], RB_BITS[6], False),
                   False))
    rowbytes = 0
    for name, cw, complete in tables:
        n, lz = emit(out, name, cw, complete)
        total += n
        rowbytes += 4 + 2 * 17 + 4     # lzmax (padded), rows, pointer
        maxlz = max(maxlz, lz)
    if maxlz > 16:
        raise SystemExit('lzmax %d > 16: struct too small' % maxlz)
    rb3 = emit_rb3(out)
    rb3 += emit_tzd(out)
    head = '''/***************************************************************************
 *             __________               __   ___.
 *   Open      \\______   \\ ____   ____ |  | _\\_ |__   _______  ___
 *   Source     |       _//  _ \\_/ ___\\|  |/ /| __ \\ /  _ \\  \\/  /
 *   Jukebox    |    |   (  <_> )  \\___|    < | \\_\\ (  <_> > <  <
 *   Firmware   |____|_  /\\____/ \\___  >__|_ \\|___  /\\____/__/\\_ \\
 *                     \\/            \\/     \\/    \\/            \\/
 *
 * H.264 CAVLC code tables (ITU-T H.264 Tables 9-5, 9-7 to 9-10) as
 * lookup tables.  GENERATED by bcmfw/h264/gen_tables.py: do not edit.
 * %d entries (%d bytes), rows, the 48-byte run_before table and the
 * total_zeros tables: %d bytes.
 *
 * Copyright (C) 2026 ProkBox contributors
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#include "h264_int.h"

''' % (total, total * 2, total * 2 + rb3 + rowbytes)
    text = prokbox_header.convert(head + '\n'.join(out), '.c')[0]
    path = os.path.join(HERE, 'vlc_tables.c')
    if '--check' in argv:
        old = open(path).read() if os.path.exists(path) else ''
        if old != text:
            print('vlc_tables.c is out of date', file=sys.stderr)
            return 1
        print('tables OK (%d entries)' % total)
        return 0
    with open(path, 'w') as f:
        f.write(text)
    print('wrote %s: %d tables, %d entries, lzmax %d' % (path, len(tables), total, maxlz))
    return 0


# the ProkBox banner for the generated file (scripts/prokbox_header.py)
sys.path.insert(0, os.path.join(HERE, '..', '..', 'scripts'))
import prokbox_header  # noqa: E402


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
