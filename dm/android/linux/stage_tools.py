# stage.sh's steps that are easier in Python, run on the Python being staged (the licences of PBS and the modules: dm/android/licenses/collect.py):
#   stage_tools.py debs <xcb_libs.tsv> <download folder> <lib folder> <licence folder>
#       the packages the list names, from Ubuntu's archive, checked against their SHA-256; their libraries (by soname) into <lib folder>, their copyright files
#       into <licence folder>
#   stage_tools.py elf-check <folder> <highest GLIBC_ version> <highest GLIBCXX_ version>
#       every ELF file under <folder>: the glibc and libstdc++ symbol versions it needs at most those, and each library it needs either in <folder> or one the
#       system provides (HOST below); readelf from binutils reads them
import hashlib
import io
import os
import re
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

UBUNTU = 'https://archive.ubuntu.com/ubuntu/'
# What DroidPlay takes from the system, as the README says: glibc and libstdc++ (GCC 11's or newer), OpenGL/EGL through libglvnd, X11 and Wayland's client
# libraries, fontconfig and FreeType, GLib, D-Bus, zlib and zstd: on AppImage's exclude list or on every desktop. GTK 3's stack only loads with
# Qt's GTK theme plugin, on a desktop that asks for it and so has GTK
HOST = {
    'libc.so.6', 'libm.so.6', 'libpthread.so.0', 'libdl.so.2', 'librt.so.1', 'libutil.so.1', 'ld-linux-x86-64.so.2', 'libstdc++.so.6', 'libgcc_s.so.1',
    'libGL.so.1', 'libEGL.so.1', 'libOpenGL.so.0', 'libX11.so.6', 'libX11-xcb.so.1', 'libxcb.so.1', 'libwayland-client.so.0',
    'libwayland-cursor.so.0', 'libwayland-egl.so.1', 'libfontconfig.so.1', 'libfreetype.so.6', 'libglib-2.0.so.0', 'libgthread-2.0.so.0', 'libgobject-2.0.so.0',
    'libgio-2.0.so.0', 'libdbus-1.so.3', 'libz.so.1', 'libzstd.so.1',
    'libgtk-3.so.0', 'libgdk-3.so.0', 'libpangocairo-1.0.so.0', 'libpango-1.0.so.0', 'libharfbuzz.so.0', 'libatk-1.0.so.0', 'libcairo-gobject.so.2',
    'libcairo.so.2', 'libgdk_pixbuf-2.0.so.0',
}


def fetch(url, dest, sha256=None):   # url into dest, unless a file of the right digest is there already; checked against sha256 when given
    dest = Path(dest)
    if sha256 and dest.is_file() and hashlib.sha256(dest.read_bytes()).hexdigest() == sha256: return dest
    dest.parent.mkdir(parents=True, exist_ok=True); part = dest.with_name(dest.name + '.part')
    with urllib.request.urlopen(urllib.request.Request(url, headers={'User-Agent': 'DroidPlay stage.sh'}), timeout=120) as r, open(part, 'wb') as f:
        shutil.copyfileobj(r, f, 1 << 20)
    if sha256 and (got := hashlib.sha256(part.read_bytes()).hexdigest()) != sha256:
        part.unlink(); sys.exit(f'{url}: SHA-256 {got}, the list says {sha256}')
    os.replace(part, dest)
    return dest


def ar_members(data):   # a .deb is an ar archive: its members' names and bytes
    if data[:8] != b'!<arch>\n': raise ValueError('not an ar archive')
    i = 8
    while i + 60 <= len(data):
        name, size = data[i:i + 16].decode().strip().rstrip('/'), int(data[i + 48:i + 58])
        yield name, data[i + 60:i + 60 + size]
        i += 60 + size + (size & 1)


def debs(tsv, cache, libdir, licdir):
    libdir, licdir = Path(libdir), Path(licdir); libdir.mkdir(parents=True, exist_ok=True); licdir.mkdir(parents=True, exist_ok=True)
    for line in Path(tsv).read_text(encoding='utf-8').splitlines():
        if not line.strip() or line.startswith('#'): continue
        pkg, path, sha = line.split('\t')
        deb = fetch(UBUNTU + path, Path(cache) / Path(path).name, sha)
        data = next(body for name, body in ar_members(deb.read_bytes()) if name.startswith('data.tar'))
        with tarfile.open(fileobj=io.BytesIO(data)) as t:
            members = {m.name.lstrip('./'): m for m in t.getmembers()}
            got = []
            for name, m in members.items():
                base = name.rsplit('/', 1)[-1]
                if name.startswith('usr/lib/x86_64-linux-gnu/') and re.fullmatch(r'lib.+\.so\.\d+', base):   # the soname, a link to the versioned file
                    real = members[str(Path(name).parent / m.linkname)] if m.issym() else m
                    src = t.extractfile(real)
                    assert src is not None
                    (libdir / base).write_bytes(src.read()); os.chmod(libdir / base, 0o755); got.append(base)
                elif name == f'usr/share/doc/{pkg}/copyright' and m.isfile():
                    src = t.extractfile(m)
                    assert src is not None
                    (licdir / f'{pkg}.copyright').write_bytes(src.read())
        if not got: sys.exit(f'{pkg}: no library in {path}')
        print(f'  {pkg}: {", ".join(got)}' + ('' if (licdir / f'{pkg}.copyright').is_file() else ' (no copyright file of its own)'))


def readelf(path):   # (sonames it needs, the symbol versions it needs), from readelf
    out = subprocess.run(['readelf', '-d', '-V', '-W', str(path)], capture_output=True, text=True, errors='replace').stdout
    return re.findall(r'\(NEEDED\)\s+Shared library: \[([^\]]+)\]', out), set(re.findall(r'Name: (GLIBC(?:XX)?_[0-9.]+)', out))


def vkey(v): return tuple(int(x) for x in v.split('_', 1)[1].split('.'))


def elf_check(root, glibc, glibcxx):
    root = Path(root); elves = []
    for d, _, names in os.walk(root):
        for n in names:
            p = Path(d) / n
            if p.is_symlink() or not p.is_file(): continue
            with open(p, 'rb') as f:
                if f.read(4) == b'\x7fELF': elves.append(p)
    bundled = {p.name for p in elves}
    worst, bad = {}, []
    for p in elves:
        needed, vers = readelf(p)
        for v in vers:
            kind = v.split('_')[0]
            if kind not in worst or vkey(v) > vkey(worst[kind][0]): worst[kind] = (v, p)
            limit = glibcxx if kind == 'GLIBCXX' else glibc
            if vkey(v) > vkey(f'X_{limit}'): bad.append(f'{p.relative_to(root)} needs {v}')
        bad += [f'{p.relative_to(root)} needs {n}, which is neither in the AppImage nor a library DroidPlay takes from the system'
                for n in needed if n.rsplit('/', 1)[-1] not in bundled and n not in HOST]   # libpython3.so names its library by a $ORIGIN path
    for kind, (v, p) in sorted(worst.items()): print(f'  highest {kind}: {v} ({p.relative_to(root)})')
    print(f'  {len(elves)} ELF files')
    if bad: sys.exit('ELF check failed:\n  ' + '\n  '.join(sorted(set(bad))))


if __name__ == '__main__':
    cmd, *a = sys.argv[1:]
    if cmd == 'debs': debs(*a)
    elif cmd == 'elf-check': elf_check(*a)
    else: sys.exit(f'unknown command {cmd}')
