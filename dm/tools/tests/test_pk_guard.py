"""dm/android/pk_guard.h (the start-up list's marker and skip list: an entry whose compile never finishes is left out from the next start on, and the whole list is set
aside after two starts in a row crashed in it; the play guard's: a pipeline whose build crashed a start is not built at the next ones), built for the PC with the
system's clang and driven through ctypes on records like the list's. POSIX only: skipped on Windows, and without a C compiler."""
import ctypes
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
REC = 96   # any record size: the list's (PK_REC) depends on the replay's key, and the header takes it as an argument

CC = ['xcrun', 'clang'] if sys.platform == 'darwin' and shutil.which('xcrun') else [shutil.which('clang') or shutil.which('cc') or '']
SRC = '''#include "pk_guard.h"
#define X __attribute__((visibility("default")))
X int g_open(const char *p) { return pkg_open(p); }
X void g_begin(int fd, const uint8_t *r, size_t n) { pkg_begin(fd, r, n); }
X void g_end(int fd) { pkg_end(fd); }
X void g_crashed(int fd, size_t n) { pkg_crashed(fd, n); }
X int g_take(const char *p, uint8_t *r, size_t n, int *crashed) { return pkg_take(p, r, n, crashed); }
X size_t g_load(const char *p, size_t n, uint8_t **out) { return pkg_skip_load(p, n, out); }
X int g_add(const char *p, const uint8_t *r, size_t n) { return pkg_skip_add(p, r, n); }
X void g_free(void *q) { free(q); }
X int g_strikes(const char *p, int crashed) { return pkg_strikes(p, crashed); }
X int g_aside(const char *l, const char *a) { return pkg_set_aside(l, a); }
X size_t g_prec(void) { return sizeof(pgd_rec); }
X void g_pmake(pgd_rec *r, char kind, uint32_t drv, uint32_t host, const char *a, const char *b, int z) { const char *sh[4] = {a, b, NULL, NULL}; pgd_make(r, kind, drv, host, sh, z); }
X void g_pnote(int fd, const pgd_rec *r) { pgd_note(fd, r); }
X int g_ptake(const char *p, pgd_rec *r) { return pgd_take(p, r); }
X size_t g_pload(const char *p, uint32_t drv, uint32_t host, pgd_rec **out) { return pgd_skip_load(p, drv, host, out); }
X uint32_t g_phost(const char *b) { return pgd_host(b); }
'''


def rec(i):   # a record with its own bytes
    return bytes((i * 7 + j) & 0xff for j in range(REC))


