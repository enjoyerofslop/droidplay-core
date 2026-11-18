"""Every game's host built again from the current checkout before the app is packed (scripts/build_android_from_nsp.py refresh_hosts: --apk-only, --push-only and a
build; the desktop app's Update games): what the library records of each host's profile and build, which profile an older library's host is built from again (its
build folder's header, else the main it rewrote), which hosts are skipped as current, a host that doesn't build kept with a [RED] line, and the order of the stages.
No NDK, phone or game files needed: the host build is a stand-in."""
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
# test_android_cli's copy of the script, not a second one: loading it again replaces functions of build_ipa_from_nsp (shared through sys.modules) with ones that
# read this copy's data_dir, which test_android_cli's own patches then miss
sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_android_cli import ba   # noqa: E402
sys.path.insert(0, str(REPO / 'dm/games'))
import game as gamemod   # noqa: E402  # pyright: ignore[reportMissingImports]  (dm/games, put on the path above)

GID = '010057D006492000'   # OCTOPATH TRAVELER: its profile registers 1.0.5; 1.0.3 is packed from a derived profile
REGISTERED, UNREGISTERED = '67dacc049ceee8585b1583336c565b43', 'd2d9a400f18d374980b0f8951e3b0db9'
STAMP = 'f73b0bbb6abc (v0.8.13-7-gf73b0bbb6)'


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, self.tmp)
        self.said = []
        for cm in (mock.patch.object(ba.bi, 'PROG', mock.Mock()), mock.patch.object(ba, 'say', self.said.append), mock.patch.object(ba, 'data_dir', lambda: self.tmp / 'data'),
                   mock.patch.object(ba, 'host_stamp', lambda: STAMP), mock.patch.object(ba, 'core_version', lambda: '0.8.13-7-gf73b0bbb6'),
                   mock.patch.object(ba, '_FRESH', set())):
            cm.__enter__(); self.addCleanup(cm.__exit__, None, None, None)

    def built(self):   # a host build's outputs: (the host, the loader, the self test folder)
        b = self.tmp / 'build'; b.mkdir(exist_ok=True)
        so = b / f'libdmhost_{GID}.so'; so.write_bytes(b'old host'); loader = b / 'libnxloader.so'; loader.write_bytes(b'loader')
        st = b / 'st'; st.mkdir(exist_ok=True); (st / 'main.patched.bin').write_bytes(b'x')
        return so, loader, st

    def derived(self, rows=(f'build\tv196608\t1.0.3\tmain={UNREGISTERED}',), upscaler=False):   # a derived profile in a game's build folder, <NSP folder>/<id>_build/profile/<id>
        d = self.tmp / 'nsp' / f'{GID}_build' / 'profile' / GID; d.mkdir(parents=True, exist_ok=True)
        tsv = (REPO / 'dm/games' / GID / 'game.tsv').read_text(encoding='utf-8')
        (d / 'game.tsv').write_text('\n'.join([ln for ln in tsv.splitlines() if not ln.startswith('build\t')] + list(rows)) + '\n', encoding='utf-8')
        (d / 'shader_map.tsv').write_text('vertex\tx\tvertex\t00000000000000aa\trom:/a.bnsh#0\n' +
                                          ('compute\tx\tcompute\t00000000000000bb\trom:/lib/agl_ndsr.pck#3\n' if upscaler else ''), encoding='utf-8')
        return d

    def meta(self, gid=GID):
        return json.loads((self.tmp / 'data/hosts' / gid / 'meta.json').read_text(encoding='utf-8'))

    def legacy(self, gid=GID, **kw):   # a library entry from before the library recorded profiles
        d = self.tmp / 'data/hosts' / gid; d.mkdir(parents=True, exist_ok=True)
        (d / f'libdmhost_{gid}.so').write_bytes(b'old host')
        (d / 'meta.json').write_text(json.dumps(dict({'title_id': gid, 'name': gid, 'version': 'v196608', 'nsp_dir': str(self.tmp / 'nsp'), 'built': '2026-11-10T11:44:22'}, **kw)), encoding='utf-8')


