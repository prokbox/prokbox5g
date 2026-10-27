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

"""After a guided Bench session (apps/ipodui/devbench.c): read the copy of
the device's hidden system folder and print what came back, failures
first; if the session earned a bcmfw/verified.txt line, write a patch for
review (never applied here).

Usage (always isolated: the pulled files are untrusted data):

  python3 -I scripts/bench_ingest.py <copied folder> [--commit SHA]
          [--patch FILE] [--context N] [--mkdevtest auto|run|render|none]
          [--corrections FILE]

  <copied folder>  the copy of the device's system folder (bench/,
                   bcmtest*.log, ipodui.log*, bcmpass*.txt, power.csv*)
  --commit SHA     the build's commit: its devbench.c/devbench.h give the
                   step table and the verdict rules (default: this tree)
  --patch FILE     where the verified patch goes (default: a file under
                   the system temp directory, printed)
  --context N      log lines shown around a step (default 30)
  --corrections FILE
                   answers the owner corrected after the session, one a
                   line: '<results file|*> <step> <answer> <who>: <why>'
                   (# comments); shown beside the device's answer, never
                   rewriting it; a corrected good answer is no anomaly
  --mkdevtest      how the patch regenerates bcm-devtest-images.c: run
                   bcmfw/mkdevtest.py in a temporary worktree (run), re-render
                   the committed images with mkdevtest's own renderer
                   (render: no VideoCore toolchain needed), auto (run if the
                   toolchain is there, else render), none (no patch)

Sections, in order: failures and anomalies; each results file's session
table; the readlog rows (bench checks, port, J_VPU, floor, drift jobs,
battery-row counters, pass flags, power marks); the stale-band hints; the drift-still lines and
their verdict; the verified line and its patch.

The results file's format, its replay and the verdict rules are
devbench.h's; this file mirrors them (bench_parse, bench_replay,
bench_settle, bench_select, bench_picks_ok, bench_full_ok,
bench_opt_passed, bench_fmt_verified_in).  The step table is read from
devbench.c's steps[] source, never from the device.  bcmtest.log's bench
records are read by bcmfw/bench/readlog.py.

Nothing from the pulled folder is executed, evaluated or used as a path;
files are read as bytes with a size cap, symlinks are skipped, and every
printed line has its control characters escaped.  A truncated or corrupt
file (a restart mid-write) is reported, not fatal.

docs/research/bcm2722/bench-p19.md, "After the session: ingest".
"""
import csv
import io
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, 'bcmfw', 'bench'))
import readlog  # noqa: E402

MAX_BYTES = 64 << 20            # per file: a log past this is cut and said so
CONTEXT = 30

# ---- devbench.h's enums ------------------------------------------------------
RES = ('none', 'start', 'pass', 'fail', 'skip', 'done', 'hang')
NONE, START, PASS, FAIL, SKIP, DONE, HANG = range(7)
ANSWERS = 4
F = dict(BENCH_F_AUTO=0x01, BENCH_F_COP=0x02, BENCH_F_REBOOTS=0x04,
         BENCH_F_GO=0x08, BENCH_F_OPTIONAL=0x10, BENCH_F_OWN=0x20,
         BENCH_F_PICKED=0x40, BENCH_F_LEAVES=0x80)
MODE_FULL, MODE_REVERIFY, MODE_PORTCHECK = 0, 1, 2
MODE_NAMES = {'reverify': MODE_REVERIFY, 'portcheck': MODE_PORTCHECK}


def parse_mode(v):
    """bench_session_fields: mode= to a mode (none or unknown: full)"""
    return MODE_NAMES.get(v, MODE_FULL)


def mode_name(m):
    return {MODE_REVERIFY: 'reverify', MODE_PORTCHECK: 'portcheck'}.get(m, 'full')
R_RUN, R_PRE, R_IFPRE, R_NEED, R_OPT, R_ALSO, R_IFBOOT = range(7)
ROLE = dict(BENCH_R_RUN=R_RUN, BENCH_R_PRE=R_PRE, BENCH_R_IFPRE=R_IFPRE,
            BENCH_R_NEED=R_NEED, BENCH_R_OPT=R_OPT, BENCH_R_ALSO=R_ALSO,
            BENCH_R_IFBOOT=R_IFBOOT)

# The bench checks whose silicon verdict this session can bring back
CHECKS = ('ASHF', 'ASHG', 'X15M', 'X15S', 'X15Z', 'MHDU', 'VCLP', 'R48E',
          'D16R', 'REPA', 'PUTV', 'BARV', 'ASHX', 'ACSA', 'GTHR',
          # HWLD: the halfword load forms (quirks.md #16, #33)
          'HL16', 'HS16', 'H28O', 'H29O', 'H48H', 'H48S', 'HSP0', 'HSP1')
# Bench row ALGN (stage 4, after the bench; bcmfw/bench/algnlog.py): the
# BCM's own alignment checks and the host port's, reported when present
ALGN_CHECKS = tuple(readlog.algnlog.CHECKS) + tuple(readlog.algnlog.PORT)
# Steps that show a moving or scrolling picture (the stale-band hints)
PICTURE_STEPS = ('s05', 's6', 's7', 'b13', 'b5', 'b3', 'b3ab', 'drill1',
                 'drill2', 'drill3', 'scroll')


def clean(s):
    """A line from the device, safe for a terminal: controls escaped."""
    return ''.join(c if (' ' <= c < '\x7f' or c > '\x9f') else '\\x%02x' % ord(c)
                   for c in s)


# ---- reading the pulled files --------------------------------------------------
class Pulled:
    """The copied folder: every file read once, as text, with its problems."""

    def __init__(self, top):
        self.top = top
        self.problems = []      # (file, what)
        self.cache = {}

    def path(self, rel):
        return os.path.join(self.top, rel)

    def exists(self, rel):
        try:
            st = os.lstat(self.path(rel))
        except OSError:
            return False
        return not os.path.islink(self.path(rel)) and os.path.isfile(self.path(rel)) \
            and st.st_size >= 0

    def listdir(self, rel=''):
        try:
            return sorted(os.listdir(self.path(rel)))
        except OSError:
            return []

    def lines(self, rel):
        """The file's lines (no newlines); [] if missing.  NULs, a cut last
        line and undecodable bytes are noted in self.problems."""
        if rel in self.cache:
            return self.cache[rel]
        p = self.path(rel)
        out = []
        if os.path.islink(p):
            self.problems.append((rel, 'a symbolic link: not read'))
        elif os.path.isfile(p):
            try:
                with open(p, 'rb') as f:
                    data = f.read(MAX_BYTES + 1)
            except OSError as e:
                self.problems.append((rel, 'unreadable: %s' % e.strerror))
                data = b''
            if len(data) > MAX_BYTES:
                self.problems.append((rel, 'over %d MB: read the first part only'
                                      % (MAX_BYTES >> 20)))
                data = data[:MAX_BYTES]
            nul = data.count(b'\0')
            if nul:
                self.problems.append((rel, '%d NUL bytes (a write cut by a restart?): '
                                      'dropped' % nul))
                data = data.replace(b'\0', b'')
            text = data.decode('utf-8', 'replace')
            if '�' in text:
                self.problems.append((rel, 'bytes that are not text: replaced'))
            if text and not text.endswith('\n'):
                self.problems.append((rel, 'the last line has no end (cut mid-write): '
                                      '"%s"' % clean(text.rsplit('\n', 1)[-1][-60:])))
            out = text.replace('\r', '').split('\n')
            if out and out[-1] == '':
                out.pop()
        self.cache[rel] = out
        return out

    def generations(self, base, ext, style):
        """A rotated log's files, oldest first.  style 'mid': name.N.ext
        (bcmtest.1.log); 'end': name.ext.N (ipodui.log.1)."""
        names = set(self.listdir())
        gens = []
        for n in range(9, 0, -1):
            name = ('%s.%d%s' % (base, n, ext) if style == 'mid'
                    else '%s%s.%d' % (base, ext, n))
            if name in names:
                gens.append(name)
        if base + ext in names:
            gens.append(base + ext)
        return gens


class Line:
    __slots__ = ('src', 'n', 'text', 'boot')

    def __init__(self, src, n, text, boot=0):
        self.src, self.n, self.text, self.boot = src, n, text, boot

    def show(self):
        return '%s:%d: %s' % (self.src, self.n, clean(self.text))


def log_lines(pulled, gens):
    out = []
    for b, name in enumerate(gens):
        for i, t in enumerate(pulled.lines(name)):
            out.append(Line(name, i + 1, t, b))
    return out


# ---- the step table, from devbench.c's source ----------------------------------
def strip_comments(s):
    out, i, n = [], 0, len(s)
    while i < n:
        if s.startswith('/*', i):
            j = s.find('*/', i + 2)
            i = n if j < 0 else j + 2
            out.append(' ')
        elif s.startswith('//', i):
            j = s.find('\n', i)
            i = n if j < 0 else j
        elif s[i] == '"':
            j = i + 1
            while j < n and s[j] != '"':
                j += 2 if s[j] == '\\' else 1
            out.append(s[i:j + 1])
            i = j + 1
        else:
            out.append(s[i])
            i += 1
    return ''.join(out)


def split_top(s, sep=','):
    parts, depth, cur, i = [], 0, [], 0
    while i < len(s):
        c = s[i]
        if c == '"':
            j = i + 1
            while j < len(s) and s[j] != '"':
                j += 2 if s[j] == '\\' else 1
            cur.append(s[i:j + 1])
            i = j + 1
            continue
        if c in '{(':
            depth += 1
        elif c in '})':
            depth -= 1
        if c == sep and depth == 0:
            parts.append(''.join(cur).strip())
            cur = []
        else:
            cur.append(c)
        i += 1
    if ''.join(cur).strip():
        parts.append(''.join(cur).strip())
    return parts


def cstr(expr):
    """A C expression of string literals (adjacent ones joined) or NULL."""
    expr = expr.strip()
    if expr in ('NULL', '0', ''):
        return None
    lits = re.findall(r'"((?:[^"\\]|\\.)*)"', expr)
    return ''.join(lits) if lits else None


def parse_flags(expr, macros):
    v = 0
    for t in re.split(r'[|()\s]+', expr):
        if not t:
            continue
        if t in macros:
            v |= macros[t]
        elif re.fullmatch(r'0x[0-9a-fA-F]+|\d+', t):
            v |= int(t, 0)
        else:
            raise ValueError('flag %s' % t)
    return v


def parse_steps(c_src, h_src):
    """devbench.c's steps[]: a list of dicts (id, title, fn, ask, answers,
    flags, cond: the #ifdef it sits under or None)."""
    macros = dict(F)
    for m in re.finditer(r'^#define\s+(BENCH_F_\w+)\s+(0x[0-9a-fA-F]+|\d+)', h_src, re.M):
        macros[m.group(1)] = int(m.group(2), 0)
    m = re.search(r'static const struct bench_step steps\[\]\s*=\s*\{', c_src)
    if not m:
        raise ValueError('no steps[] table in devbench.c')
    head = c_src[:m.start()]
    for m2 in re.finditer(r'^#define\s+(\w+)\s+(.+)$', head, re.M):
        try:
            macros[m2.group(1)] = parse_flags(strip_comments(m2.group(2)), macros)
        except ValueError:
            pass
    body = c_src[m.end():]
    end = body.find('\n};')
    body = strip_comments(body[:end])
    steps, cond, i, depth, start = [], None, 0, 0, 0
    lines = body.split('\n')
    text = []
    for ln in lines:
        s = ln.strip()
        if s.startswith('#if'):
            cond = s.split(None, 1)[1] if ' ' in s else s
            text.append('\x01%s\x01' % cond)
            continue
        if s.startswith('#endif'):
            text.append('\x01\x01')
            continue
        if s.startswith('#'):
            continue
        text.append(ln)
    body = '\n'.join(text)
    cond = None
    while i < len(body):
        c = body[i]
        if c == '\x01':
            j = body.index('\x01', i + 1)
            cond = body[i + 1:j] or None
            i = j + 1
            continue
        if c == '"':
            j = i + 1
            while body[j] != '"':
                j += 2 if body[j] == '\\' else 1
            i = j + 1
            continue
        if c == '{':
            if depth == 0:
                start = i
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                f = split_top(body[start + 1:i])
                if len(f) < 9:
                    raise ValueError('a short entry in steps[]: %s' % f[:1])
                ans = [cstr(a) for a in split_top(f[6].strip()[1:-1])]
                ans = [a for a in ans if a is not None][:ANSWERS]
                steps.append(dict(id=cstr(f[0]), title=cstr(f[1]),
                                  fn=f[4].strip() not in ('NULL', '0'),
                                  ask=cstr(f[5]), answers=ans,
                                  flags=parse_flags(f[7], macros), cond=cond))
        i += 1
    return steps


