/* ex_random.h -- a seeded random request stream for the exchange,
 * shared by the invariant and differential tests in
 * exchange_tests.c and by the allocation check in
 * alloc_exchange_test.c. Header-only; everything is static.
 *
 * The harness owns an exchange with three symbols (one deliberately
 * narrow, so the market-buy walk and crossing are frequent) and a few
 * accounts, some poor enough to hit the cash and position checks. It
 * tracks which orders are live from the event stream alone, so the
 * generator can aim cancels and reduces at real orders (and, on
 * purpose, at the wrong account or a dead id some of the time).
 */
#ifndef EX_RANDOM_H
#define EX_RANDOM_H

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "../src/exchange.h"

#define H_SYMBOLS 3
#define H_ACCOUNTS 6
#define H_MAX_ORDERS 32
#define H_BOOK_ORDERS 10
#define H_MAX_EVENTS 512

typedef struct {
    ex_t *ex;
    int64_t lo[H_SYMBOLS];
    int64_t hi[H_SYMBOLS];

    ex_req_kind_t cur_kind;
    uint64_t live_id[H_MAX_ORDERS];
    uint32_t live_acct[H_MAX_ORDERS];
    size_t nlive;

    ex_event_t ev[H_MAX_EVENTS]; /* events of the last request */
    size_t nev;

    unsigned long ack, fill, done;
    unsigned long reject[16]; /* by ex_reject_t */
} harness_t;

static inline void h_sink(const ex_event_t *ev, void *user_data)
{
    harness_t *h = user_data;
    assert(h->nev < H_MAX_EVENTS);
    h->ev[h->nev++] = *ev;

    if (ev->kind == EX_EV_ACK) {
        h->ack++;
        if (h->cur_kind == EX_REQ_NEW) {
            assert(h->nlive < H_MAX_ORDERS);
            h->live_id[h->nlive] = ev->order_id;
            h->live_acct[h->nlive++] = ev->account;
        }
    } else if (ev->kind == EX_EV_REJECT) {
        h->reject[ev->reason]++;
    } else if (ev->kind == EX_EV_FILL) {
        h->fill++;
    } else {
        h->done++;
        for (size_t i = 0; i < h->nlive; i++) {
            if (h->live_id[i] == ev->order_id) {
                h->live_id[i] = h->live_id[h->nlive - 1];
                h->live_acct[i] = h->live_acct[h->nlive - 1];
                h->nlive--;
                break;
            }
        }
    }
}

static inline void h_setup(harness_t *h)
{
    static const char *const names[H_SYMBOLS] = {"AAA", "BBB", "CCC"};
    static const int64_t lo[H_SYMBOLS] = {0, 20, 50};
    static const int64_t hi[H_SYMBOLS] = {99, 120, 70};
    ex_config_t cfg = {.max_symbols = H_SYMBOLS, .max_accounts = H_ACCOUNTS,
                       .max_orders = H_MAX_ORDERS, .book_orders = H_BOOK_ORDERS,
                       .max_window_ticks = 128};

    memset(h, 0, sizeof(*h));
    h->ex = ex_new(&cfg);
    assert(h->ex != NULL);
    for (int s = 0; s < H_SYMBOLS; s++) {
        h->lo[s] = lo[s];
        h->hi[s] = hi[s];
        assert(ex_add_symbol(h->ex, names[s], lo[s], hi[s]) == s);
    }
    for (int a = 0; a < H_ACCOUNTS; a++) {
        int64_t pos[H_SYMBOLS] = {10 + 15 * a, 40 - 5 * a, 20};
        assert(ex_add_account(h->ex, 1500 + 2500 * a, pos) == a);
    }
    ex_set_event_sink(h->ex, h_sink, h);
}

static inline void h_submit(harness_t *h, const ex_request_t *req)
{
    h->cur_kind = req->kind;
    h->nev = 0;
    ex_submit(h->ex, req);
}

/* One random request. seq counts up from 1; ts_ns is derived from it. */
static inline ex_request_t h_gen(harness_t *h, uint64_t seq)
{
    ex_request_t req;
    memset(&req, 0, sizeof(req));
    req.seq = seq;
    req.ts_ns = (int64_t)seq * 1000;

    int roll = rand() % 100;
    if (roll >= 65 && roll < 95 && h->nlive == 0)
        roll = 0;

    if (roll < 65 || roll >= 95) {
        req.kind = EX_REQ_NEW;
        req.account = (uint32_t)(rand() % H_ACCOUNTS);
        req.symbol = (uint32_t)(rand() % H_SYMBOLS);
        req.side = rand() % 2 ? LOB_SIDE_BUY : LOB_SIDE_SELL;
        req.type = roll < 45 ? EX_LIMIT : roll < 55 ? EX_IOC : roll < 65 ? EX_MARKET
                                                                        : (ex_order_type_t)(rand() % 3);
        int64_t lo = h->lo[req.symbol], hi = h->hi[req.symbol];
        req.price_ticks = lo + rand() % (hi - lo + 1);
        req.qty = 1 + (uint32_t)(rand() % 8);
        if (roll >= 95) { /* junk: break exactly one thing */
            switch (rand() % 5) {
            case 0: req.account = H_ACCOUNTS + 1; break;
            case 1: req.symbol = H_SYMBOLS; break;
            case 2: req.qty = 0; break;
            case 3: req.price_ticks = hi + 1; req.type = EX_LIMIT; break; /* market ignores price */
            default: req.price_ticks = lo - 1; req.type = EX_IOC; break;
            }
        }
        return req;
    }

    size_t k = (size_t)rand() % h->nlive;
    req.order_id = h->live_id[k];
    req.account = h->live_acct[k];
    if (rand() % 10 == 0)
        req.account = (req.account + 1) % H_ACCOUNTS; /* NOT_OWNER */
    if (rand() % 20 == 0)
        req.order_id += 1000000; /* UNKNOWN_ORDER */

    if (roll < 80) {
        req.kind = EX_REQ_CANCEL;
    } else {
        ex_order_info_t info;
        req.kind = EX_REQ_REDUCE;
        req.qty = ex_get_order(h->ex, req.order_id, &info) == 0
            ? (uint32_t)(rand() % 3 == 0 ? 0 : 1 + rand() % (int)info.open_qty)
            : 1;
    }
    return req;
}

/* splitmix64: a tiny seeded generator that keeps its state in the caller's
 * variable, for threads (rand() shares one state between all of them).
 */
static inline uint64_t ex_splitmix(uint64_t *state)
{
    uint64_t z = (*state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

#endif /* EX_RANDOM_H */
