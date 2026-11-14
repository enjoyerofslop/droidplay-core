"""DroidPlay for Windows' first run, as the desktop app (scripts/nxnative_android.py) shows it, from its Qt-free helpers in dm/android/desktop: the "Get started"
checklist's four steps worked out from what the computer has (first_run.py), Install tools' one bar across --setup's stages, and plain words for what went wrong
with the report for #help (explain.py), matched against the packing script's, nx_extract's and adb's own messages. No PySide6, phone or network needed."""
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/android/desktop'))
import first_run as fr   # noqa: E402  # pyright: ignore[reportMissingImports]  (dm/android/desktop, put on the path above)
import explain as ex   # noqa: E402  # pyright: ignore[reportMissingImports]
import game_data as gd   # noqa: E402  # pyright: ignore[reportMissingImports]
import phone_guide as pg   # noqa: E402  # pyright: ignore[reportMissingImports]
sys.path.insert(0, str(REPO / 'dm/android/tools'))
import phone_id   # noqa: E402  # pyright: ignore[reportMissingImports]  (dm/android/tools, put on the path above)
import wifi   # noqa: E402  # pyright: ignore[reportMissingImports]  (dm/android/desktop)

ROWS_OK = [{'name': n, 'ok': True, 'detail': '', 'fix': ''} for n in ('Python module lz4', '.NET SDK 10', 'Android SDK', 'NDK', 'CMake', 'build-tools 36.0.0',
                                                                       'platform android-35', 'adb', 'JDK keytool', 'JDK javac', 'signing key', 'game hosts')]


def rows(*missing): return [dict(r, ok=r['name'] not in missing) for r in ROWS_OK]


class Keys(unittest.TestCase):
    def setUp(self): self.tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, self.tmp)

    def test_none_picked_asks_for_them(self):
        s = fr.keys_step('')
        self.assertEqual((s.state, s.button), (fr.TODO, 'Choose prod.keys…'))

    def test_prod_keys_are_done(self):
        f = self.tmp / 'prod.keys'; f.write_text('header_key = 00\nmaster_key_00 = 11\n')
        s = fr.keys_step(str(f))
        self.assertTrue(s.done); self.assertIn('prod.keys', s.text)

    def test_title_keys_are_a_problem(self):   # Lockpick_RCM writes both: title.keys holds rights IDs and their keys, which can't open a game alone
        f = self.tmp / 'title.keys'; f.write_text('01006f8002326000000000000000000a = 0123456789abcdef0123456789abcdef\n')
        s = fr.keys_step(str(f))
        self.assertEqual(s.state, fr.PROBLEM); self.assertIn('not title.keys', s.text)

    def test_a_file_since_moved_is_a_problem(self):
        s = fr.keys_step(str(self.tmp / 'gone.keys'))
        self.assertEqual(s.state, fr.PROBLEM); self.assertIn("isn't there any more", s.text)


class Tools(unittest.TestCase):
    def test_looking_while_check_runs(self):
        self.assertEqual(fr.tools_step(None, True).state, fr.WORKING)

    def test_all_there_is_done(self):
        self.assertTrue(fr.tools_step(rows(), True).done)

    def test_a_new_pc_installs_all_three(self):
        s = fr.tools_step(rows('.NET SDK 10', 'Android SDK', 'adb', 'JDK keytool', 'JDK javac'), True)
        self.assertEqual((s.state, s.button), (fr.TODO, 'Install tools…')); self.assertIn('1.5 GB', s.text)

    def test_only_what_is_missing_is_named(self):
        s = fr.tools_step(rows('NDK'), True)
        self.assertIn('Still to install: the Android SDK.', s.text); self.assertNotIn('Java', s.text.split('Still')[1])

    def test_elsewhere_settings_says_how(self):   # --setup installs on Windows and x86_64 Linux: a Mac, or Linux on arm64, gets Settings' how-to
        s = fr.tools_step(rows('.NET SDK 10'), False)
        self.assertEqual(s.button, 'Open Settings'); self.assertIn('Settings says how', s.text)

    def test_one_bar_while_setup_runs(self):
        s = fr.tools_step(rows('NDK'), True, ('running', 0.4, 'Installing the Android SDK · the NDK (4 of 5)'))
        self.assertEqual((s.state, s.fraction), (fr.WORKING, 0.4))

    def test_a_stopped_setup_says_why_and_tries_again(self):
        s = fr.tools_step(rows('NDK'), True, ('failed', 'the download does not match its sha1'))
        self.assertEqual((s.state, s.button), (fr.PROBLEM, 'Try again')); self.assertIn('does not match', s.text)

    def test_a_check_that_failed_looks_again(self):
        s = fr.tools_step([{'name': 'check', 'ok': False, 'detail': 'boom', 'fix': ''}], True)
        self.assertEqual((s.state, s.button), (fr.PROBLEM, 'Look again'))


class Phone(unittest.TestCase):
    S25 = {'serial': 'R5C0', 'state': 'device', 'name': 'Galaxy S25 Edge', 'model': 'Galaxy S25 Edge', 'android': '16'}

    def test_no_adb_points_at_the_tools(self):
        s = fr.phone_step([], False)
        self.assertEqual(s.state, fr.TODO); self.assertIn('step 2', s.text)

    def test_connected_is_done_with_its_name(self):
        s = fr.phone_step([self.S25], True)
        self.assertTrue(s.done); self.assertIn('Galaxy S25 Edge', s.text); self.assertIn('Android 16', s.text)

    def test_one_waiting_for_allow_says_what_to_tap(self):   # adb lists it with no model until it allows this computer
        s = fr.phone_step([{'serial': 'R5C0', 'state': 'unauthorized', 'model': ''}], True)
        self.assertEqual(s.state, fr.WAITING); self.assertIn('Your phone', s.text); self.assertIn('Allow', s.text)

    def test_offline_says_to_plug_it_again(self):
        s = fr.phone_step([dict(self.S25, state='offline')], True)
        self.assertEqual(s.state, fr.PROBLEM); self.assertIn('plug it back in', s.text)

    def test_offline_over_wifi_says_to_wake_it(self):   # an offline Wi-Fi entry left from a phone that slept
        s = fr.phone_step([dict(self.S25, serial='192.168.1.20:37123', state='offline')], True)
        self.assertEqual(s.state, fr.PROBLEM); self.assertIn('over Wi-Fi', s.text); self.assertNotIn('plug', s.text)

    def test_one_set_up_before_stays_done_unplugged(self):   # the step can't come undone each time a phone is unplugged, or the checklist would return at every launch
        self.assertTrue(fr.phone_step([], True, seen=True).done)
        self.assertEqual(fr.phone_step([], True).state, fr.TODO)

    def test_battery_and_free_space_on_its_line(self):
        s = fr.phone_step([dict(self.S25, battery=64, charging=False, free=212 * 10**9)], True)
        self.assertIn('212 GB free · battery 64%', s.text); self.assertNotIn('⚠', s.text)

    def test_a_nearly_flat_phone_is_warned(self):   # the S25 Edge sat at 3-4% on the test PC's USB port
        s = fr.phone_step([dict(self.S25, battery=4, charging=True)], True)
        self.assertTrue(s.done); self.assertIn('⚠ Battery 4%', s.text); self.assertIn('wall charger', s.text)

    def test_the_picked_phone_first(self):
        pad = dict(self.S25, serial='PAD', name='OnePlus Pad 3')
        self.assertIn('OnePlus Pad 3', fr.phone_step([self.S25, pad], True, picked='PAD').text)

    def test_one_linux_wont_open_points_at_the_guide(self):   # no udev rule for this user: it said "connected but not ready (adb says "no")"
        s = fr.phone_step([{'serial': 'R5C0', 'state': phone_id.NO_PERMISSIONS, 'model': '', 'why': 'user andrew is not in the plugdev group'}], True)
        self.assertEqual((s.state, s.button), (fr.PROBLEM, 'Set up my phone…')); self.assertIn('no permissions', s.text); self.assertIn('Linux', s.text)


