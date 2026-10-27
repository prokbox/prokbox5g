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

"""jdec's truncation scan (bcmfw/jobs/jscan.h, jpeg-spike.md section 11).

1. The fast scan against its byte-wise definition (bcmfw/jpeg/jscantest.c):
   a mixed buffer (bytes near the rule's: FF, DA, D9, 00, FE, 7F, 80, 01,
   long runs with no FF, and the corpus's EXIF file) fed in pseudo-random
   chunks at every alignment, with skips; on the Mac (many seeds) and in
   vc2sim (the vc4 code the image runs).
2. The rule's edges, end to end through jdec on the Mac and in vc2sim
   (jdec_one), on files made here from the corpus (build/jpeg/edges):
   an EOI whose FF ends one of the decoder's 64 KiB reads and whose D9
   starts the next; the same file cut after that FF; data after the EOI,
   with and without an SOS in it, inside the last read and past it.  The
   statuses are the old byte-wise scan's (the rule is what it did).
3. Mutations of jscan.h: each must fail 1 or 2 on the Mac, so the tests
   above can see a broken scan.

Usage: bcmfw/tests/jscan_test.py [--quick]   (--quick: no vc2sim)
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

JOBS = os.path.join(FW, 'jobs')
HARNESS = os.path.join(FW, 'jpeg', 'jscantest.c')
OUT = os.path.join(ROOT, 'build', 'jscan')
EDGES = os.path.join(ROOT, 'build', 'jpeg', 'edges')
READ = 64 * 1024        # jpeg_common.h JPEG_READ_BUF_SIZE: the decoder's reads
MAC_SEEDS = 3000
SIM_SEEDS = 6

# (file in jscan.h or jdec.c, text, replacement, what it breaks)
MUTANTS = [
    ('jscan.h', 'm = 0x80808080u', 'm = 0x00808080u', 'a word lane never tested'),
    ('jscan.h', 'if (!ff)\n', 'if (1)\n', 'the byte after an FF skipped'),
    ('jscan.h', 'int ff = s->ff;', 'int ff = 0;', 'an FF ending one read, its pair in the next'),
    ('jscan.h', '(w += 4) <= last', '(w += 4) <= last + 1', 'the last 16 bytes read as a group'),
    ('jscan.h', 'ff = b == 0xFF;\n    }\n#undef', 'ff = b == 0xFF || (ff && !b);\n    }\n#undef',
     'stuffed FF 00 taken as a pending FF'),
    ('jscan.h', 's->after_sos = 1;\n            s->eoi = 0;\n        }\n        else if (ff',
     's->after_sos = 1;\n        }\n        else if (ff', 'an SOS keeps an earlier EOI'),
]


def mix():
    """bytes near the rule's, runs with no FF, and a real file's markers"""
    rnd = random.Random(125)
    spec = (0xFF, 0xDA, 0xD9, 0x00, 0xFE, 0x7F, 0x80, 0x01)
    b = bytearray()
    while len(b) < 160000:
        k = rnd.randrange(3)
        if k == 0:
            b += bytes(rnd.choice(spec) if rnd.random() < .4 else rnd.randrange(256)
                       for _ in range(rnd.randrange(1, 64)))
        elif k == 1:
            b += bytes(rnd.randrange(255) for _ in range(rnd.randrange(16, 400)))
        else:
            b += bytes(rnd.choice(spec) for _ in range(rnd.randrange(1, 8)))
    b += open(os.path.join(J.CORPUS, 't600_exif.jpg'), 'rb').read()
    return bytes(b)


def with_com(d, pad):
    """d with a COM segment of pad bytes (4 or more) after its SOI"""
    assert 4 <= pad <= 65537
    return d[:2] + b'\xff\xfe' + (pad - 2).to_bytes(2, 'big') + bytes(pad - 4) + d[2:]


def edges():
    """{name: (bytes, expected)}: expected 'TRUNCATED', or a corpus file whose
    pixels the edge must decode to"""
    big = open(os.path.join(J.CORPUS, 't1500_420_q85.jpg'), 'rb').read()
    small = open(os.path.join(J.CORPUS, 't600_420_q85.jpg'), 'rb').read()
    assert big[-2:] == b'\xff\xd9' and small[-2:] == b'\xff\xd9'
    # the EOI's FF at the last byte of the third 64 KiB read (the reads run
    # from 0 in whole 64 KiB while the decoder seeks nowhere: the COM sits
    # inside the first read, so skipping it moves the buffer, not the file)
    split = 3 * READ - 1
    e = {}
    for name, at in (('split', split), ('split_m1', split - 1), ('split_p1', split + 1)):
        e['t1500_eoi_' + name] = (with_com(big, at + 2 - len(big)), 't1500_420_q85.jpg')
    e['t1500_eoi_split_cut'] = (e['t1500_eoi_split'][0][:split + 1], 'TRUNCATED')
    e['t600_trail'] = (small + bytes(range(1, 255)) * 2, 't600_420_q85.jpg')
    # an SOS in the junk inside the decoder's last read counts as the last
    # SOS (the old rule); one past it is never read (ended() stops at the EOI)
    e['t600_trail_sos'] = (small + b'\x00\xff\xda\x00\x08junk', 'TRUNCATED')
    e['t600_trail_sos_far'] = (small + bytes(READ) + b'\xff\xda\x00\x08junk', 't600_420_q85.jpg')
    return e


def mac_unit(inc, buf):
    exe = os.path.join(OUT, 'jscantest')
    r = subprocess.run(['cc', '-O2', '-std=gnu11', '-I' + inc, HARNESS, '-o', exe],
                       capture_output=True, text=True)
    if r.returncode:
        return 'build: ' + r.stderr
    path = os.path.join(OUT, 'mix.bin')
    open(path, 'wb').write(buf)
    got = subprocess.run([exe, path, str(MAC_SEEDS)], capture_output=True, text=True).stdout.strip()
    return None if got == '0' else '%s chunks differ' % got


def want_of(exp, size='55'):
    if exp == 'TRUNCATED':
        return ['0', '0', str(J.JDEC_E['TRUNCATED']), '0']
    for n, w, h, want in J.read_table(J.GOLDEN):
        if n == exp and w == size:
            return want
    raise KeyError(exp)


def mac_edges(exe, e):
    bad = []
    for name, (data, exp) in sorted(e.items()):
        got = subprocess.run([exe, os.path.join(EDGES, name + '.jpg'), '55', '55'],
                             capture_output=True, text=True).stdout.split()
        if got != want_of(exp):
            bad.append('%s: got %s want %s' % (name, ' '.join(got), ' '.join(want_of(exp))))
    return bad


def sim(buf, e):
    import vc2cc as C
    import vc2sim as S
    bad = []
    prog = C.build([HARNESS], '-O2', extra=['-std=gnu11', '-I' + JOBS], libs=C.DEFAULT_LIBS,
                   no_float=True, conservative=True)
    s = S.Sim()
    C.load_into_sim(prog, s)
    s.mem.b[J.JPG_AT:J.JPG_AT + len(buf)] = buf
    for seed in range(1, SIM_SEEDS + 1):
        rc = s.call('jscan_check', J.JPG_AT, len(buf), seed, max_steps=10 ** 9)
        if rc:
            bad.append('vc2sim seed %d: %d chunks differ' % (seed, rc))
    print('vc2sim: the fast scan against the definition, %d seeds over %d bytes: %s' % (
        SIM_SEEDS, len(buf), 'ok' if not bad else 'DIFF'))
    prog = J.vc_build()
    s = S.Sim()
    C.load_into_sim(prog, s)
    for name, (data, exp) in sorted(e.items()):
        s.mem.b[J.JPG_AT:J.JPG_AT + len(data)] = data
        rc = s.call('jdec_one', J.JPG_AT, len(data), J.OUT_AT, 55, 55, J.WORK_AT,
                    stack_args=(J.WORK_MAX,), max_steps=8_000_000_000)
        rc = rc - (1 << 32) if rc & 0x80000000 else rc
        got = ['0', '0', str(rc), '0'] if rc else \
            ['55', '55', '6050', '%08x' % J.fnv(bytes(s.mem.b[J.OUT_AT:J.OUT_AT + 6050]))]
        ok = got == want_of(exp)
        bad += [] if ok else ['vc2sim %s: got %s want %s' % (name, got, want_of(exp))]
        print('vc2sim: %-24s %s' % (name, 'ok' if ok else 'DIFF'))
    return bad


def main(argv):
    if not os.path.isdir(J.CORPUS):
        print('jscan_test: corpus missing: tests/jpeg/make_corpus.sh')
        return 1
    os.makedirs(OUT, exist_ok=True)
    os.makedirs(EDGES, exist_ok=True)
    buf = mix()
    e = edges()
    for name, (data, _) in e.items():
        open(os.path.join(EDGES, name + '.jpg'), 'wb').write(data)
    bad = []
    r = mac_unit(JOBS, buf)
    print('Mac: the fast scan against the definition, %d seeds over %d bytes: %s' % (
        MAC_SEEDS, len(buf), r or 'ok'))
    bad += [r] if r else []
    exe = J.mac_build(out=os.path.join(OUT, 'real'))
    if exe is None:
        return 1
    eb = mac_edges(exe, e)
    print('Mac: %d edge files through jdec: %s' % (len(e), '; '.join(eb) or 'ok'))
    bad += eb
    # the mutants: each must fail the unit check or an edge
    for k, (f, old, new, what) in enumerate(MUTANTS):
        d = os.path.join(OUT, 'mut%d' % k)
        os.makedirs(d, exist_ok=True)
        for g in ('jscan.h', 'jdec.c'):
            src = open(os.path.join(JOBS, g)).read()
            if g == f:
                if src.count(old) != 1:
                    bad.append('mutant %d: %r not found once in %s' % (k, old, f))
                src = src.replace(old, new)
            open(os.path.join(d, g), 'w').write(src)
        u = mac_unit(d, buf) if f == 'jscan.h' else None
        mexe = J.mac_build(jdec_c=os.path.join(d, 'jdec.c'), out=d)
        me = mac_edges(mexe, e) if mexe else ['build failed']
        caught = bool(u) or bool(me)
        print('mutant %d (%s): %s' % (k, what, 'caught (%s; edges: %s)' % (
            'unit' if u else 'not by the unit', ', '.join(x.split(':')[0] for x in me) or 'none')
            if caught else 'NOT CAUGHT'))
        if not caught:
            bad.append('mutant %d not caught' % k)
        shutil.rmtree(d)
    if '--quick' not in argv:
        if not os.path.exists(J.C.GCC):
            print('skip vc2sim: vc4-elf-gcc not found')
        else:
            bad += sim(buf, e)
    print('jscan_test: %s' % ('PASS' if not bad else 'FAIL: %d' % len(bad)))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
