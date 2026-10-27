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

# Regenerate the shell's bitmap fonts from TeX Gyre Heros.
#
#   apps/ipodui/fonts/gen.sh <dir with texgyreheros-{bold,regular}.otf> [outdir]
#
# Source: https://mirrors.ctan.org/fonts/tex-gyre.zip
#         (tex-gyre/opentype/texgyreheros-{bold,regular}.otf, version 2.004)
# Needs Homebrew freetype + pkg-config to build tools/convttf.c (through
# convttf-shell.c, which adds resolution, tracking and weight knobs; see
# README.md).
# The output is deterministic, so the committed .fnt files can be checked
# by rerunning this and comparing.
set -e
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
SRC=${1:?usage: $0 <heros-otf-dir> [outdir]}
OUT=${2:-"$ROOT/apps/ipodui/fonts"}
TMP=${TMPDIR:-/tmp}/ipodui-convttf.$$

cc -std=c99 -O2 -w "$ROOT/apps/ipodui/fonts/convttf-shell.c" -o "$TMP" -lm \
   $(pkg-config --cflags --libs freetype2)

# -A: real advance widths and bearings
# -X 72 -p N with CONVTTF_{H,V}DPI=73: em N * 73/72 px in both directions
#   (natural Heros proportions), the size at which the hinter puts caps on
#   11 / 9 px and the x-height on 9 / 7 px like main.png
# CONVTTF_TRACK: letter-spacing in px (fractional, averaged over a word),
#   so word widths match main.png within 1-2 px; list 0.3 -> 0.4 (fidelity
#   audit 1 fix #11, 7G words ~1-1.75 px wider; best of a 0.35-0.65 sweep)
# CONVTTF_WEIGHT=0.1: list stems 0.1 px heavier (2.2 px, grey side column, like
#   the 7G; owner pick 2026-10-08, was -0.15)
# CONVTTF_GAP=soft: i/j dots and accents kept clear of the letter, soft edges
#   (CONVTTF_GAP=1: snapped, a clean empty row); LIST_WEIGHT overrides 0.1
# CONVTTF_CP1252=1: curly quotes, en/em dash, ellipsis in the C1 slots
#   (mapped by firmware/font.c, HAVE_IPOD_UI)
# CONVTTF_LSB=clip: ink left of the pen origin is dropped rather than the
#   glyph shifted right ("E ject" in Regular 12); the letters font has no
#   -A (cells cropped to the ink), so it is unaffected
C="$TMP -A -X 72 -s 32 -l 591"
export CONVTTF_HDPI=73 CONVTTF_VDPI=73 CONVTTF_CP1252=1
export CONVTTF_LSB=clip      # negative left bearings: stems stay in place
export CONVTTF_GAP=${CONVTTF_GAP:-soft}  # variants: CONVTTF_GAP=1 (snapped), CONVTTF_HINT=...
CONVTTF_TRACK=0.4 CONVTTF_WEIGHT=${LIST_WEIGHT:-0.1} \
$C -p 15 -o "$OUT/15-ShellSans-Bold.fnt" "$SRC/texgyreheros-bold.otf"    >/dev/null
CONVTTF_TRACK=0.45 \
$C -p 12 -o "$OUT/12-ShellSans-Bold.fnt" "$SRC/texgyreheros-bold.otf"    >/dev/null
CONVTTF_TRACK=0.45 \
$C -p 12 -o "$OUT/12-ShellSans.fnt"      "$SRC/texgyreheros-regular.otf" >/dev/null
# Quick-scroll letter (qscroll.c): A-Z only, cap 26 px, which qscroll.c
# scales to 35 px (bilinear) when drawing; cells cropped to the ink (no
# -A) and trimmed to the capitals plus one row for Q's tail, so it fits
# the 100 KB font budget (7,990 B; the 48 px cut was 17,066 B)
CONVTTF_LSB= "$TMP" -X 72 -s 65 -l 90 -TA 35 -TD 90 \
   -p 35 -o "$OUT/35-ShellSans-Letters.fnt" "$SRC/texgyreheros-bold.otf" >/dev/null
rm -f "$TMP"
ls -l "$OUT"/*.fnt
