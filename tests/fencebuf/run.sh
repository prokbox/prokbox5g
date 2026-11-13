#!/bin/sh
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

# Host test (#290) of the COP co-location's remote reader in the REAL
# apps/buffering.c on a two-cache model: tests/fencebuf/test_fencebuf.c.
#   tests/fencebuf/run.sh          the real file must pass, then each
#                                  mutant below (one deleted fence or
#                                  reordered publish, applied to a copy)
#                                  must FAIL the test: a surviving mutant
#                                  fails this script, and so does one
#                                  whose edit no longer applies (the
#                                  source moved: re-aim it). ~15 s.
#   tests/fencebuf/run.sh --real   the real file only
# Compiler: the newest Homebrew gcc-NN, else cc; override with HOSTCC=...
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
T=tests/fencebuf
MUTATE=1
[ "${1:-}" = "--real" ] && MUTATE=0

if [ -z "${HOSTCC:-}" ]; then
    HOSTCC=cc
    for v in 19 18 17 16 15 14; do
        if command -v gcc-$v >/dev/null 2>&1; then HOSTCC=gcc-$v; break; fi
    done
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# build_and_run DIR: buffering.c in DIR (beside it its quoted includes find
# tests/fencebuf/stub before the real headers); prints the test's output
# to DIR/out.log; returns the test's exit (2: did not compile)
build_and_run() {
    # shellcheck disable=SC2086
    if ! "$HOSTCC" -std=gnu99 -O1 -g -Wall -Wextra -Werror \
            -Wno-unused-function -Wno-unused-variable ${EXTRA_CFLAGS:-} \
            -I"$1" -I$T/stub -o "$1/test_fencebuf" $T/test_fencebuf.c \
            firmware/common/linked_list.c 2>"$1/cc.log"; then
        return 2
    fi
    (cd "$1" && ./test_fencebuf fencebuf.bin >out.log 2>&1)
}

mkdir -p "$TMP/real"
cp apps/buffering.c apps/buffering.h "$TMP/real/"
build_and_run "$TMP/real"
rc=$?
if [ $rc -eq 2 ]; then
    echo "FAIL: compiling test_fencebuf ($HOSTCC)"
    cat "$TMP/real/cc.log"
    exit 1
fi
if [ $rc -ne 0 ]; then
    cat "$TMP/real/out.log"
    echo "FAIL: fencebuf (real buffering.c)"
    exit 1
fi
grep -q "^fencebuf: [1-9][0-9]* checks, 0 failures$" "$TMP/real/out.log" || {
    cat "$TMP/real/out.log"
    echo "FAIL: fencebuf printed no verdict"
    exit 1
}
grep "^     " "$TMP/real/out.log"
echo "ok   fencebuf ($(sed -n 's/^fencebuf: \([0-9]*\) checks.*/\1/p' \
    "$TMP/real/out.log") checks: the real buffering.c remote reader on a two-cache model)"
[ $MUTATE -eq 1 ] || exit 0

