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

# Drive the simulator with a UI script and collect PNG screenshots.
#
#   scripts/shots.sh <script> [outdir]
#
# Runs build/sim/rockboxui headless (SDL dummy video driver) with
# --script <script>, writes screenshots to outdir (default
# out/shots/<scriptname>/), converts them to PNG and prints their paths.
# The simulator is killed if it runs longer than SHOTS_TIMEOUT seconds
# (default 60). Set SHOTS_WINDOW=1 to show the simulator window, and
# SIM_BUILD=<dir> to use another sim build directory (default build/sim).
# Scripts named ipodui-* or containing a "# needs: music" line get the test
# mini library (tests/ui/setup_music.sh) in $SIM_BUILD/simdisk/Music first.
# ipodui-p14a-* scripts and "# needs: video" ones get the Phase 14a video
# fixtures (tests/fixtures/videos/mkvideos.sh) and a built database.
# Scripts containing "# needs: library" get the Phase 4 test library
# (tests/ui/setup_library.sh: ~300 tracks, podcasts, audiobooks, playlists,
# tagcache config keys) in $SIM_BUILD/simdisk. "# fresh-db" deletes the
# database files (simdisk/.rockbox/database_*.tcd, and the statistics a
# rebuild would import: database_changelog.txt) before boot, so the sim
# starts as on a first boot with a new library. "# drop-db" deletes them
# after the run, for a script that leaves the database in a state the
# next library script must not inherit (no script needs it at present).
# Other library scripts get a built database: when there is none, a
# priming boot runs "build_db" first.
# Resume/playback state (.rockbox/.resume.cfg) is cleared before boot, so
# "Now Playing" visibility depends only on this script's own commands;
# "# resume: keep" inherits it instead, "# resume: playing" seeds a
# minimal one (see tests/ui/README.md). The boot stamp
# (.rockbox/boot_stamp.dat, #89) is deleted too unless "# boot-stamp: keep",
# and the On-The-Go store (.rockbox/ipodui/otg.*) unless "# keep: otg".
# Lines "# env: VAR=value" in the script are exported to the simulator
# (e.g. IPODUI_NO_STRIP=1, see tests/ui/README.md).
# A scripted run freezes the split-screen panel phase (drift framing and
# cycle, #38, apps/ipodui/preview.c) and pins the panels' clock/date
# (uisimulator/common/sim_cmd_clock.c), as it pins the battery, so
# goldens never depend on timing; "# drift: live" lets the drift run
# (exports IPODUI_SIM_DRIFT=live), and the "drift_phase" command sets it.
# "# timeout: <seconds>" raises SHOTS_TIMEOUT for one long script (never
# lowers it).
# A "wait_db", "update_db_wait" or "rebuild_db" script command that times
# out fails the run, and so does a failed "expect_*" or
# "update_db_wait_change" command. A "reboot" command runs the rest of the
# script in a new boot of the sim (same disk; logs joined in sim.log).
# SIMDISK=<dir> points the sim at another disk image instead of
# $SIM_BUILD/simdisk (--root, see firmware/target/hosted/sdl/system-sdl.c),
# so a parallel gate can give each worker its own copy. It must already
# have $SIMDISK/.rockbox installed (e.g. cloned from a primed simdisk);
# only the default disk is auto-installed here.
# SDL_AUDIODRIVER defaults to "dummy" here: the sim's playback position
# runs on the audio device's clock, and under host load (coreaudiod
# starved, many sims at once) the real device fell 15-90% behind the wall
# clock, so expect_pos/resume windows missed. The dummy driver paces the
# same callbacks in real time without the host device. Export another
# driver to override (e.g. SDL_AUDIODRIVER=coreaudio to listen).
# See uisimulator/common/sim_script.c for the script format.
set -e
export SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-dummy}
# The host-jitter watcher (uisimulator/common/sim_cmd_load.c) logs how late the
# host wakes a 10 ms sleeper: "sim_load: run jitter ..." at the end, which
# scripts/check_trace.py uses to class a late-frame failure as starved.
export SIM_LOAD_WATCH=${SIM_LOAD_WATCH:-1}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
# ipodui-p14a-* scripts (Phase 14a video) run on the sim with the video
# player by default (build/sim-ui-video); SIM_BUILD overrides, as always.
case "$(basename "${1:-}" .script)" in
    ipodui-p14a-*) DEFSIM=build/sim-ui-video ;;
    *) DEFSIM=build/sim ;;
