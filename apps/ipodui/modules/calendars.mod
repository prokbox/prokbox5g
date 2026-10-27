# MOD_CALENDARS: Extras > Calendars (P12 W3-C): the list (All Calendars,
# each calendar, To Do's), the month grid, a day's events, an event's page.
# Reads the sync-time index (calendars.idx) at entry and per opened day or
# event; never walks a folder. Its working block holds the index's tables,
# the root list, one list of rows and one event page.
class   = A
min_ram = 32
kind    = shell
exit    = immediate
work    = 12288
quota   = 0
boost   = burst
exports = calendars
