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
"""Our H.264 decoder (bcmfw/h264) on the VideoCore II model.

Compiles the decoder with vc2cc (--conservative, --no-float), runs it in
vc2sim on corpus clips, compares every frame with ffmpeg's yuv bit for
bit, and profiles the instructions executed: per frame, per macroblock,
and per stage (parse, intra, inter, IDCT, deblock).

Memory, in the simulator's flat space (the device map is in decoder.md):
  0x0200000  image (code + tables)
  0x0800000  decoder arena (context, macroblock info, the DPB)
  0x1000000  bitstream: one NAL unit at a time, as the FIFO would hold it
  0x1f00000  stack top

Usage:
  bcmfw/h264/test/sim_test.py [-v] [--frames N] [--json OUT] [--vpu LIST]
                              CLIP [CLIP...]
      CLIP: a corpus name (a_ionly_qp20) or a path to a .264 next to its .yuv
      --vpu: the vector kernel families to build (mc,idct,dbk,intra; default
      all; "none" is the portable C decoder)

Besides instructions, the profile counts vector instructions and vector
"rows" (each instruction counts its REP), for decoder.md's cycle estimate.
Exit 0 when every decoded frame is bit-exact.  Skips (exit 0) without
vc4-elf-gcc.
"""
import bisect
import collections
import json
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.dirname(HERE)
ROOT = os.path.dirname(os.path.dirname(SRC))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'bcmre'))
import vc2cc as C    # noqa: E402
import vc2sim as S   # noqa: E402

CORPUS = os.path.join(ROOT, 'build', 'video', 'corpus')
OUT = os.path.join(ROOT, 'build', 'h264', 'sim')
SOURCES = ['h264.c', 'ps.c', 'slice.c', 'mb.c', 'cavlc.c', 'mvpred.c',
           'intra.c', 'idct.c', 'mc.c', 'inter.c', 'deblock.c', 'tables.c',
           'vlc_tables.c']
CFLAGS = ['-std=gnu99', '-Wall', '-Wextra', '-Wno-unused-parameter',
          '-Werror', '-fno-tree-loop-distribute-patterns']
VPU_FAMILIES = {'mc': 'H264_VPU_MC', 'idct': 'H264_VPU_IDCT', 'dbk': 'H264_VPU_DBK',
                'intra': 'H264_VPU_INTRA'}

ARENA, ARENA_SIZE = 0x0800000, 0x0800000
BITS = 0x1000000
MAX_STEPS = 400_000_000

# Every function's stage (self time).  Anything unlisted is "other".
STAGES = [
    ('parse', ['bs_', 'h264_decode_nal', 'unescape', 'h264_parse_', 'parse_header',
               'h264_decode_slice', 'h264_decode_slice_data', 'decode_mb',
               'decode_skip', 'decode_pcm', 'residual', 'h264_residual',
               'intra4x4_pred_modes', 'inter_mb', 'read_', 'fill_part',
               'build_reflist', 'number_refs', '__ctzsi2']),
    ('mbctl', ['load_neighbours', 'store_mb', 'mb_setup']),
    ('mvpred', ['h264_mv_pred']),
    ('intra', ['h264_pred', 'recon_intra4x4', 'fill_block', 'fill_row',
               'h264_vpred']),
    ('inter', ['h264_inter_pred', 'h264_mc_', 'hpel_', 'avg2', 'edge_copy',
               'h264_vmc_']),
    ('idct', ['h264_idct', 'h264_luma_dc', 'h264_chroma_dc', 'add_residual',
              'h264_vidct']),
    ('deblock', ['h264_deblock', 'filter_mb', 'strength', 'h264_dbk_',
                 'h264_vdbk']),
    ('picture', ['start_picture', 'h264_finish_picture', 'mark', 'conceal',
                 'h264_layout', 'h264_open', 'memcpy', 'memset']),
]
STAGE_ORDER = ['parse', 'mbctl', 'mvpred', 'intra', 'inter', 'idct',
               'deblock', 'picture', 'other']


def stage_of(name):
    for st, prefixes in STAGES:
        for p in prefixes:
            if name == p or name.startswith(p):
                return st
    return 'other'


def vpu_flags(vpu):
    fams = set(VPU_FAMILIES) if vpu == 'all' else \
        set() if vpu == 'none' else set(vpu.split(','))
    if not fams:
        return []
    return ['-DH264_VPU=1'] + ['-D%s=%d' % (m, f in fams)
                               for f, m in VPU_FAMILIES.items()]


