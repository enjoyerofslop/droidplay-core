// Frame step in whole vsyncs (pure function, shared by Mac / iOS; unit test tests/t_fstep.c). The rewriter's frame-step stubs (patch_nso.py FRAME_STEP: Tears of the Kingdom 1.4.3,
// Breath of the Wild 1.9.0) step the game by the vsyncs (1/60 s) the host hands them each frame, and the host keeps each frame on screen for k vsyncs (hle_nvn.c present_itv).
// At a steady rate the step is the time on screen: even motion, and only steps the games' own interval logic uses (60 / 30 / 20 fps). Real time the game falls behind (a late
// frame, a hitch) is a debt paid back one extra vsync per frame, so over time game time = real time. k follows the frames' cost: three late frames in a row step up to the cost
// (30 fps when 60 does not hold: no 40 fps steps on 120 Hz screens); 30 frames in a row whose cost would fit k − 1 try it, and a trial that fails waits twice as long before
// the next (1 s → 8 s), so a device on the edge does not flap.
// A probe is a trial back to kmin that the cost does not justify: while the caller reports the CPU slowed for reasons other than heat (an iPhone's performance controller
// parks a game that waits out a long hold on the efficiency cores at half the clock, where its cost never fits k − 1 again), the hold-off alone starts one. Its first
// FSTEP_GRACE frames may be late without failing it, the time the controller needs to raise the clock for the shorter deadline; after them it is judged like a trial.
// A probe that holds leaves the hold-off as it is, and the run at kmin it starts must last FSTEP_HOLD_MAX (8 s) before the hold-off returns to 1 s: a shorter one counts as
// a failed probe, so a CPU that really cannot hold kmin is not probed every second.
// A step up after a run at kmin of FSTEP_HOLD_MAX keeps the cost that held there, and a cost at the longer hold of at most FSTEP_SAME times it starts a probe too: the scene
// costs what it did, so the step up came from a hitch (Breath of the Wild on an iPhone: pipeline compiles after a cutscene). The fit trial alone does not bring such a game
// back: an iPhone lowers its GPU clock until the work fills most of the frame, so a GPU-bound game's cost sits near FSTEP_FIT at either hold (BotW: GPU 12.8–15.2 ms at hold
// 2, p50, where hold 1 had held for minutes; the same 4,900 draws took 5.8 ms after a trip to the background and 13.5 ms half a minute later).
// A cap that falls and rises again (a movie, hle_movie.c dm_movie_playing; the iOS thermal cap in hle_nvn.c) keeps what was learned at each kmin: the cost that held it
// (cref) and the GPU reference (nvn_fgpu below); and the frames after the cap rises are a probe with its grace, as the clocks are still set for the longer hold. Before, the
// rise set k to kmin with no grace, so three frames late while the clock rose stepped up again, with cref dropped at the cap change and a GPU reference taken at the 30 fps
// cap: neither the fit trial (a GPU-bound cost near FSTEP_FIT) nor a probe brought kmin back (t_fstep: the movie and thermal cap cases). A cref kept from before the cap
// starts a probe only when the cost at the longer hold is within FSTEP_SAME of it, and a failed probe doubles the hold-off as any other does.
// A hold above kmin that the frames held, on time, for FSTEP_RUN frames keeps their cost (kheld): from a longer hold, a trial back to it starts after the hold-off when that
// cost fits it, whatever the cost at the longer hold is, with a probe's grace for the clock to rise. A GPU's clock falls until the work fills the longer hold: Ocarina of Time on
// the Odin 3 took 20-22 ms a frame at hold 2, then 26-31 ms at hold 3 after one heavy moment stepped it there, a cost at FSTEP_FIT of hold 2 that never made the fit trial's run of
// 30, so it stayed at 20 fps (the probe goes to kmin, 60 fps, which does not hold there). Three such trials failing in a row forget the cost until the frames hold that hold again
#pragma once
#include <math.h>
#include <stdint.h>

#define FSTEP_MAX 3           // the largest step: both games' own interval logic uses 1–3 (BotW clamps to [2, 3], TOTK picks 2 or 3), so their integer readers never see 4
#define FSTEP_LATE 0.1        // a frame is late when it or its GPU work took this many vsyncs (1.7 ms) more than k: 53 fps is not 60 (start-to-start jitter on time is well below)
#define FSTEP_FIT 0.85        // a frame would fit k − 1 when its cost is at most this share of it
#define FSTEP_RUN 30          // frames in a row that fit k − 1 before a trial, and frames a trial must hold
#define FSTEP_MISS 3          // frames of such a run that fit k but not k − 1 (not late): more end the run. A scene's odd heavier frame (OoT on the Odin 3 at hold 3: 1.86 vsyncs
                              // every dozen frames of 1.2) ended every run, and a trial back to hold 2 waited over 700 frames
#define FSTEP_SETTLE 4        // a trial's first frames, whose lateness neither fails it nor counts: presents queued at the longer hold still come at its pace (the Odin 3: the first
                              // three frames of every trial from hold 3 to 2 showed 3 vsyncs at a GPU time of 1.2, and failed it)
#define FSTEP_HITCH_MAX 4     // trials back to kmin in a row that may fail on hitches alone (late frames whose cost fits kmin) without doubling the hold-off: Pokémon Legends:
                              // Arceus's title stalls ended its first trials to 60 fps (cost 0.13-0.34 vsyncs) and the hold-off went 1, 2, 4 s, so 60 came 5-8 s after start-up
