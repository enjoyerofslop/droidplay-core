# The desktop app of the Android port: pack your games for the nxnative app, install them on your phones and watch them run.
#   uv run scripts/nxnative_android.py        (uv brings Python and the modules the block below names; or a Python that has them: pip install PySide6 lz4 capstone fonttools zbic)
# It drives scripts/build_android_from_nsp.py (its --progress json events, --info, --check, --push-only, --apk-only, --migrate) and adb:
#   Get started  a new player's four steps (keys, tools, phone, first game), each with its state: where a first launch and the empty Library land; the top bar's
#                "Setup 2 of 4" opens it while a step is left (dm/android/desktop/first_run.py works the steps out)
#   Library      your games (the NSP folders you added): the picked one large, Play (on the phone picked at the top), its options and Manage; the collection below
#   Builds       the running build: its stages, progress and messages (the bar at the bottom of every page follows it); a build started from a console
#                shows here too, from the journal the script writes (<data>/builds/*.jsonl)
#   Phones       adb devices over USB and Wi-Fi (pair, connect, discover), the app and the games on each, saves backed up to the computer
#   Performance  the game's log (adb logcat) with the frame time, GPU and CPU figures it prints, a frame's GPU passes
#   Settings     your keys, and what the build needs on this computer with how to get what is missing; on Windows and Linux, Install what's missing (--setup);
#                on Linux, from the AppImage, Add to the applications menu (dm/android/desktop/menu_entry.py)
# /// script
# # Below 3.15: lz4 has no wheels for it yet, and a player's computer has no compiler to build one. The packing script runs in this same Python.
# # PySide6-Essentials has every Qt module the app uses (Core, Gui, Widgets, Svg) without PySide6's add-ons and web engine, a few hundred MB less to download.
# requires-python = ">=3.12,<3.15"
# # zbic decodes the ZBIC (Nintendo zstd) segments of code modules built for system 22.0.0 on, which Switch 2 games such as Ocarina of Time ship (dm/tools/nso.py); it has Windows wheels.
# dependencies = ["PySide6-Essentials", "lz4", "capstone>=5,<6", "fonttools", "zbic", "backports.zstd; python_version < '3.14'"]
# ///
import json, os, re, shutil, subprocess, sys, time, uuid
from pathlib import Path

try:
    from PySide6.QtCore import QByteArray, QObject, QProcess, QProcessEnvironment, QRectF, QRunnable, QSettings, QSize, QStandardPaths, Qt, QThreadPool, QTimer, QUrl, Signal  # pyright: ignore[reportMissingImports]   (PySide6 is the desktop app's alone; the lint job does not install it)
    from PySide6.QtGui import QColor, QDesktopServices, QFont, QIcon, QLinearGradient, QPainter, QPainterPath, QPalette, QPen, QPixmap  # pyright: ignore[reportMissingImports]
    from PySide6.QtSvg import QSvgRenderer  # pyright: ignore[reportMissingImports]
    from PySide6.QtWidgets import (  # pyright: ignore[reportMissingImports]
                                   QApplication, QButtonGroup, QCheckBox, QComboBox, QDialog, QDialogButtonBox, QFileDialog, QFormLayout, QFrame, QGridLayout, QHBoxLayout,
                                   QHeaderView, QInputDialog, QLabel, QLineEdit, QListWidget, QListWidgetItem, QMainWindow, QMenu, QMessageBox, QPlainTextEdit,
                                   QProgressBar, QPushButton, QRadioButton, QScrollArea, QSplitter, QStackedWidget, QStyledItemDelegate, QTableWidget, QTableWidgetItem, QToolButton,
                                   QTreeWidget, QTreeWidgetItem, QVBoxLayout, QWidget)
except ImportError as e:   # on Linux also a system library Qt needs and the system lacks (libEGL.so.1...): the error names it
    sys.exit(f'DroidPlay (nxnative_android.py) needs PySide6 ({e}): uv run {__file__}, or {sys.executable} -m pip install PySide6')

REPO = Path(__file__).resolve().parents[1]
SCRIPT = REPO / 'scripts' / 'build_android_from_nsp.py'
PKG = 'org.nxnative.android'
REMOTE = f'/sdcard/Android/data/{PKG}/files'
sys.path.insert(0, str(REPO / 'scripts'))
import build_android_from_nsp as ba   # noqa: E402  (data_dir, library: the host library)
import phone_id   # noqa: E402  # pyright: ignore[reportMissingImports]  (dm/android/tools, which the script put on the path: a phone's ID, which outlives a Wi-Fi port)
sys.path.insert(0, str(REPO / 'dm' / 'android' / 'desktop'))
import first_run as fr   # noqa: E402  # pyright: ignore[reportMissingImports]  (dm/android/desktop, put on the path here: the Get started checklist's steps, Qt-free so its tests run without PySide6)
import game_data as gd   # noqa: E402  # pyright: ignore[reportMissingImports]  (dm/android/desktop: Phone storage or SD card, per game)
import wifi   # noqa: E402  # pyright: ignore[reportMissingImports]  (what adb pair, connect and mdns say over Wi-Fi, in plain words)
import explain as ex   # noqa: E402  # pyright: ignore[reportMissingImports]  (plain words for what went wrong, and the report for #help)
import phone_guide as pg   # noqa: E402  # pyright: ignore[reportMissingImports]  (how to connect a phone, by brand; its battery and free space)
import update as upd   # noqa: E402  # pyright: ignore[reportMissingImports]  (the newest release, and its Setup or AppImage downloaded and checked)
import procs   # noqa: E402  # pyright: ignore[reportMissingImports]  (Stop: a build and every tool it started)
import menu_entry   # noqa: E402  # pyright: ignore[reportMissingImports]  (Linux: the AppImage's entry in the applications menu)

LANGUAGES = [('en-US', 'American English'), ('en-GB', 'British English'), ('ja', 'Japanese'), ('fr', 'French'), ('de', 'German'), ('es-419', 'Latin American Spanish'),
             ('es', 'Spanish'), ('it', 'Italian'), ('nl', 'Dutch'), ('fr-CA', 'Canadian French'), ('pt', 'Portuguese'), ('ru', 'Russian'), ('ko', 'Korean'),
             ('zh-Hant', 'Traditional Chinese'), ('zh-Hans', 'Simplified Chinese'), ('pt-BR', 'Brazilian Portuguese')]
NOWINDOW = 0x08000000 if os.name == 'nt' else 0   # CREATE_NO_WINDOW: no console flashing up for each adb call
CAN_INSTALL = not ba.host_setup.unsupported()   # Install what's missing (--setup): Windows and x86_64 Linux; a Mac's DroidPlay Setup brings the tools itself
# Off Windows, a build starts in a session of its own (setsid), so it leads a process group with the tools it starts and Stop ends them all (procs.py). Qt 6.7
# and later; an older PySide6 from pip leaves it out, and Stop then ends the script alone, as before
NEW_SESSION = getattr(getattr(QProcess, 'UnixProcessFlag', None), 'CreateNewSession', None) if os.name != 'nt' else None
# The Python that builds and the script's --check and --info run on. On Windows the Start menu shortcut opens this app on pythonw.exe, which has no console, so
# each console tool a build runs on it (java, dotnet, cmake, ninja, adb…) would open a window of its own, dozens in a build. The python.exe beside it, started from
# this windowless app (QProcess, NOWINDOW), gets one hidden console that every tool shares, and their output still reaches the Builds page.
def console_python():
    exe = Path(sys.executable); con = exe.with_name('python.exe')
    return str(con) if os.name == 'nt' and exe.name.lower() == 'pythonw.exe' and con.is_file() else sys.executable
PYTHON = console_python()

# ---- the palette: a dark room lit by the game's own colours; colour only where it means something ----
INK = '#0b0b0c'          # the room
PANEL = '#141416'        # a raised surface (top bar, build bar, cards' frames)
LIFT = '#1d1d20'         # hover, the picked tab
LINE = 'rgba(255,255,255,0.08)'
TEXT, TEXT2, TEXT3 = '#f4f4f5', '#a1a1aa', '#71717a'
CREAM = '#fdf3dc'        # the one primary action: Play
READY = '#3ddc84'        # Android green: on the phone, ready to play
BUILD = '#f5b82e'        # a build running
ERR = '#f06a6a'
FONT = 'Segoe UI Variable Display' if os.name == 'nt' else 'Inter'

ICONS = {   # 24x24 strokes (lucide), drawn in the label's colour
    'library': '<line x1="6" x2="10" y1="11" y2="11"/><line x1="8" x2="8" y1="9" y2="13"/><line x1="15" x2="15.01" y1="12" y2="12"/><line x1="18" x2="18.01" y1="10" y2="10"/><path d="M17.32 5H6.68a4 4 0 0 0-3.978 3.59c-.006.052-.01.101-.017.152C2.604 9.416 2 14.456 2 16a3 3 0 0 0 3 3c1 0 1.5-.5 2-1l1.414-1.414A2 2 0 0 1 9.828 16h4.344a2 2 0 0 1 1.414.586L17 18c.5.5 1 1 2 1a3 3 0 0 0 3-3c0-1.545-.604-6.584-.685-7.258-.007-.05-.011-.1-.017-.151A4 4 0 0 0 17.32 5z"/>',
    'builds': '<path d="M11 21.73a2 2 0 0 0 2 0l7-4A2 2 0 0 0 21 16V8a2 2 0 0 0-1-1.73l-7-4a2 2 0 0 0-2 0l-7 4A2 2 0 0 0 3 8v8a2 2 0 0 0 1 1.73z"/><path d="M12 22V12"/><path d="m3.3 7 7.703 4.734a2 2 0 0 0 1.994 0L20.7 7"/><path d="m7.5 4.27 9 5.15"/>',
    'phone': '<rect width="14" height="20" x="5" y="2" rx="2" ry="2"/><path d="M12 18h.01"/>',
    'perf': '<path d="M22 12h-2.48a2 2 0 0 0-1.93 1.46l-2.35 8.36a.25.25 0 0 1-.48 0L9.24 2.18a.25.25 0 0 0-.48 0l-2.35 8.36A2 2 0 0 1 4.49 12H2"/>',
    'settings': '<line x1="21" x2="14" y1="4" y2="4"/><line x1="10" x2="3" y1="4" y2="4"/><line x1="21" x2="12" y1="12" y2="12"/><line x1="8" x2="3" y1="12" y2="12"/><line x1="21" x2="16" y1="20" y2="20"/><line x1="12" x2="3" y1="20" y2="20"/><line x1="14" x2="14" y1="2" y2="6"/><line x1="8" x2="8" y1="10" y2="14"/><line x1="16" x2="16" y1="18" y2="22"/>',
    'search': '<circle cx="11" cy="11" r="8"/><path d="m21 21-4.3-4.3"/>',
    'plus': '<path d="M5 12h14"/><path d="M12 5v14"/>',
    'more': '<circle cx="12" cy="12" r="10"/><path d="M17 12h.01"/><path d="M12 12h.01"/><path d="M7 12h.01"/>',
    'down': '<path d="m6 9 6 6 6-6"/>', 'right': '<path d="m9 18 6-6-6-6"/>', 'left': '<path d="m15 18-6-6 6-6"/>',
    'tools': '<path d="m15 12-8.373 8.373a1 1 0 1 1-3-3L12 9"/><path d="m18 15 4-4"/><path d="m21.5 11.5-1.914-1.914A2 2 0 0 1 19 8.172V7l-2.26-1.13a3.004 3.004 0 0 0-3.5.53l-.32.32"/><path d="M3 3l4 4"/>',
}
PLAY_SVG = '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"><path fill="{c}" d="M7 4.5v15a1 1 0 0 0 1.52.85l12-7.5a1 1 0 0 0 0-1.7l-12-7.5A1 1 0 0 0 7 4.5z"/></svg>'


def icon(name, color=TEXT2, size=20):
    svg = PLAY_SVG.format(c=color) if name == 'play' else (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="{color}" stroke-width="2" '
                                                         f'stroke-linecap="round" stroke-linejoin="round">{ICONS[name]}</svg>')
    pm = QPixmap(size * 2, size * 2); pm.fill(Qt.transparent)
    p = QPainter(pm); QSvgRenderer(QByteArray(svg.encode())).render(p); p.end(); pm.setDevicePixelRatio(2)
    return QIcon(pm)


# The window is DroidPlay for Windows and Linux: DroidPlay on a Mac is a separate app (github.com/enjoyerofslop/droidplay), whose name and icon it shares.
# DroidPlay's icon, from the Mac app's layers (droidplay App/AppIcon.icon: the plate, the Joy-Cons and the play glyph) on its light green, flattened for the window
# and the taskbar: the Mac draws the glass and shadows itself
APP_SVG = ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1024 1024"><defs><linearGradient id="g" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#9ee8ad"/>'
           '<stop offset="1" stop-color="#47ba75"/></linearGradient></defs><rect width="1024" height="1024" rx="228" fill="url(#g)"/>'
           '<g transform="translate(512 512) scale(1.5161) translate(-512 -512)"><rect x="413" y="334" width="198" height="356" rx="26" fill="#3a3f4b" stroke="#4a505e" stroke-width="4"/>'
           '<path d="M415,322 L415,702 L335,702 A80,80 0 0 1 255,622 L255,402 A80,80 0 0 1 335,322 Z" fill="#00b8d9"/>'
           '<g fill="#0a5f70"><circle cx="335" cy="432" r="36"/><circle cx="335" cy="556" r="15"/><circle cx="335" cy="624" r="15"/><circle cx="301" cy="590" r="15"/>'
           '<circle cx="369" cy="590" r="15"/></g><path d="M609,322 L689,322 A80,80 0 0 1 769,402 L769,622 A80,80 0 0 1 689,702 L609,702 Z" fill="#ff4f5e"/>'
           '<g fill="#8a1d27"><circle cx="689" cy="398" r="15"/><circle cx="689" cy="466" r="15"/><circle cx="655" cy="432" r="15"/><circle cx="723" cy="432" r="15"/>'
           '<circle cx="689" cy="590" r="36"/></g><path d="M490,476 L490,548 L550,512 Z" fill="#fff" stroke="#fff" stroke-width="26" stroke-linejoin="round"/></g></svg>')


def app_icon():   # every size Windows and the Linux desktops ask for, drawn from APP_SVG
    out, r = QIcon(), QSvgRenderer(QByteArray(APP_SVG.encode()))
    for s in (16, 24, 32, 48, 64, 128, 256):
        pm = QPixmap(s, s); pm.fill(Qt.transparent); p = QPainter(pm); p.setRenderHint(QPainter.Antialiasing); r.render(p); p.end(); out.addPixmap(pm)
    return out


STYLE = f"""
* {{ font-family: '{FONT}', 'Segoe UI', sans-serif; color: {TEXT}; }}
QMainWindow, #page {{ background: {INK}; }}
QWidget#topbar, QWidget#buildbar {{ background: {PANEL}; }}
QLabel#wordmark {{ font-size: 22px; font-weight: 700; letter-spacing: -0.5px; padding-right: 18px; }}
QToolButton#nav {{ background: transparent; border: none; border-radius: 10px; padding: 8px 14px; font-size: 14px; font-weight: 600; color: {TEXT2}; }}
QToolButton#nav:hover {{ background: {LIFT}; color: {TEXT}; }}
QToolButton#nav:checked {{ background: {LIFT}; color: {TEXT}; }}
QPushButton {{ background: {LIFT}; border: 1px solid {LINE}; border-radius: 9px; padding: 8px 14px; font-size: 13px; font-weight: 600; }}
QPushButton:hover {{ background: #26262a; }}
QPushButton:pressed {{ background: #2e2e33; }}
QPushButton:disabled {{ color: {TEXT3}; background: {PANEL}; }}
QPushButton#ghost {{ background: transparent; border: 1px solid {LINE}; color: {TEXT2}; }}
QPushButton#ghost:hover {{ color: {TEXT}; background: {LIFT}; }}
QPushButton#play {{ background: {CREAM}; color: #121212; border: none; border-radius: 26px; padding: 0 38px; min-height: 52px; font-size: 17px; font-weight: 700; }}
QPushButton#play:hover {{ background: #fff8e8; }}
QPushButton#play:pressed {{ background: #efe3c6; }}
QPushButton#play:disabled {{ background: #3a3a3f; color: {TEXT3}; }}
QPushButton#round {{ background: rgba(20,20,22,0.55); border: 1px solid rgba(255,255,255,0.18); border-radius: 26px; min-width: 52px; max-width: 52px; min-height: 52px; max-height: 52px; padding: 0; }}
QPushButton#round:hover {{ background: rgba(40,40,44,0.75); }}
QToolButton#manage {{ background: transparent; border: none; font-size: 14px; font-weight: 600; padding: 8px 6px; }}
QToolButton#manage:hover {{ color: white; }}
QToolButton#manage::menu-indicator {{ image: none; }}
QPushButton#next {{ background: {CREAM}; color: #121212; border: none; }}
QPushButton#next:hover {{ background: #fff8e8; }}
QPushButton#pill {{ background: rgba(245,184,46,0.12); border: 1px solid rgba(245,184,46,0.45); border-radius: 12px; color: {BUILD}; padding: 4px 14px; font-size: 13px; font-weight: 700; }}
QPushButton#pill:hover {{ background: rgba(245,184,46,0.22); }}
QPushButton#stop {{ background: transparent; border: 1px solid {BUILD}; color: {BUILD}; }}
QPushButton#stop:hover {{ background: rgba(245,184,46,0.12); }}
QPushButton#arrow {{ background: rgba(20,20,22,0.85); border: 1px solid rgba(255,255,255,0.16); border-radius: 22px; min-width: 44px; max-width: 44px; min-height: 44px; max-height: 44px; padding: 0; }}
QLineEdit, QComboBox {{ background: #09090a; border: 1px solid {LINE}; border-radius: 8px; padding: 7px 10px; font-size: 13px; selection-background-color: #3a3a40; }}
QLineEdit:focus, QComboBox:focus {{ border: 1px solid rgba(255,255,255,0.28); }}
QComboBox QAbstractItemView {{ background: {LIFT}; border: 1px solid {LINE}; selection-background-color: #2e2e33; }}
QRadioButton {{ font-size: 14px; font-weight: 600; spacing: 10px; }}
QRadioButton::indicator {{ width: 14px; height: 14px; border-radius: 9px; border: 2px solid {TEXT3}; background: transparent; }}
QRadioButton::indicator:checked {{ border: 2px solid {CREAM}; background: {CREAM}; }}
QMenu {{ background: {LIFT}; border: 1px solid {LINE}; border-radius: 10px; padding: 6px; }}
QMenu::item {{ padding: 8px 18px; border-radius: 6px; font-size: 13px; }}
QMenu::item:selected {{ background: #2e2e33; }}
QMenu::item:disabled {{ color: {TEXT3}; }}
QMenu::separator {{ height: 1px; background: {LINE}; margin: 5px 8px; }}
QProgressBar {{ background: #26262a; border: none; border-radius: 4px; max-height: 8px; min-height: 8px; text-align: right; }}
QProgressBar::chunk {{ background: {BUILD}; border-radius: 4px; }}
QTableWidget, QTreeWidget, QListWidget, QPlainTextEdit {{ background: {PANEL}; border: 1px solid {LINE}; border-radius: 10px; gridline-color: transparent; font-size: 13px; }}
QTableWidget::item, QTreeWidget::item, QListWidget::item {{ padding: 6px; }}
QTableWidget::item:selected, QTreeWidget::item:selected, QListWidget::item:selected {{ background: #26262a; }}
QHeaderView::section {{ background: {PANEL}; color: {TEXT3}; border: none; border-bottom: 1px solid {LINE}; padding: 8px; font-size: 11px; font-weight: 600; text-transform: uppercase; }}
QPlainTextEdit#log {{ font-family: 'Cascadia Mono', 'Consolas', 'Menlo', 'DejaVu Sans Mono', monospace; font-size: 12px; }}
QScrollArea {{ background: transparent; border: none; }}
QScrollBar:vertical {{ background: transparent; width: 10px; }} QScrollBar::handle:vertical {{ background: #2e2e33; border-radius: 5px; min-height: 30px; }}
QScrollBar:horizontal {{ height: 0px; }} QScrollBar::add-line, QScrollBar::sub-line {{ height: 0; width: 0; }}
QLabel#h1 {{ font-size: 28px; font-weight: 700; letter-spacing: -0.6px; }}
QLabel#h2 {{ font-size: 26px; font-weight: 700; letter-spacing: -0.5px; }}
QLabel#muted {{ color: {TEXT3}; font-size: 13px; }}
QLabel#meta {{ color: {TEXT2}; font-size: 13px; }}
QLabel#tilelabel {{ color: {TEXT3}; font-size: 11px; font-weight: 600; letter-spacing: 0.6px; }}
QLabel#tilevalue {{ font-size: 26px; font-weight: 700; letter-spacing: -0.5px; }}
QFrame#tile {{ background: {PANEL}; border: 1px solid {LINE}; border-radius: 12px; }}
QFrame#card {{ background: {PANEL}; border: 1px solid {LINE}; border-radius: 12px; }}
"""


def adb_run(serial, *args, timeout=20):   # → (ok, output); never raises
    cmd = ['adb'] + (['-s', serial] if serial else []) + [str(a) for a in args]
    try: r = subprocess.run(cmd, capture_output=True, text=True, errors='replace', timeout=timeout, creationflags=NOWINDOW, env=dict(os.environ, MSYS_NO_PATHCONV='1'))
    except (OSError, subprocess.TimeoutExpired) as e: return False, str(e)
    return r.returncode == 0, (r.stdout + r.stderr).strip()


class Job(QRunnable):   # a blocking function on the thread pool; its result comes back on the UI thread
    class Sig(QObject): done = Signal(object)
    live = set()   # the jobs in flight: Python would free a job (and its signal object) as soon as bg() returned
    def __init__(self, fn, cb):
        super().__init__(); self.setAutoDelete(False); self.fn, self.cb, self.sig = fn, cb, Job.Sig(); self.sig.done.connect(self.finish)
    def run(self):
        try: r = self.fn()
        except Exception as e: r = e
        self.sig.done.emit(r)
    def finish(self, r):
        Job.live.discard(self); self.cb(r)


