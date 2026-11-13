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

# Mutants of the code run.sh extracts (pcm-pp.c's pcm-wait block,
# dma_tx_stop, pq_stop, sink_stop_pcm; pcm.c's pcm_play_data), applied by
# tests/fencepcm/srcpatch.py.  Each must turn test_pcmwait red with its
# verdict line (a hang is reported as one, never left to run).
MUTANTS = {
    "dma_unbounded": (
        "dma_tx_stop's old wait: spins for ever with the clocks off",
        [("    pcm_wait_bounded(PCM_WAIT_CH0);\n",
          "    while (DMA0_STATUS & (DMA_STATUS_BUSY | DMA_STATUS_INTR));\n")]),
    "fifo_unbounded": (
        "sink_stop_pcm's old FIFO wait",
        [("    pcm_wait_bounded(PCM_WAIT_FIFO);\n",
          "    while (!IIS_TX_IS_EMPTY);\n")]),
    "queue_unbounded": (
        "pq_stop's wait with no bound",
        [("    pcm_wait_bounded(PCM_WAIT_CH0 | PCM_WAIT_CH1);     /* #312 */\n"
          "    pq_n = 0;",
          "    while ((PQ_STATUS(0) | PQ_STATUS(1)) &\n"
          "           (DMA_STATUS_BUSY | DMA_STATUS_INTR));\n"
          "    pq_n = 0;")]),
    "no_fifo_clear": (
        "the recovery's FIFO clear taken away",
        [("    IISFIFO_CFG |= IIS_TXCLR;\n", "")]),
    "no_record": (
        "the fault not counted (no log line would follow)",
        [("    pcm_wf.count++;     /* last: the reader's signal */\n", "")]),
    "bound_common_rate": (
        "the bound sized from 44.1 kHz instead of the slowest rate (8 kHz: "
        "the FIFO and the word in flight take 2.1 ms)",
        [("#define PCM_PP_SLOWEST_HZ   8000", "#define PCM_PP_SLOWEST_HZ   44100")]),
    "no_wake": (
        "pcm_play_data() starts without waking the codec",
        [("    if (dac_in_standby())\n        dac_wake();\n", "")]),
    "wake_always": (
        "pcm_play_data() wakes unconditionally (a mutex under the "
        "mixer's PCM lock)",
        [("    if (dac_in_standby())\n        dac_wake();\n",
          "    dac_wake();\n")]),
}
