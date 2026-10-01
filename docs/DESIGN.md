# Design notes

This is the detail behind the "Design" section of the README: memory
layout and the complexity of each operation. Read `src/lob.h` for the
actual API and `src/lob.c`'s file header for the short version of all
of this.

## Memory layout

One book owns five preallocated regions, all sized at `lob_new()` and
never resized:

```
lob_t
 |
 +-- orders[max_orders]            (the pool)
 |     [id, price, qty, side, prev, next]  x max_orders
 |     free slots threaded through `next` into a free list
 |
 +-- bid_levels[num_ticks]         (direct-indexed by price - min_tick)
 |     [head, tail, qty, count]  x num_ticks
 |
 +-- ask_levels[num_ticks]         (same layout, ask side)
 |
 +-- hash_keys / hash_vals / hash_state[hash_capacity]
       order_id -> pool index, open addressing, hash_capacity = 2x max_orders
```

A resting order's "position in the book" is three integers, not three
pointers: its pool index, and the index of the level it sits in
(`price_ticks - min_tick`). Walking a level's FIFO is index-chasing
through one contiguous array (`orders[]`), not pointer-chasing through
scattered heap nodes -- that's the cache-friendliness the README's
Motivation section is about.

```
bid_levels[...] ask_levels[...]
      |                |
      v                v
+-----------+    +-----------+
| tick 101  |    | tick 100  |
| head -----+--> | order 7   |
| tail      |    | prev/next |--> order 12 --> order 3 --> (none)
+-----------+    +-----------+
```

Each level only stores `head`/`tail` indices into `orders[]`; the FIFO
itself is the `prev`/`next` fields on the order structs, so adding a
level costs nothing beyond the two sentinel indices.

## Why direct-indexed levels, not a sorted level array

Two ways to store "the set of occupied price levels": (a) an array
indexed directly by tick offset from a moving base (what this uses),
or (b) a sorted array/tree of just the occupied levels, searched by
price.

(a) makes "find the level for this price" an O(1) array index, which
is the operation on the hot path for every single add, cancel, and
reduce. (b) makes that an O(log n) search, but avoids ever allocating
space for unoccupied prices. For a matching engine replaying LOBSTER
data, (a) is the right trade: the number of distinct ticks a liquid
name trades at in a day is small and known in advance (bound it and
preallocate it), while the number of add/cancel/reduce operations is
huge and is exactly what needs to be fast. The cost of (a) is a
bounded price window configured at `lob_new()` -- if the market
trades outside it, that's `LOB_ERR_PRICE_OUT_OF_RANGE`, not a crash.
A production version would re-center the window (rebase `min_tick`)
when the market walks toward an edge; this scaffold doesn't do that
yet.

## Complexity

| Operation | Cost | Notes |
|---|---|---|
| `lob_add_limit`, no crossing | O(1) expected | hash insert + level push, both O(1) |
| `lob_add_limit`, crosses m resting orders | O(m) | one fill callback per resting order consumed |
| `lob_cancel` | O(1) expected, worst O(k) | O(1) unless it empties the best level, then O(k) to find the next occupied tick, k = gap in ticks |
| `lob_reduce` | O(1) expected | hash lookup + in-place qty edit; FIFO position untouched |
| `lob_execute_market`, fills m orders | O(m) | same walk as a crossing limit order, no price bound |
| `lob_best_bid` / `lob_best_ask` | O(1) | cached, refreshed lazily on emptying the best level |
| `lob_depth(n)` | O(n + gaps) | walks occupied ticks outward from best; gaps between occupied prices are a wasted scan |

"Hash expected O(1)" assumes the usual open-addressing caveat: true
only because `hash_capacity` is fixed at 2x `max_orders`, so load
factor never exceeds 0.5 and probe chains stay short. Nothing here
ever rehashes or grows -- that's also why there's no allocation after
`lob_new()`.

The one spot this scaffold is honestly not O(1) in the worst case is
the best-price refresh after the best level empties: it scans
outward one tick at a time until it finds the next occupied level or
falls off the configured price window. For a liquid name with a tight
price window relative to its tick size this is fine in practice; it
is the first thing to revisit if M3's benchmark shows p99 latency
dominated by cancels at the top of book during a wide, thin market.
