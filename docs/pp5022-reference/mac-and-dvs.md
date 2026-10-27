# The "integrated MAC", voltage scaling and 100 MHz

Desk study, 2026-10-24. It answers three product-brief claims (`datasheet-features.md` rows 2, 5 and 11): an "integrated MAC", "system-enabled dynamic voltage scaling" with a 1.14–1.32 V core, and "up to 100 MHz". Each section separates what is read from code, what is measured and what is inferred. §5 holds the device probes, written as Developer › Debug stages. Nothing here has run on silicon yet.

Tags follow `index.md`. Apple's firmware is cited as `osos 0x1xxx_xxxx`: the load address `0x1000_0000` plus the offset into `osos.code` (firmware 1.3; `sources.md`). The code is linked at 0, so an address in a vtable or literal equals its file offset. Code that Apple copies into IRAM sits near the end of the image (`osos 0x1071_5xxx`–`0x1073_4xxx`). Only addresses, constants and the order of accesses were recorded. No Apple code is reproduced. Firmware 1.2.1 and 1.2.3 hold the same constants at the same roles.

Method, for reproduction: `arm-none-eabi-objdump -D -b binary -marm` over `osos.code`, then small scripts (kept outside git) that find (a) every literal-pool constant loaded by a `ldr rX, [pc, #n]` in code-like 4 KB blocks, (b) every load or store whose base register holds a known constant within a straight-line run, using simple constant propagation, (c) every coprocessor instruction with the "always" condition, and (d) clusters of long multiplies.

## 1. The integrated MAC: verdict

**It is the ARM7TDMI's own multiplier, not a separate block.** Confidence: high for "there is no software-visible MAC peripheral that Apple uses"; medium for "there is no unused MAC block at all", which only the read-only scan in §5.1 can rule out.

### 1.1 What the briefs actually say [external]

- Only the **PP5022** brief (`wb_5022_Brief_Mar05.pdf`, pp. 1–2) says "integrated MAC". The PP5020 and PP5024 briefs (`wb_5020_Brief_0108_Public.pdf`, `wb_5024_Brief_Mar05.pdf`) do not use the word. `datasheet-features.md` row 11 credited all three; that was wrong and is corrected there.
- The PP5003 brief's "MAC" is a 10/100 Ethernet MAC (`wb_5003_brief_0108_Public.pdf` p. 1), not a multiply-accumulate unit. Its "vector processor" is a vector floating-point unit of a different family.
- The same PP5022 brief calls itself "register-set compatible with the PP5020". A new register-mapped coprocessor would sit badly with that sentence. A marketing name for the core's long multiply-accumulate would not.
- The "M" in ARM7TDMI stands for the enhanced multiplier: a 32×8 array with early termination and the long forms `UMULL`, `UMLAL`, `SMULL` and `SMLAL` (64-bit result and accumulate) [external] ARM7TDMI Technical Reference Manual (ARM DDI 0210). That is a multiply-accumulate unit integrated in each core.

### 1.2 What Apple's firmware does [apple]

1. **No coprocessor use.** The ARM7TDMI coprocessor interface is where a tightly coupled MAC would attach, as on some other ARM SoCs. In the code the recursive walk reaches, there are no `MCR`, `MRC`, `CDP`, `LDC` or `STC` instructions. Across all code-like blocks, the "always"-condition coprocessor words are a few dozen, scattered at random over all 16 coprocessor numbers inside data and obfuscated tables. That is noise, not a pattern.
2. **The audio hot loops are ARM long multiplies in IRAM.** The densest multiply clusters are in the IRAM-resident code (`osos 0x1071_8000`–`0x1072_0000`: about 340 multiplies in 32 KB). A representative routine is a polyphase synthesis window. It loads four coefficients and four samples with `LDM`, runs a chain of `SMULL` then three `SMLAL` into a 64-bit accumulator, saturates the result to 16 bits, and stores a halfword sample. That is the inner loop of an MP3/AAC subband synthesis. It touches no peripheral address. Over the whole image there are about 450 `SMULL`, 175 `SMLAL`, 880 `MLA` and 6,500 `MUL` with the "always" condition.
3. **No unknown peripheral in the audio or codec paths.** Every peripheral constant that code-like blocks load, and every constant-based access the propagation found, falls in a page Rockbox already maps, with one exception (§1.3). The pages touched are: processor ID, mailbox, interrupt controller, timers, device/clock control, processor control, both DMA engines, cache control, GPIO, `0x7000_0000` (ID, straps, device init), I2S, the LCD bridges, PWM, I2C/wheel, IDE, USB and the cache/MMAP window. Nothing in the codec region loads a `0x6000_xxxx` or `0x7000_xxxx` base. The pattern a MAC peripheral would leave, "write a register pair, then read a result" in a loop, does not occur.
4. **The unmapped addresses that do appear:**