#define FSTEP_HITCH_LATE 3    // late frames such a trial may have had besides the run that failed it (a tenth of its FSTEP_RUN frames, soft-late ones included): with more, lateness is
                              // what the scene costs at kmin, not a stall. Breath of the Wild at a 60 fps target on a OnePlus Pad 3 is late nearly every frame at hold 1, each at a cost
                              // that fits it; its failed trial kept the hold-off, the next one held, and it ran at 57 fps with 1,369 of 1,800 frames late instead of a steady 30
// At kmin, late frames that do not step up (soft-late ones: FSTEP_LATE_SOFT) still say whether kmin held. A trial or probe back to kmin whose frames past grace and settling
// were late more often than not fails at its end, as three late frames in a row would fail it; and a run at kmin late more often than not neither keeps its cost as the
// cost that held kmin (cref, which starts probes) nor, after a probe, sets the hold-off back to 1 s: it ends as a failed probe. Breath of the Wild at a 60 fps target on a
// OnePlus Pad 3 is late on about three of four frames at hold 1, nearly all soft-late: trials and probes back there held, its runs at hold 1 lasted past FSTEP_HOLD_MAX, and
// the probes kept bringing it back to hold 1 (51.6 fps, 1,334 of 1,800 frames late) where main held a steady 30. Above kmin soft-late frames keep the hold as before
#define FSTEP_DEBT_MAX 15.0   // debt kept at most (0.25 s): the rest of a long stall (a load) is dropped, not replayed
#define FSTEP_HOLD0 60.0      // vsyncs since the last step up before a trial: 1 s, doubled after each failed trial up to 8 s
#define FSTEP_HOLD_MAX 480.0
#define FSTEP_GRACE 30        // a probe's first frames, whose lateness neither fails it nor counts toward a step up (the controller lowered the clock within 10 frames of a step to 30)
#define FSTEP_NEAR_FIT 1.0    // probe_near's headroom rule for a probe to a hold above kmin: the GPU's best time there against the hold itself, not FSTEP_FIT of it. A governor
                              // that sizes the clock to about 75 % busy leaves Ocarina of Time at hold 3 at 2.25-2.4 vsyncs, past FSTEP_FIT's 2.26, so no probe to hold 2 was made
                              // (Saria's scene on a Galaxy S25 Edge: 20 fps for 15 minutes at 389 of 1,200 MHz); a probe that fails there still shows its frames sooner than hold 3
