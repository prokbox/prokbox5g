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

# The visualiser's FFT for the BCM2722's vector unit (#126,
# docs/design/viz-fft-vc.md): the definition (vfft_ref.c), the kernel on a
# model of the vector unit (vfft_vpu.c), today's reference (bcmfw/jobs/
# jviz.c) and a double-precision DFT.  The forms check reads
# tools/bcmre/proven_forms.txt.
set -e
cd "$(dirname "$0")/../.."
mkdir -p build/vizfft
cc -std=gnu99 -O2 -Wall -Wextra -Werror -Itests/vizfft -Iapps/ipodui -Ibcmfw/jobs \
   tests/vizfft/vfft_test.c tests/vizfft/vfft_ref.c tests/vizfft/vfft_vpu.c \
   tests/vizfft/vfft_tabs.c apps/ipodui/viz_core.c bcmfw/jobs/jviz.c -lm \
   -o build/vizfft/vfft_test
build/vizfft/vfft_test tools/bcmre/proven_forms.txt
