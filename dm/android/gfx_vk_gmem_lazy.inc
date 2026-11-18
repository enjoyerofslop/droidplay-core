// ---- Pools placed in chunks as they are used (DM_VK_RAW_GMEM_LAZY) ----
// Placed memory is committed when it is allocated and can't go to swap. Placing whole pools pinned the pages the game never touched: in Ocarina of Time's Kokiri Forest only
// 571 of the 1,967 MiB placed had been resident before, the game's own anonymous memory sat 4.1 GB deep in swap (dumpsys meminfo), and the low-memory killer ended 2 of 11
// runs on a Galaxy Z Fold 8 (PSS about 11.2 GB on 10.8 GB). A pool is now placed in chunks of GM_CHUNK (one under it a single chunk; gm_lazy_min), each when it is first used:
// - when a storage buffer in a chunk is bound (gm_inplace → gm_lazy_bind), the chunks it covers, as one allocation so that it binds in place;
// - when a word in a storage or uniform buffer that a shader binding the raw pool table reads points into a chunk, or a word in the first 4 KiB at such a pointer does
//   (gm_lazy_ptrs, from stage_set): that chunk and the next, placed at once (a buffer is scanned again at most every 100 ms). Uniform buffers too: with storage buffers alone,
//   Link and the Mad Scrubs went dark in a fight in Ocarina of Time's Sacred Grove, lit with every written chunk placed (DM_VK_RAW_GMEM_WRITTEN=1);
// - in a pool a scanned pointer reached, every 200 ms the chunks the CPU has written (a page present or swapped out): a shader may index far into a pool from one pointer
//   (an array of light probes, say), past the two chunks placed for it;
// - every 200 ms or so at a submission's start (gm_lazy_tick), those a binding or a pointer wanted placed but which a pending write kept back (the swap waits for it there;
//   inside stage_set it may not).
// Chunks the CPU wrote but nothing binds or points to stay unplaced: a raw load from one reads 0. Placing every chunk the game had written (a page present or swapped out,
// /proc/self/pagemap) placed 1,172 MiB in Kokiri Forest, two thirds of it memory no draw, dispatch or copy reads (a test-only count on a Galaxy S25 Edge); placing on bindings
// alone drew Link and the Kokiri black again, since his data is reached through pointers, and the scan found it in 4 chunks: 693 MiB placed, the picture as before.
// DM_VK_RAW_GMEM_WRITTEN=1 (env.txt) places the chunks the CPU wrote too, as before. Two gaps remain: a pointer the scan doesn't see (deeper than two levels, or computed in
// the shader) reads 0 until its chunk is bound or written with DM_VK_RAW_GMEM_WRITTEN=1; raw stores into a chunk not placed go nowhere (DM_VK_RAW_GMEM_SINK counts them); and a
// buffer across a chunk placed earlier and one placed later is copied as before (two allocations) until its pool is merged.
// A chunk is placed while the game may be writing it, so the swap freezes it: read-only from the copy to the placed map, and a guest write meanwhile waits in gm_frz_hook, a
// host write in gm_frz_prep (a system call writing a read-only page fails rather than faults). This is the design of nx2android's lazy pool backing and its freeze window
// (vk_pool_of, nvn_dirty_freeze), written anew; it backs a chunk at its first GPU use as well, and backs every addressed pool once a shader uses raw memory, where this
// places what a binding or a pointer reaches. DM_VK_RAW_GMEM_LAZY=0 (env.txt): whole pools at their first buffer address, as before
typedef struct { uintptr_t lo, hi; const void *pool; uint8_t *st; uint32_t nch; int merge, want, raw; } gm_lz;   // st: each chunk's state (LZ_*); merge: a binding crossed two of its pieces; raw: a scanned pointer reached it (its written chunks placed too); want: a chunk of it is LZ_WANT (gm_lazy_tick visits only those)
enum { LZ_NO, LZ_WANT, LZ_PLACED, LZ_REFUSED };
static gm_lz *g_lz; static int g_nlz, g_clz;   // under g_gm_pl
static int g_lz_pend;   // under g_gm_pl: a pool may have a wanted chunk or a merge asked (gm_lazy_tick visits the pools only then, or at its 200 ms scan)
static _Atomic int g_lz_any;
static uint64_t g_lz_ptr_scans, g_lz_ptr_hits, g_lz_ptr, g_lz_ptr_ns, g_lz_raw_pools;   // g_lz_raw_pools: pools a pointer reached   // gm_lazy_ptrs: buffers scanned, words pointing into a chunked pool, chunks wanted for them
static uint64_t g_lz_overlap;   // chunk runs refused for holding another pool's placed piece (gm_lz_run)
static uint64_t g_lz_pools, g_lz_bind, g_lz_res, g_lz_waited, g_lz_busy, g_lz_scans, g_lz_scan_ns, g_lz_refused, g_lz_cross, g_lz_merges, g_lz_merge_ns, g_lz_merge_bytes;
static _Atomic uint64_t g_frz_waits, g_frz_preps;
static int gm_lazy_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_RAW_GMEM_LAZY"); v = !(e && *e == '0'); if (!v) LOG("[vk] DM_VK_RAW_GMEM_LAZY=0: memory pools are placed whole at their first buffer address"); } return v; }