def bg(fn, cb): j = Job(fn, cb); Job.live.add(j); QThreadPool.globalInstance().start(j)


def alive(pid):
    if pid <= 0: return False
    if os.name != 'nt':
        try: os.kill(pid, 0); return True
        except OSError: return False
    import ctypes; k = ctypes.windll.kernel32; h = k.OpenProcess(0x1000, False, pid)   # PROCESS_QUERY_LIMITED_INFORMATION
    if not h: return False
    code = ctypes.c_ulong(); k.GetExitCodeProcess(h, ctypes.byref(code)); k.CloseHandle(h); return code.value == 259   # STILL_ACTIVE


def journal_title(argv):   # what a build from the console does, from its command line
    if '--apk-only' in argv: return 'Update the app'
    if '--migrate' in argv: return 'Move the old install'
    if '--setup' in argv: return fr.SETUP_TITLE
    folder = next((x for x in argv if not x.startswith('-') and Path(x).is_dir()), None)
    name = None
    if folder:
        try: name = next((m.get('name') for _, (_, m) in ba.library().items() if m.get('nsp_dir') == str(Path(folder).resolve())), None)
        except Exception: pass
    what = ('Build the game app of ' if '--game-apk' in argv or '--xapk' in argv else 'Update the shaders of ' if '--update-shaders' in argv
            else 'Install ' if '--push-only' in argv else 'Build ')
    return what + (name or (Path(folder).name if folder else 'a game'))


def label(text, name=None, wrap=False):
    l = QLabel(text)
    if name: l.setObjectName(name)
    l.setWordWrap(wrap); return l


def gb_text(n): return f'{n / 2**30:.1f} GB' if n and n >= 2**30 else f'{(n or 0) / 2**20:.0f} MB'


def esc(s): return s.replace('&', '&amp;').replace('<', '&lt;')


def reveal(path): QDesktopServices.openUrl(QUrl.fromLocalFile(str(path)))   # a folder in Explorer, Finder or the Linux desktop's file manager (os.startfile is Windows' alone)


def clock_text(sec): sec = int(sec or 0); return f'{sec // 60}:{sec % 60:02d}' if sec >= 60 else f'{sec} s'


def count_text(done, total, unit):   # "1,234 / 37,393 files", or bytes as "1.2 / 3.7 GB" (unit 'B'), with the percentage
    pct = f' ({100 * done // total}%)' if total else ''
    if unit == 'B':
        u, d = next((u, d) for u, d in (('GB', 1 << 30), ('MB', 1 << 20), ('KB', 1 << 10), ('B', 1)) if total >= d or d == 1)
        return f'{done / d:.1f} / {total / d:.1f} {u}{pct}'
    return f'{done:,} / {total:,} {unit}'.rstrip() + pct


def tint(bar, color): bar.setStyleSheet(f'QProgressBar::chunk {{ background: {color}; border-radius: 4px; }}')   # amber while it runs, green once done, red when it failed


def wash(pm, w, h):   # the art's colour wash: the picture shrunk to a few pixels and spread over w x h (a soft blur of its own colours)
    if pm.isNull(): return QPixmap()
    return pm.scaled(12, 12, Qt.IgnoreAspectRatio, Qt.SmoothTransformation).scaled(w, h, Qt.IgnoreAspectRatio, Qt.SmoothTransformation)


# ---------------- a game, as the window shows it ----------------
class Game:
    def __init__(self, folder, info):
        self.folder, self.info = folder, info
        ex = info.get('extracted') or {}; self.newest = info.get('newest')
        e = ex.get(self.newest) or (next(iter(ex.values())) if ex else {})
        self.id = info.get('title_id'); self.name = e.get('name') or info.get('name') or self.id or Path(folder).name; self.error = info.get('error')
        self.detail = info.get('detail') or self.error   # what the read said in full: the files it passed over, and why, before the error
        self.captures = e.get('captures') or 0
        self.icon = e.get('icon'); self.display = e.get('display'); self.languages = e.get('languages') or [c for c, _ in LANGUAGES]
        self.profile = info.get('profile'); self.provisional = info.get('provisional'); self.host = info.get('host'); self.tested = info.get('android_tested')
        self.run_time = info.get('run_time_shaders') or {}   # {count, ready}: shaders it compiles while it runs that its tables name, which only a Mac build captures (droidplay-core#441)
        self.pm = QPixmap(self.icon) if self.icon else QPixmap()

    def help(self): return ex.explain(self.detail or '', folder=self.folder, read=True)   # the folder's read failed: what to do about it (explain.py)
    def new_shaders(self, on_phone):   # run-time shaders the phone saved beyond the ones on this computer (a provisional profile's only: build_android_from_nsp.py pull_captures)
        return max(0, (on_phone.get(self.id) or {}).get('captures', 0) - self.captures) if self.provisional else 0
    def status(self, on_phone, phone):   # (text, colour, state): state picks what Play does
        if self.info.get('needs_keys'): return 'Needs your prod.keys to read it: Get started, step 1', ERR, 'keys'
        # not the game's fault: the tools that read it aren't installed yet. It showed as unreadable, with a disabled "Not supported", to every new player
        if self.info.get('needs_tools'): return "Install DroidPlay's tools to read it: Get started, step 2", BUILD, 'tools'
        if self.error:   # in plain words (explain.py): keys older than the game get Pick your prod.keys, not a disabled "Can't read it"
            h = self.help(); return h.title, ERR, {'keys': 'keys', 'tools': 'tools', 'folder': 'folder'}.get(h.action, 'none')
        if not self.profile: return 'Not supported yet: experimental build only', ERR, 'survey'
        if self.run_time and not self.run_time.get('ready') and not self.host: return ex.mac_only(self.run_time.get('count', 0), sys.platform == 'darwin'), ERR, 'mac'   # its build would stop at once
        x = 'Experimental (provisional profile, may not boot) · ' if self.provisional else ''   # an experimental build's profile, guessed from the survey
        n = self.new_shaders(on_phone)
        if n and self.host: return f'{x}{n} new shader(s) captured on the phone · Manage → Update shaders', BUILD, 'install' if self.id not in on_phone else 'play'
        if self.id in on_phone and self.host: return f'{x}Ready on {phone or "the phone"}', ERR if x else READY, 'play'
        if self.host: return f'{x}Built · not on this phone', ERR if x else BUILD, 'install'
        return f'{x}Not built yet', ERR if x else TEXT3, 'build'


class Hero(QWidget):   # the picked game, large: its art's wash behind, the art itself on the right, the status, the title and the actions
    def __init__(self):
        super().__init__(); self.setMinimumHeight(330); self.pm = QPixmap(); self.cache = None
        lay = QVBoxLayout(self); lay.setContentsMargins(48, 44, 48, 32); lay.setSpacing(0)
        self.status = label('', wrap=False); self.status.setStyleSheet('font-size: 13px; font-weight: 700; letter-spacing: 1.4px')
        lay.addWidget(self.status); lay.addSpacing(18)
        self.title = label('', wrap=True); self.title.setStyleSheet('font-size: 54px; font-weight: 800; letter-spacing: -1.6px; line-height: 1.05')
        self.title.setMaximumWidth(900); lay.addWidget(self.title); lay.addSpacing(10)
        self.meta = label('', 'meta', wrap=True); self.meta.setMaximumWidth(900); lay.addWidget(self.meta); lay.addStretch(); lay.addSpacing(20)
        row = QHBoxLayout(); row.setSpacing(16)
        self.play = QPushButton('  Play'); self.play.setObjectName('play'); self.play.setIcon(icon('play', '#121212', 22)); self.play.setIconSize(QSize(22, 22)); self.play.setCursor(Qt.PointingHandCursor)
        self.opts = QPushButton(); self.opts.setObjectName('round'); self.opts.setIcon(icon('settings', TEXT, 22)); self.opts.setIconSize(QSize(22, 22)); self.opts.setToolTip('Version and language'); self.opts.setCursor(Qt.PointingHandCursor)
        self.manage = QToolButton(); self.manage.setObjectName('manage'); self.manage.setText(' Manage  '); self.manage.setIcon(icon('more', TEXT, 18)); self.manage.setToolButtonStyle(Qt.ToolButtonTextBesideIcon)
        self.manage.setPopupMode(QToolButton.InstantPopup); self.manage.setCursor(Qt.PointingHandCursor)
        row.addWidget(self.play); row.addWidget(self.opts); row.addWidget(self.manage); row.addStretch(); lay.addLayout(row)

    def show_game(self, g, status, color, hint=''):   # hint: what to do next, after the game's details
        self.pm = g.pm if g else QPixmap(); self.cache = None
        self.status.setText(f'<span style="color:{color}">●&nbsp;&nbsp;{esc(status.upper())}</span>' if g else '')
        self.title.setText(esc(g.name) if g else 'Add your first game')
        if g and g.error and not g.info.get('needs_keys') and not g.info.get('needs_tools'):   # what to do about it, and what the read said, to search or quote
            self.meta.setText(f'{g.help().body}<br><span style="color:{TEXT3}">What it said: {esc(g.error)}</span>')
        elif g:
            bits = [g.id, f'v{g.display}' if g.display else None, (g.info.get('table') or {}).get('engine'), 'played on Android' if g.tested else 'not played on Android yet', hint]
            self.meta.setText('   ·   '.join(b for b in bits if b))
        else: self.meta.setText('Pick the folder with your game\'s NSP or XCI files and your prod.keys, or with a Switch 2 game\'s .nup or dump (no keys): + Build game, at the top right')
        self.update()

    def resizeEvent(self, e):
        self.cache = None; px = 54 if e.size().height() >= 440 else 42
        self.title.setStyleSheet(f'font-size: {px}px; font-weight: 800; letter-spacing: -1.6px'); super().resizeEvent(e)

    def paintEvent(self, _):
        w, h = self.width(), self.height()
        if self.cache is None or self.cache.size() != QSize(w, h):
            pm = QPixmap(w, h); pm.fill(QColor(INK)); p = QPainter(pm); p.setRenderHint(QPainter.Antialiasing); p.setRenderHint(QPainter.SmoothPixmapTransform)
            if not self.pm.isNull():
                p.setOpacity(0.85); p.drawPixmap(0, 0, wash(self.pm, w, h)); p.setOpacity(1)
                art = int(h * 0.82); x, y = w - art - 64, (h - art) // 2   # the game's own picture, sharp, on the right
                path = QPainterPath(); path.addRoundedRect(QRectF(x, y, art, art), 22, 22)
                p.save(); p.setClipPath(path); p.drawPixmap(x, y, art, art, self.pm); p.restore()
                p.setPen(QPen(QColor(255, 255, 255, 26), 1)); p.drawPath(path)
            g = QLinearGradient(0, 0, w, 0)   # the text side darkened so the title reads over any art
            g.setColorAt(0, QColor(11, 11, 12, 245)); g.setColorAt(0.45, QColor(11, 11, 12, 190)); g.setColorAt(0.75, QColor(11, 11, 12, 40)); g.setColorAt(1, QColor(11, 11, 12, 0))
            p.fillRect(0, 0, w, h, g)
            v = QLinearGradient(0, h * 0.55, 0, h); v.setColorAt(0, QColor(11, 11, 12, 0)); v.setColorAt(1, QColor(11, 11, 12, 255)); p.fillRect(0, 0, w, h, v)
            p.end(); self.cache = pm
        QPainter(self).drawPixmap(0, 0, self.cache)


