// A shader's attribute arrays: the locations they cover, and the arrays split into one Input variable per location. When a shader reads an attribute at an
// index known only at run time (Maxwell's ALD / IPA with a register index), the translator declares all its generic inputs as one array,
// `layout(location = 0) in vec4 _50[16]`, as Retro Studios' engine does in Metroid Prime Remastered to pick a UV set (7,390 fragment shaders) or a vertex stream
// (3,858 vertex shaders) by a uniform.
// - Locations (spv_inarr_io, spv_inarr_mask): the replay took an array's first location only, so a vertex shader reading attributes 1 to 6 through one was
//   given attribute 0 alone (the pipeline key drops attributes the shader doesn't read), and read the rest undefined: the arm cannon lit black on every GPU.
// - Linking (spv_inarr): the stage before a fragment shader writes those locations as separate vec4 outputs, and Vulkan matches two stages' interface variable by
//   variable (Location and type), so the array matches nothing: Qualcomm's Adreno 730 driver (V@0615.98) linked only its first element, and the force fields, which
//   read their second UV set and their vertex colour through it, drew as a jumble of flat-coloured triangles. A vertex shader's input array is split the same way,
//   so each element is one attribute location as the pipeline's vertex input describes it.
// spv_inarr gives each element its own Input variable at its location (with the array's other decorations), turns the array into a Private variable of the same
// id, and fills it from those inputs at the start of the entry point's first block, after its local variables, so every access chain reads the copy. The inputs
// the stage before doesn't write are then dropped by the replay's input variant (spv_drop_inputs), as for any other input. A module that is neither a vertex nor a
// fragment shader, or has no such array, is left alone, and so is an array used other than through access chains and loads. Self-contained (no libc) for
// dm/tools/tests/test_spv_inarr.py.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "spv_lit.h"

#define SPV_INARR_MAX 4u     // arrays split at most
#define SPV_INARR_LEN 32u    // elements an array may have (a location each, inside the replay's 32-bit location masks)
#define SPV_INARR_PTEES 8u   // distinct pointee types of the access chains into them
#define SPV_INARR_SCRATCH(bound) (8u * (size_t)(bound))   // scratch words spv_inarr, spv_inarr_io and spv_inarr_mask need

// whether the module's entry point is a vertex or fragment shader (the replay's pre-check: the entry points come right after the capabilities, extensions and memory model)
static int spv_inarr_wanted(const uint32_t *w, size_t n) {
    for (size_t i = 5; i < n; ) { uint32_t len = w[i] >> 16, op = w[i] & 0xffff; if (!len || i + len > n) return 0;
        if (op == 15 && len >= 3) return w[i + 1] == 0 || w[i + 1] == 4;   // OpEntryPoint: execution model Vertex or Fragment
        if (op != 17 && op != 10 && op != 11 && op != 14) return 0;   // past the capabilities, extensions, imports and the memory model
        i += len; }
    return 0;
}

typedef struct { uint32_t var, arr, elem, len, loc, decw, ptin, ptpriv; } spv_inarr_a;   // decw: the words of the array's decorations; ptin / ptpriv: its new pointer types
typedef struct {
    spv_inarr_a a[SPV_INARR_MAX]; uint32_t na, entry, version, ptee[SPV_INARR_PTEES], ptpee[SPV_INARR_PTEES], nptee;
    size_t entry_at, fn_at, at;   // the OpEntryPoint, the first OpFunction (where the new declarations go) and where the copies go (in the entry point's first block, after its OpVariables)
} spv_inarr_t;

