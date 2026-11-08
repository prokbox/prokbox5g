#!/usr/bin/env python3
"""Misaligned halfword/word accesses to static data, found in the linked ELF.

On the PP5022 a halfword or word load/store to SDRAM at an address that is
not a multiple of its size takes a data abort [silicon, sitting 4gh,
2026-11-06; docs/pp5022-reference/quirks.md].  The sitting 4gh boot loop
was one: ata_get_info() stored IDENTIFY words with strh into static char
arrays (alignment 1) through an (unsigned short *) cast, and a link put
them on odd addresses.  The compiler cannot warn when the cast hides the
type, and the fault depends on where the link happens to put the object,
so the check runs on the linked image.

What it does: disassembles the ELF (objdump -d) and, per function, follows
registers loaded from a literal pool with an address inside a RAM data
section (.data, .bss, .ncdata, .ncbss, .ibss, .iram's data), through
mov / add / sub with an immediate and pre/post-indexed writeback.  A
halfword access (ldrh/strh/ldrsh) or word access (ldr/str, single
register) based on such a register fails the check when:
  - the offset is an immediate and base + offset is misaligned, or
  - the offset is a register and the base itself is misaligned (an odd
    base with an even index: the array-of-halfwords pattern).
A forward data flow per function: a register is forgotten when anything
else writes it, and at a join it is known only when every path agrees.  Byte accesses, ldm/stm and ldrd are not checked.

Usage: misaligned_access_check.py [--objdump TOOL] ELF...
Exit 1 on any finding (each listed with the function, the instruction, the
symbol and the address), 0 otherwise.
"""
# Copyright (C) 2026 prokopto-dev
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
# KIND, either express or implied.

import argparse
import bisect
import re
import subprocess
import sys

DATA_SECTIONS = ('.data', '.bss', '.ncdata', '.ncbss', '.ibss', '.idata')

INS = re.compile(r'^\s*([0-9a-f]+):\t([0-9a-f]{4}(?: [0-9a-f]{4})?|[0-9a-f]{8})'
                 r' *\t(\S+)\t?([^@;]*)(?:[@;]\s*(.*))?$')
FUNC = re.compile(r'^([0-9a-f]+) <(.*)>:$')
REG = r'(r\d+|ip|lr|sl|fp|sb|sp)'
# ldrh r3, [lr, r2] / strh r3, [ip, #2]! / ldr r0, [r1], #4 ...
MEM = re.compile(r'^' + REG + r',\s*\[' + REG +
                 r'(?:,\s*(#-?\d+|-?' + REG + r'(?:,[^\]]*)?))?\](!?)'
                 r'(?:,\s*(#-?\d+|' + REG + r'))?\s*$')
LIT = re.compile(r'^' + REG + r',\s*\[pc(?:,\s*#-?\d+)?\]\s*$')
ADDSUB = re.compile(r'^' + REG + r',\s*' + REG + r',\s*#(-?\d+)\s*$')
MOV = re.compile(r'^' + REG + r',\s*' + REG + r'\s*$')
COND = r'(eq|ne|cs|cc|hs|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le|al)?'
HALF = re.compile(r'^(ldr|str)' + COND + r'(h|sh)' + COND + r'(\.[nw])?$')
WORD = re.compile(r'^(ldr|str)' + COND + r'(\.[nw])?$')
NODEST = ('str', 'cmp', 'cmn', 'tst', 'teq', 'push', 'stm', 'b', 'bl', 'bx',
          'msr', 'mcr', 'nop', 'svc', 'swi', 'bkpt', 'pld', '.')


def sections(elf, objdump):
    out = subprocess.run([objdump, '-h', elf], capture_output=True,
                         text=True, check=True).stdout
    spans = []
    for line in out.splitlines():
        f = line.split()
        if len(f) >= 4 and f[0].isdigit():
            name, size, vma = f[1], int(f[2], 16), int(f[3], 16)
            if size and name.startswith(DATA_SECTIONS):
                spans.append((vma, vma + size, name))
    return sorted(spans)


def symbols(elf, nm):
    out = subprocess.run([nm, '-n', elf], capture_output=True, text=True,
                         check=True).stdout
    syms = []
    for line in out.splitlines():
        f = line.split()
        if len(f) == 3 and f[1] in 'bBdDsS' and not f[2].startswith('$'):
            syms.append((int(f[0], 16), f[2]))
    return syms


def sym_of(syms, keys, a):
    i = bisect.bisect_right(keys, a) - 1
    if i < 0:
        return '?'
    base, name = syms[i]
    return name if a == base else '%s+%d' % (name, a - base)


def check(elf, objdump, nm):
    spans = sections(elf, objdump)
    syms = symbols(elf, nm)
    keys = [s[0] for s in syms]

    def in_data(a):
        for lo, hi, _ in spans:
            if lo <= a < hi:
                return True
        return False

    dis = subprocess.run([objdump, '-d', elf], capture_output=True,
                         text=True, check=True).stdout.splitlines()
    words = {}
    for line in dis:
        m = INS.match(line)
        if m and m.group(3) == '.word':
            words[int(m.group(1), 16)] = int(m.group(2), 16)

    # the functions, each a list of (addr, op, args, note)
    funcs = []
    for line in dis:
        m = FUNC.match(line)
        if m:
            funcs.append((m.group(2), []))
            continue
        m = INS.match(line)
        if m and funcs and not m.group(3).startswith('.'):
            funcs[-1][1].append((int(m.group(1), 16), m.group(3),
                                 m.group(4).strip(), m.group(5) or ''))

    findings = set()
    for func, ins in funcs:
        flow(func, ins, words, in_data, syms, keys, findings)
    return sorted(findings)


