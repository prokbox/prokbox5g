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

"""Check the iPod UI shell's LCD/animation trace in a simulator log.

    scripts/check_trace.py <sim.log> <script> [--rows 9,5,4]
                           [--header-h 20] [--lcd-h 240] [--max-frame-ms 50]
                           [--max-open-ms 300] [--sim-factor F]
                           [--visible-rows 9]
                           [--manifest tests/ui/library_manifest.txt]

List geometry (docs/design/spec/index.md "List geometry", Phase 4b-2): every
list fills the body (lcd_h - header_h = 220 px) with whole rows, row i of
an n-row list at body * i / n (integer division): plain lists n = 9
(24/25 px), two-line songs and episodes 5 (44 px), albums with art 4
(55 px). --rows lists the n of every list style; a "row" below is a
span between two consecutive boundaries of one style, the tallest row
is body / min(n) (55 px).

The shell logs through UPDATE_TRACE (apps/ipodui/ui_main.c), one line each,
"ipodui: " prefixed in sim.log:

    update full <screen>         full-screen push
    update header y=0 h=<h>      header band
    update band y=<y> h=<h>      static selection move (Phase 2)
    update anim:scroll y= h=     one list glide frame's pushed band
    update anim:slide y= h=      one push/pop slide frame's pushed band
    update anim y=<y> h=<h>      untagged animation frame (older builds)
    update marquee y=<y> h=<h>   one marquee step (the selected row band)
    anim start boost=<n>         an animation began; n = boost count after
                                 (the sim logs the shell's own hold, 1/0;
                                 target builds log get_cpu_boost_counter())
    anim frame <ms>              one frame; <ms> = interval since the
                                 previous frame (a monotonic timestamp is
                                 also accepted, see below)
    anim end boost=<n>           an animation ended; n = boost count after

Rules (stdlib only, prints one "ok"/"FAIL" line per rule, exit 1 on any
failure):

  band   every "update band" has h <= 2 * the tallest row (a move
         redraws two rows: 110 px), or is a list shift (a non-animated
         scroll, or the view redrawn at rest): it starts and ends on
         row boundaries of one list style, inside the body
  full   number of "update full" <= 1 + SELECT/MENU key/hold lines in the
         script (boot + one per push/pop; animated pushes need none)
         + one per `unlit exit` line in the log (a dismissed face redraws
         the screen under it once) + `# fulls-extra: N`
  boost  #anim start == #anim end, the open-animation depth never goes
         negative, and the last "anim end" reports boost=0
  frame  (a late run counts as starved only while the whole process, pcpu=,
         was under half busy over the frames; busier means our threads did it)
         mean frame cpu (anim frame <ms> cpu=<us>, when logged) <=
         --max-frame-cpu-us; mean frame interval <= --max-frame-ms (sim
         sanity only), except that a late run whose frames cost under half
         the CPU budget is "starved by the host" and passes with a note, and
         a late run without CPU figures on a host the jitter watcher saw
         stalled (sim_load p95 >= --starved-jitter-ms) fails with the word
         starved in the line (the gate's ledger classes it); the
         first frame of each animation segment (interval 0, or the wait
         since the previous animation) is left out
  anim   every "update anim*" band lies inside the list body:
         y >= header_h and y + h <= lcd_h; anim:scroll and anim:slide
         bands are at most the body (220 px) tall
  marquee  (scripts whose name contains "marquee") at least one
         "update marquee" line, each exactly one row of a list style
         tall, and no
         "anim start" from the last "anim end" before the first marquee
         line through the last one (the marquee never boosts)

If the log has no "anim " lines (a Phase 2 build), the boost/frame/anim
rules are skipped with a note.

Library rules (Phase 4, docs/phase4-brief.md; each is skipped with a note
when the log has none of its lines). The library logs:

    lib open <cat> n=<count> us=<t>   a view opened (<t> = time to open,
                                      microseconds of host time; older
                                      logs: ms=<t>, 10 ms ticks)
    lib fetch n=<rows>                labels fetched for one frame
    lib row <cat> <label>             a row label shown in a <cat> view
                                      (optional; enables the books rule)

  libopen  every "lib open" is estimated to take <= --max-open-ms (300,
         the brief's target) on the device: us x --sim-factor / 1000.
         The factor (SIM_FACTOR below) is how much slower the PP5022 runs
         this code than the simulator host (see the comment there); an
         ms= value (old logs) is compared as is
  libfetch inside an animation (between "anim start" and "anim end"), the
         "lib fetch" rows of one frame (summed between consecutive
         "anim frame" lines) are <= --visible-rows (9) + 2: only the
         visible rows are ever fetched
  A script with a "# trace: lib" line fails when the log has no "lib open"
  or no "lib fetch" line (instead of skipping those two rules).
  books  no "lib row" of a music category (artists, albums, songs, genres,
         composers, compilations, shuffle; a "lib_" prefix and case are
         ignored) has a label listed in --manifest
         (tests/ui/library_manifest.txt: the audiobook and podcast titles,
         authors and genres of the test libraries)

Art rules (Phase 4b, docs/phase4b-brief.md; each is skipped with a note
when the log has none of its lines, so a build from before Agent B's
builder or Agent C's rows land passes silently). None of this code
exists yet, so these are the lines that code will log once it does:

    art get key=<hex> size=<n> res=<r>   a thumbnail read from the
                                          cache for display (one per
                                          artcache_get(); a row=<n> field
                                          is also accepted)
    art build key=<hex> boost=<n>        the builder processed one
                                          album; n = boost count after
                                          (like "anim start/end boost=")
    art io <read|write> key=<hex> playing=<0|1> active=<0|1> idle=<0|1>
                                          a builder disk read/write;
                                          active = storage already
                                          awake; idle = the user idle
                                          (clock face up, backlight off,
                                          or on the charger no key for
                                          2 s: artcache.c user_idle())

  artbuild every "art build" line has boost=0 (the builder never holds
         boost, docs/phase4b-brief.md Revision 1 Decision 2)
  artio  no "art io" line has playing=1 and idle=0 (#58 idle build,
         owner 2026-10-09: while audio plays the builder touches storage
         only while the user is idle; it may wake storage then, so
         active is no longer checked. Replaces Revision 2 D1's fill
         windows, "playing=1 and active=0")
  artglide no "art get" line falls between an "anim start" and its
         "anim end" (thumbnails are fetched only at rest, never
         mid-glide)
  artonce  inside any one rest window (between the end of one glide and
         the start of the next), no key (or row) repeats in "art get"
         lines: at most one fetch per newly visible row
  A script with a "# trace: art" line fails when the log has no "art get"
  line (instead of skipping artglide/artonce): its thumbnails must have
  been read, and only at rest.

Navigation rules (#38, docs/phase-nav-brief.md; each is skipped with a
note when the log has none of its lines, so a log from before agent A's
preview.c lands passes silently). D15's own trace line is already in the
contract (apps/ipodui/preview.h, strip-borrowing comment); the other two
are this file's proposal for preview.c to log, by the same UPDATE_TRACE
mechanism as every other line here (plain logf(), as
apps/ipodui/screen_stack.c's push/pop lines use, is a no-op in this tree
without "#define LOGF_ENABLE" in that file, which nothing in apps/ipodui/
sets -- checked empirically -- so it is not a usable source of trace
data; UPDATE_TRACE is the one mechanism that reaches sim.log):

    panel arrive owner=<name> wait=<ms>   a highlight cross-fade began
                                          (the selection rested); wait =
                                          ms since it last moved or the
                                          screen was pushed/popped onto
    panel drift step=<kind> boost=<0|1> lit=<0|1>
                                          one drift tick (nav_drift_act);
                                          boost must always be 0 (D2:
                                          "unboosted"); a line only ever
                                          appears while lit=1 in a
                                          correct build (drift is idle
                                          while dark), so the rule is
                                          "no line has lit=0", not "no
                                          lines while dark" -- which
                                          lets a regression that still
                                          ticks in the dark be caught
                                          instead of silently passing
    panel strip claim owner=<PANEL|DRIFT> top=<screen name>
                                          preview.h's own wording (Rev 5
                                          R1 / D15); top= already names
                                          the screen on top at claim
                                          time, so a claim while Now
                                          Playing (or any other
                                          full-screen screen) is on top
                                          is just another non-split top

  rest    (Phase 11: the rest delay is 0, panels fade at once) every
          "panel arrive" has wait >= 0, i.e. the line is well-formed; the
          old 0.4 s / 1.1 s minimum is gone. Identity is checked by
          panelsync below
  paneldraw   every "panel draw <name> playing" line (a panel draw while
          audio plays, sim builds) has allocs=0 and opens=0: no
          core_alloc() on the drawing thread and no lib_open() from a
          provider's or leaf's draw (D9, #38 end review)
  driftboost  every "panel drift step" has boost=0
  driftdark   every "panel drift step" has lit=1 (see above)
  stripclaim  every "panel strip claim" line's top is a split screen: a
          name-based deny-list of the screens #38 knows are full-width
          (NONSPLIT_SCREENS) fails the claim; conservative -- it only
          flags a name it can positively confirm is not split, never an
          unrecognized one. A second, positive list (SPLIT_SCREENS) names
          the screens #38 knows CAN be split -- "menu" (split only for a
          ui_menu instance with split=true, e.g. the main menu or the
          Settings tree's own list; a non-split instance such as a plain
          sub-menu never claims the strip at all, so every claim seen is
          for a split one) and "settings-choice" (the Backlight leaf,
          always split) -- so the pass message says how many claims
          landed on a screen it actually recognizes as split, instead of
          merely not-denied (D15)

"update anim:slide" and "update full" lines are also checked for width,
using ui_lcd_push()'s own trace (apps/ipodui/anim.c): full width omits
"x=.. w=.."; anything narrower logs them. The brief's "prefer full-width
pushes" (the LCD's slow path applies at width != 320):

  fullwidth  no "update anim:slide" or "update full" line has "x=..w=.."
          (panel-only, 160 px pushes are fine for "panel arrive"/"panel
          drift" -- only the screen-level slide/full redraw must be wide)
          except x=0 w=160 (SPLIT_LIST_W): a split<->split push/pop only
          slides the list column, the panel retargeting separately by
          its own cross-fade (#38)

D7 transition durations (nav_transition_ticks(), apps/ipodui/nav_core.c):
a script declares what it expects, in the order its push/pop transitions
occur:

    # transitions: <ms> <ms> ...

  transitions  the Nth declared ms matches the Nth observed boosted
          animation's duration (summed "anim frame" ms within one anim
          start/end bracket, plus one frame for the untraced gap between
          the last rendered frame and the silent "anim end" that follows
          it, TRANSITION_CLOSE_MS) within one frame (FRAME_TOLERANCE_MS =
          UI_ANIM_FRAME_TICKS = HZ/30 = 30 ms); skipped with a note
          without the directive, and fails on a count mismatch. Only a
          bracket with its own "update full"/"anim:slide" line counts as
          a transition (RE_SCREEN_TRANSITION) -- a bracket that is only
          ever a panel's own "anim:xfade"/drift pan (a main-menu row's
          preview panel arriving or animating right next to a push/pop,
          #38) is skipped rather than miscounted as one. A push/pop's
          own slide is always the very next SCREEN-LEVEL boosted
          animation after the key press that triggers it, so this only
          needs nothing else screen-level to animate
          between the declared transitions (no concurrent list glide,
          which a dedicated push/pop timing script has no reason to
          have) -- it does not need to see which screen was pushed.
"""
import argparse
import os
import re
import sys

RE_BAND = re.compile(r'update band y=(\d+) h=(\d+)')
RE_ANIM_UPD = re.compile(r'update anim(?::(\w+))? y=(-?\d+) h=(\d+)')
RE_MARQUEE = re.compile(r'update marquee y=(-?\d+) h=(\d+)')
RE_FULL = re.compile(r'update full\b')
RE_START = re.compile(r'\banim start\b.*?boost=(-?\d+)')
RE_FRAME = re.compile(r'\banim frame (\d+)')
RE_FRAME_CPU = re.compile(r'\banim frame \d+ cpu=(\d+)')
RE_FRAME_PCPU = re.compile(r'\banim frame (\d+) cpu=\d+ pcpu=(\d+)')
RE_LOAD = re.compile(r'sim_load: run jitter mean=(\d+) p95=(\d+) p99=(\d+) max=(\d+) n=(\d+)')
RE_END = re.compile(r'\banim end\b.*?boost=(-?\d+)')
RE_PRESS = re.compile(r'^\s*(key|hold)\s+(SELECT|MENU)\b', re.I)
RE_LIBOPEN = re.compile(r'\blib open (\S+) n=(-?\d+) (ms|us)=(-?\d+)')
RE_LIBFETCH = re.compile(r'\blib fetch n=(\d+)')
RE_LIBROW = re.compile(r'\blib row (\S+) (.*?)\s*$')
RE_ARTGET = re.compile(r'\bart get\b.*?(?:row|key)=(-?[0-9a-f]+)')
RE_ARTBUILD = re.compile(r'\bart build\b.*?boost=(-?\d+)')
RE_ARTIO = re.compile(r'\bart io (\S+)\b.*?playing=([01]).*?active=([01])'
                      r'(?:.*?idle=([01]))?')
MUSIC_CATS = {'artists', 'albums', 'songs', 'genres', 'composers',
              'compilations', 'shuffle'}
# #38 (docs/phase-nav-brief.md): navigation trace lines
RE_PANEL_ARRIVE = re.compile(r'\bpanel arrive owner=(\S+) wait=(-?\d+)')
RE_PANEL_DRAW = re.compile(
    r'\bpanel draw (\S+) playing allocs=(\d+) opens=(-?\d+)')
RE_PANEL_DRIFT = re.compile(r'\bpanel drift step=(\S+) boost=([01]) lit=([01])')
RE_PANEL_CLAIM = re.compile(r'\bpanel strip claim owner=(\S+) top=(\S+)')
RE_WIDE = re.compile(r'\bupdate (\S+) x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+)')
RE_TRANSITIONS_DIRECTIVE = re.compile(r'#\s*transitions:\s*(.*)$')
# A screen-level push/pop redraw (ui_lcd_push()'s "full"/"anim:slide"
# kinds; full width omits "x=..w=.." entirely, so this can't reuse
# RE_WIDE, which requires them) -- as opposed to a panel's own
# "anim:xfade" (cross-fade), "drift" (pan) or any other boosted
# animation that can be running around the same time (#38: a main-menu
# row with an animated/arriving preview panel, e.g. Extras, boosts its
# own cross-fade in a bracket right next to the push/pop's).
RE_SCREEN_TRANSITION = re.compile(r'\bupdate (?:full\b|anim:slide\b)')
# NAV_REST_ICON (nav_core.h; 0 since Phase 11): the smaller of the two rest delays, so the
# "rest" rule holds for both icon and cover panels without needing to
# tell them apart from the trace line alone.
NAV_REST_ICON_MS = 0   # Phase 11 (U2a): panels no longer wait before fading
# apps/ipodui/preview.c's hl_start() logs "panel arrive ... wait=" as a
# raw current_tick delta (HZ ticks/s), unlike every other trace line
# here (anim.c's own "anim frame" already converts to ms): preview.c
# asserts NAV_HZ == HZ (nav_core.h), and HZ is 100 on every target
# (firmware/kernel/include/tick.h), so 1 tick = 10 ms uniformly. Convert
# here rather than comparing ticks to an ms threshold (#38: this silently
# turned every "rest" check into a false FAIL, e.g. wait=42 -- 420 ms, an
# easy pass -- read as 42 ms and failed against the 370 ms floor).
NAV_HZ = 100
TICK_MS = 1000 // NAV_HZ
# full-width screen-level kinds the "fullwidth" rule applies to; a
# panel-only or drift push (160 px) is fine narrow, so it's left out.
FULLWIDTH_KINDS = {'full', 'anim:slide'}
# apps/ipodui/theme.c's split_x: the list's own width on a split screen
# (the panel runs x >= this). A split<->split "full"/"anim:slide" only
# needs to redraw the list column -- the panel's own content is still
# whatever it was (its cross-fade, not the slide, retargets it) -- so
# x=0 w=160 is the one narrow case the "fullwidth" rule allows (#38).
SPLIT_LIST_W = 160
# Screens #38 knows are full-width (non-split): conservative, so an
# unrecognized name is never flagged by "stripclaim" (see its docstring).
NONSPLIT_SCREENS = {'browse', 'stub', 'nowplaying', 'playlists',
                    'audiobooks'}
# Screens #38 knows CAN be split, so a claim on one of these is verified
# rather than merely let through by omission from NONSPLIT_SCREENS above:
# "menu" (apps/ipodui/menu.c) is split only when the ui_menu instance
# pushed has split=true (the main menu, the Settings tree's own list --
# both reachable today; a plain sub-menu like Music's with split=false
# never claims the strip at all, by construction, so every "top=menu"
# claim that does appear is for a split instance) and "settings-choice"
# (apps/ipodui/screens/settings.c's choice_screen, the Backlight leaf,
# D8) is always split. Listed apart from NONSPLIT_SCREENS, whose membership
# means the opposite, so neither set is mistaken for the other.
SPLIT_SCREENS = {'menu', 'settings-choice'}
# How many times slower the device (PP5022, ARM7TDMI at 80 MHz boosted,
# 8 KB cache, SDRAM) runs library code than the simulator host (an Apple
# M-class core, -Os). An estimate, not a measurement: ~70 vs ~20,000+
# MIPS for this branchy, memory-bound code, rounded to a conservative
# 400 (the sim's "lib open ... us=" is the thread's host USER time, no
# system-call time: library.c open_us()). Replace it with the ratio of a
# device log's "lib open ... us="
# (USEC_TIMER) to the sim's for the same library once one exists.
SIM_FACTOR = 400
DEFAULT_MANIFEST = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                '..', 'tests', 'ui', 'library_manifest.txt')


def check_library(lines, a, ok, bad, required):
    """libopen / libfetch / books rules (see the module docstring).
    required: the script has "# trace: lib", so a log without "lib open" /
    "lib fetch" lines fails instead of skipping."""
    def missing(what):
        if required:
            bad('no "%s" lines (script has "# trace: lib")' % what)
        else:
            print('note %s check skipped: no "%s" lines' % (what, what))
    # libopen: estimated device time in ms
    def est_ms(unit, t):
        return t * a.sim_factor / 1000.0 if unit == 'us' else float(t)
    opens = [(m.group(1), int(m.group(2)), est_ms(m.group(3),
                                                  int(m.group(4))))
             for l in lines for m in [RE_LIBOPEN.search(l)] if m]
    if not opens:
        missing('lib open')
    else:
        slow = [o for o in opens if o[2] > a.max_open_ms]
        if slow:
            c, n, t = slow[0]
            bad('%d lib opens estimated slower than %d ms on the device '
                '(sim time x %g), first: %s n=%d ~%.0f ms'
                % (len(slow), a.max_open_ms, a.sim_factor, c, n, t))
        else:
            c, n, t = max(opens, key=lambda o: o[2])
            ok('lib open (%d views, slowest %s n=%d ~%.0f ms on the device '
               '<= %d, sim time x %g)' % (len(opens), c, n, t,
                                         a.max_open_ms, a.sim_factor))

    # libfetch: rows per frame while an animation runs
    limit = a.visible_rows + 2
    depth = 0
    frame_rows = None       # rows fetched since the last frame line
    per_frame = []          # (rows, log line number)
    any_fetch = False
    for i, l in enumerate(lines):
        if RE_START.search(l):
            depth += 1
            frame_rows = 0 if depth == 1 else frame_rows
            continue
        if RE_END.search(l):
            depth = max(0, depth - 1)
            if depth == 0 and frame_rows:
                per_frame.append((frame_rows, i + 1))
            if depth == 0:
                frame_rows = None
            continue
        if RE_FRAME.search(l) and depth > 0:
            if frame_rows:
                per_frame.append((frame_rows, i + 1))
            frame_rows = 0
            continue
        m = RE_LIBFETCH.search(l)
        if m:
            any_fetch = True
            if depth > 0 and frame_rows is not None:
                frame_rows += int(m.group(1))
    if not any_fetch:
        missing('lib fetch')
    else:
        over = [(n, ln) for n, ln in per_frame if n > limit]
        if over:
            bad('%d animation frames fetched more than %d labels, first: '
                '%d rows (log line %d)' % (len(over), limit, over[0][0],
                                            over[0][1]))
        else:
            ok('lib fetch during animations (%d frames with fetches, max %d '
               'rows <= %d)' % (len(per_frame),
                                max((n for n, _ in per_frame), default=0),
                                limit))

    # books: no audiobook/podcast in a music list
    rows = [(m.group(1).lower(), m.group(2))
            for l in lines for m in [RE_LIBROW.search(l)] if m]
    if not rows:
        print('note books-in-music check skipped: no "lib row" lines')
        return
    try:
        banned = {}
        for l in open(a.manifest, encoding='utf-8'):
            l = l.rstrip('\n')
            if l and not l.startswith('#') and '\t' in l:
                kind, label = l.split('\t', 1)
                banned[label.strip()] = kind
    except OSError as e:
        bad('books: cannot read manifest %s: %s' % (a.manifest, e))
        return
    music = [(c[4:] if c.startswith('lib_') else c, label)
             for c, label in rows]
    music = [(c, label) for c, label in music if c in MUSIC_CATS]
    hits = [(c, label) for c, label in music if label in banned]
    if hits:
        c, label = hits[0]
        bad('%d audiobook/podcast rows in music lists, first: %s "%s" (%s)'
            % (len(hits), c, label, banned[label]))
    else:
        ok('no audiobooks/podcasts in music lists (%d music rows checked '
           'against %d manifest labels)' % (len(music), len(banned)))


