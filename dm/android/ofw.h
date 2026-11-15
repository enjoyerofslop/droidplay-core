// Order-free storage buffer writes (gfx_vk_draw.inc ofw_bind): a binding a shader writes only in a way whose result does not depend on the order the writes run in.
// - Every store through it stores the same 32-bit constant, and nothing else touches it. Tears of the Kingdom's fragment shaders store 1 at the cell of a 131,584-byte
//   grid under each pixel, from hundreds of draws a frame.
// - Every access is the same commutative atomic (add, subtract, increment or decrement; signed or unsigned min or max; and, or, xor) on 32 bits, and no one reads the
//   old value it returns (the translator stores it in a local that is never loaded). Tears of the Kingdom's tile depth bounds: atomicMin and atomicMax.
// A draw's copy of such a binding then needs nothing of guest memory: it starts as the operation's identity (ofw_identity), and when its chunk completes each word the
// GPU changed is combined with guest memory by the same operation (ofw_land). What earlier write-backs land and what the draws wrote combine as they would in the
// console's one buffer, in whatever order the draws ran, so the draw neither waits for pending writes to land nor needs a copy of its own: draws of one chunk share one.
// Self-contained (no libc): dm/tools/tests/test_ofw.py builds it alone and runs it on modules assembled by spirv-as and over a game's shaders.
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { OFW_NONE, OFW_STORE, OFW_ADD, OFW_UMIN, OFW_UMAX, OFW_SMIN, OFW_SMAX, OFW_AND, OFW_OR, OFW_XOR };
typedef struct { uint32_t binding; uint32_t cls; uint32_t k; } ofw_bind_t;   // cls: OFW_*; k: the constant an OFW_STORE binding stores

// The words of an instruction (length len, opcode op) that may name a pointer or a value: [*a, *e). Literal operands are left out, so a literal that happens to equal
// a variable's id (GLSL.std.450's FMin is 37, a common id for the first variables) is not taken for a use of it; instructions that take no pointer at all (debug,
// annotations, types, constants, variables, control flow, image operations) give none
static inline void ofw_ids(uint32_t len, uint32_t op, uint32_t *a, uint32_t *e) {
    *a = 1; *e = len;
    if (op <= 11 || (op >= 14 && op <= 17) || (op >= 19 && op <= 52) || op == 59 || (op >= 71 && op <= 75) || op == 330 || op == 332 || op == 5632 || op == 5633
        || (op >= 86 && op <= 107) || (op >= 246 && op <= 249) || op == 252 || op == 253 || op == 254) { *e = 0; return; }   // (OpLoopMerge, OpSelectionMerge, OpLabel, OpBranch, OpKill, OpReturn, OpReturnValue's value is checked below)
    if (op == 12) *a = 5;   // OpExtInst: the set and the instruction number before the operands
    else if (op == 79) *e = len < 5 ? len : 5;   // OpVectorShuffle: the two vectors, then component literals
    else if (op == 81) *e = len < 4 ? len : 4;   // OpCompositeExtract: the composite, then index literals
    else if (op == 82) *e = len < 5 ? len : 5;   // OpCompositeInsert: the object and the composite
    else if (op == 250) *e = len < 2 ? len : 2;   // OpBranchConditional: the condition (labels and weights follow)
    else if (op == 251) *e = len < 2 ? len : 2;   // OpSwitch: the selector (literal and label pairs follow)
}

