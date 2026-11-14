#!/usr/bin/env python3
# Baseline runs of games on Android devices over adb: the same save, the same scripted input and the same measuring window on every device, so devices and builds
# compare. The Android counterpart of scripts/bench.py (the Mac host's).
#   python3 scripts/bench_android.py <NSP folder> [<NSP folder> ...] [-s <serial> ...] [--route <name>] [--runs N] [--install [-k <prod.keys>]] [--out <folder>] [--env KEY=VALUE]
#   An NSP folder is the one build_android_from_nsp.py packed (it holds <title ID>_build). Without -s, every device adb lists as ready. --install copies the app and the
#   game to each device first (build_android_from_nsp.py --push-only). Games run in turn, each on every device in turn, each run --runs times (default 2),
#   after a warm-up run with --warmup (run 0, reported apart: the first launch after an install builds the driver's pipelines). Install before a run, never during one.
# A route is a line of the game's dm/games/<id>/bench_android.tsv (the first by default; without the file, `boot`: no input, no save, measured from 20 to 50 s):
#   name <TAB> DM_KEYS or - <TAB> save or - <TAB> window start s <TAB> window length s <TAB> ready pattern or - <TAB> description [<TAB> KEY=VALUE;KEY=VALUE]
#   save: a save folder beside the NSP folder or in its parent (the game's folder), or a downloaded save's archive there, with the folder inside it that holds the save's
#   files after '//' (dm/android/tools/bench_sweep.py save_dir; unpacked into the output folder, never beside the archive); each run gets a fresh copy in the app's
#   external files (DM_SAVE points there), so the device's own saves are not touched. ready: a regular expression on the host's log whose first match marks the game ready (its title, say); its time is reported.
#   The last column, if there, is switches for env.txt this route needs (DM_FIXED_TIME for a game whose scene follows the clock, DM_KBD to answer a keyboard).
# Each run: cool down (thermal status 0 and the battery at or under --cool-temp, up to --cool-max s), env.txt with DM_SAVE, DM_KEYS and per-frame timing (DM_NVN_TSV), the
# game started from the launcher, screenshots at the window's start and end, and the game stopped. env.txt as it was is put back after the run.
# A run that an adb call fails or stalls in (a timeout) is put back as well (the game stopped, its folder removed, env.txt as found) and recorded as a bench error; the rest go on.
# Devices stay awake: every 45 s each one gets KEYCODE_WAKEUP (wakes a screen that went dark, does nothing to one that is on, and never turns it off, unlike POWER),
# and the lock screen is dismissed before each start. A PIN or pattern lock cannot be dismissed over adb: set the test devices' screen lock to None or Swipe.
# Save sweeps: --sweep runs every route named dl_* (a downloaded late-game save's scene) of each game given, --routes <patterns> the routes matching comma-separated shell
#   patterns; one run each unless --runs says otherwise. They write sweep.md (one table per phone: load to the scene, fps, frame p50/p95, GPU, PSS, red lines and their
#   kinds, how it ended, the end screenshot) and sweep.json; --compare <earlier sweep folder or sweep.json> flags a route whose fps fell 5 % or more, that has red-line kinds
#   the earlier one hadn't, or that crashed or was killed where it hadn't. A downloaded save never moves a game up a tier (dm/android/README.md, Save sweeps).
# Out: <out>/<device>/<title ID>_<route>_r<N>/ (logcat.txt, log.txt, frames.tsv, the screenshots, result.json), and results.json and results.md for all runs.
# Default <out>: bench/<date-time> in build_android_from_nsp.py's data folder (~/.local/share/nxnative/android on a Mac or Linux), beside its builds journal.
# Each device it uses is locked for the whole run (dm/android/tools/device_lock.py): a device another bench or install holds is refused with who holds it, or waited
#   for with --wait-device <s>. Without -s, busy devices are left out. NXNATIVE_DEVICE_HOLDER names this run in what others are told.
import argparse, datetime, json, math, os, re, shlex, shutil, subprocess, sys, threading, time
from pathlib import Path
from typing import Any

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / 'dm/android/tools'))
import device_lock, phone_id   # noqa: E401, E402  # pyright: ignore[reportMissingImports]   (dm/android/tools, put on the path above)
import bench_sweep   # noqa: E402  # pyright: ignore[reportMissingImports]
PKG = 'org.nxnative.android'
EXT = f'/storage/emulated/0/Android/data/{PKG}/files'
GAME_PROC = PKG + ':game'
RUNS = f'{EXT}/bench_runs'   # the runs' save folders (run_folder)
TSV = f'{EXT}/bench_frames.tsv'   # the per-frame timing (DM_NVN_TSV)
ADB = shutil.which('adb') or str(Path(os.environ.get('ANDROID_HOME', Path.home() / 'Library/Android/sdk')) / 'platform-tools/adb')
TIMING = ('@t_s', '@frame_ms', '@gpu_ms', '@work_ms', '@draws')


def device_folder(d):   # a device's runs under <out>: <model>_<serial>, safe on Windows too (a Wi-Fi device's serial has a ':', phone_id.py)
    return phone_id.safe(f"{d['model'].replace(' ', '_')}_{d['serial']}")


