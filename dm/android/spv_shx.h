// Every OpGroupNonUniformShuffle as OpGroupNonUniformShuffleXor, for Qualcomm's shader compilers that crash on the shuffle. The translator (Ryujinx's, in dmsh_xlate)
// writes each of Maxwell's SHFL forms (IDX, UP, DOWN, BFLY) as OpGroupNonUniformShuffle of a float to a lane it computes, and spv_wide keeps that form. Qualcomm's
// compiler in its V@07xx drivers dies on such a module: SIGSEGV at fault address 0x48 inside vkCreate*Pipelines (libllvm-qgl.so+0x2dcef0 on a Retroid Pocket 5's
// Adreno 650, V@0746; +0x463e84 on a Retroid Pocket G2's Adreno 735, V@0762), with the shuffle as the one thing the modules it kills have in common. Of Tears of
// the Kingdom's 257 compute shaders, 6 have a shuffle and 3 of them crashed the G2's compiler, as did 1 of the 2 of its 13,646 fragment shaders that have one, and
// Ocarina of Time's four crashing compute shaders have 1 to 65 each. The drivers before (V@0615, V@0676) and after (V@0800 and up) build the same modules.
// ShuffleXor by lane ^ index reads the same lane as Shuffle by index, whatever the index (lane ^ (lane ^ index) = index), and that compiler takes it: a shuffle
// becomes OpLoad of the SubgroupLocalInvocationId built-in, OpBitwiseXor with the index, and OpGroupNonUniformShuffleXor by the result, its value, type and scope
// as they were. A module without that built-in or a 32-bit unsigned int type is left as it is (every translated module with a shuffle has both). Run after the
// subgroup rewrites (spv_xor, spv_wide, spv_narrow), which read the translator's shuffles. Self-contained (no libc), so that dm/tools/tests/test_spv_shx.py builds
// it alone.
#pragma once
#include <stddef.h>
#include <stdint.h>

enum { SPV_SHX_SITE = 9 };   // the words one shuffle adds: OpLoad (4) and OpBitwiseXor (5)

// The shuffles in the module (0: none, or words that are no module): what spv_shx would rewrite
static int spv_shx_count(const uint32_t *w, size_t n) {
    if (n < 5 || w[0] != 0x07230203) return 0;
    int k = 0;
    for (size_t i = 5; i < n; ) {
        uint32_t len = w[i] >> 16;
        if (!len || i + len > n) return 0;
        if ((w[i] & 0xffff) == 345) k++;
        i += len;
    }
    return k;
}
// The module with every shuffle a ShuffleXor in o (cap words): its word count, or 0 when nothing was rewritten (no shuffle, no lane built-in or uint type, or cap
// too small); *nsites, when given, the shuffles rewritten. Needs cap >= n + SPV_SHX_SITE * spv_shx_count(w, n)
static size_t spv_shx(const uint32_t *w, size_t n, uint32_t *o, size_t cap, int *nsites) {
    if (nsites) *nsites = 0;
    int sh = spv_shx_count(w, n);
    if (!sh || cap < n + (size_t)SPV_SHX_SITE * (size_t)sh) return 0;
    uint32_t lane_dec = 0, lane = 0, tu = 0, bound = w[3];
    for (size_t i = 5; i < n; ) {   // the lane built-in's decoration, then the variable it names, and the 32-bit unsigned int
        uint32_t len = w[i] >> 16, op = w[i] & 0xffff; const uint32_t *x = w + i;
        if (op == 71 && len >= 4 && x[2] == 11 && x[3] == 41) lane_dec = x[1];   // OpDecorate %id BuiltIn SubgroupLocalInvocationId
        else if (op == 21 && len == 4 && x[2] == 32 && x[3] == 0 && !tu) tu = x[1];   // OpTypeInt 32 0
        else if (op == 59 && len >= 4 && lane_dec && x[2] == lane_dec && x[3] == 1) lane = x[2];   // OpVariable ... Input
        i += len;
    }
    if (!lane || !tu || bound + 2u * (uint32_t)sh < bound) return 0;
    size_t k = 0; int done = 0;
    for (size_t q = 0; q < 5; q++) o[k++] = w[q];
    for (size_t i = 5; i < n; ) {
        uint32_t len = w[i] >> 16, op = w[i] & 0xffff; const uint32_t *x = w + i; i += len;
        if (op == 345 && len == 6) {   // OpGroupNonUniformShuffle %type %r %scope %value %index
            uint32_t l = bound++, m = bound++;
            o[k++] = 4u << 16 | 61; o[k++] = tu; o[k++] = l; o[k++] = lane;   // OpLoad %uint %l %lane
            o[k++] = 5u << 16 | 198; o[k++] = tu; o[k++] = m; o[k++] = l; o[k++] = x[5];   // OpBitwiseXor %uint %m %l %index
            o[k++] = 6u << 16 | 346; o[k++] = x[1]; o[k++] = x[2]; o[k++] = x[3]; o[k++] = x[4]; o[k++] = m;   // OpGroupNonUniformShuffleXor %type %r %scope %value %m
            done++;
            continue;
        }
        for (uint32_t q = 0; q < len; q++) o[k++] = x[q];
    }
    if (!done) return 0;
    o[3] = bound;
    if (nsites) *nsites = done;
    return k;
}
