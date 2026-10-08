/* Assert-based tests for the M5b gateway (src/gateway.c). Run via
 * `make test`, `make asan` and `make tsan`.
 *
 * test_ring_* pin the SPSC ring: full/empty, wraparound, bad sizes,
 * and one producer thread against one consumer thread (the part TSan
 * is for). test_determinism is the point of M5b: N producer threads
 * submit seeded random flows concurrently, the sequencer interleaves
 * them however the scheduler likes, and the journal it wrote is then
 * replayed single-threaded through a fresh exchange; the fill tapes
 * must be byte-identical. The same test also shows it can fail: a
 * journal with one timestamp nudged must give a different tape.
 *
 * Thread and iteration counts are fixed and small, and nothing sleeps
 * for correctness: threads spin with sched_yield() until the ring has
 * room or data.
 */
#define _POSIX_C_SOURCE 200809L /* sched_yield under -std=c11 */

#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/gateway.h"
#include "ex_random.h"

/* ---- ring --------------------------------------------------------- */

static void test_ring_full_empty_and_wraparound(void)
{
    uint64_t v;
    assert(gw_ring_new(0, 8) == NULL);
    assert(gw_ring_new(3, 8) == NULL); /* not a power of two */
    assert(gw_ring_new(4, 0) == NULL);

    gw_ring_t *r = gw_ring_new(4, sizeof(v));
    assert(r != NULL);
    assert(!gw_ring_pop(r, &v)); /* empty */

    for (v = 1; v <= 4; v++)
        assert(gw_ring_push(r, &v));
    v = 5;
    assert(!gw_ring_push(r, &v)); /* full: not dropped, not overwritten */
    for (uint64_t want = 1; want <= 4; want++) {
        assert(gw_ring_pop(r, &v) && v == want);
    }
    assert(!gw_ring_pop(r, &v));

    /* Many times around a 4-slot ring, with a varying fill level. */
    uint64_t next_in = 100, next_out = 100;
    for (int i = 0; i < 1000; i++) {
        int burst = 1 + i % 4;
        for (int k = 0; k < burst; k++, next_in++)
            assert(gw_ring_push(r, &next_in));
        if (burst == 4)
            assert(!gw_ring_push(r, &next_in));
        for (int k = 0; k < burst; k++, next_out++) {
            assert(gw_ring_pop(r, &v) && v == next_out);
        }
    }
    gw_ring_free(r);
    gw_ring_free(NULL);
}

#define RING_ITEMS 200000
static gw_ring_t *thr_ring;

static void *ring_producer(void *arg)
{
    (void)arg;
    for (uint64_t i = 0; i < RING_ITEMS; i++)
        while (!gw_ring_push(thr_ring, &i))
            sched_yield();
    return NULL;
}

static void test_ring_two_threads_in_order(void)
{
    pthread_t t;
    thr_ring = gw_ring_new(64, sizeof(uint64_t));
    assert(thr_ring != NULL);
    assert(pthread_create(&t, NULL, ring_producer, NULL) == 0);
    for (uint64_t want = 0; want < RING_ITEMS; want++) {
        uint64_t v;
        while (!gw_ring_pop(thr_ring, &v))
            sched_yield();
        assert(v == want);
    }
    pthread_join(t, NULL);
    gw_ring_free(thr_ring);
}

/* ---- determinism -------------------------------------------------- */

#define PRODUCERS 4
#define PER_PRODUCER 2000
#define TOTAL ((size_t)PRODUCERS * PER_PRODUCER)
#define TAPE_CAP (4 * TOTAL)

static gw_t *gw;

typedef struct {
    uint32_t id;
    uint64_t rng;       /* splitmix64 state; rand() is shared between threads */
    uint64_t live[H_MAX_ORDERS * 2]; /* ids this producer has seen ACKed and not DONE */
    size_t nlive;
    unsigned long busy; /* times the inbound ring was full */
    unsigned long events;
} producer_t;

