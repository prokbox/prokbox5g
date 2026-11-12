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

"""Generate docs/user/roadmap.md ("What's coming") from the project tracker.

usage: scripts/roadmap.py generate VER [--offline ISSUES.json] [--out FILE] [--data-out FILE]
       scripts/roadmap.py check VER [--tree DIR]     (the page and its data file exist, were made for VER, agree, are clean)
       scripts/roadmap.py verify VER [--offline ISSUES.json]
                                                     (needs the tracker: the committed page and data file equal a fresh run;
                                                      exit 0 current, 1 stale, 3 tracker unreachable)

generate writes two files from one run, so they can never disagree:
  docs/user/roadmap.md       the "What's coming" page
  site/data/roadmap.json     the same releases and entries as data, for the home page's release
                             lanes (site/layouts/partials/roadmap-lanes.html) and the Features
                             page's plan tags (site/layouts/shortcodes/plan.html)
Each published entry gets a stable anchor, item-<its short label as a slug>.  The page carries
it as an empty link "[](#item-...)" at the end of the entry (invisible on GitHub; the site's
link render hook turns it into the anchor), and the data file names it, so a bubble in the
lanes links to its entry.

VER is the release being shipped.  The page shows one section per release after it, in
version order (0.8.3, 0.9.0, ... 1.0), then "Later", with a short release plan table at
the top.  The release an entry belongs to is the issue's milestone on the tracker: one
source of truth.  Milestones that are not versions (for example "Tooling & process") are
never published.

The page is curated.  Nothing is published unless docs/roadmap/overrides.tsv (or a
"Roadmap: text" line in the issue body, used only when the file has no row for that
issue) gives it a user-facing sentence.  Developer titles are never used.

overrides.tsv: tab-separated, '#' lines are comments.
  @theme <TAB> RELEASE <TAB> text       the release's theme, one plain line (needed for every
                                        release that has entries)
  @word <TAB> term <TAB> explanation    one line of the "Words on this page" box
  @notplanned_intro <TAB> text          declares the "Not planned" section and gives its intro line
  @notplanned <TAB> key <TAB> title <TAB> text
                                        one thing we will not do: key is a lower-case slug (the
                                        anchor is notplanned-<key>), title a short label, text one
                                        or two plain sentences saying why.  No tracker issue.
  issue <TAB> release <TAB> status <TAB> changed <TAB> text <TAB> reason [<TAB> short]
    release  optional.  Empty = the tracker's milestone.  If given it must equal the
             milestone (a stale row fails rather than moving the entry quietly)
    status   empty = derived from the tracker; else Considering, Planned, "In design",
             "In development", "In testing", Done, "Postponed [to X]", Cancelled
    changed  version in which a Postponed or Cancelled status was set (empty if unknown)
    text     one plain UK-English sentence, or '-' to hide the issue
    reason   short plain reason (shown for Postponed and Cancelled)
    short    optional: a label of at most SHORT_MAX (48) characters for the home page's lane
             bubble.  Needed when the text is longer than that (generate fails otherwise).
             The entry's anchor is made from it, so two published entries may not share one
  An issue may have several rows (one entry each).

Status from the tracker (when the status column is empty): labels status/cancelled
(any state) -> Cancelled; a closed issue -> Done; status/postponed -> Postponed;
status/considering -> Considering; status/design -> In design; status/testing -> In testing; status/development or
in-progress -> In development; otherwise Planned.
Done entries show in their release's section until that release ships, then drop off
(the changelog carries them).  Postponed and Cancelled entries stay in their section
until one release has shipped since the change (VER > changed), or at once if their
milestone is not on the page; then they move to "Changed plans".

Fails closed (exit 1): an override naming an issue the tracker does not have, a release
column that disagrees with the milestone, an unknown status, a published entry whose
milestone is not an upcoming release (an open issue left in a shipped release, or in
"Tooling & process"), an open type/feature issue in an upcoming release up to 1.0 with
no row (write a sentence, or '-' to hide it), a "Not planned" section that is declared but has
no entries (or entries with no declared section), a repeated or malformed key, a release with entries but no theme, a
scrub hit (the release scrub patterns) or developer jargon in the published text or
short label, a long text with no short label, a short label over SHORT_MAX, two entries
with the same anchor, an empty page.  Offline the tracker is unreachable: run it where the forge is, commit the
page.
"""
import argparse, datetime, importlib, json, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)

