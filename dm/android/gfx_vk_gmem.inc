// ---------------- Raw global memory: the memory pools at device addresses (DM_VK_RAW_GMEM) ----------------
// A shader that loads or stores at an address it read from memory, which the translator traced to no storage buffer, looks the address up in the raw pool table (the storage
// buffer under DMSH_GMEM_MAP_CB_SLOT; docs/reference/shaders.md "Global memory at an address loaded from memory") and goes through a physical pointer: the address plus the
// delta of the pool that holds it. The Mac's pools are no-copy buffers over the guest's own pages (nvn_mtl.m gmem_map). This replay copies what each draw reads into its arena,
// so no pool had a device address: the table was 16 zero bytes, every such load read 0 and every store went nowhere (2,247 of Ocarina of Time's shaders bind the table).
// Without VK_EXT_external_memory_host (the Adreno 840 has none) Vulkan cannot import the guest's pages, so the pages become Vulkan memory instead, as Banana Native does: a pool's
// bytes are copied into a new allocation of a host-visible, coherent, cached type that has a device address (VK_KHR_buffer_device_address), and the allocation is mapped at the
// pool's own address (VK_EXT_map_memory_placed), replacing the anonymous pages there. The game, the replay's CPU copies and the GPU's raw loads and stores then reach the same
// memory, as on the Mac. Only pools the game took a buffer address in are placed, when it first takes one (see "Placing a pool" below), and only those are in the table.
// A driver without VK_EXT_map_memory_placed (the Galaxy S25 Edge's Adreno 830) gets the same layout another way, the same design as nx2android's zero-copy pools, written
// anew here: a piece's bytes go into an AHardwareBuffer (a BLOB, CPU-cached), whose dma-buf is mapped MAP_SHARED | MAP_FIXED over the pool's pages, and the buffer is
// imported into Vulkan (VK_ANDROID_external_memory_android_hardware_buffer) with a device address. Pieces of at most 256 MiB there: the Adreno 830's allocator service aborted
// on an 812 MB BLOB in nx2android's tests. DM_VK_RAW_GMEM_AHB=1 takes this route on a device that has both (a diagnostic)
// - Pieces: an allocation holds at most maxMemoryAllocationSize (1 GiB on the Adreno 840), so a larger pool is placed in pieces, each a table entry of its own.
// - Faulted in at once (MADV_POPULATE_READ): the Adreno's kernel driver fills a page at its first touch from the touch's distance to the start of its mapping, which is the
//   wrong page once the dirty tracker's mprotect has split the mapping (Banana Native found it as Donkey Kong Bananza's textures holding other textures' blocks).
// - Given back at nvnMemoryPoolFinalize (mtl_pool_del): the GPU is waited for, the bytes go to new anonymous pages (zero pages skipped), the allocation is freed, and the pages
//   are moved over the range. The free comes before the move because the Adreno's vkFreeMemory takes the placed range down even after VK_MEMORY_UNMAP_RESERVE_BIT_EXT.
// - Committed whole: driver memory is committed when allocated, an anonymous page only when first touched, so a pool the game reserved and barely used costs its whole size.
//   DM_VK_RAW_GMEM_MAX caps the bytes placed; a pool past the cap stays anonymous and raw accesses to it read 0 ([RED] once).
// - Stores land in guest memory themselves, when the GPU runs them. The replay reads guest memory when it records (arena copies, uploads, CPU-side copies), so a chunk that
//   ran raw stores counts as a write-back of unknown range until it completes (chunk_hits): the next CPU read of guest memory waits for it, as nvn_mtl.m counts the stores as a
//   write anywhere until it has learned their pools. Each store also sets its table entry's bit in a write log (DMSH_GMEM_LOG_CB_OFFSET): every draw with stores gets a zeroed
//   log from the arena, read when its chunk completes, and the pools it names are logged (the Mac's learned pools are the next step). DM_VK_RAW_GMEM_STORE_WAIT=0: not waited for
// Opt-in per game: DM_VK_RAW_GMEM=1, from env.txt or the game's android_defaults.txt (Frostpunk's), which front_android.c reads before the window's surface, so the device is
// made with the extensions only for such a game. Off, nothing changes: the device is made as before, no pool is placed and the table binding reads 16 zero bytes. Only
// Qualcomm's Adreno has run it (its own driver and Turnip), and Samsung's Xclipse (its driver compiles the physical pointers, and the AHardwareBuffer route's pages read
// cached: Frostpunk's meshes on a Galaxy Z Flip 7). Another GPU stays off unless DM_VK_RAW_GMEM_ANYGPU=1 (Mali would take the AHardwareBuffer route too, not shown there).
// A driver that refuses OpConvertUToPtr (the Ayn Odin 2's Adreno 740, the Razr's 730) reaches the pieces through a descriptor array instead (gfx_vk_gmem_noptr.inc, spv_noptr.h).
#ifndef VK_EXT_map_memory_placed   // Vulkan 1.3.290; the NDK r28 headers are 1.3.275
#define VK_EXT_map_memory_placed 1
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT ((VkStructureType)1000272000)
#define VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_PROPERTIES_EXT ((VkStructureType)1000272001)
#define VK_STRUCTURE_TYPE_MEMORY_MAP_PLACED_INFO_EXT ((VkStructureType)1000272002)
#define VK_MEMORY_MAP_PLACED_BIT_EXT 0x00000001
#define VK_MEMORY_UNMAP_RESERVE_BIT_EXT 0x00000001
typedef struct { VkStructureType sType; void *pNext; VkBool32 memoryMapPlaced, memoryMapRangePlaced, memoryUnmapReserve; } VkPhysicalDeviceMapMemoryPlacedFeaturesEXT;
typedef struct { VkStructureType sType; void *pNext; VkDeviceSize minPlacedMemoryMapAlignment; } VkPhysicalDeviceMapMemoryPlacedPropertiesEXT;
typedef struct { VkStructureType sType; const void *pNext; void *pPlacedAddress; } VkMemoryMapPlacedInfoEXT;
#endif
#ifdef __ANDROID__
#include <android/hardware_buffer.h>
#endif
#include <sys/socket.h>
#ifndef MADV_POPULATE_READ
#define MADV_POPULATE_READ 22
#endif
#include "gmem_table.h"   // the table's layout and the translator's lookup, free of Vulkan (dm/tools/tests/test_gmem_table.py)
enum { GM_TAB_BYTES = GMEM_TAB_BYTES, GM_LOG_BYTES = DMSH_GMEM_MAP_ENTRIES / 8, GM_SEEN = 256 };
// A piece's buffer is bound in place as whatever the game's buffer at that address is read as (gfx_vk_gmem_inplace.inc; a dispatch's write read as a vertex stream or a buffer
// texture through fwd_hit), so it takes every buffer usage
#define GM_USAGE (VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT_KHR | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | \
                  VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | \
                  VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT)
#ifndef __ANDROID__   // Linux (dm/linux): no AHardwareBuffers, so no device lists their extension and the route is placed maps only (g_gm_has_ahb stays 0)
typedef struct AHardwareBuffer AHardwareBuffer;
typedef PFN_vkVoidFunction PFN_vkGetAndroidHardwareBufferPropertiesANDROID;
static void AHardwareBuffer_release(AHardwareBuffer *b) { (void)b; }
#endif
typedef struct { uintptr_t lo, hi; const void *pool; VkDeviceMemory m; VkBuffer b; uint64_t gpu; AHardwareBuffer *ahb; uint64_t *rd; uint32_t slot; } gm_seg;   // one placed allocation over [lo, hi) of the pool at pool;
                                                                                                                                  // ahb: the AHardwareBuffer route's buffer; rd: each page's last in-place read (gfx_vk_gmem_inplace.inc);
                                                                                                                                  // slot: its place in the pools' descriptor array (gfx_vk_gmem_noptr.inc)
