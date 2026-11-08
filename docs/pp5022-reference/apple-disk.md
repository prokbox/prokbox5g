# Apple's disk power: what firmware 1.3 teaches about spin-up and spin-down

Desk study, 2026-11-06 (branch `p19-diskre`). The owner asked: what can Apple firmware 1.3 teach us about spinning the drive up and down, and are there specific tricks? This chapter answers that, for the **stock 1.8-inch hard drive** (Toshiba, 4,200 rpm, ATA-66; the public release) and for **flash** (our test unit). It extends [`apple-power-catalog.md`](apple-power-catalog.md) §2.2 (the disk arbiter) and **corrects two of its claims** (§7). Our side of the bus is in [`storage.md`](storage.md); the flash/HDD rules are in [`../storage-choices.md`](../storage-choices.md).

**Method.** As in the catalogue: a static read of the osos 1.3 image at base `0x1000_0000`, with 1.2.1 and 1.2.3 for comparison. We used literal-pool, call-argument, pointer-table and function-start scripts kept outside the repository, plus Ghidra 12.2 headless in read-only mode. Only addresses, constants and the order of operations were recorded; every description is in our own words, and no decompiler output was kept. Nothing touched a device.

**Tags.** [apple-RE] read from Apple's code; [hypothesis] our reading of its purpose or meaning; [unknown] not found; [code] ProkBox's tree on `p19-diskre`, by file and line; [estimate] computed from stated figures; [silicon] measured on our unit.

**Vtable note.** Apple's C++ vtables hold code offsets *without* the `0x1000_0000` base (`sources.md`). The drive object's vtable sits at `osos 0x1067_6FC0`, the ATA device class's at `0x1067_6AE0`, and the IDENTIFY/SET FEATURES layer's at `0x1066_8630`.

---

## 1. Apple's tricks, ranked

Ranked by the gain we expect on **hard-drive units** first, then flash. Each one has a row with its evidence in §2–§6, and a plan row in §8.

| # | Trick | Where | HDD gain | Flash gain |
|---|---|---|---|---|
| D-1 | **Per-client holds with a short linger after a completed song fill (250 ms)**, so the drive spins down about ¼ s after the read that needed it, not after a fixed idle timer. Follow-up work is folded in or deferred (D-3, D-4), which is why 250 ms is enough | §2.1–§2.3 | **large**: ~5 s less spinning and 2 s less standby per refill against today's 5 s + 2 s | small: we already cut 3 s after the last access |
| D-2 | **Pre-spin by play time.** While playing, when the next item is not in the cache and the time left before the cached audio ends falls below a lead (**7 s, 12 s or 20 s**, by track class), the drive is spun up ahead of the read. 1.3 holds that pre-spin for **500 ms** (1.2.x held it for **10 s**) | §3.1 | latency and no underrun: hides the 1–2 s spin-up | none: no spin-up to hide |
| D-3 | **"Only if already spinning" clients.** Settings and play-count writes, album-art reads and UI activity ask for the disk only if it is up; otherwise they wait for the next spin-up. UI activity on a spinning disk extends it by 3 s, but never starts it | §4 | large: no spin-ups of their own | moderate: fewer power-ups |
| D-4 | **Writes released with no linger.** A settings write that rode a spin-up releases with 0 ms, so it never lengthens the spin | §4 | moderate | small |
| D-5 | **Rapid-skip pre-spin.** A user skip that comes while the previous skip is still settling (the first was under 200 ms ago when the new track started, [hypothesis]) spins the drive up at once and holds 2 s, betting that the user is about to outrun the cached tracks | §3.2 | latency on fast skipping | none (a cost) |
| D-6 | **STANDBY IMMEDIATE, then cut the rail**, both every time; the drive's own APM is set to level 1 | §5.1, §5.3 | already ours (APM differs) | already ours |
| D-7 | **Fast power-on**: the rail, 4 ms, the controller clock, 10 ms, then poll for ready every 10 ms; no fixed 250 ms wait | §5.2 | none (spin-up dominates) | **~0.23 s less per power-up**, if the controller tolerates it |
| D-8 | **Generous recovery**: 10 s for ready, then a power cycle (50 ms off) and 21 s more; 58 s for a soft reset; 10 s per command, 30 s for IDENTIFY | §6 | robustness on a slow or old drive | none: our 3 s fail-fast is right for flash |

What Apple does **not** have [apple-RE, by absence]: no global spin-down timeout, no lit/dark difference in the arbiter, no special case for pause or charging, no temperature-dependent policy in the disk path, and no SLEEP command (`0xE6`) or standby timer (`0xE2`/`0xE3`) in the ATA command classes (§5.3).

---

## 2. The disk manager

### 2.1 The task and its messages [apple-RE]

`DiskMgrTask` (entry `osos 0x1028_4B0C`, created at `0x1025_D40C`) owns 45 per-channel slots: an "active" byte (`0x1086_BEDC`) and a millisecond countdown (`0x1086_BF0C`), where −1 means held with no deadline. It waits on its queue (id 20) with a timeout equal to the smallest countdown left, measures how long it actually waited (the µs timer), and takes that time off every running countdown. A channel whose countdown reaches zero goes inactive.

