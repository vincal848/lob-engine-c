"""lob -- M4: a ctypes binding to the C matching engine (src/lob.h).

Standard library only. Build the shared library first (`make lib`); the
module loads build/liblob.so, or whatever path LOB_LIB points to.

    from lob import Book, BUY, SELL
    book = Book(max_orders=1000, min_tick=9000, max_tick=11000)
    book.on_fill(lambda f: print(f))
    book.add_limit(1, SELL, 10_000, 5)
    filled = book.add_limit(2, BUY, 10_000, 3)   # crosses: 3 filled
    book.best_ask()                               # (10000, 2)

Error statuses from the C side are raised as LobError with the status
name, so a bad id or an out-of-window price can't pass silently.
"""

import ctypes as C
import os
from collections import namedtuple

BUY, SELL = 0, 1

_STATUS = [
    "OK",
    "POOL_EXHAUSTED",
    "DUPLICATE_ID",
    "NOT_FOUND",
    "PRICE_OUT_OF_RANGE",
    "INVALID_QTY",
]

Fill = namedtuple("Fill", "resting_order_id aggressor_order_id price_ticks qty aggressor_side")
Level = namedtuple("Level", "price_ticks qty order_count")


class LobError(Exception):
    pass


class _Fill(C.Structure):
    _fields_ = [
        ("resting_order_id", C.c_uint64),
        ("aggressor_order_id", C.c_uint64),
        ("price_ticks", C.c_int64),
        ("qty", C.c_uint32),
        ("aggressor_side", C.c_int),
    ]


class _Level(C.Structure):
    _fields_ = [("price_ticks", C.c_int64), ("qty", C.c_uint64), ("order_count", C.c_uint32)]


_FILL_FN = C.CFUNCTYPE(None, C.POINTER(_Fill), C.c_void_p)

_default = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "liblob.so")
_lib = C.CDLL(os.environ.get("LOB_LIB", _default))


def _sig(name, restype, *argtypes):
    f = getattr(_lib, name)
    f.restype, f.argtypes = restype, list(argtypes)


_P = C.c_void_p
_sig("lob_new", _P, C.c_uint32, C.c_int64, C.c_int64)
_sig("lob_free", None, _P)
_sig("lob_set_fill_callback", None, _P, _FILL_FN, C.c_void_p)
_sig("lob_add_limit", C.c_int, _P, C.c_uint64, C.c_int, C.c_int64, C.c_uint32, C.POINTER(C.c_uint32))
_sig("lob_cancel", C.c_int, _P, C.c_uint64)
_sig("lob_reduce", C.c_int, _P, C.c_uint64, C.c_uint32)
_sig("lob_execute_market", C.c_int, _P, C.c_int, C.c_uint32, C.POINTER(C.c_uint32))
_sig("lob_best_bid", C.c_int, _P, C.POINTER(C.c_int64), C.POINTER(C.c_uint64))
_sig("lob_best_ask", C.c_int, _P, C.POINTER(C.c_int64), C.POINTER(C.c_uint64))
_sig("lob_depth", C.c_size_t, _P, C.c_int, C.POINTER(_Level), C.c_size_t)
_sig(
    "lob_get_order",
    C.c_int,
    _P,
    C.c_uint64,
    C.POINTER(C.c_int),
    C.POINTER(C.c_int64),
    C.POINTER(C.c_uint32),
)


def _check(status):
    if status != 0:
        raise LobError(_STATUS[status] if status < len(_STATUS) else f"status {status}")


class Book:
    def __init__(self, max_orders, min_tick, max_tick):
        self._b = _lib.lob_new(max_orders, min_tick, max_tick)
        if not self._b:
            raise LobError("lob_new failed (bad arguments or out of memory)")
        self._cb = None  # keeps the ctypes callback alive while C holds it

    def close(self):
        if self._b:
            _lib.lob_free(self._b)
            self._b = None

    __del__ = close

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def on_fill(self, fn):
        """Call fn(Fill) once per resting order consumed; None disables."""
        if fn is None:
            self._cb = _FILL_FN()
        else:
            self._cb = _FILL_FN(lambda p, _ud: fn(Fill(*(getattr(p[0], n) for n in Fill._fields))))
        _lib.lob_set_fill_callback(self._b, self._cb, None)

    def add_limit(self, order_id, side, price_ticks, qty):
        """Crosses, then rests the remainder. Returns the quantity filled now."""
        filled = C.c_uint32()
        _check(_lib.lob_add_limit(self._b, order_id, side, price_ticks, qty, C.byref(filled)))
        return filled.value

    def cancel(self, order_id):
        _check(_lib.lob_cancel(self._b, order_id))

    def reduce(self, order_id, new_qty):
        _check(_lib.lob_reduce(self._b, order_id, new_qty))

    def execute_market(self, side, qty):
        """Returns the quantity filled; a market order never rests."""
        filled = C.c_uint32()
        _check(_lib.lob_execute_market(self._b, side, qty, C.byref(filled)))
        return filled.value

    def _best(self, fn):
        px, q = C.c_int64(), C.c_uint64()
        return (px.value, q.value) if fn(self._b, C.byref(px), C.byref(q)) else None

    def best_bid(self):
        """(price_ticks, qty) or None if there are no bids."""
        return self._best(_lib.lob_best_bid)

    def best_ask(self):
        """(price_ticks, qty) or None if there are no asks."""
        return self._best(_lib.lob_best_ask)

    def depth(self, side, max_levels=10):
        """Up to max_levels occupied levels on side, best first."""
        out = (_Level * max_levels)()
        n = _lib.lob_depth(self._b, side, out, max_levels)
        return [Level(lv.price_ticks, lv.qty, lv.order_count) for lv in out[:n]]

    def get_order(self, order_id):
        """(side, price_ticks, qty) of a resting order, or None."""
        side, px, q = C.c_int(), C.c_int64(), C.c_uint32()
        status = _lib.lob_get_order(self._b, order_id, C.byref(side), C.byref(px), C.byref(q))
        if status == _STATUS.index("NOT_FOUND"):
            return None
        _check(status)
        return side.value, px.value, q.value
