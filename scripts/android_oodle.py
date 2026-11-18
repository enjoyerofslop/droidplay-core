#!/usr/bin/env python3
# Unreal Engine shaders decoded with the game's own Oodle on the phone, for build_android_from_nsp.py on a computer that cannot run the game's code.
# An Unreal game's shaders are Oodle-compressed (Octopath Traveler 0: each shader in its paks' shader code archives; FINAL FANTASY RESONANCE: its IoStore container's blocks and
# each shader group), and only the decoder statically linked into main undoes it (dm/host/hle_oodle.c). The NSP scripts run it in the Mac host; a Windows or Linux PC cannot run
# the game's arm64 code, but the phone can: the game's Android host runs the same job (DM_OODLE_JOB, before nnMain, then it exits), with only the game's rewritten code on the
# phone (no RomFS: the job reads none, and its env.txt points DM_ROMFS at an empty folder of its own, since the host's rom mount refuses a game whose files are all missing). FINAL FANTASY RESONANCE's decoded shader group matched the Mac's archive byte for byte (35 of 35 shaders, OnePlus Pad 3).
# A job is one launch of the game, so the decodes uepak.py asks for one at a time are batched (Rounds): uepak runs with a decoder that answers from what the phone decoded so far
# and records the rest, the records go to the phone as one job, and uepak runs again until it asks for nothing new. An IoStore container takes two or three rounds (its blocks,
# then the shader groups inside them).
# The shader split's own Oodle-compressed archives (profile_guess.py oodle_packs: Pokémon Legends: Arceus's GFPAK entries, Legends: Z-A's Trinity packs, Bayonetta 3's PKZ entries,
# Batman: Arkham Knight's Unreal Engine 3 chunks, EA SPORTS FC 27's Frostbite items) go the same way across the split's worker processes (fill, issue #310): the split runs with
# DM_OODLE_RECORD, its workers list the streams the cache lacks, the phone decodes them into the cache (Cache, which the split reads with DM_OODLE_CACHE), and the split runs again
# until it lists nothing new; then the real split reads the cache alone, so a stream still missing is red there, never zeros.
import hashlib, shutil, struct, time
from pathlib import Path

JOB_SECS = 600   # the longest a job may take: FINAL FANTASY RESONANCE's 5,919 shader groups (707 MB decoded) took three jobs of 2 to 3 s each on a OnePlus Pad 3, the boot up to nnMain included
CHUNK = 256 << 20   # decoded bytes per job, so neither the phone's free space nor this computer's memory holds more than about that at once
SPARE = 64 << 20   # left free on the phone beyond a job's file and its output, for Android and the game's log


def oodle_head(b):   # whether the bytes start as an Oodle LZ block does (header byte: low nibble 0xC, bits 4-5 clear; bit 6 uncompressed, bit 7 decoder restart)
    return len(b) > 1 and (b[0] & 0x3F) == 0x0C


def job_bytes(records):   # hle_oodle.c's job file: "DMOJ", u32 count, then per record u32 compressed length, u32 raw length and the compressed bytes
    out = bytearray(b'DMOJ' + struct.pack('<I', len(records)))
    for c, n in records: out += struct.pack('<II', len(c), n) + c
    return bytes(out)


def read_out(path, count):   # its output: "DMOR", u32 count, then per record u32 decoded length (0: failed) and the bytes → [bytes or None]
    b = Path(path).read_bytes()
    if len(b) < 8 or b[:4] != b'DMOR' or struct.unpack_from('<I', b, 4)[0] != count: raise ValueError(f'{path}: not the output of a {count}-record Oodle job')
    o, out = 8, []
    for i in range(count):
        n = struct.unpack_from('<I', b, o)[0] if o + 4 <= len(b) else -1
        # a file that ends inside a record was not written whole: the phone's storage filled up as the job wrote it, or the copy from the phone was cut (issue #111)
        if n < 0 or o + 4 + n > len(b): raise ValueError(f'{path} is cut short: it ends at {len(b):,} bytes, inside record {i + 1} of {count}')
        o += 4; out.append(b[o:o + n] if n else None); o += n
    if o != len(b): raise ValueError(f'{path}: {len(b) - o} bytes left after its {count} records')
    return out


def out_size(records):   # the most a job's output can take: its header, then each record's length word and decoded bytes (a failed record takes only its word)
    return 8 + sum(4 + n for _, n in records)