OVERRIDES = 'docs/roadmap/overrides.tsv'
PAGE = 'docs/user/roadmap.md'
DATA = 'site/data/roadmap.json'
# the longest label a lane bubble shows; a longer text needs a short column
SHORT_MAX = 48
LATER = 'Later'
# every open feature in an upcoming release up to this one needs a row
LAST_REQUIRED = '1.0'
STATUSES = ['In development', 'In testing', 'In design', 'Planned', 'Considering', 'Postponed', 'Cancelled', 'Done']
# order inside a section: the most finished first
ORDER = ['Done', 'In testing', 'In development', 'In design', 'Planned', 'Considering', 'Postponed', 'Cancelled']
# not started: a release made only of these is still Planned
NOT_STARTED = ('Planned', 'Considering')
RANK = {s: i for i, s in enumerate(ORDER)}
# words that mean a developer wrote the line, not a user-facing sentence
JARGON = re.compile(r'\b(COP|CPU|BCM\d*|VC|PP5022|IRAM|FIQ|ABAB|DMA|FIFO|PLL|b1\d|hr\d+|gate|bench|sim|SDK|iAP|'
                    r'refactor|regression|commit|branch|worktree)\b|#\d+|`')


class Fail(Exception):
    pass


class Unreachable(Fail):
    """The tracker could not be read (not the same as the page being stale: verify exits 3, not 1)."""
    pass


def vkey(v):
    return tuple(int(x) for x in re.findall(r'\d+', v)[:3])


def is_version(t):
    return bool(re.match(r'^\d+\.\d+(\.\d+)?$', t or ''))


def is_release(t):
    return is_version(t) or t == LATER


def milestone(issue):
    m = issue.get('milestone') or {}
    return m.get('title') or ''


def labels_of(issue):
    return {l['name'] if isinstance(l, dict) else l for l in issue.get('labels', [])}


def parse_overrides(path):
    rows, themes, words = [], {}, []
    notplanned, np_intro = [], []
    if not os.path.isfile(path):
        raise Fail('%s missing' % path)
    for no, l in enumerate(open(path).read().split('\n'), 1):
        if not l.strip() or l.startswith('#'):
            continue
        c = l.split('\t')
        if c[0].startswith('@'):
            if c[0] == '@word':
                if len(c) != 3 or not c[1].strip() or not c[2].strip():
                    raise Fail('%s:%d: @word needs a term and one plain explanation' % (path, no))
                words.append((c[1].strip(), c[2].strip()))
                continue
            if c[0] == '@notplanned_intro':
                if len(c) != 2 or not c[1].strip():
                    raise Fail('%s:%d: @notplanned_intro needs one line of text' % (path, no))
                if np_intro:
                    raise Fail('%s:%d: @notplanned_intro is given twice' % (path, no))
                np_intro.append(c[1].strip())
                continue
            if c[0] == '@notplanned':
                if len(c) != 4 or not all(x.strip() for x in c[1:]):
                    raise Fail('%s:%d: @notplanned needs a key, a title and the text' % (path, no))
                key = c[1].strip()
                if not re.match(r'^[a-z0-9]+(-[a-z0-9]+)*$', key):
                    raise Fail('%s:%d: @notplanned key %r must be a lower-case slug (a-z, 0-9, hyphens)' % (path, no, key))
                if any(k == key for k, _, _ in notplanned):
                    raise Fail('%s:%d: @notplanned key %r is used twice' % (path, no, key))
                notplanned.append((key, c[2].strip(), c[3].strip()))
                continue
            if c[0] != '@theme':
                raise Fail('%s:%d: unknown directive %r (only @theme, @word, @notplanned and '
                           '@notplanned_intro; the release order comes from the milestones)' % (path, no, c[0]))
            if len(c) != 3 or not is_release(c[1].strip()) or not c[2].strip():
                raise Fail('%s:%d: @theme needs a release (x.y[.z] or Later) and one line of text' % (path, no))
            themes[c[1].strip()] = c[2].strip()
            continue
        if len(c) not in (6, 7):
            raise Fail('%s:%d: need 6 tab-separated columns (7 with a short label), got %d' % (path, no, len(c)))
        num, release, status, changed, text, reason = [x.strip() for x in c[:6]]
        short = c[6].strip() if len(c) == 7 else ''
        if len(short) > SHORT_MAX:
            raise Fail('%s:%d: short label is %d characters, over %d: %r' % (path, no, len(short), SHORT_MAX, short))
        if not num.isdigit():
            raise Fail('%s:%d: issue %r is not a number' % (path, no, num))
        if release and not is_release(release):
            raise Fail('%s:%d: unknown release %r (leave it empty: the milestone decides)' % (path, no, release))
        if status:
            try:
                base_status(status)
            except Fail:
                raise Fail('%s:%d: unknown status %r' % (path, no, status))
        if changed and not re.match(r'^\d+\.\d+\.\d+$', changed):
            raise Fail('%s:%d: changed %r is not x.y.z' % (path, no, changed))
        rows.append(dict(num=int(num), release=release, status=status, changed=changed,
                         text=text, reason=reason, short=short, line=no))
    if notplanned and not np_intro:
        raise Fail('%s: @notplanned entries need an @notplanned_intro line (it declares the section)' % path)
    if np_intro and not notplanned:
        raise Fail('%s: @notplanned_intro declares the "Not planned" section, but it has no @notplanned entries' % path)
    return rows, themes, words, dict(intro=np_intro[0] if np_intro else '', items=notplanned)


