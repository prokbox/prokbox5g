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

"""Cross-core object gate (P19 S0, K-2; docs/design/p19-ui-on-cop.md 3.2).

usage: scripts/xcore_map_check.py [--list FILE] [--require FILE:SYM]...
                                  [--max-age-from FILE] [--expect-fail] ELF...

Every kernel object (mutex, semaphore, event queue, corelock, wakeup) or
shared flag that threads on both PP5022 cores touch must be linked in
uncached memory: .ncbss/.ncdata (SHAREDBSS_ATTR, the uncached alias) or
IRAM (.ibss/.idata/.iram*, uncached).  A cached static gives each core its
own stale copy.  Reads the symbol table (arm-none-eabi-objdump -t), which
names each local symbol's source file, and checks every ELF given:

 1. "need FILE:SYM": present and uncached in every ELF (a rename must
    update the list).
    "need FILE:SYM if FILE:ANCHOR": the same, in every ELF that has the
    anchor (an object or a function: the build flavour that compiles it).
    "need FILE:SYM?": uncached where present, and present in at least one
    of the ELFs checked in this run.
 2. every cached object whose name looks like a kernel object (*_mtx,
    *mutex*, *_queue, *_sem, *_ack, *_cl, *wakeup*, or a bare sem, done,
    reply, poster, work) must be on an "allow" line (proven single-core,
    with the reason).  A new cached one fails.

--require adds need lines (the planted-object self-test: a known cached
object must fail).  --max-age-from FILE skips (and reports) any ELF older
than FILE: a stale ELF from an earlier run is never gated.  --expect-fail
inverts the exit status.
"""
import os, re, subprocess, sys

UNCACHED = re.compile(r'^\.(ncbss|ncdata|ibss|idata|iram|icode)')
KERNELISH = re.compile(r'(_mtx|mutex|_queue$|^queue$|_sem$|_ack$|_cl$|wakeup'
                       r'|^(sem|done|reply|poster|work)(\.\d+)?$)')
HERE = os.path.dirname(os.path.abspath(__file__))
OBJDUMP = os.environ.get('OBJDUMP', 'arm-none-eabi-objdump')


def symbols(elf):
    """({(file, name): section} for data objects, {(file, name)} for all)"""
    out = subprocess.run([OBJDUMP, '-t', elf], capture_output=True,
                         text=True, check=True).stdout
    objs, every, cur = {}, set(), '?'
    for line in out.splitlines():
        m = re.match(r'^[0-9a-f]{8} (.{7}) (\S+)\s+[0-9a-f]{8} (.+)$', line)
        if not m:
            continue
        flags, sect, name = m.groups()
        if 'f' in flags[5:7] and sect == '*ABS*':
            cur = name                  # the FILE symbol before its locals
            continue
        fname = cur if flags[0] == 'l' else '*'
        every.add((fname, name))
        if 'O' in flags[5:7]:
            objs[(fname, name)] = sect
    return objs, every


def parse_need(ref):
    """'F:S', 'F:S?' or 'F:S if F:A' -> (ref, optional, anchor)"""
    parts = ref.split()
    anchor = None
    if len(parts) == 3 and parts[1] == 'if':
        anchor = parts[2]
    elif len(parts) != 1:
        raise SystemExit('xcore: bad need: ' + ref)
    r = parts[0]
    return r.rstrip('?'), r.endswith('?'), anchor


def load_list(path):
    need, allow = [], set()
    for raw in open(path):
        line = raw.split('#', 1)[0].strip()
        if not line:
            continue
        kind, ref = line.split(None, 1)
        if kind == 'need':
            need.append(parse_need(ref.strip()))
        elif kind == 'allow':
            allow.add(ref.strip())
        else:
            raise SystemExit('xcore: bad list line: ' + raw.rstrip())
    return need, allow


def match(table, ref):
    f, s = ref.split(':', 1)
    return [k for k in table if k[1] == s and (k[0] == f or k[0] == '*'
                                               or f == '*')]


LOCAL_N = re.compile(r'^(.+)\.(\d+)$')