#define FSTEP_SAME 1.1        // the cost at the longer hold against the cost that held kmin: at most this much is the same scene (BotW at hold 2: 1.0–1.07 times its hold-1 GPU time)
// The grace is renewed (nvn_fstep.ramp) while the probe's frames keep getting cheaper: a controller that raises the clock slowly is still raising it. macOS raises the GPU clock
// of a game that misses its deadline at 85–100% GPU occupancy by about 100 MHz a second (Breath of the Wild on an M5 Pro: 338 → 432 MHz over a 33-frame probe, GPU 24.8 → 18.5 ms,
// then the probe failed at 22 ms frames; hold 1 needs about 560 MHz there), where the iPhone's controller settles within the 30 frames. A window of FSTEP_GRACE frames whose
// second half cost at most 1 − FSTEP_RAMP of its first half, and had a late frame (a window on time is judged as a trial), starts another, up to FSTEP_PROBE_MAX frames in all
#define FSTEP_RAMP 0.05       // the fall in cost from one half of a grace window to the next that shows the clock still rising (the per-frame noise of a 25 ms frame is 4%, of 15 frames 1%)
#define FSTEP_PROBE_MAX 180   // grace frames a probe may take in all (3 s at 60 fps): a clock that keeps rising without reaching the hold is not waited for longer
// A frame late at a hold its GPU fits with room (gpu at most FSTEP_FIT × k) by at most FSTEP_LATE_SOFT vsyncs is the replay pipeline's lateness, not the scene's: the chunks'
// turnover and the game's own waits add a few ms over the GPU's time, and a longer hold would only pace the GPU slower (the clock governors follow the hold). Such frames
// are late for the debt (paid back) but do not step up. And from a longer hold, when the GPU's time has fitted k − 1 for FSTEP_RUN frames, k − 1 is tried with a probe's
// grace whatever the cost at k says (the cost at k holds the longer hold's own waits): Ocarina of Time on a OnePlus Pad 3 at hold 3 measured 2.15 vsyncs of cost with
// 1.3 of GPU, and at hold 2 its frames were 2.0–2.1 (27.5 fps, 32 ms of GPU-bound pipeline); the fit trial (cost ≤ 1.7) and the probe (to hold 1) never tried 2. Three such
// trials failing in a row stop them until the hold changes
#define FSTEP_LATE_SOFT 0.5   // 8 ms at 60 Hz: a frame later than that at a hold its GPU fits steps up as before (the debt pays the rest back). Ocarina of Time's Kokiri frames at hold 2 on the Pad 3 are 2.0–2.4 vsyncs with a p95 of 2.65; at 0.3 three of 2.32–2.39 in a row stepped up after 22 s
#define FSTEP_GFIT_MISS 10    // late frames a GPU-fit run of FSTEP_RUN may hold: at hold 3 one frame in ten is late there (the hold's own pacing), and FSTEP_MISS (3) ended every run
// A trial back to a hold the frames held earlier in the scene (kheld: FSTEP_RUN frames on time there), whichever rule started it (the kheld trial, the GPU-fit trial, a probe
// to the next hold), gets tgrace frames of grace instead of FSTEP_GRACE when the caller sets it: a phone's GPU governor sizes the clock to the busy share, so at the longer
// hold it keeps a clock at which the GPU does not fit the shorter one, and only a trial that lasts long enough for it to see the GPU busy raises it again. Ocarina of Time on
// a Galaxy Z Fold 8 held hold 2 (GPU 1.6 vsyncs at 826 MHz) until a heavy moment sent it to 3, where the clock fell to 578 MHz and the GPU took 2.2; for six minutes every trial
// back to 2 failed within 30-45 frames with the GPU still at 2.1-2.3 (39 failed trials), and one that met the clock already up held 2 to the end of the run. The long grace ends
// early once FSTEP_TBAD frames in a row run later than a frame at the longer hold would (shown or cost more than k + 1 vsyncs: a stutter past what the longer hold shows), or
// once its frames have added half of FSTEP_DEBT_MAX to the debt, counting what was dropped (past that it would be dropped, the game slowed, not paid back). What is left is
// then the ordinary grace, counted from the trial's start: a trial with the long grace is never judged sooner than one without. Tears of the Kingdom on a Galaxy S25 Edge with
// the GPU clock held up: right after a load, the trials back to hold 2 ended at their first frame, as the debt at hold 3 (no payback there) was past 7.5 vsyncs, and failed
// while the area streamed in (frames of 2.8-3.5 vsyncs), where main's 30-frame grace held. Three such trials to one hold failing in a row stop the long grace there until the
// frames hold that hold again, a trial to it holds, or FSTEP_TCOOL has passed since the last failure; then one more is made, and its failure stops it for FSTEP_TCOOL again.
// Without the time limit the stop kept hold 3 for the whole of two of three launches (20 fps, the GPU at 1,200 MHz and 4.5-11 ms a frame: no other rule would try hold 2 there,
// as the top-clock trials had counted as GPU-fit ones and stopped those too). Trials to kmin keep FSTEP_GRACE (kheld is not learned there)
// A hold the scene never held gets the long grace too when the caller sets tnew and the GPU would fit it at its top clock: gpu_top, the frame's GPU time scaled by the clock
// (GPU time x current MHz / top MHz, from the caller; 0 where the clock is not readable), averaged at the current hold (gtop), at most FSTEP_FIT of the hold. Without a clock
// the cost of the frames on time at the current hold (ccur) stands in, which at a governor's low clock rarely fits. Where the clock is read, a held hold needs that fit too:
// under a thermal cap the top clock allowed falls, and a Galaxy S25 Edge capped at 660-734 MHz, where hold 2 never fits, took 48 long trials in 15 minutes (each held hold 2
// for a few hundred frames once in a while, which ended the three-failure stop). And such a fit, FSTEP_RUN frames in a row, starts a trial
// itself (no other rule would: no cost that held, no GPU fit at the current clock, no reference at kmin for a probe): Ocarina of Time on a Galaxy Z Fold 8, GPU 2.2 vsyncs at hold
// 3 at 578 MHz, 1.2 at its top clock. The three-failure stop and the early ends hold for these as for the others; they are not GPU-fit trials (gfail)
#define FSTEP_TGRACE 180      // the long grace (the caller's default; hle_nvn.c DM_FSTEP_TRIAL_GRACE): 6 s at hold 2 when every frame shows 2 vsyncs, 9 s when every one shows 3
#define FSTEP_TBAD 3          // frames in a row later than the longer hold would show them, which end the long grace
#define FSTEP_TCOOL 2700.0    // vsyncs (45 s) after the last failed long trial to a hold before its three-failure stop lifts for one more trial