esac
SIM=${SIM_BUILD:-$DEFSIM}
case "$SIM" in /*) ;; *) SIM="$ROOT/$SIM" ;; esac
TIMEOUT=${SHOTS_TIMEOUT:-60}
SIMDISK=${SIMDISK:-"$SIM/simdisk"}
case "$SIMDISK" in /*) ;; *) SIMDISK="$ROOT/$SIMDISK" ;; esac

[ -n "$1" ] || { echo "usage: $0 <script> [outdir]" >&2; exit 2; }
[ -f "$1" ] || { echo "no such script: $1" >&2; exit 2; }
[ -x "$SIM/rockboxui" ] || { echo "build the sim first: $SIM" >&2; exit 2; }

script=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
name=$(basename "$1" .script)
out=${2:-"$ROOT/out/shots/$name"}
mkdir -p "$out"
out=$(cd "$out" && pwd)
rm -f "$out"/*.bmp "$out"/*.png

# Populate simdisk/.rockbox on first use (the install target always
# writes to the default disk; a SIMDISK override must come pre-installed,
# normally by cloning the default disk after this runs once)
if [ ! -d "$SIM/simdisk/.rockbox/codecs" ]; then
    make -C "$SIM" install >/dev/null
fi
if [ "$SIMDISK" != "$SIM/simdisk" ] && [ ! -d "$SIMDISK/.rockbox/codecs" ]; then
    echo "SIMDISK=$SIMDISK has no .rockbox/codecs;" \
         "clone it from a primed $SIM/simdisk first" >&2
    exit 2
fi

# Mini music library (idempotent; instant when already set up)
case "$name" in
    ipodui-*) needs_music=1 ;;
    *) grep -qE '^#[[:space:]]*needs:[[:space:]]*music' "$script" \
           && needs_music=1 || needs_music=0 ;;
esac
if [ $needs_music = 1 ]; then
    "$ROOT/tests/ui/setup_music.sh" "$SIMDISK"
fi
# Phase 4 test library (idempotent, version-stamped)
grep -qE '^#[[:space:]]*needs:[[:space:]]*library' "$script" \
    && needs_library=1 || needs_library=0
grep -qE '^#[[:space:]]*fresh-db([[:space:]]|$)' "$script" \
    && fresh_db=1 || fresh_db=0
grep -qE '^#[[:space:]]*drop-db([[:space:]]|$)' "$script" \
    && drop_db=1 || drop_db=0
# "# stale-import": fresh-db plus the leftovers of an old build (see below)
grep -qE '^#[[:space:]]*stale-import([[:space:]]|$)' "$script" \
    && stale_import=1 || stale_import=0
[ $stale_import = 1 ] && fresh_db=1
# "# stale-changelog: unstamped": the changelog as builds before round 7b
# wrote it (no "## commit" line, the old database's commit IDs)
grep -qE '^#[[:space:]]*stale-changelog:[[:space:]]*unstamped' "$script" \
    && stale_unstamped=1 || stale_unstamped=0
if [ $needs_library = 1 ]; then
    "$ROOT/tests/ui/setup_library.sh" "$SIMDISK"
fi
# Phase 5 Now Playing fixtures (lyrics, wide/tall/banner covers, a book
# with chapter images) under build/np-fixtures, idempotent; scripts copy
# them into the sim disk with "copy ../../build/np-fixtures/<f> /<dst>"
if grep -qE '^#[[:space:]]*needs:[[:space:]]*np' "$script"; then
    python3 "$ROOT/tests/fixtures/np/make_np_fixtures.py" \
        || { echo "np fixtures failed" >&2; exit 2; }
fi
# P19 19-1c playback fixtures (long and hi-res FLAC, MP3, a gapless pair)
# under build/p19c-fixtures, idempotent; the ipodui-p19c-* scripts (or
# "# needs: p19c") copy them in with "copy ../../build/p19c-fixtures/<f>"
case "$name" in
    ipodui-p19c-*) needs_p19c=1 ;;
    *) grep -qE '^#[[:space:]]*needs:[[:space:]]*p19c' "$script" \
           && needs_p19c=1 || needs_p19c=0 ;;
esac
if [ $needs_p19c = 1 ]; then
    sh "$ROOT/tests/fixtures/p19c/make_p19c_fixtures.sh" \
        || { echo "p19c fixtures failed" >&2; exit 2; }
fi
# Phase 6 audiobook fixtures (chaptered m4b, no chapters, multi-file,
# ffmpeg CTOC, 1,100 chapters, UTF-8 cuts) under build/ab-fixtures,
# idempotent; scripts copy them in with "copy ../../build/ab-fixtures/<f>"
if grep -qE '^#[[:space:]]*needs:[[:space:]]*ab' "$script"; then
    python3 "$ROOT/tests/fixtures/ab/make_ab_fixtures.py" \
        || { echo "ab fixtures failed" >&2; exit 2; }
fi
# Phase 14a video fixtures (tests/fixtures/videos/mkvideos.sh: movies, TV,
# podcast episodes, a High-profile file, ...) generated once under
# build/video-fixtures (idempotent, cached) and copied into the sim disk.
# ipodui-p14a-* scripts always get them, others with "# needs: video"; like
# a library script they then also want a built database that indexed them.
case "$name" in
    ipodui-p14a-*) needs_video=1 ;;
    *) grep -qE '^#[[:space:]]*needs:[[:space:]]*video' "$script" \
           && needs_video=1 || needs_video=0 ;;
esac
if [ $needs_video = 1 ]; then
    "$ROOT/tests/fixtures/videos/mkvideos.sh" --install "$SIMDISK" \
        || { echo "video fixtures failed" >&2; exit 2; }
fi
# First boot with a fresh library: no database yet
if [ $fresh_db = 1 ]; then
    rm -f "$SIMDISK"/.rockbox/database_*.tcd \
          "$SIMDISK"/.rockbox/database_gen[012]/database* \
          "$SIMDISK"/.rockbox/database_manifest[01].* \
          "$SIMDISK"/.rockbox/database_stats.jnl \
          "$SIMDISK"/.rockbox/database_stats.prev \
          "$SIMDISK"/.rockbox/database_stats.tmp \
          "$SIMDISK"/.rockbox/database_lib.dat \
          "$SIMDISK"/.rockbox/database_changelog.txt* \
          "$SIMDISK"/.rockbox/database_import.pending
    # An old build's leftovers: the import marker and an empty tmp file,
    # with no manifest and no database (the legacy-root boot state), and
    # its complete changelog: one known track played 5 times, which the
    # import after the initial build must restore.
    if [ $stale_import = 1 ]; then
        mkdir -p "$SIMDISK/.rockbox"
        : >"$SIMDISK/.rockbox/database_import.pending"
        : >"$SIMDISK/.rockbox/database_tmp.tcd"
        if [ $stale_unstamped = 1 ]; then
            printf '%s\n' '## Changelog version 1' \
                'filename="/Library/Basalt/Columns/01 Narrow Echo.mp3" playcount="5" rating="0" playtime="60000" lastplayed="1" commitid="40" lastelapsed="0" lastoffset="0" ' \
                '## end' >"$SIMDISK/.rockbox/database_changelog.txt"
        else
            printf '%s\n' '## Changelog version 1' '## commit 1' \
                'filename="/Library/Basalt/Columns/01 Narrow Echo.mp3" playcount="5" rating="0" playtime="60000" lastplayed="1" commitid="1" lastelapsed="0" lastoffset="0" ' \
                '## end' >"$SIMDISK/.rockbox/database_changelog.txt"
        fi
    fi
fi

# Resume/playback state (apps/settings.c RESUMEFILE, ".rockbox/.resume.cfg")
# survives on the disk across boots; left alone, a script's "Now Playing"
# visibility (apps/ipodui/ui_main.c ui_now_playing_visible(): audio
# playing, OR resume_index != -1) would depend on whatever ran earlier on
# this simdisk, not on this script's own commands. So every script starts
# from a clean resume state unless it says otherwise: "# resume: keep"
# leaves the file as is (inherit it, e.g. across this script's own
# "reboot" parts, which never re-run this block); "# resume: playing"
# seeds a minimal one (resume_index 0) plus ".sim_np_row" (the sim build
# counts that queue as played this boot: after a boot the row otherwise
# waits for playback, owner #58) so the main menu's "Now Playing"
# row is deterministically visible without the script having to start
# playback itself. That seed is not a real, resumable playlist position
# (no valid crc/playlist) -- it only satisfies the visibility check above;
# a script that resumes it into the WPS needs the real thing instead.
if grep -qE '^#[[:space:]]*resume:[[:space:]]*keep([[:space:]]|$)' "$script"; then
    :
elif grep -qE '^#[[:space:]]*resume:[[:space:]]*playing([[:space:]]|$)' "$script"; then
    printf 'IDX: 0\r\nELA: 0\r\nOFF: 0\r\nPLM: 0\r\n' >"$SIMDISK/.rockbox/.resume.cfg"
    : >"$SIMDISK/.rockbox/.sim_np_row"   # counts as played this boot (ui_main.c)
    rm -f "$SIMDISK/.rockbox/.resume.cfg.new" "$SIMDISK/.rockbox/.resume.cfg.old"
else
    rm -f "$SIMDISK"/.rockbox/.resume.cfg "$SIMDISK"/.rockbox/.sim_np_row \
          "$SIMDISK"/.rockbox/.resume.cfg.new \
          "$SIMDISK"/.rockbox/.resume.cfg.old
fi

# The sim's panic/hang record (firmware/panic.c: the stand-in for RAM a
# reset keeps) is per disk; a script starts with none, as after a power-on,
# so one script's record never shows in the next's report or boot log. A
# script's own "reboot" parts keep it (this block runs once).
rm -f "$SIMDISK/.rockbox/.sim_panic_record.bin"

# The no-change boot stamp (#89, apps/ipodui/bootstamp.c: written at a
# clean shutdown, read and removed at the next boot) survives on the disk
# too: left alone, whether a script's first boot walks the library would
# depend on how the previous script on this simdisk ended. Every script
# starts with none (its first boot walks, "nostamp") unless it says
# "# boot-stamp: keep". The temporary of a cut write goes as well.
if ! grep -qE '^#[[:space:]]*boot-stamp:[[:space:]]*keep([[:space:]]|$)' "$script"; then
    rm -f "$SIMDISK/.rockbox/boot_stamp.dat" "$SIMDISK/.rockbox/boot_stamp.tmp"
fi
# The On-The-Go store (.rockbox/ipodui/otg.*) likewise: every script starts
# with an empty On-The-Go list unless it says "# keep: otg".
if ! grep -qE '^#[[:space:]]*keep:[[:space:]]*otg([[:space:]]|$)' "$script"; then
    rm -f "$SIMDISK"/.rockbox/ipodui/otg.*
fi
# Extras > Clocks (P12 W1-C): the clocks list and the home time zone live
# in config.cfg, so the Date & Time and Clocks scripts would inherit each
# other's cities. "# reset: clocks" starts a script with the defaults (one
# clock, the default city, New York).
if grep -qE '^#[[:space:]]*reset:[[:space:]]*clocks([[:space:]]|$)' "$script"; then
    # the settings' safe write may have left its .new/.old, which a boot
    # takes back: every copy loses the two lines
    for f in "$SIMDISK/.rockbox/config.cfg" "$SIMDISK/.rockbox/config.cfg.new" \
             "$SIMDISK/.rockbox/config.cfg.old"; do
        [ -f "$f" ] || continue
        { grep -v -E '^ipodui (clocks|time zone):' "$f" || true; } \
            >"$f.tmp" && mv -f "$f.tmp" "$f"
    done
fi
# The visualizer's style and the charger's face (#126) persist in
# config.cfg too: a script that picked Mirror or Visualizer would hand it
# to the next. "# reset: viz" starts a script from Bars and Clock.
if grep -qE '^#[[:space:]]*reset:[[:space:]]*viz([[:space:]]|$)' "$script"; then
    for f in "$SIMDISK/.rockbox/config.cfg" "$SIMDISK/.rockbox/config.cfg.new" \
             "$SIMDISK/.rockbox/config.cfg.old"; do
        [ -f "$f" ] || continue
        { grep -v -E '^ipodui (viz style|charge face):' "$f" || true; } \
            >"$f.tmp" && mv -f "$f.tmp" "$f"
    done
fi
# Settings > Main Menu / Music Menu persist in config.cfg as well: a
# checklist script that ticked a row moves every row below it, and a
# script that counts wheel steps from the Main Menu lands one row off (P12
# integration: Extras > Alarms opened Podcasts). "# reset: mainmenu"
# starts a script from the default Main and Music Menus.
if grep -qE '^#[[:space:]]*reset:[[:space:]]*mainmenu([[:space:]]|$)' "$script"; then
    for f in "$SIMDISK/.rockbox/config.cfg" "$SIMDISK/.rockbox/config.cfg.new" \
             "$SIMDISK/.rockbox/config.cfg.old"; do
        [ -f "$f" ] || continue
        { grep -v -E '^ipodui (main|music) menu:' "$f" || true; } \
            >"$f.tmp" && mv -f "$f.tmp" "$f"
    done
fi
# The per-book resume store (artcache/books.dat) is kept by default (many
# book scripts build on it); "# reset: books" starts a script without it.
if grep -qE '^#[[:space:]]*reset:[[:space:]]*books([[:space:]]|$)' "$script"; then
    rm -f "$SIMDISK"/.rockbox/artcache/books.dat "$SIMDISK"/.rockbox/artcache/books.old "$SIMDISK"/.rockbox/artcache/books.tmp
fi

# Shuffle state persists the same way (config.cfg "shuffle: on", and the
# library shuffle record npshuf.dat): a script that left Shuffle on would
# change the next script's queue, panel art and Settings row. Start every
# script from an explicit "shuffle: off" (deleting the line gives the
# default, which is ON) unless it keeps the resume state.
if ! grep -qE '^#[[:space:]]*resume:[[:space:]]*keep([[:space:]]|$)' "$script"; then
    rm -f "$SIMDISK/.rockbox/npshuf.dat"
    if [ -f "$SIMDISK/.rockbox/config.cfg" ]; then
        { grep -v '^shuffle:' "$SIMDISK/.rockbox/config.cfg"; echo 'shuffle: off'; } \
            >"$SIMDISK/.rockbox/config.cfg.tmp" \
            && mv -f "$SIMDISK/.rockbox/config.cfg.tmp" "$SIMDISK/.rockbox/config.cfg"
    fi
fi

# The backlight timeout persists too: "backlight_expire" saves 15 s, and a
# later script on the same disk then idles into the unlit face. Every
# script starts from the sim default (Always On) by dropping both lines.
# P12 Screen Lock: likewise its combination and lock (a lock script that
# stopped half way would lock every later script). config.cfg.new too: a
# write the last run left there replaces config.cfg at the next boot.
for cfg in "$SIMDISK/.rockbox/config.cfg" "$SIMDISK/.rockbox/config.cfg.new"; do
    [ -f "$cfg" ] || continue
    { grep -v -E '^(backlight timeout( plugged)?|ipodui lock code|ipodui locked):' "$cfg" || true; } \
        >"$cfg.tmp" && mv -f "$cfg.tmp" "$cfg"
done

[ -n "$SHOTS_WINDOW" ] || export SDL_VIDEODRIVER=dummy
# "# env: VAR=value" directives
for kv in $(sed -n 's/^#[[:space:]]*env:[[:space:]]*//p' "$script"); do
    case "$kv" in
        [A-Za-z_]*=*) export "$kv" ;;
        *) echo "bad env directive: $kv" >&2; exit 2 ;;
    esac