def data_dir():   # build_android_from_nsp.py's: the library of game hosts, its builds journal, and these results
    import data_home   # pyright: ignore[reportMissingImports]  (dm/android/tools, put on the path above)
    return data_home.data_dir()


def adb_bytes(serial, *args, timeout=120) -> bytes:
    return subprocess.run([ADB, '-s', serial, *args], capture_output=True, timeout=timeout).stdout


def adb(serial, *args, timeout=120) -> str:
    return adb_bytes(serial, *args, timeout=timeout).decode(errors='replace')


def sh(serial, cmd, timeout=60) -> str:
    return adb(serial, 'shell', cmd, timeout=timeout)


_displays: dict[str, list[tuple[str, str]]] = {}   # serial -> SurfaceFlinger's (display ID, HWC display) pairs


def display_states(dump: str) -> dict[str, str]:
    # `dumpsys display`'s Display Devices section: each screen's mUniqueId (local:<SurfaceFlinger display ID>), then its mState (ON, OFF, DOZE, ...). Other sections
    # have mState lines of their own (the brightness controller's), so the walk stops at the next top-level heading (the heading's own underline of dashes aside)
    states: dict[str, str] = {}; cur = None; inside = False
    for line in dump.splitlines():
        if line.startswith('Display Devices:'): inside = True; continue
        if not inside: continue
        if line and not line[0].isspace() and line.strip('-'): break
        m = re.match(r'\s+mUniqueId=local:(\d+)', line)
        if m: cur = m.group(1); continue
        m = re.match(r'\s+mState=(\w+)', line)
        if m and cur and cur not in states: states[cur] = m.group(1)
    return states


def shot_display(ids: list[tuple[str, str]], states: dict[str, str]) -> str | None:
    # The screen to capture on a phone with more than one: the one that is on. A Galaxy Z Fold 8 folded, or a Flip closed, shows the game on its cover screen and turns the
    # inner one off, so HWC display 0 (the inner screen) alone gave black shots. With none on, or both, HWC display 0, the screen the game opens on when unfolded
    if len(ids) < 2: return None
    on = [d for d, _ in ids if states.get(d, '').startswith('ON')]   # ON, or ON_SUSPEND
    if len(on) == 1: return on[0]
    first = [d for d, hwc in ids if hwc == '0' and (not on or d in on)] or on
    return first[0] if first else None


PNG_SIGNATURE = b'\x89PNG\r\n\x1a\n'


def png_only(out: bytes) -> bytes:
    # screencap writes its warnings to stdout too, ahead of the PNG: a Galaxy Z Fold 8 (Android 17) asked without -d gives ~350 bytes of "[Warning] Multiple displays
    # were found, ..." first, and the file is neither a PNG to `file` nor one PIL opens. Whatever comes before the signature goes; output with no PNG in it is kept
    # as it is, so what screencap said instead is still there to read
    at = out.find(PNG_SIGNATURE)
    return out[at:] if at > 0 else out


def screenshot(serial) -> bytes:
    # A phone with a second screen needs screencap's -d: without it screencap writes a warning ahead of the PNG and takes whichever display it finds first. Which
    # screen is on is asked at each shot, so folding or unfolding the phone during a bench is followed; one screen, no -d, as before. A display list that came back
    # empty (adb or dumpsys failing at that moment) is asked for again at the next shot rather than kept for the whole run, which would leave every shot without -d
    if not _displays.get(serial):
        _displays[serial] = re.findall(r'^Display (\d+) \(HWC display (\d+)\)', sh(serial, 'dumpsys SurfaceFlinger --display-id'), re.M)
    d = shot_display(_displays[serial], display_states(sh(serial, 'dumpsys display')) if len(_displays[serial]) > 1 else {})
    return png_only(adb_bytes(serial, 'exec-out', 'screencap', '-p', *(['-d', d] if d else [])))


def ready_devices():
    out = subprocess.run([ADB, 'devices'], capture_output=True, text=True).stdout.splitlines()[1:]
    return [line.split()[0] for line in out if len(line.split()) > 1 and line.split()[1] == 'device']


def phone_key(serial):   # the phone's stable ID (dm/android/tools/phone_id.py), or its serial: its lock is named after it, as build_android_from_nsp.py names it
    try: return phone_id.from_props(sh(serial, phone_id.ID_CMD, timeout=15)) or serial
    except (OSError, subprocess.SubprocessError): return serial


def device_info(serial):
    props = dict(re.findall(r'^\[([^\]]+)\]: \[([^\]]*)\]', sh(serial, 'getprop'), re.M))
    mem = re.search(r'MemTotal:\s+(\d+)', sh(serial, 'cat /proc/meminfo'))
    gles = re.search(r'GLES: ([^\n]+)', sh(serial, 'dumpsys SurfaceFlinger | grep -m1 GLES:'))
    return {'serial': serial, 'maker': props.get('ro.product.manufacturer', ''), 'model': props.get('ro.product.model', ''),
            'soc': props.get('ro.soc.model') or props.get('ro.board.platform', ''), 'android': props.get('ro.build.version.release', ''),
            'sdk': props.get('ro.build.version.sdk', ''), 'ram_gb': round(int(mem.group(1)) / 1048576, 1) if mem else None,
            'gles': gles.group(1).strip() if gles else ''}


