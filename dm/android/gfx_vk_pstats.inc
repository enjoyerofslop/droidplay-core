// DM_VK_PIPE_STATS=1 (env.txt, a diagnostic): each pipeline's executable statistics (VK_KHR_pipeline_executable_properties) as it is made, and with DM_VK_MEMREPORT=1
// the driver memory made with it. A line goes out for each pipeline whose shaders need scratch memory (=2: for every pipeline), and a summary every 10 s.
// Why: Qualcomm's Adreno 7xx drivers give every pipeline whose shader needs any scratch memory a block of its scratch, rounded up to 512 bytes, for each fiber the
// GPU can run at once (24,576 on the Ayn Odin 2's 740, so 12 MiB for 1 to 512 bytes; 8 MiB on the Razr's 730), held until the pipeline is destroyed. The statistic
// "Scratch memory usage per shader instance" says which pipelines pay it, and why is then a question of the shader (register spills, an array, a pointer in a variable)
static int g_ps_on;   // 1: scratch pipelines logged; 2: every pipeline
static PFN_vkGetPipelineExecutablePropertiesKHR g_ps_props; static PFN_vkGetPipelineExecutableStatisticsKHR g_ps_stats;
static VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR g_ps_feat = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR};
static _Atomic uint64_t g_ps_made, g_ps_scr, g_ps_scr_bytes, g_ps_scr_mem;   // pipelines reported; those with scratch; their scratch a shader instance, summed; the driver memory made with them
static struct timespec g_ps_last; static pthread_mutex_t g_ps_mu = PTHREAD_MUTEX_INITIALIZER;
// vk_try: whether to ask for the statistics (the extension and its feature)
static int ps_want(const VkExtensionProperties *de, uint32_t nd) {
    const char *e = getenv("DM_VK_PIPE_STATS");
    if (!e || (*e != '1' && *e != '2') || !has_ext(de, nd, "VK_KHR_pipeline_executable_properties")) return 0;
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &g_ps_feat}; vkGetPhysicalDeviceFeatures2(g_vk.pd, &f2);
    if (!g_ps_feat.pipelineExecutableInfo) { LOG("[vk] DM_VK_PIPE_STATS: the driver has no pipelineExecutableInfo"); return 0; }
    g_ps_on = *e - '0'; return 1;
}
static void *ps_chain(void *next) { g_ps_feat.pNext = next; return &g_ps_feat; }
static void ps_device_made(void) {
    g_ps_props = (PFN_vkGetPipelineExecutablePropertiesKHR)vkGetDeviceProcAddr(g_vk.dev, "vkGetPipelineExecutablePropertiesKHR");
    g_ps_stats = (PFN_vkGetPipelineExecutableStatisticsKHR)vkGetDeviceProcAddr(g_vk.dev, "vkGetPipelineExecutableStatisticsKHR");
    if (!g_ps_props || !g_ps_stats) g_ps_on = 0;
    LOG("[vk] DM_VK_PIPE_STATS: %s", g_ps_on ? "each pipeline's statistics captured (VK_KHR_pipeline_executable_properties)" : "the statistics functions were not found");
}
static VkPipelineCreateFlags ps_flags(void) { return g_ps_on ? VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR : 0; }
static uint64_t ps_stat_u(const VkPipelineExecutableStatisticKHR *s) {
    switch (s->format) { case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR: return s->value.b32; case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR: return s->value.i64 < 0 ? 0 : (uint64_t)s->value.i64;
                         case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR: return s->value.u64; default: return (uint64_t)s->value.f64; }
}
// After a pipeline is made: its statistics, under the names Qualcomm's driver gives them (another driver's names are logged as they are with =2).
// what: its shaders' keys; mem: the driver memory made on this thread while it was made (DM_VK_MEMREPORT), big: the largest such object
static void ps_report(VkPipeline p, const char *what, uint64_t mem, uint64_t big) {
    if (!g_ps_on || !p) return;
    VkPipelineInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR, .pipeline = p};
    uint32_t ne = 0; if (g_ps_props(g_vk.dev, &pi, &ne, NULL) != VK_SUCCESS || !ne) return;
    VkPipelineExecutablePropertiesKHR ep[5]; if (ne > 5) ne = 5;
    for (uint32_t i = 0; i < ne; i++) ep[i] = (VkPipelineExecutablePropertiesKHR){.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR};
    g_ps_props(g_vk.dev, &pi, &ne, ep);
    char s[600]; size_t n = 0; uint64_t scr = 0; s[0] = 0;
    for (uint32_t e = 0; e < ne; e++) {
        VkPipelineExecutableInfoKHR xi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR, .pipeline = p, .executableIndex = e};
        VkPipelineExecutableStatisticKHR st[48]; uint32_t ns = 48;
        for (uint32_t i = 0; i < ns; i++) st[i] = (VkPipelineExecutableStatisticKHR){.sType = VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR};
        if (g_ps_stats(g_vk.dev, &xi, &ns, st) < 0) continue;
        uint64_t es = 0, fr = 0, hr = 0, ins = 0;
        for (uint32_t i = 0; i < ns; i++) {
            if (strstr(st[i].name, "Scratch") || strstr(st[i].name, "scratch")) es = ps_stat_u(&st[i]);
            else if (strstr(st[i].name, "Full precision register")) fr = ps_stat_u(&st[i]);
            else if (strstr(st[i].name, "Half precision register")) hr = ps_stat_u(&st[i]);
            else if (!strcmp(st[i].name, "Instruction count all")) ins = ps_stat_u(&st[i]);
        }
        scr += es;
        if (n < sizeof s) n += (size_t)snprintf(s + n, sizeof s - n, "%s%s: scratch %llu B, registers %llu full / %llu half, %llu instructions", e ? "; " : "", ep[e].name,
                                                (unsigned long long)es, (unsigned long long)fr, (unsigned long long)hr, (unsigned long long)ins);
    }
    atomic_fetch_add(&g_ps_made, 1);
    if (scr) { atomic_fetch_add(&g_ps_scr, 1); atomic_fetch_add(&g_ps_scr_bytes, scr); atomic_fetch_add(&g_ps_scr_mem, big); }
    if (scr || g_ps_on == 2) LOG("[vk] pipeline stats: %s: %s; driver memory made with it %.2f MB, largest object %.2f MB", what, s, mem / 1048576.0, big / 1048576.0);
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    pthread_mutex_lock(&g_ps_mu);
    if (now.tv_sec - g_ps_last.tv_sec >= 10) {
        g_ps_last = now;
        LOG("[vk] pipeline stats: %llu pipelines made, %llu of them with scratch memory (%llu B a shader instance summed), their largest driver objects %.1f MB (cumulative; DM_VK_PIPE_STATS)",
            (unsigned long long)atomic_load(&g_ps_made), (unsigned long long)atomic_load(&g_ps_scr), (unsigned long long)atomic_load(&g_ps_scr_bytes), atomic_load(&g_ps_scr_mem) / 1048576.0);
    }
    pthread_mutex_unlock(&g_ps_mu);
}
