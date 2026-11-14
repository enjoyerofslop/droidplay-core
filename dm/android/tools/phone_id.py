#!/usr/bin/env python3
# Which phone an adb serial is, and what it is called in a file or folder name on this computer. A phone on USB has a plain serial (R5CY51ZLGKH), but one over
# Wi-Fi (wireless debugging) has its address, 192.168.1.20:37123, after `adb connect`, or the name adb found it by on the network,
# adb-R5CY51ZLGKH-AbCdEf._adb-tls-connect._tcp. The address changes whenever wireless debugging restarts or the phone reboots, and one phone on a cable and on
# Wi-Fi at once is listed twice, so anything kept per phone is kept under its stable ID: ro.serialno (what adb shows for it over USB), or, where a maker left that
# empty or a placeholder, the Android ID the shell reads (aid-<16 hex digits>, the same until a factory reset). Imported by scripts/build_android_from_nsp.py,
# scripts/bench_android.py, device_lock.py and the desktop app (scripts/nxnative_android.py); Qt-free, so its tests run without PySide6. And adb's list of devices
# as those read it (devices).
import re

ID_CMD = 'getprop ro.serialno; settings get secure android_id'   # one adb shell call; its two lines go to from_props
PLACEHOLDERS = {'', 'unknown', 'null', '0123456789abcdef', '0000000000000000', '1234567890abcdef'}   # serials cheap phones share, lower case: no use as an ID
MDNS = re.compile(r'^adb-(.+)-[^-.]+\._adb-tls-connect\._tcp\.?$')   # adbd's service name: adb-<ro.serialno>-<its own random suffix>
TRANSPORTS = ('usb', 'mdns', 'tcp')   # the order a phone listed more than once is picked in: a cable is faster than Wi-Fi, and an mDNS name survives a new port


def safe(serial):   # every character outside A-Z, a-z, 0-9 and ._- as _, and no dot at the end: Windows refuses ':' (and <>"/\|?*) in a name, and a trailing dot
    return re.sub(r'[^A-Za-z0-9._-]', '_', serial).rstrip('.') or '_'


def transport(serial):   # 'usb', 'tcp' (an address, after adb connect or adb tcpip) or 'mdns' (a name adb found on the network)
    if '._adb-tls-connect._tcp' in serial: return 'mdns'
    if re.fullmatch(r'(\[[0-9A-Fa-f:.%\w]+\]|[\w.-]+):\d+', serial): return 'tcp'
    return 'usb'


def wifi(serial): return transport(serial) != 'usb'


def mdns_serialno(name):   # the ro.serialno an mDNS service name carries, or None: a phone found on the network matched to one known, with no adb shell call
    m = MDNS.match(name)
    return m[1] if m and m[1].lower() not in PLACEHOLDERS else None


def serialno_of(name):   # the serial number in adbd's service name, with or without its type (adb-<serial number>-<suffix>[._adb-tls-….tcp]), or None
    m = re.match(r'^adb-(.+)-[^-.]+(?:\._adb(?:-tls-(?:connect|pairing))?\._tcp\.?)?$', name or '')
    return m[1] if m and m[1].lower() not in PLACEHOLDERS else None


# adb's state for a phone it found on USB but can't open (Linux, no udev rule letting this user use it): "no permissions", then why in brackets and the help page,
# all in the state column, so splitting the line at spaces read it as "no" and the phone was left out of DroidPlay's picker
NO_PERMISSIONS = 'no permissions'
_STATE = re.compile(r'(?P<state>no permissions)(?: \((?P<why>[^)]*)\))?(?:; see \[[^\]]*\])?|(?P<word>\S+)')


def devices(out):
    """`adb devices [-l]` → [{'serial', 'state', 'model', 'why'}]: state is adb's word (device, unauthorized, offline, recovery...) or NO_PERMISSIONS, why its reason
    for that ("user andrew is not in the plugdev group", '' otherwise), model devices -l's (spaces for its _)"""
    found = []
    for line in (out or '').splitlines():
        if not line.strip() or line.startswith(('List of devices', '* ', 'adb server', 'adb: ')): continue
        p = line.split(None, 1)
        if len(p) < 2: continue
        m = _STATE.match(p[1].strip())
        if not m: continue
        rest = p[1].strip()[m.end():].split()
        found.append({'serial': p[0], 'state': m['state'] or m['word'], 'model': next((x[6:] for x in rest if x.startswith('model:')), '').replace('_', ' '),
                      'why': m['why'] or ''})
    return found


KINDS = {'_adb-tls-pairing._tcp': 'pairing', '_adb-tls-connect._tcp': 'connect', '_adb._tcp': 'tcp'}   # mDNS service types: a pairing code's, wireless debugging's, adb tcpip's


def services(out):   # `adb mdns services` → [(name, kind, 'ip:port')]: name as adb-<serial number>-<suffix> (a QR code's pairing service is studio-…), kind from KINDS
    found = []
    for line in (out or '').splitlines():
        p = line.split()
        if len(p) >= 3 and p[1].rstrip('.') in KINDS and re.fullmatch(r'\S+:\d+', p[2]): found.append((p[0].split('._adb')[0], KINDS[p[1].rstrip('.')], p[2]))
    return found


def mdns_services(out):   # `adb mdns services` → [(a phone's serial number, 'ip:port')] for each phone offering a connection (not a pairing)
    return [(sn, addr) for name, kind, addr in services(out) if kind == 'connect' and (sn := mdns_serialno(name + '._adb-tls-connect._tcp'))]


def stable_id(serialno, android_id=''):   # → the phone's ID, or None when it gives neither a real serial number nor an Android ID
    s, a = (serialno or '').strip(), (android_id or '').strip()
    if s.lower() not in PLACEHOLDERS and safe(s) == s: return s
    if re.fullmatch(r'[0-9A-Fa-f]{8,64}', a) and a.strip('0'): return f'aid-{a.lower()}'
    return None


def from_props(out):   # ID_CMD's output → the phone's ID, or None
    lines = [x.strip() for x in (out or '').splitlines()] + ['', '']
    return stable_id(lines[0], lines[1])


def one_per_phone(devs):   # devices as dicts with 'serial' and 'id' (None: not known) → one per phone, by TRANSPORTS' order, in the order each phone first came
    best, order = {}, []
    for d in devs:
        k = d.get('id') or d['serial']
        if k not in best: order.append(k); best[k] = d
        elif TRANSPORTS.index(transport(d['serial'])) < TRANSPORTS.index(transport(best[k]['serial'])): best[k] = d
    return [best[k] for k in order]


def find(serials, keys, ids):   # the index in serials (a picker's entries; None for one that is no phone) of the first of keys, an ID or a serial, naming one; or -1
    for k in keys:
        for i, s in enumerate(serials):
            if k and s and k in (s, ids.get(s)): return i
    return -1


def current(serial, ready, seen):   # ready: [{'serial', 'id'}] adb lists as ready now; seen: serial → ID, all known this session → the serial that phone is on now
    # (serial itself while it is listed; else one with the ID it had, over the cable first), or None
    if any(d['serial'] == serial for d in ready): return serial
    same = [d for d in ready if seen.get(serial) and d.get('id') == seen[serial]]
    return one_per_phone(same)[0]['serial'] if same else None