done
# Phase 8a boot hooks (docs/phase8a-brief.md section 8):
# "# boot_keys: RIGHT 2500" holds those buttons (NAME[+NAME]) for that many
# ms from the first button poll of every boot, via IPODUI_SIM_BOOT_KEYS
# (firmware/target/hosted/sdl/button-sdl.c): the boot-escape finger.
# "# boot_shot" exports IPODUI_SHOT_DIR, so sim_capture_screen()
# (uisimulator/common/sim_capture.c, called from show_logo_boot()) writes
# <outdir>/boot-fw.png at the hand-off; without it the call is a no-op.
bk=$(sed -n 's/^#[[:space:]]*boot_keys:[[:space:]]*\([A-Z+]*\)[[:space:]][[:space:]]*\([0-9][0-9]*\).*/\1:\2/p' \
     "$script" | head -n 1)
if [ -n "$bk" ]; then
    export IPODUI_SIM_BOOT_KEYS="$bk"
elif grep -qE '^#[[:space:]]*boot_keys:' "$script"; then
    echo "bad boot_keys directive (want '# boot_keys: NAME[+NAME] <ms>')" >&2
    exit 2
fi
if grep -qE '^#[[:space:]]*boot_shot([[:space:]]|$)' "$script"; then
    export IPODUI_SHOT_DIR="$out"
