"""AnvilNext .forge archives (dm/gfx/tools/anvil_forge.py) and their place in shader_split.py: the LZO1X decoder on hand-made streams of every instruction kind and on damaged ones, a
synthetic version 27 file read back, its BNSH found through anvil_jobs / anvil_shaders, synthetic version 25 and 31 files (the Ezio Collection's layout) whose Oodle chunks go to the decoder
given (a stand-in here: the game's own Oodle runs only in its host), their bare control sections and code areas found by anvil_raw, the Oodle forges counted for the NSP scripts
(profile_guess.oodle_forges), and the profile's shader_separate pairing every vertex shader. No game files needed."""
import functools
import os
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools/fuzz'))
import fuzz_shader_split as fz   # its BNSH builder and shader_split with a profile
ss = fz.ss
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
import anvil_forge as af   # noqa: E402
sys.path.insert(0, str(REPO / 'dm/games'))
import profile_guess as pg   # noqa: E402

END = bytes([0x11, 0, 0])   # M4 with distance 0: the end marker


def run_bytes(t, extra):
    """A length past what the instruction holds: zero bytes of 255 each, then the rest (1..255)"""
    t -= extra; k = (t - 1) // 255
    return bytes(k) + bytes([t - 255 * k])


def lzo1x_rle(b):
    """LZO1X that writes each run of the byte before it (3 bytes or more) as an M3 match of distance 1 and the rest as literals: the matches carry the 1 to 3 literals after them in their
    low bits, longer literal runs follow a match whose low bits are 0"""
    seq, i, lit = [], 0, bytearray()
    while i < len(b):
        n = 0
        while i and i + n < len(b) and b[i + n] == b[i - 1]: n += 1
        if n >= 3: seq.append((bytes(lit), n)); lit = bytearray(); i += n
        else: lit.append(b[i]); i += 1
    seq.append((bytes(lit), 0))
    out = bytearray()
    for j, (lits, n) in enumerate(seq):
        if j == 0:
            if len(lits) <= 238: out += bytes([17 + len(lits)]) + lits if lits else b''
            else: out += b'\0' + run_bytes(len(lits) - 3, 15) + lits
        elif len(lits) >= 4: out += (bytes([len(lits) - 3]) if len(lits) - 3 <= 15 else b'\0' + run_bytes(len(lits) - 3, 15)) + lits
        else: out += lits   # 0 to 3: the previous match's low bits said so
        if n:
            after = len(seq[j + 1][0])
            out += bytes([32 | (n - 2)]) if n - 2 <= 31 else b'\x20' + run_bytes(n - 2, 31)
            out += struct.pack('<H', (0 << 2) | (after if after <= 3 else 0))   # distance 1
    return bytes(out + END)


class Lzo1x(unittest.TestCase):
    def test_instructions(self):
        lits = bytes(range(256)) * 64 + b'\x01'   # 0x4001 bytes: far enough back for M4
        cases = [
            ('a literal run at the start', bytes([17 + 5]) + b'hello' + END, b'hello'),
            ('M2: 4 bytes 4 back', bytes([17 + 4]) + b'abcd' + bytes([(3 << 5) | (3 << 2), 0]) + END, b'abcdabcd'),
            ('M2 overlapping its own output', bytes([17 + 1]) + b'a' + bytes([4 << 5, 0]) + END, b'a' * 6),
            ('M3 with an extended length', bytes([17 + 3]) + b'xyz' + bytes([0x20, 7, 2 << 2, 0]) + END, (b'xyz' * 15)[:43]),
            ('a long literal run, then M4 0x4001 back',
             b'\0' + run_bytes(len(lits) - 3, 15) + lits + bytes([16 | 3, 1 << 2, 0]) + END, lits + lits[:5]),
            ('the 3-byte match 0x801 back after a long literal run', b'\0' + run_bytes(len(lits) - 3, 15) + lits + b'\0\0' + END, lits + lits[-0x801:][:3]),
            ('a match\'s 2 literals, then the 2-byte match', bytes([17 + 4]) + b'abcd' + bytes([(3 << 5) | (3 << 2) | 2, 0]) + b'XY' + bytes([1 << 2, 0]) + END,
             b'abcdabcdXY' + b'XY'),
        ]
        for what, src, want in cases:
            with self.subTest(what): self.assertEqual(af.lzo1x(src, len(want)), want)

    def test_round_trip(self):   # a BNSH (mostly zeros) through the run encoder above: long M3 runs and literals carried in their low bits
        for b in (fz.bnsh(), bytes(0x8000), b'ab' + bytes(40) + b'cdefgh' + bytes(3) + b'i'):
            self.assertEqual(af.lzo1x(lzo1x_rle(b), len(b)), b)
            self.assertLess(len(lzo1x_rle(b)), len(b))

    def test_damaged(self):
        cases = [
            ('a match before the start', bytes([17 + 1]) + b'a' + bytes([(2 << 5) | (7 << 2), 0]) + END, 4, 'lies outside'),
            ('more output than the chunk', bytes([17 + 5]) + b'hello' + END, 4, 'runs past'),
            ('no end marker', bytes([17 + 5]) + b'hello', 5, 'without its end marker'),
            ('the end marker early', bytes([17 + 5]) + b'hello' + END, 6, 'not 0x6'),
            ('a literal run past the stream', bytes([17 + 9]) + b'abc', 9, 'runs past'),
        ]
        for what, src, n, msg in cases:
            with self.subTest(what), self.assertRaises(ValueError) as e: af.lzo1x(src, n, 'chunk')
            self.assertIn(msg, str(e.exception)); self.assertTrue(str(e.exception).startswith('chunk: '))


