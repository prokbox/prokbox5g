# 9. LCD interface and panels

The BCM2722 drives the iPod Video's 320×240 panel through an "MPU"-style (8080-type) parallel interface: an index (command) register, a data register, programmable bus timing, and an automatic pixel-run mode that a DMA channel feeds. Four panel controllers are fitted across iPod Video production; the chip reads which one from two GPIO pins.

## 1. Interface registers (`0x1000_4000`)

| Offset | Register | Use | Evidence |
|---|---|---|---|
| `+0x00` | Control | Bit 0 = interface on (write `1` to enable / return to command mode). Pixel-run arming (section 6): `0x231 \| b << 8` to prepare, `0x229 \| b << 8` to go, b = 1 when the DMA source reads 32-byte bursts. Read-back during a run started with `0x329`: `0x325` while running, `0x323` after | [apple] values; [silicon] S5 read-backs; bit meanings below [hypothesis] |
| `+0x04` | Run length | Number of bus transfers in a pixel run (76,800 for 320×240 on a 16-bit bus). Reads back the transfers done so far, **counting up** to the length | [apple]; [silicon] S5 (`0xA5` one 64 µs poll in, `0x12C00` at the end) |
| `+0x08` | Run format | Apple writes `0x1F` | [apple]; meaning unknown |
| `+0x0C` | Data FIFO | DMA destination for pixel runs; raises DREQ 3 while a run expects data | [apple]; [silicon] S5 |
| `+0x10 + 8k` | Bus timing word 0, chip select k | see section 3 | [apple]; [silicon] |
| `+0x14 + 8k` | Bus timing word 1, chip select k | see section 3; bit 30 = 16-bit bus | [apple]; [silicon] |
| `+0x178` | Index | Write a command or register index | [apple]; [silicon] |
| `+0x17C` | Data | Write a parameter, register value or pixel. 16-bit panels take one 16-bit value per write; the DCS panel one byte per write | [apple]; [silicon] (16-bit, panel 3) |

Control bits, from the S5 read-backs [hypothesis]: bit 3 = go (self-clears), bit 2 = busy, bit 1 = done; bits 8–9 as written. Apple's run-done handler (@14780) tests bit 1 and writes `3` back (interface on, and bit 1 acknowledged [hypothesis: write 1 to clear]); shutdown (@6392) writes 0 to `+0x08`, `+0x04` and `+0x00` [apple].

**Nothing else is written** [apple: every `0x1000_40xx` access in `vmcs.bin` 1.3, read statically, 2026-10-23]. Apple sets `+0x04` from the transfer's byte count, halved when the chip select's word 1 has bit 30 (the 16-bit bus) set (@14660), and copies `+0x08` from the panel record (`0x1F` for every panel; record at `0x22E94`, which also carries 320 × 240 and a nominal 60). It reaches the index and data registers as `0x1000_4140 + 0x38`/`+0x3C`. No scaler, colour-depth or refresh control is ever written, so the product brief's LCD scaling claim has no counterpart here (`open-questions.md` #58; `brief-map-peripherals.md` §3).

The CPU writes `+0x178`/`+0x17C` back to back with no status polling, in Apple's code [apple] and ours [silicon S4: correct frames]: writes stall the core, or the interface is faster than the code. Only the chip select 0 timing words are used on the iPod.

## 2. Panel identification and pins

| Signal | GPIO | Evidence |
|---|---|---|
| Panel reset, active low | 48 | [apple]; [silicon] S4 |
| Tearing effect (TE) input | 49 (edge input; interrupt source 11 measures the frame period) | [apple] |
| Panel ID bit 1 | 50 | [apple]; [silicon] S1 |
| Panel ID bit 0 | 51 | [apple]; [silicon] S1 |

**ID = GPIO 51 + 2 × GPIO 50** = bit 19 + 2 × bit 18 of `0x1000_2824` [apple; silicon S1: our unit read `0x000D_0000`, ID 3]. It can be read safely from the host while any image runs.

