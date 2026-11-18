"""dm/android/rc_pend.h (the counter reports the replay made last, found through an index: gfx_vk.c rc_note, rc_value), built for the PC with the system's clang
and driven through ctypes against the list walk it replaced. Needs no NDK."""
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
#include "rc_pend.h"
#define EXPORT __attribute__((visibility("default")))
static rc_pend p;
EXPORT void reset(void) { memset(&p, 0, sizeof p); }
EXPORT void note(uint64_t a, int kind) { rcp_note(&p, a, kind); }
EXPORT int kind(uint64_t a) { return rcp_kind(&p, a); }
EXPORT int count(void) { return p.n; }
'''


def compiler():
    if os.name == 'nt': return None
    if sys.platform == 'darwin': return ['xcrun', 'clang']
    cc = shutil.which('clang') or shutil.which('cc')
    return [cc] if cc else None


class Walk:   # the list as gfx_vk.c kept it before: a walk for each report and each lookup
    def __init__(self): self.e = []
    def note(self, a, k):
        for x in self.e:
            if x[0] == a: x[1] = k; return
        if len(self.e) == 256: self.e = self.e[128:]
        self.e.append([a, k])
    def kind(self, a):
        for x in reversed(self.e):
            if x[0] == a: return x[1]
        return 0


@unittest.skipUnless(compiler(), 'no C compiler')
class RcPend(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp())
        src = cls.tmp / 'h.c'; src.write_text(HARNESS)
        subprocess.run([*compiler(), '-shared', '-fPIC', '-O1', '-Wall', '-Wextra', '-Werror', '-I', REPO / 'dm/android', src, '-o', cls.tmp / 'h.so'], check=True)
        cls.lib = ctypes.CDLL(str(cls.tmp / 'h.so'))
        cls.lib.note.argtypes = [ctypes.c_uint64, ctypes.c_int]; cls.lib.kind.argtypes = [ctypes.c_uint64]; cls.lib.kind.restype = ctypes.c_int

    @classmethod
    def tearDownClass(cls): shutil.rmtree(cls.tmp, ignore_errors=True)

    def setUp(self): self.lib.reset()

    def test_update_in_place(self):
        self.lib.note(0x1000, 2); self.lib.note(0x2000, 1); self.lib.note(0x1000, 1)
        self.assertEqual(self.lib.count(), 2)
        self.assertEqual(self.lib.kind(0x1000), 1); self.assertEqual(self.lib.kind(0x2000), 1); self.assertEqual(self.lib.kind(0x3000), 0)

    def test_older_half_dropped(self):
        for i in range(256): self.lib.note(0x10000 + 32 * i, 2)
        self.assertEqual(self.lib.count(), 256)
        self.lib.note(0x900000, 1)   # full: the oldest 128 go, then the new one is added
        self.assertEqual(self.lib.count(), 129)
        self.assertEqual(self.lib.kind(0x10000), 0); self.assertEqual(self.lib.kind(0x10000 + 32 * 127), 0)
        self.assertEqual(self.lib.kind(0x10000 + 32 * 128), 2); self.assertEqual(self.lib.kind(0x900000), 1)

    def test_against_the_walk(self):   # reports to addresses that recur (as a game's report slots do) and colliding hashes, against the old walk
        rnd = random.Random(7); ref = Walk()
        pool = [0x40000000 + 16 * rnd.randrange(4096) for _ in range(900)] + [0x80000000 + (k << 14) for k in range(64)]
        for step in range(60000):
            a = rnd.choice(pool)
            if rnd.random() < 0.7:
                k = rnd.choice((1, 2)); self.lib.note(a, k); ref.note(a, k)
            else:
                self.assertEqual(self.lib.kind(a), ref.kind(a), f'step {step}')
            if step % 997 == 0: self.assertEqual(self.lib.count(), len(ref.e))


if __name__ == '__main__':
    unittest.main()
