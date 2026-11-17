"""dm/android/spv_nostore.h (a vertex shader's memory writes taken out, for its rasterized draws once its fragment form made them), built for the PC with
spv_readonly.h after it, as gfx_vk_lmod.inc lm_code runs them, on modules assembled with the NDK's spirv-as; spirv-val checks each result. Skipped without the NDK.
With DM_SPV_DIR=<a game's packed spv folder>, also run over every vertex shader there whose res.json says it writes a storage buffer."""
import ctypes
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_spv_readonly import NDK, HOST, EXE, REPO   # noqa: E402
from test_spv_vsfrag import VS   # noqa: E402   (a skinning vertex shader that stores into a storage buffer at gl_VertexIndex)

# the same store made an atomic add whose result the shader writes to its position: a plain load of the buffer instead
VS_ATOMIC = VS.replace('OpStore %p %s\n', '%old = OpAtomicIAdd %uint %p %u1 %u0 %s\n%oldf = OpConvertUToF %float %old\n%pos = OpCompositeInsert %v4 %oldf %a 0\n').replace(
    'OpStore %pp %a\n', 'OpStore %pp %pos\n').replace('%u8 = OpConstant %uint 8\n', '%u8 = OpConstant %uint 8\n%u1 = OpConstant %uint 1\n%u0 = OpConstant %uint 0\n')


def ops(data):   # [(opcode, operands)]
    w = struct.unpack('<%dI' % (len(data) // 4), data); i = 5; out = []
    while i < len(w):
        n = w[i] >> 16; out.append((w[i] & 0xffff, w[i + 1:i + n])); i += max(n, 1)
    return out


@unittest.skipIf(NDK is None, 'no Android NDK')
class SpvNoStore(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        assert NDK is not None
        cls.tmp = Path(tempfile.mkdtemp())
        tools = NDK / 'shader-tools' / HOST; cls.spv_as, cls.spv_val = tools / f'spirv-as{EXE}', tools / f'spirv-val{EXE}'
        cls.bin = NDK / 'toolchains/llvm/prebuilt' / HOST / 'bin'
        src = cls.tmp / 't.c'
        export = '__declspec(dllexport)' if os.name == 'nt' else '__attribute__((visibility("default")))'
        src.write_text(f'#include "spv_nostore.h"\n#include "spv_readonly.h"\n'
                       f'{export} size_t ns(const uint32_t *w, size_t n, uint32_t *s, uint32_t *o, size_t cap, int *d) {{ return spv_nostore(w, n, s, o, cap, d); }}\n'
                       f'{export} size_t ro(const uint32_t *w, size_t n, uint32_t *s, uint32_t *o, size_t cap, int *wr) {{ return spv_readonly(w, n, s, o, cap, wr); }}\n')
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
        cls.lib.ns.restype = cls.lib.ro.restype = ctypes.c_size_t

    @classmethod
    def tearDownClass(cls): shutil.rmtree(cls.tmp, ignore_errors=True)

    def asm(self, text, name):
        (self.tmp / f'{name}.spvasm').write_text(text)
        subprocess.run([self.spv_as, '--target-env', 'vulkan1.1', self.tmp / f'{name}.spvasm', '-o', self.tmp / f'{name}.spv'], check=True)
        return (self.tmp / f'{name}.spv').read_bytes()

    def valid(self, data, name):
        (self.tmp / f'{name}.out.spv').write_bytes(data)
        r = subprocess.run([self.spv_val, '--target-env', 'vulkan1.1', self.tmp / f'{name}.out.spv'], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def run2(self, data):   # → (spv_nostore's result or None, its drop count, spv_readonly's result over it or None, the variables still written)
        n = len(data) // 4; w = (ctypes.c_uint32 * n).from_buffer_copy(data); sc = (ctypes.c_uint32 * (2 * w[3]))(); o = (ctypes.c_uint32 * n)(); d = ctypes.c_int()
        k = self.lib.ns(w, n, sc, o, n, ctypes.byref(d))
        if not k: return None, d.value, None, None
        cap = k + 3 * o[3]; r = (ctypes.c_uint32 * cap)(); wr = ctypes.c_int(); sc2 = (ctypes.c_uint32 * (2 * o[3]))()
        rk = self.lib.ro(o, k, sc2, r, cap, ctypes.byref(wr))
        return bytes(o)[:4 * k], d.value, (bytes(r)[:4 * rk] if rk else None), wr.value

    def test_the_store_is_dropped_and_the_buffer_made_read_only(self):
        out, d, ro, wr = self.run2(self.asm(VS, 'vs'))
        self.assertIsNotNone(out); assert out is not None and ro is not None
        self.assertEqual((d, wr), (1, 0))
        self.valid(ro, 'vs')
        o = ops(ro)
        self.assertEqual(sum(1 for op, x in o if op == 62), 1)   # the position's store only
        self.assertIn(24, [x[1] for op, x in o if op == 71])   # NonWritable

    def test_an_atomic_becomes_a_load(self):
        out, d, ro, wr = self.run2(self.asm(VS_ATOMIC, 'vsa'))
        self.assertIsNotNone(out); assert out is not None and ro is not None
        self.assertEqual((d, wr), (1, 0))
        self.valid(ro, 'vsa')
        self.assertFalse([x for op, x in ops(ro) if op == 234])   # no OpAtomicIAdd left

    def test_a_shader_that_writes_nothing_is_left_alone(self):
        vs = VS.replace('OpStore %p %s\n', '')
        self.assertIsNone(self.run2(self.asm(vs, 'vsn'))[0])

    @unittest.skipUnless(os.environ.get('DM_SPV_DIR'), 'DM_SPV_DIR (a game\'s packed spv folder) not set')
    def test_a_games_shaders(self):   # every vertex shader that writes a storage buffer, without its writes, must pass spirv-val with nothing still written
        d = Path(os.environ['DM_SPV_DIR']); n = 0
        for f in sorted(d.glob('*.res.json')):
            res = json.loads(f.read_text(encoding='utf-8'))
            if res.get('stage', 'vs') != 'vs' or not any(b.get('flags', 0) & 1 for b in res.get('sbufs', [])): continue
            key = f.name[:-len('.res.json')]
            out, dr, ro, wr = self.run2((d / f'{key}.spv').read_bytes())
            self.assertIsNotNone(out, key); assert out is not None
            self.assertEqual(wr, 0, key)
            self.valid(ro or out, key); n += 1
        print(f'{n} vertex shaders without their writes in {d}')


if __name__ == '__main__':
    unittest.main()
