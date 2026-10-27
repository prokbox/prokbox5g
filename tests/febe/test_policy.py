#!/usr/bin/env python3
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

"""The UI boundary's audio-out rule (P19 ucop-fix2), checked on the lists.

The mixer, the PCM driver, the DSP and sound.c's settings are CPU-only:
their writers must cross by remote call, never be "direct:" in
apps/ipodui/febe_rpc.list, and the link check's "audio out" family must
cover them so a new direct call fails febe_link_check --strict.
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'scripts'))
import febe_link_check as lc  # noqa: E402

# reviewed direct reads in the family (uncached tables, one writer)
DIRECT_READS = {'mixer_channel_status', 'pcm_is_playing'}

# writers the shell calls (or did): each must be in the family
WRITERS = ['mixer_set_tap', 'mixer_channel_play_data', 'mixer_channel_stop',
           'dsp_set_eq_coefs', 'dsp_set_eq_precut', 'dsp_eq_enable',
           'dsp_set_timestretch', 'dsp_timestretch_enable',
           'sound_set_volume', 'sound_set_pitch', 'sound_set',
           'sound_settings_apply', 'pcm_play_data', 'beep_play']

fails = []
for w in WRITERS:
    if lc.family(w) is None:
        fails.append('%s is in no back-end family' % w)
wrapped, direct = lc.rpc_list()
for d in direct:
    if lc.family(d) == 'audio out' and d not in DIRECT_READS:
        fails.append('direct: %s writes audio-out state from the COP' % d)
for need in ('viz_tap_open', 'viz_tap_close', 'mixer_set_tap',
             'ui_eq_dsp_set'):
    if need not in wrapped:
        fails.append('%s does not cross by remote call' % need)
for o in ('viz_tap.o', 'audio_out.o', 'click.o'):
    if o not in lc.SHELL_BE_OBJECTS:
        fails.append('%s is not a CPU-only shell object' % o)
for f in fails:
    print('FAIL: ' + f)
sys.exit(1 if fails else 0)
