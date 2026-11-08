# Audiobooks

*Split from `7g-spec.md` "Menu trees" (2026-10-07). Now Playing audiobook view (chapter art, scrubbing) is in [now-playing.md](now-playing.md).*

- **Audiobooks (stock 2.0.5, owner decision 2026-10-06):** a list of books by title, each with a progress hint. Selecting a book **opens its list**: **"Resume"** first (resume at the saved position), then **the chapters**. Selecting a chapter plays from it. *(Photo `audiobook-open-chapters-b.jpg` confirms the opened list: full-width, single-line, no chevron on Resume, no progress hints on the chapter rows.)*
- **Last played on top (owner, 2026-10-11, #67; Phase 8b):** the most recently played book always sits at the top of the Audiobooks list, whatever the sort order, so getting back to it never means scrolling the whole list. The rest of the list keeps the chosen sort. Not stock 7G behaviour: record it in `docs/design/changes-from-7g.md` when it lands.
- **What is a book (owner, 2026-11-06, #146; 0.8.0):** decided once, when the library is updated, never while browsing. There is no setting. In order:
  1. a video is never a book;
  2. a podcast (genre Podcast or Podcasts, or a `Podcasts` folder) stays a podcast;
  3. anything else is a book if any one of these holds:
     - it is an `.m4b` file or carries the MP4 audiobook flag;
     - its genre is a book genre: Audiobook(s), Audio Book(s), Book(s), Spoken Word, Spoken & Audio, Books & Spoken, Hörbuch, Hörbücher, Hoerbuch, Hoerbuecher, Gesprochenes Wort, Luisterboek(en) or Gesproken woord. The match ignores case and surrounding spaces but is otherwise exact, so "Bookshelf" is not a book genre. Radio plays (Hörspiel) are left out on purpose;
     - it sits anywhere under a folder named `Audiobooks` or `Audio Books`, in any letter case.

  Files of one book share the album and are grouped as before, including books split into `CD1`/`CD2` folders. Music placed in an Audiobooks folder becomes a book, by design. Folder and genre books get per-book resume, the speed choice and the chapter list (from `.cue` or embedded chapters) like any `.m4b`. Rules in `apps/ipodui/bookclass.h`; tests in `tests/classify/` and `tests/ui/ipodui-146-audiobook-classify.script`.
