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

"""jdec's AC skip loop (apps/recorder/jpeg_load.c huff_block, JPEG_JDEC;
jpeg-spike.md section 12): the coefficients a scaled IDCT doesn't keep,
skipped by one table look a symbol (fix_huff_skip).

1. Corrupt files, the fast decoder against the decoder before the skip
   table (jpeg_load.c at BASE, from git), on the Mac and in vc2sim: every
   status and every pixel must be the same.  BASE gets search_restart's
   bound (RESTART_BOUND, jpeg_oob_test.py): before it, a restart could set
   a bit count past the bit buffer's width, and both loops then shifted
   by more than the word (undefined in C).  Against the shell's own
   bit-serial loop too (the same jpeg_load.c built with JDEC_BITSERIAL),
   on every file.  The files
   are made here from the corpus (into build/jskip): bits flipped in the
   entropy data, runs of noise, a stuffed zero dropped, restart markers
   put just before real ones (the bit count past 32 that a restart then
   set, before the bound), and cuts; with the sources themselves, two of them made here
   with optimised Huffman tables (a ZRL short enough for the skip table).
   Each at 55 (1/8 scale: only DC kept), 142 (1/4) and the art set
   decoded once.
2. Mutations of the skip loop and its reader: each must fail 1, or the
   golden table (tests/jpeg/golden.txt) on the Mac, so the tests above
   can see a wrong skip.

Usage: bcmfw/tests/jskip_test.py [--quick]   (--quick: no vc2sim)
Needs the corpus (tests/jpeg/make_corpus.sh)."""
import os
import random
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.dirname(HERE)
ROOT = os.path.dirname(FW)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, 'tools', 'bcmre'))
import jpeg_test as J  # noqa: E402

OUT = os.path.join(ROOT, 'build', 'jskip')
BASE = '10caf83824'     # the decoder before the skip table (p19-bcm-prep)
# search_restart's bound (changes-from-rockbox.md), given to BASE too
RESTART_BOUND = ('        p_jpeg->bitbuf_bits = p_jpeg->marker_ind;\n',
                 '        if (p_jpeg->marker_ind <= p_jpeg->bitbuf_bits)\n'
                 '            p_jpeg->bitbuf_bits = p_jpeg->marker_ind;\n'
                 '        else\n'
                 '        {\n'
                 '            p_jpeg->huff_errors++;\n'
                 '            p_jpeg->bitbuf_bits = 0;\n'
                 '        }\n')
FILES = os.path.join(OUT, 'files')
SOURCES = ('t600_420_rst.jpg', 't600_420_q85.jpg', 't600_444_q85.jpg', 't1500_420_q85.jpg',
           'opt_t1500_q50.jpg', 'opt_noise500_q85.jpg', 'opt_t600_rst1.jpg')
# made here as make_corpus.sh makes the corpus, with optimised Huffman
# tables (cjpeg -optimize): codes the standard tables don't have, among
# them a ZRL of 8 and 9 bits (6 in the noise file), short enough for the
# skip table (the standard tables' ZRL is 10-11 bits, always the general
# way).  (name, source picture, cjpeg arguments)
# The third restarts at every MCU: a fill made a symbol early or late
# shows at the next restart (search_restart's bit count and position).
OPTIMISED = (('opt_t1500_q50.jpg', 't1500', ('-quality', '50')),
             ('opt_noise500_q85.jpg', 'noise500', ('-quality', '85')),
             ('opt_t600_rst1.jpg', 't600', ('-quality', '85', '-restart', '1B')))
PER_SOURCE = 40         # corrupt files from each source
SIM_FILES = 10          # of those, decoded in vc2sim (at 55, 1/8 scale)
SIZES = (['55', '55'], ['142', '142'], list(J.ART_SET))

