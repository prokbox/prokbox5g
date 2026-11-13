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
 * taken but once per loop, so the figure is a slight overestimate.
 *
 * Plugin arguments (without them the model above is unchanged; the FLAC
 * decoder measurements, tests/flacarm, use both):
 *   exact=on  reads the registers: a multiply costs its real m from the
 *             value of Rs (1..4 by its significant bytes; all ones counts
 *             as empty for the signed forms), an instruction whose
 *             condition fails costs 1 (1S), and halfword and signed-byte
 *             loads cost 3, halfword stores 2.
 *   prof=on   also prints "SYM <cycles> <insns> <name>" per symbol (the
 *             nearest ELF symbol qemu finds for each instruction).
 *   prof=pc   prof=on, plus "PC <address> <cycles>" per translated
 *             instruction (sum the lines of one address). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static int exact, prof, pcprof;

/* exact=on: r0..r15 and cpsr, read at the instructions that need them */
static struct qemu_plugin_register *reg_h[16], *cpsr_h;
static GByteArray *rbuf;

struct sym { const char *name; uint64_t cyc, insn; struct sym *next; };
static struct sym *syms;
static GMutex lock;

struct ins {
    uint32_t w;
    unsigned int base;          /* the weight when it executes */
    struct sym *s;
    uint64_t vaddr, cyc;        /* prof=pc */
    struct ins *next;
};
static struct ins *all_ins;

static struct sym *sym_for(const char *name)
{
    struct sym *p;
    if (!name)
        name = "?";
    g_mutex_lock(&lock);
    for (p = syms; p; p = p->next)
        if (!strcmp(p->name, name))
            break;
    if (!p) {
        p = calloc(1, sizeof *p);
        p->name = strdup(name);
        p->next = syms;
        syms = p;
    }
    g_mutex_unlock(&lock);
    return p;
}

static uint32_t rd(struct qemu_plugin_register *h)
{
    uint32_t v = 0;
    g_byte_array_set_size(rbuf, 0);
    if (h && qemu_plugin_read_register(h, rbuf) && rbuf->len >= 4)
        memcpy(&v, rbuf->data, 4);
    return v;
}

static int cond_passes(unsigned int c, uint32_t f)
{
    int n = f >> 31 & 1, z = f >> 30 & 1, cy = f >> 29 & 1, v = f >> 28 & 1;
    int r;
    switch (c >> 1) {
    case 0: r = z; break;
    case 1: r = cy; break;
    case 2: r = n; break;
    case 3: r = v; break;
    case 4: r = cy && !z; break;
    case 5: r = n == v; break;
    case 6: r = !z && n == v; break;
    default: return 1;
    }
    return (c & 1) ? !r : r;
}

/* m for the ARM7TDMI multiplier: 8 bits of Rs a cycle, stopping when the
   rest is all zero (or, for the signed forms, all one) */
static unsigned int mcycles(uint32_t rs, int sgn)
{
    if (sgn && (rs & 0x80000000u))
        rs = ~rs;
    if (!(rs & 0xffffff00u)) return 1;
    if (!(rs & 0xffff0000u)) return 2;
    if (!(rs & 0xff000000u)) return 3;
    return 4;
}

static void insn_exec(unsigned int vcpu, void *ud)
{
    (void)vcpu;
    n_insn++;
    n_cyc += (uintptr_t)ud;
}

static void insn_prof(unsigned int vcpu, void *ud)
{
    struct ins *in = ud;
    (void)vcpu;
    n_insn++;
    n_cyc += in->base;
    in->s->insn++;
    in->s->cyc += in->base;
    in->cyc += in->base;
}

static void insn_regs(unsigned int vcpu, void *ud)
{
    struct ins *in = ud;
    uint32_t w = in->w;
    unsigned int c = in->base;
    (void)vcpu;
    if ((w >> 28) < 14 && !cond_passes(w >> 28, rd(cpsr_h)))
        c = 1;
    else if ((w & 0x0fc000f0) == 0x00000090)         /* MUL/MLA: S+mI(+I) */
        c = 1 + mcycles(rd(reg_h[(w >> 8) & 15]), 1) + (w >> 21 & 1);
    else if ((w & 0x0f8000f0) == 0x00800090)         /* xMULL/xMLAL */
        c = 2 + mcycles(rd(reg_h[(w >> 8) & 15]), w >> 22 & 1)
              + (w >> 21 & 1);
    n_insn++;
    n_cyc += c;
    if (in->s) {
        in->s->insn++;
        in->s->cyc += c;
        in->cyc += c;
    }
}

