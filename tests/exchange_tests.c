/* Assert-based tests for the M5a exchange state machine
 * (src/exchange.c). Run via `make test`.
 *
 * The first group pins one behavior each: every risk rule and reject
 * reason, IOC, the market-buy cost walk, fill attribution (including
 * market-order aggressors, which lob.h reports with id 0), and
 * reservation release on cancel/reduce. The last two run a seeded
 * random request stream: test_conservation_invariants checks the
 * accounting identities after every request, and
 * test_differential_against_naive_exchange replays the same kind of
 * stream through a deliberately dumb reference (linear scans, no
 * reservations stored -- they are recomputed from the open orders
 * each time) and compares every event and every balance.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/exchange.h"
#include "ex_random.h"

/* ---- capture and request helpers for the unit tests -------------- */

#define MAX_EVS 64
static ex_event_t evs[MAX_EVS];
static size_t nevs;
static uint64_t next_seq;

static void capture(const ex_event_t *ev, void *user_data)
{
    (void)user_data;
    assert(nevs < MAX_EVS);
    evs[nevs++] = *ev;
}

/* AAA and BBB, both with window [0, 200]. */
static ex_t *mk_with(uint32_t max_orders, uint32_t book_orders)
{
    ex_config_t cfg = {.max_symbols = 4, .max_accounts = 8, .max_orders = max_orders,
                       .book_orders = book_orders, .max_window_ticks = 256};
    ex_t *ex = ex_new(&cfg);
    assert(ex != NULL);
    assert(ex_add_symbol(ex, "AAA", 0, 200) == 0);
    assert(ex_add_symbol(ex, "BBB", 0, 200) == 1);
    ex_set_event_sink(ex, capture, NULL);
    return ex;
}

static ex_t *mk(void)
{
    return mk_with(16, 8);
}

static int add_acct(ex_t *ex, int64_t cash, int64_t aaa)
{
    int64_t pos[4] = {aaa, 0, 0, 0}; /* max_symbols entries */
    int a = ex_add_account(ex, cash, pos);
    assert(a >= 0);
    return a;
}

static void send(ex_t *ex, const ex_request_t *req)
{
    nevs = 0;
    ex_submit(ex, req);
}

/* Returns the assigned order id (from the ACK), or 0 if rejected. */
static uint64_t new_order(ex_t *ex, uint32_t acct, uint32_t sym, lob_side_t side,
                          ex_order_type_t type, int64_t price, uint32_t qty)
{
    ex_request_t req = {.seq = ++next_seq, .ts_ns = (int64_t)next_seq * 10,
                        .kind = EX_REQ_NEW, .account = acct, .symbol = sym, .side = side,
                        .type = type, .price_ticks = price, .qty = qty};
    send(ex, &req);
    return (nevs > 0 && evs[0].kind == EX_EV_ACK) ? evs[0].order_id : 0;
}

static void cancel(ex_t *ex, uint32_t acct, uint64_t id)
{
    ex_request_t req = {.seq = ++next_seq, .kind = EX_REQ_CANCEL, .account = acct,
                        .order_id = id};
    send(ex, &req);
}

static void reduce(ex_t *ex, uint32_t acct, uint64_t id, uint32_t qty)
{
    ex_request_t req = {.seq = ++next_seq, .kind = EX_REQ_REDUCE, .account = acct,
                        .order_id = id, .qty = qty};
    send(ex, &req);
}

static int64_t cash_of(const ex_t *ex, uint32_t a)
{
    int64_t c;
    assert(ex_get_balance(ex, a, &c, NULL) == 0);
    return c;
}

static int64_t reserved_cash_of(const ex_t *ex, uint32_t a)
{
    int64_t r;
    assert(ex_get_balance(ex, a, NULL, &r) == 0);
    return r;
}

static int64_t pos_of(const ex_t *ex, uint32_t a, uint32_t s)
{
    int64_t p;
    assert(ex_get_position(ex, a, s, &p, NULL) == 0);
    return p;
}

static int64_t reserved_pos_of(const ex_t *ex, uint32_t a, uint32_t s)
{
    int64_t r;
    assert(ex_get_position(ex, a, s, NULL, &r) == 0);
    return r;
}

static void expect_reject(ex_reject_t reason)
{
    assert(nevs == 1);
    assert(evs[0].kind == EX_EV_REJECT && evs[0].reason == reason);
}

/* ---- unit tests -------------------------------------------------- */

static void test_setup_and_symbol_table(void)
{
    ex_config_t bad = {.max_symbols = 4, .max_accounts = 4, .max_orders = 16,
                       .book_orders = 0, .max_window_ticks = 256};
    assert(ex_new(NULL) == NULL);
    assert(ex_new(&bad) == NULL);

    ex_t *ex = mk();
    assert(ex_symbol_index(ex, "AAA") == 0 && ex_symbol_index(ex, "BBB") == 1);
    assert(ex_symbol_index(ex, "ZZZ") == -1);
    assert(ex_add_symbol(ex, "AAA", 0, 10) == -1);               /* duplicate name */
    assert(ex_add_symbol(ex, "ABCDEFGHIJKLMNOP", 0, 10) == -1);  /* name too long */
    assert(ex_add_symbol(ex, "NEG", -1, 10) == -1);              /* negative ticks */
    assert(ex_add_symbol(ex, "WIDE", 0, 256) == -1);             /* 257 ticks > buffer */
    assert(ex_add_symbol(ex, "OK", 0, 255) == 2);                /* 256 ticks fits */
    assert(ex_book(ex, 0) != NULL && ex_book(ex, 3) == NULL);

    int a = add_acct(ex, 1000, 5);
    assert(add_acct(ex, 0, 0) == a + 1);
    assert(ex_add_account(ex, -1, NULL) == -1);

    /* The first request freezes the tables. */
    new_order(ex, 0, 0, LOB_SIDE_BUY, EX_LIMIT, 10, 1);
    assert(ex_add_symbol(ex, "LATE", 0, 10) == -1);
    assert(ex_add_account(ex, 1, NULL) == -1);
    ex_free(ex);
}

