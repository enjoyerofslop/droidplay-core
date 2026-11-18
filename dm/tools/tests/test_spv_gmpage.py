"""dm/android/spv_gmpage.h (the translator's raw global memory helpers read the page map after the pool table where their cached entry and their chunk's hint miss, before
they search), built for the PC and run on modules assembled with the NDK's spirv-as (test_spv_gmcache's helper); spirv-val checks each result, and spv_gmcache.h must still
take the result. The lookup itself is gmem_table.h's gmem_table_find with paged (test_gmem_table.py). Skipped without the NDK.
With DM_SPV_DIR=<a game's packed spv folder>, also run over every module there with physical pointers: each must be rewritten and pass spirv-val."""
import ctypes
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_spv_readonly import NDK, HOST, EXE, REPO   # noqa: E402
from test_spv_gmcache import helper, words, ops, ENV   # noqa: E402

FORM, ROOM = 1, 2   # spv_gmpage.h's reasons
PAGE_WORD, PAGE_BITS = 65536, 18   # gmem_table.h GMEM_PAGE_WORD, GMEM_PAGE_BITS


@unittest.skipIf(NDK is None, 'no Android NDK')
class SpvGmPage(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        assert NDK is not None
        cls.tmp = Path(tempfile.mkdtemp())
        tools = NDK / 'shader-tools' / HOST; cls.spv_as, cls.spv_val = tools / f'spirv-as{EXE}', tools / f'spirv-val{EXE}'
        cls.bin = NDK / 'toolchains/llvm/prebuilt' / HOST / 'bin'
        src = cls.tmp / 't.c'
        export = '__declspec(dllexport)' if os.name == 'nt' else '__attribute__((visibility("default")))'
        src.write_text(f'#include "spv_gmcache.h"\n#include "spv_gmpage.h"\n'
                       f'{export} size_t run(const uint32_t *w, size_t n, uint32_t *sc, uint32_t *o, size_t cap, int *why) {{ return spv_gmpage(w, n, sc, o, cap, why); }}\n'
                       f'{export} size_t cache(const uint32_t *w, size_t n, uint32_t *sc, uint32_t *o, size_t cap, int *why) {{ return spv_gmcache(w, n, sc, o, cap, why); }}\n'
                       f'{export} int wanted(const uint32_t *w, size_t n) {{ return spv_gmcache_wanted(w, n); }}\n')
        env = dict(os.environ, MSYS_NO_PATHCONV='1')
        if os.name == 'nt':   # no C library: a DLL linked by lld alone
            subprocess.run([cls.bin / 'clang.exe', '--target=x86_64-pc-windows-msvc', '-ffreestanding', '-nostdlib', '-O1', '-Wall', '-Werror', '-I', REPO / 'dm/android', '-c', src,
                            '-o', cls.tmp / 't.obj'], check=True)
            subprocess.run([cls.bin / 'ld.lld.exe', '-flavor', 'link', '-dll', '-noentry', f'-out:{cls.tmp / "t.dll"}', str(cls.tmp / 't.obj')], check=True, env=env)
            cls.lib = ctypes.CDLL(str(cls.tmp / 't.dll'))
        else:   # the system's clang, as test_spv_limits.py explains
            cc = ['xcrun', 'clang'] if sys.platform == 'darwin' else [shutil.which('clang') or cls.bin / 'clang']
            subprocess.run([*cc, '-shared', '-fPIC', '-O1', '-Wall', '-Wextra', '-Werror', '-I', REPO / 'dm/android', src, '-o', cls.tmp / 't.so'], check=True)
            cls.lib = ctypes.CDLL(str(cls.tmp / 't.so'))
        for f in (cls.lib.run, cls.lib.cache):
            f.restype = ctypes.c_size_t; f.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]
        cls.lib.wanted.argtypes = [ctypes.c_void_p, ctypes.c_size_t]

    @classmethod
    def tearDownClass(cls): shutil.rmtree(cls.tmp, ignore_errors=True)

    def asm(self, text, name, env='vulkan1.1'):
        (self.tmp / f'{name}.spvasm').write_text(text)
        subprocess.run([self.spv_as, '--target-env', env, self.tmp / f'{name}.spvasm', '-o', self.tmp / f'{name}.spv'], check=True)
        return (self.tmp / f'{name}.spv').read_bytes()

    def valid(self, data, name, env='vulkan1.1'):
        (self.tmp / f'{name}.out.spv').write_bytes(data)
        r = subprocess.run([self.spv_val, '--target-env', env, self.tmp / f'{name}.out.spv'], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def run_on(self, data, cap=None, fn=None):   # → (the rewritten module or None, why)
        n = len(data) // 4; w = (ctypes.c_uint32 * n).from_buffer_copy(data); cap = cap or 2 * n + 4096; o = (ctypes.c_uint32 * cap)()
        sc = (ctypes.c_uint32 * (4 * words(data)[3]))(); why = ctypes.c_int(-1)
        k = (fn or self.lib.run)(w, n, sc, o, cap, ctypes.byref(why))
        return (bytes(o)[:4 * k] if k else None), why.value

    def check(self, data, name, env='vulkan1.1'):
        self.assertTrue(self.lib.wanted((ctypes.c_uint32 * (len(data) // 4)).from_buffer_copy(data), len(data) // 4))
        out, why = self.run_on(data); self.assertIsNotNone(out, why); assert out is not None
        self.valid(out, name, env)
        o, oi = ops(out), ops(data)
        tab = next(x[0] for op, x in oi if op == 71 and x[1] == 33 and x[2] == 4)   # the variable at binding 4
        consts = {x[1]: x[2] for op, x in o if op == 43 and len(x) == 3}
        chains = lambda m: {x[1]: x for op, x in m if op == 65 and x[2] == tab}
        loads = lambda m: [x for op, x in m if op == 61 and x[2] in chains(m)]
        self.assertEqual(len(loads(o)), len(loads(oi)) + 1)   # one more table word: the page's
        # the search block (the hint block's branch's true label): first its page word load, at the map's first word plus the page's low bits, then a branch on its tag past
        # the search to a new block that goes on to the found block; the search's own branch goes there too
        fn = [i for i, (op, x) in enumerate(o) if op == 54]
        br0 = next(i for i in range(fn[1], len(o)) if o[i][0] == 250); hb = next(i for i in range(br0, len(o)) if o[i][0] == 248 and o[i][1][0] == o[br0][1][1])
        he = next(i for i in range(hb + 1, len(o)) if o[i][0] == 250); search, found = o[he][1][1], o[he][1][2]
        sb = next(i for i in range(he, len(o)) if o[i][0] == 248 and o[i][1][0] == search); pb = next(i for i in range(sb, len(o)) if o[i][0] == 250)
        self.assertFalse([x for op, x in o[hb:he] if op == 128 and consts.get(x[3]) == PAGE_WORD])   # a miss the hint answers reads no page word
        adds = [x for op, x in o[sb:pb] if op == 128 and consts.get(x[3]) == PAGE_WORD]; self.assertEqual(len(adds), 1)
        masks = [x for op, x in o[sb:pb] if op == 199 and consts.get(x[3]) == (1 << PAGE_BITS) - 1]; self.assertEqual(len(masks), 1)
        merge = o[pb][1][1]
        self.assertEqual(o[pb - 1], (247, (merge, 0)))
        ml = next(i for i, (op, x) in enumerate(o) if op == 248 and x[0] == merge); self.assertEqual(o[ml + 1], (249, (found,)))
        self.assertEqual(len([1 for op, x in o if op == 249 and x[0] == merge]), 1)   # the search's end
        self.assertEqual(len([x for op, x in o[sb:ml] if op == 248]), 2)   # the search block, now split in two
        self.assertEqual(len([x for op, x in o if op == 120]), 1)   # the pointer as before
        # spv_gmcache.h still takes the result
        out2, why2 = self.run_on(out, fn=self.lib.cache); self.assertIsNotNone(out2, why2); assert out2 is not None
        self.valid(out2, name + '_gmc', env)
        return o

    def test_translator_form(self):
        self.check(self.asm(helper(), 'cs'), 'cs')

    def test_ssa_form_too(self):   # the values not through locals (a later translator, or an optimizer's output)
        self.check(self.asm(helper(debug=False), 'ssa'), 'ssa')

    def test_spirv_14(self):
        self.check(self.asm(helper().replace('"main" %lid', '"main" %lid %o %tab %lastw'), 'cs14', 'vulkan1.1spv1.4'), 'cs14', 'vulkan1.1spv1.4')

    def test_other_form_left(self):   # a helper whose chunk is not the address's hi << 11 | lo >> 21 is not the translator's: left as it is
        data = self.asm(helper().replace('%ch1 = OpShiftLeftLogical %uint %a1 %u11', '%ch1 = OpShiftLeftLogical %uint %a1 %u13'), 'other')
        out, why = self.run_on(data); self.assertIsNone(out); self.assertEqual(why, FORM)

    def test_pointers_without_the_lookup_left(self):   # physical pointers without the table (test_spv_noptr's module): not the translator's helper
        from test_spv_noptr import CS
        out, why = self.run_on(self.asm(CS, 'noptr')); self.assertIsNone(out); self.assertEqual(why, FORM)

    def test_room(self):
        data = self.asm(helper(), 'room'); out, why = self.run_on(data, cap=len(data) // 4 + 8); self.assertIsNone(out); self.assertEqual(why, ROOM)

    @unittest.skipUnless(os.environ.get('DM_SPV_DIR'), 'DM_SPV_DIR (a game\'s packed spv folder) not set')
    def test_game_modules(self):   # every module with physical pointers of a game's folder: rewritten (or left for a reason), valid, and spv_gmcache still takes it
        d = Path(os.environ['DM_SPV_DIR']); n = done = 0; left = {}; bad = []
        for p in sorted(d.glob('*.spv')):
            data = p.read_bytes()
            if len(data) < 20 or not self.lib.wanted((ctypes.c_uint32 * (len(data) // 4)).from_buffer_copy(data), len(data) // 4): continue
            n += 1; out, why = self.run_on(data)
            if out is None: left[why] = left.get(why, 0) + 1; continue
            done += 1; env = ENV.get(words(data)[1], 'vulkan1.1')
            out2, why2 = self.run_on(out, fn=self.lib.cache)
            if out2 is None: bad.append(f'{p.name}: spv_gmcache left the result ({why2})'); continue
            (self.tmp / 'g.spv').write_bytes(out2)
            r = subprocess.run([self.spv_val, '--target-env', env, self.tmp / 'g.spv'], capture_output=True, text=True)
            if r.returncode: bad.append(f'{p.name}: {(r.stdout + r.stderr).strip()[:200]}')
        print(f'\n{n} modules with physical pointers in {d}: {done} rewritten, left {left}, invalid {len(bad)}')
        self.assertFalse(bad, '\n'.join(bad[:10]))
        self.assertGreater(done, 0)


if __name__ == '__main__':
    unittest.main()
