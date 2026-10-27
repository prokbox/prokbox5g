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

"""The JPEG spike's step S2 (docs/research/bcm2722/jpeg-spike.md): the
shell's decoder and resizer, unchanged (apps/recorder/jpeg_load.c with
JPEG_FROM_MEM, resize.c, bmp.c, against bcmfw/jpeg/env), built by vc2cc
and run in vc2sim on the corpus (tests/jpeg/make_corpus.sh).  Every
output must equal the golden table (tests/jpeg/golden.txt, made by the
Mac build of the same sources from build/sim-ui's objects); the
instruction counts per source megapixel and per output are the spike's
cost figures.

Decode-once (jdec.h, jpeg-spike.md section 8): each file's outputs in
one jdec_run must equal tests/jpeg/golden_once.txt, on the Mac (jdec,
J_OP_DECODE, and the definition computed the plain way, jdoracle.c) and
in vc2sim; its primary output must equal golden.txt.  --make-once writes
golden_once.txt from the Mac's jdec once all three agree.

Progressive (section 9): tests/jpeg/make_prog.sh's pairs in
build/jpeg-prog (JPEG_PROG), each progressive file against its baseline
twin, alone and decoded once; a truncated one, one past JPEG_PROG_MAX.

Usage: bcmfw/tests/jpeg_test.py [--all] [--files a,b] [--make-once]
                                [--singles | --once]
  default: the files up to 1000 px (light); --all: every file (heavy:
  run after 22:00).  In vc2sim each output decoded alone, then each
  file's set decoded once; --singles or --once: only those.
  JPEG_CORPUS: another corpus directory.
"""
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.dirname(HERE)
ROOT = os.path.dirname(FW)
sys.path.insert(0, os.path.join(ROOT, 'tools', 'bcmre'))
import vc2cc as C     # noqa: E402
import vc2sim as S    # noqa: E402

REC = os.path.join(ROOT, 'apps', 'recorder')
JPEG = os.path.join(FW, 'jpeg')
JOBS = os.path.join(FW, 'jobs')
CORPUS = os.environ.get('JPEG_CORPUS', os.path.join(ROOT, 'build', 'jpeg', 'corpus'))
GOLDEN = os.path.join(ROOT, 'tests', 'jpeg', 'golden.txt')
GOLDEN_ONCE = os.path.join(ROOT, 'tests', 'jpeg', 'golden_once.txt')
PROG = os.environ.get('JPEG_PROG', os.path.join(ROOT, 'build', 'jpeg-prog'))
# golden.txt is the shell's decoder (S1), which refuses progressive files;
# jdec decodes them to the pixels of their baseline twin (section 9)
PROG_TWINS = {'t600_prog.jpg': 't600_420_q85.jpg'}
ART_SET = ('300', '300', '142', '142', '55', '55')
CFLAGS = ['-std=gnu11', '-I' + os.path.join(JPEG, 'env', 'bcm'),
          '-I' + os.path.join(JPEG, 'env'), '-I' + REC,
          '-I' + JPEG, '-I' + JOBS, '-I' + os.path.join(FW, 'video'),
          '-fno-tree-loop-distribute-patterns', '-Wno-attributes']
JPG_AT, OUT_AT, WORK_AT, WORK_MAX = 0x800000, 0x7F0000 - 0x300000, 0xC00000, 0x800000
WH_AT = OUT_AT - 0x100


def vc_build(extra=(), defs=(), rec=REC):
    """jdec, the decoder and resizer built by vc2cc as the display-only
    image builds them, with the image's memcpy and memset (bcmfw/lib/
    string.c, word-wise), so the counts are the image's; defs: more -D
    flags, rec: another apps/recorder (jskip_test.py)"""
    lib = ('string.s', C.compile_c(os.path.join(FW, 'lib', 'string.c'), '-O2',
                                   ['-I' + FW, '-std=gnu99', '-fno-tree-loop-distribute-patterns']))
    return C.build([os.path.join(rec, 'jpeg_load.c'), os.path.join(rec, 'resize.c'),
                    os.path.join(rec, 'bmp.c'), os.path.join(JOBS, 'jdec.c'),
                    os.path.join(JPEG, 'jdone.c'), os.path.join(JPEG, 'libc.c'),
                    # the colour stage and the 8x8 IDCT on the vector unit,
                    # as the image runs them (jv_idct4n stores through
                    # jobs_kernels.S's jv_store16)
                    os.path.join(FW, 'video', 'jdec_vpu.c'),
                    os.path.join(FW, 'video', 'jdec_kernels.S'),
                    os.path.join(FW, 'video', 'jobs_kernels.S')] + list(extra) +
                   [lib], '-O2', extra=CFLAGS + ['-D' + d for d in defs], libs=C.DEFAULT_LIBS,
                   no_float=True, conservative=True)


