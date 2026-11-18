// The translator's raw global memory helpers look an address's entry up in the page map after the table (gmem_table.h gmem_table_pages) before they search. A helper
// (GlobalToStorage.TryGenerateRawStorageOp; spv_gmcache.h) whose cached entry doesn't hold the address reads the hint of its 2 MiB chunk, and where that names no entry,
// searches the table in 13 dependent steps of two loads each. In Ocarina of Time's Kokiri Forest the scene compute shaders' helpers searched about 150,000 times a
// frame on a OnePlus Pad 3: every invocation's first access through each helper misses (the cached entry starts at the empty entry 0), and the pieces they reach are
// smaller than a 2 MiB chunk or not on one, so the hint has nothing for them. The page map has a word per 4 KiB page (by the page's low 18 bits, tagged with the rest):
// the search block now reads it first and takes its entry when the tag matches, else searches as before. Only a page wholly inside one entry is mapped, so the entry chosen
// is the one the search finds. A miss the hint answers doesn't wait for the page map's word (read beside the hint, it made one shader 13 % slower on a OnePlus Pad 3,
// its hint words in the cache and the map's not). No re-translation: the rewrite runs when a module is made
// (gfx_vk_lmod.inc lm_code), and a module whose helpers are not in the translator's form is left as it is. Self-contained (no libc) for dm/tools/tests/test_spv_gmpage.py.
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { SPV_GMPAGE_FORM = 1, SPV_GMPAGE_ROOM = 2, SPV_GMPAGE_MANY = 3 };   // why a module was left: not the translator's form; out too small; more helpers than fit
#define SPV_GMPAGE_MAX 16   // helpers rewritten at most in one module
enum { SPV_GMPAGE_PAGE_SHIFT = 12, SPV_GMPAGE_BITS = 18, SPV_GMPAGE_WORD = 65536 };   // gmem_table.h GMEM_PAGE_SHIFT, GMEM_PAGE_BITS, GMEM_PAGE_WORD
enum { SPV_GMPAGE_NC = 10 };   // the constants it uses: int 20, 12, the slot mask, the map's first word, 8191, 13, 18, 0; uint 256; and none spare
#define SPV_GMPAGE_EXTRA(nf) (4 * SPV_GMPAGE_NC + (size_t)(nf) * 110)   // words a rewrite adds at most for nf helpers: the constants and each helper's lookup and blocks

typedef struct { size_t fn, end, sl, se; uint32_t tab, acty, tabc0, wty, ity, bty, plo, phi, sel, found, sm, sr; } spv_gmpage_fn;   // sl: the search block's OpLabel; se: its
// OpBranch to the found block; sel: the local both the hint block and the search store the chosen entry into; sm, sr: the new merge and search labels

