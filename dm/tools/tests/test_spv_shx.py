"""dm/android/spv_shx.h (every OpGroupNonUniformShuffle made OpGroupNonUniformShuffleXor by lane ^ index, for Qualcomm's V@07xx compilers that crash on the shuffle),
built for the PC and run on modules assembled with the NDK's spirv-as; spirv-val checks each result, and a guard after the words it was given checks that it writes
no further. Skipped without the NDK.
With DM_SPV_DIR=<a game's packed spv folder>, also run over every module there: each rewritten one must pass spirv-val and keep no plain shuffle.
With DM_TEST_SERIAL=<adb serial>, dm/android/tests/t_shx.c is built for Android and run on that device with a compute shader of eight kinds of shuffle, as compiled
and as rewritten: the rewritten one must read the right lanes (the one as compiled may crash the driver's compiler, which is what the rewrite is for)."""
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
from test_spv_wide import FS, ADRENO_830, DONE   # noqa: E402
from test_spv_xor import COMPUTE, DOWN, IDX_ONLY   # noqa: E402

GUARD = 0xa5a5a5a5
SITE = 9   # SPV_SHX_SITE

# A compute shader that shuffles to lane 0 with no SubgroupLocalInvocationId built-in: left as it is (there is no lane to XOR with)
NO_LANE = '''OpCapability Shader
OpCapability GroupNonUniform
OpCapability GroupNonUniformShuffle
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main" %gid
OpExecutionMode %main LocalSize 64 1 1
OpDecorate %gid BuiltIn GlobalInvocationId
OpDecorate %ra ArrayStride 4
OpMemberDecorate %ob 0 Offset 0
OpDecorate %ob Block
OpDecorate %out DescriptorSet 0
OpDecorate %out Binding 0
%void = OpTypeVoid
%fn = OpTypeFunction %void
%f32 = OpTypeFloat 32
%u32 = OpTypeInt 32 0
%v3u = OpTypeVector %u32 3
%pv3u = OpTypePointer Input %v3u
%ra = OpTypeRuntimeArray %f32
%ob = OpTypeStruct %ra
%pob = OpTypePointer StorageBuffer %ob
%psf = OpTypePointer StorageBuffer %f32
%gid = OpVariable %pv3u Input
%out = OpVariable %pob StorageBuffer
%scope = OpConstant %u32 3
%u0 = OpConstant %u32 0
%main = OpFunction %void None %fn
%l = OpLabel
%g = OpLoad %v3u %gid
%gx = OpCompositeExtract %u32 %g 0
%v = OpConvertUToF %f32 %gx
%s = OpGroupNonUniformShuffle %f32 %scope %v %u0
%p = OpAccessChain %psf %out %u0 %gx
OpStore %p %s
OpReturn
OpFunctionEnd
'''

# The device's shader: each invocation shuffles a float to eight computed lanes and writes its facts for t_shx.c's check (12 words each)
SELFTEST_GLSL = '''#version 450
#extension GL_KHR_shader_subgroup_shuffle : require
#extension GL_KHR_shader_subgroup_basic : require
layout(local_size_x = 128) in;
layout(std430, set = 0, binding = 0) buffer R { uint r[]; };
void main() {
    uint gid = gl_GlobalInvocationID.x, lane = gl_SubgroupInvocationID, sz = gl_SubgroupSize;
    float v = float(gid * 7u + 3u);
    uint idx[8];
    idx[0] = lane ^ 1u; idx[1] = lane ^ 13u; idx[2] = min(lane + 1u, sz - 1u); idx[3] = lane == 0u ? 0u : lane - 1u;
    idx[4] = (lane * 5u + 3u) & (sz - 1u); idx[5] = 0u; idx[6] = lane ^ ((lane >> 2) & 7u); idx[7] = (gid * 2654435761u >> 7) & (sz - 1u);
''' + ''.join(f'    r[gid * 12u + {k}u] = floatBitsToUint(subgroupShuffle(v, idx[{k}]));\n' for k in range(8)) + '''    r[gid * 12u + 8u] = gl_SubgroupID; r[gid * 12u + 9u] = lane; r[gid * 12u + 10u] = floatBitsToUint(v); r[gid * 12u + 11u] = sz;
}
'''


