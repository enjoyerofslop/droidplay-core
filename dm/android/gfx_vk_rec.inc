// The command recorder: the replay's vkCmd* calls made on a thread of their own (included by gfx_vk.c before the rest of the replay, which then calls the rec_ wrappers
// below through the macros at the end of this file; front_android.c, another file, calls the driver directly).
// Why: the replay is one thread, and the Adreno driver's command recording was about a fifth of it. Breath of the Wild on a OnePlus Pad 3 (about 3,600 draws a frame, the
// replay 30.5 ms of CPU a frame): vkCmdDrawIndexed 9.9% of the replay thread, vkCmdBindDescriptorSets 8.2%, vkCmdBeginRenderPass 1.4%, and the binds, copies and dynamic
// state besides (simpleperf). Each wrapper copies its call's arguments, arrays and structures included, into a ring, and the recorder thread makes the calls in order on
// the same command buffers; it also ends each command buffer and submits it (rec_submit), so the replay never waits for it but once a frame, at the present (rec_drain).
// The idea is nx2android's (784c9d4, DM_VK_MT_REPLAY), written anew here. Measured there (with descriptor set reuse and 512 draws a chunk): the replay's CPU 30.7 → 24.2 ms
// a frame at 20 fps, and with DM_FPS_TARGET=30 a steady 30.0 fps (frame p95 38.4-38.9 ms, the replay 17.3-17.5 ms, NVN submit+present p95 28.3 ms) where without it the game
// ran at 29.2-29.6 (frame p95 41-44 ms). The recorder thread joins the frame's performance hint session (hint_note) with the threads it records for.
// Why it is safe:
//   - one producer: every vkCmd* of the replay is made under g_bk (the replay, and the CPU-side operations that record into the current chunk, take it), so the records of
//     one command buffer reach the ring in the order the replay made them, and the recorder makes them in that order
//   - vkEndCommandBuffer waits until the recorder has made every call before it (rec_drain), so a command buffer is whole before chunk_submit hands it to the queue, and the
//     completion thread's vkResetCommandPool (after the chunk's fence) never meets a recording in progress. Only the recorder touches a command buffer while the recorder is on
//   - what a call reads through its pointers is copied at the call; handles stay valid until the chunk completes (dead_add), long after the call is made. A descriptor set is
//     written before its bind is queued, and never again in that recording: by the recorder too, as a record ahead of the bind's (R_UPDDS, below), or on the replay
//     thread with DM_VK_REC_DESC=0
//   - a call with a pNext chain this file cannot copy is made directly, after a drain
// DM_VK_RECORDER=1 (env.txt, or a game's android_defaults.txt): on. Off by default (the calls are made on the replay thread, as before) until more games and devices have run it
#include <sched.h>
enum { RQ_SIZE = 8u << 20 };   // about 25,000 records a frame of 30 to 200 bytes: several frames' worth
enum { R_BEGINQ = 1, R_BEGINRP, R_BINDDS, R_BINDIB, R_BINDP, R_BINDVB, R_BLIT, R_CLRATT, R_CLRCOL, R_CLRDS, R_CPB2I, R_CPI, R_CPI2B, R_DISPATCH, R_DRAW, R_DRAWIDX, R_ENDQ, R_ENDRP,
       R_BARRIER, R_PUSH, R_RESETQ, R_RESOLVE, R_BLENDC, R_DBIAS, R_SCISSOR, R_SCMASK, R_SREF, R_SWMASK, R_VIEWPORT, R_TS, R_BEGINCB, R_ENDCB, R_SUBMIT, R_WRAP, R_CPB, R_UPDDS, R_DRAWMESH, R_DISPIND, R_DRAWIDXIND, R_FILLB };
