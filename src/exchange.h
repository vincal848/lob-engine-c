/* exchange.h -- the M5a exchange state machine, layered on lob.h.
 *
 * Design (see docs/EXCHANGE.md for the full spec):
 *   - Many symbols, one lob_t each, behind a name -> index table.
 *   - Accounts hold cash and per-symbol positions, plus reservations
 *     for what their open orders could still spend. Pre-trade risk
 *     rejects an order that can't be paid for before it reaches the
 *     book; there is no short selling.
 *   - The exchange assigns order ids (globally unique, from a
 *     counter) and keeps its own record per open order, so it never
 *     needs anything from lob.h beyond what lob.h already exposes.
 *   - ex_submit() processes one request to completion and reports
 *     everything it did through one event sink, in order: ACK or
 *     REJECT first, then FILLs, then DONE.
 *   - Plain C11: no threads, no clocks, no OS dependency. The caller
 *     (M5b's sequencer) stamps seq/ts_ns; they are only echoed back.
 *   - ex_new() allocates all of the exchange's own state. After it,
 *     ex_submit() never allocates. (ex_add_symbol() calls lob_new(),
 *     so it allocates; symbols and accounts are added at setup and
 *     frozen by the first ex_submit().)
 */
#ifndef EXCHANGE_H
#define EXCHANGE_H

#include <stddef.h>
#include <stdint.h>

#include "lob.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EX_NAME_MAX 16 /* symbol names, including the terminating NUL */

typedef enum { EX_LIMIT = 0, EX_IOC, EX_MARKET } ex_order_type_t;
typedef enum { EX_REQ_NEW = 0, EX_REQ_CANCEL, EX_REQ_REDUCE } ex_req_kind_t;

typedef enum {
    EX_REJ_UNKNOWN_ACCOUNT = 1,
    EX_REJ_UNKNOWN_SYMBOL,
    EX_REJ_BAD_QTY,            /* qty is zero, or a reduce to >= the open qty */
    EX_REJ_BAD_PRICE,          /* price outside the symbol's window */
    EX_REJ_INSUFFICIENT_CASH,  /* buy (or market-buy walk) costs more than available cash */
    EX_REJ_INSUFFICIENT_POSITION, /* sell of more than the available position */
    EX_REJ_UNKNOWN_ORDER,      /* cancel/reduce of an id that is not open */
    EX_REJ_NOT_OWNER,          /* cancel/reduce of another account's order */
    EX_REJ_CAPACITY            /* order records or the symbol's book pool are full */
} ex_reject_t;

typedef struct {
    uint64_t seq;          /* stamped by the sequencer; echoed in events */
    int64_t ts_ns;         /* stamped by the sequencer; echoed in events */
    ex_req_kind_t kind;
    uint32_t account;
    uint32_t symbol;       /* index; ignored for CANCEL/REDUCE (the order knows) */
    uint64_t order_id;     /* target of CANCEL/REDUCE; ignored for NEW (assigned) */
    lob_side_t side;
    ex_order_type_t type;
    int64_t price_ticks;   /* ignored for EX_MARKET */
    uint32_t qty;          /* the new (smaller) open qty for REDUCE */
} ex_request_t;

typedef enum {
    EX_EV_ACK = 0,  /* request accepted; for NEW, order_id is the assigned id */
    EX_EV_REJECT,   /* request refused, reason set; nothing changed */
    EX_EV_FILL,     /* one per side per fill: buyer's event, then seller's */
    EX_EV_DONE      /* order fully filled or cancelled; id retired, qty = unfilled */
} ex_event_kind_t;

/* One struct for all kinds; the comments say which fields each kind
 * sets. seq, ts_ns, account and order_id are set on every event
 * (order_id is 0 on the REJECT of a NEW, which never got an id).
 */
typedef struct {
    ex_event_kind_t kind;
    uint64_t seq;
    int64_t ts_ns;
    uint32_t account;
    uint64_t order_id;
    ex_reject_t reason;        /* REJECT */
    uint32_t symbol;           /* FILL */
    lob_side_t side;           /* FILL: which side of the fill this event is for */
    int64_t price_ticks;       /* FILL: the resting order's price */
    uint32_t qty;              /* FILL: filled qty; DONE: qty cancelled unfilled */
    uint64_t exec_id;          /* FILL: shared by the two events of one fill */
    uint64_t counter_order_id; /* FILL: the other side's order */
    int is_aggressor;          /* FILL: this event is for the aggressing order */
} ex_event_t;

/* Everything ex_submit emits goes through one sink, in order. The
 * sink must not call back into the exchange.
 */
typedef void (*ex_event_fn)(const ex_event_t *ev, void *user_data);

typedef struct {
    uint32_t max_symbols;
    uint32_t max_accounts;
    uint32_t max_orders;       /* open orders across all symbols */
    uint32_t book_orders;      /* resting-order pool of each symbol's lob_t */
    uint32_t max_window_ticks; /* widest price window a symbol may have; sizes
                                * the market-buy walk buffer so it is exact */
} ex_config_t;

typedef struct ex ex_t;

/* A read-only view of one open order, for tests and tooling. */
typedef struct {
    uint32_t account;
    uint32_t symbol;
    lob_side_t side;
    ex_order_type_t type;
    int64_t price_ticks;
    uint32_t open_qty;
    int64_t reserved; /* cash for a buy, shares for a sell */
} ex_order_info_t;

/* Creates an exchange with room for the configured counts. Returns
 * NULL on invalid arguments (any config field zero) or if the
 * allocation fails. The only allocation besides lob_new() in
 * ex_add_symbol().
 */
ex_t *ex_new(const ex_config_t *cfg);

/* Frees everything, including each symbol's book. Safe with NULL. */
void ex_free(ex_t *ex);

/* Adds a symbol with the price window [min_tick, max_tick] (min_tick
 * >= 0, window no wider than max_window_ticks, name shorter than
 * EX_NAME_MAX and not already present). Returns its index, or -1 on
 * any failure or once the first ex_submit() has frozen the tables.
 */
int ex_add_symbol(ex_t *ex, const char *name, int64_t min_tick, int64_t max_tick);

/* Adds an account with cash (>= 0) and a starting position in each
 * symbol (>= 0; array indexed by symbol, length = symbols added so
 * far; NULL means all zero). Add symbols first. Returns the account
 * index, or -1 on failure or once frozen. Cash is in tick units:
 * buying q at p ticks costs p * q.
 */
int ex_add_account(ex_t *ex, int64_t cash, const int64_t *positions);

/* Symbol name -> index, or -1 if unknown. */
int ex_symbol_index(const ex_t *ex, const char *name);

void ex_set_event_sink(ex_t *ex, ex_event_fn cb, void *user_data);

/* Processes one request completely. Never allocates. */
void ex_submit(ex_t *ex, const ex_request_t *req);

/* Read-only state, for tests and tooling. Each returns 0 on success
 * and -1 for an unknown account/symbol/order (outputs untouched).
 */
int ex_get_balance(const ex_t *ex, uint32_t account, int64_t *cash_out,
                   int64_t *reserved_cash_out);
int ex_get_position(const ex_t *ex, uint32_t account, uint32_t symbol,
                    int64_t *position_out, int64_t *reserved_position_out);
int ex_get_order(const ex_t *ex, uint64_t order_id, ex_order_info_t *out);

/* The symbol's book (read-only use; mutating it behind the exchange's
 * back breaks the order records). NULL for an unknown symbol.
 */
const lob_t *ex_book(const ex_t *ex, uint32_t symbol);

#ifdef __cplusplus
}
#endif

#endif /* EXCHANGE_H */
