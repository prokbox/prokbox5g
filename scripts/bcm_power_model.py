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

"""The BCM's share of lit current under the release line, and the
hibernate break-even: a small model with every number cited and tagged.

Release line: --ipodui --bcmfw --bcmfw-default --bcmfw-l1 --bcmfw-merged
(no player; the job driver; the merged image resident once verified).

Cases, over one typical lit hour (the mix in PARAMS "mix"):
  A    jobs on: the drift, slides, glides and the marquee drawn on the BCM
       (our merged image, busy-polling between jobs as today);
  B1   the CPU drift: everything drawn on the PP, our image as the LCD
       driver (the same idle as A);
  B2   the CPU drift with Apple's vmcs as the LCD driver (its power
       manager: 5 MHz floor, level 5, sleeping between commands);
  A+s  A with the idle sleep (stage 5i) and level 5 (PW3): what the two
       open BCM questions are worth;
  A+24 A with the lit clock at 24 MHz once the drift no longer pins 30.

Tags: [silicon] measured on the unit; [sim] vc2sim; [apple] Apple's code;
[estimate] ours, with the range used for the sensitivity.  Nothing here
is a current measurement: no state's current has been read (#12).

P19 PW4 (--vclk): each level band's current, polling and asleep, and the
governor's estimated saving against fixed 150 MHz on the stage 6 clips and
the lit shell (scripts/vclk_gov_sim.py's workloads).

Usage: scripts/bcm_power_model.py [--json] [--md] [--vclk]
"""
import json
import sys