def thermal(serial):   # → (thermal status, battery °C)
    st = re.search(r'Thermal Status: (\d+)', sh(serial, 'dumpsys thermalservice'))
    bt = re.search(r'temperature: (\d+)', sh(serial, 'dumpsys battery'))
    return (int(st.group(1)) if st else None, int(bt.group(1)) / 10 if bt else None)


def cool_down(serial, temp, most):   # → seconds waited
    t0 = time.time()
    while time.time() - t0 < most:
        st, bt = thermal(serial)
        if (st or 0) == 0 and (bt is None or bt <= temp): break
        time.sleep(15)
    return round(time.time() - t0)


class KeepAwake(threading.Thread):
    def __init__(self, serials):
        super().__init__(daemon=True); self.serials = serials; self.stop = threading.Event()

    def run(self):
        while not self.stop.is_set():
            for s in self.serials:
                try: sh(s, 'input keyevent KEYCODE_WAKEUP', timeout=15)
                except subprocess.TimeoutExpired: pass
            self.stop.wait(45)


def routes(gid):
    f = REPO / 'dm/games' / gid / 'bench_android.tsv'
    out = []
    if f.is_file():
        for line in f.read_text(encoding='utf-8').splitlines():
            if not line.strip() or line.startswith('#'): continue
            c = line.split('\t') + ['-'] * 8
            out.append({'name': c[0], 'keys': '' if c[1] == '-' else c[1], 'save': '' if c[2] == '-' else c[2], 'start': float(c[3]), 'length': float(c[4]),
                        'ready': '' if c[5] == '-' else c[5], 'about': '' if c[6] == '-' else c[6], 'env': [] if c[7] in ('-', '') else c[7].split(';')})
    return out or [{'name': 'boot', 'keys': '', 'save': '', 'start': 20.0, 'length': 30.0, 'ready': '', 'about': 'no input, no save', 'env': []}]


def game_of(folder):   # → (title ID, game record)
    builds = sorted(Path(folder).glob('*_build/android_game.json'))
    if not builds: sys.exit(f'{folder}: no <title ID>_build/android_game.json (pack the game with build_android_from_nsp.py first)')
    g = json.loads(builds[0].read_text(encoding='utf-8'))
    return g['title_id'].upper(), g


def pct(xs, p):   # nearest rank
    if not xs: return None
    s = sorted(xs); return round(s[min(len(s) - 1, max(0, math.ceil(p / 100 * len(s)) - 1))], 2)


def frames(tsv):   # → {frame: {name: value}}
    rows = {}
    for line in tsv.splitlines():
        c = line.split('\t')
        if len(c) == 3 and c[1] in TIMING:
            try: rows.setdefault(int(c[0]), {})[c[1]] = float(c[2])
            except ValueError: pass
    return rows


def window_stats(rows, start, length):
    w = [r for r in rows.values() if '@t_s' in r and start <= r['@t_s'] < start + length]
    ft = [r['@frame_ms'] for r in w if '@frame_ms' in r]
    gpu = [r['@gpu_ms'] for r in w if '@gpu_ms' in r]
    return {'frames': len(w), 'fps': round(len(w) / length, 1) if length else None, 'frame_p50': pct(ft, 50), 'frame_p95': pct(ft, 95), 'frame_p99': pct(ft, 99),
            'frame_max': round(max(ft), 1) if ft else None, 'over_17ms_pct': round(100 * sum(x > 17.5 for x in ft) / len(ft), 1) if ft else None,
            'over_34ms_pct': round(100 * sum(x > 34 for x in ft) / len(ft), 1) if ft else None, 'gpu_p50': pct(gpu, 50), 'gpu_p95': pct(gpu, 95),
            'draws_p50': pct([r['@draws'] for r in w if '@draws' in r], 50)}


# The low-memory killer ended the game: a line of lmkd's that names org.nxnative.android:game ("Kill" in AOSP, "Reclaim" on the Galaxy S25 Edge) or the game's pid
# (process_mrelease <pid> after a kill). Any other of its lines is about another app: under memory pressure it kills dozens (the launcher's own process,
# org.nxnative.android, among them), and those don't end the run. Its tag is lmkd on the Odin 2 and the Galaxy S25 Edge (whose process_mrelease errors come under
# lowmemorykiller), AOSP's lowmemorykiller on the Pixel 9 Pro XL, so the run's logcat takes both
LMK_LINE = re.compile(r'\b(?:lmkd|lowmemorykiller)\s*[:(]')


def lmk_killed_game(line, pid):
    if not LMK_LINE.search(line): return False
    if re.search(r"(?:Kill|Reclaim) '" + re.escape(GAME_PROC) + r"' \(", line): return True
    return bool(pid) and re.search(r'process_mrelease ' + str(pid) + r'\b', line) is not None


