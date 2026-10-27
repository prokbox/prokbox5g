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

"""Legal compliance audit for ProkBox5g (spec: docs/legal.md section 3).

usage: scripts/legal_audit.py [--report] [options]

Prints LEGAL PASSED and exits 0, or prints the failure list and exits 1.
--report lists every finding (failures and warnings) and always exits 0,
for a first run on a tree that has not been triaged yet.

Checks (numbers match docs/legal.md section 3, "Audit n"):
  1 upstream files keep their licence/copyright header lines
  2 deleted upstream files that look like licence/notice files
  3 new source files carry a GPL (or other valid licence) header
  4 modified upstream files carry "Modified for ProkBox5g" (--require-modnotice)
  5 licence files present (and in a release --zip)
  6 Apple marks: added paths (report-only), --asset-names (strict),
    english.lang ipodui: strings (strict, allow-list)
  7 image/binary provenance against docs/legal.md section 5
  8 secret scanner, and --apple-fw byte-window search

Options:
  --base REV            upstream base (default: git merge-base HEAD upstream/master,
                        the same base scripts/upstream_changes.sh uses)
  --root DIR            repository root (default: this checkout)
  --tree DIR            audit an exported tree (e.g. release_tree.py output)
                        instead of the git working tree
  --allowlist FILE      reviewed exceptions (default scripts/legal_allowlist.txt)
  --require-modnotice   check 4 becomes a failure (default: a warning count)
  --zip PATH            check a release zip contains the licence texts
  --asset-names N...    strict trademark check on release asset names
  --apple-fw DIR        search added binaries for 4 KB windows of Apple firmware
  --no-secrets          skip check 8's scanner

Allow-list lines:  kind|glob-or-id|reason   (reason mandatory)
  kinds: deleted (check 2), header (check 3), image (check 7),
         lang (check 6 LANG ids), name (check 6 asset names)
Limits: renames are not followed (a renamed file is one deletion plus one
addition); only the leading comment block of a file is compared.
"""
import argparse, collections, fnmatch, hashlib, os, re, shutil, subprocess, sys, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC_EXT = ('.c', '.h', '.S', '.py', '.sh', '.pl', '.make')
SRC_NAMES = ('Makefile',)
ASSET_EXT = ('.png', '.jpg', '.jpeg', '.bmp', '.gif', '.mp4', '.bin', '.fnt', '.otf', '.ttf')
BIN_EXT = ASSET_EXT + ('.ipod', '.elf', '.o', '.a', '.zip', '.gz', '.mp3', '.flac', '.ogg', '.m4a', '.wav')
KEYWORDS = re.compile(r'copyright|\(c\)|©|licen[cs]e|\bgpl\b|general public|warranty|'
                      r'permission|redistribut|free software|all rights|spdx|public domain|'
                      r'\bbsd\b|\bmit\b|apache', re.I)
MODNOTICE = re.compile(r'Modified for ProkBox5g')
LICENCE_NAME = re.compile(r'(^|/)(COPYING[^/]*|LICEN[CS]E[^/]*|CREDITS[^/]*|NOTICE[^/]*|COPYRIGHT[^/]*|'
                          r'AUTHORS[^/]*|[^/]*\.licen[cs]e|LICENSES?)$', re.I)
LICENCE_TEXT = re.compile(r'GNU (LESSER |LIBRARY )?GENERAL PUBLIC LICENSE|Permission is hereby granted|'
                          r'Redistribution and use in source and binary|Apache License|'
                          r'Mozilla Public License|SIL OPEN FONT LICENSE|Creative Commons|'
                          r'GUST Font License', re.I)
OTHER_LICENCE = re.compile(r'General Public License|\bGPL\b|\bLGPL\b|SPDX-License-Identifier|Permission is hereby '
                           r'granted|Redistribution and use|Apache License|public domain|\bBSD\b|\bMIT License|'
                           r'Mozilla Public|Creative Commons|zlib license|Licensed under', re.I)
