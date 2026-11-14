// Android implementations of the Darwin stand-ins declared in compat/dm_android.h, and the x18 thread-slot block (dm.h dm_slot_get). The Linux host (dm/linux) builds
// it too, with its own thread slots.
#include "dm.h"
#include <dirent.h>
#include <ftw.h>
#include <dispatch/dispatch.h>
#include <CommonCrypto/CommonDigest.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <linux/fs.h>
#include <CoreFoundation/CoreFoundation.h>
// The real Linux calls under the names compat/dm_android.h redirects
#undef getxattr
#undef setxattr
#undef removexattr

#ifndef DM_LINUX   // Linux with glibc: the slots are TLS of the host (dm/linux/plat_linux.c)
// ---- Thread slots ----
// One block per guest thread: a 4 KB page whose last 32 bytes are the slots (K18, KTLS, KTLR, KGATE in that order), then SCS_SIZE bytes of shadow
// call stack. x18 points at the start of the shadow call stack, so slot k is at x18 - 32 + 8k, and system code built with ShadowCallStack (which
// pushes return addresses at [x18] and moves x18 up and back) finds a stack of its own there. A guard page follows it.
#define SLOT_PAGE 4096
#define SCS_SIZE (16 * 1024)
#define SLOT_COUNT 4
typedef struct { uint8_t *map; size_t len; uint64_t bionic_x18; } slot_block;
static __thread slot_block t_blk;
static pthread_key_t g_blk_key; static pthread_once_t g_blk_once = PTHREAD_ONCE_INIT;
static inline uint64_t *slots(void) { return t_blk.map ? (uint64_t *)(t_blk.map + SLOT_PAGE - 8 * SLOT_COUNT) : NULL; }
// Thread exit: x18 goes back to the value bionic gave the thread before the block is unmapped (code that runs after this destructor may use the shadow call stack)
static void blk_free(void *p) {
    (void)p;
    if (!t_blk.map) return;
    __asm__ volatile("mov x18, %0" :: "r"(t_blk.bionic_x18) : "memory");
    munmap(t_blk.map, t_blk.len); t_blk.map = NULL;
}
static void blk_key_init(void) { if (pthread_key_create(&g_blk_key, blk_free)) die("cannot create the slot block key"); }
void dm_slots_thread_init(void) {
    if (!t_blk.map) {
        pthread_once(&g_blk_once, blk_key_init);
        size_t len = SLOT_PAGE + SCS_SIZE + 4096;
        uint8_t *m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m == MAP_FAILED) die("thread slot block: mmap failed");
        if (mprotect(m + SLOT_PAGE + SCS_SIZE, 4096, PROT_NONE)) die("thread slot block: guard page");
        uint64_t x18; __asm__ volatile("mov %0, x18" : "=r"(x18));
        t_blk.map = m; t_blk.len = len; t_blk.bionic_x18 = x18;
        pthread_setspecific(g_blk_key, m);   // only so that the destructor runs
    }
    __asm__ volatile("mov x18, %0" :: "r"(t_blk.map + SLOT_PAGE) : "memory");
}
void dm_slots_x18_restore(void) { if (t_blk.map) __asm__ volatile("mov x18, %0" :: "r"(t_blk.bionic_x18) : "memory"); }
int64_t dm_slot_keypage_value(pthread_key_t k) { return -8 * SLOT_COUNT + 8 * (int64_t)k; }
void *dm_slot_get(pthread_key_t k) { uint64_t *s = slots(); return s ? (void *)(uintptr_t)s[k] : NULL; }
void dm_slot_set(pthread_key_t k, const void *v) {
    if (!t_blk.map) dm_slots_thread_init();
    slots()[k] = (uint64_t)(uintptr_t)v;
}
#endif

// ---- Quality of service: a nice value on the calling thread ----
static __thread qos_class_t t_qos;
int dm_android_qos_set(qos_class_t q) { t_qos = q; setpriority(PRIO_PROCESS, (id_t)gettid(), dm_qos_nice(q)); return 0; }
qos_class_t dm_android_qos_get(void) { return t_qos ? t_qos : QOS_CLASS_DEFAULT; }

// ---- The frame's deadline: an Android Performance Hint session (ADPF) for the threads that do each frame's work ----
// The time-constraint declaration (hle_nvn.c set_time_constraint, through compat mach/thread_policy.h) is how the host tells the system the frame's deadline: the present
// period, or the cap's while the frame step holds frames longer for late ones (rt_period_ns: a longer hold declared parked an iPhone on its efficiency cores). Android had
// no taker for it, and the governor sized the clock to the hold: Breath of the Wild 1.9.0 on an Ayn Odin 2 (Snapdragon 8 Gen 2, Android 13) at hold 3 kept its Presentation
// thread about 80% busy, 86% of it on the gold cores, while the Cortex-X3 (cpu7) sat idle 88% of the time, and when the mtl_addr fix cut the replay from 117 M to 97 M cycles
// a frame its CPU time stayed at 37 ms and the game at 20 fps: the clock fell to fill the same 50 ms. A hint session (APerformanceHint, API 33, through dlsym: the build targets
// API 29) holds every present's report of the frame's work (gfx_vk.c hint_frame: present to present without the pacing sleeps) against a target, and the power HAL raises the
// threads' clock and placement (uclamp.min) while the work runs over it. The target: under the frame step, the cost its fit trial needs to try the next hold down (gfx_vk.c
// hint_frame, from hle_nvn.c nvn_fstep_hold), so it follows the hold and the boost scales with the gap to the next hold; otherwise the declared period. Its threads: the ones
// that acquired, replayed or presented the frame, and the replay's completion thread (gfx_vk.c vk-done: the write-backs the replay waits for). Android 13 has no
// APerformanceHint_setThreads, so frames that run on another set for HINT_STABLE presents in a row get a new session. No thread is pinned to a core.
// A new session is made before the one it replaces is closed, so a make that fails (a binder or power HAL error, a thread that exited before the system took it) leaves the old
// one and its threads in place. Before, the old one was closed first: one failure left the process with no session for the rest of the run, as frames on the same threads never
// asked again. A failed make is tried again after HINT_STABLE presents, doubling, and after HINT_TRIES failures in a row no more (logged). A power HAL without hint sessions
// reports a preferred rate of −1 (HintManagerService isHalSupported), and no session is tried then.
// Thermal: while the device is serious or worse (dm_thermal_now() >= 2) or the frame step's thermal cap holds 30 fps, the session is closed (Android 13 has no pause, and how long a
// session keeps its boost without reports is up to the power HAL). At severe the cap gives Breath of the Wild a 33.3 ms hold and a 28.3 ms target that its 43 ms of work overruns,
// so the hint would keep asking for full boost (the Cortex-X3) exactly while the cap is meant to let the device cool. A session comes back after HINT_STABLE presents in a row
// with neither.
// DM_VK_ADPF=0: no session, the declaration fails as before; DM_VK_ADPF_HOLD=0: the target is the declared period whatever the hold (rt_period_ns: kmin's while the hold is
// raised); DM_VK_ADPF_THERMAL=0: the session stays through thermal mitigation; DM_VK_ADPF_DONE=0: the completion thread is left out; DM_VK_ADPF_LOG=1: the reported work every
// 300 presents and each change of the target
#include <dlfcn.h>
#ifdef __ANDROID__
#include <android/api-level.h>
#else
static int android_get_device_api_level(void) { return 0; }   // Linux: no libandroid.so, so neither a hint session nor the thermal manager is found
#endif
typedef struct APerformanceHintManager APerformanceHintManager;
typedef struct APerformanceHintSession APerformanceHintSession;
static struct {
    APerformanceHintManager *(*manager)(void);
    APerformanceHintSession *(*create)(APerformanceHintManager *, const int32_t *, size_t, int64_t);
    int64_t (*rate)(APerformanceHintManager *);
    int (*target)(APerformanceHintSession *, int64_t);
    int (*report)(APerformanceHintSession *, int64_t);
    void (*close)(APerformanceHintSession *);
} g_ph;
enum { HINT_MAXT = 6, HINT_STABLE = 60, HINT_TRIES = 6 };
static pthread_mutex_t g_hint_mu = PTHREAD_MUTEX_INITIALIZER;   // everything below; a session is not thread-safe
static int g_hint_on;   // 0 = not looked at yet (nothing declared), 1 = sessions can be made, −1 = none (DM_VK_ADPF=0, Android before 13, no manager, a power HAL without
                        // hint sessions, or HINT_TRIES failed makes in a row with no session left)
