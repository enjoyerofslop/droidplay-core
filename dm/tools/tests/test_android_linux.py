"""DroidPlay for Linux's AppImage and the desktop app's Linux side, without Qt, FUSE or the AppImage itself: the first start of a version unpacking it into
~/.local/share/DroidPlay/<version>/ and later starts reusing it, older versions removed but the previous one and any a running DroidPlay holds
(dm/android/linux/unpack.py); Stop ending a build's whole process group (dm/android/desktop/procs.py); the data folder under XDG_DATA_HOME
(dm/android/tools/data_home.py); the DejaVu fonts the AppImage carries, where the build looks last; Add to the applications menu (dm/android/desktop/menu_entry.py);
the licences both packages gather (dm/android/licenses/collect.py). Linux and macOS (flock, process groups); not Windows."""
import importlib.util
import io
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile
import time
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, REPO / path)
    assert spec and spec.loader
    m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m); return m


POSIX = os.name != 'nt'
if POSIX:
    unpack = load('unpack', 'dm/android/linux/unpack.py')
procs = load('procs', 'dm/android/desktop/procs.py')
data_home = load('data_home', 'dm/android/tools/data_home.py')
menu_entry = load('menu_entry', 'dm/android/desktop/menu_entry.py')
collect = load('collect', 'dm/android/licenses/collect.py')


def payload(root, version):   # a stand-in for the AppImage's payload: core with its VERSION, a runtime with the python3 link PBS has
    p = Path(root) / f'payload-{version}'
    (p / 'core' / 'scripts').mkdir(parents=True); (p / 'core' / 'VERSION').write_text(version + '\n')
    (p / 'core' / 'scripts' / 'nxnative_android.py').write_text('# the app\n')
    (p / 'runtime' / 'bin').mkdir(parents=True); (p / 'runtime' / 'bin' / 'python3.14').write_bytes(b'\x7fELF')
    (p / 'runtime' / 'bin' / 'python3').symlink_to('python3.14')
    ro = p / 'core' / 'read-only.txt'; ro.write_text('x'); ro.chmod(0o444)   # squashfs files the AppImage holds read-only
    return p


@unittest.skipUnless(POSIX, 'the AppImage is Linux only; flock')
class Unpack(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, self.tmp, True); self.root = self.tmp / 'share' / 'DroidPlay'
        self.held = []; self.addCleanup(lambda: [os.close(fd) for fd in self.held])

    def start(self, version):   # one start of that version's AppImage, as unpack.py's main does before it hands over to DroidPlay
        dest, fd = unpack.prepare(payload(self.tmp, version) if not (self.tmp / f'payload-{version}').exists() else self.tmp / f'payload-{version}', self.root)
        self.held.append(fd); return dest

    def test_first_start_unpacks_the_version(self):
        dest = self.start('0.9.0')
        self.assertEqual(dest, self.root / '0.9.0'); self.assertTrue((dest / '.complete').is_file())
        self.assertEqual(os.readlink(dest / 'runtime' / 'bin' / 'python3'), 'python3.14')   # the link stays a link: python3 is how AppRun finds Python
        self.assertTrue(os.stat(dest / 'core' / 'read-only.txt').st_mode & 0o200)   # writable, so the version can be removed later
        self.assertFalse((self.root / '0.9.0.partial').exists())

    def test_a_later_start_reuses_it(self):   # the tree stays where it is: the host's CMake build folder made from it stays valid
        dest = self.start('0.9.0'); (dest / 'core' / 'left-here').write_text('x'); before = os.stat(dest / '.complete').st_mtime_ns
        self.assertEqual(self.start('0.9.0'), dest)
        self.assertTrue((dest / 'core' / 'left-here').is_file()); self.assertEqual(os.stat(dest / '.complete').st_mtime_ns, before)

    def test_the_previous_version_stays_and_older_ones_go(self):
        for v in ('0.9.0', '0.9.1', '0.9.2'):
            self.start(v); [os.close(fd) for fd in self.held]; self.held.clear(); time.sleep(0.02)   # that DroidPlay closed again
        self.assertEqual(sorted(p.name for p in self.root.iterdir() if not p.name.startswith('.')), ['0.9.1', '0.9.2'])

    def test_a_version_in_use_is_not_removed(self):   # a DroidPlay still open on 0.9.0 (or a build it started) holds its version's lock
        self.start('0.9.0'); time.sleep(0.02); self.start('0.9.1'); time.sleep(0.02); self.start('0.9.2')
        self.assertTrue((self.root / '0.9.0' / '.complete').is_file())
        [os.close(fd) for fd in self.held]; self.held.clear(); self.start('0.9.2')   # once it has closed, the next start removes it
        self.assertFalse((self.root / '0.9.0').exists()); self.assertTrue((self.root / '0.9.1').exists())

    def test_a_copy_cut_short_is_neither_used_nor_kept(self):
        (self.root / '0.9.0.partial' / 'core').mkdir(parents=True)   # the computer went off halfway through a first start
        (self.root / '0.9.0').mkdir()   # a folder of the version's name without .complete
        dest = self.start('0.9.0')
        self.assertTrue((dest / '.complete').is_file()); self.assertFalse((self.root / '0.9.0.partial').exists())

    def test_a_full_disk_says_how_much_is_needed(self):
        p = payload(self.tmp, '0.9.0')
        with mock.patch.object(unpack.shutil, 'disk_usage', return_value=mock.Mock(free=10 << 20)), self.assertRaises(unpack.UnpackError) as e:
            unpack.prepare(p, self.root)
        self.assertIn('free', str(e.exception)); self.assertFalse((self.root / '0.9.0').exists())

    def test_a_version_that_cannot_name_a_folder_is_refused(self):
        p = payload(self.tmp, 'x'); (p / 'core' / 'VERSION').write_text('../../etc\n')
        with self.assertRaises(unpack.UnpackError): unpack.prepare(p, self.root)

    def test_where_versions_go(self):
        self.assertEqual(unpack.versions_dir({'XDG_DATA_HOME': '/data'}), Path('/data/DroidPlay'))
        self.assertEqual(unpack.versions_dir({'XDG_DATA_HOME': 'relative'}), Path.home() / '.local/share/DroidPlay')   # the XDG spec: ignored
        self.assertEqual(unpack.versions_dir({}), Path.home() / '.local/share/DroidPlay')


