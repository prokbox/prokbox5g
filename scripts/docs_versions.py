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

"""Build the versioned ProkBox5g site: one folder per release, from git.

usage: scripts/docs_versions.py [--repo DIR] [--out DIR] [--releases FILE]
                                [--dev auto|yes|no] [--pdf] [--search]
                                [--base-url URL] [--keep-work] [--patch-dir DIR]

Every release tag vX.Y.Z >= v0.8.0 in REPO (default: this checkout) is
exported from git and built with ITS OWN site/ (layouts and stylesheet as
released), so an old release's pages read and look exactly as they did; the
build driver (scripts/docs_build.sh: Hugo, the PDF; site/versions/: the
picker and the skip list) is this checkout's.  A tag whose tree has no site/
(older than the docs build) is built with the current layouts (HEAD's site/)
on its own docs.  A tag listed in site/versions/skip.tsv (tag, TAB, reason) is
left out, and the reason goes into versions.json ("skipped"), which R9
requires.  Any other tag that fails to build fails the whole run: a release
never drops out of the site quietly.

Layout of OUT (what GitHub Pages serves at the base URL):
  ./           the newest release again (a stable "newest" address; old links
               to the unversioned site keep working), with the living pages
  X.Y.Z/       each release, permanent; its own PDF in X.Y.Z/downloads/
  dev/         HEAD, when it has commits no release has, under a fixed
               "not released yet" banner (--dev auto); --dev no leaves it out
  _versions/   the version picker (site/versions/picker.{js,css})
  versions/    a plain list of every version (works without JavaScript)
  versions.json  the picker's list, written at every deploy: old pages are
               never rebuilt to learn about newer releases
"What's coming" and the changelog are living pages: they exist once, at the
root, taken from HEAD when HEAD contains the newest release (else from that
release), together with the roadmap data file the home page's lanes read.
Everything else at the root, and every version folder, is the tag's own tree:
a docs-only update to main (release-plan 3.3a) changes the living pages, dev/
and the picker, never a released version's pages.  In every version folder and in dev/ they are replaced by a page
that sends the reader to the root copy, so no snapshot shows an old plan.
A released version's pages can be CORRECTED without moving its tag (docs patch,
release-plan 3.3a, #336): files under site/versions/patches/vX.Y.Z/ (paths as in
the tree: docs/user/releases/X.Y.Z.md, ...) are laid over that tag's export before
it is built, for its folder and for the root when it is the newest.  Only paths a
docs-only update may change (the DOCS_ALLOW_* lists of release_tree.py, copied
below; a test keeps the two equal) are accepted, a patch for a tag that does not
exist fails the build, and versions.json records it ("patched").
A release whose notes page the site did not build (Hugo leaves out a page dated in
the future) fails the build, naming the date.
Search: Pagefind indexes each folder (and the root) separately (--search), and
each version's own search box loads its own index.

--releases FILE  only tags listed in FILE (one per line) are releases; CI
                 passes the published GitHub releases, so a tag whose release
                 is still a draft is not shown.
Ends with release_docs.py's R9 check of OUT; any problem fails the build."""
import argparse, datetime, json, os, re, shutil, subprocess, sys
from urllib.parse import urlparse

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DOCS_BUILD = os.path.join(HERE, 'docs_build.sh')
PICKER_SRC = os.path.join(ROOT, 'site', 'versions')
SKIP_FILE = os.path.join(PICKER_SRC, 'skip.tsv')
PATCH_DIR = os.path.join(PICKER_SRC, 'patches')
# the paths a docs patch may touch: a copy of release_tree.py's DOCS_ALLOW_* (that script is not in the public tree)
PATCH_ALLOW_DIRS = ('docs/', 'site/')
PATCH_ALLOW_FILES = frozenset([
    'README.md', '.github/workflows/pages.yml',
    'scripts/docs_build.sh', 'scripts/docs_versions.py', 'scripts/docs_shots.sh', 'scripts/readme_visuals.sh',
    'scripts/release_docs.py', 'scripts/roadmap.py', 'scripts/changelog.py', 'scripts/adr_check.py'])
