/* exchange.c -- the M5a exchange state machine.
 *
 * Internal layout, matching the plan in exchange.h/docs/EXCHANGE.md:
 *   - symbols[] holds one lob_t per symbol; names map to indices
 *     through a fixed-capacity open-addressing table (never removed
 *     from, so no tombstones).
 *   - records[] is a preallocated pool with one entry per open order,
 *     threaded into a free list through `next_free`, and found by
 *     exchange order id through a second open-addressing table
 *     (linear probing, backward-shift deletion, key 0 = empty since
 *     ids start at 1).
 *   - Accounts are parallel arrays: cash, reserved_cash, and
 *     [account * max_symbols + symbol] position/reserved_position.
 *   - The book's fill callback lands in on_fill(), which settles
 *     both sides. A market order has no id in lob.h's fill record
 *     (aggressor_order_id == 0), so ex_submit() parks the aggressor in
 *     cur_aggressor before calling the book and on_fill() reads it
 *     from there; safe because one request runs to completion before
 *     the next starts.
 *
 * ex_new() is the only place that calls calloc itself (ex_add_symbol()
 * reaches it through lob_new(), at setup). ex_submit() touches only
 * the preallocated structures.
 */
#include "exchange.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#define NO_RECORD UINT32_MAX

typedef struct {
    uint64_t id;
    int64_t limit_price;
    int64_t reserved;     /* cash for a buy, shares for a sell; 0 for a market order */
    uint32_t open_qty;
    uint32_t account;
    uint32_t symbol;
    uint8_t side;
    uint8_t type;
    uint32_t next_free;
} record_t;

typedef struct {
    char name[EX_NAME_MAX];
    lob_t *book;
    int64_t min_tick;
    int64_t max_tick;
    uint32_t open; /* records belonging to this symbol */
} symbol_t;

struct ex {
    ex_config_t cfg;
    int frozen;

    uint32_t num_symbols;
    symbol_t *symbols;
    uint32_t *name_slots;   /* symbol index + 1; 0 = empty */
    uint32_t name_capacity; /* power of two */

    uint32_t num_accounts;
    int64_t *cash;
    int64_t *reserved_cash;
    int64_t *position;
    int64_t *reserved_position;

    record_t *records;
    uint32_t free_head;
    uint64_t *id_keys;
    uint32_t *id_vals;
    uint64_t id_capacity; /* power of two */

    lob_level_snapshot_t *depth; /* market-buy walk buffer */

    uint64_t next_order_id;
    uint64_t next_exec_id;

    /* Set per request, read by on_fill(). */
    uint64_t cur_seq;
    int64_t cur_ts_ns;
    uint32_t cur_aggressor; /* record index */

    ex_event_fn sink;
    void *sink_user_data;
};