def block(data, compress=True, typ=af.LZO1X):
    """One block of one chunk: LZO1X when compress, else stored"""
    c = lzo1x_rle(data) if compress else data
    return af.BLOCK + struct.pack('<HBHHH', 1, typ, 0x8000, 0x8000, 1) + struct.pack('<HH', len(data), len(c)) + struct.pack('<I', 0x12345678) + c


def forge27(ents):
    """A version 27 .forge file of [(name, entry bytes)]: header, file data header, one table, the index, the name records, then the entries"""
    hdr, tab = 0x20, 0x60; idx = tab + 0x30; names = idx + af.INDEX * len(ents); data = names + af.NAME_REC * len(ents)
    d = bytearray(data)
    d[:9] = af.MAGIC; struct.pack_into('<Iq', d, 9, af.VERSION, hdr)
    struct.pack_into('<IIqIqIIq', d, hdr, len(ents), len(ents), 0, 0, -1, 5000, 1, tab)
    struct.pack_into('<IIqqIIqq', d, tab, len(ents), len(ents), idx, -1, 0, len(ents) - 1, names, data)
    for i, (name, e) in enumerate(ents):
        struct.pack_into('<qQI', d, idx + af.INDEX * i, len(d), 0x1000 + i, len(e))
        rec = names + af.NAME_REC * i; struct.pack_into('<IQ', d, rec, len(e), 0x1000 + i); d[rec + af.NAME_AT:rec + af.NAME_AT + len(name)] = name.encode()
        d += e
    return bytes(d)


TABLE = b'resource table' + bytes(30)
ENTS = [('GlobalMetaFile', b'\x01\0\0\0' * 8), ('MAT_Test', b'\x04' + bytes(7) + block(TABLE, False) + block(fz.bnsh())), ('TEX_Test', bytes(8) + block(bytes(0x400) + b'texels'))]