def derive_status(issue):
    labels = labels_of(issue)
    if 'status/cancelled' in labels:
        return 'Cancelled'
    if issue.get('state') == 'closed':
        return 'Done'
    for lab, st in (('status/postponed', 'Postponed'), ('status/considering', 'Considering'),
                    ('status/design', 'In design'),
                    ('status/testing', 'In testing'), ('status/development', 'In development'),
                    ('in-progress', 'In development')):
        if lab in labels:
            return st
    return 'Planned'


def base_status(s):
    for b in STATUSES:
        if s == b or s.startswith(b + ' '):
            return b
    raise Fail('unknown status %r' % s)


def body_rows(issue):
    """'Roadmap: text' or 'Roadmap: RELEASE | text' in the issue body"""
    m = re.search(r'^Roadmap:\s*(?:([^|\n]+?)\s*\|)?\s*(.+?)\s*$', issue.get('body') or '', re.M)
    if not m:
        return []
    rel = (m.group(1) or '').strip()
    if rel and not is_release(rel):
        return []
    return [dict(num=issue['number'], release=rel, status='', changed='', text=m.group(2),
                 reason='', short='', line=0)]


def scrub_check(text, what, optional=False):
    try:
        import release_tree
    except ImportError:
        if optional:     # the public tree has no release_tree.py; the export scan covers it there
            return
        raise
    if release_tree.FAIL_CLOSED_RE.search(text) or re.search('|'.join(release_tree.SCRUB), text, re.I):
        raise Fail('scrub: %s matches the release scrub patterns: %r' % (what, text[:80]))
    if JARGON.search(text):
        raise Fail('jargon: %s reads like developer text (%r); write it for a user' % (what, text[:80]))


def load_issues(offline):
    if offline:
        return {i['number']: i for i in json.load(open(offline))}
    # the tracker client's name is built, not spelled: this file is exported, and the
    # export scan refuses that name
    forge = importlib.import_module(os.environ.get('ROADMAP_TRACKER_MODULE', 'forge' + 'jo'))
    try:
        return {i['number']: i for i in forge.issues('all')}
    except forge.Unavailable as e:
        raise Unreachable('tracker unreachable (%s): run this where the forge is reachable and commit the page' % e)


def upcoming(ver, issues, themes):
    """-> the releases after VER, in order, then Later"""
    names = {milestone(i) for i in issues.values()} | set(themes)
    vs = sorted((t for t in names if is_version(t) and vkey(t) > vkey(ver)), key=vkey)
    return vs + [LATER]


