// ---------------- NVN2's texture and sampler pools read by ID (spv_pool.h) ----------------
// A shader that reads a texture through a handle the translator could not trace to one slot indexes the whole texture pool and sampler pool by ID (Ryujinx's handle
// fallback, <stage>_t_cb0_40000000); Ocarina of Time's models, terrain and foliage read every texture so. The Mac keeps a pool per element type (nvn_mtl.m pool_gpu).
// Here one descriptor set, set POOL_SET, serves every such shader: binding 0 the texture pool's IDs (every image type of the shaders aliases it, an element holding the view the
// reads asked for), 1 the sampler pool, 2 the same samplers with comparison on (depth reads), 3 the reads' marks (a word per ID, a bit per view type). spv_pool.h moves a module's
// pool arrays there when it is made.
// - Which textures: only those a shader read, as on the Mac. Each chunk's first pool draw (pool_sync) takes the IDs marked since and clears their marks, so a read in a
//   later chunk marks an ID again: only the IDs in use are looked at, as stage_textures looks at a draw's own (vt_get, view_get: a texture the CPU rewrote is uploaded
//   again, a registration changed gives its new view). A registration whose storage is in no memory pool the host maps (the game finalized it) is not made: vt_create would
//   read the freed pool object. A read of an ID not made yet gives 0 (a null descriptor, VK_EXT_robustness2, required) until the next chunk.
// - The set is never written while a chunk that bound it may still run: a change goes to a version no chunk in flight holds (POOL_VERS of them at most), brought up to
//   date with the elements changed since it was last written (pool_bind). When every version is in flight, the newest serves as it is if it only lacks views made since
//   (those reads give 0 for one more chunk); one that names a view since replaced or dropped (g_prm: the replaced view may be destroyed later) is waited for.
// - Started by the first shader that has a pool array (sh_get), not by every game's first shader.
// DM_VK_POOL=0 (env.txt): off, and the draws of such shaders are dropped as before (gfx_vk_draw.inc ds_fits)
enum { POOL_SET = 4, POOL_B_IMG = 0, POOL_B_SMP = 1, POOL_B_CMP = 2, POOL_B_SEEN = 3, POOL_NSMP = 4096, POOL_VERS = 3 };
static _Atomic int g_pool_state;   // 0 not decided yet, 1 on, -1 off (pool_on)
static pthread_mutex_t g_tpool_mu = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_pool_n;   // the image binding's elements: the game's texture pools' IDs when the first pool shader was made
static VkDescriptorSetLayout g_pool_dsl; static VkDescriptorPool g_pool_dp;
static VkBuffer g_seen_b; static VkDeviceMemory g_seen_m; static volatile uint32_t *g_seen;   // the marks: host-visible, read without waiting (a mark only ever sets bits)
typedef struct { VkDescriptorSet set; uint64_t pos, used; } pool_ver;   // pos: the changes it holds (0: none written yet); used: the last chunk that bound it
static pool_ver g_pv[POOL_VERS]; static int g_npv;
static VkImageView *g_pimg; static uint64_t *g_pimg_chg; static uint32_t *g_pwant, *g_plist, g_nplist, *g_pread;   // each ID's view and when it changed; the view types read; the IDs read; those read since the last sync
static VkSampler g_psmp[2][POOL_NSMP]; static uint64_t g_psmp_chg[POOL_NSMP];
static uint64_t g_ppos = 1, g_prm;   // changes so far (from 1: a version's pos 0 is one never written); the last that replaced or dropped a view (a version short of it may name one that is gone)
static uint64_t g_psync_seq, g_pbind_seq, g_psreg_seen; static VkDescriptorSet g_pbind_set; static int g_ptex_base = -1, g_psamp_base = -1;   // the chunk last synced / bound, the set it binds, the pools synced
static int g_psmp_ok;   // every sampler element filled (the first sync)
static uint64_t g_pool_draws;   // draws and dispatches that bound the pool set
static uint64_t g_pool_gone;   // reads of a registration whose storage is in no mapped pool
static uint64_t g_pool_syncs, g_pool_ns, g_pool_views, g_pool_newver, g_pool_stale, g_pool_waits, g_pool_shaders, g_pool_refused, g_pool_early, g_pool_loopmark;
static VkDescriptorImageInfo *g_pii; static VkWriteDescriptorSet *g_pwr; static size_t g_pii_cap, g_pwr_cap;   // pool_apply's scratch

