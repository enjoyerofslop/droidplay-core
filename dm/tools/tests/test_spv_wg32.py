"""dm/android/spv_wg32.h (a task / mesh kernel's 32-lane warp as its whole workgroup on a GPU whose subgroups are narrower, as the Pixel 9 Pro XL's Mali-G715
with 16 lanes), built for the PC and run on kernels assembled with the NDK's spirv-as in the compute form's shape: an entry point that zeroes workgroup memory,
waits at a barrier and calls the translated body, which reads the lane ID and a lane mask, packs with a ballot, an any and a shuffle, then lets the lanes past a
count return and aggregates an atomic over the lanes left (a ballot of true, its leader's add, a broadcast). spirv-val checks each result, and a guard after the
words it asked for checks that it writes no further. Skipped without the NDK.
With DM_SPV_DIR=<a game's packed spv folder>, also run over every kernel there (<key>.cs.spv) with 16-lane subgroups (each result must pass spirv-val wherever
the kernel itself does).
With DM_TEST_SERIAL=<adb serial>, dm/android/tests/t_wg32.c is built for Android and run on that device with a kernel of one 32-lane warp (a ballot and its prefix
under the Lt mask, an any, a shuffle, then a warp-aggregated atomic after the lanes from 20 up return): rewritten for the device's subgroup size where it is under
32, every lane's results must be a 32-lane warp's; where it is 32 or more, as translated (spv_wg32 is not used there)."""
import ctypes
import os
import re
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_spv_readonly import NDK, HOST, EXE, REPO   # noqa: E402

LANE, SITES, PART, OTHER = 1, 2, 4, 8   # SPV_WG32_*
GUARD = 0xa5a5a5a5
BALLOT, ANY, SHUFFLE, BARRIER, BOR, LOAD = 339, 335, 345, 224, 360, 61

