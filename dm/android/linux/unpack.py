# DroidPlay for Linux's AppImage runs this first (AppRun, on the AppImage's own Python). On the first start of a version it copies the AppImage's payload, its
# Python and DroidPlay Core, into $XDG_DATA_HOME/DroidPlay/<version>/ (~/.local/share/DroidPlay/<version>/), showing a small window meanwhile, then starts
# DroidPlay from that copy, as Windows runs it from the folder Setup installed. Later starts of the version find the copy and start at once.
# Why not run from the AppImage's mount:
#   - it is read-only, and no build path was made for that: dotnet's restore wrote nx_extract's obj/ into its project and stopped with "Permission denied";
#   - its path changes at every start (/tmp/.mount_DroidPXXXXXX), and the packing script throws away a host build folder made from another source path
#     (build_android_from_nsp.py build_host), so every build after a restart would build the host again from nothing;
#   - every Python child of a build (pools of workers) would read thousands of small files through squashfuse.
# A version stays until two newer ones have been started: the one started before the current one is kept, to go back to. Each running DroidPlay holds a
# shared lock on its version's .complete (the lock outlives the exec below, and the processes DroidPlay starts inherit it), and a version is removed only
# when its lock can be taken alone, so a version in use is never removed under it.
#   python3 -I unpack.py <payload folder> [DroidPlay's arguments...]
import fcntl
import os
import re
import shutil
import sys
import time
from pathlib import Path

VERSION = re.compile(r'[0-9A-Za-z][0-9A-Za-z._+-]*')   # a release's 0.9.0, or a CI build's git describe, 0.8.10-12-g1a2b3c4d: a folder name either way
APP_ID = 'io.github.enjoyerofslop.droidplay'


class UnpackError(Exception): pass


def versions_dir(env=None):   # where the versions go; a relative XDG_DATA_HOME is ignored, as the XDG spec says
    env = os.environ if env is None else env
    xdg = env.get('XDG_DATA_HOME', '')
    return (Path(xdg) if os.path.isabs(xdg) else Path.home() / '.local' / 'share') / 'DroidPlay'


def version_of(payload):
    v = (Path(payload) / 'core' / 'VERSION').read_text(encoding='utf-8').strip()
    if not VERSION.fullmatch(v): raise UnpackError(f'the AppImage names its version {v!r}, which is no folder name')
    return v


def lock(path, exclusive, wait=True):   # an open file holding flock's lock on path, or None when wait is False and someone else holds it
    fd = os.open(path, os.O_RDWR | os.O_CREAT, 0o644)
    try: fcntl.flock(fd, (fcntl.LOCK_EX if exclusive else fcntl.LOCK_SH) | (0 if wait else fcntl.LOCK_NB))
    except BlockingIOError: os.close(fd); return None
    return fd


def tree_size(root):
    total, files = 0, 0
    for d, _, names in os.walk(root):
        for n in names:
            p = os.path.join(d, n)
            if not os.path.islink(p): total += os.path.getsize(p); files += 1
    return total, files


def unpack(payload, dest, report=lambda done, total: None):
    """payload's tree into dest, through dest.partial, so a copy cut short (the computer off, the disk full) is never taken for a version. Raises UnpackError"""
    payload, dest = Path(payload), Path(dest)
    total, _ = tree_size(payload)
    free = shutil.disk_usage(dest.parent).free
    if free < total + (200 << 20):
        raise UnpackError(f'DroidPlay needs {total / 1e9:.1f} GB free in {dest.parent} to prepare this version, and {free / 1e9:.1f} GB are free there')
    part = dest.with_name(dest.name + '.partial')
    shutil.rmtree(part, ignore_errors=True)
    done = 0
    def copy(src, dst):
        nonlocal done
        shutil.copy2(src, dst)
        st = os.stat(dst)
        if not st.st_mode & 0o200: os.chmod(dst, st.st_mode | 0o200)   # files the AppImage holds read-only would make the version hard to remove later
        done += st.st_size; report(done, total)
    try:
        shutil.copytree(payload, part, symlinks=True, copy_function=copy)
        (part / '.complete').write_text(dest.name + '\n', encoding='utf-8')
        if dest.exists(): shutil.rmtree(dest)   # a folder of that name without .complete: nothing can be running from it
        os.rename(part, dest)
    except OSError as e:
        shutil.rmtree(part, ignore_errors=True)
        raise UnpackError(f'DroidPlay could not be prepared in {dest}: {e}') from e
    return dest


