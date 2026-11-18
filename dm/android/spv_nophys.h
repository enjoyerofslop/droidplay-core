// A module's loads and stores through physical pointers taken out, for a device made without raw global memory (gfx_vk_gmem.inc off: no bufferDeviceAddress, no pool
// placed, the raw pool table empty). The translator's raw global memory helpers look the address up in the table and dereference a pointer only for an entry they find,
// so with an empty table every raw load should read 0. Arm's Mali lost the GPU on them all the same (TRANSLATION_FAULT in the load/store cache at a guest-like address,
// Ocarina of Time's lighting pixel shaders at Kokiri's first frame), with the table empty and with robustBufferAccess: the pointer was read whatever the lookup said. Here
// each OpLoad through a PhysicalStorageBuffer pointer becomes a copy of a null constant of its type, each atomic with a result the same, and each store, atomic store or
// memory copy into one is dropped, so the shader reads 0 where the table would have sent it nowhere, as on a device that never had the pointers. Self-contained (no
// libc) for dm/tools/tests/test_spv_nophys.py.
#pragma once
#include <stddef.h>
#include <stdint.h>

#define SPV_NOPHYS_TYPES 32   // distinct result types loaded through physical pointers in one module, at most
// w[n]: the module. scratch: 2 × bound words (w[3]). o[cap]: the result (n + 4 × SPV_NOPHYS_TYPES words is enough). → its word count, or 0 when nothing changes (no
// physical pointer access) or it cannot be done (more types than fit, o too small); *changed: the accesses replaced or dropped
static size_t spv_nophys(const uint32_t *w, size_t n, uint32_t *scratch, uint32_t *o, size_t cap, int *changed) {
    *changed = 0;
    if (n < 5 || w[0] != 0x07230203u || cap < n) return 0;
    uint32_t bound = w[3], *phys = scratch, *ty = scratch + bound;   // phys[pointer type]: 1 when its class is PhysicalStorageBuffer; ty[id]: a pointer-valued id's type
    for (uint32_t i = 0; i < 2 * bound; i++) scratch[i] = 0;
    uint32_t lt[SPV_NOPHYS_TYPES], nul[SPV_NOPHYS_TYPES], nt = 0, id = bound; size_t fn_at = 0;
#define ID(x) ((x) < bound)
    for (size_t i = 5; i < n; ) {
        uint32_t len = w[i] >> 16, op = w[i] & 0xffff; const uint32_t *a = w + i + 1;
        if (!len || i + len > n) return 0;
        if ((op == 32 || op == 39) && len > 2 && ID(a[0]) && a[1] == 5349) phys[a[0]] = 1;   // OpTypePointer / OpTypeForwardPointer PhysicalStorageBuffer
        if ((op == 59 || op == 65 || op == 66 || op == 67 || op == 70 || op == 83 || op == 120 || op == 124 || op == 55 || op == 61 || op == 169 || op == 245) && len > 2 && ID(a[1])) ty[a[1]] = a[0];
        if (op == 54 && !fn_at) fn_at = i;   // the first OpFunction: the null constants go before it
        i += len;
    }
    if (!fn_at) return 0;
#define PHYS(p) (ID(p) && ty[p] && ID(ty[p]) && phys[ty[p]])
    for (size_t i = fn_at; i < n; ) {   // the result types read through them
        uint32_t len = w[i] >> 16, op = w[i] & 0xffff; const uint32_t *a = w + i + 1; uint32_t p = 0;
        if (op == 61 && len >= 4) p = a[2];
        if ((op == 227 || (op >= 229 && op <= 242) || op == 6035 || op == 5614 || op == 5615) && len > 3) p = a[2];
        if (p && PHYS(p)) { uint32_t t = a[0], k = 0; while (k < nt && lt[k] != t) k++;
            if (k == nt) { if (nt == SPV_NOPHYS_TYPES) return 0; lt[nt] = t; nul[nt++] = id++; } }
        i += len;
    }
    size_t k = 0; int nch = 0;
    for (int q = 0; q < 5; q++) o[k++] = w[q];
    for (size_t i = 5; i < n; ) {
        uint32_t len = w[i] >> 16, op = w[i] & 0xffff; const uint32_t *a = w + i + 1;
        if (i == fn_at) { if (k + 3 * (size_t)nt > cap) return 0; for (uint32_t t = 0; t < nt; t++) { o[k++] = 3u << 16 | 46; o[k++] = lt[t]; o[k++] = nul[t]; } }   // OpConstantNull
        uint32_t p = 0; int load = 0;
        if (op == 61 && len >= 4) { p = a[2]; load = 1; }
        if ((op == 227 || (op >= 229 && op <= 242) || op == 6035 || op == 5614 || op == 5615) && len > 3) { p = a[2]; load = 1; }
        if ((op == 62 || op == 63 || op == 64 || op == 228) && len > 1) p = a[0];   // OpStore, OpCopyMemory(Sized), OpAtomicStore: the target
        if (p && PHYS(p)) {
            nch++;
            if (load) { uint32_t t = 0; while (t < nt && lt[t] != a[0]) t++;
                if (t == nt || k + 4 > cap) return 0;
                o[k++] = 4u << 16 | 83; o[k++] = a[0]; o[k++] = a[1]; o[k++] = nul[t]; }   // OpCopyObject of the null
            i += len; continue; }
        if (k + len > cap) return 0;
        for (uint32_t q = 0; q < len; q++) o[k++] = w[i + q];
        i += len;
    }
#undef PHYS
#undef ID
    if (!nch) return 0;
    o[3] = id; *changed = nch;
    return k;
}
