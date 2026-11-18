"""dm/gfx/nvn/range_index.h (the dirty tracker's ranges sorted by base, so a host write the remembered sets miss finds the ranges it meets without a walk over every
one: nvn_dirty.c prep_write), built for the PC with the system's clang and driven through ctypes against that walk. Needs no NDK."""
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

HARNESS = r'''
#include "range_index.h"
#define EXPORT __attribute__((visibility("default")))
static ri_t x;
EXPORT int build(int n, const uint64_t *lo, const uint64_t *end) {
    if (!ri_reserve(&x, n)) return 0;
    for (int i = 0; i < n; i++) ri_add(&x, (uintptr_t)lo[i], (uintptr_t)end[i], i);
    ri_sort(&x); return 1;
}
/* the walk prep_write makes through the index: the numbers of the ranges meeting [lo, hi) into out, how many */
EXPORT int meet(uint64_t lo, uint64_t hi, int *out) {
    int m = 0;
    for (int k = ri_first(&x, (uintptr_t)lo); k < x.n && x.e[k].lo < hi; k++) if (x.e[k].end > lo) out[m++] = x.e[k].idx;
    return m;
}
'''


def compiler():
    if os.name == 'nt': return None
    if sys.platform == 'darwin': return ['xcrun', 'clang']
    cc = shutil.which('clang') or shutil.which('cc')
    return [cc] if cc else None


@unittest.skipUnless(compiler(), 'no C compiler')
class RangeIndex(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp())
        src = cls.tmp / 'h.c'; src.write_text(HARNESS)
        subprocess.run([*compiler(), '-shared', '-fPIC', '-O1', '-Wall', '-Wextra', '-Werror', '-I', REPO / 'dm/gfx/nvn', src, '-o', cls.tmp / 'h.so'], check=True)
        cls.lib = ctypes.CDLL(str(cls.tmp / 'h.so'))
        cls.lib.build.argtypes = [ctypes.c_int, ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(ctypes.c_uint64)]
        cls.lib.meet.argtypes = [ctypes.c_uint64, ctypes.c_uint64, ctypes.POINTER(ctypes.c_int)]

    @classmethod
    def tearDownClass(cls): shutil.rmtree(cls.tmp, ignore_errors=True)

    def build(self, rs):
        lo = (ctypes.c_uint64 * max(len(rs), 1))(*[r[0] for r in rs]); end = (ctypes.c_uint64 * max(len(rs), 1))(*[r[1] for r in rs])
        self.assertEqual(self.lib.build(len(rs), lo, end), 1)
        self.out = (ctypes.c_int * max(len(rs), 1))()

    def meet(self, lo, hi): return sorted(self.out[i] for i in range(self.lib.meet(lo, hi, self.out)))

    @staticmethod
    def walk(rs, lo, hi): return [i for i, r in enumerate(rs) if r[0] < hi and r[1] > lo]

    def test_empty(self):
        self.build([]); self.assertEqual(self.meet(0x1000, 0x2000), [])

    def test_nested_and_shared_pages(self):   # a large range holding small ones after it, and two pools sharing a rounded boundary page
        rs = [(0x10000, 0x90000), (0x20000, 0x21000), (0x88000, 0x8a000), (0x8a000, 0x8c000), (0x100000, 0x101000)]
        self.build(rs)
        for lo, hi in [(0x80000, 0x81000), (0x89000, 0x8b000), (0x8a000, 0x8a010), (0x90000, 0x100000), (0x100fff, 0x101000), (0, 0x10000), (0x101000, 0x200000)]:
            self.assertEqual(self.meet(lo, hi), self.walk(rs, lo, hi), hex(lo))

    def test_against_the_walk(self):   # pools of a game's sizes, some overlapping or nested, page-aligned as the tracker keeps them; writes of a few bytes to a few pages
        rnd = random.Random(11)
        for trial in range(20):
            rs = []
            for _ in range(rnd.choice((1, 30, 600, 4300))):
                lo = 0x4c00000000 + (rnd.randrange(1 << 18) << 12); n = rnd.choice((1, 1, 2, 4, 16, 256, 4096))
                rs.append((lo, lo + (n << 12)))
            self.build(rs)
            for _ in range(400):
                a = 0x4c00000000 + rnd.randrange(1 << 30); lo = a & ~0xfff; hi = (a + rnd.choice((16, 4096, 70000)) + 0xfff) & ~0xfff
                self.assertEqual(self.meet(lo, hi), self.walk(rs, lo, hi), f'trial {trial} {lo:#x}')


if __name__ == '__main__':
    unittest.main()
