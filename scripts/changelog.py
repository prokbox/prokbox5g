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

"""Build the changelog from the private history. Deterministic, no LLM.

usage: scripts/changelog.py [--version V] [--to REF] [--from REF] [--print V]
                            [--out FILE] [--fragments DIR] [--check] [--strict]

  (default)   rewrite docs/user/changelog.md (cumulative, newest first) and one
              fragment per version in docs/changelog/<ver>.md, the text
              release.yml appends to the GitHub release body
  --version V the newest version; its range ends at --to (default: tag vV if
              it exists, else HEAD).  Default: the newest docs/user/releases/*.md
  --print V   print only V's changes to stdout; write nothing
  --check     write nothing; exit 1 if the files on disk differ from a fresh run
  --strict    a commit prefix in no table is an error (the release check)

Versions come from docs/user/releases/<ver>.md (semver order).  Versions before
FIRST_VERSION are not listed (their range is not a clean one; the release notes
tell their story).

How the commits of version V are found (the public repo has only squashed
commits, so this runs here, in the private repo, before the release):
  end   = tag vV; else for the newest version --to / HEAD; else the newest
          commit on HEAD's history whose subject is
          "release: About says ProkBox5g V" or "release: ProkBox5g V", alone or
          followed by ";", ":" or "," (the version bump; "V-rc.N" never matches)
  start = the same lookup for the previous version in releases/
  commits = `git log --no-merges start..end`, subjects only.
A version whose end or start cannot be found is an error, never a guess.

The text of a commit is, in order of preference:
  1. a "Changelog: <user-facing text>" trailer in its message ("Changelog: none"
     drops the commit; "Changelog: [Library] text" picks the section) -- the
     normal way, written when the commit is made;
  2. a line "<sha-prefix>TAB<text or ->" in docs/changelog/overrides.tsv (the
     reviewed fix for commits made without a trailer; "-" drops the commit);
  3. its subject.
The prefix of the subject still picks the section unless the text says [Section].

Each subject "prefix: text (#NNN)" goes to the section its prefix maps to
(PREFIXES); developer-only work is dropped by rule (DROP_PREFIXES, DROP_WORDS);
a prefix in neither table is an error under --strict and goes to "Other
changes" otherwise.  Every kept line is checked against release_tree.py's own
scrub patterns and the run fails closed on a hit; a commit that cannot be
reworded can be listed in docs/changelog-exclude.txt ("SHA reason").
"""
import argparse, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)

FIRST_VERSION = '0.8.1'
OUT_DEFAULT = os.path.join(REPO, 'docs/user/changelog.md')
FRAG_DEFAULT = os.path.join(REPO, 'docs/changelog')
EXCLUDE_FILE = os.path.join(REPO, 'docs/changelog-exclude.txt')
OVERRIDE_FILE = os.path.join(REPO, 'docs/changelog/overrides.tsv')

# Section order is the order of this list; prefix -> section.
SECTIONS = ['Interface', 'Playback and sound', 'Library', 'Video', 'Storage and USB',
            'Power and start-up', 'Other changes']
PREFIXES = {
    'ipodui': 'Interface', 'ui': 'Interface', 'lang': 'Interface', 'fonts': 'Interface', 'art': 'Interface',
    'audiobook': 'Playback and sound', 'playback': 'Playback and sound', 'audio': 'Playback and sound',
    'dsp': 'Playback and sound', 'flac': 'Playback and sound', 'codec': 'Playback and sound',
    'codecs': 'Playback and sound', 'metadata': 'Playback and sound', 'wm8758': 'Playback and sound',
    'i2s': 'Playback and sound', 'chapters': 'Playback and sound',
    'tagcache': 'Library', 'library': 'Library', 'database': 'Library',
    'video': 'Video', 'bcm': 'Video', 'bcmfw': 'Video',
    'fat': 'Storage and USB', 'fs': 'Storage and USB', 'file': 'Storage and USB', 'ata': 'Storage and USB',
    'usb': 'Storage and USB', 'storage': 'Storage and USB', 'disk': 'Storage and USB',
    'firmware': 'Power and start-up', 'power': 'Power and start-up', 'boot': 'Power and start-up',
    'bootloader': 'Power and start-up', 'kernel': 'Power and start-up', 'cpu': 'Power and start-up',
    'sleep': 'Power and start-up', 'battery': 'Power and start-up', 'clock': 'Power and start-up',
}
# Developer-only prefixes: never shown to a reader.  (docs: release-note
# commits are dropped too; the notes themselves are on the Releases page.)
DROP_PREFIXES = {'tests', 'test', 'docs', 'doc', 'sim', 'wip', 'scripts', 'script', 'ci', 'gate',
                 'release', 'devbench', 'bench', 'dbgen', 'tools', 'build', 'emu', 'tooling', 'merge',
                 'revert', 'chore', 'refactor',
                 # 0.8.3 engine work: internal subsystems, user-facing lines come from trailers
                 'audio_cop', 'codec_cop', 'pcmbuf', 'buffering', 'pp', 'bcmjob', 'platform', 'audiophile'}
