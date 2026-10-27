#!/usr/bin/env python3
"""Self-test for scripts/legal_audit.py: plants defects in a throwaway git
repo under build/legal-fixture and checks each is caught (and the clean
fixture passes).  usage: python3 tests/legal/test_legal_audit.py"""
import os, shutil, subprocess, sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
AUDIT = os.path.join(REPO, 'scripts/legal_audit.py')
FX = os.path.join(REPO, 'build/legal-fixture')
HDR = '/* Copyright (C) 2003 Alice Upstream\n * Licensed under the GNU General Public License v2\n * WITHOUT WARRANTY\n */\nint x;\n'
PBX = '/* Custom firmware for the iPod Video, 5th generation\n * GNU General Public License */\nint y;\n'
LEGAL = '## 5. Asset provenance\n\n| Path | Origin | Licence |\n|---|---|---|\n| `art/ok/*.png` | gen | GPL |\n\n## 6. x\n'
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
