"""dm/android/tbuf_rows.h, a buffer texture read through a 2D texture slot as rows of 16384 texels on the Vulkan replay (gfx_vk.c vt_create / vt_upload_, gfx_vk_draw.inc
tex_rows_slot), as the Mac binds one there (nvn_mtl.m tbuf_as_2d). Assassin's Creed II (the Ezio Collection) reads a 48-float buffer texture by wrapped 1D fetches in its lit
passes; while the slot read the replay's dummy, the 3D picture was black on Android. The header is built with the system's C compiler and called through ctypes: the
translator's slot numbers that res.json's fetch1d lists, the rows' shape and their fill. Skipped without a C compiler.
With the Android NDK, the test shader is compiled with glslc and checked with spirv-val; with DM_TEST_SERIAL=<adb serial>, dm/android/tests/t_tbuf_rows.c is built for Android and
run on that device: every element must read back at the wrapped address, and the last row's texels past the buffer as 0."""
import ctypes
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

from test_spv_readonly import NDK, HOST, EXE   # noqa: E402

REPO = Path(__file__).resolve().parents[3]
CC = (['xcrun', 'clang'] if sys.platform == 'darwin' and shutil.which('xcrun') else
      [c] if (c := shutil.which('clang') or shutil.which('cc') or shutil.which('gcc')) else None)
# the translator's wrapped 1D fetch (Program.cs RecordTexelFetch1D), over every texel of the rows
ROWS_COMP = '''#version 450
layout(local_size_x = 64) in;
layout(binding = 0) uniform usampler2D rows;
layout(binding = 1, std430) writeonly buffer O { uint o[]; };
layout(push_constant) uniform P { uint n; } pc;
void main() { uint x = gl_GlobalInvocationID.x; if (x >= pc.n) return; o[x] = texelFetch(rows, ivec2(int(x & 0x3FFFu), int(x >> 14)), 0).x; }
'''


def handle(w0, w1=0, ty=0): return w0 | w1 << 14 | ty << 28   # TextureHandle.PackOffsets


