"""DroidPlay's "Get started" checklist (Windows and Linux) (scripts/nxnative_android.py's SetupPage): its four steps, worked out each time from what this computer has now
(the keys file, --check's rows, adb's devices, the game folders) and never stored, so a step can't say done when it no longer is. And the one bar Install tools
shows across all its parts, from --setup's events. Qt-free, so dm/tools/tests/test_android_desktop.py tests it without PySide6."""
import functools
import re
import sys
from dataclasses import dataclass
from pathlib import Path

import phone_guide   # this folder's: the phone's battery and free space
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from phone_id import NO_PERMISSIONS, wifi   # noqa: E402  # pyright: ignore[reportMissingImports]  (dm/android/tools, put on the path above: a serial that is a Wi-Fi connection)

TODO, WORKING, WAITING, DONE, PROBLEM = 'todo', 'working', 'waiting', 'done', 'problem'
WARN = '#f5b82e'   # the app's amber, for a warning inside a done step's text
SETUP_TITLE = 'Set up this computer'   # the Builds page's title for --setup, from the app or the console (its journal)

INTRO = ('<b>Setting up takes about half an hour the first time:</b> the tools download once (1.5 GB), and the first build of a game takes about 10–20 minutes. '
         'After that, a new game takes about 10 minutes, and playing again takes seconds.')
KEYS_INTRO = ('DroidPlay reads your games with the keys from your own Switch, a file called <b>prod.keys</b> (Lockpick_RCM makes it). '
              "They stay on this computer. A Switch 2 game's .nup or dump folder doesn't need them.")
TOOLS_INTRO = ("DroidPlay builds games with Microsoft's Java and .NET and Google's Android SDK. They download once: about 1.5 GB, 5 GB on disk.")
PHONE_INTRO = 'Plug in your Android phone or handheld with a USB cable.'
GAME_INTRO = ("Pick the folder with a game's NSP or XCI files (the base game, and its update if you have it), or the folder that holds a Switch 2 game's "
              '.nup file or its dump. '
              'DroidPlay writes its build files (a few GB) into a folder beside them.')


@dataclass
class Step:
    key: str              # keys, tools, phone, game
    title: str
    state: str
    text: str             # rich text: names in it are escaped
    button: str = ''      # its one button, '' for none
    fraction: float | None = None   # a bar under the text while it works: -1 for one with no count yet
    ref: str = ''         # the game's folder the button acts on (game)

    @property
    def done(self): return self.state == DONE


def esc(s): return str(s).replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')


@functools.lru_cache(maxsize=8)
def _keys_problem(path, _mtime, _size):
    try:
        with open(path, encoding='utf-8', errors='replace') as f: text = f.read(1 << 20)
    except OSError as e: return f"DroidPlay can't open it: {esc(e.strerror or e)}."
    # prod.keys names its keys (header_key, master_key_00…); title.keys, the file Lockpick_RCM writes beside it, is rights IDs alone
    if not re.search(r'^\s*(header_key|master_key_[0-9a-f]{2})\s*=', text, re.M | re.I): return 'This file has no Switch keys in it. Pick prod.keys, not title.keys.'
    return None


def keys_step(path):
    title = 'Your keys'
    if not path: return Step('keys', title, TODO, KEYS_INTRO, 'Choose prod.keys…')
    p = Path(path)
    try: st = p.stat()
    except OSError: return Step('keys', title, PROBLEM, f"The keys file you picked isn't there any more ({esc(path)}). Choose it again.", 'Choose prod.keys…')
    problem = _keys_problem(str(p), st.st_mtime, st.st_size)
    if problem: return Step('keys', title, PROBLEM, f'<b>{esc(p.name)}</b>: {problem}', 'Choose prod.keys…')
    return Step('keys', title, DONE, f'<b>{esc(p.name)}</b> in {esc(p.parent)}', 'Change…')


# --check's rows by the part Install tools brings them with, named as a player knows them
PARTS = (('Java', lambda n: n.startswith('JDK')), ('.NET', lambda n: n == '.NET SDK 10'),
         ('the Android SDK', lambda n: n in ('Android SDK', 'adb', 'NDK', 'CMake') or n.startswith(('build-tools ', 'platform '))))


def missing_parts(rows):
    bad = [r['name'] for r in rows if not r['ok']]
    return [part for part, has in PARTS if any(has(n) for n in bad)]


