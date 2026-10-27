#!/bin/sh
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

# Build the ProkBox5g site and the PDF user guide from docs/user/
# (docs/user/README.md; release-plan section 3.4).
#
# usage: scripts/docs_build.sh [--pdf] [--search] [--version V] [--out DIR]
#   --pdf       also print the guide to DIR/downloads/ProkBox5g-User-Guide.pdf
#               (needs a Chromium browser: $CHROME, google-chrome, chromium,
#               or Brave/Chrome on macOS)
#   --search    index the site with Pagefind ($PAGEFIND, default
#               "pagefind"; the docs image ci/docs/Dockerfile has it)
#   --version   the version printed on the guide (default: site/hugo.toml)
#   --out       output directory (default build/site)
#   --base-url  the site's address (default: site/hugo.toml)
#
# The PDF comes from the same HTML and CSS as the site: the guide section's
# print layout, served locally so its links resolve, printed by Chromium.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$ROOT/build/site PDF=0 SEARCH=0 VER= BASE=
while [ $# -gt 0 ]; do
    case "$1" in
        --pdf) PDF=1 ;;
        --search) SEARCH=1 ;;
        --version) VER=$2; shift ;;
        --out) OUT=$2; shift ;;
        --base-url) BASE=$2; shift ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
    shift
done
[ -n "$VER" ] && export HUGO_PARAMS_VERSION="$VER"
case "$OUT" in /*) ;; *) OUT=$(pwd)/$OUT ;; esac   # hugo resolves relative paths against site/

hugo --source "$ROOT/site" --destination "$OUT" --cleanDestinationDir --minify \
     --panicOnWarning ${BASE:+--baseURL "$BASE"}

if [ $SEARCH = 1 ]; then
    ${PAGEFIND:-pagefind} --site "$OUT"
fi

if [ $PDF = 1 ]; then
    chrome=${CHROME:-}
    for c in google-chrome chromium chromium-browser \
             "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" \
             "/Applications/Brave Browser.app/Contents/MacOS/Brave Browser" \
             "/Applications/Chromium.app/Contents/MacOS/Chromium"; do
        [ -n "$chrome" ] && break
        if command -v "$c" >/dev/null 2>&1 || [ -x "$c" ]; then chrome=$c; fi
    done
    [ -n "$chrome" ] || { echo "no Chromium browser found (set CHROME)" >&2; exit 1; }

    tmp=$(mktemp -d)
    port=$(( 20000 + $$ % 20000 ))
    hugo --source "$ROOT/site" --destination "$tmp/site" --minify \
         --baseURL "http://127.0.0.1:$port/" >/dev/null
    python3 -m http.server "$port" --bind 127.0.0.1 --directory "$tmp/site" >/dev/null 2>&1 &
    srv=$!
    trap 'kill $srv 2>/dev/null; rm -rf "$tmp"' EXIT
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        python3 -c "import urllib.request,sys; urllib.request.urlopen('http://127.0.0.1:$port/print/guide/')" 2>/dev/null && break
        sleep 0.3
    done
    mkdir -p "$OUT/downloads"
    "$chrome" --headless=new --disable-gpu --no-sandbox --no-pdf-header-footer \
        --user-data-dir="$tmp/profile" \
        --print-to-pdf="$OUT/downloads/ProkBox5g-User-Guide.pdf" \
        "http://127.0.0.1:$port/print/guide/" 2>/dev/null
    ls -l "$OUT/downloads/ProkBox5g-User-Guide.pdf"
fi
