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

"""#268 host check: a 24-bit-output WAV (32-bit PCM, the 24 bits at the top)
against its source WAV (16- or 24-bit PCM).  'exact IN OUT': every output
sample is the source sample placed at the top of the 32-bit word (bit-exact
24-bit passthrough).  Prints the count compared; refuses an empty compare
and a source that does not use its low bits."""
import struct
import sys
import wave


def samples(path):
    w = wave.open(path, 'rb')
    width, n = w.getsampwidth(), w.getnframes()
    raw = w.readframes(n)
    if width == 2:
        return width, list(struct.unpack('<%dh' % (len(raw) // 2), raw))
    if width == 3:
        return width, [int.from_bytes(raw[i:i + 3], 'little', signed=True)
                       for i in range(0, len(raw), 3)]
    if width == 4:
        return width, list(struct.unpack('<%di' % (len(raw) // 4), raw))
    raise SystemExit('unsupported width %d' % width)


def main():
    a, b = sys.argv[2:4]
    wa, sa = samples(a)
    wb, sb = samples(b)
    if wb != 4:
        print('output is %d-bit, not 32-bit words' % (8 * wb))
        return 1
    if not sa or len(sa) != len(sb):
        print('lengths differ or empty: %d vs %d' % (len(sa), len(sb)))
        return 1
    shift = 32 - 8 * wa
    bad = [i for i in range(len(sa)) if sb[i] != sa[i] << shift]
    low = sum(1 for v in sb if v & 0xff)
    if bad or low:
        i = bad[0] if bad else 0
        print('%d of %d samples differ (first %d: %d vs %d << %d); %d with '
              'the low byte set' % (len(bad), len(sa), i, sb[i], sa[i], shift,
                                    low))
        return 1
    # the source must use the bits a 16-bit path would lose
    if wa == 3:
        lowbits = sum(1 for v in sa if v & 0xff)
        if lowbits < len(sa) // 2:
            print('source has too few low-byte bits (%d)' % lowbits)
            return 1
    print('%d samples exact (%d-bit source)' % (len(sa), 8 * wa))
    return 0


sys.exit(main())
