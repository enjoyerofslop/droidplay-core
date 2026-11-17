// A vertex shader's storage writes run in the fragment stage, for a GPU without vertexPipelineStoresAndAtomics (Arm's Mali drivers: the Pixel 9 Pro XL's
// Mali-G715, driver 0xd803000). A draw under rasterizer discard whose vertex shader writes memory runs as a dispatch would (gfx_vk_draw.inc rd_draw); without
// vertex-stage stores it was left out (red line 184), and Xenoblade Chronicles 2, which skins every character with such draws, drew none of them on Mali.
// Mali has fragmentStoresAndAtomics, so the whole vertex shader runs as a fragment shader instead, once per vertex, over a point list:
//   - spv_vsf_vs makes a pass-through vertex shader: it reads the inputs the shader reads (the pipeline's vertex input state fetches them, formats, strides and
//     divisors as for the draw itself), hands them on as flat outputs at the same locations, with gl_VertexIndex, gl_InstanceIndex (and the draw parameters the
//     shader reads) at SPV_VSF_LOC and up, and places vertex v of instance i as a one-pixel point on pixel n & 1023, n >> 10 of a 1024 × 1024 grid, n = (i - ibase)
//     × vspan + (v - vbase) - lbase from its push constants; a vertex past the draw's (n >= total, from an index the draw's range doesn't hold, a restart index
//     drawn as a point) or past this pass's 2^20 points lands off the grid, so its writes are never made.
//   - spv_vsf_fs makes the shader itself a fragment shader: its entry point Fragment (OriginUpperLeft), its vertex inputs flat fragment inputs at the same
//     locations, its built-in inputs flat inputs at SPV_VSF_LOC + their index, its outputs (gl_PerVertex and the generic ones) private variables, and the
//     decorations only an output takes left out. Its code, its storage writes and atomics included, is unchanged.
// A point covers exactly the pixel whose centre it sits on, so each vertex's shader runs once (a vertex an index buffer names twice runs twice, as a vertex
// shader may); helper invocations of the point's quad make no writes. Self-contained (no libc) for dm/tools/tests/test_spv_vsfrag.py.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define SPV_VSF_LOC 16                // the built-ins' flat inputs: VertexIndex at 16, InstanceIndex 17, BaseVertex 18, BaseInstance 19, DrawIndex 20
#define SPV_VSF_SHIFT 10              // the grid is 1 << SPV_VSF_SHIFT pixels wide and high
#define SPV_VSF_MAX (1u << (2 * SPV_VSF_SHIFT))   // points one pass places
#define SPV_VSF_PC_BYTES 20           // the pass-through vertex shader's push constants: vbase, vspan, ibase, total, lbase (uint each)
enum { SPV_VSF_NBI = 5 };
static const uint32_t SPV_VSF_BI[SPV_VSF_NBI] = {42, 43, 4424, 4425, 4426};   // BuiltIn VertexIndex, InstanceIndex, BaseVertex, BaseInstance, DrawIndex
// What the pass-through vertex shader hands on: the shader's vertex inputs at locations below SPV_VSF_LOC (their component kind 'f' float, 'u' uint, 's' int,
// and count), and the built-ins it reads (bit b: SPV_VSF_BI[b], of kind bikind[b])
typedef struct { uint32_t in; uint8_t kind[SPV_VSF_LOC], ncomp[SPV_VSF_LOC]; uint32_t bi; uint8_t bikind[SPV_VSF_NBI]; } spv_vsf_io;

static int spv_vsf_emit(uint32_t *o, size_t *k, size_t cap, uint32_t op, const uint32_t *a, uint32_t na) {
    if (*k + 1 + na > cap) return 0;
    o[(*k)++] = (na + 1) << 16 | op; for (uint32_t i = 0; i < na; i++) o[(*k)++] = a[i];
    return 1;
}
#define SPV_VSF_PUT(op, ...) do { const uint32_t a_[] = {__VA_ARGS__}; if (!spv_vsf_emit(o, &k, cap, (op), a_, (uint32_t)(sizeof a_ / 4))) return 0; } while (0)
#define SPV_VSF_PUT0(op) do { if (!spv_vsf_emit(o, &k, cap, (op), NULL, 0)) return 0; } while (0)