PICK = re.compile(r'\{\s*"(\w+)",\s*(BENCH_R_\w+(?:\s*\|\s*BENCH_R_HELD)?)\s*(?:,\s*\{([^}]*)\}\s*)?\}')


def parse_pick_table(h_src, name):
    """bench_reverify_picks and the like: [(id, role)].  Since sitting 4c
    an entry may name the steps it needs ({ "vfeed", BENCH_R_ALSO,
    { "s6" } }): parse_pick_needs"""
    m = re.search(r'%s\[\]\s*=\s*\{(.*?)\};' % name, h_src, re.S)
    picks = []
    if m:
        for pid, role, _ in PICK.findall(strip_comments(m.group(1))):
            picks.append((pid, ROLE[role.split('|')[0].strip()],
                          'BENCH_R_HELD' in role))
    return picks


def parse_pick_needs(h_src, name):
    """{id: [the ids it needs]} (devbench.h's bench_pick.need[])"""
    m = re.search(r'%s\[\]\s*=\s*\{(.*?)\};' % name, h_src, re.S)
    out = {}
    if m:
        for pid, _, needs in PICK.findall(strip_comments(m.group(1))):
            out[pid] = re.findall(r'"(\w+)"', needs or '')
    return out


def parse_gates(h_src):
    """devbench.h's bench_gates[]: {id: (ends the session, the sentence)}"""
    m = re.search(r'bench_gates\[\]\s*=\s*\{(.*?)\n\};', h_src, re.S)
    out = {}
    if m:
        for ent in re.findall(r'\{\s*"(\w+)",\s*([^,]+),((?:\s*"(?:[^"\\]|\\.)*")+)\s*\}',
                              strip_comments(m.group(1))):
            out[ent[0]] = ('BENCH_GATE_END' in ent[1],
                           ''.join(re.findall(r'"((?:[^"\\]|\\.)*)"', ent[2])))
    return out


# An auto-skip's note (devbench.h bench_fmt_autoskip)
AUTOSKIP = re.compile(r'^auto-skip: needs (\w+) \((\w+)\)')


def parse_picks(h_src):
    picks = parse_pick_table(h_src, 'bench_reverify_picks')
    m = re.search(r'bench_full_again\[\]\s*=\s*\{(.*?)\};', h_src, re.S)
    again = re.findall(r'\{\s*"(\w+)",\s*"(\w+)"\s*\}', m.group(1)) if m else []
    full_earns = 'bench_full_ok' in h_src
    return picks, again, full_earns


class Rules:
    """The build's devbench sources: the step table and the verdict rules."""

    def __init__(self, commit=None, repo=ROOT):
        self.where = commit or 'this tree'
        if commit:
            if not re.fullmatch(r'[0-9A-Za-z._/-]{4,64}', commit):
                raise ValueError('a commit is a hash or a ref')
            c_src = git_show(repo, commit, 'apps/ipodui/devbench.c')
            h_src = git_show(repo, commit, 'apps/ipodui/devbench.h')
        else:
            c_src = open(os.path.join(repo, 'apps/ipodui/devbench.c')).read()
            h_src = open(os.path.join(repo, 'apps/ipodui/devbench.h')).read()
        self.all_steps = parse_steps(c_src, h_src)
        self.picks, self.again, self.full_earns = parse_picks(h_src)
        # the port write check (bench_portcheck_picks; none in older builds)
        self.port_picks = parse_pick_table(h_src, 'bench_portcheck_picks')
        self.needs = parse_pick_needs(h_src, 'bench_reverify_picks')
        self.gates = parse_gates(h_src)
        self.steps = self.all_steps

    def picks_for(self, mode):
        return self.port_picks if mode == MODE_PORTCHECK else self.picks

    def choose(self, ids, n_session, mode):
        """The table variant this file was written by: the #ifdef'd steps
        in or out, by the session's steps= count and the ids it names."""
        best = None
        for conds in (None, {None}):
            t = [s for s in self.all_steps if conds is None or s['cond'] in conds]
            names = {s['id'] for s in t}
            n = len(select(mode, t, self.picks_for(mode), ids))
            score = (not (ids - names), n == n_session)
            if best is None or score > best[0]:
                best = (score, t)
        self.steps = best[1]
        return best[0]


def git_show(repo, commit, path):
    r = subprocess.run(['git', '-C', repo, 'show', '%s:%s' % (commit, path)],
                       capture_output=True, text=True)
    if r.returncode:
        raise ValueError('git show %s:%s: %s' % (commit, path, r.stderr.strip()))
    return r.stdout


# ---- devbench.h's file format and replay ---------------------------------------
def field(line, key):
    """bench_field: key=value, the key at the start or after a space."""
    p = 0
    while True:
        p = line.find(key, p)
        if p < 0:
            return None
        if (p == 0 or line[p - 1] == ' ') and line[p + len(key):p + len(key) + 1] == '=':
            break
        p += len(key)
    v = line[p + len(key) + 1:]
    return re.split(r'[ \n\r]', v, maxsplit=1)[0]


def parse_line(line):
    """bench_parse, plus the note's text: (kind, id, res, answer, ms, note)"""
    if line.startswith('session '):
        return ('session', None, NONE, None, 0, None)
    if line.startswith('end ') or line == 'end':
        return ('end', None, NONE, None, 0, None)
    if line.startswith('resume '):
        return ('resume', field(line, 'after'), NONE, None, 0, None)
    if line.startswith('retry '):       # bench_fmt_retry: run it again
        sid = field(line, 'step')
        return ('retry' if sid else 'other', sid, NONE, None, 0, None)
    if line.startswith('verified '):
        return ('verified', None, NONE, None, 0, line[9:])
    if line.startswith('token '):       # devbench.c: a verified.txt token line
        return ('token', None, NONE, None, 0, line[6:])
    if not line.startswith('step='):
        return ('other', None, NONE, None, 0, None)
    sid = field(line, 'step')
    if sid is None:
        return ('other', None, NONE, None, 0, None)
    sp = line.find(' ')
    rest = line[sp + 1:] if sp >= 0 else None
    if rest is not None and rest.startswith('log='):
        return ('log', sid, NONE, None, 0, rest[4:])
    if rest is None or not rest.startswith('result='):
        return ('other', sid, NONE, None, 0, None)
    v = field(rest, 'result')
    r = RES.index(v) if v in RES[1:] else NONE
    if r == START:
        return ('start', sid, START, None, 0, None)
    if r == NONE:
        return ('other', sid, NONE, None, 0, None)
    k = rest.find(' note=')
    head = rest if k < 0 else rest[:k]
    note = '' if k < 0 else rest[k + 6:]
    ans = field(head, 'answer') or '-'
    ms = field(head, 'ms')
    msv = int(re.match(r'\d*', ms).group() or 0) if ms is not None else 0
    return ('result', sid, r, ans, msv, note)


def token(s):
    """bench_token: spaces as '_', '=' and controls dropped"""
    if not s:
        s = '-'
    return ''.join('_' if c == ' ' else c for c in s if c == ' ' or (' ' < c < '\x7f' and c != '='))[:31]


def answer_index(step, tok):
    if not tok or tok == '-':
        return -1
    if tok == 'Skip':
        return ANSWERS
    for k, a in enumerate(step['answers']):
        if token(a) == tok:
            return k
    return -1


def find(t, sid):
    for k, s in enumerate(t):
        if s['id'] == sid:
            return k
    return -1


class State:
    def __init__(self):
        self.next = 0
        self.dangling = -1
        self.last_done = -1
        self.go_yes = False
        self.cop_stuck = False
        self.ended = False
        self.started = False
        self.passed = self.failed = self.skipped = 0
        self.mode = MODE_FULL
        self.merged = False
        self.kind = None
        self.image_bytes = 0
        self.image_fnv = 0
        self.date = ''
        self.res = {}

    def count(self, r):
        if r in (PASS, DONE):
            self.passed += 1
        elif r in (FAIL, HANG):
            self.failed += 1
        elif r == SKIP:
            self.skipped += 1

    def set(self, k, r):
        self.count(r)
        if 0 <= k < 64:
            self.res[k] = r

    def uncount(self, r):
        """bench_uncount: a retried step's earlier result"""
        if r in (PASS, DONE) and self.passed:
            self.passed -= 1
        elif r in (FAIL, HANG) and self.failed:
            self.failed -= 1
        elif r == SKIP and self.skipped:
            self.skipped -= 1


def session_fields(st, line):
    st.mode = parse_mode(field(line, 'mode'))
    d = (field(line, 'date') or '')[:11]
    st.date = d if re.fullmatch(r'\d{4}-\d{2}-\d{2}', d) else ''
    v = field(line, 'image')
    if v is not None:
        st.image_bytes = int(re.match(r'\d*', v).group() or 0)
    v = field(line, 'fnv')
    if v is not None:
        m = re.match(r'[0-9a-fA-F]*', v).group()[:8]
        st.image_fnv = int(m, 16) if m else 0
    st.kind = field(line, 'kind')
    st.merged = st.kind == 'merged'


def replay(st, t, line):
    """bench_replay"""
    kind, sid, r, ans, _, _ = parse_line(line)
    if kind == 'session':
        st.started = True
        session_fields(st, line)
    elif kind == 'end':
        st.ended = True
    elif kind == 'start':
        k = find(t, sid)
        if k >= 0:
            st.dangling = k
    elif kind == 'retry':
        k = find(t, sid)
        if 0 <= k < 64 and st.res.get(k, NONE) != NONE:
            st.uncount(st.res[k])
            st.res[k] = NONE
    elif kind == 'result':
        k = find(t, sid)
        st.set(k, r)
        if k < 0:
            return
        if st.dangling == k:
            st.dangling = -1
        st.last_done = k
        st.next = max(st.next, k + 1)
        if t[k]['flags'] & F['BENCH_F_GO']:
            st.go_yes = bool(ans) and ans != '-' and answer_index(t[k], ans) == 1
        if r == HANG and t[k]['flags'] & F['BENCH_F_COP']:
            st.cop_stuck = True


def settle(st, t):
    """bench_settle, for the report: what a resume would make of a dangling
    step (START: finished at the next press; HANG: recorded hung)"""
    k = st.dangling
    if k < 0:
        return NONE
    if not t[k]['fn'] or t[k]['flags'] & (F['BENCH_F_REBOOTS'] |
                                          F['BENCH_F_LEAVES']):
        return START
    return HANG


def select(mode, t, picks, seen=None):
    """bench_select: [(k, role)]; seen: the step ids the file has results
    for (a held pick counts only when it ran)"""
    run = []
    if mode in (MODE_REVERIFY, MODE_PORTCHECK):
        for pid, role, *held in picks:
            # held for a later sitting (devbench.h BENCH_R_HELD): left out,
            # unless the file ran it (an older build's run, or the full
            # session's check, bench_full_ok)
            if held and held[0] and (seen is None or pid not in seen):
                continue
            k = find(t, pid)
            if 0 <= k < 64:
                run.append((k, role))
        return run
    return [(k, R_RUN) for k, s in enumerate(t[:64])
            if not s['flags'] & F['BENCH_F_PICKED']]