| ID | Controller family | Window registers | RAM-write | Reset wait |
|---|---|---|---|---|
| 0 | MIPI DCS (8-bit command, byte parameters): `0x2A`/`0x2B` window, `0x2C` write, `0x3A = 05` (16 bpp), `0x35` TE on | `0x2A`, `0x2B` | `0x2C` | 110 ms |
| 1 | 16-bit index/data, "0x2xx" register map (Renesas-like); two variants ("model" 1, 2) | `0x210–0x213` window, `0x200`/`0x201` address | `0x202` | 110 ms |
| 2 | 16-bit index/data, "0x2x/0x4x" map (Solomon-like) | `0x44–0x46` window, `0x20`/`0x21` address | `0x22` | 120 ms |
| 3 | As 1, a different panel; two variants | as 1 | `0x202` | 110 ms |

[apple] IDs, descriptors and scripts (descriptor tables at `0x2225C` (ID 0), `0x23404`/`0x234C4` (1), `0x23130` (2), `0x23274`/`0x23334` (3); LCD info block `0x22E94`). The controller families are [hypothesis] from the register maps. The model variant is chosen by Apple's ARM software (`display_control … model=N`) [apple]; which value a given iPod uses is not known statically. Our unit (ID 3) displays correctly with model 1's sequence [silicon S4–S7].

## 3. Bus timing

Timing word layout [apple @12A70, @C87C; reproduced exactly by our implementation for every panel]:

```
word = 0x4000_0000 | cyc(n0) << 24 (6 bits) | cyc(n2) << 16 (6 bits)
                   | cyc(n3') << 8 (7 bits) | cyc(n1)        (7 bits)
cyc(n) = max(ceil(n × f_kHz / 10^6) − 1, 0)      a field of v gives v + 1 bus cycles
n3' = n3 − (the nanoseconds already added by rounding n0 and n2 up)
```

n0–n3 are four per-panel nanosecond values; f is the bus clock the word is computed for. Bit 30 = 16-bit bus [apple]; the run length is then in 16-bit transfers.

| Panel | Word 0 ns (n0/n1/n2/n3) | Word 1 ns, power-on | Word 1 ns, after power-on | Apple's words (computed for 150 MHz) |
|---|---|---|---|---|
| 0 | 10/45/90/25 | 10/35/10/55 | same | `410D0206`, `41010705` |
| 1, 3 | 10/170/5/285 | 0/50/5/145 | 0/65/5/30 | `41002A19`, `40001407`, then `40000309` |
| 2 | 10/250/10/240 | 10/40/10/50 | same | `41012325`, `41010605` |

[apple] nanosecond tables and words.

**Measured on silicon [S6, S7]:**

- After a host start the LCD bus runs at **27 MHz** (37.04 ns per cycle).
- In a DMA pixel run each transfer takes the sum of word 1's four fields, each plus one, in bus cycles, with no other per-pixel overhead; word 0 (strobe set-up for command writes [hypothesis]) does not affect the run's time. Frame = 76,800 × Σ(field + 1) × 37.04 ns. Panel 3 examples (word 0 / word 1 → cycles → frame):

  | Words computed for | Word 0 | Word 1 | Cycles | Frame (measured) |
  |---|---|---|---|---|
  | 15 MHz | `40000202` | `40000000` | 4 | 11.42 ms |
  | 20 MHz | `40000303` | `40000001` | 5 | 14.26 ms |
  | 27 MHz | `40000604` | `40000001` | 5 | 14.26 ms |
  | 40 MHz | `40000906` | `40000002` | 6 | 17.10 ms |
  | 100 MHz | `40001B10` | `40000106` | 11 | 31.36 ms |
  | Apple, 150 MHz | `41002A19` | `40000309` | 16 | (about 45.5 ms predicted at 27 MHz) |

