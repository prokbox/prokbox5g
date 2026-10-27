# Hardware experiment backlog

The plan for every device experiment that would settle an open question in the iPod Video 5G/5.5G guide. The owner asked for it on 2026-10-15 ("start planning those other register data things we can do"), with a Bluetooth-modded iPod possibly arriving as a second unit.

Long-term the guide should work as a self-contained textbook: someone who reads it can write a kernel from zero that drives every feature, without Apple's or Rockbox's code as a reference. This file lists the experiments that turn [rockbox-derived], [apple-RE], [unknown] and [hypothesis] facts into [silicon][measured] ones. It **plans** them; nothing in §2-§6 has run. §7 records the USB experiments that did. Tags follow [`data-paths.md`](data-paths.md) §1.

**Sources of the questions.** PP5022: [`pp5022-reference/open-questions.md`][ppq] (#1–#31, cited as **PP#n**). BCM2722: [`bcm2722-reference/open-questions.md`][bcq] (cited as **BCM#n**). Clean-room gaps: [`data-paths.md`](data-paths.md) §7 (DP-*n*) and §8 (cited as **§8**).

**Already scheduled for the next device build; not redesigned here.** Rows below cite them as dependencies:
- **E2-0 … E2-7**, the second-DMA-engine port lab ([`data-paths.md`](data-paths.md) §7.1): PP#11, PP#12, the SDRAM → SDRAM half of PP#30, BCM#41.
- **dev45: stage 7, stage 8a and stage 9** (video A/V, the BCM audio path, the AAC decoder; branch `p14-dev45`): BCM#29–#33, BCM#36–#39 as far as 8a-1 reaches, BCM#45.
- **Fit-3b's counters and the boot IDENTIFY log** ([`fit3-brief.md`](fit3-brief.md)): LCD, overlay, port-poll, art and PCM write-back counts; the storage adapter's identity words.

[ppq]: pp5022-reference/open-questions.md
[bcq]: bcm2722-reference/open-questions.md

---

## 1. How to read it

### Safety classes

| Class | What it may do | Standing guards |
|---|---|---|
| **R0** | Read-only register reads and dumps. Also counts: the routine driver traffic the shipping firmware already issues (a PMU ADC conversion request, a USB enumeration), and states the shipping firmware already enters (24/30/80 MHz, DAC standby, BCM power-off) | None beyond a build ID in the log. Skip registers documented as acknowledge-on-read or FIFO data ports (`TIMERn_VAL`, `I2C_DATA` outside a transfer, `IISFIFO_*`, `WHEEL_DATA`, the BCM's FIFO address) unless the row says otherwise |
| **R1** | Reversible writes: to scratch memory, or to a register that is restored afterwards, each with a timeout | Save, write, read back, restore, read back again. Every wait bounded (e.g. 50 ms a step, as in §7.1). The BCM is power-cycled and restarted (R3 in `data-paths.md` §5) after any BCM-side R1 step |
| **R2** | Writes that change clocks, voltages, power states or pin muxing | Late batches only, after Batch A's baselines. One variable per run. Code and data in IRAM, IRQ/FIQ masked, no storage I/O during the step, automatic revert after a bounded time. Battery above 50 %, USB-C cable in reach (it is a wake source, `CHGWAK`). The stage writes a "started *step*" line and syncs before each step, so a hang is a result, not a mystery |
| **R3** | Anything that could brick the unit or harm hardware: regulator voltage writes, NOR flash commands, the PLL outside Rockbox's limits | **The owner's explicit go for each run**, a fresh backup, and a written recovery path. None is planned on the owner's only unit |

Bootloader changes are a separate gate whatever their class: any row that touches `bootloader/ipod.c` needs the owner's explicit sign-off (CLAUDE.md, the firmware-platform rules). Apple's firmware stays bootable as the recovery path (hold MENU) in every batch.

### "Nothing happened" cannot pass

From [`data-paths.md`](data-paths.md) §0 rule 5 and D1. Every experiment names, in advance, a **positive signature** that only a real effect can produce:
- **Writes and transfers:** fill the target with a unique per-run pattern, read back every word, classify each as correct, untouched, shifted (`d`) or other (as in §7.1).
- **Dumps:** each dump section includes a **canary**: a register with a known value (`PP_VER1/2` reading as ASCII; `USEC_TIMER` advancing between two reads). A section whose canary fails is invalid, and nothing in it is cited. Every register is read **twice**; a word whose two reads differ is logged as *volatile* rather than as a value.
- **Timing:** every timed loop is calibrated against `USEC_TIMER` in the same run, and a control arm that must differ runs beside it (e.g. the unlocked counter in HX-B3 must lose counts; if it doesn't, the cores never contended and the locked result proves nothing).
- **Interrupts and events:** count from zero in the same run; an event "seen" means a count above zero with the source otherwise masked.

### Logging

- Every experiment logs **raw values** in hex with the address, core, PP clock, BCM clock and state, uptime in µs, and the build ID. Interpretations go in the docs, never only in the log.
- PP-side rows go to `ipodui.log`; BCM-side and devtest stages to `bcmtest-results.log` (both survive five reboots; `docs/research/bcm2722/device-session-1.md`). A dump too large for the rotation budget writes its own file in the system folder; the implementer chooses. Nothing appears on screen except the stage's own result screen in the developer menu.
- After a session the logs are copied to `session log, session` (read only), and each finding updates its manual and `data-paths.md` in one commit (§10 there).

### Owner action per run

| Code | Meaning |
|---|---|
| **none** | Start the stage from the developer menu and wait for its result screen |
| **SELECT** | Press SELECT when the screen asks (to advance through states) |
| **do** | A physical action the screen names: toggle hold, plug or unplug headphones or USB-C, touch the wheel |
| **listen** / **look** / **feel** | Report what is heard, seen or felt (a tone's side, a backlight level, a haptic pulse) |
| **meter** | An inline meter is attached and logging; note start and end times |

---

## 2. The backlog

Columns: **Needs** is nothing / owner action / meter / scope / second unit / HDD unit / Bluetooth unit. **Outcomes** lists what each result would mean. Batch letters are defined in §3. 69 rows.

### Batch A: one read-only dump stage (R0)

| ID | Question | Experiment | Class | Needs | Outcomes → meaning | Depends | Batch |
|---|---|---|---|---|---|---|---|
| HX-A1 | PP#9, PP#24 (partly), §8 PLL rules | Dump `CLOCK_SOURCE` `0x6000_6020`, `PLL_CONTROL` `0x6000_6034`, `PLL_STATUS` `0x6000_603C`, `DEV_EN`/`DEV_EN2`/`DEV_RS`/`DEV_RS2` (`0x6000_600C/6010/6004/6008`), `DEV_INIT1`/`DEV_INIT2` (`0x7000_0010/0020`), `CACHE_PRIORITY` `0x6000_6044`, and `0x6000_6080/6094/60A0/60A4/60C4`. Once at the earliest point in the main firmware (before `system_init` rewrites them), then after each switch to 24, 30 and 80 MHz | R0 | owner: none | Values after a switch equal Rockbox's writes: the encoding in `clocks-power.md` §2–§3 is [silicon] as written. Entry values with selector ≠ #2, or non-zero `DIV_SLOW`/`DIV_FAST`: the boot chain uses forms Rockbox never writes, and they become HX-G1's first arms. Entry equals Rockbox's 24 MHz state: the bootloader already reset them, so the ROM's own state needs HX-D8 | — | A |
| HX-A2 | PP#10 | Read `DEV_TIMING1` `0x7000_0034` at entry and at each clock | R0 | none | `0x0303`/`0x0808` as Rockbox writes: confirmed. The entry value is the boot chain's choice at its clock, which bounds HX-G2's sweep | — | A |
| HX-A3 | PP#1, PP#27 (read only) | Dump `0x7000_0000`–`0x7000_0044`, plus `0x7000_008C` (strap, SDRAM controller neighbourhood, the external device clock `0x7000_0018`, the BCM bootstrap word `0x7000_0040`). **`0x7000_0030` is read, never written** (the flash write gate, `boot.md` §1) | R0 | none | Values recorded against the emulator reading in `memory-map.md` §2: agreement promotes it to [silicon] for a 64 MB unit; a 32 MB comparison needs a second unit (HX-X2). Bits in `0x7000_0030` that differ from the emulator's are recorded, not interpreted | — | A |
| HX-A4 | PP#2 | Read `MMAP0..7` (`0xF000_F000`–`0xF000_F03C`), `CACHE_MASK`, `CACHE_OPERATION` from the CPU, and from a COP job | R0 | none | COP reads MMAP1 = 0 while the CPU reads `0x2000_3C00`: the windows are banked per core. Both read the same: one register set, and the "hidden from the COP" comment needs another explanation. Low byte `0x84` read back as written or altered: whether it is a stored field or partly status | — | A |
| HX-A5 | PP#3, PP#5 | Read `CACHE_CTL` `0x6000_C000` on each core; the local vector table `0x6000_F100`–`0x6000_F17F` | R0 | none | Words shaped like ARM instructions (e.g. `0xE59F_F018`, `ldr pc,[pc,#24]`) against words shaped like addresses (`0x1xxx_xxxx`, `0x4xxx_xxxx`) hint at the table's format; HX-D4 settles it. All zero or all ones: no hint | — | A |
| HX-A6 | PP#22 (our baseline) | `PP_VER1`/`PP_VER2` (`0x7000_0000/0004`) as ASCII; the hardware-version word at NOR `0x405C` through the mapping the firmware already reads it by | R0 | none | "PP5022C" and `0x000B_0011`: our unit's identity is [silicon] (today [external]). Anything else updates `overview.md` | — | A |
| HX-A7 | PP#23, timers | Sample `0x6000_6038` (pairs, as Apple does), `0x6000_5014` (`RTC`), `0x7000_2804` and `USEC_TIMER` 1,000 times at fixed `USEC_TIMER` spacing, at 24, 30 and 80 MHz, with audio stopped and then playing | R0 | none | `6038` advances at a rate that follows the core clock: a cheap cycle counter. A fixed rate across clocks: an independent clock (its rate recorded). No order: an entropy source. `5014` steps once a second: a seconds counter. `2804` moves only while playing: I2S-related [hypothesis until its bits are matched to events] | — | A |
| HX-A8 | PP#6 (baseline) | Snapshot the interrupt controller (`0x6000_4000` block, both banks, per-core enable and status, raw `INT_STAT` `0x6000_4010`) at idle and during playback. No forcing | R0 | none | Raw bits set that no enabled handler owns are candidate sources for HX-D5 | — | A |
| HX-A9 | PP#17 (baseline) | Snapshot `WHEEL_CTL` `0x7000_C100` and `WHEEL_STAT` `0x7000_C104` at idle, while the wheel is touched, and with hold on (controller off); count packets and `0x3A` packets per second | R0 | owner: do (touch the wheel, toggle hold) | Baseline for HX-D7; bits that change only while touched are status, not control | — | A |
| HX-A10 | PP#14, PP#15 | Log `0x7000_0028`, `0x7000_002C`, `0x7000_0060` before and after each step of the USB bring-up (`usb.md` §2), and `PORTSC1`/`OTGSC` at each enumeration | R0 | owner: do (plug USB-C into the Mac three times) | Any `hs=0` session whose PHY values differ from the `hs=1` ones names the step that went wrong. Identical values with `hs=0`: the cause is outside the PHY (cable, host port, timing) | — | A |
| HX-A11 | PP#19 | Read `0x6000_60A4`; time 100 one-byte and 100 four-byte PMU reads with `USEC_TIMER` at 24, 30 and 80 MHz (minimum of each set) | R0 | none | Two-point fit gives per-byte time and per-transaction overhead; ~9 bit times a byte gives the bus rate [estimate]. Per-byte time scales with the core clock: I2C is clocked from it. Constant: an independent clock. HX-X4 (scope) gives the exact figure | — | A |
| HX-A12 | PP#20, PP#18 (state), §8 | PMU (I2C `0x08`) registers `0x00`–`0x3F`, **skipping `0x02`–`0x04`** (the interrupt flags, which may clear when read [hypothesis]). At entry (before the main firmware's PMU init) and after it | R0 | none | Register `0x00` compared with the PCF50605 and PCF50607 datasheets names the part. Regulator bytes against the datasheet's table give the voltage formula from a document, not from Rockbox. `OOCC1` (`0x08`) as found records the wake configuration. Entry equals Rockbox's values: the bootloader already ran the init; Apple's defaults then need HX-D8 | — | A |
| HX-A13 | Pin map (`gpio-i2c.md` §3), BCM#28 (line level) | All twelve GPIO ports' `ENABLE`, `OUTPUT_EN`, `OUTPUT_VAL`, `INPUT_VAL`, `INT_EN`, `INT_LEV` (not `INT_CLR`), `GPO32_VAL`/`GPO32_ENABLE`. One snapshot per state: idle; hold on; headphones out/in; USB-C in/out; charging; backlight off; playing; BCM powered off | R0 | owner: do (each state on cue, SELECT to advance) | A pin that changes with exactly one stimulus confirms or adds an assignment [silicon]. A pin that changes with none stays unassigned. GPIOC bit 6's level with the BCM on and off is the baseline for HX-B1 | — | A |
| HX-A14 | PMU ADC (`clocks-power.md` §6), accessory ID | ADC channels 0–7, each read three times, in the HX-A13 states | R0 | owner: as HX-A13 | Channel 2 tracks the logged battery voltage: scale confirmed. Channel 4 with nothing attached is the accessory-ID baseline (the Bluetooth unit compares against it, §5). Channel 7 changes with charging. Channels that move with no stimulus are recorded unexplained | HX-A13 | A |
| HX-A15 | Headphone remote (coverage) | Repeat HX-A13/A14 with a four-pole headset with inline buttons plugged in, owner pressing each button | R0 | owner: do; a headset with inline buttons | A GPIO or ADC change per button: a remote input path exists and is documented. No change: [silicon] no headphone-jack remote input on our 5.5G | HX-A13 | A |
| HX-A16 | §8 (IDE, I2S, PWM) | IDE controller registers (`IDE0_CFG`, the timing words, `0x6000_60C4`) **only while the drive is idle**, never during DMA (`storage.md`); I2S control registers (not FIFO data) stopped and playing; `PWM0_CTRL` `0x7000_A000` during a wheel click | R0 | owner: do (play, click) | Values equal to what the driver wrote: the write path is [silicon]. Bits that differ from the written value are status bits, recorded for the manual | — | A |
| HX-A17 | PP#29, DP-4 | Read the existing `m0 disk sleeps=… irq=` line (built in Fit-1 C4b) after a playback | R0 | owner: play a song for 2 min | `irq=I` with I > 0 and no `polling`: the IDE interrupt fires [silicon]. I = 0: it doesn't, and the CPU polls. No line: a logging fault, fixed before trusting Fit-3b's counters | — | A |
| HX-A18 | BCM#4, #6, #9, #10, #11, #14 (GPIO functions), #15, #16, #17 | With our image idle, at BCM 27 and 150 MHz: read words of `0x1000_0400` (+`0x00`–`0x1C`), `0x1000_0C00`, `0x1000_1400`, `0x1000_1C00`–`0x1000_1C50`, `0x1000_2800` (+`0x00`–`0x6C`), `0x1000_2C00` (all but its FIFO address), `0x1000_4000`, `0x1000_4C00`, each twice, 32-bit only. Only blocks Rockbox, Apple or our host already read; then power-cycle the BCM. Before the run, summarise S8's LCD clock reading (no device) for BCM#4 | R0 | none | Clock words that change between 27 and 150 MHz name the clocks that follow the PLL (BCM#9), including the LCD bus's (BCM#4). The regulator's `[7:4]` value at each clock is recorded (BCM#11). The panel ID our driver detects is logged for BCM#17 | dev45 image | A |

### Batch B: value to ProkBox, mostly R1

| ID | Question | Experiment | Class | Needs | Outcomes → meaning | Depends | Batch |
|---|---|---|---|---|---|---|---|
| HX-B1 | BCM#28 | Our image drives GPIO 23 in a host-requested pattern (then back to input). The PP samples GPIOC bit 6 and its `INT_STAT` with the interrupt masked, then enables source 32 with a counting handler | R1 | none | Level follows the pattern: the line is wired as Rockbox's comment says. `INT_STAT` sets: edge or level noted. Count > 0: the feed thread can sleep on the chip instead of polling (Fit-3b's port-poll counts size the gain). No change: the line is absent, or GPIO 23 needs an alternate function (HX-A18's GPIO words) | HX-A13, HX-A18 | B |
| HX-B2 | PP#7 | CPU sets a queue bit with the COP asleep and source 4 enabled for it; count wakes and latency; then the reverse | R1 | none | Wakes within µs: a cheap COP hand-off for Fit-3c (art decode on the COP). No wake: keep the tick pulse | — | B |
| HX-B3 | PP#8 | Both cores do 10⁶ `SWP`-locked increments of an uncached IRAM word; control arm: 10⁶ unlocked increments each | R1 | none | Control loses counts and the locked sum is exactly 2·10⁶: `SWP` is atomic across cores. Locked sum short: it is not; Peterson locks stay. Control loses none: no contention happened, rerun | — | B |
| HX-B4 | PP#30 (rest), DP-7 | General channel 3 IRAM → IRAM with patterns; an IDE read of a known file region into IRAM and into SDRAM, compared | R1 | none | IRAM works for either master: buffers there need no cache maintenance. Untouched: SDRAM only | E2-2 | B |
| HX-B5 | PP#31, DP-6 | A COP job reads a harmless BCM register (the GPIO level word) with the CPU idle, then writes and reads back a scratch word | R1 | none | Correct: a COP-side port feeder is possible. Wrong or hung: the port stays CPU-only (today's design) | — | B |
| HX-B6 | PP#28, DP-5 | Read the same 64 MB region as 1-, 8-, 64- and 256-sector commands at UDMA2, bypassing caches; time each command and the CPU busy share | R0 | none | Rate rises with command size: per-command overhead is the limit. Flat at ~21 MB/s: the adapter's flash. CPU busy ≈ wall time: the CPU's per-command work | Fit-3b IDENTIFY log | B |
| HX-B7 | BCM#47, DP-1 | PP pinned at 30 MHz: one 153,600-byte frame into SRAM scratch and into SDRAM scratch, BCM at 27 and 150 MHz, decoder idle and busy (8 arms), pattern-verified | R1 | none | Which of BCM clock and target memory sets the port's write rate; decides whether video uploads should go via SRAM | dev45 image | B |
| HX-B8 | BCM#44 | A timing kernel on the BCM core alone, then while the host streams a full frame, at both BCM clocks | R1 | none | Equal counts: port traffic is free to the chip. Lower: the cost per frame, for the video budget | — | B |
| HX-B9 | BCM#3 | An image that counts entries per source (timer compare, LCD DMA completion, the host doorbell 23 raised by a host write) and reports them in the mailbox | R1 | none | Count > 0 per source: confirmed on silicon. Enables an idle-sleeping BCM image (power) and doorbell-driven feeds | dev45 image | B |
| HX-B10 | BCM#48, DP-3 | Only if dev45's 8a does not include it: 8a-0's pin probe during **music** with COP decode on, elapsed confirmed moving | R0 | owner: play a song | Clocks present: COP decode doesn't stop them, and S36c's silence had another cause. Absent: COP decode stops PCM, a bug to fix before Fit-3c | dev45 8a | B |

### Batch C: power, with a meter (R0/R1)

| ID | Question | Experiment | Class | Needs | Outcomes → meaning | Depends | Batch |
|---|---|---|---|---|---|---|---|
| HX-C1 | PP#25, BCM#12 | Current per state, each held 60 s with a timestamped `state` line in `ipodui.log`: standby; menu with backlight on and off; busy loop at 24, 30 and 80 MHz; music, screen off, DAC standby on/off; BCM powered off; video playing; storage streaming; USB connected | R0 | meter | mA per state replaces the voltage-drop estimates in `hardware.md` (the budget table is the orchestrator's) | §4 meter | C |
| HX-C2 | `clocks-power.md` §8 | 24 MHz PLL-off idle against 30 MHz idle, 5 min each, twice, alternating | R0 | meter | The PLL's own cost: whether `cpu_pll_hold()` policy is worth keeping or extending | HX-C1 | C |
| HX-C3 | `input.md` §4; haptics | Wheel controller off (`DEV_OPTO` clear, as on hold) against on; 100 haptic pulses a second against none | R0 | meter | The controller's draw (unknown today) and the haptic's per-pulse cost | HX-C1 | C |
| HX-C4 | BCM#12 | BCM idle at 27 against 150 MHz, our image's `sleep` loop against a busy loop, powered off | R1 | meter | Whether an idle-sleeping BCM image is worth building (with HX-B9) | HX-C1 | C |
| HX-C5 | PP#26 | One full rundown, music on loop, screen off, `HAVE_BATTERY_LEARN` and the power log on. Starts at the end of the session; runs for days | R0 | time only | A measured discharge table for the 3000 mAh cell, checking #70's learned one | — | C |

### Batch D: the PP core lab (guide completeness, R1)

| ID | Question | Experiment | Class | Needs | Outcomes → meaning | Depends | Batch |
|---|---|---|---|---|---|---|---|
| HX-D1 | PP#2 | From IRAM, IRQ off: program unused MMAP window 2 over IRAM at a free logical address; write through it, read through IRAM; vary the mask and the low flag byte (`0x84` against `0x00`) on this window only; zero it after | R1 | none | Data correct through the window: the field layout is confirmed per mask value. A flag value that blocks access names what the byte gates | HX-A4 | D |
| HX-D2 | PP#3 | Stride-timing over 4–32 KB with the cache on; read both cores' status arrays after controlled accesses | R1 | none | Miss knees give size and associativity; whether the cores share one data array | — | D |
| HX-D3 | PP#4 | IRQ off, a dirty test region only: set `CACHE_OPERATION`'s invalidate bit; read memory and the status words after. Reboot after the step | R1 | none | What invalidate does to dirty lines (discard, write back, corrupt), explaining Rockbox's slow workaround | HX-D2 | D |
| HX-D4 | PP#5 | Fill the local vector table as handler addresses (arm 1) and as `ldr pc` instructions (arm 2); set `VECT_REMAP`; raise an `SWI`; a pre-written "started" line marks a hang | R1 | none; a hang needs owner: do (reset) | The arm that reaches the handler fixes the format; an alternative to remapping SDRAM to 0 | HX-A5 | D |
| HX-D5 | PP#6 | With `CPU_INT_EN` masked, force each source 0–63 by `INT_FORCED_SET` and read per-core status; live sources (timer 1, USB, wheel, GPIO) paused or skipped | R1 | none | A table of which status bit follows which forced source, including the UART, wheel and high-bank timers | HX-A8 | D |
| HX-D6 | PP#13 | `WAIT_REQ` with candidate IDs for the port, the IDE and the UARTs; completion timing against unpaced | R1 | none | A paced completion names an ID. None paced: the port has no request line, as Apple's use suggests | E2-2, E2-6 | D |
| HX-D7 | PP#17 | Vary one field of `0x000A_1F00` and `WHEEL_STAT` bits 24–27 at a time, 2 s each, full re-init at the end, while the owner turns the wheel | R1 | owner: do (turn the wheel) | A field that changes packet rate or errors is a divider or frame length; documented with the `0x3A` packet's behaviour | HX-A9 | D |
| HX-D8 | PP#24, HX-A1/A12 fallbacks | **Bootloader-gated:** the bootloader copies `CLOCK_SOURCE`, `PLL_CONTROL`, `DEV_EN`, `DEV_TIMING1` to reserved IRAM words at its first instruction; the main firmware logs them | R0 | owner sign-off (bootloader) | The ROM's hand-off state [silicon]; today "not recorded" (`boot.md` §1) | HX-A1 | D |

### Batch E: the BCM lab (guide completeness, R1, BCM power-cycled after each)

| ID | Question | Experiment | Class | Needs | Outcomes → meaning | Depends | Batch |
|---|---|---|---|---|---|---|---|
| HX-E1 | BCM#25, #43 | Pattern writes and reads through the ALT bank against the main bank; a lone 16-bit `DATA` write; `DATA` reads after one `RD_ADDR`; host `0xFE00_0000`, `0xC040_0000` | R1 | none | Each rule in `memory-access.md` §7a moves from untested to [silicon] | — | E |
| HX-E2 | BCM#42, #8 | Core writes a unique word in free SRAM and reads offset + `0x14_0000`, `0x20_0000`, `0x100_0000`, exception handler armed; then the host reads the same | R1 | none | Aliases or a fault above SRAM; cached/uncached views if any | HX-E3 | E |
| HX-E3 | BCM#5 | Undefined instruction, misaligned word store, odd halfword access, each counted by its vector | R1 | none | Vector numbers and whether faults are precise | — | E |
| HX-E4 | BCM#13, #14 | SDRAM → SDRAM DMA, logging status bits 9/11 and the state field per phase; force or provoke sources 8–10, 12–15, 20–22, 24–31 | R1 | none | DMA status meanings; an interrupt source table | HX-B9 | E |
| HX-E5 | BCM#1, #2, #18–#23 | The ISA bench: each open vector form, ALU ops 9/17/29, shifts ≥ 32, `msb(0)`, `vst8`/`vst8v` layouts, fetch line size, each against its scalar model | R1 | none | Each form passes or gets a quirk entry; `[hypothesis]` forms in `isa-vector.md` become [silicon] | — | E |
| HX-E6 | BCM#6 | Cache-on timing kernels; vector loads with the cache on; coherence against host writes; `0x1000_0C00` bit 16 toggled | R1 | none | Cache geometry, and whether our image can turn it on safely | HX-E5 | E |

### Batch F: clean-room re-verification by ear, eye and touch (R1)

| ID | Question | Experiment | Class | Needs | Outcomes → meaning | Depends | Batch |
|---|---|---|---|---|---|---|---|
| HX-F1 | §8 (I2S, WM8758) | A 440 Hz left / 880 Hz right tone; change one `IISCONFIG`/`IISFIFO_CFG` field at a time, restore after 3 s | R1 | owner: listen | Swapped sides, noise or silence name each field's role; the I2S table loses its [rockbox-derived] tag field by field | — | F |
| HX-F2 | BCM#36, #37, #38 | After stage 8a-1 passes: the same tone through the BCM path, one control word at a time | R1 | owner: listen | Master/slave, word length, format and order bits; channel 5's length unit | dev45 8a-1 | F |
| HX-F3 | Haptics (`hardware.md`, issue #23, H8) | Sweep `PWM0_CTRL` period and pulse length; one arm with PWM stopped | R1 | owner: feel | The motor follows the PWM line (stopped arm feels nothing); the owner picks a pulse | — | F |
| HX-F4 | §8 (USB PHY) | A test bring-up that skips or reorders one PHY step at a time; record enumeration and `hs` | R1 | owner: do (plug USB-C) | Steps that matter and steps that don't; feeds PP#14/#15 | HX-A10 | F |
| HX-F5 | §8 (backlight) | Step the GPIOD bit 7 pulse protocol through its levels | R1 | owner: look | Level ↔ pulse count table [silicon] | — | F |

### Batch G: clocks, power states and pin muxing (R2, late)

Guards for every row: the R2 standing guards (§1), plus Batch A's baseline for the same registers, and a full backup before the session.

| ID | Question | Experiment | Class | Needs | Outcomes → meaning | Depends | Batch |
|---|---|---|---|---|---|---|---|
| HX-G1 | PP#9 | Select sources 1, 3, 4 (16/33/48 MHz taps), then SLOW and FAST with `DIV` values, each 100 ms from IRAM, timed against `USEC_TIMER`; back to the crystal after each; timers checked in the 32 kHz setting | R2 | none | Each tap's real frequency; whether 48 MHz without the PLL exists (a power step between 30 and 80) | HX-A1, HX-A7 | G |
| HX-G2 | PP#10 | Step `DEV_TIMING1` down from HX-A2's value at each clock while a pattern memory test runs on scratch; stop at the first error; restore; reboot | R2 | none | Minimum safe value per clock, and the memory speed gained | HX-A2 | G |
| HX-G3 | PP#18 | Standby with `CHGWAK` + `EXTONWAK` only (never fewer); owner tries each button, the wheel and hold; then USB-C | R2 | owner: do | The input wired to `EXTONWAK` | HX-A12 | G |
| HX-G4 | §8 (IDE timing) | Reads only, of a pattern-verified 8 MB file, sweeping one timing-word field at a time; no writes to storage during the stage | R2 | none | Field meanings and margins; storage loses its [rockbox-derived] timing tag | HX-A16 | G |
| HX-G5 | BCM#9, #10 | Gate one block clock (`0x1000_1C2C`–`0x50`) or one run-control bit at a time with the image idle; power-cycle after each | R2 | none | Which block each clock feeds; gateable clocks for power | HX-A18 | G |
| HX-G6 | BCM#7 | SDRAM self-refresh and power-down entry and exit, with a retention check | R2 | none | A sequence for keeping BCM SDRAM across idle | HX-B9 | G |
| HX-G7 | `gpio-i2c.md` §4 | Clear one unnamed `DEV_EN` bit (5, 8, 30, 31 of the start-up `0xC200_0124`) at a time for 1 s, watching which blocks stop answering | R2 | none | Names for the unnamed enable bits | HX-A1 | G |

### Batch H: R3, not planned on the only unit

| ID | Question | Experiment | Class | Needs | Outcomes → meaning | Depends | Batch |
|---|---|---|---|---|---|---|---|
| HX-H1 | PP#20 (formula by writes) | One regulator step (e.g. LCD supply) and an ADC readback | R3 | owner go per run | Prefer the datasheet plus HX-A12's reads; don't run | HX-A12 | H |
| HX-H2 | BCM#11 | Write the regulator's `[7:4]` bits | R3 | owner go per run | Same advice | HX-A18 | H |
| HX-H3 | PP#27 | Flash JEDEC ID in autoselect mode | R3 | a spare unit only | Never on the owner's unit: Apple's recovery lives in that flash | — | H |
| HX-H4 | §8 (PLL rules) | The PLL core below 96 MHz, or the core above 80 MHz | R3 | owner go per run | Not planned | — | H |

### Batch X: needs equipment or other hardware (§4)

| ID | Question | Experiment | Class | Needs | Outcomes → meaning | Depends | Batch |
|---|---|---|---|---|---|---|---|
| HX-X1 | PP#21, BCM#39 | Scope the WM8758's DACDAT net with each chip driving (after 8a-1) | R0 | scope; opening the unit | Selector, gate or merge: how BCM audio reaches the codec | dev45 8a-1 | X |
| HX-X2 | PP#22, PP#1 (32 MB) | Batch A's dump on a 5G (and a 32 MB unit if available) | R0 | second unit | The 5G's chip ID; SDRAM controller values for 32 MB | HX-A3, HX-A6 | X |
| HX-X3 | Storage fold (#79) | Batch A's IDE rows, HX-A17, HX-B6 and spin-up timing on an HDD unit | R0 | HDD unit | `--storage=hdd` measured instead of [rockbox-derived] | HX-B6 | X |
| HX-X4 | PP#19 (exact) | Scope SCL during HX-A11's reads | R0 | scope | The exact bus rate | HX-A11 | X |
| HX-X5 | BCM#15, #60–#62 (TV out) | TV-out block bring-up with a composite AV cable, for when videos return. Now specified step by step as PB-6–PB-9 in `bcm2722-reference/brief-map-peripherals.md` §11 (Apple's full register sequence is known) | R2 | AV cable, a TV; owner's go | The FIFO format, the field bit, scaling; keeps the TV-out door open | PB-1 (read-only census, with PB-2–PB-4 the first wave) | X |
| HX-X6 | BCM#17 | Log the panel ID our driver detects on a second unit | R0 | second unit | Another panel variant [silicon] | HX-A18 | X |

**Counts.** Per batch: A 18, B 10, C 5, D 8, E 6, F 5, G 7, H 4, X 6 (69). Per class: **R0 30**, **R1 28**, **R2 7**, **R3 4**.

**Not device questions (static or doc work, so not rows):** BCM#26 (Apple's two transport descriptions), BCM#27 (NOR `vmcs` commands), BCM#24 (bootstrap bytes: static first). **Answered:** PP#16 (S36c), BCM#34–#35 (S32–S36c), BCM#45 (`p14-dev45`). **Scheduled elsewhere:** PP#11, #12, BCM#41 (E2 lab), BCM#0b, #29–#33 (dev45 video), BCM#40 (moot while #35 says the line floats).

---

## 3. Batches

Each batch is one developer build after the next one, sized to an owner session of about 30 minutes. Order: Batch A (baselines everything else needs), then by value to ProkBox (B, C), then guide completeness (D, E, F), then R2 (G). H is listed only to say no. X waits for equipment.

| Batch | Build content | Value | Session |
|---|---|---|---|
| **A** | One developer stage, "Register dump", with sections A1–A18 and a canary per section; the BCM block is a devtest stage | Baselines for every later batch; identity; pin map | ~25 min, mostly owner actions on cue |
| **B** | Port and inter-core labs | Feed thread sleeps (B1), COP hand-off (B2/B3), storage speed (B6), video upload path (B7) | ~20 min, automatic |
| **C** | State-holding stage that writes `state` lines | Measured battery budget | ~30 min plus a rundown over days |
| **D** | PP core lab | Memory map, cache, vectors, interrupts for a from-scratch kernel | ~15 min; D8 only with bootloader sign-off |
| **E** | BCM lab images | Port rules, exceptions, ISA | ~15 min, automatic |
| **F** | Tone, haptic and backlight stages | Clean-room audio, USB, backlight | ~25 min, owner listening |
| **G** | R2 stages, each behind a confirmation screen | Power steps, memory speed, wake source | ~30 min; owner present throughout |

### Owner test cards

**Batch A**
1. Charge above 50 %; unplug USB-C. Developer → Debug → Register dump.
2. Follow the cues, pressing SELECT after each: hold on/off; headphones out/in; USB-C in/out (three times); backlight off; touch the wheel.
3. If you have earphones with inline buttons: plug them in and press each button on cue.
4. Play a song for 2 minutes, then stop.
5. Plug in USB-C and copy `ipodui.log` and `bcmtest-results.log` from the system folder.

**Batch B**
1. Developer → Debug → Lab B. Press SELECT to start; don't touch anything until "Done".
2. If asked, play a song for 1 minute.
3. Copy both logs over USB-C.

**Batch C** (with the meter, §4)
1. Fit the meter as described, set it to log, and note the clock time.
2. Developer → Debug → Power states. Leave the iPod alone; it steps through states by itself.
3. At "Done", note the time; copy the meter's file and both logs.
4. Then play music on loop with the screen off until it shuts down; plug in and copy the logs.

**Batch D**
1. Developer → Debug → Lab D. SELECT to start.
2. When asked, turn the wheel slowly for 30 s.
3. If the screen freezes for more than 30 s: hold MENU+SELECT to reset, then copy the logs (a freeze is a result).

**Batch E**
1. Developer → Debug → Lab E. SELECT; wait for "Done"; copy the logs.

**Batch F**
1. Headphones on, volume low. Developer → Debug → Lab F.
2. For each tone, press SELECT if it sounds right; LEFT/RIGHT (⏮/⏭) if it is on the wrong side; MENU if noise or silence.
3. For haptics, rate each pulse with ⏮ (weak), SELECT (good), ⏭ (strong). For backlight, press SELECT at each visible step.

**Batch G**
1. Charge above 50 %; keep the USB-C cable in reach. Developer → Debug → Lab G; confirm each step.
2. When it powers off: try each button, then the wheel, then hold; say which woke it. If none, plug in USB-C.
3. Copy the logs.

---

## 4. What needs equipment or other hardware

| Need | For | What to use | What it can and can't tell |
|---|---|---|---|
| **Current per state** | HX-C1–C4 (PP#25, BCM#12) | **Minimum:** an inline USB-C power meter that logs (typically 10 mA / 10 mV resolution, about 1 sample a second). With the cell fitted, the meter sees **charger input**, not system draw, so the cell must be disconnected and the unit run from USB (the board has no current sense: `clocks-power.md` §7). That path goes through the LTC4066, which adds its own loss and limits current to 100 or 500 mA (`usb.md` §4) | Resolves states tens of mA apart, averaged over seconds. **Can't** resolve per-event costs (a disk wake, a single frame), currents under ~10 mA (standby), or draw on battery. **Better:** a 0.1 Ω shunt in series with the battery's positive lead plus a logging multimeter or a USB oscilloscope/DAQ at ≥ 1 kHz; this measures the real battery current, including standby and bursts. Opening the unit and fitting the shunt is an owner job |
| **I2C speed** | HX-X4 (PP#19) | A scope or a cheap logic analyser on SCL | Exact rate and gaps. Fallback without one: HX-A11's software timing gives the per-byte time within a few µs |
| **BCM audio route** | HX-X1 (PP#21, BCM#39) | Scope on the DACDAT net (inside) | Which chip drives the codec's data, and when |
| **5G chip ID** | HX-X2 (PP#22) | A 5G unit running Batch A once (an install: owner's go and backup) | The 5G's `PP_VER`, and SDRAM values if it is a 32 MB unit |
| **Storage fold** | HX-X3 (tracker #79) | A hard-drive 5G/5.5G | HDD timing, spin-up cost and the IDE interrupt on a real drive |
| **Battery rundown** | HX-C5 (PP#26) | Time only: the power log and `HAVE_BATTERY_LEARN` | A measured discharge table; no equipment |
| **TV out** | HX-X5 (BCM#15) | A composite AV cable and a TV | Only when videos return |

---

## 5. Accessory mods: the Bluetooth unit

The unit may come soon; it is **the owner's wife's daily device**. Rules before anything else:
- **Read-only first.** A full backup of its disk before any session (read-only image, stored as the stock 7G's backups are), and its own firmware left untouched.
- **Never flash, patch or install** on it without the owner's explicit go for that unit. Installing our firmware is itself a write, so every experiment that runs our code on it needs that go first; until then only observation under its own firmware.
- **First establish what it is.** It may not be a 5G/5.5G at all: a 6G/7G Classic has a different SoC, and almost nothing in this guide applies to it. Record the model and serial numbers engraved on the back and shown in its own Settings › About; capacity; whether it still has a hard drive; and which mod is fitted.

### (a) Identification

- **Photos** of the back, the dock connector and, only if the owner opens it, the board and the mod (both sides), with the board number (`820-1763-A` 5G, `820-1975-A` 5.5G: `overview.md` §2).
- **Where the mod taps in**, one of:
  - the **dock connector**: the serial lines (iAP over UART 0) and the dock's line-out pins; powered from the accessory supply (`D2REGC1`) or the battery;
  - the **headphone or line-out analog signal** inside (a transmitter board fed like a pair of headphones);
  - the **internal I2S** (digital, between the PP and the WM8758);
  - **USB**;
  - **battery and power** (its own switch, or always on).
- Under its own firmware, note: does Bluetooth audio work; does the earphone's play/pause or skip control the iPod; does the mod drain the battery while the iPod is off.

### (b) What is public about current mods [unknown; untested by us]

All of this is from product pages and press, not checked on hardware:
- **Moonlit Market "Classic Connect 2"**: a replacement back for 5G–7G with Bluetooth 5.2 (aptX claimed), USB-C charging (data needs extra soldering) and wireless charging; installation via the ribbon (ZIF) connector; how audio reaches the Bluetooth module is not stated ([ifun.de](https://www.ifun.de/classic-connect-2-bluetooth-qi-und-usb-c-fuer-den-ipod-classic-277730/); [Clubic](https://clubic.com//actualite-580045-bluetooth-usb-c-chassis-aluminium-recharge-sans-fil-ressuciter-son-ipod-classic-en-2025-c-est-possible.html); [Hardware.com.br](https://www.hardware.com.br/noticias/mod-kit-ipod-classic/amp/)).
- **Boxy Pixel MKII electronics board**: for 5G–7G boards in Boxy Pixel housings; a "KCX" Bluetooth 5.0 emitter module, A2DP with SBC; a custom audio flex and a power flex; USB-C charge-only, with optional D+/D− pads on the 30-pin connector; AVRCP is not mentioned ([Boxy Pixel](https://www.boxypixel.com/products/ipod-mkii-electronics-pcb)).
- **DIY builds**: a stand-alone 3.5 mm Bluetooth transmitter board wired inside, fed from the analog audio, beside a flash adapter ([Instructables](https://instructables.com/Bluetooth-IPod-Classic)); older 5G builds did the same in 2008 ([Ubergizmo](https://ubergizmo.com/2008/02/5g-ipod-video-gets-bluetooth-hack); [Command-Tab](https://command-tab.com/2008/02/26/ipod-5g-hack-bluetooth-and-compactflash/)).
- **30-pin dongles** (external, e.g. BD-906): powered by the iPod, A2DP plus AVRCP remote control, which reaches the iPod over the dock's iAP ([iLounge](https://www.ilounge.com/index.php/reviews/entry/bd-906-ipod-ipod-a2dp-bluetooth-wireless-audio-dongle)).

The pattern [hypothesis]: internal kits take **analog** audio and send no control back, so the firmware sees nothing but perhaps the headphone-detect line; dock-based adapters speak **iAP**.

### (c) Staged experiments (R0/R1 first)

All need the owner's go for that unit; stock-firmware observation (BT-1) needs none.

| ID | What | Class | Needs | Outcomes → meaning |
|---|---|---|---|---|
| BT-1 | Under its own firmware: the observations in (a); time to connect; whether controls work | R0 | Bluetooth unit (no install) | Controls work → the mod sends something back (iAP or a remote line). Not → audio-only |
| BT-2 | After an install: Batch A's HX-A13/A14 snapshots with the mod powered and unpowered, connected and not | R0 | Bluetooth unit; owner go | GPIO or ADC channel 4 changes → accessory presence detection is possible; the headphone-detect pin asserted → it taps the headphone path |
| BT-3 | If it sits on the dock: route the dock UART's pins (`0x7000_008C`/GPO32 bits 2–3, `gpio-i2c.md` §3) and **receive only**, at 19,200 and 57,600 baud (Rockbox's iAP rates, `apps/iap/iap-core.c`), logging raw bytes with timestamps | R2 (pin mux), receive only | as BT-2 | Bytes framed as iAP (`0xFF 0x55` start, per Rockbox's iAP code [rockbox-derived]) → an iAP accessory; the lingo IDs in its identify message say which subset it needs. Nothing → not on the serial lines |
| BT-4 | Audio routing: play a tone with the headphone amp muted and the line output (WM8758 `OUT2`) enabled, then the reverse | R1 | as BT-2; owner: listen | Sound over Bluetooth only on one path → which output feeds it; then volume behaviour can match the 7G's (fixed line level vs headphone volume) |
| BT-5 | If it taps I2S: play known sample rates (44.1, 48 kHz) and listen for pitch or dropouts | R1 | as BT-2; owner: listen | The module follows the WM8758's clocks or needs a fixed rate |

Note: logging the UART traffic **while Apple's firmware runs** is impossible with our firmware (only one firmware runs at a time); BT-3 logs what the mod sends while ours runs. **Our own test unit has no dock connector** (USB-C mod, `hardware.md`), so every dock-based mod is testable only on the second unit.

### (d) What a kernel would need to drive it

| If the mod… | The kernel needs | Status |
|---|---|---|
| takes analog audio | Nothing beyond audio; optionally a line-out mode and presence detection (BT-2) | [unknown] until BT-1/BT-2 |
| speaks iAP on the dock | UART 0 (16550-style at `0x7000_6000` per Rockbox's header [rockbox-derived]) with autobaud between 19,200 and 57,600; an iAP subset (general lingo identify, simple remote, perhaps extended interface); `HAVE_IPOD_DOCK30` builds already carry Rockbox's iAP | [rockbox-derived]; BT-3 decides the subset |
| draws power from the dock | The accessory supply (`D2REGC1` on/off, `clocks-power.md` §6), switched with sleep | [rockbox-derived] |
| taps I2S | Fixed-rate playback, or nothing if it follows the clocks (BT-5) | [unknown] |

A second unit also helps beyond Bluetooth: if it is a 5G it answers HX-X2; with a hard drive, HX-X3; its panel, HX-X6; all as R0 runs after an install.

---

## 6. Coverage

Today's state per feature, and what each batch would raise to [silicon][measured]. "Baseline" means read back as configured (Batch A); later batches change one thing at a time to learn meanings.

| Feature | Today | Raised by | Stays [rockbox-derived] after all batches |
|---|---|---|---|
| Boot | ROM chain [external]; hand-off clocks not recorded | A1, A6, D8 | The bootloader load protocol (ipodpatcher layout) [rockbox/external] |
| Clocks | Encoding [rockbox-derived]; 24/30/80 work [silicon] | A1, A2, A7, C2, G1, G2 | The PLL's ≥ 96 MHz rule and bit 27 (H4 not run) |
| Memory | MMAP, cache ops [rockbox-derived]; SDRAM init [apple-RE] | A3, A4, A5, D1–D3, B4 | SDRAM controller init (read only, never re-run); `CACHE_PRIORITY` |
| Cores | Peterson locks, mailbox sleep [rockbox-derived] | B2, B3, B5 | `PROC_CTL` sleep bit details beyond what B2 exercises |
| Interrupts | Rockbox's source list | A8, D4, D5, A17, B1 | — |
| Timers | µs timer [rockbox-derived] | A7, G1 | — |
| LCD / BCM | Port [rockbox-derived + silicon]; image [ours, silicon] | A18, B1, B7–B9, E1–E6, G5, G6 | Bootstrap bytes, the port power-down's two unknown steps |
| Video | dev45 work | B7, B8, B9 (with dev45) | — |
| TV out | nothing | A18 (read), X5 | — |
| Audio (PP I2S, WM8758) | [rockbox-derived]; WM8758 datasheet public | A16, F1 | Codec power-up order (datasheet, not ours) |
| Line out | Not on this unit's dock (removed) | BT-4 (second unit) | Dock line-out pins |
| Headphone detect | GPIOA bit 7 [rockbox-derived] | A13 | — |
| Headphone remote | nothing | A15 | — |
| Dock (iAP / serial / USB / FireWire) | Removed on our unit | BT-3 (second unit only) | iAP framing and lingos; FireWire (unused) |
| Clickwheel | Packet format [rockbox-derived] | A9, D7, C3 | The `0x3A` packet's semantics if D7 doesn't explain it |
| Hold | GPIOA bit 5 [rockbox-derived] | A13 | — |
| Backlight | Pulse protocol [rockbox-derived] | A13, F5 | — |
| PMU | PCF50605 driver [rockbox-derived] | A11, A12, A14 | Regulator writes (H1 not run; datasheet instead) |
| Battery / charging | ADC scale, LTC4066 pins [rockbox-derived]; learned table [ours] | A13, A14, C1, C5 | LTC4066 CLDIS (left alone) |
| RTC | PMU registers [rockbox-derived] | A12 | Alarm wake (would need G3-style standby) |
| Storage (flash) | UDMA2 [silicon; rockbox-derived timing] | A16, A17, B4, B6, G4 | — |
| Storage (HDD) | [rockbox-derived] | X3 (HDD unit) | Spin-down policy details if no HDD unit |
| USB device | Works [silicon]; PHY [rockbox-derived] | A10, F4 | The ARC core internals (standard, not described) |
| Clicker / haptics | Piezo PWM [rockbox-derived] | A16, C3, F3 | — |
| Sleep / standby | `OOCC1` [rockbox-derived] | A12, C1, G3 | — |
| Accessory mods | nothing | BT-1–BT-5 | Whatever BT-1 shows needs Apple's authentication (out of scope) |

**Still [rockbox-derived] after every batch, in short:** the bootloader's partition and image layout; the BCM bootstrap bytes and power-down's unknown steps; the PLL's ≥ 96 MHz rule; the SDRAM controller init (never re-run on purpose); `CACHE_PRIORITY`; the regulator voltage formula unless the datasheet settles it; iAP (needs a dock unit, and BT-3 only shows what one accessory sends); FireWire.

---

## 7. Results so far: USB on the USB-C 5.5G (2026-10-16/17, #81)

The backlog above is a plan. These are experiments that did run, on the owner's USB-C-modded unit with flash storage and macOS as the host. Chronology and commits: [`usb-history.md`](usb-history.md). Findings are one unit, one conversion board.

| Experiment | Result | Date | Verdict |
|---|---|---|---|
| CPU boost held for the USB session (`usb_boost`) A/B | Enumerated at high speed but repeatedly gave no usable storage (`Device not configured`); 68 genuine resets, active-dTD stall unchanged. Baseline without it read all 841 MiB of `Mythos.m4b` at 11.2 MiB/s | 2026-10-16 | **Unusable.** Do not hold a boost. |
| USBMODE stream-disable (`usb_sdis`) | Requested bit read back unset (`sdis=1/2`, USBMODE `0x02`); 167 resets in 351 s, same `ov80` state, same ~304 ms; apparent passes were host retries | 2026-10-16 | **No-op on PP5022.** Not a result for or against. |
| TXFILLTUNING write at `0x164` | Read-back 0; dead enumeration in 2 of 5 sessions of the build that wrote it; untouched by default afterwards | 2026-10-16 | **Do not write.** Correlation, not proof. |
| E2 off (`lcd_e2off`) | Failed at the first macOS mount | 2026-10-16 | **Withdrawn.** Not evidence against the display path; E2 stays on (`data-paths.md` D14). |
| Host automounter race | Helper sleeps 27 s after one failed mount; a manual mount wins for ~30 s after an early failure | 2026-10-16 | **Host-side confound.** A manual mount proves nothing about the attach. |
| Serial throughput (see below) | Baseline 11.2 MiB/s over 841 MiB and 8.0 MiB/s over 128 MiB; serial read-ahead 15.7-17.5 MiB/s; true no-overlap ~2.5 MiB/s in the first 18 s of a mount; flash ceiling 21-22 MiB/s | 2026-10-16/17 | See below |
| Storage handover time | Before the fix: 54.2 s (`req=-120 ack=54200`, 7 acks), host reset 4-5 times. Final acker found at 6660 ms: tagcache. After `0ded0466a6`: clean run acked in 240 ms. Later: tagcache scan quiesce (`248ee66c81`) and artcache stop gate (`7d5e7fc086`) | 2026-10-16/17 | **Root cause found and fixed**; re-test handover timing every round |
| 4 KiB dTDs, prime retry, overlay reset | No effect on the first-command timeout | 2026-10-16/17 | Hygiene; keep |
| Static INQUIRY buffer | `inq2data=1`, first reply completes | 2026-10-17 | **Fixes the never-retired INQUIRY** |
| ARC re-init at attach vs normal | Not run to a verdict | 2026-10-17 | Open |

**Serial throughput, measured.**

| Build or setting | Result |
|---|---|
| Baseline (no marker): 841 MiB `Mythos.m4b` | 11.2 MiB/s, no disconnect |
| Baseline: 128 MiB `Mythos.m4b` | 8.0 MiB/s (16.01 s) |
| Manual-mount control, 64 MiB | 9.3 MiB/s; a 256 MiB read disconnected at 166 MiB |
| Serial read-ahead (`fa571b6895`), marker path | 15.7-17.4 MiB/s, 384 MiB past the old 166 MiB failure |
| Same, no-marker production build | Failed at 345 MiB after 19.74 s (17.5 MiB/s average) |
| True no-overlap (`41103a9158`, with `23d1223bba` load-before-send) | ~2.5 MiB/s in the first 18 s of a native mount (46 MiB); real writes clean (`wr=19173`, `wrfail=0`) |
| Flash adapter alone | 21-22 MiB/s |

**Handover, 54 s case.** The `usb hand` log line proved the wait: the host's first CBW was held (WAITING_FOR_STORAGE, nothing primed, `e00`). Each slow handover coincided with the end of a `db scan ms=~60000`. Six threads acked at once; the seventh was tagcache, still scanning or committing. Fix: tagcache acknowledges at once and runs its changelog export after disconnect (`0ded0466a6`); a lock-free quiesce flag stops scan and commit at their checkpoints before the GUI acknowledges (`248ee66c81`, `ec96be8910`); artcache stops cooperatively at panel-decode rows and its final flush (`7d5e7fc086`). A commit cut this way keeps its temp database and is retried after disconnect (`5a1d396d87`).
