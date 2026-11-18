"""dm/android/app_version.h (whether a game's host was built with the app it runs in: the host's build stamp against the app's version name, for front_android.c's
start line), built for the PC with the system's clang and driven through ctypes. Skipped without a C compiler."""
import ctypes
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
CC = ['xcrun', 'clang'] if sys.platform == 'darwin' and shutil.which('xcrun') else [shutil.which('clang') or shutil.which('cc') or '']
SRC = '''#include "app_version.h"
#define X __attribute__((visibility("default")))
X int g_match(const char *stamp, const char *app) { return host_matches_app(stamp, app); }
'''


@unittest.skipIf(os.name == 'nt' or not CC[-1], 'a C compiler needed')
class AppVersion(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp()); (cls.tmp / 'v.c').write_text(SRC)
        subprocess.run([*CC, '-shared', '-fPIC', '-O1', '-Wall', '-Wextra', '-Werror', '-I', REPO / 'dm/android', cls.tmp / 'v.c', '-o', cls.tmp / 'v.so'], check=True)
        cls.lib = ctypes.CDLL(str(cls.tmp / 'v.so')); cls.lib.g_match.argtypes = [ctypes.c_char_p, ctypes.c_char_p]

    @classmethod
    def tearDownClass(cls): shutil.rmtree(cls.tmp, ignore_errors=True)

    def match(self, stamp, app): return self.lib.g_match(None if stamp is None else stamp.encode(), None if app is None else app.encode())

    def test_one_build_packed_both(self):   # build_stamp.py's version has the tag's v, build_apk.py's --version-name (core_version) has none
        self.assertEqual(self.match('f73b0bbb6abc (v0.8.13-7-gf73b0bbb6)', '0.8.13-7-gf73b0bbb6'), 1)
        self.assertEqual(self.match('f73b0bbb6abc+dirty (v0.8.13-7-gf73b0bbb6)', '0.8.13-7-gf73b0bbb6'), 1)   # the version names the commit; changes on it don't show
        self.assertEqual(self.match('0123456789ab (v0.8.14)', '0.8.14'), 1)   # a release's archive: VERSION on both sides
        self.assertEqual(self.match('not recorded (v0.8.14)', '0.8.14'), 1)

    def test_a_host_an_older_update_packed(self):   # the app updated, the game's host not built again
        self.assertEqual(self.match('0123456789ab (v0.8.13)', '0.8.14'), 0)
        self.assertEqual(self.match('0123456789ab (v0.8.13)', '0.8.13-1-gaaaaaaaa'), 0)   # a prefix isn't the version
        self.assertEqual(self.match('0123456789ab (v0.8.13-1-gaaaaaaaa)', '0.8.13'), 0)

    def test_nothing_to_compare(self):
        self.assertEqual(self.match('not recorded', '0.8.14'), -1)   # a host built outside a checkout or a release
        self.assertEqual(self.match('0123456789ab', '0.8.14'), -1)   # a shallow clone: no tags, no version
        self.assertEqual(self.match('0123456789ab (v0.8.14)', '0.1'), -1)   # an app packed without a version keeps the manifest's
        self.assertEqual(self.match('0123456789ab (v0.8.14)', ''), -1)
        self.assertEqual(self.match(None, '0.8.14'), -1); self.assertEqual(self.match('0123456789ab (v0.8.14)', None), -1)


if __name__ == '__main__':
    unittest.main()
