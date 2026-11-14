# Pack a Switch game you own for Android: your NSP / XCI files and your prod.keys in, the nxnative app and the game files on the phone out.
#   python scripts/build_android_from_nsp.py <NSP folder> -k <prod.keys> [-p <version>] [-l <language>] [--serial <adb device>] [--no-push]
#   The folder may instead hold the game as one NUP container (a .nup file: base game, updates and DLC), or as a dump folder (scripts/pack_dump.py), or the .nup file may
#   be given in its place: neither needs prod.keys (build_ipa_from_nsp.py reads them, as for iOS)
#   python scripts/build_android_from_nsp.py <NSP folder> -k <prod.keys> --push-only [--serial ...]   install the app and copy a game built before, without building
#   python scripts/build_android_from_nsp.py --apk-only [--serial ...]                                  the app with every game host built on this computer, installed
#   python scripts/build_android_from_nsp.py --migrate --title <title ID> [--serial ...]               a one-game install's folder (files/game) into files/games/<title ID>
#   python scripts/build_android_from_nsp.py --import-save <folder | archive> --title <title ID> [--serial ...]   a save for that game, swapped in by the app after it asks
#   python scripts/build_android_from_nsp.py --restore-backup <backup folder> [--title <title ID>] [--serial ...]   a game's save (or every game's) from Back up saves' copy
#   python scripts/build_android_from_nsp.py --check | --info <NSP folder>                              requirements / the game's details, as JSON (the desktop app)
#   python scripts/build_android_from_nsp.py --setup [--accept-android-licenses]                         install what --check finds missing (Windows: dm/android/tools/host_setup.py)
#   python scripts/build_android_from_nsp.py <NSP folder> -k <prod.keys> --game-apk [--xapk] [--out <folder>]   the game in an app of its own, to install anywhere (make_game_app)
#   --progress json: newline-delimited JSON events on stdout (build_ipa_from_nsp.py Progress), for the desktop app (scripts/nxnative_android.py)
#   Every build also writes those events to a journal, <data>/builds/<time>-<pid>.jsonl (start and exit events around them), whatever its --progress: the desktop app
#   shows a build started from a console too
# One app carries every game: its launcher (dm/android/java Launcher.java) lists the games on the phone, Android/data/org.nxnative.android/files/games/<title ID>/,
# and the app holds a host per game, lib/arm64-v8a/libdmhost_<title ID>.so, started by its loader (dm/android/loader.c). The hosts built on this computer are kept in a
# library (<data>/hosts/<title ID>/: %LOCALAPPDATA%\nxnative\android on Windows, ~/.local/share/nxnative/android elsewhere, or under XDG_DATA_HOME on Linux), and every APK is packed from all of them,
# so building a second game keeps the first in the app.
# Steps (the extraction and the rewrite are build_ipa_from_nsp.py's own, reused):
#   1. find the game profile from the title ID (dm/games/<id>); a game without one is surveyed and packed from a provisional profile (<NSP folder>/<id>_build/profile),
#      first with --no-push, then built and installed like the others (an experimental build: it may not boot)
#   2. extract the version to pack (the newest in the folder, or -p) with nx_extract (built with dotnet)
#   3. rewrite the code modules for Android (dm/tools/patch_nso.py with DM_TARGET=android) into <id>_build/patched_android_<version>/
#   (the shaders: split as for iOS, translated to SPIR-V for the Vulkan replay by dm/android/tools/spv_build.py)
#   4. build the game's host (dm/android, CMake + the NDK), the loader and the self test into the library, and pack the APK (dm/android/tools/build_apk.py) into <data>,
#      signed with ~/.android/nxnative-debug.jks (made on first use; every later build must use it to update the app); the button-icon font (make_ext_font.py)
#   5. unless --no-push: install the APK with adb (an update: the app's data, saves included, stay) and copy the rewritten modules, the RomFS (the extraction's
#      RomFS.img), any DLC and the translated shaders to files/games/<title ID>/, with its icon, control data, default game language (defaults.txt) and game.json.
#      A one-game install of the same game (files/game, the layout before the launcher) is moved there instead of copied again, and its save is copied by the app.
#      --romfs-dir puts the RomFS image in another folder on the phone (an SD card's: sd), which defaults.txt's DM_ROMFS names; --no-romfs leaves copying it to the player
# The picture comes from the Vulkan replay of NVN (dm/android/gfx_vk.c).
# Needs: Python with lz4, capstone, fonttools, zbic and backports.zstd (not on Python 3.14), the .NET SDK 10, the Android SDK with an NDK, CMake 3.22 and build-tools (ANDROID_HOME or --sdk), adb, keytool (a JDK).
import argparse, atexit, contextlib, datetime, errno, functools, hashlib, json, os, platform, re, shutil, struct, subprocess, sys, tarfile, tempfile, threading, time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / 'scripts'))
try: import fcntl  # noqa: F401
except ImportError: sys.path.append(str(REPO / 'dm/android/tools/compat'))   # Windows: build_ipa_from_nsp.py loads tree_tar.py, which imports fcntl (compat/fcntl.py)
if os.name == 'nt': os.environ.setdefault('DM_REWRITE_J', '1')   # patch_nso.py's parallel scans fork their workers with SIGTERM blocked, which Windows cannot: one there
import build_ipa_from_nsp as bi   # noqa: E402
from build_ipa_from_nsp import BuildError, say   # noqa: E402

PKG = 'org.nxnative.android'
# One signing key for every build on this computer: an update must be signed like the installed app, and uninstalling it would delete the saves and the game files on the phone
KEYSTORE = Path.home() / '.android' / 'nxnative-debug.jks'
# DejaVu Sans, which DroidPlay for Linux carries (dm/android/linux/stage.sh puts it here; a checkout has none): the last of both lists below, so a Linux without
# DejaVu or Liberation at the paths listed, such as Fedora's Atomic desktops, still builds. Its hinting makes it a standard font too
BUNDLED_FONTS = REPO / 'dm/android/fonts'
# The letters of the button-icon font (make_ext_font.py): macOS's Arial Rounded Bold is its default; elsewhere a bold sans-serif of the system
LETTER_FONTS = ['/System/Library/Fonts/Supplemental/Arial Rounded Bold.ttf', r'C:\Windows\Fonts\arialbd.ttf', '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf',
                '/usr/share/fonts/TTF/DejaVuSans-Bold.ttf', '/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf', '/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf',
                '/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf', '/usr/share/fonts/liberation-sans-fonts/LiberationSans-Bold.ttf',
                '/usr/share/fonts/liberation-sans/LiberationSans-Bold.ttf', str(BUNDLED_FONTS / 'DejaVuSans-Bold.ttf')]
# The standard shared font (hle_sdk16.c: every font type but the Nintendo extension), copied into <id>_build/font/standard.ttf: STD_FONT names one, else the first of these.
# It must be a hinted TrueType font. Android's own Roboto has no TrueType hinting, and Super Smash Bros. Ultimate crashed at boot on it (main+0x7caa0); Roboto with empty
# cvt / fpgm / prep tables crashed too, while Arial stripped of everything but its hinting booted. The iPhone build copies Arial Unicode the same way (build_ipa.sh)
STANDARD_FONTS = ['/System/Library/Fonts/Supplemental/Arial Unicode.ttf', '/System/Library/Fonts/Supplemental/Arial.ttf', r'C:\Windows\Fonts\arialuni.ttf',
                  r'C:\Windows\Fonts\arial.ttf', '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf', '/usr/share/fonts/TTF/DejaVuSans.ttf',
                  '/usr/share/fonts/dejavu/DejaVuSans.ttf', '/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf',
                  '/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf', '/usr/share/fonts/liberation-sans-fonts/LiberationSans-Regular.ttf',
                  '/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf', str(BUNDLED_FONTS / 'DejaVuSans.ttf')]
REMOTE = f'/sdcard/Android/data/{PKG}/files'
BUILD_TOOLS, PLATFORM = '36.0.0', 'android-35'   # build_apk.py's defaults
# libadrenotools and its hooks (dm/android/CMakeLists.txt), in every APK: DM_VK_DRIVER opens a Vulkan driver of the player's through them (dm/android/vk_driver.c), so trying
# one needs no other build. Built beside the loader, and kept with it in the library
VK_DRIVER_LIBS = ('libadrenotools.so', 'libhook_impl.so', 'libmain_hook.so')
# Games played on an Android device: the rows of the README's Game Status whose tier is Playable – minor issues, Playable – major issues or In-game from a run
# on current code, with a profile, kept in step with that table. The desktop app shows the others as not played there yet. Left out: In-game, blocked (Mario Kart
# World, Ocarina of Time), and the rows from before the nxnative merge (†: Super Mario Bros. Wonder, Super Mario 3D World + Bowser's Fury), which current code
# hasn't run
ANDROID_TESTED = {
    '01008DB008C2C000',   # Pokémon Shield (Playable – minor issues through Route 2 into Professor Magnolia's house)
    '010027400CDC6000',   # Divinity: Original Sin 2 - Definitive Edition (Playable – minor issues through the second fight below deck)
    '010003F003A34000',   # Pokémon: Let's Go, Pikachu! (Playable – minor issues through the rival's first battle)
    '0100000011D90000',   # Pokémon Brilliant Diamond (Playable – minor issues through Lake Verity's first battle)
    '0100DCD01525A000',   # Persona 3 Portable (Playable – minor issues through the first battle)
    '0100300012F2A000',   # Advance Wars 1+2: Re-Boot Camp (Playable – minor issues through Field Training's first lesson)
    '01006F8002326000',   # Animal Crossing: New Horizons (Playable – major issues through the first night)
    '0100ECD018EBE000',   # Paper Mario: The Thousand-Year Door (Playable – minor issues through the first battle; 1.0.0 from a derived profile)
    '0100000000010000',   # Super Mario Odyssey
    '01004AB00A260000',   # DARK SOULS: REMASTERED (In-game into the Asylum cell at 30 fps; Playable needs a person; 1.0.0 from a derived profile)
    '0100B1C0249B4000',   # Frostpunk (Playable – major issues through day 1 into the first night)
    '0100152000022000',   # Mario Kart 8 Deluxe (Playable – minor issues through the first 50cc race)
    '01006A800016E000',   # Super Smash Bros. Ultimate
    '0100E95004038000',   # Xenoblade Chronicles 2 (Playable – major issues through Argentum and the first save)
    '01006BB00C6F0000',   # The Legend of Zelda: Link's Awakening
    '010040801A4C8000',   # Blasphemous 2 (In-game into the weapon room at 60 fps; Playable needs a person; 1.0.1 from a derived profile)
    '01002DA013484000',   # The Legend of Zelda: Skyward Sword HD
    '01007EF00011E000',   # The Legend of Zelda: Breath of the Wild (Playable – major issues through the Shrine of Resurrection onto the Great Plateau)
    '010057D006492000',   # OCTOPATH TRAVELER (Playable – major issues through H'aanit's first battles)
    '0100A3501946E000',   # OCTOPATH TRAVELER II (Playable – major issues through Hikari's first battle; 1.0.0 from a derived profile)
    '0100606026F12000',   # Final Fantasy Resonance
    '01009D901BC56000',   # Donkey Kong Country Returns HD (In-game into 1-1 at 60 fps; Playable needs a person; 1.0.0 from a derived profile)
    '010069C01AB82000',   # SHIN MEGAMI TENSEI V: Vengeance (a contributor's report)
    '01001F5010DFA000',   # Pokémon Legends: Arceus (Playable – major issues through Prelude Beach's catching tutorial into Jubilife Village; washed-out picture, no characters on Mali)
    '0100F43008C44000',   # Pokémon Legends: Z-A (Playable – major issues through the opening onto Lumiose's station platform at 30 fps on Adreno, 1.0.0 and 2.0.2; Mali draws it nearly black)
    '010074F013262000',   # Xenoblade Chronicles 3 (In-game, blocked: characters only in cutscenes, world striped; 1.0.0 from a derived profile)
}


def data_dir(): return data_home.data_dir()   # the library of game hosts, the loader, the self test and the APK (dm/android/tools: XDG_DATA_HOME on Linux)


class Journaled(bi.Progress):   # the console's progress, and the same build's events as JSON lines in a journal file (a second Progress in json mode, written there)
    KEEP = 20   # journals kept

    def __init__(self, mode, argv):
        super().__init__(mode); self.inner = False
        d = data_dir() / 'builds'; d.mkdir(parents=True, exist_ok=True)
        for old in sorted(d.glob('*.jsonl'))[:-self.KEEP]: old.unlink(missing_ok=True)
        self.f = open(d / f'{datetime.datetime.now():%Y%m%d-%H%M%S}-{os.getpid()}.jsonl', 'a', encoding='utf-8')
        self.j = bi.Progress('json'); self.j._json = self.write
        self.write({'ev': 'start', 'pid': os.getpid(), 'id': os.environ.get('NXNATIVE_BUILD_ID'), 'argv': argv, 'cwd': os.getcwd()})

    def write(self, obj):
        obj.setdefault('t', round(datetime.datetime.now().timestamp(), 3))
        try: self.f.write(json.dumps(obj, ensure_ascii=False, default=str) + '\n'); self.f.flush()
        except (OSError, ValueError): pass   # the journal never stops the build

    def plan(self, n): super().plan(n); self.j.plan(n)
    def add(self, n): super().add(n); self.j.add(n)
    def stage(self, name, poll=None, step='', **kw):
        self.inner = True   # stage() ends the current stage and sets the counts through end() and set(): the journal's own stage() does both itself
        try: super().stage(name, None, step, **kw)
        finally: self.inner = False
        self.j.stage(name, step=step, **kw)
        if poll: self.watch(poll)
    def set(self, **kw):
        super().set(**kw)
        if not self.inner: self.j.set(**kw)
    def end(self, summary='', ok=True):
        super().end(summary, ok)
        if not self.inner: self.j.end(summary, ok)
    def say(self, msg): super().say(msg); self.j.say(msg)
    def event(self, ev, **fields): super().event(ev, **fields); self.j.event(ev, **fields)   # the desktop app follows a console build's journal: run_time_shaders reaches it there too
    def close(self, ok=True): super().close(ok); self.j.close(ok)
    def exit(self, code): self.close(ok=code == 0); self.write({'ev': 'exit', 'code': code}); self.f.close()


def sdk_dir(arg):   # --sdk, the environment, then where Android Studio puts it: Windows, Linux, macOS
    for c in (arg, os.environ.get('ANDROID_HOME'), os.environ.get('ANDROID_SDK_ROOT'), os.path.join(os.environ.get('LOCALAPPDATA', ''), 'Android', 'Sdk'), os.path.expanduser('~/Android/Sdk'),
              os.path.expanduser('~/Library/Android/sdk')):
        if c and (Path(c) / 'platform-tools').is_dir(): return Path(c)
    raise BuildError('Android SDK not found: give --sdk or set ANDROID_HOME')


sys.path.append(str(REPO / 'dm/android/tools')); import data_home, host_setup, phone_id   # noqa: E401, E402  # pyright: ignore[reportMissingImports]  (data_dir, --setup, a phone's name in folder names; dm/android/tools, put on the path here)
NDK, CMAKE = '28.2.13676358', '3.22.1'   # what --setup installs, and the CI builds with; a build takes the newest NDK and CMake the SDK has


def sdk_packages(sdk):   # check()'s rows for the SDK's parts → (sdkmanager's name, a file it puts there), for --setup
    exe = '.exe' if os.name == 'nt' else ''
    return {'adb': ('platform-tools', sdk / 'platform-tools' / f'adb{exe}'), f'platform {PLATFORM}': (f'platforms;{PLATFORM}', sdk / 'platforms' / PLATFORM / 'android.jar'),
            f'build-tools {BUILD_TOOLS}': (f'build-tools;{BUILD_TOOLS}', sdk / 'build-tools' / BUILD_TOOLS), 'NDK': (f'ndk;{NDK}', sdk / 'ndk' / NDK),
            'CMake': (f'cmake;{CMAKE}', sdk / 'cmake' / CMAKE)}


def tool_paths(sdk_arg=None):   # the JDK and .NET SDK --setup installed, and the SDK's adb, on PATH for this run and what it starts (the desktop app imports this script for it too)
    try: sdk = sdk_dir(sdk_arg)
    except BuildError: sdk = None
    host_setup.add_tool_paths(data_dir() / 'tools', sdk)


tool_paths()


def newest(d, what):
    vs = sorted((p for p in d.iterdir() if p.is_dir()), key=lambda p: [int(x) if x.isdigit() else 0 for x in p.name.split('.')]) if d.is_dir() else []
    if not vs: raise BuildError(f'no {what} in {d}')
    return vs[-1]


def build_nx_extract(work, logs, env):
    """build_ipa_from_nsp.py's, with Windows' .exe, and built once per version of its source into <data>/tools/nx_extract-<digest>, which every run then takes:
    the desktop app reads each game with --info, so a build per folder per read was a dotnet build each time, several at once writing the project's obj/.
    work is unused (that script's builder put it there)"""
    src = REPO / 'dm/tools/nx_extract'; name = 'nx_extract.exe' if os.name == 'nt' else 'nx_extract'
    h = hashlib.sha256()
    for p in sorted(src.rglob('*')):
        rel = p.relative_to(src)
        if p.is_file() and not {'bin', 'obj'} & set(rel.parts): h.update(rel.as_posix().encode() + b'\0' + p.read_bytes() + b'\0')
    out = data_dir() / 'tools' / f'nx_extract-{h.hexdigest()[:16]}'
    if (out / name).is_file(): return out / name
    with tempfile.TemporaryDirectory(prefix='nx_extract_') as tmp:   # built outside and copied in, as that script does: MSBuild breaks on an apostrophe in the path
        # obj/ (the restore's and the compiler's files) goes there too, not into the project: DroidPlay may run from a tree it can't write, such as an AppImage's
        # mount, where the restore stopped with "Permission denied". The trailing separator is MSBuild's rule for this property
        t, obj = Path(tmp) / 'out', Path(tmp) / 'obj'
        bi.run(['dotnet', 'build', src, '-c', 'Release', '-o', t, f'-p:BaseIntermediateOutputPath={obj}{os.sep}', '-v', 'quiet', '-nologo'],
               logs / 'nx_extract_build.log', env=env)
        if not (t / name).exists(): raise BuildError(f'nx_extract built without an executable (see {logs / "nx_extract_build.log"})')
        part = out.with_name(f'{out.name}.{os.getpid()}'); bi.rmtree(part); shutil.copytree(t, part)
        try: os.replace(part, out)
        except OSError: bi.rmtree(part)   # another run put the same build there meanwhile
    return out / name


# build_ipa_from_nsp.py builds nx_extract itself wherever it reads packages (package_reader, which scan() uses) and in the survey, and looks for an executable named
# nx_extract, which on Windows is nx_extract.exe: every NSP folder stopped there. This script's builder takes its place for the whole run.
bi.build_nx_extract = lambda repo, work, logs, env: build_nx_extract(work, logs, env)