def build(vpu='all'):
    os.makedirs(os.path.join(OUT, 'lib'), exist_ok=True)
    flags = vpu_flags(vpu)
    extra = ['-I' + SRC] + CFLAGS + flags
    text = C.compile_c(os.path.join(ROOT, 'bcmfw', 'lib', 'string.c'), '-O2',
                       extra + ['-I' + os.path.join(ROOT, 'bcmfw')])
    with open(os.path.join(OUT, 'lib', 'string.s'), 'w') as f:
        f.write(text)
    srcs = SOURCES + (['vpu_kernels.S'] if flags else [])
    prog = C.build([os.path.join(SRC, s) for s in srcs], '-O2', extra=extra,
                   libs=[os.path.join(OUT, 'lib')] + C.DEFAULT_LIBS,
                   no_float=True, conservative=True)
    with open(os.path.join(OUT, 'h264.map'), 'w') as f:
        prog.write_map(f)
    with open(os.path.join(OUT, 'h264.lst'), 'w') as f:
        f.write(C.listing(prog) + '\n')
    return prog


class ProfSim(S.Sim):
    """vc2sim with a per-address execution count."""
    def __init__(self):
        S.Sim.__init__(self)
        self.pcount = collections.Counter()

    def fetch(self, pc):
        fn, size, ins = S.Sim.fetch(self, pc)
        cnt = self.pcount

        def counted():
            cnt[pc] += 1
            return fn()
        self.code[pc] = (counted, size, ins)
        return self.code[pc]


def nal_units(data):
    """Annex B NAL units (start code and trailing zeros removed)."""
    out, i, n = [], 0, len(data)
    starts = []
    while True:
        j = data.find(b'\x00\x00\x01', i)
        if j < 0:
            break
        starts.append(j + 3)
        i = j + 3
    for k, s in enumerate(starts):
        e = starts[k + 1] - 3 if k + 1 < len(starts) else n
        nal = data[s:e].rstrip(b'\x00')
        if nal:
            out.append(nal)
    return out


def s32(v):
    return v - (1 << 32) if v & 0x80000000 else v


REP_RE = re.compile(r' rep(\d+)')


def vector_rows(sim, pcount):
    """(vector instructions, vector rows: each counts its REP) executed."""
    vi = vr = 0
    for pc, n in pcount.items():
        ins = sim.code[pc][2]
        if getattr(ins.e, 'vclass', None):
            m = REP_RE.search(ins.text)
            vi += n
            vr += n * (int(m.group(1)) if m else 1)
    return vi, vr


class Profile:
    def __init__(self, prog):
        self.funcs = sorted((a, n) for n, a in prog.symbols.items()
                            if prog.base <= a < prog.ends['text'] and
                            not re.match(r'__\w+_(start|end)$', n))
        self.addrs = [a for a, _ in self.funcs]

    def by_function(self, pcount):
        fc = collections.Counter()
        for pc, n in pcount.items():
            k = bisect.bisect_right(self.addrs, pc) - 1
            fc[self.funcs[k][1] if k >= 0 else '?'] += n
        return fc

    def by_stage(self, fc):
        st = collections.Counter()
        for f, n in fc.items():
            st[stage_of(f)] += n
        return st


