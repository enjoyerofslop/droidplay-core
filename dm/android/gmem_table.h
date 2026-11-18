// The raw pool table as the translator's helper reads it (dmsh_abi.h DMSH_GMEM_MAP_*; Ryujinx GlobalToStorage.TryGenerateRawStorageOp), laid out as nvn_mtl.m gmem_map does it on
// the Mac, free of Vulkan so the tool tests can build it on the PC (dm/tools/tests/test_gmem_table.py). gfx_vk_gmem.inc fills it from the placed pool pieces.
#ifndef GMEM_TABLE_H
#define GMEM_TABLE_H
#include <stdint.h>
#include <string.h>
#include "../gfx/nvn/dmsh_abi.h"

enum { GMEM_TAB_BYTES = DMSH_GMEM_MAP_ENTRIES * DMSH_GMEM_MAP_ENTRY, GMEM_INDEX_BITS = 13, GMEM_HINT_BYTE = 24 };
_Static_assert(DMSH_GMEM_MAP_ENTRIES == 1 << GMEM_INDEX_BITS, "a hint's index bits are the table's");
// The page map after the table (spv_gmpage.h reads it): one word per 4 KiB guest page, by its low GMEM_PAGE_BITS bits, holding the entry that holds the whole page in the low
// 13 bits and the page's high bits (page >> GMEM_PAGE_BITS) above them, 0 = none. Pages below 2^28 (addresses below 2^40, the hint's own limit) fit: 10 tag bits
enum { GMEM_PAGE_SHIFT = 12, GMEM_PAGE_BITS = 18, GMEM_PAGE_WORD = GMEM_TAB_BYTES / 4, GMEM_PAGED_BYTES = GMEM_TAB_BYTES + (4 << GMEM_PAGE_BITS) };