static APerformanceHintManager *g_hint_mgr; static APerformanceHintSession *g_hint;
static int64_t g_hint_target, g_hint_decl, g_hint_hold; static uint64_t g_hint_comp;   // the session's target; the declared period and computation; the frame step's target (0 = none); ns
static int32_t g_hint_done;   // the replay's completion thread (0 = none yet)
static int32_t g_hint_t[HINT_MAXT]; static int g_hint_nt;   // the session's threads (sorted)
static int32_t g_hint_w[HINT_MAXT]; static int g_hint_nw, g_hint_same;   // another set the frames ran on, and for how many presents in a row
static int g_hint_fail, g_hint_wait;   // failed makes in a row; presents before the next try (−1 = no more tries)
static int g_hint_hot, g_hint_cool;   // 1: the session is closed for heat; presents in a row since the device was last serious or capped
static unsigned g_hint_made, g_hint_nfail, g_hint_err, g_hint_nhot;
static int hint_env_on(const char *name) { const char *e = getenv(name); return !(e && !strcmp(e, "0")); }
static int hint_done_on(void) { static int v = -1; if (v < 0) v = hint_env_on("DM_VK_ADPF_DONE"); return v; }
static int hint_hold_on(void) { static int v = -1; if (v < 0) v = hint_env_on("DM_VK_ADPF_HOLD"); return v; }
static int hint_thermal_on(void) { static int v = -1; if (v < 0) v = hint_env_on("DM_VK_ADPF_THERMAL"); return v; }
static int hint_log_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_ADPF_LOG"); v = e && atoi(e); } return v; }
static int hint_load(void) {   // under g_hint_mu
    if (g_hint_on) return g_hint_on > 0;
    g_hint_on = -1;
    if (!hint_env_on("DM_VK_ADPF")) { LOG("[hint] DM_VK_ADPF=0: no performance hint session, the frame's deadline is declared to no one"); return 0; }
    void *h = dlopen("libandroid.so", RTLD_NOW);
#define PH_SYM(f, name) g_ph.f = h ? (__typeof__(g_ph.f))dlsym(h, name) : NULL
    PH_SYM(manager, "APerformanceHint_getManager"); PH_SYM(create, "APerformanceHint_createSession"); PH_SYM(rate, "APerformanceHint_getPreferredUpdateRateNanos");
    PH_SYM(target, "APerformanceHint_updateTargetWorkDuration"); PH_SYM(report, "APerformanceHint_reportActualWorkDuration"); PH_SYM(close, "APerformanceHint_closeSession");
#undef PH_SYM
    if (!g_ph.manager || !g_ph.create || !g_ph.rate || !g_ph.target || !g_ph.report || !g_ph.close) {
        LOG("[hint] no performance hint session: Android API %d has no APerformanceHint (it came with 33)", android_get_device_api_level()); return 0; }
    if (!(g_hint_mgr = g_ph.manager())) { LOG("[hint] no performance hint session: APerformanceHint_getManager returned none"); return 0; }
    int64_t r = g_ph.rate(g_hint_mgr);
    if (r < 0) { LOG("[hint] no performance hint session: the power HAL has none (preferred report rate %lld)", (long long)r); return 0; }
    g_hint_on = 1; return 1;
}
static int hint_add(int32_t *t, int n, int32_t tid) {   // into a sorted set of at most HINT_MAXT threads
    if (tid <= 0 || n >= HINT_MAXT) return n;
    int i = 0; while (i < n && t[i] < tid) i++;
    if (i < n && t[i] == tid) return n;
    memmove(t + i + 1, t + i, (size_t)(n - i) * sizeof *t); t[i] = tid; return n + 1;
}
static int hint_eq(const int32_t *a, int na, const int32_t *b, int nb) { return na == nb && !memcmp(a, b, (size_t)na * sizeof *a); }
// Under g_hint_mu: a session for these threads in place of the one there is, closed only once the new one exists. A failed make keeps the old session and its threads, and the
// next try waits HINT_STABLE presents, doubling (hint_due); after HINT_TRIES failures in a row there are no more, and with no session left the declaration fails from then on
static int hint_make(const int32_t *t, int n) {
    if (!n || g_hint_target <= 0) return 0;
    char ids[96]; size_t o = 0; ids[0] = 0;
    for (int i = 0; i < n && o < sizeof ids - 16; i++) o += snprintf(ids + o, sizeof ids - o, "%s%d", i ? ", " : "", t[i]);
    APerformanceHintSession *s = g_ph.create(g_hint_mgr, t, (size_t)n, g_hint_target);
    if (!s) {
        if (++g_hint_fail >= HINT_TRIES) {
            g_hint_wait = -1; if (!g_hint) g_hint_on = -1;
            LOG("[hint] the system made no performance hint session for threads %s (target %.3f ms) %d times in a row: no more tries, %s", ids, g_hint_target / 1e6, g_hint_fail,
                g_hint ? "the session there is keeps its threads" : "the frame's deadline is declared to no one from here on");
        } else {
            g_hint_wait = HINT_STABLE << (g_hint_fail - 1);
            if (g_hint_nfail++ < 8) LOG("[hint] the system made no performance hint session for threads %s (target %.3f ms; failure %d of %d in a row), %s; next try in %d presents",
                                         ids, g_hint_target / 1e6, g_hint_fail, HINT_TRIES, g_hint ? "the session there is stays" : "none for now", g_hint_wait);
        }
        return 0;
    }
    if (g_hint) g_ph.close(g_hint);
    g_hint = s; memcpy(g_hint_t, t, (size_t)n * sizeof *t); g_hint_nt = n; g_hint_fail = g_hint_wait = g_hint_same = 0;
    if (g_hint_made++ < 8) LOG("[hint] performance hint session made for threads %s: target %.3f ms, preferred report rate %.3f ms (DM_VK_ADPF=0 turns it off)%s",
                               ids, g_hint_target / 1e6, g_ph.rate(g_hint_mgr) / 1e6, g_hint_made == 8 ? "; further sessions not logged" : "");
    return 1;
}
static int hint_due(void) { if (g_hint_wait > 0) g_hint_wait--; return !g_hint_wait; }   // under g_hint_mu, once per present that wants a make: whether a failed make's wait is over
static void hint_retarget(void) {   // under g_hint_mu: the frame step's target while it decides the hold, else the declared period
    int64_t t = g_hint_hold > 0 ? g_hint_hold : g_hint_decl;
    if (t == g_hint_target) return;
    static unsigned nlog;
    if (hint_log_on() && g_hint_target > 0 && nlog++ < 40) LOG("[hint] target %.3f → %.3f ms (%s)%s", g_hint_target / 1e6, t / 1e6, g_hint_hold > 0 ? "the frame step's hold" : "the declared period", nlog == 40 ? "; further changes not logged" : "");
    g_hint_target = t;
    if (g_hint && t > 0) { int r = g_ph.target(g_hint, t); if (r && g_hint_err++ < 4) LOG("[hint] setting the target to %.3f ms failed (%d)", t / 1e6, r); }
}
// compat mach/thread_policy.h thread_policy_set: the declaring thread's period becomes the target (unless the frame step's is in force); the first declaration makes the session
// (with the completion thread when it exists). Accepted while the session is closed for heat: it comes back with the declared deadline
kern_return_t dm_android_rt_declare(mach_port_t t, uint64_t period_ns, uint64_t comp_ns) {
    pthread_mutex_lock(&g_hint_mu);
    if (period_ns && hint_load()) {
        g_hint_comp = comp_ns; g_hint_decl = (int64_t)period_ns; hint_retarget();
        if (!g_hint && !g_hint_hot && !g_hint_made && !g_hint_fail) { int32_t s[HINT_MAXT]; int n = hint_add(s, 0, (int32_t)t); if (hint_done_on()) n = hint_add(s, n, g_hint_done); hint_make(s, n); }
    }
    kern_return_t kr = g_hint || (g_hint_on > 0 && g_hint_hot) ? KERN_SUCCESS : KERN_FAILURE;
    pthread_mutex_unlock(&g_hint_mu);
    return kr;
}
int dm_android_rt_declared(mach_port_t t, uint64_t *period_ns, uint64_t *comp_ns) {   // thread_policy_get: whether thread t is in a session, and what was declared
    pthread_mutex_lock(&g_hint_mu);
    int in = 0; for (int i = 0; g_hint && i < g_hint_nt; i++) in |= g_hint_t[i] == (int32_t)t;
    if (in) { *period_ns = (uint64_t)g_hint_decl; *comp_ns = g_hint_comp; }
    pthread_mutex_unlock(&g_hint_mu);
    return in;
}
void dm_android_hint_thread(int32_t tid) { pthread_mutex_lock(&g_hint_mu); g_hint_done = tid; pthread_mutex_unlock(&g_hint_mu); }   // gfx_vk.c chunk_thread

