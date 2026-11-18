// A task / mesh kernel's warp as its whole workgroup on a GPU whose subgroups are narrower than Maxwell's 32 lanes: 16 on the Pixel 9 Pro XL's Mali-G715, which
// VK_EXT_subgroup_size_control cannot raise. The compute form of an NVN2 task or mesh shader (spv_meshemu.py) runs one 32-invocation workgroup per Maxwell warp,
// and the translated shader works out which vertex, primitive or meshlet each lane handles from its lane ID, then packs its outputs with ballots, lane masks and
// shuffles. On 16 lanes the second half of the warp got lane IDs 0-15 again, and its ballots and masks saw only its own half, so it did the first half's work
// over again and vertices 16-31 of each workgroup were never written: Ocarina of Time's plants on a Pixel drew black spikes from the screen's centre, and with
// spv_meshemu's in-place attribute stores (version 3) the stale vertices blew the whole picture to white. spv_narrow.h's "each subgroup a warp of its own" fits
// a game's whole-warp reductions, not this. Here every lane number is the warp's:
// - The lane ID is the invocation's index in the workgroup (LocalInvocationIndex), and each lane mask (Eq, Ge, Gt, Le, Lt) is made from it at the entry point.
// - Where every invocation of the workgroup reaches the site (spv_wg32_div), a ballot is the subgroups' own words (an OR over the subgroup of each lane's bit at
//   its index) put together through workgroup memory between two barriers: each subgroup stores its word at its SubgroupId, then every lane ORs the words; an
//   Any is that ballot tested against 0; a shuffle stores each lane's value at its index, then reads the source lane's (its index & 31), between two barriers.
// - Where lanes may have left or branched apart (div[block]), barriers cannot be used, and each subgroup acts for the active lanes it holds (SPV_WG32_PART): a
//   ballot is its subgroup's word alone, so its leader election, count and prefix under the lane masks cover the subgroup, and a warp-aggregated atomic then
//   adds each subgroup's share; a shuffle reads its source lane when that lane is in its subgroup (the Mali driver's subgroups are runs of consecutive
//   invocations: the source's place in it is the source's index less the subgroup's first, the invocation's index less its subgroup lane ID), else its own
//   value, as SHFL does for a lane outside its segment.
// Only for a GLCompute entry point of LocalSize 32 1 1 (one warp a workgroup), and only these operations: any other subgroup operation, or a built-in of the
// subgroup's own size or ID read by the game, leaves the module as it is (SPV_WG32_OTHER).
// Self-contained (no libc), so that dm/tools/tests/test_spv_wg32.py builds it alone.
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { SPV_WG32_LANE = 1, SPV_WG32_SITES = 2, SPV_WG32_PART = 4, SPV_WG32_OTHER = 8 };   // lane ID or masks rewritten; sites made workgroup-wide; sites made per subgroup (divergent); module untouched
enum { SWG_ANY = 335, SWG_BALLOT = 339, SWG_SHUFFLE = 345 };
static int swg_site(uint32_t op) { return op == SWG_ANY || op == SWG_BALLOT || op == SWG_SHUFFLE; }
static int swg_other(uint32_t op) { return (op >= 333 && op <= 366 && !swg_site(op)) || (op >= 4421 && op <= 4432); }   // OpGroupNonUniform* and OpSubgroup*KHR it does not take

// A GLCompute entry point of LocalSize 32 1 1 that reads the subgroup lane ID or has a ballot, any or shuffle: one pass over the instruction heads
static int spv_wg32_wanted(const uint32_t *w, size_t n) {
    if (n < 5 || w[0] != 0x07230203) return 0;
    int cs = 0, ls = 0, sg = 0;
    for (size_t i = 5; i < n; ) {
        uint32_t len = w[i] >> 16, op = w[i] & 0xffff;
        if (!len || i + len > n) return 0;
        if (op == 15 && len >= 3 && w[i + 1] == 5) cs = 1;
        else if (op == 16 && len == 6 && w[i + 2] == 17 && w[i + 3] == 32 && w[i + 4] == 1 && w[i + 5] == 1) ls = 1;
        else if ((op == 71 && len == 4 && w[i + 2] == 11 && (w[i + 3] == 41 || (w[i + 3] >= 4416 && w[i + 3] <= 4420))) || swg_site(op)) sg = 1;
        i += len;
    }
    return cs && ls && sg;
}