// Fill the table t (GMEM_TAB_BYTES) from n ranges seg[i] = {start, end, delta}, sorted by start and disjoint. Entry 0 stays empty (start 0, end 0): an invocation's cached
// entry starts at 0, so its first lookup searches. Unused entries start at ~0, so the search never stops on one. Each 2 MiB chunk wholly inside an entry gets a hint in entry
// (chunk & 8191): the chunk in the high bits, the entry in the low 13; a later chunk at the same entry replaces it, and the tag sends the other to the search. Hints only for
// chunks below 2^19 (addresses below 2^40: the helper takes a hint only when the address's high word is under 256). Returns the entries used; *over: ranges left out
static inline int gmem_table_fill(uint8_t *t, const uint64_t (*seg)[3], int n, int *over) {
    memset(t, 0, GMEM_TAB_BYTES);
    for (int k = 1; k < DMSH_GMEM_MAP_ENTRIES; k++) memset(t + (size_t)k * DMSH_GMEM_MAP_ENTRY, 0xff, 8);
    int e = 1; *over = 0;
    for (int i = 0; i < n; i++) { if (e == DMSH_GMEM_MAP_ENTRIES) { (*over)++; continue; } memcpy(t + (size_t)e++ * DMSH_GMEM_MAP_ENTRY, seg[i], 24); }
    for (int k = 1; k < e; k++) {
        uint64_t r[2]; memcpy(r, t + (size_t)k * DMSH_GMEM_MAP_ENTRY, 16);
        uint64_t sz = 1ull << DMSH_GMEM_HINT_SHIFT, first = (r[0] + sz - 1) >> DMSH_GMEM_HINT_SHIFT, last = r[1] >> DMSH_GMEM_HINT_SHIFT;
        for (uint64_t c = first; c < last && c < (1ull << (32 - GMEM_INDEX_BITS)); c++) {
            uint32_t hint = (uint32_t)(c << GMEM_INDEX_BITS | (unsigned)k);
            memcpy(t + (size_t)(c & (DMSH_GMEM_MAP_ENTRIES - 1)) * DMSH_GMEM_MAP_ENTRY + GMEM_HINT_BYTE, &hint, 4);
        }
    }
    return e;
}
// Fill the page map of table t (GMEM_PAGED_BYTES, gmem_table_fill done; e: the entries it used) for the pages its chunk hints don't reach: every 4 KiB page wholly inside an
// entry, where the hint of the page's 2 MiB chunk names another entry or none. A page whose word another page took first keeps the search (the tag tells them apart).
// Returns the pages mapped; *lost: pages left to the search for a taken word
static inline int gmem_table_pages(uint8_t *t, int e, int *lost) {
    uint32_t *pm = (uint32_t *)(void *)(t + GMEM_TAB_BYTES); memset(pm, 0, 4u << GMEM_PAGE_BITS);
    int got = 0; *lost = 0;
    for (int k = 1; k < e; k++) {
        uint64_t r[2]; memcpy(r, t + (size_t)k * DMSH_GMEM_MAP_ENTRY, 16);
        uint64_t p = (r[0] + (1u << GMEM_PAGE_SHIFT) - 1) >> GMEM_PAGE_SHIFT, last = r[1] >> GMEM_PAGE_SHIFT;
        if (last > 1ull << 28) last = 1ull << 28;
        while (p < last) {
            uint64_t c = p >> (DMSH_GMEM_HINT_SHIFT - GMEM_PAGE_SHIFT); uint32_t hint; memcpy(&hint, t + (size_t)(c & (DMSH_GMEM_MAP_ENTRIES - 1)) * DMSH_GMEM_MAP_ENTRY + GMEM_HINT_BYTE, 4);
            uint64_t cend = (c + 1) << (DMSH_GMEM_HINT_SHIFT - GMEM_PAGE_SHIFT);
            if (hint == (uint32_t)(c << GMEM_INDEX_BITS | (unsigned)k)) { p = cend; continue; }   // the chunk's hint names this entry: the helper takes it before the page map
            for (; p < last && p < cend; p++) { uint32_t *w = &pm[p & ((1u << GMEM_PAGE_BITS) - 1)];
                if (*w) { (*lost)++; continue; }
                *w = (uint32_t)(p >> GMEM_PAGE_BITS) << GMEM_INDEX_BITS | (unsigned)k; got++; }
        }
    }
    return got;
}
// The entry the translator's helper takes for address a, starting from the invocation's cached entry *last (0 at first), and the cache it leaves; 0: in no entry (the access
// reads 0, a store goes nowhere). The helper's order: the cached entry's range, the hint of a's 2 MiB chunk (tagged, a's high word under 256), with paged the page map
// (spv_gmpage.h), the 13-step search over starts
static inline uint32_t gmem_table_find(const uint8_t *t, uint64_t a, uint32_t *last, int paged) {
    uint64_t r[2]; memcpy(r, t + (size_t)*last * DMSH_GMEM_MAP_ENTRY, 16);
    if (r[0] <= a && a < r[1]) return *last;
    uint32_t chunk = (uint32_t)(a >> DMSH_GMEM_HINT_SHIFT), hint; memcpy(&hint, t + (size_t)(chunk & (DMSH_GMEM_MAP_ENTRIES - 1)) * DMSH_GMEM_MAP_ENTRY + GMEM_HINT_BYTE, 4);
    if ((hint & (DMSH_GMEM_MAP_ENTRIES - 1)) && hint >> GMEM_INDEX_BITS == chunk && (a >> 32) < 256) return *last = hint & (DMSH_GMEM_MAP_ENTRIES - 1);
    if (paged && (a >> 32) < 256) {   // then the page map (spv_gmpage.h's form)
        uint32_t page = (uint32_t)(a >> GMEM_PAGE_SHIFT), pw; memcpy(&pw, t + GMEM_TAB_BYTES + 4 * (size_t)(page & ((1u << GMEM_PAGE_BITS) - 1)), 4);
        if ((pw & (DMSH_GMEM_MAP_ENTRIES - 1)) && pw >> GMEM_INDEX_BITS == page >> GMEM_PAGE_BITS) return *last = pw & (DMSH_GMEM_MAP_ENTRIES - 1); }
    uint32_t i = 0;
    for (uint32_t step = DMSH_GMEM_MAP_ENTRIES / 2; step; step >>= 1) { uint64_t s; memcpy(&s, t + (size_t)(i + step) * DMSH_GMEM_MAP_ENTRY, 8); if (s <= a) i += step; }
    memcpy(r, t + (size_t)i * DMSH_GMEM_MAP_ENTRY, 16);
    if (!(r[0] <= a && a < r[1])) return 0;
    return *last = i;
}
#endif
