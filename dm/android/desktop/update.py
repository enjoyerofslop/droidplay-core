# The desktop app's updates (scripts/nxnative_android.py): the newest release on GitHub, and its DroidPlay-Setup or AppImage downloaded and checked before the
# app puts it in place. Without Qt, so the packing tests (dm/tools/tests/test_android_cli.py, test_android_linux.py) run it where PySide6 isn't installed.
#   The app says a newer release is out, as DroidPlay for Mac does. A copy DroidPlay-Setup installed (dm/android/windows/droidplay.iss) also offers Update:
#   the release's Setup, downloaded by the app and checked against the SHA-256 GitHub lists for it, then run silently (/RELAUNCH opens the new DroidPlay).
#   A download made by the app carries no mark of the web, so Windows doesn't warn about an unsigned file on every release, as it does for one from a browser.
#   DroidPlay for Linux, started from its AppImage (dm/android/linux), offers Update too: the release's DroidPlay-<version>-x86_64.AppImage, downloaded into
#   the folder of the AppImage it was started from ($APPIMAGE) and checked the same way, then renamed into place and started. Its first start prepares the
#   new version and keeps the one before it (dm/android/linux/unpack.py). A copy that can't (from source, a CI build, a folder it can't write to) gets the
#   release's page, and says why.
import errno, hashlib, json, os, re, shutil, subprocess, sys, urllib.request
from pathlib import Path

RELEASES_API = 'https://api.github.com/repos/enjoyerofslop/droidplay-core/releases/latest'
SETUP_NAME = re.compile(r'DroidPlay-Setup-\d+\.\d+\.\d+\.exe')
APPIMAGE_NAME = re.compile(r'DroidPlay-\d+\.\d+\.\d+-x86_64\.AppImage')   # the release's own name, whose version an update keeps true (replace_appimage)


class Cancelled(Exception): pass


def release_number(v):   # (0, 5, 1) for v0.5.1 or 0.5.1; None for anything else, such as a checkout's 0.5.0-3-gabc1234, which no release can be newer than
    m = re.fullmatch(r'v?(\d+)\.(\d+)\.(\d+)', (v or '').strip())
    return tuple(int(x) for x in m.groups()) if m else None


def feed(env=None):
    """Where the newest release is read: GitHub's releases/latest, or DM_UPDATE_FEED, a stand-in of the same form (a URL, or a file's absolute path), so an
    update can be tried without making a release (test_android_linux.py, a test machine)"""
    v = (os.environ if env is None else env).get('DM_UPDATE_FEED', '')
    if not v: return RELEASES_API
    return Path(v).as_uri() if os.path.isabs(v) else v


def checked_asset(a, name):   # a release asset whose name matches, with the digest GitHub lists for it, which the download is checked against; else None
    digest = re.fullmatch(r'sha256:([0-9a-f]{64})', a.get('digest') or '')
    if not (name.fullmatch(a.get('name') or '') and digest): return None
    return {'name': a['name'], 'url': a['browser_download_url'], 'size': a.get('size') or 0, 'sha256': digest.group(1)}


def latest_release(api=None):
    """The newest release: {tag, url (its page), setup, appimage}, each file {name, url, size, sha256} or None: only with the digest GitHub lists for it.
    Raises when offline or rate-limited"""
    req = urllib.request.Request(api or feed(), headers={'User-Agent': 'DroidPlay', 'Accept': 'application/vnd.github+json'})
    with urllib.request.urlopen(req, timeout=15) as r: d = json.load(r)
    out = {'tag': d['tag_name'], 'url': d['html_url'], 'setup': None, 'appimage': None}
    for a in d.get('assets') or []:
        for key, name in (('setup', SETUP_NAME), ('appimage', APPIMAGE_NAME)): out[key] = checked_asset(a, name) or out[key]
    return out


def setup_install(repo):   # whether DroidPlay-Setup installed this copy: <app>\core, with the runtime and Inno's uninstaller beside it
    app = Path(repo).parent
    return (app / 'unins000.exe').is_file() and (app / 'runtime' / 'pythonw.exe').is_file()


