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

"""Render the iPod Video hardware manual (docs/bcm2722-reference/ and
docs/pp5022-reference/) as one local HTML site: a front page, then one
page per file with a sidebar per part, for reading in a browser. Not
published anywhere; the output goes to out/ipod-hw-manual/ (gitignored).

Usage: scripts/bcmref_html.py [--open]
Needs the Python 'markdown' package (with its 'tables', 'fenced_code' and
'toc' extensions, all bundled)."""

import html
import pathlib
import re
import subprocess
import sys

import markdown

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT_ROOT = ROOT / "out" / "ipod-hw-manual"

BCM_ORDER = ["index", "programming", "overview", "memory-map", "memory-access",
             "boot-and-clocks", "host-interface", "isa-scalar", "isa-vector",
             "dma", "lcd", "interrupts-timers-gpio", "sdram", "audio", "power",
             "quirks", "open-questions", "programming-guide", "host-driver",
             "graphics", "sources", "glossary"]
PP_ORDER = ["index", "overview", "boot", "memory-map", "clocks-power",
            "interrupts-timers", "cores-and-threads", "dma", "gpio-i2c",
            "storage", "usb", "display", "audio", "input", "datasheet-features",
            "mac-and-dvs", "quirks",
            "open-questions", "sources"]

# (folder under docs/ and out/, sidebar title, page order)
MANUALS = [("pp5022-reference", "PP5022 platform", PP_ORDER),
           ("bcm2722-reference", "BCM2722 video co-processor", BCM_ORDER)]

TAGS = {"silicon": "#1a7f37", "apple": "#8250df", "rockbox": "#0969da",
        "external": "#9a6700", "hypothesis": "#cf222e", "ours": "#57606a"}

CSS = """
:root { --bg:#fff; --fg:#1f2328; --muted:#59636e; --line:#d1d9e0;
        --side:#f6f8fa; --code:#eff2f5; --link:#0969da; }
@media (prefers-color-scheme: dark) {
  :root { --bg:#0d1117; --fg:#e6edf3; --muted:#9198a1; --line:#3d444d;
          --side:#151b23; --code:#212830; --link:#4493f8; } }
* { box-sizing:border-box; }
body { margin:0; background:var(--bg); color:var(--fg);
       font:15px/1.55 -apple-system, BlinkMacSystemFont, "Helvetica Neue",
       Arial, sans-serif; display:flex; }
nav { width:240px; min-height:100vh; background:var(--side);
      border-right:1px solid var(--line); padding:16px; position:sticky;
      top:0; align-self:flex-start; }
nav h1 { font-size:14px; margin:0 0 12px; }
nav a { display:block; padding:3px 6px; color:var(--fg);
        text-decoration:none; border-radius:4px; font-size:14px; }
nav a.cur { background:var(--line); font-weight:600; }
main { flex:1; max-width:980px; padding:24px 32px 64px; overflow-x:auto; }
a { color:var(--link); }
code { background:var(--code); padding:1px 4px; border-radius:4px;
       font:13px ui-monospace, SFMono-Regular, Menlo, monospace; }
pre { background:var(--code); padding:12px; border-radius:6px;
      overflow-x:auto; }
pre code { padding:0; }
table { border-collapse:collapse; margin:12px 0; display:block;
        overflow-x:auto; }
th, td { border:1px solid var(--line); padding:5px 9px; vertical-align:top;
         text-align:left; }
th { background:var(--side); }
.tag { font-size:11px; font-weight:600; padding:0 5px; border-radius:9px;
       color:#fff; white-space:nowrap; }
.foot { color:var(--muted); font-size:12px; margin-top:48px;
        border-top:1px solid var(--line); padding-top:8px; }
@media (max-width:760px) { body { display:block; }
  nav { width:auto; min-height:0; position:static; }
  main { padding:16px; } }
"""


def tag_spans(body):
    def sub(m):
        name = m.group(1).lower()
        return ('<span class="tag" style="background:%s">%s</span>'
                % (TAGS[name], html.escape(name)))
    return re.sub(r"\[(%s)\]" % "|".join(TAGS), sub, body, flags=re.I)