KERNEL = '''OpCapability Shader
OpCapability GroupNonUniform
OpCapability GroupNonUniformVote
OpCapability GroupNonUniformBallot
OpCapability GroupNonUniformShuffle
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main" %lane %lt %li %wg
OpExecutionMode %main LocalSize 32 1 1
OpDecorate %lane BuiltIn SubgroupLocalInvocationId
OpDecorate %lt BuiltIn SubgroupLtMask
OpDecorate %li BuiltIn LocalInvocationIndex
OpDecorate %wg BuiltIn WorkgroupId
OpDecorate %Blk Block
OpMemberDecorate %Blk 0 Offset 0
OpDecorate %arr ArrayStride 4
OpDecorate %buf DescriptorSet 0
OpDecorate %buf Binding 0
%void = OpTypeVoid
%fn = OpTypeFunction %void
%bool = OpTypeBool
%u32 = OpTypeInt 32 0
%f32 = OpTypeFloat 32
%v3u = OpTypeVector %u32 3
%v4u = OpTypeVector %u32 4
%pu = OpTypePointer Input %u32
%pv3 = OpTypePointer Input %v3u
%pv4 = OpTypePointer Input %v4u
%lane = OpVariable %pu Input
%lt = OpVariable %pv4 Input
%li = OpVariable %pu Input
%wg = OpVariable %pv3 Input
%arr = OpTypeRuntimeArray %u32
%Blk = OpTypeStruct %arr
%pBlk = OpTypePointer StorageBuffer %Blk
%buf = OpVariable %pBlk StorageBuffer
%psu = OpTypePointer StorageBuffer %u32
%pfu = OpTypePointer Function %u32
%u0 = OpConstant %u32 0
%u1 = OpConstant %u32 1
%u2 = OpConstant %u32 2
%u3 = OpConstant %u32 3
%u5 = OpConstant %u32 5
%u20 = OpConstant %u32 20
%u31 = OpConstant %u32 31
%u64 = OpConstant %u32 64
%u264 = OpConstant %u32 264
%true = OpConstantTrue %bool
%n16 = OpConstant %u32 16
%wa = OpTypeArray %u32 %n16
%pwa = OpTypePointer Workgroup %wa
%pwu = OpTypePointer Workgroup %u32
%shm = OpVariable %pwa Workgroup
%main = OpFunction %void None %fn
%m0 = OpLabel
%i0 = OpLoad %u32 %li
%z = OpAccessChain %pwu %shm %i0
%lo = OpULessThan %bool %i0 %n16
OpSelectionMerge %m1 None
OpBranchConditional %lo %m2 %m1
%m2 = OpLabel
OpStore %z %u0
OpBranch %m1
%m1 = OpLabel
OpControlBarrier %u2 %u2 %u264
%call = OpFunctionCall %void %body
OpControlBarrier %u2 %u2 %u264
OpReturn
OpFunctionEnd
%body = OpFunction %void None %fn
%b0 = OpLabel
%R0 = OpVariable %pfu Function
%R1 = OpVariable %pfu Function
%ln = OpLoad %u32 %lane
%g3 = OpLoad %v3u %wg
%g = OpCompositeExtract %u32 %g3 0
%g5 = OpShiftLeftLogical %u32 %g %u5
%idx = OpIAdd %u32 %g5 %ln
OpStore %R0 %idx
%r0 = OpLoad %u32 %R0
%p = OpULessThan %bool %r0 %u20
%bl = OpGroupNonUniformBallot %v4u %u3 %p
%bx = OpCompositeExtract %u32 %bl 0
%ltp = OpAccessChain %pu %lt %u0
%ltm = OpLoad %u32 %ltp
%below = OpBitwiseAnd %u32 %bx %ltm
%pre = OpBitCount %u32 %below
%any = OpGroupNonUniformAny %bool %u3 %p
%anyu = OpSelect %u32 %any %u1 %u0
%sum = OpIAdd %u32 %pre %anyu
%nx = OpIAdd %u32 %ln %u1
%src = OpBitwiseAnd %u32 %nx %u31
%vf = OpConvertUToF %f32 %idx
%sf = OpGroupNonUniformShuffle %f32 %u3 %vf %src
%su = OpConvertFToU %u32 %sf
%out = OpIAdd %u32 %sum %su
%slot = OpAccessChain %psu %buf %u0 %r0
OpStore %slot %out
%cnt = OpAccessChain %psu %buf %u0 %u0
%cv = OpLoad %u32 %cnt
%past = OpUGreaterThanEqual %bool %r0 %cv
OpSelectionMerge %b2 None
OpBranchConditional %past %b1 %b2
%b1 = OpLabel
OpReturn
%b2 = OpLabel
%act = OpGroupNonUniformBallot %v4u %u3 %true
%ax = OpCompositeExtract %u32 %act 0
%lead = OpBitFieldUExtract %u32 %ax %u0 %u5
OpStore %R1 %lead
%le = OpLoad %u32 %R1
%isl = OpIEqual %bool %le %ln
%base = OpAccessChain %psu %buf %u0 %u64
OpSelectionMerge %b4 None
OpBranchConditional %isl %b3 %b4
%b3 = OpLabel
%pc = OpBitCount %u32 %ax
%old = OpAtomicIAdd %u32 %base %u1 %u0 %pc
OpBranch %b4
%b4 = OpLabel
%lf = OpConvertUToF %f32 %le
%bc = OpGroupNonUniformShuffle %f32 %u3 %lf %u0
%bcu = OpConvertFToU %u32 %bc
%s2 = OpAccessChain %psu %buf %u0 %u31
OpStore %s2 %bcu
OpReturn
OpFunctionEnd
'''

