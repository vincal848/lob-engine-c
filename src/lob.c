/* lob.c -- price-time priority limit order book.
 *
 * Internal layout, matching the plan in lob.h/docs/DESIGN.md:
 *   - struct lob holds two level arrays (bid_levels, ask_levels),
 *     each indexed directly by (price_ticks - min_tick).
 *   - orders[] is a preallocated pool; "pointers" between orders at
 *     the same level are pool indices threaded through prev/next, and
 *     free slots are threaded through the same next field into a
 *     free list (free_head).
 *   - hash_keys/hash_vals/hash_state is a fixed-capacity open
 *     addressing hash table (linear probing, tombstones) mapping
 *     order_id -> pool index, sized at 2x max_orders so it never
 *     fills and probe chains stay short.
 *
 * lob_new() is the only place that calls malloc. Everything else
 * (add/cancel/reduce/execute) touches only these preallocated
 * structures.
 */
#include "lob.h"

#include <stdlib.h>

#define HASH_EMPTY 0
#define HASH_OCCUPIED 1
#define HASH_TOMBSTONE 2

typedef struct {
    uint64_t order_id;
    int64_t price_ticks;
    uint32_t qty;
    uint8_t side;
    uint32_t prev; /* LOB_NULL_INDEX if none */
    uint32_t next; /* LOB_NULL_INDEX if none; also doubles as the
                     * free-list link when this slot is not in use */
} lob_order_t;

typedef struct {
    uint32_t head; /* oldest order at this price -- front of the FIFO */
    uint32_t tail; /* newest order at this price */
    uint64_t qty;  /* sum of resting qty at this price */
    uint32_t count;
} lob_level_t;

struct lob {
    uint32_t max_orders;
    int64_t min_tick;
    int64_t max_tick;
    uint64_t num_ticks;

    lob_order_t *orders;
    uint32_t free_head;

    lob_level_t *bid_levels;
    lob_level_t *ask_levels;

    int has_best_bid;
    int64_t best_bid_tick;
    int has_best_ask;
    int64_t best_ask_tick;

    uint64_t *hash_keys;
    uint32_t *hash_vals;
    uint8_t *hash_state;
    uint64_t hash_capacity; /* power of two */

    lob_fill_fn fill_cb;
    void *fill_user_data;
};

static uint64_t next_pow2(uint64_t x)
{
    uint64_t p = 1;
    while (p < x)
        p <<= 1;
    return p;
}

/* splitmix64 finalizer -- cheap, good enough avalanche for order ids
 * that are usually small sequential integers.
 */