def sof_size(d):
    i = 2
    while i + 9 < len(d):
        if d[i] != 0xFF:
            return None
        m, ln = d[i + 1], d[i + 2] << 8 | d[i + 3]
        if 0xC0 <= m <= 0xC3:
            return d[i + 7] << 8 | d[i + 8], d[i + 5] << 8 | d[i + 6]
        i += 2 + ln
    return None


def fnv(b):
    v = 2166136261
    for x in b:
        v = ((v ^ x) * 16777619) & 0xFFFFFFFF
    return v


# jdec's contract on top of the decoder (jdec.h): these fail with a code
# instead of the picture the bare decoder makes
JDEC_E = {'UNSUPPORTED': -1, 'CORRUPT': -2, 'TRUNCATED': -3}
J_E = {'UNSUPPORTED': 26, 'CORRUPT': 27, 'TRUNCATED': 28}
TO_J_E = {str(JDEC_E[k]): str(J_E[k]) for k in JDEC_E}


def expect(name, want, codes):
    """What jdec (codes JDEC_E) or the DECODE job (J_E) must print"""
    if want[2] in ('-3', '-4'):
        return ['0', '0', str(codes['UNSUPPORTED']), '0']
    if '_trunc' in name:
        return ['0', '0', str(codes['TRUNCATED']), '0']
    if '_badhuff' in name:      # the decoder's count of bad codes (#125)
        return ['0', '0', str(codes['CORRUPT']), '0']
    return want


def as_job(want):
    """golden_once's jdec line as the DECODE job prints it"""
    if want[0] == '0' and want[2] in TO_J_E:
        return ['0', '0', TO_J_E[want[2]], '0']
    return want


def groups(table):
    """{file: [(w, h, want), ...]} in table order: one decode-once set each"""
    g = {}
    for name, w, h, want in table:
        g.setdefault(name, []).append((w, h, want))
    return g


def primary(outs):
    """jdec_primary: the largest by area, the first of equals"""
    p = 0
    for k, (w, h, _) in enumerate(outs):
        if int(w) * int(h) > int(outs[p][0]) * int(outs[p][1]):
            p = k
    return p


def calc_scale(i, o):
    """jpeg_load.c's calc_scale: the IDCT scale (0: 1/8 .. 3: 1) for an
    output o pixels wide from i"""
    o <<= 3
    for s in range(3):
        if o <= i:
            return s
        i <<= 1
    return 3


def with_twins(golden):
    """golden.txt with each progressive file's lines its twin's"""
    tw = {(n, w, h): want for n, w, h, want in golden}
    return [(n, w, h, tw.get((PROG_TWINS[n], w, h), want) if n in PROG_TWINS else want)
            for n, w, h, want in golden]


def prog_cases():
    """make_prog.sh's files: [(file, twin or None, expected code or None)]"""
    if not os.path.isdir(PROG):
        return None
    out = []
    for f in sorted(os.listdir(PROG)):
        if f.startswith('p_'):
            out.append((f, 'b_' + f[2:], None))
        elif f == 'x_t600_trunc.jpg':
            out.append((f, None, 'TRUNCATED'))
        elif f == 'x_t4000_big.jpg':
            out.append((f, None, 'UNSUPPORTED'))
    return out


def mac_prog(exe):
    """Each progressive case on the Mac: (differences, {file: the expected
    art-set lines, decoded once})"""
    import subprocess

    def run(*a):
        return subprocess.run([exe] + list(a), capture_output=True, text=True).stdout.split('\n')[:-1]
    bad, want = [], {}
    for f, twin, code in prog_cases():
        path = os.path.join(PROG, f)
        if twin:
            for sizes in (ART_SET[0:2], ART_SET[2:4], ART_SET[4:6], ART_SET):
                gp, gb = run(path, *sizes), run(os.path.join(PROG, twin), *sizes)
                if gp != gb or not gb or gb[0].startswith('0 0'):
                    bad.append((f, ' '.join(sizes), '', 'prog', gp, gb))
            want[f] = run(os.path.join(PROG, twin), *ART_SET)
        else:
            want[f] = ['0 0 %d 0' % JDEC_E[code]] * 3
            if run(path, *ART_SET) != want[f]:
                bad.append((f, ' '.join(ART_SET), '', 'prog', run(path, *ART_SET), want[f]))
        g = run('--jref', path, *ART_SET)
        if g != [' '.join(as_job(x.split())) for x in want[f]]:
            bad.append((f, ' '.join(ART_SET), '', 'prog --jref', g, want[f]))
    return bad, want


