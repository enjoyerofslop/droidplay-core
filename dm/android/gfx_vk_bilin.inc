// ---------------- Bilinear filtering made by the fragment shader, for formats the GPU can't filter linearly (included by gfx_vk_draw.inc before stage_textures) ----------------
// A Maxwell sampler filters every float and normalized format, depth ones included, and games sample depth and 32-bit float targets through linear samplers. Samsung's
// Xclipse 950 has no SAMPLED_IMAGE_FILTER_LINEAR for D16_UNORM, D32_SFLOAT, D32_SFLOAT_S8_UINT or any 32-bit float colour format, and reads the nearest texel there
// (Vulkan leaves it undefined). Pokemon Legends: Arceus's screen-space shadow mask (fragment shader 3be95ee5087724c5) samples its 1024x1024 two-layer D32_SFLOAT
// shadow map through a LINEAR_MIPMAP_NEAREST / LINEAR sampler and compares the depths itself: read as steps, the shadow map's depth lay across sloped rocks and terrain
// in terraces, so the surfaces shadowed themselves in bands that followed the slope (on RADV on the same GPU too). dm/android/tests/t_bilin.c measured it on a Galaxy
// Z Flip 7: a linear sample of D32_SFLOAT or R32_SFLOAT is up to 0.078 off the bilinear filter of a smooth 8x8 texture (the nearest texel), the same sample through
// spv_bilin within 1e-7. A comparison (a shadow sampler, PCF) of D32_SFLOAT is filtered all the same, within 0.0018 (8-bit weights, as Maxwell's), so comparison slots
// are left as they are. stage_textures marks the texture slots (the first 16, one texture each, not a comparison) of a 2D or 2D array texture whose view has one level,
// in such a format, with a sampler whose minification and magnification are both linear (tc_stage.bl: a bit per slot, and the format's channels less 1 in two bits each
// above bit 16). A fragment shader's mask goes into the pipeline key (pipe_key.bl), and its pipeline's fragment module is a variant whose samples of those slots are a
// gather and bilinear weights (spv_bilin.h, made last in lm_code, over the slots' binding numbers as remapped; ps_bl_mod). That key is not in the start-up list: such a
// pipeline is made at its first draw of a session (the driver's pipeline cache keeps it). Other stages read the nearest texel as before (a red line says so once), and so
// does a texture with more levels in its view (likewise). DM_VK_BILINEAR=0 (env.txt): nothing marked, the samples read the nearest texel as before
static int bl_fmt_nolin(VkFormat f);
// on where the GPU lacks linear filtering of a format games sample (else nothing is looked at, at no cost per draw); said once with the formats
static int bl_on(void) {
    static int v = -1; if (v >= 0) return v;
    const char *e = getenv("DM_VK_BILINEAR"); v = !(e && *e == '0');
    static const VkFormat cand[] = {VK_FORMAT_D16_UNORM, VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_R32_SFLOAT,
        VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R16_UNORM, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT};
    char l[160] = ""; for (size_t i = 0; i < sizeof cand / sizeof *cand; i++) if (bl_fmt_nolin(cand[i])) { size_t n = strlen(l); snprintf(l + n, sizeof l - n, "%s%d", n ? ", " : "", (int)cand[i]); }
    if (!l[0]) v = 0;
    else LOG("[vk] VkFormat %s: sampled but not filtered linearly on this GPU; %s", l, v ? "a fragment shader's linear samples of them are filtered by the shader (spv_bilin)" : "DM_VK_BILINEAR=0: read nearest through linear samplers");
    return v;
}
// a format the GPU samples but doesn't filter linearly, which a Maxwell sampler filters (not an integer one: no GPU filters those)
static int bl_fmt_nolin(VkFormat f) {
    static uint8_t known[256]; if ((unsigned)f >= 256) return 0;   // the core formats; 0 not asked, 1 filters (or not sampled), 2 doesn't
    if (!known[f]) { VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(g_vk.pd, f, &fp); VkFormatFeatureFlags o = fp.optimalTilingFeatures;
        int ds = f >= VK_FORMAT_D16_UNORM && f <= VK_FORMAT_D32_SFLOAT_S8_UINT;
        known[f] = (o & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) && !(o & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) && f != VK_FORMAT_S8_UINT && (ds || !is_int_fmt(f)) ? 2 : 1; }
    return known[f] == 2;
}
static int bl_fmt_ch(VkFormat f) {   // the channels a gather reads of it (depth: 1)
    switch (f) {
        case VK_FORMAT_D16_UNORM: case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_D16_UNORM_S8_UINT: case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT:
        case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_SNORM: case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SNORM: return 1;
        case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_R16G16_SNORM: case VK_FORMAT_R16G16_SFLOAT: case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_SNORM: return 2;
        default: return 4;
    }
}
// a sampler that filters linearly both ways (NULL: stage_set's default sampler, which does)
static int bl_samp_lin(const samp_par *sp) { return !sp || (sp->magf && (sp->minf == NVN_MIN_FILTER_LINEAR || sp->minf == NVN_MIN_FILTER_LINEAR_MIPMAP_NEAREST || sp->minf == NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR)); }
// a resolved slot's bits in tc_stage.bl: 0 unless the shader must filter it itself
static uint64_t bl_entry(const vshader *s, const tc_ent *e) {
    const vtex *v = e->v; const rb_tex *t = &s->tx[e->slot];
    // 2D (an array too: gathers take neither 1D, 3D nor a cube's seams), not multisample, not a comparison (the Xclipse filters those); blno: the shader only fetches it
    if ((t->type & 0xff) != 3 || (t->type & 0x600) || (s->blno >> e->slot & 1)) return 0;
    const view_t *vv = e->r.has_v ? &e->r.v : NULL;
    VkFormat f = view_format(v, vv); if (!f) f = v->fmt;
    if (!bl_fmt_nolin(f) || tex_aspect(v, &e->r, vv, s) == VK_IMAGE_ASPECT_STENCIL_BIT) return 0;
    uint32_t base = vv && vv->set_levels ? vv->base_level : 0, nl = vv && vv->set_levels && vv->nlevels ? vv->nlevels : v->levels > base ? v->levels - base : 1;
    if (nl != 1) { RED_ONCE(199, "%s shader %s samples a texture of format %d, which this GPU doesn't filter linearly, through a linear sampler, its view %u levels: read nearest (said once)", sh_stage_name(s->stage), s->key, f, nl); return 0; }
    return 1ull << e->slot | (uint64_t)(bl_fmt_ch(f) - 1) << (16 + 2 * e->slot);
}
// The fragment shader's variant whose samples of the marked slots are gathers (spv_bilin.h): one per inputs variant, alpha test and mask, made here and in the
// prewarm thread outside the replay's lock (as ps_alpha_mod). NULL: spv_bilin left the module as it was, and the draw reads nearest
static pthread_mutex_t g_bl_mu = PTHREAD_MUTEX_INITIALIZER;
static lmod *ps_bl_mod(vshader *s, uint32_t miss, int alpha, uint64_t bl) {
    pthread_mutex_lock(&g_bl_mu);
    lmod *m = NULL; int i = 0;
    for (; i < s->nblv; i++) if (s->blv[i].miss == miss && s->blv[i].alpha == alpha && s->blv[i].bl == bl) { m = s->blv[i].m; break; }
    if (i == s->nblv) {
        char p[700]; snprintf(p, sizeof p, "%s/%s.spv", g_spv_dir, s->key);
        // a slot the shader never samples (it only fetches or queries the texture) needs nothing: dropped here, and from later masks (blno), so their draws take the
        // plain pipeline again; the file's binding numbers are the slots' once remapped as sh_get did
        size_t fn = 0; uint32_t *code = spv_read(p, &fn); if (code && s->bmap) spv_bind_remap(code, fn, s->bmap);
        uint16_t b[SPV_BILIN_MAX]; int nb = 0;
        for (int q = 0; q < 16 && q < s->ntx && nb < SPV_BILIN_MAX; q++) if (bl >> q & 1) {
            uint16_t bq = (uint16_t)((s->tx[q].binding & 0xfff) | (int)((bl >> (16 + 2 * q) & 3) + 1) << 12); size_t need = 0; int left = 0;
            if (code) spv_bilin(code, fn, (uint32_t)(s->set & 3), &bq, 1, NULL, 0, &need, &left);
            if (code && !need && !left) s->blno |= (uint16_t)(1u << q); else b[nb++] = bq; }
        free(code);
        m = nb ? lm_new_b(p, miss, alpha, (uint32_t)(s->set & 3), b, nb, s->bmap) : NULL;
        if (s->nblv < (int)(sizeof s->blv / sizeof *s->blv)) { s->blv[s->nblv].miss = miss; s->blv[s->nblv].alpha = alpha; s->blv[s->nblv].bl = bl; s->blv[s->nblv++].m = m; }
        static int nl;
        if (!m && nb) RED_ONCE(200, "fragment shader %s samples textures this GPU doesn't filter linearly (slots %#llx), and spv_bilin can't filter them in the shader: read nearest (said once)", s->key, (unsigned long long)(bl & 0xffff));
        else if (m && nl++ < 8) { char d[120] = ""; for (int q = 0; q < nb; q++) { size_t l = strlen(d); snprintf(d + l, sizeof d - l, "%s%d", q ? ", " : "", b[q] & 0xfff); }
            LOG("[vk] fragment shader %s: its samples of binding %s (a format this GPU doesn't filter linearly, through a linear sampler) are a gather and bilinear weights (spv_bilin)%s", s->key, d, alpha ? ", with the alpha test" : ""); }
    }
    pthread_mutex_unlock(&g_bl_mu);
    return m;
}