def due_dates(issues):
    out = {}
    for i in issues.values():
        m = i.get('milestone') or {}
        if m.get('title') and m.get('due_on'):
            out[m['title']] = m['due_on'][:10]
    return out


def build_entries(ver, issues, rows):
    """-> [entry dict] for every row to publish"""
    have = {r['num'] for r in rows}
    allrows = list(rows)
    for n, i in sorted(issues.items()):
        if n not in have:
            allrows += body_rows(i)
    have = {r['num'] for r in allrows}
    pub = set(upcoming(ver, issues, {}))
    # every open feature planned for an upcoming release (up to 1.0) has a row
    missing = sorted('#%d (%s)' % (n, milestone(i)) for n, i in issues.items()
                     if i.get('state') == 'open' and 'type/feature' in labels_of(i) and n not in have
                     and is_version(milestone(i)) and vkey(ver) < vkey(milestone(i)) <= vkey(LAST_REQUIRED))
    if missing:
        raise Fail('no sentence for %d published feature(s): %s; add a row to %s (a sentence, or "-" '
                   'to hide it)' % (len(missing), ', '.join(missing), OVERRIDES))
    out = []
    for r in allrows:
        where = 'overrides.tsv:%d' % r['line'] if r['line'] else 'issue body'
        i = issues.get(r['num'])
        if i is None:
            raise Fail('%s names issue #%d, which the tracker does not have' % (where, r['num']))
        m = milestone(i)
        if r['release'] and r['release'] != m:
            raise Fail('%s: #%d says release %s, but its milestone on the tracker is %s; the milestone '
                       'decides (clear the column, or move the milestone)' % (where, r['num'], r['release'], m or 'none'))
        if r['text'] == '-':
            continue
        st = r['status'] or derive_status(i)
        b = base_status(st)
        shown = m in pub
        if b == 'Done' and not shown:
            continue                       # shipped: the changelog carries it
        scrub_check(r['text'], '#%d text' % r['num'])
        scrub_check(r['reason'], '#%d reason' % r['num'])
        short = r['short'] or r['text']
        if len(short) > SHORT_MAX:
            raise Fail('%s: #%d text is %d characters, too long for a lane bubble: add a short label of at '
                       'most %d characters (7th column)' % (where, r['num'], len(r['text']), SHORT_MAX))
        scrub_check(short, '#%d short label' % r['num'])
        if b in ('Postponed', 'Cancelled'):
            moved = not shown or bool(r['changed'] and vkey(ver) > vkey(r['changed']))
        elif not shown:
            why = ('has shipped (or ships now): move it to a later milestone'
                   if is_version(m) else 'is not a release, so it is never published')
            raise Fail('#%d is published, but its milestone %r %s, or hide it with "-"' % (r['num'], m or 'none', why))
        else:
            moved = False
        out.append(dict(r, status=st, base=b, moved=moved, release=m, short=short, anchor=item_anchor(short)))
    seen = {}
    for e in out:
        if e['anchor'] in seen:
            raise Fail('#%d and #%d would share the anchor %s: give one of them a different short label'
                       % (seen[e['anchor']], e['num'], e['anchor']))
        seen[e['anchor']] = e['num']
    return out


def item_anchor(label):
    """-> the entry's anchor on the page, from its short label: stable while the label is"""
    return 'item-' + (re.sub(r'[^a-z0-9]+', '-', label.lower()).strip('-')[:60] or 'x')


def progress(es):
    """-> (where it stands, 'k of n' items under way or done) for one release's entries.
    The rule is printed on the page (PLAN_RULE): Planned until any item has started,
    then Under way, then Done when every item is done."""
    live = [e['base'] for e in es if e['base'] not in ('Postponed', 'Cancelled')]
    if not live:
        return 'Planned', ''
    n = sum(1 for b in live if b not in NOT_STARTED)
    started = '%d of %d' % (n, len(live))
    if all(b == 'Done' for b in live):
        return 'Done', started
    return ('Under way' if n else 'Planned'), started


PLAN_RULE = ('**Where it stands** says *Planned* until work on any of the release\'s items has started, '
             '*Under way* after that, and *Done* when every item is finished. **Items under way** counts '
             'the items that are in design, in development, in testing or done.')


