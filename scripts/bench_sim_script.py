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

"""Writes the bench session's simulator scripts (tests/ui/ipodui-p19-bench-*.script).

usage: scripts/bench_sim_script.py          (writes all six)

Sitting 4gh's run (4g's, then the core layout probe: p2 and p1 after s6, duty last; 4g: 4f's less vizsoak and, 19-9 retired, the second-core group; b13 is the host flush's A/B): the held picks (devbench.h
BENCH_R_HELD) have no lines here; a pick brought back needs its lines again
(git history: sitting 4f for vizsoak; 4c for the rest).
vlong plays the 12 s test episode on the video screen for about 8 s (the
device's: Sintel, 10 min), judged against the sim's scaled minimums (devbench_sim_calls.h
VLONG_MIN_S).

The scripts drive Developer > Bench session > Re-verify the video image
through the real menus (apps/ipodui/devbench_sim.h): every screen of the
runner is a list whose go line, or whose answers, are its last rows, so a
step is "spin to the end, step back to the answer, SELECT".  The order of
the screens here is the runner's (devbench.h bench_reverify_picks and the
table's flags in devbench.c): a step added to the run, or a screen added to
a step, needs a line here, and the script then fails where it desyncs
(bench_sim logs every screen and every SELECT that missed).
"""
import os
import sys