# (text in jpeg_load.c, replacement, what it breaks).  Not here, because
# jdec's results can't show them: a symbol taken with its last magnitude
# bit still to come ('> 32' made '> 33': the fill it needs then comes just
# after it, and fills commute with taking bits, so the same fills happen
# at the same symbols), and the guard for a count past 32 ('<= 32' made
# '<= 48': that count came only from a corrupt file's restart, which
# search_restart now bounds, so it no longer arises); and a
# fill a few bits early (the skip loop fills from the buffer only when
# neither byte is 0xFF and no marker is pending, so an early fill reads
# the bytes the later one would, and a restart's search for its marker
# starts past data either way; the Mac, which runs the mutants, has a
# 64-bit buffer, so even an overfull one goes unseen there).
MUTANTS = [
    ('<< 8 | len;', '<< 8 | (len + 1);', 'a skip one bit long'),
    ('<< 8 | len;', '<< 8 | (len - (s != 0));', 'a skip one bit short'),
    ('r == 15 ? 16 : 64', 'r == 15 ? 15 : 64', 'ZRL one coefficient short'),
    ('(s ? r + 1 :', '(s ? r + 2 :', 'a run one coefficient long'),
    ('if (k_need <= 1)', 'if (k_need <= 4)', 'kept coefficients skipped'),
    ('(uintptr_t)rd->bp + p_jpeg->buf_left - 2;', '(uintptr_t)rd->bp + p_jpeg->buf_left - 1;',
     'a fill reading past the buffer'),
    ('rd->lim = p_jpeg->marker_val ? 0 :', 'rd->lim = 0 ? 0 :', 'a pending restart marker ignored'),
    ('return k + r + 1;', 'return k + r + 2;', 'a run one long the general way'),
]


def entropy(d):
    """(start, end) of the last scan's entropy data"""
    i, sos = 2, None
    while i + 4 <= len(d):
        if d[i] != 0xFF:
            break
        m, ln = d[i + 1], d[i + 2] << 8 | d[i + 3]
        if m == 0xDA:
            sos = i + 2 + ln
            break
        i += 2 + ln
    end = d.rfind(b'\xff\xd9')
    return sos, end


def corrupt(d, rnd, kind):
    a, b = entropy(d)
    d = bytearray(d)
    if kind == 'flip':
        for _ in range(rnd.randint(1, 8)):
            p = rnd.randrange(a, b)
            d[p] ^= 1 << rnd.randrange(8)
    elif kind == 'noise':
        p = rnd.randrange(a, b - 64)
        d[p:p + rnd.randint(1, 64)] = bytes(rnd.randrange(256) for _ in range(rnd.randint(1, 64)))
    elif kind == 'unstuff':
        st = [k for k in range(a, b - 1) if d[k] == 0xFF and d[k + 1] == 0]
        if st:
            del d[rnd.choice(st) + 1]
    elif kind == 'rst':
        # a restart marker a few bytes before a real one: the fills go on
        # past it, so the restart sets a bit count of 8 + 16 a fill, past
        # 32 after two.  Only a few bytes before: further, the count
        # passes the bit buffer's width, and the shell's decoder shifts by
        # more than that (undefined in C; the fast and bit-serial loops
        # then differ, as they did before the skip table)
        rst = [k for k in range(a, b - 1) if d[k] == 0xFF and 0xD0 <= d[k + 1] <= 0xD7]
        for r in sorted(rnd.sample(rst, min(len(rst), rnd.randint(1, 3))), reverse=True):
            p = r - rnd.randint(1, 6)
            while d[p - 1] == 0xFF:
                p += 1
            d[p:p] = bytes((0xFF, 0xD0 + rnd.randrange(8)))
    elif kind == 'cut':
        p = rnd.randrange(a, b)
        d = d[:p] + (b'\xff\xd9' if rnd.random() < 0.5 else b'')
    return bytes(d)


def variants():
    """[(name, bytes)], deterministic"""
    out = []
    kinds = ('flip', 'noise', 'unstuff', 'rst', 'cut')
    for src in SOURCES:
        d = open(os.path.join(OUT if src.startswith('opt_') else J.CORPUS, src), 'rb').read()
        out.append((src[:-4], d))
        rnd = random.Random(src)
        for n in range(PER_SOURCE):
            kind = 'rst' if src == 't600_420_rst.jpg' and n % 2 else kinds[n % len(kinds)]
            out.append(('%s_%s%02d' % (src[:-4], kind, n), corrupt(d, rnd, kind)))
    return out


