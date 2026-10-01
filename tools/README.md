# tools/

Placeholder for the LOBSTER replay harness (M2). Not built yet.

The plan: a small driver that reads a LOBSTER `message` CSV (and the
paired `orderbook` CSV for the expected snapshots) and feeds each
message through `lob_add_limit` / `lob_cancel` / `lob_reduce` /
`lob_execute_market` as described in the README's "LOBSTER message
replay" section, then diffs `lob_depth()` after each line against the
corresponding row of the `orderbook` file. M2 is done when that diff
is clean across a full trading day for at least one symbol.
