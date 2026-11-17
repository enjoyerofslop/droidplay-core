"""dm/android/wb_live.h, when a later draw may read or write a pending storage buffer copy in place (gfx_vk_draw.inc wl_*), and when it needs a full barrier first.
Tears of the Kingdom's draws that store computed values into a buffer the draw before wrote, and its shaders reading what order-free copies hold, each waited for the GPU.
Built with the system's C compiler and called through ctypes. Skipped without a C compiler."""
import ctypes
import os
from pathlib import Path
import random
import shutil
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
CC = (['xcrun', 'clang'] if sys.platform == 'darwin' and shutil.which('xcrun') else
      [c] if (c := shutil.which('clang') or shutil.which('cc') or shutil.which('gcc')) else None)
READ, PLAIN = 0, 0xffffffffffffffff
STORE1 = (1 << 32) | 1   # stores of 1 (OFW_STORE, constant 1): TotK's grid
UMIN = 3 << 32   # atomic unsigned min (OFW_UMIN)
G = 0x68d7901000   # TotK's 131,584-byte grid


class St(ctypes.Structure):
    _fields_ = [('rep', ctypes.c_uint32), ('rdid', ctypes.c_uint32), ('wep', ctypes.c_uint32), ('wdid', ctypes.c_uint32), ('wk', ctypes.c_uint64)]


