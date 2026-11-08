---
title: "The ProkBox Reference"
description: "For developers: building, testing and changing ProkBox5g, and how the iPod Video's hardware works."
---

This is the developer reference for ProkBox5g, the free firmware for the iPod Video (5th generation and 5.5). It is for people who want to build the firmware, change it, write for it, or understand the hardware it runs on. If you only want to use ProkBox on your player, the [user guide](../user/guide/_index.md) is the place to start.

**This is the first edition, published with release 0.8.0.** It is incomplete, and it says so: each chapter is either finished, or marked **Draft** at the top with what has not been checked yet. Chapter numbers are fixed once published, so a link to "FW-18" will still point at testing in later editions, even as new chapters fill the gaps.

## The Parts

The reference is one volume in six Parts. Each Part has a two-letter prefix, which every chapter and section number carries: §FW-4.2 is section 2 of chapter 4 of Part V.

| Part | Prefix | Covers | In this edition |
|---|---|---|---|
| I · The 5.5G platform | `HB` | A tour of the board, then how the chips work together | Not yet. The data-path map, [`docs/data-paths.md`](../data-paths.md), is in the source tree |
| II · The PP5022 | `PP` | The main processor, its two ARM cores and the peripherals it drives | In the source tree, [`docs/pp5022-reference/`](../pp5022-reference/index.md); published edition later |
| III · The BCM2722 | `VC` | The video processor: host interface, instruction sets, display, TV out | In the source tree, [`docs/bcm2722-reference/`](../bcm2722-reference/index.md); published edition later |
| IV · The WM8758 | `WM` | The audio codec as wired in this player | An early draft in the source tree, [`docs/wm8758-reference/`](../wm8758-reference/README.md); published edition later |
| V · Writing firmware | `FW` | Building, testing and changing ProkBox | [Six chapters](fw/_index.md): the process and layout, toolchain, building, the cores, testing, contributing |
| VI · Writing games | `GS` | The games SDK | Not yet |

The hardware Parts link to the source tree because they are still being written to this reference's house style. Everything in them is already public in the source.

## Evidence tags

A statement about the hardware carries a tag saying how we know it. Much of what is known about this player was learnt by measurement and reverse engineering, not handed down in a datasheet, so the tag matters as much as the fact.

| Tag | Meaning |
|---|---|
| `[silicon]` | Measured on a real player with our own code |
| `[datasheet]` | From a published vendor datasheet or technical reference manual |
| `[apple-RE]` | Read from Apple's firmware by reverse engineering; addresses and constants only, never code |
| `[rockbox-derived]` | From Rockbox's source. A value Rockbox writes is known to work; the meaning Rockbox gives it is only as good as its comment |
| `[external]` | From community research, such as the iPodLinux project |
| `[hypothesis]` | Our inference, not yet proven |

Three more markers are not evidence: `[ours]` marks a convention our own firmware chose, `[sim]` a result from our simulator only, and `[estimate]` a figure computed rather than measured. The source-tree manuals still use some older spellings of these tags (`[apple]`, `[rockbox]`); each manual's index explains them.

Statements about ProkBox's own code carry no tag: the code is there to read.

## Models

Everything applies to both the 5G and the 5.5G unless it is marked "(5G only)" or "(5.5G only)".

## Reporting an error

If something here is wrong, or a step does not work as written, open an issue on [GitHub](https://github.com/prokbox/prokbox5g/issues) or write to prokbox.project@gmail.com. Say which section (for example §FW-4.2), what you did, what you expected and what happened.
