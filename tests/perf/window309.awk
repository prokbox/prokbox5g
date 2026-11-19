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

# #309, the 64 MB unit's paused window at 65k (floor-65k.script), read
# from a sim.log by tests/perf/floor_gate.sh (awk -v floor=BYTES -f
# window309.awk). Owner, 2026-11-14: at 65k on 64 MB the library commit
# does not fit the paused window above the floor, so the window WAITS and
# the commit runs at the stop. After "floor before_window":
#   - "maint job=commit wait floor" (the refusal), and no
#     "maint take job=commit ok=1" before the stop (nothing taken below
#     the floor);
#   - at most one "maint retry after regrow job=commit" (#309: one regrow
#     a pause, so one retry), refused again;
#   - "floor window_wait": the buffer while it waits, at or above the floor;
#   - then "sim_script: audio_stop", and after it "db commit ... ok=1"
#     (committed at the stop).
# Prints the window's line with its margin; exit 0 = the rule held, else 1
# and the reason (one line). tests/floorrules/run.sh checks the good order
# and the bad ones.
/sim_script: floor before_window / { w = 1; next }
!w { next }
!st && /ipodui: maint job=commit wait floor/ { wt++ }
!st && /ipodui: maint retry after regrow job=commit/ { retries++ }
!st && /ipodui: maint take job=commit ok=1/ { tk++ }
!st && /ipodui: db commit .*ok=1/ { early++ }
!st && /sim_script: floor window_wait / {
    for (i = 1; i <= NF; i++) if ($i ~ /^audio=/) a = substr($i, 7) + 0
    ww = 1 }
!st && /sim_script: audio_stop/ { st = 1; next }
st && /ipodui: db commit .*ok=1/ { done = 1; exit }
END {
    if (!wt) { print "no \"maint job=commit wait floor\" in the window (65k on 64 MB must wait for the stop)"; exit 1 }
    if (tk) { print "\"maint take job=commit ok=1\" in the window (it must wait, not take)"; exit 1 }
    if (early) { print "a commit before the stop"; exit 1 }
    if (retries > 1) { print "\"maint retry after regrow job=commit\" " retries " times in the window, at most once"; exit 1 }
    if (!ww) { print "no \"floor window_wait\" line in the window"; exit 1 }
    if (!st) { print "no audio_stop after the window"; exit 1 }
    if (!done) { print "no \"db commit ... ok=1\" after the stop"; exit 1 }
    if (a < floor) { print "window_wait audio=" a " B under the floor " floor " B"; exit 1 }
    printf "paused window (#309, 65k): waited (%d retry), committed at the stop; audio=%d B, floor=%d B, margin=%d B\n", retries + 0, a, floor, a - floor
}