- Words computed for 27 MHz from Apple's nanoseconds give every field exactly Apple's timing on this bus: 14.26 ms per full frame. The 15 MHz words (fields below the panel's minimums) still displayed a one-pixel checkerboard without visible error on one unit at room temperature: headroom, not margin.
- Apple's own `LCD_UPDATE` with its 150 MHz words and the core on the PLL: 15.2 ms per frame [silicon, timed from the host].

Whether the LCD bus clock follows the core onto the PLL is not recorded here (open). If it stays at 27 MHz, Apple's 150 MHz words make a frame about 45 ms.

## 4. Pixel format

RGB565, one 16-bit transfer per pixel, red in bits 15–11 [silicon S4: colour, byte order and orientation verified on panel 3]. The DCS panel is set to 16 bpp by `3A:05` and `36:00` [apple]. A frame in memory is 320 × 240 × 2 bytes, rows 640 bytes apart, top row first. In a DMA run, the lower halfword of each 32-bit word goes out first [silicon S5: pixel order correct].

## 5. Power sequences

Power-on (state 0 → 2) [apple @F672, `display_control … power=2`; reproduced event for event by our firmware and verified on panel 3, S4–S7]:

1. Interface on (`+0x00 = 1`); write the panel's timing words.
2. Wait 1 ms; set GPIO 48 to output.
3. Reset pulse on GPIO 48: low 2 ms, high 2 ms, low 2 ms, high; wait the reset time (110 ms; 120 ms for ID 2).
4. Six writes of 0 to the index register, 2 ms apart (bus resynchronisation).
5. The panel's **init** script.
6. Re-apply timing (IDs 1 and 3 switch to the shorter "after power-on" word 1).
7. The **on** script, ending with the full window and the RAM-write index.

Power-off (2 → 0): the **init** script again, then the **off** script, ending with GPIO 48 low. State 1 = initialised, display off; states 4 and −2 exist [apple]; unused here.

Notation: `rrrr=vvvv` writes register rrrr (index) then value vvvv (data); `[xx]` a bare index write; `cc:pp,…` a DCS command with parameter bytes; `{40xx=v}` an interface register write; `waitN` a delay in ms. Values for panel 2 marked * are written sign-extended by Apple (`0xFFFFE283` etc.); zero-extended values work equally (the panel bus is 16 bits) [apple; hypothesis that the upper half is ignored].

```
ID 0 (DCS) on
  {4000=1} {4010=410d0206} {4014=41010705} wait1 GPIO48 output
  reset pulse, wait110, ([0] wait2) x6
  init: 2d:<128-byte colour lookup: 32 red, 64 green, 32 blue ramps 0..0x3f>
        36:00 26:01 3a:05 30:00,00,00,ef [11] wait5 2a:00,00,01,3f 2b:00,00,00,ef
  on:   {4000=1} {4010=410d0206} {4014=41010705} [29] 35:00 2b:00,00,00,ef 2a:00,00,01,3f [2c]
ID 0 off: init, then [34] [28] [10] wait50 GPIO48 low

ID 1, model 1, on: timing 41002a19/40001407, reset, sync, then
  0001=0100 0002=2500 0003=02b0 0004=0000 0008=0404 0009=0000 000b=0000 000d=0010
  0010=0013 0011=2600 0012=2200 0013=0802 0014=080c 0015=0816 0016=2200 0031=0000
  0080=0031 0300=0705 0301=0006 0302=0302 0303=0303 0304=0300 0305=0103 0306=001f
  0307=0705 0308=0006 0309=0302 030a=0303 030b=0300 030c=0203 030d=001f
  {4014=40000309} 0400=001d 0401=0001 0007=0001 0205=0060 0110=0001 0100=15b0
  0101=0020 0102=029d 0103=0e5b 0105=0103 0106=0103 0007=0021 0102=02bd wait120
  0200=0000 0201=0000 0007=0031 0030=001f wait17 0030=03ff 0007=0133
  0210=0000 0211=013f 0212=0000 0213=00ef 0200=0000 0201=0000 [202]
ID 1, model 2 differs: 0001=0110 0002=1500; gamma 0300-030d as model 1, then 0310-031d
  (the same 14 values) and 0320-032d (0707 0006 0300 0303 0303 0107 001f, twice);
  after {4014=40000309} the order is 0400 0401 0205 0007; 0103=1158
ID 1 off: init, then 0007=0032 wait51 0007=0022 0030=0040 wait17 0007=0020 0030=0000
  0100=0580 0101=0060 0102=020d 0007=0000 wait17 0100=0700 0100=15b4 GPIO48 low

ID 2 on: timing 41012325/41010605, reset (120 ms), sync, then
  0001=011d 0002=0300 0003=1030 0007=0004 0008=0202 000c=0000 0030..0035=0000
  0036=0707 0037=0000 0038=1807 0039=1e08 (timing again) 0007=0004 0013=0000 wait10
  0011=2202 000b=e283* 0010=2d50 0013=0040 wait50 0013=0060 wait50 0014=a404*
  0010=3250 0013=0070 0071=0317 wait150 0044=013f 0045=0000 0046=ef00* 0009=0009
  0007=0016 wait50 0007=0017 0020=0000 0021=0000 0045=0000 0044=013f 0046=ef00*
  0020=0000 0021=0000 [22]
ID 2 off: init, then 000b=6c04 0007=0016 wait50 0009=0001 0007=0004 0010=0250
  0013=0060 wait50 0013=0040 wait50 0013=0000 wait200 0010=0000 GPIO48 low

ID 3, model 1, on: timing as ID 1, reset, sync, then
  0008=0808 0010=0013 0011=0300 0012=0101 0013=0a03 0014=0a0e 0015=0a19 0016=2402
  0018=0001 0090=0021 000d=0010 0003=0230
  gamma 0300-030d and 0310-031d: 0707 0207 0700 0303 0301 0707 1f07
                                 0606 0507 0701 0303 0302 0706 1f19
        0320-032d: 0607 0206 0700 0302 0302 0707 1a13 0606 0507 0702 0303 0303 0707 1f1f
  {4014=40000309} 0400=001d 0401=0001 0205=0060 0007=0001 0031=0071 0110=0001
  0100=17b0 0101=0220 0102=009b 0103=1000 0105=0103 0106=0104 0007=0021 0102=00bb
  0001=0110 0002=0500 0007=0031 0030=0007 wait17 0030=03ff wait34 0007=0072 wait85
  0007=0173 0210=0000 0211=013f 0212=0000 0213=00ef 0200=0000 0201=0000 [202]
ID 3, model 2 differs: gamma 0300-030d (0707 0002 0603 0303 0100 0307 1719, twice),
  0310-031d (0707 0204 0605 0303 0200 0307 1719, twice),
  0320-032d (0607 0001 0701 0303 0300 0707 1f19, twice); 0103=1200 0106=0105 0102=00bd
ID 3 off: init, then 0007=0172 0030=03ff wait51 0007=0120 0030=0000 0100=0780 0007=0000
  0101=0260 0102=00a9 wait17 0100=0700 0100=0704 GPIO48 low
```

The **init** part of each listing is everything before the gamma registers or the timing re-apply (ID 0's ends with `2b:…`, ID 2's with `000c=0000`).

Measured on panel 3 [silicon S4, S7]: power-on plus first frame about 283–315 ms (the scripts' own waits are about 265 ms); panel off about 68 ms.

**Standby without a reset** [ours; hypothesis, not yet run on silicon]: the panel is dark while GPIO 48 stays high, so a wake can skip steps 1–4 above (about 130 ms on panel 3). Two depths are cut from the panel's own scripts:
- **light** keeps the power circuits on. Dark: ID 3's off script up to `0007=0000`, without the `0100`–`0102` writes; ID 0's `[34] [28]`. Wake: ID 3's display-on ramp `0007=0021 0007=0031 0030=0007 wait17 0030=03ff wait34 0007=0072 wait85 0007=0173`; ID 0's `[29] 35:00`. IDs 1 and 2 have none.
- **deep** is the whole off script with the reset left high; the wake is the init and on scripts.

Whether the panel comes back right, and what each depth draws, is [`open-questions.md`](open-questions.md) #49 (stage 5s).

## 6. Sending a frame

### Window

Before pixels, set the window and write the RAM-write index [apple @12BA0, @12BF6; silicon S4 for panel 3, full and partial windows]:

| Panel | Order |
|---|---|
| 0 | `2b:y0hi,y0lo,y1hi,y1lo`, `2a:x0hi,x0lo,x1hi,x1lo`, then `[2c]` |
| 1, 3 | `0210=x0`, `0211=x1`, `0212=y0`, `0213=y1`, `0200=x0`, `0201=y0`, then `[202]` |
| 2 | `0045`, `0044`, `0046` (= `y1 << 8 \| y0` [hypothesis]), `0020`, `0021`, then `[22]` |

Full screen: x 0–319, y 0–239. Sub-rectangle meanings are verified for panel 3 only (a 100×50 window at (200,170) drawn correctly) [silicon S4]; ID 0 [hypothesis, from DCS]; ID 2's portrait mapping [hypothesis].

### By CPU

Write each pixel as a 16-bit value (zero-extended in a 32-bit store) to `+0x17C`. With the bus at 27 MHz and our timing words, a full frame took 38.97 ms (our 100 MHz words) to 49.0 ms (Apple's 150 MHz words) [silicon S4, S5]; the CPU path has pacing beyond the bus cycles (not characterised).

### By DMA (Apple's method)

[apple @10C9C, @96FC, @145A2; silicon S5–S7]

1. CPU: window and RAM-write index as above.
2. Post a DMA control block (`dma.md`) on the LCD channel: source the frame, destination `0x1000_400C`, length in 4-byte units (38,400 for a full frame), destination width 4 bytes, paced by DREQ 3, transfer info `0x1900_A051`-class (2-D: `0x1901_A021`-class). For a sub-rectangle use a 2-D block: rows of w/2 units, skip = (pitch − width)/4 units.
3. Arm the interface: `+0x00 = 0x231 | b<<8`; `+0x04 = pixel count`; `+0x08 = 0x1F`; `+0x00 = 0x229 | b<<8` (Apple writes `0x331` and `0x329`: b = 1 with 32-byte source bursts).
4. Wait for DMA status bit 8 (`dma.md`), then `+0x00 = 1` to return to command mode.

Silicon results: full frame 14.26 ms at 27 MHz-computed words; a 100×50 rectangle 2.05 ms (100 MHz words); back-to-back frames, rectangles and command-mode writes after a run all correct [S5–S7]. x and width must be even (4-byte units); otherwise use the CPU path.

### Tearing

DMA reads the source live; if the source is rewritten during a run, one frame can show both. Apple waits for TE (GPIO 49, source 11) [apple]; our images do not, and no tearing was visible in normal UI use [silicon S7, by eye].

**Two kinds of tearing.**
- The one above: the source changes under a running DMA. Avoid it with two framebuffers.
- The panel's own: it refreshes its glass from its internal memory on its own clock. A write that crosses the scan line shows the old frame above it and the new one below.

**How Apple handles the panel's scan** [apple, vmcs 1.3]:
- **The TE interrupt handler** (`@CB1C`, source 11, registered at `@12AE6`):
  - It reads the 1 MHz counter and keeps the last edge's time at `gp+60280+32`.
  - It keeps a running average of the edge-to-edge period at `+40`: new = (15 × old + this one) / 16.
  - Then it calls the display's callbacks (`gp+0x2e4`, set to `0x1295A` at `@12B1A`; and `gp+0x294`).
- **A scan-position routine** (`@4B4A`) estimates which panel line is being scanned now, from the last edge's time and the averaged period. From the rows still to be written it returns how long to wait before starting, so the write stays behind the scan ("beam racing").
- So Apple's driver has no fixed refresh rate: it measures the panel's at run time. No constant in vmcs gives it.

**What the rate is** [unknown on silicon].
- Panel 3's register map matches the Renesas R61509V family; panel 0 is MIPI DCS (`35:00` turns TE on).
- Controllers of this kind refresh at fosc / (clocks per line × divider × (lines + porches)), typically 60–70 Hz. That's a general figure for the family, not a datasheet value for this panel.
- Panel 3's script sets `0x0010 = 0x0013` (clocks per line and divider) and `0x0008 = 0x0808` (porches) [apple]. Its oscillator is unknown.
- Stage 3d measures it: the 3D image polls GPIO 49's level (`0x1000_2824` bit 17) for 500 ms and times the rising edges (`research/bcm2722/render3d.md`; open question #52).

**A tear-free present without interrupts** [ours; simulator only].
- Wait for a rising TE edge by polling, then start the frame's DMA.
- At the 27 MHz bus words a full frame takes 14.26 ms [silicon S7], faster than a 60 Hz scan (16.7 ms). So a write that starts with the scan stays ahead of it, if the panel scans in the same row order as our window writes [hypothesis].
- For 30 fps on a 60 Hz panel, present on every second edge.
