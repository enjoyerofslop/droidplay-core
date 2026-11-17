// A pending storage buffer copy that holds its range's current bytes, written in place by later draws behind a barrier instead of waiting for it to land
// (gfx_vk_draw.inc wl_*), as plain functions, so dm/tools/tests/test_wb_live.py checks them on the host.
// Why: Tears of the Kingdom's draws that store computed values into its 8 KB and 131,584-byte buffers (fragment shaders whose interlock was removed), and its
// compute and vertex shaders that read what the order-free copies (ofw.h) hold, each submitted the chunk and waited for it: 18 GPU round trips a frame at
// #457's save on a Galaxy S25 Edge, most of them a chunk of one draw.
#pragma once
#include <stddef.h>
#include <stdint.h>

// What draws did to a copy since the last full barrier: the barrier epoch of the last read and of the last in-place write, and which draw made them (WL_MANY: more than
// one); wk: the kind of the writes of that epoch (WL_PLAIN, or an order-free write's class and constant: wl_ofw_kind)
typedef struct { uint32_t rep, rdid, wep, wdid; uint64_t wk; } wl_st;
enum { WL_MANY = 0xffffffffu };
#define WL_READ 0ull
#define WL_PLAIN 0xffffffffffffffffull
static inline uint64_t wl_ofw_kind(uint32_t cls, uint32_t k) { return (uint64_t)cls << 32 | k; }

// Whether a write-back's copy holds its range's current bytes, so a later draw may read or write it in place: a copy in the chunk's arena (st < 0) of one row, with a
// snapshot to land against, not written in place in a placed pool (inpl), not a linear target's (lin) nor a depth / stencil readback (ds), not 16-bit texels to swap (rbx),
// not a discarded draw's home copy (vd); an order-free write's copy only when it started from the range's bytes (og), not from the operation's identity
static inline int wl_true(int st, size_t rows, int inpl, int lin, int ds, int rbx, int vd, uint32_t ofw, int og, int snap) {
    return st < 0 && rows == 1 && !inpl && !lin && !ds && !rbx && !vd && (!ofw || og) && snap;
}
// Whether [lo, hi) lies inside the copy of [dst, dst + n) at arena offset off, at an offset into the copy that keeps alignment al (0: none)
static inline int wl_covers(uintptr_t lo, uintptr_t hi, uintptr_t dst, size_t n, uint64_t off, uint64_t al) {
    return hi > lo && lo >= dst && hi <= dst + n && (!al || (off + (lo - dst)) % al == 0);
}
// Whether draw did's access of kind (WL_READ, WL_PLAIN or an order-free write) needs a full barrier first, at barrier epoch ep: a read after another draw's write of this
// epoch; a write after another draw's read of this epoch, or after another draw's write of this epoch unless both are the same order-free write (stores of one constant,
// one commutative atomic: their order doesn't change the result). A draw's own earlier access never needs one (the draw's own business, as on the console)
static inline int wl_need(const wl_st *s, uint32_t ep, uint32_t did, uint64_t kind) {
    int wrote = s->wep == ep && s->wdid != did, read = s->rep == ep && s->rdid != did;
    if (kind == WL_READ) return wrote;
    return read || (wrote && (kind == WL_PLAIN || s->wk != kind));
}
// Draw did's access, recorded at the epoch it runs in
static inline void wl_mark(wl_st *s, uint32_t ep, uint32_t did, uint64_t kind) {
    if (kind == WL_READ) { s->rdid = s->rep == ep && s->rdid != did ? WL_MANY : did; s->rep = ep; return; }
    if (s->wep == ep) { if (s->wdid != did) s->wdid = WL_MANY; if (s->wk != kind) s->wk = WL_PLAIN; }   // (two kinds in one epoch: only one draw's own, wl_need saw to the rest)
    else { s->wdid = did; s->wk = kind; s->wep = ep; }
}