def picks_ok(run, st, mode, alt=None):
    """bench_picks_ok: (ok, the first check that failed it)"""
    pre = next((st.res.get(k, NONE) for k, role in run if role == R_PRE), NONE)
    if not st.merged:
        return False, 'the image is not the merged one (kind=%s)' % (st.kind or 'absent')
    if not run:
        return False, 'no checks in the run'
    for p, (k, role) in enumerate(run):
        r = st.res.get(k, NONE)
        if alt and alt[p] >= 0 and r != PASS and st.res.get(alt[p], NONE) == PASS:
            r = PASS
        if role == R_PRE:
            if mode == MODE_FULL:
                continue
            if r not in (PASS, SKIP, DONE):
                return False, (k, r)
        elif role == R_IFPRE:
            if r not in (PASS, DONE) and not (mode != MODE_FULL and r == SKIP and
                                              pre in (PASS, DONE)):
                return False, (k, r)
        elif role == R_IFBOOT:
            # DONE: the restart step (boot) has no verdict of its own
            if r not in (PASS, DONE) and not (mode != MODE_FULL and r == SKIP and pre == PASS):
                return False, (k, r)
        elif role == R_OPT:
            if r not in (PASS, DONE, SKIP):
                return False, (k, r)
        elif role == R_ALSO:
            continue                # s4c: the clock flag, never the line
        elif r != PASS:
            return False, (k, r)
    return True, None


def earned(rules, st):
    """(earns, why not, the run for bench_opt_passed)"""
    t = rules.steps
    seen = {t[k]['id'] for k, r in st.res.items() if 0 <= k < len(t) and r != NONE}
    picks = select(MODE_REVERIFY, t, rules.picks, seen)
    if st.mode == MODE_PORTCHECK:
        return False, 'the port write check earns no line', picks
    if st.mode == MODE_REVERIFY:
        ok, why = picks_ok(picks, st, MODE_REVERIFY)
        return ok, why, picks
    if not rules.full_earns:
        return False, 'this build\'s full session earns no line (the re-verify run does)', picks
    alt = []
    for k, _ in picks:
        a = -1
        for sid, again in rules.again:
            if t[k]['id'] == sid:
                a = find(t, again)
        alt.append(a)
    ok, why = picks_ok(picks, st, MODE_FULL, alt)
    return ok, why, picks


def opt_passed(run, st):
    """bench_opt_passed: how many optional steps passed, 0 if any did not"""
    n = 0
    for k, role in run:
        if role != R_OPT:
            continue
        if st.res.get(k, NONE) != PASS:
            return 0
        n += 1
    return n


def fmt_verified(st, build, drift):
    """bench_fmt_verified_in"""
    return ('bcmvideo_jobs %08x  stage 6 PASS (merged, %d bytes), B5 PASS, '
            'jobs self-test PASS, J_VPU 3 PASS%s on the owner\'s 5.5G: %s session %s, '
            'build %s' % (st.image_fnv, st.image_bytes,
                          ', drift B3 and drill 2 PASS' if drift >= 2 else
                          ', drift B3 PASS' if drift == 1 else '',
                          'full' if st.mode == MODE_FULL else 're-verify',
                          st.date or '-', build))


class Session:
    """One results file, replayed."""

    def __init__(self, pulled, rel, rules):
        self.rel = rel
        self.lines = pulled.lines(rel)
        self.rules = rules
        self.session_line = None
        self.build = None
        self.end_line = None
        self.verified = []
        self.tokens = []        # verified.txt token lines (clock150)
        self.records = []       # in file order: (kind, sid, r, ans, ms, note, n)
        self.logs = {}          # sid -> [text]
        self.resumes = []
        self.bad = []           # (line no, text): lines no kind matched
        self.corr = {}          # sid -> (answer, why): --corrections
        ids = set()
        n_session = -1
        for i, ln in enumerate(self.lines):
            kind, sid, r, ans, ms, note = parse_line(ln)
            if kind == 'session' and self.session_line is None:
                self.session_line = ln
                self.build = field(ln, 'build') or '?'
                v = field(ln, 'steps')
                n_session = int(v) if v and v.isdigit() else -1
                mode = parse_mode(field(ln, 'mode'))
            if sid:
                ids.add(sid)
            if kind == 'other':
                self.bad.append((i + 1, ln))
        self.fit = rules.choose(ids, n_session, mode if self.session_line else MODE_FULL)
        t = rules.steps
        self.steps = t
        st = State()
        for i, ln in enumerate(self.lines):
            replay(st, t, ln)
            kind, sid, r, ans, ms, note = parse_line(ln)
            if kind == 'log':
                self.logs.setdefault(sid, []).append(note)
            elif kind in ('result', 'start'):
                self.records.append((kind, sid, r, ans, ms, note, i + 1))
            elif kind == 'resume':
                self.resumes.append(sid)
            elif kind == 'verified':
                self.verified.append(note)
            elif kind == 'token':
                self.tokens.append(note)
            elif kind == 'end':
                self.end_line = ln
        self.st = st
        self.settled = settle(st, t)
        self.earns, self.why_not, self.picks = earned(rules, st)
        self.computed = None
        if self.earns:
            from_name = re.sub(r'[^A-Za-z0-9-]', '', self.build or '') or 'x'
            self.computed = fmt_verified(st, from_name, opt_passed(self.picks, st))

    def last(self):
        """Each step's last record: sid -> (r, ans, ms, note, line)"""
        out = {}
        for kind, sid, r, ans, ms, note, n in self.records:
            if kind == 'result':
                out[sid] = (r, ans, ms, note, n)
            elif sid not in out:
                out[sid] = (START, None, 0, None, n)
        return out

    def step(self, sid):
        k = find(self.steps, sid)
        return self.steps[k] if k >= 0 else None


# ---- the logs: what this session needs from them -------------------------------
AUTO = re.compile(r'^\s*\d+ auto: (\S+) (\S+)')