# Developer-only words anywhere in the subject (whole words, case-insensitive).
DROP_WORDS = re.compile(r'\b(tests?|devbench|gate|golden|goldens|fixtures?|stubs?|febe|accountant|align-ok|scantrace|sim-only|simulator|CI|lint)\b', re.I)
RELEASE_BUMP = ('release: About says ProkBox5g %s', 'release: ProkBox5g %s')


class Fail(Exception):
    pass


def git(*args):
    r = subprocess.run(['git', '-C', REPO] + list(args), capture_output=True, text=True)
    if r.returncode != 0:
        raise Fail('git %s failed: %s' % (' '.join(args), r.stderr.strip()))
    return r.stdout


def vkey(v):
    m = re.match(r'^(\d+)\.(\d+)\.(\d+)(?:-(.+))?$', v)
    if not m:
        raise Fail('not a version: %s' % v)
    pre = m.group(4)
    # a release sorts after its own alpha/beta/rc
    return (int(m.group(1)), int(m.group(2)), int(m.group(3)), 1 if pre is None else 0, pre or '')


def versions():
    d = os.path.join(REPO, 'docs/user/releases')
    vs = [f[:-3] for f in os.listdir(d) if f.endswith('.md') and f != '_index.md']
    if not vs:
        raise Fail('no docs/user/releases/<ver>.md files found')
    return sorted(vs, key=vkey)


def commit_of(version, fallback=None):
    """The commit that ends `version`'s range: its tag, else `fallback` (a ref),
    else the version-bump commit.  -> (sha, how)"""
    tag = 'v' + version
    r = subprocess.run(['git', '-C', REPO, 'rev-parse', '-q', '--verify', 'refs/tags/%s^{commit}' % tag],
                       capture_output=True, text=True)
    if r.returncode == 0:
        return r.stdout.strip(), 'tag ' + tag
    if fallback:
        return git('rev-parse', fallback + '^{commit}').strip(), fallback
    for subj in RELEASE_BUMP:
        want = subj % version
        for l in git('log', '--format=%H %s', '--fixed-strings', '--grep', want, 'HEAD').splitlines():
            h, s = l.split(' ', 1)
            if s == want or re.match(re.escape(want) + r'[;:,]', s):
                return h, 'commit "%s"' % want
    raise Fail('no end commit for %s: no tag %s and no commit "%s"' % (version, tag, RELEASE_BUMP[0] % version))


def excluded():
    ex = {}
    if os.path.exists(EXCLUDE_FILE):
        for l in open(EXCLUDE_FILE):
            l = l.strip()
            if l and not l.startswith('#'):
                sha, _, why = l.partition(' ')
                ex[sha] = why
    return ex


