// ---------------- Images no draw has used for a while, evicted (included by gfx_vk.c after vt_drop_pool) ----------------
// The replay frees a GPU-made texture's image only when another description takes over its storage and no object resolves to it any more (share_take_over), or when
// the game finalizes its memory pool (vt_drop_pool); a packaged one when the game releases its object. On the console texture objects over the same memory are
// descriptors of that memory: aliases cost nothing. Here every description over a storage range is an image of its own, and Ocarina of Time keeps its objects and
// its pools for the whole game, so every scene's render targets stayed: on the Galaxy Z Fold 8 (11.3 GB) the start of its first story scene added 241 targets
// (365 MB) in 20 s, and Android's low-memory killer ended the game at 11 GB, 2.5 to 6 minutes in. Image memory is memory the system cannot page out, unlike the
// game's own. Two kinds go, every EV_EVERY presents, at a present, under g_bk:
// - Clean: an image whose content is still its storage's, so the next lookup makes it again as it was made: a packaged texture (uploaded from guest memory,
//   vt_upload) or a shared one filled from its storage when it was made (tex_init_fill). Nothing on the GPU wrote it since (vt_get_w: a pass's target, a copy's or
//   a clear's destination; an image slot), it was never read through the texture pool by ID (a view dropped from the pool set reads 0 until the sync after a draw
//   reads its ID again: pool_drop_view), and it had no lookup for DM_VK_IMAGE_EVICT_AGE presents (default EV_AGE).
// - Overwritten: a shared image that another shared image over its storage was written as since its last lookup, with no lookup for DM_VK_IMAGE_EVICT_ALIAS_AGE
//   presents (default EV_ALIAS_AGE). On the console that write landed in the same memory, so this image's content is gone there too; its next lookup makes it
//   again as a first one does (filled from its storage where tex_init_fill can, else zero).
// Under memory pressure (ev_level: PSI where the app may read it, else swap against lmkd's low-swap line; read at each scan, every EV_EVERY_PRESSED presents while it
// lasts), images of either kind with no lookup for EV_MIN_AGE presents go too, the least recently looked up first, until the image memory is at ie_target: holding at
// the first level, an eighth less at the second (img_evict.h ie_psi_level, ie_swap_level). DM_VK_IMAGE_EVICT_PSI=0: the ages alone.
// How either goes: as a pool's Finalize does it (vt_drop_pool): its objects stop resolving to it and the sharing table forgets it, so the next lookup goes the way
// a first one does (vt_create, then vt_upload or tex_init_fill); vt_destroy frees it when the chunks that may still use it have completed (dead_add), and g_vt_gen
// tells the draws' caches.
// DM_VK_IMAGE_EVICT=0 (env.txt): images stay until their storage is taken over or their pool goes, as before. DM_VK_IMAGE_EVICT_LOG=1: a line at every scan (the memory
// line comes every 300 frames, and a scene's targets come and go in less)
#include "img_evict.h"   // the rules, checked on the host (dm/tools/tests/test_img_evict.py)
#include <sys/system_properties.h>
#define EV_AGE 600
#define EV_ALIAS_AGE 30
#define EV_EVERY 30
#define EV_EVERY_PRESSED 10
#define EV_MIN_AGE 30   // (3 on the Fold in Saria's scene, where swap holds the rule on throughout: 28,343 images made again against 4,943, 13.1 fps against 14.2)
static int ev_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_IMAGE_EVICT"); v = !(e && *e == '0'); } return v; }
static uint64_t ev_env(const char *name, long def) { const char *e = getenv(name); return (uint64_t)(e && atol(e) > 0 ? atol(e) : def); }
static uint64_t ev_age(void) { static uint64_t a; if (!a) a = ev_env("DM_VK_IMAGE_EVICT_AGE", EV_AGE); return a; }
static int ev_log(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_IMAGE_EVICT_LOG"); v = e && *e == '1'; } return v; }
static uint64_t ev_alias_age(void) { static uint64_t a; if (!a) a = ev_env("DM_VK_IMAGE_EVICT_ALIAS_AGE", EV_ALIAS_AGE); return a; }
static uint64_t g_ev_n[3], g_ev_bytes[3], g_ev_back;   // images evicted and their bytes (clean, overwritten, for memory pressure), evicted storage made again (cumulative)
static uint64_t g_ev_cap, g_ev_psi_scans; static int g_ev_level;   // the pressure rule: its hold's bytes, scans it chose at, its level
static nvn_hmap g_ev_store = {.klen = 8};   // the storage of evicted images not made again yet: the lookup that makes one again counts it (ev_made)
static void ev_made(vtex *v) { uint64_t k = (uint64_t)(uintptr_t)v->store; if (k && g_ev_store.n && nvn_hm_del(&g_ev_store, &k)) g_ev_back++; }
static int ev_clean(const vtex *v, uint64_t now, uint64_t age) { return v->img && ie_clean(v->gpu_w, v->pool_read, v->pkg, v->shared, v->filled, v->last_present, now, age); }
static ssize_t ev_read(const char *path, char *b, size_t cap, int *er) {
    int fd = open(path, O_RDONLY | O_CLOEXEC); ssize_t n = fd >= 0 ? read(fd, b, cap - 1) : -1; *er = errno; if (fd >= 0) close(fd);
    if (n >= 0) b[n] = 0;
    return n;
}
// Where the pressure comes from, chosen once: the system's /proc/pressure/memory, else the process's own cgroup's memory.pressure (the same text; cgroup v2, "0::<path>" in
// /proc/self/cgroup), else swap from /proc/meminfo against lmkd's low-swap line (img_evict.h ie_swap_level). An app's SELinux domain may not read either PSI file: the
// Galaxy Z Fold 8 denies both, and there swap is the source. The start's line names the source and why the others failed. DM_VK_IMAGE_EVICT_PSI=swap skips PSI
static int g_ev_src; static char g_ev_path[256], g_ev_now[64] = "-";   // 0 none, 1 PSI at g_ev_path, 2 swap; the last reading, for the lines
static unsigned g_ev_swap_low; static uint64_t g_ev_swap_last;   // lmkd's low-swap line in percent of SwapTotal; SwapFree at the last shrink (ie_swap_level)
static unsigned ev_swap_low_pct(const char **from) {   // DM_VK_IMAGE_EVICT_SWAP_LOW, else lmkd's own setting where the app may read it, else IE_SWAP_LOW_PCT
    const char *e = getenv("DM_VK_IMAGE_EVICT_SWAP_LOW"); char p[PROP_VALUE_MAX] = "";
    if (e && atoi(e) > 0 && atoi(e) < 100) { *from = "DM_VK_IMAGE_EVICT_SWAP_LOW"; return (unsigned)atoi(e); }
    if (__system_property_get("ro.lmk.swap_free_low_percentage", p) > 0 && atoi(p) > 0 && atoi(p) < 100) { *from = "ro.lmk.swap_free_low_percentage"; return (unsigned)atoi(p); }
    *from = "the default, the Galaxy Z Fold 8's line"; return IE_SWAP_LOW_PCT;
}
static void ev_src_pick(int psi) {
    char b[2048], cg[256] = ""; int er1 = 0, er2 = ENOENT, er = 0;
    if (psi && ev_read("/proc/pressure/memory", b, 512, &er1) > 0) { snprintf(g_ev_path, sizeof g_ev_path, "/proc/pressure/memory"); g_ev_src = 1; }
    else if (psi) {
        char c[512]; const char *z = ev_read("/proc/self/cgroup", c, sizeof c, &er) > 0 ? strstr(c, "0::") : NULL;
        if (z) { size_t l = strcspn(z + 3, "\n"); snprintf(cg, sizeof cg, "/sys/fs/cgroup%.*s/memory.pressure", (int)(l < 200 ? l : 200), z + 3);
                 if (ev_read(cg, b, 512, &er2) > 0) { snprintf(g_ev_path, sizeof g_ev_path, "%s", cg); g_ev_src = 1; } }
    }
    char why[384]; snprintf(why, sizeof why, psi ? "PSI: /proc/pressure/memory %s, the process's cgroup %s %s" : "PSI skipped (DM_VK_IMAGE_EVICT_PSI=swap)%s%s%s",
                            psi ? (er1 ? strerror(er1) : "readable") : "", psi ? (cg[0] ? cg : "(none in /proc/self/cgroup)") : "", psi ? (g_ev_src ? "readable" : strerror(er2)) : "");
    if (g_ev_src) { LOG("[vk] image eviction: memory pressure from %s: holding from %.0f %% stalled, an eighth at each scan from %.0f %% (%s)", g_ev_path, IE_PSI_HOLD, IE_PSI_SHRINK, why); return; }
    uint64_t tot = ev_read("/proc/meminfo", b, sizeof b, &er) > 0 ? ie_meminfo_kb(b, "SwapTotal:") : 0;
    if (!tot) { LOG("[vk] image eviction: no memory pressure to read, the ages alone (%s; no swap in /proc/meminfo)", why); return; }
    const char *from; g_ev_swap_low = ev_swap_low_pct(&from); g_ev_src = 2;
    double low = tot / 100.0 * g_ev_swap_low / 1048576.0;
    LOG("[vk] image eviction: memory pressure from swap (/proc/meminfo): lmkd's low-swap line %u %% of %.2f GB (%s) = %.2f GB free; holding under %.2f GB, an eighth as it falls under "
        "%.2f GB and at each further %.2f GB (%s)", g_ev_swap_low, tot / 1048576.0, from, low, low * 1.25, low, tot / 32 / 1048576.0, why);
}
static int ev_level(void) {   // the pressure level now (0 none, 1 holding, 2 shrinking); g_ev_now the reading
    static int on = -1; char b[2048]; int er = 0;
    if (on < 0) { const char *e = getenv("DM_VK_IMAGE_EVICT_PSI"); on = !(e && *e == '0'); if (on) ev_src_pick(!(e && !strcmp(e, "swap"))); }
    if (!on || !g_ev_src) return 0;
    if (g_ev_src == 1) {
        if (ev_read(g_ev_path, b, 512, &er) <= 0) return 0;
        double v = ie_psi_some10(b); snprintf(g_ev_now, sizeof g_ev_now, "%.1f %% stalled", v); return ie_psi_level(v);
    }
    if (ev_read("/proc/meminfo", b, sizeof b, &er) <= 0) return 0;
    uint64_t fr = ie_meminfo_kb(b, "SwapFree:"), tot = ie_meminfo_kb(b, "SwapTotal:");
    snprintf(g_ev_now, sizeof g_ev_now, "swap %.2f of %.2f GB free", fr / 1048576.0, tot / 1048576.0);
    return ie_swap_level(fr, tot, g_ev_swap_low, &g_ev_swap_last);
}
static void ev_overwritten(uint64_t now, uint64_t age, uint8_t as) {   // the shared images another one over their storage overwrote (ie_sweep), marked as
    if (!g_share.n) return;
    ie_ent *e = malloc((size_t)g_share.n * sizeof *e); uint8_t *mark = malloc(g_share.n); if (!e || !mark) { free(e); free(mark); return; }
    uint32_t n = 0;
    for (uint32_t i = 0; i < g_share.cap && n < g_share.n; i++) {
        vtex *v = g_share.vals[i]; if (!v) continue;
        share_key k; memcpy(&k, g_share.keys + (size_t)i * g_share.klen, sizeof k);
        nvn_texlayout L; nvn_tex_layout(&k.d, &L); e[n++] = (ie_ent){k.pool, k.off, k.off + (int64_t)(L.size ? L.size : 1), v->last_present, v->last_w, v};
    }
    ie_sweep(e, n, now, age, mark);
    for (uint32_t i = 0; i < n; i++) { vtex *v = e[i].p; if (mark[i] && !v->evict) v->evict = as; }
    free(e); free(mark);
}
// Each image once: the sharing table's, then the objects' that aren't shared (a shared image is in both)
#define EV_EACH(v, ...) do { for (uint32_t i_ = 0; i_ < g_share.cap; i_++) { vtex *v = g_share.vals[i_]; if (v) { __VA_ARGS__; } } \
                             for (uint32_t i_ = 0; i_ < g_tex.cap; i_++) { vtex *v = g_tex.vals[i_]; if (v && !v->shared) { __VA_ARGS__; } } } while (0)