# name: (value, low, high, unit, tag, source)
PARAMS = {
    # ---- the battery and the rails
    'battery_mah': (3000, 2800, 3000, 'mAh', 'estimate',
                    'hardware.md: the 3000 mAh cell (unverified; the 28.7 h rundown at 105 mA '
                    'fits 3000, 98 mA fits 2800)'),
    'v_batt': (3.7, 3.6, 3.9, 'V', 'estimate',
               'the cell table mid-discharge (clocks-power.md section 6, 3,600-4,180 mV)'),
    'eta': (0.85, 0.80, 0.90, '', 'estimate', 'regulator efficiency, lit-24mhz.md A3'),
    'i_lit_total': (150.0, 130.0, 190.0, 'mA', 'estimate',
                    'lit load with music, lit-24mhz.md A2 (105 mA dark playback [silicon '
                    'rundown] + backlight, panel, BCM)'),
    'i_dark_play': (105.0, 98.0, 105.0, 'mA', 'silicon',
                    'hardware.md: 28.7 h rundown at playback load (3000 mAh assumed)'),
    # ---- the BCM's core
    'k_bcm': (0.55, 0.30, 1.00, 'mW/MHz', 'estimate',
              'BCM core busy (scalar) at 1.2 V: ~80 mW at 150 MHz, within the brief\'s '
              '"tens of mW" active (brief-map-power.md section 3, P2/P3 [external])'),
    'vec_factor': (1.6, 1.2, 2.5, 'x', 'estimate', 'vector-busy power over scalar-busy'),
    'sleep_factor': (0.25, 0.10, 0.40, 'x', 'estimate',
                     'core in `sleep` (clock tree on) over busy'),
    'p_leak11': (2.0, 0.5, 6.0, 'mW', 'estimate', 'BCM static at level 11 (PLLs off)'),
    'v_l11': (1.04, 1.04, 1.04, 'V', 'hypothesis',
              'level 11 if levels 5-15 span the brief\'s 0.8-1.2 V evenly (brief-map-power.md '
              'section 4); our images run at 11 [silicon S7]'),
    'v_l5': (0.80, 0.80, 0.80, 'V', 'hypothesis', 'level 5, Apple\'s for <= 46.9 MHz [apple]'),
    'f_ui': (27.0, 27.0, 27.0, 'MHz', 'silicon', 'our UI images run on the crystal (power-contract.md)'),
    'f_apple_idle': (5.0, 5.0, 12.0, 'MHz', 'apple',
                     'pm_set_policy min: a 5 MHz floor, load-governed (brief-map-power.md section 2)'),
    'apple_busy': (0.10, 0.05, 0.30, '', 'estimate', 'vmcs busy share while lit (LCD commands)'),
    'p_dma': (4.0, 2.0, 10.0, 'mW', 'estimate', 'BCM LCD DMA and panel bus while a present runs'),
    'present_full_ms': (14.3, 14.3, 14.3, 'ms', 'silicon', 'a full-frame present (S7)'),
    # ---- the port and the PP
    'e_pp_cyc_nj': (0.40, 0.20, 1.20, 'nJ', 'estimate',
                    'PP core energy per CPU cycle at the fixed 1.2 V (lit-24mhz.md A3: constant '
                    'per cycle)'),
    'pp_port_cyc': (10.5, 10.5, 10.5, 'cycles', 'silicon', 'PP cycles per 16-bit port transfer (S8)'),
    'e_pad_nj': (1.0, 0.3, 3.0, 'nJ', 'estimate',
                 'both chips\' pads per 16-bit port transfer (16 lines, ~10 pF, 3.3 V)'),
    'e_e2_nj': (0.8, 0.3, 2.0, 'nJ', 'estimate', 'engine 2 per transfer (DMA and bus), CPU free'),
    'lit24_ma': (2.0, 1.0, 3.0, 'mA', 'estimate',
                 'the lit clock at 24 MHz instead of 30 (lit-24mhz.md section 6)'),
    # ---- the drift (nav_core.h), per 7 s cover cycle [ours]
    'drift_fades': (8, 8, 8, '/cycle', 'silicon', 'NAV_DRIFT_FADE_STEPS'),
    'drift_pans': (44, 44, 44, '/cycle', 'silicon', 'NAV_DRIFT_PAN_MAX'),
    'drift_halves': (44, 44, 44, '/cycle', 'silicon', 'one half step per pan (8b)'),
    'drift_cycle_s': (7.0, 7.0, 7.0, 's', 'silicon', 'NAV_DRIFT_CYCLE'),
    # the CPU path per 160 x 240 frame (cycles)
    'cpu_fade': (1.19e6, 0.73e6, 1.19e6, 'cycles', 'estimate', 'xfade_rect, offload.md section 2 (static)'),
    'cpu_pan': (0.115e6, 0.08e6, 0.15e6, 'cycles', 'estimate', 'blit_window, offload.md section 2'),
    'cpu_half': (0.19e6, 0.12e6, 0.30e6, 'cycles', 'estimate', 'AVG2 two pixels a word (preview.c)'),
    'cpu_shadow': (33e3, 33e3, 33e3, 'cycles', 'estimate', 'ui_shadow_apply, hardware.md budget row 8b-3'),
    'drift_px': (38400, 38400, 38400, 'px', 'silicon', '160 x 240'),
    # the BCM path per frame, 27 MHz (jobs.md section 10, the cost audit)
    'bcm_fade_ms': (4.28, 3.78, 4.28, 'ms', 'sim', 'FADE raw (10 shaded columns)'),
    'bcm_pan_ms': (1.01, 0.71, 1.01, 'ms', 'sim', 'BLIT 0.77 out of phase + SHADE 0.24'),
    'bcm_half_ms': (5.19, 5.19, 5.19, 'ms', 'sim', 'AVG 4.95 + SHADE 0.24'),
    'bcm_commit_ms': (11.70, 11.70, 11.70, 'ms', 'sim', 'COMMIT a cover, F16w'),
    'cover_bytes': (182400, 182400, 182400, 'B', 'ours', 'a 300 x 300 cover at the 304 pitch'),
    'job_pp_cyc': (200, 200, 400, 'cycles', 'estimate', 'a record and its doorbell, offload.md section 3'),
    'job_xfers': (40, 26, 40, 'transfers', 'estimate', 'a record on the port, hostproto.md section 8'),
    # the marquee, 20 steps a second
    'mq_cpu_share': (0.05, 0.03, 0.08, 'of 30 MHz', 'estimate', 'F2 with its push (phase19 brief section 1A)'),
    'mq_bcm_ms': (0.26, 0.26, 0.26, 'ms', 'sim', 'stepped ANIM PAN, one frame'),
    'mq_hz': (20.0, 20.0, 20.0, '/s', 'silicon', 'MQ_STEP_TICKS 50 ms'),
    # slides (per slide) and glides (per frame)
    'slide_cpu_extra': (1.3e6, 0.9e6, 2.0e6, 'cycles', 'estimate',
                        'capture and swap (429 k) + 6 memcpys of 140.8 KB (jobs.md 12.8, '
                        'code-audit-1 F4); the new page\'s redraw is in both'),
    'slide_frames_cpu': (6, 6, 6, '', 'silicon', '~6 pushes of the rect (jobs.md 12.8)'),
    'slide_frames_bcm': (9, 9, 10, '', 'sim', 'every slide <= 10 frames at 33 ms'),
    'slide_bcm_ms': (7.83, 4.04, 11.62, 'ms', 'sim', 'mean of the retained-slide frames (jobs.md 10)'),
    'slide_bytes': (140800, 140800, 140800, 'B', 'silicon', '320 x 220 body'),
    'glide_bytes_cpu': (140800, 140800, 140800, 'B', 'silicon', 'a glide frame by engine 2'),
    'glide_bytes_bcm': (14400, 14400, 14400, 'B', 'sim', '17 rows + the bar (jobs.md 12.4)'),
    'glide_bcm_ms': (1.3, 1.3, 1.3, 'ms', 'sim', 'SCROLL, two BLITs, PRESENT set-up, FENCE'),
    'glide_fps': (30.0, 30.0, 30.0, '/s', 'silicon', 'a glide while scrolling'),
    # ---- the mix: one typical lit hour [estimate, ours]
    'mix_drift': (0.35, 0.15, 0.60, 'of lit time', 'estimate', 'main menu or a split screen, drifting'),
    'mix_marquee': (0.15, 0.05, 0.30, 'of lit time', 'estimate', 'a long title selected'),
    'mix_slides': (240, 120, 480, '/lit h', 'estimate', 'pushes and pops'),
    'mix_glide_s': (120, 60, 300, 's/lit h', 'estimate', 'scrolling'),
    # ---- the hibernate break-even (power-contract.md section 6.6)
    'i_hold_stop_ma': (0.2, 0.05, 1.0, 'mA', 'estimate',
                       'BCM held, its clocks stopped (PW5 "counter STOPPED"): retention leakage'),
    'i_panel_hold_ma': (1.0, 0.3, 2.5, 'mA', 'estimate', 'the panel powered, showing, backlight off'),
    'cold_s': (0.50, 0.37, 0.62, 's', 'silicon', 'cut to first frame (S7, S30)'),
    'hib_wake_s': (0.035, 0.020, 0.060, 's', 'sim',
                   'pins 5 ms + restore + second sums 13-23 ms at 27 MHz (hibwake_test) + one present'),
    'i_pp_wait_ma': (8.0, 3.0, 20.0, 'mA', 'estimate', 'the PP awake waiting through a cold wake'),
    'unlit_window_s': (90.0, 90.0, 90.0, 's', 'silicon', 'the unlit face window (power-contract.md 2)'),
    'dark_entries': (20, 5, 60, '/day', 'estimate', 'times the screen goes dark'),
}


