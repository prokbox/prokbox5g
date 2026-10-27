# MOD_BADGAME (P12 W4-0, docs/design/sdk.md 15): the misbehaving test
# game, developer builds only. Its mode comes from its state record.
class   = A
min_ram = 32
kind    = sdk
abi     = 0.1
exit    = graceful 1000
work    = 4096
state   = 64
quota   = 64
boost   = burst
exports = badgame