@unittest.skipIf(CC is None or os.name == 'nt', 'no C compiler')
class WbLive(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        assert CC is not None   # (skipped without one; pyright doesn't follow skipIf)
        cls.tmp = Path(tempfile.mkdtemp())
        src = cls.tmp / 'l.c'
        src.write_text('#include "wb_live.h"\n#define X __attribute__((visibility("default")))\n'
                       'X int true_(int st, size_t rows, int inpl, int lin, int ds, int rbx, int vd, uint32_t ofw, int og, int snap) { return wl_true(st, rows, inpl, lin, ds, rbx, vd, ofw, og, snap); }\n'
                       'X int covers(uintptr_t lo, uintptr_t hi, uintptr_t dst, size_t n, uint64_t off, uint64_t al) { return wl_covers(lo, hi, dst, n, off, al); }\n'
                       'X int need(const wl_st *s, uint32_t ep, uint32_t did, uint64_t kind) { return wl_need(s, ep, did, kind); }\n'
                       'X void mark(wl_st *s, uint32_t ep, uint32_t did, uint64_t kind) { wl_mark(s, ep, did, kind); }\n'
                       'X uint64_t kind(uint32_t cls, uint32_t k) { return wl_ofw_kind(cls, k); }\n')
        subprocess.run([*CC, '-shared', '-fPIC', '-O1', '-Wall', '-Wextra', '-Werror', '-I', str(REPO / 'dm/android'), str(src), '-o', str(cls.tmp / 'l.so')], check=True)
        L = cls.lib = ctypes.CDLL(str(cls.tmp / 'l.so'))
        L.true_.argtypes = [ctypes.c_int, ctypes.c_size_t] + [ctypes.c_int] * 5 + [ctypes.c_uint32, ctypes.c_int, ctypes.c_int]
        L.covers.argtypes = [ctypes.c_size_t] * 4 + [ctypes.c_uint64] * 2
        L.need.argtypes = [ctypes.POINTER(St), ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint64]
        L.mark.argtypes = L.need.argtypes
        L.mark.restype = None
        L.kind.argtypes = [ctypes.c_uint32, ctypes.c_uint32]
        L.kind.restype = ctypes.c_uint64

    @classmethod
    def tearDownClass(cls): shutil.rmtree(cls.tmp, ignore_errors=True)

    def true_(self, st=-1, rows=1, inpl=0, lin=0, ds=0, rbx=0, vd=0, ofw=0, og=0, snap=1):
        return self.lib.true_(st, rows, inpl, lin, ds, rbx, vd, ofw, og, snap)

    def test_which_copies_hold_the_bytes(self):
        self.assertEqual(self.true_(), 1)   # a draw's or a dispatch's copy of guest memory
        self.assertEqual(self.true_(ofw=1, og=1), 1)   # an order-free copy that started from the bytes
        self.assertEqual(self.true_(ofw=1), 0)   # one that started from the identity holds only what was written
        for kw in ({'st': 0}, {'rows': 4}, {'inpl': 1}, {'lin': 1}, {'ds': 1}, {'rbx': 1}, {'vd': 1}, {'snap': 0}):
            self.assertEqual(self.true_(**kw), 0, kw)

    def test_covers(self):
        L = self.lib
        self.assertEqual(L.covers(G, G + 0x20200, G, 0x20200, 0x1000, 64), 1)
        self.assertEqual(L.covers(G + 0x1c0, G + 0x1c0 + 0x7f80, G, 0x20200, 0x1000, 64), 1)   # TotK's vertex shader reads a part
        self.assertEqual(L.covers(G + 0x1c0, G + 0x200, G, 0x20200, 0x1000, 256), 0)   # its offset into the copy breaks the binding's alignment
        self.assertEqual(L.covers(G - 4, G + 0x100, G, 0x20200, 0, 0), 0)
        self.assertEqual(L.covers(G, G + 0x20204, G, 0x20200, 0, 0), 0)
        self.assertEqual(L.covers(G, G, G, 0x20200, 0, 0), 0)

    def seq(self, steps):
        """steps: (draw, kind[, barrier before]) run in order from epoch 1; → the draws that needed a barrier"""
        s, ep, out = St(), 1, []
        for d, k in steps:
            if self.lib.need(ctypes.byref(s), ep, d, k):
                ep += 1
                out.append(d)
            self.lib.mark(ctypes.byref(s), ep, d, k)
        return out

    def test_reads_after_writes(self):
        self.assertEqual(self.seq([(1, PLAIN), (2, READ)]), [2])
        self.assertEqual(self.seq([(1, STORE1), (2, READ), (3, READ)]), [2])   # one barrier serves both readers
        self.assertEqual(self.seq([(1, READ), (2, READ)]), [])

    def test_writes_after_reads_and_writes(self):
        self.assertEqual(self.seq([(1, READ), (2, PLAIN)]), [2])
        self.assertEqual(self.seq([(1, READ), (2, STORE1)]), [2])
        self.assertEqual(self.seq([(1, PLAIN), (2, PLAIN)]), [2])   # TotK's fragment shaders storing computed values, one after another
        self.assertEqual(self.seq([(1, PLAIN), (2, STORE1)]), [2])
        self.assertEqual(self.seq([(1, STORE1), (2, UMIN)]), [2])
        self.assertEqual(self.seq([(1, STORE1), (2, self.lib.kind(1, 2))]), [2])   # another constant

    def test_order_free_writes_together(self):
        self.assertEqual(self.seq([(d, STORE1) for d in range(1, 600)]), [])   # the grid's marks from hundreds of draws: no barrier between them
        self.assertEqual(self.seq([(1, UMIN), (2, UMIN), (3, READ), (4, UMIN)]), [3, 4])

    def test_a_draws_own_accesses(self):
        self.assertEqual(self.seq([(1, STORE1), (1, READ), (1, PLAIN)]), [])
        self.assertEqual(self.seq([(1, READ), (2, READ), (2, PLAIN)]), [2])   # draw 1's read is still there under draw 2's own
        self.assertEqual(self.seq([(1, STORE1), (2, STORE1), (2, READ)]), [2])   # draw 1's write under draw 2's own

    def test_random_against_a_model(self):
        """the rule against a model that keeps every access of the epoch: a barrier exactly when an access conflicts with another draw's since the last one"""
        rnd = random.Random(504)
        kinds = [READ, PLAIN, STORE1, UMIN]
        for _ in range(300):
            steps = [(rnd.randint(1, 5), rnd.choice(kinds)) for _ in range(rnd.randint(1, 40))]
            # draws in order: repeat draws only as runs (a draw's accesses come together)
            steps.sort(key=lambda x: x[0]) if rnd.random() < 0.2 else None
            seen, want = [], []
            for d, k in steps:
                conflict = any(d2 != d and (k == PLAIN or k2 == PLAIN or (k == READ) != (k2 == READ) or (k != READ and k2 != READ and k != k2))
                               and not (k == READ and k2 == READ) for d2, k2 in seen)
                if conflict:
                    want.append(d)
                    seen = []
                seen.append((d, k))
            self.assertEqual(self.seq(steps), want, steps)


if __name__ == '__main__':
    unittest.main()
