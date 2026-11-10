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

"""What the COP can reach, transitively (p19-copfileio; design 2.3, 4).

usage: scripts/febe_reach_check.py [--strict] [--strict-lines] [--list]
                                   [--root FN]... BUILD_DIR

febe_link_check.py looks one call deep: the shell's own objects.  The COP
runs everything those call, too: apps/ code (misc.c's event handler, the
USB screen, settings_apply), firmware text drawing (bidi.c, the scroll
engine), and anything reached through a function pointer or a table.  Bench
4f (2026-11-05, the first UI-on-COP boot): bidi.c's bidi_buf shared a 16-byte
cache line with disk_cache.c's cache_lru; the COP's text drawing dirtied the
line and its write-back put a stale LRU head over the CPU's, and 11 s later
the tagcache thread panicked "file_cache_alloc - OOM".

This reads BUILD_DIR/rockbox.elf (disassembly, symbol table) and
rockbox.map (which object owns each address) and walks the call graph from
the ui thread's root (shell_main): direct branches, function addresses in
literal pools (callbacks), and function addresses inside the data tables a
reached function names (menus, ops tables).  A __wrap_X remote-call stub
ends a path (on the COP it crosses to the CPU), and so does each name in
CPU_SIDE (reviewed: never run on the COP).  It reports:

 1. "fs": a function of the file layer or the storage driver reached
    (FS_OBJS); the COP must cross by remote call (febe_rpc.list), never
    enter the file system itself;
 2. "line": a cached, writable static that reached code may write (stored
    through a literal address, or the address passed to a call) outside
    the front end's region (app-pp.lds: apps/ipodui and FE_REGION_OBJS,
    on cache lines of their own), unless WRITE_OK lists it with a reason.

The analysis over-approximates (every literal function address is a call,
every address passed on is written): a finding is a path to review, and
the fix is to route the call, move the object into the front end's region
or list it here with the reason it is safe.  Warn mode prints and exits 0;
--strict exits 1 on any "fs" finding (the gate, through febe_link_check.py
--strict), --strict-lines on any "line" finding too (not yet the gate: the
first walk found ~200, most of them settings and DSP callbacks reached
through tables, to be routed or reviewed).  The line hazard the bench hit
is closed structurally meanwhile: the file layer's data sits on lines of
its own (app-pp.lds), which xcore_line_check.py --strict holds.  --list
prints every finding's path.
"""
import bisect
import collections
import os
import re
import struct
import subprocess
import sys

ROOTS = ['shell_main']

# reviewed: reached by the walk only through a path the COP never takes
CPU_SIDE = {
    # be_post()/be_call() serve inline only when not split or off the ui
    # thread (fe_be.c serve_inline()); from the split ui thread they queue
    'be_dispatch': 'fe_be.c serve_inline()',
}

# the file layer and the storage driver: CPU only (the open-file table,
# the FAT and disk caches, the ATA state are cached and CPU-written)
FS_OBJS = (
    'file.o', 'dir.o', 'file_internal.o', 'disk_cache.o', 'fat.o',
    'fileobj_mgr.o', 'disk.o', 'dircache.o', 'rb_namespace.o',
    'ata.o', 'ata-pp5020.o', 'ata_flash.o', 'storage.o', 'usb_storage.o',
)

# the front end's region (app-pp.lds, HAVE_UI_THREAD_SPLIT): apps/ipodui
# and these firmware objects, on cache lines of their own
FE_REGION_OBJS = ('font.o', 'lcd-16bit.o', 'bidi.o', 'diacritic.o',
                  'scroll_engine.o')

# cached statics the COP may write that are reviewed safe (object, symbol)
WRITE_OK = {
}

# reviewed: functions that name a table of function pointers only to look
# an entry up, never calling one (no table edges from them)
TABLE_LOOKUP = {
    # settings_list.c: the setting whose variable is at an address; the
    # shell resets and saves by remote call (fe_settings_save)
    'find_setting': 'a lookup',
}

