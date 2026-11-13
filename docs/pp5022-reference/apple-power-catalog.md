# Apple 1.3's PP-side battery behaviours: a catalogue for ProkBox

Desk study, 2026-11-05 (branch `p19-pwr-catalog`). It answers one question for the owner: which of Apple firmware 1.3's PP5022 power behaviours can we put into our build, and how? One row per behaviour, ranked by the battery gain we expect per unit of effort. **Awake now** rows ship as automatic A/B with a fallback. **Sleep, later** rows wait for the soft-sleep feature, which follows layout A's acceptance bench; only their hardware probes go on the bench now.

This chapter builds on [`apple-power.md`](apple-power.md) (sleep, deep sleep, the awake idle; its §7 adopt list A1–A11 and §8 silicon questions S-1–S-9) and [`apple-dual-core.md`](apple-dual-core.md). It settles most of `apple-power.md` §4.2's open items.

**Method.** The same as `apple-power.md`. We read the osos 1.3 disassembly statically: `arm-none-eabi-objdump` at base `0x1000_0000`, small literal-pool and call-graph scripts kept outside the repository, and the start-up copy tables for the IRAM code. We cross-checked against firmware 1.2.1 and 1.2.3. Only addresses, constants and the order of accesses were recorded; every description is in our own words. Nothing touched a device.

**Tags.**
- [apple-RE]: read from Apple's code in this study.
- [hypothesis]: our reading of what it means, or what it is for.
- [unknown]: not found.
- [code]: ProkBox's tree on `p19-pwr-catalog`, cited by file and line.
- [silicon]: measured on our unit (session named).
- [estimate]: computed from stated figures.

Gains are dark-playback or paused-dark current on our flash 5.5G, against the ~35–40 mA dark FLAC figure in `../design/p19-placement.md` §0. Every gain is an [estimate], since we have no meter (`no-meter-tests` rule: voltage slope and logged duty only).

---

## 1. What Apple 1.3 does, in brief

1. **A load-following clock governor, not fixed points** [apple-RE].
   - A dedicated task (loop at `osos 0x1028_5194`, body `0x100C_164C`) wakes every 10 ms. Each pass it reads both cores' idle time, works out the kHz the load needs, adds a decaying "event boost", and clamps the result to **26–80 MHz**.
   - It applies the result through the PP5022 clock driver's set-frequency method (IRAM, vtable `0x1066_E1EC` slot 1).
   - Explicit requests (`0x100C_15CC`: a deadline and a level) hold the clock at the 80 MHz ceiling for **1 s, 3 s, 5 s, 10 s or 30 s** after certain events.
   - **USB disk mode pins 80 MHz** and suspends the governor (`0x1016_39CC`/`0x1016_3A30` through `0x101A_8710`).
   - The parameter block is identical in 1.2.1, 1.2.3 and 1.3. §2.1 has the detail.
2. **The clock floor while awake is 26 MHz on the PLL.** Apple never parks the awake core on the 24 MHz crystal with the PLL off; only light sleep does that. ProkBox's 24 MHz with the PLL off, lit or dark, is **below Apple's floor** [apple-RE].
3. **The LCD bridge's serial clock divider follows the core clock** (`MLCD_SCLK_DIV`, `0x6000_602C`, bits 0 and 4, set at ≤ 30 MHz) [apple-RE].
4. **The disk is held per client, not by one global timeout.** Each of 45 channels releases the disk with its own linger time: **250 ms after a song-buffer read**, 1 s for most UI reads, 0 ms after a piggy-backed preference write. The drive powers down when the last linger expires. A **predictive spin-up on a rapid user skip** (channel 44, 2 s) and a play-time pre-spin are present in 1.2.1 already; 1.3 only names all 45 channels in its debug table [apple-RE] (`apple-disk.md`, the full study, corrects the first edition here).
5. **Dark but awake, Apple only turns the backlight off.** The panel and the LCD supply stay on until light sleep [apple-RE]. ProkBox already sleeps the LCD and the BCM 10 s after the backlight goes off, so here **we are ahead**.
6. **USB on a charger only is switched off again** when no host answers within about 100 ms. ProkBox keeps USB clocked and the PLL held for as long as a charger is plugged in [apple-RE].
7. **The codec stays fully on through pauses** and goes fully off (VMID too) only in sleep. Our 5 s DAC standby already beats Apple awake [apple-RE].
8. **Almost nothing else moves while awake:**
   - no Timer 1 slow-down;
   - no SDRAM retuning;
   - no charger writes;
   - I2S gated only around USB disk mode;
   - the PMU and every input are interrupt-driven, with no polls [apple-RE].
9. **Between versions, the PP-side change is a disk linger.** 1.3 holds the drive for 500 ms after a pre-spin, where 1.2.x held it for 10 s; the channels, the predictive spin-up and the arbiter were already in 1.2.1 (`apple-disk.md` §7). The clock governor, sleep, codec and USB-probe code was already there in 1.2.1 (§2.10).

---

## 2. Findings per area

### 2.1 Clocks

**The governor** (`osos 0x1028_5194` → `0x100C_164C`) [apple-RE unless marked]:
- **Start-up.** At boot a 92-byte parameter block is copied from the image (`osos 0x1000_1704`) to `0x1087_4758` (`osos 0x1000_0F84`). The task then reads a stored copy (store object 23, 92 bytes), which wins if its version word is not newer. The first clock set, at start-up (`osos 0x1027_6FE8`), uses the block's boot field: **80 MHz**.
- **The block** (offsets from `0x1087_4758`; the same values in 1.2.1, 1.2.3 and 1.3):

| Offset | Value | Use |
|---|---|---|
| `+0x08` | 24,000 | crystal frequency (the reference for the bridge-divider rule) |
| `+0x0C` | 80,000 | ceiling, and the level every timed request asks for |
| `+0x10` | 26,000 | floor of the clamp (`osos 0x100F_E240`) |
| `+0x14` | 80,000 | boot frequency |
| `+0x18`, `+0x1C` | 24,000, 3,000 | the bridge-divider steps (`osos 0x1000_0884`) |
| `+0x20` | 10 | the task's period in ms (a timed semaphore wait, `osos 0x100A_697C`) |
| `+0x30`, `+0x34` | 1,000, 1,000 | slew limits on the filtered need, kHz per pass [hypothesis] |
| `+0x3C` | 500 | event-boost step in kHz. Three modules raise it to 1,700 or 3,500 while active and restore it afterwards (`osos 0x1012_7E2C`, `0x1018_B4C0`, `0x101A_9824`; restores `0x101A_91E4`, IRAM-overlay code at file `0x71_D548`, `0x72_E854`). These are most likely codec engines [hypothesis] |
| `+0x40`, `+0x44` | 75, 40 | per-pass boost gain and decay, in %: the boost grows to 75/100 of itself plus the step on a busy pass, and keeps 77/128 of itself on an idle pass |
| `+0x54`, `+0x58` | 7, 4 | filter shift and gain [hypothesis] |

- **Each pass** (`osos 0x100C_164C`):
  1. read the 64-bit idle totals of both cores (`0x1498_3040`) against the last pass;
  2. look up the needed kHz in a 99-point table (`osos 0x101C_EC3C`/`0x101C_EBAC`);
  3. low-pass filter it and add the event boost and any timed request;
  4. clamp to 26–80 MHz;
  5. apply the policy floor (`osos 0x1000_099C`, below);
  6. call the driver only if the result differs from the current clock.