def val(p, k):
    return p[k][0] if isinstance(p[k], tuple) else p[k]


def model(p):
    """Every result in mA (battery) or mAh, from the parameter values"""
    g = lambda k: val(p, k)
    to_ma = lambda mw: mw / g('eta') / g('v_batt')          # core-rail mW -> battery mA
    nj_s_to_ma = lambda nj_per_s: to_ma(nj_per_s * 1e-6)    # nJ/s -> mW -> mA
    v11, v5 = g('v_l11'), g('v_l5')
    s11, s5 = (v11 / 1.2) ** 2, (v5 / 1.2) ** 2
    poll11 = g('k_bcm') * g('f_ui') * s11                   # mW, busy-polling
    leak11, leak5 = g('p_leak11'), g('p_leak11') * (v5 / v11) ** 2
    hour = 3600.0

    # ---- duties over one lit hour (seconds of each activity)
    t_drift = g('mix_drift') * hour
    cycles = t_drift / g('drift_cycle_s')
    nf, npan, nhalf = (cycles * g(k) for k in ('drift_fades', 'drift_pans', 'drift_halves'))
    frames = nf + npan + nhalf
    t_mq = g('mix_marquee') * hour
    n_sl = g('mix_slides')
    n_gl = g('mix_glide_s') * g('glide_fps')

    # the BCM's vector-busy seconds in A
    bcm_job_s = (nf * g('bcm_fade_ms') + npan * g('bcm_pan_ms') + nhalf * g('bcm_half_ms') +
                 cycles * g('bcm_commit_ms') + t_mq * g('mq_hz') * g('mq_bcm_ms') +
                 n_sl * g('slide_frames_bcm') * g('slide_bcm_ms') + n_gl * g('glide_bcm_ms')) / 1e3
    # LCD present seconds: equal for the drift and glides; slides present more in A
    rect = 320 * 220 / (320 * 240)
    present_extra_a = n_sl * (g('slide_frames_bcm') - g('slide_frames_cpu')) * \
        g('present_full_ms') * rect / 1e3

    # ---- PP energy (nJ) per lit hour, the parts that differ
    px = g('drift_px')
    push = px * g('pp_port_cyc')
    pp_b = (nf * (g('cpu_fade') + g('cpu_shadow') + push) +
            npan * (g('cpu_pan') + g('cpu_shadow') + push) +
            nhalf * (g('cpu_half') + g('cpu_shadow') + push) +
            t_mq * g('mq_cpu_share') * 30e6 +
            n_sl * g('slide_cpu_extra')) * g('e_pp_cyc_nj')
    pads_b = (frames * px + n_sl * g('slide_frames_cpu') * g('slide_bytes') / 2 +
              n_gl * g('glide_bytes_cpu') / 2) * g('e_pad_nj')
    e2_b = (n_sl * g('slide_frames_cpu') * g('slide_bytes') / 2 +
            n_gl * g('glide_bytes_cpu') / 2) * g('e_e2_nj')
    rec = g('job_pp_cyc') + g('job_xfers') * g('pp_port_cyc')
    jobs_n = frames + n_sl * 2 + n_gl * 5
    pp_a = (jobs_n * rec + n_gl * 3500 / 2 * g('pp_port_cyc')) * g('e_pp_cyc_nj')
    up_xfers_a = cycles * g('cover_bytes') / 2 + n_sl * g('slide_bytes') / 2 + \
        n_gl * (g('glide_bytes_bcm') - 3500) / 2
    pads_a = (up_xfers_a + jobs_n * g('job_xfers') + n_gl * 3500 / 2) * g('e_pad_nj')
    e2_a = up_xfers_a * g('e_e2_nj')

    def ma(nj):
        return nj_s_to_ma(nj / hour)

    # ---- the BCM's own current
    bcm_idle_poll = to_ma(poll11 + leak11)                       # A, B1: always lit
    bcm_vec_extra = to_ma(poll11 * (g('vec_factor') - 1) * bcm_job_s / hour)
    bcm_dma_a = to_ma(g('p_dma') * present_extra_a / hour)
    apple = g('k_bcm') * g('f_apple_idle') * s5 * (g('apple_busy') +
                                                    (1 - g('apple_busy')) * g('sleep_factor'))
    bcm_b2 = to_ma(apple + leak5)
    # A with idle sleep (5i) and level 5 (PW3): busy only for jobs, asleep between
    busy_frac = bcm_job_s / hour
    p_as = g('k_bcm') * g('f_ui') * s5 * (busy_frac * g('vec_factor') +
                                           (1 - busy_frac) * g('sleep_factor')) + leak5
    bcm_a_s = to_ma(p_as)

    out = {}
    out['bcm_job_s_per_lit_h'] = bcm_job_s
    out['bcm_job_duty'] = busy_frac
    out['A'] = {'bcm': bcm_idle_poll + bcm_vec_extra + bcm_dma_a,
                'pp': ma(pp_a) + ma(e2_a), 'pads': ma(pads_a)}
    out['B1'] = {'bcm': bcm_idle_poll, 'pp': ma(pp_b) + ma(e2_b), 'pads': ma(pads_b)}
    out['B2'] = {'bcm': bcm_b2, 'pp': ma(pp_b) + ma(e2_b), 'pads': ma(pads_b)}
    out['A+s'] = {'bcm': bcm_a_s + bcm_dma_a, 'pp': ma(pp_a) + ma(e2_a), 'pads': ma(pads_a)}
    # the lit clock at 24 MHz: the drift was what pinned 30 (lit-24mhz.md
    # section 2.3); the saving applies to the drift's share of lit time
    out['A+24'] = dict(out['A'], clock=-g('lit24_ma') * g('mix_drift'))
    out['A+s+24'] = dict(out['A+s'], clock=-g('lit24_ma') * g('mix_drift'))
    for k in CASES:
        c = out[k]
        c.setdefault('clock', 0.0)
        c['total'] = c['bcm'] + c['pp'] + c['pads'] + c['clock']
        c['bcm_share'] = c['bcm'] / g('i_lit_total')
    # the lit total moves with the case: B1 is the reference state (the
    # 150 mA estimate is today's lit load: the CPU drift)
    for k in CASES:
        if k != 'B1':
            out[k]['delta_vs_B1'] = out[k]['total'] - out['B1']['total']
    out['lit_ma'] = {k: g('i_lit_total') + out[k].get('delta_vs_B1', 0.0) for k in CASES}
    out['lit_hours'] = {k: g('battery_mah') / v for k, v in out['lit_ma'].items()}
    out['mah_per_lit_h_saved_vs_B1'] = {k: -out[k]['delta_vs_B1'] for k in CASES if k != 'B1'}
    out['dark_play_min_per_lit_h'] = {k: 60 * v / g('i_dark_play')
                                      for k, v in out['mah_per_lit_h_saved_vs_B1'].items()}

    # ---- the hibernate break-even
    i_poll = bcm_idle_poll
    e_cold = g('cold_s') * (i_poll + g('i_pp_wait_ma'))          # mA.s
    e_hib = g('hib_wake_s') * (i_poll + g('i_pp_wait_ma'))
    hb = {}
    for name, i_hold in (('clocks stop, panel off', g('i_hold_stop_ma')),
                         ('clocks stop, panel kept', g('i_hold_stop_ma') + g('i_panel_hold_ma')),
                         ('core runs (polls), panel kept', i_poll + g('i_panel_hold_ma'))):
        hb[name] = {'i_hold_ma': i_hold, 'breakeven_s': (e_cold - e_hib) / i_hold}
    out['hib'] = {'e_cold_mas': e_cold, 'e_hib_wake_mas': e_hib, 'cases': hb,
                  # the unlit window held in hibernate between minute updates
                  # instead of polling (C2): per dark entry, mA.s
                  'window_saving_mas': g('unlit_window_s') * (i_poll - g('i_hold_stop_ma')) -
                  2 * e_hib,
                  'window_saving_mah_day': (g('unlit_window_s') * (i_poll - g('i_hold_stop_ma')) -
                                            2 * e_hib) * g('dark_entries') / 3600.0,
                  # ... and once the idle sleep (5i) has cut C2's polling
                  'window_saving_5i_mah_day': (g('unlit_window_s') * (
                      to_ma(g('k_bcm') * g('f_ui') * s11 * g('sleep_factor') + leak11) -
                      g('i_hold_stop_ma')) - 2 * e_hib) * g('dark_entries') / 3600.0,
                  'worst_cap_waste_mah_day': (e_cold - e_hib) * g('dark_entries') / 3600.0}
    return out


