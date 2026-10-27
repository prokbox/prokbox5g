# Install and restore routes

*Owner direction, 2026-10-09; reviewed for 0.8.0 on 2026-10-26. These notes feed the installer (#43, public release), the user guide (`docs/user/guide/getting-started.md`, `going-back.md`) and the docs site. Nothing here applies to the owner's own unit, which keeps Apple's firmware as its recovery path (CLAUDE.md rule 5). Never test any of this on the owner's unit: use a spare 5G or 5.5G.*

Every install route must come with a documented, tested way back to stock. The user-facing wording lives in the guide; this file is the engineering record of what each route does and what is still unproven.

## What ships in 0.8.0

- **One install type only: side-by-side (Method A).** The release-plan (section 4.1) ships only the Apple-firmware-compatible type for 1.0 and the 0.8.x pre-releases. Method B (ProkBox only) below is planned and **not in 0.8.0**; the guide must not offer it.
- **Release builds only.** Published zips are release builds with no developer menu (release-plan 1.3a). Nothing in the install or restore text may refer to a developer escape or to a button combination that reaches one.
- **Eight zips.** The 0.8.0 downloads are the 2 x 2 x 2 matrix of battery (stock or modded), storage (hard drive or flash) and memory (32 MB or 64 MB). The user picks one (guide: getting-started; release notes). The dock connector and haptics are not separate downloads in 0.8.0; see "Build variants" below.
- **No installer app in 0.8.0 (owner, 2026-10-26).** The user installs by hand: `ipodpatcher -r ProkBox-backup.bin` (save the firmware partition), `ipodpatcher -ab bootloader-ipodvideo.ipod` (add the bootloader), then copy the zip's files to the top level of the drive with hidden files shown. Uninstall is `ipodpatcher -w ProkBox-backup.bin`. The guide (getting-started, going-back) carries these steps. The installer in release-plan section 4.2 is for a later release. ⚠️ The exact `ipodpatcher` options and the zip contents must be confirmed on the spare unit before release.

## Background: what boots what
- **Flash ROM (never touched by us):** Apple's boot code is in the player's flash ROM. It loads the `osos` image from the disk's firmware partition. **Disk Mode** (reset, then hold SELECT+PLAY) and the hardware diagnostics (reset, then hold SELECT+PREV) are also believed to live in the ROM, so they work whatever is on the disk. ⚠️ **To verify on a spare 5G/5.5G** before any route below depends on it (release-plan E9).
- **Firmware partition:** a small partition at the start of the disk holding the firmware images (`osos` and friends).
- **Data partition:** FAT32 for ProkBox, which holds the music and ProkBox's own files. Apple's firmware also runs on FAT32 (the "Windows format"). HFS+ (the "Mac format", APM partition map) works only with Apple's firmware.

## Method A: side-by-side (the 0.8.0 route)

**Install**
1. Back up anything on the player that exists nowhere else (the guide says this first and in a warning box).
2. The player must be FAT32 / Windows format. A Mac-format player must be erased and reformatted first; say so before starting, and require a typed confirmation.
3. The installer saves a copy of the firmware partition to the computer (`ProkBox-backup-<serial>-<date>.img`) before it changes anything, then adds the ProkBox bootloader to `osos`, keeping Apple's firmware alongside, and reads it back to check.
4. Copy the ProkBox files from the zip to the data partition. By hand, the ProkBox folder starts with a dot and is hidden in Finder and Explorer, so the by-hand route must say how to show hidden files; the installer avoids the problem.
5. Eject and restart. The player boots into ProkBox.

**Everyday use:** ProkBox boots normally. Holding MENU while booting starts Apple's firmware for that session.

**Restore routes (all three are in the guide)**
1. **Quick switch, nothing erased:** restart (MENU+SELECT), then hold MENU as the screen goes dark.
2. **Uninstall the bootloader, music kept:** the installer's Uninstall restores the saved firmware partition (or, if the backup is missing, removes the bootloader with the patcher's own method). The data partition is untouched. Apple's firmware then runs on FAT32 with an empty library until Apple's tools sync.
3. **Full restore to stock, erases the player:** Disk Mode, then Finder (macOS 10.15 or later), or iTunes or the Apple Devices app on Windows. This reformats to Apple's default for that computer (HFS+ on a Mac) and reinstalls Apple's firmware.

**Updating:** the installer's Update (or copying the new zip over the old files, with hidden files shown) keeps settings, library, play counts and resume points. The bootloader is untouched unless the release notes say otherwise.

## Method B: ProkBox only (planned; not in 0.8.0)
Mac-only at first, for less technical users.

