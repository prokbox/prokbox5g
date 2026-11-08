# How Apple's firmware sleeps, idles and saves power (PP5022 side)

Desk study, 2026-11-05. A static reading of Apple's iPod Video OS image (`osos`, firmware 1.3, cross-checked against 1.2.1 and 1.2.3) for everything that powers the PP5022 and the board down: light sleep, deep sleep, power-off, the awake idle, and what Apple left running. Its sibling, [`apple-dual-core.md`](apple-dual-core.md), covers how Apple split work between the two cores and compares Apple's hardware use with ours. The BCM2722 side of Apple's power manager is already mapped in `../bcm2722-reference/brief-map-power.md` §2–§3; this chapter cross-references it and does not repeat it.

**Method and rules.** Static reading only: `arm-none-eabi-objdump` over `osos.code` (linked at 0, loaded at `0x1000_0000`; cited as `osos 0x1xxx_xxxx`, the same convention as `mac-and-dvs.md`), small scripts that index literal-pool constants, and Ghidra 12.2 headless for a few functions. Nothing executed Apple code; nothing touched a device. Only addresses, register values, constants and the order of accesses were recorded; every description is ours. The scratch work (disassembly, Ghidra project, scripts) lives outside the repository.

**IRAM addresses.** Apple runs part of its code from IRAM, copied there at start-up from the end of the image. Code cited as `IRAM 0x4000_xxxx` is that copy; the copy table is in §6.2. The resident IRAM block at `0x4000_0000` comes from `osos` file offset `0x71_619C`.

**Tags.** As in `index.md`, plus **[apple-RE]**: read by us from Apple's code in this study (the same evidence class as the manual's [apple]). It says what Apple's code *does*; the meaning attached to a register is tagged separately, usually [hypothesis]. [rockbox-derived] is the manual's [rockbox].

---

## 1. Summary

### How Apple did it

1. **Two sleep levels, and neither is a timer-driven doze.**
   - **Light sleep** keeps everything in memory. The CPU parks the second core, drops the codec and LCD regulators to a low-power mode, puts the USB PHY into suspend, stops both timers, switches the core clock to the crystal and turns the PLL off. It then **puts the SDRAM into self-refresh**, gates all device clocks except a handful (the clickwheel, USB and three unnamed always-on bits), and sleeps the CPU until any enabled interrupt arrives. The whole sequence runs from IRAM, because SDRAM is unavailable while it self-refreshes. Wake-up is the reverse: about 10.5 ms of fixed clock and SDRAM restore (two 250 µs PLL waits and a 10 ms settle), then the regulators, codec and display come back [apple-RE].
   - **Deep sleep is suspend-to-disk.** Apple writes a memory image into a dedicated `hibe` slot in the disk's firmware partition, then puts the PMU into standby (power-off). On the next power-up the boot ROM loads that image instead of a fresh OS, and the OS's very first start-up code notices a resume marker, restores IRAM and the saved device registers, and carries on where it stopped [apple-RE for the writer and the resume check; [hypothesis] for "the ROM loads `hibe` and sets the marker"]. This answers the P18 brief's puzzle: Apple's "instant" wake is the light sleep, and its long sleep survives a power-off because the RAM image is on disk, not in RAM.
2. **Power-off is a single PMU bit.** Apple keeps a shadow of the PMU's `OOCC1` register, read at boot, and only ever sets bit 0 (`GOSTDBY`, standby) or bit 1 (`TOTRST`, reset) on it. It never rewrites the wake-enable bits, so they stay as the boot ROM or the PMU's defaults left them [apple-RE].
3. **Regulators have a "sleep" mode, and Apple uses it.** The codec supply (`D1REGC1`, `0x24`) and the LCD supply (`D3REGC1`, `0x26`) are switched between mode `111` (on), `010` (light sleep) and `000` (off) with the same voltage code. The core converter is never touched on a PP5022 (as `mac-and-dvs.md` §2.4 found) [apple-RE].
4. **Pads are parked.** Before either sleep Apple rewrites the pad-control words `0x7000_0010`–`0x7000_001C` (`DEV_INIT1` and its neighbours) with per-board values and clears most of `DEV_INIT2`. Rockbox never writes these after boot [apple-RE].
5. **Awake, the idle loop sleeps the core.** Both cores share one idle routine. The CPU writes `PROC_SLEEP` to its own control register; the COP does the same in a loop while a "stay parked" flag is set. Each core's idle time is accumulated into a 64-bit counter for Apple's usage statistics [apple-RE].
6. **Disk use is arbitrated by named clients.** A table of about 50 "disk channels" (song buffering, preferences, album art, a "predictive cache spin-up", and so on) obtain and release the disk; the usage log records spin-ups, spin-downs, obtains and releases. There is no single spin-down timeout: each release carries its own linger (250 ms after a song-buffer read, 1 s for most UI reads), and the drive is cut when the last one expires [apple-RE] (`apple-power-catalog.md` §2.2).

### What we can learn

- **Self-refresh is the missing piece in our soft sleep.** P18's design leaves the SDRAM controller in auto-refresh with the clock at 24 MHz. Apple asks the controller for self-refresh (`XMB_RAM_CFG` `0x7000_003C` bit 22, acknowledged in bit 30) and only then gates clocks. That is what lets the core clock and the bus stop.
- **Light sleep needs no RAM image and no reboot.** Apple does not even save IRAM for it; only the deep path saves IRAM. So a ProkBox soft sleep that keeps IRAM and SDRAM is the same shape as Apple's.
- **The long sleep is a disk image, not a held RAM.** If we ever want "a day later, still no boot", Apple's answer is suspend-to-disk through the ROM's `hibe` slot. That needs our bootloader to cooperate and a writer that runs with the disk at full speed; it is a #86 "later" item.
- **The regulator sleep mode (`010`) and the pad parking are cheap, measurable wins** for both our dark playback (LCD LDO) and our soft sleep. They need a silicon check before use.
- **Apple kept a second wake path for USB.** With a cable in, Apple does not `PROC_SLEEP` at all; it spins slowly, watching a USB status bit and the interrupt status, so a host can't be lost to a sleeping core. Our soft sleep should copy that rule.

