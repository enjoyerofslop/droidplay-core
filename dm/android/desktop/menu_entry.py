"""DroidPlay for Linux's "Add to the applications menu" (Settings, shown when DroidPlay runs from its AppImage): an AppImage is one file, with no entry in the
desktop's menu or launcher until something writes one. This writes the desktop entry and the icon where the freedesktop.org specs look for a user's own,
$XDG_DATA_HOME/applications/io.github.enjoyerofslop.droidplay.desktop and $XDG_DATA_HOME/icons/hicolor/256x256/apps/ (~/.local/share when XDG_DATA_HOME
isn't set), with Exec naming the AppImage file (its $APPIMAGE), not the unpacked version under ~/.local/share/DroidPlay, which a newer version replaces.
TryExec names it too, so the menu drops the entry by itself once the AppImage is deleted; an update that gives the AppImage the new release's name points
the entry at it (retarget). KDE and GNOME watch that folder: nothing else needs to run.
AppImageLauncher and Gear Lever users get their own entry from those tools instead. Qt-free (the icon is drawn by the caller), so
dm/tools/tests/test_android_linux.py tests it."""
import os
from pathlib import Path

APP_ID = 'io.github.enjoyerofslop.droidplay'
TEMPLATE = Path(__file__).resolve().parents[1] / 'linux' / f'{APP_ID}.desktop'   # the AppImage's own entry, which this one copies but for Exec


def data_home(env=None):   # a relative XDG_DATA_HOME is ignored, as the XDG spec says
    env = os.environ if env is None else env
    xdg = env.get('XDG_DATA_HOME', '')
    return Path(xdg) if os.path.isabs(xdg) else Path(env.get('HOME') or Path.home()) / '.local' / 'share'


def entry_path(env=None): return data_home(env) / 'applications' / f'{APP_ID}.desktop'
def icon_path(env=None): return data_home(env) / 'icons' / 'hicolor' / '256x256' / 'apps' / f'{APP_ID}.png'


def appimage(env=None):   # the AppImage file this DroidPlay was started from (its runtime sets APPIMAGE, which unpack.py's exec keeps), or None
    env = os.environ if env is None else env
    p = env.get('APPIMAGE', '')
    return Path(p) if p and os.path.isabs(p) and os.path.isfile(p) else None


def exec_arg(path):
    """path as one argument of a desktop entry's Exec key: in double quotes, with ", `, $ and \\ escaped by a backslash (the Exec quoting rule), then each
    backslash doubled (the rule for every string value, applied first when the file is read), and % doubled (field codes)"""
    path = str(path)
    if any(c in path for c in '\n\r\t'): raise ValueError(f'a desktop entry cannot start a file whose name has a line break or tab in it: {path!r}')
    quoted = ''.join({'"': r'\\"', '`': r'\\`', '$': r'\\$', '\\': r'\\\\'}.get(c, c) for c in path)
    return '"' + quoted.replace('%', '%%') + '"'


def string_value(path):   # a plain string key's value (TryExec): only the backslash and a leading space need escaping
    s = str(path).replace('\\', '\\\\')
    return '\\s' + s[1:] if s.startswith(' ') else s


def read_string(value):   # string_value read back
    out, i = [], 0
    while i < len(value):
        if value[i] == '\\' and i + 1 < len(value): out.append({'s': ' ', 'n': '\n', 't': '\t', 'r': '\r', '\\': '\\'}.get(value[i + 1], value[i + 1])); i += 2
        else: out.append(value[i]); i += 1
    return ''.join(out)


def entry_text(target, template=None):   # the AppImage's own desktop entry, starting target: its Exec, a TryExec, the rest as it is
    lines = (template if template is not None else TEMPLATE.read_text(encoding='utf-8')).splitlines()
    out = []
    for ln in lines:
        if ln.startswith(('TryExec=', 'X-DroidPlay-')): continue
        if ln.startswith('Exec='): out += [f'Exec={exec_arg(target)}', f'TryExec={string_value(target)}']
        else: out.append(ln)
    return '\n'.join(out + ['X-DroidPlay-AddedBy=DroidPlay Settings']) + '\n'


def current(env=None):   # (the entry's file, the AppImage it starts) when DroidPlay wrote one, else None
    p = entry_path(env)
    try: text = p.read_text(encoding='utf-8')
    except (OSError, UnicodeDecodeError): return None
    if 'X-DroidPlay-AddedBy=' not in text: return None   # someone else's entry of that name (a package's): neither shown as ours nor removed
    target = next((read_string(ln[8:]) for ln in text.splitlines() if ln.startswith('TryExec=')), '')
    return p, Path(target) if target else None


def state(env=None):   # 'none', 'this' (the entry starts this AppImage) or 'other' (it starts another copy: moved, renamed, or an older download)
    cur, me = current(env), appimage(env)
    if not cur: return 'none'
    return 'this' if me and cur[1] and os.path.realpath(cur[1]) == os.path.realpath(me) else 'other'


def add(target, write_icon, env=None):
    """Writes the entry starting target (the AppImage) and the icon (write_icon(path) draws it). Each file is written beside its place and renamed into it,
    so the desktop, which watches the folder, never reads half a file. → the entry's path"""
    entry, icon = entry_path(env), icon_path(env)
    icon.parent.mkdir(parents=True, exist_ok=True); entry.parent.mkdir(parents=True, exist_ok=True)
    tmp = icon.with_name('.' + icon.name + '.part')
    if not write_icon(tmp) or not tmp.is_file(): tmp.unlink(missing_ok=True); raise OSError(f'the icon could not be written to {icon.parent}')
    os.replace(tmp, icon)
    tmp = entry.with_name('.' + entry.name + '.part')
    tmp.write_text(entry_text(target), encoding='utf-8'); os.chmod(tmp, 0o644); os.replace(tmp, entry)
    return entry


def retarget(old, new, env=None):
    """DroidPlay's entry, when it starts old, made to start new: an update gave the AppImage the new release's name (update.py replace_appimage). The rest of
    the entry stays as it was written, and so does the icon. Written beside the entry and renamed into place, as add does. → whether it changed"""
    cur = current(env)
    if not cur or not cur[1] or os.path.realpath(cur[1]) != os.path.realpath(old): return False
    lines = [f'Exec={exec_arg(new)}' if ln.startswith('Exec=') else f'TryExec={string_value(new)}' if ln.startswith('TryExec=') else ln
             for ln in cur[0].read_text(encoding='utf-8').splitlines()]
    tmp = cur[0].with_name('.' + cur[0].name + '.part')
    tmp.write_text('\n'.join(lines) + '\n', encoding='utf-8'); os.chmod(tmp, 0o644); os.replace(tmp, cur[0])
    return True


def remove(env=None):   # the entry and the icon DroidPlay wrote; → whether there was an entry. Someone else's entry of that name stays
    if not current(env): return False
    entry_path(env).unlink(missing_ok=True); icon_path(env).unlink(missing_ok=True)
    return True
