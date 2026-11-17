// A summary of the guest memory pending writes cover (write-backs and queued copies), so a lookup that can't overlap any of them skips the scan of their lists
// (gfx_vk.c chunk_hits, wbf_fly_clear; gfx_vk_draw.inc fwd_hit, fwd_find). Every read the replay makes of guest memory asks whether a write still pending covers it:
// the chunk being recorded, then each chunk in flight, entry by entry, under the chunks' lock. In a Mushroom Cup race on a Galaxy Z Fold 8 (Adreno 840) Mario Kart World
// made about 75,000 chunk scans a frame in chunk_hits and 120,000 in fwd_hit (20,000 fwd_find lookups), about 2.2 million range tests a frame for a handful of real
// overlaps: fwd_find and ranges_view_ were 11% of the replay thread (about 2.8 ms of 25 ms a frame), the most of anything of the host's own.
// A summary is a 512-bit set of the 64 KiB granules its ranges touch (a bit by a hash of the granule), and up to `maxwide` ranges of more than WBF_SPAN granules kept as
// they are; more wide ranges than that, and it rules nothing out (ok 0: every lookup scans, as before). A lookup that meets no granule bit and no wide range can't
// overlap any range given: two ranges overlap only where they share a granule. Bits of other granules that hash alike only send a lookup on to the scan.
// Each chunk gets one when it is submitted and its lists stop growing (the chunk being recorded is always scanned), and the chunks in flight one together (wbf_merge).
#pragma once
#include <stdint.h>
#include <string.h>

enum { WBF_SH = 16, WBF_BITS = 512, WBF_SPAN = 16, WBF_WIDE = 4, WBF_UWIDE = 16 };   // WBF_WIDE: a chunk's wide ranges at most; WBF_UWIDE: the chunks in flight together
typedef struct { uint64_t bit[WBF_BITS / 64]; uintptr_t wlo[WBF_UWIDE], whi[WBF_UWIDE]; int nwide, maxwide, ok; } wb_filter;

static inline unsigned wbf_hash(uintptr_t g) { return (unsigned)(((uint64_t)g * 0x9e3779b97f4a7c15ull) >> (64 - 9)); }   // 0..511
static inline void wbf_reset(wb_filter *f, int maxwide) { memset(f, 0, sizeof *f); f->maxwide = maxwide < WBF_UWIDE ? maxwide : WBF_UWIDE; f->ok = 1; }
static inline void wbf_add(wb_filter *f, uintptr_t lo, uintptr_t hi) {   // [lo, hi): a range the chunk writes (or a queued copy reads)
    if (!f->ok || hi <= lo) return;
    uintptr_t g0 = lo >> WBF_SH, g1 = (hi - 1) >> WBF_SH;
    if (g1 - g0 >= WBF_SPAN) { if (f->nwide >= f->maxwide) { f->ok = 0; return; } f->wlo[f->nwide] = lo; f->whi[f->nwide++] = hi; return; }
    for (uintptr_t g = g0; g <= g1; g++) { unsigned b = wbf_hash(g); f->bit[b >> 6] |= 1ull << (b & 63); }
}
static inline void wbf_merge(wb_filter *f, const wb_filter *s) {   // what s covers into f
    if (!f->ok) return;
    if (!s->ok) { f->ok = 0; return; }
    for (int i = 0; i < WBF_BITS / 64; i++) f->bit[i] |= s->bit[i];
    for (int i = 0; i < s->nwide && f->ok; i++) { if (f->nwide >= f->maxwide) { f->ok = 0; return; } f->wlo[f->nwide] = s->wlo[i]; f->whi[f->nwide++] = s->whi[i]; }
}
// 0: no range given overlaps [lo, hi); 1: one may (scan the lists). An empty or inverted range, or one of more than WBF_SPAN granules, is scanned as before.
// A torn read (wbf_fly_clear reads the chunks' summary while the completion thread writes it, and throws the answer away then) stays inside the arrays
static inline int wbf_may(const wb_filter *f, uintptr_t lo, uintptr_t hi) {
    if (!f->ok || hi <= lo) return 1;
    int nw = f->nwide < WBF_UWIDE ? f->nwide : WBF_UWIDE;
    for (int i = 0; i < nw; i++) if (f->wlo[i] < hi && lo < f->whi[i]) return 1;
    uintptr_t g0 = lo >> WBF_SH, g1 = (hi - 1) >> WBF_SH;
    if (g1 - g0 >= WBF_SPAN) return 1;
    for (uintptr_t g = g0; g <= g1; g++) { unsigned b = wbf_hash(g); if (f->bit[b >> 6] >> (b & 63) & 1) return 1; }
    return 0;
}