static uint64_t hash_u64(uint64_t x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

static uint64_t hash_name(const char *s)
{
    uint64_t h = 0xcbf29ce484222325ULL; /* FNV-1a */
    for (; *s; s++)
        h = (h ^ (uint8_t)*s) * 0x100000001b3ULL;
    return h;
}

static uint64_t next_pow2(uint64_t x)
{
    uint64_t p = 16;
    while (p < x)
        p <<= 1;
    return p;
}

/* ---- order id -> record index ------------------------------------ */

static uint32_t id_lookup(const ex_t *ex, uint64_t id)
{
    uint64_t mask = ex->id_capacity - 1;
    for (uint64_t i = hash_u64(id) & mask; ex->id_keys[i] != 0; i = (i + 1) & mask)
        if (ex->id_keys[i] == id)
            return ex->id_vals[i];
    return NO_RECORD;
}

static void id_insert(ex_t *ex, uint64_t id, uint32_t rec)
{
    uint64_t mask = ex->id_capacity - 1;
    uint64_t i = hash_u64(id) & mask;
    while (ex->id_keys[i] != 0)
        i = (i + 1) & mask;
    ex->id_keys[i] = id;
    ex->id_vals[i] = rec;
}

static void id_remove(ex_t *ex, uint64_t id)
{
    uint64_t mask = ex->id_capacity - 1;
    uint64_t i = hash_u64(id) & mask;
    while (ex->id_keys[i] != id)
        i = (i + 1) & mask; /* present by contract */

    /* Backward-shift: pull later entries of the probe run into the
     * hole unless they already sit between their home slot and it.
     */
    uint64_t j = i;
    for (;;) {
        j = (j + 1) & mask;
        if (ex->id_keys[j] == 0)
            break;
        uint64_t home = hash_u64(ex->id_keys[j]) & mask;
        int stays = (i <= j) ? (i < home && home <= j) : (i < home || home <= j);
        if (stays)
            continue;
        ex->id_keys[i] = ex->id_keys[j];
        ex->id_vals[i] = ex->id_vals[j];
        i = j;
    }
    ex->id_keys[i] = 0;
}

/* ---- events ------------------------------------------------------ */

static void emit(ex_t *ex, ex_event_kind_t kind, uint32_t account, uint64_t order_id,
                 ex_event_t *ev)
{
    ev->kind = kind;
    ev->seq = ex->cur_seq;
    ev->ts_ns = ex->cur_ts_ns;
    ev->account = account;
    ev->order_id = order_id;
    if (ex->sink)
        ex->sink(ev, ex->sink_user_data);
}

static void emit_simple(ex_t *ex, ex_event_kind_t kind, uint32_t account, uint64_t order_id,
                        ex_reject_t reason, uint32_t qty)
{
    ex_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.reason = reason;
    ev.qty = qty;
    emit(ex, kind, account, order_id, &ev);
}

static void reject(ex_t *ex, uint32_t account, uint64_t order_id, ex_reject_t reason)
{
    emit_simple(ex, EX_EV_REJECT, account, order_id, reason, 0);
}

/* ---- records and reservations ------------------------------------ */

/* Releases whatever is still reserved for the order, announces DONE
 * with the unfilled qty, and retires the record and its id.
 */
static void finish(ex_t *ex, uint32_t idx)
{
    record_t *r = &ex->records[idx];

    if (r->side == LOB_SIDE_BUY)
        ex->reserved_cash[r->account] -= r->reserved;
    else
        ex->reserved_position[(size_t)r->account * ex->cfg.max_symbols + r->symbol] -= r->reserved;

    emit_simple(ex, EX_EV_DONE, r->account, r->id, 0, r->open_qty);

    ex->symbols[r->symbol].open--;
    id_remove(ex, r->id);
    r->next_free = ex->free_head;
    ex->free_head = idx;
}

/* ---- fills ------------------------------------------------------- */

/* Settles one side of a fill. A limit/IOC order releases what it
 * reserved for the filled qty (a buy reserved at its limit price,
 * so a better fill price is a refund); a market order reserved
 * nothing.
 */
static void settle(ex_t *ex, record_t *r, int64_t price, uint32_t qty)
{
    size_t pos = (size_t)r->account * ex->cfg.max_symbols + r->symbol;
    int reserving = (r->type != EX_MARKET);

    if (r->side == LOB_SIDE_BUY) {
        int64_t held = reserving ? r->limit_price * (int64_t)qty : 0;
        ex->cash[r->account] -= price * (int64_t)qty;
        ex->reserved_cash[r->account] -= held;
        r->reserved -= held;
        ex->position[pos] += qty;
    } else {
        int64_t held = reserving ? (int64_t)qty : 0;
        ex->position[pos] -= qty;
        ex->reserved_position[pos] -= held;
        r->reserved -= held;
        ex->cash[r->account] += price * (int64_t)qty;
    }
    r->open_qty -= qty;
}

static void emit_fill(ex_t *ex, const record_t *r, const record_t *other, int64_t price,
                      uint32_t qty, uint64_t exec_id, int is_aggressor)
{
    ex_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.symbol = r->symbol;
    ev.side = (lob_side_t)r->side;
    ev.price_ticks = price;
    ev.qty = qty;
    ev.exec_id = exec_id;
    ev.counter_order_id = other->id;
    ev.is_aggressor = is_aggressor;
    emit(ex, EX_EV_FILL, r->account, r->id, &ev);
}

static void on_fill(const lob_fill_t *fill, void *user_data)
{
    ex_t *ex = user_data;
    uint32_t resting_idx = id_lookup(ex, fill->resting_order_id);
    uint32_t aggr_idx = fill->aggressor_order_id != 0
        ? id_lookup(ex, fill->aggressor_order_id)
        : ex->cur_aggressor;
    assert(resting_idx != NO_RECORD && aggr_idx != NO_RECORD);

    record_t *resting = &ex->records[resting_idx];
    record_t *aggr = &ex->records[aggr_idx];
    record_t *buy = (fill->aggressor_side == LOB_SIDE_BUY) ? aggr : resting;
    record_t *sell = (fill->aggressor_side == LOB_SIDE_BUY) ? resting : aggr;
    uint64_t exec_id = ++ex->next_exec_id;

    settle(ex, buy, fill->price_ticks, fill->qty);
    settle(ex, sell, fill->price_ticks, fill->qty);
    emit_fill(ex, buy, sell, fill->price_ticks, fill->qty, exec_id, buy == aggr);
    emit_fill(ex, sell, buy, fill->price_ticks, fill->qty, exec_id, sell == aggr);

    /* The aggressor is finished by ex_submit() once the book returns. */
    if (resting->open_qty == 0)
        finish(ex, resting_idx);
}

/* ---- requests ---------------------------------------------------- */

/* Exact cost of buying qty at market right now: walk the ask side
 * best first, taking what each level has. The sequencer is single
 * threaded, so the book can't move between this and the execution.
 * The buffer holds every level the window can have, so the walk
 * never runs out. A book too thin for qty costs only what is there.
 */
static int64_t market_buy_cost(const ex_t *ex, const symbol_t *sym, uint32_t qty)
{
    size_t n = lob_depth(sym->book, LOB_SIDE_SELL, ex->depth, ex->cfg.max_window_ticks);
    int64_t cost = 0;
    uint64_t left = qty;

    for (size_t i = 0; i < n && left > 0; i++) {
        uint64_t take = ex->depth[i].qty < left ? ex->depth[i].qty : left;
        cost += ex->depth[i].price_ticks * (int64_t)take;
        left -= take;
    }
    return cost;
}

static void submit_new(ex_t *ex, const ex_request_t *req)
{
    uint32_t acct = req->account;
    if (acct >= ex->num_accounts) {
        reject(ex, acct, 0, EX_REJ_UNKNOWN_ACCOUNT);
        return;
    }
    if (req->symbol >= ex->num_symbols) {
        reject(ex, acct, 0, EX_REJ_UNKNOWN_SYMBOL);
        return;
    }
    if (req->qty == 0) {
        reject(ex, acct, 0, EX_REJ_BAD_QTY);
        return;
    }

    symbol_t *sym = &ex->symbols[req->symbol];
    int is_market = (req->type == EX_MARKET);
    if (!is_market && (req->price_ticks < sym->min_tick || req->price_ticks > sym->max_tick)) {
        reject(ex, acct, 0, EX_REJ_BAD_PRICE);
        return;
    }

    /* A resting limit order needs a slot in the book's pool. Counting
     * every open record of the symbol is conservative (it includes
     * orders about to be filled) and keeps lob_add_limit from ever
     * failing after it has already filled something.
     */
    if (ex->free_head == NO_RECORD ||
        (req->type == EX_LIMIT && sym->open >= ex->cfg.book_orders)) {
        reject(ex, acct, 0, EX_REJ_CAPACITY);
        return;
    }

    size_t pos = (size_t)acct * ex->cfg.max_symbols + req->symbol;
    int64_t reserve;
    if (req->side == LOB_SIDE_BUY) {
        int64_t available = ex->cash[acct] - ex->reserved_cash[acct];
        int64_t need = is_market ? market_buy_cost(ex, sym, req->qty)
                                 : req->price_ticks * (int64_t)req->qty;
        if (available < need) {
            reject(ex, acct, 0, EX_REJ_INSUFFICIENT_CASH);
            return;
        }
        reserve = is_market ? 0 : need;
    } else {
        if (ex->position[pos] - ex->reserved_position[pos] < (int64_t)req->qty) {
            reject(ex, acct, 0, EX_REJ_INSUFFICIENT_POSITION);
            return;
        }
        reserve = is_market ? 0 : (int64_t)req->qty;
    }

    /* Accepted. From here nothing can fail. */
    uint32_t idx = ex->free_head;
    record_t *r = &ex->records[idx];
    ex->free_head = r->next_free;
    r->id = ++ex->next_order_id;
    r->limit_price = is_market ? 0 : req->price_ticks;
    r->reserved = reserve;
    r->open_qty = req->qty;
    r->account = acct;
    r->symbol = req->symbol;
    r->side = (uint8_t)req->side;
    r->type = (uint8_t)req->type;
    id_insert(ex, r->id, idx);
    sym->open++;
    if (req->side == LOB_SIDE_BUY)
        ex->reserved_cash[acct] += reserve;
    else
        ex->reserved_position[pos] += reserve;

    uint64_t id = r->id;
    emit_simple(ex, EX_EV_ACK, acct, id, 0, 0);

    ex->cur_aggressor = idx;
    uint32_t filled = 0;
    lob_status_t st;
    if (is_market)
        st = lob_execute_market(sym->book, req->side, req->qty, &filled);
    else
        st = lob_add_limit(sym->book, id, req->side, req->price_ticks, req->qty, &filled);

    if (req->type == EX_LIMIT) {
        assert(st == LOB_OK); /* the capacity check above rules out a full pool */
        if (r->open_qty == 0)
            finish(ex, idx);
        return;
    }

    /* IOC and market never rest. An IOC remainder is on the book
     * after lob_add_limit (unless the pool was full, in which case
     * it never got on); take it off before reporting DONE.
     */
    if (req->type == EX_IOC && st == LOB_OK && r->open_qty > 0) {
        lob_status_t cst = lob_cancel(sym->book, id);
        assert(cst == LOB_OK);
        (void)cst;
    }
    finish(ex, idx);
}

static void submit_cancel(ex_t *ex, const ex_request_t *req)
{
    uint32_t idx = id_lookup(ex, req->order_id);
    if (idx == NO_RECORD) {
        reject(ex, req->account, req->order_id, EX_REJ_UNKNOWN_ORDER);
        return;
    }
    record_t *r = &ex->records[idx];
    if (r->account != req->account) {
        reject(ex, req->account, req->order_id, EX_REJ_NOT_OWNER);
        return;
    }

    emit_simple(ex, EX_EV_ACK, r->account, r->id, 0, 0);
    lob_status_t st = lob_cancel(ex->symbols[r->symbol].book, r->id);
    assert(st == LOB_OK);
    (void)st;
    finish(ex, idx);
}

static void submit_reduce(ex_t *ex, const ex_request_t *req)
{
    uint32_t idx = id_lookup(ex, req->order_id);
    if (idx == NO_RECORD) {
        reject(ex, req->account, req->order_id, EX_REJ_UNKNOWN_ORDER);
        return;
    }
    record_t *r = &ex->records[idx];
    if (r->account != req->account) {
        reject(ex, req->account, req->order_id, EX_REJ_NOT_OWNER);
        return;
    }
    if (req->qty == 0 || req->qty >= r->open_qty) {
        reject(ex, req->account, req->order_id, EX_REJ_BAD_QTY);
        return;
    }

    emit_simple(ex, EX_EV_ACK, r->account, r->id, 0, 0);

    uint32_t cut = r->open_qty - req->qty;
    if (r->side == LOB_SIDE_BUY) {
        int64_t refund = r->limit_price * (int64_t)cut;
        ex->reserved_cash[r->account] -= refund;
        r->reserved -= refund;
    } else {
        ex->reserved_position[(size_t)r->account * ex->cfg.max_symbols + r->symbol] -= cut;
        r->reserved -= cut;
    }
    r->open_qty = req->qty;
    lob_status_t st = lob_reduce(ex->symbols[r->symbol].book, r->id, req->qty);
    assert(st == LOB_OK);
    (void)st;
}

void ex_submit(ex_t *ex, const ex_request_t *req)
{
    ex->frozen = 1;
    ex->cur_seq = req->seq;
    ex->cur_ts_ns = req->ts_ns;

    switch (req->kind) {
    case EX_REQ_NEW:
        submit_new(ex, req);
        break;
    case EX_REQ_CANCEL:
        submit_cancel(ex, req);
        break;
    case EX_REQ_REDUCE:
        submit_reduce(ex, req);
        break;
    }
}

/* ---- setup and read-only access ---------------------------------- */

ex_t *ex_new(const ex_config_t *cfg)
{
    if (!cfg || !cfg->max_symbols || !cfg->max_accounts || !cfg->max_orders ||
        !cfg->book_orders || !cfg->max_window_ticks)
        return NULL;

    ex_t *ex = calloc(1, sizeof(*ex));
    if (!ex)
        return NULL;
    ex->cfg = *cfg;

    size_t cells = (size_t)cfg->max_accounts * cfg->max_symbols;
    ex->name_capacity = (uint32_t)next_pow2((uint64_t)cfg->max_symbols * 2);
    ex->id_capacity = next_pow2((uint64_t)cfg->max_orders * 2);

    ex->symbols = calloc(cfg->max_symbols, sizeof(*ex->symbols));
    ex->name_slots = calloc(ex->name_capacity, sizeof(*ex->name_slots));
    ex->cash = calloc(cfg->max_accounts, sizeof(*ex->cash));
    ex->reserved_cash = calloc(cfg->max_accounts, sizeof(*ex->reserved_cash));
    ex->position = calloc(cells, sizeof(*ex->position));
    ex->reserved_position = calloc(cells, sizeof(*ex->reserved_position));
    ex->records = calloc(cfg->max_orders, sizeof(*ex->records));
    ex->id_keys = calloc(ex->id_capacity, sizeof(*ex->id_keys));
    ex->id_vals = calloc(ex->id_capacity, sizeof(*ex->id_vals));
    ex->depth = calloc(cfg->max_window_ticks, sizeof(*ex->depth));

    if (!ex->symbols || !ex->name_slots || !ex->cash || !ex->reserved_cash || !ex->position ||
        !ex->reserved_position || !ex->records || !ex->id_keys || !ex->id_vals || !ex->depth) {
        ex_free(ex);
        return NULL;
    }

    for (uint32_t i = 0; i < cfg->max_orders; i++)
        ex->records[i].next_free = (i + 1 < cfg->max_orders) ? (i + 1) : NO_RECORD;
    ex->free_head = 0;
    return ex;
}

void ex_free(ex_t *ex)
{
    if (!ex)
        return;
    for (uint32_t i = 0; i < ex->num_symbols; i++)
        lob_free(ex->symbols[i].book);
    free(ex->symbols);
    free(ex->name_slots);
    free(ex->cash);
    free(ex->reserved_cash);
    free(ex->position);
    free(ex->reserved_position);
    free(ex->records);
    free(ex->id_keys);
    free(ex->id_vals);
    free(ex->depth);
    free(ex);
}

int ex_symbol_index(const ex_t *ex, const char *name)
{
    uint32_t mask = ex->name_capacity - 1;
    for (uint32_t i = (uint32_t)hash_name(name) & mask; ex->name_slots[i] != 0;
         i = (i + 1) & mask) {
        uint32_t s = ex->name_slots[i] - 1;
        if (strcmp(ex->symbols[s].name, name) == 0)
            return (int)s;
    }
    return -1;
}

int ex_add_symbol(ex_t *ex, const char *name, int64_t min_tick, int64_t max_tick)
{
    if (ex->frozen || ex->num_symbols >= ex->cfg.max_symbols)
        return -1;
    if (!name || !*name || strlen(name) >= EX_NAME_MAX || ex_symbol_index(ex, name) >= 0)
        return -1;
    if (min_tick < 0 || max_tick < min_tick ||
        (uint64_t)(max_tick - min_tick) >= ex->cfg.max_window_ticks)
        return -1;

    lob_t *book = lob_new(ex->cfg.book_orders, min_tick, max_tick);
    if (!book)
        return -1;

    uint32_t s = ex->num_symbols++;
    symbol_t *sym = &ex->symbols[s];
    strcpy(sym->name, name);
    sym->book = book;
    sym->min_tick = min_tick;
    sym->max_tick = max_tick;
    lob_set_fill_callback(book, on_fill, ex);

    uint32_t mask = ex->name_capacity - 1;
    uint32_t i = (uint32_t)hash_name(name) & mask;
    while (ex->name_slots[i] != 0)
        i = (i + 1) & mask;
    ex->name_slots[i] = s + 1;
    return (int)s;
}

int ex_add_account(ex_t *ex, int64_t cash, const int64_t *positions)
{
    if (ex->frozen || ex->num_accounts >= ex->cfg.max_accounts || cash < 0)
        return -1;
    for (uint32_t s = 0; positions && s < ex->num_symbols; s++)
        if (positions[s] < 0)
            return -1;

    uint32_t a = ex->num_accounts++;
    ex->cash[a] = cash;
    for (uint32_t s = 0; positions && s < ex->num_symbols; s++)
        ex->position[(size_t)a * ex->cfg.max_symbols + s] = positions[s];
    return (int)a;
}

void ex_set_event_sink(ex_t *ex, ex_event_fn cb, void *user_data)
{
    ex->sink = cb;
    ex->sink_user_data = user_data;
}

int ex_get_balance(const ex_t *ex, uint32_t account, int64_t *cash_out,
                   int64_t *reserved_cash_out)
{
    if (account >= ex->num_accounts)
        return -1;
    if (cash_out)
        *cash_out = ex->cash[account];
    if (reserved_cash_out)
        *reserved_cash_out = ex->reserved_cash[account];
    return 0;
}

int ex_get_position(const ex_t *ex, uint32_t account, uint32_t symbol,
                    int64_t *position_out, int64_t *reserved_position_out)
{
    if (account >= ex->num_accounts || symbol >= ex->num_symbols)
        return -1;
    size_t pos = (size_t)account * ex->cfg.max_symbols + symbol;
    if (position_out)
        *position_out = ex->position[pos];
    if (reserved_position_out)
        *reserved_position_out = ex->reserved_position[pos];
    return 0;
}

int ex_get_order(const ex_t *ex, uint64_t order_id, ex_order_info_t *out)
{
    uint32_t idx = id_lookup(ex, order_id);
    if (idx == NO_RECORD)
        return -1;
    const record_t *r = &ex->records[idx];
    out->account = r->account;
    out->symbol = r->symbol;
    out->side = (lob_side_t)r->side;
    out->type = (ex_order_type_t)r->type;
    out->price_ticks = r->limit_price;
    out->open_qty = r->open_qty;
    out->reserved = r->reserved;
    return 0;
}

const lob_t *ex_book(const ex_t *ex, uint32_t symbol)
{
    return symbol < ex->num_symbols ? ex->symbols[symbol].book : NULL;
}