def log_facts(logcat, ready):
    def epoch(line): m = re.match(r'\s*(\d+\.\d+)', line); return float(m.group(1)) if m else None
    # shift: add to the host's times (@t_s, the log's [N s]) to make them seconds from the launch. The host counts from its process's start, and Android 16 starts
    # app processes ahead of use (the OnePlus Pad 3's ran 83 s early), so a line holding both clocks (an fs open, DM_FS=1) gives the offset to ActivityManager's start
    lines = logcat.splitlines(); start = None; pid = None; out: dict[str, Any] = {'ready_s': None, 'exit': None, 'vulkan': None, 'resident_mb': None, 'shift': 0.0}; anchored = False
    for line in lines:
        if start is None and 'Start proc' in line and GAME_PROC in line: start = epoch(line)
        m = re.search(r'Start proc (\d+):' + re.escape(GAME_PROC) + '/', line)
        if m and pid is None: pid = m.group(1)
        m = re.search(r'\[dm\] vulkan: (.+)$', line)
        if m: out['vulkan'] = m.group(1).strip()
        m = re.search(r'\[vk\] frame \d+: resident (\d+) MB', line)
        if m: out['resident_mb'] = int(m.group(1))
        t = epoch(line)
        m = re.search(r'fs open \[([0-9.]+)s\]', line)
        if m and not anchored and start is not None and t is not None: out['shift'] = round(t - float(m.group(1)) - start, 3); anchored = True
        if ready and out['ready_s'] is None and start is not None and t is not None and re.search(ready, line): out['ready_s'] = round(t - start, 2)
        if 'Fatal signal' in line and out['exit'] is None: out['exit'] = 'crash: ' + line.split('Fatal signal', 1)[1].strip()[:120]
        m = re.search(r'Process \d+ exited cleanly \((\d+)\)', line)
        if m and m.group(1) != '0' and out['exit'] is None: out['exit'] = 'exit ' + m.group(1) + (' (svcBreak: the game aborted)' if m.group(1) == '42' else '')   # 0: the stop at the run's end
        if out['exit'] is None and lmk_killed_game(line, pid): out['exit'] = 'killed for memory'
    return out


# The ready line from the host's own log when logcat lost it: Android 16 drops lines from an app that logs fast (Link's Awakening logs each file open twice on the
# OnePlus Pad 3), and log.txt keeps them all. Only lines carrying the host's clock (fs open [N s]) give a time; shift turns it into seconds from the launch
def ready_in_log(text, ready, shift):
    for line in text.splitlines():
        m = re.search(r'\[([0-9.]+)s\]', line)
        if m and re.search(ready, line): return round(float(m.group(1)) + shift, 2)
    return None


# Is the game still running? An empty pidof is not an answer: on the Odin 2 (Android 13), 85-115 s into Ocarina of Time's Kokiri runs with the device out of
# memory (lmkd killing everything else, ActivityManager restarting services "for mem-pressure-event"), pidof came back empty while the game was still logging, and
# the run counted a death and force-stopped a live game. So each check has the shell echo a marker after pidof (no marker: adb itself didn't answer), and an empty
# answer counts as a death only when a second, independent check says so: the pid last seen gone from /proc (or another process's now, by its start time), or the
# run's logcat saying the game's process ended. Anything less is "unknown": the run carries on, nothing is stopped, and the miss is printed and kept in result.json
GAME_DIED = (re.compile(r'ActivityManager\s*: Process ' + re.escape(GAME_PROC) + r' \(pid (\d+)\) has died'),   # the launcher's own process is org.nxnative.android
             re.compile(r"(?:lmkd|lowmemorykiller)\s*: (?:Kill|Reclaim) '" + re.escape(GAME_PROC) + r"' \((\d+)\)"),   # lmkd's two tags (LMK_LINE)
             re.compile(r'Zygote\s*: Process (\d+) exited'))   # no name on this one: by the pid alone


def game_pid(serial):   # the game's pid; '' when the shell ran pidof and it found none; None when the shell didn't answer (adb failed or timed out)
    try: out = sh(serial, f'pidof {GAME_PROC}; echo pidof-done', timeout=20)
    except subprocess.TimeoutExpired: return None
    if 'pidof-done' not in out: return None
    pids = out.split('pidof-done')[0].split()
    return pids[0] if pids else ''


def proc_start(serial, pid):   # /proc/<pid>'s start time (stat field 22); '' when the shell answered and there is no such process; None when it didn't answer
    try: out = sh(serial, f'cat /proc/{pid}/stat 2>/dev/null; echo stat-done', timeout=20)
    except subprocess.TimeoutExpired: return None
    if 'stat-done' not in out: return None
    st = out.split('stat-done')[0].strip(); f = st.rsplit(')', 1)[-1].split() if st.startswith(f'{pid} (') else []
    return f[19] if len(f) > 19 and f[0] != 'Z' else ''   # a zombie (state Z) has ended too


def died_in_logcat(text, pid):   # the run's logcat says the game's process ended (with no pid seen yet, only the lines that name it)
    for line in text.splitlines():
        for p in GAME_DIED:
            m = p.search(line)
            if m and (m.group(1) == pid if pid else p is not GAME_DIED[2]): return True
    return False


