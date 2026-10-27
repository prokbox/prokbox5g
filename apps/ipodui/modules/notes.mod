# MOD_NOTES: Extras > Notes (P12 W1-N): the Notes list, a folder's list
# and the note reader. Reads the sync-time index (notes.idx) at entry, a
# page at a time while browsing a long one (pages), and one note per
# open; never walks a folder. Its working block holds both
# lists, the reader's 4 KB text and the index cache.
class   = A
min_ram = 32
kind    = shell
exit    = immediate
work    = 16384
quota   = 0
boost   = burst
pages   = 8                 # page loads a second (screens/*.h *_PAGES)
exports = notes
