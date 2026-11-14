// The AAudio output's 5 ms pacer (coreaudio_aaudio.c): a ring of device-format PCM16 frames that a pacer thread fills in 5 ms quanta, calling the
// host's render callback (hle_audio.c au_render) as Core Audio does on the Mac and the iPhone, where the host asks for 5 ms buffers; the stream's data
// callback only copies frames out. Plain C, for dm/tools/tests/test_au_pace.py.
//
// Why: hle_audio.c releases a game's AudioOut buffer when the render callback has played it to the end, and many games append a buffer only when one
// comes back, keeping a few small ones queued (CRIWARE in DRAGON QUEST III HD-2D Remake: about 3, ~16 ms in all). A device that asks for 20 ms in one
// data callback (AAudio's burst on an AYN Thor Pro: 960 frames) found less than that queued at every callback: the rest was padded with zeros, and since
// the game makes sound only as fast as buffers come back, its sound played slowed down and crackling. A ring kept one device callback ahead, filled at a
// steady 5 ms, hands the buffers back at the console's own rate whatever the device's callback size.
#pragma once
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

// pcm: cap frames of ch channels; cap a multiple of the quantum, so a quantum written at a multiple of it never wraps. w: frames the pacer has written,
// r: frames the data callback has read (each written by one side only)
typedef struct { int16_t *pcm; uint32_t cap, ch, quantum; _Atomic uint64_t w, r; } aup_ring;

static void aup_init(aup_ring *g, int16_t *pcm, uint32_t cap, uint32_t ch, uint32_t quantum) {
    g->pcm = pcm; g->cap = cap - cap % quantum; g->ch = ch; g->quantum = quantum;
    atomic_init(&g->w, 0); atomic_init(&g->r, 0);
}
// Only while neither side runs (the stream stopped, the pacer joined)
static void aup_reset(aup_ring *g) { atomic_store(&g->w, 0); atomic_store(&g->r, 0); }
static uint64_t aup_fill(aup_ring *g) { return atomic_load_explicit(&g->w, memory_order_acquire) - atomic_load_explicit(&g->r, memory_order_acquire); }

// A data callback larger than one quantum is what starved the games (the Mac and the iPhone ask for one quantum and never needed the ring)
static int aup_wanted(uint32_t frames, uint32_t quantum) { return frames > quantum; }

// Frames the pacer keeps written ahead: the largest data callback seen, so one always finds it whole, plus two quanta for a late tick
static uint32_t aup_target(uint32_t callback_max, uint32_t quantum, uint32_t cap) {
    uint64_t t = (uint64_t)callback_max + 2ull * quantum;
    t = (t + quantum - 1) / quantum * quantum;
    return t > cap - quantum ? cap - quantum : (uint32_t)t;
}
// Quanta to render on a tick: up to the target, as many as the time that passed (due frames, one quantum on time), so the game gets one buffer back every
// 5 ms as from the console's mixer, not a whole device callback's worth at once. Below the ring's low point (target less the largest callback: where a
// callback on time leaves it) one more, which fills the ring at the start and makes up a device clock running faster than the pacer's
static uint32_t aup_quanta(uint64_t fill, uint32_t target, uint32_t callback_max, uint32_t quantum, uint64_t due) {
    if (fill >= target) return 0;
    uint64_t need = (target - fill + quantum - 1) / quantum, most = (due + quantum / 2) / quantum;
    if (!most) most = 1;
    if (fill + callback_max < target) most++;
    return (uint32_t)(need < most ? need : most);
}
// The pacer's next quantum to render into, then commit it once rendered
static int16_t *aup_slot(aup_ring *g) { return g->pcm + (size_t)(atomic_load_explicit(&g->w, memory_order_relaxed) % g->cap) * g->ch; }
static void aup_commit(aup_ring *g) { atomic_store_explicit(&g->w, atomic_load_explicit(&g->w, memory_order_relaxed) + g->quantum, memory_order_release); }
// The data callback: up to frames frames into dst; returns how many there were (the caller pads the rest)
static uint32_t aup_read(aup_ring *g, int16_t *dst, uint32_t frames) {
    uint64_t r = atomic_load_explicit(&g->r, memory_order_relaxed), fill = atomic_load_explicit(&g->w, memory_order_acquire) - r;
    uint32_t n = fill < frames ? (uint32_t)fill : frames, at = (uint32_t)(r % g->cap), first = n < g->cap - at ? n : g->cap - at;
    memcpy(dst, g->pcm + (size_t)at * g->ch, (size_t)first * g->ch * 2);
    if (n > first) memcpy(dst + (size_t)first * g->ch, g->pcm, (size_t)(n - first) * g->ch * 2);
    atomic_store_explicit(&g->r, r + n, memory_order_release);
    return n;
}
