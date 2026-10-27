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
# The VC alignment audit: our images' workloads under vc2sim's strict
# alignment log, and a static pass over each image's listing.
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
"""The VC alignment audit (docs/research/bcm2722/alignment-audit.md).

  align_audit.py run OUT [TEST...]   run bcmfw/tests/*_test.py (or the
        named scripts, SCRIPT:ARG:ARG... for arguments) with
        VC2SIM_STRICT_ALIGN=log, six at a time; each
        unknown-alignment site lands in OUT/report.jsonl, each test's
        output in OUT/<test>.log, exit codes in OUT/rc.txt
  align_audit.py static OUT          build every image (mkdevtest.py's and
        mkvideo.py's sets) with listings into OUT/img/<name>, and list the
        sites a listing shows: vector memory ops through a register base
        (any alignment the register carries), vldp/vldq/vstr, stride
        registers, scalar copy loops (a load and a store through
        post-incremented or indexed bases in one short loop), and DMA
        transfer-information words with units under 4 bytes
  align_audit.py report OUT          the dynamic sites, by our function
        (mapped through the listings), and the static tables, as Markdown

Strict mode (tools/bcmre/vc2sim.py STRICT_ALIGN) names what silicon has
not shown: see memory-access.md "Alignment" for the rules it encodes."""
import collections
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
FW = os.path.join(ROOT, 'bcmfw')


def images():
    """(name, defines) for every image we ship or bench"""
    sys.path.insert(0, FW)
    import mkdevtest
    import mkvideo
    out = list(mkdevtest.IMAGES)
    out += [('video', mkvideo.DEFS), ('merged', mkvideo.JOBS_DEFS),
            ('merged_staged', mkvideo.STAGED_DEFS)]
    return out


# ---- run -----------------------------------------------------------------
def run(out, tests):
    os.makedirs(out, exist_ok=True)
    rep = os.path.join(out, 'report.jsonl')
    if os.path.exists(rep):
        os.remove(rep)
    tests = tests or sorted(os.path.join(FW, 'tests', f)
                            for f in os.listdir(os.path.join(FW, 'tests'))
                            if f.endswith('_test.py'))
    env = dict(os.environ, VC2SIM_STRICT_ALIGN='log', VC2SIM_ALIGN_REPORT=rep)
    procs, rcs = [], []
    pending = list(tests)
    while pending or procs:
        while pending and len(procs) < 6:
            t = pending.pop(0).split(':')       # script[:arg:arg...]
            n = os.path.basename(t[0])[:-3]
            f = open(os.path.join(out, n + '.log'), 'w')
            procs.append((n, time.time(), f, subprocess.Popen(
                ['nice', '-n', '10', sys.executable] + t, stdout=f,
                stderr=subprocess.STDOUT, env=env, cwd=ROOT)))
        for p in list(procs):
            if p[3].poll() is not None:
                procs.remove(p)
                p[2].close()
                rcs.append('%s %d %d' % (p[0], p[3].returncode, time.time() - p[1]))
                print(rcs[-1], flush=True)
        time.sleep(1)
    with open(os.path.join(out, 'rc.txt'), 'w') as f:
        f.write('\n'.join(rcs) + '\n')
    return 0 if all(r.split()[1] == '0' for r in rcs) else 1


# ---- static --------------------------------------------------------------
LINE = re.compile(r'^  ([0-9a-f]{8})  ((?:[0-9a-f]{4} ?)+)\s+(\S.*)$')
FUNC = re.compile(r'^([0-9a-f]{8}) <([^>]+)>( \(label\))?$')
VMEM = re.compile(r'^(vld8v?|vld16|vst8v?|vst16|vldp\w*|vldq\w*|vstr\w*)\s+(\S+),\s*'
                  r'\((r\d|0x[0-9a-f]+|-?\d+)?([+-][^)]*)?\)(\+=r\d)?')


def parse_listing(path):
    """[(addr, func, text)] of a vc2cc listing"""
    out, func = [], '?'
    for ln in open(path):
        m = FUNC.match(ln.rstrip('\n'))
        if m:
            if not m.group(3):
                func = m.group(2)
            continue
        m = LINE.match(ln.rstrip('\n'))
        if m and not m.group(3).startswith('.hw'):
            out.append((int(m.group(1), 16), func, m.group(3).strip()))
    return out


def build_all(out):
    sys.path.insert(0, FW)
    import build as B
    built = []
    for name, defs in images():
        d = os.path.join(out, 'img', name)
        lst = os.path.join(d, 'bcmfw.lst')
        if not os.path.exists(lst):
            B.build(d, list(defs), listing=True)
        built.append((name, lst))
    return built


def ti_units(v):
    """A DMA transfer-information word's unit in bytes, or None if the
    constant does not look like one: bit 0 set, the reserved-looking
    bits clear, and an interrupt (24), peripheral (28) or increment (17)
    bit as every known word has (dma.md)"""
    if not v & 1 or v & 0xE0000E8E or not v & 0x11020000:
        return None
    return 1 << min(v >> 4 & 7, v >> 12 & 7)