def build_host(sdk, game_id, work, logs, env):   # → (the game's host, the loader, the NDK); VK_DRIVER_LIBS beside them
    ndk = newest(sdk / 'ndk', 'NDK'); cm = newest(sdk / 'cmake', 'CMake') / 'bin'
    exe = '.exe' if os.name == 'nt' else ''
    bdir = work / 'android_host'
    # a build folder set up from another checkout (a worktree, a clone that moved, another checkout picked in a desktop app): cmake refuses a cache made for another
    # source folder, so that one starts afresh
    cache = bdir / 'CMakeCache.txt'
    if cache.is_file():
        m = re.search(r'^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)$', cache.read_text(encoding='utf-8', errors='replace'), re.M)
        if m and Path(m.group(1).strip()).resolve() != (REPO / 'dm/android').resolve(): shutil.rmtree(bdir)
    bi.run([cm / f'cmake{exe}', '-G', 'Ninja', '-S', REPO / 'dm/android', '-B', bdir, f'-DCMAKE_MAKE_PROGRAM={cm / ("ninja" + exe)}',
            f'-DCMAKE_TOOLCHAIN_FILE={ndk / "build/cmake/android.toolchain.cmake"}', '-DANDROID_ABI=arm64-v8a', '-DANDROID_PLATFORM=android-29',
            f'-DGAME={game_id}', f'-DPython3_EXECUTABLE={sys.executable}'], logs / 'android_cmake.log', env=env)
    bi.run([cm / f'ninja{exe}', '-C', bdir, 'dmhost_app', 'nxloader', *[n[3:-3] for n in VK_DRIVER_LIBS]], logs / 'android_host.log', env=env)
    return bdir / f'libdmhost_{game_id}.so', bdir / 'libnxloader.so', ndk


def build_selftest(ndk, work, logs, env):
    bin_ = next((ndk / 'toolchains/llvm/prebuilt').iterdir()) / 'bin'; exe = '.exe' if os.name == 'nt' else ''
    d = work / 'android_selftest'; d.mkdir(parents=True, exist_ok=True)
    bi.run([sys.executable, REPO / 'dm/android/tests/selftest_nso.py', bin_ / f'clang{exe}', bin_ / f'llvm-objcopy{exe}', d / 'main.nso'], logs / 'android_selftest.log', env=env)
    bi.run([sys.executable, REPO / 'dm/tools/patch_nso.py', d / 'main.nso', d / 'patched', 'main'], logs / 'android_selftest_rewrite.log', env=env)
    return d / 'patched'


def button_font(work, logs, env):   # the Nintendo extension shared font's stand-in (hle_sdk16.c: button icons at their private-use code points), into <id>_build/font
    font = work / 'font'; ttf = font / 'nintendo_ext.ttf'
    letters = next((f for f in LETTER_FONTS if Path(f).is_file()), None)
    if not letters: raise BuildError('no letter font for the button icons: ' + ', '.join(LETTER_FONTS))
    if not bi._importable('fontTools'): raise BuildError(f'the current Python ({sys.executable}) is missing fontTools (pip install fonttools)')
    font.mkdir(parents=True, exist_ok=True)
    bi.run([sys.executable, REPO / 'dm/tools/make_ext_font.py', ttf, letters], logs / 'make_ext_font.log', env=env)
    standard_font(font)
    return font


def hinted(path):   # a TrueType font with hinting programs (STANDARD_FONTS)
    try:
        from fontTools.ttLib import TTFont
        f = TTFont(path, lazy=True); return 'glyf' in f and 'fpgm' in f and 'prep' in f
    except Exception: return False


def standard_font(font):   # <font>/standard.ttf: a hinted TrueType font; without one the device falls back to its own Roboto, on which some games crash
    dst = font / 'standard.ttf'
    named = os.environ.get('STD_FONT')
    cands = [named] if named else [f for f in STANDARD_FONTS if Path(f).is_file()]
    src = next((f for f in cands if hinted(f)), None)
    if not src:
        dst.unlink(missing_ok=True)
        say(f'[RED] no hinted TrueType font for the standard shared font ({"STD_FONT=" + named + " is not one" if named else "none of " + ", ".join(STANDARD_FONTS)}): '
            "the game uses the device's Roboto, on which Super Smash Bros. Ultimate crashes at boot. Set STD_FONT to a hinted .ttf")
        return
    if not dst.is_file() or dst.stat().st_size != Path(src).stat().st_size: shutil.copyfile(src, dst)


# A full disk as a tool's log shows it: POSIX's strerror, and Windows' ERROR_DISK_FULL and ERROR_HANDLE_DISK_FULL as .NET (the translator) and Python print them
DISK_FULL = re.compile(r'No space left on device|not enough space on the disk|The disk is full|\[Errno 28\]|ENOSPC', re.I)
LOW_DISK = 1 << 30   # a step that failed with less than this free on its drive is taken for one that ran out of space
_work = None   # the game's <id>_build once main knows it: the folder a full disk's message names when the error names no file (a write to an open file names none)


def drive_of(p):   # the drive a path is on, as a player names it: C:\ on Windows, the volume's mount point elsewhere
    p = Path(os.path.abspath(p))
    if os.name == 'nt': return p.anchor or str(p)
    while not os.path.ismount(p) and p != p.parent: p = p.parent
    return str(p)


def free_on(p):   # bytes free on the drive of path p (its nearest existing folder), or None
    p = Path(os.path.abspath(p))
    while not p.exists() and p != p.parent: p = p.parent
    try: return shutil.disk_usage(p).free
    except OSError: return None


def disk_full(path):
    """What to say when this computer ran out of space writing under path (a file or folder; None: unknown): the drive, what it has free, what to free. Pokémon Pokopia's
    build on Windows ended in a traceback, OSError: [Errno 28] (droidplay-core#307), which named neither the drive nor what had filled it"""
    where = f'{drive_of(path)} ' if path else ''
    free = free_on(path) if path else None
    text = (f'This computer ran out of disk space: the drive {where}' + (f'has {size_text(free)} free' if free is not None else 'is full') +
            (f' (writing {path})' if path else '') + '. Free some space on it, or move the game\'s folder to a drive with more room, then build again: the steps already '
            'done are kept. A game\'s <title ID>_build folder beside its files holds its extracted files and translated shaders, so deleting the folder of a game you no '
            'longer build frees that much.')
    try: old = [d for d in Path(tempfile.gettempdir()).glob('spv_*') if d.is_dir()]
    except OSError: old = []
    if old:   # before spv_build.py kept them under WORK, every failed translation left one in the system's temp folder, tens of GB for a big game
        text += f' {len(old)} spv_… folder{"s" if len(old) > 1 else ""} in {tempfile.gettempdir()} {"were" if len(old) > 1 else "was"} left by failed shader translations of earlier versions: nothing uses them, so delete them.'
    return text


def filled(text):
    """The path a full disk's lines in a log name, or None: the folder spv_build.py's own line says it couldn't write, else the file of the last error that names one
    (Python's OSError, which doubles a Windows path's backslashes, and .NET's IOException end in : '<path>'). spv_build.py's temporary folder can be on another
    drive than <id>_build: runner-a's check filled a small drive mounted at <id>_build/tmp, and the message named <id>_build's drive, with its 755 GB free"""
    m = re.search(r'^\[RED\] spv_build: the disk is full: .* The drive with (.+?) has (?:[\d.]+ GB free|no free space it could read)', text, re.M)
    if m: return m.group(1)
    for ln in reversed(text.splitlines()):
        if DISK_FULL.search(ln) and (q := re.search(r": '([^']+)'\s*$", ln)): return q.group(1).replace('\\\\', '\\') if '[Errno' in ln else q.group(1)
    return None


def translation_stopped(work, log):   # what build_shaders adds when spv_build.py stopped: its own reason (a [RED] spv_build: line), and a full disk's message when the disk filled
    try: text = log.read_text(errors='replace')
    except OSError: text = ''
    own = [l[6:] for l in text.splitlines() if l.startswith('[RED] spv_build: ')]
    where = filled(text) or work
    free = free_on(where)
    full = DISK_FULL.search(text) or (free is not None and free < LOW_DISK)
    return ''.join(f'{l}\n' for l in own[-1:]) + (f'{disk_full(where)}\n' if full else '')


def build_shaders(sv, dirs, work, logs, env, g, dlc_roms=(), derive=None, derived='', oodle=None):
    """The game's NVN shaders for the Vulkan replay: the split (build_ipa_from_nsp.py split_shaders, checked against the profile's tables), then
    dm/android/tools/spv_build.py (the in-repository translator, dmsh_xlate_bin's dll run by dotnet) into <work>/spv. Returns that folder (None when there are none) and the profile.
    A registered profile's tables may differ from the split in two ways the iPhone build accepts as well: installed DLC's own shaders, and compute kernels a newer reader decodes
    from the registered build (compute_reader_additions). derive(reason) then makes the shader-only derived profile that the translation and the rest of the build use. Any other
    failure of a registered profile's split stops the build: packed without shaders, AI: The Somnium Files played its sound over a black screen (droidplay-core#218).
    derived: why main packs from a profile derived from a registered one ('' for a registered or provisional profile). Its split failing stops the build too, though the profile
    is outside dm/games: its game has the shaders the registered tables list (Pokémon Pokopia's split failed on Windows, and the build packed it without them, droidplay-core#307).
    A translation that stops (spv_build.py's exit code) stops the build for every profile: the shaders done by then are an arbitrary part of the game's, and packed with
    them the game would draw some scenes and play others' sound over a black screen, as #218's did. Pokémon Pokopia's build went on to the install after its translation
    stopped on a full disk (droidplay-core#307). A shader the translator can't handle is a [RED] line in the log, not a stop.
    oodle: build_ipa_from_nsp.py split_shaders' (a Mac host for the game's Oodle, mac_decoder), which puts the host in the split's inputs"""
    if not sv: return None, g
    sh = work / 'shader'
    own = not g.dir.resolve().is_relative_to((REPO / 'dm/games').resolve())   # a provisional or derived profile (outside dm/games) takes the split's tables, as build_ipa_from_nsp.py does; the repository's are checked
    got = {}
    def promote(reason):   # the split calls it only after its strict DLC-only or additive-compute comparison passed, and only when derive was given
        assert derive is not None
        got['g'] = derive(reason); return got['g']
    dd = (lambda: promote('the installed DLC shader content differs from the registered tables')) if derive and not own else None
    dc = (lambda: promote('the reader decoded additional compute kernels from this registered build')) if derive and not own else None
    try: bi.split_shaders(sys.executable, REPO, [bi.rfs.source(dirs[v]) for v in sv], [dirs[v] / 'ExeFS/main' for v in sv], sh / 'pkg', env, logs, g, own,   # its own stage (a DLC's shaders too)
                          oodle=oodle, dlc_roms=dlc_roms, derive_dlc=dd, derive_compute=dc,
                          targeted=not own and not derived)   # a profile that registers this version: first only the files its shader map names (docs/reference/packing.md)
    except BuildError as e:
        if not own: raise BuildError(f'{e}\nThe game is not packed: without its shaders it would play its sound over a black screen. Check that the folder holds the versions game profile '
                                     f'{g.id} registers (its game.tsv); if it does, report this build with {logs / "shader_split.log"}') from e
        if derived: raise BuildError(f'{e}\nThe game is not packed: its profile is derived from game profile {g.id} ({derived}), whose registered tables list its shaders, and without '
                                     f'them it would play its sound over a black screen. Packing a version and build that {g.id} registers (its game.tsv) uses the registered profile '
                                     f'instead (--package picks the version); otherwise report this build with {logs / "shader_split.log"} and this message') from e
        bi.PROG.end(str(e), ok=False); say('[RED] the game is packed without shaders (its draws are skipped). If it compiles its shaders while it runs, play it for a while (black is expected), '
            'then run this build again with --update-shaders (DroidPlay for Windows: Manage › Update shaders from the phone) to bring them in: '
            'docs/android.md, "Games that compile their shaders while they run"'); return None, g   # a provisional profile: shaders compiled at run time, captured later
    g = got.get('g', g)
    bi.PROG.stage('shaders for Vulkan', step='translating')
    out = work / 'spv'
    def counted(ln):   # spv_build.py's 'progress: <done>/<total>': the shaders left to translate in this run (droidplay-core#307, a translation of hours showed no count)
        if m := re.fullmatch(r'progress: (\d+)/(\d+)', ln): bi.PROG.set(done=int(m[1]), total=int(m[2]), unit='shaders')
        elif ln.startswith('step: '): bi.PROG.set(step=ln[6:].strip())   # the steps after the first pass, each counted again (droidplay-core#437: 100 % for an hour)
    try: bi.run([sys.executable, REPO / 'dm/android/tools/spv_build.py', sh / 'pkg', out, '--xlate', REPO / 'dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate.dll',
                 '--facts', g.table('shader_facts.tsv')], logs / 'spv_build.log', env=env, on_line=counted)
    except BuildError as e:
        why = translation_stopped(work, logs / 'spv_build.log')
        if why.startswith('spv_build: not enough free space'): raise BuildError(f'{e}\n{why}The game is not packed: its shaders were not translated.') from e   # spv_build.py's check, before it started
        raise BuildError(f'{e}\n{why}Shader translation failed, so the game is not packed: with only the shaders translated so far, some of its scenes would draw nothing. '
                         f'The shaders that were translated are kept for the next build. Report this build with {logs / "spv_build.log"}') from e
    last = [l for l in (logs / 'spv_build.log').read_text(errors='replace').splitlines() if l.startswith('spv_build:')]
    bi.PROG.end(last[-1][11:] if last else str(out))
    return out, g


def ue_decode(dirs, pkg_ver, nsp_dir, patched, work, logs, env, g, serial, sdk, game):
    """Unreal's shaders, decoded into each version's <extraction>/ue_shaders before the split reads them (build_ipa_from_nsp.py decode_ue_shaders, the NSP scripts' step): they are
    Oodle-compressed, and only the game's own decoder undoes it. The Mac build runs it in the Mac host; this computer may not run the game's arm64 code (Windows, Linux), so the
    phone does (scripts/android_oodle.py): the game's host goes into the app, and its rewritten code to the phone, before the shaders exist (the host does not depend on them).
    A folder whose stamp matches its paks and containers is kept, and one a Mac build made beside this NSP folder (<game>/<title ID>_build, from the same files) is taken"""
    sys.path.insert(0, str(REPO / 'dm/gfx/tools')); import uepak, android_oodle   # noqa: E401  # pyright: ignore[reportMissingImports]
    fresh = lambda d: (d / 'ue_shaders/stamp').is_file() and (d / 'ue_shaders/stamp').read_text() == bi.ue_stamp(d)
    todo = {v: d for v, d in dirs.items() if not fresh(d)}
    bi.PROG.add(1); bi.PROG.stage('UE shader decode')
    for v, d in list(todo.items()):   # DroidPlay's Mac build of the same game (the folder above the NSP folder), its decoded archives made from the same paks
        if os.environ.get('DM_UE_DECODE') == 'phone': break   # the phone's decode wanted even so (a test of it on a Mac with the game's Mac build)
        mac = nsp_dir.parent / f'{g.id}_build' / 'extract' / v / 'ue_shaders'
        if (mac / 'stamp').is_file() and (mac / 'stamp').read_text() == bi.ue_stamp(d):
            part = d / 'ue_shaders.partial'; bi.rmtree(part); shutil.copytree(mac, part); bi.swap_in(part, d / 'ue_shaders'); del todo[v]
            say(f'{v}: the decoded shader archives of the Mac build in {mac.parent.parent.parent}')
    if not todo: bi.PROG.end('reused'); return
    if not serial:
        raise BuildError('the game\'s Unreal shaders are Oodle-compressed, and only the game\'s own decoder undoes them: it runs on the phone here, so connect one (or build once without '
                         '--no-push, or build the game in DroidPlay for Mac first)')
    bi.PROG.set(step='the game\'s host and code to the phone, where its own Oodle decodes the shaders')
    phone = oodle_phone(serial, sdk, g, game, pkg_ver, nsp_dir, patched, work, logs, env)
    t0, total = time.time(), 0
    for v, d in todo.items():
        bi.PROG.set(step=f'{v}: decoding the shaders with the game\'s Oodle on the phone')
        jd, out = work / f'tmp/ue_job_{v}', d / 'ue_shaders.partial'; bi.rmtree(out)
        try: listing, n = android_oodle.decode(uepak, bi.rfs.source(d), d / 'ExeFS/main', jd, out, phone)
        except ValueError as e: raise BuildError(str(e)) from e   # a damaged pak or container, or the phone's job (its log named)
        if not listing: raise BuildError(f'the RomFS has Unreal paks, but none under {bi.rfs.source(d)} holds a shader code archive: where the shaders are has to be found first')
        say(f'{v}: ' + '; '.join(f'{x["name"]} {x["shaders"]:,} shaders' + (f' ({x["compression"]})' if x.get('compression') else '') for x in listing))
        (out / 'stamp').write_text(bi.ue_stamp(d)); bi.swap_in(out, d / 'ue_shaders'); bi.rmtree(jd)
        total += n + sum(x['shaders'] for x in listing if x['compression'] == 'IoStore, decoded')
    bi.PROG.end(f'{total:,} shaders in {bi.hms(time.time() - t0)}, {phone.jobs} job(s) on the phone')


def oodle_phone(serial, sdk, g, game, pkg_ver, nsp_dir, patched, work, logs, env):
    """The game's host into the app and its rewritten code to the phone, for the jobs of scripts/android_oodle.py Phone (ue_decode, oodle_decode): the host does not depend on the
    shaders, which do not exist yet. Only the code goes: a job reads no RomFS"""
    import android_oodle   # pyright: ignore[reportMissingImports]
    so, loader, ndk = build_host(sdk, g.id, work, logs, env); st = build_selftest(ndk, work, logs, env)
    library_add(g.id, so, loader, st, {'title_id': g.id, 'name': bi.nacp.name(game / 'control.nacp') or g.get('name', ''), 'version': pkg_ver,
                                       'display': bi.nacp.version(game / 'control.nacp'), 'nsp_dir': str(nsp_dir)})
    install(serial, make_apk(sdk, logs, env), logs)
    dst = f'{REMOTE}/games/{g.id}'; adb(serial, 'shell', 'mkdir', '-p', dst); adb(serial, 'shell', 'rm', '-rf', f'{dst}/code')
    push_dir(serial, patched, dst, 'code', logs / 'adb_push_code_oodle.log'); open_tree(serial, dst)
    return android_oodle.Phone(adb, serial, g.id, REMOTE, PKG, work, logs, say)


def mac_decoder(nsp_dir, work, g, pkg_ver, game, env):
    """DM_OODLE_HOST / DM_OODLE_MODULES for the shader split's Oodle on this computer, or None: set by hand, or on an Apple-silicon Mac the Mac host and the Apple-rewritten modules
    of DroidPlay for Mac's build of the same main (<game>/<title ID>_build beside the NSP folder, or this build's own folder when the Mac's NSP script ran on it too). The Android
    build's own modules cannot stand in: they reach their thread slots through x18 (dm/tools/nso_asm.py), where the Mac host gives them through tpidrro_el0"""
    if env.get('DM_OODLE_HOST') and env.get('DM_OODLE_MODULES'): return {k: env[k] for k in ('DM_OODLE_HOST', 'DM_OODLE_MODULES')}
    if sys.platform != 'darwin' or platform.machine() != 'arm64' or os.environ.get('DM_UE_DECODE') == 'phone': return None
    bid = bi.build_id(game / 'ExeFS/main')
    for b in (nsp_dir.parent / f'{g.id}_build', work):
        host, fx = b / 'tools/dmhost', b / f'patched_{pkg_ver}' / 'main.fixups.tsv'
        if host.is_file() and fx.is_file() and f'# BUILD_ID\t{bid}' in fx.read_text(errors='replace').splitlines()[:20]:
            say(f'the game\'s Oodle for the shader split: the Mac host of the Mac build in {b}')
            return {'DM_OODLE_HOST': str(host), 'DM_OODLE_MODULES': str(fx.parent)}
    return None