// ---- Experiment: where the replay runs (DM_VK_PIN, DM_VK_PIN_DONE, DM_VK_UCLAMP_MIN), off by default ----
// Breath of the Wild 1.9.0 on an Ayn Odin 2: while adb's screenrecord ran, the replay thread's CPU time fell about 27% (10.8 ms a frame, fitted over 14 runs) at the same
// draws, so the clock, the core or the memory bus the thread gets, not its work, decides about a quarter of the replay; the performance hint's full-boost requests did not get
// it. These test which. DM_VK_PIN=<hex CPU mask>: each thread that replays is pinned there (sched_setaffinity; the Odin 2's Cortex-X3 is cpu7, 0x80), DM_VK_PIN_DONE=<mask>
// the completion thread; DM_VK_UCLAMP_MIN=<0..1024>: their util clamp minimum (sched_setattr with SCHED_FLAG_UTIL_CLAMP_MIN, policy, priority and nice kept), which under
// EAS/WALT raises the clock the thread is given and the capacity of the core it is placed on. Each result is logged once per thread; a refusal (EPERM, EINVAL) changes nothing
#include <sched.h>
#include <sys/syscall.h>
#include <sys/resource.h>
struct dm_sched_attr { uint32_t size, policy; uint64_t flags; int32_t nice; uint32_t prio; uint64_t runtime, deadline, period; uint32_t util_min, util_max; };   // uapi sched_attr, SCHED_ATTR_SIZE_VER1
void dm_android_place_thread(int done) {   // gfx_vk.c, once per thread: done = the replay's completion thread
    int32_t tid = gettid(); const char *who = done ? "completion" : "replay";
    const char *pm = getenv(done ? "DM_VK_PIN_DONE" : "DM_VK_PIN");
    if (pm && *pm) {
        unsigned long m = strtoul(pm, NULL, 16); cpu_set_t cs; CPU_ZERO(&cs);
        for (int c = 0; c < (int)(8 * sizeof m) && c < CPU_SETSIZE; c++) if (m >> c & 1) CPU_SET(c, &cs);
        int r = m ? sched_setaffinity(tid, sizeof cs, &cs) : -1;
        LOG("[place] %s thread %d: CPU mask 0x%lx %s", who, tid, m, !m ? "is empty, not pinned" : r ? strerror(errno) : "set");
    }
    const char *um = getenv("DM_VK_UCLAMP_MIN");
    if (um && *um) {
        struct dm_sched_attr a; memset(&a, 0, sizeof a); a.size = sizeof a;
        a.flags = 0x08 | 0x10 | 0x20;   // SCHED_FLAG_KEEP_POLICY | SCHED_FLAG_KEEP_PARAMS | SCHED_FLAG_UTIL_CLAMP_MIN
        errno = 0; int nv = getpriority(PRIO_PROCESS, (id_t)tid); a.nice = errno ? 0 : nv;   // the permission check compares the nice given with the thread's: give its own
        int v = atoi(um); a.util_min = (uint32_t)(v < 0 ? 0 : v > 1024 ? 1024 : v); a.util_max = 1024;
        long r = syscall(SYS_sched_setattr, tid, &a, 0);
        LOG("[place] %s thread %d: util clamp minimum %u %s (nice %d)", who, tid, a.util_min, r ? strerror(errno) : "set", a.nice);
    }
}
int dm_thermal_now(void);
// gfx_vk.c hint_frame, on the present thread once per present: the frame's work (0 = none to report), the other threads that acquired or replayed it, the frame step's target
// (0 = it does not decide the hold) and whether its thermal cap holds 30 fps
void dm_android_hint_frame(uint64_t work_ns, const int32_t *tids, int n, uint64_t hold_ns, int thermal) {
    pthread_mutex_lock(&g_hint_mu);
    if (g_hint_on > 0) { g_hint_hold = hint_hold_on() ? (int64_t)hold_ns : 0; hint_retarget(); }
    if (g_hint_on > 0 && g_hint_target > 0) {
        int32_t w[HINT_MAXT]; int nw = hint_add(w, 0, gettid());
        for (int i = 0; i < n; i++) nw = hint_add(w, nw, tids[i]);
        if (hint_done_on()) nw = hint_add(w, nw, g_hint_done);
        int th = hint_thermal_on() ? dm_thermal_now() : -1;
        if (hint_thermal_on() && (thermal || th >= 2)) {   // thermally serious, or the frame step's thermal cap: no session until that has passed
            g_hint_cool = 0;
            if (!g_hint_hot) {
                g_hint_hot = 1; g_hint_same = 0;
                if (g_hint) { g_ph.close(g_hint); g_hint = NULL; }
                if (g_hint_nhot++ < 16) LOG("[hint] thermal state %d%s: the performance hint session is closed, no boost until neither holds for %d presents (DM_VK_ADPF_THERMAL=0 keeps it)%s",
                                            th, thermal ? ", the frame step's thermal cap holds 30 fps" : "", HINT_STABLE, g_hint_nhot == 16 ? "; further closes not logged" : "");
            }
        } else if (g_hint_hot) {
            if (++g_hint_cool >= HINT_STABLE) {
                g_hint_hot = 0; g_hint_fail = g_hint_wait = 0;
                if (g_hint_nhot < 16) LOG("[hint] thermal state %d and no thermal cap for %d presents: a performance hint session again", th, HINT_STABLE);
                hint_make(w, nw);
            }
        } else if (!g_hint) { if (hint_due()) hint_make(w, nw); }   // none: the declaration's make failed, or the last one did
        else if (!hint_eq(w, nw, g_hint_t, g_hint_nt)) {   // a frame on other threads than the session's: a new session once that holds
            if (hint_eq(w, nw, g_hint_w, g_hint_nw)) { if (g_hint_same < HINT_STABLE) g_hint_same++; } else { memcpy(g_hint_w, w, (size_t)nw * sizeof *w); g_hint_nw = nw; g_hint_same = 1; }
            if (g_hint_same >= HINT_STABLE && hint_due()) hint_make(w, nw);
        } else g_hint_same = 0;
        if (g_hint && work_ns) {
            int r = g_ph.report(g_hint, (int64_t)work_ns);
            if (r && g_hint_err++ < 4) LOG("[hint] reporting the frame's work failed (%d)", r);
            static unsigned nf, over; static double sum, mx;
            if (hint_log_on()) { double ms = work_ns / 1e6; sum += ms; if (ms > mx) mx = ms; over += (int64_t)work_ns > g_hint_target;
                if (++nf == 300) { LOG("[hint] frame work reported over the last 300 presents: mean %.2f ms, max %.2f ms, %u over the target (now %.3f ms)", sum / nf, mx, over, g_hint_target / 1e6); nf = over = 0; sum = mx = 0; } }
        }
    }
    pthread_mutex_unlock(&g_hint_mu);
}

