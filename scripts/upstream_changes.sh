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

# Print the mechanical list of every upstream-Rockbox file we change, for the
# ledger docs/changes-from-rockbox.md. Re-run at every phase close and compare.
#
#   scripts/upstream_changes.sh                 # main vs the merge-base with upstream/master
#   scripts/upstream_changes.sh p11             # another ref (branch, tag, commit)
#   scripts/upstream_changes.sh main p11        # also list files whose stat differs between two refs
#   scripts/upstream_changes.sh --names [ref]   # name-status (A/M/D) instead of the stat
#
# Excluded: the shell (apps/ipodui), docs, tests, scripts, art and .claude,
# which are ours alone and never conflict with upstream.
set -e
cd "$(git rev-parse --show-toplevel)"

mode=stat
if [ "$1" = "--names" ]; then mode=names; shift; fi
ref=${1:-main}
other=$2

base=$(git merge-base "$ref" upstream/master)
echo "# ref=$ref  upstream/master=$(git rev-parse --short upstream/master)  merge-base=$(git rev-parse --short "$base")"

excl=". :!apps/ipodui :!docs :!tests :!scripts :!art :!.claude"
# shellcheck disable=SC2086
if [ "$mode" = names ]; then
    git diff --name-status "$base" "$ref" -- $excl
else
    git diff --stat=200 "$base" "$ref" -- $excl
fi

if [ -n "$other" ]; then
    echo
    echo "# files that differ between $ref and $other (the '(<branch>, merging)' rows)"
    # shellcheck disable=SC2086
    git diff --stat=200 "$ref" "$other" -- $excl
fi