def overrides():
    """{full sha: text or None (drop)} from docs/changelog/overrides.tsv."""
    ov = {}
    if os.path.exists(OVERRIDE_FILE):
        for n, l in enumerate(open(OVERRIDE_FILE), 1):
            l = l.rstrip('\n')
            if not l.strip() or l.lstrip().startswith('#'):
                continue
            sha, tab, text = l.partition('\t')
            sha, text = sha.strip(), text.strip()
            if not tab or not re.match(r'^[0-9a-f]{7,40}$', sha) or not text:
                raise Fail('%s:%d: want "<sha>TAB<text or ->"' % (OVERRIDE_FILE, n))
            r = subprocess.run(['git', '-C', REPO, 'rev-parse', '-q', '--verify', sha + '^{commit}'],
                               capture_output=True, text=True)
            if r.returncode != 0:
                raise Fail('%s:%d: no commit %s' % (OVERRIDE_FILE, n, sha))
            ov[r.stdout.strip()] = None if text == '-' else text
    return ov


def trailer(body):
    """The last "Changelog:" trailer line of a commit message, else None."""
    found = None
    for l in body.splitlines():
        m = re.match(r'^Changelog:[ \t]*(.*\S)\s*$', l)
        if m:
            found = m.group(1)
    return found


def scrub_patterns():
    import release_tree
    return release_tree.FAIL_CLOSED_RE, re.compile('|'.join(release_tree.SCRUB), re.I)


def classify(subject):
    """-> (section or None to drop, text, prefix)"""
    m = re.match(r'^([^:]{1,40}):\s*(.*)$', subject)
    if not m:
        return 'Other changes', subject, ''
    pre, text = m.group(1).strip(), m.group(2).strip()
    parts = [p.strip().lower() for p in pre.split(',')]
    if any(p in DROP_PREFIXES or p.split(' ')[0] in DROP_PREFIXES for p in parts):
        return None, text, pre
    if DROP_WORDS.search(subject):
        return None, text, pre
    for p in parts:
        if p in PREFIXES:
            return PREFIXES[p], text, pre
    return 'Other changes', text, pre


def changes_between(start, end, strict=False):
    """-> ({section: [line]}, stats).  Fails closed on a scrub hit."""
    fc, sc = scrub_patterns()
    ex = excluded()
    raw = git('log', '--no-merges', '--reverse', '--format=%H%x1f%B%x1e', '%s..%s' % (start, end))
    lines = [c.strip('\n').split('\x1f', 1) for c in raw.split('\x1e') if c.strip()]
    if not lines:
        raise Fail('empty commit range %s..%s' % (start[:10], end[:10]))
    ov = overrides()
    out = {s: [] for s in SECTIONS}
    stats = {'total': len(lines), 'kept': 0, 'dropped': 0, 'unmapped': {}, 'trailer': 0, 'override': 0}
    hits = []
    for sha, body in lines:
        subj = body.split('\n', 1)[0]
        if any(sha.startswith(k) for k in ex):
            stats['dropped'] += 1
            continue
        section, text, pre = classify(subj)
        given, how = trailer(body), 'trailer'
        if given is None and sha in ov:
            given, how = ov[sha], 'override'
            if given is None:
                given = 'none'
        if given is not None:
            stats[how] += 1
            if given.lower() == 'none':
                stats['dropped'] += 1
                continue
            m = re.match(r'^\[([^\]]+)\]\s*(.+)$', given)
            if m:
                if m.group(1) not in SECTIONS:
                    raise Fail('%s: section [%s] is not one of: %s' % (sha[:10], m.group(1), ', '.join(SECTIONS)))
                section, text = m.group(1), m.group(2)
            else:
                text = given
                if section is None:      # a developer prefix, but someone wrote user text for it
                    section = 'Other changes'
        if section is None:
            stats['dropped'] += 1
            continue
        if section == 'Other changes' and given is None:
            k = pre or '<no prefix>'
            stats['unmapped'][k] = stats['unmapped'].get(k, 0) + 1
        text = text[:1].upper() + text[1:] if text else subj
        text = text.rstrip('. ')
        if fc.search(text) or sc.search(text):
            hits.append('%s %s' % (sha[:10], subj))
            continue
        if text not in out[section]:
            out[section].append(text)
        stats['kept'] += 1
    if hits:
        raise Fail('scrub: %d commit subject(s) match the release scrub patterns (history cannot be '
                   'reworded; list the SHA in docs/changelog-exclude.txt):\n  %s' % (len(hits), '\n  '.join(hits)))
    if strict and stats['unmapped']:
        raise Fail('commit prefixes in neither PREFIXES nor DROP_PREFIXES (scripts/changelog.py): %s'
                   % ', '.join('%s (%d)' % kv for kv in sorted(stats['unmapped'].items())))
    return out, stats