class PhoneId(unittest.TestCase):   # one phone, one name: over USB, over Wi-Fi by address or by mDNS name, before and after its port changes (phone_id.py)
    MDNS = 'adb-R5CY51ZLGKH-AbCdEf._adb-tls-connect._tcp'

    def test_transport_from_the_serial(self):
        for serial, t in (('R5CY51ZLGKH', 'usb'), ('emulator-5554', 'usb'), ('192.168.1.20:37123', 'tcp'), ('[fe80::1%wlan0]:5555', 'tcp'), ('pixel.lan:5555', 'tcp'),
                          (self.MDNS, 'mdns'), (self.MDNS + '.', 'mdns')):
            self.assertEqual(phone_id.transport(serial), t, serial)
        self.assertFalse(phone_id.wifi('R5CY51ZLGKH')); self.assertTrue(phone_id.wifi(self.MDNS))

    def test_mdns_names_carry_the_serial_number(self):   # a phone found on the network is known before anything is asked of it
        self.assertEqual(phone_id.mdns_serialno(self.MDNS), 'R5CY51ZLGKH')
        self.assertEqual(phone_id.mdns_serialno('adb-ZY22-ABC1-q1w2e3._adb-tls-connect._tcp'), 'ZY22-ABC1')   # the suffix is after the last hyphen
        self.assertIsNone(phone_id.mdns_serialno('adb-0123456789ABCDEF-q1w2e3._adb-tls-connect._tcp'))   # a placeholder is no one's
        self.assertIsNone(phone_id.mdns_serialno('192.168.1.20:37123'))

    def test_phones_offering_a_connection_on_the_network(self):   # adb mdns services, as adb 35 and older adbs print it; pairing services are not connections
        out = ('List of discovered mdns services\n'
               'adb-R5CY51ZLGKH-AbCdEf\t_adb-tls-connect._tcp\t192.168.1.20:41011\n'
               'adb-e7879cce-q1w2e3\t_adb-tls-connect._tcp.\t10.0.0.7:39999\n'
               'adb-R5CY51ZLGKH-AbCdEf\t_adb-tls-pairing._tcp\t192.168.1.20:37000\n'
               'adb-0123456789ABCDEF-zzzzzz\t_adb-tls-connect._tcp\t10.0.0.9:5555\n')
        self.assertEqual(phone_id.mdns_services(out), [('R5CY51ZLGKH', '192.168.1.20:41011'), ('e7879cce', '10.0.0.7:39999')])
        self.assertEqual(phone_id.mdns_services(''), [])

    def test_the_id_is_the_serial_number_or_the_android_id(self):
        self.assertEqual(phone_id.from_props('R5CY51ZLGKH\r\n9774d56d682e549c\r\n'), 'R5CY51ZLGKH')
        for placeholder in ('0123456789ABCDEF', 'unknown', ''):
            self.assertEqual(phone_id.from_props(f'{placeholder}\n9774D56D682E549C\n'), 'aid-9774d56d682e549c', placeholder)
        self.assertIsNone(phone_id.from_props('unknown\nnull\n')); self.assertIsNone(phone_id.from_props(''))
        self.assertIsNone(phone_id.from_props('\n0000000000000000\n'))
        self.assertEqual(phone_id.from_props('ab/cd\n9774d56d682e549c'), 'aid-9774d56d682e549c')   # a serial number no file could be named after
        self.assertEqual(phone_id.safe(phone_id.from_props('\n9774d56d682e549c')), 'aid-9774d56d682e549c')   # the ID is a name every file system takes

    def test_one_entry_per_phone_cable_first(self):
        usb, tcp, mdns = ({'serial': s, 'id': 'R5CY51ZLGKH'} for s in ('R5CY51ZLGKH', '192.168.1.20:37123', self.MDNS))
        pad = {'serial': '10.0.0.7:5555', 'id': 'e7879cce'}; new = {'serial': 'XYZ', 'id': None}
        self.assertEqual(phone_id.one_per_phone([tcp, pad, mdns, usb, new]), [usb, pad, new])   # in the order each phone first came
        self.assertEqual(phone_id.one_per_phone([tcp, mdns]), [mdns])
        self.assertEqual(phone_id.one_per_phone([{'serial': 'A', 'id': None}, {'serial': 'B', 'id': None}]), [{'serial': 'A', 'id': None}, {'serial': 'B', 'id': None}])

    def test_install_again_finds_the_phones_serial_now(self):   # the build's --serial was the old port's; the phone is back on another, or on its cable
        seen = {'192.168.1.20:37123': 'R5CY51ZLGKH', '10.0.0.7:5555': 'e7879cce'}
        ready = [{'serial': '10.0.0.7:5555', 'id': 'e7879cce'}, {'serial': '192.168.1.20:41011', 'id': 'R5CY51ZLGKH'}]
        self.assertEqual(phone_id.current('192.168.1.20:37123', ready, seen), '192.168.1.20:41011')
        self.assertEqual(phone_id.current('192.168.1.20:37123', ready + [{'serial': 'R5CY51ZLGKH', 'id': 'R5CY51ZLGKH'}], seen), 'R5CY51ZLGKH')   # the cable first
        self.assertEqual(phone_id.current('10.0.0.7:5555', ready, seen), '10.0.0.7:5555')
        self.assertIsNone(phone_id.current('192.168.1.20:37123', ready[:1], seen))   # not connected now: the build's serial stays, and the script waits for it
        self.assertIsNone(phone_id.current('192.168.9.9:5555', ready, seen))

    def test_the_picked_phone_is_found_by_its_id(self):   # the picker keeps the phone when its cable comes out and its Wi-Fi entry takes over, and the reverse
        ids = {'R5CY51ZLGKH': 'R5CY51ZLGKH', '192.168.1.20:41011': 'R5CY51ZLGKH', '10.0.0.7:5555': 'e7879cce'}
        picker = ['10.0.0.7:5555', '192.168.1.20:41011', None]
        self.assertEqual(phone_id.find(picker, ['R5CY51ZLGKH'], ids), 1)   # saved by ID before the port changed
        self.assertEqual(phone_id.find(picker, [None, '', '192.168.1.20:37123', '10.0.0.7:5555'], ids), 0)   # the first key that names a listed phone
        self.assertEqual(phone_id.find(picker, ['192.168.1.20:37123'], ids), -1)
        self.assertEqual(phone_id.find(['R5CY51ZLGKH'], ['192.168.1.20:41011', ids['192.168.1.20:41011']], ids), 0)   # the Phones page's Wi-Fi row picks the cable entry


