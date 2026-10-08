# M5 spec: exchange layer

**Status: spec only. Nothing in this document is built yet.**

This is the plan for an exchange emulator on top of the M1 book: many
symbols, participants with cash and positions, concurrent order entry
from multiple threads, and a fill tape. It is a layer *around*
`src/lob.h`, not a change to it -- the book stays single-symbol,
single-threaded, and ignorant of accounts, so everything M1-M3 proves
about it (exact LOBSTER snapshots, p50/p99, zero allocations) stays
true underneath.

It's sequenced after M2 on purpose: an exchange is only as believable
as the book it sits on, and M2 is what makes the book believable.

## Prior art

The starting point is
[psakoglou/Exchange-Matching-Engine-Emulation](https://github.com/psakoglou/Exchange-Matching-Engine-Emulation),
a C++ emulator of an exchange with traders, a pool of instruments, and
threads submitting BUY/SELL requests concurrently. The shape of this
milestone -- instruments, traders with cash, async request
submission, an order book plus a fill book, a stress demo -- comes from
there. Where this spec deliberately does something different:

| That project | This spec | Why |
|---|---|---|
| Binary max-heap per side | The M1 book (direct-indexed levels, per-level FIFO, id hash) | Cancel-by-id is O(1) expected instead of a heap search; LOBSTER replay is mostly cancels |
| Mismatched prices fill at the average | Fills at the resting order's price | That's how a continuous limit order book prices a cross, and it's what LOBSTER's data reflects |
| Instrument hash hard-coded to specific stocks | Symbols loaded from config at startup | Adding a symbol shouldn't need a code change |
| Mutex around the shared exchange | One sequencer thread owns all book and account state; producers talk to it through lock-free rings | No locks on the matching path, and a total order on inputs that can be journaled and replayed |
| Over-limit traders have pending requests purged after the fact | Pre-trade risk check rejects at entry | An order that can't be paid for never reaches the book |
| Verified by hand-checked demos | Invariant checks, a deterministic replay test, TSan in CI | Same bar as M1's differential test |

## Architecture

Two layers, split so that everything with state is single-threaded and
deterministic, and everything with threads is stateless plumbing:

```
 producer threads                 sequencer thread              outputs
 ----------------                 ----------------              -------
 client 0 --[SPSC in ring]--+
 client 1 --[SPSC in ring]--+-->  stamp seq + ts  -->  journal (input log)
 ...                        |          |
 client N --[SPSC in ring]--+          v
                                  ex_submit()  ------>  fill tape
                                       |
 client i <--[SPSC out ring]-----  acks / rejects / fills
                                       |
                                  lob_t per symbol
```

- **`src/exchange.{h,c}` -- the state machine.** Plain C11, no threads,
  no clocks, no OS dependency, same rules as `lob.c`. Owns the symbol
  table, one `lob_t` per symbol, the accounts, and the exchange-side
  order records. One entry point processes one request completely.
- **`src/gateway.{h,c}` -- the plumbing.** pthreads and
  `<stdatomic.h>`. Owns the rings, the sequencer thread, the journal,
  and timestamps. Calls `ex_submit()` and nothing else on the state
  machine.

The payoff of the split: **the journal fully determines the fill
tape.** A live multi-threaded run interleaves producers
nondeterministically, but once the sequencer has stamped a request
with a sequence number and timestamp and written it to the journal,
the rest is a pure function. Feeding the journal back through a fresh
`ex_t` single-threaded must reproduce the tape byte-for-byte. That's
the exchange-layer version of M2's "exact snapshot match".

## State machine (`exchange.h`)

Sketch, not final:

```c
typedef struct ex ex_t;

typedef enum { EX_LIMIT, EX_IOC, EX_MARKET } ex_order_type_t;
typedef enum { EX_REQ_NEW, EX_REQ_CANCEL, EX_REQ_REDUCE } ex_req_kind_t;

typedef struct {
    uint64_t seq;          /* stamped by the sequencer */
    int64_t  ts_ns;        /* stamped by the sequencer, journaled */
    ex_req_kind_t kind;
    uint32_t account;
    uint32_t symbol;       /* index, resolved from the name at entry */
    uint64_t order_id;     /* assigned by ex_submit for NEW; target for CANCEL/REDUCE */
    lob_side_t side;
    ex_order_type_t type;
    int64_t  price_ticks;  /* ignored for EX_MARKET */
    uint32_t qty;          /* new qty for REDUCE */
} ex_request_t;

/* Everything ex_submit emits goes through one sink, in order. */
typedef void (*ex_event_fn)(const ex_event_t *ev, void *user_data);

ex_t *ex_new(const ex_config_t *cfg);        /* the only allocation */
void  ex_free(ex_t *ex);
int   ex_add_symbol(ex_t *ex, const char *name, int64_t min_tick, int64_t max_tick);
int   ex_add_account(ex_t *ex, int64_t cash, const int64_t *positions);
void  ex_set_event_sink(ex_t *ex, ex_event_fn cb, void *user_data);
void  ex_submit(ex_t *ex, const ex_request_t *req);
```

`ex_event_t` is a tagged union of `ACK`, `REJECT` (with a reason code),
`FILL` (one per side per fill), and `DONE` (the order is fully filled
or cancelled and its id is retired).

### Symbols

A fixed-capacity name -> index table (open addressing, same scheme as
the order-id map in `lob.c`), filled from config at startup and frozen
before the first `ex_submit`. Each index owns one `lob_t` with its own
price window. Producers resolve the name to an index once, at entry,
so the hot path only ever sees an integer.

All symbols share one tick size in M5, so cash can be held in the same
integer tick units as prices: buying `q` at `p` ticks costs `p * q`
cash units. Per-symbol tick sizes are an open question (below).

### Order ids and order records

The exchange assigns order ids, globally unique across symbols, from
a counter. Each book only ever sees ids the exchange gave it.

Alongside each book's own id map, the exchange keeps its own
preallocated record per open order: `{account, symbol, side,
limit_price, open_qty, reserved}`. This duplicates a little of what
`lob.c` already stores, deliberately -- it keeps `lob.h` unchanged and
gives the exchange O(1) access to the fields `lob.h` doesn't expose
(which account, how much is reserved). If it turns out to cost
something measurable, a read-only `lob_order_qty()` accessor is the
fix, not merging the layers.

### Fill attribution

`lob_fill_t` already carries both `resting_order_id` and
`aggressor_order_id`, so the fill callback looks both up in the order
records and updates both accounts. The one hole is market orders,
whose `aggressor_order_id` is `0`. Because `ex_submit` is
single-threaded and processes one request to completion, the
exchange sets a "current aggressor" field before calling into the book
and the callback uses it when the id is `0`. No change to `lob.h`
needed.

### Accounts and pre-trade risk

```
account = { cash, reserved_cash, position[symbol], reserved_position[symbol] }
available_cash     = cash - reserved_cash
available_position = position - reserved_position
```

Checked at entry, before anything touches the book:

- **Limit/IOC buy:** require `available_cash >= price * qty`, then
  reserve `price * qty`. A fill at a better price `p' < price` refunds
  `(price - p') * fill_qty` from the reservation.
- **Limit/IOC sell:** require `available_position >= qty`, then reserve
  `qty` shares. No short selling (same rule as the prior-art project).
- **Market sell:** the same position check.
- **Market buy:** the price isn't known up front, so the exchange
  walks `lob_depth()` on the ask side (into a buffer preallocated at
  `ex_new`) to compute the exact cost of filling `qty` against the
  current book. Because the sequencer is single-threaded, that cost
  is exact, not an estimate. Reject if `available_cash` can't cover it.
- **Cancel / reduce:** release the reservation for the removed quantity.
- **Fill:** move cash and shares between the two accounts, and release
  the matching reservations.

A rejected request never reaches the book. No partial accepts.

### IOC

`lob.h` has no IOC flag. The exchange gets one by calling
`lob_add_limit` and then immediately `lob_cancel` on any remainder.
That's safe only because nothing else can observe the book between
the two calls -- another thing the single-threaded sequencer buys.

## Gateway (`gateway.h`)

- **Rings.** One SPSC ring per producer inbound and one per producer
  outbound, power-of-two capacity, preallocated, `_Atomic` head/tail
  with acquire/release ordering, padded to separate cache lines. SPSC
  per producer instead of one MPSC queue: simpler to get right, no
  CAS loop, and the sequencer's polling order is the only fairness
  policy.
- **Sequencer.** Polls the inbound rings round-robin, stamps `seq` and
  `ts_ns` (`CLOCK_MONOTONIC`), appends the request to the journal,
  calls `ex_submit`, and routes events to the right outbound ring by
  account.
- **Journal.** Fixed-size binary records, appended in sequence order.
  It is the input of the replay test and nothing else in M5 -- crash
  recovery is out of scope.
- **Fill tape.** Fixed-size binary records `{seq, exec_id, symbol,
  price_ticks, qty, buy_order_id, sell_order_id, buy_account,
  sell_account, aggressor_side, ts_ns}`, where `ts_ns` is the
  journaled timestamp of the request that caused the fill (so replay
  reproduces it exactly).
- **Backpressure.** A full inbound ring makes that producer spin or
  get `EX_ERR_BUSY`; the sequencer never blocks and never drops.

## Tests

`tests/exchange_tests.c`, run by `make test` and `make asan` alongside
the existing tests:

- **Unit tests** for each risk rule, reject reason, IOC, market-buy
  cost walk, and fill attribution, including market-order aggressors.
- **Conservation invariants**, checked after every request in a long
  random single-threaded run:
  - total cash across all accounts is constant
  - total position per symbol across all accounts is constant
  - no account has negative `available_cash` or `available_position`
  - each account's `reserved_cash` equals the sum over its open buy
    orders of `limit_price * open_qty` (recomputed from scratch and
    compared, the way M1 compares against a naive book)
- **Differential test** against a naive reference exchange (linear
  scans, no preallocation), in the spirit of M1's naive book.
- **Determinism test:** run the gateway with N producer threads,
  capture the journal and tape, replay the journal through a fresh
  `ex_t` single-threaded, and `memcmp` the tapes.

CI adds a **ThreadSanitizer** job (`make tsan`, mirroring `make asan`)
that runs the determinism test, since that's the test that actually
exercises the rings across threads.

## Benchmark

`bench/exchange_bench.c`, the counterpart of the prior-art project's
stress demo, reporting what the README's success metrics ask for:

- N producer threads x M symbols x K accounts, configurable
- end-to-end latency from submit to ack: **p50 / p99 / max**, not a mean
- sustained throughput (requests/s) through the sequencer
- the allocation-counter check from M3, extended to cover `ex_submit`

## Milestone breakdown

- **M5a -- state machine.** `exchange.{h,c}`, accounts, risk, IOC,
  market-buy walk, fill attribution; unit, invariant, and differential
  tests. Single-threaded only.
- **M5b -- gateway.** Rings, sequencer, journal, tape; determinism test;
  TSan job in CI.
- **M5c -- benchmark.** `bench/exchange_bench.c` with p50/p99.

## Out of scope for M5

- Networking (TCP, FIX, or any wire protocol) -- producers are threads
  in the same process.
- Crash recovery from the journal, persistence, snapshots.
- Short selling, margin, fees, settlement.
- Self-trade prevention.
- Auctions, halts, and the LOBSTER type-7 halt handling (that's M2's).
- Price-window re-centering (still an open item in `docs/DESIGN.md`).

## Open questions

- **Per-symbol tick sizes.** Supporting them means cash can no longer
  be in raw ticks; it needs a common minimum unit and a per-symbol
  multiplier. Deferred until something needs it.
- **Self-trade prevention.** Cancel-newest, cancel-oldest, or
  decrement-both are all common; M5 lets self-trades happen and
  records them on the tape.
- **Market-buy walk cost.** The `lob_depth()` pre-walk is O(levels);
  if it shows up in p99, a collar (convert to IOC at best ask + N
  ticks) is the cheaper alternative, at the cost of sometimes not
  filling everything that was affordable.
- **One sequencer vs one per symbol group.** A single sequencer gives
  one global order of events, which is what makes the replay test
  simple. Sharding by symbol would scale better but needs a
  cross-shard story for accounts. Not before M5c's numbers say it's
  needed.
