# DMA

The PP5022 has three DMA mechanisms that matter on the iPod Video:

> **See also [`../data-paths.md`](../data-paths.md)**: which DMA paths work on our unit, the recipes, and why PP DMA into the BCM's host port is a dead end so far.

| Engine | Used for | Section |
|---|---|---|
| General DMA engine, 4 channels at `0x6000_B000` | I2S playback (channel 0); our project's feed of the BCM2722 host port (channel 3) | §1–§4 |
| Second DMA engine, channels at `0x6000_9000` | Apple: every host-port write of 256 bytes or more; unused by us so far | §6 |
| IDE controller's own DMA (`0xC300_0400`) | ATA reads/writes | `storage.md` |
| USB controller's bus-master DMA | USB transfers | `usb.md` |

A fourth engine exists that no open-source driver uses: the **second DMA engine**, master register at `0x6000_8000`, channels at `0x6000_9000 + 0x20·n`. Apple's firmware drives it for every large write into the BCM2722's host port, including the upload of the BCM's image, in pieces of up to 64 KB [apple] (osos `0x1028_7BE8`, `0x1028_6FB4`; [external] siggi `08`, `10`). Its registers, Apple's programming sequence and how it differs from the general engine are in **§6**. **It works on our unit (S37):** it is the engine that reaches the host port; the general engine moves nothing there, nor even memory → memory.

All of these are bus masters that see **physical** addresses and bypass the cache.

## 1. The general DMA engine: registers [rockbox] `pp5020.h`

| Address | Name | Meaning |
|---|---|---|
| `0x6000_A000` | `DMA_MASTER_CONTROL` | bit 31 `EN`: enable the engine |
| `0x6000_A004` | `DMA_MASTER_STATUS` | bits 24–27: channel 0–3 status (meaning: active or interrupt pending; not settled) |
| `0x6000_A008` | `DMA_REQ_STATUS` | one bit per request ID (`1 << req`). Rockbox sets the I2S bit before use ("enable request??" — operation unaffected either way in their tests) |

Channel `n` (0–3) occupies 32 bytes at `0x6000_B000 + 0x20·n`:

| Offset | Name | Meaning |
|---|---|---|
| `+0x00` | `DMAn_CMD` | command (below) |
| `+0x04` | `DMAn_STATUS` | `[15:0]` bytes remaining; bit 30 interrupt pending; bit 31 busy. **Reading it clears the interrupt** |
| `+0x10` | `DMAn_RAM_ADDR` | memory address (physical) |
| `+0x14` | `DMAn_FLAGS` | bit 24 `ALIGNED`; bit 26 (unknown; Rockbox sets it for I2S) |
| `+0x18` | `DMAn_PER_ADDR` | peripheral address |
| `+0x1C` | `DMAn_INCR` | peripheral-side increment and width (below) |

`DMAn_CMD`:

| Bits | Name | Meaning |
|---|---|---|
| `[15:0]` | `SIZE` | transfer length in bytes **minus 4** |
| `[19:16]` | `REQ_ID` | peripheral request line |
| 24 | `WAIT_REQ` | pace the transfer by the peripheral's request |
| 25 | – | unknown |
| 26 | `SINGLE` | stop at the end; without it the channel reloads and repeats |
| 27 | `RAM_TO_PER` | direction memory → peripheral (clear: peripheral → memory) |
| 28 | `SLEEP_WAIT` | unknown effect |
| 30 | `INTR` | interrupt on completion |
| 31 | `START` | start; clearing it stops the channel |

`DMAn_INCR`:

| Bits | Meaning |
|---|---|
| `[18:16]` `RANGE` | 0 unlimited (incrementing), 1 fixed address, 2 alternating, 3–7 wrap within 4/8/16/32/64 |
| `[30:28]` `WIDTH` | 0 8-bit, 1 16-bit, 2 32-bit (other values unknown) |

Request IDs known [rockbox]: **2 = I2S**, 13 = SD host controller (Sansa; absent on the iPod). No ID is known for the BCM host port, the ATA controller or the UARTs.

All channels share `DMA_IRQ` (source 26); a handler reads each channel's `STATUS` to find which finished.

Maximum length per command is 65,536 bytes (16-bit size field).

## 2. Audio playback on channel 0 [rockbox] `pcm-pp.c`