typedef struct {
    int k, step;              // vsyncs each frame stays on screen; the step handed to the game for the next frame (k, or k + 1 while paying back)
    int kmin;                 // the last call's kmin (a lower one takes effect at once)
    double debt;              // real time the game is behind, in vsyncs
    int late_run, fit_run, trial;   // late frames in a row; frames in a row whose cost fits k − 1; frames left of a trial at k − 1 (0 = none)
    int probe, grace;         // 1: the trial in progress is a probe; 2: a probe held and its run at kmin has not ended yet; frames left of the probe's grace
    int ramp;                 // set by the caller (kept): 1 = a probe's grace is renewed while its frames keep getting cheaper (FSTEP_RAMP; the host's DM_FSTEP_PROBE_RAMP)
    int near_fit_off;         // set by the caller (kept): 1 = probe_near's headroom for a probe above kmin is FSTEP_FIT of the hold, as before FSTEP_NEAR_FIT (hle_nvn.c DM_FSTEP_NEAR_JUDGE=0)
    int probe_near;           // set by the caller (kept): 1 = a probe from a hold two or more above kmin goes to the next shorter hold, not to kmin. A phone's GPU governor sizes the clock
                              // to the busy share, so a probe only has to make the frames late at the next hold for the clock to rise; at kmin (60 fps) a 30 fps game's frames are
                              // late whatever the clock, the probe fails, and the step up lands on the cost's hold, where the clock falls again (Ocarina of Time on a Galaxy S25 Edge)
    double pa, pb; int plate, ptotal;   // the current grace window: the cost of its first and second halves (sums), its late frames; grace frames this probe has taken in all
    double hold, since;       // vsyncs a trial waits for after a step up; vsyncs since the last step up
    int per_vsync;            // set by the caller: the ticks a 1/60 s vsync is (0 = 1). At 120 fps the unit is 1/120 s (hle_nvn.c fps_high), and the waits between trials stay the same real time
    double shown_slack;       // vsyncs a frame on time may show longer than k (set by the caller): one refresh on screens whose rate is not a multiple of 60; the cost keeps FSTEP_LATE
    int screen;               // set by the caller before each call, then cleared: 0 = judge lateness by shown and cost; 1 = the screen showed a recent frame on time, 2 = late. With present
                              // snapshots (nvn_mtl.m) the game runs ahead of the screen and a frame that runs long is absorbed, so only what the screen shows counts
    double run, crun, cref;   // vsyncs at kmin in a row; the cost of its frames on time (running mean; not during a probe's trial, whose frames run while the clock rises);
                              // crun when the last run of FSTEP_HOLD_MAX or more ended in a step up or a cap change (0 = none yet)
    double ccur; int ck, cn;  // the cost of the frames on time at the longer hold ck (running mean) and their count
    int ckmin;                // the kmin run, crun and cref belong to
    double cref_at[FSTEP_MAX + 1];   // cref of each kmin, kept while another kmin is in force
    double kheld[FSTEP_MAX + 1];     // the cost of the frames on time at each hold above kmin, once FSTEP_RUN of them held it (0 = none or forgotten)
    int mtrial, mfail;        // the trial in progress is one by kheld; such trials failed in a row
    double gpu;               // set by the caller before each call: the frame's GPU time in vsyncs (0 = not known), for the soft-late rule and the GPU-fit trial
    int gfit_run, gfit_miss, gtrial, gfail;   // frames whose GPU time fits k − 1 (a run; late frames in it, at most FSTEP_GFIT_MISS); the trial in progress is one by the GPU fit; such trials failed in a row
    int fit_miss, settle;     // frames of the current fit run that fit k but not k − 1; frames left of a trial's settling (FSTEP_SETTLE)
    int tgrace;               // set by the caller (kept): the long grace of a trial back to a hold that held, in frames (0 = off: FSTEP_GRACE as for any trial)
    int tg, tgt, tbad;        // the grace in progress is the long one; the trial in progress had it; frames in a row later than the longer hold (FSTEP_TBAD)
    int tgfail[FSTEP_MAX + 1];   // trials with the long grace to each hold that failed in a row (3 stop it there, for FSTEP_TCOOL after the last)
    double tfail_at[FSTEP_MAX + 1], now, tdebt;   // when the last of them failed; vsyncs since the first call; debt + dropped when the long grace in progress started
    int ttrial;               // the trial in progress was started by the top-clock fit
    uint64_t tgtrials, tgends, tlifts;   // statistics: trials given the long grace; long graces ended early; three-failure stops lifted after FSTEP_TCOOL
    int tnew;                 // set by the caller (kept): a hold the scene never held gets the long grace when the GPU would fit it at its top clock (and such a fit starts a trial)
    double gpu_top;           // set by the caller before each call: the frame's GPU time scaled to the GPU's top clock, in vsyncs (0 = the clock is not known)
    double gtop; int gtn, gtk, gtop_run;   // gpu_top averaged at the current hold gtk over gtn frames; frames in a row whose average fits k − 1
    uint64_t tnewtrials;      // statistics: trials started by the top-clock fit
    int rn, rlate;            // frames of the current run at kmin past any grace, and the late ones of them (soft ones too): a run late more often than not did not hold kmin
    double late_cmax; int hitch_fails, tn, tlate;   // the highest cost of the current late run; trials back to kmin failed on hitches alone in a row (FSTEP_HITCH_MAX); the current trial's frames past grace and settling, and the late ones of them (FSTEP_HITCH_LATE)
    uint64_t late, paid, ups, downs, failed, probes; double dropped;   // statistics for the log
} nvn_fstep;

// Whether the cost at the longer hold is the cost that held kmin (a probe without a slowed report)
static inline int nvn_fstep_same(const nvn_fstep *s) { return s->cref > 0 && s->cn >= FSTEP_RUN && s->ccur <= FSTEP_SAME * s->cref; }
// Whether hold to would fit the GPU at its top clock (tnew): the clock-scaled GPU time at the current hold, or without a clock the cost of its frames on time
static inline int nvn_fstep_topfit(const nvn_fstep *s, int to) {
    double v = s->gtn >= 8 && s->gtop > 0 ? s->gtop : s->cn >= FSTEP_RUN ? s->ccur : 0;
    return v > 0 && v <= FSTEP_FIT * to;
}
// Whether the long grace is stopped at hold to: three of its trials there failed in a row, the last less than FSTEP_TCOOL ago. Once that has passed, one more is allowed
static inline int nvn_fstep_tstop(nvn_fstep *s, int to) {
    if (s->tgfail[to] < 3) return 0;
    if (s->now - s->tfail_at[to] < FSTEP_TCOOL * (s->per_vsync ? s->per_vsync : 1)) return 1;
    s->tgfail[to] = 2; s->tlifts++; return 0;
}
// The grace of a trial to hold to that starts now: the long one when the frames held that hold earlier, or (tnew) it would fit the GPU at its top clock, above kmin, and the
// long ones there are not stopped (nvn_fstep_tstop)
static inline int nvn_fstep_tgrace(nvn_fstep *s, int to, int kmin) {
    s->tbad = 0;
    int clock = s->gtn >= 8 && s->gtop > 0;   // the clock is read: a hold that does not fit even at the top clock allowed now (a thermal cap) gets no long grace, held or not
    s->tg = s->tgt = s->tgrace > 0 && to > kmin && to <= FSTEP_MAX && (s->kheld[to] > 0 || (s->tnew && nvn_fstep_topfit(s, to))) && (!clock || nvn_fstep_topfit(s, to)) && !nvn_fstep_tstop(s, to);
    if (!s->tg) return FSTEP_GRACE;
    s->tdebt = s->debt + s->dropped; s->tgtrials++; return s->tgrace > FSTEP_GRACE ? s->tgrace : FSTEP_GRACE;
}

