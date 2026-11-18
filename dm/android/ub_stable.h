// Whether a game uniform buffer has been left alone long enough to be bound in place in its placed pool (gfx_vk_gmem_inplace.inc ub_inplace), from the dirty tracker's page
// generations. Self-contained: the tracker's calls come in as ubs_ops, so dm/tools/tests/test_ub_stable.py drives it with a fake tracker.
// A buffer's first look stamps its pages and write-protects them; it is stable once UBS_STABLE presents have passed with no page written since the stamp. A write since
// (the game's through the fault handler, a host write through dm_guest_write_prepare) moves a page's generation past the stamp: the buffer is not stable, and is stamped again
// only UBS_BACKOFF presents after the write was seen, so one the game rewrites every frame costs a fault every UBS_BACKOFF presents rather than every frame.
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { UBS_STABLE = 60, UBS_BACKOFF = 30 };

typedef struct {
    const void *(*span)(const void *addr, size_t n);                              // the tracked range holding [addr, addr + n), as a handle; NULL: untracked
    uint64_t (*now)(void);                                                        // the current generation
    void (*protect)(const void *span, const void *addr, size_t n);                // the pages read-only, so the next write to each is seen
    int (*since)(const void *span, const void *addr, size_t n, uint64_t stamp);   // 1: a page was written after the stamp
} ubs_ops;

// on: a stamp holds (none yet, or the last one was written past); since: the present it was taken at; wr: the present a write past it was seen at (0: none)
typedef struct { uint64_t a, n, stamp, since, wr; const void *span; int on; } ubs_ent;

static inline ubs_ent *ubs_slot(ubs_ent *tab, unsigned mask, uint64_t a, size_t n) { return &tab[(unsigned)((a >> 4) ^ (a >> 18) ^ (uint64_t)n * 0x9e37u) & mask]; }
// 1: bind [a, a + n) in place; 0: copy it. present: the presents so far. *stats (may be NULL): [0] entries made, [1] writes seen, [2] looks still within UBS_STABLE
static inline int ubs_look(ubs_ent *e, const ubs_ops *o, uint64_t a, size_t n, uint64_t present, uint64_t *stats) {
    const void *src = (const void *)(uintptr_t)a;
    if (e->a != a || e->n != n) { *e = (ubs_ent){a, n, 0, 0, 0, o->span(src, n), 0}; if (stats) stats[0]++; }
    else if (e->on && o->since(e->span, src, n, e->stamp)) { e->on = 0; e->wr = present ? present : 1; if (stats) stats[1]++; }
    if (!e->span) return 0;
    if (!e->on) {
        if (e->wr && present - e->wr < UBS_BACKOFF) return 0;
        e->stamp = o->now(); o->protect(e->span, src, n); e->since = present; e->on = 1;   // (the stamp before the protect: a write after either moves past it)
        return 0;
    }
    if (present - e->since >= UBS_STABLE) return 1;
    if (stats) stats[2]++;
    return 0;
}
