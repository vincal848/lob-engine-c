/* lobster_replay -- M2: replay a LOBSTER message file through the book
 * and check it against LOBSTER's paired orderbook file, row by row.
 *
 *   lobster_replay [-q] [-p max_orders] MESSAGE.csv ORDERBOOK.csv
 *
 * Exit status: 0 if every row matched, 1 if any row mismatched or a
 * message referred to shares the book didn't have, 2 on bad input.
 *
 * A level-N LOBSTER file only contains events inside the N visible
 * price levels, so two kinds of liquidity are invisible to the
 * message file: orders resting before the file starts, and orders
 * that are submitted or cancelled while deeper than level N. Both
 * show up in the orderbook file without a message. The replay holds
 * that liquidity as synthetic orders (ids with SYNTHETIC_BIT set) and
 * syncs it from the snapshot in exactly two situations:
 *
 *   - Row 1: the book is seeded from the first snapshot with the
 *     first message's effect undone.
 *   - A level that was just *revealed*: the previous snapshot had all
 *     N levels on that side occupied, and this level is strictly
 *     worse than the previous snapshot's worst level. Nothing about
 *     such a level was observable before this row.
 *
 * Every other visible level must match the snapshot exactly, or the
 * row counts as a mismatch. Messages that name an order id the book
 * has never seen are applied against that level's synthetic orders.
 * Everything the replay had to infer is counted and printed, so a
 * clean run shows how much it leaned on the snapshot, not just that
 * it passed. tools/README.md has the longer version.
 */
/* clock_gettime/CLOCK_MONOTONIC are POSIX, not C11; -std=c11 hides
 * them from time.h unless this is defined first.
 */
#define _POSIX_C_SOURCE 199309L

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/lob.h"

/* LOBSTER prices are dollars x 10000; the book runs on cent ticks.
 * Only hidden executions (type 5, which never touch the visible book)
 * carry sub-penny prices, so types 1-4 must divide evenly.
 */
#define LOBSTER_UNITS_PER_TICK 100
#define LOBSTER_DUMMY_PRICE INT64_C(9999999999)
#define SYNTHETIC_BIT (UINT64_C(1) << 62)
#define MAX_LEVELS 64
#define MAX_LINE 65536
#define DEPTH_SLACK 16
#define MAX_REPORTED_MISMATCHES 10

typedef struct {
    int64_t price_ticks;
    uint64_t qty;
} level_t;

typedef struct {
    level_t levels[MAX_LEVELS];
    size_t count;
} side_snapshot_t;

typedef struct {
    int type;
    uint64_t order_id;
    uint32_t size;
    int64_t price;
    int direction;
} message_t;

typedef struct {
    uint64_t rows;
    uint64_t by_type[8];
    uint64_t mismatch_rows;
    uint64_t unknown_id_messages;
    uint64_t spill_messages;      /* a known order had fewer shares than the message removed */
    uint64_t delete_remainders;   /* a type-3 delete left shares on the order, moved to synthetic */
    uint64_t price_disagreements; /* a known order's price differed from the message's */
    uint64_t revealed_adjustments;
    uint64_t revealed_shares_in;
    uint64_t revealed_shares_out;
    uint64_t lost_shares;         /* shares a message removed that the level didn't have */
    uint64_t synthetic_orders;
    uint64_t live_orders;
    uint64_t peak_live_orders;
} stats_t;

typedef struct {
    lob_t *book;
    uint64_t *ids;      /* scratch for lob_level_orders, sized to the pool */
    size_t ids_cap;
    uint64_t next_synthetic;
    stats_t st;
    lob_level_snapshot_t *depth;
    size_t depth_cap;
} replay_t;

static double now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static void die(const char *what, const char *detail)
{
    fprintf(stderr, "lobster_replay: %s%s%s\n", what, detail ? ": " : "", detail ? detail : "");
    exit(2);
}

/* Splits a CSV line in place into up to max_fields integer fields.
 * Field 0 of a message line is a decimal timestamp and is skipped by
 * the caller. Returns the field count, or -1 on a malformed number.
 */
static int parse_int_fields(char *line, int64_t *out, int max_fields, int skip_first)
{
    int n = 0;
    char *p = line;

    if (skip_first) {
        p = strchr(p, ',');
        if (!p)
            return -1;
        p++;
    }

    while (*p && *p != '\n' && *p != '\r') {
        if (n == max_fields)
            return -1;
        char *end;
        errno = 0;
        long long v = strtoll(p, &end, 10);
        if (end == p || errno)
            return -1;
        out[n++] = (int64_t)v;
        p = end;
        if (*p == ',')
            p++;
        else if (*p && *p != '\n' && *p != '\r')
            return -1;
    }
    return n;
}

