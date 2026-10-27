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

# Host test of the USB screen's sustained-transfer detector (#58, owner:
# the arrows show only for sustained transfers, not for a host's background
# reads; apps/ipodui/usbscreen.h ui_usb_io_step()).
# Compiler: the newest Homebrew gcc-NN, else cc; override with HOSTCC=...
# Extra flags (e.g. sanitizers): EXTRA_CFLAGS="-fsanitize=address,undefined"
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
T=tests/usbio

if [ -z "${HOSTCC:-}" ]; then
    HOSTCC=cc
    for v in 19 18 17 16 15 14; do
        if command -v gcc-$v >/dev/null 2>&1; then HOSTCC=gcc-$v; break; fi
    done
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
# shellcheck disable=SC2086
if ! "$HOSTCC" -std=gnu99 -O1 -g -Wall -Wextra -Werror ${EXTRA_CFLAGS:-} \
        -Iapps -o "$TMP/test_usbio" "$T/test_usbio.c" 2>"$TMP/cc.log"; then
    echo "FAIL: compiling test_usbio ($HOSTCC)"
    cat "$TMP/cc.log"
    exit 1
fi
if "$TMP/test_usbio"; then
    echo "ok   USB sustained-transfer detector (usbscreen.h)"
else
    echo "FAIL: USB sustained-transfer detector"
    exit 1
fi
