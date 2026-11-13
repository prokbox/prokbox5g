# Docs patches for released versions

A pushed tag never moves, so a correction to a released version's pages (a wrong
date, a typo) is a *docs patch*: the corrected file, at its path in the tree, under
`site/versions/patches/vX.Y.Z/`. `scripts/docs_versions.py` lays these files over
the tag's export before it builds that version's folder (and the root, when that
version is the newest). Only paths a docs-only update may change are accepted.
The folder name must be an existing release tag. `versions.json` lists the patched
paths ("patched"). Ship a patch with a normal docs update (release-plan 3.3a).
