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

"""The shell's JPEG entropy decoder (apps/recorder/jpeg_load.c) on
corrupt files with restart markers, under AddressSanitizer and
UndefinedBehaviorSanitizer on the Mac (jpeg-spike.md sections 12 and 15;
changes-from-rockbox.md, jpeg_load.c's bounded restart).

Upstream, search_restart() set the bit count to marker_ind with no bound:
a restart marker met early makes it 8 + 16 a fill, past the bit buffer's
width.  The decoder then shifts by more than the word (undefined in C);
peek_bits and get_bits see different bits, the slow Huffman walk starts
below the length's first code, and pub[] is read with a negative offset,
about 1 KB before the decoder's state.  The count is now kept only when
the buffer holds it (else none, and jdec counts a bad code: CORRUPT).

1. jskip_test.py's files (the corpus sources, corrupted: bit flips,
   noise, a stuffed zero dropped, restart markers put early, cuts), each
   at 55, 142 and the art set once, through the shell's own bit-serial
   loop (JDEC_BITSERIAL) and jdec's fast one, both built with
   -fsanitize=address,undefined and no memory guard: no sanitizer report,
   every status a decode, CORRUPT or TRUNCATED, and the two loops the same, status
   and pixels, on every file.
2. golden.txt's baseline files through both sanitized builds: unchanged.
3. The bound removed (a mutant): the sanitizer report comes back.

Needs the corpus (tests/jpeg/make_corpus.sh) and a clang whose ASan runs
here (Homebrew llvm's; Apple clang 17's hangs at start-up on this macOS).
Usage: bcmfw/tests/jpeg_oob_test.py"""
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, 'tools', 'bcmre'))
import jpeg_test as J   # noqa: E402
import jskip_test as K  # noqa: E402

OUT = os.path.join(ROOT, 'build', 'jpeg_oob')
CLANGS = ('/opt/homebrew/opt/llvm/bin/clang', 'clang', 'cc')
# shift-base: Rockbox's -1 << s (HUFF_EXTEND), defined by gcc and clang
SAN = ('-fsanitize=address,undefined', '-fno-sanitize=shift-base',
       '-fno-sanitize-recover=all', '-g', '-fno-omit-frame-pointer')
ENV = dict(os.environ, ASAN_OPTIONS='detect_leaks=0:abort_on_error=0',
           UBSAN_OPTIONS='print_stacktrace=0')
BOUND = 'if (p_jpeg->marker_ind <= p_jpeg->bitbuf_bits)'
# a corrupt file decodes (to whatever its bits say), or fails as corrupt
# or cut short
FAILS = (str(J.JDEC_E['CORRUPT']), str(J.JDEC_E['TRUNCATED']))


def clang():
    """the first compiler whose ASan starts here, or None"""
    os.makedirs(OUT, exist_ok=True)
    src, exe = os.path.join(OUT, 'probe.c'), os.path.join(OUT, 'probe')
    open(src, 'w').write('int main(void) { return 0; }\n')
    for cc in CLANGS:
        if subprocess.run([cc, '-fsanitize=address', src, '-o', exe],
                          capture_output=True).returncode:
            continue
        try:
            if subprocess.run([exe], env=ENV, timeout=10).returncode == 0:
                return cc
        except subprocess.TimeoutExpired:
            pass
    return None


def run(exe, path, size):
    """(output words, the sanitizer's first report or '')"""
    r = subprocess.run([exe, path] + size, capture_output=True, text=True, env=ENV,
                       timeout=120)
    rep = [ln for ln in r.stderr.splitlines() if 'runtime error' in ln or 'ERROR:' in ln]
    return r.stdout.split(), rep[0].replace(ROOT + '/', '') if rep else ''


def check(exe, files):
    """{(name, size): output}, [reports]"""
    out, reps = {}, []
    for name, _ in files:
        for z in K.SIZES:
            got, rep = run(exe, os.path.join(K.FILES, name + '.jpg'), z)
            out[(name, tuple(z))] = got
            if rep:
                reps.append('%s %s: %s' % (name, 'x'.join(z) if len(z) == 2 else 'art set', rep))
    return out, reps


