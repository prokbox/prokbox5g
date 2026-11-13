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

"""The docs half of a release: one version, checked and prepared in one place.

usage: scripts/release_docs.py prepare [VER]    regenerate the changelog (needs the
                                                private history) and the roadmap page (needs the tracker)
       scripts/release_docs.py check [VER] [--tree DIR] [--fresh] [--rc] [--site SITE]
       scripts/release_docs.py images [--tree DIR]       R11 only (scripts/docs_shots.sh --check)
       scripts/release_docs.py versions SITE [--tags "vX.Y.Z ..."] [--repo DIR] [--require-pdf]

VER defaults to resolve_version(): scripts/version.sh --tag-only (the release
tag), so nothing hand-types a version.  While version.sh is absent,
resolve_version() returns None and VER is required (the one place to wire it).

`check` refuses (exit 1, one line per problem naming the rule and the value)
unless, in DIR (default: this repo; release_import.py passes the exported tree):
  R1  docs/user/releases/VER.md exists
  R2  it is not `draft: true` (Hugo skips drafts, so the site would never show
      the release; --rc allows a draft for a release candidate)
  R3  site/hugo.toml carries no hand-typed release version (its `version` is a dev
      default; the real one comes from scripts/version.sh, i.e. the tag)
  R4  docs/user/changelog.md has a "## VER" section with at least one entry
  R5  docs/changelog/VER.md exists (appended to the GitHub release by release.yml)
  R7  docs/adr passes scripts/adr_check.py (unique numbers, complete index)
  R8  docs/user/roadmap.md exists, was generated for VER (roadmap_version), lists entries and
      passes the release scrub (scripts/roadmap.py check), and its data file for the home
      page's lanes, site/data/roadmap.json, was made for VER and names the same entries;
      `prepare` regenerates both from the tracker, so it needs the forge (LAN only) and the
      result is committed
  R10 README.md, the landing page on GitHub: a "## Contents" list right after the preamble
      whose every entry names a heading (and every section is listed, in order); exactly
      one "**Current release: ..." line in the preamble, naming VER and no other version;
      every relative link and image exists in DIR (in the private tree also: not dropped
      by release_tree.py and under a path the export always keeps); every link into the
      site (site/hugo.toml baseURL) is a page the site builds (from its Markdown sources,
      or from a built site with --site SITE), with the #heading it names
  R11 every image under docs/user/images/, site/static/ and site/assets/ has a row in
      tests/ui/docs/images.tsv naming its generator (a sim script and the shot it takes,
      or a script), every row's image and generator exist (scripts/docs_shots.sh)
  R12 the notes page's front-matter `date:` is a real date (when present) and not later than today (the day the
      check runs; --today YYYY-MM-DD overrides it for tests).  Hugo leaves out a page dated in
      the future, so the release's page and its entry in the releases list would be missing
      from the site until that day.  The date is part of the tagged tree, so it cannot be set
      "at publish time": fix it before the tag, or after it with a docs patch
      (site/versions/patches/vX.Y.Z/, release-plan 3.3a)
  R6  --fresh only: docs/user/changelog.md and the fragments equal a fresh run of
      scripts/changelog.py --strict (needs git history, so private repo only)
VER is the release version without "-rc.N" (the notes page of an RC is its base's).

`versions` checks a versioned site built by scripts/docs_versions.py (R9):
  R9  the version picker's list (SITE/versions.json) and the built folders match the
      release tags vX.Y.Z >= v0.8.0 (default: every such tag in --repo): each tag is
      listed with a folder, or skipped with a reason; nothing else is listed or built;
      newest first, the newest is "latest" and sits at the root; every listed page
      exists; every HTML page carries the picker; a version folder's living pages
      (What's coming, changelog) send the reader to the root copy; dev/ pages carry
      the "not released yet" banner; --require-pdf: each version has its PDF.
"""
import argparse, datetime, json, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)


def front_matter(path):
    t = open(path).read()
    m = re.match(r'^---\n(.*?)\n---\n', t, re.S)
    return m.group(1) if m else ''


