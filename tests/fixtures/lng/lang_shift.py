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

"""lang_shift.py IN OUT [ID]: copy english.lang IN to OUT with one extra core
phrase ID (default LANG_PROKBOX_TEST_SHIFT) before the first phrase, so every
later string ID moves up by one."""
import sys
src, dst = sys.argv[1], sys.argv[2]
pid = sys.argv[3] if len(sys.argv) > 3 else 'LANG_PROKBOX_TEST_SHIFT'
s = open(src, encoding='utf-8').read()
phrase = ('<phrase>\n  id: %s\n  desc: test only\n  user: core\n'
          '  <source>\n    *: "Shift"\n  </source>\n'
          '  <dest>\n    *: "Shift"\n  </dest>\n'
          '  <voice>\n    *: ""\n  </voice>\n</phrase>\n' % pid)
i = s.index('<phrase>')
open(dst, 'w', encoding='utf-8').write(s[:i] + phrase + s[i:])
