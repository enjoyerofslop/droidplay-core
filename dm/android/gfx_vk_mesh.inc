// NVN2's DrawMeshTasks on VK_EXT_mesh_shader (g_mesh, gfx_vk.c vk_create), included in gfx_vk.c after gfx_vk_draw.inc.
// A program's task and mesh shaders are Vulkan task and mesh modules (dm/android/tools/spv_mesh.py: the translated kernel called from a TaskEXT / MeshEXT entry
// point that fills and empties its stage memory). The mesh shader takes the vertex shader's place in the pipeline key (k.sh[0], descriptor set 0) and the task shader
// the geometry shader's (k.sh[2], set 2): a mesh pipeline has neither. Their bindings are the host's stages 3 (task) and 4 (mesh) (hle_nvn.c st_host), as the Metal
// replay encodes them (nvn_geometry.inc mtk_encode). The draw's first workgroup goes in a push constant, which the task shader (or a mesh shader without one) adds to
// its workgroup's number. An indirect draw's {count, first} record (DrawMeshTasksIndirect, written by the game's culling earlier in the submission) is read on the
// CPU once the GPU's writes to it have landed, as the other indirect draws are (vk_draw).
static uint64_t g_mesh_draws, g_mesh_groups, g_mesh_indirect, g_mesh_nopipe;   // drawn, their workgroups, of them indirect; draws whose pipeline was still building (or failed)
static int vk_mesh_draw_(const p_draw *dw);
// DM_VK_MESH_LEAVE_OUT=1 (the launcher's "Fewer plants (faster)", a game setting; env.txt) leaves out the task / mesh draws whose mesh shader is one of the game profile's
// mesh_leave_out keys (DM_GAME_MESH_LEAVE_OUT); =<key>[,<key>...] names keys itself. A draw whose stage writes guest memory (a storage buffer or image) still runs: its compute
// producers, and what reads what it wrote, stay as they are. Ocarina of Time's Kokiri Forest draws its ground cover and tree canopies with seven mesh pairs (a depth prepass
// and a colour mesh each) that cost 1.7 to 3.0 ms of kernels a frame on an Adreno 840 / 830 in compute form, plus their pass pieces and fragment work: a OnePlus Pad 3 goes
// from 29.1 to 30.0 fps (GPU 27.0 to 24.2 ms) without them. The ferns and flowers go and the trees stand bare, so it is off unless the player turns it on. The list and the
// idea are nebadasSwifty's (Banana Native #167, its "Full vegetation" setting), MIT; written anew here
static uint64_t g_mesh_leftout;
static int mesh_left_out(const vshader *ms, const vshader *ts) {
    static int n = -1; static char keys[64][17];
    if (n < 0) { n = 0; const char *e = getenv("DM_VK_MESH_LEAVE_OUT"); if (e && (!strcmp(e, "1") || !strcmp(e, "on"))) e = DM_GAME_MESH_LEAVE_OUT; else if (e && !strcmp(e, "0")) e = NULL;
        while (e && *e && n < 64) { size_t l = strcspn(e, ", "); if (l == 16) { memcpy(keys[n], e, 16); keys[n++][16] = 0; } e += l; while (*e == ',' || *e == ' ') e++; }
        if (n) LOG("[vk] DM_VK_MESH_LEAVE_OUT: task / mesh draws of %d mesh shaders are not drawn (those writing guest memory are)", n); }
    if (!n) return 0;
    int hit = 0; for (int i = 0; i < n && !hit; i++) hit = !memcmp(keys[i], ms->key, 16);
    if (!hit) return 0;
    for (int k = 0; k < 2; k++) { const vshader *s = k ? ts : ms; if (!s) continue; if (s->nim) return 0; for (int i = 0; i < s->nsb; i++) if (s->sb[i].flags & 1) return 0; }
    g_mesh_leftout++; return 1;
}
static int vk_memu_draw_(const p_draw *dw);   // gfx_vk_meshemu.inc: the same draw in compute form, without VK_EXT_mesh_shader
static int vk_mesh_draw(const p_draw *dw) {
    p_draw d2;
    if (dw->ind) {   // {count, first}, read on the CPU once the GPU's writes to it have landed
        rd_rng r[1] = {{(uintptr_t)dw->ind, (uintptr_t)dw->ind + 8}}; ranges_view(r, 1);
        uint32_t a[2]; if (!ind_read(dw->ind, a, 8)) return 0;
        d2 = *dw; d2.ind = 0; d2.count = (int32_t)a[0]; d2.first = (int32_t)a[1]; dw = &d2; g_mesh_indirect++;
    }
    if (dw->count <= 0) return 1;
    int r = g_mesh ? vk_mesh_draw_(dw) : vk_memu_draw_(dw); g_drawing = 0; g_fwd_nwb = -1; g_c1d_on = 0; tc_clear(); return r;
}
static int vk_mesh_draw_(const p_draw *dw) {
    if (!g_d_init) draw_state_reset();
    pipe_key k; memset(&k, 0, sizeof k); int pst[4] = {-1, -1, -1, -1}; const prog_t *pprog[4] = {0};
    const prog_t *src[3] = {D.bst[4], D.bst[1], D.bst[3]}; const int want[3] = {SH_MS, SH_PS, SH_TK};   // the key's slots 0 (mesh), 1 (fragment), 2 (task)
    for (int q = 0; q < 3; q++) { const prog_t *pr = src[q]; if (!pr) continue;
        for (unsigned i = 0; i < pr->nstage && i < 6; i++) {
            uint8_t key[8]; char hx[17]; if (!prog_key(pr, i, key)) continue; key_hex(key, hx);
            vshader *s = sh_get(hx); if (!s) { g_drop[0]++; return 0; }
            if (s->stage != want[q]) continue;
            k.sh[q] = s; pst[q] = (int)i; pprog[q] = pr; break;
        } }
    if (!k.sh[0]) { RED_ONCE(160, "DrawMeshTasks with no mesh shader bound: draw skipped"); return 0; }
    if (mesh_left_out(k.sh[0], k.sh[2])) return 1;
    draw_view(k.sh, NULL, 0);   // what the stages read of guest memory: pending GPU writes to it land first (no vertex streams)
    for (int i = 0; i < 4; i++) if (k.sh[i]) stage_textures(k.sh[i]);
    int pool = (k.sh[0] && k.sh[0]->pool) || (k.sh[1] && k.sh[1]->pool) || (k.sh[2] && k.sh[2]->pool);   // the texture pool by ID (gfx_vk_pool.inc), as vk_draw_
    if (pool) pool_sync();
    if (!rp_prepare_(0)) { g_drop[1]++; return 0; }
    g_drawing = 1;
    pipe_key_build(&k, DM_PRIM_MESH_TASKS);   // the mesh shader reads no vertex attribute (io_in 0): the key has none
    pipe_t *pp = pipe_get(&k); if (!pp) { g_drop[2]++; g_mesh_nopipe++; return 0; }
    VkDescriptorSet sets[4] = {0}; uint32_t dyn[4][18];   // dyn: each stage's dynamic offsets (stage_set, set reuse)
    for (int t = 0;; t++) {   // as vk_draw_: every stage again from the chunk recorded now when a sync inside stage_set closed the one the sets came from
        int i = 0; g_bind_bad = 0;
        if (!cur()) { g_drop[3]++; return 0; }
        uint64_t dseq = C->seq;
        for (; i < 4; i++) if (k.sh[i] && !stage_set(k.sh[i], 0, &sets[i], dyn[i], pprog[i], pst[i], 0, dseq)) break;
        if (i == 4) break;
        if (!ds_moved(dseq) || ds_again(t, 0)) { g_drop[3]++; return 0; }
    }
    if (g_bind_bad) { g_drop[4]++; static int nb; if (nb < 10) { nb++; LOG("[RED] [vk] task / mesh draw skipped: a binding has no valid descriptor (task %s mesh %s fragment %s)", k.sh[2] ? k.sh[2]->key : "-", k.sh[0]->key, k.sh[1] ? k.sh[1]->key : "-"); } return 0; }
    if (!rp_start()) { g_drop[5]++; return 0; }
    VkCommandBuffer c = C->cb;
    occl_begin();
    pp->last = atomic_load(&g_presents); pp->seq = C->seq;   // (gfx_vk_pheavy.inc)
    bf_pipeline(c, pp->p);
    if (pp->lbpc) { int32_t b0 = 0; vkCmdPushConstants(c, pp->pl, pp->lbpc, SPV_LBASE_PC, 4, &b0); }   // the layer base: none, as vk_draw_ (spv_fit_lbase)
    for (int i = 0; i < 4; i++) if (k.sh[i]) vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pp->pl, (uint32_t)(k.sh[i]->set & 3), 1, &sets[i], k.sh[i]->ndyn, dyn[i]);
    if (pool && !pool_bind(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pp->pl)) { g_drop[3]++; return 0; }
    if (g_gm_np) { int gm = 0; for (int i = 0; i < 4; i++) gm |= k.sh[i] && k.sh[i]->gm; if (gm) gm_np_bind(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pp->pl); }   // raw memory's pools as a descriptor array, as vk_draw_ (gfx_vk_gmem_noptr.inc)
    VkViewport v; VkRect2D sc; draw_dyn_state(c, &v, &sc, 0); (void)v; (void)sc;
    // a task shader's workgroups are the draw's; with none, the mesh shader's. Past the device's X limit (65,535 on the Adreno 840) the draw goes in parts, each with its own first
    uint32_t first = (uint32_t)dw->first, left = (uint32_t)dw->count, mx = g_mesh_max[k.sh[2] ? 0 : 1];
    while (left) {
        uint32_t n = left < mx ? left : mx, pc[MESH_PC_BYTES / 4] = {first};
        vkCmdPushConstants(c, pp->pl, VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT, 0, MESH_PC_BYTES, pc);
        rec_draw_mesh(c, n, 1, 1); first += n; left -= n;   // (through the recorder, as the push constant before it: gfx_vk_rec.inc)
    }
    g_draws++; C->ndraw++; g_mesh_draws++; g_mesh_groups += (uint64_t)dw->count;
    if (g_prof_cur >= 0) { prof_rec *pr = &g_prof[g_prof_cur]; pr->draws++; }
    if (g_mesh_draws == 1) LOG("[vk] task / mesh: first draw, task %s mesh %s fragment %s, %d workgroups from %d", k.sh[2] ? k.sh[2]->key : "-", k.sh[0]->key, k.sh[1] ? k.sh[1]->key : "-", dw->count, dw->first);
    if (dump_on()) LOG("[dump] pass %d draw %llu: task %s mesh %s fs %s, %d workgroups from %d; depth test %u write %u func %u", g_dump_pass + 1, (unsigned long long)g_draws,
                       k.sh[2] ? k.sh[2]->key : "-", k.sh[0]->key, k.sh[1] ? k.sh[1]->key : "-", dw->count, dw->first, (unsigned)k.ds.dt, (unsigned)k.ds.dw, (unsigned)k.ds.df);
    return 1;
}
// ---- Fragment shaders reading per-primitive outputs ----
// Vulkan wants a fragment input that a mesh shader writes per primitive decorated PerPrimitiveEXT (Ocarina of Time's HUD mesh 147550d4e76629d5 writes locations 0, 1 and 3
// so), and SPV_EXT_mesh_shader gives that decoration only to a SPIR-V 1.4 module. The variant is the module as 1.4: every global in the entry point's interface (1.4
// lists all, 1.3 only inputs and outputs), the translator's storage buffers (Uniform variables of BufferBlock structs, which 1.4 has no more) as StorageBuffer variables
// of Block structs with the pointers made from them retyped, the capability and extension, and the decorations. spv_mesh.py does the same to the mesh modules.
// NULL if it cannot be done
static uint32_t *spv_perprim(const uint32_t *w, size_t n, uint32_t prim, size_t *on) {
    uint32_t bound = w[3]; size_t cap = n + 4096; uint32_t *o = malloc(cap * 4);
    int32_t *loc = malloc(bound * 4); uint8_t *bb = calloc(bound, 1), *sbp = calloc(bound, 1), *der = calloc(bound, 1), *glob = calloc(bound, 1);
    uint32_t *pst = calloc(bound, 4), *ppt = calloc(bound, 4), *newp = calloc(bound, 4);   // pointer types: storage, pointee; the StorageBuffer pointer to a pointee (existing or new)
    uint32_t *res = NULL, nb = bound, nglob = 0, nnew = 0, news[256][2]; size_t k = 0;
    if (!o || !loc || !bb || !sbp || !der || !glob || !pst || !ppt || !newp) goto done;
    for (uint32_t i = 0; i < bound; i++) loc[i] = -1;
    for (int pass = 0; pass < 3; pass++)   // BufferBlock structs, then arrays of them (twice: an array of arrays)
        for (size_t i = 5; i < n; ) { uint32_t len = w[i] >> 16, op = w[i] & 0xffff; if (!len || i + len > n) goto done;
            if (pass == 0 && op == 71 && len >= 3 && w[i + 2] == 3 && w[i + 1] < bound) bb[w[i + 1]] = 1;   // OpDecorate id BufferBlock
            if (pass == 0 && op == 71 && len >= 4 && w[i + 2] == 30 && w[i + 1] < bound) loc[w[i + 1]] = (int32_t)w[i + 3];
            if (pass > 0 && (op == 28 || op == 29) && len >= 3 && w[i + 1] < bound && w[i + 2] < bound && bb[w[i + 2]]) bb[w[i + 1]] = 1;
            i += len; }
    for (size_t i = 5; i < n; ) { uint32_t len = w[i] >> 16, op = w[i] & 0xffff;
        if (op == 32 && len == 4 && w[i + 1] < bound) { pst[w[i + 1]] = w[i + 2]; ppt[w[i + 1]] = w[i + 3];
            if (w[i + 2] == 2 && w[i + 3] < bound && bb[w[i + 3]]) sbp[w[i + 1]] = 1;
            if (w[i + 2] == 12 && w[i + 3] < bound) newp[w[i + 3]] = w[i + 1]; }
        if (op == 59 && len >= 4 && w[i + 3] != 7 && w[i + 2] < bound) { glob[w[i + 2]] = 1; nglob++; if (w[i + 1] < bound && sbp[w[i + 1]]) der[w[i + 2]] = 1; }
        if ((op == 65 || op == 66 || op == 67 || op == 83) && len >= 4 && w[i + 3] < bound && der[w[i + 3]] && w[i + 2] < bound && w[i + 1] < bound) {
            der[w[i + 2]] = 1; uint32_t pt = ppt[w[i + 1]]; if (pt >= bound) goto done;
            if (!newp[pt]) { if (nnew >= 256) goto done; newp[pt] = nb++; news[nnew][0] = newp[pt]; news[nnew++][1] = pt; } }
        if (op == 55 && len >= 3 && w[i + 1] < bound && sbp[w[i + 1]]) goto done;   // a storage buffer passed to a function: not retyped
        i += len; }
    for (uint32_t t = 0; t < bound; t++) if (sbp[t] && ppt[t] < bound && newp[ppt[t]] == 0) newp[ppt[t]] = t;   // the struct's own pointer type turns StorageBuffer in place
    memcpy(o, w, 20); o[1] = 0x00010400; k = 5; int caps = 0, ext = 0, dec = 0, fn = 0;
    static const char EXT[] = "SPV_EXT_mesh_shader"; uint32_t ew[6] = {0}; memcpy(ew, EXT, sizeof EXT);   // 20 bytes: 5 words
    for (size_t i = 5; i < n; ) { uint32_t len = w[i] >> 16, op = w[i] & 0xffff;
        if (k + len + nglob + 8 * nnew + 64 + 2 * 32 > cap) goto done;
        if (op == 17 && !caps) { caps = 1; o[k++] = 2u << 16 | 17; o[k++] = 5283; }   // OpCapability MeshShadingEXT
        if ((op == 11 || op == 14) && !ext) { ext = 1; o[k++] = 6u << 16 | 10; memcpy(o + k, ew, 20); k += 5; }   // OpExtension, before the imports and the memory model
        if ((op == 71 || op == 72 || (op >= 19 && op <= 39) || op == 43 || op == 59) && !dec) {   // the decorations, first in the annotations
            dec = 1;
            for (size_t j = 5; j < n; ) { uint32_t l2 = w[j] >> 16, o2 = w[j] & 0xffff;
                if (o2 == 59 && l2 >= 4 && w[j + 3] == 1 && w[j + 2] < bound && loc[w[j + 2]] >= 0 && loc[w[j + 2]] < 32 && (prim >> loc[w[j + 2]] & 1)) { o[k++] = 3u << 16 | 71; o[k++] = w[j + 2]; o[k++] = 5271; }   // PerPrimitiveEXT
                j += l2; }
        }
        if (op == 54 && !fn) { fn = 1; for (uint32_t q = 0; q < nnew; q++) { o[k++] = 4u << 16 | 32; o[k++] = news[q][0]; o[k++] = 12; o[k++] = news[q][1]; } }   // new StorageBuffer pointer types
        if (op == 15) {   // OpEntryPoint model id "name" interface...: every global
            size_t s = i + 3; while (s < i + len) { uint32_t x = w[s++]; if (!(x & 0xff) || !(x & 0xff00) || !(x & 0xff0000) || !(x & 0xff000000)) break; }
            size_t at = k; memcpy(o + k, w + i, (s - i) * 4); k += s - i;
            for (uint32_t v = 0; v < bound; v++) if (glob[v]) o[k++] = v;
            o[at] = (uint32_t)((k - at) << 16) | op; i += len; continue; }
        memcpy(o + k, w + i, len * 4);
        if (op == 71 && len >= 3 && w[i + 2] == 3) o[k + 2] = 2;   // BufferBlock → Block
        if (op == 32 && len == 4 && w[i + 1] < bound && sbp[w[i + 1]]) o[k + 2] = 12;   // Uniform → StorageBuffer
        if (op == 59 && len >= 4 && w[i + 1] < bound && sbp[w[i + 1]]) o[k + 3] = 12;
        if ((op == 65 || op == 66 || op == 67 || op == 83) && len >= 4 && w[i + 3] < bound && der[w[i + 3]] && w[i + 1] < bound) o[k + 1] = newp[ppt[w[i + 1]]];
        k += len; i += len;
    }
    if (!caps || !dec || !fn) goto done;
    o[3] = nb; res = o; o = NULL; *on = k;
done:
    free(o); free(loc); free(bb); free(sbp); free(der); free(glob); free(pst); free(ppt); free(newp);
    return res;
}
// The variant is an lmod made at once (its module, no identifier): its code goes through the rewrites lm_code gives the file's (the pool set, the subgroup
// rewrites, the set's binding numbers made dense), which the pipeline's other modules and the layout have
static lmod *ps_variant_prim(vshader *s, uint32_t miss, uint32_t prim) {
    for (int i = 0; i < s->npvar; i++) if (s->pvar[i].miss == miss && s->pvar[i].prim == prim) return s->pvar[i].m;
    char p[700]; snprintf(p, sizeof p, "%s/%s.spv", g_spv_dir, s->key);
    size_t n = 0, nd = 0, on = 0; uint32_t *w = spv_read(p, &n), *d = w && miss ? spv_drop_inputs(w, n, miss, &nd) : NULL;
    uint32_t *v = w ? spv_perprim(d ? d : w, d ? nd : n, prim, &on) : NULL;
    lmod *m = NULL;
    if (v) {
        uint32_t *code = v, *np = NULL, *pf = NULL, *fit = NULL; size_t cn = on, nn, pn, fn;
        if ((nn = gm_np_code(code, cn, p, 1, &np))) { code = np; cn = nn; }   // raw memory without physical pointers, as lm_code (gfx_vk_gmem_noptr.inc)
        if ((pn = spv_pool_fit(code, cn, p, &pf))) { code = pf; cn = pn; }
        if ((fn = spv_fit(code, cn, p, &fit))) { code = fit; cn = fn; }
        if (s->bmap) spv_bind_remap(code, cn, s->bmap);
        { const char *dd = getenv("DM_DUMP_DIR"); if (dd) { char q[700]; mkdir(dd, 0777); snprintf(q, sizeof q, "%s/pvar_%s_%x_%x.spv", dd, s->key, miss, prim); FILE *f = fopen(q, "wb"); if (f) { fwrite(code, 4, cn, f); fclose(f); } } }   // for spirv-val
        VkShaderModule mod = lm_create(code, cn, p); free(np); free(pf); free(fit);
        size_t pl = strlen(p) + 1;
        if (mod && (m = calloc(1, sizeof *m + pl))) { memcpy(m->path, p, pl); m->miss = miss; m->bm = s->bmap; atomic_store(&m->m, mod); } else if (mod) vkDestroyShaderModule(g_vk.dev, mod, NULL);
    }
    LOG("[vk] fragment shader %s: inputs at locations %#x are per-primitive outputs of a mesh shader%s: a SPIR-V 1.4 variant reads them PerPrimitiveEXT%s", s->key, prim,
        miss ? ", and some no stage writes (zero)" : "", m ? "" : " [RED] (variant failed: the pipeline keeps the shader as it is)");
    free(w); free(d); free(v);
    if (!m) m = miss ? ps_variant(s, miss) : s->mod;
    if (s->npvar < 2) { s->pvar[s->npvar].miss = miss; s->pvar[s->npvar].prim = prim; s->pvar[s->npvar++].m = m; }
    return m;
}