// ---- Thermal status: hle_nvn.c dm_thermal_now, in iOS's NSProcessInfo thermalState terms (0 nominal, 1 fair, 2 serious, 3 critical; −1 unknown) ----
// AThermal (API 30) through dlsym. NONE 0; LIGHT and MODERATE 1 (Android: throttling "where UX is not largely impacted"); SEVERE 2, where the frame step's thermal cap holds
// 30 fps (hle_nvn.c thermal_capped); CRITICAL and above 3. Probes and the GPU-slowed signal stay allowed up to 1. Read at most once a second, as each read is a binder call
// (thermal_capped asks once a second, the performance hint every present, the probe gates and the power lines too). From 2 the performance hint closes its session
// (dm_android_hint_frame), so it does not run the Odin 2's Cortex-X3 flat out while the cap lets the device cool.
// DM_VK_THERMAL=0: no thermal state (−1) as before; DM_THERMAL_CAP=0 keeps the state and turns only the cap off
typedef struct AThermalManager AThermalManager;
static AThermalManager *g_th_mgr; static int (*g_th_status)(AThermalManager *);
static void thermal_load(void) {
    const char *e = getenv("DM_VK_THERMAL");
    if (e && !strcmp(e, "0")) { LOG("[thermal] DM_VK_THERMAL=0: no thermal state (the frame step's thermal cap stays off)"); return; }
    void *h = dlopen("libandroid.so", RTLD_NOW);
    AThermalManager *(*acquire)(void) = h ? (AThermalManager *(*)(void))dlsym(h, "AThermal_acquireManager") : NULL;
    g_th_status = h ? (int (*)(AThermalManager *))dlsym(h, "AThermal_getCurrentThermalStatus") : NULL;
    if (acquire && g_th_status) g_th_mgr = acquire();
    if (!g_th_mgr) { g_th_status = NULL; LOG("[thermal] no thermal state: Android API %d has no AThermal manager (it came with 30)", android_get_device_api_level()); }
}
int dm_thermal_now(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT; static _Atomic int v = -1; static _Atomic uint64_t last; static int was = -2; static unsigned nlog;
    pthread_once(&once, thermal_load);
    if (!g_th_status) return -1;
    uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC), l = atomic_load(&last);
    if ((!l || now - l >= 1000000000ull) && atomic_compare_exchange_strong(&last, &l, now)) {   // one reader at a time; the others take the last state
        static const char *const name[] = {"none", "light", "moderate", "severe", "critical", "emergency", "shutdown"};
        int s = g_th_status(g_th_mgr), m = s < 0 ? -1 : s == 0 ? 0 : s <= 2 ? 1 : s == 3 ? 2 : 3;
        atomic_store(&v, m);
        if (s != was && nlog++ < 64) LOG("[thermal] Android thermal status %d (%s): thermal state %d%s", s, s >= 0 && s <= 6 ? name[s] : "error", m, nlog == 64 ? "; further changes not logged" : "");
        was = s;
    }
    return atomic_load(&v);
}

// The usable stack, as Darwin's pthread_get_stackaddr_np / pthread_get_stacksize_np give it. Bionic's range for a pthread_create thread starts at its mapping, guard page(s) included:
// QueryMemory reported that unreadable page as the start of the main thread's stack, and Unity's conservative GC, which scans the whole stack it was told of, faulted on it
// at the first collection (Danganronpa V3, main+0x2194c4 under GC mark, an address 8 MB below sp).
// The range must stay readable for the thread's life: a guest thread must never attach to the Java VM, since ART then protects the page above the guard (front_text_input)
size_t dm_android_stack_info(void **top) {
    pthread_attr_t a; void *lo = NULL; size_t n = 0, guard = 0;
    if (!pthread_getattr_np(pthread_self(), &a)) { pthread_attr_getstack(&a, &lo, &n); pthread_attr_getguardsize(&a, &guard); pthread_attr_destroy(&a); }
    if (guard < n) { lo = (uint8_t *)lo + guard; n -= guard; }
    *top = (uint8_t *)lo + n; return n;
}

// ---- Signal context records (Linux arm64 sigcontext.__reserved: a list of {magic, size} records ending with magic 0) ----
static void *uc_record(ucontext_t *uc, uint32_t magic) {
    uint8_t *p = (uint8_t *)uc->uc_mcontext.__reserved, *e = p + sizeof uc->uc_mcontext.__reserved;
    while (p + 8 <= e) {
        uint32_t m = ((uint32_t *)p)[0], sz = ((uint32_t *)p)[1];
        if (!m || sz < 8) return NULL;
        if (m == magic) return p;
        if (m == 0x45585401) { p = (uint8_t *)(uintptr_t)*(uint64_t *)(p + 8); e = p + 0x10000; continue; }   // EXTRA_MAGIC: the list goes on elsewhere
        p += sz;
    }
    return NULL;
}
_STRUCT_ARM_NEON_STATE64 *dm_android_uc_neon(ucontext_t *uc) {
    uint8_t *r = uc_record(uc, 0x46508001);   // FPSIMD_MAGIC
    static __thread _STRUCT_ARM_NEON_STATE64 none;
    return r ? (_STRUCT_ARM_NEON_STATE64 *)(void *)(r + 8) : (memset(&none, 0, sizeof none), &none);
}
uint64_t dm_android_uc_esr(ucontext_t *uc) { uint8_t *r = uc_record(uc, 0x45535201); return r ? *(uint64_t *)(r + 8) : 0; }   // ESR_MAGIC

// ---- Sections (hle.h tables): the linker bounds each with __start_<name> / __stop_<name> ----
const struct mach_header_64 _mh_execute_header = {0};
#define SECT(n) extern const uint8_t __start_##n[] __attribute__((weak)), __stop_##n[] __attribute__((weak));
SECT(swhle) SECT(swforce) SECT(swnvn)
const void *dm_android_section(const char *name, unsigned long *size) {
    const uint8_t *b = NULL, *e = NULL;
    if (!strcmp(name, "swhle")) { b = __start_swhle; e = __stop_swhle; }
    else if (!strcmp(name, "swforce")) { b = __start_swforce; e = __stop_swforce; }
    else if (!strcmp(name, "swnvn")) { b = __start_swnvn; e = __stop_swnvn; }
    *size = b && e ? (unsigned long)(e - b) : 0;
    return *size ? b : NULL;
}