typedef struct { uint32_t fn, n; VkCommandBuffer cb; } rq_hdr;   // n: the record's bytes, header included, a multiple of 16
static uint8_t *g_rq;
static _Atomic uint64_t g_rq_w, g_rq_r;   // bytes written / made since the start (the ring offset is the count modulo RQ_SIZE)
static _Atomic int g_rq_csleep, g_rq_pwait;   // the recorder waits for records / the replay waits for room or for the ring to empty
static pthread_mutex_t g_rq_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_rq_cdata = PTHREAD_COND_INITIALIZER, g_rq_cspace = PTHREAD_COND_INITIALIZER;
static VkResult g_rq_end;   // the last vkEndCommandBuffer's result (the recorder's, read after rec_drain)
static uint64_t g_rq_records, g_rq_drains, g_rq_direct; static _Atomic uint64_t g_rq_waits_ns;   // records queued, drains, calls made directly (pNext); the replay's time waiting for the recorder
static void *rq_thread(void *arg);
static void hint_note(void);   // gfx_vk.c: the recorder is in the frame's performance hint session, with the threads it records for
static void gpu_fault_log(VkResult r);   // gfx_vk.c: what VK_EXT_device_fault says of a lost device, once
static VkResult submit_or_fence(VkCommandBuffer cb, VkFence f, const char *who);   // gfx_vk.c: a submission whose fence reaches the queue even when its work is refused
static int rec_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("DM_VK_RECORDER"); v = e && *e == '1';
        if (v) { g_rq = malloc(RQ_SIZE); pthread_t t; if (!g_rq || pthread_create(&t, NULL, rq_thread, NULL)) { free(g_rq); g_rq = NULL; v = 0; LOG("[RED] [vk] DM_VK_RECORDER=1: the recorder thread could not start"); }
                 else { pthread_detach(t); LOG("[vk] DM_VK_RECORDER=1: the vkCmd* calls are made on a recorder thread"); } } }
    return v;
}
static void rq_wake_c(void) { if (atomic_load(&g_rq_csleep)) { pthread_mutex_lock(&g_rq_mu); pthread_cond_signal(&g_rq_cdata); pthread_mutex_unlock(&g_rq_mu); } }
static void rq_wait_until(uint64_t r_at_least) {   // the replay: until the recorder has made the records up to that count
    if (atomic_load(&g_rq_r) >= r_at_least) return;
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_MONOTONIC);
    for (int i = 0; i < 200 && atomic_load(&g_rq_r) < r_at_least; i++) sched_yield();   // the recorder is usually a few records behind
    if (atomic_load(&g_rq_r) < r_at_least) {
        pthread_mutex_lock(&g_rq_mu); atomic_store(&g_rq_pwait, 1);
        while (atomic_load(&g_rq_r) < r_at_least) { if (atomic_load(&g_rq_csleep)) pthread_cond_signal(&g_rq_cdata); pthread_cond_wait(&g_rq_cspace, &g_rq_mu); }   // g_rq_mu held: signalled directly
        atomic_store(&g_rq_pwait, 0); pthread_mutex_unlock(&g_rq_mu); }
    atomic_fetch_add(&g_rq_waits_ns, clock_gettime_nsec_np(CLOCK_MONOTONIC) - t0);
}
static void rec_drain(void) { if (g_rq) { g_rq_drains++; rq_wake_c(); rq_wait_until(atomic_load(&g_rq_w)); } }
// room for a record of n bytes (header included) → where to write it; rq_push publishes it
static uint8_t *rq_take(uint32_t fn, VkCommandBuffer cb, size_t n) {
    n = (n + 15) & ~(size_t)15;   // a multiple of the header's 16 bytes: what is left before the ring's end always holds a wrap record's header
    uint64_t w = atomic_load(&g_rq_w); size_t at = (size_t)(w % RQ_SIZE);
    if (at + n > RQ_SIZE) {   // the rest of the ring is a wrap record (at least 16 bytes: records are multiples of 16)
        rq_wait_until(w + (RQ_SIZE - at) + n > RQ_SIZE ? w + (RQ_SIZE - at) + n - RQ_SIZE : 0);
        rq_hdr *h = (rq_hdr *)(void *)(g_rq + at); h->fn = R_WRAP; h->n = (uint32_t)(RQ_SIZE - at); w += RQ_SIZE - at; atomic_store(&g_rq_w, w); at = 0; }
    rq_wait_until(w + n > RQ_SIZE ? w + n - RQ_SIZE : 0);
    rq_hdr *h = (rq_hdr *)(void *)(g_rq + at); h->fn = fn; h->n = (uint32_t)n; h->cb = cb;
    return g_rq + at;
}
// published; the recorder woken (when asleep) once RQ_BATCH bytes wait or at a submission: a wake per call cost a futex call each, and the recorder idling between them spun
enum { RQ_BATCH = 64u << 10 };
static void rq_push(const uint8_t *rec) {
    const rq_hdr *h = (const rq_hdr *)(const void *)rec; g_rq_records++;
    uint64_t w = atomic_load(&g_rq_w) + h->n; atomic_store(&g_rq_w, w);
    if (h->fn == R_SUBMIT || h->fn == R_ENDCB || w - atomic_load(&g_rq_r) >= RQ_BATCH) rq_wake_c();
}
#define RQ_ARR(dst, p, cnt) do { if (cnt) memcpy(dst, p, (size_t)(cnt) * sizeof *(p)); dst += (size_t)(cnt) * sizeof *(p); } while (0)   // an array into the record, dst advanced
#define RQ_GET(src, T, cnt) ((const T *)(const void *)((src += (size_t)(cnt) * sizeof(T)) - (size_t)(cnt) * sizeof(T)))   // an array out of it, src advanced

