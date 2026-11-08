# Videos: menus, posters, the video screen, subtitles

*Plan of record for Videos after 0.8.0. It gathers the owner's decisions of 2026-11-06 (and the earlier #154 rules) in one place. Each section names its tracker issue; the issue holds status and discussion, this page holds the agreed design. Where this page and an issue disagree, this page wins, and the issue gets a correction comment.*

The 7G is the model (see [index.md](index.md)). Where the 7G did not do something (Blu-ray subtitles, Plex names), the design follows the same idiom: automatic behaviour, very few settings, no app required.

## 1. Menu

```
Videos
  Movies            (only if any)
  TV Shows          (only if any)
  Music Videos      (only if any)
  Video Podcasts    (only if any; see Podcasts)
  Settings          (always last)
```

- **Videos › Settings** holds every video setting. Nothing about video goes in the main Settings menu. The 7G also kept its video settings inside Videos.
- Contents (subtitles first, more added only when a feature needs one):
  - **Subtitles:** On / Off.
  - **Subtitle Font:** Standard / Monospaced (see 4.4).
  - **Subtitle Size:** a few fixed steps.
- **Curated, not customisable:** the shell uses one font everywhere, as the original iPods did. The only font choice in the product is the subtitle pair above.

## 2. Play and resume (#154, in 0.8.0)

