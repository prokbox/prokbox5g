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
"""The AAC-LC decoder (bcmfw/aac: Rockbox's libfaad) on the VideoCore II model.

Compiles bcmfw/aac/SOURCES with vc2cc (--conservative, --no-float), as
bcmfw/build.py builds the image, runs it in vc2sim on the clips' access
units, and compares each AU's output with the host build of the same
sources (run.sh writes build/aac/NAME.s16 and NAME.raw): the 16-bit PCM
and libfaad's 32-bit output, byte for byte.  Reports instructions per AU
(the cycle estimate: about one instruction per cycle on dependent chains,
docs/bcm2722-reference/quirks.md #5), per stage, the stack high-water
mark, the image's code/table/state sizes, and the instruction kinds the
decoder executes that Apple's code never runs in the vc2sim self-test
(hostproto_test.py's t_coverage_vp rule).

Memory, in the simulator's flat space:
  0x0200000  image (code, tables, libfaad's static state)
  0x1000000  AudioSpecificConfig, 0x1000100 the access unit
  0x1010000  PCM out (4 KB), 0x1011000 rate/channels words
  0x1f00000  stack top

Usage:
  bcmfw/aac/test/aac_sim_test.py [--aus N] [--json OUT] [--no-cover]
                                 [--funcs K] [CLIP ...]
      CLIP: a name in build/aac/clips (default: all); --aus N: the first
      N AUs of each clip (0: all; default 12)
Exit 0 when every AU is bit-exact.  Skips (exit 0) without vc4-elf-gcc.
"""
import bisect
import collections
import json
import os
import re
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
AAC = os.path.dirname(HERE)
ROOT = os.path.dirname(os.path.dirname(AAC))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'bcmre'))
sys.path.insert(0, HERE)
import vc2cc as C    # noqa: E402
import vc2sim as S   # noqa: E402
from adts2au import read_au  # noqa: E402

OUT = os.path.join(ROOT, 'build', 'aac')
CLIPS = os.path.join(OUT, 'clips')
SIM_OUT = os.path.join(OUT, 'sim')
FAAD = os.path.join(ROOT, 'lib', 'rbcodec', 'codecs', 'libfaad')
LIB = os.path.join(ROOT, 'lib', 'rbcodec', 'codecs', 'lib')
# as run.sh's CFG, plus bcmfw's build flags (build.py CFLAGS); upstream
# warnings are not ours (run.sh builds aac.c with -Werror on the host)
CFLAGS = ['-std=gnu99', '-funsigned-char',
          '-include', os.path.join(AAC, 'aac_config.h'),
          '-I' + os.path.join(AAC, 'sys'), '-I' + FAAD, '-I' + LIB,
          '-I' + os.path.join(ROOT, 'lib', 'rbcodec'), '-I' + AAC,
          '-w', '-fno-tree-loop-distribute-patterns']

ASC_AT, AU_AT, PCM_AT, OUTW_AT = 0x1000000, 0x1000100, 0x1010000, 0x1011000
STACK_PAINT = 0x10000            # bytes below the stack top painted
PAINT = 0xA5
MAX_STEPS = 50_000_000
CLOCKS = (150, 100)              # MHz: full clock, stage 8b's reduced one
# kinds the self-test never runs but whose results the timing bench's
# scalar checks verify on silicon since S3 (bench_test.py CHECKED,
# isa-scalar.md "Verified on silicon"); reported, not counted as unproven
BENCH_CHECKED = frozenset({'alu_reg asr', 'bcc gt', 'bcc lt', 'bcc le',
                           'bcc ls', 'ldst_ind ldsb'})