def appimage_install(repo, env=None):
    """Whether DroidPlay for Linux can replace its AppImage: (the AppImage file, None); (None, why not) for an AppImage that can't; (None, None) for a copy
    that isn't one. A copy is the AppImage's when it runs from a version unpack.py prepared (<data>/DroidPlay/<version>/core, .complete beside it) and its
    APPIMAGE names a file: the runtime sets it, but every program DroidPlay starts inherits it, so a checkout started from one of those has it too"""
    env = os.environ if env is None else env
    p, repo = env.get('APPIMAGE', ''), Path(repo)
    if not sys.platform.startswith('linux') or not os.path.isabs(p) or repo.name != 'core' or not (repo.parent / '.complete').is_file(): return None, None
    img = Path(p)
    if not img.is_file(): return None, f'the AppImage it was started from, {img}, is gone'
    try: v = (repo / 'VERSION').read_text(encoding='utf-8').strip()
    except OSError: v = ''
    if not release_number(v): return None, f"this AppImage is a test build ({v or 'no version'}), which a release doesn't replace"
    if not os.access(img.parent, os.W_OK | os.X_OK): return None, f"DroidPlay can't write to the AppImage's folder, {img.parent}"
    return img, None


def how(rel, repo, env=None):
    """How this copy takes the release rel: ('setup', None) where Setup installed it, ('appimage', None) from DroidPlay for Linux's AppImage, or (None, why
    not) where the release's page is the way (why is None when there's nothing to explain: from source, an older Windows install)"""
    if rel.get('setup') and setup_install(repo): return 'setup', None
    img, why = appimage_install(repo, env)
    if not img: return None, why
    if not rel.get('appimage'): return None, 'the release has no AppImage with a checksum'
    return 'appimage', None


def fetch(asset, part, progress=None, cancelled=lambda: False):
    """asset downloaded into part, checked against its digest and flushed to the disk; part deleted when that fails. Raises Cancelled, ValueError (checksum).
    progress(done, total) is called as it comes"""
    h, done = hashlib.sha256(), 0
    req = urllib.request.Request(asset['url'], headers={'User-Agent': 'DroidPlay'})
    try:
        with urllib.request.urlopen(req, timeout=30) as r, open(part, 'wb') as f:
            total = asset.get('size') or int(r.headers.get('Content-Length') or 0)
            while chunk := r.read(1 << 16):
                if cancelled(): raise Cancelled()
                f.write(chunk); h.update(chunk); done += len(chunk)
                if progress: progress(done, total)
            f.flush(); os.fsync(f.fileno())   # on the disk before a rename makes it the AppImage, so a power cut never leaves half a file in its place
    except BaseException:
        Path(part).unlink(missing_ok=True); raise
    if h.hexdigest() != asset['sha256']:
        Path(part).unlink(missing_ok=True)
        raise ValueError(f"{asset['name']} doesn't match the release's checksum, so it was deleted and nothing was installed")


def download_setup(setup, folder, progress=None, cancelled=lambda: False):
    """The release's Setup into folder, checked: its path. Raises Cancelled, or ValueError when it doesn't match the release's digest (deleted then).
    progress(done, total) is called as it comes; older downloads in folder go"""
    folder = Path(folder); folder.mkdir(parents=True, exist_ok=True)
    out = folder / setup['name']; part = folder / (setup['name'] + '.part')
    for f in folder.glob('DroidPlay-Setup-*'):
        if f.name not in (out.name, part.name): f.unlink(missing_ok=True)
    fetch(setup, part, progress, cancelled)
    os.replace(part, out)
    return out


def tree_size(root):
    total = 0
    for d, _, names in os.walk(root):
        for n in names:
            p = os.path.join(d, n)
            if not os.path.islink(p): total += os.path.getsize(p)
    return total