Clients post one of seven message kinds (byte at message +0x10) through small wrappers:

| Kind | Wrapper | What it does |
|---|---|---|
| 0 obtain | `osos 0x1009_4E6C` (channel, wait flag); `0x1009_4EE8` sets the flag | channel active, countdown −1 (held). The flag (message +0x20) makes the manager tell the drive object synchronously [hypothesis: the caller needs the drive ready before it reads] |
| 1 obtain if spinning | `0x1009_4BBC` (channel) | attaches the channel only if the drive's power pin says it is on; replies yes or no, synchronously. The basis of D-3 |
| 2 release | `0x1009_4D80` (channel, linger ms) | countdown = linger + 1 ms; a linger of 0 drops the channel at once |
| 3 attach when up | `0x1009_4D1C` (channel) | parks the channel as "pending"; the next time the manager sees the drive powered, the pending channel is marked held |
| 4 spin down now | `0x1009_4E08` | clears every channel, logs "Disk Spindown", tells the drive object it is not needed |
| 5, 6 | `0x1009_4C24`, `0x1009_4C88` | forward drive events 7 and 8 [unknown: their meaning] |

After each message the manager recomputes "any channel active". On a change it logs "Disk Spinup" (event 0) or "Disk Spindown" (event 1) to the usage log, then posts a system event (class 14, sub-kind 4) with **4 = needed** or **2 = not needed** (`osos 0x1028_4DBC`/`0x1028_4DC8`). Obtain and release also log events 2 and 3 ("Disk Obtain/Release Access"). The usage log (strings at `osos 0x1021_2B68`–`0x1021_2CA1`) totals the disk's running time and power-on count; Apple measured exactly what we propose to measure in §9.3.

### 2.2 The channels, their callers and lingers [apple-RE]

Names from the switch at `osos 0x1021_1DDC` (debug strings only, §7). Obtains and lingers from the 57 release and 56 obtain call sites in 1.3, resolved by script. "If up" marks an obtain-if-spinning (kind 1); "wait" marks the wait flag; "var" a linger passed in a register.

| Ch | Name | Obtain | Linger(s) on release | Sites (1.3) |
|---|---|---|---|---|
| 0 | Reserved | – | – | – |
| 1 | AppBoot | obtain | **15 s** | `0x100A_25E4`, `0x100B_4A84`; release `0x100B_4F98` |
| 2 | BufferedSongReading | obtain | **250 ms** after a song fill completes; 250 ms (conditional); **2 s** after a lookup | fill `0x1008_7CEC` via `0x1009_27E0`; release `0x100A_BFA4` (fill done), `0x1009_2820`, `0x1020_3394` (2 s) |
| 3 | PrefsWriting | **if up** (three handlers); attach-when-up + wait (database load) | **0** after a piggy-backed write; **5 s** after a database load | if up `0x1011_0188`, `0x101E_E90C`, `0x1024_8480`; load `0x100B_4FE4` (release `0x100B_51C4`) |
| 4 | GeneralUserExperience | **if up** | **3 s** | `0x101D_8358`/`0x101D_8368` |
| 5 | PlayFromDisk | obtain | 250 ms (conditional) | `0x100A_CA90`, `0x1010_1908` |
| 6 | CacheSpinupDrive | obtain | **500 ms** (pre-spin, D-2); 5 s; 0 | `0x100E_1E14`/`0x100E_1E20`; `0x1024_69CC`/`69D8`; `0x1024_8420` |
| 7 | TestLogging | – | – | – |
| 8 | AppFileLoading | obtain | 1 s | `0x100C_9FC8`, `0x100C_A12C` |
| 9 | VCardReading | obtain | 1 s, 10 s | `0x101B_9B84`…`0x101B_A160` |
| 10 | LongSongScanning | – | 0 | `0x1009_9108` |
| 11 | VoiceRecording | obtain | 5 s | `0x1020_816C`…`0x1020_8614` |
| 12 | VoiceRecordingNewFileSegment | – | – | – |
| 13 | PhotoBrowse | obtain (wait) | 5 s | `0x1019_95F8`/`9604` |
| 14 | PhotoImporting | obtain | 1 s | `0x1019_A728`/`A764` |
| 15 | Notes | obtain, obtain (wait) | 500 ms; 0 | `0x1012_FCE8`/`FD14`; `0x1014_0124`/`0200` |
| 16 | PhotoFileManagement | – | – | – |
| 17 | DiskModeChannel | attach-when-up + wait | held for USB disk mode; **5 s** on exit | `0x1012_AF78`, `0x1012_B93C`; exit `0x1016_3A60` |
| 18 | FirewireChannel | – | – | – |
| 19 | USBChannel | – | – | – |
| 20 | UnitTests | obtain (wait) | 1 s | `0x1018_F394`/`F43C` |
| 21 | FreeSpaceCache | – | – | – |
| 22 | OnTheGoFileMgmt | obtain | 1 s | `0x101E_A050`, `0x101E_C20C`/`C2A4` |
| 23 | SlideShow | – | – | – |
| 24 | ImageCache | obtain (wait) | 250 ms | `0x1017_2444`/`2690` |
| 25 | AlbumArtReading | **if up** twice, then obtain | **500 ms** | `0x1020_5528`, `0x1020_5550`, `0x1020_553C`; release `0x1020_5958` |
| 26 | Video | obtain (wait) | 0 or **30 ms** (250 ms in 1.2.x) | `0x101B_57DC`, `0x101B_588C`/`58CC` |
| 27 | DiskImage | obtain | 1 s | `0x100F_C3E0`… |
| 28 | ResourceAccess | obtain | 5 s | `0x1028_7EDC`/`7F60` |
| 29 | VideoCoreBoot | obtain | 1 s | `0x101A_FE40`… |
| 30 | DiskFormatConvert | obtain | 1 s | `0x1016_3310`/`3690` |
| 31 | StreamCacheAddFile | obtain (wait) | var | `0x101F_73F0` |
| 32 | FontFileAccess | obtain (wait) | var (new sites in 1.3) | `0x101A_EC00`…`0x101A_F3A8` |
| 33 | ScreenLock | obtain | 1 s | `0x1022_C7C0`… |
| 34 | DiskReaderTask | var | var | through a register [unknown: sites] |
| 35 | ProfilerAccess | – | – | – |
| 36 | eAppAccess | obtain, **if up** | 5 s, 10 s, 30 s | `0x1012_DB28`…`0x101E_3D84`; if up `0x100C_7288` |
| 37 | eAppWriteBackCache | **if up** | var | `0x1017_124C` |
| 38 | TrainerFileAccess | – | – | – |
| 39 | IapStorage | obtain | 1 s (several; one new in 1.3) | `0x101A_79D4`…`0x101A_86B8` |
| 40 | XMLParsing | – | – | – |
| 41 | AudioPrompt | – | – | – |
| 42 | AudioPromptXML | obtain | 1 s | `0x101A_0F44` |
| 43 | StreamCacheSeek | obtain | **250 ms** | `0x1017_A7B8`/`A7EC` |
| 44 | PredictiveCacheSpinup | obtain | **2 s** (D-5) | `0x100A_97D4`/`97E0` |

