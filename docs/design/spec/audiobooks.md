# Audiobooks

*Split from `7g-spec.md` "Menu trees" (2026-10-07). Now Playing audiobook view (chapter art, scrubbing) is in [now-playing.md](now-playing.md).*

- **Audiobooks (stock 2.0.5, owner decision 2026-10-06):** a list of books by title, each with a progress hint. Selecting a book **opens its list**: **"Resume"** first (resume at the saved position), then **the chapters**. Selecting a chapter plays from it. *(Photo `audiobook-open-chapters-b.jpg` confirms the opened list: full-width, single-line, no chevron on Resume, no progress hints on the chapter rows.)*
- **Last played on top (owner, 2026-10-11, #67; Phase 8b):** the most recently played book always sits at the top of the Audiobooks list, whatever the sort order, so getting back to it never means scrolling the whole list. The rest of the list keeps the chosen sort. Not stock 7G behaviour: record it in `docs/design/changes-from-7g.md` when it lands.