// ---- Files ----
// clonefile: Android's file systems (f2fs, ext4) have no copy-on-write clone, so the tree is copied, extended attributes included (hle_fs.c keeps
// guest names in one). Save data trees are small (megabytes)
static void copy_xattrs(const char *s, const char *d) {
    char names[1024]; ssize_t n = llistxattr(s, names, sizeof names);
    for (ssize_t i = 0; i < n; i += (ssize_t)strlen(names + i) + 1) {
        char v[1024]; ssize_t k = lgetxattr(s, names + i, v, sizeof v);
        if (k >= 0) lsetxattr(d, names + i, v, (size_t)k, 0);
    }
}
static int copy_file(const char *s, const char *d, mode_t mode) {
    int in = open(s, O_RDONLY | O_CLOEXEC); if (in < 0) return -1;
    int out = open(d, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode & 07777); if (out < 0) { int e = errno; close(in); errno = e; return -1; }
    char buf[65536]; ssize_t n; int r = 0;
    while ((n = read(in, buf, sizeof buf)) > 0) for (ssize_t o = 0; o < n;) { ssize_t w = write(out, buf + o, n - o); if (w < 0) { r = -1; goto done; } o += w; }
    if (n < 0) r = -1;
done: { int e = errno; close(in); if (close(out)) r = -1; errno = e; }
    if (!r) copy_xattrs(s, d);
    return r;
}
static int copy_tree(const char *s, const char *d) {
    struct stat st; if (lstat(s, &st)) return -1;
    if (!S_ISDIR(st.st_mode)) return S_ISREG(st.st_mode) ? copy_file(s, d, st.st_mode) : 0;
    if (mkdir(d, st.st_mode & 07777)) return -1;
    copy_xattrs(s, d);
    DIR *dir = opendir(s); if (!dir) return -1;
    struct dirent *e; int r = 0;
    while (!r && (e = readdir(dir))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char a[PATH_MAX], b[PATH_MAX];
        if (snprintf(a, sizeof a, "%s/%s", s, e->d_name) >= (int)sizeof a || snprintf(b, sizeof b, "%s/%s", d, e->d_name) >= (int)sizeof b) { errno = ENAMETOOLONG; r = -1; break; }
        r = copy_tree(a, b);
    }
    closedir(dir); return r;
}
int dm_android_clonefile(const char *src, const char *dst, int flags) {
    (void)flags;
    struct stat st; if (!lstat(dst, &st)) { errno = EEXIST; return -1; }
    return copy_tree(src, dst);
}
// RENAME_SWAP is renameat2's RENAME_EXCHANGE, which f2fs and ext4 have. sdcardfs refuses every rename flag with EINVAL; it holds external storage's
// Android/data on devices with a 4.x kernel (the Retroid Pocket 5 on Android 13), where scripts/bench_android.py keeps each run's save, so there every
// Commit failed ("atomic swap failed errno=22": Skyward Sword HD then crashed, Animal Crossing stopped). There the swap is three plain renames through
// <to>.swap: to → to.swap, from → to, to.swap → from. A kill after the first leaves no <to>, and dm_android_swap_recover, at the next mount, moves to.swap
// back: the version from before the swap, as if it had not begun. A kill after the second leaves to.swap beside the new <to>, and it is deleted. So a
// save still mounts whole, the old version or the new one, never a mix
static int rm_one(const char *p, const struct stat *st, int flag, struct FTW *f) { (void)st; (void)f; return flag == FTW_DP || flag == FTW_DNR ? rmdir(p) : unlink(p); }
static int remove_tree(const char *p) { return nftw(p, rm_one, 16, FTW_DEPTH | FTW_PHYS); }   // a symbolic link is removed, never followed
static int swap_side(const char *to, char *side, size_t n) {
    if (snprintf(side, n, "%s.swap", to) < (int)n) return 0;
    errno = ENAMETOOLONG; return -1;
}
int dm_android_swap_by_renames(const char *from, const char *to) {
    char side[PATH_MAX]; struct stat st;
    if (swap_side(to, side, sizeof side) || lstat(from, &st) || lstat(to, &st)) return -1;   // both must exist, as for RENAME_EXCHANGE (ENOENT)
    if (!lstat(side, &st) && remove_tree(side)) return -1;   // a swap that a kill cut short after its second rename left it: <to> is already the newer version
    if (rename(to, side)) return -1;
    if (rename(from, to)) { int e = errno; rename(side, to); errno = e; return -1; }   // put back as it was
    if (rename(side, from)) LOG("[RED] swap: %s holds the new version, but the old one stays in %s (errno=%d) until the next mount deletes it", to, side, errno);
    return 0;
}
void dm_android_swap_recover(const char *p) {
    char side[PATH_MAX]; struct stat st;
    if (swap_side(p, side, sizeof side) || lstat(side, &st)) return;
    if (lstat(p, &st)) {
        if (rename(side, p)) LOG("[RED] swap: %s is missing and moving %s back failed errno=%d", p, side, errno);
        else LOG("swap: %s was cut short by a kill before its second rename: the version from before it is back", p);
    } else if (remove_tree(side)) LOG("[RED] swap: cannot delete %s, which a swap cut short by a kill left (errno=%d)", side, errno);
    else LOG("swap: deleted %s, the version that a swap cut short by a kill had already replaced", side);
}
int dm_android_renamex_np(const char *from, const char *to, unsigned flags) {
    if (!(flags & RENAME_SWAP)) return rename(from, to);
    if (!syscall(SYS_renameat2, AT_FDCWD, from, AT_FDCWD, to, RENAME_EXCHANGE)) return 0;
    if (errno != EINVAL) return -1;
    static _Atomic int told;
    if (!atomic_exchange(&told, 1)) LOG("renameat2 RENAME_EXCHANGE: EINVAL for %s (sdcardfs has no rename flags): folders are swapped by plain renames through <folder>.swap", to);
    return dm_android_swap_by_renames(from, to);
}
// Darwin xattr names are free-form; Linux needs a namespace: the guest-name attribute (hle_fs.c FS_GUEST_NAME) goes in user.
static const char *xname(const char *n, char *buf, size_t sz) { if (!strncmp(n, "user.", 5)) return n; snprintf(buf, sz, "user.%s", n); return buf; }
ssize_t dm_android_getxattr(const char *p, const char *n, void *v, size_t s, uint32_t pos, int opt) {
    (void)pos; char b[256]; n = xname(n, b, sizeof b);
    return opt & XATTR_NOFOLLOW ? lgetxattr(p, n, v, s) : getxattr(p, n, v, s);
}
int dm_android_setxattr(const char *p, const char *n, const void *v, size_t s, uint32_t pos, int opt) {
    (void)pos; char b[256]; n = xname(n, b, sizeof b);
    return opt & XATTR_NOFOLLOW ? lsetxattr(p, n, v, s, 0) : setxattr(p, n, v, s, 0);
}
// sdcardfs has no extended attributes at all (EOPNOTSUPP), so no name there has one: the same answer as ENOATTR. hle_fs.c's set_guest_name removes the
// guest name from every entry a game creates or renames in its save, and took EOPNOTSUPP as a failure of the create itself
int dm_android_removexattr(const char *p, const char *n, int opt) {
    char b[256]; n = xname(n, b, sizeof b);
    int r = opt & XATTR_NOFOLLOW ? lremovexattr(p, n) : removexattr(p, n);
    if (r && errno == EOPNOTSUPP) errno = ENOATTR;
    return r;
}
#undef fcntl
int dm_android_fcntl(int fd, int cmd, ...) {
    va_list ap; va_start(ap, cmd); void *arg = va_arg(ap, void *); va_end(ap);
    if (cmd == F_GETPATH) {
        char link[64]; snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
        ssize_t n = readlink(link, arg, MAXPATHLEN - 1);
        if (n < 0) return -1;
        ((char *)arg)[n] = 0; return 0;
    }
    if (cmd == F_FULLFSYNC) return fsync(fd);
    return fcntl(fd, cmd, arg);
}

