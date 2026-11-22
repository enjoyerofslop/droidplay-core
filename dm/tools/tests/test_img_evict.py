"""dm/android/img_evict.h, the Vulkan replay's image eviction rules (gfx_vk_evict.inc), built with the system's C compiler and called through ctypes: which
images are clean (their content still in guest memory), which another image over their storage overwrote, that the sweep over a pool's images finds every
overwritten one whatever their order, and the memory pressure rule (the PSI reading, its levels, the bytes to get down to, the least recently looked up first).
Skipped without a C compiler."""
import ctypes
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
CC = (['xcrun', 'clang'] if sys.platform == 'darwin' and shutil.which('xcrun') else
      [c] if (c := shutil.which('clang') or shutil.which('cc') or shutil.which('gcc')) else None)


class Cand(ctypes.Structure):   # ie_cand
    _fields_ = [('last_present', ctypes.c_uint64), ('bytes', ctypes.c_uint64), ('p', ctypes.c_void_p)]


MIB = 1 << 20


class Ent(ctypes.Structure):   # ie_ent
    _fields_ = [('pool', ctypes.c_uint64), ('lo', ctypes.c_int64), ('hi', ctypes.c_int64), ('last_present', ctypes.c_uint64), ('last_w', ctypes.c_uint64), ('p', ctypes.c_void_p)]