HEADS = {'normal': '# The whole bench session in the simulator: Developer > Bench session >\n# Re-verify the video image, end to end, through the real menus.  The\n# runner, the step table and order, the results file, resume, the\n# auto-skips and the end summary are devbench.c\'s own; only the steps\'\n# device calls are the sim\'s stubs (apps/ipodui/devbench_sim.h says what is\n# modelled and what is real: the job model\'s self-test, the probe image\'s\n# block, the video player).  The restart (boot: the display driver)\n# is a "reboot" part; the steps done\n# outside the session are scripted (an album played, Now Playing opened,\n# a video played, the wheel turned), then Bench session > Go on.  Every\n# question gets its normal answer.  Sitting 4g\'s run: the held picks\n# (BENCH_R_HELD: b3, s4b, s4c, vfeed, s7, vbv, vgov, respanel, c1,\n# vizsoak, and with 19-9 retired clc, clf, ucop, uck1, uck2, ufeel,\n# upanel, ucusb, ucmod, ucop2, ucoff) never show; the long play runs outside (vlong:\n# the test episode on the video screen, its pictures in step with the\n# audio, the minimum scaled to the clip).  vplay\n# drops the unit\'s check result (a play before the session wrote one), so\n# its play from Videos runs the first-use check (SIM_VCHECK: the model\n# image counts as one with a clock token): its vck line, about 0.9 s, and\n# the verdict on vplay\'s result line.  The end: bench_sim expect checks\n# the results file: every pick once, with a result, in the picks\' order\n# (devbench.h bench_reverify_picks), the end line and the verified line;\n# no clock150 token (s4c is held; 23693ff4\'s token is already granted);\n# ucop is held, so the end clears the "cop" switch an earlier session\n# left (bench_sim set ui_split cop at the start): the daily driver starts\n# on the main processor.\n# Sitting 4gh: the core layout probe\'s rows that may freeze (p2, p1)\n# follow s6, and its dark row (duty) closes the run; the sim refuses all\n# three (devbench_sim.c cop_row), so they skip and ask nothing.  The VC\n# power rows (p19-vcpwr): s1, l5, ck before jvpu, vs after s6, sd last of\n# all; the sim refuses them too (devbench_vcpwr.c vcp_sim_row).  The\n# battery rows (p19-pwr-awake, p19-pwr-probes): pgrd, preg, awxm, prtc after\n# pre, pldo then awld after ck, pstl, pden, psrf after awi2 (pden and psrf\n# auto-skip on preg\'s sim skip); the cable steps\n# (awd2, awd2c, awus) after s6, awi2 after p1, awlg\'s own dark soak before\n# duty; the sim refuses them (devbench_awake.c, devbench_pwr.c stubs).\n# Core layout A (p19-corelayout-k1): pcop after p1, k1 its own dark soak\n# before duty; the sim has one core, so both say Not run (sim).\n# Generated by scripts/bench_sim_script.py (normal); edit that, not this.\n# needs: video\n# golden: skip\n# np: viz-full\n# reset: viz\n# log: vlong: PASS: pictures in step with the audio\n# log: vf0: PASS\n# log: ui switch cleared\n# log: (ucop held)\n# log: p2: Not run (sim)\n# log: duty: Not run (sim)\n# log: s1: Not run (sim)\n# log: sd: Not run (sim)\n# nolog: next start: ui cop\n# env: SIM_VCHECK=1\n# log: unit check: token dropped (bench reset)\n# log: unit check: due (no token)\n# logrange: 600 1500 vck PASS band 11 150 MHz tried 1 (\\d+)ms\n# log: video check: pass 150 MHz\n# nolog: vck INCONCLUSIVE\n# nolog: glass owned STALE\n# nolog: bench_sim: watchdog\n# timeout: 300\n', 's6fail': '# The bench session\'s clean finish in the simulator: the re-verify run with\n# stage 6\'s row failing (bench_sim set fail_s6, a must-pass step).  The\n# video steps that need it (vs, vplay, vlong) skip by themselves (one\n# "Skipping ahead" screen), and the session ends: no verified line, no clock150 token, each step\n# once and in order.  ucop is held (19-9 retired), so the end clears the\n# "cop" switch an earlier session left: the unit\'s next start runs the\n# build default (the daily driver after the sitting).  The core layout\n# probe (p2, p1, duty) needs nothing, so it still runs (the sim refuses it).\n# Generated by scripts/bench_sim_script.py (s6fail); edit that, not this.\n# needs: video\n# golden: skip\n# np: viz-full\n# reset: viz\n# nolog: vlong:\n# log: ui switch cleared\n# log: (ucop held)\n# log: p2: Not run (sim)\n# log: duty: Not run (sim)\n# log: s1: Not run (sim)\n# log: sd: Not run (sim)\n# nolog: next start: ui cop\n# nolog: glass owned STALE\n# nolog: bench_sim: watchdog\n# timeout: 300\n', 'faults': '# The bench session against sitting 4c\'s faults, in the simulator: the\n# normal re-verify run (as ipodui-p19-bench-reverify) with two fault\n# switches set while the visualiser step is outside the session:\n#  - bench_sim set putfail 1: the next results-file open fails, once (the\n#    device\'s lost lines): it is uviz\'s result line, written at Go on;\n#  - bench_sim set lowmem: the probe image\'s block is refused while music\n#    plays (the visualiser\'s album still playing at jvpu).\n# The runner must not lose or run again any step (each step once, in\n# order: 4c bugs a and c), and jvpu must still pass (4c bug b), so the\n# verified line is still earned.  Then, while vplay is outside, bench_sim\n# set putfails 9: nine opens in a row fail, so three writes are refused at\n# three tries each (FAULT bench put): their lines wait in the RAM queue\n# and go in first, in order, at the next write that opens; none reaches\n# bcmtest.log\'s "bench unsaved:" fallback (the queue holds 8).  The core\n# layout probe runs as in ipodui-p19-bench-reverify.\n# Generated by scripts/bench_sim_script.py (faults); edit that, not this.\n# needs: video\n# golden: skip\n# np: viz-full\n# reset: viz\n# log: vlong: PASS: pictures in step with the audio\n# log: results file: open refused (fault switch)\n# log: results file: open refused in a row (fault switch)\n# log: FAULT bench put\n# log: (ucop held)\n# log: p2: Not run (sim)\n# log: duty: Not run (sim)\n# log: s1: Not run (sim)\n# log: sd: Not run (sim)\n# nolog: bench unsaved:\n# nolog: glass owned STALE\n# nolog: bench_sim: watchdog\n# timeout: 300\n'}
HEADS['probe'] = ('# The core layout probe in the simulator (sitting 4h): Developer > Bench\n'
    '# session > Core layout probe, end to end, through the real menus.  The\n'
    '# simulator has no second core and no interrupt controller, so it refuses\n'
    '# the rows that need one rather than model a pass (devbench_sim.c\n'
    '# cop_row: "Not run (sim)", the route [hypothesis] until the bench): p2\n'
    '# and duty skip, asking nothing.  p1 has its fault switch on (bench_sim\n'
    '# set fail_p1), so its tone question still shows and is answered "Yes";\n'
    '# the row fails all the same.  The end: each pick once, in order\n'
    '# (devbench.h bench_probe_picks), the end line, no verified line.\n'
    '# Generated by scripts/bench_sim_script.py (probe); edit that, not this.\n'
    '# needs: video\n'
    '# golden: skip\n'
    '# log: p2: Not run (sim)\n'
    '# log: duty: Not run (sim)\n'
    '# log: p1: FAIL\n'
    '# nolog: bench_sim: watchdog\n'
    '# timeout: 120\n')
