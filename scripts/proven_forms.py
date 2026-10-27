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
# Every form our shipped VideoCore II images use, against the proven-forms
# table (exit criterion E2 of the hardware closure plan).
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
"""Every form our shipped VideoCore II images use, against what silicon
has proven: exit criterion E2 of docs/research/hardware-closure-plan.md
("every form the shipped images use is proven or trapped").

The table is tools/bcmre/proven_forms.txt (tools/bcmre/proven_table.py
describes it).  This script builds every shipped image (bcmfw/build.py:
the plain and merged video images of mkvideo.py, every image the device
test embeds (mkdevtest.py), the display-only image's visualizer overlay,
the hibernate stub) with listings, and enumerates statically:

  scalar  every instruction's kind (vc2sim.cover_key)
  vector  every vector instruction's operand form (vc2vpu.form_keys)
  dma     every DMA transfer-information word the code loads as a
          constant: unit bytes and 1-D/2-D
  port    the PP's writes to the BCM host port (the C and assembly under
          firmware/target/arm/ipod/video, apps/video, apps/ipodui): the
          16-bit address and 16-bit data forms, by function

and puts each use in one class:

  proven    the form's table class is silicon or apple, or (scalar) Apple's
            static code holds the kind or tools/bcmre/device_verified.txt
            names it, or (vector) Apple's self-test executes it
  guarded   class guard, used inside a region whose class is guard (a
            start-up check keeps the C path)
  exempt    inside a bench region (the form under test)
  trapped   vc2sim refuses it in its strict defaults: an unmodelled vector
            form, a vector form with no table line (vc2vpu.STRICT_FORMS),
            a dma or port form of class trap (vc2mmio's strict DMA, the
            port models)
  UNPROVEN  anything else (a guard or bench form outside its regions; a
            scalar kind neither Apple's nor verified; a dma or port form
            with no line): the gate fails

  python3 -I scripts/proven_forms.py [--reuse] [--out DIR] [-v]
  python3 -I scripts/proven_forms.py --update     add a table line for
        each vector form a shipped image uses and the table lacks, with
        the class its regions give it (review the diff: this is where a
        new form gets its evidence)

--reuse keeps listings already under DIR (default build/bcmfw/proven) when
they are newer than every bcmfw and tools/bcmre source.  Prints a
per-image table and today's list of unproven-but-trapped forms (the
inventory for the next bench sessions).  Exit 1 on any UNPROVEN use;
exit 0 with a 'skip:' line without vc4-elf-gcc.
"""
import collections
import contextlib
import fnmatch
import glob
import io
import json
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
FW = os.path.join(ROOT, 'bcmfw')
TOOLS = os.path.join(ROOT, 'tools', 'bcmre')
sys.path.insert(0, TOOLS)
sys.path.insert(0, FW)
sys.path.insert(0, os.path.join(FW, 'h264', 'test'))
import proven_table as PT   # noqa: E402

DEFAULT_OUT = os.path.join(ROOT, 'build', 'bcmfw', 'proven')
APPLE_CACHE = os.path.join(ROOT, 'build', 'bcmre', 'apple_vforms.json')
STRENGTH = {'silicon': 4, 'apple': 3, 'guard': 2, 'bench': 1, 'trap': 0}

# The PP's port writes: the 16-bit address and data registers (lcd-video.c
# BCM_WR_ADDR, BCM_DATA), the 32-bit ones, and the assembly writers
PORT_SRC = ('firmware/target/arm/ipod/video/*.c', 'firmware/target/arm/ipod/video/*.S',
            'apps/video/*.c', 'apps/ipodui/*.c')
PORT_RE = (('addr16', re.compile(r'\bBCM_(?:ALT_)?WR_ADDR\s*=[^=]')),
           ('data16', re.compile(r'\bBCM_(?:ALT_)?DATA\s*=[^=]')),
           ('addr32', re.compile(r'\bBCM_(?:ALT_)?WR_ADDR32\s*=[^=]')),
           ('data32', re.compile(r'\bBCM_(?:ALT_)?DATA32\s*=[^=]|DMA3_PER_ADDR\s*=.*BCM_DATA32')))


def images():
    """(name, defines or None for the hibernate stub) of every shipped image"""
    import mkdevtest
    import mkvideo
    out = [(n, list(d)) for n, d in mkdevtest.IMAGES]
    out += [('plain', list(mkvideo.DEFS)), ('merged', list(mkvideo.JOBS_DEFS)),
            ('hibstub', None)]
    return out


def _newest_source():
    t = 0
    for pat in ('bcmfw/**/*.[chS]', 'bcmfw/**/*.py', 'tools/bcmre/*.py',
                'apps/recorder/jpeg_load.c', 'apps/recorder/resize.c'):
        for p in glob.glob(os.path.join(ROOT, pat), recursive=True):
            t = max(t, os.path.getmtime(p))
    return t


