# 20. Product brief map: power, bus, clocks and memory

Broadcom's two-page product brief for the BCM2722 ([external] 2722-PB01-R, dated 2005-10-18) is the only document the vendor published for this chip. This chapter takes every claim it makes about **power, the host bus, clocks and memory** and says, for each, what we know, how Apple's firmware uses it, what ProkBox could gain from it, and the device stage that would settle what is still open.

### Feature catalog (for `datasheet-features.md`)

**Status** says whether the feature is used on this device today, by our firmware or Apple's:
- **used**: in use;
- **known-unused**: it exists and its mechanism is known, but nothing runs it;
- **unknown**: not settled;
- **unverifiable**: no test is possible on this board.

The stages PW1–PW8 are in §9.

| feature | source | status | evidence tag | ProkBox benefit | probe |
|---|---|---|---|---|---|
| Bus bypass mode (host reaches LCD with BCM powered down) | brief: "Bypass mode allows host access…" | unknown | [hypothesis] | lit static screen with BCM off | PW6 |
| Run mode with DVFS (5–150 MHz, 1 MHz steps) | Apple `pm_set_policy min` @fe00 | used | [apple] | pick video clock per stream | PW4 |
| Sleep (core `sleep` instruction idle) | Apple idle loop; brief "sleep" | used | [apple] | idle C2 without polling | stage 5i |
| Hibernate (`videocore=1`, GPO32 bit 15 then bit 14 low) | Apple @14288, host `0x1028_92D4` | known-unused | [apple] | keep SRAM; wake in ~10 ms, not 0.4–0.6 s | PW5 |
| Power-down (`videocore=0` + bit 14 low) | Apple @14244; Rockbox cut | used | [silicon] | today's dark state | — |
| 0.6 V hibernation core | brief | unknown | [external] | lowest non-off state | PW5 + meter |
| Core regulator / SMPS, levels 5–15 | `0x1000_1400` [3:0]; Apple table `0x1FA68` | used | [apple] | level 5 at 27 MHz (we run 11): ~40 % less core power | PW3 |
| Regulator bits [7:4] | `0x1000_1400` | unknown | [silicon] | none known | PW1 |
| Programmable I/O 1.5–2.8 V | brief | unverifiable | [external] | none | — |
| PLL A (core clock) | `0x1000_1C00/14`; S7 | used | [silicon] | 150 MHz video | — |
| PLLs B, C, D ("3 PLLs for external devices") | brief; Apple @14912 | known-unused | [apple] | exact audio clocks (PLL C) | PW1 |
| Block clock words (src [10:8], div [7:0]) | Apple @4d96, @5334 | used | [apple] | find clocks left running | PW1 |
| Audio clocks 24.576 / 2.048 MHz (ids 2, 3) | Apple @13c32 | known-unused | [apple] | BCM audio path | see peripherals |
| SDRAM refresh follows the clock | Apple @4d18 | used | [apple] | — | — |
| I + D caches, 128 Kbit | brief; `0x1000_0C00` | known-unused | [apple] | SDRAM code/data speed | PW2 |
| Caches cover SDRAM only | Apple's flush sites @88a0, @91a4 | unknown | [hypothesis] | no flushes for SRAM rings | PW2 |
| 1.25 MB SRAM | brief; S1 on | used | [silicon] | — | — |
| 4 MB stacked SDRAM | brief; S10, S11 | used | [silicon] | — | — |
| SRAM retained in hibernate | Apple wake path @14288 | unknown | [apple] | slots survive dark | PW5 |
| SDRAM retained in hibernate | Apple probe save/restore @147ca | unknown | [hypothesis] | pools survive dark | PW5 |
| High-speed DMA, SRAM/LCD | brief; `0x1000_0800`; S4–S7 | used | [silicon] | LCD push | — |
| DMA memcpy SRAM ↔ SDRAM (ch 7) | Apple `dma_memcpy` | known-unused | [apple] | textures without the core | PW8 |
| Secondary memory interface (= LCD block CS0) | brief; `lcd.md` | used | [hypothesis] | bypass target | PW6 |
| NAND/NOR flash, NAND boot | brief | unverifiable | [external] | none (not fitted) | — |
| Memory-mapped host port, 16-bit | brief; `0x3000_0000` | used | [silicon] | — | — |
| 3-bit host address (2 banks × 4 regs) | brief; host bits 16–18 | used | [hypothesis] | — | PW7 |
| Dual software channel (ALT bank) | brief; Apple display path on bank 1 | known-unused | [apple] | per-core channels, no shared latch | PW7 |
| Programmable interface type (bootstrap bytes) | brief; #24 | unknown | [hypothesis] | none yet | — |

The other half of the brief (TV out, the LCD controller and its scaler, audio, the serial ports, USB, the camera, DRM, and the media, 3D and MIDI software) is mapped in the companion chapter, `brief-map-peripherals.md`. Where a finding here touches one of those blocks (the audio and USB clocks, the LCD bus), this chapter states the clock or power fact and points there.

**Sources read for this chapter** (2026-10-23):
- the brief itself;
- Apple's VideoCore image `vmcs.bin` (byte-identical size and layout in 1.2.1, 1.2.3 and 1.3; addresses `@xxxx` are in 1.3, as everywhere in this manual);
- Apple's ARM firmware `osos` from the 1.3 updater (host addresses `0x1xxx_xxxx`; 1.2.1 checked where stated);
- Rockbox's `lcd-video.c`, and the silicon sessions already in this manual.

Everything from Apple's code is described in our own words. The `@` and host addresses are pointers into the corpus for anyone repeating the work, not code.