@unittest.skipUnless(POSIX, 'Windows ends a build with taskkill /T')
class Stop(unittest.TestCase):   # Stop on a build: the packing script and the tools it started (cmake, ninja, dotnet, pool workers), not the script alone
    def spawn(self, new_session, secs=60):   # a stand-in build: a shell with two children of its own
        p = subprocess.Popen(['sh', '-c', f'sleep {secs} & sleep {secs} & wait'], start_new_session=new_session)
        self.addCleanup(lambda: p.poll() is None and p.kill()); time.sleep(0.3)
        return p

    def children(self, pid):
        r = subprocess.run(['ps', '-A', '-o', 'pid=,ppid=,pgid='], capture_output=True, text=True).stdout.split()
        rows = list(zip(*[iter(map(int, r))] * 3))
        return [p for p, pp, g in rows if g == pid and p != pid]

    def test_a_build_in_its_own_session_ends_with_its_tools(self):   # as the desktop app starts builds (CreateNewSession)
        p = self.spawn(True); kids = self.children(p.pid)
        self.assertEqual(len(kids), 2)
        procs.stop_tree(p.pid); p.wait(10)
        time.sleep(0.2)
        for k in kids:
            with self.assertRaises(ProcessLookupError): os.kill(k, 0)

    def test_a_process_in_our_own_group_is_ended_alone(self):   # never the group that holds this process (an older PySide6 starts builds in it)
        p = self.spawn(False, 3)   # its sleeps outlive it briefly
        procs.stop_tree(p.pid, force=False)
        self.assertEqual(p.wait(10), -signal.SIGTERM)
        os.kill(os.getpid(), 0)   # still here

    def test_one_that_has_gone_is_no_error(self):
        p = subprocess.Popen(['true']); p.wait()
        procs.stop_tree(p.pid); procs.stop_tree(0)