class Forge(unittest.TestCase):
    def test_entries_and_blocks(self):
        d = forge27(ENTS)
        self.assertEqual(af.version(d[:13]), 27); self.assertIsNone(af.version(b'BNSH' + bytes(9)))
        es = af.entries(d)
        self.assertEqual([n for _, _, n in es], ['GlobalMetaFile', 'MAT_Test', 'TEX_Test'])
        self.assertEqual(af.read(d, *es[0][:2]), [])   # no blocks: nothing
        self.assertEqual(af.read(d, *es[1][:2]), [TABLE, fz.bnsh()])   # a stored chunk and an LZO1X one
        self.assertEqual(af.read(d, *es[2][:2]), [bytes(0x400) + b'texels'])

    def test_damaged(self):
        d = forge27(ENTS)
        hdr = 0x20; tab = struct.unpack_from('<q', d, hdr + 0x24)[0]
        def changed(at, fmt, *v): b = bytearray(d); struct.pack_into(fmt, b, at, *v); return bytes(b)
        cases = [
            ('a version the reader doesn\'t know (26)', changed(9, '<I', 26), 'not an AnvilNext forge of version 25, 27, 31'),
            ('a table outside the file', changed(hdr + 0x24, '<q', len(d) + 0x100), 'outside the file'),
            ('a table that points at itself', changed(tab + 0x10, '<q', tab), 'read twice'),
            ('an entry past the end', changed(tab + 0x30 + af.INDEX, '<q', len(d) - 4), 'is outside the file'),
            ('a count the tables don\'t hold', changed(hdr, '<I', 4), 'the header counts 4'),
        ]
        for what, b, msg in cases:
            with self.subTest(what), self.assertRaises(ValueError) as e: af.entries(b, 'x.forge')
            self.assertIn(msg, str(e.exception))
        for what, ent, msg in (('a block of another type (2)', b'\x04' + bytes(7) + block(TABLE, typ=2), 'type 2'),
                               ('a chunk past its entry', b'\x04' + bytes(7) + block(fz.bnsh())[:-4], 'runs past the entry')):
            b = forge27([('E', ent)]); off, size, _ = af.entries(b)[0]
            with self.subTest(what), self.assertRaises(ValueError) as x: af.read(b, off, size, 'E')
            self.assertIn(msg, str(x.exception))

    def test_split_reads_the_bnsh(self):
        with tempfile.TemporaryDirectory() as t:
            p = os.path.join(t, 'DataNX_Test.forge'); Path(p).write_bytes(forge27(ENTS))
            other = os.path.join(t, 'Other.forge'); Path(other).write_bytes(b'scimitar\0' + struct.pack('<I', 26) + bytes(0x40))   # a version the reader doesn't know: not taken
            jobs, taken = ss.anvil_jobs([(p, 'DataNX_Test.forge'), (other, 'Other.forge')])
            self.assertEqual(taken, {p})
            found = []
            for path, rel, (kind, ents) in jobs: self.assertEqual(kind, ss.ANVIL); ss.anvil_shaders(path, rel, ents, found)
            self.assertEqual(sorted((w, st) for w, st, _, _ in found), [('/DataNX_Test.forge#MAT_Test#BNSH@0x0#var0', 'ps'), ('/DataNX_Test.forge#MAT_Test#BNSH@0x0#var0', 'vs')])
            self.assertEqual({(ctl, code) for _, _, ctl, code in found}, {fz.VS, fz.PS})


def oodle_stream(b):   # a stand-in for the game's Oodle, which only its host runs: the real streams start 8C 0C (version 31) or 8C 0A (25)
    return b'\x8c\x0c' + zlib.compress(b)


def undo(c, n):
    assert c[:2] == b'\x8c\x0c'; return zlib.decompress(c[2:])


def recorded(calls, c, n):   # undo, noting each raw size asked for
    calls.append(n); return undo(c, n)


def oblock(data, typ=6, step=0x8000):
    """One block of Oodle chunks of at most step bytes each, a chunk that would not get smaller stored as it is"""
    parts = [data[i:i + step] for i in range(0, len(data), step)]
    cs = [c if len(c := oodle_stream(p)) < len(p) else p for p in parts]
    return (af.BLOCK + struct.pack('<HBHHH', 1, typ, 0x8000, 0x8000, len(parts)) + b''.join(struct.pack('<HH', len(p), len(c)) for p, c in zip(parts, cs))
            + b''.join(struct.pack('<I', 0x12345678) + c for c in cs))


def forge31(ents, ver=31):
    """A version 25 or 31 .forge file of [(name, data)]: as forge27, but the file data header keeps the first table at +0x20, the index entries are 16 bytes, the name records 0xbc, and each
    entry is its 0x1b8-byte FILEDATA header (the name, the id and size at +0x187) and then the data, 0x800-aligned"""
    first, fmt, rec, head = af.LAYOUTS[ver]; isz = struct.calcsize(fmt)
    hdr, tab = 0x416, 0x43e; idx = tab + 0x30; names = idx + isz * len(ents); data = names + rec * len(ents)
    d = bytearray(data)
    d[:9] = af.MAGIC; struct.pack_into('<Iq', d, 9, ver, hdr)
    struct.pack_into('<IIqqIIq', d, hdr, len(ents), 1, 0, -1, 5000, 1, tab); assert first == 0x20
    struct.pack_into('<IIqqIIqq', d, tab, len(ents), 1, idx, -1, 0, 4999, names, data)
    for i, (name, e) in enumerate(ents):
        d += bytes(-len(d) % 0x800)
        struct.pack_into(fmt, d, idx + isz * i, len(d), 0x100 + i, len(e))
        r = names + rec * i; struct.pack_into('<IQ', d, r, len(e), 0x5000 + i); d[r + af.NAME_AT:r + af.NAME_AT + len(name)] = name.encode()
        h = bytearray(head); h[:8] = af.FILEDATA; h[8:8 + len(name)] = name.encode(); struct.pack_into('<II', h, 0x187, 0x100 + i, len(e))
        d += h + e
    return bytes(d)


def deps(*ids):   # an entry's own table before its blocks: a count, then {u32 id, u32 kind} per entry it refers to
    return struct.pack('<I', len(ids)) + b''.join(struct.pack('<II', i, 2) for i in ids)