def build_all(out, reuse):
    """{image: [listing paths]}; builds what is missing or stale"""
    import build as B
    newest = _newest_source() if reuse else None
    lists = {}
    for name, defs in images():
        d = os.path.join(out, name)
        main_lst = os.path.join(d, 'bcmhib.lst' if defs is None else 'bcmfw.lst')
        stamp = os.path.join(d, '.defs')
        fresh = reuse and os.path.exists(main_lst) and os.path.exists(stamp) and \
            os.path.getmtime(main_lst) > newest and open(stamp).read() == repr(defs)
        if not fresh:
            if defs is None:
                prog, _ = B.build_hibstub(d)
            else:
                prog, _ = B.build(d, defs, listing=True)
                if B.ovl_on(defs):
                    B.build_overlay(prog, d, defs, listing=True)
            with open(stamp, 'w') as f:
                f.write(repr(defs))
        lists[name] = [main_lst]
        ovl = os.path.join(d, (B.ovl_name(defs) if defs is not None else 'viz-ovl') +
                           '.lst')
        if defs is not None and B.ovl_on(defs) and os.path.exists(ovl):
            lists[name + '/ovl'] = [ovl]
    return lists


def _stamp(paths):
    return [[os.path.relpath(p, ROOT) if p.startswith(ROOT) else p, os.path.getsize(p),
             int(os.path.getmtime(p))] for p in paths if os.path.exists(p)]


def apple_vector_forms(corpus):
    """Operand form texts Apple's self-test executes (cached under build/)"""
    import vc2sim as S
    import vpu_forms as VF
    vlls, raws = [], []
    if os.path.isdir(corpus):
        vlls = sorted(glob.glob(os.path.join(corpus, 'Library', '*.vll')))
        raws = [os.path.join(corpus, 'Boot', 'vmcs.bin')]
    if not vlls:
        return None
    own = [os.path.join(TOOLS, n) for n in ('vc2sim.py', 'vc2vpu.py', 'vc2dis.py',
                                            'vc2sim_tests.py', 'vc2vpu_tests.py')]
    stamp = _stamp(vlls + raws + own)
    try:
        with open(APPLE_CACHE) as f:
            j = json.load(f)
        if j.get('stamp') == stamp:
            return set(j['forms'])
    except (OSError, ValueError):
        pass
    import vc2vpu as VP
    old = VP.set_strict_forms('')
    try:
        with contextlib.redirect_stdout(io.StringIO()):
            _, opd, _, bad = VF.apple_forms(corpus)
    finally:
        VP.set_strict_forms(old)
    forms = sorted(VP.form_text(k) for k in opd)
    if bad:
        print('note: Apple\'s self-test has %d failing cases under today\'s model' % bad)
    try:
        os.makedirs(os.path.dirname(APPLE_CACHE), exist_ok=True)
        with open(APPLE_CACHE, 'w') as f:
            json.dump({'stamp': stamp, 'forms': forms}, f, indent=0)
    except OSError:
        pass
    return set(forms)


def ti_kind(v):
    """A DMA transfer-information constant's kind, or None if the constant
    does not look like one (tools/bcmre/align_audit.py ti_units: bit 0
    set, the reserved-looking bits clear, and an interrupt (24),
    peripheral (28) or destination-increment (17) bit)"""
    if not v & 1 or v & 0xE0000E8E or not v & 0x11020000:
        return None
    unit = 1 << min(v >> 4 & 7, v >> 12 & 7)
    return 'unit %d %s' % (unit, '2-D' if v & 0x10000 else '1-D')


def scan_listing(path):
    """[(domain, key, symbol, addr, text)] of one listing"""
    import vc2dis as D  # noqa: F401  (vc2forms imports it)
    import vc2forms as F
    import vc2sim as S
    import vc2vpu as VP
    insns, syms = F._parse_listing(open(path).read())
    where = F._sym_index(syms)
    out = []
    regval = {}             # register -> the last 32-bit constant moved into it
    for i, ins in enumerate(insns):
        sym = where(ins.addr)
        if getattr(ins.e, 'vclass', None):
            k = VP.form_keys(ins)[1]
            out.append(('vector', VP.form_text(k), sym, ins.addr, ins.text,
                        k[0] == 'unmodelled'))
            continue
        out.append(('scalar', S.cover_key(ins), sym, ins.addr, ins.text, False))
        m = re.match(r'mov (r\d+), #(0x[0-9a-f]+)$', ins.text)
        if m:
            v = int(m.group(2), 16)
            kind = ti_kind(v)
            # a channel's config word (stored straight to 0x1000_08x0, a
            # channel's +0) looks like one too: not a transfer
            st = next((x for x in insns[i + 1:i + 4]
                       if re.match(r'st %s, \((r\d+)\)$' % m.group(1), x.text)), None)
            cs = st is not None and (regval.get(re.match(r'st r\d+, \((r\d+)\)$',
                                                         st.text).group(1), 0)
                                     & 0xFFFFFC7F) == 0x10000800
            # an address the next instructions load or store through
            # (rN as a base: a .bss variable such as aac.c's at 0x2f141)
            # is not a transfer word either
            reg = m.group(1)
            base = any(re.search(r'\(-*%s\b' % reg, x.text) and
                       not re.match(r'(st\w*|ld\w*) %s,' % reg, x.text)
                       for x in insns[i + 1:i + 3])
            if kind and not cs and not base:
                out.append(('dma', kind, sym, ins.addr, ins.text, False))
            regval[m.group(1)] = v
    return out