static void test_ack_echoes_seq_and_ids_are_assigned(void)
{
    ex_t *ex = mk();
    add_acct(ex, 1000, 0);
    ex_request_t req = {.seq = 77, .ts_ns = 1234, .kind = EX_REQ_NEW, .account = 0,
                        .symbol = 0, .side = LOB_SIDE_BUY, .type = EX_LIMIT,
                        .price_ticks = 10, .qty = 1, .order_id = 999 /* ignored */};
    send(ex, &req);
    assert(nevs == 1 && evs[0].kind == EX_EV_ACK);
    assert(evs[0].seq == 77 && evs[0].ts_ns == 1234 && evs[0].account == 0);
    uint64_t first = evs[0].order_id;
    assert(first != 0 && first != 999);

    send(ex, &req);
    assert(evs[0].order_id == first + 1);

    /* A rejected NEW consumes no id. */
    req.qty = 0;
    send(ex, &req);
    expect_reject(EX_REJ_BAD_QTY);
    assert(evs[0].order_id == 0 && evs[0].seq == 77);
    req.qty = 1;
    send(ex, &req);
    assert(evs[0].order_id == first + 2);
    ex_free(ex);
}

static void test_reject_reasons_leave_state_untouched(void)
{
    ex_t *ex = mk();
    int buyer = add_acct(ex, 10000, 0);
    int seller = add_acct(ex, 0, 50);

    assert(new_order(ex, 99, 0, LOB_SIDE_BUY, EX_LIMIT, 10, 1) == 0);
    expect_reject(EX_REJ_UNKNOWN_ACCOUNT);
    assert(new_order(ex, (uint32_t)buyer, 9, LOB_SIDE_BUY, EX_LIMIT, 10, 1) == 0);
    expect_reject(EX_REJ_UNKNOWN_SYMBOL);
    assert(new_order(ex, (uint32_t)buyer, 0, LOB_SIDE_BUY, EX_LIMIT, 10, 0) == 0);
    expect_reject(EX_REJ_BAD_QTY);
    assert(new_order(ex, (uint32_t)buyer, 0, LOB_SIDE_BUY, EX_LIMIT, 201, 1) == 0);
    expect_reject(EX_REJ_BAD_PRICE);
    assert(new_order(ex, (uint32_t)buyer, 0, LOB_SIDE_BUY, EX_IOC, -1, 1) == 0);
    expect_reject(EX_REJ_BAD_PRICE);

    assert(new_order(ex, (uint32_t)buyer, 0, LOB_SIDE_BUY, EX_LIMIT, 101, 100) == 0);
    expect_reject(EX_REJ_INSUFFICIENT_CASH); /* 10100 > 10000 */
    assert(new_order(ex, (uint32_t)seller, 0, LOB_SIDE_SELL, EX_LIMIT, 10, 51) == 0);
    expect_reject(EX_REJ_INSUFFICIENT_POSITION);
    assert(new_order(ex, (uint32_t)buyer, 0, LOB_SIDE_SELL, EX_MARKET, 0, 1) == 0);
    expect_reject(EX_REJ_INSUFFICIENT_POSITION); /* buyer holds no shares */

    cancel(ex, (uint32_t)buyer, 12345);
    expect_reject(EX_REJ_UNKNOWN_ORDER);
    reduce(ex, (uint32_t)buyer, 12345, 1);
    expect_reject(EX_REJ_UNKNOWN_ORDER);

    assert(cash_of(ex, (uint32_t)buyer) == 10000 && reserved_cash_of(ex, (uint32_t)buyer) == 0);
    assert(pos_of(ex, (uint32_t)seller, 0) == 50 && reserved_pos_of(ex, (uint32_t)seller, 0) == 0);
    assert(!lob_best_bid(ex_book(ex, 0), &(int64_t){0}, &(uint64_t){0}));

    /* Exactly enough is accepted (the boundary of the cash check). */
    uint64_t id = new_order(ex, (uint32_t)buyer, 0, LOB_SIDE_BUY, EX_LIMIT, 100, 100);
    assert(id != 0 && reserved_cash_of(ex, (uint32_t)buyer) == 10000);

    /* Ownership and reduce bounds. */
    cancel(ex, (uint32_t)seller, id);
    expect_reject(EX_REJ_NOT_OWNER);
    reduce(ex, (uint32_t)seller, id, 5);
    expect_reject(EX_REJ_NOT_OWNER);
    reduce(ex, (uint32_t)buyer, id, 0);
    expect_reject(EX_REJ_BAD_QTY);
    reduce(ex, (uint32_t)buyer, id, 100);
    expect_reject(EX_REJ_BAD_QTY);
    reduce(ex, (uint32_t)buyer, id, 101);
    expect_reject(EX_REJ_BAD_QTY);
    assert(reserved_cash_of(ex, (uint32_t)buyer) == 10000);

    /* A retired id is unknown, not "not owner". */
    cancel(ex, (uint32_t)buyer, id);
    assert(nevs == 2 && evs[0].kind == EX_EV_ACK && evs[1].kind == EX_EV_DONE);
    cancel(ex, (uint32_t)buyer, id);
    expect_reject(EX_REJ_UNKNOWN_ORDER);
    ex_free(ex);
}