def tools_step(rows, can_install, run=None):
    """rows: --check's, or None while it runs. can_install: --setup installs on this computer (Windows, x86_64 Linux: host_setup.unsupported); elsewhere Settings
    says how to get each part. run: ('running', fraction, text) while --setup runs, ('failed', message) after it stopped, else None"""
    title = 'Tools for building'
    if run and run[0] == 'running': return Step('tools', title, WORKING, esc(run[2]), fraction=run[1])
    if rows is None: return Step('tools', title, WORKING, 'Looking at what this computer has…')
    bad = [r for r in rows if not r['ok']]
    check = next((r for r in bad if r['name'] == 'check'), None)
    if check: return Step('tools', title, PROBLEM, f"DroidPlay couldn't look at what this computer has: {esc(check['detail'])}", 'Look again')
    python = [r['name'].split()[-1] for r in bad if r['name'].startswith('Python')]
    parts = missing_parts(rows)
    if run and run[0] == 'failed' and parts:
        return Step('tools', title, PROBLEM, f'Installing the tools stopped: {esc(run[1])}', 'Try again' if can_install else '')
    if parts:
        what = TOOLS_INTRO if len(parts) == len(PARTS) else f"{TOOLS_INTRO.split('. ')[0]}. Still to install: {', '.join(parts)}."
        return Step('tools', title, TODO, what if can_install else f"{what} Settings says how to get each one.", 'Install tools…' if can_install else 'Open Settings')
    if python:   # the app's own: Setup and uv run bring them, so only a Python picked by hand misses them
        return Step('tools', title, PROBLEM, f"The Python that runs DroidPlay doesn't have {esc(', '.join(python))}. Settings says how to get them.", 'Open Settings')
    return Step('tools', title, DONE, 'Java, .NET and the Android SDK are on this computer.')


def phone_name(d): return d.get('name') or d.get('model') or ''


def phone_step(devs, adb, picked=None, seen=False):
    """devs: refresh_devices' list (serial, state, name, model, android), None before the first look. adb: whether adb is installed. picked: the top bar's phone.
    seen: a phone was picked before (the app keeps the last one), so one unplugged now was still set up"""
    title = 'Your phone'
    if not adb:
        return Step('phone', title, DONE if seen else TODO, ("Your phone was set up before. DroidPlay finds it again once the tools are back (step 2)." if seen else
                                                           f'{PHONE_INTRO} DroidPlay finds it with Android\'s adb, which comes with the tools (step 2).'), '' if seen else 'Set up my phone…')
    if devs is None: return Step('phone', title, WORKING, 'Looking for your phone…')
    ready = sorted((d for d in devs if d['state'] == 'device'), key=lambda d: d['serial'] != picked)
    if ready:
        d = ready[0]; low = phone_guide.battery_warning(d)
        return Step('phone', title, DONE, f'<b>{esc(phone_name(d) or d["serial"])}</b>' + (f' · Android {esc(d["android"])}' if d.get('android') else '') +
                    phone_guide.readout(d) + (f'<br><span style="color:{WARN}">⚠ {esc(low)}</span>' if low else ''))
    for d in devs:   # adb lists a phone that hasn't allowed this computer with no model or name: "Your phone" then
        name = esc(phone_name(d) or 'Your phone')
        if d['state'] in ('unauthorized', 'authorizing'):
            return Step('phone', title, WAITING, f'<b>{name}</b> is waiting: unlock it and tap <b>Allow</b> on "Allow USB debugging?"', 'Set up my phone…')
        if d['state'] == NO_PERMISSIONS:   # Linux: adb sees it on USB but may not open it, for want of a udev rule giving it to this user
            return Step('phone', title, PROBLEM, f"<b>{name}</b> is plugged in, but Linux doesn't let DroidPlay use it over USB yet (adb says \"no permissions\"). "
                        "The phone guide's <b>If it doesn't work</b> says what to install for it.", 'Set up my phone…')
        if d['state'] == 'offline':
            return Step('phone', title, PROBLEM, f"<b>{name}</b> isn't answering over Wi-Fi: wake it, and check that Wireless debugging is still on." if wifi(d['serial'])
                        else f"<b>{name}</b> isn't answering: unplug it and plug it back in.", 'Set up my phone…')
    if devs:   # a phone in recovery or sideload: adb's own word, as the Phones page shows it
        d = devs[0]; return Step('phone', title, PROBLEM, f'<b>{esc(phone_name(d) or "Your phone")}</b> is connected but not ready (adb says "{esc(d["state"])}").', 'Set up my phone…')
    if seen: return Step('phone', title, DONE, "Your phone was set up before. It isn't plugged in now.")
    return Step('phone', title, TODO, PHONE_INTRO, 'Set up my phone…')


PLAY_LABELS = {'build': 'Build and install', 'install': 'Install on phone', 'play': 'Play'}


def game_step(folders, games, reading):
    """folders: the game folders added; games: what reading them gave, as dicts (folder, name, state as Game.status gives it, tested, error); reading: a read runs"""
    title = 'Your first game'
    ok = [g for g in games if g['state'] not in ('keys', 'tools', 'none', 'folder')]   # folder: the read found something to change in it
    if ok:
        g = ok[0]; label = PLAY_LABELS.get(g['state'], '')
        how = 'not supported yet' if g['state'] == 'survey' else 'played on Android' if g.get('tested') else 'not played on Android yet'
        took = ' · the first build takes about 10–20 minutes' if g['state'] == 'build' else ''
        return Step('game', title, DONE, f'<b>{esc(g["name"])}</b> · {how}{took}', label or 'Add another game…', ref=g['folder'] if label else '')
    if reading:
        name = Path(folders[-1]).name if folders else 'your game'
        return Step('game', title, WORKING, f'Reading <b>{esc(name)}</b>… The first read can take a few minutes while DroidPlay prepares its game reader.', fraction=-1)
    if games:
        g = games[0]; name = esc(g['name'])
        if g['state'] == 'keys': return Step('game', title, TODO, f'<b>{name}</b> is added. DroidPlay reads it once your keys are in (step 1).')
        if g['state'] == 'tools': return Step('game', title, TODO, f'<b>{name}</b> is added. DroidPlay reads it once the tools are installed (step 2).')
        return Step('game', title, PROBLEM, f"DroidPlay couldn't read <b>{name}</b>: {esc(g.get('error') or 'no game in that folder')}", 'Add a game…')
    return Step('game', title, TODO, GAME_INTRO, 'Add a game…')