def oodle_decode(roms, mains, dlc_roms, game, pkg_ver, nsp_dir, patched, work, logs, env, g, serial, sdk):
    """The streams of the game's Oodle-compressed archives (profile_guess.py oodle_packs) decoded with the game's own Oodle on the phone before the shader split, into
    <extraction>/oodle_cache, which the split then reads in place of the Mac host (DM_OODLE_CACHE; scripts/android_oodle.py fill). Pokémon Legends: Arceus's split failed on all
    678 GFPAKs without it (issue #310). The record runs list what the split reads (the whole split each, decoding nothing); the phone is set up only when one lists a stream the
    cache lacks. A cache that last ended on the same files, split code and decoder is taken as it is → its folder"""
    import android_oodle   # pyright: ignore[reportMissingImports]
    bi.PROG.add(1); bi.PROG.stage('Oodle decode')
    cache = android_oodle.Cache(game / 'oodle_cache', hashlib.sha256((game / 'ExeFS/main').read_bytes()).hexdigest())   # streams are keyed by content; the decoder is main's
    key = bi.fingerprint('oodle-record-v1', ('files', bi.python_tool_files(REPO, 'dm/gfx/tools/shader_split.py', ('dm/gfx/tools', 'dm/tools', 'dm/games'))), *[('tree', r) for r in roms],
                         ('files', list(mains)), *[x for r in dlc_roms for x in (('dlc', Path(r).name), ('stat', r))])
    if cache.done(key): bi.PROG.end(f'reused: {len(cache):,} streams'); return cache.d
    runs, held = [0], contextlib.ExitStack()
    def counted(ln):   # shader_split.py's 'progress: <done>/<total>'
        if m := re.fullmatch(r'progress: (\d+)/(\d+)', ln): bi.PROG.set(done=int(m[1]), total=int(m[2]), unit='files')
    def record(want):
        runs[0] += 1; out = work / 'tmp/oodle_record'; bi.rmtree(out); out.mkdir(parents=True)
        bi.PROG.set(step=f'run {runs[0]}: listing the streams the shader split reads')
        cmd = [sys.executable, REPO / 'dm/gfx/tools/shader_split.py', ','.join(map(str, roms)), ','.join(map(str, mains)), out / 'pkg'] + [out / n for n in ('map.tsv', 'facts.tsv', 'pairs.tsv', 'pso.tsv')]
        # a failed run's listed streams still count, and the real split says again whatever else went wrong
        bi.run(cmd + ([','.join(map(str, dlc_roms))] if dlc_roms else []), logs / 'oodle_record.log', env=dict(env, DM_OODLE_CACHE=str(cache.d), DM_OODLE_RECORD=str(want)),
               on_line=counted, fatal=False)
        bi.rmtree(out)
    def decoder():
        if not serial:
            raise BuildError('the game\'s shaders are in archives compressed with its own Oodle, which only the game\'s own decoder reads: it runs on the phone here, so connect one '
                             'and build without --no-push or --game-apk (or build the game in DroidPlay for Mac first)')
        held.enter_context(phone(serial, f'build_android_from_nsp.py: decoding {g.id}\'s Oodle archives'))   # only while the phone works: the record runs take minutes
        bi.PROG.set(step='the game\'s host and code to the phone, where its own Oodle decodes the archives')
        return oodle_phone(serial, sdk, g, game, pkg_ver, nsp_dir, patched, work, logs, env)
    t0 = time.time()
    with held:
        try: _, n = android_oodle.fill(cache, record, decoder, work / 'tmp/oodle_want')
        except ValueError as e: raise BuildError(str(e)) from e   # the phone's job (its log named), or a split that kept asking
    cache.finish(key)
    bi.PROG.end(f'{n:,} streams decoded on the phone in {bi.hms(time.time() - t0)}' if n else f'{len(cache):,} streams, none new ({runs[0]} run(s))')
    return cache.d


# ---------------- The library of game hosts ----------------
def library_add(gid, so, loader, selftest, meta):   # the game's host, and the loader, VK_DRIVER_LIBS and self test of the latest build (they do not depend on the game)
    lib = data_dir(); d = lib / 'hosts' / gid; d.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(so, d / so.name)
    (d / 'meta.json').write_text(json.dumps(dict(meta, built=datetime.datetime.now().isoformat(timespec='seconds')), indent=1), encoding='utf-8')
    (lib / 'loader').mkdir(exist_ok=True); shutil.copyfile(loader, lib / 'loader' / loader.name)
    for f in driver_libs(loader.parent): shutil.copyfile(f, lib / 'loader' / f.name)
    st = lib / 'selftest'; shutil.rmtree(st, ignore_errors=True); shutil.copytree(selftest, st)


def driver_libs(d):   # VK_DRIVER_LIBS in a folder: a library made before they were built has none, and its APK goes without (DM_VK_DRIVER then logs that they are missing)
    return [d / n for n in VK_DRIVER_LIBS if (d / n).is_file()]


def library():   # {title ID: (host, meta)} of the hosts built on this computer
    out = {}
    for d in sorted((data_dir() / 'hosts').glob('*')):
        so = d / f'libdmhost_{d.name}.so'
        if so.is_file():
            try: meta = json.loads((d / 'meta.json').read_text(encoding='utf-8'))
            except (OSError, ValueError): meta = {}
            out[d.name] = (so, meta)
    return out


def safe_name(name):   # a file name every file system takes, a phone's and Windows' too
    return re.sub(r'\s+', ' ', re.sub(r'[/\\:*?"<>|]', '', name)).strip()


def clone(src, dst):   # an APFS clone where the volume can make one, else a hard link, else a copy: the RomFS beside a game app costs no space where it can
    Path(dst).unlink(missing_ok=True)
    if sys.platform == 'darwin' and subprocess.run(['cp', '-c', str(src), str(dst)], capture_output=True).returncode == 0: return
    try: os.link(src, dst)
    except OSError: shutil.copyfile(src, dst)


def stage_link(dst, src):   # a symbolic link, else the files cloned (clone): Windows lets a process make symbolic links only with Developer Mode on, or as administrator
    try: Path(dst).symlink_to(src, target_is_directory=Path(src).is_dir())
    except OSError:
        if Path(src).is_dir(): shutil.copytree(src, dst, copy_function=clone)
        else: clone(src, dst)


# A game app (--game-apk): the game in an app of its own, org.nxnative.game.t<title ID> with its name and icon, entered through dm/android/java GameStart with no launcher
# (apk/AndroidManifest.game.xml). The APK carries the game's host and its files as push_game lays them out on the phone (assets/game/, unpacked by GameStart at the first
# start), and the RomFS image as a stored entry when the APK stays under 2 GiB (build_apk.py: a ZIP without ZIP64). A bigger game's RomFS goes beside it, <name>.romfs,
# which the player puts anywhere on the device and picks once in the app; --romfs-beside puts any game's there, so the APK stays small. --xapk packs one .xapk instead (an XAPK installer's format: the APK, the RomFS as the app's OBB
# file Android/obb/<package>/main.1.<package>.obb, icon.jpg and manifest.json), for any size; the two kinds sit side by side, each replacing only its own files. Nothing
# goes into the library of hosts: the nxnative app stays as it was.
# The DLC images (aoc/<index>.img) go into the APK while it can hold them; when it can't (Mario Kart 8 Deluxe's Booster Course Pass and its shaders pass 2 GiB with no
# RomFS at all: droidplay-core issue #169), they follow the RomFS in the same data file, the .romfs beside the APK or the XAPK's OBB, so a player still picks one file and
# an XAPK still has one expansion. GameStart copies them out of it into games/<title ID>/aoc/ once, where front_android.c points DM_AOC_DIR, so the host reads them as a
# launcher install's (it mounts DLC by path; only the RomFS goes to it as a descriptor). The RomFS leaves the APK first: the DLC goes beside only with it.
# The data file: the RomFS image from byte 0 (the host reads it as DM_ROMFS=fd:<descriptor>:0:<romfs_bytes>), then, at dlc_at (the RomFS's end rounded up to DLC_ALIGN),
# the DLC pack: DLC_MAGIC, the image count and a zero (little-endian 32-bit), a record per image (32-bit index, 32-bit zero, 64-bit offset from dlc_at, 64-bit size), and
# the images, each at a DLC_ALIGN offset. game.json gives romfs_bytes, data_bytes (the file's whole size) and dlc_at (none: no DLC in it), which GameStart checks.
# → the files made, the APK (or XAPK) first
GAME_APP_LIMIT = (2 << 30) - (64 << 20)   # an APK's size, kept clear of 2 GiB for the ZIP's own records
# A ZIP without ZIP64 also holds at most 65,535 entries, and a game's translated shaders are files of their own (Mario Kart 8 Deluxe's with its DLC passed that at 492 MB:
# droidplay-core issue #169). So a game app carries them as one entry, SPV_ZIP: a stored ZIP of the spv folder (ZIP64 allowed inside it: apksigner never reads it), which
# GameStart unpacks into games/<title ID>/spv/ at the first start with the rest, where the host reads them by path as from a launcher install. The other files are counted
# against GAME_APP_ENTRIES, which leaves room for the APK's own entries (classes.dex, the libraries, resources, the signature)
GAME_APP_ENTRIES = 65535 - 1024
SPV_ZIP = 'spv.zip'
DLC_MAGIC, DLC_ALIGN = b'DMAOCPK1', 4096


def game_app_limit():   # NXNATIVE_GAME_APP_LIMIT=<bytes>: a smaller limit, so that a smaller game takes a big one's layout (a device check of the DLC beside the APK)
    v = os.environ.get('NXNATIVE_GAME_APP_LIMIT', '').strip()
    return int(v) if v.isdigit() else GAME_APP_LIMIT


def gib(n): return f'{n / (1 << 30):.2f} GiB'


# Where a game app's RomFS and DLC go: → (RomFS in the APK, DLC in the APK). parts: the files the APK must carry, by name (code, shaders, font, the rest), with their
# sizes; files: how many entries each makes, dlc_files the DLC's. The RomFS leaves the APK first, then the DLC with it; what the APK must carry and can't, in bytes or in
# entries, is a BuildError naming each part
def game_app_plan(parts, dlc, rom, xapk, limit=GAME_APP_LIMIT, files=None, dlc_files=0, entries=GAME_APP_ENTRIES, beside=False):   # beside: the RomFS out whatever its size
    core = sum(parts.values()); n = sum((files or {}).values())
    if n >= entries:
        raise BuildError(f'a game app can\'t hold this game: the APK would carry {n:,} files ({", ".join(f"{k} {v:,}" for k, v in (files or {}).items())}), and an APK '
                         f'without ZIP64, which apksigner needs, holds at most 65,535 entries ({entries:,} of them for the game). '
                         'Install it through DroidPlay\'s library instead (without --game-apk), which has no such limit')
    if core >= limit:
        raise BuildError(f'a game app can\'t hold this game: the files the APK must carry are {gib(core)} ({", ".join(f"{k} {gib(v)}" for k, v in parts.items())}), and an APK '
                         f'holds {gib(limit)} (2 GiB less room for its records); the RomFS ({gib(rom)}) and the DLC ({gib(dlc)}) could go beside it, these can\'t. '
                         'Install it through DroidPlay\'s library instead (without --game-apk), which has no such limit')
    dlc_fits = core + dlc < limit and n + dlc_files < entries
    if not xapk and not beside and dlc_fits and core + dlc + rom < limit: return True, True
    return False, dlc_fits


def pack_spv(spv, dst):   # the spv folder → one stored ZIP (SPV_ZIP), its files named as in the folder; → how many
    import zipfile
    n = 0
    with zipfile.ZipFile(dst, 'w', zipfile.ZIP_STORED, allowZip64=True) as z:   # stored: the APK deflates its entries itself
        for r, ds, fs in os.walk(spv, followlinks=True):
            ds.sort()
            for f in sorted(fs): p = Path(r, f); z.write(p, p.relative_to(spv).as_posix()); n += 1
    return n


def dlc_images(aoc):   # → [(index, path)] of aoc/<index>.img, by index; a DLC kept as a folder (an older extraction) can't go into the pack
    out = []
    for p in sorted(Path(aoc).iterdir()) if aoc and Path(aoc).is_dir() else []:
        m = re.fullmatch(r'([1-9][0-9]*)(\.img)?', p.name)
        if not m: continue
        if not m.group(2) or not p.is_file(): raise BuildError(f'the DLC {p} is a folder, and a game app carries its DLC beside the APK as images: extract the game again')
        out.append((int(m.group(1)), p))
    return sorted(out)


