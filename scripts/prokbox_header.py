#!/usr/bin/env python3
"""ProkBox header sweep: rewrite only the leading comment block of a file.

usage: scripts/prokbox_header.py [--dry] FILE...
(also importable: convert(text, ext) -> (text, kind), used by the art and
bcmfw generators so a regenerated file carries the same banner)

A header-type leading block (art / GPL text / Copyright / SPDX) is replaced
by the banner (its purpose text and any foreign copyright lines kept);
anything else is left in place and the banner is inserted before it.
"""
import os, re, sys

ART = [
" *              __________                   __    __________",
" *   Open       \\______   \\_______    ____  |  | __\\______   \\  ____  ___  ___",
" *   Source      |     ___/\\_  __ \\  /  _ \\ |  |/ / |    |  _/ /  _ \\ \\  \\/  /",
" *   Player      |    |     |  | \\/ (  <_> )|    <  |    |   \\(  <_> ) >    <",
" *   Firmware    |____|     |__|     \\____/ |__|_ \\ |______  / \\____/ /__/\\_ \\",
" *                                               \\/        \\/               \\/",
]
OWNER = "prokopto-dev"
GPL = [
"This program is free software; you can redistribute it and/or",
"modify it under the terms of the GNU General Public License",
"as published by the Free Software Foundation; either version 2",
"of the License, or (at your option) any later version.",
"",
"This software is distributed on an \"AS IS\" basis, WITHOUT WARRANTY OF ANY",
"KIND, either express or implied.",
]
OURS = re.compile(r'ProkBox contributors|Rockbox Contributors', re.I)


def banner_lines(purpose, year, extra_cr, adapted):
    """Body lines (text only, no comment prefix), art lines separate."""
    body = ["Custom firmware for the iPod Video, 5th generation (5G and 5.5G).",
            "Not affiliated with or endorsed by Apple Inc.", ""]
    if purpose:
        body += purpose + [""]
    body += extra_cr
    body += ["Copyright (C) %s %s" % (year, OWNER)]
    if adapted:
        body += ["", "Portions derived from Rockbox (rockbox.org)."]
    body += [""] + GPL + [""]
    return body


def render(body, style, eol):
    out = []
    if style == 'c':
        out.append("/" + "*" * 75)
        out += ART
        out.append(" *")
        for b in body:
            out.append((" * " + b) if b else " *")
        out.append(" " + "*" * 76 + "/")
    else:
        out.append("#" + "*" * 75)
        out += ["# " + a[2:] for a in ART]
        out.append("#")
        for b in body:
            out.append(("# " + b) if b else "#")
        out.append("#" + "*" * 75)
    return eol.join(out) + eol


def is_header(text):
    return bool(re.search(r'GNU General Public|Copyright|SPDX|^\W*(Open|Source|Jukebox|Firmware|Player)\W*$|__________',
                          text, re.M))


def strip_boiler(lines):
    """lines: text lines of a header block (prefixes removed). Returns
    purpose, other copyright lines, adapted flag."""
    purpose, cr = [], []
    i = 0
    n = len(lines)
    art = False
    out = []
    while i < n:
        l = lines[i]
        s = l.strip()
        if s.startswith('This program is free software'):
            # skip to KIND line
            while i < n and 'express or implied' not in lines[i]:
                i += 1
            i += 1
            continue
        if s.startswith('SPDX-License-Identifier'):
            i += 1; continue
        if re.search(r'Copyright', s) and not re.search(r'^\W*$', s):
            if OURS.search(s):
                i += 1; continue
            cr.append(s); i += 1; continue
        if i < 12 and re.match(r'^[\sA-Za-z_\\/|()<>.,\-\'"`>]*$', s) and re.search(r'__|\\_|/_|\|_|\\/|^(Open|Source|Jukebox|Firmware|Player)\b', s):
            i += 1; continue
        out.append(l.rstrip())
        i += 1
    while out and not out[0].strip(): out.pop(0)
    while out and not out[-1].strip(): out.pop()
    return out, cr


