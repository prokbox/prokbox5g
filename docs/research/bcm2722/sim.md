# VideoCore II simulator (`vc2sim`)

Issue #60, workstream 0/3 of `approach.md`. Before any of our own VideoCore code runs on the BCM2722 we need a simulator we can trust. `tools/bcmre/vc2sim.py` executes VideoCore II scalar code and, through `vc2vpu.py`, the **vector unit**. Its semantics are checked by running Apple's own library functions (reference material only, never shipped or loaded) against independent Python models: C-library and spec definitions for the scalar code, the H.264 specification for the vector kernels.

```sh
tools/bcmre/vc2sim.py --selftest ~/Git/re/bcm2722/extract/1.3/vc/VideoCore           # 62 rows, ~4 s
tools/bcmre/vc2sim.py --selftest .../VideoCore --cover        # + executed instruction kinds
tools/bcmre/vc2sim.py --selftest .../VideoCore --controls     # flip each hypothesis (slow, ~15 min)
tools/bcmre/vc2sim.py --selftest .../VideoCore --controls --only intra,interp   # some test groups only
tools/bcmre/vc2sim.py --call .../Library/h264dec.vll CeilLog2 5 --trace
tools/bcmre/vc2sim.py --call .../Boot/vmcs.bin __rem -100 7
tools/bcmre/vc2sim.py --exports .../Boot/vmcs.bin             # vmcs.bin export table
tools/bcmre/vc2sim.py --boot .../Boot/vmcs.bin --bcm --max 400000 --hide 0x10001804   # boot, device log
tools/bcmre/vc2sim.py --boot .../Boot/vmcs.bin --bcm --max 400000 --panel 0 --then 0xf672,2 --lcd
```

Python 3 stdlib only. Decoding is imported from `vc2dis.py` (one encoding table for the disassembler, the assembler and the simulator); `vc2sim.py` adds scalar semantics, `vc2vpu.py` the vector unit; `vc2sim_tests.py` and `vc2vpu_tests.py` hold the self-test. `--only` filters on test-function names (`t_h264`, `intra`, `interp` …).

## Design

- **Decode → closure.** Each instruction is decoded once by `vc2dis.decode` and compiled into a Python closure that performs it and returns the next pc (cached per address; `Sim.invalidate()` for code that is rewritten). About 1.9 M instructions/s.
- **Memory.** One flat little-endian 32 MB space. Unaligned half/word accesses and accesses outside it raise `SimError`: deliberately strict, so a wrong guess about an offset encoding shows up as a fault rather than as silently wrong data. Map: vmcs.bin at 0, `.vll` at its link address (0x100000), heap 0x0800000–0x1800000 (bump allocator), stack top 0x1f00000, import stubs at 0x1f80000, return magic 0x1fffff0.
- **Exceptions (optional, `Sim.exceptions = True`; the bcmfw benches turn it on).** Faults enter a VC II exception instead of stopping: the `rti` frame (sr at sp, pc at sp + 4), interrupt enable cleared, jump through the word at 4·v. The misaligned load follows the silicon (device sessions 2–3, `isa.md` "Exceptions on silicon"). It reads the rounded-down address and writes rd and any writeback (`HYP['exc_mis_load_rounds']`). Vector 1 (`exc_vec_misaligned`) is then taken after `exc_defer_insns` = 4 more instructions (`exc_defer_insns_wb` = 2 for `(--rs)`/`(rs)++`), with the saved pc at the first instruction not run, plus `exc_pc_offset` = 0. A second fault inside that window takes the pending one at once, resuming at the second instruction. Misaligned stores, bus errors (vector 6) and undefined instructions (vector 3) are modelled precise, with no write; that is unmeasured [L]. `--controls` skips the `exc_` keys. The unit rows "synchronous exception model" and "misaligned load as on silicon" (the probe's checks 9 and 12 with the device's values) cover them.
- **Registers.** r0–r31 as unsigned 32-bit ints; r24 = gp, r25 = sp, r26 = lr, r30 = sr (flags in VC IV's layout Z=8 N=4 C=2 V=1, interrupt-enable bit 30 as bookkeeping), r31 = pc (reads give the current instruction's address).
- **Loader (`.vll`).** Reads the file through `vc2dis.Vll` (the one ELF reader: sections, symbols with type and shndx, every `.rela` entry with its symbol index), maps every SHF_ALLOC section, zero-fills NOBITS, applies `.rela.*`:

  | Type | Site | Field written |
  |---|---|---|
  | R1 | data word | S+A |
  | R11 | 48-bit `lea` (offset at instruction + 2) | 27-bit offset = target − instruction address |
  | R16/R17/R18 | 48-bit memory op (byte/halfword/word) | same, with the low offset bits placed as described under "48-bit memory ops" |
  | R23 | `bl` | 27-bit halfword offset |

  S is 0 for section symbols (their addend is already the absolute address) and the symbol value otherwise; undefined symbols resolve to import stubs. Apple's binaries ship with the fields zeroed, so the loader is the only thing that fills them.