PROKBOX_MARK = 'Custom firmware for the iPod Video, 5th'
MARKS = re.compile(r'ipod|apple|itunes|airdrop|cover ?flow|click ?wheel', re.I)
LANG_MARKS = re.compile(r'\b(iPod|Apple|iTunes|AirDrop|Cover Flow|Click Wheel|Genius)\b')
REQUIRED = ['docs/COPYING', 'docs/CREDITS', 'docs/LICENSES', 'apps/ipodui/fonts/GUST-FONT-LICENSE.txt']

findings = []   # (check, severity, path, message)


def add(check, sev, path, msg):
    findings.append((check, sev, path, msg))


def git(root, *args, text=True, check=True):
    r = subprocess.run(['git', '-C', root] + list(args), capture_output=True, text=text)
    if check and r.returncode:
        sys.exit('legal_audit: git %s failed: %s' % (' '.join(args), r.stderr.strip()))
    return r.stdout


class BaseReader:
    """Batched `git cat-file` reader for blobs at the base revision."""
    def __init__(self, root, base):
        self.p = subprocess.Popen(['git', '-C', root, 'cat-file', '--batch'], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE)
        self.base = base

    def get(self, path):
        self.p.stdin.write(('%s:%s\n' % (self.base, path)).encode())
        self.p.stdin.flush()
        hdr = self.p.stdout.readline().split()
        if len(hdr) < 3:
            return None
        n = int(hdr[2])
        data = self.p.stdout.read(n)
        self.p.stdout.read(1)
        return data

    def close(self):
        self.p.stdin.close(); self.p.wait()


def decode(b):
    return b.decode('utf-8', 'replace') if b is not None else ''


def leading_block(text):
    """Lines of the leading comment block (raw), '' if none."""
    lines = text.lstrip('﻿').split('\n')
    i = 0
    if lines and lines[0].startswith('#!'):
        i = 1
    while i < len(lines) and not lines[i].strip():
        i += 1
    out = []
    if i >= len(lines):
        return out
    first = lines[i].lstrip()
    if first.startswith('/*'):
        while i < len(lines):
            out.append(lines[i])
            if '*/' in lines[i]:
                break
            i += 1
    elif first.startswith('"""') or first.startswith("'''"):
        q = first[:3]
        out.append(lines[i])
        if first.count(q) < 2:
            i += 1
            while i < len(lines):
                out.append(lines[i])
                if q in lines[i]:
                    break
                i += 1
    else:
        pref = '//' if first.startswith('//') else '#' if first.startswith('#') else None
        if pref:
            while i < len(lines) and lines[i].lstrip().startswith(pref):
                out.append(lines[i]); i += 1
    return out


def norm(line):
    s = re.sub(r'^\s*(/\*+|\*+/?|//+|#+|"""|\'\'\')', '', line)
    s = re.sub(r'(\*+/|\*+|""")\s*$', '', s)
    return re.sub(r'\s+', ' ', s).strip()


def legal_lines(text):
    out = []
    for l in leading_block(text):
        n = norm(l)
        if n and KEYWORDS.search(n):
            out.append(n)
    return out


def load_allowlist(path):
    al = collections.defaultdict(list)
    if not os.path.exists(path):
        return al
    for no, line in enumerate(open(path, encoding='utf-8'), 1):
        line = line.rstrip('\n')
        if not line.strip() or line.lstrip().startswith('#'):
            continue
        parts = [p.strip() for p in line.split('|', 2)]
        if len(parts) < 3 or not parts[2]:
            sys.exit('legal_audit: %s:%d: allow-list line needs kind|pattern|reason' % (path, no))
        al[parts[0]].append(parts[1])
    return al


def allowed(al, kind, key):
    return any(fnmatch.fnmatch(key, p) for p in al.get(kind, []))


