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

# scripts/version.sh and the release_build.sh version guard, against throw-away
# git repos under build/test-version (no tag is ever created in the real repo).
# Every case pins the literal expected string.
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
W=$ROOT/build/test-version
rm -rf "$W"; mkdir -p "$W"
V=$ROOT/scripts/version.sh
fail=0; n=0
ck() {  # name expected actual
    n=$((n + 1))
    if [ "$2" = "$3" ]; then echo "  ok   $1: $3"
    else echo "  FAIL $1: expected '$2' got '$3'"; fail=1; fi
}
# $1 = dir. sha of HEAD, 10 chars
sha() { git -C "$1" rev-parse --short=10 HEAD; }
mk() {
    rm -rf "$W/r"; mkdir -p "$W/r"; R=$W/r
    git -C "$R" init -q . && git -C "$R" config user.email t@t && git -C "$R" config user.name t
    git -C "$R" config commit.gpgsign false; git -C "$R" config tag.gpgsign false
    echo a >"$R/f"; git -C "$R" add f; git -C "$R" commit -qm one
}
more() { echo "$1" >>"$R/f"; git -C "$R" commit -qam "$1"; }

mk
ck "no tag, 1 commit" "0.0.0-dev.1+g$(sha $R)" "$($V $R)"
git -C "$R" tag v3.5.1-final
ck "upstream-style tag ignored" "0.0.0-dev.1+g$(sha $R)" "$($V $R)"
git -C "$R" tag v1.2.3
ck "at tag" "1.2.3" "$($V $R)"
ck "at tag, --tag-only" "1.2.3" "$($V --tag-only $R)"
more two; more three
ck "after tag" "1.2.4-dev.2+g$(sha $R)" "$($V $R)"
ck "after tag, --short" "1.2.4-dev.2" "$($V --short $R)"
$V --tag-only $R >"$W/out" 2>"$W/err"; rc=$?
ck "after tag, --tag-only fails (rc)" "1" "$rc"
ck "after tag, --tag-only prints nothing" "" "$(cat $W/out)"
echo x >>"$R/f"
ck "dirty" "1.2.4-dev.2+g$(sha $R)-dirty" "$($V $R)"
$V --tag-only $R >/dev/null 2>&1; ck "dirty, --tag-only fails (rc)" "1" "$?"
git -C "$R" checkout -q f
ck "reproducible (same commit twice)" "$($V $R)" "$($V $R)"
echo untracked >"$R/new"
ck "untracked file is not dirty" "1.2.4-dev.2+g$(sha $R)" "$($V $R)"
rm "$R/new"
git -C "$R" tag v1.2.4-rc.1 HEAD~1
ck "nearest tag wins (rc.1 is one commit back, v1.2.3 two)" "1.2.4-rc.1.dev.1+g$(sha $R)" "$($V $R)"

mk; git -C "$R" tag v1.0.0-rc.1
ck "pre-release tag" "1.0.0-rc.1" "$($V $R)"
ck "pre-release tag, --tag-only" "1.0.0-rc.1" "$($V --tag-only $R)"
more two
ck "after pre-release tag" "1.0.0-rc.1.dev.1+g$(sha $R)" "$($V $R)"
git -C "$R" tag v1.0.0-rc.2; git -C "$R" tag v1.0.0
ck "final outranks rc on one commit" "1.0.0" "$($V $R)"
git -C "$R" tag v0.9.0 HEAD~1
ck "highest tag at the commit wins" "1.0.0" "$($V $R)"

# a tag on another branch is not an ancestor
mk; git -C "$R" tag v2.0.0; git -C "$R" checkout -q -b side; more s1; git -C "$R" tag v2.1.0
git -C "$R" checkout -q -; more m1
ck "unmerged branch tag ignored" "2.0.1-dev.1+g$(sha $R)" "$($V $R)"

# no git: the VERSION file (the public tree)
mkdir -p "$W/tar"; echo "0.9.0" >"$W/tar/VERSION"
ck "no git, VERSION file" "0.9.0" "$($V $W/tar)"
ck "no git, VERSION file, --tag-only" "0.9.0" "$($V --tag-only $W/tar)"
echo "0.9.1-dev.3+gabc" >"$W/tar/VERSION"
$V --tag-only $W/tar >/dev/null 2>&1; ck "VERSION with + is not a tag" "1" "$?"
echo "nonsense" >"$W/tar/VERSION"
$V $W/tar >/dev/null 2>&1; ck "bad VERSION file refused (rc)" "2" "$?"
rm "$W/tar/VERSION"
$V $W/tar >/dev/null 2>&1; ck "no git, no VERSION: refused (rc)" "2" "$?"
# a VERSION file beats the checkout's tags (a public checkout has its own tags)
mk; git -C "$R" tag v5.0.0; echo 5.0.1 >"$R/VERSION"
ck "VERSION file first" "5.0.1" "$($V $R)"
# a tarball unpacked inside someone else's checkout must not read that checkout
mkdir -p "$R/sub"
$V "$R/sub" >/dev/null 2>&1; ck "subdirectory of a checkout is not the checkout (rc)" "2" "$?"

