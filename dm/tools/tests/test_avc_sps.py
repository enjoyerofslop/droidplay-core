"""dm/android/avc_sps.h (whether an H.264 stream's pictures leave the decoder in decode order, read from its SPS: the VUI's bitstream_restriction, else
pic_order_cnt_type 2 or the Baseline profile; and the frames its picture buffer holds, by level and picture size), built for the PC with the system's clang and driven through ctypes on sequence parameter sets written here
field by field (H.264 7.3.2.1.1, E.1.1), emulation prevention bytes included. POSIX only: skipped on Windows, and without a C compiler."""
import ctypes
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
CC = ['xcrun', 'clang'] if sys.platform == 'darwin' and shutil.which('xcrun') else [shutil.which('clang') or shutil.which('cc') or '']
SRC = '''#include "avc_sps.h"
#define X __attribute__((visibility("default")))
X int g_parse(const uint8_t *nal, size_t n, int *f) {   // f: profile, poc_type, refs, vui, br, reorder, order, level, mbs, dpb
    avc_rbsp r; avc_sps s; int ok = avc_sps_parse(nal, n, &r, &s);
    f[0] = s.profile; f[1] = s.poc_type; f[2] = s.refs; f[3] = s.vui; f[4] = s.br; f[5] = s.reorder; f[6] = avc_sps_decode_order(&s);
    f[7] = s.level; f[8] = s.mbs; f[9] = avc_sps_dpb_frames(&s);
    return ok;
}
X int g_order(const uint8_t *nal, size_t n) { return avc_decode_order(nal, n); }
'''


class Bits:
    def __init__(self): self.b = []
    def u(self, n, v): self.b += [(v >> (n - 1 - i)) & 1 for i in range(n)]; return self
    def ue(self, v): z = (v + 1).bit_length() - 1; return self.u(z, 0).u(z + 1, v + 1)
    def se(self, v): return self.ue(2 * v - 1 if v > 0 else -2 * v)
    def nal(self, header=0x67):   # rbsp_trailing_bits, then emulation prevention (00 00 0x, x <= 3 → 00 00 03 0x)
        b = self.b + [1] + [0] * ((-len(self.b) - 1) % 8)
        raw = bytes(int(''.join(map(str, b[i:i + 8])), 2) for i in range(0, len(b), 8))
        out, z = bytearray([header]), 0
        for c in raw:
            if z >= 2 and c <= 3: out.append(3); z = 0
            out.append(c); z = z + 1 if c == 0 else 0
        return bytes(out)


def sps(profile=77, poc=2, refs=3, vui=None, high=None, poc1=(0, 0, ()), level=41, mbs=(120, 68), frames=1, constraints=0x40):
    """An SPS: 1920x1080 (cropped from 1088), frame_mbs_only; high = (chroma_format_idc, scaling lists or None); vui = a function writing vui_parameters;
    poc1 = pic_order_cnt_type 1's (offset_for_non_ref_pic, offset_for_top_to_bottom_field, offsets for the reference frames in the cycle);
    mbs = the picture's width in macroblocks and height in map units; frames = frame_mbs_only_flag (0: fields, map units of two macroblock rows)"""
    w = Bits().u(8, profile).u(8, constraints).u(8, level).ue(0)
    if high is not None:
        chroma, lists = high
        w.ue(chroma)
        if chroma == 3: w.u(1, 0)
        w.ue(0).ue(0).u(1, 0).u(1, lists is not None)
        if lists is not None:
            for i in range(12 if chroma == 3 else 8):
                deltas = lists.get(i)
                w.u(1, deltas is not None)
                for d in deltas or []: w.se(d)
    w.ue(4).ue(poc)
    if poc == 0: w.ue(2)
    elif poc == 1:
        w.u(1, 0).se(poc1[0]).se(poc1[1]).ue(len(poc1[2]))
        for o in poc1[2]: w.se(o)
    w.ue(refs).u(1, 0).ue(mbs[0] - 1).ue(mbs[1] - 1).u(1, frames)
    if not frames: w.u(1, 0)   # mb_adaptive_frame_field_flag
    w.u(1, 1)
    w.u(1, 1).ue(0).ue(0).ue(0).ue(4)   # frame cropping: 8 lines off the bottom
    w.u(1, vui is not None)
    if vui: vui(w)
    return w.nal()