// Once per frame. shown: real time since the previous frame's start, in vsyncs; cost: the frame's work, max(CPU, GPU), in vsyncs (a GPU that cannot keep up makes frames late too); kmin: 1 at a 60 fps cap, 2 at 30;
// kmax: the longest hold (FSTEP_MAX; 2 at a 30 fps cap, where the display link runs at 30 Hz and holds only even counts); fixed: a step the game has set itself (BotW's
// VFR override; 0 = none), which the host then holds as it is; slow: the CPU runs below its speed for reasons other than heat, so a cost above k − 1 is no evidence
// against kmin (probes). Returns the step for the next frame; s->k is how long that frame stays on screen
static inline int nvn_fstep_next(nvn_fstep *s, double shown, double cost, int kmin, int kmax, int fixed, int slow) {
    const double hold0 = FSTEP_HOLD0 * (s->per_vsync ? s->per_vsync : 1), hold_max = FSTEP_HOLD_MAX * (s->per_vsync ? s->per_vsync : 1);
    if (kmin < 1) kmin = 1;
    if (kmax > FSTEP_MAX) kmax = FSTEP_MAX;
    if (kmax < kmin) kmax = kmin;
    if (!s->hold) s->hold = hold0;
    if (fixed > 0) {   // the game's own step: speed is right by construction, nothing to pay back
        s->k = s->step = fixed < FSTEP_MAX ? fixed : FSTEP_MAX;
        s->debt = 0; s->late_run = s->fit_run = s->trial = s->probe = s->grace = 0; s->run = 0; s->tg = s->tgt = s->ttrial = 0;
        s->kmin = FSTEP_MAX + 1;   // when it ends, k starts again from kmin
        return s->step;
    }
    if (!s->k) { s->k = s->step = s->kmin = kmin; return s->step; }   // first frame: nothing measured yet
    int held = s->k;   // the frame that just ended stayed on screen this long
    s->debt += shown - s->step;
    if (s->debt > FSTEP_DEBT_MAX) { s->dropped += s->debt - FSTEP_DEBT_MAX; s->debt = FSTEP_DEBT_MAX; }
    if (s->debt < -1) s->debt = -1;   // ahead (a frame shorter than its step): nothing slows the game down, so at most one vsync is remembered
    s->since += shown; s->now += shown;
    int cap_probe = kmin < s->kmin && s->kmin <= FSTEP_MAX && s->k > kmin;
    if (cap_probe) {   // the cap rose: a probe at once, whose grace covers the clocks still set for the longer hold
        s->k = kmin; s->probes++; s->trial = FSTEP_RUN; s->probe = 1; s->grace = FSTEP_GRACE; s->tg = s->tgt = s->ttrial = 0; s->fit_run = s->late_run = 0; s->pa = s->pb = 0; s->plate = s->ptotal = 0;
    } else if (kmin < s->kmin || s->k < kmin) s->k = kmin;   // the game's own step ended, or the cap fell
    if (s->k > kmax) s->k = kmax;
    s->kmin = kmin;
    int scr = s->screen, tg_end = 0; s->screen = 0;
    if (s->tg && s->grace > 0 && !cap_probe) {   // the long grace: frames later than the longer hold would show them, or a debt the payback cannot keep, end it
        int bad = scr ? scr == 2 && shown > held + 1 + FSTEP_LATE + s->shown_slack : shown > held + 1 + FSTEP_LATE + s->shown_slack || cost > held + 1 + FSTEP_LATE;
        s->tbad = bad ? s->tbad + 1 : 0;
        if (s->tbad >= FSTEP_TBAD || s->debt + s->dropped - s->tdebt > FSTEP_DEBT_MAX / 2) {   // the rest of the ordinary grace is left (judged at once past it)
            s->grace = s->ptotal < FSTEP_GRACE ? FSTEP_GRACE - s->ptotal : 0; s->tg = 0; s->tbad = 0; s->tgends++; tg_end = !s->grace; }
    }
    int late = scr ? scr == 2 : shown > held + FSTEP_LATE + s->shown_slack || cost > held + FSTEP_LATE, grace = s->grace > 0;
    int soft = late && !scr && s->gpu > 0 && s->gpu <= FSTEP_FIT * held && cost <= held + FSTEP_LATE_SOFT && shown <= held + FSTEP_LATE_SOFT + s->shown_slack;   // the pipeline's lateness at a hold the GPU fits: no step up
    if (grace && !cap_probe) {   // the completed frame at the old cap does not consume the new hold's grace
        if (s->grace > FSTEP_GRACE / 2) s->pa += cost; else s->pb += cost;
        s->plate += late; s->ptotal++;
        if (!--s->grace && s->ramp && s->plate && s->ptotal + FSTEP_GRACE <= FSTEP_PROBE_MAX && s->pb <= (1 - FSTEP_RAMP) * s->pa) { s->grace = FSTEP_GRACE; s->pa = s->pb = 0; s->plate = 0; }   // still getting cheaper: another window
        if (!s->grace) s->tg = 0;
    }
    int settling = s->settle > 0; if (settling) s->settle--;
    if (late) {   // late_cmax follows late_run: a frame in grace, settling or soft-late neither counts in the run nor raises its highest cost (a long grace ended by late frames: judged as three late frames)
        int counts = !(grace || settling || soft);
        s->late++; s->late_run = !counts ? 0 : tg_end ? FSTEP_TBAD : s->late_run + 1; s->fit_run = s->fit_miss = 0;
        if (counts) s->late_cmax = s->late_run == 1 || tg_end ? cost : fmax(s->late_cmax, cost);
    } else s->late_run = 0;
    if (!s->trial) s->tn = s->tlate = 0; else if (!grace && !settling) { s->tn++; s->tlate += late; }   // the trial's late frames, soft ones too (FSTEP_HITCH_LATE)
    if (!(s->k > kmin && s->gpu > 0 && s->gpu <= FSTEP_FIT * (s->k - 1))) s->gfit_run = s->gfit_miss = 0;   // the GPU does not fit the shorter hold (or none known): no run
    else if (!late || soft) s->gfit_run++;
    else if (++s->gfit_miss > FSTEP_GFIT_MISS) s->gfit_run = s->gfit_miss = 0;   // a late frame: up to FSTEP_GFIT_MISS of them in a run (Ocarina of Time's Kokiri Forest at hold 3: one in ten)
    else s->gfit_run++;   // (a late frame does not end the run: the GPU fitted it)
    if (s->gpu_top > 0) { if (held != s->gtk) { s->gtk = held; s->gtn = 0; } s->gtop = s->gtn ? s->gtop + (s->gpu_top - s->gtop) / 8 : s->gpu_top; s->gtn++; }
    s->gtop_run = s->tnew && s->k - 1 > kmin && s->gtk == s->k && s->gtn >= 8 && s->gtop <= FSTEP_FIT * (s->k - 1) ? s->gtop_run + 1 : 0;
    // The cost of the frames on time (a hitch is not what the scene costs): at kmin, and at the longer hold
    if (kmin != s->ckmin) {   // another cap: its run starts over, and what held it before comes back
        if (s->ckmin >= 1 && s->ckmin <= FSTEP_MAX) {
            // A cap can end a sustained run before any hitch saved its cost. Short runs keep the earlier proven reference.
            if (s->run >= hold_max && s->crun > 0 && 2 * s->rlate <= s->rn) s->cref = s->crun;
            s->cref_at[s->ckmin] = s->cref;
        }
        s->ckmin = kmin; s->run = s->crun = 0; s->rn = s->rlate = 0; s->cref = kmin <= FSTEP_MAX ? s->cref_at[kmin] : 0;
        s->hitch_fails = 0;   // one cap's hitches do not use up the next one's allowance
    }
    // A frame the screen showed on time while its cost did not fit (absorbed by present snapshots) is not what the hold costs: the probes would take it for the scene that held kmin
    int fits = !scr || cost <= held;
    if (held <= kmin) { s->run += shown; s->cn = 0; if (!grace) { s->rn++; s->rlate += late; } if (!late && fits && s->probe != 1) s->crun = s->crun > 0 ? s->crun + (cost - s->crun) / 32 : cost; }
    else {
        s->run = 0; s->rn = s->rlate = 0;
        if (held != s->ck) { s->ck = held; s->cn = 0; }
        if (!late && fits) { s->ccur = s->cn ? s->ccur + (cost - s->ccur) / 8 : cost; s->cn++; if (s->cn >= FSTEP_RUN && !grace && held <= FSTEP_MAX) { s->kheld[held] = s->ccur; s->tgfail[held] = 0; } }
    }
    int rheld = 2 * s->rlate <= s->rn;   // the run at kmin was on time more often than not
    int lfail = s->trial == 1 && !grace && held == kmin && s->tn && 2 * s->tlate > s->tn;   // the last frame of a trial to kmin late more often than not: it failed
    if ((s->late_run >= 3 || lfail) && s->k < kmax) {   // step up to the cost, at least one vsync
        int want = (int)ceil(cost - 0.05);
        if (want < s->k + 1) want = s->k + 1;
        if (want > kmax) want = kmax;
        // A trial back to kmin whose late frames all cost what fits kmin, and that had few of them besides this run (FSTEP_HITCH_LATE), failed on hitches (a load, a stall), not on the scene's cost: the hold-off stays as it was, up to
        // FSTEP_HITCH_MAX such failures in a row. Trials to a longer hold, probes, trials by kheld (Ocarina of Time's 20 -> 30 fps) and GPU-fit trials (counted in gfail)
        // double it as before
        int hitch = s->trial && !s->probe && !s->mtrial && !s->gtrial && held == kmin && s->late_cmax <= FSTEP_FIT * held && s->tlate - s->late_run <= FSTEP_HITCH_LATE && s->hitch_fails < FSTEP_HITCH_MAX;
        if (hitch) { s->failed++; s->trial = 0; s->hitch_fails++; }
        else if (s->trial || (s->probe == 2 && (s->since < hold_max || !rheld))) { s->failed++; s->trial = 0; s->hold = s->hold * 2 < hold_max ? s->hold * 2 : hold_max; s->hitch_fails = 0;
            if (s->mtrial && ++s->mfail >= 3 && held <= FSTEP_MAX) s->kheld[held] = 0;   // (three failed trials by kheld: that hold's cost is no longer the scene's)
            if (s->gtrial) s->gfail++;
            if (s->tgt && held <= FSTEP_MAX) { s->tgfail[held]++; s->tfail_at[held] = s->now; } }
        else if (s->probe == 2) s->hold = hold0;   // the probe's run at kmin lasted
        if (held == kmin && s->run >= hold_max && rheld) s->cref = s->crun;   // kmin held 8 s at this cost
        s->k = want; s->ups++; s->late_run = s->fit_run = s->gfit_run = s->gfit_miss = s->gtop_run = 0; s->since = 0; s->probe = 0; s->mtrial = s->gtrial = s->ttrial = 0; s->tg = s->tgt = s->tbad = 0;
    } else if (s->trial && !grace && !--s->trial) {   // the trial held (a probe's run at kmin is judged at the next step up)
        if (s->probe) { s->probe = 2; s->since = 0; } else s->hold = hold0;
        s->mtrial = s->mfail = s->gtrial = s->gfail = s->ttrial = 0; if (s->tgt && s->k <= FSTEP_MAX) s->tgfail[s->k] = 0; s->tgt = 0; s->hitch_fails = 0; }
    if (!late) {
        if (s->k <= kmin) s->fit_run = s->fit_miss = 0;
        else if (cost <= FSTEP_FIT * (s->k - 1)) s->fit_run++;
        else if (++s->fit_miss > FSTEP_MISS) s->fit_run = s->fit_miss = 0;
    }
    if (s->fit_run >= FSTEP_RUN && s->since >= s->hold && !s->trial) { s->k--; s->downs++; s->trial = FSTEP_RUN; s->fit_run = s->fit_miss = s->gfit_run = s->gfit_miss = 0; s->settle = FSTEP_SETTLE; }
    else if (s->k - 1 > kmin && s->k - 1 <= FSTEP_MAX && s->kheld[s->k - 1] > 0 && s->kheld[s->k - 1] <= FSTEP_FIT * (s->k - 1) && s->since >= s->hold && !s->trial && !s->probe) {
        s->k--; s->downs++; s->trial = FSTEP_RUN; s->mtrial = 1; s->grace = nvn_fstep_tgrace(s, s->k, kmin); s->fit_run = s->fit_miss = s->late_run = 0; s->pa = s->pb = 0; s->plate = s->ptotal = 0;   // back to a hold that held
    } else if (s->gfit_run >= FSTEP_RUN && s->k > kmin && s->gfail < 3 && s->since >= s->hold && !s->trial && !s->probe) {   // the GPU has fitted the shorter hold: try it, with a probe's grace
        s->k--; s->downs++; s->trial = FSTEP_RUN; s->gtrial = 1; s->grace = nvn_fstep_tgrace(s, s->k, kmin); s->fit_run = s->fit_miss = s->late_run = s->gfit_run = s->gfit_miss = 0; s->pa = s->pb = 0; s->plate = s->ptotal = 0;
    } else if (s->tnew && s->tgrace > 0 && s->gtop_run >= FSTEP_RUN && s->k - 1 > kmin && s->k - 1 <= FSTEP_MAX && s->since >= s->hold && !s->trial && !s->probe && !nvn_fstep_tstop(s, s->k - 1)) {
        // the GPU would fit the shorter hold at its top clock (tnew): try it with the long grace, whether or not the scene held it (not a GPU-fit trial: its failures stop
        // the long grace, not the GPU-fit trials, which then still come back from hold 3 once the frames fit)
        s->k--; s->downs++; s->trial = FSTEP_RUN; s->ttrial = 1; s->tnewtrials++; s->grace = nvn_fstep_tgrace(s, s->k, kmin);
        s->fit_run = s->fit_miss = s->late_run = s->gfit_run = s->gfit_miss = s->gtop_run = 0; s->pa = s->pb = 0; s->plate = s->ptotal = 0;
    } else if ((slow || nvn_fstep_same(s)) && s->k > kmin && s->since >= s->hold && !s->trial
               && !(s->probe_near && s->gpu > 0 && s->gpu * s->gpu / s->k > (s->k - 1 > kmin ? (s->near_fit_off ? FSTEP_FIT : FSTEP_NEAR_FIT) * (s->k - 1) : FSTEP_FIT * kmin))) {   // probe: straight to kmin, so the controller sees the whole deadline
        // probe_near's headroom rule: a governor that sizes the clock to the busy share can raise it at most in proportion to the idle share, so the GPU's time at hold k, gpu, can
        // fall to about gpu * (gpu / k) at best; a probe whose target that does not fit is not made (Ocarina of Time on a Galaxy Z Fold 8: at hold 2 with a GPU of 1.6 vsyncs a
        // probe to 1 failed every time and doubled the hold-off, which then kept hold 3 longer)
        s->k = s->probe_near && s->k - 1 > kmin ? s->k - 1 : kmin; s->downs++; s->probes++; s->trial = FSTEP_RUN; s->probe = 1; s->grace = nvn_fstep_tgrace(s, s->k, kmin); s->fit_run = s->late_run = 0; s->pa = s->pb = 0; s->plate = s->ptotal = 0;
    }
    s->step = s->k + (s->debt >= 1 && s->k < FSTEP_MAX);
    s->paid += s->step > s->k;
    return s->step;
}

