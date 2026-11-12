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
# usage: scripts/docs_build.sh [--pdf] [--search] [--version V] [--release] [--out DIR]
#                              [--base-url URL] [--tree DIR]
#   --pdf       also print the guide to DIR/downloads/ProkBox5g-User-Guide.pdf
#               (needs a Chromium browser: $CHROME, google-chrome, chromium,
#               or Brave/Chrome on macOS)
#   --search    index the site with Pagefind ($PAGEFIND, default
#               "pagefind"; the docs image ci/docs/Dockerfile has it)
#   --version   the version printed on the guide.  Default: scripts/version.sh
#               (the release tag), else $VERSION (what release_build.sh uses),
#               else site/hugo.toml's "dev" (a local preview: the build says so)
#   --release   also refuse unless the docs are release-ready for that version
#               (scripts/release_docs.py check: notes published, site/hugo.toml
#               version, changelog), and unless the built site shows the version
#   --out       output directory (default build/site)
#   --base-url  the site's address (default: site/hugo.toml)
#   --tree      build another source tree's site/ and docs/ (default: this
#               checkout).  scripts/docs_versions.py passes each release tag's
#               exported tree, so an old release keeps its own layouts
#
# The PDF comes from the same HTML and CSS as the site: the guide section's
# print layout, served locally so its links resolve, printed by Chromium.
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$ROOT/build/site PDF=0 SEARCH=0 VER= BASE= REL=0 TREE=$ROOT
while [ $# -gt 0 ]; do
    case "$1" in
        --pdf) PDF=1 ;;
        --search) SEARCH=1 ;;
        --release) REL=1 ;;
        --version) VER=$2; shift ;;
        --out) OUT=$2; shift ;;
        --base-url) BASE=$2; shift ;;
        --tree) TREE=$2; shift ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
    shift
done
case "$TREE" in /*) ;; *) TREE=$(pwd)/$TREE ;; esac
[ -f "$TREE/site/hugo.toml" ] || { echo "docs: $TREE has no site/hugo.toml" >&2; exit 1; }
# the one place that decides the version (scripts/version.sh)
docs_version() {
    if [ -f "$ROOT/scripts/version.sh" ]; then
        sh "$ROOT/scripts/version.sh" || { echo "docs: scripts/version.sh failed" >&2; exit 1; }
    else
        echo "${VERSION:-}"
    fi
}
[ -n "$VER" ] || VER=$(docs_version)
if [ -n "$VER" ]; then
    export HUGO_PARAMS_VERSION="$VER"
else
    echo "docs: no --version or \$VERSION: preview build, version "dev" from site/hugo.toml" >&2
    [ $REL = 0 ] || { echo "docs: --release needs a version (--version, scripts/version.sh or \$VERSION)" >&2; exit 1; }
fi
if [ $REL = 1 ]; then
    # an RC (X.Y.Z-rc.N) uses its base version's notes page and may still be a draft
    base=${VER%%-rc*}; rcflag=; [ "$base" = "$VER" ] || rcflag=--rc
    python3 "$ROOT/scripts/release_docs.py" check "$base" $rcflag --tree "$TREE"
fi

# Writers' notes never reach a reader: the site, the PDF and the release notes
# are built from docs/user (0.8.0 shipped eleven "[check on device]" markers).
# A release note's line held for a bench result is an HTML comment
# "<!-- RELEASE-FILL: ... -->": invisible on the page, so it is refused here
# until it is filled or removed (0.8.1's line for #219)
marks=$(cd "$TREE" && grep -rnE '\[(check on device|TODO|TBD|FIXME)[^]]*\]|RELEASE-FILL' docs/user README.md) || rc=$?
case "${rc:-0}" in
    0) echo "docs: writers' notes left in the user docs:" >&2; echo "$marks" >&2; exit 1 ;;
    1) ;;
    *) echo "docs: the writers'-note check failed to run" >&2; exit 1 ;;
esac
case "$OUT" in /*) ;; *) OUT=$(pwd)/$OUT ;; esac   # hugo resolves relative paths against site/

hugo --source "$TREE/site" --destination "$OUT" --cleanDestinationDir --minify \
     --panicOnWarning ${BASE:+--baseURL "$BASE"}

if [ $REL = 1 ] && ! grep -q "latest: $VER" "$OUT/index.html"; then
    echo "docs: the built site's home page does not say \"latest: $VER\"" >&2; exit 1
fi

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
    trap 'rm -rf "$tmp"' EXIT
    port=$(( 20000 + $$ % 20000 ))
    hugo --source "$TREE/site" --destination "$tmp/site" --minify \
         --baseURL "http://127.0.0.1:$port/" >/dev/null
    # a tree without the print layout would print a 404 page: refuse instead
    [ -f "$tmp/site/print/guide/index.html" ] || { echo "docs: $TREE/site has no print layout (print/guide/): no PDF" >&2; exit 1; }
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