# per-function self time -> stage
STAGES = [
    ('bitstream', ['faad_', 'raw_data_block', 'decode_sce_lfe', 'decode_cpe',
                   'single_lfe_channel_element', 'channel_pair_element',
                   'individual_channel_stream', 'ics_info', 'section_data',
                   'scale_factor_data', 'spectral_data', 'side_info',
                   'pulse_data', 'tns_data', 'fill_element', 'extension_payload',
                   'data_stream_element', 'program_config_element',
                   'byte_align', 'window_grouping_info', 'huffman_',
                   'sign_bits', 'escape', 'huffman_getescape']),
    ('dequant', ['quant_to_spec', 'iquant', 'reconstruct_', 'apply_scalefactors',
                 'pulse_decode']),
    ('stereo/pns/tns', ['ms_decode', 'is_decode', 'pns_decode', 'tns_',
                        'gen_rand_vector', 'random_int', 'drc_']),
    ('imdct', ['ff_imdct', 'ff_fft', 'fft', 'pass', 'imdct']),
    ('window/overlap', ['ifilter_bank', 'vector_fmul', 'vect_']),
    ('output', ['aac_decode', 'to_s16']),
    ('libc', ['memcpy', 'memset', 'aac_memcpy', 'aac_memset']),
]
STAGE_ORDER = [s for s, _ in STAGES] + ['other']


def stage_of(name):
    for st, prefixes in STAGES:
        for p in prefixes:
            if name == p or name.startswith(p):
                return st
    return 'other'


def sources():
    with open(os.path.join(AAC, 'SOURCES')) as f:
        return [os.path.join(ROOT, ln.strip()) for ln in f
                if ln.strip() and not ln.startswith('#')]


def build():
    os.makedirs(os.path.join(SIM_OUT, 'lib'), exist_ok=True)
    text = C.compile_c(os.path.join(ROOT, 'bcmfw', 'lib', 'string.c'), '-O2',
                       ['-I' + os.path.join(ROOT, 'bcmfw'), '-std=gnu99',
                        '-fno-tree-loop-distribute-patterns'])
    with open(os.path.join(SIM_OUT, 'lib', 'string.s'), 'w') as f:
        f.write(text)
    prog = C.build(sources(), '-O2', extra=CFLAGS,
                   libs=[os.path.join(SIM_OUT, 'lib')] + C.DEFAULT_LIBS,
                   no_float=True, conservative=True)
    with open(os.path.join(SIM_OUT, 'aac.map'), 'w') as f:
        prog.write_map(f)
    lst = C.listing(prog)
    with open(os.path.join(SIM_OUT, 'aac.lst'), 'w') as f:
        f.write(lst + '\n')
    return prog, lst


def sizes(prog):
    """Section sizes, and the biggest objects of .bss/.rodata/.data."""
    e = prog.ends
    sec = {'text': e['text'] - prog.base, 'rodata': e['rodata'] - e['text'],
           'data': e['data'] - e['rodata'], 'bss': e['bss'] - e['data']}
    syms = sorted((a, n) for n, a in prog.symbols.items()
                  if not re.match(r'__\w+_(start|end)$', n))
    big = {}
    for k, (a, n) in enumerate(syms):
        nxt = syms[k + 1][0] if k + 1 < len(syms) else e['bss']
        for s, lo, hi in (('rodata', e['text'], e['rodata']),
                          ('data', e['rodata'], e['data']),
                          ('bss', e['data'], e['bss'])):
            if lo <= a < hi:
                big.setdefault(s, []).append((min(nxt, hi) - a, n))
    for s in big:
        big[s].sort(reverse=True)
    return sec, big


class ProfSim(S.Sim):
    """vc2sim with a per-address execution count (as h264's sim_test)."""
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


def s32(v):
    return v - (1 << 32) if v & 0x80000000 else v


