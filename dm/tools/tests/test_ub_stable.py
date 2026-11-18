"""dm/android/ub_stable.h (whether a game uniform buffer has gone unwritten long enough to be bound in place: gfx_vk_gmem_inplace.inc ub_inplace), built for the PC with
the system's clang and driven through ctypes with a fake tracker. As nvn_dirty.c does, the fake stamps a page only when it is written while protected. Needs no NDK."""
import ctypes
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
PAGE = 4096
STABLE, BACKOFF = 60, 30

HARNESS = r'''
#include "ub_stable.h"
#define EXPORT __attribute__((visibility("default")))
enum { NP = 64 };
static unsigned char mem[NP * 4096] __attribute__((aligned(4096)));
static uint64_t gen[NP], now_ = 1; static int prot[NP], lo = 0, hi = NP, protects;
static uintptr_t pg(const void *a) { return ((uintptr_t)a - (uintptr_t)mem) / 4096; }
static const void *f_span(const void *a, size_t n) { return pg(a) >= (uintptr_t)lo && pg((const char *)a + n - 1) < (uintptr_t)hi ? (const void *)mem : 0; }
static uint64_t f_now(void) { return now_; }
static void f_protect(const void *s, const void *a, size_t n) { (void)s; for (uintptr_t p = pg(a); p <= pg((const char *)a + n - 1); p++) prot[p] = 1; protects++; }
static int f_since(const void *s, const void *a, size_t n, uint64_t st) { (void)s; for (uintptr_t p = pg(a); p <= pg((const char *)a + n - 1); p++) if (gen[p] > st) return 1; return 0; }
static const ubs_ops ops = {f_span, f_now, f_protect, f_since};
static ubs_ent tab[16];
EXPORT void reset(void) { for (int i = 0; i < NP; i++) { gen[i] = 0; prot[i] = 0; } for (int i = 0; i < 16; i++) tab[i] = (ubs_ent){0}; now_ = 1; lo = 0; hi = NP; protects = 0; }
EXPORT void track(int l, int h) { lo = l; hi = h; }
EXPORT void write_page(int p) { if (prot[p]) { gen[p] = ++now_; prot[p] = 0; } }   /* the fault handler: stamped and writable again; a write to a writable page leaves no trace */
EXPORT int protects_n(void) { return protects; }
EXPORT int look(size_t off, size_t n, uint64_t present) { uint64_t a = (uint64_t)(uintptr_t)(mem + off); return ubs_look(ubs_slot(tab, 15, a, n), &ops, a, n, present, 0); }
'''


def compiler():
    if os.name == 'nt': return None
    if sys.platform == 'darwin': return ['xcrun', 'clang']
    cc = shutil.which('clang') or shutil.which('cc')
    return [cc] if cc else None


@unittest.skipUnless(compiler(), 'no C compiler')
class UbStable(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp())
        src = cls.tmp / 'h.c'; src.write_text(HARNESS)
        subprocess.run([*compiler(), '-shared', '-fPIC', '-O1', '-Wall', '-Wextra', '-Werror', '-I', REPO / 'dm/android', src, '-o', cls.tmp / 'h.so'], check=True)
        cls.lib = ctypes.CDLL(str(cls.tmp / 'h.so'))
        cls.lib.look.argtypes = [ctypes.c_size_t, ctypes.c_size_t, ctypes.c_uint64]

    @classmethod
    def tearDownClass(cls): shutil.rmtree(cls.tmp, ignore_errors=True)

    def setUp(self): self.lib.reset()

    def test_in_place_only_after_it_has_gone_unwritten(self):
        self.assertEqual(self.lib.look(PAGE, 600, 100), 0)               # the first look stamps and protects
        self.assertEqual(self.lib.protects_n(), 1)
        self.assertEqual(self.lib.look(PAGE, 600, 100 + STABLE - 1), 0)  # not yet
        self.assertEqual(self.lib.look(PAGE, 600, 100 + STABLE), 1)
        self.assertEqual(self.lib.protects_n(), 1)

    def test_a_buffer_written_every_frame_stays_a_copy(self):   # Ocarina of Time's pair of 12,720-byte buffers, rewritten while the replay still binds them
        protects = []
        for f in range(400):
            self.lib.write_page(1)
            self.assertEqual(self.lib.look(PAGE, 12720, 1000 + f), 0, f)
            protects.append(self.lib.protects_n())
        self.assertLessEqual(protects[-1], 400 // BACKOFF + 2)   # stamped again only after each back-off, not every frame

    def test_a_write_takes_it_out_of_place_until_stable_again(self):
        self.lib.look(0, 256, 10)
        self.assertEqual(self.lib.look(0, 256, 10 + STABLE), 1)
        self.lib.write_page(0)
        t = 10 + STABLE + 1
        self.assertEqual(self.lib.look(0, 256, t), 0)                       # seen written
        n = self.lib.protects_n()
        self.assertEqual(self.lib.look(0, 256, t + BACKOFF - 1), 0)         # within the back-off: no stamp
        self.assertEqual(self.lib.protects_n(), n)
        self.assertEqual(self.lib.look(0, 256, t + BACKOFF), 0)             # stamped again
        self.assertEqual(self.lib.protects_n(), n + 1)
        self.assertEqual(self.lib.look(0, 256, t + BACKOFF + STABLE - 1), 0)
        self.assertEqual(self.lib.look(0, 256, t + BACKOFF + STABLE), 1)

    def test_a_write_to_another_page_changes_nothing(self):
        self.lib.look(0, 256, 10); self.lib.write_page(5)
        self.assertEqual(self.lib.look(0, 256, 10 + STABLE), 1)

    def test_every_page_of_the_buffer_counts(self):
        self.lib.look(PAGE - 16, 32, 10); self.lib.write_page(1)   # the buffer's second page
        self.assertEqual(self.lib.look(PAGE - 16, 32, 10 + STABLE), 0)

    def test_untracked_memory_is_always_copied(self):
        self.lib.track(0, 8)
        for p in (10, 100, 1000): self.assertEqual(self.lib.look(7 * PAGE, 2 * PAGE, p), 0)
        self.assertEqual(self.lib.protects_n(), 0)

    def test_another_buffer_in_the_slot_starts_over(self):
        self.lib.look(0, 256, 10)
        self.lib.look(0, 512, 20)   # same address, another size: its own entry, maybe the same slot
        self.assertEqual(self.lib.look(0, 512, 20 + STABLE), 1)
        self.assertEqual(self.lib.look(0, 256, 20 + STABLE), 0)   # if it lost its slot, it starts again


if __name__ == '__main__':
    unittest.main()
