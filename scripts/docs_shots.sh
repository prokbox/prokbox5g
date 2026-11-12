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

# Remake the screenshots of the user guide, the site and the README from the
# simulator, as listed in tests/ui/docs/images.tsv (one row per image: the
# image, the sim script that draws it, the shot it takes, the scale, the sim
# build it runs on).
#
#   scripts/docs_shots.sh [--check] [--only RE] [--clip] [--dry-run]
#
#   --check     only check the list (scripts/release_docs.py images, R11):
#               every image under docs/user/images/, site/static/ and
#               site/assets/ has a row, every row's image, script and shot exist
#   --only RE   remake only the rows whose image path matches the awk regex RE
#   --clip      also run the rows whose generator is a shell script (the
#               README's navigation clip, scripts/readme_visuals.sh; slow)
#   --dry-run   print what would run and be written, run nothing
#
# Each sim script runs once (scripts/shots.sh, with SIM_BUILD from its row),
# then each row's shot is copied from out/shots/<script>/<shot>.png to its
# image: 320x240 checked, scale 2 is nearest-neighbour, written fresh with no
# text, time or EXIF chunks. A missing sim build, a failed script or a missing
# shot stops the run with the row that needed it. Synthetic and open media
# only (docs/legal.md L13a): look at every image before committing.
# Needs python3 with Pillow; the clip also needs ffmpeg.
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TSV=tests/ui/docs/images.tsv
ONLY= CLIP=0 DRY=0
while [ $# -gt 0 ]; do
    case "$1" in
        --check) exec python3 "$ROOT/scripts/release_docs.py" images --tree "$ROOT" ;;
        --only) ONLY=$2; shift ;;
        --clip) CLIP=1 ;;
        --dry-run) DRY=1 ;;
        *) echo "docs_shots: unknown option $1" >&2; exit 2 ;;
    esac
    shift
done
cd "$ROOT"
# The first-start notice names the release: the simulator reads the version
# from IPODUI_SIM_VERSION (apps/ipodui/prodver.h, simulator builds only).
# The release's X.Y.Z is the version scripts/version.sh reports without its
# -rc / -dev / +sha suffix; DOCS_SHOTS_VERSION overrides it.
if [ -z "${DOCS_SHOTS_VERSION:-}" ]; then
    DOCS_SHOTS_VERSION=$(sh "$ROOT/scripts/version.sh" 2>/dev/null | sed 's/[-+].*//')
fi
case "$DOCS_SHOTS_VERSION" in
    [0-9]*.[0-9]*.[0-9]*) ;;
    *) echo "docs_shots: no release version (got '$DOCS_SHOTS_VERSION'); set DOCS_SHOTS_VERSION=X.Y.Z" >&2; exit 2 ;;
esac
export IPODUI_SIM_VERSION="$DOCS_SHOTS_VERSION"
python3 scripts/release_docs.py images --tree "$ROOT" >/dev/null || {
    echo "docs_shots: $TSV is not consistent (see above); fix it first" >&2; exit 1; }
python3 -c 'import PIL' 2>/dev/null || { echo "docs_shots: python3 Pillow not found" >&2; exit 2; }

# the rows to remake: image generator shot scale sim
rows=$(grep -v '^[[:space:]]*#' "$TSV" | grep -v '^[[:space:]]*$' | awk -F'\t' -v o="$ONLY" '$1 ~ o')
[ -n "$rows" ] || { echo "docs_shots: no row in $TSV matches /$ONLY/" >&2; exit 1; }

# 1. run each sim script once, on its row's sim build
printf '%s\n' "$rows" | awk -F'\t' '$2 ~ /\.script$/ {print $2 "\t" $5}' | sort -u |
while IFS="$(printf '\t')" read -r script sim; do
    if [ $DRY = 1 ]; then echo "would run: SIM_BUILD=$sim scripts/shots.sh $script"; continue; fi
    [ -x "$sim/rockboxui" ] || {
        echo "docs_shots: $sim/rockboxui is missing ($script needs it); configure and build it as scripts/validate.sh does (its configure line for $(basename "$sim"))" >&2
        exit 1; }
    echo "docs_shots: SIM_BUILD=$sim scripts/shots.sh $script"
    SIM_BUILD=$sim scripts/shots.sh "$script" >/dev/null || {
        echo "docs_shots: $script failed on $sim (see out/shots/$(basename "$script" .script)/)" >&2; exit 1; }
    # "# log: TEXT" / "# nolog: TEXT" header lines: the run's log must (not) hold TEXT,
    # so a view the script cannot wait for (the visualiser) never becomes a wrong image
    slog=out/shots/$(basename "$script" .script)/sim.log
    sed -n 's/^#[[:space:]]*log:[[:space:]]*//p' "$script" | while IFS= read -r want; do
        grep -qF -- "$want" "$slog" || {
            echo "docs_shots: $script: its log has no \"$want\" ($slog)" >&2; exit 1; }
    done || exit 1
    sed -n 's/^#[[:space:]]*nolog:[[:space:]]*//p' "$script" | while IFS= read -r bad; do
        if grep -qF -- "$bad" "$slog"; then
            echo "docs_shots: $script: its log has \"$bad\" ($slog)" >&2; exit 1
        fi
    done || exit 1
done

# 2. copy each row's shot to its image
printf '%s\n' "$rows" | awk -F'\t' '$2 ~ /\.script$/' | python3 -c '
import os, sys
from PIL import Image
dry = sys.argv[1] == "1"
for line in sys.stdin:
    img, gen, shot, scale, sim = line.rstrip("\n").split("\t")[:5]
    src = os.path.join("out/shots", os.path.basename(gen)[:-len(".script")], shot + ".png")
    if dry:
        print("would write %s from %s (scale %s)" % (img, src, scale))
        continue
    if not os.path.isfile(src):
        sys.exit("docs_shots: %s: %s took no shot %s (%s missing)" % (img, gen, shot, src))
    im = Image.open(src).convert("RGB")
    if im.size != (320, 240):
        sys.exit("docs_shots: %s: %s is %dx%d, want 320x240" % (img, src, im.width, im.height))
    if scale == "2":
        im = im.resize((640, 480), Image.NEAREST)
    os.makedirs(os.path.dirname(img), exist_ok=True)
    im.save(img, optimize=True, compress_level=9)   # a fresh image: no text, time or EXIF chunks
    print("%s %d bytes (%s, shot %s)" % (img, os.path.getsize(img), gen, shot))
' "$DRY"

# 3. generators that write their image themselves (the README clip)
printf '%s\n' "$rows" | awk -F'\t' '$2 !~ /\.script$/ {print $2 "\t" $5}' | sort -u |
while IFS="$(printf '\t')" read -r gen sim; do
    if [ $CLIP = 0 ]; then echo "docs_shots: skipped $gen (add --clip to run it)"; continue; fi
    if [ $DRY = 1 ]; then echo "would run: SIM_BUILD=$sim $gen"; continue; fi
    SIM_BUILD=$sim sh "$gen" || { echo "docs_shots: $gen failed" >&2; exit 1; }
done
