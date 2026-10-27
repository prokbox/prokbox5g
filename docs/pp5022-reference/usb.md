# USB

The PP5022 contains a USB 2.0 high-speed OTG controller of the ARC/ChipIdea family (the same register model as the "EHCI-style device controller" in many SoCs) at `0xC500_0000`, with an on-chip PHY. On the iPod Video it runs as a device (mass storage). The PP-specific parts — enabling, the PHY sequence, detection, addressing — are below; the controller's own programming model follows the standard ARC device-controller documentation. [rockbox] `usb-drv-arc.c`, `usb-fw-pp502x.c`

## 1. Registers (offsets from `0xC500_0000`) [rockbox]

| Offset | Register | Offset | Register |
|---|---|---|---|
| `0x000` | ID | `0x158` | ENDPOINTLISTADDR (2 KB aligned) |
| `0x100` | CAPLENGTH (8-bit) | `0x160` | BURSTSIZE |
| `0x124` | DCCPARAMS | `0x180` | CONFIGFLAG |
| `0x140` | USBCMD (bit 0 run, bit 1 reset) | `0x184` | PORTSC1 (speed bits `[27:26]`, `0x0800_0000` = high speed; PHCD bit 23) |
| `0x144` | USBSTS (bit 0 int, 1 error, 2 port change, 6 reset, 7 SOF, 8 suspend) | `0x1A4` | OTGSC (bit 9 A-VBUS valid, bit 11 B-session valid) |
| `0x148` | USBINTR | `0x1A8` | USBMODE (2 = device) |
| `0x14C` | FRINDEX | `0x1AC` | ENDPTSETUPSTAT |
| `0x154` | DEVICEADDR (bits `[31:25]`) | `0x1B0`/`0x1B4`/`0x1B8`/`0x1BC` | ENDPTPRIME / FLUSH / STATUS / COMPLETE (OUT ep n = bit n, IN ep n = bit 16+n) |
| | | `0x1C0 + 4n` | ENDPTCTRLn |

Interrupt: source 20 (`USB_IRQ`).

Endpoints: Rockbox uses 3 (EP0 plus two), both directions (`USB_NUM_ENDPOINTS 3`); whether the silicon has more is in DCCPARAMS, not recorded here.

## 2. Bring-up on the PP5022 [rockbox] `usb_reset_controller()`, `usb_drv_reset()`

```
0x7000_002C |= 0x0300_0000              ; enable the USB module
DEV_EN |= DEV_USB0 | DEV_USB1           ; 0x8 | 0x40_0000
pulse DEV_RS.USB0, then DEV_RS.USB1
DEV_INIT2 |= INIT_USB                   ; 0x8000_0000
wait for 0x7000_0028 bit 7              ; USB clock/PHY ready [hypothesis]
0x7000_0028 |= 0x2
udelay(100000)
XMB_RAM_CFG (0x7000_003C) |= 0x47A
```

Then the controller reset (USBCMD: clear run, wait 50 ms, set reset, wait for it to clear), and — **on a PP5022 only, after that reset** — this PHY sequence (meanings unknown):

```
0x7000_0060 |= 0xF
0x7000_0028 |= 0x1_0000; 0x7000_0028 &= ~0x1_0000
0x7000_0060 &= ~0x20; udelay(10)
0x7000_0060 |= 0x20;  udelay(10)
0x7000_0060 = (0x7000_0060 & ~0xF) | 4; udelay(10)
0x7000_0060 &= ~0x20; udelay(10)
0x7000_0060 &= ~0xF;  udelay(10)
0x7000_0060 |= 0x20;  udelay(10)
0x7000_0028 |= 0x800; 0x7000_0028 &= ~0x800
wait for 0x7000_0028 bit 7
```

Then: USBMODE = 2; ENDPOINTLISTADDR; DEVICEADDR = 0; USBINTR = int | error | port change | reset; `CPU_INT_EN = USB_MASK`; run.

Apple's ROM, on the PP5022 path, writes 2 to `0xC500_0140` (USBCMD reset) and waits for the bit to clear, then waits for `0x7000_0028` bit 7 [external] siggi `16`.

Between sessions Rockbox clears `DEV_USB0`, `DEV_USB1` and `INIT_USB` to save power, but **not in the bootloader**: doing so there stops Apple's firmware from loading afterwards [rockbox].

## 3. Memory and addressing

- Queue heads (64 bytes each, 2 per endpoint) and transfer descriptors (32 bytes, 32-byte aligned, ≤ 16 KB each) live in **IRAM** in Rockbox, the queue-head array 2 KB-aligned at `0x4000_0000`. IRAM addresses are the same physically and logically, so the controller can use them directly. [rockbox] `app-pp.lds`, `pp5020.h`
- Data buffers in SDRAM are passed as **physical** addresses (`addr | 0x1000_0000`), flushed or invalidated first. The controller does not see the MMAP remap. [rockbox] `usb_storage.c`; [hypothesis] for "does not see the remap", by analogy with the other bus masters

## 4. Cable detection and charging

