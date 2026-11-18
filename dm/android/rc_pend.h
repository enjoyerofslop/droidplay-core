// Conditional rendering's list of pending timestamp reports (gfx_vk.c rc_note / rc_value, profile render_cond): an address whose latest report the replay has met counts
// as 0 if that report was a timestamp one (a timestamp report puts 0 in the counter field), and as unknown if a report of another counter kind came after it. A
// samples-passed report, or one of a counter kind the host does not implement, is not added: the counter report address set (g_rc_occ) holds those, with the fence that
// says when one has landed. They were added here too, and Ocarina of Time's 530 or so occlusion reports a frame filled the 256 entries: a full list drops its older half,
// so a timestamp report noted before them in the same submission was forgotten, and a condition on it read the 1 the CPU had written there. The game clears its 768x432
// refraction target under such a condition at every bind and reports once: the later clears ran and wiped the scene copy, and the water turned opaque brown.
// DM_VK_OCCLUSION=0 did not avoid it, as the reports are still replayed and noted. nvn_mtl.m's table has kept only timestamps per submission since Splatoon Raiders
// (docs/reference/rendering.md, Report tables that dropped entries). Plain C, for dm/tools/tests/test_rc_pend.py.
#pragma once
#include <stdint.h>
#include <string.h>

enum { RC_PEND = 256 };
enum { RCP_TIMESTAMP = 1, RCP_UNKNOWN = 2 };   // an entry's kind: a timestamp report (counter 0), or a report of another counter after one to the same address
typedef struct { uint64_t addr; int kind; } rcp_entry;
typedef struct { rcp_entry e[RC_PEND]; int n; uint64_t dropped; } rcp_list;   // dropped: entries let go when the list was full

// a report to address a: timestamp 1 for a timestamp report, 0 for any other counter kind. An entry the address has takes the new kind; a timestamp report takes a new
// entry, and a full list first lets its older half go (timestamps noted that long ago have landed, and their memory reads 0 as well); another kind takes none
static void rcp_note(rcp_list *l, uint64_t a, int timestamp) {
    int kind = timestamp ? RCP_TIMESTAMP : RCP_UNKNOWN;
    for (int i = 0; i < l->n; i++) if (l->e[i].addr == a) { l->e[i].kind = kind; return; }
    if (!timestamp) return;   // (the counter report address set holds it)
    if (l->n == RC_PEND) { memmove(l->e, l->e + RC_PEND / 2, sizeof *l->e * (RC_PEND / 2)); l->n = RC_PEND / 2; l->dropped += RC_PEND / 2; }
    l->e[l->n].addr = a; l->e[l->n++].kind = kind;
}

// the kind of address a's entry: RCP_TIMESTAMP, RCP_UNKNOWN, or 0 when it has none (its value is the address set's or memory's)
static int rcp_find(const rcp_list *l, uint64_t a) {
    for (int i = l->n - 1; i >= 0; i--) if (l->e[i].addr == a) return l->e[i].kind;
    return 0;
}