- **Loader (vmcs.bin).** Raw image at 0. gp is read from the reset code (`mov r24, #imm32` at 0x200: 0x1fc40 in 1.3). Exports come from the image's own table: 183 `(address, name pointer)` word pairs sorted by name (0x2160c in 1.3), e.g. `__modu`=0x456, `memcpy`=0xdf60, `sprintf`=0x12d2a. (A first reading as `(name, address)` pairs gives every name its neighbour's address; the code at each address settles it: 0x456 is `div.uu; mul; rsub`, an unsigned modulo.)
- **Imports.** Python stubs, documented in `STUBS`: `memcpy`/`memmove`/`vclib_memcpy` (memmove semantics; `remove_byte3` uses `vclib_memcpy` on overlapping ranges), `memset`, `vclib_memset2` (dst, halfword, count; assumed), `malloc_priority` (size, align, priority, name; from the passthru call sites), `malloc`, `malloc_256bit`, `free`, `free_256bit`, `__modu`, `__rem`, and no-op `vclib_obtain_VRF`/`release_VRF`/`dcache_flush`/`univ_setleds`/`clearleds`. Any other import (DMA, RTOS, messaging) raises `Unimplemented` naming it. After every stub call r1–r5 (and r0 for void stubs) are overwritten with `0xdead000N`, so callers that wrongly rely on caller-saved registers fail loudly.
- **Vector unit.** Modelled in `vc2vpu.py` (section "Vector unit model" below). Forms without a hypothesis raise `VectorOp` naming the instruction, as do `bkpt`, unidentified `sysN`, `op2ec`, `op24`, and unknown ALU ops (9, 17, 29; `--assume`-style candidates exist in `ALU_CANDIDATES` for experiments). `rti` pops sr then pc (below); `sleep` and `sys19` run only in boot runs ("Peripheral bus").
- **Calls.** `Sim.call(func, *args, stack_args=(), regs={}, check_abi=False)` puts args in r0–r5, extra args on the stack, lr = return magic, and runs until return; it checks that sp is restored. With `check_abi` (always on in the self-test) r6–r23 get random values and must come back unchanged; registers passed in `regs` (global register variables such as bitstream cursors) are exempt.
- **Tracing.** `--trace` (or `Sim(trace=file)`) logs each instruction with its register deltas and every stub call.
- **Coverage and controls.** `--cover` counts executed instruction kinds (class + operation + condition). `vc2sim.HYP` holds each semantic choice the corpus settled (vector ones are prefixed `v_`); `--controls` flips each boolean, or cycles through `HYP_ALTERNATIVES` for multi-valued ones, and reports which tests break (tables below).

## Peripheral bus and boot runner (`vc2mmio.py`, `--boot`)

For the hardware recovery (`hardware.md`) the simulator can run a raw image from its reset entry against modelled peripherals.

- **Bus.** `vc2mmio.Bus` replaces the flat memory: RAM below 32 MB as before (64 MB in boot runs, 32–64 MB backing the SDRAM window), every other address a device access. Each access is logged with pc, width, value, sequence number and simulated time; writes are remembered and read back; aliases map an address window onto RAM. A read that one instruction repeats 2,000 times with an unchanged result stops the run (`PollStall`) and names the register, which is how each status bit below was found.
- **Register rules** (`--reg`): `ADDR=V`, `ADDR|=M`, `ADDR&=M`, `ADDR^=M`, `ADDR+=N` (counter), on one address or a range; `ADDR|=M@N` makes M a status bit that reads 0 for N reads after each write and 1 after ("busy, then done").
- **Devices.** `SysTimer` (1 MHz counter, four compares, match status; source 16 + k) and `IntCtl` (level, per-source 4-bit priorities, pending vector at +4) with the layouts in `hardware.md`. `run_machine` advances time one µs per 100 instructions and per counter read (1 µs by default, 4 in the self-test), and takes the highest pending interrupt when sr bit 30 allows: push sr and pc, clear bit 30, jump through the vector word. `sleep` advances time to the next compare and takes its interrupt; with nothing armed it stops as idle.
- **`--bcm`** is the minimum model vmcs.bin boots under: the two "done" bits (`0x10000c00` bit 0 after a cache flush, `0x10003420` bit 15 after an SDRAM command), the 4 MB SDRAM window at 0xfe000000, timer and interrupt controller. Under it vmcs.bin 1.3 (and 1.2.1, 1.2.3, with identical device logs) runs its start-up, starts Nucleus and its tasks and services thousands of interrupts.
- **After the boot**, `--then FUNC,ARG...` calls a function (e.g. `0xf672,2`, the LCD power handler), `--panel ID` sets the panel-ID pins, `--lcd` prints the log as panel commands, GPIO edges and delays, `--hide` drops a register (the counter) from the log, `--summary` counts accesses per register.
- **Self-test rows** (`vc2boot_tests.py`, vmcs.bin 1.3 only): "vmcs.bin start-up writes" checks 17 start-up writes in order, the PLL lock wait, the 10 ms tick, that every interrupted context resumes with all registers, and the host-interface pointer at 0x1fc (23 cases); "LCD power-on per panel ID" checks, for panel IDs 0–3, the reset pulse, the bus timing words and the command sequence (12 cases). They are the controls for `push_lowest_first` and `pc48_mem_aligned` (table below).

## Our own image on the bus (`bcmfw`, milestone 3)

`bcmfw/tests/hello_test.py` runs our VideoCore image (`bringup.md`) on the modelled bus, against two models.

- **`vc2mmio.LcdPanel`.** This is the LCD interface at `0x10004000` plus the panel behind it.
  - It records time-stamped events: interface writes, index writes, parameter and register values, GPIO 48 reset edges, and the pin 48–63 function select. Pixel data is counted, not listed.
  - It decodes windows and RAM writes into a 320×240 RGB565 frame, per controller family:
    - DCS: `0x2A/0x2B`/`0x2C`;
    - 0x2xx: `0x210–0x213`, `0x200/0x201`, `0x202`;
    - 0x4x: `0x44–0x46`, `0x20/0x21`, `0x22`.
  - It counts misuse: pixel writes with the upper 16 bits set, data with no command, and pixels before the reset pulse.
  - It also attaches to a `vmcs.bin` boot. That gives Apple's power sequences in the same event form, so ours can be compared with them event for event.
- **A host model**, which is a port of `lcd-video.c`'s logic:
  - `bcm_init`: upload, `0x1F8 = 0`, the cache flush through `0x10000C00`, then `0x10000400 = 0xA5A50002`, which starts the core at the entry;
  - `lcd_update_rect`, with its even-width rounding and partial row writes;
  - `lcd_tick`, every 10 ms of simulated time, including re-kicks after 50/500 ms;
  - `bcm_command`, `lcd_sleep` (`bcm_powerdown`, then a power cut that randomises SRAM and resets the device registers) and `lcd_awake`.

  While the core is idle (status = done), time is fast-forwarded to the next host tick rather than its polling loop being simulated.
