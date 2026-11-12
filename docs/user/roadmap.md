---
title: "What's coming"
description: "What we are working on for ProkBox5g, release by release, and what has changed."
roadmap_version: "0.8.2"
page_class: "roadmap"
---

This page lists what we plan for each coming release of ProkBox5g, and where each item stands. It is a plan, not a promise: items can move, and we say so here when they do. Finished work leaves this page when its release comes out, and appears in the [changelog](../changelog/).

## Release plan

| Release | What it brings | Where it stands | Items under way |
|---|---|---|---|
| [0.8.3](#release-083) | A new playback engine, one download for every player, and fixes. | _Under way_ | 15 of 17 |
| [0.9.0](#release-090) | Sound and languages: better audio, menus in more languages, better song details, and fixes. | _Under way_ | 11 of 20 |
| [0.9.1](#release-091) | Album art, Album Flow and other screen improvements. | _Under way_ | 4 of 7 |
| [0.9.2](#release-092) | Sleep and battery life, and the first kit for making your own games, with a Sudoku game. | _Under way_ | 3 of 6 |
| [0.9.3](#release-093) | Video: more formats, a better video player, and fixes. | _Under way_ | 5 of 13 |
| [0.9.4](#release-094) | Your library and USB, and the second game kit, with a short role-playing game. | _Under way_ | 1 of 13 |
| [0.9.5](#release-095) | Docks, cars and connections. | _Under way_ | 1 of 5 |
| [0.9.6](#release-096) | Extras, games and more ways to shape your sound, and the third game kit, with an arcade racing game. | _Under way_ | 1 of 6 |
| [0.9.7](#release-097) | Polish: a careful clean-up of the whole player, making everything as lean and refined as we can, with no new features. | _Planned_ | - |
| [0.9.8](#release-098) | Interoperability Mode, and the last test version before 1.0. | _Planned_ | 0 of 2 |
| [1.0](#release-10) | Independence: our own start-up software, Disk Mode, recovery and an installer, so you can fully replace the original software. | _Planned_ | 0 of 3 |
| [Later](#later) | Ideas for later, in no fixed order. | _Under way_ | 1 of 7 |

**Where it stands** says *Planned* until work on any of the release's items has started, *Under way* after that, and *Done* when every item is finished. **Items under way** counts the items that are in design, in development, in testing or done.

Each item has a status: **Considering** (an idea we are looking at, not designed yet), **Planned** (agreed, not started), **In design** (working out how it should look and behave), **In development** (being built), **In testing** (built, being checked), **Done** (finished, arrives with that release), **Postponed** and **Cancelled** (each with a short reason).

> [!NOTE] Words on this page
>
> - **Second processor core**: the player has two processor cores. Giving the music to the second one keeps the menus smooth on the first.
> - **Video chip**: a separate chip in the player that is good at drawing and handling pictures. Using it takes work off the main processor.
> - **Start-up software**: the small program that starts the player before anything else runs (often called a bootloader).
> - **Disk Mode**: a mode in which the player shows up on your computer as a drive, even when its main software cannot start.
> - **Sample rate**: how many times a second the sound is measured, such as 44.1 kHz or 96 kHz.
> - **Bit depth**: how finely each of those measurements is stored, such as 16-bit or 24-bit.

## Release 0.8.3

A new playback engine, one download for every player, and fixes.

- _In testing:_ Smoother playback of demanding files, and less battery used on easy ones: the player now watches both of its processor cores to choose its speed. [](#item-speed-matched-to-how-hard-the-music-is)
- _In testing:_ A rare fault with very loud 24-bit FLAC files is fixed: they play correctly. [](#item-very-loud-24-bit-flac-fixed)
- _In testing:_ 32-bit FLAC files are skipped cleanly instead of playing back wrongly. They stay in your library. [](#item-32-bit-flac-skipped-cleanly)
- _In testing:_ Skipping a song the player cannot play no longer leaves a short silence. [](#item-no-silence-when-skipping-unplayable-songs)
- _In development:_ A new playback engine: all of the audio work moves to the player's second processor core, so the menus stay smooth while music plays. [](#item-new-playback-engine-on-the-second-core)
- _In development:_ 24-bit, 192 kHz FLAC files play without drop-outs. [](#item-24-bit-192-khz-flac-without-drop-outs)
- _In development:_ One download for every player: original or modded, hard drive or flash, 32 or 64 MB. It asks you once which battery your player has. [](#item-one-download-for-every-player)
- _In development:_ Dock support is built into every download, and stays idle until you connect a dock. Work on how docks behave comes in 0.9.5. [](#item-dock-support-in-every-download)
- _In development:_ Cleaner sound for songs at sample rates the player has to convert: it always uses its best-quality converter. [](#item-best-quality-sample-rate-converter-always)
- _In development:_ The video chip does the drawing for the visualiser and album art, which leaves the main processor free. [](#item-video-chip-draws-the-visualiser-and-art)
- _In development:_ Turning Hold off during a video no longer blanks the screen for a few seconds. [](#item-no-blank-screen-after-hold-in-a-video)
- _In development:_ All of the equaliser's bands will work: one of the upper bands has no effect today. The fix ships once we have checked its battery cost. [](#item-every-equaliser-band-works)
- _In development:_ Button presses no longer keep the player at full speed for longer than needed, which saves battery. [](#item-buttons-no-longer-hold-full-speed)
- _In development:_ The visualiser always opens while music plays. Today it sometimes fails to. [](#item-visualiser-always-opens)
- _In development:_ 24-bit playback: 24-bit FLAC and WAV files play at their full depth. [](#item-24-bit-playback)
- _Planned:_ Turning the wheel wakes the player from the clock screen, like a button press, without scrolling or changing the volume. [](#item-wheel-wakes-the-clock-screen)
- _Planned:_ The visualiser runs smoothly, without the stutter you may see with MP3 files. [](#item-smooth-visualiser-with-mp3-files)

## Release 0.9.0

Sound and languages: better audio, menus in more languages, better song details, and fixes.

- _In testing:_ On Now Playing, a long artist name scrolls so you can read all of it. [](#item-long-artist-names-scroll-on-now-playing)
- _In testing:_ Files tagged with the newer numbered genres, such as Audiobook and Podcast, show the right genre. [](#item-newer-numbered-genres-show-correctly)
- _In testing:_ Now Playing shows every artist on a song, not just the first. Browsing by artist still uses the first artist. [](#item-every-artist-on-now-playing)
- _In development:_ A new Settings › Audiophile menu with optional 88.2 and 96 kHz output. It is off by default. It runs the audio chip beyond its official range, so it comes with a warning. Your player tests itself first and offers only the rates that pass. If anything goes wrong, it drops back to a safe rate. [](#item-optional-88-2-and-96-khz-output)
- _In development:_ An extra info page in Now Playing shows a song's format, bit depth and sample rate, for example FLAC, 24-bit, 96 kHz. [](#item-format-and-sample-rate-page-in-now-playing)
- _In development:_ Menus in Japanese. [](#item-menus-in-japanese)
- _In development:_ Japanese and Korean characters show correctly in song, artist and album names. [](#item-japanese-and-korean-characters-in-names)
- _In development:_ Lists sort by the sort-name tags, such as Sort Artist, when your files have them. [](#item-sorting-by-sort-artist-and-other-sort-tags)
- _In design:_ We are working on keeping the volume the same when you turn on an equaliser preset. Today presets make everything quieter. [](#item-equaliser-presets-keep-the-volume)
- _In design:_ Songs the player cannot play stay in your library, greyed out with a sign, and are skipped instead of failing without explanation. [](#item-unplayable-songs-greyed-out-and-skipped)
- _In design:_ Video soundtracks and click sounds move to the second processor core too, like music. [](#item-video-sound-and-clicks-on-the-second-core)
- _Planned:_ MP3 and AAC files use the 24-bit output too, keeping more of their detail. [](#item-24-bit-output-for-mp3-and-aac)
- _Planned:_ We are working on switching between songs with different sample rates with at most a very short pause. [](#item-near-seamless-sample-rate-changes)
- _Planned:_ Menus in Dutch and German. [](#item-menus-in-dutch-and-german)
- _Planned:_ We are working on better Opus support: loudness tags for volume levelling, and Opus files that end in .ogg. We are also checking that gapless endings are clean. [](#item-better-opus-support)
- _Planned:_ 32-bit FLAC files play, instead of being skipped. [](#item-32-bit-flac-files-play)
- _Planned:_ A new guide page on putting audiobooks on the player: which formats play, chapters, resume and speed, and why copy-protected books cannot play. [](#item-guide-page-on-adding-audiobooks)
- _Planned:_ Opening some screens while music plays no longer stops the music. [](#item-music-keeps-playing-when-opening-screens)
- _Planned:_ Make up to three equaliser settings of your own, with a picture of each curve, alongside the built-in ones. [](#item-your-own-equaliser-settings)
- _Considering:_ We are looking at letting the sound chip mute itself during silent stretches, to cut faint hiss in quiet passages and while paused. [](#item-quieter-silence)

## Release 0.9.1

Album art, Album Flow and other screen improvements.

- _In testing:_ A small E badge marks explicit songs and albums. [](#item-badge-for-explicit-songs)
- _In design:_ Album Flow: flip through your albums by their cover art, from the first row of the Music menu. [](#item-album-flow)
- _In design:_ The video chip does the work of preparing album art, instead of the main processor, so songs start quickly even while art is being built. [](#item-video-chip-prepares-album-art)
- _In design:_ The Now Playing row on the main menu shows sharp album art more often, instead of a blurry small copy. [](#item-sharper-art-on-the-main-menu)
- _Planned:_ In the visualiser, the volume bar slides up and looks like the one in the video player. [](#item-visualiser-volume-bar-like-video-s)
- _Planned:_ Damaged cover images no longer show as garbled pictures. [](#item-no-garbled-damaged-covers)
- _Considering:_ We are looking at showing covers saved as progressive JPEGs, which show no art today. [](#item-progressive-jpeg-covers)

## Release 0.9.2

Sleep and battery life, and the first kit for making your own games, with a Sudoku game.

- _In testing:_ Holding the play button puts the player to sleep at every backlight setting. [](#item-hold-play-to-sleep-at-every-setting)
- _In development:_ Sleep and instant wake, like the original software, so the player rarely needs a full start-up. [](#item-sleep-and-instant-wake)
- _In design:_ The player raises and lowers its speed and power to match what you are doing, to save battery. [](#item-speed-and-power-matched-to-what-you-do)
- _Planned:_ When the player does a full start-up, the start screen shows a progress bar. [](#item-progress-bar-at-full-start-up)
- _Planned:_ The player can switch itself off when idle, even while plugged into a weak USB power source such as a car charger. [](#item-switch-off-when-idle-on-weak-usb-power)
- _Planned:_ The first kit for making your own small games and apps, with a guide. It comes with an original Sudoku game built with the kit. [](#item-first-game-kit-with-sudoku)

## Release 0.9.3

Video: more formats, a better video player, and fixes.

- _In testing:_ Subtitles for widescreen videos, from the video file or a .srt file beside it. Turn them on in Videos › Settings. [](#item-subtitles-for-widescreen-videos)
- _In testing:_ While you watch a widescreen video, a black bar with the title, time and battery slides down from the top along with the progress bar. [](#item-title-and-battery-bar-over-widescreen-video)
- _In testing:_ A simple, documented way to give your videos posters, by placing picture files beside them. [](#item-video-posters-from-picture-files)
- _In testing:_ No white flash when you wake the screen while a video is paused: the screen stays dark until the picture is ready. [](#item-no-white-flash-waking-a-paused-video)
- _In development:_ MPEG-4 videos (up to 320 × 240) play, as well as H.264. These are the two video formats the original player supported. [](#item-mpeg-4-video-up-to-320-240)
- _Planned:_ AVI video files play, when the video inside is a kind the player can decode. [](#item-avi-video-files)
- _Planned:_ MKV and WebM video files play, once the player can decode the kinds of video they usually hold. [](#item-mkv-and-webm-video-files)
- _Planned:_ A clearer message when a video is too large to play: it says what is wrong and how to convert it. [](#item-clearer-message-for-too-large-videos)
- _Planned:_ Some videos no longer start with glitches and a pause of a few seconds. [](#item-clean-video-starts-no-glitches)
- _Planned:_ The volume and progress bars respond straight away while a video plays. [](#item-quick-volume-and-progress-bars-in-video)
- _Considering:_ We are looking at playing MPEG-4 videos larger than 320 × 240. [](#item-mpeg-4-video-larger-than-320-240)
- _Considering:_ We are looking at playing MPEG-1 and MPEG-2 videos (.mpg files). [](#item-mpeg-1-and-mpeg-2-video)
- _Considering:_ We are looking at H.265 (HEVC) video, as an experiment. It ships only if it works well, and is marked experimental if it does. [](#item-h-265-video-as-an-experiment)

## Release 0.9.4

Your library and USB, and the second game kit, with a short role-playing game.

- _In design:_ We are working on making library updates quick after you add a few songs, even for a big library. [](#item-quick-library-updates-after-small-changes)
- _Planned:_ The player shows up on a Mac by itself when you plug it in, with no extra software. [](#item-shows-up-on-a-mac-without-extra-software)
- _Planned:_ Podcast episodes are ordered by their release date, even when you copied them all at once. [](#item-podcasts-ordered-by-release-date)
- _Planned:_ A genre's list of artists opens faster. [](#item-faster-genre-artist-lists)
- _Planned:_ On the first library build, hold the centre button for 5 seconds to skip waiting and use the menus while it finishes. [](#item-use-the-menus-during-the-first-library-build)
- _Planned:_ An optional tool for your computer that builds the library before the player's first start, for very large collections. [](#item-library-builder-for-your-computer)
- _Planned:_ We are working on keeping play counts, resume points and tags when you move or rename files. [](#item-keep-play-counts-when-files-move)
- _Planned:_ We are checking whether songs waiting in your computer's Bin or Trash still show in the library. If they do, we will fix it. [](#item-songs-in-the-bin-or-trash)
- _Planned:_ The second game kit, for bigger games. It comes with an original short role-playing game built with the kit. [](#item-second-game-kit-with-a-role-playing-game)
- _Planned:_ Long files such as DJ mixes and podcasts show their sections: the current section is named on Now Playing, and next and previous move between sections. [](#item-sections-in-dj-mixes-and-podcasts)
- _Planned:_ Spoken menus: the player can read out menu items and song, artist and album names. [](#item-spoken-menus)
- _Considering:_ We are looking at automatic Top 25 Most Played and Recently Played playlists. [](#item-top-25-and-recently-played-playlists)
- _Considering:_ We are looking at showing lyrics from a .lrc file next to a song, as plain text on Now Playing. [](#item-lyrics-from-lrc-files)

## Release 0.9.5

Docks, cars and connections.

- _In development:_ We are working on dock support that behaves like the original player's, with a fixed line-out level and no volume swings through a dock. [](#item-docks-that-behave-like-the-original-s)
- _Planned:_ We are working on making car stereos that play music from a USB drive work with the player too. [](#item-car-stereos-that-read-usb-drives)
- _Planned:_ We are working on digital audio over USB: first to a computer, later to cars and docks that accept it. [](#item-digital-audio-over-usb)
- _Planned:_ We are checking that a simple USB-C headphone adapter works on modded players whose USB-C port carries audio. Adapters with their own sound chip won't work. [](#item-usb-c-headphone-adapters)
- _Planned:_ While connected to a computer, the screen stays lit. On a charger alone, the player can still sleep. [](#item-screen-stays-lit-while-connected)

## Release 0.9.6

Extras, games and more ways to shape your sound, and the third game kit, with an arcade racing game.

- _In development:_ Extras: clocks, alarms, sleep timer, stopwatch, screen lock, notes, contacts, calendars and games. [](#item-extras-clocks-alarms-notes-and-more)
- _Planned:_ For game makers: building blocks and tools for drawing with the video chip, with a guide. [](#item-drawing-tools-for-game-makers)
- _Planned:_ The third game kit, for games that take over the whole player, the way video does. It comes with an original arcade racing game built with the kit. [](#item-third-game-kit-with-a-racing-game)
- _Considering:_ We are looking at automatic mixes, made on the player from your own music by genre and by songs that sound alike. [](#item-automatic-mixes-from-your-music)
- _Considering:_ We are looking at playing DSD files (.dsf and .dff) by converting them to standard audio as they play. [](#item-dsd-files)
- _Considering:_ We are looking at a rhythm game: tap the wheel in time with the music, using step-chart files. [](#item-a-rhythm-game)

## Release 0.9.7

Polish: a careful clean-up of the whole player, making everything as lean and refined as we can, with no new features.

Nothing is listed here yet.

## Release 0.9.8

Interoperability Mode, and the last test version before 1.0.

- _Planned:_ Interoperability Mode: an optional mode that keeps your library in the format the original software uses, so songs and covers you add with ProkBox5g show up there too. It is off by default, warns you before it changes anything, and offers to back up the original library first. [](#item-interoperability-mode)
- _Planned:_ A page showing where ProkBox5g has improved on Rockbox in ways that could be offered back to the Rockbox project. [](#item-improvements-that-could-go-back-to-rockbox)

## Release 1.0

Independence: our own start-up software, Disk Mode, recovery and an installer, so you can fully replace the original software.

- _Planned:_ Our own start-up software, so your player no longer needs the original software at all. It comes only after our own recovery is proven to work. [](#item-our-own-start-up-software)
- _Planned:_ Our own Disk Mode and recovery: hold a button combination as the player starts, or let it start this way after a failed start, and copy working software back from your computer. [](#item-our-own-disk-mode-and-recovery)
- _Planned:_ An installer for Mac, Windows and Linux that installs, updates, removes and recovers ProkBox5g, and backs up your player first. [](#item-an-installer-for-every-computer)

## Later

Ideas for later, in no fixed order.

- _In design:_ In long lists, when you stop the wheel on a letter while scrolling fast, the list glides to the first item for that letter. [](#item-lists-glide-to-the-letter-you-stop-on)
- _Planned:_ Spinning arrows on the screen while your computer copies music to the player. [](#item-spinning-arrows-while-copying-music)
- _Considering:_ We are looking at an optional dark look for the whole player, modelled on the dark screens of the original. [](#item-a-dark-look)
- _Considering:_ We are looking at a companion app, or an add-on for an existing music app, to copy music, podcasts and audiobooks to the player easily. [](#item-a-companion-app)
- _Considering:_ We are looking at making MP3 and AAC playback faster, which would save battery. [](#item-faster-mp3-and-aac-playback)
- _Considering:_ We are looking at languages as separate packs you can add, and more alphabets, such as Greek, Cyrillic and right-to-left scripts. [](#item-language-packs-and-more-alphabets)
- _Considering:_ We are looking at starting the hard drive just before you need it, so browsing waits less on hard-drive players. [](#item-hard-drive-starts-before-you-need-it)

## Not planned

These are things we have decided not to build, each with the reason. It is a list of decisions, not a list of gaps: if the reason changes, we will say so here.

- **Skins and themes.** Letting you restyle the player with skins or themes would mean rebuilding that whole system, which is prohibitively difficult when the interface is tuned so closely to the look and behaviour of the original player. [](#notplanned-skins-and-themes)
- **Choosing the output sample rate.** The player always picks the best rate for each file itself, so there is no setting to get wrong. This does not affect the optional high-rate output planned under Audiophile in Settings. [](#notplanned-output-sample-rate)
- **Unlocking copy-protected files.** We will not decrypt copy-protected audiobooks or songs, such as Audible files or protected AAC. They show greyed out in your library, and a guide page (planned) explains your options. [](#notplanned-copy-protected-files)
- **Plugins from other firmware.** Existing plugins will not run here as they are. Games and apps come through our own game kits, which run in a sandbox so a faulty one cannot harm the player. [](#notplanned-plugins-as-they-are)
- **System-level settings menus.** Settings stays a short, curated list in the original style. Options that change how the player works underneath are not shown. [](#notplanned-system-settings-menus)
- **Other iPod models.** ProkBox5g is tuned only for the iPod Video (5th generation and 5.5G). Support for other players would be a separate project, not a setting in this firmware. [](#notplanned-other-ipod-models)
