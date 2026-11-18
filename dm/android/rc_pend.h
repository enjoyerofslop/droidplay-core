// The counter reports the replay made last, each address once with the kind of its newest report (gfx_vk.c rc_note, rc_value: render_cond), found through an
// open-addressing index instead of a walk of the list. Self-contained: dm/tools/tests/test_rc_pend.py drives it on the PC.
// The list keeps the RC_PEND newest addresses; when it is full the older half goes (those reports have landed by then), as before. Each report and each condition walked the
// whole list: Ocarina of Time makes about 2,200 reports and 530 conditions a frame with the list full, and the walks were 4.5% of its replay thread on a Galaxy Z Fold 8 in
// Kokiri Forest (rc_note, rc_value). The index holds each address's position in the list, at most RC_PEND of its RC_PIDX slots (no deletions: it is rebuilt when the list drops
// its older half), so a probe ends at the address or at an empty slot within a few steps.
#pragma once
#include <stdint.h>
#include <string.h>

enum { RC_PEND = 256, RC_PIDX = 1024 };
typedef struct { uint64_t addr; int kind; } rc_pent;
typedef struct { rc_pent e[RC_PEND]; int n; uint16_t idx[RC_PIDX]; } rc_pend;   // idx: position + 1 in e, 0 empty

static inline uint32_t rcp_slot(const rc_pend *p, uint64_t a) {
    uint32_t i = (uint32_t)((a >> 4) * 0x9E3779B1u) & (RC_PIDX - 1);
    while (p->idx[i] && p->e[p->idx[i] - 1].addr != a) i = (i + 1) & (RC_PIDX - 1);
    return i;
}
// the newest kind noted for address a, or 0 when it is not in the list
static inline int rcp_kind(const rc_pend *p, uint64_t a) { uint32_t i = rcp_slot(p, a); return p->idx[i] ? p->e[p->idx[i] - 1].kind : 0; }
// a report of kind (not 0) to address a: its entry updated, or added as the newest (the older half dropped first when the list is full)
static inline void rcp_note(rc_pend *p, uint64_t a, int kind) {
    uint32_t i = rcp_slot(p, a);
    if (p->idx[i]) { p->e[p->idx[i] - 1].kind = kind; return; }
    if (p->n == RC_PEND) {
        memmove(p->e, p->e + RC_PEND / 2, sizeof *p->e * (RC_PEND / 2)); p->n = RC_PEND / 2;
        memset(p->idx, 0, sizeof p->idx);
        for (int k = 0; k < p->n; k++) p->idx[rcp_slot(p, p->e[k].addr)] = (uint16_t)(k + 1);
        i = rcp_slot(p, a);
    }
    p->e[p->n].addr = a; p->e[p->n].kind = kind; p->idx[i] = (uint16_t)++p->n;
}