// The instruction that defines a type (its result first) or a variable (result second); NULL when there is none
static const uint32_t *spv_vsf_def(const uint32_t *w, size_t n, uint32_t id) {
    for (size_t i = 5; i < n; ) { uint32_t len = w[i] >> 16, op = w[i] & 0xffff; if (!len || i + len > n) return NULL;
        if (op >= 19 && op <= 39 && len >= 2 && w[i + 1] == id) return w + i;
        if (op == 59 && len >= 4 && w[i + 2] == id) return w + i;
        i += len; }
    return NULL;
}
// A 32-bit scalar or vector type t: its kind ('f', 'u', 's') and component count, else 0
static int spv_vsf_kind(const uint32_t *w, size_t n, uint32_t t, uint8_t *kind, uint8_t *nc) {
    const uint32_t *d = spv_vsf_def(w, n, t); if (!d) return 0;
    uint32_t op = d[0] & 0xffff; *nc = 1;
    if (op == 23) { if (d[3] < 2 || d[3] > 4) return 0; *nc = (uint8_t)d[3]; d = spv_vsf_def(w, n, d[2]); if (!d) return 0; op = d[0] & 0xffff; }   // OpTypeVector
    if (op == 22 && d[2] == 32) { *kind = 'f'; return 1; }
    if (op == 21 && d[2] == 32) { *kind = d[3] ? 's' : 'u'; return 1; }
    return 0;
}

