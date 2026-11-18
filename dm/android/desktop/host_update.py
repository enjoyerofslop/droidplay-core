# Update games in DroidPlay for Windows and Linux (scripts/nxnative_android.py GamesBar): whether the games on the picked phone run an older engine than this DroidPlay's,
# and what the bar says. A game's engine, its host, goes onto the phone inside the DroidPlay app, as the DroidPlay that last built it made it, so updating DroidPlay on
# the computer changed nothing on the phone until each game was built and installed again. build_android_from_nsp.py --apk-only now builds every host on this computer
# again from this DroidPlay first (refresh_hosts: no game files, a few seconds a game), then updates the app. A game app carries its own host and isn't reached.
# Qt-free, for dm/tools/tests/test_android_desktop.py.

PER_GAME, APP = 7, 30   # seconds: a game's host (about 6 on an 18-core Mac, which starts processes faster than Windows), then the app packed and installed


def bare(v): return (v or '').strip().removeprefix('v')   # a release's tag (v0.8.14) and its VERSION file (0.8.14) name one version


def behind(app_version, core, hosts, on_phone=()):
    """What Update games would do for the picked phone, or None when there's nothing to do. app_version: the DroidPlay app's version on it (dumpsys package's
    versionName; None without the app, which a first build installs); core: this DroidPlay's (build_android_from_nsp.py core_version); hosts: {title ID: meta.json} of the
    host library; on_phone: the title IDs of the games on it. → {'stale': the names of the hosts not built from this DroidPlay, 'app_old': the phone's app is another
    version, 'missing': the games on the phone with no host here, which the updated app has no engine for, 'seconds': about how long it takes}"""
    if not app_version or not core or not hosts: return None
    stale = [m.get('name') or gid for gid, m in hosts.items() if bare(m.get('core')) != bare(core)]
    app_old = bare(app_version) != bare(core)
    if not stale and not app_old: return None
    return {'stale': stale, 'app_old': app_old, 'missing': sorted(g for g in on_phone if g not in hosts), 'seconds': PER_GAME * len(stale) + APP}


def duration(sec): return f'about {round(sec / 60)} min' if sec >= 90 else f'about {max(5, round(sec / 5) * 5)} s'


def bar_text(offer, phone, core):
    n = len(offer['stale'])
    what = (f"each game's engine is the one DroidPlay had when the game was built. Update games builds {n} engine{'s' * (n != 1)} again from this DroidPlay "
            f"(no game files needed) and updates the app on the phone, {duration(offer['seconds'])}" if n else
            f"Update games updates the app on the phone with the engines built from this DroidPlay, {duration(offer['seconds'])}")
    head = (f'The games on {phone} don\'t have DroidPlay {core}\'s fixes yet: ' if offer['app_old'] else
            f'{n} of your games {"were" if n != 1 else "was"} built with an older DroidPlay, so on {phone} ')
    return head + what + '. Saves, settings and game files stay. Game apps carry their own engine: build them again for the fixes.'


def missing_text(names, phone):   # the confirmation when the phone holds games this computer has no host for (built on another computer, or removed from the library)
    many = len(names) > 1; are, were, them = ('are', "weren't", 'them') if many else ('is', "wasn't", 'it')
    return (f'{", ".join(names)} {are} on {phone} but {were} built on this computer, so the updated app has no engine for {them}: build and install {them} from here '
            'afterwards (the game files and saves on the phone stay). Update games anyway?')
