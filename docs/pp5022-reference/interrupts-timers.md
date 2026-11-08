# Interrupts and timers

## 1. Exceptions and vectors

The cores are ARM7TDMI (ARMv4T): the standard eight vectors at address 0 (`reset, undef, swi, prefetch abort, data abort, reserved, IRQ, FIQ`), no vectored interrupt controller in the ARM sense, no high-vector option. [rockbox] `crt0-pp.S` `.vectors`

Two ways to supply vectors:

| Method | How | Evidence |
|---|---|---|
| Remap SDRAM to 0 | MMAP0 maps SDRAM at logical 0 (`memory-map.md` §3); copy a vector page (`ldr pc,[pc,#24]` ×8 plus eight handler addresses) to logical 0 | [rockbox] `crt0-pp.S` — the main firmware's method |
| Local vector table | Set `CACHE_CTL_VECT_REMAP` (bit 4 of `CACHE_CTL`, `0x6000_C000`); the core then takes its vectors from `INT_VECTOR_ENTRY(v)` = `0x6000_F100 + 4·v` | [rockbox] `pp5020.h`; used by the USB-mode bootloader (`system-pp502x.c` `system_init`, `HAVE_BOOTLOADER_USB_MODE`). Whether the table holds handler addresses or instructions is not stated by the code — see `open-questions.md` |

**Gotcha** [rockbox]: some Apple/OEM images do not clear `CACHE_CTL_VECT_REMAP` themselves and hang if it is left set. Clear it before jumping to foreign code (`system_prepare_fw_start()`).

## 2. The interrupt controller (`0x6000_4000`)

Two banks of 32 sources: "low" (sources 0–31, registers at `0x6000_40xx`) and "high" (sources 32–63, at `0x6000_41xx`). The whole high bank appears as **one** low source, `HI_IRQ` = 30. Each core has its own enable mask and its own IRQ/FIQ selection; the raw status is common. [rockbox] `pp5020.h`

### Register map

| Low bank | High bank | Name | Access | Meaning |
|---|---|---|---|---|
| `0x6000_4000` | `0x6000_4100` | `CPU_(HI_)INT_STAT` | R | sources pending **and enabled for the CPU as IRQ** |
| `0x6000_4004` | `0x6000_4104` | `COP_(HI_)INT_STAT` | R | same for the COP |
| `0x6000_4008` | `0x6000_4108` | `CPU_(HI_)FIQ_STAT` | R | sources pending and routed to the CPU's FIQ |
| `0x6000_400C` | `0x6000_410C` | `COP_(HI_)FIQ_STAT` | R | same for the COP |
| `0x6000_4010` | `0x6000_4110` | `(HI_)INT_STAT` | R | raw status of every source |
| `0x6000_4014` | `0x6000_4114` | `(HI_)INT_FORCED_STAT` | R | software-forced sources |
| `0x6000_4018` | `0x6000_4118` | `(HI_)INT_FORCED_SET` | W1S | force a source pending |
| `0x6000_401C` | `0x6000_411C` | `(HI_)INT_FORCED_CLR` | W1C | un-force |
| `0x6000_4020` | `0x6000_4120` | `CPU_(HI_)INT_EN_STAT` | R | the CPU's current enable mask |
| `0x6000_4024` | `0x6000_4124` | `CPU_(HI_)INT_EN` | W1S | enable sources for the CPU |
| `0x6000_4028` | `0x6000_4128` | `CPU_(HI_)INT_DIS` | W1C | disable sources for the CPU |
| `0x6000_402C` | `0x6000_412C` | `CPU_(HI_)INT_PRIORITY` | RW | **a 1 bit routes the source to FIQ** instead of IRQ for the CPU |
| `0x6000_4030`–`0x6000_403C` | `0x6000_4130`–`0x6000_413C` | `COP_…` | | the same four registers for the COP |

