"""scripts/android_oodle.py: the batching of the game's Oodle decodes into phone jobs. Rounds answers uepak.py's decode(compressed, raw length) from what the phone decoded so far, records
the rest, and runs again until nothing new is asked: a stream made from an earlier round's zeros is never sent (it does not start as Oodle's streams do), so a two-layer container
(blocks, then the shader groups inside them) settles in two rounds. The job file and its output keep hle_oodle.c's formats. Oodle stands in here as a header byte and zlib"""
import os
import shutil
import struct
import sys
import tempfile
import unittest
import zlib
from unittest import mock
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'scripts'))
import android_oodle as ao   # noqa: E402  # pyright: ignore[reportMissingImports]  (scripts/, put on the path here)


def stream(raw):   # an "Oodle" stream: a header byte whose low six bits are 0x0C, then zlib
    return b'\x8c' + zlib.compress(raw)


class FakePhone:
    def __init__(self): self.jobs = []
    def __call__(self, records):
        self.jobs.append(len(records))
        out = []
        for c, n in records:
            try: d = zlib.decompress(c[1:]) if ao.oodle_head(c) else None
            except zlib.error: d = None   # the job's record fails (decoded length 0), as hle_oodle.c reports one
            out.append(d if d is not None and len(d) == n else None)
        return out


class RoundsTest(unittest.TestCase):
    def test_two_layers_settle_in_two_rounds(self):
        inner = [b'shader %d ' % i * 50 for i in range(3)]
        outer = [stream(stream(x)) for x in inner]   # a block holding a compressed group
        sizes = [len(stream(x)) for x in inner]
        r, phone = ao.Rounds(), FakePhone()
        def fn():   # as io_archive: each block decoded, then the group inside it
            return [r(r(o, s), len(x)) for o, s, x in zip(outer, sizes, inner)]
        res, rounds = r.run(fn, phone)
        self.assertEqual(res, inner)
        self.assertEqual(rounds, 2)
        self.assertEqual(phone.jobs, [3, 3])   # the blocks, then the groups: no zeros sent

    def test_a_damaged_file_still_raises(self):
        r = ao.Rounds()
        def fn(): raise ValueError('damaged')
        with self.assertRaises(ValueError): r.run(fn, FakePhone())

    def test_parse_errors_on_zeros_are_rounds(self):   # uepak's checks fail on a round's zeros; the next round has the real bytes
        r, phone = ao.Rounds(), FakePhone()
        head = struct.pack('<I', 1) + b'table'
        def fn():
            d = r(stream(head), len(head))
            if struct.unpack_from('<I', d)[0] != 1: raise ValueError('not version 1')
            return d
        res, rounds = r.run(fn, phone)
        self.assertEqual((res, rounds), (head, 1))

    def test_a_failed_stream_is_named(self):
        r = ao.Rounds()
        with self.assertRaises(ValueError) as e: r.run(lambda: r(b'\x8c' + b'not zlib', 10), FakePhone())
        self.assertIn('failed on a', str(e.exception))


class FormatTest(unittest.TestCase):
    def test_job_and_output(self):
        recs = [(stream(b'abc'), 3), (stream(b'x' * 100), 100)]
        b = ao.job_bytes(recs)
        self.assertEqual(b[:8], b'DMOJ' + struct.pack('<I', 2))
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'job.out'
            p.write_bytes(b'DMOR' + struct.pack('<I', 2) + struct.pack('<I', 3) + b'abc' + struct.pack('<I', 0))
            self.assertEqual(ao.read_out(p, 2), [b'abc', None])
            with self.assertRaises(ValueError): ao.read_out(p, 3)

    def test_a_cut_output_is_named(self):   # issue #111: the phone's file ended inside a record, and struct.error was all the build said
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / 'job.out'
            p.write_bytes(b'DMOR' + struct.pack('<I', 2) + struct.pack('<I', 3) + b'abc' + struct.pack('<I', 100) + b'x' * 40)
            with self.assertRaises(ValueError) as e: ao.read_out(p, 2)
            self.assertIn('cut short', str(e.exception)); self.assertIn('record 2 of 2', str(e.exception))
            p.write_bytes(b'DMOR' + struct.pack('<I', 2) + struct.pack('<I', 3) + b'abc' + b'\0\0')   # ends inside the second record's length word
            with self.assertRaises(ValueError) as e: ao.read_out(p, 2)
            self.assertIn('cut short', str(e.exception))

    def test_sizes(self):
        self.assertEqual(ao.out_size([(b'', 3), (b'', 100)]), 8 + 4 + 3 + 4 + 100)
        self.assertEqual(ao.free_bytes('Filesystem 1K-blocks Used Available Use% Mounted on\n/dev/fuse 239313768 220000000 19313768 92% /storage/emulated\n'), 19313768 * 1024)
        self.assertIsNone(ao.free_bytes(''))
        self.assertIsNone(ao.free_bytes('df: /x: No such file or directory'))

    def test_oodle_head(self):
        for b in (0x0C, 0x4C, 0x8C, 0xCC): self.assertTrue(ao.oodle_head(bytes([b, 0x0A])))
        for b in (0x00, 0x1C, 0x8D, 0x78): self.assertFalse(ao.oodle_head(bytes([b, 0x0A])))


