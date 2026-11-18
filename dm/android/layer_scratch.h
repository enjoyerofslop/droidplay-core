// Which render passes draw into one-layer scratch images instead of their targets (gfx_vk_layerscratch.inc layer_scratch): a pass of one layer whose colour targets
// are two or more different layers of one level of one image. Qualcomm's driver on the Adreno 830 (0x80320048) puts such a pass's outputs into the wrong layers.
// Self-contained (no Vulkan headers), so that dm/tools/tests/test_layer_scratch.py builds it alone.
#pragma once
#include <stdint.h>

// a colour slot of the pass: img 0 when the slot is empty; lv, ly its level and layer; plain: an array or cube image (2D, more than one layer), one sample, colour, held
// as its own format (not converted on the CPU, no BC shadow) and attached in that format, so a copy of the layer moves its texels unchanged
typedef struct { uintptr_t img; uint32_t lv, ly; int plain; } lsc_slot;

// the number of slots the pass draws into scratch images (every used one), or 0: it draws into its targets. layers: the framebuffer's (a layered pass attaches whole
// arrays, not this case); samples: the pass's
static inline int lsc_takes(const lsc_slot *s, int n, uint32_t layers, uint32_t samples) {
    if (layers != 1 || samples != 1 || n > 8) return 0;
    int used = 0; const lsc_slot *first = 0;
    for (int i = 0; i < n; i++) {
        if (!s[i].img) continue;
        if (!s[i].plain) return 0;
        if (!first) first = &s[i];
        else if (s[i].img != first->img || s[i].lv != first->lv) return 0;   // another image, or another level of it
        for (int j = 0; j < i; j++) if (s[j].img && s[j].ly == s[i].ly) return 0;   // one layer in two slots (dup_slots keeps one; with DM_VK_DUP_SLOTS=0 both stay)
        used++;
    }
    return used >= 2 ? used : 0;
}