// ---- The freeze window: one at a time (placements hold g_gm_pl) ----
static _Atomic uintptr_t g_frz_lo, g_frz_hi; static _Atomic int g_frz_reads;   // reads: a merge gives pieces back through a moment the range is not mapped, so a read waits too
static int gm_frz_hook(void *addr, int is_write, void *uc) {   // a signal handler: atomics and sched_yield only
    (void)uc; uintptr_t a = (uintptr_t)addr, hi = atomic_load(&g_frz_hi);
    if ((!is_write && !atomic_load(&g_frz_reads)) || !hi || a < atomic_load(&g_frz_lo) || a >= hi) return 0;
    atomic_fetch_add(&g_frz_waits, 1);
    while (atomic_load(&g_frz_hi) == hi) sched_yield();
    return 1;   // written again on the placed map (or on the anonymous pages, a failed swap having made them writable again)
}
static void gm_frz_prep(void *addr, size_t len) {   // the host is about to write guest memory itself
    uintptr_t a = (uintptr_t)addr, hi = atomic_load(&g_frz_hi);
    if (!hi || a + len <= atomic_load(&g_frz_lo) || a >= hi) return;
    atomic_fetch_add(&g_frz_preps, 1);
    while (atomic_load(&g_frz_hi) == hi) sched_yield();
}
static void gm_frz_register(void) { static int done; if (done++ || !gm_lazy_on()) return; dm_fault_hook_register(gm_frz_hook); dm_guest_write_prep_register(gm_frz_prep); }
static int gm_frz_begin(uintptr_t lo, uintptr_t hi) {   // inside nvn_dirty_open: the tracker's pages of the range are writable and its protection held off
    atomic_store(&g_frz_lo, lo); atomic_store(&g_frz_hi, hi);
    if (mprotect((void *)lo, hi - lo, PROT_READ)) { atomic_store(&g_frz_hi, 0); return 0; }
    return 1;
}
static void gm_frz_end(uintptr_t lo, uintptr_t hi) { (void)lo; (void)hi; atomic_store(&g_frz_hi, 0); }