class Card(QWidget):   # one game of the collection: its art in a 16:9 frame (the wash, the picture centred), its name and status
    clicked = Signal(object)
    W, H = 300, 169

    def __init__(self, g):
        super().__init__(); self.g = g; self.picked = False; self.hover = False; self.status = ('', TEXT3)
        self.setFixedSize(self.W, self.H + 78); self.setCursor(Qt.PointingHandCursor); self.setMouseTracking(True)
        self.art = None

    def set_status(self, text, color): self.status = (text, color); self.update()
    def set_picked(self, on): self.picked = on; self.update()
    def enterEvent(self, e): self.hover = True; self.update()
    def leaveEvent(self, e): self.hover = False; self.update()
    def mousePressEvent(self, e): self.clicked.emit(self.g)

    def paintEvent(self, _):
        p = QPainter(self); p.setRenderHint(QPainter.Antialiasing); p.setRenderHint(QPainter.SmoothPixmapTransform)
        r = QRectF(0.5, 0.5, self.W - 1, self.H - 1); path = QPainterPath(); path.addRoundedRect(r, 10, 10)
        p.save(); p.setClipPath(path)
        if self.art is None:
            self.art = QPixmap(self.W, self.H); self.art.fill(QColor(LIFT)); q = QPainter(self.art); q.setRenderHint(QPainter.SmoothPixmapTransform)
            if not self.g.pm.isNull():
                q.drawPixmap(0, 0, wash(self.g.pm, self.W, self.H)); q.fillRect(0, 0, self.W, self.H, QColor(0, 0, 0, 60))
                s = int(self.H * 0.86); q.drawPixmap((self.W - s) // 2, (self.H - s) // 2, s, s, self.g.pm)
            else:   # no art until the game is extracted (the first build's first stages): its name
                gr = QLinearGradient(0, 0, self.W, self.H); gr.setColorAt(0, QColor('#232327')); gr.setColorAt(1, QColor('#141416')); q.fillRect(0, 0, self.W, self.H, gr)
                f = QFont(); f.setFamilies([FONT, 'Segoe UI']); f.setPixelSize(22); f.setWeight(QFont.Bold); q.setFont(f); q.setPen(QColor(TEXT2))
                q.drawText(QRectF(20, 0, self.W - 40, self.H), Qt.AlignCenter | Qt.TextWordWrap, self.g.name)
            q.end()
        p.drawPixmap(0, 0, self.art)
        if self.hover and not self.picked: p.fillRect(r, QColor(255, 255, 255, 14))
        p.restore()
        p.setPen(QPen(QColor(255, 255, 255, 235) if self.picked else QColor(255, 255, 255, 22), 2 if self.picked else 1)); p.drawPath(path)
        f = QFont(); f.setFamilies([FONT, 'Segoe UI']); f.setPixelSize(17); f.setWeight(QFont.DemiBold); p.setFont(f); p.setPen(QColor(TEXT))
        p.drawText(QRectF(0, self.H + 12, self.W, 24), Qt.AlignLeft | Qt.AlignVCenter, p.fontMetrics().elidedText(self.g.name, Qt.ElideRight, self.W))
        text, color = self.status
        p.setPen(Qt.NoPen); p.setBrush(QColor(color)); p.drawEllipse(QRectF(1, self.H + 46, 8, 8))
        f.setPixelSize(14); f.setWeight(QFont.Normal); p.setFont(f); p.setPen(QColor(color if color in (READY,) else TEXT2))
        p.drawText(QRectF(18, self.H + 38, self.W - 18, 24), Qt.AlignLeft | Qt.AlignVCenter, text)
        if self.picked: p.setPen(Qt.NoPen); p.setBrush(QColor(TEXT)); p.drawRoundedRect(QRectF(0, self.H + 72, self.W, 3), 1.5, 1.5)


# ---------------- Library ----------------
class LibraryPage(QWidget):
    def __init__(self, win):
        super().__init__(); self.win = win; self.setObjectName('page'); self.games = {}; self.cards = []; self.picked = None; self.filter = ''
        page = QVBoxLayout(self); page.setContentsMargins(0, 0, 0, 0)
        self.vscroll = QScrollArea(); self.vscroll.setWidgetResizable(True); self.vscroll.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff); page.addWidget(self.vscroll)
        inner = QWidget(); inner.setObjectName('page'); self.vscroll.setWidget(inner)   # too low a window scrolls instead of piling the collection onto the hero
        outer = QVBoxLayout(inner); outer.setContentsMargins(0, 0, 0, 0); outer.setSpacing(0)
        self.hero = Hero(); outer.addWidget(self.hero, 1)
        self.hero.play.clicked.connect(self.play); self.hero.opts.clicked.connect(self.options)
        self.menu = QMenu(self); self.hero.manage.setMenu(self.menu); self.menu.aboutToShow.connect(self.fill_menu)
        body = QWidget(); bl = QVBoxLayout(body); bl.setContentsMargins(48, 8, 48, 24); bl.setSpacing(18)
        head = QHBoxLayout(); head.addWidget(label('Your collection', 'h2')); head.addStretch(); self.count = label('', 'muted'); head.addWidget(self.count); bl.addLayout(head)
        strip = QWidget(); sl = QGridLayout(strip); sl.setContentsMargins(0, 0, 0, 0)
        self.scroll = QScrollArea(); self.scroll.setWidgetResizable(True); self.scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff); self.scroll.setVerticalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
        self.scroll.setFixedHeight(Card.H + 84)
        self.row = QWidget(); self.row.setStyleSheet('background: transparent'); self.rl = QHBoxLayout(self.row); self.rl.setContentsMargins(0, 0, 0, 0); self.rl.setSpacing(20); self.rl.addStretch()
        self.scroll.setWidget(self.row); sl.addWidget(self.scroll, 0, 0); self.scroll.viewport().installEventFilter(self)
        self.next = QPushButton(); self.next.setObjectName('arrow'); self.next.setIcon(icon('right', TEXT, 20)); self.next.clicked.connect(lambda: self.slide(1))
        self.prev = QPushButton(); self.prev.setObjectName('arrow'); self.prev.setIcon(icon('left', TEXT, 20)); self.prev.clicked.connect(lambda: self.slide(-1))
        sl.addWidget(self.next, 0, 0, Qt.AlignRight | Qt.AlignTop); sl.addWidget(self.prev, 0, 0, Qt.AlignLeft | Qt.AlignTop)
        self.next.setStyleSheet(f'margin-top: {Card.H // 2 - 22}px'); self.prev.setStyleSheet(f'margin-top: {Card.H // 2 - 22}px')
        bl.addWidget(strip); self.empty = label('No games yet. + Build game adds the folder with a game\'s NSP or XCI files, or with a Switch 2 game\'s .nup or dump.', 'muted'); bl.addWidget(self.empty)
        outer.addWidget(body, 0)
        self.scroll.horizontalScrollBar().valueChanged.connect(self.arrows); self.scroll.horizontalScrollBar().rangeChanged.connect(lambda *_: self.arrows())
        self.reload()

    def folders(self): return [f for f in (self.win.st.value('folders', []) or []) if f]
    def reload(self):   # one read of the games at a time (each runs --info per folder): one asked for while it runs, as a build's stage ends do, runs once after it
        if getattr(self, 'reading', False): self.again = True; return
        fs = list(self.folders()); seen = {str(Path(f).resolve()) for f in fs}   # resolved: a folder behind a link (Downloads moved to another drive) is the same folder
        names = {}   # a game packed on this computer keeps its name while its folder cannot be read (no keys yet), rather than the folder's (DroidPlay's are all "android")
        for gid, (_, meta) in ba.library().items():   # packed on this computer from a folder not in the list
            if not meta.get('nsp_dir'): continue
            names[str(Path(meta['nsp_dir']).resolve())] = meta.get('name')
            if str(Path(meta['nsp_dir']).resolve()) not in seen: fs.append(meta['nsp_dir']); seen.add(str(Path(meta['nsp_dir']).resolve()))
        if not fs: self.render(); return
        keys = self.win.keys()   # NSP and XCI files are encrypted: --info reads them with the keys (a NUP or a dump folder needs none)
        def read():
            out = {}
            for f in fs:
                if not Path(f).is_dir(): continue
                try: out[f] = self.win.script_json('--info', f, *(['-k', keys] if keys else []))
                except Exception as e:
                    msg = str(e).strip().splitlines()[-1].removeprefix('[RED] ') if str(e).strip() else 'could not be read'
                    # no keys yet, or no tools yet: before Install what's missing has brought the .NET SDK, nothing can read the packages (build_ipa_from_nsp.py's
                    # "needs dotnet"), and every game read as a folder that could not be read
                    out[f] = {'error': msg, 'detail': str(e).strip(), 'needs_keys': not keys and 'prod.keys' in msg, 'needs_tools': 'dotnet' in msg,
                              'name': names.get(str(Path(f).resolve())) or Path(f).name}
            return out
        def got(r):
            self.reading = False
            if isinstance(r, Exception): self.win.toast(f'Reading the games failed: {r}'); r = {}
            self.games, ids = {}, set()
            for f, i in r.items():   # one card per game, from the first folder that holds it
                g = Game(f, i)
                if g.id and g.id in ids: continue
                ids.add(g.id); self.games[f] = g
            self.render()
            if getattr(self, 'again', False): self.again = False; self.reload()
        self.reading = True; bg(read, got); self.win.setup_changed()   # the checklist's last step says it reads the game
    def no_games(self): return not self.games and not getattr(self, 'reading', False)
    def render(self):
        self.read_once = True
        while self.rl.count() > 1:
            w = self.rl.takeAt(0).widget()
            if w: w.deleteLater()
        self.cards = []
        gs = [g for g in self.games.values() if self.filter.lower() in g.name.lower()]
        for g in gs:
            c = Card(g); c.clicked.connect(self.pick); self.rl.insertWidget(self.rl.count() - 1, c); self.cards.append(c)
        n = len(self.games); self.count.setText(f'{n} game' + ('' if n == 1 else 's')); self.empty.setVisible(not n)
        self.refresh_status()
        keep = self.picked if self.picked in self.games else self.win.st.value('picked', None)
        g = self.games.get(keep) or (gs[0] if gs else None)
        self.pick(g); QTimer.singleShot(0, self.arrows)
        if self.win.navs['library'].isChecked() and self.win.pages.currentWidget() is not (self.win.setup if self.no_games() else self): self.win.go('library')
        self.win.setup_changed()
    def refresh_status(self):
        on, phone = self.win.phone_games, self.win.phone_name()
        for c in self.cards: t, col, _ = c.g.status(on, phone); c.set_status(t, col)
        if self.picked in self.games: self.show_hero(self.games[self.picked])
    def pick(self, g):
        self.picked = g.folder if g else None
        if g: self.win.st.setValue('picked', g.folder)
        for c in self.cards: c.set_picked(c.g is g)
        self.show_hero(g)
    def show_hero(self, g):
        if not g: self.hero.show_game(None, '', TEXT3); self.hero.play.setEnabled(False); self.hero.opts.setEnabled(False); return
        text, color, state = g.status(self.win.phone_games, self.win.phone_name())
        self.hero.show_game(g, text, color, ex.RUN_TIME_META if self.run_time_later(g) else ''); self.hero.opts.setEnabled(bool(g.profile))
        self.hero.play.setText({'play': '  Play', 'install': '  Install on phone', 'build': '  Build and install', 'survey': '  Experimental build', 'keys': '  Pick your prod.keys',
                                'tools': '  Install the tools', 'folder': '  Open the folder', 'none': "  Can't read it", 'mac': '  Build it for the Mac first' if sys.platform == 'darwin' else '  Builds on a Mac only'}[state])
        self.hero.play.setEnabled(state not in ('none', 'mac'))
    def run_time_later(self, g):   # a build said the game compiles shaders while it runs (run_time_shaders), which Update shaders brings in: a provisional profile's only
        return bool(g.provisional and self.win.st.value(f'runtime/{g.id}', False, type=bool))
    def current(self): return self.games.get(self.picked)
    def slide(self, d): sb = self.scroll.horizontalScrollBar(); sb.setValue(sb.value() + d * (Card.W + 20) * 2)
    def arrows(self):
        sb = self.scroll.horizontalScrollBar(); self.next.setVisible(sb.value() < sb.maximum()); self.prev.setVisible(sb.value() > 0)
    def eventFilter(self, obj, e):   # the wheel over the collection scrolls it sideways
        if e.type() == e.Type.Wheel and obj is self.scroll.viewport():
            sb = self.scroll.horizontalScrollBar()
            if sb.maximum() > 0: sb.setValue(sb.value() - e.angleDelta().y()); return True
        return super().eventFilter(obj, e)

    # ---- actions ----
    def play(self):
        g = self.current()
        if not g: return
        state = g.status(self.win.phone_games, self.win.phone_name())[2]
        s = self.win.serial()
        if state == 'keys': self.win.settings.pick_keys(); return   # none yet, or keys older than the game
        if state == 'tools': self.win.go('setup'); return   # the checklist's Install tools
        if state == 'folder': reveal(g.folder); return   # what's in the folder is what to change (explain.py's rules for the scan)
        if state in ('play', 'install') and not s: self.win.toast(self.win.no_phone()); return
        if state == 'play':
            if not self.first_start_ok(g, s): return
            bg(lambda: adb_run(s, 'shell', 'am', 'start', '-n', f'{PKG}/.Launcher', '--es', 'game', g.id, '--ez', 'play', 'true'),
               lambda r: self.win.toast(f'Starting {g.name} on {self.win.phone_name()}' if not isinstance(r, Exception) and r[0] else f'Could not start it: {r}'))
        elif state == 'install': self.run(g, ['--push-only'], 'Install')
        elif state == 'build': self.run(g, [], 'Build') if s else self.run(g, ['--no-push'], 'Build')
        elif state == 'survey':   # no profile: the script surveys the game, guesses a provisional profile and packs a best-effort APK (not installed: it may not boot)
            box = QMessageBox(QMessageBox.Warning, 'Experimental build', f'{g.name} has no profile in DroidPlay Core, so it is not supported yet. '
                              'The build extracts the game, writes a survey of it (survey.md in its folder), guesses a provisional profile and packs an APK that '
                              'will most likely not boot. Nothing is installed on the phone. It is the starting point for porting the game.', parent=self)
            go = box.addButton('Build anyway', QMessageBox.AcceptRole); box.setDefaultButton(box.addButton(QMessageBox.Cancel)); box.exec()
            if box.clickedButton() is go: self.run(g, ['--no-push'], 'Experimental build')
    def run(self, g, extra, what, after=None):
        keys = self.win.st.value('keys', '')
        if not keys or not Path(keys).is_file(): self.win.toast('Pick your prod.keys first'); self.win.go('setup'); return
        args = [g.folder, '-k', keys, '-l', self.win.st.value(f'lang/{g.id}', 'en-US') or 'en-US']
        v = self.win.st.value(f'ver/{g.id}', '')
        if v: args += ['-p', v]
        s = self.win.serial()
        data = gd.valid(self.win.st.value(f'data/{g.id}', gd.PHONE))
        if data == gd.SD and gd.flags(data, extra) == ['--romfs-dir', 'sd'] and s:   # the card is looked at now: it can have gone since Options
            if (why := gd.problem(data, extra, self.sd_cards(s))): self.win.toast(why); return
        extra = [*extra, *gd.flags(data, extra)]
        if '--no-push' not in extra and '--game-apk' not in extra:   # a game app needs no phone
            if not s: self.win.toast(self.win.no_phone()); return
            if self.win.phone_legacy and not self.legacy_ok(s): return
            args += ['--serial', s]
        done, key = after or self.after_task, self.win.phone_key(s)   # its ID now: over Wi-Fi, its serial may be another one by the time the build ends
        def installed():   # it went onto that phone: the next Play there is a first start, with its shaders to prepare
            self.win.st.setValue(f'firststart/{key}/{g.id}', True); done()
        self.win.builds.start(f'{what} {g.name}', args + extra, after=installed if '--serial' in args else done)
    def first_start_ok(self, g, s):   # the first Play since the game went onto this phone: say the screen stays black a while, so it isn't taken for a crash
        keys = {f'firststart/{self.win.phone_key(s)}/{g.id}', f'firststart/{s}/{g.id}'}   # kept by the phone's ID; by its serial before IDs
        if not any(self.win.st.value(k, False, type=bool) for k in keys) or self.win.st.value('firststart/never', False, type=bool): return True
        box = QMessageBox(QMessageBox.Information, 'The first start', '<b>The first start takes a minute or two.</b>', parent=self)
        box.setInformativeText(f'{self.win.phone_name()} prepares {g.name}\'s shaders first, and stays on the loading screen meanwhile (80 seconds for Brilliant '
                               'Diamond on a Galaxy S25 Edge, 39 on a OnePlus Pad 3). The phone may get warm. Later starts take a few seconds.')
        box.setStyleSheet('QLabel#qt_msgbox_informativelabel { min-width: 460px; }')   # Qt sizes a message box to its title line: the text wrapped in a narrow column
        never = QCheckBox("Don't show this again"); box.setCheckBox(never)
        go = box.addButton('Play', QMessageBox.AcceptRole); box.addButton(QMessageBox.Cancel); box.setDefaultButton(go); box.exec()
        if never.isChecked(): self.win.st.setValue('firststart/never', True)
        if box.clickedButton() is not go: return False
        for k in keys: self.win.st.remove(k)
        return True
    def game_app(self, g):   # the game as an app of its own (build_android_from_nsp.py --game-apk): no phone, and the launcher app on the phones is left as it is
        ex = (g.info.get('extracted') or {}).get(self.win.st.value(f'ver/{g.id}', '') or g.newest) or {}
        rom = ex.get('romfs_bytes'); data = f'its {gb_text(rom)} of game data' if rom else 'its game data'
        d = QDialog(self); d.setWindowTitle(f'Game app: {g.name}'); d.setStyleSheet(STYLE + f'QDialog {{ background: {PANEL}; }}'); d.setMinimumWidth(560)
        outer = QVBoxLayout(d); outer.setContentsMargins(0, 0, 0, 0); outer.setSpacing(0)
        scroll = QScrollArea(); scroll.setWidgetResizable(True); scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff); outer.addWidget(scroll, 1)
        body = QWidget(); body.setStyleSheet('background: transparent'); scroll.setWidget(body)
        lay = QVBoxLayout(body); lay.setContentsMargins(24, 24, 24, 8); lay.setSpacing(10)
        lay.addWidget(label(f'{g.name} as an app of its own, with its own icon and saves, opening straight into the game on any arm64 device with Vulkan. '
                            'Several games install side by side, without the launcher app.', 'meta', wrap=True))
        lay.addSpacing(6)
        apk = QRadioButton('APK'); lay.addWidget(apk)
        beside = ('as a .romfs file beside the APK, which takes no extra space on the same drive: copy both to the device, put the .romfs anywhere (an SD card '
                  'too) and pick it when the app first opens')
        lay.addWidget(label(f'{gb_text(rom)} of game data is more than an APK holds (2 GB), so the data goes {beside}.' if rom and rom >= ba.GAME_APP_LIMIT else
                            f'One file with {data} inside, while it fits under 2 GB with the game\'s code; otherwise the data goes {beside}.', 'muted', wrap=True))
        xapk = QRadioButton('XAPK'); lay.addWidget(xapk)
        lay.addWidget(label(f'One file, with {data} as the app\'s OBB file, for an XAPK installer app on the device. It holds a full copy of the data, so it takes '
                            'that much more space on this computer.', 'muted', wrap=True))
        (xapk if self.win.st.value('game_app_kind', 'apk') == 'xapk' else apk).setChecked(True)
        sd = QCheckBox('Keep the game data for an SD card'); sd.setChecked(gd.valid(self.win.st.value(f'data/{g.id}', gd.PHONE)) == gd.SD); lay.addWidget(sd)   # the same per-game choice as Options' Game data
        lay.addWidget(label(gd.MEANING_APP + ' With XAPK, the data is in the file.', 'muted', wrap=True))
        xapk.toggled.connect(lambda on: sd.setEnabled(not on)); sd.setEnabled(not xapk.isChecked())
        lay.addSpacing(6); lay.addWidget(label(f'It goes in the game\'s folder, {g.folder}, beside the other kind if that was built too.', 'muted', wrap=True))
        bb = QDialogButtonBox(QDialogButtonBox.Cancel); bb.addButton('Build', QDialogButtonBox.AcceptRole); bb.accepted.connect(d.accept); bb.rejected.connect(d.reject)
        foot = QWidget(); fl = QVBoxLayout(foot); fl.setContentsMargins(24, 8, 24, 24); fl.addWidget(bb); outer.addWidget(foot)
        # as tall as its text at its width, within the screen, and the text scrolls beyond that: left to Qt, a new window keeps within 2/3 of the screen's
        # height, and the wrapped lines were squeezed and cut mid-line on a laptop at 150% (a 1080p screen is 720 high then)
        d.ensurePolished(); need = lay.totalHeightForWidth(560) + foot.sizeHint().height()
        d.resize(560, min(need, int(self.screen().availableGeometry().height() * 0.85)))
        if not d.exec(): return
        x = xapk.isChecked(); self.win.st.setValue('game_app_kind', 'xapk' if x else 'apk')
        if not x: self.win.st.setValue(f'data/{g.id}', gd.SD if sd.isChecked() else gd.PHONE)
        made = f'{ba.safe_name(g.name) or g.id}.{"xapk" if x else "apk"}'
        def done(): self.after_task(); reveal(g.folder); self.win.toast(f'{made} is in the game\'s folder')
        self.run(g, ['--game-apk'] + (['--xapk'] if x else []), 'Build the game app of', after=done)
    def after_task(self): self.win.refresh_phone(); self.reload()
    def sd_cards(self, s):   # the phone's SD cards (volume IDs), by the script's own reading of the same two adb calls
        v = adb_run(s, 'shell', 'sm list-volumes public 2>/dev/null', timeout=8)[1]
        return ba.sd_card_ids(v, '' if ba.sd_card_ids(v, '') else adb_run(s, 'shell', 'ls /storage 2>/dev/null', timeout=8)[1])
    def legacy_ok(self, s):   # the phone holds the one-game install (files/game) and its save: installing moves it to the launcher, and the app copies the save on its next start
        b = QMessageBox(QMessageBox.Warning, 'This phone has a save', f'{self.win.phone_name()} has the app from before the launcher, with its game and save. Installing updates the app '
                        '(its data stays), moves the game folder into the launcher and the app copies the save to the game the next time it opens; the old save is left as it was.\n\n'
                        'Back up its saves first (Phones), and try this on a phone whose save you can lose before the one you care about.', QMessageBox.Cancel, self)
        go = b.addButton('Install anyway', QMessageBox.DestructiveRole); b.setDefaultButton(QMessageBox.Cancel); b.exec()
        return b.clickedButton() is go
    def options(self):
        g = self.current()
        if not g: return
        d = QDialog(self); d.setWindowTitle(g.name); d.setStyleSheet(STYLE + f'QDialog {{ background: {PANEL}; }}'); f = QFormLayout(d); f.setContentsMargins(24, 24, 24, 24); f.setSpacing(14)
        ver = QComboBox(); ex = g.info.get('extracted') or {}
        for v in reversed(g.info.get('versions') or []): ver.addItem(f'{v}' + (f'  ({ex[v]["display"]})' if (ex.get(v) or {}).get('display') else ''), v)
        i = ver.findData(self.win.st.value(f'ver/{g.id}', '') or g.newest); ver.setCurrentIndex(max(i, 0))
        lang = QComboBox()
        for c, n in LANGUAGES:
            if c in g.languages: lang.addItem(f'{n} ({c})', c)
        i = lang.findData(self.win.st.value(f'lang/{g.id}', 'en-US')); lang.setCurrentIndex(max(i, 0))
        f.addRow(label('Version to pack', 'meta'), ver); f.addRow(label('Game language', 'meta'), lang)
        f.addRow(label('The language is the default on the phone; the app\'s settings screen changes it too.', 'muted', wrap=True))
        s = self.win.serial(); cards = self.sd_cards(s) if s else []; saved = gd.valid(self.win.st.value(f'data/{g.id}', gd.PHONE))
        where = QComboBox(); where.addItem(gd.LABELS[gd.PHONE], gd.PHONE)
        if gd.offered(cards) or saved == gd.SD: where.addItem(gd.LABELS[gd.SD] + ('' if gd.offered(cards) else ' (not in the picked phone)'), gd.SD)   # a saved choice stays shown, and changeable, when the card is out
        where.setCurrentIndex(max(where.findData(saved), 0))
        f.addRow(label('Game data', 'meta'), where)
        f.addRow(label(gd.MEANING if gd.offered(cards) or saved == gd.SD else gd.no_card_note(cards), 'muted', wrap=True))
        bb = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Cancel); bb.accepted.connect(d.accept); bb.rejected.connect(d.reject); f.addRow(bb)
        if d.exec(): self.win.st.setValue(f'ver/{g.id}', ver.currentData()); self.win.st.setValue(f'lang/{g.id}', lang.currentData()); self.win.st.setValue(f'data/{g.id}', where.currentData())
    def fill_menu(self):
        self.menu.clear(); g = self.current(); s = self.win.serial()
        if not g: self.menu.addAction('Add a game…', self.win.add_game); return
        def act(text, fn, on=True): a = self.menu.addAction(text, fn); a.setEnabled(on); return a
        act('Build and install', lambda: self.run(g, [], 'Build'), bool(g.profile and s))
        act('Build only', lambda: self.run(g, ['--no-push'], 'Build'), bool(g.profile))
        act('Build a game app…', lambda: self.game_app(g), bool(g.profile))
        act('Install on phone (no build)', lambda: self.run(g, ['--push-only'], 'Install'), bool(g.profile and g.host and s))
        n, later = g.new_shaders(self.win.phone_games), self.run_time_later(g)
        up = act(f'Update shaders from the phone ({n} new)' if n else 'Update shaders from the phone' + (' (after its first scene)' if later else ''),
                 lambda: self.run(g, ['--update-shaders'], 'Update shaders'), bool(g.provisional and g.host and s))
        if n or later: f = up.font(); f.setBold(True); up.setFont(f)   # the next thing to do for this game
        self.menu.addSeparator()
        act('Update the app on the phone', lambda: self.win.builds.start('Update the app', ['--apk-only', '--serial', s], after=self.after_task), bool(s and ba.library()))
        act('Remove from the phone…', lambda: self.win.phones.remove_game(g.id, g.name), bool(s and g.id in self.win.phone_games))
        act('Import a save to the phone…', lambda: self.win.phones.import_save(g.id, g.name), bool(s and g.id in self.win.phone_games))
        act('Restore its save from a backup…', lambda: self.win.phones.restore_backup(g.id, g.name), bool(s and g.id in self.win.phone_games))
        self.menu.addSeparator()
        act('Open the game folder', lambda: reveal(g.folder))
        act('Remove from the collection…', lambda: self.remove(g))
    def remove(self, g):
        if QMessageBox.question(self, 'Remove', f'Remove {g.name} from the collection? Its files are not deleted.') != QMessageBox.Yes: return
        if g.id in ba.library() and QMessageBox.question(self, 'Remove', 'Also take it out of the app (its host on this computer)? The next app update leaves it out; '
                                                         'the game files and the save on the phone stay.') == QMessageBox.Yes:
            shutil.rmtree(ba.data_dir() / 'hosts' / g.id, ignore_errors=True)
        self.win.st.setValue('folders', [f for f in self.folders() if f != g.folder]); self.games.pop(g.folder, None); self.render()


# ---------------- Builds ----------------
class FailBanner(QFrame):   # a build that failed, at the top of the Builds page: what went wrong in plain words (explain.py), the button that fixes it, and the report for #help
    def __init__(self, page):
        super().__init__(); self.page = page; self.help = None; self.setObjectName('failbanner'); self.setAttribute(Qt.WA_StyledBackground, True)
        self.setStyleSheet('QFrame#failbanner { background: rgba(240,106,106,0.08); border: 1px solid rgba(240,106,106,0.35); border-radius: 12px; }')
        lay = QVBoxLayout(self); lay.setContentsMargins(22, 16, 22, 16); lay.setSpacing(6)
        self.title = label(''); self.title.setStyleSheet(f'font-size: 17px; font-weight: 700; color: {ERR}'); lay.addWidget(self.title)
        self.body = label('', 'meta', wrap=True); self.body.setTextFormat(Qt.RichText); lay.addWidget(self.body)
        self.raw = label('', 'muted', wrap=True); self.raw.setTextInteractionFlags(Qt.TextSelectableByMouse); lay.addWidget(self.raw)   # the message itself, to search or quote
        row = QHBoxLayout(); row.setSpacing(10); lay.addSpacing(6); lay.addLayout(row)
        self.act = QPushButton(); self.act.clicked.connect(lambda: self.help and page.win.act(self.help.action, page.context()[0]['folder'])); row.addWidget(self.act)
        self.copy = QPushButton('Copy report'); self.copy.clicked.connect(page.copy_report); row.addWidget(self.copy)
        save = QPushButton('Save full report…'); save.setObjectName('ghost'); save.clicked.connect(page.save_report); row.addWidget(save)
        self.discord = QPushButton('Open Discord'); self.discord.setObjectName('ghost'); self.discord.clicked.connect(lambda: QDesktopServices.openUrl(QUrl(ex.DISCORD))); row.addWidget(self.discord)
        row.addStretch(); self.hide()
    def show_help(self, h, raw):
        self.help = h; self.title.setText(h.title); self.body.setText(h.body)
        raw = raw.strip(); self.raw.setText(f'What it said: {raw[:400]}{"…" if len(raw) > 400 else ""}' if raw else ''); self.raw.setVisible(bool(raw))
        self.act.setText(h.label); self.act.setVisible(bool(h.action)); self.discord.setVisible(h.bug)
        for b, primary in ((self.act, bool(h.action)), (self.copy, not h.action)):   # the one thing to do now in cream: the fix, or the report when only a fix in DroidPlay helps
            b.setObjectName('next' if primary else ''); b.style().unpolish(b); b.style().polish(b)
        self.show()


class NoteBanner(QFrame):   # what the running build says to do next, at the top of the Builds page: amber, as nothing went wrong
    def __init__(self):
        super().__init__(); self.setObjectName('notebanner'); self.setAttribute(Qt.WA_StyledBackground, True)
        self.setStyleSheet('QFrame#notebanner { background: rgba(245,184,46,0.08); border: 1px solid rgba(245,184,46,0.35); border-radius: 12px; }')
        lay = QVBoxLayout(self); lay.setContentsMargins(22, 14, 22, 14); lay.setSpacing(4)
        self.title = label(''); self.title.setStyleSheet(f'font-size: 15px; font-weight: 700; color: {BUILD}'); lay.addWidget(self.title)
        self.body = label('', 'meta', wrap=True); lay.addWidget(self.body); self.hide()
    def show_note(self, title, body): self.title.setText(title); self.body.setText(body); self.show()


