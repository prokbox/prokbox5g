# ProkBox5g 0.8.0: known hardware

*Draft for the release page and the guide. It says plainly what has been tried and what has not.*

> ProkBox5g is not Apple software. It is an independent, community-driven fork of Rockbox, and Apple Inc. has not made, approved, endorsed or supported it in any way. It contains no Apple software. iPod is a trademark of Apple Inc.

0.8.0 is a pre-release. It was built for the iPod Video (5th generation and 5.5), but so far it has been used on exactly one player. Here is what that means for you.

## Tested

One player, used daily for the last stretch of development:

| | |
|---|---|
| Model | iPod Video 5.5G |
| Memory | 64 MB |
| Storage | flash (1 TB) in place of the hard drive |
| Battery | 3000 mAh replacement |
| Connector | USB-C in place of the 30-pin dock |

Everything in the release notes was tried on that player. [pending bench] The final checks on it for 0.8.0 are still running; this page will be updated with their outcome before release.

## Untested

These should work, because the firmware detects the memory size when it starts and the zips are built per storage type, but nobody has run 0.8.0 on them yet:

- **Original hard drives** (30, 60 or 80 GB models). Spin-up, the read-ahead pattern and the battery figures are all designed for a drive, but not yet proven on one.
- **32 MB players** (the 30 GB models). The music buffer is smaller, so it refills more often. Smoothness while browsing and playing at the same time is the thing to watch.
- **Original batteries.** The charge gauge uses the original cell's curve. It has not been compared against a real original battery, so the percentage may be off.
- **The 30-pin dock connector.** Builds for players that keep the dock are not proven. Fixes for crashes in those builds are open (#135) and are not part of 0.8.0 unless that issue is closed first. [pending decision]
- **Haptic clicker versus the original piezo.** Only the haptic mod has been heard and felt.

If your player is on this list, you are exactly who we want to hear from.

## Which zip is proven

There are eight zips, named `ProkBox5g-0.8.0-{stock,modded}-{flash,hdd}-{32,64}MB.zip`. Only one matches the tested player.

| Zip | Status |
|---|---|
| `modded-flash-64MB` | **Proven** on real hardware |
| `modded-flash-32MB` | Unverified (built the same way; the memory size differs) |
| `modded-hdd-64MB`, `modded-hdd-32MB` | Unverified |
| `stock-flash-64MB`, `stock-flash-32MB` | Unverified |
| `stock-hdd-64MB`, `stock-hdd-32MB` | Unverified |

All eight are built from the same source by the same script, and each is checked to carry the right version and no developer strings. That tells you the file is well formed. It does not tell you the firmware behaves well on your hardware.

[pending owner decision] Either all eight zips are published, with the table above on the download page, or only the proven one is. If only one is published, the other seven are simply not offered, and this page then reads "everything else is untested" without the table. The rest of this page holds either way.

## What you can report

Anything. A report that says "it worked" is as useful as one that says "it broke":

- it installed and started, or did not;
- music, audiobooks, USB copying and video behaved, or did not;
- how long the battery lasted, roughly;
- anything that looked wrong, felt slow or surprised you.

## How to report

1. **Save Diagnostics.** On the player choose Settings › About, go to the last page and choose **Save Diagnostics**. It writes `ProkBox Report.txt` to the top level of the player's drive. It holds the version, hardware details, counts and a short log, and no music or file names.
2. **Use an issue form** on the project's public page: the *Pre-release tester report* form for "here is how it went", or the *Bug report* form for something that went wrong. Attach the report file.
3. No account? Email **prokbox.project@gmail.com** with the file attached.

Please say which zip you used and what your player is: model, memory, hard drive or flash, and whether the battery and connector are original.

## Going back

Apple's own software stays on the player. Hold **Menu** while it starts to reach it. The guide chapter "Going back to Apple's firmware" has the full steps. Back up your music before you install.