"–" means no constant-channel call site was found; such a channel may still be used through a register.

### 2.3 Who keeps the disk up, and what 250 ms triggers [apple-RE unless marked]

- **The disk is up while any channel is active.** The longest pending linger decides the spin-down; there is no other timer in the manager.
- **After a song fill.** The fill (`osos 0x1008_7CEC`, from `0x1009_2750`) obtains channel 2 and sets a "fill in progress" byte. When the fill completes (`0x100A_BF58`) the byte is cleared and channel 2 is released with **250 ms**. With nothing else held, the manager posts "not needed" about 250 ms later.
- **What "not needed" reaches.** The drive object's power method (`osos 0x1023_3358`, vtable slot +0x24) has two states. For **off** it sends STANDBY IMMEDIATE through the ATA device (§5.1), marks the drive off, waits until the controller's work loop is idle (polled every 10 ms), resets the controller's control bits (`0x1023_332C`) and **cuts the rail** (`0x1026_5CC8(0)`). For **on** it powers the rail and re-initialises (§5.2). **[unknown]: the link between the manager's class-14 event and that method**, and so whether any extra delay sits between them. The method itself has no timer. Finding the event's receiver is open question #56.
- **So on a hard drive, Apple's 250 ms leads to standby and a rail cut**, not to a standby with the power left on. The drive's own APM level 1 (§5.3) would also let it spin down by itself; with the rail cut it rarely matters [hypothesis].
- **Paused, dark, lit, charging.** No per-state linger in the manager. Lit browsing reaches the disk only through UI channels (1 s each), and UI activity extends an already spinning disk by 3 s (channel 4, if up). [apple-RE, by absence: no backlight or charger input reaches the manager.]
- **USB disk mode.** Channel 17 is attached when the drive is up and held for the whole of disk mode; on exit it is released with 5 s (`0x1016_3A60`).
- **Sleep and shutdown.** The path into sleep (`0x100B_41A4`) posts "spin down now" (kind 4), then calls the sleep entry `0x1028_23BC`, which cuts the drive rail (`0x1028_249C`) for sleep states 1 and 2.

---

## 3. Spinning up ahead of need

### 3.1 Pre-spin by play time (D-2) [apple-RE unless marked]

- **Lead time per track** (set when a track is loaded into the cache, `osos 0x1008_7E04`–`0x1008_7E24`, stored at cache entry +0x174):
  - 7 s if bit 5 of a flags byte (track +0x8B) is set [unknown: which tracks; perhaps variable bit rate or spoken word];
  - else **20 s** if a 16-bit track field (+0x74) is ≤ 1,000, and **12 s** above 1,000 [hypothesis: the bit rate in kbit/s, so 12 s for lossless and uncompressed].
- **The check** (`0x1009_1054`, called from the playback tick at `0x100D_1B40`), all must hold:
  - playing;
  - no song fill already in progress;
  - the next item is not in the cache;
  - the time left in the cached audio (ms) is under the lead.

  Then it signals the track-cache task (`0x100B_66C4`) and marks the entry so it asks only once.