def main():
    if not os.path.isdir(J.CORPUS):
        print('jpeg_oob_test: corpus missing: tests/jpeg/make_corpus.sh')
        return 1
    cc = clang()
    if cc is None:
        print('jpeg_oob_test: no compiler whose AddressSanitizer runs here')
        return 1
    os.makedirs(K.FILES, exist_ok=True)
    e = K.optimised()
    if e:
        print('jpeg_oob_test: %s' % e)
        return 1
    files = K.variants()
    for name, data in files:
        open(os.path.join(K.FILES, name + '.jpg'), 'wb').write(data)
    bad = []
    res = {}
    for label, defs in (('bit-serial', ('JDEC_BITSERIAL',)), ('fast', ())):
        exe = J.mac_build(out=os.path.join(OUT, label), defs=defs, cflags=SAN, cc=cc)
        if exe is None:
            return 1
        res[label], reps = check(exe, files)
        odd = ['%s %s' % (n, ' '.join(v)) for (n, _), v in res[label].items()
               if v[0] == '0' and v[2] not in FAILS]
        g = []
        for name, w, h, want in J.with_twins(J.read_table(J.GOLDEN)):
            path = os.path.join(J.CORPUS, name)
            # baseline only: jprog.h's DC (jdec's own) extends a zero
            # difference with a shift by -1, a separate report
            if os.path.exists(path) and name not in J.PROG_TWINS:
                got, rep = run(exe, path, [w, h])
                if got != J.expect(name, want, J.JDEC_E) or rep:
                    g.append('%s %sx%s%s' % (name, w, h, ' ' + rep if rep else ''))
        n_bad = sum(v[0] == '0' for v in res[label].values())
        print('%s, sanitized (%s): %d files x %d sizes, %d decoded, %d failed; '
              'reports: %s; other statuses: %s; golden.txt: %s' % (
                  label, os.path.basename(cc), len(files), len(K.SIZES),
                  len(res[label]) - n_bad, n_bad, len(reps) or 'none',
                  len(odd) or 'none', 'ok' if not g else 'DIFF %d' % len(g)))
        for x in (reps + odd + g)[:6]:
            print('  ' + x)
        bad += reps + odd + g
    d = K.differ(res['fast'], res['bit-serial'])
    print('fast against bit-serial: %s' % ('the same on every file' if not d else
                                          'DIFF ' + ', '.join(d[:6])))
    bad += d
    # the mutant: the bound removed, on the files with restart markers
    md = os.path.join(OUT, 'mut')
    os.makedirs(md, exist_ok=True)
    src = open(os.path.join(J.REC, 'jpeg_load.c')).read()
    if src.count(BOUND) != 1:
        bad.append('mutant: %r not found once' % BOUND)
    else:
        open(os.path.join(md, 'jpeg_load.c'), 'w').write(src.replace(BOUND, 'if (1)'))
        for f in ('resize.c', 'bmp.c'):
            shutil.copy(os.path.join(J.REC, f), md)
        rst = [f for f in files if 'rst' in f[0]]
        for label, defs in (('bit-serial', ('JDEC_BITSERIAL',)), ('fast', ())):
            mexe = J.mac_build(out=os.path.join(md, label), rec=md, defs=defs, cflags=SAN, cc=cc)
            reps = check(mexe, rst)[1] if mexe else ['build failed']
            print('mutant (the bound removed), %s: %s' % (
                label, 'caught: %d reports, e.g. %s' % (len(reps), reps[0]) if reps
                else 'NOT CAUGHT'))
            if not reps:
                bad.append('mutant not caught (%s)' % label)
        shutil.rmtree(md)
    print('jpeg_oob_test: %s' % ('PASS' if not bad else 'FAIL: %d' % len(bad)))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
