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
"""Stage 9 as the device runs it, in vc2sim, for diffing against the chip.

The device's video image (bcmfw/mkvideo.py's defines), HELLO, PROBE, the
video OPEN, the ASC, AAC OPEN (with --flags, as stage 9's byte-copy run
passes them), then each access unit of aac9.au in its own rotating slot
(apps/video/vplayer.c vplayer_aac_test: 32 slots of 8 KB from 0x340000,
PCM at +4 KB), its PCM hashed against aac9.ref, and after every DECODE the
parse's checkpoints (HP_AAC_STATE; bcmfw/aac/aac.h AAC_T_*).  Trace lines
are printed exactly as the device logs them ("stage 9: trace AU k [i of
n]: ele=... "), for the AUs the device logs (the first three and wrong
ones, six at most).

  bcmfw/aac/test/aac_dev_sim.py [--aus N] [--flags F] [--seed S]
      the run, as the device's log lines
  bcmfw/aac/test/aac_dev_sim.py --diff bcmtest.log
      every run of stage 9 in a device log against vc2sim: for each AU
      the device traced, its checkpoints beside the simulator's, and the
      first pair that differs (the first wrong stage of the parse)

aac9.au/.ref: bcmfw/aac/test/mkdevclip.sh (made if missing; a synthetic
clip, no owner media).  Exit 0 when every AU matches (--diff: when no
traced AU differs).  Skips (exit 0) without vc4-elf-gcc.
"""
import os
import random
import re
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, os.path.join(ROOT, 'bcmfw', 'tests'))
sys.path.insert(0, os.path.join(ROOT, 'bcmfw'))
import video_test as VT     # noqa: E402
import mkvideo              # noqa: E402

T, K, S = VT.T, VT.K, VT.S
SLOTS_AT, SLOT, SLOTS, SLOT_PCM = 0x340000, 0x2000, 32, 0x1000
ASC_AT = 0x39C000
TRACE_MAX, TRACED_MAX = 40, 6           # vplayer.c AAC_TRACE_MAX, _TRACED_MAX
# vplayer.c aac_tag_name (aac.h AAC_T_*)
NAMES = ['?', 'ele', 'cpe', 'ics1', 'ics2', 'info', 'gain', 'sect', 'sf',
         'sfh', 'flags', 'spec', 'spech', 'err', 't14', 'drop']
# what each checkpoint is, for --diff's verdict
MEANING = {
    'ele': 'the element id and the bit position after it',
    'cpe': 'ms_mask_present and the bits after the common ics_info',
    'ics1': 'the first ic_stream after the copy',
    'ics2': 'the second ic_stream as copied (channel_pair_element memcpy)',
    'info': 'ics_info: window sequence, shape, max_sfb, groups, num_swb',
    'gain': 'global_gain and the bits after it',
    'sect': 'section_data: sections and the bits after them',
    'sf': 'the bits after scale_factor_data',
    'sfh': 'the scale factors themselves',
    'flags': 'pulse/tns/gain-control flags and the bits after them',
    'spec': 'the bits after spectral_data',
    'spech': 'the spectral coefficients',
    'err': "libfaad's error and the bit position",
}


def s32(v):
    return struct.unpack('<i', struct.pack('<I', v & 0xFFFFFFFF))[0]


def fnv(h, data):
    for c in data:
        h = ((h ^ c) * 16777619) & 0xFFFFFFFF
    return h


def name(tag):
    return NAMES[tag] if tag < len(NAMES) else 't?'


def load(dev_dir):
    au_p, ref_p = os.path.join(dev_dir, 'aac9.au'), os.path.join(dev_dir, 'aac9.ref')
    if not (os.path.exists(au_p) and os.path.exists(ref_p)):
        subprocess.run([os.path.join(HERE, 'mkdevclip.sh')], check=True,
                       stdout=subprocess.DEVNULL)
    au = open(au_p, 'rb').read()
    ref = open(ref_p, 'rb').read()
    assert au[:4] == b'AACU' and ref[:4] == b'AACR'
    asc_len = struct.unpack_from('<I', au, 4)[0]
    asc = au[8:8 + asc_len]
    pos, aus = 8 + asc_len, []
    n = struct.unpack_from('<I', ref, 4)[0]
    for k in range(n):
        ln = struct.unpack_from('<I', au, pos)[0]
        aus.append(au[pos + 4:pos + 4 + ln])
        pos += 4 + ln
    want = [struct.unpack_from('<II', ref, 8 + 8 * k) for k in range(n)]
    return asc, aus, want


def trace_lines(k, pairs, total):
    """vplayer.c aac_trace's lines (keep the two in step)."""
    out = []
    first = 0
    while first < len(pairs) or first == 0:
        s = 'stage 9: trace AU %d [%d of %d]:' % (k, first, total)
        for tag, v in pairs[first:first + 8]:
            s += ' %s=%08x' % (name(tag), v)
        out.append(s)
        first += 8
    return out