class Records(Base):
    def test_a_repository_profile_is_recorded_with_the_build(self):
        so, loader, st = self.built()
        ba.library_add(GID, so, loader, st, {'name': 'OT'}, gamemod.load(GID))
        m = self.meta()
        self.assertEqual((m['profile'], m['build'], m['core'], m['files_core'], m['chain']), ('repository', STAMP, '0.8.13-7-gf73b0bbb6', '0.8.13-7-gf73b0bbb6', ba.android_chain()))
        self.assertIsInstance(m['chain'], int)
        self.assertFalse((self.tmp / 'data/hosts' / GID / 'profile').exists())

    def test_a_derived_profile_is_kept_beside_the_host(self):   # it lives beside the NSP folder, which the player may delete or unplug
        so, loader, st = self.built(); d = self.derived(upscaler=True)
        ba.library_add(GID, so, loader, st, {'name': 'OT'}, gamemod.Game(d))
        self.assertEqual(self.meta()['profile'], 'derived')
        kept = self.tmp / 'data/hosts' / GID / 'profile' / GID
        self.assertEqual((kept / 'game.tsv').read_text(encoding='utf-8'), (d / 'game.tsv').read_text(encoding='utf-8'))
        self.assertEqual((kept / 'shader_map.tsv').read_text(encoding='utf-8').splitlines(), ['compute\tx\tcompute\t00000000000000bb\trom:/lib/agl_ndsr.pck#3'])   # what game.py's header reads
        self.assertEqual(gamemod.upscaler_keys(gamemod.Game(kept)), gamemod.upscaler_keys(gamemod.Game(d)))

    def test_a_provisional_profile_is_one_with_none_in_the_repository(self):
        d = self.tmp / 'p' / '0100FFFF00000000'; d.mkdir(parents=True); (d / 'game.tsv').write_text('title_id\t0100FFFF00000000\n', encoding='utf-8')
        self.assertEqual(ba.profile_kind(gamemod.Game(d)), 'provisional')
        self.assertEqual(ba.profile_kind(gamemod.load(GID)), 'repository')

    def test_a_host_built_in_this_run_is_current(self):   # a checkout with changes: the game this build made isn't built a second time
        so, loader, st = self.built()
        ba.library_add(GID, so, loader, st, {'name': 'OT'}, gamemod.load(GID))
        with mock.patch.object(ba, 'host_stamp', lambda: STAMP.replace(' (', '+dirty (')), mock.patch.object(ba, 'build_host') as bh:
            self.assertIn('already', ba.refresh_hosts(Path('sdk'), self.tmp / 'logs', {}))
        bh.assert_not_called()