static void gm_np_made(void); static uint32_t gm_np_slot(VkBuffer b, VkDeviceSize n); static void gm_np_free(uint32_t s); static void gm_np_stat(void);   // gfx_vk_gmem_noptr.inc
static int g_gm_on;   // the device has a route (gm_feat_take, gm_device_made); gm_on also asks gm_game
static int g_gm_mode, g_gm_has_placed, g_gm_has_ahb;   // the route: 1 placed maps, 2 AHardwareBuffers; what the device offers
static PFN_vkGetAndroidHardwareBufferPropertiesANDROID g_gm_ahbp;
static PFN_vkMapMemory2KHR g_gm_map2; static PFN_vkUnmapMemory2KHR g_gm_unmap2; static PFN_vkGetBufferDeviceAddressKHR g_gm_bda;
static VkPhysicalDeviceBufferDeviceAddressFeaturesKHR g_gm_bdq; static VkPhysicalDeviceMapMemoryPlacedFeaturesEXT g_gm_mpq;   // what the device offers
static VkPhysicalDeviceBufferDeviceAddressFeaturesKHR g_gm_bde; static VkPhysicalDeviceMapMemoryPlacedFeaturesEXT g_gm_mpe;   // what it is made with
static VkDeviceSize g_gm_piece = 1ull << 30, g_gm_align = 4096; static uint32_t g_gm_mt = UINT32_MAX; static uint64_t g_gm_cap;
static pthread_mutex_t g_gm_pl = PTHREAD_MUTEX_INITIALIZER;   // one placement or give-back at a time
static pthread_mutex_t g_gm_mu = PTHREAD_MUTEX_INITIALIZER;   // the segment list itself (held only for its edits and the table's reads of it)
static gm_seg *g_gm; static int g_ngm, g_cgm;   // sorted by lo, disjoint
static _Atomic uint64_t g_gm_gen = 1;   // bumped by every placement and give-back: the table is built again at its next bind
static uint64_t g_gm_bytes, g_gm_peak, g_gm_capped, g_gm_failed, g_gm_overlap, g_gm_given, g_gm_kept, g_gm_place_ns, g_gm_give_ns;
static uint64_t g_gm_tabs, g_gm_draws, g_gm_stdraws, g_gm_logs, g_gm_logbits, g_gm_unsynced, g_gm_synced;
static int gm_game(void);   // below: whether this game takes raw global memory
static int gm_on(void) { return g_gm_on && gm_game(); }
static int gm_store_wait(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_RAW_GMEM_STORE_WAIT"); v = !(e && *e == '0'); if (!v) LOG("[vk] DM_VK_RAW_GMEM_STORE_WAIT=0: CPU reads of guest memory do not wait for raw stores"); } return v; }

// ---- The device (vk_create): the extensions and features, only with DM_VK_RAW_GMEM=1 ----
static int gm_env(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_RAW_GMEM"); v = e && *e == '1'; } return v; }
static int gm_game(void) { return gm_env(); }   // (the game's own choice, through its android_defaults.txt)
static int gm_want(const VkExtensionProperties *e, uint32_t n) {
    if (!gm_env()) return 0;
    { const char *a = getenv("DM_VK_RAW_GMEM_ANYGPU"); uint32_t v = g_vk.props.vendorID;   // 0x5143 Qualcomm, 0x144d Samsung
      if (v != 0x5143 && v != 0x144d && !(a && *a == '1')) {
        LOG("[RED] [vk] raw global memory: DM_VK_RAW_GMEM=1, but it has run only on Qualcomm's Adreno and Samsung's Xclipse, and this GPU is vendor %#x: off, raw loads read 0 (DM_VK_RAW_GMEM_ANYGPU=1 tries it)", v); return 0; } }
    g_gm_has_placed = has_ext(e, n, "VK_KHR_map_memory2") && has_ext(e, n, "VK_EXT_map_memory_placed");
    g_gm_has_ahb = has_ext(e, n, "VK_ANDROID_external_memory_android_hardware_buffer") && has_ext(e, n, "VK_EXT_queue_family_foreign");
    int ok = has_ext(e, n, "VK_KHR_buffer_device_address") && (g_gm_has_placed || g_gm_has_ahb);
    // Virtual pools alias physical pages only when the guest's memory is in files (DM_GUEST_FILE=1, hle_nvn.c vpool_map): a placed map would replace such an alias
    const char *gf = getenv("DM_GUEST_FILE"); if (ok && gf && *gf == '1') { LOG("[RED] [vk] raw global memory: off with DM_GUEST_FILE=1 (a placed pool would replace a virtual pool's alias of its pages)"); return 0; }
    if (!ok) LOG("[RED] [vk] raw global memory: DM_VK_RAW_GMEM=1, but this GPU lacks VK_KHR_buffer_device_address, or both VK_EXT_map_memory_placed and VK_ANDROID_external_memory_android_hardware_buffer: raw loads read 0");
    return ok;
}
static void *gm_feat_query(void *next) {   // into vkGetPhysicalDeviceFeatures2's chain
    g_gm_bdq = (VkPhysicalDeviceBufferDeviceAddressFeaturesKHR){.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_KHR, .pNext = next};
    g_gm_mpq = (VkPhysicalDeviceMapMemoryPlacedFeaturesEXT){.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT, .pNext = &g_gm_bdq};
    return &g_gm_mpq;
}
static int gm_feat_take(const char **dext, uint32_t *nde) {   // after the query: the route, and its extensions to enable
    const char *e = getenv("DM_VK_RAW_GMEM_AHB"); int ahb_first = e && *e == '1';
    g_gm_mode = g_gm_has_placed && g_gm_mpq.memoryMapPlaced && !(ahb_first && g_gm_has_ahb) ? 1 : g_gm_has_ahb ? 2 : 0;
    if (!g_gm_bdq.bufferDeviceAddress || !g_gm_mode) { LOG("[RED] [vk] raw global memory: bufferDeviceAddress %u, memoryMapPlaced %u, AHardwareBuffer import %u: off, raw loads read 0", g_gm_bdq.bufferDeviceAddress, g_gm_mpq.memoryMapPlaced, g_gm_has_ahb); return 0; }
    dext[(*nde)++] = "VK_KHR_buffer_device_address";
    if (g_gm_mode == 1) { dext[(*nde)++] = "VK_KHR_map_memory2"; dext[(*nde)++] = "VK_EXT_map_memory_placed"; }
    else { dext[(*nde)++] = "VK_ANDROID_external_memory_android_hardware_buffer"; dext[(*nde)++] = "VK_EXT_queue_family_foreign"; }
    return 1;
}
static void *gm_feat_enable(void *next) {   // into vkCreateDevice's chain
    g_gm_bde = (VkPhysicalDeviceBufferDeviceAddressFeaturesKHR){.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_KHR, .pNext = next, .bufferDeviceAddress = VK_TRUE};
    if (g_gm_mode != 1) return &g_gm_bde;
    g_gm_mpe = (VkPhysicalDeviceMapMemoryPlacedFeaturesEXT){.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT, .pNext = &g_gm_bde, .memoryMapPlaced = VK_TRUE,
                                                          .memoryUnmapReserve = g_gm_mpq.memoryUnmapReserve};
    return &g_gm_mpe;
}
static uint64_t gm_size_env(const char *name) {   // bytes, or whole K / M / G; 0 when unset
    const char *s = getenv(name); if (!s || !*s) return 0;
    char *end; unsigned long long v = strtoull(s, &end, 0); int sh = *end == 'G' || *end == 'g' ? 30 : *end == 'M' || *end == 'm' ? 20 : *end == 'K' || *end == 'k' ? 10 : 0;
    return (uint64_t)v << sh;
}
static void gm_frz_register(void);   // gfx_vk_gmem_lazy.inc
static const char *gm_cap_str(void) { static char b[96]; snprintf(b, sizeof b, ", at most %.2f GiB placed (DM_VK_RAW_GMEM_MAX%s)", g_gm_cap / 1073741824.0, getenv("DM_VK_RAW_GMEM_MAX") ? "" : "; a quarter of the device's memory"); return b; }
static void gm_device_made(void) {   // the device has the extensions: their entry points, the allocation limits and the memory type a pool's pages take
    g_gm_map2 = (PFN_vkMapMemory2KHR)vkGetDeviceProcAddr(g_vk.dev, "vkMapMemory2KHR"); g_gm_unmap2 = (PFN_vkUnmapMemory2KHR)vkGetDeviceProcAddr(g_vk.dev, "vkUnmapMemory2KHR");
    g_gm_bda = (PFN_vkGetBufferDeviceAddressKHR)vkGetDeviceProcAddr(g_vk.dev, "vkGetBufferDeviceAddressKHR");
    g_gm_ahbp = (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)vkGetDeviceProcAddr(g_vk.dev, "vkGetAndroidHardwareBufferPropertiesANDROID");
    if (!g_gm_bda || (g_gm_mode == 1 ? !g_gm_map2 || !g_gm_unmap2 : !g_gm_ahbp)) { LOG("[RED] [vk] raw global memory: the extensions' entry points are missing: off"); return; }
    VkPhysicalDeviceMapMemoryPlacedPropertiesEXT pp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_PROPERTIES_EXT};
    VkPhysicalDeviceMaintenance3Properties m3 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES, .pNext = &pp};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &m3}; vkGetPhysicalDeviceProperties2(g_vk.pd, &p2);
    if (m3.maxMemoryAllocationSize) g_gm_piece = m3.maxMemoryAllocationSize & ~(VkDeviceSize)0xFFFF;
    long pg = sysconf(_SC_PAGESIZE); g_gm_align = pp.minPlacedMemoryMapAlignment > (VkDeviceSize)pg ? pp.minPlacedMemoryMapAlignment : (VkDeviceSize)pg;
    // A pool's pages are what the game's CPU code reads and writes, its atomics included: the type must be cached (a write-combined or uncached mapping would make every guest
    // access slow, and exclusives on it may never succeed), and coherent, since nothing flushes. Device-local when the GPU offers it so
    VkBufferCreateInfo bc = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 65536, .usage = GM_USAGE};
    VkBuffer b; VkMemoryRequirements mr = {0};
    if (vkCreateBuffer(g_vk.dev, &bc, NULL, &b) == VK_SUCCESS) { vkGetBufferMemoryRequirements(g_vk.dev, b, &mr); vkDestroyBuffer(g_vk.dev, b, NULL); }
    const VkMemoryPropertyFlags hv = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    g_gm_mt = vkb_mem_type(mr.memoryTypeBits, hv | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT); if (g_gm_mt == UINT32_MAX) g_gm_mt = vkb_mem_type(mr.memoryTypeBits, hv);
    if (g_gm_mode == 2) { long pgs = sysconf(_SC_PAGESIZE); g_gm_align = (VkDeviceSize)pgs; if (g_gm_piece > (256u << 20)) g_gm_piece = 256u << 20; }   // (the buffer's own memory types: gm_place1_ahb)
    else if (g_gm_mt == UINT32_MAX) { LOG("[RED] [vk] raw global memory: no host-visible, coherent and cached memory type for the pools (of the types %#x): off", mr.memoryTypeBits); return; }
    gm_np_made();   // a driver that refuses OpConvertUToPtr: the pools' descriptor array, and pieces no larger than one descriptor reaches (gfx_vk_gmem_noptr.inc)
    // Unset, the cap is a quarter of the device's memory: placed memory is committed whole, and Ocarina of Time places about 2 GiB in Kokiri Forest (PSS 8.7 → 10.8 GB on a
    // Fold with 10.8 GB). A pool past it stays anonymous and is copied as before, and raw accesses to it read 0 (red line 162), rather than the low-memory killer ending the game
    gm_frz_register();   // the freeze window's hooks (gfx_vk_gmem_lazy.inc), before any pool is placed
    g_gm_cap = gm_size_env("DM_VK_RAW_GMEM_MAX");
    if (!g_gm_cap) { long pages = sysconf(_SC_PHYS_PAGES), psz = sysconf(_SC_PAGESIZE); if (pages > 0 && psz > 0) g_gm_cap = (uint64_t)pages * (uint64_t)psz / 4; }
    g_gm_on = 1;
    if (g_gm_mode == 2) LOG("[vk] raw global memory on (DM_VK_RAW_GMEM=1): memory pools become AHardwareBuffers mapped at their own addresses and imported with device addresses "
                            "(%s), pieces of up to %llu MiB%s",
                            g_gm_has_placed ? "DM_VK_RAW_GMEM_AHB=1" : "no VK_EXT_map_memory_placed", (unsigned long long)(g_gm_piece >> 20), g_gm_cap ? gm_cap_str() : "");
    else LOG("[vk] raw global memory on (DM_VK_RAW_GMEM=1): memory pools become placed Vulkan memory at their own addresses, memory type %u (flags %#x), pieces of up to %llu MiB, "
        "alignment %llu, unmap reserve %u%s", g_gm_mt, g_vk.mp.memoryTypes[g_gm_mt].propertyFlags,
        (unsigned long long)(g_gm_piece >> 20), (unsigned long long)g_gm_align, g_gm_mpq.memoryUnmapReserve, g_gm_cap ? gm_cap_str() : "");
}