CASES = ('A', 'B1', 'B2', 'A+s', 'A+24', 'A+s+24')
SENSITIVE = ['k_bcm', 'e_pp_cyc_nj', 'mix_drift', 'vec_factor', 'p_leak11', 'cpu_fade',
             'i_lit_total']


def sensitivity(base):
    """One at a time, each over its range: A - B1 (mA) and A's BCM share"""
    rows = []
    for k in SENSITIVE:
        res = []
        for idx in (1, 2):
            p = dict(base)
            p[k] = (base[k][idx],) + base[k][1:]
            o = model(p)
            res.append((o['A']['delta_vs_B1'], o['A']['bcm_share'], o['B1']['bcm_share']))
        rows.append((k, base[k][1], base[k][2], res))
    return rows


def fmt(o, sens):
    L = []
    L.append('BCM job time per lit hour (A): %.1f s (duty %.2f %%) [sim costs x estimate mix]' % (
        o['bcm_job_s_per_lit_h'], 100 * o['bcm_job_duty']))
    L.append('')
    L.append('%-7s %7s %7s %7s %7s %7s %9s %10s %7s' % ('case', 'BCM mA', 'PP mA', 'pads',
                                                        'clock', 'sum', 'vs B1', 'BCM share',
                                                        'lit h'))
    for k in CASES:
        c = o[k]
        L.append('%-7s %7.2f %7.2f %7.2f %7.2f %7.2f %9s %9.1f%% %7.2f' % (
            k, c['bcm'], c['pp'], c['pads'], c['clock'], c['total'],
            '%+.2f' % c['delta_vs_B1'] if 'delta_vs_B1' in c else '-',
            100 * c['bcm_share'], o['lit_hours'][k]))
    L.append('')
    L.append('Per lit hour against B1 (the CPU drift on our image):')
    for k, v in o['mah_per_lit_h_saved_vs_B1'].items():
        L.append('  %-5s saves %+6.2f mAh (%+.3f %% of the cell; %+.1f min of dark playback)' % (
            k, v, 100 * v / PARAMS['battery_mah'][0], o['dark_play_min_per_lit_h'][k]))
    L.append('')
    L.append('Sensitivity (one at a time over its range): A - B1 mA; BCM share of lit in A, B1')
    for k, lo, hi, res in sens:
        L.append('  %-12s %8g .. %-8g  A-B1 %+6.2f .. %+6.2f   share A %4.1f..%4.1f %%  B1 %4.1f..%4.1f %%' % (
            k, lo, hi, res[0][0], res[1][0], 100 * res[0][1], 100 * res[1][1],
            100 * res[0][2], 100 * res[1][2]))
    h = o['hib']
    L.append('')
    L.append('Hibernate (power-contract.md 6.6): a cold wake %.2f mA.s, a hibernate wake %.2f mA.s' % (
        h['e_cold_mas'], h['e_hib_wake_mas']))
    for name, c in h['cases'].items():
        L.append('  %-32s hold %5.2f mA  break-even %6.1f s' % (name, c['i_hold_ma'],
                                                                   c['breakeven_s']))
    L.append('  the unlit window held in hibernate instead of C2: %.0f mA.s a dark entry, '
             '%.2f mAh a day' % (h['window_saving_mas'], h['window_saving_mah_day']))
    L.append('  ... once the idle sleep (5i) runs: %.2f mAh a day' % h['window_saving_5i_mah_day'])
    L.append('  the cap\'s worst waste (one cold wake\'s energy a spell): %.3f mAh a day' %
             h['worst_cap_waste_mah_day'])
    return '\n'.join(L)