fi
# "# drift: live" (see above)
if grep -qE '^#[[:space:]]*drift:[[:space:]]*live' "$script"; then
    export IPODUI_SIM_DRIFT=live
fi
# "# timeout: <seconds>" (see above)
script_to=$(sed -n 's/^#[[:space:]]*timeout:[[:space:]]*\([0-9][0-9]*\).*/\1/p' \
            "$script" | head -n 1)
if [ -n "$script_to" ] && [ "$script_to" -gt "$TIMEOUT" ]; then
    TIMEOUT=$script_to
fi
log="$out/sim.log"

cd "$SIM"
# run_sim <script> <log>: one headless sim boot under the watchdog; sets
# $status (137 = killed by the watchdog)
#
# The watchdog is a single `sleep $TIMEOUT` (process audit 2 #8: no more
# polling every 1s to ask "is it still alive?", which added up to 1s of
# pure dead time to every run across every gate script and every
# priming boot). It fires at most once; `wait $pid` below returns the
# instant the sim itself exits, with zero polling latency either way.
# The watchdog is killed right after (before it can race a reused pid
# against a since-exited $pid) and reaped quietly.
run_sim() {
    ./rockboxui --nobackground --root "$SIMDISK" --script "$1" \
        --shotdir "$out" >"$2" 2>&1 &
    pid=$!
    # stdout/stderr to /dev/null: the orphaned sleep must not hold a caller's
    # pipe ($(shots.sh ...)) open for the whole timeout after the sim exits
    ( sleep "$TIMEOUT"; kill -9 $pid ) >/dev/null 2>&1 &
    wdpid=$!
    status=0
    wait $pid || status=$?
    # Reap the watchdog without letting ITS exit status (it was just
    # signalled) leak out of this function under `set -e` -- callers
    # only care about $status above.
    pkill -P "$wdpid" 2>/dev/null || true
    # the watchdog may already be gone (pkill raced it): under set -e a
    # failing kill here aborted the whole run silently (rc=1, no output)
    kill "$wdpid" 2>/dev/null || true
    wait "$wdpid" 2>/dev/null || true
}