static uint64_t rnd(producer_t *p)
{
    uint64_t z = (p->rng += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

static void absorb(producer_t *p, const ex_event_t *ev)
{
    p->events++;
    size_t i;
    for (i = 0; i < p->nlive && p->live[i] != ev->order_id; i++)
        ;
    if (ev->kind == EX_EV_ACK && i == p->nlive && p->nlive < sizeof(p->live) / sizeof(p->live[0]))
        p->live[p->nlive++] = ev->order_id;
    else if (ev->kind == EX_EV_DONE && i < p->nlive)
        p->live[i] = p->live[--p->nlive];
}

static void *producer(void *arg)
{
    producer_t *p = arg;
    /* the accounts whose a % PRODUCERS == id; their events come back to this producer */
    uint32_t mine[H_ACCOUNTS], nmine = 0;
    for (uint32_t a = p->id; a < H_ACCOUNTS; a += PRODUCERS)
        mine[nmine++] = a;
    assert(nmine > 0);

    for (int i = 0; i < PER_PRODUCER; i++) {
        ex_event_t ev;
        while (gw_poll_event(gw, p->id, &ev))
            absorb(p, &ev);

        ex_request_t req;
        memset(&req, 0, sizeof(req));
        req.account = mine[rnd(p) % nmine];
        uint64_t roll = rnd(p) % 100;
        if (roll < 25 && p->nlive > 0) {
            req.order_id = p->live[rnd(p) % p->nlive];
            req.kind = roll < 18 ? EX_REQ_CANCEL : EX_REQ_REDUCE;
            req.qty = (uint32_t)(rnd(p) % 4);
        } else {
            req.kind = EX_REQ_NEW;
            req.symbol = (uint32_t)(rnd(p) % H_SYMBOLS);
            req.side = rnd(p) % 2 ? LOB_SIDE_BUY : LOB_SIDE_SELL;
            req.type = roll < 70 ? EX_LIMIT : roll < 85 ? EX_IOC : EX_MARKET;
            /* 40..79 straddles all three windows: some prices cross, some are rejected */
            req.price_ticks = 40 + (int64_t)(rnd(p) % 40);
            req.qty = 1 + (uint32_t)(rnd(p) % 8);
        }
        while (gw_submit(gw, p->id, &req) == GW_BUSY) {
            p->busy++;
            sched_yield();
        }
    }
    return NULL;
}

static void test_determinism(void)
{
    harness_t live, replay;
    h_setup(&live);
    h_setup(&replay);

    /* a small inbound ring so producers really do hit GW_BUSY; big
     * outbound rings so no event is ever dropped; one spare journal slot
     * so journal_full (set on reaching capacity) stays clear */
    gw_config_t cfg = {.producers = PRODUCERS, .in_capacity = 16, .out_capacity = 32768,
                       .max_requests = TOTAL + 1, .max_fills = TAPE_CAP};
    gw = gw_new(live.ex, &cfg);
    assert(gw != NULL);

    pthread_t th[PRODUCERS];
    producer_t pr[PRODUCERS];
    for (uint32_t i = 0; i < PRODUCERS; i++) {
        memset(&pr[i], 0, sizeof(pr[i]));
        pr[i].id = i;
        pr[i].rng = 1000 + i; /* fixed seeds; the interleaving is what varies */
        assert(pthread_create(&th[i], NULL, producer, &pr[i]) == 0);
    }
    for (int i = 0; i < PRODUCERS; i++)
        pthread_join(th[i], NULL);
    gw_stop(gw);

    /* nothing lost, nothing out of order */
    size_t nj, nt;
    const ex_request_t *journal = gw_journal(gw, &nj);
    const gw_fill_t *tape = gw_tape(gw, &nt);
    gw_stats_t st = gw_stats(gw);
    assert(nj == TOTAL);
    assert(st.events_dropped == 0 && st.fills_dropped == 0 && !st.journal_full);
    for (size_t i = 0; i < nj; i++) {
        assert(journal[i].seq == i + 1);
        assert(i == 0 || journal[i].ts_ns >= journal[i - 1].ts_ns);
    }
    assert(nt > 50); /* the flow really trades, so the comparison below means something */
    for (size_t i = 0; i < nt; i++)
        assert(i == 0 || tape[i].seq >= tape[i - 1].seq);

    /* every event reached an outbound ring: 2 per fill is not in the
     * per-request count, so just check the rings hold what the
     * producers did not already drain */
    unsigned long seen = 0;
    for (uint32_t i = 0; i < PRODUCERS; i++) {
        ex_event_t ev;
        seen += pr[i].events;
        while (gw_poll_event(gw, i, &ev))
            seen++;
    }
    assert(seen >= TOTAL); /* at least one ACK or REJECT per request */

    /* single-threaded replay of the journal through a fresh exchange */
    gw_fill_t *rtape = calloc(TAPE_CAP, sizeof(*rtape));
    assert(rtape != NULL);
    size_t nr = gw_replay(replay.ex, journal, nj, rtape, TAPE_CAP);
    assert(nr == nt);
    assert(memcmp(rtape, tape, nt * sizeof(*tape)) == 0);
    for (int a = 0; a < H_ACCOUNTS; a++) {
        int64_t c1, r1, c2, r2;
        assert(ex_get_balance(live.ex, (uint32_t)a, &c1, &r1) == 0);
        assert(ex_get_balance(replay.ex, (uint32_t)a, &c2, &r2) == 0);
        assert(c1 == c2 && r1 == r2);
    }

    /* the check can fail: nudge the timestamp of the request behind the
     * first fill and the replayed tape must differ */
    ex_request_t *tampered = malloc(nj * sizeof(*tampered));
    harness_t again;
    assert(tampered != NULL);
    memcpy(tampered, journal, nj * sizeof(*tampered));
    tampered[tape[0].seq - 1].ts_ns += 1;
    h_setup(&again);
    nr = gw_replay(again.ex, tampered, nj, rtape, TAPE_CAP);
    assert(nr != nt || memcmp(rtape, tape, nt * sizeof(*tape)) != 0);

    printf("determinism: %zu requests, %zu fills, producers hit a full ring %lu times\n", nj, nt,
           pr[0].busy + pr[1].busy + pr[2].busy + pr[3].busy);

    free(tampered);
    free(rtape);
    gw_free(gw);
    ex_free(again.ex);
    ex_free(replay.ex);
    ex_free(live.ex);
}

int main(void)
{
    test_ring_full_empty_and_wraparound();
    test_ring_two_threads_in_order();
    test_determinism();

    printf("all gateway tests passed\n");
    return 0;
}
