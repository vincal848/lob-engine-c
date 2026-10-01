/* Rough throughput benchmark: replays random order flow through the
 * book and reports ns/op. Not run in CI as a gate (timings are
 * machine-dependent) -- just built there, so it doesn't bit-rot.
 *
 * Mirrors the operation mix of tests/unit_tests.c's differential
 * test (add/cancel/reduce/market) but without the naive reference,
 * since here we only care about the real engine's wall-clock cost.
 */
/* clock_gettime/CLOCK_MONOTONIC are POSIX, not C11; -std=c11 hides
 * them from time.h unless this is defined first.
 */
#define _POSIX_C_SOURCE 199309L

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "../src/lob.h"

static void noop_fill(const lob_fill_t *fill, void *user_data)
{
    (void)fill;
    (void)user_data;
}

static double now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

int main(void)
{
    enum { MAX_ORDERS = 50000, MIN_TICK = 0, MAX_TICK = 9999, OPS = 1000000 };

    lob_t *book = lob_new(MAX_ORDERS, MIN_TICK, MAX_TICK);
    if (!book) {
        fprintf(stderr, "lob_new failed\n");
        return 1;
    }
    lob_set_fill_callback(book, noop_fill, NULL);

    /* Resting order ids we can pick for cancel/reduce, tracked the
     * cheap way: a ring buffer of the ids that are plausibly still
     * resting. It can go stale (an id might already be filled), in
     * which case lob_cancel/lob_reduce just return LOB_ERR_NOT_FOUND,
     * which is fine for a throughput benchmark.
     */
    enum { RING_CAP = 50000 };
    uint64_t ring[RING_CAP];
    size_t ring_head = 0, ring_count = 0;

    srand(1);
    uint64_t next_id = 1;
    double start = now_ns();

    for (int i = 0; i < OPS; i++) {
        int op = rand() % 4;

        if (op == 0 || ring_count == 0) {
            int side_is_buy = rand() % 2;
            int64_t price = MIN_TICK + rand() % (MAX_TICK - MIN_TICK + 1);
            uint32_t qty = 1 + (uint32_t)(rand() % 20);
            uint64_t id = next_id++;
            uint32_t filled = 0;

            lob_status_t st = lob_add_limit(book, id, side_is_buy ? LOB_SIDE_BUY : LOB_SIDE_SELL,
                                             price, qty, &filled);
            if (st == LOB_OK && filled < qty) {
                ring[(ring_head + ring_count) % RING_CAP] = id;
                if (ring_count < RING_CAP)
                    ring_count++;
                else
                    ring_head = (ring_head + 1) % RING_CAP;
            }
        } else if (op == 1) {
            uint64_t id = ring[ring_head];
            ring_head = (ring_head + 1) % RING_CAP;
            ring_count--;
            lob_cancel(book, id);
        } else if (op == 2) {
            uint64_t id = ring[(ring_head + (size_t)rand() % ring_count) % RING_CAP];
            lob_reduce(book, id, 1);
        } else {
            int side_is_buy = rand() % 2;
            uint32_t qty = 1 + (uint32_t)(rand() % 20);
            uint32_t filled = 0;
            lob_execute_market(book, side_is_buy ? LOB_SIDE_BUY : LOB_SIDE_SELL, qty, &filled);
        }
    }

    double elapsed = now_ns() - start;
    printf("%d ops in %.3f ms -> %.1f ns/op\n", OPS, elapsed / 1e6, elapsed / OPS);

    lob_free(book);
    return 0;
}
