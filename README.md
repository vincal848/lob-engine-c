# lob-engine-c

[![tests](https://github.com/vincal848/lob-engine-c/actions/workflows/tests.yml/badge.svg)](https://github.com/vincal848/lob-engine-c/actions/workflows/tests.yml)

**Status: Scaffold. M1 in progress.**

A limit order book matching engine in C, meant as the fast core that
[market-microstructure](https://github.com/vincal848/market-microstructure)
(a Hawkes-driven LOB simulator with market makers, being scaffolded in
parallel) can replay LOBSTER message data through.

This repo is ideas plus a scaffold: a spec for where this is going,
and a small, correct, tested core for where it is now -- not a full
implementation.

## Motivation

The thing I actually want to do with this is replay a full trading
day of LOBSTER message data -- millions of order events -- and have
it run fast enough that I can iterate on strategies against it rather
than wait for it. Python is the wrong tool for the inner loop of "take
one message, mutate the book, maybe fire a fill": at millions of
messages per symbol per day, and wanting to run many days and many
symbols, the per-message constant factor is the whole cost of the
project. A C core that processes one message in on the order of
hundreds of nanoseconds, with everything it touches sitting in a
handful of contiguous arrays instead of behind pointers, is the
difference between a replay that finishes in seconds and one that
doesn't finish in a session.

That's also why this is a separate repo from MarketMicrostructure
rather than a Python extension bolted onto it from the start: the book
itself has no opinion about Hawkes processes or market maker agents,
it just needs to be exactly right and fast, and those are easier to
get right in isolation with their own tests and their own benchmark
than mixed into a simulator.

## Design

- **Integer tick prices.** Every price in the public API is an
  `int64_t` count of ticks, never a float. No rounding error, no
  float comparison bugs in the hot path.
- **Price levels: a direct-indexed array, not a sorted level array.**
  Each side of the book is an array of levels indexed by
  `price_ticks - min_tick`, where `[min_tick, max_tick]` is a price
  window fixed at `lob_new()`. Finding the level for a price is an
  array index, O(1), instead of a search. The cost is that the window
  has to be sized up front -- a price outside it is
  `LOB_ERR_PRICE_OUT_OF_RANGE`, not a crash, and a real deployment
  would need to re-center the window as the market moves (not done
  yet). I picked this over a sorted array of only the occupied levels
  because the thing on the hot path is "find this price's level"
  (every add/cancel/reduce does it), not "how many distinct prices
  are occupied" -- see `docs/DESIGN.md` for the longer version of this
  trade-off and a full complexity table.
- **Per-level FIFO, intrusive, pool-indexed.** Orders at a price are a
  doubly linked FIFO (price-time priority) threaded through `prev`/
  `next` fields on the order struct itself -- no separate list nodes,
  and the "pointers" are indices into the order pool, not `malloc`'d
  pointers, so the whole FIFO lives in one contiguous array.
- **order_id -> order: a fixed-capacity hash map.** Open addressing,
  linear probing, sized to 2x the order pool so load factor never
  exceeds 0.5. This is what makes `lob_cancel(order_id)` O(1) expected
  instead of a scan of the book.
- **Preallocated pool, no `malloc` on the hot path.** `lob_new()` does
  every allocation the book will ever need (the order pool, both
  level arrays, the hash table). Add, cancel, reduce, and market
  execution only ever touch those preallocated regions; a full pool
  returns `LOB_ERR_POOL_EXHAUSTED` rather than allocating more.

See `docs/DESIGN.md` for the memory layout diagram and the per-op
complexity table.

## Interface

The public API (`src/lob.h`):

| Function | Does |
|---|---|
| `lob_new(max_orders, min_tick, max_tick)` | Allocates a book: an order pool of `max_orders`, and a price window `[min_tick, max_tick]` on both sides. The only allocation. |
| `lob_free(book)` | Frees it. |
| `lob_set_fill_callback(book, cb, user_data)` | Registers a callback invoked once per resting order consumed by a crossing limit order or a market order. |
| `lob_add_limit(book, order_id, side, price_ticks, qty, &filled_out)` | Crosses the opposite side in price-time priority, then rests any remainder. |
| `lob_cancel(book, order_id)` | Removes a resting order entirely. |
| `lob_reduce(book, order_id, new_qty)` | Shrinks a resting order's quantity in place, without touching its position in the FIFO (time priority preserved). |
| `lob_execute_market(book, side, qty, &filled_out)` | Walks the opposite side until `qty` is filled or the side is empty; never rests. |
| `lob_best_bid(book, &price_out, &qty_out)` / `lob_best_ask(...)` | O(1) top of book, or `0` if that side is empty. |
| `lob_depth(book, side, out[], max_levels)` | Snapshot of up to `max_levels` occupied price levels, best first. |

## LOBSTER message replay

[LOBSTER](https://lobsterdata.com/) message files have one row per
order book event, with an event type column:

| Type | Meaning | Planned mapping |
|---|---|---|
| 1 | New limit order submission | `lob_add_limit` |
| 2 | Partial cancellation (order shrinks, stays in the book) | `lob_reduce` |
| 3 | Deletion (full cancellation) | `lob_cancel` |
| 4 | Execution of a visible limit order | consumed as part of the resting order's fill, via the fill callback during the aggressor's `lob_add_limit`/`lob_execute_market` |
| 5 | Execution of a hidden limit order | same as type 4, but the resting order was never visible in `lob_depth` -- LOBSTER derives this from exchange hidden-liquidity fills, which this book doesn't model as a separate order type yet |
| 7 | Trading halt | not a book mutation; replay should stop feeding messages and resume on the matching resume event |

This mapping is the plan for M2 (`tools/`, not built yet): replay a
LOBSTER `message` file through the API above and diff `lob_depth()`
after each line against LOBSTER's paired `orderbook` file.

## Milestones

- **M1 -- core book + unit tests.** *This scaffold.* `lob_add_limit`,
  `lob_cancel`, `lob_reduce`, `lob_execute_market`, best bid/ask,
  depth snapshot, fill callback, all pool-backed, differentially
  tested against a naive reference.
- **M2 -- LOBSTER replay.** Reproduce LOBSTER's own `orderbook`
  snapshot files exactly, message by message, for at least one full
  trading day of one symbol.
- **M3 -- benchmark.** ns/message on a reproducible harness (not just
  `bench/bench.c`'s synthetic random flow -- real LOBSTER message
  flow), with p50/p99, not just a mean.
- **M4 -- Python binding.** `ctypes`/`cffi` binding so
  MarketMicrostructure can drive this book directly instead of
  reimplementing matching logic in Python.
- **M5 -- exchange layer.** *Spec only: [`docs/EXCHANGE.md`](docs/EXCHANGE.md).*
  Many symbols (one book each), accounts with cash and positions and
  a pre-trade risk check, multi-threaded order entry through lock-free
  rings into a single sequencer thread, and a fill tape. Done when a
  journal recorded from a multi-threaded run replays single-threaded
  into a byte-identical fill tape, under TSan. Built on top of
  `lob.h` without changing it, and after M2, so it sits on a book
  that's already been checked against LOBSTER. Inspired by
  [psakoglou/Exchange-Matching-Engine-Emulation](https://github.com/psakoglou/Exchange-Matching-Engine-Emulation);
  the spec lists what's borrowed and what's done differently.

### Benchmark (preliminary, M1)

`bench/bench.c` replays 1,000,000 random add/cancel/reduce/market
operations against a book with a 50,000-order pool and a 10,000-tick
price window:

```
1000000 ops in 228.628 ms -> 228.6 ns/op
```

One machine, indicative only (WSL Ubuntu, gcc -O2) -- not the M3
benchmark, which needs real LOBSTER message flow and a p50/p99, not a
mean over synthetic traffic.

## Success metrics

- **Exact snapshot match** against LOBSTER's own `orderbook` output,
  not just "looks plausible" -- the differential test in
  `tests/unit_tests.c` is the M1 version of this idea (naive reference
  instead of LOBSTER), and M2 replaces the reference with the real
  thing.
- **p50/p99 latency per message**, not just a mean -- a matching
  engine that's fast on average but has a long tail on, say, cancels
  that empty the best level (see `docs/DESIGN.md`'s complexity table)
  is not actually fast for a replay that cares about wall-clock time.
- **Zero allocations on the hot path, verified, not assumed.** `make
  asan` already catches memory bugs; M3 should also verify no
  `malloc`/`free` calls happen between `lob_new()` and `lob_free()`
  (e.g. an `LD_PRELOAD` allocation counter), not just rely on the code
  review claim that it doesn't.

## Quick start

```sh
make test    # build and run tests/unit_tests.c
make asan    # same tests, rebuilt with -fsanitize=address,undefined
make bench   # build and run bench/bench.c
make clean
```

`CC` defaults to `cc`; override with `make CC=clang test`.

## Repository guide

| Path | Contents |
|---|---|
| `src/lob.h`, `src/lob.c` | The M1 core: book, matching, cancel/reduce, depth snapshot. |
| `tests/unit_tests.c` | Assert-based tests, including the differential test against a naive reference over 100k random operations. |
| `bench/bench.c` | Synthetic-flow throughput benchmark, built but not a CI gate. |
| `tools/` | Placeholder for the M2 LOBSTER replay harness. |
| `docs/DESIGN.md` | Memory layout diagram and the per-operation complexity table. |
| `docs/EXCHANGE.md` | M5 spec: the exchange layer (symbols, accounts, risk, sequencer, fill tape) on top of the book. |
| `Makefile` | `CC ?= cc`, `-std=c11 -O2 -Wall -Wextra -Werror -pedantic`, `all`/`test`/`bench`/`asan`/`clean`. |
| `.github/workflows/tests.yml` | gcc/clang matrix plus a separate ASan/UBSan job. |

## Notes

- C11, POSIX `clock_gettime` in the benchmark only (not in the book
  itself, which is plain C11 with no OS dependency).
- The price window and order pool are both fixed at `lob_new()`; there
  is no rebasing or resizing yet. See `docs/DESIGN.md` for why that's
  the deliberate trade for M1 and what a production version would
  need to add.
