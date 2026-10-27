# Storage: the IDE controller

The iPod Video's storage is a 1.8-inch parallel-ATA hard drive (30, 60 or 80 GB) [external] on the PP5022's IDE controller. Flash adapters (iFlash and similar) present themselves as an ATA disk on the same bus; our unit has a 1 TB one. [rockbox]; [ours]

## 1. Registers [rockbox] `pp5020.h`, `ata-target.h`

| Address | Name | Meaning |
|---|---|---|
| `0xC300_0000` | `IDE0_PRI_TIMING0` | PIO timing |
| `0xC300_0004` | `IDE0_PRI_TIMING1` | DMA timing |
| `0xC300_0008`–`0xC300_001C` | secondary / IDE1 timings | unused |
| `0xC300_0028` | `IDE0_CFG` | configuration and status (below) |
| `0xC300_002C` | `IDE1_CFG` | unused |
| `0xC300_01E0` | `ATA_DATA` | 16-bit data |
| `0xC300_01E4` | `ATA_ERROR` / features | 8-bit |
| `0xC300_01E8` | `ATA_NSECTOR` | |
| `0xC300_01EC` | `ATA_SECTOR` | |
| `0xC300_01F0` | `ATA_LCYL` | |
| `0xC300_01F4` | `ATA_HCYL` | |
| `0xC300_01F8` | `ATA_SELECT` | |
| `0xC300_01FC` | `ATA_COMMAND` / status | |
| `0xC300_03F8` | `ATA_CONTROL` / alt status | |
| `0xC300_0400` | `IDE_DMA_CONTROL` | bit 0 start, bit 1 set at setup ("arm"), bit 3 direction (1 = read, device → memory), bit 31 cleared at finish |
| `0xC300_0408` | `IDE_DMA_LENGTH` | bytes − 4 |
| `0xC300_040C` | `IDE_DMA_ADDR` | **physical** memory address |
| `0x6000_60C4` | – | IDE clock: Rockbox writes `0xC000_0000` ("80 MHz") before setting DMA timings |

Task-file registers are 8-bit at a 4-byte stride. A 32-bit read of `ATA_DATA` returns only 16 valid bits [external] siggi `16`.

`IDE0_CFG` bits [rockbox] `ata-pp5020.c`:

| Bit | Meaning |
|---|---|
| 3 (`0x8`) | INTRQ pending — **write 1 to clear**; polled for DMA completion |
| 4 (`0x10`) | cleared at DMA init (unknown) |
| 5 (`0x20`) | set at init (unknown; iPodLinux's driver sets it too) |
| 15 (`0x8000`) | DMA enable during a transfer |
| 28 (`0x1000_0000`) | "CPU > 65 MHz" (nano only); cleared on the Video |
| 29 (`0x2000_0000`) | "≥ 50 MHz", set with the DMA timings |

## 2. Timing [rockbox] `ata-pp5020.c`

| Mode | Register | Values |
|---|---|---|
| PIO 0–4 | `TIMING0` | `0xC293`, `0x43A2`, `0x11A1`, `0x7232`, `0x3131` (Apple's values, for 80 MHz; at 24 MHz the first is 0, the rest the same). Faster values corrupt data with some flash and mSATA adapters |
| init | `TIMING1` | `0x8000_2150` |
| MWDMA 0–2 | `TIMING1` | `0xF_9F92`, `0x5_6562`, `0x4_5451` |
| UDMA 0–4 | `TIMING1` | `0x8000_37C1`, `0x8000_3491`, `0x8000_3371`, `0x8000_3271`, `0x8000_3071` |

UDMA modes above 2 work only with the CPU at 80 MHz; Rockbox boosts for the duration of each such transfer. UDMA 2 works at 30 MHz. [rockbox]

## 3. Transfers

**PIO:** standard ATA; Rockbox's copy loops read and write `ATA_DATA` with unrolled 16-bit accesses. The drive's interrupt (source 23) is never used; Rockbox polls BSY/DRDY (30 s / 10 s limits). [rockbox]

**DMA** [rockbox] `ata_dma_setup()`, `ata_dma_finish()`:

```
cache: flush (write) or clean+invalidate (read)
IDE0_CFG |= 0x8                         ; clear old INTRQ
IDE_DMA_CONTROL |= 2
IDE_DMA_LENGTH = bytes - 4
IDE_DMA_ADDR   = physical address (logical + 0x1000_0000)
IDE_DMA_CONTROL bit 3 = read ? 1 : 0
IDE0_CFG |= 0x8000
issue the ATA DMA command (nIEN clear: ATA_CONTROL = 0)
IDE_DMA_CONTROL |= 1                    ; go
poll IDE0_CFG bit 3 (10 s timeout)
IDE0_CFG &= ~0x8000; IDE_DMA_CONTROL &= ~0x8000_0001
```

Rules:

- **Do not read the ATA status register while a DMA transfer runs**: it causes failures and hangs. Wait on `IDE0_CFG` bit 3 instead. [rockbox]
- Read buffers must be 16-byte (cache-line) aligned; write buffers 4-byte aligned. [rockbox]
- On a hard disk, DMA writes measured about 25 % slower than PIO, so Rockbox uses DMA for writes only on solid-state drives (detected from IDENTIFY words 217, 168, 169, 163, 83/160). Reads by DMA were about 8× faster. [rockbox] `ipodvideo.h` comment
- No hardware reset line is used; "hard reset" is a wait for BSY to clear after power-up. Soft reset: `SELECT = LBA | dev`, `CONTROL = nIEN | SRST`, 10 ms, `CONTROL = 0`, 10 ms, then wait for ready (up to 30 s with retries). [rockbox] `ata.c`

## 4. Sector sizes and the 5.5G

- The drives report **512-byte logical sectors**. Rockbox supports LBA48 (`HAVE_LBA48`; used when IDENTIFY word 83 bit 10 is set and words 60–61 read `0x0FFF_FFFF`). [rockbox]
- The 80 GB drive (and others) may have 1024-byte physical sectors; Rockbox reads the multiplier from IDENTIFY word 106 and treats a drive that can read LBA 1 as 512-byte emulating (`MAX_PHYS_SECTOR_SIZE 1024`). [rockbox] `ata-common.c`
- **The 5.5G's partition map and file systems are laid out in 2048-byte units** by Apple's formatter, as seen over USB, although the ATA drive itself is 512-byte. Firmware must multiply partition-table numbers by 4 to get ATA LBAs (`disk_set_sector_multiplier`). [rockbox] `ipodvideo.h` (`MAX_VIRT_SECTOR_SIZE 2048`), `disk.c`
- Whether a given unit is a "2048" unit is decided from the partition table, not from the drive. [rockbox]

## 5. Power

GPO32 bit 30, active low, powers the drive; the IDE pins are handed back to GPIO (`GPIOG/H` all, `GPIOI` `0xBF`, `GPIOK` `0x1F`) when off (`clocks-power.md` §6, `gpio-i2c.md` §3). After power-on wait ≥ 250 ms before the first access: early IDE access "makes the core hang for a short time" [rockbox] `ata.c`. Standby: FLUSH CACHE (`0xEA`/`0xE7`) then STANDBY IMMEDIATE (`0xE0`), if the drive supports power management.

### 5.1 Power cycling in use (#114, P18) 

What the firmware does, and what each step assumes:

| Step | Detail | Evidence |
|---|---|---|
| Cut | FLUSH CACHE, STANDBY IMMEDIATE, then `GPO32` bit 30 high; `DEV_IDE0` off and the IDE pins back to GPIO first (`ide_power_enable(false)`) | [rockbox] `power-ipod.c` |
| Only when safe | upstream cuts only a drive that claims power management (IDENTIFY words 82/85 bit 3) **or** honours FLUSH CACHE; its comment warns that "various SD adapters" lack the former. Our adapter flushes (word 83 bits 12/13) | [rockbox] `ata.h` `ata_disk_can_sleep()`; [silicon] S37 IDENTIFY (`w83=7404`) |
| Power on | `GPO32` bit 30 low, 1 tick, `DEV_IDE0` on, pins to IDE, then **≥ 250 ms** before the first register access (earlier access stalls the core and drops audio) | [rockbox] `ata.c` `ata_power_on()` |
| Controller | `DEV_IDE0` off may lose the controller's DMA/INTRQ set-up: `ata_device_init()` again (DMA control bit 1, `IDE0_CFG` bits 5 and 4/15), `ATA_CONTROL = 0` (INTRQ on, DMA completion needs it); `set_features()` re-writes `TIMING0/1` and `IDE0_CFG` bit 29. Both run **after** the 250 ms wait (#114 M1): they are IDE controller accesses, which upstream warns stall the core and drop audio if early; nothing in the power-on path touches the controller before the wait (`ide_power_enable(true)` only sets `GPO32`, `DEV_EN` and the pin enables) | whether the registers survive the clock gate: [hypothesis]; re-written regardless. Whether a controller write (not only a taskfile read) inside the 250 ms stalls the core: [hypothesis], avoided either way |
| Ready after reset | a solid drive must clear BSY within 3 s (`STOR_PWR_READY_S`) of the end of the wait or the attempt fails; a rotating drive keeps upstream's 30 s for spin-up. A dead adapter reads BSY set (floating bus, `0xFF`) or never sets RDY | [hypothesis]: the 4-card adapter is ready well inside 3 s (SD cards may take up to 1 s each to initialise); the device round's `stor pwr on ms=` measures it |
| Failed power-up | each power-up has one retry (cut, 100 ms off, power-on again). If both fail the power is cut again and the driver marks the drive off, so the caller gets an error after the two attempts (~7 s worst case on flash) and the next access starts from a fresh power-on, never a wait on a drive half up. A size mismatch (below) counts as a failure. After `STOR_PWR_FAIL_MAX` (3) failed power-ups in a row the idle cut is switched off for the session; each failure and the give-up are logged (`stor pwr off reason=fail`, `stor pwr on reason=giveup`), nothing is shown | [rockbox] the reset sequence; the policy is ours (host test `tests/storpwr/`, failures injected) |
| Cache safe before a cut | the power is cut only if FLUSH CACHE was offered and worked, or the drive reports no write cache enabled (IDENTIFY word 85 bit 5), and upstream's guard holds (power management claimed, or FLUSH CACHE offered). STANDBY IMMEDIATE is not trusted to write the cache: a drive without power management treats it as a no-op in the driver. A refused cut keeps the power on, is counted and logged once per run (`stor pwr on reason=dirty`); a failed flush leaves the drive on so the storage tick's standby retries it | [rockbox] `ata_disk_can_sleep()` and upstream's `can_sleep || canflush`; word 85 bit 5 per the ATA spec. Our adapter: [silicon] S37 `w83=7404` (FLUSH CACHE offered) |
| Drive | hard-reset wait (BSY clear), IDENTIFY, SET FEATURES (PIO mode, UDMA 2 as negotiated, write cache and look-ahead as at boot), SET MULTIPLE, IDENTIFY, SECURITY FREEZE LOCK: the drive is treated as one that lost power, no state kept | [rockbox] `ata_power_on()` |
| Multi-card adapter | the 4-card JBOD must report the same total sectors (words 60/61, LBA48 100–103) after the reset; a different size fails the power-on and it is retried once from a fresh cut (100 ms off). A second mismatch is a failed power-up (above): the drive is left off, its IDENTIFY words stale until the next power-up rewrites them; nothing reads them while it is off | [hypothesis]: an adapter could answer before every card is ready; not seen |
| Re-enumeration after a cut | **open**: does the iFlash quad adapter come back cleanly from a cut while the PP keeps running, how long until BSY clears, and does it need more than a reset (for example a longer off time to discharge)? The device session (`docs/device-session-p18-0.md`, #114 round) measures `stor pwr on ms=` and counts failures | [hypothesis] until then |
| Cost | power-up and re-init est. 0.3–0.5 s (250 ms of it the fixed wait); the adapter's standby draw versus off is the D3/#114 A/B | [hypothesis] |

## 6. Our measurements [silicon]

| Item | Result | Source |
|---|---|---|
| Mode negotiated by our 1 TB flash adapter | **UDMA 2** (`0x42`), not 4: IDENTIFY word 93 = 0 and word 76 = 0, so the 80-wire-cable check caps it | device logs `usb ata mode=0x42 ssd=1 w88=043f w93=0000 w76=0000`, every session |
| Solid-state detected | yes (`ssd=1`): writes use DMA | same |
| Reads during USB transfers | ~22 MB/s from the flash while busy; ~18.5 MB/s end to end over USB | device log counters, 2026-10-12/13 |
| Soft resets | occasional (1–5 per session; 16 in one write-heavy session) | same |
| PIO fallbacks | rare, writes only (a dozen per session at most) | same |

| Forced UDMA 4 (developer switch, Fit-1 C7) | works: 0 bad words reading a 64 MB file 8 times; **21 MB/s at UDMA 2 and at UDMA 4** | `ipodui.log` `udma test rc=0 bad=0 mbs2=21 mbs4=21`, three runs, S36c |

The UDMA 2 cap is a firmware decision on IDENTIFY data the adapter does not set. Forcing UDMA 4 works but gains nothing and costs an 80 MHz boost per command (S36c, S36), so UDMA 2 stays; where the 21 MB/s limit sits is open (`open-questions.md` #28; `../data-paths.md` D3).