HEADS['awldhang'] = (
    '# The bench session\'s restart-and-resume path in the simulator, at an\n'
    '# early step (sitting 4gh): the re-verify run as ipodui-p19-bench-reverify,\n'
    '# but awld (the LCD supply at rest, before jvpu, on the resident image)\n'
    '# freezes the unit (bench_sim set hang_awld: the sim\'s row blocks, as a\n'
    '# unit left dark).  The script restarts it (the owner\'s MENU+SELECT),\n'
    '# opens Bench session and picks Go on: Resume records awld as hung\n'
    '# ("restarted during the step"), never runs pre, s05 or boot again, and\n'
    '# carries on at jvpu with the display driver there (the stage flags\n'
    '# bring it back at every start), so jvpu passes and the verified line\n'
    '# is still earned.  bench_sim expect checks every pick once, in order.\n'
    '# Generated by scripts/bench_sim_script.py (awldhang); edit that, not this.\n'
    '# needs: video\n'
    '# golden: skip\n'
    '# np: viz-full\n'
    '# reset: viz\n'
    '# env: SIM_VCHECK=1\n'
    '# log: awld: hanging (fault switch, sim)\n'
    '# log: start: display driver yes\n'
    '# log: vlong: PASS: pictures in step with the audio\n'
    '# log: duty: Not run (sim)\n'
    '# log: ui switch cleared\n'
    '# nolog: next start: ui cop\n'
    '# nolog: glass owned STALE\n'
    '# nolog: bench_sim: watchdog\n'
    '# timeout: 360\n')
HEADS['batch'] = (
    '# The chip update checks in the simulator (batch 5, sitting 4gh-b5):\n'
    '# Developer > Bench session > Chip update checks, end to end, through the\n'
    '# real menus (devbench.h bench_batch_picks).  pre, then s05 always (the\n'
    '# stages with the stage 4 bench), s4b hands off, s4c and its question,\n'
    '# the restart (boot) onto the display driver, the resident image\'s\n'
    '# checks (jobs, the drift outside the session, vf0, jvpu), with sitting\n'
    '# 4gh\'s VC rows whose answer batch 5 can change and the PP reruns\n'
    '# (preg, pldo, p1, pcop, psrf, awlg, duty, and 4gh-b5\'s heap A/B; s1,\n'
    '# vs, sd: all refused by\n'
    '# the sim; l5, ck and vlong passed in 4gh and are cut), s6 and its\n'
    '# question, then sdc: armed,\n'
    '# a play from Videos outside the session (the first-use check, its CLOSE\n'
    '# carrying SDRAM idle in the model), back, its question; sd last.  The\n'
    '# end: each pick once, in order, the end line, b9bcbe57\'s verified\n'
    '# line (b13 and b5 after jvpu, as in the re-verify run), the\n'
    '# clock150 token line (s4c and s6 passed).\n'
    '# Generated by scripts/bench_sim_script.py (batch); edit that, not this.\n'
    '# needs: video\n'
    '# golden: skip\n'
    '# np: viz-full\n'
    '# reset: viz\n'
    '# env: SIM_VCHECK=1\n'
    '# log: s05: stages\n'
    '# log: s4b: stages\n'
    '# log: s4c: stages\n'
    '# log: vf0: PASS\n'
    '# log: sdc: armed\n'
    '# log: sdc: idle code 0 polls\n'
    '# nolog: vlong:\n'
    '# log: s1: Not run (sim)\n'
    '# log: sd: Not run (sim)\n'
    '# log: heap: Not run (sim)\n'
    '# log: heap walk at=<jobs\n'
    '# log: heap walk at=>sd\n'
    '# nolog: heap BAD\n'
    '# log: unit check: token dropped (bench reset)\n'
    '# nolog: glass owned STALE\n'
    '# nolog: bench_sim: watchdog\n'
    '# timeout: 300\n')