def vui_timing(br=None):   # aspect ratio 1, timing 1000 / 60000 fixed, no HRD; br = (max_num_reorder_frames, max_dec_frame_buffering) or None
    def f(w):
        w.u(1, 1).u(8, 1).u(1, 0).u(1, 0).u(1, 0).u(1, 1).u(32, 1000).u(32, 60000).u(1, 1).u(1, 0).u(1, 0).u(1, 0).u(1, br is not None)
        if br: w.u(1, 1).ue(2).ue(1).ue(16).ue(16).ue(br[0]).ue(br[1])
    return f


def vui_full(reorder):   # Extended_SAR 0:1 (00 00 00 01 in the RBSP: escaped), overscan, colour description, chroma location, timing, NAL and VCL HRD with two CPBs
    def hrd(w):
        w.ue(1).u(4, 3).u(4, 5)
        for _ in range(2): w.ue(1000).ue(2000).u(1, 1)
        w.u(5, 23).u(5, 23).u(5, 23).u(5, 24)

    def f(w):
        w.u(1, 1).u(8, 255).u(16, 0).u(16, 1).u(1, 1).u(1, 1).u(1, 1).u(3, 5).u(1, 0).u(1, 1).u(8, 1).u(8, 1).u(8, 1).u(1, 1).ue(1).ue(2)
        w.u(1, 1).u(32, 1001).u(32, 60000).u(1, 0)
        w.u(1, 1); hrd(w); w.u(1, 1); hrd(w); w.u(1, 0).u(1, 1)   # NAL HRD, VCL HRD, low_delay_hrd_flag, pic_struct_present_flag
        w.u(1, 1).u(1, 1).ue(2).ue(1).ue(15).ue(15).ue(reorder).ue(4)
    return f