// The arrays to split, in *t → how many (0: none, or the module can't be followed). s: SPV_INARR_SCRATCH(bound) words
static uint32_t spv_inarr_scan(const uint32_t *w, size_t n, uint32_t *s, spv_inarr_t *t) {
    t->na = t->nptee = 0; t->entry = 0; t->entry_at = t->fn_at = t->at = 0;
    if (n < 5 || w[0] != 0x07230203u || w[3] > 0x3ffffff0u) return 0;
    uint32_t bound = w[3]; t->version = w[1];
    uint32_t *pst = s, *ppt = s + bound, *ael = s + 2 * bound, *aln = s + 3 * bound, *cval = s + 4 * bound, *locv = s + 5 * bound, *info = s + 6 * bound, *decw = s + 7 * bound;   // info bits below
    enum { ONE_LOC = 1, HAS_LOC = 2, BUILTIN = 4, CONST32 = 8, COMPONENT = 16, ARRAY = 32, CAND = 64, BAD = 128 };
    for (size_t k = 0; k < SPV_INARR_SCRATCH(bound); k++) s[k] = 0;
    uint32_t nentry = 0, model = ~0u; int in_fn = 0, in_entry = 0, seen_label = 0;
    for (size_t i = 5; i < n; ) {
        uint32_t len = w[i] >> 16, op = w[i] & 0xffff; if (!len || i + len > n) return 0;
        const uint32_t *a = w + i + 1;
        if (op == 15 && len >= 3) { nentry++; model = a[0]; t->entry = a[1]; t->entry_at = i; }
        else if (op == 71 && len >= 3 && a[0] < bound) {   // OpDecorate
            if (a[1] == 30 && len >= 4) { info[a[0]] |= HAS_LOC; locv[a[0]] = a[2]; }   // Location
            else if (a[1] == 11) info[a[0]] |= BUILTIN; else if (a[1] == 31) info[a[0]] |= COMPONENT;
            decw[a[0]] += len;
        } else if ((op == 74 || op == 332 || op == 5632) && len >= 2) { for (uint32_t q = 1; q < len; q++) if (a[q - 1] < bound) info[a[q - 1]] |= BAD; }   // decoration groups, OpDecorateId / String: not moved
        else if ((op == 21 || op == 22) && len >= 3 && a[0] < bound && a[1] == 32) info[a[0]] |= ONE_LOC;   // OpTypeInt / OpTypeFloat 32
        else if (op == 23 && len == 4 && a[0] < bound && a[1] < bound && (info[a[1]] & ONE_LOC) && a[2] <= 4) info[a[0]] |= ONE_LOC;   // OpTypeVector of them
        else if (op == 28 && len == 4 && a[0] < bound) { info[a[0]] |= ARRAY; ael[a[0]] = a[1]; aln[a[0]] = a[2]; }   // OpTypeArray
        else if (op == 43 && len == 4 && a[1] < bound) { info[a[1]] |= CONST32; cval[a[1]] = a[2]; }   // OpConstant (one word)
        else if (op == 32 && len == 4 && a[0] < bound) { pst[a[0]] = a[1] + 1; ppt[a[0]] = a[2]; }   // OpTypePointer
        else if (op == 59 && len >= 4 && !in_fn && a[2] == 1 && a[0] < bound && a[1] < bound && pst[a[0]] == 2 && ppt[a[0]] < bound && (info[ppt[a[0]]] & ARRAY)) {   // OpVariable Input of an array
            uint32_t v = a[1], at = ppt[a[0]], el = ael[at], ln = aln[at];
            if ((info[v] & (HAS_LOC | BUILTIN | COMPONENT | BAD)) == HAS_LOC && el < bound && (info[el] & ONE_LOC) && ln < bound && (info[ln] & CONST32)
                && cval[ln] >= 1 && cval[ln] <= SPV_INARR_LEN && locv[v] + cval[ln] <= 32 && t->na < SPV_INARR_MAX) {
                spv_inarr_a *c = &t->a[t->na++]; c->var = v; c->arr = at; c->elem = el; c->len = cval[ln]; c->loc = locv[v]; c->decw = decw[v]; info[v] |= CAND;
            }
        } else if (op == 54 && len >= 5) { if (!t->fn_at) t->fn_at = i; in_fn = 1; in_entry = a[1] == t->entry; seen_label = 0; }   // OpFunction
        else if (op == 56) in_fn = in_entry = 0;   // OpFunctionEnd
        else if (in_entry && op == 248 && !seen_label) { seen_label = 1; t->at = i + len; }   // the entry point's first OpLabel
        else if (in_entry && seen_label && t->at == i && op == 59) t->at = i + len;   // its leading OpVariables stay first
        i += len;
    }
    if (nentry != 1 || (model != 0 && model != 4) || !t->na || !t->at || !t->fn_at) { t->na = 0; return 0; }
    // their uses in the functions: only the base of an access chain (whose pointee is noted) or a load's pointer; anything else leaves that array as it is
    in_fn = 0;
    for (size_t i = 5; i < n; ) {
        uint32_t len = w[i] >> 16, op = w[i] & 0xffff; const uint32_t *a = w + i + 1;
        if (op == 54) in_fn = 1;
        if (in_fn) for (uint32_t q = 1; q < len; q++) {
            uint32_t id = a[q - 1]; if (id >= bound || !(info[id] & CAND) || spv_lit(op, q)) continue;
            if ((op == 65 || op == 66) && q == 3 && len >= 5 && a[0] < bound && pst[a[0]] == 2) {   // OpAccessChain / OpInBoundsAccessChain type result base indices...
                uint32_t pe = ppt[a[0]], k = 0; while (k < t->nptee && t->ptee[k] != pe) k++;
                if (k == t->nptee) { if (k == SPV_INARR_PTEES) { info[id] |= BAD; continue; } t->ptee[t->nptee++] = pe; }
            } else if (!(op == 61 && q == 3)) info[id] |= BAD;   // OpLoad type result pointer
        }
        i += len;
    }
    uint32_t k = 0;
    for (uint32_t c = 0; c < t->na; c++) if (!(info[t->a[c].var] & BAD)) t->a[k++] = t->a[c];
    t->na = k;
    return k;
}