class BuildsPage(QWidget):
    progress = Signal(str, str, float, bool)   # title, detail, fraction (-1: unknown), running
    event = Signal(str, object)   # title, a stage event as the script sent it: the checklist draws Install tools' one bar from --setup's
    def __init__(self, win):
        super().__init__(); self.win = win; self.setObjectName('page'); self.proc = None; self.after = None; self.items = {}; self.title = ''; self.buf = ''
        self.last_red = self.last_line = ''   # what a failed build last said: the checklist shows it when Install tools stops
        self.reds, self.stage, self.failed_stage, self.args, self.last, self.bid, self.journal, self.stopping = [], None, None, [], None, None, None, False
        self.gpus = {}   # serial → the GPU its SurfaceFlinger names, for the reports
        self.ids = set(); self.ext = None   # this app's builds (their journals are skipped: their events come on stdout); the console build followed: {path, pid, pos}
        lay = QVBoxLayout(self); lay.setContentsMargins(48, 32, 48, 24); lay.setSpacing(16)
        self.head = label('Builds', 'h1'); lay.addWidget(self.head); self.sub = label('Nothing running. Builds start from the Library.', 'muted'); lay.addWidget(self.sub)
        self.banner = FailBanner(self); lay.addWidget(self.banner)
        self.note = NoteBanner(); lay.addWidget(self.note)
        self.tree = QTreeWidget(); self.tree.setHeaderLabels(['Stage', 'Progress', 'Time', 'Result']); self.tree.setRootIsDecorated(False)
        hh = self.tree.header(); hh.setStretchLastSection(False); hh.setSectionResizeMode(0, QHeaderView.Stretch); hh.setSectionResizeMode(1, QHeaderView.Fixed)
        hh.setSectionResizeMode(2, QHeaderView.ResizeToContents); hh.setSectionResizeMode(3, QHeaderView.Stretch); self.tree.setColumnWidth(1, 220)
        self.tree.setTextElideMode(Qt.ElideMiddle)   # a long step or path keeps both ends; the whole text is the cell's tooltip
        self.tree.setSelectionMode(QTreeWidget.NoSelection); self.tree.setFocusPolicy(Qt.NoFocus); lay.addWidget(self.tree, 2)   # a selected row lost its result's colour
        self.log = QPlainTextEdit(); self.log.setObjectName('log'); self.log.setReadOnly(True); lay.addWidget(self.log, 3)
        row = QHBoxLayout(); b = QPushButton('Open the app folder'); b.setObjectName('ghost'); b.clicked.connect(lambda: reveal(ba.data_dir()))
        row.addWidget(b); row.addStretch(); lay.addLayout(row)
        self.live = None   # (the running stage's row, its detail, its fraction or -1)
        self.jt = QTimer(self); self.jt.timeout.connect(self.follow); self.jt.timeout.connect(self.tick); self.jt.start(1000)
    def busy(self): return self.proc is not None and self.proc.state() != QProcess.NotRunning
    def follow(self):   # a build from the console: the newest journal with no exit event whose process runs
        if self.busy(): return
        if self.ext is None:
            d = ba.data_dir() / 'builds'
            for f in (sorted(d.glob('*.jsonl'), reverse=True)[:3] if d.is_dir() else []):
                try:
                    if time.time() - f.stat().st_mtime > 6 * 3600: break
                    text = f.read_text(encoding='utf-8', errors='replace'); first = json.loads(text.split('\n', 1)[0])
                except (OSError, ValueError): continue
                if first.get('id') in self.ids or '"ev": "exit"' in text or not alive(int(first.get('pid') or 0)): continue
                self.ext = {'path': f, 'pid': int(first['pid']), 'pos': 0}; self.title = journal_title(first.get('argv') or [])
                self.head.setText(self.title + '  ·  from the console'); self.sub.setText('following its journal'); self.tree.clear(); self.items = {}; self.log.clear(); self.buf = ''
                self.fresh(first.get('argv') or [], None); self.journal = f; self.last = (self.title, first.get('argv') or [], None)   # Install again runs it here
                self.t0 = first.get('t') or time.time(); self.say('$ build_android_from_nsp.py ' + ' '.join(first.get('argv') or []), TEXT3)
                self.progress.emit(self.title, 'running', -1, True); break
            if self.ext is None: return
        try:
            with open(self.ext['path'], 'rb') as fh: fh.seek(self.ext['pos']); data = fh.read()
        except OSError: data = b''
        end = data.rfind(b'\n') + 1; self.ext['pos'] += end   # whole lines only: the last may still be written
        done = None
        for line in data[:end].decode('utf-8', 'replace').splitlines():
            try: ev = json.loads(line)
            except ValueError: continue
            if ev.get('ev') == 'exit': done = ev.get('code', 1)
            elif ev.get('ev') != 'start': self.feed(line)
        if done is None and not alive(self.ext['pid']): done = -1
        if done is not None:
            ok = done == 0; self.ext = None; self.live = None
            self.say('— the console build ' + ('finished' if ok else 'stopped' if done == -1 else f'failed (exit {done})'), READY if ok else ERR)
            h = self.show_failure() if not ok and done != -1 else None
            self.sub.setText(f'done · {self.ended(ok)}' if ok else 'stopped' if done == -1 else h.title if h else '')
            self.progress.emit(self.title, self.ended(ok) if ok else h.title if h else 'stopped', 1.0 if ok else -1, False)
            if ok: self.win.library.after_task()
    def start(self, title, args, after=None):
        if self.busy() or self.ext: QMessageBox.warning(self, 'Busy', 'A build is running: wait for it or stop it'); return
        self.live = None; self.title = title; self.head.setText(title); self.sub.setText('starting…'); self.tree.clear(); self.items = {}; self.log.clear(); self.after = after; self.buf = ''
        if '--serial' in args[:-1] and '--phone-id' not in args and (pid := self.win.ids.get(args[args.index('--serial') + 1])):
            args = [*args, '--phone-id', pid]   # the script finds the phone by its ID when adb loses it on the way (a new Wi-Fi port)
        bid = uuid.uuid4().hex; self.ids.add(bid); self.fresh(args, bid); self.last = (title, args, after)
        self.proc = QProcess(self); self.proc.setProgram(PYTHON); self.proc.setArguments([str(SCRIPT), *args, '--progress', 'json'])
        env = QProcessEnvironment.systemEnvironment(); env.insert('PYTHONUTF8', '1'); env.insert('PYTHONUNBUFFERED', '1'); env.insert('NXNATIVE_BUILD_ID', bid); self.proc.setProcessEnvironment(env)
        self.proc.setProcessChannelMode(QProcess.MergedChannels); self.proc.readyRead.connect(self.read); self.proc.finished.connect(self.finished)
        if NEW_SESSION is not None: self.proc.setUnixProcessParameters(NEW_SESSION)   # a process group its tools join, which Stop ends (procs.py)
        self.proc.start(); self.t0 = time.time(); self.say(f'$ python {SCRIPT.name} ' + ' '.join(args), TEXT3)
        self.progress.emit(title, 'starting…', -1, True)
    def fresh(self, args, bid):   # a build starts: what the last one said and where it stopped are forgotten
        self.last_red = self.last_line = ''; self.reds, self.stage, self.failed_stage, self.args, self.bid, self.journal, self.stopping = [], None, None, list(args), bid, None, False
        self.banner.hide(); self.note.hide()
    def say(self, text, color=None):
        if color is None:
            color = ERR if '[RED]' in text else None
            if text.strip(): self.last_line = text.strip()
            if '[RED]' in text: self.last_red = text.split('[RED]', 1)[1].strip(); self.reds.append(text.strip())
        self.log.appendHtml(f'<span style="color:{color}">{esc(text)}</span>' if color else esc(text))
    def read(self):
        if not self.proc: return
        self.buf += bytes(self.proc.readAll()).decode('utf-8', 'replace')
        *lines, self.buf = self.buf.split('\n')
        for line in lines: self.feed(line.rstrip('\r'))
    def feed(self, line):   # one line of the script's output (or of a journal): an event, or a plain message
        try: ev = json.loads(line) if line.startswith('{') else None
        except ValueError: ev = None
        if not ev:
            if line.strip(): self.say(line)
            return
        k = ev.get('ev')
        if k == 'say': self.say(ev.get('msg', '')); return
        if k == 'run_time_shaders': self.run_time(ev); return
        if k not in ('stage', 'step', 'progress', 'stage_end'): return
        self.event.emit(self.title, ev)
        if k == 'stage': self.stage = (ev.get('k'), ev.get('n'), ev.get('name', ''))
        if k == 'stage_end' and not ev.get('ok') and not self.failed_stage:   # where it went wrong first (the stage's count comes with its start)
            self.failed_stage = (ev.get('k'), self.stage[1] if self.stage and self.stage[0] == ev.get('k') else None, ev.get('name', ''))
        key = ev.get('k'); it = self.items.get(key)
        if it is None:
            it = QTreeWidgetItem([f'{key}/{ev.get("n", "?")}   {ev.get("name", "")}', '', '', '']); self.tree.addTopLevelItem(it); self.items[key] = it
            bar = QProgressBar(); bar.setRange(0, 0); bar.setTextVisible(False); self.tree.setItemWidget(it, 1, bar); it.t0 = ev.get('t') or time.time(); it.n = ev.get('n', '?'); it.name = ev.get('name', '')
        bar = self.tree.itemWidget(it, 1); frac = -1.0
        if ev.get('step'): it.setText(0, f'{key}/{it.n}   {it.name} · {ev["step"]}')
        it.setToolTip(0, it.text(0))
        if k == 'progress' and ev.get('total'):
            frac = (ev.get('done') or 0) / ev['total']; bar.setRange(0, 1000); bar.setValue(int(1000 * frac))
        it.setText(2, clock_text(time.time() - it.t0))
        detail = f'{it.name}' + (f' · {ev["step"]}' if ev.get('step') else '')
        if k == 'progress' and ev.get('total'): detail += (f' · {ev["note"]}' if ev.get('note') else '') + ' · ' + count_text(ev.get('done') or 0, ev['total'], ev.get('unit') or '')
        if ev.get('eta'): detail += f' · {int(ev["eta"])}s left'
        if k == 'stage_end':
            bar.setRange(0, 1); bar.setValue(1); tint(bar, READY if ev.get('ok') else ERR); it.setText(0, f'{key}/{it.n}   {it.name}'); it.setToolTip(0, it.text(0))   # its last step was not still going on
            it.setText(2, clock_text(ev.get('elapsed', 0))); it.setText(3, ev.get('summary', '')); it.setToolTip(3, ev.get('summary', '')); it.setForeground(3, QColor(READY if ev.get('ok') else ERR))
            detail = f'{it.name} done'; frac = 1.0; QTimer.singleShot(0, self.win.library.reload)
        self.sub.setText(f'stage {key} of {it.n} · {detail}')
        self.live = None if k == 'stage_end' else (it, detail, frac if frac >= 0 else (self.live[2] if self.live and self.live[0] is it and k != 'step' else -1))
        self.progress.emit(self.title, detail, self.live[2] if self.live else frac, True)
    def run_time(self, ev):   # build_android_from_nsp.py run_time_note: a provisional profile's game compiles shaders while it runs; the Library then points at Update shaders for it
        if ev.get('title_id'): self.win.st.setValue(f'runtime/{ev["title_id"]}', True)
        self.note.show_note(*ex.run_time_note(ev.get('name') or '', bool(ev.get('game_app'))))
    def tick(self):   # the running stage's clock, every second: a long step (adb copying the RomFS) sends no events
        if not self.live or not (self.busy() or self.ext): return
        it, detail, frac = self.live; t = clock_text(time.time() - it.t0)
        it.setText(2, t); self.progress.emit(self.title, f'{detail} · {t}', frac, True)
    def finished(self, code, _status):
        self.read()
        ok = code == 0; self.live = None
        if self.stopping:   # the player pressed Stop: nothing went wrong to explain
            self.say(f'— stopped after {time.time() - self.t0:.0f} s', TEXT3); self.sub.setText('stopped'); self.progress.emit(self.title, 'stopped', -1, False); self.after = None; return
        self.say(f'— finished in {time.time() - self.t0:.0f} s, exit {code}', READY if ok else ERR)
        h = None if ok else self.show_failure()
        self.sub.setText(f'done · {self.ended(ok)}' if ok else h.title if h else '')
        self.progress.emit(self.title, self.ended(ok) if ok else h.title if h else '', 1.0 if ok else -1, False)
        after, self.after = self.after, None
        if ok and after: QTimer.singleShot(200, after)
        if h: self.win.toast(f'{h.title}: see Builds')
    def retry(self):   # a failed build's Install again / Try again: the same build, which reuses what it made before, on the same phone by its ID: over Wi-Fi its serial
        # changes when wireless debugging restarts, and the old one named no device any more
        if not self.last: return
        title, args, after = self.last
        if '--serial' in args[:-1]:
            i = args.index('--serial') + 1; now = self.win.serial_of(args[i])
            if now and now != args[i]: args = args[:i] + [now] + args[i + 1:]
        self.start(title, args, after)
    def context(self):   # (explain()'s context, the game, the phone's serial, the stage it stopped in) of the last build, from its arguments
        a = self.args; folder = a[0] if a and not a[0].startswith('-') else ''
        g = self.win.library.games.get(folder) or next((x for x in self.win.library.games.values() if folder and Path(x.folder).resolve() == Path(folder).resolve()), None)
        serial = a[a.index('--serial') + 1] if '--serial' in a[:-1] else ''
        st = self.failed_stage or self.stage or (None, None, '')
        return {'phone': self.win.models.get(self.win.serial_of(serial) or serial, '') if serial else '', 'game': g.name if g else '', 'stage': st[2],
                'setup': self.title == fr.SETUP_TITLE, 'folder': folder, 'wifi': phone_id.wifi(serial) if serial else False}, g, serial, st
    def show_failure(self):
        ctx = self.context()[0]
        h = ex.explain('\n'.join(self.reds[-6:] + [self.last_line]), **ctx); self.banner.show_help(h, self.last_red or self.last_line); return h
    def report_info(self):   # the report's header: the phone by its model, never the name its owner gave it
        ctx, g, serial, st = self.context(); d = next((d for d in self.win.devs or [] if d['serial'] == serial), {})
        return {'version': self.win.version, 'title': self.title, 'stage': st[2], 'k': st[0], 'n': st[1], 'game': g.name if g else '', 'game_id': g.id if g else '',
                'game_version': (g.display or g.newest) if g else '', 'phone': d.get('model') or ('a phone' if serial else ''), 'android': d.get('android', '')}, serial
    def copy_report(self):   # Markdown for #help, under Discord's 2,000 characters: the phone's GPU asked for first (adb, a second or two)
        info, serial = self.report_info(); reds, lines = list(self.reds), self.log.toPlainText().splitlines()
        def gpu():
            if not serial or self.gpus.get(serial): return self.gpus.get(serial, '')
            ok, out = adb_run(serial, 'shell', 'dumpsys SurfaceFlinger | grep -m1 GLES:', timeout=6)
            m = re.search(r'GLES:\s*[^,]*,\s*([^,]+)', out) if ok else None   # "GLES: Qualcomm, Adreno (TM) 830, OpenGL ES 3.2 …"
            return m[1].strip() if m else ''
        def got(gp):
            info['gpu'] = self.gpus[serial] = '' if isinstance(gp, Exception) else gp
            QApplication.clipboard().setText(ex.markdown_report(info, reds, lines, str(Path.home())))
            self.win.toast('The report is copied: paste it in #help on the DroidPlay Discord')
        bg(gpu, got)
    def save_report(self):   # the whole log and where its journal is, redacted the same way
        info, serial = self.report_info(); info['gpu'] = self.gpus.get(serial, '')   # as Copy report asked the phone, if it did
        docs = Path(QStandardPaths.writableLocation(QStandardPaths.DocumentsLocation) or Path.home())   # Linux's may be named otherwise (xdg-user-dirs), or not exist
        name = f'DroidPlay report {time.strftime("%Y-%m-%d %H%M")}.txt'
        f, _ = QFileDialog.getSaveFileName(self, 'Save the full report', str((docs if docs.is_dir() else Path.home()) / name), 'Text (*.txt)')
        if not f: return
        try: Path(f).write_text(ex.full_report(info, self.log.toPlainText(), self.journal_path() or '', str(Path.home())), encoding='utf-8')
        except OSError as e: QMessageBox.warning(self, 'Save full report', str(e)); return
        self.win.toast(f'Saved {Path(f).name}: attach it in #help on the DroidPlay Discord')
    def journal_path(self):   # this build's journal (<data>/builds), which has every event and message
        if self.journal: return self.journal
        d = ba.data_dir() / 'builds'
        bid = self.bid or ''
        for f in (sorted(d.glob('*.jsonl'), reverse=True)[:5] if d.is_dir() and bid else []):
            try:
                with open(f, encoding='utf-8', errors='replace') as fh:
                    if bid in fh.readline(): return f
            except OSError: continue
        return None
    def ended(self, ok):   # what the bar says under "<build> — done" or "— failed", rather than the same word again
        if not ok: return 'see the messages on the Builds page'
        n = len(self.items); took = clock_text(time.time() - self.t0)
        return f'{n} stage{"" if n == 1 else "s"} in {took}' if n else f'took {took}'
    def stop(self):
        if self.ext:   # a console build: its process tree
            if QMessageBox.question(self, 'Stop', f'Stop {self.title}, started from the console?') != QMessageBox.Yes: return
            procs.stop_tree(self.ext['pid'], force=False)
            return
        if not self.busy() or not self.proc: return
        self.stopping = True
        procs.stop_tree(self.proc.processId())   # its tools too


class BuildBar(QWidget):   # the running build at the bottom of every page
    def __init__(self, win):
        super().__init__(); self.setObjectName('buildbar'); self.setAttribute(Qt.WA_StyledBackground, True); self.win = win
        lay = QHBoxLayout(self); lay.setContentsMargins(28, 16, 28, 16); lay.setSpacing(20)
        ic = QLabel(); ic.setPixmap(icon('tools', TEXT2, 26).pixmap(26, 26)); lay.addWidget(ic)
        col = QVBoxLayout(); col.setSpacing(2); self.title = label(''); self.title.setStyleSheet('font-size: 15px; font-weight: 700'); self.detail = label('', 'meta')
        col.addWidget(self.title); col.addWidget(self.detail); lay.addLayout(col, 3)
        self.bar = QProgressBar(); self.bar.setRange(0, 1000); self.bar.setTextVisible(False); lay.addWidget(self.bar, 4)
        self.pct = label(''); self.pct.setStyleSheet(f'font-size: 15px; font-weight: 600; color: {TEXT2}'); self.pct.setMinimumWidth(48); lay.addWidget(self.pct)
        self.view = QPushButton('View build'); self.view.clicked.connect(lambda: win.go('builds')); lay.addWidget(self.view)
        self.stop = QPushButton('Stop'); self.stop.setObjectName('stop'); self.stop.clicked.connect(win.stop_build); lay.addWidget(self.stop)
        self.hide(); self.hide_timer = QTimer(self); self.hide_timer.setSingleShot(True); self.hide_timer.timeout.connect(self.hide)
    def update_from(self, title, detail, frac, running):
        self.show(); self.hide_timer.stop()
        self.title.setText(f'{title} in progress' if running else title + (' — done' if frac >= 0 else ' — failed'))
        self.detail.setText(detail); tint(self.bar, BUILD if running else READY if frac >= 0 else ERR)
        if frac < 0: self.bar.setRange(0, 0 if running else 1000); self.pct.setText('')
        else: self.bar.setRange(0, 1000); self.bar.setValue(int(frac * 1000)); self.pct.setText(f'{frac * 100:.0f}%')
        self.stop.setVisible(running)
        if not running: self.hide_timer.start(8000)


# ---------------- Phones ----------------
class KeepColour(QStyledItemDelegate):   # a selected row keeps its coloured text (a phone's state in green or red): the selection painted all of it in one colour
    def initStyleOption(self, opt, index):
        super().initStyleOption(opt, index)
        fg = index.data(Qt.ForegroundRole)
        if fg is not None: opt.palette.setBrush(QPalette.HighlightedText, fg)


STATES = {'device': 'connected', 'unauthorized': 'allow USB debugging on the phone', 'authorizing': 'allow USB debugging on the phone', 'offline': 'offline',
          phone_id.NO_PERMISSIONS: 'USB access needed: How to connect a phone'}   # adb's words
NO_ADB = "adb isn't installed, so DroidPlay can't see your phones. It comes with the tools: Get started, step 2."
NO_PHONE = (f'No phone connected. Plug one in with USB debugging on and allow this computer when the phone asks, or pair one over Wi-Fi. '
            f'<a style="color:{READY}" href="guide">How to connect a phone</a>')


class PhoneGuide(QDialog):   # how to connect a phone, by brand (dm/android/desktop/phone_guide.py), with what DroidPlay sees now at its foot, live
    def __init__(self, win):
        super().__init__(win); self.win = win; self.setWindowTitle('Set up your phone'); self.setStyleSheet(STYLE + f'QDialog {{ background: {PANEL}; }}')
        outer = QVBoxLayout(self); outer.setContentsMargins(0, 0, 0, 0); outer.setSpacing(0)
        scroll = QScrollArea(); scroll.setWidgetResizable(True); scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff); outer.addWidget(scroll, 1)
        body = QWidget(); body.setStyleSheet('background: transparent'); scroll.setWidget(body)
        lay = QVBoxLayout(body); lay.setContentsMargins(28, 24, 28, 8); lay.setSpacing(12)
        lay.addWidget(label('Set up your phone', 'h2')); lay.addWidget(label(pg.INTRO, 'meta', wrap=True))
        row = QHBoxLayout(); row.addWidget(label('Your phone is a', 'meta')); self.brand = QComboBox()
        for k, name, *_ in pg.BRANDS: self.brand.addItem(name, k)
        row.addWidget(self.brand, 1); lay.addLayout(row)
        self.steps = label('', wrap=True); self.steps.setTextFormat(Qt.RichText); self.steps.setStyleSheet('font-size: 14px'); lay.addWidget(self.steps)
        self.note = label('', 'muted', wrap=True); self.note.setTextFormat(Qt.RichText); self.note.setOpenExternalLinks(True); lay.addWidget(self.note)
        self.more = QToolButton(); self.more.setObjectName('manage'); self.more.setText(" If it doesn't work"); self.more.setCheckable(True); self.more.setCursor(Qt.PointingHandCursor)
        self.more.setIcon(icon('right', TEXT, 16)); self.more.setToolButtonStyle(Qt.ToolButtonTextBesideIcon); lay.addWidget(self.more)
        link = lambda h: h.replace('<a ', f'<a style="color:{READY}" ')   # noqa: E731  (the app's green, not the palette's blue on the dark panel)
        self.trouble = label(link(''.join(f'<p><b>{t}{"" if t[-1] in "?!." else "."}</b> {x}</p>' for t, x in pg.trouble())), 'meta', wrap=True); self.trouble.setTextFormat(Qt.RichText)
        self.trouble.setTextInteractionFlags(Qt.TextBrowserInteraction)   # the udev rule and commands, to select and copy
        self.trouble.setOpenExternalLinks(True); self.trouble.hide(); lay.addWidget(self.trouble); lay.addStretch()
        self.more.toggled.connect(lambda on: (self.trouble.setVisible(on), self.more.setIcon(icon('down' if on else 'right', TEXT, 16))))
        foot = QWidget(); fl = QHBoxLayout(foot); fl.setContentsMargins(28, 12, 28, 22); fl.setSpacing(16)
        self.seen = label('', 'meta', wrap=True); self.seen.setTextFormat(Qt.RichText); fl.addWidget(self.seen, 1)
        done = QPushButton('Done'); done.setMinimumWidth(110); done.clicked.connect(self.accept); fl.addWidget(done); outer.addWidget(foot)
        sep = QFrame(); sep.setFixedHeight(1); sep.setStyleSheet(f'background: {LINE}'); outer.insertWidget(1, sep)
        self.brand.setCurrentIndex(max(0, self.brand.findData(win.phone_brand()))); self.brand.currentIndexChanged.connect(self.show_brand); self.show_brand()
        self.t = QTimer(self); self.t.timeout.connect(self.watch); self.t.start(1000); self.watch()
        self.ensurePolished(); need = lay.totalHeightForWidth(640) + foot.sizeHint().height() + 40   # as tall as its steps, within the screen (the game-app dialog's rule)
        self.resize(640, min(max(need, 680), int(win.screen().availableGeometry().height() * 0.85)))   # rich text's height for width comes out short: room for If it doesn't work
    def show_brand(self):
        k = self.brand.currentData(); steps = next(b[2] for b in pg.BRANDS if b[0] == k); note = pg.brand_note(k)
        self.steps.setText('<ol style="margin: 0">' + ''.join(f'<li style="margin-bottom: 10px">{x}</li>' for x in steps) + '</ol>')
        self.note.setText(note.replace('<a ', f'<a style="color:{READY}" ')); self.note.setVisible(bool(note)); self.win.st.setValue('phone_brand', k)
    def watch(self):   # the checklist's phone step, as DroidPlay sees it now (it looks every few seconds)
        s = fr.phone_step(self.win.devs, self.win.adb, self.win.serial())
        c = READY if s.done else BUILD if s.state in (fr.WAITING, fr.WORKING) else ERR if s.state == fr.PROBLEM else TEXT2
        text = (f'✓ DroidPlay sees {s.text}' if s.done else s.text if s.state != fr.TODO else "DroidPlay doesn't see a phone yet: it looks every few seconds." if self.win.adb
                else 'DroidPlay finds phones with adb, which comes with the tools: install them first (Get started, step 2).')
        self.seen.setText(f'<span style="color:{c}">{text}</span>')


