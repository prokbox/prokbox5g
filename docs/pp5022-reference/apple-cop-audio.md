# Apple's audio interrupt and the COP, step by step

Desk study, 2026-11-06, a companion to [`apple-dual-core.md`](apple-dual-core.md) and [`apple-power.md`](apple-power.md), using the same method, tags and address convention: `osos 0x1xxx_xxxx` is the 1.3 image's file offset plus `0x1000_0000`; Apple's code pointers in vtables are the same offsets without the `0x1000_0000`. **[apple-RE]** marks what we read from Apple's code in this study. Nothing here is Apple's code; it describes behaviour in our own words, with addresses.

Apple's firmware is treated as a **functional demonstration**: it shows a sequence that worked on this silicon. It is not an authority. Each step below is therefore marked **necessary** (with what breaks without it), **incidental** (Apple's choice, or a by-product of its kernel), or **questionable**.

---

## 0. The headline: Apple takes the audio DMA interrupt on the CPU, not the COP

The question we were asked was how Apple runs source 26 (the general DMA engine, which feeds I2S) as a FIQ on the COP. **On closer reading, it does not.** All three firmwares (1.2.1, 1.2.3, 1.3) agree:

1. **The interrupt service is CPU-only.** Apple's interrupt-controller class (constructor `osos 0x101F_CA84`, 64 sources, vtable at `0x1067_1F98`) holds pointers to the CPU's registers only: `CPU_INT_STAT`, `CPU_FIQ_STAT`, the raw and forced registers, `CPU_INT_EN_STAT`/`EN`/`DIS`/`PRIORITY` (`0x6000_4000`–`0x6000_402C` and their `0x41xx` twins), plus `0x6000_4030`/`4130`. It has no pointer to `COP_INT_EN`, `COP_INT_PRIORITY` or `COP_FIQ_STAT`. Its "route to FIQ" helper (`osos 0x101F_C588`) sets bits in `CPU_INT_PRIORITY` only [apple-RE].
2. **The DMA driver registers source 26 as a FIQ on that service.** The general-engine driver (`osos 0x101D_98D0`) registers Apple's interrupt numbers 27 and 28 with the "FIQ" flag set, and number 38 (a software interrupt) as a plain IRQ. The service's number table maps 27 → hardware source 26 and 28 → 27 (filled in the constructor, `osos 0x101F_CC38`–`0x101F_CC48`). These are the only two FIQ registrations in the image [apple-RE].
3. **The CPU's FIQ path dispatches it.** The FIQ vector stub (`osos 0x1000_015C`) sends the CPU to `0x1027_70C0`, which calls the service's FIQ dispatcher (`osos 0x101F_C4D0`). That reads `CPU_FIQ_STAT` and `CPU_HI_FIQ_STAT` and calls the registered handler; for source 26 that is the general engine's channel walker (`osos 0x1026_80C4` → `0x101D_9C40`) [apple-RE].
4. **The COP has no FIQ handler.** Its FIQ path jumps to IRAM `0x4000_0000`. The IRAM image there (copied from `osos 0x1071_619C`) holds two zero words and then the software-interrupt return, an exception return that does not subtract 4. It acknowledges nothing and returns one instruction late. It is a trap, not a handler [apple-RE].
5. **The COP's IRQ serves one source.** Its IRQ handler (`osos 0x1026_78E8`) checks only `COP_HI_INT_STAT & COP_HI_INT_EN_STAT` bit 29 (source 61), the CPU's request to stop or park the COP, and returns otherwise [apple-RE].
6. **The I2S device belongs to the CPU.** The I2S device object (constructor `osos 0x1018_C85C`, created with base `0x7000_2800`) checks `PROCESSOR_ID` when it is opened (`osos 0x1018_C814`). On the CPU it resets the block (`IISCONFIG` bit 31 pulse) and builds its transmit and receive transfer objects on `IISFIFO_WR`/`IISFIFO_RD`. On the COP it packages the call and posts it to the CPU instead (`osos 0x1018_C7B8`–`0x1018_C7E8`) [apple-RE].

