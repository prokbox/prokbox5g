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

# Regenerate the README's screenshots and navigation clip.
#
#   scripts/readme_visuals.sh
#
# Runs tests/ui/readme/readme-stills.script and readme-nav.script (in a
# subdirectory, so the gate's tests/ui/*.script sweep leaves them alone:
# they want the library and the video fixtures on one disk) on the
# release-shaped simulator with the video player (SIM_BUILD, default
# build/sim-rel-video: tools/configure --target=22 --type=s --ipodui
# --ipodui-release --bcmfw-video --bcmfw-merged), over the synthetic test
# library and video fixtures only (tests/ui/setup_library.py,
# tests/fixtures/videos/mkvideos.sh). Writes to docs/user/images/readme/:
#   main-menu.png albums.png now-playing.png videos.png settings.png usb.png
#       2x nearest-neighbour (640x480), lossless, no metadata chunks
#   navigation.gif
#       the clip at 20 fps, 2x nearest-neighbour, one global palette of
#       the most colours (128 down to 64) that keeps it under GIF_MAX
#       bytes (default 1,500,000), looping
# Needs ffmpeg and python3 with Pillow. Check every frame before
# committing: only made-up names and procedural art may appear.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SIM_BUILD=${SIM_BUILD:-build/sim-rel-video}
export SIM_BUILD
FPS=20                          # tests/ui/readme/readme-nav.script's record_start
GIF_MAX=${GIF_MAX:-1500000}
DEST="$ROOT/docs/user/images/readme"
SHOTS="$ROOT/out/shots"

command -v ffmpeg >/dev/null 2>&1 || { echo "ffmpeg not found" >&2; exit 2; }
python3 -c 'import PIL' 2>/dev/null || { echo "python3 Pillow not found" >&2; exit 2; }

"$ROOT/scripts/shots.sh" "$ROOT/tests/ui/readme/readme-stills.script" >/dev/null
frames="$SHOTS/readme-nav/nav_frames"
rm -rf "$frames"                # shots.sh clears only the top-level shots
"$ROOT/scripts/shots.sh" "$ROOT/tests/ui/readme/readme-nav.script" >/dev/null
n=$(ls "$frames"/frame*.bmp 2>/dev/null | wc -l | tr -d ' ')
[ "$n" -gt 0 ] || { echo "no clip frames in $frames" >&2; exit 1; }

mkdir -p "$DEST"
python3 - "$SHOTS/readme-stills" "$DEST" <<'EOF'
import os, sys
from PIL import Image
src, dest = sys.argv[1], sys.argv[2]
# shot name -> README file name
STILLS = [('main_menu', 'main-menu'), ('albums', 'albums'),
          ('now_playing', 'now-playing'), ('videos', 'videos'),
          ('settings', 'settings'), ('usb', 'usb')]
for shot, name in STILLS:
    im = Image.open(os.path.join(src, shot + '.png')).convert('RGB')
    if im.size != (320, 240):
        sys.exit('%s: %dx%d, want 320x240' % (shot, im.width, im.height))
    im = im.resize((640, 480), Image.NEAREST)
    out = os.path.join(dest, name + '.png')
    # a fresh image: no text, time or EXIF chunks are written
    im.save(out, optimize=True, compress_level=9)
    print('%s %d bytes' % (out, os.path.getsize(out)))
EOF

# The panel drift repaints most of the right half ~13 times a second, so
# the clip is costly as a GIF: take the largest palette (no dither: flat
# UI colours stay exact) that keeps it under GIF_MAX.
gif="$DEST/navigation.gif"
for colours in 128 112 96 88 80 72 64; do
    ffmpeg -y -loglevel error -framerate $FPS -i "$frames/frame%05d.bmp" \
        -filter_complex "scale=iw*2:ih*2:flags=neighbor,split[a][b];[a]palettegen=max_colors=$colours:stats_mode=full[p];[b][p]paletteuse=dither=none:diff_mode=rectangle" \
        -loop 0 -map_metadata -1 "$gif"
    size=$(wc -c <"$gif" | tr -d ' ')
    [ "$size" -le "$GIF_MAX" ] && break
done
echo "$gif $size bytes, $n frames at $FPS fps, $colours colours"
[ "$size" -le "$GIF_MAX" ] || { echo "navigation.gif is over $GIF_MAX bytes" >&2; exit 1; }