TAGRE = re.compile(r'^v(\d+)\.(\d+)\.(\d+)$')
FLOOR = (0, 8, 0)
EXPORT = ['site', 'docs', 'README.md']
# the living pages: (path under the site root, its source in a tree, title)
LIVING = [('roadmap/', 'docs/user/roadmap.md', "What's coming"),
          ('changelog/', 'docs/user/changelog.md', 'Changelog')]
# data the living pages are drawn from (the home page's release lanes read it): taken from HEAD with them,
# or the root's roadmap page would move on while its home page kept the tag's plan
LIVING_DATA = ['site/data/roadmap.json']
PDF = 'downloads/ProkBox5g-User-Guide.pdf'
MARK_PICKER = 'data-pbv-picker'
MARK_DEV = 'data-pbv-dev'
MARK_LIVING = 'data-pbv-living'


class Fail(Exception):
    pass


def git(repo, *a):
    r = subprocess.run(['git', '-C', repo] + list(a), capture_output=True, text=True)
    if r.returncode != 0:
        raise Fail('git %s failed: %s' % (' '.join(a), r.stderr.strip()))
    return r.stdout


def vkey(v):
    return tuple(int(x) for x in v.split('.'))


def release_tags(repo, only=None):
    """-> release tags >= FLOOR, newest first (only those in `only`, if given)."""
    out = []
    for t in git(repo, 'tag', '--list', 'v*').split():
        m = TAGRE.match(t)
        if m and tuple(int(x) for x in m.groups()) >= FLOOR and (only is None or t in only):
            out.append(t)
    return sorted(out, key=lambda t: vkey(t[1:]), reverse=True)


def export(repo, ref, dest):
    os.makedirs(dest)
    paths = [p for p in EXPORT if git(repo, 'ls-tree', '--name-only', ref, '--', p).strip()]
    if not paths:
        raise Fail('%s has none of %s' % (ref, EXPORT))
    a = subprocess.Popen(['git', '-C', repo, 'archive', '--format=tar', ref, '--'] + paths, stdout=subprocess.PIPE)
    x = subprocess.run(['tar', '-x', '-f', '-', '-C', dest], stdin=a.stdout)
    a.stdout.close()
    if a.wait() != 0 or x.returncode != 0:
        raise Fail('exporting %s failed' % ref)


def read_skips(path):
    """-> {tag: reason} from a skip list; a line without a reason fails."""
    out = {}
    if not os.path.isfile(path):
        return out
    for n, line in enumerate(open(path, encoding='utf-8'), 1):
        line = line.rstrip('\n')
        if not line.strip() or line.startswith('#'):
            continue
        tag, _, why = line.partition('\t')
        if not TAGRE.match(tag.strip()) or not why.strip():
            raise Fail('%s:%d: want "vX.Y.Z<TAB>reason", got %r' % (path, n, line))
        out[tag.strip()] = why.strip()
    return out


def read_patch(patch_dir, tag):
    """-> {path in the tree: file on disk} for the docs patch of `tag` ({} when it has none).
    Refuses a path outside the docs allow-list, a symlink, or an empty patch folder."""
    top = os.path.join(patch_dir, tag)
    if not os.path.isdir(top):
        return {}
    out = {}
    for d, dirs, files in os.walk(top):
        dirs.sort()
        for f in sorted(files):
            full = os.path.join(d, f)
            rel = os.path.relpath(full, top).replace(os.sep, '/')
            if os.path.islink(full):
                raise Fail('docs patch %s: %s is a symlink' % (tag, rel))
            if '..' in rel.split('/') or not (rel.startswith(PATCH_ALLOW_DIRS) or rel in PATCH_ALLOW_FILES):
                raise Fail('docs patch %s: %s is not a path a docs update may change (release_tree.py DOCS_ALLOW_*)'
                           % (tag, rel))
            out[rel] = full
    if not out:
        raise Fail('docs patch %s: %s holds no files (remove the folder)' % (tag, top))
    return out