static void test_capacity_rejects(void)
{
    /* Order records: 4 open orders fill the table, across both symbols. */
    ex_t *ex = mk_with(4, 8);
    add_acct(ex, 100000, 0);
    for (int i = 0; i < 4; i++)
        assert(new_order(ex, 0, (uint32_t)(i % 2), LOB_SIDE_BUY, EX_LIMIT, 10 + i, 1) != 0);
    assert(new_order(ex, 0, 0, LOB_SIDE_BUY, EX_LIMIT, 50, 1) == 0);
    expect_reject(EX_REJ_CAPACITY);
    assert(reserved_cash_of(ex, 0) == 10 + 11 + 12 + 13);
    ex_free(ex);

    /* One symbol's book pool: 3 resting orders fill AAA, BBB is unaffected. */
    ex = mk_with(16, 3);
    add_acct(ex, 100000, 0);
    for (int i = 0; i < 3; i++)
        assert(new_order(ex, 0, 0, LOB_SIDE_BUY, EX_LIMIT, 10 + i, 1) != 0);
    assert(new_order(ex, 0, 0, LOB_SIDE_BUY, EX_LIMIT, 50, 1) == 0);
    expect_reject(EX_REJ_CAPACITY);
    assert(new_order(ex, 0, 1, LOB_SIDE_BUY, EX_LIMIT, 50, 1) != 0);
    assert(new_order(ex, 0, 0, LOB_SIDE_BUY, EX_IOC, 5, 1) != 0); /* never rests, so allowed */
    ex_free(ex);
}

static void test_limit_buy_reserves_and_refunds_on_better_fill(void)
{
    ex_t *ex = mk();
    int buyer = add_acct(ex, 10000, 0);
    int seller = add_acct(ex, 0, 50);
    uint32_t b = (uint32_t)buyer, s = (uint32_t)seller;

    uint64_t ask = new_order(ex, s, 0, LOB_SIDE_SELL, EX_LIMIT, 100, 5);
    assert(ask != 0 && nevs == 1);

    /* Resting bid: reserves price * qty. */
    uint64_t bid = new_order(ex, b, 0, LOB_SIDE_BUY, EX_LIMIT, 90, 4);
    assert(bid != 0 && nevs == 1);
    assert(reserved_cash_of(ex, b) == 360 && cash_of(ex, b) == 10000);

    /* Crossing buy at 105 reserves 315 but pays 100 per share. */
    uint64_t agg = new_order(ex, b, 0, LOB_SIDE_BUY, EX_LIMIT, 105, 3);
    assert(nevs == 4);
    assert(evs[0].kind == EX_EV_ACK && evs[0].order_id == agg);
    assert(evs[1].kind == EX_EV_FILL && evs[1].account == b && evs[1].side == LOB_SIDE_BUY);
    assert(evs[1].price_ticks == 100 && evs[1].qty == 3 && evs[1].is_aggressor);
    assert(evs[1].order_id == agg && evs[1].counter_order_id == ask);
    assert(evs[2].kind == EX_EV_FILL && evs[2].account == s && evs[2].side == LOB_SIDE_SELL);
    assert(evs[2].price_ticks == 100 && evs[2].qty == 3 && !evs[2].is_aggressor);
    assert(evs[2].order_id == ask && evs[2].counter_order_id == agg);
    assert(evs[1].exec_id == evs[2].exec_id && evs[1].symbol == 0);
    assert(evs[3].kind == EX_EV_DONE && evs[3].order_id == agg && evs[3].qty == 0);
    assert(cash_of(ex, b) == 9700 && reserved_cash_of(ex, b) == 360); /* refund of 3*5 */
    assert(pos_of(ex, b, 0) == 3);
    assert(cash_of(ex, s) == 300 && pos_of(ex, s, 0) == 47 && reserved_pos_of(ex, s, 0) == 2);

    /* Partial: 2 fill at 100, the other 2 rest at 105 holding 210. */
    uint64_t part = new_order(ex, b, 0, LOB_SIDE_BUY, EX_LIMIT, 105, 4);
    assert(nevs == 4); /* ACK, FILL, FILL, DONE for the ask */
    assert(evs[3].kind == EX_EV_DONE && evs[3].order_id == ask);
    ex_order_info_t info;
    assert(ex_get_order(ex, part, &info) == 0);
    assert(info.open_qty == 2 && info.reserved == 210 && info.price_ticks == 105);
    assert(cash_of(ex, b) == 9500 && reserved_cash_of(ex, b) == 360 + 210);
    assert(reserved_pos_of(ex, s, 0) == 0);
    ex_free(ex);
}

static void test_sell_reserves_shares_and_no_shorting(void)
{
    ex_t *ex = mk();
    int s = add_acct(ex, 0, 50);
    uint32_t a = (uint32_t)s;

    uint64_t first = new_order(ex, a, 0, LOB_SIDE_SELL, EX_LIMIT, 100, 30);
    assert(first != 0 && reserved_pos_of(ex, a, 0) == 30);
    assert(new_order(ex, a, 0, LOB_SIDE_SELL, EX_LIMIT, 100, 21) == 0);
    expect_reject(EX_REJ_INSUFFICIENT_POSITION); /* 21 > 50 - 30 */
    assert(new_order(ex, a, 0, LOB_SIDE_SELL, EX_IOC, 100, 21) == 0);
    expect_reject(EX_REJ_INSUFFICIENT_POSITION);
    assert(new_order(ex, a, 0, LOB_SIDE_SELL, EX_LIMIT, 101, 20) != 0);
    assert(reserved_pos_of(ex, a, 0) == 50);
    assert(new_order(ex, a, 0, LOB_SIDE_SELL, EX_MARKET, 0, 1) == 0);
    expect_reject(EX_REJ_INSUFFICIENT_POSITION); /* market sell: same check */

    /* Position is per symbol. */
    assert(new_order(ex, a, 1, LOB_SIDE_SELL, EX_LIMIT, 100, 1) == 0);
    expect_reject(EX_REJ_INSUFFICIENT_POSITION);

    cancel(ex, a, first);
    assert(reserved_pos_of(ex, a, 0) == 20);
    assert(new_order(ex, a, 0, LOB_SIDE_SELL, EX_LIMIT, 102, 30) != 0);
    ex_free(ex);
}

