/* gateway.c -- the M5b gateway: SPSC rings, the sequencer thread, the
 * in-memory journal and the fill tape. See gateway.h.
 *
 * The tape is built by one small sink (tape_event) shared by the live
 * sequencer and gw_replay(), so replay cannot drift from live. The
 * exchange emits the buyer's FILL event and the seller's FILL event
 * back to back with the same exec_id; the sink pairs them.
 */
#define _POSIX_C_SOURCE 200809L /* clock_gettime, sched_yield under -std=c11 */

#include "gateway.h"

#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CACHE_LINE 64

/* ---- ring --------------------------------------------------------- */

struct gw_ring {
    alignas(CACHE_LINE) _Atomic uint64_t head; /* next slot to pop; written by the consumer */
    alignas(CACHE_LINE) _Atomic uint64_t tail; /* next slot to push; written by the producer */
    alignas(CACHE_LINE) uint64_t mask;
    size_t item_size;
    unsigned char *buf;
};

gw_ring_t *gw_ring_new(size_t capacity, size_t item_size)
{
    if (capacity == 0 || (capacity & (capacity - 1)) != 0 || item_size == 0)
        return NULL;
    /* aligned_alloc wants a size that is a multiple of the alignment */
    gw_ring_t *r = aligned_alloc(CACHE_LINE, (sizeof(*r) + CACHE_LINE - 1) / CACHE_LINE * CACHE_LINE);
    unsigned char *buf = calloc(capacity, item_size);
    if (r == NULL || buf == NULL) {
        free(r);
        free(buf);
        return NULL;
    }
    atomic_init(&r->head, 0);
    atomic_init(&r->tail, 0);
    r->mask = capacity - 1;
    r->item_size = item_size;
    r->buf = buf;
    return r;
}

void gw_ring_free(gw_ring_t *r)
{
    if (r == NULL)
        return;
    free(r->buf);
    free(r);
}

int gw_ring_push(gw_ring_t *r, const void *item)
{
    uint64_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
    uint64_t h = atomic_load_explicit(&r->head, memory_order_acquire);
    if (t - h == r->mask + 1)
        return 0;
    memcpy(r->buf + (t & r->mask) * r->item_size, item, r->item_size);
    atomic_store_explicit(&r->tail, t + 1, memory_order_release);
    return 1;
}

int gw_ring_pop(gw_ring_t *r, void *item)
{
    uint64_t h = atomic_load_explicit(&r->head, memory_order_relaxed);
    uint64_t t = atomic_load_explicit(&r->tail, memory_order_acquire);
    if (h == t)
        return 0;
    memcpy(item, r->buf + (h & r->mask) * r->item_size, r->item_size);
    atomic_store_explicit(&r->head, h + 1, memory_order_release);
    return 1;
}

/* ---- tape --------------------------------------------------------- */

typedef struct {
    gw_fill_t *rec;
    size_t n, cap;
    uint64_t dropped;
    ex_event_t pending; /* the first FILL event of a pair */
    int has_pending;
} tape_t;

static void tape_event(tape_t *t, const ex_event_t *ev)
{
    if (ev->kind != EX_EV_FILL)
        return;
    if (!t->has_pending) {
        t->pending = *ev;
        t->has_pending = 1;
        return;
    }
    t->has_pending = 0;
    assert(t->pending.exec_id == ev->exec_id);

    const ex_event_t *b = ev->side == LOB_SIDE_BUY ? ev : &t->pending;
    const ex_event_t *s = ev->side == LOB_SIDE_BUY ? &t->pending : ev;
    if (t->n == t->cap) {
        t->dropped++;
        return;
    }
    gw_fill_t *f = &t->rec[t->n++];
    memset(f, 0, sizeof(*f));
    f->seq = b->seq;
    f->exec_id = b->exec_id;
    f->price_ticks = b->price_ticks;
    f->buy_order_id = b->order_id;
    f->sell_order_id = s->order_id;
    f->ts_ns = b->ts_ns;
    f->symbol = b->symbol;
    f->qty = b->qty;
    f->buy_account = b->account;
    f->sell_account = s->account;
    f->aggressor_side = (uint32_t)(b->is_aggressor ? LOB_SIDE_BUY : LOB_SIDE_SELL);
}

static void replay_sink(const ex_event_t *ev, void *user_data)
{
    tape_event(user_data, ev);
}

size_t gw_replay(ex_t *ex, const ex_request_t *journal, size_t n, gw_fill_t *tape, size_t cap)
{
    tape_t t = {.rec = tape, .cap = cap};
    ex_set_event_sink(ex, replay_sink, &t);
    for (size_t i = 0; i < n; i++)
        ex_submit(ex, &journal[i]);
    return t.n;
}