def apply_patch(tree, files):
    for rel, src in sorted(files.items()):
        dst = os.path.join(tree, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copyfile(src, dst)


def notes_built(tree, stage, ver):
    """The release's own notes page must have been built; Hugo silently leaves out a page dated in the future.
    -> '' or the problem."""
    src = os.path.join(tree, 'docs/user/releases/%s.md' % ver)
    if not os.path.isfile(src) or os.path.isfile(os.path.join(stage, 'releases', ver, 'index.html')):
        return ''
    head = open(src, encoding='utf-8').read().split('---')
    m = re.search(r'^date:\s*["\']?(\d{4}-\d\d-\d\d)', head[1] if len(head) > 2 else '', re.M)
    if not m or m.group(1) <= today():    # not built for another reason (a draft, say): R2 and R9 judge that
        return ''
    return ('the notes page releases/%s/ was not built: its front matter date is %s, after today (%s), and Hugo skips '
            'a page dated in the future. Correct the date with a docs patch: %s/v%s/docs/user/releases/%s.md'
            % (ver, m.group(1), today(), os.path.relpath(PATCH_DIR, ROOT), ver, ver))


def today():
    return datetime.date.today().isoformat()


def docs_build(tree, version, base_url, out, pdf):
    cmd = ['sh', DOCS_BUILD, '--tree', tree, '--version', version, '--base-url', base_url, '--out', out]
    r = subprocess.run(cmd + (['--pdf'] if pdf else []), capture_output=True, text=True)
    return r.returncode, (r.stdout + r.stderr).strip()


def html_files(top):
    for d, dirs, files in os.walk(top):
        dirs.sort()
        for f in sorted(files):
            if f.endswith('.html'):
                yield os.path.join(d, f)


def pages(top):
    """-> the folder pages of a built site ('' = home, 'guide/music/', ...)."""
    out = []
    for d, dirs, files in os.walk(top):
        rel = os.path.relpath(d, top)
        if rel.split(os.sep)[0] in ('pagefind', '_versions'):
            continue
        if 'index.html' in files:
            out.append('' if rel == '.' else rel.replace(os.sep, '/') + '/')
    return sorted(out)


def inject_tags(bp):
    return ('<link rel="stylesheet" href="%s_versions/picker.css" %s>'
            '<script defer src="%s_versions/picker.js" data-base="%s" %s></script>' % (bp, MARK_PICKER, bp, bp, MARK_PICKER))


BODY = re.compile(r'<body\b[^>]*>', re.I)


def postprocess(stage, bp, dev, latest):
    """Picker on every page; dev's fixed banner.  -> pages changed."""
    head = inject_tags(bp)
    nojs = '<noscript><a class="pbv-fallback" href="%sversions/">Other versions of this guide</a></noscript>' % bp
    banner = ('<div class="pbv-banner pbv-dev" role="note" %s>Not released yet: these pages describe ProkBox5g as it is '
              'being built, and may not match any download. <a href="%s">Read the guide for %s, the newest release</a></div>'
              % (MARK_DEV, bp, latest))
    n = 0
    for f in html_files(stage):
        t = open(f, encoding='utf-8').read()
        if MARK_PICKER in t:
            continue
        i = t.find('</head>')
        m = BODY.search(t)
        if i < 0 or not m:
            raise Fail('%s: no </head> or <body>, cannot add the version picker' % f)
        add = nojs + (banner if dev else '')
        t = t[:i] + head + t[i:m.end()] + add + t[m.end():]
        open(f, 'w', encoding='utf-8').write(t)
        n += 1
    return n


def stub_living(stage, bp):
    for rel, _, title in LIVING:
        f = os.path.join(stage, rel, 'index.html')
        if not os.path.isfile(f):
            continue
        open(f, 'w', encoding='utf-8').write(
            '<!doctype html><html lang="en"><head><meta charset="utf-8"><title>%s · ProkBox5g</title>'
            '<meta name="robots" content="noindex"><meta http-equiv="refresh" content="0; url=%s%s"></head>'
            '<body %s="%s"><p>This page always shows the newest plan and the full history: '
            '<a href="%s%s">%s</a>.</p></body></html>\n' % (title, bp, rel, MARK_LIVING, rel, bp, rel, title))


def versions_page(bp, latest, built, dev, living):
    items = ''.join('<li><a href="%s">%s</a>%s%s</li>' % (
        bp if b['id'] == latest else '%s%s/' % (bp, b['id']), b['id'], ' (newest)' if b['id'] == latest else '',
        ' · <a href="%s%s/%s">PDF guide</a>' % (bp, b['id'], PDF) if b['pdf'] else '') for b in built)
    if dev:
        items = '<li><a href="%sdev/">Unreleased</a> (in development, not a release)</li>' % bp + items
    lv = ''.join('<li><a href="%s%s">%s</a></li>' % (bp, rel, title) for rel, _, title in LIVING if rel in living)
    return ('<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" '
            'content="width=device-width, initial-scale=1"><title>Versions · ProkBox5g</title>'
            '<link rel="stylesheet" href="%s_versions/picker.css" %s></head><body><main class="pbv-list">'
            '<h1>ProkBox5g guide: every version</h1><ul>%s</ul><h2>Always up to date</h2><ul>%s</ul>'
            '</main></body></html>\n' % (bp, MARK_PICKER, items, lv))


def say(msg):
    print('docs_versions: ' + msg, flush=True)


def build(a):
    repo = os.path.abspath(a.repo)
    out = os.path.abspath(a.out)
    work = out + '.work'
    for d in (out, work):
        shutil.rmtree(d, ignore_errors=True)
    os.makedirs(work)
    head = git(repo, 'rev-parse', 'HEAD').strip()
    only = None
    if a.releases:
        only = {l.strip() for l in open(a.releases) if l.strip()}
    tags = release_tags(repo, only)
    if not tags:
        raise Fail('no release tag vX.Y.Z >= v%d.%d.%d in %s%s' % (FLOOR + (repo, ' (and in %s)' % a.releases if only else '')))
    say('releases: %s' % ' '.join(tags))

    htree = os.path.join(work, 'tree-HEAD')
    export(repo, head, htree)
    base_url = a.base_url
    if not base_url:
        m = re.search(r'^baseURL\s*=\s*"([^"]+)"', open(os.path.join(htree, 'site/hugo.toml')).read(), re.M)
        if not m:
            raise Fail('HEAD site/hugo.toml has no baseURL; pass --base-url')
        base_url = m.group(1)
    base_url = base_url.rstrip('/') + '/'
    bp = urlparse(base_url).path or '/'

    built, skipped = [], []
    skips = read_skips(a.skip_file)
    for t in tags:
        ver = t[1:]
        if t in skips:
            skipped.append({'tag': t, 'why': skips[t]})
            say('skip %s (%s): %s' % (t, os.path.relpath(a.skip_file, ROOT), skips[t]))
            continue
        tree = os.path.join(work, 'tree-' + ver)
        export(repo, t, tree)
        patch = read_patch(a.patch_dir, t)
        apply_patch(tree, patch)
        own = os.path.isfile(os.path.join(tree, 'site/hugo.toml'))
        if not own:   # an older site/ without hugo.toml is replaced whole, never mixed
            shutil.rmtree(os.path.join(tree, 'site'), ignore_errors=True)
            shutil.copytree(os.path.join(htree, 'site'), os.path.join(tree, 'site'))
        stage = os.path.join(work, 'out-' + ver)
        rc, log = docs_build(tree, ver, base_url + ver + '/', stage, a.pdf)
        if rc != 0:
            raise Fail('%s failed to build with %s layouts (list it in %s with a reason to leave it out): %s'
                       % (t, 'its own' if own else 'the current', os.path.relpath(a.skip_file, ROOT),
                          ' | '.join(log.splitlines()[-3:])[:600]))
        why = notes_built(tree, stage, ver)
        if why:
            raise Fail('%s: %s' % (t, why))
        commit = git(repo, 'rev-list', '-n1', t).strip()
        built.append(dict(id=ver, tag=t, commit=commit, templates='own' if own else 'current', stage=stage,
                          tree=tree, pdf=a.pdf, patched=sorted(patch)))
        say('built %s (%s layouts)%s' % (t, 'its own' if own else 'current',
                                         ', docs patch: %s' % ' '.join(sorted(patch)) if patch else ''))
    if not built:
        raise Fail('no release built')
    stale = sorted(d for d in (os.listdir(a.patch_dir) if os.path.isdir(a.patch_dir) else [])
                   if TAGRE.match(d) and d not in release_tags(repo))
    if stale:
        raise Fail('docs patch folder(s) %s in %s match no release tag in the repository' % (stale, a.patch_dir))
    if built[0]['tag'] != tags[0]:
        raise Fail('the newest release %s is skipped; the site would show an older one as newest' % tags[0])
    latest = built[0]

    # the root: the newest release again, with the living pages from HEAD when HEAD contains it
    head_has_latest = subprocess.run(['git', '-C', repo, 'merge-base', '--is-ancestor', latest['commit'], head]).returncode == 0
    lsrc = htree if head_has_latest else latest['tree']
    rtree = os.path.join(work, 'tree-root')
    shutil.copytree(latest['tree'], rtree)
    living = []
    for rel, src, _ in LIVING:
        if os.path.isfile(os.path.join(lsrc, src)):
            shutil.copyfile(os.path.join(lsrc, src), os.path.join(rtree, src))
            living.append(rel)
    for src in LIVING_DATA:
        if os.path.isfile(os.path.join(lsrc, src)) and os.path.isdir(os.path.dirname(os.path.join(rtree, src))):
            shutil.copyfile(os.path.join(lsrc, src), os.path.join(rtree, src))
    rstage = os.path.join(work, 'out-root')
    rc, log = docs_build(rtree, latest['id'], base_url, rstage, False)
    if rc != 0:
        raise Fail('the root (newest release %s with the living pages from %s) failed: %s'
                   % (latest['tag'], 'HEAD' if head_has_latest else latest['tag'], ' | '.join(log.splitlines()[-3:])))
    if a.pdf:
        os.makedirs(os.path.join(rstage, os.path.dirname(PDF)), exist_ok=True)
        shutil.copyfile(os.path.join(latest['stage'], PDF), os.path.join(rstage, PDF))
    say('root: %s, living pages (%s) from %s' % (latest['tag'], ', '.join(living) or 'none',
                                                  'HEAD ' + head[:10] if head_has_latest else latest['tag']))

    # dev: HEAD, when it has commits no release has
    ahead = int(git(repo, 'rev-list', '--count', head, '--not', *tags).strip())
    dev = None
    if a.dev == 'yes' or (a.dev == 'auto' and ahead > 0 and head_has_latest):
        dstage = os.path.join(work, 'out-dev')
        rc, log = docs_build(htree, 'dev+g' + head[:10], base_url + 'dev/', dstage, a.pdf)
        if rc != 0:
            raise Fail('dev (HEAD %s) failed: %s' % (head[:10], ' | '.join(log.splitlines()[-3:])))
        dev = dict(commit=head, stage=dstage, pdf=a.pdf)
        say('dev: HEAD %s, %d commits after the releases' % (head[:10], ahead))

    # every page gets the picker; versions and dev send the living pages to the root
    stages = [(rstage, False)] + [(b['stage'], False) for b in built] + ([(dev['stage'], True)] if dev else [])
    for st, isdev in stages:
        if st != rstage:
            stub_living(st, bp)
        postprocess(st, bp, isdev, latest['id'])
    if a.search:
        pf = os.environ.get('PAGEFIND', 'pagefind')
        if not shutil.which(pf):
            raise Fail('--search: %s not found (set PAGEFIND; the docs image has it)' % pf)
        for st, _ in stages:
            r = subprocess.run([pf, '--site', st], capture_output=True, text=True)
            if r.returncode != 0:
                raise Fail('pagefind on %s failed: %s' % (st, (r.stdout + r.stderr).strip()[-300:]))

    # assemble
    reserved = {b['id'] for b in built} | {'dev', '_versions', 'versions', 'versions.json'}
    clash = sorted(reserved & set(os.listdir(rstage)))
    if clash:
        raise Fail('the root site already has %s, which the versioned layout needs' % clash)
    shutil.copytree(rstage, out)
    for b in built:
        shutil.copytree(b['stage'], os.path.join(out, b['id']))
    if dev:
        shutil.copytree(dev['stage'], os.path.join(out, 'dev'))
    os.makedirs(os.path.join(out, '_versions'))
    for f in ('picker.js', 'picker.css'):
        shutil.copyfile(os.path.join(PICKER_SRC, f), os.path.join(out, '_versions', f))
    os.makedirs(os.path.join(out, 'versions'))
    open(os.path.join(out, 'versions', 'index.html'), 'w').write(versions_page(bp, latest['id'], built, dev, living))
    vj = dict(schema=1, base=bp, latest=latest['id'],
              versions=[dict(id=b['id'], tag=b['tag'], commit=b['commit'], templates=b['templates'], pdf=b['pdf'], patched=b['patched'],
                             pages=[p for p in pages(b['stage']) if p not in living]) for b in built],
              root=dict(tag=latest['tag'], living_from=head if head_has_latest else latest['tag'],
                        pages=pages(rstage)),
              dev=dict(commit=dev['commit'], pdf=dev['pdf'], pages=[p for p in pages(dev['stage']) if p not in living])
              if dev else None,
              living=living, living_links=[dict(path=rel, title=title) for rel, _, title in LIVING if rel in living],
              skipped=skipped)
    with open(os.path.join(out, 'versions.json'), 'w') as f:
        json.dump(vj, f, indent=1, sort_keys=True)
        f.write('\n')

    sys.path.insert(0, HERE)
    import release_docs
    probs, n = release_docs.check_versions(out, tags, require_pdf=a.pdf)
    for p in probs:
        print('docs_versions: ' + p, file=sys.stderr)
    if probs:
        raise Fail('R9 refused the built site (%d problems)' % len(probs))
    say('R9 ok: %d versions (%s), %s, %d pages carry the picker; skipped: %s'
        % (len(built), ' '.join(b['id'] for b in built), 'dev' if dev else 'no dev', n,
           ', '.join(s['tag'] for s in skipped) or 'none'))
    if not a.keep_work:
        shutil.rmtree(work)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--repo', default=ROOT)
    ap.add_argument('--out', default=os.path.join(ROOT, 'build', 'site-versions'))
    ap.add_argument('--releases')
    ap.add_argument('--dev', choices=['auto', 'yes', 'no'], default='auto')
    ap.add_argument('--pdf', action='store_true')
    ap.add_argument('--search', action='store_true')
    ap.add_argument('--base-url')
    ap.add_argument('--keep-work', action='store_true')
    ap.add_argument('--skip-file', default=SKIP_FILE)
    ap.add_argument('--patch-dir', default=PATCH_DIR)
    a = ap.parse_args(argv)
    try:
        build(a)
    except Fail as e:
        print('docs_versions: FAILED: %s' % e, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