class GameWatch:   # one run's game process, checked once a second
    def __init__(self, serial, logcat):
        self.serial, self.logcat = serial, logcat   # logcat: a function giving the run's logcat so far
        self.pid: str | None = None; self.start: str | None = None; self.misses: list[dict[str, Any]] = []; self.missing = False   # missing: in a run of misses

    def check(self, el):   # 'alive', 'dead' or 'unknown'
        pid = game_pid(self.serial)
        if pid:
            if pid != self.pid: self.pid, self.start = pid, proc_start(self.serial, pid) or None
            self.missing = False; return 'alive'
        if self.pid:
            st = proc_start(self.serial, self.pid)
            if st and st == (self.start or st): state = 'alive'
            elif st is not None: return 'dead'   # the kernel's own answer: its pid is gone, or another process's now
            else: state = 'dead' if died_in_logcat(self.logcat(), self.pid) else 'unknown'
        else: state = 'dead' if died_in_logcat(self.logcat(), None) else 'unknown'
        if state != 'dead':
            how = 'empty' if pid == '' else 'no answer'; self.misses.append({'t_s': round(el, 1), 'pidof': how, 'game': state, 'pid': self.pid})
            if not self.missing: print(f'  {self.serial} at {el:.0f} s: pidof came back {how}, ' + (f'but the game (pid {self.pid}) is alive' if state == 'alive' else
                                       'and nothing confirms a death') + ': not counted, nothing stopped', flush=True)   # once a run of misses: all go in result.json
            self.missing = True
        return state