NAMES = {"normal": "reverify", "s6fail": "s6fail", "faults": "faults",
         "probe": "probe", "awldhang": "awldhang", "batch": "batch"}

def go(out, note=""):
    # a start/go screen: the go line is the last; the list doesn't wrap
    out.append(f"wheel +40 10{('          # ' + note) if note else ''}")
    out.append("wait 400")
    out.append("key SELECT")
    out.append("wait 800")

def answer(out, n, a=0, note=""):
    # a question: the n answers are the last rows; answer a
    out.append(f"wheel +40 10{('          # ' + note) if note else ''}")
    out.append("wait 400")
    back = n - 1 - a
    if back:
        out.append(f"wheel -{back} 200")
        out.append("wait 400")
    out.append("key SELECT")
    out.append("wait 800")

def leave_summary(out):
    out.append("key LEFT                # the stopped-session screen: leave")
    out.append("wait 800")

def to_bench(out, playing=False):
    # from the main menu: Settings > About > the developer chord > Bench
    out += ["wheel +20 40            # the last row (Now Playing while music plays)",
            "wait 400",
            ("wheel -2 200            # Settings" if playing else
             "wheel -1 200            # Settings"),
            "wait 400",
            "key SELECT",
            "wait 1200",
            "key SELECT              # About",
            "wait 1500",
            "chord SELECT+LEFT 3200  # Developer",
            "wait 1000",
            "wheel +1 200            # Bench session",
            "wait 400",
            "key SELECT",
            "wait 1200"]

def to_main(out):
    out += ["key LEFT                # out of Developer (a list: LEFT)",
            "wait 800", "key MENU                # About > Settings > Main",
            "wait 800", "key MENU", "wait 800", "key MENU", "wait 800"]

def resume(out, note):
    # the resume question: Go on / Start a new session / Cancel
    answer(out, 3, 0, note)

def manual(out, title, n_answers, outside, a=0, roundtrip=False,
           leaves=False, playing=True):
    """A manual step: its start screen (leave), the owner's part outside,
    then back: Go on, its question.  leaves: a BENCH_F_LEAVES step (its
    function runs, then a "do it outside" screen before the summary)"""
    out.append(f"# -- {title}")
    go(out, f"{title}: start screen, SELECT: leave")
    if leaves:
        out.append("key LEFT                # the do-it-outside screen: close")
        out.append("wait 800")
    leave_summary(out)
    if roundtrip:
        to_main(out)
    out += outside
    if roundtrip:
        to_bench(out, playing=playing)
    else:
        out += ["key SELECT              # Bench session (the Developer menu)",
                "wait 1000"]
    resume(out, f"Go on: {title}")
    answer(out, n_answers, a, f"{title}: the question")

def probe_script():
    """Sitting 4h's core layout probe: p2 (hands off), p1 (the tone and its
    question), duty (hands off, dark).  devbench.h bench_probe_picks"""
    o = ["bench_sim reset          # no results files, the model's defaults",
         "bench_sim set fail_p1    # p1 fails, so its question still shows",
         "play_files /Music/Tone%20Collective/Sine%20Studies/01%20Concert%20A.mp3",
         "wait 1500                # the album the probe's confirm asks for"]
    to_bench(o, playing=True)
    answer(o, 5, 3, "Core layout probe")
    go(o, "the confirm screen: start")
    o.append("# -- p2: hands off (the first step: no hands-off screen)")
    go(o, "Second core timer: start")
    go(o, "Automatic checks done")
    o.append("# -- p1: the tone")
    go(o, "Tone on the second core: start")
    answer(o, 4, 0, "p1: Yes (the row failed by its switch)")
    o.append("# -- duty: hands off, dark")
    go(o, "Hands off now")
    go(o, "Dark play, both cores: start")
    o.append("shot summary")
    o.append("bench_sim dump")
    o.append("bench_sim expect order ended noverified notoken p2=skip "
             "p1=fail duty=skip")
    leave_summary(o)
    to_main(o)
    o.append("wait 1000")
    return o