def gh_slug(text):
    """The anchor GitHub (and Hugo's default "github" heading IDs) give a heading."""
    t = re.sub(r'`([^`]*)`', r'\1', text)
    t = re.sub(r'!?\[([^\]]*)\]\([^)]*\)', r'\1', t)
    t = re.sub(r'<[^>]+>', '', t).strip().lower()
    return re.sub(r'[^\w\- ]', '', t).replace(' ', '-')


def md_headings(text):
    """[(level, text, slug)] of the ATX headings outside code fences; GitHub numbers repeats -1, -2."""
    out, seen, fence = [], {}, False
    for line in text.split('\n'):
        if re.match(r'^\s*(```|~~~)', line):
            fence = not fence
            continue
        m = None if fence else re.match(r'^(#{1,6})\s+(.*?)\s*#*\s*$', line)
        if m:
            s = gh_slug(m.group(2))
            n = seen.get(s, 0)
            seen[s] = n + 1
            out.append((len(m.group(1)), m.group(2), s if n == 0 else '%s-%d' % (s, n)))
    return out


def md_links(text):
    """Every link and image target in Markdown/HTML text, outside code fences and code spans."""
    body = re.sub(r'(?ms)^\s*(```|~~~).*?^\s*\1[^\n]*$', '', text)
    body = re.sub(r'`[^`\n]*`', '', body)
    t = [m.group(1) for m in re.finditer(r'!?\[[^\]]*\]\(\s*<?([^)\s>]+)>?(?:\s+"[^"]*")?\s*\)', body)]
    t += [m.group(1) for m in re.finditer(r'<(https?://[^>\s]+)>', body)]
    t += [m.group(2) for m in re.finditer(r'''\b(src|href)\s*=\s*["']([^"']+)["']''', body)]
    return t


def _hugo_mounts(root):
    """[(content prefix, source dir)] from site/hugo.toml's content mounts, longest prefix first."""
    try:
        toml = open(os.path.join(root, 'site/hugo.toml')).read()
    except OSError:
        return []
    out = []
    for src, tgt in re.findall(r'source\s*=\s*"([^"]+)"\s*\n\s*target\s*=\s*"([^"]+)"', toml):
        if tgt == 'content' or tgt.startswith('content/'):
            pre = tgt[len('content'):].strip('/')
            out.append((pre, os.path.normpath(os.path.join(root, 'site', src))))
    return sorted(out, key=lambda x: -len(x[0]))


def site_page_source(root, path):
    """The Markdown file Hugo makes the page PATH (no leading or trailing /) from, or None."""
    for pre, src in _hugo_mounts(root):
        if path == pre or path.startswith(pre + '/') or not pre:
            rest = path[len(pre):].strip('/') if pre else path
            base = os.path.join(src, rest) if rest else src
            for c in (base + '.md', os.path.join(base, '_index.md'), os.path.join(base, 'index.md')):
                if os.path.isfile(c):
                    return c
    return None


# README sections a release's README must keep (the owner's landing page, 2026-11-12)
README_STATUS = re.compile(r'^\*\*Current release:', re.M)
VERSION_TOKEN = re.compile(r'\b\d+\.\d+\.\d+(?:-[0-9A-Za-z.]+)?\b')