def prune(root, current):
    """Removes the versions before the one started before current, and copies a start left unfinished; not one that a running DroidPlay holds.
    Called with root's unpack lock held, so a .partial folder is no other start's work in progress. → the names removed"""
    root, gone, done = Path(root), [], []
    for d in root.iterdir():
        if d.name.endswith('.partial') and d.is_dir() and not d.is_symlink(): shutil.rmtree(d, ignore_errors=True); gone.append(d.name)
        elif d.name != current and VERSION.fullmatch(d.name) and d.is_dir() and not d.is_symlink() and (d / '.complete').is_file():
            done.append((os.stat(d / '.complete').st_mtime, d))
    for _, d in sorted(done, reverse=True)[1:]:   # the newest of the others stays
        fd = lock(d / '.complete', exclusive=True, wait=False)
        if fd is None: continue   # running
        try: shutil.rmtree(d); gone.append(d.name)
        except OSError: pass
        finally: os.close(fd)
    return gone


def prepare(payload, root, report=lambda done, total: None):
    """The version's folder under root, unpacked if it isn't yet, and a shared lock on it held by the returned file descriptor; older versions pruned"""
    payload, root = Path(payload), Path(root)
    version = version_of(payload)
    root.mkdir(parents=True, exist_ok=True)
    gate = lock(root / '.unpack.lock', exclusive=True)   # one start at a time decides: two first starts of a version would copy it twice
    try:
        dest = root / version
        if not (dest / '.complete').is_file(): unpack(payload, dest, report)
        held = lock(dest / '.complete', exclusive=False)
        prune(root, version)
    finally: os.close(gate)
    return dest, held


def can_show_window():   # a display to put the progress window on; a terminal or CI's offscreen runs print instead
    return bool(os.environ.get('WAYLAND_DISPLAY') or os.environ.get('DISPLAY')) and os.environ.get('QT_QPA_PLATFORM', '') not in ('offscreen', 'minimal')


def say(msg): print(f'DroidPlay: {msg}', file=sys.stderr, flush=True)


def prepare_with_progress(payload, root):
    version = version_of(payload)
    if (root / version / '.complete').is_file(): return prepare(payload, root)   # the usual start: nothing to show
    say(f'preparing DroidPlay {version} in {root / version} (once for each version)')
    t0, last = time.monotonic(), [0.0, -1]
    if not can_show_window():
        def report(done, total):
            pct = done * 100 // max(total, 1)
            if pct >= last[1] + 10: last[1] = pct; say(f'  {pct}%')
        try: return prepare(payload, root, report)
        finally: say(f'  took {time.monotonic() - t0:.0f} s')
    from PySide6.QtCore import Qt   # pyright: ignore[reportMissingImports]  (the AppImage's own PySide6)
    from PySide6.QtGui import QGuiApplication, QIcon   # pyright: ignore[reportMissingImports]
    from PySide6.QtWidgets import QApplication, QLabel, QMessageBox, QProgressBar, QVBoxLayout, QWidget   # pyright: ignore[reportMissingImports]
    app = QApplication(sys.argv[:1]); app.setApplicationName('DroidPlay'); app.setDesktopFileName(APP_ID)
    icon = Path(payload).parent / f'{APP_ID}.png'
    if icon.is_file(): app.setWindowIcon(QIcon(str(icon)))
    w = QWidget(); w.setWindowTitle('DroidPlay'); w.setWindowFlag(Qt.WindowType.WindowCloseButtonHint, False); w.setMinimumWidth(420)
    lay = QVBoxLayout(w); lay.setContentsMargins(24, 20, 24, 20)
    lay.addWidget(QLabel(f'Preparing DroidPlay {version}…\nThis happens once for each new version.'))
    bar = QProgressBar(); bar.setRange(0, 1000); lay.addWidget(bar)
    w.show()
    def report(done, total):
        now = time.monotonic()
        if now - last[0] > 0.03: last[0] = now; bar.setValue(done * 1000 // max(total, 1)); QGuiApplication.processEvents()
    QGuiApplication.processEvents()
    try: return prepare(payload, root, report)
    except UnpackError as e:
        w.hide(); QMessageBox.critical(None, 'DroidPlay', str(e)); raise
    finally: w.close(); say(f'  took {time.monotonic() - t0:.0f} s')


def main():
    payload = Path(sys.argv[1]).resolve()
    try: dest, held = prepare_with_progress(payload, versions_dir())
    except (UnpackError, OSError) as e: say(str(e)); sys.exit(1)
    os.set_inheritable(held, True)   # the lock stays with DroidPlay through the exec, and with what it starts
    py, app = dest / 'runtime' / 'bin' / 'python3', dest / 'core' / 'scripts' / 'nxnative_android.py'
    os.execv(py, [str(py), str(app), *sys.argv[2:]])


if __name__ == '__main__':
    main()