- **Every pass also wakes the COP if it is asleep, while the audio engine runs** (`osos 0x100C_FB08`, corrected 2026-11-06) [apple-RE]:
  - **Order.** The pass copies both 64-bit idle totals from `0x1498_3040` first (`osos 0x100C_17DC`–`17F8`), then calls the wake (`bl` at `0x100C_17FC`).
  - **Guards.** The wake runs only on the CPU, only if the word at `0x1760` is nonzero, and only while the byte `0x1493_7139` is 0. That byte is the audio-stop hold: `osos 0x1018_A900` sets it, wakes the COP, waits for its acknowledgement in `0x1493_713A` and clears `DEV_EN` bit 11 (`DEV_I2S`); `osos 0x1018_A998` undoes it. So the pulse runs at about 100 Hz during playback (dark included) and stops with the engine (disk mode).
  - **Why.** The COP's idle routine (`osos 0x100D_ED04`) runs with IRQ and FIQ masked, notes `USEC_TIMER` on entry, and adds the elapsed time to its total (`0x1498_3048`) only on exit (`osos 0x100D_EE3C`–`EE5C`), dropping an interval in which the counter wrapped. The pulse closes the COP's idle window so the next pass sees it, and the read-then-wake order keeps the pass from reading a total the COP is half-way through writing. Nothing else depends on it: the COP enables only interrupt source 26. (This was a [hypothesis] in the first version; the code settles it.)
  - **ProkBox.** Our governor reads a sleep in progress from its start stamp under a sequence counter, so it needs no pulse (`../design/pp-governor.md` §17.6).