**Tags** as in the [evidence legend](index.md#evidence-tags). Two more are used here, as in the rest of the manual: [estimate] for a figure computed from other figures, and [ours] for a stage design of our own.

## 1. The claims at a glance

"Status" uses four words:
- **known** (silicon or Apple's code settles it);
- **mapped** (Apple's code shows the mechanism; silicon has not run it);
- **open**;
- **n/a** (not fitted, or no use on the iPod).

| # | Brief claim [external] | Status | Evidence | Leverage for ProkBox | Stage (§9) |
|---|---|---|---|---|---|
| P1 | "Bus Bypass Mode"; "Bypass mode allows host access to memory-mapped peripherals with BCM2722 in power-down"; "Power-efficient bypass mode" | **open** | Apple never uses it. No host access to the panel exists outside the BCM's port, and no BCM register write enters it [apple]. The only unexplained BCM control line is `GPO32` bit 15 (§3) [hypothesis] | A lit panel with the BCM powered down: the PP draws static screens itself | PW6 |
| P2 | Four power modes: run, sleep, hibernate, power-down | **mapped** | **Run:** Apple scales the clock and the voltage with load. **Sleep:** the core's `sleep` instruction in the RTOS idle loop. **Hibernate:** a gencmd parks the chip, then the host sets `GPO32` bit 15 and clears bit 14. **Power-down:** the same park, then both pins low [apple] (§2, §3). Power-down by bit 14 alone [rockbox; silicon] | Hibernate keeps SRAM, so a wake costs milliseconds instead of a 0.37–0.62 s reload | PW5 |
| P3 | 0.8–1.2 V core; 0.6 V in hibernation | **mapped** (voltages open) | Regulator levels 5–15 follow the clock through a 13-row table; the setter accepts 1–15 [apple]. No register write selects 0.6 V, so hibernation's voltage is set by pins or by hardware [hypothesis] | A host-started image runs at level 11 at 27 MHz, where Apple's table asks for 5 (§4) | PW1, PW3 |
| P4 | On-chip SMPS controller | **mapped** | `0x1000_1400` `[3:0]`, with a 10 ms settle after each change [apple]. Bits `[7:4]` are unknown (they read `0xB`) [silicon S1, S7] | As P3 | PW1, PW3 |
| P5 | Programmable I/O, 1.5–2.8 V | n/a | No register known; the board fixes the rails | none | — |
| P6 | Clock manager: three PLLs make precision clocks up to 200 MHz for external devices | **mapped** | Four PLL register sets, A–D: A runs the core, and B, C and D feed the ten block clocks [apple]. "Three PLLs for external devices" fits B–D [hypothesis]. Apple's PLL rule and the block-clock word format are in §5 | Exact audio clocks for the BCM's audio path; per-session clock choice | PW1, PW4 |
| P7 | 150 MHz dual-ALU VideoCore II | known | PLL A ×600/108 [silicon S7]. Apple runs anywhere from 5 to 150 MHz in 1 MHz steps (§4) [apple] | Run video below 150 MHz when the stream allows | PW4 |
| P8 | 128 Kbit instruction and data caches | **open** | Apple flushes the whole cache only after it has written code: relocating a loaded library, and one library start [apple]. It never flushes for data the host writes, and its host rings live in SRAM, so the caches probably cover SDRAM only [hypothesis] (§6) | Speed for SDRAM-resident code and data (AAC far data, large DPBs, 3D textures) | PW2 |
| P9 | 10 Mbit on-chip SRAM + 32 Mbit stacked SDRAM | known (size); **mapped** (retention) | 1.25 MB SRAM, 4 MB SDRAM [silicon]. Apple's hibernate expects SRAM to hold; it saves and restores the SDRAM probe words around the re-initialisation (§3) [apple] | What survives hibernate: the resident image, the UI slots, maybe SDRAM | PW5 |
| P10 | High-speed DMA | known (core); open (SDRAM) | 8 channels, 1-D and 2-D, DREQ pacing [apple; silicon S4–S7]; Apple's memcpy uses channel 7, sub-channel 1 (`dma.md`) | Moving textures and covers between SRAM and SDRAM without the core | PW8, B5 |
| P11 | Secondary memory interface (memory-mapped LCD, flash, external peripherals) | **mapped** | The LCD block's per-chip-select bus timing (`lcd.md` §1) is what such an interface needs; only chip select 0 is used [apple]. Reading it as the secondary interface is [hypothesis] | It is the bus that bypass would connect the host to | PW6 |
| P12 | NAND/NOR flash, NAND boot | n/a | Nothing fitted; every boot is a host upload [rockbox] | none | — |
| P13 | Appears to the host as a memory-mapped peripheral | known | `0x3000_0000–0x3007_FFFF` [rockbox; silicon] | — | — |
| P14 | 3-bit address, 8/16-bit data bus | known (16-bit); mapped (3-bit) | The host selects one of eight registers with address bits 16–18 [rockbox]; that is the brief's 3-bit address (two banks × four registers) [hypothesis, strong] | — | PW7 |
| P15 | Programmable industry-standard interfaces | open | The bootstrap bytes probably program the port (`open-questions.md` #24) [hypothesis] | none yet | — |
| P16 | Dual software channel | **mapped** | Two banks, each with its own address latches and its own lock in Apple's driver. Apple sends bulk display pixels on the ALT bank and everything else on the main bank [apple] (§7) | Two threads (or the two PP cores) talk to the BCM without sharing one address latch | PW7 |

**What this means for ProkBox, best first** (by value for battery and speed):
1. **Hibernate** (P2, P9): keep the BCM's memory through dark spells and wake in milliseconds (§3, PW5).
2. **Voltage that follows the clock** (P3, P4, P6, P7): Apple's own table, applied to our images (§4, PW3, PW4).
3. **Bus bypass** (P1): a lit, static screen with the BCM powered down. The largest possible gain while the screen is lit, and the least certain (§8, PW6).

The caches (P8) come fourth: a speed lever for SDRAM work, with coherence rules to learn first (§6, PW2).

## 2. Apple's power manager (run and sleep)

### What the ARM side asks for

- Once, right after the disk `vmcs.bin` has booted, `osos` sends the text command `pm_set_policy min` over the gencmd service. If a debug flag is set it also sends `pm_show_stats 0 10 10 90 16` [apple] (host `0x101B_013C`; the strings are at host `0x101B_01E4` and `0x101B_0200`).
- Nothing else in `osos` 1.3 talks to the power manager by text.

### The VideoCore side

[apple] `vmcs.bin`. Five commands are registered together (@f92e): `pm_set_policy`, `pm_get_status`, `pm_show_stats`, `pm_start_logging` and `pm_stop_logging`.
- `pm_set_policy` (@fcec) takes `min` (policy 1), `max` (policy 2) or a number (a fixed frequency).
- `pm_get_status` replies with `core=…`, `free=…` and `overrun=…`.

**Clients.** Software that needs a minimum speed registers as a power-manager client (@fa24, up to ten). Each client holds a request: either a minimum frequency, or "full speed" (150 MHz). The TV-out path, for example, asks for full speed while it runs (@5916). The manager takes the highest request, with a floor of **5 MHz**.

**The `min` policy** (@fe00) is a load governor:
- It works in windows of 10,000 µs on the 1 MHz counter.
- It measures how busy the core was through the RTOS idle loop.
- It raises the clock by about 6 % when the core was busy, and lowers it when the core idled.
- The result is rounded to a whole MHz and clamped to 150 MHz, and never falls below the clients' requests.

**Setting a frequency** (@5134):
1. Clamp to 5–150 MHz.
2. Look up the core supply level for the new frequency (§4). If it is higher than the current level, raise the regulator first and wait 10 ms.
3. Program PLL A with Apple's PLL rule (§5) and wait 1,000 µs.
4. Switch the core clock word to PLL A with the needed divider.
5. If the new level is lower, lower the regulator afterwards.
6. Retune the SDRAM refresh for the new clock (@4d18): the interval is the clock ÷ 128,000, which gives 7.8 µs at any clock.

**Sleep.** Between tasks the RTOS idles in `ei; sleep; di`, with a 10 ms tick (`boot-and-clocks.md` §1) [apple]. No other clock gating is applied when it idles [apple, static: no clock or regulator write on the idle path].

**Two stubs.** The gencmds `power_down` and `power_management` are stubs: one logs a debug line, the other replies `Ignored` (@c3c8, @c3f0) [apple]. The real power path is `power_control` (§3).

**What it gives us.** Apple's chip spends most of its life at a low clock and a low voltage, and goes to 150 MHz only for work that needs it. Our images do the clock half already: 27 MHz for the UI, 150 MHz for a video session (`../research/bcm2722/power-contract.md` §2). They do **not** do the voltage half (§4).

## 3. Hibernate and power-down: `power_control videocore=N` and `GPO32` bit 15

### The VideoCore side

[apple] The gencmd `power_control` (registered @bcd2) passes `key=value` pairs to a handler (@14228). The only key is `videocore`; values 0, 1 and 2 are accepted and 2 does nothing. On values 0 and 1 the chip parks itself and reports each step in the SRAM word **`0x1F0`**, which the host polls through the port.

**`videocore=1`, hibernate** (@14288):
1. Interrupts off.
2. **Clocks parked** (@4f3c with 2): the current core frequency is saved, the core clock word goes to the crystal (`0x1000_1C28 = pw 0x0001`), and all four PLLs are given a divider and multiplier of 0.
3. The GPIO event status and edge-detect words, `0x1000_2828`–`0x1000_285C` (14 words), are saved to SRAM and then cleared, so no pin event can fire.
4. `0x1F0 = 0`.
5. The stack is switched to an area in SRAM (@1cdf4).
6. **SDRAM idled** (@14892): after a cache flush, cache-control bits 30 and 16 are cleared, the controller's command register `0x1000_3420` gets 0, and the code waits for its bit 15 to clear (up to 8,000 polls).
7. `0x1F0 = 0xA5A5_0000`: "parked".
8. The core **busy-polls `0x1F0`** until it reads `0xA5A5_0001`. There is no timeout and no `sleep` instruction.
9. On wake:
   - the clocks are restored: the saved frequency is set again, with PLL A and the regulator (@4f3c with 1);
   - the full SDRAM bring-up runs again (@147ca), and its probe saves and restores the two words it tests;
   - the old stack is restored;
   - `0x1F0 = 0xA5A5_0002` ("awake");
   - the GPIO words are restored and interrupts come back on.

**`videocore=0`, power-down** (@14244):
1. The same clock park (@4f3c with 3), and interrupts off.
2. `0x1F0 = 0`, then the SRAM stack, then `0x1F0 = 1`.
3. SDRAM: controller `+0x30 = 1` (the "shutdown" value `sdram.md` lists), then the same idle as step 6 above (@14316).
4. `0x1F0 = 2`.
5. The GPIO words are cleared.
6. `0x1F0 = 0xA5A5_0000`.
7. A `sleep` loop with interrupts off. Nothing but a power cut or a host restart ends it.

### The ARM side (`osos` 1.3)

[apple]
- The sender (host `0x100C_BF58`) formats `power_control videocore %d` with 1 or 0.
- A state machine (host `0x1028_92D4`) tracks four states in a byte at host `0x108C_BEC8`: 0 off, 1 hibernated, 2 booting, 3 running.
- Two pin helpers drive `GPO32` bit 14 (host `0x1028_7B90`) and **bit 15** (host `0x1028_7984`) through Apple's pin writer (host `0x1028_2CF0`, pins `0x76` and `0x77`, which it maps to `GPO32` bits 14 and 15).

| Transition | Host sequence [apple] |
|---|---|
| run → hibernate | `0x1F0 = 0`; send `power_control videocore 1`; poll `0x1F0` until `0xA5A5_0000`; **bit 15 = 1**; wait 5,000 µs; **bit 14 = 0** |
| hibernate → run | **bit 14 = 1**; wait 5,000 µs; **bit 15 = 0**; `0x1F0 = 0xA5A5_0001`; poll until `0x1F0 = 0xA5A5_0002` |
| run → off | send `power_control videocore 0`; poll until `0xA5A5_0000`; bit 15 = 0; bit 14 = 0 |
| cold power cycle (host `0x1026_611C`) | both pins low; 50 ms; bit 14 = 1, bit 15 = 0; 50 ms |

A separate three-mode helper (host `0x1026_668C`, and the same code in 1.2.1 at host `0x1025_C564`) gives the same pin orders:
- *off*: both pins low;
- *retain*: bit 15 high, then bit 14 low;
- *on*: bit 14 high, then bit 15 low.

**Nothing in `osos` 1.3 asks for hibernate.** The three static callers of the state machine ask for "running" or "off" (host `0x101A_FB9C`, `0x101A_FD3C`, `0x101B_0060`), and the three-mode helper is called only with off and on. So the hibernate path exists in both chips' code, but shipping firmware may never have used it [apple, static; a call through a pointer cannot be ruled out].

### What the bit 15 sequence most likely means

- Apple's wake path continues the parked core's busy-poll loop where it left off: the wake word is written into SRAM and the core answers. So **Apple expects the chip's registers and its SRAM to survive the time with bit 14 low, provided bit 15 went high first** [apple].
- Rockbox clears bit 14 alone, and the chip then needs a full bootstrap [rockbox; silicon every session]. (Whether SRAM actually goes blank after that cut has never been read back; every session simply uploads again.)
- Reading [hypothesis]: **bit 15 is the BCM's hibernate (retention) request**. With it high, the core supply drops to the brief's 0.6 V hibernation level, or the chip's state is otherwise held, while bit 14 removes the run supply or the run enable.
- Two other readings fit Apple's code as well:
  - bit 15 enters **bypass** or power-down mode (P1);
  - bit 14 is a clock or run enable rather than the supply.
- Stage PW5 tells them apart: after the sequence, does `0x1F0` still read `0xA5A5_0000`, and does a wake word get an answer?

**SDRAM in hibernate.** Apple idles the controller but sends no self-refresh command that we know of. The re-initialisation after waking saves and restores its probe words, which is pointless unless the rest of SDRAM was expected to hold. So either controller command 0 leaves the SDRAM self-refreshing, or Apple accepted losing it [hypothesis]. PW5 checks it.

### Leverage [estimate]

Today every dark spell ends in a cold start: bootstrap, upload of the image (about 34 ms unboosted for the 200 KB resident image), slots uploaded again, and the panel's 265 ms power-on. That is **0.37–0.62 s to the first frame** [silicon S7, S30] (`power.md` §2).

A hibernate wake would cost:
- 5 ms of pin sequence;
- the BCM's restore: 1 ms for PLL A, 10 ms more only when returning to 150 MHz, and about 1 ms of SDRAM bring-up;
- no upload: the image, its statics, the UI slots and the retained frames all stay in SRAM.

That is **about 7–17 ms**, roughly 25–90 times faster.

**Combined with the panel.** The panel keeps showing its own memory as long as its supply and reset line (BCM GPIO 48) hold. If the BCM's pins hold through hibernate (PW5 watches for this), then:
- the unlit clock face (`power-contract.md`, "The unlit face window") could sit in hibernate between its minute updates;
- the drift and the menus would come back in under 20 ms with nothing to re-upload.

This also replaces the standby states C3 and C4, which run the core at full voltage, with a state the vendor designed for retention. Its current is unknown (#12). The brief's figures (10 mW for a whole MPEG-4 encode) say the chip's active power is tens of milliwatts at most, so the gain is mostly **time and wake energy**, plus whatever the 0.6 V retention saves over standby [estimate].

## 4. Core voltage: the regulator and Apple's table

[apple] The supply level for a frequency comes from a 13-row table at `0x1F_A68` (@141ba). Each row gives an upper frequency and a level:

| Up to (Hz) | Level |
|---|---|
| 160,919,540 | 15 |
| 154,130,133 | 15 |
| 147,075,854 | 14 |
| 140,183,656 | 13 |
| 130,841,121 | 12 |
| 123,607,364 | 11 |
| 116,539,140 | 10 |
| 102,016,607 | 9 |
| 87,268,380 | 8 |
| 74,840,572 | 7 |
| 59,665,292 | 6 |
| 46,875,000 | 5 |
| (anything lower) | 5 |

- The lookup takes the first row whose frequency is at least the one wanted. If `sys19`'s low nibble is 4 or more, the level is forced to 15.
- The setter (@141f0) accepts levels 1–15. It writes `pw ((old & 0xF0) | level)` and waits 10 ms. Apple never asks for less than 5.
- On silicon:
  - a host-started image finds **`0xBB`: level 11**, at 27 MHz [silicon S7];
  - Apple's running image was read at **`0xB5`: level 5** [silicon S1].
- **Our images run the UI at 27 MHz on level 11**, six steps above what Apple's table and Apple's own running chip use for that clock (`quirks.md` #26).

**Leverage [estimate].** Core power scales with the square of the voltage. The brief gives 0.8–1.2 V for running. If levels 5–15 span that range evenly [hypothesis], level 11 is about 1.04 V and level 5 is 0.8 V. That is about **40 % less core power for every second the BCM is lit**, at no cost to what the user sees. The absolute figure waits on #12. During video, picking the PLL frequency per session (the lowest that keeps decode inside the frame time), with its table level, would save in the same proportion.

## 5. Clocks: PLLs, block clocks and Apple's rule

### PLL numbering

[apple] Apple's code numbers the PLLs 0–3 as **B, C, D, A** (@14912: index 3 is the core's PLL A at `+0x00`/`+0x14`).
- Each PLL has a divider word (`+0x00–0x0C`) and a control word (`+0x14–0x20`): `[9:0]` multiplier, bit 10 enable.
- Apple's rule (@4cb8):
  - divider M = **108**, so the reference is 27 MHz / 108 = **250 kHz**;
  - multiplier N = f_vco / 250 kHz;
  - the VCO is kept at **75 MHz or more**: for a lower clock the PLL runs at an integer multiple of the wanted frequency, and the clock word divides it back down.
- So a PLL can reach 255.75 MHz (N = 1023), which covers the brief's "up to 200 MHz" [estimate].

### The block clock word

[apple] @4d96, @4be6, @5334. Every clock word in `0x1000_1C28–0x58` (all pw) has one format:

| Bits | Field |
|---|---|
| `[7:0]` | Integer divider |
| `[10:8]` | Source: 0 none; 1 PLL B; 2 PLL C; 3 PLL D; 5 crystal (27 MHz); 7 PLL A. The core word `+0x28` and the `+0x58` word use 0 = crystal and 1 = PLL A or crystal respectively, as below |

This decodes the values already seen on silicon:
- `+0x4C = 0x051B` is the crystal ÷ 27, the 1 MHz counter [silicon S4];
- `+0x28 = 0x0101` is PLL A ÷ 1 [silicon S7].

The earlier reading of bits 8 and 10 as enables is withdrawn (`quirks.md` #25).

### Clock ids

[apple] @5334, @50a8, and the callers named.

| Apple's clock id | Word | User in Apple's code | Source and rate |
|---|---|---|---|
| 0 | `+0x2C` | none found | off |
| 1 | `+0x30` | TV out (@13c70) | crystal, 27 MHz or 9 MHz |
| 2 | `+0x34` | audio (@13c32) | PLL C, 24.576 MHz |
| 3 | `+0x38` | audio (@13c32) | PLL C, 2.048 MHz |
| 4 | `+0x40` | none found | off |
| 5 | `+0x44` | none found | off |
| 6 | `+0x3C` | USB: bit 15 is a gate; Apple clears it briefly to read the "inserted" bit, `0x1000_6400` bit 24 (@149c6, `usb_inserted`) | — |
| 7 | `+0x48` and `+0x50` (one value, two words) | none found | off |
| 8 | `+0x28` | the core | crystal or PLL A |
| 9 | `+0x58` | unknown; Apple writes `0x0101` at start-up | crystal |

- At initialisation (@50a8) Apple turns clocks 0–7 off, puts 8 and 9 on the crystal, and zeroes all four PLLs. A PLL is enabled only while some clock uses it: a user count at @4fae.
- The TV path asks the manager for a PLL of its own (@58fc).
- The audio ids answer half of open question #36: Apple's ids 2 and 3 are the words `+0x34` and `+0x38` (`audio.md`; the audio block itself is mapped in `brief-map-peripherals.md`).

**What the brief's "3 PLLs for external devices" means here** [hypothesis]: PLL A belongs to the core, and B, C and D are the three that make clocks for blocks and pins. 24.576 MHz is a classic codec master clock for the 48 kHz family.

**Leverage.**
- An image that needs exact audio clocks (the BCM's own audio path) can make them as Apple does.
- The clock-word format lets the census (PW1) say which blocks each image leaves clocked. A clock left running for nothing costs power while the panel is lit.

## 6. Caches

**What Apple does** [apple]:
- At start-up, flush and then enable (@296: flush @25a, enable @224, which sets bit 31 and keeps the rest).
- Around the SDRAM bring-up, clear bits 30 and 16, then set bit 16 at the end (`sdram.md`).
- At run time the whole cache is flushed in **two places only**:
  - after the dynamic linker has patched a loaded library's relocations (@88a0; the error string beside it is about bad symbol names);
  - when a library starts (@91a4).
- The flush is bounded: 4,096 polls, then a breakpoint.
- No per-line operation was found.
- Among the 32-bit constants in `vmcs.bin` there is no alias address for SRAM or SDRAM (no `0x4…`, `0x8…` or `0xC…` core view) [apple, static].

**What that implies** [hypothesis]:
- Apple's host rings and its statics live in SRAM (gp = `0x1_FC40`), and the ARM writes those rings while the cache is on. Yet Apple never flushes before reading them. So **SRAM is not cached**, or is kept coherent by hardware.
- Bit 16, set only once SDRAM is up, is then the "cache SDRAM" enable.
- Code patched in place needs a flush, which fits libraries that run from SDRAM.
- The brief's 128 Kbit caches would then be a front for the slower stacked SDRAM, which makes sense for a chip whose main memory is on-die.

**Leverage.** If PW2 confirms this, enabling `0x8001_0000` would speed up everything that runs from or reads SDRAM:
- the AAC decoder's far data;
- streams whose DPB lives in SDRAM;
- 3D textures and Cover Flow covers in SDRAM pools.

SRAM traffic and the host's SRAM rings would be unaffected. The bitstream FIFO in SDRAM, written by the host, would need a flush (or an uncached path) before each new packet is read. Until PW2 runs, our rule stands: cache off (`memory-access.md` §4).

## 7. The host interface: two banks, and the 3-bit address

[apple] `osos`'s port driver (host `0x1028_7800–0x1028_7DD0`):
- **Calls.** Read (host `0x1028_7A6C`), write (host `0x1028_7BE8`) and kick (host `0x1028_7BA4`, `CONTROL = 0x31`) each take a **bank** argument.
- **Locks.** Each bank has its own lock (a two-entry table at host `0x1082_358C`, both created at boot).
- **Bulk writes.** Writes of 256 bytes or more go by the PP's DMA to the bank's `DATA` register; the tail is written by halfword.
- **No write-ready wait.** Apple's write sets the two address halves and streams data without waiting for the write-ready bit. Rockbox waits for `CONTROL & 2` [rockbox]; whether the wait is needed is untested.
- **Who uses which bank.** The ALT bank (bank 1) carries the display driver's bulk pixel writes (host `0x1016_4524`, a rectangle; `0x1016_5024`, row copies) and one loader that alternates banks (host `0x100F_A21C`). Everything else uses the main bank: the bootstrap, the `0x1F0`/`0x1F8` mailbox, the cache flush, the start, and the gencmd rings.
- **Address space.** Both banks take full 32-bit BCM addresses, so they reach the same address space; each simply has its own address latch and data stream.

**Why two banks** (open question #25), answered from Apple's side: two software channels. A frame can stream through the ALT bank's latch while another task sends commands and reads replies through the main bank, without either re-sending the other's address or waiting on one shared lock [apple; the hardware independence of the latches is PW7].

**The "3-bit address"** [hypothesis, strong]: host address bits 16–18 select one of eight registers, the main and ALT banks' `DATA`, `WR_ADDR`, `RD_ADDR` and `CONTROL`. That is a 3-bit register address on a 16-bit data bus, as the brief says.

**Leverage.**
- Our driver uses only the main bank and re-sends the address for every access that isn't a stream.
- With PP2 (the COP) as the UI front end and PP1 (the CPU) running the player (`../research/bcm2722/power-contract.md`; the owner's core plan), giving each core **its own bank** would remove the shared latch and lock between them. This needs the COP's port access settled first: `data-paths.md` DP-6, PP open question #31.
- It also lets a frame upload continue while a command round trip completes, which today waits behind it.

## 8. Bus bypass

**The brief** [external]:
- The block diagram puts "Bus Bypass Mode" between the parallel host interface on one side and the display controller and the second external memory interface on the other.
- Those two drive the main LCD, a sub-LCD and an optional flash.
- In a phone this lets the baseband draw on the LCD while the multimedia chip is powered down: the chip's pads connect the host bus straight to the LCD bus.

**What we found:**
- `osos` never reaches a panel except through the BCM's port. The PP's own LCD bridges (`0x7000_3000`, `0x7000_8A00`) are not used for a panel; `0x7000_3000` appears once, in early start-up, clearing an unrelated bit. The class list names an `LcdDriverSpHw` beside `LcdDriverVC02`, but no code path we found drives a panel through the PP's hardware [apple].
- `vmcs.bin` writes no register that could be a bypass enable among the blocks it touches [apple].
- The only BCM control the host has besides power is `GPO32` bit 15 (§3).

So bypass is **not used by Apple**, and how it is entered on this board is unknown. Three candidates [hypothesis]:
1. a pin: bit 15, alone or with bit 14 low;
2. automatic whenever the core is powered down and the pads stay powered;
3. a register we have not found (for example in the bootstrap bytes, #24).

**Leverage, if it works.** Today the BCM must be powered whenever the panel is lit: states C1 and C2 (`power-contract.md`). With bypass, a static lit screen could be drawn by the PP straight onto the panel's own memory while the BCM is powered down:
- a menu that isn't moving;
- the unlit face's minute update;
- the "now playing" screen between artwork changes.

A full 153,600-byte frame through the port takes about 27 ms unboosted [silicon S8], which is fine for screens that change rarely. Animation (the drift, Cover Flow) would still wake the BCM. The saving is the BCM's whole lit-idle current, unmeasured (#12). This is the biggest possible gain while the screen is lit, and the least certain.

## 9. Device stages

Each stage is written to be built as a Developer › Advanced › Video chip › Probes row in the bench style of `../research/bcm2722/bench-p19.md`:
- one row per stage;
- a stage 5 pass flag needed;
- lines into `bcmtest.log`, and a summary screen.

They are **ranked safest first**. Wave 1 only reads, or times loops on the chip with Apple's own register values. Wave 2 changes the core voltage and clocks to Apple's table values. Wave 3 drives a host pin that Rockbox has never driven.

**General rules for every stage** [ours]:
- **Port waits.** Every port wait is bounded (1 ms per handshake, 100–200 ms per step), and a dead port (`0xFFFF` halfwords) aborts the stage.
- **Recovery.** Every abort, and every stage's end, is the standard recovery: `GPO32` bit 15 = 0 (wave 3 only), bit 14 = 0, wait 100 ms, then the normal wake path (`bcm_init()`, our boot ladder or Apple's NOR `vmcs`).
- **Nothing persists.** All BCM state is volatile, and nothing touches the NOR flash, the disk (apart from the logs) or the panel's settings beyond what the panel scripts already do.
- **GPO32 discipline.** Read-modify-write under interrupts off, and change only bits 14 and 15. Never change `GPO32_ENABLE`. Rockbox's LCD init already enables bits 14 and 15 as outputs; the stage refuses to run if `GPO32_ENABLE & 0xC000 != 0xC000`.
- **Never written, in any stage:** the regulator below level 5 or above 15; the regulator's bits `[7:4]` (always keep what was read); a PLL other than through Apple's rule; any `0x1000_xxxx` register not named in the stage; `GPO32` with bit 15 = 1 and bit 14 = 1 for longer than the 5 ms of Apple's sequence.

### Wave 1: read-only and timing

**Built** (branch `p19-pb-probes`; first run on silicon 2026-10-25, build A: PW1, PW2 and PW7 FAIL, PW8 PASS, results under each stage below): PW1, PW2, PW7 and PW8 as Developer › Advanced › Video chip › Probes rows (`../research/bcm2722/bench-p19.md`, "The brief probes", has the rows, the lines and the deviations); vc2sim checks for PW2 and PW8 in `bcmfw/tests/peek_test.py`. PW3 and PW4 are built on both sides (branch `p19-pw34`, not run; below and `bench-p19.md`, "The power probes"). PW5 is built on both sides and PW6 read only (branch `p19-pw56`, not run; below and `bench-p19.md`, "The hibernate probes").

#### PW1: register census (read-only) · **built; run 2026-10-25: FAIL (2 contexts)**

**Decides:**
- which block clocks and PLLs each image leaves running (P6);
- the regulator level found in each context (P3, P4);
- the GPO32 pins as Apple's boot leaves them (P2);
- whether Apple's NOR `vmcs` scales its clock (P7).

**BCM side:** nothing new. It runs in three contexts:
- **A:** Apple's NOR `vmcs` running, just after stage 0;
- **B:** our `hello_dma` (or the resident image) idle at 27 MHz, just after stage 5;
- **C:** optional, inside stage 4b's 150 MHz image.

**PP side**, in order, each read a bounded `bcm_read32()`:
1. `0x1000_1400`.
2. `0x1000_1C00` to `0x1000_1C58`, every word (23 reads; `+0x10` and `+0x24` are unknown words, read only).
3. `0x1000_0C00`; `0x1000_0400`; `0x1000_0408`.
4. `0x1000_3410`, `3414`, `3420`, `3424`, `3428`, `3430`.
5. `0x1000_1804` twice, 1,000 µs apart on the PP's microsecond timer.
6. PP registers: `0x7000_0080` (GPO32_VAL), `0x7000_0084` (GPO32_ENABLE), `0x7000_0088` (read-back), `0x7000_0008` (STRAP_OPT_A), `0x7000_0040`.
7. Context A only: 200 samples, every 10 ms, of `0x1000_1400`, `0x1000_1C28` and `0x1000_1C14`. The first second with the UI idle, then a scripted menu scroll for one second.

**Never read** (side effects possible): `0x1000_400C`, `0x1000_4804`, `0x1000_2C04` (FIFOs), `0x1000_3800` (the doorbell acknowledge), `0x1000_6400`, and any DMA `+0x04`.

**Log** (`pw1`):
- `pw1 A reg 10001400 = 000000B5 (level 5, hi B)`;
- `pw1 A clk +34 = 00000000 src 0 div 0 (off)`, one line per clock word, decoded by §5;
- `pw1 A pll A m=0000006C n=00000658 on f=150.0 MHz`;
- `pw1 A ctr delta 1000 us`;
- `pw1 gpo32 val=… en=… rb=…`;
- `pw1 A samples: level 5..5, sel 0101..0101, pllA 0658..0658`;
- `pw1: PASS (3 contexts)`.

**Pass:** every read completed and the counter delta is 1,000 ± 5 µs in contexts B and C. A census has no wrong answers. Fail only on a dead port or a timeout.

**Safety:** reads only. No owner go beyond the session's.

**vc2sim:** no (the values are the silicon's). The clock-word decoder is unit-testable on the host.

**On silicon, 2026-10-25 (build A, session log `bcmtest.log`, the stage's own lines)** [silicon]: FAIL, 2 contexts, because the counter and the port/core comparison missed the pass rule, not because a read failed.
- **Counter:** 992 µs in 1,000 (context A) and 991 µs (context B), outside the 1,000 ± 5 rule. The PP timer's bound on a port read is a [hypothesis] for the short count.
- **Port against PEEK:** one register differs, `0x1000_0408` (port read 0, the core's own read `0x4000`, context B).
- **Context A (Apple's `vmcs`):** regulator level 5 (`0xB5`); PLL A m = `0x6C`, n = `0x1544`, on, 81.0 MHz; the other three PLLs off. Clock words: `+28` = `0x103` (source 1, divider 3); `+4C` = `0x51B` (crystal, divider 27); `+58` = `0x138` (divider 56); `+2C`–`+48`, `+50`, `+54` all 0 (off). Idle samples over 2 s: the level, `+28` and PLL A's n each stayed constant.
- **Context B (our video image, idle at 27 MHz):** regulator level 11 (`0xBB`); the clock words `+28` to `+48` read 1 (crystal, divider 1; `+3C` also has bit 15 set, `0x8001`), `+4C` and `+58` as in A; all four PLLs read m = n = 1, off, 27 MHz.

#### PW2: caches, timing and coherence · **built** (`HP_OP_CACHE`); **run 2026-10-25: FAIL (-3)**

**Decides:** P8 (#59, #6): whether the caches speed up SRAM, SDRAM or both, and what the host sees.

**BCM side**, a lab op in the bench video image (`HP_OP_LAB` family, lab `pw2`), with a session open at 27 MHz (SDRAM up), and the cache register first reading `0x0001_0000` (ours). For each setting cc in `0x0000_0000`, `0x8000_0000`, `0x8001_0000`:
1. Flush exactly as Apple does:
   - `0x1000_0C00 = read | 0x4000_0000`;
   - poll bit 0, up to 4,096 reads;
   - `0x1000_0C00 = cc`.
2. **SRAM, fits:** 262,144 word loads over 16 KB at `0x12_B000`; a cold pass, then a warm pass. Time each on `0x1000_1804`.
3. **SDRAM, fits:** the same over 16 KB at `0xFE3F_6000`.
4. **SDRAM, too big:** 262,144 loads at a 32-byte stride over 1 MB at `0xFE10_0000`.
5. **Coherence, SRAM:**
   - the image lays V0 at `0x12_BFF0` and loads it, then posts step 1;
   - the host writes V1 there and posts step 2;
   - the image loads again and records V0 or V1;
   - the image stores V2 at `0x12_BFF4` and posts step 3;
   - the host reads it and logs what it saw;
   - the image flushes and posts step 4;
   - the host reads again.
6. **Coherence, SDRAM:** the same at `0xFE3F_FFF0` (host `0xC03F_FFF0`).

Afterwards: flush, then `0x1000_0C00 = 0x0001_0000`.

**PP side:** run the handshakes of steps 5 and 6 through the mailbox step word, 100 ms per step; read the results.

**Log** (`pw2`):
- `pw2 cc=80010000 SRAM16K cold N us warm M us`;
- the same for `SDRAM16K` and `SDRAM1M`;
- `pw2 coh cc=… SRAM core-after-host fresh|stale, host-after-core fresh|stale, after-flush fresh|stale`;
- `pw2: PASS|FAIL`.

**Pass:** every loop's checksum matches its model under all three settings. Timing and coherence are results, not pass criteria. A checksum that differs is a FAIL: the cache corrupted data. The stage then cuts.

**Safety:** only `0x1000_0C00`, only Apple's values, restored at the end. No extra go.

**vc2sim:** the flow, the checksums and the handshake (vc2sim has no cache, so it always reports "fresh"). Run it before the device.

**On silicon, 2026-10-25 (build A, session log `bcmtest.log`)** [silicon]: FAIL, error code -3. The log has no step line, so which step failed is **open**; the cache question (P8, #59) stays unanswered.

#### PW7: the ALT bank · **built; run 2026-10-25: FAIL** (the ALT write at `0x12_C400`, the area the read-back expects)

**Decides:** P14, P16 (#25, #43): whether the two banks' latches are independent, and whether the ALT bank needs the write-ready wait.

**BCM side:** any of our images, idle. Apple's `vmcs` must not be running.

**PP side:**
1. **Main bank write:** `WR_ADDR32 = 0x0012_C000`; wait `CONTROL & 2` (1 ms); 256 halfwords `0xA000 | i` to `DATA`.
2. **ALT bank write:**
   - `ALT_WR_ADDR` (`0x3005_0000`) = `0xC000`, then `0x0012` (low half first);
   - poll `ALT_CONTROL` (`0x3007_0000`) & 2 for 1 ms, and log whether it ever set (Apple doesn't wait);
   - 256 halfwords `0xB000 | i` to `ALT_DATA` (`0x3004_0000`) at `0x12_C400`.
3. **Interleaved:**
   - `WR_ADDR32 = 0x12_C800` and `ALT_WR_ADDR32 = 0x12_CC00`;
   - then 256 rounds of `DATA = 0xC000 | i`, `ALT_DATA = 0xD000 | i`.
4. **Read back** all four 512-byte areas:
   - by the main bank (`read32`);
   - by the ALT bank (`ALT_RD_ADDR` `0x3006_0000`, then wait `ALT_CONTROL & 0x10` per word, as Apple's read does).
5. **Rate:** 153,600 bytes to `0x0E_0000` (the host frame, the host's own area) through `ALT_DATA`, then through `DATA`, with the 2-register `stm` loop; µs each.

**Log** (`pw7`):
- `pw7 main N/256, alt N/256, interleave main N/256 alt N/256, alt read N/256, alt ready seen yes|no, frame main X us alt Y us`;
- `pw7: PASS|FAIL`.

**Pass:** all four areas read back right by both banks.

**Safety:** writes only into `0x12_C000–0x12_CFFF` (free SRAM in our map) and the host frame. No `CONTROL` writes. No extra go.

**vc2sim:** not modelled (the devtest host model has one bank). Silicon only.

**On silicon, 2026-10-25 (build A, session log `bcmtest.log`)** [silicon]: FAIL at step 0. Reads by the main bank returned 256 of 256 four times; reads by the ALT bank returned 18, 0, 0 and 0. The ALT bank's ready flag was seen. A frame took 9,581 µs through the main bank and 9,632 µs through the ALT bank. So the ALT bank answered (ready seen, frame time equal to the main bank's) but its reads came back wrong; the cause (the address latches, or the read-back area `0x12_C400`) is **open** [hypothesis]. Open question #25 stays open.

#### PW8: DMA memcpy · **built; run 2026-10-25: PASS** (`HP_OP_DMACPY`; 16 KB copies, destination `0x12_F000`: 64 KB would overlap the source, the parked AAC tables and SDRAM's end)

**Decides:** P10, `dma.md`'s open "DMA from and to SDRAM", and #13: the engine's SRAM ↔ SDRAM rate against the core's.

**BCM side**, a lab op:
- Channel 7, sub-channel 1, as Apple's `dma_memcpy` uses it.
- Control block `{0, src, dst, units, 0x0102_5151, 0, 0, 0}`, with 32-byte units.
- Channel `+0x04 = 0xFFFF_FFFF`, then the sub-channel's start register `+0x58` (`+0x38 + 0x20 × 1`) = the block's address.
- Wait for status bit 14 (sub-channel 1's chain end: 13 + k), 100 ms bound.
- Three 64 KB copies, each source filled with a seeded pattern and each destination with another, then compared word by word:
  - SRAM `0x12_B000` → SRAM `0x13_4A00`;
  - SRAM → SDRAM `0xFE3F_6000`;
  - SDRAM → SRAM.
- The same three copies by the core's word loop, timed.

**PP side:** start the op and read the results.

**Log** (`pw8`):
- `pw8 SRAM>SRAM dma N us core M us, wrong W`;
- the same for `SRAM>SDRAM` and `SDRAM>SRAM`;
- `pw8: PASS|FAIL`.

**Pass:** 0 wrong words in every copy.

**Safety:** channel 7 is unused by our images (1 is the LCD, 5 audio). Transfers only between our free regions. No extra go.

**vc2sim:** yes. Its DMA model runs 1-D blocks, so correctness can be checked before the device; the rates are silicon only.

### Wave 2: Apple's voltage and clock values (owner's explicit go)

**On silicon, 2026-10-25 (build A, session log `bcmtest.log`)** [silicon]: PASS, 0 wrong words in all three directions (16 KB, channel 7). Engine time against the core's own copy:

| Direction | DMA | Core |
|---|---|---|
| SRAM to SRAM | 65 µs | 1,065 µs |
| SRAM to SDRAM | 383 µs | 1,497 µs |
| SDRAM to SRAM | 575 µs | 1,032 µs |

The DMA copy is 16 times faster than the core in SRAM, 3.9 times to SDRAM and 1.8 times from SDRAM. Status read `0x4000` each time.

#### PW3: core voltage at 27 MHz · **built, not run**

**Decides:** P3, P4 (#11): whether our UI image holds at level 5 at 27 MHz, as Apple's chip did (`0xB5`, S1), and what it saves.

**BCM side**, an op in the bench image, at 27 MHz (on the crystal; refuse if `0x1000_1C28 != 0x0001`):
1. Read R0 = `0x1000_1400`, expected `0xBB`.
2. For L in 11, 9, 7, 5:
   1. `0x1000_1400 = 0xA5A5_0000 | (R0 & 0xF0) | L`.
   2. Wait 10,000 µs on `0x1000_1804`.
   3. Run the self-check:
      - the 69 timing kernels' checksums (stage 4's set);
      - 64 KB SRAM pattern write and read at `0x12_B000`;
      - 64 KB SDRAM pattern write and read at `0xFE3F_0000`, if SDRAM is up;
      - one full colour-bar frame by LCD DMA.
   4. Post the result for L.
   5. Hold 20 s while the host marks the power log `pb3_L` for a meter reading.
3. Write `0x1000_1400 = 0xA5A5_0000 | (R0 & 0xFF)`, and wait 10 ms.

**PP side:** start; for each L, wait for the posted result (deadline 2 s), add the power-log mark, and log.

**Log** (`pw3`):
- `pw3 L=5 reg 000000B5 kernels 69/69 sram ok sdram ok frame ok`;
- `pw3: PASS|FAIL at L`.

**Pass:** everything right at L = 5. The photo of each frame matches the first.

**Fail and recovery:** any wrong check at level L: restore R0 at once, then the standard cut. Log the lowest good level.

**Never:** a level below 5; this op at 150 MHz; bits `[7:4]` changed.

**Owner go:** yes (core voltage). Apple's own image ran this unit's chip at level 5 [silicon S1], so the risk is a hang that the cut clears, not damage.

**The production lever (p19-battery) [ours]:** the PP writes level 5 into `0x1000_1400` itself, through the port, for the resident merged image at its swap and every wake (setting "Video chip core level 5", default off), only while `0x1000_1C28`'s source field reads the crystal; the image's `clock_fast()`/`clock_slow()` keep it across a video session. No image change. The bench row `lvl5` A/Bs 11 against 5 with a meter and the job self-test at 5 (`../research/bcm2722/battery-levers.md`). Not run on silicon.

**vc2sim:** flow only (no voltage model).

#### PW4: Apple's clock ladder · **built, not run**

**Decides:** P6, P7, and #4 (does the LCD bus follow the core?): our kernels at Apple's intermediate clocks and levels, for picking a video session's clock.

**BCM side**, the 4b bench image extended. For f in 46, 74, 102 and 150 MHz:
1. Level from the table in §4.
2. If raising: regulator first (`pw (old & 0xF0) | level`), then 10 ms.
3. PLL A by Apple's rule:
   - `+0x00 = pw 0x6C`;
   - `+0x14 = pw (N)`, with N = vco / 250 kHz and the VCO ≥ 75 MHz (46 MHz: vco 92 MHz, N 368, clock divider 2);
   - wait 1 ms;
   - `+0x14 = pw (N | 0x400)`.
4. `+0x28 = pw (0x0100 | divider)`.
5. If lowering: regulator after.
6. Set the SDRAM refresh to f / 128,000.
7. Run the timing kernels; time one LCD frame push with the timing words recomputed for f.

Back to 27 MHz by our switch-down (`boot-and-clocks.md` §4).

**Log** (`pw4`): `pw4 f=74 level 7 kernels 69/69 x2.74 frame N us`.

**Pass:** checksums right at every point, and the kernel speed proportional to f within 1 %.

**Owner go:** yes, as stage 4b today.

**vc2sim:** flow, and the PLL arithmetic against Apple's code (`bench_test` already compares the 150 MHz writes with `vmcs.bin`).

**P19 PW4, the production side (branch `p19-video-dvfs`):** the ladder became twelve level bands (the crystal, then the top whole MHz of each of §4's level bands at its level) switched by `clock.c` `clock_band` in the video images (`HP_OP_VCLK`), with a speed check and a fallback to the crystal, and a PP-side governor (`apps/video/vclk.c`); the bench row `vbv` verifies each band on the unit. Built, not run; the merged image's change waits for the next batch (`../research/bcm2722/battery-levers.md` §4, `boot-and-clocks.md` §4).

#### How PW3 and PW4 were built (branch `p19-pw34`)

Both run through one probe op, `HP_OP_PW` (`0x18`), in the plain video image only (`bcmfw/hostproto.c` `cmd_pw`, request and reply in `hostproto.h`), and two Developer › Advanced › Video chip › Probes rows (`apps/ipodui/devbcm_probes.c`, logic in `devbcm_pw.h`). The merged image still answers `HP_E_UNKNOWN` and is byte-identical. The op follows `HP_OP_TV`'s pattern: a one-time code, a per-step allowlist, a restore with read-back on every exit, a progress word. Owner steps, lines and what each outcome means for battery life: `../research/bcm2722/bench-p19.md`, "The power probes".

**Kept from the spec:** Apple's values only (the levels from @141ba, PLL A by @4cb8's rule, the refresh `f / 128,000` from @4d18, @5134's order: a higher level before the clock, a lower one after, 10 ms each); the ladder 46/74/102/150 MHz at levels 5/7/9/15; the 20 s hold per PW3 level; never below level 5, never bits `[7:4]` changed, never PW3 at 150 MHz.

**Where it departs** [ours]:
- **One request per level or point**, each returning to the start's clock, level and refresh, read back, before it replies; the spec's single op over the whole ladder would keep a changed state across requests.
- **The self-check replaces the stage 4 kernels, the SDRAM pattern and the colour-bar frame** at every point: the 69 kernels aren't in the video image. 32 KB of SRAM at `0x12_B000`, not 64 KB (64 KB reaches the parked AAC tables at `0x13_4000`), filled by the core with two patterns by turns and summed by the core and by the vector unit (bench row SUMR's three instructions, the only vector form used, checked on silicon in session 4), against references taken at the start level. It runs over and over through the hold.
- **The frame** is pushed at the 150 MHz point only, with Apple's 150 MHz bus words (the image has no way to recompute timing words for an arbitrary clock); the lower points touch no LCD register.
- **The refresh** follows the clock only with SDRAM up: the PW4 row opens and closes a session first. Without SDRAM the refresh isn't written (flag `HP_PW_F_SDRAM` clear).
- **The entry check** takes the core word's source field (bits `[10:8]` = 0, the crystal) rather than `0x0001` exactly: the word's value after a host start hasn't been read on silicon (PW1 reads it), and the restore writes back whatever was found.
- **PW4 needs stage 4b's pass flag** (PLL A proven on this unit) and PW3's pass in the same boot. Its speed reading is logged, not a pass condition: the self-check is memory-bound.
- **The waits** end on a stopped counter (1,024 identical reads) instead of a spin count, so a stall is told from a slow clock.

**[sim] findings** (`bcmfw/tests/peek_test.py --only pw`, 395 cases): at every point and on every exit path, the level stays at or above Apple's table for the clock running and the refresh interval never exceeds 7.8 µs, checked after each single write; every register written comes back; the mutants that raise the level after the clock or skip the restore are caught. vc2sim has no voltage model: whether the chip holds at each level is silicon's question.

### Wave 3: the host pin `GPO32` bit 15 (owner's explicit go, each step)

#### PW5: hibernate, Apple's sequence · **built, not run**

**Decides:** P2, P3, P9 (#50, #57), and what bit 15 is:
- does the chip keep its registers and SRAM with bit 15 high and bit 14 low;
- does SDRAM hold;
- does the panel keep its picture;
- the wake time;
- the current while held.

**BCM side**, a new op `HP_OP_HIBERNATE` in the resident or bench image, a replica of Apple's `videocore=1` (§3):
1. Precondition: 27 MHz. If at 150 MHz, our switch-down first.
2. Lay patterns:
   - SRAM `0x12_B000–0x13_FFFF` (84 KB) with seed S1;
   - SDRAM `0xFE3F_0000–0xFE3F_FFFF` (64 KB) with seed S2;
   - CRCs of both, and of the host frame at `0xE0000`, into mailbox words *pre*.
3. Colour bars on the panel (look 1).
4. `di`.
5. PLLs off, in Apple's start-up order (B, C, D, then A): divider and control `= pw 0` (`0x1000_1C04/18`, `0C/20`… as in `boot-and-clocks.md` §4 step 4). The core is already on the crystal.
6. Save `0x1000_2828–0x1000_285C` (14 words) to SRAM, then write 0 to each.
7. SDRAM idle, as Apple does:
   - flush;
   - `0x1000_0C00 &= 0xBFFE_FFFF`;
   - `0x1000_3420 = 0`;
   - poll bit 15 clear, up to 8,000 reads.
8. `0x1F0 = 0xA5A5_0000`.
9. Busy-poll `0x1F0` for `0xA5A5_0001`, as Apple does: no timeout, no `sleep`. The host's deadlines bound it.
10. On the wake word:
    - SDRAM bring-up (ours, the same probe-save as Apple's);
    - CRCs again into words *post*;
    - restore the 14 GPIO words;
    - `0x1F0 = 0xA5A5_0002`;
    - `ei`.

**PP side**, each step logged with the microsecond timer:
1. Check `GPO32_ENABLE & 0xC000 == 0xC000`, else refuse.
2. Write `0x1F0 = 0` and send `HP_OP_HIBERNATE`. Poll `0x1F0` for `0xA5A5_0000`, deadline 100 ms.
3. `GPO32_VAL |= 0x8000` (bit 15).
4. `udelay(5000)`.
5. `GPO32_VAL &= ~0x4000` (bit 14).
6. Hold, with power-log mark `pb5_hold`. **No port access during the hold.** Holds: 1 s, then (on a pass) 10 s, then 60 s. The owner watches the panel: are the bars still shown?
7. `GPO32_VAL |= 0x4000`.
8. `udelay(5000)`.
9. `GPO32_VAL &= ~0x8000`.
10. Read `0x1F0`, bounded. **The key result**:
    - `0xA5A5_0000` means SRAM and the parked core held;
    - `0xFFFF` halfwords, or the bootstrap's `ALT_CONTROL` state, mean the chip restarted from its boot ROM;
    - anything else means SRAM was lost.
11. If it read `0xA5A5_0000`: write `0x1F0 = 0xA5A5_0001`, poll for `0xA5A5_0002` (deadline 200 ms), then read *pre* and *post*.
12. Push a frame (look 2).

**Log** (`pw5`):
- `pw5 park N us, hold T s, 1F0 after = XXXXXXXX, wake M us, sram pre/post A/B, sdram pre/post C/D, frame pre/post E/F`;
- `pw5: PASS|FAIL`.

**Pass:** `0x1F0` held, the image answered within 200 ms, and the SRAM CRC is unchanged. SDRAM and the panel's picture are reported, not required.

**Fail and recovery:** any deadline: bit 15 = 0, bit 14 = 0, wait 100 ms, then the normal wake (cold boot). The stage asserts and logs `GPO32_VAL & 0xC000 == 0x4000` at its end.

**Never:**
- bit 15 high with bit 14 high, except inside Apple's 5 ms windows;
- bit 15 set from a state where bit 14 is already low;
- any port access between steps 3 and 9.

**Owner go:** yes, explicitly, for steps 3–9. This drives a pin Rockbox has never driven. Apple's code does drive it, in this order, but Apple's 1.3 firmware has no static caller of the path (§3), so it may never have run on a shipping unit. The worst plausible outcome is a chip that needs the cut and a cold boot.

**vc2sim:** the BCM side (park, poll, wake, CRCs, GPIO save and restore) with a host script writing `0x1F0`. The pin's effect is silicon only.

#### PW6: the bypass probe · **built read only, not run** (no write into a powered-down port: below)

**Decides:** P1, P11 (#56): whether host writes reach the panel in any powered-down state.

**Needs PW5 run first**, so that bit 15's meaning is known.

**BCM side:**
1. Fill the panel white (one frame).
2. Set the window to 16 × 16 at (0, 0): panel 3 writes `0x210–0x213` and `0x200/0x201`, then index `0x202` (RAM write), per `lcd.md` §4. Leave the panel expecting pixel data.
3. Park as in PW5, steps 4–9, without the patterns.

**PP side**, variants run in this order, each followed by a photograph, then the standard cut and a cold boot:
1. **B1, control:** BCM parked, pins unchanged (bit 14 = 1, bit 15 = 0).
   - Read `DATA` (`0x3000_0000`) 4 times and log the values.
   - `WR_ADDR32 = 0x12_D000`; write 256 × `0x0000` to `DATA`. That lands in free SRAM; expect no change on the panel.
2. **B2, power-down:** bit 14 = 0 only (Rockbox's cut, with the panel left on).
   - Wait 100 ms. Photograph: did the bars and white survive the cut?
   - Read `DATA` 4 times (Rockbox already reads an unpowered port).
   - Write 256 × `0x0000` to `DATA`. Photograph.
   - Repeat with `ALT_DATA` (`0x3004_0000`).
3. **B3, Apple's retain state:** bit 15 = 1, 5 ms, bit 14 = 0, as in PW5.
   - The same reads, the 256 writes to `DATA`, then to `ALT_DATA`; a photograph after each.
   - Then bit 14 = 1, 5 ms, bit 15 = 0, before the cut.

**Why `0x0000`:**
- If a write reaches the panel as data, a black 16 × 16 square appears on the white.
- If it reaches the panel as an index, an index write of 0 is what Apple's own bus resync sends six times at every power-on (`lcd.md` §4), so it is harmless.
- Nothing else is written; in particular nothing goes to `WR_ADDR`, `RD_ADDR` or `CONTROL` in states B2 and B3.

**Log** (`pw6`):
- `pw6 B2 data reads XXXX XXXX XXXX XXXX, wrote 256 to DATA`;
- `pw6: owner looks recorded`.

**Pass:** a black square in B2 or B3 means bypass works in that state, and with which register. No square anywhere means no bypass through these registers in these states. That is a result, not a fault.

**Owner go:** yes, explicitly, for B2 and B3.

**Risk:** driving the port's data lines while the BCM's core is unpowered. Rockbox's dead-port check already drives the address and strobe lines into an unpowered chip; this adds 256 data cycles, about 0.1 ms. Back-powering through the pads is the theoretical risk, and is why the count is kept this small.

**vc2sim:** no.

#### How PW5 and PW6 were built (branch `p19-pw56`)

PW5 runs through one probe op, `HP_OP_HIB` (`0x19`), in the plain video image only (`bcmfw/hostproto.c` `cmd_hib`, with `video_sdram_idle` and `video_sdram_rewake` in `bcmfw/video/video.c`), the PP's pin sequence `bcmdt_hib_cycle` (`firmware/target/arm/ipod/video/bcm-devtest-core.c`), and two Developer › Advanced › Video chip › Probes rows (`apps/ipodui/devbcm_probes.c`, logic in `devbcm_hib.h`). The merged image still answers `HP_E_UNKNOWN` and is byte-identical. The op follows `HP_OP_TV`'s and `HP_OP_PW`'s pattern: a one-time code, a per-step allowlist, a restore with read-back on every exit, a progress word. Owner steps, lines and what each outcome means: `../research/bcm2722/bench-p19.md`, "The hibernate probes".

**Kept from the spec:** Apple's park order (PLLs B, C, D, then A at `pw 0`; the 14 GPIO event words saved and cleared; SDRAM idled as @14892 with its 8,000-poll bound; `0x1F0 = 0xA5A5_0000`; a busy poll with no `sleep`), Apple's wake (PLLs, SDRAM brought up again with the probe words kept, the event words, `0xA5A5_0002`), Apple's pin order and 5,000 µs waits (host `0x1028_92D4`, `0x1026_668C`), no port access between bit 15 high and bit 15 low, the standard cut on any failure after the first pin, `GPO32_ENABLE` checked and never written, the holds of 1 s, 10 s and 60 s, the pass rule.

**Where it departs** [ours]:
- **The windows:** 32 KB of SRAM at `0x12_B000` (the spec's 84 KB reaches the parked AAC tables at `0x13_4000`) and 32 KB of SDRAM at `0xFE3F_6000` (the spec's `0xFE3F_0000` overlaps the self-test dumps). Each is checked word by word against its seed after the wake: wrong words and the first index, not CRCs. The host frame is summed before and after.
- **The image bounds its poll:** 90 s on the 1 MHz counter (the request's word), and a counter that reads the same 1,024 times ends it; Apple's poll has no bound. Either way the image restores everything and writes `0xA5A5_00FF` ("gave up").
- **No switch-down:** the image refuses unless the core is already on the crystal, so Apple's core-word write is never needed.
- **SDRAM only when up:** without an `OPEN` it is never idled or brought up; the row opens and closes a session first.
- **Interrupts:** our image runs with them off outside its idle sleep, so Apple's `di`/`ei` have nothing to do.
- **The PLLs come back by Apple's rule** (@4cb8): each divider, its control without the enable, 1 ms, then the enables.
- **Look 2 is the bars mirrored**, so a picture the panel only kept is told from one the image pushed after the wake.
- **The park bound is 300 ms**, not 100 ms: the patterns, the bars and their push come first. The wake's bounds are 200 ms for the word to answer and 200 ms more for `0xA5A5_0002`.
- **Not built:** a read of `ALT_CONTROL` to recognise the boot ROM (all ones on `0x1F0` is taken as the restart); the spec's "pre" and "post" words in the mailbox (the reply carries them).
- **PW6 is read only.** There is no register evidence for how bypass is entered (§8), so nothing is written into the port of a powered-down chip: B1 (the image idle) and B2 (bit 14 low alone, Rockbox's cut) read `DATA`, `CONTROL`, `ALT_DATA` and `ALT_CONTROL` raw, with no address written, while the owner photographs what the panel keeps with the chip cut. The 256-word writes and B3 (reads in the retain state) are not built; B3's look is PW5's hold. If the writes are approved later, the 16 × 16 window comes from the image's own `lcd_update_rect`, with no change to `lcd.c`.
- **Locks, this boot only:** PW5 opens once PW1 passed; PW6 once PW5 drove its pins (whatever it found).

**[sim] findings:**
- `bcmfw/tests/peek_test.py --only hib` (243 cases): a park writes Apple's order exactly and nothing else; while parked the core reads only the counter; every saved word comes back after a wake, after the image's own bound, after a counter stopped while parked and after a stuck SDRAM idle; a word spoiled in each window while parked is reported with its index; the bars reach the panel and come back mirrored.
- `bcmfw/tests/devtest_test.py --only t_hib` (57 cases): `bcmdt_hib_cycle` against the shipped image with a model of the two pins: both pins high only for Apple's two 5 ms windows, no port access while bit 15 is high, the other `GPO32` bits kept; a chip that cuts anyway, scrambles SRAM, never resumes or loses its port each ends in the standard cut, with no port access after it.
- vc2sim models neither the pins nor retention, so whether bit 15 retains, what the counter does in the dark and what the panel keeps are silicon's questions. Its times (park about 17 ms, wake about 5.3 ms of which 5 ms is the pin wait) are a flow check only.

### Order for a session (proposed)

1. PW1, PW2, PW7, PW8 (wave 1) with the normal session go.
2. Then PW3 and PW4 with a go for wave 2.
3. Then PW5 with its own go.
4. Then PW6 only if PW5 has run and the owner agrees.

A USB-C meter between a plain charger and the iPod, battery full, turns PW1's contexts, PW3's levels and PW5's hold into the first current figures (#12).

## 10. What changed elsewhere in the manual

- `power.md`: the power manager, `GPO32` bit 15, and the hibernate sequence, as pointers here.
- `boot-and-clocks.md`:
  - §3, the clock-word format;
  - §4, Apple's clock ids, PLL rule and DVFS;
  - §5, Apple's own power-down and hibernate.
- `quirks.md` #25 (clock-word bits) and #26 (the regulator level after a host start).
- `open-questions.md` #56, #57 and #59, and #25 and #36 in part.
- `host-interface.md` §1 (why two banks).
- `../data-paths.md`: the host port's two banks, cell 5 (bypass), and the BCM caches row.
- `../pp5022-reference/gpio-i2c.md`: `GPO32` bit 15.
