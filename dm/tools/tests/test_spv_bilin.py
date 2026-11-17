"""dm/android/spv_bilin.h (bilinear filtering made by the shader, for a texture whose format the GPU can't filter linearly bound with a linear sampler: the Xclipse 950
filters no depth format and no 32-bit float one, and Pokemon Legends: Arceus's shadow mask samples its D32_SFLOAT shadow map linearly), built for the PC and run on
modules assembled with the NDK's spirv-as; spirv-val checks each result. Skipped without the NDK.
With DM_SPV_DIR=<a game's packed spv folder>, also run over every fragment shader there, each of its texture bindings marked (each rewritten module must pass spirv-val).
With DM_TEST_SERIAL=<adb serial>, dm/android/tests/t_bilin.c is built with the NDK and run on that device: the gathers' result against the CPU's bilinear filter (within
1e-5), for D32_SFLOAT, its comparison form and R32_SFLOAT."""
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

# A fragment shader as the translator writes Arceus's shadow mask (3be95ee5087724c5): a sampler2DArray at set 1 binding 4, sampled with an implicit level and a bias,
# only the result's x read; a sampler2D at binding 5 read with an explicit level and a constant offset, its whole vec4 stored; a cube at binding 6; a sampler2D at
# binding 7 only fetched
FS = '''OpCapability Shader
OpCapability ImageQuery
OpCapability SampledCubeArray
%glsl = OpExtInstImport "GLSL.std.450"
OpMemoryModel Logical GLSL450
OpEntryPoint Fragment %main "main" %uv %o0
OpExecutionMode %main OriginUpperLeft
OpDecorate %uv Location 0
OpDecorate %o0 Location 0
OpDecorate %t4 DescriptorSet 1
OpDecorate %t4 Binding 4
OpDecorate %t5 DescriptorSet 1
OpDecorate %t5 Binding 5
OpDecorate %t6 DescriptorSet 1
OpDecorate %t6 Binding 6
OpDecorate %t7 DescriptorSet 1
OpDecorate %t7 Binding 7
%void = OpTypeVoid
%fn = OpTypeFunction %void
%float = OpTypeFloat 32
%uint = OpTypeInt 32 0
%int = OpTypeInt 32 1
%v2 = OpTypeVector %float 2
%v3 = OpTypeVector %float 3
%v4 = OpTypeVector %float 4
%v2i = OpTypeVector %int 2
%ia = OpTypeImage %float 2D 0 1 0 1 Unknown
%sa = OpTypeSampledImage %ia
%i2 = OpTypeImage %float 2D 0 0 0 1 Unknown
%s2 = OpTypeSampledImage %i2
%ic = OpTypeImage %float Cube 0 0 0 1 Unknown
%sc = OpTypeSampledImage %ic
%psa = OpTypePointer UniformConstant %sa
%ps2 = OpTypePointer UniformConstant %s2
%psc = OpTypePointer UniformConstant %sc
%t4 = OpVariable %psa UniformConstant
%t5 = OpVariable %ps2 UniformConstant
%t6 = OpVariable %psc UniformConstant
%t7 = OpVariable %ps2 UniformConstant
%pin = OpTypePointer Input %v3
%pout = OpTypePointer Output %v4
%uv = OpVariable %pin Input
%o0 = OpVariable %pout Output
%zero = OpConstant %float 0
%bias = OpConstant %float 0.5
%i1 = OpConstant %int 1
%i0 = OpConstant %int 0
%off = OpConstantComposite %v2i %i1 %i0
%main = OpFunction %void None %fn
%l = OpLabel
%c = OpLoad %v3 %uv
%a = OpLoad %sa %t4
%r = OpImageSampleImplicitLod %v4 %a %c Bias %bias
%x = OpCompositeExtract %float %r 0
%b = OpLoad %s2 %t5
%c2 = OpVectorShuffle %v2 %c %c 0 1
%r5 = OpImageSampleExplicitLod %v4 %b %c2 Lod|ConstOffset %zero %off
%cu = OpLoad %sc %t6
%r6 = OpImageSampleImplicitLod %v4 %cu %c
%f7 = OpLoad %s2 %t7
%im7 = OpImage %i2 %f7
%r7 = OpImageFetch %v4 %im7 %off Lod %i0
%s = OpFAdd %v4 %r5 %r6
%s2v = OpFAdd %v4 %s %r7
%sum = OpVectorTimesScalar %v4 %s2v %x
OpStore %o0 %sum
OpReturn
OpFunctionEnd
'''
# a shadow sampler at binding 3, compared (Dref) with an implicit level
DREF = '''OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint Fragment %main "main" %uv %o0
OpExecutionMode %main OriginUpperLeft
OpDecorate %uv Location 0
OpDecorate %o0 Location 0
OpDecorate %t3 DescriptorSet 1
OpDecorate %t3 Binding 3
%void = OpTypeVoid
%fn = OpTypeFunction %void
%float = OpTypeFloat 32
%v2 = OpTypeVector %float 2
%v3 = OpTypeVector %float 3
%v4 = OpTypeVector %float 4
%id = OpTypeImage %float 2D 1 0 0 1 Unknown
%sd = OpTypeSampledImage %id
%psd = OpTypePointer UniformConstant %sd
%t3 = OpVariable %psd UniformConstant
%pin = OpTypePointer Input %v3
%pout = OpTypePointer Output %v4
%uv = OpVariable %pin Input
%o0 = OpVariable %pout Output
%main = OpFunction %void None %fn
%l = OpLabel
%c = OpLoad %v3 %uv
%c2 = OpVectorShuffle %v2 %c %c 0 1
%dr = OpCompositeExtract %float %c 2
%a = OpLoad %sd %t3
%r = OpImageSampleDrefImplicitLod %float %a %c2 %dr
%v = OpCompositeConstruct %v4 %r %r %r %r
OpStore %o0 %v
OpReturn
OpFunctionEnd
'''


