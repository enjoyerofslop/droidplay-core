"""dm/android/au_pace.h (the AAudio output's 5 ms pacer and its ring, coreaudio_aaudio.c), built for the PC with the system's clang and driven through ctypes:
a simulation in event time of a device asking for its frames in data callbacks of a set size, a game that keeps a few buffers queued and appends one each time
one comes back (CRIWARE in DRAGON QUEST III HD-2D Remake and FINAL FANTASY RESONANCE: three of 240 frames), and either the host's render straight from each
data callback, as before, or the pacer filling the ring on its own 5 ms clock. Frames carry their number, so the ring's order across its wrap is checked too.
POSIX only: skipped on Windows, and without a C compiler."""
import ctypes
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
CC = ['xcrun', 'clang'] if sys.platform == 'darwin' and shutil.which('xcrun') else [shutil.which('clang') or shutil.which('cc') or '']
SRC = r'''#include "au_pace.h"
#define X __attribute__((visibility("default")))
enum { Q = 240, RATE = 48000, CAP = Q * 64, CH = 2, NB = 64 };
typedef struct {   // what a run gives back
    double game_zero, ring_short, produced, seconds;   // frames the host padded for want of a game buffer, frames the ring could not give a data callback, frames the game made
    int order_bad, most_per_tick, fill_max, engaged;
} result;
static int16_t pcm[CAP * CH];
static uint64_t lcg(uint64_t *s) { *s = *s * 6364136223846793005ull + 1442695040888963407ull; return *s >> 33; }

// The game: nb buffers of bf frames; each one released comes back appended lat_ns later (its audio thread waking and mixing)
typedef struct { double back_at[NB]; int queued, pending, nb, bf; uint32_t pos; uint64_t made; double lat; } game;
static void game_run(game *g, double t) {   // appends the buffers whose mixing is done by t
    for (int i = 0; i < g->pending; ) {
        if (g->back_at[i] <= t) { g->back_at[i] = g->back_at[--g->pending]; g->queued++; } else i++;
    }
}
static void game_pull(game *g, double t, int16_t *dst, uint32_t n, uint64_t *seq, result *r) {   // hle_audio.c pull: a buffer goes back when played to the end
    game_run(g, t);
    for (uint32_t i = 0; i < n; i++) {
        if (!g->queued) { r->game_zero++; if (dst) dst[i * CH] = dst[i * CH + 1] = 0; continue; }
        uint64_t s = (*seq)++;
        if (dst) { dst[i * CH] = (int16_t)(s & 0x7fff); dst[i * CH + 1] = 1; }
        if (++g->pos == (uint32_t)g->bf) { g->pos = 0; g->queued--; g->made += g->bf; g->back_at[g->pending++] = t + g->lat; }
    }
}
// cb: data callback size (a negative one: random sizes from 64 to -cb); burst: the stream's burst, the size its callbacks come in; dev_ppm: the device clock's
// speed against the host's
X void simulate(int ring, int cb, int burst, double dev_ppm, int nb, int bf, double lat_ms, double seconds, result *r) {
    memset(r, 0, sizeof *r);
    aup_ring g; aup_init(&g, pcm, CAP, CH, Q);
    game gm = {.nb = nb, .bf = bf, .lat = lat_ms / 1e3, .queued = nb};
    uint64_t seq_in = 0, seq_out = 0, rnd = 1; int paced = 0, primed = 0;
    uint32_t cb_max = (uint32_t)burst;
    const double tick = (double)Q / RATE;
    double t_dev = 0.0005, t_tick = 0, last = 0;
    int16_t buf[4096 * CH];
    while (t_dev < seconds || t_tick < seconds) {
        if (t_dev <= t_tick) {   // a data callback
            uint32_t n = cb > 0 ? (uint32_t)cb : 64 + (uint32_t)(lcg(&rnd) % (uint64_t)(-cb - 64 + 1));
            if (n > cb_max) cb_max = n;
            if (paced) {
                uint32_t got = aup_read(&g, buf, n);
                if (got < n && primed) r->ring_short += n - got;
                if (got == n) primed = 1;
                for (uint32_t i = 0; i < got; i++) if (buf[i * CH + 1]) { if ((uint64_t)buf[i * CH] != (seq_out & 0x7fff)) r->order_bad++; seq_out++; }
            } else {
                game_pull(&gm, t_dev, NULL, n, &seq_in, r);
                if (ring && aup_wanted(n, Q)) { paced = 1; r->engaged = 1; seq_out = seq_in; last = t_tick; }
            }
            t_dev += (double)n / RATE / (1 + dev_ppm * 1e-6);
        } else {   // a pacer tick
            if (paced) {
                double due = (t_tick - last) * RATE; last = t_tick;
                uint32_t target = aup_target(cb_max, Q, g.cap), k = aup_quanta(aup_fill(&g), target, cb_max, Q, (uint64_t)due);
                if ((int)k > r->most_per_tick && t_tick > 1) r->most_per_tick = (int)k;   // once the ring has filled
                for (uint32_t i = 0; i < k; i++) { game_pull(&gm, t_tick, aup_slot(&g), Q, &seq_in, r); aup_commit(&g); }
                if ((int)aup_fill(&g) > r->fill_max) r->fill_max = (int)aup_fill(&g);
            }
            t_tick += tick;
        }
    }
    r->produced = (double)gm.made; r->seconds = seconds;
}
X uint32_t target(uint32_t m) { return aup_target(m, Q, CAP); }
X int wanted(uint32_t n) { return aup_wanted(n, Q); }
X uint32_t quanta(uint64_t fill, uint32_t target, uint32_t m, uint64_t due) { return aup_quanta(fill, target, m, Q, due); }
'''