// The locations the module's user-defined Input and Output variables (a Location) cover → *in, *out (bit n: location n; both 0 when the module can't be followed).
// An array of 32-bit scalars or vectors covers a location per element where its stage takes one attribute per element and the array is read or written whole:
// a vertex shader's inputs (each element an attribute the pipeline's vertex input must give) and outputs, a tessellation evaluation or geometry shader's outputs.
// A fragment shader's input array covers its first location here, as the input variant (spv_drop_inputs) can drop it only whole; spv_inarr_mask gives the
// elements of the arrays spv_inarr splits. Elsewhere (a geometry or tessellation shader's per-vertex inputs, a tessellation control or mesh shader's outputs)
// the outer array is the vertex's, and a variable covers its first location only, as any other. s: SPV_INARR_SCRATCH(bound) words
static void spv_inarr_io(const uint32_t *w, size_t n, uint32_t *s, uint32_t *in, uint32_t *out) {
    *in = *out = 0;
    if (n < 5 || w[0] != 0x07230203u || w[3] > 0x3ffffff0u) return;
    uint32_t bound = w[3], *locv = s, *ppt = s + bound, *ael = s + 2 * bound, *aln = s + 3 * bound, *cval = s + 4 * bound, *info = s + 5 * bound, mi = 0, mo = 0, model = ~0u;
    enum { ONE_LOC = 1, HAS_LOC = 2, CONST32 = 4, ARRAY = 8 };
    for (size_t k = 0; k < 6 * (size_t)bound; k++) s[k] = 0;
    for (size_t i = 5; i < n; ) {   // in the module's order: the entry point, then the decorations, then the types, constants and variables
        uint32_t len = w[i] >> 16, op = w[i] & 0xffff; if (!len || i + len > n) return;
        const uint32_t *a = w + i + 1;
        if (op == 15 && len >= 3 && model == ~0u) model = a[0];   // OpEntryPoint (the replay's modules have one)
        else if (op == 71 && len >= 4 && a[0] < bound && a[1] == 30) { info[a[0]] |= HAS_LOC; locv[a[0]] = a[2]; }   // OpDecorate Location
        else if ((op == 21 || op == 22) && len >= 3 && a[0] < bound && a[1] == 32) info[a[0]] |= ONE_LOC;   // OpTypeInt / OpTypeFloat 32
        else if (op == 23 && len == 4 && a[0] < bound && a[1] < bound && (info[a[1]] & ONE_LOC) && a[2] <= 4) info[a[0]] |= ONE_LOC;   // OpTypeVector of them
        else if (op == 28 && len == 4 && a[0] < bound) { info[a[0]] |= ARRAY; ael[a[0]] = a[1]; aln[a[0]] = a[2]; }   // OpTypeArray
        else if (op == 43 && len == 4 && a[1] < bound) { info[a[1]] |= CONST32; cval[a[1]] = a[2]; }   // OpConstant (one word)
        else if (op == 32 && len == 4 && a[0] < bound) ppt[a[0]] = a[2];   // OpTypePointer
        else if (op == 59 && len >= 4 && a[1] < bound && (a[2] == 1 || a[2] == 3) && (info[a[1]] & HAS_LOC) && locv[a[1]] < 32) {   // OpVariable Input / Output
            uint32_t pt = a[0] < bound ? ppt[a[0]] : 0, cover = 1;
            int per_attr = a[2] == 1 ? model == 0 : model == 0 || model == 2 || model == 3;
            if (per_attr && pt && pt < bound && (info[pt] & ARRAY) && ael[pt] < bound && (info[ael[pt]] & ONE_LOC) && aln[pt] < bound && (info[aln[pt]] & CONST32) && cval[aln[pt]])
                cover = cval[aln[pt]];
            uint32_t m = (cover >= 32 ? ~0u : (1u << cover) - 1) << locv[a[1]];
            if (a[2] == 1) mi |= m; else mo |= m;
        }
        i += len;
    }
    *in = mi; *out = mo;
}