def optimised():
    """OPTIMISED into OUT, from build/jpeg/src (make_corpus.sh); an error or None"""
    src = os.path.join(os.path.dirname(J.CORPUS), 'src')
    for name, pic, args in OPTIMISED:
        r = subprocess.run(['cjpeg', '-baseline', '-optimize'] + list(args) +
                           ['-outfile', os.path.join(OUT, name), os.path.join(src, pic + '.ppm')],
                           capture_output=True, text=True)
        if r.returncode:
            return 'cjpeg for %s: %s' % (name, r.stderr.strip() or r.returncode)
    return None


def run(exe, path, size):
    return subprocess.run([exe, path] + size, capture_output=True, text=True).stdout.split()


def outputs(exe, files):
    """{(name, size): output}"""
    return {(n, tuple(z)): run(exe, os.path.join(FILES, n + '.jpg'), z)
            for n, _ in files for z in SIZES}


def differ(got, want):
    return sorted('%s at %s' % (n, 'x'.join(z) if len(z) == 2 else 'art set')
                  for (n, z), v in got.items() if want[(n, z)] != v)


def mac_golden(exe):
    bad = []
    for name, w, h, want in J.with_twins(J.read_table(J.GOLDEN)):
        got = run(exe, os.path.join(J.CORPUS, name), [w, h])
        if got != J.expect(name, want, J.JDEC_E):
            bad.append('%s %sx%s' % (name, w, h))
    return bad


def base_rec():
    """apps/recorder's decoder at BASE, into OUT/base_src (None: no git),
    with its own headers: they guard what they declare as its sources do;
    with search_restart's bound (RESTART_BOUND)"""
    d = os.path.join(OUT, 'base_src')
    os.makedirs(d, exist_ok=True)
    for f in ('jpeg_load.c', 'resize.c', 'bmp.c', 'jpeg_load.h', 'resize.h', 'bmp.h',
              'jpeg_common.h'):
        r = subprocess.run(['git', '-C', ROOT, 'show', '%s:apps/recorder/%s' % (BASE, f)],
                           capture_output=True)
        if r.returncode:
            return None
        if f == 'jpeg_load.c':
            src = r.stdout.decode()
            if src.count(RESTART_BOUND[0]) != 1:
                return None
            r.stdout = src.replace(*RESTART_BOUND).encode()
        open(os.path.join(d, f), 'wb').write(r.stdout)
    return d


def sim(files, base):
    import vc2cc as C
    import vc2sim as S
    res = {}
    # BASE's decoder predates the 8x8 IDCT hook that jdec_vpu.c installs:
    # the hook's symbols as a stub, and the IDCT left in C
    stub = os.path.join(OUT, 'base_idct8.c')
    with open(stub, 'w') as f:
        f.write('#include <stdint.h>\nvoid (*jdec_idct)(uint8_t *px, int16_t *coef, int size);\n'
                'void (*jdec_put)(void);\nvoid (*jdec_fill)(void);\n'
                'void jdec_idct_c(int size, const int16_t *ws, unsigned char *out, int rowstep)\n'
                '{ (void)size; (void)ws; (void)out; (void)rowstep; }\n')
    for label, rec in (('fast', J.REC), ('base', base)):
        prog = (J.vc_build(rec=rec) if rec == J.REC else
                J.vc_build(extra=[stub], defs=('JDEC_VPU_IDCT=0',), rec=rec))
        s = S.Sim()
        C.load_into_sim(prog, s)
        for name, data in files:
            s.mem.b[J.JPG_AT:J.JPG_AT + len(data)] = data
            rc = s.call('jdec_one', J.JPG_AT, len(data), J.OUT_AT, 55, 55, J.WORK_AT,
                        stack_args=(J.WORK_MAX,), max_steps=8_000_000_000)
            rc = rc - (1 << 32) if rc & 0x80000000 else rc
            res.setdefault(name, []).append(
                rc if rc else J.fnv(bytes(s.mem.b[J.OUT_AT:J.OUT_AT + 6050])))
    bad = [n for n, (a, b) in res.items() if a != b]
    print('vc2sim: %d files at 55x55, fast against BASE: %s (%d decoded, %d failed)' % (
        len(files), 'ok' if not bad else 'DIFF ' + ', '.join(bad),
        sum(1 for a, _ in res.values() if a > 0), sum(1 for a, _ in res.values() if a <= 0)))
    return ['vc2sim ' + n for n in bad]


