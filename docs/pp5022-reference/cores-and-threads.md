# The two cores, the cache, and sharing work

The PP5022 has two identical ARM7TDMI cores, called **CPU** (the main core) and **COP** (the co-processor). Both execute ARM and Thumb, have no MMU and no MPU, and share every bus, peripheral and memory. Each core has its own 8 KB cache **status** array but they share the cache controller. [rockbox] `pp5020.h`, `system-pp502x.c`; [external] iPodLinux

## 1. Which core am I?

`PROCESSOR_ID` at `0x6000_0000` reads `0x5555_5555` on the CPU and `0xAAAA_AAAA` on the COP. [rockbox] `system-target.h`

- Read only the low byte: `0x55` = CPU, `0xAA` = COP (`PROC_ID_CPU`, `PROC_ID_COP`).
- Bit 7 of that byte is a ready-made core index (0 = CPU, 1 = COP); Rockbox's `current_core()` is `ldrb; lsr #7`.
- The two IDs are bitwise complements; Rockbox's Peterson lock uses `id ^ 0xFF` to name the other core (§5).

Both cores leave reset and run the same code from the same address. Every start-up path must branch on `PROCESSOR_ID` first (`boot.md` §4).

## 2. Processor control: sleep, wake, stall

| Address | Name |
|---|---|
| `0x6000_7000` | `CPU_CTL` |
| `0x6000_7004` | `COP_CTL` |

Either core can write either register. [rockbox] `pp5020.h`

| Bits | Name | Meaning |
|---|---|---|
| 31 | `PROC_SLEEP` | sleep until an interrupt (or a wake by the other core). Reading it shows whether that core is asleep |
| 30 | `PROC_WAIT_CNT` | sleep until the countdown ends |
| 29 | `PROC_WAKE_INT` | raise an interrupt on wake-up; auto-clears |
| 27 | `PROC_CNT_CLKS` | countdown unit: clock cycles |
| 25 | `PROC_CNT_USEC` | unit: µs |
| 24 | `PROC_CNT_MSEC` | unit: ms |
| 23 | `PROC_CNT_SEC` | unit: s — **PP5022 and later only** |
| `[22:8]` | – | unknown; "semaphore flags?" in the Rockbox notes. `[11:8]` are often seen holding the core's own ID nybble (`0x5`/`0xA`) while it sleeps. No effect on execution observed |
| `[7:0]` | – | write: cycles to stall on the next instruction; read: cycles remaining |

Bits `[31:23]` and `[7:0]` are from the Rockbox header's notes (originally iPodLinux) [rockbox]; the stall example there: writing `0x6800_0080` and then a `nop` stalls the `nop` 128 cycles, and the register then reads `0x4800_0000`.

**Write 0 to wake** a sleeping core (`PROC_WAKE = 0`). [rockbox] `wake_core()`

**Sleep sequence** (PP502x) [rockbox] `sleep_core()`: `str 0x8000_0000, [PROC_CTL(core)]` followed by one `nop`. (The PP5002 has a famous variant where the store must not sit at an address ending in `0x0`; that bug is **not** carried to the PP502x code.)

**Clock-change stall** [rockbox] `scale_suspend_core()`: before rewriting `CLOCK_SOURCE` the running core writes `0x4800_0003` to its own `PROC_CTL` (wait-count, clock-cycle unit, 3) and `0x4000_0000` to the other core's (wait-count with no unit and a zero count, which holds it); afterwards it writes `0x4800_001F` to its own and `0` to the other's. IRQ and FIQ are disabled across it. Treat this sequence as required around every `CLOCK_SOURCE` write (`clocks-power.md`).

**Halt for good:** `PROC_CTL(core) = 0x4000_0000` then spin, with that core's interrupts disabled [rockbox] `system_exception_wait()`.

## 3. The cache

### Geometry [rockbox]

| Item | Value |
|---|---|
| Size | 8 KB (`CACHE_SIZE 0x2000`), one data array at `0xF000_0000` |
| Line | 16 bytes (`CACHEALIGN_SIZE`) — so 512 lines |
| Status | one 32-bit word per line per core: CPU at `0xF000_4000`, COP at `0xF000_6000`; each word "mirrored four times" per line (the status array is 8 KB per core: one word per 4 bytes of data) |
| Status word | `[20:0]` line address `>> 11` (`CACHE_ADDRESS_MASK`), bit 22 dirty, bit 23 valid, others unused |
| Policy | write-back (there is a dirty bit and an explicit flush operation) |
| Associativity | not known; the status layout suggests direct-mapped [hypothesis] |

