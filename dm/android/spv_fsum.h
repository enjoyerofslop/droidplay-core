// The translator's float atomic adds to one fixed word are summed across the subgroup first, so one invocation of each subgroup makes the add. NVN's RED.F32 / ATOM's
// float add is a compare-and-swap loop on the word (InstEmitMemory.EmitAtomicAddF32: load, add, CAS, again while another invocation changed the word in between), as
// SPIR-V has no float atomic add without an extension. Invocations that add to the same word at once retry each other's CAS: a subgroup of 64 makes up to 64 rounds of
// a load and an atomic each, and the subgroups of a dispatch compete again. Ocarina of Time's scene analysis sums luminance into four words this way (6e28359aa3558f61,
// one workgroup of 256, 295 us of a OnePlus Pad 3's frame) and its probe filter into four more (1bba09ec4bc8ff67, 443 us). Here a loop whose word is a constant element
// of a global buffer, and whose result is not used after it (a RED), gets the subgroup's sum of the values (OpGroupNonUniformFAdd Reduce over the invocations that reach
// it) and runs in the one invocation OpGroupNonUniformElect picks: the same total, in another order of adds, as a float atomic add already is. The elected invocations of a
// dispatch's subgroups still compete, so the word is read once before the loop and each retry starts from the value its compare-and-swap found, instead of a load of its
// own (1bba09ec4bc8ff67, one add per 32-lane warp in 96 workgroups: about 150 us of compare-and-swaps after the sum). A loop of another form, a
// word at a computed element, or a result used later is left as it is. Run after the subgroup rewrites (spv_wide), which would otherwise take the new reduction for a
// 32-lane warp's (gfx_vk_lmod.inc lm_code). Self-contained (no libc) for dm/tools/tests/test_spv_fsum.py.
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { SPV_FSUM_FORM = 1, SPV_FSUM_ROOM = 2 };   // why a module was left: none of its float add loops in the form; out too small
#define SPV_FSUM_MAX 32   // loops rewritten at most in one module
#define SPV_FSUM_EXTRA(nl) (24 + (size_t)(nl) * 48)   // words a rewrite adds at most for nl loops: two capabilities, a constant, a pointer type, and each loop's load, sum,
                                                    // elect, merge, block, local, its first read and stores

typedef struct { size_t pre, head, mlab, add, cas, ld, fb; uint32_t m, m2, var, lty, src, bc, t, v, fty, sum, el, vpos, ve, c, e0; } spv_fsum_loop;   // pre: the preheader's OpBranch; head: the header's OpLoopMerge;
// mlab: the merge's OpLabel; add: the OpFAdd, vpos its value's word (3 or 4); the value: a load (type lty) of the local var the loop doesn't store, or the id src from before
// the loop, through an OpBitcast to float in the loop where bc; fty: the float type; t, v: the preheader's load and bitcast; ld: the loop's load of the word, which then
// reads the local ve instead (0: kept), the word read once in the preheader (chain c, load e0) and each compare-and-swap's result stored there; fb: where its function's
// first block starts (the local is declared there)

// Whether the module may have such loops: a compare-and-swap and a loop
static int spv_fsum_wanted(const uint32_t *w, size_t n) {
    if (n < 5 || w[0] != 0x07230203) return 0;
    int cas = 0, loop = 0;
    for (size_t i = 5; i < n; ) { uint32_t len = w[i] >> 16, op = w[i] & 0xffff; if (!len || i + len > n) return 0;
        if (op == 230) cas = 1; if (op == 246) loop = 1; i += len; }
    return cas && loop;
}