// The module rewritten into o (cap words) → its length; 0 when nothing is done (*why says why; 0: no helper). sc: 2 * bound words of scratch
static size_t spv_gmpage(const uint32_t *w, size_t n, uint32_t *sc, uint32_t *o, size_t cap, int *why) {
    *why = 0;
    if (n < 5 || w[0] != 0x07230203) { *why = SPV_GMPAGE_FORM; return 0; }
    uint32_t bound = w[3], *def = sc, *cv = sc + bound;   // def: each id's defining word; cv: a 32-bit constant's value + 1 (0: not a constant)
    for (size_t i = 0; i < 2 * (size_t)bound; i++) sc[i] = 0;
    size_t i_fn = 0; uint32_t uty = 0;
    for (size_t i = 5; i < n; ) { uint32_t len = w[i] >> 16, op = w[i] & 0xffff; const uint32_t *x = w + i; if (!len || i + len > n) { *why = SPV_GMPAGE_FORM; return 0; }
        if (op == 54 && !i_fn) i_fn = i;
        if (op >= 19 && op <= 39 && len >= 2 && x[1] < bound) def[x[1]] = (uint32_t)i;   // types
        else if (len >= 3 && x[2] < bound && (op == 43 || op == 55 || op == 59 || op == 61 || op == 65 || op == 124 || (op >= 126 && op <= 200))) def[x[2]] = (uint32_t)i;   // constants, parameters, variables, loads, chains, bitcasts, arithmetic, logic
        if (op == 43 && len == 4 && x[2] < bound && x[3] != 0xffffffffu) cv[x[2]] = x[3] + 1;
        if (op == 21 && len == 4 && x[2] == 32 && x[3] == 0 && !uty) uty = x[1];   // the 32-bit uint type
        i += len; }
    if (!i_fn || !uty) { *why = i_fn ? SPV_GMPAGE_FORM : 0; return 0; }
    #define GP_C(id, v) ((id) < bound && cv[id] == (uint32_t)(v) + 1)
    #define GP_OP(id) ((id) < bound && def[id] ? w[def[id]] & 0xffff : 0)
    spv_gmpage_fn F[SPV_GMPAGE_MAX]; int nf = 0;
    for (size_t i = i_fn; i < n; ) {   // each function with an OpConvertUToPtr
        size_t s = i, e = i; int conv = 0;
        for (; e < n; e += w[e] >> 16) { uint32_t op = w[e] & 0xffff; if (op == 120) conv = 1; if (op == 56) { e += w[e] >> 16; break; } }
        i = e;
        if (!conv) continue;
        if (nf == SPV_GMPAGE_MAX) { *why = SPV_GMPAGE_MANY; return 0; }
        spv_gmpage_fn *f = &F[nf]; for (size_t q = 0; q < sizeof *f; q++) ((uint8_t *)f)[q] = 0; f->fn = s; f->end = e;
        uint32_t best = 0, bestn = 0;   // the table: the base of most of the function's access chains
        for (size_t j = s; j < e; j += w[j] >> 16) if ((w[j] & 0xffff) == 65 && (w[j] >> 16) == 6) { uint32_t b = w[j + 3], c = 0;
            for (size_t q = s; q < e; q += w[q] >> 16) if ((w[q] & 0xffff) == 65 && (w[q] >> 16) == 6 && w[q + 3] == b) c++;
            if (c > bestn) { bestn = c; best = b; } }
        if (bestn < 7) { *why = SPV_GMPAGE_FORM; return 0; }
        f->tab = best;
        for (size_t j = s; j < e && !f->wty; j += w[j] >> 16) if ((w[j] & 0xffff) == 61 && (w[j] >> 16) >= 4 && GP_OP(w[j + 3]) == 65) {   // a word loaded through a chain on it
            const uint32_t *ac = w + def[w[j + 3]]; if ((ac[0] >> 16) == 6 && ac[3] == best) { f->wty = w[j + 1]; f->acty = ac[1]; f->tabc0 = ac[4]; } }
        size_t br0 = 0; for (size_t j = s; j < e && !br0; j += w[j] >> 16) if ((w[j] & 0xffff) == 250) br0 = j;   // the entry block's branch: its true label, the hint block
        if (!br0 || !f->wty) { *why = SPV_GMPAGE_FORM; return 0; }
        size_t hb = 0; for (size_t j = br0; j < e && !hb; j += w[j] >> 16) if ((w[j] & 0xffff) == 248 && w[j + 1] == w[br0 + 2]) hb = j;
        if (!hb) { *why = SPV_GMPAGE_FORM; return 0; }
        size_t he = 0; for (size_t j = hb + (w[hb] >> 16); j < e && !he; j += w[j] >> 16) { uint32_t op = w[j] & 0xffff; if (op == 248) break; if (op == 250) he = j; }
        if (!he) { *why = SPV_GMPAGE_FORM; return 0; }
        for (size_t j = hb; j < he; j += w[j] >> 16) { uint32_t op = w[j] & 0xffff; const uint32_t *x = w + j;   // the chunk's halves: hi << 11, lo >> 21, each a load of a parameter
            if ((op == 196 || op == 194) && (w[j] >> 16) == 5 && GP_C(x[4], op == 196 ? 11 : 21) && GP_OP(x[3]) == 61) {
                uint32_t p = w[def[x[3]] + 3]; if (GP_OP(p) != 55) continue;
                if (op == 196) f->phi = p; else f->plo = p;
                f->ity = x[1]; } }
        uint32_t c = w[he + 1];   // the branch: on the negated verdict to the search block, else to the found block; the hint block's last store, of its entry into a local
        if (!f->phi || !f->plo || GP_OP(c) != 168 || def[c] < hb) { *why = SPV_GMPAGE_FORM; return 0; }
        f->bty = w[def[c] + 1]; f->found = w[he + 3];
        uint32_t hv = 0; for (size_t j = hb; j < he; j += w[j] >> 16) if ((w[j] & 0xffff) == 62 && (w[j] >> 16) == 3) { f->sel = w[j + 1]; hv = w[j + 2]; }
        if (!f->sel || GP_OP(f->sel) != 59 || w[def[f->sel] + 3] != 7 || !GP_OP(hv) || w[def[hv] + 1] != f->ity) { *why = SPV_GMPAGE_FORM; return 0; }   // a Function variable, an int of the chunk's type
        { uint32_t pt = GP_OP(f->phi) == 55 ? w[def[f->phi] + 1] : 0, plt = GP_OP(f->plo) == 55 ? w[def[f->plo] + 1] : 0;   // both parameters pointers to that int type
          if (!pt || pt != plt || (w[def[pt]] & 0xffff) != 32 || w[def[pt] + 3] != f->ity) { *why = SPV_GMPAGE_FORM; return 0; } }
        // the search block: one block, ending in a branch to the found block after its own store into that local; the found block without OpPhi
        size_t sl = 0; for (size_t j = he; j < e && !sl; j += w[j] >> 16) if ((w[j] & 0xffff) == 248 && w[j + 1] == w[he + 2]) sl = j;
        if (!sl) { *why = SPV_GMPAGE_FORM; return 0; }
        size_t se = 0, last_st = 0; for (size_t j = sl + 2; j < e; j += w[j] >> 16) { uint32_t op = w[j] & 0xffff; if (op == 248 || op == 56) break; if (op == 62) last_st = j; se = j; }
        if (!se || (w[se] & 0xffff) != 249 || w[se + 1] != f->found || !last_st || w[last_st + 1] != f->sel) { *why = SPV_GMPAGE_FORM; return 0; }
        size_t fl = 0; for (size_t j = se; j < e && !fl; j += w[j] >> 16) if ((w[j] & 0xffff) == 248 && w[j + 1] == f->found) fl = j;
        if (!fl || (w[fl + 2] & 0xffff) == 245) { *why = SPV_GMPAGE_FORM; return 0; }
        f->sl = sl; f->se = se;
        nf++;
    }
    if (!nf) return 0;
    uint32_t ity = F[0].ity;
    for (int k = 1; k < nf; k++) if (F[k].ity != ity) { *why = SPV_GMPAGE_FORM; return 0; }
    static const uint32_t cval[SPV_GMPAGE_NC - 1] = {20, SPV_GMPAGE_PAGE_SHIFT, (1u << SPV_GMPAGE_BITS) - 1, SPV_GMPAGE_WORD, 8191, 13, SPV_GMPAGE_BITS, 0, 256};
    uint32_t cs[SPV_GMPAGE_NC - 1] = {0}, nb_ = bound;   // the constants: the module's own, else new (the last a uint)
    for (size_t i = 5; i < i_fn; i += w[i] >> 16) { const uint32_t *x = w + i;
        if ((x[0] & 0xffff) == 43 && (x[0] >> 16) == 4) for (int c = 0; c < SPV_GMPAGE_NC - 1; c++) if (!cs[c] && x[1] == (c == SPV_GMPAGE_NC - 2 ? uty : ity) && x[3] == cval[c]) cs[c] = x[2]; }
    for (int c = 0; c < SPV_GMPAGE_NC - 1; c++) if (!cs[c]) cs[c] = nb_++;
    if (cap < n + SPV_GMPAGE_EXTRA(nf)) { *why = SPV_GMPAGE_ROOM; return 0; }
    for (int q = 0; q < nf; q++) { F[q].sm = nb_++; F[q].sr = nb_++; }
    size_t k = 0;
    for (int q = 0; q < 5; q++) o[k++] = w[q];
    for (size_t i = 5; i < n; ) { uint32_t len = w[i] >> 16; const uint32_t *x = w + i;
        if (i == i_fn) for (int c = 0; c < SPV_GMPAGE_NC - 1; c++) if (cs[c] >= bound) { o[k++] = 4u << 16 | 43; o[k++] = c == SPV_GMPAGE_NC - 2 ? uty : ity; o[k++] = cs[c]; o[k++] = cval[c]; }
        const spv_gmpage_fn *f = 0;
        for (int q = 0; q < nf && !f; q++) if (i >= F[q].fn && i < F[q].end) f = &F[q];
        if (f && i == f->se) {   // the search's branch on to the found block now goes through the new merge
            o[k++] = 2u << 16 | 249; o[k++] = f->sm; o[k++] = 2u << 16 | 248; o[k++] = f->sm; o[k++] = 2u << 16 | 249; o[k++] = f->found; i += len; continue; }
        for (uint32_t q = 0; q < len; q++) o[k + q] = x[q];
        k += len; i += len;
        if (f && i - len == f->sl) {   // page = hi << 20 | lo >> 12; word = map[page & mask]; its entry when its tag is page >> 18 and hi < 256, else the search
            uint32_t I = f->ity, B = f->bty, l = nb_++, h = nb_++, a = nb_++, b = nb_++, pg = nb_++, sl = nb_++, wi = nb_++, pp = nb_++, pv = nb_++, pvi = nb_++, pe = nb_++, pt = nb_++,
                     tg = nb_++, k1 = nb_++, k2 = nb_++, hu = nb_++, k3 = nb_++, k4 = nb_++, ok = nb_++;
            o[k++] = 4u << 16 | 61; o[k++] = I; o[k++] = l; o[k++] = f->plo;
            o[k++] = 4u << 16 | 61; o[k++] = I; o[k++] = h; o[k++] = f->phi;
            o[k++] = 5u << 16 | 196; o[k++] = I; o[k++] = a; o[k++] = h; o[k++] = cs[0];
            o[k++] = 5u << 16 | 194; o[k++] = I; o[k++] = b; o[k++] = l; o[k++] = cs[1];
            o[k++] = 5u << 16 | 197; o[k++] = I; o[k++] = pg; o[k++] = a; o[k++] = b;
            o[k++] = 5u << 16 | 199; o[k++] = I; o[k++] = sl; o[k++] = pg; o[k++] = cs[2];
            o[k++] = 5u << 16 | 128; o[k++] = I; o[k++] = wi; o[k++] = sl; o[k++] = cs[3];
            o[k++] = 6u << 16 | 65; o[k++] = f->acty; o[k++] = pp; o[k++] = f->tab; o[k++] = f->tabc0; o[k++] = wi;
            o[k++] = 4u << 16 | 61; o[k++] = f->wty; o[k++] = pv; o[k++] = pp;
            if (f->wty != I) { o[k++] = 4u << 16 | 124; o[k++] = I; o[k++] = pvi; o[k++] = pv; } else pvi = pv;
            o[k++] = 5u << 16 | 199; o[k++] = I; o[k++] = pe; o[k++] = pvi; o[k++] = cs[4];
            o[k++] = 5u << 16 | 194; o[k++] = I; o[k++] = pt; o[k++] = pvi; o[k++] = cs[5];
            o[k++] = 5u << 16 | 194; o[k++] = I; o[k++] = tg; o[k++] = pg; o[k++] = cs[6];
            o[k++] = 5u << 16 | 171; o[k++] = B; o[k++] = k1; o[k++] = pe; o[k++] = cs[7];
            o[k++] = 5u << 16 | 170; o[k++] = B; o[k++] = k2; o[k++] = pt; o[k++] = tg;
            if (I != uty) { o[k++] = 4u << 16 | 124; o[k++] = uty; o[k++] = hu; o[k++] = h; } else hu = h;
            o[k++] = 5u << 16 | 176; o[k++] = B; o[k++] = k3; o[k++] = hu; o[k++] = cs[8];
            o[k++] = 5u << 16 | 167; o[k++] = B; o[k++] = k4; o[k++] = k1; o[k++] = k2;
            o[k++] = 5u << 16 | 167; o[k++] = B; o[k++] = ok; o[k++] = k4; o[k++] = k3;
            o[k++] = 3u << 16 | 62; o[k++] = f->sel; o[k++] = pe;   // the page's entry, which the search overwrites where it runs
            o[k++] = 3u << 16 | 247; o[k++] = f->sm; o[k++] = 0;
            o[k++] = 4u << 16 | 250; o[k++] = ok; o[k++] = f->sm; o[k++] = f->sr;
            o[k++] = 2u << 16 | 248; o[k++] = f->sr; }
    }
    #undef GP_C
    #undef GP_OP
    o[3] = nb_;
    return k;
}
