/* exchange_bench -- M5c: end-to-end latency and throughput of the
 * gateway + exchange (src/gateway.c on src/exchange.c).
 *
 * N producer threads each drive their own accounts with a seeded random
 * flow (limit, IOC, market, cancel) across M symbols and K accounts.
 * Each producer keeps up to `window` requests outstanding and timestamps
 * every request just before gw_submit() and again when its ACK or REJECT
 * comes back on its outbound ring, so a sample is submit -> ring ->
 * sequencer -> ex_submit -> ring -> producer. Printed, not gated:
 * timings are machine-dependent (CI only builds this and runs a short
 * smoke pass). Accounts are rich enough that pre-trade risk never
 * rejects; rejects that do happen are capacity or stale-cancel ones and
 * are counted.
 */
#define _POSIX_C_SOURCE 200809L /* clock_gettime, sched_yield under -std=c11 */

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/gateway.h"
#include "../tests/ex_random.h"

#define MAX_PRODUCERS 16
#define MAX_WINDOW 256
#define MAX_LIVE 256
#define MID 100 /* every symbol's window is [0, 199] */

typedef struct {
    unsigned producers, symbols, accounts, requests, window, market_pct;
    uint64_t seed;
} opts_t;

typedef struct {
    uint64_t id;
    uint32_t account;
} live_t; /* an open limit order of ours */

typedef struct {
    unsigned id;
    uint64_t rng;
    uint64_t *lat;          /* ns, one per request, in completion order */
    int64_t t0, t1;         /* first submit, last completion */
    unsigned long acks, rejects;
} producer_t;

static opts_t o = {2, 8, 64, 100000, 8, 10, 1};
static gw_t *gw;

static int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* Nearest-rank percentile of a sorted array (same rule as tools/lobster_replay.c). */
static uint64_t pct(const uint64_t *sorted, size_t n, double p)
{
    size_t k = (size_t)(p / 100.0 * (double)n);
    return sorted[k < n ? k : n - 1];
}

static ex_request_t gen(producer_t *p, const live_t *live, unsigned nlive, unsigned *cancel_at)
{
    ex_request_t req;
    memset(&req, 0, sizeof(req));
    unsigned owned = (o.accounts - p->id + o.producers - 1) / o.producers;
    req.account = p->id + o.producers * (unsigned)(ex_splitmix(&p->rng) % owned);
    unsigned roll = (unsigned)(ex_splitmix(&p->rng) % 100);

    *cancel_at = nlive;
    if (roll < 10 && nlive > 0) {
        *cancel_at = (unsigned)(ex_splitmix(&p->rng) % nlive);
        req.kind = EX_REQ_CANCEL;
        req.order_id = live[*cancel_at].id;
        req.account = live[*cancel_at].account;
        return req;
    }
    req.kind = EX_REQ_NEW;
    req.symbol = (uint32_t)(ex_splitmix(&p->rng) % o.symbols);
    req.side = ex_splitmix(&p->rng) % 2 ? LOB_SIDE_BUY : LOB_SIDE_SELL;
    req.type = roll < 10 + o.market_pct ? EX_MARKET : roll < 20 + o.market_pct ? EX_IOC : EX_LIMIT;
    req.price_ticks = MID - 5 + (int64_t)(ex_splitmix(&p->rng) % 11);
    req.qty = 1 + (uint32_t)(ex_splitmix(&p->rng) % 10);
    return req;
}

static void *producer(void *arg)
{
    producer_t *p = arg;
    int64_t t_sent[MAX_WINDOW];
    int is_limit[MAX_WINDOW];
    live_t live[MAX_LIVE];
    unsigned nlive = 0, sent = 0, done = 0;

    while (done < o.requests) {
        int idle = 1;
        ex_event_t ev;
        while (gw_poll_event(gw, p->id, &ev)) {
            idle = 0;
            if (ev.kind == EX_EV_DONE) {
                for (unsigned i = 0; i < nlive; i++)
                    if (live[i].id == ev.order_id) {
                        live[i] = live[--nlive];
                        break;
                    }
            } else if (ev.kind == EX_EV_ACK || ev.kind == EX_EV_REJECT) {
                /* only our own requests are ACKed/REJECTed on our ring, in submit order */
                int64_t t = now_ns();
                unsigned slot = done % o.window;
                p->lat[done] = (uint64_t)(t - t_sent[slot]);
                p->t1 = t;
                if (ev.kind == EX_EV_REJECT)
                    p->rejects++;
                else if (is_limit[slot] && nlive < MAX_LIVE)
                    live[nlive++] = (live_t){ev.order_id, ev.account};
                p->acks += ev.kind == EX_EV_ACK;
                done++;
            }
        }
        while (sent < o.requests && sent - done < o.window) {
            idle = 0;
            unsigned cancel_at;
            ex_request_t req = gen(p, live, nlive, &cancel_at);
            if (cancel_at < nlive)
                live[cancel_at] = live[--nlive]; /* don't cancel it twice */
            unsigned slot = sent % o.window;
            is_limit[slot] = req.kind == EX_REQ_NEW && req.type == EX_LIMIT;
            t_sent[slot] = now_ns();
            if (sent == 0)
                p->t0 = t_sent[slot];
            /* in_capacity > window, so the ring cannot be full */
            if (gw_submit(gw, p->id, &req) != 0) {
                fprintf(stderr, "exchange_bench: inbound ring unexpectedly full\n");
                exit(1);
            }
            sent++;
        }
        if (idle)
            sched_yield();
    }
    return NULL;
}