// The module rewritten into o (cap words) → its length; 0 when nothing is done (*why says why: 0 none of the loops found, else SPV_FSUM_*). sc: 3 * bound words of scratch
static size_t spv_fsum(const uint32_t *w, size_t n, uint32_t *sc, uint32_t *o, size_t cap, int *why) {
    *why = 0;
    if (n < 5 || w[0] != 0x07230203 || w[1] < 0x10300) return 0;   // the group operations need SPIR-V 1.3
    uint32_t bound = w[3], *def = sc, *stv = sc + bound, *cls = sc + 2 * (size_t)bound;   // def: each result's word; stv: a Function variable's one store's value (~0: more);
    for (size_t i = 0; i < 3 * (size_t)bound; i++) sc[i] = 0;                               // cls: a variable's storage class + 1, a constant's +0x100 (its value below)
    size_t i_fn = 0, i_cap = 0; uint32_t uty = 0, bty = 0, c3 = 0; int has61 = 0, has63 = 0;
    for (size_t i = 5; i < n; ) { uint32_t len = w[i] >> 16, op = w[i] & 0xffff; const uint32_t *x = w + i; if (!len || i + len > n) return 0;
        if (op == 17 && len == 2) { if (!i_cap) i_cap = i; if (x[1] == 61) has61 = 1; if (x[1] == 63) has63 = 1; }
        if (op == 54 && !i_fn) i_fn = i;
        if (op == 21 && len == 4 && x[2] == 32 && x[3] == 0 && !uty) uty = x[1];
        if (op == 20 && len == 2 && !bty) bty = x[1];
        if (op >= 19 && op <= 39 && len >= 2 && x[1] < bound) def[x[1]] = (uint32_t)i;
        else if (len >= 3 && x[2] < bound && ((op >= 41 && op <= 46) || op == 54 || op == 55 || op == 57 || op == 59 || op == 61 || op == 65 || op == 66 || (op >= 77 && op <= 83) ||
                 (op >= 109 && op <= 202) || (op >= 227 && op <= 242) || op == 245 || (op >= 333 && op <= 366))) def[x[2]] = (uint32_t)i;   // the instructions with a result type and id
        if (op == 59 && len >= 4 && x[2] < bound) cls[x[2]] = x[3] + 1;
        if (op == 43 && len == 4 && x[2] < bound) cls[x[2]] = 0x100;
        if (op == 43 && len == 4 && x[1] == uty && uty && x[3] == 3 && !c3) c3 = x[2];
        if (op == 62 && len >= 3 && x[1] < bound) stv[x[1]] = stv[x[1]] ? ~0u : x[2];
        i += len; }
    if (!i_fn || !uty || !bty || !i_cap) return 0;
    #define FS_OP(id) ((id) < bound && def[id] ? w[def[id]] & 0xffff : 0)
    spv_fsum_loop L[SPV_FSUM_MAX]; int nl = 0, seen = 0;
    for (size_t i = i_fn; i < n; i += w[i] >> 16) {
        if ((w[i] & 0xffff) != 230 || (w[i] >> 16) != 9) continue;   // OpAtomicCompareExchange type result pointer scope eq neq value comparator
        seen = 1;
        if (nl == SPV_FSUM_MAX) break;
        const uint32_t *x = w + i;
        // the loop around it: the nearest OpLoopMerge before, no other loop between, its merge label after
        size_t lm = 0; for (size_t j = i_fn; j < i; j += w[j] >> 16) { uint32_t op = w[j] & 0xffff; if (op == 54) lm = 0; if (op == 246) lm = j; }   // in this function
        if (!lm || (w[lm - 2] & 0xffff) != 248) continue;
        uint32_t m = w[lm + 1];
        size_t mlab = 0; for (size_t j = i; j < n && !mlab; j += w[j] >> 16) { uint32_t op = w[j] & 0xffff; if (op == 246 || op == 56) break; if (op == 248 && w[j + 1] == m) mlab = j; }
        if (!mlab) continue;
        // the word: a chain on a global variable with constant indices
        uint32_t p = x[3]; if (FS_OP(p) != 65) continue;
        const uint32_t *ac = w + def[p]; uint32_t acl = ac[0] >> 16; int ok = acl >= 5 && ac[3] < bound && cls[ac[3]] && def[ac[3]] < i_fn;
        for (uint32_t q = 4; q < acl && ok; q++) ok = ac[q] < bound && cls[ac[q]] == 0x100;
        if (!ok) continue;
        // the new value: an OpFAdd of the old word and the value, through bitcasts and once-stored locals
        #define FS_ROOT(v0, out) do { uint32_t v_ = (v0); for (int k_ = 0; k_ < 8 && v_ < bound; k_++) { uint32_t o_ = FS_OP(v_); \
            if (o_ == 124) { v_ = w[def[v_] + 3]; continue; } \
            if (o_ == 61 && w[def[v_] + 3] < bound && cls[w[def[v_] + 3]] == 7 + 1 && stv[w[def[v_] + 3]] && stv[w[def[v_] + 3]] != ~0u) { v_ = stv[w[def[v_] + 3]]; continue; } break; } (out) = v_; } while (0)
        uint32_t nv, old; FS_ROOT(x[7], nv); FS_ROOT(x[8], old);
        if (FS_OP(nv) != 129) continue;
        size_t add = def[nv]; if (add < lm || add > i) continue;
        uint32_t a = w[add + 3], b = w[add + 4], ra, rb; FS_ROOT(a, ra); FS_ROOT(b, rb);
        uint32_t val, vpos; if (ra == old && rb != old) { val = b; vpos = 4; } else if (rb == old && ra != old) { val = a; vpos = 3; } else continue;
        spv_fsum_loop *l = &L[nl]; l->head = lm; l->mlab = mlab; l->add = add; l->cas = i; l->m = m; l->fty = w[add + 1]; l->vpos = vpos;
        // the value: an id from before the loop, or a load of a local the loop doesn't store, either through a bitcast in the loop
        l->var = 0; l->bc = 0; l->src = val;
        if (def[val] > lm && FS_OP(val) == 124) { l->bc = 1; l->src = w[def[val] + 3]; }
        if (def[l->src] > lm) {
            uint32_t ld = l->src;
            if (FS_OP(ld) != 61 || w[def[ld] + 3] >= bound || cls[w[def[ld] + 3]] != 7 + 1) continue;
            uint32_t var = w[def[ld] + 3]; int st = 0;
            for (size_t j = lm; j < mlab; j += w[j] >> 16) if ((w[j] & 0xffff) == 62 && w[j + 1] == var) st = 1;
            if (st) continue;
            l->var = var; l->lty = w[def[ld] + 1]; }
        // the result: neither the CAS's result nor the locals it and the old word went through are read after the loop (a RED: the value before the add is unused)
        uint32_t res = x[2], vres = 0, vold = 0;
        for (size_t j = i; j < mlab; j += w[j] >> 16) if ((w[j] & 0xffff) == 62 && w[j + 2] == res) vres = w[j + 1];
        { uint32_t v = x[8]; for (int k = 0; k < 8 && v < bound; k++) { uint32_t op = FS_OP(v); if (op == 124) { v = w[def[v] + 3]; continue; } if (op == 61) vold = w[def[v] + 3]; break; } }
        int used = 0;
        for (size_t j = i_fn; j < n && !used; j += w[j] >> 16) { if (j >= lm && j < mlab) continue;
            uint32_t len = w[j] >> 16, op = w[j] & 0xffff;
            for (uint32_t q = 1; q < len; q++) if (w[j + q] == res || (vres && op == 61 && q == 3 && w[j + q] == vres) || (vold && op == 61 && q == 3 && w[j + q] == vold)) used = 1; }
        if (used) continue;
        // the preheader: the block laid out just before the header, ending in OpBranch to it; the merge: no OpPhi, no branch to it from outside the loop
        if ((w[lm - 4] & 0xffff) != 249 || (w[lm - 4] >> 16) != 2 || w[lm - 3] != w[lm - 1]) continue;
        l->pre = lm - 4;
        if ((w[mlab + 2] & 0xffff) == 245) continue;
        int out = 0;
        for (size_t j = i_fn; j < n && !out; j += w[j] >> 16) { if (j >= lm && j < mlab) continue; uint32_t op = w[j] & 0xffff, len = w[j] >> 16;
            if ((op == 249 && w[j + 1] == m) || (op == 250 && (w[j + 2] == m || w[j + 3] == m))) out = 1;
            if (op == 251 && len >= 3) { if (w[j + 2] == m) out = 1; for (uint32_t q = 4; q < len; q += 2) if (w[j + q] == m) out = 1; } }   // OpSwitch selector default (literal label)...
        if (out) continue;
        { int dup = 0; for (int k = 0; k < nl; k++) if (L[k].head == lm) dup = 1; if (dup) continue; }   // one CAS a loop
        // the loop's load of the word (the old value's root): through a chain on the same variable with the same indices, so the word can be read once before the loop
        // and each retry start from the value its compare-and-swap found, instead of a load of its own
        l->ld = 0; l->fb = 0;
        if (FS_OP(old) == 61 && def[old] > lm && def[old] < i && w[def[old] + 1] == x[1] && FS_OP(w[def[old] + 3]) == 65) {
            const uint32_t *lc = w + def[w[def[old] + 3]]; int same = (lc[0] >> 16) == acl && lc[1] == ac[1];
            for (uint32_t q = 3; q < acl && same; q++) same = lc[q] == ac[q];
            size_t fs = 0; for (size_t j = i_fn; j < lm; j += w[j] >> 16) if ((w[j] & 0xffff) == 54) fs = j;
            size_t fb = 0; for (size_t j = fs; j < lm && !fb; j += w[j] >> 16) if ((w[j] & 0xffff) == 248) fb = j;
            if (same && fb) { l->ld = def[old]; l->fb = fb; } }
        nl++;
    }
    if (!nl) { *why = seen ? SPV_FSUM_FORM : 0; return 0; }
    if (cap < n + SPV_FSUM_EXTRA(nl)) { *why = SPV_FSUM_ROOM; return 0; }
    uint32_t nb_ = bound;
    if (!c3) c3 = nb_++;
    for (int k = 0; k < nl; k++) { L[k].m2 = nb_++; L[k].sum = nb_++; L[k].el = nb_++; L[k].t = L[k].var ? nb_++ : L[k].src; L[k].v = L[k].bc ? nb_++ : L[k].t; }
    uint32_t pfu = 0; int anyld = 0; for (int q = 0; q < nl; q++) anyld |= L[q].ld != 0;   // a Function pointer to uint for the locals: the module's, else new
    for (size_t i = 5; i < i_fn && anyld && !pfu; i += w[i] >> 16) if ((w[i] & 0xffff) == 32 && (w[i] >> 16) == 4 && w[i + 2] == 7 && w[i + 3] == uty) pfu = w[i + 1];
    if (anyld && !pfu) pfu = nb_++;
    for (int q = 0; q < nl; q++) if (L[q].ld) { L[q].ve = nb_++; L[q].c = nb_++; L[q].e0 = nb_++; }
    size_t k = 0;
    for (int q = 0; q < 5; q++) o[k++] = w[q];
    for (size_t i = 5; i < n; ) { uint32_t len = w[i] >> 16, op = w[i] & 0xffff; const uint32_t *x = w + i;
        if (i == i_cap) { if (!has61) { o[k++] = 2u << 16 | 17; o[k++] = 61; } if (!has63) { o[k++] = 2u << 16 | 17; o[k++] = 63; } }
        if (i == i_fn && c3 >= bound) { o[k++] = 4u << 16 | 43; o[k++] = uty; o[k++] = c3; o[k++] = 3; }
        if (i == i_fn && anyld && pfu >= bound) { o[k++] = 4u << 16 | 32; o[k++] = pfu; o[k++] = 7; o[k++] = uty; }
        const spv_fsum_loop *l = 0; for (int q = 0; q < nl && !l; q++) if (i >= L[q].pre && i <= L[q].mlab) l = &L[q];
        if (l && i == l->pre) {   // the sum and the election before the loop, and the loop made the elected invocation's
            uint32_t v = l->v;
            if (l->ld) { const uint32_t *ac = w + def[w[l->cas + 3]]; uint32_t acl = ac[0] >> 16;   // the word read once: a chain like the loop's, its load into the local
                o[k++] = acl << 16 | 65; o[k++] = ac[1]; o[k++] = l->c; for (uint32_t q = 3; q < acl; q++) o[k++] = ac[q];
                o[k++] = 4u << 16 | 61; o[k++] = w[l->cas + 1]; o[k++] = l->e0; o[k++] = l->c;
                o[k++] = 3u << 16 | 62; o[k++] = l->ve; o[k++] = l->e0; }
            if (l->var) { o[k++] = 4u << 16 | 61; o[k++] = l->lty; o[k++] = l->t; o[k++] = l->var; }
            if (l->bc) { o[k++] = 4u << 16 | 124; o[k++] = l->fty; o[k++] = v; o[k++] = l->t; }
            o[k++] = 6u << 16 | 350; o[k++] = l->fty; o[k++] = l->sum; o[k++] = c3; o[k++] = 0; o[k++] = v;   // OpGroupNonUniformFAdd Subgroup Reduce
            o[k++] = 4u << 16 | 333; o[k++] = bty; o[k++] = l->el; o[k++] = c3;   // OpGroupNonUniformElect Subgroup
            o[k++] = 3u << 16 | 247; o[k++] = l->m; o[k++] = 0;   // OpSelectionMerge merge None
            o[k++] = 4u << 16 | 250; o[k++] = l->el; o[k++] = x[1]; o[k++] = l->m;
            i += len; continue; }
        if (l && i == l->mlab) { o[k++] = 2u << 16 | 248; o[k++] = l->m2; o[k++] = 2u << 16 | 249; o[k++] = l->m; }   // the loop's own merge, then on to the old one
        if (l && l->ld && i == l->ld) { o[k++] = 4u << 16 | 61; o[k++] = x[1]; o[k++] = x[2]; o[k++] = l->ve; i += len; continue; }   // the word as the local holds it
        for (uint32_t q = 0; q < len; q++) o[k + q] = x[q];
        if (l && i > l->pre && i < l->mlab) {
            if (op == 246) o[k + 1] = l->m2;
            if (op == 249 && x[1] == l->m) o[k + 1] = l->m2;
            if (op == 250) { if (x[2] == l->m) o[k + 2] = l->m2; if (x[3] == l->m) o[k + 3] = l->m2; }
            if (i == l->add) o[k + l->vpos] = l->sum;   // the subgroup's sum where the value was
        }
        k += len; i += len;
        if (l && l->ld && i - len == l->cas) { o[k++] = 3u << 16 | 62; o[k++] = l->ve; o[k++] = x[2]; }   // what the compare-and-swap found: the next try's expected value
        if (op == 248) for (int q = 0; q < nl; q++) if (L[q].ld && L[q].fb == i - len) { o[k++] = 4u << 16 | 59; o[k++] = pfu; o[k++] = L[q].ve; o[k++] = 7; }   // the locals, first in their function
    }
    #undef FS_ROOT
    #undef FS_OP
    o[3] = nb_;
    return k;
}