static int pool_init(void) {
    const char *e = getenv("DM_VK_POOL");
    if (e && *e == '0') { LOG("[vk] DM_VK_POOL=0: shaders that read the texture pool by ID are dropped"); return 0; }
    const VkPhysicalDeviceLimits *L = &g_vk.props.limits;
    uint32_t n = g_pool_texn > 0 ? ((uint32_t)g_pool_texn + 1023u) & ~1023u : 65536u; if (n > 1u << 20) n = 1u << 20;
    // room for a stage's own set too (sh_get: up to 64 textures, 16 images, texel buffers, storage and uniform buffers) and, per pipeline layout, four stages' sets
    const uint32_t own = 128, ownr = 256;
    const char *why = !g_nonuni ? "no shaderSampledImageArrayNonUniformIndexing" : !g_null_desc ? "no null descriptors (VK_EXT_robustness2), which an ID not made reads as, whatever its type"
                    : L->maxBoundDescriptorSets <= POOL_SET ? "fewer than 5 descriptor sets bound at once" : !g_vk_feat.fragmentStoresAndAtomics ? "no fragmentStoresAndAtomics, which the reads' marks need"
                    : L->maxPerStageDescriptorSampledImages < n + own || L->maxDescriptorSetSampledImages < n + 4 * own ? "too few sampled images"
                    : L->maxPerStageDescriptorSamplers < 2 * POOL_NSMP + 64 || L->maxDescriptorSetSamplers < 2 * POOL_NSMP + 4 * 64 ? "too few samplers"
                    : L->maxPerStageResources < n + 2 * POOL_NSMP + 1 + ownr ? "too few resources per stage" : NULL;
    if (why) { LOG("[RED] [vk] the texture pool by ID is not on this GPU (%s): shaders that read it are dropped", why); return 0; }
    VkDescriptorSetLayoutBinding b[4] = {{POOL_B_IMG, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, n, VK_SHADER_STAGE_ALL, NULL}, {POOL_B_SMP, VK_DESCRIPTOR_TYPE_SAMPLER, POOL_NSMP, VK_SHADER_STAGE_ALL, NULL},
                                         {POOL_B_CMP, VK_DESCRIPTOR_TYPE_SAMPLER, POOL_NSMP, VK_SHADER_STAGE_ALL, NULL}, {POOL_B_SEEN, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL, NULL}};
    VkDescriptorSetLayoutCreateInfo lc = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 4, .pBindings = b};
    VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, n * POOL_VERS}, {VK_DESCRIPTOR_TYPE_SAMPLER, 2 * POOL_NSMP * POOL_VERS}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, POOL_VERS}};
    VkDescriptorPoolCreateInfo pc = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = POOL_VERS, .poolSizeCount = 3, .pPoolSizes = ps};
    VkBufferCreateInfo bc = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)n * 4, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    if (vkCreateDescriptorSetLayout(g_vk.dev, &lc, NULL, &g_pool_dsl) != VK_SUCCESS || vkCreateDescriptorPool(g_vk.dev, &pc, NULL, &g_pool_dp) != VK_SUCCESS || vkCreateBuffer(g_vk.dev, &bc, NULL, &g_seen_b) != VK_SUCCESS) {
        LOG("[RED] [vk] the texture pool's set (%u images) could not be made: shaders that read it are dropped", n); return 0; }
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_vk.dev, g_seen_b, &mr);
    uint32_t mt = ar_mem_type(mr.memoryTypeBits);   // host-visible and coherent, cached where there is such a type: the host reads every word each chunk
    VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mt};
    void *p = NULL;
    if (mt == UINT32_MAX || vkAllocateMemory(g_vk.dev, &ma, NULL, &g_seen_m) != VK_SUCCESS || vkBindBufferMemory(g_vk.dev, g_seen_b, g_seen_m, 0) != VK_SUCCESS || vkMapMemory(g_vk.dev, g_seen_m, 0, bc.size, 0, &p) != VK_SUCCESS) {
        LOG("[RED] [vk] the texture pool's marks buffer could not be made: shaders that read it are dropped"); return 0; }
    memset(p, 0, (size_t)bc.size); g_seen = p;
    g_pimg = calloc(n, sizeof *g_pimg); g_pimg_chg = calloc(n, sizeof *g_pimg_chg); g_pwant = calloc(n, sizeof *g_pwant); g_plist = calloc(n, sizeof *g_plist); g_pread = calloc(n, sizeof *g_pread);
    if (!g_pimg || !g_pimg_chg || !g_pwant || !g_plist || !g_pread) return 0;
    g_pool_n = n;
    LOG("[vk] texture pool by ID on: %u images (the game's texture pools number %d IDs), %d samplers, %d versions of the set at most (DM_VK_POOL=0: off)", n, g_pool_texn, POOL_NSMP, POOL_VERS);
    return 1;
}
static int pool_on(void) {
    int s = atomic_load(&g_pool_state); if (s) return s > 0;
    pthread_mutex_lock(&g_tpool_mu);
    if (!(s = atomic_load(&g_pool_state))) { s = pool_init() ? 1 : -1; atomic_store(&g_pool_state, s); }
    pthread_mutex_unlock(&g_tpool_mu);
    return s > 0;
}
// The module with its pool arrays moved into the pool set (spv_pool.h), in *out (to free); 0 when it has none. t_pool_info gathers what was done
static size_t spv_pool_fit(const uint32_t *code, size_t nw, const char *what, uint32_t **out) {
    *out = NULL;
    if (atomic_load(&g_pool_state) <= 0 || nw < 5) return 0;   // started by sh_get at the first shader with a pool array
    struct spv_pool_cfg cfg = {g_pool_n, POOL_SET, POOL_B_IMG, POOL_B_SMP, POOL_B_CMP, POOL_B_SEEN, 1, g_vk_feat.vertexPipelineStoresAndAtomics};
    uint32_t *s = malloc((size_t)SPV_POOL_SCRATCH * code[3] * 4); if (!s) return 0;
    size_t need = 0, k = 0; int info = 0; uint32_t *o = NULL;
    k = spv_pool(code, nw, &cfg, s, NULL, 0, &need, &info);
    if (need && (o = malloc(need * 4))) k = spv_pool(code, nw, &cfg, s, o, need, &need, &info);
    free(s);
    t_pool_info |= info;
    if (info & SPV_POOL_FAIL) { static int nl; if (nl++ < 8) LOG("[RED] [vk] %s reads the texture pool by ID in a way spv_pool does not rewrite: its draws are dropped", what); }
    if (!k) { free(o); return 0; }
    *out = o; return k;
}
static void pool_note_shader(vshader *s, int info) {   // sh_get, with what spv_pool did to the shader's modules
    if (info & SPV_POOL_FAIL) { g_pool_refused++; return; }
    if (info & (SPV_POOL_IMG | SPV_POOL_SMP)) { s->pool = 1; g_pool_shaders++; g_pool_early += (info & SPV_POOL_EARLY) != 0; g_pool_loopmark += (info & SPV_POOL_LOOPMARK) != 0; }
}
// view_type_of's codes for an NVN texture target: the view type a registration is made as
static uint32_t pool_code_of(uint32_t target) {
    static const uint8_t c[11] = {0, 1, 2, 4, 5, 7, 8, 1, 3, 6, 1};   // 1D, 2D, 3D, 1D array, 2D array, 2D multisample (array), rectangle, cube (array), buffer
    return target < 11 ? c[target] : 1;
}
static int pool_fits(const vtex *v, VkImageViewType vt, const view_t *vv) {   // a view of v of type vt is what view_get makes (else it would make another type, which the read does not match)
    uint32_t nl = vv && vv->set_layers && vv->nlayers ? vv->nlayers : v->layers;
    if (v->type == VK_IMAGE_TYPE_3D) return vt == VK_IMAGE_VIEW_TYPE_3D;
    if (vt == VK_IMAGE_VIEW_TYPE_3D) return 0;
    if (v->type == VK_IMAGE_TYPE_1D) return vt == VK_IMAGE_VIEW_TYPE_1D || vt == VK_IMAGE_VIEW_TYPE_1D_ARRAY;
    if (vt == VK_IMAGE_VIEW_TYPE_1D || vt == VK_IMAGE_VIEW_TYPE_1D_ARRAY) return 0;
    if (vt == VK_IMAGE_VIEW_TYPE_CUBE) return nl >= 6 && v->layers % 6 == 0;
    if (vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) return nl >= 6;
    return 1;
}
// The view of a registration (a copy, taken under g_reg_mu: vt_get may upload, and the game's threads register meanwhile, as stage_textures does) as the reads asked (bits:
// their view types), made now if need be; NULL: none of that type, or its storage is in no memory pool the host maps (the game finalized it: vt_create would read the freed
// pool object; the first byte, as a sparse texture's later pages may be unmapped)
static VkImageView pool_view(const treg_t *rg, uint32_t bits) {
    if (!bits) return VK_NULL_HANDLE;
    if (!rg->t.mem || !mtl_addr((uint64_t)(uintptr_t)rg->t.mem, 1, NULL, NULL)) { g_pool_gone++; return VK_NULL_HANDLE; }
    const view_t *vv = rg->has_v ? &rg->v : NULL;
    uint32_t nat = pool_code_of(vv && vv->set_target ? vv->target : rg->t.d.target), code = (bits >> nat & 1) ? nat : (uint32_t)__builtin_ctz(bits);
    if ((bits & (bits - 1)) && !(bits >> nat & 1)) RED_ONCE(152, "a texture pool ID read as view types %#x, none its own (%u): the first is made", bits, nat);
    vtex *v = vt_get(&rg->t); if (!v) return VK_NULL_HANDLE;
    v->pool_read = 1;   // kept from eviction: a view dropped from the pool set reads 0 until the next sync after a draw reads its ID again (gfx_vk_evict.inc)
    int ms = code >= 7; VkImageViewType vt = ms ? (code == 8 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D) : (VkImageViewType)code;
    if ((v->samples > 1) != ms || !pool_fits(v, vt, vv)) return VK_NULL_HANDLE;
    uint8_t swz[4]; int has_swz = rg->t.set_swz || (vv && vv->set_swizzle);
    if (has_swz) { uint16_t sz = vv && vv->set_swizzle ? (uint16_t)(vv->swz[0] | vv->swz[1] << 3 | vv->swz[2] << 6 | vv->swz[3] << 9) : rg->t.swz; for (int q = 0; q < 4; q++) swz[q] = (sz >> (3 * q)) & 7; }
    g_view_fmt = view_format(v, vv);
    VkImageView iv = view_get(v, vt, vv && vv->set_levels ? vv->base_level : 0, vv && vv->set_levels ? vv->nlevels : 0, vv && vv->set_layers ? vv->min_layer : 0,
                              vv && vv->set_layers ? vv->nlayers : 0, v->aspect == VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT, has_swz ? swz : NULL);
    g_view_fmt = VK_FORMAT_UNDEFINED;
    return iv;
}
// views_drop: a view that goes when the current chunk completes. Elements holding it read 0 from the next version on, and no older version serves as it is (g_prm)
static void pool_drop_view(VkImageView iv) {
    if (!g_nplist || !iv) return;
    for (uint32_t q = 0; q < g_nplist; q++) { uint32_t i = g_plist[q]; if (g_pimg[i] == iv) { g_pimg[i] = VK_NULL_HANDLE; g_pimg_chg[i] = ++g_ppos; g_prm = g_ppos; } }
}
static uint32_t *g_seen_cp;   // pool_sync's copy of the marks
static int pool_snap(void) {   // DM_VK_POOL_SNAP=0 (env.txt): the marks scanned word by word in their buffer, as before
    static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_POOL_SNAP"); v = !(e && *e == '0') && (g_seen_cp = malloc((size_t)g_pool_n * 4)) != NULL; }
    return v;
}
static int pool_log(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_POOL_LOG"); v = e && *e == '1'; } return v; }   // DM_VK_POOL_LOG=1: a line per ID first read
// Under g_bk, before a pool shader's draw or dispatch takes its sets, once a chunk: the IDs read since, made; every ID read so far looked at again; the samplers registered since
static void pool_sync(void) {
    if (!cur() || g_psync_seq == C->seq) return;
    g_psync_seq = C->seq;
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_MONOTONIC);
    int base = D.tex_base, sbase = D.samp_base;
    if (g_pool_texn > (int)g_pool_n) RED_ONCE(155, "the game's texture pools grew to %d IDs after the pool set was made with %u: IDs past it read the last one", g_pool_texn, g_pool_n);
    if (base != g_ptex_base) { if (g_ptex_base >= 0) RED_ONCE(153, "draws reading the texture pool by ID now bind another texture pool (base %d, was %d): the IDs read before are taken from it", base, g_ptex_base); g_ptex_base = base; }
    uint32_t was = g_nplist, nread = 0;
    // The marks taken and cleared: a read in a later chunk marks the ID again. They are read in one copy first (pool_snap): where the GPU has no memory that is both
    // cached and coherent (Arm's Mali-G715), the marks buffer is uncached, and the word-by-word scan of Ocarina of Time's 33,792 IDs took 2.45 ms a sync on a Pixel 9 Pro
    // XL (17 a frame), against 0.33 ms on an Adreno. A word set in the copy is read again before it is cleared, so a bit the GPU set meanwhile is taken with it; one set
    // after the copy stays for the next sync. Without the copy (DM_VK_POOL_SNAP=0) the buffer is looked at two words a load (an aligned 64-bit load is one access):
    // almost every word is 0, and that walk was 2.6% of the replay thread on a Galaxy Z Fold 8
    const uint32_t *snap = pool_snap() ? g_seen_cp : NULL;
    if (snap) memcpy((void *)snap, (const void *)g_seen, (size_t)g_pool_n * 4);
    for (uint32_t i = 1; i < g_pool_n; i++) {
        if (snap) { if (!snap[i]) continue; }
        else if (!(i & 1) && i + 1 < g_pool_n && !((const volatile uint64_t *)(uintptr_t)g_seen)[i >> 1]) { i++; continue; }
        uint32_t m = g_seen[i]; if (!m) continue;
        g_seen[i] = 0;
        uint32_t first = !g_pwant[i]; if (first) g_plist[g_nplist++] = i;
        g_pwant[i] |= m; g_pread[nread++] = i | first << 31;   // bit 31: read for the first time (DM_VK_POOL_LOG)
    }
    for (uint32_t q = 0; q < nread; q++) {
        uint32_t i = g_pread[q] & 0x7fffffffu; int tid = base + (int)i; treg_t r; int ok, fresh = (int)(g_pread[q] >> 31);
        pthread_mutex_lock(&g_reg_mu); ok = tid > 0 && tid < g_ntreg && g_treg[tid].ok; if (ok) r = g_treg[tid]; pthread_mutex_unlock(&g_reg_mu);
        VkImageView iv = ok ? pool_view(&r, g_pwant[i]) : VK_NULL_HANDLE;
        if (iv != g_pimg[i]) {
            if (iv && !g_pimg[i]) g_pool_views++;
            int had = g_pimg[i] != VK_NULL_HANDLE; g_pimg[i] = iv; g_pimg_chg[i] = ++g_ppos;
            if (had) g_prm = g_ppos;   // the view replaced may be destroyed later, when no element holds it any more (pool_drop_view sees only the current ones)
        }
        if (fresh && pool_log())
            LOG("[vk] texture pool ID %u (host %d) read as types %#x: %s %ux%ux%u target %u format %#x levels %u -> view %p", i, tid, g_pwant[i], ok ? "texture" : "nothing registered",
                ok ? r.t.d.width : 0, ok ? r.t.d.height : 0, ok ? r.t.d.depth : 0, ok ? r.t.d.target : 0, ok ? r.t.d.format : 0, ok ? r.t.d.levels : 0, (void *)iv);
    }
    pthread_mutex_lock(&g_reg_mu);
    if (sbase != g_psamp_base || g_sreg_gen != g_psreg_seen) {   // samplers live as long as the replay (samp_get), so a change only ever adds one
        for (int j = 0; j < POOL_NSMP; j++) {
            int sid = j ? sbase + j : 0;   // ID 0 names no sampler, as in stage_set
            if (sbase == g_psamp_base && g_psmp[0][j] && (sid <= 0 || sid >= g_nsreg || g_sreg_ver[sid] <= g_psreg_seen)) continue;
            const samp_par *sp = sid > 0 && sid < g_nsreg && g_sreg_ok[sid] ? &g_sreg[sid] : NULL;
            VkSampler s0 = samp_get(sp, 0), s1 = samp_get(sp, 1);
            if (!s0 || !s1) { s0 = samp_get(NULL, 0); s1 = samp_get(NULL, 1); RED_ONCE(154, "a sampler of the pool read by ID could not be made: the default one stands in"); }
            if (s0 != g_psmp[0][j] || s1 != g_psmp[1][j]) { g_psmp[0][j] = s0; g_psmp[1][j] = s1; g_psmp_chg[j] = ++g_ppos; }
        }
        g_psamp_base = sbase; g_psreg_seen = g_sreg_gen;
        g_psmp_ok = 1; for (int j = 0; j < POOL_NSMP && g_psmp_ok; j++) g_psmp_ok = g_psmp[0][j] && g_psmp[1][j];
    }
    pthread_mutex_unlock(&g_reg_mu);
    uint64_t dt = clock_gettime_nsec_np(CLOCK_MONOTONIC) - t0; g_pool_syncs++; g_pool_ns += dt;
    static int nlog; static uint32_t next = 100;   // the first reads as they come, then every 100 IDs more: the periodic line comes every 300 frames, minutes apart in a slow scene
    if (g_nplist > was && (nlog < 8 || g_nplist >= next)) { nlog++; if (g_nplist >= next) next = g_nplist + 100;
        LOG("[vk] texture pool by ID: %u IDs read (%u new), %llu views made; this sync %.2f ms, %llu syncs (%.3f ms each), %llu draws bound the set", g_nplist, g_nplist - was, (unsigned long long)g_pool_views, dt / 1e6,
            (unsigned long long)g_pool_syncs, g_pool_ns / 1e6 / (double)g_pool_syncs, (unsigned long long)g_pool_draws); }
}
// Version v brought up to the changes so far: the elements changed since it was last written, every element of a new one; contiguous changed elements go in one write
static int pool_apply(pool_ver *v) {
    if (!g_pii) {   // the most one update can take: every element, each its own run
        g_pii_cap = (size_t)g_pool_n + 2 * POOL_NSMP; g_pwr_cap = g_pii_cap + 1;
        g_pii = malloc(g_pii_cap * sizeof *g_pii); g_pwr = malloc(g_pwr_cap * sizeof *g_pwr);
        if (!g_pii || !g_pwr) { free(g_pii); free(g_pwr); g_pii = NULL; g_pwr = NULL; return 0; }
    }
    uint64_t from = v->pos; size_t nii = 0, nwr = 0; VkDescriptorImageInfo *ii = g_pii; VkWriteDescriptorSet *wr = g_pwr;
    static VkDescriptorBufferInfo sb; sb = (VkDescriptorBufferInfo){g_seen_b, 0, VK_WHOLE_SIZE};
    for (uint32_t i = 0; i < g_pool_n; ) {   // images
        if (from && g_pimg_chg[i] <= from) { i++; continue; }
        uint32_t a = i; size_t at = nii;
        for (; i < g_pool_n && (!from || g_pimg_chg[i] > from); i++) ii[nii++] = (VkDescriptorImageInfo){VK_NULL_HANDLE, g_pimg[i], VK_IMAGE_LAYOUT_GENERAL};   // NULL: a null descriptor
        wr[nwr++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, v->set, POOL_B_IMG, a, i - a, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &ii[at], NULL, NULL};
    }
    for (int c = 0; c < 2; c++) for (int j = 0; j < POOL_NSMP; ) {   // samplers, plain and comparing
        if (from && g_psmp_chg[j] <= from) { j++; continue; }
        int a = j; size_t at = nii;
        for (; j < POOL_NSMP && (!from || g_psmp_chg[j] > from); j++) ii[nii++] = (VkDescriptorImageInfo){g_psmp[c][j], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
        wr[nwr++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, v->set, c ? POOL_B_CMP : POOL_B_SMP, (uint32_t)a, (uint32_t)(j - a), VK_DESCRIPTOR_TYPE_SAMPLER, &ii[at], NULL, NULL};
    }
    if (!from) wr[nwr++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, v->set, POOL_B_SEEN, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &sb, NULL};
    if (nwr) vkUpdateDescriptorSets(g_vk.dev, (uint32_t)nwr, wr, 0, NULL);
    v->pos = g_ppos;
    return 1;
}
// Under g_bk, after the draw's or dispatch's own sets are bound: the pool set at POOL_SET, the version this chunk binds (chosen at its first pool draw). 0: none to bind
static int pool_bind(VkCommandBuffer c, VkPipelineBindPoint bp, VkPipelineLayout pl) {
    if (!C) return 0;
    if (g_pbind_seq != C->seq) {
        if (!g_psmp_ok) return 0;   // no sync filled the samplers (a null sampler descriptor is never valid)
        pool_ver *lt = NULL, *v = NULL;
        for (int i = 0; i < g_npv; i++) if (!lt || g_pv[i].pos > lt->pos) lt = &g_pv[i];
        if (lt && lt->pos >= g_ppos) v = lt;   // nothing changed since it was written
        else {
            uint64_t done = __atomic_load_n(&g_seq_done, __ATOMIC_ACQUIRE);
            for (int i = 0; i < g_npv; i++) if (g_pv[i].used <= done && (!v || g_pv[i].pos > v->pos)) v = &g_pv[i];
            if (!v && g_npv < POOL_VERS) {
                VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = g_pool_dp, .descriptorSetCount = 1, .pSetLayouts = &g_pool_dsl};
                pool_ver *nv = &g_pv[g_npv];
                if (vkAllocateDescriptorSets(g_vk.dev, &ai, &nv->set) == VK_SUCCESS) { nv->pos = 0; nv->used = 0; v = nv; g_npv++; g_pool_newver++; }
            }
            if (!v && lt && lt->pos >= g_prm) { v = lt; g_pool_stale++; goto bound; }   // every version in flight: the newest only lacks views made since, which read 0 a chunk longer
            if (!v) {   // the newest names a view since dropped: the oldest version's chunk is waited for
                for (int i = 0; i < g_npv; i++) if (!v || g_pv[i].used < v->used) v = &g_pv[i];
                if (!v) return 0;
                seq_wait(v->used); g_pool_waits++;
            }
            if (!pool_apply(v)) return 0;
        }
    bound:
        v->used = C->seq; g_pbind_seq = C->seq; g_pbind_set = v->set;
    }
    vkCmdBindDescriptorSets(c, bp, pl, POOL_SET, 1, &g_pbind_set, 0, NULL); g_pool_draws++;
    return 1;
}
static void pool_stats(void) {
    if (atomic_load(&g_pool_state) <= 0) return;
    LOG("[vk] texture pool by ID: %llu shaders read it (%llu refused, %llu with early depth tests added, %llu with a mark in a loop header), %u IDs read, %llu views made, %llu reads of registrations whose storage is gone, %llu draws bound the set, %llu syncs (%.3f ms each), %d versions of the set, %llu chunks bound one short of new views, %llu waits for one (cumulative)",
        (unsigned long long)g_pool_shaders, (unsigned long long)g_pool_refused, (unsigned long long)g_pool_early, (unsigned long long)g_pool_loopmark, g_nplist, (unsigned long long)g_pool_views,
        (unsigned long long)g_pool_gone, (unsigned long long)g_pool_draws, (unsigned long long)g_pool_syncs, g_pool_syncs ? g_pool_ns / 1e6 / (double)g_pool_syncs : 0.0, g_npv, (unsigned long long)g_pool_stale, (unsigned long long)g_pool_waits);
}
