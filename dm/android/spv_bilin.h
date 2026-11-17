// Bilinear filtering made by the shader, for a texture whose format the GPU can't filter linearly bound with a linear sampler (gfx_vk_draw.inc bl_mask, ps_bl_mod).
// Maxwell filters every float and normalized format, depth ones included, and games sample depth and 32-bit float targets through linear samplers. Samsung's Xclipse
// 950 filters none of D16_UNORM, D32_SFLOAT, D32_SFLOAT_S8_UINT, R32_SFLOAT, R32G32_SFLOAT and R32G32B32A32_SFLOAT (no SAMPLED_IMAGE_FILTER_LINEAR), and Vulkan leaves
// such a sample undefined: the driver reads the nearest texel. Pokemon Legends: Arceus's screen-space shadow mask samples its D32_SFLOAT shadow map that way and
// compares the depths itself; read as steps, the shadow map's depth lies across a sloped surface in terraces, and the rocks and terrain shadowed themselves in bands.
// The sample is made as a gather instead, which needs no filtering: the four texels of the footprint the hardware would filter, weighed by the fraction of the
// position in texels, as the hardware's bilinear filter does. For OpImageSample{Implicit,Explicit}Lod and the Dref forms of a sampled image loaded from a variable at
// (set, binding) in bl (binding in the low 12 bits, the format's channels, 1 to 4, above), of a 2D or 2D array image: the size of level 0 is queried, the position
// p = uv × size - 0.5 split into floor and fraction, and the gather made at the footprint's centre (floor + 1) / size, so the texels it takes don't depend on how the
// hardware rounds p; ConstOffset and Offset go to the gather, the level operands (Bias, Lod, Grad, MinLod) are dropped: the replay asks for this only where the view has
// one level. A plain sample gathers each channel the shader reads of the result (OpCompositeExtract; another use reads all) up to the format's, the others read 0
// (alpha 1); a Dref sample gathers the comparisons (OpImageDrefGather), which bilinear weights make the filtered comparison the hardware would. The module in out
// (cap words): its word count, or 0 when nothing was rewritten; *need, when given, the words it takes; *left, when given, the samples of those images left as they were
// (a projective or cube one, an operand it doesn't take). Self-contained (no libc) for dm/tools/tests/test_spv_bilin.py.
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { SPV_BILIN_MAX = 8, SPV_BILIN_SITE_WORDS = 320 };