Whether both cores really share one 8 KB data array or each has its own is **not settled** by the code: there is one data window and two status windows. The cache is unified (instructions and data): Rockbox invalidates instructions through the same operation. [rockbox]

**The test for it:** the Developer › Advanced › Video chip › Probes row **COP row C2** (`docs/research/bcm2722/bench-p19.md`, card C2). One core writes a line it holds, cached. The other core then reads the same line through its own cached view, with the uncached alias as the control. If the reader sees the writer's value, the two status arrays front one data array; if it sees its own stale copy, each core has its own data. [hypothesis] each core has its own data array (Rockbox's two-core code would corrupt memory otherwise); not yet run.

### Registers

| Address | Name | Bits |
|---|---|---|
| `0x6000_C000` | `CACHE_CTL` | 0 `ENABLE`, 1 `RUN`, 2 `INIT`, 4 `VECT_REMAP` (local vectors, `interrupts-timers.md` §1), 14 `READY`, 15 `BUSY` |
| `0x6000_6044` | `CACHE_PRIORITY` | Rockbox ORs `0x10` on the CPU and `0x20` on the COP; `system_init()` first writes `0x3F`. Meaning unknown ("what's this do?" in the source) |
| `0xF000_F040` | `CACHE_MASK` | which addresses are cacheable (below) |
| `0xF000_F044` | `CACHE_OPERATION` | bit 1 `FLUSH` (write back dirty lines), bit 2 `INVALIDATE`. Rockbox writes `0xFC0` once at init (meaning of bits `[11:6]` unknown) |
| `0xF000_F048` | `CACHE_FLUSH_MASK` | unused by Rockbox |

**Cacheability rule** [rockbox] `init_cache()` comment: an access is cached if `(addr & mask) >> 16 == (mask & match) >> 16`. With `CACHE_MASK = 0x0000_1C00` the cached ranges are:

| Range | Cached |
|---|---|
| `0x0000_0000`–`0x03FF_FFFF` | yes (logical SDRAM after the remap) |
| `0x0400_0000`–`0x1FFF_FFFF` | no — includes the physical SDRAM alias `0x1000_0000` |
| `0x2000_0000`–`0x23FF_FFFF` | yes (flash ROM) |
| `0x2400_0000`–`0x3FFF_FFFF` | no |

Everything above `0x4000_0000` (IRAM, registers) is uncached.

### Initialisation (per core) [rockbox] `init_cache()`

```
CACHE_CTL &= ~(ENABLE|RUN);  CACHE_CTL |= INIT;
CACHE_PRIORITY |= (CPU ? 0x10 : 0x20);      /* main firmware only */
CACHE_MASK = 0x00001C00;
CACHE_OPERATION = 0xFC0;
CACHE_CTL |= INIT|ENABLE|RUN;  nop ×4
read one byte every 16 bytes over 8 KB of cached memory (from 0x2000)
```

The final read loop makes every line valid; see the invalidation gotcha next.

### Maintenance [rockbox]

- **Write back (clean):** `CACHE_OPERATION |= FLUSH`, wait for `CACHE_CTL.BUSY` to clear, four `nop`s. Only if the cache is enabled.
- **Invalidate:** Rockbox does **not** use the `INVALIDATE` bit. It rewrites every status word of the current core to `VALID | 0x1F_FFFF` (valid, clean, tagged with the highest line address, which is never accessed), 16 bytes apart over 8 KB, **with IRQ and FIQ disabled**, from IRAM. Reason given in the source: lines left *invalid* can corrupt memory when there are many writes to, and instruction fetches from, cached memory.
- **Clean and invalidate** (`commit_discard_idcache`): flush, then the status rewrite, with interrupts off.
- There is no per-line or per-range operation in use; every maintenance call is whole-cache.

## 4. Coherency between the cores

There is **no hardware coherency** between the CPU's and the COP's view of cached memory [rockbox]. Rules that follow:

| Data shared between cores | Put it in |
|---|---|
| small, hot | IRAM (uncached, single-cycle) |
| larger | the uncached SDRAM alias: Rockbox's `.ncbss`/`.ncdata` sections are linked at `0x1000_0000 + logical`, so a variable's address is already uncached (`SHAREDBSS_ATTR`, `SHAREDDATA_ATTR`) |
| a buffer handed over by pointer | flush on the producer core, then invalidate (clean+invalidate) on the consumer core |