static int read_message(FILE *f, char *line, message_t *m)
{
    int64_t v[5];
    if (!fgets(line, MAX_LINE, f))
        return 0;
    if (parse_int_fields(line, v, 5, 1) != 5)
        die("malformed message line", line);
    m->type = (int)v[0];
    m->order_id = (uint64_t)v[1];
    m->size = (uint32_t)v[2];
    m->price = v[3];
    m->direction = (int)v[4];
    if (m->type < 1 || m->type > 7 || v[2] < 0 || v[2] > UINT32_MAX)
        die("bad message type or size", line);
    return 1;
}

/* Reads one orderbook row into per-side snapshots (index by
 * lob_side_t). Returns the number of levels in the row, 0 at EOF.
 */
static size_t read_snapshot(FILE *f, char *line, side_snapshot_t snap[2])
{
    int64_t v[4 * MAX_LEVELS];
    if (!fgets(line, MAX_LINE, f))
        return 0;
    int n = parse_int_fields(line, v, 4 * MAX_LEVELS, 0);
    if (n <= 0 || n % 4 != 0)
        die("malformed orderbook line", line);

    size_t levels = (size_t)n / 4;
    snap[LOB_SIDE_BUY].count = 0;
    snap[LOB_SIDE_SELL].count = 0;
    for (size_t k = 0; k < levels; k++) {
        int64_t ask_px = v[4 * k], ask_q = v[4 * k + 1];
        int64_t bid_px = v[4 * k + 2], bid_q = v[4 * k + 3];
        if (ask_px != LOBSTER_DUMMY_PRICE) {
            if (ask_px % LOBSTER_UNITS_PER_TICK || ask_q <= 0)
                die("bad ask level in orderbook line", line);
            side_snapshot_t *s = &snap[LOB_SIDE_SELL];
            s->levels[s->count].price_ticks = ask_px / LOBSTER_UNITS_PER_TICK;
            s->levels[s->count].qty = (uint64_t)ask_q;
            s->count++;
        }
        if (bid_px != -LOBSTER_DUMMY_PRICE) {
            if (bid_px % LOBSTER_UNITS_PER_TICK || bid_q <= 0)
                die("bad bid level in orderbook line", line);
            side_snapshot_t *s = &snap[LOB_SIDE_BUY];
            s->levels[s->count].price_ticks = bid_px / LOBSTER_UNITS_PER_TICK;
            s->levels[s->count].qty = (uint64_t)bid_q;
            s->count++;
        }
    }
    return levels;
}

/* "a is a better price than b" on side. */
static int better(lob_side_t side, int64_t a, int64_t b)
{
    return side == LOB_SIDE_BUY ? a > b : a < b;
}

static lob_side_t side_of(const message_t *m)
{
    return m->direction == 1 ? LOB_SIDE_BUY : LOB_SIDE_SELL;
}

static void add_order(replay_t *r, uint64_t id, lob_side_t side, int64_t price, uint32_t qty)
{
    uint32_t filled = 0;
    lob_status_t s = lob_add_limit(r->book, id, side, price, qty, &filled);
    if (s == LOB_ERR_POOL_EXHAUSTED)
        die("order pool exhausted (raise -p)", NULL);
    if (s != LOB_OK)
        die("lob_add_limit failed", NULL);
    /* A visible LOBSTER book never crosses; a fill here means the
     * replay's book has diverged in a way the row check will report.
     */
    r->st.live_orders++;
    if (filled == qty)
        r->st.live_orders--;
    if (r->st.live_orders > r->st.peak_live_orders)
        r->st.peak_live_orders = r->st.live_orders;
}

static void add_synthetic(replay_t *r, lob_side_t side, int64_t price, uint64_t qty)
{
    while (qty > 0) {
        uint32_t chunk = qty > UINT32_MAX ? UINT32_MAX : (uint32_t)qty;
        add_order(r, SYNTHETIC_BIT | ++r->next_synthetic, side, price, chunk);
        r->st.synthetic_orders++;
        qty -= chunk;
    }
}