def is_src(path):
    b = os.path.basename(path)
    return path.endswith(SRC_EXT) or b in SRC_NAMES


def parse_provenance(root):
    pats = []
    try:
        txt = open(os.path.join(root, 'docs/legal.md'), encoding='utf-8').read()
    except OSError:
        add(7, 'fail', 'docs/legal.md', 'missing: cannot read the provenance table')
        return pats
    m = re.search(r'^## 5\..*?(?=^## )', txt, re.S | re.M)
    if not m:
        add(7, 'fail', 'docs/legal.md', 'section 5 (asset provenance) not found')
        return pats
    for row in m.group(0).split('\n'):
        if not row.startswith('|') or row.startswith('| Path') or row.startswith('|---'):
            continue
        cell = row.split('|')[1]
        pats += re.findall(r'`([^`]+)`', cell)
    if not pats:
        add(7, 'fail', 'docs/legal.md', 'section 5 table has no path globs')
    return pats


def glob_to_re(g):
    if g.endswith('/'):
        g += '**'
    out = ''
    i = 0
    while i < len(g):
        if g.startswith('**/', i):
            out += '(?:.*/)?'; i += 3
        elif g.startswith('**', i):
            out += '.*'; i += 2
        elif g[i] == '*':
            out += '[^/]*'; i += 1
        elif g[i] == '?':
            out += '[^/]'; i += 1
        else:
            out += re.escape(g[i]); i += 1
    return re.compile('^' + out + '$')


def lang_ipodui_strings(root):
    p = os.path.join(root, 'apps/lang/english.lang')
    if not os.path.exists(p):
        return []
    txt = open(p, encoding='utf-8', errors='replace').read()
    res = []
    for ph in txt.split('<phrase>')[1:]:
        m = re.search(r'^\s*id:\s*(\S+)', ph, re.M)
        if not m:
            continue
        for s in re.findall(r'^\s*ipodui:\s*"(.*)"\s*$', ph, re.M):
            if LANG_MARKS.search(s) and (m.group(1), s) not in res:
                res.append((m.group(1), s))
    return res