| Address | Where | What Apple does | Reading |
|---|---|---|---|
| `0x6000_3000`–`0x6000_300C` | `osos 0x1000_05D8` (save), `0x1000_0714` (restore), `0x1022_9368` (set) | four 32-bit registers of sixteen 2-bit fields. The setter takes a slot number and four 2-bit values, rejects them unless the four are a permutation of {0,1,2,3}, and writes value *k* into slot *n* of register *k*. Callers program slots 0–3, swapping two values depending on a mode flag. All four registers are saved and restored with the interrupt enables, `DEV_EN`, `CLOCK_SOURCE`, `PLL_CONTROL` and `CACHE_CTL` around sleep | **an arbitration/priority table** ([hypothesis]): for each of up to 16 slots, an ordering of four requesters. The brief's "cross-bar" (row 9) is the obvious candidate. Not a MAC: no data ever passes through it |
| `0x6000_6048` | PP5022 clock driver | written with bit 31 plus a multiplier/divider pair only when the CPU source is code 2 or 4; read by the frequency decoder (§3.1) | a **second PLL** that scales the 24 MHz and 48 MHz taps ([apple] for the use, [hypothesis] for the name) |
| `0x7000_0008` bits `[6:5]` | PP5022 clock-driver constructor | read once | a **speed grade** (§3.2) |
| `0x7000_2400`, `0x7000_2580`, `0x6000_6088` | the clock driver Apple builds for other chips (not the PP5022) | stored as bases | not used on our chip by this code; noted only |
| `0xC000_3024/28`, `0xCF00_1110` | shared code | PP5002-family addresses (Rockbox's `ata-pp5002.c`, `system-pp5002.c`) | the image carries code for older parts |

So the one unexplained block (`0x6000_3000`) is configuration, not arithmetic. It enters `open-questions.md` as #34.

### 1.3 What this means for ProkBox

- No hardware MAC exists for the codecs, EQ, resampler or time-stretch to use. The speed-up the brief implies is what Rockbox's ARM assembly already uses (`SMULL`/`SMLAL` in `lib/rbcodec` `asm/arm`). The ranked shortlist in `datasheet-features.md` drops this item from rank 1.
- Apple keeps its synthesis loop in IRAM, which is Rockbox's policy too (`ICODE_ATTR`). Nothing new to copy.
- The `0x6000_3000` table may matter for the CPU/COP/DMA contention questions (`data-paths.md`, row 9 of the feature table). The read-only readout in §5.2 is cheap.

## 2. The PMU and its voltage formula (open question #20)

### 2.1 Which part [apple] [rockbox]

- Apple's PMU code reads register `0x00` (ID) once. If it reads `0x24`, Apple treats the part as one type: its ADC block is at `0x30`/`0x32` and a status register at `0x36`. Otherwise it uses the other type: ADC at `0x2E`/`0x30`, status at `0x34` (`osos 0x1028_3704`, `0x1028_341C`, `0x1028_36B0`). The regulator registers `0x1B`–`0x27` are written at the same addresses for both.
- Rockbox's ADC use on the Video (`adc-ipod-pcf.c`: start at `0x2F`, result at `0x30`) matches the **second** type. So the owner's unit most likely does not read `0x24`. That is [hypothesis] until §5.3 reads the ID.
- The two types are almost certainly the PCF50605 and PCF50606/7 variants that Rockbox's shared header `pcf5060x.h` covers ("from datasheet rev 2.2"). Which ID belongs to which part number is not established. No public datasheet for the PCF50605 or PCF50606 could be retrieved: NXP no longer hosts it, and the mirrors and the old Openmoko driver pages refused access (2026-10-24). Trade-press notes confirm that the PCF50606 has "dynamic voltage management" and a step-down converter, and that the PCF50605 is the same part without the charger and touch-screen blocks [external] Philips launch note via eepower.com (title only, page not retrievable).

### 2.2 Register layout (strong)

| Field | Evidence |
|---|---|
| Every regulator control byte is `[7:5]` **mode** and `[4:0]` **voltage code** | [rockbox] values (`0xEC`, `0xF5`, `0x18`); [apple]: one PMU routine composes `DCDC1 = code \| mode<<5` and the LDO routine composes `LDO = code \| mode<<5` (`osos 0x1028_37A4`, `0x1028_3360`) |
| Mode `111` = on in all states, `000` = off in all states | [rockbox] measured comments in `pcf50606-h300.c` ("ON in all states", "OFF in all states") |
| Apple also uses modes `010` (LDOs and DCDC1) and `000` | [apple]; meaning per state not known ([hypothesis]: one bit per PMU state, ON / STANDBY / another) |
| **LDOs** (`IOREGC` `0x23`, `D1REGC1`–`D3REGC1` `0x24`–`0x26`, `LPREGC1` `0x27`): **V = 0.9 V + 0.1 V × code** | [rockbox] *measured* on an H300 and an X5 with a PCF50606 ("determined by measuring the output"); fits every iPod value (`0xF5` 3.0 V, `0xF0` 2.5 V, `0xF1` 2.6 V, `0xF8` 3.3 V) |
| **DCDC4** (`0x1E`): Apple writes `0x30` (code 3 in bits `[7:4]`) with every core-voltage change it makes, and 0 otherwise | [apple] `osos 0x1028_37A4`; meaning unknown ([hypothesis]: a ramp-rate or current-limit field of the core converter) |
| **DCDC2** (`0x1C`): Rockbox writes `0x0C`, the same code as DCDC1 but mode off, with the comment that it "may be used for voltage scaling" | [rockbox]; consistent with a second, pre-loaded voltage selected by hardware for DVS ([hypothesis]) |

### 2.3 The core converter's formula (weak; two candidates)

Data points:

| Code (`[4:0]`) | Claimed voltage | Source |
|---|---|---|
| 12 (`0xEC`) | 1.2 V | [rockbox] comment ("Default is 1.2V ON for PP5022/PP5024"); not a measurement |
| 25 (`0xF9`, `0x59`, `0x19`) and 26 (`0xFA`) | none stated | [apple] `osos 0x1028_262C`: Apple writes these **only when the chip is not a PP5022** (chip-ID byte not `'2'`, i.e. a PP5020-family or PP5026 part). On a PP5022 that routine leaves DCDC1 untouched |

Candidates:

| Formula | Code 12 | Code 25/26 | Fits a 1.7–2.0 V PP5020 core (`wb_5020_Brief_0108_Public.pdf` p. 4: 1.7 / 1.8 / 2.0 V)? |
|---|---|---|---|
| **A: V = 0.6 V + 0.05 V × code** | 1.20 V | 1.85 / 1.90 V | yes |
| B: V = 0.9 V + 0.025 V × code | 1.20 V | 1.525 / 1.55 V | no (below the PP5020's minimum) |
| C: V = 0.1 V × code | 1.20 V | 2.5 / 2.6 V | no (above the maximum) |

**Best estimate: A (50 mV steps from 0.6 V). Confidence: low to medium.** It rests on two inferences: that the non-PP5022 branch serves a 1.8 V-core PP5020-class part, and that Rockbox's "1.2 V" label for `0xEC` is right. B stays possible if that branch served some other part. Either way, **one code step is 25–50 mV**, and the codes `0xEB`–`0xEE` lie inside the PP5022's specified window under both A and B:

| `DCDC1` | A | B | In 1.14–1.32 V under both? |
|---|---|---|---|
| `0xEA` | 1.10 | 1.15 | **no** (A below the minimum) |
| `0xEB` | 1.15 | 1.175 | yes |
| `0xEC` (now) | 1.20 | 1.20 | yes |
| `0xED` | 1.25 | 1.225 | yes |
| `0xEE` | 1.30 | 1.25 | yes |
| `0xEF` | 1.35 | 1.275 | **no** (A above the maximum) |

That band, `0xEB`–`0xEE`, is the hard limit for every probe in §5. Settling the formula needs one measurement the firmware cannot make: a meter on the core rail at two codes (§5.4 step 0).

### 2.4 Apple does not scale the PP5022's core voltage [apple]

On the PP5022 path, Apple's power-state routine writes only LDO registers (`0x23`–`0x27`, for example the accessory supply `D2REGC1` = code 21, 3.0 V) and skips DCDC1. No other writer of register `0x1B` exists: the only routine that writes it is the one above, and its two callers are in that routine. **Apple ran the PP5022 at a fixed core voltage**, presumably the boot value `0xEC`, at every clock up to its speed grade. The brief's "system-enabled dynamic voltage scaling" was not used on the iPod Video.

## 3. Clocks: what Apple's PP5022 driver reveals

### 3.1 The PLL and the clock tree [apple]

Apple picks one of three clock-driver classes by the chip-ID byte (`PP_VER1[23:16]`): `'2'` (PP5022), `'6'` (PP5026) or another part (`osos 0x1018_C8BC`). This matches the boot ROM's own two-way test [external] siggi `16`. The PP5022 class's set-frequency method lives in IRAM (`osos 0x1071_6A80` onward). Its frequency decoder is at `osos 0x101B_8380`.

- **`PLL_CONTROL` fields on the PP5022:** N = `[4:0]`, M = `[14:8]`, post-divider = `[21:20]` (0 → /1, 1 → /2, 2 → /4, 3 → /8). f = 24 MHz ÷ N × M ÷ post. This agrees with Rockbox's reading (`clocks-power.md` §2) and narrows its 8-bit fields to 5 and 7 bits.
- **Apple's recipe** for a target f (kHz), always with the 24 MHz reference:

| Target | N | Post | M | VCO |
|---|---|---|---|---|
| 12–27 MHz (floor 12 MHz) | 6 | /8 | f × 48 / 24000 | 8 × f = 96–216 MHz |
| 27–60 MHz | 12 | /4 | f / 500 | 4 × f = 108–240 MHz |
| 60–**125 MHz** (clamped at 125,000 kHz) | 12 | /2 | f / 1000 | 2 × f = 120–250 MHz |

  So the PP5022 driver accepts up to **125 MHz**, and its VCO runs up to 250 MHz. Rockbox's 80 MHz value (M 20, N 3, /2, VCO 160 MHz) is the same VCO as Apple's 80 MHz (M 80, N 12, /2). The PP5026 class clamps at 100 MHz, uses N = 6 and dividers /4 to /16, and fixes its maximum at 66 MHz.
- **Switching order:** set the enable bits `0x8800_0000`. Move `CLOCK_SOURCE` to the 24 MHz code, bracketed by short self-timed waits that the running core sets in its own `CPU_CTL` (`0x4800_001F`, then `0x4800_000F`: wait-count mode, clock-cycle units). Program M and N with the post-divider at /8. Wait 255 µs (`0x4200_00FF`: wait-count mode, µs units, written to whichever core is running). Write the real post-divider, then select the PLL (code 7). Interrupts are off throughout. `0x6000_60A0` bits 31:30 are cleared during the switch and restored after.
- **`CLOCK_SOURCE` source codes, as Apple's decoder reads them:** 0 = 32 kHz, 1 = 16 MHz, 2 = 24 MHz (or 24 MHz × (M+1)/(N+1) through `0x6000_6048` when its bit 31 is set), 3 = 33 MHz, 4 = 48 MHz (likewise through `0x6000_6048`), **5 = 20 MHz, 6 = 40 MHz**, 7 = PLL. The active selector is the highest set bit of `[31:28]`, choosing field `[14:12]`, `[10:8]`, `[6:4]` or `[2:0]`. Rockbox's comments call 5 "SLOW" and 6 "FAST" (dividers of 24 MHz and the PLL). Apple's code treats them as fixed 20 and 40 MHz taps. **Conflict, unresolved:** open question #9 now has a concrete test (§5.2).

### 3.2 A speed grade in the strap register [apple]

The PP5022 class's constructor reads `STRAP_OPT_A` (`0x7000_0008`) bits `[6:5]` and stores a maximum frequency: **0 → 133 MHz, 1 → 100 MHz, 2 → 80 MHz, 3 → 66 MHz** (`osos 0x101B_85A4`). The meaning "maximum rated clock of this die" is [hypothesis]. It is the natural reading of a per-chip field that becomes a frequency ceiling. Our unit's value is unknown. Reading it is the first gate for any 100 MHz test (§5.2).

### 3.3 Memory wait states follow a 90 ns rule [apple]

Apple computes `DEV_TIMING1` from the target clock: period = ⌊10⁶ / f_kHz⌋ ns, n = ⌊89 / period⌋, capped at 15. It writes `n | n<<8` into bits `[3:0]` and `[11:8]`, but only when n > 3. As in Rockbox, it raises the value before speeding up and lowers it after slowing down (`osos 0x101B_8320`/`0x101B_8500`; the 90 ns constant is a data word, `osos 0x102A_99F4`).

| Clock | Apple's n | Rockbox |
|---|---|---|
| 30 MHz | 2 (no write; leaves the previous value) | `0x0303` |
| 66 MHz | 5 → `0x0505` | – |
| 80 MHz | 7 → `0x0707` | `0x0808` |
| **100 MHz** | **8 → `0x0808`** | – |
| 125 MHz | 11 → `0x0B0B` | – |

So Rockbox's 80 MHz value already equals Apple's 100 MHz value. That 90 ns is the access time of a static device, most likely the boot NOR flash, is [hypothesis].

## 4. What 100 MHz and voltage scaling are worth

- **Speed:** 100 MHz is 25 % more cycles for the boosted state (heavy codecs, the video feed, library builds). The BCM host port scales with the PP clock (`clocks-power.md` §4), so a full frame push would drop from about 9.6 ms to about 7.7 ms [estimate].
- **Battery, at a fixed core voltage:** close to nothing. Energy per cycle is about constant at fixed V, so 100 MHz finishes sooner and spends the same energy, apart from small gains in fixed overheads (race to idle). Its value is responsiveness, not battery.
- **Battery from voltage:** dynamic power scales with V². One step down at 30 MHz gives 1.15 V under A (−8 % core dynamic power) or 1.175 V under B (−4 %). The brief's 60–80 mW for MP3 decode (`wb_5022_Brief_Mar05.pdf` p. 3) is the whole chip, and the core's share is unknown. A realistic gain is a few milliwatts in the dominant playback state, that is low single-digit percent of runtime [estimate]. Whether 100 MHz would need a step **up** (`0xED`/`0xEE`) is the other half of the same probe.
- **Policy if both pass:** run playback at 30 MHz with `DCDC1` one step down, raise to `0xEC` before boosting, and drop back after the boost ends. The order mirrors `DEV_TIMING1`: voltage up before the clock rises, down after it falls. Apple did not do this on the PP5022 (§2.4), so it is new ground, and it waits for an owner go after the probes.

## 5. Probes (Developer › Debug stages)

Common rules for every stage:

- Built into the developer build only, reachable from Developer › Debug › Hardware probes. Each stage logs to the system log with its prefix. Nothing is shown in a splash, and each stage ends with a single `… done` line.
- Read-only stages (5.1–5.3) need no go, but they still only run when the owner starts them from the menu.
- **Write stages (5.4 and 5.5) need the owner's explicit go per run.** They also need a fresh backup of the system folder, the cell above 3.9 V at rest, USB unplugged, playback stopped, the disk idle (no writes in flight, the write cache flushed), and the backlight on.
- Recovery for any hang: hold MENU+SELECT, a hard reset. Both write stages change only volatile state (PLL registers, `DEV_TIMING1`, one PMU register), and the next boot rewrites all of it: Rockbox's `pcf50605_init()` writes `DCDC1 = 0xEC`, and `system_init()`/`set_cpu_frequency()` reprogram the clocks. If a unit will not restart, hold MENU at boot to reach Apple's firmware, which sets its own values. A PMU standby or power loss also restores the PMU's defaults.

### 5.1 `mmio scan`: read-only census of unmapped register pages

Goal: find any live register block that Apple's code does not use (the last place a MAC could hide), and catalogue the `0x6000_3000` block.

- **Abort guard:** install a temporary data-abort handler that sets a flag and resumes at the next instruction (LR_abt − 4). Restore the vector afterwards. Interrupts are off for each page, the CPU only, with the COP held in sleep.
- **Pages, each read as 64 words (offsets `0x00`–`0xFC`), read twice 10 µs apart, in this order:** `0x6000_2000`, `0x6000_3000`, `0x6000_6040`–`0x6000_60FC` (except `0x6000_60A0` and `0x6000_60A4`: known, read only), `0x6000_E000`, `0x6000_F000`, `0x7000_1000`, `0x7000_4000`, `0x7000_5000`, `0x7000_7000`, `0x7000_9000`, `0x7000_B000`, `0x7000_D000`, `0x7000_E000`, `0x7000_F000`.
- **Never read**, because they are read-to-clear or have read side effects: `TIMER1_VAL`/`TIMER2_VAL` (`0x6000_5004`, `0x6000_500C`), any DMA channel status in `0x6000_B000`/`0x6000_9000`, the I2S FIFO read ports `0x7000_2880`, the ATA registers (`0xC300_xxxx`), the UART receive registers, the I2C and wheel data registers (`0x7000_C000` page), the mailbox queues (`0x6000_1000` page) and the USB block. Those pages are excluded entirely. No writes at all.
- **Log:** `mmio scan page=0x60002000 filler=N zero=N live=N abort=N`, then one line per word that is neither `0xCACA_D0D0` nor 0 (`mmio scan 0x6000300c=0x… / 0x…`), flagging words that differ between the two reads (`chg`).
- **Pass:** the scan completes without a hang. **Finding:** any page with non-filler words other than `0x6000_3000` and known mirrors is a candidate block; record it in `memory-map.md` with [silicon].
- **What it tells us:** a page of filler or aborts confirms nothing is there. Live, stable words in an unreferenced page would be the first evidence of an unused block, and would get their own write-free follow-up.

### 5.2 `clk id`: speed grade, the second PLL and the arbitration table (read-only)

- **Reads:** `PP_VER1`, `PP_VER2`, `STRAP_OPT_A`, `STRAP_OPT_B`, `CLOCK_SOURCE`, `PLL_CONTROL`, `PLL_STATUS`, `0x6000_6048`, `0x6000_6044`, `DEV_TIMING1`, `0x6000_3000`–`0x6000_300C`. Each is read once at 30 MHz and once at 80 MHz (the normal boost).
- **Log:** `clk id ver="…" strapA=0x… grade=G maxkhz=K src=0x… pll=0x… pllb=0x… t1=0x… arb=0x…,0x…,0x…,0x…`, where `grade = (strapA >> 5) & 3` and `maxkhz` is Apple's table value.
- **Source-code test (open question #9), read-only:** count a 10,000-iteration `subs`/`bne` loop against `USEC_TIMER` for the current selector only. No selector writes in this stage.
- **Pass:** all reads complete. **Gate:** 5.5 (100 MHz) runs only if `grade` is 0 or 1.

### 5.3 `pmu reg`: PMU identity and regulator settings (read-only, I2C)

- **Reads** through `pcf50605_read()`, one register per transaction under the I2C lock: `0x00` (ID), `0x1B`–`0x22` (DCDC1–4, DCDEC1–2, DCUDC1–2), `0x23`–`0x28` (LDOs), `0x18`–`0x1A` (PSSC, PWROKM, PWROKS).
- **Log:** `pmu reg id=0x… type=A|B dcdc1=0x… dcdc2=0x… dcdc3=0x… dcdc4=0x… dcdec=0x…,0x… dcudc=0x…,0x… ldo=0x…,0x…,0x…,0x…,0x…,0x…`, where type B means ID `0x24` (Apple's ADC-at-`0x30` variant).
- **Pass:** `dcdc1` reads `0xEC` (what Rockbox wrote). A different value means the register is not readable as written, or something else writes it. In that case stop, and do not run 5.4.
- **What it tells us:** the PMU variant (it settles half of open question #20 and the scratch-register question #33), whether DCDC2/3/4 hold a pre-loaded second voltage or a ramp setting, and the live values to restore.

### 5.4 `dvs step`: core voltage margin at 30 MHz (WRITE, needs the owner's go)

- **Step 0 (owner, hardware, optional but the only way to settle the formula):** with a meter on the PP core rail (a decoupling capacitor next to the PP5022; the owner identifies the pad), read the voltage at `0xEC`. Then let the stage hold `0xEB` for 30 s and read again. Two points give the offset and the step: A predicts 1.20 → 1.15 V, B predicts 1.20 → 1.175 V.
- **Preconditions:** 5.3 passed. The clock is fixed at 30 MHz (boost locked out for the stage). The common rules above apply.
- **Sequence:**
  1. Log the baseline: `DCDC1`, battery mV.
  2. Start the load: a CRC-32 over a 256 KB SDRAM pattern on the CPU and a 32 KB IRAM pattern on the COP, compared against precomputed values, in a loop.
  3. Write `DCDC1 = 0xEB` (bits `[7:5]` unchanged, code − 1). Never write a code outside `0xEB`–`0xEE`. Never change the mode bits. Never touch DCDC2–4.
  4. Run 60 s. Log every 5 s: `dvs step code=0xEB t=… crc_ok=N crc_bad=N mv=…`.
  5. Write `DCDC1 = 0xEC` and read it back.
  6. **Abort rule:** on the first CRC mismatch, write `0xEC` at once, log `dvs step FAIL code=0xEB t=…`, and stop.
- **Pass:** zero mismatches over 60 s, and `DCDC1` reads back `0xEC` at the end. Log `dvs step PASS code=0xEB`.
- **Extension (separate go):** the same at 80 MHz for `0xEB`. Then `0xED` and `0xEE` only as part of 5.5.
- **Limits:** one step down only. No disk, USB or audio during the stage. The stage refuses to start below 3.9 V.

### 5.5 `pll100`: 100 MHz trial (WRITE, needs the owner's go)

- **Preconditions:** 5.2 reports grade 0 or 1, and 5.4 has run (so voltage headroom is known). The common rules apply. The BCM must not be pushed during the stage, since the frame push is timed for the normal clocks. Audio is stopped and the disk idle (the ATA UDMA timing assumes 80 MHz).
- **Settings:**
  - PLL `0x8A12_1903`: M 25, N 3, /2, VCO 200 MHz, the same 8 MHz reference as Rockbox's 80 MHz. Apple's own recipe for the same clock is M 100, N 12, /2 (`0x8A12_640C`), the same VCO.
  - `DEV_TIMING1`: first `0x0909` (one above Apple's rule), then a second run with `0x0808`.
- **Sequence:** use Rockbox's `set_cpu_frequency()` order (`clocks-power.md` §4).
  1. Step to 24 MHz.
  2. `DEV_TIMING1 = 0x0909`.
  3. Write the PLL value and wait for lock, with a 2 ms bound (on a timeout, abort to 24 MHz).
  4. Stall both cores and select the PLL.
  5. Run the 10,000-iteration loop timed by `USEC_TIMER`: expect about 400 µs (501 µs at 80 MHz).
  6. Run the CRC load from 5.4 for 10 s.
  7. Return to 80 MHz with `DEV_TIMING1 = 0x0808`.
  8. Log `pll100 t1=0x0909 loop_us=… crc_ok=N crc_bad=N lock_us=…`.
- **Abort rule:** on the first CRC mismatch, or a loop time outside 380–420 µs, go back to 80 MHz and log `pll100 FAIL …`.
- **Pass:** loop time 380–420 µs and zero mismatches with `0x0808`. Then, with a further go, repeat at `DCDC1 = 0xEC` for 60 s.
- **Not part of this probe:** running 100 MHz in normal use. It also needs ATA and BCM-port timing review (`storage.md`, `display.md`) and a phase brief.

## 6. Summary for the shortlist

| Item | Verdict | Next step |
|---|---|---|
| Integrated MAC | The ARM7TDMI's long multiply-accumulate. No peripheral, no coprocessor in Apple's code. Rockbox already uses it | 5.1 (read-only) only to close the last gap |
| `0x6000_3000` | An unmapped 4-register permutation table, probably bus arbitration | 5.2 readout; open question #34 |
| Speed grade `STRAP_OPT_A[6:5]` | Apple reads a per-chip maximum of 66–133 MHz | 5.2 readout; open question #35 |
| PMU formula | Field layout solid. LDOs 0.9 + 0.1 × code (measured by Rockbox). Core converter 0.6 + 0.05 × code (A) favoured over 0.9 + 0.025 × code (B), low-to-medium confidence | 5.3, then a meter reading during 5.4 |
| Core DVS | Not used by Apple on the PP5022. One step down is inside the brief's window under either formula | 5.4 with the owner's go |
| 100 MHz | Apple's PP5022 driver allows 125 MHz, with `DEV_TIMING1` `0x0808` at 100 MHz | 5.5 with the owner's go, gated on the grade |
