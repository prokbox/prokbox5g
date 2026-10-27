# Programming the BCM2722: a guide (start here)

*How to write, test and verify your own VideoCore II image for the iPod Video's video chip. This page is the narrative path; the reference chapters hold the detail and the evidence. Read it top to bottom once, then use it as a map.*

This guide adds no findings of its own. Every statement carries the evidence tag of the page it comes from and links to that page; when the two disagree, the reference page wins. It is written in our own words from our own sessions and our own reading of the reference corpus: no Broadcom or Apple text or code is reproduced here.

**Evidence tags** (the full legend is in [the index](index.md#evidence-tags)):

| Tag | Means |
|---|---|
| **[silicon]** | Measured on the owner's iPod Video 5.5G with our own code, cited by session (S1 to S37) or by a 2026-10-25 bench log |
| **[apple-RE]** | Read from Apple's VideoCore or ARM code (statically, or run in our simulator): what Apple's code *does* |
| **[rockbox]** | From Rockbox's iPod Video LCD driver |
| **[sim]** | Shown only in our simulator (`vc2sim`) or a host test |
| **[hypothesis]** | Our inference; never promoted without new evidence |
| **[ours]** | A convention or design of our own firmware, not a property of the chip |

**Status of this page:** first written on `p19` at `799ff0f8d7` (the image batch of 2026-10-25); brought up to date on 2026-10-26 with bench sittings 3, 4 and 4b and the 3D stage (the loop filter's forms, the late-write rule, the single overlay region, bounded waits, the first-use check). §13 lists what is still thin.

Older, shorter route through the manual: [chapter 16](programming-guide.md). This guide supersedes it as the entry point.

## Contents

1. [The chip in one page](#1-the-chip-in-one-page)
2. [Booting an image from the host](#2-booting-an-image-from-the-host)
3. [The memory map and the alignment rules](#3-the-memory-map-and-the-alignment-rules)
4. [Instruction forms and their silicon semantics](#4-instruction-forms-and-their-silicon-semantics)
5. [The vector unit](#5-the-vector-unit)
6. [DMA](#6-dma)
7. [Clocks and supply levels](#7-clocks-and-supply-levels)
8. [The job ABI](#8-the-job-abi)
9. [Writing a kernel](#9-writing-a-kernel)
10. [Testing in vc2sim](#10-testing-in-vc2sim)
11. [Verifying on the device](#11-verifying-on-the-device)
12. [Lessons learned](#12-lessons-learned)
13. [Where the evidence is thin](#13-where-the-evidence-is-thin)

---

## 1. The chip in one page

**What it is.** One VideoCore II scalar core with a 16-lane vector unit (VPU), 1.25 MB of internal SRAM, 4 MB of stacked SDRAM, an 8-channel DMA engine, an LCD interface, and a 16-bit host port through which the PP5022 reaches every BCM address ([overview](overview.md), [memory access §1](memory-access.md#1-the-memories-at-a-glance)). On the iPod it owns the panel; the PP owns everything else ([data paths §0](../data-paths.md#0-how-to-develop-on-this-platform-one-screen)).

| Part | What to know first | Evidence |
|---|---|---|
| Scalar core | VideoCore IV's semantics in a different encoding; 16/32/48/80-bit instructions; a dependent chain runs 1 instruction a cycle, independent work about 1.9 (dual issue) | [apple-RE] encodings; [silicon] S6, S7 timing; dual issue [hypothesis] ([isa-scalar](isa-scalar.md)) |
| Vector unit | 16 lanes, a 64 × 64-byte 2-D register file (VRF) read as rows or columns, REP up to 64, a 32-bit accumulator per lane | [apple-RE]; [silicon] S4 onward ([isa-vector](isa-vector.md)) |
| Clock | 27 MHz crystal after a host start; PLL A takes it to 150 MHz | [silicon] S7 ([boot-and-clocks §4](boot-and-clocks.md)) |
| SRAM | `0x0000_0000–0x0013_FFFF`, same address from the host; images load at 0 and enter at `0x200` | [silicon] S1 onward ([memory-access §2](memory-access.md#2-internal-sram)) |
| SDRAM | `0xFE00_0000` from the core, `0xC000_0000` from the host; dead until an image runs the bring-up | [silicon] S10, S11 ([sdram](sdram.md)) |
| Caches | 128 Kbit I + 128 Kbit D. **Every image of ours runs with the cache off**, which makes host and core writes visible at once | [external] size; [silicon] every session; [ours] the policy ([memory-access §4](memory-access.md#4-caches)) |
| Peripherals | `0x1000_0000–0x1000_FFFF` in `0x400` blocks, 32-bit accesses only; many registers want `0xA5A5` in bits 31:16 ("pw") | [apple-RE]; [silicon] ([memory-map](memory-map.md)) |
| Host port | 16-bit data bus; only the PP masters it; no path is known by which the BCM writes PP memory | [rockbox]; [silicon]; the last point [hypothesis] ([host-interface](host-interface.md), [data paths §0](../data-paths.md#0-how-to-develop-on-this-platform-one-screen)) |
| Power | The PP switches the BCM's supply (`GPO32` bit 14). A cut loses SRAM, SDRAM and the VRF: every wake is a cold upload | [rockbox]; [silicon] ([power](power.md)) |

**What runs where** [ours]: the PP decides and keeps the reference copy of everything; the BCM does pixel work and drives the panel. Every BCM job has a bit-exact CPU fallback on the PP ([jobs §7](../research/bcm2722/jobs.md#7-the-cpu-fallback-mandatory)), and nothing on the BCM is the only copy of anything ([memory contract §1](../research/bcm2722/memory-contract.md#1-who-owns-what)).

**The images** [ours] (sizes and FNV-1a hashes after the 2026-10-25 batch, [merged-image-batch §2](../research/bcm2722/merged-image-batch.md)):

| Image | File / name | Size | What it carries | Role |
|---|---|---|---|---|
| Display driver | `hello_dma` | 8,016 B | Rockbox's LCD command word, DMA frame push | the release boot path's verified rung |
| Plain (probe) | `bcmvideo.bin` (`bcmprobe.bin` on the device) | 220,964 B | H.264 video, AAC, the protocol, every lab and probe op (`PEEK`, `PINS`, `CACHE`, `DMACPY`, …) | developer probes, stages 6–9 |
| Merged | `bcmvideo-jobs.bin` | 218,692 B | the plain image's video plus the display jobs; **68 B** of room below its arena | the resident image of a `--bcmfw-merged` build |
| Display-only | `jobs_display` | 46,880 B | the jobs, JPEG `DECODE`, the overlay region; no video decoder | the warm-swap candidate (B13) |

How each build picks among them, and the boot ladder, are in [memory contract §6](../research/bcm2722/memory-contract.md#6-l2-our-image-as-the-default-and-verifiedtxt).

### Worked example: which image is running?

Every image of ours publishes its identity in low SRAM, so the host can tell without any protocol ([bcmfw README, "The host contract"](../../bcmfw/README.md)) [ours]:

```
read 0x1F8   -> 0x50420001 ("PB", ready) once the image has started; 0xFFFF while busy;
                0xFA170000 after a fault (then 0x1E0/0x1E4 hold the fault pc and sp)
read 0x1E8   -> info word: 0x5042 in bits 31:16; panel ID in 1:0; 0x100 panel powered;
                0x200 1 MHz counter running; 0x800 DMA build (the README's table)
read 0x100   -> HP_VERSION = 'VP' << 16 | major << 8 | minor (0x56500104 today), or 0:
                no protocol (hello_dma, or Apple's NOR firmware)
read 0x104   -> capabilities: HP_CAP_JOBS_2D (0x0100) means the job block exists (§8)
```

Read every word that the chip may be rewriting with a torn-read check (§2).

---

## 2. Booting an image from the host

### The sequence

The chip's boot ROM answers a handshake on the port; the host then writes the image into SRAM and starts the core. Rockbox has done this since 2006 and every session of ours has used it ([host-interface §1, "Starting an image"](host-interface.md)) [rockbox; silicon S1–S37]:

```c
/* host side (PP5022): the shape of bcm_init(), with our bounded waits */
GPO32_VAL |= 0x4000;  sleep_ms(50);              /* BCM power on */
STRAP_OPT_A &= ~0xF00;  write32(0x70000040, 0x1313);
wait(ALT_CONTROL & 0x80 clear, & 0x40 set);
for each b in {A1 81 91 02 12 22 72 62}: CONTROL = b;
for each b in {02 12 22 72 62}:          ALT_CONTROL = b;
wait(RD_ADDR & 1 and ALT_RD_ADDR & 1);  (void)WR_ADDR; (void)ALT_WR_ADDR;
wait(ALT_CONTROL & 0x80 clear, & 0x40 set);
port_write(0x00000000, image, len);              /* an even number of halfwords */
port_write32(0x1F8, 0);                          /* clear the ready word */
port_write32(0x10000C00, 0xC0000000);            /* cache: flush request */
wait(port_read32(0x10000C00) & 1);  port_write32(0x10000C00, 0);
port_write32(0x10000400, 0xA5A50002);            /* start: core enters at 0x200 */
wait(port_read32(0x1F8) != 0, 2 s);              /* the image's ready word */
```

What the meaning of the bootstrap bytes is remains unknown [hypothesis: a boot-ROM handshake] ([open questions #24](open-questions.md)). What the core finds at `0x200` [silicon S1, S2, S7; apple-RE]: undefined registers, the cache off, the core at 27 MHz, the 1 MHz counter **stopped**, interrupt priorities undefined ([boot-and-clocks §1](boot-and-clocks.md#1-getting-code-to-run)).

So an image's first instructions, as `bcmfw/crt0.S` and `main.c` do them [ours, silicon every session]:

1. disable interrupts; set gp (r24) and sp (r25, 4-aligned);
2. clear `.bss`;
3. write every interrupt priority to 0;
4. start the 1 MHz counter if it is not running ([boot-and-clocks §3](boot-and-clocks.md#3-the-1-mhz-counters-clock)): `0x10001C4C = 0xA5A5_0501`, `0x10001C54 = 0xA5A5_0000`, about 1 µs, `0x10001C4C = 0xA5A5_051B`, `0x10001C54 = 0xA5A5_0000` [silicon S4];
5. read the panel ID (GPIO 51/50; [lcd §2](lcd.md#2-panel-identification-and-pins));
6. a protocol image fills the header at `0x100` and writes `HP_VERSION` **last**, so 0 always means "not up";
7. write the ready word `0x1F8 = 0x5042_0001`.

Point every vector word (`0x000–0x0FF`) at a stub that records the fault and stops cleanly; an image that faults must never leave `0x1F8` in a value that makes the host wait for ever ([bcmfw README, "Fault"](../../bcmfw/README.md)) [ours].

### The host-port protocol

The port carries memory traffic only; every protocol is a convention in BCM memory ([host-interface §3](host-interface.md)). Ours ([hostproto §1](../research/bcm2722/hostproto.md#1-layout)) [ours; silicon S10–S37]:

- **A 64-byte header at `0x100`**: version, capabilities, the command ring (host → chip, `0xD0000`, 2,048 words), the event ring (chip → host, `0xD2000`, 1,024 words), the bitstream FIFO, four indices, `STATE` (a heartbeat), `FIFO_RD`, `HP_JOBS` (`0x138`).
- **Records**: a header word `op | n << 8 | tag << 16`, then n payload words (at most 32). Replies carry `op | 0x80` and the tag, status first. Events are `0x60–0x7F` with tag 0, so they can never alias a reply.
- **One writer per word.** The host writes the command ring, `CMD_WR` and `EVT_RD`; the chip writes everything else. No locks between the chips.
- **Torn-read proof.** A 32-bit word crosses the port as two halves, so a reader can see half an update [silicon S12, quirks #23]. Every shared index is stored as `HP_ENC(v) = ~v << 16 | v`, and a word failing `HP_VALID` means "not yet", never "dead" ([data paths D7](../data-paths.md#d7-torn-reads-mistaken-for-a-dead-port)).
- **No wait without a deadline**, on either side, and **every host wait on the chip is bounded and logs a FAULT when it expires.** The chip never waits on the host; the host bounds every handshake (20 ms in our port layer) because an unpowered port reads `0xFFFF` per halfword and an unbounded wait hangs the PP [rockbox; silicon; ours]. Sitting 4 (2026-10-26) ended in three silent freezes; the audit that followed found waits that held the port lock with no bound (the display sleep's wait for the last update, the wake's wait for the first), and the rule now covers every wait (1 s for the display sleep, 3 s for the first update after a wake, 2 s for an update in flight with Apple's program and 6 s in job mode, 500 ms for a job-driver suspend). The table is in [host-driver §2a](host-driver.md); a hang that still happens leaves a record on the PP ([PP quirks #56, #57](../pp5022-reference/quirks.md)).
- **Write ahead; read back before a close command.** A host write into SDRAM may not be seen by the core when a command written afterwards arrives: a `FEED` sent the moment its packet was written gave no picture at any clock band in sitting 4b [silicon symptom; mechanism hypothesis]. Put data in well ahead of the command that uses it, and when the command follows closely read the last word back through the port first ([quirks #47](quirks.md), [open question #73](open-questions.md), [host-driver §4](host-driver.md)).
- **The doorbell.** Rockbox writes `CONTROL = 0x31` after each command word [rockbox]. The BCM has a host doorbell interrupt (source 23, acknowledged at `0x1000_3800`) [apple-RE], but which host write raises it is [hypothesis] and no interrupt source has been confirmed on silicon ([open questions #3](open-questions.md)). **Our images poll** the rings and `0x1F8` [ours].

**The port's rules** ([memory-access §7a](memory-access.md#7a-the-protocol), [§11.4](memory-access.md)):

| Rule | Evidence |
|---|---|
| A 32-bit access to `DATA` is two 16-bit transfers, low half first; an address goes to `WR_ADDR`/`RD_ADDR` the same way | [rockbox]; [external] |
| Writes auto-increment after one address; each read sets its own address and waits for `CONTROL` bit 4 | [rockbox]; [silicon] every upload; read auto-increment untested (#43) |
| **The write address ignores bits 1:0.** A write asked at 2 mod 4 lands from the start of its word, 2 bytes low | [silicon, inferred] CR1 2026-10-25 (quirks #34); the ALGN rows confirm it directly |
| Use only the main bank after the bootstrap; what the ALT bank addresses is unknown | [rockbox]; PW7 failed on silicon (#25) |
| Longest handshake under a decode load: 47 µs, no timeouts | [silicon] S12 |
| The PP's general DMA into the port has never delivered a byte; the second engine ("engine 2") does, about 11 MB/s | [silicon] S34, S35 ([data paths D1](../data-paths.md#d1-the-pps-dma-can-feed-the-bcms-host-port)); S37 |

### Worked example: one `HELLO` round trip

Assume both ring indices are 0 after start (`hostproto.h`: `HP_OP_HELLO` = 1, `HP_REC`, `HP_ENC`) [ours]:

```
host:  write 0xD0000 = 0x00420101       ; HP_REC(HELLO, n=1, tag=0x42)
       write 0xD0004 = 0x00000104       ; the host's version, major 1 minor 4
       write 0x120   = 0xFFFD0002       ; CMD_WR = HP_ENC(2): published after the data
chip:  (its next poll) reads the record, replies into the event ring at 0xD2000:
       0x00420481                       ; HP_REC(HELLO | 0x80, n=4, tag=0x42)
       0x00000000                       ; HP_OK
       0x56500104                       ; HP_VERSION
       caps, 32                         ; capabilities, max payload words
       then 0x128 = HP_ENC(5)           ; EVT_WR, after the words
host:  poll 0x128 until HP_VALID and changed (bounded); read 5 words; write 0x12C = HP_ENC(5)
```

The port keeps writes in order, so a reader never sees an index ahead of its data [silicon: every session]. A round trip costs 6–7 µs plus the port writes [silicon S2]. A different `major` in the reply means "don't use this image"; a higher minor only adds.

---

## 3. The memory map and the alignment rules

### Who owns what

The chip has no arbitration a program can see: a host write lands whenever it arrives, and nothing tells either side the other is mid-word [silicon S12: 0 wrong while the core decodes]. **Safety is entirely by convention**: every region has one owner and a rule for when the other side may touch it ([memory-access §8](memory-access.md#8-who-may-touch-what-when-ours-unless-tagged)). The merged image's SRAM map ([memory contract §2](../research/bcm2722/memory-contract.md#2-sram-125-mb-0x0000000x13ffff-the-merged-display-image)) [ours]:

```
0x000000  vectors (0x000-0x0FF), VP header (0x100), image words (0x140-0x1DF), LCD mailbox (0x1E0-0x1FF)
0x000200  the image: code, data, .bss  -- must end below 0x035400
0x035400  UI mode: slot pools (DRIFT, STRIP, THUMB, APP) | VIDEO mode: the H.264 arena
0x0CC000  stack (16 KB, top 0x0D0000)
0x0D0000  command ring 8 KB, event ring 4 KB, DMA control blocks
0x0D4000  J_CLS_SCRATCH 32 KB
0x0DE000  job block (HP_JOBS points here), slot table, park stub 0x0DE900, hibernate stub
0x0E0000  J_SURF_FRONT: the 320x240 RGB565 frame (Rockbox's 0xE0000 contract)
0x105800  J_SURF_BACK: the second frame
0x12B000  spare; the parked AAC tables 0x134000-0x13FFFF until the first video OPEN
```

- The PP writes a slot only between `ALLOC` and `COMMIT`; it writes the rings, the host frame and nothing else outside a slot it was given [ours].
- Host writes into every pool work after the start [silicon] CR1 2026-10-25: the control area and 7 pool addresses, 0 wrong words, by our write form and each of Apple's.
- SDRAM is nobody's until an image has brought it up; the merged image brings it up only at a video `OPEN` [ours; [memory contract §3](../research/bcm2722/memory-contract.md#3-sdram-4-mb-on-demand-only)].
- Low SRAM is where a stray vector store lands (a base field of 7 means address 0, §5), so our images keep a guard block far away at `0xDF000` [silicon S13–S15; ours].
- The pools and their budgets are constants in `jobs.h` (`J_BUDGET_*`), and `build.py` checks the layout against them ([memory contract §4](../research/bcm2722/memory-contract.md#4-budgets-per-class-rev-01)) [ours].

### The alignment rules

On this chip **an alignment bug shows up as wrong data, not a crash** ([memory-access §11](memory-access.md#11-alignment)): a misaligned scalar load completes from the rounded-down address and faults a few instructions later; a plain vector load silently reads the aligned row; the host port silently drops the low address bits. The rules our code follows [ours, built on the tagged facts]:

| Path | Rule | Why |
|---|---|---|
| Scalar | every access naturally aligned; a word at 2 mod 4 or a halfword at an odd address is built from bytes | misaligned load: rounded down, then an imprecise exception [silicon S2, S3]; misaligned stores unknown (#5) |
| Vector | plain `vld`/`vst` only at their row's alignment (32 bytes for 16-bit rows, 16 for 8-bit); strides multiples of the row; misaligned data through a `vldp`/`vldq` pair or scalar code | plain loads align down [silicon S18–S22, quirk #17a]; `vstr` drops lanes past its 16-byte line [silicon S22, quirk #18] |
| DMA | units of 4 or 32 bytes, each address a multiple of its unit; control blocks 32-byte aligned | [apple-RE]; [silicon] S5, PW8; sub-word units unknown (#69) |
| Host port | every write starts 4-aligned; an odd first or last halfword goes by read-modify-write of its word | quirk #34 [silicon, inferred]; a lone trailing halfword unknown (PLH1, #70) |
| Images | every slot's pitch is its width rounded up to 16 pixels (`J_SLOT_PITCH`), so rows start 32-byte aligned | Apple's VC image pitches are the same [apple-RE]; host-described pitches round to 4 bytes [apple-RE] |

Apple's conventions, which ours now follow wherever they differ, are in [memory-access §11.5](memory-access.md) [apple-RE]; the ranked differences and the bench check for each are in [alignment-audit §4](../research/bcm2722/alignment-audit.md#4-differences-from-apples-conventions-ranked).

### Worked example: why a 301-pixel slot needs a 608-byte pitch

CR1 uploaded a 301 × 7 RGB565 slot at a pitch of 602 bytes (the width) [silicon, 2026-10-25]. Row k starts at 602·k:

```
row:      0     1     2     3     4     5     6
offset:   0   602  1204  1806  2408  3010  3612
mod 4:    0     2     0     2     0     2     0
```

Rows 1, 3 and 5 start at 2 mod 4. The port drops bits 1:0 of the write address, so each of those rows landed 2 bytes (one pixel) low: it overwrote the previous row's last pixel and lost its own last one. That is 3 rows × 302 pixels = **906 bad pixels**, by the CPU's copy and by engine 2 alike, exactly the count logged ([quirks #34](quirks.md), [bench CR1](../research/bcm2722/bench-p19.md#cr1-commit-readback)).

The fix [ours, after Apple's convention]:

```c
#define J_SLOT_PITCH(cls, w) (((uint32_t)(w) + 15) & ~15u)   /* pixels: 301 -> 304 */
/* pitch in bytes = 608: every row starts 32-byte aligned, so it is
 * 4-aligned for the port and in phase for the vector unit */
```

and the PP's writer merges an odd first or last pixel into its word by read-modify-write, while the port layer refuses a mid-word write outright ([merged-image-batch §2a](../research/bcm2722/merged-image-batch.md#2a-the-batchs-items)). The simulator's port models now drop bits 1:0 too, so this bug fails in a test before it reaches a device (§10).

---

## 4. Instruction forms and their silicon semantics

### Three levels of knowing a form

The encodings were recovered from Apple's libraries, whose symbols and relocations pin every instruction's length; disassembly and reassembly are byte-identical over about 85,600 instructions [apple-RE] ([isa-scalar, "How this was recovered"](isa-scalar.md#how-this-was-recovered-and-how-far-to-trust-it)). That proves **encodings**. Meanings come from running Apple's functions in vc2sim against independent models [apple-RE; sim], which proves only that the model and Apple's code agree, and from device checks [silicon]. So, for any form you want to emit:

1. **Does it occur in Apple's code?** If not, it may not even exist: the 16-bit "compare with an immediate" decoded fine and turned out to be a pre-decrement load on silicon ([quirks #1](quirks.md#instruction-set)) [silicon S1]. vc2cc's `--conservative` and `bcmfw/tests/forms_test.py` enforce this for every device image [ours].
2. **Does Apple's code exercise it in the vc2sim self-test?** Then its meaning is at least consistent with Apple's use [apple-RE; sim].
3. **Has silicon checked its result?** The proven lists are [isa-scalar, "Verified on silicon"](isa-scalar.md#verified-on-silicon) and [isa-vector, "Silicon check of the modelled forms"](isa-vector.md). Anything else is a [hypothesis] until a bench check reads it (§11).

A form that passes 1 and 2 but not 3 can still be wrong, and **the second level can hide it**. The halfword loads are the case study.

### Case study: the `0x28`/`0x29` halfword loads

**The forms.** The 32-bit memory instructions `0010 1000 ssss dddd` (`0x28`) and `0010 1001 ssss dddd` (`0x29`) with offset bit 0 set are halfword loads at `rs + (off & ~1)`. Apple's code names one `ldh` and the other `ldsh` [apple-RE]. Which is which was read from Apple's naming.

**How it hid.**
- S17 found that `0x28`/o1 **zero-extends**: a C inverse transform's output matched only with the three `0x28` offset loads zero-extended [silicon S17]. That became quirk #16, read as "the offset `ldsh` zero-extends", and the rule "for a signed halfword at an offset, compute the address and use `ldsh (rs)`".
- The mirror form, `0x29`/o1, stayed in the model as the zero-extending `ldh`, and vc2cc emitted it for every `uint16_t` read at an offset.
- Nothing contradicted it through every later session: the H.264 decoder used `0x29`/o1 and was **bit-exact on silicon** from S27, because those loads read values below `0x8000` or used only their low 16 bits ([merged-image-batch §3](../research/bcm2722/merged-image-batch.md#3-the-audit-every-function-whose-code-changed-and-what-it-did-on-silicon)) [silicon; sim].
- Apple's self-test passes with the two loads either way round (68 rows, 22,283 cases) [apple-RE; sim]: the model's second level could not tell the readings apart.

**How it surfaced.** On 2026-10-25 every job `COMMIT` failed with `J_E_SUM` on both images [silicon, build A]. The obvious suspects were the port (torn or short uploads) or SRAM that the host cannot write after the start. A staged upload through SDRAM was even built to route around the second ([jobs §4, "The staged upload"](../research/bcm2722/jobs.md#the-staged-upload-rev-05-sim-only-since-2026-10-25-open-question-67)) [ours]. Bench row **CR1** then wrote every pool and region by every write form, read **every pixel** back through the port, and only then sent `COMMIT`; on a sum failure it sent a second, unchecked `COMMIT` to get the BCM's own sums [ours]:

- every readback was right (control and 7 pools, 0 wrong) [silicon];
- yet 5 of 8 slots answered `J_E_SUM` [silicon].

So the BCM's checksum was wrong, not the upload. `jref_f16w` reads pixel 0 of every 8 with the 16-bit `ldh (rs)` and pixels 1–7 with `0x29`/o1.

**The exact fit.** F16w over N pixels h_i is A = Σ h_i, B = Σ (N − i)·h_i, both mod 2^32 ([jobs §4](../research/bcm2722/jobs.md#4-slots)). For an 8 × 1 slot of `0xFFFF`:

```
PP (zero-extended):   A = 8 × 65535                  = 0x0007FFF8
                      B = (8+7+...+1) × 65535 = 36 × 65535 = 0x0023FFDC
BCM, if 0x29/o1 sign-extends (pixel 0 = 65535, pixels 1..7 = -1):
                      A = 65535 - 7                  = 0x0000FFF8
                      B = 8 × 65535 - (7+6+...+1)    = 0x0007FFDC
logged: cr2 disp 8x1 direct: pp 0007fff8,0023ffdc bcm 0000fff8,0007ffdc
```

All 12 logged BCM sums (CR1's 8, CR2's 4) fit this rule exactly, both words. A 300 × 300 slot by engine 2 had 38,845 of its 77,700 offset loads read `0x8000` or more, and ours − BCM's A was `0x97BD`·2^16, which is 38,845 × 65,536 [silicon, session log] ([quirks #33](quirks.md#instruction-set)). The one slot that passed had no pixel of `0x8000` or more.

**The fix and the guard** [ours]: vc2dis, vc2sim and vc2cc now name `0x28`/o1 `ldh` (zero-extending) and `0x29`/o1 `ldsh` (sign-extending); vc2cc emits `ldh` only in forms proven to zero-extend and keeps signed halfwords as address-then-`ldsh (rs)`; every image was regenerated and every function whose code changed was audited for what it had done on silicon (`jref_f16w`, the visualizer's bars and the 3D test's texel loads had been wrong) ([merged-image-batch](../research/bcm2722/merged-image-batch.md)). [`bcmfw/tests/halfload_test.py`](../../bcmfw/tests/halfload_test.py) fails on the old model and on the old compiler. Bench row **HWLD** reads every halfword form directly with `0x8001`/`0xFFFF` ([bench HWLD](../research/bcm2722/bench-p19.md#hwld-the-halfword-load-forms-stages-4-and-4b-the-timing-bench)).

**The lesson:** when two readings both satisfy every test you have, the model must not quietly pick one. §10 describes how the simulator now refuses instead.

### The scalar rules that matter

Each with its evidence on [isa-scalar](isa-scalar.md) and [quirks](quirks.md#instruction-set):

| Rule | Evidence |
|---|---|
| Unsigned halfword at an offset: `0x28`/o1. Signed: compute the address, then the 16-bit `ldsh rd, (rs)` | [silicon] S17, 2026-10-25 |
| No gp/sp-relative or 48-bit halfword loads until HWLD reads them | [hypothesis] for both |
| `ldsb` sign-extends in every form checked | [silicon] S4 |
| Only `cmp`, `cmn`, `btst` write flags; `cmp`'s C is a borrow; no conditional ALU execution (branch around) | [apple-RE]; `bcs`/`bcc` checked [silicon] S35, S36c |
| The 16-bit range `0x7000–0x77FF` is `ld/st (--rs)`, not a compare | [silicon] S1 |
| Exceptions are imprecise: 2–4 later instructions run; a pre-decrement writes its base back even when it faults | [silicon] S2, S3 |
| A hot 32-bit loop-closing branch in the last word of a 16-byte line runs at half speed: `.align` the loop | [silicon] S6 |
| Peripheral registers: 32-bit accesses only | [apple-RE] |

### What vc2cc emits

C goes through the community VideoCore IV GCC (6.2.1) and our translator `tools/bcmre/vc2cc.py` ([compiler](../research/bcm2722/compiler.md)) [ours]:

- generated code uses only r0–r14; r15 and r22 are the translator's scratch; r16–r21 are left for hand-written kernels' global state;
- `--conservative` replaces forms Apple's code never exercises with sequences it does (`abs`, `not`, `addscale5–7`, post-increment `ldh`, 48-bit pc-relative `ld`/`st` → `mov r15, #addr` plus an indirect access);
- `--no-float` refuses any soft-float call; `config.h` poisons `float` and `double`;
- it refuses GCC's `min.l`/`max.l` with a 32-bit immediate, which lose their source register (a wrong-code bug in this GCC) ([compiler, "Known GCC bugs"](../research/bcm2722/compiler.md#known-gcc-bugs));
- its listing marks any instruction outside the proven set `; unproven form`.

### Worked example: reading 16-bit data safely in C

```c
uint16_t px = row[i];            /* unsigned at an offset: vc2cc emits 0x28/o1 (zero-extends) */
int16_t  c  = coef[i];           /* signed: vc2cc computes &coef[i], then ldsh rd, (rs) */
uint32_t w  = *(uint32_t *)p;    /* only if p is 4-aligned: a misaligned word load reads */
                                 /* the rounded-down word, then faults */
```

Check the listing (`build/bcmfw/*.lst`) for `; unproven form` after any change that touches halfword or signed data.

---

## 5. The vector unit

### The model in five facts

1. **The VRF** is 64 rows × 64 bytes, read horizontally (H) or vertically (V), 8- or 16-bit. An H16 vector at (y, x) keeps element i's low byte at column x + i and its high byte at x + 16 + i, so a 32-byte memory row loaded as H16 is **deinterleaved**. The H and V views alias the same storage, so a transpose is free ([isa-vector, "VRF layout"](isa-vector.md#vrf-layout)) [apple-RE; silicon S11].
2. **REP** repeats an instruction 1–64 times (or r0 times), stepping operands a row or column and memory addresses by a stride register. A REP-16 op costs about 16.25 cycles [silicon S7].
3. **Registers in vector operands**: a base or stride field of 7 means "no register" (address 0), and a scalar B operand naming r7 reads 0. Use r0–r6 [silicon S13–S15, S22; quirks #8, #19].
4. **`+rN` offsets** add y·64 + x field by field. On silicon: a horizontal B's x aligns down to 16 bytes; a 16-bit horizontal D's too; an **H16 A operand's x is not taken as given** (two rules fit, `dbl16` and `near32`); an H8 A's x is; y is applied as given ([quirks #17b, #28, #30](quirks.md#vector-unit)) [silicon 2026-10-25; the H8 case silicon, indirect].
5. **The accumulator** is 32 bits per lane, two 16-bit halves: `acc` adds into the high half, `x15` into the low half with carries between, `clra` clears both. So `vmul … x15` then `vmulhd … acc` is an exact 16 × 16 → 32 multiply-accumulate ([isa-vector, "The 32-bit accumulator"](isa-vector.md#the-32-bit-accumulator)) [apple-RE; silicon 2026-10-25 for the halves]. **It saturates past ±2^31 instead of wrapping** ([quirks #31, #32](quirks.md#vector-unit)) [silicon]; which saturation rule is [hypothesis] (bench ACSA).

The other silicon surprises (rounding ties away from zero, `vmulhr` reading B signed, `vmulhrs` not clamping an 8-bit D, the `vst16` field layout, the signed imm6) are each one row in [quirks, "Vector unit"](quirks.md#vector-unit); [isa-vector, "Rules for writing vector code on silicon"](isa-vector.md#rules-for-writing-vector-code-on-silicon) is the checklist.

### The start-up self-check pattern (`J_VPU`)

**An image that relies on a vector form silicon has not proven runs that form on itself at start, against its C, and uses it only if they agree.** The result is published in the job block's `J_VPU` word (`0x44`), and the PP logs it once per BCM start ([jobs.h](../../bcmfw/jobs.h)) [ours]:

| Bit | Name | Check | Falls back to |
|---|---|---|---|
| `0x01` | `J_VPU_CHECKED` | the checks ran | — |
| `0x02` | `J_VPU_SHIFT` | the lane shift (`jv_group16`) at phases 5 and 13 | C for out-of-phase sources |
| `0x04` | `J_VPU_MAC` | jdec's IDCTs (4, 8, 16 points) on four known blocks each | jdec's C IDCTs |
| `0x08` | `J_VPU_FFT` | the visualizer's FFT (overlay; sim only) | the C FFT |
| `0x10` | `J_VPU_BARS` | the visualizer's vector bars (overlay; sim only) | the C bars |
| `0x20` | `J_VPU_YUV` | jdec's colour stage, 64 pixels at two dither rows | the C colour stage |

Every fallback is exact, so a failed check costs speed, never pixels. A second word, **`J_VPU2` (`0x54`)**, holds the loop filter's seven vector forms (a bit per form, with `J_VPU2_FAIL`, `J_VPU2_GOT` and `J_VPU2_EXP` at `0x58`–`0x60` naming the first failing lane): the merged image's start-up checks for `vldp`/`vldq` 16-bit single loads, 16-bit and 8-bit `vsub` into a discarded D with `setf`, `vmov` under `ifn` and `ifz`, `vmax` 8-bit and a single `vst8` [ours]. **All seven agree with the C on silicon** (sitting 4, 2026-10-26, session log `jvpu: forms BSL1 ok BSF1 ok BSM1 ok BSR1 ok BSX1 ok BSS1 ok BSZ1 ok (vpu2 ff)`; [silicon], recorded in `tools/bcmre/proven_forms.txt`, [quirks #51](quirks.md)), so a loop filter in vectors can use them. The shape, from [`bcmfw/video/jobs_bcm.c`](../../bcmfw/video/jobs_bcm.c) [ours]:

```c
static uint32_t shift_check(void)
{
    uint32_t p, i, v = J_VPU_CHECKED | J_VPU_SHIFT;

    for (p = 5; p < 16; p += 8)                 /* two phases, both rules' cases */
    {
        for (i = 0; i < 32; i++)
            kbuf[i] = PAT(i);                   /* a known pattern */
        jv_group16(kbuf, kbuf + p, 0, 0, 1, 0); /* the kernel under test */
        for (i = 0; i < 16; i++)
            if (kbuf[i] != (uint16_t)PAT(i + p))
                v = J_VPU_CHECKED;              /* disagreed: keep the C path */
    }
    return v;
}
```

**It earned its keep on silicon.** On 2026-10-25 the lane shift disagreed and the merged image published `J_VPU` 0x1; the shell logged `bcmjob vpu=0x1 expect=0x3 why=degraded` and drew correctly on the C path [silicon, session log `ipodui.log`]. After the shift was rebuilt (below) the merged image read 3 in the second sitting [silicon, recorded in the merged image's `verified.txt` line on branch `p19-reverify-fix`]. Expected values today: 3 for the merged image, `0x27` for the display-only image [sim].

Two rules for writing a check: make its inputs reach the edge the form might get wrong (the IDCT check includes a column of 32767s that drives an unsplit sum past 2^31), and make the test suite run it under every model variant that fits silicon (§10), so a check that would pass only under one reading is caught on the Mac.

### Worked example 1: the gather by transposes

The first lane shift read 16 pixels from phase p with an H16 A operand at `+r` x = p. Silicon does not apply that x as given (quirk #30), so it was replaced by a form that never offsets x ([isa-vector, "A lane shift by transposes"](isa-vector.md#vrf-layout)) [sim; bench GTHR on silicon]:

1. Load 16 rows of 32 pixels as two H16 blocks (pixels 0–15 in rows 32–47, 16–31 in rows 48–63).
2. `vmov` H16 rows from V16 columns, REP 16, twice: the 32 pixel **columns** are now rows 16–47 at x 32.
3. `vadd` from an H16 A at `+r` = 64·p, a **y** offset (applied as given on silicon), stepping: the shift happens along y.
4. `vmov` back from the V16 columns.

Four REP-16 ops, exact under every rule for an A operand's x. `jobs_image_test.py` runs the start-up check under each fitted rule and shows the old gather failing under all but the refuted one [sim].

### Worked example 2: the two-chain IDCT

jdec's 16-point IDCT sums eight products per output with `vmul … x15 rep8` then `vmulhd … acc rep8`. With int16 inputs one chain can reach 81,678 × 32,768 ≈ 2.68·10^9, past 2^31, and the accumulator saturates there on silicon (REPA lane 0 read 32767 where a wrapping sum gives 514) [silicon, quirk #32]. `jv_dot8` therefore splits each sum ([isa-vector, "Our use: jdec's IDCTs"](isa-vector.md#the-32-bit-accumulator)) [ours; sim]:

```
chain 1: seed (caller's rounding term), 4 products, REP 4  -> high TH1, low TL1   (|sum| < 42,170 × 32,768 < 2^31)
chain 2: vmov D, TL1 clra x15; 4 products, REP 4           -> high TH2, low TL
result:  vadd TH2 + TH1 in wrapping 16-bit lanes            -> the sum mod 2^32, under every saturation rule
```

About 1% more instructions per call. The start-up check's block 1 drives the one-chain form past 2^31, so an image that still used it would fail `J_VPU_MAC` on a saturating chip.

---

## 6. DMA

Eight channels at `0x1000_0800 + 0x80·n`, each with three sub-channels that start chains of 32-byte control blocks ([dma](dma.md)) [apple-RE; silicon S4–S7]. The rules that matter:

| Rule | Evidence |
|---|---|
| Control blocks 32 bytes, 32-byte aligned: next, source, destination, length, transfer information, 2-D skips, 0, 0 | [apple-RE]; [silicon] S5 |
| Length counts units of the **narrower** of the two widths (each a power of two, 1–32 bytes) | [apple-RE]; [silicon] S5 |
| 2-D: length = `rows << 16 \| units per row`; the skip word is the **gap** after a row (pitch − width), not the pitch | [apple-RE]; [silicon] S5 (quirk #21) |
| Clear the whole status word (`+0x04 = 0xFFFF_FFFF`) before arming | [ours, proven S5–S16] |
| Completion: an LCD run ends on status bit 8; sub-channel k's chain on bit 13 + k; bits 0–7 are **not** completion bits | [silicon] S4, S5 (quirk #20); [apple-RE] |
| Bound every wait and keep a CPU path for when it runs out | [ours] |

**Channel assignments** we use, after Apple's [apple-RE]: 1 for LCD pixels, 5 for audio, 7 sub-channel 1 for memory copies. Our rule for units until the ALGN rows settle sub-word transfers: 4 or 32 bytes, each address a multiple of its unit ([memory-access §11.3](memory-access.md)) [ours].

### Recipe: a frame to the panel on channel 1

From [data paths R5](../data-paths.md#r5-bcm-lcd-dma-and-its-wait-discipline) [apple-RE values; silicon S5–S7]:

```
cb  = { 0, frame (32-byte aligned SRAM), 0x1000400C (LCD FIFO),
        38400 (4-byte units = 76,800 pixels),
        0x1900A001 | 5 << 4 (32-byte source bursts), 0, 0, 0 }
ch1 +0x00 = 0x09032101        ; enable, DREQ 3 (the LCD)
ch1 +0x04 = 0xFFFFFFFF        ; clear status
ch1 +0x38 = &cb               ; sub-channel 0: start
arm the LCD interface (lcd.md §6), wait for status bit 8 (100 ms bound, 64 us polls),
clear status, 0x10004000 = 1 (back to command mode)
```

A full frame takes 14.26 ms, set by the panel bus, not by memory [silicon S7]. The DMA reads the source live: do not rewrite it until the wait completes, and complete the wait before any other LCD command [silicon S7; ours].

### Recipe: SRAM ↔ SDRAM on channel 7 (PW8)

Apple's memory copy, reproduced as probe PW8 ([brief-map-power §9, PW8](brief-map-power.md)) [apple-RE words; silicon PW8, 2026-10-25]:

```
cb  = { 0, src, dst, bytes / 32, 0x01025151, 0, 0, 0 }   ; 32-byte units both sides
ch7 +0x00 = 0x0000E802
ch7 +0x04 = 0xFFFFFFFF
ch7 +0x58 = &cb               ; sub-channel 1's start register (+0x38 + 0x20 × 1)
wait for status bit 14 (13 + 1), 100 ms bound
```

PW8 copied SRAM → SRAM, SRAM → SDRAM and SDRAM → SRAM, 16 KB at a time, with **0 wrong words** in every copy, each source and destination pre-filled with different seeded patterns so that "nothing moved" could not pass [silicon]. Pieces of 16 KB are the proven size. Note that this channel's config word works with bit 0 clear; vc2mmio's model had to learn that bit 1 enables here ([alignment-audit §6](../research/bcm2722/alignment-audit.md#6-open-after-this-audit)) [silicon; sim].

---

## 7. Clocks and supply levels

### Apple's sequence and its rules

To go from the crystal to PLL A, Apple's start-up does this, and reproduced write for write it took our core to 150 MHz [apple-RE; silicon S7] ([boot-and-clocks §4](boot-and-clocks.md#4-plls-core-clock-select-and-regulator)):

1. run control `0x10000400 = 0xA5A5_0020`, twice;
2. the 1 MHz counter's clock pair (§2 step 4);
3. block clocks off, core word `+0x28 = pw 0x0001` (crystal), `+0x58 = pw 0x0101`;
4. PLLs B, C, D, A off;
5. **the supply level first**: `0x10001400 = pw ((old & 0xF0) | 15)`, wait 10 ms;
6. PLL A: divider `+0x00 = pw 0x6C`, control `+0x14 = pw 0x258`, wait 1 ms, `+0x14 = pw 0x658` (bit 10 enables);
7. `+0x28 = pw 0x0101`: the core onto PLL A.

The rules behind it [apple-RE] ([brief-map-power §4, §5](brief-map-power.md#4-core-voltage-the-regulator-and-apples-table)):
- **PLL rule:** M is always 108 (a 250 kHz reference) and N = f_VCO / 250 kHz; the VCO stays at 75 MHz or more, by running at a multiple of the wanted clock and dividing in the clock word.
- **Level before rise, after fall:** raise the supply level before a clock goes up, lower it after the clock comes down, 10 ms each; the level for a clock is the first row of Apple's 13-row table at or above it (46.9 MHz → 5 … 160.9 MHz → 15).
- **What follows the clock:** SDRAM refresh is counted in core cycles (f / 128,000, 7.8 µs): raise it after a switch up, lower it before a switch down [apple-RE value; ours the order]; delay loops calibrated in instructions change by 5.56× between 27 and 150 MHz, delays on the 1 MHz counter do not [silicon S7].
- After a host start the chip sits at level 11 at 27 MHz, where Apple's table and Apple's own running chip use 5 [silicon S1, S7; quirk #26]. Level 5 for our images is probe PW3: built, not run.

### Our bands and governor [ours]

Our images use twelve bands: the crystal, and each level band of Apple's table named by its top whole MHz (46, 59, 74, 87, 102, 116, 123, 130, 140, 147, 150) at Apple's level for it. The chip switches (`HP_OP_VCLK`) in Apple's order, checks the new speed against the crystal (±12.5%), and on any failure returns to the crystal at the level it found. **The PP decides**, in two layers: a band selector (up when the band's top saturates, down when the band below would carry the load at 80%, 100 ms dwell, clients' floors) and an in-band policy (the band's top by default). The merged image has no room for a governor of its own ([power-contract §8](../research/bcm2722/power-contract.md#8-the-video-chips-clock-level-bands-and-a-governor-p19-pw4-2026-10-25), [boot-and-clocks, "Our level bands"](boot-and-clocks.md#our-level-bands-against-apples-governor-p19-pw4)). A band is used only after bench row `vbv` verified it on the unit; **none has run on silicon yet**, and vc2sim checks the write order after every single write (`bcmfw/tests/vclk_test.py`) [sim].

### Worked example: the 87 MHz and 46 MHz bands

```
87 MHz: f_VCO = 87 MHz (>= 75): M = 108 (0x6C), N = 87,000,000 / 250,000 = 348 (0x15C)
        level 8 (the first table row at or above it: 87,268,380 Hz)
46 MHz: f_VCO = 92 MHz (2 x 46, to stay >= 75): N = 368 (0x170), core word 0x0102 (divide by 2)
        level 5
```

From the crystal after a host start (27 MHz, level 11) to the 87 MHz band, in our order [ours, on Apple's rules]:

```
1. the core is on the crystal (+0x28 = pw 0x0001); SDRAM refresh, if up, stays at 27 MHz's value
2. PLL A: +0x00 = pw 0x6C; +0x14 = pw 0x15C; wait 1 ms; +0x14 = pw 0x55C (bit 10: enable)
3. +0x28 = pw 0x0101 (core onto PLL A, divide by 1)
4. check the speed: count instructions against the 1 MHz counter, within 12.5 % of 87/27 x the
   crystal's count; if not, back to the crystal, PLL A off, level unchanged
5. SDRAM refresh to 87e6 / 128,000 = 679 (0x2A7) core cycles
6. only now lower the level: 0x10001400 = pw ((old & 0xF0) | 8); wait 10 ms
```

The clock rises but the level falls (11 to 8), so the level changes **last**: a level may drop only once the clock that needs no more than it is running and checked. Going the other way (87 MHz down to the crystal, or up to 150 MHz at level 15), the higher level goes first. When in doubt, keep the higher level.

---

## 8. The job ABI

The display jobs let the PP send the BCM small drawing commands instead of pixels. **The whole interface is [ours], a design**; [jobs.md](../research/bcm2722/jobs.md) is the contract and [`bcmfw/jobs.h`](../../bcmfw/jobs.h) holds every constant, shared by the image, the PP driver, the reference executor and the tests.

**Discovery** ([jobs §2](../research/bcm2722/jobs.md#2-discovery-and-the-handshake)): after `HELLO`, if `HP_CAP_JOBS_2D` (`0x0100`) is set, `HP_JOBS` (`0x138`) holds the job block's address (`0xDE000` in our images). There: `J_VERSION` (`'JB'` magic, a different major means no jobs at all), `J_OPS` (bit n = opcode `0x20 + n` implemented; **a job whose bit is clear is never sent**, the PP runs its CPU path), `J_EPOCH`, `J_MODE`, `J_VPU` (`0x44`, §5), `J_OVL` (`0x48`) and `J_CAPS` (`0x4C`).

**Transport** ([jobs §3](../research/bcm2722/jobs.md#3-transport-order-and-replies)): the same rings and records as §2, opcodes `0x20–0x3F`, run in ring order. Replies are opt-in (`J_TAG_REPLY` in the tag), except that a failure always replies and `FENCE` always replies, so a frame's jobs end with one `FENCE` and one wait. A full ring returns `BCMJOB_BUSY` at once; the caller draws that frame on the CPU.

| Op | Job | Payload (`jobs.h`) |
|---|---|---|
| `0x20` | `FENCE` | seq → seq; posts `J_FENCE` |
| `0x21` | `ALLOC` | class, `w \| h << 16`, fmt, key lo, key hi → slot, address, pitch bytes, flags (`J_ALLOC_HIT`: already held, no upload) |
| `0x22` | `COMMIT` | slot, A, B → A, B (the BCM's F16w); `J_E_SUM` on a mismatch |
| `0x24` | `BLIT` | src, `sx \| sy << 16`, `w \| h << 16`, dst, `dx \| dy << 16`, flags |
| `0x25` | `FILL` | dst, `x \| y << 16`, `w \| h << 16`, rgb565 |
| `0x26` / `0x27` | `FADE` / `AVG` | dst rect, a, b (and a weight 1–256): the shell's own arithmetic exactly |
| `0x29` | `PRESENT` | surf, `x \| y << 16`, `w \| h << 16`, flags → µs waited: the rectangle to the panel |
| `0x35` | `OVERLAY` | words, A, B: check and start the overlay in `J_OVL` (proposed, sim only) |

The full table, including `SCROLL`, `ANIM`, `SHADE` and the reserved numbers, is [jobs §5](../research/bcm2722/jobs.md#5-the-jobs-rev-04). **Surfaces** are a slot number, `J_SURF_FRONT` (`0xF0`, the frame at `0xE0000`) or `J_SURF_BACK` (`0xF1`). Nothing reaches the glass until a `PRESENT`.

**F16w** ([jobs §4](../research/bcm2722/jobs.md#4-slots)): over the slot's w × h pixels row by row (a wider pitch's padding is not summed), A = Σ h_i and B = Σ (N − i)·h_i, mod 2^32. It catches every 1- and 2-pixel error below 2^17 pixels, swapped pixels and shifted rows, and an all-black slot is checked like any other. Test vectors are in `jobs.h` (`J_F16W_VECTORS`).

**Epochs:** `J_EPOCH` goes up whenever slot contents are lost: every start, a video `OPEN`, a game session, `RESET`. The PP compares it before relying on any slot and re-uploads by key. A slot is a cache, never the only copy.

**Overlays** ([jobs §5, "Overlays"](../research/bcm2722/jobs.md#overlays-proposed-sim-only-126-2026-10-24)) [ours; sim]: a second program linked at the image's overlay region against the image's own map (vc2cc's fixed-base links). Its header starts with `'OVL1'` and the **ABI word**, the CRC-32 of the ABI version and the addresses of what it imports, so an overlay runs only in the exact image build it was linked against; anything else is refused with `HP_E_VERSION`. The PP writes it only while no overlay op is queued, then sends `OVERLAY` with its F16w.

**The merged image has one overlay region** (`0x12B000–0x133FFF`, 36 KB) shared by DECODE's overlay (`dec-ovl.bin`) and the visualizer's (`viz-ovl-m.bin`) [ours]. A load replaces what was there and the evicted one is due again at once; a track change's DECODE wins and the visualizer holds its frame and reloads; the files are read on the art thread and put piece by piece by the UI pass, the job owner; `TONE` is refused while any overlay is loaded (its buffer is inside the region); and the CPU path is a loud last resort, not the plan ([host-driver §6a](host-driver.md), [quirks #49](quirks.md)).

**Silicon status:** `ALLOC` replied from build A on [silicon]; `COMMIT` failed until the halfword fix (§4); in the second sitting the merged image passed its job self-test, `J_VPU` 3 and the drift on the chip (B3) [silicon, recorded in its `verified.txt` line on `p19-reverify-fix`]. Overlays (both of them), `SPECTRUM` and `BARS` have run only in vc2sim: in sitting 4b a stage play had left the chip on Apple's program, so neither was loaded ([quirks #48](quirks.md)).

### Worked example: put a cover on the screen

A 300 × 300 RGB565 cover into the drift's class, then shown at (10, 20) [ours]:

```
ALLOC   tag 0x8001: J_CLS_DRIFT, 300 | 300 << 16, J_FMT_RGB565, key lo, key hi
  reply: status, slot S, address A, pitch 608 (304 pixels), flags
  (J_ALLOC_HIT in flags: skip the upload)
upload: for y in 0..299: port_write(A + 608 * y, &cover[300 * y], 600 bytes)
        (A + 608 y is always 32-byte aligned; 600 bytes is a whole number of words)
        compute F16w over the 90,000 pixels as you go: A16, B16
COMMIT  S, A16, B16                         (no reply unless it fails: J_E_SUM)
BLIT    S, 0 | 0 << 16, 300 | 300 << 16, J_SURF_FRONT, 10 | 20 << 16, 0
PRESENT J_SURF_FRONT, 10 | 20 << 16, 300 | 300 << 16, 0
FENCE   seq                                 (always replies; or poll J_FENCE)
```

If any job's `J_OPS` bit is clear, or `COMMIT` fails, or the fence times out, the PP draws the same pixels with `bcmjob-cpu.c` over the reference executor, so the screen is identical either way.

---

## 9. Writing a kernel

### The pattern: C reference first

Every piece of BCM pixel code in this project is written three times, in this order [ours]:

1. **A C reference** that defines the result bit for bit. For jobs it is the reference executor in [`bcmfw/jobs/`](../../bcmfw/jobs/) (`jref.c`, `jmix.h`), built for the Mac and the PP alike, which is also the PP's CPU fallback; for the display kernels, `bcmfw/video/dispref.py`; for the jobs' mixing, the shell's own arithmetic.
2. **The same C compiled for the BCM** with vc2cc and run in vc2sim, checked against the Mac build's checksums. A match confirms both the compiler and the simulator for that code; this is how the `min.l` compiler bug was found ([compiler, "Known GCC bugs"](../research/bcm2722/compiler.md#known-gcc-bugs), `bcmfw/r3d/host`).
3. **The vector kernel**, generated by a Python script into a committed `.S` (`gen_jobs.py`, `gen_jdec.py`, `gen_disp.py`, `gen_viz.py`, each with `--check`), through the vector assembler front end `bcmfw/h264/vpu_asm.py`. It must equal the C reference bit for bit in vc2sim **under every model variant that fits silicon** before it ships ([jobs §9](../research/bcm2722/jobs.md#9-testing-without-a-device)).

### Conventions

- **vc2cc** (§4): r0–r14 in C; r16–r21 free for hand-written globals; `--conservative`; integer only.
- **Vector code**: bases and strides r0–r6 only; plain loads and stores at row alignment; any form not in Apple's code or not yet proven on silicon goes behind a start-up check (§5) with an exact C fallback.
- **Strict alignment** in every test (§10): a form whose silicon behaviour is unknown stops the run.
- **No float**, no large statics beyond the image's map, no wait without a bound.

### build.py and the image layout

[`bcmfw/build.py`](../../bcmfw/build.py) compiles C, assembles native `.S` (`.syntax vc2`, through the C preprocessor so assembly shares `config.h`), links `lib/` on demand like an archive, and lays out one flat image: `.vectors` at 0, zero padding to `BCMFW_ENTRY` (`0x200`), then `.text`, `.rodata`, `.data`, `.bss` ([bcmfw README, "Build"](../../bcmfw/README.md), [compiler, "Firmware support"](../research/bcm2722/compiler.md#firmware-support-milestone-3-done-2026-10-10)) [ours]:

```sh
bcmfw/build.py                                                          # hello image
bcmfw/build.py -D BCMFW_HOSTPROTO=1 -D BCMFW_JOBS=1 --overlay --out build/bcmfw/do
                                                                        # display-only + overlay
bcmfw/mkvideo.py [--check]      # the plain and merged video images and their headers
bcmfw/mkdevtest.py [--check]    # the embedded device-test images and verified.txt
```

It reads `config.h` through the preprocessor (a regex once took one image's `#define` for every image, and a check silently never fired: [memory contract §2](../research/bcm2722/memory-contract.md#2-sram-125-mb-0x0000000x13ffff-the-merged-display-image)), and it refuses a layout where regions overlap: the image and `.bss` against the arena or decode work area, the park stub, the overlay region (with at least 1 KB spare) and the pools against their budgets [ours].

**Size budgets today** ([merged-image-batch §2](../research/bcm2722/merged-image-batch.md)) [ours, measured by `build.py`]: the merged image has **68 B** below its arena at `0x35400`; the display-only image 1,344 B below its decode work area; the overlay region 20 KB. Room is the first constraint on any new resident kernel: the tile kernel's padded copy did not fit the merged image (28 B over) and became a C path, and the visualizer became an overlay.

### Recipe: adding a vector kernel to a job

1. Write or extend the C in `bcmfw/jobs/jref.c`; add golden cases to `jtest.c` (host).
2. Write the generator in `bcmfw/video/gen_jobs.py`; regenerate `jobs_kernels.S`; `--check` in the gate.
3. Wire it in `jobs_bcm.c` behind its `J_VPU` bit if it uses any form outside the proven tables; write the start-up check with inputs at the form's edges.
4. `bcmfw/tests/jobs_image_test.py`: bit-exact against jref, under each `SILICON_FITS` variant; the check passing under each.
5. `bcmfw/tests/forms_test.py`: every instruction's kind allowed; a new kind gets a bench row (§11).
6. `bcmfw/build.py`'s report: still inside the room; then the image's FNV changes, so it needs a re-verify before it is trusted (§11).

---

## 10. Testing in vc2sim

### The tools

- **`vc2sim`** ([sim](../research/bcm2722/sim.md)): an instruction-level simulator of the scalar core (`tools/bcmre/vc2sim.py`), the vector unit (`vc2vpu.py`) and the peripherals our images touch (`vc2mmio.py`: the LCD and its panels, DMA, timer, clocks, the host port). It runs Apple's own code as a self-test against independent models (`vc2sim.py --selftest <corpus>`) [apple-RE; sim].
- **`vc2dis`**: the disassembler and assembler, from the same encoding table, so a correction propagates everywhere.
- **`vc2cc`**: §4.

### The suites

`scripts/bcmfw_benches.sh` runs the core set in parallel and is part of the phase gate (`scripts/validate.sh`) [ours]: `vc2cc_tests`, `hello`, `dma`, `hostproto`, `realframes`, `devtest`, `bench`, `forms`, `xfade`, `halfload`, plus Apple's self-test and the disassembler round trip when the corpus is present. Beside them, each in `bcmfw/tests/`: `jobs_image_test` (the jobs against jref, and the cost audit), `video_test` (H.264 end to end against ffmpeg), `decode_image_test`, `jidct_test`, `viz_test`, `vpu_fit_test` (the silicon fits), `vclk_test`, `hibwake_test`, `peek_test` and others ([bcmfw README, "Test"](../../bcmfw/README.md)).

### The strict modes: the simulator refuses unknown silicon

Since 2026-10-25 vc2sim's policy is **refuse whatever silicon might do differently** ([memory-access §11](memory-access.md#11-alignment)) [ours]:

- `VC2SIM_STRICT_ALIGN=trap` is the default (unset means trap; `bcmfw_benches.sh` exports it): an access of ours in a form whose silicon behaviour is unknown stops the run with `AlignTrap` instead of succeeding under the model's guess. `log` counts such accesses (the audit's mode, `tools/bcmre/align_audit.py`); `0` turns the check off for comparisons with the old model.
- Apple's code is counted, never trapped; the ALGN bench checks are exempt (they are misaligned on purpose).
- A form moves into the known set (`vc2vpu._strict_vmem`, `vc2sim.STRICT_SCALAR_KNOWN`) **only when a silicon reading settles it**.
- The tests' host-port models drop a write address's bits 1:0, as the port does, so a host writer that starts mid-word fails in simulation as on the chip.
- `halfload_test.py` holds the halfword loads' readings: it fails on the old model and on the old compiler.
- vc2sim's scalar computation is silicon-exact for a large C program at 27 and 150 MHz under load (stage 3d: ten 3D check frames CRC-equal; [silicon], [quirks #52](quirks.md)); its *timing* needs a per-workload factor (the 3D renderer ran 1.14–1.64 times the estimate).
- The model of the chip a *host* test uses must not be more permissive than the image: `vbcm_model.c` refuses a `FEED` of bytes the host has not read back or let settle, and the simulator's job model offers DECODE and the visualizer's ops only once their overlays are loaded ([sim-fidelity §9a](../sim-fidelity.md)).

### Model variants

Every semantic choice the evidence has not settled is a named **hypothesis** with alternatives: `vc2sim.HYP` and `HYP_ALTERNATIVES` for the scalar core (for example `off16_h`, the offset halfword loads), `vc2vpu.HYP_DEFAULTS`, `HYP_ALTERNATIVES` and `SILICON_FITS` for the vector unit [ours]. A test switches one by setting the key in `HYP` and restoring it after, as `vpu_fit_test.py` does.

**Recipe: adding a variant when silicon disagrees** (the X15S case, [quirks #31](quirks.md#vector-unit)):

1. Dump the chip's words for the check (bench records: inputs, outputs, the first bad word), never only a pass/fail bit.
2. Add the key with the old model as one alternative and each candidate rule as another (`v_acc_sat`: `none`, `sat32`, `read16`, `hi16`).
3. Run the check's instructions in vc2sim under each alternative against **every** logged word (`vpu_fit_test.py`); keep the ones that fit all of them (`SILICON_FITS`).
4. Re-run Apple's self-test under each fitting alternative: Apple's code must still pass.
5. Pick a default as [hypothesis] with its reason, and make every kernel exact and every start-up check pass under **each** fitting alternative.
6. Design a bench row whose first record differs under each fitting alternative (ACSA, ASHX), and teach `readlog.py` to name the one silicon follows. When it comes back, the others go.

---

## 11. Verifying on the device

**The rules first** (CLAUDE.md, rule 5): nothing is written to the iPod without the owner's explicit go and a fresh backup of the system folder; Apple's firmware stays as the way back (hold MENU at boot). A hung unit recovers with MENU + SELECT for 6–8 seconds.

### The bench session

One Developer row, **Bench session**, runs a whole sitting on the iPod and records everything, so the owner writes nothing down ([bench-p19, "Session plan"](../research/bcm2722/bench-p19.md), [`apps/ipodui/devbench.c`](../../apps/ipodui/devbench.c)) [ours]:

| Mode | When | Steps |
|---|---|---|
| Full session | a new unit, or new P19 rows to run | part A automatic (after the watched stages 0–5), part B guided: one instruction a step, a question with wheel answers where a human must judge |
| Re-verify the video image | a merged image whose FNV changed | `pre` (the stage 4 and 5 flags), `s05` if needed then a restart, `jobs`, `jvpu`, `s4b`, `s4c`, `b3`, `drill2`, `s6`, `b13`, `b5`; about 13 minutes |
| Port write check | after any change to the port's write paths | `pre`, then `cr1`; about 3 minutes |

**Order the steps by what they leave the chip running.** A step that swaps the chip's program (the plain probe image, the 4b and 4c stages, B13, B5, the labs and every stage play) boots the image from its file, and its stop hands the chip to Apple's program, not to our resident image: the jobs stay off until the next start, and the visualizer and the panel decode fall back to the CPU. So the steps that need the chip's jobs run first, and those that swap the program run after them ([quirks #48](quirks.md); sitting 4b's visualizer ran on the CPU for this reason).

Each step writes one line to the results file (`bench/<date>-<build>.txt` in the hidden folder, closed a line at a time, so a restart keeps it and it doubles as the resume record) and an `auto:` line to `bcmtest.log`. Steps are entries in `steps[]` in `devbench.c` with an id, the instructions, the function, the question, flags and a watchdog ([bench-p19, "Adding steps"](../research/bcm2722/bench-p19.md#adding-steps-for-other-sessions-code)).

### Bench rows

A bench row is a check whose every outcome **names a reading**. The vector and scalar checks live in the bench image (`bcmfw/bench/`, `gen_bench.py`) and write records `B4V k TAG flags got0..3 bad want`; `bcmfw/bench/readlog.py` turns each into "as modelled", "NOT as modelled" or "neither reading" with the words. Designing one:

- **"Nothing happened" must not pass.** Pre-fill targets with a unique pattern and count wrong words ([data paths, "What to try first" item 5](../data-paths.md#0-how-to-develop-on-this-platform-one-screen)): S8's "clean" DMA result was a CPU write read back ([data paths D1](../data-paths.md#d1-the-pps-dma-can-feed-the-bcms-host-port)).
- **Choose inputs that separate the readings**: HWLD loads `0x8001` and `0xFFFF` so zero- and sign-extension differ in every word.
- **Log the raw words**, so a surprise can be fitted offline. Dumping and fitting found three rules in one session each where guessing had cost three sessions ([data paths D4](../data-paths.md#d4-bcm-memory-access-quirks-that-bit-us)).
- Informational rows never fail a stage; they inform the model.

### After a sitting: ingest

```sh
python3 -I scripts/bench_ingest.py <copy of the pulled folder> --commit <build commit> --patch <file>
```

Always with `-I`: pulled files are untrusted data. It prints failures and anomalies first, then each results file replayed, the readlog rows at each clock, `J_VPU` per image and the pass flags; it labels every run by its log file, bench image and build, so a stale log from an earlier build cannot be read as this one's ([bench-p19, "After the session: ingest"](../research/bcm2722/bench-p19.md#after-the-session-ingest)). If the session earned a verified line it writes a patch (the `verified.txt` line, the regenerated `bcm-devtest-images.c`, the reference-doc tag changes for every check that came back as modelled). **The patch is never applied for you**: review it, then `git apply` it.

### The `verified.txt` discipline

[`bcmfw/verified.txt`](../../bcmfw/verified.txt) lists the images that passed on a real unit: name, FNV-1a, and the session and log line that verified them. `mkdevtest.py` compiles it in. A release build boots the resident merged image, and turns the jobs on, **only if its FNV is listed**; otherwise it takes `hello_dma`, then Apple's firmware ([memory contract §6](../research/bcm2722/memory-contract.md#6-l2-our-image-as-the-default-and-verifiedtxt)) [ours]. Consequences:

- **Any change to an image changes its FNV**, so the default path refuses it until it is listed again; an image that never ran on a device cannot become the default.
- A line is earned only when every must-pass step passed on the unit; a must-pass step that **skipped** means "can't verify this time", never a pass.
- **Freeze the image while a verify is pending, and batch changes** behind one re-verify: the 2026-10-25 batch put the halfword fix, the pitch rounding, the park hook, the block clocks and the new bench rows into one regenerated set with one audit and one expected-results table ([merged-image-batch](../research/bcm2722/merged-image-batch.md), [bench-p19, "Sitting 3 run sheet"](../research/bcm2722/bench-p19.md#sitting-3-run-sheet)).

### Worked example: a verified line

The line the re-verify earns, and its shape [ours]:

```
bcmvideo_jobs <fnv>  stage 6 PASS (merged, <bytes> bytes), B5 PASS, jobs self-test PASS,
                     J_VPU 3 PASS[, drift B3 and drill 2 PASS] on the owner's 5.5G:
                     re-verify session <date>, build <id>
```

Take the FNV from `s6`'s log line, not from a doc page; then rerun `bcmfw/mkdevtest.py` so the list is compiled in.

### The first-use video check

A released image's `verified.txt` token opens 150 MHz for every unit; the **first-use check** then tests each unit once per image before a film trusts it ([host-driver §10](host-driver.md); design in [`first-use-check.md`](../research/bcm2722/first-use-check.md)) [ours]. It decodes a built-in 30-picture stream at the top clock band and compares each picture's checksum with the known value. Its verdicts: **PASS** (a unit token is written; a lower band than the top caps later plays), **FAIL** (every band failed: the styled error screen, asking for a Save Diagnostics report) and **INCONCLUSIVE** (the image's words not where they should be, a packet taken with no picture, the stream file missing: no token, the play continues, the next play checks again, so a bug in the check cannot lock a unit out). At most one chip reboot per band, a 4 s budget, and no check in bench plays. Sitting 4b ran it for the first time: it failed at every band because it sent a `FEED` the moment a packet was written, the late-write case above; it now writes ahead and reads back. Its first PASS on silicon is still to come ([open question #76](open-questions.md)).

---

## 12. Lessons learned

### The 27 MHz clock gate (sitting 1's skipped pictures)

**What happened** [silicon, 2026-10-25, sitting 1]: stage 6 played its clips with 69–203 pictures skipped, a FEED mean of 42.7 ms against a 33.3 ms frame. It looked like a decoder regression. It was a gate: the video player runs the chip at 150 MHz only with both the 4b and 4c pass flags, and the unit had 4b but no 4c, so it decoded at 27 MHz ([bench-p19, "Session plan"](../research/bcm2722/bench-p19.md), [battery-levers §4](../research/bcm2722/battery-levers.md)).
**Fix** [ours]: the re-verify runs `s4b` then `s4c` before `s6`, and `s6`'s line names the chip's MHz.
**Lesson:** any gate that silently degrades must say so in the log of every run it affects; read the clock before reading the timing.

### Pass flags are read at boot

**What happened** [silicon, 2026-10-25, sitting 2's first try]: `pre` failed because the flags named the previous images; `s05` rewrote them; but the boot path reads the flags only at a start, so the merged image was not the display driver, and `jobs` and `jvpu` skipped (commit `33267a7842` on `p19-reverify-fix`).
**Fix** [ours]: the re-verify restarts onto the display driver after `s05` rewrites the flags, and a must-pass step that skips stops the run with its reason.
**Lesson:** state read at boot needs a restart after it changes; a skip is not a pass.

### B13: the warm park that never parks

**What happened** [silicon]: in build A's session the warm swap failed at round 0, step 2, "never parked", against a 10 ms limit. The image waits out the LCD DMA's 14 ms frame before its stub writes the park word, so the limit went to 60 ms (`a60cc0a071`). In the second sitting it still failed: `PARK st=0 f=3`, the warm word 0 after 60 ms (recorded in the merged image's `verified.txt` comment on `p19-reverify-fix`; tracked on branch `p19-b13`).
**What it changed:** the verified line no longer claims B13, because the warm swap is the display-only image's path into the video image, not the merged image's; the display-only image does not become the resident one until B13 passes ([warm-swap §7](../research/bcm2722/warm-swap.md#7-bench-row-b13-ours)).
**Lesson:** a rule that gates the wrong thing blocks real progress; tie each verified claim to the path it proves. **Still open** (§13).

### Don't trust a permissive model

Five times a model that accepted more than silicon does hid a real bug [silicon; sim]:

| What the model allowed | What silicon does | Found by |
|---|---|---|
| `0x29`/o1 zero-extending (a name read from Apple's code) | sign-extends | CR1's sums (§4) |
| a host write at any halfword address | drops bits 1:0 | CR1's 301 × 7 slot (§3); it passed in simulation |
| a plain vector load at any address; a `vstr` across a line | aligns down; drops lanes | S18–S24 kernel dumps (quirks #17, #18) |
| a wrapping accumulator; an H16 A operand's x as given | saturates; does not apply x as given | the 2026-10-25 vector checks (quirks #30–#32) |
| a host write seen by the core the moment it is made (the job and FIFO models) | a command written afterwards can arrive first | the first-use check, sitting 4b (quirk #47); the models now refuse a `FEED` of unread, unsettled bytes |

**Lesson:** a simulator that is wrong in the permissive direction turns a misalignment or a sign into wrong data that passes every test. So vc2sim now refuses what silicon has not shown (§10), every unsettled choice is a named variant, every kernel must be exact under every variant that fits, and the forms that matter get a start-up check on the chip itself (§5).

### A stage play does not give the chip back to us

**What happened** [silicon, 2026-10-26, sitting 4b]: the owner found the visualizer "a little laggy". The log said `viz CPU FALLBACK why=nojobs` after a clip played in a bench stage. A stage play boots its image from a file, so its stop returns the chip to Apple's program, and the job driver had nothing to talk to.
**Fix** [ours]: the bench runs the chip-job steps before the first program swap.
**Lesson:** a hand-back returns the chip to the image the stop path knows; decide which image that is before you design the order of a session ([quirks #48](quirks.md)).

### A wait without a bound is a freeze without a record

**What happened** [silicon, sitting 4]: three silent freezes, logs ending mid-activity. The cause, confirmed by disassembly: a wrapped function with no real body linked (`fdprintf`), so a settings save branched to address 0 and spun there with IRQs masked. The same hunt found waits on the chip that held the port lock with no limit, a second way to freeze silently.
**Fix** [ours]: a bound and a FAULT line for every wait ([host-driver §2a](host-driver.md)), a hang record and a jump-to-0 exception on the PP ([PP quirks #56, #57](../pp5022-reference/quirks.md)), and build-time link checks on the shell's wrapped names (`scripts/febe_link_check.py`, run by the gate): every wrapped name's real body is pinned into the link, a wrapped name with no real definition is refused, and so is one that links as a data object.
**Lesson:** bound the wait, say it was bounded, and keep a record past a freeze ([open question #75](open-questions.md), answered).

### Older lessons, still true

- **Reproduce Apple's sequence write for write before improving it**; let a functional probe, not a status bit, decide (SDRAM: [data paths D8](../data-paths.md#d8-sdram-bring-up-treated-like-a-textbook)).
- **A torn word is "not yet", not "dead"** ([data paths D7](../data-paths.md#d7-torn-reads-mistaken-for-a-dead-port)).
- **Priority before frequency** when a feed starves ([data paths D9](../data-paths.md#d9-boosting-the-pp-fixes-a-starved-feed)).
- **Label every log by its build.** Sitting 2's folder still held sitting 1's rotated `bcmtest.3.log`, whose checks were first read as this sitting's failures (commit `93df839c83` on `p19-sitting2-docs`).

---

## 13. Where the evidence is thin

What a developer cannot yet rely on, with the place that tracks it. These feed the closure plan.

1. **jdec's IDCT check fails on silicon in the display-only image**: the second sitting read `J_VPU` `0x23` (MAC clear) where vc2sim gives `0x27` (recorded in `verified.txt`'s comment on `p19-reverify-fix`). The C IDCT runs instead, exact, so pictures are right; why the two-chain kernel disagrees is unknown.
2. **The warm park (B13)** fails on silicon with `PARK st=0 f=3`; cause unknown (`p19-b13`). Until it passes, the display-only image cannot become the resident one.
3. **Which accumulator saturation rule, and which H16 A `+r` x rule** (bench ACSA, ASHX): both fitted rules are kept in every test until the readings are folded in (quirks #30, #31).
4. **Unproven scalar forms**: the 48-bit and gp/sp-relative halfword loads (HWLD), misaligned stores of every width, odd halfword accesses (#5), divide by zero, and the ALU kinds listed at the end of [isa-scalar](isa-scalar.md#verified-on-silicon).
5. **Unproven vector forms**: `vst8`/`vst8v` layouts (#2), misaligned plain loads beyond quirk #17 and lone `vldp` gathers (#21, #68), 80-bit stores, unnamed ops. (The loop filter's seven start-up forms are proven: sitting 4, [quirks #51](quirks.md).)
6. **The port's finer rules**: a lone trailing halfword (PLH1), Apple's halfword forms at 2 mod 4, read auto-increment, the ALT bank (#25, #43, #70); PW7 failed.
7. **DMA**: sub-word units and misaligned addresses (#69), sub-channel concurrency, other DREQ numbers (#13); the PP's general DMA into the port delivers nothing (#41).
8. **Interrupts**: none confirmed on silicon, the doorbell included (#3, #14); every image polls.
9. **Caches**: never run with the cache on; geometry and coherence unknown (#6); PW2 failed.
10. **SDRAM**: latency and bandwidth from the core, the vector unit and DMA unmeasured; self-refresh unknown (#7); a store then an immediate load in SDRAM unchecked (#46).
11. **Clocks and power**: the bands and governor (`vbv`, `vgov`), level 5 at 27 MHz (PW3), Apple's ladder (PW4) and hibernate (PW5) are built and not run; PW1 failed; current draw in every state is unmeasured (#12).
12. **Overlays (DECODE and the visualizer, sharing one region), `SPECTRUM`, `BARS`, the staged upload, `ANIM`**: vc2sim only ([open question #74](open-questions.md)). The 3D renderer is not on this list any more: stage 3d matched the simulator on silicon ([quirks #52](quirks.md)).
13. **Restarting the core** without a power cut through the start register, and the bootstrap bytes' meaning (#10, #24).
14. **Panels 0–2**: only panel 3 has run on silicon (#17).
15. **Late writes** (does the core see SDRAM written just before a command?) and the first-use check's own figures on silicon ([#73](open-questions.md), [#76](open-questions.md)).

Everything numbered is in [open questions](open-questions.md); the bench rows that would settle most of them are already built ([bench-p19](../research/bcm2722/bench-p19.md)).