def convert(text, ext, year="2026"):
    """Return (new_text, kind): kind is replaced, inserted, adapted or already.
    text uses LF line endings; ext is the file extension (".c", ".py", "")."""
    if 'Custom firmware for the iPod Video, 5th' in text[:3000]:
        return text, 'already'
    # style
    pre = ''
    rest = text
    bom = ''
    if rest.startswith('\ufeff'):
        bom = '\ufeff'; rest = rest[1:]
    first = rest.split('\n', 1)[0]
    if first.startswith('#!'):
        style = 'hash'
        pre = first + '\n'
        rest = rest[len(first) + 1:]
    elif ext in ('.c', '.h', '.S', '.ld', '.cpp'):
        style = 'c'
    elif ext in ('.py', '.sh') or not ext:
        style = 'hash'
    else:
        return text, 'skip-ext'
    if style == 'hash' and ext == '.py':
        m = re.match(r'#.*coding[:=].*\n', rest)
        if m:
            pre += m.group(0); rest = rest[m.end():]
    # leading blank lines (kept)
    lead = re.match(r'(\s*\n)*', rest).group(0)
    body = rest[len(lead):]
    block = None
    if style == 'c':
        if body.startswith('/*'):
            e = body.find('*/')
            end = body.find('\n', e)
            end = len(body) if end < 0 else end + 1
            blk = body[:end]
            inner = body[2:e]
            lines = [re.sub(r'^\s*\*?\s?', '', l) if i else l for i, l in enumerate(inner.split('\n'))]
            lines = [l.strip('*') if re.fullmatch(r'\**', l.strip()) else l for l in lines]
            # first line: after "/*" and stars
            lines = [re.sub(r'^\*+', '', l) if i == 0 else l for i, l in enumerate(lines)]
            lines = [re.sub(r'\*+$', '', l) if i == len(lines) - 1 else l for i, l in enumerate(lines)]
            if is_header(inner):
                block = (blk, lines)
        elif body.startswith('//'):
            ls = []
            pos = 0
            for l in body.split('\n'):
                if l.startswith('//'):
                    ls.append(l); pos += len(l) + 1
                else:
                    break
            inner = '\n'.join(ls)
            if is_header(inner):
                block = (body[:pos], [l[2:].lstrip(' ') if l[2:3] == ' ' else l[2:] for l in ls])
    else:
        ls = []
        pos = 0
        for l in body.split('\n'):
            if l.startswith('#') and not l.startswith('#!'):
                ls.append(l); pos += len(l) + 1
            else:
                break
        inner = '\n'.join(ls)
        if ls and is_header(inner):
            block = (body[:pos], [re.sub(r'^#+ ?', '', l) for l in ls])
    purpose, cr, adapted = [], [], False
    if block:
        blk, lines = block
        purpose, cr = strip_boiler(lines)
        # drop pure-decoration lines
        purpose = [p for p in purpose if not re.fullmatch(r'[*#=\-/ ]*', p) or not p.strip()]
        while purpose and not purpose[0].strip(): purpose.pop(0)
        adapted = bool(cr)
        newhdr = render(banner_lines(purpose, year, cr, adapted), style, '\n')
        tail = body[len(blk):]
        new = bom + pre + lead + newhdr + tail
    else:
        newhdr = render(banner_lines([], year, [], False), style, '\n')
        sep = '\n'
        new = bom + pre + lead + newhdr + sep + body
    return new, ('adapted' if adapted else ('replaced' if block else 'inserted'))


def process(path, dry=False):
    raw = open(path, 'rb').read()
    try:
        text = raw.decode('utf-8'); enc = 'utf-8'
    except UnicodeDecodeError:
        text = raw.decode('latin-1'); enc = 'latin-1'
    crlf = '\r\n' in text
    text = text.replace('\r\n', '\n')
    new, kind = convert(text, os.path.splitext(path)[1])
    if crlf:
        new = new.replace('\n', '\r\n')
    if dry:
        return new
    if kind not in ('already', 'skip-ext'):
        open(path, 'wb').write(new.encode(enc))
    return kind


def stamp(path):
    """For a generator: give the file it just wrote the banner (idempotent)."""
    return process(path)


def run_stdout(fn, ext='.h'):
    """For a generator that prints its output: run fn() and print the
    result with the banner."""
    import contextlib, io
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        fn()
    sys.stdout.write(convert(buf.getvalue(), ext)[0])


if __name__ == '__main__':
    args = sys.argv[1:]
    dry = False
    if args and args[0] == '--dry':
        dry = True; args = args[1:]
    for f in args:
        r = process(f, dry)
        if dry:
            print('=====', f); print(r[:2600])
        else:
            print(r, f)
