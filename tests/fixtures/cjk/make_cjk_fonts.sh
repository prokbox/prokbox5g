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
#
# The ShellCJK companion fonts for the simulator (#266; #235 ships its own):
#   tests/fixtures/cjk/make_cjk_fonts.sh [<dest .rockbox/fonts dir>]
# Downloads Noto Sans CJK JP, SC and TC 2.004 (SIL OFL 1.1) once into build/noto-cjk,
# checked against pinned SHA-256s (JP: the #235 brief; SC, TC: pinned by
# package G2 on first download), and generates the three
# fonts into build/cjk-fonts with apps/ipodui/fonts/gen_cjk.py. The output
# is keyed on everything that shapes it (the generator, the cut list, the
# ShellSans partners' headers, the source hashes and the Pillow/FreeType
# versions): any change regenerates; a missing tool refuses, never a
# weaker font. With a dest dir, copies the .fnt files and the licence in.
set -eu
ROOT=$(cd "$(dirname "$0")/../../.." && pwd -P)
SRC="$ROOT/build/noto-cjk"
OUT="$ROOT/build/cjk-fonts"
BASE=https://raw.githubusercontent.com/notofonts/noto-cjk/main/Sans
SUM_BOLD=e53dcb0dcb2922e45d01aae1ebd2f382bb81d4229b18b6b883bd170678af1f76
SUM_REG=68a3fc98800b2a27b371f2fb79991daf3633bd89309d4ffaa6946fd587f375b5
SUM_SCB=b5f0d1a190a7f9b43c310a8850630af12553df32c4c050543f9059732d9b4c0a
SUM_SCR=2c76254f6fc379fddfce0a7e84fb5385bb135d3e399294f6eeb6680d0365b74b
SUM_TCB=3ee160e5015106e3ec1a394301df54fa9bbbf8a251519984aec5c0abc50840c0
SUM_TCR=dce08bd4fd91aa8aa76ed8fea4b694c2dfb8550f67871e326843212ddbeb88b4

sha() { shasum -a 256 "$1" | cut -d' ' -f1; }
# Parallel first runs (several gate workers or worktrees at once) each
# download into their own temp file next to the target and rename it into
# place, which is atomic on one file system: no run ever reads, checks or
# renames another run's half-written file.
fetch() { # name sum [region dir, default Japanese]
    if [ ! -s "$SRC/$1" ] || [ "$(sha "$SRC/$1")" != "$2" ]; then
        mkdir -p "$SRC"
        part=$(mktemp "$SRC/.$1.XXXXXX")
        curl -sSLf -o "$part" "$BASE/OTF/${3:-Japanese}/$1" \
            || { rm -f "$part"; echo "make_cjk_fonts: download of $1 failed" >&2; exit 1; }
        [ "$(sha "$part")" = "$2" ] \
            || { rm -f "$part"; echo "make_cjk_fonts: $1 SHA-256 mismatch" >&2; exit 1; }
        chmod 644 "$part"
        mv -f "$part" "$SRC/$1"
    fi
}
fetch NotoSansCJKjp-Bold.otf $SUM_BOLD
fetch NotoSansCJKjp-Regular.otf $SUM_REG
fetch NotoSansCJKsc-Bold.otf $SUM_SCB SimplifiedChinese
fetch NotoSansCJKsc-Regular.otf $SUM_SCR SimplifiedChinese
fetch NotoSansCJKtc-Bold.otf $SUM_TCB TraditionalChinese
fetch NotoSansCJKtc-Regular.otf $SUM_TCR TraditionalChinese
if [ ! -s "$SRC/LICENSE" ]; then
    part=$(mktemp "$SRC/.LICENSE.XXXXXX")
    curl -sSLf -o "$part" "$BASE/LICENSE" && [ -s "$part" ] \
        || { rm -f "$part"; echo "make_cjk_fonts: licence download failed" >&2; exit 1; }
    chmod 644 "$part"
    mv -f "$part" "$SRC/LICENSE"
fi

tools=$(python3 -c 'import PIL, PIL.features as f; print(PIL.__version__, f.version("freetype2"), "raqm", f.version("raqm"))') \
    || { echo "make_cjk_fonts: Pillow with FreeType is needed (python3 -m pip install pillow)" >&2; exit 1; }
F="$ROOT/apps/ipodui/fonts"
key=$( { echo "$tools $SUM_BOLD $SUM_REG $SUM_SCB $SUM_SCR $SUM_TCB $SUM_TCR"; cat "$0" "$F/gen_cjk.py" "$F/cjk_cut.py" "$F/cjk-cut-A.txt";
         head -c 36 "$F/15-ShellSans-Bold.fnt"; head -c 36 "$F/12-ShellSans-Bold.fnt";
         head -c 36 "$F/12-ShellSans.fnt"; } | shasum -a 256 | cut -d' ' -f1)
if [ "$(cat "$OUT/.key" 2>/dev/null || true)" != "$key" ]; then
    # Made in a temp dir next to $OUT, then renamed in, for the same reason
    # as fetch(): a parallel run never sees a half-made font set.
    GEN=$(mktemp -d "$OUT.XXXXXX")
    python3 "$F/gen_cjk.py" "$SRC" "$GEN" >&2 || { rm -rf "$GEN"; exit 1; }
    # OFL 1.1 section 2: each copy carries the copyright notice and the
    # licence. The notice is the source fonts' own (their name table,
    # which a bitmap conversion does not keep); LICENSE has no notice.
    { echo "ShellCJK (14-ShellCJK-Bold, 11-ShellCJK-Bold, 11-ShellCJK and"
      echo "35-ShellCJK-Index) are bitmap conversions of Noto Sans CJK JP, SC and TC 2.004"
      echo "(Bold and Regular), made for ProkBox5g by apps/ipodui/fonts/gen_cjk.py."
      echo
      echo "Copyright 2014-2021 Adobe (http://www.adobe.com/). Noto is a"
      echo "trademark of Google Inc."
      echo
      cat "$SRC/LICENSE"; } >"$GEN/OFL-NOTO-CJK.txt"
    echo "$key" >"$GEN/.key"
    if [ "$(cat "$OUT/.key" 2>/dev/null || true)" = "$key" ]; then
        rm -rf "$GEN"                 # a parallel run finished first
    else
        rm -rf "$OUT"
        # If another run renamed its set in meanwhile, mv would nest ours
        # inside it: that complete set stands and ours goes.
        mv "$GEN" "$OUT" 2>/dev/null || true
        rm -rf "$OUT/${GEN##*/}" "$GEN"
    fi
fi
if [ $# -ge 1 ]; then
    mkdir -p "$1"
    cp "$OUT"/*.fnt "$OUT/OFL-NOTO-CJK.txt" "$1/"
    rm -f "$1"/*ShellCJK*.gc      # a stale glyph-cache list from older files
fi

# #235: the CJK-tagged shot library (24 songs) into build/cjk-library
python3 "$ROOT/tests/fixtures/cjk/make_cjk_library.py" "$ROOT/build/cjk-library" >&2
