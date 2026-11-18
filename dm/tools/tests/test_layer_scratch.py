"""dm/android/layer_scratch.h, which render passes the Vulkan replay draws into one-layer scratch images (gfx_vk_layerscratch.inc: colour targets that are
different layers of one level of one array or cube image, which Qualcomm's driver on the Adreno 830 draws into the wrong layers), built with the system's C compiler
and called through ctypes. Skipped without a C compiler."""
import ctypes
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
_CC = shutil.which('clang') or shutil.which('cc') or shutil.which('gcc')
CC = ['xcrun', 'clang'] if sys.platform == 'darwin' and shutil.which('xcrun') else [_CC] if _CC else []   # empty: no C compiler
SRC = '''#include "layer_scratch.h"
#define X __attribute__((visibility("default")))
X int g_takes(const lsc_slot *s, int n, uint32_t layers, uint32_t samples) { return lsc_takes(s, n, layers, samples); }
'''


class Slot(ctypes.Structure):   # lsc_slot
    _fields_ = [('img', ctypes.c_size_t), ('lv', ctypes.c_uint32), ('ly', ctypes.c_uint32), ('plain', ctypes.c_int)]


CUBE, OTHER = 0x1000, 0x2000


def cube_level(lv, faces=6):   # Ocarina of Time's reflection cube filter: one pass a level, its six faces as six colour targets
    return [(CUBE, lv, f, 1) for f in range(faces)]


@unittest.skipIf(os.name == 'nt' or not CC, 'a C compiler is needed (POSIX)')
class LayerScratch(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp())
        (cls.tmp / 'g.c').write_text(SRC)
        subprocess.run([*CC, '-shared', '-fPIC', '-O1', '-Wall', '-Wextra', '-Werror', '-I', REPO / 'dm/android', cls.tmp / 'g.c', '-o', cls.tmp / 'g.so'], check=True)
        cls.lib = ctypes.CDLL(str(cls.tmp / 'g.so'))
        cls.lib.g_takes.argtypes = [ctypes.POINTER(Slot), ctypes.c_int, ctypes.c_uint32, ctypes.c_uint32]

    @classmethod
    def tearDownClass(cls): shutil.rmtree(cls.tmp, ignore_errors=True)

    def takes(self, slots, layers=1, samples=1):
        a = (Slot * max(len(slots), 1))(*[Slot(*s) for s in slots])
        return self.lib.g_takes(a, len(slots), layers, samples)

    def test_the_faces_of_one_cube_level_are_drawn_into_scratch_images(self):
        self.assertEqual(self.takes(cube_level(2)), 6)
        self.assertEqual(self.takes(cube_level(0, 2)), 2)

    def test_empty_slots_between_them_count_for_nothing(self):
        self.assertEqual(self.takes([(CUBE, 1, 0, 1), (0, 0, 0, 0), (CUBE, 1, 3, 1)]), 2)

    def test_one_target_or_one_layer_is_drawn_as_it_is(self):
        self.assertEqual(self.takes([(CUBE, 1, 4, 1)]), 0)
        self.assertEqual(self.takes([(CUBE, 1, 4, 1), (CUBE, 1, 4, 1)]), 0)   # one layer in two slots: dup_slots' case, not this one

    def test_targets_in_two_images_or_two_levels_are_drawn_as_they_are(self):
        self.assertEqual(self.takes([(CUBE, 1, 0, 1), (OTHER, 1, 1, 1)]), 0)
        self.assertEqual(self.takes([(CUBE, 1, 0, 1), (CUBE, 2, 1, 1)]), 0)
        self.assertEqual(self.takes([(CUBE, 1, 0, 1), (CUBE, 1, 1, 1), (OTHER, 0, 0, 1)]), 0)

    def test_a_target_that_is_not_plain_keeps_the_pass_as_it_is(self):   # a BC shadow, a format held another way, a view in another format, a 3D slice
        self.assertEqual(self.takes([(CUBE, 1, 0, 1), (CUBE, 1, 1, 0)]), 0)

    def test_a_layered_or_multisample_pass_is_drawn_as_it_is(self):
        self.assertEqual(self.takes(cube_level(1), layers=6), 0)
        self.assertEqual(self.takes(cube_level(1), samples=4), 0)

    def test_more_than_eight_slots_is_refused(self):
        self.assertEqual(self.takes([(CUBE, 0, i, 1) for i in range(9)]), 0)


if __name__ == '__main__':
    unittest.main()