// The GPU clock follows the hold too: Breath of the Wild on an iPhone went from 14.4 to about 25 ms of GPU time a frame at hold 2 with the same 4,900 draws, the CPU still on its
// performance cores, so its cost never fitted hold 1 again. The GPU counts as slowed (another reason for nvn_fstep_next's slow) when its time at a hold above kmin is at least
// FSTEP_GPU_SLOW times its time at kmin and it alone does not fit k − 1 (a GPU at 3 ms of 17, 6 times its time on a loading screen, is not what holds a frame back).
// The reference is not updated during a probe's grace (its frames run while the clock is still rising). The probe's frames after the grace ran at kmin with the clock up:
// they move the reference half-way each, so a scene that is heavy at every hold stops calling the GPU slowed after a failed probe. Tears of the Kingdom on the Y700 kept a
// reference from its title screen (0.1 vsyncs), so its open world (GPU 1.4-1.9 vsyncs at every hold) was "15 times slower" for good: 27 probes in 66 s, each a burst of
// late frames that sent the hold to 3 (20 fps), and every trial back from 3 to 2 that held set the hold-off back to 1 s. It is kept for each kmin: one
// learned at a 30 fps cap is the GPU time at the clock that cap left (lowered for 30 fps, and under the thermal cap for the heat), and taken as hold 1's after the cap rose it
// made the same work at hold 2 look not slowed.
// A reference that does not fit kmin itself (a mean above kmin: the GPU's time at kmin, at the clock kmin raised, is more than the hold, which no pacing sustains) rules the probe out:
// whatever the longer hold added, kmin was not held without it. Ocarina of Time on the Odin 3 (GPU clock at its maximum at every hold) costs 1.2-2.1 vsyncs of GPU time
// in Kokiri Forest by where the camera looks; a failed probe left a reference of 1.3, the heavy views were 1.5 times it, and each turn of the camera started another
// probe: 60-90 late frames at hold 1, then hold 3 (4 probes in 17 s, 358 late frames of 900; t_fstep: the view case)
// With probe_near (Android) a probe from a hold two or more above kmin goes to the next shorter hold, so the reference is held against that hold, not kmin: it says
// whether the GPU fits the probe's target. Ocarina of Time on a warm Galaxy S25 Edge: one failed probe from hold 2 to 1 left a reference of about 1.2 vsyncs, a heat dip
// later stepped to hold 3, where the governor took the GPU clock down to 525-607 MHz (2.1-2.25 vsyncs a frame), and with the reference above kmin no probe to hold 2
// was made again: 20 fps for the remaining 8-11 minutes of a 15-minute run, with the thermal cap back at 1,200 MHz (t_fstep: the warm-phone case)
#define FSTEP_GPU_SLOW 1.3    // GPU time at the longer hold against its time at kmin
#define FSTEP_GPU_N 30        // frames at the longer hold before the comparison counts
typedef struct { double ref, cur; int n, k, kmin, near_ref; double ref_at[FSTEP_MAX + 1]; } nvn_fgpu;   // GPU time at kmin (running mean; of every kmin in ref_at); at the current hold k and its frame count; the kmin ref belongs to;
                                                                                                  // near_ref: set by the caller (kept), as nvn_fstep.probe_near
