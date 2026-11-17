"""dm/android/wb_filter.h (a submitted chunk's summary of the guest memory its pending writes cover: gfx_vk.c chunk_hits and gfx_vk_draw.inc fwd_hit skip a chunk's
lists for a lookup it rules out), built for the PC with the system's clang and driven through ctypes. The summary may send a lookup on to the scan when nothing
overlaps (granules that hash alike), but it must never rule out a range that overlaps one it was given. Needs no NDK."""
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
G = 1 << 16   # a granule (WBF_SH)

HARNESS = r'''
#include "wb_filter.h"
#define EXPORT __attribute__((visibility("default")))
static wb_filter f;
EXPORT void reset(void) { wbf_reset(&f, WBF_WIDE); }
EXPORT void add(uint64_t lo, uint64_t hi) { wbf_add(&f, (uintptr_t)lo, (uintptr_t)hi); }
EXPORT int may(uint64_t lo, uint64_t hi) { return wbf_may(&f, (uintptr_t)lo, (uintptr_t)hi); }
EXPORT int ok(void) { return f.ok; }
EXPORT int nwide(void) { return f.nwide; }
static wb_filter u;   /* the chunks in flight together (gfx_vk.c wbf_union) */
EXPORT void u_reset(void) { wbf_reset(&u, WBF_UWIDE); }
EXPORT void u_merge(void) { wbf_merge(&u, &f); }
EXPORT int u_may(uint64_t lo, uint64_t hi) { return wbf_may(&u, (uintptr_t)lo, (uintptr_t)hi); }
EXPORT int u_ok(void) { return u.ok; }
'''


def compiler():
    if os.name == 'nt': return None
    if sys.platform == 'darwin': return ['xcrun', 'clang']
    cc = shutil.which('clang') or shutil.which('cc')
    return [cc] if cc else None