// ---- Placing a pool: when the game first takes a buffer address in it (mtl_pool_addressed), as the Mac lists only such pools ----
// Placing every pool at Initialize committed 4.9 GB in Ocarina of Time, and the low-memory killer ended it at Kokiri Forest. The pools the game took a buffer address in are 1.8 GB;
// the others (3.4 GB, one of them 2.4 GB) no raw pointer can reach: a pointer the game stores is a buffer's address. A pool is placed at its first buffer address, before the
// game has the address. That is after Initialize, so the swap is made with the replay held (g_bk: no copy from the pool meanwhile), the GPU waited for when a write still to land
// overlaps the pool (gm_pending), and inside nvn_dirty_open / nvn_dirty_close: the dirty tracker's protected pages of the pool are opened and hashed, and protected again
// on the placed map afterwards (a page whose bytes changed counts as written). Another guest thread writing the pool during the swap (between the copy and the placed map) would lose its write; a pool's first buffer
// is normally made by the thread that made the pool, before anything else uses it. Nothing is logged inside the window: a thread holding the log's lock may be waiting for
// the tracker (Banana Native: Pokemon Pokopia stopped there). A pool made again at an address that had a buffer address is placed at its Initialize (the game's address
// cache, hle_nvn.c pool_mark_addressed, does not ask again for the same storage)
typedef struct { int what; VkResult r; size_t n; uintptr_t lo; void *q; double mbs, ms; } gm_note;   // what went wrong or was measured, logged once the swap is over
// Whether something still to land writes into [lo, hi) (caller holds g_bk, so the replay records nothing meanwhile): a write-back, a queued copy, a counter report or an event
// word, which the completion thread writes when its chunk completes. Only then is the GPU waited for before the swap. The GPU itself reaches a pool's pages only once it
// is placed, and what the chunks in flight read of it was copied when they were recorded. A swap waited for everything before (all_wait): Kokiri Forest places pools all
// the way through, about 2,000 pieces a minute on the Fold, and each wait emptied the GPU's queue
static uint64_t g_gm_waits, g_gm_resident;   // swaps that waited; bytes of the pools placed that were resident before (mincore)
static int gm_pend_in(const chunk *c, uintptr_t lo, uintptr_t hi) {
    if (chunk_hits(c, lo, hi, 1)) return 1;
    for (int i = 0; i < c->npost; i++) { const post_op *o = &c->post[i]; uintptr_t a = o->kind == 1 ? (uintptr_t)o->val : (uintptr_t)o->addr; if (a + 32 > lo && a < hi) return 1; }
    return 0;
}
static int gm_pending(uintptr_t lo, uintptr_t hi) {
    if (C && gm_pend_in(C, lo, hi)) return 1;
    int hit = 0; pthread_mutex_lock(&g_cmu);
    for (int f = 0; f < g_fifo_n && !hit; f++) hit = gm_pend_in(g_fifo[f], lo, hi);
    pthread_mutex_unlock(&g_cmu);
    return hit;
}
static __thread gm_note t_gn;
static uint64_t gm_now(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC); }
static void fb_placed(uintptr_t lo, uintptr_t hi);   // gfx_vk_fileback.inc: a placed range out of the pool file
static void gm_seg_insert(const gm_seg *s) {   // caller holds g_gm_mu
    if (g_ngm == g_cgm) { g_cgm = g_cgm ? 2 * g_cgm : 256; g_gm = realloc(g_gm, (size_t)g_cgm * sizeof *g_gm); if (!g_gm) die("raw global memory: the segment list"); }
    int at = g_ngm; while (at > 0 && g_gm[at - 1].lo > s->lo) at--;
    memmove(g_gm + at + 1, g_gm + at, (size_t)(g_ngm - at) * sizeof *g_gm); g_gm[at] = *s; g_ngm++;
    if (g_gm_np && !g_gm[at].slot) g_gm[at].slot = gm_np_slot(s->b, s->hi - s->lo);   // (a piece listed again after a failed give-back keeps its slot)
    fb_placed(s->lo, s->hi);
}
static int gm_place1(uintptr_t lo, size_t n, gm_seg *s) {   // [lo, lo + n) as one placed allocation; 0: the range is as it was (t_gn says why). Logs nothing
    memset(s, 0, sizeof *s); s->lo = lo; s->hi = lo + n; t_gn = (gm_note){0, VK_SUCCESS, n, lo, NULL, 0, 0};
    VkBufferCreateInfo bc = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n, .usage = GM_USAGE};
    if (vkCreateBuffer(g_vk.dev, &bc, NULL, &s->b) != VK_SUCCESS) { t_gn.what = 1; return 0; }
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_vk.dev, s->b, &mr);
    // a placed map is of the whole allocation (memoryMapRangePlaced is 0 on the Adreno 840): one larger than the range would cover the bytes after it
    if (mr.size != n || !(mr.memoryTypeBits >> g_gm_mt & 1)) { t_gn.what = 2; t_gn.r = (VkResult)mr.memoryTypeBits; t_gn.q = (void *)(uintptr_t)mr.size; vkDestroyBuffer(g_vk.dev, s->b, NULL); return 0; }
    VkMemoryAllocateFlagsInfo fi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT_KHR};
    VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &fi, .allocationSize = n, .memoryTypeIndex = g_gm_mt};
    if ((t_gn.r = vkAllocateMemory(g_vk.dev, &ma, NULL, &s->m)) != VK_SUCCESS) { t_gn.what = 3; vkDestroyBuffer(g_vk.dev, s->b, NULL); return 0; }
    void *p = NULL; uint64_t t0 = gm_now();
    if (vkMapMemory(g_vk.dev, s->m, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS || !p) { t_gn.what = 4; vkFreeMemory(g_vk.dev, s->m, NULL); vkDestroyBuffer(g_vk.dev, s->b, NULL); return 0; }
    memcpy(p, (const void *)lo, n);   // the pool's bytes (a page never touched reads the zero page)
    vkUnmapMemory(g_vk.dev, s->m);
    if ((t_gn.r = vkBindBufferMemory(g_vk.dev, s->b, s->m, 0)) != VK_SUCCESS) { t_gn.what = 11; vkFreeMemory(g_vk.dev, s->m, NULL); vkDestroyBuffer(g_vk.dev, s->b, NULL); return 0; }   // before the pages are replaced
    VkMemoryMapPlacedInfoEXT pi = {.sType = VK_STRUCTURE_TYPE_MEMORY_MAP_PLACED_INFO_EXT, .pPlacedAddress = (void *)lo};
    VkMemoryMapInfoKHR mi = {.sType = VK_STRUCTURE_TYPE_MEMORY_MAP_INFO_KHR, .pNext = &pi, .flags = VK_MEMORY_MAP_PLACED_BIT_EXT, .memory = s->m, .offset = 0, .size = VK_WHOLE_SIZE};
    void *q = NULL; VkResult r = g_gm_map2(g_vk.dev, &mi, &q);
    if (r != VK_SUCCESS || q != (void *)lo) {
        // A failed MAP_FIXED may have taken the old pages down already: anonymous pages go back with the bytes, which the allocation still holds. Raw global memory is off from now on
        t_gn.what = 5; t_gn.r = r; t_gn.q = q;
        if (r == VK_SUCCESS) { VkMemoryUnmapInfoKHR ui = {.sType = VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO_KHR, .memory = s->m}; g_gm_unmap2(g_vk.dev, &ui); }
        unsigned char probe; if (vm_read_overwrite(mach_task_self(), lo, 1, (vm_address_t)&probe, NULL) != KERN_SUCCESS) {
            void *a = mmap((void *)lo, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
            if (a != (void *)lo || vkMapMemory(g_vk.dev, s->m, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) die("raw global memory: pool memory %#lx+%#zx lost in a failed placed map", (unsigned long)lo, n);
            memcpy(a, p, n); vkUnmapMemory(g_vk.dev, s->m); }
        vkFreeMemory(g_vk.dev, s->m, NULL); vkDestroyBuffer(g_vk.dev, s->b, NULL); g_gm_on = 0; return 0;
    }
    if (madvise((void *)lo, n, MADV_POPULATE_READ)) for (uintptr_t a = lo; a < lo + n; a += 4096) (void)*(volatile const uint8_t *)a;   // every page now (see the top)
    VkBufferDeviceAddressInfoKHR ai = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO_KHR, .buffer = s->b}; s->gpu = g_gm_bda(g_vk.dev, &ai);
    uint64_t dt = gm_now() - t0; g_gm_place_ns += dt;
    static int probed; if (!probed && n >= (16u << 20)) {   // how fast the CPU reads the placed pages: a slow read would mean the "cached" type is mapped uncached (the Retroid Pocket 5's driver does)
        probed = 1; uint64_t r0 = gm_now(), sum = 0; for (const uint64_t *w = (const uint64_t *)lo, *e = w + (16u << 20) / 8; w < e; w++) sum += *(volatile const uint64_t *)w;
        t_gn.what = 9; t_gn.mbs = 16.0 / ((gm_now() - r0) / 1e9) + (double)(sum & 0); t_gn.ms = dt / 1e6; }
    return 1;
}
#ifdef __ANDROID__
// The AHardwareBuffer route: the dma-buf of a BLOB buffer, which AHardwareBuffer_sendHandleToUnixSocket passes as the first descriptor of its native handle (the public way to it)
static int gm_ahb_fd(AHardwareBuffer *b) {
    int sv[2], fd = -1; if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv)) return -1;
    if (!AHardwareBuffer_sendHandleToUnixSocket(b, sv[0])) {
        char data[4096], ctl[CMSG_SPACE(sizeof(int) * 32)]; struct iovec io = {data, sizeof data};
        struct msghdr m = {.msg_iov = &io, .msg_iovlen = 1, .msg_control = ctl, .msg_controllen = sizeof ctl};
        if (recvmsg(sv[1], &m, MSG_CMSG_CLOEXEC) > 0)
            for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c)) if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
                int k = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int)); int *f = (int *)(void *)CMSG_DATA(c);
                for (int i = 0; i < k; i++) { if (fd < 0) fd = f[i]; else close(f[i]); } }
    }
    close(sv[0]); close(sv[1]); return fd;
}
static int gm_place1_ahb(uintptr_t lo, size_t n, gm_seg *s) {   // as gm_place1, through an AHardwareBuffer; 0: the range is as it was (t_gn says why). Logs nothing
    memset(s, 0, sizeof *s); s->lo = lo; s->hi = lo + n; t_gn = (gm_note){0, VK_SUCCESS, n, lo, NULL, 0, 0};
    AHardwareBuffer_Desc d = {.width = (uint32_t)n, .height = 1, .layers = 1, .format = AHARDWAREBUFFER_FORMAT_BLOB,
                              .usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN | AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER};
    if (AHardwareBuffer_allocate(&d, &s->ahb) || !s->ahb) { t_gn.what = 6; s->ahb = NULL; return 0; }
    int fd = gm_ahb_fd(s->ahb); uint64_t t0 = gm_now();
    if (fd >= 0 && lseek(fd, 0, SEEK_END) < (off_t)n) { close(fd); fd = -1; }   // the handle's first descriptor is the dma-buf on the Adreno's gralloc; one shorter than the buffer would fault the copy below
    if (fd < 0) { t_gn.what = 7; AHardwareBuffer_release(s->ahb); s->ahb = NULL; return 0; }
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { t_gn.what = 8; close(fd); AHardwareBuffer_release(s->ahb); s->ahb = NULL; return 0; }
    memcpy(p, (const void *)lo, n);   // the pool's bytes
    // imported first: a failure leaves the guest's pages as they are
    VkAndroidHardwareBufferPropertiesANDROID hp = {.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
    VkExternalMemoryBufferCreateInfo eb = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID};
    VkBufferCreateInfo bc = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &eb, .size = n, .usage = GM_USAGE};
    int ok = (t_gn.r = g_gm_ahbp(g_vk.dev, s->ahb, &hp)) == VK_SUCCESS && hp.memoryTypeBits && vkCreateBuffer(g_vk.dev, &bc, NULL, &s->b) == VK_SUCCESS;
    if (ok) {
        VkMemoryRequirements br; vkGetBufferMemoryRequirements(g_vk.dev, s->b, &br); uint32_t bits = hp.memoryTypeBits & br.memoryTypeBits;   // a type the buffer takes too
        uint32_t mt = bits ? vkb_mem_type(bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) : UINT32_MAX; if (mt == UINT32_MAX && bits) mt = (uint32_t)__builtin_ctz(bits);
        VkImportAndroidHardwareBufferInfoANDROID ii = {.sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, .buffer = s->ahb};
        VkMemoryDedicatedAllocateInfo di = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .pNext = &ii, .buffer = s->b};
        VkMemoryAllocateFlagsInfo fi = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, .pNext = &di, .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT_KHR};
        VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &fi, .allocationSize = hp.allocationSize, .memoryTypeIndex = mt};
        ok = mt != UINT32_MAX && (t_gn.r = vkAllocateMemory(g_vk.dev, &ma, NULL, &s->m)) == VK_SUCCESS;
        if (ok && (t_gn.r = vkBindBufferMemory(g_vk.dev, s->b, s->m, 0)) != VK_SUCCESS) { vkFreeMemory(g_vk.dev, s->m, NULL); ok = 0; }
        if (!ok) vkDestroyBuffer(g_vk.dev, s->b, NULL);
    }
    if (!ok) { t_gn.what = 10; munmap(p, n); close(fd); AHardwareBuffer_release(s->ahb); s->ahb = NULL; return 0; }
    void *q = mmap((void *)lo, n, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
    close(fd);
    if (q != (void *)lo) {   // a failed MAP_FIXED may have taken the old pages down: anonymous pages with the bytes, from the first mapping. Raw global memory is off from now on
        t_gn.what = 5; t_gn.q = q;
        void *a = mmap((void *)lo, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (a != (void *)lo) die("raw global memory: pool memory %#lx+%#zx lost in a failed AHardwareBuffer map", (unsigned long)lo, n);
        memcpy(a, p, n); munmap(p, n); vkFreeMemory(g_vk.dev, s->m, NULL); vkDestroyBuffer(g_vk.dev, s->b, NULL); AHardwareBuffer_release(s->ahb); s->ahb = NULL; g_gm_on = 0; return 0;
    }
    munmap(p, n);
    if (madvise((void *)lo, n, MADV_POPULATE_READ)) for (uintptr_t a = lo; a < lo + n; a += 4096) (void)*(volatile const uint8_t *)a;
    VkBufferDeviceAddressInfoKHR ai = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO_KHR, .buffer = s->b}; s->gpu = g_gm_bda(g_vk.dev, &ai);
    uint64_t dt = gm_now() - t0; g_gm_place_ns += dt;
    static int probed; if (!probed && n >= (16u << 20)) {   // how fast the CPU reads the mapped dma-buf: slow would mean an uncached mapping
        probed = 1; uint64_t r0 = gm_now(), sum = 0; for (const uint64_t *w = (const uint64_t *)lo, *e = w + (16u << 20) / 8; w < e; w++) sum += *(volatile const uint64_t *)w;
        t_gn.what = 9; t_gn.mbs = 16.0 / ((gm_now() - r0) / 1e9) + (double)(sum & 0); t_gn.ms = dt / 1e6; }
    return 1;
}
#else
static int gm_place1_ahb(uintptr_t lo, size_t n, gm_seg *s) { (void)lo; (void)n; (void)s; t_gn.what = 6; return 0; }   // never chosen on Linux (g_gm_has_ahb is 0)
#endif
static void gm_note_log(const gm_note *g) {   // t_gn, outside the swap
    switch (g->what) {
    case 1: LOG("[RED] [vk] raw global memory: vkCreateBuffer of %zu bytes failed: the pool at %#lx stays anonymous", g->n, (unsigned long)g->lo); break;
    case 2: LOG("[RED] [vk] raw global memory: a pool piece of %zu bytes needs %llu bytes of memory (types %#x): not placed", g->n, (unsigned long long)(uintptr_t)g->q, (unsigned)g->r); break;
    case 3: { static int nl; if (nl++ < 4) LOG("[RED] [vk] raw global memory: vkAllocateMemory of %zu bytes failed (%d) with %.2f GiB placed: the pool at %#lx stays anonymous", g->n, g->r, g_gm_bytes / 1073741824.0, (unsigned long)g->lo); } break;
    case 4: LOG("[RED] [vk] raw global memory: vkMapMemory of %zu bytes failed: the pool at %#lx stays anonymous", g->n, (unsigned long)g->lo); break;
    case 5: LOG("[RED] [vk] raw global memory: the placed map of %#lx+%#zx failed (%d, at %p): raw global memory is off from now on", (unsigned long)g->lo, g->n, g->r, g->q); break;
    case 6: LOG("[RED] [vk] raw global memory: AHardwareBuffer_allocate of a %zu-byte BLOB failed: the pool at %#lx stays anonymous", g->n, (unsigned long)g->lo); break;
    case 7: LOG("[RED] [vk] raw global memory: no dma-buf from the AHardwareBuffer of %zu bytes: the pool at %#lx stays anonymous", g->n, (unsigned long)g->lo); break;
    case 8: LOG("[RED] [vk] raw global memory: the AHardwareBuffer of %zu bytes could not be mapped: the pool at %#lx stays anonymous", g->n, (unsigned long)g->lo); break;
    case 10: LOG("[RED] [vk] raw global memory: the AHardwareBuffer of %zu bytes was not imported into Vulkan (%d): the pool at %#lx stays anonymous", g->n, g->r, (unsigned long)g->lo); break;
    case 11: LOG("[RED] [vk] raw global memory: a pool piece's memory of %zu bytes could not be bound to its buffer (%d): the pool at %#lx stays anonymous", g->n, g->r, (unsigned long)g->lo); break;
    case 9: LOG("%s[vk] raw global memory: placed pages read at %.0f MB/s (a 16 MiB sweep); placing %zu MiB took %.1f ms", g->mbs < 2000 ? "[RED] " : "", g->mbs, g->n >> 20, g->ms); break;
    default: break; }
}
// [lo, lo + n) of the pool made placed memory, in pieces of at most g_gm_piece, each a segment; caller holds g_gm_pl. live: the range may be in use, so with g_bk held (the
// replay records nothing meanwhile), the GPU waited for when a write still to land overlaps it (gm_pending; may_wait 0: then nothing is done, -1), and inside
// nvn_dirty_open / nvn_dirty_close. freeze: game threads may be writing the range (a chunk placed long after its pool's first buffer address), so it is read-only from the
// copy to the placed map and a write meanwhile waits in gm_frz_hook (gfx_vk_gmem_lazy.inc). → the pieces placed; fail / probe: what to log once outside
static int gm_frz_begin(uintptr_t lo, uintptr_t hi); static void gm_frz_end(uintptr_t lo, uintptr_t hi);   // gfx_vk_gmem_lazy.inc
static int gm_swap(const void *pool, uintptr_t lo, size_t n, int live, int freeze, int may_wait, gm_note *fail, gm_note *probe) {
    uintptr_t hi = lo + n;
    int bk = live && !t_bk_sub; if (bk) pthread_mutex_lock(&g_bk);
    if (live && gm_pending(lo, hi)) { if (!may_wait) { if (bk) pthread_mutex_unlock(&g_bk); return -1; } all_wait(); g_gm_waits++; }
    if (live) nvn_dirty_open((const void *)lo, n);
    int frz = freeze && gm_frz_begin(lo, hi);
    int placed = 0; uintptr_t a = lo;
    for (; a < hi && gm_on(); a += g_gm_piece) {   // pieces placed before a failure stay placed: each is an entry of its own
        size_t k = hi - a < g_gm_piece ? (size_t)(hi - a) : (size_t)g_gm_piece; gm_seg s;
        int ok = g_gm_mode == 2 ? gm_place1_ahb(a, k, &s) : gm_place1(a, k, &s); if (t_gn.what == 9) *probe = t_gn;
        if (!ok) { *fail = t_gn; g_gm_failed++; break; }
        s.pool = pool; s.rd = calloc((k + 4095) >> 12, sizeof *s.rd); placed++;
        pthread_mutex_lock(&g_gm_mu); gm_seg_insert(&s); g_gm_bytes += k; if (g_gm_bytes > g_gm_peak) g_gm_peak = g_gm_bytes; atomic_fetch_add(&g_gm_gen, 1); pthread_mutex_unlock(&g_gm_mu);
    }
    if (frz) { if (a < hi) mprotect((void *)a, hi - a, PROT_READ | PROT_WRITE); gm_frz_end(lo, hi); }   // what was not placed is anonymous and writable again
    if (live) nvn_dirty_close();
    if (bk) pthread_mutex_unlock(&g_bk);
    return placed;
}
static uintptr_t (*g_gm_addr)[2]; static int g_ngma, g_cgma;   // ranges of pools the game took a buffer address in, kept after they are given back (under g_gm_pl)
static int gm_was_addressed(uintptr_t lo, uintptr_t hi) { for (int i = 0; i < g_ngma; i++) if (g_gm_addr[i][0] < hi && lo < g_gm_addr[i][1]) return 1; return 0; }
// Shader-code pools (NVN_MEMORY_POOL_FLAGS_SHADER_CODE) are left unplaced: the replay runs translated shaders and reads a program's own constants on the CPU, and no raw
// pointer reaches shader code. Ocarina of Time has 205 of them, 345 MiB that would be committed and unswappable (BananaNative found it and leaves them out by default,
// DM_VK_PLACE_SHADER_POOLS, after Anka Native). DM_VK_RAW_GMEM_CODE=1 (env.txt): placed as other pools
static uint64_t g_gm_code_skipped;
static int gm_code_skip(int flags) {
    static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_RAW_GMEM_CODE"); v = !(e && *e == '1'); if (!v) LOG("[vk] DM_VK_RAW_GMEM_CODE=1: shader-code pools are placed as other pools"); }
    if (v && (flags & NVN_MEMORY_POOL_FLAGS_SHADER_CODE)) { g_gm_code_skipped++; return 1; }
    return 0;
}
enum { GM_CHUNK = 2u << 20 };   // pools are placed in chunks of GM_CHUNK as they are used (gfx_vk_gmem_lazy.inc); one under it is a single chunk
static uint64_t gm_lazy_min(void);   // the smallest pool placed in chunks: every pool, or DM_VK_RAW_GMEM_LAZY_MIN (gfx_vk_gmem_lazy.inc)
static int gm_lazy_on(void); static void gm_lazy_add(const void *pool, uintptr_t lo, size_t size, int live); static void gm_lazy_drop(const void *pool);
static void gm_place(const void *mem, uint64_t size, int live) {   // live: the pool is in use (mtl_pool_addressed); 0: at its Initialize, before anything uses it
    if (!size || !gm_game() || !vkb() || !gm_on()) return;   // (the game first: for another game the device is not made here, earlier than it would be)
    { static int said; if (!said++) LOG("[vk] raw global memory: memory pools are placed at their first buffer address"); }
    uintptr_t lo = (uintptr_t)mem, hi = lo + size;
    pthread_mutex_lock(&g_gm_pl);
    if (!live && !gm_was_addressed(lo, hi)) { pthread_mutex_unlock(&g_gm_pl); return; }
    if (live && !gm_was_addressed(lo, hi)) { if (g_ngma == g_cgma) { g_cgma = g_cgma ? 2 * g_cgma : 256; g_gm_addr = realloc(g_gm_addr, (size_t)g_cgma * sizeof *g_gm_addr); if (!g_gm_addr) die("raw global memory: the addressed list"); }
        g_gm_addr[g_ngma][0] = lo; g_gm_addr[g_ngma++][1] = hi; }
    pthread_mutex_lock(&g_gm_mu); int over = 0, same = 0; for (int i = 0; i < g_ngm && !over; i++) if (g_gm[i].lo < hi && lo < g_gm[i].hi) { over = 1; same = g_gm[i].pool == mem; } pthread_mutex_unlock(&g_gm_mu);
    if (same) { pthread_mutex_unlock(&g_gm_pl); return; }   // placed already
    if ((lo | size) & (g_gm_align - 1)) { g_gm_failed++; pthread_mutex_unlock(&g_gm_pl); RED_ONCE(161, "raw global memory: a memory pool at %#lx+%#llx is not aligned to %llu bytes: not placed, raw accesses to it read 0", (unsigned long)lo, (unsigned long long)size, (unsigned long long)g_gm_align); return; }
    if (over) { g_gm_overlap++; pthread_mutex_unlock(&g_gm_pl); static int nl; if (nl++ < 4) LOG("[vk] raw global memory: the memory pool at %#lx+%#llx overlaps a placed one: not placed (the placed one's pages serve both)", (unsigned long)lo, (unsigned long long)size); return; }
    { size_t np = (size_t)(size >> 12); unsigned char *v = malloc(np ? np : 1);   // how much of the pool the game had touched: the rest is committed by placing it
      if (v && !mincore((void *)lo, (size_t)size, v)) { size_t r = 0; for (size_t i = 0; i < np; i++) r += v[i] & 1; g_gm_resident += (uint64_t)r << 12; } free(v); }
    if (gm_lazy_on() && size >= gm_lazy_min()) { gm_lazy_add(mem, lo, (size_t)size, live); pthread_mutex_unlock(&g_gm_pl); return; }   // in chunks, as they are used (gfx_vk_gmem_lazy.inc)
    if (g_gm_cap && g_gm_bytes + size > g_gm_cap) { g_gm_capped++; pthread_mutex_unlock(&g_gm_pl); RED_ONCE(162, "raw global memory: %.2f GiB placed and DM_VK_RAW_GMEM_MAX is %.2f GiB: pools from %#lx+%#llx on stay anonymous, raw accesses to them read 0",
                                                                                                g_gm_bytes / 1073741824.0, g_gm_cap / 1073741824.0, (unsigned long)lo, (unsigned long long)size); return; }
    gm_note fail = {0}, probe = {0};
    int placed = gm_swap(mem, lo, (size_t)size, live, 0, 1, &fail, &probe);
    pthread_mutex_unlock(&g_gm_pl);
    if (fail.what) gm_note_log(&fail);
    if (probe.what) gm_note_log(&probe);
    static int nl; if (placed && (nl < 4 || !(nl & 255))) LOG("[vk] raw global memory: %d placed pieces, %.1f MiB (pool %#lx+%#llx%s; %.0f ms placing so far)", g_ngm, g_gm_bytes / 1048576.0, (unsigned long)lo, (unsigned long long)size,
                                                          live ? ", at its first buffer address" : ", at its Initialize: its address had a buffer before", g_gm_place_ns / 1e6);
    nl += placed != 0;
}

