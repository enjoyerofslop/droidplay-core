"""shader_split: Alchemy's IGA archives (Crash Bandicoot N. Sane Trilogy's archives/*.pak, version 12) are read through their member records and block tables (iga.py):
members under a shaders/ folder are inflated piece by piece (LZMA, 32 KB pieces, a piece stored as is when its entry's top bit is clear, a member stored whole when its
mode is 0xffffffff) and their GLSLC outputs taken; other members stay on disk. A layout the reader does not know, or a damaged piece, is named. No game files needed."""
import lzma
import os
import struct
import sys
import tempfile
import unittest
from unittest import mock
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools/fuzz'))
import fuzz_shader_split as fz   # shader_split with a profile
ss = fz.ss
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
import iga

LZMA1 = {'id': lzma.FILTER_LZMA1, 'dict_size': 0x8000, 'lc': 3, 'lp': 0, 'pb': 2}
PROPS = bytes([0x5d]) + struct.pack('<I', 0x8000)


def piece(raw):   # u32 stream length, the 5 property bytes, the raw stream
    s = lzma.compress(raw, format=lzma.FORMAT_RAW, filters=[LZMA1])
    return struct.pack('<I', len(s)) + PROPS + s


def archive(members, sector=0x200, version=12, garble=()):
    """members: (name, data, how) with how 'lzma', 'raw' (pieces stored as is) or 'stored' (mode 0xffffffff) → an IGA archive's bytes. Pieces start on sector bounds.
    garble: names whose stored bytes are replaced by bytes no LZMA decoder takes, to show they are never inflated"""
    n = len(members)
    tabs = {'s': [], 'm': [], 'l': []}; recs = []; body = bytearray(); data_at = 0x1000
    for name, data, how in members:
        while (data_at + len(body)) % sector: body.append(0)
        off = data_at + len(body)
        if how == 'stored':
            body += data; recs.append((off, len(data), 0xffffffff)); continue
        secs = -(-len(data) // sector)
        t = tabs['s'] if secs <= 0x7f else tabs['m'] if secs <= 0x7fff else tabs['l']
        flag = 0x80 if t is tabs['s'] else 0x8000 if t is tabs['m'] else 0x80000000
        first = len(t); start = len(body)
        for k in range(0, len(data), 0x8000):
            p = data[k:k + 0x8000]
            stored = (b'\xff' * len(piece(p)) if name in garble else piece(p)) if how == 'lzma' else p
            t.append((len(body) - start) // sector | (flag if how == 'lzma' else 0))
            body += stored
            while len(body) % sector: body.append(0)
        t.append((len(body) - start) // sector)
        recs.append((off, len(data), 2 << 28 | first))
    names = bytearray(4 * n)
    for i, (name, _, _) in enumerate(members):
        struct.pack_into('<I', names, 4 * i, len(names))
        names += f'temporary/build/{name}'.encode() + b'\0' + name.encode() + b'\0' + bytes(4)
    toc = bytearray()
    toc += b''.join(struct.pack('<I', i) for i in range(n))   # name hashes: not read
    for i, (off, size, mode) in enumerate(recs): toc += struct.pack('<IIII', off & 0xffffffff, off >> 32 | i << 8, size, mode)
    toc += struct.pack(f'<{len(tabs["l"])}I', *tabs['l']) + struct.pack(f'<{len(tabs["m"])}H', *tabs['m']) + bytes(tabs['s'])
    names_at = data_at + len(body)
    head = struct.pack('<4sIIIIIIIIIQII', b'IGA\x1a', version, len(toc), n, sector, 0, 0, len(tabs['l']), len(tabs['m']), len(tabs['s']), names_at, len(names), 1)
    out = bytearray(head + toc); assert len(out) <= data_at
    out += bytes(data_at - len(out)) + body + names
    return bytes(out)


class Archive(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory(); self.addCleanup(self.dir.cleanup)

    def write(self, b, name='shaders_nx.pak'):
        p = os.path.join(self.dir.name, name)
        with open(p, 'wb') as f: f.write(b)
        return p

    def test_shader_members_inflated_in_each_storage(self):
        big = bytes(range(256)) * 300   # 76,800 bytes: three pieces, past 0x7f sectors, so the medium table
        p = self.write(archive([('shaders/Standard.igz', big, 'lzma'), ('shaders/Small.igz', b'tiny' * 10, 'lzma'), ('shaders/Raw.igz', b'r' * 40000, 'raw'),
                                ('shaders/Whole.igz', b'whole member', 'stored'), ('models/crash.igz', b'm' * 5000, 'lzma')], garble={'models/crash.igz'}))
        got = list(iga.shader_files(p))
        self.assertEqual(got, [('temporary/build/shaders/Standard.igz', big), ('temporary/build/shaders/Small.igz', b'tiny' * 10),
                               ('temporary/build/shaders/Raw.igz', b'r' * 40000), ('temporary/build/shaders/Whole.igz', b'whole member')])

    def test_other_version_is_refused(self):
        p = self.write(archive([('shaders/a.igz', b'x', 'lzma')], version=11))
        with self.assertRaisesRegex(ValueError, 'IGA version 11'): list(iga.shader_files(p))

    def test_other_compression_is_refused(self):
        b = bytearray(archive([('shaders/a.igz', b'x' * 100, 'lzma')]))
        rec = 0x38 + 4
        struct.pack_into('<I', b, rec + 12, 1 << 28)
        with self.assertRaisesRegex(ValueError, 'IGA compression 1'): list(iga.shader_files(self.write(bytes(b))))

    def test_damaged_piece_is_named(self):
        p = self.write(archive([('shaders/a.igz', b'x' * 100, 'lzma')], garble={'shaders/a.igz'}))
        with self.assertRaisesRegex(ValueError, 'shaders/a.igz piece 0: '): list(iga.shader_files(p))

    def test_short_stream_is_refused(self):
        b = bytearray(archive([('shaders/a.igz', os.urandom(2048), 'lzma')]))
        struct.pack_into('<I', b, 0x1000, 20)   # the piece's stream length: the decoder runs out of input before the member's 2,048 bytes
        with self.assertRaisesRegex(ValueError, r'piece 0: LZMA stream gave \d+ bytes, not 2048'): list(iga.shader_files(self.write(bytes(b))))

    def test_blocks_past_the_table_are_refused(self):
        b = bytearray(archive([('shaders/a.igz', b'x' * 100, 'lzma')]))
        struct.pack_into('<I', b, 0x38 + 4 + 12, 2 << 28 | 5)
        with self.assertRaisesRegex(ValueError, 'pass the table'): list(iga.shader_files(self.write(bytes(b))))

    def test_truncated_archive_is_named(self):
        b = archive([('shaders/a.igz', b'x' * 5000, 'raw')])
        with self.assertRaisesRegex(ValueError, 'lies outside the archive'): list(iga.shader_files(self.write(b[:0x1100])))

    def test_split_reads_glslc_outputs_of_shader_members_only(self):
        seen = []
        out = b'\x91\x68\x86\x19' + b'output'
        p = self.write(archive([('shaders/Bloom.igz', out, 'lzma'), ('shaders/Plain.igz', b'no output here', 'lzma'), ('levels/l101.igz', out, 'lzma')]))
        with mock.patch.object(ss, 'glslc_shaders', lambda d, label, need=True, skip=(): seen.append((label, d)) or []):
            rel, keys, pairs, _ = ss.scan_file_in((p, 'archives/shaders_nx.pak'))
        self.assertEqual(seen, [('/archives/shaders_nx.pak/temporary/build/shaders/Bloom.igz', out)])
        self.assertEqual((rel, keys, pairs), ('archives/shaders_nx.pak', {}, set()))


if __name__ == '__main__':
    unittest.main()