class Result(ctypes.Structure):
    _fields_ = [('game_zero', ctypes.c_double), ('ring_short', ctypes.c_double), ('produced', ctypes.c_double), ('seconds', ctypes.c_double),
                ('order_bad', ctypes.c_int), ('most_per_tick', ctypes.c_int), ('fill_max', ctypes.c_int), ('engaged', ctypes.c_int)]


@unittest.skipIf(os.name == 'nt' or not CC[-1], 'a C compiler needed')
class AuPace(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp())
        (cls.tmp / 'p.c').write_text(SRC)
        subprocess.run([*CC, '-shared', '-fPIC', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-I', REPO / 'dm/android', cls.tmp / 'p.c', '-o', cls.tmp / 'p.so'], check=True)
        cls.lib = ctypes.CDLL(str(cls.tmp / 'p.so'))
        cls.lib.simulate.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_double, ctypes.c_int, ctypes.c_int, ctypes.c_double, ctypes.c_double, ctypes.POINTER(Result)]
        cls.lib.quanta.argtypes = [ctypes.c_uint64, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint64]
        cls.lib.quanta.restype = cls.lib.target.restype = ctypes.c_uint32

    @classmethod
    def tearDownClass(cls): shutil.rmtree(cls.tmp, ignore_errors=True)

    def run_sim(self, ring, cb, ppm=0.0, nb=3, bf=240, lat_ms=0.5, seconds=60.0, burst=0):
        r = Result()
        self.lib.simulate(ring, cb, burst, ppm, nb, bf, lat_ms, seconds, ctypes.byref(r))
        return r

    def test_direct_960_starves_three_blocks(self):   # the bug: three 240-frame buffers, 960 frames a callback: a quarter of every callback padded, the game slowed to 3/4
        r = self.run_sim(0, 960)
        self.assertFalse(r.engaged)
        self.assertAlmostEqual(r.game_zero / (r.seconds * 48000), 0.25, delta=0.01)
        self.assertAlmostEqual(r.produced / r.seconds, 36000, delta=500)

    def test_ring_960_feeds_three_blocks(self):
        r = self.run_sim(1, 960)
        self.assertTrue(r.engaged)
        self.assertEqual(r.ring_short, 0)
        self.assertLessEqual(r.game_zero, 960)   # the start only: the callback that switched to the pacer was still played straight
        self.assertEqual(r.order_bad, 0)
        self.assertAlmostEqual(r.produced / r.seconds, 48000, delta=100)
        self.assertEqual(r.most_per_tick, 1)   # one buffer back per 5 ms once the ring is full, as from the console's mixer
        self.assertLessEqual(r.fill_max, self.lib.target(960))

    def test_small_callbacks_stay_direct(self):   # the Odin 2's 144-frame burst and the Mac's 240: no ring, as before
        for cb in (144, 192, 240):
            r = self.run_sim(1, cb, seconds=10)
            self.assertFalse(r.engaged, cb)
            self.assertEqual(r.game_zero, 0, cb)
        self.assertFalse(self.lib.wanted(240))
        self.assertTrue(self.lib.wanted(241))

    def test_device_clock_drift(self):   # a device clock 0.2 % off the host's for ten minutes: the ring neither runs dry nor grows
        for ppm in (-2000, 2000):
            r = self.run_sim(1, 960, ppm=ppm, seconds=600)
            self.assertEqual(r.ring_short, 0, ppm)
            self.assertLessEqual(r.game_zero, 960, ppm)
            self.assertLessEqual(r.fill_max, self.lib.target(960), ppm)
            self.assertEqual(r.order_bad, 0, ppm)
            self.assertAlmostEqual(r.produced / r.seconds, 48000 * (1 + ppm * 1e-6), delta=150)

    def test_varying_callbacks(self):   # AAudio's legacy path: 64 to 960 frames a callback (the Thor Pro's log, burst 960)
        r = self.run_sim(1, -960, burst=960, seconds=120)
        self.assertTrue(r.engaged)
        self.assertEqual(r.ring_short, 0)
        self.assertLessEqual(r.game_zero, 960)
        self.assertEqual(r.order_bad, 0)

    def test_slow_game_thread(self):   # the game's audio thread taking 4 ms to mix a buffer: still fed, since a buffer comes back only every 5 ms
        r = self.run_sim(1, 960, lat_ms=4.0)
        self.assertEqual(r.ring_short, 0)
        self.assertLessEqual(r.game_zero, 960)
        r = self.run_sim(0, 960, lat_ms=4.0)
        self.assertGreater(r.game_zero / (r.seconds * 48000), 0.2)

    def test_other_queues(self):   # deeper queues (the audio renderer: four of 240) and larger buffers (FINAL FANTASY RESONANCE's second output, 1536 frames)
        for nb, bf in ((4, 240), (2, 1536), (2, 2048)):
            r = self.run_sim(1, 960, nb=nb, bf=bf)
            self.assertEqual(r.ring_short, 0, (nb, bf))
            self.assertLessEqual(r.game_zero, 960, (nb, bf))
            self.assertAlmostEqual(r.produced / r.seconds, 48000, delta=200)

    def test_target_and_quanta(self):
        self.assertEqual(self.lib.target(960), 1440)
        self.assertEqual(self.lib.target(1000), 1680)   # rounded up to whole quanta
        self.assertEqual(self.lib.target(100000), 240 * 63)   # never past the ring, less a quantum
        t = self.lib.target(960)
        self.assertEqual(self.lib.quanta(t, t, 960, 240), 0)   # full
        self.assertEqual(self.lib.quanta(t - 960, t, 960, 240), 1)   # where a callback on time leaves it: one a tick
        self.assertEqual(self.lib.quanta(t - 1200, t, 960, 240), 2)   # below that: one more
        self.assertEqual(self.lib.quanta(0, t, 960, 720), 4)   # a late tick (15 ms) makes up its time, plus one below the low point
        self.assertEqual(self.lib.quanta(t - 240, t, 960, 2400), 1)   # never past the target


if __name__ == '__main__':
    unittest.main()