@unittest.skipUnless(NDK, 'the NDK is not installed')
class SpvShx(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        assert NDK is not None
        cls.tmp = Path(tempfile.mkdtemp())
        tools = NDK / 'shader-tools' / HOST
        cls.spv_as, cls.spv_val, cls.spv_dis, cls.glslc = tools / f'spirv-as{EXE}', tools / f'spirv-val{EXE}', tools / f'spirv-dis{EXE}', tools / f'glslc{EXE}'
        cls.bin = NDK / 'toolchains/llvm/prebuilt' / HOST / 'bin'
        src = cls.tmp / 'x.c'
        export = '__declspec(dllexport)' if os.name == 'nt' else '__attribute__((visibility("default")))'
        src.write_text(f'#include "spv_shx.h"\n#include "spv_wide.h"\n'
                       f'{export} size_t run(const uint32_t *w, size_t n, uint32_t *o, size_t cap, int *ns) {{ return spv_shx(w, n, o, cap, ns); }}\n'
                       f'{export} int count(const uint32_t *w, size_t n) {{ return spv_shx_count(w, n); }}\n'
                       f'{export} size_t wide(const uint32_t *w, size_t n, uint32_t sg, uint32_t *s, uint32_t *o, size_t cap, size_t *need, int *what) '
                       f'{{ return spv_wide(w, n, sg, s, o, cap, need, what); }}\n'
                       f'{export} uint32_t wide_wanted(const uint32_t *w, size_t n, uint32_t sg) {{ return spv_wide_wanted(w, n, sg); }}\n')
        env = dict(os.environ, MSYS_NO_PATHCONV='1')
        if os.name == 'nt':   # no C library: a DLL linked by lld alone, as test_spv_wide
            subprocess.run([cls.bin / 'clang.exe', '--target=x86_64-pc-windows-msvc', '-ffreestanding', '-nostdlib', '-O1', '-Wall', '-Werror', '-I', REPO / 'dm/android', '-c', src,
                            '-o', cls.tmp / 'x.obj'], check=True)
            subprocess.run([cls.bin / 'ld.lld.exe', '-flavor', 'link', '-dll', '-noentry', f'-out:{cls.tmp / "x.dll"}', str(cls.tmp / 'x.obj')], check=True, env=env)
            cls.lib = ctypes.CDLL(str(cls.tmp / 'x.dll'))
        else:   # the system's clang, as test_spv_wide
            cc = ['xcrun', 'clang'] if sys.platform == 'darwin' else [shutil.which('clang') or cls.bin / 'clang']
            subprocess.run([*cc, '-shared', '-fPIC', '-O1', '-Wall', '-Wextra', '-Werror', '-I', REPO / 'dm/android', src, '-o', cls.tmp / 'x.so'], check=True)
            cls.lib = ctypes.CDLL(str(cls.tmp / 'x.so'))
        cls.lib.run.restype = ctypes.c_size_t; cls.lib.wide.restype = ctypes.c_size_t
        cls.lib.run.argtypes = [ctypes.POINTER(ctypes.c_uint32), ctypes.c_size_t, ctypes.POINTER(ctypes.c_uint32), ctypes.c_size_t, ctypes.POINTER(ctypes.c_int)]
        cls.lib.count.argtypes = [ctypes.POINTER(ctypes.c_uint32), ctypes.c_size_t]

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def asm(self, text, name):
        p = self.tmp / f'{name}.spvasm'; p.write_text(text)
        subprocess.run([self.spv_as, '--target-env', 'vulkan1.1', p, '-o', self.tmp / f'{name}.spv'], check=True)
        return (self.tmp / f'{name}.spv').read_bytes()

    def valid(self, data, name):   # under the Vulkan whose SPIR-V the module's version needs (a task / mesh module is SPIR-V 1.4)
        p = self.tmp / f'{name}.out.spv'; p.write_bytes(data)
        env = 'vulkan1.2' if int.from_bytes(data[4:8], 'little') >= 0x10400 else 'vulkan1.1'
        r = subprocess.run([self.spv_val, '--target-env', env, p], capture_output=True, text=True)
        return r.returncode == 0, r.stderr

    def dis(self, data, name):
        p = self.tmp / f'{name}.d.spv'; p.write_bytes(data)
        return subprocess.run([self.spv_dis, '--raw-id', p], capture_output=True, text=True, check=True).stdout

    def shx(self, data, extra=0):   # → (the rewritten module or None, the shuffles rewritten); extra: words short of what it needs
        n = len(data) // 4; w = (ctypes.c_uint32 * n).from_buffer_copy(data)
        sh = self.lib.count(w, n); cap = n + SITE * sh - extra
        o = (ctypes.c_uint32 * (max(cap, 0) + 64))(*[GUARD] * (max(cap, 0) + 64)); ns = ctypes.c_int()
        k = self.lib.run(w, n, o, max(cap, 0), ctypes.byref(ns))
        self.assertTrue(all(o[i] == GUARD for i in range(max(cap, 0), max(cap, 0) + 64)))   # nothing past what it was given
        self.assertLessEqual(k, max(cap, 0))
        return (bytes(o)[:4 * k] if k else None), ns.value

    def wide(self, data):
        n = len(data) // 4; w = (ctypes.c_uint32 * n).from_buffer_copy(data)
        s = (ctypes.c_uint32 * (2 * w[3]))(); need = ctypes.c_size_t(); what = ctypes.c_int()
        self.lib.wide(w, n, ADRENO_830, s, None, 0, ctypes.byref(need), ctypes.byref(what))
        o = (ctypes.c_uint32 * need.value)(); k = self.lib.wide(w, n, ADRENO_830, s, o, need.value, ctypes.byref(need), ctypes.byref(what))
        return what.value, bytes(o)[:4 * k] if k else None

    def test_every_form_becomes_shufflexor(self):   # SHFL.BFLY through the helper's locals, SHFL.DOWN as written directly, SHFL.IDX in a fragment shader
        for name, text in (('bfly', COMPUTE), ('down', DOWN), ('idx', IDX_ONLY), ('fs', FS)):
            with self.subTest(name):
                data = self.asm(text, name); before = self.dis(data, name + '0').count('OpGroupNonUniformShuffle ')
                out, ns = self.shx(data)
                assert out is not None; self.assertEqual(ns, before); self.assertGreater(ns, 0)
                ok, err = self.valid(out, name); self.assertTrue(ok, err)
                d = self.dis(out, name)
                self.assertNotIn('OpGroupNonUniformShuffle ', d); self.assertEqual(d.count('OpGroupNonUniformShuffleXor'), before + self.dis(data, name + '1').count('OpGroupNonUniformShuffleXor'))
                self.assertEqual(int.from_bytes(out[12:16], 'little'), int.from_bytes(data[12:16], 'little') + 2 * ns)   # two new ids each
                # each ShuffleXor's mask is the lane XOR the old index
                lines = d.split('\n')
                for sx in (x for x in lines if 'OpGroupNonUniformShuffleXor' in x):
                    mask = sx.split()[-1]
                    self.assertTrue(any(x.strip().startswith(f'{mask} = OpBitwiseXor') for x in lines), sx)

    def test_after_spv_wide(self):   # the replay runs it on spv_wide's output, whose shuffles name lanes of the right 32-lane warp
        what, wided = self.wide(self.asm(FS, 'w'))
        self.assertEqual(what, DONE); assert wided is not None
        out, ns = self.shx(wided)
        self.assertIsNotNone(out); self.assertGreater(ns, 0)
        ok, err = self.valid(out, 'w'); self.assertTrue(ok, err)
        self.assertNotIn('OpGroupNonUniformShuffle ', self.dis(out, 'w'))

    def test_left_as_it_is(self):
        out, ns = self.shx(self.asm(NO_LANE, 'nolane')); self.assertIsNone(out); self.assertEqual(ns, 0)   # no lane built-in
        out, _ = self.shx(self.asm(COMPUTE, 'short'), extra=1); self.assertIsNone(out)   # one word short of room
        noshuffle = COMPUTE.replace('%sh = OpGroupNonUniformShuffle %f32 %scope %v1 %t1', '%sh = OpCopyObject %f32 %v1')
        out, ns = self.shx(self.asm(noshuffle, 'none')); self.assertIsNone(out); self.assertEqual(ns, 0)
        self.assertEqual(self.lib.count((ctypes.c_uint32 * 4)(0, 0, 0, 0), 4), 0)   # no module

    def test_a_games_modules(self):   # DM_SPV_DIR: each rewritten module passes spirv-val and has no plain shuffle left
        d = os.environ.get('DM_SPV_DIR')
        if not d: self.skipTest('DM_SPV_DIR is not set')
        n = done = sites = bad = left = 0
        for p in sorted(Path(d).glob('*.spv')):
            data = p.read_bytes()
            if len(data) % 4 or len(data) < 20: continue
            n += 1
            w = (ctypes.c_uint32 * (len(data) // 4)).from_buffer_copy(data)
            if not self.lib.count(w, len(w)): continue   # no shuffle
            out, ns = self.shx(data)
            if not out: left += 1; print(p.name, 'left'); continue
            done += 1; sites += ns
            ok, err = self.valid(out, 'corpus')
            if not ok: bad += 1; print(p.name, err[:200])
        print(f'{n} modules, {done} rewritten ({sites} shuffles), {left} with a shuffle left, {bad} invalid')
        self.assertEqual(bad, 0); self.assertEqual(left, 0)

    @unittest.skipUnless(os.environ.get('DM_TEST_SERIAL'), 'DM_TEST_SERIAL not set')
    def test_lanes_on_a_device(self):
        serial = os.environ['DM_TEST_SERIAL']
        adb = shutil.which('adb') or str(Path(os.environ.get('ANDROID_HOME', '')) / 'platform-tools' / f'adb{EXE}')
        exe = self.tmp / 't_shx'
        cc = self.bin / ('aarch64-linux-android29-clang' + ('.cmd' if os.name == 'nt' else ''))
        subprocess.run([cc, '-O1', '-Wall', '-Werror', REPO / 'dm/android/tests/t_shx.c', '-lvulkan', '-o', exe], check=True)
        g = self.tmp / 'st.comp'; g.write_text(SELFTEST_GLSL)
        subprocess.run([self.glslc, '--target-env=vulkan1.1', g, '-o', self.tmp / 'st.spv'], check=True)
        out, ns = self.shx((self.tmp / 'st.spv').read_bytes()); assert out is not None; self.assertEqual(ns, 8)
        (self.tmp / 'stx.spv').write_bytes(out)
        d = '/data/local/tmp/dm_shx'
        subprocess.run([adb, '-s', serial, 'shell', 'mkdir', '-p', d], check=True)
        for f in (exe, self.tmp / 'st.spv', self.tmp / 'stx.spv'): subprocess.run([adb, '-s', serial, 'push', f, d], check=True, capture_output=True)
        def on_device(cs):
            r = subprocess.run([adb, '-s', serial, 'shell', f'cd {d} && chmod 755 t_shx && ./t_shx {cs}; echo "exit $?"'], capture_output=True, text=True)
            print(r.stdout.strip()); return r.stdout.strip()
        compiled, rewritten = on_device('st.spv'), on_device('stx.spv')
        subprocess.run([adb, '-s', serial, 'shell', 'rm', '-r', d], capture_output=True)
        self.assertTrue(rewritten.endswith('exit 0'), rewritten)
        self.assertTrue(compiled.endswith(('exit 0', 'exit 139')), compiled)   # 139: the compiler died on the plain shuffle


if __name__ == '__main__':
    unittest.main()
