// DM_VK_MEMREPORT=1 (env.txt, a diagnostic): where the process's graphics memory goes, the replay's allocations and the driver's own alike. With VK_EXT_device_memory_report
// the driver reports every memory object it allocates or frees, with the Vulkan object it belongs to (an image, a pipeline, a command buffer, a descriptor pool...). A line goes
// out each time the live total passes another 256 MB up, and at an allocation 10 s or more after the last line, with the live bytes and counts by object type. Why: Ocarina of
// Time's Kokiri Forest took the Galaxy Z Fold 8's graphics memory (dumpsys meminfo) from 0.6 to 4.6 GB, and the replay's own allocations were about 2 GB of it; the reports
// showed 1.75 GB in descriptor pools no set was made from (gfx_vk_draw.inc ds_fits). Without DM_VK_MEMREPORT=1 the reports are asked for on Qualcomm's own driver anyway, and
// the callback only counts what each pipeline's creation made on its thread (t_mr_pipe_big): the heavy pipelines' scratch blocks (gfx_vk_pheavy.inc)
#include <time.h>

enum { MR_TYPES = 27, MR_CAP = 1 << 18 };   // VkObjectType 0-25, then everything else; slots for live memory objects (a power of two)
typedef struct { uint64_t id, size; uint8_t type; } mr_ent;
static mr_ent *g_mr_tab;
static uint32_t g_mr_n, g_mr_full;
static int64_t g_mr_bytes[MR_TYPES], g_mr_count[MR_TYPES], g_mr_total, g_mr_next = 256ll << 20;
static uint64_t g_mr_failed, g_mr_imported;
static struct timespec g_mr_last;
static pthread_mutex_t g_mr_mu = PTHREAD_MUTEX_INITIALIZER;
static __thread uint64_t t_mr_pipe, t_mr_pipe_big;   // pipeline memory the driver made on this thread since the caller zeroed them, and the largest object (gfx_vk_pheavy.inc)
static int g_mr_all;   // DM_VK_MEMREPORT=1: every object kept and the lines logged; 0: only the pipelines' counts above

static const char *const g_mr_names[MR_TYPES] = {"unknown", "instance", "physical device", "device", "queue", "semaphore", "command buffer", "fence", "device memory", "buffer",
    "image", "event", "query pool", "buffer view", "image view", "shader module", "pipeline cache", "pipeline layout", "render pass", "pipeline", "set layout", "sampler",
    "descriptor pool", "descriptor set", "framebuffer", "command pool", "other"};

