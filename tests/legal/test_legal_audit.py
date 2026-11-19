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
# Self-test for scripts/legal_audit.py.
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
"""Self-test for scripts/legal_audit.py: plants defects in a throwaway git
repo under build/legal-fixture and checks each is caught (and the clean
fixture passes).  usage: python3 tests/legal/test_legal_audit.py"""
import os, shutil, subprocess, sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
AUDIT = os.path.join(REPO, 'scripts/legal_audit.py')
FX = os.path.join(REPO, 'build/legal-fixture')
HDR = '/* Copyright (C) 2003 Alice Upstream\n * Licensed under the GNU General Public License v2\n * WITHOUT WARRANTY\n */\nint x;\n'
BANNER = '/*\n * Custom firmware for the iPod Video, 5th generation (5G and 5.5G).\n *\n * Copyright (C) 2026 Owner\n *\n'
GPL2P = (' * This program is free software; you can redistribute it and/or\n'
         ' * modify it under the terms of the GNU General Public License\n'
         ' * as published by the Free Software Foundation; either version 2\n'
         ' * of the License, or (at your option) any later version.\n')
DUAL = ' * SPDX-License-Identifier: MIT OR GPL-2.0-or-later\n *\n * You may use this file under either the MIT licence or the GPL.\n'
GPL3P = GPL2P.replace('either version 2\n', 'either version 3\n')
LGPL = (' * This file is free software; you can redistribute it and/or modify it\n'
        ' * under the terms of the GNU Lesser General Public License as published by\n'
        ' * the Free Software Foundation; either version 2.1 of the License, or (at\n'
        ' * your option) any later version.\n')
PBX = BANNER + GPL2P + ' */\nint y;\n'
LIC_TABLE = ('<!-- audit3-licences-begin -->\n| Files | Licence | Why |\n|---|---|---|\n'
             '| `src/sdk/*.h`, `src/sdk/one.c` | `MIT OR GPL-2.0-or-later` | test SDK |\n'
             '| `src/ff/*.c` | `LGPL-2.1-or-later` | test third party |\n<!-- audit3-licences-end -->\n')
LEGAL = ('## 4. Tree\n\n' + LIC_TABLE + '\n## 5. Asset provenance\n\n| Path | Origin | Licence |\n|---|---|---|\n'
         '| `art/ok/*.png` | gen | GPL |\n\n## 6. x\n')
LANG = '<phrase>\n  id: LANG_IPODUI_BAD\n  <dest>\n    *: none\n    ipodui: "Sync with iTunes"\n  </dest>\n</phrase>\n'


def sh(*a, cwd=FX):
    subprocess.run(a, cwd=cwd, check=True, capture_output=True)


