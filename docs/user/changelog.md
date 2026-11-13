---
title: "Changelog"
description: "Every change in each ProkBox5g release, built from the project history."
---

Every release, newest first. The [release notes](../releases/) say what matters most; this page lists what changed between versions. Numbers like #123 refer to the project tracker.

## 0.8.3

_Changes since 0.8.2._

### Interface

- Touching the wheel wakes the screen when it shows the clock face
- Alarms: the alarm clock comes back on the right when you return to the Alarms list
- Alarms: with the Sleep Timer running, its row shows the minutes left under the alarm clock
- Alarms shows when each alarm next goes off and how long until then, and "Sleep Timer Active" with the minutes left
- Cover art builds faster for music with embedded artwork
- Waking the player from a dark screen with the clock face takes one press; the next press works at once
- Fixed: the main menu no longer flashes a Now Playing row for a video's sound as the video starts
- A first library build that finishes during the boot screen no longer flashes the library page

### Playback and sound

- FLAC files decode faster, most of all high-quality 24-bit ones
- All music is now decoded on the player's second processor
- FLAC, WAV and AIFF files recorded with more than 16 bits play at their full depth
- Better-quality conversion for music that is not at 44.1 or 48 kHz
- Some high-quality FLAC files that could play wrongly now play correctly
- FLAC files wider than 29 bits are skipped cleanly instead of playing wrongly
- Opus files need fewer hand-overs between the two processors when they start
- When a song cannot be played, the next one now starts straight away
- The EQ presets' upper-mid band (around 2-4 kHz) now takes effect
- Pressing Play/Pause now shows the change at once, even while the next song is loading

### Library

- After you unplug from a computer that changed nothing, the busy sign in the header no longer spins for two seconds
- Fixed a library update that could wait until power-off when it began while album art was being built

### Video

- A video that cannot start stops the sound and shows a message instead of a blank screen

### Storage and USB

- One build for flash and hard-drive players: the player finds out which it has
- After you unplug from a computer that only read your music, the library is ready at once; when songs were added, the progress bar shows and moves straight away
- Building album art no longer re-reads the whole Music folder for every cover it looks for

### Power and start-up

- The player watches both of its processors when it chooses its speed
- The first start asks which battery is fitted

### Other changes

- Every player now uses the same download; the first start asks which battery is fitted
- A library update that had to wait while music was paused now finishes in the same pause once memory frees up, instead of waiting for playback to stop
- After watching a video, album art and the visualiser use the video chip again

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