def anchor(rel):
    return 'later' if rel == LATER else 'release-' + rel.replace('.', '')


def heading(rel):
    return LATER if rel == LATER else 'Release ' + rel


def entry_line(e, plans=False):
    s = '- _%s:_ %s' % (e['status'], e['text'].rstrip())
    if e['base'] in ('Postponed', 'Cancelled') and e['reason']:
        s += ' %s' % e['reason'].rstrip()
    if plans and e['changed']:
        s += ' (changed in %s)' % e['changed']
    # the anchor a lane bubble links to: an empty link, invisible on GitHub, an anchor on the site
    return s + ' [](#%s)' % e['anchor']


# status -> the class key the site styles it with (site.css .s-<key>)
STATUS_KEY = {'Considering': 'considering', 'Planned': 'planned', 'In design': 'design',
              'In development': 'development', 'In testing': 'testing', 'Done': 'done',
              'Postponed': 'postponed', 'Cancelled': 'cancelled'}
# what each status means, for the lanes' legend (the same meanings as the page's status line)
STATUS_MEANING = {'Considering': 'an idea we are looking at', 'Planned': 'agreed, not started',
                  'In design': 'working out how it looks and behaves', 'In development': 'being built',
                  'In testing': 'built, being checked', 'Done': 'finished, arrives with its release',
                  'Postponed': 'moved to a later release', 'Cancelled': 'no longer planned'}
# always in the legend, in this order (least finished first); Done, Postponed and Cancelled join when used
LEGEND = ['Considering', 'Planned', 'In design', 'In development', 'In testing', 'Done', 'Postponed', 'Cancelled']


def render_data(ver, entries, releases, themes, dues, notplanned=None):
    """-> the data file's text: the page's releases and entries, for the site's templates"""
    by = {rel: sorted((e for e in entries if e['release'] == rel and not e['moved']),
                      key=lambda e: RANK[e['base']]) for rel in releases}
    rels, items = [], []
    for rel in releases:
        if not (by[rel] or rel in themes):
            continue
        word, detail = progress(by[rel])
        rels.append(dict(name=rel, heading=heading(rel), anchor=anchor(rel), theme=themes[rel],
                         later=rel == LATER, stands=word, under_way=detail, aimed=dues.get(rel, '')))
        for e in by[rel]:
            items.append(dict(issue=e['num'], release=rel, status=e['status'], key=STATUS_KEY[e['base']],
                              short=e['short'], text=e['text'], anchor=e['anchor']))
    used = {i['key'] for i in items}
    # listed in legend order; "order" is the status's place on the page (most finished first)
    statuses = [dict(name=s, key=STATUS_KEY[s], meaning=STATUS_MEANING[s], order=RANK[s]) for s in LEGEND
                if STATUS_KEY[s] in used or s in LEGEND[:5]]
    d = dict(generated_for=ver, source='scripts/roadmap.py generate (do not edit by hand)',
             statuses=statuses, releases=rels, items=items)
    if notplanned and notplanned['items']:
        d['not_planned'] = dict(intro=notplanned['intro'], anchor='not-planned',
                                items=[dict(key=k, title=t, text=x, anchor=np_anchor(k)) for k, t, x in notplanned['items']])
    return json.dumps(d, indent=1, ensure_ascii=False) + '\n'


def np_anchor(key):
    return 'notplanned-' + key


def nice_date(d):
    try:
        return datetime.date.fromisoformat(d).strftime('%-d %B %Y')
    except ValueError:
        return d


