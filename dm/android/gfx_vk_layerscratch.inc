// ---- Colour targets that are layers of one image, drawn into one-layer scratch images (layer_scratch_on) ----
// Ocarina of Time makes the mip levels of its 128x128 B10G11R11 reflection cubes with one pass a level: six colour targets, the six faces of that level, whose fragment
// shader samples the cube one level up. Qualcomm's driver on the Adreno 830 (0x80320048, an AYN Odin 3) puts the outputs of a pass whose colour targets are layers of one
// image into the wrong layers: with a fragment shader writing one constant colour per output, face 1 got output 2's colour, face 2 output 0's, faces 4 and 5 output 0's
// too, with stray 8-pixel blocks of the others (uniforms, views, sampler and barriers checked right). The levels past the first came out flat or nearly black, and the
// water of Kokiri Forest, which reads the rougher levels more the steeper the camera looks down, went black as the camera was raised. Layers of one scratch array were
// drawn wrong the same way; one image per target is drawn right. So such a pass (layer_scratch.h lsc_takes: one layer, one sample, two or more colour targets, each a
// different layer of one level of one array or cube image, no BC shadow) draws each target into a 2D image of its own: the level's layers are copied into them as the
// pass begins (rp_start: the pass loads them) and back as it ends (rp_end), every time it is begun again. The images are kept by format, size and slot (LSC_MAX at most).
// Another bug than one image in two slots (dup_slots, the same layer twice) and than small layered passes on the Adreno 6xx (gfx_vk_layerpad.inc): this pass is not layered.
// DM_VK_LAYER_SCRATCH (env.txt): 1 on any GPU, 0 off; by default on Qualcomm's own driver (g_qcom_driver), not on Turnip
static int layer_scratch_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("DM_VK_LAYER_SCRATCH"); v = e ? *e == '1' : g_qcom_driver;
                 if (v) LOG("[vk] a render pass whose colour targets are layers of one image draws into one-layer scratch images (DM_VK_LAYER_SCRATCH)"); }
    return v;
}
enum { LSC_MAX = 128 };   // (Ocarina of Time: six slots by eight level sizes)
typedef struct { VkFormat f; uint32_t w, h, slot; } lsc_key;
typedef struct { vtex *base, *sc; uint32_t lv, ly; } lsc_pend_t;   // a slot drawn into a scratch image: its target's image, level and layer
static nvn_hmap g_lsc = {.klen = sizeof(lsc_key)};   // the scratch images
static lsc_pend_t g_lsc_pend[8];   // the prepared pass's slots drawn into scratch images: copied in at each rp_start, back at each rp_end
static uint64_t g_layer_scratch;   // passes prepared so (cumulative)
static uint32_t g_lsc_n;
// slot j's scratch image for level lv of b (lw x lh texels, b's format); NULL: none could be made
static vtex *lsc_scratch(const vtex *b, uint32_t lv, uint32_t j) {
    uint32_t lw = b->w >> lv ? b->w >> lv : 1, lh = b->h >> lv ? b->h >> lv : 1;
    lsc_key k; memset(&k, 0, sizeof k); k.f = b->fmt; k.w = lw; k.h = lh; k.slot = j;
    vtex *s = nvn_hm_get(&g_lsc, &k); if (s) return s;
    if (g_lsc_n >= LSC_MAX) { RED_ONCE(187, "colour targets in layers of one image: %d scratch images made, the most kept; such passes of other formats or sizes draw into their targets (layer_scratch_on)", LSC_MAX); return NULL; }
    tex_t t; memset(&t, 0, sizeof t); t.magic = MAGIC('T', 'E', 'X', 0); t.serial = ~0x40000ull - g_lsc_n; t.d.target = NVN_TEXTURE_TARGET_2D; t.d.format = b->desc.format;
    t.d.flags = b->desc.flags & NVN_TEXTURE_FLAGS_IMAGE;   // (as b's: vt_create's choice of a held format follows it)
    t.d.width = lw; t.d.height = lh; t.d.depth = 1; t.d.levels = 1;
    if (!(s = vt_create(&t))) return NULL;
    if (s->fmt != b->fmt || s->conv || (s->usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) !=
        (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) { vt_destroy(s); return NULL; }
    nvn_hm_put(&g_lsc, &k, s); g_lsc_n++;
    static int nlog; if (nlog++ < 4) LOG("[vk] scratch image for colour targets in layers of one image: slot %u, %ux%u vk%d (layer_scratch_on)", j, lw, lh, (int)b->fmt);
    return s;
}
// rp_prepare_, after dup_slots: the pass's colour views by slot, its framebuffer's layers and samples. Fills pend (copied to g_lsc_pend once the pass is made) and
// points the slots it takes at their scratch images; leaves both as they are when the pass is not such a one or an image could not be made
static void layer_scratch(rp_key *k, VkImageView cvw[8], uint32_t layers, lsc_pend_t pend[8]) {
    if (k->ncolor < 2 || !layer_scratch_on()) return;
    lsc_slot sl[8]; memset(sl, 0, sizeof sl);
    for (uint32_t i = 0; i < k->ncolor; i++) {
        vtex *v = P.cv[i]; if (!cvw[i] || !v) continue;
        sl[i].img = (uintptr_t)v->img; sl[i].lv = P.clv[i]; sl[i].ly = P.cly[i];
        sl[i].plain = v->type == VK_IMAGE_TYPE_2D && v->layers > 1 && v->samples == VK_SAMPLE_COUNT_1_BIT && v->aspect == VK_IMAGE_ASPECT_COLOR_BIT && !v->conv &&
                      !g_bcrt_pend[i].sh && k->color[i] == v->fmt && P.clv[i] < v->levels && P.cly[i] < v->layers;
    }
    if (!lsc_takes(sl, (int)k->ncolor, layers, k->samples)) return;
    VkImageView nv[8]; memcpy(nv, cvw, sizeof nv);
    for (uint32_t i = 0; i < k->ncolor; i++) {
        if (!sl[i].img) continue;
        vtex *s = lsc_scratch(P.cv[i], P.clv[i], i);
        VkImageView iv = s ? view_get(s, VK_IMAGE_VIEW_TYPE_2D, 0, 1, 0, 1, VK_IMAGE_ASPECT_COLOR_BIT, NULL) : VK_NULL_HANDLE;
        if (!iv) { RED_ONCE(188, "colour targets in layers of one image (vk%d): no scratch image, the pass draws into its targets (layer_scratch_on)", (int)k->color[i]); memset(pend, 0, 8 * sizeof *pend); return; }
        nv[i] = iv; pend[i].base = P.cv[i]; pend[i].sc = s; pend[i].lv = P.clv[i]; pend[i].ly = P.cly[i];
    }
    memcpy(cvw, nv, sizeof nv);
    static int nlog; if (nlog++ < 4) for (uint32_t i = 0; i < k->ncolor; i++) if (pend[i].sc) {
        LOG("[vk] pass %ux%u with colour targets in layers of level %u of one image (vk%d): drawn into one-layer scratch images and copied back (layer_scratch_on)", pend[i].sc->w, pend[i].sc->h, pend[i].lv, (int)pend[i].sc->fmt); break; }
}
// outside a pass: the prepared pass's layers into its scratch images (back 0, as it begins) or out of them (as it ends)
static void lsc_copy(int back) {
    int any = 0;
    for (int i = 0; i < 8; i++) {
        if (!g_lsc_pend[i].sc) continue;
        if (!any) { cb_barrier(); any = 1; }
        vtex *b = g_lsc_pend[i].base, *s = g_lsc_pend[i].sc;
        VkImageSubresourceLayers bs = {VK_IMAGE_ASPECT_COLOR_BIT, g_lsc_pend[i].lv, g_lsc_pend[i].ly, 1}, ss = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        VkImageCopy r = {back ? ss : bs, {0, 0, 0}, back ? bs : ss, {0, 0, 0}, {s->w, s->h, 1}};
        vkCmdCopyImage(C->cb, back ? s->img : b->img, VK_IMAGE_LAYOUT_GENERAL, back ? b->img : s->img, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    }
    if (any) cb_barrier();
}
static void lsc_drop_img(VkImage img) {   // an image that goes (views_drop): the prepared pass copies nothing more into or out of it
    for (int i = 0; i < 8; i++) if (g_lsc_pend[i].sc && (g_lsc_pend[i].base->img == img || g_lsc_pend[i].sc->img == img)) memset(&g_lsc_pend[i], 0, sizeof g_lsc_pend[i]);
}
