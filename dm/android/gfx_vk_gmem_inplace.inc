// ---- Storage buffers bound in their placed pools (DM_VK_INPLACE) ----
// A storage buffer a shader only reads was copied into the arena once per chunk and compared whole with that copy at every later draw of the chunk (ucopy), unless the
// geometry cache kept it (sb_keep: up to 8 MB, and not written by the game in two frames running). Ocarina of Time's Kokiri Forest reads buffers the game writes every frame:
// on the Galaxy Z Fold 8 that compare was 83% of the replay thread (simpleperf: __memcmp_aarch64 under ucopy, stage_set's read-only storage buffer line), and the copies
// about 165 MB a second to the arena and as much again to the shadows. A placed pool's pages are the GPU's own memory (gfx_vk_gmem.inc), so such a buffer is bound as the
// piece's VkBuffer at its offset: no copy and no compare, and the GPU reads what the game wrote, as on the console.
// - The GPU reads it when it runs, not when the replay records. The game's own writes are ordered by its fences, which complete when the GPU has run the work before them
//   (mtl_sub_commit), as on the console. The replay's writes are not: it writes guest memory on the CPU while it records (uniform buffer updates, small copies and fills:
//   mem_view), and a draw recorded before that write and still to run would read the new bytes. So each page read in place keeps the chunk that last read it (gm_seg rd),
//   and a write by the replay to such a page waits for that chunk first (gm_ip_write: the chunk being recorded is submitted and waited for, one in flight waited for).
//   Write-backs, linear render targets' and large copies land in guest memory when their chunk completes, after everything recorded before them has run; a draw recorded
//   after them lands them first when it reads their range (draw_view), and a dispatch's pending write is bound on the GPU instead (sb_fwd), as before.
// - Not in place: a buffer in no placed piece (raw global memory off, a pool the game never took a buffer address in), one that crosses two pieces, and one at an offset
//   that is not a multiple of minStorageBufferOffsetAlignment. One the shader writes is in place too (gm_inplace_w, below).
// DM_VK_INPLACE=0 (env.txt): copied as before
static int ip_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_INPLACE"); v = !(e && *e == '0'); if (!v) LOG("[vk] DM_VK_INPLACE=0: read-only storage buffers in placed pools are copied as before"); } return v; }
static uint64_t g_ip_binds, g_ip_bytes, g_ip_span, g_ip_unal, g_ip_wcur, g_ip_wfly, g_ip_wfree, g_ip_wnone;
static int g_ip_any;   // a buffer was bound in place (the replay's writes look only then)
static int gm_seg_at(uintptr_t a) {   // the piece holding a, or -1 (caller holds g_gm_mu)
    int lo = 0, hi = g_ngm - 1;
    while (lo <= hi) { int m = (lo + hi) / 2; if (a < g_gm[m].lo) hi = m - 1; else if (a >= g_gm[m].hi) lo = m + 1; else return m; }
    return -1;
}
// a read-only storage buffer [addr, addr + n): 1 with *b, *off, *range the piece's buffer to bind; 0: copy it as before. The replay's thread, under g_bk, with a chunk open
// al: the binding's offset alignment (a storage buffer's for gm_inplace; 4 for an indirect dispatch's arguments, vk_dispatch)
static int gm_inplace_al(uint64_t addr, size_t n, VkDeviceSize al, VkBuffer *b, VkDeviceSize *off, VkDeviceSize *range) {
    if (!n || n > g_vk.props.limits.maxStorageBufferRange || !C || (!g_ngm && !atomic_load(&g_lz_any)) || !ip_on() || !gm_on()) return 0;   // (nothing placed yet but chunked pools: a binding may place its chunks)
    static struct { uint64_t a, n, seq; } memo[512];   // the stamps already made by this chunk: a buffer is often bound by many draws of one chunk
    unsigned h = (unsigned)((addr >> 6) ^ (addr >> 17) ^ n) & 511;
    if (!al) al = 1;
    pthread_mutex_lock(&g_gm_mu);
    int i = gm_seg_at((uintptr_t)addr);
    if (i < 0) {   // in a chunked pool, its chunks not placed yet: placed now, for this binding (gfx_vk_gmem_lazy.inc)
        pthread_mutex_unlock(&g_gm_mu);
        if (!gm_lazy_bind(addr, n)) return 0;
        pthread_mutex_lock(&g_gm_mu);
        if ((i = gm_seg_at((uintptr_t)addr)) < 0) { pthread_mutex_unlock(&g_gm_mu); return 0; } }
    const gm_seg *s = &g_gm[i]; VkDeviceSize o = (VkDeviceSize)(addr - s->lo);
    if (addr + n > s->hi) { pthread_mutex_unlock(&g_gm_mu); g_ip_span++; gm_lazy_cross(addr); return 0; }   // (a chunked pool: made one allocation at the next submission, gfx_vk_gmem_lazy.inc)
    if (o % al) { pthread_mutex_unlock(&g_gm_mu); g_ip_unal++; return 0; }
    if (s->rd && !(memo[h].a == addr && memo[h].n == n && memo[h].seq == C->seq)) {
        uint64_t sq = C->seq; for (size_t p = (size_t)(o >> 12), e = (size_t)((o + n - 1) >> 12); p <= e; p++) s->rd[p] = sq;
        memo[h].a = addr; memo[h].n = n; memo[h].seq = sq;
    }
    VkDeviceSize r = (n + 15) & ~(VkDeviceSize)15; if (o + r > (VkDeviceSize)(s->hi - s->lo)) r = n;   // bound to the 16-byte range, as the copies are, within the piece
    *b = s->b; *off = o; *range = r;
    pthread_mutex_unlock(&g_gm_mu);
    g_ip_binds++; g_ip_bytes += n; g_ip_any = 1;
    return 1;
}
// A storage buffer the shader writes, in a placed pool: bound there too, with a write-back that copies nothing (wb_t inpl). CPU reads of its range wait for its chunk as they did
// for the copy's write-back (chunk_hits), a later dispatch reads it on the GPU from the piece (fwd_hit), and when the chunk completes its pages are prepared (chunk_done:
// counted as written, so a kept copy or a texture over them is made again, and writable). It was copied into the arena with a snapshot, and written back by comparing the
// two: in Kokiri Forest about 101 write-backs and 49 MB a frame, copied twice by the replay thread and compared by the completion thread (13.9 ms a frame).
// DM_VK_INPLACE_WRITE=0 (env.txt): written ones copied and written back as before
static int ip_w_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_INPLACE_WRITE"); v = !(e && *e == '0'); if (!v) LOG("[vk] DM_VK_INPLACE_WRITE=0: written storage buffers are copied and written back as before"); } return v; }
static uint64_t g_ip_wbinds, g_ip_wbytes;
// DM_VK_UB_INPLACE=1 (env.txt; off by default): a game uniform buffer in a placed pool is bound there, as a read-only storage buffer is: no copy into the arena, no shadow and no
// compare (ucopy). The replay's own writes are ordered as for storage buffers (gm_ip_write: one to a page read in place waits for that chunk, so a buffer on a page the replay
// wrote lately, mvw_hit, is copied rather than made to wait). The game's are not: the GPU reads the buffer when the chunk runs, and Ocarina of Time, ahead of the replay,
// rewrites some uniform buffers before then, so only one the game has left alone is bound in place (ub_stable.h, below): DM_VK_UB_INPLACE_CHECK then found none of 1.7 million
// changed before their chunk completed, against 573 of 7.7 million when every buffer was bound in place. Off because in that game it saves nothing: most of its uniform buffers
// are written every frame, so only a few percent of its binds qualify, and the replay thread measured +0.6 +/- 0.6 ms a frame with it on (1.8 ms less with every buffer in place)
static int ub_inpl_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_UB_INPLACE"); v = e && *e == '1'; if (v) LOG("[vk] DM_VK_UB_INPLACE=1: uniform buffers in placed pools the game has left alone are bound in place"); } return v; }
static int mvw_hit(uint64_t a, uint64_t n);   // gfx_vk.c
static uint64_t g_ub_inpl, g_ub_inpl_mvw;   // uniform buffers bound in place; not, on a page the replay wrote lately
// DM_VK_UB_INPLACE_CHECK=1 (env.txt; a diagnostic, slow): each uniform buffer bound in place keeps its bytes at the bind (once a chunk per address and size), and the chunk compares
// them with guest memory when it completes, before its own writes land. One that changed was written while the chunk's draws could still read it, where a copy made at the bind
// would have kept the bytes it had then: counted, the first 8 logged
static int ub_inpl_check(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_UB_INPLACE_CHECK"); v = e && *e == '1'; if (v) LOG("[vk] DM_VK_UB_INPLACE_CHECK=1: uniform buffers bound in place are compared with their bytes at the bind when their chunk completes"); } return v; }
static _Atomic uint64_t g_ubk_n, g_ubk_diff, g_ubk_gone;   // compared; changed; in no pool by then (not compared)
static void ubk_note(uint64_t addr, size_t n) {
    static struct { uint64_t a, n, seq; } memo[1024]; unsigned h = (unsigned)((addr >> 4) ^ (addr >> 16) ^ n) & 1023;
    if (memo[h].a == addr && memo[h].n == n && memo[h].seq == C->seq) return;
    size_t need = C->nubk + 16 + ((n + 7) & ~(size_t)7);
    if (need > C->cubk) { size_t c = need * 2 > 65536 ? need * 2 : 65536; uint8_t *p = realloc(C->ubk, c); if (!p) return; C->ubk = p; C->cubk = c; }
    uint8_t *d = C->ubk + C->nubk; uint64_t nn = n; memcpy(d, &addr, 8); memcpy(d + 8, &nn, 8); memcpy(d + 16, (const void *)(uintptr_t)addr, n); C->nubk = need;
    memo[h].a = addr; memo[h].n = n; memo[h].seq = C->seq;
}
static void ub_inpl_verify(chunk *c) {   // the completion thread, first in chunk_done
    for (size_t k = 0; k + 16 <= c->nubk;) { uint64_t a, n; memcpy(&a, c->ubk + k, 8); memcpy(&n, c->ubk + k + 8, 8);
        if (!mtl_addr(a, n, NULL, NULL)) atomic_fetch_add(&g_ubk_gone, 1);
        else if (atomic_fetch_add(&g_ubk_n, 1), memcmp((const void *)(uintptr_t)a, c->ubk + k + 16, n)) { uint64_t d = atomic_fetch_add(&g_ubk_diff, 1);
            if (d < 8) LOG("[RED] [vk] DM_VK_UB_INPLACE_CHECK: the uniform buffer at %#llx (%llu bytes) bound in place in chunk %llu changed before the chunk completed", (unsigned long long)a, (unsigned long long)n, (unsigned long long)c->seq); }
        k += 16 + ((n + 7) & ~7ull); }
    c->nubk = 0;
}
// Only a buffer the game has left alone is bound in place (ub_stable.h: its pages stamped and write-protected at its first bind, and none written for UBS_STABLE presents since).
// One the game rewrites every frame stays a copy, at a fault every UBS_BACKOFF presents
static const void *ubs_span(const void *a, size_t n) { return nvn_dirty_span(a, n); }
static uint64_t ubs_now(void) { return nvn_dirty_now(); }
static const ubs_ops g_ubs_ops = {ubs_span, ubs_now, nvn_dirty_protect_span, nvn_dirty_since_span};
enum { UBS_N = 16384 };
static ubs_ent g_ubs[UBS_N]; static uint64_t g_ubs_st[3];   // entries made, buffers found written, binds copied while still within UBS_STABLE
static int ub_inplace(uint64_t addr, size_t n, VkBuffer *b, VkDeviceSize *off, size_t *range) {
    if (!ub_inpl_on() || !n || n > g_vk.props.limits.maxUniformBufferRange) return 0;
    if (mvw_hit(addr, n)) { g_ub_inpl_mvw++; return 0; }
    if (!ubs_look(ubs_slot(g_ubs, UBS_N - 1, addr, n), &g_ubs_ops, addr, n, atomic_load(&g_presents), g_ubs_st)) return 0;
    VkDeviceSize r; if (!gm_inplace_al(addr, n, g_vk.props.limits.minUniformBufferOffsetAlignment, b, off, &r)) return 0;
    g_ip_binds--; g_ip_bytes -= n; g_ub_inpl++;   // (counted here, not as a storage buffer)
    if (ub_inpl_check()) ubk_note(addr, n);
    *range = r > g_vk.props.limits.maxUniformBufferRange ? n : (size_t)r;
    return 1;
}
static int gm_inplace(uint64_t addr, size_t n, VkBuffer *b, VkDeviceSize *off, VkDeviceSize *range) {
    return gm_inplace_al(addr, n, g_vk.props.limits.minStorageBufferOffsetAlignment, b, off, range);
}
static int gm_inplace_w(uint64_t addr, size_t n, int cs, VkBuffer *b, VkDeviceSize *off, VkDeviceSize *range) {
    if (!ip_w_on() || !gm_inplace(addr, n, b, off, range)) return 0;
    g_ip_binds--; g_ip_bytes -= n;   // (counted as written, not read)
    wb_t *w = wb_add(); if (!w) return 0;
    *w = (wb_t){0, (size_t)*off, n, 1, n, n, (uint8_t *)(uintptr_t)addr, NULL, cs}; w->ipb = *b; w->inpl = 1;
    if (cs) C->nfwd++; else C->nipw++;   // a draw's: read where it lies by a later dispatch (gfx_vk_draw.inc fwd_hit)
    C->ip_wr = 1; g_ip_wbinds++; g_ip_wbytes += n;
    return 1;
}
// A small buffer fill (nvnCommandBufferClearBuffer) over memory the GPU still writes in place (a storage buffer written in its placed pool, raw stores), recorded as a fill on
// the GPU in that pool, after a full barrier, instead of a fill on the CPU: the CPU's had to wait until the chunk holding the write completed (mem_view: the chunk submitted and
// waited for), which Ocarina of Time met once a frame in Kokiri Forest, about 10 ms of the replay thread's wall time a frame on a Galaxy Z Fold 8 with the GPU drained. Its
// entry is an in-place write-back as gm_inplace_w makes (CPU reads of the range wait for the chunk, later dispatches read it in the pool, and its pages count as written when the
// chunk completes). Only when every pending write over the range lands in place: one that lands from a copy afterwards (an arena write-back, a queued copy) would land over the
// GPU's fill, so the CPU waits for it and fills, as before. DM_VK_FILL_GPU=0 (env.txt): every small fill on the CPU, as before
static int fill_target(uintptr_t lo, uintptr_t hi, VkBuffer *b, VkDeviceSize *o);   // gfx_vk.c
static int fill_gpu_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_FILL_GPU"); v = !(e && *e == '0'); if (!v) LOG("[vk] DM_VK_FILL_GPU=0: small buffer fills are made on the CPU"); } return v; }
static uint64_t g_fill_gpu, g_fill_arena;   // fills made on the GPU in place / in a write-back's arena copy
static int gm_fill_gpu(uint64_t dst, uint64_t n, uint32_t value) {
    if (!fill_gpu_on() || !n || ((dst | n) & 3) || vk_safe() || !cur()) return 0;
    VkBuffer b; VkDeviceSize off, rng;
    int k = fill_target((uintptr_t)dst, (uintptr_t)(dst + n), &b, &off);
    if (!k) return 0;   // nothing pending over it (the CPU fill waits for nothing), or a write the fill can't follow on the GPU
    if (k == 1) {   // in the write-back's arena copy, which lands it with the rest: the write-back already makes CPU reads of the range wait and later dispatches read the copy
        if (off & 3) return 0;
        VkCommandBuffer c = cb(); if (!c) return 0;   // (outside the pass; a run of dispatches closed)
        cb_barrier(); vkCmdFillBuffer(c, b, off, n, value); cb_barrier();
        g_fill_arena++; return 1; }
    if (!gm_on()) return 0;
    pthread_mutex_lock(&g_gm_mu); int placed = gm_seg_at((uintptr_t)dst) >= 0; pthread_mutex_unlock(&g_gm_mu);
    if (!placed) return 0;   // (a chunk's raw stores reach only placed memory: none is placed for the fill)
    if (!gm_inplace_al(dst, n, 4, &b, &off, &rng)) return 0;
    VkCommandBuffer c = cb(); if (!c) return 0;
    wb_t *w = wb_add(); if (!w) return 0;
    *w = (wb_t){0, (size_t)off, n, 1, n, n, (uint8_t *)(uintptr_t)dst, NULL, 1}; w->ipb = b; w->inpl = 1; C->nfwd++; C->ip_wr = 1;
    cb_barrier(); vkCmdFillBuffer(c, b, off, n, value); cb_barrier();
    g_fill_gpu++; return 1;
}
// The completion thread, for chunk c's write-back i written in place: its pages prepared, once per range in the chunk. Preparing costs a walk of every tracked range
// (nvn_dirty.c prep_write: thousands in Ocarina of Time, 98% of the completion thread on the Fold), and a buffer many draws write gets an entry for each
static _Atomic uint64_t g_ip_prep, g_ip_prep_dup;
static void gm_ip_done(const chunk *c, int i) {
    const wb_t *w = &c->wb[i];
    for (int k = 0; k < i; k++) if (c->wb[k].inpl && c->wb[k].dst == w->dst && c->wb[k].row_bytes >= w->row_bytes) { atomic_fetch_add(&g_ip_prep_dup, 1); return; }
    dm_guest_write_prepare(w->dst, w->row_bytes); atomic_fetch_add(&g_ip_prep, 1);
}
// DM_VK_RAW_GMEM_WSYNC=1 (env.txt; a diagnostic): a raw load may read any placed page, at the GPU's time, so the replay's write to a placed page waits for every chunk still to
// run that bound the raw pool table, not only for those that read its pages in place (the timing hazard the reference entry lists as open)
static int gm_wsync(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_RAW_GMEM_WSYNC"); v = e && *e == '1'; if (v) LOG("[vk] DM_VK_RAW_GMEM_WSYNC=1: the replay's writes to placed pages wait for chunks that bound the raw pool table"); } return v; }
static uint64_t g_gm_wsyncs;   // writes that waited for a chunk that bound the table (g_gm_rd_seq: its last, gm_bind)
// The replay is about to write guest memory [lo, hi) on the CPU (mem_view): a chunk still to run that reads those pages in place runs first
static void gm_ip_write(uintptr_t lo, uintptr_t hi) {
    int ws = gm_wsync() && g_gm_rd_seq;
    if ((!g_ip_any && !ws) || hi <= lo) return;
    uint64_t need = 0;
    pthread_mutex_lock(&g_gm_mu);
    int a = 0, z = g_ngm; while (a < z) { int m = (a + z) / 2; if (g_gm[m].hi <= lo) a = m + 1; else z = m; }   // the first piece ending past lo
    if (ws && a < g_ngm && g_gm[a].lo < hi && g_gm_rd_seq > need) { need = g_gm_rd_seq; g_gm_wsyncs++; }
    for (int i = a; i < g_ngm && g_gm[i].lo < hi; i++) {
        const gm_seg *s = &g_gm[i]; if (!s->rd) continue;
        uintptr_t x = lo > s->lo ? lo : s->lo, y = hi < s->hi ? hi : s->hi;
        for (size_t p = (x - s->lo) >> 12, e = (y - 1 - s->lo) >> 12; p <= e; p++) if (s->rd[p] > need) need = s->rd[p];
    }
    pthread_mutex_unlock(&g_gm_mu);
    if (!need) { g_ip_wnone++; return; }
    pthread_mutex_lock(&g_cmu); uint64_t done = g_seq_done; pthread_mutex_unlock(&g_cmu);
    if (need <= done) { g_ip_wfree++; return; }
    if (C && need >= C->seq) { g_ip_wcur++; submit_wait(); }
    else { g_ip_wfly++; seq_wait(need); }
}
static void gm_ip_stat(void) {
    if (!g_ip_binds && !g_ip_wbinds && !g_ip_span && !g_ip_unal && !g_gm_wsyncs && !g_fill_gpu && !g_fill_arena && !g_ub_inpl) return;
    LOG("[vk] storage buffers bound in their placed pools: %llu read-only bindings (%llu MB read in place), %llu written ones (%llu MB), not in place %llu crossing two pieces, %llu at an offset not a multiple of %llu; "
        "the replay's CPU writes over pages read in place: %llu waited for the chunk being recorded, %llu for one in flight, %llu after it had run, %llu over none; written ranges prepared %llu, "
        "again in their chunk (skipped) %llu; writes over placed pages after a table binding (DM_VK_RAW_GMEM_WSYNC) %llu; small fills made on the GPU after pending writes: in place %llu, in a write-back's arena copy %llu (DM_VK_FILL_GPU); uniform buffers bound in place %llu, copied as on a page the replay wrote lately %llu, as written by the game %llu times, still young %llu (%llu tracked) (DM_VK_UB_INPLACE); DM_VK_UB_INPLACE_CHECK: %llu compared at their chunk's end, %llu changed, %llu in no pool by then (cumulative; gfx_vk_gmem_inplace.inc)",
        (unsigned long long)g_ip_binds, (unsigned long long)(g_ip_bytes >> 20), (unsigned long long)g_ip_wbinds, (unsigned long long)(g_ip_wbytes >> 20), (unsigned long long)g_ip_span, (unsigned long long)g_ip_unal,
        (unsigned long long)g_vk.props.limits.minStorageBufferOffsetAlignment, (unsigned long long)g_ip_wcur, (unsigned long long)g_ip_wfly, (unsigned long long)g_ip_wfree, (unsigned long long)g_ip_wnone,
        (unsigned long long)atomic_load(&g_ip_prep), (unsigned long long)atomic_load(&g_ip_prep_dup), (unsigned long long)g_gm_wsyncs, (unsigned long long)g_fill_gpu, (unsigned long long)g_fill_arena, (unsigned long long)g_ub_inpl, (unsigned long long)g_ub_inpl_mvw, (unsigned long long)g_ubs_st[1], (unsigned long long)g_ubs_st[2], (unsigned long long)g_ubs_st[0],
        (unsigned long long)atomic_load(&g_ubk_n), (unsigned long long)atomic_load(&g_ubk_diff), (unsigned long long)atomic_load(&g_ubk_gone));
}