# reviewed: data tables of numbers, not pointers.  .text starts at 0 on
# this layout, so a small number can equal a function's address (the
# combined p19 gate, 2026-11-05: a word of np_glyphs equalled
# buffering_thread's 0x4344 and made np_glyph_cov "reach" storage_sleep)
NUMERIC_TABLES = {
    'np_glyphs': 'np_draw.c: glyph sizes and offsets',
    # rc-0.8.0 (2026-11-06): fp_sincos's CORDIC angles (30 nonzero words,
    # under the big-table rule below); entry 18, 0x0000a2f9, came to equal
    # settings_load_config's Thumb address (0xa148 on main, 0xa2f8 once
    # the rc merges grew the .text before it) and made the crossfeed
    # filter "reach" set_codepage
    'atan_table': 'fixedpoint.c: CORDIC angles (unsigned long constants)',
}

# reviewed: numbers in a function's own literal pool, keyed by function and
# exact value (any other function address it loads still counts).  rc-0.8.0
# (2026-11-06): 719468, the days from 0000-03-01 to 1970-01-01 that
# "z += 719468" loads, is 0x000afa6c, which came to equal the ARM entry of
# filestr_discard_cache in fw-ui-mod and made ui_sw_split "reach" the file
# layer
# rc-0.8.1 (2026-11-09): 0xfffc, the DMA0 length mask of "(cmd & 0xfffc)"
# in pcm-pp.c's stop path, came to equal draw_band's ARM entry in fw-ship
# once #225 grew the .text, and made the PCM FIQ "reach" the font and
# storage code (fiq gate: 46 statics, 51 calls, all via draw_band)
NUMERIC_LITERALS = {
    'sink_dma_stop': {0xfffc: 'pcm-pp.c: DMA0 length mask (cmd & 0xfffc)'},
    'ui_sw_split': {719468: 'stopwatch.c: civil-from-days epoch shift'},
    'civil_from_days': {719468: 'datetime.c: civil-from-days epoch shift'},
}


def numeric_literal(elf, f, v):
    """a reviewed number in function f's literal pool, not an address"""
    return v in NUMERIC_LITERALS.get(base(elf.funcs.get(f, '')), ())


# callees whose function-pointer arguments run on the CPU, not the caller's
# core (besides every __wrap_X stub): the CPU-only kernel lists and the
# thread and job starts that name their core
HANDOFF = {'tick_add_task', 'timeout_register', 'add_event', 'add_event_ex',
           'create_thread', 'storage_post_job', 'cop_job_submit'}

# names the walk never enters: the kernel and the C library are built for
# both cores (their shared objects are uncached: xcore_map_check.py)
LEAF_OK = re.compile(r'^(mem(cpy|set|move|cmp|chr)|'
                     r'str(n?cpy|n?cat|n?cmp|len|nlen|r?chr|l(cpy|cat)|'
                     r'n?casecmp|tok_r|str|spn|cspn|toul?|tol)|'
                     r'__aeabi_\w+|__(u?div|u?mod)\w*|panicf|'
                     r'(v)?s?n?printf|vuprintf|sleep|yield|'
                     r'(mutex|semaphore|queue|corelock|wakeup)_\w+|'
                     r'thread_\w+|switch_thread|commit_\w*dcache\w*|'
                     r'cpu_boost\w*|ui_log\w*)$')


def base(name):
    return name.split('.')[0]