static void test_ioc_cancels_remainder(void)
{
    ex_t *ex = mk();
    uint32_t b = (uint32_t)add_acct(ex, 10000, 0);
    uint32_t s = (uint32_t)add_acct(ex, 0, 50);
    new_order(ex, s, 0, LOB_SIDE_SELL, EX_LIMIT, 100, 5);

    /* Wants 8, only 5 available: fills 5, cancels 3, nothing rests. */
    uint64_t ioc = new_order(ex, b, 0, LOB_SIDE_BUY, EX_IOC, 105, 8);
    assert(nevs == 5); /* ACK, FILL, FILL, DONE(ask), DONE(ioc) */
    assert(evs[4].kind == EX_EV_DONE && evs[4].order_id == ioc && evs[4].qty == 3);
    assert(!lob_best_bid(ex_book(ex, 0), &(int64_t){0}, &(uint64_t){0}));
    assert(ex_get_order(ex, ioc, &(ex_order_info_t){0}) == -1);
    assert(cash_of(ex, b) == 9500 && reserved_cash_of(ex, b) == 0);
    assert(pos_of(ex, b, 0) == 5);

    /* No cross at all: ACK then DONE with the full qty, nothing reserved after. */
    ioc = new_order(ex, b, 0, LOB_SIDE_BUY, EX_IOC, 50, 2);
    assert(nevs == 2 && evs[1].kind == EX_EV_DONE && evs[1].qty == 2);
    assert(reserved_cash_of(ex, b) == 0 && cash_of(ex, b) == 9500);
    assert(!lob_best_bid(ex_book(ex, 0), &(int64_t){0}, &(uint64_t){0}));

    /* Sell side: IOC sell into a bid, remainder cancelled, shares released. */
    new_order(ex, b, 0, LOB_SIDE_BUY, EX_LIMIT, 90, 2);
    ioc = new_order(ex, s, 0, LOB_SIDE_SELL, EX_IOC, 80, 6);
    assert(nevs == 5 && evs[4].kind == EX_EV_DONE && evs[4].qty == 4);
    assert(reserved_pos_of(ex, s, 0) == 0 && pos_of(ex, s, 0) == 43);
    assert(cash_of(ex, s) == 680); /* 500 + 2 * 90 */
    ex_free(ex);
}

static void test_market_buy_prices_exactly_by_walking_the_book(void)
{
    ex_t *ex = mk();
    uint32_t s = (uint32_t)add_acct(ex, 0, 50);
    uint32_t poor = (uint32_t)add_acct(ex, 401, 0);
    uint32_t exact = (uint32_t)add_acct(ex, 402, 0);
    uint32_t held = (uint32_t)add_acct(ex, 1000, 0);
    uint32_t rich = (uint32_t)add_acct(ex, 10000, 0);
    new_order(ex, s, 0, LOB_SIDE_SELL, EX_LIMIT, 100, 2);
    new_order(ex, s, 0, LOB_SIDE_SELL, EX_LIMIT, 101, 3);

    /* 4 shares cost 2*100 + 2*101 = 402. */
    assert(new_order(ex, poor, 0, LOB_SIDE_BUY, EX_MARKET, 0, 4) == 0);
    expect_reject(EX_REJ_INSUFFICIENT_CASH);
    assert(cash_of(ex, poor) == 401);
    int64_t px;
    uint64_t q;
    assert(lob_best_ask(ex_book(ex, 0), &px, &q) && px == 100 && q == 2); /* book untouched */

    assert(new_order(ex, exact, 0, LOB_SIDE_BUY, EX_MARKET, 0, 4) != 0);
    assert(cash_of(ex, exact) == 0 && pos_of(ex, exact, 0) == 4);
    assert(reserved_cash_of(ex, exact) == 0);
    assert(lob_best_ask(ex_book(ex, 0), &px, &q) && px == 101 && q == 1);

    /* Reserved cash is not available: 1000 cash, 900 held by a resting bid. */
    assert(new_order(ex, held, 0, LOB_SIDE_BUY, EX_LIMIT, 90, 10) != 0);
    assert(new_order(ex, held, 0, LOB_SIDE_BUY, EX_MARKET, 0, 1) == 0); /* 101 > 100 */
    expect_reject(EX_REJ_INSUFFICIENT_CASH);

    /* A book thinner than qty costs only what is there; the rest is unfilled. */
    uint64_t id = new_order(ex, rich, 0, LOB_SIDE_BUY, EX_MARKET, 0, 10);
    assert(id != 0 && cash_of(ex, rich) == 10000 - 101);
    assert(evs[nevs - 1].kind == EX_EV_DONE && evs[nevs - 1].qty == 9);

    /* Empty book: accepted, nothing fills, nothing charged. */
    id = new_order(ex, rich, 0, LOB_SIDE_BUY, EX_MARKET, 0, 3);
    assert(id != 0 && nevs == 2 && evs[1].kind == EX_EV_DONE && evs[1].qty == 3);
    assert(cash_of(ex, rich) == 10000 - 101);
    ex_free(ex);
}