def run_one(serial, folder, gid, route, n, out, args):
    d = out / f'{gid}_{route["name"]}_r{n}'; d.mkdir(parents=True, exist_ok=True)
    sh(serial, f'am force-stop {PKG}')
    waited = cool_down(serial, args.cool_temp, args.cool_max)
    st0, bt0 = thermal(serial)
    env_before = sh(serial, f'cat {EXT}/env.txt 2>/dev/null')
    # A folder of its own per run: what the game writes there is its own (drwxr-x---), which the shell can read but never delete, so a reused folder would hold the
    # last run's save. bench_runs is made by mkdir under files/, so it is setgid ext_data_rw like files/, and only symbolic modes are given it below (a numeric one
    # clears setgid, and a chmod cannot set it again on this storage)
    run = run_folder(gid); PENDING[serial] = {'run': run, 'env': env_before, 'dir': d}
    env = ['# written by scripts/bench_android.py for a benchmark run; put back as it was when the run ends', f'DM_SAVE={run}/save/{gid}',
           f'DM_NVN_TSV={TSV}', 'DM_NVN_TSV_CALLS=0', 'DM_FS=1'] + ([f'DM_KEYS={route["keys"]}'] if route['keys'] else []) + route['env'] + args.env   # DM_FS: file opens in the log, for ready patterns
    # bench_runs itself opened too (symbolically, keeping setgid), as build_android_from_nsp.py opens games/'s parent: the app is not in ext_data_rw on Android 16
    # (the Pad 3), so a drwxrws--- bench_runs kept it out of the save folder inside (errno 13), where the Odin 2's let it in
    sh(serial, f'mkdir -p {run}/save; chmod a+rwx {RUNS}')
    if route['save']:
        try: src = bench_sweep.save_dir(route['save'], folder, out.parent / 'saves')
        except (OSError, ValueError, subprocess.CalledProcessError) as e: sys.exit(f'route {route["name"]}: {e}')
        # Folders made by the shell, files pushed one by one: a folder adb push makes lists empty on the Odin 2's storage (its files are there by path, but a directory
        # read misses them, and the host copies the save by reading it)
        dirs = sorted({os.path.relpath(r, src) for r, _, _ in os.walk(src)} - {'.'})
        sh(serial, 'mkdir -p ' + ' '.join(shq(f'{run}/save/{gid}/{x}') for x in ['.'] + dirs))
        for r, _, fs in os.walk(src):
            for f in fs:
                if f.startswith('.'): continue   # .DS_Store and the like
                rel = os.path.relpath(os.path.join(r, f), src); adb(serial, 'push', os.path.join(r, f), f'{run}/save/{gid}/{rel}', timeout=300)
    # The save copy is made by adb's shell user, which the game cannot write to on an enforcing device (Android 16 on the OnePlus Pad 3): opened to all, as
    # build_android_from_nsp.py opens games/, with a symbolic mode, which keeps the folders' setgid bit (a numeric 777 clears it): what the game makes there
    # then has group ext_data_rw, which the shell reads (--keep-save), not the app's own. The timing file goes in files/ itself, setgid ext_data_rw as well
    sh(serial, f'chmod -R a+rwX {run}; rm -f {TSV}')
    adb(serial, 'push', str(write_tmp(d / 'env.txt', '\n'.join(env) + '\n')), f'{EXT}/env.txt')
    adb(serial, 'logcat', '-c')
    with open(d / 'logcat.txt', 'wb') as lf:
        lc = subprocess.Popen([ADB, '-s', serial, 'logcat', '-v', 'epoch', 'DroidPlay:V', 'nxnative:V', 'ActivityManager:I', 'Zygote:I', 'DEBUG:V', 'libc:F', 'lmkd:I', 'lowmemorykiller:I', '*:S'], stdout=lf)
        PENDING[serial]['logcat'] = lc
        sh(serial, 'input keyevent KEYCODE_WAKEUP; wm dismiss-keyguard')
        t0 = time.time()
        sh(serial, f'am start -n {PKG}/.Launcher --es game {gid} --ez play true')
        shots = {route['start']: 'start', route['start'] + route['length']: 'end'}; died = None; pss = None
        watch = GameWatch(serial, lambda: (d / 'logcat.txt').read_text(errors='replace'))
        if args.shot_every: shots.update({float(t): f'{t:04d}s' for t in range(args.shot_every, int(route['start'] + route['length']), args.shot_every) if float(t) not in shots})
        end = route['start'] + route['length'] + 3
        while time.time() - t0 < end:
            time.sleep(1); el = time.time() - t0
            for at, name in list(shots.items()):
                if el >= at: (d / f'shot_{name}.png').write_bytes(screenshot(serial)); del shots[at]
            if pss is None and el >= route['start'] + route['length'] - 2:
                m = re.search(r'TOTAL PSS:\s+(\d+)|TOTAL\s+(\d+)', sh(serial, f'dumpsys meminfo {GAME_PROC}')); pss = int(m.group(1) or m.group(2)) // 1024 if m else None
            if el > 15 and watch.check(el) == 'dead': died = round(el, 1); break
        st1, bt1 = thermal(serial)
        sh(serial, f'am force-stop {PKG}'); time.sleep(1); lc.terminate()
    (d / 'frames.tsv').write_bytes(adb_bytes(serial, 'exec-out', f'cat {TSV}'))
    if args.keep_save:   # the save the run ended with, as a save folder for later routes (the game's last commit; its .work copy is left)
        keep = Path(args.keep_save); tmp = keep.with_name(keep.name + '.pulling'); shutil.rmtree(tmp, ignore_errors=True); keep.parent.mkdir(parents=True, exist_ok=True)
        adb(serial, 'pull', f'{run}/save/{gid}', str(tmp), timeout=600)
        if tmp.is_dir() and any(tmp.iterdir()): shutil.rmtree(keep, ignore_errors=True); tmp.rename(keep); print(f'  save kept in {keep}', flush=True)
        else: print(f'  no save to keep: the game made none on {serial}', flush=True)
    remove_run(serial, run)
    (d / 'log.txt').write_bytes(adb_bytes(serial, 'exec-out', f'cat {EXT}/log.txt'))
    put_env_back(serial, env_before, d); PENDING.pop(serial, None)
    facts = log_facts((d / 'logcat.txt').read_text(errors='replace'), route['ready'])
    if route['ready'] and facts['ready_s'] is None: facts['ready_s'] = ready_in_log((d / 'log.txt').read_text(errors='replace'), route['ready'], facts['shift'])
    rows = frames((d / 'frames.tsv').read_text(errors='replace'))
    for r in rows.values():
        if '@t_s' in r: r['@t_s'] += facts['shift']
    first = min((r['@t_s'] for r in rows.values() if '@t_s' in r), default=None)
    res = {'serial': serial, 'game': gid, 'route': route['name'], 'run': n, 'first_frame_s': round(first, 2) if first is not None else None, **facts,
           **window_stats(rows, route['start'], route['length']), 'pss_mb': pss, 'died_at_s': died, 'cooled_s': waited,
           'thermal': [st0, st1], 'battery_c': [bt0, bt1], 'pidof_misses': watch.misses, 'red_kinds': bench_sweep.red_kinds((d / 'log.txt').read_text(errors='replace')),
           'shot': str(d / 'shot_end.png') if (d / 'shot_end.png').is_file() else None, 'dir': str(d)}
    if died and not res['exit']: res['exit'] = f'stopped at {died} s'
    if watch.pid is None and not died and not res['exit']: res['exit'] = 'never seen running'   # an empty pidof all through, which no longer counts as a death on its own
    (d / 'result.json').write_text(json.dumps(res, indent=1), encoding='utf-8')
    return res


# Each run removes its own folder when it ends, and no other: bench_runs also holds other sessions' folders on a shared test device (a run whose save is still being
# read, a hand run's DM_SAVE), which emptying it at each start took from under them. As far as the shell can: what the game wrote there may be its own, and goes
# with the app
def run_folder(gid):
    return f'{RUNS}/{gid}_{int(time.time())}'


PENDING: dict[str, dict[str, Any]] = {}   # serial → the run's folder, env.txt as found, its output folder and logcat: what a run that stopped part-way has to put back


def put_env_back(serial, env_before, d):
    if env_before.strip(): adb(serial, 'push', str(write_tmp(d / 'env_before.txt', env_before)), f'{EXT}/env.txt')
    else: sh(serial, f'rm -f {EXT}/env.txt')


