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

# The library stress ceiling per memory size (owner, 2026-11-09, #229):
# 32 MB units are tested at 50k songs, 64 MB units at 65k. Sourced by the
# tests/perf scripts that run per unit (floor_gate.sh, resort_play_check.sh,
# commit_peak.sh, snapshot-memory.sh); needs ROOT set.
#
#   stress_k RAM      -> 50 | 65 (thousands of tracks)
#   stress_disk RAM   -> $ROOT/build/sim-<k>k/simdisk
#   stress_lib RAM    -> the script that makes that disk (lib50k.sh / lib65k.sh)
stress_k() {
    case "$1" in
        32) echo 50 ;;
        64) echo 65 ;;
        *) return 1 ;;
    esac
}
stress_disk() { _k=$(stress_k "$1") || return 1; echo "$ROOT/build/sim-${_k}k/simdisk"; }
stress_lib() { _k=$(stress_k "$1") || return 1; echo "$ROOT/tests/perf/lib${_k}k.sh"; }
