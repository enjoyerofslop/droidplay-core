// Address ranges sorted by base, with the largest end so far at each entry: the ranges that can meet [lo, hi) are found by a binary search for the first entry whose
// running end passes lo, then a walk that stops at the first base at or past hi, instead of a walk over every range. Ranges may overlap and nest (adjacent pools share
// their rounded boundary pages), which is why the search is on the running end rather than on each entry's own. nvn_dirty.c prep_write (dm/tools/tests/test_range_index.py)
#ifndef RANGE_INDEX_H
#define RANGE_INDEX_H
#include <stdint.h>
#include <stdlib.h>

typedef struct { uintptr_t lo, end; int idx; } ri_e;   // [lo, end) and the caller's own number for it
typedef struct { uint64_t gen; int n, cap; ri_e *e; uintptr_t *mx; } ri_t;   // gen: the caller's, for when the index was made; mx[k]: the largest end of e[0..k]

static int ri_cmp(const void *x, const void *y) { uintptr_t a = ((const ri_e *)x)->lo, b = ((const ri_e *)y)->lo; return a < b ? -1 : a > b; }
// room for n entries (x->n is set to 0): 0 when out of memory, the index unchanged
static inline int ri_reserve(ri_t *x, int n) {
    if (n > x->cap) {
        int c = n + 256; ri_e *ne = (ri_e *)realloc(x->e, (size_t)c * sizeof *ne); if (!ne) return 0;
        x->e = ne; uintptr_t *nm = (uintptr_t *)realloc(x->mx, (size_t)c * sizeof *nm); if (!nm) return 0;
        x->mx = nm; x->cap = c;
    }
    x->n = 0; return 1;
}
static inline void ri_add(ri_t *x, uintptr_t lo, uintptr_t end, int idx) { x->e[x->n++] = (ri_e){lo, end, idx}; }   // after ri_reserve, within its count
static inline void ri_sort(ri_t *x) {   // once the entries are added
    qsort(x->e, (size_t)x->n, sizeof *x->e, ri_cmp);
    uintptr_t best = 0; for (int k = 0; k < x->n; k++) { if (x->e[k].end > best) best = x->e[k].end; x->mx[k] = best; }
}
// the first entry that can meet anything at or past lo: every entry before it ends at or before lo. The walk from it goes while e[k].lo < hi, and takes e[k] when e[k].end > lo
static inline int ri_first(const ri_t *x, uintptr_t lo) {
    int l = 0, h = x->n;
    while (l < h) { int m = (l + h) / 2; if (x->mx[m] > lo) h = m; else l = m + 1; }
    return l;
}
#endif
