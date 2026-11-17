// ---------------- Pipelines that hold a scratch block, kept only while draws use them (included by gfx_vk.c after gfx_vk_draw.inc) ----------------
// Qualcomm's driver gives every pipeline whose shaders need any scratch (private) memory a block of its own, as big as that scratch rounded up to 512 bytes for each fiber the
// GPU runs at once, and holds it until the pipeline is destroyed. On the Ayn Odin 2's Adreno 740 (V@0676.32) that is 24,576 fibers for a compute pipeline and 49,152 for a
// graphics one (one block for all its stages, sized by the largest), so a shader needing 8 bytes costs 12 or 24 MiB, and a big one that spills 7 KB 168 MiB; the Razr's
// Adreno 730 holds 8 MiB for 1-512 bytes. The scratch comes from register spills, of kilobytes in big shaders at the register limit and of 6-56 bytes in many translated
// shaders that are not (Ocarina of Time's fragment shaders that spill 8 bytes use 28-40 of 48 full registers; the spill moves with unrelated edits, and spirv-opt -O removes
// it from 6 of 50), and from private arrays past about 32 words: nothing a translated shader can avoid for certain. Measured with VK_EXT_device_memory_report and the
// "Scratch memory usage per shader instance" statistic (DM_VK_PIPE_STATS): Brilliant Diamond's language menu holds 8 such pipelines (192 of its 214 MB of pipeline memory),
// Final Fantasy Resonance's title 3 (72 of 83 MB), and Ocarina of Time's start-up list 98 of its 742 (2,352 of 2,361 MB at the title). The block lives only as long as the
// pipeline, a derivative pipeline gets its own, and a pipeline made again from the driver's cache takes 2 ms (24 ms for the 168 MiB one) on the Odin 2. So:
//  - each pipeline's largest driver object is read as it is made (the memory report's callback, on the making thread: t_mr_pipe_big); 4 MiB or more is a scratch block;
//  - the start-up list (prewarm_thread) still compiles such a pipeline into the driver's cache, but destroys it at once: the first draw that needs it makes it again;
//  - one no draw or dispatch has bound for PH_AGE presents (PH_LOW_AGE while MemAvailable is under PH_LOW_MIB) is destroyed once the chunk that last bound it has completed;
//  - a destroyed one is made again at once when a draw needs it (pipe_get, cpipe_get), from the cache, not in the background: no draw is left out for it.
// The idea of destroying unused pipelines and making them again from the cache is droidplay-core#397's (pipelines under memory pressure, by age or least recently bound).
// On by default where Qualcomm's driver offers the memory report. DM_VK_PIPE_HEAVY=0 (env.txt): kept as before; =<presents>: that age instead of PH_AGE.
// DM_VK_PIPE_HEAVY_LOG=1: a line for every scratch pipeline made and every sweep that destroys something (the first few come anyway)
#include "pheavy_rule.h"   // the rules (dm/tools/tests/test_pheavy_rule.py)
#define PH_AGE 600                 // presents unbound before it goes (20 s at 30 fps)
#define PH_LOW_AGE 60
#define PH_LOW_MIB 2048            // MemAvailable (read every PH_EVERY presents) under it: the shorter age
#define PH_EVERY 30                // the sweep's period, in presents
static int g_ph_on = -1;   // the memory report is asked for and the sweeps run
static uint64_t g_ph_made[2], g_ph_bytes[2], g_ph_gone[2], g_ph_gone_bytes, g_ph_sweeps;   // cumulative: scratch pipelines made (graphics, compute) and their blocks; destroyed unbound
static int ph_log(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_PIPE_HEAVY_LOG"); v = e && *e == '1'; } return v; }
static int64_t ph_age(void) { static int64_t a = -1; if (a < 0) { const char *e = getenv("DM_VK_PIPE_HEAVY"); a = e && atol(e) > 1 ? atol(e) : PH_AGE; } return a; }
// vk_try, before the memory report is asked for: whether it is wanted for this (Qualcomm's own driver, DM_VK_PIPE_HEAVY not 0)
static int ph_want(void) {
    const char *e = getenv("DM_VK_PIPE_HEAVY");
    g_ph_on = g_qcom_driver && !(e && *e == '0');
    return g_ph_on;
}
static void ph_device_made(int reports) {   // vk_try: reports, whether the driver took the memory report's callback
    if (g_ph_on && !reports) g_ph_on = 0;
    if (g_ph_on) LOG("[vk] pipelines whose shaders need scratch memory (4 MiB or more of the driver's, read from VK_EXT_device_memory_report) are not kept by the start-up list "
                     "and are destroyed when unbound for %lld presents (%d under %d MiB MemAvailable), made again from the driver's cache when a draw needs them (DM_VK_PIPE_HEAVY=0: kept)",
                     (long long)ph_age(), PH_LOW_AGE, PH_LOW_MIB);
}
// After lm_pipeline made a pipeline on this thread: its scratch block's bytes (0: none, or the reports are off). kind 0 graphics, 1 compute; what: its shaders
static uint64_t ph_note(int kind, const char *a, const char *b) {
    uint64_t big = g_ph_on > 0 ? phr_block(t_lm_big) : 0; if (!big) return 0;
    pthread_mutex_lock(&g_ph_mu); g_ph_made[kind]++; g_ph_bytes[kind] += big; uint64_t n = g_ph_made[0] + g_ph_made[1]; pthread_mutex_unlock(&g_ph_mu);
    if (ph_log() || n <= 4) LOG("[vk] %s pipeline %s%s%s holds a scratch block of %.1f MiB (the driver's, for as long as it exists; DM_VK_PIPE_HEAVY)", kind ? "compute" : "graphics", a ? a : "-",
                                b ? " + " : "", b ? b : "", big / 1048576.0);
    return big;
}
static uint64_t ph_avail_mib(void) {   // /proc/meminfo's MemAvailable, in MiB (0 when unreadable)
    FILE *f = fopen("/proc/meminfo", "r"); if (!f) return 0;
    char line[128]; uint64_t kb = 0;
    while (fgets(line, sizeof line, f)) if (!strncmp(line, "MemAvailable:", 13)) { kb = strtoull(line + 13, NULL, 10); break; }
    fclose(f); return kb >> 10;
}
static void ph_sweep(void) {   // mtl_present, under g_bk
    if (g_ph_on <= 0) return;
    uint64_t now = atomic_load(&g_presents); if (now % PH_EVERY) return;
    uint64_t avail = ph_avail_mib(), age = phr_age(avail, (uint64_t)ph_age(), PH_LOW_AGE, PH_LOW_MIB); int low = age != (uint64_t)ph_age();
    if (now < age) return;
    pthread_mutex_lock(&g_cmu); uint64_t done = g_seq_done; pthread_mutex_unlock(&g_cmu);
    uint64_t n0 = g_ph_gone[0], n1 = g_ph_gone[1], b0 = g_ph_gone_bytes;
    for (uint32_t i = 0; i < g_pipes.cap; i++) { pipe_t *pp = g_pipes.vals[i];   // (one being made in the background has no pipeline yet: APQ_PENDING)
        if (!pp || !phr_goes(pp->heavy, pp->p != VK_NULL_HANDLE, pp->last, pp->seq, now, done, age)) continue;
        vkDestroyPipeline(g_vk.dev, pp->p, NULL); vkDestroyPipelineLayout(g_vk.dev, pp->pl, NULL); pp->p = VK_NULL_HANDLE; pp->pl = VK_NULL_HANDLE; pp->ev = 1;
        g_ph_gone[0]++; g_ph_gone_bytes += pp->heavy; }
    for (uint32_t i = 0; i < g_cpipes.cap; i++) { cpipe_t *cp = g_cpipes.vals[i];
        if (!cp || !phr_goes(cp->heavy, cp->p != VK_NULL_HANDLE, cp->last, cp->seq, now, done, age)) continue;
        vkDestroyPipeline(g_vk.dev, cp->p, NULL); vkDestroyPipelineLayout(g_vk.dev, cp->pl, NULL); cp->p = VK_NULL_HANDLE; cp->pl = VK_NULL_HANDLE; cp->ev = 1;
        g_ph_gone[1]++; g_ph_gone_bytes += cp->heavy; }
    if (g_ph_gone[0] == n0 && g_ph_gone[1] == n1) return;
    g_ph_sweeps++;
    g_pso_m.pp = NULL; bf_reset();   // the memo's pipeline may be gone; a handle the driver gives again is no bind the command buffer holds
    static int said; if (ph_log() || said++ < 4)
        LOG("[vk] present %llu: %llu graphics and %llu compute pipelines with scratch blocks destroyed, unbound for %llu presents%s: %.0f MiB back; made again so far %llu and %llu (DM_VK_PIPE_HEAVY)",
            (unsigned long long)now, (unsigned long long)(g_ph_gone[0] - n0), (unsigned long long)(g_ph_gone[1] - n1), (unsigned long long)age, low ? " (MemAvailable under the line)" : "",
            (g_ph_gone_bytes - b0) / 1048576.0, (unsigned long long)g_ph_back[0], (unsigned long long)g_ph_back[1]);
}
static void ph_stat(void) {   // the periodic line (mtl_mem_stat)
    if (g_ph_on <= 0) return;
    uint32_t live[2] = {0, 0}; uint64_t lb = 0;
    for (uint32_t i = 0; i < g_pipes.cap; i++) { const pipe_t *pp = g_pipes.vals[i]; if (pp && pp->p && pp->heavy) { live[0]++; lb += pp->heavy; } }
    for (uint32_t i = 0; i < g_cpipes.cap; i++) { const cpipe_t *cp = g_cpipes.vals[i]; if (cp && cp->p && cp->heavy) { live[1]++; lb += cp->heavy; } }
    LOG("[vk] pipelines with scratch blocks: %u graphics and %u compute live (%.0f MiB); made %llu and %llu (%.0f MiB), the start-up list's %llu dropped (%.0f MiB), %llu and %llu destroyed "
        "unbound (%.0f MiB), %llu and %llu made again (cumulative; DM_VK_PIPE_HEAVY)", live[0], live[1], lb / 1048576.0, (unsigned long long)g_ph_made[0], (unsigned long long)g_ph_made[1],
        (g_ph_bytes[0] + g_ph_bytes[1]) / 1048576.0, (unsigned long long)g_ph_list, g_ph_list_bytes / 1048576.0, (unsigned long long)g_ph_gone[0], (unsigned long long)g_ph_gone[1],
        g_ph_gone_bytes / 1048576.0, (unsigned long long)g_ph_back[0], (unsigned long long)g_ph_back[1]);
}