- **Policy floor** (`osos 0x1000_099C`) forces the ceiling (80 MHz) when any of these holds:
  - a debug byte (`0x1081_DFE2`, from a file in the device folder);
  - a GPIO input (Apple's pin 82, or 134 on board types 10–12, read active-low; `osos 0x1026_51E4`): external power or the dock [hypothesis];
  - two subsystem states (`osos 0x100A_7784`, `0x100F_FFF8`) [unknown];
  - a state object in mode 1 or 2 (`osos 0x1018_4904`) [unknown].

  A flag (`0x1081_EDB6`, `osos 0x100F_5D6C`) raises the floor to **58 MHz** [unknown which feature].
- **Timed requests** (`osos 0x100C_15CC`: deadline = now + d ms, level = the ceiling):

| Duration | Caller |
|---|---|
| 1 s | `0x1014_7D14` |
| 3 s | `0x1014_7BBC` |
| 5 s | four sites in the `0x1024_9xxx`–`0x1024_Axxx` module |
| 10 s | `0x1024_A7CC` |
| 30 s | `0x101B_918C`, reached from three kernel and UI paths (`0x1008_8BA8`, `0x100A_6AD4`, `0x1026_A560`) |
| track-dependent (ms computed) | `0x1024_ACE0` |

  The `0x1024_xxxx` module is most likely the playback engine (track start, seek) [hypothesis].
- **The 80 MHz pin** (`osos 0x101A_8710`, an object with the current frequency, initialised to 80,000). Entering the pinned state suspends the governor task (`osos 0x100C_FD78(1)` → kernel `0x100A_671C`); leaving resumes it. Its only callers are a disk-mode object's enter and exit (`0x1016_39CC`, `0x1016_3A30`). The exit releases disk channel 17 (DiskModeChannel) with a 5 s linger [apple-RE]. Both methods ask for 80,000 kHz, and the pin object returns early when the request equals its current value. So no unpin was seen. That fits a restart after disk mode [hypothesis]. The same two methods stop and start the audio engine (§2.4).
- **The driver.** The PP5022 class's set method (IRAM, file `0x71_684C`) takes a pointer to kHz:
  - requests in 23–26 MHz snap to the 24 MHz crystal;
  - an exact 24,000 turns the PLL off;
  - otherwise it uses Apple's PLL recipe (`mac-and-dvs.md` §3.1).

  So awake, after the clamp, Apple never runs below 26 MHz on the PLL.
- **The bridge divider** (`osos 0x1000_0884`, run when the clock changes). For f ≥ 26 MHz, with n = ⌈(f − 24,000)/3,000⌉:
  - `MLCD_SCLK_DIV` bit 4 is set when n ≤ 1;
  - bit 0 is set when n ≤ 2;
  - the field `0x77` is otherwise cleared.

  So at 26–27 MHz `0x11` is set, at 27–30 MHz `0x01`, and above 30 MHz neither [apple-RE]. Rockbox writes `MLCD_SCLK_DIV` only for the Mini 2G (`firmware/target/arm/pp/system-pp502x.c:482`) [code]. What the bits do on the 5.5G's bridge is [unknown].
- **A second, older clock path** (`osos 0x1000_067C`, in MHz, programs the PLL directly). Only the hibernation writer (80) and the 80 MHz pin use it [apple-RE].

**ProkBox today** [code]:
- `firmware/target/arm/pp/system-pp502x.c:446-617`: 24 MHz with the PLL off as the default; 30 MHz on the PLL (`CPUFREQ_NORMAL`); 80 MHz on boost; 48/60/100 MHz only under `HAVE_PP_GOV`.
- `clockpol` (`../design/lit-24mhz.md` §3.2) decides the PLL holds.
- `firmware/pgov.c` (`../design/pp-governor.md`) is a load-measured band governor, compiled in (`firmware/export/config/ipodvideo.h:400`) but off unless the developer marker is set (`firmware/pgov.c:16`).
- The tick pulses the COP 100 times a second (`firmware/target/arm/pp/kernel-pp.c:36`).

### 2.2 Disk

[apple-RE unless marked]:
- **The task.** `DiskMgrTask` (entry `osos 0x1028_4B0C`, created at `0x1025_D40C`) keeps an active byte and a millisecond countdown per channel (state at `0x1086_BEDC`/`0x1086_BF0C`). It waits on its queue (20) with a timeout equal to the smallest countdown left.
- **The client calls.** Clients call:
  - obtain (`osos 0x1009_4E6C`; event 2 "Disk Obtain Access");
  - release with a linger in ms (`osos 0x1009_4D80`; event 3).
- **When the last countdown expires,** it logs event 1 "Disk Spindown" and tells the drive object the disk is no longer needed (`osos 0x1028_4DBC`/`0x1028_4DC8`). **There is no global spin-down timeout: the longest pending linger is the timeout.**
- **The drive power-off** (`osos 0x1023_3358` → `0x1023_332C` → `0x1026_5CC8(0)`), in order:
  1. wait for the drive to go idle;
  2. shut the IDE block;
  3. clear `DEV_EN` bit 25 (`DEV_IDE0`), set the GPIO G/H enables to `0xFF`, OR the I/K enables;
  4. on board ID 12 only, switch PMU LDO index 4 (code 22) off.

  The order is the same as Rockbox's `ide_power_enable()` [apple-RE; the meaning of each step is a strong hypothesis].
- **Linger values** (channel numbers from the name switch at `osos 0x1021_1DDC`):

| Channel | Linger | Site |
|---|---|---|
| 2 BufferedSongReading | **250 ms**, or 2 s | `osos 0x100A_BFA4`, `0x1020_3394` |
| 43 StreamCacheSeek | 250 ms | `0x1017_A7EC` |
| 44 PredictiveCacheSpinup | 2 s | `0x100A_97E0` |
| 25 AlbumArtReading | 0 or 30 ms | |
| most UI clients | 1 s | |
| 3 PrefsWriting | 0 or 5 s | |
| 6 CacheSpinupDrive | 0.5, 5 or 10 s | |
| 17 DiskModeChannel | 5 s | `0x1016_3A60` |
| 1 AppBoot | 15 s | |
| 36 eAppAccess | 5, 10 or 30 s | |

- **Predictive spin-up (present since 1.2.1, `apple-disk.md` §3.2).** The next-track handler (`osos 0x100A_9734`, event 5 "Next Track") obtains channel 44 and releases it with 2 s on a user skip when a cache flag is set. That starts the drive ahead of the reads the skip will need [apple-RE; the purpose is a hypothesis].
- **What changed between versions.** 1.2.1 and 1.2.3 name 21 channels in the debug table; 1.3 names 45. **Correction (2026-11-06, `apple-disk.md` §7):** the first edition read this as a rewrite. A call-site comparison shows 1.2.1 already uses channels up to 44, including the predictive spin-up, with the same release message; only the name table grew. The real 1.3 change is the pre-spin hold (channel 6), 10 s → 500 ms [apple-RE].
- **The full study** of the arbiter, the ATA commands, the power-on and recovery sequences and the two storage profiles is [`apple-disk.md`](apple-disk.md).

**ProkBox today** [code]:
- `firmware/export/stor_pwr.h:108-111`: one hold for the whole drive, 3 s dark and 60 s lit, doubling up to 600 s.
- `firmware/drivers/ata.c:131-136`: the ATA sleep timeout is 1 s on flash, 5 s on HDD.
- `firmware/target/arm/ipod/power-ipod.c:118-138`: the rail cut, the same steps as Apple's.

### 2.3 Display

[apple-RE]:
- **`BacklightTask`** (entry `osos 0x1028_453C`) is a four-state machine: off, on, dimming, fading in. Its messages:
  - `0xA6` on;
  - `0xA7` off now;
  - `0xA8` dim, then off;
  - `0xAA` its timer.
- **Turning off.** The level goes to 0 at once (`osos 0x1028_3348(0)` → `0x1026_617C`). The supply lines are dropped 1 s later (a delayed `0xAA`, 1,000 ms, `osos 0x1028_49FC`) through `0x1026_6288(0)`, which drives two GPIO lines (Apple pins 0x27/0x66 and 0x13/0x4E). The brightness on our board class is a pulse train on pin 0x67 (`osos 0x1026_61F8`).
- **The panel stays on while dark.** The panel on/off routine (`osos 0x1028_3284`) has one caller, the pad-parking step of the sleep entry (`osos 0x1028_25CC`).
- **The LCD supply stays on too.** The regulator-state routine (`osos 0x1028_262C`) is called only by the sleep manager and by `osos 0x1026_6464` (states 4/1, USB-related; §2.6).

So, **dark and awake, Apple turns only the backlight off.** The panel and the LCD LDO (`0x26`) stay on until light sleep.

**ProkBox today** [code]:
- The backlight's enable lines drop at once after the fade (`firmware/target/arm/ipod/backlight-nano_video.c:73-102`).
- The LCD sleeps 10 s after the backlight goes off (`firmware/backlight.c:222`; `firmware/target/arm/ipod/video/lcd-video.c:1505`).
- The BCM2722 is cut (C0) in dark play (`../design/p19-placement.md` §2.1).

### 2.4 Block clock gating while awake

[apple-RE unless marked]:
- **A starting set.** One routine (`osos 0x1018_B89C`, restored by `0x1018_B800`) saves `DEV_EN`, sets `PLL_CONTROL` bits 25 and 27, and writes `DEV_EN` = **`0xC000_0137`** (bits 31, 30, 8, 5, 4, 2, 1, 0). That this is Apple's baseline is a [hypothesis]. Rockbox and ProkBox start from `0xC200_0124` (`firmware/target/arm/pp/system-pp502x.c:636`) [code].
- **Few bits move at run time.** 1.3 has only a handful of `DEV_EN`/`DEV_RS` stores.
- **I2S (bit 11), around USB disk mode.** The disk-mode object's two methods (`osos 0x1016_39CC` enter, `0x1016_3A30` exit; vtable `0x1066_9DE8`) stop and start the audio engine.
  - **Stop** (`osos 0x1018_A900`), in order:
    1. park the COP by flag and acknowledge (`0x1493_7139`/`0x1493_713A`);
    2. codec to mode 1 (`osos 0x1009_5F0C`);
    3. clear `DEV_I2S`.
  - **Start** (`osos 0x1018_A998`), in order:
    1. copy an overlay to IRAM `0x4000_1808`;
    2. set `DEV_I2S`;
    3. codec to mode 3 (`osos 0x1009_9098`);
    4. wake the COP.
  - Both methods also request 80 MHz (`osos 0x101A_8710`, §2.1).
  - The same pair exists in 1.2.1 (`0x1018_4D14`/`0x1018_4D88`) and 1.2.3 (`0x1018_57E4`/`0x1018_5858`).
  - **Apple does not gate I2S on pause**; this is the only place it moves.
- **External clocks (bit 1).** A client-mask helper (`osos 0x1026_4EA8`, writing through `0x1000_053C`) keeps `DEV_EXTCLOCKS` on only while client 3 or 5 holds it. The clients are [unknown].
- **USB1 (bit 22).**
  - On (`osos 0x1026_63AC`): set `DEV_USB1` and `DEV_INIT2` bit 31, pulse `0x7000_0028` bit 11, wait for bit 7.
  - Off: clear `0xC500_0140` bit 0, pulse `0x7000_0028` bit 16, wait, then clear `DEV_USB1` and `DEV_INIT2` bit 31.
  - The wrapper (`osos 0x1026_6464`) also sets regulator state 4 or 1. On a PP5022 these are the same.
- **Switched on at init and never off while awake:**
  - PWM (bit 17, the piezo);
  - SER0/SER1;
  - I2C (bit 12; re-run on error 21);
  - OPTO (bit 16);
  - `DEV_EN2` bit 13 with `0x6000_60A0` = `0xC000_0000` (the LCD2 bridge init, bridge at `0x7000_8A00`);
  - `DEV_EN2` bit 8.
- **IDE0 (bit 25)** changes only with the drive rail (§2.2).

**ProkBox today** [code]:
- `DEV_I2S` and `DEV_EXTCLOCKS` are set once (`firmware/target/arm/pp/wmcodec-pp.c:63,66`) and never cleared. The DAC standby (`firmware/drivers/audio/wm8758.c:98`) leaves `DEV_EN` alone.
- `DEV_IDE0` follows the drive rail (`firmware/target/arm/ipod/power-ipod.c:118-138`).
- `DEV_OPTO` goes off under Hold in the soft sleep (`firmware/target/arm/pp/power_sleep-pp502x.c:353`).

### 2.5 USB

[apple-RE unless marked]:
- **On a charger only.** Apple's probe (`osos 0x1022_09FC`, called from `0x1024_CFAC`) powers the USB block, waits about 100 ms [hypothesis on the unit], then tests a status bit (`osos 0x101C_28F0` bit 11, "a host has spoken" [hypothesis]). **If no host appeared, it powers the controller and PHY off again.** The same probe is in 1.2.1 (`0x1021_76B0`) and 1.2.3 (`0x1021_88D0`).
- **The PHY suspend** (`osos 0x1026_66F4`) is called only by the sleep manager (`osos 0x100B_4618`; undone at `0x100B_453C`). It is not used while awake.
- **ProkBox today** [code]:
  - On VBUS, `usb_enable(true)` clocks `DEV_USB0`/`DEV_USB1` and holds the PLL (`firmware/target/arm/pp/usb-fw-pp502x.c:176-189`).
  - The stack stays in `USB_POWERED` for as long as a charger is plugged in (`firmware/usb.c:499`, `firmware/usb.c:896-899`).
  - `usb_reset_controller` ORs `0x47A` into `XMB_RAM_CFG` (`firmware/target/arm/pp/usb-fw-pp502x.c:122`); Apple never writes that register while awake.

### 2.6 Timers, the COP and memory timing

[apple-RE unless marked]:
- **Timer 2** is the piezo's tone timer only (one-shot start `osos 0x100C_719C`, stop `0x1011_C750`).
- **Timer 1.** Nothing writes `TIMER1_CFG` at run time except the light-sleep stop (`osos 0x1000_07F8`) and restore (`0x1000_0668`). This holds for literal and derived addresses alike. So **Apple does not slow its tick while awake or dark** (a negative finding). Where its rate is first set is [unknown]: the loader, or a store through a data pointer.
- **The COP** is woken by work (`osos 0x100C_FB08`, five callers) and by every governor pass (§2.1). So while awake, Apple's COP also sees about 100 wakes a second.
- **Memory timing.**
  - `DEV_TIMING1` changes only with the clock (`mac-and-dvs.md` §3.3). A second calculator (`osos 0x1026_634C`) gives about one wait state per 11.1 MHz, capped at 15.
  - `XMB_RAM_CFG` is written only by the sleep path and the `0x1018_B800`/`0x1018_B89C` pair, so **SDRAM refresh is never retuned awake**.
  - `0x7000_0030` bits 15:12 are set to 6 once (`osos 0x1026_BB90`); meaning [unknown].
  - `CACHE_PRIORITY` (`0x6000_6044`) gets bit 5 or bit 4 depending on which core runs `osos 0x101E_FC2C`, and `0x3F` in sleep.
- **ProkBox today** [code]: a fixed 100 Hz tick that pulses the COP each time (`firmware/target/arm/pp/kernel-pp.c:31-36`). Parking the COP (K-1) removes the pulse in dark play (`../design/p19-placement.md` §3.4).

### 2.7 The codec (WM8758)

[apple-RE unless marked]:
- **The back end.** `osos 0x1026_53A4` picks a back end from the board ID's high halfword. Board 11 (the 5G/5.5G [hypothesis]) gets back end 3 (`osos 0x1028_39B8`), the same code in 1.2.1 (`0x1027_9C04`). Every codec register write goes through `osos 0x1028_40D8` (I2C device `0x1A`).
- **Mode 4, power-up.** In order:
  1. bias and anti-pop set-up: BIASCTRL `0x101`, OUTCTRL `0x186`, R42 `0x006`, R43 `0x004`;
  2. power the outputs: PWR2 `0x180`, PWR3 `0x060`;
  3. OUTCTRL `0x10F` → `0x10D`, R43 0, OUTCTRL `0x104`;
  4. PWR3 `0x06F`, PWR1 `0x02D`;
  5. **wait 100 ms**;
  6. OUT1/OUT2 volumes to 0 with update, BIASCTRL `0x100`, OUTCTRL `0x100`.
- **Mode 3** writes nothing; it records "running".
- **Modes 1 and 2, full off.** Mute OUT1/OUT2, OUTCTRL `0x180`, then PWR1/PWR2/PWR3 = 0, so VMID is off too.
- **The codec's clock follows its power.** Around every sequence, `osos 0x1026_4EA0` → `0x1000_053C` sets `DEV_EXTCLOCKS` (bit 1). Modes 1 and 2 clear it at the end; mode 4 leaves it on.
- **Callers.** The codec changes mode only in four places:
  - sleep entry and pad parking (mode 1);
  - the wake (modes 4 then 3);
  - the disk-mode stop and start of the audio engine (§2.4).

  **A pause or a stop never reaches standby or off;** while awake, the codec stays fully powered.
- **ProkBox today** [code; silicon]:
  - The DAC goes to standby 5 s after PCM stops (`firmware/export/dac_ext.h:73`), keeping VMID at 500 k (`firmware/drivers/audio/wm8758.c:98-117`).
  - The measured wake is 6.4 ms median (`../design/p19-placement.md` §1.2, P12).
  - `DEV_EXTCLOCKS` is always on (`firmware/target/arm/pp/wmcodec-pp.c:66`).
  - **We already beat Apple on pause.**

### 2.8 The PMU (PCF5060x)

[apple-RE unless marked]:
- **All PMU writes go through one writer** (`osos 0x1028_3634`), from ten call sites:
  - **the interrupt masks**, once: `INT1M`/`INT2M`/`INT3M` = `0x50`/`0xFC`/`0x0F`, written by `PCFPowerMgr` (`osos 0x1028_4FF4`). That task then blocks on the PMU's interrupt. On each one it reads INT1–INT3 (`osos 0x100A_7A30`) and acts on two bits only: INT1 bit 7 (the alarm) and INT1 bit 1. With the PCF50606 datasheet's bit names [hypothesis], the 1 Hz SECOND interrupt and ADC-ready stay masked. **It never polls the PMU.**
  - **the ADC, per conversion** (`osos 0x1028_341C`): write ADCC1/ADCC2, **wait 20–35 µs, then poll ADCS2 bit 7** before reading. The battery is channel 3, subtractive against ADCIN1 (`osos 0x1026_5F74`).
  - **the accessory supply** `D2REGC1` (3.3 V): mode 7, `010` or off, set by the accessory managers (`osos 0x1026_4D78`).
  - the codec and LCD LDOs, in sleep only (`apple-power.md` §2.2);
  - the RTC;
  - `OOCC1` (`apple-power.md` §3.3).
- **Never written:** the charger (`MBCC`), `BVMC` (read only, `osos 0x1028_36B0`), the DCDC converters on our PMU type, the LED and `GPOC` registers. So **Apple leaves the charger in the state the PMU's defaults and the boot ROM set.**
- **Battery bars** (`osos 0x101D_FE44`): 0–20. While discharging the bars fall at most one step per sample; while charging, 6 samples are skipped. The sampling rate is [unknown].
- **ProkBox today** [code]:
  - `adc_scan()` reads the result without waiting for the done bit (`firmware/target/arm/ipod/adc-ipod-pcf.c:74`). It may return the previous conversion [hypothesis].
  - The accessory supply is a hidden setting that defaults to on, 3.3 V (`apps/settings_list.c:1563`; `firmware/target/arm/ipod/powermgmt-ipod-pcf.c:154-160`, `D2REGC1` = `0xF8`), even though this unit has no dock connector. Since 0.8.3 (one build, #236) it is on by default on every unit (`ACCESSORY_SUPPLY_DEFAULT true` in `ipodvideo.h`); there is no dock build setting any more.

### 2.9 Inputs and sensors

Each source is a GPIO-edge debouncer (`osos 0x1016_3FFC`; pin = port × 8 + 8 + bit). It blocks on its edge interrupt and **never polls** [apple-RE]. The debounce units are ms [hypothesis].

| Source | Pin | Debounce | Evidence |
|---|---|---|---|
| Hold | GPIOA bit 5 | 100 | `osos 0x1018_93A4` |
| USB power (VBUS) | GPIOL bit 4 | 400 | `osos 0x1017_11B8` |
| Low battery (board types 10–12 only) | GPIOA bit 3 | 20 | `osos 0x1022_EC90` |
| Headphone | interrupt `0x87` | 1 s quiet | `osos 0x1028_4EB0` |

- **When the low-battery pin goes low,** the UI is told once a minute (gated on the RTC minute). With no UI registered, `battislo` goes to the IRAM mailbox and the unit powers off (`osos 0x1028_1B28`) [apple-RE].
- **Message `0x94`** (low battery, in `AlarmTask` `osos 0x1028_4374`) posts event 18 and calls back. With a configuration byte (`0x1081_DFE8`), it goes to standby after about 1 s. Its sender is [unknown].
- **ProkBox today** [code]:
  - Hold and headphones are polled on every 100 Hz tick (`firmware/drivers/button.c:222`; `firmware/target/arm/ipod/button-clickwheel.c:398-404`), with a headphone debounce of `HZ/2`.
  - GPIOA bit 3 is not used.
  - The pin assignments for Hold and VBUS match Apple's.

### 2.10 What changed between 1.2.1, 1.2.3 and 1.3

| Area | 1.2.1 → 1.2.3 | 1.2.3 → 1.3 | Evidence |
|---|---|---|---|
| Clock governor parameters | same | same (all 23 words identical) | the block at file `0x1704` in all three [apple-RE] |
| Light-sleep core and its constants | same | same | `apple-power.md` §2.1 |
| Disk channels | 21 named, same set | **45 named in the debug table**; the code used all 45 numbers already in 1.2.1 (corrected 2026-11-06) | string sets; release call sites by (channel, linger) [apple-RE] (`apple-disk.md` §7) |
| Predictive spin-up on a skip | present (`osos-1.2.1 0x100A_A9B8`) | same | channel 44 [apple-RE] |
| Pre-spin hold (channel 6) | 10 s | **500 ms** | `osos 0x100E_1E20` [apple-RE] |
| Audio-engine stop/start around disk mode (I2S gate, codec mode 1/3) | present | present | `0x1018_4D14`/`0x1018_4D88` (1.2.1), `0x1018_57E4`/`0x1018_5858` (1.2.3) [apple-RE] |
| USB charger-only probe | present | present | `0x1021_76B0` (1.2.1), `0x1021_88D0` (1.2.3) [apple-RE] |
| Codec back end 3 | same | same | `0x1027_9C04` (1.2.1) [apple-RE] |

**Reading.** By what the code shows, **1.3's battery work on the PP side is one disk linger**: the pre-spin hold cut from 10 s to 500 ms (plus a few small lingers, `apple-disk.md` §7). The clock governor, sleep, codec and USB policies were already in 1.2.1. Anything else that changed in power would sit in code we didn't diff function by function; that is [unknown], not "nothing".

---

## 3. The catalogue: awake now (ship as automatic A/B with a fallback)

Ranked by expected gain per unit of effort. Effort: S = under a day with its test, M = a few days, L = a phase.
- **Gain** is the [estimate] for dark playback unless the row says otherwise. Each one rests on `../design/p19-placement.md` §0 and §5.2: ~35–40 mA dark FLAC, the drive's powered idle at the order of 65 mA.
- **Plan** is one of:
  - **adopt**: do what Apple does;
  - **beat**: go further than Apple;
  - **skip**: leave it, with the reason;
  - **done**: we already match or beat it.
- Every row that changes behaviour ships behind an automatic fallback that logs loudly.
- **Status** (P19 `p19-pwr-awake`, 2026-11-05): built rows sit behind `firmware/export/pwr_awake.h`'s gate. A row becomes the daily default only when its bench row passes and its token goes into `firmware/export/pwr_awake_tokens.h`; the release compiles a row's code only from then on.

| # | Apple 1.3 behaviour | Evidence | ProkBox today | Gap | Plan, and why | Gain / effort | Risk | Test | Status |
|---|---|---|---|---|---|---|---|---|---|
| AW-1 | **Per-client disk linger: 250 ms after a song-buffer read**, 1 s for UI reads, 5 s after preference writes; the drive is cut when the last linger expires | `osos 0x1028_4B0C`, `0x1009_4D80`, `0x100A_BFA4`, `0x1017_A7EC` [apple-RE] | one hold: 3 s dark, 60 s lit, doubling to 600 s (`firmware/export/stor_pwr.h:108-111`); ATA sleep 1 s on flash (`firmware/drivers/ata.c:131-136`) | ~2.75 s of powered idle per dark refill | **adopt**, per reason: 250 ms after a refill that reached its target, 1 s for browsing, writes piggy-backed as today. Keep 3 s after a partial or failed refill. Fall back to 3 s automatically if the drive is re-powered within 5 s of a cut. Record in `docs/storage-choices.md` (the HDD keeps its spin-up-aware hold) | **0.3–1.1 mA** (6–22 power-ups/h × 2.75 s at ~65 mA) / **S** | more power cycles if reads straggle after a refill; the fallback covers it | T-LINGER | **built** (`p19-pwr-awake`): flash, dark, 250 ms after a fill that reached its target; back to the 3 s hold after two rewakes within 5 s; on in developer builds; default with token `pwraw-linger` (bench `awlg`) |
| AW-2 | **USB on a charger only: powers the controller and PHY off again** when no host speaks within ~100 ms of power-up; re-probes on the next VBUS edge | `osos 0x1022_09FC`, `0x101C_28F0` bit 11 [apple-RE]; the unit and the bit's meaning [hypothesis] | `USB_POWERED` for as long as a charger is in: `DEV_USB0/1` clocked, the PLL held at 30 MHz (`firmware/target/arm/pp/usb-fw-pp502x.c:176-189`, `firmware/usb.c:499`) | the PP and the USB block run for hours on a wall charger | **adopt and beat**: a 1–2 s host window (wider than Apple's, for slow hosts), then controller off and the PLL hold released. Re-probe on every VBUS edge and on the UI's USB screen entry | **2–5 mA while on a charger** (the PLL at 30 MHz plus USB clocks, against 24 MHz with the PLL off): faster, cooler charging; no gain on battery / **S–M** | a slow host missed: widen the window, re-probe on a bus reset seen through the PHY status | T-USBPROBE | **built**: a 12 s window (past `usb_core`'s 10 s no-host call, so the charger current is kept), then parked; re-probe on VBUS and on screen wake; a late host doubles the window, two give it up; on in developer builds; default with token `pwraw-usboff` (bench `awus`); **default**: `awus` PASS, token `pwraw-usboff` set [silicon, sitting 4gh, 2026-11-06] |
| AW-3 | **The accessory supply (`D2REGC1`, 3.3 V) is switched by the accessory managers only**, on when an accessory needs it | `osos 0x1026_4D78` [apple-RE] | on at 3.3 V by default (`apps/settings_list.c:1563`, `firmware/target/arm/ipod/powermgmt-ipod-pcf.c:154-160`), on every unit since 0.8.3 (one build, no dock dimension, #236), although this unit has no dock connector | an LDO and whatever hangs on it powered for nothing | **beat**: off when no accessory needs it (until 0.8.2: off on builds without `HAVE_IPOD_DOCK30`; with one build, only an accessory manager can switch it, as Apple does) | **0.05–1 mA** (the LDO's quiescent current, plus any load the USB-C mod board draws from that rail [unknown]) / **S** | the mod board may use that rail (Type-C audio accessory mode [hypothesis]); the test checks USB, charging and audio over USB-C | T-D2REG | **flag** none since 0.8.3 (`HAVE_ACCESSORY_RAIL_LOAD` removed, #236: on by default on every unit, owner 2026-11-11); bench `awd2`/`awd2c` (T-D2REG); no token: a per-unit fact; `awd2`/`awd2c` PASS: the rail off 5 s with the USB-C board's audio playing on, so its audio multiplexer is not on this rail [silicon, sitting 4gh, 2026-11-06]. Default **on** on every unit (owner, 2026-11-11, one build; an accessory-driven switch is the remaining candidate); enumeration and charging with the rail off not yet tried |
| AW-4 | **The LCD LDO stays on while dark but awake**; only sleep drops it to mode `010` | `osos 0x1028_262C`, callers only in sleep and `0x1026_6464` [apple-RE] | LCD and BCM asleep 10 s after the backlight goes off (`firmware/backlight.c:222`, `firmware/target/arm/ipod/video/lcd-video.c:1505`); LDO untouched | the sleeping panel's rail still at full regulation | **beat** (`apple-power.md` A4): mode `010` on `0x26` while the LCD sleeps, back to `111` before `lcd_awake()` | **0.2–1 mA** [estimate, wide] / **S** | the panel may not restart cleanly from `010`, or the wake grows | T-LCDLDO (= S-4) | **built**, off until token `pwraw-lcdldo` (bench `awld`, or the probes' `pldo` screen part); **default**: `awld` PASS (5 of 5 lit, worst wake 447 ms) and `pldo`'s screen part restored, token `pwraw-lcdldo` set [silicon, sitting 4gh, 2026-11-06] |
| AW-5 | **The codec's clock follows its power** (`DEV_EXTCLOCKS` off with the codec off); **I2S gated only when the audio engine stops** (disk mode), never on a pause | `osos 0x1026_4EA0`, `0x1018_A900`/`0x1018_A998` [apple-RE] | DAC standby 5 s after PCM stops (`firmware/export/dac_ext.h:73`), VMID kept; `DEV_I2S` and `DEV_EXTCLOCKS` always on (`firmware/target/arm/pp/wmcodec-pp.c:63,66`) | two clocked blocks idle while paused | **beat**: in DAC standby, clear `DEV_EXTCLOCKS` and `DEV_I2S`; set them again (and re-run the I2S init if needed) before the DAC wakes | **0.1–0.5 mA, paused only** [estimate] / **S** | lost I2S or FIFO state; a pop at wake | T-I2SGATE | **built**, off until token `pwraw-i2sgate` (bench `awi2`, variants a then b); the I2S set-up compared and restored at the ungate; `awi2` **FAIL**: 2 of 6 resumes late (variant b found `EXTCLOCKS`/`I2S` still off), the I2S words intact [silicon, sitting 4gh, 2026-11-06]; stays off |
| AW-6 | **No PMU polling**: `PCFPowerMgr` blocks on the PMU interrupt; the SECOND and ADC interrupts stay masked; the battery is read on demand | `osos 0x1028_4FF4`, `0x100A_7A30` [apple-RE] | the power thread polls (0.5 wakes/s quiet, 2/s otherwise; `../design/p19-placement.md` §1.2 P2) | wakes and I2C traffic | **adopt in part**: charger and USB edges from the PMU interrupt and GPIO; the battery read on the planned 10 s dark step (`p19-placement.md` §5) | **< 0.1 mA** / **M** | a missed charger edge; keep a slow poll as the net | none (normal use; log edges against polls) | open |
| AW-7 | **`MLCD_SCLK_DIV` (`0x6000_602C`) follows the core clock**: bits 4 and 0 set at ≤ 27 MHz, bit 0 at ≤ 30 MHz, clear above | `osos 0x1000_0884` [apple-RE]; what the bits do on the 5.5G's bridge [unknown] | written only for the Mini 2G (`firmware/target/arm/pp/system-pp502x.c:482`) | unknown | **test first**, then adopt if it lowers the slope or is needed for clean bridge timing at 24 MHz | unknown, likely small / **S** | a bridge or display glitch | T-MLCD | open |
| AW-8 | **Apple never writes `XMB_RAM_CFG` while awake** | stores only in sleep and `osos 0x1018_B800`/`0x1018_B89C` [apple-RE] | `usb_reset_controller` ORs `0x47A` into it (`firmware/target/arm/pp/usb-fw-pp502x.c:122`, from Rockbox) | an SDRAM-controller change of unknown effect after every USB reset | **check**: log the value before and after; if bits that look like refresh or power-down move, find out why Rockbox sets them | unknown / **S** | none for the read-only log | T-XMBCFG | **built** (read-only log, developer builds; bench `awxm`) |
| AW-9 | **PMU ADC: waits 20–35 µs and polls the done bit** before reading; battery on channel 3 against ADCIN1 | `osos 0x1028_341C`, `0x1026_5F74` [apple-RE] | writes ADCC1 and reads at once (`firmware/target/arm/ipod/adc-ipod-pcf.c:56-57`); the I2C write itself probably covers the wait [estimate] | a possibly stale sample | **adopt** the done-bit poll (bounded) | accuracy, not current / **S** | none | none (log a done-bit miss count) | **built**, every build (bounded at 4 reads); developer builds count misses |
| AW-10 | **Inputs are edge interrupts with debounce**: Hold 100 ms, VBUS 400 ms, headphone 1 s quiet, low battery 20 ms; no polls | `osos 0x1016_3FFC`, `0x1018_93A4`, `0x1017_11B8`, `0x1028_4EB0`, `0x1022_EC90` [apple-RE] | Hold and headphone polled on every 100 Hz tick (`firmware/drivers/button.c:222`, `firmware/target/arm/ipod/button-clickwheel.c:398-404`) | none today; it blocks a slower or tickless dark tick | **adopt with the tickless dark tick** (`p19-placement.md` §5.4), not before | **~0 alone**; enables the tick cut / **M** | a missed edge; keep a slow poll | none | open |
| AW-11 | **Predictive spin-up on a rapid user skip** (present since 1.2.1; `apple-disk.md` D-5): obtain the disk, release with 2 s | `osos 0x100A_9734`, `0x100A_97E0` [apple-RE]; the purpose [hypothesis] | none | skip latency on a hard disk | **skip on flash** (no spin-up to hide); **adopt on HDD units** as a latency item, with a `docs/storage-choices.md` entry | a small **cost** on flash; latency on HDD / **S** | an extra spin-up per skip on HDD | none | open (HDD only) |
| AW-12 | **Clock: a 10 ms load governor, 26–80 MHz on the PLL, timed 80 MHz bursts of 1–30 s, an 80 MHz pin in disk mode** | §2.1 [apple-RE] | 24 MHz with the PLL off, 30/80 MHz by holds and boosts; `pgov` built in, off by default (`firmware/pgov.c:16`) | **ProkBox is below Apple's floor**: the 24 MHz crystal against 26 MHz on the PLL | **beat, done**: keep the crystal floor. Take Apple's governor as a prior for `pgov` (a 10 ms window, a 75 %/40 % rise and decay, timed bursts instead of open-ended boosts) | none to adopt; it confirms ours / — | — | none | done |
| AW-13 | **The COP is pulsed ~100 Hz** by the governor pass; **Timer 1 is never slowed** while awake | `osos 0x100C_FB08`, `0x1000_07F8`/`0x1000_0668` [apple-RE] | the same 100 Hz tick and COP pulse (`firmware/target/arm/pp/kernel-pp.c:31-36`) | level | **beat**: K-1 parks the COP in dark play (planned); a coarse dark tick later | K-1 is part of the "second core" item in `p19-placement.md` §0 / M (planned) | — | none (K-1's own bench) | planned (K-1); built (developer, `corelayout_set_park()`); `k1`: COP wakes 100.5/s → 0.6/s, 0 underruns [silicon, sitting 4gh, 2026-11-06] |
| AW-14 | **The codec stays fully powered on pause and stop** | §2.7 [apple-RE] | standby after 5 s, 6.4 ms wake [silicon] | — | **done (ahead)** | — | — | — | done |
| AW-15 | **Drive rail cut**: IDE block off, `DEV_IDE0` off, GPIO G/H/I/K parked | `osos 0x1026_5CC8` [apple-RE] | the same steps (`firmware/target/arm/ipod/power-ipod.c:118-138`) | — | **done (level)** | — | — | — | done |
| AW-16 | **Backlight off in two stages** (level 0, supply lines 1 s later) | `osos 0x1028_49FC` [apple-RE] | lines dropped right after the fade (`firmware/target/arm/ipod/backlight-nano_video.c:95-100`) | none for power | **skip** (cosmetic; only if a quick re-light shows a flash) | — | — | — | skip |
| AW-17 | **A low-battery GPIO** (GPIOA bit 3, board types 10–12) with a 20 ms debounce; powers off with `battislo` when no UI listens | `osos 0x1022_EC90`, `0x1028_1B28` [apple-RE] | not used | a hardware low-battery signal we may have | **test**, then use it as an early cut-off warning if it is wired | safety, not current / **S** | none (read-only) | T-LOWBATT-PIN | open |

## 4. The catalogue: sleep, later (the soft-sleep feature, after layout A's acceptance bench)

The rows are the `apple-power.md` §7 list (A-n) plus the codec and PMU rows this study added. Only the probes in the last column go on the bench now.

| # | Apple 1.3 behaviour | Evidence | ProkBox today | Plan, and why | Effort | Risk | Probe now |
|---|---|---|---|---|---|---|---|
| SL-1 | SDRAM self-refresh (`XMB_RAM_CFG` bit 22, acknowledged in bit 30) from IRAM, then the clocks gated (A1) | `apple-power.md` §2.3 [apple-RE] | the P18 soft sleep keeps SDRAM in auto-refresh at 24 MHz (`firmware/target/arm/pp/power_sleep-pp502x.c`) | **adopt**: the largest sleep cut | M | a hang if bit 30 never sets: a bounded poll and a watchdog | S-1, S-2 |
| SL-2 | `DEV_EN` = `0xC001_0108`, `DEV_EN2` = 0 in sleep (A2) | same | the clickwheel kept, OPTO off under Hold (`power_sleep-pp502x.c:353`) | **adopt** | S | lost wake sources | S-3 |
| SL-3 | **Codec fully off in sleep** (mode 1: OUT1/OUT2 muted, OUTCTRL `0x180`, PWR1–3 = 0, VMID off, `DEV_EXTCLOCKS` off); at wake, mode 4's anti-pop power-up with a 100 ms wait | §2.7 [apple-RE] | standby with VMID on through sleep | **adopt** in sleep only (the 100 ms wake cost is fine there, not on a pause) | S | a pop at wake: copy Apple's order | T-CODECOFF |
| SL-4 | Codec and LCD LDOs to mode `010` (A4) | `apple-power.md` §2.2 step 12 | none | **adopt** | S | panel or codec restart | S-4 (T-LCDLDO); LCD half: mode `010` restores cleanly [silicon, sitting 4gh, 2026-11-06]; codec half not run |
| SL-5 | Pad parking (A5) | `apple-power.md` §2.2 step 10 | none | **adopt** if it measures | S | a pin left in the wrong state | S-5 |
| SL-6 | Both timers stopped; wakes only from events. The PMU masks `0x50`/`0xFC`/`0x0F` keep the SECOND and ADC interrupts off (A6) | `apple-power.md` §2.2 step 7; `osos 0x1028_4FF4` | a 60 s timer poll asleep | **adopt**: the 60 s poll becomes an RTC alarm | M | a sleep nothing can end; the hold-MENU+SELECT reset stays | S-6; **no**: `0x6000_5014` does not count awake [silicon, sitting 4gh, 2026-11-06] |
| SL-7 | No `PROC_SLEEP` with a USB cable in (A3) | `apple-power.md` §2.3 | — | **adopt** (a rule) | S | — | — |
| SL-8 | USB PHY suspend, **in sleep only** (A7); Apple doesn't use it awake | `osos 0x1026_66F4`, its only caller `0x100B_4618` [apple-RE] | none | **adopt in sleep**; awake, AW-2 does more | S | the host loses the device | S-7 |
| SL-9 | Self-timed core stalls (A9) | `apple-power.md` §2.3 | busy-waits | **adopt** in the IRAM sleep code | S | wrong units | S-8; **no**: the count-mode stalls don't hold [silicon, sitting 4gh, 2026-11-06] |
| SL-10 | Suspend-to-disk through `hibe` (A10) | `apple-power.md` §3 | none | **later**, the owner's go | L | the bootloader guardrails | S-9 |
| SL-11 | Idle-to-sleep and light-to-deep timeouts | **[unknown]** | — | match by observation | — | — | T-APPLE-SLEEPTIME |

## 5. Bench test specs

The battery-probe agent takes these as written. The common rules:
- Every test restores the exact state it found, and logs the value it found before it changes anything.
- A test that writes a register carries a watchdog that puts the old value back.
- Power is judged by the voltage slope in a fixed voltage band, the logged duty and the wake counts. There is no current meter. At today's dark draw, 1 mA is worth about **0.36 mV/h** of slope (13.7 mV/h ≈ 38 mA, `../design/p19-placement.md` §0) [estimate].
- A/B arms alternate in the same voltage band, at least 30 min each.
- **Hang heads-up:** where a test can hang the unit, it says so. The recovery is always **hold MENU+SELECT** (a hardware reset), after which the unit boots normally with the test off.

### T-LINGER (AW-1): the per-reason disk linger
- **Inputs.** The same FLAC album, dark.
  - Arm A: today's 3 s dark hold.
  - Arm B: 250 ms after a refill that reached its target, 3 s otherwise.
- **Logged.** From the `stor_pwr` ring, per refill:
  - the end tick of the refill and the tick of the cut;
  - power-ups per hour and `stor_on_pct`;
  - the count of re-power-ups within 5 s of a cut (the fallback's trigger);
  - underruns and the voltage slope.
- **Expected.**
  - Arm B cuts ≤ 0.3 s after the refill, with re-powers within 5 s ≈ 0, and a slope 0.1–0.4 mV/h lower: **adopt**.
  - If re-powers within 5 s are common, reads straggle after the refill (metadata, art, the next handle). Find the straggler and fold it into the refill before shortening the hold.
- **Safety.** Only the hold constant changes; no hang risk.

### T-USBPROBE (AW-2): USB off on a charger only
- **Inputs.** A wall charger; the Mac; a second host if one is to hand (a hub, a PC).
- **Logged.**
  - The VBUS edge tick, the first bus reset, `SET_ADDRESS`, the window's expiry and the controller-off tick;
  - the PLL hold state and `DEV_EN`;
  - on the charger, the battery voltage rise per 10 min and the time to full against the previous charge.
- **Expected.**
  - Hosts enumerate inside the window, so nothing changes for them.
  - On the charger, the controller is off and the PLL released after the window, and charging is faster or level: **adopt**.
  - If any host misses the window, widen it and re-probe on a bus reset.
- **Safety.** `usb_enable(true)` comes back on every VBUS edge and on the USB screen. Worst case: a host doesn't see the unit until a re-plug. No hang risk.

### T-I2SGATE (AW-5): the codec clock and I2S gated in DAC standby
- **Inputs.** Pause, wait for the DAC standby, then gate.
  - Variant (a): clear `DEV_EXTCLOCKS` only.
  - Variant (b): clear `DEV_EXTCLOCKS` and `DEV_I2S`.
  - In each case hold 10 s, set the bits again before `dac_wake()`, and resume. Twenty cycles per variant, then 30 min A/B paused-dark.
- **Logged.**
  - `IISCONFIG` and the FIFO configuration before the gate and after the ungate;
  - `DEV_EN` found and restored;
  - the time from resume to the first sample, the codec register log, and I2C NAKs;
  - one listen for a pop (owner, once per variant);
  - the slope.
- **Expected.**
  - Registers intact, no pop, no NAK, a lower slope: **adopt** (b).
  - I2S registers reset: adopt (b) with the I2S init re-run at wake.
  - A pop or a NAK with (a): keep `DEV_EXTCLOCKS` on, and gate I2S alone.
- **Safety.**
  - No code touches an I2S register while it is gated.
  - Watchdog: if the FIFO isn't running 50 ms after resume, re-run the full I2S and codec init and log it.
  - `DEV_EN` goes back to the value found at the end.
  - **Hang heads-up:** a read of a gated block can stall the bus. Recovery: hold MENU+SELECT.

### T-D2REG (AW-3): the accessory supply off
- **Inputs.** Dark playback.
  - Arm A: `D2REGC1` = `0xF8` (today).
  - Arm B: `0x18` (off).
  - Then, with B: USB enumeration, charging, headphone audio, a USB-C audio adapter if the owner has one, Hold and the wheel.
- **Logged.** The register as read before and after, the slope, and pass or fail per function.
- **Expected.**
  - Nothing lost and a lower or equal slope: **adopt** as a runtime rule (off until an accessory needs it), since 0.8.3 has no dock build setting.
  - Anything lost: keep it on, and record that the mod board takes power from that rail (`docs/hardware.md`).
- **Safety.** It is an I2C write. `0xF8` comes back at exit and on any VBUS edge. No hang risk.

### T-LCDLDO (AW-4, = S-4): the LCD LDO in mode `010` while the LCD sleeps
- **Inputs.** Dark for ≥ 10 s, so the LCD sleeps. Then:
  1. write `0x26` with its voltage code kept and mode `010`;
  2. after 60 s, write mode `111`, wait 10 ms, and call `lcd_awake()`;
  3. repeat 20 times;
  4. then run a 30 min A/B dark.
- **Logged.** `0x26` found and written, the time to the first frame after the wake, BCM re-init or handshake errors, panel artefacts (one look per cycle in the first five), and the slope.
- **Expected.**
  - A clean panel and a wake within +5 ms: **adopt**.
  - The panel needs a re-init: adopt only if the re-init stays inside the lit wake budget.
  - Garbled, or no wake: **skip**.
- **Safety.**
  - Mode `111` is restored before any `lcd_awake()` (the hook is in the wake path, not the test).
  - Watchdog: no BCM answer 1 s after the wake → full LCD re-init and the LDO back to `111`.
  - **Hang heads-up:** the panel may stay dark until a reset. Recovery: hold MENU+SELECT.

### T-MLCD (AW-7): the bridge clock divider
- **Inputs.** Read `MLCD_SCLK_DIV` at boot. Then:
  1. lit, at 24 MHz, write `0x11` and run a 100-row glide;
  2. at 30 MHz, write `0x01`;
  3. at 80 MHz, write 0.
  4. Then run a 30 min lit A/B.
- **Logged.** The value found, frame times, BCM port handshake waits and timeouts, and the slope.
- **Expected.**
  - No change anywhere: **skip** (the bits drive a bridge the 5.5G doesn't use [hypothesis]).
  - Fewer timeouts or a lower slope: **adopt**, following Apple's rule.
  - More errors: skip.
- **Safety.**
  - The found value comes back at exit.
  - The existing BCM handshake timeout also restores it.
  - **Hang heads-up:** a wrong bridge clock can stall the LCD path. Recovery: hold MENU+SELECT.

### T-XMBCFG (AW-8): read-only
- **Inputs.** Normal use with a USB session.
- **Logged.** `XMB_RAM_CFG` at boot, before and after `usb_reset_controller()`, and at USB exit.
- **Expected.** If it changes, record which bits move. A later A/B without the OR then needs the owner's go, since it touches the SDRAM controller.
- **Safety.** No writes.

### T-LOWBATT-PIN (AW-17): read-only
- **Inputs.** A full discharge in normal use.
- **Logged.** `GPIOA_INPUT_VAL` bit 3 and the battery voltage every 10 s (through the power log).
- **Expected.**
  - The pin goes low near the cut-off: the signal is wired, so use it as an early warning.
  - It stays high or floats: it is not wired on this board.
- **Safety.** No writes.

### T-CODECOFF (SL-3; a sleep probe for the bench now)
- **Inputs.** In a P18 soft-sleep row:
  1. at entry, mute OUT1/OUT2, then OUTCTRL `0x180`, then PWR1–PWR3 = 0;
  2. at wake, Apple's mode-4 order (§2.7) with its 100 ms wait.
- **Logged.** The wake time to the first sample, the codec register log, and one listen for a pop.
- **Expected.** No pop and about +100 ms on the wake: **adopt** for sleep.
- **Safety.** It is an I2C write. The full `audiohw` init is the fallback. No hang risk.

### T-APPLE-SLEEPTIME (SL-11): the owner, on Apple's firmware
- **Inputs.**
  1. Start Apple's firmware (hold MENU at boot), with the owner's go for that sitting.
  2. Pause a song, let the backlight time out, and leave the unit alone for 1, 2, 5, 10, 30, 60 and 120 min, then overnight.
  3. After each wait, press MENU once.
- **Logged** (by hand), per wait, which of these happened:
  - the screen is back at once (awake);
  - it is back with a short delay at the same screen (light sleep);
  - it shows the Apple logo first (deep sleep).
- **Expected.** The step from "at once" to "delay" is the idle-to-light timeout; the step to "logo" is the light-to-deep timeout.
- **Safety.** No ProkBox writes. Apple's firmware keeps its own settings as in normal use.

### Sleep probes already specified
S-1 to S-9 in `apple-power.md` §8 (`open-questions.md` #39–#47), unchanged.

## 6. What stays unresolved

| Item | Why it matters | How it gets settled |
|---|---|---|
| The idle-to-sleep and light-to-deep timeouts | The soft sleep's policy | T-APPLE-SLEEPTIME; RE next step in `apple-power.md` §4.2 |
| Who first sets Timer 1's rate | Only for completeness; it never changes at run time | RE (a store through a data pointer, or the loader) |
| The feature behind the 58 MHz floor (`0x1081_EDB6`) and the two forced-ceiling states (`osos 0x100A_7784`, `0x100F_FFF8`, `0x1018_4904`) | A hint at which workloads Apple thought needed more clock | RE (follow the flag's writers through their base pointer) |
| Which modules raise the governor's boost step to 1,700 / 3,500 | The per-codec story | RE (the three owners at `osos 0x1012_7xxx`, `0x1018_Bxxx`, `0x101A_9xxx`) |
| What `MLCD_SCLK_DIV`'s bits do on the 5.5G | AW-7 | T-MLCD |
| Whether the USB-C mod board draws from `D2REGC1` | AW-3 | T-D2REG; **audio: no** [silicon, sitting 4gh, 2026-11-06] |
| Whether I2S and the codec clock survive gating | AW-5 | T-I2SGATE; **2 of 6 resumes late** [silicon, sitting 4gh, 2026-11-06] |
| What ORing `0x47A` into `XMB_RAM_CFG` changes | AW-8 | T-XMBCFG |
| Whether GPIOA bit 3 is a low-battery signal on this board | AW-17 | T-LOWBATT-PIN |
| Who sends low-battery message `0x94`, the battery sampling rate, the voltage threshold | Our own cut-off is already calibrated (#70); nice to know | RE |
| The read-ahead size per refill | Our refill fills the buffer; nice to know | RE (the track-cache task `osos 0x100E_1DBC`; `apple-disk.md` §3.3, `open-questions.md` #57) |
| The clients 3 and 5 of the `DEV_EXTCLOCKS` mask | AW-5's variant (a) | RE (`osos 0x1026_4EA8` callers) |
| `0x7000_0030` bits 15:12 = 6 | Possibly SDRAM timing | RE; `open-questions.md` #1's read-out |

## Sources

The osos 1.3 disassembly, with 1.2.1 and 1.2.3 for the comparisons (`sources.md`, "Apple's firmware"; the method is the one at the top of this chapter). Our side: `../design/p19-placement.md`, `../design/pp-governor.md`, `../design/core-layout-decision.md`, `../hardware.md`, and the files cited per row.