static uint64_t hash_u64(uint64_t x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

static uint32_t hash_lookup(const lob_t *book, uint64_t order_id)
{
    uint64_t mask = book->hash_capacity - 1;
    uint64_t i = hash_u64(order_id) & mask;

    for (uint64_t probes = 0; probes < book->hash_capacity; probes++) {
        uint8_t state = book->hash_state[i];
        if (state == HASH_EMPTY)
            return LOB_NULL_INDEX;
        if (state == HASH_OCCUPIED && book->hash_keys[i] == order_id)
            return book->hash_vals[i];
        i = (i + 1) & mask;
    }
    return LOB_NULL_INDEX;
}

/* Returns 0 on success, -1 if order_id is already present. Capacity
 * is sized so this never runs out of room given at most max_orders
 * live entries: the probe sequence always reaches a true empty slot
 * within hash_capacity steps (live entries < capacity, by
 * pigeonhole), and that is also the point where it's safe to stop
 * looking for a duplicate. The slot actually used for the insert is
 * the *first* tombstone-or-empty seen along the way, so a tombstone
 * is reused immediately rather than only when it happens to sit
 * right next to a still-empty slot -- without that, a table with
 * many cancels but few live entries at any instant can still fill up
 * with tombstones over time and start rejecting inserts that should
 * succeed.
 */
static int hash_insert(lob_t *book, uint64_t order_id, uint32_t pool_idx)
{
    uint64_t mask = book->hash_capacity - 1;
    uint64_t i = hash_u64(order_id) & mask;
    uint64_t insert_slot = (uint64_t)-1;

    for (uint64_t probes = 0; probes < book->hash_capacity; probes++) {
        uint8_t state = book->hash_state[i];
        if (state == HASH_OCCUPIED && book->hash_keys[i] == order_id)
            return -1; /* duplicate */
        if (state == HASH_EMPTY) {
            if (insert_slot == (uint64_t)-1)
                insert_slot = i;
            break; /* a true empty slot ends the probe sequence */
        }
        if (state == HASH_TOMBSTONE && insert_slot == (uint64_t)-1)
            insert_slot = i;
        i = (i + 1) & mask;
    }

    if (insert_slot == (uint64_t)-1)
        return -1; /* table truly full -- shouldn't happen given sizing */

    book->hash_state[insert_slot] = HASH_OCCUPIED;
    book->hash_keys[insert_slot] = order_id;
    book->hash_vals[insert_slot] = pool_idx;
    return 0;
}

static void hash_remove(lob_t *book, uint64_t order_id)
{
    uint64_t mask = book->hash_capacity - 1;
    uint64_t i = hash_u64(order_id) & mask;

    for (uint64_t probes = 0; probes < book->hash_capacity; probes++) {
        uint8_t state = book->hash_state[i];
        if (state == HASH_EMPTY)
            return;
        if (state == HASH_OCCUPIED && book->hash_keys[i] == order_id) {
            book->hash_state[i] = HASH_TOMBSTONE;
            return;
        }
        i = (i + 1) & mask;
    }
}

static uint32_t pool_alloc(lob_t *book)
{
    if (book->free_head == LOB_NULL_INDEX)
        return LOB_NULL_INDEX;
    uint32_t idx = book->free_head;
    book->free_head = book->orders[idx].next;
    return idx;
}

static void pool_release(lob_t *book, uint32_t idx)
{
    book->orders[idx].next = book->free_head;
    book->free_head = idx;
}

static int price_in_range(const lob_t *book, int64_t price_ticks)
{
    return price_ticks >= book->min_tick && price_ticks <= book->max_tick;
}

static lob_level_t *level_at(lob_t *book, lob_side_t side, int64_t price_ticks)
{
    lob_level_t *levels = (side == LOB_SIDE_BUY) ? book->bid_levels : book->ask_levels;
    return &levels[(uint64_t)(price_ticks - book->min_tick)];
}

static const lob_level_t *level_at_const(const lob_t *book, lob_side_t side, int64_t price_ticks)
{
    const lob_level_t *levels = (side == LOB_SIDE_BUY) ? book->bid_levels : book->ask_levels;
    return &levels[(uint64_t)(price_ticks - book->min_tick)];
}

static void level_push_back(lob_t *book, lob_level_t *level, uint32_t order_idx)
{
    lob_order_t *order = &book->orders[order_idx];

    order->prev = level->tail;
    order->next = LOB_NULL_INDEX;
    if (level->tail != LOB_NULL_INDEX)
        book->orders[level->tail].next = order_idx;
    else
        level->head = order_idx;
    level->tail = order_idx;
    level->qty += order->qty;
    level->count++;
}

/* Unlinks order_idx from level's FIFO. Does not touch level->qty --
 * callers that remove an order because its qty hit zero have already
 * subtracted it incrementally during matching; a plain cancel
 * subtracts the full remaining qty itself.
 */
static void level_unlink_node(lob_t *book, lob_level_t *level, uint32_t order_idx)
{
    lob_order_t *order = &book->orders[order_idx];

    if (order->prev != LOB_NULL_INDEX)
        book->orders[order->prev].next = order->next;
    else
        level->head = order->next;

    if (order->next != LOB_NULL_INDEX)
        book->orders[order->next].prev = order->prev;
    else
        level->tail = order->prev;

    level->count--;
}

int lob_best_bid(const lob_t *book, int64_t *price_ticks_out, uint64_t *qty_out)
{
    if (!book->has_best_bid)
        return 0;
    if (price_ticks_out)
        *price_ticks_out = book->best_bid_tick;
    if (qty_out)
        *qty_out = level_at_const(book, LOB_SIDE_BUY, book->best_bid_tick)->qty;
    return 1;
}

int lob_best_ask(const lob_t *book, int64_t *price_ticks_out, uint64_t *qty_out)
{
    if (!book->has_best_ask)
        return 0;
    if (price_ticks_out)
        *price_ticks_out = book->best_ask_tick;
    if (qty_out)
        *qty_out = level_at_const(book, LOB_SIDE_SELL, book->best_ask_tick)->qty;
    return 1;
}

/* Called right after the cached best level has just emptied. Walks
 * outward from the stale best tick to the next occupied level. O(1)
 * when the next level is adjacent; O(k) for a gap of k empty ticks,
 * bounded by the configured price window in the worst case (an empty
 * book) -- see docs/DESIGN.md.
 */
static void refresh_best_bid(lob_t *book)
{
    for (int64_t t = book->best_bid_tick; t >= book->min_tick; t--) {
        if (level_at(book, LOB_SIDE_BUY, t)->count > 0) {
            book->best_bid_tick = t;
            return;
        }
    }
    book->has_best_bid = 0;
}

static void refresh_best_ask(lob_t *book)
{
    for (int64_t t = book->best_ask_tick; t <= book->max_tick; t++) {
        if (level_at(book, LOB_SIDE_SELL, t)->count > 0) {
            book->best_ask_tick = t;
            return;
        }
    }
    book->has_best_ask = 0;
}

/* Walks the opposite side of aggressor_side in price-time priority,
 * consuming resting orders at or better than limit_price_ticks, up to
 * qty total. Used by both a crossing limit order (limit_price_ticks
 * is the incoming order's price) and a market order
 * (limit_price_ticks is the book's far edge, so every price counts
 * as "at or better"). Returns the quantity actually filled.
 */
static uint32_t cross_against(lob_t *book, lob_side_t aggressor_side, uint64_t aggressor_order_id,
                                int64_t limit_price_ticks, uint32_t qty)
{
    lob_side_t resting_side = (aggressor_side == LOB_SIDE_BUY) ? LOB_SIDE_SELL : LOB_SIDE_BUY;
    uint32_t filled = 0;

    while (qty > 0) {
        int64_t best_tick;
        uint64_t best_qty;
        int have_best = (resting_side == LOB_SIDE_SELL)
            ? lob_best_ask(book, &best_tick, &best_qty)
            : lob_best_bid(book, &best_tick, &best_qty);
        if (!have_best)
            break;
        if (aggressor_side == LOB_SIDE_BUY && best_tick > limit_price_ticks)
            break;
        if (aggressor_side == LOB_SIDE_SELL && best_tick < limit_price_ticks)
            break;

        lob_level_t *level = level_at(book, resting_side, best_tick);

        while (level->count > 0 && qty > 0) {
            uint32_t order_idx = level->head;
            lob_order_t *order = &book->orders[order_idx];
            uint32_t trade_qty = (order->qty < qty) ? order->qty : qty;

            if (book->fill_cb) {
                lob_fill_t fill;
                fill.resting_order_id = order->order_id;
                fill.aggressor_order_id = aggressor_order_id;
                fill.price_ticks = best_tick;
                fill.qty = trade_qty;
                fill.aggressor_side = aggressor_side;
                book->fill_cb(&fill, book->fill_user_data);
            }

            qty -= trade_qty;
            filled += trade_qty;
            order->qty -= trade_qty;
            level->qty -= trade_qty;

            if (order->qty == 0) {
                uint64_t resting_id = order->order_id;
                level_unlink_node(book, level, order_idx);
                hash_remove(book, resting_id);
                pool_release(book, order_idx);
            }
        }

        if (level->count == 0) {
            if (resting_side == LOB_SIDE_SELL)
                refresh_best_ask(book);
            else
                refresh_best_bid(book);
        }
    }

    return filled;
}

lob_t *lob_new(uint32_t max_orders, int64_t min_tick, int64_t max_tick)
{
    if (max_orders == 0 || max_tick < min_tick)
        return NULL;

    uint64_t num_ticks = (uint64_t)(max_tick - min_tick) + 1;

    lob_t *book = calloc(1, sizeof(*book));
    if (!book)
        return NULL;

    book->max_orders = max_orders;
    book->min_tick = min_tick;
    book->max_tick = max_tick;
    book->num_ticks = num_ticks;

    book->orders = calloc(max_orders, sizeof(*book->orders));
    book->bid_levels = calloc(num_ticks, sizeof(*book->bid_levels));
    book->ask_levels = calloc(num_ticks, sizeof(*book->ask_levels));

    uint64_t hash_capacity = next_pow2((uint64_t)max_orders * 2);
    if (hash_capacity < 16)
        hash_capacity = 16;
    book->hash_keys = calloc(hash_capacity, sizeof(*book->hash_keys));
    book->hash_vals = calloc(hash_capacity, sizeof(*book->hash_vals));
    book->hash_state = calloc(hash_capacity, sizeof(*book->hash_state));
    book->hash_capacity = hash_capacity;

    if (!book->orders || !book->bid_levels || !book->ask_levels ||
        !book->hash_keys || !book->hash_vals || !book->hash_state) {
        lob_free(book);
        return NULL;
    }

    for (uint64_t i = 0; i < num_ticks; i++) {
        book->bid_levels[i].head = book->bid_levels[i].tail = LOB_NULL_INDEX;
        book->ask_levels[i].head = book->ask_levels[i].tail = LOB_NULL_INDEX;
    }

    for (uint32_t i = 0; i < max_orders; i++)
        book->orders[i].next = (i + 1 < max_orders) ? (i + 1) : LOB_NULL_INDEX;
    book->free_head = 0;

    return book;
}

void lob_free(lob_t *book)
{
    if (!book)
        return;
    free(book->orders);
    free(book->bid_levels);
    free(book->ask_levels);
    free(book->hash_keys);
    free(book->hash_vals);
    free(book->hash_state);
    free(book);
}

void lob_set_fill_callback(lob_t *book, lob_fill_fn cb, void *user_data)
{
    book->fill_cb = cb;
    book->fill_user_data = user_data;
}

lob_status_t lob_add_limit(lob_t *book, uint64_t order_id, lob_side_t side,
                            int64_t price_ticks, uint32_t qty,
                            uint32_t *filled_qty_out)
{
    if (filled_qty_out)
        *filled_qty_out = 0;
    if (qty == 0)
        return LOB_ERR_INVALID_QTY;
    if (!price_in_range(book, price_ticks))
        return LOB_ERR_PRICE_OUT_OF_RANGE;
    if (hash_lookup(book, order_id) != LOB_NULL_INDEX)
        return LOB_ERR_DUPLICATE_ID;

    uint32_t filled = cross_against(book, side, order_id, price_ticks, qty);
    if (filled_qty_out)
        *filled_qty_out = filled;

    uint32_t remaining = qty - filled;
    if (remaining == 0)
        return LOB_OK;

    uint32_t idx = pool_alloc(book);
    if (idx == LOB_NULL_INDEX)
        return LOB_ERR_POOL_EXHAUSTED;

    lob_order_t *order = &book->orders[idx];
    order->order_id = order_id;
    order->price_ticks = price_ticks;
    order->qty = remaining;
    order->side = (uint8_t)side;
    order->prev = LOB_NULL_INDEX;
    order->next = LOB_NULL_INDEX;

    lob_level_t *level = level_at(book, side, price_ticks);
    int was_empty = (level->count == 0);
    level_push_back(book, level, idx);

    if (hash_insert(book, order_id, idx) != 0) {
        /* Defensive only: capacity is sized for max_orders and
         * order_id was already confirmed absent above, so this
         * should not happen.
         */
        level_unlink_node(book, level, idx);
        level->qty -= order->qty;
        pool_release(book, idx);
        return LOB_ERR_POOL_EXHAUSTED;
    }

    if (was_empty) {
        if (side == LOB_SIDE_BUY) {
            if (!book->has_best_bid || price_ticks > book->best_bid_tick) {
                book->best_bid_tick = price_ticks;
                book->has_best_bid = 1;
            }
        } else {
            if (!book->has_best_ask || price_ticks < book->best_ask_tick) {
                book->best_ask_tick = price_ticks;
                book->has_best_ask = 1;
            }
        }
    }

    return LOB_OK;
}

lob_status_t lob_cancel(lob_t *book, uint64_t order_id)
{
    uint32_t idx = hash_lookup(book, order_id);
    if (idx == LOB_NULL_INDEX)
        return LOB_ERR_NOT_FOUND;

    lob_order_t *order = &book->orders[idx];
    lob_side_t side = (lob_side_t)order->side;
    int64_t price = order->price_ticks;
    lob_level_t *level = level_at(book, side, price);

    level_unlink_node(book, level, idx);
    level->qty -= order->qty;
    hash_remove(book, order_id);
    pool_release(book, idx);

    if (level->count == 0) {
        if (side == LOB_SIDE_BUY && book->has_best_bid && book->best_bid_tick == price)
            refresh_best_bid(book);
        else if (side == LOB_SIDE_SELL && book->has_best_ask && book->best_ask_tick == price)
            refresh_best_ask(book);
    }

    return LOB_OK;
}

lob_status_t lob_reduce(lob_t *book, uint64_t order_id, uint32_t new_qty)
{
    if (new_qty == 0)
        return LOB_ERR_INVALID_QTY;

    uint32_t idx = hash_lookup(book, order_id);
    if (idx == LOB_NULL_INDEX)
        return LOB_ERR_NOT_FOUND;

    lob_order_t *order = &book->orders[idx];
    if (new_qty >= order->qty)
        return LOB_ERR_INVALID_QTY;

    lob_level_t *level = level_at(book, (lob_side_t)order->side, order->price_ticks);
    level->qty -= (order->qty - new_qty);
    order->qty = new_qty;

    return LOB_OK;
}

lob_status_t lob_execute_market(lob_t *book, lob_side_t side, uint32_t qty,
                                 uint32_t *filled_qty_out)
{
    if (filled_qty_out)
        *filled_qty_out = 0;
    if (qty == 0)
        return LOB_ERR_INVALID_QTY;

    int64_t limit_price_ticks = (side == LOB_SIDE_BUY) ? book->max_tick : book->min_tick;
    uint32_t filled = cross_against(book, side, 0, limit_price_ticks, qty);
    if (filled_qty_out)
        *filled_qty_out = filled;

    return LOB_OK;
}

size_t lob_depth(const lob_t *book, lob_side_t side,
                  lob_level_snapshot_t *out, size_t max_levels)
{
    size_t n = 0;

    if (max_levels == 0)
        return 0;

    if (side == LOB_SIDE_BUY) {
        if (!book->has_best_bid)
            return 0;
        for (int64_t t = book->best_bid_tick; t >= book->min_tick && n < max_levels; t--) {
            const lob_level_t *level = level_at_const(book, LOB_SIDE_BUY, t);
            if (level->count > 0) {
                out[n].price_ticks = t;
                out[n].qty = level->qty;
                out[n].order_count = level->count;
                n++;
            }
        }
    } else {
        if (!book->has_best_ask)
            return 0;
        for (int64_t t = book->best_ask_tick; t <= book->max_tick && n < max_levels; t++) {
            const lob_level_t *level = level_at_const(book, LOB_SIDE_SELL, t);
            if (level->count > 0) {
                out[n].price_ticks = t;
                out[n].qty = level->qty;
                out[n].order_count = level->count;
                n++;
            }
        }
    }

    return n;
}
