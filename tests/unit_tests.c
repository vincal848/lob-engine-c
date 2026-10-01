/* Assert-based tests for the M1 core (src/lob.c). Run via `make test`.
 *
 * The last test, test_differential_against_naive_reference, is the
 * one that matters most: it replays 100k random operations through
 * the real engine and through a deliberately dumb reference
 * implementation (linear-scan arrays, no hashing, no direct
 * indexing) side by side, and checks every step agrees. The other
 * tests pin specific behaviors the differential test wouldn't
 * localize a failure to on its own.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "../src/lob.h"

#define MAX_CAPTURED_FILLS 64
static lob_fill_t captured_fills[MAX_CAPTURED_FILLS];
static size_t captured_fill_count;

static void capture_fill(const lob_fill_t *fill, void *user_data)
{
    (void)user_data;
    if (captured_fill_count < MAX_CAPTURED_FILLS)
        captured_fills[captured_fill_count++] = *fill;
}

static void reset_fill_capture(void)
{
    captured_fill_count = 0;
}

static void test_fifo_at_a_level(void)
{
    lob_t *book = lob_new(16, 0, 100);
    assert(book != NULL);
    lob_set_fill_callback(book, capture_fill, NULL);

    uint32_t filled = 0;
    assert(lob_add_limit(book, 1, LOB_SIDE_BUY, 5, 1, &filled) == LOB_OK && filled == 0);
    assert(lob_add_limit(book, 2, LOB_SIDE_BUY, 5, 2, &filled) == LOB_OK && filled == 0);
    assert(lob_add_limit(book, 3, LOB_SIDE_BUY, 5, 3, &filled) == LOB_OK && filled == 0);

    reset_fill_capture();
    assert(lob_execute_market(book, LOB_SIDE_SELL, 4, &filled) == LOB_OK);
    assert(filled == 4);

    /* Resting FIFO order was 1, 2, 3 -- the market sell must drain
     * them in exactly that order: all of 1, all of 2, then only 1
     * unit of 3.
     */
    assert(captured_fill_count == 3);
    assert(captured_fills[0].resting_order_id == 1 && captured_fills[0].qty == 1);
    assert(captured_fills[1].resting_order_id == 2 && captured_fills[1].qty == 2);
    assert(captured_fills[2].resting_order_id == 3 && captured_fills[2].qty == 1);

    int64_t price;
    uint64_t qty;
    assert(lob_best_bid(book, &price, &qty));
    assert(price == 5 && qty == 2); /* 2 units left of order 3 */

    lob_free(book);
}

static void test_crossing_limit_fills_and_rests_remainder(void)
{
    lob_t *book = lob_new(16, 0, 1000);
    assert(book != NULL);
    lob_set_fill_callback(book, capture_fill, NULL);

    uint32_t filled = 0;
    assert(lob_add_limit(book, 10, LOB_SIDE_SELL, 100, 5, &filled) == LOB_OK && filled == 0);
    assert(lob_add_limit(book, 11, LOB_SIDE_SELL, 101, 5, &filled) == LOB_OK && filled == 0);

    reset_fill_capture();
    assert(lob_add_limit(book, 20, LOB_SIDE_BUY, 101, 7, &filled) == LOB_OK);
    assert(filled == 7); /* fully filled by crossing; nothing rests */

    assert(captured_fill_count == 2);
    assert(captured_fills[0].resting_order_id == 10 && captured_fills[0].price_ticks == 100 &&
           captured_fills[0].qty == 5);
    assert(captured_fills[1].resting_order_id == 11 && captured_fills[1].price_ticks == 101 &&
           captured_fills[1].qty == 2);

    int64_t price;
    uint64_t qty;
    assert(lob_best_ask(book, &price, &qty));
    assert(price == 101 && qty == 3); /* order 11 had 5, 2 consumed */
    assert(!lob_best_bid(book, &price, &qty)); /* order 20 fully filled, nothing rests */

    lob_free(book);
}