**Where the earlier reading went wrong.** `apple-dual-core.md` §1 said the COP "initialises its interrupt controller (enables source bit 26 at priority `0x3F`)", from the call at `osos 0x1027_6EE8`. That function takes component 5 from Apple's component registry, and component 5 is the **cache controller**: a 12-byte object, constructor `osos 0x101E_FD64`, vtable `0x1067_1084`. The call is its "configure" method (`osos 0x101E_FC2C`) with a cacheable size of `0x0400_0000` and a field value of `0x3F`. The method sets `0x6000_6044` bit 4 or 5 for the calling core, clears and initialises the cache through `0xF000_F040`/`F044` and `CACHE_CTL`, writes `0x3F << 6` into `0xF000_F044`, enables the cache, and primes it with 512 reads from `0x1000_0000`. This is Rockbox's `init_cache()` step for step. `0x0400_0000` equals `1 << 26` only by coincidence. Both cores make this call: the CPU from `osos 0x1026_68CC`, the COP from `0x1028_5B74`, and both again after a park (`0x100D_EDE8`) and after a resume (`0x1000_1138`) [apple-RE]. The component factory is the switch at `0x1493_6E20` in the shared section: id 0 is the interrupt controller, id 1 the DMA engine, id 5 the cache.

**What this does not change.** The COP still runs Apple's message-driven work engine, and the engine is still most likely the audio decoder [hypothesis, strong; `apple-dual-core.md` §2]. It loads IRAM overlays like a codec loader, the multiply-dense code sits in those windows, and the sleep manager stops it first. But the evidence that "the COP alone enables source 26" was the cache call, so it no longer counts. **Apple's audio, as read, is a hybrid: the COP computes, and the CPU owns I2S, the DMA engine and the DMA's FIQ.**

**What this means for us.** Core layout A (`../design/core-layout-decision.md`) still stands as *our* design. But "route source 26 to the COP" is **our extension beyond Apple**, not a copy of Apple. Apple offers no evidence for or against the COP's FIQ route, because it never uses it. Sitting 4gh's P-2 shows that a COP-routed *IRQ* (Timer 2) works on our unit [silicon]. Whether a COP-routed *FIQ* works is still open; §7 gives the cheapest test.

---

## 1. Mechanisms: what the silicon requires, and where each fits our design

These are the hardware facts Apple's sequence depends on, abstracted from its code. Each says what it means for ProkBox's design: K-1's park-until-work, the watermark wake (19-1c), per-core request words, and the bitstream ring.

