"""uepak.inline_needs_decoder: a synthetic version 4 legacy pak whose shaders the phone's Oodle decodes (flag 4, Custom) and one whose Zlib Python reads itself; no game files."""
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zlib

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
import uepak


def fstr(s):
    b = s.encode() + b'\0'
    return struct.pack('<i', len(b)) + b


def entry(off, size, raw, meth, blocks, bsz):
    """FPakEntry as the index and the data header write it (version 4: block bounds from the file's start)"""
    b = struct.pack('<qqqi', off, size, raw, meth) + bytes(20)
    if meth: b += struct.pack('<i', len(blocks)) + b''.join(struct.pack('<qq', s, e) for s, e in blocks)
    return b + struct.pack('<BI', 0, bsz)


def pak(path, meth):
    """A version 4 legacy pak holding one cooked package compressed with one method: the flags have no names, and flag 4 (Custom) is a legacy pak's Oodle (Stray), 1 its Zlib, 3 DRAGON QUEST XI S's LZ5"""
    raw = b'LSLG' + bytes(range(256)) * 8
    data = zlib.compress(raw)
    head = len(entry(0, 0, 0, meth, [(0, 0)], 0))
    off = head   # the data starts right after the FPakEntry the pak repeats in front of it
    e = entry(off, len(data), len(raw), meth, [(off, off + len(data))], 0x10000)
    body = e + data
    index = fstr('../../../') + struct.pack('<i', 1) + fstr('Stray/Content/M_Test.uexp') + e
    foot = struct.pack('<BIIQQ', 0, uepak.PAK_MAGIC, 4, len(body), len(index)) + bytes(20)
    path.write_bytes(body + index + foot)


class Version4Oodle(unittest.TestCase):
    def make(self, meth):
        tmp = tempfile.TemporaryDirectory(); self.addCleanup(tmp.cleanup)
        p = Path(tmp.name) / 'pakchunk0-Switch.pak'
        pak(p, meth)
        return tmp.name

    def test_a_custom_pak_needs_the_decoder(self):   # flag 4, Custom: a legacy pak's Oodle (Stray's paks)
        self.assertTrue(uepak.inline_needs_decoder(self.make(4)))

    def test_a_zlib_pak_does_not(self):   # Python reads Zlib itself: no phone (OCTOPATH TRAVELER II's paks are version 11 and Zlib, a legacy one the same)
        self.assertFalse(uepak.inline_needs_decoder(self.make(1)))

    def test_lz5_does_not_either(self):   # DRAGON QUEST XI S: the Mac host decodes LZ5, the phone's Oodle does not read a version 4 pak
        self.assertFalse(uepak.inline_needs_decoder(self.make(3)))


if __name__ == '__main__':
    unittest.main()
