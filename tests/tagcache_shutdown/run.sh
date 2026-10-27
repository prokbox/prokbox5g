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

# Actual production shutdown/event/cancellation functions with a tiny scheduler,
# and the generation retry/deferral/abandon paths (#85 findings 1, 3, 4).
set -eu
cd "$(dirname "$0")/../.."
python3 tests/tagcache_shutdown/test_shutdown.py
python3 tests/tagcache_shutdown/test_retry.py
python3 tests/tagcache_shutdown/test_export.py
python3 tests/tagcache_shutdown/test_changelog.py
python3 tests/tagcache_shutdown/test_export_damage.py
