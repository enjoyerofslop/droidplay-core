#!/usr/bin/env python3
# One user of a phone at a time: scripts/bench_android.py and scripts/build_android_from_nsp.py hold a lock on each device they install on or run games on, so a
# second one is refused, with who holds it and since when. Before this, two benches on one device force-stopped each other's game and wrote over each other's
# env.txt, and an install force-stopped a bench's run: both runs void, with nothing in either log to say why. (On 2026-11-08 that happened three times, between
# sessions sharing three test devices.)
# The lock is the operating system's, on a file per device in <data>/locks (<data>: build_android_from_nsp.py's data folder, ~/.local/share/nxnative/android, or
# %LOCALAPPDATA%\nxnative\android on Windows): flock, or msvcrt.locking on Windows. A process that ends or crashes gives it up, so no lock outlives its holder. The
# file also says who holds it. NXNATIVE_DEVICE_HOLDER names the holder (a person, a session); otherwise it is the script, its process ID and the computer.
# A script that holds a device runs its children with NXNATIVE_DEVICE_LOCKED listing it, so bench_android.py --install's build_android_from_nsp.py goes on.
# Locks are per computer: two computers sharing one phone over adb (one adb server) don't see each other's.
#   python3 dm/android/tools/device_lock.py           which devices are held, by whom, doing what, since when
import contextlib, datetime, json, os, socket, sys, time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent)); import phone_id   # noqa: E402

LOCKED_ENV = 'NXNATIVE_DEVICE_LOCKED'   # the devices this process's parent holds, comma-separated
_held = {}                              # serial -> the open lock file


class DeviceBusy(Exception): pass


def lock_dir():
    if os.environ.get('NXNATIVE_LOCK_DIR'): return Path(os.environ['NXNATIVE_LOCK_DIR'])   # the tests'
    import data_home   # the packing script's data folder (sys.path has this folder: phone_id came from it)
    return data_home.data_dir() / 'locks'


def _path(serial):   # a Wi-Fi device's serial is host:port, or adb's mDNS name
    return lock_dir() / (phone_id.safe(serial) + '.lock')


# Windows locks bytes, and a locked byte can't be read: lock one byte far past what the file holds, so the holder's description stays readable
_FAR = 1 << 30


def _try_lock(f):
    try:
        if os.name == 'nt':
            import msvcrt
            f.seek(_FAR); msvcrt.locking(f.fileno(), msvcrt.LK_NBLCK, 1)   # pyright: ignore[reportAttributeAccessIssue]   (Windows only)
        else:
            import fcntl
            fcntl.flock(f.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        return True
    except OSError: return False


def _holder_of(serial):
    try: return json.loads(_path(serial).read_text(encoding='utf-8') or '{}')
    except (OSError, ValueError): return {}


def describe(h):   # one line: who holds it, doing what, since when
    if not h: return 'another program (it says nothing of itself)'
    who = h.get('holder') or f"{h.get('script', 'a script')} (pid {h.get('pid', '?')} on {h.get('host', '?')})"
    since = (h.get('since') or '')[11:16]
    return f"{who}: {h.get('what') or 'no description'}" + (f', since {since}' if since else '')


def inherited():
    return {s for s in os.environ.get(LOCKED_ENV, '').split(',') if s}


def acquire(serial, what, wait=0.0, say=None):
    """Hold serial's lock for this process (until release or exit). Raises DeviceBusy when another process holds it after wait seconds."""
    if serial in _held or serial in inherited(): return
    p = _path(serial); p.parent.mkdir(parents=True, exist_ok=True)
    f = open(p, 'a+', encoding='utf-8')
    end, told = time.monotonic() + wait, False
    while not _try_lock(f):
        if time.monotonic() >= end:
            f.close(); raise DeviceBusy(f'the device {serial} is in use by {describe(_holder_of(serial))}. Wait for that to finish, or stop it, then try again')
        if say and not told: say(f'waiting for the device {serial}, in use by {describe(_holder_of(serial))}'); told = True
        time.sleep(2)
    f.seek(0); f.truncate()
    f.write(json.dumps({'holder': os.environ.get('NXNATIVE_DEVICE_HOLDER', ''), 'script': Path(sys.argv[0]).name if sys.argv and sys.argv[0] else 'python',
                        'pid': os.getpid(), 'host': socket.gethostname(), 'what': what,
                        'since': datetime.datetime.now().isoformat(timespec='seconds')}))
    f.flush(); _held[serial] = f
    os.environ[LOCKED_ENV] = ','.join(sorted(inherited() | set(_held)))   # for this process's children


def release(serial):
    f = _held.pop(serial, None)
    if not f: return
    with contextlib.suppress(OSError): f.seek(0); f.truncate(); f.flush()   # nothing to say once free
    f.close()   # closing gives the lock up
    os.environ[LOCKED_ENV] = ','.join(sorted((inherited() - {serial}) | set(_held)))


@contextlib.contextmanager
def held(serial, what, wait=0.0, say=None):   # serial's lock for the block; a lock this process already had stays held after it
    had = serial in _held or serial in inherited()
    acquire(serial, what, wait, say)
    try: yield
    finally:
        if not had: release(serial)


def status():   # [(serial as the file names it, the holder's description or None when free)]
    out = []
    for p in sorted(lock_dir().glob('*.lock')) if lock_dir().is_dir() else []:
        serial = p.stem
        if serial in _held: out.append((serial, describe(_holder_of(serial)))); continue
        with open(p, 'a+', encoding='utf-8') as f:
            free = _try_lock(f)   # closing it again gives a lock taken here back
        out.append((serial, None if free else describe(_holder_of(serial))))
    return out


if __name__ == '__main__':
    rows = status()
    if not rows: print(f'no device has been locked on this computer ({lock_dir()})')
    for serial, h in rows: print(f'{serial}: {h or "free"}')