static inline int nvn_fgpu_slow(const nvn_fgpu *g) { return g->n >= FSTEP_GPU_N && g->ref > 0 && g->ref <= (g->near_ref && g->k - 1 > g->kmin ? g->k - 1 : g->kmin) && g->cur >= FSTEP_GPU_SLOW * g->ref && g->cur > FSTEP_FIT * (g->k - 1); }
// What nvn_fstep_gpu_slowed's probe takes: 0 no probe's trial, 1 a probe's trial in its grace, 2 a probe's trial after it
static inline int nvn_fstep_probe_phase(const nvn_fstep *s) { return s->probe != 1 ? 0 : s->grace > 0 ? 1 : 2; }
// Once per frame, with the frame's GPU time (vsyncs; 0 = none finished yet), the hold it ran under and the probe phase (nvn_fstep_probe_phase)
static inline int nvn_fstep_gpu_slowed(nvn_fgpu *g, double gpu, int k, int kmin, int probe) {
    if (kmin < 1 || kmin > FSTEP_MAX) return 0;   // no reference slot for it (hle_nvn.c passes 1 or 2)
    g->ref = g->ref_at[kmin]; g->kmin = kmin;
    if (gpu <= 0) return nvn_fgpu_slow(g);
    if (k <= kmin) { if (probe != 1) g->ref = g->ref_at[kmin] = g->ref > 0 ? g->ref + (gpu - g->ref) / (probe == 2 ? 2 : 32) : gpu; g->n = 0; g->k = k; return 0; }
    if (k != g->k) { g->k = k; g->n = 0; }
    g->cur = g->n ? g->cur + (gpu - g->cur) / 8 : gpu; g->n++;
    return nvn_fgpu_slow(g);
}