// The vertex shader w[n] made a fragment shader as the file header says, into o[cap] (n + 3 × its vertex inputs and built-ins + 40 words is enough; the input
// is left untouched). Its entry point is a wrapper that returns at once in a helper invocation, so the point's quad runs the shader in one invocation and the
// shader's subgroup operations (a ballot that hands out slots, XC2's 289ca4012f858d7b) see only invocations that store. scratch: 2 × bound words (w[3]).
// *io: what the pass-through vertex shader must hand on. → its word count, or 0: not a vertex shader, a second entry point, an input other than a 32-bit scalar or
// vector at a location, one of SPV_VSF_BI or a subgroup built-in, no decoration or function to place the wrapper's before, or o too small
static size_t spv_vsf_fs(const uint32_t *w, size_t n, uint32_t *scratch, uint32_t *o, size_t cap, spv_vsf_io *io) {
    enum { OUTVAR = 1, OUTTYPE = 2, INVAR = 4, FLAT = 8, BIN = 16, KEEP = 32 };   // scratch[id]: what the id is; scratch[bound + id]: its location + 1, or built-in index + 1 (BIN)
    static const uint32_t KEPT[] = {36, 41, 4416, 4417, 4418, 4419, 4420};   // SubgroupSize, SubgroupLocalInvocationId, Subgroup{Eq,Ge,Gt,Le,Lt}Mask: the same in a fragment shader
    if (n < 5 || w[0] != 0x07230203u || cap < n) return 0;
    uint32_t bound = w[3], *fl = scratch, *at = scratch + bound, entry = 0, ver = w[1], tbool = 0, fnres = 0, fntype = 0;
    for (uint32_t i = 0; i < 2 * bound; i++) scratch[i] = 0;
    io->in = 0; io->bi = 0;
    for (uint32_t l = 0; l < SPV_VSF_LOC; l++) io->kind[l] = io->ncomp[l] = 0;
    for (int b = 0; b < SPV_VSF_NBI; b++) io->bikind[b] = 0;
    for (size_t i = 5; i < n; ) { uint32_t len = w[i] >> 16, op = w[i] & 0xffff; const uint32_t *a = w + i + 1; if (!len || i + len > n) return 0;
        if (op == 15) { if (len < 3 || a[0] != 0 || entry) return 0; entry = a[1]; }   // OpEntryPoint: one, Vertex
        if (op == 59 && len >= 4 && a[1] < bound) { if (a[2] == 3) fl[a[1]] |= OUTVAR; if (a[2] == 1) fl[a[1]] |= INVAR; }   // OpVariable Output / Input
        if (op == 20 && len >= 2 && !tbool) tbool = a[0];   // OpTypeBool
        i += len; }
    if (!entry) return 0;
    for (size_t i = 5; i < n; ) { uint32_t len = w[i] >> 16, op = w[i] & 0xffff; const uint32_t *a = w + i + 1;
        if (op == 54 && len >= 5 && a[1] == entry) { fnres = a[0]; fntype = a[3]; }   // OpFunction of the entry point
        if (op == 71 && len >= 3 && a[0] < bound) {   // OpDecorate
            if (a[1] == 14) fl[a[0]] |= FLAT;
            if (a[1] == 30 && len >= 4 && (fl[a[0]] & INVAR)) at[a[0]] = a[2] + 1;   // an input's Location
            if (a[1] == 11 && len >= 4 && (fl[a[0]] & INVAR)) {   // an input's BuiltIn: one of SPV_VSF_BI (handed on), a subgroup one (kept), else not made
                int b = 0, kept = 0; while (b < SPV_VSF_NBI && SPV_VSF_BI[b] != a[2]) b++;
                for (size_t q = 0; q < sizeof KEPT / sizeof *KEPT; q++) kept |= KEPT[q] == a[2];
                if (kept) fl[a[0]] |= KEEP;
                else if (b == SPV_VSF_NBI) return 0;
                else { fl[a[0]] |= BIN; at[a[0]] = (uint32_t)b + 1; } } }
        if (op == 59 && len >= 4 && a[1] < bound && (fl[a[1]] & OUTVAR)) {   // an output's type: a block's decorations go with it
            const uint32_t *p = spv_vsf_def(w, n, a[0]); const uint32_t *t = p && (p[0] & 0xffff) == 32 ? spv_vsf_def(w, n, p[3]) : NULL;
            if (t && (t[0] & 0xffff) == 30 && t[1] < bound) fl[t[1]] |= OUTTYPE; }   // OpTypeStruct (gl_PerVertex)
        i += len; }
    if (!fnres || !fntype) return 0;
    for (size_t i = 5; i < n; ) { uint32_t len = w[i] >> 16, op = w[i] & 0xffff; const uint32_t *a = w + i + 1;   // every input known: a vertex input or a built-in
        if (op == 59 && len >= 4 && a[1] < bound && (fl[a[1]] & INVAR) && !(fl[a[1]] & KEEP)) {
            const uint32_t *p = spv_vsf_def(w, n, a[0]); uint8_t kd = 0, nc = 0;
            if (!p || (p[0] & 0xffff) != 32 || !spv_vsf_kind(w, n, p[3], &kd, &nc) || !at[a[1]]) return 0;
            if (fl[a[1]] & BIN) { uint32_t b = at[a[1]] - 1; if (nc != 1 || kd == 'f') return 0; io->bi |= 1u << b; io->bikind[b] = kd; }
            else { uint32_t l = at[a[1]] - 1; if (l >= SPV_VSF_LOC || (io->in >> l & 1)) return 0; io->in |= 1u << l; io->kind[l] = kd; io->ncomp[l] = nc; } }
        i += len; }
    uint32_t id = bound, newbool = 0, pib = id++, hv = id++, wrap = id++;   // the wrapper's: Input bool pointer, gl_HelperInvocation, the function
    if (!tbool) tbool = newbool = id++;
    size_t k = 0; int decos = 0, globs = 0;
    for (int q = 0; q < 5; q++) o[k++] = w[q];
    for (size_t i = 5; i < n; ) {
        uint32_t len = w[i] >> 16, op = w[i] & 0xffff; const uint32_t *a = w + i + 1;
        if (!decos && (op == 71 || op == 72)) { decos = 1; SPV_VSF_PUT(71, hv, 11, 23); }   // BuiltIn HelperInvocation, before the first decoration
        if (!globs && op == 54) { globs = 1;   // the wrapper's types and variable, before the first function
            if (newbool) SPV_VSF_PUT(20, newbool);
            SPV_VSF_PUT(32, pib, 1, tbool); SPV_VSF_PUT(59, pib, hv, 1); }
        if (op == 15) {   // OpEntryPoint Fragment for the wrapper, its interface without the outputs (private now; before SPIR-V 1.4 an interface holds only inputs and outputs)
            size_t h = k; if (k + len + 1 > cap) return 0;
            o[k++] = 0; o[k++] = 4; o[k++] = wrap;
            uint32_t q = 2; for (; q < len - 1; q++) { uint32_t c = a[q]; o[k++] = c; if (!(c & 0xff) || !(c & 0xff00) || !(c & 0xff0000) || !(c & 0xff000000u)) { q++; break; } }   // the name, to the word with its 0 byte
            for (; q < len - 1; q++) { uint32_t v = a[q]; if (v < bound && (fl[v] & OUTVAR) && ver < 0x00010400u) continue; o[k++] = v; }
            o[k++] = hv;
            o[h] = (uint32_t)(k - h) << 16 | 15;
            SPV_VSF_PUT(16, wrap, 7);   // OriginUpperLeft
            i += len; continue; }
        if (op == 16) { i += len; continue; }   // a vertex shader's execution modes (Xfb) mean nothing to a fragment shader
        if (op == 71 && len >= 3 && a[0] < bound) {   // OpDecorate
            if (fl[a[0]] & (OUTVAR | OUTTYPE)) { i += len; continue; }   // an output's (Location, BuiltIn, Block, Xfb…), or its block's
            if ((fl[a[0]] & BIN) && a[1] == 11) { SPV_VSF_PUT(71, a[0], 30, SPV_VSF_LOC + at[a[0]] - 1); if (!(fl[a[0]] & FLAT)) SPV_VSF_PUT(71, a[0], 14); i += len; continue; }   // a built-in: a flat input
            if ((fl[a[0]] & KEEP) && a[1] == 11 && !(fl[a[0]] & FLAT)) { if (k + len > cap) return 0; for (uint32_t q = 0; q < len; q++) o[k++] = w[i + q];   // a subgroup built-in: integer, so flat
                SPV_VSF_PUT(71, a[0], 14); i += len; continue; }
            if ((fl[a[0]] & INVAR) && !(fl[a[0]] & (BIN | KEEP)) && a[1] == 30) { if (k + len > cap) return 0; for (uint32_t q = 0; q < len; q++) o[k++] = w[i + q];   // a vertex input: flat too
                if (!(fl[a[0]] & FLAT)) SPV_VSF_PUT(71, a[0], 14);
                i += len; continue; } }
        if (op == 72 && len >= 3 && a[0] < bound && (fl[a[0]] & OUTTYPE)) { i += len; continue; }   // OpMemberDecorate of an output block (BuiltIn, Invariant, Offset…)
        if (k + len > cap) return 0;
        for (uint32_t q = 0; q < len; q++) o[k++] = w[i + q];
        if (op == 32 && len >= 4 && a[1] == 3) o[k - len + 2] = 6;   // OpTypePointer Output → Private
        if (op == 59 && len >= 4 && a[2] == 3) o[k - len + 3] = 6;   // OpVariable Output → Private
        i += len;
    }
    if (!decos || !globs) return 0;
    // the wrapper: a helper invocation returns, any other calls the shader
    uint32_t lbl = id++, h = id++, lcall = id++, lend = id++, call = id++;
    SPV_VSF_PUT(54, fnres, wrap, 0, fntype); SPV_VSF_PUT(248, lbl); SPV_VSF_PUT(61, tbool, h, hv);
    SPV_VSF_PUT(247, lend, 0); SPV_VSF_PUT(250, h, lend, lcall);   // OpSelectionMerge, OpBranchConditional
    SPV_VSF_PUT(248, lcall); SPV_VSF_PUT(57, fnres, call, entry); SPV_VSF_PUT(249, lend);   // OpFunctionCall, OpBranch
    SPV_VSF_PUT(248, lend); SPV_VSF_PUT0(253); SPV_VSF_PUT0(56);
    o[3] = id;
    return k;
}

