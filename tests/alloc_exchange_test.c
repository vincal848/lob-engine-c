/* alloc_exchange_test -- M5a: ex_submit never touches the allocator.
 *
 * Same technique as alloc_test.c (GNU ld --wrap on malloc/calloc/
 * realloc/free). The exchange and its books are built with the
 * counters armed, which proves the wrap is live; then 1M random
 * requests (all kinds, rejects included) run with the counters armed
 * and any allocator call fails the test. GNU-ld only, so it is part
 * of `make alloc-test`, not `make test`.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "ex_random.h"

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

static harness_t h; /* static, so the harness itself allocates nothing */

int main(void)
{
    enum { REQUESTS = 1000000 };

    armed = 1;
    h_setup(&h);
    armed = 0;
    assert(calls > 0 && "allocator wrap not active: link with -Wl,--wrap=...");
    calls = 0;

    srand(7);
    armed = 1;
    for (uint64_t seq = 1; seq <= REQUESTS; seq++) {
        ex_request_t req = h_gen(&h, seq);
        h_submit(&h, &req);
    }
    armed = 0;

    if (calls != 0) {
        fprintf(stderr, "alloc_exchange_test: %lu allocator calls inside ex_submit\n", calls);
        return 1;
    }
    assert(h.fill > 1000); /* the run really matched orders */
    ex_free(h.ex);
    printf("exchange alloc test passed: 0 allocator calls in %d requests (%lu fills)\n",
           REQUESTS, h.fill);
    return 0;
}