# tools/genversion.sh and the build id still work for stock builds
mkdir -p "$W/gv"
sh "$ROOT/tools/genversion.sh" "$W/gv" abc123-261111 >/dev/null
ck "genversion RBVERSION" '#define RBVERSION "abc123-261111"' "$(grep RBVERSION "$W/gv/rbversion.h")"
ck "genversion PROKVERSION default" '#define PROKVERSION "unknown"' "$(grep PROKVERSION "$W/gv/rbversion.h")"
sh "$ROOT/tools/genversion.sh" "$W/gv" abc123-261111 1.2.3-dev.4 >/dev/null
ck "genversion PROKVERSION" '#define PROKVERSION "1.2.3-dev.4"' "$(grep PROKVERSION "$W/gv/rbversion.h")"

# release_build.sh refuses a VERSION that disagrees with the tag at HEAD, and
# RELEASE_BUILD=1 refuses anywhere but a clean tag. Run from a copy of the
# scripts inside a throw-away repo; they must stop before building.
mk; mkdir -p "$R/scripts"
cp "$ROOT/scripts/release_build.sh" "$ROOT/scripts/version.sh" "$ROOT/scripts/release_flags.sh" "$R/scripts/"
git -C "$R" add scripts; git -C "$R" commit -qm scripts; git -C "$R" tag v0.8.2
out=$(cd "$R" && VERSION=0.8.3 sh scripts/release_build.sh 64 "$W/o" 2>&1); rc=$?
ck "override disagrees with tag: rc" "2" "$rc"
case "$out" in *"VERSION=0.8.3 disagrees with the tag at HEAD (v0.8.2)"*) ck "override disagrees: message" y y ;; *) ck "override disagrees: message" y "$out" ;; esac
more later

# rockbox-info "Build:" (RBVERSION) must stay the Rockbox build id even in a
# release: evaluate firmware.make's own SVNVERSION/PROKVERSION lines with the
# environment release_build.sh leaves for make (PROKBOX_VERSION set, VERSION unset).
mkdir -p "$W/mk"
grep -E '^(SVNVERSION|PROKVERSION):=' "$ROOT/firmware/firmware.make" >"$W/mk/v.mk"
printf 'all:\n\t@echo "$(SVNVERSION)|$(PROKVERSION)"\n' >>"$W/mk/v.mk"
mkout=$(env -u VERSION PROKBOX_VERSION=1.2.3 make -s -f "$W/mk/v.mk" ROOTDIR="$ROOT" TOOLSDIR="$ROOT/tools" 2>&1)
case "$mkout" in
    *"|1.2.3") ck "release: PROKVERSION is the release version" y y ;;
    *) ck "release: PROKVERSION is the release version" "...|1.2.3" "$mkout" ;;
esac
bid=${mkout%|*}
case "$bid" in
    1.2.3*) ck "release: RBVERSION (Build:) stays <id>-<date>" "<id>-<yymmdd>" "$bid" ;;
    [0-9a-f]*-[0-9][0-9][0-9][0-9][0-9][0-9]) ck "release: RBVERSION (Build:) stays <id>-<date>" y y ;;
    *) ck "release: RBVERSION (Build:) stays <id>-<date>" "<id>-<yymmdd>" "$bid" ;;
esac
out=$(cd "$R" && RELEASE_BUILD=1 sh scripts/release_build.sh 64 "$W/o" 2>&1); rc=$?
ck "RELEASE_BUILD=1 off a tag: rc" "2" "$rc"
case "$out" in *"not at a clean release tag"*) ck "RELEASE_BUILD=1 off a tag: message" y y ;; *) ck "RELEASE_BUILD=1 off a tag: message" y "$out" ;; esac

echo "$n checks"
[ "$n" -ge 30 ] || { echo "FAIL: only $n checks ran"; fail=1; }
[ $fail = 0 ] && echo "version tests passed" || { echo "VERSION TESTS FAILED"; exit 1; }