**Install**
1. A mandatory **backup prompt**: the player is erased.
2. Admin password, for raw disk access.
3. Repartition to MBR: a small firmware partition, then one FAT32 data partition. This converts a Mac-format (APM/HFS+) player.
4. Write a firmware partition holding **only the ProkBox bootloader** as `osos`. No Apple firmware is needed, shipped or copied.
5. Copy the ProkBox files.

**Everyday use:** ProkBox is the only firmware. MENU at boot does nothing extra.

**Restore routes**
1. **Back to stock, erases the player:** reset with MENU+SELECT, hold SELECT+PLAY for Disk Mode, connect to a Mac and use Finder's Restore. ⚠️ Depends on Disk Mode being in the ROM; verify first. Fallback if Finder doesn't offer Restore: the installer's "Restore stock layout" rewrites a standard partition map.
2. **Switch to side-by-side:** the installer's "Add Apple firmware" adds Apple's firmware to the firmware partition, taken from the user's own copy or from Apple's download. The installer never redistributes Apple's firmware.

## Platforms
Installers for **macOS, Windows and Linux** go on the GitHub release (release-plan 4.2). macOS is required for 0.8.0; Windows and Linux before 1.0.
- One shared core does the work: partition, firmware-partition writer, state detection, backup check, with a thin native front end per OS.
- Raw disk access needs the platform's admin step: an admin password on macOS, UAC on Windows, `pkexec`/sudo or udev rules on Linux. Each is explained before it is asked for.
- Each platform's restore guide covers that platform's own tools. On a Mac that's Finder. On Windows it's iTunes or Apple Devices. **Linux has no Apple restore tool**, so a Linux user needs the installer's Uninstall (route 2), or a Mac or Windows computer for the full restore (route 3). The guide says so plainly.

## Build variants
**0.8.0 ships eight zips**, the product of three choices the user can make with no tools:
- battery: stock or modded (changes the battery curve and estimates);
- storage: hard drive or flash (buffering, read strategy, spin-up);
- RAM: 32 or 64 MB (buffer and refill sizes).

Each is a separate release build (`--ipodui-release`) with its own code paths, run through the same gate. Dock connector and haptics are not part of the 0.8.0 matrix: the dock stays in the code, and the installer's questions (or the unit's config file) pick the USB glyph and the haptic tick.

*Contradiction to resolve:* release-plan Q9 and section 1.4 still say "one zip, unit facts in an installer-written config; two zips only if #79 misses Nov 2". If the eight-zip matrix is the decision, update those lines. The runtime detection (#79) can still be used to double-check a user's choice and warn on a mismatch.

The installer records the chosen variant on the player, so updates keep the same one. A user who picked wrong re-installs the right zip; the guide says so.

## Requirements for the installer and docs
- Every install path shows its restore path **before** it starts, in plain language, and the guide repeats it.
- No restore route may need Windows or extra hardware. The one exception to state openly: the full restore (route 3) needs a Mac or Windows computer, because Apple's restore tools do not exist on Linux. Routes 1 and 2 need neither.
- The installer detects the current state (Mac format, Windows format, side-by-side) and offers only the moves that make sense from there.
- Destructive steps need an explicit typed confirmation and a backup check.
- Everything is written in our own words (CLAUDE.md rule 2); the device is called "the player" except where the hardware is named, and uses our own screenshots.

## Open points (found in the 0.8.0 review)
- [x] 0.8.0 has no installer app: the guide gives the by-hand steps (see above). Confirm them on the spare unit.
- [ ] Update release-plan Q9 and 1.4 to the eight-zip matrix (see above) and note that the zips are `ProkBox5g-<ver>-{stock,modded}-{flash,hdd}-{32,64}MB.zip`.
- [ ] Add "what to do if an install fails halfway" to the guide: re-run the installer (it is safe to repeat), then Uninstall using the saved backup. Unproven; test on the spare unit.
- [ ] Add "I lost the backup file" to the guide: the Uninstall fallback (route 2) and the full restore (route 3).
- [ ] Say what happens on a hard-drive unit: Disk Mode and restore behave the same, but the first boot after a long copy may take longer to build the library. Needs the HDD run (later 0.8.x).

## To verify (spike on a spare 5G/5.5G, before any public release)
- [ ] Disk Mode and diagnostics work with an empty or ProkBox-only firmware partition.
- [ ] A from-scratch MBR + firmware partition + FAT32 layout boots the ProkBox bootloader (Method B).
- [ ] Finder Restore works from the ProkBox-only state on current macOS (Method B).
- [ ] The minimum firmware partition size the ROM accepts.
- [ ] Mac-format → FAT32 conversion on 30 GB and 80 GB 5G/5.5G, including 2048-byte sector models.
- [ ] Uninstall (Method A, route 2) brings back a working Apple firmware with the music intact.
- [ ] Hold MENU reaches Apple's firmware on each of the eight variants' bootloaders (they share one bootloader; confirm once).