def free_bytes(df_k):   # bytes available, from the last line of `df -k <folder>` (Filesystem, 1K-blocks, Used, Available, ...); None when it cannot be read
    f = df_k.strip().splitlines()[-1].split() if df_k.strip() else []
    return int(f[3]) * 1024 if len(f) >= 4 and f[3].isdigit() else None


def mb(n): return f'{n / 1048576:,.0f} MB'


def host_free(folder):   # bytes free on this computer's drive that holds folder
    return shutil.disk_usage(folder).free


def no_room(what, folder, need):
    """The ValueError for a drive on this computer short of space, before the build writes there: on Windows a full drive made adb pull fail with "cannot write …
    Input/output error" after 35 of Pokémon Legends: Z-A's jobs (issues #415, #436), and nothing said it was the computer's drive, not the phone's"""
    free = host_free(folder)
    return None if free >= need else ValueError(
        f'{what} needs about {mb(need)} free on this computer\'s drive with {folder}, and it has {mb(free)}: free up space there, or build from a drive with more, then build '
        'again (what the phone decoded so far is kept)')


class Rounds:
    """decode(compressed, raw length) for uepak.py: what the phone decoded so far, else zeros, the stream kept for the next job. Only a stream that starts as Oodle's do is kept:
    bytes made from an earlier round's zeros never do, so no record is a guess (uepak's checks on such bytes raise ValueError, which run() counts as a round to finish)"""
    def __init__(self): self.have, self.want = {}, {}
    def __call__(self, comp, n):
        comp = bytes(comp); k = (hashlib.blake2b(comp, digest_size=16).digest(), n)
        if k in self.have: return self.have[k]
        if oodle_head(comp): self.want[k] = comp
        return bytes(n)
    def run(self, fn, phone, rounds=8):
        """fn() until a run asks for nothing new → (its result, the rounds that went to the phone). phone(records) → [bytes or None]"""
        for r in range(rounds + 1):
            self.want = {}
            try: res = fn()
            except ValueError:
                if not self.want: raise   # a damaged file, not a round to finish
                res = None
            if not self.want: return res, r
            keys = list(self.want); got = phone([(self.want[k], k[1]) for k in keys])
            for k, d in zip(keys, got):
                if d is None or len(d) != k[1]: raise ValueError(f'the game\'s Oodle on the phone failed on a {len(self.want[k])} → {k[1]} byte stream')
                self.have[k] = d
        raise ValueError(f'uepak still asked for new Oodle streams after {rounds} rounds')


