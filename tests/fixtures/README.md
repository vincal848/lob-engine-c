# Replay fixtures

A hand-built two-level LOBSTER day for `make replay-test`, small
enough to check by hand. Prices are LOBSTER units (dollars x 10000).
Orderbook columns per level: ask price, ask size, bid price, bid size.

Before the file starts, the book already holds orders the message
file never mentions: asks 900 (100 @ 10.02), 901 (50 @ 10.03), 902
(70 @ 10.04, below the visible two levels); bids 903 (40 @ 10.00), 904
(60 @ 9.99), 905 (30 @ 9.98, below the visible levels).

| Row | Message | What it exercises |
|---|---|---|
| 1 | add sell 1: 10 @ 10.02 | seeding from the first snapshot minus the first message |
| 2 | delete 900 (100 @ 10.02) | delete of an order id the file never submitted |
| 3 | execute all of 1 | full execution of a known order (a cancel); 10.04 is revealed as level 2 |
| 4 | partial cancel 903 by 15 | partial cancel of an unknown id |
| 5 | hidden execution, sub-penny price | type 5: no book change |
| 6 | add buy 2: 30 @ 10.01 | a new best bid pushes 9.99 out of view |
| 7 | execute all of 2 | 9.99 returns to view, but 904 was cancelled while hidden: the revealed level 9.99 is removed and 9.98 (905) is revealed instead |
| 8, 9 | halt, then resume | type 7: no book change |
| 10 | add sell 3: 40 @ 10.03 | join an existing level |
| 11 | execute 15 of 3 | partial execution of a known order (a reduce) |
| 12 | delete 3 (25 left) | delete of a known order |
| 13 | delete 901 (50 @ 10.03) | the ask side drops to one level: the snapshot uses the dummy price 9999999999 for level 2 |

`replay_orderbook_2_bad.csv` is the same file with the level-1 bid
size on row 4 changed from 25 to 26. That level was visible on row 3,
so it is not a revealed level, and the harness must report it.