// The pass-through vertex shader for io (spv_vsf_fs's), SPIR-V version ver (the shader's, 1.3 at least): o[cap] (1024 words is enough); its word count, or 0
static size_t spv_vsf_vs(const spv_vsf_io *io, uint32_t ver, uint32_t *o, size_t cap) {
    if (ver < 0x00010300u) ver = 0x00010300u;
    uint32_t id = 1, tvoid = id++, tfn = id++, tf = id++, tu = id++, ti = id++, tb = id++, tv4 = id++, tvec[3][5], pin[3][5], pout[3][5];
    uint32_t pc = id++, ppc = id++, pcu = id++, pcv = id++, pinu = id++, pini = id++, pov4 = id++, pof = id++;
    uint32_t vin_[SPV_VSF_LOC], vout[SPV_VSF_LOC], bin[SPV_VSF_NBI], bout[SPV_VSF_NBI], vid = id++, iid = id++, vpos = id++, vps = id++;
    uint32_t cm[5], c1023 = id++, csh = id++, cmax = id++, cinv = id++, cbias = id++, coff = id++, chalf = id++, cone = id++;
    for (int q = 0; q < 5; q++) cm[q] = id++;
    for (int a = 0; a < 3; a++) for (int c = 0; c < 5; c++) tvec[a][c] = pin[a][c] = pout[a][c] = 0;
    for (uint32_t l = 0; l < SPV_VSF_LOC; l++) { vin_[l] = vout[l] = 0;
        if (!(io->in >> l & 1)) continue;
        uint32_t a = io->kind[l] == 'f' ? 0 : io->kind[l] == 'u' ? 1 : 2, c = io->ncomp[l]; if (c < 1 || c > 4) return 0;
        if (c > 1 && !tvec[a][c]) tvec[a][c] = id++;
        if (!pin[a][c]) { pin[a][c] = id++; pout[a][c] = id++; }
        vin_[l] = id++; vout[l] = id++; }
    for (int b = 0; b < SPV_VSF_NBI; b++) { bin[b] = bout[b] = 0; if (io->bi >> b & 1) { bin[b] = b == 0 ? vid : b == 1 ? iid : id++; bout[b] = id++; } }
    int dp = (io->bi & 0x1c) != 0;   // BaseVertex, BaseInstance, DrawIndex: DrawParameters
    size_t k = 0; if (cap < 5) return 0;
    o[k++] = 0x07230203u; o[k++] = ver; o[k++] = 0; o[k++] = 0; o[k++] = 0;
    SPV_VSF_PUT(17, 1);   // OpCapability Shader
    if (dp) SPV_VSF_PUT(17, 4427);   // DrawParameters
    SPV_VSF_PUT(14, 0, 1);   // OpMemoryModel Logical GLSL450
    uint32_t main_ = id++;
    { uint32_t ep[4 + 2 * SPV_VSF_LOC + 2 * SPV_VSF_NBI + 6]; uint32_t ne = 0; ep[ne++] = 0; ep[ne++] = main_; ep[ne++] = 0x6e69616du; ep[ne++] = 0;   // Vertex, "main"
      ep[ne++] = vid; ep[ne++] = iid; ep[ne++] = vpos; ep[ne++] = vps;
      for (uint32_t l = 0; l < SPV_VSF_LOC; l++) if (vin_[l]) { ep[ne++] = vin_[l]; ep[ne++] = vout[l]; }
      for (int b = 0; b < SPV_VSF_NBI; b++) if (bout[b]) { if (b > 1) ep[ne++] = bin[b]; ep[ne++] = bout[b]; }
      if (ver >= 0x00010400u) ep[ne++] = pc;   // 1.4 lists every global the entry point uses
      if (!spv_vsf_emit(o, &k, cap, 15, ep, ne)) return 0; }
    SPV_VSF_PUT(71, vid, 11, 42); SPV_VSF_PUT(71, iid, 11, 43); SPV_VSF_PUT(71, vpos, 11, 0); SPV_VSF_PUT(71, vps, 11, 1);
    for (int b = 2; b < SPV_VSF_NBI; b++) if (bin[b]) SPV_VSF_PUT(71, bin[b], 11, SPV_VSF_BI[b]);
    for (uint32_t l = 0; l < SPV_VSF_LOC; l++) if (vin_[l]) { SPV_VSF_PUT(71, vin_[l], 30, l); SPV_VSF_PUT(71, vout[l], 30, l); }
    for (int b = 0; b < SPV_VSF_NBI; b++) if (bout[b]) SPV_VSF_PUT(71, bout[b], 30, SPV_VSF_LOC + (uint32_t)b);
    SPV_VSF_PUT(71, ppc, 2);   // Block
    for (uint32_t q = 0; q < 5; q++) SPV_VSF_PUT(72, ppc, q, 35, 4 * q);   // Offset
    SPV_VSF_PUT(19, tvoid); SPV_VSF_PUT(33, tfn, tvoid); SPV_VSF_PUT(22, tf, 32); SPV_VSF_PUT(21, tu, 32, 0); SPV_VSF_PUT(21, ti, 32, 1); SPV_VSF_PUT(20, tb); SPV_VSF_PUT(23, tv4, tf, 4);
    const uint32_t scal[3] = {tf, tu, ti};
    for (int a = 0; a < 3; a++) for (int c = 2; c <= 4; c++) if (tvec[a][c] && !(a == 0 && c == 4)) SPV_VSF_PUT(23, tvec[a][c], scal[a], (uint32_t)c);
    SPV_VSF_PUT(30, ppc, tu, tu, tu, tu, tu); SPV_VSF_PUT(32, pcv, 9, ppc); SPV_VSF_PUT(32, pcu, 9, tu);
    SPV_VSF_PUT(32, pini, 1, ti); SPV_VSF_PUT(32, pinu, 1, tu); SPV_VSF_PUT(32, pov4, 3, tv4); SPV_VSF_PUT(32, pof, 3, tf);
    for (int a = 0; a < 3; a++) for (int c = 1; c <= 4; c++) if (pin[a][c]) { uint32_t t = c == 1 ? scal[a] : (a == 0 && c == 4) ? tv4 : tvec[a][c]; SPV_VSF_PUT(32, pin[a][c], 1, t); SPV_VSF_PUT(32, pout[a][c], 3, t); }
    uint32_t pbo[SPV_VSF_NBI]; for (int b = 0; b < SPV_VSF_NBI; b++) pbo[b] = 0;
    uint32_t pboi = 0, pbou = 0; for (int b = 0; b < SPV_VSF_NBI; b++) if (bout[b]) { if (io->bikind[b] == 'u') { if (!pbou) pbou = id++; pbo[b] = pbou; } else { if (!pboi) pboi = id++; pbo[b] = pboi; } }
    if (pboi) SPV_VSF_PUT(32, pboi, 3, ti);
    if (pbou) SPV_VSF_PUT(32, pbou, 3, tu);
    for (uint32_t q = 0; q < 5; q++) SPV_VSF_PUT(43, tu, cm[q], q);
    SPV_VSF_PUT(43, tu, c1023, (1u << SPV_VSF_SHIFT) - 1); SPV_VSF_PUT(43, tu, csh, SPV_VSF_SHIFT); SPV_VSF_PUT(43, tu, cmax, SPV_VSF_MAX);
    SPV_VSF_PUT(43, tf, cinv, 0x3b000000u); SPV_VSF_PUT(43, tf, cbias, 0xbf7fc000u);   // 1/512 and -1023/1024: column c's centre at (2c + 1) / 1024 - 1, exact in float
    SPV_VSF_PUT(43, tf, coff, 0xc0800000u); SPV_VSF_PUT(43, tf, chalf, 0x3f000000u); SPV_VSF_PUT(43, tf, cone, 0x3f800000u);   // -4: off the grid
    SPV_VSF_PUT(59, pcv, pc, 9);
    SPV_VSF_PUT(59, pini, vid, 1); SPV_VSF_PUT(59, pini, iid, 1); SPV_VSF_PUT(59, pov4, vpos, 3); SPV_VSF_PUT(59, pof, vps, 3);
    for (int b = 2; b < SPV_VSF_NBI; b++) if (bin[b]) SPV_VSF_PUT(59, io->bikind[b] == 'u' ? pinu : pini, bin[b], 1);
    for (int b = 0; b < SPV_VSF_NBI; b++) if (bout[b]) SPV_VSF_PUT(59, pbo[b], bout[b], 3);
    for (uint32_t l = 0; l < SPV_VSF_LOC; l++) if (vin_[l]) { uint32_t a = io->kind[l] == 'f' ? 0 : io->kind[l] == 'u' ? 1 : 2, c = io->ncomp[l]; SPV_VSF_PUT(59, pin[a][c], vin_[l], 1); SPV_VSF_PUT(59, pout[a][c], vout[l], 3); }
    // main: n = (i - ibase) × vspan + (v - vbase); off the grid unless n < total and n - lbase < 2^20; then pixel (n - lbase) & 1023, (n - lbase) >> 10
    uint32_t lbl = id++, vi = id++, ii = id++, vu = id++, iu = id++, pcl[5], dv = id++, di = id++, m = id++, nn = id++, ok1 = id++, l2 = id++, ok2 = id++, ok = id++;
    uint32_t col = id++, row = id++, cf = id++, rf = id++, xm = id++, x = id++, ym = id++, y = id++, xs = id++, pos = id++;
    for (int q = 0; q < 5; q++) pcl[q] = id++;
    SPV_VSF_PUT(54, tvoid, main_, 0, tfn); SPV_VSF_PUT(248, lbl);
    SPV_VSF_PUT(61, ti, vi, vid); SPV_VSF_PUT(61, ti, ii, iid); SPV_VSF_PUT(124, tu, vu, vi); SPV_VSF_PUT(124, tu, iu, ii);
    for (int q = 0; q < 5; q++) { uint32_t p = id++; SPV_VSF_PUT(65, pcu, p, pc, cm[q]); SPV_VSF_PUT(61, tu, pcl[q], p); }
    SPV_VSF_PUT(130, tu, dv, vu, pcl[0]); SPV_VSF_PUT(130, tu, di, iu, pcl[2]); SPV_VSF_PUT(132, tu, m, di, pcl[1]); SPV_VSF_PUT(128, tu, nn, m, dv);
    SPV_VSF_PUT(176, tb, ok1, nn, pcl[3]); SPV_VSF_PUT(130, tu, l2, nn, pcl[4]); SPV_VSF_PUT(176, tb, ok2, l2, cmax); SPV_VSF_PUT(167, tb, ok, ok1, ok2);   // OpULessThan, OpLogicalAnd
    SPV_VSF_PUT(199, tu, col, l2, c1023); SPV_VSF_PUT(194, tu, row, l2, csh);
    SPV_VSF_PUT(112, tf, cf, col); SPV_VSF_PUT(112, tf, rf, row);
    SPV_VSF_PUT(133, tf, xm, cf, cinv); SPV_VSF_PUT(129, tf, x, xm, cbias); SPV_VSF_PUT(133, tf, ym, rf, cinv); SPV_VSF_PUT(129, tf, y, ym, cbias);
    SPV_VSF_PUT(169, tf, xs, ok, x, coff);   // OpSelect
    SPV_VSF_PUT(80, tv4, pos, xs, y, chalf, cone); SPV_VSF_PUT(62, vpos, pos); SPV_VSF_PUT(62, vps, cone);
    for (uint32_t l = 0; l < SPV_VSF_LOC; l++) if (vin_[l]) { uint32_t a = io->kind[l] == 'f' ? 0 : io->kind[l] == 'u' ? 1 : 2, c = io->ncomp[l], v = id++;
        uint32_t t = c == 1 ? scal[a] : (a == 0 && c == 4) ? tv4 : tvec[a][c]; SPV_VSF_PUT(61, t, v, vin_[l]); SPV_VSF_PUT(62, vout[l], v); }
    for (int b = 0; b < SPV_VSF_NBI; b++) if (bout[b]) { uint32_t v = id++; SPV_VSF_PUT(61, io->bikind[b] == 'u' ? tu : ti, v, bin[b]); SPV_VSF_PUT(62, bout[b], v); }
    SPV_VSF_PUT0(253); SPV_VSF_PUT0(56);
    o[3] = id;
    return k;
}
