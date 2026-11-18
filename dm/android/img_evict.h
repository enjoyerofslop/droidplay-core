// The image eviction rules (gfx_vk_evict.inc) as plain functions of what the replay knows about an image, so dm/tools/tests/test_img_evict.py checks them on the host
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
// Clean: its content is still its storage's (a packaged texture, or a shared one filled from its storage when made), nothing on the GPU wrote it, the texture pool by
// ID never read it (a view dropped from the pool set reads 0 until the sync after a draw reads its ID again), and no lookup for age presents
static inline int ie_clean(int gpu_w, int pool_read, int pkg, int shared, int filled, uint64_t last_present, uint64_t now, uint64_t age) {
    return !gpu_w && !pool_read && (pkg || (shared && filled)) && now - last_present >= age;
}
// A shared image: its pool, its storage range there, the presents of its last lookup and of its last write by the GPU (0: none), and the caller's own pointer for it
typedef struct { uint64_t pool; int64_t lo, hi; uint64_t last_present, last_w; void *p; } ie_ent;
// a was overwritten by b: b was written after a's last lookup and covers at least half of a's storage (layouts are padded: an image another one only clips at its
// edge kept its content on the console), and a had no lookup for age presents
static inline int ie_overwritten(const ie_ent *a, const ie_ent *b, uint64_t now, uint64_t age) {
    int64_t lo = a->lo > b->lo ? a->lo : b->lo, hi = a->hi < b->hi ? a->hi : b->hi;
    return a->pool == b->pool && hi > lo && b->last_w > a->last_present && now - a->last_present >= age && 2 * (hi - lo) >= a->hi - a->lo;
}
static inline int ie_cmp(const void *x, const void *y) {
    const ie_ent *a = x, *b = y;
    return a->pool != b->pool ? (a->pool < b->pool ? -1 : 1) : a->lo != b->lo ? (a->lo < b->lo ? -1 : 1) : 0;
}
// Sorts e by pool and offset and marks each entry another one overwrote (mark[i] for e[i] after the sort). Every pair whose storage overlaps is looked at once:
// sorted by offset, the entries that overlap e[i] from after it are exactly those that start before it ends
static inline void ie_sweep(ie_ent *e, uint32_t n, uint64_t now, uint64_t age, uint8_t *mark) {
    qsort(e, n, sizeof *e, ie_cmp);
    for (uint32_t i = 0; i < n; i++) mark[i] = 0;
    for (uint32_t i = 0; i < n; i++) for (uint32_t j = i + 1; j < n && e[j].pool == e[i].pool && e[j].lo < e[i].hi; j++) {
        if (ie_overwritten(&e[i], &e[j], now, age)) mark[i] = 1;
        if (ie_overwritten(&e[j], &e[i], now, age)) mark[j] = 1;
    }
}
// An image the pressure rule may take (its content made again by its next lookup, no lookup for a few presents): its last lookup, its bytes, the caller's pointer
typedef struct { uint64_t last_present, bytes; void *p; } ie_cand;
static inline int ie_cand_cmp(const void *x, const void *y) { const ie_cand *a = x, *b = y; return a->last_present != b->last_present ? (a->last_present < b->last_present ? -1 : 1) : 0; }
// Sorts c by last lookup and marks the least recently looked up until need bytes are chosen (mark[i] for c[i] after the sort); returns the bytes chosen
static inline uint64_t ie_pick(ie_cand *c, uint32_t n, uint64_t need, uint8_t *mark) {
    qsort(c, n, sizeof *c, ie_cand_cmp);
    uint64_t got = 0;
    for (uint32_t i = 0; i < n; i++) { mark[i] = got < need; if (mark[i]) got += c[i].bytes; }
    return got;
}
// A byte budget for the image memory (DM_VK_IMAGE_BUDGET_MB), for where memory pressure cannot be read or is read too late: the ages above go by time alone, and a game
// that makes images faster than they age grows them until lmkd ends it. Over the budget, the images either age rule would take, with no lookup for EV_BUDGET_AGE presents
// instead of the rule's own age, go least recently looked up first (ie_pick) until the image memory is IE_BUDGET_LOW_PCT of the budget, so the next scan is not due at the
// next new image. By default a quarter of the device's memory (MemTotal): above the 2.23 GB the ages alone held Ocarina of Time's Saria scene to on the Galaxy Z Fold 8
// (11.3 GB), so there it changes nothing and stays a ceiling. nx2android's image budget (4a42d83e3), written anew for these rules
#define IE_BUDGET_LOW_PCT 90
// A /proc/meminfo field in kB ("MemTotal:"); 0 when it isn't there
static inline uint64_t ie_meminfo_kb(const char *buf, const char *key) { const char *p = buf ? strstr(buf, key) : NULL; return p ? strtoull(p + strlen(key), NULL, 10) : 0; }
// The default budget in bytes from MemTotal in kB: a quarter of it (0, none, when MemTotal isn't known)
static inline uint64_t ie_budget_default(uint64_t mem_total_kb) { return mem_total_kb / 4 * 1024; }
// The image bytes to get down to: none (UINT64_MAX) while the images are within the budget or there is none (0); over it, IE_BUDGET_LOW_PCT percent of it
static inline uint64_t ie_budget_target(uint64_t live, uint64_t budget) { return !budget || live <= budget ? UINT64_MAX : budget / 100 * IE_BUDGET_LOW_PCT; }
