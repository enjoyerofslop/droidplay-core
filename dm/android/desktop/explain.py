"""Plain words for what went wrong in DroidPlay for Windows and Linux (scripts/nxnative_android.py): explain() turns the packing script's and adb's own messages into a
title, what to do, and the one button that does it, for the Builds page's failure banner, the Library's statuses and the toasts. The raw message stays on screen
beside it. And the report a player copies for the Discord's #help (Markdown under its 2,000-character limit) or saves in full, with the home folder written as ~
and anything that looks like key material dropped, as scripts/bug_report.py does. Qt-free, so dm/tools/tests/test_android_desktop.py tests the rules against the
real messages without PySide6."""
import platform
import re
import sys
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'scripts'))
import bug_report   # noqa: E402  # pyright: ignore[reportMissingImports]  (scripts/, put on the path above: its redaction)

DISCORD = 'https://discord.gg/3r92f8wmh7'
DISCORD_LIMIT = 2000   # characters in one message
ISSUE_441 = 'https://github.com/enjoyerofslop/droidplay-core/issues/441'   # games only a Mac build can capture run-time shaders for
WINDOWS = sys.platform == 'win32'
KEYSTORE = r'.android\nxnative-debug.jks' if WINDOWS else '.android/nxnative-debug.jks'   # the signing key, in the user's folder (build_android_from_nsp.py KEYSTORE)
TERMINAL = 'a Command Prompt' if WINDOWS else 'a terminal'


@dataclass
class Help:
    title: str
    body: str          # rich text: names in it are escaped
    action: str = ''   # what its button does: phone, retry, keys, tools, folder, backup, issue (ISSUE_441); '' for none
    label: str = ''    # the button's text
    bug: bool = False  # nothing the player did or can fix: the report for #help is what to do


def esc(s): return str(s).replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')


# The script's stage names as a player reads them (its own name stays beside, in the report and the Builds page): a stage named with a version or a file
# ("extract v393216") is matched by its first word
STAGES = {'build nx_extract': 'Preparing the game reader', 'extract': 'Reading the game files', 'unpack': 'Reading the game files', 'check': 'Reading the game files',
          'decompress NSZ': 'Unpacking compressed game files', 'rewrite': "Adapting the game's code for Android", 'shader split': "Finding the game's shaders",
          'shaders for Vulkan': "Translating shaders for your phone's GPU", 'Android host': "Building the game's engine", 'APK': 'Packing the DroidPlay app',
          'install and copy to the phone': 'Copying the game to the phone', 'install': 'Installing DroidPlay on the phone',
          'copy the shaders to the phone': 'Copying the shaders to the phone', 'game app': "Packing the game's own app", 'move the one-game install': 'Moving the old install',
          'JDK 21': 'Downloading Java', '.NET SDK 10': 'Downloading .NET', 'Android command-line tools': 'Downloading the Android tools',
          'Android SDK packages': 'Installing the Android SDK'}


def stage_name(tech):
    if not tech: return ''
    if tech.startswith('survey'): return 'Studying the game (experimental build)'
    return STAGES.get(tech) or STAGES.get(tech.split()[0]) or tech


NETWORK = ('urlopen error', 'timed out', 'getaddrinfo failed', 'name or service not known', 'temporary failure in name resolution', 'connection reset',
           'connectionreseterror', 'remotedisconnected', 'connection aborted', 'incompleteread', 'errno 11001', 'winerror 10054', 'winerror 10060', 'http error 5')
PC_FULL = ('not enough space on the disk', 'errno 28', 'winerror 112', 'no space left on device')


def script_error(stdout, stderr):
    """What a failed run of the script said, as explain() reads it: the lines before its error, then its [RED] lines, the error last. --info and --check send
    their notes to stderr and the error, printed after, to stdout; the app read stderr alone whenever it held anything, so a folder with a file the scan skipped
    showed that note, or nothing, where the reason belonged (Skyward Sword HD on Windows, 2026-11-13)"""
    lines = [x for x in f'{(stderr or "").strip()}\n{(stdout or "").strip()}'.splitlines() if x.strip()]
    reds = [x for x in lines if x.lstrip().startswith('[RED]')]
    if not reds: return '\n'.join(lines[-8:]) or 'no output'
    return '\n'.join([x for x in lines if x not in reds][-6:] + reds)[-2000:]


# The scan's words for a file it passed over (build_ipa_from_nsp.py scan_nsps): the file, and why
SKIPPED = re.compile(r'skipping (.+?): (neither an NSP.*|its content meta could not be read.*|no base game, update or DLC content meta.*)$', re.M)


