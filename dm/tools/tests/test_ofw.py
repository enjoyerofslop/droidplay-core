"""dm/android/ofw.h (storage buffers a draw writes in an order-free way: which bindings, the identity their copy starts as, how it lands), built for the PC with its C
compiler; the scan's tests run on modules assembled by spirv-as (the Android NDK's, or one on PATH) and are skipped without it.
With DM_SPV_DIR=<a game's packed spv folder>, also run over every shader there and print how many written bindings of each stage are order-free."""
import collections
import ctypes
import json
import os
from pathlib import Path
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
CC = (['xcrun', 'clang'] if sys.platform == 'darwin' and shutil.which('xcrun') else
      [c] if (c := shutil.which('clang') or shutil.which('cc') or shutil.which('gcc')) else None)


def spirv_as():
    for c in (os.environ.get('ANDROID_NDK_ROOT'), *(Path(s) / 'ndk' for s in (os.environ.get('ANDROID_HOME'), os.environ.get('ANDROID_SDK_ROOT')) if s)):
        if c and Path(c).is_dir():
            vs = sorted(Path(c).iterdir()) if not (Path(c) / 'shader-tools').is_dir() else [Path(c)]
            for v in reversed(vs):
                for h in ('linux-x86_64', 'darwin-x86_64'):
                    if (v / 'shader-tools' / h / 'spirv-as').is_file(): return v / 'shader-tools' / h / 'spirv-as'
    w = shutil.which('spirv-as')
    return Path(w) if w else None


SPIRV_AS = spirv_as()
NONE, STORE, ADD, UMIN, UMAX, SMIN, SMAX, AND, OR, XOR = range(10)

# A fragment shader as Ryujinx's translator writes them: a storage buffer (Uniform + BufferBlock, uint[]) at binding 4, an index from an input, and BODY using %p
# (a pointer to the indexed word), %c1 / %c2 (uint constants 1 and 2), %v (a uint value), %loc (a Function variable) and %one (a float constant whose word is 1.0f)
FS = '''OpCapability Shader
%ext = OpExtInstImport "GLSL.std.450"
OpMemoryModel Logical GLSL450
OpEntryPoint Fragment %main "main" %in
OpExecutionMode %main OriginUpperLeft
OpDecorate %in Location 0
OpDecorate %buf BufferBlock
OpMemberDecorate %buf 0 Offset 0
OpDecorate %arr ArrayStride 4
OpDecorate %sb DescriptorSet 1
OpDecorate %sb Binding 4
%void = OpTypeVoid
%fn = OpTypeFunction %void
%u32 = OpTypeInt 32 0
%i32 = OpTypeInt 32 1
%f32 = OpTypeFloat 32
%arr = OpTypeRuntimeArray %u32
%buf = OpTypeStruct %arr
%pbuf = OpTypePointer Uniform %buf
%sb = OpVariable %pbuf Uniform
%pu = OpTypePointer Uniform %u32
%pin = OpTypePointer Input %f32
%in = OpVariable %pin Input
%pfu = OpTypePointer Function %u32
%i0 = OpConstant %i32 0
%c1 = OpConstant %u32 1
%c2 = OpConstant %u32 2
%cdev = OpConstant %u32 1
%csem = OpConstant %u32 0
%one = OpConstant %f32 1
%main = OpFunction %void None %fn
%l = OpLabel
%loc = OpVariable %pfu Function
%f = OpLoad %f32 %in
%fl = OpExtInst %f32 %ext Floor %f
%v = OpConvertFToU %u32 %fl
%p = OpAccessChain %pu %sb %i0 %v
BODY
OpReturn
OpFunctionEnd
'''


NEEDS_AS = unittest.skipIf(SPIRV_AS is None, 'no spirv-as')


