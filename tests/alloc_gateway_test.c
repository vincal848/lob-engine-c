/* alloc_gateway_test -- M5c: nothing between gw_new and gw_stop allocates.
 *
 * Same technique as alloc_exchange_test.c (GNU ld --wrap), extended to
 * the whole gateway hot path: two producer threads push 100k requests
 * through the rings, the sequencer journals them, ex_submit matches them,
 * and the tape and outbound rings fill. The counters are armed from the
 * moment gw_new returns until gw_stop has joined the sequencer; gw_new
 * itself is armed once first, which proves the wrap is live. Calls made
 * inside libc (pthread_create, thread stacks) are not wrapped, only calls
 * from the gateway, exchange, book and this file. GNU-ld only, so it is
 * part of `make alloc-test`, not `make test`.
 */
#define _POSIX_C_SOURCE 200809L /* sched_yield under -std=c11 */

#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include "../src/gateway.h"
#include "ex_random.h"

void *__real_malloc(size_t n);
void *__real_calloc(size_t n, size_t size);
void *__real_realloc(void *p, size_t n);
void __real_free(void *p);
void *__real_aligned_alloc(size_t align, size_t n);

static _Atomic int armed;
static _Atomic unsigned long calls;

static void count(void) { calls += atomic_load(&armed); }

void *__wrap_malloc(size_t n) { count(); return __real_malloc(n); }
void *__wrap_calloc(size_t n, size_t size) { count(); return __real_calloc(n, size); }
void *__wrap_realloc(void *p, size_t n) { count(); return __real_realloc(p, n); }
void __wrap_free(void *p) { count(); __real_free(p); }
void *__wrap_aligned_alloc(size_t align, size_t n) { count(); return __real_aligned_alloc(align, n); }

enum { PRODUCERS = 2, PER_PRODUCER = 50000 };

static harness_t h; /* static, so the harness itself allocates nothing */
static gw_t *gw;

static void *producer(void *arg)
{
    uint32_t id = (uint32_t)(uintptr_t)arg;
    uint64_t rng = 99 + id;
    for (int i = 0; i < PER_PRODUCER; i++) {
        ex_event_t ev;
        while (gw_poll_event(gw, id, &ev))
            ;
        ex_request_t req = {.kind = EX_REQ_NEW};
        req.account = id + PRODUCERS * (uint32_t)(ex_splitmix(&rng) % (H_ACCOUNTS / PRODUCERS));
        req.symbol = (uint32_t)(ex_splitmix(&rng) % H_SYMBOLS);
        req.side = ex_splitmix(&rng) % 2 ? LOB_SIDE_BUY : LOB_SIDE_SELL;
        req.type = (ex_order_type_t)(ex_splitmix(&rng) % 3);
        req.price_ticks = h.lo[req.symbol] + (int64_t)(ex_splitmix(&rng) % (uint64_t)(h.hi[req.symbol] - h.lo[req.symbol] + 1));
        req.qty = 1 + (uint32_t)(ex_splitmix(&rng) % 8);
        while (gw_submit(gw, id, &req) != 0)
            sched_yield();
    }
    return NULL;
}

int main(void)
{
    h_setup(&h);
    gw_config_t cfg = {.producers = PRODUCERS, .in_capacity = 1024, .out_capacity = 1 << 16,
                       .max_requests = PRODUCERS * PER_PRODUCER, .max_fills = 1 << 18};

    atomic_store(&armed, 1);
    gw = gw_new(h.ex, &cfg);
    atomic_store(&armed, 0);
    assert(gw != NULL);
    assert(calls > 0 && "allocator wrap not active: link with -Wl,--wrap=...");
    calls = 0;

    atomic_store(&armed, 1);
    pthread_t th[PRODUCERS];
    for (uintptr_t i = 0; i < PRODUCERS; i++)
        assert(pthread_create(&th[i], NULL, producer, (void *)i) == 0);
    for (int i = 0; i < PRODUCERS; i++)
        pthread_join(th[i], NULL);
    gw_stop(gw);
    atomic_store(&armed, 0);

    if (calls != 0) {
        fprintf(stderr, "alloc_gateway_test: %lu allocator calls between gw_new and gw_stop\n",
                (unsigned long)calls);
        return 1;
    }
    size_t nj, nt;
    (void)gw_journal(gw, &nj);
    (void)gw_tape(gw, &nt);
    assert(nj == PRODUCERS * PER_PRODUCER && nt > 100); /* the run really matched orders */
    gw_free(gw);
    ex_free(h.ex);
    printf("gateway alloc test passed: 0 allocator calls in %zu requests (%zu fills)\n", nj, nt);
    return 0;
}
