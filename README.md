# lob-engine-c

[![tests](https://github.com/vincal848/lob-engine-c/actions/workflows/tests.yml/badge.svg)](https://github.com/vincal848/lob-engine-c/actions/workflows/tests.yml)

**Status: M1 core, M2 replay, M3 benchmark, M4 Python binding, M5 exchange layer (state machine, gateway, benchmark) done. The replay matches
two full LOBSTER sample days exactly, at a p50 of 50 ns and a p99 of
251 ns per message (see Milestones).**

A limit order book matching engine in C, meant as the fast core that
[MarketMicrostructure](https://github.com/vincal848/MarketMicrostructure)
(a Hawkes-driven LOB simulator with market makers, being scaffolded in
parallel) can replay LOBSTER message data through.

The book itself is done and checked against real exchange data; what's
left is nothing in the plan: the M5 exchange layer (state machine, threaded gateway, benchmark) is built.

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
| `lob_get_order(book, order_id, &side, &price, &qty)` | Read-only lookup of a resting order. |
| `lob_level_orders(book, side, price_ticks, ids[], max_ids)` | Read-only list of the order ids at one price level, in FIFO order. |

## LOBSTER message replay

[LOBSTER](https://lobsterdata.com/) message files have one row per
order book event, with an event type column:

| Type | Meaning | Mapping |
|---|---|---|
| 1 | New limit order submission | `lob_add_limit` |
| 2 | Partial cancellation (order shrinks, stays in the book) | `lob_reduce` |
| 3 | Deletion (full cancellation) | `lob_cancel` |
| 4 | Execution of a visible limit order | `lob_reduce` on the *resting* order, or `lob_cancel` if it's fully executed. The aggressor isn't in the message file, only the resting side of each execution, so this can't be replayed as a crossing `lob_add_limit` |
| 5 | Execution of a hidden limit order | nothing: hidden liquidity never shows in the visible book |
| 6 | Cross trade | nothing |
| 7 | Trading halt / quote / resume | nothing: LOBSTER repeats the previous snapshot on these rows |

`tools/lobster_replay` (M2) replays a `message` file through the API
above and checks `lob_depth()` after each line against LOBSTER's
paired `orderbook` file. A level-N file only has messages for events
inside the N visible levels, so orders resting before 9:30 or moving
while deeper than level N are invisible to it; `tools/README.md`
explains the narrow rule the replay uses to account for that without
turning the check into "copy the snapshot".

## Milestones

- **M1 -- core book + unit tests.** *Done.* `lob_add_limit`,
  `lob_cancel`, `lob_reduce`, `lob_execute_market`, best bid/ask,
  depth snapshot, fill callback, all pool-backed, differentially
  tested against a naive reference.
- **M2 -- LOBSTER replay.** Reproduce LOBSTER's own `orderbook`
  snapshot files exactly, message by message, for at least one full
  trading day of one symbol. *Done for the 2012-06-21 AAPL and MSFT
  10-level samples* -- every row matches, under the revealed-level rule
  in `tools/README.md`. AAPL also runs in CI (`lobster-replay` job):

  | | AAPL | MSFT |
  |---|---|---|
  | rows | 400,391 | 668,765 |
  | mismatched rows | **0** | **0** |
  | lost shares | 0 | 0 |
  | messages for ids never submitted in the file | 8,919 | 11,494 |
  | revealed-level adjustments | 19,342 | 1,450 |
  | messages that spilled past their own order | 367 | 3,595 |

  The last three rows are the replay leaning on the snapshot file,
  reported rather than hidden: liquidity that a 10-level file can't
  show directly.
- **M3 -- benchmark.** ns/message on a reproducible harness (not just
  `bench/bench.c`'s synthetic random flow -- real LOBSTER message
  flow), with p50/p99, not just a mean. *Done:* `lobster_replay -t`
  (`make replay-bench`) times each message's book update on the AAPL
  day, and `make alloc-test` proves the hot path never allocates.
  Both run in CI.

  | AAPL 2012-06-21, 389,059 type 1-4 messages | ns |
  |---|---|
  | mean | 81 |
  | p50 | 50 |
  | p90 | 151 |
  | p99 | 251 |
  | p99.9 | 2,694 |
  | max | 76,614 |

  GitHub Actions `ubuntu-latest`, gcc -O2, one run. A sample is one
  message's `lob_*` calls plus the replay's own id lookup, not parsing
  or the snapshot check, and includes about 21 ns of timer overhead.
  The p99.9 and max are most likely the shared runner being
  descheduled, not the book, which has no allocation or I/O to stall
  on; a pinned core on a quiet machine is the way to tell.
- **M4 -- Python binding.** `ctypes`/`cffi` binding so
  MarketMicrostructure can drive this book directly instead of
  reimplementing matching logic in Python. *Done:* `python/lob.py`,
  standard library only:

  ```python
  from lob import Book, BUY, SELL    # after `make lib`
  with Book(max_orders=10_000, min_tick=57_000, max_tick=59_000) as book:
      book.on_fill(print)             # Fill(resting_order_id, aggressor_order_id, ...)
      book.add_limit(1, SELL, 58_001, 5)
      book.add_limit(2, BUY, 58_001, 3)   # returns 3: crossed
      book.best_ask()                      # (58001, 2)
  ```

  Every `lob.h` call is wrapped; a non-OK status raises `LobError`
  with the status name. `make python-test` runs `python/test_lob.py`
  in CI. Each call crosses the ctypes boundary, so drive the book in
  batches from C (or replay with `lobster_replay`) where speed
  matters; the binding is for simulators and notebooks.
- **M5 -- exchange layer.** *M5a (the single-threaded state machine), M5b (the gateway) and M5c (the benchmark) done: [`docs/EXCHANGE.md`](docs/EXCHANGE.md).* M5a is `src/exchange.{h,c}`: symbols, exchange-assigned order ids, accounts with reservations and pre-trade risk, IOC, exact market-buy pricing, fill attribution and an event stream, checked by conservation invariants and a differential test against a naive exchange, and `ex_submit` is verified allocation-free. M5b is `src/gateway.{h,c}`: a lock-free SPSC ring per producer each way, one sequencer thread that stamps seq and timestamp, journals every request and writes the fill tape, and a determinism test that replays the journal of a concurrent run single-threaded and requires a byte-identical tape; it also runs under ThreadSanitizer in CI.

  M5c is `bench/exchange_bench.c` (`make exchange-bench`, options such
  as `BENCH_ARGS="-p 4 -w 8"`): N producer threads x M symbols x K
  accounts, each keeping `-w` requests in flight and timestamping around
  `gw_submit` and on the ACK/REJECT coming back, so a sample is submit
  -> ring -> sequencer -> `ex_submit` -> ring -> producer. `make
  alloc-test` also covers the whole gateway hot path: nothing between
  `gw_new` and `gw_stop` calls the allocator over 100k requests from two
  producers (checked by putting a `malloc` into the sequencer loop, which
  it caught).

  | producers | window | requests/s | p50 ns | p99 ns (3 runs) | max ns |
  |---|---|---|---|---|---|
  | 1 | 1 | 974,804 | 668 | 2,345-2,487 | 233,507 |
  | 1 (40% market orders) | 1 | 1,224,903 | 601 | 2,098-2,463 | 275,623 |
  | 1 | 8 | 1,631,270 | 2,962 | 9,293-22,605 | 7,674,554 |
  | 2 | 8 | 2,027,091 | 6,711 | 18,181-33,046 | 1,857,998 |
  | 4 | 8 | 1,908,283 | 14,430 | 34,409-52,954 | 2,069,479 |
  | 4 (40% market orders) | 8 | 2,079,051 | 13,721 | 34,804-42,423 | 468,392 |

  Each row is the median-throughput run of three (`-n 200000` requests
  per producer, seeds 1-3, 8 symbols, 64 accounts, 10% market orders
  unless noted); throughput, p50 and max are from that run, the p99
  column is the range over all three. The first 5% of each producer's
  samples are dropped as warm-up. Intel i7-12700F (20 logical CPUs),
  WSL2 Ubuntu 24.04, kernel 6.18, gcc 13.3 -O2, nothing pinned. Under 4%
  of requests are rejected (cancels of an order that filled in the
  meantime, plus a few capacity rejects) and no event or fill was
  dropped. Caveats: one machine, one flow shape, spinning threads on a
  shared virtualized host, so the max is mostly the OS descheduling a
  thread and throughput and p99 move a lot between runs (one run in three
  was far off the others). With more than one request in flight the
  latency is queueing time in front of the one sequencer (p50 is roughly
  producers x window / throughput), not the cost of one request; the
  window-1 rows (about 0.6-0.7 us p50, 2.1-2.5 us p99) are the cost of
  the path with no queue.

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

One machine, indicative only (WSL Ubuntu, gcc -O2). Synthetic random
flow is much harder on the book than real flow (orders land anywhere
in a 10,000-tick window, so market orders walk many levels); the M3
numbers above are the ones to quote.

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
- **Zero allocations on the hot path, verified, not assumed.**
  `tests/alloc_test.c` wraps `malloc`/`calloc`/`realloc`/`free` at link
  time (GNU ld `--wrap`) and fails if the book calls any of them
  between `lob_new()` and `lob_free()` over 1M random operations. It
  was checked by putting a `malloc` into `lob_cancel`, which it caught.

## Quick start

```sh
make test           # unit tests + replay fixtures
make asan           # same tests, rebuilt with -fsanitize=address,undefined
make alloc-test     # fail if the book or ex_submit allocates after setup (GNU ld)
make exchange-test  # just the M5a exchange tests (also part of make test)
make tsan           # the M5b gateway tests under ThreadSanitizer (Linux or WSL)
make gateway-test   # just the M5b gateway tests (also part of make test)
make exchange-bench  # M5c: latency/throughput through the gateway (BENCH_ARGS="-p 4")
make bench          # build and run bench/bench.c
make fetch-lobster  # download the AAPL LOBSTER sample into data/ (LOBSTER_TICKER=MSFT for MSFT)
make replay         # replay it and check every row against the orderbook file
make replay-bench   # same replay, with per-message latency percentiles
make python-test    # build build/liblob.so and run the Python binding tests
make clean
```

`CC` defaults to `cc`; override with `make CC=clang test`. On Windows
with MinGW-w64 (e.g. WinLibs), `mingw32-make CC=gcc test` works from a
shell that has `sh` (Git Bash); `make asan` needs Linux or WSL.

## Repository guide

| Path | Contents |
|---|---|
| `src/lob.h`, `src/lob.c` | The M1 core: book, matching, cancel/reduce, depth snapshot. |
| `tests/unit_tests.c` | Assert-based tests, including the differential test against a naive reference over 100k random operations. |
| `tests/alloc_test.c` | M3: link-time allocator wrap proving no `malloc`/`free` on the hot path. |
| `src/exchange.h`, `src/exchange.c` | M5a: the exchange state machine (symbols, accounts, risk, IOC, events) on top of the book. |
| `tests/exchange_tests.c`, `tests/ex_random.h` | M5a unit tests, conservation invariants and the differential test against a naive exchange; the seeded request generator is shared. |
| `tests/alloc_exchange_test.c` | M5a: the allocator wrap over 1M `ex_submit` calls. |
| `src/gateway.h`, `src/gateway.c` | M5b: SPSC rings, the sequencer thread, journal, fill tape and single-threaded replay. |
| `tests/gateway_tests.c` | M5b: ring unit tests and the determinism test (concurrent producers, replay, `memcmp` of the tapes). |
| `tests/alloc_gateway_test.c` | M5c: the allocator wrap across the whole gateway hot path, `gw_new` to `gw_stop`. |
| `tests/fixtures/` | A hand-built two-level LOBSTER day for the replay harness, plus a corrupted copy it must reject. |
| `bench/bench.c` | Synthetic-flow throughput benchmark, built but not a CI gate. |
| `bench/exchange_bench.c` | M5c: producers x symbols x accounts, submit-to-ACK p50/p99/max and requests/s through the gateway; built in CI, not gated. |
| `tools/lobster_replay.c` | The M2 LOBSTER replay harness (`-t` adds M3 latency); `tools/README.md` has the message mapping and the reconciliation rule. |
| `python/lob.py`, `python/test_lob.py` | M4: the ctypes binding and its tests. |
| `docs/DESIGN.md` | Memory layout diagram and the per-operation complexity table. |
| `docs/EXCHANGE.md` | M5 spec (M5a and M5b built; "As built" lists deviations): the exchange layer (symbols, accounts, risk, sequencer, fill tape) on top of the book. |
| `Makefile` | `CC ?= cc`, `-std=c11 -O2 -Wall -Wextra -Werror -pedantic`, `all`/`test`/`bench`/`alloc-test`/`exchange-test`/`lib`/`python-test`/`asan`/`tsan`/`gateway-test`/`exchange-bench`/`replay`/`replay-bench`/`fetch-lobster`/`clean`. |
| `.github/workflows/tests.yml` | gcc/clang matrix (with the allocation test), a separate ASan/UBSan job, and a job that replays and times the AAPL LOBSTER sample. |

## Notes

- C11, POSIX `clock_gettime` in the benchmark and replay tool only (not in the book
  itself, which is plain C11 with no OS dependency).
- The price window and order pool are both fixed at `lob_new()`; there
  is no rebasing or resizing yet. See `docs/DESIGN.md` for why that's
  the deliberate trade for M1 and what a production version would
  need to add.