def step_window(bcm, sid, texts, context):
    """bcmtest.log's lines for a step: from the previous "auto:" line to
    this step's own (record() writes the step's lines, then its auto:
    line); else a window around the first of its results-file log lines."""
    idx = None
    for i in range(len(bcm) - 1, -1, -1):
        m = AUTO.match(bcm[i].text)
        if m and m.group(1) == sid:
            idx = i
            break
    if idx is not None:
        j = idx - 1
        while j >= 0 and not AUTO.match(bcm[j].text) and idx - j < 2000:
            j -= 1
        if idx - j > 1:
            return bcm[j + 1:idx + 1], 'between the auto: markers'
        h = max(1, context // 2)
        return bcm[max(0, idx - h):idx + h + 1], \
            'no lines of its own (a manual step): +-%d lines around its auto: marker' % h
    for t in texts or ():
        t = t.strip()
        if not t:
            continue
        for i, ln in enumerate(bcm):
            if ln.text.rstrip().endswith(t):
                return bcm[max(0, i - context):i + context + 1], '+-%d lines around its log line' % context
    return [], 'no marker or log line of it in bcmtest.log'


PORT_ANY = re.compile(r'video: port ')
VPU = re.compile(r'(jvpu: J_VPU \S+.*|lab5: J_VPU \S+.*|bcmjob vpu=\S+.*)$')
# batch 4 (devbench.c jvpu_batch4): the merged image's vector form checks
# and its first OPEN's load timings, information only (no verdict)
VPU2 = re.compile(r'jvpu: (forms\b.*|sdram byte .*|load timings .*)$')
# the battery rows' counters (devbench_power.c, litmiss.h): lost doorbells
# per B phase, missed frames per lit24ui phase
BATTERY = re.compile(r'^(slp [AB]\d+ (?:ms|rings|lat_p50)=\S+.*|slp B\d+: FAIL.*|'
                     r'lit24ui (?:A30|B24) drift_miss=\d+ scroll_miss=\d+ frames=\d+/\d+|'
                     r'lit24ui: (?:PASS|FAIL).*)$')
FLOOR = re.compile(r'floor dev audio=(\d+) bcm_res=(\d+)(?: free=(\d+))?')
DRIFTJ = re.compile(r'\bdrift (jobs mode=|still why=|job (fault|late)|upload|parity|settle|'
                    r'jobs resume)|\bdrift\b.*\bwhy=')
BCMJOB = re.compile(r'\bbcmjob (init |fault|up epoch=|vpu=|start vpu=|off|cpu why=|inject|selftest|'
                    r'upload_rect deferred|resync)')
BOOTP = re.compile(r'(boot path(?: toggle)?: .*|=== boot path.*|boot: BCM .* at entry|'
                   r'resident image: .*|no image .*|image .* refused|'
                   r'image .* is not this build\'s.*)$')
# the driver's per-start field line (bcmjob-core.c log_start_extras):
# vpu / vpu2 / form failure / load timings, once per BCM power-up
BCM_START = re.compile(r'\bbcmjob start (vpu=\S+.*)$')
DRILL2 = re.compile(r'(drill2: inject.*|bcmjob inject op=.*)$')
IMG6 = re.compile(r'video image \((\w+)\) (\d+) bytes fnv ([0-9a-f]{8})')

OPS = {}


def op_names():
    if not OPS:
        try:
            src = open(os.path.join(ROOT, 'bcmfw', 'jobs.h')).read()
            for name, v in re.findall(r'^#define J_OP_(\w+)\s+(0x[0-9A-Fa-f]+)', src, re.M):
                OPS[int(v, 16)] = name
        except OSError:
            pass
    return OPS


ERRS = {}


def err_name(n):
    """A job status (bcmjob fault ... st=<n>) by its J_E_* name"""
    if not ERRS:
        try:
            src = open(os.path.join(ROOT, 'bcmfw', 'jobs.h')).read()
            for name, v in re.findall(r'^#define (J_E_\w+)\s+(\d+)\b', src, re.M):
                ERRS[int(v)] = name
        except OSError:
            pass
    return ERRS.get(n, 'J_E_?')


def fault_st(text):
    """' st=21 (J_E_SLOT)' for a fault line that carries st=, else ''"""
    m = re.search(r'\bst=(\d+)', text)
    return ' st=%s (%s)' % (m.group(1), err_name(int(m.group(1)))) if m else ''


def op_name(hexs):
    try:
        v = int(hexs, 16)
    except ValueError:
        return hexs
    return '%s (0x%02x)' % (op_names().get(v, '?'), v)


# ---- printing --------------------------------------------------------------------
class Out:
    def __init__(self):
        self.buf = []

    def __call__(self, s=''):
        self.buf.append(clean(s))     # every line, whatever built it

    def head(self, s):
        self.buf.append('')
        self.buf.append('== %s ==' % s)

    def text(self):
        return '\n'.join(self.buf) + '\n'


def res_name(r):
    return RES[r] if 0 <= r < len(RES) else '?'


def session_table(p, s):
    st = s.st
    p.head('session %s' % clean(s.rel))
    if not s.session_line:
        p('no session line: not a results file, or its first line was lost')
    else:
        p('mode: %s; build %s; date %s' % (
            mode_name(st.mode), clean(s.build or '?'),
            clean(field(s.session_line, 'date') or '?')))
        if field(s.session_line, 'image') is None:
            p('image line: none (a file from before the image fields: earns no line)')
        else:
            p('image line: image=%s fnv=%s kind=%s' % tuple(
                clean(field(s.session_line, k) or '-') for k in ('image', 'fnv', 'kind')))
    p('step table: %s%s' % (s.rules.where, '' if all(s.fit) else
                            ' (does not fit this file well: %s)' % (
                                'ids unknown to the table' if not s.fit[0]
                                else 'steps= count differs')))
    if s.resumes:
        p('resumed %d time%s: after %s' % (len(s.resumes), '' if len(s.resumes) == 1 else 's',
                                           ', '.join(clean(x or '?') for x in s.resumes)))
    last = s.last()
    p('  %-7s %-6s %-22s %8s  %s' % ('step', 'result', 'answer', 'ms', 'note'))
    order = []
    for kind, sid, *_ in s.records:
        if sid not in order:
            order.append(sid)
    for sid in order:
        r, ans, ms, note, _ = last[sid]
        if r == START:
            what = {START: 'pending', HANG: 'HANG?'}.get(
                s.settled if s.st.dangling == find(s.steps, sid) else NONE, 'started')
            p('  %-7s %-6s %-22s %8s  %s' % (clean(sid), what, '-', '-',
                                             '(started, no result yet)'))
            continue
        step = s.step(sid)
        a = '-' if ans in (None, '-') else ans.replace('_', ' ')
        mark = ''
        if step and step['ask'] and a != '-':
            i = answer_index(step, ans)
            mark = '' if i == 0 else ' *'
        p('  %-7s %-6s %-22s %8d  %s' % (clean(sid), res_name(r).upper() if r in (FAIL, HANG)
                                         else res_name(r), clean(a + mark)[:22], ms,
                                         clean(note or '')))
        if sid in s.corr:
            ca, why = s.corr[sid]
            p('  %-7s corrected answer: %s (device: %s) %s' % (
                '', clean(ca.replace('_', ' ')), clean(a), clean(why)))
    auto = autoskips(s)
    for dep in sorted(auto, key=lambda d: auto[d][0]):
        _, r, sids = auto[dep]
        p('auto-skipped after %s %s: %s' % (clean(dep), clean(r), ', '.join(
            clean(x) for x in sids)))
    if s.verified:
        for v in s.verified:
            p('verified %s' % clean(v))
    for v in s.tokens:
        p('token %s  (a bcmfw/verified.txt line: the release\'s 150 MHz)' % clean(v))
    p('replayed: %d passed, %d failed, %d skipped' % (st.passed, st.failed, st.skipped))
    p('end line: %s' % (clean(s.end_line) if s.end_line else
                        'none (the session is still open: it resumes at the next press)'))


def autoskips(s):
    """The steps that skipped by themselves (a need failed), by the step
    they needed: {dep: (first line, its result as noted, [ids])}"""
    out = {}
    for sid, (r, ans, ms, note, n) in s.last().items():
        m = AUTOSKIP.match(note or '') if r == SKIP else None
        if m:
            first, res, ids = out.get(m.group(1), (n, m.group(2), []))
            out[m.group(1)] = (min(first, n), res, ids + [sid])
    for dep in out:
        out[dep][2].sort(key=lambda x: s.last()[x][4])
    return out


def anomalies_session(s, bcm, ui, context):
    """[(title, [detail lines])]"""
    out = []
    st = s.st
    last = s.last()
    # an auto-skip whose need did not fail in the replay: the device and
    # this tool disagree on the dependency rules
    for dep, (_, r, sids) in autoskips(s).items():
        k = find(s.steps, dep)
        got = st.res.get(k, NONE) if k >= 0 else NONE
        if got not in (FAIL, HANG, SKIP):
            out.append(('%s: skipped after %s, which did not fail' % (s.rel, dep),
                        ['the file: needs %s (%s); the replay: %s %s' % (
                            clean(dep), clean(r), clean(dep), res_name(got)),
                         'skipped: %s' % ', '.join(clean(x) for x in sids)]))
    for sid, (r, ans, ms, note, n) in last.items():
        step = s.step(sid)
        bad_answer = False
        if step and step['ask'] and ans not in (None, '-') and not step['flags'] & F['BENCH_F_GO']:
            bad_answer = answer_index(step, ans) != 0
        fixed = False
        if sid in s.corr and step and step['ask']:
            # the owner corrected it to the good answer: no bad answer; a
            # FAIL the device recorded still shows, marked, since only the
            # step's own lines can say whether the answer alone made it
            fixed = answer_index(step, s.corr[sid][0]) == 0
            bad_answer = bad_answer and not fixed
        if r in (FAIL, HANG) or bad_answer:
            title = '%s %s: %s%s' % (s.rel, sid, res_name(r).upper(),
                                     ' (answer owner-corrected to "%s": see OWNER '
                                     'CORRECTIONS)' % step['answers'][0] if fixed else '')
            det = []
            if step and step['ask']:
                # its first line: the question (the answers' meanings follow)
                q = step['ask'].replace('\\n', '\n').split('\n')[0]
                det.append('question: %s' % q)
                det.append('answer: %s%s' % (clean((ans or '-').replace('_', ' ')),
                                             '' if not bad_answer else
                                             ' (the good one is "%s")' % step['answers'][0]))
            if note:
                det.append('note: %s' % clean(note))
            for t in s.logs.get(sid, []):
                det.append('log= %s' % clean(t))
            win, how = step_window(bcm, sid, s.logs.get(sid), context)
            det.append('bcmtest.log, %s (%d lines%s):' % (
                how, len(win), '' if len(win) <= context else ', the last %d shown' % context))
            det += ['  ' + ln.show() for ln in win[-context:]]
            hits = []
            for t in s.logs.get(sid, []):
                t = t.strip()
                for i, ln in enumerate(ui):
                    if t and ln.text.endswith(t):
                        hits = ui[max(0, i - context // 2):i + context // 2 + 1]
                        break
                if hits:
                    break
            if hits:
                det.append('ipodui.log around it:')
                det += ['  ' + ln.show() for ln in hits]
            else:
                det.append('ipodui.log: no marker for this step (the log has no clock; '
                           'see the drift and stale-band sections)')
            out.append((title, det))
        elif r == START:
            sr = s.settled if st.dangling == find(s.steps, sid) else NONE
            out.append(('%s %s: started, never ended' % (s.rel, sid),
                        ['a resume records it %s' % ('as hung' if sr == HANG else
                                                     'when the owner finishes it')]))
    for n, ln in s.bad:
        out.append(('%s line %d: not a results-file line (corrupt or cut)' % (s.rel, n),
                    ['"%s"' % clean(ln[:120])]))
    if s.end_line:
        try:
            d = {k: int(field(s.end_line, k)) for k in ('passed', 'failed', 'skipped')}
            if (d['passed'], d['failed'], d['skipped']) != (st.passed, st.failed, st.skipped):
                out.append(('%s: the end line\'s counts differ from the replay' % s.rel,
                            ['end: %s; replay: %d/%d/%d' % (clean(s.end_line), st.passed,
                                                            st.failed, st.skipped)]))
        except (TypeError, ValueError):
            out.append(('%s: the end line is unreadable' % s.rel, [clean(s.end_line)]))
    if s.verified and s.computed and s.verified[-1] != s.computed:
        out.append(('%s: the file\'s verified line differs from the replay\'s' % s.rel,
                    ['file:   %s' % clean(s.verified[-1]), 'replay: %s' % s.computed]))
    if s.verified and not s.earns:
        out.append(('%s: the file has a verified line the replay does not earn' % s.rel,
                    ['why: %s' % why_text(s), 'the patch is not made from it']))
    if s.end_line and not s.verified and s.earns:
        out.append(('%s: the replay earns a line the file does not have' % s.rel,
                    ['replay: %s' % s.computed]))
    return out


# ---- must-pass steps that skipped (sitting 3, 2026-10-26) ----------------------
# A picked run's NEED step that ended skipped costs the verified line with
# no FAIL anywhere: sitting 3's jobs answered "self-test rc 1" twice with
# an empty note, after a restart step recorded done in the start it asked
# the owner to leave.  First in the block, with the cause the lines show.
RC1 = re.compile(r'self-test rc 1\)?:?\s*(.*)$')
NEW_START = ('=== boot path', 'boot: BCM powered')


def same_start(bcm, a_sid, b_sid):
    """True if bcmtest.log shows no new start between the restart step
    a_sid's last "auto: <a_sid> done" and the "auto: <b_sid> skip" after
    it (None: not found)"""
    ia = ib = None
    for i in range(len(bcm) - 1, -1, -1):
        m = AUTO.match(bcm[i].text)
        if m and m.group(1) == b_sid and m.group(2) == 'skip' and ib is None:
            ib = i
        elif m and m.group(1) == a_sid and m.group(2) == 'done' and ib is not None:
            ia = i
            break
    if ia is None or ib is None:
        return None
    return not any(any(k in ln.text for k in NEW_START) for ln in bcm[ia + 1:ib])


def need_skips(s, bcm):
    """[(title, [detail lines])] for each must-pass step that skipped"""
    out = []
    st = s.st
    if st.mode == MODE_FULL or not s.session_line:
        return out
    last = s.last()
    reboots = [x['id'] for x in s.steps if x['flags'] & F['BENCH_F_REBOOTS']]
    seen = {s.steps[k]['id'] for k, r in st.res.items() if 0 <= k < len(s.steps) and r != NONE}
    for k, role in select(st.mode, s.steps, s.rules.picks_for(st.mode), seen):
        sid = s.steps[k]['id']
        if role != R_NEED or st.res.get(k, NONE) != SKIP:
            continue
        _, _, _, note, at = last.get(sid) or (SKIP, None, 0, '', 0)
        note = note or ''
        logs = s.logs.get(sid, [])
        det = ['a must-pass step skipped: this session cannot earn the verified line',
               'note: %s' % (clean(note) if note else '(empty: the step gave no reason)')]
        am = AUTOSKIP.match(note)
        if am:
            det.append('it skipped by itself: it needs %s, which ended %s (the session '
                       'went on with what did not need it)' % (clean(am.group(1)),
                                                              clean(am.group(2))))
            out.append(('%s %s: a must-pass step SKIPPED (after %s)' % (
                s.rel, sid, clean(am.group(1))), det))
            continue
        det += ['log= %s' % clean(t) for t in logs]
        m = None
        for t in [note] + logs:
            m = m or RC1.search(t or '')
        if m:
            det.append('cause: the job self-test answered rc 1, "unavailable": no job '
                       'driver to use in this start (it needs our merged image as the '
                       'resident display driver, jobs allowed)%s' %
                       ('; the device said: %s' % clean(m.group(1)) if m.group(1) else
                        '; this build did not say why (empty reason)'))
        pre = last.get('pre')
        if m and pre and pre[0] == PASS and 'display driver' not in (pre[3] or ''):
            det.append('the pre-flight passed on the stage flags alone (a build before '
                       'its display-driver check), so the restart step was skipped')
        for rb in reboots:
            rr = last.get(rb)
            if not rr or rr[0] not in (DONE, PASS) or rr[4] > at:
                continue
            rlogs = s.logs.get(rb, [])
            if not any('restart asked' in t for t in rlogs) and \
                    'restart seen' not in (rr[3] or ''):
                det.append('the restart step %s was recorded %s with no restart check '
                           '(a build before the check)' % (rb, res_name(rr[0])))
            same = same_start(bcm, rb, sid)
            if same:
                det.append('bcmtest.log: no new start between "auto: %s" and "auto: %s": '
                           'the restart never happened' % (rb, sid))
        if m and not any(d.startswith('bcmtest.log: no new start') for d in det):
            det.append('check bcmtest.log for "boot path: active" in this start')
        out.append(('%s %s: a must-pass step SKIPPED%s' % (
            s.rel, sid, ' (job self-test rc 1)' if m else ''), det))
    for rb in reboots:
        for t in s.logs.get(rb, []):
            if t.startswith('No restart seen'):
                out.append(('%s %s: the owner went on without the restart it asked for'
                            % (s.rel, rb), ['log= %s' % clean(t),
                                            'the device re-offered it (not recorded done)']))
                break
    return out


def why_text(s):
    w = s.why_not
    if isinstance(w, tuple):
        k, r = w
        return 'step %s %s' % (s.steps[k]['id'], res_name(r))
    return w or '-'


# ---- the bench checks (readlog) --------------------------------------------------
BUILD_STAMP = re.compile(r'\bbuild ([0-9a-f]{7,40})(?:-\d+)?\b')
BENCH_FNV = re.compile(r'^\s*\d+ boot bench(?:_pll)?: \d+ bytes, fnv ([0-9a-f]{8})')


def bench_checks(bcm_lines, commit=None):
    """[(run label, {tag: (state, text, modelled)}, where, old)] from
    readlog's parse, one log file (one boot) at a time.  where names the
    file, the bench image's fnv and the build the boot's own lines stamp
    (if any); old is true when that build is not commit: a rotated log
    from an earlier sitting, whose image judged against its own, older
    expected values (sitting 2's folder held sitting 1's caaa9890b0
    stage 4, whose X15S/REPA flags failed against the wrapping model)."""
    files = []
    for ln in bcm_lines:
        if not files or files[-1][0] != ln.src:
            files.append((ln.src, []))
        files[-1][1].append(ln.text)
    out = []
    for src, texts in files:
        stamp = next((m.group(1) for m in map(BUILD_STAMP.search, texts) if m), None)
        fnvs = []              # each run's bench image: the first fnv after its start
        for t in texts:
            if readlog.START.match(t):
                fnvs.append(None)
            elif fnvs and fnvs[-1] is None:
                m = BENCH_FNV.match(t)
                if m:
                    fnvs[-1] = m.group(1)
        old = bool(commit and stamp and not (stamp.startswith(commit[:10]) or
                                             commit.startswith(stamp)))
        for k, run in enumerate(readlog.parse(texts)):
            recs = {r['tag']: r for r in run['vrecs'].values() if 'tag' in r}
            v = readlog.tag_verdicts(recs, CHECKS) if recs else {}
            v.update(readlog.algnlog.verdicts(run.get('algn')))
            if v:
                where = [src]
                if k < len(fnvs) and fnvs[k]:
                    where.append('bench fnv %s' % fnvs[k])
                where.append('build %s' % stamp if stamp else 'no build stamp')
                if old:
                    where.append('an older build than %s: not counted' % commit)
                out.append(('stage %s' % run['stage'], v, ', '.join(where), old))
    return out


def stale_band(p, sessions, bcm, ui, src_core):
    p.head('stale band: the top rows not refreshed')
    p('Suspects: rects that start below row 0 or leave the header band out; the '
      'scroll and pan first-row and offset parameters; engine-2 pieces on the top rows.')
    kinds = re.findall(r'\{\s*"([^"]+)",\s*(\d+),\s*(\d+),\s*(\d+),\s*(\d+)\s*\}',
                       src_core.split('e_kinds[4] =', 1)[-1].split('};', 1)[0]) if src_core else []
    if kinds:
        p('stage 5\'s scroll storm rects (bcm-devtest-core.c e_kinds; the storm logs '
          'only totals): %s' % '; '.join('%s %s,%s %sx%s' % k for k in kinds))
        top = [k for k in kinds if int(k[2]) > 0]
        if top:
            p('  by design, rows above y=%d are refreshed only by the "%s" rect during the '
              'storm: a band at the top that stops moving there is the test\'s own '
              'layout, not a fault' % (min(int(k[2]) for k in kinds if k[0] != 'clock')
                                       if any(k[0] != 'clock' for k in kinds) else 0,
                                       '", "'.join(k[0] for k in kinds if int(k[2]) < 20)))
    pats = [
        ('rect below row 0', re.compile(r'(UPDATERECT|rect)\D*(\d+),(\d+) (\d+)x(\d+)')),
        ('job driver resync', re.compile(r'bcmjob resync x=\d+ y=\d+')),
        ('upload rows deferred', re.compile(r'bcmjob upload_rect deferred rows=')),
        ('storm', re.compile(r'\bstorm\b')),
        ('drift pans / panel window', re.compile(r'drift: (fade|half)|\bpans?\b')),
        ('drift parity', re.compile(r'drift parity')),
        ('engine 2 pieces', re.compile(r'\be2 \S+: \d+ B|engine 2 \d+ us \(\d+ pieces\)|pieces')),
        ('window push (B5)', re.compile(r'lab5: |window push')),
        ('COMMIT readback (CR1)', re.compile(r'\bcr1[ :]|bcmjob fault op=0x22')),
        ('scroll / pan parameters', re.compile(r'\b(scroll|SCROLL|PAN|pan)\b.*(y|dy|row|off)')),
    ]
    found = False
    for label, pat in pats:
        hits = []
        for ln in bcm + ui:
            m = pat.search(ln.text)
            if not m:
                continue
            if label == 'rect below row 0' and int(m.group(3)) == 0:
                continue
            hits.append(ln)
        if hits:
            found = True
            p('-- %s (%d line%s):' % (label, len(hits), '' if len(hits) == 1 else 's'))
            for ln in hits[:12]:
                p('  ' + ln.show())
            if len(hits) > 12:
                p('  ... %d more' % (len(hits) - 12))
    if not found:
        p('no suspect lines in the logs')
    ran = []
    for s in sessions:
        last = s.last()
        for sid in PICTURE_STEPS:
            if sid in last:
                r, ans, *_ = last[sid]
                ran.append('%s %s %s' % (sid, res_name(r), clean(ans or '-')))
    if ran:
        p('picture steps in the file(s): %s' % ', '.join(ran))


def drift_still(p, pulled, bcm, ui, anomalies):
    p.head('drift still: why the covers would not drift')
    rows = []
    for ln in ui + bcm:
        t = ln.text
        if AUTO.match(t):
            continue
        if DRIFTJ.search(t) or BCMJOB.search(t) or BOOTP.search(t) or DRILL2.search(t) \
                or VPU.search(t):
            rows.append(ln)
    marker = pulled.exists('bcmjobs.on')
    p('developer marker bcmjobs.on: %s' % ('present' if marker else 'absent'))
    if not rows:
        p('no drift, job driver or boot path lines')
    first_still = next((ln for ln in ui if 'drift still why=' in ln.text), None)
    for src in (ui, bcm):
        for ln in src:
            if ln in rows:
                extra = ''
                if DRILL2.search(ln.text) and first_still is not None:
                    if ln.src == first_still.src:
                        extra = '   [%+d lines from the first drift still]' % (ln.n - first_still.n)
                    else:
                        extra = '   [first drift still: %s:%d]' % (first_still.src, first_still.n)
                if 'bcmjob fault ' in ln.text and fault_st(ln.text):
                    extra = '   [%s]' % fault_st(ln.text).strip() + extra
                p('  ' + ln.show() + extra)
    verdict = drift_verdict(ui, bcm, marker)
    p('verdict: %s' % verdict)
    if verdict.startswith('('):
        anomalies.append(('drift still', ['verdict: ' + verdict] +
                          (['first: ' + first_still.show()] if first_still else [])))


VCLK = re.compile(r'\b(vclk (band|sum|time) |vbv[ :]|vgov[ :])')
VCLK_BAND = re.compile(r'vclk band from=(\d+) to=(\d+) lvl=(\d+) why=(\w+) '
                       r'util=(\d+) floor=(\d+)')
VCLK_SUM = re.compile(r'vclk sum policy=(\w+) mhz=(\d+) lvl=([\d.]+) idle=(\d+) '
                      r'bands=(\d+) clocks=(\d+) lvl_changes=(\d+) fails=(\d+) '
                      r'misses=(\d+) s=(\d+)')


def video_clock(p, bcm, ui, anomalies):
    """P19 PW4: the chip's clock governor (apps/video/vclk.c's lines) and
    the clock rows (vbv, vgov): every change, each play's means and time
    at each band, the rows' verdicts.  Proxies only: no current"""
    p.head('video clock: the chip\'s bands and governor')
    rows = [ln for ln in bcm + ui if VCLK.search(ln.text)]
    if not rows:
        p('no vclk, vbv or vgov lines (no governed play, or an image '
          'without the bands)')
        return
    whys = {}
    fails = []
    for ln in rows:
        m = VCLK_BAND.search(ln.text)
        if m:
            whys[m.group(4)] = whys.get(m.group(4), 0) + 1
            if m.group(4) == 'fail':
                fails.append(ln.show())
    p('band moves by why: %s' % (', '.join('%s %d' % kv for kv in sorted(whys.items()))
                               or 'none'))
    for ln in rows:
        m = VCLK_SUM.search(ln.text)
        if m:
            p('  play (in-band policy %s): mean %s MHz, level %s, idle %s permille, '
              '%s band moves, %s clock changes (%s of level), %s fallbacks, '
              '%s misses, %s s' % m.groups())
    for ln in rows:
        p('  ' + ln.show())
    bad = [ln.show() for ln in rows if re.search(r'\b(vbv|vgov)\b.*\bFAIL\b', ln.text) or
           (VCLK_SUM.search(ln.text) and VCLK_SUM.search(ln.text).group(9) != '0')]
    if fails:
        anomalies.append(('video clock: the chip fell back to the crystal', fails))
    if bad:
        anomalies.append(('video clock: a clock row failed or a governed play missed', bad))


RESP = re.compile(r'resident show us=(\d+) rc=(-?\d+)|art panel bcm dec_ms=(\d+) '
                  r'read_ms=(\d+)|FAULT art bcm read|bcmjob overlay (ok|refused|load) k=1|'
                  r'bcmjob overlay refused|bcmjob decode why=nowork|resident set key=|'
                  r'drift cover park|bcmjob fault op=0x30')


# the step answered (bcmtest.log's auto: line, an answer given) and the
# chip's own evidence: the overlay loaded, or a panel or box decoded there
RESP_STEP = re.compile(r'\bauto: respanel (pass|fail) answer=(?!-)\S')
RESP_CHIP = re.compile(r'bcmjob overlay load k=1|art panel bcm dec_ms=|'
                       r'panel resident dec_ms=|art bcm decode ')


def resident_panel(p, bcm, ui, anomalies):
    """RESPANEL (bench-p19.md; album-render.md 5.3): the merged image's
    DECODE overlay loaded, the panel decoded on the chip into a resident
    slot, its still frame's BLIT time (resident show us=), the background
    read-back's time (read_ms=), and never a read on the UI's path"""
    p.head('resident panel: DECODE on the chip, the BLIT and the read-back')
    rows = [ln for ln in bcm + ui if RESP.search(ln.text)]
    # sitting 3: RESPANEL answered while nothing loaded the overlay and no
    # panel decoded on the chip: the owner judged the PP's panel
    asked = [ln for ln in bcm if RESP_STEP.search(ln.text)]
    on_chip = [ln for ln in bcm + ui if RESP_CHIP.search(ln.text)]
    if asked and not on_chip:
        cpu = [ln for ln in ui if 'art panel cpu ms=' in ln.text]
        anomalies.append(('respanel: DECODE on the chip untested: the panel was '
                          'decoded on the PP',
                          [asked[-1].show()] + [x.show() for x in cpu[-1:]] +
                          ['no "bcmjob overlay load k=1", no "art panel bcm dec_ms=", '
                           'no "art bcm decode"']))
    if not rows:
        p('no resident-panel lines (no panel decoded on the chip this session)')
        return
    shows = [int(m.group(1)) for m in (RESP.search(ln.text) for ln in rows)
             if m and m.group(1) and m.group(2) == '0']
    reads = [int(m.group(4)) for m in (RESP.search(ln.text) for ln in rows)
             if m and m.group(4) is not None]
    p('still frames: %d, BLIT us max %s (one frame: 33,333); read-backs: %d, '
      'read_ms max %s' % (len(shows), max(shows) if shows else '-', len(reads),
                          max(reads) if reads else '-'))
    for ln in rows:
        p('  ' + ln.show())
    faults = [ln.show() for ln in rows if 'FAULT art bcm read' in ln.text or
              'bcmjob fault op=0x30' in ln.text]
    if faults:
        anomalies.append(('resident panel: a read on the UI\'s path or a DECODE fault',
                          faults))
    slow = [ln.show() for ln in rows if RESP.search(ln.text).group(1) and
            int(RESP.search(ln.text).group(1)) > 33333]
    if slow:
        anomalies.append(('resident panel: a still frame over one frame', slow))


VFEED_START = '=== video pace (vfeed) ==='
VFEED_CLIP = re.compile(r'\bvfeed (.+?): (PASS|FAIL|Not available \(no clip\))')
VFEED_CLIPS = 2


def video_pace(p, bcm, ui, anomalies):
    """vfeed (bench-p19.md): each run's clips, their FEED p95 against the
    frame, and any run that played fewer than its two clips (sitting 3's
    zip had no stage7.m4v: the converted half went untested and only the
    log said so)"""
    p.head('video pace: vfeed')
    rows = [ln for ln in bcm + ui if 'vfeed' in ln.text]
    if not rows:
        p('no vfeed lines (the pace row did not run)')
        return
    runs = []
    for ln in rows:
        if VFEED_START in ln.text or not runs:
            runs.append([])
        if VFEED_START not in ln.text:
            runs[-1].append(ln)
    short = []
    for n, run in enumerate(runs, 1):
        played = [VFEED_CLIP.search(ln.text) for ln in run]
        played = [m for m in played if m and m.group(2) in ('PASS', 'FAIL')]
        missing = [m.group(1) for m in (VFEED_CLIP.search(ln.text) for ln in run)
                   if m and m.group(2).startswith('Not available')]
        p('run %d: %d of %d clips played%s' % (
            n, len(played), VFEED_CLIPS,
            '; missing: ' + ', '.join(missing) if missing else ''))
        for ln in run:
            p('  ' + ln.show())
        if len(played) < VFEED_CLIPS and not any(
                'stage 5 flag or video image missing' in ln.text for ln in run):
            short.append('run %d played %d of %d clips; untested this sitting: %s'
                         % (n, len(played), VFEED_CLIPS,
                            ', '.join(missing) or 'a clip that never started'))
    if short:
        anomalies.append(('video pace: vfeed played fewer than its %d clips (the '
                          'owner\'s answer covers only what played)' % VFEED_CLIPS,
                          short))


VIZ = re.compile(r'\bviz (start|hold|CPU FALLBACK|release wait|bcm frame wait)|'
                 r'art bcm viz|bcmjob overlay (load|ok|refused)|FAULT bcm viz|'
                 r'uilat cpu ')


def visualizer(p, bcm, ui, anomalies):
    """uviz (0.8.0): the visualizer on the chip through the merged image's
    overlay: its path, the holds and reloads around a track change's
    DECODE (the shared region), the CPU while it runs, and never the CPU
    fallback"""
    p.head('visualizer: on the chip, the swap, the CPU')
    rows = [ln for ln in ui + bcm if VIZ.search(ln.text)]
    if not rows:
        p('no visualizer lines (it did not run this session)')
        return
    starts = [ln for ln in rows if 'viz start' in ln.text]
    loads = [ln for ln in rows if 'overlay load' in ln.text]
    cpu = [ln for ln in rows if 'uilat cpu' in ln.text]
    p('starts: %d (%s); overlay loads: %d; cpu samples: %d' % (
        len(starts), ', '.join(sorted({m.group(1) for m in (
            re.search(r'path=(\w+)', ln.text) for ln in starts) if m})) or '-',
        len(loads), len(cpu)))
    for ln in rows:
        p('  ' + ln.show())
    bad = [ln.show() for ln in rows if 'CPU FALLBACK' in ln.text or
           'FAULT bcm viz' in ln.text or 'overlay refused' in ln.text]
    if bad:
        anomalies.append(('visualizer: off the chip (CPU fallback or the overlay '
                          'refused)', bad))


VCHK = re.compile(r'\bunit check: |\bvck (PASS|FAIL|INCONCLUSIVE) band|'
                  r'\bvck stopped|\bvck the image|\bvideo check: |\bvplay: ')
VCHK_END = re.compile(r'\bvck (PASS|FAIL|INCONCLUSIVE) band (-?\d+) (\d+) MHz '
                      r'tried (\d+) (\d+)ms')


def video_check(p, bcm, ui, anomalies):
    """vplay (sitting 4d): the first-use video check (vcheck.h) on a
    normal play from Videos, after the step dropped the unit's check
    result: its verdict, band and time (the vck line; about 0.9 s on
    silicon), and the result the step's line carries"""
    p.head('video check: the first-use check (vplay)')
    rows = [ln for ln in ui + bcm if VCHK.search(ln.text)]
    if not rows:
        p('no unit check lines (no play from Videos this session)')
        return
    ends = [(ln, VCHK_END.search(ln.text)) for ln in rows]
    ends = [(ln, m) for ln, m in ends if m]
    for ln, m in ends:
        p('verdict %s: band %s (%s MHz), %s tried, %s ms' % m.groups())
    if not ends:
        p('no vck verdict line: the check did not run (see the lines below)')
    for ln in rows:
        p('  ' + ln.show())
    bad = [ln.show() for ln, m in ends if m.group(1) != 'PASS' or
           int(m.group(5)) > 1500]
    reset = any('unit check: token' in ln.text and 'bench reset' in ln.text
                for ln in rows)
    if reset and not ends:
        bad.append('vplay dropped the check result, but no check ran')
    if bad:
        anomalies.append(('video check: not a pass, over 1.5 s, or did not run',
                          bad))


def drift_verdict(ui, bcm, marker):
    """The candidate that fits the lines best: (a) a job marked failed or
    slow; (b) PRESENT/FENCE never completing; (c) the frame_due hook
    stalling the feed; (d) jobs on but the merged image not resident;
    (e) drill 2's injected FADE failures; (f) the slot upload refused (a
    COMMIT that failed before the first still).  A cause counts only when
    it comes before the first still in the same boot (ipodui.log's order)."""
    stills = [ln for ln in ui if 'drift still why=' in ln.text]
    if stills:
        k = ui.index(stills[0])
        pre = [ln for ln in ui[:k] if ln.boot == stills[0].boot]
    else:
        pre = ui
    whys = [re.search(r'why=(\S+)', ln.text).group(1) for ln in stills]
    faults = [(re.search(r'op=0x([0-9a-f]+)', ln.text), re.search(r'why=(\S+)', ln.text), ln)
              for ln in pre if 'bcmjob fault ' in ln.text]
    late = [re.search(r'at=(\S+)', ln.text) for ln in pre if 'drift job late at=' in ln.text]
    # an injection explains only a still it precedes: ipodui.log's own
    # "bcmjob inject" line, before the first still, same boot
    inject = [ln for ln in pre if 'bcmjob inject ' in ln.text]
    commit = [f for f in faults if f[0] and f[0].group(1) == '22']
    up = [ln for ln in ui if 'bcmjob up epoch=' in ln.text]
    cpu = [ln for ln in ui if 'bcmjob cpu why=' in ln.text]
    modes = [re.search(r'mode=(\S+)', ln.text).group(1) for ln in ui
             if 'drift jobs mode=' in ln.text]
    active = [ln for ln in bcm if re.search(r'boot path: active: ', ln.text)]
    fell = [ln for ln in bcm if re.search(r'boot path: (fell back|not switched|skipped)',
                                          ln.text)]
    deferred = [ln for ln in ui if 'upload_rect deferred' in ln.text]
    dec_to = [f for f in faults if f[0] and f[0].group(1) == '30' and f[1] and
              f[1].group(1) in ('timeout', 'aborted')]
    wanted = marker or (modes and modes[-1] in ('on', 'auto', 'parity'))
    resident = bool(active) and 'bcmvideo' in active[-1].text
    if stills and inject and (('failed' in whys or 'noop' in whys) or
                              any(f[0] and f[0].group(1) == '26' for f in faults)):
        return '(e) fits: drill 2 injected FADE failures (%s), and the drift went still ' \
               '(why=%s): the fallback worked as planned' % (
                   clean(inject[0].text.strip()), ','.join(whys) or '-')
    if stills and commit:
        after = [op_name(f[0].group(1)) for f in faults
                 if f[0] and f[0].group(1) != '22' and f[2].n > commit[0][2].n]
        return '(f) fits: the slot upload was refused: COMMIT (0x22) failed%s before the ' \
               'first still (%s)%s' % (
                   fault_st(commit[0][2].text), clean(stills[0].text.split(' ', 1)[-1]),
                   '; then %s on the uncommitted slot' % ', '.join(after) if after else '')
    if wanted and (cpu or (fell and not resident) or (active and not resident) or
                   ('off' in whys and not up)):
        why = (clean(cpu[-1].text.split('bcmjob ', 1)[-1]) if cpu else
               clean(fell[-1].text.strip()) if fell else
               clean(active[-1].text.strip()) if active else 'no "bcmjob up" line')
        return '(d) fits: jobs wanted (%s) but the merged image is not the resident ' \
               'display driver: %s' % ('bcmjobs.on' if marker else 'drift mode ' + modes[-1], why)
    # "drift job late at=<op>": the frame's FENCE (after its PRESENT) did
    # not retire in time; at= names the last op sent before it
    pf = [l for l in late if l] + [f for f in faults if f[0] and f[0].group(1) in ('29', '20')]
    if pf or ('slow' in whys and not [f for f in faults if f[1] and f[1].group(1) == 'failed']):
        return '(b) fits: a frame\'s PRESENT/FENCE never completed (%s)%s' % (
            ', '.join('late ' + clean(x.group(0)) for x in late if x) or 'why=slow',
            '' if not stills else '; the drift went still (why=%s)' % ','.join(whys))
    bad = [f for f in faults if f[1] and f[1].group(1) in ('failed', 'timeout', 'slow',
                                                          'refused', 'busy-decode')]
    if stills and (bad or set(whys) & {'failed', 'noop', 'slow'}):
        ops = sorted({op_name(f[0].group(1)) + fault_st(f[2].text) for f in bad if f[0]})
        return '(a) fits: a job marked %s, so D-1\'s still drift; op %s' % (
            '/'.join(sorted({f[1].group(1) for f in bad if f[1]})) or ','.join(whys),
            ', '.join(ops) or '(no fault line: see the drift job fault lines)')
    if len(deferred) >= 3 or dec_to:
        return '(c) fits: the job owner\'s feed waited on frame_due (%d deferred ' \
               'upload%s, %d DECODE timeouts) while no fault ended the jobs' % (
                   len(deferred), '' if len(deferred) == 1 else 's', len(dec_to))
    if stills:
        return 'none fits cleanly: the drift went still (why=%s) with no fault, late or ' \
               'boot path line to explain it' % ','.join(whys)
    if wanted and up:
        return 'no candidate fits: jobs came up and nothing went still in the logs ' \
               '(a static picture with no log line points at (c), the feed)'
    return 'no candidate fits: no drift still line'


# ---- the verified patch ---------------------------------------------------------
DOC_FILES = ('docs/bcm2722-reference/isa-vector.md', 'docs/bcm2722-reference/quirks.md')


def doc_edits(text, verdicts, evidence):
    """Tag changes for the reference docs: "[silicon] pending (TAGS)" and
    "**Silicon pending**: bench TAGS" become [silicon] with the session as
    evidence, where every named check came back as modelled.  Returns the
    new text and the suggestions that need a writer."""
    sugg = []

    def tags_of(s):
        return re.findall(r'\b[A-Z0-9]{4}\b', s)

    def sub1(m):
        tags = [t for t in tags_of(m.group(1)) if t in CHECKS]
        if not tags or any(t not in verdicts for t in tags):
            return m.group(0)
        if all(verdicts[t][2] for t in tags):
            return '[silicon] as modelled (%s; %s)' % (', '.join(tags), evidence)
        sugg.append('%s: not as modelled: %s' % (', '.join(tags), '; '.join(
            '%s %s' % (t, verdicts[t][1] or verdicts[t][0]) for t in tags)))
        return m.group(0)

    def sub2(m):
        tags = [t for t in tags_of(m.group(1)) if t in CHECKS]
        if not tags or any(t not in verdicts for t in tags):
            return m.group(0)
        if all(verdicts[t][2] for t in tags):
            return '**Silicon** (%s): bench %s as modelled' % (evidence, ', '.join(tags))
        sugg.append('%s: not as modelled: %s' % (', '.join(tags), '; '.join(
            '%s %s' % (t, verdicts[t][1] or verdicts[t][0]) for t in tags)))
        return m.group(0)
    text = re.sub(r'\[silicon\] pending \(([A-Z0-9, ]+)\)', sub1, text)
    text = re.sub(r'\*\*Silicon pending\*\*: bench ([A-Z0-9, and]+?)(?=[.|]| \||$)',
                  sub2, text, flags=re.M)
    return text, sugg


def merged_verdicts(checks):
    """One verdict per tag over the runs (stage 4 and 4b): modelled only if
    every run says so"""
    out = {}
    for label, v in checks:
        for tag, (state, text, ok) in v.items():
            if tag in out:
                s0, t0, ok0 = out[tag]
                out[tag] = (s0 if s0 == state else '%s/%s' % (s0, state),
                            t0 if t0 == text else '%s | %s' % (t0, text), ok0 and ok)
            else:
                out[tag] = (state, text, ok)
    return out


def make_patch(line, verdicts, evidence, how, patch_path, repo=ROOT):
    """The patch text, or raise.  Built in a temporary worktree of repo's
    HEAD; the user's tree is never touched."""
    tmp = tempfile.mkdtemp(prefix='bench-ingest-')
    wt = os.path.join(tmp, 'wt')
    notes = []
    try:
        r = subprocess.run(['git', '-C', repo, 'worktree', 'add', '--detach', wt, 'HEAD'],
                           capture_output=True, text=True)
        if r.returncode:
            raise RuntimeError('git worktree add: %s' % r.stderr.strip())
        vt = os.path.join(wt, 'bcmfw', 'verified.txt')
        cur = open(vt).read()
        fnv = line.split()[1]
        # a clock150 token naming the same image is not its verified line
        if re.search(r'^(?!clock150\s)\S+\s+%s\b' % re.escape(fnv), cur, re.M):
            notes.append('verified.txt already lists %s: not added again' % fnv)
        else:
            with open(vt, 'a') as f:
                f.write(('' if cur.endswith('\n') else '\n') + line + '\n')
        for rel in DOC_FILES:
            p = os.path.join(wt, rel)
            if not os.path.exists(p):
                continue
            old = open(p).read()
            new, sugg = doc_edits(old, verdicts, evidence)
            notes += ['%s: %s' % (rel, s) for s in sugg]
            if new != old:
                with open(p, 'w') as f:
                    f.write(new)
        if how == 'auto':
            how = 'run' if have_vc4(wt) else 'render'
        if how == 'run':
            r = subprocess.run([sys.executable, '-I', 'bcmfw/mkdevtest.py'], cwd=wt,
                               capture_output=True, text=True)
            if r.returncode:
                raise RuntimeError('mkdevtest: %s' % (r.stderr.strip() or r.stdout.strip()))
            notes.append('bcm-devtest-images.c: bcmfw/mkdevtest.py in a temporary worktree')
        elif how == 'render':
            r = subprocess.run([sys.executable, '-I', '-c', RENDER], cwd=wt,
                               capture_output=True, text=True)
            if r.returncode:
                raise RuntimeError('mkdevtest render: %s' % (r.stderr.strip() or
                                                             r.stdout.strip()))
            notes.append('bcm-devtest-images.c: mkdevtest\'s renderer over the committed '
                         'images (no VideoCore toolchain run)')
        r = subprocess.run(['git', '-C', wt, 'diff', '--no-color'], capture_output=True,
                           text=True)
        diff = r.stdout
        os.makedirs(os.path.dirname(os.path.abspath(patch_path)), exist_ok=True)
        with open(patch_path, 'w') as f:
            f.write(diff)
        return diff, notes
    finally:
        subprocess.run(['git', '-C', repo, 'worktree', 'remove', '--force', wt],
                       capture_output=True)
        shutil.rmtree(tmp, ignore_errors=True)


def have_vc4(wt):
    r = subprocess.run([sys.executable, '-I', '-c',
                        'import sys; sys.path.insert(0, "bcmfw"); import build as B; '
                        'sys.exit(0 if B.have_gcc() else 1)'], cwd=wt, capture_output=True)
    return r.returncode == 0


# mkdevtest's own renderer over the images already committed (their bytes
# read back from the generated file): the same file mkdevtest would write
# from the same images, with the new verified.txt.  It checks itself: with
# the old verified table it must reproduce the committed file exactly.
RENDER = r'''
import os, re, sys
sys.path.insert(0, 'bcmfw')
import mkdevtest as M
src = open(M.OUT_C).read()
imgs = []
for m in re.finditer(r'/\* (\w+): (.*?); (\d+) bytes; sha256 [0-9a-f]+ \*/\n'
                     r'static const uint8_t \w+_data\[\d+\][^=]*=\n\{\n(.*?)\n\};', src, re.S):
    data = bytes(int(x, 16) for x in re.findall(r'0x([0-9a-f]{2})', m.group(4)))
    if m.group(2).startswith('bcmfw/hibstub.c'):
        defs = None             # the hibernate stub: not an image (mkdevtest.py)
    else:
        defs = [] if m.group(2) == 'default build' else m.group(2).replace('-D ', '').split()
    imgs.append((m.group(1), defs, data[:int(m.group(3))]))
names = [n for n, _ in M.IMAGES] + (['hibstub'] if any(
    i[0] == 'hibstub' for i in imgs) else [])
if [i[0] for i in imgs] != names:
    sys.exit('the committed images are not mkdevtest\'s list: %s' % [i[0] for i in imgs])
# the overlay's record (mkdevtest's render() reads its numbers only),
# read back from the committed file like the images
om = re.search(r'const struct bcmdt_ovl bcmdt_viz_ovl =\n\{\n\s*(\d+), 0x([0-9a-f]+), '
               r'0x([0-9a-f]+), (\d+), 0x([0-9a-f]+)(?:, 0x([0-9a-f]+))?', src)
# (since batch 3 the record ends with the region's base: 6 words)
ovl = (None, (int(om.group(1)), int(om.group(2), 16), int(om.group(3), 16),
              int(om.group(4)), int(om.group(5), 16)) +
       ((int(om.group(6), 16),) if om.group(6) else ())) if om else None
def text():
    import inspect
    if len(inspect.signature(M.render).parameters) > 1:
        return M.prokbox_header.convert(M.render(imgs, ovl), '.c')[0]
    return M.prokbox_header.convert(M.render(imgs), '.c')[0]
new = text()
keep = M.verified
try:
    import subprocess
    old_v = subprocess.run(['git', 'show', 'HEAD:bcmfw/verified.txt'], capture_output=True,
                           text=True).stdout
    def old_verified():
        out = []
        for line in old_v.splitlines():
            line = line.split('#', 1)[0].strip()
            if line:
                f = line.split(None, 2)
                if f[0] in getattr(M, 'TOKENS', ()):
                    continue        # a clock token, not a boot image
                out.append((f[0], int(f[1], 16), f[2] if len(f) > 2 else ''))
        return out
    M.verified = old_verified
    if text() != src:
        sys.exit('re-rendering the committed images does not reproduce the file')
finally:
    M.verified = keep
open(M.OUT_C, 'w').write(new)
'''


# ---- owner corrections (--corrections) ---------------------------------------------
def load_corrections(path):
    """[(results file or '*', step, answer, why)]; a malformed line stops
    the run (a correction must never be silently dropped)"""
    out = []
    with open(path, encoding='utf-8', errors='replace') as f:
        for n, line in enumerate(f, 1):
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            m = re.match(r'(\S+)\s+(\S+)\s+(\S+)\s+(.+)$', line)
            if not m or ':' not in m.group(4):
                sys.exit('%s:%d: not "<file|*> <step> <answer> <who>: <why>"' % (path, n))
            out.append((m.group(1), m.group(2), token(m.group(3)), m.group(4)))
    return out


# ---- main ----------------------------------------------------------------------------
# Developer builds from 69945379dd until 9d747741e3 read three .bss
# objects through the front end's remote-call wrapper (febe_rpc.list
# wrapped DATA symbols: the shell read a stub's code as the struct), so
# their "fat empty slot …", "fat slot …", "ata barrier flush …" and the
# USB screen's "ata flush …" lines carry garbage.  They are left out for
# such a build (or one git cannot place), and counted.
FEBE_BUG, FEBE_FIX = '69945379dd', '9d747741e3'
FEBE_NOISE = re.compile(r'\b(fat empty slot|fat slot (prev|this)|ata barrier flush|'
                        r'ata flush)\b')


def git_has(repo, commit, ancestor):
    """True or False when git can say whether ancestor is in commit;
    None when it can't (an unknown build)"""
    r = subprocess.run(['git', '-C', repo, 'merge-base', '--is-ancestor', ancestor,
                        commit], capture_output=True, text=True)
    return {0: True, 1: False}.get(r.returncode)


def febe_noise_build(repo, build):
    """'yes' (has the bug, not the fix), 'no', or 'unknown'"""
    hexes = re.findall(r'[0-9a-f]{7,40}', build or '')
    if not hexes:
        return 'unknown'
    fix, bug = git_has(repo, hexes[-1], FEBE_FIX), git_has(repo, hexes[-1], FEBE_BUG)
    if fix or bug is False:
        return 'no'
    return 'yes' if bug else 'unknown'


def main(argv):
    args, opts = [], {'--commit': None, '--patch': None, '--context': str(CONTEXT),
                      '--mkdevtest': 'auto', '--corrections': None}
    i = 0
    while i < len(argv):
        a = argv[i]
        if a in opts:
            if i + 1 >= len(argv):
                print('%s needs a value' % a, file=sys.stderr)
                return 2
            opts[a] = argv[i + 1]
            i += 2
            continue
        if a.startswith('-'):
            print(__doc__.split('\n\n')[1], file=sys.stderr)
            return 2
        args.append(a)
        i += 1
    if len(args) != 1 or not os.path.isdir(args[0]) or \
            opts['--mkdevtest'] not in ('auto', 'run', 'render', 'none'):
        print(__doc__.split('\n\n')[1], file=sys.stderr)
        return 2
    context = int(opts['--context']) if opts['--context'].isdigit() else CONTEXT
    pulled = Pulled(args[0])
    try:
        rules = Rules(opts['--commit'])
    except (ValueError, OSError) as e:
        print('bench_ingest: the step table: %s' % e, file=sys.stderr)
        return 2

    bcm = log_lines(pulled, pulled.generations('bcmtest', '.log', 'mid'))
    ui = log_lines(pulled, pulled.generations('ipodui', '.log', 'end'))
    # session files by content (the first line is the session line), at the
    # root (a flat copy) or under bench/
    results = []
    for rel in [n for n in pulled.listdir() if n.endswith('.txt')] + \
            ['bench/' + n for n in pulled.listdir('bench') if n.endswith('.txt')]:
        if pulled.exists(rel) and os.path.getsize(pulled.path(rel)) <= MAX_BYTES:
            ls = pulled.lines(rel)
            if ls and ls[0].startswith('session build='):
                results.append(rel)
    sessions = [Session(pulled, rel, rules) for rel in results]
    # the febe DATA-wrap garbage (FEBE_NOISE), by the newest session's build
    ref_build = opts['--commit'] or (sessions[-1].build if sessions else None)
    febe = febe_noise_build(ROOT, ref_build) if ref_build else 'unknown'
    febe_dropped = 0
    if febe != 'no':
        kept = [ln for ln in ui if not FEBE_NOISE.search(ln.text)]
        febe_dropped = len(ui) - len(kept)
        ui = kept
    corr = load_corrections(opts['--corrections']) if opts['--corrections'] else []
    for s in sessions:
        s.corr = {}
        for f, sid, ans, why in corr:
            if f in ('*', s.rel, os.path.basename(s.rel)) and s.step(sid):
                s.corr[sid] = (ans, why)

    body = Out()
    anomalies = []
    if febe_dropped:
        body.head('ignored log lines')
        body('%d "fat empty slot / fat slot / ata barrier flush / ata flush" line%s left '
             'out: build %s %s the febe DATA-wrap bug (%s, fixed in %s), so they '
             'read garbage' % (febe_dropped, '' if febe_dropped == 1 else 's',
                               ref_build, 'has' if febe == 'yes' else 'may have',
                               FEBE_BUG, FEBE_FIX))
    for s in sessions:
        session_table(body, s)
        anomalies += anomalies_session(s, bcm, ui, context)
    # a last-resort fallback off the video chip (drift_jobs.c FALLBACK) is
    # a chip bug to fix: first in the block, so it can never hide one
    fb = [ln for ln in list(ui) + list(bcm) if 'FALLBACK ' in ln.text]
    if fb:
        anomalies.insert(0, ('FALLBACK: %d line%s, work left the video chip' % (
            len(fb), '' if len(fb) == 1 else 's'), [ln.show() for ln in fb[:8]] +
            (['... %d more' % (len(fb) - 8)] if len(fb) > 8 else [])))
    # a must-pass step that skipped costs the line silently: first of all
    needs = []
    for s in sessions:
        needs += need_skips(s, bcm)
    anomalies[0:0] = needs
    if not sessions:
        anomalies.append(('no results file', ['no file starting "session build=" at the '
                                              'top of %s or under bench/' % clean(args[0])]))

    # ---- readlog rows
    body.head('bench checks (bcmtest.log, readlog)')
    # the build whose checks count: --commit, else the newest session's
    ref = opts['--commit']
    if not ref and sessions:
        hexes = re.findall(r'[0-9a-f]{7,40}', sessions[-1].build or '')
        ref = hexes[-1] if hexes else None
    checks = bench_checks(bcm, ref)
    if not checks:
        body('no vector-check records (stage 4 / 4b did not run, or its lines were lost)')
    for label, v, where, old in checks:
        body('-- %s (%s)' % (label, clean(where)))
        for tag in CHECKS:
            if tag in v:
                state, text, ok = v[tag]
                body('  %-4s %-5s %s%s' % (tag, state, clean(text) or '-',
                                           '' if ok else '   <- NOT as modelled'))
                if old:
                    continue
                if not ok:
                    anomalies.append(('bench check %s (%s): %s' % (tag, label, state),
                                      [clean(text) or 'no verdict text', clean(where)]))
                elif state == 'stale':
                    anomalies.append(('bench check %s (%s): the image\'s expected value '
                                      'is older than the model' % (tag, label),
                                      [clean(text), clean(where), 'rebuild the bench '
                                       'image (gen_bench.py) so its flag agrees']))
        missing = [t for t in CHECKS if t not in v]
        if missing:
            body('  not in this build\'s records: %s' % ', '.join(missing))
        for tag in [t for t in v if t in ALGN_CHECKS or t not in CHECKS]:
            state, text, ok = v[tag]
            body('  %-4s %-5s %s%s' % (tag, state, clean(text) or '-',
                                       '' if ok else '   <- NOT as modelled'))
            if not ok and not old:
                anomalies.append(('bench row ALGN %s (%s): %s' % (tag, label, state),
                                  [clean(text) or 'no verdict text', clean(where)]))
    verdicts = merged_verdicts([(c[0], c[1]) for c in checks if not c[3]])

    body.head('video port (MB/s)')
    hits = [ln for ln in bcm + ui if PORT_ANY.search(ln.text)]
    for ln in hits:
        body('  ' + ln.show())
    if not hits:
        body('no port lines (stage 6 did not play, or the lines were lost)')

    body.head('J_VPU by image')
    hits = [ln for ln in bcm + ui if VPU.search(ln.text) and not AUTO.match(ln.text)]
    for s in sessions:
        for sid in ('jvpu', 'jvpu2', 'b5'):
            for t in s.logs.get(sid, []):
                if 'J_VPU' in t:
                    body('  %s %s: %s' % (s.rel, sid, clean(t)))
    for ln in hits:
        body('  ' + ln.show())
    for ln in bcm:
        m = IMG6.search(ln.text)
        if m:
            body('  s6 image: %s %s bytes fnv %s (%s:%d)' % (m.group(1), m.group(2), m.group(3),
                                                             ln.src, ln.n))
            break

    body.head('BCM starts: vpu, vpu2, load timings (ipodui.log, per power-up)')
    hits = [ln for ln in ui + bcm if BCM_START.search(ln.text) and not AUTO.match(ln.text)]
    for i, ln in enumerate(hits, 1):
        body('  start %d: %s (%s:%d)' % (i, BCM_START.search(ln.text).group(1), ln.src, ln.n))
    if not hits:
        body('no "bcmjob start" lines (a build before them, or the chip never came up)')

    body.head('vector forms and load timings (batch 4; information, no verdict)')
    n = 0
    for s in sessions:
        for sid in ('jvpu', 'jvpu2'):
            for t in s.logs.get(sid, []):
                if VPU2.search(t):
                    body('  %s %s: %s' % (s.rel, sid, clean(t)))
                    n += 1
    if not n:
        body('no lines (an image before batch 4, or jvpu did not run)')

    body.head('memory floor (ipodui.log)')
    fl = [ln for ln in ui if FLOOR.search(ln.text)]
    for ln in fl:
        body('  ' + ln.show())
    if not fl:
        body('no floor dev line')

    body.head('drift jobs (ipodui.log)')
    dj = [ln for ln in ui if DRIFTJ.search(ln.text) or 'bcmjob fault' in ln.text]
    for ln in dj:
        body('  ' + ln.show())
    if not dj:
        body('no drift or bcmjob fault lines')

    body.head('battery rows: slp doorbells, lit24ui missed frames')
    seen = set()
    for ln in bcm + ui:
        t = re.sub(r'^\s*\d+ ', '', clean(ln.text))
        if AUTO.match(ln.text) or not BATTERY.match(t) or (ln.src, ln.n) in seen:
            continue
        seen.add((ln.src, ln.n))
        body('  ' + ln.show())
    if not seen:
        body('no slp or lit24ui counter lines (the rows did not run)')

    body.head('stage pass flags (bcmpass*.txt)')
    flags = [n for n in pulled.listdir() if re.fullmatch(r'bcmpass\w*\.txt', n)]
    for n in flags:
        ls = pulled.lines(n)
        body('  %-14s %s' % (n, clean(ls[0]) if ls else '(empty)'))
    if not flags:
        body('none (no stage passed since the flags were cleared)')

    body.head('power.csv marks')
    marks = 0
    for n in pulled.generations('power', '.csv', 'end'):
        text = '\n'.join(pulled.lines(n))
        try:
            rows = list(csv.DictReader(io.StringIO(text)))
        except csv.Error as e:
            pulled.problems.append((n, 'not CSV: %s' % e))
            continue
        for row in rows:
            m = (row.get('mark') or '').strip()
            if m:
                marks += 1
                body('  %s s=%s mv=%s mark=%s' % (n, clean(row.get('s') or '?'),
                                                 clean(row.get('mv') or '?'), clean(m)))
    if not marks:
        body('no marks' if pulled.generations('power', '.csv', 'end') else 'no power.csv')

    src_core = None
    try:
        src_core = open(os.path.join(ROOT, 'firmware/target/arm/ipod/video/'
                                     'bcm-devtest-core.c')).read()
    except OSError:
        pass
    stale_band(body, sessions, bcm, ui, src_core)
    drift_still(body, pulled, bcm, ui, anomalies)
    video_clock(body, bcm, ui, anomalies)
    resident_panel(body, bcm, ui, anomalies)
    video_pace(body, bcm, ui, anomalies)
    visualizer(body, bcm, ui, anomalies)
    video_check(body, bcm, ui, anomalies)

    # ---- the verified line
    body.head('verified line')
    # the patch carries the replay's own line (built from parsed numbers and
    # a filtered build id), and only when the file's line is the same
    earned_s = [s for s in sessions if s.verified and s.earns and
                s.verified[-1] == s.computed]
    patch = None
    if not earned_s:
        for s in sessions:
            body('%s: no line: %s' % (s.rel, 'the session has not ended' if not s.st.ended
                                      else why_text(s) if not s.earns
                                      else 'the file has no verified line' if not s.verified
                                      else 'the file\'s line differs from the replay\'s: '
                                      'no patch (see the anomalies)'))
        if not sessions:
            body('no session')
    else:
        s = earned_s[-1]
        line = s.computed
        body(clean(line))
        m = re.match(r'bcmvideo_jobs ([0-9a-f]{8})  ', line)
        if not m:
            anomalies.append(('the verified line is not in the expected form', [clean(line)]))
        elif opts['--mkdevtest'] == 'none':
            body('patch: not made (--mkdevtest none)')
        else:
            evidence = 'bench %s session %s, build %s' % (
                'full' if s.st.mode == MODE_FULL else 're-verify', s.st.date or '-',
                re.sub(r'[^A-Za-z0-9-]', '', s.build or '') or 'x')
            path = opts['--patch'] or os.path.join(tempfile.gettempdir(), 'bench-ingest',
                                                   '%s-verified.patch' % m.group(1))
            try:
                diff, notes = make_patch(line, verdicts, evidence, opts['--mkdevtest'], path)
                patch = (path, diff, notes)
            except (RuntimeError, OSError) as e:
                anomalies.append(('the patch could not be made', [str(e)]))
    for f, what in pulled.problems:
        anomalies.append(('%s: %s' % (clean(f), what), []))

    out = Out()
    out('bench_ingest: %s (step table: %s)' % (clean(os.path.abspath(args[0])), rules.where))
    out.head('FAILURES AND ANOMALIES (%d)' % len(anomalies))
    if not anomalies:
        out('none')
    for title, det in anomalies:
        out('* ' + clean(title))
        for d in det:
            out('    ' + d)
    cs = [(s, sid, c) for s in sessions for sid, c in sorted(s.corr.items())]
    if cs:
        out.head('OWNER CORRECTIONS (%d)' % len(cs))
        for s, sid, (ca, why) in cs:
            r, ans = s.last().get(sid, (NONE, None, 0, None, 0))[:2]
            out('* %s %s: device answer %s, corrected to %s: %s' % (
                clean(s.rel), clean(sid), clean((ans or '-').replace('_', ' ')),
                clean(ca.replace('_', ' ')), clean(why)))
    sys.stdout.write(out.text())
    sys.stdout.write(body.text())
    if patch:
        path, diff, notes = patch
        print('\n== patch (not applied): %s ==' % path)
        for n in notes:
            print('note: %s' % clean(n))
        # the generated images file's diff is long only if the images
        # changed: show its stat and the rest in full
        for chunk in re.split(r'(?=^diff --git )', diff, flags=re.M):
            if 'bcm-devtest-images.c' in chunk.split('\n', 1)[0] and chunk.count('\n') > 60:
                print(chunk.split('\n', 1)[0])
                print('    (%d lines; see the patch file)' % chunk.count('\n'))
            else:
                sys.stdout.write('\n'.join(clean(x) for x in chunk.split('\n')))
    return 1 if anomalies else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