class Elf:
    def __init__(self, path):
        d = self.d = open(path, 'rb').read()
        shoff, = struct.unpack_from('<I', d, 0x20)
        shentsize, shnum, _ = struct.unpack_from('<HHH', d, 0x2e)
        self.secs = [struct.unpack_from('<IIIIIIIIII', d, shoff + i * shentsize)
                     for i in range(shnum)]
        st = [s for s in self.secs if s[1] == 2][0]
        strt = self.secs[st[6]]
        self.funcs, self.objs = {}, {}
        for i in range(st[5] // 16):
            nm, val, size, info, _, shndx = struct.unpack_from(
                '<IIIBBH', d, st[4] + i * 16)
            s = strt[4] + nm
            name = d[s:d.index(b'\0', s)].decode()
            t = info & 15
            if t == 2:
                self.funcs[val & ~1] = name
            elif t == 1 and size and shndx < len(self.secs):
                flags = self.secs[shndx][2]
                self.objs[val] = (name, size, bool(flags & 1))  # SHF_WRITE
        self.oaddr = sorted(self.objs)
        self.prog = [s for s in self.secs if s[1] == 1 and s[2] & 2]

    def word(self, a):
        for s in self.prog:
            if s[3] <= a < s[3] + s[5] - 3:
                return struct.unpack_from('<I', self.d, s[4] + a - s[3])[0]
        return None

    def obj_at(self, a):
        i = bisect.bisect_right(self.oaddr, a) - 1
        if i >= 0 and self.oaddr[i] <= a < self.oaddr[i] + self.objs[self.oaddr[i]][1]:
            return self.oaddr[i]
        return None


# sections that occupy no memory (their map addresses are file offsets)
NOALLOC = ('.ARM.attributes', '.debug', '.comment', '.note', '.stab')


def owners(mapfile):
    own = {}
    pend = None
    one = re.compile(r'^ (\.\S+|COMMON)\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(\S+\.o\)?)$')
    split = re.compile(r'^ (\.\S+|COMMON)$')
    cont = re.compile(r'^\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(\S+\.o\)?)$')
    for l in open(mapfile, errors='replace'):
        l = l.rstrip('\n')
        m = one.match(l)
        if m:
            if int(m.group(3), 16) and not m.group(1).startswith(NOALLOC):
                own[int(m.group(2), 16)] = m.group(4)
            pend = None
            continue
        m = split.match(l)
        if m:
            pend = not m.group(1).startswith(NOALLOC)
            continue
        if pend:
            m = cont.match(l)
            if m and int(m.group(2), 16):
                own[int(m.group(1), 16)] = m.group(3)
        pend = None
    addrs = sorted(own)

    def owner_of(a):
        i = bisect.bisect_right(addrs, a) - 1
        return own[addrs[i]] if i >= 0 else '?'
    return owner_of


def objname(path):
    m = re.search(r'\(([^()]+\.o)\)$', path)
    return m.group(1) if m else os.path.basename(path)


def graph(elf, objdump):
    """edges and writes keyed by function address (static names repeat)"""
    out = subprocess.run([objdump, '-d', '--no-show-raw-insn', elf.path],
                         capture_output=True, text=True).stdout.splitlines()
    lits = {}
    for l in out:
        m = re.match(r'^\s*([0-9a-f]+):\s+\.word\s+0x([0-9a-f]+)', l)
        if m:
            lits[int(m.group(1), 16)] = int(m.group(2), 16)
    edges = collections.defaultdict(set)
    writes = collections.defaultdict(set)
    refs = collections.defaultdict(set)
    hdr = re.compile(r'^([0-9a-f]+) <([^>]+)>:')
    ins = re.compile(r'^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$')
    br = re.compile(r'([0-9a-f]+) <([^>+]+)>')
    cur, regs, fns, handed, pool, loaded = None, {}, set(), set(), set(), set()
    # a register's statics loaded under a condition (ldrne r2, =a), kept for
    # the rest of the function: a later unconditional load into it in the
    # listing need not run (gcc branches over it: rc.6's pcmbuf_pcm_callback
    # merged two counters' increments into one store this way and the
    # walk saw only the second), so every store through that register may
    # write any of them. An over-approximation: a false row is reviewed,
    # a missed one is not
    maybe = {}
    conds = ('eq', 'ne', 'cs', 'hs', 'cc', 'lo', 'mi', 'pl', 'vs', 'vc',
             'hi', 'ls', 'ge', 'lt', 'gt', 'le')

    def close():
        if cur is not None:
            edges[cur] |= fns - handed
            for w in pool - loaded:         # a table inside the function
                v = lits[w]
                if v & ~1 in elf.funcs and not numeric_literal(elf, cur, v):
                    edges[cur].add(v & ~1)
    for l in out:
        m = hdr.match(l)
        if m:
            close()
            cur = int(m.group(1), 16)
            if cur not in elf.funcs:
                cur = None
            regs, fns, handed, pool, loaded = {}, set(), set(), set(), set()
            maybe = {}
            continue
        m = ins.match(l) if cur is not None else None
        if not m:
            continue
        op, args = m.group(2), m.group(3)
        if op == '.word':
            pool.add(int(m.group(1), 16))
            continue
        mb = br.match(args)
        if op[0] == 'b' and mb and not op.startswith('bic'):
            t = int(mb.group(1), 16)
            if t in elf.funcs and t != cur:
                edges[cur].add(t)
            if op.startswith('bl'):
                # a callback handed to a remote-call stub or a CPU-only
                # registration runs on the CPU: no edge
                to_cpu = t in elf.funcs and (
                    elf.funcs[t].startswith('__wrap_')
                    or base(elf.funcs[t]) in HANDOFF)
                for r in ('r0', 'r1', 'r2', 'r3'):
                    v = regs.get(r)
                    if v is None:
                        continue
                    if v[0] == 'o':
                        writes[cur].add(v[1])
                    elif to_cpu:
                        handed.add(v[1])
                for r in ('r0', 'r1', 'r2', 'r3', 'ip', 'lr'):
                    regs.pop(r, None)
            continue
        parts = [p.strip() for p in re.split(r'[;@]', args)[0].split(',')]
        dst = parts[0]
        if op.startswith('ldr') and '[pc' in args:
            mm = re.search(r'[;@]\s*([0-9a-f]+) <', args)
            w = int(mm.group(1), 16) if mm else None
            v = lits.get(w) if w is not None else None
            if w is not None:
                loaded.add(w)
            if v is not None and numeric_literal(elf, cur, v):
                regs.pop(dst, None)
                continue
            if v is not None and v & ~1 in elf.funcs:
                regs[dst] = ('f', v & ~1)
                fns.add(v & ~1)
                continue
            o = elf.obj_at(v) if v is not None else None
            if o is not None:
                regs[dst] = ('o', o)
                refs[cur].add(o)
                if op[3:5] in conds:
                    maybe.setdefault(dst, set()).add(o)
            else:
                regs.pop(dst, None)
            continue
        if op.startswith(('str', 'stm')):
            mm = re.search(r'\[(\w+)', args)
            b = mm.group(1) if mm else dst.rstrip('!')
            if regs.get(b, ('',))[0] == 'o':
                writes[cur].add(regs[b][1])
            writes[cur] |= maybe.get(b, set())
            continue
        if op.startswith(('add', 'sub', 'mov', 'orr')) and len(parts) >= 2:
            src = [p for p in parts[1:] if p in regs]
            if src:
                regs[dst] = regs[src[0]]
            else:
                regs.pop(dst, None)
            for p in parts[1:]:
                if p in maybe:
                    maybe.setdefault(dst, set()).update(maybe[p])
            continue
        if not op.startswith(('cmp', 'tst', 'teq', 'cmn', 'push', 'pop')):
            regs.pop(dst, None)
    close()
    # the functions in the data tables a function names, through the
    # tables those point at (menus nest)
    memo = {}

    def table(o):
        if o in memo:
            return memo[o]
        memo[o] = t = set()
        name, size, _ = elf.objs[o]
        if base(name) in NUMERIC_TABLES:
            return t
        fns, subs, nonzero = set(), set(), 0
        for off in range(0, min(size, 1 << 16) - 3, 4):
            w = elf.word(o + off)
            if w is None:
                break
            if not w:
                continue
            nonzero += 1
            if w & ~1 in elf.funcs:
                fns.add(w & ~1)
            else:
                o2 = elf.obj_at(w)
                if o2 is not None and o2 != o:
                    subs.add(o2)
        # a table of numbers (the Unicode tables) can hold a word that
        # equals an address: in a big table, pointers must be a quarter of
        # its words
        if nonzero > 32 and (len(fns) + len(subs)) * 4 < nonzero:
            return t
        t |= fns
        for o2 in subs:
            t |= table(o2)
        return t
    sys.setrecursionlimit(20000)
    for f, os_ in refs.items():
        if base(elf.funcs.get(f, '')) in TABLE_LOOKUP:
            continue
        for o in os_:
            edges[f] |= table(o)
    return edges, writes


def walk(elf, edges, roots):
    prev = {r: None for r in roots}
    q = collections.deque(roots)
    while q:
        n = q.popleft()
        for m in sorted(edges.get(n, ())):
            nm = base(elf.funcs[m])
            if (m in prev or elf.funcs[m].startswith('__wrap_')
                    or nm in CPU_SIDE or LEAF_OK.match(nm)):
                continue
            prev[m] = n
            q.append(m)
    return prev


def path(elf, prev, n, k=12):
    p = []
    while n is not None and len(p) < k:
        p.append(elf.funcs[n])
        n = prev[n]
    return ' <- '.join(p)


def check(build, roots=None, objdump=None):
    """(fs, line): lists of (what, path) findings"""
    objdump = objdump or os.environ.get('OBJDUMP', 'arm-none-eabi-objdump')
    elf = Elf(os.path.join(build, 'rockbox.elf'))
    elf.path = os.path.join(build, 'rockbox.elf')
    owner_of = owners(os.path.join(build, 'rockbox.map'))
    edges, writes = graph(elf, objdump)
    byname = {}
    for a, n in elf.funcs.items():
        byname.setdefault(n, a)
    roots = [byname[r] for r in (roots or ROOTS) if r in byname]
    if not roots:
        return None, None
    # the shell modules run on the ui thread too: every core function they
    # call (modules/keep.txt; a wrapped one appears as its __wrap_ stub,
    # which ends the path) is a root
    keep = os.path.join(build, 'modules', 'keep.txt')
    if os.path.exists(keep):
        for l in open(keep):
            n = l.strip()[len('--undefined='):]
            if n in byname and not n.startswith('__wrap_') \
                    and '__wrap_' + n not in byname:
                roots.append(byname[n])
    prev = walk(elf, edges, roots)
    fs, line = [], []

    def obj_of(f):
        return objname(owner_of(f))

    def caller(f):
        # the linker's veneers (interworking, long branches) are no caller
        f = prev[f]
        while f is not None and re.match(r'^__\w+_(veneer|from_thumb|'
                                         r'from_arm)$', elf.funcs[f]):
            f = prev[f]
        return f
    for f in prev:
        # where a path enters the file layer: its caller is outside it
        o = obj_of(f)
        c = caller(f)
        if o in FS_OBJS and c is not None and obj_of(c) not in FS_OBJS:
            fs.append(('%s (%s)' % (elf.funcs[f], o), path(elf, prev, f)))
    seen = set()
    for f in prev:
        for a in writes.get(f, ()):
            name, size, writable = elf.objs[a]
            if not writable or a >= 0x10000000 or a in seen:
                continue        # read-only, uncached alias or IRAM
            own = owner_of(a)
            o = objname(own)
            if '/apps/ipodui/' in own or o in FE_REGION_OBJS:
                continue
            if (o, base(name)) in WRITE_OK:
                continue
            seen.add(a)
            line.append(('%s:%s' % (o, name), path(elf, prev, f)))
    return sorted(fs), sorted(line)


def main(argv):
    strict = '--strict' in argv
    strict_lines = '--strict-lines' in argv
    listing = '--list' in argv
    args = [a for a in argv
            if a not in ('--strict', '--strict-lines', '--list')]
    roots = []
    while '--root' in args:
        i = args.index('--root')
        roots.append(args[i + 1])
        del args[i:i + 2]
    if len(args) != 1:
        print(__doc__)
        return 2
    if not os.path.exists(os.path.join(args[0], 'rockbox.map')):
        print('febe reach: no rockbox.map in %s' % args[0])
        return 2
    fs, line = check(args[0], roots or None)
    if fs is None:
        print('febe reach: no %s in %s (not a split shell build)'
              % ('/'.join(roots or ROOTS), args[0]))
        return 0
    print('febe reach: %d file-layer entries (%s) and %d cached statics '
          'outside the front-end region (%s) reachable from the ui thread'
          % (len(fs), 'strict' if strict else 'warn', len(line),
             'strict' if strict_lines else 'warn'))
    for kind, items in (('fs', fs), ('line', line)):
        for what, p in (items if listing or kind == 'fs' else items[:4]):
            print('  %-4s %-40s %s' % (kind, what, p))
    return 1 if (strict and fs) or (strict_lines and line) else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