def host_reference(name):
    """[(frames, pcm bytes, raw 2x1024 words bytes)] per AU from run.sh."""
    raw = open(os.path.join(OUT, name + '.raw'), 'rb').read()
    pcm = open(os.path.join(OUT, name + '.s16'), 'rb').read()
    rec, out, p = 4 + 8192, [], 0
    for k in range(len(raw) // rec):
        n = struct.unpack_from('<I', raw, k * rec)[0]
        out.append((n, pcm[p:p + 4 * n], raw[k * rec + 4:(k + 1) * rec]))
        p += 4 * n
    return out


def run_clip(prog, prof, name, max_aus, verbose):
    asc, aus = read_au(os.path.join(CLIPS, name + '.au'))
    ref = host_reference(name)
    if len(ref) != len(aus):
        raise SystemExit('%s: host reference has %d AUs, the clip %d (run '
                         'run.sh --host first)' % (name, len(ref), len(aus)))
    if max_aus:
        aus = aus[:max_aus]
    sim = ProfSim()
    C.load_into_sim(prog, sim)
    top = S.STACK_TOP
    sim.mem.write(top - STACK_PAINT, bytes([PAINT]) * STACK_PAINT)
    sim.mem.write(ASC_AT, asc)
    r = s32(sim.call('aac_open', ASC_AT, len(asc), OUTW_AT, OUTW_AT + 4,
                     max_steps=MAX_STEPS))
    open_steps = sim.steps
    if r:
        raise SystemExit('%s: aac_open returned %d' % (name, r))
    rate, chans = sim.mem.r32(OUTW_AT), sim.mem.r32(OUTW_AT + 4)
    raw_at = [sim.call('aac_raw_output', ch) for ch in (0, 1)]
    imdct = prog.symbols['ff_imdct_calc']
    res = {'clip': name, 'rate': rate, 'channels': chans, 'aus': [],
           'mismatched': 0, 'open_instr': open_steps}
    t0 = time.time()
    mark = collections.Counter()
    for k, au in enumerate(aus):
        sim.mem.write(AU_AT, au)
        s0 = sim.steps
        n = s32(sim.call('aac_decode', AU_AT, len(au), PCM_AT,
                         max_steps=MAX_STEPS))
        steps = sim.steps - s0
        got_pcm = sim.mem.read(PCM_AT, 4 * n) if n > 0 else b''
        got_raw = sim.mem.read(raw_at[0], 4096) + sim.mem.read(raw_at[1], 4096)
        rn, rpcm, rraw = ref[k]
        exact = n == rn and got_pcm == rpcm and got_raw == rraw
        delta = sim.pcount - mark
        mark = collections.Counter(sim.pcount)
        fc = prof.by_function(delta)
        st = collections.Counter()
        for f, c in fc.items():
            st[stage_of(f)] += c
        res['aus'].append({'n': k, 'bytes': len(au), 'frames': n, 'instr': steps,
                           'imdct_calls': delta[imdct], 'exact': exact,
                           'stages': dict(st), 'funcs': dict(fc)})
        if not exact:
            res['mismatched'] += 1
            what = 'frames %d vs %d' % (n, rn) if n != rn else \
                'PCM' if got_pcm != rpcm else 'raw output'
            print('  %s AU %d: MISMATCH (%s)' % (name, k, what))
        if verbose:
            print('  %s AU %d: %d bytes, %d frames, %s, %d instructions (%.1f s)' % (
                name, k, len(au), n, 'bit-exact' if exact else 'MISMATCH',
                steps, time.time() - t0))
    # the stack's high-water mark: the lowest painted byte overwritten
    low = sim.mem.read(top - STACK_PAINT, STACK_PAINT)
    used = next((i for i, b in enumerate(low) if b != PAINT), STACK_PAINT)
    res['stack'] = STACK_PAINT - used
    res['seconds'] = time.time() - t0
    return res


def report(res, funcs):
    aus = [a for a in res['aus'] if a['frames'] > 0]   # the priming AU apart
    ch = res['channels']
    short = sum(max(0, (a['imdct_calls'] - ch)) // 7 for a in aus)
    print('%s: %d Hz, %d ch, %d AUs, %d mismatched: %s (%.0f s in vc2sim)' % (
        res['clip'], res['rate'], ch, len(res['aus']), res['mismatched'],
        'bit-exact' if not res['mismatched'] else 'FAIL', res['seconds']))
    if not aus:
        return None
    ins = [a['instr'] for a in aus]
    mean = sum(ins) / len(ins)
    print('  %.0f k instructions per 1024-frame AU (min %.0f k, max %.0f k); '
          '%s; AU mean %d bytes; short-window channel-frames %d of %d' % (
              mean / 1e3, min(ins) / 1e3, max(ins) / 1e3,
              ', '.join('%.2f ms at %d MHz' % (mean / mhz / 1e3, mhz)
                        for mhz in CLOCKS),
              sum(a['bytes'] for a in aus) / len(aus), short, ch * len(aus)))
    st = collections.Counter()
    for a in aus:
        st.update(a['stages'])
    tot = sum(st.values())
    print('    ' + ', '.join('%s %.0f%%' % (s, 100.0 * st[s] / tot)
                             for s in STAGE_ORDER if st[s]))
    if funcs:
        fc = collections.Counter()
        for a in aus:
            fc.update(a['funcs'])
        print('    top: ' + ', '.join('%s %.1fk' % (n, c / len(aus) / 1e3)
                                     for n, c in fc.most_common(funcs)))
    return mean, max(ins)


def coverage(ours):
    """Kinds we execute that the vc2sim self-test (Apple's code) never runs."""
    import contextlib
    import io
    import vc2forms as F
    import vc2sim_tests
    vmcs = os.path.expanduser('~/Git/re/bcm2722/extract/1.3/vc/VideoCore/Boot/vmcs.bin')
    if not os.path.exists(vmcs):
        return None
    S.COVER = collections.Counter()
    with contextlib.redirect_stdout(io.StringIO()):
        rc = vc2sim_tests.main([os.path.dirname(os.path.dirname(vmcs))])
    apple = set(S.COVER)
    S.COVER = None
    if rc:
        print('coverage: the vc2sim self-test failed')
    extra = sorted(set(ours) - apple)
    accepted = F.APPLE_STATIC_ONLY | F.DEVICE_VERIFIED | BENCH_CHECKED
    return {'executed': len(ours), 'not_in_selftest': extra,
            'unproven': [k for k in extra if k not in accepted],
            'counts': {k: ours[k] for k in extra}}


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('clips', nargs='*')
    ap.add_argument('--aus', type=int, default=12)
    ap.add_argument('--json')
    ap.add_argument('--funcs', type=int, default=0)
    ap.add_argument('--no-cover', action='store_true')
    ap.add_argument('-v', action='store_true')
    a = ap.parse_args(argv)
    if not os.path.exists(C.GCC):
        print('aac_sim_test: vc4-elf-gcc not found (%s): skipped' % C.GCC)
        return 0
    prog, lst = build()
    sec, big = sizes(prog)
    print('image: text %d, rodata %d, data %d, bss %d bytes' % (
        sec['text'], sec['rodata'], sec['data'], sec['bss']))
    for s in ('rodata', 'data', 'bss'):
        print('  %s: ' % s + ', '.join('%s %d' % (n, sz) for sz, n in big.get(s, [])[:8]))
    unproven_forms = lst.count('; unproven form')
    print('listing: %d instructions of a form absent from Apple\'s static code' %
          unproven_forms)
    clips = a.clips or sorted(f[:-3] for f in os.listdir(CLIPS) if f.endswith('.au'))
    prof = Profile(prog)
    if not a.no_cover:
        S.COVER = collections.Counter()
    results, bad, worst = [], 0, []
    for name in clips:
        res = run_clip(prog, prof, name, a.aus, a.v)
        m = report(res, a.funcs)
        if m:
            worst.append((m[0], m[1], name))
        results.append(res)
        bad += res['mismatched'] + (not res['aus'])
    print('stack high-water mark: %d bytes' % max(r['stack'] for r in results))
    if worst:
        mean = sum(w[0] for w in worst) / len(worst)
        print('all clips: %.0f k instructions per AU on average, worst AU %.0f k (%s)' % (
            mean / 1e3, max(w[1] for w in worst) / 1e3,
            max(worst, key=lambda w: w[1])[2]))
    cov = None
    if not a.no_cover:
        ours = collections.Counter(S.COVER)
        S.COVER = None
        cov = coverage(ours)
        if cov is None:
            print('coverage: Apple\'s vmcs.bin not found: not checked')
        else:
            print('coverage: %d instruction kinds executed, %d not run by the '
                  'self-test: %s' % (cov['executed'], len(cov['not_in_selftest']),
                                     ', '.join('%s (x%d)' % (k, cov['counts'][k])
                                               for k in cov['not_in_selftest']) or
                                     'none'))
            if cov['unproven']:
                print('coverage: UNPROVEN kinds: ' + ', '.join(cov['unproven']))
    if a.json:
        with open(a.json, 'w') as f:
            json.dump({'sizes': sec, 'results': results, 'coverage': cov,
                       'unproven_forms': unproven_forms}, f, indent=1)
    print('SIM AAC %s' % ('PASSED' if not bad else 'FAILED'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