def read_table(path):
    if not os.path.exists(path):
        return []
    return [(ln.split()[0], ln.split()[1], ln.split()[2], ln.split()[3:])
            for ln in open(path) if ln.strip() and not ln.startswith('#')]


def mac_build(jdec_c=None, out=None, rec=REC, defs=(), cflags=(), cc='cc'):
    """jdtest (bcmfw/jpeg/jdtest.c) on the Mac: jdec, the shell's decoder
    and resizer, the oracle and the executor.  jdec_c: another jdec.c (a
    mutant, jscan_test.py), into out; rec: another apps/recorder (a mutant
    of jpeg_load.c, jskip_test.py); defs: more -D flags for the decoder;
    cflags: more flags for every object and the link, cc: the compiler
    (sanitizers, jpeg_oob_test.py).  The exe, or None if a build failed"""
    import subprocess
    out = out or os.path.join(ROOT, 'build', 'jdec')
    os.makedirs(out, exist_ok=True)
    env = ['-I' + os.path.join(JPEG, 'env'), '-I' + rec, '-I' + REC, '-I' + JOBS, '-I' + FW,
           '-I' + JPEG] + ['-D' + d for d in defs]
    objs = []
    for src, inc in ([(os.path.join(rec, f), env) for f in ('jpeg_load.c', 'resize.c', 'bmp.c')] +
                     [(jdec_c or os.path.join(JOBS, 'jdec.c'), env), (os.path.join(JPEG, 'jdone.c'), env),
                      (os.path.join(JPEG, 'jdoracle.c'), env),
                      # J_OP_DECODE through the executor (jref.h JREF_DECODE)
                      (os.path.join(JPEG, 'jdtest.c'), ['-I' + FW, '-I' + JOBS, '-DJREF_DECODE']),
                      (os.path.join(JOBS, 'jref.c'), ['-I' + FW, '-I' + JOBS, '-DJREF_DECODE'])]):
        o = os.path.join(out, os.path.basename(src)[:-2] + '.o')
        r = subprocess.run([cc, '-std=gnu11', '-O2', '-c', src, '-o', o] + list(cflags) + inc,
                           capture_output=True, text=True)
        if r.returncode:
            print('FAIL Mac build of %s:\n%s' % (src, r.stderr))
            return None
        objs.append(o)
    exe = os.path.join(out, 'jdtest')
    if subprocess.run([cc, '-o', exe] + list(cflags) + objs).returncode:
        return None
    return exe


def mac(golden, once, make_once=False):
    """jdec (bcmfw/jobs/jdec.c) on the Mac, alone and as J_OP_DECODE
    through the executor (bcmfw/jpeg/jdtest.c): every golden line, fast;
    then each file's set decoded once (jdec, the job, the oracle).
    Returns the lines that differ, or None if the build failed; with
    make_once, the new golden_once lines too."""
    import subprocess
    exe = mac_build()
    if exe is None:
        return None
    bad = []
    for name, w, h, want in golden:
        for mode, codes in (([], JDEC_E), (['--jref'], J_E)):
            got = subprocess.run([exe] + mode + [os.path.join(CORPUS, name), w, h],
                                 capture_output=True, text=True).stdout.split()
            if got != expect(name, want, codes):
                bad.append((name, w, h, ' '.join(mode) or 'jdec', got, expect(name, want, codes)))
    single = {(n, w, h): expect(n, want, JDEC_E) for n, w, h, want in golden}
    want_once = {(n, w, h): want for n, w, h, want in once}
    made = []
    for name, outs in groups(golden).items():
        args = [x for w, h, _ in outs for x in (w, h)]
        res = {}
        for mode in ('jdec', '--oracle', '--jref'):
            r = subprocess.run([exe] + ([] if mode == 'jdec' else [mode]) +
                               [os.path.join(CORPUS, name)] + args,
                               capture_output=True, text=True).stdout.split('\n')
            res[mode] = [ln.split() for ln in r[:len(outs)]]
        p = primary(outs)
        sz = sof_size(open(os.path.join(CORPUS, name), 'rb').read())
        for k, (w, h, _) in enumerate(outs):
            got = res['jdec'][k]
            # the definition: the plain way agrees, the job agrees, and the
            # primary is the single decode at its size
            if res['--oracle'][k] != got:
                bad.append((name, w, h, 'oracle', res['--oracle'][k], got))
            if res['--jref'][k] != as_job(got):
                bad.append((name, w, h, 'once --jref', res['--jref'][k], as_job(got)))
            # the primary is the single decode at its size; so is any
            # output whose own IDCT scale is the primary's (the same rows)
            same = sz and all(calc_scale(sz[a], int(outs[k][a])) ==
                              calc_scale(sz[a], int(outs[p][a])) for a in (0, 1))
            if (k == p or same) and got != single[(name, w, h)]:
                bad.append((name, w, h, 'once = single', got, single[(name, w, h)]))
            if make_once:
                made.append('%-25s %4s %4s  %s' % (name, w, h, ' '.join(got)))
            elif got != want_once.get((name, w, h)):
                bad.append((name, w, h, 'once', got, want_once.get((name, w, h))))
    if not make_once and prog_cases() is not None:
        pb, pwant = mac_prog(exe)
        bad += pb
        mac.prog_want = pwant
    return (bad, made) if make_once else bad