# Each mutant: name | perl substitution on buffering.c (one fence deleted
# or one publish reordered). The substitution must change the file.
MUTANTS='
publish-no-writeback|s/(remote->hid != h->id\)\n        return;\n)    commit_dcache\(\);\n/$1/
publish-end-before-writeback|s/(remote->hid != h->id\)\n        return;\n)    commit_dcache\(\);\n(    remote->filesize = \(uint32_t\)h->filesize;\n    remote->end = \(uint32_t\)h->end;\n)/$1$2    commit_dcache();\n/
attach-live-before-publish|s/(    remote_publish\(h\);[^\n]*\n)(    r->live = 1;[^\n]*\n)/$2$1/
attach-live-before-words|s/(    r->hid      = handle_id;\n)((?:.*\n){6}    remote_publish\(h\);[^\n]*\n)    r->live = 1;[^\n]*\n/    r->live = 1;\n$1$2/
attach-no-publish|s/    remote_publish\(h\);          \/\* write-back, filesize, end \*\/\n//
attach-remote-after-publish|s/(    remote = r;\n)(    remote_publish\(h\);[^\n]*\n)/$2$1/
attach-no-mutex|s/(    r->live = 0;\n)    mutex_lock\(&llist_mutex\);\n(    remote_apply\(\);\n    remote_drop\(\);\n)/$1$2/
detach-drop-before-apply|s/(        remote_apply\(\);\n)((?:.*\n){2})(        remote_drop\(\);\n)/$3$2/
detach-no-apply|s/(    if \(remote == r\)\n    \{\n        struct memory_handle \*h;\n\n)        remote_apply\(\);\n/$1/
detach-no-clear-live|s/(        remote_drop\(\);\n    \}\n)    r->live = 0;\n/$1/
drop-no-clear-live|s/(    if \(!remote\)\n        return;\n)    remote->live = 0;\n(    remote = NULL;)/$1$2/
close-no-drop|s/            remote_drop\(\);      \/\* ProkBox: never expected while read \*\/\n/            (void)0;\n/
rebuffer-no-drop|s/(    if \(remote && remote->hid == handle_id\)\n)        remote_drop\(\);\n(#endif\n    \/\* Reset the handle)/$1        (void)0;\n$2/
fill-no-publish|s/        remote_publish\(h\);      \/\* ProkBox: the other core.s reader \*\/\n//
fill-short-no-publish|s/(            h->filesize = h->end;\n)            remote_publish\(h\);\n(            break;)/$1$2/
reopen-no-publish|s/(            h->filesize = h->end;\n)            remote_publish\(h\);\n(            return true;)/$1$2/
strip-no-publish|s/(    h->filesize = size;\n)    remote_publish\(h\);          \/\* ProkBox \*\/\n/$1/
counters-no-apply|s/(    mutex_lock\(&llist_mutex\);\n\n)    remote_apply\(\);             \/\* ProkBox: the other core.s reads \*\/\n(\n    int num = num_handles;)/$1$2/
shrink-no-apply|s/(    mutex_lock\(&llist_mutex\);\n\n)    remote_apply\(\);             \/\* ProkBox: the other core.s reads \*\/\n(\n    for \(struct memory_handle \*h = HLIST_LAST)/$1$2/
'

n=0; red=0; bad=0
printf "%s\n" "$MUTANTS" | while IFS='|' read -r name expr; do
    [ -n "$name" ] || continue
    d="$TMP/m-$name"
    mkdir -p "$d"
    cp apps/buffering.h "$d/"
    perl -0pe "$expr" apps/buffering.c >"$d/buffering.c"
    if cmp -s apps/buffering.c "$d/buffering.c"; then
        echo "FAIL mutant $name: the edit did not apply (source moved?)"
        echo x >>"$TMP/bad"
        continue
    fi
    build_and_run "$d"
    rc=$?
    if [ $rc -eq 2 ]; then
        echo "FAIL mutant $name: does not compile"
        sed -n 1,5p "$d/cc.log"
        echo x >>"$TMP/bad"
    elif [ $rc -eq 0 ]; then
        echo "FAIL mutant $name: SURVIVED (no check went red)"
        echo x >>"$TMP/bad"
    elif ! grep -q "^fencebuf: [0-9]* checks, [1-9][0-9]* failures$" "$d/out.log"; then
        echo "FAIL mutant $name: the test crashed, no verdict"
        tail -3 "$d/out.log"
        echo x >>"$TMP/bad"
    else
        echo "red  mutant $name: $(grep '^FAIL' "$d/out.log" | head -1 | cut -c6-)"
        echo x >>"$TMP/red"
    fi
    echo x >>"$TMP/n"
done
n=$(cat "$TMP/n" 2>/dev/null | wc -l | tr -d ' ')
red=$(cat "$TMP/red" 2>/dev/null | wc -l | tr -d ' ')
bad=$(cat "$TMP/bad" 2>/dev/null | wc -l | tr -d ' ')
if [ "$n" -eq 0 ] || [ "$bad" -ne 0 ] || [ "$red" -ne "$n" ]; then
    echo "FAIL: fencebuf mutants: $red of $n red, $bad bad"
    exit 1
fi
echo "ok   fencebuf mutants: $red of $n red (each one deleted fence or reordered publish fails the test)"