# The device test's kernel (t_wg32.c reads 129 words: a ballot, the count below, an any and a shuffle for each lane, then the aggregated add)
DEVICE = '''OpCapability Shader
OpCapability GroupNonUniform
OpCapability GroupNonUniformVote
OpCapability GroupNonUniformBallot
OpCapability GroupNonUniformShuffle
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main" %lane %lt %li
OpExecutionMode %main LocalSize 32 1 1
OpDecorate %lane BuiltIn SubgroupLocalInvocationId
OpDecorate %lt BuiltIn SubgroupLtMask
OpDecorate %li BuiltIn LocalInvocationIndex
OpDecorate %Blk Block
OpMemberDecorate %Blk 0 Offset 0
OpDecorate %arr ArrayStride 4
OpDecorate %buf DescriptorSet 0
OpDecorate %buf Binding 0
%void = OpTypeVoid
%fn = OpTypeFunction %void
%bool = OpTypeBool
%u32 = OpTypeInt 32 0
%v4u = OpTypeVector %u32 4
%pu = OpTypePointer Input %u32
%pv4 = OpTypePointer Input %v4u
%lane = OpVariable %pu Input
%lt = OpVariable %pv4 Input
%li = OpVariable %pu Input
%arr = OpTypeRuntimeArray %u32
%Blk = OpTypeStruct %arr
%pBlk = OpTypePointer StorageBuffer %Blk
%buf = OpVariable %pBlk StorageBuffer
%psu = OpTypePointer StorageBuffer %u32
%u0 = OpConstant %u32 0
%u1 = OpConstant %u32 1
%u3 = OpConstant %u32 3
%u7 = OpConstant %u32 7
%u17 = OpConstant %u32 17
%u20 = OpConstant %u32 20
%u31 = OpConstant %u32 31
%u32c = OpConstant %u32 32
%u64 = OpConstant %u32 64
%u96 = OpConstant %u32 96
%u128 = OpConstant %u32 128
%true = OpConstantTrue %bool
%main = OpFunction %void None %fn
%e = OpLabel
%ln = OpLoad %u32 %lane
%m3 = OpUMod %u32 %ln %u3
%p = OpIEqual %bool %m3 %u0
%bl = OpGroupNonUniformBallot %v4u %u3 %p
%bx = OpCompositeExtract %u32 %bl 0
%ltp = OpAccessChain %pu %lt %u0
%ltm = OpLoad %u32 %ltp
%bm = OpBitwiseAnd %u32 %bx %ltm
%pre = OpBitCount %u32 %bm
%is31 = OpIEqual %bool %ln %u31
%any = OpGroupNonUniformAny %bool %u3 %is31
%au = OpSelect %u32 %any %u1 %u0
%v = OpIMul %u32 %ln %u7
%s17 = OpIAdd %u32 %ln %u17
%src = OpBitwiseAnd %u32 %s17 %u31
%sh = OpGroupNonUniformShuffle %u32 %u3 %v %src
%o0 = OpAccessChain %psu %buf %u0 %ln
OpStore %o0 %bx
%i1 = OpIAdd %u32 %ln %u32c
%o1 = OpAccessChain %psu %buf %u0 %i1
OpStore %o1 %pre
%i2 = OpIAdd %u32 %ln %u64
%o2 = OpAccessChain %psu %buf %u0 %i2
OpStore %o2 %au
%i3 = OpIAdd %u32 %ln %u96
%o3 = OpAccessChain %psu %buf %u0 %i3
OpStore %o3 %sh
%gone = OpUGreaterThanEqual %bool %ln %u20
OpSelectionMerge %stay None
OpBranchConditional %gone %ret %stay
%ret = OpLabel
OpReturn
%stay = OpLabel
%act = OpGroupNonUniformBallot %v4u %u3 %true
%ax = OpCompositeExtract %u32 %act 0
%lw = OpBitwiseAnd %u32 %ax %ltm
%low = OpIEqual %bool %lw %u0
OpSelectionMerge %done None
OpBranchConditional %low %add %done
%add = OpLabel
%cnt = OpBitCount %u32 %ax
%ag = OpAccessChain %psu %buf %u0 %u128
%old = OpAtomicIAdd %u32 %ag %u1 %u0 %cnt
OpBranch %done
%done = OpLabel
OpReturn
OpFunctionEnd
'''


def ops(data):
    w = [int.from_bytes(data[i:i + 4], 'little') for i in range(0, len(data), 4)]; i = 5; o = []
    while i < len(w): o.append(w[i] & 0xffff); i += w[i] >> 16
    return o