def words(data): return struct.unpack('<%dI' % (len(data) // 4), data)


def ops(data):   # [(opcode, operands)]
    w = words(data); i = 5; out = []
    while i < len(w):
        n = w[i] >> 16; out.append((w[i] & 0xffff, w[i + 1:i + n])); i += max(n, 1)
    return out


@unittest.skipIf(NDK is None, 'no Android NDK')
class SpvBilin(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        assert NDK is not None
        cls.tmp = Path(tempfile.mkdtemp())
        tools = NDK / 'shader-tools' / HOST; cls.spv_as, cls.spv_val = tools / f'spirv-as{EXE}', tools / f'spirv-val{EXE}'
        cls.bin = NDK / 'toolchains/llvm/prebuilt' / HOST / 'bin'
        src = cls.tmp / 'b.c'
        export = '__declspec(dllexport)' if os.name == 'nt' else '__attribute__((visibility("default")))'
        src.write_text(f'#include "spv_bilin.h"\n{export} size_t run(const uint32_t *w, size_t n, uint32_t set, const uint16_t *bl, int nbl, uint32_t *o, size_t cap, size_t *need, int *left) '
                       f'{{ return spv_bilin(w, n, set, bl, nbl, o, cap, need, left); }}\n')
        env = dict(os.environ, MSYS_NO_PATHCONV='1')
        if os.name == 'nt':   # no C library: a DLL linked by lld alone
            subprocess.run([cls.bin / 'clang.exe', '--target=x86_64-pc-windows-msvc', '-ffreestanding', '-nostdlib', '-O1', '-Wall', '-Werror', '-I', REPO / 'dm/android', '-c', src,
                            '-o', cls.tmp / 'b.obj'], check=True)
            subprocess.run([cls.bin / 'ld.lld.exe', '-flavor', 'link', '-dll', '-noentry', f'-out:{cls.tmp / "b.dll"}', str(cls.tmp / 'b.obj')], check=True, env=env)
            cls.lib = ctypes.CDLL(str(cls.tmp / 'b.dll'))
        else:   # the system's clang, as test_spv_limits.py explains
            cc = ['xcrun', 'clang'] if sys.platform == 'darwin' else [shutil.which('clang') or cls.bin / 'clang']
            subprocess.run([*cc, '-shared', '-fPIC', '-O1', '-Wall', '-Wextra', '-Werror', '-I', REPO / 'dm/android', src, '-o', cls.tmp / 'b.so'], check=True)
            cls.lib = ctypes.CDLL(str(cls.tmp / 'b.so'))
        cls.lib.run.restype = ctypes.c_size_t

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

    def run_on(self, data, bindings, set_=1):   # bindings: [(binding, channels)] → (the rewritten module or None, samples left)
        n = len(data) // 4; w = (ctypes.c_uint32 * n).from_buffer_copy(data)
        bl = (ctypes.c_uint16 * max(1, len(bindings)))(*[b | c << 12 for b, c in bindings])
        need = ctypes.c_size_t(0); left = ctypes.c_int(0)
        self.lib.run(w, n, set_, bl, len(bindings), None, 0, ctypes.byref(need), ctypes.byref(left))
        if not need.value: return None, left.value
        o = (ctypes.c_uint32 * need.value)(); k = self.lib.run(w, n, set_, bl, len(bindings), o, need.value, None, None)
        self.assertTrue(0 < k <= need.value)
        return bytes(o)[:4 * k], left.value

    def test_an_array_sample_read_in_x_is_one_gather(self):
        out, left = self.run_on(self.asm(FS, 'fs4'), [(4, 1)]); self.assertIsNotNone(out); assert out is not None
        self.valid(out, 'fs4'); o = ops(out)
        self.assertEqual(len([x for op, x in o if op == 96]), 1)   # OpImageGather, component 0 only
        self.assertEqual(len([x for op, x in o if op == 87]), 1)   # the cube's implicit-level sample stays; binding 4's is gone
        self.assertIn(103, [op for op, x in o])   # OpImageQuerySizeLod
        self.assertEqual(len([x for op, x in o if op == 12 and x[3] == 46]), 3)   # three FMix: two rows, then between them

    def test_a_whole_vec4_read_gathers_every_channel_with_the_offset(self):
        out, _ = self.run_on(self.asm(FS, 'fs5'), [(5, 4)]); self.assertIsNotNone(out); assert out is not None
        self.valid(out, 'fs5'); o = ops(out)
        g = [x for op, x in o if op == 96]; self.assertEqual(len(g), 4)   # its vec4 is used whole: four gathers
        self.assertTrue(all(len(x) == 7 and x[5] == 0x8 for x in g))   # each with the ConstOffset (type, id, image, position, channel, mask, offset)
        self.assertEqual(len([x for op, x in o if op == 88]), 0)   # the explicit-level sample is gone
        out1, _ = self.run_on(self.asm(FS, 'fs5b'), [(5, 1)]); self.assertIsNotNone(out1); assert out1 is not None
        self.valid(out1, 'fs5b'); self.assertEqual(len([x for op, x in ops(out1) if op == 96]), 1)   # a one-channel format: one gather, the rest constant

    def test_a_comparison_is_a_dref_gather(self):
        out, _ = self.run_on(self.asm(DREF, 'dref'), [(3, 1)]); self.assertIsNotNone(out); assert out is not None
        self.valid(out, 'dref'); o = ops(out)
        self.assertEqual(len([x for op, x in o if op == 97]), 1)   # OpImageDrefGather
        self.assertFalse([x for op, x in o if op == 89])
        self.assertIn((17, (50,)), o)   # OpCapability ImageQuery added
        self.assertTrue([x for op, x in o if op == 11])   # and the GLSL.std.450 import

    def test_what_it_leaves(self):
        data = self.asm(FS, 'fsl')
        self.assertEqual(self.run_on(data, [(6, 4)]), (None, 0))   # a cube: not a 2D image, not marked
        self.assertEqual(self.run_on(data, [(7, 4)]), (None, 0))   # only fetched: nothing to filter
        self.assertEqual(self.run_on(data, [(4, 1)], set_=0)[0], None)   # another set
        self.assertEqual(self.run_on(data, [(9, 1)])[0], None)   # no such binding
        proj = FS.replace('%r = OpImageSampleImplicitLod %v4 %a %c Bias %bias', '%r = OpImageSampleProjImplicitLod %v4 %a %c Bias %bias').replace(
            '%ia = OpTypeImage %float 2D 0 1 0 1 Unknown', '%ia = OpTypeImage %float 2D 0 0 0 1 Unknown')
        self.assertEqual(self.run_on(self.asm(proj, 'proj'), [(4, 1)]), (None, 1))   # projective: left, and counted

    @unittest.skipUnless(os.environ.get('DM_SPV_DIR'), 'DM_SPV_DIR (a game\'s packed spv folder) not set')
    def test_a_games_fragment_shaders(self):   # every texture binding of each fragment shader marked: each rewritten module must pass spirv-val
        d = Path(os.environ['DM_SPV_DIR']); n = 0
        for f in sorted(d.glob('*.spv')):
            if f.name.count('.') > 1: continue   # .z.spv, .tf.spv
            data = f.read_bytes(); o = ops(data)
            if not any(op == 15 and x[0] == 4 for op, x in o): continue   # a Fragment entry point
            sets = {}; [sets.setdefault(x[0], {}).update({x[1]: x[2]}) for op, x in o if op == 71 and len(x) == 3 and x[1] in (33, 34)]
            binds = sorted({v[33] for v in sets.values() if 33 in v and 34 in v})[:8]
            out, _ = self.run_on(data, [(b, 1) for b in binds])
            if out is None: continue
            self.valid(out, f.stem); n += 1
        print(f'{n} fragment shaders rewritten in {d}')


@unittest.skipUnless(os.environ.get('DM_TEST_SERIAL'), 'DM_TEST_SERIAL not set')
class SpvBilinOnDevice(unittest.TestCase):
    def test_the_gathers_filter_as_the_cpu_does(self):
        if not NDK:
            self.skipTest('the NDK is required')
        serial = os.environ['DM_TEST_SERIAL']
        adb = shutil.which('adb') or str(Path(os.environ.get('ANDROID_HOME', '')) / 'platform-tools' / f'adb{EXE}')
        cc = Path(NDK) / 'toolchains/llvm/prebuilt' / HOST / 'bin' / ('aarch64-linux-android29-clang' + ('.cmd' if os.name == 'nt' else ''))
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / 't_bilin'
            subprocess.run([cc, '-O1', '-Wall', '-Werror', REPO / 'dm/android/tests/t_bilin.c', '-lvulkan', '-lm', '-o', exe], check=True)
            d = '/data/local/tmp/dm_bilin'
            subprocess.run([adb, '-s', serial, 'shell', 'mkdir', '-p', d], check=True)
            subprocess.run([adb, '-s', serial, 'push', exe, d], check=True, capture_output=True)
            r = subprocess.run([adb, '-s', serial, 'shell', f'cd {d} && chmod 755 t_bilin && ./t_bilin; echo "exit $?"'], capture_output=True, text=True)
            print(r.stdout.strip())
            subprocess.run([adb, '-s', serial, 'shell', 'rm', '-r', d], capture_output=True)
            self.assertTrue(r.stdout.strip().endswith('exit 0'), r.stdout)


if __name__ == '__main__':
    unittest.main()