class Profiles(Base):
    def header(self, src):   # the build folder's header of the game's last host build, as game.py writes its first line
        h = self.tmp / 'nsp' / f'{GID}_build/android_host/gen' / GID / 'game_profile.h'; h.parent.mkdir(parents=True, exist_ok=True)
        h.write_text(f'// Generated by dm/games/game.py from {src}/game.tsv; do not edit\n#pragma once\n', encoding='utf-8')

    def fixups(self, bid):   # the main module the last build of v196608 rewrote
        f = self.tmp / 'nsp' / f'{GID}_build' / 'patched_android_v196608' / 'main.fixups.tsv'; f.parent.mkdir(parents=True, exist_ok=True)
        f.write_text(f'# FORMAT\t3\n# BUILD_ID\t{bid}\n', encoding='utf-8')

    def test_game_py_names_its_profile_on_the_headers_first_line(self):   # the format host_profile reads an older library's profiles by
        h = self.tmp / 'h'; h.write_text(gamemod.header(gamemod.load(GID)), encoding='utf-8')
        self.assertEqual(ba.header_source(h), f'dm/games/{GID}')
        d = self.derived(); h.write_text(gamemod.header(gamemod.Game(d)), encoding='utf-8')
        self.assertEqual(Path(ba.header_source(h) or ''), d.resolve())

    def test_recorded_profiles(self):
        self.assertEqual(ba.host_profile(GID, {'profile': 'repository'}), ('repository', None))
        self.assertEqual(ba.host_profile('0100FFFF00000000', {'profile': 'repository'})[0], None)   # a profile gone from dm/games
        self.assertEqual(ba.host_profile(GID, {'profile': 'derived'})[0], None)   # its kept copy gone
        so, loader, st = self.built(); ba.library_add(GID, so, loader, st, {'name': 'OT'}, gamemod.Game(self.derived()))
        self.assertEqual(ba.host_profile(GID, self.meta()), ('derived', self.tmp / 'data/hosts' / GID / 'profile' / GID))

    def test_an_older_librarys_host_takes_the_profile_its_header_names(self):
        d = self.derived(); self.header(d); self.fixups(REGISTERED)   # the header wins over the main
        self.assertEqual(ba.host_profile(GID, {'nsp_dir': str(self.tmp / 'nsp'), 'version': 'v196608'}), ('derived', d))
        self.header(f'dm/games/{GID}')
        self.assertEqual(ba.host_profile(GID, {'nsp_dir': str(self.tmp / 'nsp'), 'version': 'v196608'}), ('repository', None))

    def test_without_a_header_the_profile_that_registers_the_main(self):
        d = self.derived(); meta = {'nsp_dir': str(self.tmp / 'nsp'), 'version': 'v196608'}
        self.fixups(UNREGISTERED); self.assertEqual(ba.host_profile(GID, meta), ('derived', d))
        self.fixups(REGISTERED); self.assertEqual(ba.host_profile(GID, meta), ('repository', None))
        self.header(self.tmp / 'gone'); self.fixups(UNREGISTERED)   # a header naming a folder deleted since: the main decides
        self.assertEqual(ba.host_profile(GID, meta), ('derived', d))

    def test_without_a_build_folder_the_repositorys(self):   # the game's files deleted: what's registered is all there is
        self.assertEqual(ba.host_profile(GID, {'nsp_dir': str(self.tmp / 'gone'), 'version': 'v196608'}), ('repository', None))
        self.assertEqual(ba.host_profile(GID, {'name': 'OT'}), ('repository', None))

    def test_a_game_with_no_registered_profile(self):
        gid = '0100FFFF00000000'; d = self.tmp / 'nsp' / f'{gid}_build' / 'profile' / gid; d.mkdir(parents=True)
        (d / 'game.tsv').write_text(f'title_id\t{gid}\n', encoding='utf-8')
        self.assertEqual(ba.host_profile(gid, {'nsp_dir': str(self.tmp / 'nsp')}), ('provisional', d))
        kind, why = ba.host_profile(gid, {'nsp_dir': str(self.tmp / 'gone')})
        self.assertIsNone(kind); self.assertIn('no profile', why)

    def test_current_means_built_from_this_checkout_as_it_is(self):
        self.assertTrue(ba.host_current({'build': STAMP}, STAMP))
        self.assertFalse(ba.host_current({'build': STAMP}, 'aaaaaaaaaaaa (v0.8.14)'))
        self.assertFalse(ba.host_current({}, STAMP))   # an older library's: never recorded
        dirty = STAMP.replace(' (', '+dirty (')
        self.assertFalse(ba.host_current({'build': dirty}, dirty))   # changes since may be anything
        self.assertFalse(ba.host_current({'build': 'not recorded'}, 'not recorded'))


