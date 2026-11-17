"""dm/android/spv_inarr.h (a shader's attribute arrays: every location they cover, and a vertex or fragment shader's input arrays split into an input per
location: Vulkan matches a stage's interface variable by variable, and Qualcomm's Adreno 730 driver linked only the first element of Metroid Prime Remastered's
`in vec4 _50[16]`), built for the PC and run on modules assembled with the NDK's spirv-as; spirv-val checks each result. Skipped without the NDK.
With DM_SPV_DIR=<a game's packed spv folder>, also run over every module there (each rewritten module must pass spirv-val)."""
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

# As the translator writes Metroid Prime Remastered's force field (8a9e8ae4d53c4a82): the generic inputs one array, read at an index known only at run time
# (here from the pixel's x) and at constant ones, a whole vec4 and one component; DECO adds decorations to the array
FS = '''OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint Fragment %main "main" %arr %fc %c0
OpExecutionMode %main OriginUpperLeft
OpDecorate %arr Location @LOC@
@DECO@OpDecorate %fc BuiltIn FragCoord
OpDecorate %c0 Location 0
%void = OpTypeVoid
%fn = OpTypeFunction %void
%int = OpTypeInt 32 1
%uint = OpTypeInt 32 0
%float = OpTypeFloat 32
%v4 = OpTypeVector %float 4
%uN = OpConstant %uint @LEN@
%arrt = OpTypeArray %v4 %uN
%parr = OpTypePointer Input %arrt
%pv4 = OpTypePointer Input %v4
%pf = OpTypePointer Input %float
%po = OpTypePointer Output %v4
%fpf = OpTypePointer Function %float
%arr = OpVariable %parr Input
%fc = OpVariable %pv4 Input
%c0 = OpVariable %po Output
%i1 = OpConstant %int 1
%i2 = OpConstant %int 2
%main = OpFunction %void None %fn
%l = OpLabel
@LOCALS@%fx = OpAccessChain %pf %fc %i1
%x = OpLoad %float %fx
%ix = OpConvertFToS %int %x
%pd = OpAccessChain %pv4 %arr %ix
%d = OpLoad %v4 %pd
%pc = OpAccessChain %pf %arr %i1 %i2
%c = OpLoad %float %pc
%r = OpVectorTimesScalar %v4 %d %c
OpStore %c0 %r
OpReturn
OpFunctionEnd
'''


# A vertex shader reading its attributes through one array, at an index known only at run time (from the vertex index) and at constant ones
VS = '''OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint Vertex %main "main" %arr %vi %pos %o0
OpDecorate %arr Location 0
OpDecorate %vi BuiltIn VertexIndex
OpDecorate %pos BuiltIn Position
OpDecorate %o0 Location 0
%void = OpTypeVoid
%fn = OpTypeFunction %void
%int = OpTypeInt 32 1
%uint = OpTypeInt 32 0
%float = OpTypeFloat 32
%v4 = OpTypeVector %float 4
%u6 = OpConstant %uint 6
%arrt = OpTypeArray %v4 %u6
%parr = OpTypePointer Input %arrt
%pv4 = OpTypePointer Input %v4
%pf = OpTypePointer Input %float
%pint = OpTypePointer Input %int
%po = OpTypePointer Output %v4
%arr = OpVariable %parr Input
%vi = OpVariable %pint Input
%pos = OpVariable %po Output
%o0 = OpVariable %po Output
%i1 = OpConstant %int 1
%i2 = OpConstant %int 2
%main = OpFunction %void None %fn
%l = OpLabel
%x = OpLoad %int %vi
%pd = OpAccessChain %pv4 %arr %x
%d = OpLoad %v4 %pd
%pc = OpAccessChain %pf %arr %i1 %i2
%c = OpLoad %float %pc
%r = OpVectorTimesScalar %v4 %d %c
OpStore %pos %d
OpStore %o0 %r
OpReturn
OpFunctionEnd
'''