static void ev_pressure(uint64_t now, int level) {   // after the age rules: the least recently looked up images either rule allows at EV_MIN_AGE, down to ie_target (marked 4)
    uint64_t target = ie_target(level, g_im_live, &g_ev_cap), gone = 0;
    EV_EACH(v, if (v->evict) gone += v->mem_bytes);
    if (g_im_live <= gone + target) return;
    ev_overwritten(now, EV_MIN_AGE, 5);
    uint32_t n = 0;
    EV_EACH(v, if (!v->evict && ev_clean(v, now, EV_MIN_AGE)) v->evict = 5; if (v->evict == 5) n++);
    ie_cand *c = n ? malloc(n * sizeof *c) : NULL; uint8_t *mark = n ? malloc(n) : NULL; uint32_t k = 0;
    if (c && mark) EV_EACH(v, if (v->evict == 5 && k < n) c[k++] = (ie_cand){v->last_present, v->mem_bytes, v});
    if (k) { ie_pick(c, k, g_im_live - gone - target, mark); g_ev_psi_scans++; }
    for (uint32_t i = 0; i < k; i++) ((vtex *)c[i].p)->evict = mark[i] ? 4 : 0;
    EV_EACH(v, if (v->evict == 5) v->evict = 0);   // (left over when the arrays could not be had)
    free(c); free(mark);
}
static void vt_evict(void) {   // under g_bk, at a present
    static uint64_t last; uint64_t now = atomic_load(&g_presents);
    if (!ev_on() || now - last < (g_ev_level ? EV_EVERY_PRESSED : EV_EVERY)) return;
    last = now;
    int level = ev_level();
    if (level != g_ev_level) { static int nl; if (nl++ < 32) LOG("[vk] image eviction: memory pressure (%s): %s; %llu images, %.0f MB alive", g_ev_now,
                                                                 level == 2 ? "an eighth goes at each scan" : level ? "holding" : "eased, the ages alone", (unsigned long long)g_im_n, g_im_live / 1048576.0);
                               g_ev_level = level; }
    for (uint32_t i = 0; i < g_share.cap; i++) { vtex *v = g_share.vals[i]; if (v && !v->evict && ev_clean(v, now, ev_age())) v->evict = 1; }
    for (uint32_t i = 0; i < g_tex.cap; i++) { vtex *v = g_tex.vals[i]; if (v && !v->evict && ev_clean(v, now, ev_age())) v->evict = 1; }
    ev_overwritten(now, ev_alias_age(), 3);
    if (level) ev_pressure(now, level); else g_ev_cap = 0;
    size_t nt = 0, ns = 0;
    for (uint32_t i = 0; i < g_tex.cap; i++) { vtex *v = g_tex.vals[i]; if (v && v->evict) nt++; }
    for (uint32_t i = 0; i < g_share.cap; i++) { vtex *v = g_share.vals[i]; if (v && v->evict) ns++; }
    if (ev_log()) LOG("[vk] image eviction at present %llu: %llu images, %.0f MB alive before it, %zu objects' and %zu shared entries chosen; %llu evicted so far; memory pressure %s",
                      (unsigned long long)now, (unsigned long long)g_im_n, g_im_live / 1048576.0, nt, ns, (unsigned long long)(g_ev_n[0] + g_ev_n[1] + g_ev_n[2]), g_ev_now);
    if (!nt && !ns) return;
    // the keys first, then the deletions: a deletion moves later entries of its probe chain (nvn_hm_del), so the tables are not walked while they change
    uint64_t *tk = nt ? malloc(nt * sizeof *tk) : NULL; share_key *sk = ns ? malloc(ns * sizeof *sk) : NULL;
    if ((nt && !tk) || (ns && !sk)) { free(tk); free(sk); for (uint32_t i = 0; i < g_tex.cap; i++) if (g_tex.vals[i]) ((vtex *)g_tex.vals[i])->evict = 0;
        for (uint32_t i = 0; i < g_share.cap; i++) if (g_share.vals[i]) ((vtex *)g_share.vals[i])->evict = 0; return; }
    size_t a = 0, b = 0;
    for (uint32_t i = 0; i < g_tex.cap && a < nt; i++) { vtex *v = g_tex.vals[i]; if (v && v->evict) memcpy(&tk[a++], g_tex.keys + (size_t)i * g_tex.klen, 8); }
    for (uint32_t i = 0; i < g_share.cap && b < ns; i++) { vtex *v = g_share.vals[i]; if (v && v->evict) memcpy(&sk[b++], g_share.keys + (size_t)i * g_share.klen, sizeof *sk); }
    #define EV_NOTE(v) do { if ((v)->evict == 1 || (v)->evict >= 3) { int c_ = (v)->evict == 3 ? 1 : (v)->evict == 4 ? 2 : 0; (v)->evict = 2; g_ev_n[c_]++; g_ev_bytes[c_] += (v)->mem_bytes; \
                                uint64_t k_ = (uint64_t)(uintptr_t)(v)->store; if (k_) nvn_hm_put(&g_ev_store, &k_, (void *)1); } } while (0)
    for (size_t j = 0; j < a; j++) { vtex *v = nvn_hm_del(&g_tex, &tk[j]); if (!v) continue; EV_NOTE(v); g_vt_gen++; vt_unref(v); }   // a packaged one goes with its last object
    for (size_t j = 0; j < b; j++) { vtex *v = nvn_hm_del(&g_share, &sk[j]); if (!v) continue; EV_NOTE(v); v->shared = 0; g_vt_gen++; if (v->refs <= 0) vt_destroy(v); }
    #undef EV_NOTE
    free(tk); free(sk);
}
static void ev_stat(void) {   // the periodic line (mtl_mem_stat)
    LOG("[vk] image memory: %llu images, %.0f MB; evicted clean %llu (%.0f MB, %llu presents without a lookup), overwritten %llu (%.0f MB, %llu presents), for memory pressure %llu (%.0f MB, at %llu scans; "
        "%s now), %llu of them made again (cumulative; DM_VK_IMAGE_EVICT%s)",
        (unsigned long long)g_im_n, g_im_live / 1048576.0, (unsigned long long)g_ev_n[0], g_ev_bytes[0] / 1048576.0, (unsigned long long)ev_age(), (unsigned long long)g_ev_n[1], g_ev_bytes[1] / 1048576.0,
        (unsigned long long)ev_alias_age(), (unsigned long long)g_ev_n[2], g_ev_bytes[2] / 1048576.0, (unsigned long long)g_ev_psi_scans, g_ev_now, (unsigned long long)g_ev_back, ev_on() ? "" : "=0: off");
}
