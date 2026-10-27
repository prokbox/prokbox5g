---
title: "Going back to Apple's firmware"
weight: 80
description: "How to return to Apple's firmware, from a quick switch to a full restore."
---

You can always return. There are three ways, from gentlest to most complete. Read this page before you install, so you know your way out.

## 1. Just for now (nothing is erased)

1. Restart the player: hold **Menu** and **Select** together for about 8 seconds.
2. As soon as the screen goes dark, hold **Menu**.
3. Apple's own software starts for this session.

Restart again without holding anything to come back to ProkBox. Your music is untouched.

## 2. Remove ProkBox for good, keep your music

This puts the original start-up files back. It needs the `ProkBox-backup.bin` file you saved when you installed.

1. Connect the player.
2. In a terminal, run `ipodpatcher -w ProkBox-backup.bin`.
3. Eject and restart the player.

Apple's software then runs again with your files still on the drive, but its library looks empty until you sync with Apple's own tools. You can delete the ProkBox files from the drive afterwards (turn on "show hidden files" to see them).

> [!NOTE]
> If you have lost the backup file, use route 3 below instead.

## 3. Full restore (erases everything)

Use this if the player won't start at all.

1. Restart: hold **Menu** and **Select** for about 8 seconds.
2. Right away, hold **Select** and **Play** until Disk Mode appears.
3. Connect to a computer and use Apple's own restore tool:
   - **Mac:** Finder (macOS 10.15 or later).
   - **Windows:** iTunes, or the Apple Devices app.
4. Choose **Restore**. This erases the player and puts back Apple's software, formatted for that computer.

> [!WARNING]
> A full restore erases everything on the player. Back up your music first if you can.

> [!NOTE]
> Apple has no restore tool for Linux. Use a Mac or Windows computer for this route, or ask us for help at prokbox.project@gmail.com.

## Then what?

After a restore the player is stock. You can install ProkBox again whenever you like by following [Getting started](getting-started.md).