class Chip:
    """The device image in vc2sim, through stage 9's calls."""

    def __init__(self, seed):
        import argparse
        ns = argparse.Namespace(verbose=False, seed=seed, png=os.path.join(
            ROOT, 'build', 'bcmfw', 'test-video'))
        suite = VT.Suite(ns)
        defs = tuple(mkvideo.DEFS)
        prog, rep, img = suite.image(defs)
        entry = VT.B.config_value('BCMFW_ENTRY', defs)
        b = VT.VideoBench(img, 1, entry, None)
        b.mark_pc = prog.symbols['present']
        h = T.Host(b, random.Random(seed))
        h.fb = [0] * (T.W * T.H)
        h.lcd_awake()
        h.settle()
        h.tick_task = False
        h.idle_ok = False
        self.b, self.h = b, h
        self.vp = VT.Player(b)
        self.vp.attach()

    def call(self, args, timeout_us=50_000):
        return self.vp.call(K['HP_OP_AAC'], args, timeout_us=timeout_us)

    def start(self, asc, flags):
        vp = self.vp
        vp.call(K['HP_OP_HELLO'], [K['HP_MAJOR'] << 8 | K['HP_MINOR']])
        op, st, p = self.call([0, K['HP_AAC_PROBE']])
        lines = ['stage 9: probe %08x, model %08x' % (p[0], p[1])]
        st, p = vp.open(320, 240)
        if st != K['HP_OK']:
            raise SystemExit('stage 9: video OPEN %d' % st)
        op, st, r = self.call([0, K['HP_AAC_MEMSTRESS']], timeout_us=20_000_000)
        if st == K['HP_OK'] and len(r) >= 19:
            lines.append('stage 9: memstress SRAM %s; SDRAM %s (w b h bw hw copy '
                         'clear)' % (
                             ' '.join('%d' % x for x in r[0:7]),
                             ' '.join('%d' % x for x in r[7:14])))
            if r[14]:
                lines.append('stage 9: memstress SDRAM first wrong: pattern %d, '
                             'index %d, got %08x, want %08x' % (r[14] - 1, r[15],
                                                                 r[16], r[17]))
        else:
            lines.append('stage 9: memstress: status %d, %d words' % (st, len(r)))
        self.b.host_sdram_write(ASC_AT, asc + b'\0' * (-len(asc) % 4))
        # vplayer.c always adds VPLAYER_AAC_TRACE (aac.h AAC_FLAG_TRACE)
        op, st, p = self.call([vp.session, K['HP_AAC_OPEN'], ASC_AT, len(asc),
                               flags | 2], timeout_us=500_000)
        lines.append('stage 9: AAC OPEN status %d, code %d: %d Hz, %d ch' % (
            st, s32(p[2]) if len(p) > 2 else 0, p[0] if p else 0,
            p[1] if len(p) > 1 else 0))
        return lines

    def decode(self, k, au):
        slot = SLOTS_AT + (k % SLOTS) * SLOT
        self.b.host_sdram_write(slot, au + b'\0' * (-len(au) % 4))
        op, st, p = self.call([self.vp.session, K['HP_AAC_DECODE'], slot, len(au),
                               slot + SLOT_PCM], timeout_us=3_000_000)
        base = S.SDRAM_BACKING + slot + SLOT_PCM
        got = bytes(self.b.bus.b[base:base + 4 * p[0]]) if st == K['HP_OK'] else b''
        return st, p, fnv(2166136261, got)

    def trace(self):
        pairs, total = [], 0
        for first in range(0, TRACE_MAX, 15):
            op, st, p = self.call([0, K['HP_AAC_STATE'], first])
            if st != K['HP_OK'] or not p:
                break
            total = p[0]
            for i in range(1, len(p) - 1, 2):
                if len(pairs) < TRACE_MAX:
                    pairs.append((p[i], p[i + 1]))
            if first + 15 >= total:
                break
        return pairs, total