```
init:   DMA_MASTER_CONTROL |= EN
        CPU_INT_PRIORITY  |= DMA_MASK        ; DMA interrupt → FIQ
        DMA_REQ_STATUS    |= 1 << 2           ; I2S request
        (read DMA0_STATUS)                    ; clear stale interrupt
setup:  DMA0_PER_ADDR = 0x70002840            ; IISFIFO_WR
        DMA0_FLAGS    = 1 << 26
        DMA0_INCR     = RANGE_FIXED | WIDTH_32BIT   ; 0x2001_0000
start:  DMA0_RAM_ADDR = physical buffer address
        DMA0_CMD      = (2<<16) | RAM_TO_PER | SINGLE | WAIT_REQ | INTR
                        | (bytes - 4) | START
```

- Each 32-bit word is one stereo frame (left in the low halfword), matching the I2S FIFO format Rockbox selects (`audio.md`).
- Chunks are limited to about `sample_rate >> 6` bytes (≈ 3.9 ms of audio) so the completion FIQ doubles as a coarse playback clock; the last chunk of a buffer is merged if fewer than 64 bytes (one FIFO) would remain.
- **Chunks must be whole 32-bit words** [ours, #191]. Upstream used `sample_rate >> 6` unrounded: 689 bytes at 44.1 kHz (750, 375, 250, 187 and 125 at 48, 24, 16, 12 and 8 kHz), so a 1 KB mixer frame went out as 689 + 335 bytes and every chunk after the first started 1–3 bytes off a word boundary, while `DMA0_INCR` says 32-bit width. ProkBox rounds down to a multiple of 4 (`(rate >> 6) & ~3`: 748, 688, 500, 372, 344, 248, 184, 172, 124 bytes). The buffers themselves are already whole words at word addresses (`ALIGN_AUDIOBUF`; the DMA only ever reads the mixer's `downmix_buf`). Host test: `tests/pcmdma`. **What silicon does with a length or address that is not whole words is unknown** [hypothesis, quirk 89]: candidates are that it truncates to whole words (a frame dropped per chunk), rounds up (reads past the chunk, repeats bytes), or ignores the address's low bits (plays shifted, rotated samples). The emulator models truncation. Bench row `dmaodd` (branch `p20-pcm-dma-bench`) A/Bs 688 and 689 with register read-backs at each FIQ.
- Completion FIQ: read `DMA0_STATUS` to acknowledge, add `(DMA0_CMD & 0xFFFF) + 4` to the address, start the next chunk.
- **Stopping cleanly:** clear `START|INTR` in `DMA0_CMD`, then wait until `DMA0_STATUS` shows neither busy nor interrupt. If it was busy, the bytes done are `(CMD & 0xFFFC) − (STATUS & 0xFFFC)`; if an interrupt was pending the whole chunk completed (the status register has already **reloaded** the size from `CMD`). Then wait for the I2S TX FIFO to drain.
- Locking against the FIQ is done by masking `DMA_MASK` in `CPU_INT_DIS`, not by disabling FIQ globally.
- Buffers: a logical (cached) SDRAM address is converted with `addr | 0x1000_0000` after a cache flush (`pcm_dma_addr()`, `dma_tx_buf_prepare()`).

### 2.1 FIFO continuity at high rates: the two-channel queue (0.8.3 fifo96)

**The problem** [silicon, owner's iPod, build 9b899de6c2, 2026-11-11]. With one channel, the next piece can only be armed from the completion FIQ, and from the piece's end the I2S FIFO lasts its 16 words: 333 µs at 48 kHz 16-bit, 167 µs at 96 kHz 16-bit, **83 µs at 96 kHz 24-bit** (format 3 is one word a sample, so 8 frames). A 24/192 FLAC played at 96 kHz 24-bit logged `codec fiq e=70 gap=1532 m=29/0`: 70 FIQs found the FIFO empty, the longest gap between FIQs was 1,532 µs against a 1,333 µs piece (one 1 KB mixer frame is 128 frames at 96 kHz 24-bit, one piece), so FIQs came ~200 µs late; the COP's cache drop held the FIQ off 29 µs at most, so it was not the cause. 16-bit 96 kHz (5 tracks) and 24-bit 48 kHz (12 tracks) were clean or nearly (one e=1); 16/44.1 showed e=1 on 3 of 109 tracks. Lateness, not the handler, is the variable: the handler re-arms before it mixes (the mix runs in the `PCM_DMAST_STARTED` callback, after `dma_tx_start()`).

**What delays the FIQ: ranked candidates** (none proved; the `fiq2` counters below separate them):

| # | Candidate | For | Against | Evidence |
|---|---|---|---|---|
| 1 | A section on the COP with the FIQ masked, other than the cache drop | the decode at 192 kHz in is the heaviest COP load seen | `m=` covers only the cache drop; by grep no other path the COP runs masks the FIQ (the PP sites of `disable_interrupt(IRQ_FIQ_STATUS)` are the cache drops, the clock change's `scale_suspend_core()`, the second engine's stop in `e2-port.c`, sleep, reboot and panic, all on the CPU except the COP's own drop) | [hypothesis] |
| 2 | The PCM lock held long by the CPU (it masks the COP's source; the CPU can take IRQs while holding it) | `lock_xcore` locks happen at every CPU-side mixer call | no periodic CPU-side lock during steady play found by grep | [hypothesis] |
| 3 | The core stalled, not masked: a bus master holding the path the FIQ's code needs (the cache controller's write-back, which runs with the FIQ open since 0ebecb0db6; the CPU's waits on the BCM2722 port) | stalls of tens of µs are measured on the port (`../data-paths.md` §2 item 2) | the FIQ's code and data are IRAM; whether a write-back or a port wait blocks the other core's IRAM or peripheral access is unknown | [hypothesis] |
| 4 | Clock changes (`scale_suspend_core()` stops the other core around each `CLOCK_SOURCE` write) | the governor moves with the COP's demand (p083-copclock) | the stop is a few register writes long; the lock wait runs at 24 MHz with both cores running | [rockbox-derived] code; [hypothesis] for the length |
| 5 | The handler's own length at 24-bit | the s24 mix runs in the FIQ | it runs after the re-arm, and 1.3 ms is far longer than any mix measured | [ours] code reading |

**Options to make continuity independent of FIQ latency:**

| Option | What | Evidence | Verdict |
|---|---|---|---|
| Linked descriptors / chaining | the next piece queued in hardware | none: the engine has one command word a channel, no descriptor; Apple re-arms each run from software [apple-RE] (`apple-cop-audio.md` §2.2 step 5) | not available |
| Repeat mode (`SINGLE` clear) | the channel reloads its count and runs again | [rockbox] header comment only ("no auto reload"); where the address goes after a reload is unknown | unproved, and a ring needs a reload address we cannot set; not used |
| An earlier interrupt (FIFO watermark, half-piece) | an interrupt before the FIFO empties | the I2S block's TX interrupt (`IRQTX`, source 10) fires at its attention level, at most 12 of 16 words free [rockbox] (`audio.md` §1): its feeder has the same FIFO-sized deadline; a half-piece interrupt needs a second command, which is the queue below | no gain alone |
| **Two channels, ping-pong** | channel 0 and 1 on the I2S request; one plays, the other waits armed | four channels share source 26 and a status each [rockbox]; Apple's driver opens one channel a stream [apple-RE]; how the engine arbitrates two channels on one request is **unknown** | **chosen**, behind a silent probe at boot |
| Bigger pieces (2–4 KB at 96 kHz 24-bit) | fewer FIQs | the slack after a piece is still the FIFO's 83 µs; a stall that overlaps a piece's end still clicks. Fewer ends a second means fewer chances (4 KB: a quarter as many clicks for the same stalls), but more IRAM for the mixer frame | helps in proportion, does not fix |

**The design** [ours; code `pcm-pp.c` "pcm-queue"; host model `tests/pcmdma/test_pcmq.c`]. While one channel plays a piece, the other holds the next one, armed and waiting; when the first ends, the engine moves to the second with no software in between, and the FIQ arms the freed channel with the piece after. The FIQ's deadline becomes a whole piece (1,333 µs at 96 kHz 24-bit, less the mix; 3.9 ms at 44.1 kHz 16-bit for the 688-byte piece) instead of the FIFO's 83 µs. The mixer's two buffers are enough: at a completion that ends a buffer, `PCM_DMAST_STARTED` first mixes into the buffer just freed, then the fill asks for it and arms it; so one buffer is in the hardware and the other is armed or being mixed. At the start the first buffer goes in two halves, so the second waits armed from the first word. The FIQ reads only the older piece's status (a read acknowledges); a queued piece found done too is counted (`dry`, a gap); a queued piece done before the older one (an engine the probe should have refused) is taken and counted (`odd`), so a pending request can never hold the FIQ up.

**Guarded in use** (review of the first cut). The probe polls the completion bits with the source masked, so it cannot see whether channel 1's completion raises source 26: "all channels share source 26" is [rockbox], not [silicon]. A channel that ended without its FIQ would leave the FIFO dry at every second hand-over. So the queue judges itself while it plays: a second `dry` hand-over since the start, or any `odd`, gives it up. No more pieces are queued; once the armed ones end, channel 0 carries on alone (`dma_tx_start()`, the mixer kept one buffer ahead), for the rest of the boot, counted (`fell`) and logged loudly (`pcm queue: FAILED in use`). The boot proof is the queue's first real use: it must show one FIQ a piece, at most the start's one dry hand-over and no fall, or the queue is refused and the proof runs again on one channel. A probe with no verdict at boot (no progress) refuses the queue for that boot, so no later start can turn it on unproved. A stop (`pq_stop()`) clears both channels at once and cuts up to one armed piece as well as the playing one (≤ 1.3 ms at 96 kHz 24-bit): at a pause or a track's end that is silence or the fade's tail, as the one-channel stop cuts the playing piece.

**The probe** (`pq_probe()`, first PCM start of each boot, the PCM lock held so no FIQ runs; ~3 ms of zeros, silent): 128 bytes of zeros on each channel, armed back to back, in both orders; the status of both channels and the FIFO's free count sampled until both raise their completion bit. **Proved** when the waiting channel never moved while the first was busy (one sample allowed: the hand-over's read race), the FIFO never ran dry between them, and both completion bits came. Anything else: one channel, as before, logged. No progress in 20 ms (codec clocks off): again at the next start, three runs at most. If the boot proof (`apps/audio_cop.c`) then fails with the queue on, the queue is refused and the proof runs again on one channel. Host model: the real block run against a serial engine, a 16-word FIFO and the mixer's index logic, with every word stamped: clean at 96 kHz 24-bit with every FIQ 200 words (1 ms) late; red arms (a FIQ later than a piece; an engine serving both channels at once; the mix and the fill swapped) fail as they must; faults it must survive (channel 1 ending without a FIQ, at 96 and 44.1 kHz; an engine serving the newest piece first; a stop mid-play) fall back cleanly, each red before the guard.

**The counters** (`codec` log, one `fiq2` line a run, next to `fiq e=`): `q=` queue on; `late=` the latest FIQ past its piece's end in µs (one channel: from the piece's start, length and the FIFO's free words at arming; the queue: from the next piece's progress) and how many came later than the FIFO lasts; `pc=`/`psr=` the interrupted pc and cpsr of the latest, `opc=`/`opsr=` of the last over the FIFO (taken in `fiq_by_core` from `lr_fiq`/`spsr_fiq`; symbolise with `arm-none-eabi-addr2line -e rockbox.elf`: a pc just after an interrupt unmask names the section that held the FIQ off; a pc in a busy loop names a stall; `psr` is the interrupted mode, 0x1f system, 0x13 supervisor, 0x12 IRQ, with bit 7 set when IRQs were masked there; its bit 6, the FIQ mask, is always clear, since the FIQ was taken); `rst=` entry to the next piece armed; `hnd=` entry to exit, the mix included; `lk=` the longest PCM lock while playing; `dry=`, `odd=`, `fell=` (the queue given up in this run), `slack=` (the least time left on the playing piece as a FIQ ended). And at boot, `pcm queue: proved|refused|inconclusive` with each order's overlap, gap and completion bits and the waiting channel's first status word.

**What the owner's device run must show:** `pcm queue: proved` at boot; on the same 24/192 track at 96 kHz 24-bit, `fiq e=0` with `fiq2 q=1 dry=0 odd=0` and `slack=` above zero; `late=` and `pc=` then name what delays the FIQ (candidates above). If the probe says `refused`, its `o=`/`g=`/`st0=` fields record how the engine arbitrates, and the one-channel path stays, with `late=`/`pc=` for a targeted fix.

## 3. Memory-to-memory and the BCM host port (channel 3)

Rockbox upstream uses only channel 0. Our project drives the BCM2722's host port with channel 3, memory → peripheral, without a request line. The register values, block sizes, throughput and failure modes our sessions measured are in §4; the host port itself is in `display.md`.

## 4. What our sessions learned

### The channel 3 recipe [ours] `bcm-devtest-hal.c`

```
DMA_MASTER_CONTROL |= 1<<31
DMA3_PER_ADDR = 0x3000_0000                 ; BCM_DATA (fixed)
DMA3_FLAGS    = 0x0400_0000                 ; bit 26, as for audio
DMA3_INCR     = 0x2001_0000                 ; fixed address, 32-bit
DMA3_RAM_ADDR = source | 0x1000_0000        ; after a cache flush
DMA3_CMD      = 0x8C00_0000 | (bytes - 4)   ; START | RAM_TO_PER | SINGLE, no request line
poll DMA3_STATUS bit 31; read DMA3_STATUS once more to clear
```

Before it starts, the host port's write address is set and the port's write-ready bit (`CONTROL & 0x02`) awaited (`display.md`). Pieces ≤ 32 KB; a 100 ms timeout stops the channel (`CMD = 0`). Blocks under 256 bytes go by CPU.

### Results [silicon]

| Session | Target inside the BCM | Result |
|---|---|---|
| S8 | BCM on-chip SRAM, BCM at 27 MHz, full 320×240 frame (153,600 B) | read back "clean" (0 of 48 checks bad), **but this proves nothing about delivery**: the CPU methods had written the same frame to the same place just before, so a DMA that wrote nothing reads back the same [ours, found in review]. 27,601 µs at PP 30 MHz, 10,375 µs at 80 MHz; the CPU's `ldm/stm` copy took 26,957 / 9,629 µs |
| S32 | BCM SDRAM window (`0xC000_0000` BCM-side), BCM at 150 MHz | **corrupt**: most words read back wrong; blocks lost their first word and later words came shifted |
| S33 | same | word 0 of **every** block never landed; CPU copy of the same data: 0 wrong |
| S34 | same, 256 B to 32 KB, each verified | without a CPU rewrite of word 0: every word wrong from word 0; with it: everything wrong except word 0. 32 KB took 2,469–2,680 µs (12–13 MB/s) |
| S35 | SDRAM FIFO **and** SRAM (`0xE0000 + 0x8000·k`), BCM at 150 MHz, 256 B to 32 KB, unique pattern, every word read back | **nothing landed in either**: every word wrong at every size (`session 35 bcmtest.log`, "dma lab: SRAM …"). 32 KB took 2,504–2,757 µs |

Conclusions so far:

1. **The engine is not faster than the CPU** at moving data into the port: the cost is about 10 PP cycles per 16-bit transfer either way (S8). Its value is freeing the CPU.
2. **No session has shown PP DMA delivering a byte through the port.** S8 reported SRAM clean, but that readback was invalid (above); S35 ran the lab into SRAM and SDRAM with a unique pattern and nothing landed in either, so the target memory is not the cause. The engine itself runs. Next: the second engine the way Apple drives it (§6), with memory → memory and width 16 as controls (`open-questions.md` #11, #12, #30; `../data-paths.md` §7.1).
3. After `BUSY` clears, the engine has read its last word but the port may not have accepted it: wait for the port's write-ready bit before changing the address [ours] (added after S32).
4. Our player therefore uses the CPU copy; DMA remains an experiment.

Apple's firmware feeds the port with PP DMA for transfers of 256 bytes and up, in 64 KB chunks, finishing the tail by halfword writes [apple] (osos 1.3 around `0x1028_7BE8`). **It uses the second engine, not this one**, and its driver for this engine refuses a peripheral address outside `0x7xxx_xxxx`, so the port at `0x3000_0000` cannot be a target for it in Apple's design [apple] (§6.6).

## 5. Rules

1. Program physical addresses. Logical SDRAM `x` is physical `0x1000_0000 | x` after Rockbox's remap. [rockbox]
2. Flush the cache before memory → peripheral; clean+invalidate before reading what a peripheral → memory transfer wrote. Whole-cache operations only (`cores-and-threads.md` §3). [rockbox]
3. Keep buffers 16-byte aligned (one cache line) when the CPU may touch neighbouring data during the transfer. [rockbox] (ATA rule; prudent for all)
4. Read `DMAn_STATUS` only in the completion handler: an outside read clears the pending interrupt and the handler never runs. [rockbox] `pcm-pp.c` comment
5. The engine is shared by both cores; Rockbox drives it from the CPU only. Driving it from the COP is unproven: Apple's transfer object refuses to, reason unknown [hypothesis]; a P19 bench row (engine 2 from the COP) checks it before 19-9 relies on it.

## 6. The second DMA engine

Everything in this section except the siggifly observations comes from our static reading of Apple's `osos` 1.3 (`osos.code` disassembled at base `0x1000_0000`, 2026-10-15). Apple's code says what Apple **writes**; the meanings are ours and tagged separately. **Driven on silicon in S37 (2026-10-15, E2 lab, `apps/ipodui/devbcm.c` + `bcm-devtest-hal.c`)** [silicon; measured]: Apple's sequence below works as read. Command `0xCC00_0000 | (bytes − 4)` reads back as `0x4C00_…` when the piece ends (bit 31 falls), and the status word counts the remaining bytes down to 0. Pattern-verified with guard words: SDRAM → SDRAM, SDRAM → BCM SRAM (BCM held after its bootstrap, and with our image idle) and SDRAM → BCM SDRAM, every word correct at 256 B–64 KB. 64 KB takes 5.5–5.8 ms (about 11–12 MB/s; the CPU's copy is 4.1 ms at 80 MHz). The CPU keeps 97 % of its time while a piece runs if it works from IRAM, 77 % if it touches SDRAM; I2S playback is unaffected. The register block at `0x6000_9000–7F` repeats at `0x6000_9080–FF`. Still open: the meanings of field [26:24] and bit 27, and IRQ 27 (not used: completion was polled). Results: `session log, session 37b bcmtest-results.log`; summary in `../data-paths.md` cell 14.

### 6.1 Where it is

| Address | What | Evidence |
|---|---|---|
| `0x6000_8000` | Master control. Bit 31 enables the engine | [apple] osos `0x101D_9834`: Apple's "enable DMA" method sets or clears bit 31 here and at `0x6000_A000` together, in one call |
| `0x6000_8004`, `0x6000_8008` | Not touched by Apple's driver. By analogy with the general engine, master status and request status | [hypothesis] |
| `0x6000_9000 + 0x20·n` | Channel `n`. Apple's driver builds **2** channel objects (`n` = 0, 1) | [apple] osos `0x101D_A1B8`–`0x101D_A21C`: channel register block = engine base + `0x1000` + `0x20·n` |
| `0x6000_9040`, `0x6000_9060` | Channel slots 2 and 3. Apple's shutdown routine stops four channels here (and eight on the general engine, at `0x6000_B000`–`0x6000_B0E0`), so slots 2–3 probably exist even though the driver never uses them | [apple] osos `0x1000_1244`–`0x1000_1284`; [hypothesis] for "exist" |
| (aliasing) | The channel page reads mostly `0x0000_0001` and repeats every `0x80` on hardware, which fits four `0x20` slots | [external] siggi `08`; the "four slots" reading is [hypothesis] |

The same channel layout sits at the same offset from each engine's master: the general engine is `0x6000_A000` with channels at `+0x1000`, and Apple's code reaches both through one routine with a different base [apple] osos `0x101D_A15C`.

### 6.2 Channel registers

Offsets are from the channel base. The **names** below are ours.

| Offset | Name (ours) | Apple's use | Evidence |
|---|---|---|---|
| `+0x00` | `E2_CMD` | Command word; bit layout below. Apple always read-modify-writes it | [apple] |
| `+0x04` | `E2_STATUS` | Read before a start (twice if non-zero: the first read is taken to clear a stale event) and in the completion handler, where **bit 30** marks a finished channel. No other bit is tested | [apple] osos `0x1028_E37C`, `0x101D_9BF0`; "a read acknowledges" is [hypothesis] by analogy with `DMAn_STATUS` |
| `+0x10` | `E2_SRC` | **Source** address, always | [apple] osos `0x1028_DFCC` with `0x1028_DED0` |
| `+0x14` | `E2_SRC_CFG`? | Field `[26:24]` set to 2 at channel init; the driver's per-side "size code" option writes this field with 2/3/4 for an argument of 1/2/4 | [apple] osos `0x1028_E2DC`, `0x1028_DE58`–`0x1028_DE90`; meaning [unknown] |
| `+0x18` | `E2_DST` | **Destination** address, always | [apple] |
| `+0x1C` | `E2_DST_CFG`? | Field `[26:24]` set to 2 at channel init; never changed afterwards | [apple] osos `0x1028_E2F0`; meaning [unknown] |

**This is the main difference from the general engine.** There, `+0x10` is the RAM address and `+0x18` the peripheral address whatever the direction, and `CMD` bit 27 picks the direction. On the second engine Apple writes the **source to `+0x10` and the destination to `+0x18` in both directions, and sets bit 27 in both** [apple] osos `0x1028_DF24`–`0x1028_DF5C` and `0x1028_DF88`–`0x1028_DFB8`. So bit 27 is not a direction bit here [hypothesis: an "address pair" or "memory-to-device mode" enable].

`E2_CMD`:

| Bits | Apple's use | Meaning (ours) |
|---|---|---|
| `[15:0]` | byte count − 4 | as on the general engine [apple] |
| `[19:16]` | request line, written with a value 0–15 taken straight from the caller (no translation table, unlike the general engine's driver) | request ID [apple], [hypothesis] for meaning |
| `[23:20]` | an optional setting writes `(arg + 8) & 0xF` here and also sets bit 25 | [unknown]; bit 23 is always set when used |
| 24 | set together with a request line | wait for the request (as `WAIT_REQ`) [apple], [hypothesis] |
| 25 | set only with `[23:20]`; Apple re-applies it at every start when that option is active | [unknown] |
| 26 | set at channel init and never cleared | stop at the end (as `SINGLE`) [hypothesis] |
| 27 | set by every transfer setup, both directions | [unknown]; see above |
| 28 | cleared at init, never set | [unknown] |
| 30 | set at every start; cleared on stop | interrupt on completion [apple], [hypothesis] |
| 31 | set last, to start; cleared to stop | start / running [apple], [hypothesis] |

`E2_SRC_CFG`/`E2_DST_CFG` `[26:24]`: the driver's general-engine twin uses the same field with the same codes (2, 3, 4, 5 for 1, 2, 4, 8) on its peripheral side, next to separate width (`[30:28]`) and range (`[18:16]`) fields [apple] osos `0x1028_E618`–`0x1028_E684`. The second engine's driver never touches `[30:28]` or `[18:16]`. Whether that is because the engine lacks them or because their reset values suit Apple is [unknown]. **What the field controls (bus width, burst length) is [unknown]**; the lab sweeps it (`../data-paths.md` §7.1, step E2-5).

### 6.3 Apple's programming sequence for a host-port write

From osos `0x1028_7BE8` (port write), `0x1028_6FB4` (bulk mover), `0x1015_99FC` (piece submit), `0x1012_52AC` (channel open) and the engine-2 channel methods at `0x1028_DD5C`–`0x1028_E4C4` [apple]. Restated as steps:

1. **Once at start-up:** master `0x6000_8000` bit 31 set. Each channel is initialised by read-modify-write: in `E2_CMD` clear bits 31, 24, 25, 28, 30, `[23:20]`, `[19:16]`, set bit 26; set `E2_SRC_CFG[26:24]` = 2 and `E2_DST_CFG[26:24]` = 2. Two channels are allocated to the port driver, one per host-port bank (main bank `0x3000_0000`, ALT bank `0x3004_0000`).
2. **Per write call** (BCM address, source, length, bank): take the bank's mutex for the whole call. Write the BCM address to `WR_ADDR` as **two 16-bit stores, low half first**. Apple does **not** poll the port's write-ready bit on the write path.
3. If the source is halfword- but not word-aligned, write one halfword by CPU first.
4. **Bulk part = length rounded down to a multiple of 256** goes by DMA; the rest by CPU halfword stores after the DMA finishes.
5. **Cache:** before the bulk part, a whole-cache write-back (`CACHE_OPERATION` `0xF000_F044` bit 1, then wait for `0x6000_C000` bit 15 to clear). The range arguments are ignored [apple] osos `0x1009_A9EC`.
6. The transfer object refuses to run on the COP (it checks `0x6000_0000` reads `0x55`) [apple] osos `0x1015_9CFC`.
7. A source on an odd byte address is copied into one of four 64 KB bounce buffers first [apple] osos `0x1015_9A80`.
8. **For each 64 KB piece** (the last may be shorter): set the address pair (source = SDRAM address of the piece, destination = `0x3000_0000`), `E2_CMD[15:0]` = piece − 4, OR in bit 27. Apple then asks the driver for a 4-byte size code on the source side; for an SDRAM source the driver rejects that request (it only sets the code for the non-SDRAM side), so both `[26:24]` fields keep the init value 2 [apple] osos `0x1015_9B0C`, `0x1028_DE34`. Then check `E2_STATUS`, reading it again if non-zero; OR in bit 30; OR in bit 31. Then **sleep on a semaphore** until the completion path posts it, and only then start the next piece. **The BCM address is not re-sent between pieces**: the port's auto-increment carries on.
9. After the last piece, the CPU writes the tail and releases the mutex.

For a full 64 KB piece the final command word is `0xCC00_FFFC` (bits 31, 30, 27, 26 and the count) [apple, derived], assuming bit 29, which Apple never writes, resets to 0 [hypothesis]; E2-0 in the lab records it. The general-engine recipe our sessions used was `0x8C00_xxxx` on channel 3 (§4); the second engine's word differs only by the interrupt bit.

Two details matter for the destination address:
- **No fixed-address setting is written.** On the general engine "fixed" is `INCR[18:16]` = 1; the second engine's driver never writes `[18:16]`. Whether its destination stays at `0x3000_0000` or counts up is [unknown]. Either way the transfer lands in the `DATA` register: only PP address bits 16–18 select a port register (`display.md`, `../data-paths.md` R1), so `0x3000_0000`–`0x3000_FFFF` is all `DATA`. **A 64 KB piece is exactly the largest that cannot leave that window** [hypothesis: the reason for Apple's piece size; the 16-bit count field allows the same]. siggifly describe the peripheral address as fixed [external] siggi `08`, `10`; that reading came from an emulator model.
- **No request line is set** for the port. The transfer runs free and is paced only by the port's own bus wait states [apple]; whether a request line for the port exists is [unknown] (`open-questions.md` #13).

What Apple's driver accepts as an address pair [apple] osos `0x101D_9A30`, `0x1028_DED0`: one side must be SDRAM (`addr & 0xF400_0000` = `0x1000_0000`, `0x1400_0000`, `0x8000_0000` or `0xB000_0000`); the other may be SDRAM, IRAM (`0x4xxx_xxxx`) or "anything else" except PP peripherals (`0x7xxx_xxxx`) and `0xAxxx_xxxx`. The host port at `0x3000_0000` falls in "anything else". So in Apple's model the second engine moves memory to and from **external** devices and memory; **SDRAM → SDRAM is permitted** by the driver's checks.

### 6.4 Completion and interrupts

- Apple registers one handler per engine with its interrupt service under the numbers **27** (the handler walks the general engine's active channels) and **28** (the second engine's) [apple] osos `0x101D_98F4`–`0x101D_9934`, `0x1026_80C0`/`C4`. Rockbox, whose audio FIQ works every day, puts the general engine on source **26**. If Apple's numbers are the hardware source plus one, the second engine is **source 27**, which also matches siggi `10` [hypothesis: two independent anchors agree under the +1 offset; the offset itself is not read from code].
- The second engine's handler reads each active channel's `E2_STATUS`, and for those with bit 30 set runs the channel's completion step: either restart the channel with a queued address pair (write `+0x10`, `+0x18`, then `E2_CMD` with the start bits) or count a completion. It then raises a software interrupt (Apple's number 13; the general engine's uses 38) through the forced-interrupt register to finish the work outside the handler, where the waiting thread's semaphore is posted [apple] osos `0x101D_9BE0`, `0x101D_9E14`, `0x101D_A058`; [external] siggi `10` for source 13.
- To stop a channel Apple masks IRQ and FIQ, clears bit 30, then bit 31 [apple] osos `0x1028_E1E0`.

### 6.5 Compared with the general engine

| | General engine (§1) | Second engine |
|---|---|---|
| Master | `0x6000_A000` bit 31 | `0x6000_8000` bit 31 (Apple sets both in one call) |
| Channels | 4 in Rockbox and in Apple's driver; Apple's shutdown stops 8 slots | 2 in Apple's driver; 4 slots stopped at shutdown |
| `+0x00` command | size, request line `[19:16]`, bits 24/26/27/28/30/31 as §1 | same positions for size, request, 24, 26, 30, 31; **bit 27 set in both directions**; extra field `[23:20]` with bit 25 |
| `+0x04` status | bit 31 busy, bit 30 interrupt, `[15:0]` remaining | bit 30 used by Apple; others [unknown] |
| `+0x10` / `+0x18` | RAM address / peripheral address | **source / destination** |
| `+0x14` | `FLAGS`; Apple's default `0x2400_0000` (`[26:24]` = 4, `[30:28]` = 2) | only `[26:24]` written (= 2) |
| `+0x1C` | `INCR`; Apple's default `0x2201_0000` (fixed address, `[26:24]` = 2, 32-bit) | only `[26:24]` written (= 2) |
| Peripheral side Apple allows | **only `0x7xxx_xxxx`** (PP peripherals) | anything except `0x7xxx_xxxx` and `0xAxxx_xxxx`; one side SDRAM |
| Request IDs | translated through a table (up to 30 entries) | written raw, 0–15 |
| Interrupt (Apple's number) | 27 → source 26 (Rockbox) | 28 → source 27 [hypothesis] |

Evidence for the general-engine column's Apple values: osos `0x1028_EE28`–`0x1028_EF24` (init), `0x1028_E914` (address pair: the peripheral side must classify as `0x7xxx_xxxx` or the setup fails), `0x1028_EB4C` (request table) [apple].

### 6.6 What this means for the host port

- Apple never points the general engine at the port; its own driver would refuse to [apple] (§6.5). Our channel-3 experiments (§4) therefore used an engine in a way Apple's code never does. That supports reason (b) in `../data-paths.md` D1 but does not prove it: a driver's policy is not a hardware limit [hypothesis].
- Our channel-3 values also differ from Apple's defaults for that engine (`FLAGS` `0x0400_0000` against `0x2400_0000`, `INCR` `0x2001_0000` against `0x2201_0000`). Rockbox's values work for I2S, so the difference may not matter [hypothesis].
- To reproduce Apple exactly, the lab uses the second engine with the values in §6.3 and the same order: address by two halfword stores, cache written back, piece ≤ 64 KB, multiple of 256 bytes, interrupt bit set (masked at the interrupt controller if no handler is installed), start last.