- **The track-cache task** (`TrackCacheReadTask`, created at `0x1009_4948`: 8 KB stack, priority 10; 64 read slots; loop at `0x100E_1DBC`) obtains channel 6 and releases it with **500 ms** (1.2.1 and 1.2.3: **10 s**, `osos-1.2.1 0x100E_0CB4`). The drive starts spinning at once. The fill that follows holds channel 2, so the short hold only has to bridge to it.
- **Units.** The lead is in **play time**, not bytes, and fixed per track class, not measured from this drive's spin-up. The constants (7,000, 20,000, 12,000 ms) are the same in all three versions.

### 3.2 Rapid-skip pre-spin (D-5) [apple-RE; the purpose and the window's meaning are hypotheses]

- The next-track handler (`osos 0x100A_9734`, usage event 5) on a **user** skip:
  - **if no skip is in progress**, it notes one: a flag at player +0x29F0, the playback mode at +0x29F4, the µs time at +0x29F8;
  - **if one is in progress**, it obtains channel 44 and releases it with **2 s**.
- When the new track starts (`0x100A_13E8`), the flag is cleared if more than **200,000 µs (200 ms)** have passed since it was set.
- **So a single skip is served from the cache with no spin-up**, and a second skip while the first is still settling spins the drive up straight away.
- Present, identical, in 1.2.1 (`0x100A_A9A4`–`0x100A_A9B8`).

### 3.3 Buffering [apple-RE where cited; much is open]

- The track cache has 64 read slots and reports its size in KB to the usage log ("Track cache size", `osos 0x1021_3491`).
- **A skip with the next track cached** is served without the disk (the next-track handler only spins up on the rapid case).
- **[unknown]:** the cache's total size, the read size per spin-up, its low and high marks, and whether it reads whole next tracks. Its fill loop is `osos 0x100E_1DBC`, and the parameter source behind the "Track Cache Size" string is a lead (`open-questions.md` #57).

---

## 4. Write batching and follow-up reads [apple-RE]

| What | When Apple does it | Evidence |
|---|---|---|
| Settings and play counts ("prefs") | Only if the disk is already spinning (kind 1 on channel 3); write; release with **0 ms**. If it is not spinning, nothing is written now. Three handlers do this, on player and system events | `osos 0x1011_0188`, `0x101E_E90C` (also before a playback-state change, usage event 16), `0x1024_8480` |
| Database load (after boot or sync) | Attach when up, obtain with wait, load, release with **5 s** | `0x100B_4FE4`, release `0x100B_51C4` |
| Album art | Twice "if up" first; obtains outright only on the later path; releases with 500 ms | `0x1020_5528`/`5550`, `0x1020_553C`, `0x1020_5958` |
| UI activity | Extends a spinning disk by 3 s; never starts it | `0x101D_8358` |
| Next track's metadata | From the in-RAM database, so no disk read at a track change [hypothesis: the database is loaded whole at `0x100B_4FE4`] | – |

**What this means for the 250 ms.** Apple can release 250 ms after a fill because nothing else needs a separate spin-up. The metadata is in RAM, art and settings ride a spin that is already happening, and the UI only stretches a running disk. Writes release with 0 ms, so they don't stretch it at all.

---

## 5. ATA commands and the drive's power

### 5.1 Power-down [apple-RE]

The drive object's off path (`osos 0x1023_3358`, state 1), in order:

