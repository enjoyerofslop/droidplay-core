"""dm/android/img_evict.h, the Vulkan replay's image eviction rules (gfx_vk_evict.inc), built with the system's C compiler and called through ctypes: which
images are clean (their content still in guest memory), which another image over their storage overwrote, that the sweep over a pool's images finds every
overwritten one whatever their order, and the byte budget: its default, when it asks for images to go and how many, and that the least recently looked up go first.
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


class Ent(ctypes.Structure):   # ie_ent
    _fields_ = [('pool', ctypes.c_uint64), ('lo', ctypes.c_int64), ('hi', ctypes.c_int64), ('last_present', ctypes.c_uint64), ('last_w', ctypes.c_uint64), ('p', ctypes.c_void_p)]


class Cand(ctypes.Structure):   # ie_cand
    _fields_ = [('last_present', ctypes.c_uint64), ('bytes', ctypes.c_uint64), ('p', ctypes.c_void_p)]


MB = 1 << 20
NONE = (1 << 64) - 1   # UINT64_MAX: nothing to go


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
                       '__attribute__((visibility("default"))) uint64_t pick(ie_cand *c, uint32_t n, uint64_t need, uint8_t *mark) { return ie_pick(c, n, need, mark); }\n'
                       '__attribute__((visibility("default"))) uint64_t meminfo(const char *b, const char *k) { return ie_meminfo_kb(b, k); }\n'
                       '__attribute__((visibility("default"))) uint64_t bdefault(uint64_t kb) { return ie_budget_default(kb); }\n'
                       '__attribute__((visibility("default"))) uint64_t btarget(uint64_t live, uint64_t budget) { return ie_budget_target(live, budget); }\n')
        subprocess.run([*CC, '-shared', '-fPIC', '-O1', '-Wall', '-Wextra', '-Werror', '-I', str(REPO / 'dm/android'), str(src), '-o', str(cls.tmp / 'e.so')], check=True)
        cls.lib = ctypes.CDLL(str(cls.tmp / 'e.so'))
        cls.lib.clean.argtypes = [ctypes.c_int] * 5 + [ctypes.c_uint64] * 3
        cls.lib.over.argtypes = [ctypes.POINTER(Ent), ctypes.POINTER(Ent), ctypes.c_uint64, ctypes.c_uint64]
        cls.lib.sweep.argtypes = [ctypes.POINTER(Ent), ctypes.c_uint32, ctypes.c_uint64, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint8)]
        cls.lib.pick.argtypes = [ctypes.POINTER(Cand), ctypes.c_uint32, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint8)]
        cls.lib.meminfo.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
        cls.lib.bdefault.argtypes = [ctypes.c_uint64]
        cls.lib.btarget.argtypes = [ctypes.c_uint64, ctypes.c_uint64]
        for f in ('pick', 'meminfo', 'bdefault', 'btarget'):
            getattr(cls.lib, f).restype = ctypes.c_uint64

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

    def test_the_budget_defaults_to_a_quarter_of_the_device_memory(self):
        meminfo = b'MemTotal:       11849504 kB\nMemFree:          412352 kB\nSwapTotal:      12582908 kB\n'   # a Galaxy Z Fold 8's 11.3 GB
        kb = self.lib.meminfo(meminfo, b'MemTotal:')
        self.assertEqual(kb, 11849504)
        self.assertEqual(self.lib.bdefault(kb), 11849504 // 4 * 1024)   # 2.83 GB: over the 2.23 GB the ages alone held Saria's scene to there
        self.assertEqual(self.lib.bdefault(16 << 20), 4096 * MB)   # a 16 GB device: 4 GB
        self.assertEqual(self.lib.meminfo(b'MemFree: 1 kB\n', b'MemTotal:'), 0)   # not there
        self.assertEqual(self.lib.bdefault(0), 0)   # and then no budget

    def test_the_budget_asks_for_nothing_until_it_is_passed_then_for_a_tenth_under_it(self):
        b = 3072 * MB
        self.assertEqual(self.lib.btarget(b, b), NONE)   # at the budget: nothing
        self.assertEqual(self.lib.btarget(b - 1, b), NONE)
        self.assertEqual(self.lib.btarget(b + 1, b), b // 100 * 90)   # past it: down to 90 % of it, not just under it
        self.assertEqual(self.lib.btarget(8192 * MB, b), b // 100 * 90)
        self.assertEqual(self.lib.btarget(8192 * MB, 0), NONE)   # no budget

    def pick(self, cands, need):
        arr = (Cand * len(cands))(*[Cand(last, size, i) for i, (last, size) in enumerate(cands)]); mark = (ctypes.c_uint8 * len(cands))()
        got = self.lib.pick(arr, len(cands), need, mark)
        return got, [arr[i].p if arr[i].p is not None else 0 for i in range(len(cands)) if mark[i]]   # the original indices chosen, least recently looked up first

    def test_the_least_recently_looked_up_go_first_until_enough_is_chosen(self):
        cands = [(900, 64 * MB), (100, 16 * MB), (500, 32 * MB), (300, 8 * MB)]   # (last lookup, bytes)
        self.assertEqual(self.pick(cands, 20 * MB), (24 * MB, [1, 3]))   # 100 then 300: 16 + 8 MB covers 20
        self.assertEqual(self.pick(cands, 16 * MB), (16 * MB, [1]))   # exactly one image's worth: no more
        self.assertEqual(self.pick(cands, 1000 * MB), (120 * MB, [1, 3, 2, 0]))   # more than there is: all of them, short of it
        self.assertEqual(self.pick(cands, 0), (0, []))


if __name__ == '__main__':
    unittest.main()