def batch_script():
    """Batch 5's chip update checks (devbench.h bench_batch_picks): pre,
    s05, s4b, s4c, boot, jobs, b3, vf0, jvpu, s6, sdc"""
    o = ["bench_sim reset          # no results files, the model's defaults",
         # a play first: this unit's check result stored, which sdc drops
         "copy ../../bcmfw/video/vcheck.bin /.rockbox/vcheck.bin",
         "rm_file /.rockbox/.sim_vcheck_band",
         "rm_file /.rockbox/.sim_vcheck_bad",
         "video_start /Misc/S01E03.m4v   # this unit's check result",
         "wait 4000", "video_stop", "wait 500"]
    to_bench(o)
    answer(o, 5, 4, "Chip update checks")
    go(o, "the confirm screen: start")
    o.append("# -- preg: hands off (refused by the sim)")
    go(o, "Hands off now (preg)")
    go(o, "Automatic checks done (preg)")
    o.append("# -- s05: the test pictures and the stage 4 bench (always)")
    go(o, "Test pictures: start")
    answer(o, 4, 0, "s05: Yes")
    o.append("# -- s4b: hands off")
    go(o, "Hands off now (s4b)")
    go(o, "Automatic checks done")
    o.append("# -- s4c")
    go(o, "Fast clock pictures: start")
    answer(o, 4, 0, "s4c: Yes")
    o.append("# -- boot (Display program): the restart")
    go(o, "Display program: start")
    go(o, "Now restart: close")
    leave_summary(o)
    o.append("reboot")
    o.append("wait 4000")
    to_bench(o)
    resume(o, "Go on: Display program")
    o.append("# -- jobs: hands off")
    go(o, "Hands off now (jobs)")
    go(o, "Automatic checks done")
    manual(o, "Drift on the chip", 4,
           ["wait 1000                # the drift, outside the session"])
    o.append("# -- vf0: hands off")
    go(o, "Hands off now (vf0)")
    go(o, "Automatic checks done")
    o.append("# -- p19-vcpwr: s1 (refused by the sim)")
    go(o, "Chip sleeps when idle: start")
    o.append("# -- pldo (refused by the sim: no question)")
    go(o, "Screen and sound supply: start")
    o.append("# -- jvpu: hands off")
    go(o, "Hands off now (jvpu)")
    go(o, "Automatic checks done")
    o.append("# -- b13, b5: the chip handed to Apple's program")
    go(o, "Program swap: start")
    answer(o, 3, 0, "b13: No")
    go(o, "Window test: start")
    answer(o, 3, 0, "b5: Yes")
    o.append("# -- s6")
    go(o, "Test clips: start")
    answer(o, 4, 0, "s6: Smooth, in sync")
    o.append("# -- vs (refused by the sim)")
    go(o, "Video with chip sleep: start")
    manual(o, "Video memory rest", 4,
           ["video_start /Misc/S01E03.m4v   # the player, as Videos plays it",
            "wait 4000                # the check (its CLOSE idles SDRAM), then the play",
            "video_stop",
            "wait 500"], leaves=True)
    o.append("# -- the PP reruns (docs/bench-reruns.md): p1 (refused by the sim)")
    go(o, "Tone on the second core: start (the sim refuses: no question)")
    o.append("# pcop skips behind p1's gate, psrf behind preg's: a screen each")
    go(o, "Skipping ahead: pcop")
    go(o, "Skipping ahead: psrf")
    manual(o, "Play an album", 2,
           ["play_files /Music/Tone%20Collective/Sine%20Studies/01%20Concert%20A.mp3",
            "wait 1500                # an album plays"], leaves=True)
    o.append("# -- awlg, duty: the dark soaks (refused by the sim)")
    go(o, "Drive rest after reading: start")
    go(o, "Hands off now (duty)")
    go(o, "Dark play, both cores: start")
    o.append("# -- heap: the heap A/B on both cores (refused by the sim)")
    go(o, "Memory check, both cores: start")
    o.append("# -- sd, last of all (refused by the sim)")
    go(o, "Automatic checks done (after heap)")
    go(o, "Chip memory idle: start")
    o.append("shot summary")
    o.append("bench_sim dump")
    o.append("bench_sim expect order ended verified token s05=pass "
             "s4b=pass s4c=pass jobs=pass vf0=pass jvpu=pass b5=pass s6=pass "
             "sdc=pass alb=pass s1=skip vs=skip sd=skip preg=skip "
             "pldo=skip p1=skip pcop=skip psrf=skip awlg=skip duty=skip "
             "heap=skip")
    leave_summary(o)
    to_main(o)
    o.append("wait 1000")
    return o