class AdbDevices(unittest.TestCase):   # adb devices -l as adb 37 prints it, read into one dict a phone (phone_id.devices)
    OUT = ('* daemon not running; starting now at tcp:5037\n* daemon started successfully\nList of devices attached\n'
           'R5CY51ZLGKH            no permissions (user andrew is not in the plugdev group); see [http://developer.android.com/tools/device.html] usb:3-3 transport_id:2\n'
           '0123ABC\tunauthorized usb:1-1 transport_id:1\n'
           '192.168.1.20:37123     device product:pa3q model:SM_S937U1 device:pa3q transport_id:4\n'
           'ZY22\tno permissions (missing udev rules? user is in the plugdev group); see [http://developer.android.com/tools/device.html]\n\n')

    def test_each_state_whole(self):
        got = [(d['serial'], d['state'], d['model'], d['why']) for d in phone_id.devices(self.OUT)]
        self.assertEqual(got, [('R5CY51ZLGKH', 'no permissions', '', 'user andrew is not in the plugdev group'), ('0123ABC', 'unauthorized', '', ''),
                               ('192.168.1.20:37123', 'device', 'SM S937U1', ''), ('ZY22', 'no permissions', '', 'missing udev rules? user is in the plugdev group')])

    def test_nothing_listed(self):
        self.assertEqual(phone_id.devices('List of devices attached\n\n'), []); self.assertEqual(phone_id.devices(''), [])