static void test_market_orders_are_attributed_to_the_right_accounts(void)
{
    ex_t *ex = mk();
    uint32_t b = (uint32_t)add_acct(ex, 10000, 0);
    uint32_t s1 = (uint32_t)add_acct(ex, 0, 10);
    uint32_t s2 = (uint32_t)add_acct(ex, 0, 10);
    uint32_t b1 = (uint32_t)add_acct(ex, 5000, 0);
    uint32_t b2 = (uint32_t)add_acct(ex, 5000, 0);
    uint64_t a1 = new_order(ex, s1, 0, LOB_SIDE_SELL, EX_LIMIT, 100, 2);
    uint64_t a2 = new_order(ex, s2, 0, LOB_SIDE_SELL, EX_LIMIT, 101, 2);

    /* lob.h reports aggressor id 0 for this; the exchange must still
     * credit and debit the market order's own account.
     */
    uint64_t m = new_order(ex, b, 0, LOB_SIDE_BUY, EX_MARKET, 0, 3);
    assert(nevs == 7);
    assert(evs[0].kind == EX_EV_ACK && evs[0].order_id == m);
    assert(evs[1].kind == EX_EV_FILL && evs[1].account == b && evs[1].is_aggressor);
    assert(evs[1].order_id == m && evs[1].counter_order_id == a1 && evs[1].qty == 2);
    assert(evs[2].kind == EX_EV_FILL && evs[2].account == s1 && evs[2].order_id == a1);
    assert(evs[3].kind == EX_EV_DONE && evs[3].order_id == a1 && evs[3].qty == 0);
    assert(evs[4].kind == EX_EV_FILL && evs[4].account == b && evs[4].price_ticks == 101);
    assert(evs[5].kind == EX_EV_FILL && evs[5].account == s2 && evs[5].order_id == a2);
    assert(evs[6].kind == EX_EV_DONE && evs[6].order_id == m);
    assert(cash_of(ex, b) == 10000 - 200 - 101 && pos_of(ex, b, 0) == 3);
    assert(cash_of(ex, s1) == 200 && pos_of(ex, s1, 0) == 8 && reserved_pos_of(ex, s1, 0) == 0);
    assert(cash_of(ex, s2) == 101 && pos_of(ex, s2, 0) == 9 && reserved_pos_of(ex, s2, 0) == 1);

    /* Market sell: the seller is the aggressor; resting bids are released. */
    new_order(ex, b1, 0, LOB_SIDE_BUY, EX_LIMIT, 90, 1);
    new_order(ex, b2, 0, LOB_SIDE_BUY, EX_LIMIT, 89, 5);
    assert(reserved_cash_of(ex, b1) == 90 && reserved_cash_of(ex, b2) == 445);
    m = new_order(ex, b, 0, LOB_SIDE_SELL, EX_MARKET, 0, 3);
    assert(m != 0);
    assert(cash_of(ex, b) == 10000 - 301 + 90 + 2 * 89 && pos_of(ex, b, 0) == 0);
    assert(reserved_pos_of(ex, b, 0) == 0);
    assert(cash_of(ex, b1) == 4910 && pos_of(ex, b1, 0) == 1 && reserved_cash_of(ex, b1) == 0);
    assert(cash_of(ex, b2) == 5000 - 178 && pos_of(ex, b2, 0) == 2);
    assert(reserved_cash_of(ex, b2) == 3 * 89); /* 3 shares still open */
    ex_free(ex);
}

static void test_cancel_and_reduce_release_reservations(void)
{
    ex_t *ex = mk();
    uint32_t b = (uint32_t)add_acct(ex, 10000, 40);
    ex_order_info_t info;

    uint64_t bid = new_order(ex, b, 0, LOB_SIDE_BUY, EX_LIMIT, 90, 4);
    assert(reserved_cash_of(ex, b) == 360);
    reduce(ex, b, bid, 1);
    assert(nevs == 1 && evs[0].kind == EX_EV_ACK && evs[0].order_id == bid);
    assert(reserved_cash_of(ex, b) == 90);
    assert(ex_get_order(ex, bid, &info) == 0 && info.open_qty == 1 && info.reserved == 90);
    uint32_t book_qty;
    assert(lob_get_order(ex_book(ex, 0), bid, NULL, NULL, &book_qty) == LOB_OK && book_qty == 1);
    cancel(ex, b, bid);
    assert(nevs == 2 && evs[0].kind == EX_EV_ACK);
    assert(evs[1].kind == EX_EV_DONE && evs[1].order_id == bid && evs[1].qty == 1);
    assert(reserved_cash_of(ex, b) == 0 && cash_of(ex, b) == 10000);
    assert(ex_get_order(ex, bid, &info) == -1);
    assert(lob_get_order(ex_book(ex, 0), bid, NULL, NULL, NULL) == LOB_ERR_NOT_FOUND);

    uint64_t ask = new_order(ex, b, 0, LOB_SIDE_SELL, EX_LIMIT, 110, 30);
    assert(reserved_pos_of(ex, b, 0) == 30);
    reduce(ex, b, ask, 10);
    assert(reserved_pos_of(ex, b, 0) == 10);
    cancel(ex, b, ask);
    assert(evs[1].qty == 10 && reserved_pos_of(ex, b, 0) == 0 && pos_of(ex, b, 0) == 40);
    ex_free(ex);
}

static void test_self_trade_is_allowed_and_nets_to_zero(void)
{
    ex_t *ex = mk();
    uint32_t a = (uint32_t)add_acct(ex, 1000, 10);
    uint64_t ask = new_order(ex, a, 0, LOB_SIDE_SELL, EX_LIMIT, 100, 5);
    uint64_t bid = new_order(ex, a, 0, LOB_SIDE_BUY, EX_LIMIT, 100, 5);
    assert(nevs == 5 && evs[1].kind == EX_EV_FILL && evs[2].kind == EX_EV_FILL);
    assert(evs[1].account == a && evs[2].account == a);
    assert(evs[1].counter_order_id == ask && evs[2].counter_order_id == bid);
    assert(cash_of(ex, a) == 1000 && pos_of(ex, a, 0) == 10);
    assert(reserved_cash_of(ex, a) == 0 && reserved_pos_of(ex, a, 0) == 0);
    ex_free(ex);
}

