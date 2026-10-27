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

"""Worst-case call-chain stack depth from gcc -fcallgraph-info=su,da .ci
files (risk tool 3, docs/risk-tooling.md).

    stack_depth.py <build-dir> <entry-function> [<entry-function> ...]

Scans <build-dir> for every "*.ci" file (gcc -fcallgraph-info=su,da: one
per translation unit, a small VCG graph with each function's own stack
frame size, "node: { title: "f" label: "f\\n<file>:<line>:<col>\\nN bytes
(static|dynamic)..." }", and its direct calls, "edge: { sourcename: "A"
targetname: "B" ... }"), builds one combined call graph, and for each
entry function prints the worst-case (deepest total stack bytes) call
chain by a DAG longest-path search (memoized DFS). A cycle (direct or
mutual recursion) is reported, not silently unrolled: the cycle's own
functions are counted once each (not again per unknown iteration count),
and the result is flagged "(recursive: unbounded)".

**Limitation, stated rather than hidden**: this follows only *direct*
calls ("bl"/function-name calls) -- gcc's callgraph does not record
indirect calls through a function pointer (event handler callbacks,
`vtbl`-style dispatch), so a thread that reaches deep code only through
one is undercounted here. A function with no outgoing edges recorded at
all beyond plain calls, but which qemu/objdump shows taking a function
pointer argument, should be checked by hand; this script does not detect
indirect calls (see scripts/risk/lib/objdump_count.py's "calls" column
for a related, direct-call-only view used by risk tool 1, with the same
limitation).

**Also**: function names are matched as plain symbol names, like the
callgraph does. Two unrelated `static` functions of the same name in
different translation units are indistinguishable here (rare in this
codebase, but possible) -- a path through one may be misattributed to
the other, making the computed depth a plausible but not certified
upper bound. The .ci stack byte figures are gcc's own worst-case frame
size per function (not a measurement), already conservative (padding,
spill slots for every code path, not just the one taken).
"""
import glob
import os
import re
import sys

RE_NODE = re.compile(
    r'node:\s*\{\s*title:\s*"([^"]*)"\s*label:\s*"([^"]*)"\s*\}')
RE_EDGE = re.compile(
    r'edge:\s*\{\s*sourcename:\s*"([^"]*)"\s*targetname:\s*"([^"]*)"')
RE_BYTES = re.compile(r'(\d+) bytes \((\w+)\)')


def parse_ci_files(build_dir):
    """Returns (stack_bytes: {name: int}, calls: {name: set(name)},
    dynamic: {name: bool})."""
    stack_bytes = {}
    calls = {}
    dynamic = {}
    for path in glob.glob(os.path.join(build_dir, '**', '*.ci'),
                          recursive=True):
        with open(path, errors='replace') as f:
            text = f.read()
        for m in RE_NODE.finditer(text):
            name, label = m.group(1), m.group(2)
            bm = RE_BYTES.search(label)
            if bm:
                n, kind = int(bm.group(1)), bm.group(2)
                # keep the largest recorded frame for this symbol name
                # (static-name collisions across TUs: see module docstring)
                if n > stack_bytes.get(name, -1):
                    stack_bytes[name] = n
                    dynamic[name] = (kind != 'static')
            calls.setdefault(name, set())
        for m in RE_EDGE.finditer(text):
            src, dst = m.group(1), m.group(2)
            calls.setdefault(src, set()).add(dst)
    return stack_bytes, calls, dynamic


def worst_case(entry, stack_bytes, calls, leaves=()):
    """DFS with memoization; returns (total_bytes, path, recursive_set).
    'leaves': function names whose own callees are never followed (their
    own stack frame still counts) -- for 'yield'/'switch_thread', whose
    kernel-internal stack-overflow check (thread_stkov -> thread_panicf
    -> a printf-style formatter) is reachable from literally every
    blocking call in the firmware, so following it into every thread's
    worst case makes the deepest path nearly identical in shape for
    every entry point and hides each thread's own logic -- see
    docs/risk-tooling.md. Reported as a separate, shared, system-wide
    figure instead (scripts/risk/arm_insn_count... /stack_usage.sh)."""
    memo = {}
    on_stack = []
    recursive = set()

    def visit(name):
        if name in memo:
            return memo[name]
        if name in on_stack:
            recursive.add(name)
            return (0, [])          # cycle: counted at the call site instead
        on_stack.append(name)
        own = stack_bytes.get(name, 0)
        best_extra, best_path = 0, []
        if name not in leaves:
            for callee in sorted(calls.get(name, ())):
                extra, path = visit(callee)
                if extra > best_extra:
                    best_extra, best_path = extra, path
        on_stack.pop()
        result = (own + best_extra, [name] + best_path)
        memo[name] = result
        return result

    total, path = visit(entry)
    return total, path, recursive


def short(name):
    """Drop the absolute-path prefix gcc adds to disambiguate a static
    function's name ("<abspath>:<func>" -> "<basename>:<func>")."""
    if ':' in name and '/' in name.rsplit(':', 1)[0]:
        path, func = name.rsplit(':', 1)
        return '%s:%s' % (os.path.basename(path), func)
    return name


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    build_dir = sys.argv[1]
    entries = sys.argv[2:]
    # system-wide shared tail, reachable from almost any blocking call
    # (see worst_case's docstring); reported once at the end instead of
    # inflating every single thread's own number identically.
    SHARED_LEAVES = ('yield', 'switch_thread')
    stack_bytes, calls, dynamic = parse_ci_files(build_dir)
    if not stack_bytes:
        sys.exit('no .ci data found under %s (build with '
                  '-fcallgraph-info=su,da first)' % build_dir)
    all_names = set(stack_bytes) | set(calls)
    for entry in entries:
        resolved = entry
        if entry not in all_names:
            # static functions are keyed "<abspath>:<name>" (gcc
            # disambiguates internal linkage per translation unit);
            # match by suffix when the plain name isn't a key.
            matches = sorted(n for n in all_names
                             if n == entry or n.endswith(':' + entry))
            if not matches:
                print('%-28s NOT FOUND in callgraph (wrong name, or '
                      'never called/compiled into this build)' % entry)
                continue
            if len(matches) > 1:
                print('%-28s NOTE: %d definitions match "%s" (static-name '
                      'collision across files); using %s'
                      % ('', len(matches), entry, matches[0]))
            resolved = matches[0]
        total, path, recursive = worst_case(resolved, stack_bytes, calls,
                                            leaves=SHARED_LEAVES)
        tag = (' (%d other recursive function(s) reachable from here, not '
               'on this path: unbounded if actually called in a cycle)'
               % len(recursive)) if recursive else ''
        print('%-28s %6d bytes worst-case%s' % (entry, total, tag))
        print('  path: ' + ' -> '.join(
            '%s(%d)' % (short(f), stack_bytes.get(f, 0)) for f in path))

    if 'yield' in all_names:
        ytotal, ypath, _ = worst_case('yield', stack_bytes, calls)
        print('\nshared tail (every yield()/switch_thread() call can '
              'reach this; add it on top of any of the above that ends '
              'at a blocking call): yield(...) worst case %d bytes'
              % ytotal)
        print('  path: ' + ' -> '.join(
            '%s(%d)' % (short(f), stack_bytes.get(f, 0)) for f in ypath))


if __name__ == '__main__':
    main()