---

## 2. The light sleep

### 2.1 Where it is

| Piece | Address | Notes |
|---|---|---|
| Sleep manager (light or deep) | `osos 0x100B_43DC` | Two arguments: "light allowed" and "force deep". A configuration byte at `0x1081_DFE7`, set from a file under the device folder at start-up, also forces the deep path [apple-RE]; it reads like a test override [hypothesis] |
| Its caller | `osos 0x101D_FDD4` | A method (vtable slot at `osos 0x1067_0790`) that asks its object whether light sleep is allowed, logs, and calls the manager [apple-RE] |
| Light-sleep core (IRAM) | `IRAM 0x4000_045C` (file `0x71_65F8`), reached through `osos 0x1008_43CC` | The register sequence of §2.3 [apple-RE] |
| Deep-sleep entry (IRAM) | `IRAM 0x4000_03C8`, reached through `osos 0x1008_43C4` | COP park, IRAM save, image write, standby (§3) [apple-RE] |
| Present in 1.2.1 and 1.2.3 | the light-sleep literal pool (`0xC001_0108`, `0x4100_000A`, `0x4200_00FA`, `0x4800_000F`, `0x4800_0013`) sits in the same IRAM block in all three images (1.2.1 `osos 0x1070_8238`; 1.2.3 `0x1071_400C`; 1.3 `0x1071_6810`), and the "Enter/Exit Light/Deep Sleep" strings are in all three | [apple-RE] |

### 2.2 The entry sequence (what the manager does before the IRAM core)

In order, from `osos 0x100B_43DC` and its callees. "Light" and "deep" share the first part.

