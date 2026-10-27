# MOD_CONTACTS: Extras > Contacts (P12 W3-K): the Contacts list and a
# contact's page. Reads the sync-time index (contacts.idx) at entry, a
# page at a time while browsing a long list (pages: the rate the
# accountant allows), and one card per open; never walks a folder. Its
# working block holds the page cache, the open card's text and its line
# table.
class   = A
min_ram = 32
kind    = shell
exit    = immediate
work    = 16384
quota   = 0
boost   = burst
pages   = 8                 # page loads a second (screens/*.h *_PAGES)
exports = contacts
