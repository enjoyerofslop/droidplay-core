// A vertex shader's memory writes taken out, for a rasterized draw on a GPU without vertexPipelineStoresAndAtomics (Arm's Mali). Its writes are made first by
// the shader's fragment form over points (spv_vsfrag.h, gfx_vk_draw.inc vsw_pre), and the draw itself then runs this variant, which Vulkan allows there: every
// store, memory copy, atomic store and image write into a storage buffer, raw global memory or a storage image is dropped, and each read-modify-write atomic
// (and atomic load) of a storage buffer becomes a plain load, which reads what the fragment form wrote. The shader's outputs are computed as before, so the
// picture is the same, except where an output depends on an atomic's result (it then sees the value after every vertex's atomic, not its own turn's).
// spv_readonly.h then finds nothing written and makes the storage resources NonWritable. Self-contained (no libc) for dm/tools/tests/test_spv_nostore.py.
#pragma once
#include <stddef.h>
#include <stdint.h>

// w[n]: the module. scratch: 2 × bound words (w[3]). o[cap]: the result (n words is enough). → the result's word count, or 0 when nothing is dropped (not a
// vertex-pipeline stage, no write) or it cannot be done (an atomic on an image texel, an atomic flag, o too small); *dropped: the writes dropped or made loads
static size_t spv_nostore(const uint32_t *w, size_t n, uint32_t *scratch, uint32_t *o, size_t cap, int *dropped) {
    *dropped = 0;
    if (n < 5 || w[0] != 0x07230203u || cap < n) return 0;
    uint32_t bound = w[3], *cls = scratch, *ty = scratch + bound;   // cls[pointer type]: its storage class + 1; ty[id]: a pointer-valued id's type
    for (uint32_t i = 0; i < 2 * bound; i++) scratch[i] = 0;
    int vtx = 0;
#define ID(x) ((x) < bound)
    for (size_t i = 5; i < n; ) {
        uint32_t len = w[i] >> 16, op = w[i] & 0xffff; const uint32_t *a = w + i + 1;
        if (!len || i + len > n) return 0;
        if (op == 15 && len > 1 && a[0] <= 3) vtx = 1;   // OpEntryPoint Vertex, TessellationControl, TessellationEvaluation, Geometry
        if ((op == 32 || op == 39) && len > 2 && ID(a[0])) cls[a[0]] = a[1] + 1;   // OpTypePointer, OpTypeForwardPointer
        // results that may be pointers: their type, to find the class a write goes to
        if ((op == 59 || op == 65 || op == 66 || op == 67 || op == 70 || op == 83 || op == 60 || op == 120 || op == 124 || op == 55 || op == 61 || op == 169 || op == 245) && len > 2 && ID(a[1])) ty[a[1]] = a[0];
        i += len;
    }
    if (!vtx) return 0;
    size_t k = 0; int n_drop = 0;
    for (int q = 0; q < 5; q++) o[k++] = w[q];
    for (size_t i = 5; i < n; ) {
        uint32_t len = w[i] >> 16, op = w[i] & 0xffff; const uint32_t *a = w + i + 1;
        uint32_t p = 0; int atom = 0;   // the pointer written through, for the ops that write through one; atom: an atomic with a result
        if ((op == 62 || op == 63 || op == 64 || op == 228) && len > 1) p = a[0];   // OpStore, OpCopyMemory(Sized), OpAtomicStore: the target
        if ((op == 227 || (op >= 229 && op <= 242) || op == 6035 || op == 5614 || op == 5615) && len > 3) { p = a[2]; atom = 1; }   // OpAtomicLoad, the read-modify-write atomics (and FAdd/FMin/FMax EXT)
        uint32_t c = p && ID(p) && ty[p] && ID(ty[p]) ? cls[ty[p]] : 0;   // the class + 1
        int mem = c == 2 + 1 || c == 12 + 1 || c == 5349 + 1 || c == 11 + 1;   // Uniform (a BufferBlock: a uniform buffer takes no write), StorageBuffer, PhysicalStorageBuffer, Image (a texel pointer)
        if (op == 318 || op == 319) return 0;   // OpAtomicFlagTestAndSet / Clear
        if (op == 99) { n_drop++; i += len; continue; }   // OpImageWrite: in a vertex shader, a storage image's
        if (mem && !atom) { n_drop++; i += len; continue; }   // a store, memory copy or atomic store: dropped
        if (mem && atom) {   // an atomic with a result: a plain load of what is there
            if (c == 11 + 1) return 0;   // an image texel's: no load of an Image pointer
            if (k + 4 > cap) return 0;
            o[k++] = 4u << 16 | 61; o[k++] = a[0]; o[k++] = a[1]; o[k++] = a[2];
            n_drop++; i += len; continue; }
        if (k + len > cap) return 0;
        for (uint32_t q = 0; q < len; q++) o[k++] = w[i + q];
        i += len;
    }
#undef ID
    *dropped = n_drop;
    return n_drop ? k : 0;
}