1. Send **STANDBY IMMEDIATE (`0xE0`)** with a 10 s timeout (ATA device method `0x1022_7598`; the IDENTIFY layer's state 2 at `0x1014_2FA4` → `0x1014_3218` does the same).
2. Mark the drive off.
3. Wait for the controller's work loop to be idle, polling every 10 ms.
4. Clear the controller's control bits (`0x1023_332C`).
5. Cut the rail (`0x1026_5CC8(0)`):
   - clear `DEV_EN` bit 25;
   - wait 10 ms;
   - park GPIO G/H (enables `0xFF`) and OR I (`0xBF`) and K (`0x1F`);
   - drive the power pin (Apple pin `0x52`, or `0x86` on one board variant);
   - on board ID 12, switch off PMU LDO 4.

No separate FLUSH CACHE was found before the standby. STANDBY IMMEDIATE writes the cache per the ATA standard. FLUSH CACHE EXT (`0xEA`) exists as an ATA device method (`0x1022_663C`). **Apple always does both the standby and the cut**; it never leaves a spun-down drive powered between uses.

### 5.2 Power-up [apple-RE]

- **Rail on** (`0x1026_5CC8(1)`):
  1. on board 12, PMU LDO 4 on and 2 ms;
  2. the power pin;
  3. **4 ms**;
  4. `DEV_EN` bit 25 on, GPIO handed back to the IDE;
  5. **10 ms**.
- **Controller set-up** (`0x1023_2DA4`): the configuration bits; timing computed for an 80 MHz bus clock (`0x1023_2D14`).
- **Ready probe** (vtable +0x80 → `0x1023_2EC8`):
  1. wait 10 ms;
  2. poll the status register every 10 ms for **BSY clear and DRDY set**, up to **10 s**;
  3. on timeout: **cut the rail, 50 ms, rail on, 2 ms**, then poll again up to **21 s**;
  4. then select the device.
- **Re-initialise the drive** (the IDENTIFY layer's state 1/4 → `0x1014_316C`): SET FEATURES with:
  - transfer mode (sub-command 3) for PIO, multiword DMA and UDMA, each from the drive's IDENTIFY;
  - **write cache on** (`0x02`);
  - **read look-ahead on** (`0xAA`);
  - **APM (`0x05`) level 1**.
- **At first detection only** (`0x1014_349C`): IDENTIFY (`0xEC`, or `0xA1` for a packet device), 30 s timeout; the mode tables; then the same SET FEATURES.

### 5.3 Commands present and absent [apple-RE]

| Present | Absent (no call site) |
|---|---|
| IDENTIFY `0xEC`/`0xA1`; SET FEATURES `0xEF` (3, 2, `0xAA`, 5 = APM level 1); STANDBY IMMEDIATE `0xE0`; IDLE IMMEDIATE `0xE1`; FLUSH CACHE EXT `0xEA`; READ/WRITE DMA `0xC8`/`0xCA`; READ/WRITE SECTORS `0x20`/`0x30`; READ VERIFY `0x40`; RECALIBRATE `0x10`; SMART `0xB0` (READ DATA `0xD0`, READ LOG `0xD5`, EXECUTE OFF-LINE `0xD4`) | SLEEP `0xE6`; STANDBY with a timer `0xE2`; IDLE with a timer `0xE3`; SET FEATURES APM disable (`0x85`) |

The ATA device class's methods (vtable `0x1067_6AE0`) are each a full taskfile: the command, a timeout (10,000 ms for most, 30,000 ms for IDENTIFY and one command with flags 5 at `0x1022_82F0`), the transfer size and direction. We found no caller of IDLE IMMEDIATE, RECALIBRATE or the SMART methods outside the class's own table [unknown: probably the diagnostics].

**The APM setting is the same in 1.2.1 and 1.2.3** (the identical SET FEATURES sequence at `osos-1.2.1 0x1014_0DE4`, `osos-1.2.3 0x1014_1970`).

---

## 6. Errors and recovery [apple-RE]

| Case | Apple | ProkBox [code] |
|---|---|---|
| Ready after power-on | 10 s, then one power cycle (50 ms off, 2 ms settle), then 21 s | flash 3 s, HDD 30 s; one retry after 100 ms off; give up the idle cut after 3 failed power-ups (`firmware/drivers/ata.c:1366-1373`; `storage.md` §5.1) |
| Reset path when the drive won't come ready (`0x1023_2DEC`) | 50 ms, then poll DRDY up to 10 s (1,000 × 10 ms); on timeout power cycle 100 ms off / 100 ms on, re-initialise, return an error | soft reset, then the command's retry (`ata.c:655-668, 756-837`) |
| Soft reset (`0x1023_314C`) | controller bits, 10 ms, assert the reset, then poll BSY up to **58 s** | `perform_soft_reset()`, `wait_for_rdy()` 10 s (`ata.c:269-286, 1267-1310`) |
| Per-command timeout | 10 s (30 s for IDENTIFY) | `READWRITE_TIMEOUT` and `wait_for_rdy()` 10 s |
| Temperature | none found in the disk path | none |
| SMART | in the ATA class, apparently for diagnostics | `HAVE_ATA_SMART` on HDD builds (`storage-choices.md` §2) |

---

## 7. What changed between 1.2.1, 1.2.3 and 1.3 [apple-RE]

The catalogue's first edition read the disk arbiter as rewritten for 1.3, with 24 new channels and a new predictive spin-up. **A call-site comparison shows otherwise:**

- **The channel numbers and the predictive spin-up were already in 1.2.1.** Its code obtains and releases channels up to 44 (for example channel 44 with 2 s at `osos-1.2.1 0x100A_A9B8`, channel 43 with 250 ms at `0x1017_4D30`, channel 36 at `0x1012_CAA0`). The rapid-skip logic is the same, field for field. Only the **debug name table** grew: 1.2.x names 21 channels and prints "Unknown Disk Channel" for the rest; 1.3 names all 45.
- **The release wrapper's body is the same message**, at a different address (`osos-1.2.1 0x1009_6BA4`, `osos-1.2.3 0x1009_4C30`, 1.3 `0x1009_4D80`). 1.3's also logs the usage event; the usage-log strings exist in all three.
- **What did change, by the release sites' (channel, linger) pairs:**

| Change | 1.2.1 / 1.2.3 | 1.3 | Site (1.3) |
|---|---|---|---|
| **Pre-spin hold (channel 6, D-2)** | **10 s** | **500 ms** | `0x100E_1E20` |
| Video release (channel 26) | 250 ms | 30 ms | `0x101B_58CC` |
| FontFileAccess (channel 32) | – | two new release sites | `0x101A_EC8C`, `0x101A_F3A8` |
| IapStorage (channel 39) | – | one more 1 s site (matched by count) | one of `0x101A_7A30`…`0x101A_86B8` |
| One more 1 s release on a register channel | – | new | – |

- **Unchanged:** the 250 ms after a fill, the 2 s rapid skip, the 7/12/20 s leads, prefs "if up" with 0 ms, APM level 1, the standby and cut.

**Reading.** Apple's last PP-side disk-power improvement was to **stop holding the drive for 10 s after a pre-spin** and let the fill's own 250 ms decide. That is up to ~9.5 s of spinning saved per refill on a hard drive [estimate, if the fill used to finish well inside the 10 s].

---

## 8. Against ProkBox: one row per behaviour

**Rule (owner, 2026-10-15):** storage may change tuning, never what the user sees. Each row has a plan per profile (§9).

| ID | Apple (evidence) | ProkBox today [code] | Gap | HDD plan | Flash plan | Risk | Test |
|---|---|---|---|---|---|---|---|
| D-1 | 250 ms after a completed fill, then standby and cut (§2.3) [apple-RE; the event link unknown] | one hold from the last access of any thread. **HDD:** upstream's spin-down setting, 5 s (`apps/settings_list.c:1762`; `ata.c:131-136`), then standby and **cut 2 s later** (`ata.c:80, 973`). **Flash:** standby after 1 s, cut 3 s dark / 60 s lit, doubling to 600 s after short off periods (`firmware/export/stor_pwr.h:108-114`) | HDD: ~4.75 s more spinning and 2 s more standby per refill | **adopt**: a per-reason linger. After a fill that reached its target, `linger_fill`; after a partial fill or other reads, `linger_other`; writes 0. Start at Apple's values and tune from the wake log (§9.3). Keep the backoff | **adopt, tuned**: `linger_fill` at the break-even (~0.6 s, §9.2) until the wake log says otherwise; `linger_other` 3 s as today | more spin-ups if follow-up reads straggle (art, tagcache, the idle notices' writes); on HDD each costs 1–2 s at high current | host: `tests/storpwr/` cases for the per-reason linger and the backoff; sim: `sim_stor_tune()` with a straggler; bench (flash, owner): T-LINGER in the catalogue; **HDD tester**: T-HDD-LINGER (§9.4) |
| D-2 | pre-spin when the cached audio left < 7/12/20 s and the next item is uncached; hold 500 ms (§3.1) | watermark = measured spin-up + 5 s + `buffer_margin` in **bytes** from the last track's bit rate (`apps/playback.c:1399-1424, 1448-1480`); the refill read spins the drive up synchronously | equivalent trigger. Ours is in bytes; Apple's is in play time with a fixed floor per class. Our refill spins up on demand, with no early, separate spin-up | **adopt in part**: express the HDD margin as play time (measured spin-up + 5 s, with a floor of 12 s, or 20 s for compressed tracks [hypothesis on the class]), scaled by the playback speed as the flash path already is | **skip** (no spin-up); keep the 6 s power-up margin | a longer margin refills a little earlier, which is harmless | host: a `stor_pwr_refill_secs()` case per profile; **HDD tester**: underrun count = 0 over a 2 h mixed-bit-rate playlist, `spinup_ms` logged |
| D-3 | "if up" clients: prefs, art, UI extension 3 s (§4) | statistics drain only while warm (`tagcache.c` `ui_drain_wanted()`, `storage_power_warm()`); cover render only with the disk active (`storage-choices.md` "Now Playing cover render"); browsing reads spin up on demand; lit hold 60 s on flash | the same idea for writes. A lit-UI read on a HDD spins it up; Apple's UI never extends a stopped disk | **adopt**: UI activity extends a spinning disk by 3 s (`ui_extend`), never starts it. Settings save rides a spin | **done** (warm drain); `ui_extend` = the lit hold as today | none new | host: drain gated on `powered()`; sim script: browse while dark-playing, count `disk_wakes` by waker |
| D-4 | writes release with 0 ms (§4) | writes extend `last_disk_activity` like reads (`ata.c` `keep_ata_active()`) | a write after a fill stretches the hold | **adopt**: a piggy-backed write doesn't restart the linger | **adopt** | a write that needs a follow-up read | host: write after fill, the cut time unchanged |
| D-5 | rapid-skip pre-spin, 2 s (§3.2) | none: a skip past the buffer rebuffers on demand | skip latency on HDD | **adopt**: a second user skip within 200 ms of the previous track start spins the drive up asynchronously, held `skip_prespin` 2 s | **skip**: a power-up costs ~0.4 s at ~100 mA and saves nothing | an extra spin-up per fast-skip burst | **HDD tester**: time from the 3rd fast skip to sound, with and without |
| D-6 | STANDBY IMMEDIATE then the cut; APM level 1 (§5.1, §5.3) | FLUSH CACHE, STANDBY IMMEDIATE, then the cut (`ata.c:954-1000`); APM `0x80`, the lowest power without standby (`ata.c:1490`) | APM only | **skip APM 1 for now**: we cut the rail ourselves, so the drive's own standby adds only load/unload cycles. Revisit if the HDD tester's SMART load-cycle count is low | n/a (adapters ignore APM [hypothesis]) | APM 1 on a Toshiba 1.8" could unload the heads during a long fill gap | **HDD tester**: SMART attribute 193 before and after a day [hypothesis: exposed] |
| D-7 | rail, 4 ms, clock on, 10 ms, then poll every 10 ms (§5.2) | rail, **250 ms**, then the controller and reset (`ata.c:1343-1373`); upstream warns that early IDE access stalls the core and drops audio | 236 ms per power-up | **skip**: spin-up dominates | **beat**: poll after Apple's ~15 ms, with the audio-dropout check as the pass criterion | a core stall and a dropout ([rockbox] warning, Mini 2G) | bench (owner, flash): 50 power-ups during playback, 0 dropouts, `stor pwr on ms=` falls by ~200 |
| D-8 | 10 s ready + power cycle + 21 s; 58 s reset (§6) | flash 3 s, HDD 30 s, one retry, give-up after 3 | none material | **adopt Apple's shape**: 10 s, power cycle (50 ms off), 21 s; reset BSY up to 30 s | **keep** fail-fast 3 s | a failing HDD takes longer to give up | host: fault injection in `tests/storpwr/` for the HDD profile |
| D-9 | no lit/dark/charging special case in the arbiter (§2.3) | flash: lit hold 60 s vs dark 3 s | ours is deliberate (a power-up mid-scroll would show) | keep the lit hold, but as `ui_extend` (D-3) | keep | – | – |

---

## 9. Two storage profiles: a proposed design

The owner's note: *"We may need to switch timings, or have an extra process for non-SSD/flash storage versions."* One build, the profile picked at runtime from the detected storage (#79).

### 9.1 Where the switch lives

- **Detection:** at `ata_init()`, as `spw_solid` is today (`ata.c:1708`): `ata_disk_isssd()`, IDENTIFY word 83 bit 2, or the measured wake (`storage-choices.md` §1). When unsure, choose the HDD profile, which is conservative.
- **The table:** a `struct stor_profile` in `firmware/export/stor_pwr_core.h`, with two constant instances (`stor_profile_hdd`, `stor_profile_solid`) in `firmware/common/stor_pwr_core.c`. It is pure data, so the host test covers both. `struct stor_pwr_tune` becomes part of it.
- **The users:**
  - `ata.c` reads it for the linger, the ready waits and the power-on settle. The HDD path stops using upstream's `sleep_timeout` + `ATA_POWER_OFF_TIMEOUT` pair for the idle cut.
  - `apps/playback.c` reads it for the refill margin. The compile-time `HAVE_DISK_STORAGE`/`HAVE_STORAGE_POWEROFF` branches at `playback.c:1399-1446` become one runtime branch.
  - The skip handler reads `skip_prespin`.
  - The settings `disk_spindown` and `buffer_margin` are ignored on both profiles and never surfaced.

### 9.2 The parameters that differ

| Parameter | HDD profile (Apple-derived) | Flash profile | Basis |
|---|---|---|---|
| `linger_fill`: after a fill reached its target | **250 ms** (Apple), tuned by the wake log | **~600 ms** (the break-even below), tuned by the wake log | §2.3; §9.2 |
| `linger_other`: after other reads | **1 s** (Apple's UI clients); 2 s after a lookup | 3 s (today's dark hold) | §2.2 |
| `linger_write` | 0 | 0 | §4 |
| `ui_extend`: UI activity on a spinning drive | **3 s**, never starts it | the lit hold (60 s with backoff), as today | §4; `stor_pwr.h` |
| `prespin_lead`: refill margin in play time | max(measured spin-up + 5 s, **12 s**; 20 s for compressed [hypothesis]) × the speed | 1 s + 2 s budget, × the speed (≥ 2×) | §3.1; `playback.c` |
| `prespin_hold` | **500 ms**, bridging to the fill | 0 | §3.1, §7 |
| `skip_prespin` | **2 s**, after a second skip within 200 ms | 0 (off) | §3.2 |
| `pwron_settle` | 250 ms (upstream; spin-up dominates) | **~15 ms, then poll**, if T-FASTON passes | §5.2 |
| `ready_timeout` / retry | **10 s**, power cycle 50 ms off, **21 s** | 3 s, 100 ms off, once | §6 |
| `min_off` (the backoff trigger) | ~2 × break-even ≈ 10 s [estimate] | 30 s (today) | §9.3 |
| APM | `0x80` (today) | n/a | §8 D-6 |

**The HDD's extra process**, which flash doesn't need:
1. **Measure the spin-up on every power-on.** `ata_spinup_time()` exists (`firmware/drivers/ata-common.c:30-39`); log it in the wake log.
2. **STANDBY IMMEDIATE before every cut, waited for.** This parks the heads; we do it already. It must never be skipped on the HDD profile, even when the cache is clean.
3. **The rapid-skip pre-spin** (D-5) and the time-based pre-spin (D-2).
4. **The longer recovery** (D-8).
5. Optional: a SMART load-cycle read at boot, logged (D-6).

No temperature policy: Apple has none in the disk path either.

### 9.3 The owner's question: is 250 ms too short?

**For Apple it is enough**, because nothing else in a refill wants the disk (§4):
- the next track's metadata is in RAM;
- art and settings ride a spin that is already happening;
- the UI only extends a running disk;
- writes release with 0 ms.

The 250 ms only has to cover the gap between the fill's last read and its completion handling. Then STANDBY IMMEDIATE and the rail cut follow (§2.3; whether anything sits between the event and the drive object is open question #56).

**For us it depends on our follow-ups**: the art read-ahead, the tagcache drain, the idle notices' writes, the cover render. They are why `STOR_PWR_HOLD_DARK_S` counts from the last access of any thread. Two things settle it.

**1. The break-even linger per profile** (the ski-rental bound: holding for exactly the break-even time is never worse than twice the best choice made in hindsight):

- **HDD** [estimate, to be replaced by the drive's datasheet and the HDD tester's slope]:
  - spin-up ~1.5 s at ~400 mA on top of idle ≈ 600 mA·s;
  - spinning idle ~130 mA;
  - **break-even ≈ 4.5 s**, plus one head load and unload per cycle.

  So a 250 ms linger pays only if a re-access within ~4.5 s is rare. That is exactly why Apple folds or defers every follow-up.
- **Flash** [estimate from `stor_pwr.h` and the catalogue's AW-1]:
  - power-up ~0.4 s at ~100 mA ≈ 40 mA·s;
  - powered idle ~65 mA;
  - **break-even ≈ 0.6 s**.

  A 250 ms linger is **below** flash's break-even: a re-wake within 0.6 s of the cut costs more than holding would have.

**2. The wake log**, which decides the value per profile:
- **Per power-on:**
  - the time since the last cut (ms);
  - the waker: the thread slot that issued the first command, from the power log's existing `disk_by` attribution (`apps/ipodui/powerlog_core.h:63`);
  - the reason (refill, art, settings, tagcache, skip, USB);
  - the spin-up or re-init time.

  This goes in one `stor_pwr` event: `ms` already holds the re-init time, and an off-period field and a waker byte are added.
- **Per power-log row:** a histogram of off-periods in buckets: < 0.25, 0.25–0.6, 0.6–2, 2–5, 5–30, > 30 s.
- **The rule:**
  - If wakes under the break-even (flash: < 0.6 s; HDD: < 4.5 s) after a fill exceed **5 %** of power-ons, find the waker and **fold it into the refill** (read it before the fill completes, or make it "if up"), rather than raising the linger.
  - If no fold is possible, set `linger_fill` to the smallest bucket edge that brings those wakes under 5 %.
  - The owner's flash logs decide the flash value; the HDD tester's logs decide the HDD value.

**"Fold follow-ups into the refill", where needed:**
- the next track's art read-ahead (a read that follows the fill by a moment);
- the tagcache drain (it should run before the fill releases, while the drive is warm);
- the idle notices' writes.

Each becomes either part of the fill or an "if up" client released with 0 ms.

### 9.4 Tests

| Test | Profile | Who | Pass |
|---|---|---|---|
| `tests/storpwr/`: the profile table; per-reason lingers; writes don't extend; backoff; the HDD recovery shape (10 s, power cycle, 21 s) with injected failures | both | host | all cases green on both instances |
| `stor_pwr_refill_secs()` per profile, at 1×, 2× and 3× speed | both | host | HDD ≥ 12 s × speed; flash 6 s × speed |
| Sim (`sim_stor_tune()`, `sim_stor_pwrup()`): dark playback with art and tagcache stragglers | both | sim | off-period histogram shows < 5 % under the break-even after folding |
| T-LINGER (catalogue §5) with the wake log | flash | owner | re-wakes < 0.6 s after a cut < 5 %; slope lower than the 3 s arm |
| T-FASTON: 50 power-ups during playback with a 15 ms settle | flash | owner | 0 audio dropouts; `stor pwr on ms=` ~200 lower |
| T-HDD-LINGER: the same FLAC album, dark; arm A today (5 s + 2 s), arm B `linger_fill` 250 ms | HDD | **HDD tester** | underruns 0; re-wakes < 4.5 s after a cut < 5 %; disk on-time per hour lower; voltage slope lower |
| T-HDD-PRESPIN: 2 h mixed bit-rate playlist, dark | HDD | **HDD tester** | underruns 0; the log shows the spin-up starting ≥ the spin-up time before the buffer end |
| T-HDD-SKIP: five fast skips past the buffer | HDD | **HDD tester** | sound after the 3rd skip sooner with `skip_prespin` on; one spin-up per burst |
| T-HDD-APM: SMART load cycles over a day | HDD | **HDD tester** | logged only; decides D-6 |

---

## 10. Sources

- The osos 1.3, 1.2.1 and 1.2.3 images (`sources.md`, "Apple's firmware"), read as described under Method.
- Our side: `firmware/drivers/ata.c`, `firmware/drivers/ata-common.c`, `firmware/export/stor_pwr.h`, `firmware/export/stor_pwr_core.h`, `firmware/common/stor_pwr_core.c`, `tests/storpwr/`, `apps/playback.c`, `apps/ipodui/powerlog_core.h`, and [`../storage-choices.md`](../storage-choices.md).