def tidy(serial):   # after a run raised part-way (an adb call that stalled): the game stopped, the run's folder removed, env.txt put back; best effort, as adb may still be down
    p = PENDING.pop(serial, None)
    if p is None: return
    if p.get('logcat'): p['logcat'].terminate()
    try: sh(serial, f'am force-stop {PKG}'); remove_run(serial, p['run']); put_env_back(serial, p['env'], p['dir'])
    except (subprocess.SubprocessError, OSError) as e: print(f"  could not tidy up {serial} ({type(e).__name__}): its run folder {p['run']} and env.txt may be left", flush=True)


def failed_run(serial, gid, route, n, e):   # the result of a run that raised: every field report() and the sweep table read, the error as how it ended
    why = f'adb timed out after {e.timeout:g} s' if isinstance(e, subprocess.TimeoutExpired) else type(e).__name__
    return {'serial': serial, 'game': gid, 'route': route['name'], 'run': n, 'battery_c': [None, None], 'red_kinds': {}, 'shot': None, 'dir': None, 'exit': f'bench error: {why}',
            **dict.fromkeys(('first_frame_s', 'ready_s', 'fps', 'frame_p50', 'frame_p95', 'frame_p99', 'over_17ms_pct', 'over_34ms_pct', 'gpu_p50', 'pss_mb', 'died_at_s'))}


def remove_run(serial, run):
    name = run[len(RUNS) + 1:]
    if not run.startswith(RUNS + '/') or not name or '/' in name or name in ('.', '..') or '*' in name:   # one run's folder, never bench_runs or a pattern
        raise ValueError(f'not a run folder: {run}')
    sh(serial, f'rm -rf {shq(run)} 2>/dev/null')


def shq(x):   # a path for the device's shell
    return shlex.quote(x)


def write_tmp(p, text):
    p.write_text(text, encoding='utf-8'); return p


def report(out, devices, results):
    (out / 'results.json').write_text(json.dumps({'devices': devices, 'runs': results}, indent=1), encoding='utf-8')
    names = {d['serial']: f"{d['maker']} {d['model']}" for d in devices}
    def f(v): return '–' if v is None else str(v)
    lines = ['| Device | Game | Route | Run | First frame s | Ready s | FPS | Frame p50 / p95 / p99 ms | > 17.5 ms % | > 34 ms % | GPU p50 ms | PSS MB | Battery °C | Exit |',
             '|---|---|---|---|---|---|---|---|---|---|---|---|---|---|']
    for r in results:
        lines.append(f"| {names.get(r['serial'], r['serial'])} | {r['game']} | {r['route']} | {r['run'] or 'warm-up'} | {f(r['first_frame_s'])} | {f(r['ready_s'])} | {f(r['fps'])} | "
                     f"{f(r['frame_p50'])} / {f(r['frame_p95'])} / {f(r['frame_p99'])} | {f(r['over_17ms_pct'])} | {f(r['over_34ms_pct'])} | {f(r['gpu_p50'])} | "
                     f"{f(r['pss_mb'])} | {f(r['battery_c'][0])} → {f(r['battery_c'][1])} | {r['exit'] or 'ran'} |")
    dev = ['', '| Device | SoC | Android | RAM GB | Vulkan (from the host) |', '|---|---|---|---|---|']
    for d in devices:
        vk = next((r['vulkan'] for r in results if r['serial'] == d['serial'] and r.get('vulkan')), None) or d['gles']
        dev.append(f"| {names[d['serial']]} | {d['soc']} | {d['android']} | {f(d['ram_gb'])} | {vk} |")
    (out / 'results.md').write_text('\n'.join(lines + dev) + '\n', encoding='utf-8')