# ---- Install tools' one bar ----
# --setup's stages in the order host_setup.setup() runs them, each with what it downloads (MB): a stage's share of the bar, as about 1.6 GB in all
SETUP_STAGES = (('JDK 21', 'Java', 190), ('.NET SDK 10', '.NET', 290), ('Android command-line tools', 'the Android tools', 150), ('Android SDK packages', 'the Android SDK', 950))
PACKAGE_NAMES = {'platform-tools': 'adb', 'platforms': 'the platform', 'build-tools': 'the build tools', 'ndk': 'the NDK', 'cmake': 'CMake'}
# the SDK's packages in the order --setup installs them (build_android_from_nsp.sdk_packages), with what each downloads (MB): the NDK is most of the stage, and
# counted as one package of five the bar said 70% through the stage while it installed (measured on the test PC: 50 of its 60 seconds)
PACKAGE_MB = (('platform-tools', 10), ('platforms', 60), ('build-tools', 60), ('ndk', 700), ('cmake', 20))


def mb_text(n): return f'{n / 2**30:.1f} GB' if n >= 2**30 else f'{n / 2**20:.0f} MB'


def left_text(sec):   # rounded, so it doesn't jump about as a countdown would
    if sec is None: return ''
    return 'less than a minute left' if sec < 60 else f'about {round(sec / 60)} minute{"" if round(sec / 60) == 1 else "s"} left'


class SetupProgress:
    """--setup's events → one fraction across its stages and a line saying what it does now. It installs only what is missing, so its first stage event's count (n)
    says how many of SETUP_STAGES run: the ones not seen yet are the last ones after the current, as its order is fixed"""
    def __init__(self, t0):
        self.t0, self.n, self.seen, self.frac, self.ev = t0, 0, [], 0.0, {}

    def feed(self, ev):
        k = ev.get('ev')
        if k == 'stage':
            self.n = ev.get('n') or self.n
            if ev.get('name') not in self.seen: self.seen.append(ev.get('name')); self.frac = 0.0
            self.ev = ev
        elif k in ('step', 'progress') and self.seen:
            if k == 'progress' and ev.get('total') and ev.get('done') is not None: self.frac = self.part_done(ev)
            self.ev = ev
        elif k == 'stage_end': self.frac = 1.0

    @staticmethod
    def part_done(ev):   # the fraction of the stage a progress event says is done: bytes for a download, packages weighted by size when all of them install
        done, total = ev['done'], ev['total']
        if ev.get('unit') != 'B' and total == len(PACKAGE_MB) and done < total:
            mb = [m for _, m in PACKAGE_MB]; return sum(mb[:done]) / sum(mb)
        return min(done / total, 1.0)

    def planned(self):
        names = [s[0] for s in SETUP_STAGES]
        if not self.seen: return []
        cur = self.seen[-1]; after = names[names.index(cur) + 1:] if cur in names else []
        left = max(0, self.n - len(self.seen))
        return self.seen + (after[len(after) - left:] if left else [])

    def fraction(self):
        weight = {s[0]: s[2] for s in SETUP_STAGES}; plan = self.planned()
        if not plan: return 0.0
        total = sum(weight.get(p, 100) for p in plan)
        done = sum(weight.get(p, 100) for p in self.seen[:-1]) + weight.get(self.seen[-1], 100) * self.frac
        return min(done / total, 1.0)

    def text(self, now):
        if not self.seen: return 'Starting…'
        plain = {s[0]: s[1] for s in SETUP_STAGES}.get(self.seen[-1], self.seen[-1]); step = self.ev.get('step') or ''
        if step.startswith('downloading'):
            line = f'Downloading {plain}'
            if self.ev.get('unit') == 'B' and self.ev.get('total'): line += f' · {mb_text(self.ev.get("done") or 0)} of {mb_text(self.ev["total"])}'
        elif step.startswith('unpacking'): line = f'Unpacking {plain}'
        elif step.startswith('installing '):
            pkg = step.split(' ', 1)[1].split(';')[0]; n, total = self.ev.get('done'), self.ev.get('total')
            line = f'Installing {plain} · {PACKAGE_NAMES.get(pkg, pkg)}' + (f' ({n + 1} of {total})' if total and n is not None and n < total else '')
        else: line = f'Installing {plain}'
        f, took = self.fraction(), now - self.t0
        left = took * (1 - f) / f if f >= 0.02 and took >= 10 else None   # the rate so far, once there is one
        return line + (f' · {left_text(left)}' if left is not None else '')


def ready_count(steps): return sum(s.done for s in steps)
