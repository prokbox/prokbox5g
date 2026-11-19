# ADR 0010: Unplayable files are shown greyed out, never hidden

- Status: Accepted
- Date: 2026-11-10

## Context

A 32-bit FLAC (#285) or a DRM-protected purchase synced by iTunes was listed but silently failed; an all-unplayable album just stopped playback (#281).

## Decision

Index every file we can recognise. A metadata parser marks a file unplayable, with a reason, at index time from its headers; lists draw it greyed with a sign, without opening the file. One shared record and one visual treatment cover Music and, later, Videos and Photos.

## Consequences

- A new field (planned as the last member of `struct mp3entry`) and a tagcache upgrade path.
- Design options are rendered at real size for the owner before building.

## Alternatives considered

- Hide unplayable files: the library looks incomplete and the cause is invisible.
- Show an error on selection: against "errors are logged, not shown".

## Owner decision

"songs the iPod can't play stay in the library, but show greyed out with a clear sign that they can't play. Selecting one doesn't start playback, and a queue skips them." (#286, 2026-11-10)

## Amendment (owner, 2026-11-13): SELECT shows why

SELECT on a greyed row no longer does nothing. It opens a short page that says why: a song or an audiobook gets a light prompt page ("Can't Play Song" / "Can't Play Audiobook", a heading, one sentence for the reason, OK); a video gets the existing "Can't Play Video" page with its reason. Nothing plays, nothing stops, and the queue is untouched; OK or MENU returns to the list on the same row. This overrides "Show an error on selection" above, for a selection the user made, as video failures already did (changes-from-7g 2.34). Errors found anywhere else are still logged, not shown. Code: `apps/ipodui/screens/noplay.c`, `ui_video_show_why()`.

The same holds in every list that can play a row (2026-11-13, p090-286tail): the library's lists, Search (song, episode and book results) and On-The-Go draw such rows greyed and open the page through one entry, `ui_noplay_row()`, which picks the kind from the entry's class in the index (a podcast episode gets "Can't Play Episode"). On-The-Go plays from a playable row as before, and the queue skips the files that can't play. The hold menu leaves out Add to On-The-Go for a row that can't play (the menu has no greyed items; a video is already handled this way) and keeps Browse Album, Browse Artist and Cancel.

## Links

- #286, #285, #281, #225