def main():
    ap = argparse.ArgumentParser(description='Baseline runs of games on Android devices over adb (see the comment at the top)')
    ap.add_argument('folders', nargs='+', help='NSP folders packed by build_android_from_nsp.py')
    ap.add_argument('-s', '--serial', action='append', help='an adb device (repeatable; default: every ready one)')
    ap.add_argument('--route', help="a route name of the game's bench_android.tsv (default: its first)")
    ap.add_argument('--runs', type=int, help='runs of each route (default 2; 1 for --sweep and --routes)'); ap.add_argument('--install', action='store_true', help='build_android_from_nsp.py --push-only to each device first')
    ap.add_argument('-k', '--keys', default=str(Path.home() / '.switch/prod.keys'), help='prod.keys for --install (default ~/.switch/prod.keys)')
    ap.add_argument('--warmup', action='store_true', help='a warm-up run (run 0) before the measured ones, reported apart: the first launch after an install builds pipelines')
    ap.add_argument('--shot-every', type=int, default=0, help='also a screenshot every this many seconds (for working out a route)')
    ap.add_argument('--keep-save', help="after each run, copy the save it ended with to this folder (a run that plays into a game makes a route's save)")
    ap.add_argument('--out'); ap.add_argument('--env', action='append', default=[], help='KEY=VALUE for every run (repeatable)')
    ap.add_argument('--cool-temp', type=float, default=38.0, help='start a run once the battery is at or under this many °C (default 38)')
    ap.add_argument('--cool-max', type=int, default=300, help='wait at most this many seconds to cool down (default 300)')
    ap.add_argument('--wait-device', type=float, default=0, help='wait up to this many seconds for a device another bench or install holds (default: refuse at once)')
    ap.add_argument('--sweep', action='store_true', help="every dl_* route of each game: the downloaded saves' scenes, one summary table per phone (sweep.md)")
    ap.add_argument('--routes', help='the routes matching these comma-separated shell patterns (dl_*,boot), summarised like --sweep')
    ap.add_argument('--compare', help='an earlier sweep (its folder or sweep.json): flag fps 5 %% lower, new red-line kinds, a crash or kill')
    args = ap.parse_args()
    sweep = args.sweep or bool(args.routes)
    if args.compare and not sweep: ap.error('--compare goes with --sweep or --routes')
    if args.route and sweep: ap.error('--route is one route; --sweep and --routes pick several')
    runs = args.runs if args.runs is not None else (1 if sweep else 2)
    try: earlier = bench_sweep.load(args.compare) if args.compare else None   # read before any run, so a wrong path costs nothing
    except (OSError, ValueError, KeyError) as e: ap.error(f'--compare {args.compare}: {e}')
    serials = args.serial or ready_devices()
    if not serials: sys.exit('no adb device ready')
    what = 'bench_android.py: ' + ', '.join(Path(f).expanduser().resolve().parent.name for f in args.folders) + (f' ({args.route})' if args.route else '')
    keys = {s: phone_key(s) for s in serials}   # a phone on a cable and on Wi-Fi at once is one device: run once, over the cable, under one lock
    serials = [d['serial'] for d in phone_id.one_per_phone([{'serial': s, 'id': keys[s]} for s in serials])]
    for s in list(serials):   # each device for the whole run: another bench's or an install's force-stop and env.txt would void this one's runs, and the reverse
        try: device_lock.acquire(keys[s], what, args.wait_device, say=lambda m: print(m, flush=True))
        except device_lock.DeviceBusy as e:
            if args.serial: sys.exit(str(e))
            print(f'left out: {e}', flush=True); serials.remove(s)
    if not serials: sys.exit('every ready device is in use')
    out = Path(args.out or data_dir() / 'bench' / datetime.datetime.now().strftime('%Y%m%d-%H%M%S')); out.mkdir(parents=True, exist_ok=True)
    devices = [device_info(s) for s in serials]
    for d in devices: print(f"device {d['serial']}: {d['maker']} {d['model']}, {d['soc']}, Android {d['android']}, {d['ram_gb']} GB", flush=True)
    awake = KeepAwake(serials); awake.start(); results = []
    try:
        for folder in args.folders:
            gid, g = game_of(folder)
            rs = routes(gid)
            if sweep:
                picked = bench_sweep.pick_routes(rs, args.routes or 'dl_*')
                if not picked: print(f"{g.get('name', gid)}: no route matches {args.routes or 'dl_*'} in dm/games/{gid}/bench_android.tsv", flush=True); continue
            else:
                route = next((r for r in rs if r['name'] == args.route), None) if args.route else rs[0]
                if not route: sys.exit(f'{gid}: no route {args.route} in dm/games/{gid}/bench_android.tsv')
                picked = [route]
            for d in devices:
                if args.install:
                    print(f"{gid} → {d['model']}: installing", flush=True)
                    subprocess.run([sys.executable, str(REPO / 'scripts/build_android_from_nsp.py'), folder, '-k', args.keys, '--push-only', '--serial', d['serial'], '--progress', 'lines'], check=True)
                for route in picked:
                    for n in range(0 if args.warmup else 1, runs + 1):   # run 0: the warm-up, the first launch after an install (pipelines built, files cached)
                        print(f"{g.get('name', gid)} ({route['name']}) on {d['model']}, {'warm-up' if n == 0 else f'run {n}'}", flush=True)
                        try: r = run_one(d['serial'], folder, gid, route, n, out / device_folder(d), args)
                        except (subprocess.SubprocessError, OSError) as e:   # one stalled adb call costs this run, not the rest of the sweep
                            tidy(d['serial']); r = failed_run(d['serial'], gid, route, n, e)
                        except SystemExit: tidy(d['serial']); raise
                        results.append(r)
                        print(f"  first frame {r['first_frame_s']} s, ready {r['ready_s']} s, {r['fps']} fps, frame p50 {r['frame_p50']} / p95 {r['frame_p95']} ms, "
                              f"GPU p50 {r['gpu_p50']} ms, {sum(r['red_kinds'].values())} red lines, {r['exit'] or 'ran'}", flush=True)
                        report(out, devices, results)
    finally:
        awake.stop.set()
    print(f'results: {out / "results.md"}')
    if sweep and results:
        names = {d['serial']: f"{d['maker']} {d['model']}" for d in devices}
        reg = bench_sweep.write(out, bench_sweep.summarize(results), names, earlier)
        print(f'sweep: {out / "sweep.md"}' + (f', {len(reg)} route(s) flagged against {args.compare}' if args.compare else ''))


if __name__ == '__main__':
    main()