static void test_partial_cancel_preserves_time_priority(void)
{
    lob_t *book = lob_new(16, 0, 100);
    assert(book != NULL);
    lob_set_fill_callback(book, capture_fill, NULL);

    uint32_t filled = 0;
    assert(lob_add_limit(book, 1, LOB_SIDE_BUY, 5, 3, &filled) == LOB_OK);
    assert(lob_add_limit(book, 2, LOB_SIDE_BUY, 5, 2, &filled) == LOB_OK);

    /* Reduce order 1 from 3 down to 1. If this touched time priority
     * (e.g. by re-inserting at the tail) order 2 would be filled
     * first below; it must not be.
     */
    assert(lob_reduce(book, 1, 1) == LOB_OK);

    reset_fill_capture();
    assert(lob_execute_market(book, LOB_SIDE_SELL, 2, &filled) == LOB_OK);
    assert(filled == 2);

    assert(captured_fill_count == 2);
    assert(captured_fills[0].resting_order_id == 1 && captured_fills[0].qty == 1);
    assert(captured_fills[1].resting_order_id == 2 && captured_fills[1].qty == 1);

    lob_free(book);
}

static void test_full_cancel_removes_and_best_updates(void)
{
    lob_t *book = lob_new(16, 0, 100);
    assert(book != NULL);

    int64_t price;
    uint64_t qty;
    uint32_t filled;

    assert(lob_add_limit(book, 1, LOB_SIDE_BUY, 10, 5, &filled) == LOB_OK);
    assert(lob_add_limit(book, 2, LOB_SIDE_BUY, 9, 5, &filled) == LOB_OK);
    assert(lob_best_bid(book, &price, &qty) && price == 10);

    assert(lob_cancel(book, 1) == LOB_OK);
    assert(lob_best_bid(book, &price, &qty) && price == 9);

    assert(lob_cancel(book, 2) == LOB_OK);
    assert(!lob_best_bid(book, &price, &qty));

    /* Canceling something that isn't resting is an error, not a crash. */
    assert(lob_cancel(book, 999) == LOB_ERR_NOT_FOUND);

    lob_free(book);
}

static void test_market_order_larger_than_book_reports_unfilled(void)
{
    lob_t *book = lob_new(16, 0, 100);
    assert(book != NULL);

    uint32_t filled = 0;
    assert(lob_add_limit(book, 1, LOB_SIDE_SELL, 50, 3, &filled) == LOB_OK);

    assert(lob_execute_market(book, LOB_SIDE_BUY, 10, &filled) == LOB_OK);
    assert(filled == 3); /* only 3 were available; 7 goes unfilled */

    int64_t price;
    uint64_t qty;
    assert(!lob_best_ask(book, &price, &qty)); /* side fully emptied */

    lob_free(book);
}

static void test_pool_exhaustion_returns_error_not_crash(void)
{
    lob_t *book = lob_new(2, 0, 100); /* room for exactly two resting orders */
    assert(book != NULL);

    uint32_t filled = 0;
    assert(lob_add_limit(book, 1, LOB_SIDE_BUY, 5, 1, &filled) == LOB_OK);
    assert(lob_add_limit(book, 2, LOB_SIDE_BUY, 6, 1, &filled) == LOB_OK);

    /* Pool is full; a third resting order must fail cleanly. */
    assert(lob_add_limit(book, 3, LOB_SIDE_BUY, 7, 1, &filled) == LOB_ERR_POOL_EXHAUSTED);

    /* The book must be unaffected by the failed add. */
    int64_t price;
    uint64_t qty;
    assert(lob_best_bid(book, &price, &qty) && price == 6);

    /* Freeing a slot must make room again. */
    assert(lob_cancel(book, 2) == LOB_OK);
    assert(lob_add_limit(book, 3, LOB_SIDE_BUY, 7, 1, &filled) == LOB_OK);

    lob_free(book);
}

static void test_get_order_and_level_orders(void)
{
    lob_t *book = lob_new(16, 0, 100);
    assert(book != NULL);

    uint32_t filled = 0;
    assert(lob_add_limit(book, 7, LOB_SIDE_SELL, 40, 5, &filled) == LOB_OK);
    assert(lob_add_limit(book, 8, LOB_SIDE_SELL, 40, 6, &filled) == LOB_OK);
    assert(lob_add_limit(book, 9, LOB_SIDE_SELL, 40, 7, &filled) == LOB_OK);
    assert(lob_reduce(book, 8, 2) == LOB_OK);

    lob_side_t side;
    int64_t price;
    uint32_t qty;
    assert(lob_get_order(book, 8, &side, &price, &qty) == LOB_OK);
    assert(side == LOB_SIDE_SELL && price == 40 && qty == 2);
    assert(lob_get_order(book, 8, NULL, NULL, NULL) == LOB_OK);
    assert(lob_get_order(book, 99, &side, &price, &qty) == LOB_ERR_NOT_FOUND);

    /* FIFO order, and the return value counts past max_ids. */
    uint64_t ids[2];
    assert(lob_level_orders(book, LOB_SIDE_SELL, 40, ids, 2) == 3);
    assert(ids[0] == 7 && ids[1] == 8);

    assert(lob_cancel(book, 7) == LOB_OK);
    assert(lob_level_orders(book, LOB_SIDE_SELL, 40, ids, 2) == 2);
    assert(ids[0] == 8 && ids[1] == 9);

    /* Empty level, wrong side, and out-of-window prices are all 0. */
    assert(lob_level_orders(book, LOB_SIDE_BUY, 40, ids, 2) == 0);
    assert(lob_level_orders(book, LOB_SIDE_SELL, 41, ids, 2) == 0);
    assert(lob_level_orders(book, LOB_SIDE_SELL, 1000, ids, 2) == 0);
    assert(lob_level_orders(book, LOB_SIDE_SELL, -1, ids, 2) == 0);

    lob_free(book);
}