class Phone:
    """Oodle jobs run by the game's Android host on the phone: the host and the game's rewritten code must be installed (build_android_from_nsp.py ue_decode).
    adb(serial, *args, check=True) and sh(text) are build_android_from_nsp.py's; remote = the app's external files folder"""
    def __init__(self, adb, serial, gid, remote, pkg, work, logs, say):
        self.adb, self.serial, self.gid, self.remote, self.pkg, self.work, self.logs, self.say = adb, serial, gid, remote, pkg, Path(work), Path(logs), say
        self.jobs = 0
    def sh(self, cmd, check=True): return self.adb(self.serial, 'shell', cmd, check=check)
    def evidence(self):
        """What the phone shows of a job that stopped early, for its log (issue #228: an empty log.txt said nothing more): the files folder (whether log.txt and the job's output
        were written) and logcat's lines of the app, of the system starting and ending its process (Zygote's exit status), and of any crash. The app tells a missing game or host
        only in a toast (Launcher.java)"""
        ls = self.sh(f'ls -l {self.remote}', check=False)
        lc = self.adb(self.serial, 'logcat', '-d', '-v', 'time', '-t', '3000', '-s', 'DroidPlay:V', 'ActivityManager:I', 'ActivityTaskManager:I', 'Zygote:I',
                      'AndroidRuntime:E', 'DEBUG:V', 'libc:F', check=False)
        return (f'\n---- ls -l {self.remote} ----\n{ls}\n---- logcat ----\n{lc}\n---- if the phone showed "The game … is not on this phone, or this app has no host for it", '
                'the app was installed without this game\'s host, or the game\'s code did not reach the phone ----\n')
    def __call__(self, records):   # → [bytes or None], in jobs of at most CHUNK decoded bytes
        out, part = [], []
        for c, n in records:
            if part and sum(x[1] for x in part) + n > CHUNK: out += self.job(part); part = []
            part.append((c, n))
        return out + (self.job(part) if part else [])
    def job(self, records):
        self.jobs += 1; t0 = time.time()
        local = self.work / 'tmp' / 'oodle_phone.job'; local.parent.mkdir(parents=True, exist_ok=True); local.write_bytes(job_bytes(records))
        rjob, rlog, renv = f'{self.remote}/oodle.job', f'{self.remote}/log.txt', f'{self.remote}/env.txt'
        rom = f'{self.remote}/oodle_romfs'
        # The job's host mounts the rom before anything runs (hle_fs.c) and refuses a game whose files are all missing, and a job reads none: env.txt points DM_ROMFS at an empty
        # folder of the job's own, beside the job file. Not the game folder's RomFS, which start_host prefers over RomFS.img: an empty one left there would show a later launch an empty game
        self.sh(f'mkdir -p {rom}; chmod 777 {rom}', check=False)   # 777: adb's shell user made it, and Android 16 lets the game read only what it can
        # the job writes its whole output next to its file, and the game's host does not stop when a write fails: on a phone short of space the output came back cut (issue #111)
        need, free = local.stat().st_size + out_size(records) + SPARE, free_bytes(self.sh(f'df -k {self.remote}', check=False))
        if free is not None and free < need:
            local.unlink(missing_ok=True)
            raise ValueError(f'the phone has {mb(free)} free, and decoding with the game\'s Oodle there needs about {mb(need)} at a time: free up space on the phone, then build again')
        env_before = self.sh(f'cat {renv} 2>/dev/null', check=False)   # the player's env.txt, put back after the job
        try:
            self.sh(f'am force-stop {self.pkg}; rm -f {rjob} {rjob}.out {rlog}')
            self.adb(self.serial, 'push', local, rjob)
            envf = self.work / 'tmp' / 'oodle_env.txt'; envf.write_text(f'DM_OODLE_JOB={rjob}\nDM_ROMFS={rom}\n', encoding='utf-8', newline='\n')
            self.adb(self.serial, 'push', envf, renv)
            self.sh(f'chmod 666 {rjob} {renv}', check=False)   # Android 16 (a OnePlus Pad 3): the game cannot read what adb's shell user wrote otherwise
            self.sh(f'am start -n {self.pkg}/.Launcher --es game {self.gid} --ez play true')
            done, fail, log, gone = None, None, '', False
            while time.time() - t0 < JOB_SECS:
                time.sleep(1)
                log = self.sh(f'grep -E "DM_OODLE_JOB|FATAL|CRASH" {rlog} 2>/dev/null', check=False)
                if 'records decoded' in log: done = True; break
                if 'FATAL' in log or 'CRASH' in log or 'cannot' in log: fail = log; break
                if time.time() - t0 > 20 and not self.sh(f'pidof {self.pkg}:game', check=False).strip() and 'records decoded' not in log:
                    gone, fail = True, log or 'the game stopped before it decoded anything'; break
            text = self.sh(f'cat {rlog} 2>/dev/null', check=False)
            # The host writes its output whole, logs "records decoded" and _exit()s. On Android its stderr reaches log.txt through a thread reading a pipe (front_android.c
            # log_reader), and the process can end before that thread has written the last lines: on an Ayn Odin 2 the log stopped at the decoder's address, and on an AYN Thor
            # log.txt stayed empty (issue #228), while Zygote logged the process as exited cleanly (0). A game that has exited and left its output has finished: the copy below
            # and read_out check that the output is whole
            if gone and self.sh(f'stat -c %s {rjob}.out 2>/dev/null', check=False).strip().isdigit():
                done = True; text += '\n[build] the game exited and left its output, but its last log lines did not reach log.txt: the output is checked instead\n'
            if not done: text += self.evidence()
            (self.logs / f'oodle_phone_{self.jobs}.log').write_text(text, encoding='utf-8', errors='replace')
            if not done: raise ValueError(f'the Oodle job on the phone did not finish ({(fail or f"no answer in {JOB_SECS} s").strip()[-400:]}); its log: {self.logs / f"oodle_phone_{self.jobs}.log"}')
            res = self.work / 'tmp' / 'oodle_phone.job.out'; size = self.sh(f'stat -c %s {rjob}.out', check=False).strip()
            if (e := no_room('Copying the Oodle job\'s output from the phone', res.parent, (int(size) if size.isdigit() else out_size(records)) + SPARE)): raise e
            for _ in range(2):   # a copy that came over shorter than the phone's file is made again, once
                try: self.adb(self.serial, 'pull', f'{rjob}.out', res)
                except Exception as e:   # build_android_from_nsp.py's BuildError; PhoneLost (the phone went away) goes on as it is
                    if 'cannot write' not in str(e) and 'cannot create' not in str(e): raise
                    raise ValueError(f'this computer could not write the Oodle job\'s output to {res} ({str(e).strip()[-200:]}). This is usually its drive filling up, or a '
                                     f'drive that went away; it has {mb(host_free(res.parent))} free now: free up space there, then build again (what the phone decoded so far '
                                     'is kept)') from None
                if not size.isdigit() or res.stat().st_size == int(size): break
            else: raise ValueError(f'the Oodle job\'s output came over from the phone as {res.stat().st_size:,} of its {int(size):,} bytes, twice: check the USB cable, then build again')
            try: got = read_out(res, len(records))
            except ValueError as e:
                free = free_bytes(self.sh(f'df -k {self.remote}', check=False))
                raise ValueError(f'{e}. The phone wrote it so ({size or "?"} bytes there), and has {mb(free) if free is not None else "an unknown amount"} free now: '
                                 f'if that is little, free up space on the phone and build again; its log: {self.logs / f"oodle_phone_{self.jobs}.log"}') from None
            res.unlink()
        finally:
            self.sh(f'am force-stop {self.pkg}; rm -f {rjob} {rjob}.out', check=False)
            if env_before.strip():
                envb = self.work / 'tmp' / 'oodle_env_before.txt'; envb.write_text(env_before, encoding='utf-8', newline='\n'); self.adb(self.serial, 'push', envb, renv, check=False)
            else: self.sh(f'rm -f {renv}', check=False)
            local.unlink(missing_ok=True)
        self.say(f'Oodle job {self.jobs} on the phone: {len(records):,} streams, {sum(n for _, n in records) / 1048576:.1f} MB decoded in {time.time() - t0:.0f} s')
        return got