| Signal | Pin | Evidence |
|---|---|---|
| VBUS present | GPIOL bit 4, high | [rockbox] `usb-fw-pp502x.c` |
| External (dock/FireWire) power | GPIOL bit 3, low | [rockbox] |
| USB current 100 / 500 mA | GPIOA bit 2 (LTC4066 HPWR) | [rockbox] |
| No USB current | GPIOL bit 2 (LTC4066 SUSP) | [rockbox] |

Detection is by interrupt: enable GPIOL bit 4's interrupt with `INT_LEV` set to the inserted level, and on each event flip `INT_LEV` to the opposite level, clear, and debounce 200 ms. The controller's OTGSC VBUS bit is also readable. The iPod Video has no FireWire detection in Rockbox. USB IDs used: vendor `0x05AC`, product `0x1209`. [rockbox]

The 500 mA limit must be requested by firmware (HPWR high) after enumeration; the LTC4066 otherwise limits to 100 mA.

## 5. Our measurements [silicon]

| Item | Result | Source |
|---|---|---|
| Link speed | high speed in most sessions (`hs=1`); some sessions full speed (`hs=0`), cause unknown | device logs 2026-10-12/13 |
| Mass-storage reads, 1 MB requests (before read-ahead) | ~7.5 MB/s | 2026-10-10 |
| Host's mount-time FAT scan (12 KB average requests) | ~5.4 MB/s | 2026-10-10 |
| Reads with read-ahead in a 2 × 64 KB buffer | ~18.5 MB/s while busy | 2026-10-12/13 |

The controller's per-transfer overhead was not the limit: the ARC driver issues 16 KB descriptors and takes one interrupt per transfer [rockbox]; the flash read and the per-command turnaround dominated.

## 6. Behaviour we found on the PP5022 [silicon]

Device logs 2026-10-16/17 on the USB-C-modded 5.5G; sessions and commits in [`../usb-history.md`](../usb-history.md). These are findings from one unit and one conversion board; they are not proven for the stock dock path.

| Finding | Detail | Source |
|---|---|---|
| **A static, page-aligned, uncached INQUIRY buffer fixes the first INQUIRY that never retired.** | With the reply in movable memory, the 36-byte INQUIRY Bulk-IN dTD stayed primed and ACTIVE (up to 108 s) and macOS stopped at the SCSI logical unit. Putting the reply in a static page-aligned buffer, addressed through the uncached alias, makes it complete (`inq2data=1`). It is now the default. The cause (alignment, cache line or placement) is not separated. | `2465b3d092` |
| **Active-dTD stall signature.** | During a READ: IN endpoint ACTIVE (`e20 t80`), overlay `ov80`, 23-24 x 512 B left of a 16 KB transfer, no completion for ~304 ms, then a genuine host reset. No dTD error bits, no USB error interrupt (`uei=0`). The active dTD starts at page offset `0xb60`, so it is not a first-page boundary fault. | 2026-10-16 logs, `e576031066` |
| **4 KiB dTD split.** | `USB_ARC_SMALL_DTD` keeps the 64 KiB BOT transfer but describes it as sixteen 4 KiB dTDs, to avoid a partly consumed 16 KiB tail. It did not remove the first-command timeout. | `2cefd681ce` |
| **Prime retry.** | A dropped Bulk-IN prime after a host reset is retried (3 attempts, pipe flushed, data toggle and BOT command untouched). Counters showed no recovered drop in the failing case. | `b9d19f7166` |
| **Queue-head overlay reset on configure.** | A bus reset stops the endpoint engine but does not clear queue-head RAM. At configuration the overlay, halt and toggle state are cleared so a previous configuration cannot take part in the first BOT reply. Baseline hygiene; no demonstrated fix. | `cc16c920ec` |
| **ARC re-init on Video USB-C attach.** | A second `usb_drv_init()` at attach (Rockbox's old workaround, removed upstream in `5af0a50031` for dock compatibility, not disproven on this controller). Developer row `legacy` / `normal` for an A/B. No verdict yet. | `6de6b2986a` |
| **The stream-disable bit does nothing.** | USBMODE.SDIS was requested; USBMODE read back `0x02`, not `0x12`. The PP5022 ignores it. The A/B changed no behaviour. | `050d0d9cf9`, `usb_sdis` session |
| **TXFILLTUNING (offset `0x164`) reads back 0.** | Writing `0x10` (TXFIFOTHRES) read back as 0. On this older core the offset may not be TXFILLTUNING; it is a host-mode register on other ChipIdea parts. Writing it correlated with dead enumeration. It is left untouched by default. | `f4db3af867`, `ea36e78e09` sessions |
| **CPU boost for the USB session breaks enumeration.** | With the CPU boost held, the unit enumerated at high speed but repeatedly failed to expose storage (68 resets in one session). Do not hold a boost. | `session 38o` |
| **ATA DMA overlapping Bulk-IN DMA shortens long reads.** | Serialising them (prefetch worker off; then every next ATA read after the IN completes) lifted reads past the old 166 MiB failure point. See [`../data-paths.md`](../data-paths.md) D12. | `fa571b6895`, `41103a9158` |
