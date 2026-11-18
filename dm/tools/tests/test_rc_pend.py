"""dm/android/rc_pend.h (conditional rendering's pending timestamp reports, gfx_vk.c rc_note / rc_value), built with the system's C compiler and called through
ctypes: however many occlusion reports come after a timestamp report, the timestamp is still known as 0, as nvn_mtl.m's table keeps it (dm/gfx/tests/t_sbw.m).
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
_CC = shutil.which('clang') or shutil.which('cc') or shutil.which('gcc')
CC = ['xcrun', 'clang'] if sys.platform == 'darwin' and shutil.which('xcrun') else [_CC] if _CC else []   # empty: no C compiler
SRC = '''#include "rc_pend.h"
#define X __attribute__((visibility("default")))
static rcp_list g_l;
X void g_reset(void) { memset(&g_l, 0, sizeof g_l); }
X void g_note(uint64_t a, int timestamp) { rcp_note(&g_l, a, timestamp); }
X int g_find(uint64_t a) { return rcp_find(&g_l, a); }
X int g_n(void) { return g_l.n; }
X uint64_t g_dropped(void) { return g_l.dropped; }
X int g_cap(void) { return RC_PEND; }
'''
TIMESTAMP, UNKNOWN = 1, 2
REC = 0x7100000000   # report records 16 bytes apart, as a game's are
QUERY = 0x7200000000


@unittest.skipIf(os.name == 'nt' or not CC, 'a C compiler is needed (POSIX)')
class RcPend(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp())
        (cls.tmp / 'g.c').write_text(SRC)
        subprocess.run([*CC, '-shared', '-fPIC', '-O1', '-Wall', '-Wextra', '-Werror', '-I', REPO / 'dm/android', cls.tmp / 'g.c', '-o', cls.tmp / 'g.so'], check=True)
        L = cls.lib = ctypes.CDLL(str(cls.tmp / 'g.so'))
        L.g_note.argtypes = [ctypes.c_uint64, ctypes.c_int]
        L.g_find.argtypes = [ctypes.c_uint64]
        L.g_dropped.restype = ctypes.c_uint64

    @classmethod
    def tearDownClass(cls): shutil.rmtree(cls.tmp, ignore_errors=True)

    def setUp(self): self.lib.g_reset()

    def queries(self, n, base=QUERY):
        for i in range(n): self.lib.g_note(base + 16 * i, 0)

    def test_a_timestamp_before_a_frame_of_occlusion_reports_is_still_known(self):   # Ocarina of Time's refraction clear: one report, then ~530 queries, then the condition
        self.lib.g_note(REC, 1)
        self.queries(9000)
        self.assertEqual(self.lib.g_find(REC), TIMESTAMP)
        self.assertEqual(self.lib.g_n(), 1)

    def test_a_timestamp_after_them_is_known(self):   # t_sbw.m's case
        self.queries(9000); self.lib.g_note(REC, 1)
        self.assertEqual(self.lib.g_find(REC), TIMESTAMP)

    def test_an_occlusion_report_takes_no_entry(self):   # the counter report address set answers for it
        self.queries(3)
        self.assertEqual(self.lib.g_find(QUERY), 0); self.assertEqual(self.lib.g_n(), 0)

    def test_a_query_after_a_timestamp_to_one_address_is_unknown(self):
        self.lib.g_note(REC, 1); self.lib.g_note(REC, 0)
        self.assertEqual(self.lib.g_find(REC), UNKNOWN)
        self.lib.g_note(REC, 1)   # and a timestamp after that is known again
        self.assertEqual(self.lib.g_find(REC), TIMESTAMP); self.assertEqual(self.lib.g_n(), 1)

    def test_a_record_with_no_report_has_no_entry(self):
        self.lib.g_note(REC, 1)
        self.assertEqual(self.lib.g_find(REC + 16), 0)

    def test_more_timestamps_than_the_list_holds_keep_the_newest(self):
        cap = self.lib.g_cap(); n = cap + 44
        for i in range(n): self.lib.g_note(REC + 16 * i, 1)
        self.assertLessEqual(self.lib.g_n(), cap)
        self.assertEqual(self.lib.g_find(REC + 16 * (n - 1)), TIMESTAMP)
        self.assertEqual(self.lib.g_find(REC + 16 * (n - cap // 2)), TIMESTAMP)
        self.assertEqual(self.lib.g_find(REC), 0)   # the oldest went
        self.assertEqual(self.lib.g_dropped(), cap // 2)


if __name__ == '__main__':
    unittest.main()