// w[n]: the module. size: the subgroup size, a power of two from 4 to 16. div: per id, nonzero for a block where lanes may be apart (NULL: none). sc: 2 × bound
// words of scratch. o[cap]: the result → its word count, or 0 when it is left as it is (not such a kernel, nothing to do, SPV_WG32_OTHER, or o NULL / smaller
// than *need: call again with that much). *what: SPV_WG32_*; *made, *part: the sites made workgroup-wide and per subgroup
static size_t spv_wg32(const uint32_t *w, size_t n, uint32_t size, const uint8_t *div, uint32_t *sc, uint32_t *o, size_t cap, size_t *need, int *what, int *made, int *part) {
    *what = 0; *need = 0; *made = 0; *part = 0;
    if (n < 5 || w[0] != 0x07230203 || size < 4 || size >= 32 || (size & (size - 1))) return 0;
    uint32_t bound = w[3], nsg = 32 / size;
    uint32_t *t32 = sc, *mk = sc + bound;   // t32[id]: 1 a 32-bit uint type, 2 another 32-bit scalar type; mk[id]: a lane mask variable's built-in - 4416 + 1
    for (size_t i = 0; i < 2 * (size_t)bound; i++) sc[i] = 0;
#define ID(x) ((x) && (x) < bound)
    uint32_t E = 0, tu = 0, tb = 0, tv4 = 0, tpIu = 0, lane = 0, li = 0, nentry = 0, ls = 0, nlane = 0, nsite = 0, nkept = 0, cap61 = 0, cap63 = 0, mused = 0;
    size_t lastcap = 0, lastann = 0, firstfn = 0; int other = 0;
    uint32_t cur = 0;
    for (size_t i = 5; i < n; ) {   // pass 1
        uint32_t len = w[i] >> 16, op = w[i] & 0xffff; const uint32_t *x = w + i;
        if (!len || i + len > n) return 0;
        if (op == 17 && len == 2) { lastcap = i + len; cap61 |= x[1] == 61; cap63 |= x[1] == 63; }
        else if (op == 15 && len >= 3) { nentry++; if (x[1] == 5) E = x[2]; }
        else if (op == 16 && len == 6 && x[2] == 17 && x[3] == 32 && x[4] == 1 && x[5] == 1) ls = 1;
        else if ((op >= 71 && op <= 75) || op == 332 || op == 5632 || op == 5633) {
            lastann = i + len;
            if (op == 71 && len == 4 && x[2] == 11 && ID(x[1])) {
                if (x[3] == 41) lane = x[1]; else if (x[3] == 29) li = x[1];
                else if (x[3] >= 4416 && x[3] <= 4420) mk[x[1]] = x[3] - 4416 + 1;
                else if (x[3] == 36 || x[3] == 38 || x[3] == 40) other = 1;   // SubgroupSize, NumSubgroups, SubgroupId read by the game
            }
        }
        else if (op == 21 && len == 4 && x[2] == 32 && ID(x[1])) { t32[x[1]] = x[3] ? 2 : 1; if (!x[3]) tu = x[1]; }
        else if (op == 22 && len >= 3 && x[2] == 32 && ID(x[1])) t32[x[1]] = 2;
        else if (op == 20 && len == 2) tb = x[1];
        else if (op == 23 && len == 4 && tu && x[2] == tu && x[3] == 4) tv4 = x[1];
        else if (op == 32 && len == 4 && x[2] == 1 && tu && x[3] == tu) tpIu = x[1];
        else if (op == 54 && !firstfn) firstfn = i;
        if (firstfn) {
            if (op == 248) cur = x[1];
            if (swg_other(op)) other = 1;
            if (swg_site(op)) {
                if (op == SWG_SHUFFLE ? len != 6 || !ID(x[1]) || !t32[x[1]] : len != 5) other = 1;   // only 32-bit scalars go through the exchange words
                if (div && ID(cur) && div[cur]) nkept++; else nsite++;
            }
            if (op == 61 && len >= 4 && lane && x[3] == lane) nlane++;
            else if ((op == 61 || op == 65 || op == 66) && len >= 4 && ID(x[3]) && mk[x[3]]) { mused |= 1u << (mk[x[3]] - 1); if (op != 61 && x[1] != tpIu) other = 1; }
            else if (op == 57) { for (uint32_t q = 4; q < len; q++) if (ID(x[q]) && mk[x[q]]) other = 1; }   // a mask variable handed to a function, copied or
            else if ((op == 83 || op == 67) && len >= 4 && ID(x[3]) && mk[x[3]]) other = 1;                  // reached otherwise (only id operands are looked at:
            else if ((op == 63 || op == 64) && len >= 3 && ((ID(x[1]) && mk[x[1]]) || (ID(x[2]) && mk[x[2]]))) other = 1;   // a literal may equal its id)
        }
        i += len;
    }
    if (nentry != 1 || !E || !ls || !tu || !tb || !firstfn) return 0;
    if (other) { *what = SPV_WG32_OTHER; return 0; }
    if ((mused || nsite || nkept) && (!tv4 || !tpIu)) { *what = SPV_WG32_OTHER; return 0; }
    if (nkept && !lane) { *what = SPV_WG32_OTHER; return 0; }   // a shuffle per subgroup reads the subgroup lane ID
    *what = (nlane || mused ? SPV_WG32_LANE : 0) | (nsite ? SPV_WG32_SITES : 0) | (nkept ? SPV_WG32_PART : 0); *part = (int)nkept;
    if (!nlane && !mused && !nsite && !nkept) return 0;
    *need = n + 170 + 14 * 5 + (size_t)(nsite + nkept) * (64 + 14 * nsg);
    if (!o || cap < *need) return 0;
    // new ids
    uint32_t nb = bound;
    uint32_t pIu = tpIu ? tpIu : nb++, li2 = li ? 0 : nb++, sgv = nsite ? nb++ : 0, pWu = nb++, c0 = nb++, c1 = nb++, c2 = nb++, c31 = nb++, c32 = nb++, c264 = nb++,
             tarr = nb++, pWa = nb++, sh = nb++, csz = nb++, pPu = mused ? nb++ : 0, pPv = mused ? nb++ : 0, mv[5] = {0}, cj[8] = {0};
    for (int k = 0; k < 5; k++) if (mused & (1u << k)) mv[k] = nb++;
    cj[0] = c0; cj[1] = c1; for (uint32_t j = 2; j < nsg; j++) cj[j] = nb++;
    uint32_t LI = li ? li : li2;
    size_t k = 0; int full = 0, incap = 0, inann = 0, intypes = 0, inentry = 0, initdone = 0, made_ = 0;
#define PUT(...) do { const uint32_t a_[] = {__VA_ARGS__}; size_t na_ = sizeof a_ / sizeof *a_; if (k + na_ > cap) full = 1; else for (size_t q_ = 0; q_ < na_; q_++) o[k++] = a_[q_]; } while (0)
    for (size_t i = 0; i < 5; i++) o[k++] = w[i];
    cur = 0;
    for (size_t i = 5; i < n; ) {
        uint32_t len = w[i] >> 16, op = w[i] & 0xffff; const uint32_t *x = w + i; size_t at = i; i += len;
        if (!incap && at == (lastcap ? lastcap : 5)) {   // after the last capability: the ones the new code needs
            incap = 1;
            if ((nsite || nkept) && !cap61) PUT(2u << 16 | 17, 61u);   // GroupNonUniform (SubgroupId)
            if ((nsite || nkept) && !cap63) PUT(2u << 16 | 17, 63u);   // GroupNonUniformArithmetic (BitwiseOr)
        }
        if (!inann && lastann && at == lastann) {   // after the last annotation: the new built-ins
            inann = 1;
            if (sgv) PUT(4u << 16 | 71, sgv, 11u, 40u);
            if (li2) PUT(4u << 16 | 71, li2, 11u, 29u);
        }
        if (op == 54 && !intypes) {   // before the first function: types, constants, variables
            intypes = 1;
            if (!inann) { inann = 1; if (sgv) PUT(4u << 16 | 71, sgv, 11u, 40u); if (li2) PUT(4u << 16 | 71, li2, 11u, 29u); }   // (no annotation in the module: spirv-val would refuse
                                                                                                                            // a decoration here, which the caller's check sees)
            if (!tpIu) PUT(4u << 16 | 32, pIu, 1u, tu);
            PUT(4u << 16 | 32, pWu, 4u, tu);
            PUT(4u << 16 | 43, tu, c0, 0u); PUT(4u << 16 | 43, tu, c1, 1u); PUT(4u << 16 | 43, tu, c2, 2u); PUT(4u << 16 | 43, tu, c31, 31u); PUT(4u << 16 | 43, tu, c32, 32u);
            PUT(4u << 16 | 43, tu, c264, 0x108u);   // AcquireRelease | WorkgroupMemory
            PUT(4u << 16 | 43, tu, csz, size);
            for (uint32_t j = 2; j < nsg; j++) PUT(4u << 16 | 43, tu, cj[j], j);
            PUT(4u << 16 | 28, tarr, tu, c32); PUT(4u << 16 | 32, pWa, 4u, tarr); PUT(4u << 16 | 59, pWa, sh, 4u);
            if (sgv) PUT(4u << 16 | 59, pIu, sgv, 1u);
            if (li2) PUT(4u << 16 | 59, pIu, li2, 1u);
            if (mused) { PUT(4u << 16 | 32, pPu, 6u, tu); PUT(4u << 16 | 32, pPv, 6u, tv4); for (int q = 0; q < 5; q++) if (mv[q]) PUT(4u << 16 | 59, pPv, mv[q], 6u); }
        }
        if (op == 15 && len >= 3 && x[2] == E) {   // the entry point's interface gets the new inputs
            uint32_t add = (sgv ? 1 : 0) + (li2 ? 1 : 0);
            if (k + len + add > cap) { full = 1; continue; }
            o[k++] = (len + add) << 16 | 15; for (uint32_t q = 1; q < len; q++) o[k++] = x[q];
            if (sgv) o[k++] = sgv;
            if (li2) o[k++] = li2;
            continue;
        }
        if (op == 54 && len == 5 && x[2] == E) inentry = 1;
        else if (op == 54) inentry = 0;
        if (op == 248) cur = x[1];
        if (inentry && !initdone && op != 248 && op != 54 && op != 59 && cur) {   // the entry point's first block, after its variables: the lane masks
            initdone = 1;
            if (mused) {
                uint32_t l = nb++, eq = nb++, lt = nb++, s2 = nb++, le = nb++, gt = nb++, ge = nb++;
                PUT(4u << 16 | 61, tu, l, LI); PUT(5u << 16 | 196, tu, eq, c1, l); PUT(5u << 16 | 130, tu, lt, eq, c1);   // OpLoad, OpShiftLeftLogical, OpISub
                PUT(5u << 16 | 196, tu, s2, c2, l); PUT(5u << 16 | 130, tu, le, s2, c1); PUT(4u << 16 | 200, tu, gt, le); PUT(4u << 16 | 200, tu, ge, lt);   // 2 << 31 wraps to 0: Le of lane 31 is all ones
                const uint32_t mval[5] = {eq, ge, gt, le, lt};   // SubgroupEqMask 4416, Ge, Gt, Le, Lt 4420
                for (int q = 0; q < 5; q++) if (mv[q]) { uint32_t v = nb++; PUT(7u << 16 | 80, tv4, v, mval[q], c0, c0, c0); PUT(3u << 16 | 62, mv[q], v); }   // OpCompositeConstruct, OpStore
            }
        }
        if (op == 61 && len >= 4 && lane && x[3] == lane) { PUT(4u << 16 | 61, x[1], x[2], LI); continue; }   // the lane ID: the workgroup index
        if (op == 61 && len >= 4 && ID(x[3]) && mk[x[3]]) { PUT(4u << 16 | 61, x[1], x[2], mv[mk[x[3]] - 1]); continue; }
        if ((op == 65 || op == 66) && len >= 4 && ID(x[3]) && mk[x[3]]) {   // a mask's component: of the private copy
            if (k + len > cap) { full = 1; continue; }
            o[k++] = x[0]; o[k++] = pPu; o[k++] = x[2]; o[k++] = mv[mk[x[3]] - 1]; for (uint32_t q = 4; q < len; q++) o[k++] = x[q];
            continue;
        }
        if (swg_site(op) && div && ID(cur) && div[cur]) {   // lanes may be apart: per subgroup, in the warp's lane numbers
            if (op == SWG_SHUFFLE) {   // its source lane's value when that lane is in its subgroup, else its own
                uint32_t l = nb++, g = nb++, b0 = nb++, ix = nb++, src = nb++, ok = nb++, sel = nb++, r = nb++;
                PUT(4u << 16 | 61, tu, l, LI); PUT(4u << 16 | 61, tu, g, lane); PUT(5u << 16 | 130, tu, b0, l, g);   // the subgroup's first invocation
                PUT(5u << 16 | 199, tu, ix, x[5], c31); PUT(5u << 16 | 130, tu, src, ix, b0); PUT(5u << 16 | 176, tb, ok, src, csz);   // OpULessThan (wraps: below the first too)
                PUT(6u << 16 | 169, tu, sel, ok, src, g);
                PUT(6u << 16 | 345, x[1], r, x[3], x[4], sel); PUT(6u << 16 | 169, x[1], x[2], ok, r, x[4]);
            } else {
                uint32_t l = nb++, bit = nb++, b = nb++, v = nb++;
                PUT(4u << 16 | 61, tu, l, LI); PUT(5u << 16 | 196, tu, bit, c1, l); PUT(6u << 16 | 169, tu, b, x[4], bit, c0);
                PUT(6u << 16 | 360, tu, v, x[3], 0u, b);
                if (op == SWG_BALLOT) PUT(7u << 16 | 80, x[1], x[2], v, c0, c0, c0); else PUT(5u << 16 | 171, x[1], x[2], v, c0);
            }
            continue;
        }
        if (swg_site(op)) {
            made_++;
            if (op == SWG_SHUFFLE) {   // %r = OpGroupNonUniformShuffle %T %scope %value %id
                uint32_t u = x[4], l = nb++, p = nb++, ix = nb++, q = nb++;
                if (t32[x[1]] != 1) { u = nb++; PUT(4u << 16 | 124, tu, u, x[4]); }   // OpBitcast to uint
                PUT(4u << 16 | 224, c2, c2, c264);   // OpControlBarrier: the last site's reads are done
                PUT(4u << 16 | 61, tu, l, LI); PUT(5u << 16 | 65, pWu, p, sh, l); PUT(3u << 16 | 62, p, u);
                PUT(4u << 16 | 224, c2, c2, c264);
                PUT(5u << 16 | 199, tu, ix, x[5], c31); PUT(5u << 16 | 65, pWu, q, sh, ix);   // OpBitwiseAnd: the translator's source lane is 0-31
                if (t32[x[1]] == 1) PUT(4u << 16 | 61, tu, x[2], q);
                else { uint32_t r = nb++; PUT(4u << 16 | 61, tu, r, q); PUT(4u << 16 | 124, x[1], x[2], r); }
            } else {   // %r = OpGroupNonUniformBallot %v4uint %scope %pred, or OpGroupNonUniformAny %bool %scope %pred
                uint32_t l = nb++, bit = nb++, b = nb++, v = nb++, sg = nb++, p = nb++, acc = 0;
                PUT(4u << 16 | 61, tu, l, LI); PUT(5u << 16 | 196, tu, bit, c1, l); PUT(6u << 16 | 169, tu, b, x[4], bit, c0);   // OpLoad, OpShiftLeftLogical, OpSelect
                PUT(6u << 16 | 360, tu, v, x[3], 0u, b);   // OpGroupNonUniformBitwiseOr Reduce over the subgroup
                PUT(4u << 16 | 224, c2, c2, c264);
                PUT(4u << 16 | 61, tu, sg, sgv); PUT(5u << 16 | 65, pWu, p, sh, sg); PUT(3u << 16 | 62, p, v);
                PUT(4u << 16 | 224, c2, c2, c264);
                for (uint32_t j = 0; j < nsg; j++) {
                    uint32_t pj = nb++, lj = nb++; PUT(5u << 16 | 65, pWu, pj, sh, cj[j]); PUT(4u << 16 | 61, tu, lj, pj);
                    if (!j) acc = lj; else { uint32_t a2 = nb++; PUT(5u << 16 | 197, tu, a2, acc, lj); acc = a2; }   // OpBitwiseOr
                }
                if (op == SWG_BALLOT) PUT(7u << 16 | 80, x[1], x[2], acc, c0, c0, c0);   // OpCompositeConstruct
                else PUT(5u << 16 | 171, x[1], x[2], acc, c0);   // OpINotEqual
            }
            continue;
        }
        if (k + len > cap) { full = 1; continue; }
        for (uint32_t q = 0; q < len; q++) o[k++] = x[q];
    }
#undef PUT
#undef ID
    if (full || !intypes || (mused && !initdone)) { *what |= SPV_WG32_OTHER; return 0; }
    *made = made_;
    o[3] = nb;
    return k;
}