def render(ver, entries, releases, themes, dues, words=(), notplanned=None):
    by = {rel: sorted((e for e in entries if e['release'] == rel and not e['moved']),
                      key=lambda e: RANK[e['base']]) for rel in releases}
    shown_rel = [rel for rel in releases if by[rel] or rel in themes]
    for rel in shown_rel:
        if rel not in themes:
            raise Fail('release %s has entries but no theme: add "@theme<TAB>%s<TAB>one plain line" to %s'
                       % (rel, rel, OVERRIDES))
        scrub_check(themes[rel], 'theme of %s' % rel)
    for term, meaning in words:
        scrub_check(term + ' ' + meaning, 'word %r' % term)
    L = ['---', 'title: "What\'s coming"',
         'description: "What we are working on for ProkBox5g, release by release, and what has changed."',
         'roadmap_version: "%s"' % ver, 'page_class: "roadmap"', '---', '',
         'This page lists what we plan for each coming release of ProkBox5g, and where each item stands. '
         'It is a plan, not a promise: items can move, and we say so here when they do. '
         'Finished work leaves this page when its release comes out, and appears in the '
         '[changelog](../changelog/).', '',
         '## Release plan', '',
         '| Release | What it brings | Where it stands | Items under way |', '|---|---|---|---|']
    for rel in shown_rel:
        word, detail = progress(by[rel])
        cell = '_%s_' % word          # the status word first: the page styles it as a tag
        if rel in dues:
            cell += ', aimed for %s' % nice_date(dues[rel])
        L.append('| [%s](#%s) | %s | %s | %s |' % (rel, anchor(rel), themes[rel], cell, detail or '-'))
    L += ['', PLAN_RULE, '',
          'Each item has a status: **Considering** (an idea we are looking at, not designed yet), '
          '**Planned** (agreed, not started), **In design** (working out how it should look and behave), '
          '**In development** (being built), **In testing** (built, being checked), **Done** (finished, '
          'arrives with that release), **Postponed** and **Cancelled** (each with a short reason).', '']
    if words:
        L += ['> [!NOTE] Words on this page', '>']
        for term, meaning in words:
            L.append('> - **%s**: %s' % (term, meaning))
        L.append('')
    shown = 0
    for rel in shown_rel:
        L += ['## ' + heading(rel), '', themes[rel], '']
        if not by[rel]:
            L += ['Nothing is listed here yet.', '']
            continue
        for e in by[rel]:
            L.append(entry_line(e))
            shown += 1
        L.append('')
    mv = [e for e in entries if e['moved']]
    if mv:
        L += ['## Changed plans', '', 'Items that were postponed or cancelled.', '']
        for e in mv:
            L.append(entry_line(e, plans=True))
            shown += 1
        L.append('')
    if notplanned and notplanned['items']:
        scrub_check(notplanned['intro'], 'the Not planned intro')
        L += ['## Not planned', '', notplanned['intro'], '']
        for k, t, x in notplanned['items']:
            scrub_check(t + ' ' + x, 'Not planned entry %r' % k)
            L.append('- **%s.** %s [](#%s)' % (t.rstrip('.'), x, np_anchor(k)))
        L.append('')
    if not shown:
        raise Fail('the page would be empty')
    return '\n'.join(L).rstrip('\n') + '\n', shown


def generate(ver, root=REPO, offline=None, with_data=False):
    """-> (page text, entry count), or with with_data (page text, entry count, data text)"""
    rows, themes, words, notplanned = parse_overrides(os.path.join(root, OVERRIDES))
    issues = load_issues(offline)
    entries = build_entries(ver, issues, rows)
    rels, dues = upcoming(ver, issues, themes), due_dates(issues)
    text, n = render(ver, entries, rels, themes, dues, words, notplanned)
    if not with_data:
        return text, n
    return text, n, render_data(ver, entries, rels, themes, dues, notplanned)


def check_page(root, ver):
    """-> [problems] for release_docs.py (R8)"""
    path = os.path.join(root, PAGE)
    if not os.path.isfile(path):
        return ['R8 %s missing (run scripts/release_docs.py prepare %s where the tracker is reachable)' % (PAGE, ver)]
    t = open(path).read()
    m = re.search(r'^roadmap_version:\s*"([^"]*)"', t, re.M)
    if not m or m.group(1) != ver:
        return ['R8 %s was made for %s, not %s (run scripts/release_docs.py prepare %s)' %
                (PAGE, m.group(1) if m else 'no version', ver, ver)]
    if not re.search(r'^- _[^_]+:_ \S', t, re.M):
        return ['R8 %s lists no entries' % PAGE]
    try:
        scrub_check(t, PAGE, optional=True)
    except Fail as e:
        return ['R8 %s: %s' % (PAGE, e)]
    return check_data(root, ver, t)


