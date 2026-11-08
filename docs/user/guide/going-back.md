---
title: "Going back to Apple's firmware"
weight: 80
description: "How to return to Apple's firmware, from a quick switch to a full restore."
---

You can always return. There are four ways, from gentlest to most complete. Read this page before you install, so you know your way out.

## 1. Just for now (nothing is erased)

Either of these starts Apple's own software for this session only:

- Slide the **Hold** switch on and restart the player, or
- hold **Menu** while it starts. Hold it from the moment you begin the restart (**Menu** and **Select** together for about 8 seconds) until Apple's screen appears, rather than waiting for the screen to go dark.

Restart again with Hold off and nothing held to come back to ProkBox. Your music is untouched.

## 2. Remove ProkBox for good, keep your music

This puts the original start-up files back. It needs the dated backup file you saved once, before the first install (for example `ProkBox-backup-2026-11-06.bin`). You also need `ipodpatcher` and the same privileges as for installing: `sudo` on macOS and Linux, an Administrator Command Prompt on Windows. See [Getting started](getting-started.md).

1. Connect the player. On macOS, close Music and iTunes, and unmount the disk without ejecting it: `diskutil unmountDisk /dev/disk4` (with your own disk number from `diskutil list`). Never format.
2. Run `ipodpatcher -w ProkBox-backup-2026-11-06.bin`, with your own file name. Do not unplug it while it runs.
3. Eject and restart the player.

Apple's software then runs again with your files still on the drive, but its library looks empty until you sync with Apple's own tools. A recent Mac may have no way to sync it; if so, use a Windows computer with iTunes. You can delete the ProkBox files from the drive afterwards (show hidden files to see them).

> [!WARNING]
> Only use a backup that was taken before ProkBox was installed on that same player. A backup taken after the install contains the ProkBox start-up program, and writing it back leaves ProkBox in place. Never write one player's backup to another player.

## 3. Lost the backup file

Remove just the start-up program with `ipodpatcher -d`, run the same way (unmount first on macOS). It takes ProkBox out of the start-up chain and leaves your music alone. If that does not bring Apple's software back, use route 4.

## 4. Full restore (erases everything)

Use this if the player won't start at all, or you want it back to a factory state. If ProkBox merely misbehaves, try route 1 first.

1. Restart: hold **Menu** and **Select** for about 8 seconds.
2. Right away, hold **Select** and **Play** until Disk Mode appears.
3. Connect to a computer and use Apple's own restore tool. iTunes on Windows is the proven one. Finder on a recent Mac and the Apple Devices app may also offer it; we haven't tried them.
4. Choose **Restore**. This erases the player and puts back Apple's software, formatted for that computer. A Mac restore leaves the Mac format, which ProkBox cannot use until it is restored to Windows format again from a Windows computer.

> [!WARNING]
> A full restore erases everything on the player. Back up your music first if you can.

> [!NOTE]
> Apple has no restore tool for Linux. Use a Mac or Windows computer for this route, or ask us for help at prokbox.project@gmail.com.

## Then what?

After a restore the player is stock. You can install ProkBox again whenever you like by following [Getting started](getting-started.md).