@unittest.skipIf(CC is None or os.name == 'nt', 'no C compiler')
class ImageEviction(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        assert CC is not None   # (skipped without one; pyright doesn't follow skipIf)
        cls.tmp = Path(tempfile.mkdtemp())
        src = cls.tmp / 'e.c'
        src.write_text('#include "img_evict.h"\n'
                       '__attribute__((visibility("default"))) int clean(int w, int pr, int pkg, int sh, int f, uint64_t last, uint64_t now, uint64_t age) '
                       '{ return ie_clean(w, pr, pkg, sh, f, last, now, age); }\n'
                       '__attribute__((visibility("default"))) int over(const ie_ent *a, const ie_ent *b, uint64_t now, uint64_t age) { return ie_overwritten(a, b, now, age); }\n'
                       '__attribute__((visibility("default"))) void sweep(ie_ent *e, uint32_t n, uint64_t now, uint64_t age, uint8_t *mark) { ie_sweep(e, n, now, age, mark); }\n'
                       '__attribute__((visibility("default"))) double psi(const char *b) { return ie_psi_some10(b); }\n'
                       '__attribute__((visibility("default"))) int level(double v) { return ie_psi_level(v); }\n'
                       '__attribute__((visibility("default"))) uint64_t meminfo(const char *b, const char *k) { return ie_meminfo_kb(b, k); }\n'
                       '__attribute__((visibility("default"))) int swap(uint64_t f, uint64_t t, unsigned p, uint64_t *last) { return ie_swap_level(f, t, p, last); }\n'
                       '__attribute__((visibility("default"))) uint64_t target(int l, uint64_t live, uint64_t *cap) { return ie_target(l, live, cap); }\n'
                       '__attribute__((visibility("default"))) uint64_t pick(ie_cand *c, uint32_t n, uint64_t need, uint8_t *mark) { return ie_pick(c, n, need, mark); }\n')
        subprocess.run([*CC, '-shared', '-fPIC', '-O1', '-Wall', '-Wextra', '-Werror', '-I', str(REPO / 'dm/android'), str(src), '-o', str(cls.tmp / 'e.so')], check=True)
        cls.lib = ctypes.CDLL(str(cls.tmp / 'e.so'))
        cls.lib.clean.argtypes = [ctypes.c_int] * 5 + [ctypes.c_uint64] * 3
        cls.lib.over.argtypes = [ctypes.POINTER(Ent), ctypes.POINTER(Ent), ctypes.c_uint64, ctypes.c_uint64]
        cls.lib.sweep.argtypes = [ctypes.POINTER(Ent), ctypes.c_uint32, ctypes.c_uint64, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint8)]
        cls.lib.psi.argtypes, cls.lib.psi.restype = [ctypes.c_char_p], ctypes.c_double
        cls.lib.level.argtypes = [ctypes.c_double]
        cls.lib.meminfo.argtypes, cls.lib.meminfo.restype = [ctypes.c_char_p, ctypes.c_char_p], ctypes.c_uint64
        cls.lib.swap.argtypes = [ctypes.c_uint64, ctypes.c_uint64, ctypes.c_uint, ctypes.POINTER(ctypes.c_uint64)]
        cls.lib.target.argtypes, cls.lib.target.restype = [ctypes.c_int, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64)], ctypes.c_uint64
        cls.lib.pick.argtypes, cls.lib.pick.restype = [ctypes.POINTER(Cand), ctypes.c_uint32, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint8)], ctypes.c_uint64

    def clean(self, gpu_w=0, pool_read=0, pkg=0, shared=0, filled=0, last=0, now=1000, age=600):
        return self.lib.clean(gpu_w, pool_read, pkg, shared, filled, last, now, age)

    def test_clean_needs_its_content_in_guest_memory_and_an_idle_age(self):
        self.assertTrue(self.clean(pkg=1))   # a packaged texture, 1000 presents without a lookup
        self.assertTrue(self.clean(shared=1, filled=1))   # a shared one filled from its storage
        self.assertFalse(self.clean(shared=1))   # a shared one the GPU gave its content: not its storage's
        self.assertFalse(self.clean(pkg=1, gpu_w=1))   # written by the GPU since
        self.assertFalse(self.clean(pkg=1, pool_read=1))   # read through the texture pool by ID
        self.assertFalse(self.clean(pkg=1, last=500))   # looked up 500 presents ago, under the age
        self.assertTrue(self.clean(pkg=1, last=400))   # exactly the age

    def over(self, a, b, now=1000, age=30):
        return self.lib.over(ctypes.byref(Ent(*a)), ctypes.byref(Ent(*b)), now, age)

    def test_overwritten_needs_a_later_write_over_half_of_it(self):
        # (pool, lo, hi, last lookup, last write)
        a = (1, 0, 1000, 900, 900)
        self.assertTrue(self.over(a, (1, 0, 1000, 950, 950)))   # the same storage, written after a's last lookup
        self.assertFalse(self.over(a, (1, 0, 1000, 950, 900)))   # written in the same present as a's last lookup
        self.assertFalse(self.over(a, (1, 0, 1000, 950, 800)))   # written before it
        self.assertFalse(self.over(a, (1, 0, 1000, 950, 0)))   # never written: only read
        self.assertFalse(self.over(a, (2, 0, 1000, 950, 950)))   # another pool
        self.assertFalse(self.over(a, (1, 1000, 2000, 950, 950)))   # next to it, not over it
        self.assertTrue(self.over(a, (1, 500, 2000, 950, 950)))   # over half of it
        self.assertFalse(self.over(a, (1, 501, 2000, 950, 950)))   # under half: an edge a padded layout clips
        self.assertTrue(self.over(a, (1, 0, 1000, 950, 950), now=930))   # exactly the age
        self.assertFalse(self.over(a, (1, 0, 1000, 950, 950), now=929))   # under it

    def sweep(self, ents, now=1000, age=30):
        arr = (Ent * len(ents))(*[Ent(*e, i) for i, e in enumerate(ents)]); mark = (ctypes.c_uint8 * len(ents))()
        self.lib.sweep(arr, len(ents), now, age, mark)
        return {arr[i].p if arr[i].p is not None else 0 for i in range(len(ents)) if mark[i]}   # the original indices marked

    def test_the_sweep_finds_every_overwritten_image_in_any_order(self):
        ents = [(7, 4096, 8192, 900, 900),    # 0: an old target over the second half of 3's storage, 4 wrote over it later: overwritten
                (3, 0, 4096, 990, 990),       # 1: pool 3, used now
                (7, 0, 16384, 995, 995),      # 2: the scene's new target over all of pool 7's region: it overwrote 0 and 3
                (7, 0, 4096, 800, 800),       # 3: an old target at the region's start: overwritten by 2
                (3, 0, 4096, 500, 0),         # 4: pool 3, a texture never written, idle, under 1 which was written after it: overwritten
                (7, 16384, 20000, 100, 100),  # 5: next to 2, not over it: kept
                (3, 8192, 9000, 10, 0)]       # 6: alone in pool 3: kept
        self.assertEqual(self.sweep(ents), {0, 3, 4})
        self.assertEqual(self.sweep(list(reversed(ents))), {6 - 0, 6 - 3, 6 - 4})   # the same images, given in the other order
        self.assertEqual(self.sweep(ents, age=150), {3, 4})   # a longer age: 0, looked up 100 presents ago, stays
        self.assertEqual(self.sweep([]), set())

    def test_the_pressure_reading_and_its_levels(self):
        text = b'some avg10=7.31 avg60=2.05 avg300=0.50 total=123456\nfull avg10=1.20 avg60=0.40 avg300=0.10 total=4567\n'
        self.assertAlmostEqual(self.lib.psi(text), 7.31)   # "some", not "full": lmkd's thrashing rule reads "some" too
        self.assertEqual(self.lib.psi(b'full avg10=3.00 avg60=0 avg300=0 total=0\n'), -1)
        self.assertEqual(self.lib.psi(b''), -1)
        self.assertEqual([self.lib.level(v) for v in (-1, 0, 4.99, 5, 14.99, 15, 80)], [0, 0, 0, 1, 1, 2, 2])

    def test_swap_from_meminfo(self):
        text = b'MemTotal:       11893412 kB\nMemAvailable:     801196 kB\nSwapTotal:      12582908 kB\nSwapFree:        4446376 kB\n'
        self.assertEqual(self.lib.meminfo(text, b'SwapTotal:'), 12582908)
        self.assertEqual(self.lib.meminfo(text, b'SwapFree:'), 4446376)
        self.assertEqual(self.lib.meminfo(text, b'Shmem:'), 0)

    def swap(self, frees, total=12 * 1024 * 1024, pct=40):   # GiB of SwapFree in turn, with ~12 GiB of swap at lmkd's 40 %: the line 4.8 GiB, holding under 6.0
        last = ctypes.c_uint64(0)
        return [self.lib.swap(int(f * 1024 * 1024), total, pct, ctypes.byref(last)) for f in frees]

    def test_swap_holds_near_lmkds_line_and_shrinks_as_it_falls(self):
        self.assertEqual(self.swap([8, 6.1, 5.9, 5.0]), [0, 0, 1, 1])   # under the line plus a quarter: holding
        self.assertEqual(self.swap([5.0, 4.7, 4.6, 4.5, 4.3, 4.2, 3.9]), [1, 2, 1, 1, 2, 1, 2])   # under it: an eighth as it crosses, again each further 32nd (0.375 GiB)
        self.assertEqual(self.swap([4.7, 5.0, 4.7]), [2, 1, 2])   # back over the line and under it again: a new crossing
        self.assertEqual(self.swap([4.7, 7, 4.7]), [2, 0, 2])
        self.assertEqual(self.swap([1], total=0), [0])   # no swap
        self.assertEqual(self.swap([1], pct=0), [0])

    def target(self, steps):
        cap = ctypes.c_uint64(0)
        out = []
        for l, live in steps:
            t = self.lib.target(l, live * MIB, ctypes.byref(cap)); out.append((-1 if t == 2 ** 64 - 1 else t, cap.value // MIB))
        return out

    def test_the_target_holds_at_the_least_alive_and_shrinks_by_an_eighth(self):
        self.assertEqual(self.target([(1, 1600), (1, 1700), (1, 1500), (1, 1550)]),
                         [(1600 * MIB, 1600), (1600 * MIB, 1600), (1500 * MIB, 1500), (1500 * MIB, 1500)])   # holding: the least alive since the hold began
        self.assertEqual(self.target([(2, 1600), (1, 1500), (2, 1400)]),
                         [(1400 * MIB, 1400), (1400 * MIB, 1400), (1225 * MIB, 1225)])   # shrinking: an eighth under what is alive, and the hold continues from there
        self.assertEqual(self.target([(2, 1600), (0, 1700), (1, 1700)]), [(1400 * MIB, 1400), (-1, 0), (1700 * MIB, 1700)])   # eased: the next hold starts anew
        self.assertEqual(self.target([(2, 520), (1, 300)]), [(512 * MIB, 512), (512 * MIB, 512)])   # never under the floor

    def pick(self, cands, need):
        arr = (Cand * len(cands))(*[Cand(lp, b * MIB, i + 1) for i, (lp, b) in enumerate(cands)]); mark = (ctypes.c_uint8 * len(cands))()
        got = self.lib.pick(arr, len(cands), need * MIB, mark)
        return got // MIB, sorted(arr[i].p - 1 for i in range(len(cands)) if mark[i])

    def test_the_pick_takes_the_least_recently_looked_up_until_enough(self):
        cands = [(900, 64), (100, 32), (500, 128), (990, 16), (300, 8)]   # (last lookup, MiB)
        self.assertEqual(self.pick(cands, 100), (168, [1, 2, 4]))   # 100 (32), 300 (8), 500 (128): enough after the third
        self.assertEqual(self.pick(list(reversed(cands)), 100), (168, [4 - 4, 4 - 2, 4 - 1]))   # the same images whatever the order
        self.assertEqual(self.pick(cands, 0), (0, []))
        self.assertEqual(self.pick(cands, 10000), (248, [0, 1, 2, 3, 4]))   # not enough: all of them


if __name__ == '__main__':
    unittest.main()