def decode(uepak, romfs, main, jd, out, phone):
    """uepak.job / uepak.finish as build_ipa_from_nsp.py decode_ue_shaders runs them, with the phone for the game's Oodle: the IoStore containers' streams in rounds (Rounds),
    then the paks' Oodle job as it is → (uepak's listing, the shaders decoded by job records)"""
    import shutil
    rounds = Rounds()
    def one():
        shutil.rmtree(jd, ignore_errors=True); return uepak.job(str(romfs), str(main), str(jd), decode=rounds)
    res, _ = rounds.run(one, phone)
    if res is None: raise ValueError('uepak gave no listing')   # (run returns only once a run asked for nothing new, which returns one)
    listing, n = res
    if n:   # the paks' shaders (Octopath Traveler 0): uepak wrote the job; the phone decodes it whole
        jf = Path(jd) / 'oodle.job'; b = jf.read_bytes(); cnt = struct.unpack_from('<I', b, 4)[0]; recs, o = [], 8
        for _ in range(cnt):
            cl, rl = struct.unpack_from('<II', b, o); o += 8; recs.append((b[o:o + cl], rl)); o += cl
        got = phone(recs)
        res = bytearray(b'DMOR' + struct.pack('<I', cnt))
        for d in got: res += struct.pack('<I', len(d) if d else 0) + (d or b'')
        (Path(jd) / 'oodle.job.out').write_bytes(bytes(res))
    uepak.finish(str(jd), str(out))
    return listing, n


def stream_key(comp, n): return hashlib.blake2b(comp, digest_size=16).hexdigest(), n   # a stream as shader_split.py oodle_cached looks it up