def check_readme(root, ver, site=None):
    """R10: README.md, the landing page on GitHub, is current and its links resolve.
    -> [problem strings].  SITE: a built site to check site links against (else the
    Markdown sources the site is built from)."""
    p = []
    path = os.path.join(root, 'README.md')
    if not os.path.isfile(path):
        return ['R10 README.md missing']
    text = open(path, encoding='utf-8').read()
    heads = md_headings(text)
    h2 = [h for h in heads if h[0] == 2]
    # (a) a table of contents right after the preamble
    m1 = re.search(r'^# .*$', text, re.M)
    m2 = re.search(r'^## .*$', text, re.M)
    if not m1 or not m2 or m2.start() < m1.start():
        return p + ['R10 README.md needs a "# " title, a preamble, then "## Contents"']
    preamble = text[m1.end():m2.start()]
    if not h2 or h2[0][1] != 'Contents':
        p.append('R10 README.md: the first section after the preamble must be "## Contents" (found "%s")'
                 % (h2[0][1] if h2 else 'none'))
    if not preamble.strip():
        p.append('R10 README.md has no preamble between the title and the contents')
    nxt = re.search(r'^## .*$', text[m2.end():], re.M)
    toc = text[m2.end():m2.end() + nxt.start()] if nxt else text[m2.end():]
    entries = re.findall(r'^\s*[-*]\s+\[([^\]]+)\]\(#([^)]+)\)\s*$', toc, re.M)
    if not entries:
        p.append('R10 README.md "## Contents" lists no [entry](#anchor) lines')
    stray = [l for l in toc.split('\n') if l.strip() and not re.match(r'^\s*[-*]\s+\[[^\]]+\]\(#[^)]+\)\s*$', l)]
    if stray:
        p.append('R10 README.md "## Contents" has a line that is not a contents entry: "%s"' % stray[0].strip())
    slugs = {h[2]: h for h in heads}
    for label, anchor in entries:
        if anchor not in slugs:
            p.append('R10 README.md contents entry "%s" points at #%s, which is no heading' % (label, anchor))
        elif slugs[anchor][1] != label:
            p.append('R10 README.md contents entry "%s" points at the heading "%s"' % (label, slugs[anchor][1]))
    want = [h[2] for h in h2[1:]]
    got = [a for _, a in entries if a in slugs and slugs[a][0] == 2]
    for h in h2[1:]:
        if h[2] not in got:
            p.append('R10 README.md section "%s" is missing from the contents' % h[1])
    if [a for a in got if a in want] != [a for a in want if a in got]:
        p.append('R10 README.md contents are not in the order of the sections')
    # (d) the release status line names the release being checked
    st = [l for l in preamble.split('\n') if README_STATUS.match(l)]
    if len(st) != 1:
        p.append('R10 README.md preamble needs exactly one "**Current release: ..." line (found %d)' % len(st))
    else:
        vs = VERSION_TOKEN.findall(st[0])
        if vs != [ver]:
            p.append('R10 README.md status line names %s, not the release being checked, %s: "%s"'
                     % (', '.join(vs) or 'no version', ver, st[0].strip()[:100]))
    # (b) relative links and images resolve in the (exported) tree; (c) site links
    try:
        sys.path.insert(0, HERE)
        import release_tree as rt  # private tree only: it is dropped from the export
    except ImportError:
        rt = None
    base = None
    tm = re.search(r'^\s*baseURL\s*=\s*"([^"]+)"', open(os.path.join(root, 'site/hugo.toml')).read(), re.M) \
        if os.path.isfile(os.path.join(root, 'site/hugo.toml')) else None
    if tm:
        base = tm.group(1).rstrip('/') + '/'
    nrel = nsite = 0
    for target in md_links(text):
        if target.startswith('#'):
            continue
        if base and target.startswith(base):
            nsite += 1
            page, _, frag = target[len(base):].partition('#')
            page = page.split('?')[0].strip('/')
            if site:
                f = os.path.join(site, page)          # a file (the PDF) or a page's folder
                if not (page and os.path.isfile(f)):
                    f = os.path.join(site, page, 'index.html')
                if not os.path.isfile(f):
                    p.append('R10 README.md links to %s, which the built site %s does not have' % (target, site))
                elif frag and not re.search(r'''\bid=["']?%s(["'\s>])''' % re.escape(frag), open(f, encoding='utf-8').read()):
                    p.append('R10 README.md links to %s: the page has no #%s' % (target, frag))
            else:
                src = site_page_source(root, page)
                if not src:
                    p.append('R10 README.md links to %s, but no page in the site sources makes /%s/' % (target, page))
                elif frag and frag not in {h[2] for h in md_headings(open(src, encoding='utf-8').read())}:
                    p.append('R10 README.md links to %s: %s has no heading #%s' % (target, os.path.relpath(src, root), frag))
            continue
        if re.match(r'^[a-z][a-z0-9+.-]*:', target, re.I):
            continue    # another site, or mailto:
        nrel += 1
        rel = os.path.normpath(target.split('#')[0].split('?')[0]).replace(os.sep, '/')
        if rel.startswith('../') or os.path.isabs(rel):
            p.append('R10 README.md links outside the tree: %s' % target)
        elif not os.path.exists(os.path.join(root, rel)):
            p.append('R10 README.md links to %s, which is not in the tree' % target)
        elif rt and (rt.match_any(rel, rt.DROP) or any(rt.match_any(f, rt.DROP) for f in _files_under(root, rel))):
            p.append('R10 README.md links to %s, which the public export drops (release_tree.py DROP)' % target)
        elif rt and '/' in rel and not (rel + ('/' if os.path.isdir(os.path.join(root, rel)) else '')).startswith(
                tuple(rt.KEEP_DIRS)) and not rt.LICENCE_NAME.search(rel):
            p.append('R10 README.md links to %s, outside the paths the public export always keeps '
                     '(release_tree.py KEEP_DIRS); it may be stripped' % target)
    if nrel == 0:
        p.append('R10 README.md has no relative links or images: nothing was checked (expected the screenshots)')
    if nsite == 0:
        p.append('R10 README.md has no links to the site %s' % (base or '(no baseURL in site/hugo.toml)'))
    return p