/* ---- conservation invariants over a random run ------------------- */

static void check_invariants(harness_t *h, int64_t total_cash, const int64_t *total_pos)
{
    ex_t *ex = h->ex;
    int64_t cash_sum = 0, pos_sum[H_SYMBOLS] = {0};
    int64_t want_rc[H_ACCOUNTS] = {0}, want_rp[H_ACCOUNTS][H_SYMBOLS] = {{0}};

    /* Recompute reservations from scratch from the open orders. */
    for (size_t i = 0; i < h->nlive; i++) {
        ex_order_info_t o;
        assert(ex_get_order(ex, h->live_id[i], &o) == 0);
        assert(o.account == h->live_acct[i] && o.open_qty > 0);
        assert(o.type != EX_MARKET); /* market orders never survive a request */

        uint32_t book_qty;
        lob_side_t book_side;
        int64_t book_px;
        assert(lob_get_order(ex_book(ex, o.symbol), h->live_id[i], &book_side, &book_px,
                              &book_qty) == LOB_OK);
        assert(book_qty == o.open_qty && book_side == o.side && book_px == o.price_ticks);

        if (o.side == LOB_SIDE_BUY) {
            want_rc[o.account] += o.price_ticks * (int64_t)o.open_qty;
            assert(o.reserved == o.price_ticks * (int64_t)o.open_qty);
        } else {
            want_rp[o.account][o.symbol] += o.open_qty;
            assert(o.reserved == (int64_t)o.open_qty);
        }
    }

    for (uint32_t a = 0; a < H_ACCOUNTS; a++) {
        int64_t cash, rc;
        assert(ex_get_balance(ex, a, &cash, &rc) == 0);
        cash_sum += cash;
        assert(cash - rc >= 0);
        assert(rc == want_rc[a]);
        for (uint32_t s = 0; s < H_SYMBOLS; s++) {
            int64_t pos, rp;
            assert(ex_get_position(ex, a, s, &pos, &rp) == 0);
            pos_sum[s] += pos;
            assert(pos - rp >= 0);
            assert(rp == want_rp[a][s]);
        }
    }
    assert(cash_sum == total_cash);
    for (uint32_t s = 0; s < H_SYMBOLS; s++)
        assert(pos_sum[s] == total_pos[s]);
}

static void test_conservation_invariants(void)
{
    enum { REQUESTS = 200000 };
    harness_t *h = malloc(sizeof(*h));
    assert(h != NULL);
    h_setup(h);

    int64_t total_cash = 0, total_pos[H_SYMBOLS] = {0};
    for (uint32_t a = 0; a < H_ACCOUNTS; a++) {
        total_cash += cash_of(h->ex, a);
        for (uint32_t s = 0; s < H_SYMBOLS; s++)
            total_pos[s] += pos_of(h->ex, a, s);
    }

    srand(2024); /* fixed seed: this test must be reproducible */
    for (uint64_t seq = 1; seq <= REQUESTS; seq++) {
        ex_request_t req = h_gen(h, seq);
        h_submit(h, &req);
        check_invariants(h, total_cash, total_pos);
    }

    /* The stream must have exercised the rules it claims to check. */
    assert(h->fill > 1000 && h->done > 1000);
    for (int r = EX_REJ_UNKNOWN_ACCOUNT; r <= EX_REJ_CAPACITY; r++)
        assert(h->reject[r] > 0);

    ex_free(h->ex);
    free(h);
}

/* ---- differential test against a naive reference exchange -------- */

typedef struct {
    uint64_t id;
    uint32_t account, symbol;
    lob_side_t side;
    int64_t price;
    uint32_t qty;
    uint64_t arrival;
} norder_t;

typedef struct {
    norder_t o[H_MAX_ORDERS];
    size_t n;
    uint64_t arrival, next_id, next_exec;
    int64_t cash[H_ACCOUNTS];
    int64_t pos[H_ACCOUNTS][H_SYMBOLS];
    int64_t lo[H_SYMBOLS], hi[H_SYMBOLS];

    ex_event_t ev[H_MAX_EVENTS];
    size_t nev;
    uint64_t cur_seq;
    int64_t cur_ts;
} nex_t;

static void n_emit(nex_t *x, ex_event_kind_t kind, uint32_t acct, uint64_t id, ex_event_t ev)
{
    ev.kind = kind;
    ev.seq = x->cur_seq;
    ev.ts_ns = x->cur_ts;
    ev.account = acct;
    ev.order_id = id;
    assert(x->nev < H_MAX_EVENTS);
    x->ev[x->nev++] = ev;
}

static void n_simple(nex_t *x, ex_event_kind_t kind, uint32_t acct, uint64_t id,
                     ex_reject_t reason, uint32_t qty)
{
    ex_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.reason = reason;
    ev.qty = qty;
    n_emit(x, kind, acct, id, ev);
}

/* Reservations are never stored, only recomputed from the open orders. */
static int64_t n_avail_cash(const nex_t *x, uint32_t a)
{
    int64_t c = x->cash[a];
    for (size_t i = 0; i < x->n; i++)
        if (x->o[i].account == a && x->o[i].side == LOB_SIDE_BUY)
            c -= x->o[i].price * (int64_t)x->o[i].qty;
    return c;
}