class WiFi(unittest.TestCase):   # PairDialog and Connect…: adb's mDNS list, pair and connect replies as adb 35 prints them, in plain words (wifi.py)
    MDNS = ('List of discovered mdns services\n'
            'adb-e7879cce-DkIYY8\t_adb-tls-pairing._tcp\t192.168.50.66:41234\n'
            'adb-e7879cce-DkIYY8\t_adb-tls-connect._tcp\t192.168.50.66:37853\n'
            'adb-R5CY51ZLGKH-AbCdEf\t_adb-tls-pairing._tcp.\t192.168.50.20:39001\n'
            'studio-xh3Kq9\t_adb-tls-pairing._tcp\t192.168.50.30:40000\n'
            'adb-GF518M0010960000\t_adb._tcp\t192.168.50.92:5555\n')

    def test_the_services_on_the_network(self):   # the Pad 3 offered both at once (q215); a QR code's pairing service is named studio-…
        self.assertEqual(phone_id.services(self.MDNS)[0], ('adb-e7879cce-DkIYY8', 'pairing', '192.168.50.66:41234'))
        self.assertEqual([k for _, k, _ in phone_id.services(self.MDNS)], ['pairing', 'connect', 'pairing', 'pairing', 'tcp'])
        self.assertEqual(phone_id.mdns_services(self.MDNS), [('e7879cce', '192.168.50.66:37853')])
        for name in ('adb-e7879cce-DkIYY8', 'adb-e7879cce-DkIYY8._adb-tls-pairing._tcp', 'adb-e7879cce-DkIYY8._adb-tls-connect._tcp.'):
            self.assertEqual(phone_id.serialno_of(name), 'e7879cce', name)
        self.assertIsNone(phone_id.serialno_of('studio-xh3Kq9'))

    def test_phones_showing_a_pairing_code_are_listed_with_their_address(self):
        self.assertEqual(wifi.pairing(self.MDNS), [('e7879cce · 192.168.50.66:41234', '192.168.50.66:41234'), ('R5CY51ZLGKH · 192.168.50.20:39001', '192.168.50.20:39001'),
                                                   ('studio-xh3Kq9 · 192.168.50.30:40000', '192.168.50.30:40000')])
        self.assertEqual(wifi.pairing('List of discovered mdns services\n'), [])

    def test_pairing_replies_in_plain_words(self):
        self.assertEqual(wifi.pair_result(True, 'Successfully paired to 192.168.50.66:41234 [guid=adb-e7879cce-DkIYY8]'), (True, 'e7879cce', 'Paired'))
        ok, _, text = wifi.pair_result(False, 'Failed: Wrong password or connection was dropped.')
        self.assertFalse(ok); self.assertIn("code didn't match", text)
        for raw in ('Failed: Unable to start pairing client.', 'failed to connect to 192.168.50.66:41234', 'error: connect failed: Connection refused'):
            self.assertIn("couldn't reach the phone", wifi.pair_result(False, raw)[2], raw)
        self.assertIn('Pairing failed: something new', wifi.pair_result(False, 'something new')[2])   # adb's own words when they're new

    def test_connect_replies_in_plain_words(self):   # adb connect exits 0 when it fails too: the words decide
        self.assertTrue(wifi.connect_result(True, 'connected to 192.168.50.66:37853')[0]); self.assertTrue(wifi.connect_result(True, 'already connected to 192.168.50.66:37853')[0])
        ok, text = wifi.connect_result(True, 'failed to connect to 192.168.50.66:5555'); self.assertFalse(ok); self.assertIn('No phone answered there', text)
        self.assertIn('pair it first', wifi.connect_result(True, 'failed to authenticate to 192.168.50.66:37853')[1])
        self.assertIn('not one adb can use', wifi.connect_result(False, "cannot resolve host 'pad' and port 5555: nodename nor servname provided")[1])

    def test_discover_lists_every_phone_on_the_network(self):   # it offered the first one only
        devs = 'List of devices attached\nR5CY51ZLGKH\tdevice\n'
        mdns = self.MDNS + 'adb-R5CY51ZLGKH-AbCdEf\t_adb-tls-connect._tcp\t192.168.50.20:40111\n'
        self.assertEqual(wifi.discovered(mdns, devs, {'e7879cce': 'OnePlus Pad 3'}), [
            ('OnePlus Pad 3 · 192.168.50.66:37853', '192.168.50.66:37853', False),
            ('GF518M0010960000 · 192.168.50.92:5555 · adb tcpip, not encrypted', '192.168.50.92:5555', False),
            ('R5CY51ZLGKH · 192.168.50.20:40111', '192.168.50.20:40111', True)])   # on its cable already: the same phone
        self.assertEqual(wifi.discovered('List of discovered mdns services\n', devs), [])

    def test_disconnect_by_how_the_phone_is_connected(self):   # adb disconnect adds :5555 to an mDNS name (adb 37): "no such device 'adb-…._tcp:5555'"
        self.assertEqual(wifi.disconnect_plan('192.168.50.66:37853'), ('disconnect', '192.168.50.66:37853'))
        self.assertEqual(wifi.disconnect_plan('adb-e7879cce-DkIYY8._adb-tls-connect._tcp'), ('wireless off', 'adb-e7879cce-DkIYY8._adb-tls-connect._tcp'))
        self.assertEqual(wifi.disconnect_plan('e7879cce'), ('usb', 'e7879cce'))

    def test_a_phone_that_dropped_off_is_looked_for_by_its_id(self):   # its new port first (adb mdns services), then its old address
        mdns = self.MDNS.replace('192.168.50.66:37853', '192.168.50.66:41011')
        self.assertEqual(wifi.reconnect_targets('e7879cce', mdns, '192.168.50.66:37853'), ['192.168.50.66:41011', '192.168.50.66:37853'])
        self.assertEqual(wifi.reconnect_targets('e7879cce', '', 'adb-e7879cce-DkIYY8._adb-tls-connect._tcp'), [])   # an mDNS name is no address to connect to
        self.assertEqual(wifi.reconnect_targets('aid-9774d56d682e549c', mdns, '10.0.0.7:5555'), ['10.0.0.7:5555'])   # no serial number to find it by: its address

    def test_the_offline_entries_a_phone_left_are_dropped_once_it_is_back(self):
        devs = [{'serial': '192.168.50.66:37853', 'state': 'offline'}, {'serial': '192.168.50.66:41011', 'state': 'device', 'id': 'e7879cce'},
                {'serial': '10.0.0.7:5555', 'state': 'offline'}, {'serial': 'R5CY51ZLGKH', 'state': 'offline'}]
        self.assertEqual(wifi.stale(devs, 'e7879cce', {}, '192.168.50.66:41011'), ['192.168.50.66:37853'])   # the same address, the old port
        self.assertEqual(wifi.stale(devs, 'e7879cce', {'10.0.0.7:5555': 'e7879cce'}, 'adb-e7879cce-DkIYY8._adb-tls-connect._tcp'), ['10.0.0.7:5555'])   # known by ID this session
        self.assertEqual(wifi.stale(devs, 'e7879cce', {}, 'R5CY51ZLGKH'), [])   # back on its cable: an address's IP isn't known to be its

    def test_a_cable_phones_wifi_address(self):   # Use over Wi-Fi reads it before adb tcpip, while the cable still reaches the phone
        out = ('1: lo: <LOOPBACK,UP,LOWER_UP> mtu 65536 qdisc noqueue state UNKNOWN group default qlen 1000\n    inet 127.0.0.1/8 scope host lo\n'
               '14: rmnet_data2: <UP,LOWER_UP> mtu 1500 qdisc mq state UNKNOWN group default qlen 1000\n    inet 10.168.2.17/30 scope global rmnet_data2\n'
               '30: wlan0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc mq state UP group default qlen 3000\n'
               '    inet 192.168.50.66/24 brd 192.168.50.255 scope global dynamic wlan0\n')
        self.assertEqual(wifi.wifi_ip(out), '192.168.50.66')   # the Pad 3's, as q215 read it; not the mobile network's
        self.assertIsNone(wifi.wifi_ip(out.split('30: wlan0')[0].replace('rmnet_data2', 'rmnet_data2')))   # on mobile data only: no Wi-Fi address
        self.assertEqual(wifi.wifi_ip('5: eth0: <UP>\n    inet 10.0.0.9/24 brd 10.0.0.255 scope global eth0\n'), '10.0.0.9')   # a handheld on a dock's Ethernet

    def test_adb_tcpip_replies(self):
        self.assertEqual(wifi.tcpip_result(True, 'restarting in TCP mode port: 5555')[0], True)
        ok, text = wifi.tcpip_result(False, 'error: closed'); self.assertFalse(ok); self.assertIn('error: closed', text)
        self.assertIn('not encrypted', wifi.TCPIP_WARNING); self.assertIn('Android 10', wifi.TCPIP_WARNING)

    def test_after_pairing_the_phones_connection_is_found(self):   # adb's own connection by mDNS name, or the service the phone offers at the same address
        devs = 'List of devices attached\nR5CY51ZLGKH\tdevice\nadb-e7879cce-DkIYY8._adb-tls-connect._tcp\tdevice\n'
        self.assertEqual(wifi.listed(devs, 'e7879cce', '192.168.50.66'), 'adb-e7879cce-DkIYY8._adb-tls-connect._tcp')
        self.assertEqual(wifi.listed('List of devices attached\n192.168.50.66:37853\toffline\n', 'e7879cce', '192.168.50.66'), None)   # not ready: not connected
        self.assertEqual(wifi.connect_target(self.MDNS, 'e7879cce', '192.168.50.66'), '192.168.50.66:37853')
        self.assertEqual(wifi.connect_target(self.MDNS, None, '192.168.50.66'), '192.168.50.66:37853')   # a guid adb didn't print: by the address
        self.assertIsNone(wifi.connect_target(self.MDNS, 'R5CY51ZLGKH', '192.168.50.20'))   # paired, but not offering a connection yet


class Game(unittest.TestCase):
    BD = {'folder': 'D:/Games/BD', 'name': 'Pokémon Brilliant Diamond', 'state': 'build', 'tested': True, 'error': None}

    def test_none_yet_asks_for_a_folder(self):
        s = fr.game_step([], [], False)
        self.assertEqual((s.state, s.button), (fr.TODO, 'Add a game…'))

    def test_reading_names_the_folder(self):
        s = fr.game_step(['D:/Games/BD'], [], True)
        self.assertEqual((s.state, s.fraction), (fr.WORKING, -1)); self.assertIn('BD', s.text)

    def test_read_is_done_and_builds_next(self):
        s = fr.game_step(['D:/Games/BD'], [self.BD], False)
        self.assertTrue(s.done); self.assertEqual((s.button, s.ref), ('Build and install', 'D:/Games/BD')); self.assertIn('played on Android', s.text)

    def test_waiting_for_tools_isnt_called_unreadable(self):   # a supported game read before the tools came read as "Not supported" (droidplay-core #55)
        s = fr.game_step(['D:/Games/BD'], [dict(self.BD, state='tools')], False)
        self.assertEqual(s.state, fr.TODO); self.assertIn('once the tools are installed', s.text)

    def test_unreadable_says_why(self):
        s = fr.game_step(['D:/x'], [dict(self.BD, name='x', state='none', error='no NSP or XCI in it')], False)
        self.assertEqual(s.state, fr.PROBLEM); self.assertIn('no NSP or XCI', s.text)

    def test_names_are_escaped(self):
        self.assertIn('&lt;b&gt;', fr.game_step(['D:/x'], [dict(self.BD, name='<b>')], False).text)


