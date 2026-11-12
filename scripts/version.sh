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

# The one source of the ProkBox5g version: git tags (docs/process.md,
# "Versions"). usage: scripts/version.sh [--tag-only] [--short] [SRC-ROOT]
#
#   at a release tag vX.Y.Z[-rc.N]   X.Y.Z[-rc.N]
#   after a final tag vX.Y.Z         X.Y.(Z+1)-dev.N+g<sha>
#   after a pre-release tag          X.Y.Z-rc.N.dev.M+g<sha>
#   no release tag at all            0.0.0-dev.N+g<sha>
#   any dev form on a dirty tree     ...+g<sha>-dirty (a tag + edits too)
#   a VERSION file in SRC-ROOT       its contents, checked first: release_tree.py
#                                    writes it into the public tree (a squashed
#                                    import with its own tags, or a tarball);
#                                    the private repo never tracks one
#
# N/M = commits since the tag. Only tags vX.Y.Z, vX.Y.Z-alpha[.N],
# -beta[.N] and -rc.N count (upstream Rockbox's v3.5.1-final etc. are
# ignored). The same commit always gives the same string (no date, no
# clock). --tag-only: exit 1, printing nothing, unless the string is an
# exact release (HEAD at a release tag with a clean tree, or a VERSION
# file without "+"). --short: drop the "+g<sha>[-dirty]" build metadata
# (what the device's About page shows). A failure prints to stderr and
# exits 2; a build never gets an empty version.
TAGRE='^v[0-9]+\.[0-9]+\.[0-9]+(-(alpha|beta|rc)(\.[0-9]+)?)?$'
TAGONLY=0; SHORT=0; TOP=
for a in "$@"; do
    case "$a" in
        --tag-only) TAGONLY=1 ;;
        --short) SHORT=1 ;;
        -*) echo "version.sh: unknown option $a" >&2; exit 2 ;;
        *) TOP=$a ;;
    esac
done
[ -n "$TOP" ] || TOP=$(cd "$(dirname "$0")/.." && pwd)

# sortable key: a final release outranks its own pre-releases
tagkey() {
    t=${1#v}; core=${t%%-*}; pre=
    [ "$core" != "$t" ] && pre=${t#*-}
    oldifs=$IFS; IFS=.; set -- $core; IFS=$oldifs
    printf '%05d%05d%05d-' "$1" "$2" "$3"
    if [ -z "$pre" ]; then printf 'zzz\n'; else printf '%s\n' "$pre"; fi
}

emit() {  # $1 = version
    v=$1
    if [ $TAGONLY = 1 ]; then
        case "$v" in *+*) echo "version.sh: --tag-only: not at a release tag ($v)" >&2; exit 1 ;; esac
    fi
    [ $SHORT = 1 ] && v=${v%%+*}
    echo "$v"; exit 0
}

if [ -r "$TOP/VERSION" ]; then
    v=$(head -1 "$TOP/VERSION" | tr -d ' \r\n')
    echo "$v" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?(\+[0-9A-Za-z.-]+)?$' \
        || { echo "version.sh: $TOP/VERSION does not hold a version ($v)" >&2; exit 2; }
    emit "$v"
fi

top_real=$(cd "$TOP" 2>/dev/null && pwd -P) || { echo "version.sh: no such directory $TOP" >&2; exit 2; }
git_top=$(git -C "$TOP" rev-parse --show-toplevel 2>/dev/null) && git_top=$(cd "$git_top" && pwd -P)

if [ "$git_top" = "$top_real" ] && git -C "$TOP" rev-parse --verify -q HEAD >/dev/null; then
    sha=$(git -C "$TOP" rev-parse --short=10 HEAD)
    dirty=
    [ -n "$(git -C "$TOP" status --porcelain --untracked-files=no 2>/dev/null)" ] && dirty=-dirty
    best=; bestn=; bestkey=
    for t in $(git -C "$TOP" tag --merged HEAD | grep -E "$TAGRE"); do
        n=$(git -C "$TOP" rev-list --count "$t"..HEAD) || exit 2
        k=$(tagkey "$t")
        if [ -z "$best" ] || [ "$n" -lt "$bestn" ] || { [ "$n" -eq "$bestn" ] && [ "$k" \> "$bestkey" ]; }; then
            best=$t; bestn=$n; bestkey=$k
        fi
    done
    if [ -z "$best" ]; then
        n=$(git -C "$TOP" rev-list --count HEAD)
        emit "0.0.0-dev.$n+g$sha$dirty"
    fi
    ver=${best#v}
    if [ "$bestn" -eq 0 ]; then
        [ -n "$dirty" ] && emit "$ver+g$sha$dirty"
        emit "$ver"
    fi
    case "$ver" in
        *-*) emit "$ver.dev.$bestn+g$sha$dirty" ;;
        *) oldifs=$IFS; IFS=.; set -- $ver; IFS=$oldifs
           emit "$1.$2.$(($3 + 1))-dev.$bestn+g$sha$dirty" ;;
    esac
fi

echo "version.sh: $TOP is not a git checkout of this project and has no VERSION file" >&2
exit 2