static int64_t n_avail_pos(const nex_t *x, uint32_t a, uint32_t s)
{
    int64_t p = x->pos[a][s];
    for (size_t i = 0; i < x->n; i++)
        if (x->o[i].account == a && x->o[i].symbol == s && x->o[i].side == LOB_SIDE_SELL)
            p -= x->o[i].qty;
    return p;
}

/* Index of the best order on `side` of symbol s with qty left in
 * left[] (NULL = use o[].qty) that a taker with the given limit
 * reaches; price priority, then arrival. -1 if none.
 */
static int n_best(const nex_t *x, uint32_t s, lob_side_t side, int has_limit, int64_t limit,
                  const uint32_t *left)
{
    int best = -1;
    for (size_t i = 0; i < x->n; i++) {
        const norder_t *o = &x->o[i];
        uint32_t q = left ? left[i] : o->qty;
        if (o->symbol != s || o->side != side || q == 0)
            continue;
        if (has_limit && (side == LOB_SIDE_SELL ? o->price > limit : o->price < limit))
            continue;
        if (best < 0) {
            best = (int)i;
            continue;
        }
        const norder_t *b = &x->o[best];
        int better = (side == LOB_SIDE_SELL) ? o->price < b->price : o->price > b->price;
        if (better || (o->price == b->price && o->arrival < b->arrival))
            best = (int)i;
    }
    return best;
}

static void n_remove(nex_t *x, size_t i)
{
    x->o[i] = x->o[--x->n];
}

static void n_submit_new(nex_t *x, const ex_request_t *r, uint32_t book_orders)
{
    uint32_t a = r->account, s = r->symbol;
    int is_market = (r->type == EX_MARKET);
    if (a >= H_ACCOUNTS) { n_simple(x, EX_EV_REJECT, a, 0, EX_REJ_UNKNOWN_ACCOUNT, 0); return; }
    if (s >= H_SYMBOLS) { n_simple(x, EX_EV_REJECT, a, 0, EX_REJ_UNKNOWN_SYMBOL, 0); return; }
    if (r->qty == 0) { n_simple(x, EX_EV_REJECT, a, 0, EX_REJ_BAD_QTY, 0); return; }
    if (!is_market && (r->price_ticks < x->lo[s] || r->price_ticks > x->hi[s])) {
        n_simple(x, EX_EV_REJECT, a, 0, EX_REJ_BAD_PRICE, 0);
        return;
    }
    uint32_t in_symbol = 0;
    for (size_t i = 0; i < x->n; i++)
        in_symbol += x->o[i].symbol == s;
    if (x->n >= H_MAX_ORDERS || (r->type == EX_LIMIT && in_symbol >= book_orders)) {
        n_simple(x, EX_EV_REJECT, a, 0, EX_REJ_CAPACITY, 0);
        return;
    }

    if (r->side == LOB_SIDE_BUY) {
        int64_t need = r->price_ticks * (int64_t)r->qty;
        if (is_market) { /* walk a scratch copy of the asks */
            uint32_t left[H_MAX_ORDERS];
            for (size_t i = 0; i < x->n; i++)
                left[i] = x->o[i].qty;
            uint32_t want = r->qty;
            need = 0;
            for (int i; want > 0 && (i = n_best(x, s, LOB_SIDE_SELL, 0, 0, left)) >= 0;) {
                uint32_t take = left[i] < want ? left[i] : want;
                need += x->o[i].price * (int64_t)take;
                left[i] -= take;
                want -= take;
            }
        }
        if (n_avail_cash(x, a) < need) {
            n_simple(x, EX_EV_REJECT, a, 0, EX_REJ_INSUFFICIENT_CASH, 0);
            return;
        }
    } else if (n_avail_pos(x, a, s) < (int64_t)r->qty) {
        n_simple(x, EX_EV_REJECT, a, 0, EX_REJ_INSUFFICIENT_POSITION, 0);
        return;
    }

    uint64_t id = ++x->next_id;
    n_simple(x, EX_EV_ACK, a, id, 0, 0);

    lob_side_t opp = r->side == LOB_SIDE_BUY ? LOB_SIDE_SELL : LOB_SIDE_BUY;
    uint32_t rem = r->qty;
    for (int i; rem > 0 && (i = n_best(x, s, opp, !is_market, r->price_ticks, NULL)) >= 0;) {
        norder_t *rest = &x->o[i];
        uint32_t q = rest->qty < rem ? rest->qty : rem;
        uint32_t buyer = r->side == LOB_SIDE_BUY ? a : rest->account;
        uint32_t seller = r->side == LOB_SIDE_BUY ? rest->account : a;
        uint64_t buy_id = r->side == LOB_SIDE_BUY ? id : rest->id;
        uint64_t sell_id = r->side == LOB_SIDE_BUY ? rest->id : id;
        uint64_t exec = ++x->next_exec;
        int64_t px = rest->price;

        x->cash[buyer] -= px * (int64_t)q;
        x->pos[buyer][s] += q;
        x->cash[seller] += px * (int64_t)q;
        x->pos[seller][s] -= q;
        rem -= q;
        rest->qty -= q;

        ex_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.symbol = s;
        ev.price_ticks = px;
        ev.qty = q;
        ev.exec_id = exec;
        ev.side = LOB_SIDE_BUY;
        ev.counter_order_id = sell_id;
        ev.is_aggressor = r->side == LOB_SIDE_BUY;
        n_emit(x, EX_EV_FILL, buyer, buy_id, ev);
        ev.side = LOB_SIDE_SELL;
        ev.counter_order_id = buy_id;
        ev.is_aggressor = r->side == LOB_SIDE_SELL;
        n_emit(x, EX_EV_FILL, seller, sell_id, ev);

        if (rest->qty == 0) {
            n_simple(x, EX_EV_DONE, rest->account, rest->id, 0, 0);
            n_remove(x, (size_t)i);
        }
    }

    if (r->type == EX_LIMIT && rem > 0) {
        norder_t *o = &x->o[x->n++];
        o->id = id; o->account = a; o->symbol = s; o->side = r->side;
        o->price = r->price_ticks; o->qty = rem; o->arrival = ++x->arrival;
    } else {
        n_simple(x, EX_EV_DONE, a, id, 0, rem);
    }
}