/* --- Naive reference book, used only by the differential test below.
 * Deliberately the dumbest correct implementation: unsorted arrays,
 * linear scans for everything. Its only job is to be obviously
 * correct so disagreements point at the real engine.
 */
#define NAIVE_CAP 1000

typedef struct {
    uint64_t id;
    int64_t price;
    uint32_t qty;
} naive_order_t;

typedef struct {
    naive_order_t items[NAIVE_CAP];
    size_t n;
} naive_side_t;

static void naive_push(naive_side_t *s, uint64_t id, int64_t price, uint32_t qty)
{
    assert(s->n < NAIVE_CAP);
    s->items[s->n].id = id;
    s->items[s->n].price = price;
    s->items[s->n].qty = qty;
    s->n++;
}

/* Order-preserving removal: the array's order IS time priority, so
 * shifting the tail down (instead of swap-with-last) is required.
 */
static void naive_remove_at(naive_side_t *s, size_t idx)
{
    for (size_t i = idx; i + 1 < s->n; i++)
        s->items[i] = s->items[i + 1];
    s->n--;
}

/* Index of the best order on this side: lowest price for an ask
 * side (want_min=1), highest price for a bid side (want_min=0); ties
 * broken by array position, i.e. insertion order.
 */
static int naive_best_index(const naive_side_t *s, int want_min)
{
    int best = -1;
    for (size_t i = 0; i < s->n; i++) {
        if (best == -1) {
            best = (int)i;
            continue;
        }
        if (want_min ? s->items[i].price < s->items[(size_t)best].price
                     : s->items[i].price > s->items[(size_t)best].price)
            best = (int)i;
    }
    return best;
}

static uint32_t naive_cross(naive_side_t *resting, int resting_is_ask, int has_limit,
                             int64_t limit_price, uint32_t qty)
{
    uint32_t filled = 0;
    while (qty > 0 && resting->n > 0) {
        int idx = naive_best_index(resting, resting_is_ask);
        int64_t price = resting->items[(size_t)idx].price;
        if (has_limit) {
            if (resting_is_ask && price > limit_price)
                break;
            if (!resting_is_ask && price < limit_price)
                break;
        }
        uint32_t cur = resting->items[(size_t)idx].qty;
        uint32_t trade = (cur < qty) ? cur : qty;
        resting->items[(size_t)idx].qty -= trade;
        qty -= trade;
        filled += trade;
        if (resting->items[(size_t)idx].qty == 0)
            naive_remove_at(resting, (size_t)idx);
    }
    return filled;
}

static uint32_t naive_add_limit(naive_side_t *own, naive_side_t *opp, int own_is_bid,
                                 uint64_t id, int64_t price, uint32_t qty)
{
    uint32_t filled = naive_cross(opp, own_is_bid, 1, price, qty);
    uint32_t remaining = qty - filled;
    if (remaining > 0)
        naive_push(own, id, price, remaining);
    return filled;
}