def dlc_pack(images):   # → the pack's header and its records [(index, offset from the pack's start, size)], and its length
    head = len(DLC_MAGIC) + 8 + 24 * len(images); off = -(-head // DLC_ALIGN) * DLC_ALIGN; recs = []
    for i, p in images:
        size = Path(p).stat().st_size; recs.append((i, off, size)); off = -(-(off + size) // DLC_ALIGN) * DLC_ALIGN
    end = recs[-1][1] + recs[-1][2] if recs else head
    return DLC_MAGIC + struct.pack('<II', len(recs), 0) + b''.join(struct.pack('<IIQQ', i, 0, o, n) for i, o, n in recs), recs, end


def write_data(dst, rom, images):   # the data file beside a game app: the RomFS (an APFS clone where the volume makes one, never a hard link: the DLC is appended), then the DLC pack
    rom_bytes = Path(rom).stat().st_size; at = -(-rom_bytes // DLC_ALIGN) * DLC_ALIGN
    head, recs, _ = dlc_pack(images); Path(dst).unlink(missing_ok=True)
    if not (sys.platform == 'darwin' and subprocess.run(['cp', '-c', str(rom), str(dst)], capture_output=True).returncode == 0): shutil.copyfile(rom, dst)
    with open(dst, 'r+b') as f:
        f.truncate(at); f.seek(at); f.write(head)
        for (i, p), (_, off, size) in zip(images, recs):
            f.seek(at + off)
            with open(p, 'rb') as src: shutil.copyfileobj(src, f, 16 << 20)
    return at


# The game's defaults.txt: the language, then the profile's Android switches (dm/games/<id>/android_defaults.txt: KEY=VALUE per line, # comments), for a game that needs
# a switch only some games want. The host takes them as it takes env.txt's, without overriding a key env.txt or the settings screen sets (front_android.c); a game app
# sets them the same way (GameStart.java)
def defaults_text(g, lang, romfs=None):   # romfs: the RomFS image's path on the phone when it is not in the game's folder (--romfs-dir); the launcher reads defaults.txt before it sets its own
    lines = [f'DM_LANG={lang}'] + [f'DM_ROMFS={romfs}'] * bool(romfs)
    own = g.dir / 'android_defaults.txt'
    if own.is_file(): lines += [ln.strip() for ln in own.read_text(encoding='utf-8').splitlines() if '=' in ln and not ln.lstrip().startswith('#')]
    return ''.join(ln + '\n' for ln in lines)


def make_game_app(sdk, g, game, patched, spv, font, aoc, defaults, so, meta, work, logs, env, out_dir, xapk, beside=False):
    sys.path.insert(0, str(REPO / 'dm/android/tools')); import build_apk   # pyright: ignore[reportMissingImports]  # dm/android/tools, put on the path here
    pkg = build_apk.game_package(g.id)
    rom = bi.rfs.source(game)
    if not bi.rfs.is_image(rom): raise BuildError(f'a game app reads the RomFS as one image ({game}/RomFS.img), and this extraction has a RomFS folder: extract the game again')
    stage = work / 'game_app'; shutil.rmtree(stage, ignore_errors=True); stage.mkdir(parents=True)
    def walk(p): return [(Path(r, f), Path(r, f).stat().st_size) for r, _, fs in os.walk(p, followlinks=True) for f in fs] if p and Path(p).is_dir() else []
    for name, src in (('code', patched), ('font', font)):
        if src and Path(src).is_dir(): stage_link(stage / name, Path(src).resolve())
    if spv and Path(spv).is_dir(): bi.PROG.set(step='packing the shaders into one file'); pack_spv(spv, stage / SPV_ZIP)
    for f in ('icon.jpg', 'control.nacp'):
        if (game / f).is_file(): stage_link(stage / f, (game / f).resolve())
    (stage / 'defaults.txt').write_text(defaults, encoding='utf-8', newline='\n')   # \n on Windows too, as the phone's files are
    staged = [(f.relative_to(stage).as_posix(), n) for f, n in walk(stage)]
    parts = {'code': 0, 'shaders': 0, 'font': 0, 'the rest': 4096 + sum(len(f) + 1 for f, _ in staged)}   # the rest: icon, control data, defaults; game.json and files.txt
    files = {'code': 0, 'shaders': 0, 'font': 0, 'the rest': 2}
    for f, n in staged:
        k = {'code': 'code', SPV_ZIP: 'shaders', 'font': 'font'}.get(f.split('/')[0], 'the rest'); parts[k] += n; files[k] += 1
    rom_bytes = Path(str(rom)).stat().st_size; dlc_walk = walk(aoc); dlc = sum(n for _, n in dlc_walk)
    inside, dlc_inside = game_app_plan(parts, dlc, rom_bytes, xapk, game_app_limit(), files, len(dlc_walk), beside=beside)
    if dlc_inside and dlc: stage_link(stage / 'aoc', Path(aoc).resolve())
    out_images = dlc_images(aoc) if dlc and not dlc_inside else []
    dlc_at = -(-rom_bytes // DLC_ALIGN) * DLC_ALIGN if out_images else 0
    data_bytes = (dlc_at + dlc_pack(out_images)[2]) if out_images else rom_bytes
    info = {**meta, 'package': pkg, 'data_bytes': data_bytes, **({'dlc_at': dlc_at} if dlc_at else {})}
    (stage / 'game.json').write_text(json.dumps(info, indent=1), encoding='utf-8', newline='\n')
    files = sorted(Path(r, f).relative_to(stage).as_posix() for r, _, fs in os.walk(stage, followlinks=True) for f in fs)
    (stage / 'files.txt').write_text(''.join(f + '\n' for f in files + ['files.txt']), encoding='utf-8', newline='\n')
    out_dir.mkdir(parents=True, exist_ok=True)
    base = safe_name(meta.get('name') or g.id) or g.id
    apk = (work / f'{pkg}.apk') if xapk else out_dir / f'{base}.apk'
    icon = game / 'icon.jpg'
    bi.PROG.set(step='packing the APK' + (' with the RomFS inside' if inside else ''))
    bi.run([sys.executable, REPO / 'dm/android/tools/build_apk.py', '--sdk', sdk, '--game-app', g.id, '--so', so, '--game-dir', stage,
            *[x for f in driver_libs(Path(so).parent) for x in ('--lib', f)], *(['--romfs', str(rom)] if inside else []), '--keystore', KEYSTORE, '-o', apk, '--label', meta.get('name') or g.id,
            *(['--icon', icon] if icon.is_file() else [])], logs / 'android_game_apk.log', env=env)
    Path(f'{apk}.idsig').unlink(missing_ok=True)   # apksigner's v4 signature, for adb's incremental installs only: clutter beside an app to share
    if xapk:
        out = out_dir / f'{base}.xapk'; obb = f'Android/obb/{pkg}/main.1.{pkg}.obb'
        manifest = {'xapk_version': 2, 'package_name': pkg, 'name': meta.get('name') or g.id, 'locales_name': {}, 'version_code': '1', 'version_name': '0.1',
                    'min_sdk_version': '29', 'target_sdk_version': '34', 'permissions': ['android.permission.VIBRATE'],
                    'total_size': apk.stat().st_size + data_bytes, 'expansions': [{'file': obb, 'install_location': 'EXTERNAL_STORAGE', 'install_path': obb}]}
        if icon.is_file(): manifest['icon'] = 'icon.jpg'
        tmp = out.with_suffix('.xapk.part')
        import zipfile
        with zipfile.ZipFile(tmp, 'w', zipfile.ZIP_STORED) as z:   # stored: the APK is compressed already, and the RomFS is read where it lies
            z.writestr('manifest.json', json.dumps(manifest, indent=1, ensure_ascii=False))
            if icon.is_file(): z.write(icon, 'icon.jpg')
            z.write(apk, f'{pkg}.apk')
            with z.open(zipfile.ZipInfo(obb, datetime.datetime.now().timetuple()[:6]), 'w', force_zip64=True) as dst:   # the OBB: the RomFS, then the DLC pack when the APK can't hold it
                done = 0
                def put(path):
                    nonlocal done
                    with open(str(path), 'rb') as src:
                        while chunk := src.read(16 << 20):
                            dst.write(chunk); done += len(chunk); bi.PROG.set(step='packing the XAPK', done=done, total=data_bytes, unit='B')
                put(rom)
                if out_images:
                    head, recs, _ = dlc_pack(out_images)
                    dst.write(b'\0' * (dlc_at - rom_bytes)); dst.write(head); pos = len(head)
                    for (_, p), (_, off, n) in zip(out_images, recs): dst.write(b'\0' * (off - pos)); put(p); pos = off + n
        tmp.replace(out); apk.unlink(missing_ok=True); made = [out]
    else:   # an APK and the data file beside it, or an APK with the RomFS inside, whichever the game's size gave: a .romfs left from the other way goes
        made = [apk]; romfs = out_dir / f'{base}.romfs'
        if inside: romfs.unlink(missing_ok=True)
        else:
            if out_images: write_data(romfs, rom, out_images)
            else: clone(str(rom), romfs)
            made.append(romfs)
    if out_images: say(f'the DLC ({len(out_images)} image{"s" * (len(out_images) != 1)}, {gib(dlc)}) is in {made[-1].name} after the RomFS: the APK can\'t hold it; the app copies it out at its first start')
    return made, inside


def core_version():   # DroidPlay Core's version, for the launcher app's: the VERSION file a release's archives carry (release-assets.yml), else git describe in a checkout
    if (REPO / 'VERSION').is_file(): return (REPO / 'VERSION').read_text(encoding='utf-8').strip() or None
    try: r = subprocess.run(['git', '-C', str(REPO), 'describe', '--tags', '--always'], capture_output=True, text=True, timeout=10,
                            creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))   # the desktop app asks too, from pythonw.exe: no console window for git
    except (OSError, subprocess.TimeoutExpired): return None
    return (r.stdout.strip().removeprefix('v') or None) if r.returncode == 0 else None


def make_apk(sdk, logs, env):   # the app with every host in the library → this run's own copy, which it installs; the library's nxnative.apk becomes the same app
    lib = data_dir(); hosts = library()
    loader, st = lib / 'loader' / 'libnxloader.so', lib / 'selftest'
    if not hosts or not loader.is_file() or not st.is_dir(): raise BuildError(f'no game host built on this computer yet ({lib}): build a game first')
    KEYSTORE.parent.mkdir(parents=True, exist_ok=True)
    # Packed under a name of this process's own: two builds in parallel (other sessions, other phones) packed and installed the one nxnative.apk, so a build's install
    # could get the other's half-written file (apksigner failed with "offset (16777216) > source size (0)")
    apk, own = lib / 'nxnative.apk', lib / f'nxnative.{os.getpid()}.apk'
    atexit.register(own.unlink, missing_ok=True)
    bi.run([sys.executable, REPO / 'dm/android/tools/build_apk.py', '--sdk', sdk, '--loader', loader, *[x for so, _ in hosts.values() for x in ('--so', so)],
            *[x for f in driver_libs(loader.parent) for x in ('--lib', f)], '--selftest', st, '--keystore', KEYSTORE, '-o', own,
            *(['--version-name', v] if (v := core_version()) else [])], logs / 'android_apk.log', env=env)
    if own.is_file(): tmp = lib / f'.nxnative.{os.getpid()}.apk'; shutil.copyfile(own, tmp); os.replace(tmp, apk)   # whole or not at all, for anyone who takes it from there
    names = [m.get('name') or gid for gid, (_, m) in hosts.items()]
    say(f'app: {", ".join(names)} ({apk})')
    return own


# ---------------- The phone ----------------
WAIT_DEVICE = 0.0   # --wait-device


_keys = {}   # serial → the phone's ID, or None when it gave none: asked once a run
_hint = {}   # serial → the ID the desktop app knew it by (--phone-id): the phone's ID while it doesn't answer (its port changed before the build began)


def phone_key(serial):   # the phone's stable ID (dm/android/tools/phone_id.py), or its serial when it gives none: what its lock and its logs folder are named after,
    # so a phone on a cable and on Wi-Fi at once is one device with one lock, and a Wi-Fi phone keeps its folder when its port changes
    if serial not in _keys:
        try: _keys[serial] = phone_id.from_props(adb(serial, 'shell', phone_id.ID_CMD, check=False, timeout=15))
        except (OSError, BuildError): _keys[serial] = None   # no adb at all, or no phone: the step that needs it says so
        if _hint.get(serial) and _keys[serial] and _keys[serial] != _hint[serial]:   # the address went to another phone (Wi-Fi addresses are lent out again)
            raise BuildError(f'{serial} is another phone now: pick the phone again')
        _keys[serial] = _keys[serial] or _hint.get(serial)
    return _keys[serial] or serial


@contextlib.contextmanager
def phone(serial, what):   # the device to this build while it installs or runs something there (dm/android/tools/device_lock.py): a bench's runs on it, or another build's
    # install, would be force-stopped by this one, and this one's by them. Not held while it builds on the computer
    sys.path.insert(0, str(REPO / 'dm/android/tools')); import device_lock   # pyright: ignore[reportMissingImports]
    try: lock = device_lock.held(phone_key(serial), what, WAIT_DEVICE, say); lock.__enter__()
    except device_lock.DeviceBusy as e: raise BuildError(str(e)) from None
    try: yield
    finally: lock.__exit__(None, None, None)


def pick_device():
    """The one device adb lists as ready ('device'): an offline Wi-Fi entry left from an earlier connection does not count, and a phone on a cable and on Wi-Fi
    at once is one device, used over the cable"""
    out = subprocess.run(['adb', 'devices'], capture_output=True, text=True, errors='replace').stdout.splitlines()[1:]
    rows = [l.split() for l in out if l.strip()]
    ready = [r[0] for r in rows if len(r) > 1 and r[1] == 'device']
    if len(ready) > 1: ready = [d['serial'] for d in phone_id.one_per_phone([{'serial': r, 'id': phone_key(r)} for r in ready])]
    if len(ready) == 1: return ready[0]
    listed = ', '.join(f'{r[0]} ({r[1]})' for r in rows) or 'none'
    raise BuildError(f'{len(ready)} adb devices ready ({listed}): connect one, or pick it with --serial')


class PhoneLost(BuildError):   # adb lost the phone, or the phone stopped answering, as opposed to a command that failed: survives_drops waits for it and goes on
    def __init__(self, msg, why):
        super().__init__(msg); self.why = why


# adb's own words when it has lost the phone, not the command's: a Wi-Fi phone that slept, roamed or restarted wireless debugging, or a cable that came out
LOST = re.compile(r"device '[^']*' not found|device offline|device still (connecting|authorizing)|no devices/emulators found|error: closed|connection reset|"
                  r"broken pipe|protocol fault|failed to (read|write|get feature set)|unexpected eof|connect failed", re.I)
SHELL_SECS = 120        # a shell step's longest wait: ours take seconds, and a phone gone silent over Wi-Fi held the build at one percentage until TCP gave up (minutes)
COPY_RATE = 512 << 10   # bytes a second: a copy slower than this (a tenth of a poor Wi-Fi link) counts as lost, so its wait is 60 s plus its bytes at this rate
STALL_SECS = 90         # a copy whose byte count stops growing for this long is stopped and counted as lost (counting), well before its own wait runs out
_now = {}               # the serial a run took the phone on → the one it answers on since it came back (reacquire): every later call goes there
_running, _running_lock = set(), threading.Lock()   # the adb processes running now, for the stall watchdog to stop


def adb_timeout(a):   # a call's longest wait, in seconds
    if a[:1] in (('push',), ('install',)):   # its local files' bytes at COPY_RATE (a push's last argument is the phone's path)
        n = sum(sum(f.stat().st_size for f in p.rglob('*') if f.is_file()) if p.is_dir() else p.stat().st_size if p.is_file() else 0
                for p in (Path(str(x)) for x in (a[1:-1] if a[0] == 'push' else a[1:])))
        return 60 + n / COPY_RATE
    return 1800 if a[:1] == ('pull',) else SHELL_SECS   # a pull's size isn't known here: the Oodle job's output and the captures are the largest


def run_adb(args, log=None, check=True, timeout=None):   # adb with these arguments (-s <serial> among them, as given) → its output
    cmd = ['adb'] + [str(x) for x in args]
    a = tuple(cmd[3:] if cmd[1:2] == ['-s'] else cmd[1:])
    t = adb_timeout(a) if timeout is None else timeout
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, errors='replace', env=dict(os.environ, MSYS_NO_PATHCONV='1'))
    with _running_lock: _running.add(p)
    try: out, err = p.communicate(timeout=t)
    except subprocess.TimeoutExpired:
        p.kill(); out, err = p.communicate(); p.stalled = f'no answer from the phone in {t:.0f} s'   # pyright: ignore[reportAttributeAccessIssue]
    finally:
        with _running_lock: _running.discard(p)
    if log: log.write_text(out + err)
    text, stalled = (out + err).strip(), getattr(p, 'stalled', '')
    if stalled or (p.returncode and (LOST.search(text) or (p.returncode == 255 and not text))):
        why = stalled or (text.splitlines()[-1][:200] if text else f'exit status {p.returncode} with no output')
        raise PhoneLost(f'{" ".join(cmd)}: adb lost the phone: {why}', why)
    # the exit status too: a step that printed nothing left only "failed:" (droidplay-core issue #37), with no way to tell a missing program (127), adb losing the phone (255)
    # and a command that failed (1)
    if p.returncode and check: raise BuildError(f'{" ".join(cmd)} failed (exit status {p.returncode}): {text[-400:] if text else "no output"}')
    return out


def adb(serial, *a, log=None, check=True, timeout=None):   # timeout: the longest wait in seconds (default adb_timeout); PhoneLost when adb loses the phone, whatever check
    serial = _now.get(serial, serial)
    return run_adb((['-s', serial] if serial else []) + list(a), log=log, check=check, timeout=timeout)


def stop_stalled(why):   # the stall watchdog's (counting): every adb process running now is stopped, and its call raises PhoneLost
    with _running_lock: ps = list(_running)
    for p in ps:
        if p.poll() is None: p.stalled = why; p.kill()


# ---------------- A phone lost on the way ----------------
# Over Wi-Fi a phone drops off when it sleeps, roams to another access point or restarts wireless debugging (a new port), and comes back as the same serial, as
# another address, or by its mDNS name. An install waits for it by its ID (phone_key), connects to it again where it can, and takes the step it was on again
RECONNECT_WAIT = 180.0   # --reconnect-wait: how long a lost phone is waited for before the install stops
MAX_DROPS = 5            # more losses than this in a row with nothing copied between them stop the run: the link is too poor to finish over. A count per run
# stopped a real one: Link's Awakening over a Wi-Fi link that dropped after every few 256 MiB pieces, 5.9 of its 6.2 GB on the phone and still moving
_drops, _streak, _progress, _streak_at = 0, 0, 0, -1   # losses this run; losses in a row; pieces copied so far (moved); _progress at the last loss


def moved():   # a piece of a large copy reached the phone: the next loss starts a new run of them
    global _progress; _progress += 1


def ask_id(serial):   # a serial adb lists now → the phone's ID, or None; kept only when it answered
    if _keys.get(serial): return _keys[serial]
    try: pid = phone_id.from_props(run_adb(['-s', serial, 'shell', phone_id.ID_CMD], check=False, timeout=10))
    except BuildError: return None
    if pid: _keys[serial] = pid
    return pid


def answers(serial):
    try: return run_adb(['-s', serial, 'shell', 'echo ok'], check=False, timeout=10).strip() == 'ok'
    except BuildError: return False


def reacquire(serial, why):   # → the serial the phone answers on once it is back (every later call goes there); BuildError after RECONNECT_WAIT
    phone_key(serial); want, cur = _keys.get(serial), _now.get(serial, serial)   # want: the ID read when the run took the phone (a USB phone's is its serial)
    usb = not phone_id.wifi(cur)
    how = 'plug it back in' if usb else 'wake it, and keep it on the same network as this computer with Wireless debugging on'
    say(f'[RED] adb lost the phone ({why}): waiting up to {RECONNECT_WAIT:.0f} s for it to come back ({how}); the copy goes on from there')
    end = time.monotonic() + RECONNECT_WAIT
    while True:
        bi.PROG.set(note=f'waiting for the phone to come back: {how}')
        try: listed = {r[0]: r[1] for r in (ln.split() for ln in run_adb(['devices'], check=False, timeout=15).splitlines()[1:]) if len(r) > 1}
        except BuildError: listed = {}
        for s, st in listed.items():   # its own serial again (a cable plugged back in, a connection that came back), or another one with its ID (a new port, its mDNS name)
            if st == 'device' and (s == cur or (want and ask_id(s) == want)) and answers(s):
                _now[serial] = s; say('the phone is back' + ('' if s == cur else f' as {s}') + ': going on'); return s
        if not usb:   # a connection adb still lists that answers nothing is dropped and made again; the phone's address on the network, by its serial number, too
            with contextlib.suppress(BuildError):
                if cur in listed: run_adb(['disconnect', cur], check=False, timeout=10)
                if phone_id.transport(cur) == 'tcp': run_adb(['connect', cur], check=False, timeout=10)
                for sn, addr in phone_id.mdns_services(run_adb(['mdns', 'services'], check=False, timeout=10)):
                    if sn == want: run_adb(['connect', addr], check=False, timeout=10)
        if time.monotonic() >= end:
            raise BuildError(f'the phone did not come back within {RECONNECT_WAIT:.0f} s after adb lost it ({why}). ' + ('Plug it back in' if usb else
                             'Wake it, check that it is on the same network as this computer and that Wireless debugging is still on') +
                             ', then install again: the copy goes on from what the phone holds')
        time.sleep(3)


def survives_drops(f):   # f taken again once the phone is back, when adb lost it on the way: each of these steps can run again, and the large copies go on from what
    # the phone holds (push_resumable's part, push_tar's manifest)
    @functools.wraps(f)
    def run(serial, *a, **k):
        global _drops, _streak, _streak_at
        while True:
            try: return f(serial, *a, **k)
            except PhoneLost as e:
                _drops += 1; _streak = _streak + 1 if _progress == _streak_at else 1; _streak_at = _progress
                if _streak > MAX_DROPS:
                    raise BuildError(f'{e}\nadb lost the phone {_streak} times in a row with nothing copied in between, so the connection is too poor to finish over: ' +
                                     ('move the phone closer to the router (5 GHz), or use a USB cable' if phone_id.wifi(_now.get(serial, serial)) else 'try another cable or USB port') +
                                     ', then install again') from e
                reacquire(serial, e.why)
    return run


def sh_quote(s): return "'" + s.replace("'", "'\\''") + "'"


# adb push prints no progress when its output is not a terminal: a copy's count is read from the phone while it runs (the files under the destination folder, or the
# size of the file being written), so the desktop app sees it move
def remote_files(serial, d):
    n = adb(serial, 'shell', f'find {sh_quote(d)} -type f 2>/dev/null | wc -l', check=False, timeout=30).strip()
    return int(n) if n.isdigit() else None


def remote_size(serial, f):
    n = adb(serial, 'shell', f'stat -c %s {sh_quote(f)} 2>/dev/null', check=False, timeout=15).strip()
    return int(n) if n.isdigit() else None


# A phone that fills up in the middle of an install fails at whatever step comes next, often with nothing printed (droidplay-core issue #37: the folder script, after an install
# that ran out of room). The large copies check the room first, and a failed install on a nearly full phone says how full it is
ROOM_SPARE = 256 << 20   # left free besides a copy's own bytes: Android, the app's saves and its log need room too
LOW_ROOM = 1 << 30       # a failed install on a phone with less than this free names the free space as the likely cause


def size_text(n): return f'{n / 1e9:.1f} GB' if n >= 1e9 else f'{max(n, 0) / 1e6:.0f} MB'


def remote_free(serial, d):   # bytes free on the phone's storage where folder d is, or None
    row = adb(serial, 'shell', f'df -k {sh_quote(d)} 2>/dev/null | tail -n 1', check=False).split()
    return int(row[3]) * 1024 if len(row) >= 4 and row[3].isdigit() else None


def need_room(serial, d, need, what):
    free = remote_free(serial, d)
    if free is None or free >= need + ROOM_SPARE: return
    raise BuildError(f'the phone has {size_text(free)} free: {what} needs {size_text(need)}, plus {size_text(ROOM_SPARE)} left free for Android and the app. '
                     f'Free up {size_text(need + ROOM_SPARE - free)} on the phone, then install again')


def says_when_phone_full(f):   # a failed step's error, plus the phone's free space when that is low (a full phone fails steps that print nothing)
    @functools.wraps(f)
    def run(serial, *a, **k):
        try: return f(serial, *a, **k)
        except PhoneLost: raise   # no phone to ask
        except BuildError as e:
            try: free = remote_free(serial, REMOTE)
            except BuildError: free = None   # the phone gone (reacquire gave up): the error as it is
            if free is None or free >= LOW_ROOM or 'free up' in str(e).lower(): raise
            raise BuildError(f'{e}\nThe phone has only {size_text(free)} free, which is likely why: free up space on the phone '
                             f'(a game\'s RomFS.img.partial is a copy cut short, kept so the next install goes on from it), then install again') from e
    return run


@contextlib.contextmanager
def counting(note, total, unit, read):   # while the block runs: done = read() (None: not known yet) of total, in unit ('B': bytes)
    # A byte count that stops growing for STALL_SECS stops the copy, which then counts as lost (a phone gone silent over Wi-Fi). Not a count of files: one large
    # file can take minutes on its own
    bi.PROG.set(done=0, total=total, unit=unit, note=note)
    seen = {'done': -1, 'at': time.monotonic()}
    def poll():
        try: v = read()
        except BuildError: v = None   # the phone not answering: no count, and the clock runs on
        now = time.monotonic()
        if v is not None and v > seen['done']: seen.update(done=v, at=now)
        elif unit == 'B' and now - seen['at'] > STALL_SECS: stop_stalled(f'the copy made no progress in {STALL_SECS} s'); seen['at'] = now
        return None if v is None else {'done': min(v, total)}
    bi.PROG.watch(poll)
    try: yield
    finally: bi.PROG.watch(None)


TAR_FILES = 200   # a folder with more files than this goes over as one tar archive (push_tar)
MANIFEST = '.nxnative_manifest'   # in a folder copied as a tar: each file's size and modification time, for the next copy to send only what changed


def local_room(f, need, what):   # room on this computer for a file of need bytes the copy writes at f (what it holds now counts, as it is overwritten), or a plain stop
    free = free_on(f.parent)
    if free is None: return
    have = f.stat().st_size if f.is_file() else 0
    if free + have >= need + LOCAL_SPARE: return
    raise BuildError(f'this computer has {size_text(free)} free on {drive_of(f)}: {what} needs {size_text(need)} there for the copy to the phone ({f}), plus '
                     f'{size_text(LOCAL_SPARE)} left free. Free up {size_text(need + LOCAL_SPARE - free - have)} on that drive, then install again')


LOCAL_SPARE = 64 << 20   # left free on this computer besides a copy's piece


def tar_pieces(local, rels, most):   # rels (sorted) → lists of them of about most bytes each, in order; a larger file alone
    out, cur, n = [], [], 0
    for r in rels:
        size = (local / r).stat().st_size
        if cur and n + size > most: out.append(cur); cur, n = [], 0
        cur.append(r); n += size
    return out + [cur] if cur else out


def push_tar(serial, local, parent, name, files, log, sync):
    # adb push sends a folder file by file: Super Mario Bros. Wonder's 37,393 shader files took over 20 minutes to a phone that had none (about 30 files a second).
    # Tar archives, unpacked by the phone's tar, take a minute or two. With sync, the manifest a copy left in the folder keeps it incremental, as push --sync was.
    # The files go in archives of about CHUNK bytes, each named in the manifest once the phone has unpacked it: one archive of every file (8.4 GB for Final Fantasy
    # Resonance) started again from nothing after a drop over Wi-Fi, needed twice its size free on the phone and as much again on this computer
    dst = f'{parent}/{name}'
    rows = {f.relative_to(local).as_posix(): f'{f.stat().st_size} {f.stat().st_mtime_ns}' for f in files}
    old = {}
    if sync:
        for line in adb(serial, 'shell', f'cat {sh_quote(dst + "/" + MANIFEST)} 2>/dev/null', check=False).splitlines():
            meta, _, rel = line.rstrip('\r').partition('\t')
            if rel: old[rel] = meta
    send = sorted(r for r in rows if old.get(r) != rows[r])   # sorted: the same pieces on every try, so a piece the phone has is not sent again
    kept = {r: m for r, m in rows.items() if old.get(r) == m}   # on the phone already, as its manifest says
    adb(serial, 'shell', 'mkdir', '-p', dst)
    pieces, total = tar_pieces(local, send, CHUNK), sum((local / r).stat().st_size for r in send)
    if send: need_room(serial, dst, total + min(CHUNK, total) + (1 << 20) * len(pieces), f'{name} ({len(send):,} files, sent in archives of up to {size_text(CHUNK)})')
    man, remote, tar, done = log.with_suffix('.manifest'), f'{parent}/.{name}.tar', log.with_suffix('.tar'), 0
    def manifest():   # what the phone holds now, for the next copy to send only the rest
        man.write_text(''.join(f'{v}\t{k}\n' for k, v in sorted(kept.items())), encoding='utf-8', newline='\n')
        adb(serial, 'push', man, f'{dst}/{MANIFEST}')
    for i, piece in enumerate(pieces, 1):
        bi.PROG.set(done=done, total=total, unit='B', note='packing' + (f' archive {i} of {len(pieces)}' if len(pieces) > 1 else ''))
        size = sum(-(-(local / r).stat().st_size // 512) * 512 + 512 for r in piece) + 1024   # the archive: each file in 512-byte blocks after its header, then two closing
        local_room(tar, -(-size // 10240) * 10240, f'{name}\'s archive')                      # blocks, in tarfile's 10 KiB records
        try:   # packing in it: an archive cut short by a full disk is removed too
            with tarfile.open(tar, 'w', format=tarfile.USTAR_FORMAT) as t:
                for r in piece:
                    ti = t.gettarinfo(local / r, arcname=r); ti.mode = 0o666; ti.uid = ti.gid = 0; ti.uname = ti.gname = ''
                    with open(local / r, 'rb') as fh: t.addfile(ti, fh)
            with counting('sending' + (f' archive {i} of {len(pieces)}' if len(pieces) > 1 else ''), total, 'B',
                          lambda base=done: (lambda n: None if n is None else base + n)(remote_size(serial, remote))):
                adb(serial, 'push', tar, remote, log=log)
            bi.PROG.set(note='unpacking on the phone')
            adb(serial, 'shell', f'tar -xf {sh_quote(remote)} -C {sh_quote(dst)}', timeout=SHELL_SECS + tar.stat().st_size / (10 << 20))
        finally:
            with contextlib.suppress(BuildError): adb(serial, 'shell', 'rm', '-f', remote, check=False, timeout=15)   # a lost phone's: the next install removes it
            tar.unlink(missing_ok=True)
        done += sum((local / r).stat().st_size for r in piece); kept.update((r, rows[r]) for r in piece); moved()
        if i < len(pieces): manifest()
    manifest()   # the last one too when nothing was sent: files gone from this computer leave it
    say(f'{name}: {len(send):,} of {len(rows):,} files sent' + (f' in {len(pieces)} archives' if len(pieces) > 1 else ' in one archive' if pieces else '') +
        ('' if len(send) == len(rows) else ' (the others were on the phone already)'))


CHUNK = 256 << 20   # push_resumable's piece


def remote_hash(serial, remote, a, n):   # SHA-256 of n bytes at offset a (a multiple of 4 KiB) of a file on the phone, or None
    out = adb(serial, 'shell', f'dd if={sh_quote(remote)} bs=4096 skip={a // 4096} count={(n + 4095) // 4096} 2>/dev/null | head -c {n} | sha256sum', check=False).split()[:1]
    return out[0] if out else None


def same_ends(serial, local, remote):   # the first and last MiB of a local file and of the same-size file on the phone hash the same
    size = local.stat().st_size
    with open(local, 'rb') as f:
        for a in sorted({0, max(0, size - (1 << 20)) // 4096 * 4096}):
            f.seek(a); n = min(1 << 20, size - a)
            if remote_hash(serial, remote, a, n) != hashlib.sha256(f.read(n)).hexdigest(): return False
    return True


@survives_drops
def push_resumable(serial, local, remote, log):
    # A large file (a game's RomFS.img, gigabytes) in pieces appended to <remote>.partial, renamed when whole: adb push cannot resume a file, and wireless adb dropped
    # twice in the middle of Bowser's Fury's 2.8 GB image, each time from zero. A copy cut short goes on from what the phone holds, once the last MiB of it matches
    size, part = local.stat().st_size, remote + '.partial'
    have = remote_size(serial, part) or 0
    if have > size: have = 0
    if have:
        a = max(0, have - (1 << 20)) // 4096 * 4096   # the phone's last MiB (from a 4 KiB boundary), hashed on both sides
        theirs = adb(serial, 'shell', f'dd if={sh_quote(part)} bs=4096 skip={a // 4096} count={(have - a + 4095) // 4096} 2>/dev/null | head -c {have - a} | sha256sum', check=False).split()[:1]
        with open(local, 'rb') as f: f.seek(a); ours = hashlib.sha256(f.read(have - a)).hexdigest()
        if theirs != [ours]: say(f'{remote}: the part on the phone ({have:,} bytes) differs from this file: sent again from the start'); have = 0
        else: say(f'{remote}: {have:,} of {size:,} bytes on the phone already: going on from there')
    if not have: adb(serial, 'shell', f': > {sh_quote(part)}')
    if have < size:   # the part grows to the whole file, and each piece is on the phone twice for a moment (as itself, then appended)
        folder, file = remote.rsplit('/', 1)
        need_room(serial, folder, size - have + min(CHUNK, size - have), f'{file} ({size_text(size - have)} left to send)')
    piece, tmp = log.with_suffix('.piece'), remote + '.piece'
    if have < size: local_room(piece, min(CHUNK, size - have), f'{remote.rsplit("/", 1)[1]}\'s piece')   # Pokopia's full disk was found only at piece.write_bytes (#307)
    try:   # the piece goes whatever stops the copy: a full disk, a lost phone (survives_drops takes it again), an adb error
        with counting('sending', size, 'B', lambda: (lambda a, b: None if a is None else a + (b or 0))(remote_size(serial, part), remote_size(serial, tmp))):
            with open(local, 'rb') as f:
                f.seek(have)
                while have < size:
                    data = f.read(min(CHUNK, size - have)); piece.write_bytes(data)
                    adb(serial, 'push', piece, tmp, log=log)
                    adb(serial, 'shell', f'cat {sh_quote(tmp)} >> {sh_quote(part)} && rm {sh_quote(tmp)}')
                    have += len(data); moved()
    finally: piece.unlink(missing_ok=True)
    if remote_size(serial, part) != size: raise BuildError(f'{part} is not {size:,} bytes after the copy: run the install again')
    adb(serial, 'shell', f'mv -f {sh_quote(part)} {sh_quote(remote)}')


MKDIRS_DONE = 'nxnative_mkdirs_done'   # the last line push_dir's folder script prints


@survives_drops
def push_dir(serial, local, parent, name, log, sync=False):
    files = [f for f in local.rglob('*') if f.is_file()]
    if len(files) > TAR_FILES: return push_tar(serial, local, parent, name, files, log, sync)
    # <local folder> → <parent>/<name>: adb push copies a folder by its own name into the remote parent ("<folder>/." makes "/./" paths that adb cannot create
    # subfolders under on Windows); a folder named otherwise is pushed under its own name and renamed. adbd may not create folders under Android/data (its FUSE
    # mount refuses them: "secure_mkdirs() failed"), while the shell may: the whole tree is made first by a mkdir script run in the shell
    need_room(serial, parent, sum(f.stat().st_size for f in files), name)
    dirs = [f'{parent}/{local.name}'] + [f'{parent}/{local.name}/{d.relative_to(local).as_posix()}' for d in sorted(local.rglob('*')) if d.is_dir()]
    # set -e: the script's status was its last mkdir's, so one that failed in the middle went unnoticed until adb push stopped on the missing folder. The closing
    # line is checked too: an adb without the shell protocol reports 0 whatever the command did, and an empty script succeeds
    script = log.with_suffix('.mkdirs.sh')
    script.write_text('set -e\n' + ''.join(f'mkdir -p {sh_quote(d)}\n' for d in dirs) + f'echo {MKDIRS_DONE}\n', newline='\n')
    adb(serial, 'push', script, '/data/local/tmp/nxnative_mkdirs.sh')
    out = adb(serial, 'shell', 'sh', '/data/local/tmp/nxnative_mkdirs.sh')
    if MKDIRS_DONE not in out:
        raise BuildError(f'the folders for {name} were not made on the phone: the folder script stopped early, ' + (f'printing {out.strip()[-300:]}' if out.strip() else 'printing nothing'))
    if any(f.stat().st_size > CHUNK for f in files): push_files(serial, local, dirs[0], files, log, sync)   # DLC images (gigabytes): adb push started a file cut short
        # again from zero, so those go through the RomFS image's resumable copy
    else:
        with counting('sending', len(files), 'files', lambda: remote_files(serial, dirs[0])):
            adb(serial, 'push', *(['--sync'] if sync else []), local, f'{parent}/', log=log)
    if local.name != name:
        adb(serial, 'shell', 'rm', '-rf', f'{parent}/{name}'); adb(serial, 'shell', 'mv', f'{parent}/{local.name}', f'{parent}/{name}')


def push_files(serial, local, dst, files, log, sync):   # a folder's files one by one into dst (made already): those over CHUNK through push_resumable, kept when the
    # phone has the same (size, first and last MiB), the others by adb push (--sync with sync)
    for f in sorted(files):
        remote = f'{dst}/{f.relative_to(local).as_posix()}'
        if f.stat().st_size <= CHUNK: adb(serial, 'push', *(['--sync'] if sync else []), f, remote, log=log); continue
        if sync and remote_size(serial, remote) == f.stat().st_size and same_ends(serial, f, remote): say(f'{f.name}: on the phone already (same size, first and last MiB)'); continue
        push_resumable(serial, f, remote, log.with_name(f'{log.stem}.{f.name}.log'))


# Shaders a game compiles at run time with its own GLSLC: the app saves each new one on the phone (front_android.c DM_GLSLC_CAPTURE: games/<title ID>/glslc_capture, or
# glslc_capture/<title ID> where env.txt pointed it before), and every build or install of a game with a provisional profile pulls the ones this computer lacks into
# <version dir>/glslc_capture, where the shader split reads them. A registered profile's tables name the captures they were made from, so its games are left as they are
CAPTURE_DIRS = ('games/{id}/glslc_capture', 'glslc_capture/{id}')


def pull_captures(serial, gid, dest, logs):   # → how many new captures were copied to dest
    have = {f.name for f in dest.glob('*.glslc')} if dest.is_dir() else set()
    new = 0
    for rel in CAPTURE_DIRS:
        d = f'{REMOTE}/{rel.format(id=gid)}'
        names = [n.strip() for n in adb(serial, 'shell', f'ls {sh_quote(d)} 2>/dev/null', check=False).split() if re.fullmatch(r'[0-9a-f]{16}\.glslc', n.strip())]
        for n in names:
            if n in have: continue
            dest.mkdir(parents=True, exist_ok=True)
            adb(serial, 'pull', f'{d}/{n}', dest / n, log=logs / 'adb_pull_captures.log')
            have.add(n); new += 1
    if new: say(f'{new} new run-time shader(s) captured on the phone: copied to {dest}')
    return new


# A registered profile whose shader map names captures (build_ipa_from_nsp.py glslc_wanted: DRAGON QUEST BUILDERS 2, Fire Emblem: Three Houses, Portal, Portal 2, Hyrule
# Warriors: Definitive Edition, Catherine: Full Body) needs them in the split, or its table check stops the build. Only the Mac build captures them (capture_glslc boots the
# game in the Mac host). On Windows, Three Houses' build extracted the game and then stopped at the split with a table difference that named no cause (issue #441)
GLSLC_ISSUE = 'droidplay-core#441 (https://github.com/enjoyerofslop/droidplay-core/issues/441)'


def capture_folders(g, v, ex, nsp_dir, env):   # where version v's run-time captures may be, in the order taken: DM_GLSLC_CAPTURE (which the split then reads), this build's own
    # extraction (build_ipa_from_nsp.py captures into it when it ran on the same folder), then DroidPlay for Mac's build beside the NSP folder (<game>/<title ID>_build, where ue_decode looks too)
    if env.get('DM_GLSLC_CAPTURE'): return [Path(env['DM_GLSLC_CAPTURE'])]
    return [ex / v / 'glslc_capture', nsp_dir.parent / f'{g.id}_build' / 'extract' / v / 'glslc_capture']


def run_time_missing(g, want, sv, ex, nsp_dir, env, take=False):
    """The shader versions (sv) none of whose folders (capture_folders) holds every capture in want, the profile's shader map's. Asked before the extraction, which takes minutes,
    and again before the split, since a version extracted again has lost its own folder. take: a Mac build's captures are copied into this build's extraction, where the split reads them"""
    left = []
    for v in sv:
        have = next((d for d in capture_folders(g, v, ex, nsp_dir, env) if bi.glslc_done(d) >= want), None)
        own = ex / v / 'glslc_capture'
        if have is None: left.append(v)
        elif take and have != own and not env.get('DM_GLSLC_CAPTURE'):
            own.mkdir(parents=True, exist_ok=True)
            for n in sorted(want): shutil.copyfile(have / n, own / n)
            say(f'{v}: the {len(want)} shaders the game compiles while it runs, from the Mac build in {have.parents[2]}')
    return left


def run_time_stop(g, want, name):   # why a registered profile's game is not built: its shader tables name shaders the game compiles while it runs, and no game file holds them
    head = f'{name} compiles {len(want)} of its shaders while it runs, with its own shader compiler (GLSLC), and game profile {g.id}\'s shader tables need them. '
    if sys.platform == 'darwin':   # the Android build's modules can't boot in the Mac host (mac_decoder says why): the Mac build captures them, and this build takes them
        return head + ('Only a build for the Mac captures them so far: build the game for the Mac first (DroidPlay for Mac, or scripts/build_ipa_from_nsp.py on this folder), '
                       f'then build it for Android again, which takes them from that build. {GLSLC_ISSUE}')
    return head + f'Only a build on a Mac can capture them so far, so this game can\'t be built on Windows or Linux yet: {GLSLC_ISSUE}'


def run_time_note(on, name, gid, prov, want, derived, captured, game_app=False):
    """What the build says before its long steps about a game that compiles some of its shaders while it runs, or None. Until then only a provisional build whose split found
    no shaders at all said anything, at its end. on: the game files say it does (profile_guess.py shader_sources glslc_capture); want: the registered profile's shader map names
    them (a build from that profile takes them or stops: run_time_missing); captured: how many the packed version's extraction holds, which a derived profile's split reads"""
    if prov:   # the phone captures each one the first time the game compiles it (front_android.c DM_GLSLC_CAPTURE), and pull_captures brings them in
        if not on: return None
        if game_app: return (f'[RED] {name} compiles some of its shaders while it runs, so a game app built now may draw some things black, and it can\'t take them in later. '
                             'To pack them, install the game with DroidPlay first, play it to its first scene, run --update-shaders (DroidPlay for Windows: Manage › Update '
                             'shaders from the phone), then build the game app again: docs/android.md, "Games that compile their shaders while they run"')
        return (f'[RED] {name} compiles some of its shaders while it runs, so its first install may draw some things black. Play it to its first scene, then bring them in '
                'with --update-shaders (DroidPlay for Windows: Manage › Update shaders from the phone): docs/android.md, "Games that compile their shaders while they run"')
    if want:   # a profile derived from it splits its own tables, without whatever no capture in this extraction holds; --update-shaders pulls nothing for it
        return (f'[RED] {name} compiles {len(want)} of its shaders while it runs, and this build, from a profile derived from game profile {gid}, packs without them: '
                f'whatever they draw may be black. Only a build on a Mac captures them so far: {GLSLC_ISSUE}') if derived and not captured else None
    return (f'{name} has its own shader compiler (GLSLC), and game profile {gid}\'s shader tables list no shader it compiles while it runs, so none are pulled from the phone '
            'for it') if on else None


def open_tree(serial, d):
    # adb makes the folders as the shell user, drwxrws--- (group ext_data_rw): on Android 15 (a Galaxy S21 Ultra) the app could not enter them, found no game and ran its self test.
    # Opened to everyone; Android/data/<package> is reachable only by the app and the shell, so this gives no other app access
    # One chmod -R (rw for files, rwx for folders: X): find -exec chmod {} + overflowed the argument list on 37,000 shader files. The games folder too, which adb made the same way
    # A file the app made there itself (launcher.txt, its settings) is the app's: the shell may not chmod it ("Operation not permitted"), and the app needs no help to read it
    out = adb(serial, 'shell', f"chmod -R a+rwX {sh_quote(d)} 2>&1; chmod a+rwx {sh_quote(d.rsplit('/', 1)[0])} 2>&1", check=False, timeout=600)
    bad = [ln for ln in out.splitlines() if ln.strip() and 'Operation not permitted' not in ln]
    if bad: raise BuildError(f'opening {d} to the app failed: {"; ".join(bad)[:400]}')


@survives_drops
def install(serial, apk, logs):   # an update (-r): the app's data, saves included, stay. Never uninstalled here: that would delete the saves
    if not KEYSTORE.is_file(): raise BuildError(f'no signing key {KEYSTORE}: the app on the phone could not be updated')
    try: adb(serial, 'install', '-r', apk, log=logs / 'adb_install.log')
    except BuildError as e:
        if 'INSTALL_FAILED_UPDATE_INCOMPATIBLE' in str(e):
            raise BuildError(f'the DroidPlay app ({PKG}) on the phone was signed with another key than {KEYSTORE}. Uninstalling it deletes its saves and game files: '
                             f'back up {REMOTE} and the saves first (the launcher\'s Back up saves), then uninstall it by hand and run this again') from e
        raise


def remote_exists(serial, path): return adb(serial, 'shell', f'[ -e {sh_quote(path)} ] && echo yes', check=False).strip() == 'yes'


def migrate_legacy(serial, gid, nacp_local=None):
    """The one-game layout (files/game, before the launcher) → files/games/<gid>, with a legacy_save marker: the app then copies its internal files/save to saves/<gid>
    (Launcher.java migrate, after a backup; files/save itself is left as it was). With nacp_local, only when the phone's game is that one (same control.nacp)
    → whether it moved"""
    old, new = f'{REMOTE}/game', f'{REMOTE}/games/{gid}'
    if not remote_exists(serial, f'{old}/code/main.fixups.tsv'): return False
    if nacp_local is not None:
        theirs = adb(serial, 'shell', f'sha256sum {sh_quote(old + "/control.nacp")}', check=False).split()[:1]
        if theirs != [hashlib.sha256(Path(nacp_local).read_bytes()).hexdigest()]:
            say(f'[RED] the phone has another game in {old} (the layout before the launcher): left as it is; move it with --migrate --title <its title ID>'); return False
    if remote_exists(serial, new): raise BuildError(f'{new} exists already: the old {old} is left as it is (remove one of them by hand)')
    adb(serial, 'shell', f'mkdir -p {sh_quote(REMOTE + "/games")} && mv {sh_quote(old)} {sh_quote(new)} && touch {sh_quote(new + "/legacy_save")}')
    open_tree(serial, new)
    say(f'moved the one-game install {old} to {new}: the app copies its save to that game when it next opens (after a backup in files/backups/)')
    return True


ROMFS_SD = f'Android/data/{PKG}/files/romfs'   # --romfs-dir sd: the app's own folder on the card, which Android 11 and later let it read with no storage permission


def sd_card_ids(volumes, storage):   # the SD cards in `sm list-volumes public`'s output, else the card folders in `ls /storage`'s: their volume IDs (the desktop app asks the same, with its own adb calls)
    ids = [w[2] for w in (ln.split() for ln in volumes.splitlines()) if len(w) == 3 and w[0].startswith('public:') and w[1] == 'mounted' and w[2] != 'null']
    return ids or [n for n in storage.split() if re.fullmatch(r'[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}|[0-9A-Fa-f-]{16,}', n)]


def sd_card(serial):   # the phone's SD card, its volume ID (/storage/<ID>): a mounted public volume (sm list-volumes), else a card's folder in /storage
    out = adb(serial, 'shell', 'sm list-volumes public 2>/dev/null', check=False)
    ids = sd_card_ids(out, '')
    if not ids: ids = sd_card_ids('', adb(serial, 'shell', 'ls /storage 2>/dev/null', check=False))
    if not ids: raise BuildError('--romfs-dir sd: the phone has no SD card mounted (sm list-volumes, /storage): put one in, or give --romfs-dir a folder')
    if len(ids) > 1: raise BuildError(f'--romfs-dir sd: the phone has {len(ids)} cards ({", ".join(ids)}): give the folder, --romfs-dir /storage/<ID>/{ROMFS_SD}')
    return ids[0]


def romfs_path(serial, romfs_dir, gid):   # --romfs-dir → the RomFS image's path on the phone, <folder>/<title ID>/RomFS.img
    d = f'/storage/{sd_card(serial)}/{ROMFS_SD}' if romfs_dir.lower() == 'sd' else romfs_dir.rstrip('/')
    if not d.startswith('/') or '\n' in d: raise BuildError(f'--romfs-dir {romfs_dir}: a folder on the phone from / (such as /storage/<card ID>/{ROMFS_SD}), or sd for that one')
    if f'/Android/data/{PKG}/' not in d + '/':
        say(f'[RED] --romfs-dir {d} is outside the app\'s own folders (Android/data/{PKG}, on the phone or its card): Android 11 and later let the app read only those, '
            'so the game may not start (the log says so when it can\'t read the image)')
    return f'{d}/{gid}/RomFS.img'


def push_image(serial, rom, remote, log):   # the RomFS image → remote, kept when the phone holds the same one
    # there whole, the same size and the same first and last MiB: kept. Not adb push --sync, which also compares modification times: push_resumable's image is
    # assembled on the phone (cat >>), so its time is the copy's, and --sync sent the whole image again on every install (Danganronpa V3's 6.7 GB)
    if remote_size(serial, remote) == rom.stat().st_size and same_ends(serial, rom, remote): say(f'{remote.rsplit("/", 1)[1]}: on the phone already (same size, first and last MiB)')
    else: push_resumable(serial, rom, remote, log)


# ---------------- Saves from the computer ----------------
# adb can't write the app's internal storage, where the saves are (saves/<title ID>), so a save to swap in goes to files/import/<title ID>/. The launcher takes it at its
# start or when it comes back to the front (LauncherSaves.incoming), finds the save in it as an import from Android's file picker does (Saves.java: a DroidPlay backup,
# yuzu and its successors, Ryujinx, JKSV, EdiZon, Checkpoint, or the folder as it is), asks, keeps the game's save in files/backups/<time>-<title ID>/ and swaps the new
# one in. --import-save sends a folder under its own name (which can say whose save it is) or an archive's files; --restore-backup sends a game's save from a backup that
# Back up saves to this computer pulled, in the backup's own layout (saves/<title ID>/, xattrs.tsv)
IMPORT = f'{REMOTE}/import'
SAVE_DIR = re.compile(r'(?i)(0[1-7][0-9a-f]{14})(_device)?')   # Saves.TITLE: a title ID's folder, never a Ryujinx save ID (00…) or a system save's (80…)


def save_title_ids(root, own_name=True):   # the title IDs a save's files give, as Saves.java reads them: folder names (root's own too, with own_name), Checkpoint's 0x<id>, EdiZon's json, JKSV's meta, Ryujinx's ExtraData0
    ids = set()
    for p in [root] * own_name + [*root.rglob('*')]:
        if p.is_dir():
            m = re.fullmatch(r'(?i)(?:0x)?(0[1-7][0-9a-f]{14})(?:_device| .*)?', p.name)
            if m: ids.add(m[1].upper())
        elif p.name == 'edizon_save_metadata.json':
            m = re.search(r'"title_id"\s*:\s*"(0[1-7][0-9A-Fa-f]{14})"', p.read_text(encoding='utf-8', errors='replace'))
            if m: ids.add(m[1].upper())
        elif p.name in ('.nx_save_meta.bin', 'ExtraData0'):   # JKSV: "JKSV", a revision byte, then the title ID; Ryujinx: the title ID first
            b = p.read_bytes()[:13]; at = 5 if p.name.startswith('.') else 0
            if len(b) >= at + 8 and (at == 0 or b[:4] == b'JKSV') and any(b[at:at + 8]): ids.add(f'{struct.unpack_from("<Q", b, at)[0]:016X}')
    return ids


def unpack_save(src, dst):   # an archive's files → dst: Python's zipfile (Stored, Deflate, bzip2, LZMA), else tar (bsdtar, on Windows 10 and later and on a Mac: RAR, 7z)
    import zipfile
    shutil.rmtree(dst, ignore_errors=True); dst.mkdir(parents=True); method = None
    try:
        with zipfile.ZipFile(src) as z: z.extractall(dst)   # names that would leave dst are cut down to stay inside it
        return
    except NotImplementedError as e: method = e   # Deflate64, which Windows' own zip uses for big files: bsdtar (libarchive 3.7) doesn't read it either
    except zipfile.BadZipFile: pass   # not a zip
    shutil.rmtree(dst); dst.mkdir()
    try: r = subprocess.run(['tar', '-xf', str(src), '-C', str(dst)], capture_output=True, text=True, errors='replace')
    except OSError as e: raise BuildError(f"{src.name} isn't a zip, and there's no tar here to unpack it ({e}): unpack it with another tool and send the folder") from None
    if r.returncode and method: raise BuildError(f"{src.name} is packed with a compression Python and tar don't read ({method}): unpack it with Windows' Explorer "
                                                 "(Extract all), which wrote it, or another tool, then send the folder")
    if r.returncode:
        raise BuildError(f"{src.name} can't be unpacked here ({(r.stderr or r.stdout).strip()[-300:] or f'tar exit status {r.returncode}'}): unpack it with another tool and send the folder")


def send_save(serial, gid, local, parent, name, logs):
    dst = f'{IMPORT}/{gid}'
    adb(serial, 'shell', f'rm -rf {sh_quote(dst)} && mkdir -p {sh_quote(dst)}')
    push_dir(serial, local, parent, name, logs / 'adb_push_save.log')
    adb(serial, 'shell', f'find {sh_quote(dst)} -name {MANIFEST} -delete', check=False)   # a copy as a tar leaves its manifest, which isn't the save's
    open_tree(serial, dst)
    # the launcher takes it when it comes to the front: brought there now, unless a game is being played (a jump to the launcher would interrupt it). Single-top
    # (0x20000000), so a launcher in front already gets the intent (onNewIntent): a plain am start only brings its task forward, and it never looked
    if not adb(serial, 'shell', f'pidof {PKG}:game', check=False).strip(): adb(serial, 'shell', 'am', 'start', '-f', '0x20000000', '-n', f'{PKG}/.Launcher', check=False)


@survives_drops
@says_when_phone_full
def import_save(serial, gid, src, work, logs):   # a save folder or archive → files/import/<gid>/ on the phone
    gid = gid.upper(); src = Path(src).expanduser()
    if not src.exists(): raise BuildError(f'no such file or folder: {src}')
    if src.is_file(): local = work / 'save_import' / gid; unpack_save(src, local); parent, name = IMPORT, gid
    else: local, parent, name = src, f'{IMPORT}/{gid}', src.name
    if not any(f.is_file() for f in local.rglob('*')): raise BuildError(f'{src} holds no files')
    ids = save_title_ids(local, own_name=src.is_dir())   # an archive's folder here is named by gid
    if ids and gid not in ids:
        say(f'[RED] {src.name} gives the title ID{"s" if len(ids) > 1 else ""} {", ".join(sorted(ids))}, not {gid}: it may be another game\'s save. '
            'The app on the phone shows what it found and asks before swapping it in')
    send_save(serial, gid, local, parent, name, logs)
    say(f'sent to {IMPORT}/{gid}: DroidPlay on the phone shows the save it found for {gid} and asks before swapping it in (it keeps the save there in files/backups/ first)')


def backup_games(folder):   # title ID → (the backup folder, its saves/<id>[_device] folders): the newest by name of the backups under folder holding that game (Launcher.backup's layout)
    out = {}
    for saves in sorted(p for p in [Path(folder), *Path(folder).rglob('saves')] if p.name == 'saves' and p.is_dir()):
        parts = {}
        for k in saves.iterdir():
            m = SAVE_DIR.fullmatch(k.name)
            if m and k.is_dir() and any(f.is_file() for f in k.rglob('*')): parts.setdefault(m[1].upper(), []).append(k)
        for gid, ks in parts.items(): out[gid] = (saves.parent, ks)   # sorted: a later backup (its time) replaces an earlier one
    return out


@survives_drops
@says_when_phone_full
def restore_backup(serial, folder, gid, work, logs):   # a game's save (gid), or every game's, from a backup on this computer → files/import/<id>/ on the phone
    games = backup_games(folder)
    if not games: raise BuildError(f'{folder} holds no DroidPlay save backup: a folder with saves/<title ID>/ in it, as Back up saves makes')
    if gid and gid.upper() not in games: raise BuildError(f'{folder} holds no save of {gid.upper()}; it has {", ".join(sorted(games))}')
    on = set(adb(serial, 'shell', f'ls {sh_quote(REMOTE + "/games")} 2>/dev/null', check=False).upper().split())
    for g in [gid.upper()] if gid else sorted(games):
        if g not in on: say(f'{g}: not on the phone, so its save is left out'); continue
        root, ks = games[g]; local = work / 'save_restore' / g; shutil.rmtree(local, ignore_errors=True)
        for k in ks: shutil.copytree(k, local / 'saves' / (g + ('_device' if k.name.lower().endswith('_device') else '')))
        if (root / 'xattrs.tsv').is_file(): shutil.copy2(root / 'xattrs.tsv', local / 'xattrs.tsv')
        send_save(serial, g, local, IMPORT, g, logs)
        say(f'{g}: the save in {root} sent to {IMPORT}/{g}: DroidPlay on the phone asks before putting it back (it keeps the save there in files/backups/ first)')


@survives_drops
@says_when_phone_full
def push_game(serial, gid, game, patched, spv, font, aoc, defaults, work, logs, meta, romfs_at=None, romfs_push=True):
    """The game's files → files/games/<gid>/ (the RomFS with --sync: a repeat copies only what changed). romfs_at: the RomFS image's path elsewhere on the phone
    (--romfs-dir), which defaults names; romfs_push False: the player copies it there (--no-romfs)"""
    dst = f'{REMOTE}/games/{gid}'
    # each device's copy logs, and the tar archives and RomFS pieces named after them, in a folder of its own: two installs at once (one per device) shared
    # adb_push_spv.tar, and one's cleanup removed the archive the other was sending. Named by phone_id.safe: a Wi-Fi phone's serial, 192.168.1.20:37123, made a
    # folder Windows refuses, and every install over Wi-Fi from the desktop app stopped there. After the phone's ID, not its serial, which changes with its port
    logs = logs / f'push_{phone_id.safe(phone_key(serial))}'; logs.mkdir(parents=True, exist_ok=True)
    parts = ['the one-game install', 'font', 'code'] + ['shaders'] * bool(spv) + ['RomFS'] + ['DLC'] * aoc.is_dir() + ['launcher files', 'permissions']
    def part(name, step=None):   # the part in the step's name; the count is the part's files or bytes (counting), or the parts when it has none
        bi.PROG.set(step=f'{step or name} · part {parts.index(name) + 1} of {len(parts)}'); bi.PROG.set(done=parts.index(name), total=len(parts), unit='parts')
    part('the one-game install', 'looking for the one-game install'); migrate_legacy(serial, gid, game / 'control.nacp')
    adb(serial, 'shell', 'rm', '-rf', f'{dst}/code', f'{dst}/font', f'{dst}/{patched.name}'); adb(serial, 'shell', 'mkdir', '-p', dst)
    # an install cut short (the phone full, the cable out) can leave a folder's archive (push_tar) or a RomFS piece (push_resumable) behind: gigabytes that kept the
    # phone full, and that no install reuses. RomFS.img.partial stays, as the next copy goes on from it
    adb(serial, 'shell', f'rm -f {sh_quote(dst)}/.*.tar {sh_quote(dst + "/RomFS.img.piece")} {sh_quote(dst + "/aoc")}/*.piece', check=False)
    part('font'); push_dir(serial, font, dst, 'font', logs / 'adb_push_font.log')
    part('code'); push_dir(serial, patched, dst, 'code', logs / 'adb_push_code.log')
    if spv: part('shaders', 'shaders (only the changed files)'); push_dir(serial, spv, dst, 'spv', logs / 'adb_push_spv.log', sync=True)
    part('RomFS', 'RomFS (a large game takes a while; only a changed image is copied again)')
    rom = bi.rfs.source(game)   # the extraction's RomFS image (RomFS.img: the host reads it as it is, hle_fs.c), or a RomFS folder from an older extraction
    if romfs_at:   # --romfs-dir: the image in a folder of the player's choosing, an SD card's say
        if not bi.rfs.is_image(rom): raise BuildError(f'--romfs-dir takes the RomFS as one image ({game}/RomFS.img), and this extraction has a RomFS folder: extract the game again')
        folder = romfs_at.rsplit('/', 1)[0]
        if romfs_push:
            adb(serial, 'shell', f'mkdir -p {sh_quote(folder)} && rm -f {sh_quote(romfs_at + ".piece")}')
            push_image(serial, Path(str(rom)), romfs_at, logs / 'adb_push_romfs.log'); open_tree(serial, folder)
        else: say(f'--no-romfs: copy {rom} to {romfs_at} on the phone yourself; the game reads it from there')
        # a copy left in the game's folder by an install before is read no more: gigabytes freed
        adb(serial, 'shell', f'rm -rf {sh_quote(dst + "/RomFS")} {sh_quote(dst + "/RomFS.img")} {sh_quote(dst + "/RomFS.img.partial")}')
    elif bi.rfs.is_image(rom):   # not rom.is_file(): romfs.Path takes an image as its root folder
        # a RomFS.img folder left by an earlier push would make adb fail ("Is a directory"); a RomFS.img file stays, for --sync
        adb(serial, 'shell', f"rm -rf {sh_quote(dst + '/RomFS')}; if [ -d {sh_quote(dst + '/RomFS.img')} ]; then rm -rf {sh_quote(dst + '/RomFS.img')}; fi")
        push_image(serial, Path(str(rom)), f'{dst}/RomFS.img', logs / 'adb_push_romfs.log')
    else:
        adb(serial, 'shell', 'rm', '-f', f'{dst}/RomFS.img')
        push_dir(serial, rom, dst, 'RomFS', logs / 'adb_push_romfs.log', sync=True)
    if aoc.is_dir(): part('DLC'); push_dir(serial, aoc, dst, 'aoc', logs / 'adb_push_aoc.log', sync=True)
    part('launcher files')
    # the launcher (dm/android/java Launcher.java): the game's icon and control data (name, version, languages), its defaults (defaults_text) and
    # game.json; env.txt is the player's and stays as it is (a key it sets wins over the settings screen)
    for f in ('icon.jpg', 'control.nacp'):
        if (game / f).is_file(): adb(serial, 'push', game / f, f'{dst}/{f}')
    dflt = work / 'android_defaults.txt'; dflt.write_text(defaults, encoding='utf-8', newline='\n')   # \n on Windows too: the desktop app reads game.json by lines
    adb(serial, 'push', dflt, f'{dst}/defaults.txt')
    gj = work / 'android_game.json'; gj.write_text(json.dumps(meta, indent=1), encoding='utf-8', newline='\n')
    adb(serial, 'push', gj, f'{dst}/game.json')
    part('permissions'); open_tree(serial, dst)


# ---------------- --check and --info (JSON for the desktop app) ----------------
def tool_version(cmd):
    try: r = subprocess.run(cmd, capture_output=True, text=True, errors='replace', timeout=30)
    except (OSError, subprocess.TimeoutExpired): return None
    return (r.stdout + r.stderr).strip() if r.returncode == 0 else None


def check(sdk_arg):
    rows = []
    def row(name, ok, detail, fix=''):
        if os.name == 'nt' and fix and not name.startswith('Python'): fix = "Install what's missing (the desktop app's Settings), or build_android_from_nsp.py --setup"
        rows.append({'name': name, 'ok': bool(ok), 'detail': detail, 'fix': '' if ok else fix})
    for m, pip in (('lz4.block', 'lz4'), ('capstone', 'capstone'), ('fontTools', 'fonttools'), ('zbic', 'zbic'), ('PySide6', 'PySide6')):
        row(f'Python module {pip}', bi._importable(m), sys.executable, f'{sys.executable} -m pip install {pip}')
    z = bi._importable('compression.zstd') or bi._importable('backports.zstd')
    row('Python zstd', z, sys.executable, f'{sys.executable} -m pip install backports.zstd (or Python 3.14+)')
    env = dict(os.environ)
    try: bi.configure_dotnet(env); dn = tool_version(['dotnet', '--version'])
    except BuildError as e: dn = None; row('.NET SDK 10', False, str(e), 'install the .NET 10 SDK: https://dotnet.microsoft.com/download')
    else: row('.NET SDK 10', dn and dn.split('.')[0] == '10', dn or 'not found', 'install the .NET 10 SDK: https://dotnet.microsoft.com/download')
    try: sdk = sdk_dir(sdk_arg)
    except BuildError: sdk = None; row('Android SDK', False, 'not found', 'install Android Studio or the command-line tools, then set ANDROID_HOME (or give --sdk)')
    else: row('Android SDK', True, str(sdk))
    if sdk:
        for what, d in (('NDK', sdk / 'ndk'), ('CMake', sdk / 'cmake')):
            try: row(what, True, str(newest(d, what)))
            except BuildError as e: row(what, False, str(e), f'sdkmanager "{f"ndk;{NDK}" if what == "NDK" else f"cmake;{CMAKE}"}"')
        row(f'build-tools {BUILD_TOOLS}', (sdk / 'build-tools' / BUILD_TOOLS).is_dir(), str(sdk / 'build-tools' / BUILD_TOOLS), f'sdkmanager "build-tools;{BUILD_TOOLS}"')
        row(f'platform {PLATFORM}', (sdk / 'platforms' / PLATFORM / 'android.jar').is_file(), str(sdk / 'platforms' / PLATFORM), f'sdkmanager "platforms;{PLATFORM}"')
    a = tool_version(['adb', 'version'])
    row('adb', a, a.splitlines()[0] if a else 'not found', 'install the Android SDK platform-tools and put them on PATH')
    for t in ('keytool', 'javac'):
        v = shutil.which(t)
        if v and os.name == 'nt': v = str(Path(v).with_suffix(Path(v).suffix.lower()))   # which() spells the extension as PATHEXT does (javac.EXE), not as the file is named
        row(f'JDK {t}', v, v or 'not found', 'install a JDK (17 or newer) and put its bin folder on PATH')
    row('signing key', True, f'{KEYSTORE}' + ('' if KEYSTORE.is_file() else ' (made at the first build)'))
    hosts = library(); row('game hosts', True, f'{len(hosts)} in {data_dir() / "hosts"}' + (': ' + ', '.join(hosts) if hosts else ''))
    return rows


def setup(sdk_arg, accept):   # --setup: what check() finds missing, installed (host_setup.py), then checked again; the Python modules stay pip's (or uv run's)
    missing = {r['name'] for r in check(sdk_arg) if not r['ok']}
    try: sdk = sdk_dir(sdk_arg)
    except BuildError: sdk = Path(sdk_arg or os.path.join(os.environ.get('LOCALAPPDATA') or Path.home(), 'Android', 'Sdk')).expanduser()   # Android Studio's place
    try: done = host_setup.setup(missing, sdk, data_dir() / 'tools', sdk_packages(sdk), accept, bi.PROG, say)
    except host_setup.SetupError as e: raise BuildError(str(e)) from e
    host_setup.add_tool_paths(data_dir() / 'tools', sdk)
    still = [r for r in check(sdk_arg) if not r['ok']]
    if done: say(f'installed: {", ".join(done)}')
    if [r for r in still if not r['name'].startswith('Python')]: raise BuildError('still missing: ' + ', '.join(r['name'] for r in still))
    for r in still: say(f'{r["name"]} is missing: {r["fix"]}')


GRAPHICS_NAMES = {'nvn': 'NVN', 'vulkan': 'Vulkan', 'gl': 'OpenGL'}


def unsupported_graphics(gfx, name):   # why a game cannot be built for Android, from profile_guess.build_sources' (graphics API, reason); None for an NVN game
    # The Android host replays NVN on Vulkan (dm/android/gfx_vk.c) and has nothing for a game that calls Vulkan or OpenGL itself: the host's layers for those are
    # Metal (dm/gfx/vk/hle_vk.m, the GL layer). Built anyway, such a game stopped at launch on its first unimplemented vk* import (Hades, issue #112)
    api, why = gfx
    if api == 'nvn': return None
    return (f'{name} draws with {GRAPHICS_NAMES.get(api, api)}, not NVN ({why}). DroidPlay Core on Android runs NVN games only so far, '
            f'so this game cannot be built for Android yet')


def main_graphics(imports, gl_names):
    """(api, why) for a game whose main alone shows it draws with Vulkan or OpenGL, by profile_guess.shader_sources' rules, else None. None as well when main imports nn::ro (it
    loads NROs at run time, which can draw with NVN: Portal's Source engine) or nvnBootstrapLoader (NVN, or NVN beside an EGL it never calls: Rayman 30th Anniversary Edition):
    the survey reads the rest and the check after the extraction decides (unsupported_graphics)"""
    if 'nvnBootstrapLoader' in imports or any(s.startswith('_ZN2nn2ro') for s in imports): return None
    vk = sum(1 for s in imports if re.match(r'vk[A-Z]', s) and s != 'vkGetInstanceProcAddr')   # vkGetInstanceProcAddr alone: SDL's Vulkan loader, never used (Balatro)
    if vk: return 'vulkan', f'main imports {vk} Vulkan functions and not nvnBootstrapLoader'
    if 'eglCreateWindowSurface' in imports and gl_names: return 'gl', 'main imports EGL and names glShaderSource: it renders with OpenGL'   # EGL alone: SDL3's (Absolum)
    return None


def code_graphics(code):   # main_graphics of bi.code_probe's folder; None without a main
    main = Path(code) / 'ExeFS' / 'main'
    if not main.exists(): return None
    img = bi.pg.nso(main)[0]
    return main_graphics({s for s, defined, _ in bi.pg.nso_syms.dynsyms(img) if not defined}, img.count(b'glShaderSource'))


def undecodable_packs(packs, name, decoder):   # why a game's shaders cannot be split here, from profile_guess.build_sources' oodle_packs; None when the split can read them
    # Only the game's own Oodle reads these archives: the phone (oodle_decode), a Mac host (mac_decoder), or what an earlier build with the phone decoded. Without one, Pokémon
    # Legends: Arceus's split failed on all 678 GFPAKs after about 220 s, then the build stopped (issue #310). ue_legacy is left out, as Python reads its Zlib and LZ4 paks
    on, why = packs
    if not on or decoder: return None
    return (f'{name} keeps its shaders in files compressed with the game\'s own Oodle ({why.split(", Oodle-compressed")[0]}), which only the game\'s own decoder reads. '
            'An Android build runs it on the phone: connect one and build without --no-push or --game-apk (later builds and game apps take what it decoded), '
            'or build the game in DroidPlay for Mac first')


def games_table():   # {title ID: {engine, graphics, build, sdk}} from dm/games/README.md's Games table
    out = {}
    for line in (REPO / 'dm/games/README.md').read_text(encoding='utf-8').splitlines():
        c = [x.strip() for x in line.strip().strip('|').split('|')]
        if len(c) == 6 and re.fullmatch(r'`[0-9A-F]{16}`', c[1]):
            out[c[1].strip('`')] = {'build': c[2], 'sdk': c[3], 'engine': c[4], 'graphics': c[5]}
    return out


def provisional_profile(gamemod, nsp_dir, app):   # the provisional profile an experimental build wrote (<NSP folder>/<id>_build/profile/<id>), joined to game.py's search path; None before the survey
    pdir = nsp_dir / f'{app:016X}_build' / 'profile'
    if not (pdir / f'{app:016X}' / 'game.tsv').is_file(): return None
    os.environ['DM_GAMES_DIR'] = os.pathsep.join([str(pdir)] + [d for d in os.environ.get('DM_GAMES_DIR', '').split(os.pathsep) if d and d != str(pdir)])
    return gamemod.load(f'{app:016X}')


def title_id(text):   # --title-id / TITLE_ID → the title ID as a number, or None when empty; anything else than 16 hex digits is a BuildError
    t = (text or '').strip()
    if not t: return None
    if not re.fullmatch(r'0[0-9A-Fa-f]{15}', t): raise BuildError(f'--title-id / TITLE_ID: {t!r} is no title ID (16 hex digits, e.g. 0100E870163CA000)')
    return int(t, 16)


def scan(src, keys, env, nsz=True, want=None):
    """The game in src as build_ipa_from_nsp.py's main finds it: (folder, title ID, fam, kind). src is a folder of NSPs / XCIs (kind None), a dump folder ('folder')
    or a NUP container ('nup': a .nup file, or the one .nup in the folder); the folder is where <title ID>_build goes. A NUP and a dump are not encrypted: no keys.
    want: the game to pack when the folder holds several (--title-id / TITLE_ID), as build_ipa_from_nsp.py's: a cartridge with more than one game (Batman: Arkham
    Trilogy) stopped with the scan's "more than one game" error, which names an option this script didn't have"""
    nup = bi.nup_file(src)
    folder = src.parent if src.is_file() else src
    if nup:
        app, fam = bi.scan_nup(nup, want)
        say(f'a NUP: {nup.name}, {app:016X}, {len(fam["updates"])} update(s), {len(fam["dlc"])} DLC')
        if fam['base'] is None: say(bi.pack_nup.NO_BASE)
        return folder, app, fam, 'nup'
    if bi.dump_folder(folder):
        app, fam = bi.scan_dump(folder, want)
        say(f'a dump folder: {app:016X}, {len(fam["updates"])} update(s), {len(fam["dlc"])} DLC')
        return folder, app, fam, 'folder'
    if not keys: raise BuildError('give -k <prod.keys>: the game is in NSP / XCI files, which are encrypted')
    with bi.package_reader(REPO, keys, env) as packages:
        app, fam = bi.scan_nsps(folder, packages.contents, want)
        packages.check_keys(fam)
    if nsz: fam = bi.decompress_nsz(folder, app, fam, keys, env)
    return folder, app, fam, None


def game_files(fam, kind):   # what the game was read from, for the desktop app: the NSPs, the NUP, or a dump's version folders
    if kind == 'nup': p = Path(fam['nup'].path); return [{'name': p.name, 'bytes': p.stat().st_size}]
    if kind == 'folder': return [{'name': p.name, 'bytes': bi.tree_size(p)[1]} for p in [fam['base'], *[p for _, p in fam['updates']]] if p]
    return [{'name': p.name, 'bytes': p.stat().st_size} for p in [fam['base'], *[p for _, p in fam['updates']], *[p for _, _, p in fam['dlc']]] if p]


def info(src, keys=None, want=None):
    sys.path.insert(0, str(REPO / 'dm/games')); import game as gamemod
    sys.path.insert(0, str(REPO / 'dm/tools')); import nacp
    env = dict(os.environ); bi.configure_dotnet(env)
    nsp_dir, app, fam, kind = scan(src, keys, env, nsz=False, want=want)
    tid = f'{app:016X}'; g = gamemod.by_title(app); prov = False
    if g is None: g = provisional_profile(gamemod, nsp_dir, app); prov = g is not None   # an experimental build's: its art, versions and host show like a profiled game's
    out = {'title_id': tid, 'folder': str(nsp_dir), 'profile': bool(g), 'provisional': prov, 'name': g.get('name', '') if g else '', 'table': games_table().get(tid),
           'android_tested': tid in ANDROID_TESTED, 'dlc': len(fam['dlc']), 'container': kind or 'nsp', 'files': game_files(fam, kind)}
    if g:
        have = bi.nsp_versions(fam)
        out['versions'] = sorted(have, key=bi.vnum); out['newest'] = max(have, key=bi.vnum)
        work = nsp_dir / f'{g.id}_build'; ex = work / 'extract'
        out['extracted'] = {}
        for v in out['versions']:
            d = ex / v
            if (d / 'control.nacp').is_file():
                rom = bi.rfs.source(d)
                out['extracted'][v] = {'icon': str(d / 'icon.jpg') if (d / 'icon.jpg').is_file() else None, 'name': nacp.name(d / 'control.nacp'),
                                       'display': nacp.version(d / 'control.nacp'), 'languages': nacp.languages(d / 'control.nacp'),
                                       'romfs_bytes': Path(rom).stat().st_size if bi.rfs.is_image(rom) else None,
                                       'built': (work / f'patched_android_{v}' / 'main.fixups.tsv').is_file(),
                                       'captures': len(list((d / 'glslc_capture').glob('*.glslc')))}   # run-time shaders on this computer (pull_captures)
        h = library().get(g.id); out['host'] = h[1] if h else None
        if not prov and (want := bi.glslc_wanted(g)):   # shaders it compiles while it runs, which its tables name: built only with a Mac build's captures (run_time_missing)
            out['run_time_shaders'] = {'count': len(want), 'ready': not run_time_missing(g, want, g.words('shader_versions') or [out['newest']], ex, nsp_dir, env)}
    return out


# ---------------- main ----------------
def main(argv):
    ap = argparse.ArgumentParser(description='Pack a Switch game you own for Android (the DroidPlay app + the game files on the phone)')
    ap.add_argument('nsp_dir', nargs='?'); ap.add_argument('-k', '--keys', help='your prod.keys')
    ap.add_argument('-p', '--package', help='the version to pack (v0 = the base game; default: the newest in the folder)')
    ap.add_argument('-l', '--language', default='en-US', help='the game language (a code or a name; English by default)')
    ap.add_argument('--sdk', help='the Android SDK (default ANDROID_HOME)'); ap.add_argument('--serial', help='the adb device (default: the only one)')
    ap.add_argument('--no-push', action='store_true', help='build the APK only; do not install it or copy the game files to the phone')
    ap.add_argument('--push-only', action='store_true', help='install the app and copy a game built before to the phone, without building')
    ap.add_argument('--romfs-dir', help="put the RomFS image in this folder on the phone, as <folder>/<title ID>/RomFS.img, not in the game's folder; "
                                        f"sd = the app's own folder on the SD card (/storage/<card ID>/{ROMFS_SD}), which it can read with no storage permission")
    ap.add_argument('--no-romfs', action='store_true', help='with --romfs-dir: copy no RomFS; you copy RomFS.img there yourself, and the game reads it from there')
    ap.add_argument('--wait-device', type=float, default=0, help='wait up to this many seconds for a phone a bench or another build holds (default: stop at once)')
    ap.add_argument('--reconnect-wait', type=float, default=180.0, help='wait up to this many seconds for a phone adb lost on the way, over Wi-Fi or USB, then go on (default 180)')
    ap.add_argument('--phone-id', help="with --serial: the phone's ID (ro.serialno, or aid-<Android ID>), to find it by when its serial changes (a new Wi-Fi port)")
    ap.add_argument('--update-shaders', action='store_true', help='pull the shaders a game compiled at run time on the phone, translate the new ones and copy them (no app build)')
    ap.add_argument('--apk-only', action='store_true', help='pack the app with every game host built on this computer and install it')
    ap.add_argument('--migrate', action='store_true', help="move a one-game install's files/game to files/games/<--title> on the phone")
    ap.add_argument('--title', help='the title ID for --migrate, --import-save and --restore-backup')
    ap.add_argument('--title-id', default=os.environ.get('TITLE_ID', ''), help='the game to pack when the folder holds several (a cartridge with more than one game): '
                    'its title ID, 16 hex digits (default: the TITLE_ID environment variable)')
    ap.add_argument('--import-save', metavar='PATH', help="send a save folder or archive (an emulator's, a JKSV / EdiZon / Checkpoint backup, a PC port's) for --title to the phone, "
                    'where DroidPlay asks before swapping it in')
    ap.add_argument('--restore-backup', metavar='FOLDER', help="send a game's save (--title) or every game's from a backup Back up saves pulled to this computer, to put back on the phone")
    ap.add_argument('--check', action='store_true', help='print the requirements as JSON and stop')
    ap.add_argument('--info', action='store_true', help="print the NSP folder's game details as JSON and stop")
    ap.add_argument('--progress', choices=('auto', 'bar', 'lines', 'json', 'off'), default='auto', help='stage lines on the console; json = newline-delimited JSON events')
    ap.add_argument('--allow-damaged', action='store_true', help='pack files that fail the NCA hash check as read')
    ap.add_argument('--game-apk', action='store_true', help='the game in an app of its own, to install on any device (no phone needed; the DroidPlay app is left as it is)')
    ap.add_argument('--xapk', action='store_true', help='with --game-apk: one .xapk (the APK with the RomFS as its OBB file) for an XAPK installer')
    ap.add_argument('--romfs-beside', action='store_true', help="with --game-apk: the RomFS beside the APK as <name>.romfs whatever the game's size, "
                                                                 'so the APK stays small; the player puts it anywhere on the device and picks it once')
    ap.add_argument('--out', help='with --game-apk: the folder for the app (default: the NSP folder)')
    ap.add_argument('--setup', action='store_true', help='install what --check finds missing (Windows: the JDK, the .NET SDK and the Android SDK)')
    ap.add_argument('--accept-android-licenses', action='store_true', help="with --setup: you accept the Android SDK License (https://developer.android.com/studio/terms)")
    a = ap.parse_args(argv)
    # adb is run by name: --sdk's platform-tools go on PATH too. The import-time tool_paths() only knew the SDK found without the options, so on a Mac with no
    # ANDROID_HOME and no adb on PATH, --sdk built the app and then stopped at its install with "No such file or directory: 'adb'"
    if a.sdk: tool_paths(a.sdk)
    global WAIT_DEVICE; WAIT_DEVICE = a.wait_device
    global RECONNECT_WAIT; RECONNECT_WAIT = a.reconnect_wait
    if a.phone_id:
        if not a.serial or phone_id.safe(a.phone_id) != a.phone_id: raise BuildError("--phone-id goes with --serial, and is the phone's ID (letters, digits, . _ -)")
        _hint[a.serial] = a.phone_id
    if a.romfs_beside and (a.xapk or not a.game_apk):
        raise BuildError('--romfs-beside goes with --game-apk, without --xapk (an XAPK carries the RomFS as its OBB file)')
    if a.xapk: a.game_apk = True
    if a.game_apk:
        if a.push_only or a.update_shaders or a.apk_only or a.migrate: raise BuildError('--game-apk builds the game; it goes with none of --push-only, --update-shaders, --apk-only, --migrate')
        a.no_push = True   # a game app goes to no phone from here
    if a.no_romfs and not a.romfs_dir: raise BuildError('--no-romfs goes with --romfs-dir: the folder on the phone you copy RomFS.img into yourself')
    if a.romfs_dir and (a.no_push or a.apk_only or a.update_shaders or a.migrate):
        raise BuildError('--romfs-dir is for a game copied to a phone (a build that installs it, or --push-only); a game app keeps its RomFS beside it or inside')
    bi.PROG.mode = bi.progress_mode('off' if a.check or a.info else a.progress); bi.ALLOW_DAMAGED = a.allow_damaged
    if not (a.check or a.info) and os.environ.get('NXNATIVE_JOURNAL', '1') != '0': bi.PROG = Journaled(bi.PROG.mode, argv)
    if a.check or a.info:   # stdout carries the JSON alone: messages on the way (say) go to stderr
        if a.info and not a.nsp_dir: raise BuildError('--info needs the NSP folder')
        with contextlib.redirect_stdout(sys.stderr): out = check(a.sdk) if a.check else info(Path(a.nsp_dir).expanduser().resolve(), Path(a.keys).expanduser().resolve() if a.keys else None, title_id(a.title_id))
        print(json.dumps(out, indent=1)); return
    if a.setup: setup(a.sdk, a.accept_android_licenses); return
    if a.apk_only or a.migrate or a.import_save or a.restore_backup:
        logs = data_dir() / 'logs'; logs.mkdir(parents=True, exist_ok=True)
        serial = a.serial or os.environ.get('ANDROID_SERIAL') or pick_device()
        if a.import_save or a.restore_backup:
            if a.import_save and a.restore_backup: raise BuildError('--import-save and --restore-backup go one at a time')
            if a.title and not re.fullmatch(r'[0-9A-Fa-f]{16}', a.title) or a.import_save and not a.title:
                raise BuildError(f'{"--import-save" if a.import_save else "--restore-backup"} needs --title <the game\'s 16-digit title ID>' + ('' if a.import_save else ' (or none: every game)'))
            work = data_dir() / 'saves_sent'; work.mkdir(parents=True, exist_ok=True)
            bi.PROG.plan(1); bi.PROG.stage('send the save')
            with phone(serial, 'build_android_from_nsp.py: sending a save'):
                if a.import_save: import_save(serial, a.title, a.import_save, work, logs)
                else: restore_backup(serial, a.restore_backup, a.title, work, logs)
            shutil.rmtree(work, ignore_errors=True); bi.PROG.end(); return
        if a.migrate:
            if not a.title or not re.fullmatch(r'[0-9A-Fa-f]{16}', a.title): raise BuildError('--migrate needs --title <the game\'s 16-digit title ID>')
            bi.PROG.plan(1); bi.PROG.stage('move the one-game install')
            with phone(serial, f'build_android_from_nsp.py: moving {a.title.upper()} to the launcher'):
                if not migrate_legacy(serial, a.title.upper()): say(f'nothing to move: no game in {REMOTE}/game')
            bi.PROG.end(); return
        bi.PROG.plan(2); env = dict(os.environ)
        bi.PROG.stage('APK'); apk = make_apk(sdk_dir(a.sdk), logs, env); bi.PROG.end(str(apk))
        bi.PROG.stage('install')
        with phone(serial, 'build_android_from_nsp.py: installing the app'): install(serial, apk, logs)
        bi.PROG.end(serial)
        return
    if not a.nsp_dir: raise BuildError('give the NSP folder (or the .nup file) and -k <prod.keys> (or --check / --apk-only / --migrate / --import-save / --restore-backup)')
    src, keys = Path(a.nsp_dir).expanduser().resolve(), Path(a.keys).expanduser().resolve() if a.keys else None
    if not src.exists(): raise BuildError(f'not a folder: {src}')
    if keys and not keys.is_file(): raise BuildError(f'no such file: {keys}')
    # zbic: a code module built for system 22.0.0 on (Switch 2 games such as Ocarina of Time) has ZBIC segments, which stop the build at its first read without it (droidplay#16)
    missing = [m for m in ('lz4.block', 'capstone', 'zbic') if not bi._importable(m)]
    if missing: raise BuildError(f'the current Python ({sys.executable}) is missing modules: {", ".join(missing)} (pip install lz4 capstone zbic)')
    if not (bi._importable('compression.zstd') or bi._importable('backports.zstd')):   # the shader split decompresses zstd files (Python 3.14's compression.zstd, or its backport)
        raise BuildError(f'the current Python ({sys.executable}) has no zstd module: pip install backports.zstd (or use Python 3.14 or newer)')
    sys.path.insert(0, str(REPO / 'dm/games')); import game as gamemod
    import profile_guess; bi.pg = profile_guess
    sys.path.insert(0, str(REPO / 'dm/tools')); import nacp; bi.nacp = nacp
    sdk = sdk_dir(a.sdk)
    env = dict(os.environ); bi.configure_dotnet(env)
    nsp_dir, app, fam, kind = scan(src, keys, env, nsz=False, want=title_id(a.title_id))   # NSZ files are decompressed once the profile is known: a game this computer can't build stops first
    def unpack(fam): return bi.decompress_nsz(nsp_dir, app, fam, keys, env) if kind is None else fam   # nothing left to do for a fam decompressed already
    g = gamemod.by_title(app)
    if g is None:   # no game profile: surveyed and packed from a provisional profile outside the repository, as build_ipa_from_nsp.py does (the game may not boot)
        pdir = nsp_dir / f'{app:016X}_build' / 'profile'
        if (pdir / f'{app:016X}' / 'game.tsv').exists(): say(f'no game profile for {app:016X}: reusing the provisional profile from the last run (delete it to survey again)')
        else:   # the first experimental build only packs; once its provisional profile exists, it is built and installed like a profiled game's (to try it on a phone)
            if not a.no_push: raise BuildError(f'no game profile for {app:016X}: a game without one is first packed with --no-push (a survey, a provisional profile and an APK that may not boot)')
            fam = unpack(fam)   # the survey reads the packages
            # the code modules first: a game Android can't build stops now, not after the survey's RomFS manifest and the extraction (FM24 Touch: 9 minutes, droidplay-core#476)
            code = bi.code_probe(nsp_dir, app, fam, keys, REPO, env)
            gfx = code_graphics(code)
            if gfx: raise BuildError(unsupported_graphics(gfx, nacp.name(code / 'control.nacp') or f'{app:016X}'))
            facts = bi.survey(nsp_dir, app, fam, keys, REPO, sys.executable, env)
            facts['survey'] = str(nsp_dir / f'{app:016X}_survey' / 'survey.md')
            bi.write_profile(pdir / f'{app:016X}', app, facts)
        g = provisional_profile(gamemod, nsp_dir, app); env['DM_GAMES_DIR'] = os.environ['DM_GAMES_DIR']
        if g is None: raise BuildError(f'no provisional profile in {pdir}: the survey wrote none')
        say('[RED] provisional game profile: values guessed from the survey, the game may not boot; bring it up per docs/agents after packing')
    say(f'game profile: {g.id} ({g.get("name", "")})')
    builds = g.builds(); have = bi.nsp_versions(fam)
    pkg_ver = a.package or max(have, key=bi.vnum)
    if pkg_ver not in have: raise BuildError(f'version {pkg_ver} is not in the folder, which holds {", ".join(sorted(have, key=bi.vnum))}')
    # A version the profile does not register, or one whose shader tables' versions are not all in the folder, is packed from a profile derived from it (derive_profile below),
    # with its own shaders, as build_ipa_from_nsp.py's main does: Mario Kart World's NUP holds update 1.8.0 alone, and its profile registers the base game
    prov = not g.dir.resolve().is_relative_to((REPO / 'dm/games').resolve())   # its captures are pulled (pull_captures)
    derived = ''
    if not prov:
        need = g.words('shader_versions')
        registered = ', '.join(f'{v} {b["display"]}' for v, b in builds.items())
        if pkg_ver not in builds: derived = f'{pkg_ver} is not a version game profile {g.id} registers ({registered})'
        elif not all(v in have for v in need): derived = f'the folder has no {", ".join(v for v in need if v not in have)}, whose files game profile {g.id}\'s shader tables were made from'
        if derived: say(f'{derived}: packing {pkg_ver} from a profile derived from it')
    work = nsp_dir / f'{g.id}_build'; logs = work / 'logs'; ex = work / 'extract'
    want = None if prov else bi.glslc_wanted(g)   # the shaders it compiles while it runs that the registered tables name (run_time_missing)
    if want and not derived and not (a.push_only or a.update_shaders) and run_time_missing(g, want, g.words('shader_versions') or [pkg_ver], ex, nsp_dir, env):
        raise BuildError(run_time_stop(g, want, g.get('name', '') or g.id))   # before the NSZ decompression and the extraction (Three Houses: 47 s and more, then the split's table difference)
    fam = unpack(fam)
    global _work; _work = work
    ex.mkdir(parents=True, exist_ok=True); (work / 'tmp').mkdir(exist_ok=True)
    env.update(DM_GAME=g.id, DM_ROOT=str(work), WORK=str(work / 'tmp'), DM_TARGET='android', PYTHONUTF8='1')   # the tools' text files in UTF-8 on Windows too (its default is the ANSI code page)
    patched = work / f'patched_android_{pkg_ver}'
    serial = None if a.no_push else a.serial or os.environ.get('ANDROID_SERIAL')
    if not a.no_push and not serial:   # the phone now (its captures are pulled before the shader stages); none connected fails at the install, as before, unless only the phone is wanted
        try: serial = pick_device()
        except BuildError:
            if a.push_only or a.update_shaders: raise
    romfs_at = romfs_path(serial, a.romfs_dir, g.id) if a.romfs_dir and serial else None   # now, so that a phone with no card stops the build before its shaders
    capture_dir = ex / pkg_ver / 'glslc_capture'
    runtime = False   # a provisional profile's game that compiles shaders while it runs (run_time_note): the end of an install says what to do next
    if a.push_only or a.update_shaders:   # what an earlier build left: the extraction, the rewrite, the shaders, the font and the host in the library
        game, spv, font = ex / pkg_ver, work / 'spv', work / 'font'
        need = [p for p in (game / 'control.nacp', patched / 'main.fixups.tsv', font) if not p.exists()]
        if need or g.id not in library(): raise BuildError(f'{g.id} {pkg_ver} was not built on this computer yet ({", ".join(map(str, need)) or "no host in the library"}): build it first')
        if not spv.is_dir(): spv = None
        new = pull_captures(serial, g.id, capture_dir, logs) if prov else 0
        if a.update_shaders and not prov: say('a registered profile: its shader tables name the captures they were made from, nothing is pulled')
        bi.PROG.plan((0 if a.update_shaders else 3) + 2 * bool(new) + bool(a.update_shaders))
        if new:   # the split again with the new captures, and the new ones translated (the others are kept)
            aoc = work / 'aoc'; dlc_roms = sorted(p for p in aoc.iterdir() if p.is_dir() or p.suffix == '.img') if aoc.is_dir() else []
            if (game / 'oodle_cache/complete').is_file(): env['DM_OODLE_CACHE'] = str(game / 'oodle_cache')   # what the full build decoded on the phone (oodle_decode)
            spv = build_shaders(g.words('shader_versions') or [pkg_ver], {pkg_ver: game}, work, logs, env, g, dlc_roms)[0] or spv
        if a.update_shaders:
            if not new: say('no new run-time shaders on the phone'); bi.PROG.end(); return
            bi.PROG.stage('copy the shaders to the phone'); dst = f'{REMOTE}/games/{g.id}'
            adb(serial, 'shell', 'am', 'force-stop', PKG)   # the game reads its shaders when it starts
            if spv: push_dir(serial, spv, dst, 'spv', logs / 'adb_push_spv.log', sync=True); open_tree(serial, f'{dst}/spv')
            bi.PROG.end(f'{dst}/spv'); say('done: open the game again on the phone'); return
    else:
        # the shaders are split from every version the profile's shader tables were made from (shader_versions), as the iOS build does
        sv = [pkg_ver] if derived else g.words('shader_versions') or [pkg_ver]
        if not all(v in have for v in sv): say(f'[RED] the folder has no {", ".join(v for v in sv if v not in have)}, whose files the profile shader tables were made from: no shaders are translated'); sv = []
        picks = bi.pick_nsps(fam, have, sorted(set(sv) | {pkg_ver}, key=bi.vnum))
        bi.PROG.plan(4 + (not kind) + 2 * bool(sv) + len(set(sv) - {pkg_ver}) + bool(fam['dlc']) + (not a.no_push))   # an extraction per other shader version, and the DLC's
        nx = None
        if not kind: bi.PROG.stage('build nx_extract'); nx = build_nx_extract(work, logs, env); bi.PROG.end()   # a NUP or a dump folder is read without it
        def extract(v):   # one version into extract/<v>, as build_ipa_from_nsp.py's main does: a NUP's packages, a dump's folders, or the NSPs through nx_extract
            b, u = picks[v]
            if kind == 'nup': return bi.extract_nup(ex, v, u or b, b, app, fam['nup'])
            if kind == 'folder': return bi.extract_dump(ex, v, u or b, app)
            return bi.extract(nx, keys, ex, v, b, u, None, logs, g, app)
        game = extract(pkg_ver)
        src, name = bi.pg.build_sources(game / 'ExeFS', bi.rfs.source(game), g.words('modules') or ['main']), nacp.name(game / 'control.nacp') or g.get('name', '') or g.id
        mac_oodle = mac_decoder(nsp_dir, work, g, pkg_ver, game, env) if src['oodle_packs'][0] else None   # not only with sv: a build mismatch below sets it
        why = unsupported_graphics(src['graphics'], name) or (undecodable_packs(src['oodle_packs'], name, mac_oodle or serial or (game / 'oodle_cache/complete').is_file()) if sv else None)
        if why: raise BuildError(why)   # before the rewrite and the shaders, which take minutes and make an app that cannot start (or no app at all)
        bad = [] if derived or prov else bi.build_mismatch(game, pkg_ver, g)
        if bad:   # files of another build under a registered version's tag: packed like any other version
            derived = f'not the builds game profile {g.id} registers: {"; ".join(bad)}'; sv = [pkg_ver]
            say(f'{derived}: packing {pkg_ver} from a profile derived from it')
        reg = g   # the profile as registered (refine_after_rewrite checks a derived build's keys against it)
        if derived:   # where a provisional profile goes (<id>_build/profile), and game.py (the host's CMake, the shader tools) takes it first (DM_GAMES_FIRST)
            pdir = work / 'profile'
            disp = bi.derive_profile(g, pdir / g.id, pkg_ver, game, g.words('modules'), app)
            os.environ['DM_GAMES_DIR'] = env['DM_GAMES_DIR'] = os.pathsep.join([str(pdir)] + [d for d in os.environ.get('DM_GAMES_DIR', '').split(os.pathsep) if d and d != str(pdir)])
            os.environ['DM_GAMES_FIRST'] = env['DM_GAMES_FIRST'] = '1'
            g = gamemod.load(g.id)
            say(f'derived game profile for {pkg_ver} ({disp}): {g.dir}')
        if note := run_time_note(src['glslc_capture'][0], name, g.id, prov, want, derived, len(bi.glslc_done(game / 'glslc_capture')), a.game_apk):   # before the rewrite and the shaders
            say(note); runtime = bool(prov)
            if runtime: bi.PROG.event('run_time_shaders', title_id=g.id, name=name, game_app=a.game_apk)   # the desktop app's notice and its Update shaders, highlighted
        g = bi.rewrite(sys.executable, REPO, game / 'ExeFS', patched, env, logs, g, bool(prov))
        if prov or derived: bi.provisional_prep(sys.executable, REPO, env, logs, g); g = bi.refine_after_rewrite(g, game, patched, reg if derived else None)
        # the DLC in the folder, as for iOS: <id>_build/aoc/<index>.img, which push_game copies to the game's aoc/ and the host mounts and reports as owned (DM_AOC_DIR).
        # It was not extracted for Android: The Binding of Isaac's Repentance (DLC 010021C000B6B001) started as Afterbirth+
        aoc = bi.extract_aoc(nx, keys, REPO, work, app, fam, logs)
        if prov and serial: pull_captures(serial, g.id, capture_dir, logs)
        dlc_roms = sorted(p for p in aoc.iterdir() if p.is_dir() or p.suffix == '.img') if aoc else []
        sdirs = {v: (game if v == pkg_ver else extract(v)) for v in sv}
        if want and not derived and run_time_missing(g, want, sv, ex, nsp_dir, env, take=True): raise BuildError(run_time_stop(g, want, name))   # a version extracted again lost its own
        if sv and bi.pg.build_sources(game / 'ExeFS', bi.rfs.source(game), g.words('modules') or ['main'])['ue_decode'][0]:   # Unreal's Oodle-compressed shaders (ue_decode)
            with phone(serial, f'build_android_from_nsp.py: decoding {g.id}\'s Unreal shaders') if serial else contextlib.nullcontext():
                ue_decode(sdirs, pkg_ver, nsp_dir, patched, work, logs, env, g, serial, sdk, game)
        if sv and src['oodle_packs'][0] and not mac_oodle:   # the archives' Oodle streams, decoded on the phone before the split
            env['DM_OODLE_CACHE'] = str(oodle_decode([bi.rfs.source(sdirs[v]) for v in sv], [sdirs[v] / 'ExeFS/main' for v in sv], dlc_roms, game, pkg_ver, nsp_dir, patched,
                                                     work, logs, env, g, serial, sdk))
        def derive_shader_tables(reason):   # build_ipa_from_nsp.py main's: the registered profile with the split's shader tables, everything else as registered
            assert g is not None   # main stopped earlier without a profile; a nested function keeps no narrowing
            pdir = work / 'profile'
            bi.derive_profile(g, pdir / g.id, pkg_ver, game, g.words('modules'), app, shader_only=True)
            os.environ['DM_GAMES_DIR'] = env['DM_GAMES_DIR'] = os.pathsep.join([str(pdir)] + [d for d in os.environ.get('DM_GAMES_DIR', '').split(os.pathsep) if d and d != str(pdir)])
            os.environ['DM_GAMES_FIRST'] = env['DM_GAMES_FIRST'] = '1'
            dg = gamemod.load(g.id)
            say(f'{reason}: using a shader-only derived profile at {dg.dir}; registered module fixes are preserved')
            return dg
        spv, g = build_shaders(sv, sdirs, work, logs, env, g, dlc_roms, derive_shader_tables, derived, (lambda: mac_oodle) if mac_oodle else None)
        bi.PROG.stage('Android host'); so, loader, ndk = build_host(sdk, g.id, work, logs, env); st = build_selftest(ndk, work, logs, env)
        if not a.game_apk: library_add(g.id, so, loader, st, {'title_id': g.id, 'name': nacp.name(game / 'control.nacp') or g.get('name', ''), 'version': pkg_ver,
                                            'display': nacp.version(game / 'control.nacp'), 'nsp_dir': str(nsp_dir)})
        bi.PROG.end(str(so))
        font = button_font(work, logs, env)
    have_l = nacp.languages(game / 'control.nacp')
    lang = bi.game_language(bi.language_code(a.language) or 'en-US', a.language != 'en-US', have_l)
    defaults = defaults_text(g, lang)
    label = nacp.name(game / 'control.nacp') or g.get('name', '') or g.id
    if a.game_apk:
        rom = bi.rfs.source(game)
        meta = {'title_id': g.id, 'name': label, 'version': pkg_ver, 'display': nacp.version(game / 'control.nacp'),
                'romfs_bytes': Path(rom).stat().st_size if bi.rfs.is_image(rom) else None}
        bi.PROG.stage('game app')
        made, inside = make_game_app(sdk, g, game, patched, spv, font, work / 'aoc', defaults, so, meta, work, logs, env,
                                     Path(a.out).expanduser().resolve() if a.out else nsp_dir, a.xapk, a.romfs_beside)
        bi.PROG.end(str(made[0]))
        if a.xapk: say(f'done: {made[0]}. Install it with an XAPK installer on the device; it puts the game data in Android/obb. Or unzip it, install the APK and copy Android/obb/ to the device')
        elif inside: say(f'done: {made[0]}, the game with its data inside. Install it on any Android device (arm64, Vulkan)')
        else: say(f'done: {made[0]} and {made[1].name}. Install the APK, put {made[1].name} anywhere on the device or its SD card, and pick it when the app first opens')
        return
    bi.PROG.stage('APK'); apk = make_apk(sdk, logs, env); bi.PROG.end(str(apk))
    if a.no_push:
        say(f'done: {data_dir() / "nxnative.apk"}. On the phone: install it, then copy {patched} to {REMOTE}/games/{g.id}/code and {bi.rfs.source(game)} to {REMOTE}/games/{g.id}/'); return
    bi.PROG.stage('install and copy to the phone')
    serial = serial or pick_device()
    with phone(serial, f'build_android_from_nsp.py: installing {label} ({g.id})'):
        bi.PROG.set(step='installing the app (an update keeps its data)'); install(serial, apk, logs)   # words that hold for a first install too, with no adb call to tell them apart
        adb(serial, 'shell', 'am', 'force-stop', PKG)
        rom = bi.rfs.source(game)
        romfs_at = romfs_at or (romfs_path(serial, a.romfs_dir, g.id) if a.romfs_dir else None)
        push_game(serial, g.id, game, patched, spv, font, work / 'aoc', defaults_text(g, lang, romfs_at), work, logs,
                  {'title_id': g.id, 'name': label, 'version': pkg_ver, 'display': nacp.version(game / 'control.nacp'),
                   'romfs_bytes': Path(rom).stat().st_size if bi.rfs.is_image(rom) else None}, romfs_at=romfs_at, romfs_push=not a.no_romfs)
    bi.PROG.end(f'{REMOTE}/games/{g.id}')
    if romfs_at: say(f'the RomFS: {romfs_at}' + (f' (copy {bi.rfs.source(game)} there before playing)' if a.no_romfs else ''))
    say(f'done: open DroidPlay on the phone and pick "{label}". Log: {REMOTE}/log.txt (adb logcat -s DroidPlay)')
    if runtime: say('once it reaches its first scene, bring in the shaders it compiled while it ran with --update-shaders (DroidPlay for Windows: Manage › Update shaders from the phone)')


if __name__ == '__main__':
    code = 1
    try: main(sys.argv[1:]); code = 0
    except BuildError as e:
        bi.PROG.close(ok=False); bi.PROG.say(f'[RED] {e}'); sys.exit(1)
    except OSError as e:   # a full disk anywhere (Windows' ERROR_DISK_FULL is ENOSPC too): said plainly, not as a traceback
        if e.errno != errno.ENOSPC: raise
        bi.PROG.close(ok=False); bi.PROG.say(f'[RED] {disk_full(e.filename or _work)}'); sys.exit(1)
    except SystemExit as e: code = e.code if isinstance(e.code, int) else (0 if e.code is None else 1); raise
    except KeyboardInterrupt: code = 130; raise
    finally:
        bi.PROG.close(ok=False)
        if isinstance(bi.PROG, Journaled): bi.PROG.exit(code)