// the locations the arrays spv_inarr splits cover (with spv_inarr_io's, a split fragment shader's input mask); s: SPV_INARR_SCRATCH(bound) words
static uint32_t spv_inarr_mask(const uint32_t *w, size_t n, uint32_t *s) {
    spv_inarr_t t; uint32_t m = 0;
    if (!spv_inarr_scan(w, n, s, &t)) return 0;
    for (uint32_t c = 0; c < t.na; c++) m |= (t.a[c].len >= 32 ? ~0u : ((1u << t.a[c].len) - 1)) << t.a[c].loc;
    return m;
}

// words spv_inarr writes at most for the module (0: nothing to do); s: SPV_INARR_SCRATCH(bound) words
static size_t spv_inarr_need(const uint32_t *w, size_t n, uint32_t *s) {
    spv_inarr_t t; if (!spv_inarr_scan(w, n, s, &t)) return 0;
    size_t need = n + 4 * (size_t)t.nptee;
    for (uint32_t c = 0; c < t.na; c++) need += 8 + (size_t)t.a[c].len * (1 + 4 + 4 + 1 + t.a[c].decw) + 3 + 3;   // types; per element: interface, variable, load, constituent, decorations; construct, store
    return need;
}

// The module rewritten into o (cap words) → its length; 0 when nothing is done (not a fragment shader, no array to split, a malformed module, o too small:
// spv_inarr_need words). s: SPV_INARR_SCRATCH(bound) words
static size_t spv_inarr(const uint32_t *w, size_t n, uint32_t *s, uint32_t *o, size_t cap) {
    spv_inarr_t t; if (!spv_inarr_scan(w, n, s, &t)) return 0;
    uint32_t bound = w[3], *ppt = s + bound, nb = bound;
    for (uint32_t c = 0; c < t.na; c++) { t.a[c].ptin = nb++; t.a[c].ptpriv = nb++; }
    for (uint32_t k = 0; k < t.nptee; k++) t.ptpee[k] = nb++;
    uint32_t elem0[SPV_INARR_MAX], load0[SPV_INARR_MAX], comp[SPV_INARR_MAX];   // each array's first new variable and first load (the rest follow), its composite
    for (uint32_t c = 0; c < t.na; c++) { elem0[c] = nb; nb += t.a[c].len; load0[c] = nb; nb += t.a[c].len; comp[c] = nb++; }
    size_t k = 0;
#define PUT(x) do { if (k >= cap) return 0; o[k++] = (x); } while (0)
    for (uint32_t q = 0; q < 5; q++) PUT(w[q]);
    for (size_t i = 5; i < n; ) {
        uint32_t len = w[i] >> 16, op = w[i] & 0xffff; const uint32_t *a = w + i + 1;
        int cand = -1;
        if (i == t.fn_at) {   // before the first function, where every type is declared: the new pointer types, then each array as Private and its inputs
            for (uint32_t c = 0; c < t.na; c++) { PUT(4u << 16 | 32); PUT(t.a[c].ptin); PUT(1); PUT(t.a[c].elem); PUT(4u << 16 | 32); PUT(t.a[c].ptpriv); PUT(6); PUT(t.a[c].arr); }   // OpTypePointer Input element, Private array
            for (uint32_t p = 0; p < t.nptee; p++) { PUT(4u << 16 | 32); PUT(t.ptpee[p]); PUT(6); PUT(t.ptee[p]); }   // OpTypePointer Private, the chains' pointees
            for (uint32_t c = 0; c < t.na; c++) {
                PUT(4u << 16 | 59); PUT(t.a[c].ptpriv); PUT(t.a[c].var); PUT(6);   // OpVariable Private
                for (uint32_t e = 0; e < t.a[c].len; e++) { PUT(4u << 16 | 59); PUT(t.a[c].ptin); PUT(elem0[c] + e); PUT(1); }   // OpVariable Input
            }
        }
        if (i == t.at)   // the copies: each array built from its inputs and stored whole
            for (uint32_t c = 0; c < t.na; c++) {
                for (uint32_t e = 0; e < t.a[c].len; e++) { PUT(4u << 16 | 61); PUT(t.a[c].elem); PUT(load0[c] + e); PUT(elem0[c] + e); }   // OpLoad
                PUT((3u + t.a[c].len) << 16 | 80); PUT(t.a[c].arr); PUT(comp[c]); for (uint32_t e = 0; e < t.a[c].len; e++) PUT(load0[c] + e);   // OpCompositeConstruct
                PUT(3u << 16 | 62); PUT(t.a[c].var); PUT(comp[c]);   // OpStore
            }
        if (op == 15 && i == t.entry_at) {   // OpEntryPoint model id "name" interface...: the arrays' inputs instead of the arrays (which stay listed from SPIR-V 1.4 on, as Private)
            size_t s0 = i + 3; while (s0 < i + len) { uint32_t x = w[s0++]; if (!(x & 0xff) || !(x & 0xff00) || !(x & 0xff0000) || !(x & 0xff000000)) break; }
            size_t head = k; for (size_t q = i; q < s0; q++) PUT(w[q]);
            for (size_t q = s0; q < i + len; q++) {
                int is = 0; for (uint32_t c = 0; c < t.na; c++) is |= w[q] == t.a[c].var;
                if (!is || t.version >= 0x10400u) PUT(w[q]);
            }
            for (uint32_t c = 0; c < t.na; c++) for (uint32_t e = 0; e < t.a[c].len; e++) PUT(elem0[c] + e);
            o[head] = (uint32_t)(k - head) << 16 | 15; i += len; continue;
        }
        for (uint32_t c = 0; c < t.na; c++) if ((op == 71 && len >= 3 && a[0] == t.a[c].var) || (op == 59 && len >= 4 && a[1] == t.a[c].var)) cand = (int)c;
        if (cand >= 0 && op == 71) {   // the array's decoration, on each of its inputs: Location one apart from its own
            const spv_inarr_a *c = &t.a[cand];
            for (uint32_t e = 0; e < c->len; e++) { PUT(len << 16 | 71); PUT(elem0[cand] + e); for (uint32_t q = 2; q < len; q++) PUT(a[q - 1] + (a[1] == 30 && q == 3 ? e : 0)); }
            i += len; continue;
        }
        if (cand >= 0) { i += len; continue; }   // its variable: declared again before the first function
        if ((op == 65 || op == 66) && len >= 4) {   // an access chain into an array: its result a Private pointer
            int into = 0; for (uint32_t c = 0; c < t.na; c++) into |= a[2] == t.a[c].var;
            if (into) {
                uint32_t p = 0; while (p < t.nptee && t.ptee[p] != ppt[a[0]]) p++;
                if (p == t.nptee) return 0;
                PUT(w[i]); PUT(t.ptpee[p]); for (uint32_t q = 2; q < len; q++) PUT(a[q - 1]);
                i += len; continue;
            }
        }
        for (uint32_t q = 0; q < len; q++) PUT(w[i + q]);
        i += len;
    }
#undef PUT
    o[3] = nb;
    return k;
}