def run_audit(a):
    root = os.path.abspath(a.root)
    tree = os.path.abspath(a.tree) if a.tree else root
    base = a.base or git(root, 'merge-base', 'HEAD', 'upstream/master').strip()
    al = load_allowlist(a.allowlist or os.path.join(root, 'scripts/legal_allowlist.txt'))
    base_files = set(git(root, 'ls-tree', '-r', '--name-only', base).split('\n')) - {''}
    if a.tree:
        cur = set()
        for dp, dn, fn in os.walk(tree):
            dn[:] = [d for d in dn if d not in ('.git', 'build', 'build-verify')]
            for f in fn:
                cur.add(os.path.relpath(os.path.join(dp, f), tree))
    else:
        cur = {f for f in git(root, 'ls-files').split('\n') if f and os.path.exists(os.path.join(tree, f))}
    added = sorted(cur - base_files)
    deleted = sorted(base_files - cur)
    if a.tree:
        cand = sorted(cur & base_files)
    else:
        ch = set(git(root, 'diff', '--name-only', '--no-renames', base).split('\n'))
        cand = sorted(ch & cur & base_files)
    print('# base=%s  tree=%s  upstream files kept=%d  deleted=%d  added=%d  compared=%d'
          % (base[:10], tree, len(cur & base_files), len(deleted), len(added), len(cand)))
    rd = BaseReader(root, base)

    def read(rel):
        try:
            return open(os.path.join(tree, rel), 'rb').read()
        except OSError:
            return b''

    # 1 and 4
    nomod = 0
    for rel in cand:
        old = rd.get(rel)
        new = read(rel)
        if old == new or b'\0' in old[:4096]:
            continue
        ot, nt = decode(old), decode(new)
        ol, nl = legal_lines(ot), legal_lines(nt)
        has_mod = bool(MODNOTICE.search('\n'.join(leading_block(nt))))
        co, cn = collections.Counter(ol), collections.Counter(nl)
        for line, n in (co - cn).items():
            add(1, 'fail', rel, 'header line removed/changed: %r' % line[:100])
        extra = [l for l in (cn - co).elements() if not MODNOTICE.search(l)]
        for line in extra:
            add(1, 'warn', rel, 'header line added: %r' % line[:100])
        if ol and not has_mod:
            nomod += 1
            if a.require_modnotice:
                add(4, 'fail', rel, 'modified upstream file lacks "Modified for ProkBox5g"')
        if MODNOTICE.search(nt) and sum(1 for l in nt.split('\n')[:80] if MODNOTICE.search(l)) > 1:
            add(4, 'warn', rel, 'more than one "Modified for ProkBox5g" line')
    if nomod and not a.require_modnotice:
        add(4, 'warn', '(%d files)' % nomod, 'modified upstream files lack "Modified for ProkBox5g" '
            '(not enforced; use --require-modnotice)')
    rd.close()

    # 2 deleted licence-like files
    rd = BaseReader(root, base)
    for rel in deleted:
        if allowed(al, 'deleted', rel):
            continue
        why = None
        if LICENCE_NAME.search(rel) or 'docs/LICENSES' in rel or '/licenses/' in rel.lower():
            why = 'name looks like a licence/notice file'
        elif not rel.endswith(SRC_EXT + ('.cpp', '.s', '.lds', '.make')) and not rel.endswith(BIN_EXT):
            d = rd.get(rel)
            if d is not None and len(d) < 400000 and b'\0' not in d[:4096] and LICENCE_TEXT.search(decode(d)):
                why = 'content contains licence text'
        if why:
            add(2, 'fail', rel, 'deleted upstream file: ' + why + ' (review, then allow-list)')
    rd.close()

    # 3 headers on new source files
    for rel in added:
        if not is_src(rel) or allowed(al, 'header', rel):
            continue
        head = decode(read(rel)[:6000])
        if PROKBOX_MARK in head or OTHER_LICENCE.search('\n'.join(head.split('\n')[:80])):
            continue
        add(3, 'fail', rel, 'new source file has no GPL/licence header')

    # 5 licence files
    need = list(REQUIRED)
    for f in base_files:
        if f.startswith('lib/') and LICENCE_NAME.search(f):
            need.append(f)
    for f in sorted(set(need)):
        if f not in cur:
            add(5, 'fail', f, 'required licence file missing')
    if a.zip:
        try:
            names = [os.path.basename(n).lower() for n in zipfile.ZipFile(a.zip).namelist()]
            for want in ('copying', 'credits', 'licenses', 'gust-font-license.txt'):
                if not any(n == want or n.startswith(want) for n in names):
                    add(5, 'fail', a.zip, 'release zip lacks a %s file' % want.upper())
        except (OSError, zipfile.BadZipFile) as e:
            add(5, 'fail', a.zip, 'cannot read zip: %s' % e)

    # 6 trademarks
    for rel in added:
        if MARKS.search(rel):
            add(6, 'warn', rel, 'added path contains an Apple mark (report-only)')
    for n in a.asset_names or []:
        if MARKS.search(n) and not allowed(al, 'name', n):
            add(6, 'fail', n, 'release asset name contains an Apple mark')
    for lid, s in lang_ipodui_strings(root if not a.tree else tree):
        if re.match(r'LANG_IPODUI_(LEGAL|NOTICE)', lid) or allowed(al, 'lang', lid):
            continue
        add(6, 'fail', lid, 'english.lang ipodui string has an Apple mark: %r' % s[:80])

    # 7 provenance
    pats = [glob_to_re(g) for g in parse_provenance(tree if os.path.exists(os.path.join(tree, 'docs/legal.md')) else root)]
    for rel in added:
        if rel.lower().endswith(ASSET_EXT) and not allowed(al, 'image', rel):
            if not any(p.match(rel) for p in pats):
                add(7, 'fail', rel, 'added binary/image not in docs/legal.md section 5 table')
    for rel in cur:
        if rel.startswith('docs/reference/') and not (rel.endswith('.md') or rel.endswith('.gitignore')):
            add(7, 'fail', rel, 'non-markdown file under docs/reference/ (Apple reference material?)')

    # 8 secrets and Apple firmware bytes
    if not a.no_secrets:
        run_scanner(tree)
    if a.apple_fw:
        apple_scan(a.apple_fw, tree, added)


