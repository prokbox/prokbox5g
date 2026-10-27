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

"""P19 screen truth gate (docs/phase19-brief.md section 3.6).

Every FBADDR( / lcd_panel() site in the shell (apps/ipodui) reads or
writes the framebuffer as "what is on screen".  While the BCM2722 owns a
region (the drift's panel), that is only true after ui_fb_settle().  So
each site must, in the same function, call ui_fb_settle() (or preview.c's
drift_settle()), or carry a note on its line or the line above:
    /* not ownable: <why> */
A file whose screen can never show an owned region may say so once:
    /* not ownable (file): <why> */
A new site without either fails.  Exit 1 on any failure.
"""
import os, re, sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
SITE = re.compile(r'\bFBADDR\s*\(|\blcd_panel\s*\(\s*\)')
FUNC = re.compile(r'^[A-Za-z_][\w \*]*\b(\w+)\s*\([^;]*$')
NOTE = re.compile(r'not ownable:\s*\S')
FILE_NOTE = re.compile(r'not ownable \(file\):\s*\S')
SETTLE = re.compile(r'\bui_fb_settle\s*\(|\bdrift_settle\s*\(')


def functions(lines):
    """(name, first, last) of each top-level function body"""
    out, i, n = [], 0, len(lines)
    while i < n:
        m = FUNC.match(lines[i])
        if m and not lines[i].rstrip().endswith(';'):
            j = i
            while j < n and '{' not in lines[j] and ';' not in lines[j]:
                j += 1
            if j < n and '{' in lines[j] and lines[j].lstrip().startswith(('{', m.group(0)[:1])) or (j < n and lines[j].strip() == '{'):
                depth, k = 0, j
                while k < n:
                    depth += lines[k].count('{') - lines[k].count('}')
                    if depth <= 0 and k >= j:
                        break
                    k += 1
                out.append((m.group(1), i, k))
                i = k + 1
                continue
        i += 1
    return out


def check(path):
    rel = os.path.relpath(path, ROOT)
    lines = open(path, encoding='utf-8', errors='replace').read().split('\n')
    text = '\n'.join(lines)
    file_ok = FILE_NOTE.search(text)
    fns = functions(lines)
    bad = []
    nsites = 0
    for no, l in enumerate(lines):
        code = l.split('//')[0]
        if not SITE.search(code) or code.lstrip().startswith(('*', '/*', '#define')):
            continue
        nsites += 1
        if file_ok or NOTE.search(l) or (no and NOTE.search(lines[no - 1])):
            continue
        fn = [f for f in fns if f[1] <= no <= f[2]]
        if fn and any(SETTLE.search(x) for x in lines[fn[0][1]:fn[0][2] + 1]):
            continue
        bad.append('%s:%d: %s() reads/writes the framebuffer with no '
                   'ui_fb_settle() and no "not ownable:" note'
                   % (rel, no + 1, fn[0][0] if fn else '?'))
    return nsites, bad


def main():
    base = os.path.join(ROOT, 'apps', 'ipodui')
    total, bad = 0, []
    for d, _, fs in os.walk(base):
        for f in sorted(fs):
            if f.endswith('.c'):
                n, b = check(os.path.join(d, f))
                total += n
                bad += b
    for b in bad:
        print('FAIL ' + b)
    print('%s screen truth: %d framebuffer sites, %d without a settle or '
          'note' % ('ok  ' if not bad else 'FAIL', total, len(bad)))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