| # | Mechanism | Evidence | Fit with our design |
|---|---|---|---|
| M1 | **Interrupt masks in the CPSR are per core, and only the core itself can change them.** A core takes an exception only when its own I/F bit is clear, whatever the controller does | ARM architecture; [apple-RE]: the COP clears both bits itself at the end of its stack set-up (`osos 0x1008_47A0`), and the CPU clears them in its kernel; [silicon, sitting 4gh]: the COP sat with F set (`cop_cpsr=0x5f`) and took no FIQ | Any COP interrupt plan needs **COP code** that clears the bit (in the COP's start-up, as the sitting-4gh fix on the layout-A line now does; quirk 74 there) and a way to put it back. A CPU-side "route" alone can never work |
| M2 | **Routing is per core: an enable mask and a FIQ-select mask for each core; raw status is common.** `*_INT_EN`/`DIS` are set/clear registers, and `*_INT_PRIORITY` bit = FIQ | [rockbox] `pp5020.h`; [apple-RE]: Apple drives only the CPU's set, through one class, and never writes `COP_INT_PRIORITY` (no reference to `0x6000_403C` in the image); [silicon]: COP Timer 2 as an IRQ (P-2, 1000/1000) | A COP FIQ route is ours to prove (§7). The **per-core status views** (`COP_FIQ_STAT` `0x6000_400C`, `COP_INT_STAT` `0x6000_4004`) show where a pending source is routed **even with the CPSR masked**. That is a free diagnostic |
| M3 | **One vector table, split by `PROCESSOR_ID` in the stubs.** FIQ mode banks r8–r14, so a stub can work in r10/r12 without a stack | [apple-RE] `osos 0x1000_015C` (the COP branch uses only banked registers, which is why the COP needs no FIQ stack) | Rockbox's vector page is shared and its FIQ goes through one pointer (`fiq_function`). A COP FIQ either shares the handler, which must then be core-aware, or needs a core split in the vector. Either way it needs its own FIQ stack, or no stack use at all (Rockbox's `cop_fiq_stack` is the CPU's `fiq_stack` top) |
| M4 | **The DMA engine raises its interrupt per channel**, from the channel's command bit 30, when a run completes; the status read acknowledges it. The engine does not refill itself: a stalled refill means silence one run later | [apple-RE] the general engine's stop clears bit 30 then bit 31 (`osos 0x1028_ECB0`); the FIQ walker tests status bit 30 per channel; [rockbox] `pcm-pp.c`; [silicon, sitting 4gh]: one 8 KB run (46 ms at 44.1 kHz) was heard as a blip | The refill deadline is one run long, so **run length sets both FIQ rate and slack**. A bigger run (or a ring of runs) lowers the wake rate of whichever core owns the FIQ. That suits the watermark wake: the decoder fills a deep PCM ring, and the FIQ only re-arms |
| M5 | **Devices have an owner core; other cores forward requests.** I2S, the DMA engines and the interrupt service live on the CPU; COP callers post a message | [apple-RE] `osos 0x1018_C814`/`0x1018_C7B8` (I2S); `dma.md` §6.6 (engine 2 refuses the COP) | Matches our **per-core request words**: one owner per device, others write a request word and wake the owner. With layout A the PCM device's owner becomes a design choice (§6) |
| M6 | **Interrupt work is split: the FIQ does the minimum and forces a software interrupt for the rest** | [apple-RE] the walker raises Apple's number 38 through the forced-interrupt registers when a channel's queue is not empty (`osos 0x101D_9CF4`–`9CFC`) | Same shape as our FIQ → `pcm_play_dma_complete_callback` split; for layout A the "rest" is a wake of the decoder core when the ring crosses its low watermark, not a per-run message |
| M7 | **The COP is woken by explicit writes, never left to an interrupt.** The CPU clears `COP_CTL` bit 31 when it posts work (five callers of `osos 0x100C_FB08`), and the park request writes `COP_CTL` = 0 as well | [apple-RE] `apple-dual-core.md` §3, `apple-power.md` §3.1 | Exactly K-1's **park-until-work**. Whether a COP-routed interrupt wakes a COP sleeping in `PROC_SLEEP` is **not shown by Apple** (the CPU's interrupts do wake the CPU: [silicon], daily Rockbox playback). If the COP owns the PCM FIQ, that wake is required; see hypothesis (e) |
| M8 | **Caches are per core and set up per core**, including after every park and resume | [apple-RE] the cache component's configure method (`osos 0x101E_FC2C`) is called on each core at start-up, on release from a park and on resume | Matches quirk 52 (write-back is per core): the core that writes PCM writes it back, and a DMA reading SDRAM needs that write-back first. A FIQ handler that only re-arms reads no PCM, so it needs no cache work |

---

## 2. Apple's sequence, step by step

### 2.1 Bring-up (both cores)