def skipped_files(t):   # → rich text naming up to three files the scan passed over, and why, or ''
    s = SKIPPED.findall(t)
    if not s: return ''
    shown = '; '.join(f'<b>{esc(f)}</b> ({esc(why[:120])})' for f, why in s[:3])
    return f' It passed over {shown}' + (f' and {len(s) - 3} more' if len(s) > 3 else '') + '.'


def explain(text, phone='', game='', stage='', setup=False, folder='', wifi=False, read=False):
    """text: what failed said (its [RED] lines, the last line); phone, game: their names; stage: the script's name of the stage it stopped in; setup: it was
    --setup (Install tools); folder: the game's folder; wifi: the phone was connected over Wi-Fi (wireless debugging); read: the Library's read of a game
    folder (--info), not a build. → Help"""
    t = text or ''; low = t.lower(); wifi = wifi or 'wireless debugging is still on' in low   # the script's own words for a Wi-Fi phone that didn't come back
    ph, the_phone, g = esc(phone) if phone else 'Your phone', esc(phone) if phone else 'your phone', esc(game) if game else 'The game'
    adb_said = re.search(r'\badb\b', low) is not None   # an adb command's failure: "No space left on device" is then the phone's, not this computer's
    if 'INSTALL_FAILED_USER_RESTRICTED' in t:
        return Help('Your Xiaomi phone blocked the install', 'In Developer options, turn on <b>Install via USB</b> (it may ask you to sign in to your Xiaomi account), '
                    'then Install again.', 'retry', 'Install again')
    m = re.search(r"compiles (\d+) of its shaders while it runs, with its own shader compiler \(GLSLC\), and game profile \w+'s shader tables need them", t)
    if m:   # build_android_from_nsp.py run_time_stop: not a bug to report, and nothing the player can fix on this computer
        mac = 'Only a build for the Mac captures them' in t
        return Help('Build it for the Mac first' if mac else 'This game builds on a Mac only for now', f'{g} compiles {m[1]} of its shaders while it runs, and only a build on '
                    'a Mac can capture them so far. ' + ('Build it for the Mac with DroidPlay for Mac, then build it for Android again: this build takes them from there.' if mac else
                    'Its fix is followed in droidplay-core issue #441 on GitHub.'), '' if mac else 'issue', '' if mac else 'Follow the fix on GitHub')
    if 'INSTALL_FAILED_UPDATE_INCOMPATIBLE' in t or 'signed with another key' in t:
        return Help('DroidPlay on this phone came from another computer', f'To update it from this computer, copy the signing key (<b>{KEYSTORE}</b> in '
                    'your user folder) from that computer to this one. Uninstalling DroidPlay from the phone instead deletes its saves.', 'backup', 'Back up saves')
    m = re.search(r'the phone has ([\d.]+ [KMG]?B) free: .+? needs ([\d.]+ [KMG]?B).*?Free up ([\d.]+ [KMG]?B)', t, re.S)
    if m: return Help(f'Not enough space on {the_phone}', f'{g} needs {m[2]} and {the_phone} has {m[1]} free. Free up {m[3]} (videos, other games), then Install again.',
                      'retry', 'Install again')
    m = re.search(r'The phone has only ([\d.]+ [KMG]?B) free', t)
    if m or 'INSTALL_FAILED_INSUFFICIENT_STORAGE' in t or (adb_said and 'no space left on device' in low):
        return Help(f'Not enough space on {the_phone}', (f'{ph} has only {m[1]} free. ' if m else '') + 'Free up a few GB (videos, other games), then Install again: '
                    'a copy cut short carries on from where it stopped.', 'retry', 'Install again')
    if 'no permissions' in low and ('udev' in low or 'plugdev' in low or 'device' in low):
        # Linux: adb found the phone on USB but may not open it: no udev rule gives it to this user (phone_id.NO_PERMISSIONS). An AppImage can't install one
        return Help(f"Linux doesn't let DroidPlay use {the_phone} over USB yet", 'A program may use a phone over USB only where a udev rule allows it. '
                    "Install your distribution's Android rules (<b>android-sdk-platform-tools-common</b> on Ubuntu, Debian and Mint, <b>android-udev</b> on "
                    'Arch), then unplug the phone and plug it back in. <b>Set up my phone…</b> has the details, and a rule to add by hand.', 'phone', 'Set up my phone…')
    if ('unauthorized' in low or 'authorizing' in low) and 'http error' not in low:   # adb's state, not a web server's 401
        if wifi: return Help(f"{ph} doesn't know this computer over Wi-Fi", 'Pair it again: on the phone, Wireless debugging › Pair device with pairing code, then '
                             '<b>Pair over Wi-Fi…</b> on the Phones page.', 'phone', 'Set up my phone…')
        return Help(f"{ph} hasn't allowed this computer yet", 'Unlock it and tap <b>Allow</b> on "Allow USB debugging?" (tick "Always allow from this computer"). '
                    'No prompt? Unplug it and plug it back in.', 'phone', 'Set up my phone…')
    m = re.search(r'lost the phone (\d+) times', low)   # the script gave up on a connection that kept dropping (survives_drops)
    if m:
        return Help(f'The connection to {the_phone} keeps dropping', f'It dropped {m[1]} times in one install. ' +
                    ('Move the phone closer to the router (5 GHz is faster), keep it awake and charging, or use a USB cable for this install, then Install again: '
                     'the copy carries on from where it stopped.' if wifi or 'router' in low else
                     'Try another cable or USB port (one on the computer itself, not a hub), then Install again: the copy carries on from where it stopped.'), 'retry', 'Install again')
    if ('device offline' in low or '(offline)' in low or 'no devices/emulators found' in low or re.search(r"device '?\S*'? not found", low)
            or 'exit status 255' in low or 'error: closed' in low or 'did not come back within' in low):
        if wifi: return Help(f'{ph} dropped off Wi-Fi', 'Wake it, and check that it is on the same network as this computer and that Wireless debugging is still on '
                             '(its address changes when Wireless debugging restarts: DroidPlay finds it again). Then Install again: the copy carries on from where '
                             'it stopped.', 'retry', 'Install again')
        return Help(f'{ph} stopped answering', 'Unplug it and plug it back in, then Install again: the copy carries on from where it stopped. A different cable or USB '
                    'port helps if it keeps happening.', 'retry', 'Install again')
    if re.search(r'\b0 adb devices ready', low):
        return Help('No phone connected', 'Plug in your phone with a USB cable that carries data, with USB debugging on, or connect it over Wi-Fi on the Phones page '
                    '(Android 11 and later).', 'phone', 'Set up my phone…')
    m = re.search(r'less than ([\d.]+) GB free on the volume of (.+)', t)
    if m or any(x in low for x in PC_FULL):
        where = m[2].strip() if m else folder or ''
        d = re.match(r'[A-Za-z]:', where); drive = f' on drive {d[0].upper()}' if d else ''   # a Windows path's, read on any system
        more = f'about {m[1]} GB free' if m else 'more room'
        return Help('Not enough space on this computer', f'Building {g.lower() if not game else g} needs {more}{drive}' + (f' (in {esc(where)})' if where else '') +
                    ". Free some up, or move the game's folder to a drive with more room.", 'folder' if where else '', 'Open the folder' if where else '')
    m = re.search(r'prod\.keys has no \S+ \(master key \w+, which came with ((?:firmware|a firmware after) [\w.]+)\)', t)
    if m: return Help('Your keys are older than this game', f'It needs keys from a Switch on {esc(m[1])}{"" if "after" in m[1] else " or later"}. Update your Switch, '
                      'dump prod.keys again, and pick the new file.', 'keys', 'Choose prod.keys…')
    if 'no title key for rights id' in low or 'has no common ticket' in low:
        return Help("This game file can't be opened with your keys", 'The file has no ticket for it. Dump the game again from your Switch, with its ticket.')
    # a game that draws with OpenGL or Vulkan (build_android_from_nsp.py unsupported_graphics): nothing the player can change, and not a bug to report. It showed as "The build
    # stopped at Reading the game files" for Football Manager 2024 Touch (droidplay-core#476)
    m = re.search(r'(?:\[RED\] )?([^\n]*?) draws with (OpenGL|Vulkan), not NVN', t)
    if m:
        name = g if game else (esc(m[1]) or 'This game')
        return Help("This game isn't supported on Android yet", f"{name} draws with {m[2]}, and DroidPlay on Android runs only games that draw with NVN, the Switch's own "
                    'graphics API, so far: on the phone it would stop at its first graphics call, so it isn\'t built. Nothing on this computer or in the game files needs fixing.')
    # the game reader (nx_extract, built with .NET the first time a game is read) couldn't get its NuGet packages: NU1100 "Unable to resolve 'LibHac'" with
    # nuget.org missing or turned off in .NET's sources, NU1301 when the source can't be reached. It showed as "The build stopped" on a Windows PC (2026-11-13)
    if re.search(r'\bNU1(100|101|301)\b', t) or "unable to resolve 'libhac" in low or 'unable to load the service index' in low:
        return Help("DroidPlay couldn't download its game reader", "The first time it reads a game, DroidPlay builds a small game reader with .NET, which "
                    "downloads its parts from nuget.org, and .NET couldn't get them. Check this computer is online. If it is, nuget.org may be missing or turned off in "
                    f".NET's settings: in {TERMINAL}, <b>dotnet nuget list source</b> shows them; <b>dotnet nuget enable source nuget.org</b> turns it back "
                    "on, or <b>dotnet nuget add source https://api.nuget.org/v3/index.json -n nuget.org</b> adds it. Then open DroidPlay again.")
    # what reading the folder found (build_ipa_from_nsp.py scan_nsps), before anything is built: said as what to change in the folder, which the button opens
    show = ('folder', 'Open the folder') if folder else ('', '')
    m = re.search(r'more than one game in the folder: ([^;]+);', t)
    if m:
        return Help('More than one game in this folder', 'DroidPlay reads one game per folder: the game with its own updates and DLC. This folder holds the games '
                    f'{esc(m[1])}. Move each game into a folder of its own, then add those folders.', *show)
    if 'no base game nsp found' in low:
        return Help('The game itself is missing from this folder', "This folder has the game's update or DLC but not the game: put the base game's XCI (the "
                    'cartridge) or NSP in the same folder, beside the update.', *show)
    m = re.search(r'no NSP for update (\S+) \(the game profile needs these versions: ([^)]+)\)', t)
    if m:
        return Help('An update the build needs is missing', f"{g} is built from versions {esc(m[2])}, and this folder has no NSP for update {esc(m[1])}. Put "
                    "that update's NSP in the game's folder, beside the base game.", *show)
    if 'no game package with a readable content meta' in low:
        return Help('No game DroidPlay can open in this folder', "DroidPlay needs the game's own .nsp or .xci files: not a .zip or .rar, not a dump split into "
                    'parts (join them into one file first), and fully on this computer (OneDrive\'s online-only files can\'t be read). If the files are right, pick '
                    'your prod.keys again: keys from another Switch can\'t open them.' + skipped_files(t), *show)
    if 'give -k' in low or ('prod.keys' in low and 'needs' in low and 'keys' in low and 'master key' not in low):
        return Help('DroidPlay needs your keys', 'NSP and XCI games are encrypted: pick your prod.keys first.', 'keys', 'Choose prod.keys…')
    if 'needs dotnet' in low or 'install the .net 10 sdk' in low:
        return Help('Install the tools first', "DroidPlay reads and builds games with tools it hasn't installed yet.", 'tools', 'Install tools…')
    if 'does not match its' in low:
        return Help('The download came through damaged', "It didn't match what its publisher lists, so DroidPlay deleted it. Try again; if it keeps happening, something "
                    'on your network is changing downloads.', 'retry', 'Try again')
    if any(x in low for x in NETWORK):
        return Help('The download stopped', f'Check your internet connection, then {"Install tools" if setup else "try"} again.', 'retry', 'Try again')
    if read:   # the Library's read of a folder, not a build: no report to copy there, so the message itself (shown under it) is what to post
        return Help("DroidPlay couldn't read this game", 'This is probably a bug in DroidPlay, not something you did. Post what it said (below) in <b>#help</b> '
                    'on the Discord.', *show, bug=True)
    what, at = 'Installing the tools' if setup else 'The build', stage_name(stage)
    return Help(f'{what} stopped at "{at}"' if at else f'{what} stopped', 'This is probably a bug in DroidPlay, not something you did. Copy the report and post it in '
                '<b>#help</b> on the Discord.', bug=True)


