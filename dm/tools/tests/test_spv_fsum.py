"""dm/android/spv_fsum.h (the translator's float atomic adds to a fixed word, compare-and-swap loops, summed across the subgroup first and made by one invocation), built for
the PC and run on compute shaders assembled with the NDK's spirv-as in the translator's form (InstEmitMemory.EmitAtomicAddF32, DebugMode locals and SSA values); spirv-val
checks each result. Loops on a computed element or whose result is read later are left. Skipped without the NDK.
With DM_SPV_DIR=<a game's packed spv folder>, also run over every module there with a compare-and-swap in a loop: each result must pass spirv-val."""
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

FORM, ROOM = 1, 2   # spv_fsum.h's reasons
ENV = {0x10300: 'vulkan1.1', 0x10400: 'vulkan1.1spv1.4', 0x10500: 'vulkan1.2', 0x10600: 'vulkan1.3'}


def cs(words=(0,), debug=True, index=None, use_after=False, value_int=False):
    """A compute shader that adds a float value to storage words (constant elements `words`, or element `index`, an id) through the translator's loop: load the word,
    FAdd, OpAtomicCompareExchange, again while the word changed. debug: every value through a Function local, as DebugMode writes it; use_after: the loop's result
    (the word before the add) stored after the loop; value_int: the value an int bitcast to float in the loop, as some shaders have it"""
    out = []
    pre = ['%v = OpConvertUToF %float %x']
    if debug: pre += ['OpStore %lv %v']
    if value_int:
        pre = ['%vi = OpBitcast %int %x'] + (['OpStore %lvi %vi'] if debug else [])
    body_end = []
    for n, w in enumerate(words):
        el = index or f'%i{w}'
        out += [f'OpBranch %h{n}', f'%h{n} = OpLabel', f'OpLoopMerge %m{n} %c{n} None', f'OpBranch %b{n}', f'%b{n} = OpLabel',
                f'%p{n} = OpAccessChain %psu %buf %i0 {el}', f'%o{n} = OpLoad %uint %p{n}', f'%of{n} = OpBitcast %float %o{n}']
        if debug: out += [f'OpStore %lo{n} %of{n}', f'%oa{n} = OpLoad %float %lo{n}']
        old = f'%oa{n}' if debug else f'%of{n}'
        if value_int:
            out += [f'%vl{n} = OpLoad %int %lvi' if debug else f'%vl{n} = OpCopyObject %int %vi', f'%va{n} = OpBitcast %float %vl{n}']
        else:
            out += [f'%va{n} = OpLoad %float %lv'] if debug else []
        val = f'%va{n}' if (debug or value_int) else '%v'
        out += [f'%s{n} = OpFAdd %float {old} {val}']
        if debug: out += [f'OpStore %ls{n} %s{n}', f'%sa{n} = OpLoad %float %ls{n}']
        snew = f'%sa{n}' if debug else f'%s{n}'
        out += [f'%q{n} = OpAccessChain %psu %buf %i0 {el}', f'%nu{n} = OpBitcast %uint {snew}', f'%ou{n} = OpBitcast %uint {old}',
                f'%r{n} = OpAtomicCompareExchange %uint %q{n} %u1 %u0 %u0 %nu{n} %ou{n}']
        if debug: out += [f'OpStore %lr{n} %r{n}', f'%ra{n} = OpLoad %uint %lr{n}']
        res = f'%ra{n}' if debug else f'%r{n}'
        out += [f'%ne{n} = OpINotEqual %bool {res} %ou{n}', f'OpBranch %c{n}', f'%c{n} = OpLabel', f'OpBranchConditional %ne{n} %h{n} %m{n}', f'%m{n} = OpLabel']
        if use_after: body_end += [f'%pa{n} = OpAccessChain %psu %buf %i0 %i9', f'OpStore %pa{n} {"%ra" if debug else "%r"}{n}' if not debug else f'%rz{n} = OpLoad %uint %lr{n}', *( [f'OpStore %pa{n} %rz{n}'] if debug else [])]
    locs = ['%lv = OpVariable %pff Function', '%lvi = OpVariable %pfi Function'] + [f'%l{t}{n} = OpVariable {ty} Function' for n in range(len(words)) for t, ty in (('o', '%pff'), ('s', '%pff'), ('r', '%pfu'))]
    if not debug: locs = []
    return f'''OpCapability Shader
OpMemoryModel Logical GLSL450
OpEntryPoint GLCompute %main "main" %lid
OpExecutionMode %main LocalSize 64 1 1
OpDecorate %lid BuiltIn LocalInvocationId
OpDecorate %ra_t ArrayStride 4
OpMemberDecorate %bb 0 Offset 0
OpDecorate %bb Block
OpDecorate %buf DescriptorSet 0
OpDecorate %buf Binding 3
%void = OpTypeVoid
%fn = OpTypeFunction %void
%uint = OpTypeInt 32 0
%int = OpTypeInt 32 1
%float = OpTypeFloat 32
%bool = OpTypeBool
%v3uint = OpTypeVector %uint 3
%ra_t = OpTypeRuntimeArray %uint
%bb = OpTypeStruct %ra_t
%pbb = OpTypePointer StorageBuffer %bb
%psu = OpTypePointer StorageBuffer %uint
%buf = OpVariable %pbb StorageBuffer
%pv3 = OpTypePointer Input %v3uint
%lid = OpVariable %pv3 Input
%pff = OpTypePointer Function %float
%pfu = OpTypePointer Function %uint
%pfi = OpTypePointer Function %int
%i0 = OpConstant %int 0
%i1 = OpConstant %int 1
%i2 = OpConstant %int 2
%i3 = OpConstant %int 3
%i9 = OpConstant %int 9
%u0 = OpConstant %uint 0
%u1 = OpConstant %uint 1
%main = OpFunction %void None %fn
%ml = OpLabel
{chr(10).join(locs)}
%lvv = OpLoad %v3uint %lid
%x = OpCompositeExtract %uint %lvv 0
%xi = OpBitcast %int %x
{chr(10).join(pre)}
{chr(10).join(out)}
{chr(10).join(body_end)}
OpReturn
OpFunctionEnd
'''


