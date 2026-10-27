/***************************************************************************
 *              __________                   __    __________
 *   Open       \______   \_______    ____  |  | __\______   \  ____  ___  ___
 *   Source      |     ___/\_  __ \  /  _ \ |  |/ / |    |  _/ /  _ \ \  \/  /
 *   Player      |    |     |  | \/ (  <_> )|    <  |    |   \(  <_> ) >    <
 *   Firmware    |____|     |__|     \____/ |__|_ \ |______  / \____/ /__/\_ \
 *                                               \/        \/               \/
 *
 * Custom firmware for the iPod Video, 5th generation (5G and 5.5G).
 * Not affiliated with or endorsed by Apple Inc.
 *
 * Copyright (C) 2026 prokopto-dev
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

/* qemu-system-arm TCG plugin: executed instruction count plus an ARM7TDMI
 * cycle estimate (P19 19-13 resampler measurements, tests/resample).
 *
 * The estimate weights each executed ARM instruction by its ARM7TDMI
 * timing (ARM7TDMI TRM, chapter 6) with zero-wait memory (IRAM, or cache
 * hits) and the multiplier's worst case (m = 4): data processing 1 (+1
 * register shift, +2 writing pc), LDR 3 (5 to pc), STR 2, LDM n+2 (+2 to
 * pc), STM n+1, B/BL 3, MUL 5, MLA 6, SMULL/UMULL 6, SMLAL/UMLAL 7.
 * Conditional instructions are counted as executed; loop branches are
 * taken but once per loop, so the figure is a slight overestimate. */
#include <stdio.h>
#include <inttypes.h>
#include <glib.h>
#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

static uint64_t n_insn, n_cyc;

static unsigned int weight(uint32_t i)
{
    unsigned int op = (i >> 25) & 7;
    if ((i & 0x0fc000f0) == 0x00000090)             /* MUL/MLA */
        return (i & (1u << 21)) ? 6 : 5;
    if ((i & 0x0f8000f0) == 0x00800090)             /* long multiply */
        return (i & (1u << 21)) ? 7 : 6;
    if (op == 2 || op == 3)                          /* LDR/STR */
    {
        if (!(i & (1u << 20)))
            return 2;
        return ((i >> 12) & 15) == 15 ? 5 : 3;
    }
    if (op == 4)                                     /* LDM/STM */
    {
        unsigned int n = __builtin_popcount(i & 0xffff);
        if (!(i & (1u << 20)))
            return n + 1;
        return n + 2 + ((i & 0x8000) ? 2 : 0);
    }
    if (op == 5)                                     /* B/BL */
        return 3;
    if (op == 0 || op == 1)                          /* data processing */
    {
        unsigned int c = 1;
        if (op == 0 && (i & 0x10) && !(i & 0x80))
            c++;                                     /* shift by register */
        if (((i >> 12) & 15) == 15 && ((i >> 23) & 3) != 2)
            c += 2;                                  /* writes pc */
        return c;
    }
    return 1;
}

static void insn_exec(unsigned int vcpu, void *ud)
{
    (void)vcpu;
    n_insn++;
    n_cyc += (uintptr_t)ud;
}

static void tb_trans(struct qemu_plugin_tb *tb, void *ud)
{
    (void)ud;
    size_t n = qemu_plugin_tb_n_insns(tb);
    for (size_t k = 0; k < n; k++)
    {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, k);
        uint32_t w = 0;
        qemu_plugin_insn_data(insn, &w, sizeof w);
        qemu_plugin_register_vcpu_insn_exec_cb(insn, insn_exec,
            QEMU_PLUGIN_CB_NO_REGS, (void *)(uintptr_t)weight(w));
    }
}

static void at_exit(void *p)
{
    (void)p;
    fprintf(stderr, "INSN_COUNT %" PRIu64 "\nCYC_COUNT %" PRIu64 "\n",
            n_insn, n_cyc);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv)
{
    (void)info; (void)argc; (void)argv;
    qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans, NULL);
    qemu_plugin_register_atexit_cb(id, at_exit, NULL);
    return 0;
}