class PairDialog(QDialog):   # Pair over Wi-Fi: the phones showing a pairing code, found on the network by adb (mDNS) with their address filled in, so the player
    # types only the code; then the phone's connection, which adb makes by itself or DroidPlay makes to the address the phone offers. It asked for two addresses
    # by hand before, the second one easy to get wrong (the Wireless debugging screen's, not the pairing one's)
    WAIT = 10   # seconds after pairing for adb to connect to the phone by itself, before DroidPlay connects to it, or asks for the address
    def __init__(self, win):
        super().__init__(win); self.win = win; self.setWindowTitle('Pair over Wi-Fi'); self.setStyleSheet(STYLE + f'QDialog {{ background: {PANEL}; }}'); self.setMinimumWidth(560)
        lay = QVBoxLayout(self); lay.setContentsMargins(28, 24, 28, 22); lay.setSpacing(12)
        lay.addWidget(label('Pair over Wi-Fi', 'h2'))
        steps = label('<ol style="margin: 0">' + ''.join(f'<li style="margin-bottom: 6px">{x}</li>' for x in wifi.STEPS) + '</ol>', wrap=True); steps.setTextFormat(Qt.RichText); lay.addWidget(steps)
        self.list = QListWidget(); self.list.setMaximumHeight(110); lay.addWidget(self.list)
        self.addr = QLineEdit(); self.addr.setPlaceholderText('Not listed? The IP address & port shown with the code, e.g. 192.168.1.20:37123'); lay.addWidget(self.addr)
        row = QHBoxLayout(); row.addWidget(label('Pairing code', 'meta')); self.code = QLineEdit(); self.code.setMaxLength(6); self.code.setPlaceholderText('6 digits'); self.code.setFixedWidth(120)
        row.addWidget(self.code); row.addStretch(); lay.addLayout(row)
        self.status = label('', 'meta', wrap=True); self.status.setTextFormat(Qt.RichText); lay.addWidget(self.status)
        foot = QHBoxLayout(); foot.addStretch(); cancel = QPushButton('Cancel'); cancel.clicked.connect(self.reject); foot.addWidget(cancel)
        self.go = QPushButton('Pair'); self.go.setDefault(True); self.go.clicked.connect(self.pair); foot.addWidget(self.go); lay.addLayout(foot)
        for w in (self.code, self.addr): w.textChanged.connect(self.ready)
        self.list.currentItemChanged.connect(self.ready); self.code.returnPressed.connect(self.pair)
        self.busy, self.found, self.deadline, self.target = False, [], 0.0, ('', None)
        self.t = QTimer(self); self.t.timeout.connect(self.look); self.t.start(2000); self.look(); self.ready()
    def look(self):   # the pairing services on the network, every 2 s while the dialog is open
        if not self.busy: bg(lambda: adb_run(None, 'mdns', 'services', timeout=8)[1], self.listed)
    def listed(self, out):
        if isinstance(out, Exception) or self.busy: return
        found = wifi.pairing(out)
        if found == self.found and self.list.count(): return
        keep = self.list.currentItem().data(Qt.UserRole) if self.list.currentItem() else None
        self.found = found; self.list.blockSignals(True); self.list.clear()
        for text, addr in found:
            it = QListWidgetItem(text); it.setData(Qt.UserRole, addr); self.list.addItem(it)
            if addr == keep or len(found) == 1: self.list.setCurrentItem(it)
        if not found:
            it = QListWidgetItem('Looking for a phone showing a pairing code…'); it.setFlags(Qt.NoItemFlags); self.list.addItem(it)
        self.list.blockSignals(False); self.ready()
    def address(self):
        typed = self.addr.text().strip()
        return typed or (self.list.currentItem().data(Qt.UserRole) if self.list.currentItem() else None)
    def ready(self, *_): self.go.setEnabled(not self.busy and bool(self.address()) and re.fullmatch(r'\d{6}', self.code.text().strip()) is not None)
    def say(self, text, colour=TEXT2): self.status.setText(f'<span style="color:{colour}">{text}</span>')
    def pair(self):
        addr, code = self.address(), self.code.text().strip()
        if not self.go.isEnabled() or not addr: return
        self.busy = True; self.ready(); self.say('Pairing…', BUILD)
        bg(lambda: adb_run(None, 'pair', addr, code, timeout=30), lambda r: self.paired(addr, r))
    def paired(self, addr, r):
        ok, sn, text = wifi.pair_result(*r) if not isinstance(r, Exception) else (False, None, str(r))
        if not ok: self.busy = False; self.ready(); self.say(text, ERR); return
        self.say('Paired. Connecting…', BUILD); self.deadline = time.time() + self.WAIT; self.target = (addr.rsplit(':', 1)[0], sn); self.connect_paired()
    def connect_paired(self):   # adb connects a paired phone by itself when it sees it on the network; else the phone's own address is asked for its connection
        ip, sn = self.target
        def look():
            listed = wifi.listed(adb_run(None, 'devices', timeout=10)[1], sn, ip)
            if listed: return True, f'connected to {listed}'
            to = wifi.connect_target(adb_run(None, 'mdns', 'services', timeout=8)[1], sn, ip)
            return wifi.connect_result(*adb_run(None, 'connect', to, timeout=15)) + (to,) if to else None
        def got(r):
            if isinstance(r, Exception): r = None
            if r and r[0]: self.connected(); return
            if time.time() < self.deadline: QTimer.singleShot(1500, self.connect_paired); return
            self.ask_address(ip)
        bg(look, got)
    def ask_address(self, ip):   # neither adb nor the network offered the phone's connection: the Wireless debugging screen's address, by hand
        addr, ok = QInputDialog.getText(self, 'Connect', 'Paired. Now the IP address & port on the phone\'s Wireless debugging screen itself (not the pairing one):', text=f'{ip}:')
        if not ok or not addr.strip(): self.busy = False; self.ready(); self.say('Paired, not connected: <b>Connect…</b> on the Phones page takes the address later.', BUILD); return
        self.say('Connecting…', BUILD)
        def got(r):
            ok2, text = wifi.connect_result(*r) if not isinstance(r, Exception) else (False, str(r))
            if ok2: self.win.st.setValue('wifi', addr.strip()); self.connected()
            else: self.busy = False; self.ready(); self.say(text, ERR)
        bg(lambda: adb_run(None, 'connect', addr.strip(), timeout=30), got)
    def connected(self):   # not done(): that is QDialog's own, which accept() calls
        self.t.stop(); self.win.refresh_devices(); self.win.toast('Connected over Wi-Fi: the phone is in the list at the top'); self.accept()


class PhonesPage(QWidget):
    def __init__(self, win):
        super().__init__(); self.win = win; self.setObjectName('page')
        lay = QVBoxLayout(self); lay.setContentsMargins(48, 32, 48, 24); lay.setSpacing(16)
        lay.addWidget(label('Phones', 'h1'))
        lay.addWidget(label('Over Wi-Fi (Android 11 and later): turn on Developer options › Wireless debugging and open "Pair device with pairing code"; Pair over Wi-Fi… '
                            'then finds the phone and asks only for its code. For Connect… by hand, '
                            'the address on the Wireless debugging screen itself is the one to Connect to (its port changes when wireless debugging restarts).', 'muted', wrap=True))
        self.table = QTableWidget(0, 7); self.table.setHorizontalHeaderLabels(['Device', 'Model', 'Android', 'Battery', 'Free', 'Connection', 'State'])
        hh = self.table.horizontalHeader(); hh.setSectionResizeMode(QHeaderView.Stretch); hh.setSectionResizeMode(2, QHeaderView.ResizeToContents)
        for i in (3, 4): hh.setSectionResizeMode(i, QHeaderView.ResizeToContents)
        hh.setDefaultAlignment(Qt.AlignLeft | Qt.AlignVCenter)
        self.table.verticalHeader().hide(); self.table.setShowGrid(False); self.table.setItemDelegate(KeepColour(self.table))
        self.table.setSelectionBehavior(QTableWidget.SelectRows); self.table.setEditTriggers(QTableWidget.NoEditTriggers); self.table.itemSelectionChanged.connect(self.selected)
        self.table.setMaximumHeight(200); lay.addWidget(self.table)
        self.note = label('', 'meta', wrap=True); self.note.hide(); lay.addWidget(self.note)   # why the list is empty: no adb, or no phone
        self.note.setTextFormat(Qt.RichText); self.note.linkActivated.connect(lambda _: win.phone_guide())
        self.warn = label('', wrap=True); self.warn.setStyleSheet(f'color: {BUILD}; font-size: 13px'); self.warn.hide(); lay.addWidget(self.warn)   # the picked phone's low battery
        row = QHBoxLayout(); self.adb_buttons = []
        b = QPushButton('How to connect a phone…'); b.clicked.connect(win.phone_guide); row.addWidget(b)
        for text, fn in (('Pair over Wi-Fi…', self.pair), ('Connect…', self.connect), ('Discover', self.discover), ('Disconnect', self.disconnect), ('Use over Wi-Fi…', self.tcpip)):
            b = QPushButton(text); b.clicked.connect(fn); row.addWidget(b); self.adb_buttons.append(b)
        row.addStretch(); lay.addLayout(row)
        lay.addSpacing(10); self.ph = label('The picked phone', 'h2'); lay.addWidget(self.ph); self.app = label('—', 'meta', wrap=True); lay.addWidget(self.app)
        self.games = QListWidget(); lay.addWidget(self.games, 1)
        row = QHBoxLayout()
        for text, fn, name in (('Update the app', self.update_app, None), ('Back up saves to this computer', self.backup, None), ('Restore a backup…', self.restore_all, None),
                               ('Move the old one-game install…', self.migrate, 'ghost')):
            b = QPushButton(text); b.clicked.connect(fn); row.addWidget(b)
            if name: b.setObjectName(name)
        row.addStretch(); lay.addLayout(row)
    def fill(self, devs, adb=True):   # the phone's own name and its model's, with the serial beside how it is connected
        self.table.setRowCount(len(devs))
        for i, d in enumerate(devs):
            how = 'Wi-Fi' if ':' in d['serial'] or '._adb-tls' in d['serial'] else 'USB'
            bat = f'{d["battery"]}%' + (' charging' if d.get('charging') and d['battery'] < 100 else '') if d.get('battery') is not None else ''
            free = pg.size_text(d['free']) if d.get('free') is not None else ''
            for j, v in enumerate((d.get('name') or d['serial'], d.get('model', ''), d.get('android', ''), bat, free, f'{how} · {d["serial"]}', STATES.get(d['state'], d['state']))):
                it = QTableWidgetItem(v); it.setData(Qt.UserRole, d['serial'])
                if j == 6: it.setForeground(QColor(READY if d['state'] == 'device' else ERR))
                if j == 3 and pg.battery_warning(d): it.setForeground(QColor(BUILD))
                self.table.setItem(i, j, it)
        self.note.setText('' if devs else NO_PHONE if adb else NO_ADB); self.note.setVisible(not devs)
        low = next((pg.battery_warning(d) for d in devs if d['serial'] == self.win.serial()), '')
        self.warn.setText(f'⚠ {low}'); self.warn.setVisible(bool(low))
        for b in self.adb_buttons: b.setEnabled(adb)
    def selected(self):
        r = self.table.currentRow()
        if r >= 0 and self.table.item(r, 0): self.win.set_serial(self.table.item(r, 0).data(Qt.UserRole))
    def show_phone(self, pkg, games, legacy):
        self.ph.setText(self.win.phone_name() or 'No phone'); self.games.clear()
        m = {k: v.strip() for k, v in re.findall(r'(versionName|lastUpdateTime)=(.+)', pkg)}   # dumpsys package's lines
        self.app.setText(f'The DroidPlay app {m.get("versionName", "")} · updated {m["lastUpdateTime"][:16]}' if 'lastUpdateTime' in m else
                         'The DroidPlay app is not installed' if self.win.serial() else 'The app and the games on the picked phone show here.')
        for gid, meta in games.items():
            it = QListWidgetItem(f'{meta.get("name") or gid}     {gid} · {meta.get("display") or meta.get("version") or "?"}'); it.setData(Qt.UserRole, gid); self.games.addItem(it)
        if legacy: self.games.addItem('A one-game install (files/game, before the launcher): Move the old one-game install puts it in the launcher, its save copied over')
    def pair(self): PairDialog(self.win).exec()
    def connect(self):
        addr, ok = QInputDialog.getText(self, 'Connect', 'Address and port from the Wireless debugging screen (e.g. 192.168.1.125:39973):', text=self.win.st.value('wifi', ''))
        if not ok or not addr: return
        self.win.st.setValue('wifi', addr.strip())
        def got(r):   # adb's words in plain ones: it exits 0 from a failed connect too
            ok2, text = wifi.connect_result(*r) if not isinstance(r, Exception) else (False, str(r))
            self.win.toast(f'Connected to {addr.strip()}' if ok2 else re.sub(r'<[^>]+>', '', text)); self.win.refresh_devices()
        bg(lambda: adb_run(None, 'connect', addr.strip(), timeout=30), got)
    def discover(self):   # every phone offering a connection on the network, to connect to: it offered the first one only
        names = {i: self.win.models.get(s) for s, i in self.win.ids.items() if self.win.models.get(s)}
        def got(r):
            found = wifi.discovered(r[0], r[1], names) if not isinstance(r, Exception) else []
            if not found: QMessageBox.information(self, 'Discover', 'No phone offers wireless debugging on this network. Pair it first (Pair over Wi-Fi…), keep its '
                                                  'Wireless debugging screen on, and check that ' + wifi.SAME_NETWORK + '.'); return
            d = QDialog(self); d.setWindowTitle('Discover'); d.setStyleSheet(STYLE + f'QDialog {{ background: {PANEL}; }}'); d.setMinimumWidth(480)
            lay = QVBoxLayout(d); lay.setContentsMargins(24, 20, 24, 18); lay.addWidget(label('Phones offering wireless debugging on this network', 'h2'))
            lst = QListWidget()
            for text, addr, on in found:
                it = QListWidgetItem(text + (' · connected' if on else '')); it.setData(Qt.UserRole, addr)
                it.setFlags(Qt.ItemIsUserCheckable | Qt.ItemIsEnabled if not on else Qt.NoItemFlags); it.setCheckState(Qt.Unchecked if on else Qt.Checked); lst.addItem(it)
            lay.addWidget(lst); box = QDialogButtonBox(QDialogButtonBox.Ok | QDialogButtonBox.Cancel); box.button(QDialogButtonBox.Ok).setText('Connect')
            box.accepted.connect(d.accept); box.rejected.connect(d.reject); lay.addWidget(box)
            if d.exec() != QDialog.Accepted: return
            want = [lst.item(i).data(Qt.UserRole) for i in range(lst.count()) if lst.item(i).checkState() == Qt.Checked]
            def connect_all(): return [(a, *wifi.connect_result(*adb_run(None, 'connect', a, timeout=20))) for a in want]
            def done(rs):
                bad = [f'{a}: {re.sub(r"<[^>]+>", "", t)}' for a, ok, t in rs if not ok] if not isinstance(rs, Exception) else [str(rs)]
                self.win.toast(f'Connected to {len(want) - len(bad)} of {len(want)}' + (f'. {bad[0]}' if bad else '')); self.win.refresh_devices()
            if want: bg(connect_all, done)
        bg(lambda: (adb_run(None, 'mdns', 'services', timeout=8)[1], adb_run(None, 'devices', timeout=10)[1]), got)
    def disconnect(self):   # the row picked here, else the phone picked at the top: a phone on USB and Wi-Fi has one entry at the top, its USB one
        r = self.table.currentRow(); s = self.table.item(r, 0).data(Qt.UserRole) if r >= 0 and self.table.item(r, 0) else self.win.serial()
        if not s: return
        how, s = wifi.disconnect_plan(s); name = self.win.models.get(s) or s
        if how == 'usb': self.win.toast(f'{name} is connected by its cable: unplug it to disconnect it'); return
        self.win.let_go.update(dict.fromkeys({self.win.phone_key(s), s}, 'leaving'))   # dropped on purpose: not connected again behind the player's back (track_lost)
        if self.win.lost and self.win.lost['id'] == self.win.phone_key(s): self.win.lost = None
        if how == 'wireless off':
            if QMessageBox.question(self, 'Disconnect', f'Turn off Wireless debugging on {name}? adb connected it by itself over the network and would connect it again '
                                    'while Wireless debugging is on. Turn it on again on the phone to reconnect.') != QMessageBox.Yes: return
            bg(lambda: adb_run(s, 'shell', 'settings put global adb_wifi_enabled 0', timeout=15),
               lambda _: (self.win.toast(f'Wireless debugging is off on {name}: turn it on again on the phone to reconnect'), self.win.refresh_devices())); return
        bg(lambda: adb_run(None, 'disconnect', s, timeout=15), lambda r: (self.win.toast(f'Disconnected {name}' if not isinstance(r, Exception) and r[0] else f'Could not disconnect {name}: {r[1] if not isinstance(r, Exception) else r}'),
                                                                           self.win.refresh_devices()))
    def tcpip(self):   # a phone on its cable, over Wi-Fi with no pairing (adb tcpip): Android 10 has no Wireless debugging, and a phone allowed over USB needs no pairing
        r = self.table.currentRow(); s = self.table.item(r, 0).data(Qt.UserRole) if r >= 0 and self.table.item(r, 0) else self.win.serial()
        name = self.win.models.get(s) or s
        if not s or phone_id.wifi(s): self.win.toast('Pick a phone connected by its cable: Use over Wi-Fi starts from there'); return
        box = QMessageBox(QMessageBox.Question, 'Use over Wi-Fi', f'<b>Use {esc(name)} over Wi-Fi?</b>', parent=self); box.setInformativeText(wifi.TCPIP_WARNING)
        box.setStyleSheet('QLabel#qt_msgbox_informativelabel { min-width: 460px; }')
        go = box.addButton('Use over Wi-Fi', QMessageBox.AcceptRole); box.addButton(QMessageBox.Cancel); box.setDefaultButton(go); box.exec()
        if box.clickedButton() is not go: return
        def run():   # its Wi-Fi address first: adb on the phone is out of reach over USB for a moment once it restarts
            ip = wifi.wifi_ip(adb_run(s, 'shell', wifi.IP_CMD, timeout=10)[1])
            if not ip: return False, f'{name} is not on Wi-Fi: connect it to the same network as this computer, then try again'
            ok, text = wifi.tcpip_result(*adb_run(s, 'tcpip', str(wifi.TCPIP_PORT), timeout=20))
            if not ok: return False, text
            addr = f'{ip}:{wifi.TCPIP_PORT}'
            for _ in range(6):   # adb on the phone takes a second or two to listen again
                time.sleep(2); ok, text = wifi.connect_result(*adb_run(None, 'connect', addr, timeout=10))
                if ok: return True, addr
            return False, text
        def got(r):
            ok, text = r if not isinstance(r, Exception) else (False, str(r))
            if ok: self.win.st.setValue('wifi', text)
            self.win.toast(f'{name} is connected over Wi-Fi at {text} (not encrypted): you can unplug its cable' if ok else re.sub(r'<[^>]+>', '', text)); self.win.refresh_devices()
        self.win.toast(f'Connecting to {name} over Wi-Fi…'); bg(run, got)
    def update_app(self):
        s = self.win.serial()
        if s: self.win.builds.start('Update the app', ['--apk-only', '--serial', s], after=self.win.refresh_phone)
    def migrate(self):
        s = self.win.serial()
        if not s: return
        lib = ba.library()
        items = [f'{m.get("name") or gid} ({gid})' for gid, (_, m) in lib.items()]
        if not items: QMessageBox.information(self, 'Move', 'Build the game first, so the app gets its host'); return
        pick, ok = QInputDialog.getItem(self, 'Move the old one-game install', 'The game it holds (its save is copied to that game; the old save stays as it was):', items, 0, False)
        if not ok: return
        gid = pick.rsplit('(', 1)[-1].rstrip(')')
        if QMessageBox.question(self, 'Move', 'First the save backups on the phone are copied to this computer, then the game folder moves and the app is updated; '
                                'the app copies the save when it next opens. Go on?') != QMessageBox.Yes: return
        self.backup(then=lambda: self.win.builds.start('Move the old install', ['--migrate', '--title', gid, '--serial', s],
                                                       after=lambda: self.win.builds.start('Update the app', ['--apk-only', '--serial', s], after=self.win.refresh_phone)))
    def backup(self, then=None):
        s = self.win.serial()
        if not s: return
        dst = ba.data_dir() / 'backups' / phone_id.safe(self.win.phone_key(s)) / time.strftime('%Y%m%d-%H%M%S')   # one folder a phone, whatever its Wi-Fi port
        def run():
            ok, out = adb_run(s, 'shell', f'ls {REMOTE}/backups')
            if not ok or not out.strip(): return None, 'no backups on the phone yet: open the app and use Back up saves in its game list'
            dst.mkdir(parents=True, exist_ok=True)
            return adb_run(s, 'pull', f'{REMOTE}/backups', str(dst), timeout=600)
        def got(r):
            if isinstance(r, Exception): QMessageBox.warning(self, 'Backup', str(r)); return
            ok, out = r
            if ok: self.win.toast(f'The phone\'s save backups are in {dst}')
            elif not then: QMessageBox.warning(self, 'Backup', out)
            if then and ok: then()
            elif then and QMessageBox.question(self, 'Backup', f'No backup was copied ({out}). Go on anyway? The old save stays on the phone either way.') == QMessageBox.Yes: then()
        bg(run, got)
    # A save sent to the phone's files/import/<title ID>/ (build_android_from_nsp.py --import-save / --restore-backup): DroidPlay on the phone shows what it found and asks,
    # keeps the save it has in its files/backups/, then swaps the new one in (dm/android/java LauncherSaves.incoming)
    def import_save(self, gid, name):
        s = self.win.serial()
        if not s: return
        b = QMessageBox(QMessageBox.Question, 'Import a save', f'A save for {name} on {self.win.phone_name()}: a save folder (an emulator\'s, a JKSV, EdiZon or Checkpoint backup, '
                        'a PC port\'s), or a zip of one.\n\nDroidPlay on the phone shows the save it found and asks before swapping it in, and keeps the save it has first.',
                        QMessageBox.Cancel, self)
        folder = b.addButton('A folder…', QMessageBox.AcceptRole); archive = b.addButton('A zip or other archive…', QMessageBox.AcceptRole); b.exec()
        if b.clickedButton() is folder: path = QFileDialog.getExistingDirectory(self, f'The save folder for {name}', str(Path.home()))
        elif b.clickedButton() is archive: path = QFileDialog.getOpenFileName(self, f'A save archive for {name}', str(Path.home()), 'Archives (*.zip *.rar *.7z *.tar *.tgz *.gz);;All files (*)')[0]
        else: return
        if path: self.win.builds.start(f'Import a save for {name}', ['--import-save', path, '--title', gid, '--serial', s], after=self.win.refresh_phone)
    def restore_all(self): self.restore_backup()   # a button's clicked(checked) would pass its bool as gid
    def restore_backup(self, gid=None, name=None):   # one game's save from a backup Back up saves pulled, or every game's in it
        s = self.win.serial()
        if not s: return
        mine = ba.data_dir() / 'backups' / phone_id.safe(self.win.phone_key(s))   # where backup() puts this phone's
        start = mine if mine.is_dir() else ba.data_dir() / 'backups'
        path = QFileDialog.getExistingDirectory(self, f'A backup holding {name}\'s save' if name else 'A save backup to put back (every game in it)', str(start if start.is_dir() else Path.home()))
        if path: self.win.builds.start('Restore a save backup', ['--restore-backup', path, *(['--title', gid] if gid else []), '--serial', s], after=self.win.refresh_phone)
    def remove_game(self, gid, name):
        s = self.win.serial()
        if not s or QMessageBox.question(self, 'Remove', f'Delete {name} from {self.win.phone_name()}? Its save stays in the app.') != QMessageBox.Yes: return
        bg(lambda: adb_run(s, 'shell', 'rm', '-rf', f'{REMOTE}/games/{gid}'), lambda _: self.win.refresh_phone())


