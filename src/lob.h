/* lob.h -- public interface to the limit order book matching engine.
 *
 * Design (see docs/DESIGN.md for the full write-up):
 *   - Prices are integer ticks (no floating point anywhere near the
 *     hot path).
 *   - Each side of the book is a fixed-size array of price levels,
 *     indexed directly by (price_ticks - min_tick). That makes
 *     "find the level for this price" an O(1) array index instead of
 *     a search, at the cost of a bounded price window configured up
 *     front at lob_new().
 *   - Each level is a FIFO (price-time priority) of orders, stored as
 *     an intrusive doubly linked list threaded through a preallocated
 *     order pool -- "pointers" are indices into that pool, not
 *     malloc'd nodes.
 *   - order_id -> pool index is a fixed-capacity open-addressing hash
 *     table, so cancel-by-id is O(1) expected instead of O(n).
 *   - lob_new() does the only allocation. Every operation after that
 *     (add, cancel, reduce, market execute) touches only the
 *     preallocated pool, levels, and hash table -- no malloc/free on
 *     the hot path.
 */
#ifndef LOB_H
#define LOB_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sentinel used for "no order"/"no next"/"no prev" pool indices. */
#define LOB_NULL_INDEX UINT32_MAX

typedef enum {
    LOB_SIDE_BUY = 0,
    LOB_SIDE_SELL = 1
} lob_side_t;

typedef enum {
    LOB_OK = 0,
    LOB_ERR_POOL_EXHAUSTED,    /* order pool is full */
    LOB_ERR_DUPLICATE_ID,      /* order_id already resting in the book */
    LOB_ERR_NOT_FOUND,         /* order_id not resting in the book */
    LOB_ERR_PRICE_OUT_OF_RANGE,/* price_ticks outside [min_tick, max_tick] */
    LOB_ERR_INVALID_QTY        /* qty is zero, or a reduce asked for >= current qty */
} lob_status_t;

/* One resting-order fill. Reported once per resting order touched, so
 * a single aggressive order can produce several fill callbacks (one
 * per price-time-priority level it walks through).
 */
typedef struct {
    uint64_t resting_order_id;
    uint64_t aggressor_order_id; /* 0 for a market order, which has no id */
    int64_t price_ticks;         /* price the fill happened at (resting side's price) */
    uint32_t qty;
    lob_side_t aggressor_side;
} lob_fill_t;

typedef void (*lob_fill_fn)(const lob_fill_t *fill, void *user_data);

typedef struct {
    int64_t price_ticks;
    uint64_t qty;
    uint32_t order_count;
} lob_level_snapshot_t;

typedef struct lob lob_t;

/* Creates a book with a preallocated pool of max_orders resting
 * orders and a price ladder covering ticks [min_tick, max_tick]
 * inclusive on both sides. Returns NULL on invalid arguments or if
 * the one-time allocation fails.
 */
lob_t *lob_new(uint32_t max_orders, int64_t min_tick, int64_t max_tick);

/* Frees everything lob_new allocated. Safe to call with NULL. */
void lob_free(lob_t *book);

/* Registers a callback invoked once per resting order consumed by a
 * crossing limit order or a market order. Pass cb == NULL to disable.
 */
void lob_set_fill_callback(lob_t *book, lob_fill_fn cb, void *user_data);

/* Adds a limit order. It first crosses the opposite side in
 * price-time priority (consuming resting orders at or better than
 * price_ticks), then rests any remainder on the book at price_ticks.
 * *filled_qty_out (if non-NULL) receives how much of qty was filled
 * immediately by crossing.
 */
lob_status_t lob_add_limit(lob_t *book, uint64_t order_id, lob_side_t side,
                            int64_t price_ticks, uint32_t qty,
                            uint32_t *filled_qty_out);

/* Removes a resting order entirely. */
lob_status_t lob_cancel(lob_t *book, uint64_t order_id);

/* Reduces a resting order's quantity in place (its position in the
 * FIFO, and therefore its time priority, is unchanged). new_qty must
 * be greater than zero and strictly less than the order's current
 * quantity; use lob_cancel for a full cancel.
 */
lob_status_t lob_reduce(lob_t *book, uint64_t order_id, uint32_t new_qty);

/* Walks the opposite side in price-time priority until qty is filled
 * or the side is empty. *filled_qty_out (if non-NULL) receives the
 * amount actually filled; qty - *filled_qty_out is the unfilled
 * remainder (a market order never rests).
 */
lob_status_t lob_execute_market(lob_t *book, lob_side_t side, uint32_t qty,
                                 uint32_t *filled_qty_out);

/* Best bid/ask. Returns false (and leaves the outputs untouched) if
 * that side of the book is empty.
 */
int lob_best_bid(const lob_t *book, int64_t *price_ticks_out, uint64_t *qty_out);
int lob_best_ask(const lob_t *book, int64_t *price_ticks_out, uint64_t *qty_out);

/* Fills out[] with up to max_levels occupied price levels on side,
 * best price first. Returns the number of levels written.
 */
size_t lob_depth(const lob_t *book, lob_side_t side,
                  lob_level_snapshot_t *out, size_t max_levels);

/* Looks up a resting order by id. On LOB_OK, writes its side, price,
 * and remaining quantity to whichever outputs are non-NULL; returns
 * LOB_ERR_NOT_FOUND (outputs untouched) if it isn't resting. Read
 * only, O(1) expected.
 */
lob_status_t lob_get_order(const lob_t *book, uint64_t order_id, lob_side_t *side_out,
                            int64_t *price_ticks_out, uint32_t *qty_out);

/* Copies the ids of the orders resting at price_ticks on side into
 * ids_out[], oldest (front of the FIFO) first, writing at most
 * max_ids. Returns the total number of orders at that level, which
 * can exceed max_ids; 0 for an empty level or a price outside the
 * book's window. Read only, O(orders at the level).
 */
size_t lob_level_orders(const lob_t *book, lob_side_t side, int64_t price_ticks,
                         uint64_t *ids_out, size_t max_ids);

#ifdef __cplusplus
}
#endif

#endif /* LOB_H */