class SetupBar(unittest.TestCase):   # Install tools: one bar across the parts --setup installs, rather than one per stage that starts again from nothing
    def run_events(self, *evs, t0=0.0):
        p = fr.SetupProgress(t0)
        for e in evs: p.feed(e)
        return p

    def test_a_new_pc_runs_all_four_weighted_by_size(self):
        p = self.run_events({'ev': 'stage', 'k': 1, 'n': 4, 'name': 'JDK 21'}, {'ev': 'progress', 'k': 1, 'step': 'downloading the JDK', 'done': 95 << 20, 'total': 190 << 20, 'unit': 'B'})
        self.assertEqual(p.planned(), [s[0] for s in fr.SETUP_STAGES])
        self.assertAlmostEqual(p.fraction(), 95 / 1580, places=3)
        self.assertTrue(p.text(5).startswith('Downloading Java · 95 MB of 190 MB'))

    def test_later_stages_count_what_came_before(self):
        p = self.run_events({'ev': 'stage', 'k': 1, 'n': 2, 'name': '.NET SDK 10'}, {'ev': 'stage_end', 'k': 1},
                            {'ev': 'stage', 'k': 2, 'n': 2, 'name': 'Android SDK packages'}, {'ev': 'progress', 'k': 2, 'step': 'installing ndk;28.2.13676358', 'done': 3, 'total': 5, 'unit': ''})
        self.assertEqual(p.planned(), ['.NET SDK 10', 'Android SDK packages'])
        self.assertAlmostEqual(p.fraction(), (290 + 950 * 130 / 850) / 1240, places=3)   # the NDK, most of the stage, still to come
        self.assertIn('Installing the Android SDK · the NDK (4 of 5)', p.text(5))

    def test_unplanned_stages_are_the_last_ones(self):   # a PC with the command-line tools already: n=2 after the JDK means the packages alone are left
        p = self.run_events({'ev': 'stage', 'k': 1, 'n': 2, 'name': 'JDK 21'})
        self.assertEqual(p.planned(), ['JDK 21', 'Android SDK packages'])

    def test_time_left_once_there_is_a_rate(self):
        p = self.run_events({'ev': 'stage', 'k': 1, 'n': 1, 'name': 'JDK 21'}, {'ev': 'progress', 'k': 1, 'step': 'downloading the JDK', 'done': 50, 'total': 100, 'unit': 'B'})
        self.assertNotIn('left', p.text(5))   # too soon to say
        self.assertIn('about 4 minutes left', p.text(240))
        self.assertIn('less than a minute left', p.text(40))


class Count(unittest.TestCase):
    def test_counts_the_done_steps(self):
        steps = [fr.keys_step(''), fr.tools_step(rows(), True), fr.phone_step([], True, seen=True), fr.game_step([], [], False)]
        self.assertEqual(fr.ready_count(steps), 2)