def _files_under(root, rel):
    d = os.path.join(root, rel)
    if not os.path.isdir(d):
        return []
    return [os.path.relpath(os.path.join(a, f), root).replace(os.sep, '/') for a, _, fs in os.walk(d) for f in fs]


IMAGES_TSV = 'tests/ui/docs/images.tsv'
IMAGE_EXT = ('.png', '.gif', '.jpg', '.jpeg', '.svg', '.webp')
IMAGE_DIRS = ('docs/user/images', 'site/static', 'site/assets')


def read_images_tsv(root):
    """[(line number, image, generator, shot, scale)] from tests/ui/docs/images.tsv."""
    rows = []
    for n, line in enumerate(open(os.path.join(root, IMAGES_TSV), encoding='utf-8'), 1):
        line = line.rstrip('\n')
        if not line.strip() or line.lstrip().startswith('#'):
            continue
        f = line.split('\t')
        rows.append((n, f[0], f[1] if len(f) > 1 else '', f[2] if len(f) > 2 else '', f[3] if len(f) > 3 else ''))
    return rows


def check_images(root):
    """R11: every image the docs and the site ship has a generator listed in
    tests/ui/docs/images.tsv (scripts/docs_shots.sh remakes them), and every row is real."""
    p = []
    if not os.path.isfile(os.path.join(root, IMAGES_TSV)):
        return ['R11 %s missing (the list of doc images and the scripts that make them)' % IMAGES_TSV]
    rows = read_images_tsv(root)
    found = set()
    for d in IMAGE_DIRS:
        for f in _files_under(root, d):
            if f.lower().endswith(IMAGE_EXT):
                found.add(f)
    if not found:
        p.append('R11 no images found under %s: nothing was checked' % ', '.join(IMAGE_DIRS))
    listed = {}
    for n, img, gen, shot, scale in rows:
        where = '%s:%d' % (IMAGES_TSV, n)
        if img in listed:
            p.append('R11 %s lists %s twice (also line %d)' % (where, img, listed[img]))
        listed[img] = n
        if not os.path.isfile(os.path.join(root, img)):
            p.append('R11 %s lists %s, which does not exist' % (where, img))
        if not gen or not os.path.isfile(os.path.join(root, gen)):
            p.append('R11 %s: %s has no generator, or its generator "%s" does not exist' % (where, img, gen))
        elif gen.endswith('.script'):
            if not shot or not re.search(r'^(video_)?shot\s+%s\b' % re.escape(shot),
                                         open(os.path.join(root, gen)).read(), re.M):
                p.append('R11 %s: %s never takes "shot %s"' % (where, gen, shot))
            if scale not in ('1', '2'):
                p.append('R11 %s: scale must be 1 or 2, not "%s"' % (where, scale))
    for f in sorted(found - set(listed)):
        p.append('R11 %s has no generator: add a row to %s (image, script, shot, scale)' % (f, IMAGES_TSV))
    return p