static unsigned arg_uint(const char *s, unsigned lo, unsigned hi, const char *name)
{
    char *end;
    unsigned long v = strtoul(s, &end, 10);
    if (*s == '\0' || *end != '\0' || v < lo || v > hi) {
        fprintf(stderr, "exchange_bench: %s must be %u..%u\n", name, lo, hi);
        exit(2);
    }
    return (unsigned)v;
}

static void usage(void)
{
    fprintf(stderr,
            "usage: exchange_bench [-p producers] [-s symbols] [-a accounts] "
            "[-n requests/producer] [-w window] [-m market%%] [-r seed]\n"
            "  accounts must be >= producers\n");
    exit(2);
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i += 2) {
        if (i + 1 >= argc)
            usage();
        if (strcmp(argv[i], "-p") == 0)
            o.producers = arg_uint(argv[i + 1], 1, MAX_PRODUCERS, "-p producers");
        else if (strcmp(argv[i], "-s") == 0)
            o.symbols = arg_uint(argv[i + 1], 1, 64, "-s symbols");
        else if (strcmp(argv[i], "-a") == 0)
            o.accounts = arg_uint(argv[i + 1], 1, 100000, "-a accounts");
        else if (strcmp(argv[i], "-n") == 0)
            o.requests = arg_uint(argv[i + 1], 100, 2000000, "-n requests per producer");
        else if (strcmp(argv[i], "-w") == 0)
            o.window = arg_uint(argv[i + 1], 1, MAX_WINDOW, "-w window");
        else if (strcmp(argv[i], "-m") == 0)
            o.market_pct = arg_uint(argv[i + 1], 0, 60, "-m market percent");
        else if (strcmp(argv[i], "-r") == 0)
            o.seed = arg_uint(argv[i + 1], 0, 1000000, "-r seed");
        else
            usage();
    }
    if (o.accounts < o.producers)
        usage();

    /* Accounts hold enough cash and shares that risk never rejects. */
    ex_config_t cfg = {.max_symbols = o.symbols, .max_accounts = o.accounts,
                       .max_orders = 1u << 16, .book_orders = 1u << 14, .max_window_ticks = 256};
    ex_t *ex = ex_new(&cfg);
    int64_t pos[64];
    if (ex == NULL)
        return 1;
    for (unsigned s = 0; s < o.symbols; s++) {
        char name[EX_NAME_MAX];
        snprintf(name, sizeof(name), "S%03u", s);
        pos[s] = 10000000;
        if (ex_add_symbol(ex, name, 0, 199) != (int)s)
            return 1;
    }
    for (unsigned a = 0; a < o.accounts; a++)
        if (ex_add_account(ex, 1000000000000LL, pos) != (int)a)
            return 1;

    size_t total = (size_t)o.producers * o.requests;
    gw_config_t gcfg = {.producers = o.producers, .in_capacity = 1024, .out_capacity = 1u << 16,
                        .max_requests = total + 1, .max_fills = 2 * total};
    gw = gw_new(ex, &gcfg);
    if (gw == NULL)
        return 1;

    pthread_t th[MAX_PRODUCERS];
    producer_t pr[MAX_PRODUCERS];
    uint64_t *all = malloc(total * sizeof(*all));
    if (all == NULL)
        return 1;
    for (unsigned i = 0; i < o.producers; i++) {
        memset(&pr[i], 0, sizeof(pr[i]));
        pr[i].id = i;
        pr[i].rng = o.seed * 1000 + i;
        pr[i].lat = all + (size_t)i * o.requests;
        if (pthread_create(&th[i], NULL, producer, &pr[i]) != 0)
            return 1;
    }
    for (unsigned i = 0; i < o.producers; i++)
        pthread_join(th[i], NULL);
    gw_stop(gw);

    /* Throughput: first submit of any producer to the last completion. */
    int64_t t0 = pr[0].t0, t1 = pr[0].t1;
    unsigned long acks = 0, rejects = 0;
    for (unsigned i = 0; i < o.producers; i++) {
        t0 = pr[i].t0 < t0 ? pr[i].t0 : t0;
        t1 = pr[i].t1 > t1 ? pr[i].t1 : t1;
        acks += pr[i].acks;
        rejects += pr[i].rejects;
    }
    double secs = (double)(t1 - t0) / 1e9;

    /* Latency: drop each producer's first 5% as warm-up, then pool. */
    size_t n = 0, warm = o.requests / 20;
    for (unsigned i = 0; i < o.producers; i++)
        for (size_t k = warm; k < o.requests; k++)
            all[n++] = pr[i].lat[k];
    qsort(all, n, sizeof(*all), cmp_u64);

    size_t nfills;
    (void)gw_tape(gw, &nfills);
    gw_stats_t st = gw_stats(gw);
    printf("exchange_bench: producers=%u symbols=%u accounts=%u window=%u market=%u%% "
           "requests=%zu seed=%llu\n",
           o.producers, o.symbols, o.accounts, o.window, o.market_pct, total,
           (unsigned long long)o.seed);
    printf("  throughput: %.0f requests/s (%.3f s)\n", (double)total / secs, secs);
    printf("  submit->ACK ns (%zu samples): p50 %llu  p90 %llu  p99 %llu  p99.9 %llu  max %llu\n", n,
           (unsigned long long)pct(all, n, 50), (unsigned long long)pct(all, n, 90),
           (unsigned long long)pct(all, n, 99), (unsigned long long)pct(all, n, 99.9),
           (unsigned long long)all[n - 1]);
    printf("  acks %lu  rejects %lu  fills %zu  dropped events %llu  dropped fills %llu\n", acks,
           rejects, nfills, (unsigned long long)st.events_dropped,
           (unsigned long long)st.fills_dropped);

    free(all);
    gw_free(gw);
    ex_free(ex);
    return 0;
}