# Games that compile some of their shaders while they run, with their own GLSLC (build_android_from_nsp.py run_time_missing, run_time_note): the Library's words before a build,
# and the Builds page's after one says so (its run_time_shaders event)
RUN_TIME_META = 'compiles some shaders while it runs: after its first scene, Manage › Update shaders from the phone'


def mac_only(count, mac=False):   # the status of a game whose profile's tables need shaders it compiles while it runs, with no Mac build's captures on this computer (#441)
    return f'{"Build it for the Mac first" if mac else "Builds on a Mac only for now"}: it compiles {count} shaders while it runs'


def run_time_note(game='', game_app=False):   # → (title, body) of the Builds page's notice for a provisional profile's game that compiles shaders while it runs
    g = esc(game) if game else 'The game'
    if game_app: return ('Some shaders come later', f'{g} compiles some of its shaders while it runs, so this game app may draw some things black, and it can\'t take them in '
                         'later. To pack them, install the game with DroidPlay first, play it to its first scene, use Manage › Update shaders from the phone, then build the '
                         'game app again.')
    return ('Some shaders come after the first play', f'{g} compiles some of its shaders while it runs, so the first install may draw some things black. Play it to its '
            'first scene, then use Manage › Update shaders from the phone to bring them in.')


# ---- the report ----
def system_name(os_release=None):   # os_release: /etc/os-release's fields, for the tests (any system)
    if sys.platform == 'win32' and os_release is None:
        b = sys.getwindowsversion().build   # pyright: ignore[reportAttributeAccessIssue]  (Windows only)
        return f'Windows {11 if b >= 22000 else 10} (build {b})'
    if sys.platform.startswith('linux') or os_release is not None:   # "Linux 7.2.9" says little: the distribution decides fonts, udev rules, FUSE and libraries
        try: rel = os_release if os_release is not None else platform.freedesktop_os_release()
        except OSError: rel = {}
        name, v = rel.get('PRETTY_NAME') or rel.get('NAME') or '', rel.get('VERSION_ID') or ''
        if v and v not in name: name = f'{name} {v}'.strip()   # Bazzite's PRETTY_NAME is "Bazzite" alone; Ubuntu's and Fedora's carry the version
        if name: return f'{name} (Linux {platform.release()})'
    return f'{platform.system()} {platform.release()}'


