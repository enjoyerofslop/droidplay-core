// GPU breadcrumbs around every dispatch (DM_VK_BC=1, a diagnostic; off by default, and nothing is recorded or written when off).
// Why: a chunk the GPU hangs in is named by gpu_slow (its crumbs), but a chunk holds tens of passes, draws and dispatches, and the device lost after it says nothing more.
// Mario Kart World on a Galaxy S25 Edge (Adreno 830) lost the GPU about 10 s in, each slow chunk holding the same twenty-odd items; skipping compute shaders by key to find
// the one took a dozen runs, and VK_EXT_device_fault's addresses (1.0f's bit pattern plus an offset, null plus an offset) named no buffer. Here the GPU itself says how far
// it got: before a dispatch it writes the dispatch's number into a host-visible ring (vkCmdFillBuffer, then a barrier so the dispatch starts after it and the write reaches the host), after it (a barrier
// so the write waits for the dispatch to finish) the same number into the next word. A dispatch whose first word holds its number and whose second does not began and
// never ended. The host keeps each number's shader, groups and bindings (stage_set's resources, as resolved: uniform buffers with their first words, storage buffers with
// their address, size and first words, textures and images with their serial, size and format), so the report names what the dispatch read.
// The ring is read where the replay already reports a hang: gpu_slow (a chunk over GPU_SLOW_NS: its dispatches up to the first one never begun) and gpu_fault_log (the
// device lost: the last eight begun and never ended). The barriers serialize the dispatches with the transfers around them, so frame times with it on are not the game's.
// The recorder thread (DM_VK_RECORDER=1) makes the vkCmd* calls later, on its own thread, out of step with the host records kept here: with it on, breadcrumbs stay off
#include <stdarg.h>
enum { BC_N = 4096, BC_HOST = 2048, BC_DESC = 2400 };   // GPU ring slots (two words each), host records kept, bytes of a record's bindings text
typedef struct { uint32_t id; uint64_t seq; const char *key; uint32_t g[3]; char desc[BC_DESC]; } bc_ent;
static bc_ent *g_bc_host; static stg_t g_bc; static uint32_t g_bc_id; static char g_bc_desc[BC_DESC]; static int g_bc_dn;
static int bc_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("DM_VK_BC"); v = e && *e == '1';
        if (v && rec_on()) { v = 0; LOG("[RED] [vk] DM_VK_BC=1 with DM_VK_RECORDER=1: no breadcrumbs (the recorder makes the calls later, on its own thread)"); }
        else if (v && !(g_bc_host = calloc(BC_HOST, sizeof *g_bc_host))) v = 0;
        if (v) LOG("[vk] DM_VK_BC=1: GPU breadcrumbs around every dispatch (frame times are not the game's: each dispatch waits for the one before)"); }
    return v;
}
static void bc_addf(const char *fmt, ...) {   // appends to the bindings text of the dispatch being resolved (stage_set)
    if (g_bc_dn >= BC_DESC - 1) return;
    va_list ap; va_start(ap, fmt); int k = vsnprintf(g_bc_desc + g_bc_dn, (size_t)(BC_DESC - g_bc_dn), fmt, ap); va_end(ap);
    if (k > 0) g_bc_dn = g_bc_dn + k < BC_DESC - 1 ? g_bc_dn + k : BC_DESC - 1;
}
static void bc_words(const char *tag, const void *p, size_t n, size_t max) {   // a buffer's first words, in hex
    const uint32_t *w = p; size_t k = n / 4 < max ? n / 4 : max; bc_addf(" %s[", tag);
    for (size_t i = 0; i < k; i++) bc_addf("%x%s", w[i], i + 1 < k ? " " : "");
    bc_addf("]");
}
static uint32_t bc_begin(VkCommandBuffer c, const char *key, const uint32_t g[3], uint64_t seq) {   // → the dispatch's number, 0 without breadcrumbs
    if (!g_bc.p) { if (!stg_new(&g_bc, BC_N * 8)) return 0; memset(g_bc.p, 0, BC_N * 8); }
    uint32_t id = ++g_bc_id; bc_ent *h = &g_bc_host[id % BC_HOST];
    h->id = id; h->seq = seq; h->key = key; memcpy(h->g, g, sizeof h->g); memcpy(h->desc, g_bc_desc, BC_DESC); h->desc[BC_DESC - 1] = 0;
    vkCmdFillBuffer(c, g_bc.b, (VkDeviceSize)(id % BC_N) * 8, 4, id);
    // the dispatch starts after the write, and the write is made available to the host: a word left in a GPU cache when the hang hits would read as never begun
    VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_SHADER_READ_BIT};
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    return id;
}
static void bc_end(VkCommandBuffer c, uint32_t id) {
    if (!id) return;
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 0, NULL);
    vkCmdFillBuffer(c, g_bc.b, (VkDeviceSize)(id % BC_N) * 8 + 4, 4, id);
    VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT};   // available to the host, as the begin word
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}
static void bc_show(uint32_t id, const char *what) {
    const bc_ent *h = &g_bc_host[id % BC_HOST];
    LOG("[vk]   dispatch #%u (submission %llu) %s %ux%ux%u groups: %s", id, (unsigned long long)h->seq, h->key ? h->key : "?", h->g[0], h->g[1], h->g[2], what);
    LOG("[vk]     bindings:%s", h->desc);
}
// seq: that chunk's dispatches that did not end, up to its first one never begun (the GPU stopped before the rest); 0 (the device lost): the last eight begun and never ended
static void bc_report(const char *why, uint64_t seq) {
    if (!bc_on() || !g_bc.p) return;
    const volatile uint32_t *m = (const volatile uint32_t *)g_bc.p;
    uint32_t top = g_bc_id, lo = top > BC_HOST ? top - BC_HOST + 1 : 1, open[8]; int no = 0, ended = 0;
    for (uint32_t id = lo; id && id <= top; id++) {
        const bc_ent *h = &g_bc_host[id % BC_HOST]; if (h->id != id || (seq && h->seq != seq)) continue;
        uint32_t b = m[2 * (id % BC_N)], e = m[2 * (id % BC_N) + 1];
        if (b == id && e == id) { ended++; continue; }
        if (seq) { bc_show(id, b == id ? "BEGUN, NEVER ENDED" : "never begun (the GPU stopped before it)"); if (b != id) break; continue; }
        if (b == id) open[no++ % 8] = id;
    }
    if (seq) { LOG("[vk] breadcrumbs (%s): %d of its dispatches ended", why, ended); return; }
    LOG("[vk] breadcrumbs (%s): %u dispatches marked, %d begun and never ended among the last %u", why, top, no, top - lo + 1);
    for (int k = no > 8 ? no - 8 : 0; k < no; k++) bc_show(open[k % 8], "BEGUN, NEVER ENDED");
}
