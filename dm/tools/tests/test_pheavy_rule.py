"""dm/android/pheavy_rule.h, the rules for pipelines that hold a scratch block on Qualcomm's driver (gfx_vk_pheavy.inc), built with the system's C compiler and called
through ctypes: which driver object is a scratch block, the age under memory pressure, and which pipelines the sweep destroys. Skipped without a C compiler."""
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
MIB = 1 << 20


@unittest.skipIf(CC is None or os.name == 'nt', 'no C compiler')
class HeavyPipelines(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        assert CC is not None   # (skipped without one; pyright doesn't follow skipIf)
        cls.tmp = Path(tempfile.mkdtemp())
        src = cls.tmp / 'p.c'
        src.write_text('#include "pheavy_rule.h"\n'
                       '__attribute__((visibility("default"))) uint64_t block(uint64_t b) { return phr_block(b); }\n'
                       '__attribute__((visibility("default"))) uint64_t age(uint64_t a, uint64_t g, uint64_t la, uint64_t lm) { return phr_age(a, g, la, lm); }\n'
                       '__attribute__((visibility("default"))) int goes(uint64_t b, int m, uint64_t l, uint64_t s, uint64_t n, uint64_t d, uint64_t g) '
                       '{ return phr_goes(b, m, l, s, n, d, g); }\n')
        subprocess.run([*CC, '-shared', '-fPIC', '-O1', '-Wall', '-Wextra', '-Werror', '-I', str(REPO / 'dm/android'), str(src), '-o', str(cls.tmp / 'p.so')], check=True)
        cls.lib = ctypes.CDLL(str(cls.tmp / 'p.so'))
        cls.lib.block.argtypes = [ctypes.c_uint64]; cls.lib.block.restype = ctypes.c_uint64
        cls.lib.age.argtypes = [ctypes.c_uint64] * 4; cls.lib.age.restype = ctypes.c_uint64
        cls.lib.goes.argtypes = [ctypes.c_uint64, ctypes.c_int] + [ctypes.c_uint64] * 5

    def test_a_block_is_4_mib_or_more(self):
        self.assertEqual(self.lib.block(12 * MIB), 12 * MIB)   # a compute pipeline's block on the Adreno 740
        self.assertEqual(self.lib.block(8 * MIB), 8 * MIB)   # the 730's
        self.assertEqual(self.lib.block(4 * MIB), 4 * MIB)
        self.assertEqual(self.lib.block(1060864), 0)   # the code of a 1.4 MB compute shader
        self.assertEqual(self.lib.block(0), 0)   # no report

    def test_the_short_age_only_under_the_line(self):
        self.assertEqual(self.lib.age(6000, 600, 60, 2048), 600)
        self.assertEqual(self.lib.age(1500, 600, 60, 2048), 60)
        self.assertEqual(self.lib.age(2048, 600, 60, 2048), 600)   # at the line
        self.assertEqual(self.lib.age(0, 600, 60, 2048), 600)   # /proc/meminfo unread

    def goes(self, block=24 * MIB, made=1, last=100, seq=5, now=700, done=5, age=600):
        return self.lib.goes(block, made, last, seq, now, done, age)

    def test_goes_when_unbound_for_the_age_and_its_chunk_done(self):
        self.assertTrue(self.goes())   # unbound exactly the age, its chunk completed
        self.assertFalse(self.goes(last=101))   # bound a present later
        self.assertFalse(self.goes(seq=6))   # its last chunk still on the GPU
        self.assertFalse(self.goes(block=0))   # no block: never swept
        self.assertFalse(self.goes(made=0))   # being built in the background, or destroyed already
        self.assertTrue(self.goes(last=0, seq=0, now=600))   # made by a draw that never bound it (the start-up list's are dropped when made)


if __name__ == '__main__':
    unittest.main()