def scan_port():
    """[(domain, key, function, file:line, text)] of the PP's port writes"""
    out = []
    for pat in PORT_SRC:
        for p in sorted(glob.glob(os.path.join(ROOT, pat))):
            func = '?'
            rel = os.path.relpath(p, ROOT)
            for n, ln in enumerate(open(p, errors='replace'), 1):
                m = re.match(r'^(?:static\s+|inline\s+|extern\s+)*[\w\s\*]+?\b(\w+)\s*\(', ln)
                if m and not ln.startswith((' ', '\t', '#', '/', '*')) and not ln.rstrip().endswith(';'):
                    func = m.group(1)
                m = re.match(r'^([A-Za-z_]\w*):', ln)  # an assembly label
                if m and p.endswith('.S') and not m.group(1).startswith('.'):
                    func = m.group(1)
                code = ln.split('//')[0]
                if code.lstrip().startswith(('*', '/*')):
                    continue
                for kind, rx in PORT_RE:
                    if rx.search(code):
                        out.append(('port', kind, func, '%s:%d' % (rel, n), ln.strip(), False))
                if p.endswith('.S') and re.search(r'#0x30000000|0x30010000', code):
                    out.append(('port', 'data32', func, '%s:%d' % (rel, n), ln.strip(), False))
    return out


def region_of(regs, sym, domain):
    """The first region line whose pattern names sym (port regions are
    written 'port:<function pattern>')"""
    for pat, cls, ev in regs:
        if domain == 'port':
            if pat.startswith('port:') and fnmatch.fnmatchcase(sym, pat[5:]):
                return cls, ev
        elif not pat.startswith('port:') and fnmatch.fnmatchcase(sym, pat):
            return cls, ev
    return None, None


def classify(use, table, regs, apple_scalar, verified, apple_vec):
    """(class, why) of one form use"""
    dom, key, sym, _, _, unmodelled = use
    rcls, rev = region_of(regs, sym, dom)
    if rcls == 'bench':
        return 'exempt', rev
    cls, ev = table.get((dom, key), (None, None))
    if dom == 'scalar':
        if cls in ('silicon', 'apple'):
            return 'proven', ev
        if key in verified:
            return 'proven', 'device_verified.txt'
        if apple_scalar is not None and key in apple_scalar:
            return 'proven', "Apple's static code"
        if apple_scalar is None:
            return 'proven', 'no corpus: scalar kinds not checked'
        return 'UNPROVEN', 'kind neither Apple\'s nor device-verified'
    if dom == 'vector':
        if unmodelled:
            return 'trapped', 'vc2sim has no model (VectorOp)'
        if cls in ('silicon', 'apple'):
            return 'proven', ev
        if apple_vec is not None and key in apple_vec:
            return 'proven', "Apple's self-test executes it"
        if cls == 'guard' and rcls == 'guard':
            return 'guarded', rev
        if cls is None:
            return 'trapped', 'no table line: vc2sim strict forms traps it'
        return 'UNPROVEN', 'class %s outside its regions (%s)' % (cls, rcls or 'no region')
    # dma, port
    if cls in ('silicon', 'apple'):
        return 'proven', ev
    if cls == 'guard' and rcls == 'guard':
        return 'guarded', rev
    if cls == 'trap':
        return 'trapped', ev
    return 'UNPROVEN', 'no table line' if cls is None else \
        'class %s outside its regions (%s)' % (cls, rcls or 'no region')