def title_of(text, stem):
    m = re.search(r"^# (.+)$", text, re.M)
    return m.group(1).strip() if m else stem


def render(folder, name, order, rev_all):
    src = ROOT / "docs" / folder
    out = OUT_ROOT / folder
    out.mkdir(parents=True, exist_ok=True)
    pages = [p for p in order if (src / (p + ".md")).exists()]
    pages += sorted(p.stem for p in src.glob("*.md")
                    if p.stem not in pages and not p.stem.startswith("README"))
    texts = {p: (src / (p + ".md")).read_text() for p in pages}
    rev = subprocess.run(["git", "-C", str(ROOT), "log", "-1",
                          "--format=%h %cs", "--", str(src)],
                         capture_output=True, text=True).stdout.strip()
    others = "".join('<a href="../%s/index.html">%s</a>' % (f, html.escape(n))
                     for f, n, _ in MANUALS if f != folder)
    for p in pages:
        md = markdown.Markdown(extensions=["tables", "fenced_code", "toc"])
        body = md.convert(texts[p])
        # links between the manual's files, and into the other part
        body = re.sub(r'href="\.\./([a-z0-9-]+)/([a-z0-9-]+)\.md(#[^"]*)?"',
                      lambda m: 'href="../%s/%s.html%s"'
                      % (m.group(1), m.group(2), m.group(3) or ""), body)
        body = re.sub(r'href="([a-z0-9-]+)\.md(#[^"]*)?"',
                      lambda m: 'href="%s.html%s"'
                      % (m.group(1), m.group(2) or ""), body)
        body = tag_spans(body)
        nav = "".join('<a href="%s.html"%s>%s</a>'
                      % (q, ' class="cur"' if q == p else "",
                         html.escape(title_of(texts[q], q)
                                     if q != "index" else "Contents"))
                      for q in pages)
        page = ("<!doctype html><html lang=en><head><meta charset=utf-8>"
                "<meta name=viewport content='width=device-width,"
                "initial-scale=1'><title>%s</title><style>%s</style></head>"
                "<body><nav><h1><a href='../index.html'>iPod Video "
                "hardware manual</a></h1><h1>%s</h1>%s<h1>Other part</h1>%s"
                "</nav><main>%s<p class=foot>Draft, not published. Source: "
                "docs/%s/ at %s.</p></main></body></html>"
                % (html.escape(title_of(texts[p], p)), CSS, html.escape(name),
                   nav, others, body, folder,
                   html.escape(rev or "working tree")))
        (out / (p + ".html")).write_text(page)
    return title_of(texts["index"], folder) if "index" in texts else name


def main():
    OUT_ROOT.mkdir(parents=True, exist_ok=True)
    rev = subprocess.run(["git", "-C", str(ROOT), "log", "-1",
                          "--format=%h %cs", "--", "docs/bcm2722-reference",
                          "docs/pp5022-reference"],
                         capture_output=True, text=True).stdout.strip()
    parts = []
    for folder, name, order in MANUALS:
        if (ROOT / "docs" / folder).is_dir():
            title = render(folder, name, order, rev)
            parts.append('<li><a href="%s/index.html">%s</a><br><span '
                         'class=foot>%s</span></li>'
                         % (folder, html.escape(name), html.escape(title)))
    front = ("<!doctype html><html lang=en><head><meta charset=utf-8>"
             "<meta name=viewport content='width=device-width,"
             "initial-scale=1'><title>iPod Video hardware manual</title>"
             "<style>%s main{max-width:760px} li{margin:12px 0}</style>"
             "</head><body><main><h1>iPod Video 5G/5.5G hardware manual"
             "</h1><p>A reverse-engineered reference to the main platform "
             "and the video co-processor, in two parts. Every fact carries "
             "an evidence tag.</p><ul>%s</ul><p class=foot>Draft, not "
             "published. Sources at %s.</p></main></body></html>"
             % (CSS, "".join(parts), html.escape(rev or "working tree")))
    (OUT_ROOT / "index.html").write_text(front)
    print(OUT_ROOT / "index.html")
    if "--open" in sys.argv:
        subprocess.run(["open", str(OUT_ROOT / "index.html")])


if __name__ == "__main__":
    main()
