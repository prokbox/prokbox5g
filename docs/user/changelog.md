---
title: "Changelog"
description: "Every change in each ProkBox5g release, built from the project history."
---

Every release, newest first. The [release notes](../releases/) say what matters most; this page lists what changed between versions. Numbers like #123 refer to the project tracker.

## 0.9.0

_Changes since 0.8.3._

### Interface

- Japanese text breaks between lines in the right places
- Now Playing artist and album lines scroll too, one line at a time (#180)
- Settings gains Audiophile (88.2 and 96 kHz output, stereo width) and Now Playing gains a format and output page
- Song, album and artist names in Japanese, Chinese and Korean now show their characters in every list and on Now Playing
- Japanese and Korean names now sort in their own order (kana by the Japanese syllable table, Hangul by its letters) after A to Z, with numbers and symbols last
- Songs and audiobooks that can't play are greyed out with a small sign
- Chinese song, artist and album names now show their characters instead of blanks
- Custom EQ: edit your own 5-band curve from Settings > EQ or Extras
- Songs and audiobooks that can't play show a slightly larger can't-play sign, centred on the row's text
- Custom EQ: the Bass and Treble handles now sit at the level you set
- Settings > Audiophile has a new Hi-Res 128x switch that runs the audio chip's filter at its full rate for music at 88.2 kHz and up (off by default; a short test runs the first time)
- At 88.2 and 96 kHz, EQ presets now pause (the EQ caption says so) instead of sounding an octave too bright; custom EQs still apply
- Choosing a song, audiobook or video that can't play now shows a short page saying why (copy-protected, or a format that isn't supported); what was playing keeps playing
- Search, On-The-Go and Podcasts grey out items that can't play and say why when you select one; the hold menu no longer offers to add them to On-The-Go
- Settings > Language switches between English, Deutsch and Nederlands at once
- Menus in German and Dutch (first drafts)
- Menus in Japanese (first draft)
- Menus in German, Dutch and Japanese now cover every 0.9.0 screen (first drafts, awaiting review)
- Episodes that can't play no longer show the blue "new" dot
- Japanese UI wording reviewed for 0.9.0
- German UI wording reviewed for 0.9.0
- Dutch UI wording reviewed for 0.9.0
- Dates, times and decimal numbers now follow the chosen language, and German, Dutch and Japanese start on the 24-hour clock
- The Now Playing screen's title always matches its menu item
- Settings rows with a value next to the name keep long translated names whole more often
- The custom equaliser's band names no longer run into each other in German and Dutch
- Japanese text wraps correctly on the battery question and in empty Notes and Contacts
- Long messages are no longer cut off after three lines, and long headings wrap instead of running off the screen
- The first-start notice fits on one screen in every language
- Shuffle Songs on a small library now picks every order with equal chance
- Shuffle Songs starts playing at once, even on a large library; the rest of the shuffle is built while the first song plays
- Fixed a stray "tracks added" message that could appear when a song list failed to start playing
- A damaged cover picture can no longer corrupt memory when its album plays
- A damaged cover inside an Ogg or Opus file no longer leaves the player stuck working on it
- Settings > Legal now credits the font used for Japanese, Korean and Chinese characters
- Fixes a false "no progress" record on the first start after choosing Japanese
- Changing the language now stops the music and returns to the main menu, as on the classic iPod

### Playback and sound

- The equaliser and other sound processing now run on the second core, with the decoding
- Songs with several artists show every artist on Now Playing; ID3 genres such as Audiobook and Podcast (numbers 148 to 191) are recognised
- 32-bit FLAC files up to 48 kHz now play instead of being skipped
- Songs at different sample rates or bit depths now follow each other without a gap, and MP3 and AAC play through the 24-bit output
- Opening a screen while music plays no longer makes the song stop and start again
- The library reads Sort Artist, Sort Album and Sort Album Artist tags; protected purchases, Audible books and files in formats the iPod can't play are recognised
- Turning on an EQ preset no longer makes your music quieter, and Settings > EQ has three custom EQ slots
- Skipping between hi-res and standard-rate songs now switches the output rate properly
- Pausing now stops the sound straight away, even while the player is busy reading its storage
- Shuffle Songs no longer re-reads the songs it already loaded each time it adds more, and changing the shuffle mode answers at once
- Fixed some MP3s playing as silence when their encoder delay is exactly one or two frames long
- After a seamless change of sample rate, the next song plays at its own speed
- A stop during a change of sample rate no longer leaves the player waiting for a change that never comes
- Fewer short drop-outs in demanding files while the second core tidies its memory
- The audio chip is powered at 3.0 V, as in the original software, for a little more headroom
- The Hi-Res 128x test plays a few seconds at each rate, including a near-silent stretch, and fails on the smallest break
- Songs and audiobooks whose tags sit after a damaged or large-size section now show their details and chapters
- A FLAC file with a damaged cover picture no longer crashes the player
- WAV files with damaged or very long text tags no longer crash the player
- AIFF files now show tags stored before the audio, and long AIFF text tags are cut safely
- Game music (VGM) files with long Japanese titles now show the right game and composer
- Cover art in MP3 files tagged by Windows Media Player and similar tools now shows correctly
- A damaged M4A file no longer stalls the library build or the next song
- A FLAC file with damaged header blocks is now skipped instead of crashing the player
- A FLAC song with a damaged spot now skips past it and plays on instead of stopping
- A FLAC song with a damaged picture inside now plays, without its cover, instead of being skipped
- FLAC songs with several pictures inside now show the front cover
- A damaged Apple Lossless file now stops the track instead of writing past the decoder's memory
- 24-bit Apple Lossless files now play at their full 24-bit precision; before, the lowest 8 bits were lost
- With Hi-Res 128x on, music at 88.2 and 96 kHz now runs the audio chip's sound processing at full speed, which removes a faint crackle in quiet endings; the audio chip's limiter now guards the equaliser against sudden peaks

### Library

- The library is rebuilt once after updating to 0.9.0; play counts, ratings and resume points are kept
- Artists and albums sort by their Sort Artist and Sort Album tags, and Japanese names sort in kana order when they have a reading
- After a software update that needs the library rebuilt, the player now shows the "Preparing Your Library" page with its progress bar instead of empty menus
- Pressing Play right after a library update no longer leaves a smaller audio buffer for the rest of the session

### Video

- A rare video-chip hiccup when switching between album art and the visualiser is fixed

### Other changes

- A video's sound is now decoded on the second core, like music
- Every sound, including the first moments of a song or a click, is now produced on the second core
- Opus and other formats use less processor time while playing
- Opus files play to their exact end (for gapless albums), very short Opus files play, and Sound Check uses Opus loudness tags
- A 24-bit song now gathers its full head start before it plays, as 16-bit songs do
- Hi-Res's audio-chip check starts afresh with each track, so a quick track change can't be mistaken for a fault
- The download now includes the Japanese fonts, so Japanese menus are offered straight away
- Fixed: a file's latest change could be lost when its folder entry sat in the last slot of the disk cache
- M4A songs and M4B audiobooks made by ffmpeg with a large-size audio section now play
- M4A and M4B files recorded on Android phones now play correctly
- Seeking and resuming in audiobooks longer than 27 hours now land in the right place
- A WAV or AIFF file claiming fewer than 8 bits a sample no longer restarts the player
- Damaged or very large game music (VGM/VGZ) files no longer crash the player
- FLAC files encoded with variable block sizes now show the right time and seek correctly
- Damaged FLAC files are handled safely instead of risking a crash
- Fixes a crash when opening a song after changing the language while music played

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