// ---- sysctl: only KERN_PROC_PID's start time (main.c), from /proc/self/stat's starttime (clock ticks since boot) ----
#undef sysctl
int dm_android_sysctl(int *mib, unsigned n, void *out, size_t *outn, void *in, size_t inn) {
    (void)in; (void)inn;
    if (n != 4 || mib[0] != CTL_KERN || mib[1] != KERN_PROC || mib[2] != KERN_PROC_PID || !out || *outn < sizeof(struct kinfo_proc)) { errno = ENOTSUP; return -1; }
    FILE *f = fopen("/proc/self/stat", "r"); if (!f) return -1;
    char buf[1024]; size_t k = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[k] = 0;
    char *p = strrchr(buf, ')'); if (!p) return -1;
    unsigned long long start = 0; int field = 2;
    for (char *t = strtok(p + 1, " "); t; t = strtok(NULL, " ")) if (++field == 22) { start = strtoull(t, NULL, 10); break; }
    struct timespec boot, real, mono; clock_gettime(CLOCK_BOOTTIME, &boot); clock_gettime(CLOCK_REALTIME, &real); (void)mono;
    double since_boot = (double)start / (double)sysconf(_SC_CLK_TCK);
    double up = boot.tv_sec + boot.tv_nsec / 1e9, wall = real.tv_sec + real.tv_nsec / 1e9 - (up - since_boot);
    struct kinfo_proc *kp = out; memset(kp, 0, sizeof *kp);
    kp->kp_proc.p_starttime.tv_sec = (time_t)wall; kp->kp_proc.p_starttime.tv_usec = (suseconds_t)((wall - (double)(time_t)wall) * 1e6);
    *outn = sizeof *kp; return 0;
}

// ---- dispatch semaphores ----
dispatch_time_t dispatch_time(dispatch_time_t when, int64_t delta) {
    if (when == DISPATCH_TIME_FOREVER) return when;
    uint64_t base = when ? when : clock_gettime_nsec_np(CLOCK_MONOTONIC);
    return delta < 0 && (uint64_t)-delta > base ? 1 : base + (uint64_t)delta;
}
dispatch_semaphore_t dispatch_semaphore_create(long value) {
    dispatch_semaphore_t s = calloc(1, sizeof *s); if (!s) return NULL;
    pthread_mutex_init(&s->m, NULL);
    pthread_condattr_t a; pthread_condattr_init(&a); pthread_condattr_setclock(&a, CLOCK_MONOTONIC); pthread_cond_init(&s->c, &a); pthread_condattr_destroy(&a);
    s->v = value; return s;
}
long dispatch_semaphore_signal(dispatch_semaphore_t s) {
    pthread_mutex_lock(&s->m); long woke = s->v < 0; s->v++; pthread_cond_signal(&s->c); pthread_mutex_unlock(&s->m); return woke;
}
long dispatch_semaphore_wait(dispatch_semaphore_t s, dispatch_time_t timeout) {
    pthread_mutex_lock(&s->m);
    struct timespec ts = {(time_t)(timeout / 1000000000ull), (long)(timeout % 1000000000ull)};
    while (s->v <= 0) {
        if (timeout == DISPATCH_TIME_FOREVER) pthread_cond_wait(&s->c, &s->m);
        else if (pthread_cond_timedwait(&s->c, &s->m, &ts) == ETIMEDOUT && s->v <= 0) { pthread_mutex_unlock(&s->m); return 1; }
    }
    s->v--; pthread_mutex_unlock(&s->m); return 0;
}
void dispatch_release(void *o) {
    dispatch_semaphore_t s = o; if (!s) return;
    pthread_mutex_destroy(&s->m); pthread_cond_destroy(&s->c); free(s);
}
// The global queues: the qos is taken on the work's own thread, as a global queue's worker runs at it
struct dm_dispatch_queue { long qos; };
static struct dm_dispatch_queue g_global_q[6];
dispatch_queue_t dispatch_get_global_queue(long qos, unsigned long flags) {
    (void)flags; long i = qos >= QOS_CLASS_USER_INTERACTIVE ? 0 : qos >= QOS_CLASS_USER_INITIATED ? 1 : qos >= QOS_CLASS_DEFAULT ? 2 : qos >= QOS_CLASS_UTILITY ? 3 : qos ? 4 : 5;
    g_global_q[i].qos = qos; return &g_global_q[i];
}
typedef struct { void *ctx; void (*work)(void *); long qos; } dm_async_item;
static void *dm_async_main(void *p) {
    dm_async_item it = *(dm_async_item *)p; free(p);
    if (it.qos) dm_android_qos_set((qos_class_t)it.qos);
    it.work(it.ctx); return NULL;
}
void dispatch_async_f(dispatch_queue_t q, void *ctx, void (*work)(void *)) {
    dm_async_item *it = malloc(sizeof *it); pthread_t th;
    if (!it) { work(ctx); return; }   // no memory for the item: run it here rather than lose it
    *it = (dm_async_item){ctx, work, q ? q->qos : 0};
    if (pthread_create(&th, NULL, dm_async_main, it)) { free(it); work(ctx); return; }
    pthread_detach(th);
}

// ---- mergesort (BSD libc): a stable sort; bionic has none (hle_hid.c sorts DM_KEYS steps with it) ----
int mergesort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    if (n < 2 || !size) return 0;
    char *a = base, *t = malloc(n * size); if (!t) { errno = ENOMEM; return -1; }
    for (size_t w = 1; w < n; w *= 2) {   // bottom-up: runs of w merged into runs of 2w, ties taken from the left run
        for (size_t lo = 0; lo < n; lo += 2 * w) {
            size_t mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n, i = lo, j = mid, k = lo;
            while (i < mid && j < hi) { if (cmp(a + j * size, a + i * size) < 0) memcpy(t + k++ * size, a + j++ * size, size); else memcpy(t + k++ * size, a + i++ * size, size); }
            if (i < mid) memcpy(t + k * size, a + i * size, (mid - i) * size), k += mid - i;
            if (j < hi) memcpy(t + k * size, a + j * size, (hi - j) * size);
        }
        memcpy(a, t, n * size);
    }
    free(t); return 0;
}

// ---- SHA-256 (FIPS 180-4) ----
static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha_block(uint32_t h[8], const uint8_t *p) {
    uint32_t w[64], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) w[i] = w[i - 16] + (ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3)) + w[i - 7] + (ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10));
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = k + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}
int CC_SHA256_Init(CC_SHA256_CTX *c) {
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(c->h, iv, sizeof iv); c->len = 0; c->n = 0; return 1;
}
int CC_SHA256_Update(CC_SHA256_CTX *c, const void *d, CC_LONG n) {
    const uint8_t *p = d; c->len += n;
    while (n) {
        uint32_t k = 64 - c->n < n ? 64 - c->n : n;
        memcpy(c->buf + c->n, p, k); c->n += k; p += k; n -= k;
        if (c->n == 64) { sha_block(c->h, c->buf); c->n = 0; }
    }
    return 1;
}
int CC_SHA256_Final(unsigned char *md, CC_SHA256_CTX *c) {
    uint64_t bits = c->len * 8; uint8_t pad = 0x80, z = 0;
    CC_SHA256_Update(c, &pad, 1);
    while (c->n != 56) CC_SHA256_Update(c, &z, 1);
    uint8_t lb[8]; for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(bits >> (56 - 8 * i));
    CC_SHA256_Update(c, lb, 8);
    for (int i = 0; i < 8; i++) { md[4 * i] = (uint8_t)(c->h[i] >> 24); md[4 * i + 1] = (uint8_t)(c->h[i] >> 16); md[4 * i + 2] = (uint8_t)(c->h[i] >> 8); md[4 * i + 3] = (uint8_t)c->h[i]; }
    return 1;
}
unsigned char *CC_SHA256(const void *d, CC_LONG n, unsigned char *md) { CC_SHA256_CTX c; CC_SHA256_Init(&c); CC_SHA256_Update(&c, d, n); CC_SHA256_Final(md, &c); return md; }