def check_art(lines, ok, bad, required=False):
    """artbuild / artio / artglide / artonce rules (see the module
    docstring). Each is skipped with a note when its lines are absent, so
    a log from before Agent B's builder or Agent C's rows land passes
    silently; nothing emits "art *" lines yet."""
    builds = [(i, int(m.group(1))) for i, l in enumerate(lines)
              for m in [RE_ARTBUILD.search(l)] if m]
    ios = [(i, m.group(1), m.group(2) == '1', m.group(4) == '1')
           for i, l in enumerate(lines) for m in [RE_ARTIO.search(l)] if m]
    gets = [(i, m.group(1)) for i, l in enumerate(lines)
            for m in [RE_ARTGET.search(l)] if m]

    if not builds:
        print('note artbuild check skipped: no "art build" lines')
    else:
        held = [(i, n) for i, n in builds if n != 0]
        if held:
            bad('art build with boost held, first: line %d boost=%d'
                % (held[0][0] + 1, held[0][1]))
        else:
            ok('artbuild never holds boost (%d builds)' % len(builds))

    if not ios:
        print('note artio check skipped: no "art io" lines')
    else:
        lit = [(i, op) for i, op, playing, idle in ios
               if playing and not idle]
        if lit:
            bad('art io %s while playing with the user on (idle=0), '
                'first: line %d' % (lit[0][1], lit[0][0] + 1))
        else:
            ok('artio: storage only with the user idle while playing (%d '
               'io lines, %d while playing)'
               % (len(ios), sum(1 for _, _, p, _ in ios if p)))

    if not gets:
        if required:
            bad('no "art get" line (the script says "# trace: art")')
        else:
            print('note artglide/artonce checks skipped: no "art get" '
                  'lines')
        return

    # artglide: no "art get" between an "anim start" and its "anim end"
    depth = 0
    midglide = None
    for i, l in enumerate(lines):
        if RE_START.search(l):
            depth += 1
        elif RE_END.search(l):
            depth = max(0, depth - 1)
        elif (RE_ARTGET.search(l) and ' size=2 ' not in l + ' '
              and depth > 0 and midglide is None):
            # size=2 is ARTCACHE_NOWPLAYING: Now Playing reads its one
            # bundle at enter, during the push slide (brief §6, Art)
            midglide = i
    if midglide is not None:
        bad('art get mid-glide (between anim start/end), line %d'
            % (midglide + 1))
    else:
        ok('artglide: no art get mid-glide (%d art get lines)' % len(gets))

    # artonce: no row repeats within one rest window (a glide clears it,
    # since a row becomes "newly visible" again only after one)
    depth = 0
    seen = set()
    dup = None
    for l in lines:
        if RE_START.search(l):
            depth += 1
            seen.clear()
        elif RE_END.search(l):
            depth = max(0, depth - 1)
        else:
            m = None if " size=2 " in l + " " else RE_ARTGET.search(l)
            if m and depth == 0:
                row = m.group(1)
                if dup is None and row in seen:
                    dup = row
                seen.add(row)
    if dup is not None:
        bad('%s fetched more than once at rest (not just for newly '
            'visible rows)' % dup)
    else:
        ok('artonce: at most one art get per newly visible row (%d gets, '
           '%d distinct rows)' % (len(gets), len(set(r for _, r in gets))))


FRAME_TOLERANCE_MS = 30   # 1 frame: UI_ANIM_FRAME_TICKS = HZ/30 = 3 ticks
# A slide's last "anim frame" line marks its last RENDERED frame, not
# the moment ui_main.c's slide code notices it has reached
# nav_transition_ticks()'s target and calls ui_anim_idle() -- apps/ipodui/
# anim.c's "anim end" has no frame (and so no ms) of its own, one more
# UI_ANIM_FRAME_TICKS (one "frame" rule tick) after the last one counted.
# Measured empirically (#38, three runs of ipodui-nav-transitions.script):
# every bracket's summed "anim frame" ms is short of its declared D7
# duration by consistently about one frame; adding it back here, rather
# than widening FRAME_TOLERANCE_MS, keeps that tolerance meaning real
# jitter instead of absorbing this structural gap too.
TRANSITION_CLOSE_MS = FRAME_TOLERANCE_MS


def check_nav(lines, script_lines, ok, bad):
    """rest / driftboost / driftdark / stripclaim / fullwidth / transitions
    rules (see the module docstring). Each of the first five is skipped
    with a note when its lines are absent; "transitions" is skipped with
    a note when the script has no "# transitions:" directive."""
    arrivals = [(i, m.group(1), int(m.group(2)) * TICK_MS)
                for i, l in enumerate(lines)
                for m in [RE_PANEL_ARRIVE.search(l)] if m]
    if not arrivals:
        print('note rest check skipped: no "panel arrive" lines')
    else:
        late = [(i, o, w) for i, o, w in arrivals
                if w < NAV_REST_ICON_MS - FRAME_TOLERANCE_MS]
        if late:
            i, o, w = late[0]
            bad('panel arrive owner=%s wait=%d ms < %d (line %d): arrived '
                'before the rest delay' % (o, w, NAV_REST_ICON_MS
                                            - FRAME_TOLERANCE_MS, i + 1))
        else:
            ok('panel arrives only after rest (%d arrivals, min wait %d ms '
               '>= %d)' % (len(arrivals), min(w for _, _, w in arrivals),
                           NAV_REST_ICON_MS - FRAME_TOLERANCE_MS))

    draws = [(i, m.group(1), int(m.group(2)), int(m.group(3)))
             for i, l in enumerate(lines)
             for m in [RE_PANEL_DRAW.search(l)] if m]
    if not draws:
        print('note paneldraw check skipped: no "panel draw ... playing" '
              'lines')
    else:
        heavy = [d for d in draws if d[2] or d[3]]
        if heavy:
            i, o, a, n = heavy[0]
            bad('panel draw %s while playing: allocs=%d opens=%d (line %d): '
                'a draw must not allocate or open a library view'
                % (o, a, n, i + 1))
        else:
            ok('panel draws while playing allocate nothing, open nothing '
               '(%d draws: %s)' % (len(draws),
                                   ', '.join(sorted({d[1] for d in draws}))))

    drifts = [(i, m.group(1), m.group(2), m.group(3))
              for i, l in enumerate(lines)
              for m in [RE_PANEL_DRIFT.search(l)] if m]
    if not drifts:
        print('note driftboost/driftdark checks skipped: no "panel drift" '
              'lines')
    else:
        boosted = [(i, k) for i, k, b, lit in drifts if b != '0']
        if boosted:
            bad('panel drift step=%s boosted (line %d): drift must never '
                'boost (D2)' % (boosted[0][1], boosted[0][0] + 1))
        else:
            ok('drift never boosts (%d steps)' % len(drifts))

        dark = [(i, k) for i, k, b, lit in drifts if lit != '1']
        if dark:
            bad('panel drift step=%s lit=0 (line %d): drift ticked while '
                'the backlight was off' % (dark[0][1], dark[0][0] + 1))
        else:
            ok('drift idle while the backlight is off (%d steps, all lit)'
               % len(drifts))

    # stripclaim: "top=" already names the screen on top at claim time
    # (preview.h's own wording), so a claim while Now Playing (or any
    # other full-screen screen) is on top is caught the same way as any
    # other non-split top -- no separate push/pop tracking needed. (Plain
    # logf(), as apps/ipodui/screen_stack.c's push/pop lines use, is a
    # no-op without "#define LOGF_ENABLE" in that file, apps/ipodui/*
    # doesn't set it, and UPDATE_TRACE is reserved for the lines this
    # file checks -- so those two lines never reach sim.log; checked
    # empirically against this script's own log.)
    claims = [(i, m.group(1), m.group(2)) for i, l in enumerate(lines)
              for m in [RE_PANEL_CLAIM.search(l)] if m]
    if not claims:
        print('note stripclaim check skipped: no "panel strip claim" lines')
    else:
        bad_top = [(i, o, t) for i, o, t in claims if t in NONSPLIT_SCREENS]
        if bad_top:
            i, o, t = bad_top[0]
            bad('panel strip claim owner=%s top=%s (line %d): not a split '
                'screen (D15)' % (o, t, i + 1))
        else:
            known = sum(1 for _, _, t in claims if t in SPLIT_SCREENS)
            unrecognized = sorted({t for _, _, t in claims
                                    if t not in SPLIT_SCREENS})
            msg = ('panel strip claims only on a split top (%d claims, '
                   '%d on a known-split top%s)'
                   % (len(claims), known,
                      ': ' + ', '.join(unrecognized) + ' unverified'
                      if unrecognized else ''))
            ok(msg)

    wide = [(i, m.group(1), m.group(2), m.group(4))
            for i, l in enumerate(lines)
            for m in [RE_WIDE.search(l)]
            if m and m.group(1) in FULLWIDTH_KINDS
            and not (m.group(2) == '0' and int(m.group(4)) == SPLIT_LIST_W)]
    if wide:
        i, k, x, w = wide[0]
        bad('update %s x=%s w=%s (line %d): not full-width' % (k, x, w,
                                                                i + 1))
    else:
        ok('full-width pushes (no narrow "update full"/"anim:slide")')

    # transitions: each declared ms is matched, in order, against one
    # observed boosted anim segment's total duration (summed "anim
    # frame" ms within one start/end bracket). A push/pop's own slide is
    # always the very next boosted animation after the key press that
    # triggers it (ui_take_transition() runs before anything else can
    # animate), so with no other animation competing -- no concurrent
    # list glide, which a plain push/pop timing script has no reason to
    # have -- the Nth segment in the log is the Nth transition in the
    # script. (ipodui: push/pop lines would make this independent of
    # that assumption, but see the stripclaim comment above: they don't
    # reach sim.log.)
    want = None
    for l in script_lines:
        m = RE_TRANSITIONS_DIRECTIVE.match(l.strip())
        if m:
            want = [int(t) for t in m.group(1).split()]
            break
    if want is None:
        print('note transitions check skipped: no "# transitions:" line')
        return

    segs = []       # duration_ms of each boosted anim start/end bracket
                     # that actually contains a screen-level transition
    depth = 0
    cur = []
    is_screen = False
    for l in lines:
        if RE_START.search(l):
            if depth == 0:
                cur = []
                is_screen = False
            depth += 1
            continue
        if RE_END.search(l):
            depth = max(0, depth - 1)
            if depth == 0 and is_screen:
                segs.append(sum(cur) + TRANSITION_CLOSE_MS)
            continue
        if RE_SCREEN_TRANSITION.search(l):
            is_screen = True
        m = RE_FRAME.search(l)
        if m and depth > 0:
            cur.append(int(m.group(1)))

    if len(segs) != len(want):
        bad('transitions: %d observed boosted animation(s), %d declared '
            'in "# transitions:"' % (len(segs), len(want)))
        return
    off = [(i, g, w) for i, (g, w) in enumerate(zip(segs, want))
           if abs(g - w) > FRAME_TOLERANCE_MS]
    busy = frame_busy(lines)
    if off and all(g > w for _i, g, w in off) and busy is not None \
            and busy < STARVED_BUSY:
        # too long, and the process was mostly idle: the host held it up
        i, g, w = off[0]
        ok('transition #%d: %d ms, want %d +/- %d ms (D7) but the process was '
           'only %d%% busy: starved by the host'
           % (i + 1, g, w, FRAME_TOLERANCE_MS, round(100 * busy)))
    elif off:
        i, g, w = off[0]
        bad('transition #%d: %d ms, want %d +/- %d ms (D7)'
            % (i + 1, g, w, FRAME_TOLERANCE_MS))
    else:
        ok('transition durations within %d ms of D7 (%d transitions)'
           % (FRAME_TOLERANCE_MS, len(segs)))


RE_NPUPD = re.compile(r'update np:(\w+) y=(\d+) h=(\d+)')
RE_STRIPCLAIM = re.compile(r'panel strip claim owner=(\w+) top=(.*)')


NP_BANDS = {  # band -> max rows (the screen's own bands, nowplaying.c)
    'body': 220, 'info': 178, 'title': 34, 'row': 34, 'rowfull': 34,
    'stars': 34, 'glyphs': 34, 'chapter': 34, 'counter': 34,
    'viz': 240,  # the visualizer's CPU path (viz.h: the full panel; its
                 # bands start at y 0, so a script says '# np: viz-full')
}
RE_NPSEEK = re.compile(r'\bnp seek ms=(\d+) id=(\d+)')
RE_NPSNAP = re.compile(r'\bnp snap id=(\d+) seq=(\d+)')
RE_NPDRAW = re.compile(r'\bnp draw id=(\d+) playing=(\d+)')
RE_NPSEEK_DROP = re.compile(r'\bnp seek ms=\d+ dropped \((\w+)\)')
RE_SHUF_REFILL = re.compile(r'shuf: refill \+(\d+) -(\d+) skipped=(\d+) '
                            r'src=(\w+) queued=(\d+)')
SHUF_WINDOW = 50        # lib_shuffle.h LIB_SHUF_WINDOW
SHUF_QUEUE_MAX = 400    # a window of a few hundred entries (shuffle.md)


# ---------------------------------------------------------------------------
# Phase 8a (docs/phase8a-brief.md section 4.8 and Revision 1): the native
# system screens. Every rule FAILS when its anchor lines are absent: a p8a
# script never skips a rule. A rule is armed by the script's name
# (ipodui-p8a-boot*, -usb*, -unlit*, -devmode*, -splash*, -log*) or by a
# `# trace: boot|usb|unlit|devmode|splash|log` line. The negative rules
# (splash-pass, malformed values, stray usb:/unlit: updates) run on every
# script.
RE_P8A_BOOT = re.compile(r'\bboot screen gen=([0-9a-fA-F]+)')
RE_P8A_USB_ENTER = re.compile(r'\busb enter glyph=(\w+) shell=([01])')
RE_P8A_USB_IO = re.compile(r'\busb io=([01])')
RE_P8A_USB_EXIT = re.compile(r'\busb exit frames=(\d+) boost=(-?\d+)')
RE_P8A_USB_UPD = re.compile(r'\bupdate usb:(\w+)(?: y=(\d+) h=(\d+))?')
RE_P8A_UNLIT_UPD = re.compile(r'\bupdate unlit:(\w+)(?: y=(\d+) h=(\d+))?')
RE_P8A_ANYUPD = re.compile(r'\bupdate (\S+)')
RE_P8A_UNLIT_EXIT = re.compile(r'\bunlit exit (\w+)')
# `unlit skip <why>` or `unlit skip-expired` (unlit.c traces both)
RE_P8A_UNLIT_SKIP = re.compile(r'\bunlit skip(?: (\S+)|(-expired)\b)')
RE_P8A_SWALLOW_END = re.compile(r'\bunlit swallow-end ([\w-]+)')
RE_P8A_LOGFLUSH = re.compile(r'\blog flush lines=(\d+) seq=(\d+)-(\d+)')
RE_P8A_CHORD = re.compile(r'^\s*chord\s+\S+\s+(\d+)')
# allowed (y, h) per usb:<kind>; None = full screen (no y/h). The disc
# band is UI_USB_BAND_Y/H (usbscreen.h: disc 92 px at (160, 99) -> y 53,
# 92 rows); the header is USB_HDR_H (usbscreen_data.h). usb:static is the
# whole screen at entry (y=0 h=240, or no y/h), then the disc band when the
# arrows stop.
P8A_USB_BANDS = {'static': (None, (0, 240), (53, 92)), 'frame': ((53, 92),),
                 'header': ((0, 22),)}
P8A_UNLIT_BANDS = {'full': None, 'digits': (62, 68), 'status': (161, 37)}
P8A_DEVMODE_HOLD_MS = 3000          # DEVMODE_HOLD_TICKS (devmode.h)
P8A_BOOTSCREEN_DATA = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), '..', 'firmware', 'export',
    'bootscreen_data.h')