- **`vc2mmio.DmaEngine`** (the DMA build's bench, `bcmfw/tests/dma_test.py`). It models the engine at `0x10000800`: channel config, completion status, sub-channel starts, and 32-byte control blocks (1-D, 2-D with skips, chains). Transfers are paced by a request line and progress against the system timer. `LcdPanel` models the matching automatic pixel run and the FIFO at `+0x0C`, which raises DREQ 3 while a run expects data. Field meanings are our readings (`bringup.md`, "DMA frame push").
- **`run_machine(..., stop=True)`** returns the pc at the step limit instead of raising. The host model uses it to run the core in slices of 2,000 instructions.

Frames are written to PNG (`build/bcmfw/test/`), read back and compared pixel for pixel with the host framebuffer. When the reference corpus is present, the bench also runs this self-test under `COVER`. It checks that the image executes only instruction kinds the self-test exercises, apart from three accepted encoding forms (`bringup.md`).

## Calling convention (derived, then verified on every self-test call)

- Arguments in **r0–r5**, result in **r0** (64-bit results in r0 = low, r1 = high: `__lmul`, `__ldivs`).
- **Stack arguments start at the caller's sp** at the `bl`: argument 7 at [sp], 8 at [sp+4]. Varargs callees reserve 16 bytes and spill r2–r5 just below the caller's sp, so the arguments form one array (`sprintf`, `psnprintf`: `add sp,#-16; st r3,(sp+4)…; push lr; …; st r2,(sp+8)`). `calcmvpredictor` reads its three stack arguments at sp+20/24/28 after pushing five registers. Verified: `sprintf` with up to 8 arguments, `calcmvpredictor` with 3 stack arguments, 0 failures.
- **r6–r15 callee-saved** (`push r6-rN, lr` / `pop r6-rN, pc`), lr = r26, `bl` writes lr = next instruction. Verified: r6–r23 randomised around 20,576 calls, never clobbered.
- **r16–r23 are not used by compiled code at all** (no push/pop of r16+ anywhere in the six libraries; a handful of uses in vmcs.bin). Hand-written codec code keeps global register variables there (below); vmcs exports `vclib_save_high_registers`/`vclib_restore_high_registers` for them.
- r0–r5 are caller-saved (the stub poisoning never broke a caller).
- sp is kept 4-byte aligned (frames are multiples of 4).
- Leaf helpers in hand-written code may use private conventions: `predresidual` takes and returns its value in **r4**.

## r16 (and r19/r20): software-expanded bitstreams, not hardware

**r16 is the H.264 decoder's bit cursor into a "window table" built in memory.** For every bit position *i* of the stream the table holds, at `base + 2·i`, the 16-bit value of bits *i…i+15* (first bit in bit 15). Reading *n* ≤ 16 bits is one halfword load and two shifts; advancing *n* bits is `adds2 r16, r16, rN` (r16 += 2n); Exp-Golomb codes use `msb` on one window plus a second window 16 bits on for long codes. `Bitstream_Set` points r16 at a `.bss` buffer + 32 (0x10e660 + 32 in 1.3) and `Bitstream_Update` computes consumed bits as (r16 − start) / 2; the table is filled by the vector-unit routines `xbit_expand_reset`/`xbit_expand_remove3`, which also strip emulation-prevention bytes. Nothing in the scalar code suggests hardware assist: the self-test builds the table in Python and every reader returns the right value and advance.

`mpg4dec` uses the same trick with **r20** as cursor and **r19** as refill limit (`vc_GetBits`: `adds2 r20, r20, r0 … cmp r20, r19; blt` else refill), but its table is oriented the other way: entry *i* holds the 16 bits **ending** at bit *i* (bit *i*−1 in bit 0), and `vc_ShowBits(n)` combines the entries at *i+n* and *i+n*−16 and masks *n* bits (`bmask`).

Consequence for our CAVLC reader: this is a memory-for-speed layout (2 bytes per stream bit, i.e. 16× the bitstream, built by the vector unit). We may choose it or an ordinary 32-bit-cache reader; nothing in the hardware forces either.

## Validation (seed 2722)

| Function | Library | Cases | Pass | Reference model |
|---|---|---|---|---|
| `__modu` | vmcs | 1572 | 1572 | a % b unsigned, edges + random |
| `__rem` | vmcs | 1571 | 1571 | C remainder (truncating) |
| `__lmul` | vmcs | 1000 | 1000 | 64×64 → low 64 |
| `__ldivs` (and the unexported unsigned 64-bit divide it calls) | vmcs | 600 | 600 | truncating 64-bit signed division |
| `rand` | vmcs | 1000 | 1000 | Park–Miller minimal standard (16807 mod 2³¹−1), seed at gp+0x298 |
| `memcpy`, `memmove` (overlap), `memset` | vmcs | 900 | 900 | ISO C |
| `memchr` (0xdf22), `memcmp` (0xdf3a), unexported | vmcs | 600 | 600 | ISO C (memcmp returns −1/0/1); identified by these tests |
| `strcmp`, `strncmp`, `_stricmp`, `_strnicmp` | vmcs | 1000 | 1000 | sign of the result |
| `strlen`, `strchr`, `strrchr`, `strstr` | vmcs | 1000 | 1000 | ISO C |
| `strcpy`, `strncpy` (zero fill), `strcat` | vmcs | 750 | 750 | ISO C, guard bytes |
| `strtoul` | vmcs | 300 | 300 | bases 0/2/8/10/16/36, leading space, end pointer |
| `qsort` with a VC II comparator (assembled by vc2dis, called through `bl rN`) | vmcs | 60 | 60 | sorted() |
| `sprintf` (`%d %u %x %X %s %c %%`, widths, `-`/`0` flags, up to 8 args) | vmcs | 150 | 150 | Python % formatting |
| `CeilLog2` | h264dec | 410 | 410 | bit length (isa.md's folded +1) |
| `h264_readue`, `h264_readse`, `h264_readue_byte` | h264dec | 1200 | 1200 | H.264 9.1 Exp-Golomb, codeNum up to 2¹⁶−2 |
| `h264_read1bit`, `h264_readte` | h264dec | 800 | 800 | te(v); read1bit returns **!bit** (te with range 1) |
| `h264_readbits`, `_advance`, `_more_advance` (n ≤ 16), `h264_advancebits` | h264dec | 1600 | 1600 | bit values and advance |
| `h264_readmvpair` (table path and `readse` fallback) | h264dec | 400 | 400 | two se(v), packed x \| y<<16 |
| `predresidual` | h264dec | 300 | 300 | rem_intra4x4_pred_mode rule (8.3.1.1) |
| `Bitstream_Set` | h264dec | 50 | 50 | struct fields, r16 |
| `calcmvpredictor` | h264dec | 568 | 568 | H.264 8.4.1.3 (median, single-reference rule, C←D, B/C←A) |
| `calcmvpredictor`, only D available | h264dec | 32 | 32 | Apple bug, see below |
| `vc_ShowBits`, `vc_GetBits`, `vc_FlushBits` | mpg4dec | 900 | 900 | bit values and advance |
| `H263GetCBPY`, `MPEG4GetCBPY` | mpg4dec | 400 | 400 | H.263 Table 13 / MPEG-4 B-8 (returns inter CBPY) |
| AES tables | passthru | 3 | 3 | S-box computed from GF(2⁸) inverse + affine map |
| `aes_rotword`, `aes_subword` | passthru | 600 | 600 | FIPS-197 |
| `aes_init` (128/192/256, bad sizes rejected) | passthru | 70 | 70 | FIPS-197 key expansion, App. A vectors |
| `mp_fifo_*` (11 functions) | mplayer | 1640 | 1640 | ring buffer, random operation sequences |
| `codec_string` | mplayer | 200 | 200 | fourcc → text |
| `aac_initbits`, `aac_get_processed_bits`, `aac_byte_align` | aacdec | 600 | 600 | byte-pointer bit reader |
| `adts_frame` (fixed + variable header + CRC) | aacdec | 300 | 300 | ADTS header (13818-7); struct offsets from the stores |
| **Total (scalar)** | | **20,576** | **20,576** | 1.54 M instructions simulated |

The vector rows (next section) add 1,446 cases and the boot rows ("Peripheral bus") 35: **68 rows, 22,283 cases, 0 failures** (seed 2722, about 5 s; 66 rows and 22,163 cases before P19's `jhoz`/`jvert` rows).

`--cover` lists the instruction kinds these runs execute: branch conditions al/eq/ne/cs/cc/hi/ls/ge/lt/gt/le (not vs/vc/mi/pl), push/pop, tbb/tbh, `bl rN`, the 16/32/48-bit loads and stores (indexed, post-increment, sp/gp forms, 48-bit `ld`/`ldh`/`ldb`/`ldsb`), and the ALU ops add/sub/rsub/mul/and/or/eor/bic/lsl/lsr/asr/bmask/btst/cmp/cmn/mov/neg/msb/adds2/adds4/adds8/min/max, `mulhd.uu`, `div.ss`, `div.uu`, `addscale8`. `sxtb` (26) runs only on 0/1 values, so it isn't really tested. **Not exercised** (semantics are VC IV-derived guesses): `brev`, `not`, `abs`, `sxth`, `adds16`, `mulhd.ss/su/us`, `div.su/us`, saturating `adds`/`subs`, `shls`, `clamp16`, `addscale5–7`, the 48-bit stores and `ldsh`, conditions vs/vc/mi/pl.

### Controls: does the test tell the hypotheses apart?

Each row flips one choice in `vc2sim.HYP` and reruns the whole self-test (`--controls`):

| Hypothesis (chosen) | Alternative | Failed cases | Rows broken |
|---|---|---|---|
| 48-bit op: hw0 bits 6/5 are offset bits 1/0 | ignore them | 660 | readue, readmvpair, predresidual |
| `(rs + ri)` scaled by access width | unscaled | 852 | sprintf, readmvpair, aes_init, calcmvpredictor ×2 |
| only cmp/cmn/btst write flags | every ALU op writes Z/N | 648 | 9 rows (memchr, strlen, readse, …) |
| cmp C = borrow (a <u b) | ARM C = a ≥u b | 3641 | 15 rows |
| cmn C = carry out | inverted | 131 | __ldivs |
| `2dxx` o = 4–7, 13 are sp-relative | gp-relative | 334 | sprintf, codec_string |
| btst sets Z when the bit is clear | set when the bit is set | 1421 | 7 rows |
| unary ops read rs (`neg rd, rs` = −rs) | read rd | 1692 | 6 rows |
| 48-bit pc-relative base = instruction address | next instruction | 1481 | strtoul and sprintf in vmcs.bin (raw image, Apple's own offsets), plus the .vll rows whose offsets our loader wrote |
| pc-relative ld/st base rounded down to a word (`pc48_mem_aligned`) | not rounded | 13 | the vmcs.bin boot (`ld r4, 0x1fc24` at 0xde0e faults) and the LCD row |
| push stores the **highest** register lowest, lr on top (`push_lowest_first` = False) | lowest register lowest | 1 | the vmcs.bin boot: 2 of 4,983 interrupted contexts come back clobbered (the library rows cannot tell) |

With the vector rows in the suite, `alu_sets_flags` also breaks `vc_interp_chr8x8/8x4` (its `cmp.32 r5, #8` decides whether the `vldq8` loads run) and `btst_z_when_clear` breaks the 4x4 intra DC rows (availability bits).

## Vector unit model (`vc2vpu.py`)

VC IV's VPU model with VC II's operand encoding as the experiments below pinned it. Every uncertain choice is a `v_*` key of `HYP`.

**State.** VRF of 64 × 64 bytes (`vrf.b`, row-major, offset y·64 + x); 16 per-lane accumulators (Python ints, unbounded); per-lane flags Z, N, C. The corpus addresses rows up to 63 and columns up to 63 and nothing beyond, so 64 × 64 (VC IV's size) fits; nothing proves the VRF is not larger.

**Vector layout (VC IV's).** Element *i*, byte *k* of an H vector at (y, x) is (y, x + i + 16k); of a V vector, (y + i, x + 16k). A 16-bit vector keeps its high bytes 16 columns on, so `H16(y, 0)` covers columns 0–31 and `V16(y, 32)` columns 32 and 48. 8-bit elements read unsigned, 16/32-bit signed. Coordinates wrap mod 64.

**Operands.**

| Operand | Decoding | Evidence (tests) |
|---|---|---|
| D | hw2 = `d8 rrr ttttt`. d8 = y5 y4 x5 x4 c3…c0 (c = y for H, x for V). **Direction and width come from the op's `.w` suffix** (op9[2:1]): 0 H8, 1 H16, 2 V8, 3 V16. t5 does **not** move D. | every row; control `v_d_dir_from_w` breaks 6 rows |
| D discard | a 16-bit D in x block 1 (`d10`) writes nothing. The 338 corpus uses of H16 `d10` are all dummies (acc chains, `setf`, `sum`); no 16-bit D ever sits in x block 1 or 3 otherwise. 8-bit `d10` is a real location (`alpha_blt_block`). | `v_d16_block1_discard` breaks 6 rows |
| t5 | describes **A**: bit 0 x += 32, bit 1 y += 16, bit 2 y += 32 (block of A), bit 3 x += 16 (assumed, only in `xbit_expand_remove3`; in a non-`x` 48-bit op: A takes the +r, `v_r48`), bit 4 see A | `v_t5_block_bits` breaks 8 rows |
| A (80-bit) | A11 = hw4[9:4]:hw3[15:11], +r(hw4[3:1]). **A11[10] = 1: H8 at (dy = A[9:6], dx = A[4:0])** plus the t5 block, element i at x + i; steps one row per REP iff t5 bit 4. **A11[10:8] = 0:** V8 column A[4:0] when t5 bit 4 (A[5] = step); `a000` without bit 4 is (0, 0) H8, in practice "A unused". **A11[10:8] = 2/3: 16-bit.** t5 bit 4: V16 column A[3:0], A[5] = step. Otherwise H16 row A[3:2]·4 + A[7:6]; mode 3 steps per REP, mode 2 doesn't. | `v_a_mode4_dydx` 7 rows, `v_a_mode4_step_t5` 4, `v_a_mode2_h_rows` 4 |
| A (48-bit) | 5 bits: the row (H) or column (V) index in D's direction and width, plus the t5 block | chroma deblocking `vasr/vmax/vmin d40.02, a00` |
| +r of a 48-bit op (P19) | one field (hw2[7:5]): in a non-`x` op it goes to A if t5 bit 3 is set and to B if hw3[1] is (both may), and then not to D | `jhoz`/`jvert` pair sums; `v_r48` = 'd' fails all 120 of their cases |
| 16-bit H D at +r (P19) | x part aligned down to 16, like a horizontal B's | `jvert`'s h at `d25.01+r0`; `v_d16_reg_x_align` = 0 fails its 60 cases |
| B | vector: d8 coordinate hw3[9:2]; **hw3[0] = V, hw4[11] (`.t`) = 16-bit**; `++` (hw4[10]) steps per REP; +r(hw4[14:12]). Scalar: rN + s12. Immediate: imm16 (signed; the `vmulhr` family reads B unsigned). 48-bit: hw3[0] = V, width = D's. hw3[1] is still open (`b0b.3`, `b20.2`, raises). | `v_b_v_bit0` 4 rows, `v_mulhr_b_unsigned` 2 |
| +rN | added to the operand's (y, x) **field by field**, x wrapping mod 64 without carrying into y | `vc_ChromaEdges_edge` with r6 = 32 reaches the transposed block at columns 14–17; `v_r_2d` |
| REP / inc | REP n from hw5[7:5] (80-bit) or hw3[8:6] (48-bit imm form); hw4[0] steps **D** (H: next row, V: next column); A and B step by their own bits | `v_inc_steps_d` breaks 8 rows |

**Execution of one repetition**: enabled lanes from the predicate (none, ifz, ifnz, ifn, ifnn, ifc, ifnc); read A and B; compute; `clra` zeroes the enabled lanes' accumulators; `acc` adds the result into them and the result becomes the accumulator; saturating ops clamp to D's width (8-bit 0…255, 16-bit signed); `setf` updates Z/N/C of the **enabled lanes only**, from the full-precision result; the scalar-result unit writes Σ, min or max of the enabled lanes' results into rN; D is written on the enabled lanes (truncated to its width).

**Operations modelled** (op6): 4 vmov (B), 8/9 vadd/vadds, 10/11 vsub/vsubs (A − B), 12/13 vrsub/vrsubs (B − A), 14 vmul, 16/17 vmulm/vmulms ((A·B) >> 8), 20/21 vmulhr/vmulhrs ((A·B + 0x8000) >> 16, B unsigned), 24 vshl, 26 vasr, 27 vlsr (logical on A's width), 28/29 vmin/vmax, 30 vdist (|A − B|), 44 vbitplanes (lane i ← bit i of B), 52 vmulhru (as 20 on the tested inputs), and since P19: 18/19/51 vmulhd/vmulhdsu/vmulhduu ((A·B) >> 16 floored; 19 B unsigned, 51 both), 37 vclip (clamp A to 0..B). Not modelled (raise): 42/43, 59, the `vop1xx` forms listed in isa.md, `x8`, sru function 0, `x15` on a saturating op or an 8-bit D.

**The 32-bit accumulator** (P19, `isa.md` "The 32-bit accumulator"): each lane holds `acc` (the high half, unbounded as before) and `acclo` (16 bits). An `x15` op adds at bit 0, the low 16 bits zero-extended (alone) or the signed result (with `acc`), carries into `acc`, and its result (D, flags, scalar-result unit) is the whole value `acc · 65536 + acclo`, so D keeps the low half. A plain `acc` op is unchanged (`acc += result`, result = `acc`). `clra` clears both. Code without `x15` never touches `acclo`, so every earlier count and golden is the same.

**Memory.** op9 < 0x20 = kind (op9[4:3]: ld, st, ldp, ldq) · w (op9[2:1], **the same H8/H16/V8 code as the ALU `.w`**) · 0. A V8 load puts memory byte i in row y + i of one column (`vld8v`, formerly printed `vld32`).
- 48-bit: D = d8 (no +r), address r(base) + off · **8 bytes** (15-bit signed field), `+=rS` adds rS per REP, D steps one row (H) or column (V) per REP. `vst16` stores 32 bytes.
- `vldp`/`vldq` are the first and second 16-byte line of a misaligned access: `vldp` writes the lanes whose bytes lie in the line of the start address, `vldq` the rest. Apple's code issues `vldq` exactly when the needed bytes cross a line (chroma: `(x | stride) & 15 > 8`; 6-tap: alignment > 11), and the chroma rows pass at every alignment. Whether `vldp` alone would also fetch the crossing lanes cannot be told from the corpus (control `v_ld_split_lines`: 0 failures).
- 80-bit `.1f a3f` loads (`vldp8 d40.1f, (r0-2)+=r1 rep16, a3f bx=3f inc`): plain loads at r(base) + off7 **bytes**; hw4[12] set (`bx=3f`) loads one byte further on, so the x − 2 loads deliver the left neighbour at the D column; hw4[0] steps D. Not modelled: the `D+r0 / A+r0 bx=38` loaders of the 6-tap (they must deliver src + 14 … at column 48; `vldq` is issued only when (src + 14) & 15 > 11, so they load from src + 14 with the line split), `vc_calc_strength_load` (`bx=00`, hw5 0x00c7) and the 80-bit stores.
- `vstr` (0x170): stores the enabled lanes of A (bytes) at r(base) (+= rS per REP); the D field only supplies A's t5 block.

## Vector experiments (seed 2722)

Each row runs one of Apple's kernels on random inputs (random VRF background, random memory) and compares the result with a model written from the H.264 specification. Where a kernel's inputs come from loaders that aren't modelled, the test places the inputs in the VRF itself; that layout is part of the claim.

| Kernel(s) | Cases | Pass | Reference | Hypotheses it discriminates (controls) |
|---|---|---|---|---|
| `invzigzag` | 100 | 100 | 4x4 frame zigzag (Table 8-13), V16 columns | D from `.w`, D inc |
| `vc_inverseinteger` | 100 | 100 | 8.5.12.2 4x4 transform, 16 blocks in the lanes | D inc, A mode 2 V |
| `vc_interp16xN_hoz` / `_vert` | 60 + 60 | 120 | 8.4.2.2.1 half-pel b / h (6-tap, (Σ + 16) >> 5, clip) | A mode 4 (dy, dx), d10 discard, 8-bit unsigned, saturation |
| `vc_interp16x16`, dx = 0 | 80 | 80 | G, d, h, n; 48-bit `vldp8`/`vldq8` at any alignment | `vmulhrs` B unsigned, A step (t5 bit 4), mode 3 |
| `vc_ChromaEdges_edge`, bS = 4 | 120 | 120 | 8.7.2.4 chroma strong filter, alpha/beta tests | +r 2-D add, flags only on enabled lanes, B V bit, t5 block, mode 2 H rows |
| `vc_intra_4x4_*` + `storeback` | 336 | 336 | 8.3.1.2: all nine modes, both above-right cases, residual added and stored | V8 loads, 80-bit `.1f` loads (+1 byte), A mode 4, `vstr`, vshl/vlsr/vmulhrs |
| `h264_calculateintrapred16x16` | 140 | 140 | 8.3.3: vertical, horizontal, DC (4 availability cases), plane | 48-bit offset unit 8, V8 above row, accumulator chains |
| `vc_interp_chr8x8` / `8x4` | 120 | 120 | 8.4.2.2.2 eighth-pel bilinear, any alignment | mode 0 V, mode 2 H rows + t5 y block, d10 discard, `vldq` split |
| `h264_combinepred` | 40 | 40 | clip(pred + residual), luma and chroma | B V16, A mode 0 step |
| `vc_storeback` | 40 | 40 | 16 luma rows + 8 U/V rows (`vstr ifnz`, stride / 2) | A mode 6 (dy = 8), t5 block |
| `h264_clearnzcoeffs` | 40 | 40 | 32 zero bytes, guards intact | `vst16` width |
| `alpha_blt_block` (slideshow) | 30 | 30 | per byte: (s2·a >> 8) + (s1·(255 − a) >> 8) | vmulm, vrsub #-1, 8-bit `d10` is real |
| `vc_copy8x4` | 60 | 60 | masked row copy (its plain meaning; no spec equation) | `vbitplanes rN`, predicated REP moves, +r |
| `vc_interp16xN_jhoz` (P19) | 60 | 60 | 8.4.2.2.1 centre half-pel j = Clip1((j1 + 512) >> 10), half the cases high-contrast | `x15` (zero-extended low half, carry), `vmulhd` floored, `vclip` lower bound, 48-bit +r enables |
| `vc_interp16xN_jvert` (P19) | 60 | 60 | j unclipped (16-bit) and the vertical half-pel h/m beside it | as `jhoz`, and a 16-bit D's +r x aligned |
| **Total** | **1,446** | **1,446** | | |

Controls (`--controls`, vector rows only, before the `vc_copy8x4` row; failures out of 1,266):

| Hypothesis (chosen) | Alternative | Failed | Rows |
|---|---|---|---|
| D direction/width from `.w` | from t5 bit 4 | 430 | 6 |
| 16-bit D in x block 1 discards | written | 597 | 6 |
| t5[2:0] = A block (x+32, y+16, y+32) | ignored | 843 | 8 |
| mode 4–7 A steps iff t5 bit 4 | never | 133 | 4 |
| mode 2/3 H row = A[3:2]·4 + A[7:6] | A[3:0] / A[7:0] | 289 / 289 | 4 |
| mode 4–7 = (dy = A[9:6], dx = A[4:0]) | swapped | 628 | 7 |
| B direction = hw3[0] (width = hw4[11]) | swapped | 542 | 4 |
| 8-bit elements unsigned | signed | 809 | 8 |
| `vmulhr` B unsigned | signed | 91 | 2 |
| saturation to D's width | to 16 bits | 699 | 6 |
| `setf` under predication: enabled lanes only | all lanes | 93 | 1 |
| inc (hw4[0]) steps D | D fixed | 618 | 8 |
| +rN field-wise (x wraps) | linear y·64 + x | 33 | 1 |
| 48-bit vector offset × 8 | × 1 / 2 / 16 | 20 each | 1 |
| `vldp`/`vldq` split by line | `vldp` loads every lane | **0** | not determined |
| `x15` zero-extends alone, signed with `acc` (P19) | sign-extended alone / read as plain `acc` | 105 / 120 of 120 | 2 |
| `vmulhd` floors | rounds (ties away) | 45 of 60 | 1 |
| 48-bit +r per t5 bit 3 / hw3[1] | D gets it | 120 of 120 | 2 |
| `vclip` clamps to 0..B | min(A, B) | 60 of 60 | 1 |
| 16-bit H D's +r x aligned to 16 | as given | 60 of 60 | 1 |

Static coverage: 90.9% of the corpus' vector bytes decode to a modelled form. The rest: other 80-bit memory forms 2.9%, `x15` 0.9%, and unnamed ops of 0.1–0.5% each (`vop02a`, `vop028`, `vclip`, `vop150` with B type bit 1, `vop15a`, `vop092`, `vop1da`/`vop1d8` tc computation, …).

## Semantics corrections and additions for isa.md (with evidence; folded into isa.md 2026-10-10)

1. **48-bit memory family `2f0d`/`2f4d`/`2f8d`–`2fed` (replaces `mem0.2`, `mem2.2`, `mem4.x`… and "unknown (half?)").** Opcode = hw0 bit 7 plus hw1 bits 1:0: bit 7 = 0: 0 `ld`, 1 `st`, 2 `ldh`, 3 `sth`; bit 7 = 1: 0 `ldb`, 1 `stb`, 2 `ldsh`, 3 `ldsb`. Offset = (o27 & ~3) | hw0 bit 6 << 1 | (hw0 bit 5 if bit 7). (With bit 7 = 0, bit 5 = 1 is `lea`/`mov32`, so word/halfword ops lose offset bit 0 only.) Evidence: `h264_readue` `2f42 ffe2 87ff` must load 16 bits ahead from r16+62 (offset −30) while `vc_GetBits` `2f03 ffe2 a7ff` with the same o27 matches `vc_ShowBits`' explicit `ldsh r3,(r1-32)`; `predresidual` `2f43 0002 8000` reads the window one bit on (+2); `vc_readzerosdc1` reads two consecutive bits' signs from `2f80`/`2fc1` at the same r16 (+0/+2); `h264_readmvpair` `2fa0 0000 8000` indexes a 256-entry table with the next 8 bits (byte at +1 of a little-endian window); `adts_error_check` `2f83 0007 0000` = `ldsb (r0+4)` tests protection_absent stored at +4. R17 (passthru `.sdata` short) and R18 sites match the halfword/word rows. Control: 660 failures without the bits.
2. **`2dxx` is gp- *and* sp-relative.** o = hw0 bits 7:4: o 0–3 gp-relative, o 4–7 sp-relative, with o & 3 the width class of the 32-bit 28/29/2a/2b family (0: word ld/st or `ldsh` by offset low bits, 1: `ldh`/`sth`, 2: `ldb`, 3: `stb`); o 12 = `lea rd, gp+u16` (as vc2dis has it), o 13 = `lea rd, sp+u16`. Offsets unsigned. Evidence: `codec_string` (`lea r4,(sp+3)` walks a fourcc stored at sp+0), `psnprintf` (`ldb r5,(sp+20)` reloads the spilled r2), `AACDecode_Ittiam` (frame 584 bytes, stores at sp+0x224/0x228), `h264_fastinter8x8` (`sth`/`ldh` at sp+0x80…0x84). The `.vll` libraries never set gp. Control: 334 failures with gp.
3. **Indexed loads/stores `(rs + ri)` scale the index by the access width** (open question 6): `h264_readmvpair` uses one index for a byte table and the word table 1024 bytes below it; `aes_init` indexes per-context word arrays. Control: 852 failures unscaled.
4. **Flags:** only `cmp`, `cmn`, `btst` (and `cmp rd, #0`) write them; `add`, shifts and `addcmpb` don't (`memchr`: `cmp r2,#0; add r2,#-1; beq`; `h264_readse`: `btst; lsr; asr; add r16,#62; beq`). cmp's C is a **borrow** (cs = unsigned lower, hi = !C && !Z), cmn's C is the carry out. btst sets Z when the bit is clear.
5. **ALU op names:** 30 = `abs` (mpg4dec: `sub; abs; rsub; abs; addcmpbge` = \|a−b\| ≥ \|c−b\|; `lsr r0,r8,#31; abs r8,r8` sign/magnitude), 27 = `sxth`, 26 = `sxtb` (unary sign extensions: `alu27` follows `ldh`/`sth` and precedes signed tests such as `addcmpbge r3,#0` that would be pointless after a zero extension; `alu26` copies a value just stored with `stb`). Confidence medium for 30, low-medium for 26/27 (no test discriminates them). 9, 17, 29 never occur; the unused VC IV ops (ror, bset, bclr, bchg) are the candidates. `min`/`max` are **signed** (`calcmvpredictor` with negative MVs).
6. **Unary ops read rs** (`neg rd, rs` = −rs; `msb`, `neg`, `abs`, `brev`, `not`, `sxt*`).
7. **48-bit pc-relative base is the instruction's own address** (`sprintf`'s callback `lea r0, 0x10336` and `strtoul` in vmcs.bin work only with it), **rounded down to a word for loads and stores** (not for `lea`): all 15 pc-relative word ld/st in vmcs.bin at an address = 2 mod 4 are unaligned otherwise, and the boot faults at the first one (0xde0e).
11. **push order:** the block is stored with the highest register at the lowest address and lr (pc for pop) on top. Interrupt handlers save `r0-r5, lr` and then `r6-r15` in two pushes and restore everything with one `pop r0-r15` (vmcs.bin 0x2122, 0x2202), which only works this way round; 4,983 interrupted contexts in the boot run come back intact, 2 clobbered with the other order.
12. **rti** pops sr from [sp] and pc from [sp+4] (sp += 8); interrupt entry pushes them and clears sr bit 30. Nucleus starts its first task through it (frame sr = 0x40000000 below the entry point).
8. **Calling convention** as above: promote r0–r5/r6–r15 to high confidence, stack arguments at the caller's sp (high), r16–r23 unused by compiled code.
9. `bl rN` (`4730|d`) confirmed by `qsort` calling our comparator.
10. Function-level facts for the corpus notes: `h264_read1bit` returns **the inverted bit** (it is te() with range 1); `h264_readbits_more_advance` reads its second window 8 bits on instead of 16 (`2f02 0012 8000`), so it is wrong for n > 16 (9/200 correct, 200/200 match the "8 bits on" model), which valid streams never request (all callers read ≤ 16 bits); `calcmvpredictor` returns the MV at index refIdx when only neighbour D is available (`mov r1, r4` after `ldh r4,(r0+4)`), a case that cannot occur in valid streams (A, B unavailable imply D unavailable). The ADTS parser stores profile + 1 when ID = 1.

## Open points

Vector unit:
- The 6-tap `D+r0/A+r0 bx=38` loaders (`vc_interp16x16_loadh`/`loadall`, also `_T`), `vc_calc_strength_load` and the 80-bit stores; the edge-replication `vadd.2x`/`vadd.0x` ops (op9 bit 0, with D t5 0x19/0x09/0x0f/0x18).
- Ops 42/43, 59 and the `vop1xx` group; 34 (add with carry? it needs the C flag of a 16-bit add, which the model's `setf` does not yet give as an unsigned carry); sru function 0 (in the audio FIR at vmcs `0x10730` it sums the low-half accumulators: modelled only if needed); B type bit hw3[1] of 80-bit ops; t5 bit 3 of 80-bit ops; the 48-bit `x` forms' +r. Done (P19): `x15`, 18/19/51, 37, the 48-bit +r enables.
- The deblocking normal (bS < 4) path and the luma edges remain untested.

Scalar:

- `bmask` with a count of 0 or ≥ 32, `msb(0)` (−1 assumed), shift counts ≥ 32 (taken mod 32): unexercised.
- The saturating/extended ops, 26/27/30 and the stores listed under "not exercised".
- Remaining `2dxx` o values (8–11, 14, 15), `misc4` selectors (`sys19 rd` reads a value, modelled as 0; `sys20`–`sys23 r0` sit next to VRF save/restore code), `bkpt`.
- Imports beyond the stub list (DMA, RTOS): any function that calls them stops with `Unimplemented`.

## vc2dis API (wish list done, 2026-10-10)

1. 48-bit memory ops decode as `ld/st/ldh/sth/ldb/stb/ldsh/ldsb` with the exact offset (`vc2dis.mem48_decode` / `mem48_encode_offset`; the simulator's versions wrap them so the `mem48_offset_bits` control still works); `2dxx` decodes gp (o 0–3) and sp (o 4–7) accesses by width class and `lea rd, (sp+u16)` (o 13). `mem0.2`/`mem{f}.{c}` are gone.
2. `ins.values()` returns `{field: number}`: registers as numbers, immediates scaled and signed, targets absolute, name tables as their index, vector operands as small dicts (`d8/reg/t5`, `a/reg`, `b8/type/t/reg/step` or `imm`/`sreg`, modifiers, address). `vc2sim.fval` uses it.
3. `Vll` is the one ELF reader: `sections` (named tuples), `symtab` (name, value, size, info, shndx), `rela` (section, offset, type, symbol index, addend), `alloc_sections()`, `reader()`; the older `sec`/`syms`/`relocs`/`funcs` attributes are kept.
4. `--raw FILE.vll [START [END]]` maps sections at their link addresses (default `.text`).
5. ALU 26/27/30 print as `sxtb`/`sxth`/`abs`. Vector memory ops print V loads as `vld8v`/`vldp8v`/`vldq8v`/`vst8v` and 48-bit offsets in bytes.

## Next steps

1. ~~Write the first **own** VideoCore code, a bring-up "hello" that answers `BCMCMD_LCD_UPDATE`.~~ Done as `bcmfw/` (milestone 3, `bringup.md`): 28 bench rows and 643 cases on the modelled bus. Next is the DMA pixel path. The vector unit is now usable for its blit/blend loops (`vld8/vst8` with `+=stride rep16`, `vmulm` blends as in `alpha_blt_block`).
2. Vector: the 6-tap `bx=38` loaders (target stated above), then the deblocking normal path (op 59; 37 is done), each against its H.264 definition (`jhoz`/`jvert` done, P19); `xbit_expand_*` against the bit-window table described above.
3. Extend the scalar self-test to the ops still unexercised (find corpus functions that use them, or assemble targeted snippets once the semantics are pinned).
