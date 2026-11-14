# Opens DroidPlay's window as a start from the AppImage does, on the display Qt picks (offscreen, X11 or Wayland), waits for its first-run checks to answer
# (Get started's steps, and --check run through the packing script on this same Python), saves a picture of the window, and says which platform plugin drew
# it and where each Qt and X11 library it loaded came from. linux-package.yml's smoke test (smoke_test.sh) runs it from the unpacked AppImage in each distro's
# container, under X11 and Wayland as well as offscreen. It needs no phone, game or keys, and exits non-zero when the window doesn't show or the checks
# don't answer. It uses this user's DroidPlay settings and data: give it a scratch XDG_CONFIG_HOME and XDG_DATA_HOME.
#   <runtime>/bin/python3 window_check.py <core> <picture.png> [--expect xcb|wayland|offscreen] [--timeout <s>]
import argparse
import sys
import time
from pathlib import Path

a = argparse.ArgumentParser(); a.add_argument('core'); a.add_argument('png'); a.add_argument('--expect', default=''); a.add_argument('--timeout', type=float, default=120)
args = a.parse_args()
core = Path(args.core).resolve()
sys.path.insert(0, str(core / 'scripts'))
import nxnative_android as nx   # noqa: E402  # pyright: ignore[reportMissingImports]  (the unpacked tree's app, put on the path above)
from PySide6.QtCore import QThreadPool, QTimer   # noqa: E402  # pyright: ignore[reportMissingImports]

t0 = time.monotonic()
app, w = nx.open_window([str(core / 'scripts' / 'nxnative_android.py')])
platform = app.platformName()
result = {}


def maps():   # each Qt, X11 and Wayland library this process loaded, and where from: the AppImage's copy or the system's
    seen = {}
    for line in open('/proc/self/maps', encoding='utf-8', errors='replace'):
        parts = line.split(maxsplit=5); path = parts[5].strip() if len(parts) == 6 else ''
        name = path.rsplit('/', 1)[-1]
        if name.startswith(('libQt6', 'libxcb', 'libxkbcommon', 'libwayland', 'libicu', 'libX11', 'libEGL', 'libGL')) and name not in seen: seen[name] = path
    return seen


def poll():
    exposed = bool(w.windowHandle() and w.windowHandle().isExposed())
    if w.settings.checked and exposed:
        result['ok'] = True; app.quit()
    elif time.monotonic() - t0 > args.timeout:
        result['ok'] = False; result['why'] = ('the checks did not answer' if not w.settings.checked else '') + (' the window was never exposed' if not exposed else '')
        app.quit()


timer = QTimer(); timer.timeout.connect(poll); timer.start(200)
app.exec()
QThreadPool.globalInstance().waitForDone(15000)   # the app's background calls (adb devices, --check) end before the objects they report to
if not w.grab().save(args.png): print(f'window_check: could not save {args.png}')
page = next((k for k, p in w.page_of.items() if p is w.pages.currentWidget()), '?')
missing = [r['name'] for r in (w.settings.rows or []) if not r['ok']] if getattr(w.settings, 'rows', None) else []
print(f'window_check: platform {platform}, {w.width()}x{w.height()}, page {page}, after {time.monotonic() - t0:.1f} s')
print(f'window_check: --check answered: {bool(w.settings.checked)}; missing: {", ".join(missing) or "nothing"}')
for s in w.setup.steps: print(f'window_check:   step {s.key}: {s.state}')
for name, path in sorted(maps().items()): print(f'window_check:   {name}: {"AppImage" if str(core.parent) in path else "system"} ({path})')
if args.expect and platform != args.expect: print(f'window_check: FAILED: drawn by {platform}, not {args.expect}'); sys.exit(1)
if not result.get('ok'): print(f'window_check: FAILED: {result.get("why", "?").strip()}'); sys.exit(1)
print('window_check: ok')
