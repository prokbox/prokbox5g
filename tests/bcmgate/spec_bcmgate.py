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

# Mutants of the accessors run.sh extracts (lcd-video.c, bcm-devtest-hal.c,
# e2-port.c), applied by tests/fencepcm/srcpatch.py.  Each takes one
# accessor's power gate away (or unbounds one port-lock wait) and must turn
# test_bcmgate red with its verdict line: a touch of the unpowered port,
# a refusal named after a helper instead of the entry point, or a hang.


def ungate(fn, ret):
    """The gate line pair `if (!BCM_PORT_OK("fn"))` + `return ret;` removed"""
    return ("the gate taken from %s" % fn,
            [('    if (!BCM_PORT_OK("%s"))\n        return%s;\n' % (fn, ret), "")])


MUTANTS = {
    "bcm_port_wait": ("the gate taken from bcm_port_wait",
        [('    if (!BCM_PORT_OK("bcm_port_wait"))\n'
          '        return false;           /* unpowered: not dead, nothing touched */\n', "")]),
    "bcm_write_addr": ungate("bcm_write_addr", ""),
    "bcm_write32": ungate("bcm_write32", ""),
    "bcm_read32": ("the gate taken from bcm_read32",
        [('    if (!BCM_PORT_OK("bcm_read32"))\n'
          '        return 0xFFFF;          /* reads as busy: nothing is kicked */\n', "")]),
    "doorbell": ("the doorbell macro ungated",
        [('    do { if (BCM_PORT_OK(fn)) BCM_CONTROL = 0x31; } while (0)',
          '    do { BCM_CONTROL = 0x31; } while (0)')]),
    "lcd_update_rect": ("the gate taken from lcd_update_rect",
        [('    if (!BCM_PORT_OK("lcd_update_rect"))\n', '    if (0)\n')]),
    "lcd_blit_yuv": ("the gate taken from lcd_blit_yuv",
        [('    if (!BCM_PORT_OK("lcd_blit_yuv"))\n', '    if (0)\n')]),
    "bcm_command": ("the gate taken from bcm_command",
        [('    if (!BCM_PORT_OK("bcm_command"))\n'
          '        return;                 /* the 0xFFFF it would read: no wait */\n', "")]),
    "dt_until": ("the gate taken from dt_until",
        [('    if (!BCM_PORT_OK("dt_until"))      /* #324: unpowered, not touched */\n'
          '        return false;\n', "")]),
    "dt_wait": ("the gate taken from dt_wait",
        [('    if (!BCM_PORT_OK("dt_wait"))       /* no diagnostic reads either */\n'
          '        return false;\n', "")]),
    "dt_rd": ungate("dt_rd", " -1"),
    "dt_wr": ungate("dt_wr", " -1"),
    "bpb_addr": ungate("bpb_addr", " -1"),
    "bpb_words": ("the gate taken from bpb_words",
        [('    if (BCM_PORT_OK("bpb_words"))\n', '    if (1)\n')]),
    "bpb_word": ("the gate taken from bpb_word",
        [('    if (BCM_PORT_OK("bpb_word"))\n', '    if (1)\n')]),
    "dt_wr_block": ungate("dt_wr_block", " -1"),
    "dt_ppdma": ungate("dt_ppdma", " -1"),
    "dt_fb_upload": ungate("dt_fb_upload", " -1"),
    "dt_upload_fb": ungate("dt_upload_fb", " -1"),
    "dt_doorbell": ("the gate taken from dt_doorbell",
        [('    if (BCM_PORT_OK("dt_doorbell"))\n', '    if (1)\n')]),
    "dt_pa_addr": ungate("dt_pa_addr", " -1"),
    "bcm_video_port_raw": ("the gate taken from bcm_video_port_raw",
        [('    if (!BCM_PORT_OK("bcm_video_port_raw"))\n', '    if (0)\n')]),
    "port_raw_data_reads": ("bcm_video_port_raw reads DATA and ALT_DATA raw again "
                            "(b12's vcdark freeze, chip on)",
        [('    out[4] = BCM_CONTROL;\n}',
          '    for (i = 0; i < 4; i++)\n        out[i] = BCM_DATA;\n'
          '    out[4] = BCM_CONTROL;\n'
          '    for (i = 0; i < 4; i++)\n        out[5 + i] = BCM_ALT_DATA;\n'
          '    out[9] = BCM_ALT_CONTROL;\n}')]),
    "port_raw_altctl": ("bcm_video_port_raw reads ALT_CONTROL raw again (unproven)",
        [('    out[4] = BCM_CONTROL;\n}',
          '    out[4] = BCM_CONTROL;\n    out[9] = BCM_ALT_CONTROL;\n}')]),
    "read32_no_address": ("bcm_read32 loads DATA before writing the read address",
        [('    BCM_RD_ADDR32 = address;\n    if (!bcm_port_wait(&BCM_CONTROL, 0x10, true))\n'
          '        return 0xFFFF;\n    return BCM_DATA32;',
          '    (void)BCM_DATA32;\n    BCM_RD_ADDR32 = address;\n'
          '    if (!bcm_port_wait(&BCM_CONTROL, 0x10, true))\n'
          '        return 0xFFFF;\n    return BCM_DATA32;')]),
    "bcm_video_alt_lab": ("the gate taken from bcm_video_alt_lab",
        [('    if (vdma_wait() || !BCM_PORT_OK("bcm_video_alt_lab"))\n',
          '    if (vdma_wait())\n')]),
    "vwr_block": ("the gate taken from vwr_block (the DMA start)",
        [('    if (!BCM_PORT_OK("vwr_block"))     /* no DMA started into it either */\n'
          '        return -1;\n', "")]),
    "bcm_video_wr_variant": ("the gate taken from bcm_video_wr_variant",
        [(' || !BCM_PORT_OK("bcm_video_wr_variant"))', ')')]),
    "e2port_port_ready": ("the gate taken from e2-port.c's port_ready",
        [('    if (!E2_PORT_OK("e2port_port_ready"))\n        return false;\n', "")]),
    "e2port_start": ("the gate taken from e2port_start (engine 2's start)",
        [('    if (!E2_PORT_OK("e2port_start"))   /* nor the engine into it */\n'
          '        return -1;\n', "")]),
    "lock_unbounded_video": ("bcm_video_rd back on the unbounded port lock",
        [('int bcm_video_rd(uint32_t addr, uint32_t *val)\n{\n'
          '    int lk = lcd_port_lock_bounded(BW_LOCK_VIDEO);',
          'int bcm_video_rd(uint32_t addr, uint32_t *val)\n{\n'
          '    int lk = lcd_port_lock() ? 1 : 0;')]),
    "lock_unbounded_update": ("lcd_update_rect back on the unbounded port lock",
        [('        int lk = lcd_port_lock_bounded(BW_LOCK_UPDATE);',
          '        int lk = lcd_port_lock() ? 1 : 0;')]),
    "lock_tmo_forever": ("lcd_port_lock_tmo with no bound",
        [('    while (TIME_BEFORE(current_tick, end));\n    return -1;',
          '    while (1);\n    return -1;')]),
    "lock_no_record": ("a lock timeout not recorded",
        [('    if (lk < 0)\n        bcm_wait_fault(k, current_tick - t0);\n', "")]),
    "refusal_no_log": ("the refusal never asks for its log line",
        [('    if (first)\n        button_queue_post(SYS_BCMJOB_REQ, 0);', '    if (0)\n        button_queue_post(SYS_BCMJOB_REQ, 0);')]),
}
