// ---------------- Shader modules made when a pipeline needs them (VK_EXT_shader_module_identifier) ----------------
// Samsung's Xclipse 950 driver does a compile's work in vkCreateShaderModule: 0.8-0.9 ms a module against about 0.1 on the Adreno 830, and its pipeline cache does
// not keep that work. Brilliant Diamond's 8,844 modules took 6-10 s of every start on a Galaxy Z Flip 7 (its first frame at 10-12 s, 3-4 on the Pad 3), while its
// 2,982 pipelines came out of the cache in 0.2-0.4 s. With VK_EXT_shader_module_identifier (and pipeline creation cache control) a module is only its identifier,
// a hash of its code (0.005 ms there), until a pipeline needs it: a pipeline is asked for from the identifiers first, with
// VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT, which the driver's cache serves (2 ms) or answers VK_PIPELINE_COMPILE_REQUIRED (0.02 ms). Only then are
// its modules made, from the files again through the same rewrites, and the pipeline compiled as before. The SPIR-V is not kept meanwhile (Brilliant Diamond's
// modules are about 28 KB each). Without the extension, or with DM_VK_MODULE_ID=0 (env.txt), every module is made at once, as before
typedef struct { uint32_t set; int n; uint16_t b[SPV_BILIN_MAX]; } lm_bl;   // a fragment shader's samples of these bindings filtered by the shader (spv_bilin.h; n 0: none)
typedef struct lmod { _Atomic(VkShaderModule) m; uint32_t miss, idn; int alpha, tff; uint64_t e5; lm_bl bl; const spv_bmap *bm; uint8_t id[VK_MAX_SHADER_MODULE_IDENTIFIER_SIZE_EXT]; char path[]; } lmod;   // miss: ps_variant's (0: the file as it is); alpha: spv_alpha.h's test added to the code; tff: 1 the vertex shader wrapped for the fragment-stage capture (spv_tffrag.h), 2 the vertex shader made a fragment shader over points (spv_vsfrag.h), 3 the vertex shader without its memory writes (spv_nostore.h), 4 both 1 and 3; bl: spv_bilin's bindings (gfx_vk_bilin.inc ps_bl_mod)
static _Atomic uint64_t g_lm_ids, g_lm_hit, g_lm_miss;   // modules known by their identifier; pipelines the driver's cache gave for identifiers; pipelines that needed modules after all
static __thread int t_pool_info;   // what spv_pool did to the modules lm_code made since sh_get cleared it
static __thread int t_harr_info;   // what spv_harr did to them (SPV_HARR_SHRUNK, SPV_HARR_FAIL), the same way
// The module's code as it is made (to free): the file, a vertex or fragment shader's attribute arrays split (spv_inarr); a fragment shader variant's unwritten inputs made constants (spv_drop_inputs), else the vertex pipeline's
// storage made read-only where the GPU needs it (spv_readonly); then physical pointers through the pools' array where the driver refuses them (gm_np_code), the pool set (spv_pool_fit), arrays longer than stage_set writes (spv_fit_harr), a kernel's warp as its workgroup (spv_fit_wg32), the subgroup rewrites (spv_fit) and, last, the set's binding numbers compacted (spv_bind.h). first: the module's first
// making, which tells *b0, logs and dumps (made again for a pipeline, it says nothing new)
static uint32_t *lm_code(const char *path, uint32_t miss, int alpha, int tff, uint64_t e5, const lm_bl *bl, const spv_bmap *bm, int first, int *b0, size_t *onw) {
    size_t n = 0; uint32_t *code = spv_read(path, &n); if (!code) return NULL;
    uint32_t *ia = NULL; size_t ian = spv_fit_inarr(code, n, path, &ia); if (ian) { free(code); code = ia; n = ian; }   // first: a shader's attribute arrays become an input per location, which spv_drop_inputs can then drop one by one
    if (tff == 2) {   // the vertex shader as a fragment shader (spv_vsfrag.h), after the attribute split and before the rewrites below, so the rewrites below see a fragment shader: spv_readonly, spv_fit_vsinit and spv_fit_precise leave it alone
        size_t cap = n + 3 * 32 + 64, k = 0; uint32_t *o = malloc(cap * 4), *s = malloc(2 * (size_t)code[3] * 4); spv_vsf_io io;
        if (o && s) k = spv_vsf_fs(code, n, s, o, cap, &io);
        free(s); free(code);
        if (!k) { free(o); if (first) LOG("[RED] [vk] %s: an input that is neither a vertex attribute nor a built-in a fragment shader can be handed, so its memory writes cannot run in the fragment stage", path); return NULL; }
        code = o; n = k;
        const char *d = getenv("DM_DUMP_DIR");   // for spirv-val
        if (first && d) { char q[700]; mkdir(d, 0777); const char *b = strrchr(path, '/'); snprintf(q, sizeof q, "%s/vsf_%.16s.fs.spv", d, b ? b + 1 : path); FILE *f = fopen(q, "wb"); if (f) { fwrite(code, 4, n, f); fclose(f); } }
    }
    if (tff == 1 || tff == 4) {   // first, so the rewrites below see the wrapper as any other code
        size_t cap = n + 400, k = 0; uint32_t *o = malloc(cap * 4);
        if (o) k = spv_tff_vs(code, n, o, cap);
        free(code);
        if (!k) { free(o); if (first) LOG("[RED] [vk] %s: no vec4 position to place, so its transform feedback cannot be captured in the fragment stage", path); return NULL; }
        code = o; n = k;
        const char *d = getenv("DM_DUMP_DIR");   // for spirv-val
        if (first && d) { char q[700]; mkdir(d, 0777); const char *b = strrchr(path, '/'); snprintf(q, sizeof q, "%s/tff_%.16s.vs.spv", d, b ? b + 1 : path); FILE *f = fopen(q, "wb"); if (f) { fwrite(code, 4, n, f); fclose(f); } }
    }
    if (e5) {   // a compute variant whose storage images at these bindings are E5B9G9R9 textures bound through R32_UINT views (spv_e5.h): RGB9E5 packed and unpacked by the shader
        uint32_t *s = malloc(5 * (size_t)code[3] * 4), *o = NULL; size_t need = 0, k = 0; int what = 0;
        if (s) { spv_e5(code, n, e5, s, NULL, 0, &need, &what); if (need && (o = malloc(need * 4))) k = spv_e5(code, n, e5, s, o, need, &need, &what); }
        free(s); free(code);
        if (!k) { free(o); if (first) LOG("[RED] [vk] %s: its storage image at binding mask %#llx is an E5B9G9R9 texture, and spv_e5 cannot rewrite the shader for it (%s): its dispatches are skipped", path, (unsigned long long)e5, what == SPV_E5_KEPT ? "another use of the image" : "no such image"); return NULL; }
        code = o; n = k;
        if (first) LOG("[vk] %s: its storage images at binding mask %#llx are E5B9G9R9 textures: stored through R32_UINT views, RGB9E5 packed by the shader (spv_e5)", path, (unsigned long long)e5);
    }
    if (miss) {
        size_t on = 0; uint32_t *v = spv_drop_inputs(code, n, miss, &on); free(code); if (!v) return NULL; code = v; n = on;
        const char *d = getenv("DM_DUMP_DIR");   // for spirv-val
        if (first && d) { char q[700]; mkdir(d, 0777); const char *b = strrchr(path, '/'); snprintf(q, sizeof q, "%s/var_%.16s_%x.spv", d, b ? b + 1 : path, miss); FILE *f = fopen(q, "wb"); if (f) { fwrite(code, 4, n, f); fclose(f); } }
    } else {
        if (b0) *b0 |= spv_b0_used(code, n);
        if (tff == 3 || tff == 4) {   // its writes made by its fragment form first (gfx_vk_draw.inc vsw_pre): the draw's own module without them, as a GPU without vertex-stage stores takes it
            uint32_t *s = malloc(2 * (size_t)code[3] * 4), *o = malloc(n * 4); size_t on = 0; int dr = 0;
            if (s && o) on = spv_nostore(code, n, s, o, n, &dr);
            if (on) { free(code); code = o; n = on; o = NULL; } else if (first) LOG("[RED] [vk] %s: its memory writes cannot be taken out (an atomic on an image texel or an atomic flag): its rasterized draws keep them", path);
            free(s); free(o);
        }
        if (!g_vk_feat.vertexPipelineStoresAndAtomics) {   // the Mali-G76: the vertex pipeline's storage resources must be NonWritable (spv_readonly.h)
            size_t cap = n + 3 * (size_t)code[3], on = 0; int wr = 0;
            uint32_t *s = malloc(2 * (size_t)code[3] * 4), *o = malloc(cap * 4);
            if (s && o && (on = spv_readonly(code, n, s, o, cap, &wr))) { free(code); code = o; n = on; o = NULL; }
            if (wr && first) LOG("[RED] [vk] %s writes %d storage resource(s) from the vertex pipeline: the GPU has no vertexPipelineStoresAndAtomics, the driver may refuse it", path, wr);
            free(s); free(o);
        }
    }
    uint32_t *nph = NULL; size_t nphn = spv_fit_nophys(code, n, path, &nph); if (nphn) { free(code); code = nph; n = nphn; }   // raw memory off: no physical pointer read (spv_nophys.h)
    uint32_t *gp = NULL; size_t gpn = spv_fit_gmpage(code, n, path, &gp); if (gpn) { free(code); code = gp; n = gpn; }   // raw memory helpers read the page map before they search (spv_gmpage.h)
    uint32_t *gc = NULL; size_t gcn = spv_fit_gmcache(code, n, path, &gc); if (gcn) { free(code); code = gc; n = gcn; }   // raw memory helpers' last entry kept in private words (spv_gmcache.h)
    uint32_t *np = NULL; size_t npn = gm_np_code(code, n, path, first, &np); if (npn) { free(code); code = np; n = npn; }   // raw memory without physical pointers (spv_noptr.h)
    uint32_t *vi = NULL; size_t vn = spv_fit_vsinit(code, n, path, &vi); if (vn) { free(code); code = vi; n = vn; }   // vertex outputs start as (0,0,0,1)
    uint32_t *pf = NULL; size_t pn = spv_pool_fit(code, n, path, &pf); if (pn) { free(code); code = pf; n = pn; }   // the texture pool by ID moved into the pool set
    uint32_t *ha = NULL; size_t hn = spv_fit_harr(code, n, path, &ha); if (hn) { free(code); code = ha; n = hn; }   // arrays past what stage_set writes shortened to it
    uint32_t *wg = NULL; size_t wgn = spv_fit_wg32(code, n, path, &wg); if (wgn) { free(code); code = wg; n = wgn; }   // a task / mesh kernel's warp as its workgroup (spv_wg32.h)
    uint32_t *fit = NULL; size_t fn = spv_fit(code, n, path, &fit); if (fn) { free(code); code = fit; n = fn; }
    uint32_t *fs = NULL; size_t fsn = spv_fit_fsum(code, n, path, &fs); if (fsn) { free(code); code = fs; n = fsn; }   // after the subgroup rewrites: float atomic adds summed across the subgroup (spv_fsum.h)
    t_wg32 = 0;
    uint32_t *ly = NULL; size_t ln = spv_fit_layer(code, n, path, &ly); if (ln) { free(code); code = ly; n = ln; }
    uint32_t *lb = NULL; size_t lbn = tff == 1 || tff == 2 || tff == 4 || alpha ? 0 : spv_fit_lbase(code, n, path, &lb); if (lbn) { free(code); code = lb; n = lbn; }   // the layer less or plus a base (not in the capture's wrapper, nor beside the alpha test's push constant)
    uint32_t *ts = NULL; size_t tn = spv_fit_tgs(code, n, path, &ts); if (tn) { free(code); code = ts; n = tn; }   // shared memory past the GPU's limit (spv_tgspill)
    uint32_t *pr = NULL; size_t prn = spv_fit_precise(code, n, path, &pr); if (prn) { free(code); code = pr; n = prn; }   // FMUL and FADD unfused where vertices are placed
    if (alpha) {   // NVN's alpha test, run in a wrapper entry point (spv_alpha.h); a shader it does not apply to is no module
        size_t cap = n + 256; uint32_t *o = malloc(cap * 4), fn_id = 0;
        size_t on = o ? spv_alpha(code, n, o, cap, &fn_id) : 0;
        free(code);
        if (!on) { free(o); if (first) LOG("[RED] [vk] fragment shader %s: an alpha test is set, but the shader has no float vec4 colour 0 to test (an integer target or no such output): the draw keeps the shader as it is", path); return NULL; }
        code = o; n = on;
    }
    if (bm) spv_bind_remap(code, n, bm);   // last, so every rewrite above saw the numbers the file has (spv_bind.h; sh_get gave the layout the same map)
    if (bl && bl->n) {   // after it: the slots' binding numbers are the layout's (sh_get remapped them); a variant only ps_bl_mod asks for, so none is made without its gathers
        size_t need = 0; int left = 0; spv_bilin(code, n, bl->set, bl->b, bl->n, NULL, 0, &need, &left);
        uint32_t *o = need ? malloc(need * 4) : NULL; size_t k = o ? spv_bilin(code, n, bl->set, bl->b, bl->n, o, need, NULL, NULL) : 0;
        free(code); if (!k) { free(o); return NULL; }
        if (left && first) LOG("[RED] [vk] %s: %d sample(s) of its textures this GPU doesn't filter linearly are left nearest (projective, or an operand spv_bilin doesn't take)", path, left);
        code = o; n = k;
    }
    *onw = n; return code;
}
static VkShaderModule lm_create(const uint32_t *code, size_t nw, const char *what) {
    VkShaderModule m = VK_NULL_HANDLE; VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = nw * 4, .pCode = code};
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_MONOTONIC);
    if (vkCreateShaderModule(g_vk.dev, &ci, NULL, &m) != VK_SUCCESS) { LOG("[RED] [vk] shader module %s refused by the driver", what); m = VK_NULL_HANDLE; }
    atomic_fetch_add(&g_mod_ns, clock_gettime_nsec_np(CLOCK_MONOTONIC) - t0); atomic_fetch_add(&g_mod_made, 1);
    return m;
}
// The module of a .spv file (and a fragment shader's variant): its identifier where the GPU takes identifiers, else made now. NULL: no file, or the driver refused it
static lmod *lm_new_x(const char *path, uint32_t miss, int alpha, int tff, uint64_t e5, const lm_bl *bl, const spv_bmap *bm, int *b0) {
    size_t n = 0; uint32_t *code = lm_code(path, miss, alpha, tff, e5, bl, bm, 1, b0, &n); if (!code) return NULL;
    size_t pl = strlen(path) + 1; lmod *l = calloc(1, sizeof *l + pl); if (!l) { free(code); return NULL; }
    memcpy(l->path, path, pl); l->miss = miss; l->alpha = alpha; l->tff = tff; l->e5 = e5; if (bl) l->bl = *bl; l->bm = bm;
    if (g_smid) {
        VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = n * 4, .pCode = code};
        VkShaderModuleIdentifierEXT id = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_IDENTIFIER_EXT};
        g_smid(g_vk.dev, &ci, &id);
        l->idn = id.identifierSize < sizeof l->id ? id.identifierSize : (uint32_t)sizeof l->id; memcpy(l->id, id.identifier, l->idn);
        if (l->idn) atomic_fetch_add(&g_lm_ids, 1);
    }
    if (!l->idn) { VkShaderModule m = lm_create(code, n, path); if (!m) { free(code); free(l); return NULL; } atomic_store(&l->m, m); }
    free(code); return l;
}
static lmod *lm_new_e(const char *path, uint32_t miss, int alpha, uint64_t e5, const spv_bmap *bm, int *b0) { return lm_new_x(path, miss, alpha, 0, e5, NULL, bm, b0); }
static lmod *lm_new_a(const char *path, uint32_t miss, int alpha, const spv_bmap *bm, int *b0) { return lm_new_e(path, miss, alpha, 0, bm, b0); }
static lmod *lm_new_t(const char *path, const spv_bmap *bm, int *b0) { return lm_new_x(path, 0, 0, 1, 0, NULL, bm, b0); }   // the vertex shader wrapped for the fragment-stage capture
static lmod *lm_new_vsf(const char *path, const spv_bmap *bm, int *b0) { return lm_new_x(path, 0, 0, 2, 0, NULL, bm, b0); }   // the vertex shader made a fragment shader (spv_vsfrag.h)
static lmod *lm_new_nost(const char *path, const spv_bmap *bm, int *b0) { return lm_new_x(path, 0, 0, 3, 0, NULL, bm, b0); }   // the vertex shader without its memory writes (spv_nostore.h)
static lmod *lm_new_tn(const char *path, const spv_bmap *bm, int *b0) { return lm_new_x(path, 0, 0, 4, 0, NULL, bm, b0); }   // the fragment-stage capture's wrapper, without the shader's memory writes
static lmod *lm_new_b(const char *path, uint32_t miss, int alpha, uint32_t set, const uint16_t *b, int nb, const spv_bmap *bm) {   // a fragment shader's samples of bindings b filtered by it (spv_bilin.h)
    lm_bl bl; memset(&bl, 0, sizeof bl); bl.set = set; bl.n = nb < SPV_BILIN_MAX ? nb : SPV_BILIN_MAX; for (int i = 0; i < bl.n; i++) bl.b[i] = b[i];
    return lm_new_x(path, miss, alpha, 0, 0, &bl, bm, NULL); }