def render_section(sections, level='###'):
    parts = []
    for s in SECTIONS:
        if sections.get(s):
            parts.append('%s %s\n\n%s\n' % (level, s, '\n'.join('- ' + t for t in sections[s])))
    return '\n'.join(parts)


def build(newest=None, to=None, frm=None, strict=False):
    allv = versions()
    vs = [v for v in allv if vkey(v) >= vkey(FIRST_VERSION)]
    if newest:
        if newest not in vs:
            raise Fail('version %s has no docs/user/releases/%s.md (or is before %s)' % (newest, newest, FIRST_VERSION))
    else:
        newest = vs[-1]
    vs = [v for v in vs if vkey(v) <= vkey(newest)]
    res = {}
    for v in vs:
        prev = allv[allv.index(v) - 1]
        end, how_end = commit_of(v, (to or 'HEAD') if v == newest else None)
        start, how_start = (frm, 'explicit') if (frm and v == newest) else commit_of(prev)
        sections, stats = changes_between(start, end, strict)
        res[v] = dict(sections=sections, stats=stats, prev=prev, start=start, end=end,
                      how='%s .. %s' % (how_start, how_end))
    return res, vs


def fragment(v, d):
    return ('## Changes since %s\n\n%s\nThis list is built from the project history; the notes above say what '
            'matters most.\n' % (d['prev'], render_section(d['sections'], '###')))


def changelog_md(res, vs):
    o = ['---\ntitle: "Changelog"\ndescription: "Every change in each ProkBox5g release, built from the project history."\n---\n',
         'Every release, newest first. The [release notes](../releases/) say what matters most; this page lists '
         'what changed between versions. Numbers like #123 refer to the project tracker.\n']
    for v in reversed(vs):
        d = res[v]
        o.append('## %s\n\n_Changes since %s._\n\n%s' % (v, d['prev'], render_section(d['sections'], '###')))
    o.append('Releases before %s: see their [release notes](../releases/).\n' % FIRST_VERSION)
    return '\n'.join(o)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--version'); ap.add_argument('--to'); ap.add_argument('--from', dest='frm')
    ap.add_argument('--print', dest='prt'); ap.add_argument('--out', default=OUT_DEFAULT)
    ap.add_argument('--fragments', default=FRAG_DEFAULT); ap.add_argument('--check', action='store_true')
    ap.add_argument('--strict', action='store_true')
    a = ap.parse_args()
    try:
        res, vs = build(a.version or a.prt, a.to, a.frm, a.strict)
        if a.prt:
            d = res[a.prt]
            print('# %s: %s..%s (%s) kept %d of %d' % (a.prt, d['start'][:10], d['end'][:10], d['how'],
                  d['stats']['kept'], d['stats']['total']))
            if d['stats']['unmapped']:
                print('# unmapped prefixes (listed under Other changes): %s' % d['stats']['unmapped'])
            print(render_section(d['sections'], '##'))
            return 0
        files = {a.out: changelog_md(res, vs)}
        for v in vs:
            files[os.path.join(a.fragments, v + '.md')] = fragment(v, res[v])
        if a.check:
            bad = [p for p, t in files.items() if not os.path.exists(p) or open(p).read() != t]
            if bad:
                print('changelog: out of date (run scripts/changelog.py): ' + ', '.join(os.path.relpath(p, REPO) for p in bad))
                return 1
            print('changelog: up to date (%d versions: %s)' % (len(vs), ', '.join(vs)))
            return 0
        os.makedirs(a.fragments, exist_ok=True)
        for p, t in files.items():
            open(p, 'w').write(t)
        for v in vs:
            s = res[v]['stats']
            print('%s: %s, kept %d of %d commits (%d dropped)%s' % (
                v, res[v]['how'], s['kept'], s['total'], s['dropped'],
                '; unmapped prefixes: %s' % s['unmapped'] if s['unmapped'] else ''))
        return 0
    except Fail as e:
        print('changelog: FAILED: %s' % e, file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