// mach_absolute_time as a real function, for assembly that calls it (nvn_entry.S; tools/asm_elf.py renames the call)
uint64_t dm_mach_absolute_time(void) { return mach_absolute_time(); }

// ---- vm_region_64: the region holding *a, or the next one above it (Mach's answer), from /proc/self/maps ----
// Adjacent lines with the same permissions are one region, as the kernel may split one mapping (Scudo names its pieces). Android tags malloc's pointers in
// the top byte (0xb4..., the app's native heap pointer tagging), and the maps list untagged addresses: the region is found by the untagged address and its
// start given back with the query's tag, so a caller comparing the tagged pointer with the bounds (hle_uemalloc.c dm_ue_heap_owns) compares like with like.
// A query reads the file, so it is for rare questions (a thread's stack, a fault's region)
kern_return_t vm_region_64(mach_port_t t, vm_address_t *a, vm_size_t *s, int f, vm_region_info_t info, mach_msg_type_number_t *n, mach_port_t *o) {
    (void)t; (void)f; (void)n;
    if (o) *o = 0;
    uintptr_t tag = (uintptr_t)*a & 0xff00000000000000ULL, want = (uintptr_t)*a & ~0xff00000000000000ULL;
    FILE *m = fopen("/proc/self/maps", "re");
    if (!m) return KERN_FAILURE;
    char line[512], perm[5], seen[5] = "";
    uintptr_t lo = 0, hi = 0;
    while (fgets(line, sizeof line, m)) {
        unsigned long l, h;
        if (sscanf(line, "%lx-%lx %4s", &l, &h, perm) != 3) continue;
        if (hi) {   // a region found: extend it over the lines that continue it
            if (l != hi || strncmp(perm, seen, 3)) break;
            hi = h; continue;
        }
        if (h > want) { lo = l; hi = h; memcpy(seen, perm, sizeof seen); }
    }
    fclose(m);
    if (!hi) return KERN_FAILURE;
    vm_region_basic_info_data_64_t *bi = (vm_region_basic_info_data_64_t *)info;
    if (bi) bi->protection = bi->max_protection = (seen[0] == 'r' ? VM_PROT_READ : 0) | (seen[1] == 'w' ? VM_PROT_WRITE : 0) | (seen[2] == 'x' ? VM_PROT_EXECUTE : 0);
    *a = lo | tag; *s = hi - lo;
    return KERN_SUCCESS;
}

// ---- thread_info: THREAD_BASIC_INFO from /proc/self/task/<tid>/stat (state) and the thread's CPU clock (user time; system time is not split) ----
kern_return_t thread_info(mach_port_t t, int flavor, thread_info_t out, mach_msg_type_number_t *n) {
    if (flavor != THREAD_BASIC_INFO || !out) return KERN_INVALID_ARGUMENT;
    thread_basic_info_data_t *bi = (thread_basic_info_data_t *)(void *)out; memset(bi, 0, sizeof *bi);
    clockid_t ck = (clockid_t)((~(unsigned)t << 3) | 6);   // MAKE_THREAD_CPUCLOCK(tid, CPUCLOCK_SCHED)
    struct timespec ts; if (clock_gettime(ck, &ts)) return KERN_FAILURE;
    bi->user_time.seconds = (int)ts.tv_sec; bi->user_time.microseconds = (int)(ts.tv_nsec / 1000);
    char path[64], buf[256]; snprintf(path, sizeof path, "/proc/self/task/%u/stat", t);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ssize_t k = read(fd, buf, sizeof buf - 1); close(fd);
        if (k > 0) { buf[k] = 0; const char *p = strrchr(buf, ')');
            if (p && p[1] == ' ') switch (p[2]) {
                case 'R': bi->run_state = TH_STATE_RUNNING; break; case 'D': bi->run_state = TH_STATE_UNINTERRUPTIBLE; break;
                case 'T': case 't': bi->run_state = TH_STATE_STOPPED; break; case 'Z': case 'X': bi->run_state = TH_STATE_HALTED; break;
                default: bi->run_state = TH_STATE_WAITING; } }
    }
    if (n) *n = THREAD_BASIC_INFO_COUNT;
    return KERN_SUCCESS;
}

// ---- CFMutableDictionary with pointer keys (open addressing, linear probing, tombstones) ----
struct dm_cfdict { const void **k, **v; size_t cap, n, used; };
#define CF_TOMB ((const void *)1)
static size_t cf_h(const void *k, size_t cap) { uint64_t x = (uint64_t)(uintptr_t)k * 0x9E3779B97F4A7C15ull; return (size_t)(x >> 17) & (cap - 1); }
CFMutableDictionaryRef CFDictionaryCreateMutable(CFAllocatorRef a, CFIndex cap, const void *kcb, const void *vcb) {
    (void)a; (void)cap; (void)kcb; (void)vcb;
    struct dm_cfdict *d = calloc(1, sizeof *d); d->cap = 64; d->k = calloc(d->cap, sizeof *d->k); d->v = calloc(d->cap, sizeof *d->v); return d;
}
static const void **cf_slot(struct dm_cfdict *d, const void *k, int insert) {
    const void **tomb = NULL;
    for (size_t i = cf_h(k, d->cap);; i = (i + 1) & (d->cap - 1)) {
        if (d->k[i] == k) return &d->k[i];
        if (d->k[i] == CF_TOMB) { if (!tomb) tomb = &d->k[i]; continue; }
        if (!d->k[i]) return insert ? (tomb ? tomb : &d->k[i]) : NULL;
    }
}
static void cf_grow(struct dm_cfdict *d) {
    const void **ok = d->k, **ov = d->v; size_t oc = d->cap;
    d->cap *= 2; d->k = calloc(d->cap, sizeof *d->k); d->v = calloc(d->cap, sizeof *d->v); d->used = d->n;
    for (size_t i = 0; i < oc; i++) if (ok[i] && ok[i] != CF_TOMB) { const void **s = cf_slot(d, ok[i], 1); *s = ok[i]; d->v[s - d->k] = ov[i]; }
    free(ok); free(ov);
}
const void *CFDictionaryGetValue(CFDictionaryRef d, const void *k) { const void **s = cf_slot(d, k, 0); return s ? d->v[s - d->k] : NULL; }
void CFDictionarySetValue(CFMutableDictionaryRef d, const void *k, const void *v) {
    if ((d->used + 1) * 4 > d->cap * 3) cf_grow(d);
    const void **s = cf_slot(d, k, 1);
    if (*s != k) { if (!*s) d->used++; *s = k; d->n++; }
    d->v[s - d->k] = v;
}
void CFDictionaryRemoveValue(CFMutableDictionaryRef d, const void *k) { const void **s = cf_slot(d, k, 0); if (s) { *s = CF_TOMB; d->v[s - d->k] = NULL; d->n--; } }
CFIndex CFDictionaryGetCount(CFDictionaryRef d) { return (CFIndex)d->n; }
void CFRelease(CFTypeRef o) { struct dm_cfdict *d = (struct dm_cfdict *)o; if (d) { free(d->k); free(d->v); free(d); } }