def freeze_rows(out):
    """Sitting 4gh: the rows that may freeze, after s6 (p2, p1; devbench.h
    bench_reverify_picks group 4b).  The sim refuses both (devbench_sim.c
    cop_row: "Not run (sim)"), so p1 asks nothing; the start screens show"""
    out.append("# -- sitting 4gh: the may-freeze rows (p2, p1)")
    go(out, "Hands off now (p2)")
    go(out, "Second core timer: start")
    go(out, "Automatic checks done")
    go(out, "Tone on the second core: start (the sim refuses: no question)")
    # core layout A: pcop after p1; the sim refused p1, so pcop
    # auto-skips behind p1's gate (one screen, no question)
    go(out, "Skipping ahead: pcop (p1's gate in the sim)")
    # p19-pwr-awake: awi2 may freeze, after p1 (refused by the sim)
    go(out, "Sound clock at rest: start")
    # p19-pwr-probes: the writing probes, least risky first (refused by
    # the sim; preg's skip there gates pden and psrf: one gate screen)
    go(out, "Hands off now (pstl)")
    go(out, "Timed pauses: start")
    # no "Automatic checks done": the steps after pstl until vplay all
    # skip, so the next screen is pden's gate
    go(out, "Skipping ahead: pden and psrf (preg's gate in the sim), once")

def cable_rows(out):
    """p19-pwr-awake: the cable steps after s6 (awd2, awd2c, awus),
    refused by the sim, so none asks and awus doesn't leave"""
    out.append("# -- p19-pwr-awake: the cable steps (refused by the sim)")
    go(out, "Accessory supply: start")
    go(out, "Accessory supply, USB-C sound: start")
    go(out, "Charger only: start")

def duty_row(out):
    """Sitting 4gh: duty, the long dark row, at the very end (refused by
    the sim, as p2 and p1)"""
    out.append("# -- p19-pwr-awake: awlg, its own dark soak (refused by the sim)")
    go(out, "Drive rest after reading: start")
    out.append("# -- core layout A: k1, its own dark soak (refused by the sim)")
    go(out, "Hands off now (k1, duty)")
    go(out, "Second core parked: start")
    out.append("# -- sitting 4gh: duty (k1 is hands off too: no screen between)")
    go(out, "Dark play, both cores: start")

def sd_row(out):
    """p19-vcpwr: sd last of all (its cut drops the resident image),
    refused by the sim"""
    out.append("# -- sd, last of all (refused by the sim)")
    go(out, "Automatic checks done (after duty)")
    go(out, "Chip memory idle: start")