# P12: the sim's PMU model (uisimulator/common/sim_cmd_extras.c) keeps the
# alarm registers, RTCWAK and the RTC offset across the "reboot" parts of
# one run in this file; it starts every run empty (a stale one from an
# earlier run would arm a wake that script never made).
export SIM_PMU_STATE="$out/.pmu-state"
rm -f "$SIM_PMU_STATE"
# Likewise the alarms (.rockbox/ipodui/alarms.*, apps/ipodui/alarm.c): a
# run starts with none, so an alarm one script made never rings in the
# next, and a script that counts the Alarms list's rows sees only its
# own. "# keep: alarms" inherits them.
if ! grep -qE '^#[[:space:]]*keep:[[:space:]]*alarms([[:space:]]|$)' "$script"; then
    rm -f "$SIMDISK"/.rockbox/ipodui/alarms.dat "$SIMDISK"/.rockbox/ipodui/alarms.tmp \
          "$SIMDISK"/.rockbox/ipodui/alarms.old
fi

# Library scripts start with a built database (except "# fresh-db" ones),
# made by a priming boot (build_db, wait_db). The --ipodui sim commits it
# in that same boot (commit() borrows a temporary buffer: HAVE_IPOD_UI
# without dircache); the stock sim has no buffer to commit into ("commit
# delayed") and commits at the start of the script's own boot (no prompt
# in the macOS sim), before wait_db returns.
if { [ $needs_library = 1 ] || [ $needs_video = 1 ]; } && [ $fresh_db = 0 ] \
        && [ ! -f "$SIMDISK/.rockbox/database_idx.tcd" ] \
        && [ ! -f "$SIMDISK/.rockbox/database_manifest0.dat" ] \
        && [ ! -f "$SIMDISK/.rockbox/database_manifest1.dat" ] \
        && [ ! -f "$SIMDISK/.rockbox/database_tmp.tcd" ]; then
    printf 'build_db\nwait_db 45000\nart_wait 60000\nquit\n' >"$out/.prime-db.script"
    run_sim "$out/.prime-db.script" "$out/prime-db.log"
    rm -f "$out/.prime-db.script"
    grep "^sim_script" "$out/prime-db.log" | sed 's/^/prime-db: /' >&2 || true