def main(argv):
    if not os.path.exists(C.GCC):
        print('skip: vc4-elf-gcc not found')
        return 0
    if not os.path.isdir(CORPUS):
        print('jpeg_test: corpus missing: tests/jpeg/make_corpus.sh')
        return 1
    every = '--all' in argv
    only = None
    if '--files' in argv:
        only = set(argv[argv.index('--files') + 1].split(','))
    t0 = time.time()
    golden = with_twins(read_table(GOLDEN))
    once = read_table(GOLDEN_ONCE)
    if prog_cases() is None and '--make-once' not in argv:
        print('jpeg_test: progressive cases missing: tests/jpeg/make_prog.sh')
        return 1
    if '--make-once' in argv:
        r = mac(golden, once, make_once=True)
        if r is None:
            return 1
        mbad, made = r
        for b in mbad:
            print('  DIFF %s %s %s (%s): got %s want %s' % b)
        if mbad:
            print('jpeg_test: not writing golden_once.txt')
            return 1
        with open(GOLDEN_ONCE, 'w') as f:
            f.write('# file asked-w asked-h  got-w got-h bytes fnv1a, or 0 0 <JDEC_E_*> 0:'
                    ' each file\'s outputs\n# in one jdec_run (decode-once, jdec.h);'
                    ' made by jpeg_test.py --make-once\n')
            f.write('\n'.join(made) + '\n')
        print('jpeg_test: wrote %d lines to %s' % (len(made), GOLDEN_ONCE))
        return 0
    if not once:
        print('jpeg_test: tests/jpeg/golden_once.txt missing: --make-once')
        return 1
    mbad = mac(golden, once)
    if mbad is None:
        return 1
    print('Mac, jdec and J_OP_DECODE: %d outputs each, decoded once per output and '
          'once per file; %d differ' % (len(golden), len(mbad)))
    for b in mbad[:8]:
        print('  DIFF %s %s %s (%s): got %s want %s' % b)
    prog = vc_build()
    text = prog.ends['text'] - prog.symbols['__text_start']
    print('vc2cc: decoder and resizer, %d bytes of code' % text)
    s = S.Sim()
    C.load_into_sim(prog, s)
    bad = n = 0
    per_mpx = []
    want_once = {(nm, w, h): want for nm, w, h, want in once}
    singles_only = '--singles' in argv
    once_only = '--once' in argv

    def run(entry, args, data, sz, label):
        """one call in vc2sim: (rc, steps) or None on a SimError"""
        s.mem.b[JPG_AT:JPG_AT + len(data)] = data
        s0 = s.steps
        try:
            rc = s.call(entry, JPG_AT, len(data), OUT_AT, *args, WORK_AT,
                        stack_args=(WORK_MAX,), max_steps=8_000_000_000)
        except S.SimError as e:
            print('FAIL %s: %s' % (label, str(e).split('\n')[0]))
            return None
        return (rc - (1 << 32) if rc & 0x80000000 else rc), s.steps - s0

    for name, outs in groups(golden).items():
        if only and name not in only:
            continue
        data = open(os.path.join(CORPUS, name), 'rb').read()
        sz = sof_size(data)
        if not every and only is None and sz and max(sz) > 1000:
            continue
        mpx = sz[0] * sz[1] / 1e6 if sz else 0
        single_steps = 0
        for w, h, want in ([] if once_only else outs):
            r = run('jdec_one', (int(w), int(h)), data, sz, '%s %s %s' % (name, w, h))
            n += 1
            if r is None:
                bad += 1
                continue
            rc, steps = r
            single_steps += steps
            if rc:
                got = ['0', '0', str(rc), '0']
            else:
                nb = int(w) * int(h) * 2
                got = [w, h, str(nb), '%08x' % fnv(bytes(s.mem.b[OUT_AT:OUT_AT + nb]))]
            want = expect(name, want, JDEC_E)
            ok = got == want
            bad += not ok
            if mpx and rc == 0:
                per_mpx.append(steps / 1e6 / mpx)
            print('%-24s %4s %4s  %s  %7.2f M instructions%s%s' % (
                name, w, h, 'ok  ' if ok else 'DIFF', steps / 1e6,
                ' (%.1f M per source Mpx)' % (steps / 1e6 / mpx) if mpx and rc == 0 else '',
                '' if ok else '  got %s want %s' % (' '.join(got), ' '.join(want))))
        if singles_only:
            continue
        # decode-once: the file's outputs in one jdec_run, packed at OUT_AT
        wh = [int(w) | int(h) << 16 for w, h, _ in outs]
        for k, v in enumerate(wh):
            s.mem.b[WH_AT + 4 * k:WH_AT + 4 * k + 4] = v.to_bytes(4, 'little')
        r = run('jdec_many', (WH_AT, len(outs)), data, sz, '%s once' % name)
        n += len(outs)
        if r is None:
            bad += len(outs)
            continue
        rc, steps = r
        at, diffs = OUT_AT, []
        for w, h, _ in outs:
            nb = int(w) * int(h) * 2
            got = ['0', '0', str(rc), '0'] if rc else \
                [w, h, str(nb), '%08x' % fnv(bytes(s.mem.b[at:at + nb]))]
            at += nb
            if got != want_once.get((name, w, h)):
                diffs.append('%sx%s got %s want %s' % (w, h, ' '.join(got),
                                                        ' '.join(want_once.get((name, w, h)) or ['?'])))
        bad += len(diffs)
        print('%-24s once %-14s %s  %7.2f M instructions%s%s' % (
            name, ','.join('%sx%s' % (w, h) for w, h, _ in outs), 'ok  ' if not diffs else 'DIFF',
            steps / 1e6,
            ' (singles %.2f M: %.0f %% saved)' % (single_steps / 1e6, 100 - 100.0 * steps / single_steps)
            if single_steps and rc == 0 else '', ''.join('  ' + d for d in diffs)))
    # progressive: each file's art set decoded once, as its twin on the Mac
    for f, twin, code in ([] if singles_only else prog_cases()):
        if only and f not in only:
            continue
        data = open(os.path.join(PROG, f), 'rb').read()
        sz = sof_size(data) or (0, 0)
        if not every and only is None and max(sz) > 1000:
            continue
        wh = [int(ART_SET[2 * k]) | int(ART_SET[2 * k + 1]) << 16 for k in range(3)]
        for k, v in enumerate(wh):
            s.mem.b[WH_AT + 4 * k:WH_AT + 4 * k + 4] = v.to_bytes(4, 'little')
        r = run('jdec_many', (WH_AT, 3), data, sz, '%s once' % f)
        n += 3
        if r is None:
            bad += 3
            continue
        rc, steps = r
        at, got = OUT_AT, []
        for k in range(3):
            w, h = ART_SET[2 * k], ART_SET[2 * k + 1]
            nb = int(w) * int(h) * 2
            got.append('0 0 %d 0' % rc if rc else '%s %s %d %08x' % (
                w, h, nb, fnv(bytes(s.mem.b[at:at + nb]))))
            at += nb
        ok = got == mac.prog_want[f]
        bad += 0 if ok else 3
        print('%-24s once %-14s %s  %7.2f M instructions%s' % (
            f, 'art set', 'ok  ' if ok else 'DIFF', steps / 1e6,
            '' if ok else '  got %s want %s' % (got, mac.prog_want[f])))
    if per_mpx:
        print('instructions per source Mpx: %.1f-%.1f M (median %.1f M)' % (
            min(per_mpx), max(per_mpx), sorted(per_mpx)[len(per_mpx) // 2]))
    print('jpeg_test: %d outputs, %d differ (%.0f s)' % (n, bad, time.time() - t0))
    return 1 if bad or mbad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