// ---- The wrappers: with the recorder off, the driver directly ----
typedef struct { VkQueryPool p; uint32_t q; VkQueryControlFlags f; } a_beginq;
static void rec_vkCmdBeginQuery(VkCommandBuffer cb, VkQueryPool p, uint32_t q, VkQueryControlFlags f) {
    if (!rec_on()) { vkCmdBeginQuery(cb, p, q, f); return; }
    uint8_t *r = rq_take(R_BEGINQ, cb, sizeof(rq_hdr) + sizeof(a_beginq)); *(a_beginq *)(void *)(r + sizeof(rq_hdr)) = (a_beginq){p, q, f}; rq_push(r);
}
typedef struct { VkRenderPassBeginInfo bi; VkSubpassContents c; } a_beginrp;
static void rec_vkCmdBeginRenderPass(VkCommandBuffer cb, const VkRenderPassBeginInfo *bi, VkSubpassContents c) {
    if (!rec_on() || bi->pNext) { if (g_rq) { rec_drain(); g_rq_direct++; } vkCmdBeginRenderPass(cb, bi, c); return; }
    uint8_t *r = rq_take(R_BEGINRP, cb, sizeof(rq_hdr) + sizeof(a_beginrp) + bi->clearValueCount * sizeof(VkClearValue)), *d = r + sizeof(rq_hdr) + sizeof(a_beginrp);
    *(a_beginrp *)(void *)(r + sizeof(rq_hdr)) = (a_beginrp){*bi, c}; RQ_ARR(d, bi->pClearValues, bi->clearValueCount); rq_push(r);
}
typedef struct { VkPipelineBindPoint bp; VkPipelineLayout l; uint32_t first, n, ndyn; } a_bindds;
static void rec_vkCmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t first, uint32_t n, const VkDescriptorSet *s, uint32_t ndyn, const uint32_t *dyn) {
    if (!rec_on()) { vkCmdBindDescriptorSets(cb, bp, l, first, n, s, ndyn, dyn); return; }
    uint8_t *r = rq_take(R_BINDDS, cb, sizeof(rq_hdr) + sizeof(a_bindds) + n * sizeof *s + ndyn * sizeof *dyn), *d = r + sizeof(rq_hdr) + sizeof(a_bindds);
    *(a_bindds *)(void *)(r + sizeof(rq_hdr)) = (a_bindds){bp, l, first, n, ndyn}; RQ_ARR(d, s, n); RQ_ARR(d, dyn, ndyn); rq_push(r);
}
typedef struct { VkBuffer b; VkDeviceSize o; VkIndexType t; } a_bindib;
static void rec_vkCmdBindIndexBuffer(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkIndexType t) {
    if (!rec_on()) { vkCmdBindIndexBuffer(cb, b, o, t); return; }
    uint8_t *r = rq_take(R_BINDIB, cb, sizeof(rq_hdr) + sizeof(a_bindib)); *(a_bindib *)(void *)(r + sizeof(rq_hdr)) = (a_bindib){b, o, t}; rq_push(r);
}
typedef struct { VkPipelineBindPoint bp; VkPipeline p; } a_bindp;
static void rec_vkCmdBindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipeline p) {
    if (!rec_on()) { vkCmdBindPipeline(cb, bp, p); return; }
    uint8_t *r = rq_take(R_BINDP, cb, sizeof(rq_hdr) + sizeof(a_bindp)); *(a_bindp *)(void *)(r + sizeof(rq_hdr)) = (a_bindp){bp, p}; rq_push(r);
}
typedef struct { uint32_t first, n; } a_bindvb;
static void rec_vkCmdBindVertexBuffers(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer *b, const VkDeviceSize *o) {
    if (!rec_on()) { vkCmdBindVertexBuffers(cb, first, n, b, o); return; }
    uint8_t *r = rq_take(R_BINDVB, cb, sizeof(rq_hdr) + sizeof(a_bindvb) + n * (sizeof *b + sizeof *o)), *d = r + sizeof(rq_hdr) + sizeof(a_bindvb);
    *(a_bindvb *)(void *)(r + sizeof(rq_hdr)) = (a_bindvb){first, n}; RQ_ARR(d, b, n); RQ_ARR(d, o, n); rq_push(r);
}
typedef struct { VkImage s; VkImageLayout sl; VkImage d; VkImageLayout dl; uint32_t n; VkFilter f; } a_blit;
static void rec_vkCmdBlitImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage dimg, VkImageLayout dl, uint32_t n, const VkImageBlit *rg, VkFilter f) {
    if (!rec_on()) { vkCmdBlitImage(cb, s, sl, dimg, dl, n, rg, f); return; }
    uint8_t *r = rq_take(R_BLIT, cb, sizeof(rq_hdr) + sizeof(a_blit) + n * sizeof *rg), *d = r + sizeof(rq_hdr) + sizeof(a_blit);
    *(a_blit *)(void *)(r + sizeof(rq_hdr)) = (a_blit){s, sl, dimg, dl, n, f}; RQ_ARR(d, rg, n); rq_push(r);
}
typedef struct { uint32_t na, nr; } a_clratt;
static void rec_vkCmdClearAttachments(VkCommandBuffer cb, uint32_t na, const VkClearAttachment *a, uint32_t nr, const VkClearRect *rc) {
    if (!rec_on()) { vkCmdClearAttachments(cb, na, a, nr, rc); return; }
    uint8_t *r = rq_take(R_CLRATT, cb, sizeof(rq_hdr) + sizeof(a_clratt) + na * sizeof *a + nr * sizeof *rc), *d = r + sizeof(rq_hdr) + sizeof(a_clratt);
    *(a_clratt *)(void *)(r + sizeof(rq_hdr)) = (a_clratt){na, nr}; RQ_ARR(d, a, na); RQ_ARR(d, rc, nr); rq_push(r);
}
typedef struct { VkImage i; VkImageLayout l; VkClearColorValue v; uint32_t n; } a_clrcol;
static void rec_vkCmdClearColorImage(VkCommandBuffer cb, VkImage i, VkImageLayout l, const VkClearColorValue *v, uint32_t n, const VkImageSubresourceRange *rg) {
    if (!rec_on()) { vkCmdClearColorImage(cb, i, l, v, n, rg); return; }
    uint8_t *r = rq_take(R_CLRCOL, cb, sizeof(rq_hdr) + sizeof(a_clrcol) + n * sizeof *rg), *d = r + sizeof(rq_hdr) + sizeof(a_clrcol);
    *(a_clrcol *)(void *)(r + sizeof(rq_hdr)) = (a_clrcol){i, l, *v, n}; RQ_ARR(d, rg, n); rq_push(r);
}
typedef struct { VkImage i; VkImageLayout l; VkClearDepthStencilValue v; uint32_t n; } a_clrds;
static void rec_vkCmdClearDepthStencilImage(VkCommandBuffer cb, VkImage i, VkImageLayout l, const VkClearDepthStencilValue *v, uint32_t n, const VkImageSubresourceRange *rg) {
    if (!rec_on()) { vkCmdClearDepthStencilImage(cb, i, l, v, n, rg); return; }
    uint8_t *r = rq_take(R_CLRDS, cb, sizeof(rq_hdr) + sizeof(a_clrds) + n * sizeof *rg), *d = r + sizeof(rq_hdr) + sizeof(a_clrds);
    *(a_clrds *)(void *)(r + sizeof(rq_hdr)) = (a_clrds){i, l, *v, n}; RQ_ARR(d, rg, n); rq_push(r);
}
typedef struct { VkBuffer s, d; uint32_t n; } a_cpb;
static void rec_vkCmdCopyBuffer(VkCommandBuffer cb, VkBuffer sb, VkBuffer db, uint32_t n, const VkBufferCopy *rg) {
    if (!rec_on()) { vkCmdCopyBuffer(cb, sb, db, n, rg); return; }
    uint8_t *r = rq_take(R_CPB, cb, sizeof(rq_hdr) + sizeof(a_cpb) + n * sizeof *rg), *d = r + sizeof(rq_hdr) + sizeof(a_cpb);
    *(a_cpb *)(void *)(r + sizeof(rq_hdr)) = (a_cpb){sb, db, n}; RQ_ARR(d, rg, n); rq_push(r);
}
typedef struct { VkBuffer b; VkImage i; VkImageLayout l; uint32_t n; } a_cpb2i;
static void rec_vkCmdCopyBufferToImage(VkCommandBuffer cb, VkBuffer b, VkImage i, VkImageLayout l, uint32_t n, const VkBufferImageCopy *rg) {
    if (!rec_on()) { vkCmdCopyBufferToImage(cb, b, i, l, n, rg); return; }
    uint8_t *r = rq_take(R_CPB2I, cb, sizeof(rq_hdr) + sizeof(a_cpb2i) + n * sizeof *rg), *d = r + sizeof(rq_hdr) + sizeof(a_cpb2i);
    *(a_cpb2i *)(void *)(r + sizeof(rq_hdr)) = (a_cpb2i){b, i, l, n}; RQ_ARR(d, rg, n); rq_push(r);
}
typedef struct { VkImage s; VkImageLayout sl; VkImage d; VkImageLayout dl; uint32_t n; } a_cpi;
static void rec_vkCmdCopyImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage dimg, VkImageLayout dl, uint32_t n, const VkImageCopy *rg) {
    if (!rec_on()) { vkCmdCopyImage(cb, s, sl, dimg, dl, n, rg); return; }
    uint8_t *r = rq_take(R_CPI, cb, sizeof(rq_hdr) + sizeof(a_cpi) + n * sizeof *rg), *d = r + sizeof(rq_hdr) + sizeof(a_cpi);
    *(a_cpi *)(void *)(r + sizeof(rq_hdr)) = (a_cpi){s, sl, dimg, dl, n}; RQ_ARR(d, rg, n); rq_push(r);
}
typedef struct { VkImage i; VkImageLayout l; VkBuffer b; uint32_t n; } a_cpi2b;
static void rec_vkCmdCopyImageToBuffer(VkCommandBuffer cb, VkImage i, VkImageLayout l, VkBuffer b, uint32_t n, const VkBufferImageCopy *rg) {
    if (!rec_on()) { vkCmdCopyImageToBuffer(cb, i, l, b, n, rg); return; }
    uint8_t *r = rq_take(R_CPI2B, cb, sizeof(rq_hdr) + sizeof(a_cpi2b) + n * sizeof *rg), *d = r + sizeof(rq_hdr) + sizeof(a_cpi2b);
    *(a_cpi2b *)(void *)(r + sizeof(rq_hdr)) = (a_cpi2b){i, l, b, n}; RQ_ARR(d, rg, n); rq_push(r);
}
typedef struct { uint32_t x, y, z; } a_dispatch;
static void rec_vkCmdDispatch(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) {
    if (!rec_on()) { vkCmdDispatch(cb, x, y, z); return; }
    uint8_t *r = rq_take(R_DISPATCH, cb, sizeof(rq_hdr) + sizeof(a_dispatch)); *(a_dispatch *)(void *)(r + sizeof(rq_hdr)) = (a_dispatch){x, y, z}; rq_push(r);
}
typedef struct { VkBuffer b; VkDeviceSize o, n; uint32_t v, pad; } a_fillb;   // a buffer fill on the GPU (gfx_vk_gmem_inplace.inc gm_fill_gpu)
static void rec_vkCmdFillBuffer(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkDeviceSize n, uint32_t v) {
    if (!rec_on()) { vkCmdFillBuffer(cb, b, o, n, v); return; }
    uint8_t *r = rq_take(R_FILLB, cb, sizeof(rq_hdr) + sizeof(a_fillb)); *(a_fillb *)(void *)(r + sizeof(rq_hdr)) = (a_fillb){b, o, n, v, 0}; rq_push(r);
}
// The indirect forms (a task or mesh draw in compute form, gfx_vk_meshemu.inc): the GPU reads their arguments from the buffer when the command runs, so the record
// is the buffer and the place in it. Made on the replay thread they went into the command buffer ahead of the binds queued for them, beside the recorder's own calls
typedef struct { VkBuffer b; VkDeviceSize o; } a_dispind;
static void rec_vkCmdDispatchIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o) {
    if (!rec_on()) { vkCmdDispatchIndirect(cb, b, o); return; }
    uint8_t *r = rq_take(R_DISPIND, cb, sizeof(rq_hdr) + sizeof(a_dispind)); *(a_dispind *)(void *)(r + sizeof(rq_hdr)) = (a_dispind){b, o}; rq_push(r);
}
typedef struct { uint32_t vc, ic, fv, fi; } a_draw;
static void rec_vkCmdDraw(VkCommandBuffer cb, uint32_t vc, uint32_t ic, uint32_t fv, uint32_t fi) {
    if (!rec_on()) { vkCmdDraw(cb, vc, ic, fv, fi); return; }
    uint8_t *r = rq_take(R_DRAW, cb, sizeof(rq_hdr) + sizeof(a_draw)); *(a_draw *)(void *)(r + sizeof(rq_hdr)) = (a_draw){vc, ic, fv, fi}; rq_push(r);
}
// vkCmdDrawMeshTasksEXT (gfx_vk_mesh.inc), which the device gives by name (gfx_vk.c g_draw_mesh): there is no name of the loader's to redirect, so the replay calls this
typedef struct { uint32_t x, y, z; } a_drawmesh;
static void rec_draw_mesh(VkCommandBuffer cb, uint32_t x, uint32_t y, uint32_t z) {
    if (!rec_on()) { g_draw_mesh(cb, x, y, z); return; }
    uint8_t *r = rq_take(R_DRAWMESH, cb, sizeof(rq_hdr) + sizeof(a_drawmesh)); *(a_drawmesh *)(void *)(r + sizeof(rq_hdr)) = (a_drawmesh){x, y, z}; rq_push(r);
}
typedef struct { uint32_t xc, ic, fx; int32_t vo; uint32_t fi; } a_drawidx;
static void rec_vkCmdDrawIndexed(VkCommandBuffer cb, uint32_t xc, uint32_t ic, uint32_t fx, int32_t vo, uint32_t fi) {
    if (!rec_on()) { vkCmdDrawIndexed(cb, xc, ic, fx, vo, fi); return; }
    uint8_t *r = rq_take(R_DRAWIDX, cb, sizeof(rq_hdr) + sizeof(a_drawidx)); *(a_drawidx *)(void *)(r + sizeof(rq_hdr)) = (a_drawidx){xc, ic, fx, vo, fi}; rq_push(r);
}
typedef struct { VkBuffer b; VkDeviceSize o; uint32_t n, stride; } a_drawidxind;
static void rec_vkCmdDrawIndexedIndirect(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t stride) {
    if (!rec_on()) { vkCmdDrawIndexedIndirect(cb, b, o, n, stride); return; }
    uint8_t *r = rq_take(R_DRAWIDXIND, cb, sizeof(rq_hdr) + sizeof(a_drawidxind)); *(a_drawidxind *)(void *)(r + sizeof(rq_hdr)) = (a_drawidxind){b, o, n, stride}; rq_push(r);
}
typedef struct { VkQueryPool p; uint32_t q; } a_endq;
static void rec_vkCmdEndQuery(VkCommandBuffer cb, VkQueryPool p, uint32_t q) {
    if (!rec_on()) { vkCmdEndQuery(cb, p, q); return; }
    uint8_t *r = rq_take(R_ENDQ, cb, sizeof(rq_hdr) + sizeof(a_endq)); *(a_endq *)(void *)(r + sizeof(rq_hdr)) = (a_endq){p, q}; rq_push(r);
}
static void rec_vkCmdEndRenderPass(VkCommandBuffer cb) {
    if (!rec_on()) { vkCmdEndRenderPass(cb); return; }
    uint8_t *r = rq_take(R_ENDRP, cb, sizeof(rq_hdr)); rq_push(r);
}
typedef struct { VkPipelineStageFlags ss, ds; VkDependencyFlags df; uint32_t nm, nb, ni; } a_barrier;
static void rec_vkCmdPipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags ss, VkPipelineStageFlags ds, VkDependencyFlags df, uint32_t nm, const VkMemoryBarrier *m,
                                     uint32_t nb, const VkBufferMemoryBarrier *b, uint32_t ni, const VkImageMemoryBarrier *im) {
    int chained = 0; for (uint32_t k = 0; k < nm; k++) chained |= m[k].pNext != NULL; for (uint32_t k = 0; k < nb; k++) chained |= b[k].pNext != NULL; for (uint32_t k = 0; k < ni; k++) chained |= im[k].pNext != NULL;
    if (!rec_on() || chained) { if (g_rq) { rec_drain(); g_rq_direct++; } vkCmdPipelineBarrier(cb, ss, ds, df, nm, m, nb, b, ni, im); return; }
    uint8_t *r = rq_take(R_BARRIER, cb, sizeof(rq_hdr) + sizeof(a_barrier) + nm * sizeof *m + nb * sizeof *b + ni * sizeof *im), *d = r + sizeof(rq_hdr) + sizeof(a_barrier);
    *(a_barrier *)(void *)(r + sizeof(rq_hdr)) = (a_barrier){ss, ds, df, nm, nb, ni}; RQ_ARR(d, m, nm); RQ_ARR(d, b, nb); RQ_ARR(d, im, ni); rq_push(r);
}
typedef struct { VkPipelineLayout l; VkShaderStageFlags st; uint32_t off, n; } a_push;
static void rec_vkCmdPushConstants(VkCommandBuffer cb, VkPipelineLayout l, VkShaderStageFlags st, uint32_t off, uint32_t n, const void *v) {
    if (!rec_on()) { vkCmdPushConstants(cb, l, st, off, n, v); return; }
    uint8_t *r = rq_take(R_PUSH, cb, sizeof(rq_hdr) + sizeof(a_push) + n), *d = r + sizeof(rq_hdr) + sizeof(a_push);
    *(a_push *)(void *)(r + sizeof(rq_hdr)) = (a_push){l, st, off, n}; RQ_ARR(d, (const uint8_t *)v, n); rq_push(r);
}
typedef struct { VkQueryPool p; uint32_t q, n; } a_resetq;
static void rec_vkCmdResetQueryPool(VkCommandBuffer cb, VkQueryPool p, uint32_t q, uint32_t n) {
    if (!rec_on()) { vkCmdResetQueryPool(cb, p, q, n); return; }
    uint8_t *r = rq_take(R_RESETQ, cb, sizeof(rq_hdr) + sizeof(a_resetq)); *(a_resetq *)(void *)(r + sizeof(rq_hdr)) = (a_resetq){p, q, n}; rq_push(r);
}
typedef struct { VkImage s; VkImageLayout sl; VkImage d; VkImageLayout dl; uint32_t n; } a_resolve;
static void rec_vkCmdResolveImage(VkCommandBuffer cb, VkImage s, VkImageLayout sl, VkImage dimg, VkImageLayout dl, uint32_t n, const VkImageResolve *rg) {
    if (!rec_on()) { vkCmdResolveImage(cb, s, sl, dimg, dl, n, rg); return; }
    uint8_t *r = rq_take(R_RESOLVE, cb, sizeof(rq_hdr) + sizeof(a_resolve) + n * sizeof *rg), *d = r + sizeof(rq_hdr) + sizeof(a_resolve);
    *(a_resolve *)(void *)(r + sizeof(rq_hdr)) = (a_resolve){s, sl, dimg, dl, n}; RQ_ARR(d, rg, n); rq_push(r);
}
typedef struct { float c[4]; } a_blendc;
static void rec_vkCmdSetBlendConstants(VkCommandBuffer cb, const float c[4]) {
    if (!rec_on()) { vkCmdSetBlendConstants(cb, c); return; }
    uint8_t *r = rq_take(R_BLENDC, cb, sizeof(rq_hdr) + sizeof(a_blendc)); memcpy(r + sizeof(rq_hdr), c, sizeof(a_blendc)); rq_push(r);
}
typedef struct { float k, clamp, slope; } a_dbias;
static void rec_vkCmdSetDepthBias(VkCommandBuffer cb, float k, float clamp, float slope) {
    if (!rec_on()) { vkCmdSetDepthBias(cb, k, clamp, slope); return; }
    uint8_t *r = rq_take(R_DBIAS, cb, sizeof(rq_hdr) + sizeof(a_dbias)); *(a_dbias *)(void *)(r + sizeof(rq_hdr)) = (a_dbias){k, clamp, slope}; rq_push(r);
}
typedef struct { uint32_t first, n; } a_rects;   // scissors and viewports
static void rec_vkCmdSetScissor(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkRect2D *s) {
    if (!rec_on()) { vkCmdSetScissor(cb, first, n, s); return; }
    uint8_t *r = rq_take(R_SCISSOR, cb, sizeof(rq_hdr) + sizeof(a_rects) + n * sizeof *s), *d = r + sizeof(rq_hdr) + sizeof(a_rects);
    *(a_rects *)(void *)(r + sizeof(rq_hdr)) = (a_rects){first, n}; RQ_ARR(d, s, n); rq_push(r);
}
static void rec_vkCmdSetViewport(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkViewport *v) {
    if (!rec_on()) { vkCmdSetViewport(cb, first, n, v); return; }
    uint8_t *r = rq_take(R_VIEWPORT, cb, sizeof(rq_hdr) + sizeof(a_rects) + n * sizeof *v), *d = r + sizeof(rq_hdr) + sizeof(a_rects);
    *(a_rects *)(void *)(r + sizeof(rq_hdr)) = (a_rects){first, n}; RQ_ARR(d, v, n); rq_push(r);
}
typedef struct { VkStencilFaceFlags f; uint32_t v; } a_stencil;   // compare mask, reference, write mask
static void rq_stencil(uint32_t fn, VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t v) {
    uint8_t *r = rq_take(fn, cb, sizeof(rq_hdr) + sizeof(a_stencil)); *(a_stencil *)(void *)(r + sizeof(rq_hdr)) = (a_stencil){f, v}; rq_push(r);
}
static void rec_vkCmdSetStencilCompareMask(VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t v) { if (!rec_on()) vkCmdSetStencilCompareMask(cb, f, v); else rq_stencil(R_SCMASK, cb, f, v); }
static void rec_vkCmdSetStencilReference(VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t v) { if (!rec_on()) vkCmdSetStencilReference(cb, f, v); else rq_stencil(R_SREF, cb, f, v); }
static void rec_vkCmdSetStencilWriteMask(VkCommandBuffer cb, VkStencilFaceFlags f, uint32_t v) { if (!rec_on()) vkCmdSetStencilWriteMask(cb, f, v); else rq_stencil(R_SWMASK, cb, f, v); }
typedef struct { VkPipelineStageFlagBits st; VkQueryPool p; uint32_t q; } a_ts;
static void rec_vkCmdWriteTimestamp(VkCommandBuffer cb, VkPipelineStageFlagBits st, VkQueryPool p, uint32_t q) {
    if (!rec_on()) { vkCmdWriteTimestamp(cb, st, p, q); return; }
    uint8_t *r = rq_take(R_TS, cb, sizeof(rq_hdr) + sizeof(a_ts)); *(a_ts *)(void *)(r + sizeof(rq_hdr)) = (a_ts){st, p, q}; rq_push(r);
}
static VkResult rec_vkBeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo *bi) {
    if (!rec_on() || bi->pNext || bi->pInheritanceInfo) { if (g_rq) { rec_drain(); g_rq_direct++; } return vkBeginCommandBuffer(cb, bi); }
    uint8_t *r = rq_take(R_BEGINCB, cb, sizeof(rq_hdr) + sizeof *bi); memcpy(r + sizeof(rq_hdr), bi, sizeof *bi); rq_push(r);
    return VK_SUCCESS;   // the replay never looks at it (cb())
}
// chunk_submit with the recorder on: the command buffer (VK_NULL_HANDLE: a chunk never begun, the fence alone) ended and submitted with its fence by the recorder, after
// every call queued before, so the queue gets the chunks in the order the replay closed them. The replay does not wait: the completion thread waits for the fence, which
// signals only once the recorder has submitted it. The front end blits a presented frame on the same queue, so mtl_present drains first (its chunks submitted before it)
// → the submission's ticket: the completion thread waits for it (rec_wait_submitted) before it waits for the fence, since a fence not yet submitted is not one to wait on
typedef struct { VkFence f; VkResult *res; } a_submit;   // res: the chunk's sub_r, written before g_rq_subs_done counts the submission
static uint64_t g_rq_subs_queued; static _Atomic uint64_t g_rq_subs_done; static _Atomic int g_rq_swait; static pthread_cond_t g_rq_csub = PTHREAD_COND_INITIALIZER;
static uint64_t rec_submit(VkCommandBuffer cb, VkFence f, VkResult *res) {
    uint8_t *r = rq_take(R_SUBMIT, cb, sizeof(rq_hdr) + sizeof(a_submit)); *(a_submit *)(void *)(r + sizeof(rq_hdr)) = (a_submit){f, res}; rq_push(r);
    return ++g_rq_subs_queued;
}
static void rec_wait_submitted(uint64_t ticket) {   // the completion thread: until the recorder has submitted that one
    if (!ticket || atomic_load(&g_rq_subs_done) >= ticket) return;
    pthread_mutex_lock(&g_rq_mu); atomic_store(&g_rq_swait, 1);
    while (atomic_load(&g_rq_subs_done) < ticket) pthread_cond_wait(&g_rq_csub, &g_rq_mu);
    atomic_store(&g_rq_swait, 0); pthread_mutex_unlock(&g_rq_mu);
}
static VkResult rec_vkEndCommandBuffer(VkCommandBuffer cb) {   // the command buffer whole when this returns: chunk_submit hands it to the queue next
    if (!rec_on()) return vkEndCommandBuffer(cb);
    uint8_t *r = rq_take(R_ENDCB, cb, sizeof(rq_hdr)); rq_push(r); rec_drain();
    return g_rq_end;
}