/* Removes up to want shares from order id; returns how many it took. */
static uint32_t take_from(replay_t *r, uint64_t id, uint32_t want)
{
    uint32_t have;
    if (lob_get_order(r->book, id, NULL, NULL, &have) != LOB_OK)
        return 0;
    if (want >= have) {
        lob_cancel(r->book, id);
        r->st.live_orders--;
        return have;
    }
    lob_reduce(r->book, id, have - want);
    return want;
}

/* Removes qty shares from the level at (side, price). Takes from
 * prefer first (the order the message named, if the book has it),
 * then synthetic orders newest-first, then any order newest-first.
 * Returns the shares taken from orders other than prefer; anything
 * the level couldn't supply is added to lost_shares.
 */
static uint64_t remove_from_level(replay_t *r, lob_side_t side, int64_t price, uint64_t qty,
                                  uint64_t prefer, int have_prefer)
{
    uint64_t spilled = 0;

    if (have_prefer && qty > 0)
        qty -= take_from(r, prefer, qty > UINT32_MAX ? UINT32_MAX : (uint32_t)qty);

    for (int pass = 0; pass < 2 && qty > 0; pass++) {
        size_t n = lob_level_orders(r->book, side, price, r->ids, r->ids_cap);
        if (n > r->ids_cap)
            n = r->ids_cap;
        for (size_t i = n; i-- > 0 && qty > 0;) {
            int synthetic = (r->ids[i] & SYNTHETIC_BIT) != 0;
            if (pass == 0 && !synthetic)
                continue;
            uint32_t got = take_from(r, r->ids[i], qty > UINT32_MAX ? UINT32_MAX : (uint32_t)qty);
            qty -= got;
            spilled += got;
        }
    }

    r->st.lost_shares += qty;
    return spilled;
}

static void apply_message(replay_t *r, const message_t *m)
{
    lob_side_t side = side_of(m);
    int64_t price = m->price / LOBSTER_UNITS_PER_TICK;

    if (m->type == 1) {
        add_order(r, m->order_id, side, price, m->size);
        return;
    }

    lob_side_t order_side;
    int64_t order_price;
    uint32_t order_qty;
    int known = lob_get_order(r->book, m->order_id, &order_side, &order_price, &order_qty) == LOB_OK;
    if (!known)
        r->st.unknown_id_messages++;
    else if (order_side != side || order_price != price)
        r->st.price_disagreements++;

    if (known && (order_side != side || order_price != price)) {
        /* Trust the message's level; the order itself is wrong. */
        known = 0;
    }

    if (remove_from_level(r, side, price, m->size, m->order_id, known) > 0 && known)
        r->st.spill_messages++;

    /* A delete removes the whole order. If the book had more on it
     * than the message's size, the extra was hidden liquidity that
     * belongs to someone else at this level -- keep it, anonymously.
     */
    if (m->type == 3 && known &&
        lob_get_order(r->book, m->order_id, NULL, NULL, &order_qty) == LOB_OK) {
        lob_cancel(r->book, m->order_id);
        r->st.live_orders--;
        add_synthetic(r, side, price, order_qty);
        r->st.delete_remainders++;
    }
}

/* Seeds the book from the first snapshot with the first message's
 * effect undone, as synthetic orders.
 */
static void seed(replay_t *r, side_snapshot_t snap[2], const message_t *m)
{
    for (int s = 0; s < 2; s++) {
        lob_side_t side = (lob_side_t)s;
        for (size_t k = 0; k < snap[s].count; k++) {
            int64_t px = snap[s].levels[k].price_ticks;
            int64_t q = (int64_t)snap[s].levels[k].qty;
            if (m->type >= 1 && m->type <= 4 && side_of(m) == side &&
                m->price / LOBSTER_UNITS_PER_TICK == px)
                q += m->type == 1 ? -(int64_t)m->size : (int64_t)m->size;
            if (q > 0)
                add_synthetic(r, side, px, (uint64_t)q);
        }
        /* A removal at a price the snapshot no longer shows: the
         * level existed before the message and the message emptied it.
         */
        if (m->type >= 2 && m->type <= 4 && side_of(m) == side) {
            int64_t px = m->price / LOBSTER_UNITS_PER_TICK;
            int present = 0;
            for (size_t k = 0; k < snap[s].count; k++)
                present |= snap[s].levels[k].price_ticks == px;
            if (!present)
                add_synthetic(r, side, px, m->size);
        }
    }
}

typedef struct {
    int full;            /* all N levels occupied */
    int64_t worst_price; /* valid if count > 0 */
    size_t count;
} side_view_t;

/* Compares one side of the book with the snapshot, syncing revealed
 * levels and resyncing mismatched ones so one bad row doesn't cascade.
 * Returns 1 if a non-revealed level disagreed.
 */
