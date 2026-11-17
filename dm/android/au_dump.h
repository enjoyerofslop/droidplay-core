// DM_AUDIO_DUMP's device side on Android (coreaudio_aaudio.c). hle_audio.c writes the blocks a game appends; a pop can also come from what happens after
// that (an empty queue when a quantum is rendered, a data callback the ring could not fill, a stream that ran late), so each AAudio stream also writes
// <file>.dev<K>: the PCM16 every data callback handed the stream (its channel count, 48 kHz), and <file>.dev<K>.ev: one aud_ev per data callback, pacer tick
// and change of the stream's xrun count. K counts the streams opened, in the order of hle_audio.c's outputs (#N). The real-time side only copies into two
// single-producer rings (the data callback's, the pacer's); a writer thread empties them every 20 ms, and a full ring drops the record and counts it.
#pragma once
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// kind_n: kind in the top 4 bits, frames (a data callback: asked; a tick: quanta rendered) below. fill: the ring's frames before (UINT32_MAX: rendered straight, no ring).
// x: a tick's lateness in µs past its 5 ms mark; a data callback's frames taken straight from the game's queue; an xrun record's count.
// y: a tick's target; a data callback's frames padded with zeros (written once the callback has filled its buffer)
typedef struct { uint64_t t; uint32_t kind_n, fill, x, y; } aud_ev;
enum { AUD_CB = 0, AUD_TICK = 1, AUD_XRUN = 2 };
typedef struct { uint8_t *buf; uint32_t cap; _Atomic uint64_t w, r, dropped; } aud_ring;
typedef struct { aud_ring pcm, ev_cb, ev_tick; FILE *fpcm, *fev; pthread_t th; _Atomic int run; int xruns; } aud_dump;

static uint64_t aud_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
static int aud_ring_init(aud_ring *g, uint32_t cap) { g->buf = calloc(1, cap); g->cap = cap; atomic_init(&g->w, 0); atomic_init(&g->r, 0); atomic_init(&g->dropped, 0); return g->buf != NULL; }
static void aud_put(aud_ring *g, const void *p, uint32_t n) {
    uint64_t w = atomic_load_explicit(&g->w, memory_order_relaxed), r = atomic_load_explicit(&g->r, memory_order_acquire);
    if (w + n - r > g->cap) { atomic_fetch_add_explicit(&g->dropped, n, memory_order_relaxed); return; }
    uint32_t at = (uint32_t)(w % g->cap), first = n < g->cap - at ? n : g->cap - at;
    memcpy(g->buf + at, p, first); memcpy(g->buf, (const uint8_t *)p + first, n - first);
    atomic_store_explicit(&g->w, w + n, memory_order_release);
}
static void aud_drain(aud_ring *g, FILE *f) {
    uint64_t r = atomic_load_explicit(&g->r, memory_order_relaxed), w = atomic_load_explicit(&g->w, memory_order_acquire);
    while (r < w) {
        uint32_t at = (uint32_t)(r % g->cap), n = (uint32_t)(w - r < g->cap - at ? w - r : g->cap - at);
        if (f) fwrite(g->buf + at, 1, n, f);
        r += n;
    }
    atomic_store_explicit(&g->r, r, memory_order_release);
}
static void aud_ev_put(aud_ring *g, uint32_t kind, uint32_t n, uint32_t fill, uint32_t x, uint32_t y) {
    aud_ev e = {aud_now(), kind << 28 | (n & 0x0fffffffu), fill, x, y}; aud_put(g, &e, sizeof e);
}