// ---- The chunks (caller holds g_gm_pl) ----
// g_lz is kept sorted by address (pools don't overlap: none did in Ocarina of Time, counted on a Galaxy S25 Edge), so a lookup is a binary search. The pointer scan asks for
// every word it reads, and with every pool chunked there are thousands of them (4,041 in Kokiri Forest). A word below the first pool or past the last is no pointer into one,
// and most words are such (floats and small integers, where Ocarina of Time's pools lie past 0x4c00000000): they skip the search, which was 3% of its replay thread on a
// Galaxy Z Fold 8 in Kokiri Forest (the pointer scan's 132 buffers a frame, 512 words each, a search per word)
static gm_lz *gm_lz_of(uintptr_t a) {
    if (!g_nlz || a < g_lz[0].lo || a >= g_lz[g_nlz - 1].hi) return NULL;   // (sorted, no overlaps: the last pool ends the highest)
    int lo = 0, hi = g_nlz;   // the first pool starting above a
    while (lo < hi) { int m = (lo + hi) / 2; if (g_lz[m].lo <= a) lo = m + 1; else hi = m; }
    return lo > 0 && a < g_lz[lo - 1].hi ? &g_lz[lo - 1] : NULL;
}
// Every pool is placed in chunks by default, a pool under GM_CHUNK being a single chunk: placing the pools under 4 MiB whole at their first buffer address placed about 500 MiB in
// Kokiri Forest, 356 MiB of it memory no binding reads (a test-only count); chunked, 123 MiB is placed in all against 693, the picture as before and no raw store lost, for about
// 1-3 ms more of the replay thread a frame on a Galaxy S25 Edge. DM_VK_RAW_GMEM_LAZY_MIN=<bytes> (or whole K / M / G; env.txt): the smallest pool placed in chunks, the rest whole
static uint64_t gm_lazy_min(void) {
    static uint64_t v; static int done; if (!done) { done = 1; if (getenv("DM_VK_RAW_GMEM_LAZY_MIN")) { v = gm_size_env("DM_VK_RAW_GMEM_LAZY_MIN"); LOG("[vk] DM_VK_RAW_GMEM_LAZY_MIN: pools of %llu bytes or more are placed in chunks, the rest whole", (unsigned long long)v); } }
    return v ? v : 1;
}
static uintptr_t gm_lz_end(const gm_lz *L, uint32_t c) { uintptr_t e = L->lo + (uintptr_t)(c + 1) * GM_CHUNK; return e < L->hi ? e : L->hi; }
// Whether a page of [a, z) was ever written: present or swapped out, from /proc/self/pagemap's bits 63 and 62 (mincore misses a swapped page, and under this memory pressure
// much of the game's anonymous memory is in zram). A page the game only read maps the zero page and counts too
static int g_pm_fd = -2;
static int gm_lz_touched(uintptr_t a, uintptr_t z) {
    if (g_pm_fd == -2) g_pm_fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    if (g_pm_fd >= 0) {
        uint64_t e[512];
        for (uintptr_t p = a; p < z;) { size_t np = (z - p) >> 12; if (np > 512) np = 512;
            ssize_t r = pread(g_pm_fd, e, np * 8, (off_t)((p >> 12) * 8)); if (r <= 0) break;
            for (size_t i = 0; i < (size_t)r / 8; i++) if (e[i] >> 62) return 1;
            p += ((uintptr_t)r / 8) << 12; }
        return 0; }
    unsigned char v[512]; for (uintptr_t p = a; p < z; p += (uintptr_t)512 << 12) { size_t len = z - p < ((uintptr_t)512 << 12) ? z - p : (size_t)512 << 12;
        if (!mincore((void *)p, len, v)) for (size_t i = 0; i < (len >> 12); i++) if (v[i] & 1) return 1; }
    return 0;
}
static int gm_lz_written(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_RAW_GMEM_WRITTEN"); v = e && *e == '1'; if (v) LOG("[vk] DM_VK_RAW_GMEM_WRITTEN=1: chunks the CPU wrote are placed too"); } return v; }
// DM_VK_RAW_GMEM_REACHED=0 (env.txt): a pool a scanned pointer reached gets only the two chunks at the pointer, as before (to tell the two apart in a run)
static int gm_lz_reached(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_RAW_GMEM_REACHED"); v = !(e && *e == '0'); if (!v) LOG("[vk] DM_VK_RAW_GMEM_REACHED=0: pools a pointer reached get no written chunks placed"); } return v; }
static void gm_lz_scan(gm_lz *L) {   // the chunks not placed with a page written: wanted. Every pool with DM_VK_RAW_GMEM_WRITTEN=1; else only those a pointer reached (L->raw)
    if (!gm_lz_written() && !(L->raw && gm_lz_reached())) return;
    uint64_t t0 = gm_now();
    for (uint32_t c = 0; c < L->nch; c++) if (L->st[c] == LZ_NO && gm_lz_touched(L->lo + (uintptr_t)c * GM_CHUNK, gm_lz_end(L, c))) { L->st[c] = LZ_WANT; L->want = 1; g_lz_res++; }
    g_lz_scans++; g_lz_scan_ns += gm_now() - t0;
}
// chunks [c, e), all wanted, as one swap. may_wait 0 (inside stage_set): when a pending write overlaps them they stay wanted, -1. → chunks placed
static int gm_lz_run(gm_lz *L, uint32_t c, uint32_t e, int live, int may_wait) {
    uintptr_t a = L->lo + (uintptr_t)c * GM_CHUNK; size_t n = gm_lz_end(L, e - 1) - a; int got = 0;
    if (g_gm_cap && g_gm_bytes + n > g_gm_cap) {
        for (uint32_t k = c; k < e; k++) L->st[k] = LZ_REFUSED; g_lz_refused += e - c; g_gm_capped++;
        RED_ONCE(162, "raw global memory: %.2f GiB placed and DM_VK_RAW_GMEM_MAX is %.2f GiB: chunks from %#lx+%#zx on stay anonymous, raw accesses to them read 0",
                 g_gm_bytes / 1073741824.0, g_gm_cap / 1073741824.0, (unsigned long)a, n);
        return 0; }
    // a piece of another pool placed inside the run already (a smaller pool over the same storage, placed whole at its own buffer address): placing over it would map the
    // range twice, and the table would list both. Refused: the placed piece serves its own range, and raw accesses to the rest of the run read 0
    { int ov = 0; pthread_mutex_lock(&g_gm_mu); for (int i = 0; i < g_ngm && !ov; i++) ov = g_gm[i].lo < a + n && a < g_gm[i].hi; pthread_mutex_unlock(&g_gm_mu);
      if (ov) { for (uint32_t k = c; k < e; k++) L->st[k] = LZ_REFUSED; g_lz_refused += e - c; g_lz_overlap++;
                RED_ONCE(164, "raw global memory: chunks %#lx+%#zx of a chunked pool hold a piece of another pool placed already: not placed, raw accesses to the rest read 0", (unsigned long)a, n);
                return 0; } }
    gm_note fail = {0}, probe = {0};
    int r = gm_swap(L->pool, a, n, live, live, may_wait, &fail, &probe);
    if (r < 0) { g_lz_waited++; return -1; }   // wanted still: the next tick waits for the write
    uintptr_t done = a + (uintptr_t)r * (uintptr_t)g_gm_piece; if (done > a + n) done = a + n;
    for (uint32_t k = c; k < e; k++) { int in = gm_lz_end(L, k) <= done; L->st[k] = in ? LZ_PLACED : LZ_REFUSED; if (in) got++; else g_lz_refused++; }
    if (fail.what) gm_note_log(&fail);
    if (probe.what) gm_note_log(&probe);
    return got;
}
static int gm_lz_place(gm_lz *L, int live, int may_wait) {   // every run of wanted chunks. → chunks placed
    int got = 0;
    for (uint32_t c = 0; c < L->nch;) {
        if (L->st[c] != LZ_WANT) { c++; continue; }
        uint32_t e = c; while (e < L->nch && L->st[e] == LZ_WANT) e++;
        int r = gm_lz_run(L, c, e, live, may_wait); if (r > 0) got += r;
        c = e;
    }
    L->want = 0; for (uint32_t c = 0; c < L->nch && !L->want; c++) L->want = L->st[c] == LZ_WANT;   // a pending write kept some back: the tick tries again
    return got;
}
static void gm_lazy_add(const void *pool, uintptr_t lo, size_t size, int live) {   // gm_place, at the pool's first buffer address (or its Initialize)
    for (int i = 0; i < g_nlz; i++) if (g_lz[i].pool == pool) return;
    if (g_nlz == g_clz) { g_clz = g_clz ? 2 * g_clz : 32; g_lz = realloc(g_lz, (size_t)g_clz * sizeof *g_lz); if (!g_lz) die("raw global memory: the chunked pools"); }
    uint32_t nch = (uint32_t)((size + GM_CHUNK - 1) / GM_CHUNK); uint8_t *st = calloc(nch, 1); if (!st) return;
    int at = g_nlz; while (at > 0 && g_lz[at - 1].lo > lo) at--;   // sorted by address (gm_lz_of)
    memmove(g_lz + at + 1, g_lz + at, (size_t)(g_nlz - at) * sizeof *g_lz);
    gm_lz *L = &g_lz[at]; memset(L, 0, sizeof *L);
    L->lo = lo; L->hi = lo + size; L->pool = pool; L->nch = nch; L->st = st;
    g_nlz++; g_lz_pools++; atomic_store(&g_lz_any, 1); atomic_fetch_add(&g_gm_gen, 1);   // (the table's sink ranges: DM_VK_RAW_GMEM_SINK)
    gm_lz_scan(L);
    int got = gm_lz_place(L, live, 1); g_lz_pend |= L->want;
    static int nl; if (nl++ < 4) LOG("[vk] raw global memory: memory pool %#lx+%#zx placed in chunks of %u MiB as they are used: %d of %u now (DM_VK_RAW_GMEM_LAZY=0: whole)",
                                     (unsigned long)lo, size, GM_CHUNK >> 20, got, L->nch);
}
static void gm_lazy_drop(const void *pool) {   // gm_unplace, the pool's Finalize (its placed chunks are segments of the pool and given back with them)
    for (int i = 0; i < g_nlz; i++) if (g_lz[i].pool == pool) { free(g_lz[i].st); memmove(g_lz + i, g_lz + i + 1, (size_t)(--g_nlz - i) * sizeof *g_lz); atomic_fetch_add(&g_gm_gen, 1); return; }
}
// the replay's thread, under g_bk, inside stage_set: a storage buffer [addr, addr + n) in no placed piece. → 1 when the chunks holding it were placed now (as one allocation)
static int gm_lazy_bind(uint64_t addr, size_t n) {
    if (!atomic_load(&g_lz_any)) return 0;
    if (pthread_mutex_trylock(&g_gm_pl)) { g_lz_busy++; return 0; }   // (lock order g_gm_pl before g_bk: a game thread placing a pool holds it and may wait for g_bk)
    gm_lz *L = gm_lz_of((uintptr_t)addr); int got = 0;
    if (L && addr + n <= L->hi) {
        uint32_t c0 = (uint32_t)((addr - L->lo) / GM_CHUNK), c1 = (uint32_t)((addr + n - 1 - L->lo) / GM_CHUNK); int ok = 1;
        for (uint32_t k = c0; k <= c1; k++) if (L->st[k] == LZ_PLACED || L->st[k] == LZ_REFUSED) ok = 0;
        if (!ok) { if (!L->merge) { L->merge = 1; g_lz_cross++; g_lz_pend = 1; } }   // part of it placed already (an allocation of its own) or refused: copied now, the pool made one allocation at the next tick
        else { for (uint32_t k = c0; k <= c1; k++) L->st[k] = LZ_WANT; L->want = 1;
               int r = gm_lz_run(L, c0, c1 + 1, 1, 0); got = r > 0 ? r : 0;   // this run alone now: the pool's other wanted chunks wait for the tick
               g_lz_pend = 1;   // (the tick looks at the pool's wanted chunks again)
               g_lz_bind += (uint64_t)got; }
    }
    pthread_mutex_unlock(&g_gm_pl);
    return got > 0;
}
// stage_set, the replay's thread under g_bk: a storage buffer [addr, addr + n) that a shader binding the raw pool table reads. Its first 4 KiB, and the first 4 KiB at each word
// in them pointing into a chunked pool, are read for such words; the chunk each lands in and the next (a record or an array may run on) are wanted and placed now, before the
// draw records its table. Each buffer is scanned again at most every 100 ms: in Kokiri Forest every binding scanned in every chunk cost the replay about 3-5 ms of CPU a frame
static void gm_lz_ptr_scan(uintptr_t addr, size_t n, int depth, gm_lz **hit, int *nhit) {   // g_gm_pl held
    const uint64_t *w = (const uint64_t *)(addr & ~(uintptr_t)7); size_t nw = (n < 4096 ? n : 4096) / 8; g_lz_ptr_scans++;
    for (size_t k = 0; k < nw; k++) {
        uint64_t v = w[k]; gm_lz *L = v >> 40 ? NULL : gm_lz_of((uintptr_t)v); if (!L) continue;
        g_lz_ptr_hits++; uint32_t c = (uint32_t)(((uintptr_t)v - L->lo) / GM_CHUNK); int got = 0;
        if (!L->raw) { L->raw = 1; g_lz_raw_pools++; L->want = 1; g_lz_pend = 1; }   // the shader may index anywhere in the pool from here: its written chunks go at the next tick (gm_lz_scan)
        for (uint32_t x = c; x < L->nch && x <= c + 1; x++) if (L->st[x] == LZ_NO) { L->st[x] = LZ_WANT; L->want = 1; g_lz_ptr++; got = 1; g_lz_pend = 1; }
        if (got) { int dup = 0; for (int i = 0; i < *nhit && !dup; i++) dup = hit[i] == L; if (!dup && *nhit < 8) hit[(*nhit)++] = L; }
        if (!depth) gm_lz_ptr_scan((uintptr_t)v, L->hi - (uintptr_t)v, 1, hit, nhit);
    }
}
static void gm_lazy_ptrs(uint64_t addr, size_t n) {
    if (!atomic_load(&g_lz_any) || !addr || !n || addr >> 40) return;
    static struct { uint64_t a, t; } memo[1024]; unsigned h = (unsigned)((addr >> 4) ^ (addr >> 14)) & 1023; uint64_t now = gm_now();
    if (memo[h].a == addr && now - memo[h].t < 100000000ull) return;
    if (pthread_mutex_trylock(&g_gm_pl)) { g_lz_busy++; return; }   // (lock order g_gm_pl before g_bk, as gm_lazy_bind)
    memo[h].a = addr; memo[h].t = now;
    gm_lz *hit[8]; int nhit = 0; gm_lz_ptr_scan((uintptr_t)addr, n, 0, hit, &nhit);
    for (int i = 0; i < nhit; i++) gm_lz_place(hit[i], 1, 0);   // a pending write over one keeps it wanted for the tick
    g_lz_ptr_ns += gm_now() - now;   // the scans and their placements, on the replay's thread
    pthread_mutex_unlock(&g_gm_pl);
}
// A storage buffer across two placed pieces of a chunked pool can't be bound in place (two allocations), and it is copied as before: the game fills its dynamic pools a little
// further each frame, so the scan places neighbouring chunks at different times and buffers across them fell back to the per-draw copy and compare (Kokiri Forest: 26,420 such
// bindings in a run, 2.1 fps against 10.2 with whole pools). Such a pool is made one allocation at the next submission's start (gm_lz_merge), the whole pool: it is a pool the game
// writes all over in time. gm_inplace asks for it (the replay's thread, g_gm_mu not held)
static void gm_lazy_cross(uint64_t addr) {
    if (!atomic_load(&g_lz_any) || pthread_mutex_trylock(&g_gm_pl)) return;
    gm_lz *L = gm_lz_of((uintptr_t)addr); if (L && !L->merge) { L->merge = 1; g_lz_cross++; g_lz_pend = 1; }
    pthread_mutex_unlock(&g_gm_pl);
}
// The whole pool as one allocation (pieces of at most g_gm_piece): the GPU waited for, the tracker's pages opened, the pool frozen (reads too), its pieces given back to
// anonymous pages, then placed again in one swap. The replay's thread at a submission's start, g_bk and g_gm_pl held
static void gm_lz_merge(gm_lz *L) {
    size_t size = L->hi - L->lo, add = 0;
    for (uint32_t k = 0; k < L->nch; k++) if (L->st[k] != LZ_PLACED) add += gm_lz_end(L, k) - (L->lo + (uintptr_t)k * GM_CHUNK);
    L->merge = 0;
    if (g_gm_cap && g_gm_bytes + add > g_gm_cap) { g_gm_capped++; return; }
    uint64_t t0 = gm_now();
    all_wait(); g_gm_waits++;   // the old pieces' last readers (in-place bindings, raw loads) and every pending write done
    nvn_dirty_open((const void *)L->lo, size);
    atomic_store(&g_frz_reads, 1); int frz = gm_frz_begin(L->lo, L->hi);
    static gm_seg *got; static int cap; int n = gm_seg_take(L->pool, L->lo, L->hi, &got, &cap), kept = 0;
    for (int k = 0; k < n; k++) kept += !gm_seg_giveback(&got[k]);
    gm_note fail = {0}, probe = {0}; int r = 0;
    if (!kept) r = gm_swap(L->pool, L->lo, size, 0, 1, 1, &fail, &probe);   // live 0: the GPU is idle and the tracker open already; it freezes the range again itself
    if (frz) gm_frz_end(L->lo, L->hi);
    atomic_store(&g_frz_reads, 0);
    nvn_dirty_close();
    uintptr_t done = L->lo + (uintptr_t)r * (uintptr_t)g_gm_piece; if (done > L->hi) done = L->hi;
    for (uint32_t k = 0; k < L->nch; k++) L->st[k] = gm_lz_end(L, k) <= done ? LZ_PLACED : LZ_REFUSED;
    g_lz_merges++; g_lz_merge_ns += gm_now() - t0; g_lz_merge_bytes += size;
    if (kept) LOG("[RED] [vk] raw global memory: merging pool %#lx+%#zx: %d pieces could not be given back, so its chunks are left as they are", (unsigned long)L->lo, size, kept);
    if (fail.what) gm_note_log(&fail);
    if (probe.what) gm_note_log(&probe);
    static int nl; if (nl++ < 8) LOG("[vk] raw global memory: chunked pool %#lx+%#zx made one allocation (a binding crossed two of its %d pieces), %.1f ms", (unsigned long)L->lo, size, n, (gm_now() - t0) / 1e6);
}
// mtl_sub_begin, the replay's thread under g_bk, between submissions: wanted chunks placed (waiting for pending writes if need be), and every 200 ms the chunks written since
static void gm_lazy_tick(void) {
    if (!atomic_load(&g_lz_any)) return;
    static uint64_t last; uint64_t now = gm_now(); int scan = now - last > 200000000ull;
    if (pthread_mutex_trylock(&g_gm_pl)) { g_lz_busy++; return; }
    int wr = scan && gm_lz_written();   // the written-pages scan: every pool with DM_VK_RAW_GMEM_WRITTEN=1; otherwise the pools a pointer reached, and only pools with a merge or a wanted chunk are placed (thousands of pools)
    // the pools visited only when one may have work: every submission walked all of them (4,041 in Kokiri Forest, about 73 submissions a frame), 2% of Ocarina of Time's replay
    // thread on a Galaxy Z Fold 8 for pools with nothing to do
    if (scan || g_lz_pend) { int pend = 0;
        for (int i = 0; i < g_nlz; i++) { gm_lz *L = &g_lz[i]; if (L->merge) { gm_lz_merge(L); continue; } if (wr || (scan && L->raw)) gm_lz_scan(L); if (L->want) gm_lz_place(L, 1, 1); pend |= L->want || L->merge; }
        g_lz_pend = pend; }
    if (scan) last = now;
    pthread_mutex_unlock(&g_gm_pl);
}
static void gm_lazy_stat(void) {
    LOG("[vk] arena blocks kept by the chunks: %llu, %.1f MiB (gfx_vk_draw.inc ar_new)", (unsigned long long)g_ar_blocks, g_ar_bytes / 1048576.0);
    if (!g_lz_pools) return;
    uint64_t placed = 0, all = 0; pthread_mutex_lock(&g_gm_pl);
    for (int i = 0; i < g_nlz; i++) for (uint32_t k = 0; k < g_lz[i].nch; k++) { uint64_t len = gm_lz_end(&g_lz[i], k) - (g_lz[i].lo + (uintptr_t)k * GM_CHUNK); all += len; if (g_lz[i].st[k] == LZ_PLACED) placed += len; }
    int n = g_nlz; pthread_mutex_unlock(&g_gm_pl);
    LOG("[vk] raw global memory in chunks: %d pools of %.1f MiB, %.1f MiB of them placed; chunks placed for a binding %llu, wanted for pointers %llu (%llu buffers scanned in %.0f ms with the placements they made, %llu words pointing into chunked pools, %llu pools reached), wanted for written pages %llu, kept back by a pending write %llu, "
        "refused %llu (%llu runs over another pool's placed piece), pools merged for a binding across two pieces %llu (%.1f MiB, %.0f ms), bindings across a placed chunk %llu, placements put off (lock busy) %llu; %llu scans (%.2f ms in all); writes that waited for a swap %llu, host writes %llu; raw stores into chunks not placed "
        "(DM_VK_RAW_GMEM_SINK) %llu (cumulative; gfx_vk_gmem_lazy.inc)",
        n, all / 1048576.0, placed / 1048576.0, (unsigned long long)g_lz_bind, (unsigned long long)g_lz_ptr, (unsigned long long)g_lz_ptr_scans, g_lz_ptr_ns / 1e6, (unsigned long long)g_lz_ptr_hits, (unsigned long long)g_lz_raw_pools, (unsigned long long)g_lz_res, (unsigned long long)g_lz_waited, (unsigned long long)g_lz_refused, (unsigned long long)g_lz_overlap,
        (unsigned long long)g_lz_merges, g_lz_merge_bytes / 1048576.0, g_lz_merge_ns / 1e6, (unsigned long long)g_lz_cross, (unsigned long long)g_lz_busy, (unsigned long long)g_lz_scans, g_lz_scan_ns / 1e6,
        (unsigned long long)atomic_load(&g_frz_waits), (unsigned long long)atomic_load(&g_frz_preps), (unsigned long long)atomic_load(&g_sink_hits));
}

// ---- DM_VK_RAW_GMEM_SINK=1 (env.txt; a diagnostic): raw stores into chunks not placed, counted ----
// A raw store to an address missing from the table goes nowhere and leaves no trace. With the switch, each chunk not placed gets a table entry onto one zeroed 2 MiB sink buffer,
// so such a store sets that entry's bit in its write log, which gm_chunk_done reads: the stores a chunked pool would lose are counted and the first chunks named. Raw loads
// from those chunks then read the sink (zero unless such a store wrote it) instead of 0
static int gm_sink_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_RAW_GMEM_SINK"); v = e && *e == '1'; if (v) LOG("[vk] DM_VK_RAW_GMEM_SINK=1: chunks not placed are in the raw pool table, onto a sink, so raw stores into them are counted"); } return v; }
static VkBuffer g_sink_b; static VkDeviceMemory g_sink_m; static uint64_t g_sink_gpu;   // (g_sink_hits: gfx_vk_gmem.inc)
static int gm_sink_make(void) {
    if (g_sink_gpu) return 1;
    VkBufferCreateInfo bc = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = GM_CHUNK, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR};
    if (vkCreateBuffer(g_vk.dev, &bc, NULL, &g_sink_b) != VK_SUCCESS) return 0;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_vk.dev, g_sink_b, &mr);
    uint32_t mt = vkb_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateFlagsInfo fi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT_KHR};
    VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &fi, .allocationSize = mr.size, .memoryTypeIndex = mt};
    void *p = NULL;
    if (mt == UINT32_MAX || vkAllocateMemory(g_vk.dev, &ma, NULL, &g_sink_m) != VK_SUCCESS) { vkDestroyBuffer(g_vk.dev, g_sink_b, NULL); g_sink_b = VK_NULL_HANDLE; return 0; }
    vkBindBufferMemory(g_vk.dev, g_sink_b, g_sink_m, 0);
    if (vkMapMemory(g_vk.dev, g_sink_m, 0, VK_WHOLE_SIZE, 0, &p) == VK_SUCCESS) { memset(p, 0, (size_t)mr.size); vkUnmapMemory(g_vk.dev, g_sink_m); }
    VkBufferDeviceAddressInfoKHR ai = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO_KHR, .buffer = g_sink_b}; g_sink_gpu = g_gm_bda(g_vk.dev, &ai);
    if (g_gm_np && g_sink_gpu) { pthread_mutex_lock(&g_gm_mu); gm_np_write(0, g_sink_b, GM_CHUNK); pthread_mutex_unlock(&g_gm_mu); }   // slot 0 (gfx_vk_gmem_noptr.inc)
    return g_sink_gpu != 0;
}
// gm_table: the chunks not placed as ranges onto the sink, appended to *seg from n on (grown as needed). → the new count. Skipped when the placement lock is busy (the next build has them)
static int gm_sink_ranges(uint64_t (**seg)[3], int *cap, int n) {
    if (!gm_sink_on() || !atomic_load(&g_lz_any) || !gm_sink_make() || pthread_mutex_trylock(&g_gm_pl)) return n;
    for (int i = 0; i < g_nlz; i++) for (uint32_t k = 0; k < g_lz[i].nch; k++) {
        if (g_lz[i].st[k] == LZ_PLACED) continue;
        if (n == *cap) { *cap = *cap ? 2 * *cap : 1024; *seg = realloc(*seg, (size_t)*cap * sizeof **seg); if (!*seg) die("raw global memory: the table's ranges"); }
        uint64_t a = g_lz[i].lo + (uint64_t)k * GM_CHUNK; (*seg)[n][0] = a; (*seg)[n][1] = gm_lz_end(&g_lz[i], k); (*seg)[n++][2] = (g_gm_np ? 0 : g_sink_gpu) - a; }
    pthread_mutex_unlock(&g_gm_pl);
    return n;
}
static int gm_sink_is(const uint64_t e[3]) { return g_sink_gpu && e[0] + e[2] == (g_gm_np ? 0 : g_sink_gpu); }