Moving a thread between cores (Rockbox `switch_thread_core`) flushes the departing core's cache on its idle stack and cleans+invalidates the arriving core's cache before resuming. [rockbox] `thread-pp.c`. A thread that exits on the COP flushes the COP's cache before it is freed.

DMA masters (I2S DMA, ATA DMA, USB) see physical memory only; see `dma.md` and `quirks.md`.

## 5. Synchronisation primitives

### Mailbox (`0x6000_1000`) [rockbox]

| Address | Name | Meaning |
|---|---|---|
| `0x6000_1000` | `MBX_MSG_STAT` | read the mailbox bits |
| `0x6000_1004` | `MBX_MSG_SET` | write 1s to set bits |
| `0x6000_1008` | `MBX_MSG_CLR` | write 1s to clear bits |
| `0x6000_100C` | `MBX_UNKNOWN1` | unknown |
| `0x6000_1010` | `CPU_QUEUE` | the COP can set bit 29; only a **CPU** read clears it |
| `0x6000_1020` | `COP_QUEUE` | the CPU can set bit 29; only a **COP** read clears it |

`MAILBOX_IRQ` (source 4) exists; Rockbox never enables it and uses the mailbox bits purely as shared flags. How the queue registers relate to the IRQ is not documented in Rockbox (`open-questions.md`).

**Apple uses the mailbox interrupt** [external] siggi `03` §57: RetailOS takes `MAILBOX_IRQ` and reads `CPU_QUEUE` in its handler; the read clears bit 29 and with it the interrupt (an emulator whose word read skipped the clear took the same interrupt 2,953,894 times). So the queue registers are Apple's cross-core doorbell, where Rockbox polls flags and wakes with `PROC_CTL`.

**Rockbox's sleep/wake handshake** (`thread-pp.c`, PP502x asm) uses mailbox bits per core `c` (0 CPU, 1 COP): `0x4<<c` "intends to sleep", `0x10<<c` "stay awake (wake requested)", `0x1<<c` "waker busy".

- `core_sleep(c)`: set `0x4<<c`; if `0x10<<c` is not set, write `PROC_SLEEP` to `PROC_CTL(c)` and then 0 after waking; clear `0x4<<c | 0x10<<c`; spin while `0x1<<c` is set; enable IRQ.
- `core_wake(o)` (IRQ off): set `0x11<<o`; wait until the other core is either not intending to sleep or actually asleep (`PROC_CTL(o)` bit 31); if asleep write 0 to `PROC_CTL(o)`; clear `0x1<<o`.

This closes the race where one core decides to sleep just as the other wakes it. At start-up the CPU clears bits `0x3F` before waking the COP (`core_thread_init`).

### Locks

ARM7TDMI's `SWP` is not used across cores. Rockbox's `corelock` is **Peterson's algorithm** on a 3-byte struct (`myl[2]`, `turn`) in uncached memory (IRAM or `.ncbss`). [rockbox] `firmware/asm/arm/corelock.c`. Byte stores to uncached memory are the only atomic operation relied on. Whether `SWP` is atomic between the two cores on this bus is unknown (`open-questions.md`).

## 6. Starting and parking the COP

- At reset both cores run the boot code; Rockbox's crt0 parks the COP with `PROC_SLEEP` until the CPU has remapped memory, wakes it to do its own remap, parks it again, and finally wakes it after the CPU's kernel is ready (`boot.md` §4).
- A COP image that never reaches its own code is normal: Rockbox's `cop_main` notes that with an old bootloader the COP never starts, so firmware must not assume the COP is available.
- The COP takes **only Timer 2** interrupts in Rockbox and is woken every tick by the CPU (`core_wake(COP)` in the Timer 1 ISR). An idle COP sleeps in `PROC_SLEEP`.
- Before reloading an image (rolo), the CPU has the COP disable its interrupts, clean+invalidate its cache, disable the cache, signal through an IRAM flag, and jump to physical `0x1000_0000`, where the new image's crt0 parks it again. [rockbox] `rolo.c`

## 7. What can run on the COP

