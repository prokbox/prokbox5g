# Boot

The chain on an iPod Video: **reset → Apple's boot ROM in the 1 MB NOR flash → an image read from the disk's firmware partition into SDRAM → (optionally) a second-stage bootloader inside that image → the operating system.** Nothing on the disk runs before the ROM has set up SDRAM, the clocks and the BCM2722.

## 1. Reset and the flash ROM

| Item | Value | Evidence |
|---|---|---|
| Part | 1 MB NOR, SST 39WF800A (some references say 39VF800A) | [external] siggi `05` |
| Mapping at reset | at `0x0000_0000` **and** `0x2000_0000` | [external] siggi `03`, `07`; [rockbox] `hwcompat.h` reads `0x0000_405C` pre-remap |
| Cores | **both** leave reset and run the ROM from address 0 | [external] siggi `06`; [rockbox] |
| Reset vector | a branch to `0x8000` (works at either mapping); the other vectors jump to absolute IRAM addresses `0x4000_0004`… | [external] siggi `07`, from the ROM's bytes |
| ROM working set | the ROM copies code and its vector table from NOR `0x8000`–`0x2_0000` into IRAM in scattered pieces and runs there | [external] siggi `03`, `07` |
| Core split | the ROM reads `PROCESSOR_ID` (as a word, masked to the low byte); the COP writes `0x8000_0000` to `COP_CTL` (sleeps) and, once woken, jumps through the word at IRAM `0x4000_0050` | [external] siggi `06` (ROM at `0x8738`, `0x8054`) |
| Hand-off | the ROM writes the entry (`0x1000_0000`) to `0x4000_0050`, wakes the COP (`COP_CTL = 0`) and branches to the entry itself. **Both cores enter the loaded image at the same address** | [external] siggi `07`; [rockbox] `crt0-pp-bl.S:90` ("Jump both CPU and COP there") |
| MMAP at hand-off | the ROM programs no remap window; SDRAM is at `0x1000_0000` | [external] siggi `07` (counted writes); [rockbox] (the bootloader runs unremapped) |
| CPU clock at hand-off | whatever the ROM chose; Rockbox's bootloader does not change it. Not recorded | [rockbox] |
| BCM2722 at hand-off | powered (`GPO32` bit 14) and running Apple's boot image from the flash | [rockbox] `lcd-video.c:578`; [silicon] every session since S9 |

### Flash contents