def check_p8a(lines, script, script_lines, ok, bad):
    """Phase 8a rules (see the section comment above). Rules:

      p8a-boot     `boot screen gen=<hex>`: exactly one per boot (1 + the
                   script's `reboot` lines) and equal to BOOTSCREEN_GEN in
                   firmware/export/bootscreen_data.h (a missing header or
                   define FAILS: the hand-off identity cannot be checked)
      p8a-usb      `usb enter glyph=<usbc|dock30> shell=<0|1>` and
                   `usb exit frames=<n> boost=<n>` paired in order, every
                   exit boost=0, one or more of each; between them the only
                   `update` lines are usb:static (full y=0 h=240, or y=53 h=92),
                   usb:frame y=53 h=92 and usb:header y=0 h=22; a usb:frame needs the latest
                   `usb io=` since the enter to be 1; no `update usb:`
                   outside a pair
      p8a-unlit    `unlit enter` / `unlit exit <key|holdoff|usb|alarm>` paired
                   (alarm: a ring leaves the face, ui_unlit_dismiss(), P12),
                   at least one pair; between them only `update unlit:*`
                   (full once at most per pair, digits y=62 h=68, status
                   y=161 h=37); none outside a pair; after an exit key or
                   holdoff the first `update` is `update full`;
                   `# unlit-digits: N [M]` = exactly N (or N..M) digits pushes
                   (one per minute step); skip (`unlit skip <sleep|usb|
                   devmode>` or `unlit skip-expired`) and swallow-end
                   values are checked
      p8a-devmode  `devmode arm` / `enter` / `exit` in order (an enter
                   needs a preceding arm, unless the script has a
                   `# boot_keys:` header: the boot escape), at least one
                   enter, every enter exited; the script must hold a
                   `chord` of >= 3000 ms per arm-enter (the 3 s rule: the
                   log carries no timestamps, so the chord duration is what
                   is checked); `# devmode: none` instead requires zero
                   enter lines (wrong screen, short chord)
      p8a-splash   at least one `splash-suppressed` / `yesno-suppressed`
                   line (`# trace: splash`); on EVERY script, no
                   `splash-pass` unless the script is a devmode script
      p8a-log      `log flush lines=<n> seq=<a>-<b>`: n >= 1, a <= b (a
                   16-bit wrap is allowed); `# trace: log` requires one
    """
    base = os.path.basename(script)
    declared = set()
    for l in script_lines:
        m = re.match(r'#\s*trace:\s*(.*)$', l)
        if m:
            declared.update(m.group(1).split())
    for kind in ('boot', 'usb', 'unlit', 'devmode', 'splash', 'log'):
        if re.search(r'ipodui-p8a-%s(?![a-z])' % kind, base):
            declared.add(kind)
    devmode_script = ('devmode' in declared
                      or any(re.match(r'#\s*devmode:', l)
                             for l in script_lines))

    # ---- p8a-boot
    if 'boot' in declared:
        gens = [m.group(1).lower() for l in lines
                for m in [RE_P8A_BOOT.search(l)] if m]
        boots = 1 + sum(1 for l in script_lines
                        if re.match(r'\s*reboot\b', l))
        want = None
        try:
            for l in open(P8A_BOOTSCREEN_DATA):
                m = re.match(r'#define\s+BOOTSCREEN_GEN\s+\(?0[xX]([0-9a-fA-F]+)',
                             l)
                if m:
                    want = m.group(1).lower().lstrip('0') or '0'
        except OSError:
            pass
        if not gens:
            bad('p8a-boot: no "boot screen gen=" line in the log (script '
                '%s)' % base)
        elif len(gens) != boots:
            bad('p8a-boot: %d "boot screen gen=" lines, want exactly %d '
                '(one per boot)' % (len(gens), boots))
        elif want is None:
            bad('p8a-boot: no BOOTSCREEN_GEN in %s: cannot compare '
                'gen=%s' % (P8A_BOOTSCREEN_DATA, gens[0]))
        else:
            wrong = [g for g in gens if (g.lstrip('0') or '0') != want]
            if wrong:
                bad('p8a-boot: gen=%s != BOOTSCREEN_GEN 0x%s' % (wrong[0],
                                                                 want))
            else:
                ok('p8a-boot (%d boot line(s), gen=%s = BOOTSCREEN_GEN)'
                   % (len(gens), want))

    # ---- p8a-usb
    inside, lit_io, enters, exits, frames = False, 0, 0, 0, 0
    problems = []
    for l in lines:
        if inside and 'sim_script: power_cut' in l:
            # a crash with USB still in (npfix H1, ipodui-boot-settle-usb):
            # no exit path runs; the next part is a new boot
            inside = False
            exits += 1
            continue
        m = RE_P8A_USB_ENTER.search(l)
        if m:
            if inside:
                problems.append('usb enter inside an open usb enter')
            if m.group(1) not in ('usbc', 'dock30'):
                problems.append('usb enter glyph=%s (want usbc|dock30)'
                                % m.group(1))
            inside, lit_io = True, 0
            enters += 1
            continue
        m = RE_P8A_USB_EXIT.search(l)
        if m:
            if not inside:
                problems.append('usb exit without a usb enter')
            if int(m.group(2)) != 0:
                problems.append('usb exit boost=%s, want 0' % m.group(2))
            inside = False
            exits += 1
            continue
        m = RE_P8A_USB_IO.search(l)
        if m and inside:
            lit_io = int(m.group(1))
            continue
        if RE_P8A_ANYUPD.search(l):
            um = RE_P8A_USB_UPD.search(l)
            if inside:
                if not um:
                    problems.append('inside usb: non-usb update "%s"'
                                    % l.strip()[-60:])
                    continue
                kind = um.group(1)
                if kind not in P8A_USB_BANDS:
                    problems.append('unknown update usb:%s' % kind)
                elif (None if um.group(2) is None
                      else (int(um.group(2)), int(um.group(3)))) \
                        not in P8A_USB_BANDS[kind]:
                    problems.append('update usb:%s y=%s h=%s, want %s'
                                    % (kind, um.group(2), um.group(3),
                                       ' or '.join(
                                           'full' if b is None else str(b)
                                           for b in P8A_USB_BANDS[kind])))
                if kind == 'frame':
                    frames += 1
                    if not lit_io:
                        problems.append('usb:frame while io=0')
            elif um:
                problems.append('update usb:%s outside enter/exit'
                                % um.group(1))
    if inside:
        problems.append('usb enter without a usb exit')
    if 'usb' in declared and not enters:
        problems.append('no "usb enter glyph=" line in the log')
    if enters != exits:
        problems.append('%d usb enter vs %d usb exit' % (enters, exits))
    if 'usb' in declared or enters or exits:
        if problems:
            bad('p8a-usb: %s (+%d more)' % (problems[0], len(problems) - 1))
        else:
            ok('p8a-usb (%d enter/exit pair(s), %d usb:frame, boost=0 at '
               'every exit)' % (enters, frames))

    # ---- p8a-unlit
    inside, enters, exits, fulls, digits, skips = False, 0, 0, 0, 0, 0
    problems = []
    after_exit = False
    for l in lines:
        if inside and re.search(r'\bsys lowbatt\b', l):
            # P11: the battery monitor's shutdown while the unlit face is
            # up: "Connect to Power" replaces it and the power goes off,
            # so the face ends here with no key/holdoff/usb exit
            inside = False
            exits += 1
            continue
        if re.search(r'\bunlit enter\b', l):
            if inside:
                problems.append('unlit enter inside an open unlit enter')
            inside, fulls = True, 0
            enters += 1
            after_exit = False
            continue
        m = RE_P8A_UNLIT_EXIT.search(l)
        if m:
            if not inside:
                problems.append('unlit exit without an unlit enter')
            if m.group(1) not in ('key', 'holdoff', 'usb', 'alarm'):
                problems.append('unlit exit %s (want key|holdoff|usb|alarm)'
                                % m.group(1))
            inside = False
            exits += 1
            after_exit = m.group(1) in ('key', 'holdoff')
            continue
        m = RE_P8A_UNLIT_SKIP.search(l)
        if m and not m.group(2) and m.group(1) not in ('sleep', 'usb',
                                                       'devmode'):
            problems.append('unlit skip %s (want sleep|usb|devmode, or '
                            'unlit skip-expired)' % m.group(1))
        if m:
            skips += 1
        m = RE_P8A_SWALLOW_END.search(l)
        if m and m.group(1) not in ('release', 'buttons-up', 'timeout'):
            problems.append('unlit swallow-end %s' % m.group(1))
        if RE_P8A_ANYUPD.search(l):
            um = RE_P8A_UNLIT_UPD.search(l)
            if inside:
                if not um:
                    problems.append('inside unlit: non-unlit update "%s"'
                                    % l.strip()[-60:])
                    continue
                kind = um.group(1)
                if kind not in P8A_UNLIT_BANDS:
                    problems.append('unknown update unlit:%s' % kind)
                    continue
                if kind == 'full':
                    fulls += 1
                    if fulls > 1:
                        problems.append('second unlit:full in one face')
                elif (um.group(2) is None
                      or (int(um.group(2)), int(um.group(3)))
                      != P8A_UNLIT_BANDS[kind]):
                    problems.append('update unlit:%s y=%s h=%s, want %s'
                                    % (kind, um.group(2), um.group(3),
                                       P8A_UNLIT_BANDS[kind]))
                if kind == 'digits':
                    digits += 1
            else:
                if um:
                    problems.append('update unlit:%s outside enter/exit'
                                    % um.group(1))
                elif after_exit and not re.search(r'\bupdate full\b', l):
                    problems.append('first update after unlit exit is not '
                                    '"update full": "%s"' % l.strip()[-60:])
                after_exit = False
    if inside:
        problems.append('unlit enter without an unlit exit')
    if 'unlit' in declared and not enters:
        problems.append('no "unlit enter" line in the log')
    if enters != exits:
        problems.append('%d unlit enter vs %d unlit exit' % (enters, exits))
    for l in script_lines:
        m = re.match(r'#\s*unlit-digits:\s*(\d+)(?:\s+(\d+))?', l)
        if m:
            lo = int(m.group(1))
            hi = int(m.group(2)) if m.group(2) else lo
            if not lo <= digits <= hi:
                problems.append('# unlit-digits: %d..%d but %d update '
                                'unlit:digits lines' % (lo, hi, digits))
    if 'unlit' in declared or enters or exits:
        if problems:
            bad('p8a-unlit: %s (+%d more)' % (problems[0], len(problems) - 1))
        else:
            ok('p8a-unlit (%d face(s), %d digits push(es), %d skip line(s))'
               % (enters, digits, skips))

    # ---- p8a-devmode
    # a script's "reboot" starts a new boot: developer mode is off again
    # (the bench session's scripts restart inside it)
    seq = ['boot' if 'sim_script: reboot' in l else m.group(1)
           for l in lines
           for m in [re.search(r'\bdevmode (arm|enter|exit)\b|'
                               r'sim_script: reboot', l)] if m]
    boot_escape = any(re.match(r'#\s*boot_keys:', l) for l in script_lines)
    none_ok = any(re.match(r'#\s*devmode:\s*none', l) for l in script_lines)
    long_chords = sum(1 for l in script_lines
                      for m in [RE_P8A_CHORD.match(l)]
                      if m and int(m.group(1)) >= P8A_DEVMODE_HOLD_MS)
    problems = []
    armed = entered = False
    n_enter = n_armed_enter = 0
    for t in seq:
        if t == 'boot':
            armed = entered = False
        elif t == 'arm':
            armed = True
        elif t == 'enter':
            n_enter += 1
            if entered:
                problems.append('devmode enter inside devmode')
            if armed:
                n_armed_enter += 1
            elif not boot_escape:
                problems.append('devmode enter with no devmode arm before '
                                'it (needs >= 3 s of chord after arm)')
            entered, armed = True, False
        elif t == 'exit':
            if not entered:
                problems.append('devmode exit without enter')
            entered = False
    if entered:
        problems.append('devmode enter without exit')
    if none_ok:
        if n_enter:
            problems.append('# devmode: none but %d devmode enter line(s)'
                            % n_enter)
    else:
        if 'devmode' in declared and not n_enter:
            problems.append('no "devmode enter" line in the log')
        if n_armed_enter > long_chords:
            problems.append('%d armed devmode enter(s) but only %d script '
                            'chord(s) of >= %d ms' % (n_armed_enter,
                                                      long_chords,
                                                      P8A_DEVMODE_HOLD_MS))
    if 'devmode' in declared or [t for t in seq if t != 'boot'] or none_ok:
        if problems:
            bad('p8a-devmode: %s (+%d more)' % (problems[0],
                                                len(problems) - 1))
        else:
            ok('p8a-devmode (%d enter(s), %d arm-enter(s) <= %d chord(s) '
               '>= 3 s%s)' % (n_enter, n_armed_enter, long_chords,
                              ', boot escape' if boot_escape else ''))

    # ---- p8a-splash (and the global 0 splash-pass)
    supp = sum(1 for l in lines if re.search(
        r'\b(?:splash|yesno)-suppressed\b', l))
    passes = sum(1 for l in lines if re.search(r'\bsplash-pass\b', l))
    if 'splash' in declared and not supp:
        bad('p8a-splash: no "splash-suppressed" / "yesno-suppressed" line '
            'in the log')
    elif 'splash' in declared:
        ok('p8a-splash (%d suppressed line(s))' % supp)
    if passes and not devmode_script:
        bad('p8a-splash: %d "splash-pass" line(s) in a script that never '
            'enters developer mode (a stock splash reached the screen)'
            % passes)
    elif passes:
        ok('p8a-splash: %d splash-pass line(s), devmode script' % passes)

    # ---- p8a-log
    flushes = [(int(m.group(1)), int(m.group(2)), int(m.group(3)))
               for l in lines for m in [RE_P8A_LOGFLUSH.search(l)] if m]
    badf = [f for f in flushes
            if f[0] < 1 or (f[1] > f[2] and not f[1] > 60000)]
    if 'log' in declared and not flushes:
        bad('p8a-log: no "log flush lines= seq=" line in the log')
    elif badf:
        bad('p8a-log: bad flush line lines=%d seq=%d-%d' % badf[0])
    elif flushes:
        ok('p8a-log (%d flush(es), %d line(s))'
           % (len(flushes), sum(f[0] for f in flushes)))


RE_AB_STORE = re.compile(r'\bab store w=(\w+) book=([0-9a-f]+) ms=(\d+)')
RE_AB_SLOTW = re.compile(r'\bab slot w=(\d+) n=(\d+) cut=(\d+)')
RE_AB_SLOTR = re.compile(r'\bab slot read slot=(\d+) ok=(\d)')
RE_AB_OPEN = re.compile(r'\bab open book=([0-9a-f]+) reads=(\d+)')
RE_AB_PAK = re.compile(r'\bab pak (hit|miss) book=')
RE_AB_SPEED = re.compile(r'\bab speed (\d+)')
RE_AB_ANY = re.compile(r'\bab (slot|pak|open|map|view|store|seed|speed|resume)\b')
RE_AB_MARK = re.compile(r'sim_script: ab_mark (\S+)')
AB_OPEN_READS_MAX = 8       # REAL read() calls of the UI thread (fix round
                            # L4; the estimate said 3): the store sector,
                            # books.idx header + binary search, the blob;
                            # measured 4-7 (7 with a 2,048-record store)
AB_OPEN_MISS_EXTRA = 256    # single-file pak miss: + the path and the
                            # file's own metadata walk (get_metadata +
                            # chapters: 224 reads for 1,100 chapters)
RE_NP_SNAP = re.compile(r'\bnp snap id=(\d+) seq=\d+ len=\d+ ch=(\d+) book=(\d)')
AB_IDLE_MAX = 4             # "ab store w=idle" per script (no wall clock)


# ---- Phase 11 (docs/phase11-brief.md section 4.10, Rev 1) --------------
RE_P11_ARENA_PUSH = re.compile(r'\barena push s=(\S+) n=(\d+) free=(\d+)')
RE_P11_ARENA_FULL = re.compile(r'\barena full need=(\d+)')
RE_P11_MEM_ALLOC = re.compile(r'\bmem core_alloc s=(\S+)')
RE_P11_SEARCH = re.compile(
    r'\bsearch q=(\d+) kind_n=([\d,]+) n=(\d+) us=(\d+) yields=(\d+) '
    r'refine=([01]) cap=(\d+)')
RE_P11_OTG_ADD = re.compile(r'\botg add n=(\d+) reads=(\d+) writes=(\d+)')
RE_P11_OTG_RES = re.compile(
    r'\botg resolve n=(\d+) unresolved=(\d+) bytes=(\d+) reads=(\d+)')
RE_P11_OTG_PLAY = re.compile(r'\botg play n=(\d+) pos=(-?\d+) reads=(\d+)')
RE_P11_OTG_WRITE = re.compile(r'\botg (add|save|clear)\b')
RE_P11_HOLD = re.compile(r'\bhold open ctx=(\S+) items=(0[xX][0-9a-fA-F]+|\d+)')
RE_P11_VL_SET = re.compile(r'\bvlimit set=(-?\d+) code=([01]) applied=(-?\d+)')
RE_P11_VL_UNLOCK = re.compile(r'\bvlimit unlock ok=([01])')
RE_P11_ABOUT = re.compile(r'\babout page=(\d+)')
RE_P11_SYS_CH = re.compile(r'\bsys charging state=(\w+)')
RE_P11_SYS_POP = re.compile(r'\bsys charging pop=(\w+)')
RE_P11_PANEL_WANT = re.compile(r'\bpanel want key=(\S+) arg=(\S+)')
RE_P11_PANEL_SHOW = re.compile(r'\bpanel show key=(\S+) arg=(\S+) how=')
RE_P11_NPART = re.compile(r'\bnp art src=(\S+) reads=(\d+) us=(\d+)')
P11_SRC = ('bundle', 'legacy', 'ph-note', 'ph-book', 'ph-render')
P11_OTG_RESOLVE_BLOCK = 32 * 1024       # otg.h: one 32 KB read per block
# ui_hold_items() (holdmenu.h), bit = 1 << enum ui_hold_item (ADD 1, REMOVE
# 2, BROWSE_ALBUM 4, BROWSE_ARTIST 8, REBUILD 16, BUILD_ART 32, CANCEL 64:
# the Music maintenance items, #85, sit before Cancel so it stays last)
P11_HOLD_CTX = {   # ctx (number or name) -> (full mask, mask without Browse)
    '0': (0x4d, 0x41), 'np': (0x4d, 0x41),
    '1': (0x4d, 0x41), 'song': (0x4d, 0x41),
    '2': (0x41, 0x41), 'group': (0x41, 0x41),
    '3': (0x4e, 0x42), 'in_otg': (0x4e, 0x42), 'otg': (0x4e, 0x42),
    '4': (0x70, 0x70), 'music': (0x70, 0x70),
}


