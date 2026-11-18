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
// Over the byte budget (ev_budget: DM_VK_IMAGE_BUDGET_MB, else a quarter of the device's memory; checked at every present, scans every EV_EVERY_PRESSED presents while
// over it), images either rule would take with no lookup for EV_BUDGET_AGE presents go too, the least recently looked up first, until the image memory is 90 % of the
// budget (img_evict.h ie_budget_target, ie_pick). The ages work by time and the budget by bytes: a game that makes images faster than they age is held under it, where no
// memory pressure can be read. Nothing the GPU wrote and no image the texture pool reads by ID goes for it, as for the ages, so a scene whose images are all of those
// stays over it (a line says so).
// How either goes: as a pool's Finalize does it (vt_drop_pool): its objects stop resolving to it and the sharing table forgets it, so the next lookup goes the way
// a first one does (vt_create, then vt_upload or tex_init_fill); vt_destroy frees it when the chunks that may still use it have completed (dead_add), and g_vt_gen
// tells the draws' caches.
// DM_VK_IMAGE_EVICT=0 (env.txt): images stay until their storage is taken over or their pool goes, as before. DM_VK_IMAGE_EVICT_LOG=1: a line at every scan (the memory
// line comes every 300 frames, and a scene's targets come and go in less)
#include "img_evict.h"   // the rules, checked on the host (dm/tools/tests/test_img_evict.py)
#define EV_AGE 600
#define EV_ALIAS_AGE 30
#define EV_EVERY 30
#define EV_EVERY_PRESSED 10
#define EV_BUDGET_AGE 60   // (nx2android's budget: an image looked up in the last 60 presents may be in the submissions in flight, and is likely to be looked up again)
static int ev_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_IMAGE_EVICT"); v = !(e && *e == '0'); } return v; }
static uint64_t ev_env(const char *name, long def) { const char *e = getenv(name); return (uint64_t)(e && atol(e) > 0 ? atol(e) : def); }
static uint64_t ev_age(void) { static uint64_t a; if (!a) a = ev_env("DM_VK_IMAGE_EVICT_AGE", EV_AGE); return a; }
static int ev_log(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_IMAGE_EVICT_LOG"); v = e && *e == '1'; } return v; }
static uint64_t ev_alias_age(void) { static uint64_t a; if (!a) a = ev_env("DM_VK_IMAGE_EVICT_ALIAS_AGE", EV_ALIAS_AGE); return a; }
static uint64_t g_ev_n[3], g_ev_bytes[3], g_ev_back;   // images evicted and their bytes (clean, overwritten, over the byte budget), evicted storage made again (cumulative)
static uint64_t g_ev_bscans, g_ev_bshort;   // the budget's scans that chose images, and those that could not get under it
static nvn_hmap g_ev_store = {.klen = 8};   // the storage of evicted images not made again yet: the lookup that makes one again counts it (ev_made)
static void ev_made(vtex *v) { uint64_t k = (uint64_t)(uintptr_t)v->store; if (k && g_ev_store.n && nvn_hm_del(&g_ev_store, &k)) g_ev_back++; }
static int ev_clean(const vtex *v, uint64_t now, uint64_t age) { return v->img && ie_clean(v->gpu_w, v->pool_read, v->pkg, v->shared, v->filled, v->last_present, now, age); }
// The byte budget, chosen once: DM_VK_IMAGE_BUDGET_MB (0: none), else a quarter of MemTotal (img_evict.h ie_budget_default); the start's line gives it and where it came from
static uint64_t ev_budget(void) {
    static uint64_t b = UINT64_MAX;
    if (b != UINT64_MAX) return b;
    const char *e = getenv("DM_VK_IMAGE_BUDGET_MB");
    if (e && *e) { long long m = atoll(e); b = m > 0 ? (uint64_t)m << 20 : 0; }
    else { char t[512]; int fd = open("/proc/meminfo", O_RDONLY | O_CLOEXEC); ssize_t n = fd >= 0 ? read(fd, t, sizeof t - 1) : -1; if (fd >= 0) close(fd);
           t[n > 0 ? n : 0] = 0; b = ie_budget_default(ie_meminfo_kb(t, "MemTotal:")); }
    if (b) LOG("[vk] image eviction: a byte budget of %llu MB (%s): over it, images a lookup makes again go after %d presents without one, the least recently looked up first",
               (unsigned long long)(b >> 20), e && *e ? "DM_VK_IMAGE_BUDGET_MB" : "a quarter of MemTotal", EV_BUDGET_AGE);
    else LOG("[vk] image eviction: no byte budget (%s)", e && *e ? "DM_VK_IMAGE_BUDGET_MB=0" : "MemTotal unread in /proc/meminfo");
    return b;
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
static void ev_over_budget(uint64_t now, uint64_t target) {   // after the age rules: the least recently looked up images either rule allows at EV_BUDGET_AGE, down to target (marked 4)
    uint64_t gone = 0;
    EV_EACH(v, if (v->evict) gone += v->mem_bytes);
    if (g_im_live <= gone + target) return;
    ev_overwritten(now, EV_BUDGET_AGE, 5);
    uint32_t n = 0;
    EV_EACH(v, if (!v->evict && ev_clean(v, now, EV_BUDGET_AGE)) v->evict = 5; if (v->evict == 5) n++);
    ie_cand *c = n ? malloc(n * sizeof *c) : NULL; uint8_t *mark = n ? malloc(n) : NULL; uint32_t k = 0;
    if (c && mark) EV_EACH(v, if (v->evict == 5 && k < n) c[k++] = (ie_cand){v->last_present, v->mem_bytes, v});
    uint64_t need = g_im_live - gone - target, got = k ? ie_pick(c, k, need, mark) : 0;
    if (k) g_ev_bscans++;
    if (got < need) { g_ev_bshort++; static int nl; if (nl++ < 8) LOG("[vk] image eviction: %.0f MB of images over the %llu MB byte budget, and only %.0f MB of them may go (%u images a lookup makes "
        "again with none for %d presents); the rest the GPU wrote, the texture pool reads by ID, or was looked up lately", g_im_live / 1048576.0, (unsigned long long)(ev_budget() >> 20), got / 1048576.0, k, EV_BUDGET_AGE); }
    for (uint32_t i = 0; i < k; i++) ((vtex *)c[i].p)->evict = mark[i] ? 4 : 0;
    EV_EACH(v, if (v->evict == 5) v->evict = 0);   // (left over when the arrays could not be had)
    free(c); free(mark);
}
static void vt_evict(void) {   // under g_bk, at a present
    static uint64_t last; uint64_t now = atomic_load(&g_presents);
    if (!ev_on()) return;
    uint64_t target = ie_budget_target(g_im_live, ev_budget());   // (UINT64_MAX: within the budget)
    if (now - last < (target != UINT64_MAX ? EV_EVERY_PRESSED : EV_EVERY)) return;
    last = now;
    for (uint32_t i = 0; i < g_share.cap; i++) { vtex *v = g_share.vals[i]; if (v && !v->evict && ev_clean(v, now, ev_age())) v->evict = 1; }
    for (uint32_t i = 0; i < g_tex.cap; i++) { vtex *v = g_tex.vals[i]; if (v && !v->evict && ev_clean(v, now, ev_age())) v->evict = 1; }
    ev_overwritten(now, ev_alias_age(), 3);
    if (target != UINT64_MAX) ev_over_budget(now, target);
    size_t nt = 0, ns = 0;
    for (uint32_t i = 0; i < g_tex.cap; i++) { vtex *v = g_tex.vals[i]; if (v && v->evict) nt++; }
    for (uint32_t i = 0; i < g_share.cap; i++) { vtex *v = g_share.vals[i]; if (v && v->evict) ns++; }
    if (ev_log()) LOG("[vk] image eviction at present %llu: %llu images, %.0f MB alive before it, %zu objects' and %zu shared entries chosen; %llu evicted so far; %s the %llu MB byte budget",
                      (unsigned long long)now, (unsigned long long)g_im_n, g_im_live / 1048576.0, nt, ns, (unsigned long long)(g_ev_n[0] + g_ev_n[1] + g_ev_n[2]),
                      target != UINT64_MAX ? "over" : "within", (unsigned long long)(ev_budget() >> 20));
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
    LOG("[vk] image memory: %llu images, %.0f MB; evicted clean %llu (%.0f MB, %llu presents without a lookup), overwritten %llu (%.0f MB, %llu presents), over the %llu MB byte budget %llu (%.0f MB, "
        "at %llu scans, %llu of them short of it), %llu of them made again (cumulative; DM_VK_IMAGE_EVICT%s)",
        (unsigned long long)g_im_n, g_im_live / 1048576.0, (unsigned long long)g_ev_n[0], g_ev_bytes[0] / 1048576.0, (unsigned long long)ev_age(), (unsigned long long)g_ev_n[1], g_ev_bytes[1] / 1048576.0,
        (unsigned long long)ev_alias_age(), (unsigned long long)(ev_budget() >> 20), (unsigned long long)g_ev_n[2], g_ev_bytes[2] / 1048576.0, (unsigned long long)g_ev_bscans,
        (unsigned long long)g_ev_bshort, (unsigned long long)g_ev_back, ev_on() ? "" : "=0: off");
}