static void test_differential_against_naive_reference(void)
{
    enum { MAX_ORDERS = 1000, MIN_TICK = 0, MAX_TICK = 199, ITERATIONS = 100000 };

    lob_t *book = lob_new(MAX_ORDERS, MIN_TICK, MAX_TICK);
    assert(book != NULL);

    naive_side_t naive_bids = {.n = 0};
    naive_side_t naive_asks = {.n = 0};

    srand(42); /* fixed seed: this test must be reproducible */
    uint64_t next_id = 1;

    for (int iter = 0; iter < ITERATIONS; iter++) {
        int op = rand() % 4;

        if (op == 0) { /* add limit */
            if (naive_bids.n + naive_asks.n >= MAX_ORDERS)
                continue; /* avoid pool exhaustion here; that's its own test */

            int side_is_buy = rand() % 2;
            int64_t price = MIN_TICK + rand() % (MAX_TICK - MIN_TICK + 1);
            uint32_t qty = 1 + (uint32_t)(rand() % 20);
            uint64_t id = next_id++;

            uint32_t engine_filled = 0;
            lob_status_t st = lob_add_limit(book, id, side_is_buy ? LOB_SIDE_BUY : LOB_SIDE_SELL,
                                             price, qty, &engine_filled);
            assert(st == LOB_OK);

            uint32_t naive_filled = side_is_buy
                ? naive_add_limit(&naive_bids, &naive_asks, 1, id, price, qty)
                : naive_add_limit(&naive_asks, &naive_bids, 0, id, price, qty);
            assert(engine_filled == naive_filled);
        } else if (op == 1) { /* cancel */
            size_t total = naive_bids.n + naive_asks.n;
            if (total == 0)
                continue;
            size_t pick = (size_t)rand() % total;
            uint64_t id;
            if (pick < naive_bids.n) {
                id = naive_bids.items[pick].id;
                naive_remove_at(&naive_bids, pick);
            } else {
                pick -= naive_bids.n;
                id = naive_asks.items[pick].id;
                naive_remove_at(&naive_asks, pick);
            }
            assert(lob_cancel(book, id) == LOB_OK);
        } else if (op == 2) { /* partial reduce */
            size_t total = naive_bids.n + naive_asks.n;
            if (total == 0)
                continue;
            size_t pick = (size_t)rand() % total;
            naive_side_t *s = (pick < naive_bids.n) ? &naive_bids : &naive_asks;
            size_t idx = (pick < naive_bids.n) ? pick : pick - naive_bids.n;
            uint32_t cur = s->items[idx].qty;
            if (cur <= 1)
                continue; /* nothing smaller to reduce to */
            uint32_t new_qty = 1 + (uint32_t)(rand() % (int)(cur - 1));
            uint64_t id = s->items[idx].id;

            assert(lob_reduce(book, id, new_qty) == LOB_OK);
            s->items[idx].qty = new_qty;
        } else { /* market order */
            int side_is_buy = rand() % 2;
            uint32_t qty = 1 + (uint32_t)(rand() % 20);

            uint32_t engine_filled = 0;
            assert(lob_execute_market(book, side_is_buy ? LOB_SIDE_BUY : LOB_SIDE_SELL, qty,
                                       &engine_filled) == LOB_OK);

            uint32_t naive_filled = side_is_buy
                ? naive_cross(&naive_asks, 1, 0, 0, qty)
                : naive_cross(&naive_bids, 0, 0, 0, qty);
            assert(engine_filled == naive_filled);
        }

        /* Core differential check: the engine and the dumb reference
         * must agree on the best price every side, and the book must
         * never cross itself.
         */
        int64_t engine_bid_price = 0, engine_ask_price = 0;
        uint64_t engine_bid_qty = 0, engine_ask_qty = 0;
        int engine_has_bid = lob_best_bid(book, &engine_bid_price, &engine_bid_qty);
        int engine_has_ask = lob_best_ask(book, &engine_ask_price, &engine_ask_qty);

        int naive_bid_idx = naive_best_index(&naive_bids, 0);
        int naive_ask_idx = naive_best_index(&naive_asks, 1);

        assert(engine_has_bid == (naive_bid_idx != -1));
        assert(engine_has_ask == (naive_ask_idx != -1));
        if (engine_has_bid)
            assert(engine_bid_price == naive_bids.items[(size_t)naive_bid_idx].price);
        if (engine_has_ask)
            assert(engine_ask_price == naive_asks.items[(size_t)naive_ask_idx].price);
        if (engine_has_bid && engine_has_ask)
            assert(engine_bid_price < engine_ask_price);
    }

    lob_free(book);
}

int main(void)
{
    test_fifo_at_a_level();
    test_crossing_limit_fills_and_rests_remainder();
    test_partial_cancel_preserves_time_priority();
    test_full_cancel_removes_and_best_updates();
    test_market_order_larger_than_book_reports_unfilled();
    test_pool_exhaustion_returns_error_not_crash();
    test_get_order_and_level_orders();
    test_differential_against_naive_reference();

    printf("all unit tests passed\n");
    return 0;
}