def main(argv):
    if not os.path.isdir(J.CORPUS):
        print('jskip_test: corpus missing: tests/jpeg/make_corpus.sh')
        return 1
    os.makedirs(FILES, exist_ok=True)
    e = optimised()
    if e:
        print('jskip_test: %s' % e)
        return 1
    files = variants()
    for name, data in files:
        open(os.path.join(FILES, name + '.jpg'), 'wb').write(data)
    base = base_rec()
    if base is None:
        print('jskip_test: %s not in git, or its search_restart not as expected' % BASE)
        return 1
    bad = []
    fast = J.mac_build(out=os.path.join(OUT, 'fast'))
    ref = J.mac_build(out=os.path.join(OUT, 'ref'), defs=('JDEC_BITSERIAL',))
    bexe = J.mac_build(out=os.path.join(OUT, 'base'), rec=base)
    if None in (fast, ref, bexe):
        return 1
    got, want, serial = outputs(fast, files), outputs(bexe, files), outputs(ref, files)
    d = differ(got, want)
    print('Mac: %d files x %d sizes, fast against BASE: %s (%d decoded, %d failed)' % (
        len(files), len(SIZES), 'ok' if not d else 'DIFF ' + ', '.join(d[:6]),
        sum(v[0] != '0' for v in want.values()), sum(v[0] == '0' for v in want.values())))
    bad += d
    ds = differ(got, serial)
    print('Mac: against the bit-serial loop: %s' % ('ok' if not ds else 'DIFF ' + ', '.join(ds[:6])))
    bad += ds
    g = mac_golden(fast)
    print('Mac: golden.txt through jdec: %s' % ('ok' if not g else 'DIFF ' + ', '.join(g[:6])))
    bad += g
    src = open(os.path.join(J.REC, 'jpeg_load.c')).read()
    for k, (old, new, what) in enumerate(MUTANTS):
        md = os.path.join(OUT, 'mut%d' % k)
        os.makedirs(md, exist_ok=True)
        if src.count(old) != 1:
            bad.append('mutant %d: %r not found once' % (k, old))
            continue
        open(os.path.join(md, 'jpeg_load.c'), 'w').write(src.replace(old, new))
        for f in ('resize.c', 'bmp.c'):
            shutil.copy(os.path.join(J.REC, f), md)
        mexe = J.mac_build(out=md, rec=md)
        if mexe is None:
            caught = 'build failed'
        else:
            mg = mac_golden(mexe)
            mc = differ(outputs(mexe, files), want)
            caught = ', '.join(x for x in ('%d golden' % len(mg) if mg else '',
                                           '%d corrupt' % len(mc) if mc else '') if x)
        print('mutant %d (%s): %s' % (k, what, 'caught (%s)' % caught if caught else 'NOT CAUGHT'))
        if not caught:
            bad.append('mutant %d not caught' % k)
        shutil.rmtree(md)
    if '--quick' not in argv:
        if not os.path.exists(J.C.GCC):
            print('skip vc2sim: vc4-elf-gcc not found')
        else:
            pick = [f for f in files if f[0].startswith('t600')]
            step = max(1, len(pick) // SIM_FILES)
            # and the optimised noise file (its short ZRL) as it is
            bad += sim(pick[::step][:SIM_FILES] +
                       [f for f in files if f[0] == 'opt_noise500_q85'], base)
    print('jskip_test: %s' % ('PASS' if not bad else 'FAIL: %d' % len(bad)))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
