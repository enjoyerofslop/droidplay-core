"""Read the shader members of Alchemy's IGA archives without inflating the rest.

Vicarious Visions' engine (Crash Bandicoot N. Sane Trilogy) keeps every RomFS file in
archives/*.pak, IGA version 12, little-endian, its members LZMA-compressed in 32 KB
pieces. Only archives/shaders_nx.pak holds shaders: 83 .igz files under shaders/ with
2,249 whole GLSLC outputs. Inflating all 166 archives (9.9 GB) found no other member
holding a GLSLC output or BNSH, so members outside a shaders/ folder stay on disk.

Header, 0x38 bytes: magic 'IGA\\x1a', u32 version, u32 table size (counted from +0x38),
u32 member count, u32 sector size, u32 hash divider, u32 hash slop, u32 large, medium and
small block counts, u64 name table offset, u32 name table size, u32 flags. Then a u32
name hash per member, a 16-byte record per member {u40 offset, u24 ordinal, u32 length,
u32 mode}, and the block tables: large u32, medium u16, small u8 entries.

A mode of 0xffffffff is a member stored whole at its offset. Otherwise the top nibble is
the compression (2: LZMA) and the rest the member's first entry in one block table, the
table picked by the member's length in sectors (up to 0x7f small, up to 0x7fff medium,
else large). Entry i is piece i's sector from the member's offset, its top bit set when
the piece is compressed; one more entry gives the end. A compressed piece is u32 stream
length, the 5 LZMA property bytes and a raw LZMA stream without an end marker.

The name table is a u32 offset per member; at each, the member's build path and its name
in the archive, each NUL-ended. The build path is the one returned.
"""
import lzma
import os
import struct

MAGIC = b'IGA\x1a'
HEAD = struct.Struct('<4sIIIIIIIIIQII')
PIECE = 0x8000
LZMA = 2
STORED = 0xffffffff


def _shader_member(name):
    return 'shaders' in name.split('/')[:-1]


def _inflate(piece, want, where):
    if len(piece) < 9: raise ValueError(f'{where}: LZMA piece of {len(piece)} bytes')
    n, = struct.unpack_from('<I', piece, 0)
    if n > len(piece) - 9: raise ValueError(f'{where}: LZMA stream of {n} bytes in a piece of {len(piece)}')
    if piece[4] >= 9 * 5 * 5: raise ValueError(f'{where}: LZMA properties byte {piece[4]:#x}')
    dec = lzma.LZMADecompressor(lzma.FORMAT_ALONE)
    try: out = dec.decompress(piece[4:9] + struct.pack('<Q', want) + piece[9:9 + n], want + 1)
    except lzma.LZMAError as e: raise ValueError(f'{where}: damaged LZMA stream ({e})') from None
    if not dec.eof or len(out) != want: raise ValueError(f'{where}: LZMA stream gave {len(out)} bytes, not {want}')
    return out


def shader_files(path, *, opener=open, want=_shader_member):
    """Yield (member build path, bytes) for members want() takes: by default those under a shaders/ folder."""
    with opener(path, 'rb') as f:
        # A RomFS-image member's descriptor belongs to the whole image, so offsets are bounded against this stream
        f.seek(0, os.SEEK_END)
        length = f.tell()

        def read(at, n, what):
            if at < 0 or n < 0 or at + n > length: raise ValueError(f'{path}: {what} at {at:#x} of {n:#x} bytes lies outside the archive')
            f.seek(at)
            return f.read(n)

        magic, ver, toc, count, sector, _, _, nl, nm, ns, names_at, names_size, _ = HEAD.unpack(read(0, HEAD.size, 'header'))
        if magic != MAGIC or ver != 12: raise ValueError(f'{path}: IGA version {ver} (only version 12, Crash Bandicoot N. Sane Trilogy, is read)')
        if sector < 0x200 or sector & (sector - 1): raise ValueError(f'{path}: IGA sector size {sector:#x}')
        records = HEAD.size + 4 * count
        tables = records + 16 * count
        if tables + 4 * nl + 2 * nm + ns > HEAD.size + toc: raise ValueError(f'{path}: IGA tables of {count} members and {nl}/{nm}/{ns} blocks pass the table size {toc:#x}')
        t = read(0, HEAD.size + toc, 'table')
        blocks = ((struct.unpack_from(f'<{ns}B', t, tables + 4 * nl + 2 * nm), 0x7f, 0x80),
                  (struct.unpack_from(f'<{nm}H', t, tables + 4 * nl), 0x7fff, 0x8000),
                  (struct.unpack_from(f'<{nl}I', t, tables), 0x7fffffff, 0x80000000))
        if names_size < 4 * count: raise ValueError(f'{path}: IGA name table of {names_size:#x} bytes for {count} members')
        nt = read(names_at, names_size, 'name table')
        for i in range(count):
            at, = struct.unpack_from('<I', nt, 4 * i)
            end = nt.find(b'\0', at)
            if at < 4 * count or end < 0: raise ValueError(f'{path}: IGA member {i}: name at {at:#x} is outside the name table')
            name = nt[at:end].decode('utf-8', 'replace')
            if not want(name): continue
            lo, hi, size, mode = struct.unpack_from('<IIII', t, records + 16 * i)
            off = lo | (hi & 0xff) << 32
            where = f'{path}/{name}'
            if mode == STORED:
                yield name, read(off, size, name)
                continue
            if mode >> 28 != LZMA: raise ValueError(f'{where}: IGA compression {mode >> 28} (only LZMA, 2, is read)')
            sectors = -(-size // sector)
            tab, mask, flag = blocks[0] if sectors <= 0x7f else blocks[1] if sectors <= 0x7fff else blocks[2]
            first, pieces = mode & 0x0fffffff, -(-size // PIECE)
            if first + pieces + 1 > len(tab): raise ValueError(f'{where}: IGA blocks {first}..{first + pieces} pass the table of {len(tab)}')
            ents = tab[first:first + pieces + 1]
            secs = [e & mask for e in ents]
            if any(b < a for a, b in zip(secs, secs[1:])): raise ValueError(f'{where}: IGA piece sectors go backwards')
            stored = read(off + secs[0] * sector, (secs[-1] - secs[0]) * sector, name)
            out = bytearray()
            for k in range(pieces):
                a, b = (secs[k] - secs[0]) * sector, (secs[k + 1] - secs[0]) * sector
                n = min(PIECE, size - k * PIECE)
                if ents[k] & flag: out += _inflate(stored[a:b], n, f'{where} piece {k}')
                elif b - a < n: raise ValueError(f'{where} piece {k}: {b - a} bytes stored for {n}')
                else: out += stored[a:a + n]
            yield name, bytes(out)