def run_clip(prog, prof, clip, max_frames, verbose):
    path = clip if clip.endswith('.264') else os.path.join(CORPUS, clip + '.264')
    name = os.path.basename(path)[:-4]
    data = open(path, 'rb').read()
    ref = open(path[:-4] + '.yuv', 'rb').read()
    sim = ProfSim()
    C.load_into_sim(prog, sim)
    d = sim.call('h264_open', ARENA, ARENA_SIZE)
    if not d:
        raise SystemExit('h264_open failed')
    frames, mism, errors = [], 0, 0
    pos = 0
    t0 = time.time()
    steps_mark = sim.steps
    pc_mark = collections.Counter()
    for nal in nal_units(data):
        if max_frames and len(frames) >= max_frames:
            break
        sim.mem.write(BITS, nal)
        while True:
            r = s32(sim.call('h264_decode_nal', d, BITS, len(nal),
                             max_steps=MAX_STEPS))
            if r < 0:
                errors += 1
                print('  %s: NAL type %d: error %d' % (name, nal[0] & 31, r))
            if r not in (1, 2):
                break
            # a picture: compare and account
            p = sim.call('h264_frame', d)
            w = [sim.mem.r32(p + 4 * k) for k in range(10)]
            y, u, v, sy, sc, wd, ht = w[:7]
            fsize = wd * ht * 3 // 2
            ok = pos + fsize <= len(ref)
            first_bad = None
            if ok:
                planes = ((y, sy, wd, ht, 0), (u, sc, wd // 2, ht // 2, wd * ht),
                          (v, sc, wd // 2, ht // 2, wd * ht * 5 // 4))
                for pi, (base, st, pw, ph, off) in enumerate(planes):
                    for row in range(ph):
                        got = sim.mem.read(base + row * st, pw)
                        want = ref[pos + off + row * pw:pos + off + (row + 1) * pw]
                        if got != want:
                            x = next(i for i in range(pw) if got[i] != want[i])
                            first_bad = ('YUV'[pi], x, row)
                            break
                    if first_bad:
                        break
            pos += fsize
            steps = sim.steps - steps_mark
            delta = sim.pcount - pc_mark
            pc_mark = collections.Counter(sim.pcount)
            steps_mark = sim.steps
            fc = prof.by_function(delta)
            vi, vr = vector_rows(sim, delta)
            frames.append({'n': len(frames), 'idr': w[8], 'frame_num': w[7],
                           'instr': steps, 'vins': vi, 'vrows': vr,
                           'stages': dict(prof.by_stage(fc)),
                           'funcs': dict(fc), 'exact': ok and not first_bad})
            if not ok or first_bad:
                mism += 1
                print('  %s frame %d: MISMATCH %s' % (
                    name, len(frames) - 1, 'beyond the reference' if not ok else
                    'plane %s at (%d,%d), MB (%d,%d)' % (
                        first_bad[0], first_bad[1], first_bad[2],
                        first_bad[1] // (16 if first_bad[0] == 'Y' else 8),
                        first_bad[2] // (16 if first_bad[0] == 'Y' else 8))))
            if verbose:
                print('  %s frame %d (%s): %s, %d instructions (%.1f s)' % (
                    name, len(frames) - 1, 'I' if w[8] else 'P',
                    'bit-exact' if frames[-1]['exact'] else 'MISMATCH', steps,
                    time.time() - t0))
            if r != 2:
                break
    st = sim.call('h264_stats', d)
    stats = [sim.mem.r32(st + 4 * k) for k in range(6)]
    return {'clip': name, 'frames': frames, 'mismatched': mism,
            'errors': errors, 'mbs': stats[2], 'skipped': stats[3],
            'seconds': time.time() - t0}


def report(res, mbs_per_frame, funcs=0):
    fr = res['frames']
    if not fr:
        return
    print('%s: %d frames, %d mismatched, %d errors: %s (%.0f s in vc2sim)' % (
        res['clip'], len(fr), res['mismatched'], res['errors'],
        'bit-exact' if not res['mismatched'] and not res['errors'] else 'FAIL',
        res['seconds']))
    for kind, sel in (('I', [f for f in fr if f['idr']]),
                      ('P', [f for f in fr if not f['idr']])):
        if not sel:
            continue
        tot = sum(f['instr'] for f in sel) / len(sel)
        st = collections.Counter()
        for f in sel:
            st.update(f['stages'])
        vi = sum(f['vins'] for f in sel) / len(sel)
        vr = sum(f['vrows'] for f in sel) / len(sel)
        print('  %s frames (%d): %.2f M instructions/frame (max %.2f), %.0f per MB;'
              ' vector: %.0f k instructions, %.0f k rows; estimate %.2f M cycles' % (
                  kind, len(sel), tot / 1e6, max(f['instr'] for f in sel) / 1e6,
                  tot / mbs_per_frame, vi / 1e3, vr / 1e3, (tot - vi + vr) / 1e6))
        print('    ' + ', '.join('%s %.0f%%' % (s, 100.0 * st[s] / sum(st.values()))
                                 for s in STAGE_ORDER if st[s]))
        if funcs:
            fc = collections.Counter()
            for f in sel:
                fc.update(f['funcs'])
            print('    top: ' + ', '.join('%s %.0f' % (n, c / len(sel) / mbs_per_frame)
                                         for n, c in fc.most_common(funcs)) +
                  ' (instructions per MB)')


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('clips', nargs='+')
    ap.add_argument('--frames', type=int, default=0)
    ap.add_argument('--json')
    ap.add_argument('-v', action='store_true')
    ap.add_argument('--vpu', default='all',
                    help='vector kernel families: all, none, or a list of '
                         'mc,idct,dbk,intra')
    ap.add_argument('--funcs', type=int, default=0,
                    help='also list the N most expensive functions')
    a = ap.parse_args(argv)
    if not os.path.exists(C.GCC):
        print('sim_test: vc4-elf-gcc not found (%s): skipped' % C.GCC)
        return 0
    prog = build(a.vpu)
    print('vector kernels: %s' % a.vpu)
    e = prog.ends
    print('image: text %d, rodata %d, data %d, bss %d bytes' % (
        e['text'] - prog.base, e['rodata'] - e['text'], e['data'] - e['rodata'],
        e['bss'] - e['data']))
    prof = Profile(prog)
    results, bad = [], 0
    for clip in a.clips:
        res = run_clip(prog, prof, clip, a.frames, a.v)
        first = res['frames'][0] if res['frames'] else None
        mbs = res['mbs'] / max(1, len(res['frames'])) if first else 1
        report(res, mbs, a.funcs)
        res['mbs_per_frame'] = mbs
        results.append(res)
        bad += res['mismatched'] + res['errors'] + (not res['frames'])
    if a.json:
        with open(a.json, 'w') as f:
            json.dump(results, f, indent=1)
    print('SIM H264 %s' % ('PASSED' if not bad else 'FAILED'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
