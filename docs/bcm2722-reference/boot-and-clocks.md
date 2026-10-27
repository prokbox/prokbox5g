# 4. Boot, cache, clocks, regulator and power-down

## 1. Getting code to run

The BCM2722 has an internal boot ROM that answers a handshake on the host port; the host then writes an image into SRAM and starts the core. The host side is in `host-interface.md` ("Starting an image"). The core side:

| Fact | Value | Evidence |
|---|---|---|
| Load address | Core address 0: the host writes the image from SRAM offset 0 | [rockbox]; [silicon] every session |
| Entry point | `0x200`. The first `0x200` bytes hold vectors and are not executed on start | [silicon] S1 (the image reached its main code with nothing at 0); [apple] both Apple boot images begin at `0x200` |
| Start trigger | Host writes `0x10000400 = 0xA5A5_0002` | [rockbox]; [silicon] |
| Registers at entry | Not defined. Set gp (r24) and sp (r25) first | [apple] `vmcs.bin` sets gp then sp at `0x200` |
| Interrupts at entry | Treat as disabled; all source priorities are unknown, so write them | [hypothesis]; [apple] Apple's quiesce step writes them to 0 |
| Cache at entry | Off: the host flushed it and wrote `0x10000C00 = 0` | [rockbox] |
| Core clock at entry | 27 MHz | [silicon] S7 |
| 1 MHz counter at entry | Stopped | [silicon] S2, S4 |

A safe image layout [ours, proven on silicon]: every vector word pointing at a fault stub; code from `0x200`; stack below the framebuffer (top at `0xD0000`); the framebuffer at `0xE0000` if Rockbox's host driver is used.

### Apple's start-up order (for reference)

[apple] `vmcs.bin` 1.3, entry `0x200`; identical in 1.2.1 and 1.2.3.

| # | Step | Registers and values |
|---|---|---|
| 1 | gp, sp | gp = `0x1FC40`; sp = top of `.bss` (`0x316A0`), 4-aligned |
| 2 | Copy and zero data | memory only |
| 3 | Cache flush, then enable | `0x10000C00 = old \| 0x4000_0000`, wait for bit 0, write `old` back; then set bit 31 |
| 4 | Run control | `0x10000400 = 0xA5A5_0000 \| (old & ~1)`; `0x10000408 = 0xA5A5_0002` |
| 5 | Quiesce | interrupt priorities `0x10001010–1C = 0`; timer compares `0x10001808–14 = 0`; every DMA channel `+0x04 = 0xFFFF_FFFF`, `+0x00 = 0`; GPIO function selects `0x10002800–0C = 0`, event status `0x10002828/2C = ~0`, edge enables `0x10002830–3C = 0`, `0x10002840–5C = 0`; `0x10004C30 = 0` |
| 6 | Clocks to 150 MHz | section 4 below |
| 7 | 10 ms system tick | timer status `= 0xF`; compare 0 = counter + 10,000; source 16 priority 1 |
| 8 | SDRAM | `sdram.md` |
| 9 | RTOS (Nucleus PLUS) | tasks started through `rti`, frame `sr = 0x4000_0000`; idle loop `ei; sleep; di` |

## 2. Cache control (`0x10000C00`)

| Bit | Meaning | Evidence |
|---|---|---|
| 31 | Cache enable | [apple] set after a flush at start-up and in SDRAM set-up; [hypothesis] covers both I and D |
| 30 | Flush (write back and invalidate) request | [apple]; [rockbox] host writes `0xC000_0000` (bits 31 and 30) then waits for bit 0 |
| 16 | Set by Apple after SDRAM is up (`0x8001_0000` final value) | [apple] @147ca; meaning [hypothesis: enables caching or a path for SDRAM] |
| 0 | Flush done (read) | [apple]; [rockbox]; [silicon] host-side wait completes every boot. It **stays set** afterwards: PW1 read the word as `0x0001_0001` in a running image, long after the host's flush and its write of 0 [silicon, 2026-10-25] |

Flush procedure [apple, rockbox]: write `old | 0x4000_0000`; poll until bit 0 reads 1; write the wanted final value (bit 30 clear).

