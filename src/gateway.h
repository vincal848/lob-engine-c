/* gateway.h -- the M5b threaded plumbing around the exchange state machine.
 *
 * Design (see docs/EXCHANGE.md):
 *   - One SPSC ring per producer inbound (requests) and one outbound
 *     (events). Power-of-two capacity, preallocated, _Atomic head/tail
 *     with acquire/release, head and tail on separate cache lines.
 *   - One sequencer thread owns the exchange. It polls the inbound
 *     rings round-robin (one request per ring per pass), stamps seq
 *     (from 1) and ts_ns (CLOCK_MONOTONIC), appends the stamped request
 *     to the journal, calls ex_submit(), writes the fill tape, and
 *     routes each event to outbound ring `account % producers`.
 *   - The journal fully determines the tape: gw_replay() feeds a
 *     journal single-threaded through a fresh exchange and must
 *     reproduce the live tape byte for byte.
 *   - A full inbound ring makes gw_submit() return GW_BUSY; the
 *     sequencer never blocks on a producer and never drops a request.
 *     It does drop an event whose outbound ring is full (producers
 *     must drain), and a fill when the tape is full; both are counted
 *     in gw_stats_t. If the journal reaches capacity it stops consuming
 *     (producers then see GW_BUSY), because an unjournaled request
 *     would break replay.
 */
#ifndef GATEWAY_H
#define GATEWAY_H

#include <stddef.h>
#include <stdint.h>

#include "exchange.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- SPSC ring: one producer thread, one consumer thread --------- */

typedef struct gw_ring gw_ring_t;

/* Copies of `item_size`-byte items; capacity must be a power of two.
 * NULL on a bad argument or allocation failure.
 */
gw_ring_t *gw_ring_new(size_t capacity, size_t item_size);
void gw_ring_free(gw_ring_t *r);                  /* safe with NULL */
int gw_ring_push(gw_ring_t *r, const void *item); /* 1 pushed, 0 full; producer only */
int gw_ring_pop(gw_ring_t *r, void *item);        /* 1 popped, 0 empty; consumer only */

/* ---- gateway ------------------------------------------------------ */

#define GW_BUSY 1 /* gw_submit(): the producer's inbound ring is full */

/* One fill, one record. No padding bytes are left uninitialised, so two
 * tapes compare with memcmp. ts_ns is the journaled timestamp of the
 * request that caused the fill. aggressor_side is a lob_side_t.
 */
typedef struct {
    uint64_t seq;
    uint64_t exec_id;
    int64_t price_ticks;
    uint64_t buy_order_id;
    uint64_t sell_order_id;
    int64_t ts_ns;
    uint32_t symbol;
    uint32_t qty;
    uint32_t buy_account;
    uint32_t sell_account;
    uint32_t aggressor_side;
    uint32_t reserved; /* always 0; keeps the record free of padding */
} gw_fill_t;

typedef struct {
    uint32_t producers;     /* >= 1 */
    uint32_t in_capacity;   /* inbound ring slots per producer; power of two */
    uint32_t out_capacity;  /* outbound ring slots per producer; power of two */
    size_t max_requests;    /* journal capacity */
    size_t max_fills;       /* tape capacity */
} gw_config_t;

typedef struct {
    uint64_t events_dropped; /* an outbound ring was full */
    uint64_t fills_dropped;  /* the tape was full */
    int journal_full;        /* the journal reached capacity; later requests stay in their rings */
} gw_stats_t;

typedef struct gw gw_t;

/* Takes over ex's event sink (the exchange stays owned by the caller
 * and must outlive the gateway) and starts the sequencer thread.
 * Add all symbols and accounts first. NULL on a bad argument or
 * failure. The only allocations are here.
 */
gw_t *gw_new(ex_t *ex, const gw_config_t *cfg);

/* Producer `p` (each in at most one thread) submits a request; seq and
 * ts_ns are overwritten by the sequencer. Returns 0, or GW_BUSY. The
 * request's account decides which producer's outbound ring gets its
 * events.
 */
int gw_submit(gw_t *gw, uint32_t p, const ex_request_t *req);

/* Producer `p` takes its next event; 1 got one, 0 none. */
int gw_poll_event(gw_t *gw, uint32_t p, ex_event_t *ev);

/* Stops the sequencer after it has drained every inbound ring and joins
 * it. Producers must have finished submitting. Idempotent.
 */
void gw_stop(gw_t *gw);

/* After gw_stop() only: the journal, the tape and the counters. */
const ex_request_t *gw_journal(const gw_t *gw, size_t *n_out);
const gw_fill_t *gw_tape(const gw_t *gw, size_t *n_out);
gw_stats_t gw_stats(const gw_t *gw);

/* Stops if needed, frees the gateway (not the exchange). Safe with NULL. */
void gw_free(gw_t *gw);

/* Replays an already-stamped journal single-threaded through `ex` (a
 * fresh exchange set up like the live one; its event sink is replaced)
 * and writes the tape to `tape` (room for `cap` records). Returns the
 * number of records written; fills beyond `cap` are discarded.
 */
size_t gw_replay(ex_t *ex, const ex_request_t *journal, size_t n, gw_fill_t *tape, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* GATEWAY_H */