def room_needed(asset, img, repo):
    """[(folder, bytes needed, bytes free)] for each disk an AppImage update writes to: the new AppImage beside the old one, and its first start's copy of
    the new version beside this one (unpack.py wants its size and 200 MB), taken as this version's size and a quarter, for a version that grew. Checked
    before anything is written: unpack.py would stop at the new version's first start, with the old AppImage already replaced"""
    versions = Path(repo).parent.parent
    need = [(img.parent, (asset.get('size') or 0) + (64 << 20)), (versions, int(tree_size(Path(repo).parent) * 1.25) + (200 << 20))]
    if os.stat(img.parent).st_dev == os.stat(versions).st_dev: need = [(img.parent, need[0][1] + need[1][1])]   # one disk: both from the same free space
    return [(d, n, shutil.disk_usage(d).free) for d, n in need]


def download_appimage(asset, img, repo, progress=None, cancelled=lambda: False):
    """The release's AppImage downloaded beside img, the AppImage DroidPlay was started from (repo, its unpacked core), checked and made executable: the
    hidden file it is in, for replace_appimage. img is left as it is. Raises Cancelled, ValueError (checksum), OSError (no room, a folder it can't write)"""
    img = Path(img)
    for d, n, free in room_needed(asset, img, repo):
        if free < n: raise OSError(errno.ENOSPC, f'DroidPlay needs {n / 1e9:.1f} GB free in {d} for the new version (its AppImage and its first start), and '
                                                 f'{free / 1e9:.1f} GB are free there')
    part = img.parent / f'.{asset["name"]}.part'   # hidden, in the same folder: on the same disk, so the rename into place is atomic
    for f in img.parent.glob('.DroidPlay-*-x86_64.AppImage.part'): f.unlink(missing_ok=True)   # an earlier download cut short
    try: fetch(asset, part, progress, cancelled)
    except OSError as e:
        if e.errno == errno.ENOSPC: raise OSError(e.errno, f'the disk holding {img.parent} filled up while the new AppImage was downloaded') from e
        if e.errno in (errno.EACCES, errno.EPERM, errno.EROFS): raise OSError(e.errno, f"DroidPlay can't write to the AppImage's folder, {img.parent}") from e
        raise
    os.chmod(part, (os.stat(img).st_mode & 0o777) | 0o100)   # executable, with the old file's other permissions
    return part


def sync_dir(d):   # the folder's entries on the disk too, so the rename survives a power cut
    try: fd = os.open(d, os.O_RDONLY)
    except OSError: return
    try: os.fsync(fd)
    except OSError: pass
    finally: os.close(fd)


def replace_appimage(part, img, moved=lambda old, new: None):
    """The checked download part put in the place of img. While img has a release's name (DroidPlay-0.9.0-x86_64.AppImage) the new file takes the release's
    name, so the name still says which version it is, and img goes: moved(img, new) is called first (Settings' menu entry follows, menu_entry.retarget).
    Any other name (the player's own, an AppImage manager's) is kept and replaced in place, so whatever starts it still does. The running DroidPlay is
    unaffected either way: its AppImage stays mounted from the old file until it closes. → the new AppImage's path"""
    part, img = Path(part), Path(img)
    new = img.parent / part.name[1:].removesuffix('.part') if APPIMAGE_NAME.fullmatch(img.name) else img
    os.replace(part, new); sync_dir(new.parent)
    if new != img:
        try: moved(img, new)
        except (OSError, ValueError): pass   # the menu entry then starts a file that's gone: Settings says so and offers Start this copy
        try: img.unlink()
        except OSError: pass
        sync_dir(new.parent)
    return new


def relaunch(path, env=None):
    """Starts the AppImage at path on its own: in a session of its own, so it outlives this DroidPlay, and with none of the files this one holds open, among
    them the lock on this version's folder (unpack.py), which it would keep for as long as it ran, so the folder would never be removed. The old mount's
    APPDIR and APPIMAGE are left out: the runtime sets them for the new file. → its Popen, or None when it couldn't be started"""
    env = {k: v for k, v in (os.environ if env is None else env).items() if k not in ('APPDIR', 'APPIMAGE', 'ARGV0', 'OWD')}
    try: return subprocess.Popen([str(path)], env=env, stdin=subprocess.DEVNULL, close_fds=True, start_new_session=True)
    except OSError: return None