def run(asc, aus, want, flags, seed, n_aus, emit=print, keep=None):
    """One stage 9 run; returns (matched, {k: (pairs, total)} for keep or
    the device's choice of AUs)."""
    chip = Chip(seed)
    for line in chip.start(asc, flags):
        emit(line)
    matched, traced, kept = 0, 0, {}
    copy_checked = copy_bad = 0
    for k, au in enumerate(aus[:n_aus]):
        st, p, h = chip.decode(k, au)
        n_want, h_want = want[k]
        bad = True
        if st == K['HP_E_CORRUPT']:
            emit('stage 9: AU %d (%d bytes): DECODE refused, code %d (libfaad '
                 'error %d)' % (k, len(au), s32(p[3]), -100 - s32(p[3])))
        elif st == K['HP_OK'] and p[0] == n_want and h == h_want:
            matched += 1
            bad = False
        else:
            emit('stage 9: AU %d differs: %d frames, hash %08x; want %d, %08x' % (
                k, p[0] if p else 0, h, n_want, h_want))
        pairs, total = chip.trace()
        d = dict((name(t), v) for t, v in reversed(pairs))
        if 'ics1' in d and 'ics2' in d:
            copy_checked += 1
            if d['ics1'] != d['ics2']:
                copy_bad += 1
                emit('stage 9: AU %d: ic_stream copy WRONG (first %08x, copy %08x)'
                     % (k, d['ics1'], d['ics2']))
        log_it = traced < TRACED_MAX and (k < 3 or bad)
        if keep is not None:
            if k in keep:
                kept[k] = (pairs, total)
        elif log_it:
            kept[k] = (pairs, total)
        if log_it:
            traced += 1
            for line in trace_lines(k, pairs, total):
                emit(line)
    emit('stage 9: %d of %d AUs matched; ic_stream copies wrong %d of %d' % (
        matched, min(n_aus, len(aus)), copy_bad, copy_checked))
    return matched, kept


TRACE_RE = re.compile(r'stage 9: trace AU (\d+) \[(\d+) of (\d+)\]:(.*)$')


def device_runs(path):
    """The device log's stage 9 runs: [(what, flags, {k: [(name, value)]})]
    in order; a run starts at each 'AAC OPEN' line and is named by the
    summary line that follows it."""
    runs, cur = [], None
    for line in open(path, errors='replace'):
        if 'stage 9: AAC OPEN' in line:
            cur = {'what': '?', 'aus': {}}
            runs.append(cur)
            continue
        m = TRACE_RE.search(line)
        if m and cur is not None:
            k = int(m.group(1))
            for nm, v in re.findall(r'(\w+)=([0-9a-f]{8})', m.group(4)):
                cur['aus'].setdefault(k, []).append((nm, int(v, 16)))
            continue
        m = re.search(r'stage 9 (.+?): (PASS|FAIL) \(rc', line)
        if m and cur is not None and cur['what'] == '?':
            cur['what'] = m.group(1)
    return runs


def diff(path, asc, aus, want, seed):
    runs = device_runs(path)
    if not runs:
        print('aac_dev_sim: no stage 9 trace lines in %s (a dev44 or older '
              'build logs none)' % path)
        return 1
    worst = 0
    for r in runs:
        flags = 1 if r['what'] == 'byte copies' else 0
        if not r['aus']:
            print('== %s: no AUs traced' % r['what'])
            continue
        keep = set(r['aus'])
        _, sim = run(asc, aus, want, flags, seed, max(keep) + 1,
                     emit=lambda s: None, keep=keep)
        print('== %s (OPEN flags %d)' % (r['what'], flags))
        for k in sorted(keep):
            dev = r['aus'][k]
            simp = [(name(t), v) for t, v in sim.get(k, ([], 0))[0]]
            first = None
            for i in range(max(len(dev), len(simp))):
                a = dev[i] if i < len(dev) else None
                b = simp[i] if i < len(simp) else None
                if a != b:
                    first = i
                    break
            if first is None:
                print('AU %d: all %d checkpoints agree' % (k, len(dev)))
                continue
            worst = 1
            a = dev[first] if first < len(dev) else ('(none)', 0)
            b = simp[first] if first < len(simp) else ('(none)', 0)
            print('AU %d: first difference at checkpoint %d: chip %s=%08x, '
                  'vc2sim %s=%08x' % (k, first, a[0], a[1], b[0], b[1]))
            nm = b[0] if b[0] != '(none)' else a[0]
            print('       (%s)' % MEANING.get(nm, nm))
            agree = ', '.join('%s=%08x' % p for p in simp[:first]) or 'nothing'
            print('       agreed before it: %s' % agree)
    return worst


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--aus', type=int, default=8, help='AUs (0: all of aac9)')
    ap.add_argument('--flags', type=int, default=0,
                    help='AAC OPEN flags (1: byte copies, stage 9 run 2)')
    ap.add_argument('--seed', type=int, default=2722)
    ap.add_argument('--dir', default=os.path.join(ROOT, 'build', 'aac', 'device'))
    ap.add_argument('--diff', metavar='BCMTEST_LOG')
    a = ap.parse_args(argv)
    if not VT.B.have_gcc():
        print('skip: vc4-elf-gcc not found')
        return 0
    asc, aus, want = load(a.dir)
    if a.diff:
        return diff(a.diff, asc, aus, want, a.seed)
    n = a.aus or len(aus)
    matched, _ = run(asc, aus, want, a.flags, a.seed, n)
    return 0 if matched == min(n, len(aus)) else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