def allow_key(f, n, objs):
    """The allow-list key of object (f, n).  GCC names a function-local
    static NAME.N, N a counter over the whole translation unit, so any
    new local static elsewhere in the file renumbers it (rc-0.8.0:
    #154's tags[] turned tagcache.c's wakeup_tick.6 into .7).  An allow
    line names it without the number, "FILE:NAME", and that key covers
    it only while it is the file's one object of that name; a second
    one keeps its numbered key, which no line names, so it fails."""
    m = LOCAL_N.match(n)
    if not m or f == '*':
        return '%s:%s' % (f, n)
    same = [k for k in objs if k[0] == f and LOCAL_N.match(k[1])
            and LOCAL_N.match(k[1]).group(1) == m.group(1)]
    return '%s:%s' % (f, m.group(1) if len(same) == 1 else n)


def check(elf, need, allow, seen, used):
    objs, every = symbols(elf)
    bad, ok = [], 0
    for ref, opt, anchor in need:
        if anchor and not match(every, anchor):
            continue                    # not this build's flavour
        hits = match(objs, ref)
        if hits:
            seen.add(ref)
        elif not opt:
            bad.append('missing  %s (not in %s; renamed? update the list)'
                       % (ref, elf))
        for k in hits:
            if UNCACHED.match(objs[k]):
                ok += 1
            else:
                bad.append('cached   %s in %s (needs SHAREDBSS/XCORE_BSS)'
                           % (ref, objs[k]))
    needed = {r for r, _, _ in need}
    kl, allowed = 0, 0
    for (f, n), sect in sorted(objs.items()):
        if UNCACHED.match(sect) or not KERNELISH.search(n):
            continue
        if sect.startswith('.text') or sect.startswith('.rodata'):
            continue
        kl += 1
        key = allow_key(f, n, objs)
        if key in allow:
            allowed += 1
            used.add(key)
            continue
        if '%s:%s' % (f, n) in needed:
            continue                    # reported above
        bad.append('unlisted %s:%s in %s (cached kernel-like object: '
                   'XCORE_BSS, or an allow line with the reason)'
                   % (f, n, sect))
    for b in bad:
        print('xcore: ' + b)
    print('xcore: %s: %d need ok, %d allowed, %d cached kernel-like'
          % (elf, ok, allowed, kl))
    return not bad


def main(argv):
    lst = os.path.join(HERE, 'xcore_objects.txt')
    extra, elfs, expect_fail, since = [], [], False, None
    i = 0
    while i < len(argv):
        a = argv[i]
        if a in ('--list', '--require', '--max-age-from'):
            if i + 1 >= len(argv):
                print(__doc__)
                return 2
            v = argv[i + 1]
            i += 1
            if a == '--list':
                lst = v
            elif a == '--require':
                extra.append(parse_need(v))
            else:
                since = os.path.getmtime(v)
        elif a == '--expect-fail':
            expect_fail = True
        else:
            elfs.append(a)
        i += 1
    if since is not None:
        fresh = [e for e in elfs if os.path.getmtime(e) >= since]
        for e in elfs:
            if e not in fresh:
                print('xcore: skip %s (not built in this run)' % e)
        elfs = fresh
    if not elfs:
        print('xcore: no ELF to check')
        return 0 if not expect_fail else 1
    need, allow = load_list(lst)
    need += extra
    seen = set()
    used = set()
    good = all([check(e, need, allow, seen, used) for e in elfs])
    for ref in sorted(allow - used):
        # a note, not a failure: a flavour this run did not build may
        # still need the line
        print('xcore: note: allow %s matched nothing in the ELFs checked '
              '(renamed, numbered, or another flavour\'s?)' % ref)
    for ref, opt, anchor in need:
        if opt and ref not in seen:
            print('xcore: absent   %s? from every ELF checked (renamed? '
                  'update the list)' % ref)
            good = False
    if expect_fail:
        print('xcore: planted object %s' % ('caught' if not good
                                             else 'NOT caught'))
        return 0 if not good else 1
    return 0 if good else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
