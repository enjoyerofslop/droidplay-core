// Whether a compute shader reads its dispatch's group counts from its c0 bank, where the replay writes them (gfx_vk_draw.inc bank_c0: DMSH_C0_DISPATCH, and
// DMSH_C0_DISPATCH_NVN2 for NVN2 games). An indirect dispatch whose arguments are on the GPU copies the counts into the bank on the GPU before it runs (vk_dispatch), a
// transfer between two full barriers; a shader that never reads them needs no copy, and its dispatch can then follow the dispatch before it as a direct one does. The
// translator declares a constant buffer as one block of vec4[4096] at stride 16 (member 0), read through access chains: the counts are elements 0 (NVN2: words 1-3) and
// 32 (words 0-2). Any other use of the bank's variable, an element that is not a constant, a layout not recognized, or decoration groups count as reading them, so the
// answer can only be "reads" where it is unsure. Self-contained (no libc) for dm/tools/tests/test_spv_cdim.py.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "spv_lit.h"

enum { SPV_CDIM_EL_NVN2 = 0, SPV_CDIM_EL = 32 };   // the vec4 elements that hold the counts (DMSH_C0_DISPATCH_NVN2 / 16, DMSH_C0_DISPATCH / 16)

// w, n: the module; binding: the c0 bank's binding number (set 0); sc: bound words of scratch. 1: the counts may be read; 0: they are not; no variable with that
// binding: 0 (the shader reads no c0)
static int spv_cdim_read(const uint32_t *w, size_t n, uint32_t binding, uint32_t *sc) {
    if (n < 5 || w[0] != 0x07230203) return 1;
    uint32_t bound = w[3];
    for (uint32_t i = 0; i < bound; i++) sc[i] = 0;   // 1: an id decorated with the binding
    uint32_t var = 0, nvar = 0;
    for (size_t i = 5; i < n; ) { uint32_t len = w[i] >> 16, op = w[i] & 0xffff; if (!len || i + len > n) return 1;
        if (op >= 73 && op <= 75) return 1;   // OpDecorationGroup / OpGroupDecorate / OpGroupMemberDecorate: not followed
        if (op == 71 && len >= 4 && w[i + 2] == 33 && w[i + 3] == binding && w[i + 1] < bound) { sc[w[i + 1]] = 1; }   // OpDecorate id Binding b
        if (op == 54) break;
        i += len; }
    for (size_t i = 5; i < n; ) { uint32_t len = w[i] >> 16, op = w[i] & 0xffff;   // the variable with that binding (another set's would be a second: unsure)
        if (op == 59 && len >= 4 && w[i + 2] < bound && sc[w[i + 2]] == 1) { var = w[i + 2]; nvar++; }
        if (op == 54) break;
        i += len; }
    if (!nvar) return 0;
    if (nvar > 1) return 1;
    for (uint32_t i = 0; i < bound; i++) sc[i] = 0xffffffffu;   // each 32-bit integer constant's value (~0: not one)
    for (size_t i = 5; i < n; ) { uint32_t len = w[i] >> 16, op = w[i] & 0xffff;
        if (op == 43 && len == 4 && w[i + 2] < bound && w[i + 3] != 0xffffffffu) sc[w[i + 2]] = w[i + 3];
        if (op == 54) break;
        i += len; }
    int fn = 0;
    for (size_t i = 5; i < n; ) { uint32_t len = w[i] >> 16, op = w[i] & 0xffff;
        if (op == 54) fn = 1;   // the functions come after every global declaration
        if (!fn) { if (op == 59 && len >= 5 && w[i + 4] == var) return 1; i += len; continue; }   // a global initialized from it
        for (uint32_t q = 1; q < len; q++) {
            if (w[i + q] != var || spv_lit(op, q)) continue;
            if ((op == 65 || op == 66) && q == 3 && len >= 6) {   // OpAccessChain type result base member element...
                if (w[i + 4] >= bound || sc[w[i + 4]] != 0 || w[i + 5] >= bound) return 1;
                uint32_t el = sc[w[i + 5]];
                if (el == 0xffffffffu || el == SPV_CDIM_EL_NVN2 || el == SPV_CDIM_EL) return 1;
                continue; }
            return 1; }
        i += len; }
    return 0;
}