// A module whose code exists only in memory (the capture's fragment shader, spv_tffrag.h), made now: no file to make it again from. what names it in logs
static lmod *lm_gen(const uint32_t *code, size_t nw, const char *what) {
    size_t pl = strlen(what) + 1; lmod *l = calloc(1, sizeof *l + pl); if (!l) return NULL;
    memcpy(l->path, what, pl);
    VkShaderModule m = lm_create(code, nw, what); if (!m) { free(l); return NULL; }
    atomic_store(&l->m, m); return l;
}
static lmod *lm_new(const char *path, uint32_t miss, const spv_bmap *bm, int *b0) { return lm_new_a(path, miss, 0, bm, b0); }
static VkShaderModule lm_module(lmod *l) {   // the module itself, made now when it was its identifier only; VK_NULL_HANDLE: the driver refused it
    VkShaderModule m = atomic_load(&l->m); if (m) return m;
    size_t n = 0; uint32_t *code = lm_code(l->path, l->miss, l->alpha, l->tff, l->e5, l->bl.n ? &l->bl : NULL, l->bm, 0, NULL, &n); if (!code) return VK_NULL_HANDLE;
    VkShaderModule made = lm_create(code, n, l->path); free(code); if (!made) return VK_NULL_HANDLE;
    VkShaderModule none = VK_NULL_HANDLE;
    if (!atomic_compare_exchange_strong(&l->m, &none, made)) { vkDestroyShaderModule(g_vk.dev, made, NULL); return none; }   // another thread's pipeline made it meanwhile
    return made;
}
static __thread uint64_t t_lm_big, t_lm_mem;   // the largest pipeline object the driver made in this thread's last lm_pipeline, and all it made (the memory report; gfx_vk_pheavy.inc)
static void lm_pstats(VkPipeline p, lmod *const *lms, uint32_t ns) {   // DM_VK_PIPE_STATS: the pipeline's statistics, named by its shaders' files
    if (!g_ps_on) return;
    char w[200]; size_t n = 0; w[0] = 0;
    for (uint32_t i = 0; i < ns && i < 5 && n < sizeof w; i++) { const char *b = strrchr(lms[i]->path, '/'); n += (size_t)snprintf(w + n, sizeof w - n, "%s%s", i ? " + " : "", b ? b + 1 : lms[i]->path); }
    ps_report(p, w, t_lm_mem, t_lm_big);
}
// A graphics (gp) or compute (cp) pipeline whose ns stages st name the modules lms: from the identifiers of those not made yet when the driver's cache has the
// pipeline, else with the modules made now. Called with or without the replay's lock, as pipe_make and cpipe_get are
static VkResult lm_pipeline(VkGraphicsPipelineCreateInfo *gp, VkComputePipelineCreateInfo *cp, VkPipelineShaderStageCreateInfo *st, lmod *const *lms, uint32_t ns, VkPipeline *out) {
    VkPipelineShaderStageModuleIdentifierCreateInfoEXT sid[5]; int byid = 0;
    for (uint32_t i = 0; i < ns && i < 5; i++) {
        st[i].module = atomic_load(&lms[i]->m);
        if (!st[i].module) { sid[i] = (VkPipelineShaderStageModuleIdentifierCreateInfoEXT){.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_MODULE_IDENTIFIER_CREATE_INFO_EXT,
                                                                                         .identifierSize = lms[i]->idn, .pIdentifier = lms[i]->id};
                             st[i].pNext = &sid[i]; byid = 1; }
    }
    VkPipelineCreateFlags *fl = gp ? &gp->flags : &cp->flags; VkResult pr;
    if (g_ps_on) *fl |= ps_flags();   // DM_VK_PIPE_STATS
    t_mr_pipe = t_mr_pipe_big = 0;   // what the driver makes for it on this thread (the memory report's callback, when asked for)
    if (byid) {
        *fl |= VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_EXT;
        pr = gp ? vkCreateGraphicsPipelines(g_vk.dev, pcache(), 1, gp, NULL, out) : vkCreateComputePipelines(g_vk.dev, pcache(), 1, cp, NULL, out);
        *fl &= ~(VkPipelineCreateFlags)VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_EXT;
        if (pr == VK_SUCCESS) { atomic_fetch_add(&g_lm_hit, 1); t_lm_big = t_mr_pipe_big; t_lm_mem = t_mr_pipe; lm_pstats(*out, lms, ns); return pr; }
        // VK_PIPELINE_COMPILE_REQUIRED, the cache's answer for a pipeline it does not have (any other answer is taken as that too): the modules, then a compile
        atomic_fetch_add(&g_lm_miss, 1); *out = VK_NULL_HANDLE;
        for (uint32_t i = 0; i < ns && i < 5; i++) if (!st[i].module) { st[i].pNext = NULL; if (!(st[i].module = lm_module(lms[i]))) return VK_ERROR_INITIALIZATION_FAILED; }
    }
    pr = gp ? vkCreateGraphicsPipelines(g_vk.dev, pcache(), 1, gp, NULL, out) : vkCreateComputePipelines(g_vk.dev, pcache(), 1, cp, NULL, out);
    t_lm_big = t_mr_pipe_big; t_lm_mem = t_mr_pipe;
    if (pr == VK_SUCCESS) lm_pstats(*out, lms, ns);
    return pr;
}