| # | Who | Step | Where | Class |
|---|---|---|---|---|
| 1 | both | Enter at `0x1000_0000`, remap, split on `PROCESSOR_ID` | `osos 0x1000_01F0`, `0x1000_0240` | necessary (each core must take its own path) |
| 2 | CPU | Quiesce: stop all channels of both DMA engines and IDE DMA; **disable every source on both cores** (`CPU_INT_DIS`, `COP_INT_DIS`, the high and forced registers = all ones) | `osos 0x1000_1244`, `0x1000_15FC` | necessary: a stale enable or a channel left running by the loader fires into half-built state |
| 3 | COP | Sleep in `PROC_SLEEP` until the CPU has built memory | `osos 0x1000_1498` | necessary for Apple (the COP must not run before `.data`/`.bss`); incidental in shape |
| 4 | CPU | Copy data, IRAM blocks (the COP's FIQ target at `0x4000_0000` among them) and the shared section; zero `.bss` | `osos 0x1008_3848` | necessary |
| 5 | CPU | Mode stacks, IRQ and FIQ included (`CPSR` `0xD1`, sp from the table at IRAM `0x4000_6000 + 0x2C`) | `osos 0x1008_46A0` | necessary for the CPU's FIQ: its stub pushes r0–r8 and lr |
| 6 | CPU | Interrupt-service set-up: enables sources 0 and 32 on the CPU; sets `0x6000_414C` bit 19; **routes sources 63 and 31 to the CPU's FIQ** (`CPU_HI_INT_PRIORITY` and `CPU_INT_PRIORITY` bit 31) and enables both. What sources 31 and 63 are is [unknown]. It reports an error if it ever runs on the COP | `osos 0x1022_2AB4`–`0x1022_2B20` | necessary in part (the routing); the source-31/63 FIQ bits are unexplained, so questionable until named |
| 7 | CPU | Per-core cache configure (§0) | `osos 0x1026_68CC` | necessary (M8) |
| 8 | CPU | Wake the COP (`COP_CTL` = 0) as the last step of its init | `osos 0x1000_0330` | necessary |
| 9 | COP | Resume check, then mode stacks: abort, IRQ, undefined, then **SVC with I and F both clear** (`0x13`). **No FIQ stack** | `osos 0x1000_10C0`, `0x1008_4734`–`0x1008_47A8` | the I clear is necessary (the COP's IRQ carries the park request). **The F clear is incidental**: no FIQ is ever routed to the COP, and its FIQ target (§0 item 4) could not serve one. **Questionable**: an F bit left clear in front of a trap vector would turn a stray COP route into an interrupt storm |
| 10 | COP | Shared and COP-specific init (`osos 0x1027_EAEC`, `0x1025_FCF8`; content [unknown]), then cache configure (`0x1028_5B74` → `0x1027_6EE8`) and the work engine's loop (`0x1024_92B0`, object `0x1493_9F60`) | `osos 0x1000_0314`–`0x1000_0328` | necessary for Apple's engine |

**What the COP never does:** write `COP_INT_PRIORITY`, read `COP_FIQ_STAT`, program a DMA channel, or touch I2S [apple-RE: no reference in the image; the I2S object forwards COP calls].

### 2.2 Starting playback (the CPU's side)

| # | Who | Step | Where | Class |
|---|---|---|---|---|
| 1 | CPU (COP calls are forwarded) | Open the I2S device: `IISCONFIG` reset pulse (bit 31 set, then cleared), build the transmit transfer object on `IISFIFO_WR` (`0x7000_2840`) and the receive one on `IISFIFO_RD` | `osos 0x1018_C6D8`, `0x101A_3080` | the reset is necessary (Rockbox does the same); the forwarding is Apple's ownership model (M5) |
| 2 | CPU | DMA driver init: register source 26 (Apple's 27) and 27 (28) **as FIQs** with the CPU's service, enable both; register software interrupts 38 and 13 as IRQs | `osos 0x101D_98D0`–`0x101D_9968` | necessary: the routing and the enable (M2) |
| 3 | CPU | Program the channel: peripheral side, RAM side, request line, count; set command bit 30 (interrupt), then bit 31 (start) | general-engine channel methods `osos 0x1028_E5xx`–`0x1028_EF24` (`dma.md` §6.5) | necessary. Which of the four general channels carries audio is chosen at run time; that it is channel 0 is [unknown] |

### 2.3 Each completed run (the refill)

1. The engine sets the channel's status bit 30 and raises source 26. Because `CPU_INT_PRIORITY` bit 26 is set, it is the CPU's FIQ [apple-RE for the routing; the raising is [rockbox]].
2. The CPU's stub saves r0–r8 and lr on its FIQ stack and calls `0x1027_70C0`, which gets the interrupt service (built on first use under a lock) and calls its FIQ dispatcher [apple-RE].
3. The dispatcher reads `CPU_FIQ_STAT`/`CPU_HI_FIQ_STAT` and calls the handler registered for source 26 [apple-RE].
4. The walker (`osos 0x101D_9C40`) goes through the active channel list (head at `0x1083_3CBC + 8`). For each channel whose status has bit 30 set, it calls the channel's completion step (`[channel + 0x88]`, which re-arms from a queued address pair or counts a completion; `dma.md` §6.4). If that channel's request queue is not empty, it forces software interrupt 38, and the IRQ that follows posts the waiting thread's semaphore [apple-RE].
5. The new buffer reaches the device through the transfer object's queue. Apple does not chain hardware descriptors: each run is one command word, re-armed from software [apple-RE, `dma.md` §6.3–§6.4].

How the COP's engine hands PCM to that queue was **not traced**: whether it posts each buffer through the mailbox semaphores and doorbell (`apple-dual-core.md` §3) or writes a shared ring that the CPU drains is [unknown]. The engine's queue and semaphore code is in IRAM (`0x4001_C8FC`–`DED4`); the I2S stream's queue code is in SDRAM (around `osos 0x101A_30D8`).

### 2.4 SDRAM or IRAM?

**Apple's FIQ path runs from SDRAM.** The stub (`0x1000_015C`), the CPU's FIQ entry (`0x1027_70C0`), the dispatcher (`0x101F_C4D0`) and the walker (`0x101D_9C40`) are all ordinary code in the cached logical view, and the channel list is in SDRAM `.data` (`0x1083_xxxx`). Only the stacks' table is in IRAM [apple-RE]. So Apple's audio interrupt path needs no IRAM-only discipline. It does mean the FIQ can never run while SDRAM is in self-refresh, and Apple's sleep turns IRQ and FIQ off first (`apple-power.md` §2.3 step 2). Whether the PCM buffers themselves are in SDRAM or IRAM was not established [unknown]. The general engine reaches either (`dma.md` §6.5).

**Coherency.** The FIQ path reads only the engine's registers and its own list, so it needs no cache maintenance. Whatever core writes PCM must write it back before the run starts (quirk 52). In Apple's hybrid that is the COP, if the COP decodes into a cached view. Apple also links a whole shared section at `0x14xx_xxxx` for cross-core data (open question 48).

---

## 3. Pause, stop, track change and sleep

| Event | What Apple does | Evidence |
|---|---|---|
| **Stop a channel** (pause, stop, end of stream) | IRQ and FIQ off on the calling core; clear command bit 30 (no more interrupts), then bit 31 (stop); unlink the channel from the active list; restore the CPSR. **The interrupt controller is not touched**: source 26 stays enabled and routed to the CPU's FIQ for the life of the system | `osos 0x1028_ECB0`–`0x1028_ED14` [apple-RE] |
| **Stop inside the completion path** | Same two bit clears, without the CPSR change (already in FIQ), then the unlink | `osos 0x1028_EB08` [apple-RE] |
| **Track change** | Nothing specific to the interrupt was found. A track change is a stream-level event, and the channel keeps running or is stopped as above | [unknown]; nothing in the interrupt code depends on tracks |
| **Light sleep** | The sleep manager stops the COP's engine first (`apple-power.md` §2.2 step 1); the IRAM sleep routine turns IRQ and FIQ off on the CPU, holds the COP, and gates `DEV_I2S` (`apple-power.md` §2.3 steps 2, 7, 13) | [apple-RE] |
| **Save/restore around sleep** | The CPU saves `CPU_INT_EN_STAT`, `COP_INT_EN_STAT` and their high twins (`osos 0x1014_7D48`), then restores them by writing all ones to `CPU_INT_DIS` and `COP_INT_DIS` and the saved words to `CPU_INT_EN`, `COP_INT_EN` and the high twins (`osos 0x1014_7B4C`). The priority registers are not part of that save; they persist | [apple-RE] |
| **Per-core "all off" and "restore"** | `osos 0x1000_1534` (save this core's enables to a shadow at `0x1498_30F0`, then write all ones to its `*_INT_DIS`) and `0x1000_0940` (write the shadow back to `*_INT_EN`) | [apple-RE] |
| **Park the COP (hibernation)** | Flag and acknowledge in uncached memory, plus the COP's one IRQ source (61) | `apple-power.md` §3.1 [apple-RE] |

**Necessary vs incidental.** Clearing bit 30 before bit 31 is necessary: it stops a completion interrupt from racing the stop. Masking the CPSR around it is necessary in Apple's design, because the FIQ walks the same list the stop unlinks. Leaving the route alone is a good idea we should copy: an always-on route with a per-channel interrupt bit makes pause and resume cheap.

---

## 4. Cross-version check (1.2.1, 1.2.3, 1.3)

Byte patterns searched in all three images (a throwaway script kept outside git; `xver.sh` for the sleep constants):

| Pattern | 1.2.1 | 1.2.3 | 1.3 |
|---|---|---|---|
| FIQ stub: the COP's jump to `0x4000_0000` | `0x1000_016C` | `0x1000_016C` | `0x1000_016C` |
| IRAM block head: two zero words, then the SWI return | `0x1070_7BC4` | `0x1071_3998` | `0x1071_619C` |
| The COP's stack set-up ends in SVC with I and F clear | `0x1008_6D1C` | `0x1008_479C` | `0x1008_479C` |
| DMA driver: number 27 registered as FIQ | `0x101D_0A14` | `0x101D_1C1C` | `0x101D_98FC` |
| DMA driver: number 28 registered as FIQ | `0x101D_0A38` | `0x101D_1C40` | `0x101D_9920` |
| Interrupt-controller constructor's register list (CPU registers) | `0x101F_3FD0` | `0x101F_51F0` | `0x101F_CF50` |
| The COP's IRQ: the bit-29 test | `0x1025_D7DC` | `0x1025_E9FC` | `0x1026_7904` |
| `xver.sh` sleep constants and strings | present | present | present |

**The design is the same in all three** [apple-RE].

---

## 5. Why P-1 failed: hypotheses, ranked

P-1 (sitting 4gh): source 26 was routed to the COP from the CPU (`CPU_INT_DIS`, `COP_INT_PRIORITY |= DMA_MASK`), and a COP job set its FIQ stack, enabled `COP_INT_EN` and started DMA0 on 8 KB runs of a tone for 10 s. Result: `cop_fiq=0 cpu_fiq=0`, `cop_cpsr=0x5f fbit=1`, and a brief blip instead of 10 s of tone.

| Rank | Hypothesis | For | Against | Cheap test |
|---|---|---|---|---|
| 1 | **(a) The COP never cleared CPSR F.** | The log: `0x5f` is System mode, I clear, **F set**. Rockbox's `crt0-pp.S` starts both cores at `0xdf`; only `main.c` (`enable_fiq()`, on the CPU) clears F; thread switches don't save the CPSR [rockbox]. Apple's COP clears F itself (§2.1 step 9), so the bit is plainly the core's own to clear (M1) [apple-RE]. A masked FIQ cannot be taken whatever the routing | none | **Already settled as the first-order cause** [silicon + code; quirk 74 on the 4gh line]. The retest (§7) reads the CPSR after the clear (`0x1f`) |
| 2 | **(c) The blip is a consequence of (a).** | One run is 8,192 bytes = 2,048 frames = **46 ms at 44.1 kHz**; with no FIQ nothing re-arms DMA0 (M4), so one run plays and the I2S FIFO drains | none | In the retest, count runs: `expect ≈ 215` in 10 s. A blip with `cop_fiq` ≥ 1 would point elsewhere |
| 3 | **(e) A COP in `PROC_SLEEP` may not wake for a FIQ.** | Apple never relies on it: the COP is always woken by a `COP_CTL` write (M7) [apple-RE]. The P-1 job ended and the COP went back to its idle sleep within milliseconds, so every later FIQ would arrive at a sleeping COP | P-2: Timer 2 routed to the COP as an **IRQ** counted 1000/1000 [silicon], so a COP-routed IRQ does wake the sleeping COP (or the COP was awake each time, which the 100/s tick pulse makes unlikely at 100 Hz) | Retest with K-1's parked idle: if `cop_fiq` counts only while a COP job spins, and drops when the COP sleeps, (e) holds. Cheaper split: route **Timer 2 as a FIQ** on the COP (P-2 with `COP_INT_PRIORITY \|= TIMER2_MASK`) |
| 4 | **(b) `COP_INT_PRIORITY` is not the COP's FIQ select, or has another meaning.** | Apple never writes it, so nothing in Apple supports it [apple-RE]; the name and the "1 = FIQ" meaning come from Rockbox's CPU use and iPodLinux's header [rockbox] | The register layout is regular: the COP's block mirrors the CPU's at +0x10, and the CPU's priority bit is proven daily [silicon] | **Read the status views with F still set** (no handler needed): after the first run completes, `INT_STAT` `0x6000_4010` bit 26 = 1, and `COP_FIQ_STAT` `0x6000_400C` bit 26 = 1 with `COP_INT_STAT` `0x6000_4004` bit 26 = 0 if the route works. If bit 26 shows in `COP_INT_STAT`, the source went to IRQ, not FIQ |
| 5 | **(f) The COP's FIQ entered but the handler failed.** | The COP reaches the shared vector page and `fiq_function` through its own MMAP0 remap [rockbox]; the probe's handler is a GCC `interrupt("FIQ")` function in SDRAM, run from the COP's cache | `cop_n` would then show at least 1, or the COP would hang (the dog did not fire) | The counter is the first statement after the status read; a non-zero count with no refill would indicate this |
| 6 | **(d) Priority or ownership conflict with the CPU.** | The CPU keeps `CPU_INT_PRIORITY` bit 26 set from `pcm-pp.c`, and `pcm_play_lock`/`unlock` on the CPU writes `CPU_INT_EN`/`DIS` for DMA | Playback was stopped, the CPU's enable was off, and `cpu_fiq=0`. Two cores routed to the same source is a normal controller state (per-core masks, M2) | Log `CPU_INT_EN_STAT & DMA_MASK` at the end of the run; it should still be 0 |
| 7 | **(g) The vector and stack belong to the CPU.** | Rockbox's `cop_fiq_stack` is the CPU's `fiq_stack` top | The probe set its own COP FIQ stack, and nothing fired anyway | Design requirement, not a cause: keep the probe's own stack, or write the handler to need none (FIQ-banked registers only, as Apple's COP stub does) |

**Ranking summary:** (a) is the cause, and (c) its audible symptom. (e) and (b) are the two real unknowns the retest must settle; (f), (d) and (g) are guards.

---

## 6. Alternatives (cost and risk)

These are our designs. Apple's own design is option 1.

| # | Design | How | Cost | Risk | Fits |
|---|---|---|---|---|---|
| 1 | **Hybrid, Apple's actual design: the COP decodes, the CPU takes the FIQ and only re-arms DMA** | Keep `pcm-pp.c`'s CPU FIQ (proven daily). The COP decoder fills a PCM ring in uncached or written-back memory, and the CPU's FIQ re-arms DMA0 from it. The FIQ handler reads the ring's read index, reads no samples, and calls nothing | Smallest change: no new silicon path. The CPU wakes once per run: 21.5/s at 8 KB, **2.7/s with 64 KB runs**, each a few µs of FIQ | Lowest. It conflicts with "prove the CPU asleep with the screen off" only at the wake rate, and a large run size makes that small. The ring indices cross cores and must sit in the uncached view (quirk 54) | Layout A with the CPU as the device owner (M5), watermark wake for the COP's decoder, per-core request words for control. **Recommended fallback, and arguably the primary design** |
| 2 | **The COP takes source 26 as a FIQ** (P-1's goal) | COP's start-up clears F; COP FIQ stack; `CPU_INT_DIS`, `COP_INT_PRIORITY`, `COP_INT_EN`; a core-aware `fiq_function` | Moderate: `pcm-pp.c`'s locks become per core or COP-only (`core-layout-decision.md` §4.1); the vector shared through one pointer | (e) and (b) unproven; a COP FIQ storm if the COP's handler is ever missing while the route is set (Apple's trap vector shows how that would look) | Layout A as written; the CPU then sleeps through playback entirely |
| 3 | **The COP takes source 26 as an IRQ** | Same route without the priority bit; the COP's `irq_handler` branch handles DMA | Small on top of option 2; P-2 proved a COP IRQ route | IRQ latency: the COP's IRQ is masked during its own critical sections, and the I2S FIFO holds only a few frames once a run ends. Fine if the COP's masked sections stay short (the refill deadline is the FIFO's drain time, not the run's length) | Layout A; avoids the FIQ vector sharing entirely |
| 4 | **Long runs or a ring of runs, so interrupts are rare** | Runs of 32–64 KB (the count field allows 64 KB − 4), with the next address queued; or two channels alternating | Memory: a 64 KB run is 370 ms of 44.1 kHz audio; latency for pause/seek grows unless stop is immediate (it is: bit 31 clear) | Low; the engine has no hardware descriptor chain, so "chaining" is still one interrupt per run | Combines with any of 1–3; with option 1 it makes the CPU's FIQ rate negligible |
| 5 | **Polled refill from the COP engine's loop** | No interrupt: the COP's decoder loop checks DMA0's status (bit 30 or the remaining count) and re-arms | Cheapest code | **A gap whenever the COP sleeps or works longer than one run**, so it forbids K-1's park-until-work during play unless runs are long and the COP wakes on a timer; burns COP wakes | Only as a lab tool or with option 4's long runs |

**Ranked for ProkBox:** 1 + 4 (proven path, low risk, the CPU's FIQ rate made negligible), then 3 (if the CPU must stay fully asleep), then 2 (only if 3's latency matters, after the §7 retest passes), and 5 not for shipping.

---

## 7. Recommended retest: our design, informed by Apple

Apple contributes three mechanisms: the routed core clears its own mask (M1), the route is set once and left alone (§3), and the core that owns the interrupt is woken explicitly (M7). The rest is ours. The retest is ordered so that each step can only fail for one reason.

| Step | Who | Action | Expected log [hypothesis until run] |
|---|---|---|---|
| R0 | – | PCM stopped, DAC held awake (as P-1) | – |
| R1 | CPU | Route: `CPU_INT_DIS = DMA_MASK`; `COP_INT_PRIORITY \|= DMA_MASK`. **Do not enable the COP yet** | `cpu_en=0 cop_pri=1` |
| R2 | COP job | Read the CPSR (expect `0x5f` today, or `0x1f` once the COP's start-up clears F); install the COP's own FIQ stack; **clear F** (`msr cpsr_c` with F = 0, I unchanged); read the CPSR again | `cop_cpsr_before=0x5f cop_cpsr_after=0x1f` |
| R3 | COP job | **Diagnostic run with F set again** (re-mask for R3–R4): read `DMA0_STATUS` to clear stale requests, `COP_INT_EN = DMA_MASK`, start **one** 8 KB run, spin 60 ms with F masked | – |
| R4 | COP job | Read `INT_STAT`, `COP_FIQ_STAT`, `COP_INT_STAT`, `CPU_FIQ_STAT` (bit 26 of each) | `raw=1 cop_fiq_stat=1 cop_irq_stat=0 cpu_fiq_stat=0`: **settles (b)** |
| R5 | COP job | Clear F; the pending FIQ is taken at once; the handler counts, reads `DMA0_STATUS`, re-arms | `cop_n=1` within µs of the clear: the route and the vector work |
| R6 | COP | Leave the job; the COP returns to its idle (`PROC_SLEEP`). The FIQ re-arms on its own for 10 s | `p1 cop_fiq≈215 (204..226) cpu_fiq=0 tone=1000Hz` and a clean tone: **settles (e)** |
| R7 | CPU | Unroute in the stop order of §3: `COP_INT_DIS`, then DMA0 bit 30, then bit 31, then the priority bit; a COP job sets F again | `stack=back dog=0` |

If R4 passes and R6 fails with a count that tracks how long the COP stays busy, (e) holds. Then option 3 (IRQ, which P-2 shows wakes the COP) or option 1 is the answer, not a further fix to the FIQ route. If R4 shows bit 26 in `COP_INT_STAT`, (b) holds: the COP's priority register does not select FIQ, and option 3 is the natural route.

---

## 8. What was not found

- How the COP's engine passes PCM to the CPU's I2S queue (mailbox semaphores, the doorbell or a shared ring) [unknown].
- Which hardware events Apple routes to the CPU's FIQ as sources 31 and 63 [unknown].
- The content of the COP's start-up calls `osos 0x1027_EAEC` and `0x1025_FCF8` [unknown].
- Who raises source 61 for the COP's park, and how it is enabled for the COP (the forced-set registers are not reached by a literal) [unknown].
