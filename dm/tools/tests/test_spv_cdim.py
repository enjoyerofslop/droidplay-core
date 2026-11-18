"""dm/android/spv_cdim.h (whether a compute shader reads its dispatch's group counts from its c0 bank, so that an indirect dispatch on the GPU's arguments must copy them
there first), built for the PC with its C compiler and run on modules assembled with the NDK's spirv-as: only a module that binds no c0, or reads only other elements of
it through constant access chains, is found not to read them. Skipped without the NDK."""
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


def cs(reads, binding=1):
    """A compute shader with the translator's c0 (binding 1: a block of vec4[4096] at stride 16) that stores what `reads` loads (SPIR-V lines ending in a float %r)"""
    return f'''OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main" %lid
OpExecutionMode %main LocalSize 64 1 1
OpDecorate %lid BuiltIn LocalInvocationId
OpDecorate %arr ArrayStride 16
OpMemberDecorate %cb 0 Offset 0
OpDecorate %cb Block
OpDecorate %c0 DescriptorSet 0
OpDecorate %c0 Binding {binding}
OpDecorate %fa ArrayStride 4
OpMemberDecorate %ob 0 Offset 0
OpDecorate %ob Block
OpDecorate %o DescriptorSet 0
OpDecorate %o Binding 3
%void = OpTypeVoid
%fn = OpTypeFunction %void
%float = OpTypeFloat 32
%v4 = OpTypeVector %float 4
%uint = OpTypeInt 32 0
%int = OpTypeInt 32 1
%v3uint = OpTypeVector %uint 3
%u4096 = OpConstant %uint 4096
%arr = OpTypeArray %v4 %u4096
%cb = OpTypeStruct %arr
%pcb = OpTypePointer Uniform %cb
%parr = OpTypePointer Uniform %arr
%pv4 = OpTypePointer Uniform %v4
%pf = OpTypePointer Uniform %float
%c0 = OpVariable %pcb Uniform
%fa = OpTypeRuntimeArray %float
%ob = OpTypeStruct %fa
%pob = OpTypePointer StorageBuffer %ob
%psf = OpTypePointer StorageBuffer %float
%o = OpVariable %pob StorageBuffer
%pv3 = OpTypePointer Input %v3uint
%lid = OpVariable %pv3 Input
%i0 = OpConstant %int 0
%i1 = OpConstant %int 1
%i2 = OpConstant %int 2
%i32 = OpConstant %int 32
%i33 = OpConstant %int 33
%i49 = OpConstant %int 49
%main = OpFunction %void None %fn
%ml = OpLabel
%lv = OpLoad %v3uint %lid
%x = OpCompositeExtract %uint %lv 0
{reads}
%po = OpAccessChain %psf %o %i0 %x
OpStore %po %r
OpReturn
OpFunctionEnd
'''


@unittest.skipIf(NDK is None, 'no Android NDK')
class SpvCdim(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        assert NDK is not None
        cls.tmp = Path(tempfile.mkdtemp())
        cls.spv_as = NDK / 'shader-tools' / HOST / f'spirv-as{EXE}'
        cls.bin = NDK / 'toolchains/llvm/prebuilt' / HOST / 'bin'
        src = cls.tmp / 't.c'
        export = '__declspec(dllexport)' if os.name == 'nt' else '__attribute__((visibility("default")))'
        src.write_text(f'#include "spv_cdim.h"\n{export} int run(const uint32_t *w, size_t n, uint32_t b, uint32_t *sc) {{ return spv_cdim_read(w, n, b, sc); }}\n')
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
        cls.lib.run.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_void_p]

    @classmethod
    def tearDownClass(cls): shutil.rmtree(cls.tmp, ignore_errors=True)

    def reads(self, text, binding=1):
        (self.tmp / 'm.spvasm').write_text(text)
        subprocess.run([self.spv_as, '--target-env', 'vulkan1.1', self.tmp / 'm.spvasm', '-o', self.tmp / 'm.spv'], check=True)
        data = (self.tmp / 'm.spv').read_bytes(); n = len(data) // 4
        w = (ctypes.c_uint32 * n).from_buffer_copy(data); sc = (ctypes.c_uint32 * struct.unpack_from('<I', data, 12)[0])()
        return self.lib.run(w, n, binding, sc)

    def test_other_elements(self):   # the uniform buffer table and the shader's own constants: no count read
        self.assertEqual(self.reads(cs('%p = OpAccessChain %pf %c0 %i0 %i49 %i1\n%r = OpLoad %float %p\n%q = OpAccessChain %pf %c0 %i0 %i33 %i0\n%r2 = OpLoad %float %q')), 0)

    def test_nvn2_counts(self):   # element 0: DMSH_C0_DISPATCH_NVN2's words 1-3
        self.assertEqual(self.reads(cs('%p = OpAccessChain %pf %c0 %i0 %i0 %i2\n%r = OpLoad %float %p')), 1)

    def test_counts(self):   # element 32: DMSH_C0_DISPATCH
        self.assertEqual(self.reads(cs('%p = OpAccessChain %pv4 %c0 %i0 %i32\n%v = OpLoad %v4 %p\n%r = OpCompositeExtract %float %v 0')), 1)

    def test_computed_element(self):   # an element the shader computes may be any of them
        self.assertEqual(self.reads(cs('%e = OpBitcast %int %x\n%p = OpAccessChain %pf %c0 %i0 %e %i0\n%r = OpLoad %float %p')), 1)

    def test_other_use(self):   # the array taken whole, then indexed: not followed
        self.assertEqual(self.reads(cs('%a = OpAccessChain %parr %c0 %i0\n%p = OpAccessChain %pf %a %i49 %i0\n%r = OpLoad %float %p')), 1)

    def test_no_c0(self):   # no variable at the bank's binding: nothing read
        self.assertEqual(self.reads(cs('%p = OpAccessChain %pf %c0 %i0 %i0 %i1\n%r = OpLoad %float %p', binding=5)), 0)


if __name__ == '__main__':
    unittest.main()