def update_table(uses, table, regs, apple_vec, path=PT.TABLE):
    """Append a line for each vector form with no table line: the
    strongest class its sites' regions give it (silicon > guard > bench),
    apple when Apple's self-test executes it"""
    best = {}
    for u in uses:
        if u[0] != 'vector' or u[5] or ('vector', u[1]) in table:
            continue
        if apple_vec is not None and u[1] in apple_vec:
            cand = ('apple', "Apple's self-test executes it")
        else:
            rcls, rev = region_of(regs, u[2], 'vector')
            if rcls is None:
                continue
            cand = (rcls, '%s (%s)' % (rev, u[2]))
        if u[1] not in best or STRENGTH[cand[0]] > STRENGTH[best[u[1]][0]]:
            best[u[1]] = cand
    if not best:
        print('table: nothing to add')
        return 0
    with open(path, 'a') as f:
        f.write('\n# added by scripts/proven_forms.py --update (%s)\n' %
                time.strftime('%Y-%m-%d'))
        for k in sorted(best):
            f.write('vector | %s | %s | %s\n' % (k, best[k][0], best[k][1]))
    print('table: %d vector line(s) added; review the diff' % len(best))
    return 0


def main(argv):
    verbose = '-v' in argv
    reuse = '--reuse' in argv
    out = DEFAULT_OUT
    if '--out' in argv:
        out = argv[argv.index('--out') + 1]
    import build as B
    if not B.have_gcc():
        print('skip: vc4-elf-gcc not found (set VC4_GCC)')
        return 0
    t0 = time.time()
    import vc2forms as F
    table = PT.load()
    regs = PT.regions(table)
    apple_scalar = F.apple_kinds() or None
    corpus = os.environ.get('BCM_CORPUS', os.path.expanduser(
        '~/Git/re/bcm2722/extract/1.3/vc/VideoCore'))
    apple_vec = apple_vector_forms(corpus)
    if apple_scalar is None or apple_vec is None:
        print("note: no Apple corpus: Apple's kinds and self-test forms not used")
    lists = build_all(out, reuse)
    uses = {}
    for name, paths in lists.items():
        uses[name] = [u for p in paths for u in scan_listing(p)]
    uses['PP port'] = scan_port()
    if '--update' in argv:
        return update_table([u for us in uses.values() for u in us], table, regs, apple_vec)
    verified = F.DEVICE_VERIFIED
    classes = ('proven', 'guarded', 'exempt', 'trapped', 'UNPROVEN')
    print('%-18s %9s %9s %9s %9s %9s   (form kinds;sites)' % (('image',) + classes))
    bad = []
    trapped = collections.OrderedDict()
    guarded = collections.OrderedDict()
    for name, us in uses.items():
        kinds = {c: set() for c in classes}
        sites = collections.Counter()
        for u in us:
            c, why = classify(u, table, regs, apple_scalar, verified, apple_vec)
            kinds[c].add((u[0], u[1]))
            sites[c] += 1
            if c == 'UNPROVEN':
                bad.append((name, u, why))
            elif c == 'trapped':
                trapped.setdefault((u[0], u[1]), [why, set()])[1].add('%s:%s' % (name, u[2]))
            elif c == 'guarded':
                guarded.setdefault((u[0], u[1]), [why, set()])[1].add('%s:%s' % (name, u[2]))
            if verbose:
                print('    %-8s %-6s %-60s %s  %s' % (c, u[0], u[1][:60], u[2], why))
        print('%-18s %s' % (name[:18], ' '.join('%4d;%-4d' % (len(kinds[c]), sites[c])
                                                for c in classes)))
    print('\nunproven but trapped (vc2sim refuses them; bench sessions settle them):')
    for (dom, key), (why, where) in trapped.items():
        print('  %-6s %-58s %s; in %s' % (dom, key[:58], why, ', '.join(sorted(where)[:3])
                                           + (' ...' if len(where) > 3 else '')))
    # the model's own traps: what strict alignment refuses (memory-access.md 11)
    import vc2sim as S
    import vc2vpu as VP
    print('  align  scalar misaligned access in any form but %d proven ones '
          '(vc2sim.STRICT_SCALAR_KNOWN)' % len(S.STRICT_SCALAR_KNOWN))
    print('  align  plain vector load/store at an offset outside %s' % '; '.join(
        '%s %s' % (VP.VMEM_NAMES.get(k, k), sorted(v)) for k, v in
        sorted(VP.VMEM_KNOWN_OFFSETS.items())))
    print('  align  vector store REP at a stride off its size; vldp/vldq REP whose '
          'line phase moves by other than 4 bytes a row (H8)')
    for (d, k), (c, e) in table.items():
        if d in ('dma', 'port') and c == 'trap' and (d, k) not in trapped:
            print('  %-6s %-58s %s (not used today)' % (d, k[:58], e))
    print('\nunproven but guarded (a start-up check keeps the C if silicon differs):')
    for (dom, key), (why, where) in guarded.items():
        print('  %-6s %-58s %s' % (dom, key[:58], why))
    for name, u, why in bad:
        print('UNPROVEN %s: %s %s at %s (%s): %s' % (name, u[0], u[1], u[2],
                                                     u[4] if u[0] != 'port' else u[3], why))
    print('%d unproven, untrapped form use(s) (%.0f s)' % (len(bad), time.time() - t0))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