static int spv_bilin_str(const uint32_t *s, uint32_t nw, const char *e) {   // an instruction's literal string equal to e (no libc)
    for (uint32_t k = 0;; k++) { if (k / 4 >= nw) return 0; char c = (char)(s[k / 4] >> (8 * (k % 4))); if (c != e[k]) return 0; if (!c) return 1; }
}
// the image operands a sample may have that the gather keeps or drops: Bias, Lod, Grad, ConstOffset, Offset, MinLod; *off and *offw the offset's bit and id (0: none)
static int spv_bilin_ops(const uint32_t *w, uint32_t at, uint32_t len, uint32_t *off, uint32_t *offw) {
    *off = 0; *offw = 0; if (at >= len) return 1;
    uint32_t m = w[at], k = at + 1;
    if (m & ~0x9fu) return 0;
    if (m & 0x1) k++;   // Bias
    if (m & 0x2) k++;   // Lod
    if (m & 0x4) k += 2;   // Grad
    if (m & 0x8) { *off = 0x8; *offw = w[k++]; }   // ConstOffset
    if (m & 0x10) { if (*off) return 0; *off = 0x10; *offw = w[k++]; }   // Offset
    if (m & 0x80) k++;   // MinLod
    return k <= len;
}
static size_t spv_bilin(const uint32_t *w, size_t n, uint32_t set, const uint16_t *bl, int nbl, uint32_t *out, size_t cap, size_t *need, int *left) {
    if (need) *need = 0;
    if (left) *left = 0;
    if (n < 5 || nbl <= 0 || nbl > SPV_BILIN_MAX) return 0;
    uint32_t bound = w[3], glsl = 0, tf = 0, ti = 0, v2f = 0, v3f = 0, v4f = 0, v2i = 0, v3i = 0;
    int hasq = 0, hasgx = 0; size_t memmodel = 0, firstcap = 0;
    // the variables at (set, binding): id, channels, the sampled image's type and its image type; up to SPV_BILIN_MAX
    uint32_t var[SPV_BILIN_MAX], vch[SPV_BILIN_MAX], vsi[SPV_BILIN_MAX], vim[SPV_BILIN_MAX], varr[SPV_BILIN_MAX]; int nv = 0;
    for (size_t i = 5; i < n; ) { uint32_t len = w[i] >> 16, op = w[i] & 0xffff; if (!len || i + len > n) return 0;
        if (op == 17 && len == 2) { if (!firstcap) firstcap = i; if (w[i + 1] == 50) hasq = 1; if (w[i + 1] == 25) hasgx = 1; }   // OpCapability ImageQuery / ImageGatherExtended
        if (op == 11 && len >= 3 && spv_bilin_str(w + i + 2, len - 2, "GLSL.std.450")) glsl = w[i + 1];   // OpExtInstImport
        if (op == 14 && !memmodel) memmodel = i;   // OpMemoryModel
        if (op == 22 && len == 3 && w[i + 2] == 32) tf = w[i + 1];   // OpTypeFloat 32
        if (op == 21 && len == 4 && w[i + 2] == 32 && !ti) ti = w[i + 1];   // OpTypeInt 32
        i += len; }
    if (!tf || !memmodel || !firstcap) return 0;
    for (size_t i = 5; i < n; ) { uint32_t len = w[i] >> 16, op = w[i] & 0xffff;
        if (op == 23 && len == 4 && w[i + 2] == tf) { if (w[i + 3] == 2) v2f = w[i + 1]; if (w[i + 3] == 3) v3f = w[i + 1]; if (w[i + 3] == 4) v4f = w[i + 1]; }   // OpTypeVector float
        if (op == 23 && len == 4 && ti && w[i + 2] == ti) { if (w[i + 3] == 2) v2i = w[i + 1]; if (w[i + 3] == 3) v3i = w[i + 1]; }   // OpTypeVector int
        if (op == 59 && len >= 4 && w[i + 3] == 0 && nv < SPV_BILIN_MAX) {   // OpVariable UniformConstant: at (set, binding)?
            uint32_t v = w[i + 2], s = ~0u, b = ~0u;
            for (size_t j = 5; j < i; ) { uint32_t l2 = w[j] >> 16, o2 = w[j] & 0xffff;
                if (o2 == 71 && l2 == 4 && w[j + 1] == v && w[j + 2] == 34) s = w[j + 3];   // DescriptorSet
                if (o2 == 71 && l2 == 4 && w[j + 1] == v && w[j + 2] == 33) b = w[j + 3];   // Binding
                j += l2; }
            for (int q = 0; q < nbl; q++) if (s == set && b == (uint32_t)(bl[q] & 0xfff)) {
                uint32_t pt = w[i + 1], si = 0, im = 0, dim = 0, arr = 0, ms = 1;
                for (size_t j = 5; j < i; ) { uint32_t l2 = w[j] >> 16, o2 = w[j] & 0xffff; if (o2 == 32 && l2 == 4 && w[j + 1] == pt) si = w[j + 3]; j += l2; }   // OpTypePointer
                for (size_t j = 5; j < i && si; ) { uint32_t l2 = w[j] >> 16, o2 = w[j] & 0xffff; if (o2 == 27 && l2 == 3 && w[j + 1] == si) im = w[j + 2]; j += l2; }   // OpTypeSampledImage
                for (size_t j = 5; j < i && im; ) { uint32_t l2 = w[j] >> 16, o2 = w[j] & 0xffff; if (o2 == 25 && l2 >= 9 && w[j + 1] == im) { dim = w[j + 3]; arr = w[j + 5]; ms = w[j + 6]; } j += l2; }   // OpTypeImage
                uint32_t ch = (uint32_t)(bl[q] >> 12); if (ch < 1 || ch > 4) ch = 4;
                if (im && dim == 1 && !ms) { var[nv] = v; vch[nv] = ch; vsi[nv] = si; vim[nv] = im; varr[nv] = arr; nv++; }   // 2D (an array too)
            }
        }
        i += len; }
    if (!nv) return 0;
    // the sampled images loaded from them, and the samples of those: counted, and the channels each plain sample's result is read in
    uint32_t nsite = 0, nleft = 0, needq = 0, need3 = 0;
    for (size_t i = 5; i < n; ) { uint32_t len = w[i] >> 16, op = w[i] & 0xffff;
        if (op >= 87 && op <= 90 && len >= (op >= 89 ? 6u : 5u)) {
            uint32_t si = w[i + 3], ld = 0, vi = 0;
            for (size_t j = 5; j < i; ) { uint32_t l2 = w[j] >> 16, o2 = w[j] & 0xffff;
                if (o2 == 61 && l2 >= 4 && w[j + 2] == si) for (int q = 0; q < nv; q++) if (w[j + 3] == var[q]) { ld = 1; vi = (uint32_t)q; }   // %si = OpLoad %type %var
                j += l2; }
            if (ld) { uint32_t off, offw;
                if (!spv_bilin_ops(w + i, op >= 89 ? 6u : 5u, len, &off, &offw) || (off == 0x10 && !hasgx)) nleft++;
                else { nsite++; needq = 1; if (varr[vi]) need3 = 1; } }
        }
        if (op >= 91 && op <= 94 && len >= 5) {   // a projective sample of one: left (fetches and gathers need no filter)
            uint32_t si = w[i + 3]; for (size_t j = 5; j < i; ) { uint32_t l2 = w[j] >> 16, o2 = w[j] & 0xffff;
                if (o2 == 61 && l2 >= 4 && w[j + 2] == si) for (int q = 0; q < nv; q++) if (w[j + 3] == var[q]) nleft++;
                j += l2; } }
        i += len; }
    if (left) *left = (int)nleft;
    if (!nsite) return 0;
    size_t want = n + 6 + 2 + 4 * 5 + 4 * 9 + 2 * 5 + (size_t)nsite * SPV_BILIN_SITE_WORDS;
    if (need) *need = want;
    if (!out || cap < want) return 0;
    uint32_t nid = bound;
    uint32_t nglsl = glsl ? 0 : nid++; if (!glsl) glsl = nglsl;
    uint32_t nti = ti ? 0 : nid++; if (!ti) ti = nti;
    uint32_t nv2f = v2f ? 0 : nid++; if (!v2f) v2f = nv2f;
    uint32_t nv3f = v3f || !need3 ? 0 : nid++; if (!v3f) v3f = nv3f;
    uint32_t nv4f = v4f ? 0 : nid++; if (!v4f) v4f = nv4f;
    uint32_t nv2i = v2i ? 0 : nid++; if (!v2i) v2i = nv2i;
    uint32_t nv3i = v3i || !need3 ? 0 : nid++; if (!v3i) v3i = nv3i;
    uint32_t ci[4] = {nid, nid + 1, nid + 2, nid + 3}; nid += 4;
    uint32_t fzero = nid++, fone = nid++, fhalf = nid++, half2 = nid++, one2 = nid++;
    size_t k = 0; for (; k < 5; k++) out[k] = w[k];
    int types = 0;
    for (size_t i = 5; i < n; ) { uint32_t len = w[i] >> 16, op = w[i] & 0xffff;
        if (i == firstcap && needq && !hasq) { out[k++] = 2u << 16 | 17; out[k++] = 50; }   // OpCapability ImageQuery
        if (i == memmodel && nglsl) {   // OpExtInstImport %glsl "GLSL.std.450"
            static const char nm[16] = "GLSL.std.450"; out[k++] = 6u << 16 | 11; out[k++] = nglsl;
            for (int q = 0; q < 4; q++) out[k++] = (uint32_t)(uint8_t)nm[4 * q] | (uint32_t)(uint8_t)nm[4 * q + 1] << 8 | (uint32_t)(uint8_t)nm[4 * q + 2] << 16 | (uint32_t)(uint8_t)nm[4 * q + 3] << 24; }
        if (op == 54 && !types) {   // before the first OpFunction: the types the module lacks, then the constants
            types = 1;
            if (nti) { out[k++] = 4u << 16 | 21; out[k++] = nti; out[k++] = 32; out[k++] = 1; }
            if (nv2f) { out[k++] = 4u << 16 | 23; out[k++] = nv2f; out[k++] = tf; out[k++] = 2; }
            if (nv3f) { out[k++] = 4u << 16 | 23; out[k++] = nv3f; out[k++] = tf; out[k++] = 3; }
            if (nv4f) { out[k++] = 4u << 16 | 23; out[k++] = nv4f; out[k++] = tf; out[k++] = 4; }
            if (nv2i) { out[k++] = 4u << 16 | 23; out[k++] = nv2i; out[k++] = ti; out[k++] = 2; }
            if (nv3i) { out[k++] = 4u << 16 | 23; out[k++] = nv3i; out[k++] = ti; out[k++] = 3; }
            for (uint32_t q = 0; q < 4; q++) { out[k++] = 4u << 16 | 43; out[k++] = ti; out[k++] = ci[q]; out[k++] = q; }   // OpConstant %int 0..3
            out[k++] = 4u << 16 | 43; out[k++] = tf; out[k++] = fzero; out[k++] = 0;   // 0.0
            out[k++] = 4u << 16 | 43; out[k++] = tf; out[k++] = fone; out[k++] = 0x3f800000u;   // 1.0
            out[k++] = 4u << 16 | 43; out[k++] = tf; out[k++] = fhalf; out[k++] = 0x3f000000u;   // 0.5
            out[k++] = 5u << 16 | 44; out[k++] = v2f; out[k++] = half2; out[k++] = fhalf; out[k++] = fhalf;   // OpConstantComposite (0.5, 0.5)
            out[k++] = 5u << 16 | 44; out[k++] = v2f; out[k++] = one2; out[k++] = fone; out[k++] = fone;   // (1, 1)
        }
        int site = 0; uint32_t vi = 0, off = 0, offw = 0;
        if (op >= 87 && op <= 90 && len >= (op >= 89 ? 6u : 5u)) {
            uint32_t si = w[i + 3];
            for (size_t j = 5; j < i; ) { uint32_t l2 = w[j] >> 16, o2 = w[j] & 0xffff;
                if (o2 == 61 && l2 >= 4 && w[j + 2] == si) for (int q = 0; q < nv; q++) if (w[j + 3] == var[q]) { site = 1; vi = (uint32_t)q; }
                j += l2; }
            if (site && (!spv_bilin_ops(w + i, op >= 89 ? 6u : 5u, len, &off, &offw) || (off == 0x10 && !hasgx))) site = 0;
        }
        if (site) {
            int dref = op >= 89; uint32_t rt = w[i + 1], r = w[i + 2], si = w[i + 3], co = w[i + 4], dr = dref ? w[i + 5] : 0, arr = varr[vi];
            uint32_t comps = 0xf;   // the channels the result is read in: OpCompositeExtract of it only, else all
            if (!dref) { comps = 0; int other = 0;
                for (size_t j = 5; j < n; ) { uint32_t l2 = w[j] >> 16, o2 = w[j] & 0xffff;
                    if (j != i) { if (o2 == 81 && l2 == 5 && w[j + 3] == r) comps |= w[j + 4] < 4 ? 1u << w[j + 4] : 0xfu;
                                  else for (uint32_t q = 1; q < l2; q++) if (w[j + q] == r) other = 1; }
                    j += l2; }
                if (other) comps = 0xf;
                comps &= (1u << vch[vi]) - 1; if (!comps) comps = 1; }
            uint32_t img = nid++, sz = nid++, sz2 = arr ? nid++ : sz, szf = nid++, uv = nid++, p = nid++, p2 = nid++, fl = nid++, fr = nid++, c1 = nid++, cc = nid++, nc = arr ? nid++ : cc, fx = nid++, fy = nid++;
            out[k++] = 4u << 16 | 100; out[k++] = vim[vi]; out[k++] = img; out[k++] = si;   // OpImage %img %si
            out[k++] = 5u << 16 | 103; out[k++] = arr ? v3i : v2i; out[k++] = sz; out[k++] = img; out[k++] = ci[0];   // OpImageQuerySizeLod: level 0
            if (arr) { out[k++] = 7u << 16 | 79; out[k++] = v2i; out[k++] = sz2; out[k++] = sz; out[k++] = sz; out[k++] = 0; out[k++] = 1; }   // OpVectorShuffle: its width and height
            out[k++] = 4u << 16 | 111; out[k++] = v2f; out[k++] = szf; out[k++] = sz2;   // OpConvertSToF
            out[k++] = 7u << 16 | 79; out[k++] = v2f; out[k++] = uv; out[k++] = co; out[k++] = co; out[k++] = 0; out[k++] = 1;   // the position's u, v
            out[k++] = 5u << 16 | 133; out[k++] = v2f; out[k++] = p; out[k++] = uv; out[k++] = szf;   // OpFMul: in texels
            out[k++] = 5u << 16 | 131; out[k++] = v2f; out[k++] = p2; out[k++] = p; out[k++] = half2;   // OpFSub: from the texels' centres
            out[k++] = 6u << 16 | 12; out[k++] = v2f; out[k++] = fl; out[k++] = glsl; out[k++] = 8; out[k++] = p2;   // Floor
            out[k++] = 5u << 16 | 131; out[k++] = v2f; out[k++] = fr; out[k++] = p2; out[k++] = fl;   // the fraction: the weights
            out[k++] = 5u << 16 | 129; out[k++] = v2f; out[k++] = c1; out[k++] = fl; out[k++] = one2;   // OpFAdd
            out[k++] = 5u << 16 | 136; out[k++] = v2f; out[k++] = cc; out[k++] = c1; out[k++] = szf;   // OpFDiv: the footprint's centre
            if (arr) { out[k++] = 8u << 16 | 79; out[k++] = v3f; out[k++] = nc; out[k++] = cc; out[k++] = co; out[k++] = 0; out[k++] = 1; out[k++] = 4; }   // and the layer
            out[k++] = 5u << 16 | 81; out[k++] = tf; out[k++] = fx; out[k++] = fr; out[k++] = 0;
            out[k++] = 5u << 16 | 81; out[k++] = tf; out[k++] = fy; out[k++] = fr; out[k++] = 1;
            uint32_t ch[4] = {fzero, fzero, fzero, fone};
            for (uint32_t c = 0; c < 4; c++) { if (!(comps >> c & 1)) continue;
                uint32_t g = nid++, gx = nid++, gy = nid++, gz = nid++, gw = nid++, t0 = nid++, t1 = nid++, rr = dref ? r : nid++;
                // OpImageGather %v4 %g %si %nc %comp / OpImageDrefGather %v4 %g %si %nc %dref, with the offset: (0, 1), (1, 1), (1, 0), (0, 0) in x, y, z, w
                out[k++] = (6u + (off ? 2u : 0u)) << 16 | (dref ? 97u : 96u); out[k++] = v4f; out[k++] = g; out[k++] = si; out[k++] = nc; out[k++] = dref ? dr : ci[c];
                if (off) { out[k++] = off; out[k++] = offw; }
                out[k++] = 5u << 16 | 81; out[k++] = tf; out[k++] = gx; out[k++] = g; out[k++] = 0;
                out[k++] = 5u << 16 | 81; out[k++] = tf; out[k++] = gy; out[k++] = g; out[k++] = 1;
                out[k++] = 5u << 16 | 81; out[k++] = tf; out[k++] = gz; out[k++] = g; out[k++] = 2;
                out[k++] = 5u << 16 | 81; out[k++] = tf; out[k++] = gw; out[k++] = g; out[k++] = 3;
                out[k++] = 8u << 16 | 12; out[k++] = tf; out[k++] = t0; out[k++] = glsl; out[k++] = 46; out[k++] = gw; out[k++] = gz; out[k++] = fx;   // FMix: the row at v0
                out[k++] = 8u << 16 | 12; out[k++] = tf; out[k++] = t1; out[k++] = glsl; out[k++] = 46; out[k++] = gx; out[k++] = gy; out[k++] = fx;   // the row at v1
                out[k++] = 8u << 16 | 12; out[k++] = dref ? rt : tf; out[k++] = rr; out[k++] = glsl; out[k++] = 46; out[k++] = t0; out[k++] = t1; out[k++] = fy;
                ch[c] = rr;
                if (dref) break; }
            if (!dref) { out[k++] = 7u << 16 | 80; out[k++] = rt; out[k++] = r; for (int c = 0; c < 4; c++) out[k++] = ch[c]; }   // OpCompositeConstruct: the sample's own result
            i += len; continue;
        }
        for (uint32_t q = 0; q < len; q++) out[k + q] = w[i + q];
        k += len; i += len; }
    if (!types) return 0;
    out[3] = nid; return k;
}