- Play, resume and Now Playing are **audio only** (music, audiobooks, audio podcasts). A video starts only from the Videos menu and never plays in the background.
- The screen stays on while a video plays. While it's paused, the normal backlight timeout applies.
- **The resume position is always kept.** About half a second into any pause it's written, so it survives backlight-off, Hold, sleep, a low battery and a power cut. Over USB it's held in RAM and written after unplug, never as the USB screen opens. Open bug: the spot reads 0 after a USB session (#165).
- **Leaving a video** (MENU and the like) goes straight back to the list with the normal slide, and never shows "Preparing video…" on the way out (#168, before 1.0).

## 3. Posters and art (#173)

### 3.1 The rule: plain drag and drop

The folder and file-name convention below is **the contract**. It must work with nothing but a computer's file manager. The converter (`scripts/prokbox-convert.py`) and any later lookup (Plex, TMDB/TheTVDB, opt-in) are **optional helpers** that write exactly these files. A poster embedded in a video (`covr`) is one accepted input, never a requirement. The user guide shows the folder layout first and mentions the helper second.

### 3.2 Layouts

```
Movies/Film Name.m4v
Movies/Film Name.jpg                    the movie's poster (same name as the video)

Movies/Film Name/Film Name.m4v          or one folder per movie
Movies/Film Name/poster.jpg             that folder's poster

TV Shows/Show/poster.jpg                the show's art, used for every season and episode
TV Shows/Show/Season 4/poster.jpg       optional season art (replaces the show's)
TV Shows/Show/Season 4/Episode.m4v
TV Shows/Show/Season 4/Episode.jpg      optional episode thumbnail

Music Videos/Artist - Song.m4v
Music Videos/Artist - Song.jpg
```

Both movie layouts are valid and may be mixed.

### 3.3 Accepted image names

- **Same name as the video:** `<name>.jpg` or `<name>.png`.
- **Per folder:** `poster`, `folder` or `cover`, as `.jpg` or `.png`.
- **What Plex, Jellyfin and Kodi already write**, so exported libraries work as they are:
  - `<name>-thumb.jpg` beside an episode;
  - `seasonNN-poster.jpg` in the show folder (also `season-specials-poster.jpg`);
  - `show.jpg` in the show folder.
- **Ignored:** `fanart`, `backdrop`, `banner`, `logo`, `clearart` (not useful on a 320×240 screen).
- Any size. Images are scaled and stored at sync time (after unplug), never decoded while browsing.

### 3.4 Precedence (most specific first)

1. An image with the video's own name.
2. The image embedded in the video (`covr`).
3. The video's own folder art: the movie folder, or the season folder for an episode.
4. The show folder's art (episodes only).
5. Our own film-frame placeholder.

A picture the user drops in later therefore beats what the converter embedded.

### 3.5 Edge rules

- **Shared folders:** a `poster.jpg` directly in `Movies/` (or `Music Videos/`) never applies to the videos beside it. One film's poster must not land on every film.
- **Several videos in one movie folder** (a multi-part film, a director's cut) share the folder poster, unless a same-name image overrides it.
- **Extras folders** inside a movie or show folder (`Featurettes`, `Extras`, `Behind The Scenes`, `Deleted Scenes`, `Trailers`, `Interviews`, `Shorts`, `Specials` excepted for TV) are hidden from Movies and TV Shows. They may later appear as a movie's extras.
- **Titles** come from the tags, else the file name. A movie folder's name is used only when the file name is generic (`movie.m4v`, `video.m4v`).
- **TV show rows** show the show art. Whether **episode rows** show an episode thumbnail or the show art follows the 7G reference photos (to check before building: `docs/reference/7g/owner/`).

### 3.6 Where art may come from (helpers, all optional)

1. The user's own files (3.2–3.3).
2. The converter's `--poster`, or the source's embedded picture; folder art is copied (not embedded) when converting a whole show.
3. Later, opt-in lookups: the owner's Plex server, or TMDB/TheTVDB with the user's own key. These only run when asked, credit the source, and write the files in 3.2.

## 4. Subtitles (#169; text v1 targeted at 0.9)

### 4.1 Real-time, never burned in

Subtitles are drawn by the iPod while the video plays. The converter never burns them into the picture by default (`--burn-subs` stays as an explicit option only).

### 4.2 Sources, in order (the first found wins)

1. A text track inside the `.m4v` (MP4 timed text, `tx3g`).
2. A sidecar `.srt` with the video's name (also `<name>.<lang>.srt` and `<name>.<lang>.hi.srt`; the language picks the UI language first, then English, then any).
3. A sidecar `.sup` with the video's name: Blu-ray picture subtitles (PGS), shown as pictures (4.5).

### 4.3 Drawing

- **Widescreen video:** subtitles sit in the letterbox band, drawn by the PP. No video-chip image change is needed (v1).
- **4:3 or full-frame video:** subtitles over the picture need the video chip to blend them (the VC overlay). That's a merged-image change, so it waits for the next image round (v2).
- When the scrub or volume bar shows, the subtitles **slide up above it**, and back down when it hides, with the same easing.
- They also stay clear of the top bar (section 5).

### 4.4 Fonts

- **Standard** is ShellSans, the UI font.
- **Monospaced** needs a licence-clean source (OFL or GUST, Heros-compatible weight).
- Both need the same glyph coverage, including music notes (♪ ♫), typographic punctuation and Latin Extended-A. That coverage work is on branch `p19-font-coverage`.
- No Apple fonts, ever (rule 2).

### 4.5 Picture subtitles (PGS) on the device

- **The fallback** when no text subtitles exist (owner).
- **The converter** extracts the stream unchanged to `<name>.sup`.
- **On the device:**
  - parse the PGS segments (PCS, WDS, PDS, ODS, END);
  - decode each subtitle's RLE bitmap once when it changes;
  - scale it with its alpha to the video's display size;
  - keep it until the next event;
  - draw it through the same path as text (4.3).
- **Limit:** 1080p glyphs come out about 9–11 px tall, and the size setting can't enlarge them cleanly.
- **Converter option:** OCR on the Mac (tesseract) to make a `.srt` instead, for crisp text in our font.

### 4.6 Parser edge cases seen in real files

- A UTF-8 byte-order mark at the start.
- Hearing-impaired cues: "(chalk screeches)", "♪ … ♪".
- CRLF line ends.
- Overlapping cues.
- Italic tags `<i>…</i>`: strip them, or draw them slanted only if the font allows.

### 4.7 Tests

- Host parser tests (tx3g, srt, sup), using synthetic fixtures only: never owner media in the repo.
- A sim script with a subtitled fixture clip, run red before the change and green after.

## 5. The video screen's bars (#171, 0.9)

- **When the controls show**, a black **top bar slides down** while the **scrub/volume bar slides up** from the bottom. They use the same timing, and both hide together.
- **Top bar contents:**
  - the title (episode, movie or music video), scrolling like the marquee when it's too long;
  - the current time;
  - the battery level.
- **Look:** like the shell header's elements, in the black colourway.
- **Drawing:** the same split as subtitles. The PP draws into letterbox bands; over-picture drawing waits for the VC overlay.

## 6. Video settings budget

Every item here follows `docs/hardware.md`:
- art scaled at sync time;
- no new resident statics (fw-ship is at its cap, #147);
- subtitle bitmaps in the video session's arena, freed with the screen;
- no allocation from the audio buffer while playing (#170; the overlay piece-buffer lesson from `docs/data-paths.md` R14).