The meanings of the `*_STAT` registers are inferred from how Rockbox's handlers use them; `INT_PRIORITY` = "FIQ select" is how Rockbox uses it (it sets the DMA bit, or the I2S bit, to get the audio FIQ) [rockbox] `pcm-pp.c`. Read-modify-write is safe on `*_PRIORITY` only; use the set/clear registers for enables.

**Masking at start-up.** crt0 writes `0xFFFF_FFFF` to `CPU_INT_DIS` (`0x6000_4028`) or `COP_INT_DIS` (`0x6000_4038`) before anything else; `disable_all_interrupts()` later clears both cores' low and high enables, both forced sets, and every GPIO port's `INT_EN`. [rockbox]

### Source numbers

| # | Name | Device | Evidence |
|---|---|---|---|
| 0 | `TIMER1_IRQ` | Timer 1 (the kernel tick) | [rockbox] |
| 1 | `TIMER2_IRQ` | Timer 2 (user timer) | [rockbox] |
| 4 | `MAILBOX_IRQ` | inter-core mailbox | [rockbox] (defined, unused by Rockbox) |
| 10 | `IIS_IRQ` | I2S FIFO | [rockbox] |
| 20 | `USB_IRQ` | USB OTG controller | [rockbox] |
| 23 | `IDE_IRQ` | ATA controller | [rockbox] (Rockbox polls instead, `storage.md`) |
| 25 | `FIREWIRE_IRQ` | FireWire | [rockbox] |
| 26 | `DMA_IRQ` | the general DMA engine (any channel) | [rockbox] |
| 27 | – | the second DMA engine (`0x6000_8000`) | [external] siggi `10`; [hypothesis] from Apple's interrupt numbers 27/28 for the two engines, which become 26/27 if Apple's numbers are the source plus one (`dma.md` §6.4) |
| 30 | `HI_IRQ` | the high bank (any of 32–63) | [rockbox] |
| 32 | `GPIO0_IRQ` | GPIO ports A–D | [rockbox] |
| 33 | `GPIO1_IRQ` | GPIO ports E–H | [rockbox] |
| 34 | `GPIO2_IRQ` | GPIO ports I–L | [rockbox] |
| 36 | `SER0_IRQ` | UART 0 (dock serial) | [rockbox] |
| 37 | `SER1_IRQ` | UART 1 | [rockbox] |
| 40 | `I2C_IRQ` | I2C block — and, on the iPod 4G-style pad (the Video included), **the clickwheel** | [rockbox] `irq_handler`: `CPU_HI_INT_STAT & I2C_MASK` → `ipod_4g_button_int()` |

Apple's firmware also uses source 13 as a software interrupt: its DMA handler sets it through `INT_FORCED_SET` to defer work [external] siggi `10`. Observed enable masks: Apple's firmware `CPU_INT_EN_STAT = 0xC000_0017`; Rockbox `0x4000_0003` plus whatever drivers add [external] siggi `06`.

Other numbers are unassigned in Rockbox. High-bank sources are tested by masks relative to 32 (`GPIO2_MASK = 1<<2` in the high registers).

### Dispatch, iPod Video [rockbox] `system-pp502x.c` `irq_handler`

On the CPU, in order: Timer 1 → Timer 2 → USB → high bank: clickwheel (`I2C`) → `GPIO2` with `GPIOL_INT_STAT & 0x10` (USB cable insert) → `SER0` (dock serial, when compiled). On the COP only Timer 2 is handled. The handler is a plain `if/else if` chain: one source per entry, the IRQ re-enters for the next.

**FIQ** is reserved for audio: the FIQ vector jumps through a patchable pointer (`fiq_function`) to the PCM DMA completion handler (or the recording handler). [rockbox] `pcm-pp.c`

### FIQ on either core (core layout A, step 0b)

