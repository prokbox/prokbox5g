# Clocks and power

## 1. Clock sources [rockbox] `system-pp502x.c` (`set_cpu_frequency` comments)

| Source | Frequency | Notes |
|---|---|---|
| 32 kHz | 32.768 kHz | from the PMU's clock output [hypothesis]; used for the "sleep" setting |
| main crystal | **24 MHz** | the reference for everything; the "external device clock" to the WM8758 is derived from it |
| PLL | 96 MHz minimum on PP5022 (see §2) | |
| fixed taps 16, 33, 48 MHz | as named | listed in the `CLOCK_SOURCE` encoding; Rockbox never selects them |

## 2. The PLL

| Address | Name |
|---|---|
| `0x6000_6034` | `PLL_CONTROL` |
| `0x6000_603C` | `PLL_STATUS` |
| `0x7000_0020` bit 30 | `DEV_INIT2.INIT_PLL` — PLL power |

`PLL_CONTROL` fields, as Rockbox's values and comments reveal them [rockbox] (field boundaries partly [hypothesis]):

| Bits | Meaning |
|---|---|
| 31 | PLL enable |
| 27 (`0x0800_0000`) | set before switching away from the PLL and when enabling (meaning unknown; Rockbox ORs `0x0800_0000` before every switch and `0x0C00_0000` before the 32 kHz switch) |
| `[21:20]` | **post-divider** (PP5022): 0 = /1, 1 = /2, 2 = /4, 3 = /8 |
| `[15:8]` | multiplier M |
| `[7:0]` | divider N |
| `0x0A00_0000` | always present in Rockbox's values (`0x8A…`) |

f_PLL = 24 MHz × M / N / post-div. **On the PP5022 the PLL's own (pre-divider) output must be at least 96 MHz** [rockbox] comment ("must be run at >= 96MHz"); the post-divider brings it down. The PP5020 has no post-divider and needs an extra unlock write for > 66 MHz.

Values Rockbox uses on PP5022 [rockbox]:

| Target | `PLL_CONTROL` | M | N | post | PLL core | Output |
|---|---|---|---|---|---|---|
| 80 MHz | `0x8A12_1403` | 20 | 3 | /2 | 160 MHz | 80 MHz |
| 30 MHz | `0x8A22_0501` | 5 | 1 | /4 | 120 MHz | 30 MHz |

Lock: poll `PLL_STATUS` bit 31 until set. (The PP5020 path instead waits 500 µs, and writes `PLL_STATUS = 0xD19B` to unlock > 66 MHz.) [rockbox]

## 3. `CLOCK_SOURCE` (`0x6000_6020`)

