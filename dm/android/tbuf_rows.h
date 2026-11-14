// A buffer texture read through a 2D texture slot, as rows of 16384 texels (gfx_vk.c vt_create / vt_upload_, gfx_vk_draw.inc tex_rows_slot), as plain functions, so
// dm/tools/tests/test_tbuf_rows.py checks them on the host and dm/android/tests/t_tbuf_rows.c on a GPU.
// The translator reads a 1D integer fetch whose texture type it has no answer for as a 2D fetch at (x & 0x3FFF, x >> 14) (Program.cs RecordTexelFetch1D, res.json
// "fetch1d"), and declares a texture whose handle sits in a uniform buffer 2D: either slot may hold a 1D texture or a buffer texture, which only the bound descriptor
// tells. The Mac binds a buffer texture there as a linear 2D texture of such rows over the same memory (nvn_mtl.m tbuf_as_2d, nvn_tex.c nvn_tbuf_rows). Vulkan can't view
// buffer memory as an optimal 2D image, so the replay makes one and fills it from guest memory: element i lands at (i % 16384, i / 16384), which the wrapped address
// reads back; the last row's texels past the buffer read 0.
#pragma once
#include <stdint.h>
#include <string.h>

#define TBR_ROW 16384u   // texels in a row: the translator's x & 0x3FFF (nvn_tex.h NVN_TBUF_ROW)

// The translator's slot number of a texture binding (Program.cs Slot), as res.json's "fetch1d" lists them: from the handle's offsets into the driver's table (c2), a
// texture slot from 0x20 on, a separate texture from 0x168 on (slot 128 and up); -1 for a handle elsewhere. cbuf_slot: the binding's texture bank, -1 for none given (c2)
static inline int tbr_slot(uint32_t handle, int cbuf_slot) {
    if (cbuf_slot >= 0 && cbuf_slot != 2) return -1;
    uint32_t w[2] = {handle & 0x3fff, (handle >> 14) & 0x3fff}, ty = handle >> 28;   // TextureHandle.UnpackOffsets
    int nw = ty == 0 || ty == 3 ? 1 : 2;   // CombinedSampler and SeparateConstantSamplerHandle name one word, the others a sampler word as well
    for (int i = 0; i < nw; i++) {
        uint32_t b = w[i] * 4;
        if (b % 8) continue;
        if (b >= 0x20 && b < 0x168) return (int)((b - 0x20) / 8);
        if (b >= 0x168 && b < 0x568) return 128 + (int)((b - 0x168) / 8);
    }
    return -1;
}
// The rows of n elements: width (n up to a row, else a whole row) and height. 0 when n is 0 or a side would pass max_dim (the GPU's maxImageDimension2D)
static inline int tbr_shape(uint64_t n, uint32_t max_dim, uint32_t *w, uint32_t *h) {
    if (!n) return 0;
    uint64_t rw = n < TBR_ROW ? n : TBR_ROW, rh = (n + TBR_ROW - 1) / TBR_ROW;
    if (rw > max_dim || rh > max_dim) return 0;
    *w = (uint32_t)rw; *h = (uint32_t)rh;
    return 1;
}
// The image's texels, packed (w * bpb bytes a row), from the buffer's n bytes: the buffer's bytes are already rows of w texels back to back (a full row is TBR_ROW
// texels, and only the last may be short), so they are copied as they are and the rest of the last row is 0. out: the image's bytes, w * h * bpb
static inline void tbr_fill(uint8_t *dst, size_t out, const uint8_t *src, size_t n) {
    size_t k = n < out ? n : out;
    memcpy(dst, src, k);
    memset(dst + k, 0, out - k);
}