- **One vector, two cores.** Both cores run the same vector table at address 0, so the FIQ vector and its one target word (`fiq_function`) are shared [code]. A handler that either core may take must find out which core it is on.
- **Apple's form** [apple-RE, `osos` 1.3]:
  - the FIQ vector (`0x1000_001C`) branches to a stub at `0x1000_015C`. The stub loads `0x6000_0000` into banked `r10`, reads `PROCESSOR_ID` into banked `r12`, keeps the low byte, and compares it with `0x55`;
  - the COP (any other value) jumps straight to IRAM `0x4000_0000` with no register saved;
  - the CPU saves `r0`–`r8` and `lr` on its FIQ stack and calls `0x1027_70C0`;
  - the IRQ vector (`0x1000_0018` → `0x1000_0180`) tests bit 0 of `PROCESSOR_ID` instead (`0x55` odd, `0xAA` even) and calls one handler per core.
- **Apple's exception stacks** [apple-RE]: the CPU's routine at `0x1008_46A0` gives FIQ, abort, IRQ, undefined and SVC a stack each, from a table in IRAM at `0x4000_6000` (8-byte entries, the top at `+0x2C`); the COP's routine at `0x1008_4734` sets abort, IRQ, undefined and SVC only. **Apple's COP has no FIQ stack.** Its FIQ code in IRAM must therefore never touch `sp`, or set it itself [hypothesis].
- **Ours** (`pcm-pp.c`, developer builds, `HAVE_CORE_LAYOUT_A`) [code; device: bench row pcop]:
  - `fiq_function` holds `fiq_by_core`. That routine reads `PROCESSOR_ID` into banked `r12` and jumps through `fiq_core_target[core]`, two words in IRAM;
  - a core with no target masks its own source 26 and returns (`fiq_spurious`, counted);
  - our handlers are C (`interrupt("FIQ")`, which pushes `r0`–`r8` and `lr`) and call the PCM callback chain, so the COP needs a stack: `crt0-pp.S` now gives it 1 KB of its own (`cop_fiq_stack`).
  - Before this change `cop_fiq_stack` was the top of the CPU's `fiq_stack` (the `#if 0` space). A FIQ taken by each core at the same moment would have run on one stack [code].
- **Masking the other core's source.** The PCM lock writes `pcm_core`'s `INT_DIS` (`0x6000_4028`, or `0x6000_4038` for the COP) from either core. That a CPU write to `COP_INT_DIS` and `COP_INT_EN` masks and unmasks the COP's source is [hypothesis]; bench row pcop checks it, and it logs which form took if only the COP's own write works. A FIQ that is already running cannot be stopped by the mask: a locker on the other core waits while the handler's flag in IRAM is up (bounded, counted).

## 3. Timers (`0x6000_5000`)