**Apple's own flush in full** [apple-RE, `vmcs.bin` @25a]: interrupts off (sr saved); `keep` = the word with bits 31 and 30 cleared; write `keep | 0x4000_0000`; read the word, and if bit 0 already reads 1, write `keep` and request again (no bound); then poll bit 0 up to 4,096 reads (a breakpoint if it never sets); write `keep`; sr back. @224 then sets bit 31 again (with 7 `nop`s before and 5 after the write) when the cache is to run on. Because bit 0 is left set (above), a flush that requests and polls at once can read a stale "done": Apple's re-request is the guard.

**The host's flush on a running core** [ours, following @25a]: the cold start's host flush (`0xC000_0000`, wait, `0`) is made with the core held, after a power cut that leaves bit 0 clear. The warm swap flushes from the host while the core spins in the park stub, with bit 0 still set by the park's own flush; it now uses @25a's form (bits 31 and 30 cleared in the request, so the cache is never switched on under running code; the request again while bit 0 reads 1 straight after it; then `0`). How reliable bit 0 is when read through the port, whose reads are microseconds apart, is not known: `quirks.md` #41, B13's `warm N: diag flush` line.

**Code written by the core** [apple-RE]: Apple runs nothing it has just written without this flush first: after the dynamic linker patches a library's relocations (@88a0, flush and enable, @296) and at a library's start (@91a4, flush, @25a). Ours did once (the warm swap's park stub, copied to `0xDE900` with data stores and called at once) and never got there on silicon (`quirks.md` #40). **Rule:** after code is written (by the core, or by the host after the start), run @25a's flush before the first call into it (`hp_code_flush`: the park stub, the PP's hibernate stub, the overlay); the host's own uploads already end with its flush (`host-interface.md`, "Starting an image", step 6).

**Rule:** the host writes into SRAM behind the core's back. With the cache off (the state the host leaves) host writes are seen directly [silicon: every image runs this way]. If an image enables the cache, it must flush before reading data the host wrote. Cache line size, associativity and whether the vector unit's loads go through the data cache are unknown.

## 3. The 1 MHz counter's clock

After a host start the system timer's counter (`0x10001804`) does not move [silicon S2, S3: 0 ticks over 1.5 s]. It is started by the clock block's `+0x4C`/`+0x54` pair [silicon S4, every image of every run]:

```
0x10001C4C = 0xA5A5_0501
0x10001C54 = 0xA5A5_0000
wait about 1 µs (Apple: about 20 nops)
0x10001C4C = 0xA5A5_051B
0x10001C54 = 0xA5A5_0000
```

- Found before the write on a host start: `+0x4C = 0x0000_0001`, `+0x54 = 0`, `+0x58 = 0x0000_0138` [silicon S4].
- The counter then runs at exactly 1 MHz: the host's clock and the counter agree to 0.1% [silicon S4].
- Reading [apple, from Apple's clock manager, `brief-map-power.md` §5]: every clock word is `[7:0]` integer divider and `[10:8]` source (5 = crystal), so `0x051B` = crystal ÷ 27 = 1 MHz. The earlier reading of bits 8 and 10 as enables is withdrawn (`quirks.md` #25).
- `+0x58 = 0x0101`, Apple's other start-up write, is not needed for the counter [silicon S4].

## 4. PLLs, core clock select and regulator

### Registers (`0x10001C00`, all pw)

| Offset | Register | Evidence |
|---|---|---|
| `+0x00`, `+0x04`, `+0x08`, `+0x0C` | PLL A, B, C, D input divider M | [apple]; [silicon] S7 readback of PLL A |
| `+0x14`, `+0x18`, `+0x1C`, `+0x20` | PLL A–D control: `[9:0]` multiplier N, bit 10 enable | [apple]; [silicon] S7 |
| `+0x28` | Core clock select: `0x0001` = crystal, `0x0101` = PLL A | [apple] values; [silicon] S7 readback `0x101` after the switch; field meanings [hypothesis] |
| `+0x2C–0x3C`, `+0x40`, `+0x44`, `+0x48`, `+0x50` | Block clock controls (nine words; Apple zeroes them all before the switch) | [apple] values; consumers unknown |
| `+0x4C`, `+0x54` | The timer counter's clock (section 3) | [silicon] S4 |
| `+0x58` | Block clock; Apple writes `0x0101` | [apple] |

f(PLL) = 27 MHz × N / M. For 150 MHz: M = `0x6C` (108), N = `0x258` (600) [apple; silicon S7: every kernel 5.56 × faster = 150 / 27].

**Apple's clock manager** [apple] (detail in `brief-map-power.md` §5):
- **PLL rule.** Always M = 108, a 250 kHz reference, and N = f_vco / 250 kHz. The VCO is kept at 75 MHz or more by running at a multiple of the wanted clock and dividing in the clock word.
- **PLL numbering.** Apple's code numbers the PLLs B, C, D, A.
- **Clock words.** All of `+0x28–0x58` are `[10:8]` source and `[7:0]` divider. Block-clock sources: 1 PLL B, 2 PLL C, 3 PLL D, 5 crystal, 7 PLL A. The core word `+0x28` uses 0 = crystal and 1 = PLL A.
- **Clock ids.** 1 `+0x30` (TV), 2 `+0x34` and 3 `+0x38` (audio, from PLL C), 6 `+0x3C` (USB; bit 15 a gate), 7 `+0x48`/`+0x50`, 8 `+0x28` (core), 9 `+0x58`.
- **DVFS.** Under `pm_set_policy min` the core runs at 5–150 MHz in 1 MHz steps. The regulator is raised before a clock rises and lowered after it falls, and the SDRAM refresh is set to f / 128,000.

### Regulator (`0x10001400`, pw)

| Bits | Meaning | Evidence |
|---|---|---|
| `[3:0]` | Core supply level, chosen from frequency. Apple's 13-row table (`0x1FA68`) maps e.g. 160.9 MHz → 15, 102 MHz → 9, 46.9 MHz → 5. A 10 ms wait follows every change | [apple]; "regulator" is [hypothesis] |
| `[7:4]` | Preserved by Apple; Rockbox's host clears them before cutting BCM power | [apple]; [rockbox] |

Read values: `0xB5` with Apple's firmware running [silicon S1]; `0xBB` after a host start of our image, `0xBF` after the switch to level 15 [silicon S7]. If `sys19`'s low nibble is ≥ 4 Apple forces level 15 [apple]. Apple's setter accepts levels 1–15; its table never asks for less than 5 [apple]. **Our 27 MHz images therefore run at level 11 where Apple's table and Apple's own running chip use 5** (`quirks.md` #26; stage PW3 in `brief-map-power.md` §9). The full table is in `brief-map-power.md` §4.

### Switching to 150 MHz

This is Apple's start-up sequence; reproduced write for write it takes a host-started core to 150 MHz [silicon S7]:

1. `0x10000400 = 0xA5A5_0020`, twice.
2. `+0x4C = pw 0x501`; `+0x54 = pw 0`; about 1 µs; `+0x4C = pw 0x51B`; `+0x54 = pw 0`.
3. Block clocks `+0x2C, +0x30, +0x34, +0x38, +0x40, +0x44, +0x3C, +0x48, +0x50 = pw 0`; `+0x28 = pw 0x0001`; `+0x58 = pw 0x0101`.
4. PLLs B, C, D, then A off: each divider and control `= pw 0`.
5. Regulator `0x10001400 = pw ((old & 0xF0) | 15)`; wait 10 ms.
6. PLL A: `+0x00 = pw 0x6C`; `+0x14 = pw 0x258`; wait 1 ms; `+0x14 = pw 0x658` (enable).
7. `+0x28 = pw 0x0101` (core onto PLL A).

Readback after the switch [silicon S7]: PLL A divider `0x0000006C`, control `0x00000658`, clock select `0x00000101`, regulator low byte `0xBF`. The 1 MHz counter keeps its rate across the switch [silicon S7]. Memory and peripheral accesses scale with the core clock (every bench kernel, loads and stores included, scaled by the same 5.56) [silicon S7].

Time cost: dominated by the 10 ms regulator wait and the 1 ms PLL wait [apple values].

### Switching back to 27 MHz

Not Apple's (Apple never switches down at run time). Our sequence [ours], which ran on silicon with the panel active and left the system working [silicon S8, pass-level evidence only]:

1. `+0x28 = pw 0x0001` (core onto the crystal).
2. PLL A control and divider `= pw 0`.
3. Regulator back to the level read before the switch up; wait 10 ms.

### Our level bands against Apple's governor (P19 PW4)

| | Apple's `vmcs` [apple] | Ours [ours] |
|---|---|---|
| Clocks | any whole MHz, 5–150 | twelve bands: the crystal (27 MHz), then each level band of @141ba's table, named by its top whole MHz: 46, 59, 74, 87, 102, 116, 123, 130, 140, 147, 150; inside a band any whole MHz above the band below's top |
| Level | @141ba's for the clock | the same: 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15; the crystal keeps the level found there (11 after a host start [silicon S7], 5 with the PW3 lever) |
| PLL A | M 108, N = VCO / 250 kHz, VCO ≥ 75 MHz | the same: below 75 MHz the VCO at twice the clock (core word `0x102`), below 38 MHz three times (`0x103`) |
| Order | a higher level before the clock, a lower one after, 10 ms each; then the PLL, 1 ms; the core word | the same, with the core on the crystal while PLL A locks; then **the core's speed checked** against its crystal speed (±12.5 %); a stalled wait or a wrong speed: the crystal, PLL A off, the level found [ours] |
| SDRAM refresh | f / 128,000 after the change | 27 MHz's interval before the change (safe at every clock), the new clock's after |
| Panel bus words | (Apple's per clock) | Apple's 150 MHz words on PLL A at any band (longer cycles at a lower clock: safe); ours on the crystal |
| Governor | on the chip: 10 ms windows, +6 % busy, −6 % idle, any MHz, clients' floors (@fe00, @fa24) | on the PP, two layers over the chip's busy meter: a band selector (up a band when the band's top saturates, down when the band below carries it at 80 %, 100 ms dwell; the clients' floors a minimum band) and an in-band policy (the band's top clock by default, or proportional); the merged image had no room for it (`../research/bcm2722/battery-levers.md` §4) |
| Silicon | Apple's chip ran it [silicon S1: level 5 at idle] | built; **not run**. `vc2sim` checks the order after every write, the speed check and the fallbacks (`bcmfw/tests/vclk_test.py`); the bench row `vbv` verifies each band on the unit, PW4 the ladder |

### What follows the clock

- **SDRAM refresh** is counted in core cycles: raise it after switching up, lower it before switching down (`sdram.md`).
- **LCD bus timing.** After a host start the LCD bus runs at 27 MHz [silicon S6, S7]. Whether it follows the core onto the PLL was tested in S8 but the figure is not recorded here (open).
- **Delay loops** calibrated in instructions change by 5.56×; delays on the 1 MHz counter do not.

## 5. Power-down

The host owns the BCM's power (`GPO32` bit 14). Rockbox's order [rockbox], which also works with our images [silicon S4–S7]:

1. Backlight off (avoids a visible flash).
2. Host: `0x10001400 &= ~0xF0` (through the port).
3. Command word: LCD sleep (`BCM_CMD(8)`); the image runs the panel's off script and acknowledges.
4. Command word: `BCM_CMD(0xC)`; acknowledge.
5. Host clears `GPO32` bit 14. All BCM state is lost; the next use is a cold bootstrap.

Every wake is therefore a full cold start of the BCM: bootstrap, upload, start, then the panel's power-on sequence on the first update [silicon S7: image ready about 2 ms after upload; first frame 436–458 ms after the upload began, of which the panel's own power-on waits are about 265 ms].

Rockbox's comments call step 2 and step 4 "not sure what this does" [rockbox]. Neither is known to be necessary [hypothesis].

**Apple's own power-down and hibernate** [apple] (`brief-map-power.md` §3). The gencmd `power_control videocore=0` or `=1` parks the chip: core on the crystal, PLLs zeroed, GPIO event enables cleared, stack moved to SRAM, SDRAM idled. It then writes `0x1F0 = 0xA5A5_0000`. For power-down the host clears `GPO32` bit 14 (bit 15 stays low). For hibernate the host sets bit 15, waits 5 ms, then clears bit 14; to wake, it reverses the pins and writes `0x1F0 = 0xA5A5_0001`, and the chip restores its clocks and SDRAM and answers `0xA5A5_0002`. Not yet run on silicon (stage PW5).

Idle without power-down: the core can execute `sleep` and be woken by an interrupt (timer compare, host doorbell). Apple's idle loop is `ei; sleep; di` with a 10 ms tick [apple]. Current draw in any state has not been measured.
