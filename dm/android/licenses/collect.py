# The licences of what DroidPlay's packages carry besides DroidPlay Core, gathered by both stage scripts (dm/android/linux/stage.sh for the AppImage,
# dm/android/windows/stage.ps1 for Setup) into the package's licenses folder, so the two ship the same notices: the texts in this folder (LGPL-3.0, GPL-3.0
# and GPL-2.0 for Qt and PySide6, Apache-2.0 for OpenSSL) are copied by the stage scripts themselves. Run on the Python being staged (3.14: tarfile reads zstd).
#   collect.py pbs <runtime> <download folder> <licence folder>
#       the licence files of python-build-standalone's build of this runtime: its install archive, which uv installs, has CPython's LICENSE.txt but not the
#       licences of the libraries built into it (OpenSSL, SQLite, libffi, XZ, zstd...). Its "full" archive of the same build has them, under python/licenses
#   collect.py modules <site-packages> <licence folder>
#       each installed module's licence files (its dist-info's licenses/, else the files named like a licence) into <licence folder>/<module>; prints a
#       Markdown line per module with its version and licence, for NOTICES.md
#   collect.py notices <template> <out>
#       the template with each @NAME@ replaced by the environment variable NAME (stage.sh and stage.ps1 set them)
import hashlib
import os
import re
import shutil
import sys
import tarfile
import urllib.request
from pathlib import Path

PBS = 'https://github.com/astral-sh/python-build-standalone/releases/download/'
FULL = {'win32': 'x86_64-pc-windows-msvc-pgo-full', 'linux': 'x86_64-unknown-linux-gnu-pgo+lto-full'}   # the flavour uv's install archives are cut from


def fetch(url, dest, sha256=None):   # url into dest, unless a file of the right digest is there already; checked against sha256 when given
    dest = Path(dest)
    if sha256 and dest.is_file() and hashlib.sha256(dest.read_bytes()).hexdigest() == sha256: return dest
    dest.parent.mkdir(parents=True, exist_ok=True); part = dest.with_name(dest.name + '.part')
    with urllib.request.urlopen(urllib.request.Request(url, headers={'User-Agent': 'DroidPlay stage'}), timeout=120) as r, open(part, 'wb') as f:
        shutil.copyfileobj(r, f, 1 << 20)
    if sha256 and (got := hashlib.sha256(part.read_bytes()).hexdigest()) != sha256:
        part.unlink(); sys.exit(f'{url}: SHA-256 {got}, the list says {sha256}')
    os.replace(part, dest)
    return dest


def pbs(runtime, cache, licdir):
    runtime, licdir = Path(runtime), Path(licdir)
    build = (runtime / 'BUILD').read_text(encoding='utf-8').strip()   # python-build-standalone's release, which uv installed from
    ver = '.'.join(map(str, sys.version_info[:3]))
    name = f'cpython-{ver}+{build}-{FULL["win32" if sys.platform == "win32" else "linux"]}.tar.zst'
    sums = fetch(f'{PBS}{build}/SHA256SUMS', Path(cache) / f'pbs-{build}-SHA256SUMS').read_text(encoding='utf-8')
    sha = next((ln.split()[0] for ln in sums.splitlines() if ln.split()[-1:] == [name]), None)
    if not sha: sys.exit(f'{name} is not in python-build-standalone {build}\'s SHA256SUMS')
    arc = fetch(f'{PBS}{build}/{name.replace("+", "%2B")}', Path(cache) / name, sha)
    licdir.mkdir(parents=True, exist_ok=True); names = []
    with tarfile.open(arc, 'r|zst') as t:   # read as a stream: only python/licenses is wanted out of the whole build
        for m in t:
            if m.isfile() and m.name.startswith('python/licenses/'):
                src = t.extractfile(m)
                assert src is not None
                (licdir / Path(m.name).name).write_bytes(src.read()); names.append(Path(m.name).name)
    if not names: sys.exit(f'{name} has no python/licenses')
    print(f'  python-build-standalone {build}: {", ".join(sorted(names))}', file=sys.stderr)


def field(meta, key):
    m = re.search(rf'^{key}: (.+)$', meta, re.M)
    return m[1].strip() if m else ''


def modules(site, licdir):
    site, licdir = Path(site), Path(licdir); lines = []
    for d in sorted(site.glob('*.dist-info'), key=lambda p: p.name.lower()):
        meta = (d / 'METADATA').read_text(encoding='utf-8', errors='replace')
        name, ver, lic = field(meta, 'Name'), field(meta, 'Version'), field(meta, 'License-Expression') or field(meta, 'License')
        out = licdir / name; out.mkdir(parents=True, exist_ok=True)
        if (d / 'licenses').is_dir(): shutil.copytree(d / 'licenses', out, dirs_exist_ok=True)
        else:
            for f in d.iterdir():
                if f.is_file() and re.search(r'licen[cs]e|copying|notice', f.name, re.I): shutil.copy2(f, out / f.name)
        lines.append(f'- {name} {ver}' + (f' ({lic})' if lic and len(lic) < 80 else ''))
    print('\n'.join(lines))


def notices(template, out):
    t = Path(template).read_text(encoding='utf-8')
    Path(out).write_text(re.sub(r'@([A-Z_]+)@', lambda m: os.environ[m.group(1)].rstrip(), t), encoding='utf-8', newline='\n')


if __name__ == '__main__':
    cmd, *a = sys.argv[1:]
    if cmd == 'pbs': pbs(*a)
    elif cmd == 'modules': modules(*a)
    elif cmd == 'notices': notices(*a)
    else: sys.exit(f'unknown command {cmd}')