@unittest.skipIf(os.name == 'nt' or not CC[-1], 'POSIX file calls and a C compiler needed')
class PkGuard(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp())
        (cls.tmp / 'g.c').write_text(SRC)
        subprocess.run([*CC, '-shared', '-fPIC', '-O1', '-Wall', '-Wextra', '-Werror', '-I', REPO / 'dm/android', cls.tmp / 'g.c', '-o', cls.tmp / 'g.so'], check=True)
        L = cls.lib = ctypes.CDLL(str(cls.tmp / 'g.so'))
        L.g_load.restype = ctypes.c_size_t
        L.g_load.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(ctypes.POINTER(ctypes.c_uint8))]
        L.g_take.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_int)]
        L.g_begin.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_size_t]
        L.g_crashed.argtypes = [ctypes.c_int, ctypes.c_size_t]
        L.g_add.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t]
        L.g_open.argtypes = [ctypes.c_char_p]
        L.g_free.argtypes = [ctypes.c_void_p]
        L.g_strikes.argtypes = [ctypes.c_char_p, ctypes.c_int]
        L.g_aside.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
        L.g_prec.restype = ctypes.c_size_t
        L.g_pmake.argtypes = [ctypes.c_char_p, ctypes.c_char, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
        L.g_phost.restype = ctypes.c_uint32
        L.g_phost.argtypes = [ctypes.c_char_p]
        L.g_pnote.argtypes = [ctypes.c_int, ctypes.c_char_p]
        L.g_ptake.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
        L.g_pload.restype = ctypes.c_size_t
        L.g_pload.argtypes = [ctypes.c_char_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.POINTER(ctypes.c_void_p)]

    @classmethod
    def tearDownClass(cls): shutil.rmtree(cls.tmp, ignore_errors=True)

    def setUp(self):
        self.d = Path(tempfile.mkdtemp(dir=self.tmp)); self.mk = str(self.d / 'c.making4').encode(); self.sk = str(self.d / 'c.skip4').encode()
        self.st = str(self.d / 'c.strikes4').encode(); self.ls = self.d / 'c.keys4'; self.aside = self.d / 'c.keys4.aside'

    def take(self):   # → (record or None, crashed)
        buf = ctypes.create_string_buffer(REC); cr = ctypes.c_int(-1)
        ok = self.lib.g_take(self.mk, buf, REC, ctypes.byref(cr))
        return (buf.raw if ok else None), cr.value

    def load(self):
        p = ctypes.POINTER(ctypes.c_uint8)(); n = self.lib.g_load(self.sk, REC, ctypes.byref(p))
        out = [bytes(p[i * REC:(i + 1) * REC]) for i in range(n)]
        self.lib.g_free(p); return out

    def start(self, crash_at=None, crashed=False, entries=5):   # a session's prewarm over entries 0..entries-1, ending (without pkg_end) at crash_at
        fd = self.lib.g_open(self.mk); self.assertGreaterEqual(fd, 0)
        for i in range(entries):
            self.lib.g_begin(fd, rec(i), REC)
            if i == crash_at:
                if crashed: self.lib.g_crashed(fd, REC)
                break
            self.lib.g_end(fd)
        os.close(fd)

    def test_a_start_that_finishes_its_compiles_leaves_nothing(self):
        self.start(); self.assertEqual(self.take(), (None, 0))

    def test_a_start_that_ends_in_a_compile_leaves_that_entry_once(self):
        self.start(crash_at=3)
        self.assertEqual(self.take(), (rec(3), 0))
        self.assertFalse(os.path.exists(self.mk)); self.assertEqual(self.take(), (None, 0))

    def test_the_crash_hooks_flag(self):
        self.start(crash_at=2, crashed=True); self.assertEqual(self.take(), (rec(2), 1))

    def test_a_record_without_its_busy_byte_counts_and_a_part_record_does_not(self):
        Path(self.mk.decode()).write_bytes(rec(4)); self.assertEqual(self.take(), (rec(4), 0))
        Path(self.mk.decode()).write_bytes(rec(4)[:REC - 1]); self.assertEqual(self.take(), (None, 0))

    def test_the_skip_list_holds_each_entry_once(self):
        self.assertEqual(self.load(), [])
        self.assertEqual(self.lib.g_add(self.sk, rec(1), REC), 1); self.assertEqual(self.lib.g_add(self.sk, rec(1), REC), 0)
        self.assertEqual(self.lib.g_add(self.sk, rec(2), REC), 1); self.assertEqual(self.load(), [rec(1), rec(2)])

    def test_a_part_record_at_the_skip_lists_end_is_overwritten(self):
        self.lib.g_add(self.sk, rec(1), REC)
        with open(self.sk.decode(), 'ab') as f: f.write(rec(9)[:10])   # a start that ended while it wrote
        self.lib.g_add(self.sk, rec(2), REC); self.assertEqual(self.load(), [rec(1), rec(2)])

    def test_the_next_start_leaves_out_the_entry_the_last_one_ended_in(self):   # the prewarm's own steps (gfx_vk_draw.inc pk_guard_init, prewarm_thread)
        self.start(crash_at=3, crashed=True)
        left, _ = self.take(); self.lib.g_add(self.sk, left, REC)
        skip = set(self.load()); listed = [rec(i) for i in range(5) if rec(i) not in skip]
        self.assertEqual(listed, [rec(0), rec(1), rec(2), rec(4)])
        self.start(entries=0); self.assertEqual(self.take(), (None, 0))   # the session after makes the rest and leaves nothing


    def init(self):   # gfx_vk_draw.inc pk_guard_init's steps: the marker's entry to the skip list, the count, and the list set aside at the second crash in a row
        left, crashed = self.take()
        if left: self.lib.g_add(self.sk, left, REC)
        n = self.lib.g_strikes(self.st, int(bool(left) and bool(crashed)))
        if n >= 2:
            self.assertEqual(self.lib.g_aside(str(self.ls).encode(), str(self.aside).encode()), 1); self.lib.g_strikes(self.st, 0)
        return n

    def test_two_starts_in_a_row_that_crash_in_the_list_set_it_aside(self):
        self.ls.write_bytes(b''.join(rec(i) for i in range(32)))
        self.start(crash_at=0, crashed=True); self.assertEqual(self.init(), 1); self.assertTrue(self.ls.exists())   # one crash: that entry skipped, the list kept
        self.start(crash_at=1, crashed=True); self.assertEqual(self.init(), 2)                                     # the second in a row: the whole list set aside
        self.assertFalse(self.ls.exists()); self.assertEqual(len(self.aside.read_bytes()), 32 * REC)
        self.assertEqual(self.lib.g_strikes(self.st, 0), 0)   # and the count starts again
        self.assertEqual(self.load(), [rec(0), rec(1)])

    def test_a_clean_start_or_a_kill_starts_the_count_again(self):
        self.ls.write_bytes(rec(0) + rec(1))
        self.start(crash_at=0, crashed=True); self.assertEqual(self.init(), 1)
        self.start(); self.assertEqual(self.init(), 0)                             # a start that ran its list through
        self.start(crash_at=1, crashed=True); self.assertEqual(self.init(), 1)
        self.start(crash_at=1, crashed=False); self.assertEqual(self.init(), 0)    # ended in a compile without the crash hook (killed for memory, stopped)
        self.assertTrue(self.ls.exists())

    def test_a_list_set_aside_replaces_an_earlier_one(self):
        self.aside.write_bytes(b'old'); self.ls.write_bytes(rec(5))
        self.assertEqual(self.lib.g_aside(str(self.ls).encode(), str(self.aside).encode()), 1)
        self.assertEqual(self.aside.read_bytes(), rec(5)); self.assertFalse(self.ls.exists())


    # The play guard (pgd_*): only the crash hook writes; the next start takes the record into the skip list, kept per driver version and host build
    def prec(self, kind, drv, a, b=None, z=0, host=5):
        n = self.lib.g_prec(); buf = ctypes.create_string_buffer(n)
        self.lib.g_pmake(buf, kind, drv, host, a, b, z); return buf.raw

    def ptake(self, path):
        buf = ctypes.create_string_buffer(self.lib.g_prec()); return buf.raw if self.lib.g_ptake(path, buf) else None

    def pload(self, path, drv, host=5):
        n = self.lib.g_prec(); p = ctypes.c_void_p(); k = self.lib.g_pload(path, drv, host, ctypes.byref(p))
        out = [ctypes.string_at(p.value + i * n, n) for i in range(k)]
        self.lib.g_free(p); return out

    def test_the_play_guards_record(self):
        self.assertEqual(self.lib.g_prec(), 96)
        r = self.prec(b'c', 0x802fa01f, b'a039399e6336de1b')
        self.assertEqual(r[:4], b'PGD1'); self.assertEqual(r[4:5], b'c'); self.assertEqual(r[16:33], b'a039399e6336de1b\0')
        self.assertEqual(r[33:], bytes(96 - 33))   # the other slots and the padding zero, so records compare whole
        self.assertEqual(self.lib.g_phost(b''), 2166136261); self.assertNotEqual(self.lib.g_phost(b'2a376a350475 (v0.8.12)'), self.lib.g_phost(b'2a376a350476 (v0.8.12)'))
        self.assertNotEqual(self.prec(b'g', 1, b'aaaaaaaaaaaaaaaa', b'bbbbbbbbbbbbbbbb', 1), self.prec(b'g', 1, b'aaaaaaaaaaaaaaaa', b'bbbbbbbbbbbbbbbb', 0))

    def test_a_crash_in_a_build_is_taken_once_at_the_next_start(self):
        mk = str(self.d / 'c.pcrash1').encode()
        fd = self.lib.g_open(mk); os.close(fd); self.assertIsNone(self.ptake(mk))   # a session with no crash leaves an empty marker
        fd = self.lib.g_open(mk); r = self.prec(b'c', 7, b'a039399e6336de1b'); self.lib.g_pnote(fd, r); os.close(fd)
        self.assertEqual(self.ptake(mk), r); self.assertFalse(os.path.exists(mk)); self.assertIsNone(self.ptake(mk))
        Path(mk.decode()).write_bytes(r[:50]); self.assertIsNone(self.ptake(mk))   # a part record
        Path(mk.decode()).write_bytes(b'X' + r[1:]); self.assertIsNone(self.ptake(mk))   # not a record

    def test_the_play_skip_list_is_kept_per_driver_version_and_host_build(self):
        sk = str(self.d / 'c.pskip1').encode(); n = self.lib.g_prec()
        a, b, c = self.prec(b'c', 7, b'a039399e6336de1b'), self.prec(b'g', 7, b'1111111111111111', b'2222222222222222'), self.prec(b'k', 8, b'3333333333333333')
        d = self.prec(b'c', 7, b'4444444444444444', host=6)
        for r in (a, b, a, c, d): self.lib.g_add(sk, r, n)
        self.assertEqual(self.pload(sk, 7), [a, b]); self.assertEqual(Path(sk.decode()).read_bytes(), a + b)   # the other drivers' and builds' records dropped from the file
        for r in (c, d): self.lib.g_add(sk, r, n)
        self.assertEqual(self.pload(sk, 7, host=6), [d]); self.assertEqual(self.pload(sk, 9), []); self.assertEqual(Path(sk.decode()).read_bytes(), b'')
        self.assertEqual(self.pload(str(self.d / 'none').encode(), 7), [])


if __name__ == '__main__':
    unittest.main()
