# Rendering this manual

`scripts/bcmref_html.py` renders only `docs/bcm2722-reference/`; its input directory is fixed. This manual uses the same conventions (Markdown tables, `[tag]` evidence markers, relative links between files, `../bcm2722-reference/` cross-links), so the renderer should work unchanged once it accepts a source directory.

Wanted (for the script's owner): a generic flag, for example `--src docs/pp5022-reference --title "PP5022 reference"`, with the output directory derived from the source name, and cross-manual links (`../bcm2722-reference/x.md`) rewritten to the other manual's rendered page.

Until then, read the Markdown directly.