@unittest.skipIf(CC is None or os.name == 'nt', 'no C compiler')
class Ofw(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        assert CC is not None   # (skipped without one; pyright doesn't follow skipIf)
        cls.tmp = Path(tempfile.mkdtemp()); cls.spv_as = SPIRV_AS
        src = cls.tmp / 'o.c'
        src.write_text('#include "ofw.h"\n'
                       'int scan(const uint32_t *w, size_t n, uint32_t *s, ofw_bind_t *o, int cap) { return ofw_scan(w, n, s, o, cap); }\n'
                       'uint32_t identity(uint32_t c, uint32_t k) { return ofw_identity(c, k); }\n'
                       'size_t land(uint32_t c, uint32_t k, const uint32_t *copy, uint32_t *g, size_t nw) { size_t a, e; return ofw_span(c, k, copy, nw, &a, &e) ? ofw_land(c, k, copy, g, a, e) : 0; }\n')
        subprocess.run([*CC, '-shared', '-fPIC', '-O1', '-Wall', '-Wextra', '-Werror', '-I', str(REPO / 'dm/android'), str(src), '-o', str(cls.tmp / 'o.so')], check=True)
        cls.lib = ctypes.CDLL(str(cls.tmp / 'o.so'))
        cls.lib.land.restype = ctypes.c_size_t; cls.lib.identity.restype = ctypes.c_uint32

    class B(ctypes.Structure):
        _fields_ = [('binding', ctypes.c_uint32), ('cls', ctypes.c_uint32), ('k', ctypes.c_uint32)]

    def scan(self, data):   # → [(binding, class, constant)]
        n = len(data) // 4; w = (ctypes.c_uint32 * n).from_buffer_copy(data); bound = w[3]
        s = (ctypes.c_uint32 * (6 * bound))(); o = (self.B * 8)()
        k = self.lib.scan(w, n, s, o, 8)
        return [(o[i].binding, o[i].cls, o[i].k) for i in range(k)]

    def asm(self, body, name):
        p = self.tmp / f'{name}.spvasm'; p.write_text(FS.replace('BODY', body))
        subprocess.run([self.spv_as, '--target-env', 'vulkan1.1', p, '-o', self.tmp / f'{name}.spv'], check=True)
        return (self.tmp / f'{name}.spv').read_bytes()

    def cls_of(self, body, name):
        r = self.scan(self.asm(body, name)); self.assertEqual(len(r), 1); self.assertEqual(r[0][0], 4)
        return r[0][1], r[0][2]

    @NEEDS_AS
    def test_stores_of_one_constant(self):   # Tears of the Kingdom's grid: fp_s0[i] = 1u
        self.assertEqual(self.cls_of('OpStore %p %c1\n%p2 = OpAccessChain %pu %sb %i0 %i0\nOpStore %p2 %c1', 'store1'), (STORE, 1))

    @NEEDS_AS
    def test_a_binding_never_written_is_not(self):
        self.assertEqual(self.cls_of('%pf = OpTypePointer Uniform %f32', 'nostore')[0], NONE)   # (a type alone: nothing written)

    @NEEDS_AS
    def test_two_constants_are_not_order_free(self):
        self.assertEqual(self.cls_of('OpStore %p %c1\nOpStore %p %c2', 'store12')[0], NONE)

    @NEEDS_AS
    def test_a_computed_value_stored_is_not(self):
        self.assertEqual(self.cls_of('OpStore %p %v', 'storev')[0], NONE)

    @NEEDS_AS
    def test_a_load_makes_it_not(self):
        self.assertEqual(self.cls_of('%x = OpLoad %u32 %p\nOpStore %p %c1', 'load')[0], NONE)

    @NEEDS_AS
    def test_atomic_min_whose_old_value_goes_to_an_unread_local(self):   # the translator's pattern: OpStore %local %old, the local never loaded
        self.assertEqual(self.cls_of('%o = OpAtomicUMin %u32 %p %cdev %csem %v\nOpStore %loc %o', 'umin'), (UMIN, 0))

    @NEEDS_AS
    def test_atomic_whose_old_value_is_read_is_not(self):
        self.assertEqual(self.cls_of('%o = OpAtomicUMax %u32 %p %cdev %csem %v\nOpStore %loc %o\n%r = OpLoad %u32 %loc', 'umaxread')[0], NONE)
        self.assertEqual(self.cls_of('%o = OpAtomicIAdd %u32 %p %cdev %csem %v\n%q = OpIAdd %u32 %o %c1', 'addused')[0], NONE)

    @NEEDS_AS
    def test_add_family_and_mixed_operations(self):
        self.assertEqual(self.cls_of('%o = OpAtomicIAdd %u32 %p %cdev %csem %v\n%o2 = OpAtomicIIncrement %u32 %p %cdev %csem\n%o3 = OpAtomicISub %u32 %p %cdev %csem %c2', 'adds')[0], ADD)
        self.assertEqual(self.cls_of('%o = OpAtomicUMin %u32 %p %cdev %csem %v\n%o2 = OpAtomicUMax %u32 %p %cdev %csem %v', 'minmax')[0], NONE)
        self.assertEqual(self.cls_of('%o = OpAtomicUMin %u32 %p %cdev %csem %v\nOpStore %p %c1', 'minstore')[0], NONE)
        self.assertEqual(self.cls_of('%o = OpAtomicExchange %u32 %p %cdev %csem %v', 'xchg')[0], NONE)

    @NEEDS_AS
    def test_a_literal_equal_to_the_variable_id_is_not_a_use(self):   # FMin is GLSL.std.450 instruction 37: the scan reads only the operands that are ids
        data = bytearray(self.asm('OpStore %p %c1\n%m = OpExtInst %f32 %ext FMin %f %f', 'lit'))
        w = list(struct.unpack(f'<{len(data) // 4}I', data)); i = 5; sb = None
        while i < len(w):
            if w[i] & 0xffff == 71 and w[i + 2] == 33: sb = w[i + 1]
            i += w[i] >> 16
        i = 5
        while i < len(w):   # renumber: the variable takes id 37 (FMin's number), 37 takes its old id
            ln, op = w[i] >> 16, w[i] & 0xffff
            for a in range(1, ln):
                if op == 12 and a == 4: continue   # the instruction number itself stays 37
                if op in (43,) and a == 3: continue   # constants' values
                if w[i + a] == sb: w[i + a] = 37
                elif w[i + a] == 37: w[i + a] = sb
            i += ln
        w[3] = max(w[3], 38)   # (a small module's bound may not reach 37)
        r = self.scan(struct.pack(f'<{len(w)}I', *w))
        self.assertEqual(r, [(4, STORE, 1)])

    def test_land(self):
        def land(c, k, copy, guest):
            g = (ctypes.c_uint32 * len(guest))(*guest); cp = (ctypes.c_uint32 * len(copy))(*copy)
            n = self.lib.land(c, k, cp, g, len(copy)); return list(g), n
        idn = lambda c, k=0: self.lib.identity(c, k)
        self.assertEqual(land(STORE, 1, [idn(STORE, 1), 1, idn(STORE, 1), 1], [0, 0, 7, 1]), ([0, 1, 7, 1], 2))   # the words stored land, whatever the guest held
        self.assertEqual(land(UMIN, 0, [idn(UMIN), 5, 9], [3, 8, 4]), ([3, 5, 4], 2))
        self.assertEqual(land(UMAX, 0, [0, 5, 9], [3, 8, 4]), ([3, 8, 9], 2))
        self.assertEqual(land(SMIN, 0, [idn(SMIN), 0xfffffffe], [1, 1]), ([1, 0xfffffffe], 1))
        self.assertEqual(land(SMAX, 0, [idn(SMAX), 0xfffffffe], [0xffffff00, 0xffffff00]), ([0xffffff00, 0xfffffffe], 1))
        self.assertEqual(land(ADD, 0, [0, 3, 0xffffffff], [10, 10, 10]), ([10, 13, 9], 2))
        self.assertEqual(land(AND, 0, [idn(AND), 0xf0], [0xff, 0xff]), ([0xff, 0xf0], 1))
        self.assertEqual(land(OR, 0, [0, 0x100], [1, 1]), ([1, 0x101], 1))
        self.assertEqual(land(XOR, 0, [0, 3], [1, 1]), ([1, 2], 1))
        self.assertEqual(land(UMIN, 0, [idn(UMIN)] * 3, [1, 2, 3]), ([1, 2, 3], 0))   # nothing written: nothing lands

    def test_copies_in_draw_order_match_one_buffer(self):
        # draws of one operation share a copy; another operation starts a new copy; the copies land in order. That must equal the draws run one after another on
        # guest memory (stores of one constant per class, min / max / add / or)
        rnd = random.Random(7); N = 64
        ops = {STORE: lambda g, v, k: k, UMIN: lambda g, v, k: min(g, v), UMAX: lambda g, v, k: max(g, v), ADD: lambda g, v, k: (g + v) & 0xffffffff, OR: lambda g, v, k: g | v}
        for trial in range(200):
            guest = [rnd.randrange(1 << 32) for _ in range(N)]; ref = list(guest)
            draws = []
            for _ in range(rnd.randrange(1, 12)):
                c = rnd.choice(list(ops)); k = rnd.choice([1, 2]) if c == STORE else 0
                draws.append((c, k, [(rnd.randrange(N), rnd.randrange(1 << 32)) for _ in range(rnd.randrange(1, 10))]))
            for c, k, wr in draws:
                for i, v in wr: ref[i] = ops[c](ref[i], v, k)
            copies = []
            for c, k, wr in draws:
                if not copies or copies[-1][0] != (c, k): copies.append(((c, k), [self.lib.identity(c, k)] * N))
                cp = copies[-1][1]
                for i, v in wr: cp[i] = ops[c](cp[i], v, k)
            g = (ctypes.c_uint32 * N)(*guest)
            for (c, k), cp in copies: self.lib.land(c, k, (ctypes.c_uint32 * N)(*cp), g, N)
            self.assertEqual(list(g), ref, f'trial {trial}: {draws}')

    @unittest.skipUnless(os.environ.get('DM_SPV_DIR'), 'DM_SPV_DIR not set')
    def test_game_shaders(self):
        d = Path(os.environ['DM_SPV_DIR']); count = collections.Counter(); names = ['none', 'store', 'add', 'umin', 'umax', 'smin', 'smax', 'and', 'or', 'xor']
        for f in sorted(d.glob('*.spv')):
            if f.name.count('.') != 1: continue
            rj = d / f'{f.stem}.res.json'
            if not rj.is_file(): continue
            r = json.loads(rj.read_text()); wr = {b['binding'] for b in r.get('sbufs', []) if b.get('flags', 0) & 1}
            for b, c, _ in self.scan(f.read_bytes()):
                if b in wr: count[(r.get('stage'), names[c])] += 1
        for k, v in sorted(count.items()): print(k, v)


if __name__ == '__main__':
    unittest.main()
