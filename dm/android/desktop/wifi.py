"""DroidPlay for Windows' Wi-Fi setup (scripts/nxnative_android.py's PairDialog and the Phones page's Connect…): what adb pair, adb connect, adb devices and
adb mdns services say, read into what the dialog needs and into plain words. adb's own text ("Failed: Unable to start pairing client.") was shown raw in a toast.
Qt-free, so dm/tools/tests/test_android_desktop.py tests it against adb's real messages without PySide6."""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import phone_id   # noqa: E402  # pyright: ignore[reportMissingImports]  (dm/android/tools, put on the path above: mDNS names and services)

SAME_NETWORK = 'the phone and this computer are on the same Wi-Fi network (a guest network keeps devices apart)'
STEPS = ('On the phone: <b>Settings › Developer options › Wireless debugging</b>, turned on (Android 11 and later).',
         'Tap <b>Pair device with pairing code</b> and keep that screen open.',
         'Pick the phone below and type the six-digit code it shows.')


def pairing(out):   # adb mdns services → [(label, 'ip:port')]: the phones showing a pairing code now
    return [(f'{phone_id.serialno_of(name) or name} · {addr}', addr) for name, kind, addr in phone_id.services(out) if kind == 'pairing']


def last_line(out): return next((x.strip() for x in reversed((out or '').splitlines()) if x.strip()), '') or 'no answer'


def pair_result(ok, out):   # adb pair's exit and output → (paired, the phone's serial number from adb's guid or None, what to tell the player)
    t = out or ''; low = t.lower()
    m = re.search(r'successfully paired to \S+ \[guid=([^\]\s]+)\]', t, re.I)
    if m or (ok and 'successfully paired' in low): return True, phone_id.serialno_of(m[1]) if m else None, 'Paired'
    if 'wrong password' in low:
        return False, None, "The code didn't match. Type the six digits the phone shows now: it makes a new code each time that screen opens."
    if any(x in low for x in ('timed out', 'timeout')):
        return False, None, f"The phone didn't answer in time. Keep its pairing screen open, and check that {SAME_NETWORK}."
    if any(x in low for x in ('unable to start pairing client', 'failed to connect', 'connection refused', 'no route to host', 'unreachable', 'cannot connect')):
        return False, None, f"DroidPlay couldn't reach the phone. Keep its <b>Pair device with pairing code</b> screen open, and check that {SAME_NETWORK}."
    if 'unknown host' in low or 'cannot resolve' in low or 'bad address' in low:
        return False, None, 'That address is not one adb can use: type it as the phone shows it, e.g. 192.168.1.20:37123.'
    return False, None, f'Pairing failed: {last_line(t)}'


def connect_result(ok, out):   # adb connect's exit and output → (connected, what to tell the player). adb connect exits 0 when it fails, too
    t = out or ''; low = t.lower()
    if re.search(r'\b(already )?connected to ', low) and 'failed' not in low and 'cannot' not in low: return True, 'Connected'
    if 'failed to authenticate' in low or 'unauthorized' in low:
        return False, "The phone didn't accept this computer: pair it first (<b>Pair over Wi-Fi…</b>)."
    if 'cannot resolve' in low or 'unknown host' in low or 'bad address' in low:
        return False, 'That address is not one adb can use: type it as the phone shows it, e.g. 192.168.1.20:37123.'
    if any(x in low for x in ('failed to connect', 'connection refused', 'no route to host', 'unreachable', 'timed out', 'cannot connect')):
        return False, ("No phone answered there. Check the address and port on the phone's <b>Wireless debugging</b> screen (the port changes each time it restarts), "
                       f'and that {SAME_NETWORK}.')
    return False, f'Connecting failed: {last_line(t)}'


def listed(devices_out, serialno, ip):   # adb devices' output → the serial adb already lists for that phone, ready: adb connects a paired phone by itself (mDNS)
    for line in (devices_out or '').splitlines()[1:]:
        p = line.split()
        if len(p) > 1 and p[1] == 'device' and ((serialno and phone_id.serialno_of(p[0]) == serialno) or (ip and p[0].startswith(ip + ':'))): return p[0]
    return None


def connect_target(mdns_out, serialno, ip):   # after pairing: the phone's wireless debugging service ('ip:port'), by its serial number or its address, or None
    for name, kind, addr in phone_id.services(mdns_out):
        if kind == 'connect' and ((serialno and phone_id.serialno_of(name) == serialno) or (ip and addr.rsplit(':', 1)[0] == ip)): return addr
    return None