fi

# A "reboot" command splits the script into boots of the sim on the same
# disk (e.g. to check what survives a power cycle); the logs are joined.
if grep -qE '^[[:space:]]*reboot([[:space:]#]|$)' "$script"; then
    rm -f "$out"/.part-*.script
    awk -v dir="$out" 'BEGIN { n = 0; f = dir "/.part-0.script" }
        { print > f }
        /^[[:space:]]*reboot([[:space:]#]|$)/ {
            close(f); n++; f = dir "/.part-" n ".script" }' "$script"
    export SIM_SCRIPT_DIR="$(dirname "$script")"
    : >"$log"
    part_status=0
    n=0
    while [ -f "$out/.part-$n.script" ]; do
        run_sim "$out/.part-$n.script" "$log.part"
        cat "$log.part" >>"$log"
        [ $status -eq 0 ] || part_status=$status
        n=$((n + 1))
    done
    status=$part_status
    rm -f "$out"/.part-*.script "$log.part"
else
    run_sim "$script" "$log"
fi
if [ $drop_db = 1 ]; then
    rm -f "$SIMDISK"/.rockbox/database_*.tcd \
          "$SIMDISK"/.rockbox/database_gen[012]/database* \
          "$SIMDISK"/.rockbox/database_manifest[01].* \
          "$SIMDISK"/.rockbox/database_stats.jnl \
          "$SIMDISK"/.rockbox/database_stats.prev \
          "$SIMDISK"/.rockbox/database_stats.tmp \
          "$SIMDISK"/.rockbox/database_lib.dat \
          "$SIMDISK"/.rockbox/database_changelog.txt* \
          "$SIMDISK"/.rockbox/database_import.pending
fi

if [ $status -eq 137 ]; then
    echo "sim timed out after ${TIMEOUT}s (log: $log)" >&2
fi
grep "^sim_script:" "$log" | grep -v ": shot " >&2 || true

grep "^sim_script: shot " "$log" | sed 's/^sim_script: shot //' >"$out/.shots"
found=0
while IFS= read -r bmp; do
    [ -f "$bmp" ] || continue
    png="${bmp%.bmp}.png"
    sips -s format png "$bmp" --out "$png" >/dev/null 2>&1 && rm -f "$bmp"
    echo "$png"
    found=1
done <"$out/.shots"
rm -f "$out/.shots"

[ $found -eq 1 ] || { echo "no screenshots taken (log: $log)" >&2; exit 1; }
if grep -qE "wait_db (timed out|commit delayed)|update_db_wait timed out|wait_commit timed out|wait_pos timed out|rebuild_db timed out|update_db_wait_change failed|dirty_db failed" "$log"; then
    echo "wait_db/update_db_wait: database not usable (log: $log)" >&2; exit 1
fi
# "# expect-exists: <sim path>": after the run the sim file must exist
# (an empty marker file, for example)
sed -n 's/^#[[:space:]]*expect-exists:[[:space:]]*//p' "$script" |
while IFS= read -r ep; do
    [ -e "$SIMDISK$ep" ] \
        || { echo "expect-exists: $ep does not exist in the sim disk" >&2; exit 1; }
done || exit 1
# "# expect-file: <sim path> <text>": after the run the sim file must exist
# and contain <text> (Phase 8a: /.rockbox/ipodui.log after a flush).
# Several lines allowed. Fails loudly with the path and the text.
# "# expect-file-lacks: <sim path> <text>": the file must exist and must
# NOT contain <text>, in any case (the diagnostics report's privacy check).
# A path with spaces goes in double quotes: "/ProkBox Report.txt" <text>
sed -n 's/^#[[:space:]]*expect-file\(-lacks\)\{0,1\}:[[:space:]]*/\1 /p' "$script" |
while IFS= read -r ef; do
    case "$ef" in
        -lacks\ *) lacks=1; ef=${ef#-lacks } ;;
        *) lacks=0; ef=${ef# } ;;
    esac
    case "$ef" in
        \"*) ef=${ef#\"}; efp=${ef%%\"*}; eft=${ef#*\" } ;;
        *) efp=${ef%% *}; eft=${ef#* } ;;
    esac
    if [ ! -f "$SIMDISK$efp" ]; then
        echo "expect-file: $efp does not exist in the sim disk" >&2; exit 1
    fi
    if [ $lacks -eq 1 ]; then
        if grep -qiF -- "$eft" "$SIMDISK$efp"; then
            echo "expect-file-lacks: $efp holds '$eft'" >&2; exit 1
        fi
    else
        grep -qF -- "$eft" "$SIMDISK$efp" \
            || { echo "expect-file: $efp lacks '$eft'" >&2; exit 1; }
    fi
done || exit 1
if grep -qE "expect_[a-z_]+ failed|wait_(pos|index) failed" "$log"; then
    echo "script expectation failed (log: $log)" >&2; exit 1
fi
[ $status -eq 0 ] || exit $status