def run_scanner(tree):
    for tool, cmd in (('gitleaks', ['gitleaks', 'detect', '--no-git', '-s', tree, '--no-banner', '-q']),
                      ('trufflehog', ['trufflehog', 'filesystem', tree, '--fail', '--no-update'])):
        if shutil.which(tool):
            r = subprocess.run(cmd, capture_output=True, text=True)
            if r.returncode:
                add(8, 'fail', tool, 'reported secrets: ' + (r.stdout + r.stderr).strip()[:300])
            else:
                print('# %s: clean' % tool)
            return
    print('# secret scan skipped: neither gitleaks nor trufflehog is on PATH')


def windows(data, step, off0=0):
    for o in range(off0, len(data) - 4095, step):
        w = data[o:o + 4096]
        if len(set(w)) > 16:
            yield o, hashlib.sha1(w).digest()


def apple_scan(fwdir, tree, added):
    idx = {}
    for dp, _, fn in os.walk(fwdir):
        for f in fn:
            p = os.path.join(dp, f)
            for o, h in windows(open(p, 'rb').read(), 4096):
                idx[h] = (p, o)
    print('# apple-fw: indexed %d windows from %s' % (len(idx), fwdir))
    for rel in added:
        if not (rel.lower().endswith(BIN_EXT) or True):
            continue
        p = os.path.join(tree, rel)
        try:
            d = open(p, 'rb').read()
        except OSError:
            continue
        if b'\0' not in d[:8192] and not rel.lower().endswith(BIN_EXT):
            continue
        for o, h in windows(d, 512):
            if h in idx:
                add(8, 'fail', rel, 'offset %d matches Apple firmware %s@%d' % (o, idx[h][0], idx[h][1]))
                break


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--report', action='store_true')
    ap.add_argument('--base'); ap.add_argument('--tree'); ap.add_argument('--allowlist')
    ap.add_argument('--root', default=os.path.dirname(HERE))
    ap.add_argument('--require-modnotice', action='store_true')
    ap.add_argument('--zip'); ap.add_argument('--apple-fw'); ap.add_argument('--no-secrets', action='store_true')
    ap.add_argument('--asset-names', nargs='+')
    a = ap.parse_args()
    run_audit(a)
    fails = [f for f in findings if f[1] == 'fail']
    if a.report:
        by = collections.OrderedDict()
        for c, s, p, m in sorted(findings, key=lambda f: (f[0], f[1] != 'fail', f[2])):
            by.setdefault((c, s), []).append((p, m))
        for (c, s), items in by.items():
            print('\n== check %d %s: %d' % (c, s, len(items)))
            for p, m in items:
                print('  %s: %s' % (p, m))
        print('\n== summary (check, severity: count)')
        for (c, s), items in by.items():
            print('  %d %-4s %d' % (c, s, len(items)))
        return 0
    warns = collections.Counter(c for c, s, p, m in findings if s == 'warn')
    for c, n in sorted(warns.items()):
        print('warning: check %d: %d finding(s), --report lists them' % (c, n))
    if fails:
        print('\nLEGAL FAILED: %d finding(s)' % len(fails))
        for c, s, p, m in fails:
            print('  check %d: %s: %s' % (c, p, m))
        return 1
    print('LEGAL PASSED')
    return 0


if __name__ == '__main__':
    sys.exit(main())