def redact(text, home):   # bug_report's rules; Windows spells the home folder with \ and, in some tools' messages, with /. And the phone's serial, which a post
    # in a public channel has no need of: the script's --serial and adb's -s name it
    if home: text = text.replace(home.replace('\\', '/'), home)
    text = re.sub(r'(--serial[ =]|--phone-id[ =]|\badb(?:\.exe)? -s )\S+', r'\1<serial>', text)
    text = re.sub(r'\badb-[A-Za-z0-9]+-[^\s.]+\._adb-tls-(?:connect|pairing)\._tcp\.?', '<serial>', text)   # a Wi-Fi phone's mDNS name carries its serial number
    return bug_report.redact(text, home)[0]


def header(info):   # info: version, title, stage, k, n, game, game_id, game_version, phone, android, gpu
    st = info.get('stage') or ''
    lines = [f'**DroidPlay {info.get("version") or "?"}** on {system_name()}',
             f'**{info.get("title") or "A build"}** stopped' + (f' at {stage_name(st)} (`{st}`' + (f', stage {info["k"]} of {info["n"]}' if info.get('k') else '') + ')' if st else '')]
    if info.get('game_id'): lines.append(f'Game: {info.get("game") or "?"} `{info["game_id"]}`' + (f' v{info["game_version"]}' if info.get('game_version') else ''))
    if info.get('phone'):
        lines.append('Phone: ' + ' · '.join(x for x in (info['phone'], f'Android {info["android"]}' if info.get('android') else '', info.get('gpu') or '') if x))
    return lines


def markdown_report(info, reds, log_lines, home, limit=DISCORD_LIMIT - 10):
    """The report to paste in #help: who, what, where it stopped, its [RED] lines and the last lines of its log, cut to fit one Discord message"""
    reds = list(dict.fromkeys(r.strip() for r in reds if r.strip()))[-6:]; tail = [x for x in log_lines if x.strip()][-15:]
    def make(reds, tail):
        out = header(info)
        if reds: out += ['**Error**', '```text', *reds, '```']
        if tail: out += ['**Last lines**', '```text', *tail, '```']
        return redact('\n'.join(out), home)
    text = make(reds, tail)
    while len(text) > limit and tail: tail = tail[1:]; text = make(reds, tail)
    while len(text) > limit and len(reds) > 1: reds = reds[1:]; text = make(reds, tail)
    if len(text) > limit: text = make([r[-300:] for r in reds], tail)
    return text[:limit]


def full_report(info, log_text, journal, home):   # Save full report: the same header, where the journal is, and the whole log
    head = [x.replace('**', '').replace('`', '') for x in header(info)]
    return redact('\n'.join(head + ['', f'Journal: {journal}' if journal else 'Journal: none', '', '---- log ----', log_text]), home)