// ---- Giving a pool back (mtl_pool_del, nvnMemoryPoolFinalize) ----
// One placed piece back to anonymous pages at its range, the bytes kept (zero pages left untouched, so uncommitted). The GPU must be done with it. Logs nothing (it runs inside a
// lazy pool's merge window too). → 0 when no room was found: the piece stays placed and is listed again
static int gm_seg_giveback(const gm_seg *s) {
    size_t len = s->hi - s->lo;
    uint8_t *t = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (t == MAP_FAILED) {   // no room: the piece stays placed, and listed again, so its memory is still accounted and in the table
        g_gm_kept++; pthread_mutex_lock(&g_gm_mu); gm_seg_insert(s); g_gm_bytes += len; atomic_fetch_add(&g_gm_gen, 1); pthread_mutex_unlock(&g_gm_mu); return 0; }
    for (uintptr_t a = s->lo; a < s->hi; a += 4096) {
        const uint64_t *w = (const uint64_t *)a; uint64_t o = 0; for (int j = 0; j < 512 && !o; j++) o |= w[j];
        if (o) memcpy(t + (a - s->lo), (const void *)a, 4096); }
    if (!s->ahb) {   // a placed map: the free takes the range down, so it comes before the pages move in
        VkMemoryUnmapInfoKHR ui = {.sType = VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO_KHR, .flags = g_gm_mpq.memoryUnmapReserve ? VK_MEMORY_UNMAP_RESERVE_BIT_EXT : 0, .memory = s->m};
        g_gm_unmap2(g_vk.dev, &ui);
        vkDestroyBuffer(g_vk.dev, s->b, NULL); vkFreeMemory(g_vk.dev, s->m, NULL); }
    if (mremap(t, len, len, MREMAP_MAYMOVE | MREMAP_FIXED, (void *)s->lo) != (void *)s->lo) {
        void *a = mmap((void *)s->lo, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (a != (void *)s->lo) die("raw global memory: pool memory %#lx+%#zx could not be given back (errno %d)", (unsigned long)s->lo, len, errno);
        memcpy(a, t, len); munmap(t, len); }
    if (s->ahb) { vkDestroyBuffer(g_vk.dev, s->b, NULL); vkFreeMemory(g_vk.dev, s->m, NULL); AHardwareBuffer_release(s->ahb); }   // the mapping is ours: the pages replaced it first
    gm_np_free(s->slot); free(s->rd);
    g_gm_given++;
    return 1;
}
// The pool's pieces out of the list (g_gm_mu taken here): into *got (grown), → how many. A chunked pool has one per run of chunks placed together
static int gm_seg_take(const void *mem, uintptr_t lo, uintptr_t hi, gm_seg **got, int *cap) {
    int n = 0;
    pthread_mutex_lock(&g_gm_mu);
    for (int i = 0; i < g_ngm;) if (g_gm[i].pool == mem && g_gm[i].lo >= lo && g_gm[i].hi <= hi) {
        if (n == *cap) { *cap = *cap ? 2 * *cap : 64; *got = realloc(*got, (size_t)*cap * sizeof **got); if (!*got) die("raw global memory: the pieces given back"); }
        (*got)[n++] = g_gm[i]; g_gm_bytes -= g_gm[i].hi - g_gm[i].lo; memmove(g_gm + i, g_gm + i + 1, (size_t)(--g_ngm - i) * sizeof *g_gm); } else i++;
    if (n) atomic_fetch_add(&g_gm_gen, 1);   // a draw recorded from here takes a table without them
    pthread_mutex_unlock(&g_gm_mu);
    return n;
}
static void gm_unplace(const void *mem) {
    if (!gm_on() && !g_ngm) return;
    pthread_mutex_lock(&g_gm_pl);
    static gm_seg *got; static int cap;   // (g_gm_pl)
    int n = gm_seg_take(mem, 0, UINTPTR_MAX, &got, &cap);
    gm_lazy_drop(mem);
    if (!n) { pthread_mutex_unlock(&g_gm_pl); return; }
    // The GPU first: a chunk recorded or in flight may still read through the old table (the game finalizes a pool once its own GPU work is done, so this is short). The replay stays
    // held through the give-back: on the placed route a piece's range is unmapped between its free and the pages moving in, and nothing of the replay may read it then
    uint64_t t0 = gm_now(); int kept = 0;
    BK_LOCK(); all_wait();
    for (int k = 0; k < n; k++) kept += !gm_seg_giveback(&got[k]);
    BK_UNLOCK();
    g_gm_give_ns += gm_now() - t0;
    pthread_mutex_unlock(&g_gm_pl);
    if (kept) LOG("[RED] [vk] raw global memory: no room to give %d pieces of pool %p back: they stay placed", kept, mem);
    static int nl; if (nl++ < 4) LOG("[vk] raw global memory: memory pool %p given back as anonymous memory (%d pieces, %.1f MiB still placed)", mem, n - kept, g_gm_bytes / 1048576.0);
}

// ---- The table and the write log (stage_set, the replay's thread under g_bk) ----
// The table: entry 0 empty, then the placed pieces by address, each {start, end, device address minus start}, unused entries starting at ~0, and the 2 MiB chunk hints, as
// nvn_mtl.m gmem_map lays it out (dmsh_abi.h DMSH_GMEM_MAP_*). Built again when a piece comes or goes; the one it replaces is destroyed once the chunk recorded now completes
// The tables replaced go to a short list and are filled again once the chunk that may have read them last has completed: Kokiri Forest builds about 500 a minute as pools
// are placed, and each new one's vkAllocateMemory was 8.6% of the replay thread on the Fold
typedef struct { VkBuffer b; VkDeviceMemory m; uint8_t *p; uint64_t gen, seq; } gm_tab;   // seq: kept until that chunk completed
static void gm_selftest(void);   // DM_VK_RAW_GMEM_TEST (gfx_vk_gmem_check.inc)
static gm_tab g_gt, g_gt_free[8]; static int g_ngt_free; static uint64_t g_gm_tab_new; static uint64_t g_gm_rd_seq;   // g_gm_rd_seq: gfx_vk_gmem_inplace.inc
static int gm_tab_new(gm_tab *t) {   // a new table buffer, host-visible and mapped
    VkBufferCreateInfo bc = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = GM_TAB_BYTES, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    if (vkCreateBuffer(g_vk.dev, &bc, NULL, &t->b) != VK_SUCCESS) return 0;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_vk.dev, t->b, &mr);
    uint32_t mt = vkb_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (mt == UINT32_MAX) mt = vkb_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mt};
    if (mt == UINT32_MAX || vkAllocateMemory(g_vk.dev, &ma, NULL, &t->m) != VK_SUCCESS) { vkDestroyBuffer(g_vk.dev, t->b, NULL); t->b = VK_NULL_HANDLE; return 0; }
    vkBindBufferMemory(g_vk.dev, t->b, t->m, 0);
    if (vkMapMemory(g_vk.dev, t->m, 0, VK_WHOLE_SIZE, 0, (void **)&t->p) != VK_SUCCESS) { vkFreeMemory(g_vk.dev, t->m, NULL); vkDestroyBuffer(g_vk.dev, t->b, NULL); t->b = VK_NULL_HANDLE; return 0; }
    g_gm_tab_new++; return 1;
}
static int gm_sink_ranges(uint64_t (**seg)[3], int *cap, int n); static int gm_sink_is(const uint64_t e[3]); static _Atomic uint64_t g_sink_hits;   // gfx_vk_gmem_lazy.inc
// How much of the placed memory the helper's 2 MiB chunk hints reach (gmem_table.h): a hint covers a whole 2 MiB guest chunk inside one range, so a piece smaller than that, or
// one not on a 2 MiB boundary, has none and every address in it that the invocation's cached entry doesn't hold takes the 13-step search (26 table loads). Said at 50, 100, 200
// and 400 pieces (q291 diagnostic)
static void gm_hint_stat(const uint64_t (*seg)[3], int n) {
    static int said; int lvl = n >= 400 ? 4 : n >= 200 ? 3 : n >= 100 ? 2 : n >= 50 ? 1 : 0;
    if (lvl <= said) return; said = lvl;
    uint64_t tot = 0, hinted = 0; int none = 0;
    for (int i = 0; i < n; i++) { uint64_t a = seg[i][0], z = seg[i][1], sz = 1ull << DMSH_GMEM_HINT_SHIFT, f = (a + sz - 1) & ~(sz - 1), l = z & ~(sz - 1);
        tot += z - a; if (l > f) hinted += l - f; else none++; }
    LOG("[vk] raw global memory: %d placed pieces, %.1f MiB; the 2 MiB chunk hints reach %.1f MiB of them (%.0f %%); %d pieces have no hint (an address in one that the cached entry doesn't hold searches)",
        n, tot / 1048576.0, hinted / 1048576.0, tot ? 100.0 * hinted / tot : 0.0, none);
}
static int gm_seg_cmp(const void *x, const void *y) { uint64_t a = *(const uint64_t *)x, b = *(const uint64_t *)y; return a < b ? -1 : a > b; }
static int gm_table(void) {
    uint64_t gen = atomic_load(&g_gm_gen);
    if (g_gt.b && g_gt.gen == gen) return 1;
    gm_tab t = {0};
    pthread_mutex_lock(&g_cmu); uint64_t done = g_seq_done; pthread_mutex_unlock(&g_cmu);
    for (int i = 0; i < g_ngt_free; i++) if (g_gt_free[i].seq <= done) { t = g_gt_free[i]; g_gt_free[i] = g_gt_free[--g_ngt_free]; break; }
    if (!t.b && !gm_tab_new(&t)) return 0;
    static uint64_t (*seg)[3]; static int cseg; int n = 0, over = 0;   // the pieces as the table's ranges, then laid out (gmem_table.h)
    pthread_mutex_lock(&g_gm_mu);
    t.gen = atomic_load(&g_gm_gen);
    if (g_ngm > cseg) { cseg = g_ngm + 256; seg = realloc(seg, (size_t)cseg * sizeof *seg); if (!seg) die("raw global memory: the table's ranges"); }
    for (int i = 0; i < g_ngm; i++) { if (g_gm_np && !g_gm[i].slot) continue;   // without physical pointers: the slot in the high word, the offset in the low one (gfx_vk_gmem_noptr.inc)
        seg[n][0] = g_gm[i].lo; seg[n][1] = g_gm[i].hi; seg[n++][2] = g_gm_np ? ((uint64_t)g_gm[i].slot << 32) - g_gm[i].lo : g_gm[i].gpu - g_gm[i].lo; }
    pthread_mutex_unlock(&g_gm_mu);
    { int n0 = n; n = gm_sink_ranges(&seg, &cseg, n); if (n > n0) qsort(seg, (size_t)n, sizeof *seg, gm_seg_cmp); }   // DM_VK_RAW_GMEM_SINK (gfx_vk_gmem_lazy.inc)
    n = gmem_table_fill(t.p, (const uint64_t (*)[3])seg, n, &over);
    if (over) RED_ONCE(163, "raw global memory: %d more placed pieces than the %d entries of the pool table: addresses in them are not reached", over, DMSH_GMEM_MAP_ENTRIES - 1);
    if (g_gt.b) { g_gt.seq = C ? C->seq : g_seq;   // the chunk recorded now may read it
        if (g_ngt_free < 8) g_gt_free[g_ngt_free++] = g_gt; else { dead_add(5, (uint64_t)g_gt.b); dead_add(4, (uint64_t)g_gt.m); } }
    g_gt = t; g_gm_tabs++;
    { static int said; if (!said++) { LOG("[vk] raw global memory: pool table of %d placed pieces, built again whenever a pool is placed or given back", n - 1); gm_selftest(); } }
    gm_hint_stat(seg, n - 1);
    return 1;
}
// A raw-memory binding of shader s (cb_offset 0 the table, DMSH_GMEM_LOG_CB_OFFSET the write log): the buffer, offset and range to bind
static int gm_bind(int cb_off, VkBuffer *b, VkDeviceSize *off, VkDeviceSize *range) {
    if (!gm_table()) return 0;
    if (cb_off != DMSH_GMEM_LOG_CB_OFFSET) { *b = g_gt.b; *off = 0; *range = GM_TAB_BYTES; g_gm_draws++; g_gm_rd_seq = C->seq; return 1; }
    uint8_t *d = ar_alloc(GM_LOG_BYTES, b, off); if (!d) return 0;
    memset(d, 0, GM_LOG_BYTES); *range = GM_LOG_BYTES;
    if (C->ngml == C->cgml) { C->cgml = C->cgml ? 2 * C->cgml : 16; C->gml = realloc(C->gml, (size_t)C->cgml * sizeof *C->gml); if (!C->gml) die("raw global memory: the write log list"); }
    C->gml[C->ngml++] = (gm_logref){C->ar_last, *off, g_gt.p};   // the table this draw searches, whose entries the bits name (alive until this chunk completes: dead_add)
    C->gm_st = 1; g_gm_stdraws++;
    return 1;
}
// The chunk's write logs, when it completes (chunk_done, before its dead objects go): the pools the stores reached, the first ones logged
static void gm_chunk_done(chunk *c) {
    static uintptr_t seen[GM_SEEN][2]; static int nseen;
    for (int i = 0; i < c->ngml; i++) {
        const uint32_t *w = (const uint32_t *)(c->ar[c->gml[i].blk].p + c->gml[i].off); const uint8_t *t = c->gml[i].tab; int any = 0;
        for (int j = 0; j < GM_LOG_BYTES / 4; j++) for (uint32_t bits = w[j]; bits; bits &= bits - 1) {
            uint64_t e[3]; memcpy(e, t + (size_t)(j * 32 + __builtin_ctz(bits)) * DMSH_GMEM_MAP_ENTRY, sizeof e); any = 1; g_gm_logbits++;
            if (gm_sink_is(e)) { static int ns; if (atomic_fetch_add(&g_sink_hits, 1) < 1 || ns < 16) { ns++; LOG("[RED] [vk] raw global memory: raw stores reached %#llx-%#llx, a chunk not placed: they went to the sink (DM_VK_RAW_GMEM_SINK)", (unsigned long long)e[0], (unsigned long long)e[1]); } continue; }
            int known = 0; for (int k = 0; k < nseen && !known; k++) known = seen[k][0] == e[0] && seen[k][1] == e[1];
            if (!known && nseen < GM_SEEN) { seen[nseen][0] = (uintptr_t)e[0]; seen[nseen++][1] = (uintptr_t)e[1];
                if (nseen <= 16) LOG("[vk] raw global memory: stores reached %#llx-%#llx (%llu KiB), pool range %d seen so far", (unsigned long long)e[0], (unsigned long long)e[1], (unsigned long long)((e[1] - e[0]) >> 10), nseen); }
        }
        g_gm_logs += any;
    }
    c->ngml = 0; c->gm_st = 0; c->ip_wr = 0;
}
// Before a draw or dispatch whose shaders bind the table (draw_view): the GPU reads guest memory when it runs, not when the replay copies, so a write-back still pending (a storage
// buffer an earlier dispatch wrote in its arena copy) is not in guest memory for it yet. DM_VK_RAW_GMEM_SYNC=1 lands every pending write-back first (as cpu_view does for a CPU
// read); by default such draws are only counted
static int gm_sync(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_RAW_GMEM_SYNC"); v = e && *e == '1'; if (v) LOG("[vk] DM_VK_RAW_GMEM_SYNC=1: pending write-backs land before every draw that binds the raw pool table"); } return v; }
static int gm_view(vshader *const sh[4]) {   // 1: everything pending landed (cpu_view)
    if (!gm_on()) return 0;
    int any = 0; for (int k = 0; k < 4; k++) any |= sh[k] && sh[k]->gm;
    if (!any) return 0;
    if (!(C && (C->nwb || C->ncop)) && !atomic_load(&g_inflight_wb) && !atomic_load(&g_inflight_cop)) return 0;   // (large copies too: the completion thread makes them after the chunk)
    if (!gm_sync()) { g_gm_unsynced++; return 0; }
    g_gm_synced++; cpu_view(); return 1;
}
static void gm_ip_stat(void); static void gm_lazy_stat(void);   // gfx_vk_gmem_inplace.inc, gfx_vk_gmem_lazy.inc
static void gm_stat(void) {
    if (!gm_on() && !g_ngm) return;
    gm_ip_stat(); gm_lazy_stat(); gm_np_stat();
    {   uint64_t by[4] = {0}, nb[4] = {0}; const pool_rng *big[8] = {0};   // the pools by the addresses the game took in them (mtl_pool_addressed), and the largest
        pthread_mutex_lock(&g_pool_mu);
        for (int i = 0; i < g_npool; i++) { const pool_rng *p = &g_pools[i]; int c = p->how & 1 ? 1 : p->how & 2 ? 2 : 0; by[c] += (uint64_t)(p->hi - p->lo); nb[c]++;
            for (int k = 0; k < 8; k++) if (!big[k] || p->hi - p->lo > big[k]->hi - big[k]->lo) { memmove(big + k + 1, big + k, (size_t)(7 - k) * sizeof *big); big[k] = p; break; } }
        char t[400]; int q = 0; for (int k = 0; k < 8 && big[k]; k++) q += snprintf(t + q, sizeof t - (size_t)q, " %.0f%c", (big[k]->hi - big[k]->lo) / 1048576.0, big[k]->how & 1 ? 'b' : big[k]->how & 2 ? 't' : '-');
        pthread_mutex_unlock(&g_pool_mu);
        LOG("[vk] raw global memory: memory pools by the addresses taken in them: a buffer's %llu (%.1f MiB), only a texture's %llu (%.1f MiB), none %llu (%.1f MiB); the largest (MiB, b/t/-):%s",
            (unsigned long long)nb[1], by[1] / 1048576.0, (unsigned long long)nb[2], by[2] / 1048576.0, (unsigned long long)nb[0], by[0] / 1048576.0, t); }
    LOG("[vk] raw global memory: %d placed pieces, %.1f MiB (peak %.1f; %.1f MiB of the pools were resident when placed), placing took %.0f ms, giving back %.0f ms (%llu given back, %llu kept placed); pools not placed: %llu over DM_VK_RAW_GMEM_MAX, %llu overlapping, "
        "%llu failed, %llu shader-code pools left out; %llu swaps waited for the GPU; %llu tables built (%llu buffers made); %llu bindings of the table, %llu of a write log (%llu logs with stores, %llu entry bits, %llu CPU reads that met their chunk); draws that met pending write-backs: %llu as they were, %llu landed first (cumulative)",
        g_ngm, g_gm_bytes / 1048576.0, g_gm_peak / 1048576.0, g_gm_resident / 1048576.0, g_gm_place_ns / 1e6, g_gm_give_ns / 1e6, (unsigned long long)g_gm_given, (unsigned long long)g_gm_kept, (unsigned long long)g_gm_capped,
        (unsigned long long)g_gm_overlap, (unsigned long long)g_gm_failed, (unsigned long long)g_gm_code_skipped, (unsigned long long)g_gm_waits, (unsigned long long)g_gm_tabs, (unsigned long long)g_gm_tab_new, (unsigned long long)g_gm_draws, (unsigned long long)g_gm_stdraws, (unsigned long long)g_gm_logs,
        (unsigned long long)g_gm_logbits, (unsigned long long)g_gm_store_hits, (unsigned long long)g_gm_unsynced, (unsigned long long)g_gm_synced);
}
#include "gfx_vk_gmem_noptr.inc"   // the pools through a descriptor array, for drivers that refuse OpConvertUToPtr (DM_VK_RAW_GMEM_NOPTR)
#include "gfx_vk_gmem_check.inc"   // DM_VK_RAW_GMEM_TEST and DM_VK_RAW_GMEM_CHECK: the self-test on the GPU and the pointers the shaders read
#include "gfx_vk_gmem_lazy.inc"   // large pools placed in chunks as they are used (DM_VK_RAW_GMEM_LAZY)
#include "gfx_vk_gmem_inplace.inc"   // storage buffers bound in their placed pools (DM_VK_INPLACE)