static int check_side(replay_t *r, lob_side_t side, const side_snapshot_t *snap, size_t levels,
                      const side_view_t *prev, int reveal_all, uint64_t row, const message_t *m)
{
    int full = snap->count == levels;
    size_t want = full ? levels + DEPTH_SLACK : r->depth_cap;
    if (want > r->depth_cap)
        want = r->depth_cap;
    size_t n = lob_depth(r->book, side, r->depth, want);
    if (full && n == want && !better(side, snap->levels[snap->count - 1].price_ticks,
                                     r->depth[n - 1].price_ticks))
        n = lob_depth(r->book, side, r->depth, r->depth_cap);

    int mismatch = 0;
    size_t i = 0, j = 0;
    while (i < n || j < snap->count) {
        int64_t px;
        uint64_t ours = 0, theirs = 0;
        if (j >= snap->count ||
            (i < n && better(side, r->depth[i].price_ticks, snap->levels[j].price_ticks))) {
            px = r->depth[i].price_ticks;
            ours = r->depth[i++].qty;
        } else if (i >= n || better(side, snap->levels[j].price_ticks, r->depth[i].price_ticks)) {
            px = snap->levels[j].price_ticks;
            theirs = snap->levels[j++].qty;
        } else {
            px = r->depth[i].price_ticks;
            ours = r->depth[i++].qty;
            theirs = snap->levels[j++].qty;
        }

        /* Below the visible window: nothing to compare against. */
        if (full && better(side, snap->levels[snap->count - 1].price_ticks, px))
            break;
        if (ours == theirs)
            continue;

        int revealed = reveal_all ||
                       (prev->full && prev->count > 0 && better(side, prev->worst_price, px));
        if (revealed) {
            r->st.revealed_adjustments++;
            if (theirs > ours)
                r->st.revealed_shares_in += theirs - ours;
            else
                r->st.revealed_shares_out += ours - theirs;
        } else {
            mismatch = 1;
            if (r->st.mismatch_rows < MAX_REPORTED_MISMATCHES)
                fprintf(stderr,
                        "row %" PRIu64 " (type %d, id %" PRIu64 ", size %" PRIu32
                        ", price %" PRId64 "): %s %" PRId64 " book %" PRIu64
                        " snapshot %" PRIu64 "\n",
                        row, m->type, m->order_id, m->size, m->price,
                        side == LOB_SIDE_BUY ? "bid" : "ask", px, ours, theirs);
        }

        if (theirs > ours)
            add_synthetic(r, side, px, theirs - ours);
        else
            remove_from_level(r, side, px, ours - theirs, 0, 0);
    }
    return mismatch;
}

static void usage(void)
{
    fprintf(stderr, "usage: lobster_replay [-q] [-p max_orders] MESSAGE.csv ORDERBOOK.csv\n");
    exit(2);
}

