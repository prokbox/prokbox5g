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

# Host test of scripts/bench_ingest.py (the post-bench turnaround tool)
# over synthesized pulled folders: the fixtures must match their generator,
# then test_ingest.py runs the tool on each, isolated (python3 -I)
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
python3 -I tests/bench_ingest/gen_fixtures.py "$TMP/fx" || exit 1
if ! diff -r tests/bench_ingest/fixtures "$TMP/fx" >/dev/null; then
    echo "bench_ingest: fixtures/ is stale; run tests/bench_ingest/gen_fixtures.py tests/bench_ingest/fixtures"
    exit 1
fi
python3 -I tests/bench_ingest/test_ingest.py