# The Ezio Collection's shader entry: no BNSH, each shader a control section directly followed by its code area, at odd offsets among the object's fields; a control magic by chance
# (not followed by a code area) in between
STRAY = 0x40 + len(fz.VS[0]) + len(fz.VS[1]) + 3
SHADERS = (b'\x7a\x3c\xfb\xbc' + b'AC2_Test\0' + bytes(0x40 - 13) + fz.VS[0] + fz.VS[1] + b'\x01\x02\x03' + ss.CTL_MAGIC + bytes(0x7c0) + b'\x05' + fz.PS[0] + fz.PS[1] + bytes(9))
RES = struct.pack('<H', 1) + struct.pack('<IIH', 0x100, len(SHADERS), 0xffff)   # the resource table: one resource, its id and size
ENTS31 = [('GlobalMetaFile', bytes(0xcf)), ('AC2_Test', deps() + oblock(RES) + oblock(SHADERS)), ('', deps(0x101, 0x102) + oblock(b'\x05\0' + bytes(40)) + oblock(bytes(0x9000) + b'cell'))]


class Oodle(unittest.TestCase):
    def test_entries_and_blocks(self):
        for ver, typ in ((31, 6), (25, 4)):
            ents = [(n, e.replace(struct.pack('<HB', 1, 6), struct.pack('<HB', 1, typ))) for n, e in ENTS31]   # the block type (no other place has these bytes)
            d = forge31(ents, ver)
            with self.subTest(ver):
                self.assertEqual(af.version(d[:13]), ver)
                es = af.entries(d)
                self.assertEqual([n for _, _, n in es], ['GlobalMetaFile', 'AC2_Test', '0x102'])   # an entry without a name: its id
                self.assertTrue(all(d[off - 0x1b8:off - 0x1b0] == af.FILEDATA for off, _, _ in es))   # the offset after each entry's own header, the size the index gives
                self.assertEqual([size for _, size, _ in es], [len(e) for _, e in ents])
                calls = []; dec = functools.partial(recorded, calls)
                self.assertEqual(af.read(d, *es[0][:2], decode=dec), [])
                self.assertEqual(af.read(d, *es[1][:2], decode=dec), [RES, SHADERS])
                self.assertEqual(af.read(d, *es[2][:2], decode=dec), [b'\x05\0' + bytes(40), bytes(0x9000) + b'cell'])
                self.assertEqual(calls, [len(SHADERS), 42, 0x8000, 0x1004])   # only the chunks stored smaller (not the 12-byte table), each with its raw size
                self.assertTrue(af.oodle(d))
        self.assertFalse(af.oodle(forge27(ENTS)))

    def test_damaged(self):
        d = forge31(ENTS31)
        es = af.entries(d); off, size, _ = es[1]
        with self.assertRaises(ValueError) as e: af.read(d, off, size, 'AC2_Test')
        self.assertIn('no decoder', str(e.exception))
        with self.assertRaises(ValueError) as e: af.read(d, off, size, 'AC2_Test', lambda c, n: bytes(n - 1))
        self.assertIn('decoded to', str(e.exception))
        idx = 0x43e + 0x30
        def changed(at, fmt, *v): b = bytearray(d); struct.pack_into(fmt, b, at, *v); return bytes(b)
        cases = [('an entry without its FILEDATA header', changed(es[1][0] - 0x1b8, '<8s', b'FILEDAT\0'), 'FILEDATA header'),
                 ('an entry whose data runs past the end', changed(idx + 16 * 2 + 12, '<I', len(d) - es[2][0] + 1), 'is outside the file'),
                 ('the first table read where version 27 keeps it', changed(0x416 + 0x20, '<q', len(d)), 'outside the file')]
        for what, b, msg in cases:
            with self.subTest(what), self.assertRaises(ValueError) as e: af.entries(b, 'x.forge')
            self.assertIn(msg, str(e.exception))

    def test_split_reads_the_bare_shaders(self):
        with tempfile.TemporaryDirectory() as t:
            p = os.path.join(t, 'DataNX_Test.forge'); Path(p).write_bytes(forge31(ENTS31))
            jobs, taken = ss.anvil_jobs([(p, 'DataNX_Test.forge')])
            self.assertEqual(taken, {p})
            found = []
            with mock.patch.object(ss, 'oodle', undo):   # the game's decoder in the Mac host (or the phone's cache) in a real split
                for path, rel, (kind, ents) in jobs: self.assertEqual(kind, ss.ANVIL); ss.anvil_shaders(path, rel, ents, found)
            vs = 0x40; ps = STRAY + 4 + 0x7c0 + 1
            self.assertEqual(sorted((w, st) for w, st, _, _ in found), [(f'/DataNX_Test.forge#AC2_Test#RAW@{ps:#x}', 'ps'), (f'/DataNX_Test.forge#AC2_Test#RAW@{vs:#x}', 'vs')])
            self.assertEqual({(ctl, code) for _, _, ctl, code in found}, {fz.VS, fz.PS})
        self.assertEqual(ss.anvil_raw(SHADERS[:-9 - len(fz.PS[1]) // 2], 'x'), [('x#RAW@0x40', 'vs', *fz.VS)])   # a code area cut short: not a shader

    def test_main_read_when_the_forges_are_oodle(self):   # anvil_main: AC2's main embeds the same objects, a compute shader among them with no code header
        cs_ctl = bytearray(0x880); cs_ctl[:4] = ss.CTL_MAGIC; code = bytes.fromhex('f00720e2001c0000') * 4
        struct.pack_into('<I', cs_ctl, 0x20, 0x880); struct.pack_into('<I', cs_ctl, 0x704, len(code)); struct.pack_into('<3I', cs_ctl, 0x748, 8, 8, 1)
        img = bytes(0x101) + bytes(cs_ctl) + code + bytes(3) + fz.VS[0] + fz.VS[1]
        with tempfile.TemporaryDirectory() as t:
            ez, rebel = os.path.join(t, 'a.forge'), os.path.join(t, 'b.forge'); Path(ez).write_bytes(forge31(ENTS31, 25)); Path(rebel).write_bytes(forge27(ENTS))
            got = ss.anvil_main(img, 'main', [rebel, ez])
            self.assertEqual([(w, st) for w, st, _, _ in got], [('main#RAW@0x101', 'cs'), (f'main#RAW@{0x101 + 0x880 + len(code) + 3:#x}', 'vs')])
            self.assertEqual(ss.anvil_main(img, 'main', [rebel]), [])   # the Rebel Collection's main: left to the other readers, as before

    def test_oodle_forges_counted_for_the_nsp_scripts(self):   # profile_guess.build_sources: oodle_packs, so the split gets the game's decoder (the Mac host, or the phone on Android)
        with tempfile.TemporaryDirectory() as t:
            for name, b in (('a/DataNX.forge', forge31(ENTS31)), ('b/DataNX.forge', forge31(ENTS31, 25)), ('c/DataNX.forge', forge27(ENTS)), ('d/Bad.forge', forge31(ENTS31)[:0x500]),
                            ('e/Other.forge', b'scimitar\0' + struct.pack('<I', 26) + bytes(0x40)), ('f/DataNX.bin', forge31(ENTS31))):
                Path(t, name).parent.mkdir(); Path(t, name).write_bytes(b)
            self.assertEqual(pg.oodle_forges(t), 2)   # not the LZO1X one, a cut one, an unknown version or another extension
        on, why = pg.shader_sources({'nvnBootstrapLoader'}, 0, [], 0, 0, oodle_forge=2)['oodle_packs']
        self.assertTrue(on); self.assertIn('2 AnvilNext .forge archive(s) of Oodle blocks', why)
        self.assertFalse(pg.shader_sources({'nvnBootstrapLoader'}, 0, [], 0, 0)['oodle_packs'][0])


class Separate(unittest.TestCase):
    def test_every_vertex_shader_gets_the_cover(self):
        keys = {'v1': ('/a.forge#M#BNSH@0x0#var0', 'vs', b'', b'v1'), 'v2': ('main#BNSH@0x40#var0', 'vs', b'', b'v2'),
                'p1': ('/a.forge#N#BNSH@0x0#var0', 'ps', b'', b'p1'), 'p2': ('/a.forge#O#BNSH@0x0#var0', 'ps', b'', b'p2')}
        reads = {b'p1': {3}, b'p2': {0, 1}}
        with mock.patch.object(ss, 'ps_inputs', lambda code: reads[code]):
            with mock.patch.object(ss, 'SEPARATE', False): self.assertEqual(ss.separable_pairs(keys), (set(), []))   # BNSH pair their own stages
            with mock.patch.object(ss, 'SEPARATE', True):
                pairs, cover = ss.separable_pairs(keys)
        self.assertEqual(cover, ['p2', 'p1']); self.assertEqual(pairs, {(v, p) for v in ('v1', 'v2') for p in ('p1', 'p2')})


if __name__ == '__main__':
    unittest.main()