# ---- P19 PW4: the level bands (bcmfw/clock.c; battery-levers.md section 4)
BANDS = [(27, None), (46, 5), (59, 6), (74, 7), (87, 8), (102, 9), (116, 10),
         (123, 11), (130, 12), (140, 13), (147, 14), (150, 15)]


def band_ma(p, mhz, level, busy, idle_factor):
    """[estimate] the BCM core at a band, as mA at the battery: k_bcm x f x
    (V / 1.2)^2 x (busy + (1 - busy) x idle_factor) + p_leak11 x (V /
    1.04)^2, V = 0.80 + 0.04 (level - 5) [hypothesis: levels 5-15 span the
    brief's 0.8-1.2 V evenly]; idle_factor 1 polling, sleep_factor asleep"""
    g = lambda k: val(p, k)
    v = 0.80 + 0.04 * (level - 5)
    mw = g('k_bcm') * mhz * (v / 1.2) ** 2 * (busy + (1 - busy) * idle_factor) + \
        g('p_leak11') * (v / g('v_l11')) ** 2
    return mw / g('eta') / g('v_batt')


def vclk_table(p, xtal_level=11):
    """per band: polling (a video session today), and asleep at 0, 25 and
    50 % busy (the idle sleep)"""
    rows = []
    for mhz, lv in BANDS:
        lv = lv or xtal_level
        rows.append((mhz, lv, band_ma(p, mhz, lv, 1.0, 1.0),
                     band_ma(p, mhz, lv, 0.0, val(p, 'sleep_factor')),
                     band_ma(p, mhz, lv, 0.25, val(p, 'sleep_factor')),
                     band_ma(p, mhz, lv, 0.50, val(p, 'sleep_factor'))))
    return rows