def check_tree(root, ver, fresh=False, rc=False, site=None, today=None):
    """-> [problem strings]; empty means the docs are release-ready."""
    p = []
    rel = os.path.join(root, 'docs/user/releases/%s.md' % ver)
    if not os.path.isfile(rel):
        p.append('R1 release notes missing: docs/user/releases/%s.md' % ver)
    elif re.search(r'^draft:\s*true\s*$', front_matter(rel), re.M) and not rc:
        p.append('R2 docs/user/releases/%s.md is still "draft: true" (Hugo would not publish it)' % ver)
    if os.path.isfile(rel):
        m = re.search(r'^date:\s*["\']?([^\s"\']+)', front_matter(rel), re.M)
        today = today or datetime.date.today().isoformat()
        try:
            d = datetime.date.fromisoformat(m.group(1)[:10]) if m else None
        except ValueError:
            d = None
        if m and d is None:
            p.append('R12 docs/user/releases/%s.md has `date: %s`, which is not YYYY-MM-DD' % (ver, m.group(1)))
        elif d and d.isoformat() > today:
            p.append('R12 docs/user/releases/%s.md is dated %s, later than today (%s): Hugo would leave the page '
                     'out of the site.  Set `date:` to the publication day (a released tag: use a docs patch, '
                     'site/versions/patches/v%s/docs/user/releases/%s.md)' % (ver, d.isoformat(), today, ver, ver))
    toml = os.path.join(root, 'site/hugo.toml')
    if not os.path.isfile(toml):
        p.append('R3 site/hugo.toml missing')
    else:
        m = re.search(r'^\s*version\s*=\s*"([^"]*)"', open(toml).read(), re.M)
        if m and re.match(r'^\d+\.\d+\.\d+', m.group(1)):
            p.append('R3 site/hugo.toml has a hand-typed version "%s"; the version comes from scripts/version.sh '
                     '(set it to "dev")' % m.group(1))
    cl = os.path.join(root, 'docs/user/changelog.md')
    if not os.path.isfile(cl):
        p.append('R4 docs/user/changelog.md missing (run scripts/release_docs.py prepare %s)' % ver)
    else:
        t = open(cl).read()
        m = re.search(r'^## %s\n(.*?)(?=^## |\Z)' % re.escape(ver), t, re.S | re.M)
        if not m:
            p.append('R4 docs/user/changelog.md has no "## %s" section' % ver)
        elif not re.search(r'^- ', m.group(1), re.M):
            p.append('R4 docs/user/changelog.md section %s has no entries' % ver)
    fr = os.path.join(root, 'docs/changelog/%s.md' % ver)
    if not os.path.isfile(fr) or not open(fr).read().strip():
        p.append('R5 docs/changelog/%s.md missing or empty' % ver)
    sys.path.insert(0, HERE)
    import adr_check, roadmap
    ap_, n_ = adr_check.check(root)
    p += ['R7 ' + q for q in ap_]
    p += roadmap.check_page(root, ver)
    p += check_readme(root, ver, site)
    p += check_images(root)
    if fresh:
        r = subprocess.run([sys.executable, os.path.join(HERE, 'changelog.py'), '--check', '--strict',
                            '--version', ver], capture_output=True, text=True)
        if r.returncode != 0:
            p.append('R6 changelog is stale or failed: %s' % (r.stdout + r.stderr).strip().replace('\n', ' | ')[:400])
    return p


VTAG = re.compile(r'^v(\d+)\.(\d+)\.(\d+)$')
VFLOOR = (0, 8, 0)
VPDF = 'downloads/ProkBox5g-User-Guide.pdf'


def _vkey(v):
    return tuple(int(x) for x in v.split('.'))