kern_return_t task_info(mach_port_t t, int flavor, task_info_t out, mach_msg_type_number_t *n) {
    (void)t; (void)n;
    if (flavor != TASK_VM_INFO) return KERN_FAILURE;
    FILE *f = fopen("/proc/self/statm", "r"); unsigned long long vs = 0, rs = 0;
    if (!f) return KERN_FAILURE;
    int ok = fscanf(f, "%llu %llu", &vs, &rs) == 2; fclose(f);
    if (!ok) return KERN_FAILURE;
    task_vm_info_data_t *vi = (task_vm_info_data_t *)(void *)out; long pg = sysconf(_SC_PAGESIZE);
    vi->virtual_size = vs * pg; vi->resident_size = vi->phys_footprint = rs * pg; vi->resident_size_peak = 0;
    return KERN_SUCCESS;
}

kern_return_t vm_remap(mach_port_t t, vm_address_t *to, vm_size_t size, vm_address_t mask, int flags, mach_port_t st, vm_address_t from, boolean_t copy,
                       vm_prot_t *cur, vm_prot_t *max, vm_inherit_t inh) {
    (void)t; (void)mask; (void)st; (void)inh;
    if (copy) return KERN_FAILURE;
    // The duplicate is made where the kernel puts it, and only then moved to a fixed destination. mremap with MREMAP_FIXED unmaps the destination before it looks at the
    // source, so a duplication it then refuses (old size 0 of a private mapping: EINVAL) left a hole where the destination had been mapped. A virtual memory pool's
    // requests (hle_nvn.c vpool_map) all fail that way on Android, whose pools are private memory: Mario Kart World made about 9,000, each a hole in the pool's
    // reservation, the driver's next mappings landed in the holes (an arena block, its own structures), and the next request over the same range unmapped them: a fault in
    // the replay's copy into the arena on a Galaxy S25 Edge, and in Qualcomm's driver at a submission on a OnePlus Pad 3
    void *r = mremap((void *)from, 0, size, MREMAP_MAYMOVE);
    if (r == MAP_FAILED) return KERN_FAILURE;   // nothing was touched
    if (!(flags & VM_FLAGS_ANYWHERE)) {
        void *at = mremap(r, size, size, MREMAP_MAYMOVE | MREMAP_FIXED, (void *)*to);
        if (at == MAP_FAILED) {   // the destination may be unmapped now: it stays reserved, as inaccessible pages, rather than open to any later mapping
            munmap(r, size); mmap((void *)*to, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
            return KERN_FAILURE;
        }
        r = at;
    }
    *to = (vm_address_t)r; if (cur) *cur = VM_PROT_READ | VM_PROT_WRITE; if (max) *max = VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE;
    return KERN_SUCCESS;
}

// ---- Thread suspension (thread_suspend / thread_get_state / thread_resume): a real-time signal sent to the thread; its handler saves the registers in Darwin's
// layout (DM_SS) and waits on a futex word until the thread is resumed. Used by the host's hang reports and the sampling probe, and by the preemption tests ----
#include <linux/futex.h>
#define SUSP_MAX 128
typedef struct { _Atomic int tid; int count; _Atomic int stopped, go; struct __darwin_arm_thread_state64 ss; } susp_t;
static susp_t g_susp[SUSP_MAX];
static pthread_mutex_t g_susp_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t g_susp_once = PTHREAD_ONCE_INIT;
static int fx_wait(_Atomic int *w, int v, const struct timespec *t) { return (int)syscall(SYS_futex, (int *)w, FUTEX_WAIT_PRIVATE, v, t, NULL, 0); }
static void fx_wake(_Atomic int *w) { syscall(SYS_futex, (int *)w, FUTEX_WAKE_PRIVATE, 0x7fffffff, NULL, NULL, 0); }
int dm_android_suspend_signal(void) { return SIGRTMIN + 6; }
static susp_t *susp_of(int tid) { for (int i = 0; i < SUSP_MAX; i++) if (atomic_load(&g_susp[i].tid) == tid) return &g_susp[i]; return NULL; }
static void on_suspend(int sig, siginfo_t *si, void *uc_) {
    (void)sig; (void)si; int e0 = errno;
    susp_t *s = susp_of(gettid());
    if (s) {
        s->ss = DM_SS((ucontext_t *)uc_);
        atomic_store(&s->stopped, 1); fx_wake(&s->stopped);
        while (!atomic_load(&s->go)) fx_wait(&s->go, 0, NULL);
        atomic_store(&s->stopped, 0); fx_wake(&s->stopped);
    }
    errno = e0;
}
static void susp_install(void) {
    struct sigaction sa = {.sa_sigaction = on_suspend, .sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART};
    sigfillset(&sa.sa_mask);
    sigaction(dm_android_suspend_signal(), &sa, NULL);
}
kern_return_t thread_suspend(mach_port_t t) {
    if ((int)t == gettid() || !t) return KERN_INVALID_ARGUMENT;
    pthread_once(&g_susp_once, susp_install);
    pthread_mutex_lock(&g_susp_mu);
    susp_t *s = susp_of((int)t);
    if (s && s->count++ > 0) { pthread_mutex_unlock(&g_susp_mu); return KERN_SUCCESS; }
    if (!s) {
        for (int i = 0; i < SUSP_MAX && !s; i++) if (!atomic_load(&g_susp[i].tid)) s = &g_susp[i];
        if (!s) { pthread_mutex_unlock(&g_susp_mu); return KERN_FAILURE; }
        atomic_store(&s->stopped, 0); atomic_store(&s->go, 0); s->count = 1; atomic_store(&s->tid, (int)t);
    }
    pthread_mutex_unlock(&g_susp_mu);
    if (syscall(SYS_tgkill, getpid(), (int)t, dm_android_suspend_signal())) goto fail;
    // the thread is stopped once its handler runs; a thread that has the signal blocked or has exited never gets there (1 s)
    for (int i = 0; i < 100 && !atomic_load(&s->stopped); i++) { struct timespec ts = {0, 10000000}; fx_wait(&s->stopped, 0, &ts); }
    if (atomic_load(&s->stopped)) return KERN_SUCCESS;
fail:
    pthread_mutex_lock(&g_susp_mu); atomic_store(&s->go, 1); fx_wake(&s->go); atomic_store(&s->tid, 0); s->count = 0; pthread_mutex_unlock(&g_susp_mu);
    return KERN_FAILURE;
}
kern_return_t thread_resume(mach_port_t t) {
    pthread_mutex_lock(&g_susp_mu);
    susp_t *s = susp_of((int)t);
    if (!s || s->count <= 0) { pthread_mutex_unlock(&g_susp_mu); return KERN_FAILURE; }
    if (--s->count == 0) {
        atomic_store(&s->go, 1); fx_wake(&s->go);
        while (atomic_load(&s->stopped)) { struct timespec ts = {0, 1000000}; fx_wait(&s->stopped, 1, &ts); }   // out of the handler before the slot is reused
        atomic_store(&s->tid, 0);
    }
    pthread_mutex_unlock(&g_susp_mu);
    return KERN_SUCCESS;
}
kern_return_t thread_get_state(mach_port_t t, int flavor, thread_state_t out, mach_msg_type_number_t *n) {
    if (flavor != ARM_THREAD_STATE64) return KERN_INVALID_ARGUMENT;
    pthread_mutex_lock(&g_susp_mu);
    susp_t *s = susp_of((int)t); int ok = s && atomic_load(&s->stopped);
    if (ok) { memcpy(out, &s->ss, sizeof s->ss); if (n) *n = ARM_THREAD_STATE64_COUNT; }
    pthread_mutex_unlock(&g_susp_mu);
    return ok ? KERN_SUCCESS : KERN_FAILURE;
}
