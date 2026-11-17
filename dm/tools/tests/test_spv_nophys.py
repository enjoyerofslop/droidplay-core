"""dm/android/spv_nophys.h (with raw global memory off, loads and stores through physical pointers taken out, so Arm's Mali never reads one), built for the PC
and run on a module assembled with the NDK's spirv-as; spirv-val checks the result. Skipped without the NDK."""
import ctypes
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

# a raw global memory access as the translator makes one: a pointer from a 64-bit address, a load and a store through it
CS = '''OpCapability Shader
OpCapability PhysicalStorageBufferAddresses
OpCapability Int64
OpMemoryModel PhysicalStorageBuffer64 GLSL450
OpEntryPoint GLCompute %main "main"
OpExecutionMode %main LocalSize 1 1 1
%void = OpTypeVoid
%fn = OpTypeFunction %void
%uint = OpTypeInt 32 0
%ulong = OpTypeInt 64 0
%pp = OpTypePointer PhysicalStorageBuffer %uint
%pf = OpTypePointer Function %uint
%c = OpConstant %ulong 4096
%u1 = OpConstant %uint 1
%main = OpFunction %void None %fn
%l = OpLabel
%v = OpVariable %pf Function
%p = OpConvertUToPtr %pp %c
%x = OpLoad %uint %p Aligned 4
OpStore %v %x
OpStore %p %u1 Aligned 4
%y = OpAtomicIAdd %uint %p %u1 %u1 %u1
OpStore %v %y
OpReturn
OpFunctionEnd
'''


def ops(data):
    w = struct.unpack('<%dI' % (len(data) // 4), data); i = 5; out = []
    while i < len(w):
        n = w[i] >> 16; out.append((w[i] & 0xffff, w[i + 1:i + n])); i += max(n, 1)
    return out


@unittest.skipIf(NDK is None, 'no Android NDK')
class SpvNoPhys(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        assert NDK is not None
        cls.tmp = Path(tempfile.mkdtemp())
        tools = NDK / 'shader-tools' / HOST; cls.spv_as, cls.spv_val = tools / f'spirv-as{EXE}', tools / f'spirv-val{EXE}'
        cls.bin = NDK / 'toolchains/llvm/prebuilt' / HOST / 'bin'
        src = cls.tmp / 't.c'
        export = '__declspec(dllexport)' if os.name == 'nt' else '__attribute__((visibility("default")))'
        src.write_text(f'#include "spv_nophys.h"\n{export} size_t np(const uint32_t *w, size_t n, uint32_t *s, uint32_t *o, size_t cap, int *c) {{ return spv_nophys(w, n, s, o, cap, c); }}\n')
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
        cls.lib.np.restype = ctypes.c_size_t

    @classmethod
    def tearDownClass(cls): shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_loads_read_null_and_stores_go(self):
        (self.tmp / 'cs.spvasm').write_text(CS)
        subprocess.run([self.spv_as, '--target-env', 'vulkan1.2', self.tmp / 'cs.spvasm', '-o', self.tmp / 'cs.spv'], check=True)
        data = (self.tmp / 'cs.spv').read_bytes(); n = len(data) // 4; w = (ctypes.c_uint32 * n).from_buffer_copy(data)
        cap = n + 128; o = (ctypes.c_uint32 * cap)(); sc = (ctypes.c_uint32 * (2 * w[3]))(); c = ctypes.c_int()
        k = self.lib.np(w, n, sc, o, cap, ctypes.byref(c)); self.assertTrue(k)
        out = bytes(o)[:4 * k]; (self.tmp / 'out.spv').write_bytes(out)
        r = subprocess.run([self.spv_val, '--target-env', 'vulkan1.2', self.tmp / 'out.spv'], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        o2 = ops(out)
        self.assertEqual(c.value, 3)   # the load, the store and the atomic
        self.assertEqual(sum(1 for op, x in o2 if op == 61), 0)   # no load left
        self.assertEqual(sum(1 for op, x in o2 if op == 62), 2)   # the two stores into the local only
        self.assertEqual(sum(1 for op, x in o2 if op == 46), 1)   # one null constant
        self.assertFalse([x for op, x in o2 if op == 234])   # no atomic left


if __name__ == '__main__':
    unittest.main()