def write(rel, data, mode='w'):
    p = os.path.join(FX, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    open(p, mode).write(data)
    sh('git', 'add', '-A')   # the audit looks at tracked files only


def setup():
    shutil.rmtree(FX, ignore_errors=True)
    os.makedirs(FX)
    sh('git', 'init', '-q')
    sh('git', 'config', 'user.email', 't@t'); sh('git', 'config', 'user.name', 't')
    write('docs/COPYING', 'GNU GENERAL PUBLIC LICENSE\n'); write('docs/CREDITS', 'c\n')
    write('docs/LICENSES', 'l\n'); write('apps/ipodui/fonts/GUST-FONT-LICENSE.txt', 'g\n')
    write('lib/foo/COPYING', 'GNU GENERAL PUBLIC LICENSE\n')
    write('src/a.c', HDR); write('src/b.c', HDR); write('src/NOTICE', 'Permission is hereby granted\n')
    write('apps/lang/english.lang', '')
    sh('git', 'add', '-A'); sh('git', 'commit', '-qm', 'base'); sh('git', 'tag', 'base')
    write('docs/legal.md', LEGAL); write('src/new.c', PBX); write('art/ok/a.png', 'x')
    sh('git', 'add', '-A'); sh('git', 'commit', '-qm', 'ours')


def audit(*extra):
    r = subprocess.run([sys.executable, AUDIT, '--base', 'base', '--root', FX, '--no-secrets',
                        '--allowlist', os.path.join(FX, 'none.txt')] + list(extra),
                       capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def expect(name, rc_want, text_want, extra=()):
    rc, out = audit(*extra)
    ok = rc == rc_want and (text_want in out)
    print('%-4s %s' % ('ok' if ok else 'FAIL', name))
    if not ok:
        print(out)
    return ok


def main():
    setup()
    res = [expect('clean fixture passes', 0, 'LEGAL PASSED')]
    # 1: delete a copyright line from a modified upstream file
    write('src/a.c', HDR.replace('Copyright (C) 2003 Alice Upstream', 'Copyright (C) 2026 Me') + '/*x*/\n')
    res.append(expect('check 1 removed copyright line', 1, 'header line removed/changed'))
    # a legal modification (one added notice line) must pass
    write('src/a.c', HDR.replace(' */', ' * Modified for ProkBox5g (2026); see docs/changes-from-rockbox.md.\n */') + 'int z;\n')
    res.append(expect('check 1 allows the modified notice', 0, 'LEGAL PASSED'))
    # 4: enforced only on request
    write('src/b.c', HDR + 'int q;\n')
    res.append(expect('check 4 default is not a failure', 0, 'LEGAL PASSED'))
    res.append(expect('check 4 --require-modnotice', 1, 'lacks "Modified for ProkBox5g"', ['--require-modnotice']))
    write('src/b.c', HDR)
    # 2: deleted licence file
    os.remove(os.path.join(FX, 'src/NOTICE'))
    res.append(expect('check 2 deleted NOTICE', 1, 'src/NOTICE'))
    write('scripts_allow.txt', 'deleted|src/NOTICE|reviewed in test\n')
    rc, out = subprocess.run([sys.executable, AUDIT, '--base', 'base', '--root', FX, '--no-secrets', '--allowlist',
                              os.path.join(FX, 'scripts_allow.txt')], capture_output=True, text=True).returncode, ''
    res.append(print('%-4s check 2 allow-list' % ('ok' if rc == 0 else 'FAIL')) or rc == 0)
    write('src/NOTICE', 'Permission is hereby granted\n')
    # 3: new .c with no header
    write('src/nohdr.c', 'int main(void){return 0;}\n')
    res.append(expect('check 3 headerless new .c', 1, 'src/nohdr.c'))
    os.remove(os.path.join(FX, 'src/nohdr.c'))
    # 3: the licence itself (#483).  Each red case is a file the audit
    # must name; each green case must pass.
    def c3(name, rel, text, rc_want):
        write(rel, text)
        ok = expect(name, rc_want, rel if rc_want else 'LEGAL PASSED')
        os.remove(os.path.join(FX, rel)); sh('git', 'add', '-A')
        return ok
    res.append(c3('check 3 banner + GPL-3.0-or-later', 'src/wrong.c', BANNER + GPL3P + ' */\n', 1))
    res.append(c3('check 3 banner + no licence', 'src/nolic.c', BANNER + ' */\n', 1))
    res.append(c3('check 3 banner + short "GPL v2 or later"', 'src/short.c',
                  BANNER + ' * GPL v2 or later.\n */\n', 1))
    res.append(c3('check 3 banner only in code, no licence', 'src/code.py',
                  'S = "Custom firmware for the iPod Video, 5th generation"\n', 1))
    res.append(c3('check 3 dual outside the allowed set', 'src/dual.c', BANNER + DUAL + ' */\n', 1))
    res.append(c3('check 3 dual and GPL-only grant together', 'src/sdk/both.h', BANNER + DUAL + GPL2P + ' */\n', 1))
    res.append(c3('check 3 dual in the allowed set (glob)', 'src/sdk/ok.h', BANNER + DUAL + ' */\n', 0))
    res.append(c3('check 3 dual in the allowed set (path)', 'src/sdk/one.c', BANNER + DUAL + ' */\n', 0))
    res.append(c3('check 3 GPL file in the dual set', 'src/sdk/gpl.h', PBX, 0))
    res.append(c3('check 3 listed LGPL file', 'src/ff/idct.c', '/* from FFmpeg\n *\n' + LGPL + ' */\n', 0))
    res.append(c3('check 3 LGPL file not listed', 'src/lgpl.c', '/* from FFmpeg\n *\n' + LGPL + ' */\n', 1))
    res.append(c3('check 3 banner + listed licence of another file', 'src/ff/b.c', BANNER + LGPL + ' */\n', 1))
    res.append(c3('check 3 MIT-only file', 'src/mit.c', '/* Permission is hereby granted, free of charge */\n', 1))
    res.append(c3('check 3 GPL-2.0-or-later without the banner', 'src/up.c', '/*\n * Copyright (C) 2003 X\n *\n' + GPL2P + ' */\n', 0))
    res.append(c3('check 3 SPDX GPL-2.0-or-later', 'src/spdx.sh',
                  '#!/bin/sh\n# Custom firmware for the iPod Video, 5th generation\n'
                  '# SPDX-License-Identifier: GPL-2.0-or-later\n', 0))
    # fail closed: no table in docs/legal.md
    write('docs/legal.md', LEGAL.replace(LIC_TABLE, ''))
    res.append(expect('check 3 licence table missing', 1, 'audit3-licences'))
    write('docs/legal.md', LEGAL)
    # 5: missing licence file
    os.remove(os.path.join(FX, 'lib/foo/COPYING'))
    res.append(expect('check 5 missing lib licence', 1, 'lib/foo/COPYING'))
    write('lib/foo/COPYING', 'GNU GENERAL PUBLIC LICENSE\n')
    # 6: asset names, lang
    res.append(expect('check 6 --asset-names', 1, 'iPod-Firmware.zip', ['--asset-names', 'ProkBox5g-1.zip', 'iPod-Firmware.zip']))
    write('apps/lang/english.lang', LANG)
    res.append(expect('check 6 lang string', 1, 'LANG_IPODUI_BAD'))
    write('apps/lang/english.lang', '')
    # 7: unlisted png, docs/reference
    write('art/bad/evil.png', 'x')
    res.append(expect('check 7 unlisted png', 1, 'art/bad/evil.png'))
    os.remove(os.path.join(FX, 'art/bad/evil.png'))
    write('docs/reference/7g/shot.png', 'x')
    res.append(expect('check 7 docs/reference binary', 1, 'docs/reference/7g/shot.png'))
    os.remove(os.path.join(FX, 'docs/reference/7g/shot.png'))
    # 8: Apple firmware window found in an added binary
    fw = os.path.join(FX, 'fwdir'); os.makedirs(fw)
    blob = bytes((i * 7 + i // 256) % 251 for i in range(8192))
    open(os.path.join(fw, 'apple.bin'), 'wb').write(blob)
    open(os.path.join(FX, 'art/ok/emb.png'), 'wb').write(b'\x89PNG' + b'\0' * 508 + blob + b'tail')
    sh('git', 'add', '-A'); sh('git', 'commit', '-qm', 'emb')
    res.append(expect('check 8 --apple-fw', 1, 'matches Apple firmware', ['--apple-fw', fw]))
    # --report never fails
    res.append(expect('--report exits 0', 0, '== summary', ['--report']))
    shutil.rmtree(FX, ignore_errors=True)
    print('ALL OK' if all(res) else 'SOME FAILED')
    return 0 if all(res) else 1


if __name__ == '__main__':
    sys.exit(main())
