# tools/

## `lobster_replay` -- M2

Replays a LOBSTER `message` file through the book and checks it
against LOBSTER's paired `orderbook` file after every message.

```sh
make fetch-lobster                    # AAPL, 10 levels, into data/ (gitignored)
make replay                           # replay it
make fetch-lobster replay LOBSTER_TICKER=MSFT
./build/lobster_replay [-q] [-p max_orders] MESSAGE.csv ORDERBOOK.csv
```

Exit status is 0 if every row matched, 1 if any row didn't (the first
ten are printed to stderr), 2 on bad input. Prices are converted
from LOBSTER's dollars x 10000 to cent ticks; the price window passed
to `lob_new()` is the range of every price either file mentions.

### Message mapping

| Type | Meaning | Replay |
|---|---|---|
| 1 | New limit order | `lob_add_limit` (a visible LOBSTER book never crosses, so this never fills) |
| 2 | Partial cancel | remove `size` shares from the order |
| 3 | Delete | remove the order |
| 4 | Visible execution | remove `size` shares from the *resting* order -- the aggressor isn't in the file, so this is a reduce (or a cancel, if it empties the order), not a crossing add |
| 5 | Hidden execution | nothing: hidden liquidity never shows in the visible book |
| 6 | Cross trade | nothing |
| 7 | Halt / quote / resume | nothing: LOBSTER repeats the previous snapshot on these rows |

### What a level-N file can't tell you

A level-N LOBSTER file only has messages for events *inside the N
visible levels*. Checked against the 2012-06-21 AAPL sample: every
type 1-4 message changes the snapshot, and none lands outside it.
So two kinds of liquidity are invisible to the message file:

- **Orders already resting at 9:30.** AAPL's file has thousands of
  deletes and executions for order ids it never submitted.
- **Orders submitted or cancelled while deeper than level N.** When
  the book thins out and they move into the top N, they appear in
  the snapshot with no message.

A pure message replay therefore can't reproduce the snapshots, and
"just copy the snapshot whenever it disagrees" would make the check
meaningless. The rule used here:

1. **Seed** the book on row 1 from the first snapshot, with the
   first message's effect undone.
2. **Revealed levels sync.** A level may be set from the snapshot
   only if it was just *revealed*: the previous snapshot had all N
   levels on that side occupied, and this level is strictly worse
   than the previous snapshot's worst level. Nothing about such a
   level was observable before this row.
3. **Everything else must match exactly.** Any other visible level
   that disagrees with the snapshot is a mismatch. (The level is
   then resynced, so one bad row is reported once rather than
   cascading.)

Liquidity the replay had to infer is held as synthetic orders (ids
with bit 62 set). A message naming an order id the book has never
seen is applied against that level's synthetic orders; a message
removing more shares than its named order has takes the rest from
synthetic orders first. If a level can't supply the shares at all,
they're counted as **lost shares**, which fails the run like a
mismatch does.

Every inference is counted in the summary (unknown-id messages,
spills, revealed adjustments and the shares they moved in and out),
so a clean run says how much it leaned on the snapshot file, not just
that it passed.

### Fixtures

`make test` runs the harness against a hand-built two-level day in
`tests/fixtures/` (see its README) and checks that a copy with one
corrupted level is reported as a mismatch. The real LOBSTER samples
aren't committed; CI downloads AAPL with `make fetch-lobster` in a
separate job.
