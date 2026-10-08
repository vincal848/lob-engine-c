"""Checks for the ctypes binding: `make python-test` (runs `python3 python/test_lob.py`).

Plain asserts, no test framework: each check exercises one path
through the binding (argument types, out-params, callbacks, errors)
against a behavior the C unit tests already pin down.
"""

import random

from lob import BUY, SELL, Book, Fill, LobError

# AAPL-sized prices in cent ticks, so a too-narrow ctypes type truncates them.
P = 58_000


def test_crossing_fills_in_price_time_order_and_rests_remainder():
    fills = []
    with Book(100, P - 10, P + 10) as b:
        b.on_fill(fills.append)
        b.add_limit(1, SELL, P + 1, 5)
        b.add_limit(2, SELL, P, 3)
        b.add_limit(3, SELL, P, 4)
        assert b.add_limit(10, BUY, P + 1, 10) == 10
        assert fills == [
            Fill(2, 10, P, 3, BUY),
            Fill(3, 10, P, 4, BUY),
            Fill(1, 10, P + 1, 3, BUY),
        ]
        assert b.best_ask() == (P + 1, 2)
        assert b.best_bid() is None


def test_reduce_cancel_depth_and_get_order():
    with Book(100, P - 10, P + 10) as b:
        b.add_limit(1, BUY, P - 1, 5)
        b.add_limit(2, BUY, P - 1, 7)
        b.add_limit(3, BUY, P - 3, 1)
        b.reduce(1, 2)
        assert b.get_order(1) == (BUY, P - 1, 2)
        assert b.depth(BUY) == [(P - 1, 9, 2), (P - 3, 1, 1)]
        b.cancel(2)
        assert b.get_order(2) is None
        assert b.execute_market(SELL, 10) == 3  # only 3 shares rest
        assert b.depth(BUY) == []


def test_errors_raise_instead_of_passing_silently():
    with Book(2, P - 10, P + 10) as b:
        for call, status in [
            (lambda: b.add_limit(1, BUY, P + 100, 1), "PRICE_OUT_OF_RANGE"),
            (lambda: b.cancel(42), "NOT_FOUND"),
            (lambda: b.add_limit(1, BUY, P, 0), "INVALID_QTY"),
        ]:
            try:
                call()
            except LobError as e:
                assert str(e) == status, e
            else:
                raise AssertionError(f"expected {status}")


def test_conservation_over_random_flow():
    """Shares in = shares resting + shares filled, through the binding."""
    rng = random.Random(3)
    filled = []
    with Book(5000, P, P + 200) as b:
        b.on_fill(lambda f: filled.append(f.qty))
        added = 0
        for i in range(1, 20001):
            qty = rng.randint(1, 20)
            added += qty
            b.add_limit(i, rng.choice((BUY, SELL)), P + rng.randint(0, 200), qty)
        resting = sum(lv.qty for s in (BUY, SELL) for lv in b.depth(s, 201))
        # each fill removes qty from both the resting and the aggressing order
        assert added == resting + 2 * sum(filled), (added, resting, sum(filled))


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            fn()
    print("python binding tests passed")