def check_versions(site, tags, require_pdf=False):
    """R9 -> ([problem strings], number of HTML pages checked for the picker)."""
    p = []
    vj = os.path.join(site, 'versions.json')
    if not os.path.isfile(vj):
        return ['R9 %s missing (scripts/docs_versions.py writes it)' % vj], 0
    try:
        v = json.load(open(vj))
        listed = [x['id'] for x in v['versions']]
        skipped = {s['tag'][1:]: s.get('why', '') for s in v.get('skipped', [])}
    except (ValueError, KeyError, TypeError) as e:
        return ['R9 versions.json is not a version list: %s' % e], 0
    want = sorted({t[1:] for t in tags if VTAG.match(t) and tuple(int(x) for x in VTAG.match(t).groups()) >= VFLOOR},
                  key=_vkey, reverse=True)
    if not want:
        p.append('R9 no release tag vX.Y.Z >= v0.8.0 to check against')
    for w in want:
        if w in listed and w in skipped:
            p.append('R9 %s is both in the picker and skipped' % w)
        elif w in skipped and not skipped[w].strip():
            p.append('R9 tag v%s is skipped without a reason' % w)
        elif w not in listed and w not in skipped:
            p.append('R9 tag v%s is neither in the version picker nor skipped with a reason' % w)
    for x in listed:
        if x not in want:
            p.append('R9 the version picker lists %s, which is not a release tag' % x)
    for x in skipped:
        if x not in want:
            p.append('R9 versions.json skips v%s, which is not a release tag' % x)
    if listed != sorted(listed, key=_vkey, reverse=True):
        p.append('R9 the version picker is not newest first: %s' % listed)
    if listed and v.get('latest') != listed[0]:
        p.append('R9 "latest" is %s but the newest listed version is %s' % (v.get('latest'), listed[0]))
    if want and listed and listed[0] != want[0]:
        p.append('R9 the newest release v%s is not the newest version on the site (%s)' % (want[0], listed[0]))
    folders = sorted((d for d in os.listdir(site) if re.match(r'^\d+\.\d+\.\d+$', d)
                      and os.path.isdir(os.path.join(site, d))), key=_vkey, reverse=True)
    if folders != sorted(listed, key=_vkey, reverse=True):
        p.append('R9 the version folders %s differ from the version picker %s' % (folders, listed))
    has_dev = os.path.isdir(os.path.join(site, 'dev'))
    if has_dev != bool(v.get('dev')):
        p.append('R9 dev/ %s but versions.json says %s' % ('exists' if has_dev else 'is missing',
                                                          'dev' if v.get('dev') else 'no dev'))
    living = v.get('living', [])
    entries = [('', (v.get('root') or {}).get('pages'))] + [(x['id'] + '/', x.get('pages')) for x in v['versions']]
    if v.get('dev'):
        entries.append(('dev/', v['dev'].get('pages')))
    for pre, pg in entries:
        if not pg or '' not in pg:
            p.append('R9 %s has no page list with a home page in versions.json' % (pre or 'the root'))
            continue
        for q in pg:
            if not os.path.isfile(os.path.join(site, pre + q, 'index.html')):
                p.append('R9 versions.json lists %s%s but it was not built' % (pre, q))
        if pre:
            for q in living:
                f = os.path.join(site, pre + q, 'index.html')
                if os.path.isfile(f) and 'data-pbv-living' not in open(f, encoding='utf-8').read():
                    p.append('R9 %s%s is a frozen copy of a living page (it must send readers to the root)' % (pre, q))
    for q in living:
        f = os.path.join(site, q, 'index.html')
        if not os.path.isfile(f) or 'data-pbv-living' in open(f, encoding='utf-8').read():
            p.append('R9 the living page %s is missing at the root' % q)
    if require_pdf:
        for x in v['versions']:
            if not os.path.isfile(os.path.join(site, x['id'], VPDF)):
                p.append('R9 %s has no PDF guide (%s/%s)' % (x['id'], x['id'], VPDF))
    n, nopick = 0, []
    for d, dirs, files in os.walk(site):
        rel = os.path.relpath(d, site).replace(os.sep, '/')
        if '/pagefind' in '/' + rel or rel.startswith('_versions'):
            continue
        for f in files:
            if not f.endswith('.html'):
                continue
            t = open(os.path.join(d, f), encoding='utf-8').read()
            n += 1
            if 'data-pbv-picker' not in t:
                nopick.append(os.path.join(rel, f))
            if (rel == 'dev' or rel.startswith('dev/')) and 'data-pbv-dev' not in t and 'data-pbv-living' not in t:
                p.append('R9 dev page %s/%s has no "not released yet" banner' % (rel, f))
    if nopick:
        p.append('R9 %d page(s) without the version picker, e.g. %s' % (len(nopick), sorted(nopick)[:3]))
    if n == 0:
        p.append('R9 no HTML pages under %s' % site)
    return p, n