def vclk_report():
    L = ['The level bands [estimate] (mA at the battery; k_bcm %.2f mW/MHz, '
         'p_leak11 %.1f mW, sleep_factor %.2f):' % (val(PARAMS, 'k_bcm'),
                                                   val(PARAMS, 'p_leak11'),
                                                   val(PARAMS, 'sleep_factor'))]
    L.append('  %5s %5s %9s %9s %9s %9s' % ('MHz', 'level', 'polling', 'asleep',
                                            '25% busy', '50% busy'))
    for r in vclk_table(PARAMS):
        L.append('  %5d %5d %9.2f %9.2f %9.2f %9.2f' % r)
    for lvl in (5,):
        r = vclk_table(PARAMS, lvl)[0]
        L.append('  %5d %5d %9.2f %9.2f %9.2f %9.2f   (the crystal at PW3 level 5)' % r)
    try:
        import vclk_gov_sim as G
    except ImportError:
        return '\n'.join(L)
    for idle, label in ((1.0, 'polling between frames (a session today)'),
                        (val(PARAMS, 'sleep_factor'), 'asleep between frames')):
        L.append('')
        L.append('The governor against today (a video at 150 MHz, the shell at 27 on '
                 'level 11), %s [sim, estimate]:' % label)
        for name, jobs, frame in G.workloads():
            r = {x[0]: G.simulate(G.make(x[0], x[1]), jobs, idle, frame)
                 for x in G.GOVS if x[0] in ('fixed150', 'fixed27', '2-layer: top')}
            a, b = r['fixed150' if frame else 'fixed27'], r['2-layer: top']
            L.append(('  %-10s ' + ('150 MHz' if frame else ' 27 MHz') +
                      ' %6.2f mA; governed %6.2f mA (%+.2f, %+.0f %%), '
                      'mean %5.1f MHz level %4.1f, %d misses') % (
                         name, a['ma'], b['ma'], b['ma'] - a['ma'],
                         100 * (b['ma'] - a['ma']) / a['ma'], b['mean_mhz'],
                         b['mean_level'], b['misses']))
    return '\n'.join(L)


def main(argv):
    if '--vclk' in argv:
        print(vclk_report())
        return 0
    o = model(PARAMS)
    sens = sensitivity(PARAMS)
    if '--json' in argv:
        print(json.dumps({'results': o, 'sensitivity': sens}, indent=1, default=float))
        return 0
    if '--md' in argv:
        print('| Parameter | Value | Range | Unit | Tag | Source |\n|---|---|---|---|---|---|')
        for k, (v, lo, hi, u, tag, src) in PARAMS.items():
            print('| `%s` | %g | %g-%g | %s | [%s] | %s |' % (k, v, lo, hi, u, tag, src))
        return 0
    print(fmt(o, sens))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