class Explain(unittest.TestCase):   # the messages as the script, nx_extract and adb print them; each gets a title, what to do and its button
    def test_phone_full_names_the_sizes(self):
        h = ex.explain('the phone has 2.1 GB free: the RomFS image needs 4.7 GB, plus 268 MB left free for Android and the app. Free up 2.9 GB on the phone, then install again',
                       phone='Galaxy S25 Edge', game='Pokémon Brilliant Diamond')
        self.assertEqual((h.title, h.action), ('Not enough space on Galaxy S25 Edge', 'retry'))
        self.assertIn('needs 4.7 GB and Galaxy S25 Edge has 2.1 GB free. Free up 2.9 GB', h.body)

    def test_a_phone_linux_wont_open_says_what_to_install(self):   # the picker's tooltip, and the console build's pick_device
        for t in ('device no permissions (user andrew is not in the plugdev group)',
                  '0 adb devices ready (R5C (no permissions: missing udev rules? user is in the plugdev group)): connect one, or pick it with --serial. '
                  'A phone with "no permissions" needs a udev rule that lets you use it: docs/android.md, "Phones on Linux"'):
            h = ex.explain(t); self.assertEqual(h.action, 'phone'); self.assertIn('Linux', h.title); self.assertIn('android-udev', h.body)

    def test_a_step_that_failed_on_a_nearly_full_phone(self):   # says_when_phone_full's line under a step that printed nothing
        h = ex.explain('adb -s X shell sh /data/local/tmp/nxnative_mkdirs.sh failed (exit status 1): no output\nThe phone has only 310 MB free, which is likely why: free up space')
        self.assertTrue(h.title.startswith('Not enough space on')); self.assertIn('310 MB', h.body)

    def test_adb_no_space_is_the_phones_and_oserror_the_pcs(self):
        self.assertIn('your phone', ex.explain('adb -s X push a b failed (exit status 1): adb: error: failed to copy: No space left on device').title)
        self.assertEqual(ex.explain("[Errno 28] No space left on device: 'D:\\BD\\x'", folder='D:\\Games\\BD').title, 'Not enough space on this computer')

    def test_pc_full_names_the_drive(self):
        h = ex.explain('less than 12 GB free on the volume of D:\\Games\\BD\\0100000011D90000_build', game='Pokémon Brilliant Diamond')
        self.assertEqual((h.title, h.action), ('Not enough space on this computer', 'folder')); self.assertIn('about 12 GB free on drive D:', h.body)

    def test_unauthorized_asks_for_allow(self):
        for t in ('0 adb devices ready (R5C0 (unauthorized)): connect one, or pick it with --serial', 'adb: device unauthorized.'):
            h = ex.explain(t, phone='Galaxy S25 Edge'); self.assertEqual((h.title, h.action), ("Galaxy S25 Edge hasn't allowed this computer yet", 'phone'))

    def test_a_web_servers_401_isnt_the_phone(self):
        self.assertEqual(ex.explain('<urlopen error HTTP Error 401: Unauthorized>', setup=True).title, 'The download stopped')

    def test_offline_and_unplugged_mid_copy(self):
        for t in ("adb: device offline", "adb: error: device 'R5C0' not found", 'adb -s R5C0 push x y failed (exit status 255): no output', 'error: no devices/emulators found'):
            self.assertEqual(ex.explain(t).title, 'Your phone stopped answering', t)

    def test_no_phone(self):
        h = ex.explain('0 adb devices ready (none): connect one, or pick it with --serial')
        self.assertEqual((h.title, h.label), ('No phone connected', 'Set up my phone…')); self.assertIn('over Wi-Fi', h.body)

    def test_a_phone_that_dropped_off_wifi_is_told_wifi_things(self):   # "unplug it and plug it back in" was a cable's advice, at the worst moment
        for t in ("adb -s 192.168.1.20:37123 push x y: adb lost the phone: error: closed", "adb: error: device '192.168.1.20:37123' not found"):
            h = ex.explain(t, phone='Galaxy S25 Edge', wifi=True)
            self.assertEqual((h.title, h.action), ('Galaxy S25 Edge dropped off Wi-Fi', 'retry'), t); self.assertIn('Wireless debugging is still on', h.body)
            self.assertNotIn('plug', h.body)
        gave_up = ('the phone did not come back within 180 s after adb lost it (error: closed). Wake it, check that it is on the same network as this computer and '
                   'that Wireless debugging is still on, then install again: the copy goes on from what the phone holds')
        self.assertEqual(ex.explain(gave_up).title, 'Your phone dropped off Wi-Fi')   # the script's words say it was Wi-Fi
        cable = 'the phone did not come back within 180 s after adb lost it (device offline). Plug it back in, then install again: the copy goes on from what the phone holds'
        self.assertEqual(ex.explain(cable).title, 'Your phone stopped answering')

    def test_a_connection_that_keeps_dropping(self):
        wifi = ('adb -s 10.0.0.7:5555 push x y: adb lost the phone: error: closed\nadb lost the phone 6 times in a row with nothing copied in between, so the connection is too poor to finish '
                'over: move the phone closer to the router (5 GHz), or use a USB cable, then install again')
        h = ex.explain(wifi, phone='OnePlus Pad 3'); self.assertEqual(h.title, 'The connection to OnePlus Pad 3 keeps dropping'); self.assertIn('closer to the router', h.body)
        h = ex.explain('adb lost the phone 6 times in a row with nothing copied in between, so the connection is too poor to finish over: try another cable or USB port, then install again')
        self.assertIn('another cable or USB port', h.body)

    def test_a_wifi_phone_that_forgot_this_pc_pairs_again(self):
        self.assertIn('Pair over Wi-Fi', ex.explain('adb: device unauthorized.', wifi=True).body)

    def test_keys(self):
        self.assertEqual(ex.explain('give -k <prod.keys>: the game is in NSP / XCI files, which are encrypted').action, 'keys')
        h = ex.explain('[RED] BD.nsp: prod.keys has no titlekek_13 (master key 13, which came with firmware 17.0.0): the keys are older than the game; dump prod.keys again')
        self.assertEqual(h.title, 'Your keys are older than this game'); self.assertIn('firmware 17.0.0 or later', h.body)

    def test_no_ticket(self):
        h = ex.explain('BD.nsp: no title key for rights ID 0100000011d90000000000000000000d: the package has no common ticket for it')
        self.assertEqual((h.title, h.action), ("This game file can't be opened with your keys", ''))

    def test_tools_missing_isnt_called_unsupported(self):
        h = ex.explain("reading the packages' titles and versions needs dotnet (.NET SDK 10), which the extraction needs too")
        self.assertEqual((h.title, h.action), ('Install the tools first', 'tools'))

    def test_install_blocks(self):
        self.assertEqual(ex.explain('adb: failed to install x.apk: Failure [INSTALL_FAILED_USER_RESTRICTED: Install canceled by user]').title, 'Your Xiaomi phone blocked the install')
        self.assertEqual(ex.explain('the DroidPlay app (org.nxnative.android) on the phone was signed with another key than ~/.android/nxnative-debug.jks').action, 'backup')

    def test_setup_downloads(self):
        self.assertEqual(ex.explain('<urlopen error [Errno 11001] getaddrinfo failed>', setup=True).body, 'Check your internet connection, then Install tools again.')
        self.assertEqual(ex.explain('the JDK: the download does not match its sha256 (https://aka.ms/x)', setup=True).title, 'The download came through damaged')

    def test_anything_else_is_a_bug_with_the_plain_stage(self):
        h = ex.explain('patch_nso.py exit code 1 on subsdk0 (full output: x)', stage='rewrite')
        self.assertTrue(h.bug); self.assertEqual(h.title, 'The build stopped at "Adapting the game\'s code for Android"'); self.assertIn('#help', h.body)
        self.assertEqual(ex.explain('boom', stage='extract v393216').title, 'The build stopped at "Reading the game files"')
        self.assertEqual(ex.explain('boom', setup=True, stage='Android SDK packages').title, 'Installing the tools stopped at "Installing the Android SDK"')

    # what --info prints when it can't read a game folder: its notes on stderr, the error on stdout (scan_nsps); the Library showed the generic "The build stopped"
    # with "Can't read it" and no reason for a Skyward Sword HD folder on Windows (2026-11-13)
    D = 'D:\\Games\\SSHD'

    def test_script_error_puts_the_error_last(self):
        err = '[12:00:01] skipping game.zip: neither an NSP (PFS0) nor an XCI (HEAD at 0x100)\n[RED] skipping update.nsp: its content meta could not be read: bad header'
        t = ex.script_error('[RED] no game package with a readable content meta in D:\\Games\\SSHD\n', err)
        self.assertTrue(t.splitlines()[-1].startswith('[RED] no game package'))   # the Library shows the last line: the reason, not a note
        self.assertIn('skipping game.zip', t); self.assertIn('skipping update.nsp', t)
        self.assertEqual(ex.script_error('', 'Traceback (most recent call last):\n  File "x"\nValueError: boom'), 'Traceback (most recent call last):\n  File "x"\nValueError: boom')
        self.assertEqual(ex.script_error('', ''), 'no output')

    def test_folder_reads_say_what_to_change(self):
        h = ex.explain('[RED] more than one game in the folder: 01002DA013484000, 0100000000010000; keep one game per folder, or name the one to pack with '
                       '--title-id / TITLE_ID', folder=self.D, read=True)
        self.assertEqual((h.title, h.action, h.bug), ('More than one game in this folder', 'folder', False)); self.assertIn('0100000000010000', h.body)
        h = ex.explain('[RED] no base game NSP found in D:\\Games\\SSHD (title ID 01002DA013484000)', folder=self.D, read=True)
        self.assertEqual((h.title, h.action), ('The game itself is missing from this folder', 'folder'))
        h = ex.explain('no NSP for update 1.0.1 (the game profile needs these versions: 1.0.0, 1.0.1)', game='The Legend of Zelda: Skyward Sword HD', folder=self.D)
        self.assertEqual(h.title, 'An update the build needs is missing'); self.assertIn('update 1.0.1', h.body)

    def test_a_folder_with_nothing_readable_names_the_files_it_passed_over(self):
        t = ex.script_error('[RED] no game package with a readable content meta in D:\\Games\\SSHD', '[12:00:01] skipping SSHD.zip: neither an NSP (PFS0) nor an '
                            'XCI (HEAD at 0x100)\n[RED] skipping SSHD.nsp: its content meta could not be read: NCA header')
        h = ex.explain(t, folder=self.D, read=True)
        self.assertEqual((h.title, h.action, h.bug), ('No game DroidPlay can open in this folder', 'folder', False))
        self.assertIn('<b>SSHD.zip</b>', h.body); self.assertIn('<b>SSHD.nsp</b>', h.body); self.assertIn('prod.keys', h.body)
        self.assertNotIn('passed over', ex.explain('[RED] no game package with a readable content meta in D:\\x', read=True).body)

    def test_the_game_reader_without_nuget_org(self):   # dotnet's restore for nx_extract, as a Windows PC printed it, then the script's own line
        t = ex.script_error('[RED] dotnet exit code 1 (full output: C:\\Temp\\title_probe_x\\logs\\nx_extract_build.log)',
                            "C:\\DroidPlay\\core\\dm\\tools\\nx_extract\\nx_extract.csproj : error NU1100: Unable to resolve 'LibHac (>= 0.19.0)' for 'net10.0'.\n"
                            '    0 Warning(s)\n    1 Error(s)\n\nTime Elapsed 00:00:00.76')
        h = ex.explain(t, read=True)
        self.assertEqual(h.title, "DroidPlay couldn't download its game reader"); self.assertIn('dotnet nuget enable source nuget.org', h.body); self.assertFalse(h.bug)

    def test_a_game_that_draws_with_opengl_or_vulkan_is_not_a_bug(self):   # build_android_from_nsp.py unsupported_graphics' words, as droidplay-core#476's log had them
        h = ex.explain('[RED] FM24 Touch draws with OpenGL, not NVN (main imports EGL and names glShaderSource: it renders with OpenGL). DroidPlay Core on Android runs NVN '
                       'games only so far, so this game cannot be built for Android yet', stage='extract v458752')
        self.assertEqual((h.title, h.bug, h.action), ("This game isn't supported on Android yet", False, '')); self.assertIn('FM24 Touch draws with OpenGL', h.body)
        h = ex.explain('[RED] Hades draws with Vulkan, not NVN (main imports 99 Vulkan functions and not nvnBootstrapLoader). …', game='Hades')
        self.assertIn('Hades draws with Vulkan', h.body)

    def test_an_unknown_read_failure_says_read_not_build(self):
        h = ex.explain('[RED] something new', folder=self.D, read=True)
        self.assertTrue(h.bug); self.assertEqual(h.title, "DroidPlay couldn't read this game"); self.assertNotIn('stopped', h.title)

    # build_android_from_nsp.py run_time_stop's words, on Windows and on a Mac: a registered profile's tables name shaders the game compiles while it runs (droidplay-core#441)
    STOP = ("[RED] Fire Emblem: Three Houses compiles 9 of its shaders while it runs, with its own shader compiler (GLSLC), and game profile 010055D009F78000's shader tables "
            "need them. ")
    PC = "Only a build on a Mac can capture them so far, so this game can't be built on Windows or Linux yet: droidplay-core#441 (https://github.com/enjoyerofslop/droidplay-core/issues/441)"
    MAC = ('Only a build for the Mac captures them so far: build the game for the Mac first (DroidPlay for Mac, or scripts/build_ipa_from_nsp.py on this folder), then build it '
           'for Android again, which takes them from that build. droidplay-core#441 (https://github.com/enjoyerofslop/droidplay-core/issues/441)')

    def test_a_game_only_a_mac_build_can_capture_for_says_so(self):   # not "The build stopped", a bug to report: nothing the player did, and nothing to report
        h = ex.explain(self.STOP + self.PC, game='Fire Emblem: Three Houses', stage='')
        self.assertEqual((h.title, h.bug, h.action), ('This game builds on a Mac only for now', False, 'issue'))   # its button opens #441 (ISSUE_441)
        self.assertIn('Fire Emblem: Three Houses compiles 9 of its shaders while it runs', h.body); self.assertIn('#441', h.body); self.assertIn('issue #441', h.body)
        h = ex.explain(self.STOP + self.MAC, game='Fire Emblem: Three Houses')
        self.assertEqual((h.title, h.action), ('Build it for the Mac first', '')); self.assertIn('DroidPlay for Mac', h.body)

    def test_the_librarys_words_for_run_time_shaders(self):   # before a build (--info's run_time_shaders) and after one said so (its run_time_shaders event)
        self.assertEqual(ex.mac_only(9), 'Builds on a Mac only for now: it compiles 9 shaders while it runs')
        self.assertEqual(ex.mac_only(9, mac=True), 'Build it for the Mac first: it compiles 9 shaders while it runs')
        title, body = ex.run_time_note('Crash <N. Sane>')
        self.assertEqual(title, 'Some shaders come after the first play'); self.assertIn('Crash &lt;N. Sane&gt; compiles some', body)
        self.assertIn('first install may draw some things black', body); self.assertIn('Manage › Update shaders from the phone', body)
        title, body = ex.run_time_note('Crash', game_app=True)
        self.assertIn('this game app may draw some things black', body); self.assertIn('build the game app again', body)
        self.assertIn('Update shaders', ex.RUN_TIME_META)