// The storage buffer variables of module w[n] and how each is written. scratch: 6 × bound words (w[3]). out[cap]: one entry per storage buffer variable with a Binding
// decoration. → the entries written (OFW_NONE: read, or written some other way, or not written at all)
static int ofw_scan(const uint32_t *w, size_t n, uint32_t *scratch, ofw_bind_t *out, int cap) {
    enum { T32 = 1, CONST = 2, BB = 4, SBPTR = 8, BIND = 16, LOADED = 32, ARES = 64 };   // info bits
    enum { CLS = 15, BAD = 16, STORED = 32, ATOM = 64 };   // st bits: the class seen so far, a write that is not order-free (or a read), a store seen, an atomic seen
    if (n < 5 || w[0] != 0x07230203u) return 0;
    uint32_t bound = w[3];
    uint32_t *root = scratch, *froot = scratch + bound, *info = scratch + 2 * bound, *val = scratch + 3 * bound, *st = scratch + 4 * bound, *kv = scratch + 5 * bound;
    for (size_t i = 0; i < 6 * (size_t)bound; i++) scratch[i] = 0;
#define ID(x) ((x) < bound)
#define LEN(i) (w[i] >> 16)
#define BADR(r) (st[r] |= BAD)
    for (size_t i = 5; i < n; ) {   // decorations, types, constants, variables
        uint32_t len = LEN(i), op = w[i] & 0xffff;
        if (!len || i + len > n) return 0;
        if ((op == 21 || op == 22) && len > 2 && ID(w[i + 1]) && w[i + 2] == 32) info[w[i + 1]] |= T32;   // OpTypeInt / OpTypeFloat 32
        if (op == 43 && len == 4 && ID(w[i + 1]) && ID(w[i + 2]) && (info[w[i + 1]] & T32)) { info[w[i + 2]] |= CONST; val[w[i + 2]] = w[i + 3]; }   // OpConstant, 32 bits
        if (op == 71 && len > 2 && ID(w[i + 1])) {   // OpDecorate
            if (w[i + 2] == 3) info[w[i + 1]] |= BB;   // BufferBlock
            if (w[i + 2] == 33 && len > 3) { info[w[i + 1]] |= BIND; val[w[i + 1]] = w[i + 3]; }   // Binding (val: a constant's value, a variable's binding, an atomic result's root)
        }
        if (op == 32 && len > 3 && ID(w[i + 1]) && ID(w[i + 3]) && (w[i + 2] == 12 || (w[i + 2] == 2 && (info[w[i + 3]] & BB)))) info[w[i + 1]] |= SBPTR;   // StorageBuffer, or Uniform with BufferBlock
        if (op == 59 && len > 3 && ID(w[i + 1]) && ID(w[i + 2])) {   // OpVariable
            if (info[w[i + 1]] & SBPTR) root[w[i + 2]] = w[i + 2];
            if (w[i + 3] == 7) froot[w[i + 2]] = w[i + 2];   // Function
        }
        i += len;
    }
    for (int changed = 1, pass = 0; changed && pass < 16; pass++) {   // pointers derived from them: access chains and OpCopyObject (an OpPhi forward reference: another pass)
        changed = 0;
        for (size_t i = 5; i < n; i += LEN(i)) {
            uint32_t op = w[i] & 0xffff;
            if ((op == 65 || op == 66 || op == 67 || op == 83) && LEN(i) > 3 && ID(w[i + 2]) && ID(w[i + 3])) {
                uint32_t to = w[i + 2], from = w[i + 3];
                if (root[from] && !root[to]) { root[to] = root[from]; changed = 1; }
                if (froot[from] && !froot[to]) { froot[to] = froot[from]; changed = 1; }
            }
        }
    }
    for (size_t i = 5; i < n; i += LEN(i)) {   // the locals that are read (an atomic's old value stored in one that is never read is unused)
        uint32_t len = LEN(i), op = w[i] & 0xffff;
        if (op == 61 && len > 3 && ID(w[i + 3]) && froot[w[i + 3]]) info[froot[w[i + 3]]] |= LOADED;   // OpLoad
        if ((op == 63 || op == 64) && len > 2 && ID(w[i + 2]) && froot[w[i + 2]]) info[froot[w[i + 2]]] |= LOADED;   // OpCopyMemory(Sized) from one
        if (op == 57) for (uint32_t a = 4; a < len; a++) if (ID(w[i + a]) && froot[w[i + a]]) info[froot[w[i + a]]] |= LOADED;   // passed to a function
    }
    for (size_t i = 5; i < n; i += LEN(i)) {   // each use of a storage buffer pointer
        uint32_t len = LEN(i), op = w[i] & 0xffff;
        uint32_t a0, a1; ofw_ids(len, op, &a0, &a1);
        if (a1 <= a0) continue;   // takes no pointer
        if (op == 65 || op == 66 || op == 67 || op == 83) continue;   // a derivation (above)
        if (op == 61) { if (len > 3 && ID(w[i + 3]) && root[w[i + 3]]) BADR(root[w[i + 3]]); continue; }   // OpLoad: read
        if (op == 62 && len > 2) {   // OpStore
            uint32_t p = w[i + 1], o = w[i + 2];
            if (ID(o) && root[o]) BADR(root[o]);   // the pointer itself stored: escapes
            if (ID(p) && root[p]) { uint32_t r = root[p];
                if (!ID(o) || !(info[o] & CONST) || ((st[r] & STORED) && kv[r] != val[o])) BADR(r);   // not a constant, or another one
                else { st[r] |= STORED; kv[r] = val[o]; } }
            continue;
        }
        if (op >= 232 && op <= 242 && len > 3 && ID(w[i + 3]) && root[w[i + 3]]) {   // the commutative atomics: IIncrement, IDecrement, IAdd, ISub, SMin, UMin, SMax, UMax, And, Or, Xor
            uint32_t r = root[w[i + 3]], c = op <= 235 ? OFW_ADD : op == 236 ? OFW_SMIN : op == 237 ? OFW_UMIN : op == 238 ? OFW_SMAX : op == 239 ? OFW_UMAX : op == 240 ? OFW_AND : op == 241 ? OFW_OR : OFW_XOR;
            if (!ID(w[i + 1]) || !(info[w[i + 1]] & T32) || !ID(w[i + 2])) { BADR(r); continue; }
            if ((st[r] & ATOM) && (st[r] & CLS) != c) BADR(r);
            st[r] = (st[r] & ~(uint32_t)CLS) | ATOM | c;
            for (uint32_t a = 4; a < len; a++) if (ID(w[i + a]) && root[w[i + a]]) BADR(root[w[i + a]]);
            info[w[i + 2]] |= ARES; val[w[i + 2]] = r;   // its old value: unused, checked below
            continue;
        }
        for (uint32_t a = a0; a < a1; a++) if (ID(w[i + a]) && root[w[i + a]]) BADR(root[w[i + a]]);   // any other use (another atomic, a call, a select, a conversion): not order-free
    }
    for (size_t i = 5; i < n; i += LEN(i)) {   // atomics' old values: used unless only stored in a local never read
        uint32_t len = LEN(i), op = w[i] & 0xffff;
        uint32_t a0, a1; ofw_ids(len, op, &a0, &a1);
        if (op == 254) { a0 = 1; a1 = len; }   // OpReturnValue: its value
        int def = op >= 232 && op <= 242 && len > 2 && ID(w[i + 2]) && (info[w[i + 2]] & ARES);
        for (uint32_t a = a0; a < a1; a++) { uint32_t x = w[i + a];
            if (!ID(x) || !(info[x] & ARES) || (def && a == 2)) continue;
            if (op == 62 && a == 2 && ID(w[i + 1]) && froot[w[i + 1]] && !(info[froot[w[i + 1]]] & LOADED)) continue;   // stored in a local never read
            BADR(val[x]); }
    }
    int k = 0;
    for (uint32_t v = 1; v < bound && k < cap; v++) if (root[v] == v && (info[v] & BIND)) {
        uint32_t s = st[v], c = OFW_NONE, kk = 0;
        if (!(s & BAD) && (s & STORED) && !(s & ATOM)) { c = OFW_STORE; kk = kv[v]; }
        else if (!(s & BAD) && (s & ATOM) && !(s & STORED)) c = s & CLS;
        out[k++] = (ofw_bind_t){val[v], c, kk};
    }
#undef ID
#undef LEN
#undef BADR
    return k;
}

