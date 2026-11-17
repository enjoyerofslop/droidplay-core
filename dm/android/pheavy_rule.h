// The rules for pipelines that hold a scratch block (gfx_vk_pheavy.inc) as plain functions of what the replay knows about a pipeline, so
// dm/tools/tests/test_pheavy_rule.py checks them on the host
#pragma once
#include <stdint.h>
// A driver object this big, made with a pipeline, is its scratch block: Qualcomm's smallest block is 8 MiB (the Adreno 730; 12 MiB for a compute pipeline and 24 MiB
// for a graphics one on the 740), its largest code objects about 1 MB
#define PHR_MIN_BYTES (4ull << 20)
static inline uint64_t phr_block(uint64_t largest) { return largest >= PHR_MIN_BYTES ? largest : 0; }
// The age in presents a scratch pipeline may stay unbound: the short one while the device's MemAvailable (MiB, 0: unread) is under low_mib
static inline uint64_t phr_age(uint64_t avail_mib, uint64_t age, uint64_t low_age, uint64_t low_mib) { return avail_mib && avail_mib < low_mib ? low_age : age; }
// Whether the sweep destroys it: it holds a block, is made (not being built, not destroyed already), no draw or dispatch bound it for age presents, and the chunk that last
// bound it has completed (seq against the completion thread's done)
static inline int phr_goes(uint64_t block, int made, uint64_t last, uint64_t seq, uint64_t now, uint64_t done, uint64_t age) {
    return block && made && last + age <= now && seq <= done;
}
