# MOD_CHESS (#132, docs/design/sdk.md 13.5): chess against the engine, at
# three levels. An SDK module: linked only against the SDK table (sdk.sym).
# Class A: the engine's transposition table is sized to the module arena
# (docs/design/games.md 5.6); a Class B borrow would only grow the table.
class   = A
min_ram = 32
kind    = sdk
abi     = 0.1
exit    = graceful 200
work    = 17408
state   = 2048
quota   = 64
boost   = burst
strings = chess
exports = chess