# A geometry shader's per-vertex input at location 2 (an array over the triangle's vertices) and its output at 0
GS = '''OpCapability Geometry
OpMemoryModel Logical GLSL450
OpEntryPoint Geometry %main "main" %gin %gout
OpExecutionMode %main Triangles
OpExecutionMode %main Invocations 1
OpExecutionMode %main OutputTriangleStrip
OpExecutionMode %main OutputVertices 3
OpDecorate %gin Location 2
OpDecorate %gout Location 0
%void = OpTypeVoid
%fn = OpTypeFunction %void
%int = OpTypeInt 32 1
%uint = OpTypeInt 32 0
%float = OpTypeFloat 32
%v4 = OpTypeVector %float 4
%u3 = OpConstant %uint 3
%arrt = OpTypeArray %v4 %u3
%pin = OpTypePointer Input %arrt
%pv = OpTypePointer Input %v4
%pout = OpTypePointer Output %v4
%gin = OpVariable %pin Input
%gout = OpVariable %pout Output
%i0 = OpConstant %int 0
%main = OpFunction %void None %fn
%l = OpLabel
%p = OpAccessChain %pv %gin %i0
%v = OpLoad %v4 %p
OpStore %gout %v
OpEmitVertex
OpReturn
OpFunctionEnd
'''


def fs(loc=0, n=4, deco='', locals_=''): return FS.replace('@LOC@', str(loc)).replace('@LEN@', str(n)).replace('@DECO@', deco).replace('@LOCALS@', locals_)