def script(kind):
    if kind == "probe":
        return probe_script()
    if kind == "batch":
        return batch_script()
    o = []
    o.append("bench_sim reset          # no results files, the model's defaults")
    if kind in ("normal", "awldhang"):
        # a play first, as sitting 4c's left this unit: its check result
        # stored (vcheck.tok), which vplay must drop
        o += ["copy ../../bcmfw/video/vcheck.bin /.rockbox/vcheck.bin",
              "rm_file /.rockbox/.sim_vcheck_band",
              "rm_file /.rockbox/.sim_vcheck_bad",
              "video_start /Misc/S01E03.m4v   # this unit's check result",
              "wait 4000", "video_stop", "wait 500"]
    if kind == "s6fail":
        o.append("bench_sim set fail_s6    # stage 6's row fails (a must-pass step)")
    # an earlier session left the switch on the COP: ucop is held, so
    # the session's end must clear it (the daily driver)
    o.append("bench_sim set ui_split cop  # an earlier session's switch")
    to_bench(o)
    answer(o, 5, 1, "Re-verify the video image")
    go(o, "the confirm screen: start")
    # p19-pwr-awake: awxm, read-only, straight after pre (refused by the sim)
    go(o, "Hands off now (pgrd, preg, awxm, prtc)")
    go(o, "Seconds counter: start")
    # pre: done (the flags, no display driver); s05 not needed; boot
    o.append("# -- boot (Display program): the restart")
    go(o, "Display program: start")
    go(o, "Now restart: close")
    leave_summary(o)
    o.append("shot boot_asked")
    o.append("reboot")
    o.append("wait 4000")
    to_bench(o)
    resume(o, "Go on: Display program")
    # jobs: hands off
    go(o, "Hands off now")
    go(o, "Automatic checks done")
    # sitting 4f: respanel (Cover on the chip) and c1 held (passed in 4e)
    if kind == "faults":
        # the open of the next results line fails once: uviz's result line
        # (sitting 4c's lost line); the probe image refused while music
        # plays (sitting 4c's "no probe image" after the visualiser)
        uviz_after = ["bench_sim set putfail 1",
                      "bench_sim set lowmem"]
    else:
        uviz_after = []
    manual(o, "Visualiser on the chip", 4,
           ["play_files /Music/Tone%20Collective/Sine%20Studies/01%20Concert%20A.mp3",
            "wait 1500",
            "wheel +20 40             # Now Playing (the main menu's last row)",
            "wait 400",
            "key SELECT",
            "wait 1500",
            # sitting 4f: uviz counts its own views: the modes to the
            # Visualizer, a few seconds, back to Normal (its soak line)
            "key SELECT               # Scrubber",
            "wait 400",
            "key SELECT               # Rating",
            "wait 600",
            "key SELECT               # Shuffle",
            "wait 600",
            "key SELECT               # Visualizer",
            "wait 3000                # the visualiser on the panel",
            "shot uviz_np",
            "key SELECT               # back to Normal: the view closes",
            "wait 1200",
            "key MENU                 # back to the main menu",
            "wait 3000                # the visualiser plays"] + uviz_after,
           roundtrip=True, leaves=True)
    # sitting 4g: vizsoak held (git history: 4f's lines)
    # vf0 and jvpu: one hands-off block (c1 held)
    go(o, "Hands off now (vf0)")
    go(o, "Automatic checks done")
    # p19-vcpwr: the chip's awake A/Bs before jvpu (the resident image);
    # the sim refuses them (devbench_vcpwr.c vcp_sim_row): no questions
    o.append("# -- p19-vcpwr: s1, l5, ck (refused by the sim)")
    go(o, "Chip sleeps when idle: start")
    go(o, "Chip low core level: start")
    go(o, "Chip block clocks: start")
    # p19-pwr-probes: pldo, then awld, on the resident image, before jvpu
    go(o, "Screen and sound supply: start")
    if kind == "awldhang":
        # the unit freezes in awld (the screen left dark): the owner holds
        # MENU+SELECT; Resume records awld as hung and goes on at jvpu,
        # with the display driver back (the stage flags, this start)
        o.append("bench_sim set hang_awld  # awld freezes the unit")
        go(o, "Screen supply at rest: start (it hangs)")
        o += ["wait 3000", "shot awld_hung",
              "reboot                   # the owner's MENU+SELECT",
              "wait 4000",
              "bench_sim clear hang_awld"]
        to_bench(o)
        resume(o, "Go on: after awld's restart")
    else:
        go(o, "Screen supply at rest: start")
    go(o, "Hands off now (jvpu)")
    go(o, "Automatic checks done")
    # b13: the host flush's A/B (the sim's row is a stub)
    o.append("# -- b13")
    go(o, "Program swap: start")
    answer(o, 3, 0, "b13: No")
    o.append("# -- b5")
    go(o, "Window test: start")
    answer(o, 3, 0, "b5: Yes")
    o.append("# -- s6")
    go(o, "Test clips: start")
    if kind == "s6fail":
        # s6 failed by itself: no question (the row's lines failed it), or
        # the question: the runner asks it after the row
        answer(o, 4, 0, "s6: Smooth, in sync (the row failed)")
        # vs is the first step that needs s6: the gate's one screen
        cable_rows(o)
        go(o, "Skipping ahead: the video steps (s6's gate)")
        freeze_rows(o)
        # the screen shows once a gate in a row (devbench.c gate_shown):
        # preg's gate (the sim's skip) came between, so s6's shows again
        # at vplay
        go(o, "Skipping ahead: vplay, vlong (s6's gate again)")
        duty_row(o)
        sd_row(o)
    else:
        answer(o, 4, 0, "s6: Smooth, in sync")
        cable_rows(o)
        o.append("# -- vs (refused by the sim)")
        go(o, "Video with chip sleep: start")
        freeze_rows(o)
        manual(o, "Play a video", 4,
               ["video_start /Misc/S01E03.m4v   # the player, as Videos plays it",
                "wait 4000                # the check (~0.9 s), then the play",
                "video_stop",
                "wait 500"]
               + (["bench_sim set putfails 9   # three writes in a row refused"]
                  if kind == "faults" else []), leaves=True)
        # sitting 4e: the long play, the test episode to its end (the
        # sim's 12 s clip; the device's 10 min of Sintel)
        manual(o, "Long video play", 3,
               ["video_screen /Misc/S01E03.m4v  # the video screen, as Videos opens it",
                "wait 8000                # 8 s of the 12 s clip, sound and pictures",
                "shot vlong",
                "key MENU                 # leave the video (back to the main menu): its end line",
                "wait 2000"], leaves=True, roundtrip=True, playing=False)
        duty_row(o)
        sd_row(o)
    # 19-9 retired (owner, 2026-11-05): the core-layout arms and the
    # second-core group held (git history: 4g's lines for them)
    o.append("shot summary")
    o.append("bench_sim dump")
    if kind == "s6fail":
        o.append("bench_sim expect order ended noverified notoken s6=fail "
                 "vplay=autoskip vlong=autoskip jvpu=pass b5=pass "
                 "vs=autoskip s1=skip l5=skip ck=skip sd=skip "
                 "awxm=skip awd2=skip awd2c=skip awus=skip "
                 "pgrd=skip preg=skip prtc=skip pldo=skip pstl=skip "
                 "pden=autoskip psrf=autoskip pcop=skip k1=skip "
                 + ("awld=hang " if kind == "awldhang" else "awld=skip ") +
                 "awi2=skip awlg=skip "
                 "p2=skip p1=skip duty=skip nextdefault")
    else:
        o.append("bench_sim expect order ended verified notoken jobs=pass "
                 "jvpu=pass b5=pass s6=pass vplay=pass vlong=pass "
                 "s1=skip l5=skip ck=skip vs=skip sd=skip "
                 "awxm=skip awd2=skip awd2c=skip awus=skip "
                 "pgrd=skip preg=skip prtc=skip pldo=skip pstl=skip "
                 "pden=autoskip psrf=autoskip pcop=skip k1=skip "
                 + ("awld=hang " if kind == "awldhang" else "awld=skip ") +
                 "awi2=skip awlg=skip "
                 "p2=skip p1=skip duty=skip nextdefault")
    # out of the summary and Developer (the trace's devmode enter/exit)
    leave_summary(o)
    to_main(o)
    o.append("wait 1000")
    return o

if __name__ == "__main__":
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    for kind, name in NAMES.items():
        path = os.path.join(root, "tests", "ui",
                            "ipodui-p19-bench-%s.script" % name)
        with open(path, "w") as f:
            f.write(HEADS[kind] + "wait 4000\n" + "\n".join(script(kind))
                    + "\n")
        print(os.path.relpath(path, root))
