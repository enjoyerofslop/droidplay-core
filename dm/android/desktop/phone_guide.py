"""How to connect a phone, by brand, for DroidPlay's PhoneGuide dialog (scripts/nxnative_android.py): the steps that turn on USB debugging, each maker's
catch (Samsung's Auto Blocker greys the switch out, Xiaomi needs Install via USB, a Pixel may need Google's driver on Windows), and what to try when the phone
doesn't show up, on Windows (drivers) or Linux (udev rules, which an AppImage can't install: the player does, once). And a phone's battery and free space from
one adb shell call, with the warning a nearly flat phone needs: a computer's USB port charges slowly (a Galaxy S25 Edge sat at 3-4% on the test PC) and an
install runs for minutes. Qt-free, so dm/tools/tests/test_android_desktop.py tests it."""
import re
import sys

INTRO = ("DroidPlay installs games over USB with Android's developer tools. You turn them on once, on the phone. "
         'Names vary a little between Android versions.')
PLUG = 'Plug the phone into this computer, unlock it, tick "Always allow from this computer" and tap <b>Allow</b>.'
BUILD_NUMBER = 'tap <b>Build number</b> 7 times (enter your PIN if it asks): "You are now a developer".'
GOOGLE_DRIVER = 'https://developer.android.com/studio/run/win-usb'
WINDOWS_NOTES = ('samsung', 'pixel')   # the brands whose note is about Windows' drivers: shown on Windows alone

# (key, name, steps, a note under them)
BRANDS = (
    ('samsung', 'Samsung Galaxy', [
        f'Settings › About phone › Software information: {BUILD_NUMBER}',
        'Settings › Security and privacy › <b>Auto Blocker</b>: turn it off. It blocks USB debugging and greys out the switch below. You can turn it back on '
        'when you\'re done installing games.',
        'Settings › <b>Developer options</b> (at the bottom of Settings): turn on <b>USB debugging</b>.',
        PLUG], 'No driver needed on Windows 10 or 11 (checked on a Galaxy S25 Edge).'),
    ('pixel', 'Google Pixel', [
        f'Settings › About phone: {BUILD_NUMBER}',
        'Settings › System › Developer options: turn on <b>USB debugging</b>.',
        PLUG], f'If DroidPlay still doesn\'t see it, Windows may need <a href="{GOOGLE_DRIVER}">Google\'s USB driver</a>.'),
    ('xiaomi', 'Xiaomi, Redmi, POCO', [
        'Settings › About phone: tap <b>OS version</b> (HyperOS) or <b>MIUI version</b> 7 times.',
        'Settings › Additional settings › Developer options: turn on <b>USB debugging</b>, then <b>Install via USB</b> (Xiaomi asks you to sign in to a Xiaomi '
        'account, and may need a SIM card in the phone), then <b>USB debugging (Security settings)</b>.',
        PLUG], 'When DroidPlay installs, the phone may ask whether to allow it: tap <b>Install</b>.'),
    ('oneplus', 'OnePlus, OPPO, realme', [
        f'Settings › About device › Version: {BUILD_NUMBER}',
        'Settings › System › Developer options (Additional settings on some): turn on <b>USB debugging</b>.',
        PLUG], ''),
    ('handheld', 'AYN, Retroid, AYANEO and other handhelds', [
        f'Settings › About (device or tablet): {BUILD_NUMBER}',
        'Settings › System › Developer options: turn on <b>USB debugging</b>.',
        PLUG], ''),
    ('other', 'Other Android', [
        f'Settings › About phone: {BUILD_NUMBER}',
        'Settings › Developer options (often under System): turn on <b>USB debugging</b>.',
        PLUG], ''),
)

TROUBLE = (
    ("It charges but DroidPlay doesn't see it", 'Many cables that come with controllers and power banks only charge. Try the phone\'s own cable, in a port on the '
     'computer rather than a hub. Some phones offer USB debugging only in <b>File transfer</b> mode: pick it in the phone\'s USB notification.'),
    ('"Allow USB debugging?" never appears', 'Unlock the phone and pull down the notifications. Or, in Developer options, tap <b>Revoke USB debugging '
     'authorizations</b>, then unplug it and plug it back in.'),
    ('USB debugging is greyed out (Samsung)', 'Turn off <b>Auto Blocker</b> (step 2).'),
    ('Device Manager shows "ADB Interface" with a yellow mark', f'Windows needs the maker\'s USB driver, or <a href="{GOOGLE_DRIVER}">Google\'s</a> for a Pixel.'),
    ('No cable?', 'Android 11 and later can connect over Wi-Fi: Developer options › <b>Wireless debugging</b>, then <b>Pair over Wi-Fi…</b> on the Phones page.'),
)
WINDOWS_TROUBLE = ('Device Manager shows "ADB Interface" with a yellow mark',)