# ---------------- Performance ----------------
class Chart(QWidget):   # frame time history (ms), with the 16.7 and 33.3 ms lines
    def __init__(self): super().__init__(); self.vals = []; self.setMinimumHeight(150)
    def add(self, v): self.vals = (self.vals + [v])[-120:]; self.update()
    def paintEvent(self, _):
        p = QPainter(self); p.setRenderHint(QPainter.Antialiasing); w, h = self.width(), self.height(); top = max([50.0] + self.vals) * 1.1
        path = QPainterPath(); path.addRoundedRect(QRectF(0.5, 0.5, w - 1, h - 1), 12, 12); p.fillPath(path, QColor(PANEL)); p.setPen(QPen(QColor(255, 255, 255, 20))); p.drawPath(path)
        f = QFont(); f.setFamilies([FONT, 'Segoe UI']); f.setPixelSize(11); p.setFont(f)
        for ms, c in ((16.7, READY), (33.3, BUILD)):
            y = h - ms / top * h; col = QColor(c); col.setAlpha(110); p.setPen(QPen(col, 1, Qt.DashLine)); p.drawLine(12, int(y), w - 12, int(y)); p.drawText(16, int(y) - 4, f'{ms} ms')
        if len(self.vals) > 1:
            p.setPen(QPen(QColor(TEXT), 2)); step = (w - 24) / (len(self.vals) - 1)
            for i in range(1, len(self.vals)): p.drawLine(int(12 + (i - 1) * step), int(h - self.vals[i - 1] / top * h), int(12 + i * step), int(h - self.vals[i] / top * h))
        else: p.setPen(QColor(TEXT3)); p.drawText(self.rect(), Qt.AlignCenter, 'Watch the log while a game runs: its frame time shows here every ~10 s')


class PerformancePage(QWidget):
    PATS = {'frame': re.compile(r'timing up to frame \d+: frame time p50 ([\d.]+) / p95 ([\d.]+)'), 'gpu': re.compile(r'GPU busy ([\d.]+) ms'),
            'cpu': re.compile(r'translation up to .*CPU ([\d.]+) ms, (\d+) draws'), 'speed': re.compile(r'audio clock rate ([\d.]+)'),
            'wait': re.compile(r'SyncWait blocked \d+ times: mean ([\d.]+) ms'), 'pass': re.compile(r'\[prof\]\s+(\d+) us  pass #\d+  (\d+)x(\d+)  colour \d+ \(vk\d+\) depth \d  draws (\d+)')}

    def __init__(self, win):
        super().__init__(); self.win = win; self.setObjectName('page'); self.proc = None; self.buf = ''; self.passes = []
        lay = QVBoxLayout(self); lay.setContentsMargins(48, 32, 48, 24); lay.setSpacing(16)
        head = QHBoxLayout(); head.addWidget(label('Performance', 'h1')); head.addStretch()
        self.b_run = QPushButton('Watch the log'); self.b_run.clicked.connect(self.toggle); head.addWidget(self.b_run)
        for text, fn in (('Profile a frame', self.profile), ('Thermal', self.thermal), ('Open the app', self.launch), ('Stop the app', self.stop_app), ('Save log.txt…', self.save)):
            b = QPushButton(text); b.setObjectName('ghost'); b.clicked.connect(fn); head.addWidget(b)
        lay.addLayout(head)
        tiles = QGridLayout(); tiles.setSpacing(12); self.tiles = {}
        for i, (k, t) in enumerate((('fps', 'FRAMES PER SECOND'), ('frame', 'FRAME TIME P50 / P95'), ('gpu', 'GPU BUSY PER FRAME'), ('cpu', 'REPLAY CPU PER FRAME'),
                                    ('speed', 'GAME SPEED'), ('wait', 'WAIT FOR THE GPU'))):
            f = QFrame(); f.setObjectName('tile'); fl = QVBoxLayout(f); fl.setContentsMargins(16, 14, 16, 14)
            v = label('—', 'tilevalue'); fl.addWidget(label(t, 'tilelabel')); fl.addWidget(v); self.tiles[k] = v; tiles.addWidget(f, 0, i)
        lay.addLayout(tiles); self.chart = Chart(); lay.addWidget(self.chart)
        split = QSplitter(Qt.Horizontal)
        left = QWidget(); ll = QVBoxLayout(left); ll.setContentsMargins(0, 0, 0, 0)
        self.filter = QLineEdit(); self.filter.setPlaceholderText('Filter the log (e.g. RED)'); ll.addWidget(self.filter)
        self.log = QPlainTextEdit(); self.log.setObjectName('log'); self.log.setReadOnly(True); self.log.setMaximumBlockCount(5000); ll.addWidget(self.log)
        split.addWidget(left)
        self.prof = QTableWidget(0, 3); self.prof.setHorizontalHeaderLabels(['GPU passes of a frame', 'ms', 'share']); self.prof.horizontalHeader().setSectionResizeMode(0, QHeaderView.Stretch)
        self.prof.verticalHeader().hide(); self.prof.setShowGrid(False); split.addWidget(self.prof); split.setSizes([700, 380])
        lay.addWidget(split, 1)
    def toggle(self):
        if self.proc and self.proc.state() != QProcess.NotRunning: self.wanted = False; self.proc.kill(); self.b_run.setText('Watch the log'); return
        s = self.win.serial()
        if not s: self.win.toast(self.win.no_phone()); return
        self.proc = QProcess(self); self.proc.setProgram('adb'); self.proc.setArguments(['-s', s, 'logcat', '-T', '200', '-s', 'DroidPlay', 'nxnative'])   # the app's tag; nxnative from an app older than 0.5.0
        self.proc.readyReadStandardOutput.connect(self.read); self.proc.finished.connect(self.ended); self.proc.start(); self.b_run.setText('Stop watching'); self.wanted = True
    def ended(self, *_):   # logcat ended without Stop watching: the phone dropped off (Wi-Fi). resume() starts it again when the phone is back
        if getattr(self, 'wanted', False): self.b_run.setText('Watch the log'); self.log.appendHtml(f'<span style="color:{BUILD}">— the phone dropped off: the log goes on when it is back</span>')
    def resume(self):
        if getattr(self, 'wanted', False) and not (self.proc and self.proc.state() != QProcess.NotRunning) and self.win.serial(): self.toggle()
    def read(self):
        if not self.proc: return
        self.buf += bytes(self.proc.readAllStandardOutput()).decode('utf-8', 'replace')
        *lines, self.buf = self.buf.split('\n'); flt = self.filter.text().strip()
        for line in lines:
            line = line.rstrip('\r'); self.parse(line)
            if not flt or flt.lower() in line.lower():
                self.log.appendHtml(f'<span style="color:{ERR}">{esc(line)}</span>' if '[RED]' in line or 'CRASH' in line else esc(line))
    def parse(self, line):
        P = self.PATS
        if m := P['frame'].search(line):
            p50, p95 = float(m[1]), float(m[2]); self.tiles['frame'].setText(f'{p50:.1f} / {p95:.1f}'); self.tiles['fps'].setText(f'{1000 / p50:.0f}' if p50 else '—'); self.chart.add(p50)
        if m := P['gpu'].search(line): self.tiles['gpu'].setText(f'{float(m[1]):.1f} ms')
        if m := P['cpu'].search(line): self.tiles['cpu'].setText(f'{float(m[1]):.1f} ms')
        if m := P['speed'].search(line): v = float(m[1]); self.tiles['speed'].setText(f'{v * 100:.0f}%'); self.tiles['speed'].setStyleSheet(f'color: {READY if v > 0.97 else BUILD}')
        if m := P['wait'].search(line): self.tiles['wait'].setText(f'{float(m[1]):.1f} ms')
        if m := P['pass'].search(line): self.passes.append((int(m[1]), int(m[2]), int(m[3]), int(m[4]))); QTimer.singleShot(500, self.show_passes)
    def show_passes(self):   # the profiled frame's passes, grouped: without draws (clears), full size, small (bloom, blur, shadows), the rest (3D at the render size)
        if not self.passes: return
        groups = {}
        for us, w, h, draws in self.passes:
            k = 'Without draws (clears)' if draws == 0 else 'Full size (720p / 1080p)' if w >= 1280 else 'Small (bloom, blur, shadows)' if w < 400 else '3D scene and the rest'
            groups[k] = groups.get(k, 0) + us
        total = sum(groups.values()) or 1; self.prof.setRowCount(len(groups) + 1)
        for i, (k, us) in enumerate(sorted(groups.items(), key=lambda x: -x[1])):
            for j, v in enumerate((k, f'{us / 1000:.1f}', f'{100 * us / total:.0f}%')): self.prof.setItem(i, j, QTableWidgetItem(v))
        for j, v in enumerate((f'{len(self.passes)} passes', f'{total / 1000:.1f}', '')): self.prof.setItem(len(groups), j, QTableWidgetItem(v))
        self.passes = []
    def profile(self):
        s = self.win.serial()
        if not s: return
        if not (self.proc and self.proc.state() != QProcess.NotRunning): self.toggle()
        self.passes = []; bg(lambda: adb_run(s, 'shell', f'touch {REMOTE}/prof_now'), lambda r: self.win.toast('A frame is profiled within ~1 s while a game runs'))
    def thermal(self):
        s = self.win.serial()
        if s: bg(lambda: adb_run(s, 'shell', 'dumpsys thermalservice | grep -E "Thermal Status|cpufreq|devfreq|Temperature\\{" | head -20; dumpsys battery | grep -E "temperature|powered"'),
                 lambda r: QMessageBox.information(self, 'Thermal', (r[1] if not isinstance(r, Exception) else str(r)) + '\n\nA cpufreq value above 0: the phone limits those cores because of heat.'))
    def launch(self):
        s = self.win.serial()
        if s: bg(lambda: adb_run(s, 'shell', 'am', 'start', '-n', f'{PKG}/.Launcher'), lambda _: None)
    def stop_app(self):
        s = self.win.serial()
        if s: bg(lambda: adb_run(s, 'shell', 'am', 'force-stop', PKG), lambda _: None)
    def save(self):
        s = self.win.serial()
        if not s: return
        f, _ = QFileDialog.getSaveFileName(self, 'Save the app log', 'log.txt', 'Text (*.txt)')
        if f: bg(lambda: adb_run(s, 'pull', f'{REMOTE}/log.txt', f, timeout=60), lambda r: self.win.toast(r[1] if not isinstance(r, Exception) else str(r)))


# ---------------- Settings ----------------
class SettingsPage(QWidget):
    def __init__(self, win):
        super().__init__(); self.win = win; self.setObjectName('page')
        outer = QVBoxLayout(self); outer.setContentsMargins(0, 0, 0, 0)
        scroll = QScrollArea(); scroll.setWidgetResizable(True); scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff); outer.addWidget(scroll)
        inner = QWidget(); inner.setObjectName('page'); scroll.setWidget(inner)   # a low window scrolls the page: the table is as tall as its rows, with no scroller of its own
        lay = QVBoxLayout(inner); lay.setContentsMargins(48, 32, 48, 24); lay.setSpacing(16)
        lay.addWidget(label('Settings', 'h1'))
        keys = QHBoxLayout(); keys.addWidget(label('Your prod.keys', 'meta')); self.keys = QLineEdit(win.st.value('keys', '')); self.keys.setPlaceholderText('the keys dumped from your Switch')
        self.keys.editingFinished.connect(lambda: self.set_keys(self.keys.text().strip())); keys.addWidget(self.keys, 1)
        b = QPushButton('Choose…'); b.clicked.connect(self.pick_keys); keys.addWidget(b); lay.addLayout(keys)
        upd = QHBoxLayout(); upd.addWidget(label('Updates', 'meta')); self.update_line = label('', 'muted'); upd.addWidget(self.update_line, 1)
        self.update_btn = QPushButton('Check for updates'); self.update_btn.setObjectName('ghost'); self.update_btn.clicked.connect(self.update_clicked); upd.addWidget(self.update_btn)
        lay.addLayout(upd)
        start = QHBoxLayout(); start.addWidget(label('Get started', 'meta')); self.start_line = label('', 'muted'); start.addWidget(self.start_line, 1)
        b = QPushButton('Show the setup checklist'); b.setObjectName('ghost'); b.clicked.connect(lambda: win.go('setup')); start.addWidget(b); lay.addLayout(start)
        if sys.platform.startswith('linux') and menu_entry.appimage():   # the AppImage, which has no menu entry until one is written (menu_entry.py)
            menu = QHBoxLayout(); menu.addWidget(label('Applications menu', 'meta')); self.menu_line = label('', 'muted', wrap=True); menu.addWidget(self.menu_line, 1)
            self.menu_add = QPushButton(); self.menu_add.setObjectName('ghost'); self.menu_add.clicked.connect(self.menu_write); menu.addWidget(self.menu_add)
            self.menu_del = QPushButton('Remove'); self.menu_del.setObjectName('ghost'); self.menu_del.clicked.connect(self.menu_remove); menu.addWidget(self.menu_del)
            lay.addLayout(menu); self.show_menu()
        lay.addSpacing(8)
        head = QHBoxLayout(); head.addWidget(label('Requirements', 'h2')); head.addStretch(); b = QPushButton('Check again'); b.setObjectName('ghost'); b.clicked.connect(lambda *_: self.run()); head.addWidget(b)
        self.setup = QPushButton("Install what's missing"); self.setup.clicked.connect(self.install); self.setup.hide(); head.addWidget(self.setup); self.rows = []
        self.checked = False   # --check has answered once: until then the checklist says it is looking, not that tools are missing
        lay.addLayout(head)
        self.summary = label('Checking what the build needs…', 'muted', wrap=True); lay.addWidget(self.summary)
        self.table = QTableWidget(0, 3); self.table.setHorizontalHeaderLabels(['', 'What', 'Detail / how to get it']); self.table.verticalHeader().hide(); self.table.setShowGrid(False)
        self.table.horizontalHeader().setSectionResizeMode(2, QHeaderView.Stretch); self.table.setColumnWidth(0, 36); self.table.setColumnWidth(1, 220); self.table.setWordWrap(True)
        self.table.horizontalHeader().setDefaultAlignment(Qt.AlignLeft | Qt.AlignVCenter); self.table.setVerticalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
        self.table.setSelectionMode(QTableWidget.NoSelection); self.table.setFocusPolicy(Qt.NoFocus)   # nothing to pick here, and a selected row lost its dot's colour
        self.table.horizontalHeader().sectionResized.connect(lambda i, old, new: i == 2 and old != new and QTimer.singleShot(0, self.fit))
        lay.addWidget(self.table); lay.addStretch(1)
    def show_update(self, newer):   # the version, when DroidPlay last looked, and a newer release if there is one
        st, v = self.win.st, self.win.version; checked = st.value('update/checked', '')
        when = f' Last checked {checked}.' if checked else ''
        rel = self.win.release; can, why = upd.how(rel, REPO) if newer and rel else (None, None)
        if newer:   # with why Update can't, where there's a reason to give (the AppImage's folder can't be written, a test build)
            self.update_line.setText(f'DroidPlay {v} · {newer.lstrip("v")} is out.{when}' + (f' {why[0].upper() + why[1:]}: Download opens the release page.' if why else ''))
            self.update_btn.setText(f'{"Update to" if can else "Download"} {newer.lstrip("v")}')
        elif upd.release_number(v): self.update_line.setText(f'DroidPlay {v}.{when}'); self.update_btn.setText('Check for updates')
        else:   # a checkout's git describe, or an AppImage built by CI from one (linux-package.yml), which a release doesn't replace
            what = 'a test build' if menu_entry.appimage() else 'a copy from source'
            self.update_line.setText(f'DroidPlay {v or "(its version unknown)"}: {what}, so it doesn\'t look for releases by itself.'); self.update_btn.setText('Check for updates')
        self.newer = newer
    def show_menu(self):
        st, cur = menu_entry.state(), menu_entry.current()
        self.menu_line.setText({'none': 'DroidPlay is not in your applications menu.', 'this': 'DroidPlay is in your applications menu.',
                                'other': f'The menu entry starts another copy, {cur[1] if cur and cur[1] else "a file that is gone"}.'}[st])
        self.menu_add.setText({'none': 'Add to the applications menu', 'this': 'Write it again', 'other': 'Start this copy'}[st]); self.menu_del.setVisible(st != 'none')
    def menu_write(self):
        try: menu_entry.add(menu_entry.appimage(), lambda p: app_icon().pixmap(256, 256).save(str(p), 'PNG'))
        except (OSError, ValueError, TypeError) as e: QMessageBox.warning(self, 'Applications menu', f'DroidPlay could not add itself to the menu: {e}'); return
        self.show_menu(); self.win.toast('DroidPlay is in your applications menu (a few seconds on some desktops)')
    def menu_remove(self):
        try: menu_entry.remove()
        except OSError as e: QMessageBox.warning(self, 'Applications menu', f'DroidPlay could not remove its menu entry: {e}'); return
        self.show_menu(); self.win.toast('DroidPlay is out of your applications menu')
    def update_clicked(self):
        if getattr(self, 'newer', None) and self.win.release: self.win.update_now()   # Update, or the release's page where it can't
        elif getattr(self, 'newer', None): QDesktopServices.openUrl(QUrl(self.win.st.value('update/url', '') or 'https://github.com/enjoyerofslop/droidplay-core/releases/latest'))
        else: self.win.check_updates(asked=True)
    def fit(self):   # the rows' heights at the table's width now: rows sized while the page was hidden wrapped at that narrower width and kept two lines
        t = self.table; t.resizeRowsToContents()
        t.setFixedHeight(t.horizontalHeader().height() + sum(t.rowHeight(i) for i in range(t.rowCount())) + 2 * t.frameWidth())
    def showEvent(self, e): super().showEvent(e); QTimer.singleShot(0, self.fit)
    def pick_keys(self):
        f, _ = QFileDialog.getOpenFileName(self, 'Your prod.keys', '', 'Keys (*.keys);;All files (*)')
        if f: self.keys.setText(f); self.set_keys(f)
    def set_keys(self, f):   # the library reads NSP and XCI games with the keys: read it again when they change
        if f == (self.win.st.value('keys', '') or ''): return
        self.win.st.setValue('keys', f); self.win.library.reload(); self.win.setup_changed()
    def run(self, then=None):
        def got(rows):
            failed = isinstance(rows, Exception)
            if failed: rows = [{'name': 'check', 'ok': False, 'detail': str(rows), 'fix': ''}]
            self.rows = rows; fixes = set(self.installable())
            # what is missing first; the Python modules, which come with the app, in one row while they are all there. The fix of what Install what's missing
            # gets is that button, above, rather than the console's --setup
            missing = [dict(r, fix="Install what's missing gets it") if r['name'] in fixes else r for r in rows if not r['ok']]
            found = [r for r in rows if r['ok'] and not r['name'].startswith('Python')]
            py = [r for r in rows if r['ok'] and r['name'].startswith('Python')]
            if py: found.insert(0, {'name': 'Python modules', 'ok': True, 'detail': ', '.join(r['name'].split()[-1] for r in py), 'fix': '', 'tip': py[0]['detail']})
            shown = missing + found; self.table.setRowCount(len(shown))
            for i, r in enumerate(shown):
                mark = QTableWidgetItem('●'); mark.setForeground(QColor(READY if r['ok'] else ERR)); mark.setTextAlignment(Qt.AlignCenter); self.table.setItem(i, 0, mark)
                detail = QTableWidgetItem(r['detail'] + (f'\n→ {r["fix"]}' if r['fix'] else '')); detail.setToolTip(r.get('tip') or '')
                self.table.setItem(i, 1, QTableWidgetItem(r['name'])); self.table.setItem(i, 2, detail)
            n, it = len(missing), 'it' if len(missing) == 1 else 'them'
            self.summary.setText('Could not check what the build needs:' if failed else 'Everything the build needs is on this computer.' if not n else
                                 f"{n} missing: Install what's missing gets {it}." if len(fixes) == n else f'{n} missing: the rows below say how to get {it}.')
            self.fit(); self.setup.setVisible(bool(fixes)); self.checked = True
            if then: then(rows)
            self.win.setup_changed()
        bg(lambda: self.win.script_json('--check'), got)
    def installable(self):   # the failing rows --setup installs: on Windows and x86_64 Linux (host_setup.unsupported), and not the Python modules (uv run brings those, or pip)
        return [r['name'] for r in self.rows if not r['ok'] and r['name'] != 'check' and not r['name'].startswith('Python')] if CAN_INSTALL else []
    def install(self, show_builds=True):   # build_android_from_nsp.py --setup, once the player has seen what it downloads and, for the Android SDK's parts, accepted Google's licence
        missing = self.installable()
        if not missing: return
        sdk_rows = set(ba.sdk_packages(Path('.'))) | {'Android SDK'}
        parts = (['JDK 21, Microsoft\'s build (about 190 MB)'] if any(n.startswith('JDK') for n in missing) else []) + \
                (['.NET SDK 10 (about 290 MB)'] if '.NET SDK 10' in missing else []) + \
                (['the Android SDK: its command-line tools, platform-tools, platform, build-tools, NDK and CMake (about 1.1 GB to download, 4 GB installed)']
                 if 'Android SDK' in missing else   # with no SDK found, --check lists none of its parts but adb
                 [f'the Android SDK\'s {", ".join(n for n in missing if n in sdk_rows)}'] if any(n in sdk_rows for n in missing) else [])
        sdk = any(n in sdk_rows for n in missing)
        try: where = f'the Android SDK into the one it found, {ba.sdk_dir(None)}'
        except ba.BuildError: where = f'the Android SDK into {ba.default_sdk()}' + (', where Android Studio keeps it' if os.name == 'nt' else '')
        b = QMessageBox(QMessageBox.Question, "Install what's missing", 'DroidPlay downloads and installs:\n\n• ' + '\n• '.join(parts) +
                        f'\n\nThe JDK and the .NET SDK go into {ba.data_dir() / "tools"}, for this app alone' + (f'; {where}' if sdk else '') + '. All of it goes into your own folders: '
                        'nothing asks for administrator rights.', QMessageBox.Cancel, self)
        if sdk: b.setInformativeText(f'The Android SDK comes from Google: installing its parts means accepting the <a style="color:{READY}" '
                                     'href="https://developer.android.com/studio/terms">Android SDK License</a>. Nothing is sent to Google about you: '
                                     'the SDK\'s usage metrics stay off.')
        go = b.addButton('Accept and install' if sdk else 'Install', QMessageBox.AcceptRole); b.setDefaultButton(QMessageBox.Cancel); b.exec()
        if b.clickedButton() is not go: return
        self.win.builds.start(fr.SETUP_TITLE, ['--setup'] + (['--accept-android-licenses'] if sdk else []),
                              after=lambda: (ba.tool_paths(), self.run(), self.win.refresh_devices(), self.win.library.reload()))   # this app's PATH takes the new tools too (adb and dotnet), and the games are read again with them
        if show_builds: self.win.go('builds')   # from the checklist, its card shows the one bar and the player stays on the next step