// Which blocks may hold a strict subset of the workgroup's invocations, for spv_wg32's div: a flow-insensitive pass over the translator's code, whose registers are
// Function variables. A value depends on the lane when it comes from a lane built-in (LocalInvocationIndex / Id, GlobalInvocationId, the subgroup's lane ID or
// masks), a load through a pointer that does, a load of an invocation's own variable (Function, Private) that some store gave such a value or that was stored
// in such a block (workgroup and buffer memory read at one address give every lane the same word), an atomic, a phi from such a block, or an operand that is
// one; a ballot's or any's result, and a shuffle's from a lane-independent source lane, is the same for every lane. A conditional branch or switch on such a
// value makes divergent every block reachable from it before its immediate post-dominator (to the end of the function when a lane may return on the way: a lane
// that left does not reach the barriers after it). Calls are taken per call site: a second taint follows what a function's parameters reach (the translator's
// raw-memory helpers are called from both kinds of places), so a call's result depends on the lane when the function's own code makes it so, or when it follows
// a parameter and that call's argument does; likewise what it stores through a pointer parameter, and anything it stores when the call itself is in a divergent
// block. A load right after a store to the same variable in straight-line code takes the stored value's dependence. A function called from a divergent
// block has every block divergent (for its sites). Iterated to a fixpoint. w[n]: the module; div[bound]: the result
// (1: may diverge); sc: SPV_WG32_DIV_SC(bound, n) words of scratch. → 1, or 0 when it could not tell (div is then all ones)
#define SPV_WG32_DIV_SC(bound, n) (10 * ((size_t)(bound) + 1) + 2 * (size_t)(n))
enum { SWD_DIV = 1, SWD_VAR = 2, SWD_LANE = 4, SWD_BLK = 8, SWD_DIVB = 16, SWD_CALLD = 32, SWD_RETD = 64, SWD_FN = 128, SWD_REG = 256,   // REG: an invocation's own variable
       SWD_PDEP = 512, SWD_PVAR = 1024, SWD_PDIVB = 2048, SWD_RETP = 4096, SWD_PARAM = 8192, SWD_PST = 16384 };   // the parameter taint: value, variable, block, return; a parameter, stored through
