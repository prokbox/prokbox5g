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

# PCM DMA lengths and addresses (issue #191): every transfer the PP502x
# playback path programs into DMA0 is whole 32-bit words, at a word address.
# Extracts the real code from firmware/target/arm/pp/pcm-pp.c (no copy kept
# here) and runs it against a register model; see test_pcmdma.c.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
cd "$ROOT"
T=tests/pcmdma
SRC=${PCMDMA_SRC:-firmware/target/arm/pp/pcm-pp.c}
if [ -z "${HOSTCC:-}" ]; then
    HOSTCC=cc
    for v in 19 18 17 16 15 14; do
        if command -v gcc-$v >/dev/null 2>&1; then HOSTCC=gcc-$v; break; fi
    done
fi
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# extract NAME REGEX END: the first line matching REGEX through the first
# later line matching END (NOBS: the first line not ending in a backslash);
# an empty result fails the test
fail=0
extract() {
    awk -v re="$2" -v end="$3" '
        !on && $0 ~ re { on = 1 }
        on { print
             if (end == "NOBS") { if (substr($0, length($0)) != "\\") exit }
             else if ($0 ~ end) exit }
    ' "$4" >"$TMP/part"
    if [ ! -s "$TMP/part" ]; then
        echo "FAIL: $1 not found in $4 (the extraction must see the real code)"
        fail=1
        return
    fi
    cat "$TMP/part" >>"$TMP/pcmpp_parts.c"
    echo >>"$TMP/pcmpp_parts.c"
}
: >"$TMP/pcmpp_parts.c"
extract "struct dma_data"        '^struct dma_data$'                         '^};'  "$SRC"
extract "dma_play_data"          '^static struct dma_data dma_play_data'     '^};'  "$SRC"
extract "max_dma_chunk_size"     '^static uint16_t max_dma_chunk_size;'      ';'    "$SRC"
extract "DMA_PLAY_CONFIG"        '^#define DMA_PLAY_CONFIG'                  NOBS "$SRC"
extract "dma_tx_buf_prepare"     '^static inline unsigned long dma_tx_buf_prepare' '^}' "$SRC"
extract "dma_tx_start"           '^static inline void dma_tx_start[(]bool begin[)]$' '^}' "$SRC"
extract "fiq_playback (PP502x)"  '^void fiq_playback[(]void[)]$'               '^}'   "$SRC"
extract "sink_set_freq"          '^static void sink_set_freq'                '^}'   "$SRC"
extract "sink_start_pcm"         '^static void sink_start_pcm'               '^}'   "$SRC"
extract "sink_dma_start"         '^static void sink_dma_start[(]const void'   '^}'   "$SRC"
grep -E '^[[:space:]]*#define (DMA_CMD_|DMA_REQ_IIS)' firmware/export/pp5020.h \
    >"$TMP/dma_defs.h"
grep -A2 '^#define ALIGN_AUDIOBUF' firmware/export/pcm-internal.h \
    >"$TMP/align_audiobuf.h"
cp firmware/export/pcm_sampr.h "$TMP/"
{ echo '#ifndef HOST_CONFIG_H'; echo '#define HOST_CONFIG_H'
  awk '/^#define HW_SAMPR_CAPS/ { on = 1 } on { print } on && !/\\$/ { exit }' \
      firmware/export/config/ipodvideo.h
  echo '#endif'; } >"$TMP/config.h"
for f in dma_defs.h align_audiobuf.h config.h; do
    grep -q define "$TMP/$f" || { echo "FAIL: $f extracted empty"; fail=1; }
done
[ $fail -eq 0 ] || exit 1

CFLAGS="-std=gnu99 -O1 -g -Wall -Wextra -Werror -Wno-unused-function -Wno-override-init ${EXTRA_CFLAGS:-} -I$TMP"
# shellcheck disable=SC2086
if ! "$HOSTCC" $CFLAGS -include "$TMP/config.h" -o "$TMP/t" \
        "$T/test_pcmdma.c" firmware/pcm_sampr.c 2>"$TMP/cc.log"; then
    echo "FAIL: compiling test_pcmdma.c with the code extracted from $SRC ($HOSTCC)"
    cat "$TMP/cc.log"; exit 1
fi
"$TMP/t" || { echo "FAIL: PCM DMA lengths/addresses (issue #191)"; exit 1; }