class PhoneTest(unittest.TestCase):
    """Phone.job against a stand-in for adb: what it runs on the phone, and what it does with the answers"""
    def run_job(self, df_free_k, pulled, size_word=None, log='DM_OODLE_JOB: 1 records decoded', logged=None, host=1 << 40, pull_error=None, env=None):
        """log: what the grep of the game's log.txt finds; pulled[0] None: the game left no output. logged, a list: gets the job's log file as the build wrote it.
        env, a list: gets the env.txt the job pushed. host: bytes free on this computer's drive; pull_error: what adb pull fails with"""
        calls = []
        def adb(serial, *args, check=True):
            calls.append(args)
            cmd = args[1] if args[0] == 'shell' else ''
            if cmd.startswith('df -k'): return f'Filesystem 1K-blocks Used Available Use% Mounted on\n/dev/fuse 1000000 0 {df_free_k} 0% /storage/emulated\n'
            if 'grep -E' in cmd: return log
            if cmd.startswith('stat -c'): return '' if pulled[0] is None else str(size_word if size_word is not None else len(pulled[0]))
            if cmd.startswith('ls -l'): return '-rw-rw---- 1 u0_a160 ext_data_rw 0 log.txt'
            if args[0] == 'logcat': return 'I/Zygote  ( 1279): Process 10751 exited cleanly (0)'
            if args[0] == 'pull' and pull_error: raise RuntimeError(pull_error)
            if args[0] == 'pull': Path(args[2]).write_bytes(pulled.pop(0) if len(pulled) > 1 else pulled[0])
            return ''
        class Clock:   # time for android_oodle: each sleep moves it on, so the 20 s before a gone game counts take no real time
            t = 1000.0
            def time(self): return self.t
            def sleep(self, s): self.t += s
        with tempfile.TemporaryDirectory() as d, mock.patch.object(ao, 'time', Clock()), mock.patch.object(ao, 'host_free', lambda f: host):
            ph = ao.Phone(adb, 'SERIAL', '0100606026F12000', '/sdcard/x', 'org.nxnative.android', d, d, lambda s: None)
            try: r = ph.job([(stream(b'abc'), 3)])
            except ValueError as e: r = e
            if logged is not None: logged.append((Path(d) / 'oodle_phone_1.log').read_text())
            if env is not None: env.append((Path(d) / 'tmp' / 'oodle_env.txt').read_text())
            return r, calls

    def test_too_little_space_stops_before_the_job(self):
        r, calls = self.run_job(1024, [b''])
        self.assertIsInstance(r, ValueError); self.assertIn('free up space', str(r))
        self.assertFalse([c for c in calls if c[0] in ('push', 'pull')])

    def test_a_full_drive_here_stops_before_the_copy(self):   # issue #436: the computer's drive, not the phone's, was full
        good = b'DMOR' + struct.pack('<I', 1) + struct.pack('<I', 3) + b'abc'
        r, calls = self.run_job(10 << 20, [good], host=1 << 20)
        self.assertIsInstance(r, ValueError); self.assertIn("this computer's drive", str(r)); self.assertIn('decoded so far is kept', str(r))
        self.assertFalse([c for c in calls if c[0] == 'pull'])

    def test_a_copy_that_cannot_be_written_here_is_named(self):   # issue #436: adb pull's "cannot write … Input/output error" on Windows
        good = b'DMOR' + struct.pack('<I', 1) + struct.pack('<I', 3) + b'abc'
        r, _ = self.run_job(10 << 20, [good], pull_error="adb pull failed (exit status 1): adb: error: cannot write 'C:\\b\\oodle_phone.job.out': Input/output error")
        self.assertIsInstance(r, ValueError); self.assertIn('this computer could not write', str(r)); self.assertIn('Input/output error', str(r))
        with self.assertRaises(RuntimeError) as e: self.run_job(10 << 20, [good], pull_error='adb lost the phone: device offline')
        self.assertNotIsInstance(e.exception, ValueError)   # a lost phone goes on as it is, for the reconnect

    def test_a_short_copy_is_made_again(self):
        good = b'DMOR' + struct.pack('<I', 1) + struct.pack('<I', 3) + b'abc'
        r, calls = self.run_job(10 << 20, [good[:-1], good], size_word=len(good))
        self.assertEqual(r, [b'abc']); self.assertEqual(len([c for c in calls if c[0] == 'pull']), 2)

    def test_a_cut_file_on_the_phone_names_its_space(self):
        cut = b'DMOR' + struct.pack('<I', 1) + struct.pack('<I', 3) + b'ab'
        r, _ = self.run_job(10 << 20, [cut])
        self.assertIsInstance(r, ValueError); self.assertIn('cut short', str(r)); self.assertIn('free now', str(r))

    def test_a_game_gone_without_its_last_line_is_taken_at_its_output(self):   # issue #228: on Android the host's last lines can miss log.txt as it _exit()s
        good = b'DMOR' + struct.pack('<I', 1) + struct.pack('<I', 3) + b'abc'; logged = []
        r, calls = self.run_job(10 << 20, [good], log='DM_OODLE_JOB: OodleLZ_Decompress at main+0x5a9e590', logged=logged)
        self.assertEqual(r, [b'abc']); self.assertIn('its last log lines did not reach log.txt', logged[0])
        self.assertFalse([c for c in calls if c[0] == 'logcat'])   # a job that finished needs no evidence

    def test_a_gone_game_without_output_logs_what_the_phone_shows(self):   # issue #228: an empty log.txt said nothing more
        logged = []
        r, calls = self.run_job(10 << 20, [None], log='', logged=logged)
        self.assertIsInstance(r, ValueError); self.assertIn('stopped before it decoded anything', str(r))
        self.assertIn('---- ls -l /sdcard/x ----', logged[0]); self.assertIn('exited cleanly (0)', logged[0]); self.assertIn('no host for it', logged[0])
        self.assertFalse([c for c in calls if c[0] == 'pull'])

    def test_the_job_mounts_an_empty_rom_of_its_own(self):   # the host's rom mount refuses a game whose files are all missing (hle_fs.c), and a job reads none: its env points DM_ROMFS
        env, good = [], b'DMOR' + struct.pack('<I', 1) + struct.pack('<I', 3) + b'abc'   # at an empty folder beside the job, not at the game folder's RomFS
        r, calls = self.run_job(10 << 20, [good], env=env)
        self.assertEqual(r, [b'abc'])
        self.assertTrue(any(c[0] == 'shell' and 'mkdir -p /sdcard/x/oodle_romfs' in c[1] for c in calls))
        self.assertIn('DM_ROMFS=/sdcard/x/oodle_romfs', env[0])
        self.assertFalse(any('games' in c[1] and 'RomFS' in c[1] for c in calls if c[0] == 'shell'))   # the game folder keeps no placeholder