def static(out):
    rows = []
    for name, lst in build_all(out):
        ins = parse_listing(lst)
        vsites = collections.OrderedDict()
        for i, (a, f, t) in enumerate(ins):
            m = VMEM.match(t)
            if m:
                op, base, off, stride = m.group(1), m.group(3), m.group(4) or '', m.group(5)
                kind = 'pair' if op.startswith(('vldp', 'vldq')) else \
                    'vstr' if op.startswith('vstr') else 'plain'
                key = (f, op, kind)
                e = vsites.setdefault(key, [0, set(), set()])
                e[0] += 1
                e[1].add(off or '+0')
                if stride:
                    e[2].add(stride[2:])
            mv = re.match(r'mov r\d+, #(0x[0-9a-f]+)$', t)
            if mv:
                v = int(mv.group(1), 16)
                u = ti_units(v)
                if u is not None and u < 4:
                    rows.append((name, f, 'DMA ti %08x: %d-byte units' % (v, u), 1))
        for (f, op, kind), (n, offs, strides) in vsites.items():
            rows.append((name, f, '%s %s x%d, offsets %s%s' % (
                kind, op, n, ' '.join(sorted(offs)),
                ', stride r%s' % '/r'.join(sorted(strides)) if strides else ''), n))
        # copy loops: a backward branch over <= 12 instructions holding a
        # load and a store through (rs)++ or (--rs) of the same width
        for i, (a, f, t) in enumerate(ins):
            m = re.match(r'(b\w*|addcmpb\w*) .*?(0x[0-9a-f]+)$', t)
            if not m:
                continue
            tgt = int(m.group(2), 16)
            if not a - 40 <= tgt < a:
                continue
            body = [x for x in ins[max(0, i - 20):i + 1] if tgt <= x[0] <= a]
            lds = [x[2] for x in body if re.match(r'ld[bhs]?h? r\d+, \(r\d+\)\+\+|'
                                                  r'ld[bh]? r\d+, \(--r\d+\)', x[2])]
            sts = [x[2] for x in body if re.match(r'st[bh]? r\d+, \(r\d+\)\+\+|'
                                                  r'st[bh]? r\d+, \(--r\d+\)', x[2])]
            if lds and sts:
                w = sorted(set(x.split()[0] for x in lds + sts))
                rows.append((name, f, 'copy loop at %08x: %s' % (tgt, ' '.join(w)), 1))
    seen = collections.OrderedDict()
    for name, f, what, n in rows:
        seen.setdefault((f, what), []).append(name)
    with open(os.path.join(out, 'static.json'), 'w') as fo:
        json.dump([dict(func=f, what=w, images=imgs) for (f, w), imgs in seen.items()],
                  fo, indent=0)
    print('%d static sites over %d images' % (len(seen), len(images())))
    return 0


# ---- report --------------------------------------------------------------
def report(out):
    lst = {}
    imgdir = os.path.join(out, 'img')
    if os.path.isdir(imgdir):
        for name in sorted(os.listdir(imgdir)):
            p = os.path.join(imgdir, name, 'bcmfw.lst')
            if os.path.exists(p):
                lst[name] = {a: (f, t) for a, f, t in parse_listing(p)}
    sites = collections.OrderedDict()
    rep = os.path.join(out, 'report.jsonl')
    for ln in open(rep) if os.path.exists(rep) else ():
        d = json.loads(ln)
        if d.get('origin') == 'apple':
            continue
        funcs = sorted(set(m[d['pc']][0] for m in lst.values()
                           if d['pc'] in m and m[d['pc']][1].split()[0] == d['insn'].split()[0]))
        k = (', '.join(funcs) or '(test kernel, not in an image listing)', d['what'],
             d['insn'])
        e = sites.setdefault(k, [0, set(), d['addr']])
        e[0] += d['count']
        e[1].add(d['test'].replace('.py', ''))
    print('## Dynamic sites (strict log, our code)\n')
    print('| Function | Access | Instruction | Count | Tests | First address |')
    print('|---|---|---|---|---|---|')
    for (f, what, insn), (n, tests, a) in sites.items():
        print('| %s | %s | `%s` | %d | %s | `0x%x` |' % (f, what, insn, n,
                                                      ', '.join(sorted(tests)), a))
    sj = os.path.join(out, 'static.json')
    if os.path.exists(sj):
        print('\n## Static sites (listings)\n')
        print('| Function | Site | Images |')
        print('|---|---|---|')
        for d in json.load(open(sj)):
            print('| %s | %s | %s |' % (d['func'], d['what'], ', '.join(d['images'])))
    return 0


def main(argv):
    if len(argv) < 2 or argv[0] not in ('run', 'static', 'report'):
        print(__doc__)
        return 2
    if argv[0] == 'run':
        return run(argv[1], argv[2:])
    if argv[0] == 'static':
        return static(argv[1])
    return report(argv[1])


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