int main(int argc, char **argv)
{
    int quiet = 0;
    unsigned long max_orders = 1UL << 20;
    int argi = 1;

    for (; argi < argc && argv[argi][0] == '-'; argi++) {
        if (strcmp(argv[argi], "-q") == 0) {
            quiet = 1;
        } else if (strcmp(argv[argi], "-p") == 0 && argi + 1 < argc) {
            max_orders = strtoul(argv[++argi], NULL, 10);
            if (max_orders == 0 || max_orders > UINT32_MAX - 1)
                usage();
        } else {
            usage();
        }
    }
    if (argc - argi != 2)
        usage();

    FILE *mf = fopen(argv[argi], "r");
    FILE *of = fopen(argv[argi + 1], "r");
    if (!mf)
        die("cannot open", argv[argi]);
    if (!of)
        die("cannot open", argv[argi + 1]);

    char *line = malloc(MAX_LINE);
    side_snapshot_t snap[2];
    message_t m;
    if (!line)
        die("out of memory", NULL);

    /* Pass 1: the price window, from every price either file mentions. */
    int64_t lo = INT64_MAX, hi = INT64_MIN;
    size_t levels = 0;
    while (read_message(mf, line, &m)) {
        if (m.type >= 1 && m.type <= 4) {
            if (m.price % LOBSTER_UNITS_PER_TICK)
                die("sub-penny price on a visible-book message", NULL);
            int64_t px = m.price / LOBSTER_UNITS_PER_TICK;
            lo = px < lo ? px : lo;
            hi = px > hi ? px : hi;
        }
    }
    size_t row_levels;
    while ((row_levels = read_snapshot(of, line, snap)) > 0) {
        if (levels == 0)
            levels = row_levels;
        else if (row_levels != levels)
            die("orderbook rows have different level counts", NULL);
        for (int s = 0; s < 2; s++)
            for (size_t k = 0; k < snap[s].count; k++) {
                int64_t px = snap[s].levels[k].price_ticks;
                lo = px < lo ? px : lo;
                hi = px > hi ? px : hi;
            }
    }
    if (levels == 0 || lo > hi)
        die("empty input", NULL);
    if (levels > MAX_LEVELS)
        die("too many levels per orderbook row", NULL);
    rewind(mf);
    rewind(of);

    replay_t r;
    memset(&r, 0, sizeof(r));
    r.book = lob_new((uint32_t)max_orders, lo, hi);
    r.ids_cap = max_orders;
    r.ids = malloc(r.ids_cap * sizeof(*r.ids));
    r.depth_cap = (size_t)(hi - lo) + 1;
    r.depth = malloc(r.depth_cap * sizeof(*r.depth));
    if (!r.book || !r.ids || !r.depth)
        die("out of memory", NULL);

    /* Pass 2: the replay. */
    side_view_t prev[2];
    memset(prev, 0, sizeof(prev));
    double t0 = now_ns();

    for (;;) {
        int have_m = read_message(mf, line, &m);
        size_t have_o = have_m ? read_snapshot(of, line, snap) : 0;
        if (!have_m)
            break;
        if (!have_o)
            die("orderbook file has fewer rows than message file", NULL);

        r.st.rows++;
        r.st.by_type[m.type]++;
        if (r.st.rows == 1)
            seed(&r, snap, &m);
        if (m.type <= 4)
            apply_message(&r, &m);

        int mismatch = 0;
        for (int s = 0; s < 2; s++)
            mismatch |= check_side(&r, (lob_side_t)s, &snap[s], levels, &prev[s],
                                   r.st.rows == 1, r.st.rows, &m);
        if (mismatch)
            r.st.mismatch_rows++;

        for (int s = 0; s < 2; s++) {
            prev[s].full = snap[s].count == levels;
            prev[s].count = snap[s].count;
            if (snap[s].count > 0)
                prev[s].worst_price = snap[s].levels[snap[s].count - 1].price_ticks;
        }
    }
    if (read_snapshot(of, line, snap) > 0)
        die("orderbook file has more rows than message file", NULL);

    double elapsed_ns = now_ns() - t0;
    const stats_t *st = &r.st;
    int ok = st->mismatch_rows == 0 && st->lost_shares == 0;

    if (!quiet || !ok) {
        printf("rows                  %" PRIu64 "  (%zu levels)\n", st->rows, levels);
        printf("by type 1..7          %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64
               " %" PRIu64 " %" PRIu64 "\n",
               st->by_type[1], st->by_type[2], st->by_type[3], st->by_type[4], st->by_type[5],
               st->by_type[6], st->by_type[7]);
        printf("mismatched rows       %" PRIu64 "\n", st->mismatch_rows);
        printf("lost shares           %" PRIu64 "\n", st->lost_shares);
        printf("unknown-id messages   %" PRIu64 "\n", st->unknown_id_messages);
        printf("spilled messages      %" PRIu64 "\n", st->spill_messages);
        printf("delete remainders     %" PRIu64 "\n", st->delete_remainders);
        printf("price disagreements   %" PRIu64 "\n", st->price_disagreements);
        printf("revealed adjustments  %" PRIu64 "  (+%" PRIu64 " / -%" PRIu64 " shares)\n",
               st->revealed_adjustments, st->revealed_shares_in, st->revealed_shares_out);
        printf("synthetic orders      %" PRIu64 "\n", st->synthetic_orders);
        printf("peak live orders      %" PRIu64 "  (pool %lu)\n", st->peak_live_orders, max_orders);
        printf("price window          [%" PRId64 ", %" PRId64 "] ticks\n", lo, hi);
        printf("elapsed               %.1f ms, %.1f ns/row (incl. parsing and checks)\n",
               elapsed_ns / 1e6, st->rows ? elapsed_ns / (double)st->rows : 0.0);
        printf("%s\n", ok ? "MATCH" : "MISMATCH");
    }

    lob_free(r.book);
    free(r.ids);
    free(r.depth);
    free(line);
    fclose(mf);
    fclose(of);
    return ok ? 0 : 1;
}