static void n_submit(nex_t *x, const ex_request_t *r, uint32_t book_orders)
{
    x->nev = 0;
    x->cur_seq = r->seq;
    x->cur_ts = r->ts_ns;
    if (r->kind == EX_REQ_NEW) {
        n_submit_new(x, r, book_orders);
        return;
    }

    size_t i = 0;
    while (i < x->n && x->o[i].id != r->order_id)
        i++;
    if (i == x->n) { n_simple(x, EX_EV_REJECT, r->account, r->order_id, EX_REJ_UNKNOWN_ORDER, 0); return; }
    if (x->o[i].account != r->account) {
        n_simple(x, EX_EV_REJECT, r->account, r->order_id, EX_REJ_NOT_OWNER, 0);
        return;
    }
    if (r->kind == EX_REQ_CANCEL) {
        n_simple(x, EX_EV_ACK, r->account, r->order_id, 0, 0);
        n_simple(x, EX_EV_DONE, r->account, r->order_id, 0, x->o[i].qty);
        n_remove(x, i);
    } else if (r->qty == 0 || r->qty >= x->o[i].qty) {
        n_simple(x, EX_EV_REJECT, r->account, r->order_id, EX_REJ_BAD_QTY, 0);
    } else {
        n_simple(x, EX_EV_ACK, r->account, r->order_id, 0, 0);
        x->o[i].qty = r->qty;
    }
}

static int same_event(const ex_event_t *a, const ex_event_t *b)
{
    return a->kind == b->kind && a->seq == b->seq && a->ts_ns == b->ts_ns &&
           a->account == b->account && a->order_id == b->order_id && a->reason == b->reason &&
           a->symbol == b->symbol && a->side == b->side && a->price_ticks == b->price_ticks &&
           a->qty == b->qty && a->exec_id == b->exec_id &&
           a->counter_order_id == b->counter_order_id && a->is_aggressor == b->is_aggressor;
}

static void test_differential_against_naive_exchange(void)
{
    enum { REQUESTS = 100000 };
    harness_t *h = malloc(sizeof(*h));
    nex_t *x = calloc(1, sizeof(*x));
    assert(h != NULL && x != NULL);
    h_setup(h);

    for (uint32_t a = 0; a < H_ACCOUNTS; a++) {
        x->cash[a] = cash_of(h->ex, a);
        for (uint32_t s = 0; s < H_SYMBOLS; s++)
            x->pos[a][s] = pos_of(h->ex, a, s);
    }
    for (uint32_t s = 0; s < H_SYMBOLS; s++) {
        x->lo[s] = h->lo[s];
        x->hi[s] = h->hi[s];
    }

    srand(99); /* fixed seed: this test must be reproducible */
    for (uint64_t seq = 1; seq <= REQUESTS; seq++) {
        ex_request_t req = h_gen(h, seq);
        h_submit(h, &req);
        n_submit(x, &req, H_BOOK_ORDERS);

        /* Same events, in the same order, field for field. */
        assert(h->nev == x->nev);
        for (size_t i = 0; i < h->nev; i++)
            assert(same_event(&h->ev[i], &x->ev[i]));

        /* Same balances, and the same open orders with the same reservations. */
        for (uint32_t a = 0; a < H_ACCOUNTS; a++) {
            int64_t cash, rc;
            assert(ex_get_balance(h->ex, a, &cash, &rc) == 0);
            assert(cash == x->cash[a] && cash - rc == n_avail_cash(x, a));
            for (uint32_t s = 0; s < H_SYMBOLS; s++) {
                int64_t pos, rp;
                assert(ex_get_position(h->ex, a, s, &pos, &rp) == 0);
                assert(pos == x->pos[a][s] && pos - rp == n_avail_pos(x, a, s));
            }
        }
        assert(h->nlive == x->n);
        for (size_t i = 0; i < x->n; i++) {
            ex_order_info_t o;
            assert(ex_get_order(h->ex, x->o[i].id, &o) == 0);
            assert(o.account == x->o[i].account && o.symbol == x->o[i].symbol);
            assert(o.side == x->o[i].side && o.price_ticks == x->o[i].price);
            assert(o.open_qty == x->o[i].qty);
        }
    }

    assert(h->fill > 1000);
    for (int r = EX_REJ_UNKNOWN_ACCOUNT; r <= EX_REJ_CAPACITY; r++)
        if (h->reject[r] == 0) { fprintf(stderr, "no reject %d\n", r); abort(); }

    ex_free(h->ex);
    free(h);
    free(x);
}

int main(void)
{
    test_setup_and_symbol_table();
    test_ack_echoes_seq_and_ids_are_assigned();
    test_reject_reasons_leave_state_untouched();
    test_capacity_rejects();
    test_limit_buy_reserves_and_refunds_on_better_fill();
    test_sell_reserves_shares_and_no_shorting();
    test_ioc_cancels_remainder();
    test_market_buy_prices_exactly_by_walking_the_book();
    test_market_orders_are_attributed_to_the_right_accounts();
    test_cancel_and_reduce_release_reservations();
    test_self_trade_is_allowed_and_nets_to_zero();
    test_conservation_invariants();
    test_differential_against_naive_exchange();

    printf("all exchange tests passed\n");
    return 0;
}