| Address | Name | Meaning | Evidence |
|---|---|---|---|
| `0x6000_5000` | `TIMER1_CFG` | bit 31 enable, bit 30 repeat (auto-reload), bits `[28:0]` period − 1 in **µs** | [rockbox] `kernel-pp.c`, `timer-pp.c` (`0xC000_0000 \| (n−1)`, max accepted period `0x2000_0000`) |
| `0x6000_5004` | `TIMER1_VAL` | **reading acknowledges** the timer's interrupt | [rockbox] |
| `0x6000_5008` | `TIMER2_CFG` | as `TIMER1_CFG` | [rockbox] |
| `0x6000_500C` | `TIMER2_VAL` | read to acknowledge | [rockbox] |
| `0x6000_5010` | `USEC_TIMER` | free-running 32-bit microsecond counter, wraps every ~71.6 min | [rockbox] `udelay()`, `TIMER_FREQ 1000000` |
| `0x6000_5014` | `RTC` | named "RTC" by iPodLinux; Rockbox does not use it (the wall clock is the PMU's, `clocks-power.md`). Believed a seconds-like counter [hypothesis]. **Does not count awake** [silicon, sitting 4gh]: 0 at both ends of 11 s | [rockbox]; [silicon] |

On hardware the timer block repeats every `0x20` bytes through its page (`0x6000_5020` reads as `TIMER1_CFG`) [external] siggi `08`. Apple's firmware runs Timer 1 at `0xC000_03FC` (≈1 kHz tick) [external].

The timer clock is independent of the CPU clock: Rockbox writes µs values and never rescales them when it changes `CLOCK_SOURCE` between 24, 30 and 80 MHz [rockbox]. What clocks it during the 32 kHz sleep setting is unknown.

**Kernel tick** [rockbox] `kernel-pp.c`: `TIMER1_CFG = 0` (stop), read `TIMER1_VAL` (clear), `TIMER1_CFG = 0xC000_0000 | (interval_ms·1000 − 1)`, `CPU_INT_EN = TIMER1_MASK`. Rockbox's HZ is 100, so 10 ms. Each tick, after the tick tasks, the CPU **pulses the COP awake** (`core_wake(COP)`); the COP has no tick of its own.

**Changing a running timer's period** [rockbox] `timer-pp.c`: writing `TIMERn_CFG` from outside the ISR restarts the count; Rockbox defers the new period to the next ISR to keep the phase.

## 4. Processor sleep as a timer (`PROC_CTL`)

`CPU_CTL` (`0x6000_7000`) and `COP_CTL` (`0x6000_7004`) can put a core to sleep until an interrupt **or** until a countdown ends (`PROC_WAIT_CNT`, with a unit of clocks, µs, ms, or — on PP5022 and later only — seconds). Full bit table in `cores-and-threads.md` §2. Rockbox uses only "sleep until woken" and the countdown-with-clocks form in `scale_suspend_core()` (`clocks-power.md`).

## 5. GPIO interrupts

Each GPIO port has `INT_STAT`, `INT_EN`, `INT_LEV` (level/polarity), `INT_CLR` (`gpio-i2c.md`). The port's group raises `GPIO0/1/2_IRQ` in the high bank. iPod Video uses one: `GPIOL` bit 4 (USB cable) [rockbox]. The level register is used as a polarity toggle: on each edge the handler flips `INT_LEV` to catch the opposite edge (the USB-insert handler does this) [rockbox] `usb-fw-pp502x.c` — see `usb.md`.

## 6. Routing a source to the COP (sitting 4h, the core layout probe)

**Why.** The owner chose Apple's layout (A) on 2026-11-05: the UI, file system, disk, USB and BCM driver on the CPU, the audio pipeline on the COP (`../design/core-layout-decision.md`). (A) stands on two routes Rockbox never used: the PCM DMA's interrupt (source 26) as a **FIQ on the COP**, and **Timer 2 as an IRQ on the COP** (the COP's own deadline source, independent of the CPU's tick). The probe run (Developer › Bench session › Core layout probe; `apps/ipodui/devbench_cop.c`) proves or refutes both. Until its rows pass on silicon, every COP route below is **[hypothesis]**; the simulator refuses the rows (they record "Not run (sim)", a skip) and never models a pass.

**What Apple does** [apple-RE] (`apple-dual-core.md` §1): the COP's start-up enables source bit 26 and nothing else, with a priority word of `0x3F`, and its FIQ vector goes to a handler in IRAM at `0x4000_0000`; the CPU never enables source 26. That the COP takes 26 as a FIQ is [hypothesis, strong]. Apple's general DMA engine (the one that feeds I2S) does not refuse the COP; the second engine (the BCM port) does (`dma.md` §6.6).

### 6.1 The route and its order (P-1)

| Step | Core | Write | Evidence |
|---|---|---|---|
| 0 | CPU | stop playback; wait until `pcm_is_playing()` is false (the mixer's tail); `dac_standby_allow(false)` (the DAC awake) | [code] |
| 1 | CPU | snapshot `CPU_INT_EN_STAT & DMA_MASK`, `COP_INT_EN_STAT & DMA_MASK`, `COP_INT_PRIORITY & DMA_MASK` and the FIQ vector's target | [rockbox-derived] register map above |
| 2 | CPU | `CPU_INT_DIS = DMA_MASK` (the CPU never serves this run) | [rockbox-derived] |
| 3 | CPU | `COP_INT_PRIORITY \|= DMA_MASK` (FIQ, not IRQ, on the COP) | [rockbox-derived] the CPU's equivalent in `pcm-pp.c`; on the COP [hypothesis] |
| 4 | CPU | the FIQ vector's target = the lab's handler (see 6.3) | [rockbox-derived] |
| 5 | COP (a job) | its F bit clear (see 6.3: the COP's start-up does it; the probe checks), its own FIQ stack (see 6.3), then `COP_INT_EN = DMA_MASK`, then DMA0 programmed and started as `pcm-pp.c` does (`DMA0_PER_ADDR = &IISFIFO_WR`, `DMA0_FLAGS = DMA_FLAGS_UNK26`, `DMA0_INCR = RANGE_FIXED \| WIDTH_32BIT`, `DMA0_CMD = IIS request \| RAM→peripheral \| single \| wait request \| interrupt \| (8192 − 4) \| start`) | [rockbox-derived] DMA0; [apple-RE] the COP enabling 26; the COP taking it [hypothesis] |
| 6 | COP (FIQ) | read `DMA0_STATUS` (acknowledge), count, re-arm the next 8 KB piece of a 64 KB looped tone | [rockbox-derived] `fiq_playback` |
| 7 | CPU (after 10 s) | `COP_INT_DIS = DMA_MASK` first, then stop DMA0 (clear `START \| INTR`, wait not busy), wait 50 µs (a FIQ already taken has read the vector), then the priority, the vector's target and the CPU's enable **as found**; wait for the I2S FIFO to drain | [rockbox-derived] `dma_tx_stop` |
| 8 | COP (a job) | its FIQ stack pointer put back | [code] |

Every exit path runs step 7 from the CPU: the normal end, a COP job that never finishes (bounded at 1 s), and a **tick-task watchdog** that fires 15 s after the route if the thread never got there. The CPU's set/clear writes to `COP_INT_DIS` act on the COP's mask whatever the COP is doing ([rockbox-derived] for the set/clear form; [hypothesis] that a CPU write masks the COP's source, proved by the same row). If the route fails the other way (the source arrives as an IRQ on the COP, whose `irq_handler` serves only Timer 2), the COP spins in its IRQ until step 7 masks the source; the CPU is unaffected. After the row the next playback start reprograms DMA0, the vector's target and `CPU_INT_EN` itself (`sink_dma_start`, `sink_unlock`).

**Expected readings** (`p1 cop_fiq=N cpu_fiq=M expect=E rate= tone= us=`, then `p1 cop_cpsr= fbit= stack= dog=`): `E` = elapsed µs × rate × 4 / 8192 / 10⁶, **215 for 10 s at 44.1 kHz**. **Pass:** `N` within ±5 % of `E`, `M` = 0, the owner hears one clean, steady ~1 kHz tone (1,001 Hz at 44.1 kHz: 372 whole cycles in the 16,384-frame loop, so the loop is seamless; −20 dBFS). `fbit` is the COP's thread-mode CPSR F bit (0 expected: FIQs enabled). A fail with `N = 0, M = 0` means the COP never took the FIQ (route or F bit); `M > 0` means the CPU still took it; `N` far off with a clean tone means the count, not the route.

### 6.2 Timer 2 on the COP (P-2)

`timer_register(1, NULL, TIMER_FREQ / 100, isr, COP)` [rockbox-derived] `timer-pp.c`: it unmasks `TIMER2_MASK` in `COP_INT_EN`, and the COP's branch of `irq_handler` calls `TIMER2()`, which acknowledges by reading `TIMER2_VAL` and calls the ISR. The ISR counts per core. 10 s, then `timer_unregister()` (stops the timer, clears both cores' masks). **Expected:** `p2 cop_t2=N cpu_t2=M expect=1000`; **pass:** `N` within ±2 % of 1000 (980–1020), `M` = 0. The backlight is held on during the row: its fade owns Timer 2 at a lower priority and would be displaced. **Result [silicon, sitting 4gh, 2026-11-06]: PASS**, `cop_t2=1000 cpu_t2=0` over 10.0 s: Timer 2 is an IRQ on the COP alone when only `COP_INT_EN` has its bit.

### 6.3 Code findings for (A)

- **The COP never unmasked its own FIQ** [silicon, sitting 4gh; code]. P-1 read `cop_fiq=0 cpu_fiq=0` with `cop_cpsr=0x5f` (System mode, IRQ on, **F set**) in the COP's job. The CPSR is per core and only that core can change it: `crt0-pp.S` starts both cores with F set (`0xdf`), `main.c` clears it on the CPU (`enable_fiq()`), and nothing ever cleared it on the COP; thread switches don't save or change the CPSR's mask bits. So the route (priority, enable, vector) may have been right and the COP simply could not take a FIQ; DMA0's first 8 KB piece played (the owner's brief blip, ~46 ms) and its completion FIQ was never taken, so nothing re-armed it. **Fix:** in layout (A) builds the COP clears its F bit in its own start-up (`system_init()`'s COP branch, `system-pp502x.c`), after zeroing `COP_INT_PRIORITY` and `COP_HI_INT_PRIORITY`, so a COP source arrives as a FIQ only once `pcm_pp_set_core()` or the probe routes it. P-1 now logs the bit as found, opens it for its run if start-up left it set (`fbit=1 opened`), and fails unless start-up had opened it. **Apple does not do this** (`apple-cop-audio.md`, branch `p19-cop-fiq-re`): its CPU owns I2S, DMA and the FIQ, and the COP's FIQ vector is a trap. The COP enabling source 26 that `apple-dual-core.md` reads is therefore not an audio FIQ path; FIQ on the COP is **our own extension**, and P-1 is an experiment that tests each of its hypotheses in turn.

**The probe as rebuilt after 4gh** (`devbench_cop.c`), one log line per step so a failure names its step:

| Step | Who | What | Logged | Settles |
|---|---|---|---|---|
| 0 | COP | the CPSR as the COP's start-up left it | `p1 step0 start-up cpsr= fbit=` | that the start-up fix took (layout (A): F clear) |
| 1 | CPU, then COP | `COP_INT_PRIORITY \|= DMA_MASK`, the vector's target the lab's; the COP enables 26 and reads both bits back | `p1 step1 route: pri_bit= en_bit=` | the routing writes land |
| 2 | COP, then CPU | the COP masks its own F, starts **one** 8 KB piece; 100 ms later the CPU reads source 26 in `INT_STAT` (raw), `COP_INT_STAT`, `COP_FIQ_STAT`, `CPU_INT_STAT`, `CPU_FIQ_STAT` (no DMA0 status read: it acknowledges) | `p1 step2 one piece, F masked: raw= cop_fiq= cop_irq= cpu_fiq= cpu_irq= fiqs=` | **the routing**: a working route shows raw and `cop_fiq`, nothing on the CPU, no FIQ taken while masked. `cop_irq` is logged, not judged (whether the controller also flags a FIQ-select source as an IRQ is itself unknown) |
| 3 | COP | clears its own F bit | `p1 step3 unmask on the COP: cpsr 0x5f -> 0x1f fiqs=` | the pending FIQ is taken at once (`fiqs` ≥ 1) and the loop starts |
| 4 | COP idle, 10 s | the handler acknowledges and re-arms each piece | `p1 step4 10 s, COP idle: cop_fiq= cpu_fiq= expect= (lo..hi)` | **a sleeping COP wakes for its FIQ**: ~215 (204–226 at 44.1 kHz), 0 on the CPU, a clean tone after the first piece's short gap |

Pass: steps 2, 3 and 4, and in layout (A) builds step 0. Our design against Apple's, and why: we route source 26 per run (`pcm_pp_set_core()`, or the probe) because the CPU stays the PCM's default core until (A) is proven and every switch must be reversible; the COP's F bit is cleared once at start-up (its priority words zeroed first, so nothing arrives as a FIQ until routed), which costs nothing while no COP source is FIQ-selected.

- **The COP has no FIQ stack of its own** [code, 2026-11-05]. `crt0-pp.S` sets the COP's FIQ-mode SP to `cop_fiq_stack`, but the 1 KB under that label is inside `#if 0` ("We'll need this soon - just reserve the symbol"): the label sits at the top of the CPU's `fiq_stack`. A COP FIQ that pushes anything writes the CPU's FIQ stack. The probe gives the COP a 256-byte stack of its own for the row (a COP job switches to FIQ mode, sets SP, switches back, and restores it after). (A) must reserve the real one (the 1 KB `.space` under `#if 0`).
- **The FIQ vector's target is one word shared by both cores** [rockbox-derived]: `fiq_handler` is `ldr pc, [pc, #-4]` followed by that word (`fiq_function`, a file-local label in `pcm-pp.c`). The probe finds it as `fiq_handler + 4` and refuses to run unless the instruction is `0xE51F_F004`. In (A) only the COP may route source 26 to its FIQ, so one shared target is enough while the CPU takes no FIQ; the `fiq_record` path (recording, unused here) would need its own core.

### 6.4 The dark duty row (what "essentially asleep" means today)

Developer builds count, for this row only (`HAVE_PP_IRQ_TALLY`, `system-target.h`; a release compiles none of it): the CPU's IRQ entries by the source `irq_handler` serves (tick, Timer 2, USB, IDE, wheel, GPIO, other), and each core's returns from `core_sleep()` (its wakes). The row plays music with the screen off and Hold on, arms C F C F (decode on the CPU; decode on the COP through `codec_cop_set_place(CPL_MODE_COP)`, each arm a fresh codec load), 30 s settle and three 60 s windows each, and logs only after the arms (a log write would wake storage). Per window: `duty C1 w1 s= cpu_idle=‰ cop_idle=‰ wakes=/s cop_wakes=/s tick=/s dec= scr= hold= mv=`, `irq tick= t2= usb= ide= wheel= gpio= other= disk_wakes=`, `thr all= <top three threads>=`, `clk at24=‰ ent= bfi= lock= mhz=`; then one line per arm. A reading, no verdict. **Expected** [hypothesis, from Fit-2's silicon numbers]: arm C, CPU idle ~450 ‰ with ~100 tick IRQs/s and the codec's wakes; arm F, CPU idle ~900 ‰ and the tick (100/s) the largest single wake source, the COP's idle falling by the decode's share. The tick is the floor in both arms and in (A) (§4.2 of the decision study).

**Sitting 4gh (2026-11-06), arm C only** [silicon] (the session read arm C; arm F logged "playback did not start" and was not read): decode on the CPU at 30 MHz (80 MHz in window 2), screen dark, Hold on: **CPU idle 136 ‰ over 180 s (144 ‰ in window 3), CPU wakes 60/s (63.5/s), COP wakes 100.6/s with COP idle 1000 ‰**; IRQs in window 3: tick 6,003 in 60 s, nothing else (no Timer 2, USB, IDE, wheel or GPIO), no disk wake. Thread runs per window: the bench's watchdog 251, the codec 19, main 6. Against the expectation (~450 ‰ idle) the CPU is far busier than modelled; the audio was a video's soundtrack (#154), not the music the row assumes, so arm C is a reading for that format only. **K-1 (`k1`)**: pulse arm COP 100.5 wakes/s, park arm **0.6/s** (3 pulses sent, 15,000 held in 150 s), 0 underruns both arms, 44 pcm rescues in each arm (equal: not the park's; a refill-pacing finding).