def conditional(op):
    """an instruction with a condition other than al (it may not run)"""
    m = re.match(r'^(ldr|str|add|sub|mov)(eq|ne|cs|cc|hs|lo|mi|pl|vs|vc|hi|'
                 r'ls|ge|lt|gt|le)', op)
    return m is not None


def transfer(state, ins, words, in_data, syms, keys, func, findings):
    """one instruction: the registers known to hold a static data address
    after it, and any misaligned access it makes"""
    addr, op, args, note = ins
    regs = dict(state)
    base_op = op.rstrip('.nw')
    cond = conditional(base_op)

    def kill(r):
        regs.pop(r, None)

    def setr(r, v):
        if cond and regs.get(r) != v:
            kill(r)             # may not run: the old value may stay
        else:
            regs[r] = v

    lm = LIT.match(args)
    if base_op.startswith('ldr') and lm and WORD.match(op):
        la = re.match(r'([0-9a-f]+)', note)
        v = words.get(int(la.group(1), 16)) if la else None
        if v is not None and in_data(v):
            setr(lm.group(1), v)
        else:
            kill(lm.group(1))
        return regs

    mm = MEM.match(args)
    half = HALF.match(op)
    word = WORD.match(op)
    if mm and (half or word):
        rt, rn, off, wb, post = (mm.group(1), mm.group(2), mm.group(3),
                                 mm.group(5), mm.group(6))
        size = 2 if half else 4
        if rn in regs:
            base = regs[rn]
            if off and off.startswith('#'):
                ea = base + int(off[1:])
                bad = ea % size != 0
            else:
                ea = base
                bad = base % size != 0
            if bad:
                findings.add('%s: %08x %s %s  -> %s @ %08x (%d-byte access, '
                             'address %% %d = %d)' % (
                                 func, addr, op, args, sym_of(syms, keys, ea),
                                 ea, size, size, ea % size))
            if wb == '!' and off and off.startswith('#'):
                setr(rn, base + int(off[1:]))
            elif post and post.startswith('#'):
                setr(rn, base + int(post[1:]))
            elif wb == '!' or post:
                kill(rn)
        elif wb == '!' or post:
            kill(rn)
        if op.startswith('ldr'):
            kill(rt)
        return regs

    am = ADDSUB.match(args)
    if am and base_op[:3] in ('add', 'sub') and \
            not base_op.startswith(('adds', 'subs')):
        rd, rs, imm = am.group(1), am.group(2), int(am.group(3))
        if rs in regs:
            setr(rd, regs[rs] + (imm if base_op.startswith('add') else -imm))
        else:
            kill(rd)
        return regs
    mv = MOV.match(args)
    if mv and base_op.startswith('mov') and not base_op.startswith('movs'):
        if mv.group(2) in regs:
            setr(mv.group(1), regs[mv.group(2)])
        else:
            kill(mv.group(1))
        return regs
    if base_op.startswith(('ldm', 'pop')):
        return {}
    if base_op.startswith(('bl', 'blx')) and not base_op.startswith('bls'):
        for r in ('r0', 'r1', 'r2', 'r3', 'ip', 'lr'):
            kill(r)
        return regs
    # anything else that writes its first operand forgets it
    if not base_op.startswith(NODEST):
        d = re.match(REG, args)
        if d:
            kill(d.group(1))
    return regs


def flow(func, ins, words, in_data, syms, keys, findings):
    """a forward data flow over one function: at a join, a register is
    known only when every path into it agrees (back edges included)"""
    n = len(ins)
    index = {a: i for i, (a, _, _, _) in enumerate(ins)}
    succ = [[] for _ in range(n)]
    for i, (a, op, args, note) in enumerate(ins):
        base_op = op.rstrip('.nw')
        t = re.match(r'([0-9a-f]+) <', args)
        is_branch = re.match(r'^b' + COND + r'$', base_op) is not None
        uncond = base_op in ('b', 'bal') or \
            re.match(r'^bx(al)?$', base_op) is not None or \
            (base_op.startswith(('pop', 'ldm')) and 'pc' in args and
             not conditional(base_op[:3] + base_op[3:])) or \
            (base_op in ('mov', 'ldr') and args.startswith('pc'))
        if is_branch and t and int(t.group(1), 16) in index:
            succ[i].append(index[int(t.group(1), 16)])
        if not uncond and i + 1 < n:
            succ[i].append(i + 1)
    TOP = None
    inn = [TOP] * n
    if n:
        inn[0] = {}
    work = list(range(n))
    seen = set()
    while work:
        i = work.pop(0)
        if inn[i] is TOP:
            continue
        out = transfer(inn[i], ins[i], words, in_data, syms, keys, func,
                       set())
        for j in succ[i]:
            if inn[j] is TOP:
                new = dict(out)
            else:
                new = {r: v for r, v in inn[j].items() if out.get(r) == v}
            if new != inn[j] or j not in seen:
                seen.add(j)
                if new != inn[j]:
                    inn[j] = new
                    work.append(j)
    for i in range(n):
        if inn[i] is not TOP:
            transfer(inn[i], ins[i], words, in_data, syms, keys, func,
                     findings)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--objdump', default='arm-none-eabi-objdump')
    ap.add_argument('--nm', default='arm-none-eabi-nm')
    ap.add_argument('elf', nargs='+')
    a = ap.parse_args()
    rc = 0
    for elf in a.elf:
        f = check(elf, a.objdump, a.nm)
        if f:
            rc = 1
            print('misaligned access check: %s: %d misaligned halfword/word '
                  'access(es) to static data (a data abort on the PP5022; '
                  'docs/pp5022-reference/quirks.md):' % (elf, len(f)))
            for x in f:
                print('  ' + x)
        else:
            print('misaligned access check: %s: clean' % elf)
    return rc


if __name__ == '__main__':
    sys.exit(main())
