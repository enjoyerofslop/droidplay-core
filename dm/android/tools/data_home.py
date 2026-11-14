# Where DroidPlay keeps what it makes on this computer: the game hosts, the loader, the APK, the builds journal, the device locks and the tools Install
# what's missing downloads. %LOCALAPPDATA%\nxnative\android on Windows; on Linux $XDG_DATA_HOME/nxnative/android, which is ~/.local/share/nxnative/android
# when XDG_DATA_HOME isn't set; ~/.local/share/nxnative/android on a Mac. One function, so the packing script, the device locks and the bench never disagree
# about the folder: a lock taken in one folder would not be seen by a script looking in another.
import os
import sys
from pathlib import Path


def data_dir(env=None):
    env = os.environ if env is None else env
    if os.name == 'nt' and env.get('LOCALAPPDATA'): return Path(env['LOCALAPPDATA']) / 'nxnative' / 'android'
    xdg = env.get('XDG_DATA_HOME', '')
    # the XDG spec says a relative path is to be ignored; a Mac keeps the folder it always had, whatever a shell set
    base = Path(xdg) if sys.platform.startswith('linux') and os.path.isabs(xdg) else Path.home() / '.local' / 'share'
    return base / 'nxnative' / 'android'