class DataHome(unittest.TestCase):   # the folder of hosts, builds journal, device locks and tools: one for the packing script, the locks and the bench
    def test_linux_follows_xdg_data_home(self):
        with mock.patch.object(data_home.os, 'name', 'posix'), mock.patch.object(data_home.sys, 'platform', 'linux'):
            self.assertEqual(data_home.data_dir({'XDG_DATA_HOME': '/xdg'}), Path('/xdg/nxnative/android'))
            self.assertEqual(data_home.data_dir({'XDG_DATA_HOME': 'rel'}), Path.home() / '.local/share/nxnative/android')
            self.assertEqual(data_home.data_dir({}), Path.home() / '.local/share/nxnative/android')

    def test_a_mac_keeps_its_folder(self):
        with mock.patch.object(data_home.os, 'name', 'posix'), mock.patch.object(data_home.sys, 'platform', 'darwin'):
            self.assertEqual(data_home.data_dir({'XDG_DATA_HOME': '/xdg'}), Path.home() / '.local/share/nxnative/android')

    def test_the_locks_and_the_bench_use_it(self):
        for f in ('dm/android/tools/device_lock.py', 'scripts/bench_android.py', 'scripts/build_android_from_nsp.py'):
            self.assertIn('data_home.data_dir()', (REPO / f).read_text(encoding='utf-8'), f)


class Fonts(unittest.TestCase):   # DejaVu Sans, which the AppImage carries in dm/android/fonts: found where the system has neither DejaVu nor Liberation
    def test_the_bundled_fonts_come_last(self):
        text = (REPO / 'scripts/build_android_from_nsp.py').read_text(encoding='utf-8')
        for name, font in (('LETTER_FONTS', 'DejaVuSans-Bold.ttf'), ('STANDARD_FONTS', 'DejaVuSans.ttf')):
            listed = text.split(f'\n{name} = [', 1)[1].split(']\n', 1)[0]
            self.assertTrue(listed.rstrip().endswith(f"str(BUNDLED_FONTS / '{font}')"), name)
            self.assertIn('dejavu-sans-fonts', listed)   # Fedora's path

    def test_stage_puts_them_there(self):
        stage = (REPO / 'dm/android/linux/stage.sh').read_text(encoding='utf-8')
        self.assertIn('"$pay/core/dm/android/fonts/"', stage)


class MenuEntry(unittest.TestCase):   # Settings' Add to the applications menu, from the AppImage: the entry starts the AppImage file, wherever it is and however named
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, self.tmp, True)
        self.img = self.tmp / 'My Apps' / 'DroidPlay-0.9.0-x86_64.AppImage'; self.img.parent.mkdir(); self.img.write_bytes(b'AI'); self.img.chmod(0o755)
        self.env = {'HOME': str(self.tmp / 'home'), 'XDG_DATA_HOME': str(self.tmp / 'share'), 'APPIMAGE': str(self.img)}

    def icon(self, p): Path(p).write_bytes(b'\x89PNG'); return True

    def test_exec_quoting(self):   # the Desktop Entry spec's: quoted, ", `, $ and \\ escaped, then every backslash doubled again, and % doubled
        self.assertEqual(menu_entry.exec_arg('/home/a b/DroidPlay.AppImage'), '"/home/a b/DroidPlay.AppImage"')
        self.assertEqual(menu_entry.exec_arg('/x/$HOME"`\\100%'), r'"/x/\\$HOME\\"\\`\\\\100%%"')
        with self.assertRaises(ValueError): menu_entry.exec_arg('/x/a\nb')

    def test_add_writes_the_entry_and_the_icon_and_remove_takes_them(self):
        self.assertEqual(menu_entry.state(self.env), 'none')
        entry = menu_entry.add(menu_entry.appimage(self.env), self.icon, self.env)
        self.assertEqual(entry, self.tmp / 'share/applications/io.github.enjoyerofslop.droidplay.desktop')
        text = entry.read_text()
        self.assertIn(f'Exec="{self.img}"\n', text); self.assertIn(f'TryExec={self.img}\n', text)   # TryExec: the menu drops it once the AppImage is gone
        self.assertIn('Icon=io.github.enjoyerofslop.droidplay\n', text); self.assertIn('StartupWMClass=DroidPlay\n', text); self.assertNotIn('Exec=DroidPlay\n', text)
        self.assertTrue((self.tmp / 'share/icons/hicolor/256x256/apps/io.github.enjoyerofslop.droidplay.png').is_file())
        self.assertEqual([p.name for p in entry.parent.iterdir()], [entry.name])   # no .part file left
        self.assertEqual(menu_entry.state(self.env), 'this')
        self.assertTrue(menu_entry.remove(self.env)); self.assertFalse(entry.exists())
        self.assertFalse((self.tmp / 'share/icons/hicolor/256x256/apps/io.github.enjoyerofslop.droidplay.png').exists())
        self.assertEqual(menu_entry.state(self.env), 'none')

    def test_an_entry_for_another_copy_is_noticed(self):   # the AppImage moved or renamed, or another download added it
        menu_entry.add(menu_entry.appimage(self.env), self.icon, self.env)
        moved = self.tmp / 'DroidPlay.AppImage'; self.img.rename(moved)
        self.assertEqual(menu_entry.state(dict(self.env, APPIMAGE=str(moved))), 'other')
        self.assertEqual(menu_entry.current(self.env)[1], self.img)

    def test_without_xdg_data_home_it_goes_under_the_home_folder(self):
        env = {k: v for k, v in self.env.items() if k != 'XDG_DATA_HOME'}
        self.assertEqual(menu_entry.entry_path(env), self.tmp / 'home/.local/share/applications/io.github.enjoyerofslop.droidplay.desktop')
        self.assertEqual(menu_entry.entry_path(dict(env, XDG_DATA_HOME='relative')), menu_entry.entry_path(env))   # a relative one is ignored, as the spec says

    def test_someone_elses_entry_of_that_name_stays(self):   # a distribution's package, say
        p = menu_entry.entry_path(self.env); p.parent.mkdir(parents=True); p.write_text('[Desktop Entry]\nExec=droidplay\n')
        self.assertEqual(menu_entry.state(self.env), 'none'); self.assertFalse(menu_entry.remove(self.env)); self.assertTrue(p.exists())

    def test_no_appimage_no_entry(self):   # from source, or APPIMAGE naming a file that's gone
        self.assertIsNone(menu_entry.appimage({})); self.assertIsNone(menu_entry.appimage({'APPIMAGE': str(self.tmp / 'gone.AppImage')}))