static int swd_noresult(uint32_t op) {   // instructions in a function body without a result id at word 2
    return op == 62 || op == 63 || op == 224 || op == 225 || op == 228 || (op >= 246 && op <= 255) || op == 8 || op == 317 || op == 56;
}
static int spv_wg32_div(const uint32_t *w, size_t n, uint8_t *div, uint32_t *sc) {
    if (n < 5 || w[0] != 0x07230203) return 0;
    uint32_t bound = w[3], V = bound, B1 = bound + 1;
    for (uint32_t i = 0; i < bound; i++) div[i] = 1;
    uint32_t *fl = sc, *base = sc + B1, *ss = sc + 2 * (size_t)B1, *ps = sc + 3 * (size_t)B1, *po = sc + 4 * (size_t)B1, *ipd = sc + 5 * (size_t)B1,
             *vis = sc + 6 * (size_t)B1, *ord = sc + 7 * (size_t)B1, *stk = sc + 8 * (size_t)B1, *seq = sc + 9 * (size_t)B1, *L = sc + 10 * (size_t)B1, *P = L + n;
    // ss / ps: a block's successor / predecessor list in L / P (a count, then the blocks) + 1; for a function id, ss: its OpFunction's position + 1
    for (size_t i = 0; i < SPV_WG32_DIV_SC(bound, n); i++) sc[i] = 0;
#define ID(x) ((x) && (x) < bound)
    size_t firstfn = 0, nl = 0, np = 0;
    for (size_t i = 5; i < n; ) {
        uint32_t len = w[i] >> 16, op = w[i] & 0xffff; const uint32_t *x = w + i;
        if (!len || i + len > n) return 0;
        if (op == 71 && len == 4 && x[2] == 11 && ID(x[1]) && (x[3] == 27 || x[3] == 28 || x[3] == 29 || x[3] == 41 || (x[3] >= 4416 && x[3] <= 4420))) fl[x[1]] |= SWD_LANE;
        if (op == 54 && len >= 3 && ID(x[2])) { if (!firstfn) firstfn = i; ss[x[2]] = (uint32_t)i + 1; fl[x[2]] |= SWD_FN; }
        if (op == 59 && len >= 4 && ID(x[2]) && (x[3] == 6 || x[3] == 7)) fl[x[2]] |= SWD_REG;
        if (op == 55 && len >= 3 && ID(x[2])) fl[x[2]] |= SWD_REG | SWD_PARAM | SWD_PDEP;
        i += len;
    }
    if (!firstfn) return 0;
    uint32_t cur = 0;
    for (size_t i = firstfn; i < n; ) {   // successor lists
        uint32_t len = w[i] >> 16, op = w[i] & 0xffff; const uint32_t *x = w + i; i += len;
        if (op == 248 && len == 2 && ID(x[1])) { cur = x[1]; fl[cur] |= SWD_BLK; continue; }
        if (!cur || !(op == 249 || op == 250 || op == 251 || (op >= 252 && op <= 255))) continue;
        uint32_t c = op == 249 ? 1 : op == 250 ? 2 : op == 251 ? 1 + (len - 3) / 2 : 0;
        if (nl + 1 + c > n) return 0;
        ss[cur] = (uint32_t)nl + 1; L[nl++] = c;
        if (op == 249) L[nl++] = x[1]; else if (op == 250) { L[nl++] = x[2]; L[nl++] = x[3]; } else if (op == 251) { L[nl++] = x[2]; for (uint32_t q = 4; q < len; q += 2) L[nl++] = x[q]; }
        for (uint32_t q = 0; q < c; q++) if (!ID(L[nl - c + q])) return 0;
        cur = 0;
    }
    for (uint32_t b = 1; b < bound; b++) if ((fl[b] & SWD_BLK) && ss[b]) { uint32_t *l = L + ss[b] - 1; for (uint32_t q = 1; q <= l[0]; q++) { if (!(fl[l[q]] & SWD_BLK)) return 0; po[l[q]]++; } }   // po: predecessor counts for now
    for (uint32_t b = 1; b < bound; b++) if (fl[b] & SWD_BLK) { if (np + 1 + po[b] > n) return 0; ps[b] = (uint32_t)np + 1; P[np] = 0; np += 1 + po[b]; po[b] = 0; }
    for (uint32_t b = 1; b < bound; b++) if ((fl[b] & SWD_BLK) && ss[b]) { uint32_t *l = L + ss[b] - 1; for (uint32_t q = 1; q <= l[0]; q++) { uint32_t *p = P + ps[l[q]] - 1; p[1 + p[0]++] = b; } }
    // immediate post-dominators, function by function: Cooper, Harvey and Kennedy over the reverse CFG from a virtual exit V
    for (size_t i = firstfn; i < n; ) {
        uint32_t len = w[i] >> 16, op = w[i] & 0xffff; size_t j = i; i += len;
        if (op != 54) continue;
        uint32_t nb = 0;
        for (; j < n && (w[j] & 0xffff) != 56; j += w[j] >> 16) if ((w[j] & 0xffff) == 248) { if (nb >= bound) return 0; ord[nb++] = w[j + 1]; }
        if (!nb) continue;
        for (uint32_t q = 0; q < nb; q++) { vis[ord[q]] = 0; po[ord[q]] = 0; ipd[ord[q]] = 0; }
        uint32_t cnt = 0, sp = 0;
        for (uint32_t q = 0; q < nb; q++) {   // a depth-first walk from each exit block (V's children) over predecessors, numbering blocks as they finish
            uint32_t b = ord[q];
            if ((ss[b] && L[ss[b] - 1]) || vis[b]) continue;
            vis[b] = 1; stk[sp++] = b; stk[sp++] = 0;
            while (sp) {
                uint32_t x = stk[sp - 2], k = stk[sp - 1], *p = P + ps[x] - 1;
                if (k < p[0]) { stk[sp - 1]++; uint32_t y = p[1 + k]; if (!vis[y]) { if (sp + 2 > B1) return 0; vis[y] = 1; stk[sp++] = y; stk[sp++] = 0; } }
                else { seq[cnt] = x; po[x] = ++cnt; sp -= 2; }
            }
        }
        po[V] = cnt + 1; ipd[V] = V;
        for (int it = 0, ch = 1; ch && it < 1000; it++) {
            ch = 0;
            for (uint32_t r = cnt; r-- > 0; ) {   // reverse postorder
                uint32_t b = seq[r], *l = ss[b] ? L + ss[b] - 1 : NULL, nw = 0;
                if (!l || !l[0]) nw = V;
                else for (uint32_t q = 1; q <= l[0]; q++) {
                    uint32_t s = l[q]; if (!po[s] || !ipd[s]) continue;   // not reached yet, or never reaches an exit
                    if (!nw) { nw = s; continue; }
                    uint32_t a = s, c = nw; long g = 0;
                    while (a != c && g++ < (1L << 22)) { while (po[a] < po[c] && g++ < (1L << 22)) a = ipd[a]; while (po[c] < po[a] && g++ < (1L << 22)) c = ipd[c]; }
                    if (a != c) return 0;
                    nw = a;
                }
                if (nw && ipd[b] != nw) { ipd[b] = nw; ch = 1; }
            }
        }
        for (uint32_t q = 0; q < nb; q++) if (!ipd[ord[q]]) ipd[ord[q]] = V;   // never reaches an exit: its region runs to the end
    }
    // the fixpoint over values, variables, functions and blocks, in both taints
    for (uint32_t b = 0; b < B1; b++) { vis[b] = 0; po[b] = 0; }   // vis: a branch's regions made (1 lane, 2 parameter); po: the region walk's stamp
    uint32_t stamp = 0, bst = 0;   // bst: a stretch of straight-line code (a block up to its first call), for a load after a store in it: the stored value's
    uint32_t *lastv = seq, *lastb = ord;   // (the translator stores a register and loads it again at once): lastv / lastb per variable
    for (uint32_t b = 0; b < B1; b++) { lastv[b] = 0; lastb[b] = 0; }
    for (int it = 0, ch = 1; ch && it < 200; it++) {
        ch = 0; cur = 0; uint32_t curf = 0;
#define SET(id, f) do { if (!(fl[id] & (f))) { fl[id] |= (f); ch = 1; } } while (0)
#define BASE(p) (base[p] ? base[p] : (p))
        for (size_t i = firstfn; i < n; ) {
            uint32_t len = w[i] >> 16, op = w[i] & 0xffff; const uint32_t *x = w + i; i += len;
            if (op == 54 && len >= 3) { curf = x[2]; cur = 0; continue; }
            if (op == 56) { curf = 0; cur = 0; continue; }
            if (op == 248) { cur = x[1]; bst++; continue; }
            int bd = cur && (fl[cur] & SWD_DIVB), bp = cur && (fl[cur] & SWD_PDIVB);
            if ((op == 59 || op == 55) && len >= 3 && ID(x[2])) { base[x[2]] = x[2]; continue; }
            if (op == 62 && len >= 3 && ID(x[1])) {   // OpStore
                uint32_t b = BASE(x[1]);
                if ((ID(x[2]) && (fl[x[2]] & SWD_DIV)) || bd || (fl[x[1]] & SWD_DIV)) SET(b, SWD_VAR);
                if ((ID(x[2]) && (fl[x[2]] & SWD_PDEP)) || bp || (fl[x[1]] & SWD_PDEP)) SET(b, SWD_PVAR);
                if (fl[b] & SWD_PARAM) SET(b, SWD_PST);
                if (b == x[1] && ID(x[2])) { lastv[b] = x[2]; lastb[b] = bst; }
                continue;
            }
            if (op == 63 && len >= 3 && ID(x[1]) && ID(x[2])) {   // OpCopyMemory
                uint32_t b = BASE(x[1]), a = BASE(x[2]);
                if ((fl[a] & SWD_LANE) || ((fl[a] & SWD_VAR) && (fl[a] & SWD_REG)) || bd || ((fl[x[1]] | fl[x[2]]) & SWD_DIV)) SET(b, SWD_VAR);
                if ((fl[a] & SWD_PARAM) || ((fl[a] & SWD_PVAR) && (fl[a] & SWD_REG)) || bp || ((fl[x[1]] | fl[x[2]]) & SWD_PDEP)) SET(b, SWD_PVAR);
                if (fl[b] & SWD_PARAM) SET(b, SWD_PST);
                continue;
            }
            if (op == 254 && len >= 2 && curf) {
                if ((ID(x[1]) && (fl[x[1]] & SWD_DIV)) || bd) SET(curf, SWD_RETD);
                if ((ID(x[1]) && (fl[x[1]] & SWD_PDEP)) || bp) SET(curf, SWD_RETP);
                continue;
            }
            if ((op == 250 || op == 251) && cur && ID(x[1])) {   // a divergent branch: the blocks before its post-dominator
                for (uint32_t t = 0; t < 2; t++) {
                    uint32_t want = t ? SWD_PDEP : SWD_DIV, mark = t ? SWD_PDIVB : SWD_DIVB;
                    if (!(fl[x[1]] & want) || (vis[cur] & (1u << t))) continue;
                    vis[cur] |= 1u << t; ch = 1; stamp++;
                    uint32_t stop = ipd[cur], sp = 0, *l = L + ss[cur] - 1;
                    for (uint32_t q = 1; q <= l[0]; q++) if (l[q] != stop && po[l[q]] != stamp) { po[l[q]] = stamp; stk[sp++] = l[q]; }
                    while (sp) {
                        uint32_t b = stk[--sp], *m = ss[b] ? L + ss[b] - 1 : NULL; fl[b] |= mark;
                        for (uint32_t q = 1; m && q <= m[0]; q++) if (m[q] != stop && po[m[q]] != stamp) { if (sp >= B1) return 0; po[m[q]] = stamp; stk[sp++] = m[q]; }
                    }
                }
                continue;
            }
            if (swd_noresult(op) || len < 3 || !ID(x[2])) continue;
            uint32_t r = x[2], q0 = op == 12 ? 5 : 3; int dv = 0, pv = 0;   // (OpExtInst: its set and instruction number are not operands)
            if (op == 61 && len >= 4 && ID(x[3])) {   // a load: of a lane built-in, through a lane-dependent pointer, or of an invocation's own variable stored so
                uint32_t b = BASE(x[3]);   // (workgroup or buffer memory at one address reads one word for every lane)
                if (b == x[3] && lastb[b] == bst && lastv[b]) { dv = (fl[lastv[b]] & SWD_DIV) != 0; pv = (fl[lastv[b]] & SWD_PDEP) != 0; }   // just stored in this stretch
                else {
                    dv = (fl[b] & SWD_LANE) || (fl[x[3]] & SWD_DIV) || ((fl[b] & SWD_VAR) && (fl[b] & SWD_REG));
                    pv = (fl[b] & SWD_PARAM) || (fl[x[3]] & SWD_PDEP) || ((fl[b] & SWD_PVAR) && (fl[b] & SWD_REG));
                }
            }
            else if (op == 65 || op == 66 || op == 67) {
                if (ID(x[3])) base[r] = BASE(x[3]);
                for (uint32_t q = 3; q < len; q++) if (ID(x[q])) { dv |= (fl[x[q]] & SWD_DIV) != 0; pv |= (fl[x[q]] & SWD_PDEP) != 0; }
            }
            else if (op == SWG_BALLOT || op == SWG_ANY) dv = pv = 0;
            else if (op == SWG_SHUFFLE && len == 6) { dv = ID(x[5]) && (fl[x[5]] & SWD_DIV); pv = ID(x[5]) && (fl[x[5]] & SWD_PDEP); }
            else if (op >= 227 && op <= 242) dv = 1;   // atomics
            else if (op == 57 && len >= 4 && ID(x[3]) && (fl[x[3]] & SWD_FN)) {   // a call: per call site (and the end of a stretch: it may store through its arguments)
                bst++;
                uint32_t f = x[3]; size_t pj = ss[f] - 1; pj += w[pj] >> 16;   // the callee's parameters follow its OpFunction
                int ad = 0, ap = 0;   // an argument that depends on the lane / on this function's parameters
                for (uint32_t q = 4; q < len; q++) if (ID(x[q])) {
                    uint32_t a = x[q], b = BASE(a);
                    ad |= (fl[a] & SWD_DIV) || (fl[b] & SWD_LANE) || ((fl[b] & SWD_VAR) && (fl[b] & SWD_REG));
                    ap |= (fl[a] & SWD_PDEP) || (fl[b] & SWD_PARAM) || ((fl[b] & SWD_PVAR) && (fl[b] & SWD_REG));
                }
                dv = (fl[f] & SWD_RETD) || ((fl[f] & SWD_RETP) && ad);
                pv = (fl[f] & SWD_RETP) && ap;
                if (bd) SET(f, SWD_CALLD);
                for (uint32_t q = 4; q < len && pj < n && (w[pj] & 0xffff) == 55; q++, pj += w[pj] >> 16) {   // what it stores through a pointer parameter
                    uint32_t prm = w[pj + 2], a = x[q]; if (!ID(prm) || !ID(a) || !(fl[prm] & SWD_PST)) continue;
                    uint32_t b = BASE(a);
                    if (bd || (fl[prm] & SWD_VAR) || ((fl[prm] & SWD_PVAR) && ad)) SET(b, SWD_VAR);
                    if (bp || ((fl[prm] & SWD_PVAR) && ap)) SET(b, SWD_PVAR);
                    if (fl[b] & SWD_PARAM) SET(b, SWD_PST);
                }
            }
            else if (op == 57) dv = 1;
            else if (op == 245) {
                for (uint32_t q = 3; q + 1 < len; q += 2) {
                    if ((ID(x[q]) && (fl[x[q]] & SWD_DIV)) || (ID(x[q + 1]) && (fl[x[q + 1]] & SWD_DIVB))) dv = 1;
                    if ((ID(x[q]) && (fl[x[q]] & SWD_PDEP)) || (ID(x[q + 1]) && (fl[x[q + 1]] & SWD_PDIVB))) pv = 1;
                }
            }
            else for (uint32_t q = q0; q < len; q++) if (ID(x[q])) { dv |= (fl[x[q]] & SWD_DIV) != 0; pv |= (fl[x[q]] & SWD_PDEP) != 0; }
            if (dv) SET(r, SWD_DIV);
            if (pv) SET(r, SWD_PDEP);
        }
#undef BASE
#undef SET
    }
    cur = 0; uint32_t curf = 0;
    for (size_t i = firstfn; i < n; ) {
        uint32_t len = w[i] >> 16, op = w[i] & 0xffff; const uint32_t *x = w + i; i += len;
        if (op == 54 && len >= 3) curf = x[2];
        else if (op == 248 && ID(x[1])) div[x[1]] = (fl[x[1]] & SWD_DIVB) || (curf && (fl[curf] & SWD_CALLD));
    }
#undef ID
    return 1;
}