class Refresh(Base):
    selftest_env: 'dict[str, str]' = {}   # the environment the self test was rewritten in

    def setUp(self):
        super().setUp(); self.builds = []
        def build_host(sdk, gid, work, logs, env):   # the CMake build's stand-in: one host per game, made from the profile game.py would take
            if gid in getattr(self, 'broken', ()): raise ba.BuildError(f'ninja failed (see {logs}/android_host.log)\nld.lld: error: undefined symbol: mtl_link_mode_set')
            self.builds.append((gid, env.get('DM_GAMES_DIR'), env.get('DM_GAMES_FIRST'), work, logs))
            so = work / 'android_host' / f'libdmhost_{gid}.so'; so.parent.mkdir(parents=True, exist_ok=True); so.write_bytes(f'new host {gid}'.encode())
            (work / 'android_host/libnxloader.so').write_bytes(b'new loader')
            return so, work / 'android_host/libnxloader.so', Path('ndk')
        def build_selftest(ndk, work, logs, env):
            self.selftest_env = env; d = work / 'android_selftest/patched'; d.mkdir(parents=True, exist_ok=True); (d / 'main.patched.bin').write_bytes(b'st'); return d
        for cm in (mock.patch.object(ba, 'build_host', build_host), mock.patch.object(ba, 'build_selftest', build_selftest)):
            cm.__enter__(); self.addCleanup(cm.__exit__, None, None, None)

    def test_each_host_built_again_with_its_own_profile(self):
        d = self.derived()
        self.legacy(GID); self.legacy('0100000000010000', version='v393216', nsp_dir=str(self.tmp / 'mario'))   # Super Mario Odyssey: no build folder, registered
        self.legacy('01006BB00C6F0000', build=STAMP, profile='repository')   # built from this checkout already
        f = self.tmp / 'nsp' / f'{GID}_build' / 'patched_android_v196608' / 'main.fixups.tsv'; f.parent.mkdir(parents=True); f.write_text(f'# BUILD_ID\t{UNREGISTERED}\n')
        out = ba.refresh_hosts(Path('sdk'), self.tmp / 'logs', {'DM_GAMES_DIR': '/elsewhere', 'DM_GAMES_FIRST': '1', 'PATH': '/bin'})
        self.assertEqual(out[:2], '2 ')
        self.assertIn('1 built from it already', out)
        b = {gid: (gd, first) for gid, gd, first, *_ in self.builds}
        self.assertEqual(b, {GID: (str(d.parent), '1'), '0100000000010000': (None, None)})   # the repository's profile whatever the console's DM_GAMES_DIR said
        self.assertTrue(all(w == self.tmp / 'data' / ba.HOST_BUILD for *_, w, _ in self.builds))   # one build folder in the data folder, not the game's own
        self.assertEqual({lg for *_, lg in self.builds}, {self.tmp / 'logs/hosts' / GID, self.tmp / 'logs/hosts/0100000000010000'})   # each game's logs apart
        m = self.meta()
        self.assertEqual((m['profile'], m['build'], m['core'], m['version'], m['nsp_dir']), ('derived', STAMP, '0.8.13-7-gf73b0bbb6', 'v196608', str(self.tmp / 'nsp')))
        self.assertNotEqual(m['built'], '2026-11-10T11:44:22')
        self.assertEqual((self.tmp / 'data/hosts' / GID / f'libdmhost_{GID}.so').read_bytes(), f'new host {GID}'.encode())
        self.assertTrue((self.tmp / 'data/hosts' / GID / 'profile' / GID / 'game.tsv').is_file())   # found once, kept for the next time
        self.assertEqual(self.meta('0100000000010000')['profile'], 'repository')
        self.assertEqual((self.tmp / 'data/hosts/01006BB00C6F0000/libdmhost_01006BB00C6F0000.so').read_bytes(), b'old host')
        self.assertEqual((self.tmp / 'data/loader/libnxloader.so').read_bytes(), b'new loader')
        self.assertTrue((self.tmp / 'data/selftest/main.patched.bin').is_file())
        self.assertEqual(self.selftest_env.get('DM_TARGET'), 'android')   # rewritten for Android's x18 thread slots, whatever --apk-only's environment
        self.builds.clear()
        self.assertIn('already', ba.refresh_hosts(Path('sdk'), self.tmp / 'logs', {}))   # a second update builds nothing
        self.assertEqual(self.builds, [])

    def test_a_host_that_does_not_build_is_kept_and_said(self):   # the fps_max 120 games' link failure on main, say: the others still update
        self.legacy(GID); self.legacy('0100000000010000'); self.broken = {'0100000000010000'}
        out = ba.refresh_hosts(Path('sdk'), self.tmp / 'logs', {})
        self.assertIn('1 kept as they were (0100000000010000)', out)
        red = [s for s in self.said if s.startswith('[RED] 0100000000010000')]
        self.assertEqual(len(red), 1); self.assertIn('ninja failed', red[0]); self.assertIn('build and install 0100000000010000 again', red[0])
        self.assertEqual((self.tmp / 'data/hosts/0100000000010000/libdmhost_0100000000010000.so').read_bytes(), b'old host')
        self.assertNotIn('build', self.meta('0100000000010000'))

    def test_none_built_stops_an_update_of_the_app(self):   # the NDK gone, say: the app would go to the phone with every old host, as if updated
        self.legacy(GID); self.broken = {GID}
        with self.assertRaisesRegex(ba.BuildError, 'no game host could be built from this checkout'): ba.refresh_hosts(Path('sdk'), self.tmp / 'logs', {}, strict=True)
        self.assertIn('kept as they were', ba.refresh_hosts(Path('sdk'), self.tmp / 'logs', {}))   # a build of another game goes on

    def test_no_library_no_update(self):
        with self.assertRaisesRegex(ba.BuildError, 'build a game first'): ba.refresh_hosts(Path('sdk'), self.tmp / 'logs', {})