def words(data): return struct.unpack('<%dI' % (len(data) // 4), data)


def ops(data):   # [(opcode, operands)]
    w = words(data); i = 5; out = []
    while i < len(w):
        n = w[i] >> 16; out.append((w[i] & 0xffff, w[i + 1:i + n])); i += max(n, 1)
    return out


@unittest.skipIf(NDK is None, 'no Android NDK')
class SpvInarr(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        assert NDK is not None
        cls.tmp = Path(tempfile.mkdtemp())
        tools = NDK / 'shader-tools' / HOST; cls.spv_as, cls.spv_val = tools / f'spirv-as{EXE}', tools / f'spirv-val{EXE}'
        cls.bin = NDK / 'toolchains/llvm/prebuilt' / HOST / 'bin'
        src = cls.tmp / 'v.c'
        export = '__declspec(dllexport)' if os.name == 'nt' else '__attribute__((visibility("default")))'
        src.write_text(f'#include "spv_inarr.h"\n{export} size_t run(const uint32_t *w, size_t n, uint32_t *s, uint32_t *o, size_t cap) {{ return spv_inarr(w, n, s, o, cap); }}\n'
                       f'{export} size_t need(const uint32_t *w, size_t n, uint32_t *s) {{ return spv_inarr_need(w, n, s); }}\n'
                       f'{export} void io(const uint32_t *w, size_t n, uint32_t *s, uint32_t *in, uint32_t *out) {{ spv_inarr_io(w, n, s, in, out); }}\n'
                       f'{export} uint32_t mask(const uint32_t *w, size_t n, uint32_t *s) {{ return spv_inarr_mask(w, n, s); }}\n'
                       f'{export} int wanted(const uint32_t *w, size_t n) {{ return spv_inarr_wanted(w, n); }}\n')
        env = dict(os.environ, MSYS_NO_PATHCONV='1')
        if os.name == 'nt':   # no C library: a DLL linked by lld alone
            subprocess.run([cls.bin / 'clang.exe', '--target=x86_64-pc-windows-msvc', '-ffreestanding', '-nostdlib', '-O1', '-Wall', '-Werror', '-I', REPO / 'dm/android', '-c', src,
                            '-o', cls.tmp / 'v.obj'], check=True)
            subprocess.run([cls.bin / 'ld.lld.exe', '-flavor', 'link', '-dll', '-noentry', f'-out:{cls.tmp / "v.dll"}', str(cls.tmp / 'v.obj')], check=True, env=env)
            cls.lib = ctypes.CDLL(str(cls.tmp / 'v.dll'))
        else:   # the system's clang, as test_spv_limits.py explains
            cc = ['xcrun', 'clang'] if sys.platform == 'darwin' else [shutil.which('clang') or cls.bin / 'clang']
            subprocess.run([*cc, '-shared', '-fPIC', '-O1', '-Wall', '-Wextra', '-Werror', '-I', REPO / 'dm/android', src, '-o', cls.tmp / 'v.so'], check=True)
            cls.lib = ctypes.CDLL(str(cls.tmp / 'v.so'))
        cls.lib.run.restype = cls.lib.need.restype = ctypes.c_size_t; cls.lib.mask.restype = ctypes.c_uint32

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

    def args(self, data):
        n = len(data) // 4; return (ctypes.c_uint32 * n).from_buffer_copy(data), n, (ctypes.c_uint32 * (8 * words(data)[3]))()

    def run_on(self, data):   # → the rewritten module or None
        w, n, s = self.args(data); need = self.lib.need(w, n, s)
        if not need: return None
        o = (ctypes.c_uint32 * need)(); k = self.lib.run(w, n, s, o, need)
        self.assertGreater(k, 0); self.assertLessEqual(k, need)
        return bytes(o)[:4 * k]

    def io(self, data):   # → (the input mask, the output mask)
        i, o = ctypes.c_uint32(), ctypes.c_uint32(); self.lib.io(*self.args(data), ctypes.byref(i), ctypes.byref(o)); return i.value, o.value

    def mask(self, data): return self.lib.mask(*self.args(data))   # the split arrays' locations

    def wanted(self, data): w, n, _ = self.args(data); return self.lib.wanted(w, n)

    def check_split(self, src, name, loc, n, deco=(), vs=False):
        data = self.asm(src, name); self.assertTrue(self.wanted(data))
        self.assertEqual(self.mask(data), ((1 << n) - 1) << loc)
        self.assertEqual(self.io(data)[0], ((1 << n) - 1 if vs else 1) << loc)   # a vertex shader's array: every attribute; a fragment shader's: its first, until split
        out = self.run_on(data); self.assertIsNotNone(out); assert out is not None
        self.valid(out, name)
        before, after = ops(data), ops(out)
        arr = next(x[1] for op, x in before if op == 59 and x[2] == 1 and any(o2 == 71 and y[:2] == (x[1], 30) for o2, y in before))   # the Input variable with a Location
        self.assertIn((arr, 6), [(x[1], x[2]) for op, x in after if op == 59])   # the array: Private now, the same id
        ins = [x[1] for op, x in after if op == 59 and x[2] == 1 and x[1] >= words(data)[3]]   # its inputs: new ids
        self.assertEqual(len(ins), n)
        locs = {x[0]: x[2] for op, x in after if op == 71 and x[1] == 30}
        self.assertEqual([locs[v] for v in ins], list(range(loc, loc + n)))
        self.assertNotIn(arr, locs)   # no decoration left on the array
        for d in deco: self.assertEqual(sum(1 for op, x in after if op == 71 and x[0] in ins and x[1] == d), n)
        entry = next(x for op, x in after if op == 15)
        self.assertNotIn(arr, entry[3:]); self.assertTrue(set(ins) <= set(entry))
        ptrs = {x[0]: x[1] for op, x in after if op == 32}
        for op, x in after:
            if op == 65 and x[2] == arr: self.assertEqual(ptrs[x[0]], 6)   # its access chains: Private pointers
        lab = next(i for i, (op, _) in enumerate(after) if op == 248)   # the entry point's first OpLabel
        i = lab + 1
        while after[i][0] == 59: i += 1   # its local variables stay first
        self.assertEqual([after[i + k][0] for k in range(n)], [61] * n)   # the inputs loaded,
        self.assertEqual(after[i + n][0], 80)   # built into the array
        self.assertEqual(after[i + n + 1], (62, (arr, after[i + n][1][1])))   # and stored
        return out

    def test_the_array_is_split_into_an_input_per_location(self):
        self.check_split(fs(), 'fs', 0, 4)

    def test_its_decorations_go_to_each_input_and_locations_follow_its_own(self):
        self.check_split(fs(2, 3, 'OpDecorate %arr Flat\n'), 'flat', 2, 3, deco=(14,))

    def test_local_variables_stay_first_in_the_block(self):
        self.check_split(fs(locals_='%t = OpVariable %fpf Function\n%t2 = OpVariable %fpf Function\n'), 'loc', 0, 4)

    def test_an_array_used_otherwise_is_left_alone(self):   # copied whole into a local: not a use spv_inarr follows
        src = fs().replace('%fpf = OpTypePointer Function %float\n', '%fpf = OpTypePointer Function %float\n%farr = OpTypePointer Function %arrt\n') \
                  .replace('%l = OpLabel\n', '%l = OpLabel\n%cp = OpVariable %farr Function\nOpCopyMemory %cp %arr\n')
        data = self.asm(src, 'copy'); self.assertIsNone(self.run_on(data)); self.assertEqual(self.mask(data), 0); self.assertEqual(self.io(data)[0], 1)   # kept whole: the input variant can drop it only whole

    def test_a_fragment_shader_without_arrays_is_left_alone(self):
        src = '''OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint Fragment %main "main" %i0 %c0
OpExecutionMode %main OriginUpperLeft
OpDecorate %i0 Location 0
OpDecorate %c0 Location 0
%void = OpTypeVoid
%fn = OpTypeFunction %void
%float = OpTypeFloat 32
%v4 = OpTypeVector %float 4
%pi = OpTypePointer Input %v4
%po = OpTypePointer Output %v4
%i0 = OpVariable %pi Input
%c0 = OpVariable %po Output
%main = OpFunction %void None %fn
%l = OpLabel
%v = OpLoad %v4 %i0
OpStore %c0 %v
OpReturn
OpFunctionEnd
'''
        data = self.asm(src, 'plain'); self.assertTrue(self.wanted(data)); self.assertIsNone(self.run_on(data)); self.assertEqual(self.io(data), (1, 1)); self.assertEqual(self.mask(data), 0)

    def test_a_vertex_shaders_input_array_is_split_too(self):   # its attributes read through one, as 3,858 of the game's vertex shaders do
        self.check_split(VS, 'vs', 0, 6, vs=True)

    def test_a_vertex_shaders_output_array_covers_its_elements(self):   # not split (no game's shaders have one), but every location it writes counts
        src = VS.replace('OpEntryPoint Vertex %main "main" %arr %vi %pos %o0', 'OpEntryPoint Vertex %main "main" %vi %pos %o0') \
                .replace('OpDecorate %arr Location 0\n', '').replace('OpDecorate %o0 Location 0', 'OpDecorate %o0 Location 1') \
                .replace('%arr = OpVariable %parr Input\n', '').replace('%po = OpTypePointer Output %v4\n', '%po = OpTypePointer Output %v4\n%poa = OpTypePointer Output %arrt\n') \
                .replace('%o0 = OpVariable %po Output', '%o0 = OpVariable %poa Output')
        src = src[:src.index('%x = OpLoad')] + '%po1 = OpAccessChain %po %o0 %i2\n%v = OpCompositeConstruct %v4 %f0 %f0 %f0 %f0\nOpStore %po1 %v\nOpStore %pos %v\nOpReturn\nOpFunctionEnd\n'
        src = src.replace('%i2 = OpConstant %int 2\n', '%i2 = OpConstant %int 2\n%f0 = OpConstant %float 0\n')
        data = self.asm(src, 'vsout'); self.assertTrue(self.wanted(data)); self.assertIsNone(self.run_on(data)); self.assertEqual(self.io(data), (0, 0x3f << 1))
        self.valid(data, 'vsout')

    def test_a_geometry_shaders_per_vertex_input_covers_one_location(self):   # its outer array is the vertex's, not one attribute per element
        data = self.asm(GS, 'gs'); self.assertFalse(self.wanted(data)); self.assertIsNone(self.run_on(data)); self.assertEqual(self.io(data), (1 << 2, 1))

    @unittest.skipUnless(os.environ.get('DM_SPV_DIR'), 'DM_SPV_DIR (a game\'s packed spv folder) not set')
    def test_a_games_shaders(self):   # each rewritten module must pass spirv-val
        d = Path(os.environ['DM_SPV_DIR']); n = 0
        for f in sorted(d.glob('*.spv')):
            data = f.read_bytes()
            if not self.wanted(data): continue
            out = self.run_on(data)
            if out is None: continue
            self.valid(out, f.stem); n += 1
        print(f'{n} modules rewritten and valid')


if __name__ == '__main__':
    unittest.main()
