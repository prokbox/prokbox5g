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

# The floor gate's upgrade case (tests/perf/floor_gate.sh): the first boot
# of this firmware on the 0.8.0 library, in the order the session's audio
# buffer is sized right (p081-floor, p090-floor). Read from a sim.log
# (awk -f upgrade_order.awk LOG); exit 0 = the order held, else 1, with
# one line either way.
#   Same library format: the boot's publication frees the retired
#   generation's RAM cache ("db retired ram free") before the first
#   "lib play" (a cache kept past it would size the session's buffer
#   without those bytes).
#   A format bump ("db upgrade owed from=.. to=.. why=..": the library
#   stamp, apps/ipodui/libstamp.h; SORTKEY_VERSION 2 to 3
#   in 0.9.0): the old library is refused, so it has no RAM cache to
#   retire; the boot rebuilds it once. Before the first "lib play" the
#   old library's statistics are exported ("db upgrade export rc=") and
#   the rebuild is published ("db gen N n=..."); after it, a RAM cache
#   is freed ("db retired ram free") only by a later publication (a "db
#   gen" between the first play and the free: the session's own library
#   updates), never with none (a cache from before the play, held while
#   playing).
#   After that boot the library is in the new format, so a later boot is
#   the same-format case again.
# tests/floorrules/run.sh runs both cases' good and bad orders.
/ipodui: db upgrade owed from=/ { if (!play) owed = 1 }
/ipodui: db upgrade export rc=/ { if (!play) exported = 1 }
/ipodui: db gen [0-9]+ n=/ { if (!play) published = 1; else regen = 1 }
/ipodui: db retired ram free gen=/ { if (!play) retired = 1; else if (!regen) late = 1 }
/ipodui: lib play / { if (!play) { play = 1; owed_seen = owed } }
END {
    if (!play) { print "no \"lib play\" in the log"; exit 1 }
    if (owed_seen) {
        if (!exported) { print "format bump: no \"db upgrade export\" before the first play"; exit 1 }
        if (!published) { print "format bump: the rebuild was not published (\"db gen\") before the first play"; exit 1 }
        if (late) { print "format bump: a RAM cache freed after the first play with no publication since (an old cache held while playing)"; exit 1 }
        print "format bump: rebuilt, statistics exported and published before the first play"
        exit 0
    }
    if (!retired) { print "no \"db retired ram free\" before the first \"lib play\" (the boot's publication did not free the old RAM cache before playback)"; exit 1 }
    print "same format: the old RAM cache retired before the first play"
}
