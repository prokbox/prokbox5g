---
title: "Changelog"
description: "Every change in each ProkBox5g release, built from the project history."
---

Every release, newest first. The [release notes](../releases/) say what matters most; this page lists what changed between versions. Numbers like #123 refer to the project tracker.

## 0.8.2

_Changes since 0.8.1._

### Interface

- Album covers no longer start over again when nothing has changed
- Your library is built first: covers and other background work wait until it has finished
- The library page says in plain words which step it is on, with a light that keeps moving
- Full speed is held while a high-quality track plays

### Playback and sound

- More FLAC files play: blocks of up to 65,535 samples are now supported
- A queue in which nothing can play now stops instead of cycling through it
- Shuffle no longer gets stuck on a very small library

## 0.8.1

_Changes since 0.8.0._

### Interface

- Leaving a video never shows "Preparing Video..." (#168)
- Heal cut cache files before rotating them; power-cut injection in the sim (#177)
- Hold during a video locks the keys, never the screen (#184)
- Contracts for the cover database format module and the synced-cover map (#225)
- No image-file search for tracks a sync app put in iPod_Control/Music (#225 S1)
- Reader for the stock firmware's cover database and song locations (#225 S3)
- The synced-cover map over the stock firmware's databases (#225 S3)
- Covers from the stock firmware's cover database as an art source (#225 S3)
- Shuffle record v2 counts each lap's songs
- Now Playing counts a library shuffle library-wide
- The cover reader accepts padded slots and wide strides (#225 S3)
- Synced covers by album key, a durable re-check, terminal bad images (#225 S3 review)
- Synced covers keep their padded slot and stride (#225 S3)
- Now Playing's cover takes a synced track's embedded art first (#225, owner)
- The cover map's build never waits holding its block (#225 budget review)
- Library: composers' and genres' albums from precomputed ranges (#230)
- Library: the group ranges fit a smaller block (#230)
- The library page: one component for the first build and the unplug (#237, #157)
- The library page holds a first build for power below 15 % (#237)
- The unplug page pushes nothing before it shows; no boost while held for power (#157, #237)
- Every art pass boosted, never in a wait or a gap
- Library page gives its build boost back for sleep and USB (p081-pageboost)

### Playback and sound

- Mp4 chapter count from the first parse, no second walk

### Library

- Read-only library progress for the library page (#237, #157)
- A first build waits for power below 15 %; a cut one is not built twice (#237)
- USB during a first build costs one build again, not two (#237)
- A first build walked whole settles the post-USB check (#237)
- No boot walk for a slow digest; a first build leaves a stamp
- The library walk turns the lookup cache on (#243)
- A damaged library is still a library: no first-build page (#237)
- Commit trace: time and storage commands per commit stage (#245)
- The deleted-file pass looks up folders through the walk cache (#243)
- Commit in big chunks, dedupe by hash, no quadratic reverse lookup (#245)
- Free a retired generation's RAM cache; a commit that misses the floor logs its wait
- Boot trace of the library's boot steps (p081-bootload)
- Crcfast, CRC-32 four bytes a step (p081-bootload)
- Boot without waiting for the library's CRC pass; RAM load in big reads (p081-bootload)

### Video

- Vpc_stop() reads the load timings only while the image runs
- A video's start no longer stalls or seeks on passing audio positions (#218)
- Log lines put their numbers before the legend (#224)
- The #218 piece-from-file log line fits #224's slot rule
- Bcm_port_unlock puts the tick block back as it found it

### Storage and USB

- Opendir_child(), a subfolder opened from its parent's entry
- Directory-entry generation for the walk lookup cache (#243)
- Walk lookup cache for folders, on while a walk runs (#243)
- The chain guard moves out of the shell's front end (apps/chaincheck.c)

### Power and start-up

- Directory burst slots 2 -> 8 (#225 S2)

Releases before 0.8.1: see their [release notes](../releases/).