static unsigned int weight_exact(uint32_t i)
{
    /* LDRH/LDRSB/LDRSH 3, STRH 2: the default model takes them for data
       processing */
    if ((i & 0x0e000090) == 0x00000090 && (i & 0x60))
        return (i & (1u << 20)) ? 3 : 2;
    return weight(i);
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
        if (!exact && !prof) {
            qemu_plugin_register_vcpu_insn_exec_cb(insn, insn_exec,
                QEMU_PLUGIN_CB_NO_REGS, (void *)(uintptr_t)weight(w));
            continue;
        }
        struct ins *in = g_new0(struct ins, 1);
        in->w = w;
        in->base = exact ? weight_exact(w) : weight(w);
        in->s = prof ? sym_for(qemu_plugin_insn_symbol(insn)) : NULL;
        in->vaddr = qemu_plugin_insn_vaddr(insn);
        if (pcprof) {
            g_mutex_lock(&lock);
            in->next = all_ins;
            all_ins = in;
            g_mutex_unlock(&lock);
        }
        int regs = exact && ((w >> 28) < 14
                             || (w & 0x0fc000f0) == 0x00000090
                             || (w & 0x0f8000f0) == 0x00800090);
        if (regs)
            qemu_plugin_register_vcpu_insn_exec_cb(insn, insn_regs,
                                                   QEMU_PLUGIN_CB_R_REGS, in);
        else if (prof)
            qemu_plugin_register_vcpu_insn_exec_cb(insn, insn_prof,
                                                   QEMU_PLUGIN_CB_NO_REGS, in);
        else
            qemu_plugin_register_vcpu_insn_exec_cb(insn, insn_exec,
                QEMU_PLUGIN_CB_NO_REGS, (void *)(uintptr_t)in->base);
    }
}

static void vcpu_init(unsigned int vcpu, void *ud)
{
    static const char *const nm[16] = { "r0", "r1", "r2", "r3", "r4", "r5",
        "r6", "r7", "r8", "r9", "r10", "r11", "r12", "sp", "lr", "pc" };
    GArray *regs = qemu_plugin_get_registers();
    (void)ud; (void)vcpu;
    for (guint k = 0; regs && k < regs->len; k++) {
        qemu_plugin_reg_descriptor *d =
            &g_array_index(regs, qemu_plugin_reg_descriptor, k);
        for (int r = 0; r < 16; r++)
            if (!strcmp(d->name, nm[r]))
                reg_h[r] = d->handle;
        if (!strcmp(d->name, "cpsr"))
            cpsr_h = d->handle;
    }
    if (regs)
        g_array_free(regs, TRUE);
    for (int r = 0; r < 15; r++)
        if (!reg_h[r])
            cpsr_h = NULL;
    if (!cpsr_h) {   /* never quietly fall back to the coarse model */
        fprintf(stderr, "cyc_plugin: exact=on but the registers are not "
                        "readable\n");
        exit(2);
    }
}

static void at_exit(void *p)
{
    (void)p;
    fprintf(stderr, "INSN_COUNT %" PRIu64 "\nCYC_COUNT %" PRIu64 "\n",
            n_insn, n_cyc);
    for (struct sym *s = syms; s; s = s->next)
        if (s->insn)
            fprintf(stderr, "SYM %" PRIu64 " %" PRIu64 " %s\n",
                    s->cyc, s->insn, s->name);
    for (struct ins *in = all_ins; in; in = in->next)
        if (in->cyc)       /* one line per translation: sum them by address */
            fprintf(stderr, "PC %08" PRIx64 " %" PRIu64 "\n", in->vaddr,
                    in->cyc);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv)
{
    (void)info;
    for (int k = 0; k < argc; k++) {
        if (!strcmp(argv[k], "exact=on"))
            exact = 1;
        else if (!strcmp(argv[k], "prof=on"))
            prof = 1;
        else if (!strcmp(argv[k], "prof=pc"))
            prof = pcprof = 1;
        else {
            fprintf(stderr, "cyc_plugin: unknown argument %s\n", argv[k]);
            return -1;
        }
    }
    rbuf = g_byte_array_new();
    if (exact)
        qemu_plugin_register_vcpu_init_cb(id, vcpu_init, NULL);
    qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans, NULL);
    qemu_plugin_register_atexit_cb(id, at_exit, NULL);
    return 0;
}
