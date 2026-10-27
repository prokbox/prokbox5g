# docs/user/

The source of the ProkBox5g user guide, site and PDF (https://prokbox.github.io/prokbox5g/). One Markdown source; `scripts/docs_build.sh` turns it into the site (Hugo layouts in `site/`) and the PDF.

- `_index.md`: the home page. `guide/`: one file per chapter, ordered by `weight`. `releases/<ver>.md`: release notes (also used for the GitHub release text). `images/`: screenshots, referenced relatively (`../images/x.png`).
- Front matter on every page: `title`, `weight` (chapters) or `date` (release notes), `description`.
- No H1 in the body; sections start at `##`. Callouts: `> [!NOTE]`, `> [!TIP]`, `> [!WARNING]`. Buttons in bold; menu paths with ›.
- Our own words; screenshots from the simulator with synthetic or open media only; never "Rockbox" outside credits, never the hidden folder's name; Apple marks only to name the hardware.
