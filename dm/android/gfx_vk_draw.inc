// Draws of the Android NVN replay (included by gfx_vk.c, which owns the device, textures, the command buffer and the submission).
// Shaders are translated offline (dmsh_xlate, then tools/spv_android.py): <DM_SPV_DIR>/<key>.spv, <key>.z.spv (vertex shaders, for NVN's -1..1 depth mode) and
// <key>.res.json (what each binding is: the translator's constant / storage buffers and texture handles). Every stage has its descriptor set (vertex 0, fragment 1,
// geometry 2); binding 0 of each is Ryujinx's support buffer.
//   Constant buffers: c0 and c2 are the driver's banks, made here from the bound state as the driver fills them (c2: texture handles at 0x20 + 8r, images at
//     0x120 + 8r, separate textures at 0x168 + 8r, separate samplers at 0x568 + 8r; c0: storage buffer addresses); c1 is the shader's own constant segment (in its
//     code memory, control +0x700..+0x704); c3 + r is NVN uniform slot r, copied from guest memory at the draw.
//   Textures: the translator names each sampled texture by the bank and word of its handle; the handle is read there (c2 as made, or the game's own uniform
//     buffer for a bindless handle), its IDs looked up in the texture / sampler pools the game registered (mtl_reg_tex / mtl_reg_samp).
//   Vertex and index data, uniform and storage buffers are copied from guest memory into a host-visible arena at the draw; storage buffers a shader writes are
//     copied back when the submission completes.
// Transform feedback (the Adreno has no VK_EXT_transform_feedback): a vertex shader with a feedback layout has a <key>.tf.spv variant that stores its captured
// outputs into storage buffers (set 0, binding 100 + buffer; tools/spv_android.py tf_variant); the capture draws it as points, rasterization off, in a pass
// without attachments, vertex v of the draw at v × stride from the buffer's write offset, copied back to guest memory when the chunk completes (nvn_mtl.m tf_draw:
// non-indexed, non-instanced, not fans / quads). A GPU without vertex-stage storage writes (Mali) captures in the fragment stage
// instead (spv_tffrag.h, tf_frag): the shader wrapped to put vertex v on pixel v of a 1024-wide grid as a one-pixel point, and a fragment shader made at run time from
// the feedback layout storing the outputs there. Any other draw under rasterizer discard runs only when its vertex or geometry stage writes memory, as a dispatch
// (rd_draw: storage buffers, images), its writes on a copy of their home range (rd_bind_w); one that writes nothing draws nothing.
// Compute dispatches run the same way (set 0, the compute stage's banks, c0 + 0x200 the group counts). Buffer textures and buffer images are texel buffer views over
// copies in the arena (written ones copied back); images are the textures' own images (storage usage), so what a shader writes stays on the GPU.
// Coherence: submissions are synchronous and guest memory is copied at each draw, so a draw or dispatch after one that wrote guest memory (storage buffers, buffer
// images, texture-to-buffer copies) first completes the submission; indirect arguments are then read on the CPU and the draw made direct.
// The window origin follows NVN's (mtl_set_origin_lower_left): lower left (GL, NVN's default) keeps the viewport as is, so memory rows are GL's window rows; upper
// left flips the viewport. The front face is inverted by the queue's raster flip alone (g_queue_ll, captured when the queue was initialized), as Metal's draws do: the
// viewport places vertices as Metal's does for both origins, and both judge winding as seen in the framebuffer. Taking the device origin too (front ^ queue ^ device,
// from Metal's CPU coverage test, which works in other coordinates) cancelled the inversion for Link's Awakening, which keeps the lower-left origin for its device
// and its queue: its back-face culled full-screen passes (ambient occlusion, its blurs, the composite) drew nothing, and the world stayed black behind the HUD. Depth -1..1 (mtl_set_depth_zero 0, NVN's default) takes the vertex shaders' .z variants.

// ---------------- A small JSON reader (the translator's res.json) ----------------
typedef struct jv { int t; double num; char *s; struct jv *kid; char **key; int n; } jv;   // t: 0 null 1 number 2 string 3 array 4 object 5 bool
static const char *js_ws(const char *p) { while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++; return p; }
static const char *js_parse(const char *p, jv *v);
static const char *js_str(const char *p, char **out) {
    p++; const char *b = p; while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; p++; }
    size_t n = (size_t)(p - b); *out = malloc(n + 1); memcpy(*out, b, n); (*out)[n] = 0;
    return *p ? p + 1 : p;
}
static const char *js_parse(const char *p, jv *v) {
    memset(v, 0, sizeof *v); p = js_ws(p);
    if (*p == '{' || *p == '[') {
        int obj = *p == '{'; v->t = obj ? 4 : 3; p = js_ws(p + 1);
        while (*p && *p != (obj ? '}' : ']')) {
            v->kid = realloc(v->kid, (v->n + 1) * sizeof *v->kid);
            if (obj) { v->key = realloc(v->key, (v->n + 1) * sizeof *v->key); p = js_str(js_ws(p), &v->key[v->n]); p = js_ws(p); if (*p == ':') p++; }
            p = js_ws(js_parse(p, &v->kid[v->n])); v->n++;
            if (*p == ',') p = js_ws(p + 1);
        }
        return *p ? p + 1 : p;
    }
    if (*p == '"') { v->t = 2; return js_str(p, &v->s); }
    if (!strncmp(p, "true", 4)) { v->t = 5; v->num = 1; return p + 4; }
    if (!strncmp(p, "false", 5)) { v->t = 5; return p + 5; }
    if (!strncmp(p, "null", 4)) return p + 4;
    char *e; v->num = strtod(p, &e); v->t = 1; return e > p ? e : p + 1;
}
static void js_free(jv *v) { for (int i = 0; i < v->n; i++) { js_free(&v->kid[i]); if (v->key) free(v->key[i]); } free(v->kid); free(v->key); free(v->s); }
static const jv *js_get(const jv *o, const char *k) { if (o->t != 4) return NULL; for (int i = 0; i < o->n; i++) if (!strcmp(o->key[i], k)) return &o->kid[i]; return NULL; }
static int js_int(const jv *o, const char *k, int def) { const jv *v = js_get(o, k); return v && (v->t == 1 || v->t == 5) ? (int)v->num : def; }

// ---------------- The pipeline cache ----------------
// The driver's compiled pipelines kept across runs: loaded from DM_PIPELINE_CACHE (front_android.c: the app's internal files) when the first pipeline is made,
// written back by vkb_pipeline_cache_save when it grew: the app paused or stopped, the start-up list done, and during play (pcache_made)
static VkPipelineCache g_pc; static int g_pc_tried; static size_t g_pc_saved;
// DM_PIPELINE_CACHE, with the name of a DM_VK_DRIVER driver after it: a driver ignores another's cache, so one file for both would lose the system driver's whenever a
// player tried another (and a smaller cache is written only once it outgrows the file it replaced)
static char g_pc_path[600];
static VkPipelineCache pcache(void) {
    if (g_pc_tried) return g_pc;
    g_pc_tried = 1;
    const char *env = getenv("DM_PIPELINE_CACHE"), *drv = vk_driver_name(), *path = NULL; void *data = NULL; long n = 0;
    // compiled with FPCR 0 (vk_fp_host): a cache file of its own, so the pipelines compiled under the game's flush-to-zero are not taken from the old one
    const char *fz = vk_fp_host() ? ".fp0" : "";
    if (env && drv) snprintf(g_pc_path, sizeof g_pc_path, "%s%s.%s", env, fz, drv); else if (env) snprintf(g_pc_path, sizeof g_pc_path, "%s%s", env, fz);
    if (env) path = g_pc_path;
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (f) { fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET); data = malloc((size_t)n); if (!data || fread(data, 1, (size_t)n, f) != (size_t)n) n = 0; fclose(f); }
    VkPipelineCacheCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, .initialDataSize = (size_t)n, .pInitialData = n ? data : NULL};
    if (vkCreatePipelineCache(g_vk.dev, &ci, NULL, &g_pc) != VK_SUCCESS) { ci.initialDataSize = 0; ci.pInitialData = NULL; vkCreatePipelineCache(g_vk.dev, &ci, NULL, &g_pc); n = 0; }   // a cache of another driver is refused: started empty
    free(data); g_pc_saved = (size_t)n;
    if (path) LOG("[vk] pipeline cache: %ld bytes from %s", n, path);
    return g_pc;
}
// Under a lock of its own, not the replay's: the driver's cache is internally synchronized, and copying out its tens of megabytes held the game's draws up
static pthread_mutex_t g_pc_mu = PTHREAD_MUTEX_INITIALIZER;
void vkb_pipeline_cache_save(void) {
    const char *path = g_pc_path;   // (set by pcache before g_pc)
    if (!g_pc || !path[0]) return;
    pthread_mutex_lock(&g_pc_mu);
    size_t n = 0; void *data = NULL;
    if (vkGetPipelineCacheData(g_vk.dev, g_pc, &n, NULL) == VK_SUCCESS && n > g_pc_saved && (data = malloc(n)) && vkGetPipelineCacheData(g_vk.dev, g_pc, &n, data) == VK_SUCCESS) {
        char tmp[620]; snprintf(tmp, sizeof tmp, "%s.new", path); FILE *f = fopen(tmp, "wb");
        if (f && fwrite(data, 1, n, f) == n && !fclose(f) && !rename(tmp, path)) { LOG("[vk] pipeline cache: %zu bytes written", n); g_pc_saved = n; }
        else if (f) fclose(f);
    }
    free(data);
    pthread_mutex_unlock(&g_pc_mu);
}
// It is also written during play, after every PC_EVERY pipelines the game makes and at most every PC_GAP_NS, on a thread of its own (copying it out takes a while). Written only
// when the app paused, a session stopped by force (swiped away, updated, the benchmark's stop) lost what it compiled, and the next start made its whole list again without the
// driver's binaries: a Super Smash Bros. Ultimate battle's 700 to 1,100 pipelines, about 13 ms each on a OnePlus Pad 3, competing with play
enum { PC_EVERY = 64 };
#define PC_GAP_NS (15ull * 1000000000ull)
static atomic_int g_pc_new, g_pc_busy; static uint64_t g_pc_last;
static void *pcache_save_thread(void *arg) {
    (void)arg; pthread_setname_np(pthread_self(), "vk-pcache"); setpriority(PRIO_PROCESS, (id_t)gettid(), 10);
    vkb_pipeline_cache_save(); atomic_store(&g_pc_busy, 0); return NULL;
}
// n: what the pipeline counts as. A compile that took long counts as PC_EVERY, so it alone asks for the file (gfx_vk_meshemu.inc: a task / mesh kernel's compute pipeline
// takes 2 to 9 s on a Galaxy Z Flip 7, and a scene's 30 to 40 of them were made again at every start that was stopped before 64 more pipelines); 0: a present, which
// writes what a request inside the gap left waiting
static void pcache_made_n(int n) {   // under g_bk
    if (atomic_fetch_add(&g_pc_new, n) + n < PC_EVERY) return;
    uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC); if (g_pc_last && now - g_pc_last < PC_GAP_NS) return;
    int idle = 0; if (!atomic_compare_exchange_strong(&g_pc_busy, &idle, 1)) return;
    g_pc_last = now; atomic_store(&g_pc_new, 0);
    pthread_t th; if (pthread_create(&th, NULL, pcache_save_thread, NULL)) atomic_store(&g_pc_busy, 0); else pthread_detach(th);
}
static void pcache_made(void) { pcache_made_n(1); }   // pipe_get made a pipeline (under g_bk)

// ---------------- Shaders ----------------
typedef struct { int binding, slot; uint32_t ext; } rb_cbuf;   // ext: bytes the shader reads of it (cb_ext_pick: res.json's or spv_cb_ext2's; 0 = all)
typedef struct { int binding, slot, cb_slot, cb_off, flags; uint32_t ofw, ofk; } rb_sbuf;   // ofw, ofk: a written one's order-free class and store constant (ofw.h, ofw_note)
// The elements of a stage's texture or image array that stage_set writes at most; spv_harr.h shortens a module's longer arrays to it (spv_fit_harr), so that its
// layout holds no more descriptors than are written
enum { TEX_ARR_MAX = 1024 };
typedef struct { int binding, type, cbuf_slot, handle, array, separate, flags; VkFormat fmt; int samp_slot; uint8_t f1d; } rb_tex;   // fmt: an image's declared format (0: Unknown); cbuf_slot / samp_slot: the banks of the texture and sampler handles (rb_tex_read); f1d: res.json's fetch1d names its slot (tex_rows_slot)
enum { SH_VS, SH_PS, SH_GS, SH_TS, SH_CS, SH_MS, SH_TK };   // SH_TS tessellation; SH_MS / SH_TK NVN2's mesh and task shaders (gfx_vk_mesh.inc)
// constant buffers a shader may bind: up to 18 of banks 0 to 17, plus NVN2's push constants (c24, DMSH_PUSH_BANK) and the uniform slots' size table (c25,
// DMSH_CBSIZE_BANK), which the translator adds to every shader that reads a uniform slot: a binding past the cap would be dropped without a word
#define CB_MAX 20
typedef struct {
    char key[17]; int stage, set, ok; VkShaderStageFlagBits vkstage; struct lmod *mod, *modz, *modtf;   // modz, modtf: NULL without a .z / .tf variant (gfx_vk_lmod.inc)
    struct lmod *modtff;   // the capture's fragment shader (tf_frag): modtf is then the shader wrapped by spv_tff_vs, not its .tf.spv; NULL on the vertex-stage capture
    uint32_t tf_stride[4]; VkDescriptorSetLayout dsl_tf;
    int ncb, nsb, ntx, nim; rb_cbuf cb[CB_MAX]; rb_sbuf sb[16]; rb_tex tx[64], im[16];
    VkDescriptorSetLayout dsl;
    char vin[16];   // vertex inputs' types: 'f' float, 'u' uint, 's' int (res.json "vin")
    int vid;   // a vertex shader reading gl_VertexIndex: its draws keep their vertex numbers (bind_vbufs lo 0)
    uint32_t io_in, io_out;   // the locations of its user-defined inputs and outputs (bit n: location n)
    uint32_t io_prim;   // a mesh shader's per-primitive outputs (res.json prim_locs, spv_mesh.py): its fragment shaders read them PerPrimitiveEXT
    struct { uint32_t miss; struct lmod *m; } var[4]; int nvar;   // fragment shaders: variants with unwritten inputs made constants (ps_variant)
    uint32_t tgs;   // bytes of shared memory a workgroup takes from the spill buffer at SPV_TGS_BINDING (its module declares more than the GPU takes: spv_fit_tgs); 0: none
    struct { uint32_t miss, prim; struct lmod *m; } pvar[2]; int npvar;   // fragment shaders: variants reading a mesh shader's per-primitive outputs (gfx_vk_mesh.inc ps_variant_prim)
    int b0;   // a module of its set (.spv, .z.spv, .tf.spv; ps_variant only makes inputs private) reads binding 0, the support buffer (spv_b0_used): stage_set writes it only then
    int wlayer;   // a vertex, tessellation or geometry shader that writes the layer (spv_layer_out): its draws get every layer of their targets (rp_prepare)
    int lbpc, lbase;   // the layer less the push constant base in a vertex shader, plus it in a fragment shader (spv_fit_lbase): lbpc, a module of it reads the base (its
                       // pipelines' layouts take it, and their draws set it); lbase, a vertex shader's every module does, so its draws may be made once per g_vs_layers layers (vs_layer_groups)
    struct { uint32_t miss; struct lmod *m; } avar[4]; int navar;   // alpha-test variants (ps_alpha_mod, spv_alpha.h) by the inputs variant; a NULL module: the transform does not apply
    ds_list *dsf;   // its descriptor sets kept by each chunk, [2 chunk + tf] (dsl, dsl_tf): made at its first set (ds_get), never freed (as the shader)
    uint32_t ndesc;   // descriptors in dsl, an array's counted up to TEX_ARR_MAX (what stage_set writes of it): DM_VK_SET_STATS=1 checks each stage_set writes them all
    uint8_t dsbig;   // bit tf: one set of its layout (dsl, dsl_tf) does not fit an empty pool (sh_get ds_fits, else ds_alloc_n, RED 93): ds_get gives none for it, without making another pool
    uint8_t cbdyn[18], ndyn;   // set_reuse: cb[i] a dynamic uniform buffer, its offset at cbdyn[i] of the bind's (0xff: a plain one, its offset in the set); ndyn of them
    uint8_t pool;   // reads the texture pool by ID: its modules read the pool set (gfx_vk_pool.inc), which its pipelines' layouts take at POOL_SET
    uint8_t gm;   // binds the raw pool table (a storage buffer under DMSH_GMEM_MAP_CB_SLOT): 1, 2 with the stores' write log too (gfx_vk_gmem.inc)
    uint64_t e5[2]; void *cpe5[2];   // compute: sets of storage image bindings bound to E5B9G9R9 textures, and its pipeline for each with those packed (cpipe_e5, spv_e5.h)
    uint8_t upsq;   // a compute shader DM_VK_UPSCALER_SQUARE_CS lists: its dispatches are replaced (gfx_vk_upsq.inc)
    const spv_bmap *bmap;   // its set's binding numbers compacted (spv_bind.h): NULL when none is at SPV_BIND_FROM or past it; its modules and its layout take it
    struct memu_t *memu;   // a task or mesh shader's compute form, on a GPU without VK_EXT_mesh_shader (gfx_vk_meshemu.inc: its facts, pipeline and pass-through vertex shader)
    uint8_t tbrows;   // a texture slot of it read a buffer texture as rows (tex_rows_slot, stage_textures): draw_view lands pending writes to those first
} vshader;
static int memu_on(void); static int memu_facts(vshader *s, const jv *e); static int memu_ready(vshader *s); static vshader *memu_pvs_get(const char *key);   // gfx_vk_meshemu.inc
// DM_VK_OFW=0 (env.txt): a storage buffer a draw writes in an order-free way takes a copy of its own, as other written storage buffers do (ofw_bind)
static int ofw_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_OFW"); v = !(e && *e == '0'); if (!v) LOG("[vk] DM_VK_OFW=0: storage buffers draws write in an order-free way take a copy each, as before"); } return v; }
static const char *ofw_name(uint32_t c) { static const char *n[] = {"-", "stores of one constant", "atomic adds", "atomic unsigned min", "atomic unsigned max", "atomic signed min", "atomic signed max", "atomic and", "atomic or", "atomic xor"}; return c < 10 ? n[c] : "?"; }
static uint64_t g_ofw_shaders;   // shaders with a storage buffer written in an order-free way
// Which storage buffers a draw's shader writes in an order-free way (ofw.h), from its module as packed (the file's binding numbers, as res.json's). A dispatch's are left as
// they are: a dispatch writes on its chunk's copy already, behind a barrier (buf_fwd); so is a task or mesh shader in compute form (its kernels are dispatches)
static void ofw_note(vshader *s, const uint32_t *w, size_t nw) {
    if (s->stage == SH_CS || s->memu || nw < 5 || !w[3] || !ofw_on()) return;
    int any = 0; for (int i = 0; i < s->nsb; i++) any |= s->sb[i].flags & 1;
    if (!any) return;
    uint32_t *sc = malloc(6 * (size_t)w[3] * sizeof *sc); if (!sc) return;
    ofw_bind_t o[32]; int k = ofw_scan(w, nw, sc, o, 32); free(sc);
    int got = 0;
    for (int i = 0; i < s->nsb; i++) if (s->sb[i].flags & 1) for (int j = 0; j < k; j++)
        if (o[j].binding == (uint32_t)s->sb[i].binding && o[j].cls != OFW_NONE) { s->sb[i].ofw = o[j].cls; s->sb[i].ofk = o[j].k; got = 1;
            if (g_ofw_shaders < 8) LOG("[vk] shader %s writes storage buffer binding %d in an order-free way (%s, constant %#x): its draws share one copy a chunk, landed by combining (ofw_bind)",
                                       s->key, s->sb[i].binding, ofw_name(o[j].cls), o[j].k); }
    g_ofw_shaders += got;
}
// Descriptor sets bound again instead of written again (stage_set, sr_find). Every draw copies its constant buffers to new arena offsets, so a set holding those offsets
// never matched the draw before, and stage_set wrote every set of every stage: Breath of the Wild about 7,500 a frame, vkUpdateDescriptorSets 3.4 ms a frame of the
// replay on an Ayn Odin 2. With the constant buffers dynamic uniform buffers, their offsets go to vkCmdBindDescriptorSets, and what a set holds (buffer, range, views,
// samplers) repeats from draw to draw: a set already written with the same descriptors in this recording is bound again. The same design as nx2android's dynamic uniform
// buffers and per-submission set cache (9013fc9, 2b7e2a1), BOTWRecompAndroid's (7cdb67e, 37aaefe) and triasbrata's nxnative-android (DM_VK_STAGE_REUSE), written anew here.
// A shader has at most maxDescriptorSetUniformBuffersDynamic / 2 of them (a pipeline layout's limit covers all its sets: the vertex and fragment shaders' halves), compute
// all it can take, a geometry shader none; the rest stay plain, their offsets part of the set. Binding 0 (the support buffer) stays plain: it is unwritten in most sets.
// DM_VK_SET_REUSE=1 (env.txt, or a game's android_defaults.txt): on. Off by default (plain uniform buffers and a write for every set, as before): it changes every shader's set
// layout, so each device's pipeline cache and start-up list miss once, and dynamic uniform buffers have run only on the Pad 3's Adreno 830 so far
static int set_reuse(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_SET_REUSE"); v = e && *e == '1'; if (v) LOG("[vk] DM_VK_SET_REUSE=1: dynamic uniform buffers, and a descriptor set written in the chunk is bound again"); } return v; }
static void sh_dyn(vshader *s) {   // which constant buffers are dynamic (cbdyn, ndyn): the first that fit, offsets ordered by binding as vkCmdBindDescriptorSets takes them
    memset(s->cbdyn, 0xff, sizeof s->cbdyn); s->ndyn = 0;
    uint32_t lim = g_vk.props.limits.maxDescriptorSetUniformBuffersDynamic, n = s->stage == SH_CS ? lim : s->stage == SH_VS || s->stage == SH_PS ? lim / 2 : 0;
    if (!set_reuse()) n = 0;
    if (n > (uint32_t)s->ncb) n = (uint32_t)s->ncb;
    for (uint32_t i = 0; i < n; i++) { int r = 0; for (uint32_t j = 0; j < n; j++) r += s->cb[j].binding < s->cb[i].binding; s->cbdyn[i] = (uint8_t)r; }
    s->ndyn = (uint8_t)n;
}
static nvn_hmap g_sh = {.klen = 16};
static char g_spv_dir[512];
static int g_depth_zero, g_origin_ll = 1, g_queue_ll;   // g_queue_ll: the queue's raster flip, captured when it was initialized (mtl_set_queue_origin_lower_left); winding = front ^ queue, as Metal's draws
// ---- Inputs no earlier stage writes ----
// Vulkan wants every fragment shader input written by the stage before it (the Switch reads an unwritten one as whatever is there), and the Adreno 619 refuses such
// a pipeline (VK_ERROR_UNKNOWN): Super Mario Bros. Wonder pairs fragment shader f4b9c56566956b03 (locations 0-4) with vertex shaders that write 0, 1, 2 and 4.
// The pipeline then takes a variant of the fragment shader in which those inputs are private variables holding zero
static uint32_t *spv_read(const char *path, size_t *nw) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint32_t *code = malloc((size_t)n + 4); size_t got = code ? fread(code, 1, (size_t)n, f) : 0; fclose(f);
    if (!code || got != (size_t)n || n % 4 || n < 20) { free(code); return NULL; }
    *nw = (size_t)n / 4; return code;
}
static void spv_io(const uint32_t *w, size_t n, uint32_t *in, uint32_t *out) {   // the location masks of the user-defined Input / Output variables
    *in = *out = 0; uint32_t bound = w[3]; int32_t *loc = malloc(bound * sizeof *loc); if (!loc) return;
    for (uint32_t i = 0; i < bound; i++) loc[i] = -1;
    for (size_t i = 5; i < n; ) { uint32_t len = w[i] >> 16, op = w[i] & 0xffff; if (!len || i + len > n) break;
        if (op == 71 && len >= 4 && w[i + 2] == 30 && w[i + 1] < bound) loc[w[i + 1]] = (int32_t)w[i + 3];   // OpDecorate id Location L
        i += len; }
    for (size_t i = 5; i < n; ) { uint32_t len = w[i] >> 16, op = w[i] & 0xffff; if (!len || i + len > n) break;
        if (op == 59 && len >= 4 && w[i + 2] < bound && loc[w[i + 2]] >= 0 && loc[w[i + 2]] < 32) { if (w[i + 3] == 1) *in |= 1u << loc[w[i + 2]]; else if (w[i + 3] == 3) *out |= 1u << loc[w[i + 2]]; }   // OpVariable Input / Output
        i += len; }
    free(loc);
}
static int spv_vid(const uint32_t *w, size_t n) {   // a variable decorated BuiltIn VertexIndex (42)
    for (size_t i = 5; i < n; ) { uint32_t len = w[i] >> 16, op = w[i] & 0xffff; if (!len || i + len > n) break;
        if (op == 71 && len >= 4 && w[i + 2] == 11 && w[i + 3] == 42) return 1;   // OpDecorate id BuiltIn VertexIndex
        i += len; }
    return 0;
}
// ---- Which modules read the support buffer (binding 0) ----
// spv_lit (which words of an instruction are literals): spv_lit.h
// 1 when the module statically uses binding 0 (Vulkan: an instruction in the entry point's call tree names the variable): a variable decorated Binding 0 is an id operand
// of an instruction in a function body, or a global variable's initializer; 1 too when the module cannot be followed (malformed, decoration groups). The support buffer
// is binding 0 of every set (Ryujinx's 1,392 bytes: render scale, the viewport's inverse and size), read by few shaders: 200 of Breath of the Wild's 19,592 (render scale
// only). By operand, not by word: its variable is %13 there, and a literal 13 (an extended instruction's number, a composite index) is no use of it. spv_cb_ext's word match
// takes 15,833 of the game's 28,523 modules (main and .z) for readers; this scan 219, the same modules as spirv-dis's operands
static int spv_b0_used(const uint32_t *w, size_t n) {
    if (n < 5) return 1;
    uint32_t bound = w[3]; uint8_t *b0 = calloc(bound ? bound : 1, 1); int any = 0, used = 0, fn = 0;
    if (!b0) return 1;
    for (size_t i = 5; i < n; ) { uint32_t len = w[i] >> 16, op = w[i] & 0xffff; if (!len || i + len > n) { used = 1; break; }
        if (op == 71 && len >= 4 && w[i + 2] == 33 && w[i + 3] == 0 && w[i + 1] < bound) { b0[w[i + 1]] = 1; any = 1; }   // OpDecorate id Binding 0
        if (op >= 73 && op <= 75) { used = 1; break; }   // OpDecorationGroup / OpGroupDecorate / OpGroupMemberDecorate: not followed
        i += len; }
    for (size_t i = 5; any && !used && i < n; ) { uint32_t len = w[i] >> 16, op = w[i] & 0xffff; if (!len || i + len > n) { used = 1; break; }
        if (op == 54) fn = 1;   // the functions come after every global declaration
        if (fn) { for (uint32_t q = 1; q < len && !used; q++) if (w[i + q] < bound && b0[w[i + q]] && !spv_lit(op, q)) used = 1; }
        else if (op == 59 && len >= 5 && w[i + 4] < bound && b0[w[i + 4]]) used = 1;   // OpVariable type result class initializer
        i += len; }
    free(b0); return used;
}
// whether the module decorates a variable or a block member BuiltIn Layer (OpDecorate id BuiltIn Layer, OpMemberDecorate type member BuiltIn Layer): in a vertex,
// tessellation or geometry shader, a layer output
static int spv_layer_out(const uint32_t *w, size_t n) {
    for (size_t i = 5; i < n; ) { uint32_t len = w[i] >> 16, op = w[i] & 0xffff; if (!len || i + len > n) return 0;
        if ((op == 71 && len >= 4 && w[i + 2] == 11 && w[i + 3] == 9) || (op == 72 && len >= 5 && w[i + 3] == 11 && w[i + 4] == 9)) return 1;
        i += len; }
    return 0;
}
// How many bytes of each constant buffer the shader reads (ext[i] for binding bind[i]; 0 = all of it): the translator declares every one as 4096 vec4, and the replay copied
// the bound buffer whole for every draw (up to 64 KB, six or seven a shader) where Super Mario Bros. Wonder's scene shaders read 64 bytes to 2.3 KB of each. The extent is the
// largest constant element index of the access chains on the buffer's variable; a variable index, or any other use of the variable, keeps the whole buffer
static void spv_cb_ext(const uint32_t *w, size_t n, const rb_cbuf *cb, int ncb, uint32_t *ext) {
    for (int i = 0; i < ncb; i++) ext[i] = 0;
    uint32_t bound = w[3]; int32_t *bind = malloc(bound * sizeof *bind); int64_t *cval = malloc(bound * sizeof *cval); uint8_t *isc = calloc(bound, 1);
    if (!bind || !cval || !isc) { free(bind); free(cval); free(isc); return; }
    for (uint32_t i = 0; i < bound; i++) bind[i] = -1;
    for (size_t i = 5; i < n; ) { uint32_t len = w[i] >> 16, op = w[i] & 0xffff; if (!len || i + len > n) break;
        if (op == 71 && len >= 4 && w[i + 2] == 33 && w[i + 1] < bound) bind[w[i + 1]] = (int32_t)w[i + 3];   // OpDecorate id Binding b
        if (op == 43 && len == 4 && w[i + 2] < bound) { isc[w[i + 2]] = 1; cval[w[i + 2]] = (int32_t)w[i + 3]; }   // OpConstant (32-bit)
        i += len; }
    int64_t mx[CB_MAX]; int full[CB_MAX]; for (int k = 0; k < ncb; k++) { mx[k] = -1; full[k] = 0; }
    for (size_t i = 5; i < n; ) { uint32_t len = w[i] >> 16, op = w[i] & 0xffff; if (!len || i + len > n) break;
        if (op != 71 && op != 5 && op != 6 && op != 15 && op != 59) {   // decorations, names, entry points and the variable's own declaration are not uses
            for (uint32_t q = 1; q < len; q++) { uint32_t id = w[i + q]; if (id >= bound || bind[id] < 0) continue;
                int k = 0; while (k < ncb && cb[k].binding != bind[id]) k++; if (k == ncb) continue;
                if ((op == 65 || op == 66) && q == 3 && len >= 6 && isc[w[i + 5]] && cval[w[i + 5]] >= 0) { if (cval[w[i + 5]] > mx[k]) mx[k] = cval[w[i + 5]]; }   // OpAccessChain t r var member element
                else full[k] = 1; } }
        i += len; }
    for (int k = 0; k < ncb; k++) ext[k] = full[k] || mx[k] < 0 ? 0 : (uint32_t)((mx[k] + 1) * 16 > 0x10000 ? 0x10000 : (mx[k] + 1) * 16);
    free(bind); free(cval); free(isc);
}
// spv_cb_ext by operand, not by word (sh_get; res.json's extent goes over it where it has one, cb_ext_pick). The word match took any word equal to the variable's id for
// a use that keeps the whole buffer: an OpConstant's literal (%int_23 and %int_25 in Breath of the Wild's 753d0169ae3ce4f0), OpMemberDecorate's Offset value 35, an extended
// instruction's number, a composite index. On the Odin 2 4,215 of a frame's 19,633 constant buffer bindings were copied whole (2,336 bytes where 304 are read): its dumped
// frame read 26.2 MB of constant buffers, 21.0 MB with this. Here a use is an id operand (spv_lit) of an instruction in a function body, or a global variable's initializer;
// the buffer's variable is the one decorated with its binding, and its layout must be the translator's (member 0 at offset 0, an array of 4-component 32-bit vectors at
// stride 16) for an element index to count 16 bytes. An access chain from member 0 with a constant element takes it; one with a computed element takes the declared array
// (the whole buffer at 4096 vec4); any other use, a layout not recognized, decoration groups or pointer arithmetic (OpPtrAccessChain) keep the whole buffer. A binding no
// instruction reads gets 16 bytes (a descriptor's range cannot be 0). Over the game's 19,592 shaders it bounds 59,263 of 63,480 bindings, the word match 41,837, and gives the
// same extent wherever the word match bounds one. DM_VK_CB_EXT2=0 (env.txt): spv_cb_ext, as before
static int cb_ext2_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_CB_EXT2"); v = !(e && *e == '0'); } return v; }
static int cb_index(const rb_cbuf *cb, int ncb, int32_t binding) { int k = 0; while (k < ncb && cb[k].binding != binding) k++; return k; }   // ncb: none
static void spv_cb_ext2(const uint32_t *w, size_t n, const rb_cbuf *cb, int ncb, uint32_t *ext) {
    for (int i = 0; i < ncb; i++) ext[i] = 0;
    if (n < 5 || !w[3]) return;
    typedef struct { int64_t c; int32_t bind; uint32_t op, a, b, stride; uint8_t isc, off_nz; } spv_id;   // a, b: a type's / variable's operands (below)
    uint32_t bound = w[3]; spv_id *t = calloc(bound, sizeof *t); if (!t) return;
    for (uint32_t i = 0; i < bound; i++) t[i].bind = -1;
    int give_up = 0;
    for (size_t i = 5; i < n; ) { uint32_t len = w[i] >> 16, op = w[i] & 0xffff; if (!len || i + len > n) { give_up = 1; break; }
        if (op == 71 && len >= 4 && w[i + 1] < bound) { if (w[i + 2] == 33) t[w[i + 1]].bind = (int32_t)w[i + 3]; else if (w[i + 2] == 6) t[w[i + 1]].stride = w[i + 3]; }   // OpDecorate Binding / ArrayStride
        else if (op == 72 && len >= 5 && w[i + 1] < bound && w[i + 2] == 0 && w[i + 3] == 35 && w[i + 4]) t[w[i + 1]].off_nz = 1;   // OpMemberDecorate struct 0 Offset (not 0)
        else if (op == 43 && len == 4 && w[i + 2] < bound) { t[w[i + 2]].isc = 1; t[w[i + 2]].c = w[i + 3]; }   // OpConstant (32-bit: an index is taken unsigned)
        else if ((op == 21 || op == 22) && len >= 3 && w[i + 1] < bound) { t[w[i + 1]].op = op; t[w[i + 1]].a = w[i + 2]; }   // OpTypeInt / OpTypeFloat width
        else if ((op == 23 || op == 28) && len >= 4 && w[i + 1] < bound) { t[w[i + 1]].op = op; t[w[i + 1]].a = w[i + 2]; t[w[i + 1]].b = w[i + 3]; }   // OpTypeVector comp count, OpTypeArray elem length
        else if (op == 30 && len >= 3 && w[i + 1] < bound) { t[w[i + 1]].op = op; t[w[i + 1]].a = w[i + 2]; }   // OpTypeStruct member 0
        else if (op == 32 && len >= 4 && w[i + 1] < bound) { t[w[i + 1]].op = op; t[w[i + 1]].a = w[i + 2]; t[w[i + 1]].b = w[i + 3]; }   // OpTypePointer storage pointee
        else if (op == 59 && len >= 4 && w[i + 2] < bound) { t[w[i + 2]].op = op; t[w[i + 2]].a = w[i + 1]; t[w[i + 2]].b = w[i + 3]; }   // OpVariable type (result) class
        else if ((op >= 73 && op <= 75) || op == 67 || op == 70) give_up = 1;   // decoration groups, OpPtrAccessChain / OpInBoundsPtrAccessChain
        i += len; }
    int64_t mx[CB_MAX]; int full[CB_MAX]; uint32_t alen[CB_MAX]; for (int k = 0; k < ncb; k++) { mx[k] = -1; full[k] = give_up; alen[k] = 0; }
    for (uint32_t v = 0; v < bound && !give_up; v++) {   // each buffer's variables: Uniform, vec4[L] at stride 16 as member 0 at offset 0
        if (t[v].op != 59 || t[v].bind < 0) continue;
        int k = cb_index(cb, ncb, t[v].bind); if (k == ncb) continue;
        uint32_t p = t[v].a, s = p < bound && t[p].op == 32 ? t[p].b : 0, a = s < bound && t[s].op == 30 && !t[s].off_nz ? t[s].a : 0;
        uint32_t e = a && a < bound && t[a].op == 28 && t[a].stride == 16 ? t[a].a : 0, len = a && a < bound && t[a].b < bound && t[t[a].b].isc ? (uint32_t)t[t[a].b].c : 0;
        uint32_t ct = e && e < bound && t[e].op == 23 && t[e].b == 4 ? t[e].a : 0;
        if (t[v].b != 2 || !len || !ct || ct >= bound || (t[ct].op != 21 && t[ct].op != 22) || t[ct].a != 32) full[k] = 1;
        else if (!alen[k] || len < alen[k]) alen[k] = len;
    }
    for (size_t i = 5, fn = 0; i < n && !give_up; ) { uint32_t len = w[i] >> 16, op = w[i] & 0xffff;
        if (op == 54) fn = 1;   // the functions come after every global declaration
        if (!fn) { if (op == 59 && len >= 5 && w[i + 4] < bound && t[w[i + 4]].op == 59 && t[w[i + 4]].bind >= 0) { int k = cb_index(cb, ncb, t[w[i + 4]].bind); if (k < ncb) full[k] = 1; } }   // an initializer
        else for (uint32_t q = 1; q < len; q++) { uint32_t id = w[i + q]; if (id >= bound || t[id].op != 59 || t[id].bind < 0 || spv_lit(op, q) || (op == 59 && q == 2)) continue;
            int k = cb_index(cb, ncb, t[id].bind); if (k == ncb) continue;
            uint32_t m = len >= 6 ? w[i + 4] : 0, el = len >= 6 ? w[i + 5] : 0;
            if ((op == 65 || op == 66) && q == 3 && len >= 6 && m < bound && t[m].isc && t[m].c == 0 && el < bound && alen[k]) {   // OpAccessChain type result var 0 element...
                int64_t x = t[el].isc ? t[el].c : (int64_t)alen[k] - 1;   // a computed element: any of the declared array
                if (x >= alen[k]) full[k] = 1; else if (x > mx[k]) mx[k] = x;
            } else full[k] = 1;
        }
        i += len; }
    for (int k = 0; k < ncb; k++) ext[k] = full[k] || (mx[k] + 1) * 16 >= 0x10000 ? 0 : (uint32_t)(mx[k] < 0 ? 16 : (mx[k] + 1) * 16);
    free(t);
}
// The extents a shader's constant buffers are copied with. res.json's (tools/spv_android.py cb_extents, written when the shader is translated) also bound an element
// computed at run time: the translator's integer registers run along every path with small sets of known values, a loop with a known trip count unrolled. Breath of the
// Wild's vertex shader 5ee9a682fff60855 indexes c3 with a do-while counter (2, then 1) whose exit test is a bool set in the body and branched on in the latch: the scan keeps
// all 64 KB of c3, res.json gives 400 bytes; its 222 draws a frame on the Odin 2 copied 64 KB each into the arena and again into ucopy's shadow, 29 MB a frame (memcpy at
// about 1.2 ms of the replay), and the dumped frame's constant buffer reads go from 21.0 MB (spv_cb_ext2) to 6.6 MB. res.json's extent is taken when it has one (0 or none:
// whole) and it is not below the scan's (a stale or wrong one: the larger, and a red line). DM_VK_RES_EXT=0 (env.txt): res.json's extents ignored, the scan's alone
static int res_ext_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_RES_EXT"); v = !(e && *e == '0'); } return v; }
static uint64_t g_cbx[4];   // the loaded shaders' constant buffer bindings: extent from res.json, res.json's below the scan's (scan kept), extent from the scan, whole
static void cb_ext_pick(vshader *s, const uint32_t *scan, const uint32_t *rext) {
    for (int i = 0; i < s->ncb; i++) {
        uint32_t e = scan[i], r = res_ext_on() && rext[i] <= 0x10000 && !(rext[i] & 15) ? rext[i] : 0;
        if (r && e > r) { RED_ONCE(120, "shader %s: res.json gives constant buffer binding %d %u bytes, its SPIR-V reads %u: the larger is copied (a res.json of another translation?)", s->key, s->cb[i].binding, r, e); g_cbx[1]++; }
        else if (r) { e = r; g_cbx[0]++; }
        else g_cbx[e ? 2 : 3]++;
        s->cb[i].ext = e;
    }
}
// the Private pointer type to pointee t: an existing one, else a new id (listed in np to be declared)
static uint32_t priv_ptr(uint32_t t, uint32_t bound, const uint32_t *pst, const uint32_t *ppt, uint32_t *privp, uint32_t *nb, uint32_t np[][2], int *nnp) {
    if (t >= bound) return 0;
    if (privp[t]) return privp[t];
    for (uint32_t q = 0; q < bound; q++) if (pst[q] == 6 && ppt[q] == t) return privp[t] = q;
    if (*nnp >= 32) return 0;
    privp[t] = (*nb)++; np[*nnp][0] = t; np[(*nnp)++][1] = privp[t];
    return privp[t];
}
// w with its Input variables at the locations in miss made Private, initialized to zero (OpConstantNull); NULL if it cannot be done
static uint32_t *spv_drop_inputs(const uint32_t *w, size_t n, uint32_t miss, size_t *on) {
    uint32_t bound = w[3]; size_t cap = n + 2048; uint32_t *o = malloc(cap * 4);   // what is inserted (pointer types, zeros, variables) is at most 32 x 12 words
    int32_t *loc = malloc(bound * 4); uint32_t *pst = calloc(bound, 4), *ppt = calloc(bound, 4), *privp = calloc(bound, 4), *nullc = calloc(bound, 4); uint8_t *der = calloc(bound, 1);
    uint32_t *res = NULL, nb = bound; size_t k = 0;
    uint32_t np[32][2]; int nnp = 0, nvar = 0; uint32_t vars[32][2];   // new Private pointer types {pointee, id}; the variables {id, pointee}
    if (!o || !loc || !pst || !ppt || !privp || !nullc || !der) goto done;
    for (uint32_t i = 0; i < bound; i++) loc[i] = -1;
    for (size_t i = 5; i < n; ) { uint32_t len = w[i] >> 16, op = w[i] & 0xffff; if (!len || i + len > n) goto done;
        if (op == 71 && len >= 4 && w[i + 2] == 30 && w[i + 1] < bound) loc[w[i + 1]] = (int32_t)w[i + 3];
        if (op == 32 && len == 4 && w[i + 1] < bound) { pst[w[i + 1]] = w[i + 2]; ppt[w[i + 1]] = w[i + 3]; }   // OpTypePointer id storage pointee
        i += len; }
    for (size_t i = 5; i < n; ) { uint32_t len = w[i] >> 16, op = w[i] & 0xffff;
        if (op == 59 && len >= 4 && w[i + 3] == 1 && w[i + 2] < bound && w[i + 1] < bound && loc[w[i + 2]] >= 0 && loc[w[i + 2]] < 32 && (miss >> loc[w[i + 2]] & 1) && nvar < 32) {
            uint32_t t = ppt[w[i + 1]]; if (!priv_ptr(t, bound, pst, ppt, privp, &nb, np, &nnp)) goto done;
            der[w[i + 2]] = 1; vars[nvar][0] = w[i + 2]; vars[nvar++][1] = t; if (t < bound && !nullc[t]) nullc[t] = nb++; }
        if ((op == 65 || op == 66) && len >= 4 && w[i + 3] < bound && der[w[i + 3]] && w[i + 2] < bound && w[i + 1] < bound) {   // access chains into them
            der[w[i + 2]] = 1; if (!priv_ptr(ppt[w[i + 1]], bound, pst, ppt, privp, &nb, np, &nnp)) goto done; }
        i += len; }
    if (!nvar) goto done;
    memcpy(o, w, 20); k = 5; int glob = 0;
    for (size_t i = 5; i < n; ) { uint32_t len = w[i] >> 16, op = w[i] & 0xffff;
        if (k + len + 512 > cap) goto done;
        if (op == 15) {   // OpEntryPoint model id "name" interface...: the variables leave the interface
            size_t s = i + 3; while (s < i + len) { uint32_t x = w[s++]; if (!(x & 0xff) || !(x & 0xff00) || !(x & 0xff0000) || !(x & 0xff000000)) break; }
            size_t at = k; memcpy(o + k, w + i, (s - i) * 4); k += s - i;
            for (size_t q = s; q < i + len; q++) if (!(w[q] < bound && der[w[q]])) o[k++] = w[q];
            o[at] = (uint32_t)((k - at) << 16) | op; i += len; continue; }
        if ((op == 71 || op == 72) && w[i + 1] < bound && der[w[i + 1]]) { i += len; continue; }   // their decorations
        if (op == 59 && w[i + 2] < bound && der[w[i + 2]]) { i += len; continue; }   // declared again below
        if (op == 54 && !glob) {   // before the first function: the pointer types, the zeros and the variables
            glob = 1;
            for (int q = 0; q < nnp; q++) { o[k++] = 4u << 16 | 32; o[k++] = np[q][1]; o[k++] = 6; o[k++] = np[q][0]; }
            for (uint32_t t = 0; t < bound; t++) if (nullc[t]) { o[k++] = 3u << 16 | 46; o[k++] = t; o[k++] = nullc[t]; }
            for (int q = 0; q < nvar; q++) { o[k++] = 5u << 16 | 59; o[k++] = privp[vars[q][1]]; o[k++] = vars[q][0]; o[k++] = 6; o[k++] = nullc[vars[q][1]]; }
        }
        memcpy(o + k, w + i, len * 4);
        if ((op == 65 || op == 66) && w[i + 3] < bound && der[w[i + 3]] && w[i + 1] < bound) o[k + 1] = privp[ppt[w[i + 1]]];
        k += len; i += len;
    }
    o[3] = nb; res = o; o = NULL; *on = k;
done:
    free(o); free(loc); free(pst); free(ppt); free(privp); free(nullc); free(der);
    return res;
}
// The module with each 32 lanes of a wider subgroup one warp (spv_wide.h), in *out (to free); 0 when it needs nothing: a subgroup of 32 lanes, a stage that runs
// no subgroup operations (spv_limits'), or a module that uses none; 0 too for a stage spv_wide leaves (SPV_WIDE_STAGES: not fragment or compute), which the log
// names once. DM_SPV_WIDE=0 (env.txt): modules as translated; DM_SPV_WIDE_CS=0: compute modules as translated
static size_t spv_fit_wide(const uint32_t *code, size_t nw, const char *what, uint32_t **out) {
    static int on = -1, cs = 1;
    if (on < 0) { const char *c = getenv("DM_SPV_WIDE_CS"); cs = !(c && *c == '0'); const char *e = getenv("DM_SPV_WIDE"); on = !(e && *e == '0'); }
    *out = NULL;
    uint32_t st = on && g_sg_size > 32 ? spv_wide_wanted(code, nw, g_sg_stages) : 0;
    if (!st) return 0;
    if (!(st & SPV_WIDE_STAGES & (cs ? ~0u : ~0x20u))) {   // a stage no device test covers yet, or compute switched off: as translated, said once for each
        static _Atomic uint32_t told;
        if (!(atomic_fetch_or(&told, st) & st))
            LOG("[RED] [vk] %s: a %s shader with Maxwell's 32-lane warp instructions on the GPU's %u-lane subgroups: %s, so it is made as translated, and its lanes "
                "32 and up may shuffle from or vote with the wrong lanes (said once for its stage)", what,
                st == 0x20 ? "compute" : st == 0x40 ? "task" : st == 0x80 ? "mesh" : st == 0x8 ? "geometry" : st & 6 ? "tessellation" : "vertex", g_sg_size,
                st == 0x20 ? "DM_SPV_WIDE_CS=0 leaves compute shaders" : "spv_wide rewrites fragment and compute shaders only");
        return 0;
    }
    uint32_t *s = malloc(2 * (size_t)code[3] * 4), *o = NULL; size_t need = 0, k = 0; int got = 0;
    if (!s) return 0;
    spv_wide(code, nw, g_sg_stages, s, NULL, 0, &need, &got);
    if (need && (o = malloc(need * 4))) k = spv_wide(code, nw, g_sg_stages, s, o, need, &need, &got);
    free(s);
    if (!k) { free(o); o = NULL; }
    static _Atomic int logged, nred;
    if (k && !atomic_exchange(&logged, 1))
        LOG("[vk] the GPU's subgroups have %u lanes: shaders that use Maxwell's 32-lane warps are rewritten so that each 32 lanes are one warp (lane & 31, shuffles "
            "within it, its own ballot word). First: %s", g_sg_size, what);
    if (k && vk_debug()) LOG("[vk] %s: warps of 32 lanes in a %u-lane subgroup", what, g_sg_size);
    if ((got & SPV_WIDE_KEPT) && atomic_fetch_add(&nred, 1) < 8)
        LOG("[RED] [vk] %s uses subgroup operations spv_wide does not rewrite for the GPU's %u-lane subgroups: made as it is, lanes 32 and up may read other lanes", what, g_sg_size);
    *out = o; return k;
}
// The module with each subgroup a warp of its own on a GPU whose subgroups are narrower than Maxwell's 32 lanes (spv_narrow.h: the Pixel 9 Pro XL's Mali-G715,
// 16 lanes), in *out (to free); 0 when it needs nothing: 32 lanes or more, a stage that runs no subgroup operations (spv_limits'), or a module with no shuffle.
// DM_SPV_NARROW=0 (env.txt): modules as translated
static size_t spv_fit_narrow(const uint32_t *code, size_t nw, const char *what, uint32_t **out) {
    static int on = -1; if (on < 0) { const char *e = getenv("DM_SPV_NARROW"); on = !(e && *e == '0'); }
    *out = NULL;
    if (!on || g_sg_size >= 32 || !spv_narrow_wanted(code, nw)) return 0;
    size_t sm = 4096; uint32_t *s = malloc((9 * (size_t)code[3] + 2 * sm) * 4), *o = NULL; size_t need = 0, k = 0; int got = 0;
    if (!s) return 0;
    spv_narrow(code, nw, g_sg_stages, g_sg_size, s, sm, NULL, 0, &need, &got);
    if (need && (o = malloc(need * 4))) k = spv_narrow(code, nw, g_sg_stages, g_sg_size, s, sm, o, need, &need, &got);
    free(s);
    if (!k) { free(o); o = NULL; }
    static _Atomic int logged, steps, nred;
    if (k && !atomic_exchange(&logged, 1))
        LOG("[vk] the GPU's subgroups have %u lanes: each runs as a warp of its own (a shuffle from past it reads the lane's own value, and a whole-warp reduction step "
            "past it is the value itself). First: %s", g_sg_size, what);
    if (k && (got & SPV_NARROW_STEP) && !atomic_exchange(&steps, 1)) LOG("[vk] %s: whole-warp reduction steps past the %u-lane subgroup made the value itself (the first such shader)", what, g_sg_size);
    if (k && vk_debug()) LOG("[vk] %s: subgroups of %u lanes as warps%s", what, g_sg_size, got & SPV_NARROW_STEP ? ", reduction steps past them the value itself" : "");
    if ((got & SPV_NARROW_KEPT) && atomic_fetch_add(&nred, 1) < 8)
        LOG("[RED] [vk] %s: a whole-warp reduction step past the GPU's %u-lane subgroup whose result spv_narrow does not follow: left as translated (its shuffle reads the lane's own value)", what, g_sg_size);
    *out = o; return k;
}
// The module with Maxwell's FMUL and FADD kept unfused (NoContraction), for a module of a stage that places vertices (spv_precise.h), in *out (to free); 0 when it
// needs nothing (a fragment or compute shader, or nothing to mark). On every GPU: Maxwell rounds them apart, so the driver may not fuse them either, and two vertex
// shaders that compute one position give one depth. DM_SPV_PRECISE=0 (env.txt): modules as translated, the driver contracting as it likes
static size_t spv_fit_precise(const uint32_t *code, size_t nw, const char *what, uint32_t **out) {
    static int on = -1; if (on < 0) { const char *e = getenv("DM_SPV_PRECISE"); on = !(e && *e == '0'); }
    *out = NULL;
    if (!on || !spv_precise_wanted(code, nw)) return 0;
    uint32_t *s = malloc(((size_t)code[3] + 31) / 32 * 4 + 4), *o = NULL; size_t need = 0, k = 0; int got = 0;
    if (!s) return 0;
    spv_precise(code, nw, s, NULL, 0, &need, &got);
    if (need && (o = malloc(need * 4))) k = spv_precise(code, nw, s, o, need, &need, &got);
    free(s);
    if (!k) { free(o); return 0; }
    static _Atomic int logged;
    if (!atomic_exchange(&logged, 1))
        LOG("[vk] vertex, tessellation and geometry shaders keep Maxwell's unfused FMUL and FADD (NoContraction), so a depth pre-pass and a later pass with another "
            "vertex shader get the same depths (DM_SPV_PRECISE=0: the driver may fuse them). First: %s, %d", what, got);
    if (vk_debug()) LOG("[vk] %s: %d results kept unfused", what, got);
    *out = o; return k;
}
// The module with its layer and viewport output made ordinary outputs, on a GPU that has no such output from vertex shaders (spv_layer.h): no
// VK_EXT_shader_viewport_index_layer, Vulkan 1.1. Its compiler crashed on them (the Adreno 730's driver V@0615.98). In *out (to free); 0 when it needs nothing.
// DM_SPV_LAYER=0 (env.txt): modules as translated
// ---- Compute shared memory past the GPU's limit (spv_tgspill.h) ----
// The limit: maxComputeSharedMemorySize (32 KB on Qualcomm's Adreno 830 driver), or DM_VK_SHARED_MAX=<bytes> (env.txt: a smaller one, to try the move on a GPU with
// room). DM_SPV_TGSPILL=0 (env.txt): every module as it is
static long long tgs_limit(void) { static long long v = -1; if (v < 0) { const char *e = getenv("DM_VK_SHARED_MAX"); v = e && *e ? atoll(e) : (long long)g_vk.props.limits.maxComputeSharedMemorySize; } return v; }
static int tgs_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_SPV_TGSPILL"); v = !(e && *e == '0'); } return v; }
// bytes a workgroup of the module takes from the spill buffer: its shared memory, when that is past the limit and spv_tgspill can move it; 0 otherwise
static uint32_t tgs_bytes(const uint32_t *code, size_t nw, const char *what) {
    if (!tgs_on() || nw < 5 || !code[3]) return 0;
    uint32_t *sc = malloc(16 * (size_t)code[3]); if (!sc) return 0;
    long long b = spv_tgs_bytes(code, nw, sc); free(sc);
    if (b == SPV_TGS_TYPE || b == SPV_TGS_USE) { static _Atomic int nred; if (atomic_fetch_add(&nred, 1) < 8) LOG("[RED] [vk] %s declares shared memory spv_tgspill does not move (%s): made as it is, whatever its size against the GPU's %lld bytes",
                                                     what, b == SPV_TGS_TYPE ? "not arrays of one 32-bit type" : "a use other than through one element", tgs_limit()); return 0; }
    return b > tgs_limit() ? (uint32_t)b : 0;
}
static size_t spv_fit_tgs(const uint32_t *code, size_t nw, const char *what, uint32_t **out) {
    *out = NULL; uint32_t b = tgs_bytes(code, nw, what); if (!b) return 0;
    size_t cap = 4 * nw + 1024; uint32_t *sc = malloc(16 * (size_t)code[3]), *o = malloc(cap * 4);
    size_t k = sc && o ? spv_tgspill(code, nw, SPV_TGS_BINDING, sc, o, cap) : 0; free(sc);
    if (!k) { free(o); static _Atomic int nred; if (atomic_fetch_add(&nred, 1) < 8) LOG("[RED] [vk] %s: spv_tgspill could not move its %u bytes of shared memory: made as it is, past the GPU's %lld", what, b, tgs_limit()); return 0; }
    static _Atomic int logged; if (!atomic_exchange(&logged, 1)) LOG("[vk] the GPU takes %lld bytes of shared memory a compute shader: shaders that declare more take it from a storage buffer, a region a workgroup (spv_tgspill). First: %s, %u bytes", tgs_limit(), what, b);
    *out = o; return k;
}
// The vertex shader with its outputs started as (0,0,0,1) (spv_vsinit.h), in *out (to free); 0 when it is no vertex shader or has no output to start.
// On every GPU: an unwritten component is undefined in Vulkan and reads 0 on the Adreno 840 and the Mali-G715, where the hardware gives (0,0,0,1).
// DM_SPV_VSINIT=0 (env.txt): modules as translated
static size_t spv_fit_vsinit(const uint32_t *code, size_t nw, const char *what, uint32_t **out) {
    static int on = -1; if (on < 0) { const char *e = getenv("DM_SPV_VSINIT"); on = !(e && *e == '0'); if (!on) LOG("[vk] DM_SPV_VSINIT=0: vertex shader outputs are not started as (0,0,0,1)"); }
    *out = NULL;
    if (!on || nw < 5 || !spv_vsinit_wanted(code, nw)) return 0;
    size_t cap = nw + 15 + 3 * SPV_VSINIT_MAX; uint8_t *s = malloc(2 * (size_t)code[3]); uint32_t *o = malloc(cap * 4);
    size_t k = s && o ? spv_vsinit(code, nw, s, o, cap) : 0; free(s);
    if (!k) { free(o); return 0; }
    static _Atomic int logged;
    if (!atomic_exchange(&logged, 1)) LOG("[vk] vertex shader outputs start as (0,0,0,1), as on the hardware, where Vulkan leaves an unwritten component undefined (DM_SPV_VSINIT=0: as translated). First: %s", what);
    *out = o; return k;
}
// The module with each raw global memory helper keeping its last pool table entry's range and delta in private words (spv_gmcache.h), in *out (to free); 0 when it
// has no such helper, or one not in the translator's form (said once). DM_SPV_GMCACHE=0 (env.txt): helpers as translated, reading the entry's six words on every access
static size_t spv_fit_gmcache(const uint32_t *code, size_t nw, const char *what, uint32_t **out) {
    static int on = -1; if (on < 0) { const char *e = getenv("DM_SPV_GMCACHE"); on = !(e && *e == '0'); if (!on) LOG("[vk] DM_SPV_GMCACHE=0: raw global memory helpers read their last entry's range from the table on every access"); }
    *out = NULL;
    if (!on || nw < 5 || !spv_gmcache_wanted(code, nw)) return 0;
    size_t cap = nw + SPV_GMCACHE_EXTRA(SPV_GMCACHE_MAX); uint32_t *s = malloc(4 * (size_t)code[3] * 4), *o = malloc(cap * 4); int why = 0;
    size_t k = s && o ? spv_gmcache(code, nw, s, o, cap, &why) : 0; free(s);
    if (!k) { free(o);
        static _Atomic int told; if (why && !atomic_exchange(&told, 1)) LOG("[vk] %s: raw global memory helpers not in the translator's form (%d), made as translated (said once)", what, why);
        return 0; }
    static _Atomic int logged;
    if (!atomic_exchange(&logged, 1)) LOG("[vk] raw global memory helpers keep their last entry's range and delta in private words, so a hit reads no table word (DM_SPV_GMCACHE=0: as translated). First: %s", what);
    *out = o; return k;
}
static size_t spv_fit_layer(const uint32_t *code, size_t nw, const char *what, uint32_t **out) {
    static int on = -1; if (on < 0) { const char *e = getenv("DM_SPV_LAYER"); on = !(e && *e == '0'); }
    *out = NULL;
    if (!on || g_layer_out || !spv_layer_wanted(code, nw)) return 0;
    uint32_t *o = malloc(nw * 4); size_t k = o ? spv_layer(code, nw, o, nw) : 0;
    static _Atomic int logged, nred;
    if (!k) {
        int parsed = o != NULL; free(o);
        if (parsed && atomic_fetch_add(&nred, 1) < 8) LOG("[RED] [vk] %s declares layer or viewport output that spv_layer does not rewrite (a block member): made as it is, on a GPU without it", what);
        return 0;
    }
    if (!atomic_exchange(&logged, 1))
        LOG("[vk] the GPU has no layer or viewport output from vertex shaders (no VK_EXT_shader_viewport_index_layer, Vulkan 1.1): shaders that write them write an unused "
            "output instead, and draw to layer 0 and viewport 0 (DM_SPV_LAYER=0: as translated). First: %s", what);
    if (vk_debug()) LOG("[vk] %s: layer or viewport output rewritten as an ordinary output", what);
    *out = o; return k;
}
static __thread int t_lbase_info;   // what spv_fit_lbase did to the modules lm_code made since sh_get cleared it: 1, a module rewritten; 2, one with layer output left as it was
// The module with its layer output less the push constant base, or its layer input plus it (spv_layer.h spv_layer_base), in *out (to free); 0 when it needs nothing:
// a GPU whose vertex shaders reach every layer (g_vs_layers 0), or no layer in the module. DM_SPV_LBASE=0 (env.txt): as translated, and each draw made once (the layers
// past the limit undrawn)
static size_t spv_fit_lbase(const uint32_t *code, size_t nw, const char *what, uint32_t **out) {
    static int on = -1; if (on < 0) { const char *e = getenv("DM_SPV_LBASE"); on = !(e && *e == '0'); }
    *out = NULL;
    if (!on || !g_vs_layers || !g_layer_out || !spv_layer_out(code, nw)) return 0;
    size_t need = 0; spv_layer_base(code, nw, NULL, 0, &need);
    uint32_t *o = need ? malloc(need * 4) : NULL; size_t k = o ? spv_layer_base(code, nw, o, need, NULL) : 0;
    if (!k) { free(o); t_lbase_info |= 2; return 0; }   // a geometry shader's (no limit there), or a form spv_layer_base leaves
    t_lbase_info |= 1;
    static _Atomic int logged;
    if (!atomic_exchange(&logged, 1))
        LOG("[vk] the GPU's vertex shaders reach only layers 0-%d of a layered pass: those that write the layer write it less a base (fragment shaders that read it add the "
            "base back), and their draws into more layers are made once per %d layers (DM_VK_VS_LAYERS, DM_SPV_LBASE=0: as translated). First: %s", g_vs_layers - 1, g_vs_layers, what);
    *out = o; return k;
}
// The module with its butterfly shuffles as ShuffleXor (spv_xor.h), in *out (to free); 0 when it needs nothing: a GPU spv_xor is not for, subgroups under 32 lanes
// (spv_narrow rewrites those), a stage that runs no subgroup operations (spv_limits'), or no shuffle by the lane ID ^ a mask. For Qualcomm's own driver below
// Vulkan 1.3 (its GPU name "Adreno (TM) ..."; Turnip's differs): the Adreno 730's (V@0615.98) returns garbage from the translator's Shuffle by the lane ^ the
// mask in compute shaders, and ShuffleXor is right. The Adreno 740's and 830's drivers (Vulkan 1.3) run the shuffle as translated, which is left to them.
// DM_SPV_XOR=1 (env.txt): on any GPU of 32 lanes or more (ShuffleXor is exact everywhere); =0: never
static int xor_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_SPV_XOR");
        int q = g_vk.props.vendorID == 0x5143 && !strncmp(g_vk.props.deviceName, "Adreno (TM)", 11) && g_vk.props.apiVersion < VK_API_VERSION_1_3;
        v = e && *e == '0' ? 0 : e && *e == '1' ? 1 : q;
    }
    return v;
}
static size_t spv_fit_xor(const uint32_t *code, size_t nw, const char *what, uint32_t **out) {
    *out = NULL;
    if (g_sg_size < 32 || !xor_on() || !spv_xor_wanted(code, nw) || !spv_wide_wanted(code, nw, g_sg_stages)) return 0;
    uint32_t *s = malloc(SPX_N * (size_t)code[3] * 4), *o = NULL; size_t need = 0, k = 0; int ns = 0;
    if (!s) return 0;
    spv_xor(code, nw, s, NULL, 0, &need, &ns);
    if (need && (o = malloc(need * 4))) k = spv_xor(code, nw, s, o, need, &need, &ns);
    free(s);
    if (!k) { free(o); return 0; }
    static _Atomic int logged;
    if (!atomic_exchange(&logged, 1))
        LOG("[vk] %s (Vulkan %u.%u): the translator's butterfly shuffles (Shuffle by the lane ^ a mask) are made ShuffleXor, which this driver runs right "
            "(DM_SPV_XOR=0: as translated). First: %s, %d", g_vk.props.deviceName, VK_API_VERSION_MAJOR(g_vk.props.apiVersion), VK_API_VERSION_MINOR(g_vk.props.apiVersion), what, ns);
    if (vk_debug()) LOG("[vk] %s: %d butterfly shuffles made ShuffleXor", what, ns);
    *out = o; return k;
}
// The module without subgroup operations in a stage where the GPU runs none (spv_limits.h), in *out (to free); 0 when it needs nothing. A GPU that runs them in
// every graphics stage gets every module as translated, untouched; so does one that runs them in the module's stage. DM_SPV_LIMITS=0 (env.txt): modules as translated
static size_t spv_fit(const uint32_t *code, size_t nw, const char *what, uint32_t **out) {
    static int on = -1; if (on < 0) { const char *e = getenv("DM_SPV_LIMITS"); on = !(e && *e == '0'); }
    *out = NULL;
    uint32_t *xo = NULL; size_t xk = spv_fit_xor(code, nw, what, &xo);   // first: spv_wide keeps its ShuffleXor
    if (xk) { code = xo; nw = xk; }
    size_t wk = spv_fit_wide(code, nw, what, out); if (wk) { free(xo); return wk; }   // a stage that runs them: as wide as the GPU's subgroups
    size_t nk = spv_fit_narrow(code, nw, what, out); if (nk) { free(xo); return nk; }   // or as narrow
    if (xk) { *out = xo; return xk; }   // a stage that runs them (spv_fit_xor's check), so nothing for spv_limits
    if (!on || (g_sg_stages & VK_SHADER_STAGE_ALL_GRAPHICS) == VK_SHADER_STAGE_ALL_GRAPHICS || !spv_lim_wanted(code, nw, g_sg_stages)) return 0;
    uint32_t *s = malloc(2 * (size_t)code[3] * 4), *o = NULL; size_t need = 0, k = 0; int got = 0;
    if (!s) return 0;
    spv_limits(code, nw, g_sg_stages, s, NULL, 0, &need, &got);
    if (need && (o = malloc(need * 4))) k = spv_limits(code, nw, g_sg_stages, s, o, need, &need, &got);
    free(s);
    if (!k) { free(o); o = NULL; }
    static _Atomic int logged, nred;
    if (k && !atomic_exchange(&logged, 1))
        LOG("[vk] the GPU runs subgroup operations in stages %#x only: shaders of other stages that use them are rewritten without (fragment shaders: lanes of a 2x2 quad, "
            "shuffles through fine derivatives; other stages: one lane). First: %s", g_sg_stages, what);
    if (k && vk_debug()) LOG("[vk] %s: subgroup operations rewritten (%s)", what, got & SPV_LIM_SG_QUAD ? "quad" : "one lane");
    if ((got & SPV_LIM_SG_KEPT) && atomic_fetch_add(&nred, 1) < 8)
        LOG("[RED] [vk] %s uses subgroup operations in a stage where the GPU has none (it runs them in stages %#x), some of which spv_limits does not rewrite: made as it is", what, g_sg_stages);
    *out = o; return k;
}
static size_t spv_pool_fit(const uint32_t *code, size_t nw, const char *what, uint32_t **out);   // gfx_vk_pool.inc
static size_t spv_fit_harr(const uint32_t *code, size_t nw, const char *what, uint32_t **out);   // after gfx_vk_pool.inc (POOL_SET)
static int pool_on(void); static void pool_note_shader(vshader *s, int info); static void pool_drop_view(VkImageView iv);
#include "gfx_vk_lmod.inc"   // shader modules made when a pipeline needs them: lmod, lm_new, lm_pipeline
static lmod *ps_variant(vshader *s, uint32_t miss) {
    for (int i = 0; i < s->nvar; i++) if (s->var[i].miss == miss) return s->var[i].m;
    char p[700]; snprintf(p, sizeof p, "%s/%s.spv", g_spv_dir, s->key);
    lmod *m = lm_new(p, miss, s->bmap, NULL);
    LOG("[vk] fragment shader %s: inputs at locations %#x are written by no earlier stage of a pipeline: a variant reads them as zero%s", s->key, miss, m ? "" : " [RED] (variant failed: the pipeline keeps the shader as it is)");
    if (!m) m = s->mod;
    if (s->nvar < 4) { s->var[s->nvar].miss = miss; s->var[s->nvar++].m = m; }
    return m;
}
// Where transform feedback is captured. The vertex stage's capture (.tf.spv) needs vertexPipelineStoresAndAtomics, which Arm's Mali drivers lack, so there the
// capture runs in the fragment stage (spv_tffrag.h), which needs fragmentStoresAndAtomics.
// DM_VK_TF_FRAG (env.txt): 1 captures in the fragment stage on any GPU with fragment stores (to compare the two on an Adreno), 0 never (the capture is then refused, RED 115)
static int tf_frag(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("DM_VK_TF_FRAG");
        int want = e && *e == '1' ? 1 : e && *e == '0' ? 0 : !g_vk_feat.vertexPipelineStoresAndAtomics;
        v = want && g_vk_feat.fragmentStoresAndAtomics;
        if (v || (e && *e)) LOG("[vk] transform feedback: captured in the %s stage%s", v ? "fragment" : "vertex", e && *e ? " (DM_VK_TF_FRAG)" : ": the GPU has no vertex-stage storage writes");
        else if (want) LOG("[RED] [vk] transform feedback: the GPU has neither vertex- nor fragment-stage storage writes, so nothing can be captured"); }
    return v;
}
// The capture's fragment shader for vertex shader s (its layout L, its vec4 outputs outs and SPIR-V version ver): made now, a few hundred words. NULL: red line
static lmod *tff_fs_mod(const vshader *s, const spv_tff_layout *L, uint32_t outs, uint32_t ver) {
    enum { CAP = 4096 }; uint32_t *o = malloc(CAP * 4); if (!o) return NULL;
    size_t n = spv_tff_fs(ver, L, outs, o, CAP); lmod *m = NULL;
    char what[96]; snprintf(what, sizeof what, "%s (its transform feedback's fragment shader)", s->key);
    if (n) {
        const char *d = getenv("DM_DUMP_DIR");   // for spirv-val
        if (d) { char q[700]; mkdir(d, 0777); snprintf(q, sizeof q, "%s/tff_%s.fs.spv", d, s->key); FILE *f = fopen(q, "wb"); if (f) { fwrite(o, 4, n, f); fclose(f); } }
        m = lm_gen(o, n, what);
    } else LOG("[RED] [vk] vertex shader %s: its feedback layout has a buffer of more than 128 words a vertex, so it cannot be captured in the fragment stage", s->key);
    free(o); return m;
}
static lmod *sh_module(const vshader *s, const char *path, int *b0) { return lm_new(path, 0, s->bmap, b0); }   // *b0 |= the module reads the support buffer (spv_b0_used)
// Ryujinx's TextureFormat names (an image's declared format) → Vulkan, with the texel size
static const struct { const char *n; VkFormat f; int bytes; } DECL_FMT[] = {
    {"R8Unorm", VK_FORMAT_R8_UNORM, 1}, {"R8Snorm", VK_FORMAT_R8_SNORM, 1}, {"R8Uint", VK_FORMAT_R8_UINT, 1}, {"R8Sint", VK_FORMAT_R8_SINT, 1},
    {"R16Float", VK_FORMAT_R16_SFLOAT, 2}, {"R16Unorm", VK_FORMAT_R16_UNORM, 2}, {"R16Snorm", VK_FORMAT_R16_SNORM, 2}, {"R16Uint", VK_FORMAT_R16_UINT, 2}, {"R16Sint", VK_FORMAT_R16_SINT, 2},
    {"R32Float", VK_FORMAT_R32_SFLOAT, 4}, {"R32Uint", VK_FORMAT_R32_UINT, 4}, {"R32Sint", VK_FORMAT_R32_SINT, 4},
    {"R8G8Unorm", VK_FORMAT_R8G8_UNORM, 2}, {"R8G8Snorm", VK_FORMAT_R8G8_SNORM, 2}, {"R8G8Uint", VK_FORMAT_R8G8_UINT, 2}, {"R8G8Sint", VK_FORMAT_R8G8_SINT, 2},
    {"R16G16Float", VK_FORMAT_R16G16_SFLOAT, 4}, {"R16G16Unorm", VK_FORMAT_R16G16_UNORM, 4}, {"R16G16Snorm", VK_FORMAT_R16G16_SNORM, 4}, {"R16G16Uint", VK_FORMAT_R16G16_UINT, 4}, {"R16G16Sint", VK_FORMAT_R16G16_SINT, 4},
    {"R32G32Float", VK_FORMAT_R32G32_SFLOAT, 8}, {"R32G32Uint", VK_FORMAT_R32G32_UINT, 8}, {"R32G32Sint", VK_FORMAT_R32G32_SINT, 8},
    {"R8G8B8A8Unorm", VK_FORMAT_R8G8B8A8_UNORM, 4}, {"R8G8B8A8Snorm", VK_FORMAT_R8G8B8A8_SNORM, 4}, {"R8G8B8A8Uint", VK_FORMAT_R8G8B8A8_UINT, 4}, {"R8G8B8A8Sint", VK_FORMAT_R8G8B8A8_SINT, 4},
    {"R16G16B16A16Float", VK_FORMAT_R16G16B16A16_SFLOAT, 8}, {"R16G16B16A16Unorm", VK_FORMAT_R16G16B16A16_UNORM, 8}, {"R16G16B16A16Snorm", VK_FORMAT_R16G16B16A16_SNORM, 8},
    {"R16G16B16A16Uint", VK_FORMAT_R16G16B16A16_UINT, 8}, {"R16G16B16A16Sint", VK_FORMAT_R16G16B16A16_SINT, 8},
    {"R32G32B32A32Float", VK_FORMAT_R32G32B32A32_SFLOAT, 16}, {"R32G32B32A32Uint", VK_FORMAT_R32G32B32A32_UINT, 16}, {"R32G32B32A32Sint", VK_FORMAT_R32G32B32A32_SINT, 16},
    {"R10G10B10A2Unorm", VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4}, {"R10G10B10A2Uint", VK_FORMAT_A2B10G10R10_UINT_PACK32, 4}, {"R11G11B10Float", VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4}};
static int fmt_bytes(VkFormat f) {   // the texel size of an uncompressed colour format the views below meet (0: not known here)
    for (size_t i = 0; i < sizeof DECL_FMT / sizeof *DECL_FMT; i++) if (DECL_FMT[i].f == f) return DECL_FMT[i].bytes;
    switch (f) { case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_SRGB: case VK_FORMAT_A8B8G8R8_UNORM_PACK32: return 4; default: return 0; }
}
static void rb_tex_read(const jv *a, rb_tex *out, int *n, int cap) {
    for (int i = 0; a && i < a->n && *n < cap; i++) {
        const jv *o = &a->kid[i]; const jv *fm = js_get(o, "format"); VkFormat f = VK_FORMAT_UNDEFINED;
        for (size_t k = 0; fm && fm->s && k < sizeof DECL_FMT / sizeof *DECL_FMT; k++) if (!strcmp(fm->s, DECL_FMT[k].n)) f = DECL_FMT[k].f;
        // the translator's cbuf_slot is Ryujinx's TextureHandle.PackSlots: texture bank in the low 16 bits, sampler bank + 1 in the high ones (0: the texture's); -1: none given (c2).
        // Read whole it named no bank for a separate texture with its sampler (0x30002: both in c2), and every such texture read 0 (Danganronpa V3, a black screen)
        int cs = js_int(o, "cbuf_slot", 2), ts = cs < 0 ? -1 : (int)(uint16_t)cs, ss = cs < 0 ? -1 : (uint32_t)cs >> 16 ? (int)((uint32_t)cs >> 16) - 1 : ts;
        out[(*n)++] = (rb_tex){js_int(o, "binding", 0), js_int(o, "type", 3), ts, js_int(o, "handle", 0), js_int(o, "array", 1), js_int(o, "separate", 0), js_int(o, "flags", 0), f, ss};
    }
}
// A texture slot that may hold a buffer texture read as rows of 16384 texels (tbuf_rows.h, gfx_vk.c vt_get_s): a single 2D slot (no array, multisample or shadow) that
// the translator read by wrapped 1D fetches (res.json fetch1d) or whose handle sits in a uniform buffer, where the Mac binds such rows (nvn_mtl.m tbuf_as_2d: f1d or TEXU,
// declared 2D). Elsewhere a buffer texture in a texture slot reads 0, on the Mac as here. Assassin's Creed II (the Ezio Collection) reads its 48-float buffer texture so in its
// lit passes, and every one of them was black while the slot read the dummy
static int tex_rows_slot(const rb_tex *t) { return (t->type & 0x7ff) == 3 && t->array <= 1 && (t->f1d || t->cbuf_slot >= 3) && tbuf_rows_on(); }
static void rb_tex_f1d(const jv *f1, rb_tex *tx, int n) {   // res.json's fetch1d (translator slots, tbr_slot) onto the texture bindings
    for (int i = 0; f1 && f1->t == 3 && i < n; i++) { int sl = tbr_slot((uint32_t)tx[i].handle, tx[i].cbuf_slot);
        for (int q = 0; sl >= 0 && q < f1->n; q++) if ((int)f1->kid[q].num == sl) tx[i].f1d = 1; }
}
static VkDescriptorType tex_dtype(const rb_tex *t, int image) {
    int buf = (t->type & 0xff) == 2;
    return image ? (buf ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) : (buf ? VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
}
// What each pool holds (ds_pool_add), by the descriptor types sh_get's layouts use
static const VkDescriptorPoolSize g_ds_pool_sz[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 16384}, {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 16384}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16384},
                                                    {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1024}, {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1024}, {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1024}};
// Whether one set of a layout with these bindings fits an empty pool. A layout over it never gets a set, and it is known from the layout alone: tried in a pool, each such
// shader made a pool of its own (ds_alloc_n's fresh pool, about 9.75 MB on Adreno 840) before its first draw was dropped. NVN2 shaders that read the texture pool by ID
// (Ryujinx's handle fallback, <stage>_t_cb0_40000000: 2^20 elements; nvn_mtl.m pool_gpu on the Mac) were over it: Ocarina of Time's Kokiri Forest met 180 of them in 40 s,
// 1.75 GB of pools no set was ever made from, beside the 4.6 GB the process held when the low-memory killer ended it (DM_VK_MEMREPORT). Their arrays are the pool set's now
// (gfx_vk_pool.inc), and they are over it only with DM_VK_POOL=0 or on a GPU without what the pool set needs
static int ds_fits(const VkDescriptorSetLayoutBinding *b, uint32_t nb) {
    for (size_t i = 0; i < sizeof g_ds_pool_sz / sizeof *g_ds_pool_sz; i++) {
        uint64_t n = 0;
        for (uint32_t j = 0; j < nb; j++) if (b[j].descriptorType == g_ds_pool_sz[i].type) n += b[j].descriptorCount;
        if (n > g_ds_pool_sz[i].descriptorCount) return 0;
    }
    return 1;
}
static uint64_t g_ds_toobig;   // shaders whose layout fits no pool (ds_fits): their draws are dropped
static int upsq_match(const vshader *s); static int upsq_run(const vshader *s);   // gfx_vk_upsq.inc
static vtex *g_upsq_src, *g_upsq_dst; static VkImageView g_upsq_src_iv;   // a listed upscaler's input and output, as stage_set binds them for the dispatch at hand (gfx_vk_upsq.inc)
// A shader's set with far-apart binding numbers (spv_bind.h): the map from its res.json, made before its modules, which take it (lm_code); sh_bind_apply gives its
// layout and stage_set's writes the new numbers once the matching of res.json against the file (spv_cb_ext) is done. DM_VK_BIND_COMPACT=0: the numbers as they are
static uint64_t g_bind_shaders, g_bind_entries;
static int bind_compact_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_BIND_COMPACT"); v = !(e && *e == '0'); if (!v) LOG("[vk] DM_VK_BIND_COMPACT=0: binding numbers as the translator gave them"); } return v; }
static void sh_bind_map(vshader *s) {
    if (!bind_compact_on()) return;
    uint32_t b[CB_MAX + 16 + 64 + 16 + 4]; int nb = 0;
    for (int i = 0; i < s->ncb; i++) b[nb++] = (uint32_t)s->cb[i].binding;
    for (int i = 0; i < s->nsb; i++) b[nb++] = (uint32_t)s->sb[i].binding;
    for (int i = 0; i < s->ntx; i++) b[nb++] = (uint32_t)s->tx[i].binding;
    for (int i = 0; i < s->nim; i++) b[nb++] = (uint32_t)s->im[i].binding;
    spv_bmap m; int n = spv_bmap_make(b, nb, &m);
    if (n < 0) { RED_ONCE(179, "shader %s: more than %d bindings at %u or past it: their numbers are left as they are, and a driver that sizes a layout by its highest binding keeps memory for each", s->key, SPV_BIND_MAX, SPV_BIND_FROM); return; }
    if (!n) return;
    spv_bmap *c = malloc(sizeof *c); if (!c) return; *c = m; s->bmap = c;
    g_bind_shaders++; g_bind_entries += (uint64_t)n;
    static int nl; if (nl++ < 4) LOG("[vk] shader %s: %d binding numbers made dense (%u -> %u%s; spv_bind.h, DM_VK_BIND_COMPACT=0: off)", s->key, n, m.from[0], m.to[0], n > 1 ? ", ..." : "");
}
static void sh_bind_apply(vshader *s) {
    const spv_bmap *m = s->bmap; if (!m) return;
    for (int i = 0; i < s->ncb; i++) s->cb[i].binding = (int32_t)spv_bmap_get(m, (uint32_t)s->cb[i].binding);
    for (int i = 0; i < s->nsb; i++) s->sb[i].binding = (int32_t)spv_bmap_get(m, (uint32_t)s->sb[i].binding);
    for (int i = 0; i < s->ntx; i++) s->tx[i].binding = (int32_t)spv_bmap_get(m, (uint32_t)s->tx[i].binding);
    for (int i = 0; i < s->nim; i++) s->im[i].binding = (int32_t)spv_bmap_get(m, (uint32_t)s->im[i].binding);
}
static vshader *sh_get(const char *key) {
    if (key[0] >= 'g' && key[0] <= 'v') return memu_pvs_get(key);   // a mesh shader's pass-through vertex shader (gfx_vk_meshemu.inc memu_pvs_key)
    char k[16]; memcpy(k, key, 16);
    vshader *s = nvn_hm_get(&g_sh, k); if (s) return s->ok ? s : NULL;
    s = calloc(1, sizeof *s); memcpy(s->key, key, 16); nvn_hm_put(&g_sh, k, s);
    if (!g_spv_dir[0]) { const char *d = getenv("DM_SPV_DIR"); snprintf(g_spv_dir, sizeof g_spv_dir, "%s", d ? d : ""); }
    char p[700]; snprintf(p, sizeof p, "%s/%s.res.json", g_spv_dir, s->key);
    FILE *f = fopen(p, "rb");
    if (!f) { static int n; if (n++ < 16) LOG("[RED] [vk] no translated shader %s (DM_SPV_DIR %s)", s->key, g_spv_dir); return NULL; }
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET); char *txt = calloc(1, (size_t)len + 1); fread(txt, 1, (size_t)len, f); fclose(f);
    jv r; js_parse(txt, &r); free(txt);
    const jv *st = js_get(&r, "stage"); const char *sn = st && st->s ? st->s : "vs";
    s->stage = !strcmp(sn, "ps") ? SH_PS : !strcmp(sn, "gs") ? SH_GS : !strcmp(sn, "cs") ? SH_CS : !strcmp(sn, "vs") ? SH_VS : !strcmp(sn, "ms") ? SH_MS : !strcmp(sn, "ts") ? SH_TK : SH_TS;
    s->vkstage = s->stage == SH_PS ? VK_SHADER_STAGE_FRAGMENT_BIT : s->stage == SH_GS ? VK_SHADER_STAGE_GEOMETRY_BIT : s->stage == SH_CS ? VK_SHADER_STAGE_COMPUTE_BIT
               : s->stage == SH_MS ? VK_SHADER_STAGE_MESH_BIT_EXT : s->stage == SH_TK ? VK_SHADER_STAGE_TASK_BIT_EXT : VK_SHADER_STAGE_VERTEX_BIT;
    s->set = js_int(&r, "set", s->stage == SH_PS ? 1 : s->stage == SH_GS || s->stage == SH_TK ? 2 : 0);
    s->io_prim = (uint32_t)js_int(&r, "prim_locs", 0);
    if ((s->stage == SH_MS || s->stage == SH_TK) && !g_mesh) {   // its compute form (gfx_vk_meshemu.inc): the kernel's own bindings at set 0 of a compute pipeline
        if (!memu_on() || !memu_facts(s, js_get(&r, "mesh_emu"))) { js_free(&r); RED_ONCE(161, "task / mesh shader %s on a GPU without VK_EXT_mesh_shader, and no compute form of it (DM_VK_MESH_EMU, res.json mesh_emu): its draws are skipped", s->key); return NULL; }
        s->vkstage = VK_SHADER_STAGE_COMPUTE_BIT; s->set = 0;
    }
    const jv *a = js_get(&r, "cbufs"); uint32_t rext[CB_MAX] = {0};   // rext: res.json's extents (cb_ext_pick)
    for (int i = 0; a && i < a->n && s->ncb < CB_MAX; i++) { rext[s->ncb] = (uint32_t)js_int(&a->kid[i], "ext", 0); s->cb[s->ncb++] = (rb_cbuf){js_int(&a->kid[i], "binding", 0), js_int(&a->kid[i], "slot", 0), 0}; }
    a = js_get(&r, "sbufs");
    for (int i = 0; a && i < a->n && s->nsb < 16; i++) { const jv *o = &a->kid[i]; s->sb[s->nsb++] = (rb_sbuf){js_int(o, "binding", 0), js_int(o, "slot", 0), js_int(o, "cb_slot", 0), js_int(o, "cb_offset", 0), js_int(o, "flags", 0)}; }
    for (int i = 0; i < s->nsb; i++) if (s->sb[i].cb_slot == DMSH_GMEM_MAP_CB_SLOT) s->gm = s->gm > 1 || s->sb[i].cb_off == DMSH_GMEM_LOG_CB_OFFSET ? 2 : 1;
    rb_tex_read(js_get(&r, "textures"), s->tx, &s->ntx, 64); rb_tex_read(js_get(&r, "images"), s->im, &s->nim, 16); rb_tex_f1d(js_get(&r, "fetch1d"), s->tx, s->ntx);
    s->upsq = (uint8_t)upsq_match(s);   // an upscaler network to replace: listed (1), or by its interface (2; gfx_vk_upsq.inc)
    // the texture and sampler pools read by ID (handle 0x40000000, the arrays as long as the ID ranges) are the pool set's, not the stage's (spv_pool.h); the first such shader
    // starts the pool set, so a game without one never makes it
    int haspool = 0; for (int i = 0; i < s->ntx; i++) haspool |= s->tx[i].handle == 0x40000000 && s->tx[i].cbuf_slot == 0 && s->tx[i].array > 32;
    if (haspool && pool_on()) {
        int k = 0; for (int i = 0; i < s->ntx; i++) { if (s->tx[i].handle == 0x40000000 && s->tx[i].cbuf_slot == 0 && s->tx[i].array > 32) s->pool = 1; else s->tx[k++] = s->tx[i]; }
        s->ntx = k;
    }
    memset(s->vin, 'f', sizeof s->vin);
    a = js_get(&r, "vin");
    for (int q = 0; a && a->t == 4 && q < a->n; q++) { int l = atoi(a->key[q]); if (l >= 0 && l < 16 && a->kid[q].s) s->vin[l] = a->kid[q].s[0]; }
    a = js_get(&r, "tf");   // [[outputs, stride] or [] per feedback buffer], the outputs [[generic output, components], ...] in their order in the record
    spv_tff_layout tfl; memset(&tfl, 0, sizeof tfl);
    for (int b = 0; a && a->t == 3 && b < a->n && b < 4; b++) if (a->kid[b].t == 3 && a->kid[b].n == 2) {
        s->tf_stride[b] = tfl.stride[b] = (uint32_t)a->kid[b].kid[1].num;
        const jv *ou = &a->kid[b].kid[0];
        for (int q = 0; ou->t == 3 && q < ou->n && tfl.nout[b] < SPV_TFF_MAX_OUT; q++) if (ou->kid[q].t == 3 && ou->kid[q].n == 2) {
            tfl.loc[b][tfl.nout[b]] = (uint8_t)ou->kid[q].kid[0].num; tfl.ncomp[b][tfl.nout[b]++] = (uint8_t)ou->kid[q].kid[1].num; } }
    int has_tf = (s->tf_stride[0] | s->tf_stride[1] | s->tf_stride[2] | s->tf_stride[3]) != 0; uint32_t tf_outs = 0, tf_ver = 0;
    js_free(&r);
    sh_bind_map(s);   // before its modules: they and the layout below take the same numbers
    t_pool_info = 0; t_harr_info = 0; t_lbase_info = 0;
    snprintf(p, sizeof p, s->memu ? "%s/%s.cs.spv" : "%s/%s.spv", g_spv_dir, s->key); s->mod = sh_module(s, p, &s->b0);   // (a task / mesh shader in compute form: its kernel, through the same rewrites and binding map)
    { size_t nw = 0; uint32_t *w = spv_read(p, &nw);
      if (w) { if (s->stage == SH_CS || s->memu) s->tgs = tgs_bytes(w, nw, s->key); if (has_tf && nw > 1) { tf_outs = spv_tff_outs(w, nw); tf_ver = w[1]; } spv_io(w, nw, &s->io_in, &s->io_out); ofw_note(s, w, nw); s->vid = s->stage == SH_VS && spv_vid(w, nw); s->wlayer = s->stage != SH_PS && s->stage != SH_CS && spv_layer_out(w, nw); uint32_t ext[CB_MAX]; (cb_ext2_on() ? spv_cb_ext2 : spv_cb_ext)(w, nw, s->cb, s->ncb, ext); cb_ext_pick(s, ext, rext); free(w); } }
    sh_bind_apply(s);   // the layout and stage_set's writes from here on: the numbers the modules have (the matching above read the file's)
    if (s->stage == SH_VS || (s->stage == SH_MS && !s->memu)) { snprintf(p, sizeof p, "%s/%s.z.spv", g_spv_dir, s->key); s->modz = sh_module(s, p, &s->b0); }
    if (t_lbase_info & 1) { s->lbpc = 1; s->lbase = s->stage == SH_VS && s->wlayer && !(t_lbase_info & 2); }   // its modules read the layer base (spv_fit_lbase)
    if (has_tf && s->stage == SH_VS && tf_frag()) {   // the fragment-stage capture; if it cannot be made, the .tf.spv below (which tf_capture refuses on such a GPU, RED 115)
        snprintf(p, sizeof p, "%s/%s.spv", g_spv_dir, s->key); s->modtf = lm_new_t(p, s->bmap, &s->b0);
        if (s->modtf) s->modtff = tff_fs_mod(s, &tfl, tf_outs, tf_ver);
        if (!s->modtff) s->modtf = NULL;   // (an lmod is never freed: its module may be in a pipeline being made)
    }
    if (has_tf && !s->modtf) { snprintf(p, sizeof p, "%s/%s.tf.spv", g_spv_dir, s->key); s->modtf = sh_module(s, p, &s->b0); }
    if (!s->mod) return NULL;
    // a pool read spv_pool left, or pool arrays res.json names that it found no variable for: their arrays would be in no layout
    if (s->pool && !(t_pool_info & (SPV_POOL_IMG | SPV_POOL_SMP)) && !(t_pool_info & SPV_POOL_FAIL)) { t_pool_info |= SPV_POOL_FAIL; static int nl; if (nl++ < 8) LOG("[RED] [vk] shader %s: res.json names texture pool arrays its module does not have as spv_pool knows them: its draws are dropped", s->key); }
    pool_note_shader(s, t_pool_info); if (t_pool_info & SPV_POOL_FAIL) return NULL;
    // arrays spv_harr shortened in every module of the shader: the layout as long as they are now (a module it left keeps the whole array, and so does the layout)
    if ((t_harr_info & SPV_HARR_SHRUNK) && !(t_harr_info & SPV_HARR_FAIL)) {
        for (int i = 0; i < s->ntx; i++) if (s->tx[i].array > TEX_ARR_MAX) s->tx[i].array = TEX_ARR_MAX;
        for (int i = 0; i < s->nim; i++) if (s->im[i].array > TEX_ARR_MAX) s->im[i].array = TEX_ARR_MAX;
    }
    // the set's layout: support buffer, constant buffers, storage buffers, textures, images
    VkDescriptorSetLayoutBinding b[1 + CB_MAX + 16 + 64 + 16 + 5]; uint32_t nb = 0;
    b[nb++] = (VkDescriptorSetLayoutBinding){0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, (VkShaderStageFlags)s->vkstage, NULL};
    sh_dyn(s);
    for (int i = 0; i < s->ncb; i++) b[nb++] = (VkDescriptorSetLayoutBinding){(uint32_t)s->cb[i].binding, s->cbdyn[i] != 0xff ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, (VkShaderStageFlags)s->vkstage, NULL};
    for (int i = 0; i < s->nsb; i++) b[nb++] = (VkDescriptorSetLayoutBinding){(uint32_t)s->sb[i].binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, (VkShaderStageFlags)s->vkstage, NULL};
    for (int i = 0; i < s->ntx; i++) b[nb++] = (VkDescriptorSetLayoutBinding){(uint32_t)s->tx[i].binding, tex_dtype(&s->tx[i], 0), (uint32_t)(s->tx[i].array > 1 ? s->tx[i].array : 1), (VkShaderStageFlags)s->vkstage, NULL};
    for (int i = 0; i < s->nim; i++) b[nb++] = (VkDescriptorSetLayoutBinding){(uint32_t)s->im[i].binding, tex_dtype(&s->im[i], 1), (uint32_t)(s->im[i].array > 1 ? s->im[i].array : 1), (VkShaderStageFlags)s->vkstage, NULL};
    if (s->tgs) b[nb++] = (VkDescriptorSetLayoutBinding){SPV_TGS_BINDING, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, (VkShaderStageFlags)s->vkstage, NULL};   // its shared memory (spv_tgspill)
    VkDescriptorSetLayoutCreateInfo lc = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = nb, .pBindings = b};
    if (vkCreateDescriptorSetLayout(g_vk.dev, &lc, NULL, &s->dsl) != VK_SUCCESS) { LOG("[RED] [vk] descriptor layout of shader %s failed", s->key); return NULL; }
    for (uint32_t i = 0; i < nb; i++) s->ndesc += b[i].descriptorCount > TEX_ARR_MAX ? TEX_ARR_MAX : b[i].descriptorCount;
    if (!ds_fits(b, nb)) { s->dsbig |= 1; g_ds_toobig++;
        uint32_t big = 0; for (uint32_t i = 0; i < nb; i++) if (b[i].descriptorCount > big) big = b[i].descriptorCount;
        static int n; if (n++ < 4) LOG("[RED] [vk] shader %s: one descriptor set of its layout is larger than a whole pool (an array of %u elements): its draws are dropped", s->key, big); }
    if (s->modtf) {   // the capture variant's set: the same bindings and the feedback buffers
        for (int q = 0; q < 4; q++) if (s->tf_stride[q]) b[nb++] = (VkDescriptorSetLayoutBinding){100u + (uint32_t)q, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, s->modtff ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT, NULL};
        lc.bindingCount = nb;
        if (vkCreateDescriptorSetLayout(g_vk.dev, &lc, NULL, &s->dsl_tf) != VK_SUCCESS) s->modtf = s->modtff = NULL;
        if (!ds_fits(b, nb)) s->dsbig |= 2;
    }
    if (s->memu && !memu_ready(s)) return NULL;   // a mesh shader's pass-through vertex shader
    s->ok = 1;
    if (vk_debug()) LOG("[vk] shader %s stage %d: %d constant buffers, %d storage buffers, %d textures, %d images%s%s", s->key, s->stage, s->ncb, s->nsb, s->ntx, s->nim, s->modz ? ", depth -1..1 variant" : "", s->b0 ? ", reads the support buffer" : "");
    return s;
}
static void key_hex(const uint8_t k[8], char out[17]) { static const char *hx = "0123456789abcdef"; for (int i = 0; i < 8; i++) { out[2 * i] = hx[k[i] >> 4]; out[2 * i + 1] = hx[k[i] & 15]; } out[16] = 0; }

// ---------------- The arena: host-visible memory for one submission's vertex, index, uniform and storage data ----------------
// The arena's memory type: host-visible and coherent, and cached where the driver has such a type, as staging is (stg_new). The completion thread reads the arena back: a
// storage buffer or buffer image a shader wrote is compared with its snapshot and its changed bytes copied to guest memory (chunk_done -> wb_apply, a byte at a time where
// a word differs), and every one of those reads from write-combined memory goes to DRAM. Qualcomm's driver on an Ayn Odin 2 offers the arena's buffers one host-visible
// type, cached and coherent (types 0 and 6: flags 0x1 and 0xf), so the arena was cached there all along. Mesa's Turnip lists a write-combined type first (types 0 to 2:
// 0x7, then 0xf), which the arena took: Pokémon Brilliant Diamond's write-backs held the completion thread about 37 ms a frame, the replay waited for free chunks (cur)
// and the game ran at 20 fps; 60 fps in the cached type, GPU time unchanged (5.9 against 6.1 ms). DM_VK_ARENA_CACHED=0 (env.txt): the first host-visible coherent type,
// as before (the A/B of this alone). A type's flags do not say how the CPU maps it: the Retroid Pocket 5's driver maps its cached coherent type uncached, and the write-backs
// are then read through a cached copy (gfx_vk_readback.inc)
static uint32_t ar_mem_type(uint32_t bits) {
    static int cached = -1, logged;
    if (cached < 0) { const char *e = getenv("DM_VK_ARENA_CACHED"); cached = !(e && *e == '0'); }
    uint32_t mt = cached ? vkb_mem_type(bits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT) : UINT32_MAX;
    if (mt == UINT32_MAX) mt = vkb_mem_type(bits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!logged++) LOG("[vk] arena: memory type %u (flags %#x, of the types %#x)%s", mt, mt < g_vk.mp.memoryTypeCount ? g_vk.mp.memoryTypes[mt].propertyFlags : 0, bits,
                       cached ? "" : " (DM_VK_ARENA_CACHED=0: the first host-visible coherent type)");
    return mt;
}
#define AR_USAGE (VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | \
                  VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT)   // also gfx_vk_readback.inc rb_speed
// the current chunk's (gfx_vk.c): blocks kept and reused when it completes
static uint64_t g_ar_bytes, g_ar_blocks;   // every chunk's arena blocks together (they are kept: the raw memory line reports them)
static int ar_new(VkDeviceSize n) {
    if (C->nar == C->car) { C->car = C->car ? 2 * C->car : 8; C->ar = realloc(C->ar, C->car * sizeof *C->ar); }
    arena_blk *a = &C->ar[C->nar]; memset(a, 0, sizeof *a);
    VkBufferCreateInfo bc = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n, .usage = AR_USAGE};
    if (vkCreateBuffer(g_vk.dev, &bc, NULL, &a->b) != VK_SUCCESS) return 0;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_vk.dev, a->b, &mr);
    uint32_t mt = ar_mem_type(mr.memoryTypeBits);
    VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mt};
    if (mt == UINT32_MAX || vkAllocateMemory(g_vk.dev, &ma, NULL, &a->m) != VK_SUCCESS) { vkDestroyBuffer(g_vk.dev, a->b, NULL); return 0; }
    vkBindBufferMemory(g_vk.dev, a->b, a->m, 0); vkMapMemory(g_vk.dev, a->m, 0, n, 0, (void **)&a->p); a->n = n; C->nar++; g_ar_bytes += n; g_ar_blocks++;
    return 1;
}
// n bytes aligned to 256 (the largest minimum uniform / storage offset alignment); the arena's blocks are reused after each submission completes
static uint8_t *ar_alloc(VkDeviceSize n, VkBuffer *b, VkDeviceSize *off) {
    n = n ? n : 16; if (!cur()) return NULL;
    for (;;) {
        if (C->ar_cur < C->nar) {
            arena_blk *a = &C->ar[C->ar_cur]; VkDeviceSize o = (a->used + 255) & ~(VkDeviceSize)255;
            if (o + n <= a->n) { a->used = o + n; *b = a->b; *off = o; C->ar_last = C->ar_cur; return a->p + o; }
            C->ar_cur++; continue;
        }
        if (!ar_new(n > (16u << 20) ? n : (16u << 20))) { RED_ONCE(80, "arena block of %llu bytes failed", (unsigned long long)n); return NULL; }
    }
}

// ---------------- Geometry kept on the GPU between frames ----------------
// A draw's indices and vertex streams were copied into the arena at every draw of every frame. Super Mario Bros. Wonder's world map with the camera far out draws 1,068 draws
// a frame reading 2.8 million indices and about 9 MB of vertices, nearly all of it the same from frame to frame: copying it (indices widened to 32 bits, their range scanned)
// was most of the render thread on an Adreno 619 (vk_draw, bind_vbufs and memcpy; 60 to 118 ms a frame). A range of guest memory is kept in a GPU buffer of its own once
// it was seen unchanged in a later frame, and used while the dirty-page tracker (nvn_dirty.c) shows no write to it since its stamp: the stamp is taken before the pages are
// protected, so any later write by the game or the host faults or pre-writes and raises the pages' generation. A range written twice is left to the per-draw copy (and no
// longer protected, so a buffer the game rewrites every frame costs no faults), and looked at again after a while. A kept copy's memory is reused only once the chunks that
// read it have completed (g_seq_done). DM_VK_GEOCACHE=0 turns it off
enum { GC_BLOCK = 32u << 20, GC_BUDGET = 256u << 20, GC_MAXN = 8u << 20, GC_NCLASS = 16, GC_RETRY = 600 };   // classes of 256 B << c up to 8 MB
typedef struct { VkBuffer b; VkDeviceMemory m; uint8_t *p; VkDeviceSize used; } gc_blk;
typedef struct gc_reg { int blk; VkDeviceSize off; uint64_t seq; struct gc_reg *next; } gc_reg;   // a free region: reused when g_seq_done reaches seq
typedef struct { uintptr_t src; uint64_t n; uint64_t kind; } gc_key;   // kind 0: vertex bytes as they are; 1 + 2 × index size (+ 32: restart index made 0xffffffff): indices as 32 bits
typedef struct { gc_key k; const void *span; uint64_t stamp, seq, seen, dyn_at; int state, dirty, blk, cls; VkDeviceSize off; uint32_t mn, mx; } gc_ent;   // state 0 watched, 1 kept, 2 left to the copy
static gc_blk g_gc_blk[GC_BUDGET / GC_BLOCK]; static int g_gc_nblk; static gc_reg *g_gc_free[GC_NCLASS];
static nvn_hmap g_gc = {.klen = sizeof(gc_key)};
static uint64_t g_gc_hit, g_gc_made, g_gc_copy, g_gc_bytes;   // draws served from a kept copy, copies kept, draws copied as before, bytes kept
static int gc_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_GEOCACHE"); v = !(e && *e == '0') && !vk_safe(); } return v; }
static int gc_class(uint64_t n) { int c = 0; while ((256ull << c) < n) c++; return c; }
static void gc_release(gc_ent *e) {   // its region back to its class's list, for when the chunks that read it have completed
    gc_reg *r = malloc(sizeof *r); if (!r) return;
    *r = (gc_reg){e->blk, e->off, e->seq, g_gc_free[e->cls]}; g_gc_free[e->cls] = r; g_gc_bytes -= 256ull << e->cls;
}
// The kept copies no draw used in the last few seconds are let go when a new one finds no room. The walk covers the whole table (tens of thousands of entries), so it is
// made at most once a present, and after one that freed nothing only every 60th: Mario Kart World's race asked 16 to 43 times a frame with the cache full, and the walks
// were 3 to 9 ms of the replay's frame on a Galaxy S25 Edge (gc_evict under gc_alloc). DM_VK_GC_EVICT_PACE=0 (env.txt): a walk at every request, as before
static uint64_t g_gc_evicts, g_gc_evicted, g_gc_evict_skips;   // walks made, copies they let go, requests that found a walk not due
static int gc_evict(void) {   // → 1 when it walked
    uint64_t now = atomic_load(&g_presents); static uint64_t next; static int pace = -1;
    if (pace < 0) { const char *e = getenv("DM_VK_GC_EVICT_PACE"); pace = !(e && *e == '0'); }
    if (pace && now < next) { g_gc_evict_skips++; return 0; }
    uint64_t n = 0;
    for (uint32_t i = 0; i < g_gc.cap; i++) { gc_ent *e = g_gc.vals[i]; if (e && e->state == 1 && e->seen + 120 < now) { gc_release(e); e->state = 0; e->dirty = 0; n++; } }
    g_gc_evicts++; g_gc_evicted += n; next = now + (n ? 1 : 60);
    return 1;
}
static uint8_t *gc_alloc(int cls, int *blk, VkDeviceSize *off) {
    uint64_t done = __atomic_load_n(&g_seq_done, __ATOMIC_ACQUIRE);
    for (gc_reg **pp = &g_gc_free[cls]; *pp; pp = &(*pp)->next) if ((*pp)->seq <= done) {
        gc_reg *r = *pp; *pp = r->next; *blk = r->blk; *off = r->off; free(r); return g_gc_blk[*blk].p + *off; }
    VkDeviceSize n = 256ull << cls;
    for (int pass = 0; pass < 2; pass++) {
        if (g_gc_nblk && g_gc_blk[g_gc_nblk - 1].used + n <= GC_BLOCK) { gc_blk *b = &g_gc_blk[g_gc_nblk - 1]; *blk = g_gc_nblk - 1; *off = b->used; b->used += n; return b->p + *off; }
        if (g_gc_nblk < (int)(GC_BUDGET / GC_BLOCK)) {
            gc_blk *b = &g_gc_blk[g_gc_nblk]; memset(b, 0, sizeof *b);
            VkBufferCreateInfo bc = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = GC_BLOCK, .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};   // storage: sb_keep
            if (vkCreateBuffer(g_vk.dev, &bc, NULL, &b->b) != VK_SUCCESS) return NULL;
            VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_vk.dev, b->b, &mr);
            uint32_t mt = vkb_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mt};
            if (mt == UINT32_MAX || vkAllocateMemory(g_vk.dev, &ma, NULL, &b->m) != VK_SUCCESS) { vkDestroyBuffer(g_vk.dev, b->b, NULL); RED_ONCE(119, "geometry cache block failed: draws copy their geometry as before"); return NULL; }
            vkBindBufferMemory(g_vk.dev, b->b, b->m, 0); vkMapMemory(g_vk.dev, b->m, 0, GC_BLOCK, 0, (void **)&b->p); g_gc_nblk++;
            continue;
        }
        if (pass == 0) { if (!gc_evict()) return NULL; for (gc_reg **pp = &g_gc_free[cls]; *pp; pp = &(*pp)->next) if ((*pp)->seq <= done) {
            gc_reg *r = *pp; *pp = r->next; *blk = r->blk; *off = r->off; free(r); return g_gc_blk[*blk].p + *off; } }
    }
    return NULL;
}
// the kept GPU copy of guest memory [src, src + n) as kind (out_n bytes on the GPU): *fresh when the caller fills it now (from src: unchanged since the stamp); NULL: copy it per draw
static gc_ent *gc_get(uintptr_t src, uint64_t n, uint64_t kind, uint64_t out_n, uint8_t **fill) {
    *fill = NULL;
    if (!gc_on() || !C || !src || !n || out_n > GC_MAXN) return NULL;
    gc_key k; memset(&k, 0, sizeof k); k.src = src; k.n = n; k.kind = kind;
    uint64_t now = atomic_load(&g_presents); gc_ent *e = nvn_hm_get(&g_gc, &k);
    if (!e) {
        if (g_gc.n >= 65536 || !(e = calloc(1, sizeof *e))) return NULL;
        e->k = k; e->span = nvn_dirty_span((const void *)src, n); e->seen = now;
        if (!e->span) e->state = 2; else { e->stamp = nvn_dirty_now(); nvn_dirty_protect((const void *)src, n); }   // untracked memory: always copied
        nvn_hm_put(&g_gc, &e->k, e); return NULL;
    }
    if (e->state == 2) {
        if (!e->span || now < e->dyn_at + GC_RETRY) return NULL;
        e->state = 0; e->dirty = 0; e->seen = now; e->stamp = nvn_dirty_now(); nvn_dirty_protect((const void *)src, n); return NULL;   // looked at again
    }
    if (nvn_dirty_since_span(e->span, (const void *)src, n, e->stamp)) {   // written since the stamp
        if (e->state == 1) { gc_release(e); e->state = 0; }
        if (++e->dirty >= 2) { e->state = 2; e->dyn_at = now; return NULL; }
        e->seen = now; e->stamp = nvn_dirty_now(); nvn_dirty_protect((const void *)src, n); return NULL;
    }
    if (e->state == 0) {   // kept once seen unchanged in a later frame than it was stamped in
        if (e->seen == now) return NULL;
        uint8_t *p = gc_alloc(gc_class(out_n), &e->blk, &e->off); if (!p) return NULL;
        e->cls = gc_class(out_n); e->state = 1; g_gc_made++; g_gc_bytes += 256ull << e->cls; *fill = p;
    }
    e->seen = now; e->seq = C->seq; g_gc_hit++;
    return e;
}
static VkBuffer gc_buf(const gc_ent *e) { return g_gc_blk[e->blk].b; }
// A storage buffer a shader only reads is kept the same way, as its raw bytes (kind 0, so a buffer also read as a vertex stream has one copy). It was copied into the arena once
// in every chunk that read it and compared with its shadow (ucopy) at every other draw of the chunk: Super Smash Bros. Ultimate's battles read about 44 storage buffers a frame
// (2.4 to 2.7 MB, tens of KB to hundreds each), the same bytes every frame, and copying and comparing them cost a Retroid Pocket 5's replay thread 29 µs a draw against 22 kept
// (about 1.2 ms a frame). Its pending writes were dealt with before (draw_view lands them, sb_fwd binds a dispatch's on the GPU), and a write since the stamp, the game's or the
// host's, sends it back to the copy as gc_get does. DM_VK_SB_KEEP=0 (env.txt): copied in every chunk, as before
static uint64_t g_gc_sb;   // storage buffer bindings served from a kept copy (also counted in g_gc_hit)
static int sb_keep_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_SB_KEEP"); v = !(e && *e == '0'); if (!v) LOG("[vk] DM_VK_SB_KEEP=0: read-only storage buffers are copied in every chunk"); } return v; }
static int sb_keep(uint64_t addr, size_t n, VkBuffer *b, VkDeviceSize *off) {   // → 1 with *b / *off the kept copy (filled now when new); 0: copy it (ucopy)
    if (!sb_keep_on()) return 0;
    uint8_t *fill = NULL; gc_ent *e = gc_get((uintptr_t)addr, n, 0, n, &fill); if (!e) return 0;
    if (fill) { size_t al = (n + 15) & ~(size_t)15; memcpy(fill, (const void *)(uintptr_t)addr, n); if (al > n) memset(fill + n, 0, al - n); }   // bound to the 16-byte range, as ucopy's
    *b = gc_buf(e); *off = e->off; g_gc_sb++; return 1;
}


// ---------------- Bound state ----------------
#define NTEX 256
static struct {
    const prog_t *bst[5];   // the program of each graphics stage (BindProgram binds per stage; nvn_mtl.m bind_stages)
    const prog_t *cprog;    // the compute program (stage bit 0x20 has a binding of its own)
    color_st color; chmask_st chmask; blend_st blend[8]; ms_st ms; poly_st poly; ds_st ds;
    uint32_t attr[16]; int nattr; vs_st stream[16]; int nstream; NVNbufferRange vbuf[16];
    p_bind_buf ubo[6][16], ssbo[6][16]; uint64_t tex[6][NTEX], samp[6][128], img[6][32];
    int tex_base, samp_base; uint32_t st_vmask[2], st_wmask[2], st_ref[2];
    p_poly_offset po; float vp[16][6]; int has_vp; float dr[2]; int depth_clamp; float blend_color[4]; p_prim_restart restart;
    p_tf_bind tf[4]; uint64_t tf_off[4]; int tf_on, rast_discard;
    uint8_t push[2][DMSH_PUSH_SIZE]; float alpha_ref;   // NVN2 push constants, graphics and compute blocks (read as c[DMSH_PUSH_BANK]); the alpha test's reference (spv_alpha.h)
} D;
static int g_d_init;
// The pipeline key's state (vk_draw_) as the key takes it from D: the fields a blend or depth / stencil object sets, over zeroed bits (the bits they leave over are whatever
// the game's object held). o is zeroed by the caller
static void blend_k(blend_st *o, const blend_st *b) { o->target = b->target; o->src_rgb = b->src_rgb; o->dst_rgb = b->dst_rgb; o->src_a = b->src_a;
    o->dst_a = b->dst_a; o->eq_rgb = b->eq_rgb; o->eq_a = b->eq_a; o->adv = b->adv; o->adv_npm = b->adv_npm; o->adv_ovl = b->adv_ovl; }
static void ds_k(ds_st *o, const ds_st *d) { o->dt = d->dt; o->dw = d->dw; o->df = d->df; o->st = d->st; o->sf_f = d->sf_f; o->sf_b = d->sf_b;
    o->fail_f = d->fail_f; o->zfail_f = d->zfail_f; o->zpass_f = d->zpass_f; o->fail_b = d->fail_b; o->zfail_b = d->zfail_b; o->zpass_b = d->zpass_b; }
// g_pso_gen: bumped when a value the pipeline key takes from D really changes (draw_state_op compares before it stores; a state object sent again with the same
// fields leaves it), so a draw with the generation, shaders, pass, primitive and origin of the draw before has its pipeline without building and hashing the key
// (vk_draw_ pso_memo). Any new input of the key read from D must bump it here (or join pso_memo's key)
static int pso_memo_on(void);
static uint64_t g_pso_gen = 1;
static void draw_state_reset(void) {
    memset(&D, 0, sizeof D); g_d_init = 1; g_pso_gen++;
    for (int i = 0; i < 8; i++) D.blend[i] = (blend_st){(uint64_t)i, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD};
    D.color = (color_st){0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS}; D.chmask.mask = 0xffffffffu;
    D.poly = (poly_st){NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0};
    D.ds = (ds_st){0, 1, NVN_DEPTH_FUNC_LESS, 0, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS, 1, 1, 1, 1, 1, 1};
    D.st_vmask[0] = D.st_vmask[1] = D.st_wmask[0] = D.st_wmask[1] = 0xff; D.dr[1] = 1;
}
int nvn_dpool_base(const void *pool, int tex);   // hle_nvn.c
// The texture and sampler pools' entries, as the game registers them (IDs include each pool's base in the host table)
typedef struct { tex_t t; view_t v; int has_v, ok; } treg_t;
static treg_t *g_treg; static int g_ntreg; static samp_par *g_sreg; static uint8_t *g_sreg_ok; static int g_nsreg;
static uint64_t *g_sreg_ver, g_sreg_gen;   // when each sampler ID was last registered (the pool set takes those registered since: gfx_vk_pool.inc pool_sync)
static VkSampler (*g_scache)[2];   // each sampler ID's VkSampler (no shadow / shadow) once made for its registration, emptied when it is registered again (stage_set, DM_VK_TEX_ONCE)
static pthread_mutex_t g_reg_mu = PTHREAD_MUTEX_INITIALIZER;
static void reg_tex(int id, const tex_t *t, const view_t *v) {
    if (id < 0) return;
    pthread_mutex_lock(&g_reg_mu);
    if (id >= g_ntreg) { int n = id + 1024; g_treg = realloc(g_treg, n * sizeof *g_treg); memset(g_treg + g_ntreg, 0, (n - g_ntreg) * sizeof *g_treg); g_ntreg = n; }
    treg_t *r = &g_treg[id]; if (t) { r->t = *t; r->ok = 1; } else r->ok = 0;
    r->has_v = v != NULL; if (v) r->v = *v;
    pthread_mutex_unlock(&g_reg_mu);
}
static void reg_samp(int id, const samp_par *p) {
    if (id < 0) return;
    pthread_mutex_lock(&g_reg_mu);
    if (id >= g_nsreg) { int n = id + 1024; g_sreg = realloc(g_sreg, n * sizeof *g_sreg); g_sreg_ok = realloc(g_sreg_ok, n); memset(g_sreg_ok + g_nsreg, 0, n - g_nsreg);
        g_scache = realloc(g_scache, n * sizeof *g_scache); if (g_scache) memset(g_scache + g_nsreg, 0, (n - g_nsreg) * sizeof *g_scache);
        g_sreg_ver = realloc(g_sreg_ver, n * sizeof *g_sreg_ver); memset(g_sreg_ver + g_nsreg, 0, (n - g_nsreg) * sizeof *g_sreg_ver); g_nsreg = n; }
    if (p) { g_sreg[id] = *p; g_sreg_ok[id] = 1; } else g_sreg_ok[id] = 0;
    g_sreg_ver[id] = ++g_sreg_gen;
    if (g_scache) g_scache[id][0] = g_scache[id][1] = VK_NULL_HANDLE;
    pthread_mutex_unlock(&g_reg_mu);
}

static int draw_state_is(uint32_t op) {
    switch (op) {
        case OP_BIND_PROGRAM: case OP_BIND_STATE: case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF: case OP_BIND_VBUF1: case OP_BIND_UBO: case OP_BIND_UBO_SMALL:
        case OP_BIND_SSBO: case OP_BIND_TEX: case OP_BIND_TEX1: case OP_BIND_SAMP: case OP_BIND_IMG: case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: case OP_STENCIL:
        case OP_POLY_OFFSET: case OP_VIEWPORTS: case OP_DEPTH_RANGE: case OP_DEPTH_CLAMP: case OP_BLEND_COLOR: case OP_PRIM_RESTART:
        case OP_TF_BIND: case OP_TF_STATE: case OP_RAST_DISCARD: case OP_PUSH_CONST: case OP_ALPHA_REF: return 1;
        default: return 0;
    }
}
// state records (nvn_mtl.m state_op): 1 when the op was one
static int draw_state_op(uint32_t op, const void *a) {
    if (!g_d_init) draw_state_reset();   // NVN's defaults, once (the state carries over between submissions, as in nvn_mtl.m)
    switch (op) {
        case OP_BIND_PROGRAM: { const p_bind_program *b = a; int bits = (b->stages & 0x1f) | (b->stages & 0x40 ? 8 : 0) | (b->stages & 0x80 ? 16 : 0);   // NVN2's task and mesh bits: stages 3 and 4 (nvn_mtl.m gfx_stage_bits)
            for (int st = 0; st < 5; st++) if (bits >> st & 1) D.bst[st] = b->prog; if (b->stages & 0x20) D.cprog = b->prog; return 1; }
        case OP_BIND_STATE: { const p_bind_state *b = a; blend_st bs;
            if (pso_memo_on() && b->kind >= 0 && b->kind <= 5) {   // g_pso_gen: the fields the key takes (vk_draw_), before they are stored (colour's alpha test and multisample's
                int ch = 0;                                         // fields but alpha-to-coverage are not in the key)
                switch (b->kind) {
                    case 0: { blend_st n, o; memcpy(&bs, b->bytes, sizeof bs); memset(&n, 0, sizeof n); memset(&o, 0, sizeof o); blend_k(&n, &bs); blend_k(&o, &D.blend[bs.target & 7]); ch = memcmp(&n, &o, sizeof n) != 0; break; }
                    case 1: { color_st c; memcpy(&c, b->bytes, sizeof c); ch = c.blend_en != D.color.blend_en || c.logic_op != D.color.logic_op || c.alpha_func != D.color.alpha_func; break; }
                    case 2: ch = memcmp(&D.chmask, b->bytes, sizeof D.chmask) != 0; break;
                    case 3: { ms_st m; memcpy(&m, b->bytes, sizeof m); ch = m.a2c != D.ms.a2c; break; }
                    case 4: { poly_st p; memcpy(&p, b->bytes, sizeof p); ch = p.cull != D.poly.cull || p.front != D.poly.front || p.mode != D.poly.mode || p.offset_en != D.poly.offset_en; break; }
                    case 5: { ds_st d, n, o; memcpy(&d, b->bytes, sizeof d); memset(&n, 0, sizeof n); memset(&o, 0, sizeof o); ds_k(&n, &d); ds_k(&o, &D.ds); ch = memcmp(&n, &o, sizeof n) != 0; break; }
                }
                if (ch) g_pso_gen++;
            }
            switch (b->kind) { case 0: memcpy(&bs, b->bytes, sizeof bs); D.blend[bs.target & 7] = bs; break; case 1: memcpy(&D.color, b->bytes, sizeof D.color); break;
                case 2: memcpy(&D.chmask, b->bytes, sizeof D.chmask); break; case 3: memcpy(&D.ms, b->bytes, sizeof D.ms); break;
                case 4: memcpy(&D.poly, b->bytes, sizeof D.poly); break; case 5: memcpy(&D.ds, b->bytes, sizeof D.ds); break; }
            return 1; }
        // the whole attribute / stream state is replaced (as nvn_mtl.m state_op): a slot past the count is unused, never a stale one read from a freed buffer
        case OP_BIND_VATTR: { const p_bind_vattr *b = a; int n = b->n < 0 ? 0 : b->n > 16 ? 16 : b->n;
            if (pso_memo_on()) { uint32_t na[16]; memset(na, 0, sizeof na); memcpy(na, b->st, 4 * (size_t)n); if (memcmp(na, D.attr, sizeof na)) g_pso_gen++; }
            memset(D.attr, 0, sizeof D.attr); memcpy(D.attr, b->st, 4 * (size_t)n); D.nattr = n; return 1; }
        case OP_BIND_VSTREAM: { const p_bind_vstream *b = a; int n = b->n < 0 ? 0 : b->n > 16 ? 16 : b->n;
            if (pso_memo_on()) { vs_st ns[16]; memset(ns, 0, sizeof ns); memcpy(ns, b->st, sizeof *b->st * (size_t)n); if (memcmp(ns, D.stream, sizeof ns)) g_pso_gen++; }
            memset(D.stream, 0, sizeof D.stream); memcpy(D.stream, b->st, sizeof *b->st * (size_t)n); D.nstream = n; return 1; }
        case OP_BIND_VBUF: case OP_BIND_VBUF1: { p_bind_vbuf tmp; const p_bind_vbuf *b = vbuf_rec(op, a, &tmp); for (int i = 0; i < b->n && b->first + i < 16; i++) D.vbuf[b->first + i] = b->r[i]; return 1; }
        case OP_BIND_UBO: case OP_BIND_UBO_SMALL: { p_bind_buf tmp; const p_bind_buf *b = bind_buf_rec(op, a, &tmp); if (b->stage < 6 && b->index < 16) D.ubo[b->stage][b->index] = *b; return 1; }
        case OP_BIND_SSBO: { const p_bind_buf *b = a; if (b->stage < 6 && b->index < 16) D.ssbo[b->stage][b->index] = *b; return 1; }
        case OP_BIND_TEX: case OP_BIND_TEX1: { p_bind_handles t1; const p_bind_handles *b = bind_rec(op, a, &t1);
            if (b->n < 0 || b->n > 32 || b->stage >= 6 || b->first < 0 || b->first + b->n > NTEX) return 1;
            for (int k = 0; k < b->n; k++) D.tex[b->stage][b->first + k] = b->defer && b->src ? *(const uint64_t *)(uintptr_t)(b->src + 8u * k) : b->h[k];
            return 1; }
        case OP_BIND_SAMP: { const p_bind_handles *b = a; if (b->stage < 6 && b->first >= 0 && b->first + b->n <= 128) for (int k = 0; k < b->n; k++) D.samp[b->stage][b->first + k] = b->defer && b->src ? *(const uint64_t *)(uintptr_t)(b->src + 8u * k) : b->h[k]; return 1; }
        case OP_BIND_IMG: { const p_bind_handles *b = a; if (b->stage < 6 && b->first >= 0 && b->first + b->n <= 32) for (int k = 0; k < b->n; k++) D.img[b->stage][b->first + k] = b->h[k]; return 1; }
        case OP_SET_TEX_POOL: D.tex_base = nvn_dpool_base(((const p_set_pool *)a)->pool, 1); return 1;
        case OP_SET_SAMP_POOL: D.samp_base = nvn_dpool_base(((const p_set_pool *)a)->pool, 0); return 1;
        case OP_STENCIL: { const p_stencil *b = a; for (int f = 0; f < 2; f++) if (b->face & (1 << f)) { if (b->which == 0) D.st_vmask[f] = (uint32_t)b->value_mask; else if (b->which == 1) D.st_wmask[f] = (uint32_t)b->mask; else D.st_ref[f] = (uint32_t)b->ref; } return 1; }
        case OP_POLY_OFFSET: D.po = *(const p_poly_offset *)a; return 1;
        case OP_VIEWPORTS: { const p_viewports *b = a; for (int i = 0; i < b->n && b->first + i < 16; i++) memcpy(D.vp[b->first + i], b->v[i], sizeof D.vp[0]); D.has_vp = 1; return 1; }
        case OP_DEPTH_RANGE: { const p_depth_range *b = a; D.dr[0] = b->n; D.dr[1] = b->f; return 1; }
        case OP_DEPTH_CLAMP: if (D.depth_clamp != ((const p_depth_clamp *)a)->on) g_pso_gen++; D.depth_clamp = ((const p_depth_clamp *)a)->on; return 1;
        case OP_BLEND_COLOR: memcpy(D.blend_color, ((const p_blend_color *)a)->rgba, 16); return 1;
        case OP_PRIM_RESTART: if (D.restart.on != ((const p_prim_restart *)a)->on) g_pso_gen++; D.restart = *(const p_prim_restart *)a; return 1;   // the key takes .on (the index is the type's all-ones)
        case OP_TF_BIND: { const p_tf_bind *b = a; if (b->index >= 0 && b->index < 4) D.tf[b->index] = *b; return 1; }
        case OP_TF_STATE: { const p_tf_state *b = a;   // Begin writes from the start of each buffer; Pause / Resume keep the offsets (nvn_mtl.m: the game's control buffer is not used)
            if (b->what == 0) { D.tf_on = 1; memset(D.tf_off, 0, sizeof D.tf_off); } else D.tf_on = b->what == 3; return 1; }
        case OP_RAST_DISCARD: D.rast_discard = ((const p_rast_discard *)a)->on; return 1;
        case OP_PUSH_CONST: { const p_push_const *b = a;   // the bytes at offset in one block, as nvn_mtl.m state_op; stage_set reads the block as c[DMSH_PUSH_BANK]
            if (b->offset < DMSH_PUSH_SIZE && b->size <= DMSH_PUSH_SIZE - b->offset) memcpy(D.push[b->compute ? 1 : 0] + b->offset, b->data, b->size);
            else RED_ONCE(132, "push constants at %u, %u bytes, past the %u-byte block: dropped", b->offset, b->size, (unsigned)DMSH_PUSH_SIZE);
            return 1; }
        case OP_ALPHA_REF: D.alpha_ref = ((const p_alpha_ref *)a)->ref; return 1;   // the alpha test's reference: a push constant set at each draw (spv_alpha.h), not part of the key
        default: return 0;
    }
}

// ---------------- Image views, samplers, dummies ----------------
typedef struct { VkImage img; VkImageViewType type; VkFormat fmt; uint32_t base_level, nlevels, base_layer, nlayers; VkImageAspectFlags aspect; uint8_t swz[4]; } view_key;
static int g_view_slice;   // view_get: a render target's view (rp_start): a 3D image's slice as a 2D view, not the whole volume (Bowser's Fury draws its colour-grading LUT a slice at a time)
static VkFormat g_view_fmt;   // view_get: a format other than the image's (a storage view in a shader's declared format; the image is mutable), 0 = the image's
static nvn_hmap g_views = {.klen = sizeof(view_key)};
static VkFormat view_format(const vtex *v, const view_t *vv);   // below: a texture view's own format (0: the image's)
static VkImageView view_get(const vtex *v, VkImageViewType type, uint32_t level, uint32_t nlevels, uint32_t layer, uint32_t nlayers, VkImageAspectFlags aspect, const uint8_t swz[4]) {
    view_key k; memset(&k, 0, sizeof k);
    if (level >= v->levels) level = v->levels - 1;
    if (g_view_slice && v->type == VK_IMAGE_TYPE_3D) {   // a render target's slice of a 3D image (layer = the depth slice): a 2D view of it, which the image is 2D_ARRAY_COMPATIBLE for (gfx_vk.c vt_new);
        uint32_t dz = v->d >> level ? v->d >> level : 1; if (layer >= dz) layer = 0;   // nlayers > 1 (a layered pass, rp_prepare): those slices as a 2D array
        if (!nlayers || layer + nlayers > dz) nlayers = dz - layer;
        type = nlayers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D; nlevels = 1; goto make;
    }
    if (!nlevels || level + nlevels > v->levels) nlevels = v->levels - level;
    if (layer >= v->layers) layer = 0;
    if (!nlayers || layer + nlayers > v->layers) nlayers = v->layers - layer;
    if (type == VK_IMAGE_VIEW_TYPE_CUBE && (nlayers < 6 || !(v->layers % 6 == 0))) type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    if (type == VK_IMAGE_VIEW_TYPE_CUBE) nlayers = 6;
    if (type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) nlayers -= nlayers % 6;
    if ((type == VK_IMAGE_VIEW_TYPE_2D || type == VK_IMAGE_VIEW_TYPE_1D) && nlayers > 1) nlayers = 1;
    if (v->type == VK_IMAGE_TYPE_3D) type = VK_IMAGE_VIEW_TYPE_3D;
    else if (type == VK_IMAGE_VIEW_TYPE_3D) type = VK_IMAGE_VIEW_TYPE_2D;
    if (v->type == VK_IMAGE_TYPE_1D && type != VK_IMAGE_VIEW_TYPE_1D_ARRAY) type = VK_IMAGE_VIEW_TYPE_1D;
    if (v->type == VK_IMAGE_TYPE_2D && (type == VK_IMAGE_VIEW_TYPE_1D || type == VK_IMAGE_VIEW_TYPE_1D_ARRAY)) type = nlayers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
make:
    k.img = v->img; k.type = type; k.fmt = g_view_fmt ? g_view_fmt : v->fmt; k.base_level = level; k.nlevels = nlevels; k.base_layer = layer; k.nlayers = nlayers; k.aspect = aspect;
    if (swz) memcpy(k.swz, swz, 4);
    VkImageView iv = nvn_hm_get(&g_views, &k); if (iv) return iv;
    static const VkComponentSwizzle S_[6] = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A};
    VkComponentMapping cm = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    if (swz) { cm.r = swz[0] < 6 ? S_[swz[0]] : cm.r; cm.g = swz[1] < 6 ? S_[swz[1]] : cm.g; cm.b = swz[2] < 6 ? S_[swz[2]] : cm.b; cm.a = swz[3] < 6 ? S_[swz[3]] : cm.a; }
    VkImageViewCreateInfo ic = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = v->img, .viewType = type, .format = k.fmt, .components = cm,
                                .subresourceRange = {aspect, level, nlevels, layer, nlayers}};
    // an image with STORAGE for views of another format (ext_store, vt_storage_up): a view would otherwise take the image's usage, and one in its own format would claim a
    // storage use that format does not have. Each takes what its format supports, so the format (in the key) decides the usage
    VkImageViewUsageCreateInfo vu = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
    if (v->ext_store) {
        VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(g_vk.pd, k.fmt, &fp); VkFormatFeatureFlags ff = fp.optimalTilingFeatures;
        vu.usage = v->usage & (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | (ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT ? VK_IMAGE_USAGE_SAMPLED_BIT : 0) |
                               (ff & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT ? VK_IMAGE_USAGE_STORAGE_BIT : 0) | (ff & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0));
        if (vu.usage) ic.pNext = &vu;
    }
    if (vkCreateImageView(g_vk.dev, &ic, NULL, &iv) != VK_SUCCESS) { RED_ONCE(81, "image view of format %d type %d failed", v->fmt, type); return VK_NULL_HANDLE; }
    nvn_hm_put(&g_views, &k, iv);
    return iv;
}
// the views (and framebuffers) of an image that goes: destroyed with it
static nvn_hmap g_fbs;
static void bcrt_drop(VkImage base);   // below, with the render passes
static void lsnap_drop_img(VkImage img);   // below, with the render passes
static void views_drop(VkImage img) {
    bcrt_drop(img); lsnap_drop_img(img);
    for (uint32_t i = 0; i < g_views.cap; i++) {
        if (!g_views.vals[i]) continue;
        view_key *k = (view_key *)(void *)(g_views.keys + (size_t)i * g_views.klen);
        if (k->img != img) continue;
        VkImageView iv = g_views.vals[i];
        for (uint32_t j = 0; j < g_fbs.cap; j++) {   // framebuffers holding the view
            if (!g_fbs.vals[j]) continue;
            const VkImageView *fv = (const VkImageView *)(void *)(g_fbs.keys + (size_t)j * g_fbs.klen);
            int hit = 0; for (int q = 0; q < 9; q++) hit |= fv[q] == iv;
            if (hit) { uint8_t fk[9 * sizeof(VkImageView) + 12]; memcpy(fk, fv, sizeof fk); dead_add(2, (uint64_t)(VkFramebuffer)g_fbs.vals[j]); nvn_hm_del(&g_fbs, fk); j = (uint32_t)-1; }
        }
        pool_drop_view(iv); dead_add(1, (uint64_t)iv); view_key kk = *k; nvn_hm_del(&g_views, &kk); i = (uint32_t)-1;
    }
}
static nvn_hmap g_samps = {.klen = sizeof(samp_par) + 4};
static VkSampler samp_get(const samp_par *p, int shadow) {
    struct { samp_par p; int shadow; } k; memset(&k, 0, sizeof k); if (p) k.p = *p; k.shadow = shadow;
    VkSampler s = nvn_hm_get(&g_samps, &k); if (s) return s;
    static const VkSamplerAddressMode W[8] = {VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
        VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    int minf = p ? p->minf : 1, magf = p ? p->magf : 1;
    VkSamplerCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = magf ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .minFilter = minf == 1 || minf == 3 || minf == 5 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .mipmapMode = minf == 4 || minf == 5 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = W[p ? p->ws & 7 : 7], .addressModeV = W[p ? p->wt & 7 : 7], .addressModeW = W[p ? p->wr & 7 : 7],
        .mipLodBias = p ? p->lod_bias : 0, .minLod = p ? p->lod_min : 0, .maxLod = minf >= 2 ? (p ? p->lod_max : 1000.f) : 0.25f,
        .compareEnable = shadow || (p && p->cmp_mode), .compareOp = p && p->cmp_func ? (VkCompareOp)(p->cmp_func - 1) : VK_COMPARE_OP_LESS_OR_EQUAL};
    if (p && p->aniso > 1.f) { ci.anisotropyEnable = VK_TRUE; ci.maxAnisotropy = p->aniso > g_vk.props.limits.maxSamplerAnisotropy ? g_vk.props.limits.maxSamplerAnisotropy : p->aniso; }
    if (p) { float a = p->border[3], l = p->border[0] + p->border[1] + p->border[2];   // border colours: Vulkan without VK_EXT_custom_border_color has three
        ci.borderColor = a < 0.5f ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK : l > 1.5f ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK; }
    if (vkCreateSampler(g_vk.dev, &ci, NULL, &s) != VK_SUCCESS) return VK_NULL_HANDLE;
    nvn_hm_put(&g_samps, &k, s);
    return s;
}
// a 1×1 (×6 layers) zero texture for each view kind: unbound and unresolvable slots read 0, as on the Switch
static vtex *g_dummy;
static VkImageView dummy_view(VkImageViewType type) {
    if (!g_dummy) {
        tex_t t; memset(&t, 0, sizeof t); t.magic = MAGIC('T', 'E', 'X', 0); t.serial = ~0ull; t.d.target = NVN_TEXTURE_TARGET_CUBEMAP_ARRAY; t.d.format = NVN_FORMAT_RGBA8;
        t.d.width = t.d.height = 1; t.d.depth = 6; t.d.levels = 1;
        if (!(g_dummy = vt_create(&t))) return VK_NULL_HANDLE;
    }
    if (type == VK_IMAGE_VIEW_TYPE_3D) type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;   // a 3D slot gets a 2D array view: not a match, reported once
    return view_get(g_dummy, type, 0, 1, 0, type == VK_IMAGE_VIEW_TYPE_CUBE ? 6 : type == VK_IMAGE_VIEW_TYPE_2D_ARRAY || type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY ? 6 : 1, VK_IMAGE_ASPECT_COLOR_BIT, NULL);
}
static VkImageViewType view_type_of(int st) {   // Ryujinx's SamplerType
    int arr = (st & 0x100) != 0;
    switch (st & 0xff) {
        case 1: return arr ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
        case 4: return VK_IMAGE_VIEW_TYPE_3D;
        case 5: return arr ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE;
        default: return arr ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    }
}
#include "gfx_vk_pool.inc"   // NVN2's texture and sampler pools read by ID: the pool set
// The module with its texture, sampler and image arrays longer than stage_set writes shortened to TEX_ARR_MAX (spv_harr.h), in *out (to free); 0 when it has none or
// they cannot be (t_harr_info says which). Why: a layout must hold as many descriptors as the module's array is long, and Paper Mario: The Thousand-Year Door's
// reflection shaders, whose handle array from a uniform buffer is 8,192 long, filled a descriptor pool every two sets: 539 pools, 6.7 GB on a Galaxy S25 Edge,
// before the low-memory killer ended the game in the prologue. DM_VK_HARR=0 (env.txt): the arrays as the translator declared them, as before
static uint64_t g_harr_shaders, g_harr_left;   // modules shortened; modules with such an array left as they are (SPV_HARR_FAIL)
static size_t spv_fit_harr(const uint32_t *code, size_t nw, const char *what, uint32_t **out) {
    *out = NULL;
    static int on = -1; if (on < 0) { const char *e = getenv("DM_VK_HARR"); on = !(e && *e == '0'); if (!on) LOG("[vk] DM_VK_HARR=0: descriptor arrays as long as the translator declared them"); }
    if (!on || nw < 5) return 0;
    uint32_t *s = malloc((size_t)SPV_HARR_SCRATCH * code[3] * 4); if (!s) return 0;
    size_t need = 0, k = 0; int info = 0; uint32_t *o = NULL;
    k = spv_harr(code, nw, POOL_SET, TEX_ARR_MAX, s, NULL, 0, &need, &info);
    if (need && (o = malloc(need * 4))) k = spv_harr(code, nw, POOL_SET, TEX_ARR_MAX, s, o, need, &need, &info);
    free(s);
    t_harr_info |= info;
    if (info & SPV_HARR_FAIL) { g_harr_left++; static int nl; if (nl++ < 8) LOG("[RED] [vk] %s uses a descriptor array longer than %d in a way spv_harr does not rewrite: its layout keeps the whole array", what, TEX_ARR_MAX); }
    if (!k) { free(o); return 0; }
    if (!g_harr_shaders++) LOG("[vk] %s: descriptor arrays longer than %d shortened to the %d elements written (spv_harr.h; DM_VK_HARR=0: off)", what, TEX_ARR_MAX, TEX_ARR_MAX);
    *out = o; return k;
}
#include "gfx_vk_gmem.inc"   // raw global memory: the memory pools at device addresses and the raw pool table (DM_VK_RAW_GMEM)
#include "gfx_vk_fileback.inc"   // memory pools backed by a file (DM_VK_POOL_FILEBACK)

// ---------------- Render passes and framebuffers ----------------
typedef struct { VkFormat color[8]; VkFormat depth; uint32_t ncolor, samples; } rp_key;
static nvn_hmap g_rps = {.klen = sizeof(rp_key)}, g_fbs = {.klen = 9 * sizeof(VkImageView) + 12};
static VkRenderPass rp_get(const rp_key *k) {
    VkRenderPass rp = nvn_hm_get(&g_rps, k); if (rp) return rp;
    VkAttachmentDescription at[9]; VkAttachmentReference cr[8], dr; uint32_t na = 0;
    for (uint32_t i = 0; i < k->ncolor; i++) {
        if (!k->color[i]) { cr[i] = (VkAttachmentReference){VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_GENERAL}; continue; }
        at[na] = (VkAttachmentDescription){0, k->color[i], (VkSampleCountFlagBits)k->samples, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                           VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL};
        cr[i] = (VkAttachmentReference){na++, VK_IMAGE_LAYOUT_GENERAL};
    }
    if (k->depth) {
        at[na] = (VkAttachmentDescription){0, k->depth, (VkSampleCountFlagBits)k->samples, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_LOAD,
                                           VK_ATTACHMENT_STORE_OP_STORE, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL};
        dr = (VkAttachmentReference){na++, VK_IMAGE_LAYOUT_GENERAL};
    }
    VkSubpassDescription sp = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = k->ncolor, .pColorAttachments = cr, .pDepthStencilAttachment = k->depth ? &dr : NULL};
    VkSubpassDependency dep[2] = {{VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 0},
                                  {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 0}};
    VkRenderPassCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = na, .pAttachments = at, .subpassCount = 1, .pSubpasses = &sp, .dependencyCount = 2, .pDependencies = dep};
    if (vkCreateRenderPass(g_vk.dev, &ci, NULL, &rp) != VK_SUCCESS) { RED_ONCE(82, "render pass creation failed"); return VK_NULL_HANDLE; }
    nvn_hm_put(&g_rps, k, rp);
    return rp;
}
static struct { int active, layered; VkRenderPass rp; rp_key key; uint32_t w, h, layers; VkFramebuffer fb; vtex *cv[8], *dv; uint32_t clv[8], cly[8];
                uint8_t *snap[8]; size_t snap_cap[8], snap_n[8]; uint8_t *snap_dst[8]; VkImage snap_img[8]; } P;   // snap: a linear target's guest bytes at rp_prepare (lsnap_take), until its write-back takes them
static uint64_t g_passes;
// DM_VK_DUMP=<frame>: every render pass of that frame (counted in presents from 1) has its colour targets copied out when it ends, written to
// $DM_DUMP_DIR/f<frame>_p<pass>_a<attachment>_<w>x<h>_vk<format>_s<serial>.raw once the frame is presented, with a log line per draw (shaders, viewport, textures)
typedef struct { stg_t s; uint32_t w, h; VkFormat f; int pass, att; uint64_t ser; } dump_t;   // att < 0: a texture the pass's draws read
static int dump_bytes(VkFormat f) {   // the texel size of what the dump copies: fmt_bytes', and the 16-bit packed formats (a movie's RGB565 picture)
    int b = fmt_bytes(f); if (b) return b;
    switch (f) { case VK_FORMAT_R5G6B5_UNORM_PACK16: case VK_FORMAT_B5G6R5_UNORM_PACK16: case VK_FORMAT_R4G4B4A4_UNORM_PACK16: case VK_FORMAT_A1R5G5B5_UNORM_PACK16: case VK_FORMAT_R5G5B5A1_UNORM_PACK16: return 2; default: return 0; }
}
static uint64_t g_dump_tex[32]; static int g_ndump_tex;   // the textures this pass's draws read (dump_note_tex), copied at its end with the targets
static void dump_note_tex(uint64_t ser) { for (int i = 0; i < g_ndump_tex; i++) if (g_dump_tex[i] == ser) return; if (g_ndump_tex < 32) g_dump_tex[g_ndump_tex++] = ser; }
static dump_t g_dumps[1024]; static int g_ndump, g_dump_pass;
// or a file named dump_now in $DM_DUMP_DIR's parent: the frame two presents after it is seen (checked every 30 presents; the file is removed)
static long g_dump_at = -2;
static long dump_frame(void) { if (g_dump_at == -2) { const char *e = getenv("DM_VK_DUMP"); g_dump_at = e ? atol(e) : -1; } return g_dump_at; }
static void dump_poll(uint64_t presents) {
    const char *dir = getenv("DM_DUMP_DIR"); if (!dir || presents % 30) return;
    char p[700]; snprintf(p, sizeof p, "%s/../dump_now", dir);
    if (!access(p, F_OK)) { unlink(p); mkdir(dir, 0777); dump_frame(); g_dump_at = (long)presents + 2; LOG("[dump] dump_now seen: frame %ld is dumped", g_dump_at); }
}
static int dump_on(void) { long f = dump_frame(); return f > 0 && (long)atomic_load(&g_presents) + 1 == f; }
static void dump_pass_end(void) {
    g_dump_pass++;
    for (int i = 0; i < 8; i++) {
        vtex *v = P.cv[i]; if (!v || g_ndump >= (int)(sizeof g_dumps / sizeof *g_dumps)) continue;
        int bpp = dump_bytes(v->fmt); if (!bpp) { LOG("[dump] pass %d att %d: format %d not dumped", g_dump_pass, i, v->fmt); continue; }
        uint32_t w = v->w >> P.clv[i] ? v->w >> P.clv[i] : 1, h = v->h >> P.clv[i] ? v->h >> P.clv[i] : 1;
        dump_t *d = &g_dumps[g_ndump]; if (!stg_new(&d->s, (VkDeviceSize)w * h * bpp)) continue;
        d->w = w; d->h = h; d->f = v->fmt; d->pass = g_dump_pass; d->att = i; d->ser = v->serial; g_ndump++;
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
        vkCmdPipelineBarrier(C->cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        int z3 = v->type == VK_IMAGE_TYPE_3D;   // a 3D target's slice is a depth offset, not a layer
        VkBufferImageCopy r = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, P.clv[i], z3 ? 0 : P.cly[i], 1}, .imageOffset = {0, 0, z3 ? (int32_t)P.cly[i] : 0}, .imageExtent = {w, h, 1}};
        vkCmdCopyImageToBuffer(C->cb, v->img, VK_IMAGE_LAYOUT_GENERAL, d->s.b, 1, &r);
        vkCmdPipelineBarrier(C->cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    }
    for (int i = 0; i < g_ndump_tex; i++) {   // level 0, layer 0 of each texture the draws read (colour, uncompressed): f<frame>_p<pass>_t_<w>x<h>_vk<format>_s<serial>
        // a 3D texture: every depth slice, stacked downwards (the height in the name is the slices' total)
        vtex *v = nvn_hm_get(&g_tex, &g_dump_tex[i]); int bpp = v ? dump_bytes(v->fmt) : 0;
        if (!bpp || v->aspect != VK_IMAGE_ASPECT_COLOR_BIT || v->samples > 1 || g_ndump >= (int)(sizeof g_dumps / sizeof *g_dumps)) continue;
        uint32_t z = v->type == VK_IMAGE_TYPE_3D ? v->d : 1;
        dump_t *d = &g_dumps[g_ndump]; if (!stg_new(&d->s, (VkDeviceSize)v->w * v->h * z * bpp)) continue;
        d->w = v->w; d->h = v->h * z; d->f = v->fmt; d->pass = g_dump_pass; d->att = -1; d->ser = v->serial; g_ndump++;
        VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
        vkCmdPipelineBarrier(C->cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
        VkBufferImageCopy r = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {v->w, v->h, z}};
        vkCmdCopyImageToBuffer(C->cb, v->img, VK_IMAGE_LAYOUT_GENERAL, d->s.b, 1, &r);
    }
    g_ndump_tex = 0;
}
static void dump_write(void) {   // after the dumped frame's commands completed
    const char *dir = getenv("DM_DUMP_DIR"); if (!dir) dir = ".";
    for (int i = 0; i < g_ndump; i++) {
        dump_t *d = &g_dumps[i]; char path[700];
        if (d->att < 0) snprintf(path, sizeof path, "%s/f%ld_p%03d_t_%ux%u_vk%d_s%llu.raw", dir, dump_frame(), d->pass, d->w, d->h, d->f, (unsigned long long)d->ser);
        else snprintf(path, sizeof path, "%s/f%ld_p%03d_a%d_%ux%u_vk%d_s%llu.raw", dir, dump_frame(), d->pass, d->att, d->w, d->h, d->f, (unsigned long long)d->ser);
        FILE *fp = fopen(path, "wb"); if (fp) { fwrite(d->s.p, 1, (size_t)d->s.n, fp); fclose(fp); }
        memcpy(path + strlen(path) - 4, ".png", 4); png_texels(path, d->f, dump_bytes(d->f), d->s.p, d->w, 0, 0, d->w, d->h, 0);   // beside the raw texels, in the formats gfx_vk.c px_rgb converts
        stg_free(&d->s);
    }
    LOG("[dump] frame %ld: %d targets written to %s", dump_frame(), g_ndump, dir); g_ndump = 0;
}
static void occl_end(void) { if (g_q_act >= 0 && C && C->open) vkCmdEndQuery(C->cb, C->qp, (uint32_t)g_q_act); g_q_act = -1; }   // the pass's open occlusion query (gfx_vk.c)
static void occl_begin(void) {   // a draw while samples passed are counted: inside a query of this pass
    if (!g_q_on || g_q_act != -1 || !C || !C->qp || C->nq >= 1024) return;
    int mode = occl_mode();
    if (!mode) { g_qres[g_qslot++ % QRES] = 1; g_q_act = -2; g_q_noq++; return; }   // the stretch's slot reads 1 with no query (DM_VK_OCCLUSION=0; the completion thread reads only the slots of queries)
    g_q_act = C->nq++; C->qg[g_q_act] = g_qslot++; g_q_queries++;
    vkCmdBeginQuery(C->cb, C->qp, (uint32_t)g_q_act, mode == 2 && g_vk_feat.occlusionQueryPrecise ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
}
// ---- GPU time per render pass: a file named prof_now beside $DM_DUMP_DIR profiles the frame two presents later (the log lists its passes, costliest first) ----
typedef struct { chunk *c; int s0, s1, draws, ncolor, depth, cont; uint32_t w, h; VkFormat f0; uint64_t t0, t1, seq, verts, big; const char *bigvs, *bigfs; VkFramebuffer fb; } prof_rec;   // verts: vertices drawn (indices × instances); big: the largest draw's, and its shaders (a dispatch: groups in w, h, draws; its compute shader in bigfs); cont: a piece of the pass before it (the same framebuffer, begun again after a chunk was sent in its middle)
static prof_rec g_prof[1024]; static int g_nprof, g_prof_cur = -1; static long g_prof_at = -1; static uint64_t g_prof_seq;
static int prof_on(void) { return g_prof_at > 0 && (long)atomic_load(&g_presents) + 1 == g_prof_at; }
static void prof_poll(uint64_t presents) {
    const char *dir = getenv("DM_DUMP_DIR"); if (!dir || presents % 30) return;
    char p[700]; snprintf(p, sizeof p, "%s/../prof_now", dir);
    if (!access(p, F_OK)) { unlink(p); g_prof_at = (long)presents + 2; g_nprof = 0; LOG("[prof] prof_now seen: frame %ld is profiled", g_prof_at); }
}
static void prof_pass_begin(void) {
    if (!prof_on() || !C || !C->pq || C->npq + 2 > 512 || g_nprof >= 1024) return;
    prof_rec *r = &g_prof[g_nprof]; memset(r, 0, sizeof *r); r->c = C; r->s0 = C->npq++; r->s1 = -1; r->w = P.w; r->h = P.h; r->ncolor = (int)P.key.ncolor; r->depth = P.key.depth != 0;
    for (uint32_t i = 0; i < P.key.ncolor; i++) if (P.key.color[i]) { r->f0 = P.key.color[i]; break; }
    r->seq = g_prof_seq++; r->fb = P.fb;
    for (int i = g_nprof - 1; i >= 0; i--) if (g_prof[i].ncolor >= 0) { r->cont = g_prof[i].fb == P.fb && g_prof[i].seq + 1 == r->seq; break; }   // the record before it is a pass of the same framebuffer: a piece
    vkCmdWriteTimestamp(C->cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, C->pq, (uint32_t)r->s0); g_prof_cur = g_nprof++;
}
static void prof_pass_end(void) {
    if (g_prof_cur < 0) return;
    prof_rec *r = &g_prof[g_prof_cur]; g_prof_cur = -1;
    if (r->c != C || C->npq >= 512) return;
    r->s1 = C->npq++; vkCmdWriteTimestamp(C->cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, C->pq, (uint32_t)r->s1);
}
static void prof_collect(chunk *c) {   // the completion thread: this chunk's timestamps into its records
    uint64_t t[512]; memset(t, 0, sizeof t);
    VkResult res = vkGetQueryPoolResults(g_vk.dev, c->pq, 0, (uint32_t)c->npq, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT); int got = 0, mine = 0;
    for (int i = 0; i < g_nprof; i++) if (g_prof[i].c == c) { mine++; if (res == VK_SUCCESS && g_prof[i].s1 >= 0) { g_prof[i].t0 = t[g_prof[i].s0]; g_prof[i].t1 = t[g_prof[i].s1]; g_prof[i].c = NULL; got++; } }
    LOG("[prof] chunk %d done: %d timestamps, VkResult %d, %d of its %d records read", (int)(c - g_ch), c->npq, res, got, mine);
    c->npq = 0;
}
static int prof_cmp(const void *a, const void *b) { const prof_rec *x = a, *y = b; uint64_t dx = x->t1 - x->t0, dy = y->t1 - y->t0; return dx < dy ? 1 : dx > dy ? -1 : 0; }
static void prof_write(void) {
    double per = g_vk.props.limits.timestampPeriod, sum = 0; uint64_t lo = UINT64_MAX, hi = 0; int n = 0;
    for (int i = 0; i < g_nprof; i++) if (g_prof[i].t1 > g_prof[i].t0) { sum += (g_prof[i].t1 - g_prof[i].t0) * per / 1e3; if (g_prof[i].t0 < lo) lo = g_prof[i].t0; if (g_prof[i].t1 > hi) hi = g_prof[i].t1; n++; }
    int unclosed = 0, unread = 0, ncs = 0; double cs = 0;
    for (int i = 0; i < g_nprof; i++) { if (g_prof[i].s1 < 0) unclosed++; else if (g_prof[i].c) unread++; if (g_prof[i].ncolor < 0 && g_prof[i].t1 > g_prof[i].t0) { ncs++; cs += (g_prof[i].t1 - g_prof[i].t0) * per / 1e3; } }
    int npieces = 0; double pieces = 0;   // passes begun again on the same framebuffer after a chunk was sent in their middle (gfx_vk.c MID_DRAWS): each loads and stores its targets again
    for (int i = 0; i < g_nprof; i++) if (g_prof[i].ncolor >= 0 && g_prof[i].cont && g_prof[i].t1 > g_prof[i].t0) { npieces++; pieces += (g_prof[i].t1 - g_prof[i].t0) * per / 1e3; }
    LOG("[prof] frame %ld: %d records (%d not closed, %d not read), %d timed, %.0f us in them (%d dispatches %.0f us; %d passes continued after a cut, %.0f us in them), first start to last end %.0f us",
        g_prof_at, g_nprof, unclosed, unread, n, sum, ncs, cs, npieces, pieces, hi > lo ? (hi - lo) * per / 1e3 : 0.0);
    // each pass with its pieces, costliest first: a target drawn again later in the frame is another pass
    { struct { VkFramebuffer fb; uint64_t seq, verts; int draws, pieces; double us; uint32_t w, h; } g[256]; int ng = 0;
      for (int i = 0; i < g_nprof; i++) { prof_rec *r = &g_prof[i]; if (r->ncolor < 0 || r->t1 <= r->t0) continue;
          if (r->cont && ng && g[ng - 1].fb == r->fb) { g[ng - 1].draws += r->draws; g[ng - 1].pieces++; g[ng - 1].us += (r->t1 - r->t0) * per / 1e3; g[ng - 1].verts += r->verts; continue; }
          if (ng == 256) break;
          g[ng].fb = r->fb; g[ng].seq = r->seq; g[ng].draws = r->draws; g[ng].pieces = 1; g[ng].us = (r->t1 - r->t0) * per / 1e3; g[ng].verts = r->verts; g[ng].w = r->w; g[ng].h = r->h; ng++; }
      for (int a = 0; a < ng; a++) for (int b = a + 1; b < ng; b++) if (g[b].us > g[a].us) { __typeof__(g[0]) t = g[a]; g[a] = g[b]; g[b] = t; }
      for (int i = 0; i < ng && i < 16; i++) LOG("[prof] whole pass from #%llu: %7.0f us  %ux%u  %d pieces  draws %d  vertices %llu", (unsigned long long)g[i].seq, g[i].us, g[i].w, g[i].h, g[i].pieces, g[i].draws, (unsigned long long)g[i].verts); }
    qsort(g_prof, (size_t)g_nprof, sizeof *g_prof, prof_cmp);
    static int top = -1; if (top < 0) { const char *e = getenv("DM_VK_PROF_TOP"); top = e ? atoi(e) : 1024; }   // every record, costliest first; DM_VK_PROF_TOP=<n>: the first n
    for (int i = 0; i < g_nprof && i < top; i++) { prof_rec *r = &g_prof[i]; if (r->t1 <= r->t0) continue;
        if (r->ncolor < 0) LOG("[prof] %7.0f us  dispatch #%llu  %ux%ux%u groups  cs %s", (r->t1 - r->t0) * per / 1e3, (unsigned long long)r->seq, r->w, r->h, (unsigned)r->draws, r->bigfs ? r->bigfs : "-");
        else LOG("[prof] %7.0f us  pass #%llu%s  %ux%u  colour %d (vk%d) depth %d  draws %d  vertices %llu (largest draw %llu: vs %s fs %s)", (r->t1 - r->t0) * per / 1e3, (unsigned long long)r->seq, r->cont ? " (piece)" : "", r->w, r->h,
                 r->ncolor, r->f0, r->depth, r->draws, (unsigned long long)r->verts, (unsigned long long)r->big, r->bigvs ? r->bigvs : "-", r->bigfs ? r->bigfs : "-"); }
    g_nprof = 0; g_prof_at = -1;
}
// Compressed render targets through an uncompressed shadow (nvn_mtl.m bcrt_attach / bcrt_flush). A GPU block encoder draws into a BC texture through an uncompressed
// integer view, one texel per block: agl's BC4 encoder in Breath of the Wild (8e2cc2671cae6020, a 32×32 BC4 texture through an RGBA16UI view) and Tears of the Kingdom
// (one depth plane of a 32×32×32 BC4 texture per pass). Vulkan has no such attachment: the BC image itself was attached, and every submission holding that pass stopped the
// Adreno 740's command processor ("CP opcode error", three at every Breath of the Wild start, in the kernel log), and the kernel driver dropped each such submission whole,
// with everything else recorded in it (uploads, other passes). The pass draws into a shadow of blocks × blocks in the view's
// format instead; the level's (layer's or depth plane's) blocks are copied into it before the pass and back after it (vkCmdCopyImage: a block and a texel of one size).
// A BC texture the CPU decoded (no BC textures on the GPU) is shadowed too, for BC4 and BC5: the shadow keeps its blocks between passes, and after each pass they are
// decoded into the image on the GPU (gfx_vk_bcdec.inc bcd_decode) instead of copied back
typedef struct { VkImage base; uint32_t lv, ly; VkFormat f; } bcrt_key;
static int bcd_takes(const vtex *b); static int bcd_decode(vtex *b, vtex *s, uint32_t lv, uint32_t ly);   // gfx_vk_bcdec.inc
static nvn_hmap g_bcrt = {.klen = sizeof(bcrt_key)};
static struct { vtex *base, *sh; uint32_t lv, ly; } g_bcrt_pend[8];   // the pass's shadowed attachments: copied back at rp_end
static uint64_t g_bcrt_n;
static int bc_block_bytes(VkFormat f) {   // a BC format's block size; 0: not BC
    if (f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC1_RGBA_SRGB_BLOCK) return 8;
    if (f == VK_FORMAT_BC4_UNORM_BLOCK || f == VK_FORMAT_BC4_SNORM_BLOCK) return 8;
    if (f >= VK_FORMAT_BC2_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK) return 16;
    return 0;
}
static VkImageSubresourceLayers bcrt_sub(const vtex *b, uint32_t lv, uint32_t ly) { return (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, lv, b->type == VK_IMAGE_TYPE_3D ? 0 : ly, 1}; }
static VkOffset3D bcrt_off(const vtex *b, uint32_t ly) { return (VkOffset3D){0, 0, b->type == VK_IMAGE_TYPE_3D ? (int32_t)ly : 0}; }
// the shadow of base's level lv, layer (or depth plane) ly, for an attachment view of NVN format nf: NULL when base is not BC or the view is not one block per texel
static vtex *bcrt_shadow(vtex *b, uint32_t nf, uint32_t lv, uint32_t ly) {
    int bb = bc_block_bytes(b->conv ? b->nvfmt : b->fmt); uint32_t m = 0; uint8_t sw[4];   // a decoded image's blocks: its NVN format's
    if (!bb || !nvn_fmt_to_mtl(nf, &m, sw) || !m) return NULL;
    VkFormat f = vk_of_mtl(m); if (!f || fmt_bytes(f) != bb || (b->conv && !bcd_takes(b))) return NULL;
    bcrt_key k; memset(&k, 0, sizeof k); k.base = b->img; k.lv = lv; k.ly = ly; k.f = f;
    vtex *s = nvn_hm_get(&g_bcrt, &k); if (s) return s;
    uint32_t lw = b->w >> lv ? b->w >> lv : 1, lh = b->h >> lv ? b->h >> lv : 1;
    tex_t t; memset(&t, 0, sizeof t); t.magic = MAGIC('T', 'E', 'X', 0); t.serial = ~0x10000ull - g_bcrt_n++; t.d.target = NVN_TEXTURE_TARGET_2D; t.d.format = nf;
    t.d.width = (lw + 3) / 4; t.d.height = (lh + 3) / 4; t.d.depth = 1; t.d.levels = 1;
    if (!(s = vt_create(&t))) return NULL;
    nvn_hm_put(&g_bcrt, &k, s);
    static int nlog; if (nlog++ < 4) LOG("[vk] compressed render target s%llu (vk%d %ux%u level %u layer %u) drawn through a %ux%u shadow in vk%d", (unsigned long long)b->serial, b->fmt, b->w, b->h, lv, ly, t.d.width, t.d.height, f);
    return s;
}
static void bcrt_drop(VkImage base) {   // base goes: its shadows too
    for (uint32_t i = 0; i < g_bcrt.cap; i++) {
        vtex *s = g_bcrt.vals[i]; if (!s) continue;
        bcrt_key k; memcpy(&k, g_bcrt.keys + (size_t)i * g_bcrt.klen, sizeof k); if (k.base != base) continue;
        nvn_hm_del(&g_bcrt, &k); vt_destroy(s); i = (uint32_t)-1;   // the table moved entries on delete: scan again
    }
}
static void bcrt_copy(vtex *b, vtex *s, uint32_t lv, uint32_t ly, int back) {   // outside a pass: the blocks into the shadow (back 0) or out of it
    VkImageCopy r; memset(&r, 0, sizeof r);
    VkImageSubresourceLayers bs = bcrt_sub(b, lv, ly), ss = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    if (back) { r.srcSubresource = ss; r.dstSubresource = bs; r.dstOffset = bcrt_off(b, ly); r.extent = (VkExtent3D){s->w, s->h, 1}; }   // the extent is the source's: texels of the shadow
    else { uint32_t lw = b->w >> lv ? b->w >> lv : 1, lh = b->h >> lv ? b->h >> lv : 1;
           r.srcSubresource = bs; r.srcOffset = bcrt_off(b, ly); r.dstSubresource = ss; r.extent = (VkExtent3D){lw, lh, 1}; }   // texels of the BC level
    cb_barrier();
    vkCmdCopyImage(C->cb, back ? s->img : b->img, VK_IMAGE_LAYOUT_GENERAL, back ? b->img : s->img, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    cb_barrier();
}
// A linear render target (LINEAR / LINEAR_RENDER_TARGET in a pool the CPU can read) is how a game hands GPU results to the CPU: it draws into the texture and reads the
// pool's memory after a fence. nvn_mtl.m makes such a texture a view on the pool's memory, so the GPU writes land there; here the image is the texture, so the level drawn
// is copied back into guest memory (rows at the texture's stride, as vt_upload reads them) when the chunk completes, before its fence is signalled (chunk_done).
// Breath of the Wild draws about twenty (1×1 to 512×512, CPU_CACHED | GPU_CACHED pools) and reads ground heights from them: read as zeros, every grass press got the presser's
// world height instead of its height above the ground (about 250 instead of below 1 at Link's feet), pressed nothing, and the grass stood through Link's legs.
// Only the bytes the pass changed against its snapshot are written, and the texture keeps its image after its own write-back (gfx_vk.c, linear render targets' write-backs)
// the level of a linear render target its pass writes back (lin_writeback, lsnap_take): rows of pitch bytes at stride srow from dst; size: the storage's; 0 = none (not one, or its pool the CPU cannot read)
static int lin_geom(const vtex *v, uint32_t lv, uint32_t ly, size_t *pitch, size_t *rows, size_t *srow, uint8_t **dst, uint64_t *size) {
    if (!v->pkg || !v->store || v->conv || lv || v->type == VK_IMAGE_TYPE_3D || v->aspect != VK_IMAGE_ASPECT_COLOR_BIT || !(v->desc.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET))) return 0;
    if (v->pool && (((const pool_t *)v->pool)->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS)) return 0;   // the CPU cannot read it: nothing to hand over
    int bw, bh, bpb = nvn_fmt_bpb(v->desc.format, &bw, &bh); if (!bpb) return 0;
    nvn_texlayout L; nvn_tex_layout(&v->desc, &L);
    *pitch = (size_t)((v->w + bw - 1) / bw) * bpb; *rows = (v->h + bh - 1) / bh; *srow = L.layer_size / (*rows ? *rows : 1); if (*srow < *pitch) *srow = *pitch;
    *dst = (uint8_t *)v->store + (size_t)ly * L.layer_size; *size = L.size;
    return bpb;
}
static int linwb_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_LINWB"); v = !e || atoi(e) != 0; } return v; }   // DM_VK_LINWB=0 (env.txt): no linear write-back, a diagnostic
// The pass's snapshot of a linear target's guest bytes (gfx_vk.c, linear render targets' write-backs): taken at rp_prepare right after vt_get, when the image equals guest memory
// there, unless a write-back or queued copy to those bytes is still pending (it would land after the snapshot: the pass is then written back whole). Packed at the pitch, read at
// the texture's stride as vt_upload_ reads it; held in P until rp_end hands it to the write-back (a pooled buffer for 64 KB and up: snap_take), given back on every other path:
// the next rp_prepare (a draw that failed after it, or none began the pass), a failed rp_prepare, a target its write-back did not take, an image that goes (views_drop)
static void lsnap_drop(int i) { snap_give(P.snap[i], P.snap_cap[i]); P.snap[i] = NULL; P.snap_cap[i] = 0; }
static void lsnap_drop_all(void) { for (int i = 0; i < 8; i++) if (P.snap[i]) lsnap_drop(i); }
static void lsnap_drop_img(VkImage img) { for (int i = 0; i < 8; i++) if (P.snap[i] && P.snap_img[i] == img) lsnap_drop(i); }
static void lsnap_take(int i, const vtex *v, uint32_t lv, uint32_t ly) {
    if ((!lsnap_on() && !wb_revert_on()) || !linwb_on()) return;
    size_t pitch, rows, srow, cap; uint8_t *dst; uint64_t size;
    if (!lin_geom(v, lv, ly, &pitch, &rows, &srow, &dst, &size)) return;
    if (wr_pending((uintptr_t)dst, (uintptr_t)dst + srow * (rows - 1) + pitch)) { g_lsnap_stale++; return; }
    uint8_t *p = snap_take(pitch * rows, &cap); if (!p) return;
    for (size_t r = 0; r < rows; r++) memcpy(p + r * pitch, dst + r * srow, pitch);
    P.snap[i] = p; P.snap_cap[i] = cap; P.snap_n[i] = pitch * rows; P.snap_dst[i] = dst; P.snap_img[i] = v->img; g_lsnap++;
}
static void lin_writeback(int i) {   // attachment i of the pass that ends
    vtex *v = P.cv[i]; uint32_t ly = P.cly[i]; size_t pitch, rows, srow; uint8_t *dst; uint64_t size;
    int bpb = lin_geom(v, P.clv[i], ly, &pitch, &rows, &srow, &dst, &size); if (!bpb) return;
    stg_sl s = stg_take(pitch * rows, bpb); if (!s.p) return;
    VkBufferImageCopy r = {.bufferOffset = s.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, ly, 1}, .imageExtent = {v->w, v->h, 1}};
    cb_barrier(); vkCmdCopyImageToBuffer(C->cb, v->img, VK_IMAGE_LAYOUT_GENERAL, s.b, 1, &r); cb_barrier();
    wb_t *w = wb_add();
    if (w) { *w = (wb_t){s.st, (size_t)s.off, pitch, rows, pitch, srow, dst, NULL, 0};
        w->lin = v->levels == 1 && v->layers == 1 && !ly && srow * (rows - 1) + pitch <= size ? 2 : 1;   // 2: it covers every byte vt_upload_ reads, inside the storage it protects (ab_note)
        w->ser = v->serial; w->img = (uint64_t)v->img; w->fmt = v->desc.format; w->bpb = (uint32_t)bpb; w->w = v->w; w->h = v->h; w->tsize = size; w->rbx = v->rbx;   // rbx: into the guest's order as it lands
        if (P.snap[i] && P.snap_dst[i] == dst && P.snap_n[i] == pitch * rows) { *(lsnap_on() ? &w->snap : &w->dsnap) = P.snap[i]; w->snap_cap = P.snap_cap[i]; P.snap[i] = NULL; P.snap_cap[i] = 0; } }
    static int nlog; if (nlog++ < 4) LOG("[vk] linear render target s%llu %ux%u (format %#x) written back to guest memory after its pass", (unsigned long long)v->serial, v->w, v->h, v->desc.format);
}
static void rp_end(void) {
    if (!P.active) return;
    occl_end(); vkCmdEndRenderPass(C->cb); prof_pass_end(); P.active = 0; if (dump_on()) dump_pass_end();   // the profile's end timestamp after the pass: on a tiler the tiles' stores are part of it (a timestamp inside a pass is written per tile, and only the last tile's draws came before it)
    for (int i = 0; i < 8; i++) if (g_bcrt_pend[i].sh) {
        if (g_bcrt_pend[i].base->conv) bcd_decode(g_bcrt_pend[i].base, g_bcrt_pend[i].sh, g_bcrt_pend[i].lv, g_bcrt_pend[i].ly);   // decoded on the GPU into the image the game samples
        else bcrt_copy(g_bcrt_pend[i].base, g_bcrt_pend[i].sh, g_bcrt_pend[i].lv, g_bcrt_pend[i].ly, 1);
        g_bcrt_pend[i].sh = NULL; }
    if (linwb_on()) for (int i = 0; i < 8; i++) if (P.cv[i]) lin_writeback(i);
    lsnap_drop_all();   // what no write-back took
}
// a pass's target: drawn into in this present, and in how many presents in a row (apipe_ok)
static void rt_note(vtex *v) { uint64_t p = atomic_load(&g_presents) + 1; if (v->rt_last != p) { v->rt_runs = v->rt_last + 1 == p ? v->rt_runs + 1 : 1; v->rt_last = p; } }
// the render pass of the bound render targets (R.rt): images, views and framebuffer, made before the draw's textures are resolved (that may record uploads, outside a pass).
// layered: the draw's pipeline writes the layer (vshader wlayer), so a 3D or array target is attached whole, each slice or layer its view covers a layer of the framebuffer.
// Attached a slice at a time (layer 0 of the view, a framebuffer of one layer), the draws of a volume texture's slices, one instance per slice with a geometry shader
// picking the layer, wrote slice 0 alone: Final Fantasy Resonance's 32x32x32 colour-grading LUT kept its other 31 slices as whatever its memory held, and the tone
// mapper drew the field as white specks on black. A layered pass serves the draws that write no layer too (layer 0, as before), so only a layered draw meeting a pass
// of one layer ends it; a view of one layer (Bowser's Fury draws its LUT a slice at a time) stays that layer. DM_VK_LAYERED=0 (env.txt): one layer, as before
static int layered_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_LAYERED"); v = !(e && *e == '0'); if (!v) LOG("[vk] DM_VK_LAYERED=0: render targets are attached a layer at a time"); } return v; }
static uint64_t g_layered_passes;
// the layers of target v a layered pass attaches from layer (or depth slice) ly of level lv: what its view covers, else every one from ly on
static uint32_t rt_layers(const vtex *v, const view_t *cv, uint32_t lv, uint32_t ly) {
    uint32_t tot = v->type == VK_IMAGE_TYPE_3D ? (v->d >> lv ? v->d >> lv : 1) : v->layers, n;
    if (ly >= tot) return 1;
    n = cv && cv->set_layers && cv->nlayers ? cv->nlayers : tot - ly;
    return n > tot - ly ? tot - ly : n ? n : 1;
}
// A render pass with colour slot 0 empty below a used slot, on Adreno 8xx: Breath of the Wild lost the GPU on a OnePlus Pad 3 (Adreno 830, driver 0x8032004a) at its first
// frame in the field. One pass binds a 640x360 target in slot 5 alone (slots 0-4 VK_ATTACHMENT_UNUSED); the pass itself runs, but a later pass drawing into that target as
// slot 0 then hung the GPU every frame (the driver's recovery about 250 ms in) until one hang ran past its 3 s and the device was lost (dm/android/baselines.md problem 14).
// Skipping the first pass's draws did not help (its clear still begins it); a scratch target in slot 0 alone stops it, slots 1-4 left empty, and an empty slot 6 below a
// used slot 7 needs nothing. The Odin 2's Adreno 740 runs the same frames without it. On a Moto Razr Plus 2023 (Adreno 730, Qualcomm's driver V@0615.98) Mario Kart 8
// Deluxe crashed at every course load (SIGSEGV at address 4 in vulkan.adreno.so, in vkCmdEndRenderPass): a one-draw pass with slots 0-2 empty and an A2B10G10R10 target in
// slot 3; with slot 0 filled it runs. So it is on for every Adreno on Qualcomm's driver (its driver ID, g_qcom_driver; not Turnip, however a build names it). One RGBA8 image serves every
// such pass (an attachment may be larger than the framebuffer), made again larger when a pass is larger; the earlier ones are kept, as cached framebuffers name their views.
// Its sides are rounded up to 512: Mario Kart 8 Deluxe's dynamic resolution grows that pass a few texels at a time (480x270, 484x272, 493x277...), which used up the 8
// sizes in seconds and left a larger pass's slot 0 empty again. Divinity: Original Sin 2 crashed the 730 the same way (address 4 in vkCmdEndRenderPass) 8 s into every
// start, on a pass with one colour slot, empty, and a D32_S8 target: a one-draw depth-stencil pass that writes no colour. A pass whose colour slots are all empty is made with
// none (g_slot0_none): a depth-only pass, which the driver runs. A scratch target in slot 0 ran it too, but would be loaded and stored in each such pass (28 a frame at
// Divinity's title), and layered or multisample ones would keep slot 0 empty. DM_VK_SLOT0_FILL: 1 on any GPU, 0 off (both)
static uint64_t g_slot0_fills, g_slot0_none;
static int slot0_fill_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("DM_VK_SLOT0_FILL"); v = e ? *e == '1' : g_qcom_driver;
                 LOG("[vk] a render pass with colour slot 0 empty: %s", v ? "a scratch target in slot 0 below a used slot, no colour slot when every one is empty (DM_VK_SLOT0_FILL)" : "slot 0 left empty"); }
    return v;
}
static vtex *slot0_scratch(uint32_t w, uint32_t h) {
    static vtex *sv[8]; static int n;
    vtex *v = n ? sv[n - 1] : NULL;
    if (v && v->w >= w && v->h >= h) return v;
    if (n == 8) { RED_ONCE(152, "the scratch target for an empty colour slot 0 was made larger 8 times: passes larger than %ux%u keep slot 0 empty", v->w, v->h); return NULL; }
    tex_t t; memset(&t, 0, sizeof t); t.magic = MAGIC('T', 'E', 'X', 0); t.serial = ~0ull - 1 - (uint64_t)n; t.d.target = NVN_TEXTURE_TARGET_2D; t.d.format = NVN_FORMAT_RGBA8;
    uint32_t nw = v && v->w > w ? v->w : w, nh = v && v->h > h ? v->h : h;
    t.d.width = (nw + 511) & ~511u; t.d.height = (nh + 511) & ~511u; t.d.depth = 1; t.d.levels = 1;
    if (!(v = vt_create(&t))) return NULL;
    LOG("[vk] scratch target for an empty colour slot 0: %ux%u", v->w, v->h);
    return sv[n++] = v;
}
#include "gfx_vk_layerpad.inc"   // small layered passes padded past the tile memory (layer_pad_on)
// One image in two colour slots of a pass: Breath of the Wild's 1280x720 G-buffer passes bind one target at slots 1 and 2 and another at 3 and 4, and write only the
// first of each (channel mask 0xf0f0ff). Vulkan leaves attachments that alias in one subpass undefined: the OnePlus Pad 3's Adreno 830 stored the unwritten copy over
// the written one, and the Shrine of Resurrection was drawn dark (Link unlit) and blurred (dm/android/baselines.md problem 15), where the Odin 2's Adreno 740 drew it
// right. So on every GPU a pass attaches each image once: at the first of its slots the draw beginning the pass writes, else at the first; its other slots are
// VK_ATTACHMENT_UNUSED, and a clear of one of them clears the kept slot. A later draw of the pass writing a slot left unused and not the kept one loses that write: counted
// (g_dup_lost), with a red line. DM_VK_DUP_SLOTS=0: the image attached at each of its slots, as before
static uint8_t g_dup_mask; static int8_t g_dup_of[8];   // the pass's colour slots left unused as copies, and the slot each one's image is kept at
static uint64_t g_dup_passes, g_dup_lost;
static int dup_slots_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("DM_VK_DUP_SLOTS"); v = !(e && *e == '0'); if (!v) LOG("[vk] DM_VK_DUP_SLOTS=0: an image in two colour slots of a pass is attached at both"); }
    return v;
}
static void dup_slots(rp_key *k, VkImageView *cvw) {
    uint32_t m = D.chmask.mask;
    for (uint32_t i = 1; i < k->ncolor; i++) {
        if (!cvw[i] || !P.cv[i]) continue;
        uint32_t j = 0; while (j < i && !(cvw[j] && P.cv[j] == P.cv[i] && P.clv[j] == P.clv[i] && P.cly[j] == P.cly[i])) j++;
        if (j == i) continue;   // j: the slot this image is kept at so far (the only earlier one still attached)
        uint32_t d = i, keep = j;
        if (!(m >> 4 * j & 0xf) && (m >> 4 * i & 0xf)) { d = j; keep = i; }
        cvw[d] = VK_NULL_HANDLE; k->color[d] = VK_FORMAT_UNDEFINED; P.cv[d] = NULL; g_bcrt_pend[d].sh = NULL;   // a BC shadow's copy back is the kept slot's
        for (int s = 0; s < 8; s++) if (g_dup_mask >> s & 1 && (uint32_t)g_dup_of[s] == d) g_dup_of[s] = (int8_t)keep;
        g_dup_mask |= (uint8_t)(1u << d); g_dup_of[d] = (int8_t)keep;
    }
    if (g_dup_mask) g_dup_passes++;
}
// the slot a clear of colour slot i reaches in the pass: the kept one when i is a copy left unused (dup_slots)
static int dup_slot(int i) { return i >= 0 && i < 8 && g_dup_mask >> i & 1 ? g_dup_of[i] : i; }
static void dup_check(void) {
    uint32_t m = D.chmask.mask;
    for (int d = 0; d < 8; d++) if (g_dup_mask >> d & 1 && m >> 4 * d & 0xf && !(m >> 4 * g_dup_of[d] & 0xf)) {
        g_dup_lost++; RED_ONCE(170, "a draw writes colour slot %d and not slot %d, where the pass keeps their image: the write is lost (dup_slots; DM_VK_DUP_SLOTS=0 attaches both)", d, g_dup_of[d]); return;
    }
}
static uint32_t g_lbase;   // a layered pass's first layer past its targets' views (vs_layer_groups: 0, then a multiple of g_vs_layers while a draw is made again per group)
static int rp_prepare_(int layered) {
    layered = layered && layered_on();
    if (P.active && (P.layered || !layered)) return 1;
    if (P.active) rp_end();   // a draw that writes the layer, in a pass of one layer: the pass again, layered
    lsnap_drop_all();   // an earlier rp_prepare's that no pass used
    if (!R.has_rt) { RED_ONCE(83, "draw with no render target set"); return 0; }
    memset(P.cv, 0, sizeof P.cv); P.dv = NULL; memset(g_bcrt_pend, 0, sizeof g_bcrt_pend); g_dup_mask = 0;
    rp_key k; memset(&k, 0, sizeof k); VkImageView views[9]; memset(views, 0, sizeof views); uint32_t nv = 0, w = UINT32_MAX, h = UINT32_MAX, layers = UINT32_MAX; k.samples = 1;
    VkImageView cvw[8] = {0}, dvw = VK_NULL_HANDLE;   // by slot, then the depth view: views[] in attachment order once slot 0 may be filled (slot0_fill_on)
    k.ncolor = (uint32_t)(R.rt.n > 8 ? 8 : R.rt.n);
    for (uint32_t i = 0; i < k.ncolor; i++) {
        if (!R.rt.color[i]) continue;
        vtex *v = vt_get_w(R.rt.color[i]); if (!v) continue;
        const view_t *cv = R.rt.has_cview[i] ? &R.rt.cview[i] : NULL;
        uint32_t lv = cv && cv->set_levels ? cv->base_level : 0, ly = cv && cv->set_layers ? cv->min_layer : 0;
        vtex *sh = cv && cv->set_format ? bcrt_shadow(v, cv->format, lv, ly) : NULL;   // a BC texture written through a one-block-per-texel view (bcrt_shadow)
        if (sh) {
            VkCommandBuffer c = cb(); if (!c) continue;
            if (!v->conv) bcrt_copy(v, sh, lv, ly, 0);   // a decoded image holds texels, not blocks: its shadow keeps the blocks its last pass drew
            VkImageView iv = view_get(sh, VK_IMAGE_VIEW_TYPE_2D, 0, 1, 0, 1, VK_IMAGE_ASPECT_COLOR_BIT, NULL); if (!iv) continue;
            k.color[i] = sh->fmt; k.samples = sh->samples; cvw[i] = iv; nv++; P.cv[i] = sh; P.clv[i] = 0; P.cly[i] = 0; rt_note(sh);
            g_bcrt_pend[i].base = v; g_bcrt_pend[i].sh = sh; g_bcrt_pend[i].lv = lv; g_bcrt_pend[i].ly = ly;
            if (sh->w < w) w = sh->w; if (sh->h < h) h = sh->h;
            continue;
        }
        // in the format of the target's view: Super Mario Bros. Wonder's last pass writes its RGBA8 window texture through an RGBA8_SRGB view, and in the image's
        // format the linear values were stored unencoded (the whole picture darker and more saturated; nvn_mtl.m found the same)
        g_view_fmt = cv ? view_format(v, cv) : VK_FORMAT_UNDEFINED; VkFormat af = g_view_fmt ? g_view_fmt : v->fmt;
        if (layered) ly += g_lbase;
        uint32_t nl = layered ? rt_layers(v, cv, lv, ly) : 1;
        g_view_slice = 1; VkImageView iv = view_get(v, nl > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D, lv, 1, ly, nl, VK_IMAGE_ASPECT_COLOR_BIT, NULL); g_view_slice = 0; g_view_fmt = VK_FORMAT_UNDEFINED; if (!iv) continue;
        if (nl < layers) layers = nl;
        k.color[i] = af; k.samples = v->samples; cvw[i] = iv; nv++; P.cv[i] = v; P.clv[i] = lv; P.cly[i] = ly; rt_note(v);
        lsnap_take((int)i, v, lv, ly);   // a linear target: its guest bytes now, which its write-back diffs against
        uint32_t lw = v->w >> lv ? v->w >> lv : 1, lh = v->h >> lv ? v->h >> lv : 1; if (lw < w) w = lw; if (lh < h) h = lh;
    }
    if (R.rt.depth) {
        vtex *v = vt_get_w(R.rt.depth);
        if (v && v->aspect != VK_IMAGE_ASPECT_COLOR_BIT) {
            const view_t *dv = R.rt.has_dview ? &R.rt.dview : NULL;
            uint32_t lv = dv && dv->set_levels ? dv->base_level : 0, ly = dv && dv->set_layers ? dv->min_layer : 0;
            if (layered) ly += g_lbase;
            uint32_t nl = layered ? rt_layers(v, dv, lv, ly) : 1;
            g_view_slice = 1; VkImageView iv = view_get(v, nl > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D, lv, 1, ly, nl, v->aspect, NULL); g_view_slice = 0;
            if (iv && nl < layers) layers = nl;
            if (iv) { k.depth = v->fmt; k.samples = v->samples; dvw = iv; nv++; P.dv = v; rt_note(v); uint32_t lw = v->w >> lv ? v->w >> lv : 1, lh = v->h >> lv ? v->h >> lv : 1; if (lw < w) w = lw; if (lh < h) h = lh; }
        }
    }
    if (!nv || w == UINT32_MAX) { RED_ONCE(84, "render targets without an image"); lsnap_drop_all(); return 0; }
    if (layers == UINT32_MAX) layers = 1;   // only BC shadows: one layer
    if (k.ncolor > 1 && dup_slots_on()) dup_slots(&k, cvw);   // before slot 0 is filled: a copy left unused there leaves slot 0 empty
    if (k.ncolor && slot0_fill_on()) {   // no colour slot holds a target: the pass gets none, so it has no empty slot 0 (slot0_fill_on)
        uint32_t i = 0; while (i < k.ncolor && !cvw[i]) i++;
        if (i == k.ncolor) { k.ncolor = 0; g_slot0_none++; }
    }
    if (!cvw[0] && k.ncolor > 1 && slot0_fill_on()) {
        int used = 0; for (uint32_t i = 1; i < k.ncolor; i++) used |= cvw[i] != VK_NULL_HANDLE;
        if (used && (layers > 1 || k.samples > 1)) RED_ONCE(153, "a layered or multisample render pass with colour slot 0 empty below a used slot: slot 0 left empty (slot0_fill_on)");
        vtex *sv = used && layers == 1 && k.samples == 1 ? slot0_scratch(w, h) : NULL;
        VkImageView iv = sv ? view_get(sv, VK_IMAGE_VIEW_TYPE_2D, 0, 1, 0, 1, VK_IMAGE_ASPECT_COLOR_BIT, NULL) : VK_NULL_HANDLE;
        if (iv) { cvw[0] = iv; k.color[0] = sv->fmt; nv++; g_slot0_fills++; }
    }
    layer_pad(&k, cvw, w, h, layers);   // after slot 0 is filled: the slots it adds come after the game's
    nv = 0; for (uint32_t i = 0; i < k.ncolor; i++) if (cvw[i]) views[nv++] = cvw[i];
    if (dvw) views[nv++] = dvw;
    VkRenderPass rp = rp_get(&k); if (!rp) { lsnap_drop_all(); return 0; }
    struct { VkImageView v[9]; uint32_t w, h, l; } fk; memset(&fk, 0, sizeof fk); memcpy(fk.v, views, sizeof views); fk.w = w; fk.h = h; fk.l = layers;
    VkFramebuffer fb = nvn_hm_get(&g_fbs, &fk);
    if (!fb) {
        VkFramebufferCreateInfo fc = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp, .attachmentCount = nv, .pAttachments = views, .width = w, .height = h, .layers = layers};
        if (vkCreateFramebuffer(g_vk.dev, &fc, NULL, &fb) != VK_SUCCESS) { RED_ONCE(85, "framebuffer %ux%u failed", w, h); lsnap_drop_all(); return 0; }
        nvn_hm_put(&g_fbs, &fk, fb);
    }
    P.rp = rp; P.key = k; P.w = w; P.h = h; P.layers = layers; P.fb = fb; P.layered = layered; if (layers > 1) g_layered_passes++;
    return 1;
}
static int rp_prepare(void) { return rp_prepare_(0); }
static void copied_depth_pass(VkCommandBuffer c);   // gfx_vk_dscopy.inc
static int rp_start(void) {
    if (P.active) return 1;
    VkCommandBuffer c = cb(); if (!c) return 0;
    VkRenderPassBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = P.rp, .framebuffer = P.fb, .renderArea = {{0, 0}, {P.w, P.h}}};
    if (prof_on()) prof_pass_begin();
    vkCmdBeginRenderPass(c, &bi, VK_SUBPASS_CONTENTS_INLINE); bf_reset(); crumb('p', NULL, NULL, P.w, P.h);
    copied_depth_pass(c);   // a depth target a copy wrote: a draw Qualcomm's driver needs first (gfx_vk_dscopy.inc)
    P.active = 1; g_passes++;
    return 1;
}
// a completed chunk's draw resources (the completion thread, after its fence, before busy = 0): arena blocks kept for reuse, texel buffer views destroyed, and its descriptor
// sets free again for its next recording (ds_chunk_done; DM_VK_SET_RECYCLE=0: its pools reset)
static void ds_chunk_done(chunk *c);
static void draw_chunk_reset(chunk *c) {
    for (int i = 0; i < c->nar; i++) c->ar[i].used = 0; c->ar_cur = 0;
    if (ds_recycle()) ds_chunk_done(c);
    else { for (int i = 0; i < c->ndp; i++) vkResetDescriptorPool(g_vk.dev, c->dp[i], 0); c->dp_cur = 0; c->ndsc = 0; c->nbcd = 0; c->nups = 0; }   // the reset freed the copies' and decodes' sets too
    c->dsc_used = 0; c->bcd_used = 0; c->ups_used = 0;   // the depth-stencil copies' sets (gfx_vk_dscopy.inc), the BC decodes' (gfx_vk_bcdec.inc) and the upscaler's (gfx_vk_upsq.inc), free again
    for (int i = 0; i < c->nbv; i++) vkDestroyBufferView(g_vk.dev, c->bv[i], NULL); c->nbv = 0;
}

// ---------------- Pipelines ----------------
static VkFormat vtx_fmt(int f);
// Vertex attributes as Maxwell's fetch hands them to the shader: each component a 32-bit register, a float-type format (unorm / snorm / half / float, 10:10:10:2,
// 11:11:10) converted to float32 bits, an integer one widened by its own signedness, a *_UI2F / *_I2F one (integers read as their float value) converted to float; missing
// components 0, 0, 0 and 1 (1.0 for the float types). The shader declares each input float, uint or int (res.json "vin": spv_android.py; tools/spv_build.py makes
// integer-only inputs uint, as dmsh_build.py int_inputs). An attribute is bound as it is when the format's kind matches the input and the GPU takes it as a vertex format;
// otherwise its register bits are made on the CPU as the vertex data is copied, into a binding of its own (16 + attribute) read as R32G32B32A32 of the input's type
// (nvn_mtl.m vfmt_for_ct / vconv: the same cases on Metal). The Adreno 619 takes no scaled format at all; a skinning shader reading bone indices declared float stretched
// characters into streaks.
// NVN's vertex formats: n components, the component kind (0 unorm 1 snorm 2 uint 3 sint 4 float, 5 uint-to-float 6 sint-to-float, 7 / 8 / 9 / 10 RGB10A2 unorm / snorm /
// uint / sint, 11 / 12 RGB10A2 uint / sint to float, 13 R11G11B10F) and its bits (8 / 16 / 32); 0 = not one
static int vfmt_desc(int f, int *kind, int *bits) {
    static const int s8[4] = {0x01, 0x0D, 0x19, 0x25}, s16[4] = {0x05, 0x11, 0x1D, 0x29}, s32[4] = {0x0A, 0x16, 0x22, 0x2E}, sc[4] = {0x51, 0x57, 0x5D, 0x63};
    for (int n = 0; n < 4; n++) {
        if (f >= s8[n] && f < s8[n] + 4) { static const int k[4] = {0, 1, 2, 3}; *kind = k[f - s8[n]]; *bits = 8; return n + 1; }
        if (f >= s16[n] && f < s16[n] + 5) { static const int k[5] = {4, 0, 1, 2, 3}; *kind = k[f - s16[n]]; *bits = 16; return n + 1; }
        if (f >= s32[n] && f < s32[n] + 3) { static const int k[3] = {4, 2, 3}; *kind = k[f - s32[n]]; *bits = 32; return n + 1; }
        if (f >= sc[n] && f < sc[n] + 6) { int i = f - sc[n]; *kind = 5 + (i & 1); *bits = 8 << (i / 2); return n + 1; }
    }
    switch (f) {
        case 0x3D: *kind = 7; break; case 0x69: *kind = 8; break; case 0x3E: *kind = 9; break; case 0x6A: *kind = 10; break;
        case 0x6B: *kind = 11; break; case 0x6C: *kind = 12; break; case 0x3F: *kind = 13; *bits = 32; return 3;
        default: return 0;
    }
    *bits = 32; return 4;
}
static char vfmt_class(int kind) { return kind == 2 || kind == 9 ? 'u' : kind == 3 || kind == 10 ? 's' : 'f'; }   // what the register holds
static float half_f(uint16_t h) { uint32_t s = (uint32_t)(h >> 15) << 31, e = h >> 10 & 31, m = h & 1023, u;
    if (!e) { if (!m) u = s; else { e = 113; while (!(m & 1024)) { m <<= 1; e--; } m &= 1023; u = s | e << 23 | m << 13; } }
    else if (e == 31) u = s | 0x7f800000u | m << 13; else u = s | (e + 112) << 23 | m << 13;
    float f; memcpy(&f, &u, 4); return f; }
static float uf_small(uint32_t v, int mbits) { uint32_t e = v >> mbits, m = v & ((1u << mbits) - 1); return !e ? ldexpf((float)m, -14 - mbits) : e == 31 ? INFINITY : ldexpf(1.f + (float)m / (float)(1u << mbits), (int)e - 15); }
static void vfmt_regs(int kind, int bits, int n, const uint8_t *src, uint32_t out[4]) {   // one attribute element → its four registers
    float one = 1.f; uint32_t onef; memcpy(&onef, &one, 4);
    int ftype = vfmt_class(kind) == 'f';
    out[0] = out[1] = out[2] = 0; out[3] = ftype ? onef : 1;
    for (int c = 0; c < n; c++) {
        float fv = 0; int64_t iv = 0; int isf = 1;
        if (kind >= 7 && kind <= 12) {   // RGB10A2
            uint32_t v; memcpy(&v, src, 4); int b = c == 3 ? 2 : 10; uint32_t u = v >> (10 * c) & ((1u << b) - 1); int32_t sv = (int32_t)(u << (32 - b)) >> (32 - b);
            if (kind == 7) fv = (float)u / (float)((1u << b) - 1); else if (kind == 8) { fv = (float)sv / (float)((1 << (b - 1)) - 1); if (fv < -1) fv = -1; }
            else if (kind == 9) { iv = u; isf = 0; } else if (kind == 10) { iv = sv; isf = 0; } else fv = kind == 11 ? (float)u : (float)sv;
        } else if (kind == 13) {   // R11G11B10F
            uint32_t v; memcpy(&v, src, 4); fv = c == 2 ? uf_small(v >> 22, 5) : uf_small(v >> (11 * c) & 0x7ff, 6);
        } else {
            uint64_t u = 0; memcpy(&u, src + c * bits / 8, (size_t)bits / 8); int64_t sv = bits == 64 ? (int64_t)u : (int64_t)(u << (64 - bits)) >> (64 - bits);
            switch (kind) {
                case 0: fv = (float)u / (float)((1ull << bits) - 1); break;
                case 1: fv = (float)sv / (float)((1ull << (bits - 1)) - 1); if (fv < -1) fv = -1; break;
                case 2: iv = (int64_t)u; isf = 0; break; case 3: iv = sv; isf = 0; break;
                case 4: if (bits == 16) fv = half_f((uint16_t)u); else { uint32_t w = (uint32_t)u; memcpy(&fv, &w, 4); } break;
                case 5: fv = (float)u; break; case 6: fv = (float)sv; break;
            }
        }
        if (isf) memcpy(&out[c], &fv, 4); else out[c] = (uint32_t)iv;
    }
}
// whether attribute format f can be bound as it is for a shader input of type t ('f' / 'u' / 's'): its kind matches and the GPU takes the Vulkan format as a vertex format
static int vfmt_direct_(int f, char t);
static int vfmt_direct(int f, char t) {   // cached: the driver's format query took 5% of the render thread when asked for every attribute of every draw
    static int8_t memo[256][3]; int ti = t == 'u' ? 1 : t == 's' ? 2 : 0;
    if (f < 0 || f >= 256) return vfmt_direct_(f, t);
    if (!memo[f][ti]) memo[f][ti] = vfmt_direct_(f, t) ? 1 : -1;
    return memo[f][ti] > 0;
}
static int vfmt_direct_(int f, char t) {
    int kind, bits, n = vfmt_desc(f, &kind, &bits);
    if (!n || kind == 5 || kind == 6 || kind == 11 || kind == 12) return 0;   // integers read as float: no vertex format here
    if (vfmt_class(kind) != t) return 0;
    VkFormat vf = vtx_fmt(f); if (!vf) return 0;
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(g_vk.pd, vf, &fp);
    return (fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) != 0;
}
static VkFormat vtx_fmt(int f) {   // an NVNformat as a vertex attribute
    static const uint8_t k8[4] = {0, 1, 4, 5}, k16[5] = {6, 0, 1, 4, 5};   // offsets in Vulkan's per-width order: UNORM SNORM USCALED SSCALED UINT SINT (SFLOAT at 6 for 16-bit)
    static const int b8[4] = {9, 16, 23, 37}, b16[4] = {70, 77, 84, 91}, b32[4] = {98, 101, 104, 107};
    for (int n = 0; n < 4; n++) {
        static const int s8[4] = {0x01, 0x0D, 0x19, 0x25}, s16[4] = {0x05, 0x11, 0x1D, 0x29}, s32[4] = {0x0A, 0x16, 0x22, 0x2E}, sc[4] = {0x51, 0x57, 0x5D, 0x63};
        if (f >= s8[n] && f < s8[n] + 4) return (VkFormat)(b8[n] + k8[f - s8[n]]);
        if (f >= s16[n] && f < s16[n] + 5) return (VkFormat)(b16[n] + k16[f - s16[n]]);
        if (f >= s32[n] && f < s32[n] + 3) return (VkFormat)(b32[n] + (f - s32[n] == 0 ? 2 : f - s32[n] - 1));
        if (f >= sc[n] && f < sc[n] + 6) { int i = f - sc[n], width = i / 2, sg = i & 1;   // *_UI2F / *_I2F: the scaled formats; 32-bit ones read as float of the integer (UINT / SINT taken as is)
            if (width == 0) return (VkFormat)(b8[n] + 2 + sg); if (width == 1) return (VkFormat)(b16[n] + 2 + sg); return (VkFormat)(b32[n] + sg); }
    }
    switch (f) {
        case 0x3D: return VK_FORMAT_A2B10G10R10_UNORM_PACK32; case 0x3E: return VK_FORMAT_A2B10G10R10_UINT_PACK32; case 0x3F: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
        case 0x69: return VK_FORMAT_A2B10G10R10_SNORM_PACK32; case 0x6A: return VK_FORMAT_A2B10G10R10_SINT_PACK32; case 0x6B: return VK_FORMAT_A2B10G10R10_USCALED_PACK32;
        case 0x6C: return VK_FORMAT_A2B10G10R10_SSCALED_PACK32; case 0x99: case 0x9A: return VK_FORMAT_B8G8R8A8_UNORM;
        default: return VK_FORMAT_UNDEFINED;
    }
}
static VkPrimitiveTopology topo_of(int p) {
    switch (p) {
        case 0: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST; case 1: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST; case 2: case 3: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case 5: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; case 6: case 9: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
        case 10: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY; case 11: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;
        case 12: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY; case 13: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
        case 14: return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
        default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;   // triangles, quads (made triangles at the draw)
    }
}
static const VkBlendFactor BF[0x65] = {[0x1] = VK_BLEND_FACTOR_ZERO, [0x2] = VK_BLEND_FACTOR_ONE, [0x3] = VK_BLEND_FACTOR_SRC_COLOR, [0x4] = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
    [0x5] = VK_BLEND_FACTOR_SRC_ALPHA, [0x6] = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, [0x7] = VK_BLEND_FACTOR_DST_ALPHA, [0x8] = VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
    [0x9] = VK_BLEND_FACTOR_DST_COLOR, [0xA] = VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, [0xB] = VK_BLEND_FACTOR_SRC_ALPHA_SATURATE, [0x10] = VK_BLEND_FACTOR_SRC1_COLOR,
    [0x11] = VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR, [0x12] = VK_BLEND_FACTOR_SRC1_ALPHA, [0x13] = VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA, [0x61] = VK_BLEND_FACTOR_CONSTANT_COLOR,
    [0x62] = VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR, [0x63] = VK_BLEND_FACTOR_CONSTANT_ALPHA, [0x64] = VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA};
static VkBlendFactor bfac(unsigned v, VkBlendFactor def) { return v && v < 0x65 && (v == 1 || BF[v]) ? BF[v] : def; }
static VkBlendOp bop(unsigned v) { return v >= 1 && v <= 5 ? (VkBlendOp)(v - 1) : VK_BLEND_OP_ADD; }
static VkCompareOp cmpop(unsigned v, VkCompareOp def) { return v >= 1 && v <= 8 ? (VkCompareOp)(v - 1) : def; }
static VkStencilOp stop(unsigned v) { return v >= 1 && v <= 8 ? (VkStencilOp)(v - 1) : VK_STENCIL_OP_KEEP; }
typedef struct {
    vshader *sh[4]; int z, tf; VkRenderPass rp; rp_key rk; uint32_t ncolor;   // tf: 1 the capture pipeline (the vertex shader's .tf.spv, no rasterization; or its wrapper and modtff, points rasterized), 2 a discarded draw's that writes (its own modules, no rasterization: rd_key); rk: the pass's formats and samples
    uint32_t attr[16]; vs_st stream[16]; uint8_t topo, restart, depth_clamp, ll;
    color_st color; chmask_st chmask; blend_st blend[8]; ms_st ms; poly_st poly; ds_st ds;
} pipe_key;
// the key's fields by name, for the logs that say which fields set two keys apart (pipe_get, pso_memo_note); the shaders and the pass are compared apart
static const struct { const char *n; size_t o, s; } PK_F[] = {{"z", offsetof(pipe_key, z), sizeof(int)}, {"tf", offsetof(pipe_key, tf), sizeof(int)}, {"rk", offsetof(pipe_key, rk), sizeof(rp_key)},
    {"ncolor", offsetof(pipe_key, ncolor), 4}, {"attr", offsetof(pipe_key, attr), sizeof(((pipe_key *)0)->attr)}, {"stream", offsetof(pipe_key, stream), sizeof(((pipe_key *)0)->stream)},
    {"topo", offsetof(pipe_key, topo), 1}, {"restart", offsetof(pipe_key, restart), 1}, {"depth_clamp", offsetof(pipe_key, depth_clamp), 1}, {"ll", offsetof(pipe_key, ll), 1},
    {"color", offsetof(pipe_key, color), sizeof(color_st)}, {"chmask", offsetof(pipe_key, chmask), sizeof(chmask_st)}, {"blend", offsetof(pipe_key, blend), sizeof(((pipe_key *)0)->blend)},
    {"ms", offsetof(pipe_key, ms), sizeof(ms_st)}, {"poly", offsetof(pipe_key, poly), sizeof(poly_st)}, {"ds", offsetof(pipe_key, ds), sizeof(ds_st)}};
typedef struct { VkPipeline p; VkPipelineLayout pl; int alpha; VkShaderStageFlags lbpc; } pipe_t;   // alpha: the fragment module is spv_alpha.h's, and the draw sets its reference (a push
                                                                                              // constant); lbpc: the stages whose modules read the layer base (spv_fit_lbase), which the draw sets
static nvn_hmap g_pipes = {.klen = sizeof(pipe_key)};
static int is_int_fmt(VkFormat f);   // below: an integer colour format, which takes no alpha test (as in OpenGL)
// Whether this draw runs NVN's alpha test: a fragment shader, a function other than ALWAYS, and no integer colour 0. A depth-only pass runs it too (shadow maps of
// foliage rely on it, as on Metal): the test reads what the shader writes to colour 0 whether or not the pass keeps it
static int alpha_test_on(const pipe_key *k) {
    return k->sh[1] && !k->tf && k->color.alpha_func && k->color.alpha_func != NVN_ALPHA_FUNC_ALWAYS && !(k->ncolor && is_int_fmt(k->rk.color[0]));
}
// The fragment shader's module with the alpha test in it (spv_alpha.h): one per shader and inputs variant, its function set at each pipeline (VkSpecializationInfo,
// SpecId 0) and the reference a push constant set at each alpha-tested draw, so one module serves every function and reference. A shader whose earlier stage
// leaves inputs unwritten is transformed first (spv_drop_inputs), then the alpha test (lm_code). NULL: the transform does not apply (colour 0 is no vec4 of
// float), the draw keeps the shader as it is, and that is remembered so no pipeline retries it
static pthread_mutex_t g_alpha_mu = PTHREAD_MUTEX_INITIALIZER;   // one variant per shader and inputs variant, made here and in the prewarm thread (pipe_prep) outside the replay's lock
static lmod *ps_alpha_mod_locked(vshader *s, uint32_t miss) {
    for (int i = 0; i < s->navar; i++) if (s->avar[i].miss == miss) return s->avar[i].m;
    char p[700]; snprintf(p, sizeof p, "%s/%s.spv", g_spv_dir, s->key);
    lmod *m = lm_new_a(p, miss, 1, s->bmap, NULL);   // the code goes through spv_alpha.h (lm_code), and its identifier is of the test in it
    if (s->navar < 4) { s->avar[s->navar].miss = miss; s->avar[s->navar++].m = m; }
    if (m) { static int nl; if (nl++ < 8) LOG("[vk] fragment shader %s: alpha-test variant (its function a specialization constant, the reference a push constant)", s->key); }
    return m;
}
static lmod *ps_alpha_mod(vshader *s, uint32_t miss) {   // the cache and the module are one thread's at a time (pipeline creation, the prewarm thread)
    pthread_mutex_lock(&g_alpha_mu);
    lmod *m = ps_alpha_mod_locked(s, miss);
    pthread_mutex_unlock(&g_alpha_mu);
    return m;
}
// The fragment shader's module: the alpha-test variant (spv_alpha.h), and a variant when the earlier stage leaves inputs unwritten (spv_drop_inputs). *alpha is 1
// when the alpha variant is the one returned; the caller sets the function's specialization then
static lmod *ps_variant_prim(vshader *s, uint32_t miss, uint32_t prim);   // gfx_vk_mesh.inc
// the stage before the fragment shader: the geometry shader (a task shader in slot 2 comes before the mesh shader), else the vertex or mesh shader
static const vshader *pipe_ps_prev(const pipe_key *k) { return k->sh[2] && k->sh[2]->stage == SH_GS ? k->sh[2] : k->sh[0]; }
static lmod *pipe_ps_mod(const pipe_key *k, vshader *s, int *alpha) {
    const vshader *pv = pipe_ps_prev(k); uint32_t miss = pv ? s->io_in & ~pv->io_out : 0;
    *alpha = 0;
    if (pv && pv->stage == SH_MS) {   // after a mesh shader: the inputs it writes per primitive (ps_variant_prim), and no alpha test: a mesh pipeline's push constant holds the draw's first workgroup (pipe_make)
        if (alpha_test_on(k)) RED_ONCE(165, "fragment shader %s after mesh shader %s: the game's alpha test (function %u) is not run in a task / mesh pipeline", s->key, pv->key, (unsigned)k->color.alpha_func);
        uint32_t prim = s->io_in & pv->io_prim & ~miss; return prim ? ps_variant_prim(s, miss, prim) : miss ? ps_variant(s, miss) : s->mod; }
    if (alpha_test_on(k)) { lmod *m = ps_alpha_mod(s, miss); if (m) { *alpha = 1; return m; } }
    return miss ? ps_variant(s, miss) : s->mod;
}
// ---- Pipelines made again at start ----
// Creating a pipeline costs the Adreno driver about 2 ms (a shader compile; less when its pipeline cache has the binary): Super Mario Bros. Wonder made 122 in one
// frame entering a level (222 ms) and a few every time a new effect showed up. Every pipeline made is appended to <DM_PIPELINE_CACHE>.keys (its shaders' keys, the
// pass's formats and the rest of the key, which is plain state), and a thread makes the listed ones again when the app starts, one at a time under the replay's lock
// (between the game's submissions), so a later session meets them made
typedef struct { uint32_t magic, size; char sh[4][17]; } pk_head;
enum { PK_MAGIC = 0x50574b34 };   // 'PWK4': keys normalized (vk_draw_): blend enables, polygon offset and the state objects' unused bits too
static FILE *g_pk_out;
static nvn_hmap g_pk_listed;   // the records in the list (whole records), so a session appends only pipelines the list does not have yet (it used to grow by thousands of repeats)
static size_t pk_tail_off(void) { return offsetof(pipe_key, ncolor); }
enum { PK_TAIL = sizeof(pipe_key) - offsetof(pipe_key, ncolor), PK_REC = sizeof(pk_head) + 4 + sizeof(rp_key) + PK_TAIL };
static void pk_rec(const pipe_key *k, uint8_t *r) {   // a list record: head, z, the pass's key, the plain tail of the key
    pk_head h; memset(&h, 0, sizeof h); h.magic = PK_MAGIC; h.size = PK_TAIL;
    for (int i = 0; i < 4; i++) if (k->sh[i]) memcpy(h.sh[i], k->sh[i]->key, 17);
    int32_t z = k->z; memcpy(r, &h, sizeof h); memcpy(r + sizeof h, &z, 4); memcpy(r + sizeof h + 4, &k->rk, sizeof k->rk);
    memcpy(r + sizeof h + 4 + sizeof k->rk, (const uint8_t *)k + pk_tail_off(), PK_TAIL);
}
static FILE *pk_open(const char *mode) {   // <DM_PIPELINE_CACHE>.keys4 (earlier lists, .keys2 and .keys3, are removed: their keys are not made any more)
    const char *c = getenv("DM_PIPELINE_CACHE"); if (!c) return NULL; char p[700];
    static int old; if (!old++) for (int v = 2; v <= 3; v++) { snprintf(p, sizeof p, "%s.keys%d", c, v); remove(p); }
    snprintf(p, sizeof p, "%s.keys4", c); return fopen(p, mode); }
// The guard against an entry whose compile never finishes (pk_guard.h): the marker <cache>.making4, which only the prewarm thread writes, and the skip list <cache>.skip4,
// read once (pk_guard, before the first pipeline of the session) and read-only after
static int g_pkg_fd = -1;
static _Atomic int g_pkg_tid;   // the prewarm thread while it compiles a list entry, so the crash dump flags the marker (pb_dump)
static nvn_hmap g_pk_skip;
static pthread_once_t g_pkg_once = PTHREAD_ONCE_INIT;
static void pk_guard_init(void) {
    const char *c = getenv("DM_PIPELINE_CACHE"); if (!c) return;
    char mk[700], sk[700]; snprintf(mk, sizeof mk, "%s.making4", c); snprintf(sk, sizeof sk, "%s.skip4", c);
    uint8_t r[PK_REC]; int crashed; pk_head h;
    int took = pkg_take(mk, r, PK_REC, &crashed) && (memcpy(&h, r, sizeof h), h.magic == PK_MAGIC && h.size == PK_TAIL);
    if (took) {   // an earlier start ended during this entry's compile
        int32_t z; memcpy(&z, r + sizeof h, 4); for (int j = 0; j < 4; j++) h.sh[j][16] = 0;
        pkg_skip_add(sk, r, PK_REC);
        LOG("[RED] [vk] start-up list: the entry with shaders %s %s %s %s%s %s, so it is left out from now on (%s)", h.sh[0][0] ? h.sh[0] : "-", h.sh[1][0] ? h.sh[1] : "-",
            h.sh[2][0] ? h.sh[2] : "-", h.sh[3][0] ? h.sh[3] : "-", z ? " (vertex .z variant)" : "",
            crashed ? "crashed the driver's shader compiler at an earlier start" : "was being compiled when an earlier start ended (a crash, a kill or a stop)", sk);
    }
    // the second start in a row that crashed inside the list: the whole list set aside (pk_guard.h), before anything of this session reads or appends to it
    char st[700]; snprintf(st, sizeof st, "%s.strikes4", c);
    if (pkg_strikes(st, took && crashed) >= 2) {
        char ls[700], as[708]; snprintf(ls, sizeof ls, "%s.keys4", c); snprintf(as, sizeof as, "%s.aside", ls);
        struct stat lst; long ent = stat(ls, &lst) ? -1 : (long)(lst.st_size / PK_REC);
        if (pkg_set_aside(ls, as)) LOG("[RED] [vk] start-up list: two starts in a row crashed in it, so the whole list (%ld entries) is set aside as %s and this session starts a new one", ent, as);
        pkg_strikes(st, 0);
    }
    uint8_t *all; size_t n = pkg_skip_load(sk, PK_REC, &all);
    g_pk_skip.klen = PK_REC; for (size_t i = 0; i < n; i++) nvn_hm_put(&g_pk_skip, all + i * PK_REC, (void *)1);
    free(all);
    g_pkg_fd = pkg_open(mk);
}
static void pk_guard(void) { pthread_once(&g_pkg_once, pk_guard_init); }
static int pk_skipped(const uint8_t *r) { return g_pk_skip.n && nvn_hm_get(&g_pk_skip, r); }
static void pk_record(const pipe_key *k) {   // under g_bk
    if (k->tf) return;
    uint8_t r[PK_REC]; pk_rec(k, r);
    if (pk_skipped(r)) return;   // not listed again when play makes it
    if (!g_pk_listed.klen) g_pk_listed.klen = PK_REC;
    if (nvn_hm_get(&g_pk_listed, r)) return;
    nvn_hm_put(&g_pk_listed, r, (void *)1);
    if (!g_pk_out) { static int tried; if (tried++) return; g_pk_out = pk_open("ab"); if (!g_pk_out) return; }
    fwrite(r, PK_REC, 1, g_pk_out); fflush(g_pk_out);
}
// Whether each shader of a key is the stage of its slot (vk_draw__: vertex 0, fragment 1, geometry 2; slot 3 stays empty, as tessellation draws are skipped there). sh_get makes
// every stage it does not draw (task, mesh, tessellation) SH_TS with the vertex stage bit, and a draw with one is skipped before it has a key; but the start-up list makes its
// entries from their shader keys alone, and an entry a build that drew mesh shaders had written put Ocarina of Time's mesh shader 0c1ffecd02018fb0 in the vertex slot. Made as
// a vertex stage, that module crashed the Adreno 830's compiler (libllvm-qgl.so) a few seconds into every start on a OnePlus Pad 3
static const char *sh_stage_name(int st) { return st == SH_VS ? "vertex" : st == SH_PS ? "fragment" : st == SH_GS ? "geometry" : st == SH_CS ? "compute" : st == SH_MS ? "mesh" : st == SH_TK ? "task" : "tessellation"; }
static const char *const PK_SLOT[4] = {"vertex", "fragment", "geometry", "fourth (unused)"};
static int pk_stages_ok(const pipe_key *k, int *slot) {
    static const int want[4] = {SH_VS, SH_PS, SH_GS, -1};
    int mesh = g_mesh && k->sh[0] && k->sh[0]->stage == SH_MS;   // a task / mesh pipeline (gfx_vk_mesh.inc): the mesh shader in the vertex slot, a task shader in the geometry slot
    for (int i = 0; i < 4; i++) if (k->sh[i] && k->sh[i]->stage != (mesh && i == 0 ? SH_MS : mesh && i == 2 ? SH_TK : want[i])) { *slot = i; return 0; }
    return 1;
}
static pipe_t *pipe_get(const pipe_key *k);
static VkResult pipe_make(const pipe_key *k, pipe_t *pp, uint32_t *ons, uint32_t *onva, VkPrimitiveTopology *otp);
static void pipe_prep(const pipe_key *k);
static VkRenderPass rp_get(const rp_key *k);
// The list is read whole first (repeats dropped, the file written again without them when it had any), then made one pipeline at a time at a low priority: the shaders and
// the pass under the replay's lock, the compile without it (pipe_make). Compiling under the lock kept the game's thread waiting for seconds in a row (menus at 1-2 s a frame),
// and later, with pauses between pipelines, still for part of every frame in a level while the list ran (10,000 pipelines and more)
static void *prewarm_thread(void *arg) {
    (void)arg; pthread_setname_np(pthread_self(), "vk-prewarm"); setpriority(PRIO_PROCESS, (id_t)gettid(), 10);
    pk_guard();
    FILE *f = pk_open("rb"); if (!f) return NULL;
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_MONOTONIC); int made = 0, had = 0, bad = 0, n = 0, rep = 0, wrong = 0, skip = 0;
    uint8_t *all = NULL; size_t cnt = 0, cap = 0, rec = PK_REC;
    pthread_mutex_lock(&g_bk); if (!g_pk_listed.klen) g_pk_listed.klen = PK_REC; pthread_mutex_unlock(&g_bk);
    for (;;) {
        uint8_t r[PK_REC]; pk_head h;
        if (fread(r, rec, 1, f) != 1) break;
        memcpy(&h, r, sizeof h); if (h.magic != PK_MAGIC || h.size != PK_TAIL) break;
        n++;
        if (pk_skipped(r)) { skip++; continue; }   // an earlier start ended during its compile (pk_guard_init): dropped from the list below
        pthread_mutex_lock(&g_bk); int seen = nvn_hm_get(&g_pk_listed, r) != NULL; if (!seen) nvn_hm_put(&g_pk_listed, r, (void *)1); pthread_mutex_unlock(&g_bk);
        if (seen) { rep++; continue; }
        if (cnt == cap) { cap = cap ? cap * 2 : 1024; all = realloc(all, cap * rec); }
        memcpy(all + cnt++ * rec, r, rec);
    }
    fclose(f);
    if (rep || skip) {   // written again without the repeats and the skipped (records this session appended meanwhile are in g_pk_listed and the new file)
        pthread_mutex_lock(&g_bk);
        if (g_pk_out) { fclose(g_pk_out); g_pk_out = NULL; }
        FILE *o = pk_open("wb"); if (o) { fwrite(all, rec, cnt, o); fclose(o); }
        g_pk_out = pk_open("ab");
        pthread_mutex_unlock(&g_bk);
    }
    // At most the list's first PREWARM_MAX (DM_VK_PREWARM overrides): what every session meets early (menus, the map, common effects). The whole list of a Super Mario Bros.
    // Wonder save grew past 34,000 pipelines over a few sessions; making them all took the phone's two fast cores and about a gigabyte for most of an hour of play
    size_t lim = 3000; { const char *e = getenv("DM_VK_PREWARM"); if (e) lim = (size_t)strtoul(e, NULL, 10); } if (cnt > lim) cnt = lim;
    for (size_t i = 0; i < cnt; i++) {
        const uint8_t *r = all + i * rec; pk_head h; int32_t z; rp_key rk;
        memcpy(&h, r, sizeof h); memcpy(&z, r + sizeof h, 4); memcpy(&rk, r + sizeof h + 4, sizeof rk);
        // the marker holds the entry from its shader modules (sh_get, pipe_prep) to the end of its compile (pk_guard.h)
        atomic_store(&g_pkg_tid, gettid()); pkg_begin(g_pkg_fd, r, PK_REC);
        pthread_mutex_lock(&g_bk);   // the shaders, the pass and what the pipeline shares (pipe_prep), under the lock; the compile without it
        pipe_key k; memset(&k, 0, sizeof k); int ok = 1, mk = 0;
        for (int j = 0; j < 4 && ok; j++) if (h.sh[j][0]) { h.sh[j][16] = 0; k.sh[j] = sh_get(h.sh[j]); if (!k.sh[j]) ok = 0; }
        int slot, out = 0;   // out: left out without a continue, so the marker is emptied below (pkg_end)
        if (ok && !pk_stages_ok(&k, &slot)) {   // an entry the replay would never make for a draw (pk_stages_ok)
            if (!wrong++) LOG("[RED] [vk] start-up list: an entry with %s shader %s in the %s slot is left out (the replay makes no such pipeline; a build that drew those shaders wrote it)",
                              sh_stage_name(k.sh[slot]->stage), k.sh[slot]->key, PK_SLOT[slot]);
            out = 1; }
        if (ok && !out) { k.z = z; k.rk = rk; k.rp = rp_get(&rk); memcpy((uint8_t *)&k + pk_tail_off(), r + sizeof h + 4 + sizeof rk, PK_TAIL); if (!k.rp) ok = 0; }
        if (!out) { if (!ok) bad++; else if (nvn_hm_get(&g_pipes, &k)) had++; else { pipe_prep(&k); mk = 1; } }
        pthread_mutex_unlock(&g_bk);
        VkResult pr = VK_SUCCESS; pipe_t *pp = NULL; uint32_t ns, nva; VkPrimitiveTopology tp;
        if (mk) { pp = calloc(1, sizeof *pp); pr = pp ? pipe_make(&k, pp, &ns, &nva, &tp) : VK_ERROR_OUT_OF_HOST_MEMORY; }
        pkg_end(g_pkg_fd); atomic_store(&g_pkg_tid, 0);
        if (!mk) continue;
        pthread_mutex_lock(&g_bk);
        if (pr == VK_SUCCESS && !nvn_hm_get(&g_pipes, &k)) { nvn_hm_put(&g_pipes, &k, pp); made++; pp = NULL; }
        else if (pr == VK_SUCCESS) had++;   // the game made it meanwhile
        else bad++;
        pthread_mutex_unlock(&g_bk);
        if (pp) { if (pp->p) vkDestroyPipeline(g_vk.dev, pp->p, NULL); if (pp->pl) vkDestroyPipelineLayout(g_vk.dev, pp->pl, NULL); free(pp); }
        usleep(1000);
    }
    free(all);
    LOG("[vk] pipelines made again from the list: %d made, %d there already, %d failed, %d left out (a shader in the wrong slot), of the first %zu of %d records (%d repeats "
        "and %d skipped dropped: an earlier start ended in their compile), in %.1f s", made, had, bad, wrong, cnt, n, rep, skip, (clock_gettime_nsec_np(CLOCK_MONOTONIC) - t0) / 1e9);
    // the cache was written only when the app paused: a session that ended otherwise (killed for an update or by the system) lost what the list compiled, and the next start
    // compiled it all again (Super Mario Bros. Wonder on the Adreno 619: about 23 ms a pipeline from the stale cache, 7 from a saved one)
    if (made) vkb_pipeline_cache_save();
    return NULL;
}
static void prewarm_start(void) { static int started; if (started++) return; pthread_t th; if (!pthread_create(&th, NULL, prewarm_thread, NULL)) pthread_detach(th); }
static VkDescriptorSetLayout g_empty_dsl;
// Pipelines being built, for a crash dump (crash.c crash_set_gfx_hook): Brilliant Diamond on the Retroid Pocket 5 crashes in the Adreno 650 driver's shader compiler
// (libllvm-qgl.so) while it builds its first pipelines (baselines problem 2), and nothing said which shaders. Each build takes a slot for its compile (graphics in pipe_make, which
// the replay, the start-up list and the background builds call; compute in cpipe_get) and gives it back after; the dump prints every slot in use, the crashing thread's marked.
// Shader keys are the translator's bundle keys (vshader key), the same as the pass dumps and the res.json files name
enum { PB_MAX = 8 };
static struct { _Atomic int tid; const char *sh[4]; char kind; uint8_t topo, ncolor, z; uint32_t miss; } g_pb[PB_MAX];
static int pb_begin(char kind, const char *const sh[4], uint8_t topo, uint8_t ncolor, uint8_t z, uint32_t miss) {
    int me = gettid();
    for (int i = 0; i < PB_MAX; i++) { int none = 0;
        if (atomic_compare_exchange_strong(&g_pb[i].tid, &none, -1)) {
            for (int j = 0; j < 4; j++) g_pb[i].sh[j] = sh[j];
            g_pb[i].kind = kind; g_pb[i].topo = topo; g_pb[i].ncolor = ncolor; g_pb[i].z = z; g_pb[i].miss = miss; atomic_store(&g_pb[i].tid, me); return i; } }
    return -1;
}
static void pb_end(int i) { if (i >= 0) atomic_store(&g_pb[i].tid, 0); }
static void pb_dump(void) {   // crash.c, on the crashing thread, after its backtrace
    int me = gettid();
    if (atomic_load(&g_pkg_tid) == me) pkg_crashed(g_pkg_fd, PK_REC);   // the start-up list's entry crashed its compile: the next start says so and skips it (pk_guard_init)
    for (int i = 0; i < PB_MAX; i++) { int t = atomic_load(&g_pb[i].tid); if (t <= 0) continue;
        char tn[32] = ""; char path[64]; snprintf(path, sizeof path, "/proc/self/task/%d/comm", t);
        int fd = open(path, O_RDONLY); if (fd >= 0) { ssize_t n = read(fd, tn, sizeof tn - 1); close(fd); if (n > 0) tn[n - (tn[n - 1] == '\n')] = 0; }
        fprintf(stderr, "[dm] pipeline being built%s: thread %d \"%s\", %s, shaders %s %s %s %s%s, topology %u, %u colour targets%s\n", t == me ? " by the crashing thread" : "", t, tn,
                g_pb[i].kind == 'c' ? "compute" : "graphics", g_pb[i].sh[0] ? g_pb[i].sh[0] : "-", g_pb[i].sh[1] ? g_pb[i].sh[1] : "-", g_pb[i].sh[2] ? g_pb[i].sh[2] : "-",
                g_pb[i].sh[3] ? g_pb[i].sh[3] : "-", g_pb[i].z ? " (vertex .z variant)" : "", g_pb[i].topo, g_pb[i].ncolor, g_pb[i].miss ? " (fragment variant without some inputs)" : ""); }
}
// The pipeline of key k into pp (its layout and pipeline). Under the replay's lock from pipe_get; the start-up list (prewarm_thread) calls it without the lock after pipe_prep,
// so a compile there no longer holds the game's draws up (the driver's pipeline cache is internally synchronized)
#define MESH_PC_BYTES 16   // a task / mesh pipeline's push constant block (spv_mesh.py, gfx_vk_mesh.inc): the draw's first workgroup at 0
static VkResult pipe_make(const pipe_key *k, pipe_t *pp, uint32_t *ons, uint32_t *onva, VkPrimitiveTopology *otp) {
    int slot; if (!pk_stages_ok(k, &slot)) {   // never handed to the driver (pk_stages_ok): a module of another stage in a slot crashed a compiler
        RED_ONCE(178, "a pipeline with %s shader %s in its %s slot is not made (said once)", sh_stage_name(k->sh[slot]->stage), k->sh[slot]->key, PK_SLOT[slot]);
        return VK_ERROR_INITIALIZATION_FAILED; }
    pk_guard();
    if (g_pk_skip.n && atomic_load(&g_pkg_tid) != gettid()) {   // play needs a pipeline the start-up list skips: said, so a crash in this compile names it in the log too
        uint8_t r[PK_REC]; pk_rec(k, r);
        if (nvn_hm_get(&g_pk_skip, r)) { static _Atomic int nl; if (atomic_fetch_add(&nl, 1) < 8)
            LOG("[RED] [vk] a pipeline the start-up list skips (an earlier start ended during its compile) is compiled now for a draw: shaders %s %s; if the app ends here, the driver "
                "cannot compile it", k->sh[0] ? k->sh[0]->key : "-", k->sh[1] ? k->sh[1]->key : "-"); } }
    if (!g_empty_dsl) { VkDescriptorSetLayoutCreateInfo lc = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; vkCreateDescriptorSetLayout(g_vk.dev, &lc, NULL, &g_empty_dsl); }
    VkDescriptorSetLayout sets[SPV_NOPTR_SET + 1] = {g_empty_dsl, g_empty_dsl, g_empty_dsl, g_empty_dsl, g_empty_dsl, g_empty_dsl}; uint32_t nset = 1; int gmnp = 0;
    VkPipelineShaderStageCreateInfo st[4]; lmod *lms[4]; uint32_t ns = 0; VkSpecializationInfo psi; memset(&psi, 0, sizeof psi);
    VkSpecializationMapEntry pme[1]; uint32_t psv = 0; const vshader *pspec = NULL;
    for (int i = 0; i < 4; i++) { vshader *s = k->sh[i]; if (!s) continue;
        sets[s->set & 3] = k->tf == 1 ? s->dsl_tf : s->dsl; if ((uint32_t)(s->set & 3) + 1 > nset) nset = (uint32_t)(s->set & 3) + 1;
        lmod *mod = k->tf == 1 ? s->modtf : (s->stage == SH_VS || s->stage == SH_MS) && k->z && s->modz ? s->modz : s->mod;
        if (s->stage == SH_PS) { int alpha = 0; mod = pipe_ps_mod(k, s, &alpha);   // the alpha test (spv_alpha.h) and unwritten inputs (spv_drop_inputs)
            if (alpha) {   // the variant's function, by SpecId (spv_alpha.h decorates it 0); the reference arrives in a push constant
                pme[0] = (VkSpecializationMapEntry){0, 0, 4}; psv = k->color.alpha_func; psi = (VkSpecializationInfo){1, pme, 4, &psv}; pspec = s; } }
        if (!mod) return VK_ERROR_INITIALIZATION_FAILED;
        lms[ns] = mod; st[ns] = (VkPipelineShaderStageCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = s->vkstage, .pName = "main"};   // its module: lm_pipeline
        if (s->stage == SH_PS && pspec == s) st[ns].pSpecializationInfo = &psi;
        if (s->stage == SH_VS && pos_rows_for(s->key)) st[ns].pSpecializationInfo = &g_pr_spec;   // run-time rows in the position (gfx_vk_posrows.inc)
        ns++;
        if (s->pool) { sets[POOL_SET] = g_pool_dsl; nset = POOL_SET + 1; }
        gmnp |= s->gm && g_gm_np; }
    if (gmnp && !gm_np_layout(sets, &nset, g_empty_dsl)) return VK_ERROR_INITIALIZATION_FAILED;   // raw memory through the pools' descriptor array (gfx_vk_gmem_noptr.inc)
    if (k->tf == 1 && k->sh[0] && k->sh[0]->modtff) {   // the fragment-stage capture: its fragment shader takes the vertex shader's set (bindings 100 + buffer)
        lms[ns] = k->sh[0]->modtff; st[ns++] = (VkPipelineShaderStageCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .pName = "main"}; }
    // a task / mesh pipeline (gfx_vk_mesh.inc): no vertex input, and the draw's first workgroup in a push constant at the bytes the alpha test's reference has in the others
    // (so pipe_ps_mod gives a mesh pipeline no alpha-test variant)
    int mesh = k->sh[0] && k->sh[0]->stage == SH_MS;
    VkPushConstantRange pcr = mesh ? (VkPushConstantRange){VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT, 0, MESH_PC_BYTES}
                                   : (VkPushConstantRange){VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4};   // the alpha test's reference (spv_alpha.h); unused by shaders that do not declare it
    VkShaderStageFlags lbpc = mesh ? 0 : (k->sh[0] && k->sh[0]->lbpc ? VK_SHADER_STAGE_VERTEX_BIT : 0) | (k->sh[1] && k->sh[1]->lbpc ? VK_SHADER_STAGE_FRAGMENT_BIT : 0);
    VkPushConstantRange pcrs[2] = {pcr, {lbpc, SPV_LBASE_PC, 4}};   // [1]: the layer base the modules of those stages read (spv_fit_lbase); only then
    VkPipelineLayoutCreateInfo lc = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = nset, .pSetLayouts = sets, .pushConstantRangeCount = lbpc ? 2u : 1u, .pPushConstantRanges = pcrs};
    if (vkCreatePipelineLayout(g_vk.dev, &lc, NULL, &pp->pl) != VK_SUCCESS) return VK_ERROR_INITIALIZATION_FAILED;
    pp->alpha = pspec != NULL; pp->lbpc = lbpc;
    VkVertexInputBindingDescription vb[32]; VkVertexInputAttributeDescription va[16]; VkVertexInputBindingDivisorDescriptionEXT vd[32]; uint32_t nvd = 0; uint32_t nvb = 0, nva = 0; uint32_t used = 0;
    for (int i = 0; i < 16; i++) {
        va_st a; memcpy(&a, &k->attr[i], 4);
        if (!a.fmt) continue;
        char t = k->sh[0] ? k->sh[0]->vin[i] : 'f';
        int kind, bits;
        if (vfmt_desc(a.fmt, &kind, &bits) && !vfmt_direct(a.fmt, t)) {   // its register bits, made on the CPU (see vfmt_desc), in a binding of its own
            va[nva++] = (VkVertexInputAttributeDescription){(uint32_t)i, 16u + (uint32_t)i, t == 'u' ? VK_FORMAT_R32G32B32A32_UINT : t == 's' ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_SFLOAT, 0};
            vb[nvb++] = (VkVertexInputBindingDescription){16u + (uint32_t)i, 16, k->stream[a.stream].divisor ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
            if (k->stream[a.stream].divisor > 1 && g_divisor) vd[nvd++] = (VkVertexInputBindingDivisorDescriptionEXT){16u + (uint32_t)i, k->stream[a.stream].divisor};
            continue;
        }
        VkFormat f = vtx_fmt(a.fmt); if (!f) { RED_ONCE(86, "vertex attribute format %#x not mapped", a.fmt); continue; }
        VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(g_vk.pd, f, &fp);
        if (!(fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)) RED_ONCE(119, "vertex attribute format %#x (Vulkan %d) is not a vertex format on this GPU: it reads undefined values", a.fmt, f);
        va[nva++] = (VkVertexInputAttributeDescription){(uint32_t)i, a.stream, f, a.offset}; used |= 1u << a.stream;
    }
    for (int s = 0; s < 16; s++) if (used >> s & 1) {
        vb[nvb++] = (VkVertexInputBindingDescription){(uint32_t)s, k->stream[s].stride, k->stream[s].divisor ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
        if (k->stream[s].divisor > 1) {
            if (g_divisor) vd[nvd++] = (VkVertexInputBindingDivisorDescriptionEXT){(uint32_t)s, k->stream[s].divisor};
            else RED_ONCE(87, "vertex stream divisor %u > 1 on a GPU without VK_EXT_vertex_attribute_divisor: read as 1", k->stream[s].divisor);
        }
    }
    VkPipelineVertexInputDivisorStateCreateInfoEXT vds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT, .vertexBindingDivisorCount = nvd, .pVertexBindingDivisors = vd};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, .pNext = nvd ? &vds : NULL, .vertexBindingDescriptionCount = nvb, .pVertexBindingDescriptions = vb,
                                               .vertexAttributeDescriptionCount = nva, .pVertexAttributeDescriptions = va};
    VkPrimitiveTopology tp = (VkPrimitiveTopology)k->topo;
    int strip = tp == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP || tp == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP || tp == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN || tp == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY || tp == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = tp, .primitiveRestartEnable = strip && k->restart};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    int front_ccw = k->poly.front == NVN_FRONT_FACE_CCW; if (k->ll) front_ccw = !front_ccw;   // see the file header: the queue's lower-left raster flip inverts the winding
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .rasterizerDiscardEnable = k->tf == 2 || (k->tf == 1 && !(k->sh[0] && k->sh[0]->modtff)), .depthClampEnable = k->depth_clamp && g_vk_feat.depthClamp,
        .polygonMode = k->poly.mode == NVN_POLYGON_MODE_LINE && g_vk_feat.fillModeNonSolid ? VK_POLYGON_MODE_LINE : k->poly.mode == NVN_POLYGON_MODE_POINT && g_vk_feat.fillModeNonSolid ? VK_POLYGON_MODE_POINT : VK_POLYGON_MODE_FILL,
        .cullMode = (VkCullModeFlags)(k->poly.cull & 3), .frontFace = front_ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE, .depthBiasEnable = k->poly.offset_en != 0, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = (VkSampleCountFlagBits)(k->rk.samples ? k->rk.samples : 1), .alphaToCoverageEnable = k->ms.a2c != 0};
    VkStencilOpState sf = {stop(k->ds.fail_f), stop(k->ds.zpass_f), stop(k->ds.zfail_f), cmpop(k->ds.sf_f, VK_COMPARE_OP_ALWAYS), 0xff, 0xff, 0};
    VkStencilOpState sb = {stop(k->ds.fail_b), stop(k->ds.zpass_b), stop(k->ds.zfail_b), cmpop(k->ds.sf_b, VK_COMPARE_OP_ALWAYS), 0xff, 0xff, 0};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, .depthTestEnable = k->ds.dt, .depthWriteEnable = k->ds.dt && k->ds.dw,
        .depthCompareOp = cmpop(k->ds.df, VK_COMPARE_OP_LESS), .stencilTestEnable = k->ds.st, .front = sf, .back = sb, .maxDepthBounds = 1};
    VkPipelineColorBlendAttachmentState cba[8];
    for (uint32_t i = 0; i < k->ncolor && i < 8; i++) {
        const blend_st *b = &k->blend[i];
        cba[i] = (VkPipelineColorBlendAttachmentState){(k->color.blend_en >> i & 1) != 0, bfac(b->src_rgb, VK_BLEND_FACTOR_ONE), bfac(b->dst_rgb, VK_BLEND_FACTOR_ZERO), bop(b->eq_rgb),
            bfac(b->src_a, VK_BLEND_FACTOR_ONE), bfac(b->dst_a, VK_BLEND_FACTOR_ZERO), bop(b->eq_a), (VkColorComponentFlags)(k->chmask.mask >> (4 * i) & 0xf)};
        if (b->adv) RED_ONCE(88, "advanced blend mode %u: drawn with plain blending", (unsigned)b->adv);
    }
    VkPipelineColorBlendStateCreateInfo cb_ = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = k->ncolor, .pAttachments = cba,
        .logicOpEnable = k->color.logic_op != NVN_LOGIC_OP_COPY && g_vk_feat.logicOp, .logicOp = (VkLogicOp)(k->color.logic_op & 15)};
    if (k->color.logic_op != NVN_LOGIC_OP_COPY && !g_vk_feat.logicOp) RED_ONCE(89, "logic op %u on a GPU without logic ops: drawn as copy", (unsigned)k->color.logic_op);
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                            VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_DEPTH_BIAS};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = sizeof dyn / sizeof *dyn, .pDynamicStates = dyn};
    VkGraphicsPipelineCreateInfo pc = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = ns, .pStages = st, .pVertexInputState = mesh ? NULL : &vi, .pInputAssemblyState = mesh ? NULL : &ia,
        .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb_, .pDynamicState = &dy, .layout = pp->pl, .renderPass = k->rp};
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_MONOTONIC);
    const char *pbk[4]; uint32_t miss = 0; for (int i = 0; i < 4; i++) { pbk[i] = k->sh[i] ? k->sh[i]->key : NULL;
        if (k->sh[i] && k->sh[i]->stage == SH_PS) { const vshader *pv = pipe_ps_prev(k); miss = pv ? k->sh[i]->io_in & ~pv->io_out : 0; } }
    int pb = pb_begin('g', pbk, (uint8_t)tp, (uint8_t)k->ncolor, (uint8_t)(k->z != 0), miss);
    VkResult pr = lm_pipeline(&pc, NULL, st, lms, ns, &pp->p);
    pb_end(pb);
    atomic_fetch_add(&g_pso_ns, clock_gettime_nsec_np(CLOCK_MONOTONIC) - t0); atomic_fetch_add(&g_pso_made, 1);
    *ons = ns; *onva = nva; *otp = tp;
    return pr;
}
static void pipe_prep(const pipe_key *k) {   // under the lock: what pipe_make would otherwise create in shared state
    if (!g_empty_dsl) { VkDescriptorSetLayoutCreateInfo lc = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; vkCreateDescriptorSetLayout(g_vk.dev, &lc, NULL, &g_empty_dsl); }
    for (int i = 0; i < 4; i++) if (k->sh[i] && k->sh[i]->stage == SH_PS && !k->tf) { int a; pipe_ps_mod(k, k->sh[i], &a); }
}
// ---------------- Pipelines met for the first time, built in the background ----------------
// A pipeline met for the first time cost the driver's compile on the replay thread, which the game's frame waits for: 20 to 35 ms a graphics pipeline on an Adreno 830, and the
// OnePlus Pad 3's driver builds them about six times slower than the Ayn Odin 2's (baselines problem 5: Smash battles of 100 to 370 ms frames while it met new ones).
// nx2android (github.com/shirai91/nx2android, 0d0468d) builds such pipelines on two threads of their own and leaves the draw out until its pipeline exists, a few frames,
// under the rule kept here: every target of the draw's pass was drawn into in this present and the two before (rt_runs >= 3), so the draw shows a few frames late on a target
// the game draws every frame. A target drawn for the first time (an environment map made once at a scene's load) builds at once, as before: its one drawing would be lost
// for good. So do a linear render target's (the CPU reads it back: Super Mario Odyssey's fog solver feeds what it reads into the next step) and transform feedback (later draws
// read what it writes). The keys' shaders and pass are made under the replay's lock as pipe_get does (pipe_prep), the compile without it, as the start-up list does
// (prewarm_thread). DM_VK_ASYNC_PIPE=0 (env.txt): every pipeline built at once, on the replay thread. A draw that re-draws geometry whose depth an earlier pass laid down
// (depth EQUAL, not written) builds at once too: left out, the target's clear colour shows through that depth (apipe_rule.h; DM_VK_ASYNC_EQUAL=1 leaves it out, as before).
// DM_VK_ASYNC_DELAY_MS=<n> (diagnostic): each background build waits that long first, which shows what a slow driver leaves out
enum { APQ = 256 };
#define APQ_PENDING ((uintptr_t)1)   // a queued pipeline's pl until its thread fills it in (or, the build failed, clears it): never a layout of its own
typedef struct { pipe_key k; pipe_t *pp; vshader *kern; } apq_t;   // kern: a task / mesh kernel's compute pipeline instead (gfx_vk_meshemu.inc memu_cpipe_bg)
static void memu_cpipe_bg(vshader *s);
static apq_t g_apq[APQ]; static unsigned g_apq_h, g_apq_t; static int g_apq_on;   // under g_apq_mu: the queue (head, tail), its threads started
static pthread_mutex_t g_apq_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_apq_cv = PTHREAD_COND_INITIALIZER;
static uint64_t g_apq_queued, g_apq_built, g_apq_failed, g_apq_left, g_apq_redraw;   // under g_bk: pipelines queued, built, failed; draws left out waiting for one; pipelines of depth-equal re-draws built at once
static void *apipe_thread(void *arg) {
    (void)arg; pthread_setname_np(pthread_self(), "vk-apipe");
    for (;;) {
        pthread_mutex_lock(&g_apq_mu); while (g_apq_h == g_apq_t) pthread_cond_wait(&g_apq_cv, &g_apq_mu);
        apq_t q = g_apq[g_apq_t % APQ]; g_apq_t++; pthread_mutex_unlock(&g_apq_mu);
        if (q.kern) { memu_cpipe_bg(q.kern); continue; }
        { static int d = -1; if (d < 0) { const char *e = getenv("DM_VK_ASYNC_DELAY_MS"); d = e && atoi(e) > 0 ? atoi(e) : 0; if (d) LOG("[vk] DM_VK_ASYNC_DELAY_MS=%d: each background pipeline build waits that long first (diagnostic)", d); }
          if (d) usleep((useconds_t)d * 1000); }
        pipe_t tmp; memset(&tmp, 0, sizeof tmp); uint32_t ns = 0, nva = 0; VkPrimitiveTopology tp = 0; uint64_t t0 = clock_gettime_nsec_np(CLOCK_MONOTONIC);
        VkResult pr = pipe_make(&q.k, &tmp, &ns, &nva, &tp);
        double ms = (clock_gettime_nsec_np(CLOCK_MONOTONIC) - t0) / 1e6;
        pthread_mutex_lock(&g_bk);
        if (pr == VK_SUCCESS) { *q.pp = tmp; pk_record(&q.k); pcache_made(); g_apq_built++; } else { q.pp->pl = VK_NULL_HANDLE; g_apq_failed++; vk_start_pipe_failed(q.k.sh[0] ? q.k.sh[0]->key : NULL, q.k.sh[1] ? q.k.sh[1]->key : "-"); }   // (failed: never made again, as at once)
        uint64_t built = g_apq_built, queued = g_apq_queued, left = g_apq_left;
        pthread_mutex_unlock(&g_bk);
        if (pr != VK_SUCCESS) { static int n; if (n++ < 16) LOG("[RED] [vk] pipeline creation failed in the background: VkResult %d (shaders %s %s; %u stages, %u vertex attributes, %u colour targets, topology %d)",
                                                                  pr, q.k.sh[0] ? q.k.sh[0]->key : "-", q.k.sh[1] ? q.k.sh[1]->key : "-", ns, nva, q.k.ncolor, (int)tp); }
        else { static int n; if (n++ < 8 || built % 100 == 0) LOG("[vk] pipeline built in the background in %.1f ms (shaders %s %s): %llu built of %llu queued, %llu draws left out meanwhile (DM_VK_ASYNC_PIPE=0 builds at once)",
                                                          ms, q.k.sh[0] ? q.k.sh[0]->key : "-", q.k.sh[1] ? q.k.sh[1]->key : "-", (unsigned long long)built, (unsigned long long)queued, (unsigned long long)left); }
    }
    return NULL;
}
static int apipe_any(void) {   // DM_VK_ASYNC_PIPE: pipelines may be built in the background at all
    static int on = -1; if (on < 0) { const char *e = getenv("DM_VK_ASYNC_PIPE"); on = !(e && *e == '0'); if (!on) LOG("[vk] DM_VK_ASYNC_PIPE=0: pipelines met for the first time are built at once, on the replay thread"); }
    return on;
}
static int apipe_redraw_too(void) {   // DM_VK_ASYNC_EQUAL=1: depth-equal re-draws are left out as well (the rule before droidplay-core#444, for an A/B)
    static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_ASYNC_EQUAL"); v = e && *e == '1'; if (v) LOG("[vk] DM_VK_ASYNC_EQUAL=1: draws that test depth EQUAL without writing it are left out while their pipeline builds, as before"); }
    return v;
}
static int apipe_ok(const pipe_key *k) {   // under g_bk, right after rp_prepare (P is the draw's pass): whether a new pipeline may be built in the background (apipe_rule.h)
    if (!apipe_any()) return 0;
    apr_target t[9]; int n = 0;
    for (int i = 0; i < 8; i++) { const vtex *v = P.cv[i]; if (v) t[n++] = (apr_target){v->rt_runs > 3 ? 3 : (int)v->rt_runs, !!(v->desc.flags & NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)}; }
    if (P.dv) t[n++] = (apr_target){P.dv->rt_runs > 3 ? 3 : (int)P.dv->rt_runs, 0};
    if (apr_leave_out(k->tf, t, n, k->ds.dt, k->ds.dw, k->ds.df, apipe_redraw_too())) return 1;
    if (apr_leave_out(k->tf, t, n, k->ds.dt, k->ds.dw, k->ds.df, 1)) g_apq_redraw++;   // would have been left out before: a depth-equal re-draw built at once
    return 0;
}
static int apipe_queue(const pipe_key *k, pipe_t *pp, vshader *kern) {   // under g_bk: 1 = queued (pp stays without a pipeline until a thread has built it; kern: that kernel's pipeline instead)
    (void)pcache();   // made here, under the lock: pcache makes it on first use, which a build outside the lock must not race
    pthread_mutex_lock(&g_apq_mu);
    if (!g_apq_on) { g_apq_on = 1; for (int i = 0; i < 2; i++) { pthread_t th; if (!pthread_create(&th, NULL, apipe_thread, NULL)) pthread_detach(th); } }
    int ok = g_apq_h - g_apq_t < APQ; if (ok) { apq_t *q = &g_apq[g_apq_h % APQ]; memset(q, 0, sizeof *q); if (kern) q->kern = kern; else { q->k = *k; q->pp = pp; } g_apq_h++; pthread_cond_signal(&g_apq_cv); }
    pthread_mutex_unlock(&g_apq_mu);
    if (ok && !kern) g_apq_queued++;
    return ok;
}
static pipe_t *pipe_get(const pipe_key *k) {
    pipe_t *pp = nvn_hm_get(&g_pipes, k); if (pp) { if (!pp->p && pp->pl == (VkPipelineLayout)APQ_PENDING) g_apq_left++; return pp->p ? pp : NULL; }
    pp = calloc(1, sizeof *pp); nvn_hm_put(&g_pipes, k, pp);
    if (apipe_ok(k)) { pipe_prep(k); pp->pl = (VkPipelineLayout)APQ_PENDING; if (apipe_queue(k, pp, NULL)) { g_apq_left++; return NULL; } pp->pl = VK_NULL_HANDLE; }
    uint32_t ns, nva; VkPrimitiveTopology tp; VkResult pr = pipe_make(k, pp, &ns, &nva, &tp);
    if (pr == VK_SUCCESS) { pk_record(k); pcache_made(); }
    {   // which fields set a new pipeline apart from the last one with the same shaders and pass (the first 40): key churn
        static struct { vshader *a, *b; VkRenderPass rp; pipe_key k; } last[64]; static int nlog;
        unsigned h = (unsigned)(((uintptr_t)k->sh[0] >> 4) ^ ((uintptr_t)k->sh[1] >> 6) ^ ((uintptr_t)k->rp >> 3)) % 64;
        if (last[h].a == k->sh[0] && last[h].b == k->sh[1] && last[h].rp == k->rp && nlog < 40) {
            char d[300] = ""; for (size_t i = 0; i < sizeof PK_F / sizeof *PK_F; i++) if (memcmp((const uint8_t *)k + PK_F[i].o, (const uint8_t *)&last[h].k + PK_F[i].o, PK_F[i].s)) { size_t l = strlen(d); snprintf(d + l, sizeof d - l, " %s", PK_F[i].n); }
            uint32_t c0, c1, p0, p1; memcpy(&c0, &last[h].k.color, 4); memcpy(&c1, &k->color, 4); memcpy(&p0, &last[h].k.poly, 4); memcpy(&p1, &k->poly, 4);
            nlog++; LOG("[vk] pipeline %s %s again with other:%s (colour %#x -> %#x, polygon %#x -> %#x, %u targets)", k->sh[0] ? k->sh[0]->key : "-", k->sh[1] ? k->sh[1]->key : "-", d[0] ? d : " (padding only)",
                c0, c1, p0, p1, k->ncolor); }
        last[h].a = k->sh[0]; last[h].b = k->sh[1]; last[h].rp = k->rp; last[h].k = *k;
    }
    if (pr != VK_SUCCESS) {
        static int n; if (n++ < 16) LOG("[RED] [vk] pipeline creation failed: VkResult %d (shaders %s%s %s; %u stages, %u vertex attributes, %u colour targets, topology %d, %u samples)", pr, k->sh[0] ? k->sh[0]->key : "-",
            k->z && k->sh[0] && k->sh[0]->modz ? " (.z)" : "", k->sh[1] ? k->sh[1]->key : "-", ns, nva, k->ncolor, (int)tp, k->rk.samples);
        vk_start_pipe_failed(k->sh[0] ? k->sh[0]->key : NULL, k->sh[1] ? k->sh[1]->key : "-");   // the start record's count, which the desktop app reads
        pp->p = VK_NULL_HANDLE; return NULL;
    }
    if (vk_debug()) LOG("[vk] pipeline %u for shaders %s %s", g_pipes.n, k->sh[0] ? k->sh[0]->key : "-", k->sh[1] ? k->sh[1]->key : "-");
    return pp;
}

// ---------------- Descriptor sets ----------------
// Recycled per chunk and per set layout. Every stage of every draw took a new set from the chunk's pools (vkAllocateDescriptorSets) and the completion thread reset the pools
// once the chunk completed: Breath of the Wild on an Ayn Odin 2 makes about 7,500 sets a frame (3,753 draws), and vkAllocateDescriptorSets was 8.3% of the replay thread,
// 3.3 ms a frame, about 0.5 us a set, all of it from here (simpleperf: the Adreno driver callocs each set object in it, and frees there the ones a pool reset left). Now a chunk
// keeps the sets it made, in a list per shader and layout (vshader dsf: no lookup), and hands them out again when it next records: ds_get takes the list's next free set and
// makes sets only when the list has none left, 1, 1, 2, then 4 at a time in one call (a frame's 373 layouts are used about 10 times a chunk, 40% of them once a frame: a fixed
// batch of 16 held twice the sets for no fewer calls once warm). Why a set handed out again is never one a command buffer still uses:
//   - a list is one chunk's, and its sets are handed out only while that chunk records (C): each is written and bound in that chunk's recording alone. stage_set writes a set
//     only while its chunk is still the one recorded, and nothing from that write to the bind closes a chunk (ds_moved: a sync inside stage_set closed it, RED 3; the draw,
//     capture or dispatch takes its sets again from the chunk recorded now)
//   - they are free again only in draw_chunk_reset, on the completion thread after vkWaitForFences on that chunk's fence, before its command pool's reset and before busy = 0 under
//     g_cmu; cur() takes a chunk only with busy == 0 under g_cmu, so its next recording, the only one that writes them again, comes after. A held chunk (fwd_find) stays busy
//   - within a recording each set is handed out once (used only grows): a set bound earlier in the command buffer is never written again
// Why it reads nothing stale: stage_set writes every binding of the layout, every element of an array (up to TEX_ARR_MAX: in a module spv_harr left, dsl's elements past it are never written, RED 92, undefined in
// a new set too), in one vkUpdateDescriptorSets before the set is bound, and tf_capture each feedback binding of dsl_tf. Binding 0 (the support buffer) stays unwritten only in
// the sets of a shader none of whose modules uses it (s->b0, fixed per shader: no set of its lists ever had it written), and a descriptor not statically used need not be valid.
// A failed stage_set leaves its set unbound (the draw is dropped). A set handed out again may still name a texel buffer view or image view destroyed since its last use: Vulkan
// allows writing over such a descriptor, and every one is written before the set is bound. DM_VK_SET_STATS=1 checks that each stage_set writes all of its layout's descriptors.
// Memory: the pools are reset only once a chunk holds more than DS_CAP sets (DM_VK_SET_CAP=<n>), all of them, and the chunk's lists emptied (a set is not freed alone: the
// pools have no FREE_DESCRIPTOR_SET_BIT). A layout one set of which does not fit an empty pool gets none (sh_get ds_fits; else ds_alloc_n finds it: one pool, once; vshader dsbig, RED 93). DM_VK_SET_RECYCLE=0 (env.txt):
// a new set for each stage and the pools reset when the chunk completes, as before
enum { DS_CAP = 16384 };   // a chunk holds at most what each list gave in one recording, plus 3 from a batch: about 4,000 once warm on a dumped frame of Breath of the Wild (16.6k over 4 chunks), the whole frame's 7,500 if every window came to each
static int g_ds_on = -1, g_ds_cap = DS_CAP, g_ds_stats;
static int ds_recycle(void) {   // the switches, read once (chunks_init: before the completion thread starts, which reads them in draw_chunk_reset)
    if (g_ds_on < 0) { const char *e = getenv("DM_VK_SET_RECYCLE"), *c = getenv("DM_VK_SET_CAP"), *t = getenv("DM_VK_SET_STATS");
        if (c && atoi(c) > 0) g_ds_cap = atoi(c);
        g_ds_stats = t && *t && *t != '0'; g_ds_on = !(e && *e == '0');
        if (!g_ds_on) LOG("[vk] DM_VK_SET_RECYCLE=0: a new descriptor set for every stage, the pools reset when the chunk completes"); }
    return g_ds_on;
}
static uint64_t g_ds_got, g_ds_reused, g_ds_made, g_ds_calls, g_ds_checked, g_ds_short; static _Atomic uint64_t g_ds_resets;   // sets handed out, of them from a list's free ones; sets made, in calls; DM_VK_SET_STATS
static int ds_pool_add(void) {   // a new pool for the current chunk
    VkDescriptorPoolCreateInfo pc = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 2048, .poolSizeCount = sizeof g_ds_pool_sz / sizeof *g_ds_pool_sz, .pPoolSizes = g_ds_pool_sz};
    if (C->ndp == C->cdp) { C->cdp = C->cdp ? 2 * C->cdp : 4; C->dp = realloc(C->dp, C->cdp * sizeof *C->dp); }
    if (vkCreateDescriptorPool(g_vk.dev, &pc, NULL, &C->dp[C->ndp]) != VK_SUCCESS) { RED_ONCE(90, "descriptor pool creation failed"); return 0; }
    C->ndp++; return 1;
}
static VkDescriptorSet ds_alloc(VkDescriptorSetLayout l) {   // DM_VK_SET_RECYCLE=0: a new set from the chunk's pools
    if (!cur()) return VK_NULL_HANDLE;
    for (;;) {
        if (C->dp_cur < C->ndp) {
            VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = C->dp[C->dp_cur], .descriptorSetCount = 1, .pSetLayouts = &l};
            VkDescriptorSet s; g_ds_calls++; if (vkAllocateDescriptorSets(g_vk.dev, &ai, &s) == VK_SUCCESS) { g_ds_made++; return s; }
            C->dp_cur++; continue;
        }
        if (!ds_pool_add()) return VK_NULL_HANDLE;
    }
}
// k (1 to 4) sets of layout l from the current chunk's pools into out → how many were made: k, 1 when only one fits the pool (a call making several fails whole), 0 when no
// pool can be made (RED 90), -1 when one set does not fit even an empty pool (RED 93; ds_get marks the layout so that no later call makes another pool for it)
static int ds_alloc_n(VkDescriptorSetLayout l, int k, VkDescriptorSet *out) {
    VkDescriptorSetLayout ls[4] = {l, l, l, l}; int fresh = 0, d0 = C->dp_cur;   // fresh: the pool tried was made by this call (empty)
    for (;;) {
        if (C->dp_cur < C->ndp) {
            VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = C->dp[C->dp_cur], .descriptorSetCount = (uint32_t)k, .pSetLayouts = ls};
            g_ds_calls++; if (vkAllocateDescriptorSets(g_vk.dev, &ai, out) == VK_SUCCESS) return k;
            if (k > 1) { k = 1; continue; }
            // (ds_alloc made pools until creation failed). dp_cur goes back to where it was: the pools passed over failed this layout alone and still take others, the new one after them
            if (fresh) { C->dp_cur = d0; RED_ONCE(93, "a descriptor set larger than a whole pool: its shader's draws are dropped"); return -1; }
            C->dp_cur++; continue;   // the pools before dp_cur hold sets the lists keep: tried again only after a full reset
        }
        if (!ds_pool_add()) return 0;
        fresh = 1;
    }
}
static int ds_room(ds_list ***a, int n, int *cap) {   // room for one more list in a chunk's array
    if (n < *cap) return 1;
    int c = *cap ? 2 * *cap : 64; ds_list **x = realloc(*a, (size_t)c * sizeof *x); if (!x) return 0;
    *a = x; *cap = c; return 1;
}
// a descriptor set of shader s's layout (tf: dsl_tf) for the current chunk, to be written whole by stage_set and bound in this chunk
static VkDescriptorSet ds_get(vshader *s, int tf) {
    VkDescriptorSetLayout l = tf ? s->dsl_tf : s->dsl;
    g_ds_got++;
    if (s->dsbig >> !!tf & 1) return VK_NULL_HANDLE;   // no pool holds one (ds_fits, RED 93): ds_alloc would make pools until creation failed
    if (!ds_recycle()) return ds_alloc(l);
    if (!cur() || (!s->dsf && !(s->dsf = calloc(2 * (size_t)g_nchunk, sizeof *s->dsf)))) return VK_NULL_HANDLE;
    ds_list *L = &s->dsf[2 * (C - g_ch) + !!tf];
    if (!L->used && !ds_room(&C->dt, C->ndt, &C->cdt)) return VK_NULL_HANDLE;   // the arrays grow before any set is made: a list holding sets is always in dl
    if (L->used < L->n) g_ds_reused++;
    else {
        int k = L->n < 4 ? (L->n ? L->n : 1) : 4;   // 1, 1, 2, then 4 at a time
        if (L->n + k > L->cap) { int c = L->cap ? 2 * L->cap : 4; while (c < L->n + k) c *= 2; VkDescriptorSet *x = realloc(L->s, (size_t)c * sizeof *x); if (!x) return VK_NULL_HANDLE; L->s = x; L->cap = c; }
        if (!L->n && !ds_room(&C->dl, C->ndl, &C->cdl)) return VK_NULL_HANDLE;
        int got = ds_alloc_n(l, k, L->s + L->n); if (got <= 0) { if (got < 0) s->dsbig |= (uint8_t)(1u << !!tf); return VK_NULL_HANDLE; }
        if (!L->n) C->dl[C->ndl++] = L;
        L->n += got; C->nds += got; g_ds_made += (uint64_t)got;
    }
    if (!L->used) C->dt[C->ndt++] = L;
    return L->s[L->used++];
}
// 1 when the chunk recorded now is not the one a set came from (seq: its sequence number at ds_get): a sync inside stage_set (a texture the CPU wrote between stage_textures
// and stage_set uploaded again: vt_get → vt_upload_ → cpu_view → submit_wait) closed it, RED 3. Its completion has run by then (submit_wait waits for it): its sets were
// free to be handed out again, and past g_ds_cap its pools reset, the set freed; bound in a later chunk, a set would also still be pending there once its own chunk hands it
// out again. So stage_set writes nothing once it is true (checked after its last vt_get, before its vkUpdateDescriptorSets), and vk_draw_, tf_capture and vk_dispatch take
// their sets again from the chunk recorded now, from their first stage (ds_again): the sync has landed and the texture it was for is uploaded, so a second pass closes the
// chunk again only if the game changed one of the textures once more meanwhile (then the work is dropped, RED 94). With recycling only: DM_VK_SET_RECYCLE=0 writes and binds
// them, as before. RED 3 was not seen on Breath of the Wild; a dispatch does not raise it (g_drawing is a draw's): DM_VK_SET_STATS=1 counts the passes taken again and the drops
static int ds_moved(uint64_t seq) { return ds_recycle() && !(C && C->seq == seq); }
static uint64_t g_ds_again, g_ds_lost[3];   // passes taken again after ds_moved; draws, captures and dispatches dropped when the second pass's chunk closed too (DM_VK_SET_STATS)
static int ds_again(int pass, int kind) {   // pass 0 or 1 of a draw (kind 0), capture (1) or dispatch (2) whose set's chunk closed (ds_moved) → 1: dropped
    if (!pass) { g_ds_again++; return 0; }
    g_ds_lost[kind]++; RED_ONCE(94, "a chunk was closed inside stage_set twice in one draw or dispatch: it is dropped");
    return 1;
}
// the completion thread (draw_chunk_reset: after the chunk's fence, before busy = 0): the sets its recording took are free again; past g_ds_cap held, the pools are reset
static void ds_chunk_done(chunk *c) {
    for (int i = 0; i < c->ndt; i++) c->dt[i]->used = 0;
    c->ndt = 0;
    if (c->nds <= g_ds_cap) return;
    for (int i = 0; i < c->ndp; i++) vkResetDescriptorPool(g_vk.dev, c->dp[i], 0);
    for (int i = 0; i < c->ndl; i++) c->dl[i]->n = 0;
    c->dp_cur = 0; c->ndl = 0; c->nds = 0; atomic_fetch_add(&g_ds_resets, 1);
}
static const uint8_t c_bases[6] = {0x11, 0x51, 0x41, 0x67, 0x77, 0x31};   // c0 storage-buffer address table per NVN stage (×0x10): vertex 0x110, fragment 0x510, geometry 0x410, compute 0x310, NVN2 task 0x670 and mesh 0x770 (dmsh_abi.h)
// The driver banks of a stage, made from the bound state: c0 (storage buffer addresses, base instance at 0x644) and c2 (texture, image and sampler handles)
static uint32_t g_cdim[3];   // the current dispatch's group counts (c0 + 0x200 for compute)
// An indirect dispatch whose arguments a dispatch before it wrote (a pending write-back) runs as vkCmdDispatchIndirect on that write's GPU copy, its group counts copied into its c0
// bank on the GPU (bank_c0's words), instead of the replay submitting the chunk and waiting for the write to land so it can read the counts (ranges_view, ind_read): Ocarina of
// Time's culling makes such dispatches about 1.4 times a frame, and each drain took the replay thread 7-10 ms a frame on a Galaxy Z Fold 8 in Kokiri Forest, the GPU idle
// meanwhile. Not for a kernel with spilled shared memory (tgs: sized from the counts on the CPU) or the upscaler replacement. DM_VK_IND_FWD=1 (env.txt, or a game's
// android_defaults.txt: Ocarina of Time's) turns it on; off by default until more games have run it
static int ind_fwd_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_IND_FWD"); v = e && *e == '1'; if (v) LOG("[vk] DM_VK_IND_FWD=1: an indirect dispatch whose arguments a dispatch wrote runs on the GPU's copy of them"); } return v; }
static VkBuffer g_c0_b; static VkDeviceSize g_c0_off;   // where stage_set put the compute c0 bank (the arena): the GPU writes the counts into it
static uint64_t g_ind_gpu, g_ind_cpu, g_ind_cpu_wait, g_ind_seq, g_ind_inpl;   // g_ind_inpl: of g_ind_gpu, read in place in a placed pool   // indirect dispatches run on the GPU's copy of their arguments / with the arguments read on the CPU / of them, after a wait
static VkBuffer g_tgs_b; static VkDeviceSize g_tgs_off;   // where the dispatch's spilled shared memory lies when it is not the chunk's arena (stage_set; gfx_vk_meshemu.inc memu_kernel sets it for a kernel)
static void bank_c0(int nst, uint8_t *b, uint32_t base_inst) {
    memset(b, 0, 0x800);
    if (nst == 5) { memcpy(b + DMSH_C0_DISPATCH, g_cdim, 12);   // compute: the group counts, then the uniform buffer table (0x210 + 16 r)
        // NVN2 compute also reads them at the start of the segment, as nvn_mtl.m's encode_args_ writes them: Ocarina of Time's fog takes its sample count from x · y · 32 · 32
        // (21d9f83e6027c565), and at zero divided by it, which put NaN in the fog's froxel volumes and left Kokiri Forest black
        if (dm_sdk_ounce()) memcpy(b + DMSH_C0_DISPATCH_NVN2, g_cdim, 12);
        for (int r = 0; r < 16; r++) { uint64_t a = D.ubo[5][r].addr; uint32_t n = (uint32_t)D.ubo[5][r].size; memcpy(b + 0x210 + 16 * r, &a, 8); memcpy(b + 0x218 + 16 * r, &n, 4); } }
    uint32_t o = (uint32_t)c_bases[nst] << 4;
    for (int r = 0; r < 16 && o && o + 16 * r + 12 <= 0x800; r++) { uint64_t a = D.ssbo[nst][r].addr; uint32_t n = (uint32_t)D.ssbo[nst][r].size; memcpy(b + o + 16 * r, &a, 8); memcpy(b + o + 16 * r + 8, &n, 4); }
    memcpy(b + 0x644, &base_inst, 4);
    // graphics stages (task and mesh too): the largest point size, to which GLSLC clamps a shader-written gl_PointSize, as nvn_mtl.m writes it. At 0 every
    // point sprite had size 0 (Super Mario Odyssey's fog splats on the Mac; Ocarina of Time draws five point-mesh programs)
    if (nst != 5) { const float max_point = 2047.0f; memcpy(b + DMSH_C0_POINT_SIZE, &max_point, 4); }
}
// c2's layout, as nvn_mtl.m's C2_* and c2_handle: a 32-bit handle word every 8 bytes, the row's upper word 0; combined slots 0..40 from 0x20 (32..40 share the
// image table: a bound image is what lies there; nvn_mtl.m tex_slot_h stops at 39, the image table's 8 slots), separate textures from 0x168, separate samplers from 0x568;
// 0 below 0x20 and from 0x968 to the bank's end
enum { C2_ROW = 8, C2_TEX = 0x20, C2_TEX_SLOTS = 41, C2_SEPTEX = 0x168, C2_SAMP = 0x568, C2_SEP_SLOTS = 128, C2_SIZE = 0xA00 };
_Static_assert(C2_TEX + C2_ROW * C2_TEX_SLOTS == C2_SEPTEX && C2_SEPTEX + C2_ROW * C2_SEP_SLOTS == C2_SAMP && C2_SAMP + C2_ROW * C2_SEP_SLOTS <= C2_SIZE, "c2 tables");
_Static_assert(NTEX >= 128 + C2_SEP_SLOTS && sizeof D.img[0] / sizeof D.img[0][0] > C2_TEX_SLOTS - 1 - 32 && sizeof D.samp[0] / sizeof D.samp[0][0] >= C2_SEP_SLOTS, "c2 sources");
static void bank_c2(int nst, uint8_t *b) {
    memset(b, 0, C2_SIZE);
    for (int r = 0; r < C2_TEX_SLOTS; r++) { uint32_t h = (uint32_t)(r >= 32 && D.img[nst][r - 32] ? D.img[nst][r - 32] : D.tex[nst][r]); memcpy(b + C2_TEX + C2_ROW * r, &h, 4); }
    for (int r = 0; r < C2_SEP_SLOTS; r++) { uint32_t h = (uint32_t)D.tex[nst][128 + r]; memcpy(b + C2_SEPTEX + C2_ROW * r, &h, 4); }
    for (int r = 0; r < C2_SEP_SLOTS; r++) { uint32_t h = (uint32_t)D.samp[nst][r]; memcpy(b + C2_SAMP + C2_ROW * r, &h, 4); }
}
// The word bank_c2 writes at byte (a multiple of 4: every caller reads (w0 + 2e) × 4 or w1 × 4), read straight from the bound state: D's handles change only by state ops,
// which mtl_exec replays between draws, never while a draw's stages are set up. Building the whole bank for each lookup was stage_textures' and stage_set's for every stage of
// every draw, used or not: Breath of the Wild on the Odin 2, bank_c2 6.6% of the replay thread (2.65 ms a frame: 15,000 banks of 2,560 bytes, 297 handle stores each)
// and 0.3 ms more in its memset, for the 2 to 4 handle words a stage reads. DM_VK_C2_DIRECT=0 (env.txt): the bank as before, for each stage
static uint32_t c2_word(int nst, uint32_t byte) {
    if (byte < C2_TEX || (byte - C2_TEX) % C2_ROW) return 0;
    uint32_t r = (byte - C2_TEX) / C2_ROW;
    if (r < C2_TEX_SLOTS) return (uint32_t)(r >= 32 && D.img[nst][r - 32] ? D.img[nst][r - 32] : D.tex[nst][r]);
    if ((r -= C2_TEX_SLOTS) < C2_SEP_SLOTS) return (uint32_t)D.tex[nst][128 + r];
    if ((r -= C2_SEP_SLOTS) < C2_SEP_SLOTS) return (uint32_t)D.samp[nst][r];
    return 0;
}
static int c2_direct(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_C2_DIRECT"); v = !(e && *e == '0'); } return v; }
// DM_VK_C2_CHECK=1 (env.txt; a diagnostic, off by default): the bank is built as before and its word used, and every lookup is compared with c2_word, every 64th bank whole;
// a difference is a [RED] line (the first 16), the counts are in the periodic statistics (mtl_mem_stat)
static int c2_check(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_C2_CHECK"); v = e && *e && *e != '0'; } return v; }
static uint64_t g_c2_checked, g_c2_bad;
static void c2_differs(int nst, uint32_t byte, uint32_t bank, uint32_t direct) {
    if (g_c2_bad++ < 16) LOG("[RED] [vk] c2 check: stage %d byte %#x: the bank holds %#x, c2_word gives %#x", nst, byte, bank, direct);
}
// stage_textures' and stage_set's c2: NULL to read each word with c2_word, else the bank built in buf (DM_VK_C2_DIRECT=0, DM_VK_C2_CHECK=1)
static const uint8_t *c2_bank(int nst, uint8_t *buf) {
    if (c2_direct() && !c2_check()) return NULL;
    bank_c2(nst, buf);
    static uint32_t nb; if (c2_check() && !(nb++ & 63)) for (uint32_t b = 0; b < C2_SIZE; b += 4) { uint32_t w; memcpy(&w, buf + b, 4); uint32_t d = c2_word(nst, b); g_c2_checked++; if (w != d) c2_differs(nst, b, w, d); }
    return buf;
}
static int nvn_stage_of(int s) { return s == SH_PS ? 1 : s == SH_GS ? 2 : s == SH_CS ? 5 : s == SH_TK ? 3 : s == SH_MS ? 4 : 0; }   // task and mesh: the host's stages 3 and 4 (hle_nvn.c st_host)
// a word of a constant bank: c2 (c2_bank: NULL reads it with c2_word), or a game uniform buffer (c3 + slot)
static uint32_t bank_word(int nst, const uint8_t *c2, int slot, uint32_t byte) {
    uint32_t w = 0;
    if (slot < 0) slot = 2;   // a bound texture's handle: the translator gives no bank (-1), the driver's c2
    if (slot == 2) {
        if (!c2) return c2_word(nst, byte);
        if (byte + 4 <= C2_SIZE) memcpy(&w, c2 + byte, 4);
        if (c2_check()) { uint32_t d = c2_word(nst, byte); g_c2_checked++; if (d != w) c2_differs(nst, byte, w, d); }
        return w;
    }
    if (slot >= 3 && slot - 3 < 16) { const p_bind_buf *u = &D.ubo[nst][slot - 3]; if (u->addr && byte + 4 <= u->size) memcpy(&w, (const void *)(uintptr_t)(u->addr + byte), 4); }
    return w;
}
// A buffer texture (target BUFFER) as a texel buffer view over a copy of its guest memory; written (an image) → copied back when the submission completes. NULL: a dummy view
static VkBuffer g_dbuf; static VkDeviceMemory g_dmem; static VkBufferView g_dbv;
static VkBufferView dummy_tbuf(void) {
    if (g_dbv) return g_dbv;
    VkBufferCreateInfo bc = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 256, .usage = VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT};
    if (vkCreateBuffer(g_vk.dev, &bc, NULL, &g_dbuf) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_vk.dev, g_dbuf, &mr);
    VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = vkb_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)};
    if (vkAllocateMemory(g_vk.dev, &ma, NULL, &g_dmem) != VK_SUCCESS) return VK_NULL_HANDLE;
    vkBindBufferMemory(g_vk.dev, g_dbuf, g_dmem, 0);
    void *p; if (vkMapMemory(g_vk.dev, g_dmem, 0, 256, 0, &p) == VK_SUCCESS) { memset(p, 0, 256); vkUnmapMemory(g_vk.dev, g_dmem); }   // reads 0
    VkBufferViewCreateInfo vc = {.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, .buffer = g_dbuf, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .range = VK_WHOLE_SIZE};
    vkCreateBufferView(g_vk.dev, &vc, NULL, &g_dbv);
    return g_dbv;
}
// ---- Buffers a dispatch wrote, read on the GPU (buf_fwd) ----
// A storage buffer, buffer texture or buffer image a shader reads, written earlier by a compute dispatch whose write-back has not landed: bound from the dispatch's own
// arena copy, as a vertex stream is (fwd_find), when that write-back alone covers it and the offset keeps the GPU's alignment for the binding (a buffer read at the address it
// was written at always does: the arena aligns to 256). A dispatch begins and ends with a full memory barrier (vk_dispatch cb_barrier), so any later draw or dispatch sees its
// writes. One a dispatch reads and writes (a read-write storage buffer, a buffer image) is bound in place, on the arena copy of a write-back of the chunk being recorded: it
// adds no write-back of its own, and the one it writes into lands what both wrote when the chunk completes (a chunk in flight may land before this dispatch runs: synced).
// Only a dispatch writes in place: draws in a render pass have no barrier between them, so a draw's write keeps its own write-back, which later readers sync on.
// Synced instead (draw_view: the chunk submitted and waited for), a chain of dispatches each reading the one before's output was a GPU round trip apiece: Final Fantasy
// Resonance's opening, ~100 a frame (compute shaders with image atomics on 8 KB and 32 KB buffer images, each dispatch on what the one before wrote, and one reading a 64 KB storage buffer the
// dispatch before wrote). draw_view and stage_set ask the same question: between them only a sync that lands everything can close the chunk, so where stage_set no longer
// finds the write-back pending, guest memory holds it. DM_VK_BUF_FWD=0 (env.txt): such reads sync, as before (the A/B of this alone)
static int fwd_find(uintptr_t lo, uintptr_t hi, VkBuffer *b, VkDeviceSize *o);
static int fwd_hit(chunk *c, int nwb, uintptr_t lo, uintptr_t hi, VkBuffer *b, VkDeviceSize *o, int *res);
// A draw's storage buffer write made in place in a placed pool (gm_inplace_w with cs 0) is not forwardable to a later draw: a render pass has no barrier inside it. A
// dispatch begins behind a full barrier (vk_dispatch, or the one before the run of dispatches it chains to), so it reads such a write where it lies: the pool memory is
// the GPU's, and the write-back is only a note (its pages prepared once the chunk completes). Ocarina of Time's Kokiri Forest: a draw writes a 23,360-byte table in place
// and a compute shader reads it twice a frame; each read submitted the chunk and waited for the whole ring (ranges_view), the GPU idle meanwhile. g_rd_cs: the reader is
// a dispatch, or a draw run as one (draw_view, stage_set, vk_dispatch's argument lookup). DM_VK_IP_READ=0 (env.txt): such reads wait, as before
static int ip_read_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_IP_READ"); v = !(e && *e == '0'); if (!v) LOG("[vk] DM_VK_IP_READ=0: a dispatch reading a draw's in-place storage buffer write waits for the chunk"); } return v; }
static int g_rd_cs; static uint64_t g_ip_read;   // reads bound so
static int g_fwd_nwb = -1; static uint64_t g_fwd;   // the chunk's write-backs counted when the draw began (its own come after); streams bound so
// A draw under rasterizer discard whose vertex or geometry stage writes memory runs as a dispatch does (rd_writes, vk_draw_): outside the targets' pass, with a full barrier
// before and after it, so it may write in place and its write-backs are read on the GPU by later shaders, as a dispatch's are. Set for the one draw (vk_draw__ clears it)
static int g_vdisp;
static int disp_like(const vshader *s) { return s->stage == SH_CS || g_vdisp; }
static rd_homes g_rd_homes; static uint64_t g_rd_learned;   // (rd_home.h, rd_bind_w)
enum { RD_HOME_MAX = 4 << 20 };   // a home range of more bytes is not learned: the copy is made for every chunk that writes it
// Reading a pending write on the GPU needs the chunk the draw is recorded in: the read binds the write-back's copy there and holds the chunk in flight that holds it until
// that one completes (fwd_find: hold = C->seq). draw_view ran before anything of the draw had opened a chunk, so the first draw after every submission in the middle of a
// pass (64 draws, or new targets after 48) found none (buf_fwd, stream_fwd) and read the range on the CPU instead, waiting for every chunk in flight. Xenoblade Chronicles 3's
// character draws met their skinning draws' write-backs that way about 6 times a frame, each a drain of the GPU at its lowest clock. draw_view now takes the chunk first
// (cur), as the draw would right after. DM_VK_FWD_OPEN=0 (env.txt): as before
static int fwd_open_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_FWD_OPEN"); v = !(e && *e == '0'); if (!v) LOG("[vk] DM_VK_FWD_OPEN=0: a draw before its chunk is open reads no pending write on the GPU"); } return v; }
static int buf_fwd_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_BUF_FWD"); v = !(e && *e == '0'); if (!v) LOG("[vk] DM_VK_BUF_FWD=0: a buffer a dispatch wrote syncs before a later shader reads it"); } return v; }
static uint64_t g_buf_fwd[2];   // bindings read on the GPU from a pending write-back / written in place
// al: the binding's offset alignment; write: the shader writes it too (a dispatch's only, in place on the current chunk's write-back)
static int buf_fwd(uint64_t addr, uint64_t n, int write, VkDeviceSize al, VkBuffer *b, VkDeviceSize *o) {
    if (vk_safe() || !addr || !n || !buf_fwd_on() || !C) return 0;
    int ipw = g_rd_cs && ip_read_on() && (C->nipw || atomic_load(&g_inflight_ipw));   // a draw's in-place write a dispatch may read where it lies
    if (!C->nfwd && !ipw && (write || !atomic_load(&g_inflight_fwd))) return 0;
    uintptr_t lo = (uintptr_t)addr, hi = (uintptr_t)(addr + n); VkBuffer fb; VkDeviceSize fo; int r = 0;
    if (write) { int nw = g_fwd_nwb >= 0 && g_fwd_nwb <= C->nwb ? g_fwd_nwb : C->nwb; if (!fwd_hit(C, nw, lo, hi, &fb, &fo, &r) || !r) return 0; }
    else if (!fwd_find(lo, hi, &fb, &fo)) return 0;
    if (al && fo % al) return 0;
    *b = fb; *o = fo; return 1;
}
static int sb_fwd(uint64_t addr, uint64_t n, int write, VkBuffer *b, VkDeviceSize *o) { return buf_fwd(addr, n, write, g_vk.props.limits.minStorageBufferOffsetAlignment, b, o); }
static int tb_fwd(uint64_t addr, uint64_t n, int write, VkBuffer *b, VkDeviceSize *o) { return buf_fwd(addr, n, write, g_vk.props.limits.minTexelBufferOffsetAlignment, b, o); }
static int ub_fwd(uint64_t addr, uint64_t n, VkBuffer *b, VkDeviceSize *o) { return buf_fwd(addr, n, 0, g_vk.props.limits.minUniformBufferOffsetAlignment, b, o); }
// A read that the newest write of the current chunk holds whole, where that write is a draw's plain arena copy (wb_promote.h), ends the targets' pass, puts a full barrier
// after everything recorded and marks the write forwardable: the read binds its copy (buf_fwd: a storage buffer, ub_fwd: a uniform buffer), as a dispatch's, instead of
// waiting for the chunk. The pass begins again at the next draw. Xenoblade Chronicles 3 reads such a write as a uniform buffer twice a frame. Draws' writes in a chunk in
// flight still wait (taking one would change the completion thread's count of chunks with forwardable write-backs as it runs). DM_VK_WB_PROMOTE=0 (env.txt): such reads wait, as before
static int wb_promote_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_WB_PROMOTE"); v = !(e && *e == '0'); if (!v) LOG("[vk] DM_VK_WB_PROMOTE=0: a read of a draw's pending storage buffer write waits for the GPU"); } return v; }
static uint64_t g_wb_promote;   // draws' writes made forwardable for a later read
static void wb_promote(uintptr_t lo, uintptr_t hi) {
    if (!C || hi <= lo || vk_safe() || !buf_fwd_on() || !wb_promote_on()) return;
    int copy = 0; for (int k = 0; k < C->ncop; k++) if (C->cop[k].dst < hi && lo < C->cop[k].dst + C->cop[k].n) copy = 1;
    int n = g_fwd_nwb >= 0 && g_fwd_nwb <= C->nwb ? g_fwd_nwb : C->nwb;
    for (int i = n - 1; i >= 0; i--) { wb_t *w = &C->wb[i]; if (!wb_hits(w, lo, hi)) continue;   // the newest that writes into the range decides
        if (w->ofw) return;   // an order-free write's copy is not the range's bytes (ofw_bind): the read waits for it to land
        if (wb_promote_ok(lo, hi, (uintptr_t)w->dst, w->row_bytes, w->rows, w->st, w->fwd, w->inpl, w->lin, w->ds_n != 0, copy)) { rp_end(); cb_barrier(); w->fwd = 1; C->nfwd++; g_wb_promote++; }
        return; }
}
// A storage buffer a discarded draw writes (run as a dispatch: g_vdisp), bound on a copy of its whole home range (rd_home.h) that is a forwardable write-back of the chunk,
// so the draws after it find every byte they bind in that one write-back (fwd_find) and read it on the GPU. The chunk's first such write makes the copy (rd_home_copy):
// its bytes copied on the GPU from the newest copy of a chunk in flight (fwd_find holds that chunk until this one completes), else from guest memory, which draw_view
// has made current (FWD: the home range landed unless found whole on the GPU); the writes after it go in place on it. A write whose place in the copy is not a multiple
// of the storage buffer alignment (Xenoblade Chronicles 2's bone slots are 48 bytes, the Adreno's alignment 64) goes through a scratch copy: copied from the home copy
// before the draw, back into it after the draw (rd_close), each side of a full barrier. The snapshot is guest memory's, older than a chunk in flight's copy: its
// write-back lands every byte that differs from it, the in-flight chunk's own changes too, again and after them. DM_VK_RD_HOME=0 (env.txt): such writes take a copy
// of their own range, as dispatches' do (reads they do not cover sync)
static int rd_home_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_RD_HOME"); v = !(e && *e == '0'); if (!v) LOG("[vk] DM_VK_RD_HOME=0: a discarded draw's storage buffer writes take a copy of their own range"); } return v; }
enum { RD_BACK = 16 };
static struct { VkBuffer sb, hb; VkDeviceSize so, ho, n; uint64_t seq; } g_rd_back[RD_BACK]; static int g_rd_nback;   // scratch copies to copy back after the draw (rd_close)
static uint64_t g_rd_w[4];   // discarded draws' storage buffer writes: in place on a home copy, through a scratch copy; home copies made, of them copied on the GPU from a chunk in flight
static int rd_home_copy(uintptr_t hl, uintptr_t hh, VkBuffer *hb, VkDeviceSize *ho) {   // the chunk's copy of home range [hl, hh), a forwardable write-back
    size_t n = hh - hl; if (!n || !mtl_addr(hl, n, NULL, NULL)) return 0;
    VkBuffer fb; VkDeviceSize fo; int gpu = fwd_find(hl, hh, &fb, &fo);
    if (!gpu && wr_pending(hl, hh)) return 0;   // a write not landed that no copy holds whole: the range grew since draw_view landed it (a read learned more meanwhile)
    uint8_t *d = ar_alloc((n + 15) & ~(size_t)15, hb, ho); if (!d) return 0;
    int blk = C->ar_last; uint8_t *snap = malloc(n); if (!snap) return 0; memcpy(snap, (const void *)hl, n);
    if (gpu) {   // the newest bytes are on the GPU still
        VkCommandBuffer c = cb(); if (!c) { free(snap); return 0; }
        VkBufferCopy cp = {fo, *ho, n}; vkCmdCopyBuffer(c, fb, *hb, 1, &cp); cb_barrier(); g_rd_w[3]++;
    } else memcpy(d, snap, n);
    wb_t *w = wb_add(); if (!w) { free(snap); return 0; }
    *w = (wb_t){-1 - blk, (size_t)*ho, n, 1, n, n, (uint8_t *)hl, snap}; w->fwd = 1; w->vd = 1; C->nfwd++; g_rd_w[2]++;
    return 1;
}
static int rd_bind_w(uint64_t addr, size_t n, VkBuffer *b, VkDeviceSize *ao) {   // 0: not done (the caller takes a copy of the range alone)
    if (!C || !n) return 0;
    uintptr_t lo = (uintptr_t)addr, hi = lo + n, hl, hh; rd_home_find(&g_rd_homes, lo, hi, &hl, &hh);
    int nw = g_fwd_nwb >= 0 && g_fwd_nwb <= C->nwb ? g_fwd_nwb : C->nwb, r = 0; VkBuffer hb; VkDeviceSize ho;
    if (!(fwd_hit(C, nw, lo, hi, &hb, &ho, &r) && r)) { if (!rd_home_copy(hl, hh, &hb, &ho)) return 0; ho += lo - hl; }   // (draw_view made sure none of the chunk overlaps the home range then)
    VkDeviceSize al = g_vk.props.limits.minStorageBufferOffsetAlignment;
    if (!al || ho % al == 0) { *b = hb; *ao = ho; g_rd_w[0]++; return 1; }
    if (g_rd_nback == RD_BACK) return 0;
    VkBuffer sb; VkDeviceSize so; if (!ar_alloc((n + 15) & ~(size_t)15, &sb, &so)) return 0;
    VkCommandBuffer c = cb(); if (!c) return 0;
    VkBufferCopy in = {ho, so, n}; vkCmdCopyBuffer(c, hb, sb, 1, &in);   // before the draw: rd_open's barrier orders it
    g_rd_back[g_rd_nback].sb = sb; g_rd_back[g_rd_nback].so = so; g_rd_back[g_rd_nback].hb = hb; g_rd_back[g_rd_nback].ho = ho; g_rd_back[g_rd_nback].n = n; g_rd_back[g_rd_nback++].seq = C->seq;
    *b = sb; *ao = so; g_rd_w[1]++; return 1;
}
// A storage buffer a draw writes in an order-free way (ofw.h: stores of one constant, or one commutative atomic whose old value no one reads). Each draw took its own
// arena copy of it and a write-back against a snapshot, and the next draw to bind it waited for that write-back to land before copying it again (draw_view): Tears of the
// Kingdom's fragment shaders mark cells of a 131,584-byte grid from about 700 draws a frame at Thyphlo Ruins by night, each a submission and a wait for it (2.9 fps on a
// Galaxy S25 Edge, the GPU 118 ms a frame with its render passes cut at every draw). Such a copy needs nothing of guest memory: it starts as the operation's identity, and the
// draws of a chunk that write the range the same way share it. The newest write of the chunk to the range decides: after another kind of write (a dispatch's, another
// constant, another operation) a new copy starts, so the copies land in the draws' order. Each lands by combining the words the GPU changed with guest memory (gfx_vk.c
// ofw_apply), after what earlier write-backs land. draw_view makes no sync for such a binding; no later shader reads the copy on the GPU (fwd_hit: not forwardable,
// wb_promote and own_alias leave it): a read of the range waits for it to land, as for any write-back. The draws of a chunk write the copy with no barrier between them:
// stores of one constant and commutative atomics come out the same in any order. DM_VK_OFW=0 (env.txt): a copy for each draw, as before
static int ofw_ok(const rb_sbuf *sb, int cs, uint64_t addr, uint64_t n) {   // cs: a dispatch, or a draw run as one (draw_view's cs, stage_set's g_rd_cs)
    return sb->ofw && (sb->flags & 1) && !cs && addr && n && !(addr & 3) && !(n & 3) && n <= g_vk.props.limits.maxStorageBufferRange && !vk_safe() && ofw_on();
}
static uint64_t g_ofw_chain, g_ofw_new;   // bindings on a copy of the chunk; copies made
static int ofw_bind(const rb_sbuf *sb, uint64_t addr, size_t n, VkBuffer *b, VkDeviceSize *ao) {
    if (!cur()) return 0;
    uintptr_t lo = (uintptr_t)addr, hi = lo + n; VkDeviceSize al = g_vk.props.limits.minStorageBufferOffsetAlignment;
    for (int i = C->nwb - 1; i >= 0; i--) { const wb_t *w = &C->wb[i]; if (!wb_hits(w, lo, hi)) continue;
        if (w->ofw == sb->ofw && w->ofk == sb->ofk && w->st < 0 && w->rows == 1 && lo >= (uintptr_t)w->dst && hi <= (uintptr_t)w->dst + w->row_bytes) {
            VkDeviceSize o = (VkDeviceSize)w->off + (VkDeviceSize)(lo - (uintptr_t)w->dst);
            if (!al || o % al == 0) { *b = C->ar[-1 - w->st].b; *ao = o; g_ofw_chain++; return 1; } }
        break; }
    uint32_t *d = (uint32_t *)(void *)ar_alloc((n + 15) & ~(size_t)15, b, ao); if (!d) return 0;
    uint32_t id = ofw_identity(sb->ofw, sb->ofk); for (size_t q = 0; q < n / 4; q++) d[q] = id;
    wb_t *w = wb_add(); if (!w) return 0;
    *w = (wb_t){-1 - C->ar_last, (size_t)*ao, n, 1, n, n, (uint8_t *)(uintptr_t)addr, NULL}; w->ofw = sb->ofw; w->ofk = sb->ofk;
    g_ofw_new++; return 1;
}
// A shader that binds the same guest memory twice (Final Fantasy Resonance's Niagara argument shader 544e80ec57b3a10b: one 8 KB buffer as an R32ui image it
// reads and as a format-less one it clears): on the Switch the two views are one buffer. When the draw or dispatch is the first in its chunk to write it, buf_fwd
// finds no write-back to bind in place (it looks only at those from before the draw, g_fwd_nwb), so each binding got an arena copy and a write-back of its own:
// a write through one view was not what the other read, and the two write-backs, with one snapshot, landed byte by byte over each other. A binding inside a
// write-back this same draw or dispatch made is bound on that copy (wb_alias.h), with no copy or write-back of its own, and what both views wrote lands once.
// DM_VK_ALIAS=0 (env.txt): each binding gets its own copy, as before (the A/B of this alone)
static int alias_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_ALIAS"); v = !(e && *e == '0'); if (!v) LOG("[vk] DM_VK_ALIAS=0: a shader's bindings of the same memory get copies of their own"); } return v; }
static uint64_t g_alias;   // bindings bound on the copy of another binding of the same draw or dispatch (own_alias)
static int own_alias(const char *key, uint64_t addr, uint64_t n, VkDeviceSize al, VkBuffer *b, VkDeviceSize *o) {
    if (!C || !addr || !n || g_fwd_nwb < 0 || g_fwd_nwb > C->nwb || !alias_on()) return 0;
    for (int i = C->nwb - 1; i >= g_fwd_nwb; i--) {   // this draw's or dispatch's own write-backs (g_fwd_nwb: the chunk's count when it began)
        const wb_t *w = &C->wb[i]; uint64_t at;
        if (w->ofw) continue;   // an order-free write's copy holds only what was written (ofw_bind)
        if (!wb_alias_at(w->st < 0 || w->inpl, w->rows, (uintptr_t)w->dst, w->row_bytes, w->off, addr, n, al, &at)) continue;   // (in place: off is its offset in the pool piece, gfx_vk_gmem_inplace.inc)
        if (!g_alias++) LOG("[vk] shader %s binds %#llx+%llu again: bound on the copy of its other binding (own_alias)", key, (unsigned long long)addr, (unsigned long long)n);
        *b = w->inpl ? w->ipb : C->ar[-1 - w->st].b; *o = (VkDeviceSize)at; return 1;
    }
    return 0;
}
// the guest bytes tbuf_view copies of a buffer texture: its width in texels, cut to the GPU's limit, times the texel size; 0 when it copies none (the dummy is bound).
// draw_view asks it for the range a draw reads, so it must cover at least what tbuf_view reads
static size_t tbuf_bytes(const tex_t *t) {
    int bw, bh, bpb = t && t->d.target == NVN_TEXTURE_TARGET_BUFFER && t->mem ? nvn_fmt_bpb(t->d.format, &bw, &bh) : 0;
    if (bpb <= 0) return 0;
    uint64_t n = (uint64_t)(t->d.width ? t->d.width : 1);
    if (n > g_vk.props.limits.maxTexelBufferElements) n = g_vk.props.limits.maxTexelBufferElements;
    return (size_t)n * (size_t)bpb;
}
static const char *g_alias_key = "";   // the shader stage_set binds, for own_alias's log
static VkBufferView tbuf_view(const tex_t *t, int write, int cs) {   // cs: a dispatch binds it (a buffer image it writes may be bound in place, and its write-back is forwardable: buf_fwd)
    uint32_t m = 0; uint8_t swz[4];
    if (!t || t->d.target != NVN_TEXTURE_TARGET_BUFFER || !t->mem || !nvn_fmt_to_mtl(t->d.format, &m, swz) || !m) return VK_NULL_HANDLE;
    VkFormat f = vk_of_mtl(m); size_t bytes = tbuf_bytes(t);
    if (!f || !bytes) return VK_NULL_HANDLE;
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(g_vk.pd, f, &fp);
    if (!(fp.bufferFeatures & (write ? VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT : VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT))) { RED_ONCE(101, "buffer texture format %d (NVN %#x) not usable as a %s texel buffer here", f, t->d.format, write ? "storage" : "uniform"); return VK_NULL_HANDLE; }
    if (t->d.width > g_vk.props.limits.maxTexelBufferElements) RED_ONCE(102, "buffer texture of %u elements over the GPU's %u: cut", (unsigned)t->d.width, g_vk.props.limits.maxTexelBufferElements);
    VkBuffer b; VkDeviceSize off;
    if ((!write || cs) && tb_fwd((uint64_t)(uintptr_t)t->mem, bytes, write, &b, &off)) g_buf_fwd[write]++;   // on the GPU from a dispatch's pending write (buf_fwd)
    else if (own_alias(g_alias_key, (uint64_t)(uintptr_t)t->mem, bytes, g_vk.props.limits.minTexelBufferOffsetAlignment, &b, &off)) {}   // a range another binding of this shader wrote
    else { uint8_t *d = ar_alloc(bytes, &b, &off); if (!d) return VK_NULL_HANDLE;
        int inpool = mtl_addr((uint64_t)(uintptr_t)t->mem, bytes, NULL, NULL);
        if (inpool) memcpy(d, t->mem, bytes); else { memset(d, 0, bytes); RED_ONCE(103, "buffer texture outside every memory pool: reads 0"); }
        if (write) { wb_t *w = wb_add(); uint8_t *snap = malloc(bytes); if (snap) { if (inpool) memcpy(snap, t->mem, bytes); else memset(snap, 0, bytes); }   // from guest memory: the arena may be write-combined (slow to read: ar_mem_type)
            if (w) { *w = (wb_t){-1 - C->ar_last, (size_t)off, bytes, 1, bytes, bytes, t->mem, snap}; w->fwd = cs; if (cs) C->nfwd++; } } }
    VkBufferViewCreateInfo vc = {.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, .buffer = b, .format = f, .offset = off, .range = bytes};
    VkBufferView v; if (vkCreateBufferView(g_vk.dev, &vc, NULL, &v) != VK_SUCCESS) return VK_NULL_HANDLE;
    if (C->nbv == C->cbv) { C->cbv = C->cbv ? 2 * C->cbv : 64; C->bv = realloc(C->bv, C->cbv * sizeof *C->bv); }
    C->bv[C->nbv++] = v;
    return v;
}
// a storage image view of a texture: its own image (created with storage usage for NVN_TEXTURE_FLAGS_IMAGE, or given it when first bound to an image slot: vt_storage_up), at the view's level and layers
// A storage view: the image is viewed in the format the shader declares (Vulkan needs them to match), which must have its texel size; Unknown takes the image's own.
// 0 when it cannot be (the draw is then skipped: a wrong-format storage image is undefined, and can hang the GPU)
// A texture view's own format (view_t set_format): the image viewed in it when it is mutable and the texel sizes match; 0 = the image's
static VkFormat view_format(const vtex *v, const view_t *vv) {
    if (!vv || !vv->set_format) return VK_FORMAT_UNDEFINED;
    uint32_t m = 0; uint8_t sw[4];
    if (!nvn_fmt_to_mtl(vv->format, &m, sw) || !m) return VK_FORMAT_UNDEFINED;
    VkFormat f = vk_of_mtl(m);
    // a BC image decoded on the CPU (vt_create) holds the decoder's texels: a BC view of it is read as what that view's format decodes to, so a view in the image's own
    // BC format is the image itself and its sRGB twin is the decoded format's twin
    if (v->conv && f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK) { int c, px; f = bc_target(f, &c, &px); }
    if (v->conv == CONV_U16H || v->conv == CONV_S16H) { int c, px; VkFormat h = n16_twin(f, &c, &px); if (h) f = h; }   // a 16-bit normalized view of an image made as halves
    if (!f || f == v->fmt) return VK_FORMAT_UNDEFINED;
    if (v->fixed_fmt && f != srgb_twin(v->fmt)) { RED_ONCE(121, "texture view in format %d over an image of format %d, which lists only its own and its sRGB twin (DM_VK_UBWC=0 views it in any): read in the image's", f, v->fmt); return VK_FORMAT_UNDEFINED; }
    if (!v->mutable_ || !fmt_bytes(f) || fmt_bytes(f) != fmt_bytes(v->fmt)) { RED_ONCE(118, "texture view in format %d over an image of format %d: read in the image's", f, v->fmt); return VK_FORMAT_UNDEFINED; }
    return f;
}
static int g_bind_bad;   // set by stage_set when a binding has no valid descriptor: the draw / dispatch is skipped
// The aspect a texture slot reads of its image. A depth-stencil texture reads its depth, unless it is in stencil mode: a view that sets it
// (nvnTextureViewSetDepthStencilMode STENCIL) or, with no view setting it, a texture built in it (nvnTextureBuilderSetDepthStencilMode, tex_t stencil), which reads the
// stencil, as nvn_mtl.m's X32_Stencil8 views do. The shader declares a float texture and takes the stencil value from the raw bits it reads (the translator has no
// integer texture type). Xenoblade Chronicles 2 classifies its deferred lighting this way: a full-screen pass tests stencil bits 3 and 4 (floatBitsToInt(texel) & 24)
// and writes a material class into a 16-bit depth buffer, and each lighting variant draws only where that depth is equal to its own. Reading depth instead, the bits
// followed the depth's mantissa, two thirds of a lit surface's pixels took the wrong class, and no variant lit them: a dark stipple and stripes over the characters
// and the world. A stencil-only image (S8) reads its stencil
static int g_st_reads;   // stencil-mode slots bound (the first one is logged)
static VkImageAspectFlags tex_aspect(const vtex *v, const treg_t *rg, const view_t *vv, const vshader *s) {   // (tex_aspect.h)
    int missing; unsigned a = tex_aspect_pick((unsigned)v->aspect, vv && vv->set_dsmode, vv && vv->dsmode == NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL, rg->t.stencil, &missing);
    if (missing) RED_ONCE(185, "shader %s reads a depth texture of format %d in stencil mode, which has no stencil: its depth is read (said once)", s->key, v->fmt);
    else if (a == VK_IMAGE_ASPECT_STENCIL_BIT && (v->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) && !g_st_reads++)
        LOG("[vk] a texture slot in stencil mode reads the stencil of its depth-stencil image, as raw bits (shader %s, format %d)", s->key, v->fmt);
    return (VkImageAspectFlags)a;
}
static uint64_t g_e5_need;   // compute: the storage image bindings stage_set bound to E5B9G9R9 textures (vk_dispatch takes cpipe_e5 for them)
// An image slot's texture made without storage use takes it (vt_storage_up), unless it is a target of the pass a draw prepared (rp_prepare ran before stage_set): rp_start would
// begin that pass's framebuffer, made on the image the upgrade replaces. A dispatch's copy ends the pass, and the next rp_prepare makes the targets' views again
// DM_VK_STORAGE_UP (env.txt): 0 leaves such a texture as before (bound as it is, an sRGB one skipped). 1 (default) upgrades one whose own format the GPU stores to, and still
// skips one that would be stored through its UNORM twin (sRGB): with that, Ocarina of Time's title runs its upscaler's compute, which on the Pad 3's Adreno 830 drew noise over the
// whole picture and took the GPU from 24 to 108 ms a frame (DeathKhan's nx2android fork met the same and replaced the upscaler). 2: those too, through the twin
static int st_up_mode(void) { static int m = -1; if (m < 0) { const char *e = getenv("DM_VK_STORAGE_UP"); m = e && *e >= '0' && *e <= '2' ? *e - '0' : 1; if (m != 1) LOG("[vk] DM_VK_STORAGE_UP=%d: %s", m, m ? "a texture bound to an image slot without storage use takes it, an sRGB one through its UNORM twin" : "a texture bound to an image slot without storage use is bound as it is"); } return m; }
static int img_up(vtex *v, int compute) {
    int m = st_up_mode(); if (!m) return 1;
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(g_vk.pd, v->fmt, &fp);
    int e5 = v->fmt == VK_FORMAT_E5B9G9R9_UFLOAT_PACK32 && e5_store_on();   // stored through an R32_UINT view (vt_storage_up, spv_e5.h)
    if (e5 && !compute) { RED_ONCE(126, "texture ser=%llu %ux%u E5B9G9R9 is bound to an image slot of a draw: only compute shaders are rewritten to pack it (spv_e5.h), skipped", (unsigned long long)v->serial, v->w, v->h); return 0; }
    if (m == 1 && !e5 && !(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {   // said once for each format: a later one, once hidden behind the first, can be another cause
        static uint8_t told[256]; static int other; int f_ = (int)v->fmt;
        if (f_ >= 0 && f_ < 256 ? !told[f_] : !other) { if (f_ >= 0 && f_ < 256) told[f_] = 1; else other = 1;
            LOG("[RED] [vk] texture ser=%llu %ux%u format %d, made without storage use, is bound to an image slot in a format the GPU cannot store to: skipped (DM_VK_STORAGE_UP=2 stores an sRGB one through its UNORM twin; said once for each format)", (unsigned long long)v->serial, v->w, v->h, v->fmt); }
        return 0; }
    if (!compute) { int hit = P.dv == v; for (int i = 0; i < 8; i++) hit |= P.cv[i] == v;
        if (hit) { RED_ONCE(124, "texture ser=%llu, made without storage use, is bound to an image slot of a draw that renders to it: skipped", (unsigned long long)v->serial); return 0; } }
    return vt_storage_up(v);
}
static VkImageView storage_view(vtex *v, const view_t *vv, VkImageViewType vt, VkFormat decl) {
    VkFormat f = decl ? decl : v->fmt;
    if (f != v->fmt && (v->conv || !fmt_bytes(v->fmt) || fmt_bytes(v->fmt) != fmt_bytes(f))) { RED_ONCE(110, "image of format %d bound where the shader declares format %d: skipped", v->fmt, f); return VK_NULL_HANDLE; }
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(g_vk.pd, f, &fp);
    // an sRGB format the GPU cannot store to is stored through its UNORM twin, texels as the shader writes them, as Maxwell's image stores do and the Mac's linear-twin
    // views (nvn_mtl.m img_fit); the image takes STORAGE for such views (vt_storage_up). Only into an image that has it: with DM_VK_STORAGE_UP=0 an sRGB image without it was
    // viewed through its twin all the same, an invalid storage view, and Ocarina of Time's title then ran its movie compositor at 8 fps instead of skipping it as before
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) && !v->conv && (v->usage & VK_IMAGE_USAGE_STORAGE_BIT) && srgb_twin(f)) {
        VkFormatProperties tp; vkGetPhysicalDeviceFormatProperties(g_vk.pd, srgb_twin(f), &tp);
        if (tp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) { f = srgb_twin(f); fp = tp; }
    }
    // E5B9G9R9 through an R32_UINT view, on every GPU: the compute shader bound to it packs RGB9E5 (cpipe_e5, spv_e5.h), and Maxwell's stores to it are raw words
    // already packed (Ocarina of Time's a335c1db6270c355, SUST.B), which a format view would pack again. Samsung's Xclipse stores to E5B9G9R9 itself, and there the view
    // was E5B9G9R9: each word went in as the red channel's float, green and blue 0, and Ocarina of Time's picture was solid red after its upscaler
    if (!v->conv && (v->usage & VK_IMAGE_USAGE_STORAGE_BIT) && f == VK_FORMAT_E5B9G9R9_UFLOAT_PACK32 && e5_store_on()) {
        VkFormatProperties tp; vkGetPhysicalDeviceFormatProperties(g_vk.pd, VK_FORMAT_R32_UINT, &tp);
        if (tp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) { f = VK_FORMAT_R32_UINT; fp = tp; }
    }
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) || v->conv) { RED_ONCE(104, "image format %d is not storage-capable here: skipped", f); return VK_NULL_HANDLE; }
    g_view_fmt = f == v->fmt ? VK_FORMAT_UNDEFINED : f;
    VkImageView iv = view_get(v, vt, vv && vv->set_levels ? vv->base_level : 0, 1, vv && vv->set_layers ? vv->min_layer : 0, vv && vv->set_layers ? vv->nlayers : 0, VK_IMAGE_ASPECT_COLOR_BIT, NULL);
    g_view_fmt = VK_FORMAT_UNDEFINED;
    return iv;
}
static uint32_t nvn_fmt_of(VkFormat f) {   // an NVN format for a dummy of a declared storage format (formats with the same texel layout)
    switch (f) {
        case VK_FORMAT_R8_UNORM: return NVN_FORMAT_R8; case VK_FORMAT_R8_SNORM: return NVN_FORMAT_R8SN; case VK_FORMAT_R8_UINT: return NVN_FORMAT_R8UI; case VK_FORMAT_R8_SINT: return NVN_FORMAT_R8I;
        case VK_FORMAT_R16_SFLOAT: return NVN_FORMAT_R16F; case VK_FORMAT_R16_UNORM: return NVN_FORMAT_R16; case VK_FORMAT_R16_SNORM: return NVN_FORMAT_R16SN; case VK_FORMAT_R16_UINT: return NVN_FORMAT_R16UI; case VK_FORMAT_R16_SINT: return NVN_FORMAT_R16I;
        case VK_FORMAT_R32_SFLOAT: return NVN_FORMAT_R32F; case VK_FORMAT_R32_UINT: return NVN_FORMAT_R32UI; case VK_FORMAT_R32_SINT: return NVN_FORMAT_R32I;
        case VK_FORMAT_R8G8_UNORM: return NVN_FORMAT_RG8; case VK_FORMAT_R8G8_SNORM: return NVN_FORMAT_RG8SN; case VK_FORMAT_R8G8_UINT: return NVN_FORMAT_RG8UI; case VK_FORMAT_R8G8_SINT: return NVN_FORMAT_RG8I;
        case VK_FORMAT_R16G16_SFLOAT: return NVN_FORMAT_RG16F; case VK_FORMAT_R16G16_UNORM: return NVN_FORMAT_RG16; case VK_FORMAT_R16G16_SNORM: return NVN_FORMAT_RG16SN; case VK_FORMAT_R16G16_UINT: return NVN_FORMAT_RG16UI; case VK_FORMAT_R16G16_SINT: return NVN_FORMAT_RG16I;
        case VK_FORMAT_R32G32_SFLOAT: return NVN_FORMAT_RG32F; case VK_FORMAT_R32G32_UINT: return NVN_FORMAT_RG32UI; case VK_FORMAT_R32G32_SINT: return NVN_FORMAT_RG32I;
        case VK_FORMAT_R8G8B8A8_UNORM: return NVN_FORMAT_RGBA8; case VK_FORMAT_R8G8B8A8_SNORM: return NVN_FORMAT_RGBA8SN; case VK_FORMAT_R8G8B8A8_UINT: return NVN_FORMAT_RGBA8UI; case VK_FORMAT_R8G8B8A8_SINT: return NVN_FORMAT_RGBA8I;
        case VK_FORMAT_R16G16B16A16_SFLOAT: return NVN_FORMAT_RGBA16F; case VK_FORMAT_R16G16B16A16_UNORM: return NVN_FORMAT_RGBA16; case VK_FORMAT_R16G16B16A16_SNORM: return NVN_FORMAT_RGBA16SN;
        case VK_FORMAT_R16G16B16A16_UINT: return NVN_FORMAT_RGBA16UI; case VK_FORMAT_R16G16B16A16_SINT: return NVN_FORMAT_RGBA16I;
        case VK_FORMAT_R32G32B32A32_UINT: return NVN_FORMAT_RGBA32UI; case VK_FORMAT_R32G32B32A32_SINT: return NVN_FORMAT_RGBA32I;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return NVN_FORMAT_RGB10A2; case VK_FORMAT_A2B10G10R10_UINT_PACK32: return NVN_FORMAT_RGB10A2UI; case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return NVN_FORMAT_R11G11B10F;
        default: return NVN_FORMAT_RGBA32F;
    }
}
static nvn_hmap g_dummy_st = {.klen = 4};
static VkImageView dummy_storage(VkImageViewType type, VkFormat decl) {   // a 1×1 (×6) storage image in the declared format: writes to an unbound slot land there
    uint32_t nf = nvn_fmt_of(decl); vtex *d = nvn_hm_get(&g_dummy_st, &nf);
    if (!d) {
        tex_t t; memset(&t, 0, sizeof t); t.magic = MAGIC('T', 'E', 'X', 0); t.serial = ~0x100ull - nf; t.d.target = NVN_TEXTURE_TARGET_2D_ARRAY; t.d.format = nf;
        t.d.width = t.d.height = 1; t.d.depth = 6; t.d.levels = 1; t.d.flags = NVN_TEXTURE_FLAGS_IMAGE;
        if (!(d = vt_create(&t))) return VK_NULL_HANDLE;
        nvn_hm_put(&g_dummy_st, &nf, d);
    }
    if (type == VK_IMAGE_VIEW_TYPE_3D || type == VK_IMAGE_VIEW_TYPE_CUBE || type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) { RED_ONCE(111, "unbound 3D / cube image slot: a 2D array stand-in"); type = VK_IMAGE_VIEW_TYPE_2D_ARRAY; }
    return storage_view(d, NULL, type, decl);
}
// a multisampled 1×1 zero texture: an unbound multisample slot (a single-sample view there is an invalid descriptor)
static vtex *g_dummy_ms[2];
static VkImageView dummy_ms(int depth) {
    if (!g_dummy_ms[depth]) {
        VkSampleCountFlags ok = depth ? g_vk.props.limits.sampledImageDepthSampleCounts : g_vk.props.limits.sampledImageColorSampleCounts;
        int n = ok & VK_SAMPLE_COUNT_4_BIT ? 4 : ok & VK_SAMPLE_COUNT_2_BIT ? 2 : 0; if (!n) return VK_NULL_HANDLE;
        tex_t t; memset(&t, 0, sizeof t); t.magic = MAGIC('T', 'E', 'X', 0); t.serial = ~0x200ull - depth; t.d.target = NVN_TEXTURE_TARGET_2D_MULTISAMPLE; t.d.format = depth ? NVN_FORMAT_DEPTH32F : NVN_FORMAT_RGBA8;
        t.d.width = t.d.height = 1; t.d.depth = 1; t.d.levels = 1; t.d.samples = n;
        if (!(g_dummy_ms[depth] = vt_create(&t))) return VK_NULL_HANDLE;
    }
    return view_get(g_dummy_ms[depth], VK_IMAGE_VIEW_TYPE_2D, 0, 1, 0, 1, g_dummy_ms[depth]->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, NULL);
}
// ---- Each texture of a draw resolved once ----
// stage_textures resolved every texture element of a stage (its handle, the registry entry under g_reg_mu, vt_get) so that uploads come before any descriptor write, and
// stage_set resolved each again: Breath of the Wild on the Odin 2, the second vt_get 1.27% of the replay thread (0.51 ms a frame, 13,600 bindings), the registry lock taken
// per element 0.73%, samp_get's hash and compare 0.99%. stage_textures now copies the stage's registry entries under one lock, makes their images current outside it and
// keeps {ID, entry, image} per element for the stage's slot (g_tc); stage_set takes them instead of the registry and vt_get while two tokens read when stage_textures
// began are unchanged, each checked at every element:
//   - g_vt_gen (gfx_vk.c), the generation guard for the images themselves: a vtex is freed (vt_destroy) or a serial stops resolving to it (the g_tex deletions) only with it
//     bumped, so an image kept is alive and is what vt_get would answer (another stage's vt_get or rp_prepare's may make a serial anew: a texture registered as an image too)
//   - nvn_dirty_now(), the landing token for their contents: a packaged texture vt_get found clean (or uploaded) stays clean while no protected page was written, by the
//     game or by a write-back or copy the completion thread landed (dm_guest_write_prepare stamps the pages it unprotects), which is what vt_get would check again
//     (except an image adopted from its own write-back, ab_take: its edge pages may be unprotected, so it is not kept and stage_set asks vt_get again)
// A GPU texture's v->used is g_fence, which moves only at a submission's end. A packaged texture outside every tracked range (no span) was uploaded at both lookups; it is
// uploaded once. The element's ID is compared with the one kept (the handles cannot change between: they are bound state). A sampler ID's VkSampler is kept per
// registration (g_scache, emptied by reg_samp under g_reg_mu; VkSamplers are never destroyed). The kept entries go with the draw or dispatch (tc_clear).
// DM_VK_TEX_ONCE=0 (env.txt): every lookup again in stage_set, the lock per element, samp_get and vkb() for every texture, as before
static int tex_once(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_TEX_ONCE"); v = !(e && *e == '0'); if (!v) LOG("[vk] DM_VK_TEX_ONCE=0: stage_set resolves every texture again"); } return v; }
typedef struct { int tid; treg_t r; vtex *v; uint8_t rows; } tc_ent;   // r: the registry entry when stage_textures read it (r.ok 0: none), v its image (vt_get); rows: a texture slot that reads a buffer texture as rows (tex_rows_slot)
typedef struct { const vshader *s; uint64_t gen, tok; int n, cap; tc_ent *e; } tc_stage;   // gen, tok: g_vt_gen and nvn_dirty_now() when stage_textures began
static tc_stage g_tc[5];   // by stage slot (tc_slot); s NULL: nothing kept
static uint64_t g_tc_hit, g_tc_again, g_sc_hit, g_sc_made;   // elements stage_set took from stage_textures / resolved again; samplers found per ID / made or looked up
static int tc_slot(int stage) { return stage == SH_VS || stage == SH_MS ? 0 : stage == SH_PS ? 1 : stage == SH_GS || stage == SH_TK ? 2 : stage == SH_CS ? 4 : 3; }   // a mesh draw has no vertex or geometry shader
static void tc_clear(void) { for (int i = 0; i < 5; i++) g_tc[i].s = NULL; }
static const tc_ent *tc_el(const tc_stage *c, int ci, int tid) {   // stage_set's element ci (the stage's texture then image elements, buffer textures left out) as kept, or NULL
    if (!c || ci >= c->n || c->e[ci].tid != tid || (c->e[ci].r.ok && !c->e[ci].v) || c->gen != g_vt_gen || c->tok != nvn_dirty_now()) { g_tc_again++; return NULL; }   // no image made: asked again
    g_tc_hit++; return &c->e[ci];
}
// A stage's textures and images made current before any descriptor of the draw is written: their uploads read guest memory, so pending write-backs land
// first (cpu_view), and that must not close the chunk after this draw has added write-backs of its own
static void stage_textures(vshader *s) {
    int nst = nvn_stage_of(s->stage);
    if (tex_once()) {   // the entries copied under one lock, then made current outside it (an upload may wait for the GPU); kept for stage_set (tex_once)
        tc_stage *c = &g_tc[tc_slot(s->stage)]; c->s = NULL; c->n = 0;
        uint64_t gen = g_vt_gen, tok = nvn_dirty_now(); int nel = 0;
        for (int pass = 0; pass < 2; pass++) for (int i = 0; i < (pass ? s->nim : s->ntx); i++) { const rb_tex *t = pass ? &s->im[i] : &s->tx[i];
            if ((t->type & 0xff) != 2) nel += t->array > 1 ? (t->array > TEX_ARR_MAX ? TEX_ARR_MAX : t->array) : 1; }
        if (nel > c->cap) { int nc = nel > 16 ? nel : 16; tc_ent *ne = realloc(c->e, (size_t)nc * sizeof *ne); if (!ne) goto each; c->e = ne; c->cap = nc; }
        uint8_t c2buf[C2_SIZE]; const uint8_t *c2p = c2_bank(nst, c2buf); int n = 0;
        pthread_mutex_lock(&g_reg_mu);
        for (int pass = 0; pass < 2; pass++) for (int i = 0; i < (pass ? s->nim : s->ntx); i++) {
            const rb_tex *t = pass ? &s->im[i] : &s->tx[i]; if ((t->type & 0xff) == 2) continue;
            int cnt = t->array > 1 ? (t->array > TEX_ARR_MAX ? TEX_ARR_MAX : t->array) : 1; uint32_t w0 = (uint32_t)t->handle & 0x3fff;
            for (int e = 0; e < cnt; e++, n++) { int tid = (int)(bank_word(nst, c2p, t->cbuf_slot, (w0 + 2u * e) * 4) & 0xfffff); if (tid && t->cbuf_slot >= 3) tid += D.tex_base;
                tc_ent *x = &c->e[n]; x->tid = tid; x->rows = !pass && tex_rows_slot(t); if (tid > 0 && tid < g_ntreg && g_treg[tid].ok) x->r = g_treg[tid]; else x->r.ok = 0;
                if (x->rows && x->r.ok && x->r.t.d.target == NVN_TEXTURE_TARGET_BUFFER) s->tbrows = 1; }
        }
        pthread_mutex_unlock(&g_reg_mu);
        for (int j = 0; j < n; j++) { vtex *v = c->e[j].r.ok ? vt_get_s(&c->e[j].r.t, c->e[j].rows) : NULL;
            // an image adopted from its own write-back (gfx_vk.c ab_take) comes back with seen at the note's stamp, not the clock: an edge page a neighbour unprotected
            // after the note can still be written with no stamp, so the clock is no landing token for it (vt_get looks at its pages again, and so does stage_set then)
            c->e[j].v = v && v->pkg && v->seen != nvn_dirty_now() ? NULL : v; }
        c->n = n; c->gen = gen; c->tok = tok; c->s = s;
        return;
    }
    each:
    {
        uint8_t c2buf[C2_SIZE]; const uint8_t *c2p = c2_bank(nst, c2buf);
        for (int pass = 0; pass < 2; pass++) for (int i = 0; i < (pass ? s->nim : s->ntx); i++) {
            const rb_tex *t = pass ? &s->im[i] : &s->tx[i]; if ((t->type & 0xff) == 2) continue;
            int cnt = t->array > 1 ? (t->array > TEX_ARR_MAX ? TEX_ARR_MAX : t->array) : 1; uint32_t w0 = (uint32_t)t->handle & 0x3fff;
            for (int e = 0; e < cnt; e++) { int tid = (int)(bank_word(nst, c2p, t->cbuf_slot, (w0 + 2u * e) * 4) & 0xfffff); if (tid && t->cbuf_slot >= 3) tid += D.tex_base;
                pthread_mutex_lock(&g_reg_mu); tex_t tt; int ok = tid > 0 && tid < g_ntreg && g_treg[tid].ok; if (ok) tt = g_treg[tid].t; pthread_mutex_unlock(&g_reg_mu);
                int rows = !pass && tex_rows_slot(t); if (ok && rows && tt.d.target == NVN_TEXTURE_TARGET_BUFFER) s->tbrows = 1;
                if (ok) vt_get_s(&tt, rows); }
        }
    }
}
// A draw's copy of a constant or storage buffer reuses the copy an earlier draw of the same chunk made of the same bytes: same source and size, and the source still
// equal to a shadow kept in host memory (the arena may be write-combined, slow to read: ar_mem_type). Super Mario Bros. Wonder's scene draws read six or seven uniform buffers
// each, mostly the same from draw to draw; copying them again for every draw was the largest cost of its render thread on an Adreno 619 (memcpy, 15%)
typedef struct { const void *src; size_t n; uint64_t seq; VkBuffer b; VkDeviceSize off; uint8_t *shadow; size_t cap; } ucopy_t;
static int support_skip(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_SUPPORT_SKIP"); v = !(e && *e == '0'); } return v; }   // stage_set
static int desc_scratch(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_DESC_SCRATCH"); v = !(e && *e == '0'); } return v; }   // stage_set
// DM_VK_EXT_CHECK=1 (env.txt; a diagnostic, off by default): a constant buffer the shader is said to read less of than is bound (cb_ext_pick) is copied whole, the bytes past
// the extent replaced by NaN (0x7fc00000 words), and bound whole: an extent too small, which would corrupt the shader's constants, shows as a broken picture (compare a capture
// with the switch off). Not cached (ucopy), so slower
static uint64_t g_ext_checked;   // bindings copied so (stage_set)
static int ext_check(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_EXT_CHECK"); v = e && *e && *e != '0'; if (v) LOG("[vk] DM_VK_EXT_CHECK: constant buffers read NaN past their extents"); } return v; }
static uint64_t g_sup_written, g_sup_skipped;   // stages that wrote the support buffer / left it unwritten (stage_set)
// g_ucopy_bytes / g_ucopy_shadow: what the misses copied to the arena and again to the shadows, two memcpy of the same n bytes, both saved by a smaller extent (cb_ext_pick);
// DM_VK_EXT_CHECK's whole copies are in neither, so with it on they do not compare with a run without
static ucopy_t g_ucopy[1024]; static uint64_t g_ucopy_hit, g_ucopy_miss, g_ucopy_bytes, g_ucopy_shadow;
static int ucopy(const void *src, size_t n, size_t al, VkBuffer *b, VkDeviceSize *off) {   // src NULL: al zero bytes (not cached)
    if (!src) { uint8_t *d = ar_alloc(al, b, off); if (!d) return 0; memset(d, 0, al); return 1; }
    uintptr_t h = ((uintptr_t)src >> 4) ^ ((uintptr_t)src >> 14) ^ n * 0x9e37u; ucopy_t *e = &g_ucopy[h & 1023];
    if (C && e->src == src && e->n == n && e->seq == C->seq && !memcmp(src, e->shadow, n)) { *b = e->b; *off = e->off; g_ucopy_hit++; return 1; }
    uint8_t *d = ar_alloc(al, b, off); if (!d) return 0;
    memcpy(d, src, n); if (al > n) memset(d + n, 0, al - n);
    g_ucopy_miss++; g_ucopy_bytes += n;
    if (!C) return 1;
    if (e->cap < n) { free(e->shadow); e->shadow = malloc(n); e->cap = e->shadow ? n : 0; }
    if (e->shadow) { memcpy(e->shadow, src, n); g_ucopy_shadow += n; e->src = src; e->n = n; e->seq = C->seq; e->b = *b; e->off = *off; } else e->src = NULL;
    return 1;
}
#include "gfx_vk_sbcopy.inc"   // read-only storage buffers outside placed pools: a chunk's copy bound again while the dirty tracker shows its pages unwritten
// ---- The program-to-shader memo (vk_draw_) ----
// Which shader each stage of a draw takes depends on the five bound programs (D.bst) alone: prog_key reads the program's key record (hle_nvn.c pk_get, found by the program's
// address), set at SetShaders or computed once at the first draw, and sh_get's table only grows (a vshader is never freed, a failed one stays failed). Resolving it again
// for every draw (two g_pk_mu round trips and a CFDictionary lookup per program, a hex key, a hash and a 16-byte compare per stage, and stage_set's prog_c1 with g_pk_mu
// again) cost Breath of the Wild on the Odin 2 about 3.2% of the replay thread (prog_key 1.15%, sh_get 1.1%, prog_c1 0.36%, the loop 0.5%: about 1.2 ms a frame), where
// 90% of the draws bind the programs of the draw before. An entry is keyed on all five (program address, serial) pairs, as nvn_mtl.m bind_stages keys its ring: SetShaders
// gives a program a new serial (nvn_new_serial), a by-value copy of an NVNprogram has another address (and its own key record, found by address), and a program of
// serial 0 (initialized and never set: a struct the game wrote itself) is never memoized. Any Finalize or SetShaders empties the memo (g_prog_gen), moved before they drop
// or reset a key record (gfx_vk.c mtl_prog_release) and again once it is dropped or filled (mtl_prog_keys_changed): they run on the game's threads while the replay draws, and
// an entry made from a record in between (SetShaders gives the new serial before the keys and c1 ranges) holds a generation read before the second move and no longer hits
// once that lands, so a change races the draws made during it alone, as resolving every draw does. Only a complete resolution is kept: a missing
// shader, a tessellation stage or no vertex shader takes the full path every time, with its counters and logs. An entry holds the shaders, their stage index in the program
// and each stage's c1 range (prog_c1), never a program pointer (pprog[slot] is D.bst[slot]) nor c1's bytes (stage_set copies them from the program's code every draw).
// The replay alone uses it (mtl_exec, under g_bk). DM_VK_PROG_MEMO=0 (env.txt): resolved for every draw, as before
static int prog_memo_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_PROG_MEMO"); v = !(e && *e == '0'); if (!v) LOG("[vk] DM_VK_PROG_MEMO=0: every draw resolves its programs' shaders"); } return v; }
// DM_VK_MEMO_LOG=1 (env.txt; a diagnostic, off by default): the per-draw memos' counts in the periodic statistics (mtl_mem_stat), cumulative
static int memo_log(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_MEMO_LOG"); v = e && *e && *e != '0'; } return v; }
enum { PMEMO = 512 };   // direct-mapped by the pairs' hash: a frame of Breath of the Wild binds about 250 program pairs
typedef struct { const prog_t *pr[5]; uint64_t ser[5], gen; vshader *sh[4]; int pst[4]; int8_t c1ok[4]; uint32_t c1[4][2]; } prog_memo_t;   // c1ok: prog_c1's result, -1 not asked
static prog_memo_t g_pmemo[PMEMO]; static uint64_t g_pmemo_hit, g_pmemo_miss, g_pmemo_ser0;
// the c1 ranges of the draw being made (vk_draw_ to stage_set, also its tf_capture): -1 when the memo has none for (pr, stage), else prog_c1's result into *o / *e
static struct { const prog_t *pr; int st, ok; uint32_t o, e; } g_c1d[4]; static int g_c1d_on;
static int c1_memo(const prog_t *pr, int st, uint32_t *o, uint32_t *e) {
    if (!g_c1d_on) return -1;
    for (int i = 0; i < 4; i++) if (g_c1d[i].pr == pr && g_c1d[i].st == st && g_c1d[i].ok >= 0) { *o = g_c1d[i].o; *e = g_c1d[i].e; return g_c1d[i].ok; }
    return -1;
}
static int has_c1(const vshader *s) { for (int i = 0; i < s->ncb; i++) if (s->cb[i].slot == 1) return 1; return 0; }
// ---- Sets written in this recording, by what they hold (set_reuse) ----
// Direct-mapped by a hash of the layout and the descriptors (binding, type, count, then each buffer, offset and range, sampler and view, or texel buffer view; a dynamic
// buffer's offset is 0 there), compared whole on a hit. An entry holds only while its recording does (seq): a set is one chunk's, written once in a recording and free again
// once that chunk completes (ds_get), so it is bound again only in the command buffer it was written for. A view or image dropped while a chunk records is destroyed only
// once that chunk completes (dead_add), so a handle in an entry names the same object for the whole recording. Sets of the capture variant (dsl_tf) are not kept: tf_capture
// writes the feedback buffers into the set after stage_set. A set too large to describe in SR_SIG words is written every time
enum { SR_N = 4096, SR_SIG = 2048 };
typedef struct { uint64_t seq, h; VkDescriptorSetLayout l; VkDescriptorSet set; uint32_t n, cap; uint64_t *sig; } sr_ent;
static sr_ent g_sr[SR_N]; static uint64_t g_sig[SR_SIG], g_sig_h; static uint32_t g_nsig;   // the description sr_find made last, for sr_put
static uint64_t g_sr_hit, g_sr_miss, g_sr_long;   // sets bound again; written and kept; too large to keep (cumulative)
// the entry for layout l holding wr's descriptors (their sets not named yet) → *hit 1 when it holds a set so written in this recording; NULL when too large to describe
static sr_ent *sr_find(VkDescriptorSetLayout l, const VkWriteDescriptorSet *wr, uint32_t nw, int *hit) {
    uint32_t n = 0; *hit = 0;
    for (uint32_t i = 0; i < nw; i++) { const VkWriteDescriptorSet *w = &wr[i]; uint32_t k = w->descriptorCount;
        if (n + 1 + 3 * k > SR_SIG) { g_sr_long++; return NULL; }
        g_sig[n++] = (uint64_t)w->dstBinding << 40 | (uint64_t)w->descriptorType << 20 | k;
        for (uint32_t j = 0; j < k; j++) {
            if (w->pBufferInfo) { g_sig[n++] = (uint64_t)w->pBufferInfo[j].buffer; g_sig[n++] = w->pBufferInfo[j].offset; g_sig[n++] = w->pBufferInfo[j].range; }
            else if (w->pImageInfo) { g_sig[n++] = (uint64_t)w->pImageInfo[j].sampler; g_sig[n++] = (uint64_t)w->pImageInfo[j].imageView; }   // every image is in GENERAL
            else g_sig[n++] = (uint64_t)w->pTexelBufferView[j]; } }
    uint64_t h = (uint64_t)l * 0x9e3779b97f4a7c15ull;
    for (uint32_t i = 0; i < n; i++) h = (h ^ g_sig[i]) * 0xff51afd7ed558ccdull;
    h ^= h >> 29; g_sig_h = h; g_nsig = n;
    sr_ent *e = &g_sr[(h ^ h >> 32) & (SR_N - 1)];
    *hit = e->seq == C->seq && e->l == l && e->h == h && e->n == n && !memcmp(e->sig, g_sig, n * sizeof *g_sig);
    return e;
}
static void sr_put(sr_ent *e, VkDescriptorSetLayout l, VkDescriptorSet set) {   // set written with sr_find's last description, in this recording
    if (e->cap < g_nsig) { uint32_t c = g_nsig < 64 ? 64 : g_nsig; uint64_t *x = realloc(e->sig, c * sizeof *x); if (!x) { e->seq = 0; return; } e->sig = x; e->cap = c; }
    memcpy(e->sig, g_sig, g_nsig * sizeof *g_sig); e->n = g_nsig; e->h = g_sig_h; e->l = l; e->set = set; e->seq = C->seq;
}
// The set of shader s's stage for the draw being made into *pset, and the offsets of its dynamic uniform buffers into dyn (s->ndyn of them, by binding): a set written with
// the same descriptors in this recording (sr_find), else one from the chunk (ds_get; tf: of dsl_tf) written here. dseq: the chunk recorded when the draw's first stage began
// (C->seq after cur()): nothing is written or taken once another is recorded (ds_moved), and the caller tells that 0 from another failure by ds_moved
static int stage_set(vshader *s, int tf, VkDescriptorSet *pset, uint32_t *dyn, const prog_t *pr, int prog_stage, uint32_t base_inst, uint64_t dseq) {
    g_alias_key = s->key;
    int bcs = s->stage == SH_CS && bc_on(); if (bcs) { g_bc_dn = 0; g_bc_desc[0] = 0; }   // the bindings text of a dispatch's breadcrumb (gfx_vk_bc.inc)
    int nst = nvn_stage_of(s->stage);
    VkDescriptorSet set = VK_NULL_HANDLE; *pset = VK_NULL_HANDLE;   // the writes name their set once it is known (below)
    // one write per binding; every element of an array is written (an unwritten descriptor is undefined)
    VkWriteDescriptorSet wr[1 + CB_MAX + 16 + 64 + 16 + 1]; VkDescriptorBufferInfo bi[1 + CB_MAX + 16 + 1]; VkDescriptorImageInfo *ii; VkBufferView *bvs; uint32_t nw = 0, nbi = 0, nii = 0, nbv = 0;
    uint8_t c2buf[C2_SIZE]; const uint8_t *c2 = c2_bank(nst, c2buf);   // NULL: handle words read with c2_word, the whole bank made only for a c2 constant buffer (below)
    // support buffer: render scale 1, the viewport's inverse and size. Written only for a stage whose modules read it (sh_get spv_b0_used): a descriptor no instruction of the
    // pipeline uses need not be valid (Vulkan: VUID-vkCmdDraw-None-08114, valid "if they are statically used"), so binding 0 stays in the layout, unwritten. Breath of the Wild
    // on the Odin 2: 41 of a frame's 7,506 stages read it, and its 1,392-byte ucopy compare in every stage cost 1.05 ms a frame (2.6% of the replay thread), its descriptor
    // write about 0.3 ms more. DM_VK_SUPPORT_SKIP=0 (env.txt): written for every stage, as before
    VkBuffer b; VkDeviceSize off;
    if (s->b0 || !support_skip()) {
        static uint8_t p[1392]; static int filled;   // filled: a flag (a counter here, `if (!once++)`, overflowed after 2^31 stages, about 4.4 hours at 136,000 a second)
        float vw = D.has_vp ? D.vp[0][2] : (float)P.w, vh = D.has_vp ? D.vp[0][3] : (float)P.h;
        float vinv[4] = {vw ? 2.f / vw : 0, vh ? 2.f / vh : 0, 0, 0}, vsz[4] = {vw, vh, 1, 0}; memcpy(p + 144, vinv, 16); memcpy(p + 160, vsz, 16);
        if (!filled) { for (int i = 0; i < 73; i++) { float one = 1.f; memcpy(p + 192 + 16 * i, &one, 4); } filled = 1; }
        if (!ucopy(p, 1392, 1392, &b, &off)) return 0;
        bi[nbi] = (VkDescriptorBufferInfo){b, off, 1392}; wr[nw++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, NULL, &bi[nbi++], NULL};
        g_sup_written++;
    } else g_sup_skipped++;
    for (int i = 0; i < s->ncb; i++) {
        int slot = s->cb[i].slot; size_t n = 16; const void *src = NULL; uint8_t bank[0xA00];
        if (slot == 0) { bank_c0(nst, bank, base_inst); src = bank; n = 0x800; }
        else if (slot == 2) { if (!c2) bank_c2(nst, bank); src = c2 ? c2 : bank; n = C2_SIZE; }
        else if (slot == DMSH_PUSH_BANK) { src = D.push[nst == 5]; n = DMSH_PUSH_SIZE; }   // NVN2 push constants: compute's block or the graphics stages' (OP_PUSH_CONST)   // c2 read as a constant buffer: 81 of Breath of the Wild's 19,592 shaders, none of a measured frame's
        else if (slot == DMSH_CBSIZE_BANK) {   // the uniform slots' bound sizes in bytes (nvn_mtl.m NVN_RB_CBSIZE), which the translator's uniform reads compare with: past its slot's size a read gives 0, as Maxwell's constant buffers do
            memset(bank, 0, 16 * ((DMSH_CBSIZE_SLOTS + 3) / 4));
            for (int r = 0; r < DMSH_CBSIZE_SLOTS; r++) { const p_bind_buf *u = &D.ubo[nst][r]; uint32_t v = u->addr ? (u->size < 65536 ? (uint32_t)u->size : 65536) : 0; memcpy(bank + 4 * r, &v, 4); }
            src = bank; n = 16 * ((DMSH_CBSIZE_SLOTS + 3) / 4); }
        else if (slot == 1) {   // the shader's own constant segment, in its code memory: its bounds were read from the control section at SetShaders (gone by now: Danganronpa V3's movie shader read zeros)
            const NVNshaderData *sd = pr && prog_stage >= 0 ? &pr->sd[prog_stage] : NULL; uint32_t o = 0, e = 0; int c1m;
            if (sd && sd->data && ((c1m = c1_memo(pr, prog_stage, &o, &e)) >= 0 ? c1m : prog_c1(pr, (unsigned)prog_stage, &o, &e)) && e > o && e - o <= 0x10000) { src = (const void *)(uintptr_t)(sd->data + o); n = e - o; }
        } else if (slot >= 3 && slot - 3 < 16) { const p_bind_buf *u = &D.ubo[nst][slot - 3];
            if (u->addr && u->size) { src = (const void *)(uintptr_t)u->addr; n = u->size > 0x10000 ? 0x10000 : (size_t)u->size;
                size_t rd = s->cb[i].ext && n > s->cb[i].ext ? s->cb[i].ext : n;   // in a memory pool still, as storage buffers below (else it reads 0)
                if (s->gm) gm_lazy_ptrs(u->addr, rd);   // its pointers' chunks placed too (gfx_vk_gmem_lazy.inc): a raw load may take its address from a uniform buffer
                if (!mtl_addr(u->addr, rd, NULL, NULL)) { RED_ONCE(119, "uniform buffer slot %d of shader %s at %#llx is in no memory pool: it reads 0", slot - 3, s->key, (unsigned long long)u->addr); src = NULL; n = 16; } }
            else { static int nlog; if (nlog < 8) { nlog++; LOG("[RED] [vk] shader %s reads uniform slot %d, which has no buffer bound: it reads 0", s->key, slot - 3); } } }
        size_t nall = n;   // (ext_check)
        if (bcs) { bc_addf(" | c%d b%d %s%zu bytes", slot, s->cb[i].binding, src ? "" : "none, ", n); if (src) bc_words("", src, n, 16); }
        if (s->cb[i].ext && n > s->cb[i].ext) n = s->cb[i].ext;   // only what the shader reads (cb_ext_pick)
        if (dump_on() && slot != 0 && slot != 2) {   // the draw's constant buffers in the dump's log: c1 (the shader's own segment) and the game's uniform buffers, the first 8 vec4
            char vb[700]; int q = 0; const float *fv = src; size_t nf = src ? n / 4 : 0; if (nf > 32) nf = 32;
            for (size_t j = 0; j < nf && q < (int)sizeof vb - 20; j++) q += snprintf(vb + q, sizeof vb - q, "%s%g", j % 4 ? " " : j ? ") (" : "(", fv[j]);
            LOG("[dump]   %s c%d: %s %zu bytes %s%s", s->key, slot, src ? "read" : "no source,", n, vb, nf ? ")" : "");
        }
        size_t al = (n + 15) & ~(size_t)15;
        if (nall > n && ext_check()) {   // the whole range bound, NaN past the extent: a shader reading past it draws wrong
            size_t aa = (nall + 15) & ~(size_t)15, q = (n + 3) & ~(size_t)3; uint8_t *d = ar_alloc(aa, &b, &off); const uint32_t nan = 0x7fc00000u; if (!d) return 0;
            if (src) memcpy(d, src, n); else memset(d, 0, n);
            memset(d + n, 0, q - n); for (; q + 4 <= aa; q += 4) memcpy(d + q, &nan, 4);
            al = aa; g_ext_checked++;
        } else if (slot >= 3 && slot - 3 < 16 && src && src == (const void *)(uintptr_t)D.ubo[nst][slot - 3].addr && ub_fwd(D.ubo[nst][slot - 3].addr, n, &b, &off)) g_buf_fwd[0]++;   // a pending write read on the GPU (wb_promote, buf_fwd)
        else if (!ucopy(src, n, al, &b, &off)) return 0;
        if (slot == 0 && nst == 5) { g_c0_b = b; g_c0_off = off; }   // (vk_dispatch: an indirect dispatch's counts copied here on the GPU)
        int dy = s->cbdyn[i] != 0xff; if (dy) dyn[s->cbdyn[i]] = (uint32_t)off;   // a dynamic one's offset goes to the bind, its descriptor at 0
        bi[nbi] = (VkDescriptorBufferInfo){b, dy ? 0 : off, al};
        wr[nw++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, (uint32_t)s->cb[i].binding, 0, 1, dy ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, NULL, &bi[nbi++], NULL};
    }
    for (int i = 0; i < s->nsb; i++) {   // the address in c0 at the translator's offset names one of the bound storage buffers
        // where the address is (dmsh_build.py reflect): c0's storage buffer table of the stage, compute's uniform buffer table (c0 + 0x210 + 16 r: a uniform buffer read through
        // global memory), or the game's own uniform buffer (c3 + slot: {address, size} at the offset); cb_offset counts words
        const rb_sbuf *sb = &s->sb[i]; uint32_t off = 4u * (uint32_t)sb->cb_off, base = (uint32_t)c_bases[nst] << 4;
        if (sb->cb_slot == DMSH_GMEM_MAP_CB_SLOT && gm_on()) {   // not a game buffer: the raw pool table or the stores' write log (gfx_vk_gmem.inc); off, 16 zero bytes as below
            VkDeviceSize ao, rng; if (!gm_bind(sb->cb_off, &b, &ao, &rng)) return 0;
            bi[nbi] = (VkDescriptorBufferInfo){b, ao, rng}; wr[nw++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, (uint32_t)sb->binding, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bi[nbi++], NULL};
            continue; }
        p_bind_buf tmp = {0}; const p_bind_buf *u = NULL;
        if (sb->cb_slot == 0 && nst == 5 && off >= 0x210 && off < 0x310 && !((off - 0x210) % 16)) u = &D.ubo[5][(off - 0x210) / 16];
        else if (sb->cb_slot == 0 && base && off >= base && off - base < 0x100 && !((off - base) % 16)) u = &D.ssbo[nst][(off - base) / 16];
        else if (sb->cb_slot >= 3 && sb->cb_slot - 3 < 16) { const p_bind_buf *g = &D.ubo[nst][sb->cb_slot - 3];
            if (g->addr && off + 12 <= g->size) { uint64_t ad; uint32_t sz; memcpy(&ad, (const void *)(uintptr_t)(g->addr + off), 8); memcpy(&sz, (const void *)(uintptr_t)(g->addr + off + 8), 4);
                if (ad && sz && mtl_addr(ad, sz, NULL, NULL)) { tmp.addr = ad; tmp.size = sz; u = &tmp; } } }
        size_t n = u && u->addr ? (size_t)u->size : 16;
        // a binding whose range is in no memory pool any more (the game finalized the pool and left the binding): the GPU would find no pages there, the replay's copy of it
        // faulted (Super Mario Bros. Wonder on Android: SIGSEGV in memcpy from stage_set); read as unbound, as copies and indirect arguments outside every pool are
        if (u && u->addr && !mtl_addr(u->addr, n, NULL, NULL)) { RED_ONCE(118, "storage buffer of shader %s at %#llx (%zu bytes) is in no memory pool: it reads 0", s->key, (unsigned long long)u->addr, n); u = NULL; n = 16; }
        if (bcs) { bc_addf(" | storage b%d c%d+%#x %s %#llx %zu bytes", sb->binding, sb->cb_slot, off, sb->flags & 1 ? "written" : "read", u ? (unsigned long long)u->addr : 0ull, n);
            if (u && u->addr) bc_words("", (const void *)(uintptr_t)u->addr, n, 32); }
        if (!u || !u->addr) RED_ONCE(91, "storage buffer of shader %s (c%d +%#x) is not a bound buffer: it reads 0", s->key, sb->cb_slot, off);
        if (s->gm && gm_check_on() && u && u->addr) gm_check(s, sb->cb_slot, off, u->addr, n);   // DM_VK_RAW_GMEM_CHECK (gfx_vk_gmem.inc)
        if (s->gm && u && u->addr) gm_lazy_ptrs(u->addr, n);   // chunks its pointers reach, placed (gfx_vk_gmem_lazy.inc)
        VkDeviceSize ao, ipr;   // ao: the arena offset (off above is the c0 / uniform buffer word); ipr: the range bound in place
        int wr_ = (sb->flags & 1) != 0;   // written by the shader
        g_rd_cs = disp_like(s) || ((s->stage == SH_MS || s->stage == SH_TK) && !g_mesh);
        if (u && u->addr && (!wr_ || disp_like(s)) && sb_fwd(u->addr, n, wr_, &b, &ao)) g_buf_fwd[wr_]++;   // on the GPU from a dispatch's pending write (buf_fwd)
        else if (u && u->addr && wr_ && g_vdisp && rd_home_on() && rd_bind_w(u->addr, n, &b, &ao)) {}   // a discarded draw's write: on its home range's copy (rd_bind_w)
        else if (u && u->addr && !wr_ && gm_inplace(u->addr, n, &b, &ao, &ipr)) {   // read only, in a placed pool: bound there (gfx_vk_gmem_inplace.inc)
            bi[nbi] = (VkDescriptorBufferInfo){b, ao, ipr}; wr[nw++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, (uint32_t)sb->binding, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bi[nbi++], NULL};
            continue; }
        else if (u && u->addr && own_alias(s->key, u->addr, n, g_vk.props.limits.minStorageBufferOffsetAlignment, &b, &ao)) {}   // a range another binding of this shader wrote
        else if (u && u->addr && !wr_) { if (!sb_keep(u->addr, n, &b, &ao) && !sb_copy((const void *)(uintptr_t)u->addr, n, (n + 15) & ~(size_t)15, &b, &ao)) return 0; }   // read only: kept between frames (sb_keep), or shared as uniforms are
        else if (u && u->addr && gm_inplace_w(u->addr, n, disp_like(s), &b, &ao, &ipr)) {   // written, in a placed pool: bound there, its write-back only a note (gfx_vk_gmem_inplace.inc)
            bi[nbi] = (VkDescriptorBufferInfo){b, ao, ipr}; wr[nw++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, (uint32_t)sb->binding, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bi[nbi++], NULL};
            continue; }
        else if (u && u->addr && ofw_ok(sb, g_rd_cs, u->addr, n)) { if (!ofw_bind(sb, u->addr, n, &b, &ao)) return 0; }   // written in an order-free way: on its chunk's copy, or a new one that starts as the identity (draw_view made no sync for it: never a copy of guest memory)
        else if (u && u->addr) { uint8_t *d = ar_alloc((n + 15) & ~(size_t)15, &b, &ao); if (!d) return 0; memcpy(d, (const void *)(uintptr_t)u->addr, n);
            if (sb->flags & 1) {   // written by the shader: what it changed is copied back when the chunk completes
                wb_t *w = wb_add(); uint8_t *snap = malloc(n); if (snap) memcpy(snap, (const void *)(uintptr_t)u->addr, n);   // from guest memory, as d was: the arena may be write-combined (ar_mem_type)
                if (w) { *w = (wb_t){-1 - C->ar_last, (size_t)ao, n, 1, n, n, (uint8_t *)(uintptr_t)u->addr, snap}; w->fwd = disp_like(s); w->vd = g_vdisp; if (w->fwd) C->nfwd++; } } }
        else if (!ucopy(NULL, 16, 16, &b, &ao)) return 0;
        bi[nbi] = (VkDescriptorBufferInfo){b, ao, (n + 15) & ~(size_t)15}; wr[nw++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, (uint32_t)sb->binding, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bi[nbi++], NULL};
    }
    if (s->tgs) {   // the dispatch's shared memory (spv_tgspill): a region of the arena for each workgroup of it (g_cdim: indirect counts already read)
        VkDeviceSize sz = (VkDeviceSize)s->tgs * g_cdim[0] * g_cdim[1] * g_cdim[2];
        if (!sz || sz > g_vk.props.limits.maxStorageBufferRange) { RED_ONCE(179, "compute shader %s: %u x %u x %u workgroups of %u bytes of shared memory pass the GPU's largest storage buffer: skipped", s->key, g_cdim[0], g_cdim[1], g_cdim[2], s->tgs); g_bind_bad = 1; }
        else { VkDeviceSize to;
            if (g_tgs_b) { b = g_tgs_b; to = g_tgs_off; }   // a task / mesh kernel in compute form: a range of that arena (gfx_vk_meshemu.inc memu_kernel)
            else if (!ar_alloc(sz, &b, &to)) return 0;
            bi[nbi] = (VkDescriptorBufferInfo){b, to, sz}; wr[nw++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, SPV_TGS_BINDING, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bi[nbi++], NULL}; }
    }
    size_t nel = 0; for (int i = 0; i < s->ntx; i++) nel += s->tx[i].array > 1 ? (size_t)(s->tx[i].array > TEX_ARR_MAX ? TEX_ARR_MAX : s->tx[i].array) : 1;
    for (int i = 0; i < s->nim; i++) nel += s->im[i].array > 1 ? (size_t)(s->im[i].array > TEX_ARR_MAX ? TEX_ARR_MAX : s->im[i].array) : 1;
    // the texture and image descriptors' infos in a scratch kept from call to call, grown to nel + 1 here, before any pointer into it is in wr[], and never in the middle: a malloc
    // and a free of each per stage cost Breath of the Wild 1.6 ms a frame on the Odin 2 (30,000 calls, mostly scudo's lock atomics). One scratch serves because stage_set is
    // the replay's alone: its callers (vk_draw, tf_capture, vk_dispatch) run only in mtl_exec, between mtl_sub_begin and mtl_sub_commit under g_bk, nothing it calls reaches
    // them again, and Vulkan reads pImageInfo / pTexelBufferView only during vkUpdateDescriptorSets (as the static support buffer p above, and C).
    // DM_VK_DESC_SCRATCH=0 (env.txt): a malloc and a free of each per stage, as before (the A/B of this alone)
    static VkDescriptorImageInfo *g_ii; static VkBufferView *g_bvs; static size_t g_icap; int own = !desc_scratch();
    if (own) { ii = malloc((nel + 1) * sizeof *ii); bvs = malloc((nel + 1) * sizeof *bvs); if (!ii || !bvs) { free(ii); free(bvs); return 0; } }
    else { if (nel + 1 > g_icap) { size_t c = nel + 1 > 64 ? nel + 1 : 64; VkDescriptorImageInfo *ni = realloc(g_ii, c * sizeof *ni); if (!ni) return 0; g_ii = ni;
               VkBufferView *nb = realloc(g_bvs, c * sizeof *nb); if (!nb) return 0; g_bvs = nb; g_icap = c; }
           ii = g_ii; bvs = g_bvs; }   // (no early return past here but ds_moved's below, which frees them)
    const tc_stage *tc = NULL; int ci = 0;   // the elements stage_textures resolved (tex_once), taken once
    if (tex_once()) { tc_stage *c = &g_tc[tc_slot(s->stage)]; if (c->s == s) tc = c; c->s = NULL; }
    pthread_mutex_lock(&g_reg_mu);
    for (int i = 0; i < s->ntx; i++) {
        const rb_tex *t = &s->tx[i]; int cnt = t->array > 1 ? (t->array > TEX_ARR_MAX ? TEX_ARR_MAX : t->array) : 1;
        if (t->array > TEX_ARR_MAX) RED_ONCE(92, "texture array of %d elements: the first %d are bound", t->array, TEX_ARR_MAX);
        VkImageViewType vt = view_type_of(t->type); int tbuf = (t->type & 0xff) == 2;
        uint32_t w0 = (uint32_t)t->handle & 0x3fff, w1 = ((uint32_t)t->handle >> 14) & 0x3fff, ty = (uint32_t)t->handle >> 28;
        if (tbuf) {   // a buffer texture: a uniform texel buffer
            for (int e = 0; e < cnt; e++) {
                uint32_t hw = bank_word(nst, c2, t->cbuf_slot, (w0 + 2u * e) * 4); int tid = (int)(hw & 0xfffff); if (tid && t->cbuf_slot >= 3) tid += D.tex_base;
                VkBufferView v = tid > 0 && tid < g_ntreg && g_treg[tid].ok ? tbuf_view(&g_treg[tid].t, 0, disp_like(s)) : VK_NULL_HANDLE;
                bvs[nbv + e] = v ? v : dummy_tbuf();
            }
            wr[nw++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, (uint32_t)t->binding, 0, (uint32_t)cnt, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, NULL, NULL, &bvs[nbv]};
            nbv += cnt; continue;
        }
        for (int e = 0; e < cnt; e++) {
            uint32_t hw = bank_word(nst, c2, t->cbuf_slot, (w0 + 2u * e) * 4), sw = hw;
            if (ty == 1) sw = bank_word(nst, c2, t->samp_slot, w1 * 4);
            int ubo = t->cbuf_slot >= 3, subo = (ty == 1 ? t->samp_slot : t->cbuf_slot) >= 3;
            int tid = (int)(hw & 0xfffff); if (tid && ubo) tid += D.tex_base;
            int sid = ty == 2 ? (int)w1 : (int)(sw >> 20); if (sid && subo && ty != 2) sid += D.samp_base;
            VkImageView iv = VK_NULL_HANDLE;
            int ms = (t->type & 0x200) != 0;
            const tc_ent *ce = tex_once() ? tc_el(tc, ci++, tid) : NULL;   // as stage_textures resolved it, or from the registry again
            const treg_t *rg = ce ? (ce->r.ok ? &ce->r : NULL) : tid > 0 && tid < g_ntreg && g_treg[tid].ok ? &g_treg[tid] : NULL;
            if (rg) {
                vtex *v = ce ? ce->v : vt_get_s(&rg->t, tex_rows_slot(t));
                if (v && (v->samples > 1) != ms) { RED_ONCE(112, "a %s texture in a %s slot (shader %s): reads 0", v->samples > 1 ? "multisample" : "single-sample", ms ? "multisample" : "single-sample", s->key); v = NULL; }
                if (v) { const view_t *vv = rg->has_v ? &rg->v : NULL;
                    uint8_t swz[4]; int has_swz = rg->t.set_swz || (vv && vv->set_swizzle);
                    if (has_swz) { uint16_t sz = vv && vv->set_swizzle ? (uint16_t)(vv->swz[0] | vv->swz[1] << 3 | vv->swz[2] << 6 | vv->swz[3] << 9) : rg->t.swz; for (int q = 0; q < 4; q++) swz[q] = (sz >> (3 * q)) & 7; }
                    g_view_fmt = view_format(v, vv);
                    iv = view_get(v, vt, vv && vv->set_levels ? vv->base_level : 0, vv && vv->set_levels ? vv->nlevels : 0, vv && vv->set_layers ? vv->min_layer : 0,
                                  vv && vv->set_layers ? vv->nlayers : 0, tex_aspect(v, rg, vv, s), has_swz ? swz : NULL);
                    g_view_fmt = VK_FORMAT_UNDEFINED;
                    if (s->upsq && !g_upsq_src) { g_upsq_src = v; g_upsq_src_iv = iv; } }   // a listed upscaler's input (gfx_vk_upsq.inc)
            }
            if (!iv) iv = ms ? dummy_ms((t->type & 0x400) != 0) : dummy_view(vt);
            if (!iv) { g_bind_bad = 1; RED_ONCE(113, "no stand-in for an unbound multisample slot on this GPU: skipped"); iv = dummy_view(vt); }
            const samp_par *sp = sid > 0 && sid < g_nsreg && g_sreg_ok[sid] ? &g_sreg[sid] : NULL;
            if (dump_on() && e < 4) { const vtex *dv = tid > 0 && tid < g_ntreg && g_treg[tid].ok ? nvn_hm_get(&g_tex, &g_treg[tid].t.serial) : NULL; if (dv) dump_note_tex(dv->serial);
                LOG("[dump]   %s binding %d[%d] type %#x tid %d sid %d -> %s s%llu %ux%ux%u layers %u levels %u vk%d pkg %d image %#llx", s->key, t->binding, e, t->type, tid, sid, dv ? "tex" : "none",
                    dv ? (unsigned long long)dv->serial : 0ull, dv ? dv->w : 0, dv ? dv->h : 0, dv ? dv->d : 0, dv ? dv->layers : 0, dv ? dv->levels : 0, dv ? dv->fmt : 0, dv ? dv->pkg : 0, dv ? (unsigned long long)(uint64_t)dv->img : 0ull); }
            if (bcs) { const vtex *bv = tid > 0 && tid < g_ntreg && g_treg[tid].ok ? nvn_hm_get(&g_tex, &g_treg[tid].t.serial) : NULL;
                bc_addf(" | texture b%d[%d] type %#x tid %d sid %d: %s s%llu %ux%ux%u, %u layers, %u levels, vk format %d%s", t->binding, e, t->type, tid, sid, bv ? "" : rg ? "registered, no image," : "unregistered (a stand-in),",
                        bv ? (unsigned long long)bv->serial : 0ull, bv ? bv->w : 0, bv ? bv->h : 0, bv ? bv->d : 0, bv ? bv->layers : 0, bv ? bv->levels : 0, bv ? (int)bv->fmt : 0, sp ? "" : ", no sampler"); }
            int shd = (t->type & 0x400) != 0; VkSampler smp;
            if (tex_once() && sp && g_scache) { VkSampler *sc = &g_scache[sid][shd]; if (*sc) g_sc_hit++; else { *sc = samp_get(sp, shd); g_sc_made++; } smp = *sc; }   // per ID (g_scache)
            else if (tex_once() && !sp) { static VkSampler none[2]; if (!none[shd]) none[shd] = samp_get(NULL, shd); smp = none[shd]; }
            else smp = samp_get(sp, shd);
            ii[nii + e] = (VkDescriptorImageInfo){smp, iv, VK_IMAGE_LAYOUT_GENERAL};
        }
        wr[nw++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, (uint32_t)t->binding, 0, (uint32_t)cnt, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii[nii], NULL, NULL};
        nii += cnt;
    }
    for (int i = 0; i < s->nim; i++) {   // images (load / store / atomics): the handle names a texture pool entry, as a texture's does (hle_nvn.c BindImages)
        const rb_tex *t = &s->im[i]; int cnt = t->array > 1 ? (t->array > TEX_ARR_MAX ? TEX_ARR_MAX : t->array) : 1, tbuf = (t->type & 0xff) == 2;
        VkImageViewType vt = view_type_of(t->type); uint32_t w0 = (uint32_t)t->handle & 0x3fff;
        for (int e = 0; e < cnt; e++) {
            uint32_t hw = bank_word(nst, c2, t->cbuf_slot, (w0 + 2u * e) * 4); int tid = (int)(hw & 0xfffff); if (tid && t->cbuf_slot >= 3) tid += D.tex_base;
            treg_t *rg = tid > 0 && tid < g_ntreg && g_treg[tid].ok ? &g_treg[tid] : NULL;
            if (tbuf) { VkBufferView v = rg ? tbuf_view(&rg->t, 1, disp_like(s)) : VK_NULL_HANDLE; bvs[nbv + e] = v ? v : dummy_tbuf(); continue; }
            const tc_ent *ce = tex_once() ? tc_el(tc, ci++, tid) : NULL; const treg_t *ri = ce ? (ce->r.ok ? &ce->r : NULL) : rg;   // as the textures above
            VkImageView iv = VK_NULL_HANDLE; vtex *v = !ri ? NULL : ce ? ce->v : vt_get(&ri->t);
            if (v && !(v->usage & VK_IMAGE_USAGE_STORAGE_BIT) && !img_up(v, s->stage == SH_CS)) { g_bind_bad = 1; v = NULL; }   // made without storage use (vt_storage_up)
            if (v) { v->gpu_w = 1; v->last_w = atomic_load(&g_presents); }   // an image slot: the shader may store to it (gfx_vk_evict.inc)
            if (v && s->upsq && !g_upsq_dst) g_upsq_dst = v;   // a listed upscaler's output (gfx_vk_upsq.inc)
            if (v) { iv = storage_view(v, ri->has_v ? &ri->v : NULL, vt, t->fmt); if (!iv) g_bind_bad = 1; }
            if (iv && v->fmt == VK_FORMAT_E5B9G9R9_UFLOAT_PACK32 && s->stage == SH_CS) { if (t->binding < 64) g_e5_need |= 1ull << t->binding; else g_bind_bad = 1; }   // its pipeline packs RGB9E5 there (cpipe_e5)
            if (!iv) iv = dummy_storage(vt, t->fmt);
            if (bcs) bc_addf(" | image b%d[%d] type %#x tid %d: %s s%llu %ux%ux%u, %u layers, vk format %d, usage %#x", t->binding, e, t->type, tid, v ? "" : ri ? "registered, no image (a stand-in)," : "unregistered (a stand-in),",
                             v ? (unsigned long long)v->serial : 0ull, v ? v->w : 0, v ? v->h : 0, v ? v->d : 0, v ? v->layers : 0, v ? (int)v->fmt : 0, v ? (unsigned)v->usage : 0u);
            if (!iv) g_bind_bad = 1;
            ii[nii + e] = (VkDescriptorImageInfo){VK_NULL_HANDLE, iv, VK_IMAGE_LAYOUT_GENERAL};
        }
        if (tbuf) { wr[nw++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, (uint32_t)t->binding, 0, (uint32_t)cnt, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, NULL, NULL, &bvs[nbv]}; nbv += cnt; }
        else { wr[nw++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, (uint32_t)t->binding, 0, (uint32_t)cnt, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ii[nii], NULL, NULL}; nii += cnt; }
    }
    pthread_mutex_unlock(&g_reg_mu);
    // a vt_get above synced and closed the draw's chunk (ds_moved): the sets of its earlier stages came from that chunk, whose completion may have handed them out again or
    // reset its pools (past g_ds_cap: the sets freed), so nothing is taken or written; the caller takes every stage's again from the chunk recorded now. Nothing past here
    // closes a chunk (ds_get only takes cur(), the chunk dseq names)
    if (ds_moved(dseq)) { if (own) { free(ii); free(bvs); } return 0; }
    if (g_ds_stats) {   // every descriptor of the layout written (binding 0 only where it is used): a set handed out again (ds_get) must keep none of an earlier draw's
        uint32_t nd = 0; for (uint32_t i = 0; i < nw; i++) nd += wr[i].descriptorCount;
        g_ds_checked++; if (nd + (s->b0 || !support_skip() ? 0u : 1u) != s->ndesc && g_ds_short++ < 16) LOG("[RED] [vk] shader %s: its descriptor set written with %u of its layout's %u descriptors", s->key, nd, s->ndesc); }
    // the chunk dseq names, still recorded: with DM_VK_SET_RECYCLE=0 ds_moved is always 0, and a sync inside stage_set may have closed it (C NULL, or another chunk)
    int hit = 0; sr_ent *e = !tf && set_reuse() && C && C->seq == dseq ? sr_find(s->dsl, wr, nw, &hit) : NULL;
    if (hit) { *pset = e->set; g_sr_hit++; if (own) { free(ii); free(bvs); } return 1; }
    if (!(set = ds_get(s, tf))) { if (own) { free(ii); free(bvs); } return 0; }
    for (uint32_t i = 0; i < nw; i++) wr[i].dstSet = set;
    vkUpdateDescriptorSets(g_vk.dev, nw, wr, 0, NULL);
    if (own) { free(ii); free(bvs); }
    if (e) { sr_put(e, s->dsl, set); g_sr_miss++; }
    *pset = set;
    return 1;
}

// Data a compute dispatch of the current chunk wrote to a storage buffer, read by a later draw as a vertex stream (Super Mario Bros. Wonder: a few hundred bytes of
// vertices made by a compute shader, about once a frame): the draw binds the dispatch's own arena copy instead of waiting for the chunk to complete and copying it
// back (that sync emptied the GPU queue every frame). The dispatch ends with a full memory barrier (vk_dispatch cb_barrier). Only when the write-back alone covers the
// stream's bytes, no queued copy writes them, and every attribute of the stream is read as it is (a converted one needs the bytes on the CPU)
// chunk c's newest write to [lo, hi): 1 when one decides (*res: whether it can be read on the GPU, *b / *o where), 0 when the chunk does not write the range
static int fwd_hit(chunk *c, int nwb, uintptr_t lo, uintptr_t hi, VkBuffer *b, VkDeviceSize *o, int *res) {
    for (int k = 0; k < c->ncop; k++) if (c->cop[k].dst < hi && lo < c->cop[k].dst + c->cop[k].n) { *res = 0; return 1; }
    for (int i = nwb - 1; i >= 0; i--) { const wb_t *w = &c->wb[i]; if (!wb_hits(w, lo, hi)) continue;
        int ipr = !w->fwd && w->inpl && g_rd_cs && ip_read_on();   // a draw's in-place write, read by a dispatch
        *res = (w->fwd || ipr) && (w->st < 0 || w->inpl) && w->rows == 1 && lo >= (uintptr_t)w->dst && hi <= (uintptr_t)w->dst + w->row_bytes;   // (in place: read where it was written)
        if (*res) { *b = w->inpl ? w->ipb : c->ar[-1 - w->st].b; *o = (VkDeviceSize)w->off + (VkDeviceSize)(lo - (uintptr_t)w->dst); if (ipr) g_ip_read++; }
        else if (w->vd && w->rows == 1) {   // a discarded draw's write that the read is not inside: the two as one home range (rd_home.h)
            uintptr_t wl = (uintptr_t)w->dst, wh = wl + w->row_bytes, hl = lo < wl ? lo : wl, hh = hi > wh ? hi : wh;
            if (rd_home_learn(&g_rd_homes, hl, hh, RD_HOME_MAX) && g_rd_learned++ < 8)
                LOG("[vk] rasterizer discard: home range %#lx+%#lx learned (a read of %#lx+%#lx met a write of %#lx+%#lx)", (unsigned long)hl, (unsigned long)(hh - hl), (unsigned long)lo, (unsigned long)(hi - lo), (unsigned long)wl, (unsigned long)(wh - wl)); }
        return 1; }
    return 0;
}
// the current chunk first (its write-backs from before this draw), then the chunks in flight, newest first. A chunk in flight whose arena is read is held: it is
// not recorded again until the reading chunk completes (chunk_thread), so its memory stays as the dispatch left it
static int fwd_find(uintptr_t lo, uintptr_t hi, VkBuffer *b, VkDeviceSize *o) {
    if (!C || hi <= lo) return 0;
    int n = g_fwd_nwb >= 0 && g_fwd_nwb <= C->nwb ? g_fwd_nwb : C->nwb, r = 0;
    if (fwd_hit(C, n, lo, hi, b, o, &r)) return r;
    if (!atomic_load(&g_inflight_wb) && !atomic_load(&g_inflight_cop)) return 0;
    pthread_mutex_lock(&g_cmu);
    for (int f = g_fifo_n - 1; f >= 0; f--) { chunk *w = g_fifo[f]; int nw = w->nwb;
        if (fwd_hit(w, nw, lo, hi, b, o, &r)) { if (r && w->hold < C->seq) w->hold = C->seq; break; } }
    pthread_mutex_unlock(&g_cmu);
    return r;
}
static int stream_direct(const vshader *vs, int s) {   // every attribute of stream s bound as it is (vfmt_direct)
    for (int i = 0; i < 16; i++) { va_st a; memcpy(&a, &D.attr[i], 4); if (a.fmt && a.stream == s && !vfmt_direct(a.fmt, vs ? vs->vin[i] : 'f')) return 0; }
    return 1;
}
// Forwarding finds a stream only in a forwardable write-back (fwd: stage_set gives it to a compute shader's storage buffer, nothing else does): with none in the chunk being
// recorded (C->nfwd) and no chunk in flight holding one (g_inflight_fwd: counted at submission, taken off once its write-backs landed, in the g_cmu section that takes it off
// g_fifo), fwd_find cannot answer 1, and stream_fwd answers 0 before stream_direct's 16-attribute vfmt_direct loop and fwd_find's scans. draw_view asks once per stream,
// not once per attribute. Breath of the Wild on the Odin 2: stream_fwd and vfmt_direct about 1.5% of the replay thread (about 15,500 stream_direct calls a frame for 3,750
// draws), with about 16 forwarded streams a frame from its GPU particle dispatches. DM_VK_FWD_EARLY=0 (env.txt): every stream looked up, once per attribute, as before
static int fwd_early(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_FWD_EARLY"); v = !(e && *e == '0'); if (!v) LOG("[vk] DM_VK_FWD_EARLY=0: every vertex stream is looked up among the forwardable write-backs"); } return v; }
static uint64_t g_fwd_early, g_fwd_look;   // streams answered without a lookup (none pending) / looked up
// The bytes of stream s the draw being made fetches: from its first vertex to the end of its last one's attributes, when the vertices are known (a draw that is not
// indexed: g_vr0, g_vr1, set by vk_draw_), else the whole bound range. Games bind a stream's whole buffer: Xenoblade Chronicles 2 binds each attribute of a buffer as a
// stream of its own, 32 bytes on per attribute and as long as the buffer, so the bound ranges run past the buffer, over what a write-back covers and into the next buffer
static int64_t g_vr0, g_vr1;
static void stream_rng(int s, uintptr_t *lo, uintptr_t *hi) {
    const NVNbufferRange *r = &D.vbuf[s]; uint32_t st = D.stream[s].stride; *lo = (uintptr_t)r->address; *hi = (uintptr_t)(r->address + r->size);
    if (g_vr1 <= g_vr0 || g_vr0 < 0 || !st || D.stream[s].divisor) return;
    uint64_t end = 0;
    for (int i = 0; i < 16; i++) { va_st a; memcpy(&a, &D.attr[i], 4); if (!a.fmt || a.stream != s) continue;
        int kind, bits, cn = vfmt_desc(a.fmt, &kind, &bits); uint64_t e = (uint64_t)a.offset + (cn ? (kind >= 7 ? 4u : (uint64_t)cn * (uint64_t)bits / 8) : 16u); if (e > end) end = e; }
    uint64_t l = (uint64_t)g_vr0 * st, h = (uint64_t)(g_vr1 - 1) * st + end;
    if (!end || l >= r->size) return;
    if (h > r->size) h = r->size;
    *lo = (uintptr_t)(r->address + l); *hi = (uintptr_t)(r->address + h);
}
// An indexed draw's stream that starts in a forwardable write-back and runs past its end is looked up only as far as that end (stream_clip.h says when), so its overrun
// neither misses the write-back nor teaches a home range two buffers wide. Only the newest write-back holding the stream's first byte counts. DM_VK_STREAM_CLIP=0 (env.txt):
// such streams are looked up as bound, as before
static int stream_clip_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_STREAM_CLIP"); v = !(e && *e == '0'); if (!v) LOG("[vk] DM_VK_STREAM_CLIP=0: an indexed draw's stream is looked up as bound, past a write-back's end"); } return v; }
static uint64_t g_stream_clip;   // streams looked up as far as the end of the write-back they start in
static uintptr_t wb_clip(const chunk *c, int nwb, uintptr_t lo, uintptr_t hi, int *found) {   // (stream_clip_end) in c's write-backs; *found: one of them holds lo
    for (int i = nwb - 1; i >= 0; i--) { const wb_t *w = &c->wb[i]; if (!wb_hits(w, lo, lo + 1)) continue;
        *found = 1; return stream_clip_end(lo, hi, (uintptr_t)w->dst, w->row_bytes, w->fwd && w->rows == 1); }
    return 0;
}
static void stream_clip(uintptr_t lo, uintptr_t *hi) {   // the current chunk's write-backs from before the draw, then the chunks in flight, newest first (as fwd_find)
    if (!C || !stream_clip_on()) return;
    int n = g_fwd_nwb >= 0 && g_fwd_nwb <= C->nwb ? g_fwd_nwb : C->nwb, found = 0; uintptr_t e = wb_clip(C, n, lo, *hi, &found);
    if (!found && atomic_load(&g_inflight_wb)) { pthread_mutex_lock(&g_cmu);
        for (int f = g_fifo_n - 1; f >= 0 && !found; f--) e = wb_clip(g_fifo[f], g_fifo[f]->nwb, lo, *hi, &found);
        pthread_mutex_unlock(&g_cmu); }
    if (e) { *hi = e; g_stream_clip++; }
}
static int stream_fwd(const vshader *vs, int s, VkBuffer *b, VkDeviceSize *o) {   // *o: where the stream's address is in the copy (bind_vbufs adds its first vertex's offset)
    if (vk_safe()) return 0;
    const NVNbufferRange *r = &D.vbuf[s]; if (!r->address || !r->size) return 0;
    if (fwd_early() && (!C || (!C->nfwd && !atomic_load(&g_inflight_fwd)))) { g_fwd_early++; return 0; }
    if (!stream_direct(vs, s)) return 0;
    g_fwd_look++; uintptr_t lo, hi; stream_rng(s, &lo, &hi);
    if (g_vr1 <= g_vr0 || g_vr0 < 0) stream_clip(lo, &hi);   // indexed: the bound range (before fwd_find, which would learn its overrun as a home range)
    if (!fwd_find(lo, hi, b, o)) return 0;
    VkDeviceSize back = (VkDeviceSize)(lo - (uintptr_t)r->address); if (*o < back) return 0;   // (a copy that starts past the stream's address: its binding cannot start there)
    *o -= back; return 1;
}
// ---- Binds the command buffer already holds ----
// Every draw recorded its pipeline, the seven dynamic states (ten calls), the index buffer and each vertex binding, changed or not, and the Adreno driver marks state
// dirty for each call (only vkCmdBindPipeline compares with what is bound): on Breath of the Wild on the Odin 2 those calls cost about 1.3 ms a frame (pipeline 0.41,
// vertex buffers 0.41, index buffer 0.1, dynamic state about 0.2), and their dirty bits re-emit state in vkCmdDrawIndexed (the viewport's block about 0.18 ms), while 87% of a
// pass's draws keep the draw before's pipeline and 95% its viewport and scissor. A shadow of what the command buffer being recorded holds lets a draw record only what
// differs (values compared bitwise). It is emptied wherever state is set behind it or may be lost: a command buffer's begin (cb), a render pass's begin (rp_start, and
// tf_capture's own pass: Vulkan keeps the state across passes, the shadow does not rely on it), tf_capture (its own pipeline, viewport and scissor), and the clears drawn in
// a pass (clear_color_pass, clear_ds_pass: their pipeline makes only viewport, scissor and stencil reference dynamic, so binding it leaves blend constants, stencil masks and
// depth bias undefined for the next draw). Compute binds have a bind point of their own. Descriptor sets are bound for every draw still (each draw allocates its own).
// Vertex bindings repeat only when the draw before bound the same buffer at the same offset (bindings start at each draw's first vertex: bind_vbufs).
// DM_VK_BIND_FILTER=0 (env.txt): every draw records every bind, as before
static int bind_filter(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_BIND_FILTER"); v = !(e && *e == '0'); if (!v) LOG("[vk] DM_VK_BIND_FILTER=0: every draw records every bind"); } return v; }
enum { BF_PIPE = 1, BF_VP = 2, BF_SC = 4, BF_BC = 8, BF_CM0 = 16, BF_CM1 = 32, BF_WM0 = 64, BF_WM1 = 128, BF_REF0 = 256, BF_REF1 = 512, BF_DB = 1024, BF_IB = 2048 };
static struct { VkCommandBuffer cb; uint32_t ok, vbok; VkPipeline pipe; VkViewport vp; VkRect2D sc; float bc[4], db[3]; uint32_t cm[2], wm[2], ref[2];
                VkBuffer ib; VkDeviceSize io; VkIndexType it; VkBuffer vb[32]; VkDeviceSize vo[32]; } g_bf;   // ok / vbok: what is known (BF_*; vertex binding i)
static uint64_t g_bf_skip[4], g_bf_made[4];   // binds left out / recorded: 0 pipeline, 1 dynamic state, 2 vertex buffers, 3 index buffer
static void bf_reset(void) { g_bf.ok = g_bf.vbok = 0; }
static int bf_cb(VkCommandBuffer c) { if (!bind_filter()) return 0; if (g_bf.cb != c) { g_bf.cb = c; bf_reset(); } return 1; }   // the filter on, the shadow c's
static void bf_pipeline(VkCommandBuffer c, VkPipeline p) {
    if (bf_cb(c) && (g_bf.ok & BF_PIPE) && g_bf.pipe == p) { g_bf_skip[0]++; return; }
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, p); g_bf.pipe = p; g_bf.ok |= BF_PIPE; g_bf_made[0]++;
}
static int bf_same(VkCommandBuffer c, uint32_t bit, void *sh, const void *v, size_t n) {   // 1: dynamic state bit already holds v (the call is left out); else it is noted
    if (!bf_cb(c)) return 0;
    if ((g_bf.ok & bit) && !memcmp(sh, v, n)) { g_bf_skip[1]++; return 1; }
    memcpy(sh, v, n); g_bf.ok |= bit; g_bf_made[1]++; return 0;
}
static void bf_ibuf(VkCommandBuffer c, VkBuffer b, VkDeviceSize o, VkIndexType t) {
    if (bf_cb(c) && (g_bf.ok & BF_IB) && g_bf.ib == b && g_bf.io == o && g_bf.it == t) { g_bf_skip[3]++; return; }
    vkCmdBindIndexBuffer(c, b, o, t); g_bf.ib = b; g_bf.io = o; g_bf.it = t; g_bf.ok |= BF_IB; g_bf_made[3]++;
}
static void bf_vbuf(VkCommandBuffer c, uint32_t i, VkBuffer b, VkDeviceSize o) {   // i < 32: streams 0..15, converted attributes 16 + attribute
    if (bf_cb(c) && (g_bf.vbok >> i & 1) && g_bf.vb[i] == b && g_bf.vo[i] == o) { g_bf_skip[2]++; return; }
    vkCmdBindVertexBuffers(c, i, 1, &b, &o); g_bf.vb[i] = b; g_bf.vo[i] = o; g_bf.vbok |= 1u << i; g_bf_made[2]++;
}
// ---------------- The draw ----------------
// A draw whose vertex shader writes the layer, into a layered pass of more layers than a vertex shader reaches (g_vs_layers: Arm's Mali-G715 driver drops a primitive
// whose layer from a vertex shader is 8 or more, whatever the framebuffer's layer count; a geometry shader's reaches every layer). Pokemon Legends: Z-A builds its
// 32x32x32 colour-grading LUT with one such draw (b29f08619f379a83, an instance per slice) and tone-maps the frame through it: on a Pixel 9 Pro XL only slices 1-7
// were drawn, and the world came out nearly black. The draw is made first as it is (layers 0-7 drawn), then again for each later group of layers in a pass of its
// own whose views start at the group's first layer, the shader's layer less that base (spv_layer_base), so a primitive lands in its group alone (the GPU drops the
// layers below 0 as well as from 8 on). The pass's bindings stay bound across its passes (one command buffer); the next draw begins its own pass from layer 0
static uint64_t g_lg_draws;   // draws made again for a later group of layers
static void vs_layer_groups(VkCommandBuffer c, const pipe_t *pp, uint32_t count, uint32_t inst, int32_t first, uint32_t first_inst, int indexed) {
    uint32_t L = P.layers, n = (uint32_t)g_vs_layers;
    if (!g_lg_draws) LOG("[vk] a draw whose vertex shader writes the layer into %u layers: made again for each later %u (vs_layer_groups)", L, n);
    for (uint32_t b = n; b < L; b += n) {
        rp_end(); g_lbase = b;
        int ok = rp_prepare_(1) && P.layers > 0 && rp_start() && C->cb == c;
        if (!ok) { RED_ONCE(196, "a draw made again for layers from %u: its pass could not begin in the same command buffer, the layers from there are not drawn", b); break; }
        int32_t base = (int32_t)b; vkCmdPushConstants(c, pp->pl, pp->lbpc, SPV_LBASE_PC, 4, &base);   // the vertex shader's layer less it, a fragment shader's plus it
        if (indexed) vkCmdDrawIndexed(c, count, inst, 0, first, first_inst); else vkCmdDraw(c, count, inst, (uint32_t)first, first_inst);
        g_lg_draws++;
    }
    rp_end(); g_lbase = 0;   // the next draw's pass from layer 0, as before
}
// vertex data: each stream's bytes from vertex lo to the last vertex (or instance) the draw reads; the bindings start at vertex lo, so the draw numbers its vertices
// from lo (firstVertex / vertexOffset less lo). Copying from vertex 0 instead made a draw late in a large vertex buffer copy (and convert) all of it before its
// vertices: Hades II draws its sprites from one growing buffer, and its render thread spent 61% of its time here (bind_vbufs and memcpy) at about 3 fps
// The instances a draw's instance-rate streams are moved by, in place of its first instance: its base instance where the divisor needs a draw with a stream stepping per
// N > 1 instances to start at instance 0 (g_div_first0, the KHR divisor without supportsNonZeroFirstInstance), else 0. The streams then start at the base instance's
// element and the draw at instance 0. Translated shaders read their instance as InstanceIndex - BaseInstance, which does not move, and NVN's base instance is in c0
static uint32_t inst_shift(uint32_t base_inst) {
    if (!g_div_first0 || !g_divisor || !base_inst) return 0;
    for (int i = 0; i < 16; i++) { va_st a; memcpy(&a, &D.attr[i], 4); if (!a.fmt || D.stream[a.stream].divisor <= 1) continue;
        static int once; if (!once) { once = 1; LOG("[vk] first draw with a stream stepping per %u instances at base instance %u: drawn from instance 0, the base instance in its instance streams' offsets", D.stream[a.stream].divisor, base_inst); }
        return base_inst; }
    return 0;
}
static int bind_vbufs(VkCommandBuffer c, const vshader *vs, int64_t lo, int64_t vhi, uint32_t base_inst, uint32_t inst) {
    if (lo < 0) lo = 0;
    uint32_t ish = inst_shift(base_inst);
    uint32_t used = 0; for (int i = 0; i < 16; i++) { va_st a; memcpy(&a, &D.attr[i], 4); if (a.fmt) used |= 1u << a.stream; }
    if (dump_on()) for (int i = 0; i < 16; i++) { va_st a; memcpy(&a, &D.attr[i], 4); if (!a.fmt) continue;
        const NVNbufferRange *r = &D.vbuf[a.stream]; uint32_t st = D.stream[a.stream].stride; char hx[200] = "";
        for (int v = 0; v < 4; v++) { size_t l = strlen(hx); uint64_t at = (uint64_t)v * st + a.offset; float f[2] = {0, 0};
            if (r->address && at + 8 <= r->size) memcpy(f, (const void *)(uintptr_t)(r->address + at), 8); snprintf(hx + l, sizeof hx - l, " (%g %g)", f[0], f[1]); }
        LOG("[dump]   attr %d fmt %#x stream %d offset %d stride %u divisor %u vin %c direct %d size %llu as f32:%s", i, a.fmt, a.stream, a.offset, st, D.stream[a.stream].divisor,
            vs ? vs->vin[i] : '?', vfmt_direct(a.fmt, vs ? vs->vin[i] : 'f'), (unsigned long long)r->size, hx); }
    for (int s = 0; s < 16; s++) if (used >> s & 1) {
        const NVNbufferRange *r = &D.vbuf[s]; uint32_t stride = D.stream[s].stride; uint64_t need = r->size;
        if (stride) { uint32_t dv = D.stream[s].divisor; int64_t last = dv ? (int64_t)base_inst + (dv > 1 && g_divisor ? (inst + dv - 1) / dv : inst) : vhi; uint64_t want = (uint64_t)(last > 0 ? last : 0) * stride + 64; if (want < need) need = want; }
        uint64_t from = (uint64_t)(D.stream[s].divisor ? ish : lo) * stride; if (from > need) from = need;   // instanced streams count from base_inst (from ish when the draw starts there: inst_shift), not lo
        VkBuffer vb; VkDeviceSize vo;
        if (stream_fwd(vs, s, &vb, &vo)) { g_fwd++; vo += from; bf_vbuf(c, (uint32_t)s, vb, vo); continue; }   // on the GPU: fwd_find
        uint8_t *fill = NULL; gc_ent *ge = r->address ? gc_get((uintptr_t)r->address, r->size, 0, r->size, &fill) : NULL;   // the whole buffer kept, bound at the draw's first vertex
        if (ge) {
            if (fill) memcpy(fill, (const void *)(uintptr_t)r->address, r->size);
            vb = gc_buf(ge); vo = ge->off + from; bf_vbuf(c, (uint32_t)s, vb, vo); continue;
        }
        g_gc_copy++;
        uint64_t n = need - from; uint8_t *d = ar_alloc(n ? n : 16, &vb, &vo); if (!d) return 0;
        if (r->address && n) memcpy(d, (const void *)(uintptr_t)(r->address + from), n); else memset(d, 0, 16);
        bf_vbuf(c, (uint32_t)s, vb, vo);
    }
    for (int i = 0; i < 16; i++) {   // attributes as register bits (vfmt_desc): each vertex (or instance) of the range, from its stream
        va_st a; memcpy(&a, &D.attr[i], 4); int kind, bits, cn = a.fmt ? vfmt_desc(a.fmt, &kind, &bits) : 0;
        if (!cn || vfmt_direct(a.fmt, vs ? vs->vin[i] : 'f')) continue;
        const NVNbufferRange *r = &D.vbuf[a.stream]; uint32_t stride = D.stream[a.stream].stride, dv = D.stream[a.stream].divisor;
        int64_t last = dv ? (int64_t)base_inst + (dv > 1 && g_divisor ? (inst + dv - 1) / dv : inst) : vhi; if (last < 1) last = 1;
        if (!stride) last = 1;
        int bytes = kind >= 7 ? 4 : cn * bits / 8;
        int64_t v0 = !stride ? 0 : dv ? ((int64_t)ish < last ? (int64_t)ish : last - 1) : lo < last ? lo : last - 1;   // the first vertex (or instance: inst_shift) converted: the binding's element 0
        VkBuffer vb; VkDeviceSize vo;
        // the whole buffer converted once and kept (gc_get): every element the buffer holds, bound at v0
        uint64_t nall = !r->address ? 0 : !stride ? 1 : r->size >= (uint64_t)a.offset + (uint64_t)bytes ? (r->size - a.offset - bytes) / stride + 1 : 0;
        if (nall && (uint64_t)last <= nall) {
            uint8_t *fill = NULL; gc_ent *ge = gc_get((uintptr_t)r->address, r->size, 1ull << 63 | (uint64_t)a.fmt | (uint64_t)a.offset << 16 | (uint64_t)stride << 32, nall * 16, &fill);
            if (ge) {
                if (fill) for (uint64_t v = 0; v < nall; v++) vfmt_regs(kind, bits, cn, (const uint8_t *)(uintptr_t)(r->address + v * stride + a.offset), (uint32_t *)fill + v * 4);
                vb = gc_buf(ge); vo = ge->off + (VkDeviceSize)v0 * 16; bf_vbuf(c, 16u + (uint32_t)i, vb, vo); continue;
            }
        }
        uint32_t *d = (uint32_t *)ar_alloc((VkDeviceSize)(last - v0) * 16, &vb, &vo); if (!d) return 0;
        for (int64_t v = v0; v < last; v++) {
            uint64_t at = (uint64_t)v * stride + a.offset; uint32_t *o = d + (v - v0) * 4;
            if (r->address && at + (uint64_t)bytes <= r->size) vfmt_regs(kind, bits, cn, (const uint8_t *)(uintptr_t)(r->address + at), o);
            else memset(o, 0, 16);
        }
        bf_vbuf(c, 16u + (uint32_t)i, vb, vo);
    }
    return 1;
}
static uint64_t g_captures;
static VkRenderPass g_null_rp; static VkFramebuffer g_null_fb, g_grid_fb;   // g_grid_fb: the fragment-stage capture's (spv_tffrag.h), 1024 × 1024 without attachments
// The pass without attachments (made once) and its 1 × 1 framebuffer: a vertex-stage capture, or a discarded draw that writes (rd_open), rasterizes nothing; the
// fragment-stage capture runs its fragment shader in it for its side effects alone, on g_grid_fb
static VkRenderPass null_pass(void) {
    if (!g_null_rp) { VkSubpassDescription sp = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS};
        VkRenderPassCreateInfo rc = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .subpassCount = 1, .pSubpasses = &sp};
        if (vkCreateRenderPass(g_vk.dev, &rc, NULL, &g_null_rp) != VK_SUCCESS) { g_null_rp = VK_NULL_HANDLE; return VK_NULL_HANDLE; } }
    if (!g_null_fb) { VkFramebufferCreateInfo fc = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = g_null_rp, .width = 1, .height = 1, .layers = 1};
        if (vkCreateFramebuffer(g_vk.dev, &fc, NULL, &g_null_fb) != VK_SUCCESS) { g_null_fb = VK_NULL_HANDLE; return VK_NULL_HANDLE; } }
    return g_null_rp;
}
static int tf_capture(vshader *vs, const prog_t *pr, int pst, const p_draw *dw, const p_draw_idx *di) {
    if (!D.tf_on || !vs->modtf) { if (D.tf_on) RED_ONCE(114, "transform feedback with vertex shader %s, which has no capture variant: nothing captured", vs->key); return 1; }
    int frag = vs->modtff != NULL;   // captured in the fragment stage (tf_frag)
    if (!frag && !g_vk_feat.vertexPipelineStoresAndAtomics) { RED_ONCE(115, "transform feedback: the GPU has no vertex-stage storage writes"); return 0; }
    if (!dw || dw->instances > 1 || dw->prim == 6 || dw->prim == 7) { RED_ONCE(116, "transform feedback draw that is indexed, instanced or a fan / quad list (vertex %s): not captured", vs->key); return 0; }
    if (dw->count <= 0) return 1;
    if (frag && (uint32_t)dw->count > SPV_TFF_MAX_VERTS) { RED_ONCE(180, "transform feedback draw of %d vertices (vertex %s): the fragment-stage capture takes %u at most, not captured", dw->count, vs->key, SPV_TFF_MAX_VERTS); return 0; }
    for (int b = 0; b < 4; b++) if (vs->tf_stride[b]) {
        uint64_t n = (uint64_t)dw->count * vs->tf_stride[b];
        if (!D.tf[b].addr || D.tf_off[b] + n > D.tf[b].size || !mtl_addr(D.tf[b].addr + D.tf_off[b], n, NULL, NULL)) {
            RED_ONCE(117, "transform feedback buffer %d of vertex %s is unbound, too small or in no memory pool: not captured", b, vs->key); return 0; }
    }
    if (!null_pass()) return 0;
    VkFramebuffer *fbp = frag ? &g_grid_fb : &g_null_fb; uint32_t side = frag ? 1u << SPV_TFF_SHIFT : 1;
    if (!*fbp) { VkFramebufferCreateInfo fc = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = g_null_rp, .width = side, .height = side, .layers = 1};
                 if (vkCreateFramebuffer(g_vk.dev, &fc, NULL, fbp) != VK_SUCCESS) { *fbp = VK_NULL_HANDLE; return 0; } }
    // the fragment-stage capture's points cover the grid's first rows: vertex v at pixel (v & 1023, v >> 10)
    VkRect2D area = {{0, 0}, {side, frag ? ((uint32_t)dw->count + side - 1) >> SPV_TFF_SHIFT : 1}};
    stage_textures(vs);
    if (vs->pool) pool_sync();
    pipe_key k; memset(&k, 0, sizeof k); k.sh[0] = vs; k.tf = 1; k.rp = g_null_rp; k.topo = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; k.poly = D.poly; k.poly.cull = 0; k.color = D.color; k.color.logic_op = NVN_LOGIC_OP_COPY;   // no colour target: blending and logic ops do not apply
    if (frag) k.poly.offset_en = 0;   // rasterized, its depth bias would want the dynamic state set, and there is no depth to bias
    memcpy(k.attr, D.attr, sizeof k.attr); memcpy(k.stream, D.stream, sizeof k.stream);
    pipe_t *pp = pipe_get(&k); if (!pp) return 0;
    g_drawing = 1;
    VkDescriptorSet set; uint32_t dyn[18];
    for (int t = 0;; t++) {   // t 1: the set taken again from the chunk recorded now when a sync inside stage_set closed the one it came from (ds_moved), before any feedback buffer is queued
        g_bind_bad = 0; if (!cur()) return 0; uint64_t dseq = C->seq;
        if (stage_set(vs, 1, &set, dyn, pr, pst, (uint32_t)dw->base_instance, dseq)) break;
        if (!ds_moved(dseq) || ds_again(t, 1)) return 0;
    }
    if (g_bind_bad) return 0;
    VkDescriptorBufferInfo bi[4]; VkWriteDescriptorSet wr[4]; uint32_t nw = 0;   // from here to the bind nothing closes a chunk (ar_alloc, wb_add and cb take cur())
    for (int b = 0; b < 4; b++) if (vs->tf_stride[b]) {   // vertex i of the draw writes at i × stride from the write offset; copied back whole
        uint64_t n = (uint64_t)dw->count * vs->tf_stride[b]; VkBuffer buf; VkDeviceSize off; uint8_t *d = ar_alloc(n, &buf, &off); if (!d) return 0;
        uint8_t *dst = (uint8_t *)(uintptr_t)(D.tf[b].addr + D.tf_off[b]); memcpy(d, dst, n);
        wb_t *w = wb_add(); if (w) *w = (wb_t){-1 - C->ar_last, (size_t)off, (size_t)n, 1, (size_t)n, (size_t)n, dst, NULL};
        bi[nw] = (VkDescriptorBufferInfo){buf, off, n}; wr[nw] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, 100u + (uint32_t)b, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bi[nw], NULL}; nw++;
        D.tf_off[b] += n;
    }
    vkUpdateDescriptorSets(g_vk.dev, nw, wr, 0, NULL);
    VkCommandBuffer c = cb(); if (!c) return 0;   // ends the targets' pass
    VkRenderPassBeginInfo rb = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = g_null_rp, .framebuffer = *fbp, .renderArea = area};
    vkCmdBeginRenderPass(c, &rb, VK_SUBPASS_CONTENTS_INLINE); bf_reset();   // its pipeline, viewport and scissor are set behind the bind filter (bf_*): unknown to it
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pp->p);
    if (pp->lbpc) { int32_t b0 = 0; vkCmdPushConstants(c, pp->pl, pp->lbpc, SPV_LBASE_PC, 4, &b0); }   // the layer base: none (spv_fit_lbase)
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pp->pl, 0, 1, &set, vs->ndyn, dyn);
    if (vs->pool && !pool_bind(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pp->pl)) { vkCmdEndRenderPass(c); cb_barrier(); bf_reset(); g_drawing = 0; return 0; }
    if (vs->gm && g_gm_np) gm_np_bind(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pp->pl);
    VkViewport v = {0, 0, (float)side, (float)side, 0, 1}; vkCmdSetViewport(c, 0, 1, &v); vkCmdSetScissor(c, 0, 1, &area);
    int ok = bind_vbufs(c, vs, dw->first, (int64_t)dw->first + dw->count, (uint32_t)dw->base_instance, 1);
    if (ok) vkCmdDraw(c, (uint32_t)dw->count, 1, 0, (uint32_t)dw->base_instance - inst_shift((uint32_t)dw->base_instance));   // gl_VertexIndex counts from 0: the vertex buffers start at the first vertex
    vkCmdEndRenderPass(c); cb_barrier(); bf_reset();
    g_drawing = 0;
    { static int once; if (!once) { once = 1; LOG("[vk] transform feedback: first capture (vertex %s, %d vertices, in the %s stage)", vs->key, dw->count, frag ? "fragment" : "vertex"); } }
    g_captures++;
    return ok;
}
static uint64_t g_draws, g_draws_dropped;
static int vk_draw_(const p_draw *dw, const p_draw_idx *di);
static void rd_close(void);
static int ind_read(uint64_t addr, void *out, size_t n) {   // indirect arguments (and multi-draw counts), read on the CPU once the GPU's writes have landed
    if (!addr || !mtl_addr(addr, n, NULL, NULL)) { RED_ONCE(105, "indirect arguments at %#llx are in no memory pool: the draw is skipped", (unsigned long long)addr); return 0; }
    memcpy(out, (const void *)(uintptr_t)addr, n); return 1;
}
static int vk_draw__(const p_draw *dw, const p_draw_idx *di) { int r = vk_draw_(dw, di); rd_close(); g_vdisp = 0; g_vr0 = g_vr1 = 0; g_drawing = 0; g_fwd_nwb = -1; g_c1d_on = 0; tc_clear(); return r; }
// DM_VK_TBUF_RANGE=0 (env.txt): a shader with a buffer texture or image syncs its draw on anything pending, as before draw_view looked their memory up (the A/B of that alone)
static int tbuf_range_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_TBUF_RANGE"); v = !(e && *e == '0'); if (!v) LOG("[vk] DM_VK_TBUF_RANGE=0: a buffer texture or image syncs on anything pending"); } return v; }
// DM_VK_DEP_LOG=<n> (env.txt; a diagnostic): one in every n reads that met a pending write-back is logged, the whole run long, instead of the first 40
static int dep_every(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_DEP_LOG"); v = e && atoi(e) > 0 ? atoi(e) : 0; } return v; }
static void dep_log(vshader *const sh[4], const rd_rng *r, const char *kd, const signed char *ks, int n) {   // reads that met a pending write-back, with what wrote it
    static int nlog; static uint64_t nhit; int every = dep_every(); if (!C || (!every && nlog >= 40)) return;
    for (int q = 0; q < n; q++) for (int w = 0; w < C->nwb; w++) if (wb_hits(&C->wb[w], r[q].lo, r[q].hi)) {
        if (every ? (nhit++ % (uint64_t)every) != 0 : nlog++ >= 40) continue;
        const wb_t *b = &C->wb[w]; const vshader *rs = ks[q] >= 0 && sh[ks[q]] ? sh[ks[q]] : sh[0];
        LOG("[vk] draw dependency: shader %s (stage %d) reads %c %#lx+%#lx, pending write-back %d of %d: %s%s dst %p rows %zu x %zu", rs ? rs->key : "-", rs ? rs->stage : -1, kd[q], (unsigned long)r[q].lo,
            (unsigned long)(r[q].hi - r[q].lo), w, C->nwb, b->ofw ? "order-free copy (storage buffer)" : b->inpl ? "in place in a placed pool (storage buffer)" : b->st >= 0 ? "staging (texture copy)" : b->snap ? "arena with snapshot (storage buffer)" : "arena (feedback / buffer image)", b->fwd ? " of a dispatch" : "",
            (void *)b->dst, b->rows, b->row_bytes); }
}
// What a draw copies from guest memory lands first when an earlier draw, dispatch or copy wrote it and that write is still pending (gfx_vk.c ranges_view):
// the vertex streams, the index buffer, each stage's uniform slots and storage buffers, and its buffer textures and images. Called before anything of the draw is recorded.
// A buffer texture's memory is the texture pool entry its handle names, looked up as stage_set's tbuf_view does. Synced on anything pending instead (cpu_view), it made every
// such draw after one GPU write-back a round trip: Final Fantasy Resonance's opening reads buffer textures in most draws, ~100 syncs a frame, 15.5 fps on the Odin 2 and 10 on the Pad 3
static void draw_view(vshader *const sh[4], const p_draw_idx *di, int vtx) {   // vtx: the vertex streams too (a draw; 0 for a dispatch)
    if (vk_safe()) { g_fwd_nwb = 0; cpu_view(); return; }
    if (!C && fwd_open_on()) cur();   // the chunk the draw is recorded in, before the lookups of pending writes in the chunks in flight (fwd_open_on)
    if (gm_view(sh)) { g_fwd_nwb = 0; return; }   // raw global memory reads anything at the GPU's time: with DM_VK_RAW_GMEM_SYNC=1 every pending write-back landed (gfx_vk_gmem.inc)
    rd_rng r[16 + 1 + 4 * 32 + 4 * 32]; char kd[16 + 1 + 4 * 32 + 4 * 32], K = 'v'; signed char ks[16 + 1 + 4 * 32 + 4 * 32], KS = -1; int n = 0, over = 0;
    #define RNG(a, s) do { uintptr_t a_ = (uintptr_t)(a); uint64_t s_ = (s); if (a_ && s_) { if (n < (int)(sizeof r / sizeof *r)) { kd[n] = K; ks[n] = KS; r[n++] = (rd_rng){a_, a_ + (uintptr_t)s_}; } else over = 1; } } while (0)
    #define FWD(f, a, s, w) do { VkBuffer fb_; VkDeviceSize fo_; int w_ = (w);   /* a dispatch's pending write: read on the GPU (buf_fwd) */ \
        if (w_ && g_vdisp && (f) == sb_fwd && rd_home_on()) { uintptr_t hl_ = 0, hh_ = 0; if ((a) && (s)) rd_home_find(&g_rd_homes, (uintptr_t)(a), (uintptr_t)((a) + (s)), &hl_, &hh_); \
            if (hh_ > hl_ && !fwd_find(hl_, hh_, &fb_, &fo_)) RNG(hl_, hh_ - hl_); break; }   /* a discarded draw's write: its home range, found whole on the GPU or landed (rd_bind_w) */ \
        if (!w_ && (a) && (s)) wb_promote((uintptr_t)(a), (uintptr_t)((a) + (s)));   /* a draw's write it reads whole: on the GPU after a barrier */ \
        if ((w_ && !cs) || !f((a), (s), w_, &fb_, &fo_)) RNG(a, s); } while (0)
    g_fwd_nwb = C ? C->nwb : 0;   // cs: a dispatch (its stage alone), a draw run as one (g_vdisp), or a task / mesh draw in compute form, whose kernels are dispatches behind full barriers (gfx_vk_meshemu.inc)
    const int cs = (sh[0] && (sh[0]->stage == SH_CS || ((sh[0]->stage == SH_MS || sh[0]->stage == SH_TK) && !g_mesh))) || g_vdisp;
    g_rd_cs = cs;
    if (vtx) { uint32_t seen = 0; for (int i = 0; i < 16; i++) { va_st a; memcpy(&a, &D.attr[i], 4); VkBuffer fb; VkDeviceSize fo;
        if (a.fmt && fwd_early()) { if (seen >> a.stream & 1) continue; seen |= 1u << a.stream; }   // each stream once (stream_fwd)
        if (a.fmt && !stream_fwd(sh[0], a.stream, &fb, &fo)) { uintptr_t sl, sh_; stream_rng(a.stream, &sl, &sh_); RNG(sl, sh_ - sl); } } }
    K = 'i'; if (di && di->ibuf) RNG(di->ibuf, (uint64_t)(di->count > 0 ? di->count : 0) * (di->itype == 0 ? 1 : di->itype == 1 ? 2 : 4) + 4);
    for (int k = 0; k < 4; k++) { const vshader *s = sh[k]; if (!s) continue; int nst = nvn_stage_of(s->stage); KS = (signed char)k;
        if (!tbuf_range_on()) { for (int i = 0; i < s->ntx; i++) if ((s->tx[i].type & 0xff) == 2) { g_vs = 5; cpu_view(); return; }
                                for (int i = 0; i < s->nim; i++) if ((s->im[i].type & 0xff) == 2) { g_vs = 6; cpu_view(); return; } }
        K = 'u'; for (int i = 0; i < s->ncb; i++) { int sl = s->cb[i].slot; if (sl < 3 || sl - 3 >= 16) continue; const p_bind_buf *u = &D.ubo[nst][sl - 3];
            size_t un = u->size > 0x10000 ? 0x10000 : (size_t)u->size; if (s->cb[i].ext && un > s->cb[i].ext) un = s->cb[i].ext;   // what stage_set binds (ub_fwd there)
            VkBuffer ub_; VkDeviceSize uo_; if (u->addr && un && !ext_check()) { wb_promote((uintptr_t)u->addr, (uintptr_t)(u->addr + un)); if (ub_fwd(u->addr, un, &ub_, &uo_)) continue; }
            RNG(u->addr, u->size); }
        K = 's'; for (int i = 0; i < s->nsb; i++) { const rb_sbuf *sb = &s->sb[i]; uint32_t off = 4u * (uint32_t)sb->cb_off, base = (uint32_t)c_bases[nst] << 4;
            if (sb->cb_slot == 0 && nst == 5 && off >= 0x210 && off < 0x310 && !((off - 0x210) % 16)) { const p_bind_buf *u = &D.ubo[5][(off - 0x210) / 16]; if (!ofw_ok(sb, cs, u->addr, u->size)) FWD(sb_fwd, u->addr, u->size, sb->flags & 1); }   // compute: its uniform buffer table (stage_set); an order-free write needs no sync (ofw_bind)
            else if (sb->cb_slot == 0 && base && off >= base && off - base < 0x100 && !((off - base) % 16)) { const p_bind_buf *u = &D.ssbo[nst][(off - base) / 16]; if (!ofw_ok(sb, cs, u->addr, u->size)) FWD(sb_fwd, u->addr, u->size, sb->flags & 1); } } }
    if (over) { cpu_view(); g_fwd_nwb = 0; return; }   // more ranges than r holds: anything pending lands
    dep_log(sh, r, kd, ks, n);
    int before = (int)(g_view_cur + g_view_fly); ranges_view(r, n);
    if ((int)(g_view_cur + g_view_fly) != before) { g_fwd_nwb = 0; return; }   // synced: everything landed (nothing to read on the GPU)
    n = 0; over = 0;   // storage buffers named by the game's own uniform buffers: their addresses are read from them (landed, as checked above)
    K = 's'; for (int k = 0; k < 4; k++) { const vshader *s = sh[k]; if (!s) continue; int nst = nvn_stage_of(s->stage); KS = (signed char)k;
        for (int i = 0; i < s->nsb; i++) { const rb_sbuf *sb = &s->sb[i]; uint32_t off = 4u * (uint32_t)sb->cb_off;
            if (sb->cb_slot < 3 || sb->cb_slot - 3 >= 16) continue;
            const p_bind_buf *g = &D.ubo[nst][sb->cb_slot - 3]; uint64_t ad; uint32_t sz;
            if (g->addr && off + 12 <= g->size) { memcpy(&ad, (const void *)(uintptr_t)(g->addr + off), 8); memcpy(&sz, (const void *)(uintptr_t)(g->addr + off + 8), 4); if (!ofw_ok(sb, cs, ad, sz)) FWD(sb_fwd, ad, sz, sb->flags & 1); } } }
    // buffer textures (t) and images (m): their handles may sit in the game's uniform buffers too (landed above), and their memory is the registry entry each names
    if (tbuf_range_on()) { pthread_mutex_lock(&g_reg_mu);
        for (int k = 0; k < 4; k++) { const vshader *s = sh[k]; if (!s) continue; int nst = nvn_stage_of(s->stage); KS = (signed char)k;
            for (int pass = 0; pass < 2; pass++) for (int i = 0; i < (pass ? s->nim : s->ntx); i++) { const rb_tex *t = pass ? &s->im[i] : &s->tx[i];
                // a buffer texture read as rows (tex_rows_slot) is uploaded from guest memory when its pages change (gfx_vk.c vt_get_): a write to it still pending lands
                // first, so stage_textures sees its pages written. Only for a shader that has met one (tbrows): the other texture slots cost no lookup here
                int rows = !pass && s->tbrows && tex_rows_slot(t); if ((t->type & 0xff) != 2 && !rows) continue;
                int cnt = t->array > 1 ? (t->array > TEX_ARR_MAX ? TEX_ARR_MAX : t->array) : 1; uint32_t w0 = (uint32_t)t->handle & 0x3fff; K = pass ? 'm' : 't';
                for (int e = 0; e < cnt; e++) { int tid = (int)(bank_word(nst, NULL, t->cbuf_slot, (w0 + 2u * e) * 4) & 0xfffff); if (tid && t->cbuf_slot >= 3) tid += D.tex_base;
                    if (tid <= 0 || tid >= g_ntreg || !g_treg[tid].ok) continue;
                    if (rows) { if (g_treg[tid].t.d.target == NVN_TEXTURE_TARGET_BUFFER) RNG(g_treg[tid].t.mem, g_treg[tid].t.L.size); continue; }
                    FWD(tb_fwd, (uint64_t)(uintptr_t)g_treg[tid].t.mem, tbuf_bytes(&g_treg[tid].t), pass); } } }
        pthread_mutex_unlock(&g_reg_mu); }
    #undef RNG
    #undef FWD
    if (over) { g_vs = 5; cpu_view(); g_fwd_nwb = 0; return; }   // more than r holds (a long array): anything pending lands
    dep_log(sh, r, kd, ks, n);
    if (n) { g_view_free--; before = (int)(g_view_cur + g_view_fly); ranges_view(r, n); if ((int)(g_view_cur + g_view_fly) != before) g_fwd_nwb = 0; }   // counted once
}
static int vk_draw(const p_draw *dw, const p_draw_idx *di) {
    { uintptr_t ia = dw ? dw->ind : di->ind, ma = dw ? dw->mdi_cnt : di->mdi_cnt; rd_rng r[2]; int n = 0;   // indirect arguments, read on the CPU below
      if (ia) r[n++] = (rd_rng){ia, ia + 20}; if (ma) r[n++] = (rd_rng){ma, ma + 4};
      if (n) ranges_view(r, n); }
    uint64_t mdi = dw ? dw->mdi_cnt : di->mdi_cnt; int mdi_i = dw ? dw->mdi_i : di->mdi_i;
    if (mdi) { uint32_t n = 0; if (!ind_read(mdi, &n, 4)) return 0; if ((uint32_t)mdi_i >= n) return 1; }   // past the count the GPU wrote: not drawn
    p_draw d2; p_draw_idx i2;
    if (dw && dw->ind) { uint32_t a[4]; if (!ind_read(dw->ind, a, 16)) return 0;   // count, instances, first, base instance
        d2 = *dw; d2.ind = 0; d2.count = (int32_t)a[0]; d2.instances = (int32_t)a[1]; d2.first = (int32_t)a[2]; d2.base_instance = (int32_t)a[3]; dw = &d2; if (!a[0] || !a[1]) return 1; }
    if (di && di->ind) { uint32_t a[5]; if (!ind_read(di->ind, a, 20)) return 0;   // count, instances, first index, base vertex, base instance
        i2 = *di; i2.ind = 0; i2.count = (int32_t)a[0]; i2.instances = (int32_t)a[1]; i2.ibuf = di->ibuf + (uint64_t)a[2] * (di->itype == 0 ? 1u : di->itype == 1 ? 2u : 4u);
        i2.base_vertex = (int32_t)a[3]; i2.base_instance = (int32_t)a[4]; di = &i2; if (!a[0] || !a[1]) return 1; }
    return vk_draw__(dw, di);
}
// ---- The pipeline-key memo (vk_draw_) ----
// A draw whose pipeline key has every input of the draw before's has its pipeline: the bound state the key takes (D, through g_pso_gen: draw_state_op bumps it only when
// one of those values really changes), the shaders (the program memo's or resolved), the pass (P.rp and its key), the primitive, the depth mode (g_depth_zero) and the
// queue's raster flip (g_queue_ll). The rest the key reads is fixed per shader (io_in, modz) or device (logicOp). Building the key (about 400 bytes, field by field and
// masked), its 49-word hash and its compare in pipe_get cost Breath of the Wild on the Odin 2 about 2.35% of the replay thread (pipe_get 1.72%, the build 0.63%), for 82% of
// draws that repeat the draw before's state (attributes included). Pipelines are never freed (g_pipes keeps every pipe_t; prewarm_thread only adds), so the memo keeps a
// pointer. Only a made pipeline is kept; a dump frame builds every key (its log reads the key's fields). DM_VK_PSO_MEMO=0 (env.txt): every draw builds its key, as before;
// DM_VK_PSO_CHECK=1 (a diagnostic, off by default): a draw the memo answers builds the key anyway and takes the full key's pipeline; a key or pipeline other than the
// memo's is a [RED] line (the first 16) naming the fields that differ, and the counts are in the periodic statistics (DM_VK_MEMO_LOG=1)
static int pso_memo_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_PSO_MEMO"); v = !(e && *e == '0'); if (!v) LOG("[vk] DM_VK_PSO_MEMO=0: every draw builds its pipeline key"); } return v; }
static int pso_check(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_PSO_CHECK"); v = e && *e && *e != '0'; if (v) LOG("[vk] DM_VK_PSO_CHECK=1: the pipeline memo's answers are checked against full keys"); } return v; }
static struct { uint64_t gen; vshader *sh[4]; VkRenderPass rp; rp_key rk; int prim, z; uint8_t ll; pipe_t *pp; pipe_key k; } g_pso_m;   // k: kept for DM_VK_PSO_CHECK only
static uint64_t g_pso_hit, g_pso_miss, g_pso_checked, g_pso_bad;
static pipe_t *pso_memo_find(vshader *const sh[4], int prim) {
    if (!pso_memo_on() || dump_on() || !g_pso_m.pp || g_pso_m.gen != g_pso_gen || memcmp(g_pso_m.sh, sh, sizeof g_pso_m.sh) || g_pso_m.rp != P.rp || memcmp(&g_pso_m.rk, &P.key, sizeof P.key)
        || g_pso_m.prim != prim || g_pso_m.z != !g_depth_zero || g_pso_m.ll != (uint8_t)g_queue_ll) return NULL;
    g_pso_hit++; return g_pso_m.pp;
}
static void pso_memo_note(const pipe_key *k, int prim, pipe_t *pp, pipe_t *pm) {   // the key built, its pipeline pp; pm: the memo's answer (DM_VK_PSO_CHECK), compared
    if (!pso_memo_on()) return;
    if (pm) { g_pso_checked++; if (pp == pm && !memcmp(k, &g_pso_m.k, sizeof *k)) return;
        if (g_pso_bad++ < 16) { char d[300] = ""; for (size_t i = 0; i < sizeof PK_F / sizeof *PK_F; i++) if (memcmp((const uint8_t *)k + PK_F[i].o, (const uint8_t *)&g_pso_m.k + PK_F[i].o, PK_F[i].s)) { size_t l = strlen(d); snprintf(d + l, sizeof d - l, " %s", PK_F[i].n); }
            LOG("[RED] [vk] pipeline memo check: shaders %s %s, the memo's key differs in:%s%s", k->sh[0] ? k->sh[0]->key : "-", k->sh[1] ? k->sh[1]->key : "-", d[0] ? d : " (padding only)", pp != pm ? " (another pipeline)" : ""); } }
    else g_pso_miss++;
    g_pso_m.gen = g_pso_gen; memcpy(g_pso_m.sh, k->sh, sizeof g_pso_m.sh); g_pso_m.rp = P.rp; g_pso_m.rk = P.key; g_pso_m.prim = prim; g_pso_m.z = !g_depth_zero;
    g_pso_m.ll = (uint8_t)g_queue_ll; g_pso_m.pp = pp;
    if (pso_check()) g_pso_m.k = *k;
}
// the pipeline key of the draw being made, over k's zeroed bits with its shaders set: the bound state (D), the pass (P), the primitive and the origin
static void pipe_key_build(pipe_key *k, int prim) {
    k->z = !g_depth_zero; k->rp = P.rp; k->rk = P.key; k->ncolor = P.key.ncolor; k->ll = (uint8_t)g_queue_ll;
    memcpy(k->attr, D.attr, sizeof k->attr); memcpy(k->stream, D.stream, sizeof k->stream);
    k->topo = (uint8_t)topo_of(prim); k->restart = (uint8_t)D.restart.on; k->depth_clamp = (uint8_t)D.depth_clamp;
    // the state objects field by field over zeroed bits: bind_state copies the game's objects whole, and the bits the fields leave over (16 of colour's 32, 24 of polygon's,
    // 18 of each blend's 64, 25 of depth / stencil's) are whatever the game's object held (on its stack, without SetDefaults): each value made a new pipeline key
    k->color.blend_en = D.color.blend_en; k->color.logic_op = D.color.logic_op; k->color.alpha_func = D.color.alpha_func;
    k->chmask = D.chmask; k->ms = D.ms;
    k->poly.cull = D.poly.cull; k->poly.front = D.poly.front; k->poly.mode = D.poly.mode; k->poly.offset_en = D.poly.offset_en;
    for (int i = 0; i < 8; i++) blend_k(&k->blend[i], &D.blend[i]);
    ds_k(&k->ds, &D.ds);
    if (!P.key.depth) { k->ds.dt = 0; k->ds.st = 0; }
    // only what the pipeline uses goes in its key: state left over from other draws (attributes the shader does not read, streams no read attribute uses, blend of
    // targets that are not there or not blended, write masks of absent targets, depth / stencil functions while their test is off, primitive restart of a list topology,
    // multisample and alpha-test fields the pipeline does not take) made a new pipeline of the same one again and again: 54,164 pipelines listed in a session of
    // Super Mario Bros. Wonder, a creation (about 2 ms on the Adreno) every few frames
    { uint32_t vin = k->sh[0]->io_in, streams = 0;
      for (int i = 0; i < 16; i++) { va_st a; memcpy(&a, &k->attr[i], 4); if (!(vin >> i & 1)) k->attr[i] = 0; else if (a.fmt) streams |= 1u << a.stream; }
      for (int s = 0; s < 16; s++) if (!(streams >> s & 1)) k->stream[s] = (vs_st){0, 0};
      for (uint32_t i = 0; i < 8; i++) if (i >= k->ncolor || !(k->color.blend_en >> i & 1)) memset(&k->blend[i], 0, sizeof k->blend[i]);
      if (k->ncolor < 8) k->chmask.mask &= (1u << (4 * k->ncolor)) - 1;
      // the alpha test's function stays in the key while a test runs (a fragment shader and a float colour 0): its pipeline's fragment module is the
      // alpha-test variant, made by the function and the reference (ps_alpha_mod, VkSpecializationInfo)
      if (!(k->sh[1] && !k->tf && k->color.alpha_func && k->color.alpha_func != NVN_ALPHA_FUNC_ALWAYS && !(k->ncolor && is_int_fmt(k->rk.color[0])))) k->color.alpha_func = 0;
      if (k->ncolor < 8) k->color.blend_en &= (1u << k->ncolor) - 1;   // enables of targets the pass does not have, and which of the three polygon offsets is on (the pipeline
      k->poly.offset_en = k->poly.offset_en != 0;                       // takes one depth-bias switch): 186,825 keys listed after a few sessions of Super Mario Bros. Wonder
      if (k->color.logic_op != NVN_LOGIC_OP_COPY && !g_vk_feat.logicOp) k->color.logic_op = NVN_LOGIC_OP_COPY;   // drawn as copy anyway (pipe_make)
      if (!k->ds.dt) { k->ds.df = 0; k->ds.dw = 0; }
      if (!k->ds.st) { k->ds.sf_f = k->ds.sf_b = 0; k->ds.fail_f = k->ds.zfail_f = k->ds.zpass_f = k->ds.fail_b = k->ds.zfail_b = k->ds.zpass_b = 0; }
      VkPrimitiveTopology tp = (VkPrimitiveTopology)k->topo;
      if (!(tp == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP || tp == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP || tp == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN || tp == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY || tp == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY)) k->restart = 0;
      ms_st m; memset(&m, 0, sizeof m); m.a2c = k->ms.a2c; k->ms = m;
      if (!k->sh[0]->modz) k->z = 0; }
}
// a draw's dynamic state: the viewport (see the file header for the origin), the scissor, stencil, blend constants, depth bias (vk_draw_, gfx_vk_mesh.inc vk_mesh_draw_);
// *ov / *osc: the viewport and scissor set, for the dump's log; rd: the 1 × 1 pass without attachments of a discarded draw (rd_draw: rasterization is off)
static void draw_dyn_state(VkCommandBuffer c, VkViewport *ov, VkRect2D *osc, int rd) {
    float vx = 0, vy = 0, vw = (float)P.w, vh = (float)P.h; if (D.has_vp) { vx = D.vp[0][0]; vy = D.vp[0][1]; vw = D.vp[0][2]; vh = D.vp[0][3]; }
    float n = D.dr[0] < 0 ? 0 : D.dr[0] > 1 ? 1 : D.dr[0], f = D.dr[1] < 0 ? 0 : D.dr[1] > 1 ? 1 : D.dr[1];
    VkViewport v = g_origin_ll ? (VkViewport){vx, vy, vw, vh, n, f} : (VkViewport){vx, vy + vh, vw, -vh, n, f};
    if (rd) v = (VkViewport){0, 0, 1, 1, 0, 1};
    if (!bf_same(c, BF_VP, &g_bf.vp, &v, sizeof v)) vkCmdSetViewport(c, 0, 1, &v);
    VkRect2D sc = {{0, 0}, {P.w, P.h}};
    if (R.has_sc) { int x0 = R.sc.x < 0 ? 0 : R.sc.x, y0 = R.sc.y < 0 ? 0 : R.sc.y; int64_t x1 = (int64_t)R.sc.x + R.sc.w, y1 = (int64_t)R.sc.y + R.sc.h;
        if (x1 > (int64_t)P.w) x1 = P.w; if (y1 > (int64_t)P.h) y1 = P.h;
        sc = (VkRect2D){{x0, y0}, {(uint32_t)(x1 > x0 ? x1 - x0 : 0), (uint32_t)(y1 > y0 ? y1 - y0 : 0)}}; }
    if (rd) sc = (VkRect2D){{0, 0}, {1, 1}};
    if (!bf_same(c, BF_SC, &g_bf.sc, &sc, sizeof sc)) vkCmdSetScissor(c, 0, 1, &sc);
    if (!bf_same(c, BF_BC, g_bf.bc, D.blend_color, sizeof g_bf.bc)) vkCmdSetBlendConstants(c, D.blend_color);
    if (!bf_same(c, BF_CM0, &g_bf.cm[0], &D.st_vmask[0], 4)) vkCmdSetStencilCompareMask(c, VK_STENCIL_FACE_FRONT_BIT, D.st_vmask[0]);
    if (!bf_same(c, BF_CM1, &g_bf.cm[1], &D.st_vmask[1], 4)) vkCmdSetStencilCompareMask(c, VK_STENCIL_FACE_BACK_BIT, D.st_vmask[1]);
    if (!bf_same(c, BF_WM0, &g_bf.wm[0], &D.st_wmask[0], 4)) vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_FRONT_BIT, D.st_wmask[0]);
    if (!bf_same(c, BF_WM1, &g_bf.wm[1], &D.st_wmask[1], 4)) vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_BACK_BIT, D.st_wmask[1]);
    if (!bf_same(c, BF_REF0, &g_bf.ref[0], &D.st_ref[0], 4)) vkCmdSetStencilReference(c, VK_STENCIL_FACE_FRONT_BIT, D.st_ref[0]);
    if (!bf_same(c, BF_REF1, &g_bf.ref[1], &D.st_ref[1], 4)) vkCmdSetStencilReference(c, VK_STENCIL_FACE_BACK_BIT, D.st_ref[1]);
    { float db[3] = {D.po.units, g_vk_feat.depthBiasClamp ? D.po.clamp : 0, D.po.factor}; if (!bf_same(c, BF_DB, g_bf.db, db, sizeof db)) vkCmdSetDepthBias(c, db[0], db[1], db[2]); }
    *ov = v; *osc = sc;
}
// ---- Draws under rasterizer discard that write memory ----
// Rasterizer discard stops a draw at rasterization: its vertex and geometry shaders still run, with their storage buffer and image writes and atomics. Xenoblade
// Chronicles 2 skins each character with such a draw (shd_post.wishp var122, no transform feedback): its vertex shader writes the blended bone matrices, 48 bytes
// at gl_VertexIndex, into slot 0 of one 384,000-byte storage buffer, which the character's draws then read. The replay made a discarded draw only as a transform
// feedback capture (tf_capture), so these did nothing and every character went undrawn on Android, the world and the HUD around them drawn (nvn_mtl.m met the
// same on Xenoblade Chronicles 3 and Metroid Prime Remastered: docs/reference/rendering.md, Rasterizer discard preserves vertex storage writes).
// Such a draw now runs as a dispatch does (vk_dispatch): the targets' pass ended before its bindings are made, its vertex (and geometry) stage alone in a pipeline
// with rasterization off in the pass without attachments (rd_key), a full barrier before it (earlier draws may still read what it writes over: XC2's next character
// writes from slot 0 again) and after it (rd_close: later draws read what it wrote). So, as a dispatch's, its writes may go in place on a write-back of the chunk and
// later shaders read them on the GPU (g_vdisp: disp_like, buf_fwd) instead of syncing: XC2 skins dozens of characters a frame, each read by the draws after it.
// The cost is a pass break at each one. A discarded draw that writes nothing still draws nothing. DM_VK_RAST_DISCARD=0 (env.txt): every discarded draw without
// transform feedback is left out, as before (the A/B of this alone)
static int rd_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_RAST_DISCARD"); v = !(e && *e == '0'); if (!v) LOG("[vk] DM_VK_RAST_DISCARD=0: draws under rasterizer discard are left out unless transform feedback captures them"); } return v; }
static uint64_t g_rd_made, g_rd_none;   // discarded draws made as dispatches; left out (nothing they write)
static int g_rd_open;   // the pass without attachments of the discarded draw being made is open (rd_open; vk_draw__ closes it, whatever the draw's outcome)
static int rd_writes(const vshader *s) {   // the stage writes memory: a storage buffer (or atomics on one: Ryujinx's BufferUsageFlags.Write), an image (TextureUsageFlags.ImageStore), raw global memory (its write log)
    if (!s) return 0;
    for (int i = 0; i < s->nsb; i++) if (s->sb[i].flags & 1) return 1;
    for (int i = 0; i < s->nim; i++) if (s->im[i].flags & 4) return 1;
    return s->gm == 2;
}
static int rd_draw(vshader *const sh[4]) {   // under rasterizer discard, without a transform feedback capture: whether the draw runs, as a dispatch
    if (D.tf_on || !rd_on()) return 0;
    if (!rd_writes(sh[0]) && !rd_writes(sh[2])) { g_rd_none++; return 0; }
    if (!g_vk_feat.vertexPipelineStoresAndAtomics) { RED_ONCE(184, "a draw under rasterizer discard writes memory from its vertex stage (shader %s), which this GPU cannot: left out (said once)", sh[0]->key); g_rd_none++; return 0; }
    return 1;
}
static void rd_key(pipe_key *k) {   // over the built key: the vertex (and geometry) stage alone, rasterization off, in the pass without attachments; no target state
    k->tf = 2; k->rp = null_pass(); memset(&k->rk, 0, sizeof k->rk); k->ncolor = 0; k->z = 0; k->ll = 0; k->depth_clamp = 0;
    memset(&k->color, 0, sizeof k->color); k->color.logic_op = NVN_LOGIC_OP_COPY; memset(&k->chmask, 0, sizeof k->chmask); memset(k->blend, 0, sizeof k->blend);
    memset(&k->ms, 0, sizeof k->ms); memset(&k->poly, 0, sizeof k->poly); memset(&k->ds, 0, sizeof k->ds);
}
static int rd_open(void) {   // after the draw's bindings (stage_set), in their chunk: the pass without attachments begun, after a full barrier
    VkCommandBuffer c = cb(); if (!c || !null_pass()) return 0;   // cb: outside the targets' pass
    cb_barrier();
    VkRenderPassBeginInfo rb = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = g_null_rp, .framebuffer = g_null_fb, .renderArea = {{0, 0}, {1, 1}}};
    vkCmdBeginRenderPass(c, &rb, VK_SUBPASS_CONTENTS_INLINE); bf_reset();   // its binds are made behind the bind filter's back: it forgets what it knew
    g_rd_open = 1; return 1;
}
static void rd_close(void) {   // the pass ended, the scratch copies back in their home copies (rd_bind_w), and everything after it ordered after the draw's writes
    int opened = g_rd_open, nb = 0;
    if (opened) { g_rd_open = 0; if (C && C->open) { vkCmdEndRenderPass(C->cb); cb_barrier(); } bf_reset(); }
    for (int i = 0; i < g_rd_nback && opened; i++) { if (!C || !C->open || g_rd_back[i].seq != C->seq) continue;   // (not drawn: the scratch holds what the home copy does; another chunk: a sync made it, and the draw took its bindings again there)
        VkBufferCopy cp = {g_rd_back[i].so, g_rd_back[i].ho, g_rd_back[i].n}; vkCmdCopyBuffer(C->cb, g_rd_back[i].sb, g_rd_back[i].hb, 1, &cp); nb++; }
    if (nb) cb_barrier();
    g_rd_nback = 0;
}
static uint64_t g_drop[8];   // draws not made, by reason: 0 shader missing, 1 targets, 2 pipeline, 3 descriptors, 4 an invalid binding, 5 pass, 6 vertex buffers
static int vk_draw_(const p_draw *dw, const p_draw_idx *di) {
    if (!g_d_init) draw_state_reset();
    if (!D.bst[0]) { RED_ONCE(95, "draw with no vertex program bound"); return 0; }
    pipe_key k; memset(&k, 0, sizeof k); int pst[4] = {-1, -1, -1, -1}; const prog_t *pprog[4] = {0};
    prog_memo_t *pe = NULL; uint64_t pser[5] = {0}, pgen = 0;   // the program-to-shader memo (prog_memo_on): pe the entry of the bound programs
    if (prog_memo_on()) {
        pgen = atomic_load_explicit(&g_prog_gen, memory_order_acquire); uint64_t h = pgen; int ok = 1;
        for (int b = 0; b < 5 && ok; b++) { const prog_t *pr = D.bst[b]; if (pr && !(pser[b] = pr->serial)) ok = 0; h = (h ^ (uintptr_t)pr ^ pser[b] * 0x9e3779b97f4a7c15ull) * 0xff51afd7ed558ccdull; }
        if (!ok) g_pmemo_ser0++;
        else if ((pe = &g_pmemo[(h >> 40) & (PMEMO - 1)])->gen == pgen && !memcmp(pe->pr, D.bst, sizeof pe->pr) && !memcmp(pe->ser, pser, sizeof pser)) {
            for (int q = 0; q < 4; q++) { if ((k.sh[q] = pe->sh[q])) { pst[q] = pe->pst[q]; pprog[q] = D.bst[q]; }
                g_c1d[q].pr = pprog[q]; g_c1d[q].st = pst[q]; g_c1d[q].ok = pe->c1ok[q]; g_c1d[q].o = pe->c1[q][0]; g_c1d[q].e = pe->c1[q][1]; }
            g_c1d_on = 1; g_pmemo_hit++; goto resolved;
        }
    }
    for (int b = 0; b < 5; b++) {   // each bound program gives the stages it is bound for
        const prog_t *pr = D.bst[b]; if (!pr) continue;
        int dup = 0; for (int q = 0; q < b; q++) dup |= D.bst[q] == pr; if (dup) continue;
        for (unsigned i = 0; i < pr->nstage && i < 6; i++) {
            uint8_t key[8]; char hx[17]; if (!prog_key(pr, i, key)) continue; key_hex(key, hx);
            vshader *s = sh_get(hx); if (!s) { g_drop[0]++; return 0; }
            if (s->stage == SH_CS || s->stage == SH_MS || s->stage == SH_TK) continue;   // a mesh program's stages: its DrawMeshTasks (gfx_vk_mesh.inc)
            if (s->stage == SH_TS) { RED_ONCE(100, "tessellation shaders are not on Vulkan yet: draw skipped (shader %s)", s->key); return 0; }
            int slot = s->stage == SH_VS ? 0 : s->stage == SH_PS ? 1 : s->stage == SH_GS ? 2 : 3;
            if (D.bst[slot] != pr) continue;
            k.sh[slot] = s; pst[slot] = (int)i; pprog[slot] = pr;
        }
    }
    if (!k.sh[0]) { RED_ONCE(96, "draw with no vertex shader"); return 0; }
    if (pe) {   // a complete resolution: kept, with the c1 ranges of the stages that read c1 (asked here instead of in stage_set)
        memcpy(pe->pr, D.bst, sizeof pe->pr); memcpy(pe->ser, pser, sizeof pser); pe->gen = pgen;
        for (int q = 0; q < 4; q++) { pe->sh[q] = k.sh[q]; pe->pst[q] = pst[q]; pe->c1ok[q] = -1; pe->c1[q][0] = pe->c1[q][1] = 0;
            if (k.sh[q] && has_c1(k.sh[q])) pe->c1ok[q] = (int8_t)prog_c1(pprog[q], (unsigned)pst[q], &pe->c1[q][0], &pe->c1[q][1]);
            g_c1d[q].pr = pprog[q]; g_c1d[q].st = pst[q]; g_c1d[q].ok = pe->c1ok[q]; g_c1d[q].o = pe->c1[q][0]; g_c1d[q].e = pe->c1[q][1]; }
        g_c1d_on = 1; g_pmemo_miss++;
    }
    resolved:;
    // under rasterizer discard: a transform feedback capture, nothing, or (a stage writes memory) the draw run as a dispatch, its fragment stage dropped and the
    // targets' pass ended before its bindings are made, as vk_dispatch does (an image it writes may be one of the targets)
    int rd = D.rast_discard && rd_draw(k.sh);
    if (rd) { k.sh[1] = NULL; g_vdisp = 1; rp_end(); }
    if (dw) { g_vr0 = dw->first; g_vr1 = (int64_t)dw->first + dw->count; }   // the vertices it fetches (stream_rng)
    draw_view(k.sh, di, 1);   // what this draw reads of guest memory: pending GPU writes to it land first
    if (D.rast_discard && !rd) return tf_capture(k.sh[0], pprog[0], pst[0], dw, di);   // the vertex stage only: a capture, or nothing
    for (int i = 0; i < 4; i++) if (k.sh[i]) stage_textures(k.sh[i]);
    int pool = (k.sh[0] && k.sh[0]->pool) || (k.sh[1] && k.sh[1]->pool) || (k.sh[2] && k.sh[2]->pool) || (k.sh[3] && k.sh[3]->pool);
    if (pool) pool_sync();   // the IDs read since made, before the pass begins (their uploads would end it)
    if (!rd && !rp_prepare_((k.sh[0] && k.sh[0]->wlayer) || (k.sh[2] && k.sh[2]->wlayer) || (k.sh[3] && k.sh[3]->wlayer))) { g_drop[1]++; return 0; }
    if (!rd && g_dup_mask) dup_check();
    g_drawing = 1;   // from here no chunk is closed (chunk_submit reports one)
    int prim = dw ? dw->prim : di->prim;
    pipe_t *pm = rd ? NULL : pso_memo_find(k.sh, prim), *pp = pm;   // the draw before's pipeline when nothing its key takes has changed (pso_memo_find); NULL: the key is built
    if (!pm || pso_check()) { pipe_key_build(&k, prim); if (rd) rd_key(&k); pp = pipe_get(&k); if (!pp) { g_drop[2]++; return 0; } if (!rd) pso_memo_note(&k, prim, pp, pm); }   // (the memo keeps the targets' pass's pipelines)
    uint32_t base_inst = (uint32_t)(dw ? dw->base_instance : di->base_instance);
    VkDescriptorSet sets[4] = {0}; uint32_t dyn[4][18];   // dyn: each stage's dynamic offsets (stage_set)
    for (int t = 0;; t++) {   // t 1: every stage again from the chunk recorded now when a sync inside stage_set closed the one the sets came from (ds_moved)
        int i = 0; g_bind_bad = 0;
        if (!cur()) { g_drop[3]++; return 0; }
        uint64_t dseq = C->seq;   // the chunk the sets come from (one: only a sync inside stage_set closes a chunk, and that stage_set then fails)
        for (; i < 4; i++) if (k.sh[i] && !stage_set(k.sh[i], 0, &sets[i], dyn[i], pprog[i], pst[i], base_inst, dseq)) break;
        if (i == 4) break;
        if (!ds_moved(dseq) || ds_again(t, 0)) { g_drop[3]++; return 0; }
    }
    if (g_bind_bad) { g_drop[4]++; static int nb; if (nb < 10) { nb++; LOG("[RED] [vk] draw skipped: a binding has no valid descriptor (shaders %s %s)", k.sh[0]->key, k.sh[1] ? k.sh[1]->key : "-"); } return 0; }
    if (rd ? !rd_open() : !rp_start()) { g_drop[5]++; return 0; }   // after the textures: their uploads end a pass begun before (it is begun again on the same targets); it only takes cur(), so the sets' chunk is the one they are bound in below
    VkCommandBuffer c = C->cb;
    if (!rd) occl_begin();
    else if (!g_rd_made++) LOG("[vk] rasterizer discard: a draw whose vertex stage writes memory runs as a dispatch, in a pass of its own (vertex %s, %u %s; DM_VK_RAST_DISCARD=0 leaves it out)", k.sh[0]->key, dw ? (unsigned)dw->count : (unsigned)di->count, dw ? "vertices" : "indices");
    bf_pipeline(c, pp->p);   // binds the command buffer already holds are left out (bf_*)
    if (pp->alpha) vkCmdPushConstants(c, pp->pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof D.alpha_ref, &D.alpha_ref);   // the alpha test's reference, data rather than a key (spv_alpha.h)
    if (pp->lbpc) { int32_t b0 = 0; vkCmdPushConstants(c, pp->pl, pp->lbpc, SPV_LBASE_PC, 4, &b0); }   // the layer base: none (spv_fit_lbase)
    for (int i = 0; i < 4; i++) if (k.sh[i]) vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pp->pl, (uint32_t)(k.sh[i]->set & 3), 1, &sets[i], k.sh[i]->ndyn, dyn[i]);
    if (pool && !pool_bind(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pp->pl)) { g_drop[3]++; return 0; }
    if (g_gm_np) { int gm = 0; for (int i = 0; i < 4; i++) gm |= k.sh[i] && k.sh[i]->gm; if (gm) gm_np_bind(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pp->pl); }   // (gfx_vk_gmem_noptr.inc)
    VkViewport v; VkRect2D sc; draw_dyn_state(c, &v, &sc, rd);
    // indices: copied, quads made triangles; vertex range for the vertex data copy
    int64_t vlo = 0, vhi = 0; uint32_t count = dw ? (uint32_t)dw->count : (uint32_t)di->count, inst = (uint32_t)(dw ? dw->instances : di->instances); if (!inst) inst = 1;
    // More instances than any Switch game draws (2^24): the GPU would work through them until the driver's hang recovery cut the submission short, and the game drew no
    // frame after. Final Fantasy Resonance's first field on the Adreno 730 asks 1 to 3.8 billion of one quad every frame, its own count (not found yet). A mitigation
    if (inst > (1u << 24)) { RED_ONCE(177, "a draw of %u instances (%#x) skipped, shaders %s %s, %u %s: more than any Switch game draws; the GPU would run it until its hang "
                                    "recovery (said once; every such draw is skipped)", inst, inst, k.sh[0] ? k.sh[0]->key : "-", k.sh[1] ? k.sh[1]->key : "-", count, di ? "indices" : "vertices"); return 1; }
    int quads = prim == 7, qstrip = prim == 8; VkBuffer ib = VK_NULL_HANDLE; VkDeviceSize ioff = 0; uint32_t icount = count; int i32 = 0;
    if (di) {
        int isz = di->itype == 0 ? 1 : di->itype == 1 ? 2 : 4; const uint8_t *src = (const uint8_t *)(uintptr_t)di->ibuf; uint32_t mx = 0, mn = UINT32_MAX;
        if (!src) { RED_ONCE(98, "indexed draw with no index buffer"); return 0; }
        uint32_t nq = quads ? count / 4 : qstrip ? (count >= 4 ? (count - 2) / 2 : 0) : 0;
        icount = quads || qstrip ? nq * 6 : count;
        uint32_t rv = D.restart.on ? D.restart.index : UINT32_MAX;
        // kept 16- and 32-bit indices stay in their own size (Vulkan's restart index is the type's all-ones, as NVN's 0xffff / 0xffffffff): half the index reads of a 16-bit
        // draw on the GPU, which the far map view is bound by (2.8 million indices a frame); 8-bit ones are widened, as Vulkan takes them only with VK_EXT_index_type_uint8
        int native = isz != 1;
        uint8_t *fill = NULL; gc_ent *ge = quads || qstrip ? NULL : gc_get((uintptr_t)src, (uint64_t)count * isz, 1u + 2u * isz + ((isz == 2 && rv == 0xffff) || (isz == 1 && rv == 0xff) ? 32u : 0u), (uint64_t)count * (native ? isz : 4), &fill);
        if (ge && fill && native) {   // copied as they are, the vertex range scanned once
            memcpy(fill, src, (size_t)count * isz);
            if (isz == 2) { const uint16_t *s16 = (const uint16_t *)(const void *)src; int rs = D.restart.on && rv == 0xffff;
                for (uint32_t j = 0; j < count; j++) { uint32_t x = s16[j]; if (rs && x == 0xffff) continue; if (x > mx) mx = x; if (x < mn) mn = x; } }
            else { const uint32_t *s32 = (const uint32_t *)(const void *)src; for (uint32_t j = 0; j < count; j++) { uint32_t x = s32[j]; if (x == UINT32_MAX) continue; if (x > mx) mx = x; if (x < mn) mn = x; } }
            if (mn == UINT32_MAX) mn = mx = 0;
            ge->mn = mn; ge->mx = mx; fill = NULL;
        }
        if (ge && !fill) {   // kept on the GPU, with its vertex range
            ib = gc_buf(ge); ioff = ge->off; i32 = !native || isz == 4; mn = ge->mn; mx = ge->mx;
            vlo = (int64_t)mn + di->base_vertex; vhi = (int64_t)mx + di->base_vertex + 1;
            goto indexed;
        }
        uint32_t *o = fill ? (uint32_t *)fill : (uint32_t *)ar_alloc((VkDeviceSize)icount * 4, &ib, &ioff); if (!o) return 0; i32 = 1;
        if (fill) { ib = gc_buf(ge); ioff = ge->off; } else g_gc_copy++;
        #define IDX(j) (isz == 1 ? (uint32_t)src[j] : isz == 2 ? (uint32_t)((const uint16_t *)(const void *)src)[j] : ((const uint32_t *)(const void *)src)[j])
        // the vertex range is taken from the indices as they are written: the arena may be write-combined memory (ar_mem_type), and reading the indices back from it
        // took over half of the render thread in a Super Mario Bros. Wonder level on an Adreno 619 (uncached reads)
        #define MM(x) do { uint32_t x_ = (x); if (x_ != UINT32_MAX) { if (x_ > mx) mx = x_; if (x_ < mn) mn = x_; } } while (0)
        if (quads || qstrip) for (uint32_t q = 0; q < nq; q++) { uint32_t a0, a1, a2, a3; if (quads) { a0 = IDX(4 * q); a1 = IDX(4 * q + 1); a2 = IDX(4 * q + 2); a3 = IDX(4 * q + 3); } else { a0 = IDX(2 * q); a1 = IDX(2 * q + 1); a2 = IDX(2 * q + 3); a3 = IDX(2 * q + 2); }
                uint32_t t6[6] = {a0, a1, a2, a0, a2, a3}; memcpy(o + 6 * q, t6, sizeof t6); MM(a0); MM(a1); MM(a2); MM(a3); }
        else if (isz == 2) { const uint16_t *s16 = (const uint16_t *)(const void *)src; int rs = D.restart.on && rv == 0xffff;
            for (uint32_t j = 0; j < count; j++) { uint32_t x = s16[j]; if (rs && x == 0xffff) x = UINT32_MAX; o[j] = x; MM(x); } }
        else if (isz == 4) { const uint32_t *s32 = (const uint32_t *)(const void *)src;
            for (uint32_t j = 0; j < count; j++) { uint32_t x = s32[j]; o[j] = x; MM(x); } }
        else for (uint32_t j = 0; j < count; j++) { uint32_t x = src[j]; if (x == 0xff && D.restart.on && rv == 0xff) x = UINT32_MAX; o[j] = x; MM(x); }
        #undef MM
        #undef IDX
        if (mn == UINT32_MAX) mn = mx = 0;
        if (fill) { ge->mn = mn; ge->mx = mx; }
        vlo = (int64_t)mn + di->base_vertex; vhi = (int64_t)mx + di->base_vertex + 1;
        indexed:;
    } else {
        vlo = dw->first; vhi = (int64_t)dw->first + count;
        if (quads || qstrip) {   // non-indexed quads: indices 0,1,2 0,2,3 per quad from first
            uint32_t nq = quads ? count / 4 : (count >= 4 ? (count - 2) / 2 : 0); icount = nq * 6;
            uint32_t *o = (uint32_t *)ar_alloc((VkDeviceSize)icount * 4, &ib, &ioff); if (!o) return 0; i32 = 1;
            for (uint32_t q = 0; q < nq; q++) { uint32_t b0 = (uint32_t)dw->first + (quads ? 4 * q : 2 * q); uint32_t t6[6] = {b0, b0 + 1, b0 + 2, b0, b0 + 2, b0 + 3}; if (qstrip) { t6[2] = b0 + 3; t6[4] = b0 + 3; t6[5] = b0 + 2; } memcpy(o + 6 * q, t6, sizeof t6); }
        }
    }
    if (prim == 2 || prim == 9) RED_ONCE(99, "line loop / polygon drawn as a strip / fan");
    int64_t lo = k.sh[0]->vid || vlo < 0 ? 0 : vlo;   // the bindings start at vertex lo, and the draw counts from there; a shader reading gl_VertexIndex sees its own numbers
    if (!bind_vbufs(c, k.sh[0], lo, vhi, base_inst, inst)) { g_drop[6]++; return 0; }
    if (ib) { bf_ibuf(c, ib, ioff, i32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16); vkCmdDrawIndexed(c, icount, inst, 0, (int32_t)((di ? di->base_vertex : 0) - lo), base_inst - inst_shift(base_inst)); }
    else vkCmdDraw(c, count, inst, (uint32_t)(dw->first - lo), base_inst - inst_shift(base_inst));   // inst_shift: the base instance in the streams' offsets instead
    if (!rd && (pp->lbpc & VK_SHADER_STAGE_VERTEX_BIT) && k.sh[0]->lbase && !k.sh[2] && !k.sh[3] && P.layered && P.layers > (uint32_t)g_vs_layers)   // the layers past the vertex shader's reach
        vs_layer_groups(c, pp, ib ? icount : count, inst, ib ? (int32_t)((di ? di->base_vertex : 0) - lo) : (int32_t)(dw->first - lo), base_inst - inst_shift(base_inst), ib != VK_NULL_HANDLE);
    g_draws++; C->ndraw++; crumb((dw ? dw->ind : di->ind) ? 'D' : 'd', k.sh[0]->key, k.sh[1] ? k.sh[1]->key : NULL, ib ? icount : count, inst);
    if (g_prof_cur >= 0) { prof_rec *pr = &g_prof[g_prof_cur]; uint64_t nv = (uint64_t)(ib ? icount : count) * inst; pr->draws++; pr->verts += nv;
        if (nv > pr->big) { pr->big = nv; pr->bigvs = k.sh[0]->key; pr->bigfs = k.sh[1] ? k.sh[1]->key : NULL; } }
    if (dump_on()) {
        char tg[160] = ""; for (int i = 0; i < 8; i++) if (P.cv[i]) { size_t l = strlen(tg); snprintf(tg + l, sizeof tg - l, " a%d=s%llu(%ux%u vk%d)", i, (unsigned long long)P.cv[i]->serial, P.cv[i]->w, P.cv[i]->h, P.cv[i]->fmt); }
        // the depth-stencil attachment's format and the depth and stencil tests too: a draw that writes nothing may be failing one (k.ds: test, write, function)
        LOG("[dump] pass %d draw %llu: vs %s fs %s prim %d count %u inst %u%s vp %.0f,%.0f %.0fx%.0f sc %d,%d %ux%u ll %d targets%s depth vk%d; blend %#x [%u %u %u / %u %u %u] mask %#x cull %u front %u; "
            "depth test %u write %u func %u, stencil test %u func %u ref %#x mask %#x", g_dump_pass + 1, (unsigned long long)g_draws,
            k.sh[0]->key, k.sh[1] ? k.sh[1]->key : "-", prim, count, inst, di ? " indexed" : "", v.x, v.y, v.width, v.height, sc.offset.x, sc.offset.y, sc.extent.width, sc.extent.height, g_origin_ll, tg,
            (int)P.key.depth, k.color.blend_en, (unsigned)k.blend[0].src_rgb, (unsigned)k.blend[0].dst_rgb, (unsigned)k.blend[0].eq_rgb, (unsigned)k.blend[0].src_a, (unsigned)k.blend[0].dst_a,
            (unsigned)k.blend[0].eq_a, k.chmask.mask, (unsigned)k.poly.cull, (unsigned)k.poly.front, (unsigned)k.ds.dt, (unsigned)k.ds.dw, (unsigned)k.ds.df, (unsigned)k.ds.st, (unsigned)k.ds.sf_f,
            D.st_ref[0], D.st_vmask[0]);
    }
    if (D.tf_on && k.sh[0]->modtf) { g_drawing = 0; return tf_capture(k.sh[0], pprog[0], pst[0], dw, di); }   // drawn and captured (nvn_mtl.m draw)
    return 1;
}

// ---------------- Compute ----------------
typedef struct { VkPipeline p; VkPipelineLayout pl; } cpipe_t;
static nvn_hmap g_cpipes = {.klen = 8};
static uint64_t g_dispatches;
static cpipe_t *cpipe_make(vshader *s, cpipe_t *cp, lmod **mod);
static cpipe_t *cpipe_get(vshader *s) {
    cpipe_t *cp = nvn_hm_get(&g_cpipes, &s); if (cp) return cp->p ? cp : NULL;
    cp = calloc(1, sizeof *cp); nvn_hm_put(&g_cpipes, &s, cp);
    return cpipe_make(s, cp, &s->mod);
}
// The compute pipeline whose storage images at the bindings of mask are E5B9G9R9 textures bound through R32_UINT views (stage_set's g_e5_need): its module packs RGB9E5 there
// (spv_e5.h). Same layout as cpipe_get's, so the set stage_set wrote binds to it. Kept per shader for two sets of bindings (Ocarina of Time's a335c1db6270c355 always binds
// one), so a shader alternating between them makes no pipeline again; a third set is refused, its dispatches skipped with a red line. NULL: it cannot be (red in lm_code)
static cpipe_t *cpipe_e5(vshader *s, uint64_t mask) {
    for (int i = 0; i < 2; i++) if (s->cpe5[i] && s->e5[i] == mask) { cpipe_t *cp = s->cpe5[i]; return cp->p ? cp : NULL; }
    int slot = !s->cpe5[0] ? 0 : !s->cpe5[1] ? 1 : -1;
    if (slot < 0) { RED_ONCE(127, "compute shader %s binds E5B9G9R9 textures at a third set of image bindings (%#llx): its pipelines are kept for two, so this dispatch is skipped", s->key, (unsigned long long)mask); return NULL; }
    char p[700]; snprintf(p, sizeof p, "%s/%s.spv", g_spv_dir, s->key);
    lmod *m = lm_new_e(p, 0, 0, mask, s->bmap, NULL);
    cpipe_t *cp = calloc(1, sizeof *cp); if (!cp) return NULL;
    s->cpe5[slot] = cp; s->e5[slot] = mask; if (!m) return NULL;   // kept with no pipeline: not tried again
    return cpipe_make(s, cp, &m);
}
static cpipe_t *cpipe_make(vshader *s, cpipe_t *cp, lmod **mod) {
    if (!g_empty_dsl) { VkDescriptorSetLayoutCreateInfo ec = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; vkCreateDescriptorSetLayout(g_vk.dev, &ec, NULL, &g_empty_dsl); }
    VkDescriptorSetLayout sl[SPV_NOPTR_SET + 1] = {s->dsl, g_empty_dsl, g_empty_dsl, g_empty_dsl, s->pool ? g_pool_dsl : g_empty_dsl, g_empty_dsl};   // the pool set after empty ones (gfx_vk_pool.inc)
    uint32_t nsl = s->pool ? POOL_SET + 1 : 1;
    if (s->gm && g_gm_np && !gm_np_layout(sl, &nsl, g_empty_dsl)) return NULL;   // raw memory through the pools' descriptor array (gfx_vk_gmem_noptr.inc)
    VkPipelineLayoutCreateInfo lc = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = nsl, .pSetLayouts = sl};
    if (vkCreatePipelineLayout(g_vk.dev, &lc, NULL, &cp->pl) != VK_SUCCESS) return NULL;
    VkComputePipelineCreateInfo pc = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .layout = cp->pl,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .pName = "main"}};   // its module: lm_pipeline
    const char *pbk[4] = {s->key, NULL, NULL, NULL}; int pb = pb_begin('c', pbk, 0, 0, 0, 0);
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_MONOTONIC); VkResult pr = lm_pipeline(NULL, &pc, &pc.stage, mod, 1, &cp->p);
    pb_end(pb);
    atomic_fetch_add(&g_cpso_ns, clock_gettime_nsec_np(CLOCK_MONOTONIC) - t0); atomic_fetch_add(&g_cpso_made, 1);
    if (pr != VK_SUCCESS) { static int n; if (n++ < 16) LOG("[RED] [vk] compute pipeline of shader %s failed", s->key); vk_start_pipe_failed(s->key, NULL); cp->p = VK_NULL_HANDLE; return NULL; }
    if (vk_debug()) LOG("[vk] compute pipeline for shader %s", s->key);
    return cp;
}
// nn::ndve's movie network (Movie/Ndve/Ndve-Nvn2-vid_seq5.dat, a model any game linking the library may ship; nvn_ndve.inc runs it with host kernels on the Mac) is left
// out on Vulkan: every one of the file's 22 compute shaders (its NDVE network and its NDSR upscaler, shader_map.tsv), of which a movie frame dispatches 14. Its U-Net
// convolutions translate to SPIR-V modules of up to 2.2 MB (e6be82d835fbe4d3: 139,000 lines of emulated HMMA), and Qualcomm's compiler (Adreno 840, driver 0x8034a013)
// was still building that one minutes later: the replay thread waited in it and Ocarina of Time's title never came. With only the U-Net left out, the colour conversion
// before it and the last convolution and NDSR upscaler after it still ran, for a movie picture that stayed black: 37 ms of the Adreno 840's 38 ms GPU frame (30 fps), and
// 1.2 s of every frame on an Adreno 740 (driver 0x802a4000, 0.8 fps from the title on: the upscaler 4f41ccf24f163e0d 790 ms, the last convolution 5a7d69b067a7f011
// 432 ms, both straight-line emulated HMMA with thousands of local variables and no loops). Banana Native leaves out the same 22. DM_VK_NDVE=1 (env.txt) runs them all
static int ndve_left_out(const char *key) {
    static const char k[][17] = {"c2fc2ddd604c25e7", "4f41ccf24f163e0d",   // NDSR
        "ba079f68ae288dba", "a8b4fb48fee0ccf8", "e2eebd7ba18fe35f", "e6be82d835fbe4d3", "9442573ee4ace011", "0a56002cdc7e1fce", "d15370a382630d4e", "7f7ff0f36b54d90f",
        "5a7d69b067a7f011", "329e4b0f0b3f2bc6", "7653fe5f6ef0b2f2", "b6e34b46dea44278", "3f18f89630c05813", "539ad3b353758de0", "d72077c4b6cc264f", "ab9a92b55950ec02",
        "c41f451b88a050f6", "55ee6dc89a2b64e2", "49dff010b9832251", "8e98d86c0e4a7b53"};   // NDVE
    static int on = -1; if (on < 0) { const char *e = getenv("DM_VK_NDVE"); on = e && *e == '1'; }
    if (on) return 0;
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) if (!memcmp(key, k[i], 16)) return 1;
    return 0;
}
static uint64_t g_ndve_skipped;
static int vk_dispatch(const p_dispatch *dp) {
    if (!g_d_init) draw_state_reset();
    rp_end(); tc_clear();
    const prog_t *pr = D.cprog; if (!pr) { RED_ONCE(106, "dispatch with no compute program bound"); return 0; }
    vshader *s = NULL; int pst = -1;
    for (unsigned i = 0; i < pr->nstage && i < 6 && !s; i++) { uint8_t key[8]; char hx[17]; if (!prog_key(pr, i, key)) continue; key_hex(key, hx); vshader *x = sh_get(hx); if (x && x->stage == SH_CS) { s = x; pst = (int)i; } }
    if (!s) { RED_ONCE(107, "compute program without a translated compute shader"); return 0; }
    if (ndve_left_out(s->key)) { if (!g_ndve_skipped++) LOG("[vk] nn::ndve movie network: its dispatches are left out (its picture stayed black without the U-Net, which Qualcomm's compiler does not finish, and the rest cost up to 1.2 s a frame; DM_VK_NDVE=1 runs them): first %s", s->key); return 1; }
    uint32_t g[3] = {(uint32_t)dp->x, (uint32_t)dp->y, (uint32_t)dp->z};
    VkBuffer ib = VK_NULL_HANDLE; VkDeviceSize io = 0; int gpu_args = 0;   // the arguments read on the GPU from a pending write of them (ind_fwd_on)
    g_rd_cs = 1;
    if (dp->ind && ind_fwd_on() && !s->tgs && !s->upsq && cur()) { g_fwd_nwb = -1; gpu_args = fwd_find((uintptr_t)dp->ind, (uintptr_t)dp->ind + 12, &ib, &io) && !(io % 4);
        // no pending write-back holds them (the writer stored them through the raw pool table, or the game wrote them): in a placed pool they are read where they lie, which a
        // raw store or the CPU wrote and the dispatch's barrier orders (gfx_vk_gmem_inplace.inc); on the Galaxy S25 Edge 18,600 of 25,600 indirect dispatches a run took the CPU path
        VkDeviceSize rng_; if (!gpu_args && gm_inplace_al(dp->ind, 12, 4, &ib, &io, &rng_)) { gpu_args = 2; g_ind_inpl++; } }
    { vshader *sh[4] = {s, NULL, NULL, NULL}; rd_rng r[1] = {{(uintptr_t)dp->ind, (uintptr_t)dp->ind + 12}};   // what the dispatch reads of guest memory lands first (draw_view), not everything pending
      if (dp->ind && !gpu_args) { uint64_t w0 = g_view_cur + g_view_fly; ranges_view(r, 1); g_ind_cpu++; g_ind_cpu_wait += g_view_cur + g_view_fly != w0; } draw_view(sh, NULL, 0); }
    if (dp->ind && !gpu_args && !ind_read(dp->ind, g, 12)) return 0;
    if (!gpu_args && (!g[0] || !g[1] || !g[2])) return 1;
    if (gpu_args) { g[0] = 0xFFFFFFFFu; g[1] = (uint32_t)++g_ind_seq; g[2] = 0; g_ind_gpu++; }   // a c0 bank of its own (ucopy shares equal banks): the GPU writes the counts into it below
    cpipe_t *cp = cpipe_get(s); if (!cp) return 0;
    stage_textures(s);
    if (s->pool) pool_sync();
    memcpy(g_cdim, g, sizeof g_cdim);
    VkDescriptorSet set; int ok; uint32_t dyn[18];
    for (int t = 0;; t++) {   // t 1: the set taken again from the chunk recorded now when a sync inside stage_set closed the one it came from (ds_moved)
        g_upsq_src = g_upsq_dst = NULL; g_upsq_src_iv = VK_NULL_HANDLE;
        g_bind_bad = 0; g_e5_need = 0; if (!cur()) return 0; uint64_t dseq = C->seq; int got = stage_set(s, 0, &set, dyn, pr, pst, 0, dseq); ok = got && !g_bind_bad;
        if (got || !ds_moved(dseq)) break;
        if (ds_again(t, 2)) return 0;
    }
    if (!ok) return 0;
    if (s->upsq && upsq_run(s)) { g_dispatches++; C->ndraw++; return 1; }   // a game's upscaler network replaced (gfx_vk_upsq.inc)
    if (g_e5_need) { cpipe_t *ce = cpipe_e5(s, g_e5_need); if (!ce) return 0; cp = ce; }   // E5B9G9R9 storage images: the pipeline that packs RGB9E5 into their R32_UINT views
    g_cs_in_disp = 1; VkCommandBuffer c = cb(); g_cs_in_disp = 0; if (!c) return 0;
    int chain = cs_game() && g_cs_chain && C->seq == g_cs_chain_seq && !gpu_args;   // the command before this one in the command buffer is a dispatch with its barrier deferred (gfx_vk.c cs_chain_close); the copy of the counts below reads the writer's output: a barrier first
    if (!chain) { cb_barrier(); g_cs_first++; g_cs_chain = 0; } else if (g_cs_gbar) { cb_barrier(); g_cs_gbarred++; g_cs_chain = 0; } else g_cs_free++;   // a run left open stays open: a return before the dispatch below leaves its barrier to the next
    g_cs_gbar = 0;
    if (gpu_args) { VkBufferCopy cpy[2] = {{io, g_c0_off + DMSH_C0_DISPATCH, 12}, {io, g_c0_off + DMSH_C0_DISPATCH_NVN2, 12}};   // the group counts into the bank, where the shader reads them (bank_c0)
        vkCmdCopyBuffer(c, ib, g_c0_b, dm_sdk_ounce() ? 2u : 1u, cpy); cb_barrier(); }
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, cp->p);
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, cp->pl, 0, 1, &set, s->ndyn, dyn);
    if (s->pool && !pool_bind(c, VK_PIPELINE_BIND_POINT_COMPUTE, cp->pl)) return 0;
    if (s->gm && g_gm_np) gm_np_bind(c, VK_PIPELINE_BIND_POINT_COMPUTE, cp->pl);
    int pr_i = -1;
    if (prof_on() && C->pq && C->npq + 2 <= 512 && g_nprof < 1024) { prof_rec *r = &g_prof[pr_i = g_nprof++]; memset(r, 0, sizeof *r); r->c = C; r->ncolor = -1; r->w = g[0]; r->h = g[1]; r->draws = (int)g[2]; r->seq = g_prof_seq++; r->bigfs = s->key;
        r->s0 = C->npq++; vkCmdWriteTimestamp(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, C->pq, (uint32_t)r->s0); }
    uint32_t bcid = bc_on() ? bc_begin(c, s->key, g, C->seq) : 0;   // DM_VK_BC (gfx_vk_bc.inc)
    if (gpu_args) vkCmdDispatchIndirect(c, ib, io); else vkCmdDispatch(c, g[0], g[1], g[2]);
    crumb(dp->ind ? 'C' : 'c', s->key, NULL, g[0], g[1] * g[2]);
    bc_end(c, bcid);
    if (pr_i >= 0) { g_prof[pr_i].s1 = C->npq++; vkCmdWriteTimestamp(c, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, C->pq, (uint32_t)g_prof[pr_i].s1); }
    if (cs_game()) { g_cs_chain = 1; g_cs_chain_seq = C->seq; } else cb_barrier();   // the barrier after it waits for the next command of another kind, or the game's barrier
    g_dispatches++; C->ndraw++;
    return 1;
}

// ---------------- Clears inside a render pass ----------------
// A clear limited by the scissor is vkCmdClearAttachments on the bound targets' pass. One with a channel mask, or a partial stencil mask, is a draw: a full-target
// triangle (shaders/clear.vert, clear.frag; glslc --target-env=vulkan1.1 -mfmt=num into shaders/clear_*.inc) whose pipeline writes only those channels / bits
static const uint32_t CLEAR_VS[] = {
#include "shaders/clear_vert.inc"
}, CLEAR_FS[] = {
#include "shaders/clear_frag.inc"
};
static VkRect2D clear_rect(void) {   // the scissor on the pass's targets (window rows are memory rows in both origin modes, as for draws)
    VkRect2D sc = {{0, 0}, {P.w, P.h}};
    if (R.has_sc) { int x0 = R.sc.x < 0 ? 0 : R.sc.x, y0 = R.sc.y < 0 ? 0 : R.sc.y; int64_t x1 = (int64_t)R.sc.x + R.sc.w, y1 = (int64_t)R.sc.y + R.sc.h;
        if (x1 > (int64_t)P.w) x1 = P.w; if (y1 > (int64_t)P.h) y1 = P.h;
        sc = (VkRect2D){{x0, y0}, {(uint32_t)(x1 > x0 ? x1 - x0 : 0), (uint32_t)(y1 > y0 ? y1 - y0 : 0)}}; }
    return sc;
}
typedef struct { VkRenderPass rp; uint32_t ncolor, idx, mask, samples, stencil_mask, depth; } cpk_t;
static nvn_hmap g_clr_pipes = {.klen = sizeof(cpk_t)};
static VkPipelineLayout g_clr_pl; static VkShaderModule g_clr_vs, g_clr_fs;
static VkPipeline clear_pipe(const cpk_t *k) {
    VkPipeline p = nvn_hm_get(&g_clr_pipes, k); if (p) return p;
    if (!g_clr_pl) {
        VkPushConstantRange pr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 20};
        VkPipelineLayoutCreateInfo lc = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
        VkShaderModuleCreateInfo v = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof CLEAR_VS, .pCode = CLEAR_VS};
        VkShaderModuleCreateInfo f = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof CLEAR_FS, .pCode = CLEAR_FS};
        if (vkCreatePipelineLayout(g_vk.dev, &lc, NULL, &g_clr_pl) != VK_SUCCESS || vkCreateShaderModule(g_vk.dev, &v, NULL, &g_clr_vs) != VK_SUCCESS || vkCreateShaderModule(g_vk.dev, &f, NULL, &g_clr_fs) != VK_SUCCESS) return VK_NULL_HANDLE;
    }
    VkPipelineShaderStageCreateInfo st[2] = {{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = g_clr_vs, .pName = "main"},
                                             {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = g_clr_fs, .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = (VkSampleCountFlagBits)(k->samples ? k->samples : 1)};
    VkStencilOpState so = {VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE, VK_COMPARE_OP_ALWAYS, 0xff, k->stencil_mask, 0};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, .depthTestEnable = k->depth, .depthWriteEnable = k->depth, .depthCompareOp = VK_COMPARE_OP_ALWAYS,
        .stencilTestEnable = k->stencil_mask != 0, .front = so, .back = so, .maxDepthBounds = 1};
    VkPipelineColorBlendAttachmentState cba[8]; memset(cba, 0, sizeof cba);
    for (uint32_t i = 0; i < k->ncolor && i < 8; i++) cba[i].colorWriteMask = i == k->idx ? (VkColorComponentFlags)(k->mask & 0xf) : 0;
    VkPipelineColorBlendStateCreateInfo cb_ = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = k->ncolor, .pAttachments = cba};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 3, .pDynamicStates = dyn};
    VkGraphicsPipelineCreateInfo pc = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2, .pStages = st, .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vp, .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb_, .pDynamicState = &dy, .layout = g_clr_pl, .renderPass = k->rp};
    if (vkCreateGraphicsPipelines(g_vk.dev, pcache(), 1, &pc, NULL, &p) != VK_SUCCESS) { RED_ONCE(108, "clear pipeline creation failed"); return VK_NULL_HANDLE; }
    nvn_hm_put(&g_clr_pipes, k, p);
    return p;
}
static int is_int_fmt(VkFormat f) {
    VkClearColorValue t = clear_value(f, (const float[4]){1.5f, 0, 0, 0}); return t.uint32[0] == 1;   // clear_value converts only for integer formats
}
// The pass a clear of colour slot `slot` (-1: the depth target) is made in: a layered one when the target's view covers more than one layer or slice, as a layered draw
// gets, so the clear reaches all of them in either order, clear then draw or draw then clear (as nvn_mtl.m clears a view of more layers than its pass in a pass of its
// own: rendering.md, "Clears of layered render targets in a pass that is not layered"). In a pass of one layer, a clear before the first layered draw cleared slice 0
// alone, and that draw reopened the pass layered and loaded the other slices as they were. full: the clear writes every channel / stencil bit, so it can be a rect of every
// layer (clear_rect_layers); a masked clear is a draw of layer 0 whatever the pass, so it asks for no layered pass, which would only end the open one. → *nl: the layers
// the view covers (1: one), 1 for a masked clear
static int clear_rp_prepare(int slot, int full, uint32_t *nl) {
    vtex *v = NULL; const view_t *vw = NULL; *nl = 1;
    if (slot >= 0 && slot < R.rt.n && R.rt.color[slot]) { v = vt_get_w(R.rt.color[slot]); vw = R.rt.has_cview[slot] ? &R.rt.cview[slot] : NULL; }
    else if (slot < 0 && R.rt.depth) { v = vt_get_w(R.rt.depth); vw = R.rt.has_dview ? &R.rt.dview : NULL; }
    if (v && full) { uint32_t lv = vw && vw->set_levels ? vw->base_level : 0, ly = vw && vw->set_layers ? vw->min_layer : 0; if (lv < v->levels) *nl = rt_layers(v, vw, lv, ly); }
    return rp_prepare_(*nl > 1);
}
// A full clear in the bound targets' pass when its target is the pass's size and the pass has every layer its view covers; else (DM_VK_LAYERED=0, or a pass whose other
// targets have fewer layers) the clear is made outside the pass, on the whole view (gfx_vk.c clear_color, clear_ds)
static int clear_in_pass(const vtex *v, int slot, unsigned level, int depth) {
    uint32_t nl; if (!clear_rp_prepare(slot, 1, &nl) || (depth && !P.key.depth) || (nl > 1 && P.layers < nl)) return 0;
    return (v->w >> level ? v->w >> level : 1) == P.w && (v->h >> level ? v->h >> level : 1) == P.h;
}
// A clear in the pass covers every layer of its framebuffer (clear_rp_prepare above makes it layered for a view of several): NVN clears the whole bound target, and in a layered pass (a 3D or array target attached whole for a draw
// that writes the layer, rp_prepare) a rect of layer 0 alone left the other slices with what earlier frames drew. A full clear of a 3D target is in the pass whenever its
// size matches (its one array layer: gfx_vk.c clear_color), so a volume drawn slice by slice and cleared each frame was cleared at slice 0 only: Final Fantasy Resonance
// adds its lights into 32x32x32 volumes with additive blending, and in shirai91/nx2android's replay the fog boxes that read them changed every frame on its first road
// (37c76f613 there). The masked clears below are a draw of layer 0, which a layered pass cannot widen without a layer-writing shader: a red line says when one meets it
static VkClearRect clear_rect_layers(VkRect2D r) { return (VkClearRect){r, 0, P.layers ? P.layers : 1}; }
// A colour clear of target idx limited by the scissor and / or a channel mask (gfx_vk.c clear_color takes the full ones)
static void clear_color_pass(const p_clear_color *cc) {
    bf_reset();   // its pipeline and dynamic state are set behind the bind filter (bf_*)
    uint32_t nl; if (!clear_rp_prepare(cc->index, (cc->mask & 0xf) == 0xf, &nl) || !rp_start()) return;
    VkRect2D r = clear_rect(); if (!r.extent.width || !r.extent.height) return;
    int ci = dup_slot(cc->index); VkFormat f = P.key.color[ci];
    if (nl > 1 && P.layers < nl) RED_ONCE(172, "clear of colour slot %d in a pass of %u layers, its view %u: the layers past the pass are not cleared", cc->index, P.layers, nl);
    if ((cc->mask & 0xf) == 0xf) {
        VkClearAttachment at = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)ci, {.color = clear_value(f, cc->rgba)}};
        VkClearRect cr = clear_rect_layers(r); vkCmdClearAttachments(C->cb, 1, &at, 1, &cr); return;
    }
    if (is_int_fmt(f)) { RED_ONCE(109, "masked clear of an integer target (format %d): not done", f); return; }
    if (P.layers > 1) RED_ONCE(171, "masked clear in a layered pass of %u layers: layer 0 alone is cleared", P.layers);
    cpk_t k; memset(&k, 0, sizeof k); k.rp = P.rp; k.ncolor = P.key.ncolor; k.idx = (uint32_t)ci; k.mask = (uint32_t)cc->mask & 0xf; k.samples = P.key.samples;
    VkPipeline p = clear_pipe(&k); if (!p) return;
    float pc[5] = {cc->rgba[0], cc->rgba[1], cc->rgba[2], cc->rgba[3], 0};
    VkViewport vp = {0, 0, (float)P.w, (float)P.h, 0, 1};
    vkCmdBindPipeline(C->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p); vkCmdSetViewport(C->cb, 0, 1, &vp); vkCmdSetScissor(C->cb, 0, 1, &r);
    vkCmdPushConstants(C->cb, g_clr_pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc); vkCmdDraw(C->cb, 3, 1, 0, 0);
}
// A depth-stencil clear limited by the scissor and / or a partial stencil mask
static void clear_ds_pass(const p_clear_ds *cd, VkImageAspectFlags a) {
    bf_reset();   // as clear_color_pass
    uint32_t nl; if (!clear_rp_prepare(-1, !(a & VK_IMAGE_ASPECT_STENCIL_BIT) || (cd->stencil_mask & 0xff) == 0xff, &nl) || !rp_start() || !P.key.depth) return;
    if (nl > 1 && P.layers < nl) RED_ONCE(172, "depth-stencil clear in a pass of %u layers, its view %u: the layers past the pass are not cleared", P.layers, nl);
    VkRect2D r = clear_rect(); if (!r.extent.width || !r.extent.height) return;
    uint32_t sm = (uint32_t)cd->stencil_mask & 0xff;
    if (!(a & VK_IMAGE_ASPECT_STENCIL_BIT) || sm == 0xff) {
        VkClearAttachment at = {a, 0, {.depthStencil = {cd->depth, (uint32_t)cd->stencil}}};
        VkClearRect cr = clear_rect_layers(r); vkCmdClearAttachments(C->cb, 1, &at, 1, &cr); return;
    }
    if (P.layers > 1) RED_ONCE(171, "masked clear in a layered pass of %u layers: layer 0 alone is cleared", P.layers);
    cpk_t k; memset(&k, 0, sizeof k); k.rp = P.rp; k.ncolor = P.key.ncolor; k.idx = 99; k.samples = P.key.samples; k.stencil_mask = sm; k.depth = (a & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
    VkPipeline p = clear_pipe(&k); if (!p) return;
    float pc[5] = {0, 0, 0, 0, cd->depth};
    VkViewport vp = {0, 0, (float)P.w, (float)P.h, 0, 1};
    vkCmdBindPipeline(C->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p); vkCmdSetViewport(C->cb, 0, 1, &vp); vkCmdSetScissor(C->cb, 0, 1, &r);
    vkCmdSetStencilReference(C->cb, VK_STENCIL_FACE_FRONT_AND_BACK, (uint32_t)cd->stencil);
    vkCmdPushConstants(C->cb, g_clr_pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc); vkCmdDraw(C->cb, 3, 1, 0, 0);
}
