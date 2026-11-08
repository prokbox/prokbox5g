# How Apple used the two cores, and a hardware-use comparison with ProkBox

Desk study, 2026-11-05, the companion of [`apple-power.md`](apple-power.md) (same method, rules and tags: static reading of `osos` 1.3, cross-checked against 1.2.1/1.2.3; `osos 0x1xxx_xxxx` = load address + file offset; **[apple-RE]** = read by us from Apple's code in this study). Part 1 is what Apple's code does with the CPU and the COP; the audio interrupt path, step by step, is in [`apple-cop-audio.md`](apple-cop-audio.md). Part 2 compares the hardware features Apple's firmware uses with ProkBox's, and says for each difference whether it suggests an improvement, a risk, or nothing.

---

## Part 1: Apple's dual-core design

### 1. Start-up and the split

| Step | CPU | COP | Evidence |
|---|---|---|---|
| Entry | both cores enter `osos 0x1000_0000`; the reset vector programs the remap windows (`0xF000_F000`–`0xF000_F014`) and jumps to the logical address | same | [apple-RE] `osos 0x1000_01F0` |
| Identity | `PROCESSOR_ID` byte `0x55` | `0xAA` | [apple-RE] `osos 0x1000_0240` |
| Stack | IRAM `0x4000_3FF8` | IRAM `0x4000_5FF8` | [apple-RE] |
| First action | quiesce: stop every channel of both DMA engines and the IDE DMA, disable every interrupt on both cores (`0x6000_401C`/`28`/`38`, `0x6000_411C`/`28`/`38` = `0xFFFF_FFFF`) | puts itself to sleep (`COP_CTL` = `PROC_SLEEP`) until the CPU has initialised memory | [apple-RE] `osos 0x1000_1244`, `0x1000_15FC`, `0x1000_1498` |
| Resume check | hibernation marker at `0x4001_FF10` (`apple-power.md` §3.2) | same, its own half | [apple-RE] |
| Memory set-up | copies `.data`, the IRAM blocks and an uncached data section, zeroes `.bss` | asleep | [apple-RE] `osos 0x1008_3848` |
| Release | wakes the COP (`COP_CTL` = 0) as the last step of its own init | configures its own cache (`osos 0x1027_6EE8`, the cache component, **not** the interrupt controller; corrected 2026-11-06, [`apple-cop-audio.md`](apple-cop-audio.md) §0) and runs the **work engine** forever (`osos 0x1028_5B70` → `0x1024_92B0`, object `0x1493_9F60`) | [apple-RE] `osos 0x1000_0330`, `0x1027_6EE8` |
| Interrupt handlers | IRQ → `osos 0x1028_9E30`, FIQ → `0x1027_70C0` (the audio DMA's FIQ, source 26) | IRQ → `osos 0x1026_78E8` (serves source 61 only, the park request); FIQ → IRAM `0x4000_0000`, which is a trap, not a handler (`apple-cop-audio.md` §0) | [apple-RE] the vector stubs at `osos 0x1000_015C`/`0x1000_0180` branch on `PROCESSOR_ID` |

### 2. What runs where

- **CPU:** the RTOS with the UI, the file system and disk, USB, the driver tasks (`apple-power.md` §5.5), the BCM driver, the power manager. The second DMA engine's transfer object refuses to run on the COP (`dma.md` §6.6) [apple-RE].
- **COP:** a single message-driven **work engine**. Its loop takes the next message from one of two queues; for the first kind it first **swaps an overlay into IRAM** (`0x4001_8800`, `osos 0x1008_398C`) and then runs it; when both queues are empty it waits (`IRAM 0x4001_D724` through `osos 0x1008_45D4`) [apple-RE]. The engine's core lives in IRAM (`0x4001_C8FC`–`0x4001_DED4`, 5.5 KB) and models a graph of stages with per-stage state [apple-RE].
- **What the engine does is not named in the code**, but the pointers say audio: the engine swaps IRAM overlays like a codec loader, the multiply-dense synthesis code sits in the IRAM overlay windows (`mac-and-dvs.md` §1.2), and the sleep manager stops this engine first [apple-RE for each; "the COP decodes audio" is a **[hypothesis, strong]**]. An earlier reading also counted "the COP alone enables source 26"; that call is the per-core cache set-up, and **the audio DMA's interrupt is the CPU's FIQ**: the CPU owns I2S, the DMA engines and their interrupts, and COP calls to the I2S device are forwarded to the CPU (`apple-cop-audio.md` §0, §2) [apple-RE].
- **The BCM** decodes video, runs its own power governor and owns the panel (`../bcm2722-reference/brief-map-power.md` §2).

### 3. How the cores talk

| Mechanism | What Apple does | Evidence |
|---|---|---|
| **Mailbox semaphores** | a table maps each cross-core semaphore to a bit of the mailbox: test in `MBX_MSG_STAT` (`0x6000_1000`), take by `MBX_MSG_CLR` (`+0x08`), give by `MBX_MSG_SET` (`+0x04`); a waiter that finds the bit clear blocks in the kernel and retries | [apple-RE] `IRAM 0x4001_D7F0`–`0x4001_D868` |
| **Queue doorbell** | a post writes `0xA000_0000 \| (id << 20)` to `CPU_QUEUE` (`0x6000_1010`) under a byte lock in IRAM (`0x4000_0FAC`), after checking a second "busy" byte | [apple-RE] `IRAM 0x4001_D8D0`; that the write raises the other core's queue interrupt is [hypothesis] (Rockbox names the register but does not use it this way) |
| **Wake** | the CPU clears `COP_CTL` bit 31 only when the COP is asleep (five callers, one of them the clock governor's pass, §5) | [apple-RE] `osos 0x100C_FB08` |
| **Park** | flag byte + acknowledge byte in uncached memory (`0x1493_713B`/`C`); the COP saves its context, acknowledges, and loops on `PROC_SLEEP` while the flag stays set | [apple-RE] `apple-power.md` §3.1 |
| **Idle** | both cores share one idle routine and sleep in `PROC_SLEEP` (`apple-power.md` §5.1) | [apple-RE] |

### 4. Shared memory and caches

- **An uncached view for shared data.** Apple links a whole shared section at `0x1493_690C`: about 306 KB copied at start-up and 557 KB zeroed after it (`osos 0x1008_3848` table), and keeps the cross-core flags, the park bytes, the engine object (`0x1493_9F60`) and even some code (thunks into `0x1493_6AD0`) there [apple-RE]. Apple's DMA driver also classifies `0x14xx_xxxx` addresses as SDRAM (`dma.md` §6.5). So `0x14xx_xxxx` is a second SDRAM view that Apple treats as shared [apple-RE]; that it is uncached (the 64 MB mirror above the physical range) is [hypothesis].
- **Cache maintenance is whole-cache.** Start-up and the hibernation writer use `CACHE_OPERATION` (`0xF000_F044`) bits 1 **and** 2 together (write back and invalidate) and wait for `CACHE_CTL` bit 15; the cache is then disabled by clearing `CACHE_CTL` bit 0 and zeroing `0xF000_F040`/`F044` [apple-RE] `osos 0x1000_1390`, `0x1000_135C`. Rockbox avoids the invalidate bit (quirk 9); Apple uses it, but only with the cache about to be disabled or at boot.
- **Before a DMA to the BCM port:** a whole-cache write-back, range arguments ignored (`dma.md` §6.2 step 5) [apple-RE].

### 5. Parking the COP when idle

The COP sleeps in its idle routine with `PROC_SLEEP`. The CPU wakes it when it posts work (§3), **and** once per clock-governor pass, every 10 ms, while the audio engine runs. The first edition of this chapter said there was no periodic pulse; that was wrong (corrected 2026-11-06) [apple-RE]:

- **The pulse.** The governor pass (`osos 0x100C_164C`) copies both cores' 64-bit idle totals from `0x1498_3040` and only then calls the wake (`osos 0x100C_17DC`–`17F8`, then `bl 0x100C_FB08` at `0x100C_17FC`).
- **The wake's guards** (`osos 0x100C_FB08`). It returns at once unless the word at `0x1760` is nonzero, the caller is the CPU (`PROC_ID` reads `0x55`), and the byte at `0x1493_7139` is 0. Then it clears `COP_CTL` bit 31 if it is set. What `0x1760` means is [hypothesis]: the idle routine tests the same word first, so most likely "the cores are running their schedulers".
- **The byte `0x1493_7139` is the audio-stop hold.** `osos 0x1018_A900` sets it, writes 0 to `COP_CTL` (a wake), waits until the COP sets `0x1493_713A`, then clears `DEV_EN` bit 11 (`DEV_I2S`). `osos 0x1018_A998` sets the bit again and clears the byte. While the hold is set the COP loops in `PROC_SLEEP` and the governor does not pulse it. So Apple pulses the COP only while the audio engine runs, dark playback included, and never while it is stopped (disk mode).
- **Why: the COP's idle window closes only on exit.** The idle routine (`osos 0x100D_ED04`) masks IRQ and FIQ, notes `USEC_TIMER` on entry, and adds the elapsed time to the core's total (`0x1498_3048` for the COP) only on the way out (`osos 0x100D_EE3C`–`EE5C`); an interval in which the counter wrapped is dropped (`subcs`/`movcc`). Without a wake, a sleeping COP's total would stay stale for the whole sleep, and the governor would read it as busy. The read-then-wake order also means the governor never reads a 64-bit total while the COP is writing it.
- **Nothing else depends on it.** The COP enables only interrupt source 26 for itself; the pulse carries no work.

ProkBox's governor reads a sleep in progress directly (a start stamp plus a sequence counter, `../design/pp-governor.md` §17.6), so it needs no pulse to close the window. For hibernation the COP is parked by handshake (§3). For light sleep it is held by writing `0x4000_0000` to `COP_CTL` around the clock change (`apple-power.md` §2.3 step 7).

---

## Part 2: Hardware use, Apple vs ProkBox

Our side from `../data-paths.md`, `../design/p19-placement.md`, `../design/p19-ui-on-cop.md`, `../hardware-features.md` and `datasheet-features.md`. "Suggests": **improvement** (worth doing), **risk** (something to watch or avoid), **nothing**.

| Feature | Apple | ProkBox | Suggests |
|---|---|---|---|
| **Core split** | CPU: UI, OS, disk, USB, drivers; COP: one audio work engine [hypothesis, strong]; BCM: video, panel | PP1 (CPU) = playback back end (decode in dark play, storage, power); PP2 (COP) = UI front end; VC (BCM) = display jobs (glides, drift, marquee, Cover Flow) | **nothing to copy**: our split is the mirror image and fits our dark-playback goal (one core, COP parked). It is also a **risk note**: Apple kept the audio on the core that never runs the UI, which our design matches only once the UI is fully on the COP (`p19-placement.md` §4.3) |
| **Cross-core messaging** | mailbox semaphore bits + a queue doorbell (`CPU_QUEUE`) | queues in uncached memory + `core_wake()` + cache operations per hand-off (`data-paths.md` R7) | **improvement (small)**: the mailbox bits are hardware-atomic set/clear, which could replace Peterson locks on bytes (PP #8 says `SWP` across cores is unproven); probe first |
| **COP idle** | sleeps until the CPU posts work | woken 100/s by the CPU's tick today; K-1 parks it | **improvement**: K-1 is exactly Apple's model; land it |
| **Shared memory** | a large linked section in the `0x14xx_xxxx` view, flags and engine state there | `SHAREDBSS`/`XCORE_BSS` through the `0x1000_0000` uncached alias | **nothing** (same idea); **risk**: don't use `0x14xx_xxxx` without a silicon check that it mirrors SDRAM uncached |
| **Cache** | whole-cache write-back + invalidate (bits 1+2) at boot and before the cache is disabled | write-back, then a manual status rewrite instead of the invalidate bit (quirk 9) | **nothing**: Apple's use is consistent with quirk 9 (it never invalidates a live cache under load) |
| **IRAM** | resident sleep/clock code, an interrupt path, the COP engine, three overlay windows swapped per job (§6.1 of `apple-power.md`) | codecs and hot paths; "IRAM is for codecs only" (rule 3) | **improvement**: overlays let ~100 KB of hot code share 96 KB; **and** a documented exception for the self-refresh sleep routine (it must run with SDRAM off) |
| **DMA engines** | general engine for I2S (**CPU FIQ**, source 26; corrected 2026-11-06), second engine for the BCM port in 64 KB pieces (CPU only) | general engine for I2S (CPU FIQ), second engine for the BCM port (E2, [silicon] S37), IDE DMA | **nothing** for the BCM path (we already match it); **nothing to copy for the route**: Apple's audio DMA interrupt is on the CPU like ours; routing it to the COP is our own extension (`apple-cop-audio.md` §6–§7) |
| **MAC unit** | ARM long multiplies in IRAM, no coprocessor (`mac-and-dvs.md` §1) | the same in Rockbox's codec assembly | **nothing** |
| **DVS / PLL bands** | PLL up to 125 MHz with a speed grade, `DEV_TIMING1` by a 90 ns rule, fixed 1.2 V core | 24 (PLL off) / 30 / 80 MHz, fixed 1.2 V | **improvement (probe)**: the speed grade read and the 100 MHz and DVS probes (`mac-and-dvs.md` §5) |
| **Cache controller modes** | cache disabled around hibernation; the `0x6000_3000` arbitration table saved and restored around sleep | cache always on; the table untouched | **improvement (probe)**: read the table (PP #34); **risk**: a sleep that gates clocks must save/restore it as Apple does |
| **Clock gating** | `DEV_EN` cut to `0xC001_0108` in light sleep; two extra bits gated per board | `DEV_EN` at Rockbox's start-up value; drive and BCM power cuts | **improvement**: A2 in `apple-power.md` §7 |
| **SDRAM low power** | self-refresh request/acknowledge in `XMB_RAM_CFG` | none | **improvement**: A1 (the largest sleep item) |
| **BCM roles** | video decode, panel, a load governor (`pm_set_policy min`); the hibernate pin path coded but never called | display jobs at 27 MHz, 150 MHz for video and Class C; cold cut in dark; hibernate path under study | **improvement**: we already use the chip for more than Apple did (UI jobs); the hibernate pin path is our chance to wake faster than Apple (`brief-map-power.md` §3) |
| **I2S / codec** | codec power modes through a three-variant driver; codec as clock master | DAC standby after a pause (P15, measured wake 6.4 ms median) | **nothing** now; **open**: whether Apple standbys the codec on pause (`apple-power.md` §5.3) |
| **USB modes** | device (mass storage, a UAC1 isochronous source for docks), host (camera connector), PHY suspend before sleep, no core sleep with a cable in | device mass storage; isochronous probe built, unproven; no PHY suspend | **improvement**: A3 and A7 (`apple-power.md` §7); isochronous IN is proven by Apple's use (quirk 58) |
| **PMU** | LDO sleep mode `010`, standby via `GOSTDBY` only, alarm "off" year `0xAA`, interrupt masks `0x50/0xFC/0x0F`; never changes the wake enables | LDOs at lower voltages (2.5/2.6 V), standby with `CHGWAK \| EXTONWAK (\| RTCWAK)`, alarm "off" year `0x99` | **improvement**: A4 (LDO sleep mode); **risk**: our standby rewrites the wake bits, Apple's never does; both work, but a wrong value there leaves a unit that cannot wake (quirk 39) |
| **Suspend-to-disk** | `hibe` image through the ROM | none | **later**: A10 |
| **Usage statistics** | per-core idle time, sleep/disk/backlight totals in a log | power log rows | **improvement (small)**: A11 |

**ProkBox uses that Apple didn't:** the BCM for UI composition (glides, drift, marquee, Cover Flow), the PLL fully off while playing dark at 24 MHz, the drive power cut between refills (flash), lower LDO voltages, a DAC standby measured at 6.4 ms wake, the second DMA engine driven from a measured recipe rather than a driver policy, and the COP as a decode engine selectable per screen state. None of these has a downside in Apple's code; the open question for each is the measured saving.