static uint32_t mr_slot(uint64_t id) { return (uint32_t)((id * 0x9E3779B97F4A7C15ull) >> 46) & (MR_CAP - 1); }
static void mr_log(const char *why) {   // under g_mr_mu
    char s[1024]; size_t n = 0; s[0] = 0;
    for (int i = 0; i < MR_TYPES; i++) if (g_mr_count[i] && n < sizeof s)
        n += (size_t)snprintf(s + n, sizeof s - n, "%s%s %.1f MB (%lld)", n ? ", " : "", g_mr_names[i], g_mr_bytes[i] / 1048576.0, (long long)g_mr_count[i]);
    LOG("[vk] memory report (%s): %.1f MB live in %u memory objects: %s; %llu allocations failed, %llu imports, %u frees of objects not seen (cumulative; DM_VK_MEMREPORT)", why,
        g_mr_total / 1048576.0, g_mr_n, s, (unsigned long long)g_mr_failed, (unsigned long long)g_mr_imported, g_mr_full);
    clock_gettime(CLOCK_MONOTONIC, &g_mr_last);
}
static void VKAPI_PTR mr_cb(const VkDeviceMemoryReportCallbackDataEXT *d, void *user) {
    (void)user;
    if (d->objectType == VK_OBJECT_TYPE_PIPELINE && (d->type == VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_ALLOCATE_EXT || d->type == VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_IMPORT_EXT)) {
        t_mr_pipe += d->size; if (d->size > t_mr_pipe_big) t_mr_pipe_big = d->size; }
    if (!g_mr_all) return;
    uint64_t id = d->memoryObjectId ? d->memoryObjectId : 1;   // 0 marks a free slot
    pthread_mutex_lock(&g_mr_mu);
    if (d->type == VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_ALLOCATION_FAILED_EXT) { g_mr_failed++; mr_log("an allocation failed"); }
    else if (d->type == VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_ALLOCATE_EXT || d->type == VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_IMPORT_EXT) {
        if (d->type == VK_DEVICE_MEMORY_REPORT_EVENT_TYPE_IMPORT_EXT) g_mr_imported++;
        uint8_t t = d->objectType < MR_TYPES - 1 ? (uint8_t)d->objectType : MR_TYPES - 1;
        if (g_mr_n < MR_CAP - MR_CAP / 8) {
            uint32_t i = mr_slot(id); while (g_mr_tab[i].id) i = (i + 1) & (MR_CAP - 1);
            g_mr_tab[i] = (mr_ent){id, d->size, t}; g_mr_n++;
            g_mr_bytes[t] += (int64_t)d->size; g_mr_count[t]++; g_mr_total += (int64_t)d->size;
        }
        struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
        if (g_mr_total >= g_mr_next) { while (g_mr_next <= g_mr_total) g_mr_next += 256ll << 20; mr_log("passed another 256 MB"); }
        else if (now.tv_sec - g_mr_last.tv_sec >= 10) mr_log("10 s");
    } else {   // a free or an unimport: the object's slot goes, and the run after it shifts back so lookups stay unbroken (linear probing)
        uint32_t i = mr_slot(id);
        while (g_mr_tab[i].id && g_mr_tab[i].id != id) i = (i + 1) & (MR_CAP - 1);
        if (!g_mr_tab[i].id) g_mr_full++;
        else {
            mr_ent e = g_mr_tab[i]; g_mr_bytes[e.type] -= (int64_t)e.size; g_mr_count[e.type]--; g_mr_total -= (int64_t)e.size; g_mr_n--;
            for (uint32_t j = (i + 1) & (MR_CAP - 1); g_mr_tab[j].id; j = (j + 1) & (MR_CAP - 1)) {
                uint32_t h = mr_slot(g_mr_tab[j].id);
                if (((j - h) & (MR_CAP - 1)) >= ((j - i) & (MR_CAP - 1))) { g_mr_tab[i] = g_mr_tab[j]; i = j; }
            }
            g_mr_tab[i].id = 0;
            if (g_mr_total + (512ll << 20) < g_mr_next) g_mr_next -= 256ll << 20;   // a drop, then growth again, is reported again
        }
    }
    pthread_mutex_unlock(&g_mr_mu);
}
static VkDeviceDeviceMemoryReportCreateInfoEXT g_mr_ci = {.sType = VK_STRUCTURE_TYPE_DEVICE_DEVICE_MEMORY_REPORT_CREATE_INFO_EXT, .pfnUserCallback = mr_cb};
static VkPhysicalDeviceDeviceMemoryReportFeaturesEXT g_mr_feat = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_MEMORY_REPORT_FEATURES_EXT};
// vk_create: whether to ask for the reports (the extension and its feature), and the create infos that do, chained in front of next. light: wanted without
// DM_VK_MEMREPORT=1 too (gfx_vk_pheavy.inc ph_want: the pipelines' counts only)
static int mr_want(const VkExtensionProperties *de, uint32_t nd, int light) {
    const char *e = getenv("DM_VK_MEMREPORT"); int full = e && *e == '1';
    if (!full && !light) return 0;
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &g_mr_feat};
    if (has_ext(de, nd, "VK_EXT_device_memory_report")) vkGetPhysicalDeviceFeatures2(g_vk.pd, &f2);
    if (!g_mr_feat.deviceMemoryReport) { if (full) LOG("[vk] DM_VK_MEMREPORT=1, but the driver has no VK_EXT_device_memory_report"); return 0; }
    g_mr_feat.pNext = NULL;
    if (full && !(g_mr_tab = calloc(MR_CAP, sizeof *g_mr_tab))) return light;
    g_mr_all = full; return 1;
}
static void *mr_chain(void *next) { g_mr_ci.pNext = next; g_mr_feat.pNext = &g_mr_ci; return &g_mr_feat; }