@unittest.skipIf(os.name == 'nt' or not CC[-1], 'a C compiler needed')
class AvcSps(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp())
        (cls.tmp / 'g.c').write_text(SRC)
        subprocess.run([*CC, '-shared', '-fPIC', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-I', REPO / 'dm/android', cls.tmp / 'g.c', '-o', cls.tmp / 'g.so'], check=True)
        cls.lib = ctypes.CDLL(str(cls.tmp / 'g.so'))
        cls.lib.g_parse.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_int)]
        cls.lib.g_order.argtypes = [ctypes.c_char_p, ctypes.c_size_t]

    @classmethod
    def tearDownClass(cls): shutil.rmtree(cls.tmp, ignore_errors=True)

    def parse(self, nal):   # → (ok, {field: value})
        f = (ctypes.c_int * 10)()
        ok = self.lib.g_parse(nal, len(nal), f)
        names: list[str] = ['profile', 'poc', 'refs', 'vui', 'br', 'reorder', 'order', 'level', 'mbs', 'dpb']
        return ok, dict(zip(names, list(f)))

    def check(self, nal, **want):
        ok, f = self.parse(nal)
        self.assertTrue(ok)
        for k, v in want.items(): self.assertEqual(f[k], v, k)
        self.assertEqual(self.lib.g_order(nal, len(nal)), want.get('order', f['order']))

    def test_poc2_vui_without_restriction(self):   # Donkey Kong Country Returns HD 1.0.0's opening movie: Main, pic_order_cnt_type 2, timing, no bitstream_restriction
        self.check(sps(77, 2, 3, vui_timing()), profile=77, poc=2, refs=3, vui=1, br=0, order=1)

    def test_poc2_without_vui(self): self.check(sps(77, 2, 4), poc=2, refs=4, vui=0, order=1)

    def test_poc0_main_may_reorder(self): self.check(sps(77, 0, 4, vui_timing()), poc=0, refs=4, br=0, order=0)

    def test_baseline_poc0(self): self.check(sps(66, 0, 1), profile=66, poc=0, order=1)

    def test_restriction_says(self):   # the VUI's word wins over pic_order_cnt_type either way
        self.check(sps(77, 0, 3, vui_timing((0, 3))), br=1, reorder=0, order=1)
        self.check(sps(77, 2, 3, vui_timing((1, 3))), br=1, reorder=1, order=0)
        self.check(sps(66, 0, 1, vui_timing((2, 3))), br=1, reorder=2, order=0)

    def test_high_scaling_lists(self):   # lists of 16 and 64 entries, one ending early (a delta to 0 stops it), 4:2:0 and 4:4:4
        lists = {0: [-8], 1: [1] * 16, 6: [2] * 3 + [-14], 7: [0] * 64}
        self.check(sps(100, 2, 2, vui_timing((0, 2)), high=(1, lists)), profile=100, poc=2, refs=2, br=1, reorder=0, order=1)
        lists444 = {i: [1] * (16 if i < 6 else 64) for i in range(12)}
        self.check(sps(244, 0, 5, vui_timing((2, 5)), high=(3, lists444)), profile=244, poc=0, refs=5, br=1, reorder=2, order=0)
        self.check(sps(100, 2, 3, None, high=(1, None)), profile=100, poc=2, refs=3, vui=0, order=1)

    def test_poc1(self): self.check(sps(77, 1, 2, vui_timing(), poc1=(-2, 0, [2, -1, 3])), poc=1, refs=2, br=0, order=0)

    def test_full_vui_with_hrd_and_escapes(self):
        nal = sps(77, 0, 4, vui_full(0))
        self.assertIn(b'\x00\x00\x03', nal)   # the Extended_SAR's zeros needed an emulation prevention byte
        self.check(nal, poc=0, refs=4, vui=1, br=1, reorder=0, order=1)
        self.check(sps(77, 2, 4, vui_full(3)), br=1, reorder=3, order=0)

    def test_picture_buffer(self):   # Table A-1's MaxDpbMbs over the frame's macroblocks, at most 16, at least max_num_ref_frames
        self.check(sps(100, 0, 3, vui_timing(), high=(1, None), level=41), level=41, mbs=8160, dpb=4)   # 1080p at 4.1
        self.check(sps(100, 0, 5, vui_timing(), high=(1, None), level=41), dpb=5)   # more reference frames than the level allows: the references
        self.check(sps(100, 0, 3, vui_timing(), high=(1, None), level=31, mbs=(80, 45)), level=31, mbs=3600, dpb=5)   # 720p at 3.1 (Qualcomm kept 5 back without bitstream_restriction)
        self.check(sps(100, 0, 3, None, high=(1, None), level=40, mbs=(80, 45)), dpb=9)
        self.check(sps(100, 0, 3, None, high=(1, None), level=51, mbs=(80, 45)), dpb=16)
        self.check(sps(77, 0, 2, None, level=31, mbs=(80, 23), frames=0), mbs=3680, dpb=4)   # fields: map units are macroblock pairs
        self.check(sps(66, 0, 1, None, level=11, mbs=(11, 9), constraints=0x50), level=9, mbs=99, dpb=4)   # level 1b: 11 with constraint_set3_flag
        self.check(sps(66, 0, 1, None, level=11, mbs=(11, 9)), level=11, dpb=9)
        self.check(sps(77, 0, 2, None, level=99), dpb=16)   # a level the table does not have

    def test_not_an_sps_or_cut_short(self):
        nal = sps(77, 2, 3, vui_timing((0, 3)))
        self.assertFalse(self.parse(b'\x68' + nal[1:])[0])   # a PPS header
        self.assertEqual(self.lib.g_order(b'\x68' + nal[1:], len(nal)), 0)
        for cut in (3, 8, len(nal) - 2):
            self.assertFalse(self.parse(nal[:cut])[0], cut)
            self.assertEqual(self.lib.g_order(nal[:cut], cut), 0)


if __name__ == '__main__':
    unittest.main()