@unittest.skipIf(CC is None or os.name == 'nt', 'no C compiler')
class TbufRows(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        assert CC is not None   # (skipped without one; pyright doesn't follow skipIf)
        cls.tmp = Path(tempfile.mkdtemp())
        src = cls.tmp / 'r.c'
        src.write_text('#include "tbuf_rows.h"\n'
                       '#define X __attribute__((visibility("default")))\n'
                       'X int slot(uint32_t h, int cs) { return tbr_slot(h, cs); }\n'
                       'X int shape(uint64_t n, uint32_t max, uint32_t *w, uint32_t *h) { return tbr_shape(n, max, w, h); }\n'
                       'X void fill(uint8_t *dst, size_t out, const uint8_t *src, size_t n) { tbr_fill(dst, out, src, n); }\n')
        subprocess.run([*CC, '-shared', '-fPIC', '-O1', '-Wall', '-Wextra', '-Werror', '-I', str(REPO / 'dm/android'), str(src), '-o', str(cls.tmp / 'r.so')], check=True)
        cls.lib = ctypes.CDLL(str(cls.tmp / 'r.so'))
        cls.lib.slot.argtypes = [ctypes.c_uint32, ctypes.c_int]; cls.lib.slot.restype = ctypes.c_int
        cls.lib.shape.argtypes = [ctypes.c_uint64, ctypes.c_uint32, ctypes.POINTER(ctypes.c_uint32), ctypes.POINTER(ctypes.c_uint32)]; cls.lib.shape.restype = ctypes.c_int
        cls.lib.fill.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]; cls.lib.fill.restype = None

    @classmethod
    def tearDownClass(cls): shutil.rmtree(cls.tmp, ignore_errors=True)

    def shape(self, n, mx=16384):
        w, h = ctypes.c_uint32(0), ctypes.c_uint32(0)
        return (w.value, h.value) if self.lib.shape(n, mx, ctypes.byref(w), ctypes.byref(h)) else None

    def test_ezio_compute_shader_slot(self):   # 2405a4193559eaec's res.json: handle 98, no bank given, fetch1d [132] (a separate texture at c2 0x188)
        self.assertEqual(self.lib.slot(98, -1), 132)
        self.assertEqual(self.lib.slot(98, 2), 132)

    def test_texture_and_separate_texture_slots(self):
        self.assertEqual(self.lib.slot(handle(0x20 // 4), -1), 0)   # the first texture slot
        self.assertEqual(self.lib.slot(handle(0x160 // 4), -1), 40)   # the last before the separate textures
        self.assertEqual(self.lib.slot(handle(0x168 // 4), -1), 128)
        self.assertEqual(self.lib.slot(handle(0x560 // 4), -1), 128 + 0x3f8 // 8)

    def test_handles_outside_the_tables_or_c2_have_no_slot(self):
        self.assertEqual(self.lib.slot(handle(0x24 // 4), -1), -1)   # not 8-byte aligned
        self.assertEqual(self.lib.slot(handle(0x10 // 4), -1), -1)   # below the textures
        self.assertEqual(self.lib.slot(handle(0x568 // 4), -1), -1)   # the samplers' table
        self.assertEqual(self.lib.slot(handle(0x20 // 4), 3), -1)   # a handle in a uniform buffer (the replay takes those as rows without fetch1d)

    def test_separate_sampler_handles_look_at_both_words(self):   # Program.cs Slot: CombinedSampler and SeparateConstantSamplerHandle name one word, the others two
        self.assertEqual(self.lib.slot(handle(0x24 // 4, 0x30 // 4, 1), -1), 2)   # the first word is not a slot, the second is
        self.assertEqual(self.lib.slot(handle(0x24 // 4, 0x30 // 4, 0), -1), -1)
        self.assertEqual(self.lib.slot(handle(0x24 // 4, 0x30 // 4, 3), -1), -1)

    def test_shapes(self):
        self.assertEqual(self.shape(48), (48, 1))   # Ezio's buffer texture
        self.assertEqual(self.shape(16384), (16384, 1))
        self.assertEqual(self.shape(16385), (16384, 2))
        self.assertEqual(self.shape(1048576), (16384, 64))   # Age of Calamity's R32F buffer texture
        self.assertIsNone(self.shape(0))
        self.assertEqual(self.shape(16384 * 16384), (16384, 16384))
        self.assertIsNone(self.shape(16384 * 16384 + 1))   # a 16385th row
        self.assertIsNone(self.shape(48, mx=32))   # a GPU whose largest 2D image is narrower than the buffer

    def test_fill_copies_the_buffer_and_zeroes_the_rest(self):
        src = bytes(range(1, 21))
        out = ctypes.create_string_buffer(b'\xab' * 32, 32)
        self.lib.fill(out, 32, src, 20)
        self.assertEqual(out.raw, src + bytes(12))
        out = ctypes.create_string_buffer(b'\xab' * 8, 8)
        self.lib.fill(out, 8, src, 20)   # never past the image
        self.assertEqual(out.raw, src[:8])


@unittest.skipIf(NDK is None, 'no Android NDK')
class TbufRowsDevice(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        assert NDK is not None
        cls.tmp = Path(tempfile.mkdtemp())
        tools = NDK / 'shader-tools' / HOST
        cls.spv_val = tools / f'spirv-val{EXE}'
        cls.bin = NDK / 'toolchains/llvm/prebuilt' / HOST / 'bin'
        (cls.tmp / 'rows.comp').write_text(ROWS_COMP)
        cls.spv = cls.tmp / 'rows.spv'
        subprocess.run([tools / f'glslc{EXE}', '--target-env=vulkan1.1', '-O', cls.tmp / 'rows.comp', '-o', cls.spv], check=True)

    @classmethod
    def tearDownClass(cls): shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_shader_is_valid_spirv(self):
        r = subprocess.run([self.spv_val, '--target-env', 'vulkan1.1', self.spv], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)

    @unittest.skipUnless(os.environ.get('DM_TEST_SERIAL'), 'DM_TEST_SERIAL not set')
    def test_rows_on_a_device(self):
        serial = os.environ['DM_TEST_SERIAL']
        adb = shutil.which('adb') or str(Path(os.environ.get('ANDROID_HOME', '')) / 'platform-tools' / f'adb{EXE}')
        exe = self.tmp / 't_tbuf_rows'
        cc = self.bin / ('aarch64-linux-android29-clang' + ('.cmd' if os.name == 'nt' else ''))
        subprocess.run([cc, '-O1', '-Wall', '-Werror', REPO / 'dm/android/tests/t_tbuf_rows.c', '-lvulkan', '-o', exe], check=True)
        d = '/data/local/tmp/dm_tbuf_rows'
        subprocess.run([adb, '-s', serial, 'shell', 'mkdir', '-p', d], check=True)
        for f in (exe, self.spv): subprocess.run([adb, '-s', serial, 'push', f, d], check=True, capture_output=True)
        r = subprocess.run([adb, '-s', serial, 'shell', f'cd {d} && chmod 755 t_tbuf_rows && ./t_tbuf_rows rows.spv; echo "exit $?"'], capture_output=True, text=True)
        subprocess.run([adb, '-s', serial, 'shell', 'rm', '-r', d], capture_output=True)
        print(r.stdout.strip())
        self.assertTrue(r.stdout.strip().endswith('exit 0'), r.stdout + r.stderr)


if __name__ == '__main__':
    unittest.main()
