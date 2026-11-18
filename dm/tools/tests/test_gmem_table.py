"""dm/android/gmem_table.h (the raw pool table of the Vulkan replay's raw global memory, laid out as nvn_mtl.m gmem_map lays it out on the Mac, and the translator's lookup
over it), built for the PC with its C compiler: every address must find the entry holding it, through the cached entry, a chunk hint, the page map after the table
(spv_gmpage.h) or the search, and an address in no range none. Skipped without a C compiler."""
import bisect
import ctypes
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
ENTRIES, ENTRY, HINT_SHIFT = 8192, 32, 21   # dmsh_abi.h DMSH_GMEM_MAP_ENTRIES, _ENTRY, DMSH_GMEM_HINT_SHIFT
PAGE, PAGE_BITS = 4096, 18   # gmem_table.h GMEM_PAGE_SHIFT, GMEM_PAGE_BITS
MB2 = 1 << HINT_SHIFT


def compiler():
    if sys.platform == 'darwin': return ['xcrun', 'clang']
    for c in ('clang', 'cc', 'gcc'):
        if shutil.which(c): return [c]
    return None


@unittest.skipIf(compiler() is None or os.name == 'nt', 'no C compiler')
class GmemTable(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp())
        src = cls.tmp / 't.c'
        src.write_text('#include "gmem_table.h"\n'
                       '#define X __attribute__((visibility("default")))\n'
                       'X int fill(uint8_t *t, const uint64_t *seg, int n, int *over) { return gmem_table_fill(t, (const uint64_t (*)[3])seg, n, over); }\n'
                       'X void find_many(const uint8_t *t, const uint64_t *a, uint32_t *out, int n, int paged) { uint32_t last = 0; for (int i = 0; i < n; i++) out[i] = gmem_table_find(t, a[i], &last, paged); }\n'
                       'X int pages(uint8_t *t, int e, int *lost) { return gmem_table_pages(t, e, lost); }\n'
                       'X int paged_bytes(void) { return GMEM_PAGED_BYTES; }\n')
        subprocess.run([*compiler(), '-shared', '-fPIC', '-O1', '-Wall', '-Wextra', '-Werror', '-I', REPO / 'dm/android', src, '-o', cls.tmp / 't.so'], check=True)
        cls.lib = ctypes.CDLL(str(cls.tmp / 't.so'))
        u8p, u64p, u32p = ctypes.POINTER(ctypes.c_uint8), ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(ctypes.c_uint32)
        cls.lib.fill.argtypes = [u8p, u64p, ctypes.c_int, ctypes.POINTER(ctypes.c_int)]; cls.lib.fill.restype = ctypes.c_int
        cls.lib.find_many.argtypes = [u8p, u64p, u32p, ctypes.c_int, ctypes.c_int]; cls.lib.find_many.restype = None
        cls.lib.pages.argtypes = [u8p, ctypes.c_int, ctypes.POINTER(ctypes.c_int)]; cls.lib.pages.restype = ctypes.c_int

    def build(self, segs, paged=False):   # segs: [(start, end)] sorted and disjoint; the delta is made up per entry. paged: the page map filled too
        t = (ctypes.c_uint8 * self.lib.paged_bytes())(); flat = []
        for k, (lo, hi) in enumerate(segs): flat += [lo, hi, (0x1000 * (k + 1)) & (2**64 - 1)]
        arr = (ctypes.c_uint64 * max(len(flat), 1))(*flat); over = ctypes.c_int()
        used = self.lib.fill(t, arr, len(segs), ctypes.byref(over))
        if paged: lost = ctypes.c_int(); self.mapped = self.lib.pages(t, used, ctypes.byref(lost)); self.lost = lost.value
        return t, used, over.value

    def find(self, t, addrs, paged=False):
        a = (ctypes.c_uint64 * len(addrs))(*addrs); out = (ctypes.c_uint32 * len(addrs))()
        self.lib.find_many(t, a, out, len(addrs), int(paged))
        return list(out)

    def page_word(self, t, page): return struct.unpack_from('<I', bytes(t), ENTRIES * ENTRY + 4 * (page % (1 << PAGE_BITS)))[0]

    def expect(self, segs, addrs, used):   # the entry holding each address, as a plain search over the ranges finds it (0: none)
        starts = [lo for lo, _ in segs[:used - 1]]; want = []
        for a in addrs:
            i = bisect.bisect_right(starts, a) - 1
            want.append(i + 1 if i >= 0 and a < segs[i][1] else 0)
        return want

    def probe(self, segs):   # addresses at, inside and around every range, then the same in a shuffled order (the cached entry carried between them)
        addrs = []
        for lo, hi in segs:
            addrs += [lo, lo + 4, (lo + hi) // 2 & ~3, hi - 4, hi, lo - 4 if lo >= 4 else 0, hi + MB2]
        rnd = random.Random(7); mixed = addrs[:]; rnd.shuffle(mixed)
        return addrs + mixed

    def test_layout(self):
        segs = [(0x5f00000000, 0x5f00200000), (0x5f00400000, 0x5f01000000)]
        t, used, over = self.build(segs)
        self.assertEqual((used, over), (3, 0))
        raw = bytes(t)
        self.assertEqual(struct.unpack_from('<3Q', raw, 0), (0, 0, 0))   # entry 0 empty
        for k, (lo, hi) in enumerate(segs, 1): self.assertEqual(struct.unpack_from('<2Q', raw, k * ENTRY), (lo, hi))
        self.assertEqual(struct.unpack_from('<Q', raw, 3 * ENTRY)[0], 2**64 - 1)   # an unused entry starts at ~0
        c = 0x5f00400000 >> HINT_SHIFT   # the second range's first whole chunk: hinted, tagged with the chunk
        self.assertEqual(struct.unpack_from('<I', raw, (c % ENTRIES) * ENTRY + 24)[0], c << 13 | 2)

    def test_lookups(self):
        # gaps, adjacent ranges, a range of many chunks, ranges shorter than a chunk, one above 4 GiB that starts mid-chunk, one past 2^40 (no hints there)
        segs = [(0x10000, 0x11000), (0x11000, 0x12000), (0x5f00001000, 0x5f00003000), (0x5f00200000, 0x5f40000000), (0x5f40000000, 0x5f40001000),
                (0x7f12345000, 0x7f12545000), (0x123_0000_0000, 0x123_0040_0000)]
        t, used, _ = self.build(segs)
        addrs = self.probe(segs)
        self.assertEqual(self.find(t, addrs), self.expect(segs, addrs, used))

    def test_colliding_hints(self):
        # chunks c and c + 8192 share a hint slot: the later range's hint replaces the earlier's, whose addresses must still be found through the search
        c = 0x2f800
        segs = [(c * MB2, (c + 2) * MB2), ((c + ENTRIES) * MB2, (c + ENTRIES + 2) * MB2)]
        t, used, _ = self.build(segs)
        addrs = self.probe(segs)
        self.assertEqual(self.find(t, addrs), self.expect(segs, addrs, used))

    def test_many_pieces(self):
        # pool pieces as Ocarina of Time's Kokiri Forest has them on the Galaxy Z Fold 8: about 3,600, from one page to 1 GiB, some touching, near 0x5f00000000
        rnd = random.Random(1); segs = []; a = 0x5ee0000000
        for _ in range(3600):
            a += rnd.choice([0, 0, 0x1000, 0x7000, 0x200000])
            n = rnd.choice([0x1000] * 8 + [0x3000, 0x40000, 0x2d00000, 0x13100000, 0x40000000])
            segs.append((a, a + n)); a += n
        t, used, over = self.build(segs)
        self.assertEqual((used, over), (3601, 0))
        addrs = self.probe(segs) + [rnd.randrange(segs[0][0], segs[-1][1]) & ~3 for _ in range(20000)]
        self.assertEqual(self.find(t, addrs), self.expect(segs, addrs, used))

    def test_full_table(self):
        # more ranges than entries: the first 8,191 are listed, the rest reported as left out, and their addresses find none
        segs = [(0x100000000 + k * 0x2000, 0x100000000 + k * 0x2000 + 0x1000) for k in range(ENTRIES + 10)]
        t, used, over = self.build(segs)
        self.assertEqual((used, over), (ENTRIES, 11))
        addrs = [segs[0][0], segs[ENTRIES - 2][0], segs[ENTRIES - 1][0] + 8, segs[-1][0]]
        self.assertEqual(self.find(t, addrs), [1, ENTRIES - 1, 0, 0])

    def test_page_map(self):
        # pieces smaller than a chunk or off a chunk boundary get their whole pages mapped; a hinted chunk's pages don't; a page cut by a piece's end isn't
        segs = [(0x5f00001000, 0x5f00003000), (0x5f00003000, 0x5f00003800), (0x5f00200000, 0x5f00600000), (0x5f00601000, 0x5f00800400)]
        t, used, _ = self.build(segs, paged=True)
        p = lambda a: a // PAGE
        self.assertEqual(self.page_word(t, p(0x5f00001000)) & 8191, 1)
        self.assertEqual(self.page_word(t, p(0x5f00002000)) >> 13, p(0x5f00002000) >> PAGE_BITS)   # tagged with the page's high bits
        self.assertEqual(self.page_word(t, p(0x5f00003000)), 0)   # piece 2 holds only half of its page
        self.assertEqual(self.page_word(t, p(0x5f00300000)), 0)   # piece 3's chunks are hinted
        self.assertEqual(self.page_word(t, p(0x5f00601000)) & 8191, 4); self.assertEqual(self.page_word(t, p(0x5f007ff000)) & 8191, 4)
        self.assertEqual(self.page_word(t, p(0x5f00800000)), 0)   # cut by the end
        self.assertEqual(self.mapped, 2 + 0x1ff); self.assertEqual(self.lost, 0)
        addrs = self.probe(segs)
        self.assertEqual(self.find(t, addrs, paged=True), self.expect(segs, addrs, used))

    def test_page_map_collisions(self):
        # pages 2^18 apart share a word: the first piece's page keeps it, the other's address searches and is found all the same; a page past 2^28 isn't mapped
        a = 0x5f00001000; b = a + (PAGE << PAGE_BITS); c = 0x123_0000_1000
        segs = [(a, a + 0x1000), (b, b + 0x1000), (c, c + 0x1000)]
        t, used, _ = self.build(segs, paged=True)
        self.assertEqual((self.mapped, self.lost), (1, 1))
        self.assertEqual(self.page_word(t, a // PAGE) & 8191, 1)
        addrs = self.probe(segs)
        self.assertEqual(self.find(t, addrs, paged=True), self.expect(segs, addrs, used))

    def test_many_pieces_paged(self):   # test_many_pieces with the page map: the same entries
        rnd = random.Random(1); segs = []; a = 0x5ee0000000
        for _ in range(3600):
            a += rnd.choice([0, 0, 0x1000, 0x7000, 0x200000])
            n = rnd.choice([0x1000] * 8 + [0x3000, 0x40000, 0x2d00000, 0x13100000, 0x40000000])
            segs.append((a, a + n)); a += n
        t, used, _ = self.build(segs, paged=True)
        self.assertGreater(self.mapped, 0)
        addrs = self.probe(segs) + [rnd.randrange(segs[0][0], segs[-1][1]) & ~3 for _ in range(20000)]
        self.assertEqual(self.find(t, addrs, paged=True), self.expect(segs, addrs, used))


if __name__ == '__main__':
    unittest.main()