// A descriptor set's writes are made by the recorder as well: vkUpdateDescriptorSets was the replay thread's largest call into the driver once its copies were gone (Mario
// Kart World's race: 10.4 of the replay's 40 ms a frame on a OnePlus Pad 3 with raw global memory; 5.3 ms on a Galaxy S25 Edge with the recorder on, about 5,000 sets, 0.6
// packed and queued). The writes go into the ring packed (rec_desc.h), ahead of
// the set's bind, so the recorder writes a set before it binds it, as the replay did; every other write of the replay (transform feedback's bindings, the texture pool's
// set, the copies' and decodes' sets) takes the same road, so two writes of one set keep their order and never meet on two threads.
//   - A set's pool is reset or the set handed out again only once its chunk completed (ds_chunk_done), after the recorder submitted it, so after every write queued for it.
//   - The views, samplers and buffers a write names stay until their chunk completes, as for every queued call.
//   - The replay may take sets from a pool (vkAllocateDescriptorSets) while the recorder writes others of it: Vulkan asks for one thread at a time per set written and per
//     pool allocated from, not across the two.
//   - Writes this cannot pack (a pNext chain, copies between sets, more than a block holds) are made directly after a drain.
// DM_VK_REC_DESC=0 (env.txt): the writes stay on the replay thread, as before
static int rec_desc(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_REC_DESC"); v = !(e && *e == '0'); if (!v) LOG("[vk] DM_VK_REC_DESC=0: descriptor sets are written on the replay thread"); } return v; }
static uint64_t g_rq_desc, g_rq_desc_direct;   // descriptor set writes queued / made directly after a drain
typedef struct { uint32_t n, pad; } a_updds;
static void rec_vkUpdateDescriptorSets(VkDevice dev, uint32_t nw, const VkWriteDescriptorSet *w, uint32_t nc, const VkCopyDescriptorSet *c) {
    if (!nw && !nc) return;
    if (!rec_on() || !rec_desc()) { vkUpdateDescriptorSets(dev, nw, w, nc, c); return; }
    size_t b = nc ? 0 : rd_size(nw, w);
    if (!b) { rec_drain(); g_rq_desc_direct++; vkUpdateDescriptorSets(dev, nw, w, nc, c); return; }
    uint8_t *r = rq_take(R_UPDDS, VK_NULL_HANDLE, sizeof(rq_hdr) + sizeof(a_updds) + b);
    *(a_updds *)(void *)(r + sizeof(rq_hdr)) = (a_updds){nw, 0}; rd_pack(r + sizeof(rq_hdr) + sizeof(a_updds), nw, w); rq_push(r); g_rq_desc++;
}

// ---- The recorder thread ----
static void rq_make(const uint8_t *rec) {   // one record's call
    const rq_hdr *h = (const rq_hdr *)(const void *)rec; const uint8_t *s = rec + sizeof(rq_hdr); VkCommandBuffer cb = h->cb;
    switch (h->fn) {
        case R_BEGINQ: { const a_beginq *a = (const void *)s; vkCmdBeginQuery(cb, a->p, a->q, a->f); break; }
        case R_BEGINRP: { const a_beginrp *a = (const void *)s; VkRenderPassBeginInfo bi = a->bi; bi.pClearValues = bi.clearValueCount ? (const VkClearValue *)(const void *)(s + sizeof *a) : NULL; vkCmdBeginRenderPass(cb, &bi, a->c); break; }
        case R_BINDDS: { const a_bindds *a = (const void *)s; const uint8_t *p = s + sizeof *a; const VkDescriptorSet *ds = RQ_GET(p, VkDescriptorSet, a->n); const uint32_t *dy = RQ_GET(p, uint32_t, a->ndyn);
                         vkCmdBindDescriptorSets(cb, a->bp, a->l, a->first, a->n, ds, a->ndyn, a->ndyn ? dy : NULL); break; }
        case R_BINDIB: { const a_bindib *a = (const void *)s; vkCmdBindIndexBuffer(cb, a->b, a->o, a->t); break; }
        case R_BINDP: { const a_bindp *a = (const void *)s; vkCmdBindPipeline(cb, a->bp, a->p); break; }
        case R_BINDVB: { const a_bindvb *a = (const void *)s; const uint8_t *p = s + sizeof *a; const VkBuffer *b = RQ_GET(p, VkBuffer, a->n); const VkDeviceSize *o = RQ_GET(p, VkDeviceSize, a->n);
                         vkCmdBindVertexBuffers(cb, a->first, a->n, b, o); break; }
        case R_BLIT: { const a_blit *a = (const void *)s; vkCmdBlitImage(cb, a->s, a->sl, a->d, a->dl, a->n, (const VkImageBlit *)(const void *)(s + sizeof *a), a->f); break; }
        case R_CLRATT: { const a_clratt *a = (const void *)s; const uint8_t *p = s + sizeof *a; const VkClearAttachment *at = RQ_GET(p, VkClearAttachment, a->na); const VkClearRect *rc = RQ_GET(p, VkClearRect, a->nr);
                         vkCmdClearAttachments(cb, a->na, at, a->nr, rc); break; }
        case R_CLRCOL: { const a_clrcol *a = (const void *)s; vkCmdClearColorImage(cb, a->i, a->l, &a->v, a->n, (const VkImageSubresourceRange *)(const void *)(s + sizeof *a)); break; }
        case R_CLRDS: { const a_clrds *a = (const void *)s; vkCmdClearDepthStencilImage(cb, a->i, a->l, &a->v, a->n, (const VkImageSubresourceRange *)(const void *)(s + sizeof *a)); break; }
        case R_CPB: { const a_cpb *a = (const void *)s; vkCmdCopyBuffer(cb, a->s, a->d, a->n, (const VkBufferCopy *)(const void *)(s + sizeof *a)); break; }
        case R_CPB2I: { const a_cpb2i *a = (const void *)s; vkCmdCopyBufferToImage(cb, a->b, a->i, a->l, a->n, (const VkBufferImageCopy *)(const void *)(s + sizeof *a)); break; }
        case R_CPI: { const a_cpi *a = (const void *)s; vkCmdCopyImage(cb, a->s, a->sl, a->d, a->dl, a->n, (const VkImageCopy *)(const void *)(s + sizeof *a)); break; }
        case R_CPI2B: { const a_cpi2b *a = (const void *)s; vkCmdCopyImageToBuffer(cb, a->i, a->l, a->b, a->n, (const VkBufferImageCopy *)(const void *)(s + sizeof *a)); break; }
        case R_DISPATCH: { const a_dispatch *a = (const void *)s; vkCmdDispatch(cb, a->x, a->y, a->z); break; }
        case R_DISPIND: { const a_dispind *a = (const void *)s; vkCmdDispatchIndirect(cb, a->b, a->o); break; }
        case R_DRAW: { const a_draw *a = (const void *)s; vkCmdDraw(cb, a->vc, a->ic, a->fv, a->fi); break; }
        case R_DRAWIDX: { const a_drawidx *a = (const void *)s; vkCmdDrawIndexed(cb, a->xc, a->ic, a->fx, a->vo, a->fi); break; }
        case R_DRAWIDXIND: { const a_drawidxind *a = (const void *)s; vkCmdDrawIndexedIndirect(cb, a->b, a->o, a->n, a->stride); break; }
        case R_FILLB: { const a_fillb *a = (const void *)s; vkCmdFillBuffer(cb, a->b, a->o, a->n, a->v); break; }
        case R_DRAWMESH: { const a_drawmesh *a = (const void *)s; g_draw_mesh(cb, a->x, a->y, a->z); break; }
        case R_ENDQ: { const a_endq *a = (const void *)s; vkCmdEndQuery(cb, a->p, a->q); break; }
        case R_ENDRP: vkCmdEndRenderPass(cb); break;
        case R_BARRIER: { const a_barrier *a = (const void *)s; const uint8_t *p = s + sizeof *a; const VkMemoryBarrier *m = RQ_GET(p, VkMemoryBarrier, a->nm);
                          const VkBufferMemoryBarrier *b = RQ_GET(p, VkBufferMemoryBarrier, a->nb); const VkImageMemoryBarrier *im = RQ_GET(p, VkImageMemoryBarrier, a->ni);
                          vkCmdPipelineBarrier(cb, a->ss, a->ds, a->df, a->nm, a->nm ? m : NULL, a->nb, a->nb ? b : NULL, a->ni, a->ni ? im : NULL); break; }
        case R_PUSH: { const a_push *a = (const void *)s; vkCmdPushConstants(cb, a->l, a->st, a->off, a->n, s + sizeof *a); break; }
        case R_RESETQ: { const a_resetq *a = (const void *)s; vkCmdResetQueryPool(cb, a->p, a->q, a->n); break; }
        case R_RESOLVE: { const a_resolve *a = (const void *)s; vkCmdResolveImage(cb, a->s, a->sl, a->d, a->dl, a->n, (const VkImageResolve *)(const void *)(s + sizeof *a)); break; }
        case R_BLENDC: { const a_blendc *a = (const void *)s; vkCmdSetBlendConstants(cb, a->c); break; }
        case R_DBIAS: { const a_dbias *a = (const void *)s; vkCmdSetDepthBias(cb, a->k, a->clamp, a->slope); break; }
        case R_SCISSOR: { const a_rects *a = (const void *)s; vkCmdSetScissor(cb, a->first, a->n, (const VkRect2D *)(const void *)(s + sizeof *a)); break; }
        case R_VIEWPORT: { const a_rects *a = (const void *)s; vkCmdSetViewport(cb, a->first, a->n, (const VkViewport *)(const void *)(s + sizeof *a)); break; }
        case R_SCMASK: { const a_stencil *a = (const void *)s; vkCmdSetStencilCompareMask(cb, a->f, a->v); break; }
        case R_SREF: { const a_stencil *a = (const void *)s; vkCmdSetStencilReference(cb, a->f, a->v); break; }
        case R_SWMASK: { const a_stencil *a = (const void *)s; vkCmdSetStencilWriteMask(cb, a->f, a->v); break; }
        case R_TS: { const a_ts *a = (const void *)s; vkCmdWriteTimestamp(cb, a->st, a->p, a->q); break; }
        case R_UPDDS: { const a_updds *a = (const void *)s; VkWriteDescriptorSet w[RD_MAX]; rd_unpack(s + sizeof *a, a->n, w); vkUpdateDescriptorSets(g_vk.dev, a->n, w, 0, NULL); break; }
        case R_BEGINCB: vkBeginCommandBuffer(cb, (const VkCommandBufferBeginInfo *)(const void *)s); break;
        case R_ENDCB: g_rq_end = vkEndCommandBuffer(cb); break;
        case R_SUBMIT: { const a_submit *a = (const void *)s;
                         // a command buffer that did not end is not submitted: its fence goes alone, so the chunk still completes (its work dropped) and no wait hangs on it
                         if (cb) { VkResult e = vkEndCommandBuffer(cb); if (e != VK_SUCCESS) { RED_ONCE(150, "vkEndCommandBuffer failed on the recorder: %d (the chunk's work dropped)", e); if (e == VK_ERROR_DEVICE_LOST) gpu_fault_log(e); cb = VK_NULL_HANDLE; } }
                         *a->res = submit_or_fence(cb, a->f, " on the recorder");   // before the count below: the completion thread reads it once the submission is counted
                         atomic_fetch_add(&g_rq_subs_done, 1);
                         if (atomic_load(&g_rq_swait)) { pthread_mutex_lock(&g_rq_mu); pthread_cond_broadcast(&g_rq_csub); pthread_mutex_unlock(&g_rq_mu); }
                         break; }
        default: break;   // R_WRAP: nothing to make
    }
}
static void *rq_thread(void *arg) {
    (void)arg; pthread_setname_np(pthread_self(), "vk-record");
    for (;;) {
        uint64_t r = atomic_load(&g_rq_r);
        if (r == atomic_load(&g_rq_w)) {   // nothing queued: wait for a record (the replay wakes it after each one, when it sleeps)
            int spun = 0; while (spun++ < 16 && r == atomic_load(&g_rq_w)) sched_yield();   // briefly: the replay wakes it for a batch or a submission
            if (r == atomic_load(&g_rq_w)) { pthread_mutex_lock(&g_rq_mu); atomic_store(&g_rq_csleep, 1);
                while (r == atomic_load(&g_rq_w)) pthread_cond_wait(&g_rq_cdata, &g_rq_mu);
                atomic_store(&g_rq_csleep, 0); pthread_mutex_unlock(&g_rq_mu); }
            continue; }
        const uint8_t *rec = g_rq + (size_t)(r % RQ_SIZE);
        hint_note(); rq_make(rec);
        atomic_store(&g_rq_r, r + ((const rq_hdr *)(const void *)rec)->n);
        if (atomic_load(&g_rq_pwait)) { pthread_mutex_lock(&g_rq_mu); pthread_cond_signal(&g_rq_cspace); pthread_mutex_unlock(&g_rq_mu); }
    }
    return NULL;
}

// ---- The replay below calls the wrappers ----
#define vkCmdBeginQuery rec_vkCmdBeginQuery
#define vkCmdBeginRenderPass rec_vkCmdBeginRenderPass
#define vkCmdBindDescriptorSets rec_vkCmdBindDescriptorSets
#define vkCmdBindIndexBuffer rec_vkCmdBindIndexBuffer
#define vkCmdBindPipeline rec_vkCmdBindPipeline
#define vkCmdBindVertexBuffers rec_vkCmdBindVertexBuffers
#define vkCmdBlitImage rec_vkCmdBlitImage
#define vkCmdClearAttachments rec_vkCmdClearAttachments
#define vkCmdClearColorImage rec_vkCmdClearColorImage
#define vkCmdClearDepthStencilImage rec_vkCmdClearDepthStencilImage
#define vkCmdCopyBuffer rec_vkCmdCopyBuffer
#define vkCmdCopyBufferToImage rec_vkCmdCopyBufferToImage
#define vkCmdCopyImage rec_vkCmdCopyImage
#define vkCmdCopyImageToBuffer rec_vkCmdCopyImageToBuffer
#define vkCmdDispatch rec_vkCmdDispatch
#define vkCmdDispatchIndirect rec_vkCmdDispatchIndirect
#define vkCmdDraw rec_vkCmdDraw
#define vkCmdDrawIndexed rec_vkCmdDrawIndexed
#define vkCmdDrawIndexedIndirect rec_vkCmdDrawIndexedIndirect
#define vkCmdFillBuffer rec_vkCmdFillBuffer
#define vkCmdEndQuery rec_vkCmdEndQuery
#define vkCmdEndRenderPass rec_vkCmdEndRenderPass
#define vkCmdPipelineBarrier rec_vkCmdPipelineBarrier
#define vkCmdPushConstants rec_vkCmdPushConstants
#define vkCmdResetQueryPool rec_vkCmdResetQueryPool
#define vkCmdResolveImage rec_vkCmdResolveImage
#define vkCmdSetBlendConstants rec_vkCmdSetBlendConstants
#define vkCmdSetDepthBias rec_vkCmdSetDepthBias
#define vkCmdSetScissor rec_vkCmdSetScissor
#define vkCmdSetStencilCompareMask rec_vkCmdSetStencilCompareMask
#define vkCmdSetStencilReference rec_vkCmdSetStencilReference
#define vkCmdSetStencilWriteMask rec_vkCmdSetStencilWriteMask
#define vkCmdSetViewport rec_vkCmdSetViewport
#define vkCmdWriteTimestamp rec_vkCmdWriteTimestamp
#define vkUpdateDescriptorSets rec_vkUpdateDescriptorSets
#define vkBeginCommandBuffer rec_vkBeginCommandBuffer
#define vkEndCommandBuffer rec_vkEndCommandBuffer