class SplitCacheTest(unittest.TestCase):
    """The shader split's Oodle streams decoded on the phone before it runs (issue #310): Cache writes what shader_split.py oodle_cached reads (DM_OODLE_CACHE), a record run
    (DM_OODLE_RECORD) lists the streams the cache lacks with zeros in their place, and fill runs the split until it lists nothing new. The split here is a reader in this process"""
    @classmethod
    def setUpClass(cls):
        had = os.environ.get('DM_GAME'); os.environ['DM_GAME'] = '01001F5010DFA000'   # shader_split loads a profile at import (Pokémon Legends: Arceus's here)
        try:
            sys.path.insert(0, str(REPO / 'dm/gfx/tools')); import shader_split; cls.ss = shader_split   # pyright: ignore[reportMissingImports]
        finally:
            if had is None: os.environ.pop('DM_GAME', None)
            else: os.environ['DM_GAME'] = had

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, self.tmp, True)
        self.cache = ao.Cache(self.tmp / 'cache', 'main-1')
        self.addCleanup(setattr, self.ss, '_CACHED', None)

    def run_split(self, fn, record=None):   # one run of the split, as a new process: its cache index read afresh
        self.ss._CACHED = None
        env = {'DM_OODLE_CACHE': str(self.cache.d)} | ({'DM_OODLE_RECORD': str(record)} if record else {})
        with mock.patch.dict(os.environ, env):
            if not record: os.environ.pop('DM_OODLE_RECORD', None)
            return fn()

    def test_the_split_reads_the_cache_and_lists_what_it_lacks(self):
        a, b, bad = stream(b'a' * 300), stream(b'b' * 50), b'\x8c' + b'refused'
        self.cache.add([(ao.stream_key(a, 300), b'a' * 300), (ao.stream_key(bad, 9), None)])
        self.assertEqual(self.run_split(lambda: self.ss.oodle(a, 300)), b'a' * 300)
        self.assertIsNone(self.run_split(lambda: self.ss.oodle_supposed(bad, 9)))   # refused, as the Mac host's 0: a blind scan's guess is no table
        with self.assertRaises(SystemExit) as e: self.run_split(lambda: self.ss.oodle(bad, 9))
        self.assertIn('refused this stream', str(e.exception))
        with self.assertRaises(SystemExit) as e: self.run_split(lambda: self.ss.oodle(b, 50))   # missing, and no record run: red, never zeros
        self.assertIn('not among the streams decoded before the split', str(e.exception))
        want = self.tmp / 'want'; want.mkdir()
        got = self.run_split(lambda: [self.ss.oodle(b, 50), self.ss.oodle(b, 50), self.ss.oodle(a, 300)], want)
        self.assertEqual(got, [bytes(50), bytes(50), b'a' * 300])
        w = ao.wants(want)
        self.assertEqual(list(w), [ao.stream_key(b, 50)])   # listed once
        f, at, cl, head = w[ao.stream_key(b, 50)]
        self.assertEqual((f.read_bytes()[at:at + cl], head), (b, True))

    def test_fill_settles_two_layers_then_the_split_needs_no_phone(self):
        inner = [b'shader %d ' % i * 40 for i in range(3)]
        outer = [(stream(stream(x)), len(stream(x))) for x in inner]
        def read(): return [self.ss.oodle(self.ss.oodle(o, n), len(x)) for (o, n), x in zip(outer, inner)]   # a block holding a compressed group
        phone, asked = FakePhone(), []
        def decoder(): asked.append(1); return phone
        runs, n = ao.fill(self.cache, lambda w: self.run_split(read, w), decoder, self.tmp / 'want')
        self.assertEqual((runs, n, phone.jobs, len(asked)), (2, 6, [3, 3], 1))   # the blocks, then the groups; the groups' zeros of the first run never sent
        self.assertEqual(self.run_split(read), inner)   # the real split: the cache alone
        self.assertFalse((self.tmp / 'want').exists())
        def no_phone(): raise AssertionError('the phone was asked for')
        self.assertEqual(ao.fill(self.cache, lambda w: self.run_split(read, w), no_phone, self.tmp / 'want'), (0, 0))

    def test_jobs_of_at_most_chunk_and_a_cut_batch(self):
        streams = [(stream(bytes([65 + i]) * 100), 100) for i in range(5)]
        def read(): return [self.ss.oodle(c, n) for c, n in streams]
        phone = FakePhone()
        with mock.patch.object(ao, 'CHUNK', 250): ao.fill(self.cache, lambda w: self.run_split(read, w), lambda: phone, self.tmp / 'want')
        self.assertEqual(phone.jobs, [2, 2, 1])   # each written to the cache as it comes back
        self.assertEqual(sorted(p.name for p in self.cache.d.glob('*.idx')), ['1.idx', '2.idx', '3.idx'])
        self.assertEqual(self.run_split(read), [bytes([65 + i]) * 100 for i in range(5)])
        (self.cache.d / '4.pack').write_bytes(b'cut'); (self.cache.d / '4.idx.partial').write_text('x\t1\t0\n')   # a batch whose index never landed
        again = ao.Cache(self.cache.d, 'main-1')
        self.assertEqual(len(again), 5)
        again.add([(ao.stream_key(b'\x8cz', 1), b'z')]); self.assertTrue((self.cache.d / '4.idx').is_file())

    def test_a_drive_too_small_for_the_decode_stops_before_the_phone(self):   # issue #436: told before the jobs, not after an hour of them
        streams = [(stream(bytes([65 + i]) * 100), 100) for i in range(5)]
        def read(): return [self.ss.oodle(c, n) for c, n in streams]
        def no_phone(): raise AssertionError('the phone was asked for')
        with mock.patch.object(ao, 'host_free', lambda f: 1000), self.assertRaises(ValueError) as e:
            ao.fill(self.cache, lambda w: self.run_split(read, w), no_phone, self.tmp / 'want')
        self.assertIn('5 remaining Oodle streams', str(e.exception)); self.assertIn(str(self.cache.d), str(e.exception))

    def test_another_decoder_empties_the_cache(self):
        self.cache.add([(ao.stream_key(stream(b'x'), 1), b'x')]); self.cache.finish('inputs-1')
        self.assertTrue(ao.Cache(self.cache.d, 'main-1').done('inputs-1'))
        self.assertFalse(ao.Cache(self.cache.d, 'main-1').done('inputs-2'))
        other = ao.Cache(self.cache.d, 'main-2')
        self.assertEqual((len(other), other.done('inputs-1'), sorted(p.name for p in other.d.iterdir())), (0, False, ['stamp']))


if __name__ == '__main__':
    unittest.main()