Four 4-bit source selectors in bits `[15:0]` (#1 … #4), and bits `[31:28]` choosing which selector is active. Rockbox always writes `0x2…`, i.e. **selector #2 active**, and writes the same code to all four. [rockbox] comments

Source codes: 0 = 32 kHz, 1 = 16 MHz, 2 = 24 MHz, 3 = 33 MHz, 4 = 48 MHz, 5 = SLOW, 6 = FAST, 7 = PLL. (Apple's PP5022 driver decodes 5 and 6 as fixed 20 and 40 MHz, and 2 and 4 as scaled by a second PLL at `0x6000_6048` when its bit 31 is set [apple]; the conflict is open question #9. Apple's PLL recipe, a 125 MHz ceiling, a speed grade in `STRAP_OPT_A[6:5]` and its `DEV_TIMING1` rule are in `mac-and-dvs.md` §3.)

- SLOW = 24 MHz / (`DIV_SLOW` + 1), `DIV_SLOW` in bits `[19:16]`.
- FAST = PLL / (`DIV_FAST` + 1), `DIV_FAST` in bits `[23:20]`.

Values written [rockbox]:

| Value | Meaning |
|---|---|
| `0x2000_2222` | 24 MHz crystal |
| `0x2000_7777` | PLL |
| `0x2000_0000` | 32 kHz |

The core clock and the bus clock appear to be one: Rockbox changes the memory wait states (`DEV_TIMING1`) with it (§4). Peripheral clocks (timers, I2S reference, USB, IDE) are independent of `CLOCK_SOURCE`. [rockbox] by usage

## 4. Switching frequency (Rockbox's sequence) [rockbox]

Rockbox's four settings, from `system-target.h`: `CPUFREQ_SLEEP` 32,768 Hz, `CPUFREQ_DEFAULT` 24 MHz, `CPUFREQ_NORMAL` 30 MHz, `CPUFREQ_MAX` 80 MHz.

**To 80 MHz:**

```
DEV_INIT2   |= INIT_PLL           ; PLL power
PLL_CONTROL |= 0x8800_0000         ; enable
suspend()                          ; §cores-and-threads 2: stall both cores
CLOCK_SOURCE = 0x2000_2222         ; run from 24 MHz while the PLL moves
DEV_TIMING1  = 0x0000_0303
resume()
PLL_CONTROL  = 0x8A12_1403         ; 80 MHz
while (!(PLL_STATUS & 0x8000_0000)) ;
suspend()
DEV_TIMING1  = 0x0000_0808         ; slower memory timing for the faster clock
CLOCK_SOURCE = 0x2000_7777         ; PLL
resume()
```

**To 30 MHz:** the same with `0x8A22_0501`, and `DEV_TIMING1 = 0x0303` at the end.

**To 24 MHz (default, PLL off):** `PLL_CONTROL |= 0x0800_0000`; suspend; `CLOCK_SOURCE = 0x2000_2222`; `DEV_TIMING1 = 0x0303`; resume; `PLL_CONTROL &= ~0x8000_0000`; `DEV_INIT2 &= ~INIT_PLL`.

**To 32 kHz:** `PLL_CONTROL |= 0x0C00_0000`; suspend; `CLOCK_SOURCE = 0x2000_0000`; resume; PLL off and unpowered.

Rules:

1. Every `CLOCK_SOURCE` write is bracketed by `scale_suspend_core()`, with IRQ and FIQ disabled and the other core held (`cores-and-threads.md` §2).
2. Never retune the PLL while running from it: step to 24 MHz first.
3. Raise `DEV_TIMING1` **before** selecting the faster clock and lower it **after** selecting the slower one ([hypothesis] that it is a wait-state field; the ordering is Rockbox's).
4. A multi-core lock (`cpufreq_cl`) serialises frequency changes between the cores.
5. The ATA DMA timing register assumes 80 MHz (`storage.md`); UDMA modes above 2 need the CPU boosted.

**Rockbox's policy** [rockbox] `firmware/system.c`: `cpu_boost(true)` increments a counter and switches to 80 MHz on 0 → 1; `cpu_boost(false)` decrements and switches to 30 MHz (or 24 MHz in "idle mode") on reaching 0.

**Read back on hardware** [external] siggi `08`: under Rockbox at 80 MHz, `PLL_CONTROL` = `0x8A12_1403` and `PLL_STATUS` = `0x8000_0034` (bit 31 = locked; the low bits' meaning is unknown). A `subs`/`bne` loop of 10,000 iterations took 501 µs at 80 MHz, as ARM7TDMI cycle counts predict (~4 cycles per iteration).

**Measured by us** [silicon]:

- Host-port throughput scales with the PP clock: a full 153,600-byte frame took 26,957 µs at 30 MHz and 9,629 µs at 80 MHz by CPU copy (ratio 2.8 against a clock ratio of 2.67), about 10.5 PP cycles per 16-bit transfer at either clock (S8; `display.md`).
- At 30 MHz, a feeding thread at ordinary priority ran up to 2.1 s late while the codec held higher priority (S31); thread priority mattered more than the boost (S34 matrix: unboosted with the right priority kept 16 ms drift). Frequency is not a substitute for scheduling. ProkBox now fixes the bands (codec 13, video feed 14, buffering 15, UI 16) with time-bounded aging instead of following the codec's boosts (Fit-1 B, dev43) [ours]; its first device run is the check.

**Undocumented counter** [external] siggi `08`/`09`: `0x6000_603C`'s neighbour `0x6000_6038` reads as a free-running 16-bit counter that changes on every read and decodes only 8 address bits; Apple's firmware reads it twice and XORs the results (entropy or a timestamp). Not in Rockbox's header.

## 5. Other clock-related registers

| Address | Name | Known use | Evidence |
|---|---|---|---|
| `0x6000_602C` | `MLCD_SCLK_DIV` | mono LCD bridge serial divider (mini 2G only) | [rockbox] |
| `0x6000_6044` | `CACHE_PRIORITY` | see `cores-and-threads.md`; Rockbox's debug screen also shows it as "DEV_EN3 (to be verified)" | [rockbox] |
| `0x6000_6080` | `IISDIV` | I2S clock divider (used only when the PP is I2S master; not on the iPod) | [rockbox] |
| `0x6000_6094` | `ADC_CLOCK_SRC` | name only | [rockbox] |
| `0x6000_60A0` | `CLCD_CLOCK_SRC` | name only | [rockbox] |
| `0x6000_60A4` | – | I2C clock/config (`gpio-i2c.md`) | [rockbox] |
| `0x6000_60C4` | – | IDE clock: `0xC000_0000` = "80 MHz" (`storage.md`) | [rockbox] `ata-pp5020.c` |
| `0x7000_0034` | `DEV_TIMING1` | §4 | [rockbox] |

## 6. The PMU (PCF50605 family, I2C `0x08`)

The iPod Video's power-management unit holds the regulators, the real-time clock, the ADC used for battery voltage, and the power-on/standby logic. Rockbox drives it as a Philips PCF50605 (`CONFIG_RTC RTC_PCF50605`). [rockbox] `firmware/drivers/pcf50605.c`, `firmware/export/pcf5060x.h`

### Registers used

| Reg | Name | Use |
|---|---|---|
| `0x02` / `0x05` | `INT1` / `INT1M` | bit 7: RTC alarm flag / its mask |
| `0x08` | `OOCC1` | power state: bit 0 `GOSTDBY`, 1 `TOTRST`, 2 `CLK32ON`, 3 `WDTRST`, 4 `RTCWAK`, 5 `CHGWAK`, 6 `EXTONWAK` |
| `0x0A`–`0x10` | RTC | BCD: sec, min, hour, weekday, day, month, year (from 2000) |
| `0x11`–`0x17` | alarm | same layout; Rockbox marks "no alarm" by writing `0x99` to `0x17` |
| `0x1B` | `DCDC1` | core supply |
| `0x1C` | `DCDC2` | |
| `0x21` | `DCUDC1` | |
| `0x23` | `IOREGC` | I/O supply |
| `0x24` | `D1REGC1` | codec supply |
| `0x25` | `D2REGC1` | accessory (dock) supply |
| `0x26` | `D3REGC1` | LCD supply |
| `0x27` | `LPREGC1` | |
| `0x2E`–`0x31` | `ADCC1`, `ADCC2`, `ADCS1`, `ADCS2` | ADC control and result |

### Regulator settings Rockbox applies on the Video [rockbox] `pcf50605_init`

| Reg | Value | Voltage | Note |
|---|---|---|---|
| `IOREGC` | `0xF5` | 3.0 V | Apple's default `0xF8` = 3.3 V |
| `DCDC1` | `0xEC` | 1.2 V | core |
| `DCDC2` | `0x0C` | off | |
| `DCUDC1` | `0xE3` | 1.8 V | purpose unknown (SDRAM? [hypothesis]) |
| `D1REGC1` | `0xF0` | 2.5 V | codec; Apple `0xF5` = 3.0 V |
| `D3REGC1` | `0xF1` | 2.6 V | LCD; Apple `0xF5` = 3.0 V |
| `D2REGC1` | `0xF8` on / `0x18` off | 3.3 V | accessory supply |
| `LPREGC1` | untouched | – | `0x1F` (off) on the Video |

Bits `[7:5]` are the mode (`111` on in all states, `000` off) and bits `[4:0]` the voltage code [rockbox] [apple]. LDOs (`0x23`–`0x27`): V = 0.9 V + 0.1 V × code [rockbox, measured on PCF50606 players]. The core converter's formula is not settled: 0.6 V + 0.05 V × code is favoured over 0.9 V + 0.025 V × code (`mac-and-dvs.md` §2; open question #20). Apple never changes `DCDC1` on a PP5022.

### ADC [rockbox] `adc-ipod-pcf.c`

Write `(channel << 1) | 1` to register `0x2F`, read two bytes from `0x30`; result = `(ADCS1 << 2) | (ADCS2 & 3)` (10-bit). (Rockbox's comment calls `0x2F` "ADCC1"; the header names `0x2F` `ADCC2`. The value written is what matters.)

| Channel | Signal |
|---|---|
| 2 | battery voltage (ADCIN1 through a resistive divider) |
| 4 | accessory identification |
| 7 | LTC4066 charge-current status (Video and nano) |

**Battery millivolts = raw × 6000 / 1024.** [rockbox] `powermgmt-ipod-pcf.c` (`BATTERY_SCALE_FACTOR 6000`)

### Charging [rockbox] `power-ipod.c`

The charger is a Linear Technology **LTC4066** USB power manager [rockbox].

| Pin | PP signal | Meaning |
|---|---|---|
| HPWR | GPIOA bit 2 | 1 = 500 mA USB limit, 0 = 100 mA |
| SUSP | GPIOL bit 2 | 1 = take no current from USB |
| CLDIS | GPO32 bit 28 | wall-adapter current limit disable (up to 1.5 A); untouched |
| CHRG status | GPIOB bit 0 | 0 = charging |
| – | GPIOL bit 3 | 0 = external (dock/FireWire) power |
| – | GPIOL bit 4 | 1 = USB VBUS |

Charging is fully automatic in the LTC4066; firmware only chooses the current limit and reads status. Charge current itself (and the battery's temperature) can be read only coarsely through ADC channel 7.

### Battery voltages Rockbox uses [rockbox] `powermgmt-ipod-pcf.c`

| Item | mV |
|---|---|
| shutoff | 3300 |
| "disk safe" (no writes below) | 3500 |
| boot floor (shell build: below it at rest on battery, the firmware shows the low-battery screen and powers off; a cell its own low-battery shutdown marked drained is refused at any voltage until power is seen) [ours] | 3400 |
| empty cell at rest: cut-off under playback 3,362 (filtered), next boots 3,576 / 3,569, about +210 [silicon] (owner rundown 2026-10-22) | – |
| discharge curve 0 %…100 % (stock 400 mAh cell) | 3600, 3720, 3750, 3780, 3810, 3840, 3880, 3950, 4020, 4100, 4180 |

Our unit's 3000 mAh cell uses the same table until a measured rundown replaces it [ours].

### Power off [rockbox] `power-ipod.c`, `pcf50605.c`

1. (Rockbox) clear the upper 80 KB of IRAM (`0x4000_C000`, `0x14000` bytes): Apple's firmware keeps a boot-failure flag there and would otherwise refuse to boot cleanly next time [rockbox] comment.
2. Write `OOCC1 = GOSTDBY | CHGWAK | EXTONWAK` (plus `RTCWAK` if an alarm is set). **Never enter standby without `CHGWAK` or `EXTONWAK`**, or nothing can wake the unit (iPodLinux warning, quoted in Rockbox).

The wake sources are then: the hold/buttons through `EXTONWAK` [hypothesis: the ON line is wired to the wheel controller], a charger, or the RTC alarm. Apple's bootloader clears the PMU's interrupt registers, so firmware cannot read why it woke; Rockbox infers an alarm wake by comparing the alarm and the clock within 10 s. [rockbox] `rtc_pcf50605.c`

### The RTC alarm as ProkBox uses it (P12, 2026-10-23; nothing here has run on silicon yet)

- **Register image** `0x11..0x17`: BCD sec, min, hour, weekday, mday, month, year (from 2000); each field has an "ignore" value (`0x7f, 0x7f, 0x3f, 0x07, 0x3f, 0x1f, 0xff`), which upstream writes to the date fields for a daily h:m:s match [rockbox-derived] `rtc_pcf50605.c` `alarm_disable[]`. The weekday ignore value `0x07` is not a valid weekday, so the part either skips ignore-valued fields or never compares the weekday [hypothesis].
- **Year `0x99` as "off"** is upstream's convention; that the part compares the year at all, so `0x99` never matches, is [hypothesis] (D-A3). ProkBox's "no alarm" image is 2099-01-01 00:00:00 with year `0x99` and RTCWAK cleared, so even a part that ignored the year would not wake the unit.
- **Exact-date (one-shot) match** is what ProkBox arms (`alarm_pmu_regs()`, the brief's recommendation): [hypothesis] until D-A3. If it fails, `ALARM_PMU_DAILY` in `apps/ipodui/alarm.c` switches to the daily match.
- **INT1 bit 7** latches at a match and clears on read [rockbox-derived]; upstream's power thread disarmed the alarm on it (year `0x99`, RTCWAK off) every step. Under the shell it only reads it (`powermgmt.c`, H2).
- **RTCWAK lives in RAM until standby** (`pcf50605_wakeup_flags`, written to OOCC1 by `pcf50605_standby_mode()`), and starts at 0 at every boot [rockbox-derived] `pcf50605.c`. So a power-off before the shell re-arms would drop the wake. `rtc_init()` and the boot guard call `rtc_keep_alarm_wake()` (`firmware/export/rtc_alarm_keep.h`), which keeps it for a valid alarm still ahead [ours].
- **Alarm power-on to boot** (the boot ROM, then our bootloader, then ProkBox) is [rockbox-derived] and never run on this unit (D-A1). Upstream's 10 s "alarm boot" window is replaced by 120 s from the persisted armed time (`alarm_is_boot()`), since a disk spin-up can exceed 10 s [ours].

**Reboot:** `DEV_RS |= DEV_SYSTEM` (`0x6000_6004` bit 2) and spin. [rockbox] `system_reboot()`

### Drive power

GPO32 bit 30, active low; on: clear it, wait ≥ 4 ms, `DEV_EN |= DEV_IDE0`, give the ATA pins to the controller (`GPIOG/H_ENABLE = 0`, clear `GPIOI_ENABLE` bits `0xBF` and `GPIOK_ENABLE` bits `0x1F`), wait 10 µs. Off: the reverse, pins back to GPIO (`0xFF`, `0xBF`, `0x1F`), then set bit 30. [rockbox] `ide_power_enable()`. Rockbox's flash-storage build never cuts drive power.

## 7. Measured power [silicon]

There is no current sensing on the board; only battery voltage is observable. From our power logs (3000 mAh cell; voltage averaged by the firmware; not a calibrated rundown):

| Condition | Duration | Voltage | Source |
|---|---|---|---|
| full, rested | – | 4,204–4,208 mV | battery-day log 2026-10-12 |
| playback, CPU boosted 4 %, backlight 7 % | 1.94 h | 4,087 → 4,005 mV (≈ 42 mV/h) | same |
| playback, boosted 34 %, backlight 17 % | 1.62 h | 4,192 → 4,136 mV | same |
| charging over USB (500 mA limit), boosted 98 % | 0.69 h | 3,987 → 4,218 mV | same |
| idle, nothing on | 8.26 h | 4,134 → 4,129 mV | same (the next boot read 3,923 mV; the last logged row may be stale) |

WM8758 wake from our DAC-standby state (PLL, DACs, outputs back on): median 6.4 ms, 95th percentile 9.5 ms, worst 12.0 ms over 657 wakes [silicon] (device logs, 2026-10-12/13). No full discharge has been logged.

## 8. Running at 24 MHz with the PLL off (Fit-1 C6)

Gathered for `docs/fit1-brief.md` section C; the policy that uses it is [ours] (`firmware/system.c` `cpu_pll_hold()`, `firmware/pll_idle.c`, `apps/ipodui/clockpol.c`).

- **The switch is upstream's `CPUFREQ_DEFAULT` case** (§4), unchanged: `PLL_CONTROL |= 0x0800_0000`; stall both cores; `CLOCK_SOURCE = 0x2000_2222`; `DEV_TIMING1 = 0x0303`; release; then PLL enable bit off and `DEV_INIT2.INIT_PLL` off. Upstream ran it at boot, in the bootloader and for `cpu_idle_mode()` users (radio, the pre-ARC USB path) [rockbox].
- **Every PLL setting already passes through 24 MHz** [rockbox] `set_cpu_frequency()`. Before it retunes the PLL, it powers the PLL, switches the core to the crystal, writes the new `PLL_CONTROL`, and polls `PLL_STATUS` bit 31 while running at 24 MHz. Only then does it select the PLL. So a boost from 30 MHz already waits for a relock at 24 MHz. A boost from 24 MHz-idle adds only the PLL's power-up to that wait. Both the boost and the 30 MHz request return only once the PLL is locked.
- **Lock time:** not documented anywhere we have. The PP5020 path waits a fixed 500 µs [rockbox]. On the PP5022 the firmware now times each wait that follows a power-up with `USEC_TIMER` and logs it (`clk lock n= us= max=`) [ours]; the value is pending a device session. A value near 0 µs would mean the lock bit reads stale right after power-up [hypothesis to test].
- **Timers do not depend on the core clock.** Timer 1 (the tick), Timer 2 and `USEC_TIMER` count at a fixed 1 MHz (`TIMER_FREQ`, `kernel-pp.c` writes µs) [rockbox] (`interrupts-timers.md`). Nothing needs rescaling at 24 MHz.
- **Audio does not depend on the PLL.** The WM8758 is the I2S clock master (`CLKCTRL_MS`). Its MCLK is the PP's external device clock, set to 24 MHz from the crystal (`0x7000_0018` bits 2–3 cleared, `wmcodec-pp.c`) [rockbox] (`audio.md`). The PP's I2S block is a slave (`IISDIV` unused).
- **ATA:** upstream rates UDMA2 as stable at 30 MHz and UDMA1 at 24 MHz; its bootloader runs at 24 MHz with UDMA1 (`ata-target.h`) [rockbox]. The UDMA and PIO timing registers are not rewritten when the clock changes. So we hold the PLL for each DMA transfer [ours], and PIO (HDD writes, unaligned reads) runs at 24 MHz as in the bootloader. Whether the IDE clock (`0x6000_60C4 = 0xC000_0000`) comes from the PLL is unknown [hypothesis: no, since UDMA1 works in the bootloader with the PLL off].
- **USB:** the ARC controller's clocking is undocumented. The reset waits on `0x7000_0028` bit 7, probably a PHY PLL [hypothesis]. We hold the PLL whenever `usb_enable(true)` is in force [ours].
- **Clickwheel serial and I2C (`0x6000_60A4`):** never reconfigured across upstream's 30↔80 MHz switches, and both work in the bootloader at 24 MHz [rockbox], so they are either clocked independently or tolerate the change [hypothesis].
- **BCM host port:** about 10.5 PP cycles per 16-bit transfer (§4 [silicon]), so a full frame takes about 33.7 ms at 24 MHz [estimate from that rate]. A lit screen holds the PLL [ours].
- **PP5021C (5G):** Rockbox builds the 5G with `CONFIG_CPU == PP5022` and runs the same PLL sequence on it [rockbox]. It has not been separately verified on 5G silicon.
- **Energy:** the core voltage is fixed (DCDC1 1.2 V), so energy per cycle is about constant. What 24 MHz saves is the PLL's own power, plus 20 % fewer cycles on the clock tree, the bus and the SDRAM controller wherever they run while the cores sleep [hypothesis]. The size of the saving is unmeasured: there is no current sense, so it waits for the M0 kit's `power.csv` A/B.

## 9. Soft sleep: what Phase 18-1 assumes (2026-10-22; nothing here has run on silicon yet)

`power_sleep()` (`firmware/target/arm/pp/power_sleep-pp502x.c`, [ours]) keeps SDRAM and stops the kernel: the tick (Timer 1) stopped, every interrupt source masked except the wake set, the CPU in `PROC_SLEEP` with IRQ and FIQ off at the core, the COP in its scheduler's idle sleep. Each assumption, with the device step that settles it (`docs/phase18-brief.md` §4.0 D2, the developer Sleep lab):

| Assumption | Evidence | Settled by |
|---|---|---|
| A timer interrupt wakes a core from `PROC_SLEEP` at 24 MHz with the PLL off | [silicon, indirect]: the unboosted dark idle (§8) sleeps in `core_sleep()` and is woken by Timer 1 a hundred times a second every day. Timer 2 is the same block [rockbox-derived] | D2 "Poll timer test": each poll's RTC seconds and `USEC_TIMER` microseconds logged |
| An enabled source wakes `PROC_SLEEP` while CPSR's I and F bits are set, and the handler does not run | [rockbox-derived]: `core_sleep()` is entered with IRQs off and enables them after the wake | every D2 wake row |
| Timer 2 one-shot: `TIMER2_CFG = 0x8000_0000 \| (µs − 1)` (enable without the repeat bit 30) fires once | [rockbox-derived] for the field layout (`timer-pp.c` always sets bit 30); the one-shot reading is [hypothesis] | D2 poll test (a second poll would show as a wake with no RTC progress) |
| `PROC_WAIT_CNT \| PROC_CNT_SEC \| n` (`0x4080_00nn`) sleeps the core n seconds on a PP5022 | [hypothesis]: the unit is named in upstream's header, never used; whether an interrupt also ends it is unknown | D2 "Countdown poll test" (5 s polls; MENU+SELECT is the recovery if it never ends) |
| `USEC_TIMER` keeps counting while the core sleeps | [hypothesis] (the raw I2C timeouts only need it with the core awake) | the poll test's `us=` against `rtc=` |
| The PMU's RTC and ADC answer the PP's I2C block at 24 MHz with interrupts masked, by register polling with no tick | [rockbox-derived] register sequence (`i2c-pp.c`); the block works at 24 MHz in the bootloader [rockbox-derived] | every poll's `mv`, D2 rows (ix) |
| The 7-byte RTC read as one address write and block reads of 4 and 3 bytes continues the PMU's register pointer across transactions | [rockbox-derived] (`i2c_readbytes()` does exactly this) | host test (transaction shape), the wake's `s=` |
| SDRAM keeps its contents with both cores asleep and Timer 1 stopped | [hypothesis] (the controller's auto-refresh is independent of core activity) | D1 (18-0), and every wake that returns to a working UI |
| The deep tier (`sys_poweroff()` → `pcf50605_standby_mode()`) adds `RTCWAK` when an alarm is set | [rockbox-derived]: `rtc_enable_alarm(true)` sets `pcf50605_wakeup_flags \|= 0x10` | the deep test with an alarm (P12) |

How Apple's own light sleep differs (SDRAM self-refresh, clock gating, regulator sleep modes, no timer wakes): §10 and `apple-power.md` §2.

The COP park has no COP-side code: the COP's scheduler calls `core_sleep()` only with an empty run queue and its lock released (`thread.c`), so `COP_CTL` reading `PROC_SLEEP` (twice, 50 µs apart, within 100 ms) is the acknowledgement; its own interrupt enables are masked for the sleep and restored, and the CPU wakes it with `core_wake(COP)` after its own restore [ours].

## 10. What Apple's sleep code adds (2026-11-05 desk study)

From `apple-power.md` (static reading of `osos` 1.3, cross-checked against 1.2.1 and 1.2.3). Each item is [apple-RE] for what Apple writes; meanings are tagged separately.

- **SDRAM self-refresh:** Apple's light sleep sets `XMB_RAM_CFG` (`0x7000_003C`) bit 22 and waits for bit 30 to set before it gates clocks. At wake it clears bit 22 and, after the PLL relocks, waits for bit 30 to clear. That this is a self-refresh request and acknowledge is [hypothesis] (open question #39).
- **Self-timed core stalls:** Apple writes count-mode words to the running core's own `CPU_CTL`: `0x4800_00nn` (n clock cycles), `0x4200_00nn` (n µs; `0x4200_00FA` = 250 µs for a PLL relock) and `0x4100_00nn` (n ms; `0x4100_000A` = 10 ms around the self-refresh). The unit bits match Rockbox's `PROC_CNT_CLKS`, `PROC_CNT_USEC` and `PROC_CNT_MSEC`, which Rockbox never uses (#46).
- **Sleep clock state:** `DEV_EN` = `0xC001_0108` (the clickwheel, USB0 and three unnamed bits kept), `DEV_EN2` = 0, `0x6000_6044` = `0x3F`, `DEV_INIT2` bit 6 set, `CLOCK_SOURCE` with no active selector, `PLL_CONTROL` = `0x0200_0000`, then `PROC_SLEEP`. With a USB cable in, Apple spins on `0x7000_0028` bit 2 and `CPU_INT_STAT` instead of sleeping.
- **Regulator modes:** the codec (`0x24`) and LCD (`0x26`) LDOs go to mode `010` in light sleep and `000` when off, with the voltage code unchanged, and back to `111` at wake. What `010` does is [hypothesis: on in the PMU's active state only, or a low-power mode] (#42).
- **Pad parking:** on a PP5022, before either sleep, `0x7000_0010` = `0x0004_0000`, `0x7000_0014` = `0x0A00`, `0x7000_0018` = `0x5_0200`, `0x7000_001C` = 0 and `DEV_INIT2 &= 0xE000_0000` (#43).
- **Timers stop:** `TIMER1_CFG` and `TIMER2_CFG` are saved and zeroed for the sleep, so no timer wakes it.
- **A seconds counter in the timer page:** `0x6000_5014` holds days, hours, minutes and seconds in four 6-bit fields (`[29:24]`, `[21:16]`, `[13:8]`, `[5:0]`), which Apple converts to seconds. Whether it runs during sleep is open (#44). **Awake it does not** [silicon, sitting 4gh]: it read 0 at both ends of 11 s.
- **Power-off:** Apple sets only `OOCC1` bit 0 (standby) or bit 1 (reset), from a shadow read at boot, and never writes the wake-enable bits. Its alarm "disabled" year is `0xAA` (Rockbox uses `0x99`).
- **The long sleep is suspend-to-disk:** an image written to the firmware partition's `hibe` slot, then standby. The resume marker is `0x4001_FF10` = 8 (`apple-power.md` §3).

## 11. The battery probes: which bench row answers what (2026-11-05; results of sitting 4gh, 2026-11-06, at the end)

`apps/ipodui/devbench_pwr.{c,h}` [ours], a picked-only row group ("Battery probes", `bench_pwr_picks`) in developer bench builds. Each row saves what it changes, puts it back, reads it back and logs `restored` or the word that did not come back; the simulator answers every row "Not run (sim)". Run order, basic to advanced:

| Row | Question | What it does | Pass |
|---|---|---|---|
| `pgrd` | #35 speed grade | reads `STRAP_OPT_A` (and `_B`); logs `[6:5]` and Apple's 133/100/80/66 MHz meaning | done (a reading) |
| `preg` | Apple's register values | reads `PP_VER1/2`, `DEV_EN`, `DEV_EN2`, `0x6000_6044`, `DEV_RS`/`RS2`, `0x7000_0010`–`0x7000_0020`, `DEV_TIMING1`, `XMB_RAM_CFG`, `CLOCK_SOURCE`, `PLL_CONTROL`/`STATUS`, `0x6000_6048`, `CPU_CTL`, `COP_CTL`, both timer configs, `0x7000_0028`, and the power catalogue's read-only checks (`apple-power-catalog.md` §3: `MLCD_SCLK_DIV` `0x6000_602C` for T-MLCD, `0x7000_0030`, `GPIOA_INPUT_VAL` for T-LOWBATT-PIN's bit 3, `XMB_RAM_CFG` for T-XMBCFG's "before"); the PMU's `0x24`, `0x25`, `0x26`, `0x1B`, `0x1C`, `0x1E`, `0x21` | done |
| `prtc` | #44 (awake half) | `0x6000_5014`'s change edges ~10 s apart against `USEC_TIMER` and the PMU's RTC | counts, 1 Hz within 2 %, the PMU within 1 s |
| `pstl` | #46 | `CPU_CTL` = Apple's count words, IRQ/FIQ off, music paused, 3 reps each: `0x4800_000F`, `0x4200_000A`, `0x4200_0064`, `0x4200_00FA`, `0x4100_0001`, `0x4100_000A`, each only after the ones before held; `CPU_CTL` then cleared as `core_sleep()` does | 90–110 % of the stated time (+8 µs); the clock-cycle form under 8 µs |
| `pldo` | #42 | screen dark (backlight off, `lcd_sleep()`), `0x26` to mode `010` at the same voltage code for 5 s, restored, lit: "Does the screen look normal?"; only then, music paused and the DAC in standby (headphones noted), `0x24` the same: "Do you hear the music normally after resume?" | both Yes (the screen part alone: done) |
| `pden` | #41 (awake half) | each **named-unused** `DEV_EN` bit that is on (below) cleared alone, ≥ 2 s, a SELECT press asked for, then set again (that bit only) | the tick within 10 % of `USEC_TIMER`, SELECT seen, the music played on |
| `psrf` | #39 | USB out, music paused, disk idle, the screen engine claimed and idle, the COP parked (its enables masked, `PROC_SLEEP` seen twice); from IRAM with IRQ/FIQ off: `XMB_RAM_CFG` bit 22 set, bit 30 polled (100 ms bound), 1 s `USEC_TIMER` hold, bit 22 cleared, bit 30 polled clear (100 ms); CRC-32 of the first 1 MB of the firmware's code through the uncached alias before and after | acknowledged and cleared within the bounds, the word back as found, the CRC equal |

`pden` and `psrf` need `preg` (the starting words are in the log before any write). `pldo` gates its codec part on its screen part.

**`DEV_EN` users on the iPod Video** (grep of `firmware/`, `apps/`, `bootloader/`, 2026-11-05) [rockbox-derived]: start-up sets `0xC200_0124` (bits 31, 30, 25 `IDE0`, 8, 5, 2 `SYSTEM`); `EXTCLOCKS` (1) and `I2S` (11) by the codec driver; `I2C` (12) by `i2c-pp.c`; `OPTO` (16) by the clickwheel driver (off with Hold); `PWM` (17) by the piezo (the clicker); `IDE0` (25) by `ide_power_enable()`; `USB0` (3) and `USB1` (22) together by the USB driver; `SER0`/`SER1` (6, 7) by `uart-pp.c` when the dock's serial port is set up. **No code on the Video sets** `FIREWIRE` (23), `LCD` (26) or `ATA` (14, the Sansas' SD controller). So `pden`'s candidates are those three, `USB1` with the USB controller off, and the UARTs with no accessory; with Rockbox's start-up word none is on, and the row then only logs that awake gating has nothing to cut.

**Sitting 4gh results** (2026-11-06, build `56cd4e2096`, the owner's 5.5G) [silicon]:

| Row | Reading | Answer |
|---|---|---|
| `pgrd` | `STRAP_OPT_A` = `0x003F_008C`, `[6:5]` = 0; `STRAP_OPT_B` = `0x0000_EAFF` | grade 0: 133 MHz in Apple's table (the meaning [hypothesis]); #35 read |
| `preg` | at 80 MHz, music stopped, USB out: `PP_VER2`/`PP_VER1` "PP5022C "; `DEV_EN` `0xC203_1926`, `DEV_EN2` 0, `0x6000_6044` `0x3F`, `DEV_RS`/`RS2` 0, `DEV_INIT1` `0x0004_0000`, `0x7000_0014` `0x0A00`, `0x7000_0018` `0x5_0200`, `0x7000_001C` 0, `DEV_INIT2` `0x4000_0000`, `DEV_TIMING1` `0x0808`, `XMB_RAM_CFG` `0xA006_16FA`, `CLOCK_SOURCE` `0x2000_7777`, `PLL_CONTROL` `0x8A12_1403`, `PLL_STATUS` `0x8000_0034`, `CPU_CTL` 0, `COP_CTL` `0x8000_0000` (asleep), `TIMER1_CFG` `0xC000_270F` (100 Hz), `TIMER2_CFG` 0, `0x7000_0028` `0x0200_408A`, `MLCD_SCLK_DIV` 0, `0x7000_0030` `0x9801_6460` (bits 15:12 = 6), `GPIOA_INPUT_VAL` `0xA8` (bit 3 high, battery healthy); PMU `0x24` `0xF0`, `0x25` `0xF8`, `0x26` `0xF1`, `0x1B` `0xEC`, `0x1C` `0x0C`, `0x1E` `0x30`, `0x21` `0xE3` | Against Apple: the pad words and `DEV_INIT2` already equal Apple's sleep values, and `0x6000_6044` equals Apple's sleep `0x3F` (Rockbox's `CACHE_PRIORITY`), so none of those writes would change anything here (#43). `DEV_EN` differs from Apple's sleep set `0xC001_0108` only by blocks in use awake (on here: `EXTCLOCKS`, `SYSTEM`, bit 5, `I2S`, `I2C`, `PWM`, `IDE0`; `USB0` is in Apple's set and off here with no cable): no unused block on. The three LDO bytes are mode `111` with their codes, as Apple runs them awake |
| `prtc` | `0x6000_5014` `0` → `0` over 11 s, 0 edges, the PMU 11 s | **does not count** awake (S-6: no) |
| `pstl` | `0x4800_000F` 0/0/1 µs; `0x4200_000A` 12/0/0 µs, `CPU_CTL` read back `0x4200_0000`, no interrupt pending | **the count forms don't hold** (S-8: no): the µs form stalled once in three |
| `pldo` | `0x26` `0xF1` → `0x51` read `0x51`, 5 s, restored | the screen came back normal (the "Blank" answer was the owner's slip, per their note); the codec part did not run: **rerun** |
| `pden` | `DEV_EN` `0xC203_1926`, 0 of 6 candidates on | **nothing to cut** awake (quirk 71 confirmed) |
| `psrf` | not run: the disk or the music started again (the row needs music stopped) | **rerun** with music stopped |

**Not built, and why:** S-3's sleep half (`PROC_SLEEP` with Apple's `DEV_EN` = `0xC001_0108`) is left out: that set drops `SYSTEM` (2) and `I2C` (12), and whether the timer block and the wheel's interrupt path still run without them is unknown, so no wake path (Timer 2 or a timer-interrupt watchdog) is provably safe; sleep labs also wait for the locked awake topology (owner). S-7 (USB PHY suspend) needs a host that has suspended the bus. S-2, S-5 and S-9 need a soft sleep, a meter-free A/B over hours, or a stock unit's backup.

## 12. The awake battery rows from Apple 1.3 (P19 `p19-pwr-awake`, 2026-11-05; results of sitting 4gh, 2026-11-06, at the end)

What the firmware now does while awake, row by row (`apple-power-catalog.md` §3; the gate in `firmware/export/pwr_awake.h`). Each switch row is on by its bench token (`pwr_awake_tokens.h`) or, in developer builds, by the developer mask; any failure signal turns it off for the session, and every act is counted for `ipodui.log` (`pwraw <row> acts= undos= fails= a= b=`).

| Row | Registers and sequence | Default now | Failure signal |
|---|---|---|---|
| AW-1 drive release linger | none of its own: the drive's existing cut (`ide_power_enable(false)`, Apple's order, §"Drive power"), 250 ms after a fill that reached its target instead of 3 s after the last access; flash, dark | developer: on; release: compiled out until `pwraw-linger` | two power-ons in a row within 5 s of a linger's cut |
| AW-2 USB parked on a charger | `usb_enable(false)` (USB core exit, `DEV_USB0/1` off, `DEV_INIT2 &= ~INIT_USB`, the PLL hold released) 12 s after VBUS with no bus reset, `SET_ADDRESS` or transfer; the charge current (LTC4066 `HPWR`/`SUSP`, `power-ipod.c`) written back to what the stack chose; `usb_enable(true)` on a re-probe | developer: on; release: compiled out until `pwraw-usboff` | a host first heard in a re-probe's window (VBUS never dropped): the window doubles, two in a row give it up |
| AW-3 accessory supply | `D2REGC1` (0x25) `0xF8` on / `0x18` off, unchanged code; only the setting's default moves (`ACCESSORY_SUPPLY_DEFAULT`) | on (this unit: the USB-C board may draw from it) | — (bench `awd2`/`awd2c`) |
| AW-4 LCD LDO | `D3REGC1` (0x26): mode bits 7:5 `111` → `010`, voltage code kept, written after `bcm_powerdown()` in `lcd_sleep()`; the found value written back and ≥ 10 ms (`sleep(2)`) before `bcm_init()` in `lcd_awake()`, both under the port lock; applied only when found in mode `111` | off until `pwraw-lcdldo` (or the probes' `pldo`) | a read-back that doesn't match, either way (the found value written back) |
| AW-5 codec clock and I2S | in DAC standby, after the PLL-off write: `DEV_EN &= ~(DEV_EXTCLOCKS \| DEV_I2S)` (variant b; a: `DEV_EXTCLOCKS` only), IRQs masked for the read-modify-write; before the standby wake's first write: the bits set again, 10 µs, then `IISCONFIG` and `IISFIFO_CFG` compared with the values saved at the gate | off until `pwraw-i2sgate` | either I2S register changed (both written back) |
| AW-8 `XMB_RAM_CFG` | read before and after `usb_reset_controller()`'s `\|= 0x47A`; nothing else changes | developer builds log it | — |
| AW-9 PMU ADC | after the ADCC1 start (0x2F), read ADCS1/ADCS2 (0x30/0x31) until ADCS2 bit 7 is set, at most 4 reads, the last kept as before | every build | the bit never seen (counted, developer builds) |

[hypothesis] for every effect: the bench rows `awxm`, `awd2`, `awd2c`, `awlg`, `awus`, `awld`, `awi2` (`apps/ipodui/devbench_awake.h`) are the silicon tests. Open risk recorded for AW-4: the BCM job driver may drive the BCM while dark without `lcd_awake()` (`pgov.c`'s note); if `D3REGC1` feeds anything the BCM needs, a dark job would run on the lowered rail. `awld` checks the panel; a dark-job check belongs with the BCM session's bench.

**Sitting 4gh results** (2026-11-06, build `56cd4e2096`) [silicon]:

- **AW-2 (`awus`): PASS**, token `pwraw-usboff` earned and set: parked on the charger after the host window (`acts=1`, the charge current kept at 500 mA), the owner's "Yes" to the host question. An `artcache` hang record (10 s with no progress, recovered after 27 s) fell inside this row's wait; it is logged, not the row's verdict, and belongs to the art cache's owner.
- **AW-3 (`awd2`, `awd2c`): PASS**, both: `D2REGC1` `0xF8` → `0x18` (read back `0x18`) for 5 s with audio playing through the USB-C board (headphones in, no USB, no charger), the owner heard it continue, and `0xF8` restored. **So the USB-C board's audio multiplexer is not fed from this rail on this unit.** Enumeration and charging with the rail off were not part of the row. The default stays **on** until the owner decides (`HAVE_ACCESSORY_RAIL_LOAD`, `docs/hardware.md`); the finding makes "off for the USB-C profile" a candidate.
- **AW-4 (`awld`): PASS**, token `pwraw-lcdldo` earned and set: 5 cycles, lowered 5, restored 5, lit 5 of 5, worst wake 447,625 µs (cycles 2–5: 420–448 ms), `D3REGC1` back at `0xF1` each time.
- **AW-5 (`awi2`): FAIL**: 6 pauses gated, 4 resumes running, **2 late**; variant b cycles 2 and 3 found `pcm=0` with `DEV_EN` `0xC203_1124` (`EXTCLOCKS` and `I2S` still off when checked). The I2S words came back intact (`IISCONFIG` `0x2000_0070`). Token stays off.
- **AW-1 (`awlg`)**: did not really run: nothing was playing (`playing=0`, `acts=0`, no fill in 893 s). Token stays off; rerun with music.
- **AW-8 (`awxm`)**: `XMB_RAM_CFG` `0xA006_16FA` before and after the last USB reset's OR (unchanged). **AW-9**: 0 done-bit misses.

## 13. Clock policy today (2026-11-06)

What each build does with the clock; the mechanism is §4 and §8, the history and plans are linked.

**Release build** (`--ipodui-release`) [code]:
- **24 MHz, PLL off** whenever nothing holds the PLL (`firmware/system.c` `unboosted_freq()`); this is where dark playback of the light formats runs [silicon] (`../design/p19-placement.md` §4.1).
- **30 MHz on the PLL** while any holder is set (`CPU_PLL_HOLD_*`, `firmware/export/system.h`): the lit screen, a format that does not fit at 24 MHz or one struck this session (`clockpol`), each ATA DMA transfer, USB enabled (`../design/lit-24mhz.md` §1.3).
- **80 MHz** while anything holds `cpu_boost()`.
- **The input boost:** a lit wheel step or press boosts for about a second after the last input, only while the codec shares the UI's core and something plays (`apps/ipodui/clockpol_core.h` `cp_input_boost()`).
- The core voltage never moves (`DCDC1 = 0xEC`).

**Developer builds add** [code]: the band governor `pgov` (`HAVE_PP_GOV`, off until its marker is set): bands from measured per-core idle time, holders as floors, `cpu_boost()` wrapped; and its stage 2 (`HAVE_PP_GOV_DVS`): 100 MHz and one core-voltage step down behind two more markers, with a watchdog (`../design/pp-governor.md` §4, §16).

**Next** [ours]: the layout A plan (a band per track, a busy-time step-down, per-core request words, deadline bursts, pins for disk mode and video) is `../design/pp-governor.md` §17. Apple 1.3's governor, for comparison, is `apple-power-catalog.md` §2.1.