class SpvWg32(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if NDK is None: raise unittest.SkipTest('no Android NDK')
        cls.tmp = Path(tempfile.mkdtemp())
        tools = NDK / 'shader-tools' / HOST; cls.spv_as, cls.spv_val = tools / f'spirv-as{EXE}', tools / f'spirv-val{EXE}'
        cls.bin = NDK / 'toolchains/llvm/prebuilt' / HOST / 'bin'
        src = cls.tmp / 'w.c'
        export = '__declspec(dllexport)' if os.name == 'nt' else '__attribute__((visibility("default")))'
        src.write_text(f'#include "spv_wg32.h"\n'
                       f'{export} size_t run(const uint32_t *w, size_t n, uint32_t size, const uint8_t *div, uint32_t *sc, uint32_t *o, size_t cap, size_t *need, int *what, '
                       f'int *made, int *part) {{ return spv_wg32(w, n, size, div, sc, o, cap, need, what, made, part); }}\n'
                       f'{export} int divs(const uint32_t *w, size_t n, uint8_t *div, uint32_t *sc) {{ return spv_wg32_div(w, n, div, sc); }}\n'
                       f'{export} size_t divsc(uint32_t bound, size_t n) {{ return SPV_WG32_DIV_SC(bound, n); }}\n'
                       f'{export} int wanted(const uint32_t *w, size_t n) {{ return spv_wg32_wanted(w, n); }}\n')
        env = dict(os.environ, MSYS_NO_PATHCONV='1')
        if os.name == 'nt':   # no C library: a DLL linked by lld alone, as test_spv_wide
            subprocess.run([cls.bin / 'clang.exe', '--target=x86_64-pc-windows-msvc', '-ffreestanding', '-nostdlib', '-O1', '-Wall', '-Werror', '-I', REPO / 'dm/android', '-c', src,
                            '-o', cls.tmp / 'w.obj'], check=True)
            subprocess.run([cls.bin / 'ld.lld.exe', '-flavor', 'link', '-dll', '-noentry', f'-out:{cls.tmp / "w.dll"}', str(cls.tmp / 'w.obj')], check=True, env=env)
            cls.lib = ctypes.CDLL(str(cls.tmp / 'w.dll'))
        else:
            cc = ['xcrun', 'clang'] if sys.platform == 'darwin' else [shutil.which('clang') or cls.bin / 'clang']
            subprocess.run([*cc, '-shared', '-fPIC', '-O1', '-Wall', '-Wextra', '-Werror', '-I', REPO / 'dm/android', src, '-o', cls.tmp / 'w.so'], check=True)
            cls.lib = ctypes.CDLL(str(cls.tmp / 'w.so'))
        for f, r in (('run', ctypes.c_size_t), ('divsc', ctypes.c_size_t), ('divs', ctypes.c_int), ('wanted', ctypes.c_int)): getattr(cls.lib, f).restype = r
        cls.lib.run.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p,
                                ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
        cls.lib.divs.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_void_p]
        cls.lib.divsc.argtypes = [ctypes.c_uint32, ctypes.c_size_t]
        cls.lib.wanted.argtypes = [ctypes.c_void_p, ctypes.c_size_t]

    def div(self, w, n):   # → the per-id divergence bytes, and whether the analysis could tell
        d = (ctypes.c_uint8 * w[3])(); sc = (ctypes.c_uint32 * self.lib.divsc(w[3], n))()
        return d, self.lib.divs(w, n, d, sc)

    def run_on(self, data, size=16, analyse=True):   # → (the rewritten module or None, SPV_WG32_* bits, sites made workgroup-wide, sites per subgroup)
        n = len(data) // 4; w = (ctypes.c_uint32 * n).from_buffer_copy(data)
        d, ok = self.div(w, n) if analyse else (None, 0)
        if analyse: self.assertEqual(ok, 1)
        sc = (ctypes.c_uint32 * (2 * w[3]))(); need, what, made, part = ctypes.c_size_t(), ctypes.c_int(), ctypes.c_int(), ctypes.c_int()
        self.assertEqual(self.lib.run(w, n, size, d, sc, None, 0, ctypes.byref(need), ctypes.byref(what), ctypes.byref(made), ctypes.byref(part)), 0)
        if need.value: self.assertTrue(self.lib.wanted(w, n))   # the replay's pre-check never passes over a module that needs it
        if not need.value: return None, what.value, 0, 0
        cap = need.value; o = (ctypes.c_uint32 * (cap + 64))(*[GUARD] * (cap + 64))
        k = self.lib.run(w, n, size, d, sc, o, cap, ctypes.byref(need), ctypes.byref(what), ctypes.byref(made), ctypes.byref(part))
        self.assertTrue(0 < k <= cap)
        self.assertEqual(list(o[cap:]), [GUARD] * 64, 'written past the words it asked for')
        return bytes(o)[:4 * k], what.value, made.value, part.value

    def val(self, data, name):
        p = self.tmp / name; p.write_bytes(data)
        r = subprocess.run([self.spv_val, '--target-env', 'vulkan1.1', p], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    def asm(self, text, name):
        p = self.tmp / f'{name}.spvasm'; p.write_text(text)
        subprocess.run([self.spv_as, '--target-env', 'vulkan1.1', p, '-o', self.tmp / f'{name}.spv'], check=True)
        data = (self.tmp / f'{name}.spv').read_bytes(); self.val(data, f'{name}_in.spv')
        return data

    def test_kernel(self):
        data = self.asm(KERNEL, 'k')
        out, what, made, part = self.run_on(data)
        self.assertEqual(what, LANE | SITES | PART)
        self.assertEqual((made, part), (3, 2))   # the ballot, the any and the shuffle before the return; the ballot of true and the broadcast after it
        self.val(out, 'k_out.spv')
        o, i = ops(out), ops(data)
        self.assertEqual(o.count(BALLOT), 0); self.assertEqual(o.count(ANY), 0)
        self.assertEqual(o.count(SHUFFLE), 1)   # the broadcast after the return: from the lane's own subgroup
        self.assertEqual(o.count(BOR), 3)   # each ballot and any: the subgroup's OR of its lanes' bits
        self.assertEqual(o.count(BARRIER), i.count(BARRIER) + 2 * 3)   # two for each site every invocation reaches, none after the return

    def test_divergence(self):   # the blocks after the lanes past the count return, and the leader's, may hold some lanes; the rest every lane
        data = self.asm(KERNEL, 'kd'); n = len(data) // 4; w = (ctypes.c_uint32 * n).from_buffer_copy(data)
        d, ok = self.div(w, n); self.assertEqual(ok, 1)
        assert NDK is not None   # setUpClass skips without it
        dis = subprocess.run([NDK / 'shader-tools' / HOST / f'spirv-dis{EXE}', '--raw-id', self.tmp / 'kd.spv'], capture_output=True, text=True).stdout
        raw = [l.split()[0] for l in dis.splitlines() if l.strip().endswith('= OpLabel')]
        nam = [l.split()[0] for l in KERNEL.splitlines() if l.endswith('= OpLabel')]   # (the same order)
        div = {a: d[int(b[1:])] for a, b in zip(nam, raw)}
        self.assertEqual(div, {'%m0': 0, '%m2': 1, '%m1': 0, '%b0': 0, '%b1': 1, '%b2': 1, '%b3': 1, '%b4': 1})

    def test_without_analysis(self):   # every site per subgroup: no barrier added
        data = self.asm(KERNEL, 'kn')
        out, what, made, part = self.run_on(data, analyse=False)
        self.assertEqual((what, made, part), (LANE | SITES, 5, 0))   # (div NULL: every block as one every lane reaches)
        out2 = None
        n = len(data) // 4; w = (ctypes.c_uint32 * n).from_buffer_copy(data); d = (ctypes.c_uint8 * w[3])(*[1] * w[3])
        sc = (ctypes.c_uint32 * (2 * w[3]))(); need, wh, m, p = ctypes.c_size_t(), ctypes.c_int(), ctypes.c_int(), ctypes.c_int()
        self.lib.run(w, n, 16, d, sc, None, 0, ctypes.byref(need), ctypes.byref(wh), ctypes.byref(m), ctypes.byref(p))
        o = (ctypes.c_uint32 * need.value)(); k = self.lib.run(w, n, 16, d, sc, o, need.value, ctypes.byref(need), ctypes.byref(wh), ctypes.byref(m), ctypes.byref(p))
        out2 = bytes(o)[:4 * k]
        self.assertEqual((wh.value, m.value, p.value), (LANE | PART, 0, 5))
        self.val(out, 'kn_out.spv'); self.val(out2, 'kn_out2.spv')
        self.assertEqual(ops(out2).count(BARRIER), ops(data).count(BARRIER))

    def test_eight_lanes(self):   # four subgroups: four words read for each ballot
        out, what, made, part = self.run_on(self.asm(KERNEL, 'k8'), size=8)
        self.assertEqual((what, made, part), (LANE | SITES | PART, 3, 2))
        self.val(out, 'k8_out.spv')

    def test_left_as_it_is(self):
        data = self.asm(KERNEL, 'kl')
        self.assertEqual(self.run_on(data, size=32)[:2], (None, 0))   # 32 lanes: not its work (the replay does not call it)
        wide = self.asm(KERNEL.replace('LocalSize 32 1 1', 'LocalSize 64 1 1'), 'k64')   # two warps a workgroup (a packed kernel): not this
        self.assertEqual(self.run_on(wide)[:2], (None, 0))
        n = len(wide) // 4; self.assertFalse(self.lib.wanted((ctypes.c_uint32 * n).from_buffer_copy(wide), n))
        bf = KERNEL.replace('%bcu = OpConvertFToU %u32 %bc\n', '%bf = OpGroupNonUniformBroadcastFirst %f32 %u3 %bc\n%bcu = OpConvertFToU %u32 %bf\n')
        self.assertEqual(self.run_on(self.asm(bf, 'kbf'))[:2], (None, OTHER))   # an operation it does not take: the module as it is
        sz = KERNEL.replace('OpEntryPoint GLCompute %main "main" %lane %lt %li %wg', 'OpEntryPoint GLCompute %main "main" %lane %lt %li %wg %ssz').replace(
            'OpDecorate %wg BuiltIn WorkgroupId\n', 'OpDecorate %wg BuiltIn WorkgroupId\nOpDecorate %ssz BuiltIn SubgroupSize\n').replace(
            '%wg = OpVariable %pv3 Input\n', '%wg = OpVariable %pv3 Input\n%ssz = OpVariable %pu Input\n')
        self.assertEqual(self.run_on(self.asm(sz, 'ksz'))[:2], (None, OTHER))   # the game reads the subgroup's size

    @unittest.skipUnless(os.environ.get('DM_TEST_SERIAL'), 'DM_TEST_SERIAL not set')
    def test_on_a_device(self):
        serial = os.environ['DM_TEST_SERIAL']
        adb = shutil.which('adb') or str(Path(os.environ.get('ANDROID_HOME', '')) / 'platform-tools' / f'adb{EXE}')
        exe = self.tmp / 't_wg32'
        cc = self.bin / ('aarch64-linux-android29-clang' + ('.cmd' if os.name == 'nt' else ''))
        subprocess.run([cc, '-O1', '-Wall', '-Werror', REPO / 'dm/android/tests/t_wg32.c', '-lvulkan', '-o', exe], check=True)
        d = '/data/local/tmp/dm_wg32'
        subprocess.run([adb, '-s', serial, 'shell', 'mkdir', '-p', d], check=True)
        data = self.asm(DEVICE, 'dev'); (self.tmp / 'dev_in.spv').write_bytes(data)
        for f in (exe, self.tmp / 'dev_in.spv'): subprocess.run([adb, '-s', serial, 'push', f, d], check=True, capture_output=True)
        def on_device(cs):
            r = subprocess.run([adb, '-s', serial, 'shell', f'cd {d} && chmod 755 t_wg32 && ./t_wg32 {cs}; echo "exit $?"'], capture_output=True, text=True)
            print(r.stdout.strip()); return r.stdout
        translated = on_device('dev_in.spv')
        m = re.search(r'subgroup size (\d+)', translated); assert m, translated
        size = int(m.group(1))
        if size < 32:
            out, what, made, part = self.run_on(data, size=size)
            self.assertEqual((made, part), (3, 1))   # the ballot, any and shuffle before the return; the ballot of true after it
            assert out is not None, what
            (self.tmp / 'dev_out.spv').write_bytes(out); subprocess.run([adb, '-s', serial, 'push', self.tmp / 'dev_out.spv', d], check=True, capture_output=True)
            result = on_device('dev_out.spv')
        else: result = translated
        subprocess.run([adb, '-s', serial, 'shell', 'rm', '-r', d], capture_output=True)
        self.assertTrue(result.strip().endswith('exit 0'), result)

    @unittest.skipUnless(os.environ.get('DM_SPV_DIR'), 'DM_SPV_DIR not set')
    def test_a_games_kernels(self):
        d = Path(os.environ['DM_SPV_DIR']); n = done = bad = made = part = 0
        for p in sorted(d.glob('*.cs.spv')):
            data = p.read_bytes(); out, what, m, pt = self.run_on(data)
            n += 1
            if out is None: continue
            done += 1; made += m; part += pt
            if subprocess.run([self.spv_val, '--target-env', 'vulkan1.1', p], capture_output=True).returncode: continue   # the kernel itself fails: not this rewrite's
            q = self.tmp / 'game_out.spv'; q.write_bytes(out)
            r = subprocess.run([self.spv_val, '--target-env', 'vulkan1.1', q], capture_output=True, text=True)
            if r.returncode: bad += 1; print(p.name, r.stdout[:300])
        print(f'{n} kernels, {done} rewritten: {made} sites through workgroup memory, {part} per subgroup; {bad} failed spirv-val')
        self.assertEqual(bad, 0)


if __name__ == '__main__':
    unittest.main()