def resolve_version(tag_only=True):
    """The release version from scripts/version.sh; None if the script is absent."""
    sh = os.path.join(HERE, 'version.sh')
    if not os.path.exists(sh):
        return None
    r = subprocess.run(['sh', sh] + (['--tag-only'] if tag_only else []), capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit('release_docs: scripts/version.sh failed (HEAD not at a release tag?): %s' % r.stderr.strip())
    return r.stdout.strip()


def prepare(ver):
    rc = subprocess.run([sys.executable, os.path.join(HERE, 'changelog.py'), '--strict', '--version', ver]).returncode
    rc2 = subprocess.run([sys.executable, os.path.join(HERE, 'roadmap.py'), 'generate', ver]).returncode
    return rc or rc2


def main_versions(argv):
    ap = argparse.ArgumentParser(prog='release_docs.py versions')
    ap.add_argument('site')
    ap.add_argument('--tags', help='space-separated release tags (default: every tag in --repo)')
    ap.add_argument('--repo', default=REPO)
    ap.add_argument('--require-pdf', action='store_true')
    a = ap.parse_args(argv)
    if a.tags is not None:
        tags = a.tags.split()
    else:
        r = subprocess.run(['git', '-C', a.repo, 'tag', '--list', 'v*'], capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit('release_docs: git tag failed in %s: %s' % (a.repo, r.stderr.strip()))
        tags = r.stdout.split()
    probs, n = check_versions(os.path.abspath(a.site), tags, a.require_pdf)
    for q in probs:
        print('release_docs: ' + q, file=sys.stderr)
    if probs:
        return 1
    print('release_docs: versioned site %s matches the tags (R9; %d pages carry the picker)' % (a.site, n))
    return 0


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if argv[:1] == ['versions']:
        return main_versions(argv[1:])
    if argv[:1] == ['images']:
        ap = argparse.ArgumentParser(prog='release_docs.py images')
        ap.add_argument('--tree', default=REPO)
        a = ap.parse_args(argv[1:])
        probs = check_images(os.path.abspath(a.tree))
        for q in probs:
            print('release_docs: ' + q, file=sys.stderr)
        if probs:
            return 1
        print('release_docs: every doc image has a generator (R11; %d rows in %s)'
              % (len(read_images_tsv(os.path.abspath(a.tree))), IMAGES_TSV))
        return 0
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('cmd', choices=['prepare', 'check'])
    ap.add_argument('version', nargs='?')
    ap.add_argument('--tree', default=REPO)
    ap.add_argument('--fresh', action='store_true')
    ap.add_argument('--rc', action='store_true')
    ap.add_argument('--today', help='YYYY-MM-DD, the date R12 compares the notes date with (tests)')
    ap.add_argument('--site', help='a built site (scripts/docs_build.sh --out) to check README site links against')
    a = ap.parse_args(argv)
    if not a.version:
        a.version = resolve_version(tag_only=(a.cmd == 'check' and not a.fresh))
        if not a.version:
            sys.exit('release_docs: no VER given and scripts/version.sh is absent')
    a.version = re.sub(r'-rc\.\d+$', '', a.version)
    if not re.match(r'^\d+\.\d+\.\d+(-(alpha|beta)(\.\d+)?)?$', a.version):
        sys.exit('release_docs: version %r is not x.y.z (drop any -rc.N)' % a.version)
    if a.cmd == 'prepare':
        return prepare(a.version)
    probs = check_tree(os.path.abspath(a.tree), a.version, a.fresh, a.rc,
                       os.path.abspath(a.site) if a.site else None, a.today)
    for q in probs:
        print('release_docs: ' + q, file=sys.stderr)
    if probs:
        return 1
    print('release_docs: docs for %s are release-ready (R1-R5, R7, R8, R10-R12%s)' % (a.version, ', R6' if a.fresh else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main())