# ---------------- Get started ----------------
class StepCard(QFrame):   # one of the checklist's steps: its number (a ✓ once done), what it is and what to do, a bar while it works, its one button
    COLOURS = {fr.TODO: TEXT3, fr.WORKING: BUILD, fr.WAITING: BUILD, fr.DONE: READY, fr.PROBLEM: ERR}

    def __init__(self, n):
        super().__init__(); self.n = n; self.setObjectName('card')
        lay = QHBoxLayout(self); lay.setContentsMargins(22, 18, 22, 18); lay.setSpacing(18)
        self.mark = QLabel(); self.mark.setFixedSize(32, 32); self.mark.setAlignment(Qt.AlignCenter); lay.addWidget(self.mark, 0, Qt.AlignTop)
        col = QVBoxLayout(); col.setSpacing(6)
        self.title = label(''); col.addWidget(self.title)
        self.text = label('', 'meta', wrap=True); self.text.setTextFormat(Qt.RichText); col.addWidget(self.text)
        self.bar = QProgressBar(); self.bar.setTextVisible(False); self.bar.hide(); col.addSpacing(2); col.addWidget(self.bar)
        lay.addLayout(col, 1)
        self.button = QPushButton(); self.button.setCursor(Qt.PointingHandCursor); self.button.setMinimumWidth(170); lay.addWidget(self.button, 0, Qt.AlignVCenter)

    def show_step(self, s, first):   # first: the first step not done, whose button is the one to press now
        c = self.COLOURS[s.state]; done = s.state == fr.DONE
        self.mark.setText('✓' if done else '!' if s.state == fr.PROBLEM else str(self.n))
        self.mark.setStyleSheet(f'border: 2px solid {c}; border-radius: 16px; font-size: 15px; font-weight: 800; ' + (f'background: {c}; color: {INK}' if done else f'color: {c}'))
        self.title.setText(s.title); self.title.setStyleSheet(f'font-size: 17px; font-weight: 700; color: {TEXT2 if done else TEXT}')
        self.text.setText(s.text)
        self.bar.setVisible(s.fraction is not None); tint(self.bar, BUILD)
        if s.fraction is not None:
            if s.fraction < 0: self.bar.setRange(0, 0)
            else: self.bar.setRange(0, 1000); self.bar.setValue(int(s.fraction * 1000))
        self.button.setText(s.button); self.button.setVisible(bool(s.button))
        name = 'next' if first and not done else 'ghost' if done else ''
        if self.button.objectName() != name: self.button.setObjectName(name); self.button.style().unpolish(self.button); self.button.style().polish(self.button)
        self.setStyleSheet(f'QFrame#card {{ border: 1px solid {"rgba(253,243,220,0.35)" if first and not done else LINE}; }}')


class SetupPage(QWidget):   # Get started: keys, tools, phone, first game. Not in the nav row: a first launch with a step left, the empty Library and the top bar's pill open it
    def __init__(self, win):
        super().__init__(); self.win = win; self.setObjectName('page'); self.run = None; self.failed = None; self.steps = []
        outer = QVBoxLayout(self); outer.setContentsMargins(0, 0, 0, 0)
        scroll = QScrollArea(); scroll.setWidgetResizable(True); scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff); outer.addWidget(scroll)
        inner = QWidget(); inner.setObjectName('page'); scroll.setWidget(inner)
        lay = QVBoxLayout(inner); lay.setContentsMargins(48, 32, 48, 24); lay.setSpacing(14)
        lay.addWidget(label('Get started', 'h1'))
        intro = label(fr.INTRO, 'meta', wrap=True); intro.setTextFormat(Qt.RichText); intro.setMaximumWidth(860); lay.addWidget(intro); lay.addSpacing(8)
        self.cards = {}
        for i, key in enumerate(('keys', 'tools', 'phone', 'game'), 1):
            c = StepCard(i); c.setMaximumWidth(1000); c.button.clicked.connect(lambda _=False, k=key: self.act(k)); lay.addWidget(c); self.cards[key] = c
        self.foot = label('', 'muted', wrap=True); lay.addSpacing(4); lay.addWidget(self.foot); lay.addStretch(1)
        win.builds.progress.connect(self.build_progress); win.builds.event.connect(self.build_event)

    def build_progress(self, title, _detail, frac, running):   # --setup, started here, in Settings or from the console: the tools card follows it
        if title != fr.SETUP_TITLE: return
        if running and self.run is None: self.run = fr.SetupProgress(time.time()); self.failed = None
        elif not running:
            self.run = None; b = self.win.builds
            self.failed = None if frac >= 0 else (b.last_red or b.last_line or 'see the Builds page for what happened')
        self.refresh()
    def build_event(self, title, ev):
        if title == fr.SETUP_TITLE and self.run: self.run.feed(ev); self.refresh()

    def compute(self):
        w = self.win; lib = w.library
        games = [{'folder': g.folder, 'name': g.name, 'state': g.status(w.phone_games, w.phone_name())[2], 'tested': g.tested, 'error': g.error} for g in lib.games.values()]
        run = ('running', self.run.fraction(), self.run.text(time.time())) if self.run else ('failed', self.failed) if self.failed else None
        return [fr.keys_step(w.st.value('keys', '') or ''), fr.tools_step(w.settings.rows if w.settings.checked else None, CAN_INSTALL, run),
                fr.phone_step(w.devs, w.adb, w.serial(), bool(w.st.value('serial', ''))), fr.game_step(lib.folders(), games, getattr(lib, 'reading', False))]
    def refresh(self):   # → how many steps are done
        if not hasattr(self.win, 'library'): return 0
        self.steps = self.compute(); first = next((s.key for s in self.steps if not s.done), None)
        for s in self.steps: self.cards[s.key].show_step(s, s.key == first)
        n = fr.ready_count(self.steps)
        self.foot.setText('All set: your game is in the Library.' if n == 4 else 'Settings › Show the setup checklist brings you back here.')
        return n
    def step(self, key): return next(s for s in self.steps if s.key == key)
    def act(self, key):
        s = next((s for s in self.steps if s.key == key), None)
        if not s: return
        w = self.win
        if key == 'keys': w.settings.pick_keys()
        elif key == 'tools':
            if s.button == 'Look again': w.settings.run()
            elif s.button == 'Open Settings': w.go('settings')
            else: w.settings.install(show_builds=False)
        elif key == 'phone': w.phone_guide()
        elif key == 'game':
            g = w.library.games.get(s.ref)
            if not g: w.add_game(); return
            w.library.pick(g); w.go('library'); w.library.play()


# ---------------- Updates ----------------
# The app says a newer release is out, as DroidPlay for Mac does; a copy DroidPlay-Setup installed and DroidPlay for Linux's AppImage also offer Update
# (dm/android/desktop/update.py has why and how).
class Progress(QObject): moved = Signal(object, object)   # a download's bytes so far and in all, from the thread pool to the window


def uv_install():   # DroidPlay as Install DroidPlay.cmd installed it before 0.5.0: uv runs it from %LOCALAPPDATA%\DroidPlay\droidnative
    la = os.environ.get('LOCALAPPDATA')
    return os.name == 'nt' and bool(la) and REPO == Path(la, 'DroidPlay', 'droidnative').resolve()


class UpdateBar(QWidget):   # under the top bar, on every page, while a newer release is out and the player hasn't put it off; or, once, the installer
    def __init__(self, win):
        super().__init__(); self.win = win; self.tag = self.url = None; self.setObjectName('updatebar'); self.setAttribute(Qt.WA_StyledBackground, True)
        self.setStyleSheet('QWidget#updatebar { background: rgba(61,220,132,0.10); border-bottom: 1px solid rgba(61,220,132,0.25); }')
        lay = QHBoxLayout(self); lay.setContentsMargins(28, 10, 28, 10); lay.setSpacing(14)
        self.text = label(''); self.text.setStyleSheet('font-size: 14px; font-weight: 600'); lay.addWidget(self.text); lay.addStretch()
        self.can_update = False
        self.get = QPushButton('Download'); self.get.clicked.connect(self.go); lay.addWidget(self.get)
        self.notes = QPushButton("What's new"); self.notes.setObjectName('ghost'); self.notes.clicked.connect(lambda: self.url and QDesktopServices.openUrl(QUrl(self.url)))
        lay.addWidget(self.notes); self.notes.hide()
        self.later_btn = QPushButton('Not now'); self.later_btn.setObjectName('ghost'); self.later_btn.clicked.connect(self.later); lay.addWidget(self.later_btn)
        self.hide()
    def go(self):
        if self.can_update: self.win.update_now()
        elif self.url: QDesktopServices.openUrl(QUrl(self.url))
    def show_release(self, rel):   # Update where Setup installed this copy or it's the AppImage, and the release has that file; the release's page otherwise
        self.tag, self.url = rel['tag'], rel['url']; v = self.tag.lstrip('v')
        kind, why = upd.how(rel, REPO); self.can_update = bool(kind)
        self.get.setEnabled(True); self.later_btn.setText('Not now'); self.notes.setVisible(self.can_update)
        if self.can_update:
            self.get.setText('Update')
            self.text.setText(f'DroidPlay {v} is out. Update downloads it, then DroidPlay closes and opens again, updated; your games, builds and settings stay.')
        else:
            self.get.setText('Download'); what = 'its installer' if os.name == 'nt' else 'its AppImage' if menu_entry.appimage() else 'it'
            self.text.setText(f'DroidPlay {v} is out. Download {what} from the release page' + (f' ({why})' if why else '') + '; your games, builds and settings stay.')
        self.show()
    def show_progress(self, done, total):
        self.get.setEnabled(False); self.notes.hide(); self.later_btn.setText('Cancel')
        self.text.setText(f'Downloading DroidPlay {(self.tag or "").lstrip("v")}: ' + (count_text(done, total, 'B') if total else gb_text(done)))
    def show_switch(self):   # once, in the copy Install DroidPlay.cmd installed: Setup replaces it (droidplay.iss removes the old folder and shortcuts)
        self.tag, self.url, self.can_update = None, 'https://github.com/enjoyerofslop/droidplay-core/releases/latest', False
        self.get.setText('Get the installer'); self.notes.hide()
        self.text.setText('DroidPlay has an installer now: it installs like any Windows app, with an entry in Settings › Apps, and replaces this copy. '
                          'Your games, builds and settings stay.'); self.show()
    def later(self):   # a release until the one after it; the installer for good; a download, cancelled
        if self.win.updating: self.win.updating['cancel'] = True; return
        self.win.st.setValue('update/later' if self.tag else 'switch/later', self.tag or '1'); self.hide()