@unittest.skipUnless(compiler(), 'no C compiler')
class WbFilter(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp())
        src = cls.tmp / 'h.c'; src.write_text(HARNESS)
        subprocess.run([*compiler(), '-shared', '-fPIC', '-O1', '-Wall', '-Wextra', '-Werror', '-I', REPO / 'dm/android', src, '-o', cls.tmp / 'h.so'], check=True)
        cls.lib = ctypes.CDLL(str(cls.tmp / 'h.so'))
        for fn in ('add', 'may', 'u_may'): getattr(cls.lib, fn).argtypes = [ctypes.c_uint64, ctypes.c_uint64]

    @classmethod
    def tearDownClass(cls): shutil.rmtree(cls.tmp, ignore_errors=True)

    def setUp(self): self.lib.reset()

    def test_an_empty_summary_rules_out_everything(self):
        for lo in (0x10000000, 0x7f12345000, 0x123):
            self.assertEqual(self.lib.may(lo, lo + 4096), 0)

    def test_a_range_given_is_never_ruled_out(self):   # an overlap anywhere in it: its first byte, its last, one straddling either end
        base = 0x7f00010000 + 0x3456; self.lib.add(base, base + 3 * G)
        for lo, hi in ((base, base + 1), (base + 3 * G - 1, base + 3 * G), (base - 100, base + 1), (base + 3 * G - 1, base + 3 * G + 500), (base - G, base + 4 * G)):
            self.assertEqual(self.lib.may(lo, hi), 1, (hex(lo), hex(hi)))

    def test_a_range_in_other_granules_is_ruled_out_unless_a_hash_meets(self):   # most of them: the point of the summary
        self.lib.add(0x7f00000000, 0x7f00000000 + 4096)
        far = [0x7e00000000 + k * 7 * G for k in range(2000)]
        self.assertLess(sum(self.lib.may(a, a + 4096) for a in far), 20)

    def test_wide_ranges_are_kept_exactly(self):   # a 9.56 MB buffer is not 146 granule bits: a lookup beside it is still ruled out
        big = 0x7f10000000; self.lib.add(big, big + 9_560_000)
        self.assertEqual(self.lib.nwide(), 1)
        self.assertEqual(self.lib.may(big + 5_000_000, big + 5_000_016), 1)
        self.assertEqual(self.lib.may(big + 9_560_000, big + 9_560_016), 0)
        self.assertEqual(self.lib.may(big - 16, big), 0)

    def test_too_many_wide_ranges_leave_no_summary(self):   # every lookup scans the chunk, as before the summary
        for k in range(4): self.lib.add(0x7f00000000 + k * 0x10000000, 0x7f00000000 + k * 0x10000000 + 32 * G)
        self.assertEqual(self.lib.ok(), 1)
        self.lib.add(0x7f80000000, 0x7f80000000 + 32 * G)
        self.assertEqual(self.lib.ok(), 0)
        self.assertEqual(self.lib.may(0x10000, 0x10010), 1)

    def test_empty_and_long_lookups_go_on_to_the_scan(self):   # as chunk_hits and fwd_hit took them before
        self.assertEqual(self.lib.may(0x5000, 0x5000), 1)
        self.assertEqual(self.lib.may(0x6000, 0x5000), 1)
        self.assertEqual(self.lib.may(0x10000000, 0x10000000 + 17 * G), 1)

    def test_the_chunks_in_flight_together_cover_what_each_covers(self):   # wbf_union: one lookup rules out every chunk in flight, or none
        self.lib.u_reset(); chunks = [[(0x7f00000000 + k * 0x1000000, 0x7f00000000 + k * 0x1000000 + 8192), (0x7e00000000 + k * 0x2000000, 0x7e00000000 + k * 0x2000000 + 40 * G)] for k in range(4)]
        for rs in chunks:
            self.lib.reset()
            for lo, hi in rs: self.lib.add(lo, hi)
            self.lib.u_merge()
        self.assertEqual(self.lib.u_ok(), 1)
        for rs in chunks:
            for lo, hi in rs: self.assertEqual(self.lib.u_may(lo + 100, lo + 200), 1)
        self.assertEqual(self.lib.u_may(0x7d00000000, 0x7d00000000 + 4096), 0)

    def test_a_chunk_without_a_summary_leaves_the_chunks_in_flight_without_one(self):   # raw stores, too many wide ranges: every lookup scans
        self.lib.u_reset(); self.lib.reset(); self.lib.add(0x7f00000000, 0x7f00001000); self.lib.u_merge(); self.assertEqual(self.lib.u_ok(), 1)
        self.lib.reset()
        for k in range(5): self.lib.add(0x7f00000000 + k * 0x10000000, 0x7f00000000 + k * 0x10000000 + 32 * G)
        self.lib.u_merge(); self.assertEqual(self.lib.u_ok(), 0); self.assertEqual(self.lib.u_may(0x10000, 0x10010), 1)

    def test_more_wide_ranges_in_flight_than_the_union_holds_leave_it_without_one(self):
        self.lib.u_reset()
        for k in range(5):
            self.lib.reset()
            for j in range(4): self.lib.add(0x7000000000 + (4 * k + j) * 0x10000000, 0x7000000000 + (4 * k + j) * 0x10000000 + 32 * G)
            self.lib.u_merge()
        self.assertEqual(self.lib.u_ok(), 0)

    def test_random_ranges_never_ruled_out_when_they_overlap(self):   # against a plain list scan, as chunk_hits makes it
        rnd = random.Random(351)
        for _ in range(200):
            self.lib.reset(); rs = []
            for _ in range(rnd.randrange(1, 40)):
                lo = 0x7f00000000 + rnd.randrange(0, 1 << 28) & ~15; n = rnd.choice([16, 256, 4096, 60000, 200000, 2_000_000])
                rs.append((lo, lo + n)); self.lib.add(lo, lo + n)
            for _ in range(200):
                lo = 0x7f00000000 + rnd.randrange(0, 1 << 28); n = rnd.choice([1, 64, 4096, 65536, 300000])
                if any(a < lo + n and lo < b for a, b in rs) and self.lib.ok():
                    self.assertEqual(self.lib.may(lo, lo + n), 1, (hex(lo), n))


if __name__ == '__main__':
    unittest.main()