| Step | What | Where | Tag |
|---|---|---|---|
| 1 | Stop the COP's work engine (the message-driven engine of `apple-dual-core.md` §2) | `osos 0x1017_7E68` → `0x1024_AD20` | [apple-RE] |
| 2 | Log "enter light sleep" (event 5) or "enter deep sleep" (event 8) to the usage log; deep only: four extra save steps (`0x1008_4F48`, `0x101A_0120`, `0x1019_FF0C`, `0x1010_5080`) | [apple-RE]; what the four save is [unknown] (database and settings flushes [hypothesis]) |
| 3 | Display/bridge power mode 1 (a GPIO/peripheral handshake that waits on a busy bit with a timeout) | `osos 0x1026_6B18` → `0x100B_2CE0` | [apple-RE]; meaning [hypothesis: the LCD bridge or BCM link to its idle state] |
| 4 | Several kernel calls: take a lock, quiesce subsystems, raise the calling task's priority (kernel service `0x1B`, to level 2) | `0x100A_6540`, `0x1018_482C`, `0x101A_FD74`, `0x1009_4E08`, `0x100A_6198` | [apple-RE] |
| 5 | An I2C-bus lock; then, on boards that have the feature, **clear `PLL_CONTROL` bit 29 and `DEV_EN` bits 27 and 19**, saving them for the wake | `osos 0x1026_6B4C` (restore `0x1026_6BDC`) | [apple-RE]; what bits 27/19 and PLL bit 29 clock [unknown] |
| 6 | A GPIO pin (Apple's pin 32) driven to its sleep level, and a peripheral told to sleep | `osos 0x1026_6BA4` | [apple-RE] |
| 7 | **Stop both timers**: save and zero `TIMER1_CFG` (`0x6000_5000`) and `TIMER2_CFG` (`0x6000_5008`) | `osos 0x1000_07F8` (restore `0x1000_0668`) | [apple-RE] |
| 8 | Wait 10 ms (busy-wait on `USEC_TIMER` `0x6000_5010`) | `osos 0x1028_28B8` | [apple-RE] |
| 9 | Save the interrupt-controller state and the active driver set (a kernel object method) | `osos 0x1028_1F14` and the indirect call at `0x100B_44F8` | [apple-RE] |
| 10 | **Pad parking**, state 2 (light) or 3 (deep): `0x7000_0010` = `0x0004_0000` (or `0xFC04_0000` / `0xA804_0000` on some board types), `0x7000_0014` = `0x0A00` (or `0x0A0F`), `0x7000_0018` = `0x5_0200` (PP5022; `0x5_0208` on one board variant), `0x7000_001C` = 0, `DEV_INIT2 &= 0xE000_0000`. Then backlight off and the LCD panel off through the panel driver | `osos 0x1028_23BC` | [apple-RE] for the writes; that these are pad pull/keeper/drive controls [hypothesis] |
| 11 | Wait 10 ms | `0x1028_28B8` | [apple-RE] |
| 12 | **Regulators to the light-sleep mode**: codec LDO (`0x24`) and LCD LDO (`0x26`, code 21) to mode `010`; on parts other than the PP5022 also `DCDC1` | `osos 0x1028_262C` (state 3) | [apple-RE] |
| 13 | **USB PHY to suspend** if a cable is present and the port reports the right line state: pulse `0x7000_0028` bit 16, wait for bit 7 to clear, set bits 8–9 | `osos 0x1026_66F4` (undo `0x1026_6840`) | [apple-RE]; "suspend" is [hypothesis] (Rockbox's PHY bring-up waits for bit 7 to *set*) |
| 14 | Enter the IRAM core (§2.3) | `osos 0x1008_43CC` | [apple-RE] |

The PMU writes in steps 10 and 12 go through Apple's I2C PMU writer (`osos 0x1028_3634`: device `0x08`, at most 3 data bytes per transaction, which matches quirk 22).

### 2.3 The IRAM core: clocks, SDRAM and the CPU (step table)

`IRAM 0x4000_045C`, in order. Registers by Rockbox's names; `CPU_CTL` = `0x6000_7000`, `COP_CTL` = `0x6000_7004`.

| # | Action | Register and value | Tag |
|---|---|---|---|
| 1 | Decide the wait style: if a USB cable is present (Apple's pin 0x23/0x64 read) **and** `0x7000_0028` bit 2 is set, remember "USB mode" | `0x7000_0028` bit 2 | [apple-RE]; bit 2 = VBUS/session valid [hypothesis] |
| 2 | IRQ and FIQ off | CPSR | [apple-RE] |
| 3 | Save `DEV_INIT2`, `DEV_EN`, `DEV_EN2`, `0x6000_6044`, `PLL_CONTROL`, `CLOCK_SOURCE` | | [apple-RE] |
| 4 | Core clock to the 24 MHz crystal on selector 1: `CLOCK_SOURCE` low nibble = 2, then top nibble = 1 | `0x6000_6020` | [apple-RE] |
| 5 | Short self-timed stall: `CPU_CTL` = `0x4800_000F` (count mode, clock-cycle units, 15) | | [apple-RE] |
| 6 | PLL off: `PLL_CONTROL` = `0x0A00_0000` | `0x6000_6034` | [apple-RE] |
| 7 | Hold the COP: save `COP_CTL` if it reads `0x4000_0000`, then write `0x4000_0000` | | [apple-RE]; the COP is already parked in its sleep loop (§3.1); what count mode with a zero count does to it [hypothesis: holds it stopped] |
| 8 | `CPU_CTL` = `0x4200_0000` (count mode, µs units, 0), then four `nop`s | | [apple-RE] |
| 9 | **SDRAM self-refresh request**: `XMB_RAM_CFG` (`0x7000_003C`) bit 22 set; **wait for bit 30 to set** | `0x7000_003C` | [apple-RE] for the access; "self-refresh request / in self-refresh" is [hypothesis] (strong: it brackets the clock gating and is undone before the PLL returns) |
| 10 | `CPU_CTL` = `0x4100_000A`: the CPU stops itself for 10 ms (count mode, ms units) | | [apple-RE] |
| 11 | `DEV_INIT2` bit 6 set | `0x7000_0020` | [apple-RE]; meaning [unknown] |
| 12 | `0x6000_6044` = `0x3F` | Rockbox's `CACHE_PRIORITY` / "DEV_EN3" | [apple-RE]; meaning [unknown] |
| 13 | **Device clocks to the minimum**: `DEV_EN` = `0xC001_0108`, `DEV_EN2` = 0 | `0x6000_600C`, `0x6000_6010` | [apple-RE]. Kept: bits 31, 30 (unnamed, also on in Rockbox's `0xC200_0124`), 16 (`DEV_OPTO`, the clickwheel), 8 (unnamed), 3 (`DEV_USB0`). Dropped among others: 2 (`DEV_SYSTEM`), 25 (`DEV_IDE0`), 11 (`DEV_I2S`), 12 (`DEV_I2C`) |
| 14 | `CLOCK_SOURCE` top nibble cleared (no selector active) | `0x6000_6020` | [apple-RE]; what the core runs from then (the 32 kHz source, or a stopped clock) is [hypothesis] |
| 15 | `CPU_CTL` = `0x4800_000F`; `PLL_CONTROL` = `0x0200_0000` | | [apple-RE] |
| 16a | **Normal:** `CPU_CTL` = `0x8000_0000` (`PROC_SLEEP`): the CPU sleeps until an enabled interrupt is pending | | [apple-RE] |
| 16b | **USB mode:** no sleep; loop while `0x7000_0028` bit 2 is set and `CPU_INT_STAT` (`0x6000_4000`) reads 0 | | [apple-RE] |
| 17 | Wake: `PLL_CONTROL` = `0x0A00_0000`; `CPU_CTL` = `0x4800_0013` (19 cycles); `CLOCK_SOURCE` selector 1 again (crystal); `CPU_CTL` = `0x4800_000F` | | [apple-RE] |
| 18 | Restore `DEV_EN`, `DEV_EN2`, `0x6000_6044`; `DEV_INIT2` bit 6 cleared | | [apple-RE] |
| 19 | `XMB_RAM_CFG` bit 22 cleared (leave self-refresh) | | [apple-RE] |
| 20 | Restore `PLL_CONTROL` (the PLL relocks); `CPU_CTL` = `0x4200_00FA` twice (**2 × 250 µs**) | | [apple-RE] |
| 21 | Wait for `XMB_RAM_CFG` bit 30 to clear | | [apple-RE] |
| 22 | `CPU_CTL` = `0x4100_000A` (**10 ms**), then restore `CLOCK_SOURCE` (back to the PLL), `CPU_CTL` = `0x4800_000F`, `COP_CTL`, `DEV_INIT2`; restore CPSR | | [apple-RE] |

**What this says about the hardware** (folded into `clocks-power.md` §10 and `quirks.md`):
- `CPU_CTL`/`COP_CTL` count mode is usable as a self-timed stall from IRAM: `0x48…` clock cycles, `0x42…` µs, `0x41…` ms (the units match Rockbox's `PROC_CNT_*` names; Apple uses them, Rockbox never does) [apple-RE].
- `XMB_RAM_CFG` has a request/acknowledge pair for a low-power SDRAM state (bits 22/30) [apple-RE; meaning hypothesis].
- With the PLL off, Apple runs the core from the crystal before it gates anything, as Rockbox does for 24 MHz [apple-RE, rockbox-derived].

### 2.4 The wake path after the IRAM core

From `osos 0x100B_4534` onward: undo the USB PHY suspend (`0x1028_2774`), a kernel restore (`0x1027_EA44`), the bridge resume (`0x1026_6840`), the I2C lock released, **regulators back to mode `111`** (`0x1028_262C(1)`), the codec driver to mode 4 then 3 (`0x1026_5E54`; §5.3), the display and backlight paths, the device-clock bits of step 5 restored (`0x1026_6BDC`), the timers restarted (`0x1000_0668`), the task priority restored, and the drivers notified. If the sleep was a deep one that came back through a power cycle, the same path continues with a few extra restores (`0x1025_FBE8`, `0x1009_0E84`) [apple-RE].

**Timing (PP side):** the IRAM core's fixed waits are 10 ms after the self-refresh acknowledge on entry, and 2 × 250 µs for the PLL plus a 10 ms stall before the PLL is selected on the way out, so **about 10.5 ms from the waking interrupt to running on the PLL** [apple-RE, from the count words; not timed]. The manager adds two 10 ms waits on entry (§2.2). The rest of the wake is regulator ramp, the codec and the panel. The panel's own power-on dominates when the LCD rail was in mode `010` [hypothesis; the BCM side's numbers are in `../bcm2722-reference/brief-map-power.md` §3 "Leverage"].

### 2.5 Wake sources

| Source | How it reaches the sleeping CPU | Evidence |
|---|---|---|
| Clickwheel and its buttons | `DEV_OPTO` stays clocked through the sleep; the wheel controller raises its interrupt (I2C high source, quirk 19) | [apple-RE] for the kept clock; the interrupt path [rockbox-derived] |
| USB cable / host | `DEV_USB0` stays clocked; with a cable present Apple does not sleep the core at all (§2.3 16b), and leaves when the cable goes or any interrupt is pending | [apple-RE] |
| Hold switch, headphone/dock detect, charger | GPIO interrupts; the GPIO block is not in `DEV_EN`, so it stays live [hypothesis]. Apple has a task per source (`HoldSwitchTask`, `HPhoneDetTask`, `AccessoryDetectTask`, `USBPowerSense`, `TopPlugTask`) | [apple-RE] task names; routing [hypothesis] |
| PMU events (on-key, charger, alarm, battery) | The PMU's interrupt line into a GPIO. Apple's PMU task programs `INT1M`/`INT2M`/`INT3M` (`0x05`–`0x07`) = `0x50`, `0xFC`, `0x0F` | [apple-RE] values (`osos 0x1028_5018`); which bits each unmasks [hypothesis] |
| Timers | **None.** Both timers are stopped (§2.2 step 7), so nothing periodic wakes a light sleep | [apple-RE] |
| RTC alarm | Apple's RTC writer sets the alarm registers `0x11`–`0x16` after first writing **`0xAA`** to the alarm year (`0x17`) as "disarmed while updating"; it is called only from the clock/alarm code, not from the sleep manager | [apple-RE] (`osos 0x1028_37F8`, caller `0x100B_C63C`) |

So a light sleep lasts until the user, a cable, or a PMU event wakes it. How Apple turns a long light sleep into a deep one is §4.2.

---

## 3. The deep sleep (suspend-to-disk)

### 3.1 Entry (`IRAM 0x4000_03C8`)

| # | Action | Where | Tag |
|---|---|---|---|
| 1 | IRQ/FIQ off; set the park flag (byte at uncached `0x1493_713B`) and wake the COP (`COP_CTL` = 0) | | [apple-RE] |
| 2 | Wait for the COP's acknowledge byte (`0x1493_713C`). The COP, in its idle routine, sees the flag, saves its own state, sets the acknowledge and loops on `COP_CTL` = `PROC_SLEEP` while the flag stays set | CPU side here; COP side `osos 0x1011_10E8` and the idle routine `0x100D_ED04` | [apple-RE] |
| 3 | **Save IRAM to SDRAM**: copy IRAM from `0x4000_0000` (length from `osos 0x1026_54E8`) to `0x1084_BEDC` | `osos 0x1000_0574` | [apple-RE] |
| 4 | **Save the device registers** into a table at `0x1084_BE94`: `CPU_QUEUE` (`0x6000_1010`, saved but not restored), the four `0x6000_3000` words, `CPU_INT_EN_STAT`, `CPU_INT_PRIORITY`, `CPU_HI_INT_EN_STAT`, `CPU_HI_INT_PRIORITY`, `DEV_EN`, `DEV_EN2`, `0x6000_6044`, `CLOCK_SOURCE`, `PLL_CONTROL`, `0x6000_6048`, `0x6000_60C4`, `CACHE_CTL`, `DEV_INIT2` | `osos 0x1000_0714` | [apple-RE] |
| 5 | Record the stack and context pointers for the resume (`0x1493_7328`, `0x1493_732C`) | | [apple-RE] |
| 6 | **Write the image**: load the hibernation overlay into IRAM (`0x4000_1808`, 10.6 KB), pass 80,000 (kHz) to an overlay routine (the clock setter [hypothesis]), configure the IDE timing for it, power the disk on and wait 100 ms, **flush and disable the cache**, find the `!ATA`/`hibe` entry in the firmware partition's directory, check that the slot is exactly the image size plus 2 KB, and write the image and then the directory | `osos 0x1000_0388` | [apple-RE] |
| 7 | Disk off, then **PMU standby** | `osos 0x1000_07DC` → `0x1026_5CC8(0)`, `0x1026_66E0` → `0x100B_34F4` | [apple-RE] |

If any step of the write fails, Apple goes to standby anyway (the error branches call the same power-off) [apple-RE]. So a failed hibernation becomes a cold boot, not a hang.

### 3.2 Resume

- **Both cores' first code** (`osos 0x1000_0240`) checks a word in the ROM's IRAM mailbox: the last 256 bytes of IRAM, here `0x4001_FF10` (the same page as Apple's disk-mode request, `boot.md` §3). **Value 8 means "resuming"** [apple-RE].
- **CPU** (`osos 0x1000_1100`): if 8, it does a little re-initialisation, wakes the COP and waits until the COP clears the word; then it **copies the saved IRAM back** and **rewrites the saved registers** (`osos 0x1000_0830` → `0x1000_05D8`: the `0x6000_3000` table, the interrupt enables and priorities, `DEV_EN`/`DEV_EN2`, `0x6000_6044`, `0x6000_6048`, `0x6000_60C4`, `DEV_INIT2`) and returns into the sleep manager's wake path (§2.4) [apple-RE].
- **COP** (`osos 0x1000_10C0`): if 8, it restores its own context, clears the word to 0 and sleeps itself until the CPU wakes it [apple-RE].
- The resume check runs **before** the start-up code copies `.data` and clears `.bss` (`osos 0x1008_3848`), so the image's memory is untouched [apple-RE].
- **Who writes the 8, and who loads `hibe`:** nothing in `osos` writes 8 there, and `hibe` is one of the image types in the partition directory (`boot.md` §2). So the boot ROM most likely loads the `hibe` image when it is valid and sets the marker [hypothesis, strong]. The same page holds an 8-byte state string Apple writes at boot (`booting!`, `hotstuff`, `battislo`, `--------`; `osos 0x1000_0FB0`) [apple-RE].

**What survives:** all of SDRAM that the image covers (its extent comes from a memory-manager descriptor at run time, `osos 0x1026_5598`), IRAM, and the listed registers. What is re-initialised: everything else, including the PLL setting (from the saved `CLOCK_SOURCE`/`PLL_CONTROL` through the normal clock driver) and the drivers' hardware state through their wake hooks [apple-RE].

**Cost:** a disk write of the image at entry (with the disk at full speed, the core at 80 MHz) and a ROM load at wake. Neither was measured; on a hard disk it is seconds [estimate].

### 3.3 Power-off, reset and the PMU

| Action | Apple's write | Where | Tag |
|---|---|---|---|
| Standby (power-off) | `OOCC1` (`0x08`) shadow bit 0 set and written; bits 0–1 then cleared in the shadow (they self-clear); the CPU then spins | `osos 0x1028_375C` (bit writer), `0x100B_34F4` | [apple-RE] |
| Reset | the same writer with bit 1 (`TOTRST`) | `osos 0x100B_0BFC` | [apple-RE] |
| Wake enables (`CHGWAK`, `EXTONWAK`, `RTCWAK`) | **never written**; the shadow is read from the PMU, so they stay as found | only two callers of the bit writer exist | [apple-RE] |
| Regulator power states | state 1 = on (`111`), 3 = light sleep (`010`), 2 = off (`000`) for the codec and LCD LDOs | `osos 0x1028_262C` | [apple-RE] |
| PMU type | ID register `0x00` read once; `0x24` → one register map, anything else → ours (LDO index 0–4 = `0x23`–`0x27`) | `osos 0x1028_3704`, `0x1028_3360` | [apple-RE] (confirms `mac-and-dvs.md` §2.1) |
| Low battery | message `0x94` in the alarm task's queue (`AlarmTask`, `osos 0x1028_4374`; corrected 2026-11-05, it is not `PCFPowerMgr`) posts a low-battery event to the UI, or, with a configuration flag (`0x1081_DFE8`), goes straight to standby after about 1 s. A low-battery GPIO (GPIOA bit 3, boards 10–12) has its own 20 ms debouncer; with no UI registered it leaves `battislo` in the IRAM mailbox and powers off (`osos 0x1028_1B28`) | `osos 0x1028_4374`, `0x1028_44C4`, `0x1022_EC90`, `0x1000_0FB0` | [apple-RE]; who sends `0x94` and the voltage threshold are [unknown] (`apple-power-catalog.md` §2.9) |

**Correction to `mac-and-dvs.md` §2.4:** Apple's code 21 (3.0 V) goes to LDO index 3, which on our PMU type is `D3REGC1` (`0x26`, the LCD supply in Rockbox's naming), not `D2REGC1` (`0x25`). On the other PMU type, index 3 is `0x28` [apple-RE].

---

## 4. Policy: when Apple sleeps, and for how long

### 4.1 What was found

| Item | Value | Tag |
|---|---|---|
| Who decides | a method on a UI/power object asks "is light sleep allowed?" (vtable slot `+0xF0`) and passes "deep" when it is not or when forced; the light/deep log events are 5 and 8 | [apple-RE] |
| Debug override | a byte set from a file in the device folder forces deep sleep; another forces an immediate standby on the low-battery event | [apple-RE] |
| User sleep timer | in minutes, armed in pieces of at most 30 minutes (`osos 0x101D_85E8`, constant 1,800,000 ms) | [apple-RE] |
| Usage statistics | the log keeps total time and count for light sleep, deep sleep, disk on, backlight on, playback and "DMIA" | [apple-RE] strings |
| The PP's own seconds counter | `0x6000_5014` holds a day/hour/minute/second count in four 6-bit fields; Apple converts it to seconds (`osos 0x1028_2810`) | [apple-RE]; that it keeps counting in sleep [hypothesis] |

### 4.2 What was not found (open), and what the catalogue settled

- **The idle-to-sleep timeout** (how long paused or idle before light sleep) and **the light-to-deep timeout**: still open. The timers are stopped in light sleep, so the light-to-deep step must ride a PMU event (the RTC alarm or the PMU's periodic interrupt) or be taken at the next wake [hypothesis]. Next step for the RE: follow the callers of the vtable slot at `osos 0x1067_0790` and the queue messages around `osos 0x100C_D314`.
- **Settled: the disk spin-down timeout.** There is no global one. Each of 45 named disk channels releases the disk with its own linger time, and the drive is cut when the last one expires: 250 ms after a song-buffer read, 1 s for most UI reads, 5 s after preference writes [apple-RE] (`apple-power-catalog.md` §2.2). The read-ahead size is still open.
- **Settled: which clock Apple asks for.** A 10 ms load-following governor picks 26–80 MHz from both cores' idle time, plus timed 80 MHz requests of 1–30 s after events, and an 80 MHz pin in USB disk mode [apple-RE] (`apple-power-catalog.md` §2.1). Per-codec levels are not fixed numbers: three modules (most likely codec engines [hypothesis]) raise the governor's boost step while they run.

---

## 5. Awake power saving

### 5.1 The idle loop (both cores)

`osos 0x100D_ED04`, shared:
- **CPU:** add one to a 64-bit idle count, write `PROC_SLEEP` to `CPU_CTL` (`osos 0x100A_058C`); the next interrupt wakes it [apple-RE].
- **COP:** write `PROC_SLEEP` to `COP_CTL` once, and again for as long as the audio-stop hold `0x1493_7139` is set (it acknowledges the hold in `0x1493_713A`), counting each pass; or, when the park flag `0x1493_713B` is set, save its context, acknowledge and stay asleep (§3.1), then re-initialise its interrupt controller when released [apple-RE]. The first edition called `0x1493_7139` the COP's "sleep" byte; it is the hold that `osos 0x1018_A900` sets before it gates `DEV_I2S` to stop the audio engine, not the park flag (corrected 2026-11-06, `apple-dual-core.md` §5).
- Both cores add the time spent idle (from the microsecond counter, noted on entry with IRQ and FIQ masked) to a per-core 64-bit total at `0x1498_3040`, on the way out only (`osos 0x100D_EE3C`–`EE5C`); the clock governor reads these [apple-RE].
- The CPU wakes the COP by clearing `COP_CTL` bit 31 only if the COP is asleep (`osos 0x100C_FB08`, five callers) [apple-RE].

Apple's idle is therefore the same as Rockbox's `core_sleep()`: no clock change on the idle path itself [apple-RE]. The COP is woken by work, and also by the clock governor's pass every 10 ms while the audio engine runs, which closes its idle window (`apple-dual-core.md` §5; corrected 2026-11-06) [apple-RE].

### 5.2 Clocks

Covered by `mac-and-dvs.md` §3: a per-request driver up to 125 MHz with a 90 ns `DEV_TIMING1` rule and no core-voltage change. The hibernation writer passes 80 (MHz) to a second, SDRAM-resident PLL setter (`osos 0x1000_067C`) before it writes [apple-RE].

**Who sets the clock** (settled by `apple-power-catalog.md` §2.1) [apple-RE]:
- **The governor.** A governor task (`osos 0x1028_5194` → `0x100C_164C`) runs every 10 ms. It sets 26–80 MHz from both cores' idle totals, a decaying event boost, and timed requests (`osos 0x100C_15CC`) that hold 80 MHz for 1–30 s.
- **The disk-mode pin.** USB disk mode pins 80 MHz and suspends the governor.
- **No crystal while awake.** The awake floor is **26 MHz on the PLL**; Apple never idles awake on the 24 MHz crystal.
- **The COP pulse.** Each governor pass, after it has read the idle totals, wakes the COP if it is asleep (`osos 0x100C_FB08`), but only while the audio engine runs (the hold byte `0x1493_7139` clear). So Apple's COP sees a ~100 Hz pulse during playback, dark included, as Rockbox's tick gives ours (`apple-dual-core.md` §5).
- **Unchanged across versions.** The parameters are identical in 1.2.1, 1.2.3 and 1.3.

### 5.3 Audio (the WM8758)

- Apple's codec layer has a numbered power mode (`osos 0x1026_5E54`) with three codec back ends, picked by a board query (`osos 0x1026_53A4`) [apple-RE].
- It is called from only five places: the sleep path (modes 4 then 3 at wake), the pad-parking routine (mode 1), and two small functions (`osos 0x1009_5F0C` sets mode 1 after clearing a bit in a status word; `osos 0x1009_9098` sets mode 3 and then calls `0x1028_40FC(0)`) [apple-RE].
- What each mode is (on, standby, off) and whether a pause reaches mode 3 is open: the two small functions' callers decide it [unknown]. Our DAC standby after a pause (P15) is already measured (`clocks-power.md` §7).

### 5.4 Backlight and panel

- The backlight is driven as a 0–100 % brightness scaled into a hardware range (`osos 0x1028_3230`) and switched on/off through one routine (`osos 0x1028_31B4`) with three hardware variants [apple-RE].
- The panel on/off dispatches on one of five panel types (`osos 0x1028_3284`) [apple-RE].
- A `BacklightTask` exists; the backlight timer is a user setting [apple-RE strings].
- **Settled (`apple-power-catalog.md` §2.3):**
  - Off is two-stage: the level goes to 0 at once, and the backlight supply lines drop 1 s later.
  - The panel on/off routine is called only on the way into sleep, and the LCD LDO changes mode only in sleep.
  - So **dark but awake, Apple turns off only the backlight**. ProkBox goes further (the LCD and BCM sleep 10 s after the backlight goes off) [apple-RE; code].

### 5.5 Driver tasks Apple keeps running

`osos` creates a task per hardware source (strings at file `0x25_D63C`–`0x25_D8A0`): `FirewireTask`, `OptoTask`, `SerialOptoTask`, `BacklightTask`, `CNATask`, `USBPowerSense`, `DiskMgrTask`, `HoldSwitchTask`, `TopPlugTask`, `HPhoneDetTask`, `LowBattDebounceTask`, `AccessoryDetectTask`, `RTCTimerMgr`, `AlarmTask`, `WatchdogTask`, `PCFPowerMgr`, `AsyncPiezo`, three accessory managers and `USBAudioTask` [apple-RE]. Most are event-driven (they block on a queue or an edge interrupt) [apple-RE for the input tasks, `apple-power-catalog.md` §2.9]. **Correction (2026-11-05):** `osos 0x1028_4374` is `AlarmTask` (piezo alarm beeps and the low-battery message), not `PCFPowerMgr` [hypothesis: by its place in the task descriptor table]. `PCFPowerMgr` is `osos 0x1028_4FF4`: it writes the interrupt masks once and then blocks on the PMU's interrupt, acting only on the alarm and one other bit; it never polls [apple-RE].

---

## 6. Paused and screen-off: what is still running

| Item | Apple | Tag |
|---|---|---|
| Timers | Timer 1 and Timer 2 run while awake; both stop in light sleep | [apple-RE] |
| COP | asleep in its idle loop unless its engine has work; parked by handshake for deep sleep | [apple-RE] |
| Display | only the backlight goes off (level 0, then its supply lines 1 s later); the panel and the LCD LDO stay on until light sleep; the BCM's own power manager runs its `min` load governor (`brief-map-power.md` §2) | [apple-RE] (`apple-power-catalog.md` §2.3) |
| Disk | released by its clients, each with its own linger; cut when the last expires | [apple-RE] (`apple-power-catalog.md` §2.2) |
| How often it wakes while paused and dark | the tick (Timer 1, never slowed), the 10 ms clock-governor pass, which also wakes the COP, and nothing else periodic: the PMU, Hold, headphone and VBUS tasks block on interrupts. Then nothing periodic once light sleep starts | [apple-RE] (`apple-power-catalog.md` §2.1, §2.6, §2.9); Timer 1's rate itself [unknown] |

### 6.1 The IRAM map Apple uses

Copied at start-up by `osos 0x1008_3848` and `0x1008_392C` (source = file offset, destination = IRAM):

| IRAM | Size | Source (file) | Use |
|---|---|---|---|
| `0x4000_0000`–`0x4000_0FB8` (+`0x59C` zeroed) | 4.0 KB | `0x71_619C` | resident: vectors' targets, thunks back to SDRAM, a polyphase filter, **the clock driver, the light-sleep core and the deep-sleep entry** |
| `0x4000_1808`–… | up to ~11 KB | several (hibernation writer `0x71_715C`, 10.6 KB; others `0x71_9B00`, `0x72_960C`, `0x72_B704`) | **an overlay window**: different modules are copied in on demand |
| `0x4000_8000`/`0x4000_8008`–`0x4000_E014` | ~24 KB | `0x71_A80C`, `0x72_35F8`, `0x72_DC04` | overlay window 2 (codec hot loops [hypothesis]; `mac-and-dvs.md` §1.2 found the multiply-dense code here) |
| `0x4001_0000`–`0x4001_7694` | 14.6 KB + 15 KB zeroed | `0x71_EE84` | resident data/code [hypothesis] |
| `0x4001_8000`/`0x4001_8800`–`0x4001_9468` | ~3 KB | `0x72_292C`, `0x73_1ED4` | overlay window 3, swapped by the COP engine per job |
| `0x4001_C8FC`–`0x4001_DED4` | 5.5 KB | `0x73_26C8` | the COP engine's core (queues, semaphores) |
| `0x4001_FF00`–`0x4001_FFFF` | 256 B | – | the ROM's mailbox (§3.2, `boot.md` §3) |

Apple therefore uses IRAM for three things: code that must run while SDRAM is unavailable (clock switches, light sleep, hibernation), the COP's engine, and swapped hot loops. Our rule "IRAM is for codecs only" would need a documented exception for a self-refresh sleep (`../phase18-brief.md` already grants one for its 32 kHz spike).

---

## 7. What to adopt (ranked)

| # | Idea | Effort | Where it fits | Needs first |
|---|---|---|---|---|
| A1 | **SDRAM self-refresh in the soft sleep**: request bit 22, wait for bit 30, then gate clocks; reverse at wake. The routine and its stack in IRAM | M (IRAM routine, ~0.5 KB; a test row) | #86 sleep (18-1) | S-1, S-2 (§8) |
| A2 | **Minimum `DEV_EN` in sleep** = `0xC001_0108`, `DEV_EN2` = 0, restored at wake | S | #86 sleep; partly now for dark play (gate unused blocks) | S-3 |
| A3 | **No sleep with a USB cable in**: spin at the crystal clock on the USB status bit instead of `PROC_SLEEP`, as Apple does | S | #86 sleep | none (a rule) |
| A4 | **LDO sleep mode `010`** for the LCD supply (`0x26`) while dark and asleep; the codec supply (`0x24`) in sleep only | S (two PMU writes) | **now** for the LCD rail in dark playback, if S-4 shows the panel holds or restarts cleanly; #86 for both | S-4 |
| A5 | **Pad parking** (`0x7000_0010`–`0x7000_001C`, `DEV_INIT2`) with Apple's PP5022 values in sleep | S | #86 sleep | S-5 (current A/B) |
| A6 | **Stop both timers in deep idle**, waking only on events; P18's 60 s poll then becomes an RTC alarm or PMU tick | M | #86 sleep | S-6 |
| A7 | **USB PHY suspend** (`0x7000_0028` bit 16 pulse, bit 7 clear, bits 8–9) when the host has suspended the bus | S | now (USB idle on a charger) and #86 | S-7 |
| A8 | **The COP parked by flag-and-acknowledge** before any clock or SDRAM change (already our design, `clocks-power.md` §9) | done | – | – |
| A9 | **Self-timed core stalls** (`CPU_CTL` count mode in µs/ms) instead of busy loops in IRAM-only code that can't read the timer safely | S | #86 sleep; clock switches | S-8 |
| A10 | **Suspend-to-disk through the ROM's `hibe` slot** for "days asleep, no boot" | L (bootloader cooperation, an image writer, the ROM's format) | later, owner's go | S-9; the bootloader guardrails |
| A11 | **Usage statistics like Apple's** (time in light/deep sleep, disk-on, backlight-on) in our power log | S | now | none |

**Update (2026-11-05):** the awake-time rows this list lacked are in [`apple-power-catalog.md`](apple-power-catalog.md) §3 (AW-1 to AW-17), ranked by gain per effort. Its §4 re-ranks A1–A10 as SL-1 to SL-10 and adds the codec full-off in sleep (SL-3) and Apple's PMU interrupt masks (SL-6). A4's "now" half (the LCD rail in dark playback) is AW-4: a **beat** item, since Apple never drops that rail while awake.

## 8. Open questions for silicon (`open-questions.md` #39–#49; S-n = #(38+n))

The awake-time questions this study added (`open-questions.md` #50–#55) carry their own bench specs in `apple-power-catalog.md` §5: T-LINGER, T-USBPROBE, T-I2SGATE, T-D2REG, T-LCDLDO (= S-4), T-MLCD, T-XMBCFG, T-LOWBATT-PIN, T-CODECOFF and T-APPLE-SLEEPTIME.

| ID | Question | Check |
|---|---|---|
| S-1 | Does setting `XMB_RAM_CFG` bit 22 put the SDRAM into self-refresh, and does bit 30 acknowledge it? | From IRAM, with interrupts off: set bit 22, poll bit 30 with a bound, wait 1 s, clear, poll bit 30 clear; check a 1 MB CRC region (P18's D1) |
| S-2 | Does SDRAM hold for 1 min / 1 h in that state with `DEV_EN` minimal and the PLL off? | D1 with the A1 routine |
| S-3 | Do the clickwheel and the USB wake a `PROC_SLEEP` core with `DEV_EN` = `0xC001_0108`? Which interrupt sources stay live? | D2 rows with the A2 set |
| S-4 | What does LDO mode `010` do to the LCD panel and the codec (held, reset, off)? | Write `0x26` = `0x55` (code 21, mode 010) with the screen dark; wake and look for a clean panel; then the codec with the DAC in standby |
| S-5 | Do Apple's pad values change the sleep or dark-play current? | Voltage-slope A/B (no current meter) |
| S-6 | Does `0x6000_5014` keep counting with the timers stopped and the clock gated? | Read it before and after a light sleep against the PMU RTC |
| S-7 | Does the PHY suspend sequence lower the charging-idle draw, and does the host still see the device? | A/B on a charger with the bus suspended |
| S-8 | Do `0x42…`/`0x41…` count-mode words stall the CPU for the stated µs/ms on our unit? | Time them against `USEC_TIMER` |
| S-9 | Does the ROM load a `hibe` image and set `0x4001_FF10` = 8? | Read-only first: dump the partition directory on a stock unit's backup and look for a `hibe` entry and its size |

## 9. Meet or beat

Columns: the area; Apple's approach and its visible cost; ProkBox's approach today; our verdict; the next step to get or stay ahead, with an effort estimate. "Measure" marks where only a measurement can settle the verdict.

| Area | Apple | ProkBox today | Verdict | Next step (effort) |
|---|---|---|---|---|
| **Idle power** (paused, dark) | Light sleep: SDRAM self-refresh, PLL off, clocks gated, timers stopped, LDOs in `010`, pads parked, CPU in `PROC_SLEEP`. Cost: ~10–20 ms of entry waits; no periodic wake | No sleep yet: hold PLAY powers off; paused-dark runs the 100 Hz tick, 24 MHz with the PLL off, drive cut, DAC standby (`p19-placement.md` §2.1) | **behind** (and better than a full boot only because we power off) | Build P18's soft sleep with A1–A6 (M–L, 1–2 sittings); measure the slope against power-off |
| **Playback power** (dark) | Unknown mix; decode most likely on the COP (`apple-dual-core.md` §2); 1.2 V fixed core; drive policy open | One core decoding at 24 MHz as the target, COP parked, drive bursts ≤ 6/h target; ~35–40 mA estimated today (`p19-placement.md` §0) | **unknown; measure** (we cannot meter Apple's firmware on this unit) | Run Apple's firmware (hold MENU, read-only) through the same FLAC album dark and log the voltage slope against ours (S, one sitting; no writes) |
| **Wake latency** | Light sleep: ~10.5 ms of PP restore, then LDO ramp, codec and panel; deep: a ROM boot from the `hibe` image (seconds) | Full boot every time (power-off); the BCM's cold start is 0.37–0.62 s to the first frame alone | **behind** | Soft sleep (P18) brings us to "instant"; the BCM hibernate pin path (`brief-map-power.md` §3) can beat Apple's panel restart (M) |
| **UI responsiveness** | One OS, UI on the CPU [hypothesis], BCM at a load-governed clock | UI moving to the COP, BCM jobs for glides and the drift, CPU free for playback | **ahead** by design; **measure** the lit glide frame time on both firmwares | Time a 100-row glide on Apple's firmware (camera, read-only) against ours (S) |
| **Disk I/O** | 45 named disk channels (21 in 1.2.x), each releasing with its own linger: **250 ms after a song-buffer read**, 1 s for UI reads, 5 s after preference writes; a predictive spin-up on a skip (new in 1.3) [apple-RE] | Burst refills, a 3 s dark hold, writes piggy-backed on refills; UDMA2 at 21 MB/s | **behind on the hold** (3 s against 250 ms), ahead on batching | AW-1, the per-reason linger with an automatic fallback (S); T-LINGER |
| **Boot time** | Rare: the user mostly sees light-sleep wakes; deep sleep restores an image | Every start is a full boot (bootloader, firmware, BCM upload, library) | **behind** | Soft sleep first (P18); then measure our cold boot against Apple's `hibe` resume before deciding on A10 (L) |
| **Core use** | Both cores sleep in `PROC_SLEEP` when idle; the COP woken by work **and by every 10 ms governor pass** (`osos 0x100C_FB08`), so ~100 Hz while awake [apple-RE]; mailbox semaphores and a doorbell for cross-core messages | COP pulsed 100/s by the tick today; K-1 parks it; the RPC uses queues plus cache operations | **behind until K-1 lands**, then level; ahead in placement (one core in dark play) | Land K-1 (M); consider the mailbox doorbell for COP wakes instead of a tick (`apple-dual-core.md` §4) |
| **Clock** | A 10 ms load governor at **26–80 MHz on the PLL**, timed 80 MHz bursts of 1–30 s, 80 MHz pinned in disk mode; no voltage scaling; never the crystal while awake (`apple-power-catalog.md` §2.1) [apple-RE] | 24/30/80 MHz; PLL off at idle and in dark play | **ahead** (a lower floor, the PLL off); the 100 MHz and DVS probes could add to it (`mac-and-dvs.md` §5) | Keep the crystal floor; take Apple's governor as a prior for `pgov` (AW-12); read the speed grade (S, read-only) |
| **Regulators** | LDO sleep mode `010` for codec and LCD, in sleep only; LDO voltages higher than ours (3.0 V vs our 2.5/2.6 V for codec/LCD); the accessory supply on only for an accessory; charger registers never written [apple-RE] | Lower LDO voltages always; no sleep mode; the accessory supply on by default on a unit with no dock | **ahead awake** (except the accessory rail), **behind asleep** | AW-3 (accessory supply off without a dock), AW-4 (LCD rail `010` while the LCD sleeps), SL-4 (S each) |
| **Statistics** | Usage log with light/deep sleep, disk, backlight, playback totals | Power log rows (`stor_on_pct`, `idle_pct`, …) | **level** | A11: add sleep totals once P18 lands (S) |
| **USB on a charger** | Probes for a host after VBUS and powers the controller and PHY off again if none answers [apple-RE] | USB clocked and the PLL held at 30 MHz for as long as the charger is in | **behind** | AW-2 with a wider window (S–M); T-USBPROBE |
| **Codec** | Fully on through pauses; fully off (VMID too, its clock gated) only in sleep [apple-RE] | Standby 5 s after PCM stops, 6.4 ms wake [silicon] | **ahead awake**; behind asleep | AW-5 (gate its clock and I2S in standby), SL-3 (full off in sleep) (S each) |

---

## Sources

Apple's iPod Video firmware 1.3, with 1.2.1 and 1.2.3 for cross-checks, read statically as described above (`sources.md`, "Apple's firmware"). Our side: `../design/p19-placement.md`, `../phase18-brief.md`, `../data-paths.md`, `clocks-power.md`, `../bcm2722-reference/brief-map-power.md`.