class Report(unittest.TestCase):   # Copy report: one Discord message, home folder as ~, never key material
    def test_linux_names_its_distribution(self):   # "Linux 7.2.9" said nothing of the fonts, udev rules or libraries a distribution has
        name = ex.system_name({'NAME': 'Bazzite', 'VERSION_ID': '44', 'PRETTY_NAME': 'Bazzite'})   # Bazzite's PRETTY_NAME carries no version
        self.assertTrue(name.startswith('Bazzite 44 (Linux '), name)
        self.assertTrue(ex.system_name({'NAME': 'Ubuntu', 'VERSION_ID': '24.04', 'PRETTY_NAME': 'Ubuntu 24.04.1 LTS'}).startswith('Ubuntu 24.04.1 LTS (Linux '))
        self.assertTrue(ex.system_name({'NAME': 'Arch Linux', 'PRETTY_NAME': 'Arch Linux'}).startswith('Arch Linux (Linux '))

    INFO = {'version': '0.5.0', 'title': 'Build Pokémon Brilliant Diamond', 'stage': 'rewrite', 'k': 3, 'n': 8, 'game': 'Pokémon Brilliant Diamond',
            'game_id': '0100000011D90000', 'game_version': '393216', 'phone': 'Galaxy S25 Edge', 'android': '16', 'gpu': 'Adreno (TM) 830'}

    def test_fits_one_discord_message_however_long_the_log(self):
        log = [f'C:\\Users\\andre\\games\\step {i} ' + 'x' * 150 for i in range(400)]
        text = ex.markdown_report(self.INFO, ['[RED] patch_nso.py exit code 1 on subsdk0'] * 3 + ['[RED] ' + 'y' * 900], log, 'C:\\Users\\andre')
        self.assertLess(len(text), ex.DISCORD_LIMIT)
        self.assertIn('stopped at Adapting the game\'s code for Android (`rewrite`, stage 3 of 8)', text); self.assertIn('Adreno (TM) 830', text)
        self.assertIn('step 399', text); self.assertNotIn('andre', text)

    def test_key_lines_are_dropped(self):
        text = ex.markdown_report(self.INFO, [], ['header_key = 0123456789abcdef0123456789abcdef0123456789abcdef', 'fine'], '/home/p')
        self.assertNotIn('0123456789abcdef', text); self.assertIn('fine', text)

    def test_the_phones_serial_stays_out(self):
        text = ex.markdown_report(self.INFO, ['[RED] adb -s R5CY00000XX push a b failed (exit status 255): no output'], ['$ python b.py D:/BD -k k --serial R5CY00000XX'], '/home/p')
        self.assertNotIn('R5CY00000XX', text); self.assertIn('adb -s <serial> push', text); self.assertIn('--serial <serial>', text)

    def test_a_wifi_phones_mdns_name_and_id_stay_out(self):   # adb-<serial number>-<suffix>._adb-tls-connect._tcp carries the serial number
        text = ex.markdown_report(self.INFO, ['[RED] the phone is back as adb-R5CY00000XX-AbCdEf._adb-tls-connect._tcp: going on'],
                                  ['$ python b.py D:/BD -k k --serial 192.168.1.20:37123 --phone-id R5CY00000XX'], '/home/p')
        self.assertNotIn('R5CY00000XX', text); self.assertIn('back as <serial>', text)

    def test_forward_slashed_home_is_redacted_too(self):
        self.assertNotIn('andre', ex.full_report(self.INFO, 'C:/Users/andre/x.log failed', 'C:\\Users\\andre\\j.jsonl', 'C:\\Users\\andre'))