def ops(data):
    w = struct.unpack('<%dI' % (len(data) // 4), data); i = 5; out = []
    while i < len(w): n = w[i] >> 16; out.append((w[i] & 0xffff, w[i + 1:i + n])); i += max(n, 1)
    return out


@unittest.skipIf(NDK is None, 'no Android NDK')
class SpvFsum(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        assert NDK is not None
        cls.tmp = Path(tempfile.mkdtemp())
        tools = NDK / 'shader-tools' / HOST; cls.spv_as, cls.spv_val = tools / f'spirv-as{EXE}', tools / f'spirv-val{EXE}'
        cls.bin = NDK / 'toolchains/llvm/prebuilt' / HOST / 'bin'
        src = cls.tmp / 't.c'
        export = '__declspec(dllexport)' if os.name == 'nt' else '__attribute__((visibility("default")))'
        src.write_text(f'#include "spv_fsum.h"\n{export} size_t run(const uint32_t *w, size_t n, uint32_t *sc, uint32_t *o, size_t cap, int *why) {{ return spv_fsum(w, n, sc, o, cap, why); }}\n'
                       f'{export} int wanted(const uint32_t *w, size_t n) {{ return spv_fsum_wanted(w, n); }}\n')
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
        cls.lib.run.restype = ctypes.c_size_t; cls.lib.run.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]
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

    def run_on(self, data, cap=None):
        n = len(data) // 4; w = (ctypes.c_uint32 * n).from_buffer_copy(data); cap = cap or 2 * n + 4096; o = (ctypes.c_uint32 * cap)()
        sc = (ctypes.c_uint32 * (3 * struct.unpack_from('<I', data, 12)[0]))(); why = ctypes.c_int(-1)
        k = self.lib.run(w, n, sc, o, cap, ctypes.byref(why))
        return (bytes(o)[:4 * k] if k else None), why.value

    def check(self, text, name, loops, env='vulkan1.1'):
        data = self.asm(text, name, env)
        self.assertTrue(self.lib.wanted((ctypes.c_uint32 * (len(data) // 4)).from_buffer_copy(data), len(data) // 4))
        out, why = self.run_on(data); self.assertIsNotNone(out, why); assert out is not None
        self.valid(out, name, env)
        o = ops(out)
        caps = {x[0] for op, x in o if op == 17}; self.assertTrue({61, 63} <= caps)
        sums = [x for op, x in o if op == 350]; els = [x for op, x in o if op == 333]
        self.assertEqual((len(sums), len(els)), (loops, loops))
        self.assertTrue(all(x[3] == 0 for x in sums))   # Reduce
        adds = [x for op, x in o if op == 129]; self.assertEqual(sorted(x[3] if x[3] in {s[1] for s in sums} else x[2] for x in adds), sorted(s[1] for s in sums))   # each add takes a sum
        # each loop entered on its election: a selection whose condition is the elect, whose merge is the loop's old merge; the loop merges into a new block that goes there
        conds = [x for op, x in o if op == 250 and x[0] in {e[1] for e in els}]; self.assertEqual(len(conds), loops)
        lms = [x for op, x in o if op == 246]
        for x in lms:
            j = next(i for i, (op, y) in enumerate(o) if op == 248 and y[0] == x[0])
            self.assertEqual(o[j + 1][0], 249)   # the new merge block: a branch on
        self.assertEqual(len([x for op, x in o if op == 230]), loops)   # the compare-and-swaps stay
        # each loop reads the word once before it (into a new local) and starts each retry from what its compare-and-swap found
        oi = ops(data); before = {x[1] for op, x in oi if op == 59 and x[2] == 7}
        new = {x[1] for op, x in o if op == 59 and x[2] == 7} - before; self.assertEqual(len(new), loops)
        cas = {x[1] for op, x in o if op == 230}
        self.assertEqual(len({x[0] for op, x in o if op == 62 and x[0] in new and x[1] in cas}), loops)
        self.assertEqual(len([x for op, x in o if op == 61 and x[2] in new]), loops)   # the loop's load of the word, now of the local
        return o

    def test_debug_form(self): self.check(cs(), 'dbg', 1)

    def test_ssa_form(self): self.check(cs(debug=False), 'ssa', 1)

    def test_chained_loops(self): self.check(cs(words=(0, 1, 2, 3)), 'four', 4)   # one loop's merge is the next one's preheader

    def test_int_value(self): self.check(cs(value_int=True), 'ival', 1)

    def test_spirv_14(self): self.check(cs().replace('"main" %lid', '"main" %lid %buf'), 'v14', 1, 'vulkan1.1spv1.4')

    def test_computed_element_left(self):   # each invocation may add to another word: no sum across them
        out, why = self.run_on(self.asm(cs(index='%xi'), 'idx')); self.assertIsNone(out); self.assertEqual(why, FORM)

    def test_result_used_left(self):   # an ATOM whose value before the add is read after the loop
        out, why = self.run_on(self.asm(cs(use_after=True), 'use')); self.assertIsNone(out); self.assertEqual(why, FORM)

    def test_room(self):
        data = self.asm(cs(), 'room'); out, why = self.run_on(data, cap=len(data) // 4 + 4); self.assertIsNone(out); self.assertEqual(why, ROOM)

    @unittest.skipUnless(os.environ.get('DM_SPV_DIR'), 'DM_SPV_DIR (a game\'s packed spv folder) not set')
    def test_game_modules(self):
        d = Path(os.environ['DM_SPV_DIR']); n = done = 0; bad = []
        for p in sorted(d.glob('*.spv')):
            data = p.read_bytes()
            if len(data) < 20 or not self.lib.wanted((ctypes.c_uint32 * (len(data) // 4)).from_buffer_copy(data), len(data) // 4): continue
            n += 1; out, why = self.run_on(data)
            if out is None: continue
            done += 1; (self.tmp / 'g.spv').write_bytes(out)
            r = subprocess.run([self.spv_val, '--target-env', ENV.get(struct.unpack_from('<I', data, 4)[0], 'vulkan1.1'), self.tmp / 'g.spv'], capture_output=True, text=True)
            if r.returncode: bad.append(f'{p.name}: {(r.stdout + r.stderr).strip()[:200]}')
        print(f'\n{n} modules with a compare-and-swap and a loop in {d}: {done} rewritten, invalid {len(bad)}')
        self.assertFalse(bad, '\n'.join(bad[:10]))


if __name__ == '__main__':
    unittest.main()
