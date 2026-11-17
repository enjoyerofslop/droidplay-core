"""dm/android/spv_vsfrag.h (a vertex shader that writes memory run as a fragment shader over points, for a GPU without vertex-stage storage writes: the
Mali-G715), built for the PC and run on modules assembled with the NDK's spirv-as; spirv-val checks each result. Skipped without the NDK.
With DM_SPV_DIR=<a game's packed spv folder>, also run over every vertex shader there (DM_SPV_WRITERS=1: only those whose res.json says they write memory)."""
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

# A skinning vertex shader as the translator writes one (Xenoblade Chronicles 2's 3fb8ebc13eb37ad7, cut down): vertex inputs at locations 0 (float) and 1 (uint),
# gl_VertexIndex, gl_PerVertex as a block, and a store of the vertex's value into a storage buffer at gl_VertexIndex
VS = '''OpCapability Shader
OpCapability DrawParameters
OpMemoryModel Logical GLSL450
OpEntryPoint Vertex %main "main" %in0 %in1 %vid %pv
OpDecorate %in0 Location 0
OpDecorate %in1 Location 1
OpDecorate %vid BuiltIn VertexIndex
OpDecorate %blk Block
OpMemberDecorate %blk 0 Invariant
OpMemberDecorate %blk 0 BuiltIn Position
OpMemberDecorate %blk 1 BuiltIn PointSize
OpMemberDecorate %blk 2 BuiltIn ClipDistance
OpMemberDecorate %blk 3 BuiltIn CullDistance
OpDecorate %rta ArrayStride 4
OpDecorate %sb BufferBlock
OpMemberDecorate %sb 0 Offset 0
OpDecorate %buf DescriptorSet 0
OpDecorate %buf Binding 3
%void = OpTypeVoid
%fn = OpTypeFunction %void
%float = OpTypeFloat 32
%v4 = OpTypeVector %float 4
%uint = OpTypeInt 32 0
%v4u = OpTypeVector %uint 4
%int = OpTypeInt 32 1
%i0 = OpConstant %int 0
%u8 = OpConstant %uint 8
%arr = OpTypeArray %float %u8
%blk = OpTypeStruct %v4 %float %arr %arr
%pblk = OpTypePointer Output %blk
%pv4o = OpTypePointer Output %v4
%pv4i = OpTypePointer Input %v4
%pv4ui = OpTypePointer Input %v4u
%pii = OpTypePointer Input %int
%rta = OpTypeRuntimeArray %uint
%sb = OpTypeStruct %rta
%psb = OpTypePointer Uniform %sb
%pu = OpTypePointer Uniform %uint
%in0 = OpVariable %pv4i Input
%in1 = OpVariable %pv4ui Input
%vid = OpVariable %pii Input
%pv = OpVariable %pblk Output
%buf = OpVariable %psb Uniform
%main = OpFunction %void None %fn
%l = OpLabel
%a = OpLoad %v4 %in0
%b = OpLoad %v4u %in1
%v = OpLoad %int %vid
%x = OpCompositeExtract %float %a 0
%xb = OpBitcast %uint %x
%y = OpCompositeExtract %uint %b 0
%s = OpIAdd %uint %xb %y
%p = OpAccessChain %pu %buf %i0 %v
OpStore %p %s
%pp = OpAccessChain %pv4o %pv %i0
OpStore %pp %a
OpReturn
OpFunctionEnd
'''


class Io(ctypes.Structure):
    _fields_ = [('inmask', ctypes.c_uint32), ('kind', ctypes.c_uint8 * 16), ('ncomp', ctypes.c_uint8 * 16), ('bi', ctypes.c_uint32), ('bikind', ctypes.c_uint8 * 5)]