/* ---- gateway ------------------------------------------------------ */

struct gw {
    ex_t *ex;
    uint32_t producers;
    gw_ring_t **in;  /* [producers] */
    gw_ring_t **out; /* [producers] */

    ex_request_t *journal; /* written by the sequencer; read after the join */
    size_t nj, max_requests;
    tape_t tape;
    uint64_t events_dropped;
    int journal_full;

    pthread_t thread;
    int running;
    _Atomic int stop;
};

static void route_event(const ex_event_t *ev, void *user_data)
{
    gw_t *gw = user_data;
    tape_event(&gw->tape, ev);
    if (!gw_ring_push(gw->out[ev->account % gw->producers], ev))
        gw->events_dropped++;
}

static int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static void *sequencer(void *arg)
{
    gw_t *gw = arg;
    for (;;) {
        /* Read stop BEFORE the pass: if it was set and the pass finds
         * nothing, every request submitted before gw_stop() was seen. */
        int stopping = atomic_load_explicit(&gw->stop, memory_order_acquire);
        int worked = 0;
        for (uint32_t p = 0; p < gw->producers; p++) {
            if (gw->nj == gw->max_requests) {
                gw->journal_full = 1;
                break;
            }
            ex_request_t *rec = &gw->journal[gw->nj];
            if (!gw_ring_pop(gw->in[p], rec))
                continue;
            rec->seq = (uint64_t)++gw->nj;
            rec->ts_ns = now_ns();
            ex_submit(gw->ex, rec);
            worked = 1;
        }
        if (!worked) {
            if (stopping)
                return NULL;
            sched_yield();
        }
    }
}

gw_t *gw_new(ex_t *ex, const gw_config_t *cfg)
{
    if (ex == NULL || cfg == NULL || cfg->producers == 0 || cfg->max_requests == 0 ||
        cfg->max_fills == 0)
        return NULL;
    gw_t *gw = calloc(1, sizeof(*gw));
    if (gw == NULL)
        return NULL;
    gw->ex = ex;
    gw->producers = cfg->producers;
    gw->max_requests = cfg->max_requests;
    gw->in = calloc(cfg->producers, sizeof(*gw->in));
    gw->out = calloc(cfg->producers, sizeof(*gw->out));
    gw->journal = calloc(cfg->max_requests, sizeof(*gw->journal));
    gw->tape.rec = calloc(cfg->max_fills, sizeof(*gw->tape.rec));
    gw->tape.cap = cfg->max_fills;
    int ok = gw->in && gw->out && gw->journal && gw->tape.rec;
    for (uint32_t p = 0; ok && p < cfg->producers; p++) {
        gw->in[p] = gw_ring_new(cfg->in_capacity, sizeof(ex_request_t));
        gw->out[p] = gw_ring_new(cfg->out_capacity, sizeof(ex_event_t));
        ok = gw->in[p] && gw->out[p];
    }
    if (ok) {
        ex_set_event_sink(ex, route_event, gw);
        ok = pthread_create(&gw->thread, NULL, sequencer, gw) == 0;
        gw->running = ok;
    }
    if (!ok) {
        gw_free(gw);
        return NULL;
    }
    return gw;
}

int gw_submit(gw_t *gw, uint32_t p, const ex_request_t *req)
{
    return gw_ring_push(gw->in[p], req) ? 0 : GW_BUSY;
}

int gw_poll_event(gw_t *gw, uint32_t p, ex_event_t *ev)
{
    return gw_ring_pop(gw->out[p], ev);
}

void gw_stop(gw_t *gw)
{
    if (!gw->running)
        return;
    atomic_store_explicit(&gw->stop, 1, memory_order_release);
    pthread_join(gw->thread, NULL);
    gw->running = 0;
}

const ex_request_t *gw_journal(const gw_t *gw, size_t *n_out)
{
    *n_out = gw->nj;
    return gw->journal;
}

const gw_fill_t *gw_tape(const gw_t *gw, size_t *n_out)
{
    *n_out = gw->tape.n;
    return gw->tape.rec;
}

gw_stats_t gw_stats(const gw_t *gw)
{
    return (gw_stats_t){.events_dropped = gw->events_dropped,
                        .fills_dropped = gw->tape.dropped,
                        .journal_full = gw->journal_full};
}

void gw_free(gw_t *gw)
{
    if (gw == NULL)
        return;
    gw_stop(gw);
    for (uint32_t p = 0; p < gw->producers; p++) {
        if (gw->in)
            gw_ring_free(gw->in[p]);
        if (gw->out)
            gw_ring_free(gw->out[p]);
    }
    free(gw->in);
    free(gw->out);
    free(gw->journal);
    free(gw->tape.rec);
    free(gw);
}
