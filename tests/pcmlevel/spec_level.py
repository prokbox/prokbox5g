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

# srcpatch spec (tests/fencepcm/srcpatch.py): the real data_level() from
# apps/pcmbuf.c, verbatim, and the one mutant that puts back the 4-byte
# frame count in the 24-bit build.
SLICES = [
    (r"^/\* Return data level in 1/4-second increments \*/$",
     r"^/\* Number of bytes played per second \*/$"),
]
INSTRUMENT = []
MUTANTS = {
    "four_byte_frames": (
        "a 24-bit frame counted as 4 bytes (the 0.9.0 B finding)",
        [("return mixer_get_frequency() * PCMBUF_SAMPLE_SIZE / 4 * quarter_secs;",
          "return mixer_get_frequency() * quarter_secs;")],
    ),
}