def words(data): return struct.unpack('<%dI' % (len(data) // 4), data)


def ops(data):   # [(opcode, operands)]
    w = words(data); i = 5; out = []
    while i < len(w):
        n = w[i] >> 16; out.append((w[i] & 0xffff, w[i + 1:i + n])); i += max(n, 1)
    return out


@unittest.skipIf(NDK is None, 'no Android NDK')
class SpvVsFrag(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        assert NDK is not None
        cls.tmp = Path(tempfile.mkdtemp())
        tools = NDK / 'shader-tools' / HOST; cls.spv_as, cls.spv_val = tools / f'spirv-as{EXE}', tools / f'spirv-val{EXE}'
        cls.bin = NDK / 'toolchains/llvm/prebuilt' / HOST / 'bin'
        src = cls.tmp / 't.c'
        export = '__declspec(dllexport)' if os.name == 'nt' else '__attribute__((visibility("default")))'
        src.write_text(f'#include "spv_vsfrag.h"\n'
                       f'{export} size_t fs(const uint32_t *w, size_t n, uint32_t *s, uint32_t *o, size_t cap, spv_vsf_io *io) {{ return spv_vsf_fs(w, n, s, o, cap, io); }}\n'
                       f'{export} size_t vs(const spv_vsf_io *io, uint32_t v, uint32_t *o, size_t cap) {{ return spv_vsf_vs(io, v, o, cap); }}\n')
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
        cls.lib.fs.restype = cls.lib.vs.restype = ctypes.c_size_t

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

    def fs(self, data):   # → (the fragment shader, io) or (None, None)
        n = len(data) // 4; w = (ctypes.c_uint32 * n).from_buffer_copy(data); cap = n + 3 * 32 + 64; o = (ctypes.c_uint32 * cap)()
        sc = (ctypes.c_uint32 * (2 * w[3]))(); io = Io()
        k = self.lib.fs(w, n, sc, o, cap, ctypes.byref(io))
        return (bytes(o)[:4 * k], io) if k else (None, None)

    def vs(self, io, version=0x00010300):
        o = (ctypes.c_uint32 * 1024)(); k = self.lib.vs(ctypes.byref(io), version, o, 1024)
        return bytes(o)[:4 * k] if k else None

    def test_the_skinning_shader_becomes_a_fragment_shader(self):
        data = self.asm(VS, 'vs'); out, io = self.fs(data); self.assertIsNotNone(out); assert out is not None and io is not None
        self.valid(out, 'fs')
        o = ops(out)
        entry = [x for op, x in o if op == 15][0]
        self.assertEqual(entry[0], 4)   # Fragment
        orig = [x for op, x in ops(data) if op == 15][0][1]
        self.assertTrue([x for op, x in o if op == 57 and x[2] == orig])   # the wrapper calls the shader
        self.assertEqual([x[1] for op, x in o if op == 16], [7])   # OriginUpperLeft
        self.assertFalse([x for op, x in o if op == 72 and x[2] == 11])   # no output built-ins left
        self.assertFalse([x for op, x in o if op == 59 and x[2] == 3])   # no Output variable left
        self.assertIn(23, [x[2] for op, x in o if op == 71 and x[1] == 11])   # gl_HelperInvocation, for the wrapper
        self.assertEqual(sorted(x[2] for op, x in o if op == 71 and x[1] == 30), [0, 1, 16])   # the inputs at their locations, gl_VertexIndex at 16
        self.assertEqual(sum(1 for op, x in o if op == 71 and x[1] == 14), 3)   # all flat
        self.assertEqual(sum(1 for op, x in o if op == 62), 2)   # the shader's own stores, untouched
        self.assertEqual((io.inmask, chr(io.kind[0]), io.ncomp[0], chr(io.kind[1]), io.ncomp[1], io.bi, chr(io.bikind[0])), (0b11, 'f', 4, 'u', 4, 1, 's'))

    def test_the_pass_through_vertex_shader(self):
        _, io = self.fs(self.asm(VS, 'vs2')); assert io is not None
        out = self.vs(io); self.assertIsNotNone(out); assert out is not None
        self.valid(out, 'pvs')
        o = ops(out)
        self.assertEqual([x for op, x in o if op == 15][0][0], 0)   # Vertex
        self.assertEqual(sorted(x[2] for op, x in o if op == 71 and x[1] == 11), [0, 1, 42, 43])   # Position, PointSize, VertexIndex, InstanceIndex
        self.assertEqual(sorted(x[2] for op, x in o if op == 71 and x[1] == 30), [0, 0, 1, 1, 16])   # in and out at 0 and 1, gl_VertexIndex out at 16
        self.assertTrue([x for op, x in o if op == 43 and x[2] == 0x3b000000])   # 1/512
        self.assertEqual(sum(1 for op, x in o if op == 72 and x[2] == 35), 5)   # five push constants

    def test_spirv_1_4_keeps_the_private_variables_in_the_interface(self):
        vs = VS.replace('%vid %pv\n', '%vid %pv %buf\n').replace('OpDecorate %sb BufferBlock', 'OpDecorate %sb Block').replace('OpTypePointer Uniform', 'OpTypePointer StorageBuffer').replace(
            '%buf = OpVariable %psb Uniform', '%buf = OpVariable %psb StorageBuffer')
        data = self.asm(vs, 'vs14', 'vulkan1.2')
        out, io = self.fs(data); self.assertIsNotNone(out); assert out is not None and io is not None
        self.assertEqual(len([x for op, x in ops(out) if op == 15][0]), len([x for op, x in ops(data) if op == 15][0]) + 1)   # the interface as it was, plus gl_HelperInvocation
        self.valid(out, 'fs14', 'vulkan1.2')
        pvs = self.vs(io, words(data)[1]); self.assertIsNotNone(pvs); assert pvs is not None
        self.valid(pvs, 'pvs14', 'vulkan1.2')

    def test_subgroup_built_ins_are_kept_flat(self):   # a ballot that hands out slots (XC2's 289ca4012f858d7b)
        vs = VS.replace('OpCapability DrawParameters\n', 'OpCapability DrawParameters\nOpCapability GroupNonUniform\n').replace(
            '%vid %pv\n', '%vid %pv %sli\n').replace('OpDecorate %vid BuiltIn VertexIndex\n', 'OpDecorate %vid BuiltIn VertexIndex\nOpDecorate %sli BuiltIn SubgroupLocalInvocationId\n').replace(
            '%pii = OpTypePointer Input %int\n', '%pii = OpTypePointer Input %int\n%piu = OpTypePointer Input %uint\n').replace('%vid = OpVariable %pii Input\n', '%vid = OpVariable %pii Input\n%sli = OpVariable %piu Input\n')
        out, io = self.fs(self.asm(vs, 'vssg')); self.assertIsNotNone(out); assert out is not None and io is not None
        self.valid(out, 'fssg')
        self.assertIn(41, [x[2] for op, x in ops(out) if op == 71 and x[1] == 11])   # still the built-in
        self.assertEqual(io.bi, 1)

    def test_an_unknown_built_in_is_refused(self):
        vs = VS.replace('BuiltIn VertexIndex', 'BuiltIn ViewIndex').replace('OpCapability DrawParameters\n', 'OpCapability DrawParameters\nOpCapability MultiView\n')
        out, _ = self.fs(self.asm(vs, 'vsview')); self.assertIsNone(out)

    def test_a_fragment_shader_is_refused(self):
        out, io = self.fs(self.asm(VS, 'vs3')); assert out is not None
        self.assertIsNone(self.fs(out)[0])

    @unittest.skipUnless(os.environ.get('DM_SPV_DIR'), 'DM_SPV_DIR (a game\'s packed spv folder) not set')
    def test_a_games_shaders(self):   # every vertex shader's fragment form and pass-through vertex shader must pass spirv-val
        d = Path(os.environ['DM_SPV_DIR']); n = 0; writers = os.environ.get('DM_SPV_WRITERS') == '1'
        for f in sorted(d.glob('*.res.json')):
            res = json.loads(f.read_text(encoding='utf-8'))
            if res.get('stage', 'vs') != 'vs': continue
            if writers and not any(b.get('flags', 0) & 1 for b in res.get('sbufs', [])) and not any(m.get('flags', 0) & 4 for m in res.get('images', [])): continue
            key = f.name[:-len('.res.json')]; data = (d / f'{key}.spv').read_bytes()
            out, io = self.fs(data); self.assertIsNotNone(out, key); assert out is not None and io is not None
            self.valid(out, key + '.fs')
            pvs = self.vs(io, words(data)[1]); self.assertIsNotNone(pvs, key); assert pvs is not None
            self.valid(pvs, key + '.pvs'); n += 1
        print(f'{n} vertex shaders made fragment shaders in {d}')


if __name__ == '__main__':
    unittest.main()