Anything that touches no peripheral owned by the CPU and shares data correctly. In upstream Rockbox only these use it [rockbox]: the MP3 decoder's synthesis filter (`mpa.c`, `MPA_SYNTH_ON_COP`, with the subband buffers in IRAM; the share of decode time this moves is not measured here), MPEG video decoding in the old video plugin, the FFT plugin, two encoders. Interrupt-driven drivers (USB, ATA completion, audio FIQ, clickwheel) are CPU-only in Rockbox's design because the handlers live in the CPU's dispatch.

Practical constraints:

- Both cores compete for the same SDRAM and IRAM; a COP running a memory-heavy loop slows the CPU.
- `cpu_boost` changes the clock of **both** cores (one `CLOCK_SOURCE`); the change stalls both (§2).
- Cache maintenance is per core and whole-cache; frequent hand-offs of cached buffers cost a full 8 KB flush each.

### Prior art: the codec on the COP

- **Rockbox ran the codec thread on the COP for 22 days** [rockbox]: `82f9056988` (2007-03-04, "Dual core support for PP502x") created it on the COP; `66258a30a4` (2007-03-26) moved it back: "not yet possible because more protection on file system level is necessary". It has been `IF_COP(, CPU)` since. Today the thread would also share cached state with the CPU: pcmbuf's indices with the PCM DMA interrupt, buffering's handle structs with the buffering thread, and `pcm_play_lock()`, which masks the DMA interrupt and so excludes the handler only on the handler's own core.
- **The MP3 synthesis filter on the COP** (`4e36a2b991`, 2008, FS#9318): "~90 % speedup according to test_codec on the Sansa ... allows MP3 decoding at 30 MHz without boosting" [rockbox], quoting the author's measurement. The pattern: pure compute, buffers in IRAM on PP5022/5024, two IRAM semaphores, one `commit_discard_dcache()` when the thread quits.
- **Apple** [external] siggi `03` §57–58: RetailOS runs both cores (the COP works, sleeps and wakes about 15,000 times in a cold boot under emulation), and its audio layer's voice pool fills only with the COP alive. Which code runs on the COP, and whether it decodes, is not shown. [hypothesis] Apple's audio engine runs on the COP.
- **Ours** (Fit-2, `docs/fit2-brief.md`): the codec thread stays on the CPU, and the codec's run (the decode) goes to a COP worker, with each playback callback carried back by `cop_job_call()` (`firmware/export/cop_job.h`). [ours]. It ran on silicon in S36 (2026-10-14/15): over an hour of FLAC with no gaps, CPU idle ~88–92 % against ~45 % with the switch off, COP idle ~41–46 % (`docs/fit2-brief.md` §12) [silicon]. A video's soundtrack stays on the CPU until the S36c stall with the switch on is explained (commit `6e9fecc4a8`)

### Open for P19 19-9 (the UI and the BCM job ring on the COP)

Three bench rows are waiting for a session (`docs/research/bcm2722/bench-p19.md`, cards C1–C3). All three are [hypothesis] until they run:
- **C1, engine 2 from the COP.** The second DMA engine (`0x6000_8000`) is started and polled from a COP job into the BCM's free SRAM, then read back on the CPU. Expected: it works as it does from the CPU, since the engine is a bus master and either core can reach its registers.
  - Known from the code [ours]: `e2port_start()`'s `commit_dcache()` writes back only the running core's cache. The core that produced the source must write it back itself.
  - Known from the code [ours]: e2-port.c's owner flag is core-local (cached `.bss`, guarded by `disable_irq_save()`).
- **C2, the shared data array** (§3 above).
- **C3, sleep under `yield()`.** Expected: a COP thread that polls with `yield()` never lets the COP sleep. It is the only runnable thread, so `switch_thread()` never reaches `core_sleep()`. `sleep(1)` and a blocking wait both let it sleep between tick wakes.

### Does an idle COP cost anything?

Rockbox's idle COP sleeps in `PROC_SLEEP` and is woken by the CPU every tick (100 Hz) from the Timer 1 handler (`kernel-pp.c`); each wake runs the COP's scheduler and sleeps again, well under 0.1 % of a core [rockbox; estimate]. What a sleeping core draws is not documented anywhere we know of [hypothesis: clock-gated, near zero dynamic power]. Fit-1 M0's `cop_idle_pct` measures the COP's duty; before Fit-2 the counters it reads were in cached memory written by both cores, so earlier readings may be stale [ours].