class Stages(Base):
    def run_apk_only(self, *extra):
        calls = []
        with mock.patch.object(ba, 'sdk_dir', lambda a: Path('sdk')), mock.patch.object(ba, 'refresh_hosts', lambda *a, **k: calls.append(('refresh', k.get('strict', a[3] if len(a) > 3 else False))) or 'ok'), \
             mock.patch.object(ba, 'make_apk', lambda *a: calls.append(('apk',)) or Path('app.apk')), mock.patch.object(ba, 'install', lambda s, apk, logs: calls.append(('install', s))), \
             mock.patch.object(ba, 'phone', mock.MagicMock()), mock.patch.dict(os.environ, NXNATIVE_JOURNAL='0'):
            ba.main(['--apk-only', '--serial', 'ZY22K7GVGP', '--progress', 'off', *extra])
        return calls, [c.args[0] for c in ba.bi.PROG.stage.call_args_list]

    def test_update_the_app_builds_the_hosts_first(self):
        calls, stages = self.run_apk_only()
        self.assertEqual(calls, [('refresh', True), ('apk',), ('install', 'ZY22K7GVGP')])
        self.assertEqual(stages, ['game hosts', 'APK', 'install'])

    def test_keep_hosts_packs_them_as_they_are(self):
        calls, stages = self.run_apk_only('--keep-hosts')
        self.assertEqual(calls, [('apk',), ('install', 'ZY22K7GVGP')])
        self.assertEqual(stages, ['APK', 'install'])

    def test_a_game_app_carries_the_apps_version(self):   # its host logs it beside its own (front_android.c), as the launcher app's does
        stage = self.tmp / 'game'; (stage / 'ExeFS').mkdir(parents=True); (stage / 'RomFS.img').write_bytes(b'r' * 4096)
        so = self.tmp / f'libdmhost_{GID}.so'; so.write_bytes(b'host'); ran = []
        g = mock.Mock(id=GID)
        with mock.patch.object(ba.bi, 'run', lambda cmd, log, env=None: ran.append([str(c) for c in cmd])), mock.patch.object(ba.bi.rfs, 'source', lambda d: d / 'RomFS.img'), \
             mock.patch.object(ba.bi.rfs, 'is_image', lambda p: True):
            ba.make_game_app(Path('sdk'), g, stage, self.tmp / 'code', None, None, None, 'DM_LANG=en-US\n', so, {'name': 'OT'}, self.tmp, self.tmp, {}, self.tmp / 'out', False)
        cmd = ran[-1]
        self.assertEqual(cmd[cmd.index('--version-name') + 1], '0.8.13-7-gf73b0bbb6')


if __name__ == '__main__':
    unittest.main()