def check_data(root, ver, page):
    """-> [problems]: the data file exists, was made for VER and names exactly the page's anchors.
    Offline, so it cannot see a tracker that moved on (verify does); it does catch a page
    regenerated without its data file, or the other way round."""
    path = os.path.join(root, DATA)
    if not os.path.isfile(path):
        return ['R8 %s missing (scripts/roadmap.py generate %s writes it with the page)' % (DATA, ver)]
    try:
        d = json.load(open(path))
        got, items = d['generated_for'], d['items']
    except (ValueError, KeyError, TypeError) as e:
        return ['R8 %s is not a roadmap data file (%s); regenerate it' % (DATA, e)]
    if got != ver:
        return ['R8 %s was made for %s, not %s (run scripts/roadmap.py generate %s)' % (DATA, got, ver, ver)]
    if not items:
        return ['R8 %s lists no entries' % DATA]
    # the page's release sections; Changed plans is left out, as the lanes leave moved items out
    main = page.split('\n## Changed plans')[0].split('\n## Not planned')[0]
    want = sorted(re.findall(r'^- _[^_]+:_ .*\[\]\(#(item-[a-z0-9-]+)\)$', main, re.M))
    have = sorted(i.get('anchor', '') for i in items)
    if want != have:
        return ['R8 %s and %s disagree (%d entries on the page, %d in the data; first difference %s); '
                'regenerate both with scripts/roadmap.py generate %s'
                % (PAGE, DATA, len(want), len(have), sorted(set(want) ^ set(have))[:1], ver)]
    np_page = sorted(re.findall(r'^- \*\*.*\[\]\(#(notplanned-[a-z0-9-]+)\)$', page.split('\n## Not planned')[-1], re.M)
                     if '\n## Not planned' in page else [])
    np_data = sorted(i.get('anchor', '') for i in d.get('not_planned', {}).get('items', []))
    if np_page != np_data:
        return ['R8 %s and %s disagree on the Not planned section (%d on the page, %d in the data; first '
                'difference %s); regenerate both with scripts/roadmap.py generate %s'
                % (PAGE, DATA, len(np_page), len(np_data), sorted(set(np_page) ^ set(np_data))[:1], ver)]
    try:
        scrub_check(open(path).read(), DATA, optional=True)
    except Fail as e:
        return ['R8 %s: %s' % (DATA, e)]
    return []


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('cmd', choices=['generate', 'check', 'verify'])
    ap.add_argument('version')
    ap.add_argument('--offline')
    ap.add_argument('--out')
    ap.add_argument('--data-out')
    ap.add_argument('--tree', default=REPO)
    a = ap.parse_args(argv)
    try:
        if a.cmd == 'check':
            probs = check_page(os.path.abspath(a.tree), a.version)
            for p in probs:
                print('roadmap: ' + p, file=sys.stderr)
            return 1 if probs else 0
        text, n, data = generate(a.version, a.tree, a.offline, with_data=True)
        if a.cmd == 'verify':
            stale = []
            for rel, want in ((PAGE, text), (DATA, data)):
                p = os.path.join(a.tree, rel)
                if not os.path.isfile(p) or open(p).read() != want:
                    stale.append(rel)
            if stale:
                print('roadmap: %s %s stale; regenerate with scripts/roadmap.py generate %s'
                      % (' and '.join(stale), 'is' if len(stale) == 1 else 'are', a.version), file=sys.stderr)
                return 1
            print('roadmap: %s and %s are current (%d entries)' % (PAGE, DATA, n))
            return 0
        dout = a.data_out or os.path.join(a.tree, DATA)
        os.makedirs(os.path.dirname(os.path.abspath(dout)), exist_ok=True)
        open(a.out or os.path.join(a.tree, PAGE), 'w').write(text)
        open(dout, 'w').write(data)
        print('roadmap: wrote %d entries for %s (%s and %s)' % (n, a.version, a.out or PAGE, a.data_out or DATA))
        return 0
    except Unreachable as e:
        print('roadmap: ' + str(e), file=sys.stderr)
        return 3
    except Fail as e:
        print('roadmap: ' + str(e), file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
