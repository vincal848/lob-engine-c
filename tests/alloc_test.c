/* alloc_test -- M3: verify the "no malloc/free on the hot path" claim
 * instead of trusting the code review.
 *
 * Linked with -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free
 * (GNU ld), so every allocator call made by lob.o goes through the
 * counters below. The book is created, then 1M random add/cancel/
 * reduce/market/depth operations run with the counters armed; any
 * allocation in that window fails the test. GNU-ld only, so it is a
 * separate `make alloc-test` target rather than part of `make test`.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "../src/lob.h"

void *__real_malloc(size_t n);
void *__real_calloc(size_t n, size_t size);
void *__real_realloc(void *p, size_t n);
void __real_free(void *p);

static int armed;
static unsigned long calls;

void *__wrap_malloc(size_t n) { calls += armed; return __real_malloc(n); }
void *__wrap_calloc(size_t n, size_t size) { calls += armed; return __real_calloc(n, size); }
void *__wrap_realloc(void *p, size_t n) { calls += armed; return __real_realloc(p, n); }
void __wrap_free(void *p) { calls += armed; __real_free(p); }

static void noop_fill(const lob_fill_t *fill, void *user_data)
{
    (void)fill;
    (void)user_data;
}

int main(void)
{
    enum { MAX_ORDERS = 50000, MIN_TICK = 0, MAX_TICK = 9999, OPS = 1000000, RING_CAP = 50000 };

    /* The wrap itself must work: lob_new allocates, so it is counted. */
    armed = 1;
    lob_t *book = lob_new(MAX_ORDERS, MIN_TICK, MAX_TICK);
    armed = 0;
    assert(book);
    assert(calls > 0 && "allocator wrap not active: link with -Wl,--wrap=...");
    calls = 0;

    lob_set_fill_callback(book, noop_fill, NULL);
    static uint64_t ring[RING_CAP];
    size_t ring_head = 0, ring_count = 0;
    lob_level_snapshot_t depth[16];
    uint64_t next_id = 1;
    srand(7);

    armed = 1;
    for (int i = 0; i < OPS; i++) {
        int op = rand() % 5;
        lob_side_t side = rand() % 2 ? LOB_SIDE_BUY : LOB_SIDE_SELL;
        uint32_t qty = 1 + (uint32_t)(rand() % 20), filled;

        if (op == 0 || ring_count == 0) {
            uint64_t id = next_id++;
            int64_t price = MIN_TICK + rand() % (MAX_TICK - MIN_TICK + 1);
            if (lob_add_limit(book, id, side, price, qty, &filled) == LOB_OK && filled < qty) {
                ring[(ring_head + ring_count) % RING_CAP] = id;
                if (ring_count < RING_CAP)
                    ring_count++;
                else
                    ring_head = (ring_head + 1) % RING_CAP;
            }
        } else if (op == 1) {
            lob_cancel(book, ring[ring_head]);
            ring_head = (ring_head + 1) % RING_CAP;
            ring_count--;
        } else if (op == 2) {
            lob_reduce(book, ring[(ring_head + (size_t)rand() % ring_count) % RING_CAP], 1);
        } else if (op == 3) {
            lob_execute_market(book, side, qty, &filled);
        } else {
            lob_depth(book, side, depth, 16);
        }
    }
    armed = 0;

    if (calls != 0) {
        fprintf(stderr, "alloc_test: %lu allocator calls between lob_new and lob_free\n", calls);
        return 1;
    }
    lob_free(book);
    printf("alloc test passed: 0 allocator calls in %d operations\n", OPS);
    return 0;
}