class Licences(unittest.TestCase):   # what both stage scripts gather (collect.py): each module's licence files and its line in NOTICES.md
    def test_modules_and_notices(self):
        tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, tmp, True)
        a = tmp / 'site/lz4-4.4.4.dist-info'; (a / 'licenses').mkdir(parents=True); (a / 'METADATA').write_text('Name: lz4\nVersion: 4.4.4\nLicense-Expression: BSD-3-Clause\n')
        (a / 'licenses/LICENSE').write_text('bsd')
        b = tmp / 'site/zbic-1.0.dist-info'; b.mkdir(); (b / 'METADATA').write_text('Name: zbic\nVersion: 1.0\nLicense: MIT\n'); (b / 'LICENSE.txt').write_text('mit')
        out = io.StringIO()
        with mock.patch('sys.stdout', out): collect.modules(tmp / 'site', tmp / 'lic')
        self.assertEqual(out.getvalue().splitlines(), ['- lz4 4.4.4 (BSD-3-Clause)', '- zbic 1.0 (MIT)'])
        self.assertEqual((tmp / 'lic/lz4/LICENSE').read_text(), 'bsd'); self.assertEqual((tmp / 'lic/zbic/LICENSE.txt').read_text(), 'mit')
        (tmp / 'T.md').write_text('DroidPlay @VERSION@\n@MODULES@\n')
        with mock.patch.dict(os.environ, {'VERSION': '0.9.0', 'MODULES': out.getvalue()}): collect.notices(tmp / 'T.md', tmp / 'N.md')
        self.assertEqual((tmp / 'N.md').read_text(), 'DroidPlay 0.9.0\n- lz4 4.4.4 (BSD-3-Clause)\n- zbic 1.0 (MIT)\n')

    def test_both_packages_notice_qt_with_the_same_texts(self):   # Setup shipped Qt without its LGPL notice; the texts are shared (dm/android/licenses)
        for t in ('dm/android/linux/NOTICES.md', 'dm/android/windows/NOTICES.md'):
            text = (REPO / t).read_text()
            for f in ('LGPL-3.0.txt', 'GPL-3.0.txt', 'GPL-2.0.txt', 'Apache-2.0.txt'): self.assertIn(f, text, t); self.assertTrue((REPO / 'dm/android/licenses' / f).is_file())


if __name__ == '__main__':
    unittest.main()