class Guide(unittest.TestCase):   # PhoneGuide: the brand picked from what adb says, and the battery and space read in the same shell call
    def test_brand_from_the_manufacturer(self):
        for maker, model, want in (('samsung', 'SM-S937U1', 'samsung'), ('Google', 'Pixel 9', 'pixel'), ('Xiaomi', 'Redmi Note 10 Pro', 'xiaomi'),
                                   ('OnePlus', 'OPD2415', 'oneplus'), ('AYN', 'Odin2', 'handheld'), ('Moorechip', 'Retroid Pocket 5', 'handheld'), ('motorola', 'edge', 'other')):
            self.assertEqual(pg.brand_for(maker, model), want, maker)

    def test_a_phone_waiting_for_allow_has_only_its_model(self):   # devices -l's: a Galaxy's model code still says Samsung
        self.assertEqual(pg.brand_for('', 'SM_S937U1'), 'samsung'); self.assertEqual(pg.brand_for('', ''), 'other')

    def test_every_brand_ends_with_plugging_in(self):
        self.assertEqual({k for k, *_ in pg.BRANDS}, {'samsung', 'pixel', 'xiaomi', 'oneplus', 'handheld', 'other'})
        self.assertTrue(all(steps[-1] == pg.PLUG for _, _, steps, _ in pg.BRANDS))

    def test_driver_advice_on_windows_and_udev_on_linux(self):   # Device Manager and Google's driver mean nothing on Linux; udev rules mean nothing on Windows
        win, lin, mac = (dict(pg.trouble(s)) for s in ('win32', 'linux', 'darwin'))
        self.assertTrue(any('Device Manager' in t for t in win)); self.assertFalse(any('udev' in x for x in win.values()))
        self.assertFalse(any('Device Manager' in t for t in lin)); self.assertTrue(any(pg.UDEV_RULE.replace('"', '&quot;') in x for x in lin.values()))
        self.assertFalse(any('Device Manager' in t for t in mac) or any('udev' in x for x in mac.values()))
        self.assertIn('Google', pg.brand_note('pixel', 'win32')); self.assertNotIn('Google', pg.brand_note('pixel', 'linux'))
        self.assertIn('If it doesn', pg.brand_note('pixel', 'linux')); self.assertEqual(pg.brand_note('pixel', 'darwin'), '')
        self.assertIn('tap <b>Install</b>', pg.brand_note('xiaomi', 'linux'))   # a maker's own catch stays everywhere

    def test_battery_and_space_from_one_shell_call(self):   # a OnePlus Pad 3 on a Mac's USB port, as it printed them
        text = ('  AC powered: false\n  USB powered: true\n  Wireless powered: false\n  Dock powered: false\n  status: 4\n  level: 80\n'
                '/dev/fuse      233949316 153851140  79967104  66% /storage/emulated\n')
        self.assertEqual(pg.parse_status(text), {'battery': 80, 'charging': False, 'free': 79967104 * 1024})
        self.assertEqual(pg.parse_status(''), {})

    def test_warning_below_20_percent(self):
        self.assertEqual(pg.battery_warning({'battery': 20}), '')
        self.assertIn('wall charger', pg.battery_warning({'battery': 12, 'charging': False}))
        self.assertIn("it's charging", pg.battery_warning({'battery': 15, 'charging': True}))


class GameDataTests(unittest.TestCase):
    def test_phone_storage_is_the_default_and_adds_nothing(self):
        for saved in (None, '', 'phone', 'junk'):
            for extra in ([], ['--push-only'], ['--game-apk'], ['--no-push']): self.assertEqual(gd.flags(saved, extra), [])

    def test_sd_card_for_a_library_install_is_romfs_dir_sd(self):
        self.assertEqual(gd.flags('sd', []), ['--romfs-dir', 'sd']); self.assertEqual(gd.flags('sd', ['--push-only']), ['--romfs-dir', 'sd'])

    def test_sd_card_for_a_game_app_keeps_the_data_beside_the_apk(self):
        self.assertEqual(gd.flags('sd', ['--game-apk']), ['--romfs-beside'])
        self.assertEqual(gd.flags('sd', ['--game-apk', '--xapk']), [])   # the script refuses --romfs-beside with --xapk: its data is the OBB file

    def test_runs_that_copy_no_game_take_no_flag(self):   # the script refuses --romfs-dir with each of these
        for x in ('--no-push', '--update-shaders', '--apk-only', '--migrate'): self.assertEqual(gd.flags('sd', [x]), [])

    def test_sd_card_is_offered_for_exactly_one_card(self):
        self.assertFalse(gd.offered([])); self.assertTrue(gd.offered(['6FB7-F185'])); self.assertFalse(gd.offered(['6FB7-F185', '1234-ABCD']))
        self.assertIn('no SD card', gd.no_card_note([])); self.assertIn('2 SD cards', gd.no_card_note(['a', 'b'])); self.assertEqual(gd.no_card_note(['a']), '')

    def test_an_install_with_the_card_out_is_stopped_in_words(self):
        self.assertIn('no SD card', gd.problem('sd', ['--push-only'], [])); self.assertIn('2 SD cards', gd.problem('sd', [], ['a', 'b']))
        self.assertEqual(gd.problem('sd', [], ['a']), ''); self.assertEqual(gd.problem('phone', [], []), '')
        self.assertEqual(gd.problem('sd', ['--game-apk'], []), ''); self.assertEqual(gd.problem('sd', ['--no-push'], []), '')   # no phone is asked

    def test_the_words_say_what_it_means(self):
        for w in ('on the SD card', 'rest of the game stays on the phone', "won't start without the card", 'copies the data back', 'delete'): self.assertIn(w, gd.MEANING)
        self.assertIn("won't start without the card", gd.MEANING_APP)


if __name__ == '__main__':
    unittest.main()