class Cache:
    """The streams the game's decoder answered, for shader_split.py oodle_cached (DM_OODLE_CACHE): per batch, <n>.pack holds the decoded bytes back to back and <n>.idx a line per
    stream (blake2b-128 of the compressed bytes in hex, raw length, offset in the pack or -1 for a stream the decoder refused). The index is written last, so a batch cut short is
    not taken. stamp names the decoder (the main it is in): a cache another one made is emptied. done(key) / finish(key): the split's inputs the last fill ended on with nothing
    new, so a build from the same files runs no record split"""
    def __init__(self, folder, stamp):
        self.d = Path(folder)
        if not (self.d / 'stamp').is_file() or (self.d / 'stamp').read_text() != stamp: shutil.rmtree(self.d, ignore_errors=True)
        self.d.mkdir(parents=True, exist_ok=True); (self.d / 'stamp').write_text(stamp)
        self.keys = {(h, int(raw)) for f in self.d.glob('*.idx') for h, raw, _ in (ln.split('\t') for ln in f.read_text(encoding='ascii').splitlines())}
    def __contains__(self, k): return k in self.keys
    def __len__(self): return len(self.keys)
    def add(self, items):   # [(key, decoded bytes or None)], one batch
        n, lines, at = 1 + max((int(f.stem) for f in self.d.glob('*.idx')), default=0), [], 0
        with open(self.d / f'{n}.pack', 'wb') as f:
            for (h, raw), d in items:
                if d is None or len(d) != raw: lines.append(f'{h}\t{raw}\t-1\n'); continue   # refused (a job record of length 0)
                lines.append(f'{h}\t{raw}\t{at}\n'); f.write(d); at += raw
        part = self.d / f'{n}.idx.partial'; part.write_text(''.join(lines), encoding='ascii'); part.replace(self.d / f'{n}.idx')
        self.keys.update(k for k, _ in items)
    def done(self, key): return (self.d / 'complete').is_file() and (self.d / 'complete').read_text() == key
    def finish(self, key): (self.d / 'complete').write_text(key)


def wants(folder):
    """The streams a record run listed (shader_split.py oodle_cached's want.<pid> files: u32 compressed length, u32 raw length, the bytes) → {key: (file, offset of the bytes, length,
    whether they start as Oodle's do)}. Only where they are is kept, not the bytes: Pokémon Legends: Arceus's GFPAKs hold 1.6 GB of them, Legends: Z-A's Trinity packs 4.4 GB"""
    out = {}
    for f in sorted(Path(folder).glob('want.*')):
        with open(f, 'rb') as h:
            while len(hd := h.read(8)) == 8:
                cl, raw = struct.unpack('<II', hd); at = h.tell(); c = h.read(cl)
                if len(c) < cl: break   # its worker stopped while it wrote
                out.setdefault(stream_key(c, raw), (f, at, cl, oodle_head(c)))
    return out


def fill(cache, record, phone, want_dir, rounds=8):
    """cache filled with every stream the shader split reads → (the runs that needed the phone, the streams decoded). record(folder) runs the split with DM_OODLE_CACHE and
    DM_OODLE_RECORD=folder; phone() returns the decoder (records → [bytes or None]), called only once a run lists a stream the cache lacks, as the phone and the app are set up then.
    The runs go on until one lists nothing new. A stream that does not start as Oodle's do is not sent: the split asks for one only from an earlier run's zeros (as in Rounds), and
    the real split, reading the cache alone, finds it missing, loudly. Each batch of at most CHUNK decoded bytes is one job, written to the cache as it comes back"""
    want_dir, dec, total = Path(want_dir), None, 0
    try:
        for r in range(rounds + 1):
            shutil.rmtree(want_dir, ignore_errors=True); want_dir.mkdir(parents=True)
            record(want_dir)
            todo = [(k, v) for k, v in wants(want_dir).items() if k not in cache and v[3]]
            if not todo: return r, total
            # the cache keeps every decoded byte, so a drive too small for them is told now, not after an hour of jobs (issue #436); a job's own copy is checked as it comes
            if (e := no_room(f'Decoding the shader split\'s {len(todo):,} remaining Oodle streams with the phone', cache.d, sum(k[1] for k, _ in todo) + SPARE)): raise e
            dec = dec or phone()
            batches, cur, size = [], [], 0
            for k, v in todo:
                if cur and size + k[1] > CHUNK: batches.append(cur); cur, size = [], 0
                cur.append((k, v)); size += k[1]
            for b in batches + [cur]:
                recs = []
                for (_, raw), (f, at, cl, _) in b:
                    with open(f, 'rb') as h: h.seek(at); recs.append((h.read(cl), raw))
                cache.add(list(zip((k for k, _ in b), dec(recs)))); total += len(b)
        raise ValueError(f'the shader split still listed new Oodle streams after {rounds} runs with the phone')
    finally: shutil.rmtree(want_dir, ignore_errors=True)
