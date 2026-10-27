# 3. Memory access: every memory, every path

How each kind of memory on the BCM2722 is reached, by whom, at what width and speed, and what goes wrong. This page is a map: it states each rule once with its evidence and links to the page that holds the detail. Addresses are core addresses unless marked "host" (the PP5022's port addresses, `host-interface.md`).

> **See also [`../data-paths.md`](../data-paths.md)**: the board-wide map of masters × targets (both chips and the WM8758), with status, rates, recipes and dead ends. Read it before any DMA, memory, cache or cross-chip work.

## 1. The memories at a glance

| Memory | Core address | Host address | Size | Speed (measured) | Evidence |
|---|---|---|---|---|---|
| Internal SRAM | `0x0000_0000–0x0013_FFFF` | the same, through the port | 1.25 MB | scalar and vector kernels run at full core speed from it (§6) | [apple] heap table; [external] brief; [silicon] S1 onward |
| SDRAM (stacked) | `0xFE00_0000–0xFE3F_FFFF` | `0xC000_0000–0xC03F_FFFF` | 4 MB | not measured from the core | [silicon] S10, S11; `sdram.md` |
| Caches | — (no address) | — | 128 Kbit instruction + 128 Kbit data | not measured | [external] brief; control at `0x1000_0C00` [apple] |
| Vector register file | — (not memory-mapped) | — | 64 × 64 bytes | one 32-byte row a cycle (§6) | [apple]; [silicon] S7; `isa-vector.md` |
| Peripherals | `0x1000_0000–0x1000_FFFF` | the same, through the port | 64 KB, blocks of `0x400` | — | [apple]; [silicon]; `memory-map.md` |

Everything else in the 32-bit space is unknown (`memory-map.md`).

## 2. Internal SRAM

### Hardware

- **Range** `0x0000_0000–0x0013_FFFF` [apple; external; silicon]. What the core or the host sees at `0x0014_0000` and above (an alias of SRAM, a bus error, or open bus) is **unknown**: no session has read there, and none of the sources in `sources.md` (FS#9787 included) reports a wrap. Do not rely on either behaviour (`open-questions.md` #42).
- **Images load at 0, enter at `0x200`** [silicon S1]. The host's upload streams through the port into SRAM before the core starts.
- **Cache state at entry:** off; the host flushed it and wrote `0x1000_0C00 = 0` [rockbox]. With the cache off, host writes are visible to the core at once, and core writes to the host [silicon: every image].
- Low SRAM is architecturally special only in that the exception vectors (`0x000–0x017`) and interrupt vectors (`0x080–0x0FF`) live there [apple]. `0x100–0x1FF` is a software mailbox by convention (§2, table below) [rockbox; ours].

### Our image's SRAM map [ours]

From `bcmfw/config.h`, `bcmfw/hostproto.h` and `bcmfw/video/video.h` (video build, dev41). A different image can lay SRAM out differently; only the vectors, the entry at `0x200` and Rockbox's `0x1F8`/`0xE0000` contract are fixed by others.

| Range | Size | Use | Writer |
|---|---|---|---|
| `0x000000–0x0000FF` | 256 B | Exception and interrupt vectors | image |
| `0x000100–0x00013F` | 64 B | Video-protocol header (`HP_*`: version, ring addresses, indices, `STATE`, `FIFO_RD`) | one writer per word (§7) |
| `0x000140–0x0001DF` | 160 B | Diagnostic mailbox words: limits, clocks, DMA push record, step, progress (`0x1C4` phase, `0x1C8` packets) | image |
| `0x0001E0–0x0001FF` | 32 B | Fault pc/sp, info, frame count, `UPDATERECT` rectangle, `0x1F8` command/status word (Rockbox's `BCMA_COMMAND`), `0x1FC` | host writes commands; image writes status |
| `0x000200–0x01FFFF` | 127.5 KB | Image: code, data, `.bss` (a 96,008-byte image in S34). `build.py` refuses an image whose `.bss` passes `0x20000` | upload, then image |
| `0x020000–0x0CBFFF` | 688 KB | Decoder arena (`BCMFW_VIDEO_ARENA`): context, macroblock info and, at 320×240, the reference frames (S34's planes at `0x29FC0`, `0x3CBC0`, `0x416C0`) | image |
| `0x0CC000–0x0CFFFF` | 16 KB | Stack, top `0xD0000`, grows down | image |
| `0x0D0000–0x0D1FFF` | 8 KB | Command ring, 2,048 words (host → chip) | host |
| `0x0D2000–0x0D2FFF` | 4 KB | Event ring, 1,024 words (chip → host) | image |
| `0x0D3000–0x0D3FFF` | 4 KB | Reserved (planned DMA blocks; today's control blocks are in `.bss`) | — |
| `0x0D4000–0x0DBFFF` | 32 KB | The skeleton image's SRAM FIFO (non-video builds only) | host |
| `0x0DC000–0x0DDDFF` | 7.5 KB | The scaler's strip | image |
| `0x0DDE00–0x0DEFFF` | 4.5 KB | Self-test area | image |
| `0x0DF000–0x0DF0DF` | 224 B | **Guard block** (`VIDEO_GUARD`, 56 words): header check, first hit, refusal or first decode error | image; host reads |
| `0x0E0000–0x1057FF` | 150 KB | **Host frame** (Rockbox's `BCMA_CMDPARAM`): 320×240 RGB565, 640-byte rows | host (§7) |
| `0x105800–0x12AFFF` | 150 KB | `VIDEO_FB`: the converted picture the LCD DMA reads | image |
| `0x12C000–0x1349CF` | 35 KB | Stage 8a-1 tone buffer, only while `HP_OP_TONE` runs | image |
| `0x1349D0–0x13FFFF` | 46 KB | Free | — |

## 3. SDRAM

### Hardware

| | | Evidence |
|---|---|---|
| Size and decode | 4 MB; core address bit 22 is not decoded, so `0xFE40_0000` reads `0xFE00_0000`. Higher aliases, and aliasing of the host's `0xC040_0000`, are untested | [silicon] S11 |
| Two views | The core addresses SDRAM at `0xFE00_0000`, the host port at `0xC000_0000`; the same bytes (64 host-written words read back by the core, then 256 words over 4 MB, 0 wrong). Whether the core can use `0xC000_0000`, or the host `0xFE00_0000`, is untested | [apple] pool table; [rockbox] `BCMA_SDRAM_BASE`; [silicon] S10, S11 |
| Host writes and the commands after them | A host write into SDRAM may not be visible to the core when a command written to SRAM afterwards reaches it. Symptom [silicon, sitting 4b, 2026-10-26]: a `FEED` sent the moment its packet was written gave no picture at any clock band; packets written well ahead play. Reproduced in vc2sim by making SDRAM writes visible only behind a host read of SDRAM or 2 ms later [sim]. Mechanism: posted writes [hypothesis]; the other candidate is the image's first-`OPEN` read of the FIFO's first 8 KB (`quirks.md` #47, `open-questions.md` #73). Practice: write ahead, and read the newest packet's last word back before a close `FEED` (`host-driver.md` §4) | [silicon] symptom; [sim]; [hypothesis] |
| After a host start | Not initialised: nothing may touch it, from either side, until an image has run the bring-up | [hypothesis] (nothing configures it); every design so far waits for it |
| Bring-up | Refresh word, timing words, cache flushed and off, `+0x30 = 0x80E8`, a power-up wait [ours], command 7 with the mode word, a geometry probe at `0xFE00_0000`/`0xFE00_0200`; on failure reset the controller and retry with the other mode field (8 ↔ 4); then `+0x30 = 0x64` and cache-control bit 16. Full sequence: `sdram.md` | [apple]; [silicon] S10 |
| Failure modes | A command-7 poll timeout is **not** fatal (let the probe decide). Field 8 fails on our unit; field 4 passes on attempt 2 in every session, at 150 MHz (`OPEN` word `0x5344_0233`) and, in the 27 MHz rows of S32–S34, at 27 MHz too (`0x5344_0231`), so field 8 fails at both clocks. S9 failed by treating the timeout as fatal, using field 7, skipping the reset and clearing the cache before the flush (`quirks.md` #22) | [silicon] S9, S10, S28–S34 |
| Refresh | `+0x24[31:16]` = 7.8 µs in **core cycles**: `0x0493` at 150 MHz, `0x00D2` at 27 MHz. Raise it after a clock switch up, lower it before a switch down, so it is never too slow for the clock running | [apple] value; [ours] the 27 MHz value and the order |
| Correctness | Scalar 64 KB grid over all 4 MB: 0 wrong. Vector row load from and store to SDRAM: right. Host port writes while the core decodes: 0 wrong (§5). Byte and halfword stores into SDRAM, read back after all the stores (`HP_AAC_MEMPROBE`: `stb` at +1 and +6, `sth` at +8 and +14): right at 150 and 27 MHz, and the host reads the same words (S36c, `open-questions.md` #45). A store followed **at once** by a load of the same word is not yet checked: the next build's `HP_AAC_MEMSTRESS` (#46) | [silicon] S11, S12, S36c; store-then-load [open] |
| Latency, bandwidth | **Not measured** from the core, the vector unit or the DMA engine, cache on or off | `open-questions.md` #7 |
| Self-refresh, power-down | Unknown (Apple's shutdown writes `+0x30 = 1`) | [apple]; #7 |

### Our image's SDRAM map [ours]

Offsets from the base: add `0xFE00_0000` on the core, `0xC000_0000` on the host (`bcmfw/video/video.h`).

| Offset | Size | Use | Writer |
|---|---|---|---|
| `0x000000–0x0FFFFF` | 1 MB | Bitstream FIFO (packets 64-byte aligned; `host-driver.md` §4). The bring-up probe writes words 0 and `0x200` at `OPEN` | host |
| `0x100000–0x2FBFFF` | 2 MB − 16 KB | DPB: decoded frames when they don't fit the SRAM arena (larger than 320×240, or the diagnostic `HP_OPEN_SDRAM_DPB`). The display path reads up to 16 rows past a plane, hence the 16 KB margin | image |
| `0x300000–0x33FFFF` | 256 KB | Wrap copy: a packet that crosses the FIFO's end, copied linear before decoding (so a wrapped packet must be ≤ 256 KB) | image |
| `0x340000–0x37FFFF` | 256 KB | The audio window (`VIDEO_AUDIO_*`). Stage 8b's two PCM rings (`HP_OP_AUDIO`; the image refuses rings outside this window). Stage 9 (dev42) rotates each AU and its PCM through 32 slots of 8 KB here: the AU at +0, 4 KB of PCM at +4 KB | host writes one ring (or the AU), image the other (or the PCM) |
| `0x380000–0x39BFFF` | 112 KB | The AAC decoder's far data (`BCMFW_AAC_FAR`): its tables (`.xdata`, 48 KB in the dev42 image, parked by `crt0` at the top of the decoder arena and copied here at the boot's first H.264 `OPEN`), then its channel elements (`.sdbss`, about 40 KB; written by byte stores, see "Correctness" above) | image |
| `0x39C000–0x39FFFF` | 16 KB | AAC test I/O (`VIDEO_AAC_IO`): an AudioSpecificConfig at `+0`; `0x39E000–0x39EFFF` is `HP_AAC_MEMSTRESS`'s window; the last 16 bytes (`0x39FFF0`) are `HP_AAC_MEMPROBE`'s scratch | host, image |
| `0x3A0000–0x3CFFFF` | 192 KB | Dump: the first picture's Y, U, V (`HP_OPEN_DUMP`) | image |
| `0x3D0000–0x3D57FF` | 22 KB | Dump: the VRF after each self-test kernel (11 × 2 KB) | image |
| `0x3E0000–0x3F5FFF` | 88 KB | Dump: each failing self-test's areas (11 × 8 KB) | image |
| `0x3F6000–0x3FFFFF` | 40 KB | Free | — |

The diagnostic memory test (`HP_OPEN_SDRAM_DPB`) writes across all 4 MB to find the size; it destroys whatever the host had written.

## 4. Caches

| | | Evidence |
|---|---|---|
| Size | 128 Kbit instruction and 128 Kbit data | [external] brief |
| Geometry | Line size, associativity, write policy: unknown. A loop-closing branch in the last word of a 16-byte line runs at half speed, which hints at a 16- or 32-byte fetch line (`quirks.md` #6) | [silicon] S6; geometry [hypothesis] |
| Control `0x1000_0C00` | Bit 31 enable (I and D both [hypothesis]); bit 30 flush request (write back and invalidate); bit 0 flush done (read); bit 16 set by Apple once SDRAM is up (meaning unknown) | [apple]; [rockbox]; `boot-and-clocks.md` §2 |
| Flush | Write the old value with bit 30 set, poll until bit 0 reads 1, write the wanted value with bit 30 clear. The host can do it through the port (Rockbox does at every boot) | [apple]; [rockbox]; [silicon] every boot |
| Our state | Off for the whole session; after SDRAM bring-up `0x1000_0C00 = 0x0001_0000` (bit 16 only). Apple ends at `0x8001_0000` (cache on) | [ours]; [apple] |
| Coherence with the host | With the cache off, host writes into SRAM and SDRAM are seen at once [silicon S10, S12]. With it on, the core must flush before reading anything the host wrote, and before the host reads anything the core wrote [hypothesis for the second half: write-back is assumed] | [silicon]; [hypothesis] |
| Coherence with the vector unit | Whether `vld`/`vst` go through the data cache is unknown | `open-questions.md` #6 |
| Coherence with DMA | Unknown. Transfer-information bit 27 separates Apple's cached and `_uncached` memcpy paths, so the engine probably does not snoop [apple; hypothesis]. Flush before DMA reads core-written data if the cache is on | `dma.md` |

Rule [ours, proven on every session]: run with the cache off and nothing here can bite. Turning it on is an untested speed lever that brings every coherence question above with it.

## 5. Peripheral space and the low mailbox

- **`0x1000_0000–0x1000_FFFF`**, blocks of `0x400` (`memory-map.md` has every known block). **32-bit accesses only**; Apple's code never uses another width [apple]. Some registers need `0xA5A5` in bits `[31:16]` ("pw").
- Blocks our images touch [ours, each device-run]: run control `0x1000_0400` (the host's start write), DMA `0x1000_0800` (LCD ch 1; audio ch 5 in stage 8a-1), cache `0x1000_0C00`, interrupt controller `0x1000_1000` (idle-sleep builds), regulator `0x1000_1400`, system timer `0x1000_1800` (the 1 MHz counter at `+0x04` bounds every wait), clocks `0x1000_1C00`, GPIO `0x1000_2800`, SDRAM controller `0x1000_3400`, host doorbell acknowledge `0x1000_3800`, LCD `0x1000_4000`, audio `0x1000_4800` (stage 8a-1 only).
- **The host can read and write peripheral registers through the port while the core runs** [rockbox; silicon]: Rockbox reads `0x1000_1400`; our host reads GPIO, DMA and LCD registers and drives the cache flush and the start.
- **The mailbox (`0x100–0x1FF`)** is plain SRAM that both sides agree to use. Rockbox's protocol uses `0x1F8` (command, written `~x << 16 | x`) and `0x1FC` [rockbox]; Apple's disk `vmcs.bin` puts its host-interface block address at `0x1FC` and the ARM side expects `0x1F8 = 1` [apple; external]; ours is §2's table [ours]. Low SRAM is the place a stray vector store lands: a vector base of r7 reads as 0 and wrote over `0x000–0x1FF` (`quirks.md` #8), which is why our image keeps a guard block far away at `0xDF000`.

## 6. Access paths from the chip

### 6a. The scalar core

| Rule | Evidence |
|---|---|
| Widths: `ld`/`st` 32-bit, `ldh`/`sth` 16-bit, `ldb`/`stb` 8-bit, plus `ldsh`/`ldsb` | [apple]; [silicon] |
| Sign extension: `ldsb` sign-extends in every form checked. `ldsh` sign-extends only in its 16-bit `(rs)` form; the 32-bit `(rs + off)` form **zero-extends** (quirk #16). For a signed halfword at an offset, compute the address and use `ldsh rd, (rs)` | [silicon] S4, S17, S18 |
| Indexed form `(rs + ri)`: the index is scaled by the access width | [apple] |
| Pc-relative 48-bit forms count from the instruction's address rounded down to a word | [apple]; [silicon] S2 |
| **Misaligned access** completes from the address rounded down, then raises exception 1, imprecisely (2–4 later instructions run first); a pre-decrement writes its base back even when it faults. Misaligned stores and odd halfword accesses: not characterised (§11.1, bench ALGN). Keep every access naturally aligned | [silicon] S2, S3; quirks #2–#4; `open-questions.md` #5 |
| r7 as an ordinary scalar register is fine; the r7 traps are in the **vector** unit (6b) | [silicon] S15, S22 |
| sp (r25) stays 4-byte aligned | [apple] |
| Peripheral registers: 32-bit only | [apple] |
| Speed: memory kernels in SRAM scale exactly with the core clock (×5.56 from 27 to 150 MHz); an `add`/`ld` pair issues together (about 1.9 instructions a cycle) | [silicon] S6, S7 (cache off) |

### 6b. The vector unit

| Rule | Evidence |
|---|---|
| The VRF is 64 rows × 64 bytes, not memory-mapped; it moves to and from memory only through `vld`/`vst`/`vstr`. H and V views alias the same storage (`isa-vector.md`, "VRF layout") | [apple]; [silicon] |
| **Base or stride field 7 means "no register" (address 0 + offset)**, not r7; only r0–r6 can be a base or stride. A scalar B operand naming r7 also reads 0 | [silicon] S15, S22; quirks #8, #19 |
| **Plain `vld8`/`vld16` align their address down to the access size** (a `vld16` at `r2 + 8` reads from `r2`). A horizontal B operand's `+rN` aligns its x down to 16 bytes. Misaligned data needs the `vldp`/`vldq` pair (`vldq` exactly when the bytes cross a 16-byte line) | [silicon] S18–S22; quirk #17; [apple] |
| **A `vstr` writes only the lanes in the 16-byte line of its address**; lanes past the line are dropped. Store crossing data in two parts | [silicon] S22, S24; quirk #18 |
| The 48-bit `vst16` takes its offset (32-byte units) from the field a load uses for D, and its source row from the load's offset field | [silicon] S5; quirk #9 |
| `vst8`/`vst8v` layouts unchecked: store `vst8` only from row 0 at offset 0 | [silicon-derived rule]; `open-questions.md` #2 |
| Keep 16-bit row loads and stores 32-byte aligned (every silicon check used it) | [silicon] |
| SDRAM: a vector row loads from and stores to SDRAM correctly | [silicon] S11 |
| Speed in SRAM: `vld16` and `vst16` at REP 16 take 16.63 and 16.50 cycles, so about 31 bytes a cycle (≈ 4.6 GB/s at 150 MHz) | [silicon] S7; arithmetic [ours] |

### 6c. The chip's DMA engine (`0x1000_0800`)

Detail in `dma.md`; the memory-relevant rules:

| Rule | Evidence |
|---|---|
| 8 channels `0x80` apart, 3 sub-channels each; a sub-channel starts a chain of **32-byte, 32-byte-aligned control blocks** (next, source, destination, length, transfer information, 2-D skips) | [apple]; [silicon] S5 |
| Length is in units of the narrower of the source and destination widths (each 1–32 bytes, log2 in the transfer information); Apple picks each width as the largest power of two ≤ 32 dividing the length and both addresses | [apple]; [silicon] S5 |
| 2-D: length = rows in `[31:16]`, units per row in `[15:0]`; the skip word is the **gap** after a row (pitch − width), not the pitch | [apple]; [silicon] S5; quirk #21 |
| Completion: clear the status word (`+0x04`, write 1s) before arming; an LCD run ends on **bit 8** (status `0x2300` after a frame); a generic sub-channel k chain ends on bit 13 + k. Bits 0–7 are not completion bits | [silicon] S4, S5; quirk #20 |
| **LCD, channel 1**: source a 32-byte-aligned frame in SRAM (`VIDEO_FB` or the host frame), destination the LCD FIFO `0x1000_400C`, DREQ 3, 32-byte source bursts; 14.26 ms a 320×240 frame, set by the LCD bus | [silicon] S5–S7 |
| **Audio, channel 5**: destination the audio FIFO `0x1000_4804`, DREQ 7 (config `0x0607_6002`), transfer information `0x1000_A041`. Apple heads every chain with a **16-byte zeroed block whose next pointer is itself**, so an underrun loops on silence instead of stopping; our tone test (stage 8a-1, built but not yet run) loops one 35 KB SRAM buffer the same way. Length here appears to count bytes | [apple]; [ours]; length unit [hypothesis]; `audio.md` §3 |
| SDRAM as a DMA source or destination: Apple's TV and memcpy paths use it; **not exercised on silicon** | `dma.md` "Open" |

## 7. Access paths from the host (PP5022)

The port gives the host every BCM address: SRAM, SDRAM at `0xC000_0000` and the peripheral registers. Registers and handshakes are in `host-interface.md` §1; here are the access rules.

### 7a. The protocol

```
write:  WR_ADDR32 = addr;  wait CONTROL & 0x02;  then DATA (16 or 32 bits) for each value, auto-incrementing
read:   wait RD_ADDR & 0x01;  RD_ADDR32 = addr;  wait CONTROL & 0x10;  value = DATA32
```

| Rule | Evidence |
|---|---|
| **Two banks.** The main bank (`0x3000_0000–0x3003_FFFF` host) and the ALT bank (`0x3004_0000–0x3007_FFFF`). After the bootstrap our port and Rockbox use only the main bank; what the ALT bank addresses is unknown | [rockbox]; `open-questions.md` #25 |
| **The data bus is 16 bits.** A 32-bit host access (and `ldm`/`stm` bursts) to `DATA` arrives as two 16-bit transfers, low half first; a 32-bit address goes to `WR_ADDR`/`RD_ADDR` the same way | [rockbox]; [external] siggifly note 03 |
| Writes auto-increment: one address, then any number of halfwords or words. Uploads are an even number of halfwords. Whether a lone 16-bit write commits by itself is untested [hypothesis: words are assembled from two halves]. **A write starting at 2 mod 4 lands 2 bytes low**: the write address ignores bits 1:0 [silicon, inferred] CR1 (§11.4) | [rockbox]; [silicon] every upload; `open-questions.md` #43 |
| **Reads wait on `CONTROL` bit 4** (`0x10`) after the address is set; `RD_ADDR` bit 0 says the port takes a new read address. Our port sets the address for every word read; whether `DATA` reads auto-increment is untested | [rockbox]; [apple] `osos` read loop waits per word; #43 |
| Every wait is bounded in our port: 20 ms a handshake (`DT_PORT_US`), then the caller decides. Stock Rockbox waits for ever, and an unpowered port (reading `0xFFFF` per halfword) then hangs the host | [ours]; [rockbox]; `host-interface.md` §4 |
| Longest handshake wait under a decode load: 47 µs; 0 timeouts | [silicon] S12; quirk #24 |
| **What reaches what:** host addresses below `0x0014_0000` land in SRAM, `0xC000_0000–0xC03F_FFFF` in SDRAM (only after the chip's bring-up), `0x1000_xxxx` in the peripherals | [rockbox]; [silicon] S10, S11 |
| **Torn reads.** A 32-bit word the chip rewrites while the host reads it can mix halves. Anything the host polls is stored as a 16-bit value with its complement in the high half (`HP_ENC`) and rejected unless the halves agree (`HP_VALID`); a bad word means "not yet", never "dead" | [silicon] S12; quirk #23; [ours] |
| **Readback verification [ours].** The host client reads back the first and last word of each FIFO piece it writes (`bcmvp.c` `verify_word`) and logs words checked and wrong; before play it reads 64 words over the 1 MB FIFO | [ours]; results §8 |
| Packets are written as whole 32-bit words, so up to 3 bytes past a packet's end are overwritten; harmless because the next packet starts 64-byte aligned | [ours] |
| **The write address ignores bits 1:0.** A write asked at 2 mod 4 lands from the word's start, 2 bytes low. Start every write at a word address; merge an odd first or last halfword into its word by read-modify-write. Our slots' pitches are multiples of 32 bytes (`jobs.h` `J_SLOT_PITCH`), and the PP's port layer refuses a mid-word write | [silicon, inferred from CR1's 301×7 slot, 2026-10-25: rows at 2 mod 4 landed one pixel early, 906 bad pixels by CPU and engine 2; ALGN to confirm]; [apple-RE] pitches rounded to 16 pixels; `quirks.md` #34 |

### 7b. The PP's DMA into the port

The PP5022's DMA channel 3, memory to the fixed `DATA` register, 32-bit, free running (no request line), at most 32 KB a piece (the count field is 16 bits); the source must be word-aligned and cache-clean (`commit_dcache` or an uncached alias) (`bcm-devtest-hal.c` `dt_ppdma`, `bcm_video_wr_block`) [ours]. Apple's `osos` uses the PP's DMA for blocks of 256 bytes and up in 64 KB chunks [apple].

| Session | Target | Result | Evidence |
|---|---|---|---|
| S8 (upload lab) | SRAM host frame `0xE0000`, one address, five 32 KB pieces | 27.6 ms unboosted, 10.4 ms boosted for 153,600 B; 48 sampled words read back right. **But** the CPU methods had just written the same frame to the same place, so the readback cannot tell a landed transfer from none [ours: from `bcm-devtest-core.c` `e_lab`]. S8 proves the PP's DMA runs and how long it takes, not that the bytes arrived | [silicon] S8 log |
| S32 | SDRAM FIFO, an address per block | Read back 77% wrong: each block's first word missing and later words shifted | [silicon] S32 |
| S33 | SDRAM FIFO, after waiting for the port's write side after each block | Word 0 of every block still never landed | [silicon] S33 |
| S34 (DMA lab) | SDRAM FIFO, blocks of 256 B, 1, 2, 8 and 32 KB, unique fill pattern, every word read back | **Nothing landed** at any size: 64 of 64 … 8,192 of 8,192 words wrong; with the CPU rewriting word 0 afterwards, only word 0 was right. Times 58 µs (256 B) to 2.5–2.7 ms (32 KB) | [silicon] S34 log |
| S35 (DMA lab) | SDRAM FIFO and SRAM `0xE0000 + 0x8000·k`, unique fill pattern, 256 B to 32 KB | **Nothing landed in either**: every word wrong at every size; 32 KB in 2.5–2.8 ms | [silicon] S35 log (`session 35 bcmtest.log`) |

**Status:** PP DMA into the port has not been shown to deliver a single byte, into SRAM or SDRAM (S8 reported clean; S35 showed that reading invalid; `../data-paths.md` D1). Our player's default is the CPU's copy (dev39a onward) [ours]; the DMA path stays behind a switch for the lab.

## 8. Who may touch what, when [ours unless tagged]

The chip has no arbitration a program can see: the host's port writes land in memory whenever they arrive [silicon S12: 0 wrong while the core decodes], and nothing tells either side that the other is in the middle of a word. Safety is entirely by convention.

| Region | Owner | Rule |
|---|---|---|
| Whole SRAM, before the start write | host | The upload writes `0x0` up; then `0x1F8 = 0`, the cache flush and the start at `0x1000_0400`. After start, the host writes only the regions below |
| VP header `0x100–0x13F` | one writer per word | Host: `CMD_WR`, `EVT_RD`. Chip: every other word; `HP_VERSION` written last at start-up, so 0 means "not up". Indices encoded (torn-read check) |
| **Slot pools** (`jobs.h` classes; `config.h` `BCMFW_JOBS_*`) | host writes a slot between `ALLOC` and `COMMIT`; chip reads it after | Host writes into every pool work after the start: [silicon] CR1 2026-10-25, the control area and 7 pool addresses, 0 wrong, by ours and Apple's write forms. Word-aligned writes only (§7a) |
| Command ring `0xD0000` | host writes, chip reads | The host fills the record's words, then publishes `CMD_WR`. The port keeps writes in order, so the chip never sees an index ahead of its data [silicon: every session] |
| Event ring `0xD2000` | chip writes, host reads | The same, mirrored, with `EVT_WR`/`EVT_RD` |
| `0x1F8` command word | alternating | Host writes a command (`BCM_CMD(x)`) and kicks `CONTROL = 0x31`; the image overwrites it with a status when done; the host writes again only after it sees a non-busy status (or 50 ms of busy) [rockbox protocol] |
| **Overlay region `0x12B000–0x133FFF`** (merged image) | host writes between jobs; chip runs it after `OVERLAY` | One region, 36 KB, shared by DECODE's and the visualizer's overlays: a load replaces the other's, and a load is written only while no overlay op is queued (`FENCE` first). The PP's job owner puts it piece by piece; the file's sums are checked on both sides. `HP_OP_TONE`'s buffer is inside it, so TONE is refused while any overlay is live [ours]. `host-driver.md` §6a; `quirks.md` #49 |
| Bitstream FIFO (SDRAM `0x000000–0x0FFFFF`) | host writes ahead of `FIFO_RD`; chip reads behind it | Host writes only after the `OPEN` reply (the bring-up probe and memory test write there); only into free space = size − (written − 64·`FIFO_RD`); each packet 64-byte aligned, then a `FEED` record. The chip advances `FIFO_RD` (64-byte units, encoded) only after the packet is **decoded**, so the host never overwrites bytes still being read |
| Wrap copy, DPB, dumps | chip | The host only reads dumps, after the chip says they are written |
| PCM rings (stage 8b) | host writes the input ring, chip the output ring | Only inside `0x340000–0x39FFFF`; each block handed over by an `AUDIO` command, read back after the reply |
| **Host frame `0xE0000`** | host writes; chip reads on command | **Outside a session**: the host draws its UI here and sends `LCD_UPDATE`/`LCD_UPDATERECT`; the image pushes it to the panel (Rockbox's contract) [rockbox]. **During a session** (`HP_OPEN_OWN_DISPLAY`): LCD commands are acknowledged without drawing; the host may still write here, and an `OVERLAY` command makes the image blit a rectangle of it over each picture. The image never writes here; labs may |
| `VIDEO_FB` `0x105800` | chip | The LCD DMA reads it; the host has no reason to touch it |
| Guard `0xDF000`, progress words `0x1C4`–`0x1D0`, `STATUS` replies | chip writes, host reads | Read-only diagnostics for the host; multi-word records may be read mid-update, so treat them as hints |
| SDRAM, before `OPEN` | nobody | Not initialised; host writes to `0xC000_0000` are undefined until the chip's bring-up has passed |
| Everything, across a power cut | — | Nothing survives: SRAM, SDRAM and the VRF are lost, and every wake is a fresh upload [silicon]; `host-driver.md` §3 |

## 9. Performance

Measured on silicon unless marked as an estimate.

| Path | Figure | Evidence |
|---|---|---|
| Host → SRAM, CPU 2-register `ldm`/`stm` | 27.0 ms a 153,600-byte frame unboosted (PP 30 MHz): 351 ns a 16-bit transfer, 5.7 MB/s. Boosted (80 MHz): 9.6 ms, 125 ns, 16 MB/s | [silicon] S8 lab |
| Host → SRAM, 8-register bursts | 27.4 / 9.9 ms: no gain over 2 registers | [silicon] S8 |
| Host → SRAM, PP DMA | 27.6 / 10.4 ms (landing unproven, §7b) | [silicon] S8 |
| Scaling | Into SRAM with the BCM at 27 MHz, time falls 2.8× for a 2.67× PP clock: about 10 PP cycles a 16-bit transfer by every method, so for that target the limit sits on the PP side [silicon] S8; reading [hypothesis]. Into SDRAM with the PP at 80 MHz the BCM's clock matters too (next row), so "never the BCM" does not hold in general (#47) | [silicon] S8, S33, S34 |
| One short write with its address | 7.4 µs (240 two-pixel rows in 1.78 ms, unboosted) | [silicon] S8 lab |
| Host → SDRAM, CPU copy during video | PP at 80 MHz: 17–18 MB/s of port busy time with the BCM at 150 MHz, 10–12 MB/s with it at 27 MHz (boosted matrix rows). Unboosted rows also logged ~16 MB/s, but the PP had been boosted when each run started. Readback 0 wrong over thousands of words a session | [silicon] S31, S33, S34 |
| Host → SDRAM, PP DMA | 32 KB in 2.5–2.7 ms (≈ 12–13 MB/s) of engine time, but nothing landed | [silicon] S34 |
| Host read of one word | Not measured alone. Estimate 0.5–1.0 µs a word (handshake plus the chip's read latency) | [hypothesis] `audio-spike.md` §2 |
| Command round trip (host → chip → host) | 6–7 µs | [silicon] S2 |
| Port handshake, worst under load | 47 µs | [silicon] S12 |
| Core, vector row load/store in SRAM | ≈ 31 bytes a cycle at REP 16 | [silicon] S7 |
| Core, scalar memory in SRAM | 1 dependent instruction a cycle; loads pair with ALU work | [silicon] S6, S7 |
| LCD DMA from SRAM | 14.26 ms a frame, bound by the LCD bus, not memory | [silicon] S7 |
| Core or vector unit to SDRAM; chip DMA to or from SDRAM | Not measured | #7 |
| Upload of a 93 KB image | Part of the 0.37–0.61 s boot; a ~8 KB image is ready ~2 ms after its upload | [silicon] S7, S30 |

A 30 fps, 768 kbit/s stream needs about 3.2 KB a picture: under 0.6 ms of port time unboosted, by the S8 rate [estimate, ours].

## 10. Open

New items, numbered in `open-questions.md`:

- **#41** PP DMA into the port: nothing landed in SRAM or SDRAM (S35); why is open.
- **#42** What lies at and above `0x0014_0000` (SRAM alias, bus error, open bus).
- **#43** The port's finer rules: a lone halfword write, auto-incrementing reads, the ALT bank's view, cross-views (`0xFE…` from the host, `0xC0…` from the core).
- **#44** Whether host-port traffic steals cycles from the core or its memory.
- **#45** Byte and halfword stores into SDRAM: right when read back later (S36c).
- **#73** Whether the core sees the port's SDRAM writes before a command that follows them (a late write before a `FEED`; sitting 4b).
- **#68–#70** Alignment: misaligned vector forms, DMA sub-word units, the port's 2-mod-4 writes in halfword forms (§11, bench ALGN).
- **#46** The AAC decoder's refusals on silicon, which differ run to run; a store then an immediate load in SDRAM is the remaining suspect (`HP_AAC_MEMSTRESS`, `HP_AAC_STATE`).

Already listed: the cache (#6), SDRAM latency, bandwidth and power-down (#7), address aliases (#8), DMA with SDRAM (#13), exceptions for other misaligned forms (#5), `vst8` layouts (#2).

## 11. Alignment

Both chips need aligned accesses, but they fail differently. On the PP5022 a misaligned 16- or 32-bit access to SDRAM is a precise **data abort** (`../pp5022-reference/quirks.md` #51 [silicon]), and the gate's UBSan alignment checks catch host-side code before it reaches the device. On the BCM nothing stops at the access: a misaligned scalar load completes from the rounded-down address and raises vector 1 a few instructions later, a misaligned plain vector load silently reads the aligned row, and the host port drops the write address's low bits. So a BCM alignment bug shows up as **wrong data**, not a crash. This section says what each path does with an address that is not naturally aligned, how sure we are, and the rule our code follows. "Unknown" means no session has run it.

The tools: vc2sim's **strict alignment mode** (`tools/bcmre/vc2sim.py` `STRICT_ALIGN`; environment `VC2SIM_STRICT_ALIGN=trap|log`) traps (or logs) every access of ours whose silicon behaviour this section marks unknown, and lets the known ones run as modelled; `tools/bcmre/align_audit.py` runs the images' workloads under it and lists the static sites; the findings are in [`../research/bcm2722/alignment-audit.md`](../research/bcm2722/alignment-audit.md). Bench row **ALGN** (`../research/bcm2722/bench-p19.md`, "ALGN") has a silicon check for each unknown below.

**The policy (2026-10-25): the simulator refuses whatever silicon might do differently.** `trap` is vc2sim's **default** (`VC2SIM_STRICT_ALIGN` unset), in every bcmfw test and in `scripts/validate.sh`'s BCM steps (`scripts/bcmfw_benches.sh` exports it): an access of ours in a form this section marks unknown stops the run with `AlignTrap`, rather than quietly succeeding under the model's guess. `log` counts instead (the audit's mode, `align_audit.py`); `0` turns it off (comparisons with the old model only). Apple's code is counted, never trapped; bench row ALGN's checks are exempt (misaligned on purpose). A form moves into the known set (`vc2vpu._strict_vmem`, `vc2sim.STRICT_SCALAR_KNOWN`) only when an ALGN reading settles it. The same rule holds for the tests' host-port models: they drop a write address's bits 1:0 as the port does (quirk #34; `tests/bcmjob` and `hello_test.py`'s harness), so a host writer that starts mid-word fails in simulation as on the chip. The lessons behind it: CR1's 301 × 7 slot (a port model that honoured halfword addresses) and the `0x29` load (a model that zero-extended where silicon sign-extends, quirk #33).

The image batch of 2026-10-25 (`../research/bcm2722/merged-image-batch.md`) removed the three shipped forms that trapped: jdec's colour stage loads three aligned planes (no lone `vldp8` gather) and has a start-up check (`J_VPU_YUV`); the H.264 edge-emulation window has strides of 32 and 16; a frame whose chroma stride is not a multiple of 16 converts in C.

### 11.1 Scalar loads and stores

| Access | Behaviour | Evidence | ALGN check |
|---|---|---|---|
| 32-bit `ld rd, (rs)` (16-bit form) at +1 | Completes from the address rounded down to 4 (no rotation, no byte merge), writes rd, then exception vector 1, imprecise: 4 later instructions run first; `rti` resumes correctly | [silicon] S2, S3; quirk #2 | LW1C (the control) |
| 32-bit `ld rd, (--rs)` at an odd address | The same, 2 later instructions; the base is written back | [silicon] S2, S3 | — (probe check 9) |
| 32-bit `ld` at +2 or +3, and through `(rs + off)` (32-bit `28xx`), the 48-bit `(rs + s27)`, `(rs)++` and `(rs + ri)` | **As the measured case:** each reads the word at the address rounded down, writes rd, then raises vector 1 after 4 later instructions; the post-increment form (LWP2) writes its base back | [silicon] 2026-10-25, sitting 2 (session log `bcmtest.1.log`, bench row ALGN, build 799ff0f8d7): all six as modelled | LWR2, LWR3, LWO2, LWL2, LWP2, LWX2 |
| `ldh`, `ldsh` at an odd address (16-bit `(rs)`, 32-bit `(rs + off)`, `(rs)++`) | **The same:** the halfword at the address rounded down to 2, then vector 1 after 4 later instructions; the post-increment form writes its base back | [silicon] sitting 2 (same log): all five as modelled | LHR1, LHR3, LHO1, LSH1, LHP1 |
| `ldb`, `ldsb`, `stb` | Always aligned | — | — |
| Any misaligned store (`st` at +1, +2, +3; `sth` at an odd address; every form) | **It stores at the address rounded down to its size, then raises vector 1 imprecisely** (the saved pc is 10 or 12 bytes past the store: later instructions ran first). A misaligned store is not lost: it overwrites the aligned word or halfword below its target. vc2sim modelled a precise fault with nothing stored; silicon contradicts both halves | [silicon] sitting 2 (session log `bcmtest.1.log`): all eight store rounded down and fault imprecisely, saved pc +10 (+12 for the offset forms SWO2, SHO1) | SWR1, SWR2, SWR3, SWO2, SWP2, SHR1, SHR3, SHO1 |
| gp- and sp-relative forms (`2Dxx`) at a misaligned address | **Unknown, and not benched:** a misaligned sp would put the exception frame itself out of line. sp stays 4-aligned (as Apple's); our code does not use gp | [apple] | none, by design |
| 48-bit pc-relative `ld`/`st` | Use the instruction's address rounded down to a word: always aligned | [apple]; [silicon] S2 | — |
| Peripheral registers | 32-bit accesses only | [apple] | — |

Rule [ours]: every scalar access naturally aligned; a halfword at an odd offset or a word at 2 mod 4 is built from bytes.

### 11.2 Vector loads and stores

| Access | Behaviour | Evidence | ALGN check |
|---|---|---|---|
| Plain `vld16` (an H16 row, 32 bytes) at +8, +16, +24 | Reads the 32-byte row at the address rounded down to 32 | [silicon] S18–S22; quirk #17a | VL68 (the control) |
| Plain `vld16` at +2 or +4 (not a multiple of 8) | Reads the row at +0, as modelled (rounded down) | [silicon] sitting 2 (session log `bcmtest.1.log`): VL68, VL64, VL62 | VL64, VL62 |
| Plain `vld8` (an H8 row, 16 bytes) at +8, +4, +2 | Reads the row at +0, as modelled. These offsets cannot tell rounding to 16 from rounding to 32 | [silicon] sitting 2: VL88, VL84, VL82 | VL88, VL84, VL82 |
| `vld8v` (a column) off 16 | **Unknown**; our code uses none | — | — |
| A REP whose stride is not a multiple of the row (row k at base + k·stride) | **Each row's address rounds down on its own.** A `vld16` REP 2 at a stride of 40 read its second row from +32, not +40. The model (the base rounds once, later rows as given) was wrong | [silicon] sitting 2: VLS4 | VLS4 (stride 40) |
| The `vldp`/`vldq` pair at any base, REP rows a multiple of 16 bytes apart | Apple's way to load misaligned data: `vldp` takes the lanes in the first 16-byte line, `vldq` those in the second, issued exactly when the bytes cross a line. Ours: the H.264 decoder's chroma MC and the display path's colour tile (`disp_yuv_tile`, chroma and luma rows at any alignment) | [apple]; [silicon, implicit] the decoder bit-exact (S27) and the colour right on screen (S16 on); no per-lane check | — |
| A pair REP whose stride moves the line phase row by row (`disp_yuv_tile` and the decoder's MC at strides of 88 or 120, for picture widths that are not multiples of 16) | Each row splits at its own line, as modelled: a `vldp8`/`vldq8` pair REP 4 at a stride of 20 read every row right. Apple's own strides are multiples of 16, so Apple never needs it | [silicon] sitting 2: VPQS | VPQS |
| A lone `vldp` REP whose stride moves the line phase row by row (`jv_yuv16`'s gather: REP 16, stride 4, lanes 0–3 of each row, all in the first line for a 4-aligned source) | Loads each row's first-line lanes (phases 12, 0, 4, 8), as modelled. A lone `vldp8` from +14 gets lanes 0–1 right and drops the lanes past the line, as modelled (a `vldq` is needed for those) | [silicon] sitting 2: VPG4, VPGX | VPG4; VPGX (#21) |
| `vst16` at a 32-byte-aligned address | Right, rows 0–15 at REP 16 | [silicon] S5, S7, 2026-10-25 | — |
| `vst16` at +2, +4, +8 | **Stores at +0: the address rounds down, as a load's does.** The model (the exact address) was wrong. These offsets cannot tell rounding to 16 from rounding to 32 | [silicon] sitting 2 (session log `bcmtest.1.log`): VS68, VS64, VS62 | VS62, VS64, VS68 |
| `vst8` | Aligned (row 0 at offset 0): stores at the exact address in the modelled layout. At +8 and +4 it stores at +0, rounded down like `vst16` (16 against 32 not told apart) | [silicon] sitting 2: VS80 exact; VS88, VS84 rounded down | VS80 (the layout), VS88, VS84 |
| `vstr` with 8-bit lanes, any address | Writes only the enabled lanes inside the 16-byte line of its address; lanes past the line are dropped | [silicon] S22, S24; quirk #18 | — |
| `vstr` with 16-bit lanes at an odd address or across a line | **Unknown**; our code uses none | — | — |
| Not memory, for completeness: a horizontal B's or a 16-bit D's `+rN` x aligns down to 16; an A operand's `+rN` x is **not** applied as vc2sim models | | [silicon] quirks #17b, #28, #30 | (B4V rows) |

Rule [ours]: plain vector loads and stores only at addresses aligned to their row (32 bytes for 16-bit rows, 16 for 8-bit), strides multiples of the row; misaligned data goes through scalar code, or through a lane shift inside the VRF only after its start-up check passes (quirk #30).

### 11.3 The BCM's DMA engine

| Transfer | Behaviour | Evidence | ALGN check |
|---|---|---|---|
| Control block address | Must be 32-byte aligned (Apple builds it at `(buf + 51) & ~31`) | [apple-RE] vmcs 0x7fc4; [silicon] S5 | — |
| LCD, channel 1: SRAM source 32-byte aligned with 32-byte bursts, or 4-byte units for a sub-rectangle (x and w even) | Right | [silicon] S5–S7 | — |
| Audio, channel 5: 16-byte source width, 4-byte destination width into the FIFO | Runs (S35's tone); the length unit [hypothesis: bytes] | [silicon] S35 | — |
| Channel 7 sub-channel 1, 32-byte units, SRAM ↔ SDRAM (PW8: config `0x0000_E802`, transfer information `0x0102_5151`) | Right, 0 wrong words over 16 KB | [silicon] PW8 | D32C (SRAM → SRAM, the control) |
| 4-byte units, an odd count (9) | Copies exactly (2 µs) | [silicon] sitting 2 (session log `bcmtest.1.log`): D4W9; the control D32C exact | D4W9 |
| 2-byte units (ends at 2 mod 4), 1-byte units (odd length, odd ends) | **Copy exactly on silicon**, as Apple's set-up assumes: the unit is the largest power of two ≤ 32 dividing both addresses, the pitches and the row length, so a 602-byte row runs in 2-byte units [apple-RE] vmcs 0x83a4, 0x816e, 0x80d2. vc2mmio moves whole words a unit (wrong for these) | [silicon] sitting 2: D2H3, D1B5 exact, 2 µs each, status `0x4000` | D2H3, D1B5 |
| An address that is not a multiple of the chosen width (32-byte units to +4) | **The destination rounds down:** the 32 bytes landed at +0, not +4, with no error (status `0x4000`, 1 µs). The logged window cannot tell rounding to 8 from rounding to 32. Apple never sets one up | [silicon] sitting 2: D32M | D32M |
| A peripheral FIFO destination | Width capped at 4 bytes by Apple | [apple-RE] vmcs 0x80d2 | — |

Rule [ours]: units of 4 or 32 bytes, each address a multiple of its width, every 2-D skip keeping that. Byte and halfword units in one dimension are proven (D2H3, D1B5) and vc2sim's strict DMA lets them run; in 2-D they stay refused until a row runs them.

### 11.4 The host port (PP → BCM)

| Access | Behaviour | Evidence | ALGN check |
|---|---|---|---|
| A write whose address is 4-aligned, any number of words, by 32-bit or 16-bit data stores | Lands exactly | [silicon] every upload; CR1 (2026-10-25): every region, Apple's four write forms, every row at 0 mod 4 | PWD0, PWD4, P164, P168, P16C |
| **A write starting at 2 mod 4** | **Lands 2 bytes low: the port's write address ignores bits 1:0.** CR1's 301 × 7 slot (pitch 602) read back wrong from x 300 of row 0, then every pixel of row 1 one to the left; 906 bad pixels = rows 1, 3 and 5 (the rows at 2 mod 4) × 302 (300 shifted, the row above's x 300 overwritten, the row's own last pixel lost); rows at 0 mod 4 exact; the same by the CPU's copy and by engine 2 | [silicon, inferred] session log `bcmtest.log` (the `cr1 strip 301x7` lines); the count fits exactly. **[silicon] directly, sitting 2** (session log `bcmtest.1.log`): PWD2 (word data at +2) wrote 129 of 160 halfwords wrong, 128 of them 2 bytes low | PWD2, R602 (did not run, below) |
| The same with halfword data stores, or Apple's halfword address writes | **The same: 2 bytes low** (129 of 160 halfwords wrong, 128 of them 2 bytes low, in both forms). The halfword forms do not rescue a 2-mod-4 start | [silicon] sitting 2: PHD2, PAD2 | PHD2, PAD2 |
| **A lone trailing halfword** (an odd number of halfword stores, then a new address) | **Dropped.** After 1, 3 or 5 halfword stores at a 4-aligned address and then a word written elsewhere, exactly one halfword was wrong each time: the last, unpaired one (+0x20, +0x24, +0x28), which still held the fill; the pairs before it and the word and block after it landed. The port commits halfwords in pairs, and an unpaired last one is lost when the address changes. Apple's writer sends one whenever a length is 2 mod 4 [apple-RE], so Apple's callers must avoid that case or follow it with more data [hypothesis] | [silicon] sitting 2: PLH1, PLH3, PLH5 (1 of 96 halfwords wrong each) | PLH1, PLH3, PLH5 |
| Our odd last pixel as a 32-bit word (high half 0) | Writes 2 bytes past the row: harmless at a 4- or 32-byte pitch (padding), corrupting at a 2-mod-4 one. Three halfwords sent this way, then a word elsewhere, landed exactly | [ours]; CR1; [silicon] sitting 2: PLW3 | PLW3, R64W |
| Engine 2's block, then at once a new address and a word, against waiting for the port's write-ready | **Unknown** whether the tail can overtake the engine's last words. Apple waits for each DMA piece before the PIO tail | [apple-RE] osos 0x10286fb4 | E2ND, E2DR |
| A row written at 2 mod 4 by read-modify-write of its end words | Expected exact (aligned words only) | [hypothesis] | RX13, RX1W |
| Reads: `RD_ADDR`, then 32-bit `DATA` reads | Right per word; Apple sets the address once a call and reads on (auto-increment) | [silicon] every readback; [apple-RE] osos 0x10287a6c | — (#43) |
| PP side: the engine-2 source | Word-aligned, a multiple of 4 bytes, 4 bytes to 64 KB (`e2port_fits`); the general engine's channel 3 delivers nothing (§7b) | [silicon] S37; [ours] | — |

**Sitting 2's engine-2 rows did not run.** R602 (301 × 7 at pitch 602 by engine 2 with a halfword tail) returned port error −1 before its check, and a port error ends the part, so R604, R608, R64W, E2ND, E2DR, RX13 and RX1W never ran: 54 of ALGN's 61 checks have readings. The bench's engine-2 path returns −1 for a refused claim, a source `e2port_fits` rejects, a start failure, a timeout, or a port not ready afterwards, and the log does not say which [silicon: the failure; its cause unknown]. R602's pitch (602) is itself 2 mod 4.

Rules [ours, from 2026-10-25]: every host write starts at a 4-aligned BCM address, so every row of a slot or frame starts 4-aligned (a pitch that is a multiple of 4 bytes, of 32 where vectors walk it); an odd last pixel goes by read-modify-write of its word, or as a word with a zero high half where the pitch has room, **never as a lone halfword store** (PLH1: it is dropped); a write that must start at 2 mod 4 merges its first halfword into the word below by read-modify-write. The `ALLOC` pitch rounding and the PP writer's fix are on branch `p19-image-batch-2`; ALGN confirms them on silicon.

**What the 301-pixel case implies.** Any slot of odd width in a class whose pitch is the width (`J_SLOT_PITCH`: every class but the drift's) has its odd rows at 2 mod 4, and those land one pixel low and overwrite the row above's last pixel, whichever PP path writes them. Rockbox's own driver always rounded x and width to even and pushed whole rows, which hid this ([rockbox] "the BCM ignores small unaligned writes", `../pp5022-reference/display.md`). The `J_E_SUM` failures of the even widths in the same run are a different fault (readback right, the BCM's own sum differs: `open-questions.md` #67).

### 11.5 Apple's conventions [apple-RE]

Read from Apple's 1.3 images; our own words, addresses only.

- **Host-described images** (osos 0x10286a1c): pitch = bytes per pixel × width, rounded **up to 4 bytes**; a 301-pixel RGB565 image gets 604, never 602. Every row starts word-aligned in VC memory.
- **Display updates** (osos 0x10164450, 0x10164f44): whole rows only, one contiguous band from y0·pitch to y1·pitch; x and width never cross the port (the rectangle travels in a message, and the VC composes).
- **The file service** that loads libraries and media into VC memory (osos 0x100f98a0–0x100f9a64): every port write starts 16-aligned and is a multiple of 16 bytes, in 1 KB pieces; a head or tail under 16 bytes travels inside the reply and the VC places it itself. Messages are a 16-byte header plus a body padded to 16 (osos 0x100fa0b4); rings keep 16-byte slots (osos 0x1028871c, 0x10288800).
- **A streaming ring** (osos 0x1027eefc): chunk lengths are multiples of 4; any other spins for ever.
- **The writer** (osos 0x10287be8): never checks or rounds the VC address; a leading halfword when the *source* is 2 mod 4; DMA for the middle of 256 bytes and up, each piece waited to completion; a 16-bit PIO tail (a lone halfword allowed, an odd byte dropped). Its callers always hand it word-aligned, usually 16-aligned, addresses [hypothesis].
- **VC DMA set-up** (vmcs 0x83a4, 0x816e, 0x80d2, 0xdcd2): each side's width is the largest power of two up to 32 dividing its address, pitch and row length; the unit is the smaller; a FIFO destination is capped at 4 bytes; no rounding and no refusals. Control blocks 32-byte aligned (vmcs 0x7fc4).
- **VC images** (vmcs 0x4a76, 0x17c48, 0x187ec): an RGB565 pitch is the width rounded up to 16 pixels (a multiple of 32 bytes); allocations round the height up to 16 rows.
- **Copies** (vmcs 0xdf60 `memcpy`, 0x1d080 `vclib_memcpy`): `memcpy` never makes a misaligned scalar access (words only when both pointers are 4-aligned, halfwords when both are even, else bytes); `vclib_memcpy` uses plain `vld16`/`vst16` only when both are 32-aligned, brings the destination to 16 with a scalar head, reads a misaligned source only through `vldp`/`vldq`, and stores 16-aligned (`vstr`).
- **Vector forms in reached code:** 345 plain loads and stores, none at a provably misaligned address; 63 pair loads for misaligned data. Apple's pattern is the one quirk #17 forces.

Where ours differs from these, ranked, with the rule to adopt and the ALGN check for each: [`../research/bcm2722/alignment-audit.md`](../research/bcm2722/alignment-audit.md) §4.

### 11.6 Open

`open-questions.md` #5 (answered by sitting 2 for loads and stores), #43 (the lone halfword answered: dropped; auto-incrementing reads still open), #68 (answered, except whether 8-bit rows round to 16 or 32), #69 (answered), #70 (answered for the CPU forms; the engine-2 rows R602–RX1W did not run). `scripts/bench_ingest.py` reports the readings after a sitting.

**What vc2sim models since 2026-10-26 (branch `p19-sim-trust`), the default variant each time [sim, fitted to the words above]:** a misaligned scalar store lands at the address rounded down, then vector 1 after 4 later instructions (HYP `exc_mis_store_rounds`; every load and store form ALGN ran is in `STRICT_SCALAR_KNOWN`, keyed by encoding, operation and indexing); `vst16` and `vst8` off their size store at the address rounded down to their own size (`v_st_align` `'width'`) and every REP row rounds on its own (`v_ld_rep_round` `'row'`); byte and halfword DMA units copy exactly and a 32-byte unit to a destination off 32 lands at the destination rounded down to 32 (`DmaEngine.mis32` `'round'`); the host-port models (`tests/bcmjob`, `hello_test.py`'s harness, `apps/video/vbcm_model.c`) drop a lone trailing halfword. **Strict mode still refuses what silicon left open:** a misaligned access in any other scalar form (2Dxx, the 16-bit `ldst_off`/`ldst_sp`, 48-bit stores, `(--rs)` stores); plain vector loads and stores at offsets no session ran (`vc2vpu.VMEM_KNOWN_OFFSETS`: `vld16` at 0/2/4/8/16/24 mod 32, `vld8` at 0/2/4/8 mod 16, `vst16` at 0/2/4/8, `vst8` at 0/4/8, columns aligned only), each REP row checked on its own; a store REP at a stride off its size; a pair REP whose line phase moves by other than 4 bytes a row; any DMA unit off its width (D32M included), units of 8, 16, 64 or 128 bytes and sub-word units in 2-D. vc2sim's strict forms mode (`VC2SIM_STRICT_FORMS`, default `trap`) also refuses any vector operand form `tools/bcmre/proven_forms.txt` does not list, and `scripts/proven_forms.py` checks every shipped image against that table (exit criterion E2 of `../research/hardware-closure-plan.md`). The silicon words are fixtures: `tests/silicon/20261025-799ff0f8d7/stage4.log`, replayed by `bcmfw/tests/silicon_test.py`.
