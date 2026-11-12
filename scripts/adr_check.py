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

"""Check docs/adr: unique numbers, one index row per file, valid status.

usage: scripts/adr_check.py [--root DIR] [--main REF]

Fails (exit 1, one line per problem) on: two files with the same number; an ADR
file with no row in docs/adr/README.md, or a row naming no file; a status line
that is not Proposed / Accepted / Rejected / Superseded by NNNN (existing); a
status that differs from the index row; a title heading with the wrong number.
--main REF (a revision, resolved in --root) also fails when REF already gives a
number to a file with a different name (two branches took the same number).
Prints how many ADRs it checked; zero ADRs is an error, not a pass.
"""
import argparse, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
FILE_RE = re.compile(r'^(\d{4})-([a-z0-9-]+)\.md$')
STATUS_RE = re.compile(r'^(Proposed|Accepted|Rejected|Superseded by (\d{4}))$')


def check(root, main=None):
    """-> (problems, count)"""
    p = []
    d = os.path.join(root, 'docs/adr')
    if not os.path.isdir(d):
        return ['docs/adr missing'], 0
    files = {}
    for f in sorted(os.listdir(d)):
        m = FILE_RE.match(f)
        if m and m.group(1) != '0000':
            files.setdefault(m.group(1), []).append(f)
    for n, fs in files.items():
        if len(fs) > 1:
            p.append('duplicate ADR number %s: %s' % (n, ', '.join(fs)))
    if not files:
        p.append('no ADRs found in docs/adr (a check that inspected nothing must not pass)')
    idx = os.path.join(d, 'README.md')
    rows = {}
    if not os.path.isfile(idx):
        p.append('docs/adr/README.md (the index) missing')
    else:
        for l in open(idx).read().split('\n'):
            m = re.match(r'^\|\s*\[(\d{4})\]\(([^)]+)\)\s*\|[^|]*\|\s*([^|]+?)\s*\|', l)
            if m:
                if m.group(1) in rows:
                    p.append('index lists ADR %s twice' % m.group(1))
                rows[m.group(1)] = (m.group(2), m.group(3))
    for n, fs in sorted(files.items()):
        for f in fs:
            t = open(os.path.join(d, f)).read()
            if n not in rows:
                p.append('ADR %s (%s) has no row in docs/adr/README.md' % (n, f))
            elif rows[n][0] != f:
                p.append('index row %s links %s, the file is %s' % (n, rows[n][0], f))
            h = re.search(r'^# ADR (\d{4}):', t, re.M)
            if not h or h.group(1) != n:
                p.append('%s: title heading must start "# ADR %s:"' % (f, n))
            s = re.search(r'^- Status:\s*(.+?)\s*$', t, re.M)
            if not s or not STATUS_RE.match(s.group(1)):
                p.append('%s: status %r is not Proposed / Accepted / Rejected / Superseded by NNNN' %
                         (f, s.group(1) if s else None))
                continue
            sm = STATUS_RE.match(s.group(1))
            if sm.group(2) and sm.group(2) not in files:
                p.append('%s: superseded by %s, which does not exist' % (f, sm.group(2)))
            if n in rows and rows[n][1] != s.group(1):
                p.append('%s: status %r differs from the index row %r' % (f, s.group(1), rows[n][1]))
    for n in rows:
        if n not in files:
            p.append('index row %s names no file' % n)
    if main:
        r = subprocess.run(['git', '-C', root, 'ls-tree', '--name-only', main, 'docs/adr/'],
                           capture_output=True, text=True)
        if r.returncode != 0:
            p.append('--main %s: ls-tree failed: %s' % (main, r.stderr.strip()))
        else:
            for line in r.stdout.split():
                b = os.path.basename(line)
                m = FILE_RE.match(b)
                if m and m.group(1) != '0000' and m.group(1) in files and b not in files[m.group(1)]:
                    p.append('ADR number %s is already %s on %s; renumber yours' % (m.group(1), b, main))
    return p, sum(len(v) for v in files.values())


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--root', default=REPO)
    ap.add_argument('--main')
    a = ap.parse_args(argv)
    probs, n = check(os.path.abspath(a.root), a.main)
    for q in probs:
        print('adr_check: ' + q, file=sys.stderr)
    if probs:
        return 1
    print('adr_check: %d ADRs, numbers unique, index complete' % n)
    return 0


if __name__ == '__main__':
    sys.exit(main())