| Offset | What | Evidence |
|---|---|---|
| `0x0000` | vectors | [external] |
| `0x4000` | system configuration block ("SysCfg", tags stored byte-reversed): serial number, firmware id, hardware id, **hardware version** (value word at `0x405C`), region, model number | [external] siggi `03`, `07`, `16` |
| `0x8000`–`0x1_FFFF` | the ROM's bootloader | [external] |
| image area | `disk` (Apple's USB disk mode), `diag` (service diagnostics), `logo`, `vmcs` (BCM2722 boot image) | [external] siggi `07`; [rockbox] (`vmcs`) |
| `0xF_FE00` | **image directory**: 40-byte entries `{ 'flsh', id, –, offset, length, –, –, checksum, … }` (32-bit words), offset from the flash base, checksum = byte sum | [rockbox] `lcd-video.c:179–209`; [external] siggi `07` |

Hardware version (`0x405C`): `0x000B_0005` on a retail 5G, **`0x000B_0011` on a retail 5.5G** (read from a real unit) [external] siggi `17`. Rockbox prints it and does not act on it. Apart from the configuration page, the 5G and 5.5G flash images are identical [external].

The flash is writable through a gate in `0x7000_0030` (bit 30 write-enable, bit 27 ready) with standard JEDEC command sequences [external] siggi `04` (Apple's ROM under emulation). **Never write it**: it is the only path to Apple's recovery.

## 2. The disk's firmware partition

| Item | Value | Evidence |
|---|---|---|
| Partition map | 5G and 5.5G retail disks: an Apple Partition Map (Windows-formatted units: an MBR). The firmware partition is first (`Apple_MDFW`, or MBR type 0 with a non-zero size), the FAT32/HFS data partition second | [rockbox] `utils/ipodpatcher/ipodpatcher.c:151–266`; [external] siggi `17` (a retail 5.5G: firmware partition at block 63, 2048-byte blocks) |
| Block size | the 5.5G's disk is formatted with **2048-byte** logical blocks for the partition map and the file system, although the ATA drive reports 512-byte sectors (`storage.md` §4) | [rockbox] `ipodvideo.h`, ipodpatcher; [external] |
| Partition header | +`0x000`: Apple's ASCII "STOP" banner; +`0x100`: magic `]ih[`; +`0x104`: directory offset (LE32, add `0x200`); +`0x10A`: format version (LE16; 3 on the Video) | [rockbox] ipodpatcher `read_directory` |

Directory entries (40 bytes, little-endian, up to 10):

| Offset | Field |
|---|---|
| +0 | device tag `!ATA` |
| +4 | type, byte-reversed: `osos` (the OS), `rsrc` (resources), `aupd` (flash updater), `hibe`, `osbk` |
| +8 | id (Apple's ROM treats +8 of `aupd` as an "applied" flag [external]) |
| +12 | `devOffset` |
| +16 | length |
| +20 | load address (`0x1000_0000`) |
| +24 | `entryOffset` (0 = start of the image) |
| +28 | checksum: unsigned byte sum of the image, no seed |
| +32 | version (`>> 8` = `0xB0` identifies the Video) |
| +36 | `loadAddr` |

Image location on disk: partition start + one block + `devOffset` (format version 3). [rockbox] ipodpatcher

The ROM verifies the checksum, reads `osos` (whole image, up to its length) to **`0x1000_0000`** and enters at `0x1000_0000 + entryOffset` [rockbox] `bootloader/ipod.c:244–298`. Per the emulator notes it reads in 2048-byte blocks with the IDE controller's DMA in 128 KB transfers, and runs `aupd` instead of `osos` when an un-applied updater is present [external] siggi `03`, `07`. `rsrc` is a FAT12 volume that Apple's OS reads itself (it holds the BCM's run-time firmware) [external].

## 3. Installing a second-stage bootloader (how Rockbox does it) [rockbox] ipodpatcher `add_bootloader`

The bootloader is **appended to Apple's `osos`**:

1. `entryOffset` = the image length rounded up to a block (or the existing `entryOffset`).
2. If the larger image would overlap the next image, the later images are moved up on the disk.
3. Write Apple's OS followed by the bootloader; set length = `entryOffset` + bootloader length, `entryOffset`, a new checksum, `loadAddr = 0xFFFF_FFFF`.

The ROM then loads Apple's OS **and** the bootloader to `0x1000_0000` and enters the bootloader. Apple's OS is still in SDRAM, so the bootloader can start it by jumping to `0x1000_0000` (it checks for the string `portalplayer` at `0x1000_0020`). Uninstall: length = `entryOffset`, `entryOffset` = 0, recompute the checksum. The ROM's image limit is not known; Rockbox's tools buffer 8 MB.

Apple's OS also recognises a request left in **the last 256 bytes of IRAM**:

| Address | Content | Effect after a reset |
|---|---|---|
| `0x4001_FF00` (PP5022; `0x4001_7F00` on 96 KB parts) | the 21 bytes `diskmode\0\0hotstuff\0\0\1` | Apple's firmware starts in USB disk mode |
| `0x4001_FF18` / `0x4001_FF1C` | tag `IsyS` and a pointer to the ROM's system-information block (board name "iPod M25", serial number, hardware revision at +`0x84`, …) | left by the ROM for the OS [external] siggi `02`, `16` |

[rockbox] `bootloader/ipod.c` (disk mode); [external] (the information block).

## 4. Starting a loaded image

### Rockbox's bootloader [rockbox] `crt0-pp-bl.S`, `boot-pp.lds`, `bootloader/ipod.c`

1. Both cores arrive. `msr cpsr_c, #0xDF` (System mode, IRQ off).
2. Copy itself (`.text` + `.data`, from `pc & ~0xFF`) to IRAM `0x4000_0000` and jump there. `.bss` goes to SDRAM `0x1100_0000` (above the 8 MB load area).
3. COP: write `0x8000_0000` to `COP_CTL` (sleep). CPU: wait for `COP_CTL` bit 31, clear `.bss`, set up a stack, run `main()`.
4. `main()` reads the hold switch first, initialises the LCD port (no BCM bootstrap: the ROM's BCM is already running), the font, the disk, mounts FAT, then loads a file to `0x1000_0000` (§6). It returns the entry address.
5. CPU: flush the cache if enabled (`0xF000_F044 |= 2`, wait `CACHE_CTL` bit 15), wake the COP (`COP_CTL = 0`), jump to the entry. COP, on waking: invalidate the cache (`|= 6`), jump to the same entry.

**Loaded file format** (`.ipod`): 4-byte big-endian checksum, 4-byte model tag `ipvd`, then the raw image. Checksum = 5 (the Video's model number) + the unsigned byte sum of the image. Loaded to `0x1000_0000`, entered at its first byte [rockbox] `rb-loader.c`, `tools/scramble.c`.

**What the next image receives:** both cores at `0x1000_0000`; CPU in System mode with IRQ off; COP freshly woken; SDRAM not remapped; caches flushed; the ROM's clock; the BCM running; the IDE controller initialised; USB left enabled (disabling it in the bootloader stops Apple's firmware from loading) [rockbox] `usb-fw-pp502x.c:123–129`.

### Rockbox's main image [rockbox] `crt0-pp.S`

Offset `0x00`: `msr cpsr_c, #0xD3; b skip`; offset `0x20`: the 8 bytes `Rockbox\1` (a signature the bootloader checks before re-entering an image left in SDRAM); room for vectors after it.

| Step | CPU | COP |
|---|---|---|
| 1 | read `PROCESSOR_ID`; mask all its interrupts (`CPU_INT_DIS = ~0`) | same with `COP_INT_DIS`; then sleep (`COP_CTL = 0x8000_0000`) |
| 2 | wait for the COP to sleep; `CACHE_CTL = 0`; probe 32/64 MB (`memory-map.md` §2) | asleep |
| 3 | copy the remap stub to IRAM `0x4000_0000`, jump to it, write MMAP0 (`memory-map.md` §3), jump to the logical address | asleep |
| 4 | wake the COP | runs the same stub for itself, then sleeps |
| 5 | wait for the COP to sleep; copy vectors to logical 0; copy `.iram`; zero `.ibss` and `.bss`; set stacks (IRQ, FIQ; SVC/abort/undef share the IRQ stack); System mode; `main()` | asleep |
| 6 | kernel init; wake the COP and sleep until it is done | set its stacks; `cop_main()`: its own `system_init()` (cache) and kernel init; wake the CPU |

Rule kept throughout: **CPU first, then COP**, each step gated by `PROC_CTL` bit 31 (the other core is asleep). Never run the remap stub on both cores at once.

### Re-entering an image without a reset ("rolo") [rockbox] `rolo.c`

Copy the new image to physical `0x1000_0000` from IRAM code; `CPU_INT_DIS = ~0`; clean+invalidate; `CACHE_CTL = 0`; zero all MMAP registers; hand-shake with the COP (which did the same for itself and is waiting in IRAM); both jump to `0x1000_0000`. The new image's crt0 treats this exactly like a cold start, except that the BCM may be unpowered (`display.md` §3).

## 5. The COP and old bootloaders

If the bootloader in front does not release the COP into the image, the COP never starts; Rockbox's `cop_main` comment warns that firmware must not assume the COP exists. With the flow above both cores always arrive. [rockbox]

## 6. Key combinations

| Held at power-on | Who acts | Result | Evidence |
|---|---|---|---|
| MENU + SELECT held for several seconds (while running) | hardware/firmware | reset | [external] Apple's user documentation; the mechanism (wheel controller or PMU) is not known |
| SELECT + LEFT (rewind) | Apple's ROM | the flash `diag` image | [external] siggi `07` (ROM under emulation: the ROM sends `0x8000_023A` to the wheel three times before loading) |
| SELECT + PLAY | Apple's ROM | Apple's USB disk mode (`disk` image) | [external] (expected, not tested by them) |
| hold switch on, or MENU | Rockbox bootloader | Apple's OS: a file `apple_os.ipod` from the system folder or the root, else the copy of Apple's OS already in SDRAM | [rockbox] `bootloader/ipod.c:400–430` |
| PLAY | Rockbox bootloader | `/linux.bin`, raw, no header | [rockbox] `:432–441` |
| RIGHT | Rockbox bootloader | verbose boot text | [rockbox] `:347` |
| none | Rockbox bootloader | `rockbox.ipod` from the system folder or the root, else an image already in SDRAM carrying `Rockbox\1` | [rockbox] `:443–461` |

The bootloader reads the buttons without the button driver [rockbox] `bootloader/ipod.c:75–146`: clear `GPIOB_ENABLE` bit 7; `0x7000_C104 |= 0x0C00_0000`; write `0x8000_023A` to `0x7000_C120`; `0x7000_C100 |= 0x8000_0000`; drive GPIOB bit 4 low; wait up to 1,500 µs for `0x7000_C104` bit 31; then undo those (`0x7000_C100 &= ~0x8000_0000`, `GPIOB_ENABLE |= 0x80`, GPIOB bit 4 high and released, `0x7000_C104 |= 0x0C00_0000`, `0x7000_C100 |= 0x6000_0000`); wait up to 1,500 µs for `0x7000_C104` bit 26; read `0x7000_C140`. A reply is valid when `(v & ~0x7FFF_0000) == 0x8000_023A`; buttons = `((v << 11) >> 27) ^ 0x1F`, active low: `0x02` RIGHT, `0x04` LEFT, `0x08` PLAY, `0x10` MENU (bit 0 SELECT [hypothesis]). Up to 5 tries. It relies on the ROM having enabled the wheel block.

**Recovery path:** keep Apple's `osos` intact inside the firmware partition and the MENU/hold branch in the bootloader. With those, a broken main image never prevents reaching Apple's firmware or its disk mode.

## 7. Failure handling in Rockbox's bootloader [rockbox] `bootloader/ipod.c:186–242`

If nothing can be loaded, it starts USB; when a cable is seen it puts the drive to sleep, waits 2 s, writes the disk-mode request (§3) and reboots (`DEV_RS |= DEV_SYSTEM`), so Apple's firmware comes up as a USB disk.