# ---------------- The window ----------------
class Window(QMainWindow):
    def __init__(self):
        super().__init__(); self.setWindowTitle('DroidPlay'); self.setWindowIcon(app_icon()); self.setMinimumSize(1000, 620)
        a = QApplication.primaryScreen().availableGeometry(); self.resize(min(1440, a.width() - 40), min(900, a.height() - 40))
        self.st = QSettings('nxnative', 'android'); self.devs = None; self.adb = True; self.phone_games = {}; self.phone_legacy = False; self.models = {}   # the old name: the settings stay
        central = QWidget(); central.setObjectName('page'); root = QVBoxLayout(central); root.setContentsMargins(0, 0, 0, 0); root.setSpacing(0); self.setCentralWidget(central)
        # the top bar: the wordmark, the pages, the phone, search, + Build game
        top = QWidget(); top.setObjectName('topbar'); top.setAttribute(Qt.WA_StyledBackground, True); tl = QHBoxLayout(top); tl.setContentsMargins(28, 14, 28, 14); tl.setSpacing(6)
        tl.addWidget(label('DroidPlay', 'wordmark'))
        self.navs = {}; group = self.navgroup = QButtonGroup(self); group.setExclusive(True)
        for key, text, ic in (('library', 'Library', 'library'), ('builds', 'Builds', 'builds'), ('phones', 'Phones', 'phone'), ('perf', 'Performance', 'perf'), ('settings', 'Settings', 'settings')):
            b = QToolButton(); b.setObjectName('nav'); b.setText(f' {text}'); b.setIcon(icon(ic, TEXT2, 18)); b.setIconSize(QSize(18, 18)); b.setToolButtonStyle(Qt.ToolButtonTextBesideIcon)
            b.setCheckable(True); b.setCursor(Qt.PointingHandCursor); b.clicked.connect(lambda _=False, k=key: self.go(k)); group.addButton(b); tl.addWidget(b); self.navs[key] = b
        tl.addStretch()
        self.pill = QPushButton(); self.pill.setObjectName('pill'); self.pill.setCursor(Qt.PointingHandCursor); self.pill.setToolTip('The steps left before your first game')
        self.pill.clicked.connect(lambda: self.go('setup')); self.pill.hide(); tl.addWidget(self.pill); tl.addSpacing(10)
        self.phone = QComboBox(); self.phone.setFixedWidth(250); self.phone.setSizeAdjustPolicy(QComboBox.AdjustToMinimumContentsLengthWithIcon); self.phone.setMinimumContentsLength(18); self.phone.setToolTip('The phone builds install to'); self.phone.currentIndexChanged.connect(self.phone_changed)
        self.phone.view().setMinimumWidth(320)   # the open list may be wider than the box: a phone's name and Android version uncut
        tl.addWidget(self.phone); tl.addSpacing(8)
        self.search = QLineEdit(); self.search.setPlaceholderText('Search your games'); self.search.setFixedWidth(220); self.search.hide()
        self.search.textChanged.connect(self.filter_games)
        sb = QToolButton(); sb.setObjectName('nav'); sb.setIcon(icon('search', TEXT, 20)); sb.setIconSize(QSize(20, 20)); sb.setCursor(Qt.PointingHandCursor); sb.clicked.connect(self.toggle_search)
        tl.addWidget(self.search); tl.addWidget(sb); tl.addSpacing(8)
        add = QPushButton('  Build game'); add.setObjectName('ghost'); add.setIcon(icon('plus', TEXT2, 16)); add.setCursor(Qt.PointingHandCursor); add.clicked.connect(self.add_game); tl.addWidget(add)
        root.addWidget(top)
        sep = QFrame(); sep.setFixedHeight(1); sep.setStyleSheet(f'background: {LINE}'); root.addWidget(sep)
        self.version = ba.core_version()   # the VERSION file a release carries (its installer's copy too), git describe in a checkout
        self.release = None; self.updating = None   # the newest release as GitHub last said; a running update's download (its cancel flag)
        self.updatebar = UpdateBar(self); root.addWidget(self.updatebar)
        self.pages = QStackedWidget(); root.addWidget(self.pages, 1)
        self.builds = BuildsPage(self); self.phones = PhonesPage(self); self.perf = PerformancePage(self); self.settings = SettingsPage(self); self.setup = SetupPage(self)
        self.library = LibraryPage(self)
        self.page_of = {'library': self.library, 'builds': self.builds, 'phones': self.phones, 'perf': self.perf, 'settings': self.settings, 'setup': self.setup}
        for p in self.page_of.values(): self.pages.addWidget(p)
        self.buildbar = BuildBar(self); root.addWidget(self.buildbar); self.builds.progress.connect(self.buildbar.update_from)
        self.toastl = label('', wrap=True); self.toastl.setParent(central); self.toastl.setStyleSheet(f'background: {LIFT}; border: 1px solid {LINE}; border-radius: 10px; padding: 10px 16px; font-size: 13px')
        self.toastl.hide(); self.toast_t = QTimer(self); self.toast_t.setSingleShot(True); self.toast_t.timeout.connect(self.toastl.hide)
        self.go('library')
        # a first launch, or one with a step left, lands on the checklist rather than the Library: at once for what is known now (keys, a game, a phone seen
        # before), and for the tools once --check answers, unless the player has gone elsewhere meanwhile
        self.setup_changed()
        if self.pages.currentWidget() is not self.setup and (not self.setup.step('keys').done or self.library.no_games() or
                                                             not self.setup.step('phone').done and not self.st.value('serial', '')): self.go('setup')
        self.ids = {}   # serial → the phone's ID (phone_id.py), asked once per serial: its settings, backups and first-start notice are kept under it
        self.seen = {}   # every serial → ID this session, kept after a serial goes: Install again finds the phone a build was for on the serial it has now
        self.lost, self.reconnecting, self.reconnect_at = None, False, 0.0   # the picked phone, gone off Wi-Fi: looked for by its ID (track_lost, reconnect)
        self.let_go = {}   # ID → 'leaving' or 'gone': phones the player disconnected (Disconnect), not looked for until they have gone and come back
        for k in self.st.allKeys():   # first-start notices kept under a Wi-Fi address, before IDs: the address is gone with its port, and the notice with it
            if k.startswith('firststart/') and k.count('/') == 2 and phone_id.wifi(k.split('/')[1]): self.st.remove(k)
        self.timer = QTimer(self); self.timer.timeout.connect(self.refresh_devices); self.timer.start(5000); self.refresh_devices()
        self.settings.run(then=self.after_check)
        if os.name == 'nt': self.dark_title()
        self.settings.show_update(None)
        if uv_install() and not self.st.value('switch/later', ''): self.updatebar.show_switch()
        QTimer.singleShot(5000, self.check_updates); self.update_t = QTimer(self); self.update_t.timeout.connect(self.check_updates); self.update_t.start(24 * 3600 * 1000)

    def check_updates(self, asked=False):   # at launch and once a day; Settings' Check for updates asks too. Quiet when it can't reach GitHub, unless asked
        if (not upd.release_number(self.version) and not asked) or self.updating: return
        def got(r):
            if isinstance(r, Exception):
                if asked: self.toast(f'Could not reach GitHub to look for a newer DroidPlay: {r}')
                return
            tag, url = r['tag'], r['url']; self.st.setValue('update/checked', time.strftime('%Y-%m-%d %H:%M')); self.st.setValue('update/latest', tag); self.st.setValue('update/url', url)
            mine, theirs = upd.release_number(self.version), upd.release_number(tag)
            newer = bool(mine and theirs and theirs > mine); self.release = r if newer else None
            if newer and (asked or self.st.value('update/later', '') != tag): self.updatebar.show_release(r)
            elif not newer: self.updatebar.hide()
            self.settings.show_update(tag if newer else None)
            if asked and not newer: self.toast(f'DroidPlay {self.version} is the newest' if mine else f'The newest release is {tag}; this copy is {self.version or "of no release"}')
        bg(upd.latest_release, got)

    def update_now(self):   # Update: the release's Setup downloaded and checked, then run silently; DroidPlay closes, and Setup's /RELAUNCH opens the new one
        rel = self.release
        if self.updating or not rel: return
        kind, _ = upd.how(rel, REPO)
        if not kind: QDesktopServices.openUrl(QUrl(rel['url'])); return
        if self.builds.busy() or self.builds.ext: self.toast('A build is running: wait for it to finish, or stop it, then update'); return
        if kind == 'appimage': self.update_appimage(rel); return
        v, mb = rel['tag'].lstrip('v'), rel['setup']['size'] / 2**20
        if QMessageBox.question(self, 'Update DroidPlay', f'Update to DroidPlay {v}?\n\nDroidPlay downloads its installer ({mb:.0f} MB) and checks it against the release, '
                                'then closes, updates, and opens again. Your games, builds and settings stay.') != QMessageBox.Yes: return
        self.updating = flag = {'cancel': False}; sig = Progress(); sig.moved.connect(self.updatebar.show_progress); self.updatebar.show_progress(0, rel['setup']['size'])
        def fetch(): return upd.download_setup(rel['setup'], ba.data_dir() / 'updates', sig.moved.emit, lambda: flag['cancel'])
        def got(r):
            self.updating = None; sig.deleteLater()
            if isinstance(r, upd.Cancelled): self.updatebar.show_release(rel); return
            if isinstance(r, Exception): self.updatebar.show_release(rel); QMessageBox.warning(self, 'Update DroidPlay', f'The update could not be downloaded: {r}'); return
            if self.builds.busy() or self.builds.ext:   # one started while it downloaded: the installer waits, checked, for the next Update
                self.updatebar.show_release(rel); self.toast('A build started: Update again once it has finished'); return
            started = QProcess.startDetached(str(r), ['/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/RELAUNCH'])
            if not (started[0] if isinstance(started, tuple) else started):
                self.updatebar.show_release(rel); QMessageBox.warning(self, 'Update DroidPlay', f'Windows did not start {r}. Run it yourself, or download it from the release page.'); return
            QApplication.quit()   # Setup would close it anyway (the Restart Manager); it opens the new DroidPlay when it is done
        bg(fetch, got)

    def update_appimage(self, rel):   # Linux: the release's AppImage downloaded beside this one and checked, put in its place and started; this DroidPlay closes
        img, a, v = upd.appimage_install(REPO)[0], rel['appimage'], rel['tag'].lstrip('v')
        name = a['name'] if upd.APPIMAGE_NAME.fullmatch(img.name) else img.name   # replace_appimage: a release's name follows the version, the player's stays
        if QMessageBox.question(self, 'Update DroidPlay', f'Update to DroidPlay {v}?\n\nDroidPlay downloads its AppImage ({a["size"] / 2**20:.0f} MB) into {img.parent}, '
                                f'checks it against the release and puts it in place of this one as {name}, then closes and opens again; the first start of {v} '
                                'prepares it, once. Your games, builds and settings stay.') != QMessageBox.Yes: return
        self.updating = flag = {'cancel': False}; sig = Progress(); sig.moved.connect(self.updatebar.show_progress); self.updatebar.show_progress(0, a['size'])
        def fetch(): return upd.download_appimage(a, img, REPO, sig.moved.emit, lambda: flag['cancel'])
        def got(r):
            self.updating = None; sig.deleteLater()
            if isinstance(r, upd.Cancelled): self.updatebar.show_release(rel); return
            if isinstance(r, Exception):
                self.updatebar.show_release(rel)
                QMessageBox.warning(self, 'Update DroidPlay', f'The update could not be downloaded: {getattr(r, "strerror", None) or r}\n\n{img.name} is as it was.'); return
            if self.builds.busy() or self.builds.ext:   # one started while it downloaded: nothing replaced, and closing would end it
                Path(r).unlink(missing_ok=True); self.updatebar.show_release(rel); self.toast('A build started: Update again once it has finished'); return
            try: new = upd.replace_appimage(r, img, menu_entry.retarget)
            except OSError as e:
                Path(r).unlink(missing_ok=True); self.updatebar.show_release(rel)
                QMessageBox.warning(self, 'Update DroidPlay', f'The new AppImage could not be put in place of {img}: {e.strerror or e}. {img.name} is as it was.'); return
            if upd.relaunch(new) is None:
                self.updatebar.hide(); QMessageBox.warning(self, 'Update DroidPlay', f'DroidPlay {v} is in place, {new}, but did not start: close DroidPlay and open it.'); return
            QApplication.quit()   # the new AppImage, started on its own, prepares its version (the window it shows) and opens
        bg(fetch, got)

    def dark_title(self):   # Windows' own title bar in its dark mode, to match the room
        try:
            import ctypes; v = ctypes.c_int(1)
            ctypes.windll.dwmapi.DwmSetWindowAttribute(int(self.winId()), 20, ctypes.byref(v), ctypes.sizeof(v))  # pyright: ignore[reportAttributeAccessIssue]   (Windows only) DWMWA_USE_IMMERSIVE_DARK_MODE
        except Exception: pass
    def go(self, key):
        page = self.page_of.get(key, key) if isinstance(key, str) else key
        nav = page   # the tab lit: the checklist has none of its own, but is the Library's while it has no game (adding one is its last step)
        if page is self.library and self.library.no_games(): page = self.setup
        self.pages.setCurrentWidget(page)
        self.navgroup.setExclusive(False)   # an exclusive group keeps one tab lit: the checklist, opened from the pill, lights none
        for k, b in self.navs.items(): b.setChecked(self.page_of[k] is nav); b.setIcon(icon({'library': 'library', 'builds': 'builds', 'phones': 'phone', 'perf': 'perf', 'settings': 'settings'}[k], TEXT if self.page_of[k] is nav else TEXT2, 18))
        self.navgroup.setExclusive(True)
        if hasattr(self, 'pill'): self.show_pill()
    def act(self, action, folder=''):   # the button of what went wrong (explain.py's Help)
        if action == 'phone': self.phone_guide()
        elif action == 'retry': self.builds.retry()
        elif action == 'keys': self.settings.pick_keys()
        elif action == 'tools': self.settings.install(show_builds=False); self.go('setup')
        elif action == 'folder' and folder: reveal(folder)
        elif action == 'backup': self.phones.backup()
        elif action == 'issue': QDesktopServices.openUrl(QUrl(ex.ISSUE_441))
    def phone_guide(self): PhoneGuide(self).exec()
    def phone_brand(self):   # the guide's brand: of the phone adb sees (its maker once it has allowed this computer, its model code before), else the one picked last time
        d = next((d for d in self.devs or [] if d['state'] == 'device'), None) or next(iter(self.devs or []), None)
        b = pg.brand_for(d.get('manufacturer', ''), d.get('model', '')) if d else 'other'
        return b if b != 'other' else self.st.value('phone_brand', 'other') or 'other'
    def no_phone(self):   # why there is no phone to install to, from what adb lists: one waiting for Allow or not answering says so
        d = next((d for d in self.devs or [] if d['state'] != 'device'), None)
        h = ex.explain(f'0 adb devices ready (x ({d["state"]}))' if d else '0 adb devices ready (none)', phone=d.get('model', '') if d else '')
        return f'{h.title}. ' + re.sub(r'<[^>]+>', '', h.body)
    def after_check(self, _rows):   # --check's first answer: a launch with the tools missing lands on the checklist too, unless the player has gone elsewhere meanwhile
        self.setup_changed()
        if self.pages.currentWidget() is self.library and not self.setup.step('tools').done: self.go('setup')
    def setup_changed(self):   # keys, tools, phones or games changed: the checklist's steps, the pill and Settings' line again
        if not hasattr(self, 'library') or not hasattr(self, 'pill'): return   # still making the pages
        n = self.setup.refresh(); self.pill.setText(f'Setup {n} of 4')
        self.settings.start_line.setText('All four steps are done.' if n == 4 else f'{n} of 4 steps done.'); self.show_pill()
    def show_pill(self):   # while a step is left, but not over the checklist itself, and only once --check and the first read of the games have answered: until then
        # their steps say they are looking, and a set-up player's every launch flashed "Setup 2 of 4"
        n = fr.ready_count(self.setup.steps)
        self.pill.setVisible(n < 4 and self.pages.currentWidget() is not self.setup and self.settings.checked and getattr(self.library, 'read_once', False))
    def toast(self, text):
        self.toastl.setText(text); self.toastl.setMaximumWidth(560); self.toastl.adjustSize()
        self.toastl.move(self.width() - self.toastl.width() - 28, 84); self.toastl.raise_(); self.toastl.show(); self.toast_t.start(5000)
    def toggle_search(self):
        on = not self.search.isVisible(); self.search.setVisible(on)
        if on: self.search.setFocus(); self.go('library')
        else: self.search.clear()
    def filter_games(self, t): self.library.filter = t; self.library.render()
    def add_game(self):
        d = QFileDialog.getExistingDirectory(self, 'The folder with your game: its NSP / XCI files, or a Switch 2 game\'s .nup or dump')
        if not d: return
        fs = [f for f in (self.st.value('folders', []) or []) if f]
        if d not in fs: self.st.setValue('folders', fs + [d])
        if self.pages.currentWidget() is not self.setup: self.go('library')   # the checklist says it reads the game, then what comes next
        self.library.reload(); self.toast('Reading the game… then Build and install puts it on your phone')
    def stop_build(self): self.builds.stop()
    def keys(self): k = self.st.value('keys', '') or ''; return k if k and Path(k).is_file() else None
    def script_json(self, *args):
        r = subprocess.run([PYTHON, str(SCRIPT), *args], capture_output=True, text=True, encoding='utf-8', errors='replace', creationflags=NOWINDOW, env=dict(os.environ, PYTHONUTF8='1'))
        if r.returncode: raise RuntimeError(ex.script_error(r.stdout, r.stderr))   # its error is on stdout after --info's notes on stderr: the error last
        return json.loads(r.stdout)
    def serial(self): return self.phone.currentData() or None
    def phone_name(self): return self.models.get(self.serial()) or self.serial()
    def phone_key(self, s=None):   # a phone's ID (phone_id.py), or its serial when it gives none: what its settings and backups are kept under
        s = s or self.serial()
        return self.ids.get(s) or s
    def find_phone(self, keys): return phone_id.find([self.phone.itemData(i) for i in range(self.phone.count())], keys, self.ids)
    def serial_of(self, s): return phone_id.current(s, [d for d in self.devs or [] if d['state'] == 'device'], self.seen)   # the serial s's phone is on now, or None
    def set_serial(self, s):   # a row of the Phones page: the picker has one entry a phone, so a phone's Wi-Fi row picks its entry over the cable
        i = self.find_phone([s, self.ids.get(s)])
        if i >= 0: self.phone.setCurrentIndex(i)
    def track_lost(self, was, devs):   # the picked phone gone from adb's ready list after it was there over Wi-Fi (asleep, roamed, a new port): looked for by its ID
        # every RECONNECT_EVERY s for RECONNECT_FOR s. Back: its stale offline entry dropped, and the Performance page's log started again if it was running
        here = {d.get('id') or d['serial']: d['serial'] for d in devs if d['state'] == 'device'}; s = self.serial()
        listed = set(here) | set(here.values())   # by ID and by serial: an ID the first look hadn't read yet
        for i in list(self.let_go):   # still listed a moment after Disconnect (adb takes a second), then gone; listed again later: looked for again when it next drops
            if i not in listed: self.let_go[i] = 'gone'
            elif self.let_go[i] == 'gone': del self.let_go[i]
        if was and s and phone_id.wifi(s) and was not in here and not self.lost and was not in self.let_go and s not in self.let_go:
            self.lost = {'id': was, 'name': self.models.get(s) or s, 'addr': s, 'until': time.time() + wifi.RECONNECT_FOR}
        if not self.lost: return
        if self.lost['id'] in here:
            old, back = self.lost, here[self.lost['id']]; self.lost = None
            gone = wifi.stale(devs, old['id'], self.seen, back)
            if gone: bg(lambda: [adb_run(None, 'disconnect', g, timeout=10) for g in gone], lambda _: QTimer.singleShot(0, self.refresh_devices))
            self.toast(f'{old["name"]} is back over Wi-Fi'); QTimer.singleShot(500, self.perf.resume); return
        if time.time() > self.lost['until']: self.lost = None; return
        self.reconnect()
    def reconnect(self):   # once every RECONNECT_EVERY s while the phone is lost: its address on the network, by its serial number, else the last one it had
        if self.reconnecting or time.time() < self.reconnect_at or not self.lost: return
        lost = dict(self.lost); self.reconnecting, self.reconnect_at = True, time.time() + wifi.RECONNECT_EVERY
        def look():
            for a in wifi.reconnect_targets(lost['id'], adb_run(None, 'mdns', 'services', timeout=8)[1], lost['addr']):
                if wifi.connect_result(*adb_run(None, 'connect', a, timeout=10))[0]: return a
            return None
        def got(a):
            self.reconnecting = False
            if a and not isinstance(a, Exception): self.refresh_devices()
        bg(look, got)
    def phone_changed(self, _):
        s = self.serial()
        if s: self.st.setValue('serial', s); self.st.setValue('phone', self.phone_key(s))   # the phone by its ID too, which outlives a Wi-Fi port
        self.refresh_phone()
    def refresh_phone(self):   # the app and the games on the picked phone: the Library's statuses and the Phones page
        s = self.serial()
        if not s: self.phone_games = {}; self.library.refresh_status(); self.phones.show_phone('', {}, False); return
        def look():
            _, pkg = adb_run(s, 'shell', f'dumpsys package {PKG} | grep -E "lastUpdateTime|versionName" | head -2')
            # game.json on one line, its \r too: a build on Windows wrote it with \r\n, and splitlines() below cut the JSON at each \r
            _, ls = adb_run(s, 'shell', f'for d in {REMOTE}/games/*/; do [ -f "$d/code/main.fixups.tsv" ] || continue; echo "@$d"; cat "$d/game.json" 2>/dev/null | tr -d "\\r\\n"; echo; echo "#$(ls "$d/glslc_capture" 2>/dev/null | wc -l)"; done; '
                                        f'[ -f {REMOTE}/game/code/main.fixups.tsv ] && echo LEGACY')
            games, legacy, cur = {}, False, None
            for l in ls.splitlines():
                if l == 'LEGACY': legacy = True
                elif l.startswith('@'): cur = l[1:].rstrip('/').rsplit('/', 1)[-1].upper(); games[cur] = {}
                elif cur and l.strip().startswith('{'):
                    try: games[cur] = dict(json.loads(l), **{k: v for k, v in games[cur].items() if k == 'captures'})
                    except ValueError: pass
                elif cur and l.startswith('#') and l[1:].strip().isdigit(): games[cur]['captures'] = int(l[1:])   # run-time shaders the game saved on the phone
            return pkg, games, legacy
        def got(r):
            if isinstance(r, Exception): return
            pkg, games, legacy = r
            self.phone_games = games if 'lastUpdateTime' in pkg else {}; self.phone_legacy = legacy
            self.library.refresh_status(); self.phones.show_phone(pkg, games, legacy)
        bg(look, got)
    def refresh_devices(self):
        def look():
            if not shutil.which('adb'): return None   # Install what's missing brings it, with the Android SDK
            ok, out = adb_run(None, 'devices', '-l', timeout=10)
            devs = []
            for d in phone_id.devices(out) if ok else []:   # Linux's "no permissions (...)" is one state with spaces in it: a split line read it as "no"
                if d['state'] == 'device':   # in one call: Android's version, the model's market name where the maker sets one (devices -l has its code: OPD2415 for a OnePlus Pad 3), the phone's own name
                    # and the model as the phone spells it: devices -l turns what isn't a letter or a digit into _, so a Galaxy's SM-S937U1 came out as SM S937U1
                    # its maker (the phone guide's brand), and its battery and free space (a computer's USB port charges slowly; an install needs room)
                    got_p, p = adb_run(d['serial'], 'shell', 'getprop ro.build.version.release; getprop ro.product.marketname; getprop ro.vendor.oplus.market.name; '
                                       'getprop ro.product.model; settings get global device_name; getprop ro.product.manufacturer; '
                                       'dumpsys battery | grep -E "powered:|level:|status:"; df -k /sdcard | tail -n 1', timeout=8)
                    v = [x.strip() for x in p.splitlines()] + [''] * 6 if got_p else [''] * 6
                    d['android'], d['model'] = v[0], v[1] or v[2] or v[3] or d['model']
                    d['name'] = v[4] if v[4] not in ('', 'null') else d['model']
                    d['manufacturer'] = v[5]; d.update(pg.parse_status(p) if got_p else {})
                devs.append(d)
            ids = self.ids.copy()   # read on the pool's thread: a copy is one step
            for d in devs:   # each phone's ID (phone_id.py), asked once per serial: one phone on a cable and on Wi-Fi is one phone, and keeps its settings over a new port
                if d['state'] == 'device' and d['serial'] not in ids:
                    got_i, out_i = adb_run(d['serial'], 'shell', phone_id.ID_CMD, timeout=8)
                    if got_i: ids[d['serial']] = phone_id.from_props(out_i)
                d['id'] = ids.get(d['serial']) or phone_id.mdns_serialno(d['serial'])
            return devs if ok else out
        def got(r):
            if isinstance(r, Exception): return
            adb = r is not None
            if isinstance(r, str):   # adb didn't answer: the list stays as it was, or says no phone is connected when it was never filled
                if self.devs is not None: return
                r = []
            r = r or []
            was = self.phone_key(); self.ids = {d['serial']: d['id'] for d in r if d.get('id')}; self.seen.update(self.ids)   # the picked phone's ID as the last list had it
            self.track_lost(was, r)
            if r == self.devs and adb == self.adb: return
            ident = lambda ds: [(d['serial'], d['state'], d.get('model'), d.get('name'), d.get('android')) for d in ds]   # noqa: E731
            same = self.devs is not None and adb == self.adb and ident(r) == ident(self.devs)
            self.devs, self.adb = r, adb
            if same: self.phones.fill(r, adb); self.setup_changed(); return   # only a battery level or the free space moved: the picker and the phone's games stay
            keep = [was, (self.lost or {}).get('id'), self.st.value('phone', ''), self.st.value('serial', '')]   # the picked phone by its ID: the same phone over Wi-Fi once its cable is out
            self.models = {d['serial']: d.get('name') or d.get('model') or d['serial'] for d in r}
            self.phone.blockSignals(True); self.phone.clear()
            for d in phone_id.one_per_phone([d for d in r if d['state'] == 'device']):   # one entry a phone, over the cable when it is on a cable and on Wi-Fi (the Phones page lists both)
                self.phone.addItem(icon('phone', TEXT2, 16), f'{self.models[d["serial"]]}' + (f' · Android {d.get("android")}' if d.get('android') else ''), d['serial'])
            # a phone plugged in but not ready: listed, greyed, with what to do; it said "No phone connected" while the phone waited for Allow, and left out a phone
            # Linux doesn't let this user open (no udev rule), which Get started then called "connected but not ready"
            for d in r:
                if d['state'] in ('unauthorized', 'authorizing', 'offline', phone_id.NO_PERMISSIONS):
                    off, perm = d['state'] == 'offline', d['state'] == phone_id.NO_PERMISSIONS
                    self.phone.addItem(icon('phone', ERR if off or perm else BUILD, 16), 'Phone found: USB access needed (How to connect a phone)' if perm else
                                       ((f'Reconnecting to {self.lost["name"]} over Wi-Fi…' if self.lost else 'Wake your phone (Wi-Fi)')
                                        if phone_id.wifi(d['serial']) else 'Plug your phone in again') if off else 'Tap Allow on your phone', None)
                    it = self.phone.model().item(self.phone.count() - 1); it.setEnabled(False)
                    it.setToolTip(re.sub(r'<[^>]+>', '', ex.explain(f'device {d["state"]} ({d.get("why", "")})' if perm else f'device {d["state"]}',
                                                                    phone=d.get('model', ''), wifi=phone_id.wifi(d['serial'])).body))
            if not self.phone.count(): self.phone.addItem(icon('phone', BUILD if self.lost else TEXT3, 16), f'Reconnecting to {self.lost["name"]} over Wi-Fi…' if self.lost and adb else
                                                          'No phone connected' if adb else 'Install the tools to find phones', None)
            self.phone.setToolTip('The phone builds install to' if adb else 'DroidPlay finds phones with adb, which comes with the tools: Get started, step 2')
            i = self.find_phone(keep); self.phone.setCurrentIndex(i if i >= 0 else 0); self.phone.blockSignals(False)
            self.phones.fill(r, adb); self.phone_changed(0); self.setup_changed()
        bg(look, got)


def open_window(argv):   # the app and its window, shown: main's, and the AppImage's window check (dm/android/linux/window_check.py)
    if os.name == 'nt':   # a taskbar button of its own, with DroidPlay's name and icon, not grouped with every other Python window
        try: import ctypes; ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID('io.github.enjoyerofslop.droidplay.desktop')
        except Exception: pass
    app = QApplication(argv); app.setApplicationName('DroidPlay'); app.setWindowIcon(app_icon()); app.setStyle('Fusion'); app.setStyleSheet(STYLE)
    # Wayland takes a window's icon and its dock entry from the .desktop file named by the app ID (the AppImage's, dm/android/linux), not from setWindowIcon
    app.setDesktopFileName('io.github.enjoyerofslop.droidplay')
    w = Window(); w.show()
    return app, w


def main():
    if '--write-icon' in sys.argv[:-1]:   # DroidPlay's icon as a file: an .ico for the Windows installer's shortcuts, a .png for the AppImage (dm/android/linux/stage.sh)
        app = QApplication(sys.argv[:1]); path = sys.argv[sys.argv.index('--write-icon') + 1]
        if not app_icon().pixmap(256, 256).save(path, 'PNG' if path.lower().endswith('.png') else 'ICO'): sys.exit(f'could not write {path}')
        return
    app, _ = open_window(sys.argv); sys.exit(app.exec())


if __name__ == '__main__':
    main()