def check_p11(lines, script, script_lines, ok, bad):
    """Phase 11 rules (docs/phase11-brief.md section 4.10). Every script:

      p11-arena    no `arena full` line (the brief: none while audio plays
                   or is paused; a script that exercises the stopped-only
                   core_alloc fallback or a pinned-small arena declares
                   `# arena: full-ok <why>`); no `mem core_alloc s=<screen>`
                   line (a screen-state core_alloc after shell start)

    Selected by `# trace: <kind>...` or the script name
    (ipodui-p11-<kind>*, ipodui-qw2-*); a selected kind that finds none of
    its lines FAILS (never skips):

      p11-search   `search q= kind_n= n= us= yields= refine= cap=`:
                   refine=1 on every step that appends one letter to the
                   previous query (q = previous q + 1, previous q >= 1);
                   refine=0 on the first letter (q=1); yields >= sum(kind_n)
                   / 4096
      p11-otg      `otg add`: reads <= 1 + n; `otg resolve`: reads ==
                   ceil(bytes / 32 KB), and at most 1 + the script's
                   reboot / db_rebuild lines + `# otg-resolves: N` (one pass
                   per open); no `playlist_create ... otg.m3u8` after an
                   `otg play`; no `otg add|save|clear` inside an animation
                   (anim start .. anim end)
      p11-hold     `hold open ctx= items=`: items is the ui_hold_items()
                   mask for the context (Browse bits may be dropped for a
                   spoken or untagged row); ctx is 0..3 or np|song|group|
                   in_otg
      p11-vlimit   `vlimit set=<db> code= applied=`: applied <= set; once a
                   set shows code=1 (or `# vlimit: locked` in the header)
                   the next set needs a `vlimit unlock ok=1` first
      p11-about    `about page=<n>` n in 1..4; a script with a >= 3000 ms
                   chord shows `devmode enter`
      p11-sys      `sys charging state=` follows a `sleep: charging` line,
                   state in charging|charged, pop in key|unplug|usb;
                   `# sys: dark` requires, after the first `sys charging`
                   line, the backlight-off path (`sleep: backlight off`, or
                   an `unlit enter` / `unlit skip` line) and no second
                   `sleep: charging` before it (no nested loop)
      p11-qw2      `np art src= reads= us=`: src in bundle|legacy|ph-note|
                   ph-book|ph-render; reads <= 1; no np_art_render line
                   unless the last `np art` src is legacy or ph-render;
                   `# qw2: bundle` requires every `np art` after the first
                   src=bundle line to be src=bundle
    """
    base = os.path.basename(script)
    declared = set()
    for l in script_lines:
        m = re.match(r'#\s*trace:\s*(.*)$', l)
        if m:
            declared.update(m.group(1).split())
    m = re.match(r'ipodui-p11-([a-z]+)', base)
    if m:
        declared.add(m.group(1))
    if base.startswith('ipodui-qw2-'):
        declared.add('qw2')

    def hdr(name):
        for l in script_lines:
            m = re.match(r'#\s*%s:\s*(.*)$' % name, l)
            if m:
                return m.group(1).strip()
        return None

    def count(rx):
        return sum(1 for l in script_lines if re.match(rx, l))

    # ---- p11-panelsync (every script; U2a): a panel shows what was wanted
    want, shows, errs = None, 0, []
    for i, l in enumerate(lines):
        m = RE_P11_PANEL_WANT.search(l)
        if m:
            want = (m.group(1), m.group(2))
            continue
        m = RE_P11_PANEL_SHOW.search(l)
        if m:
            shows += 1
            got = (m.group(1), m.group(2))
            if got[0] == 'left':
                # preview.c gone_ops: the key of a screen just popped
                # (a multi-level pop, e.g. Time Zone city -> Date & Time)
                # draws nothing and keeps what is on the LCD until the
                # parent reports its row; not a different row's panel
                continue
            if want is None:
                errs.append('panel show key=%s arg=%s before any panel '
                            'want (log line %d)' % (got + (i + 1,)))
            elif got != want:
                errs.append('panel show key=%s arg=%s != last want key=%s '
                            'arg=%s (log line %d)' % (got + want + (i + 1,)))
    if 'panelsync' in declared and not shows:
        errs.append('no "panel show" line in the log of %s (declared '
                    '# trace: panelsync)' % base)
    if errs:
        bad('p11-panelsync: ' + '; '.join(errs[:3]))
    elif shows:
        ok('p11-panelsync (%d shows match their want)' % shows)

    # ---- p11-arena (every script)
    fulls = [(i, m) for i, l in enumerate(lines)
             for m in [RE_P11_ARENA_FULL.search(l)] if m]
    pushes = [m for l in lines for m in [RE_P11_ARENA_PUSH.search(l)] if m]
    memcore = [l for l in lines if RE_P11_MEM_ALLOC.search(l)]
    if fulls and (hdr('arena') or '').split()[:1] != ['full-ok']:
        bad('p11-arena: %d "arena full" line(s) in %s (first: need=%s, log '
            'line %d); a script that wants the fallback declares "# arena: '
            'full-ok <why>"' % (len(fulls), base, fulls[0][1].group(1),
                                fulls[0][0] + 1))
    elif memcore:
        bad('p11-arena: screen-state core_alloc after shell start in %s: %s'
            % (base, memcore[0].strip()))
    elif pushes:
        ok('p11-arena (%d pushes, 0 arena full, 0 screen core_alloc)'
           % len(pushes))

    def need(kind, found, what):
        if kind in declared and not found:
            bad('p11-%s: no "%s" line in the log of %s (the screen must '
                'emit it, brief 4.10)' % (kind, what, base))
            return False
        return kind in declared

    # ---- p11-search
    sr = [(i, m) for i, l in enumerate(lines)
          for m in [RE_P11_SEARCH.search(l)] if m]
    if need('search', sr, 'search q='):
        prev, errs = 0, []
        for i, m in sr:
            q, refine = int(m.group(1)), int(m.group(6))
            n_entries = sum(int(x) for x in m.group(2).split(','))
            yields = int(m.group(5))
            if q == prev + 1 and prev >= 1 and not refine:
                errs.append('q=%d after q=%d has refine=0 (log line %d)'
                            % (q, prev, i + 1))
            # q is the FOLDED length: a typed space ("h" -> "h ") folds to
            # the same query, and refining an identical query is right
            # (the same rows); only a first letter after none may not refine
            if q == 1 and refine and prev != 1:
                errs.append('q=1 has refine=1 (log line %d)' % (i + 1))
            if yields < n_entries // 4096:
                errs.append('yields=%d < n_entries/4096 = %d (log line %d)'
                            % (yields, n_entries // 4096, i + 1))
            prev = q
        if errs:
            bad('p11-search: ' + '; '.join(errs[:3]))
        else:
            ok('p11-search (%d steps, refine on appended letters, yields '
               'ok)' % len(sr))

    # ---- p11-otg
    otg_any = [l for l in lines
               if re.search(r'\botg (add|resolve|play|save|clear)\b', l)]
    if need('otg', otg_any, 'otg add/resolve/play/save/clear'):
        errs = []
        for l in lines:
            m = RE_P11_OTG_ADD.search(l)
            if m and int(m.group(2)) > 1 + int(m.group(1)):
                errs.append('otg add reads=%s > 1 + n=%s'
                            % (m.group(2), m.group(1)))
            m = RE_P11_OTG_RES.search(l)
            if m:
                # one pass: the list and the tag file in 32 KB blocks,
                # each ending on a short read (bytes = list + tag file;
                # reads = the resolve's own); first seen with generations,
                # where a rebuild's new commit ID makes the store resolve
                want = 1 - (-int(m.group(3)) // P11_OTG_RESOLVE_BLOCK)
                if int(m.group(4)) > want:
                    errs.append('otg resolve reads=%s > 1 + ceil(bytes=%s / '
                                '32 KB) = %d (one pass)'
                                % (m.group(4), m.group(3), want))
        nres = sum(1 for l in lines if RE_P11_OTG_RES.search(l))
        allowed = 1 + count(r'\s*(reboot|db_rebuild_keep_otg|rebuild_db)\b')
        extra = hdr('otg-resolves')
        if extra:
            allowed += int(extra.split()[0])
        if nres > allowed:
            errs.append('%d otg resolve lines > %d allowed (one per open: '
                        '1 + reboots/rebuilds + "# otg-resolves: N")'
                        % (nres, allowed))
        played = False
        depth = 0
        for i, l in enumerate(lines):
            if RE_P11_OTG_PLAY.search(l):
                played = True
            if played and re.search(r'playlist_create.*otg\.m3u8', l):
                errs.append('playlist_create of otg.m3u8 after otg play '
                            '(log line %d)' % (i + 1))
            if RE_START.search(l):
                depth += 1
            elif RE_END.search(l):
                depth = max(0, depth - 1)
            elif depth and RE_P11_OTG_WRITE.search(l):
                errs.append('"%s" inside an animation (log line %d)'
                            % (RE_P11_OTG_WRITE.search(l).group(0), i + 1))
        if errs:
            bad('p11-otg: ' + '; '.join(errs[:3]))
        else:
            ok('p11-otg (%d lines, %d resolve, reads match)'
               % (len(otg_any), nres))

    # ---- p11-hold
    ho = [(i, m) for i, l in enumerate(lines)
          for m in [RE_P11_HOLD.search(l)] if m]
    if need('hold', ho, 'hold open ctx='):
        errs = []
        for i, m in ho:
            ctx, mask = m.group(1).lower(), int(m.group(2), 0)
            if ctx not in P11_HOLD_CTX:
                errs.append('unknown ctx=%s (log line %d)' % (ctx, i + 1))
                continue
            full, nobrowse = P11_HOLD_CTX[ctx]
            if mask not in (full, nobrowse):
                errs.append('ctx=%s items=0x%x, want 0x%x (or 0x%x for a '
                            'spoken/untagged row) (log line %d)'
                            % (ctx, mask, full, nobrowse, i + 1))
        if errs:
            bad('p11-hold: ' + '; '.join(errs[:3]))
        else:
            ok('p11-hold (%d opens, masks match ui_hold_items)' % len(ho))

    # ---- p11-vlimit
    vl = []
    for i, l in enumerate(lines):
        m = RE_P11_VL_SET.search(l)
        if m:
            vl.append((i, 'set', m))
            continue
        m = RE_P11_VL_UNLOCK.search(l)
        if m:
            vl.append((i, 'unlock', m))
    if need('vlimit', vl, 'vlimit set='):
        errs = []
        locked = (hdr('vlimit') or '').split()[:1] == ['locked']
        unlocked = False
        for i, kind, m in vl:
            if kind == 'unlock':
                if m.group(1) == '1':
                    unlocked = True
                continue
            lim, code, applied = (int(m.group(1)), m.group(2),
                                  int(m.group(3)))
            if applied > lim:
                errs.append('vlimit set=%d applied=%d > limit (log line %d)'
                            % (lim, applied, i + 1))
            if locked and not unlocked:
                errs.append('vlimit set while locked without "vlimit '
                            'unlock ok=1" (log line %d)' % (i + 1))
            locked, unlocked = code == '1', False
        if errs:
            bad('p11-vlimit: ' + '; '.join(errs[:3]))
        else:
            ok('p11-vlimit (%d lines, applied <= limit, unlock before '
               'locked change)' % len(vl))

    # ---- p11-about (the 7G's three pages and ours, Save Diagnostics:
    # apps/ipodui/screens/about.c PAGES)
    ABOUT_PAGES = 4
    ab = [(i, m) for i, l in enumerate(lines)
          for m in [RE_P11_ABOUT.search(l)] if m]
    if need('about', ab, 'about page='):
        errs = ['about page=%s (log line %d)' % (m.group(1), i + 1)
                for i, m in ab if not 1 <= int(m.group(1)) <= ABOUT_PAGES]
        chords = [int(m.group(1)) for l in script_lines
                  for m in [RE_P8A_CHORD.match(l)] if m]
        if any(c >= P8A_DEVMODE_HOLD_MS for c in chords) \
                and not any(re.search(r'\bdevmode enter\b', l)
                            for l in lines):
            errs.append('chord >= 3000 ms but no "devmode enter" line')
        if errs:
            bad('p11-about: ' + '; '.join(errs[:3]))
        else:
            ok('p11-about (%d page lines)' % len(ab))

    # ---- p11-sys
    sy = [l for l in lines
          if re.search(r'\bsys (charging|lowbatt)\b|\busb ejected\b', l)]
    if need('sys', sy, 'sys charging / sys lowbatt / usb ejected'):
        errs = []
        first_ch = None
        sleeps = 0
        for i, l in enumerate(lines):
            if re.search(r'\bsleep: charging\b', l):
                sleeps += 1
            m = RE_P11_SYS_CH.search(l)
            if m:
                if m.group(1) not in ('charging', 'charged'):
                    errs.append('state=%s (log line %d)'
                                % (m.group(1), i + 1))
                if not sleeps:
                    errs.append('sys charging with no earlier "sleep: '
                                'charging" (log line %d)' % (i + 1))
                if first_ch is None:
                    first_ch = i
            m = RE_P11_SYS_POP.search(l)
            if m and m.group(1) not in ('key', 'unplug', 'usb'):
                errs.append('pop=%s (log line %d)' % (m.group(1), i + 1))
        if (hdr('sys') or '').split()[:1] == ['dark']:
            if first_ch is None:
                errs.append('# sys: dark but no sys charging line')
            else:
                after = lines[first_ch + 1:]
                dark = [j for j, l in enumerate(after) if re.search(
                    r'sleep: backlight off|\bunlit (enter|skip)', l)]
                nest = [j for j, l in enumerate(after)
                        if re.search(r'\bsleep: charging\b', l)]
                if not dark:
                    errs.append('no backlight-off path (sleep: backlight '
                                'off / unlit enter|skip) after sys charging')
                elif nest and nest[0] < dark[0]:
                    errs.append('second "sleep: charging" before the dark '
                                'path (nested loop)')
        if errs:
            bad('p11-sys: ' + '; '.join(errs[:3]))
        else:
            ok('p11-sys (%d lines)' % len(sy))

    # ---- p11-qw2
    qa = {i: m for i, l in enumerate(lines)
          for m in [RE_P11_NPART.search(l)] if m}
    if need('qw2', qa, 'np art src='):
        errs, seen_bundle, last = [], False, None
        want_bundle = (hdr('qw2') or '').split()[:1] == ['bundle']
        for i, l in enumerate(lines):
            m = qa.get(i)
            if m:
                src, reads = m.group(1), int(m.group(2))
                if src not in P11_SRC:
                    errs.append('src=%s (log line %d)' % (src, i + 1))
                if reads > 1:
                    errs.append('np art reads=%d > 1 (log line %d)'
                                % (reads, i + 1))
                if want_bundle and seen_bundle and src != 'bundle':
                    errs.append('src=%s after the bundle pass (log line %d)'
                                % (src, i + 1))
                seen_bundle = seen_bundle or src == 'bundle'
                last = src
            elif re.search(r'\bnp_art_render\b', l) \
                    and last not in ('legacy', 'ph-render'):
                errs.append('np_art_render with last src=%s (log line %d)'
                            % (last, i + 1))
        if errs:
            bad('p11-qw2: ' + '; '.join(errs[:3]))
        else:
            ok('p11-qw2 (%d np art lines)' % len(qa))


RE_DRIFT_COVER = re.compile(r'panel cover key=([0-9a-f]{16}) set=(-?\d+) '
                            r'how=(\w+)')


def check_driftrepeat(lines, script, script_lines, ok, bad):
    """#58 (owner, 2026-10-09): the drift never shows the same cover twice
    in a row, including at the cycle wrap and after the panel set changed
    order. apps/ipodui/preview.c logs `panel cover key=<16 hex> set=<n>
    how=<draw|first|strip|direct>` each time a new cover goes on screen
    (`panel cover hold ...` lines, the held pan of a lone cover, are not
    matched). Every script: two consecutive `panel cover key=` lines never
    name the same key. `# trace: driftrepeat` (or a script named
    ipodui-fb-drift*) FAILS when there is no such line.
    """
    base = os.path.basename(script)
    declared = base.startswith('ipodui-fb-drift') or any(
        re.match(r'#\s*trace:.*\bdriftrepeat\b', l) for l in script_lines)
    covers = [(i, m.group(1), m.group(2), m.group(3))
              for i, l in enumerate(lines)
              for m in [RE_DRIFT_COVER.search(l)] if m]
    if declared and not covers:
        bad('driftrepeat: no "panel cover key=" line in the log of %s '
            '(declared)' % base)
        return
    errs = []
    # A reboot starts a fresh sequence: the first cover of a new boot may
    # equal the last one before it (nothing is on screen in between).
    boots = [i for i, l in enumerate(lines)
             if 'sim_script: reboot' in l or 'ipodui: arena reserve size=' in l]
    for (i0, k0, _, _), (i1, k1, s1, how) in zip(covers, covers[1:]):
        if any(i0 < b < i1 for b in boots):
            continue
        if k0 == k1:
            errs.append('key %s set=%s how=%s again at log line %d (last '
                        'at line %d)' % (k1, s1, how, i1 + 1, i0 + 1))
    if errs:
        bad('driftrepeat: ' + '; '.join(errs[:3]))
    elif covers:
        ok('driftrepeat (%d covers, no cover twice in a row)' % len(covers))


RE_PANEL_SET = re.compile(r'panel set read set=(-?\d+) why=([\w-]+)')


def check_panelset(lines, script_lines, ok, bad):
    """Fit-3a #73 (owner): the drift slot holds one cover of one set
    (music, books); moving between their rows must never leave the panel
    without a cover of the selected set, on flash and hard-disk builds
    alike. apps/ipodui/preview.c logs `panel set read set=<n> why=<w>` at
    every read (read-*), deferral (defer-*), skip (skip-*) and wake
    (wake-setchange) of the slot, and why=empty for a coverless drift
    draw. Declared by `# trace: panelset`: FAILS when there is no such
    line, or on any why=empty line."""
    if not any(re.match(r'#\s*trace:.*\bpanelset\b', l)
               for l in script_lines):
        return
    ev = [(i, m.group(1), m.group(2)) for i, l in enumerate(lines)
          for m in [RE_PANEL_SET.search(l)] if m]
    if not ev:
        bad('panelset: no "panel set read" line in the log (declared)')
        return
    empty = [(i, st) for i, st, w in ev if w == 'empty']
    if empty:
        bad('panelset: %d coverless panel draw(s), first set=%s at log '
            'line %d' % (len(empty), empty[0][1], empty[0][0] + 1))
        return
    wakes = sum(1 for _, _, w in ev if w == 'wake-setchange')
    defers = sum(1 for _, _, w in ev if w.startswith('defer'))
    ok('panelset (%d slot events, %d wake reads, %d deferrals, never '
       'empty)' % (len(ev), wakes, defers))


RE_PANEL_HOLD = re.compile(r'panel hold set=(-?\d+) why=([\w-]+) ms=(-?\d+)')
RE_DRIFT_STEP = re.compile(r'panel drift step=(\d+)')


def check_panelhold(lines, script_lines, ok, bad):
    """#73 follow-up (owner, device: "a small skip at the beginning of the
    slideshow"): a set's first cover holds still (phase frozen) through
    its fade and ~0.5 s while the next cover is prefetched, then the pan
    starts from its beginning. apps/ipodui/preview.c logs `panel hold
    set=<n> why=begin-*|end-* ms=<held>`; pan steps are `panel drift
    step=<half steps>`. Declared by `# trace: panelhold`: FAILS with no
    hold end, a hold over 2 s, or a first pan step after a hold end
    bigger than one normal step (step > 1: the pan jumped ahead)."""
    if not any(re.match(r'#\s*trace:.*\bpanelhold\b', l)
               for l in script_lines):
        return
    ends, errs, pending = 0, [], None
    for i, l in enumerate(lines):
        m = RE_PANEL_HOLD.search(l)
        if m:
            if m.group(2).startswith('end'):
                ends += 1
                pending = i
                if int(m.group(3)) > 2100:
                    errs.append('hold of %s ms at log line %d'
                                % (m.group(3), i + 1))
            else:
                pending = None
            continue
        if 'sim_script: drift_phase' in l:
            pending = None          # a scripted phase jump, not a hold
            continue
        m = RE_DRIFT_STEP.search(l)
        if m and pending is not None:
            if int(m.group(1)) > 1:
                errs.append('first pan step=%s after the hold end at log '
                            'line %d (line %d)' % (m.group(1), pending + 1,
                                                   i + 1))
            pending = None
    if not ends:
        bad('panelhold: no "panel hold ... why=end-" line (declared)')
    elif errs:
        bad('panelhold: ' + '; '.join(errs[:3]))
    else:
        ok('panelhold (%d holds, each pan starts at its first step)' % ends)


RE_RA_FETCH = re.compile(r'rowart fetch key=([0-9a-f]{16}) res=(-?\d+) '
                         r'lookup_us=(\d+) read_us=(\d+) decode_us=(\d+)')
RE_RA_DRAW = re.compile(r'rowart draw key=([0-9a-f]{16}) thumb=([01]) '
                        r'draw_us=(\d+)')
RE_RA_FILL = re.compile(r'rowart fill row=(\d+) fill_us=(\d+) push_us=(\d+) '
                        r'px=(\d+) since_glide_ms=(-?\d+)')
RE_PAN_COST = re.compile(r'panel cost kind=(pan|fade) step=(\d+) blit_us=(\d+) '
                         r'push_us=(\d+) gap_us=(\d+) px=(\d+)')
# Instruction estimates per pixel, ARM7TDMI-class (PP5022), the method of
# the panels.c/preview.c comments: a window blit moves whole rows with
# ldm/stm (4 words per 2 instructions = 0.25 per 16-bit pixel); a
# cross-fade pixel is 2 loads, 1 compare, ~10 ALU, 1 store (equal pixels
# are skipped, so this is the upper bound).
EST_INSN_PER_PX = {'pan': 0.25, 'fade': 14.0}


def _stats(v):
    return 'n=%d min=%d mean=%d max=%d' % (len(v), min(v), sum(v) // len(v),
                                           max(v))


def check_p8b(lines, script, script_lines, ok, bad):
    """Phase 8b-0 performance traces (apps/ipodui/ptrace.h hooks; sim
    microseconds are host CPU time, relative only). A script selects a rule
    with `# trace: p8bperf p8bpan`; a selected rule that finds none of its
    lines FAILS. With the lines present a rule always runs.

      p8bperf  row-art fill latency. `rowart fetch key= res= lookup_us=
               read_us= decode_us=` (art cache index lookup, pak read;
               thumbnails are stored ready to blit so decode_us is 0),
               `rowart draw key= thumb= draw_us=` (the blit) and `rowart
               fill row= fill_us= push_us= px= since_glide_ms=` (an at-rest
               fill: row redraw, then the LCD push of the art box). Fails
               when decode_us != 0 (a decode crept into the browse path,
               hardware.md "pre-compute at sync time"), a fetch/draw/fill
               stage exceeds 20 ms (sim sanity), or a fill starts more than
               1000 ms after the last glide frame. Always prints per-stage
               min/mean/max and the worst fill, so the slow stage is named.
      p8bpan   panel drift step cost. `panel cost kind=pan|fade step=
               blit_us= push_us= gap_us= px=`. Fails when a step's compose
               or push exceeds 20 ms (sim sanity) or a fade step is not in
               1..25. Prints compose and push stats, the estimated
               instructions per step (EST_INSN_PER_PX), the gap (frame
               pacing) stats, and the number of pan steps that skipped a
               pixel (step jump > 1: visible stutter).
               `# pan-max-jump: N` makes more than N skipped steps a FAIL
               (default: report only).
    """
    base = os.path.basename(script)
    decl = set()
    for l in script_lines:
        m = re.match(r'#\s*trace:\s*(.*)', l)
        if m:
            decl.update(m.group(1).split())
    stage_max_us, fill_max_ms, step_max_us = 20000, 1000, 20000

    fetch = [(int(m.group(1), 16),) + tuple(int(g) for g in m.groups()[1:])
             for m in map(RE_RA_FETCH.search, lines) if m]
    draw = [int(m.group(3)) for m in map(RE_RA_DRAW.search, lines) if m]
    fill = [tuple(int(g) for g in m.groups())
            for m in map(RE_RA_FILL.search, lines) if m]
    if 'p8bperf' in decl and not (fetch or draw or fill):
        bad('p8b-rowart: no "rowart fetch/draw/fill" line in the log of %s '
            '(declared)' % base)
    elif fetch or draw or fill:
        errs = []
        for key, res, lk, rd, dec in fetch:
            if dec != 0:
                errs.append('fetch key=%016x decode_us=%d, must be 0 (a '
                            'decode in the browse path)' % (key, dec))
            if max(lk, rd) > stage_max_us:
                errs.append('fetch key=%016x lookup_us=%d read_us=%d over '
                            '%d' % (key, lk, rd, stage_max_us))
        for d in draw:
            if d > stage_max_us:
                errs.append('draw_us=%d over %d' % (d, stage_max_us))
        for row, fu, pu, px, since in fill:
            if max(fu, pu) > stage_max_us:
                errs.append('fill row=%d fill_us=%d push_us=%d over %d'
                            % (row, fu, pu, stage_max_us))
            if since > fill_max_ms or since < 0:
                errs.append('fill row=%d since_glide_ms=%d outside '
                            '0..%d' % (row, since, fill_max_ms))
        if errs:
            bad('p8b-rowart: ' + '; '.join(errs[:4]))
        else:
            ok('p8b-rowart (%d fetch, %d draw, %d fill)'
               % (len(fetch), len(draw), len(fill)))
        if fetch:
            print('note p8b-rowart lookup_us %s; read_us %s; decode_us 0'
                  % (_stats([f[2] for f in fetch]),
                     _stats([f[3] for f in fetch])))
        if draw:
            print('note p8b-rowart draw_us %s' % _stats(draw))
        if fill:
            print('note p8b-rowart fill_us %s; push_us %s; since_glide_ms %s'
                  % (_stats([f[1] for f in fill]),
                     _stats([f[2] for f in fill]),
                     _stats([f[4] for f in fill])))
            w = max(fill, key=lambda f: f[1] + f[2])
            print('note p8b-rowart worst fill row=%d: fill_us=%d push_us=%d'
                  % (w[0], w[1], w[2]))

    cost = [(m.group(1),) + tuple(int(g) for g in m.groups()[1:])
            for m in map(RE_PAN_COST.search, lines) if m]
    if 'p8bpan' in decl and not cost:
        bad('p8b-pan: no "panel cost kind=" line in the log of %s '
            '(declared)' % base)
    elif cost:
        errs = []
        for kind, step, blit, push, gap, px in cost:
            if max(blit, push) > step_max_us:
                errs.append('%s step=%d blit_us=%d push_us=%d over %d'
                            % (kind, step, blit, push, step_max_us))
            if kind == 'fade' and not 1 <= step <= 25:
                errs.append('fade step=%d outside 1..25' % step)
        pans = [c for c in cost if c[0] == 'pan']
        fades = [c for c in cost if c[0] == 'fade']
        jumps = sum(1 for a, b in zip(pans, pans[1:])
                    if 1 < b[1] - a[1] < 1000)
        maxj = None
        for l in script_lines:
            m = re.match(r'#\s*pan-max-jump:\s*(\d+)', l)
            if m:
                maxj = int(m.group(1))
        if maxj is not None and jumps > maxj:
            errs.append('%d pan steps skipped a pixel (> %d allowed)'
                        % (jumps, maxj))
        if errs:
            bad('p8b-pan: ' + '; '.join(errs[:4]))
        else:
            ok('p8b-pan (%d pan, %d fade steps)' % (len(pans), len(fades)))
        for name, rows in (('pan', pans), ('fade', fades)):
            if not rows:
                continue
            # gap: 0 on the first step, huge after a pause: left out
            gaps = [r[4] for r in rows if 0 < r[4] < 1000000]
            est = int(EST_INSN_PER_PX[name] * rows[0][5])
            print('note p8b-pan %s: compose_us %s; push_us %s; est %d '
                  'instr/step (estimate, not measured); gap_us %s'
                  % (name, _stats([r[2] for r in rows]),
                     _stats([r[3] for r in rows]), est,
                     _stats(gaps) if gaps else 'none'))
        if pans:
            print('note p8b-pan pan steps that skipped a pixel: %d of %d'
                  % (jumps, len(pans)))


def check_ab(lines, script_lines, ok, bad):
    """Phase 6 audiobook rules (docs/phase6-brief.md Rev 1 item 7, Rev 2,
    D16). The AB_TR_* lines come from apps/ipodui/ab_book.h. Rules skip with
    a note when their lines are absent, except under `# trace: ab` (a script
    that declares it needs at least one `ab ...` line, so a build before
    B/M land fails loudly instead of passing silently).

      aballoc   no `ab alloc` / `ab core_alloc` line: a book never
                allocates while playing (the memory rule is "no core_alloc
                while audio_status() != 0"; the sim's expect_ab_mem_same
                is the direct check)
      abopen    `ab open book= reads=` (real read() calls): reads <= %d after `ab pak hit` (the
                store sector, the index, the blob); one more after `ab pak
                miss` (the store sector, the file's path, the header walk,
                the titles)
      abch      a book track whose chapter table was written (`ab slot w=S
                n=N`, N > 0, attributed to the next track snapshotted after
                it) must end with a snapshot that has them: the last `np
                snap id=X ... book=1` of that track has ch > 0 (the P6
                blocker: a book started from the shell showed ch=0)
      abstore   `ab store w=idle`: at most `# ab-idle-max: N` (default %d)
                per script. The log has no clock, so this bounds the
                forced 30-minute checkpoint wake (one per 30 min while
                playing) AND the fill-ridden ones in a short script
      abpending a track change inside a book is a PENDING save: no
                `ab store w=now` between `ab_mark track_change_begin` and
                `ab_mark track_change_end` (the script brackets its NEXT)
      abslot    `ab slot w=S`: with `# ab-slots: strict`, consecutive
                writes name different slots (the writer fills the slot the
                playing id3 does NOT reference); `ab slot read ... ok=0`
                is counted in a note
      abspeed   `# ab-speed: a b c`: the `ab speed N` lines contain those
                percents as an ordered subsequence (75 / 100 / 125 only)
    """ % (AB_OPEN_READS_MAX, AB_IDLE_MAX)
    declared = lambda pat: any(re.match(pat, l) for l in script_lines)
    abl = [(i, l) for i, l in enumerate(lines) if RE_AB_ANY.search(l)]
    if declared(r'#\s*trace:.*\bab\b') and not abl:
        bad('script declares "# trace: ab" but the log has no "ab ..." line '
            '(apps/ipodui/ab_book.h AB_TR_*; needs agents B and M)')
        return
    if not abl:
        print('note ab rules skipped: no "ab ..." trace lines')
        return
    ok('ab trace lines: %d' % len(abl))

    al = [i for i, l in enumerate(lines)
          if re.search(r'\bab (alloc|core_alloc)\b', l)]
    if al:
        bad('ab allocation logged (a book allocates nothing while playing), '
            'line %d: %s' % (al[0] + 1, lines[al[0]].strip()))
    else:
        ok('no ab allocation logged')

    miss_extra = AB_OPEN_MISS_EXTRA     # `# ab-open-miss-max: N` overrides
    for l in script_lines:
        mm = re.match(r'#\s*ab-open-miss-max:\s*(\d+)', l)
        if mm:
            miss_extra = int(mm.group(1)) - AB_OPEN_READS_MAX
    opens, miss = [], False
    for i, l in enumerate(lines):
        m = RE_AB_PAK.search(l)
        if m:
            miss = m.group(1) == 'miss'
            continue
        m = RE_AB_OPEN.search(l)
        if m:
            opens.append((i, int(m.group(2)),
                          AB_OPEN_READS_MAX + (miss_extra if miss else 0)))
    over = [(i, r, cap) for i, r, cap in opens if r > cap]
    if over:
        bad('ab open reads=%d > %d (the store sector + a blob via the index; '
            'a single-file miss: + the path + header walk + titles), line %d'
            % (over[0][1], over[0][2], over[0][0] + 1))
    elif opens:
        ok('ab open reads <= %d/%d hit/miss (%d opens)'
           % (AB_OPEN_READS_MAX, AB_OPEN_READS_MAX + miss_extra, len(opens)))

    # abch: walk snapshots in order; a slot write with n > 0 belongs to
    # the track whose LAST snapshot follows it (a start: write after the
    # first snap, then a re-snap; a track change: write, then the snap)
    snaps = []                      # [track id, last ch, book, written?]
    wrote = False
    for l in lines:
        m = RE_AB_SLOTW.search(l)
        if m and int(m.group(2)) > 0:
            wrote = True
            continue
        m = RE_NP_SNAP.search(l)
        if not m:
            continue
        tid, ch, book = m.group(1), int(m.group(2)), m.group(3) == '1'
        if snaps and snaps[-1][0] == tid:
            snaps[-1][1] = ch
            snaps[-1][3] = snaps[-1][3] or wrote
        else:
            snaps.append([tid, ch, book, wrote])
        wrote = False
    nochap = [t for t in snaps if t[2] and t[3] and t[1] == 0]
    if nochap:
        bad('np snap id=%s ends with ch=0 book=1 although its chapter table '
            'was written ("ab slot w= n>0"): Now Playing never saw the '
            'chapters' % nochap[0][0])
    elif any(t[2] and t[3] for t in snaps):
        ok('book snapshots carry their chapters (%d tracks)'
           % sum(1 for t in snaps if t[2] and t[3]))

    idle = [i for i, l in enumerate(lines)
            if (lambda m: m and m.group(1) == 'idle')(RE_AB_STORE.search(l))]
    cap = AB_IDLE_MAX
    for l in script_lines:
        m = re.match(r'#\s*ab-idle-max:\s*(\d+)', l)
        if m:
            cap = int(m.group(1))
    if len(idle) > cap:
        bad('%d "ab store w=idle" lines > %d (forced checkpoint wakes are '
            'capped at one per 30 min; fills may add more, declare '
            '"# ab-idle-max: N" if they legitimately do), first line %d'
            % (len(idle), cap, idle[0] + 1))
    elif any(RE_AB_STORE.search(l) for l in lines):
        ok('ab store idle writes %d <= %d' % (len(idle), cap))

    inside = False
    for i, l in enumerate(lines):
        m = RE_AB_MARK.search(l)
        if m:
            inside = m.group(1) == 'track_change_begin' or \
                (inside and m.group(1) != 'track_change_end')
            continue
        m = RE_AB_STORE.search(l)
        if inside and m and m.group(1) == 'now':
            bad('"ab store w=now" on a track change (it must be pending, '
                'written at pause/stop/USB/shutdown), line %d: %s'
                % (i + 1, l.strip()))
            break
    else:
        if any(RE_AB_MARK.search(l) and 'track_change_begin' in l
               for l in lines):
            ok('track changes save pending, never write-now')

    sw = [(i, int(m.group(1))) for i, l in enumerate(lines)
          for m in [RE_AB_SLOTW.search(l)] if m]
    rd = [m for l in lines for m in [RE_AB_SLOTR.search(l)] if m]
    if declared(r'#\s*ab-slots:\s*strict'):
        rep = [k for k in range(1, len(sw)) if sw[k][1] == sw[k - 1][1]]
        if len(sw) < 2:
            bad('"# ab-slots: strict" needs two "ab slot w=" lines, got %d'
                % len(sw))
        elif rep:
            bad('two consecutive "ab slot w=%d" writes (line %d): the '
                'writer must fill the slot the playing id3 does not '
                'reference' % (sw[rep[0]][1], sw[rep[0]][0] + 1))
        else:
            ok('ab slot writes alternate (%d writes, %d rejected reads)'
               % (len(sw), sum(1 for m in rd if m.group(2) == '0')))
    elif sw:
        print('note ab slot writes: %d' % len(sw))

    want = None
    for l in script_lines:
        m = re.match(r'#\s*ab-speed:\s*([\d ]+?)\s*$', l)
        if m:
            want = [int(x) for x in m.group(1).split()]
    if want is not None:
        got = [int(m.group(1)) for l in lines for m in [RE_AB_SPEED.search(l)]
               if m]
        if any(g not in (75, 100, 125) for g in got):
            bad('ab speed outside 75/100/125: %s' % got)
        else:
            it = iter(got)
            if all(any(g == w for g in it) for w in want):
                ok('ab speed lines %s contain %s in order' % (got, want))
            else:
                bad('ab speed lines %s do not contain %s in order (books at '
                    '75/100/125, music and podcasts always 100)'
                    % (got, want))


def check_np(lines, script_lines, ok, bad):
    """Phase 5 Now Playing rules (docs/phase5-brief.md section 8). A rule
    whose lines are absent skips with a note, EXCEPT where the script
    declares it (`# seeks: N`, `# trace: np`, `# trace: shuf`,
    `# samtrack-nobody`): then absent lines FAIL (docs/process.md).

      npband   `update np:<band> y= h=`: inside the body (y >= 20, y + h
               <= 240); the band must be one the screen has (NP_BANDS:
               body, info <= 178 rows, title, row, rowfull, stars,
               glyphs, chapter, counter <= 34 rows)
      npstrip  `panel strip claim owner=PANEL|DRIFT top=Now Playing` never
               appears (brief Revision 1 item 5)
      npalloc  no `np alloc` / `core_alloc` line and no `np io` line while
               playing with `lit=0` (proposed lines, skipped while absent)
      npseek   `np seek ms= id= pre=` lines (np_playback.c; `dropped (..)`
               lines are not seeks): exactly the script's `# seeks: N`
      npsnap   `# trace: np` requires `np snap` lines; `# samtrack-nobody`
               requires two `np snap` lines with one track id (a seq move
               inside a track) and no `update np:body` after the second
      npwake   after each `unlit exit` whose first LCD update is Now
               Playing's, an `np draw id=<a> playing=<b>` comes before
               that update and has a == b
               (#58: the wake paints the playing track); `# np-wake`
               requires one
      shuf     `shuf:` lines (lib_shuffle.c): never `record write
               failed|short`, `no file name`, `refill stop`, nor a
               `start|mode .. fail` (unless the script says
               `# shuf-fail-ok`); a refill adds at most 50 entries and
               queued stays under 400; `# trace: shuf` requires lines
      logrange `# logrange: <lo> <hi> [after=<mark>] [until=<mark>] <regex
               with one (\\d+) group>`: every matching line's value (between
               those ab_mark lines, if given) is within lo..hi, and one
               exists
      npviz    `# np: viz-full` (#126, owner 2026-10-25: the Visualizer is
               full screen, no header): Now Playing's bands may start at
               y 0; np:viz and np:body may cover the panel (240 rows),
               np:row the volume overlay over the bars' foot (42 rows);
               a slide back to the headerless page may be 240 rows from 0
      log      `# log: <text>` must appear in the log, `# nolog: <text>`
               must not (any script, any rule's line)
      logafter `# logafter: <mark> <regex>`: a line matching regex comes
               after the script's `ab_mark <mark>` line (order, e.g. a
               checkpoint that rides a storage wake after a USB session)
      lcdpush  (automatic with `# env: SIM_TRACE_LCD_PUSH=1`) every shot's
               changed pixels lie inside the panel pushes since the previous
               shot (check_lcdpush)
      logbetween `# logbetween: <from> <to> [no] <regex>`: between the
               script's `ab_mark <from>` and `ab_mark <to>` lines (`start`
               / `end`: the log's ends) a line matches regex, or with
               `no` none does (#58: no builder I/O while the user is on,
               no builder wake over a long idle once done)
    """
    declared = lambda pat: any(re.match(pat, l) for l in script_lines)
    upd = [(i, m.group(1), int(m.group(2)), int(m.group(3)))
           for i, l in enumerate(lines) for m in [RE_NPUPD.search(l)] if m]
    if not upd:
        print('note np band check skipped: no "update np:" lines')
    else:
        bands, top = NP_BANDS, 20
        if declared(r'#\s*np:\s*viz-full\b'):
            bands = dict(NP_BANDS, viz=240, body=240, row=42)
            top = 0
        # the visualizer view is full screen (no header, changes-from-7g
        # 2.48): its own np:viz band may start at y 0; every other band
        # keeps the header's top
        badb = [(i, n, y, h) for i, n, y, h in upd
                if y < (0 if n == 'viz' else top) or y + h > 240
                or n not in bands or h > bands[n]]
        if badb:
            i, n, y, h = badb[0]
            bad('np:%s band y=%d h=%d outside the body, an unknown band or '
                'over its %d rows (%d bad of %d), line %d'
                % (n, y, h, bands.get(n, 0), len(badb), len(upd), i + 1))
        else:
            ok('np bands (%d np updates, %d full-body)'
               % (len(upd), sum(1 for u in upd if u[1] == 'body')))
    claims = [(i, m.group(1)) for i, l in enumerate(lines)
              for m in [RE_STRIPCLAIM.search(l)]
              if m and 'Now Playing' in m.group(2)
              and m.group(1) in ('PANEL', 'DRIFT')]
    if claims:
        bad('strip claimed by %s while Now Playing is on top, line %d'
            % (claims[0][1], claims[0][0] + 1))
    else:
        ok('strip never claimed by panels or drift under Now Playing')
    allocs = [i for i, l in enumerate(lines)
              if re.search(r'\bnp alloc\b|\bnp core_alloc\b', l)]
    dark = [i for i, l in enumerate(lines)
            if re.search(r'\bnp io\b.*\bplaying=1\b.*\blit=0\b', l)]
    if allocs:
        bad('allocation while Now Playing runs, line %d' % (allocs[0] + 1))
    elif dark:
        bad('Now Playing read storage while dark, line %d' % (dark[0] + 1))
    else:
        ok('no np allocation or dark storage read logged')

    seeks = [i for i, l in enumerate(lines) if RE_NPSEEK.search(l)]
    drops = [m.group(1) for l in lines for m in [RE_NPSEEK_DROP.search(l)]
             if m]
    want = None
    for l in script_lines:
        m = re.match(r'#\s*seeks:\s*(\d+)', l)
        if m:
            want = int(m.group(1))
    if want is not None:
        if len(seeks) != want:
            bad('%d "np seek" lines (+%d dropped), the script declares %d '
                '(exactly one seek per scrub gesture)'
                % (len(seeks), len(drops), want))
        else:
            ok('np seeks: %d as declared (%d dropped)' % (want, len(drops)))

    snaps = [(i, m.group(1), int(m.group(2))) for i, l in enumerate(lines)
             for m in [RE_NPSNAP.search(l)] if m]
    if declared(r'#\s*trace:.*\bnp\b') and not snaps:
        bad('script declares "# trace: np" but the log has no "np snap" '
            'line (np_playback.c logs one per snapshot)')
    elif snaps:
        ok('np snapshots: %d' % len(snaps))
    # npwake (#58 device report: Now Playing woke on the song from before
    # the dark): after each "unlit exit", the first full paint of Now
    # Playing ("np draw id=<drawn> playing=<now>") draws the playing track
    # and comes before any LCD update; `# np-wake` requires one
    exits = [i for i, l in enumerate(lines) if 'unlit exit' in l]
    wakes = 0
    for x in exits:
        draw = None
        upd_at = None
        for j in range(x + 1, len(lines)):
            if 'unlit enter' in lines[j]:
                break
            m = RE_NPDRAW.search(lines[j])
            if m and draw is None:
                draw = (j, int(m.group(1)), int(m.group(2)))
            if re.search(r'\bupdate \S', lines[j]):
                upd_at = j
                break
        if upd_at is None or not re.search(
                r'\bupdate (full nowplaying|np:)', lines[upd_at]):
            continue                # another screen woke (not Now Playing)
        wakes += 1
        if draw is None:
            bad('npwake: Now Playing pushed (line %d) after "unlit exit" '
                '(line %d) with no full paint before it'
                % (upd_at + 1, x + 1))
        elif draw[1] != draw[2]:
            bad('npwake: the first paint after "unlit exit" (line %d) drew '
                'track id=%d while id=%d plays (line %d)'
                % (x + 1, draw[1], draw[2], draw[0] + 1))
    if declared(r'#\s*np-wake') and not wakes:
        bad('script declares "# np-wake" but no "np draw" line follows an '
            '"unlit exit"')
    elif wakes:
        ok('npwake: %d wake paint(s) checked' % wakes)
    if declared(r'#\s*samtrack-nobody'):
        pair = [k for k in range(1, len(snaps))
                if snaps[k][1] == snaps[k - 1][1]
                and snaps[k][2] != snaps[k - 1][2]]
        if not pair:
            bad('"# samtrack-nobody" declared but no two "np snap" lines '
                'share a track id with a moved seq (no seq move in a track)')
        else:
            at = snaps[pair[0]][0]
            body = [i for i, n, y, h in upd if n == 'body' and i > at]
            if body:
                bad('np:body pushed (line %d) after a same-track seq move '
                    '(snap line %d): a seq move only re-points the chapter '
                    'table' % (body[0] + 1, at + 1))
            else:
                ok('same-track seq move without an np:body push')

    shuf = [(i, l) for i, l in enumerate(lines) if 'shuf:' in l]
    if declared(r'#\s*trace:.*\bshuf\b') and not shuf:
        bad('script declares "# trace: shuf" but the log has no "shuf:" line')
    if shuf:
        failok = declared(r'#\s*shuf-fail-ok')
        problems = []
        for i, l in shuf:
            if re.search(r'shuf: record write (failed|short)', l):
                problems.append((i, 'record write failed'))
            elif 'shuf: no file name' in l:
                problems.append((i, 'a queue entry had no file name'))
            elif 'shuf: refill stop' in l:
                problems.append((i, 'a refill stopped'))
            elif not failok and re.search(r'shuf: (start|mode) \w+ fail', l):
                problems.append((i, 'a start/mode change failed'))
            m = RE_SHUF_REFILL.search(l)
            if m and int(m.group(1)) > SHUF_WINDOW:
                problems.append((i, 'a refill added %s > %d entries'
                                 % (m.group(1), SHUF_WINDOW)))
            if m and int(m.group(5)) > SHUF_QUEUE_MAX:
                problems.append((i, 'queued=%s > %d' % (m.group(5),
                                                        SHUF_QUEUE_MAX)))
        if problems:
            i, why = problems[0]
            bad('shuf: %s (%d problem(s)), line %d: %s'
                % (why, len(problems), i + 1, lines[i].strip()))
        else:
            ok('shuf: %d lines, no failure, refills within %d'
               % (len(shuf), SHUF_WINDOW))

    text = '\n'.join(lines)
    for l in script_lines:
        m = re.match(r'#\s*logafter:\s*(\S+)\s+(.+?)\s*$', l)
        if not m:
            continue
        mark, pat = m.group(1), m.group(2)
        at = next((i for i, x in enumerate(lines)
                   if re.search(r'ab_mark %s(\s|$)' % re.escape(mark), x)),
                  None)
        if at is None:
            bad('# logafter: no "ab_mark %s" line' % mark)
        elif not any(re.search(pat, x) for x in lines[at + 1:]):
            bad('# logafter: no /%s/ after "ab_mark %s"' % (pat, mark))
        else:
            ok('logafter /%s/ after "ab_mark %s"' % (pat, mark))
    for l in script_lines:
        m = re.match(r'#\s*logbetween:\s*(\S+)\s+(\S+)\s+(no\s+)?(.+?)\s*$',
                     l)
        if not m:
            continue
        a, b, neg, pat = m.group(1), m.group(2), bool(m.group(3)), m.group(4)

        def mark_at(name, after):
            if name == 'start':
                return 0
            if name == 'end':
                return len(lines)
            return next((i for i, x in enumerate(lines)
                         if i >= after and re.search(
                             r'ab_mark %s(\s|$)' % re.escape(name), x)),
                        None)
        i0 = mark_at(a, 0)
        i1 = mark_at(b, i0 or 0) if i0 is not None else None
        if i0 is None or i1 is None:
            bad('# logbetween: no "ab_mark %s" line'
                % (a if i0 is None else b))
            continue
        hits = [i for i in range(i0, i1) if re.search(pat, lines[i])]
        what = '/%s/ between %s and %s' % (pat, a, b)
        if neg and hits:
            bad('# logbetween: %s, first: line %d (%s)'
                % (what, hits[0] + 1, lines[hits[0]].strip()))
        elif not neg and not hits:
            bad('# logbetween: no %s' % what)
        else:
            ok('logbetween %s%s (%d lines)'
               % ('no ' if neg else '', what, i1 - i0))
    for l in script_lines:
        m = re.match(r'#\s*logrange:\s*(\d+)\s+(\d+)\s+(.+?)\s*$', l)
        if not m:
            continue
        lo, hi, pat = int(m.group(1)), int(m.group(2)), m.group(3)
        # optional leading after=<mark> / until=<mark>: only the lines
        # between those ab_mark lines
        span = lines
        while True:
            mm = re.match(r'(after|until)=(\S+)\s+(.+)$', pat)
            if not mm:
                break
            at = next((i for i, x in enumerate(span)
                       if re.search(r'ab_mark %s(\s|$)'
                                    % re.escape(mm.group(2)), x)), None)
            if at is None:
                span = []
            elif mm.group(1) == 'after':
                span = span[at + 1:]
            else:
                span = span[:at]
            pat = mm.group(3)
        vals = [int(g.group(1)) for x in span
                for g in [re.search(pat, x)] if g]
        if not vals:
            bad('# logrange: no log line matches /%s/' % pat)
        elif any(v < lo or v > hi for v in vals):
            bad('# logrange: /%s/ gave %s, outside %d..%d'
                % (pat, vals, lo, hi))
        else:
            ok('logrange /%s/ = %s within %d..%d' % (pat, vals, lo, hi))
    for l in script_lines:
        m = re.match(r'#\s*(no)?log:\s*(.+?)\s*$', l)
        if not m:
            continue
        found = m.group(2) in text
        if m.group(1) and m.group(2).isdigit():
            # a digits-only nolog (a passcode) matches a whole number
            # only: "1234" inside a tick or a timing like 12345 is not it
            found = re.search(r'(?<!\d)%s(?!\d)' % m.group(2),
                              text) is not None
        if m.group(1) and found:
            bad('log contains "%s" (# nolog)' % m.group(2))
        elif not m.group(1) and not found:
            bad('log lacks "%s" (# log)' % m.group(2))
        else:
            ok('%s "%s"' % ('nolog' if m.group(1) else 'log', m.group(2)))

RE_LCDPUSH = re.compile(r'\bsim lcd push x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+)')
RE_SHOTLINE = re.compile(r'sim_script: shot (\S+?)(?:\.bmp|\.png)?\s*$')


RE_TEXT_FORBID = re.compile(r'sim text: forbidden x=(-?\d+) y=(-?\d+) text="(.*)"')
RE_LANG_WARN = re.compile(r'lang: str id=(-?\d+) (zero|out-of-range) at (\S+)')


RE_BG_ON = re.compile(r'busyglyph on run=(\d+) t=(-?\d+)')
RE_BG_OFF = re.compile(r'busyglyph off on=(\d+) t=(-?\d+)')
RE_BG_OFF_DARK = re.compile(r'busyglyph off dark on=(\d+) t=(-?\d+)')
RE_BG_IDLE = re.compile(r'busyglyph (idle|work) t=(-?\d+)')
RE_BG_STEP = re.compile(r'busyglyph step phase=\d+ t=(-?\d+) usb=(\d)')
BG_TAIL_MS = 1000 + 250     # LIB_GAP / LIB_MIN_ON, plus a pass's slack
RE_BG_DARK = re.compile(r'sim_script: (backlight_expire|dark on)\b'
                        r'|busyglyph bl=0|\bunlit enter\b'
                        r'|\bsleep enter why=')
RE_BG_LIT = re.compile(r'sim_script: dark off\b|busyglyph bl=1'
                       r'|\bunlit exit\b|\bsleep lit\b'
                       r'|\busb enter\b')


def check_busyglyph(lines, script_lines, ok, bad):
    """3.20 (owner, 2026-10-25): the header's busy glyph for any library
    update (apps/ipodui/usbbusy.c). Always checked when its lines are
    there: no blink (up only after the work ran >= 1000 ms, then up >=
    1000 ms) and nothing while dark (no "busyglyph step", and no header
    push while the glyph is up, between a backlight_expire / dark on /
    bl=0 / unlit enter and the next dark off / bl=1 / unlit exit).
    Lit work only (owner, 2026-10-25): dark ends it ("busyglyph off dark",
    no minimum: nothing is seen), so it is never up at the end of a run
    that ends dark; lit, it never steps more than 1.25 s past both the
    work's end ("busyglyph idle") and its own 1 s (a glyph with no work
    behind it). A run may end lit inside that tail.
    "# trace: busyglyph" requires at least one on/off pair;
    "# busyglyph-ons: N" requires exactly N."""
    required = any(re.match(r'#\s*trace:.*\bbusyglyph\b', l)
                   for l in script_lines)
    want = [int(m.group(1)) for l in script_lines
            for m in [re.match(r'#\s*busyglyph-ons:\s*(\d+)', l)] if m]
    if not required and not want \
            and not any('busyglyph ' in l for l in lines):
        return
    ons = offs = 0
    up = dark = False
    on_t = idle_t = None
    errs = []
    for i, l in enumerate(lines):
        m = RE_BG_ON.search(l)
        if m:
            ons += 1
            up = True
            on_t, idle_t = int(m.group(2)), None
            if int(m.group(1)) < 1000:
                errs.append('line %d: glyph up after %s ms of work (< 1000)'
                            % (i + 1, m.group(1)))
            continue
        m = RE_BG_OFF.search(l)
        if m:
            offs += 1
            up = False
            if int(m.group(1)) < 1000:
                errs.append('line %d: glyph up only %s ms (< 1000)'
                            % (i + 1, m.group(1)))
            continue
        m = RE_BG_OFF_DARK.search(l)
        if m:
            offs += 1
            up = False
            dark = True         # the dark's own end of it
            continue
        m = RE_BG_IDLE.search(l)
        if m:
            idle_t = int(m.group(2)) if m.group(1) == 'idle' else None
            continue
        m = RE_BG_STEP.search(l)
        if m and up and not dark and idle_t is not None \
                and m.group(2) == '0' \
                and int(m.group(1)) > max(idle_t, on_t) + BG_TAIL_MS:
            errs.append('line %d: glyph up %d ms after the work ended'
                        % (i + 1, int(m.group(1)) - idle_t))
        if RE_BG_DARK.search(l):
            dark = True
        elif RE_BG_LIT.search(l):
            dark = False
        elif dark and 'busyglyph step' in l:
            errs.append('line %d: glyph stepped while dark' % (i + 1))
        elif dark and up and re.search(r'\bupdate header\b', l):
            errs.append('line %d: header pushed while dark with the glyph up'
                        % (i + 1))
    if ons - offs not in (0, 1) or offs > ons:
        errs.append('%d "busyglyph on" vs %d "off"' % (ons, offs))
    if up and dark:
        errs.append('glyph still up at the end of a run that ends dark')
    if required and not ons:
        errs.append('no "busyglyph on" (# trace: busyglyph)')
    if want and ons != want[0]:
        errs.append('%d "busyglyph on", want %d (# busyglyph-ons)'
                    % (ons, want[0]))
    if errs:
        bad('busyglyph: %s%s' % (errs[0], ' (+%d more)' % (len(errs) - 1)
                                 if len(errs) > 1 else ''))
    else:
        ok('busyglyph (%d on/off, none under 1 s, nothing dark)' % ons)


def check_illusion(lines, script_lines, ok, bad):
    """Runtime native illusion (P12 close, the "Rockbox" checklist rows).

    The sim logs every drawn string that contains "rockbox" (any case,
    ".rockbox" too: "sim text: forbidden ...", uisimulator/common/
    sim_cmd_text.c) and every str() in the shell's own files with id 0 or
    an id outside the language array ("lang: str id= ... at file:line", the
    label an unlabeled LANG id reads). Either FAILS the script. A script
    that legitimately shows stock Rockbox screens (the developer combo's
    menus) declares "# illusion: devmenu <why>": forbidden text is then a
    note, the str() warnings still fail. Without "sim text: watch on" the
    build has no watch: a note, which the gate fails, unless the script
    declares "# illusion: preboot <why>" (the sim exits before the shell
    starts, so there is nothing to watch)."""
    watch = any('sim text: watch on' in l for l in lines)
    texts = [m for l in lines for m in [RE_TEXT_FORBID.search(l)] if m]
    # tests/ui/illusion_allow.txt: sentences that may name Rockbox (the
    # Legal credit); a drawn fragment of one of them passes
    allow = []
    ap = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..',
                      'tests', 'ui', 'illusion_allow.txt')
    if os.path.exists(ap):
        allow = [l.strip() for l in open(ap) if l.strip()
                 and not l.lstrip().startswith('#')]
    # (a fragment of four words or more: a bare "Rockbox" label, the
    # LANG id 0 case, is a piece of the credit too and must still fail)
    allowed = [m for m in texts
               if len(m.group(3).split()) >= 4
               and any(m.group(3).strip() in a for a in allow)]
    if allowed:
        print('note illusion: %d drawn text(s) name Rockbox inside an '
              'allowed sentence (tests/ui/illusion_allow.txt): %s'
              % (len(allowed), '; '.join('"%s"' % m.group(3)
                                         for m in allowed[:3])))
    texts = [m for m in texts if m not in allowed]
    langs = [m for l in lines for m in [RE_LANG_WARN.search(l)] if m]
    devmenu = any(re.match(r'#\s*illusion:\s*devmenu\b', l)
                  for l in script_lines)
    if langs:
        for m in langs[:5]:
            bad('illusion: str() with LANG id %s (%s) at %s: an unlabeled or '
                'out-of-range id (id 0 reads "Rockbox")'
                % (m.group(1), m.group(2), m.group(3)))
    if texts:
        what = '; '.join('"%s" at x=%s y=%s' % (m.group(3), m.group(1),
                                                m.group(2))
                         for m in texts[:5])
        if devmenu:
            print('note illusion: %d forbidden text(s) drawn, allowed by '
                  '"# illusion: devmenu": %s' % (len(texts), what))
        else:
            bad('illusion: %d drawn text(s) contain "Rockbox"/".rockbox" '
                '(a user-visible Rockbox string): %s' % (len(texts), what))
    if not langs and not texts:
        if watch:
            ok('illusion: no drawn text contains "rockbox", no str() of '
               'LANG id 0 or out of range')
        elif any(re.match(r'#\s*illusion:\s*preboot\b', l)
                 for l in script_lines):
            print('note illusion: "# illusion: preboot", nothing to '
                  'watch (the sim exits before the shell starts)')
        else:
            print('note illusion: no text watch in this log (a build '
                  'without uisimulator/common/sim_cmd_text.c)')


def check_lcdpush(lines, logpath, ok, bad):
    """Framebuffer-vs-panel rule (#57, after a device-only bug where stock
    lists drew into the framebuffer and never reached the LCD while every
    screenshot, which reads the framebuffer, looked right).

    Active only when the log carries `sim lcd push` lines (a script with
    `# env: SIM_TRACE_LCD_PUSH=1`). For every `shot` the pixels that changed
    since the previous shot (or, for the first shot, any non-black pixel)
    must lie inside the union box of the panel pushes logged between the two
    shots. A shot whose pixels did not change needs no push, so unchanged
    screens never false-positive. Uses the PNGs shots.sh leaves next to the
    log; a shot whose PNG is missing is skipped."""
    if not any(RE_LCDPUSH.search(l) for l in lines):
        return
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from pngdiff import decode
    outdir = os.path.dirname(os.path.abspath(logpath))
    prev, box, n_ok, skipped = None, None, 0, 0
    for l in lines:
        m = RE_LCDPUSH.search(l)
        if m:
            x, y, w, h = (int(g) for g in m.groups())
            if w > 0 and h > 0:
                b = (x, y, x + w, y + h)
                box = b if box is None else (
                    min(box[0], b[0]), min(box[1], b[1]),
                    max(box[2], b[2]), max(box[3], b[3]))
            continue
        m = RE_SHOTLINE.search(l)
        if not m:
            continue
        name = os.path.basename(m.group(1))
        png = os.path.join(outdir, name + '.png')
        if not os.path.exists(png):
            skipped += 1
            box = None
            continue
        w, h, px = decode(png)
        if prev is None:
            changed = [i for i, v in enumerate(px) if v != (0, 0, 0)]
        elif prev[0] != (w, h):
            changed = list(range(len(px)))
        else:
            changed = [i for i, (a, b) in enumerate(zip(px, prev[1]))
                       if a != b]
        if changed:
            xs = [i % w for i in changed]
            ys = [i // w for i in changed]
            need = (min(xs), min(ys), max(xs) + 1, max(ys) + 1)
            if box is None or need[0] < box[0] or need[1] < box[1] \
                    or need[2] > box[2] or need[3] > box[3]:
                bad('lcdpush: shot "%s" changed pixels %s but %s (drawn into '
                    'the framebuffer and not pushed to the panel; '
                    'scripts/check_trace.py check_lcdpush)'
                    % (name, need,
                       'no panel push since the previous shot'
                       if box is None else
                       'the panel pushes since the previous shot cover only %s'
                       % (box,)))
                return
            n_ok += 1
        prev, box = ((w, h), px), None
    ok('lcdpush (%d shots with changes covered by panel pushes%s)'
       % (n_ok, ', %d without PNG skipped' % skipped if skipped else ''))


STARVED_BUSY = 0.5      # process CPU / wall below this: the host held us up


def frame_busy(lines):
    """Share of the animation frames' wall time the whole simulator process
    spent on a CPU (anim frame <ms> cpu=<us> pcpu=<us>), or None without
    figures. Low: the host took the CPU away (starved). High: our own
    threads kept the UI thread waiting."""
    wall = busy = 0
    for l in lines:
        m = RE_FRAME_PCPU.search(l)
        if m and int(m.group(1)) > 0:
            wall += int(m.group(1)) * 1000
            busy += int(m.group(2))
    return busy / wall if wall else None


def frame_intervals(segments):
    """Each segment is the list of "anim frame" values between a start and
    its end. Values are intervals, unless a segment is a non-decreasing run
    starting above 200 (a timestamp in ms), which is differenced."""
    out = []
    for seg in segments:
        if len(seg) >= 2 and seg[0] > 200 and \
                all(b >= a for a, b in zip(seg, seg[1:])):
            out += [b - a for a, b in zip(seg, seg[1:])]
        else:
            out += seg[1:]          # the first frame has no predecessor
    return out


# ---- Phase 15 (docs/phase15-brief.md 2.4): DAC, hardware EQ, volume, fades
RE_P15_EQ = re.compile(r'\bipodui: eq engine=(\w+) preset=(-?\d+) '
                       r'maxerr=(-?\d+) precut=(-?\d+)')
RE_P15_EQSET = re.compile(r'\bipodui: eq preset=(-?\d+) enabled=(\d+) '
                          r'precut=(\d+)')
RE_P15_DAC = re.compile(r'\bipodui: dac standby=(-?\d+) sleeps=(\d+) '
                        r'wakes=(\d+) wake_us=(\d+)')
RE_P15_VOL = re.compile(r'\bipodui: vol step=(-?\d+) db=(-?\d+) '
                        r'bar=(-?\d+)')
RE_P15_FADE = re.compile(r'\bipodui: fade dir=(\w+) steps=(-?\d+)')
RE_P15_SC = re.compile(r'\bipodui: sc engine=(\w+) gain_db10=(-?\d+) '
                       r'track=(-?\d+)')
P15_ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
P15_HW_MAXERR = 255         # apps/ipodui/eq_hw.h EQ_HW_MAXERR (all on hw, owner)
P15_VOL_STEPS = 48          # apps/ipodui/volcurve.h VOL_STEPS


def p15_eq_table():
    """(precut, maxerr) per preset from apps/ipodui/eq_hw_presets.c, in
    preset order (index n-1 for preset n)."""
    path = os.path.join(P15_ROOT, 'apps/ipodui/eq_hw_presets.c')
    rows = []
    for l in open(path):
        m = re.match(r'\s*\{ \{ .*\}, (\d+), (\d+) \},\s*$', l)
        if m:
            rows.append((int(m.group(1)), int(m.group(2))))
    return rows


def p15_vol_table():
    """The dB of each volume step from apps/ipodui/volcurve.c (step 0 is
    VOL_DB_MUTE, -90)."""
    path = os.path.join(P15_ROOT, 'apps/ipodui/volcurve.c')
    text = open(path).read()
    m = re.search(r'vol_db\[VOL_STEPS\] = \{(.*?)\};', text, re.S)
    body = re.sub(r'/\*.*?\*/', '', m.group(1), flags=re.S) if m else ''
    return [-90 if v == 'VOL_DB_MUTE' else int(v)
            for v in re.findall(r'VOL_DB_MUTE|-?\d+', body)]


def check_npstall(lines, script_lines, ok, bad):
    """npfix: the Now Playing clock skips (tests/ui/ipodui-np-stall.script).

    Every log:
      artslice  no `art slice ms=N lit=1` line: the art builder never ran
                over 100 ms between two decoder yield points while audio
                played and the user was active (artcache.c art_gate())
      uiio      no `sim io ui pwr=` line: no UI-thread file call while a
                script's `stor_watch_ui on` window is open
      shufchunk every `shuf: chunk +n` has n <= 5 (a UI-pass refill chunk)
    Selected by `# trace: npstall` (FAILS without `np secdt` lines):
      npjump    no `np jump sec=` line
      npsec     consecutive `np secdt <s>` lines step by exactly +1; a
                track change (`np snap`), a full paint (`np draw`), an
                unlit exit or a seek starts a new run
    """
    want = any(re.match(r'#\s*trace:.*\bnpstall\b', l) for l in script_lines)
    slices = [l for l in lines if re.search(r'\bart slice ms=\d+ lit=1', l)]
    if slices:
        bad('artslice: %d builder slice(s) over 100 ms while lit and '
            'playing, first: %s' % (len(slices), slices[0].strip()))
    elif want or any('art park ms=' in l for l in lines):
        ok('artslice: no builder slice over 100 ms while lit and playing')
    uiio = [l for l in lines if re.search(r'\bsim io ui pwr=', l)]
    if uiio:
        bad('uiio: %d UI-thread file call(s) while watched (%d with the '
            'drive off)' % (len(uiio), sum('pwr=0' in l for l in uiio)))
    elif want:
        ok('uiio: no UI-thread file call while watched')
    chunks = [int(m.group(1)) for l in lines
              for m in [re.search(r'\bshuf: chunk \+(\d+)', l)] if m]
    big = [n for n in chunks if n > 5]
    if big:
        bad('shufchunk: a refill chunk of %d entries (> 5)' % big[0])
    elif chunks:
        ok('shufchunk: %d refill chunk(s), each <= 5 entries' % len(chunks))
    if not want:
        return
    jumps = [l for l in lines if re.search(r'\bnp jump sec=', l)]
    if jumps:
        bad('npjump: %d jump(s), first: %s' % (len(jumps), jumps[0].strip()))
    else:
        ok('npjump: no np jump')
    prev, steps, gaps = None, 0, []
    for l in lines:
        if re.search(r'\bnp (snap|draw) |\bunlit exit\b|\bnp seek\b', l):
            prev = None
            continue
        m = re.search(r'\bnp secdt (\d+) ', l)
        if not m:
            continue
        sec = int(m.group(1))
        if prev is not None:
            steps += 1
            if sec != prev + 1:
                gaps.append((prev, sec))
        prev = sec
    if not steps:
        bad('npsec: no consecutive "np secdt" lines (script declares '
            '"# trace: npstall")')
    elif gaps:
        bad('npsec: %d step(s) other than +1, first %d->%d'
            % (len(gaps), gaps[0][0], gaps[0][1]))
    else:
        ok('npsec: %d steps, each +1' % steps)


def check_p15(lines, script, script_lines, ok, bad):
    """Phase 15 rules. A script selects a rule with `# trace: p15eq p15dac
    p15vol p15fade` (or by name: ipodui-p15-eq*, -standby*, -volume*,
    -fade*); a selected rule that finds none of its lines FAILS. With the
    lines present, a rule always runs.

      p15eq    `eq engine=hw|sw preset=<n> maxerr= precut=`: n in 0..22;
               Off (0) is sw with maxerr=0 precut=0; maxerr and precut
               equal the generated table (apps/ipodui/eq_hw_presets.c) for
               n >= 1; engine is hw iff maxerr <= 15 (EQ_HW_MAXERR; the
               sim has IPODUI_HW_EQ=1); the `eq preset=<n> enabled=
               precut=` line before it shows exactly one engine: hw ->
               enabled=0 precut=0 (software EQ flat), sw preset ->
               enabled=1, Off -> enabled=0.
               `# eq-engines: <n>=hw|sw ...` pins presets to an engine.
      p15dac   `dac standby=<0|1> sleeps= wakes= wake_us=`: standby is 0
               or 1; sleeps and wakes never decrease within a boot (a
               `boot screen` line starts a new count); the line is logged
               on change, so each differs from the previous in a count;
               wakes <= sleeps + 1; standby=1 needs sleeps >= 1.
               `# dac: sleeps>=N wakes>=N` bounds the final counts.
      p15vol   `vol step= db= bar=`: step 0..47; bar == step*1024/47 (0 at
               step 0, 1024 at 47); db equals apps/ipodui/volcurve.c's
               table. `# vol-max: N`: no step above N and N is reached.
               `# vol-walk: N`: neighbouring lines differ by <= N steps
               (one wheel click is one step, never a jump).
      p15fade  `ipodui: fade dir=in|out steps=<n>`: n in 0..25.
               `# fade: reverse` needs a line of the opposite direction to
               its predecessor with 0 < steps < 25 (a turn-around
               mid-fade, not a restart)
      p15sc    `sc engine=hw gain_db10=<n> track=<k>` (hardware Sound
               Check, wave 2b): engine is hw; -480 <= n <= 290 (ReplayGain
               -48..+17 dB plus a 12 dB preamp); each line differs from
               the previous in n or k (logged on change; a `boot screen`
               line starts afresh). `# sc-seq: k:n
               ...` pins the exact sequence of (track, gain) pairs.
    """
    base = os.path.basename(script)
    declared = set()
    for l in script_lines:
        m = re.match(r'#\s*trace:\s*(.*)$', l)
        if m:
            declared.update(m.group(1).split())
    for pre, kind in (('ipodui-p15-eq', 'p15eq'),
                      ('ipodui-p15-standby', 'p15dac'),
                      ('ipodui-p15-volume', 'p15vol'),
                      ('ipodui-p15-fade', 'p15fade'),
                      ('ipodui-p15-soundcheck', 'p15sc')):
        if base.startswith(pre):
            declared.add(kind)

    def hdr(name):
        for l in script_lines:
            m = re.match(r'#\s*%s:\s*(.*)$' % name, l)
            if m:
                return m.group(1).strip()
        return None

    def verdict(rule, errs, good):
        if errs:
            bad('%s: %s%s' % (rule, errs[0], ' (+%d more)' % (len(errs) - 1)
                              if len(errs) > 1 else ''))
        else:
            ok('%s (%s)' % (rule, good))

    # ---- eq
    engines = [(i, RE_P15_EQ.search(l)) for i, l in enumerate(lines)]
    engines = [(i, m) for i, m in engines if m]
    if 'p15eq' in declared and not engines:
        bad('p15eq: %s declares the eq rule but the log has no "eq engine= '
            'preset= maxerr= precut=" line' % base)
    elif engines:
        table = p15_eq_table()
        pins = {}
        pin = hdr('eq-engines')
        if pin:
            for kv in pin.split():
                n, _, e = kv.partition('=')
                pins[int(n)] = e
        seen, errs = {}, []
        for i, m in engines:
            eng, n, maxerr, precut = (m.group(1), int(m.group(2)),
                                      int(m.group(3)), int(m.group(4)))
            seen[n] = eng
            where = 'line %d "%s"' % (i + 1, m.group(0).strip())
            if eng not in ('hw', 'sw'):
                errs.append('%s: engine %s is not hw|sw' % (where, eng))
                continue
            if not 0 <= n <= len(table):
                errs.append('%s: preset %d outside 0..%d'
                            % (where, n, len(table)))
                continue
            if n == 0:
                if (eng, maxerr, precut) != ('sw', 0, 0):
                    errs.append('%s: Off must be sw maxerr=0 precut=0' % where)
            else:
                tp, te = table[n - 1]
                if maxerr != te:
                    errs.append('%s: maxerr %d, table says %d'
                                % (where, maxerr, te))
                if (eng == 'hw') != (te <= P15_HW_MAXERR):
                    errs.append('%s: engine %s but table maxerr %d '
                                '(hw iff <= %d)'
                                % (where, eng, te, P15_HW_MAXERR))
                if eng == 'hw' and precut != tp:
                    errs.append('%s: hw precut %d, table says %d'
                                % (where, precut, tp))
                if not 0 <= precut <= 12:
                    errs.append('%s: precut %d outside 0..12' % (where, precut))
            # the settings line just before: exactly one engine on
            prev = None
            for j in range(i - 1, max(i - 6, -1), -1):
                pm = RE_P15_EQSET.search(lines[j])
                if pm:
                    prev = pm
                    break
            if prev is not None:
                pn, en, pc = (int(prev.group(1)), int(prev.group(2)),
                              int(prev.group(3)))
                if pn != n:
                    errs.append('%s: preceded by "eq preset=%d"' % (where, pn))
                elif eng == 'hw' and (en, pc) != (0, 0):
                    errs.append('%s: hw engine but software EQ on '
                                '(enabled=%d precut=%d): two engines'
                                % (where, en, pc))
                elif eng == 'sw' and n > 0 and en != 1:
                    errs.append('%s: sw engine but software EQ off '
                                '(enabled=%d): no engine' % (where, en))
                elif n == 0 and en != 0:
                    errs.append('%s: Off but software EQ enabled' % where)
        for n, e in pins.items():
            if n not in seen:
                errs.append('# eq-engines: preset %d never logged' % n)
            elif seen[n] != e:
                errs.append('# eq-engines: preset %d logged %s, want %s'
                            % (n, seen[n], e))
        verdict('p15eq', errs, '%d engine lines, presets %s'
                % (len(engines), sorted(seen)))

    # ---- dac
    dacs = [(i, RE_P15_DAC.search(l)) for i, l in enumerate(lines)]
    dacs = [(i, m) for i, m in dacs if m]
    if 'p15dac' in declared and not dacs:
        bad('p15dac: %s declares the dac rule but the log has no "dac '
            'standby= sleeps= wakes= wake_us=" line' % base)
    elif dacs:
        errs, last, prev_i = [], (0, 0), -1
        boots = [j for j, x in enumerate(lines) if 'ipodui: boot screen' in x]
        for i, m in dacs:
            sb, sl, wk, us = (int(m.group(k)) for k in (1, 2, 3, 4))
            if any(prev_i < j < i for j in boots):
                last = (0, 0)       # a new boot (reboot, power cut): counts restart
            prev_i = i
            where = 'line %d "%s"' % (i + 1, m.group(0).strip())
            if sb not in (0, 1):
                errs.append('%s: standby %d not 0|1' % (where, sb))
            if sl < last[0] or wk < last[1]:
                errs.append('%s: counts went down (was sleeps=%d wakes=%d)'
                            % (where, last[0], last[1]))
            if (sl, wk) == last:
                errs.append('%s: logged without a count changing' % where)
            if wk > sl + 1:
                errs.append('%s: wakes %d > sleeps %d + 1' % (where, wk, sl))
            if sb == 1 and sl < 1:
                errs.append('%s: standby=1 with no sleep counted' % where)
            last = (sl, wk)
        b = hdr('dac')
        if b:
            for kind, n in re.findall(r'(sleeps|wakes)>=(\d+)', b):
                have = last[0] if kind == 'sleeps' else last[1]
                if have < int(n):
                    errs.append('# dac: final %s=%d < %s' % (kind, have, n))
        verdict('p15dac', errs, '%d lines, final sleeps=%d wakes=%d'
                % (len(dacs), last[0], last[1]))

    # ---- vol
    vols = [(i, RE_P15_VOL.search(l)) for i, l in enumerate(lines)]
    vols = [(i, m) for i, m in vols if m]
    if 'p15vol' in declared and not vols:
        bad('p15vol: %s declares the vol rule but the log has no "vol step= '
            'db= bar=" line' % base)
    elif vols:
        table = p15_vol_table()
        errs, top = [], -1
        if len(table) != P15_VOL_STEPS:
            errs.append('could not read %d dB values from volcurve.c (got %d)'
                        % (P15_VOL_STEPS, len(table)))
        for i, m in vols:
            st, db, bar = (int(m.group(k)) for k in (1, 2, 3))
            top = max(top, st)
            where = 'line %d "%s"' % (i + 1, m.group(0).strip())
            if not 0 <= st < P15_VOL_STEPS:
                errs.append('%s: step outside 0..%d'
                            % (where, P15_VOL_STEPS - 1))
                continue
            want = 0 if st == 0 else st * 1024 // (P15_VOL_STEPS - 1)
            if bar != want:
                errs.append('%s: bar %d, want step*1024/47 = %d'
                            % (where, bar, want))
            if len(table) == P15_VOL_STEPS and db != table[st]:
                errs.append('%s: db %d, volcurve.c says %d at step %d'
                            % (where, db, table[st], st))
        vm = hdr('vol-max')
        if vm and vm.isdigit():
            if top > int(vm):
                errs.append('# vol-max: step %d reached, cap is %s'
                            % (top, vm))
            elif top < int(vm):
                errs.append('# vol-max: top step %d < %s (cap never reached)'
                            % (top, vm))
        vw = hdr('vol-walk')
        if vw and vw.isdigit():
            steps = [int(m.group(1)) for _, m in vols]
            for k in range(1, len(steps)):
                if abs(steps[k] - steps[k - 1]) > int(vw):
                    errs.append('# vol-walk: line %d step %d follows %d, '
                                'more than %s apart'
                                % (vols[k][0] + 1, steps[k], steps[k - 1], vw))
                    break
        verdict('p15vol', errs, '%d lines, top step %d' % (len(vols), top))

    # ---- fade
    fades = [(i, RE_P15_FADE.search(l)) for i, l in enumerate(lines)]
    fades = [(i, m) for i, m in fades if m]
    if 'p15fade' in declared and not fades:
        bad('p15fade: %s declares the fade rule but the log has no "ipodui: '
            'fade dir= steps=" line' % base)
    elif fades:
        errs, prev, turned = [], None, False
        for i, m in fades:
            d, n = m.group(1), int(m.group(2))
            where = 'line %d "%s"' % (i + 1, m.group(0).strip())
            if d not in ('in', 'out'):
                errs.append('%s: dir %s not in|out' % (where, d))
            if not 0 <= n <= 25:
                errs.append('%s: steps %d outside 0..25' % (where, n))
            if prev and prev != d and 0 < n < 25:
                turned = True
            prev = d
        if hdr('fade') == 'reverse' and not turned:
            errs.append('# fade: reverse: no opposite-direction line with '
                        '0 < steps < 25 (a restart, or no overlap)')
        verdict('p15fade', errs, '%d lines%s' % (
            len(fades), ', reversal seen' if turned else ''))

    # ---- hardware Sound Check
    scs = [(i, RE_P15_SC.search(l)) for i, l in enumerate(lines)]
    scs = [(i, m) for i, m in scs if m]
    if 'p15sc' in declared and not scs:
        bad('p15sc: %s declares the sc rule but the log has no "ipodui: sc '
            'engine= gain_db10= track=" line' % base)
    elif scs:
        errs, prev, seq = [], None, []
        boots = [j for j, x in enumerate(lines) if 'ipodui: boot screen' in x]
        last = -1
        for i, m in scs:
            e, n, k = m.group(1), int(m.group(2)), int(m.group(3))
            if any(last < j < i for j in boots):
                prev = None             # a new boot logs afresh
            last = i
            where = 'line %d "%s"' % (i + 1, m.group(0).strip())
            if e != 'hw':
                errs.append('%s: engine %s, not hw' % (where, e))
            if not -480 <= n <= 290:
                errs.append('%s: gain_db10 %d outside -480..290' % (where, n))
            if prev == (k, n):
                errs.append('%s: repeats the previous line (logged on '
                            'change only)' % where)
            prev = (k, n)
            seq.append('%d:%d' % (k, n))
        want = hdr('sc-seq')
        if want is not None and want.split() != seq:
            errs.append('# sc-seq: want "%s", log has "%s"'
                        % (want, ' '.join(seq)))
        verdict('p15sc', errs, '%d lines: %s' % (len(scs), ' '.join(seq)))


# ---- Phase 12 (docs/phase12-brief.md 9, "Trace rules"; owner W1-T): Extras
RE_P12_MODCLASS = re.compile(r'\bmodclass (?!viol\b)(\S+) class=(\w) '
                             r'reads=(\d+) writes=(\d+) allocs=(\d+) '
                             r'boost_pct=(\d+) fps=(\d+)(?: pages=(\d+))?')
RE_P12_VIOL = re.compile(r'\bmodclass viol (\S+)\s*(.*)$')
RE_P12_RING = re.compile(r'\balarm ring i=(-?\d+)')
RE_P12_RING_END = re.compile(r'\balarm (?:snooze|dismiss|missed|stop)\b')
RE_P12_ARM = re.compile(r'\balarm arm rtc=(\d+) ok=(\d+)')
RE_P12_PIEZO = re.compile(r'\bpiezo play hz=(\d+) ms=(\d+)')
RE_P12_XIDX = re.compile(r'\bxidx build kind=(\w+) n=(\d+) ms=(\d+)')
RE_P12_XIDX_TRIG = re.compile(r'sim_script: usb_disconnect|\bboot screen\b'
                              r'|\bmaint (?:take|job)\b|\bxidx (?:owed|window)'
                              r'\b|\bpost-usb\b')
RE_P12_JOBOPEN = re.compile(r'\bmod job_open stk=(\d+)')
# scripts that open an Extra: the modclass rule is armed by name
P12_MODCLASS_NAMES = ('extras', 'clocks', 'stopwatch', 'lock', 'notes',
                      'contacts', 'calendars', 'sdk', 'modclass', 'mod-swap')


def check_p12(lines, script, script_lines, ok, bad):
    """Phase 12 rules. A script arms a rule with `# trace: modclass alarm
    alarmarm piezo xidx jobopen` (some are armed by the script's name, see
    below); an armed rule that finds none of its lines FAILS. With the
    lines present, a rule always runs.

      modclass  `modclass <name> class=A reads= writes= allocs= boost_pct=
                fps=`: at least one line (armed by `# trace: modclass`, or
                by the name ipodui-p12-{extras,clocks,stopwatch,lock,notes,
                contacts,calendars,sdk,modclass,mod-swap}* or *-while-
                playing*; `# modclass: none <why>` opts out); and NO
                `modclass viol <name> <rule>` line, in any script.
                `# modclass: steady` also needs reads=0 on every line of a
                module after its first (the entry period). Paged lists
                (brief 2.1's declared exception): a module whose manifest
                says `pages = N` books its page loads apart (`pages=` on
                the line, not in reads=); the accountant itself logs
                `modclass viol <name> page` (a load of more than 2
                commands) or `pages` (more than N a second), which this
                rule fails like any viol. `# modclass: paged` also needs
                pages > 0 on some line (the script reaches past the cached
                head).
      alarm     `alarm ring i=`: the count equals `# alarm-rings: N`
                (N may be 0). A script named ipodui-p12-alarm-* must
                declare it; a ring in any other script needs it too.
      alarmarm  `alarm arm rtc= ok=`: every line has ok=1 (the retry's final
                line); `# alarm-arms: N` needs at least N lines. Armed by
                ipodui-p12-alarm-* names (>= 1 line) unless `# alarm-arm:
                none <why>`.
      piezo     `piezo play hz= ms=`: only between an `alarm ring` and the
                next `alarm snooze|dismiss|missed|stop`; a `boot screen`
                ends a ringing. `# trace: piezo` (or the name ipodui-p12-
                alarm-ring*) needs at least one line. `# piezo: free <why>`
                lifts the ringing condition (the tooling test drives the
                piezo by hand with piezo_test).
      xidx      `xidx build kind= n= ms=`: n and ms are sane, and a sync
                moment came first (`sim_script: usb_disconnect`, `boot
                screen`, `maint take|job`, `xidx owed|window` or `post-usb`
                earlier in the log), so a build never runs on a browse alone.
                `# xidx-builds: N` fixes the count (N may be 0).
      jobopen   `mod job_open stk=<n>` (target logs, D-M3): n > 0.
    """
    base = os.path.basename(script)
    declared = set()
    for l in script_lines:
        m = re.match(r'#\s*trace:\s*(.*)$', l)
        if m:
            declared.update(m.group(1).split())

    def hdr(name):
        for l in script_lines:
            m = re.match(r'#\s*%s:\s*(.*)$' % name, l)
            if m:
                return m.group(1).strip()
        return None

    def verdict(rule, errs, good):
        if errs:
            bad('%s: %s%s' % (rule, errs[0], ' (+%d more)' % (len(errs) - 1)
                              if len(errs) > 1 else ''))
        else:
            ok('%s (%s)' % (rule, good))

    p12 = base.startswith('ipodui-p12-')
    rest = base[len('ipodui-p12-'):] if p12 else ''
    mc_hdr = hdr('modclass') or ''
    if p12 and (any(rest.startswith(n) for n in P12_MODCLASS_NAMES)
                or '-while-playing' in rest) and not mc_hdr.startswith('none'):
        declared.add('modclass')
    if p12 and rest.startswith('alarm-'):
        declared.update(('alarm', 'alarmarm'))
        if (hdr('alarm-arm') or '').startswith('none'):
            declared.discard('alarmarm')
    if p12 and rest.startswith('alarm-ring'):
        declared.add('piezo')

    # ---- modclass
    mcs = [(i, RE_P12_MODCLASS.search(l)) for i, l in enumerate(lines)]
    mcs = [(i, m) for i, m in mcs if m]
    viols = [(i, RE_P12_VIOL.search(l)) for i, l in enumerate(lines)]
    viols = [(i, m) for i, m in viols if m]
    errs = ['line %d "modclass viol %s %s": the accountant caught a breach'
            % (i + 1, m.group(1), m.group(2)) for i, m in viols]
    if 'modclass' in declared and not mcs:
        errs.append('%s declares the modclass rule but the log has no '
                    '"modclass <name> class= reads= writes= allocs= '
                    'boost_pct= fps=" line (accountant not hooked, or no '
                    'Extra opened)' % base)
    if mc_hdr == 'steady':
        seen = {}
        for i, m in mcs:
            n = seen.get(m.group(1), 0)
            seen[m.group(1)] = n + 1
            if n >= 1 and int(m.group(3)) != 0:
                errs.append('line %d "modclass %s ... reads=%s": steady '
                            'state must have reads=0' % (i + 1, m.group(1),
                                                         m.group(3)))
    if mc_hdr == 'paged' and not any(int(m.group(8) or 0) for _, m in mcs):
        errs.append('# modclass: paged, but no "modclass ... pages=N" line '
                    'has N > 0 (no page load past the entry read)')
    if mcs or viols or 'modclass' in declared:
        verdict('p12modclass', errs, '%d lines, %d module(s), %d page '
                'load(s), no viol' % (
                    len(mcs), len({m.group(1) for _, m in mcs}),
                    max([int(m.group(8) or 0) for _, m in mcs] or [0])))

    # ---- alarm rings, arms and the piezo
    rings = [i for i, l in enumerate(lines) if RE_P12_RING.search(l)]
    want = hdr('alarm-rings')
    if 'alarm' in declared and want is None:
        bad('p12alarm: %s must declare "# alarm-rings: N" (N = scripted '
            'rings, may be 0); the log has %d "alarm ring" line(s)'
            % (base, len(rings)))
    elif want is None and rings:
        bad('p12alarm: line %d "%s": a ring in a script with no "# alarm-'
            'rings: N" declaration' % (rings[0] + 1, lines[rings[0]].strip()))
    elif want is not None:
        try:
            n = int(want.split()[0])
        except ValueError:
            n = -1
        if n != len(rings):
            bad('p12alarm: "alarm ring" appears %d time(s), "# alarm-rings: '
                '%s" says %s' % (len(rings), want, want.split()[0]))
        else:
            ok('p12alarm (%d ring(s))' % n)

    arms = [(i, RE_P12_ARM.search(l)) for i, l in enumerate(lines)]
    arms = [(i, m) for i, m in arms if m]
    errs = ['line %d "%s": ok=%s (the PMU read-back did not match)'
            % (i + 1, m.group(0), m.group(2)) for i, m in arms
            if m.group(2) != '1']
    need = hdr('alarm-arms')
    need = int(need.split()[0]) if need and need.split()[0].isdigit() else (
        1 if 'alarmarm' in declared else 0)
    if len(arms) < need:
        errs.append('%d "alarm arm rtc= ok=" line(s), need at least %d'
                    % (len(arms), need))
    if arms or need:
        verdict('p12alarmarm', errs, '%d arm line(s)' % len(arms))

    ringing, errs, plays = False, [], 0
    for i, l in enumerate(lines):
        if RE_P12_RING.search(l):
            ringing = True
        elif RE_P12_RING_END.search(l) or 'boot screen' in l:
            ringing = False
        m = RE_P12_PIEZO.search(l)
        if m:
            plays += 1
            if not ringing and not (hdr('piezo') or '').startswith('free'):
                errs.append('line %d "%s": a piezo play while no alarm is '
                            'ringing' % (i + 1, m.group(0)))
    if 'piezo' in declared and not plays:
        bad('p12piezo: %s declares the piezo rule but the log has no '
            '"piezo play hz= ms=" line' % base)
    elif plays:
        verdict('p12piezo', errs, '%d play line(s)' % plays)

    # ---- xidx
    xs = [(i, RE_P12_XIDX.search(l)) for i, l in enumerate(lines)]
    xs = [(i, m) for i, m in xs if m]
    xwant = hdr('xidx-builds')
    if 'xidx' in declared and not xs and xwant != '0':
        bad('p12xidx: %s declares the xidx rule but the log has no "xidx '
            'build kind= n= ms=" line' % base)
    elif xs or xwant is not None:
        errs = []
        for i, m in xs:
            where = 'line %d "%s"' % (i + 1, m.group(0))
            if not any(RE_P12_XIDX_TRIG.search(x) for x in lines[:i]):
                errs.append('%s: no sync moment (usb_disconnect, boot, '
                            'maint take, xidx owed) before it: built while '
                            'browsing' % where)
            if int(m.group(3)) > 60000:
                errs.append('%s: ms %s over 60000' % (where, m.group(3)))
        if xwant is not None and xwant.split()[0].isdigit() \
                and int(xwant.split()[0]) != len(xs):
            errs.append('"xidx build" appears %d time(s), "# xidx-builds: %s"'
                        ' says %s' % (len(xs), xwant, xwant.split()[0]))
        verdict('p12xidx', errs, '%d build(s)' % len(xs))

    # ---- job_open (target logs)
    jo = [(i, RE_P12_JOBOPEN.search(l)) for i, l in enumerate(lines)]
    jo = [(i, m) for i, m in jo if m]
    if 'jobopen' in declared and not jo:
        bad('p12jobopen: %s declares the jobopen rule but the log has no '
            '"mod job_open stk=" line' % base)
    elif jo:
        errs = ['line %d "%s": stk must be > 0' % (i + 1, m.group(0))
                for i, m in jo if int(m.group(1)) <= 0]
        verdict('p12jobopen', errs, '%d line(s)' % len(jo))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('log')
    ap.add_argument('script')
    ap.add_argument('--rows', default='9,5,4',
                    help='rows per view of each list style')
    ap.add_argument('--header-h', type=int, default=20)
    ap.add_argument('--lcd-h', type=int, default=240)
    ap.add_argument('--max-frame-ms', type=float, default=50.0)
    ap.add_argument('--max-frame-cpu-us', type=float, default=8000.0,
                    help='UI-thread CPU per frame (anim frame ... cpu=); '
                         'provisional, calibrate from passing runs')
    ap.add_argument('--starved-jitter-ms', type=int, default=20,
                    help='host jitter p95 (sim_load) that marks a stalled host')
    ap.add_argument('--max-open-ms', type=int, default=300)
    ap.add_argument('--sim-factor', type=float, default=SIM_FACTOR)
    ap.add_argument('--visible-rows', type=int, default=9)
    ap.add_argument('--manifest', default=DEFAULT_MANIFEST)
    a = ap.parse_args()
    body = a.lcd_h - a.header_h
    styles = sorted({int(n) for n in a.rows.split(',') if n.strip()})
    # row boundaries (px from the body top) of each list style
    bounds = [{body * i // n for i in range(n + 1)} for n in styles]
    row_hs = {body * (i + 1) // n - body * i // n
              for n in styles for i in range(n)}
    tallest = max(row_hs)

    lines = open(a.log, errors='replace').read().splitlines()
    presses = sum(1 for l in open(a.script) if RE_PRESS.match(l))
    fail = 0

    def ok(msg):
        print('ok   ' + msg)

    def bad(msg):
        nonlocal fail
        print('FAIL: ' + msg)
        fail = 1

    # band
    bands = [(int(m.group(1)), int(m.group(2)))
             for l in lines for m in [RE_BAND.search(l)] if m]
    shifts = [(y, h) for y, h in bands if h > 2 * tallest]
    badb = [(y, h) for y, h in shifts
            if not any(y - a.header_h in b and y + h - a.header_h in b
                       for b in bounds)]
    if badb:
        bad('update band y=%d h=%d: more than %d px and not a list shift '
            '(rows of a %s-row list)'
            % (badb[0][0], badb[0][1], 2 * tallest, a.rows))
    else:
        ok('band updates (%d, %d list shifts, max %dpx)'
           % (len(bands), len(shifts), max((h for _, h in bands), default=0)))

    # full
    fulls = sum(1 for l in lines if RE_FULL.search(l))
    # every boot ("reboot" in the script: shots.sh runs the rest in a new
    # sim) starts with one full frame, and so does the screen after a USB
    # session ("usb_disconnect")
    boots = 1 + sum(1 for l in open(a.script)
                    if re.match(r'\s*(reboot|usb_disconnect)\b', l))
    # "# fulls-extra: N <why>": N more full updates the script's own
    # events cause (the main menu redraws in full when its Now Playing row
    # appears or goes away without a key press)
    extra = sum(int(m.group(1)) for l in open(a.script)
                for m in [re.match(r'#\s*fulls-extra:\s*(\d+)', l)] if m)
    # every dismissed unlit face (`unlit exit key|holdoff|usb|alarm`; a wheel
    # dismiss logs `key`) redraws the screen under it in full once
    unlit_exits = sum(1 for l in lines if re.search(r'\bunlit exit \w', l))
    allowed = boots + presses + extra + unlit_exits
    if fulls > allowed:
        bad('%d full updates > %d allowed (%d boot(s) + %d SELECT/MENU + '
            '%d unlit exit(s) + %d declared)'
            % (fulls, allowed, boots, presses, unlit_exits, extra))
    else:
        ok('full updates (%d <= %d)' % (fulls, allowed))

    script_lines = list(open(a.script))
    required = any(re.match(r'#\s*trace:\s*lib\b', l) for l in script_lines)
    check_library(lines, a, ok, bad, required)
    check_art(lines, ok, bad,
              any(re.match(r'#\s*trace:.*\bart\b', l) for l in script_lines))
    check_nav(lines, script_lines, ok, bad)
    check_np(lines, script_lines, ok, bad)
    check_p8a(lines, a.script, script_lines, ok, bad)
    check_ab(lines, script_lines, ok, bad)
    check_p11(lines, a.script, script_lines, ok, bad)
    check_p15(lines, a.script, script_lines, ok, bad)
    check_p12(lines, a.script, script_lines, ok, bad)
    check_npstall(lines, script_lines, ok, bad)
    check_driftrepeat(lines, a.script, script_lines, ok, bad)
    check_panelset(lines, script_lines, ok, bad)
    check_panelhold(lines, script_lines, ok, bad)
    check_p8b(lines, a.script, script_lines, ok, bad)
    check_lcdpush(lines, a.log, ok, bad)
    check_illusion(lines, script_lines, ok, bad)
    check_busyglyph(lines, script_lines, ok, bad)

    # animation trace
    if not any(RE_START.search(l) or RE_FRAME.search(l) or RE_END.search(l)
               or RE_ANIM_UPD.search(l) for l in lines):
        print('note anim checks skipped: no anim trace in log (Phase 2 build, or no strip)')
        return fail

    starts = ends = depth = 0
    min_depth = 0
    last_end_boost = None
    segments, cur = [], None
    anims = []
    frame_cpu = []          # UI-thread CPU us per frame (anim frame <ms> cpu=<us>)
    for l in lines:
        m = RE_START.search(l)
        if m:
            starts += 1
            depth += 1
            if cur is None:
                cur = []
            continue
        m = RE_END.search(l)
        if m:
            ends += 1
            depth -= 1
            min_depth = min(min_depth, depth)
            last_end_boost = int(m.group(1))
            if depth <= 0 and cur is not None:
                segments.append(cur)
                cur = None
            continue
        m = RE_FRAME.search(l)
        if m:
            mc = RE_FRAME_CPU.search(l)
            if cur is None:          # frame outside start/end: own segment
                segments.append([int(m.group(1))])
            else:
                if mc and cur:       # (the first frame of a segment is left out)
                    frame_cpu.append(int(mc.group(1)))
                cur.append(int(m.group(1)))
            continue
        m = RE_ANIM_UPD.search(l)
        if m:
            anims.append((m.group(1) or '', int(m.group(2)),
                          int(m.group(3))))
    if cur is not None:
        segments.append(cur)

    # boost
    if starts != ends:
        bad('anim start/end unbalanced: %d starts, %d ends' % (starts, ends))
    elif min_depth < 0:
        bad('anim end without a matching start')
    elif ends and last_end_boost != 0:
        bad('last anim end has boost=%d, want 0' % last_end_boost)
    else:
        ok('boost balanced (%d animations, last boost=%s)'
           % (starts, last_end_boost))

    # frame
    # Frame cost versus host load (docs/gate-speed-brief.md section 4). The
    # wall interval is a tick difference and the simulator's ticks follow the
    # wall clock, so a stalled host stretches it. With a CPU figure per frame
    # (the shell's sim-only trace) a late frame that cost little CPU is the
    # host's doing: it is reported as starved, not failed; frames that cost
    # too much CPU fail whatever the load. The jitter watcher's line
    # (sim_load: run jitter ...) marks a run on a stalled host in the failure
    # text so the gate's ledger can class it.
    jit = None
    for l in lines:
        m = RE_LOAD.search(l)
        if m:
            jit = tuple(int(x) for x in m.groups())     # mean p95 p99 max n
    iv = frame_intervals(segments)
    if iv:
        mean = sum(iv) / len(iv)
        cpu_mean = sum(frame_cpu) / len(frame_cpu) if frame_cpu else None
        busy = frame_busy(lines)
        starved_host = jit is not None and jit[1] >= a.starved_jitter_ms
        if cpu_mean is not None and cpu_mean > a.max_frame_cpu_us:
            bad('mean frame cpu %.0f us > %g us (%d frames, wall mean %.1f ms)'
                % (cpu_mean, a.max_frame_cpu_us, len(frame_cpu), mean))
        elif mean > a.max_frame_ms and cpu_mean is not None \
                and cpu_mean <= a.max_frame_cpu_us / 2 \
                and (busy is None or busy < STARVED_BUSY):
            ok('frame interval mean %.1f ms > %g ms but frames cost %.0f us cpu%s: '
               'starved by the host%s (%d frames)'
               % (mean, a.max_frame_ms, cpu_mean,
                  ', process %d%% busy' % round(100 * busy) if busy is not None else '',
                  ' (jitter p95 %d ms)' % jit[1] if jit else '', len(iv)))
        elif mean > a.max_frame_ms:
            bad('mean frame interval %.1f ms > %g ms (%d frames)%s%s'
                % (mean, a.max_frame_ms, len(iv),
                   ' [starved: host jitter p95 %d ms]' % jit[1] if starved_host
                   else '',
                   ' [process %d%% busy: our own threads delayed the UI]'
                   % round(100 * busy) if busy is not None and busy >= STARVED_BUSY
                   else ''))
        else:
            ok('frame interval mean %.1f ms, max %d ms (%d frames%s)'
               % (mean, max(iv), len(iv),
                  ', cpu %.0f us/frame' % cpu_mean if cpu_mean is not None else ''))
    else:
        print('note no anim frame lines')

    # anim bands
    limit = {'scroll': body, 'slide': body}
    # npviz: a slide to the headerless Visualizer covers the panel
    viz_full = any(re.match(r'#\s*np:\s*viz-full\b', l) for l in script_lines)
    badb = [(k, y, h) for k, y, h in anims
            if not (viz_full and k == 'slide' and y == 0 and h == a.lcd_h)
            and (y < a.header_h or y + h > a.lcd_h or h > limit.get(k, body))]
    if badb:
        k, y, h = badb[0]
        bad('%d update anim bands outside the body [%d,%d) or taller than '
            'their limit (scroll %d, slide %d), first anim:%s y=%d h=%d'
            % (len(badb), a.header_h, a.lcd_h, body, body, k, y, h))
    else:
        def mx(kind):
            return max((h for k, _, h in anims if k == kind), default=0)
        ok('anim updates (%d: scroll max h %dpx <= %d, slide max h %dpx '
           '<= %d)' % (len(anims), mx('scroll'), body, mx('slide'), body))

    # marquee
    if 'marquee' in os.path.basename(a.script):
        idx = [i for i, l in enumerate(lines) if RE_MARQUEE.search(l)]
        tall = [i for i in idx
                if int(RE_MARQUEE.search(lines[i]).group(2)) not in row_hs]
        if not idx:
            bad('no "update marquee" line')
        elif tall:
            bad('%d marquee updates not one row tall (%s px), first: %s'
                % (len(tall), sorted(row_hs), lines[tall[0]].strip()))
        else:
            ends = [i for i in range(idx[0]) if RE_END.search(lines[i])]
            first = ends[-1] + 1 if ends else 0
            boosts = [i for i in range(first, idx[-1] + 1)
                      if RE_START.search(lines[i])]
            if boosts:
                bad('anim start during the marquee (line %d of the log)'
                    % (boosts[0] + 1))
            else:
                ok('marquee (%d row updates, one row each, no boost)'
                   % len(idx))
    return fail


if __name__ == '__main__':
    sys.exit(main())