# Linux: a program may open a phone's USB debugging interface only where a udev rule gives it to the user. systemd's own 70-uaccess.rules does for the person at
# the screen (TAG+="uaccess" on ADB's interface, ff4201) on Fedora, Arch, Bazzite and other distributions with a recent systemd; elsewhere adb lists the
# phone as "no permissions". An AppImage can't install a rule (nor run sudo): the guide says what to install, or the one line to add, as plainly as it can
UDEV_RULE = 'SUBSYSTEM=="usb", ENV{ID_USB_INTERFACES}=="*:ff4201:*", TAG+="uaccess"'
LINUX_NOTE = 'On Linux, if DroidPlay says the phone needs USB access ("no permissions"), see <b>If it doesn\'t work</b> below.'
LINUX_TROUBLE = (
    ('DroidPlay says "USB access needed" or adb says "no permissions"', 'Linux lets a program use a phone over USB only where a udev rule allows it. Fedora, '
     'Arch, Bazzite and other distributions with a recent systemd allow the person at the screen with nothing to install. Elsewhere, install your '
     "distribution's Android rules: <b>android-sdk-platform-tools-common</b> (Ubuntu, Debian, Mint) or <b>android-udev</b> (Arch, Manjaro), then unplug the "
     'phone and plug it back in.'),
    ('No package, or it still says "no permissions"', f'Add the rule yourself: put the line <code>{UDEV_RULE.replace(chr(34), "&quot;")}</code> in '
     '<b>/etc/udev/rules.d/70-android-adb.rules</b>, run <code>sudo udevadm control --reload</code> and <code>sudo udevadm trigger</code>, then plug the phone in '
     'again. If adb still says it, run <code>adb kill-server</code> (DroidPlay starts it again).'),
    ('An older guide says to join the plugdev group', 'Rules with <code>GROUP="plugdev"</code> work only once you\'re in that group (<code>sudo usermod -aG '
     'plugdev $USER</code>, then log out and back in), and systemd 258 and later ignore them when plugdev isn\'t a system group. The rule above, with '
     '<code>TAG+="uaccess"</code>, needs no group.'),
    ('Pair over Wi-Fi doesn\'t list the phone', "A firewall may block the phone's announcement (mDNS, UDP port 5353: Ubuntu's ufw does when it's on). "
     'Type the IP address and port shown with the pairing code instead.'),
)


def brand_note(key, system=sys.platform):   # the note under a brand's steps: Windows' driver advice on Windows alone, Linux's pointer to the udev help there
    note = next((b[3] for b in BRANDS if b[0] == key), '')
    if system == 'win32': return note
    if system.startswith('linux'): return LINUX_NOTE if key in WINDOWS_NOTES or not note else f'{note} {LINUX_NOTE}'
    return '' if key in WINDOWS_NOTES else note


def trouble(system=sys.platform):   # If it doesn't work: (problem, what to do), the Windows driver line on Windows, the udev lines on Linux
    rows = [t for t in TROUBLE if system == 'win32' or t[0] not in WINDOWS_TROUBLE]
    return rows + list(LINUX_TROUBLE) if system.startswith('linux') else rows

# what adb's getprop ro.product.manufacturer says (a phone that hasn't allowed this computer yet gives only devices -l's model): a maker's name, or a handheld's
# board maker (a Retroid Pocket 5 says Moorechip)
MAKERS = (('samsung', ('samsung',)), ('pixel', ('google', 'pixel')), ('xiaomi', ('xiaomi', 'redmi', 'poco')), ('oneplus', ('oneplus', 'oppo', 'realme')),
          ('handheld', ('ayn', 'retroid', 'moorechip', 'ayaneo', 'anbernic')))


def brand_for(manufacturer='', model=''):
    words = f'{manufacturer} {model}'.lower()
    if re.match(r'\s*sm[-_ ]', model.lower()): return 'samsung'   # a Galaxy's model code (SM-S937U1), all a phone waiting for Allow shows
    return next((key for key, names in MAKERS if any(re.search(rf'\b{n}', words) for n in names)), 'other')


LOW_BATTERY = 20   # percent


def parse_status(text):
    """dumpsys battery's powered/level/status lines and df -k's last line, as one adb shell call prints them → {battery, charging, free} (absent when not printed)"""
    out = {}
    m = re.search(r'^\s*level:\s*(\d+)', text, re.M)
    if m: out['battery'] = int(m[1])
    m = re.search(r'^\s*status:\s*(\d+)', text, re.M)
    # BatteryManager's 2 charging, 5 full: a phone on a computer's USB port may show "USB powered: true" and still be discharging (4), as a OnePlus Pad 3 did
    if m: out['charging'] = m[1] in ('2', '5')
    row = next((l.split() for l in text.splitlines() if l.startswith('/') and len(l.split()) >= 6), None)
    if row and row[3].isdigit(): out['free'] = int(row[3]) * 1024
    return out


def size_text(n): return f'{n / 1e9:.0f} GB' if n >= 10e9 else f'{n / 1e9:.1f} GB' if n >= 1e9 else f'{n / 1e6:.0f} MB'


def battery_warning(d):
    b = d.get('battery')
    if b is None or b >= LOW_BATTERY: return ''
    return (f'Battery {b}%: a computer\'s USB port charges slowly, and an install takes a few minutes. Charge the phone a little first, from a wall charger.'
            if not d.get('charging') or b < 10 else f'Battery {b}%: it\'s charging, but slowly from a computer\'s USB port. An install takes a few minutes.')


def readout(d):   # " · 212 GB free · battery 64%", for the phone's line on the checklist and the Phones page
    bits = ([f'{size_text(d["free"])} free'] if d.get('free') is not None else []) + ([f'battery {d["battery"]}%'] if d.get('battery') is not None else [])
    return ''.join(f' · {b}' for b in bits)