// The word an order-free copy starts as: the operation leaves guest memory as it is when combined with it (a store's: any word but the constant)
static inline uint32_t ofw_identity(uint32_t cls, uint32_t k) {
    switch (cls) { case OFW_STORE: return ~k; case OFW_UMIN: case OFW_AND: return 0xffffffffu; case OFW_SMIN: return 0x7fffffffu; case OFW_SMAX: return 0x80000000u; default: return 0; }
}
// The words of copy c[0, nw) the GPU changed, as a span [*i0, *i1]: 0 when none
static inline int ofw_span(uint32_t cls, uint32_t k, const uint32_t *c, size_t nw, size_t *i0, size_t *i1) {
    uint32_t id = ofw_identity(cls, k); size_t a = 0, e = nw;
    while (a < nw && c[a] == id) a++;
    if (a == nw) return 0;
    while (e > a && c[e - 1] == id) e--;
    *i0 = a; *i1 = e - 1; return 1;
}
// Copy c[i0, i1] lands on guest words g[i0, i1]: each word the GPU changed combined with the guest's by the operation. → the words written
static inline size_t ofw_land(uint32_t cls, uint32_t k, const uint32_t *c, volatile uint32_t *g, size_t i0, size_t i1) {
    uint32_t id = ofw_identity(cls, k); size_t n = 0;
    for (size_t i = i0; i <= i1; i++) { uint32_t v = c[i], x; if (v == id) continue;
        switch (cls) {
        case OFW_STORE: x = k; break;
        case OFW_ADD: x = g[i] + v; break;
        case OFW_UMIN: x = g[i] < v ? g[i] : v; break;
        case OFW_UMAX: x = g[i] > v ? g[i] : v; break;
        case OFW_SMIN: x = (int32_t)g[i] < (int32_t)v ? g[i] : v; break;
        case OFW_SMAX: x = (int32_t)g[i] > (int32_t)v ? g[i] : v; break;
        case OFW_AND: x = g[i] & v; break;
        case OFW_OR: x = g[i] | v; break;
        case OFW_XOR: x = g[i] ^ v; break;
        default: continue; }
        g[i] = x; n++; }
    return n;
}