def discovered(mdns_out, devices_out, names=None):   # Discover: every phone offering a connection on the network → [(label, 'ip:port', connected already)]
    # names: serial number → the phone's name, for phones DroidPlay knows. adb tcpip's (_adb._tcp) are listed too, marked as not encrypted
    ready = [p[0] for p in (ln.split() for ln in (devices_out or '').splitlines()[1:]) if len(p) > 1 and p[1] == 'device']
    out = []
    for name, kind, addr in phone_id.services(mdns_out):
        if kind == 'pairing': continue
        sn = phone_id.serialno_of(name) or (name[4:] if kind == 'tcp' and name.startswith('adb-') else None)   # adb tcpip names it adb-<serial number>, no suffix
        on = addr in ready or bool(sn and any(s == sn or phone_id.serialno_of(s) == sn for s in ready))   # its USB serial is its serial number
        out.append((f'{(names or {}).get(sn) or sn or name} · {addr}' + ('' if kind == 'connect' else ' · adb tcpip, not encrypted'), addr, on))
    return out


def disconnect_plan(serial):   # Disconnect, by the serial's kind → ('disconnect', serial) for an address adb connected to; ('wireless off', serial) for a phone adb
    # connected by itself over the network (adb disconnect can't name it: it adds :5555 to the name, and adb would connect it again anyway); ('usb', serial)
    t = phone_id.transport(serial)
    return ('disconnect' if t == 'tcp' else 'wireless off' if t == 'mdns' else 'usb'), serial


RECONNECT_EVERY, RECONNECT_FOR = 20, 600   # seconds: how often a phone that dropped off Wi-Fi is looked for, and for how long


def reconnect_targets(want, mdns_out, last):   # addresses to adb connect for the phone with ID want: the ones adb mdns services gives for its serial number (a new
    # port after wireless debugging restarted), then the last one it had (the same port, after a sleep or a roam)
    addrs = [addr for sn, addr in phone_id.mdns_services(mdns_out) if sn == want]
    return addrs + [last] if last and phone_id.transport(last) == 'tcp' and last not in addrs else addrs


def stale(devs, want, seen, now):   # the offline Wi-Fi entries a phone left behind, once it is back as serial now → serials to adb disconnect. devs: refresh_devices'
    # list; seen: serial → ID, all known this session. adb keeps an offline ip:port entry after a drop, which the picker showed greyed beside the phone itself
    ip = now.rsplit(':', 1)[0] if phone_id.transport(now) == 'tcp' else None
    return [d['serial'] for d in devs if d['state'] == 'offline' and d['serial'] != now and phone_id.wifi(d['serial']) and
            (seen.get(d['serial']) == want or phone_id.serialno_of(d['serial']) == want or (ip and d['serial'].rsplit(':', 1)[0] == ip))]


TCPIP_PORT = 5555   # adb tcpip's: Use over Wi-Fi restarts the phone's adb on it
TCPIP_WARNING = ("DroidPlay restarts adb on the phone in network mode (adb tcpip) and connects to it at its Wi-Fi address, with no pairing: this works on "
                 "Android 10 too. <b>This connection is not encrypted</b>: someone on your network could see what goes over it (the phone still accepts only "
                 "computers it has allowed). It lasts until the phone restarts. On Android 11 and later, Pair over Wi-Fi gives an encrypted connection.")
IP_CMD = 'ip -f inet addr show'   # every interface's IPv4 address, in one shell call


def wifi_ip(out):   # `ip -f inet addr show` → the phone's Wi-Fi address (a wlan* interface's; else the first that isn't loopback or a phone network's), or None
    found = re.findall(r'inet (\d+\.\d+\.\d+\.\d+)/\d+.*?\b(\S+)\s*$', out or '', re.M)
    for ip, dev in found:
        if dev.startswith('wlan'): return ip
    return next((ip for ip, dev in found if not dev.startswith(('lo', 'rmnet', 'ccmni', 'dummy', 'tun', 'v4-'))), None)


def tcpip_result(ok, out):   # adb tcpip's exit and output → (restarted, what to tell the player)
    if 'restarting in tcp mode' in (out or '').lower(): return True, 'adb restarted on the phone in network mode'
    return False, f"adb on the phone didn't restart in network mode: {last_line(out)}"
