// NVN replay on Vulkan for Android: the interface of nvn_mtl.m (nvn_mtl.h, nvn_obj.h) on a Vulkan device.
// Stage 1 (this file): textures, copies, clears, CPU reads and presents. Submissions run synchronously: the replay records a command buffer and
// submits it at commit (or earlier, when the CPU needs what the GPU wrote), waits for it, then makes the CPU-visible effects in command order, as
// nvn_mtl.m's completion handlers do (events, counter reports, fences). Draws and dispatches are not rendered yet (no shader translation for Vulkan):
// they are counted and reported once.
//   Memory: Vulkan cannot address guest memory on GPUs without VK_EXT_external_memory_host (the Adreno 6xx), so pool data goes through host-visible
//     staging buffers: copied in when a command reads it (at replay time, which in this synchronous model is the GPU's time), and written back after the
//     submission completes when a command writes it (texture-to-buffer copies). A CPU operation of the replay (buffer copies, fills, uniform updates)
//     first waits for those write-backs.
//   Textures: one VkImage per NVN texture serial, always in VK_IMAGE_LAYOUT_GENERAL, zeroed at creation (fresh Switch memory reads as 0). Textures whose
//     content lives in guest memory (packaged layouts, CPU writes) are deswizzled on the CPU and uploaded, their pages write-protected (nvn_dirty.c), and
//     uploaded again on their next use after a write, unless only the pages they share with other data were written and their own bytes there are
//     unchanged (nvn_mtl.m pkg_edges_clean). BC textures are decoded on the CPU (dm/third_party/bcdec) when the GPU has no BC support.
#include <sys/stat.h>
#include <sys/resource.h>
#include <unistd.h>
#include <fcntl.h>
#include "dm.h"
#include "nvn_obj.h"
#include "nvn_mtl.h"
#include "nvn_timestamp.h"
#include "nvn_hmap.h"
#include "nvn_dirty.h"
#include "nvn_tex.h"
#include "nvn_deswizzle.h"
#include "nvn_fmt_mtl.h"
#include "gfx_vk.h"
#include "spv_readonly.h"
#include "spv_alpha.h"
#include "spv_limits.h"
#include "spv_layer.h"   // layer and viewport output made ordinary outputs on a GPU that has none (gfx_vk_draw.inc spv_fit_layer)
#include "spv_vsinit.h"   // vertex shader outputs start as (0,0,0,1), as on the hardware (gfx_vk_draw.inc spv_fit_vsinit)
#include "spv_gmcache.h"   // raw global memory helpers keep their last entry's range and delta in private words (gfx_vk_draw.inc spv_fit_gmcache)
#include "pk_guard.h"   // the start-up list's marker and skip list (gfx_vk_draw.inc pk_guard)
#include "spv_pool.h"
#include "spv_harr.h"   // descriptor arrays longer than stage_set writes shortened to it (gfx_vk_draw.inc spv_fit_harr)
#include "spv_wide.h"
#include "spv_e5.h"   // E5B9G9R9 storage images through R32_UINT views, RGB9E5 packed by the shader (vt_storage_up, gfx_vk_draw.inc cpipe_e5)
#include "ds_texels.h"   // depth / stencil readbacks rebuilt in NVN's layouts (mtl_read_texels, copy_t2b)
#include "rb16.h"   // NVN's RGB-order 16-bit formats held in Vulkan's order, converted where they cross guest memory (vt_create's rbx)
#include "spv_narrow.h"   // each subgroup of a GPU narrower than 32 lanes a warp of its own (gfx_vk_draw.inc spv_fit_narrow)
#include "spv_xor.h"   // Maxwell's butterfly shuffle as ShuffleXor, on the drivers whose Shuffle by the lane ^ a mask is wrong (gfx_vk_draw.inc spv_fit_xor)
#include "spv_tgspill.h"   // compute shared memory past the GPU's limit from a storage buffer (gfx_vk_draw.inc spv_fit_tgs)
#include "spv_noptr.h"   // raw global memory through a descriptor array, for drivers that refuse OpConvertUToPtr (gfx_vk_gmem_noptr.inc)
#include "spv_precise.h"   // Maxwell's FMUL and FADD unfused in the shaders that place vertices (gfx_vk_draw.inc spv_fit_precise)
#include "spv_bind.h"   // a set's far-apart binding numbers made dense, for drivers that size a layout by its highest (gfx_vk_draw.inc sh_get, gfx_vk_lmod.inc lm_code)
#include "spv_tffrag.h"   // transform feedback captured in the fragment stage on a GPU without vertex-stage storage writes (gfx_vk_draw.inc tf_capture)
#include "wb_alias.h"   // a binding inside another binding's write-back of the same draw or dispatch (gfx_vk_draw.inc own_alias)
#include "rec_desc.h"   // a descriptor set's writes packed for the recorder (gfx_vk_rec.inc R_UPDDS)
#include "sb_reuse.h"   // a read-only storage buffer's copy bound again while its pages are unwritten (gfx_vk_sbcopy.inc)
#include "apipe_rule.h"   // which draws are left out while their new pipeline builds in the background (gfx_vk_draw.inc apipe_ok)
#include "wb_promote.h"   // whether a read of a draw's pending storage buffer write takes its copy on the GPU after a barrier (gfx_vk_draw.inc wb_promote)
#include "tex_aspect.h"   // which aspect of its image a texture slot reads: stencil mode (gfx_vk_draw.inc tex_aspect)
#include "tbuf_rows.h"   // a buffer texture read through a 2D slot as rows of 16384 texels (vt_create, vt_upload_, gfx_vk_draw.inc tex_rows_slot)
#include "rd_home.h"   // the home ranges of storage buffers draws under rasterizer discard write (gfx_vk_draw.inc rd_bind_w)
#include "stream_clip.h"   // where an indexed draw's stream is looked up to among forwardable write-backs (gfx_vk_draw.inc stream_clip)
#include "nvn_fstep.h"   // FSTEP_FIT: the performance hint's target (hint_frame)
#include "nvn_queue.h"   // the queue scheduler (hle_nvn.c nvn_queue.inc): event publication and GPU progress
#include <stdatomic.h>
#include <math.h>
#define BCDEC_IMPLEMENTATION
#define BCDEC_STATIC
#define BCDEC_BC4BC5_PRECISE
#include "../third_party/bcdec/bcdec.h"

static int vk_debug(void) { static int v = -1; if (v < 0) v = getenv("DM_VK_DEBUG") != NULL; return v; }   // DM_VK_DEBUG=1: a log line per upload and texture-to-buffer copy
#define RED_ONCE(id, ...) do { static int once_; if (!once_) { once_ = 1; LOG("[RED] [vk] " __VA_ARGS__); } } while (0)   // a flag: a counter (!once_++) overflowed on a path taken every draw
static pthread_mutex_t g_bk = PTHREAD_MUTEX_INITIALIZER;   // the replay is one thread's at a time: submissions, CPU reads, texture lifetimes
static __thread int t_bk_sub;   // this thread holds g_bk for a submission (mtl_sub_begin to mtl_sub_commit): see BK_LOCK

// ---------------- The device ----------------
static int g_driver_id;   // VkPhysicalDeviceDriverProperties driverID, 0 when not known (the start record, vk_start.c)
static int g_qcom_driver;   // Qualcomm's own Vulkan driver, not another on an Adreno such as Turnip (VkPhysicalDeviceDriverProperties driverID; vk_init)
static uint32_t g_sg_stages = VK_SHADER_STAGE_ALL;   // the stages that run subgroup operations (subgroupSupportedStages; spv_limits.h); all when not known
static int g_layer_out = 1;   // vertex shaders may write the layer and viewport index: VK_EXT_shader_viewport_index_layer or Vulkan 1.2 (spv_layer.h)
static int g_vs_layers;   // a vertex shader's layer reaches only layers below this (Arm's Mali driver: 8; gfx_vk_draw.inc vs_layer_groups), 0: all of them
static uint32_t g_sg_size = 32;   // the subgroup size (subgroupSize): over 32, Maxwell's warps are 32-lane parts of a subgroup (spv_wide.h)
static VkPhysicalDeviceFeatures g_vk_feat; static int g_divisor;   // what the device was created with; VK_EXT_vertex_attribute_divisor or the KHR one   // what the device was created with
// The KHR divisor without supportsNonZeroFirstInstance (the Pixel 9 Pro XL's Mali-G715): a draw with a stream stepping per N > 1 instances must start at instance 0, so its
// base instance moves into the instance-rate streams' offsets instead (gfx_vk_draw.inc inst_shift)
static int g_div_first0;
// NVN2 task and mesh shaders (gfx_vk_mesh.inc): VK_EXT_mesh_shader with its task stage, the draw calls, and the most workgroups a draw's X may launch (task, mesh)
static int g_mesh; static PFN_vkCmdDrawMeshTasksEXT g_draw_mesh; static uint32_t g_mesh_max[2];
static int memu_on(void);   // gfx_vk_meshemu.inc: their draws in compute form without it
static int g_fmt_list;   // VK_KHR_image_format_list enabled (vt_create)
static void pb_dump(void);   // gfx_vk_draw.inc: the pipelines being built, for a crash dump
static int g_stencil_export;   // VK_EXT_shader_stencil_export enabled: a fragment shader writes the stencil test's value (gfx_vk_dscopy.inc)
static int g_nonuni, g_null_desc;   // for the texture pool by ID (gfx_vk_pool.inc): VK_EXT_descriptor_indexing's non-uniform sampled image indexing; VK_EXT_robustness2's null descriptors
static int g_pool_texn;   // the IDs of the game's texture pools in the host table (mtl_set_tex_pool_n, hle_nvn.c): the pool set's image count (gfx_vk_pool.inc)
static vkb_ctx g_vk; static pthread_mutex_t g_vk_mu = PTHREAD_MUTEX_INITIALIZER; static _Atomic int g_vk_made; static int g_vk_ok;   // vkb; vkb_fallback makes it again
#include "gfx_vk_floor.inc"   // DM_VK_GPU_FLOOR: a GPU clock floor through KGSL (the launcher's Keep the GPU clock up, off by default)
#include "gfx_vk_rec.inc"   // the vkCmd* calls below go through its wrappers (DM_VK_RECORDER)
// Query resets (the chunks' query pools, "Query resets" below). DM_VK_QRESET_USED=0 (env.txt): every pool is reset whole in the command buffer at every chunk begin, as before
// (no host resets either); DM_VK_HOST_QRESET=0: VK_EXT_host_query_reset is not enabled, and the used queries are reset in the command buffer at the chunk's next begin
static int qr_used(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_QRESET_USED"); v = !(e && *e == '0'); } return v; }
static int qr_host_env(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_HOST_QRESET"); v = !(e && *e == '0') && qr_used(); } return v; }
static PFN_vkResetQueryPoolEXT g_qreset;   // vkResetQueryPoolEXT, through vkGetDeviceProcAddr (the instance is Vulkan 1.1, and the android-29 libvulkan.so stub has no vkResetQueryPool); NULL: no host resets
static PFN_vkGetShaderModuleCreateInfoIdentifierEXT g_smid;   // VK_EXT_shader_module_identifier enabled: modules made only for pipelines the driver's cache lacks (gfx_vk_lmod.inc); NULL: made at once
static PFN_vkGetDeviceFaultInfoEXT g_fault_info;   // VK_EXT_device_fault enabled: what the driver says of a lost device (gpu_fault_log); NULL: not offered
static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name) { for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return 1; return 0; }
#include "gfx_vk_memreport.inc"   // DM_VK_MEMREPORT: the driver's memory reports, by object type
static int gm_want(const VkExtensionProperties *e, uint32_t n); static void *gm_feat_query(void *next); static int gm_feat_take(const char **dext, uint32_t *nde);   // raw global memory's
static void *gm_feat_enable(void *next); static void gm_device_made(void);   // device part (gfx_vk_gmem.inc, DM_VK_RAW_GMEM)
static int g_gm_np;   // raw global memory without physical pointers (gfx_vk_gmem_noptr.inc): the pools' descriptor array, and modules rewritten by spv_noptr
static int gm_np_take(const VkPhysicalDeviceDescriptorIndexingFeaturesEXT *d, int listed, const char **dext, uint32_t *nde); static int gm_np_enable(VkPhysicalDeviceDescriptorIndexingFeaturesEXT *d);
static size_t gm_np_code(const uint32_t *code, size_t nw, const char *what, int first, uint32_t **out);
// A step of vk_try that failed: why, for its [RED] line (or the fallback's). forced: DM_VK_DRIVER_FAIL made it fail
static int vk_failed(char *why, size_t n, const char *what, const char *call, VkResult r, int forced) {
    if (forced) snprintf(why, n, "%s (DM_VK_DRIVER_FAIL)", what);
    else if (call) snprintf(why, n, "%s (%s: %s)", what, call, vk_result_str(r));
    else snprintf(why, n, "%s", what);
    return 0;
}
// One try at the device, on the driver vk_driver_load opened: 1, or 0 with why (vk_create destroys the instance it made)
static int vk_try(char *why, size_t nwhy) {
    // what a try whose driver then failed may have left: values, and a feature chain into its stack
    g_sg_stages = VK_SHADER_STAGE_ALL; g_sg_size = 32; g_div_first0 = 0; g_mr_feat = (VkPhysicalDeviceDeviceMemoryReportFeaturesEXT){.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_MEMORY_REPORT_FEATURES_EXT};
    uint32_t ni = 0; vkEnumerateInstanceExtensionProperties(NULL, &ni, NULL);
    VkExtensionProperties *ie = calloc(ni ? ni : 1, sizeof *ie); vkEnumerateInstanceExtensionProperties(NULL, &ni, ie);
    const char *iext[2]; uint32_t nie = 0;
    if (has_ext(ie, ni, "VK_KHR_surface") && has_ext(ie, ni, "VK_KHR_android_surface")) { iext[nie++] = "VK_KHR_surface"; iext[nie++] = "VK_KHR_android_surface"; g_vk.has_surface = 1; }
    free(ie);
    VkApplicationInfo ai = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "nxnative", .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ic = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai, .enabledExtensionCount = nie, .ppEnabledExtensionNames = iext};
    int forced = vk_driver_fail("instance");
    VkResult r = forced ? VK_ERROR_INITIALIZATION_FAILED : vkCreateInstance(&ic, NULL, &g_vk.inst);
    if (r != VK_SUCCESS) { g_vk.inst = VK_NULL_HANDLE; return vk_failed(why, nwhy, "no Vulkan instance", "vkCreateInstance", r, forced); }
    vk_driver_instance(g_vk.inst);
    uint32_t n = 1; r = vkEnumeratePhysicalDevices(g_vk.inst, &n, &g_vk.pd);
    if ((forced = vk_driver_fail("gpu")) || (r != VK_SUCCESS && r != VK_INCOMPLETE) || !n)
        return vk_failed(why, nwhy, "no GPU", r != VK_SUCCESS && r != VK_INCOMPLETE ? "vkEnumeratePhysicalDevices" : NULL, r, forced);
    vkGetPhysicalDeviceProperties(g_vk.pd, &g_vk.props); vkGetPhysicalDeviceMemoryProperties(g_vk.pd, &g_vk.mp);
    if (VK_API_VERSION_MINOR(g_vk.props.apiVersion) >= 1 || VK_API_VERSION_MAJOR(g_vk.props.apiVersion) > 1) {
        VkPhysicalDeviceSubgroupProperties sg = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
        VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &sg};
        vkGetPhysicalDeviceProperties2(g_vk.pd, &p2); g_sg_stages = sg.supportedStages; g_sg_size = sg.subgroupSize;
    }
    VkPhysicalDeviceFeatures f; vkGetPhysicalDeviceFeatures(g_vk.pd, &f);
    g_vk.has_bc = f.textureCompressionBC; g_vk.has_astc = f.textureCompressionASTC_LDR;
    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(g_vk.pd, &nq, NULL);
    VkQueueFamilyProperties qp[16]; if (nq > 16) nq = 16; vkGetPhysicalDeviceQueueFamilyProperties(g_vk.pd, &nq, qp);
    for (g_vk.qf = 0; g_vk.qf < nq && !(qp[g_vk.qf].queueFlags & VK_QUEUE_GRAPHICS_BIT); g_vk.qf++) {}
    if ((forced = vk_driver_fail("queue")) || g_vk.qf == nq) return vk_failed(why, nwhy, "no graphics queue", NULL, VK_SUCCESS, forced);
    uint32_t nd = 0; vkEnumerateDeviceExtensionProperties(g_vk.pd, NULL, &nd, NULL);
    VkExtensionProperties *de = calloc(nd ? nd : 1, sizeof *de); vkEnumerateDeviceExtensionProperties(g_vk.pd, NULL, &nd, de);
    // Which driver runs the Adreno: Qualcomm's, or Turnip (libadrenotools loads one with DM_VK_DRIVER). Builders name Turnip's device differently ("Turnip Adreno (TM) 650",
    // "PurpleVK-public Adreno (TM) 650", Mr. Purple's T30 "Adreno (TM) 840 (git-62ac221a33)/unknown"), so the driver ID decides: VkPhysicalDeviceDriverProperties, core in 1.2
    // and VK_KHR_driver_properties before. Without either, Qualcomm's driverVersion has its top bit set (0x80267062 on an Adreno 730, 0x8032004a on an 830) and Mesa's does not
    { VkDriverId id = 0;
      if (has_ext(de, nd, "VK_KHR_driver_properties") || VK_API_VERSION_MINOR(g_vk.props.apiVersion) >= 2 || VK_API_VERSION_MAJOR(g_vk.props.apiVersion) > 1) {
          VkPhysicalDeviceDriverProperties dp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
          VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &dp}; vkGetPhysicalDeviceProperties2(g_vk.pd, &p2); id = dp.driverID;
          LOG("[vk] driver ID %d: %s, %s", (int)id, dp.driverName, dp.driverInfo); }
      g_driver_id = (int)id;
      g_qcom_driver = id ? id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY : g_vk.props.vendorID == 0x5143 && (g_vk.props.driverVersion & 0x80000000u); }
    const char *dext[19]; uint32_t nde = 0;   // every extension below at once: swapchain, divisor, stencil export, format list, memory report, host query reset, device fault, descriptor indexing, robustness2,
                                              // shader module identifier, pipeline creation cache control, display timing and the three of task / mesh shaders; raw global memory's (gm_feat_take, gm_np_take) take what of the 19 is left
    if (has_ext(de, nd, "VK_KHR_swapchain")) { dext[nde++] = "VK_KHR_swapchain"; g_vk.has_swapchain = 1; }
    // A DM_VK_DRIVER driver must offer what the window needs (front_android.c vk_init refuses a device without it); the system's driver goes on without, as before
    if (vk_driver_name() && ((forced = vk_driver_fail("extensions")) || !g_vk.has_surface || !g_vk.has_swapchain)) {
        free(de); return vk_failed(why, nwhy, forced ? "no window extensions" : !g_vk.has_surface ? "no VK_KHR_android_surface" : "no VK_KHR_swapchain", NULL, VK_SUCCESS, forced); }
    { const char *e = getenv("DM_VK_PRESENT_TIMING");   // frames start on the display's vsyncs and are shown at a time given with them (gfx_vk_vsync.inc); Android's loader implements it on every driver
      if (g_vk.has_swapchain && has_ext(de, nd, "VK_GOOGLE_display_timing") && !(e && *e == '0')) { dext[nde++] = "VK_GOOGLE_display_timing"; g_vk.has_display_timing = 1; } }
    // NVN vertex streams step per N instances (gfx_vk_draw.inc pipe_get). The KHR extension (core in Vulkan 1.4) has the EXT's structures and type values: a device that
    // offers only it (the Mali-G715) read every such stream per instance before. Its property supportsNonZeroFirstInstance says whether a draw may start past instance 0
    int divisor = has_ext(de, nd, "VK_EXT_vertex_attribute_divisor"), div_khr = !divisor && has_ext(de, nd, "VK_KHR_vertex_attribute_divisor");
    if (divisor) dext[nde++] = "VK_EXT_vertex_attribute_divisor";
    else if (div_khr) { dext[nde++] = "VK_KHR_vertex_attribute_divisor"; divisor = 1;
        VkPhysicalDeviceVertexAttributeDivisorPropertiesKHR dvp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &dvp}; vkGetPhysicalDeviceProperties2(g_vk.pd, &p2);
        g_div_first0 = !dvp.supportsNonZeroFirstInstance; }
    // depth-stencil copies drawn in one pass (gfx_vk_dscopy.inc), else a draw per stencil bit; DM_VK_STENCIL_EXPORT=0 (env.txt, a diagnostic): the per-bit draws anyway
    { const char *e = getenv("DM_VK_STENCIL_EXPORT"); g_stencil_export = has_ext(de, nd, "VK_EXT_shader_stencil_export") && !(e && *e == '0'); }
    if (g_stencil_export) dext[nde++] = "VK_EXT_shader_stencil_export";
    // colour images name the formats they are viewed in (vt_create: Adreno's framebuffer compression); core only from Vulkan 1.2, and the instance asks for 1.1
    if ((g_fmt_list = has_ext(de, nd, "VK_KHR_image_format_list"))) dext[nde++] = "VK_KHR_image_format_list";
    // A 1.2 device's shaderOutputLayer isn't asked (the instance asks for 1.1); every 1.2 or later GPU here takes the translator's layer output as it is
    g_layer_out = has_ext(de, nd, "VK_EXT_shader_viewport_index_layer") || VK_API_VERSION_MAJOR(g_vk.props.apiVersion) > 1 || VK_API_VERSION_MINOR(g_vk.props.apiVersion) >= 2;
    // Arm's Mali-G715 driver drops a primitive whose layer from a vertex shader is 8 or more (a geometry shader's reaches every layer): such draws into more layers are
    // drawn once per 8 (vs_layer_groups). DM_VK_VS_LAYERS=<n> (env.txt): that limit on any GPU; 0: none, a draw made once as before
    { const char *e = getenv("DM_VK_VS_LAYERS"); g_vs_layers = e ? atoi(e) : g_driver_id == VK_DRIVER_ID_ARM_PROPRIETARY ? 8 : 0; if (g_vs_layers < 0) g_vs_layers = 0; }
    int hqr = qr_host_env() && has_ext(de, nd, "VK_EXT_host_query_reset");   // the completion thread resets the queries a chunk used (Query resets below); core only from Vulkan 1.2
    int memreport = mr_want(de, nd);   // DM_VK_MEMREPORT (gfx_vk_memreport.inc)
    if (memreport) dext[nde++] = "VK_EXT_device_memory_report";
    int dfault = has_ext(de, nd, "VK_EXT_device_fault");   // a lost device's faults: their kind and address (gpu_fault_log)
    // NVN2's task and mesh shaders as Vulkan's (Adreno 840: Qualcomm's driver has it, Turnip and the Adreno 7xx drivers do not). Its modules are SPIR-V 1.4, which the
    // Vulkan 1.1 instance takes through VK_KHR_spirv_1_4 (and the float controls it builds on). DM_VK_MESH=0 (env.txt): the draws are skipped, as on GPUs without it
    int mesh = has_ext(de, nd, "VK_EXT_mesh_shader") && has_ext(de, nd, "VK_KHR_spirv_1_4") && has_ext(de, nd, "VK_KHR_shader_float_controls");
    // On Qualcomm's own driver the native path draws them in the compute form by default (gfx_vk_meshemu.inc): Ocarina of Time's foliage, drawn by a mesh depth prepass and an
    // EQUAL colour pass whose positions come from the same code and the same data, lands behind its own prepass depth through the Adreno 840's mesh pipeline (white tears across
    // the canopy), and not in the compute form. DM_VK_MESH=1: the native path all the same (for an A/B); 0: neither (the draws skipped where the compute form is off too)
    { const char *e = getenv("DM_VK_MESH"); if (e && *e == '0') mesh = 0;
      else if (mesh && g_qcom_driver && !(e && *e == '1')) { mesh = 0; LOG("[vk] task / mesh shaders: Qualcomm's driver has VK_EXT_mesh_shader, but its draws run in compute form (DM_VK_MESH=1: the native path)"); } }
    // the texture pool by ID (gfx_vk_pool.inc): its indices differ between the lanes of a subgroup, and an ID not made yet reads 0 whatever type it is read as. DM_VK_POOL=0: neither
    { const char *e = getenv("DM_VK_POOL"); int pool = !(e && *e == '0'); g_nonuni = pool && has_ext(de, nd, "VK_EXT_descriptor_indexing"); g_null_desc = pool && has_ext(de, nd, "VK_EXT_robustness2"); }
    // pipelines from the driver's cache by their modules' identifiers, the modules made only when it lacks one (gfx_vk_lmod.inc). DM_VK_MODULE_ID=0: modules made at once
    int smid; { const char *e = getenv("DM_VK_MODULE_ID"); smid = !(e && *e == '0') && has_ext(de, nd, "VK_EXT_shader_module_identifier") && has_ext(de, nd, "VK_EXT_pipeline_creation_cache_control"); }
    int gm = gm_want(de, nd);   // raw global memory (gfx_vk_gmem.inc): asked for only with DM_VK_RAW_GMEM=1
    int dix = has_ext(de, nd, "VK_EXT_descriptor_indexing"), gmdi = gm && (dix || VK_API_VERSION_MINOR(g_vk.props.apiVersion) >= 2 || VK_API_VERSION_MAJOR(g_vk.props.apiVersion) > 1);   // its descriptor array's features
    free(de);
    VkPhysicalDeviceFeatures want = {.textureCompressionBC = f.textureCompressionBC, .textureCompressionASTC_LDR = f.textureCompressionASTC_LDR,
                                     .textureCompressionETC2 = f.textureCompressionETC2, .independentBlend = f.independentBlend, .geometryShader = f.geometryShader,
                                     .depthClamp = f.depthClamp, .samplerAnisotropy = f.samplerAnisotropy, .fragmentStoresAndAtomics = f.fragmentStoresAndAtomics,
                                     .shaderStorageImageWriteWithoutFormat = f.shaderStorageImageWriteWithoutFormat, .shaderStorageImageReadWithoutFormat = f.shaderStorageImageReadWithoutFormat, .fillModeNonSolid = f.fillModeNonSolid, .logicOp = f.logicOp,
                                     .depthBiasClamp = f.depthBiasClamp, .dualSrcBlend = f.dualSrcBlend, .tessellationShader = f.tessellationShader, .imageCubeArray = f.imageCubeArray,
                                     .shaderClipDistance = f.shaderClipDistance, .shaderCullDistance = f.shaderCullDistance, .shaderInt16 = f.shaderInt16, .shaderInt64 = f.shaderInt64,
                                     .shaderFloat64 = f.shaderFloat64, .vertexPipelineStoresAndAtomics = f.vertexPipelineStoresAndAtomics, .sampleRateShading = f.sampleRateShading,
                                     .shaderImageGatherExtended = f.shaderImageGatherExtended, .shaderStorageImageExtendedFormats = f.shaderStorageImageExtendedFormats,
                                     .multiDrawIndirect = f.multiDrawIndirect, .largePoints = f.largePoints, .wideLines = f.wideLines, .occlusionQueryPrecise = f.occlusionQueryPrecise,
                                     .shaderSampledImageArrayDynamicIndexing = f.shaderSampledImageArrayDynamicIndexing};
    g_vk_feat = want;
    float pri = 1; VkDeviceQueueCreateInfo qc = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = g_vk.qf, .queueCount = 1, .pQueuePriorities = &pri};
    // translated shaders declare DrawParameters (Ryujinx's base instance / vertex): a Vulkan 1.1 feature of its own
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT dv = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT};
    VkPhysicalDeviceHostQueryResetFeaturesEXT hq = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES_EXT, .pNext = divisor ? &dv : NULL};
    VkPhysicalDeviceShaderDrawParametersFeatures dp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES, .pNext = hqr ? (void *)&hq : divisor ? (void *)&dv : NULL};
    VkPhysicalDeviceFaultFeaturesEXT ff = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT, .pNext = &dp};
    void *qn = dfault ? (void *)&ff : (void *)&dp;
    VkPhysicalDeviceRobustness2FeaturesEXT rbq = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT, .pNext = qn}; if (g_null_desc) qn = &rbq;
    VkPhysicalDeviceDescriptorIndexingFeaturesEXT diq = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES_EXT, .pNext = qn}; if (g_nonuni || gmdi) qn = &diq;
    VkPhysicalDevicePipelineCreationCacheControlFeaturesEXT pcq = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES_EXT, .pNext = qn};
    VkPhysicalDeviceShaderModuleIdentifierFeaturesEXT smq = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MODULE_IDENTIFIER_FEATURES_EXT, .pNext = &pcq}; if (smid) qn = &smq;
    if (gm) qn = gm_feat_query(qn);
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = qn};
    if (VK_API_VERSION_MINOR(g_vk.props.apiVersion) >= 1 || VK_API_VERSION_MAJOR(g_vk.props.apiVersion) > 1) vkGetPhysicalDeviceFeatures2(g_vk.pd, &f2);
    hqr = hqr && hq.hostQueryReset;
    if (hqr) dext[nde++] = "VK_EXT_host_query_reset";
    if ((dfault = dfault && ff.deviceFault)) dext[nde++] = "VK_EXT_device_fault";
    if ((g_nonuni = g_nonuni && diq.shaderSampledImageArrayNonUniformIndexing && f2.features.shaderSampledImageArrayDynamicIndexing)) dext[nde++] = "VK_EXT_descriptor_indexing";
    if ((g_null_desc = g_null_desc && rbq.nullDescriptor)) dext[nde++] = "VK_EXT_robustness2";
    VkPhysicalDeviceMeshShaderFeaturesEXT msf = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT};
    if (mesh) { VkPhysicalDeviceFeatures2 mf = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &msf}; vkGetPhysicalDeviceFeatures2(g_vk.pd, &mf); mesh = msf.meshShader && msf.taskShader; }
    VkPhysicalDeviceMeshShaderFeaturesEXT mse = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT, .meshShader = VK_TRUE, .taskShader = VK_TRUE};
    if (mesh) { dext[nde++] = "VK_EXT_mesh_shader"; dext[nde++] = "VK_KHR_spirv_1_4"; dext[nde++] = "VK_KHR_shader_float_controls";
        VkPhysicalDeviceMeshShaderPropertiesEXT mp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT}; VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &mp};
        vkGetPhysicalDeviceProperties2(g_vk.pd, &p2); g_mesh_max[0] = mp.maxTaskWorkGroupCount[0]; g_mesh_max[1] = mp.maxMeshWorkGroupCount[0]; }
    smid = smid && smq.shaderModuleIdentifier && pcq.pipelineCreationCacheControl;
    if (smid) { dext[nde++] = "VK_EXT_pipeline_creation_cache_control"; dext[nde++] = "VK_EXT_shader_module_identifier"; }
    if (gm) gm = gm_feat_take(dext, &nde);
    if (gm && gmdi) gm_np_take(&diq, dix, dext, &nde);   // the pools' descriptor array, for a driver that refuses OpConvertUToPtr (gfx_vk_gmem_noptr.inc)
    VkPhysicalDeviceVertexAttributeDivisorFeaturesEXT dve = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT, .vertexAttributeInstanceRateDivisor = dv.vertexAttributeInstanceRateDivisor};
    VkPhysicalDeviceHostQueryResetFeaturesEXT hqe = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES_EXT, .pNext = divisor ? &dve : NULL, .hostQueryReset = VK_TRUE};
    VkPhysicalDeviceShaderDrawParametersFeatures dpe = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES, .pNext = hqr ? (void *)&hqe : divisor ? (void *)&dve : NULL, .shaderDrawParameters = dp.shaderDrawParameters};
    g_divisor = divisor && dv.vertexAttributeInstanceRateDivisor;
    if (div_khr) LOG("[vk] vertex streams stepping per N instances: VK_KHR_vertex_attribute_divisor (%s)%s", g_div_first0 ? "a draw that has one starts at instance 0, its base instance in the stream offsets"
                                                                                                                  : "any first instance", g_divisor ? "" : ", but without vertexAttributeInstanceRateDivisor: read per instance");
    if (!dp.shaderDrawParameters) LOG("[RED] [vk] no shaderDrawParameters: translated vertex shaders declare DrawParameters");
    if (memreport) dpe.pNext = mr_chain(dpe.pNext);
    if (mesh) { mse.pNext = dpe.pNext; dpe.pNext = &mse; }
    VkPhysicalDeviceFaultFeaturesEXT ffe = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT, .pNext = &dpe, .deviceFault = VK_TRUE};
    void *en = dfault ? (void *)&ffe : (void *)&dpe;
    VkPhysicalDeviceRobustness2FeaturesEXT rbe = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT, .pNext = en, .nullDescriptor = VK_TRUE}; if (g_null_desc) en = &rbe;
    VkPhysicalDeviceDescriptorIndexingFeaturesEXT die = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES_EXT, .pNext = en, .shaderSampledImageArrayNonUniformIndexing = g_nonuni ? VK_TRUE : VK_FALSE};
    if ((gm && gm_np_enable(&die)) || g_nonuni) en = &die;
    VkPhysicalDevicePipelineCreationCacheControlFeaturesEXT pce = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_CREATION_CACHE_CONTROL_FEATURES_EXT, .pNext = en, .pipelineCreationCacheControl = VK_TRUE};
    VkPhysicalDeviceShaderModuleIdentifierFeaturesEXT sme = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MODULE_IDENTIFIER_FEATURES_EXT, .pNext = &pce, .shaderModuleIdentifier = VK_TRUE}; if (smid) en = &sme;
    if (gm) en = gm_feat_enable(en);
    VkDeviceCreateInfo dc = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = en, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qc, .enabledExtensionCount = nde, .ppEnabledExtensionNames = dext,
                             .pEnabledFeatures = &want};
    forced = vk_driver_fail("device");
    fl_before_device();
    r = forced ? VK_ERROR_INITIALIZATION_FAILED : vkCreateDevice(g_vk.pd, &dc, NULL, &g_vk.dev);
    if (r != VK_SUCCESS) { g_vk.dev = VK_NULL_HANDLE; return vk_failed(why, nwhy, "no Vulkan device", "vkCreateDevice", r, forced); }
    vkGetDeviceQueue(g_vk.dev, g_vk.qf, 0, &g_vk.q);
    fl_device_made();
    if (mesh) { g_draw_mesh = (PFN_vkCmdDrawMeshTasksEXT)vkGetDeviceProcAddr(g_vk.dev, "vkCmdDrawMeshTasksEXT");
        g_mesh = g_draw_mesh != NULL && g_mesh_max[0] && g_mesh_max[1]; }
    LOG("[vk] task / mesh shaders: %s", g_mesh ? "on (VK_EXT_mesh_shader)" : mesh ? "[RED] VK_EXT_mesh_shader enabled but vkCmdDrawMeshTasksEXT was not found: their draws are skipped" : memu_on() ? "no VK_EXT_mesh_shader (or DM_VK_MESH=0): their draws run in compute form (gfx_vk_meshemu.inc)" : "no VK_EXT_mesh_shader (or DM_VK_MESH=0), and DM_VK_MESH_EMU=0: their draws are skipped");
    if (hqr && !(g_qreset = (PFN_vkResetQueryPoolEXT)vkGetDeviceProcAddr(g_vk.dev, "vkResetQueryPoolEXT"))) LOG("[RED] [vk] VK_EXT_host_query_reset is enabled but vkResetQueryPoolEXT was not found");
    if (dfault) g_fault_info = (PFN_vkGetDeviceFaultInfoEXT)vkGetDeviceProcAddr(g_vk.dev, "vkGetDeviceFaultInfoEXT");
    if (smid && !(g_smid = (PFN_vkGetShaderModuleCreateInfoIdentifierEXT)vkGetDeviceProcAddr(g_vk.dev, "vkGetShaderModuleCreateInfoIdentifierEXT"))) LOG("[RED] [vk] VK_EXT_shader_module_identifier is enabled but vkGetShaderModuleCreateInfoIdentifierEXT was not found: modules made at once");
    LOG("[vk] shader modules: %s", g_smid ? "made only for a pipeline the driver's cache does not have, which takes their identifiers (VK_EXT_shader_module_identifier; DM_VK_MODULE_ID=0: made at once)" : "made as each shader loads");
    if (gm) gm_device_made();
    LOG("[vk] query resets: %s", g_qreset ? "on the host, the queries each chunk used, once read (VK_EXT_host_query_reset)" : qr_used() ? "in the command buffer, the queries the chunk used last time"
                                                                                                                                     : "in the command buffer, every pool at every chunk begin (DM_VK_QRESET_USED=0)");
    LOG("[vk] descriptor sets bound at once: %u, subgroup operations in stages %#x, %u lanes", g_vk.props.limits.maxBoundDescriptorSets, g_sg_stages, g_sg_size);
    { void crash_set_gfx_hook(void (*)(void)); crash_set_gfx_hook(pb_dump); }   // a crash dump lists the pipelines being built (gfx_vk_draw.inc)
    LOG("[vk] %s (Vulkan %u.%u.%u): BC %s, ASTC %s", g_vk.props.deviceName, VK_API_VERSION_MAJOR(g_vk.props.apiVersion), VK_API_VERSION_MINOR(g_vk.props.apiVersion),
        VK_API_VERSION_PATCH(g_vk.props.apiVersion), g_vk.has_bc ? "native" : "decoded on the CPU", g_vk.has_astc ? "native" : "missing");
    vk_start_device(g_vk.props.deviceName, g_driver_id);
    return 1;
}
// A DM_VK_DRIVER driver opens only if it offers a GPU (vk_driver.c), but it can still fail any step after that. Each such failure tries again on the system's driver, with a
// [RED] line saying why (vk_driver_fallback): before this, the game ran with no picture, as the front end had no device to draw with
static void vk_make(void) {
    for (;;) {
        char why[200];
        if (vk_try(why, sizeof why)) { g_vk_ok = 1; return; }
        if (!vk_driver_name()) { LOG("[RED] [vk] %s", why); return; }   // the system's driver: nothing to fall back to
        if (g_vk.inst) vkDestroyInstance(g_vk.inst, NULL);
        pthread_mutex_destroy(&g_vk.qmu); memset(&g_vk, 0, sizeof g_vk); pthread_mutex_init(&g_vk.qmu, NULL);
        if (!vk_driver_fallback(why)) return;
    }
}
// Made at the first call, under g_vk_mu rather than a pthread_once, since vkb_fallback (after gfx_vk_draw.inc) can make it once more
vkb_ctx *vkb(void) {
    if (!atomic_load_explicit(&g_vk_made, memory_order_acquire)) {
        pthread_mutex_lock(&g_vk_mu);
        if (!atomic_load_explicit(&g_vk_made, memory_order_relaxed)) {
            pthread_mutex_init(&g_vk.qmu, NULL);
            if (vk_driver_load()) vk_make();
            atomic_store_explicit(&g_vk_made, 1, memory_order_release);
        }
        pthread_mutex_unlock(&g_vk_mu);
    }
    return g_vk_ok ? &g_vk : NULL;
}
uint32_t vkb_mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < g_vk.mp.memoryTypeCount; i++) if ((bits >> i & 1) && (g_vk.mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

// ---------------- Staging buffers ----------------
typedef struct { VkBuffer b; VkDeviceMemory m; uint8_t *p; VkDeviceSize n, used; } stg_t;   // used: a pool block's bytes cut so far (stg_take)
static _Atomic uint64_t g_stg_allocs, g_stg_frees; static uint32_t g_stg_mt = UINT32_MAX;   // staging vkAllocateMemory / vkFreeMemory calls (DM_VK_STG_STAT); the memory type the last one took
static int stg_new(stg_t *s, VkDeviceSize n) {
    memset(s, 0, sizeof *s); if (!n) n = 16;
    VkBufferCreateInfo bc = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = n, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    if (vkCreateBuffer(g_vk.dev, &bc, NULL, &s->b) != VK_SUCCESS) return 0;
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g_vk.dev, s->b, &mr);
    uint32_t mt = vkb_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (mt == UINT32_MAX) mt = vkb_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo ma = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mt};
    if (mt == UINT32_MAX || vkAllocateMemory(g_vk.dev, &ma, NULL, &s->m) != VK_SUCCESS) { vkDestroyBuffer(g_vk.dev, s->b, NULL); return 0; }
    atomic_fetch_add(&g_stg_allocs, 1); g_stg_mt = mt;
    vkBindBufferMemory(g_vk.dev, s->b, s->m, 0);
    if (vkMapMemory(g_vk.dev, s->m, 0, n, 0, (void **)&s->p) != VK_SUCCESS) { vkDestroyBuffer(g_vk.dev, s->b, NULL); vkFreeMemory(g_vk.dev, s->m, NULL); atomic_fetch_add(&g_stg_frees, 1); return 0; }
    s->n = n; return 1;
}
static void stg_free(stg_t *s) { if (s->b) { vkDestroyBuffer(g_vk.dev, s->b, NULL); vkFreeMemory(g_vk.dev, s->m, NULL); atomic_fetch_add(&g_stg_frees, 1); } memset(s, 0, sizeof *s); }
#include "gfx_vk_bc.inc"   // DM_VK_BC: GPU breadcrumbs around every dispatch, read when the GPU hangs or is lost

// ---------------- Submissions: chunks ----------------
// The commands go to the GPU in chunks: a command buffer with everything the GPU reads through it (staging buffers, the draws' arena, descriptor pools, texel
// buffer views), what is copied back to guest memory once it is done (write-backs), the events and counters it signals (posts), and the objects to destroy after
// it. A chunk is submitted without waiting (a submission's end, a present, a CPU read); the completion thread waits for chunks in order and then writes back,
// posts, completes the NVN fence and recycles the chunk. Write-backs come before posts: a game waiting on an event reads what the GPU wrote.
// A write-back with a snapshot (the guest bytes when the GPU took its copy) writes only the bytes that differ from it: the CPU may have written the others since.
// Before the replay reads guest memory on the CPU (an upload, a draw's data, a buffer copy), pending write-backs land first (cpu_view).
typedef struct { int st; size_t off, row_bytes, rows, src_pitch, dst_pitch; uint8_t *dst, *snap; int fwd;   // st < 0: the chunk's arena block -1 - st; st >= SP_ST: its staging pool block st - SP_ST (stg_take); otherwise its staging buffer st; fwd: a dispatch wrote it (later draws may read it on the GPU: gfx_vk_draw.inc fwd_find)
                 // a linear render target's (gfx_vk_draw.inc lin_writeback): lin 1, 2 when its texture is one level and one layer (the write-back covers all it uploads: ab_note); dsnap: the pass's
                 // snapshot kept only for the revert check (DM_VK_LINWB_SNAP=0 with DM_VK_WB_REVERT=1); snap_cap: snap / dsnap is a pooled buffer of that size (snap_give), 0: malloc'd;
                 // the texture's serial, image, NVN format, texel block bytes, size and storage size
                 // rb: its GPU copy's offset in the chunk's readback buffer + 1 (gfx_vk_readback.inc rb_record), 0: read from its source
                 // inpl: a storage buffer the GPU writes in its placed pool (gfx_vk_gmem_inplace.inc), bound as ipb at off: nothing to copy, its pages prepared once the chunk completes
                 int lin; uint8_t *dsnap; size_t snap_cap; uint64_t ser, img; uint32_t fmt, bpb, w, h; uint64_t tsize; size_t rb; VkBuffer ipb; int inpl;
                 // a depth / stencil readback (ds_wb): rows of ds_n texels of NVN format ds_fmt rebuilt from the slice's planes at the source (ds_texels.h ds_slice: the depth
                 // plane of ds_dkind, then the stencil plane when ds_sten); ds_n 0: a plain write-back
                 uint32_t ds_n, ds_fmt; int ds_dkind, ds_sten;
                 // vd: a draw under rasterizer discard wrote it, run as a dispatch (gfx_vk_draw.inc rd_bind_w): a read it overlaps but does not cover learns a home range (fwd_hit)
                 int vd;
                 // rbx: texels of an image of NVN's RGB-order 16-bit formats (vtex rbx): exchanged into the guest's order at the source before they land (chunk_done)
                 int rbx; } wb_t;
typedef struct { VkBuffer b; VkDeviceMemory m; uint8_t *p; VkDeviceSize n, used; } arena_blk;
typedef struct { int kind; volatile uint32_t *val; uint32_t mode, v; uint64_t addr; uint32_t type; uint64_t q0, q1; } post_op;   // kind 1 event, 2 counter, 3 samples passed (occlusion queries q0..q1)
typedef struct { int kind; uint64_t h; } dead_t;
typedef struct { int blk; VkDeviceSize off; const uint8_t *tab; } gm_logref;
static int gm_store_wait(void); static uint64_t g_gm_store_hits;   // gfx_vk_gmem.inc: a chunk's raw stores count as a GPU write anywhere until it completes (DM_VK_RAW_GMEM_STORE_WAIT)   // a raw store write log in arena block blk, and the pool table its bits index (gfx_vk_gmem.inc gm_bind)
typedef struct { uint64_t src, dst, n; uint32_t value; int fill; } cop_t;   // a large buffer copy or fill, done by the completion thread in its place in the queue   // kind 1 image view, 2 framebuffer, 3 image, 4 device memory, 5 buffer
// One chunk's descriptor sets of one set layout (a shader's dsl or dsl_tf; gfx_vk_draw.inc ds_get): s[0, used) handed out while the chunk records, s[used, n) free again
typedef struct ds_list { VkDescriptorSet *s; int n, used, cap; } ds_list;
enum { CR_MAX = 1280 };   // a chunk's breadcrumbs (crumb): its passes, draws and dispatches, the last 1280 kept: a chunk of DM_VK_MID_DRAWS=512 (Breath of the Wild) with a pass each, and its
                         // dispatches (160 kept only the last 160 of such a chunk, which could leave out the draw that hung). About 40 KB a chunk
typedef struct chunk {
    VkCommandPool pool; VkCommandBuffer cb; VkFence fence; int open, busy, ts, ndraw, held; uint64_t seq, nvn_fence, hold;
    int qd, td, pd, sd;   // queries of qp / tq / pq written since their last reset, [0, n): reset in the command buffer when the chunk next begins; sd: its two g_tsq slots (Query resets below)
    VkQueryPool qp; int nq; uint64_t qg[1024];   // occlusion queries begun in it: query i counts NVN samples-passed slot qg[i] (occl_* below)
    VkQueryPool pq; int npq;   // render pass timestamps of a profiled frame (prof_* in gfx_vk_draw.inc)
    VkQueryPool tq; int ntq; uint64_t t_end;   // timestamps of the game's counter reports (kind 4 posts); the GPU time the chunk ended (g_tsq)   // hold: a later chunk reads this one's arena on the GPU (gfx_vk_draw.inc fwd_find): kept busy (held) until that one completes   // ndraw: draws and dispatches recorded (early submission: mtl_exec)   // ts: GPU timestamps written at its start and end (g_tsq)
    uint64_t gf; int gtag;   // gtag: its GPU time counts toward frame gf's (g_fgpu, mtl_gpu_ms)
    stg_t *stg; int nstg, capstg; wb_t *wb; int nwb, capwb, nfwd, nipw; post_op *post; int npost, cappost; dead_t *dead; int ndead, capdead; cop_t *cop; int ncop, capcop;
    stg_t *sb; int nsb, csb;   // the staging pool blocks it took while recorded, the last one being cut (stg_take); back to the free list in chunk_done
    arena_blk *ar; int nar, car, ar_cur, ar_last;   // gfx_vk_draw.inc
    stg_t rb;   // its arena write-backs' GPU copies, read by the completion thread (gfx_vk_readback.inc): kept and grown, used the bytes copied in its last recording
    VkDescriptorPool *dp; int ndp, cdp, dp_cur; VkBufferView *bv; int nbv, cbv;
    VkDescriptorSet *dsc; int ndsc, cdsc, dsc_used;   // the depth-stencil copies' sets (gfx_vk_dscopy.inc dsc_set): made from its pools, handed out again once it completes
    VkDescriptorSet *bcd; int nbcd, cbcd, bcd_used;   // the GPU BC decodes' sets (gfx_vk_bcdec.inc bcd_set), the same way
    VkDescriptorSet *ups; int nups, cups, ups_used;   // the upscaler replacement's sets (gfx_vk_upsq.inc upsq_set), the same way
    uint64_t rq_ticket;   // its submission's ticket with the recorder on (gfx_vk_rec.inc rec_submit), 0 when submitted by the replay
    VkResult sub_r;   // VK_SUCCESS once its fence is on the queue; else what the queue said, and the completion thread doesn't wait on the fence (submit_or_fence)
    ds_list **dl; int ndl, cdl; ds_list **dt; int ndt, cdt; int nds;   // recycled descriptor sets (gfx_vk_draw.inc ds_get): the lists holding sets, those handed out from in this recording, the sets held
    gm_logref *gml; int ngml, cgml, gm_st, ip_wr;   // its raw store write logs; gm_st: a shader with raw stores ran in it (gfx_vk_gmem.inc); ip_wr: one wrote a storage buffer in place
    struct { const char *a, *b; uint32_t n, m; char kind; } cr[CR_MAX]; int ncr;   // what it recorded, the last CR_MAX of ncr (crumb): named when the GPU hangs or is lost in it (gpu_slow)
} chunk;
// Chunks in use (g_nchunk of NCHUNK_MAX): the one being recorded and those submitted, until the completion thread is done with them. With none free, cur() waits for the
// oldest (the wait is in the GPU busy line: g_chunk_wait_ns). A submission is cut every 48-64 draws (EARLY_DRAWS, MID_DRAWS), so a game sends many a frame, and with four
// the replay could not get far ahead of what had come back: Pokémon Brilliant Diamond on a Retroid Pocket 5 (Adreno 650, Turnip), about 18 chunks a frame, waited here 10-11 ms
// of each 21 ms frame with the GPU busy 11 ms, at 47.6 fps. Six gave 52.5 fps, eight 53.3, sixteen 54.0; and Skyward Sword HD on an Odin 2 (Qualcomm's driver) held 60 fps
// only with eight. Each chunk keeps the arena blocks and descriptor sets it has used: eight cost Brilliant Diamond 68 MB more, sixteen 202 MB.
// DM_VK_CHUNKS=<2-16> (env.txt): another count (4: as before)
enum { NCHUNK_MAX = 16, NCHUNK_DEF = 8 };
static int g_nchunk = NCHUNK_DEF;   // set in chunks_init, before the first chunk is made
static chunk g_ch[NCHUNK_MAX], *C;   // C: the chunk being recorded (NULL: none yet)
static chunk *g_fifo[NCHUNK_MAX]; static int g_fifo_n;   // submitted, oldest first
// a pass ('p': its size), draw ('d', 'D' indirect: vertex and fragment shader, vertices or indices, instances) or dispatch ('c', 'C' indirect: groups) the chunk recorded now made (gpu_slow)
static void crumb(char kind, const char *a, const char *b, uint32_t n, uint32_t m) {
    if (!C) return;
    chunk *c = C; int i = c->ncr++ % CR_MAX; c->cr[i].kind = kind; c->cr[i].a = a; c->cr[i].b = b; c->cr[i].n = n; c->cr[i].m = m;
}
static _Atomic uint64_t g_chunk_wait_ns;   // the replay's time in cur() waiting for a free chunk
static pthread_mutex_t g_cmu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_ccv = PTHREAD_COND_INITIALIZER;
static uint64_t g_seq, g_seq_done; static _Atomic int g_inflight_wb, g_inflight_cop, g_inflight_fwd, g_inflight_ipw; static int g_drawing;   // g_drawing: a draw is being made (its resources are in C)
static void rp_end(void); static void views_drop(VkImage img); static void draw_chunk_reset(chunk *c); static void bf_reset(void); static int ds_recycle(void);   // gfx_vk_draw.inc
static void *chunk_thread(void *arg); static int stg_pool_on(void);
// ---------------- Occlusion queries (NVN_COUNTER_TYPE_SAMPLES_PASSED) ----------------
// NVN's samples passed is a counter on the queue: ResetCounter zeroes it, every draw after adds its samples, ReportCounter writes the sum at that point when the GPU gets
// there. Here, as nvn_mtl.m does with visibility buffers: while counting, draws are inside an occlusion query, one per render pass stretch (a query cannot outlive its
// pass), each a numbered slot; a report notes the slots since the reset and, when its chunk completes, writes their sum. The completion thread copies every chunk's query
// results into g_qres by slot first. The replay used to write 0: everything a game tests for visibility was fully hidden (nvn_mtl.m found culled lights the same way)
enum { QRES = 1 << 16 };
static uint64_t g_qres[QRES], g_qslot;   // results by slot (a ring), the next slot
static int g_q_on = 0, g_q_act = -1; static uint64_t g_q_win;   // counting since a reset; the query open in the current pass (chunk-local index; -2: a stretch counted with no query, occl_mode 0); the window's first slot
static uint64_t g_q_reports, g_q_queries, g_q_noq, g_q_resets, g_ts_reports, g_qr_cmd; static _Atomic uint64_t g_qr_host;   // g_qr_*: queries reset in command buffers / on the host (Query resets below)
// DM_VK_OCCLUSION (env.txt, or a game's android_defaults.txt): what a draw stretch between two samples-passed reports records. `precise` (the default): an occlusion query with
// VK_QUERY_CONTROL_PRECISE_BIT where the GPU offers it, as before; `any`: one without it, whose result is zero when no sample passed and otherwise any nonzero count (Vulkan 1.3
// "Occlusion Queries"), which is all a condition's equality needs; `0`: no query at all: every stretch counts as one sample passed, the report writes the number of stretches
// since the reset, and every condition on such a report keeps drawing (rc_landed_on), so the game draws what its queries would have culled. The games whose conditions the host
// evaluates compare a report with an earlier one of the same counter (render if equal / not equal, OP_RENDER_COND). Ocarina of Time resets its counter before every report, about
// 530 queries a frame on binned 1440x810 passes, where Qualcomm's guidance puts them at 20-40 % of the command processor's time. OnePlus Pad 3 (Adreno 830 at 1,100 MHz), Kokiri
// Forest: the queries themselves cost 5.25 ms of a 21.3 ms frame (24.8 ms with them and every condition drawing against 19.6 ms with neither), the culling they buy is worth
// 3.5 ms (2,100 draws a frame instead of 2,420), and a query without the precise bit costs the same as one with it on Qualcomm's driver; so that game's android_defaults.txt sets
// =0, GPU 21.3 -> 19.6 ms a frame (the reference entry Occlusion queries left out for a game whose culling costs more than it saves, rendering.md)
static int occl_mode(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("DM_VK_OCCLUSION"); v = !e ? 2 : *e == '0' ? 0 : !strcmp(e, "any") ? 1 : 2;
        if (v != 2) LOG("[vk] DM_VK_OCCLUSION=%s: %s", e, v ? "occlusion queries without the precise count" : "no occlusion queries: every draw stretch counts as one sample passed, every condition draws"); }
    return v;
}
static int chunk_make(chunk *c) {
    VkCommandPoolCreateInfo pc = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = g_vk.qf};   // one pool per chunk: reset by the completion thread
    if (vkCreateCommandPool(g_vk.dev, &pc, NULL, &c->pool) != VK_SUCCESS) return 0;
    VkCommandBufferAllocateInfo ca = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = c->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkFenceCreateInfo fc = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkQueryPoolCreateInfo qc = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_OCCLUSION, .queryCount = 1024};
    if (vkCreateQueryPool(g_vk.dev, &qc, NULL, &c->qp) != VK_SUCCESS) c->qp = VK_NULL_HANDLE;
    VkQueryPoolCreateInfo tc = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 256};
    if (!g_vk.props.limits.timestampComputeAndGraphics || vkCreateQueryPool(g_vk.dev, &tc, NULL, &c->tq) != VK_SUCCESS) c->tq = VK_NULL_HANDLE;
    tc.queryCount = 512; if (!g_vk.props.limits.timestampComputeAndGraphics || vkCreateQueryPool(g_vk.dev, &tc, NULL, &c->pq) != VK_SUCCESS) c->pq = VK_NULL_HANDLE;
    if (g_qreset) { if (c->qp) g_qreset(g_vk.dev, c->qp, 0, 1024); if (c->tq) g_qreset(g_vk.dev, c->tq, 0, 256); if (c->pq) g_qreset(g_vk.dev, c->pq, 0, 512); }   // a new query's state is undefined
    else { c->qd = 1024; c->td = 256; c->pd = 512; }   // reset whole by the first begin
    c->sd = !g_qreset;   // g_tsq: reset whole on the host in chunks_init
    return vkAllocateCommandBuffers(g_vk.dev, &ca, &c->cb) == VK_SUCCESS && vkCreateFence(g_vk.dev, &fc, NULL, &c->fence) == VK_SUCCESS;
}
// GPU time: each chunk's command buffer writes a timestamp at its start and its end; the completion thread adds up the differences (the chunks run one after
// another on the queue, so the sum is the GPU's busy time). Logged with the memory line every 300 frames (mtl_mem_stat)
static VkQueryPool g_tsq; static _Atomic uint64_t g_gpu_ns, g_gpu_chunks;
// GPU time per frame (mtl_gpu_ms), as nvn_mtl.m keeps it: a chunk counts toward the frame current when it is submitted (mtl_set_frame comes after the present that flushed the frame's
// last chunk), and a frame's time is known once all its chunks have completed. Each chunk adds the part of its span after the previous chunk's end (one queue: the union of the spans is
// the GPU's busy time, even if a TOP_OF_PIPE timestamp were written while the chunk before still ran). It is the frame step's GPU term (hle_nvn.c fstep_tick: cost = max(CPU, GPU));
// mtl_gpu_ms returned -1 here, so the cost was the CPU's alone, and once the present's grid sleep left the cost (mtl_present_slept_ns) a GPU-bound frame whose CPU part fitted k - 1
// would have started a trial the GPU cannot hold (Breath of the Wild on an Ayn Odin 2: GPU busy 19.5-20.6 ms a frame at hold 3, CPU about 43 ms). It also fills the timing line's GPU
// column and the long-frame lines. DM_VK_FRAME_GPU=0: no GPU time per frame (mtl_gpu_ms -1, as before)
enum { FGPU = 64 };   // frames kept (hle_nvn.c reads up to 8 back)
static struct { _Atomic uint64_t frame, ns; _Atomic int pending; } g_fgpu[FGPU];   // frame + 1 (0 = none), its busy time so far, its chunks not completed yet
static _Atomic uint64_t g_cur_frame; static _Atomic int g_fgpu_on = -1;
static int fgpu_on(void) {
    if (g_fgpu_on < 0) { const char *e = getenv("DM_VK_FRAME_GPU"); g_fgpu_on = !(e && !strcmp(e, "0")); if (!g_fgpu_on) LOG("[vk] DM_VK_FRAME_GPU=0: no GPU time per frame (the frame step's cost is the CPU's alone)"); }
    return g_fgpu_on;
}
static void fgpu_begin(uint64_t f) {   // under g_cmu, before the chunk goes to the completion thread; pending is set before the frame, so a reader never sees a new frame complete
    unsigned k = f % FGPU;
    if (atomic_load(&g_fgpu[k].frame) != f + 1) { atomic_store(&g_fgpu[k].pending, 1); atomic_store(&g_fgpu[k].ns, 0); atomic_store(&g_fgpu[k].frame, f + 1); }
    else atomic_fetch_add(&g_fgpu[k].pending, 1);
}
static void fgpu_done(uint64_t f, uint64_t ns) {   // under g_cmu (completion thread)
    unsigned k = f % FGPU;
    if (atomic_load(&g_fgpu[k].frame) != f + 1) return;
    atomic_fetch_add(&g_fgpu[k].ns, ns); atomic_fetch_sub(&g_fgpu[k].pending, 1);
}
static int lsnap_on(void), wb_revert_on(void), wb_absorb_on(void), up_range_on(void), wb_words_on(void);   // the write-back switches (below, before chunk_done): read before the completion thread starts
static int chunks_init(void) {
    static int ok = -1; if (ok >= 0) return ok;
    ok = 0; if (!vkb()) return 0;
    // A test of the crash guard (vk_driver.c crash_hook): the driver reads a create info at address 0. Here, at the replay's first use of the device, since the host's
    // crash handler goes in only as the host starts (main.c), after the device and the first window: a crash before it leaves the record at trying
    if (vk_driver_fail("crash")) { LOG("[vk] DM_VK_DRIVER_FAIL=crash: the driver is about to crash, on purpose"); VkSampler s; vkCreateSampler(g_vk.dev, NULL, NULL, &s); }
    if (vk_driver_fail("crash_host")) { LOG("[vk] DM_VK_DRIVER_FAIL=crash_host: the host is about to crash outside the driver, on purpose"); *(volatile int *)16 = 0; }   // crashed elsewhere
    fgpu_on(); stg_pool_on(); lsnap_on(); wb_revert_on(); wb_absorb_on(); up_range_on(); wb_words_on(); ds_recycle();   // read before the completion thread starts (sp_done reads g_sp_on, draw_chunk_reset the recycling switch)
    { const char *e = getenv("DM_VK_CHUNKS"); int n = e ? atoi(e) : 0; if (n > 0) g_nchunk = n < 2 ? 2 : n > NCHUNK_MAX ? NCHUNK_MAX : n;
      LOG("[vk] chunks in flight: up to %d%s", g_nchunk, g_nchunk != NCHUNK_DEF ? " (DM_VK_CHUNKS)" : ""); }
    if (g_vk.props.limits.timestampComputeAndGraphics) { VkQueryPoolCreateInfo qc = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2 * (uint32_t)g_nchunk};
        if (vkCreateQueryPool(g_vk.dev, &qc, NULL, &g_tsq) != VK_SUCCESS) g_tsq = VK_NULL_HANDLE; else if (g_qreset) g_qreset(g_vk.dev, g_tsq, 0, 2 * (uint32_t)g_nchunk); }
    for (int i = 0; i < g_nchunk; i++) if (!chunk_make(&g_ch[i])) { LOG("[RED] [vk] command buffers could not be made"); return 0; }
    pthread_t th; if (pthread_create(&th, NULL, chunk_thread, NULL)) return 0;
    pthread_detach(th);
    return ok = 1;
}
static chunk *cur(void) {   // the chunk being recorded: a free one is taken (waiting for the oldest to complete when all are in flight)
    if (C) return C;
    if (!chunks_init()) return NULL;
    pthread_mutex_lock(&g_cmu);
    uint64_t t0 = 0;
    for (;;) { for (int i = 0; i < g_nchunk && !C; i++) if (!g_ch[i].busy) C = &g_ch[i]; if (C) break; if (!t0) t0 = clock_gettime_nsec_np(CLOCK_MONOTONIC); pthread_cond_wait(&g_ccv, &g_cmu); }
    if (t0) atomic_fetch_add(&g_chunk_wait_ns, clock_gettime_nsec_np(CLOCK_MONOTONIC) - t0);
    C->busy = 1; C->seq = ++g_seq; C->nvn_fence = 0; C->ndraw = 0; C->ncr = 0;
    pthread_mutex_unlock(&g_cmu);
    return C;
}
// ---------------- Query resets ----------------
// A query is reset before each use. Every chunk begin used to reset all of its pools in the command buffer (g_tsq's 2 slots, qp 1024, tq 256, pq 512), and
// vkCmdResetQueryPool costs the Adreno driver about 15 ns a query plus about 1.6 us a call on the recording thread (a call per query inside, building packets): 4.8% of
// Breath of the Wild's render thread on an Ayn Odin 2, about 1.9 ms a frame at about 62 chunks a frame (simpleperf: the stripped driver function 409aee7a under cb(); qp 2.6%,
// pq 1.35%, tq 0.7%, g_tsq 0.25%), and about 111k query resets a frame for the GPU, inside the chunks' timed spans (not measured apart). The game uses about 3 occlusion
// queries a chunk, about 4 counter timestamps a frame, and pq only in a frame profiled through prof_now. Every query is used from 0 up by its own counter (nq, ntq, npq; a chunk's two
// g_tsq slots), so "every query not written since its last reset is still reset" holds once each pool is reset whole at creation, and only [0, used) needs a reset again:
//   - VK_EXT_host_query_reset (g_qreset): qr_done resets those ranges on the host, on the completion thread. Nothing is recorded in cb() at all
//   - otherwise (or DM_VK_HOST_QRESET=0): cb() resets in the command buffer only what the chunk's previous recording used (qd, td, pd, sd; whole pools once, at creation)
// Why a host reset cannot touch a query the GPU still writes or the host has not read yet: qr_done runs on vk-done after vkWaitForFences on the chunk's own fence (all of its
// commands, the queries' last writes among them, have completed) and after every read of them: g_tsq's pair in chunk_thread (it gives t_end, the kind 4 back-projection's
// reference), qp into g_qres (the samples-passed sums post_apply adds up) and tq for the kind 4 counter timestamps in chunk_done, pq in prof_collect; a skipped or failed read
// is reset too. qp, tq and pq belong to one chunk, and g_tsq's slots 2i and 2i+1 are written only by chunk i's command buffer: chunks in flight write other slots, which
// vkResetQueryPool allows (only the range reset must be idle, and vkGetQueryPoolResults / vkResetQueryPool run on vk-done alone). It runs before busy = 0 under g_cmu, and cur()
// takes a chunk only with busy == 0 under g_cmu: the reset happens before the chunk's next recording and submission. A failed fence wait leaves the ranges to cb() instead
static void qr_done(chunk *c, int fence_ok, int ts, int nq, int ntq, int npq) {   // the completion thread, before the chunk is free again: the queries it used, all read
    if (!qr_used()) { c->qd = 1024; c->td = 256; c->pd = 512; c->sd = 1; return; }
    if (g_qreset && fence_ok) {
        if (ts) g_qreset(g_vk.dev, g_tsq, 2 * (uint32_t)(c - g_ch), 2);
        if (nq) g_qreset(g_vk.dev, c->qp, 0, (uint32_t)nq);
        if (ntq) g_qreset(g_vk.dev, c->tq, 0, (uint32_t)ntq);
        if (npq) g_qreset(g_vk.dev, c->pq, 0, (uint32_t)npq);
        atomic_fetch_add(&g_qr_host, (uint64_t)(2 * !!ts + nq + ntq + npq)); return; }
    if (ts) c->sd = 1;
    if (nq > c->qd) c->qd = nq;
    if (ntq > c->td) c->td = ntq;
    if (npq > c->pd) c->pd = npq;
}
// Compute dispatches are ordered by the game's own barriers, as NVN orders them: the same rule as seedds/nxnative 547c5db6 (PR #2250, nvn_mtl.m dispatch_enc_cs, MIT), written
// anew for the Vulkan replay. vk_dispatch put a full pipeline barrier before and after every dispatch, so the GPU ran them one at a time, with a launch and a drain each:
// Ocarina of Time's Kokiri Forest frame has about 140 dispatches, 110 of them under 60 us, 5.6 ms together on an Adreno 830 at 1,100 MHz. NVN makes a dispatch's writes visible
// to later work only when the game issues nvnCommandBufferBarrier (OP_BARRIER, any bits), so a dispatch that follows another dispatch in the command buffer gets a barrier
// only when the game issued one between them (g_cs_gbar); the barrier after a run of dispatches is deferred (g_cs_chain) and recorded before the next command of any other
// kind (cb(), which every transfer, clear, upload and render pass goes through; the chunk's end has its own). A dispatch after any other command keeps the full barrier
// before it, as before: what a pass or a copy wrote is visible to it whatever the game asked. The replay's own syncs stand for the rest (a dispatch's guest memory reads
// land first: draw_view). DM_VK_CS_BARRIER=full (env.txt): a full barrier before and after every dispatch, as before (diagnostic)
static int g_cs_gbar, g_cs_chain, g_cs_in_disp; static uint64_t g_cs_chain_seq;   // a game barrier since the last dispatch; a run of dispatches open in chunk seq g_cs_chain_seq with its barrier deferred; cb() called by vk_dispatch itself
static uint64_t g_cs_free, g_cs_gbarred, g_cs_first, g_cs_closed;   // dispatches with no barrier before them / after a game barrier / after another command (the full order); runs closed by a later command
static int cs_game(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_CS_BARRIER"); v = !(e && !strcmp(e, "full")); if (!v) LOG("[vk] DM_VK_CS_BARRIER=full: a full barrier before and after every compute dispatch (diagnostic)"); } return v; }
static void cb_barrier(void);
static void cs_chain_close(void) {   // the barrier deferred after a run of dispatches, before a command of another kind
    if (!g_cs_chain) return;
    g_cs_chain = 0; if (C && C->open && C->seq == g_cs_chain_seq) { cb_barrier(); g_cs_closed++; }
}
static VkCommandBuffer cb(void) {
    if (!cur()) return VK_NULL_HANDLE;
    if (!C->open) { VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT}; vkBeginCommandBuffer(C->cb, &bi); C->open = 1;
        if (g_tsq) { uint32_t q = 2 * (uint32_t)(C - g_ch); if (C->sd) { vkCmdResetQueryPool(C->cb, g_tsq, q, 2); g_qr_cmd += 2; } C->sd = 0;
            vkCmdWriteTimestamp(C->cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_tsq, q); C->ts = 1; }
        if (C->qp && C->qd) { vkCmdResetQueryPool(C->cb, C->qp, 0, (uint32_t)C->qd); g_qr_cmd += (uint64_t)C->qd; }   // the queries the last recording used (Query resets above)
        if (C->tq && C->td) { vkCmdResetQueryPool(C->cb, C->tq, 0, (uint32_t)C->td); g_qr_cmd += (uint64_t)C->td; }
        if (C->pq && C->pd) { vkCmdResetQueryPool(C->cb, C->pq, 0, (uint32_t)C->pd); g_qr_cmd += (uint64_t)C->pd; }
        C->qd = C->td = C->pd = 0; C->nq = C->ntq = C->npq = 0; bf_reset(); }   // a new recording holds no bound state (gfx_vk_draw.inc bf_*)
    rp_end();   // transfers and clears run outside a draw's render pass (only the draw path records inside one, through C->cb)
    if (!g_cs_in_disp) cs_chain_close();
    return C->cb;
}
// Every transfer / clear is ordered after everything before it (the images stay in GENERAL; one full barrier between operations; a chunk also ends with one, which
// orders it before every later submission on the queue)
static void cb_barrier(void) {
    if (!C || !C->open) return;
    VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(C->cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}
// ---------------- Staging: slices of pooled blocks ----------------
// A transfer's staging lives until its chunk completes: an upload's bytes are read by the GPU in the chunk's command buffer, a readback's (texture-to-buffer copies,
// ReadTexels, linear write-backs) by the completion thread in chunk_done (wb_apply). Every request was a buffer of its own (stg_new: vkCreateBuffer, vkAllocateMemory,
// vkMapMemory, and vkFreeMemory in chunk_done). Breath of the Wild on an Ayn Odin 2 makes one per texture upload and one per linear write-back (about 20 linear render
// targets a frame, lin_writeback): stg_new was 1.5% of the Presentation thread, about 0.6 ms a frame, nearly all of it vkAllocateMemory in the kernel (kgsl_sharedmem_alloc,
// page zeroing, the map; 57% under vt_upload, 43% under rp_end -> lin_writeback), and the completion thread spent about 0.65 ms a frame in vkFreeMemory (kgsl_sharedmem_free,
// munmap) inside chunk_done, before the chunk's write-backs counted as landed and its NVN fence completed: every sync on them waited for those frees too.
// Now a request of up to SP_MAXREQ is a slice cut from a block of SP_BLOCK, made once, persistently mapped, in the memory type stg_new picks by its flags (HOST_VISIBLE |
// HOST_COHERENT | HOST_CACHED, so wb_apply reads cached memory; HOST_VISIBLE | HOST_COHERENT where no type has all three: no flush or invalidate either way). A chunk takes
// blocks from a free list as it is recorded and gives them back in chunk_done once its write-backs are applied: a slice is read by the GPU only in its own command buffer and
// by the CPU only in its own chunk_done (fwd_find forwards arena write-backs only, never staging), so a block is free once its chunk's fence has signalled and chunk_done ran.
// Blocks never outlive their use in another chunk's commands, and the render thread (taking) and the completion thread (giving back) meet only at the free list (g_sp_mu).
// At most SP_NBLK blocks exist (SP_KEEP); a request over SP_MAXREQ, or one made while every block is taken and the cap is reached, gets a buffer of its own as before, freed
// in chunk_done. Blocks the free list held through a whole window of SP_TRIM chunk completions (it never fell below that many) were not needed: they are freed, one a
// completion after its fence (sp_trim), so a loading burst's blocks do not stay (a window is about 4 s of Breath of the Wild at 56 chunks a frame and 20 fps).
// Blocks were 8 MB and requests over 4 MB had a buffer of their own: Skyward Sword HD's 1920x1080 RGBA8 linear target (8.3 MB, written back every frame) did, its
// vkAllocateMemory zeroing 8 MB on the render thread (13.6% of it on an Ayn Odin 2), and on the completion thread the first-touch faults of reading those pages and their free,
// 7 to 8 ms a frame before the chunk's fence: its long frames' submit+present, and so its present interval falling to 2. Blocks are now 16 MiB and a request as large as a
// block is cut from one (a 1920x1080 RGBA16F target, 16.6 MB, fits), at most 128 MiB of them: the same 8 blocks.
// A slice is handed out by value (buffer, offset, mapped pointer, the write-back's name for its buffer). stg_take returned a pointer into C->stg, which realloc moves:
// vt_upload_, copy_b2t, copy_t2b and mtl_read_texels called cb() after it, and cb() -> rp_end -> lin_writeback -> stg_take grows C->stg when a pass was open (stage_textures'
// uploads end a pass begun before), so they wrote through and copied from a freed stg_t, and copy_t2b and mtl_read_texels took its index (s - C->stg) after cb(). A slice taken
// before cb() stays valid now, in both modes: blocks and buffers never move, only the arrays naming them. Offsets: a multiple of the copies' texel block size and of 4
// (vkCmdCopyBufferToImage / vkCmdCopyImageToBuffer bufferOffset; 4 for depth and stencil), of optimalBufferCopyOffsetAlignment and nonCoherentAtomSize, and at least 64.
// DM_VK_STG_POOL=0 (env.txt): a buffer per request, as before. DM_VK_STG_STAT=1: a line every 300 frames with the staging vkAllocateMemory / vkFreeMemory calls a frame and
// the pool's counts (mtl_mem_stat)
typedef struct { VkBuffer b; VkDeviceSize off; uint8_t *p; int st; } stg_sl;   // a slice: its buffer and its offset there (bufferOffset; a write-back's wb_t.off), its mapped bytes, the write-back's wb_t.st; p NULL: none
enum { SP_BLOCK = 16u << 20, SP_MAXREQ = SP_BLOCK, SP_KEEP = 128u << 20, SP_NBLK = SP_KEEP / SP_BLOCK, SP_TRIM = 4096, SP_ST = 1 << 24 };   // SP_MAXREQ: a 1920x1080 RGBA8 or RGBA16F linear target (8.3 / 16.6 MB) and its uploads stay pooled
static pthread_mutex_t g_sp_mu = PTHREAD_MUTEX_INITIALIZER; static stg_t g_sp_free[SP_NBLK]; static int g_sp_nfree, g_sp_nblk, g_sp_low;   // under g_sp_mu: the free list, the blocks alive, the free list's least length this window
static int g_sp_on = -1; static uint64_t g_sp_slices, g_sp_big, g_sp_full, g_sp_made; static _Atomic uint64_t g_sp_trimmed;   // slices cut, requests over SP_MAXREQ, requests with the pool at its cap, blocks made / trimmed
static int stg_pool_on(void) {
    if (g_sp_on < 0) { const char *e = getenv("DM_VK_STG_POOL"); g_sp_on = !(e && *e == '0');
        if (g_sp_on) LOG("[vk] staging: slices of %u MB blocks for requests up to %u MB, at most %u MB of blocks (DM_VK_STG_POOL=0: a buffer per request)", SP_BLOCK >> 20, SP_MAXREQ >> 20, SP_KEEP >> 20);
        else LOG("[vk] DM_VK_STG_POOL=0: a staging buffer per request"); }
    return g_sp_on;
}
static int stg_stat(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_STG_STAT"); v = e && atoi(e) != 0; } return v; }
static VkDeviceSize sp_gcd(VkDeviceSize a, VkDeviceSize b) { while (b) { VkDeviceSize r = a % b; a = b; b = r; } return a; }
static VkDeviceSize stg_align(int texel) {   // a slice's offset alignment: lcm of the texel block size (0: no image copy), 4, and the device's copy and atom alignments (powers of two), at least 64
    static VkDeviceSize base; if (!base) { base = 64; VkDeviceSize o = g_vk.props.limits.optimalBufferCopyOffsetAlignment, k = g_vk.props.limits.nonCoherentAtomSize; if (o > base) base = o; if (k > base) base = k; }
    VkDeviceSize t = texel > 0 ? (VkDeviceSize)texel : 1;
    return base / sp_gcd(base, t) * t;   // base is a multiple of 4: lcm(base, t) is a multiple of lcm(t, 4)
}
static stg_t *sp_block(void) {   // a block for the current chunk: from the free list, else a new one while fewer than SP_NBLK exist; NULL: none (the request takes a buffer of its own)
    stg_t b; int got = 0, make = 0;
    pthread_mutex_lock(&g_sp_mu);
    if (g_sp_nfree) { b = g_sp_free[--g_sp_nfree]; got = 1; if (g_sp_nfree < g_sp_low) g_sp_low = g_sp_nfree; }
    else if (g_sp_nblk < SP_NBLK) { g_sp_nblk++; make = 1; }
    pthread_mutex_unlock(&g_sp_mu);
    if (make) {
        if (!(got = stg_new(&b, SP_BLOCK))) { pthread_mutex_lock(&g_sp_mu); g_sp_nblk--; pthread_mutex_unlock(&g_sp_mu); RED_ONCE(4, "staging pool block of %u bytes failed", SP_BLOCK); }
        else if (!g_sp_made++) LOG("[vk] staging: first pool block, memory type %u (flags %#x)", g_stg_mt, g_stg_mt < g_vk.mp.memoryTypeCount ? g_vk.mp.memoryTypes[g_stg_mt].propertyFlags : 0);
    }
    if (!got) return NULL;
    if (C->nsb == C->csb) { C->csb = C->csb ? 2 * C->csb : 4; C->sb = realloc(C->sb, C->csb * sizeof *C->sb); }
    b.used = 0; C->sb[C->nsb] = b; return &C->sb[C->nsb++];
}
// n bytes of staging that live until the current chunk completes; texel: the texel block size of the image copies made through it (0: none). By value: see above
static stg_sl stg_take(VkDeviceSize n, int texel) {
    stg_sl s; memset(&s, 0, sizeof s);
    if (!cur()) return s;
    if (!n) n = 16;
    if (stg_pool_on()) {
        if (n > SP_MAXREQ) g_sp_big++;
        else {
            VkDeviceSize a = stg_align(texel), o = 0; stg_t *k = C->nsb ? &C->sb[C->nsb - 1] : NULL;
            if (k) o = (k->used + a - 1) / a * a;
            if (!k || o + n > k->n) { k = sp_block(); o = 0; }   // the rest of a block too full for this request stays unused until the chunk completes
            if (k) { k->used = o + n; g_sp_slices++; s.b = k->b; s.off = o; s.p = k->p + o; s.st = SP_ST + (int)(k - C->sb); return s; }
            g_sp_full++;
        }
    }
    if (C->nstg == C->capstg) { C->capstg = C->capstg ? 2 * C->capstg : 32; C->stg = realloc(C->stg, C->capstg * sizeof *C->stg); }
    if (!stg_new(&C->stg[C->nstg], n)) { RED_ONCE(1, "staging buffer of %llu bytes failed", (unsigned long long)n); return s; }
    s.b = C->stg[C->nstg].b; s.p = C->stg[C->nstg].p; s.st = C->nstg++;
    return s;
}
// the completion thread, in chunk_done after the chunk's write-backs: its blocks back to the free list
static void sp_done(chunk *c) {
    if (g_sp_on <= 0 || !c->nsb) return;   // g_sp_on: set before the completion thread started (chunks_init)
    pthread_mutex_lock(&g_sp_mu); for (int i = 0; i < c->nsb; i++) g_sp_free[g_sp_nfree++] = c->sb[i]; pthread_mutex_unlock(&g_sp_mu); c->nsb = 0;   // every block is in one chunk, the free list or sp_trim's list: at most SP_NBLK
}
// the completion thread, in chunk_thread once the chunk's NVN fence, write-backs (g_inflight_wb), g_seq_done and busy are published: every SP_TRIM completions, the blocks
// the free list held all along leave it, and one of them is freed a completion. Freed in chunk_done, a trim's vkDestroyBuffer + vkFreeMemory of up to SP_NBLK blocks
// (kgsl_sharedmem_free and an munmap of 4,096 pages each) ran before the chunk's fence and write-backs counted as done, so every sync on them waited for all of those frees
// at once; here the game's fence wait, submit_wait / seq_wait and cur() are released first, and one block's free delays at most the next chunk's completion
static void sp_trim(void) {
    static stg_t gone[SP_NBLK]; static int ngone; static unsigned win;   // completion thread only: blocks taken off the free list, not freed yet (still in g_sp_nblk, so at most SP_NBLK exist)
    if (g_sp_on <= 0) return;
    if (++win >= SP_TRIM) { win = 0;
        pthread_mutex_lock(&g_sp_mu);
        for (int k = g_sp_low; k > 0 && g_sp_nfree && ngone < SP_NBLK; k--) gone[ngone++] = g_sp_free[--g_sp_nfree];   // the free list never held fewer than g_sp_low: that many were idle the whole window
        g_sp_low = g_sp_nfree;
        pthread_mutex_unlock(&g_sp_mu); }
    if (!ngone) return;
    stg_free(&gone[--ngone]); atomic_fetch_add(&g_sp_trimmed, 1);
    pthread_mutex_lock(&g_sp_mu); g_sp_nblk--; pthread_mutex_unlock(&g_sp_mu);   // only now may sp_block make another in its place
}
static wb_t *wb_add(void) {
    if (!cur()) return NULL;
    if (C->nwb == C->capwb) { C->capwb = C->capwb ? 2 * C->capwb : 16; C->wb = realloc(C->wb, C->capwb * sizeof *C->wb); }
    return &C->wb[C->nwb++];
}
static void dead_add(int kind, uint64_t h) {   // destroyed when the current chunk completes (after every chunk before it, which may use it too)
    if (!h || !cur()) return;
    if (C->ndead == C->capdead) { C->capdead = C->capdead ? 2 * C->capdead : 32; C->dead = realloc(C->dead, C->capdead * sizeof *C->dead); }
    C->dead[C->ndead++] = (dead_t){kind, h};
}
static uint64_t g_flushes;
// ---------------- A GPU that hangs ----------------
// A chunk the GPU could not finish: on a OnePlus Pad 3 (Adreno 830, Android 16) Breath of the Wild's first frames in the field each had one chunk that took 240-430 ms on the
// GPU, the driver's hang recovery cutting it short (its fence signalled, its end timestamp never written), until one ran past the driver's 3 s, the device was lost
// (vkQueueSubmit -4) and the loading screen stayed up for good, with nothing in the log to say where. The kernel's report (kgsl) needs root on a phone, and VK_EXT_device_fault
// had nothing to say there (no address, no vendor record: a hang, not a page fault), so the log names what the chunk recorded instead (crumb): the GPU runs the chunks in
// order, the one before had completed, and the chunk it was in is the first that took over GPU_SLOW_NS. Breath of the Wild's named one draw every time, once the
// chunks were cut after every draw (a file vk_mid holding 1, mid_poll). At most 3 such chunks are logged, and the first lost fence or submission says what VK_EXT_device_fault gives
enum { GPU_SLOW_NS = 200000000 };   // a chunk this long on the GPU (64 draws by default, 512 with DM_VK_MID_DRAWS=512) is a hang or as good as one
static void gpu_fault_log(VkResult r) {   // once, from whichever thread sees the loss first
    static _Atomic int once; if (atomic_exchange(&once, 1)) return;
    vk_driver_lost(r);   // a DM_VK_DRIVER driver's lost device: the next start runs the system's driver once
    vk_start_lost(r);   // and the start record's, whatever the driver
    if (!g_fault_info) { LOG("[RED] [vk] the GPU was lost (%d); the driver offers no VK_EXT_device_fault to say why", r); return; }
    VkDeviceFaultCountsEXT fc = {.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    VkResult q = g_fault_info(g_vk.dev, &fc, NULL);
    if (q != VK_SUCCESS && q != VK_INCOMPLETE) { LOG("[RED] [vk] the GPU was lost (%d); vkGetDeviceFaultInfoEXT gave %d", r, q); return; }
    VkDeviceFaultAddressInfoEXT ad[16]; VkDeviceFaultVendorInfoEXT vd[16];
    if (fc.addressInfoCount > 16) fc.addressInfoCount = 16;
    if (fc.vendorInfoCount > 16) fc.vendorInfoCount = 16;
    fc.vendorBinarySize = 0;   // deviceFaultVendorBinary is not enabled
    VkDeviceFaultInfoEXT fi = {.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT, .pAddressInfos = fc.addressInfoCount ? ad : NULL, .pVendorInfos = fc.vendorInfoCount ? vd : NULL};
    q = g_fault_info(g_vk.dev, &fc, &fi);
    LOG("[RED] [vk] the GPU was lost (%d): \"%s\", %u fault addresses, %u vendor records (VK_EXT_device_fault: %d)", r, fi.description, fc.addressInfoCount, fc.vendorInfoCount, q);
    static const char *const kind[] = {"none", "a read of an invalid address", "a write to an invalid address", "execution at an invalid address", "instruction pointer, unknown",
                                       "instruction pointer, invalid", "instruction pointer of the fault"};
    for (uint32_t i = 0; i < fc.addressInfoCount; i++)
        LOG("[vk]   fault: %s at %#llx (precision %#llx)", (unsigned)ad[i].addressType < 7 ? kind[ad[i].addressType] : "?", (unsigned long long)ad[i].reportedAddress, (unsigned long long)ad[i].addressPrecision);
    for (uint32_t i = 0; i < fc.vendorInfoCount; i++)
        LOG("[vk]   vendor: \"%s\", code %#llx, data %#llx", vd[i].description, (unsigned long long)vd[i].vendorFaultCode, (unsigned long long)vd[i].vendorFaultData);
    bc_report("device lost", 0);
}
// A submission the queue refuses leaves its fence off the queue, and the completion thread waited on that fence for good: on an AYN Thor Pro (Adreno 740, Qualcomm's
// driver) Skyrim's vkQueueSubmit gave -3, which isn't one of a submission's results, the replay stopped behind it and Android called the app not responding
// (droidplay-core#444). The fence goes again alone, its work dropped, as a command buffer that didn't end does on the recorder. When the queue refuses that too, the GPU
// counts as lost (gpu_fault_log: the start record, and the next start on the system's driver after a DM_VK_DRIVER one) and the chunk keeps the result, so the completion
// thread doesn't wait on its fence. who: "" from the replay, " on the recorder". → VK_SUCCESS once the fence is on the queue, else the queue's result
// DM_VK_REFUSE_SUBMIT=n (env.txt), to test this: the nth submission is refused with -3 as that one was, and its fence alone goes; -n refuses that too
static int refuse_at(void) { static int v = -1; static int n; if (v < 0) { const char *e = getenv("DM_VK_REFUSE_SUBMIT"); n = e ? atoi(e) : 0; v = 1; } return n; }
static VkResult submit_or_fence(VkCommandBuffer cb, VkFence f, const char *who) {
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = cb ? 1u : 0u, .pCommandBuffers = &cb};
    static long nsub; int ra = refuse_at();
    pthread_mutex_lock(&g_vk.qmu);
    long k = ++nsub; int refuse = ra && k == (ra < 0 ? -(long)ra : (long)ra);
    VkResult r = refuse ? VK_ERROR_INITIALIZATION_FAILED : vkQueueSubmit(g_vk.q, 1, &si, f), alone = r;
    if (r != VK_SUCCESS && r != VK_ERROR_DEVICE_LOST && cb) { si.commandBufferCount = 0; alone = refuse && ra < 0 ? r : vkQueueSubmit(g_vk.q, 1, &si, f); }
    pthread_mutex_unlock(&g_vk.qmu);
    if (r == VK_SUCCESS) return VK_SUCCESS;
    if (alone == VK_SUCCESS) { RED_ONCE(166, "vkQueueSubmit failed%s: %d; its fence went alone, so the chunk completes with its work dropped", who, r); return VK_SUCCESS; }
    RED_ONCE(2, "vkQueueSubmit failed%s: %d%s", who, r, alone != r || (cb && r != VK_ERROR_DEVICE_LOST) ? ", and its fence alone too: the GPU counts as lost" : "");
    gpu_fault_log(alone);
    return alone;
}
// the completion thread: c took ns on the GPU, its fence gave fr; cut: its end timestamp was never written (the driver's hang recovery skipped the rest of it)
static void gpu_slow(chunk *c, VkResult fr, uint64_t ns, int cut) {
    if (fr != VK_SUCCESS) gpu_fault_log(fr);
    static int nrep; if (nrep >= 3) return;
    nrep++; bc_report("a slow submission", c->seq); int n = c->ncr < CR_MAX ? c->ncr : CR_MAX;
    LOG("[RED] [vk] submission %llu took %.0f ms on the GPU%s%s (the one before had completed): it recorded %d passes, draws and dispatches, the last %d of them:", (unsigned long long)c->seq, ns / 1e6,
        cut ? ", its end timestamp never written (the driver's hang recovery cut it short)" : "", fr == VK_ERROR_DEVICE_LOST ? ", and its fence reports the device lost" : fr != VK_SUCCESS ? ", never on the queue (it refused the submission)" : "", c->ncr, n);
    for (int k = 0; k < n;) {   // a run of the same crumb (Breath of the Wild's chunks hold runs of 17 alike draws) is one line, "× run"
        int i = (c->ncr - n + k) % CR_MAX, run = 1; char kd = c->cr[i].kind; int at = c->ncr - n + k;
        while (k + run < n) { int j = (c->ncr - n + k + run) % CR_MAX;
            if (c->cr[j].kind != kd || c->cr[j].a != c->cr[i].a || c->cr[j].b != c->cr[i].b || c->cr[j].n != c->cr[i].n || c->cr[j].m != c->cr[i].m) break;
            run++; }
        char x[24] = ""; if (run > 1) snprintf(x, sizeof x, " (x %d, to %d)", run, at + run - 1);
        if (kd == 'p') LOG("[vk]   %d: pass %u x %u%s", at, c->cr[i].n, c->cr[i].m, x);
        else if (kd == 'c' || kd == 'C') LOG("[vk]   %d: %sdispatch, compute %s, %u x %u groups%s", at, kd == 'C' ? "indirect " : "", c->cr[i].a, c->cr[i].n, c->cr[i].m, x);
        else LOG("[vk]   %d: %sdraw, vertex %s fragment %s, %u vertices x %u instances%s", at, kd == 'D' ? "indirect " : "", c->cr[i].a, c->cr[i].b ? c->cr[i].b : "-", c->cr[i].n, c->cr[i].m, x);
        k += run; }
}
static void rb_record(chunk *c), rb_invalidate(chunk *c);   // gfx_vk_readback.inc
// Submit the current chunk (an empty one too: its fence then follows the earlier ones); nvn_fence: the NVN fence it completes. → its sequence number, 0 if none
static uint64_t chunk_submit(uint64_t nvn_fence) {
    if (!C) return 0;
    if (g_drawing) RED_ONCE(3, "a chunk was closed while a draw was being made: that draw's resources went with it");
    rp_end();
    int rec = C->open;   // a chunk whose command buffer was never begun is submitted with none: the fence alone
    int deferred = rec_on();   // the recorder ends the command buffer and submits it after the calls queued before (rec_submit): the replay goes on recording meanwhile
    if (rec && (C->gm_st || C->ip_wr)) {   // raw stores or storage buffers written in place wrote guest memory itself (gfx_vk_gmem.inc): made visible to the host, which reads it once the fence is signalled
        VkMemoryBarrier hb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT | VK_ACCESS_HOST_WRITE_BIT};
        vkCmdPipelineBarrier(C->cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, NULL, 0, NULL); }
    g_cs_chain = 0;   // (the chunk's end barrier closes a run of dispatches)
    if (rec) { cb_barrier(); rb_record(C); if (C->ts) vkCmdWriteTimestamp(C->cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_tsq, 2 * (uint32_t)(C - g_ch) + 1); if (!deferred) vkEndCommandBuffer(C->cb); C->open = 0; }
    C->nvn_fence = nvn_fence;
    chunk *c = C; C = NULL;
    c->sub_r = VK_SUCCESS; c->rq_ticket = 0;
    if (deferred) c->rq_ticket = rec_submit(rec ? c->cb : VK_NULL_HANDLE, c->fence, &c->sub_r);   // the recorder submits it and keeps what the queue said in sub_r (rq_make R_SUBMIT)
    else c->sub_r = submit_or_fence(rec ? c->cb : VK_NULL_HANDLE, c->fence, "");
    g_flushes++;
    pthread_mutex_lock(&g_cmu);
    if (c->nwb || (c->gm_st && gm_store_wait())) atomic_fetch_add(&g_inflight_wb, 1);   // (raw stores: as a write-back of unknown range, chunk_hits)
    if (c->ncop) atomic_fetch_add(&g_inflight_cop, 1);
    if (c->nfwd) atomic_fetch_add(&g_inflight_fwd, 1);   // chunks in flight with a forwardable write-back (nfwd: gfx_vk_draw.inc stream_fwd)
    if (c->nipw) atomic_fetch_add(&g_inflight_ipw, 1);   // and with a draw's in-place write-back (nipw: gm_inplace_w), which a dispatch reads where it lies (fwd_hit)
    if (c->ts && g_fgpu_on > 0) { c->gf = atomic_load(&g_cur_frame); c->gtag = 1; fgpu_begin(c->gf); }
    g_fifo[g_fifo_n++] = c; uint64_t seq = c->seq;
    pthread_cond_broadcast(&g_ccv);
    pthread_mutex_unlock(&g_cmu);
    return seq;
}
static void seq_wait(uint64_t seq) { pthread_mutex_lock(&g_cmu); while (g_seq_done < seq) pthread_cond_wait(&g_ccv, &g_cmu); pthread_mutex_unlock(&g_cmu); }
static void submit_wait(void) { uint64_t s = chunk_submit(0); if (s) seq_wait(s); }   // submit and wait until the completion thread has processed it
static void flush(void) { chunk_submit(0); }
static void all_wait(void) { uint64_t s = chunk_submit(0); seq_wait(s ? s : g_seq); }   // everything submitted so far completed (submit_wait waits for nothing when no chunk is open)   // send what is recorded, without waiting
// before the replay reads or writes guest memory on the CPU: the GPU's pending writes land first (this chunk's: it is submitted and waited for; earlier ones: waited for)
// cops: the large copies and fills queued for the completion thread land too (wb_view: only the GPU's writes, for a copy queued behind the others)
static uint64_t g_vsite[8][2];   // syncs that waited, by call site (0 other, 1 draw reads in flight, 2 texture upload, 3 texture-to-buffer copy, 4 large copy, 5 buffer texture, 6 buffer image, 7 dispatch): submitted / in flight
static int g_vs;   // the call site of the next view_
static void view_(int cops) {
    int site = g_vs; g_vs = 0;
    if (C && (C->nwb || (C->gm_st && gm_store_wait()) || (cops && C->ncop))) { g_vsite[site][0]++; submit_wait(); return; }
    if (!atomic_load(&g_inflight_wb) && !(cops && atomic_load(&g_inflight_cop))) return;
    g_vsite[site][1]++;
    pthread_mutex_lock(&g_cmu); while (atomic_load(&g_inflight_wb) || (cops && atomic_load(&g_inflight_cop))) pthread_cond_wait(&g_ccv, &g_cmu); pthread_mutex_unlock(&g_cmu);
}
static void cpu_view(void) { view_(1); }
static void wb_view(void) { view_(0); }
// Pending guest writes by range: a draw syncs only when what it reads overlaps a write-back or a queued copy not landed yet (draw_view in gfx_vk_draw.inc).
// A sync on anything pending (cpu_view) made every draw after one storage-buffer write wait for the GPU: Super Mario Bros. Wonder spent 30% of its render thread there.
// Guest CPU reads need no gate here: the game reads GPU results only after an NVN fence, which completes after the chunk's write-backs
// DM_VK_SAFE=1 (env.txt): the earlier, coarser syncs (every draw, dispatch and CPU write waits for anything pending; no reads on the GPU of another draw's data, no early
// submissions; window acquires wait for everything submitted, hle_nvn.c): to tell a regression of these from another cause
static int vk_safe(void) { static int v = -1; if (v < 0) v = getenv("DM_VK_SAFE") != NULL; return v; }
static uint64_t g_view_cur, g_view_fly, g_view_free;   // draws that waited for the current chunk / for chunks in flight / that needed no wait
static int wb_hits(const wb_t *w, uintptr_t lo, uintptr_t hi) { uintptr_t a = (uintptr_t)w->dst, e = a + w->dst_pitch * (w->rows ? w->rows - 1 : 0) + w->row_bytes; return a < hi && lo < e; }
static int chunk_hits(const chunk *c, uintptr_t lo, uintptr_t hi, int write) {   // write: the CPU writes the range (a queued copy's source counts too: it is read when the copy runs)
    if (c->gm_st && gm_store_wait()) { g_gm_store_hits++; return 1; }   // raw stores ran in it: where they landed is known only from its write log, read when it completes
    for (int i = 0; i < c->nwb; i++) if (wb_hits(&c->wb[i], lo, hi)) return 1;
    for (int i = 0; i < c->ncop; i++) { const cop_t *o = &c->cop[i]; if ((o->dst < hi && lo < o->dst + o->n) || (write && !o->fill && o->src < hi && lo < o->src + o->n)) return 1; }
    return 0;
}
typedef struct { uintptr_t lo, hi; } rd_rng;
// the guest memory the replay is about to read (write: or write) on the CPU: when something pending overlaps it, the sync is made (the current chunk submitted and
// waited for, or the chunks in flight waited for)
static void ranges_view_(const rd_rng *r, int n, int write) {
    int cur_hit = 0, fly_hit = 0;
    if (C) for (int i = 0; i < n && !cur_hit; i++) if (r[i].hi > r[i].lo && chunk_hits(C, r[i].lo, r[i].hi, write)) cur_hit = 1;
    if (cur_hit) { g_view_cur++; submit_wait(); return; }
    if (atomic_load(&g_inflight_wb) || atomic_load(&g_inflight_cop)) {
        pthread_mutex_lock(&g_cmu);
        for (int f = 0; f < g_fifo_n && !fly_hit; f++) for (int i = 0; i < n && !fly_hit; i++) if (r[i].hi > r[i].lo && chunk_hits(g_fifo[f], r[i].lo, r[i].hi, write)) fly_hit = 1;
        pthread_mutex_unlock(&g_cmu);
    }
    if (fly_hit) { g_view_fly++; g_vs = 1; cpu_view(); return; }
    g_view_free++;
}
static void ranges_view(const rd_rng *r, int n) { ranges_view_(r, n, 0); }
// whether a write-back or a queued copy not landed yet (in the current chunk or one in flight) writes into [lo, hi): no wait, no count (gfx_vk_draw.inc lsnap_take)
static int wr_pending(uintptr_t lo, uintptr_t hi) {
    if (C && chunk_hits(C, lo, hi, 0)) return 1;
    if (!atomic_load(&g_inflight_wb) && !atomic_load(&g_inflight_cop)) return 0;
    int hit = 0; pthread_mutex_lock(&g_cmu);
    for (int f = 0; f < g_fifo_n && !hit; f++) hit = chunk_hits(g_fifo[f], lo, hi, 0);
    pthread_mutex_unlock(&g_cmu);
    return hit;
}
static uint64_t g_mem_cur, g_mem_fly, g_mem_free;   // the same for the submission's own CPU writes and small copies (uniform buffer updates, buffer copies and fills)
static void gm_ip_write(uintptr_t lo, uintptr_t hi);   // gfx_vk_gmem_inplace.inc: a chunk still to run that reads those pages in place runs first
static void mem_view(uint64_t rd, uint64_t rn, uint64_t wr, uint64_t wn) {
    if (wn) gm_ip_write((uintptr_t)wr, (uintptr_t)(wr + wn));
    if (vk_safe()) { cpu_view(); return; }
    uint64_t c = g_view_cur, f = g_view_fly, e = g_view_free;
    rd_rng r[1] = {{(uintptr_t)rd, (uintptr_t)(rd + rn)}}, w[1] = {{(uintptr_t)wr, (uintptr_t)(wr + wn)}};
    if (rn) ranges_view_(r, 1, 0);
    if (wn) ranges_view_(w, 1, 1);
    g_mem_cur += g_view_cur - c; g_mem_fly += g_view_fly - f; g_mem_free += g_view_free - e; g_view_cur = c; g_view_fly = f; g_view_free = e;
}
// ---------------- Linear render targets' write-backs (gfx_vk_draw.inc lin_writeback) ----------------
// A linear render target's level is copied back into guest memory when its pass's chunk completes, so the CPU reads what the GPU drew. It was copied back whole, with no snapshot,
// after wb_apply prepared (unprotected and stamped) every page. Breath of the Wild's s7928 (512x512 RGBA8, linear, CPU_CACHED; the grass vertex shaders read it, its w scaling all
// wind and flutter displacement) gets one draw a frame that changes its B byte under Link (MIN blend, mask 0x4): a few changed bytes became 1,048,576 written (Ayn Odin 2), and
//   - a byte the CPU wrote between the pass's upload and the write-back landing (the first 3 to 6 ms of each replay) was put back to its old value, which the GPU never saw: the grass
//     flicker's leading suspect (the revert check below counts it)
//   - all 256 pages were stamped, so the next sampling re-uploaded the level: about 4 re-uploads (1 MB) and 2.1 to 2.65 upload syncs a frame, and a G-buffer split before the first grass draw
// Off by default, with DM_VK_WB_ABSORB and DM_VK_UP_RANGE below: with the three on, Breath of the Wild's tall grass collapsed in single frames on a period of about 10 frames on an Ayn
// Odin 2 (4 of 4 captures, 0 of 3 with them off, draws unchanged), and which of them causes it is not found yet. =1 turns each on for that bisection and for measurements.
// DM_VK_LINWB_SNAP (=1; off: written back whole, every page prepared): the pass snapshots the target's guest bytes at rp_prepare, after vt_get made the image equal to them
// (gfx_vk_draw.inc lsnap_take), and the write-back writes only the bytes the GPU changed against it, preparing only the span from the first of them to the last (none changed: nothing
// prepared, no page stamped, no re-upload). A write still pending to those bytes at rp_prepare (an earlier pass's write-back, a queued copy) lands after the snapshot, and a byte this
// pass leaves at (or sets back to) the snapshot's value would then keep that write's value: such a pass takes no snapshot and is written back whole, as before. Two differences from
// the hardware remain, races a game would have anyway: a byte the CPU wrote between vt_get's upload and the snapshot is still put back; one the CPU wrote and the GPU set to the
// snapshot's value keeps the CPU's.
// The narrowing holds for every write-back with a snapshot: a linear render target's with this switch, a storage buffer's by default (DM_VK_WB_WORDS, below wb_row_diff)
static int lsnap_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_LINWB_SNAP"); v = e && !strcmp(e, "1"); if (v) LOG("[vk] DM_VK_LINWB_SNAP=1: linear render targets are written back as the bytes their pass changed (off by default: the grass flicker)"); } return v; }
static uint64_t g_lsnap, g_lsnap_stale;   // passes' snapshots taken; not taken: a write to the target was pending (gfx_vk_draw.inc lsnap_take)
static _Atomic uint64_t g_awb_n, g_awb_bytes, g_awb_rb, g_wb_ns;   // arena write-backs landed, their bytes, those read through the GPU's copy (gfx_vk_readback.inc); chunk_done's time landing every write-back
static _Atomic uint64_t g_lwb_whole, g_lwb_diff, g_lwb_same, g_lwb_bytes, g_lwb_ns;   // linear write-backs landed whole / as their changed bytes / with nothing changed; the bytes the latter two wrote;
                                                                                   // the completion thread's time landing them and noting them (ab_note), before the chunk's fence
// Snapshot buffers: one of 64 KB or more comes from a few kept for reuse (on Android a 1 MB malloc is an mmap and 256 first-touch faults, every pass), smaller ones are malloc'd
enum { SNAP_POOL_MIN = 64 << 10, SNAP_KEEP = 8, SNAP_KEEP_BYTES = 32 << 20 };
static struct { uint8_t *p; size_t cap; } g_snapf[SNAP_KEEP]; static int g_nsnapf; static size_t g_snapf_bytes; static pthread_mutex_t g_snap_mu = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *snap_take(size_t n, size_t *cap) {   // *cap: the pooled buffer's size, 0 for a malloc'd one (both go back through snap_give)
    *cap = 0; if (n < SNAP_POOL_MIN) return malloc(n ? n : 1);
    uint8_t *p = NULL; size_t c = 0; int best = -1;
    pthread_mutex_lock(&g_snap_mu);
    for (int i = 0; i < g_nsnapf; i++) if (g_snapf[i].cap >= n && (best < 0 || g_snapf[i].cap < g_snapf[best].cap)) best = i;
    if (best >= 0) { p = g_snapf[best].p; c = g_snapf[best].cap; g_snapf_bytes -= c; g_snapf[best] = g_snapf[--g_nsnapf]; }
    pthread_mutex_unlock(&g_snap_mu);
    if (!p && (p = malloc(n))) c = n;
    *cap = c; return p;
}
static void snap_give(uint8_t *p, size_t cap) {   // either thread; cap 0: free
    if (!p) return;
    if (cap) { pthread_mutex_lock(&g_snap_mu);
        int k = g_nsnapf < SNAP_KEEP ? g_nsnapf : -1;
        if (k < 0) { k = 0; for (int i = 1; i < SNAP_KEEP; i++) if (g_snapf[i].cap < g_snapf[k].cap) k = i;   // full: a larger one replaces the smallest
                     if (g_snapf[k].cap < cap) { free(g_snapf[k].p); g_snapf_bytes -= g_snapf[k].cap; g_nsnapf--; g_snapf[k] = g_snapf[g_nsnapf]; k = g_nsnapf; } else k = -1; }
        if (k >= 0 && g_snapf_bytes + cap <= SNAP_KEEP_BYTES) { g_snapf[k].p = p; g_snapf[k].cap = cap; g_nsnapf++; g_snapf_bytes += cap; p = NULL; }
        pthread_mutex_unlock(&g_snap_mu); }
    free(p);
}
// DM_VK_WB_REVERT=1 (diagnostic; the flicker plan's experiment 4a): before a linear render target's write-back lands, the bytes where guest memory differs from the pass's snapshot
// and from the GPU's result (the CPU, or the replay, wrote them since the pass began, to another value than the GPU's) are counted per channel (the texel's bytes in four groups: R G B A
// for RGBA8) and per texture, and of them those the GPU left at the snapshot's value: the whole-level write-back put those back to the old value (DM_VK_LINWB_SNAP keeps the CPU's).
// The first 10 write-backs with such bytes are logged, the counts every 300 frames (mtl_mem_stat). Without DM_VK_LINWB_SNAP the snapshot is taken for this alone (dsnap) and the
// write-back is whole; DM_VK_WB_ABSORB=1 and DM_VK_UP_RANGE=1 change the uploads and their syncs and so each pass's window from rp_prepare to its write-back landing, which the count
// samples. The default plus the count is DM_VK_WB_REVERT=1 alone. A pass with no snapshot (a write to its target was pending) is not compared
static int wb_revert_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_WB_REVERT"); v = e && atoi(e) != 0; if (v) LOG("[vk] DM_VK_WB_REVERT=1: linear write-backs are checked for bytes the CPU wrote during their pass"); } return v; }
enum { RV_SER = 32 };
static struct { uint64_t ser; uint32_t fmt, w, h; uint64_t wbs, events, cpu[4], rev[4]; } g_rv[RV_SER];
static int g_nrv, g_rv_logged; static uint64_t g_rv_wbs, g_rv_events, g_rv_unchecked, g_rv_lost; static pthread_mutex_t g_rv_mu = PTHREAD_MUTEX_INITIALIZER;
static void rv_check(const wb_t *w, const uint8_t *s) {   // the completion thread, before wb_apply(w)
    const uint8_t *o0 = w->snap ? w->snap : w->dsnap; uint64_t cpu[4] = {0}, rev[4] = {0}; size_t r0 = 0, r1 = 0; int any = 0; unsigned bpb = w->bpb ? w->bpb : 1;
    if (o0) for (size_t y = 0; y < w->rows; y++) {
        const uint8_t *g = w->dst + y * w->dst_pitch, *o = o0 + y * w->row_bytes, *q = s + y * w->src_pitch;
        if (!memcmp(g, o, w->row_bytes)) continue;
        for (size_t i = 0; i < w->row_bytes; i++) if (g[i] != o[i] && g[i] != q[i]) { unsigned b = (unsigned)(i % bpb), ch = bpb >= 4 ? b * 4 / bpb : b; cpu[ch]++; if (q[i] == o[i]) rev[ch]++; if (!any) r0 = y; r1 = y; any = 1; }
    }
    pthread_mutex_lock(&g_rv_mu);
    if (!o0) { g_rv_unchecked++; pthread_mutex_unlock(&g_rv_mu); return; }
    g_rv_wbs++; g_rv_events += (uint64_t)any;
    int k = 0; while (k < g_nrv && g_rv[k].ser != w->ser) k++;
    if (k == g_nrv) { if (g_nrv < RV_SER) { memset(&g_rv[k], 0, sizeof g_rv[k]); g_rv[k].ser = w->ser; g_rv[k].fmt = w->fmt; g_rv[k].w = w->w; g_rv[k].h = w->h; g_nrv++; } else k = -1; }
    if (k >= 0) { g_rv[k].wbs++; g_rv[k].events += (uint64_t)any; for (int c = 0; c < 4; c++) { g_rv[k].cpu[c] += cpu[c]; g_rv[k].rev[c] += rev[c]; } }
    else if (any) g_rv_lost++;
    int log = any && g_rv_logged < 10; g_rv_logged += log;
    pthread_mutex_unlock(&g_rv_mu);
    if (log) LOG("[vk] write-back revert check: s%llu %ux%u format %#x, rows %zu-%zu of %zu: bytes the CPU wrote since the pass's snapshot, not as the GPU did, %llu/%llu/%llu/%llu (by channel), of them left alone by the GPU "
                 "(put back by a whole write-back%s) %llu/%llu/%llu/%llu", (unsigned long long)w->ser, w->w, w->h, w->fmt, r0, r1, w->rows, (unsigned long long)cpu[0], (unsigned long long)cpu[1],
                 (unsigned long long)cpu[2], (unsigned long long)cpu[3], w->snap ? ", kept by this one" : ", as this one does", (unsigned long long)rev[0], (unsigned long long)rev[1], (unsigned long long)rev[2], (unsigned long long)rev[3]);
}
static void rv_stat(unsigned long long frame) {   // every 300 frames (mtl_mem_stat): the counts since the last line
    pthread_mutex_lock(&g_rv_mu);
    LOG("[vk] write-back revert check up to frame %llu: %llu linear write-backs compared with their pass's snapshot, %llu found bytes the CPU wrote during the pass, %llu not compared (no snapshot: a write to the target was pending)%s",
        frame, (unsigned long long)g_rv_wbs, (unsigned long long)g_rv_events, (unsigned long long)g_rv_unchecked, g_rv_lost ? " (more textures than listed)" : "");
    for (int k = 0; k < g_nrv; k++) if (g_rv[k].events) LOG("[vk]   s%llu %ux%u format %#x: %llu of %llu write-backs; bytes the CPU wrote by channel %llu/%llu/%llu/%llu, of them left alone by the GPU %llu/%llu/%llu/%llu",
        (unsigned long long)g_rv[k].ser, g_rv[k].w, g_rv[k].h, g_rv[k].fmt, (unsigned long long)g_rv[k].events, (unsigned long long)g_rv[k].wbs, (unsigned long long)g_rv[k].cpu[0], (unsigned long long)g_rv[k].cpu[1],
        (unsigned long long)g_rv[k].cpu[2], (unsigned long long)g_rv[k].cpu[3], (unsigned long long)g_rv[k].rev[0], (unsigned long long)g_rv[k].rev[1], (unsigned long long)g_rv[k].rev[2], (unsigned long long)g_rv[k].rev[3]);
    g_nrv = 0; g_rv_wbs = g_rv_events = g_rv_unchecked = g_rv_lost = 0;
    pthread_mutex_unlock(&g_rv_mu);
}
// A linear render target's own write-back is not new content for it (DM_VK_WB_ABSORB=1; off by default, see DM_VK_LINWB_SNAP: re-uploaded). What wb_apply wrote is what the image already holds, but the
// pages it prepared were stamped, so the next sampling re-uploaded the level: Breath of the Wild's s7928 at its first grass draw each frame (1 MB through a new staging buffer, an upload
// sync draining the GPU, the G-buffer pass split there at 46 | 18 draws) and its small linear targets at rp_prepare and clear_color. Right after that write-back lands, the completion
// thread takes a stamp, protects the storage again and keeps the texture's bytes in its edge pages, and notes (serial, storage, image, stamp) in a table under g_ab_mu only when guest
// memory then equals the GPU's copy in every row: a CPU write the diff kept (made since the snapshot) or one made before the protect is not in the image, and the texture is uploaded
// again. It never touches the vtex, which vt_destroy frees at once. vt_get, finding the texture written since its stamp, takes the note when it is still that texture's image, it was
// not uploaded after the note, no page wholly inside it was written after the stamp and its edge bytes are unchanged: the stamp and the edge bytes become its own and the image stays
// (ab_take), and the next lookup checks the pages again (an edge page a neighbour unprotected goes through edges_clean). Only one level and one layer (lin 2): the write-back then
// covers every byte vt_upload_ reads. A newer note of a serial replaces the older; a texture made again over the same storage has another serial or another image. The scheme is
// upstream nxnative's for storage-backed render targets on Metal (#604, not in this fork's nvn_mtl.m), which notes without comparing guest memory with the readback. Open: the
// pool's Finalize racing the protect (a game finalizes after the fence, which is completed after this)
static int wb_absorb_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_WB_ABSORB"); v = e && !strcmp(e, "1"); if (v) LOG("[vk] DM_VK_WB_ABSORB=1: a linear render target keeps its image after its own write-back (off by default: the grass flicker)"); } return v; }
typedef struct { uint64_t ser, img, gen; const uint8_t *store; uint8_t *edge[2]; uint32_t edge_n[2]; } ab_rec;
enum { NAB = 64 };
static ab_rec g_ab[NAB]; static int g_nab; static _Atomic int g_nab_n; static pthread_mutex_t g_ab_mu = PTHREAD_MUTEX_INITIALIZER;   // g_nab_n: read without the lock first (ab_take)
static _Atomic uint64_t g_ab_noted, g_ab_differ, g_ab_taken, g_ab_refused, g_ab_clean;
// Whether the CPU left a noted target's storage alone since its last note: then guest memory still holds that write-back in every whole page, the snapshot the next pass took
// equals it, and that pass's write-back makes guest memory the GPU's copy there by itself, so ab_note compares only the rows in the edge pages (which a neighbour's write may
// change). The full compare read the 8.3 MB of Skyward Sword HD's target twice more every frame on the completion thread, before the chunk's fence. nx2android
// (github.com/shirai91/nx2android, 70139b2) syncs only the blocks the CPU wrote, read from the same write-protect tracking. Kept apart from g_ab, which ab_take empties when
// the replay adopts a note. Asked before wb_apply, whose prepare makes the pages it writes dirty. DM_VK_AB_CLEAN=0 (env.txt): every row compared, as before
typedef struct { uint64_t ser, gen; const uint8_t *store; } ab_gen;
static ab_gen g_abg[NAB]; static int g_nabg;   // under g_ab_mu: each noted serial's last note stamp
static int ab_clean_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_AB_CLEAN"); v = !(e && *e == '0'); } return v; }
static void edge_rng(uintptr_t a, uintptr_t e, uintptr_t r[2][2], uintptr_t *ia, uintptr_t *ie) {   // [a, e)'s bytes in the host pages it shares with other data; [ia, ie): its whole pages
    uintptr_t pg = (uintptr_t)getpagesize();
    *ia = (a + pg - 1) & ~(pg - 1); *ie = e & ~(pg - 1);
    if (*ie <= *ia) { r[0][0] = a; r[0][1] = e; r[1][0] = r[1][1] = 0; *ia = *ie = 0; return; }
    r[0][0] = a; r[0][1] = *ia; r[1][0] = *ie; r[1][1] = e;
}
static int ab_clean(const wb_t *w) {   // the completion thread, before w lands: no whole page of its storage written since its last note (ab_note compares only the edge rows)
    if (!ab_clean_on()) return 0;
    uint64_t gen = 0; pthread_mutex_lock(&g_ab_mu);
    for (int i = 0; i < g_nabg; i++) if (g_abg[i].ser == w->ser && g_abg[i].store == w->dst) { gen = g_abg[i].gen; break; }
    pthread_mutex_unlock(&g_ab_mu);
    if (!gen) return 0;
    uintptr_t r[2][2], ia, ie; edge_rng((uintptr_t)w->dst, (uintptr_t)w->dst + (uintptr_t)w->tsize, r, &ia, &ie);
    return ie > ia && !nvn_dirty_since((const void *)ia, ie - ia, gen);
}
static void ab_note(const wb_t *w, const uint8_t *s, int clean) {   // the completion thread, right after w landed: before any later write of the chunk, its posts and its NVN fence
    uint64_t st = nvn_dirty_now(); nvn_dirty_protect(w->dst, (size_t)w->tsize);   // stamp, protect, then read (as vt_upload_): a later CPU write faults and is newer than st
    uintptr_t r[2][2], ia, ie; edge_rng((uintptr_t)w->dst, (uintptr_t)w->dst + (uintptr_t)w->tsize, r, &ia, &ie);
    ab_rec e; memset(&e, 0, sizeof e); e.ser = w->ser; e.img = w->img; e.gen = st; e.store = w->dst;
    for (int k = 0; k < 2; k++) if (r[k][1] > r[k][0]) { e.edge_n[k] = (uint32_t)(r[k][1] - r[k][0]); if (!(e.edge[k] = malloc(e.edge_n[k]))) goto drop; memcpy(e.edge[k], (const void *)r[k][0], e.edge_n[k]); }
    // after the edge bytes: a CPU write to the texture's own bytes before the copy differs here; one after it differs at ab_take
    for (size_t y = 0; y < w->rows; y++) {
        uintptr_t a = (uintptr_t)(w->dst + y * w->dst_pitch), z = a + w->row_bytes;   // clean (ab_clean): only a row with bytes in an edge page
        if (clean && !(z > r[0][0] && a < r[0][1]) && !(z > r[1][0] && a < r[1][1])) continue;
        if (memcmp(w->dst + y * w->dst_pitch, s + y * w->src_pitch, w->row_bytes)) { atomic_fetch_add(&g_ab_differ, 1); goto drop; }
    }
    if (clean) atomic_fetch_add(&g_ab_clean, 1);
    pthread_mutex_lock(&g_ab_mu);
    {   int k = 0; while (k < g_nabg && g_abg[k].ser != e.ser) k++;
        if (k == g_nabg && g_nabg == NAB) { k = 0; for (int j = 1; j < NAB; j++) if (g_abg[j].gen < g_abg[k].gen) k = j; }   // full: the oldest goes (its next write-back is compared whole)
        if (k == g_nabg) g_nabg++;
        g_abg[k] = (ab_gen){e.ser, st, e.store}; }
    int i = 0; while (i < g_nab && g_ab[i].ser != e.ser) i++;
    if (i == g_nab && g_nab == NAB) { i = 0; for (int k = 1; k < NAB; k++) if (g_ab[k].gen < g_ab[i].gen) i = k; }   // full: the oldest note goes (that texture is uploaded again)
    if (i < g_nab) { free(g_ab[i].edge[0]); free(g_ab[i].edge[1]); } else g_nab++;
    g_ab[i] = e; atomic_store(&g_nab_n, g_nab);
    pthread_mutex_unlock(&g_ab_mu);
    atomic_fetch_add(&g_ab_noted, 1); return;
drop: free(e.edge[0]); free(e.edge[1]);
}
// A row's bytes the GPU changed against the snapshot (DM_VK_LINWB_SNAP, DM_VK_WB_WORDS; both off: wb_apply's byte loop, as before), compared a word at a time: a word all changed is stored whole, one
// partly changed byte by byte. Never a merge of the word over the bytes left alone ((d & same) | (g & ~same)): its store would put back the value read just before it, losing a CPU
// write landing in between, the bytes the snapshot keeps. The byte loop made a memcpy call per run of changed bytes, a call and two word compares a texel when each texel keeps a
// channel (512x512 RGBA8 on an M-series Mac: alpha kept 0.99 ms, 0.30 ms this way; B alone changed 0.97 against 0.14), on the completion thread before the chunk's fence. → bytes written
_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "wb_row_diff: byte k of a word loaded from memory is its bits 8k..8k+7");
static size_t wb_row_diff(uint8_t *d, const uint8_t *g, const uint8_t *o, size_t rb) {
    if (!memcmp(g, o, rb)) return 0;
    size_t n = 0, i = 0;
    for (; i + 8 <= rb; i += 8) {
        uint64_t a, b; memcpy(&a, g + i, 8); memcpy(&b, o + i, 8); uint64_t x = a ^ b;
        if (!x) continue;
        uint64_t m = (((x & 0x7f7f7f7f7f7f7f7full) + 0x7f7f7f7f7f7f7f7full) | x) & 0x8080808080808080ull;   // bit 8k+7: byte k changed (no carry crosses a byte)
        if (m == 0x8080808080808080ull) { memcpy(d + i, &a, 8); n += 8; continue; }
        n += (size_t)__builtin_popcountll(m);
        for (; m; m &= m - 1) { unsigned k = (unsigned)__builtin_ctzll(m) >> 3; d[i + k] = (uint8_t)(a >> 8 * k); }
    }
    for (; i < rb; i++) if (g[i] != o[i]) { d[i] = g[i]; n++; }
    return n;
}
// The first and the last byte that differ in a span known to hold one, found a word at a time (the span's ends in wb_apply: a byte at a time, a storage buffer of hundreds of
// KB changed in its middle was walked to there from both ends)
static size_t wb_first_diff(const uint8_t *g, const uint8_t *o, size_t n) {
    size_t i = 0;
    for (; i + 8 <= n; i += 8) { uint64_t a, b; memcpy(&a, g + i, 8); memcpy(&b, o + i, 8); if (a != b) return i + (size_t)__builtin_ctzll(a ^ b) / 8; }
    for (; i < n; i++) if (g[i] != o[i]) return i;
    return n;
}
static size_t wb_last_diff(const uint8_t *g, const uint8_t *o, size_t n) {
    size_t i = n;
    for (size_t t = n & 7; t; t--, i--) if (g[i - 1] != o[i - 1]) return i - 1;
    for (; i >= 8; i -= 8) { uint64_t a, b; memcpy(&a, g + i - 8, 8); memcpy(&b, o + i - 8, 8); if (a != b) return i - 8 + (size_t)(63 - __builtin_clzll(a ^ b)) / 8; }
    return 0;
}
// Storage buffers' write-backs (and storage texel buffers'), which always carry a snapshot, land as the bytes the GPU changed found a word at a time, and only the span from the
// first of them to the last is prepared, as a linear render target's with DM_VK_LINWB_SNAP. They took the byte loop below whenever that switch was off (the default, kept for linear
// render targets: the grass flicker): a memcmp call per 8 bytes and a memcpy call per run of changed bytes, after every page of the buffer was prepared, on the completion thread
// before the chunk's NVN fence. Super Smash Bros. Ultimate's battles write back 1.1 to 2.1 MB of storage buffers a frame, which took a Retroid Pocket 5's completion thread 0.9 to
// 5.7 ms a frame to land. The same bytes are written either way; only pages no changed byte is in are left unprepared (not stamped: nothing kept from them is uploaded again).
// DM_VK_WB_WORDS=0 (env.txt): the byte loop and every page prepared, as before
static int wb_words_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_WB_WORDS"); v = !(e && !strcmp(e, "0")); if (!v) LOG("[vk] DM_VK_WB_WORDS=0: storage buffers' write-backs land a byte at a time, every page prepared"); } return v; }
// → whether it wrote (and prepared) anything
static int wb_apply(const wb_t *w, const uint8_t *s) {
    if (w->ds_n) {   // a depth / stencil readback: NVN's texels rebuilt from the aspects' planes, row by row (ds_wb)
        dm_guest_write_prepare(w->dst, w->dst_pitch * (w->rows ? w->rows - 1 : 0) + w->row_bytes);
        ds_slice((int)w->ds_fmt, w->ds_dkind, w->ds_sten, s, w->ds_n, w->rows, w->dst, w->dst_pitch);
        return 1;
    }
    size_t y0 = 0, y1 = w->rows, i0 = 0, i1 = 0; const size_t rb = w->row_bytes;
    int narrow = w->snap && (lsnap_on() || wb_words_on());
    if (!narrow) dm_guest_write_prepare(w->dst, w->dst_pitch * (w->rows ? w->rows - 1 : 0) + rb);   // pages the dirty tracker protected become writable (and dirty) first
    else {   // only the span from the first byte the GPU changed to the last is prepared and written; none: nothing at all
        while (y0 < w->rows && !memcmp(s + y0 * w->src_pitch, w->snap + y0 * rb, rb)) y0++;
        if (y0 == w->rows) return 0;
        y1 = w->rows - 1; while (y1 > y0 && !memcmp(s + y1 * w->src_pitch, w->snap + y1 * rb, rb)) y1--;
        i0 = wb_first_diff(s + y0 * w->src_pitch, w->snap + y0 * rb, rb); i1 = wb_last_diff(s + y1 * w->src_pitch, w->snap + y1 * rb, rb);
        uint8_t *a = w->dst + y0 * w->dst_pitch + i0, *e = w->dst + y1 * w->dst_pitch + i1 + 1;
        dm_guest_write_prepare(a, (size_t)(e - a)); y1++;
    }
    uint64_t n = 0;
    for (size_t y = y0; y < y1; y++) {
        uint8_t *d = w->dst + y * w->dst_pitch; const uint8_t *g = s + y * w->src_pitch;
        if (!w->snap) { memcpy(d, g, rb); continue; }
        const uint8_t *o = w->snap + y * rb;   // only what the GPU changed
        if (narrow) { size_t lo = y == y0 ? i0 : 0, hi = y + 1 == y1 ? i1 + 1 : rb; n += wb_row_diff(d + lo, g + lo, o + lo, hi - lo); continue; }   // the first and last rows from the span's ends
        for (size_t i = 0; i < w->row_bytes;) {
            if (i + 8 <= w->row_bytes && !memcmp(g + i, o + i, 8)) { i += 8; continue; }
            size_t e = i; while (e < w->row_bytes && g[e] != o[e]) e++;
            if (e > i) { memcpy(d + i, g + i, e - i); n += e - i; i = e; } else i++;
        }
    }
    if (w->lin && w->snap) atomic_fetch_add(&g_lwb_bytes, n);
    return 1;
}
static void post_apply(const post_op *o);   // with the submission code below
static void gm_ip_done(const chunk *c, int i);   // gfx_vk_gmem_inplace.inc
static void gm_chunk_done(chunk *c);   // gfx_vk_gmem.inc
typedef struct spare_mem spare_mem; static void spare_put(spare_mem *s);   // gfx_vk_spare.inc: dead kind 7
static void prof_collect(chunk *c);   // gfx_vk_draw.inc
static void chunk_done(chunk *c) {
    for (int i = 0; i < c->ncop; i++) {   // queued before any write-back of the chunk (a copy is queued only once the GPU's writes before it have landed)
        const cop_t *o = &c->cop[i]; dm_guest_write_prepare((void *)(uintptr_t)o->dst, o->n);
        if (!o->fill) memmove((void *)(uintptr_t)o->dst, (const void *)(uintptr_t)o->src, o->n);
        else { uint8_t *d = (uint8_t *)(uintptr_t)o->dst; for (uint64_t k = 0; k + 4 <= o->n; k += 4) memcpy(d + k, &o->value, 4); }
    }
    rb_invalidate(c);   // before any write-back is read from the GPU's copies
    uint64_t wt0 = c->nwb ? clock_gettime_nsec_np(CLOCK_MONOTONIC) : 0, an = 0, ab = 0, ar = 0;
    for (int i = 0; i < c->nwb; i++) { const wb_t *w = &c->wb[i];
        if (w->inpl) { gm_ip_done(c, i); continue; }   // the GPU wrote guest memory itself: its pages count as written (and are writable) from now on (gfx_vk_gmem_inplace.inc)
        const uint8_t *s = w->rb ? c->rb.p + (w->rb - 1) : (w->st >= SP_ST ? c->sb[w->st - SP_ST].p : w->st >= 0 ? c->stg[w->st].p : c->ar[-1 - w->st].p) + w->off;
        if (w->st < 0) { an++; ab += w->rows ? w->src_pitch * (w->rows - 1) + w->row_bytes : 0; ar += w->rb != 0; }
        if (w->rbx) rb16_rows((uint8_t *)(uintptr_t)s, w->row_bytes, w->rows, w->src_pitch, w->rbx);   // the GPU's copy, in Vulkan's order, into the guest's (before the snapshot compare)
        if (w->lin && wb_revert_on()) rv_check(w, s);   // before it lands: guest memory still holds what the CPU wrote during the pass
        uint64_t t0 = w->lin ? clock_gettime_nsec_np(CLOCK_MONOTONIC) : 0; int clean = w->lin == 2 && wb_absorb_on() ? ab_clean(w) : 0, wrote = wb_apply(w, s);
        if (w->lin) atomic_fetch_add(!w->snap || !lsnap_on() ? &g_lwb_whole : wrote ? &g_lwb_diff : &g_lwb_same, 1);
        if (w->lin == 2 && wrote && wb_absorb_on()) ab_note(w, s, clean);   // right after it: before a later write-back or post of the chunk
        if (w->lin) atomic_fetch_add(&g_lwb_ns, clock_gettime_nsec_np(CLOCK_MONOTONIC) - t0);
        snap_give(w->snap, w->snap_cap); snap_give(w->dsnap, w->snap_cap); }   // a storage buffer's snapshot: snap_cap 0, freed
    if (c->nwb) { atomic_fetch_add(&g_wb_ns, clock_gettime_nsec_np(CLOCK_MONOTONIC) - wt0); atomic_fetch_add(&g_awb_n, an); atomic_fetch_add(&g_awb_bytes, ab); atomic_fetch_add(&g_awb_rb, ar); }
    c->rb.used = 0;
    if (c->npq) prof_collect(c);
    if (c->nq) { uint64_t r[1024]; memset(r, 0, sizeof r);
        if (vkGetQueryPoolResults(g_vk.dev, c->qp, 0, (uint32_t)c->nq, sizeof r, r, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS)
            for (int i = 0; i < c->nq; i++) g_qres[c->qg[i] % QRES] = r[i];
        c->nq = 0; }
    if (c->ntq) { uint64_t r[256]; memset(r, 0, sizeof r);
        int ok = c->t_end && vkGetQueryPoolResults(g_vk.dev, c->tq, 0, (uint32_t)c->ntq, sizeof r, r, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS;
        uint64_t g = guest_ns(), base = g ? g : clock_gettime_nsec_np(CLOCK_MONOTONIC);
        for (int i = 0; i < c->npost; i++) { post_op *o = &c->post[i]; if (o->kind != 4) continue;
            uint64_t back = ok && o->q0 < (uint64_t)c->ntq && r[o->q0] && r[o->q0] <= c->t_end ? (uint64_t)((double)(c->t_end - r[o->q0]) * g_vk.props.limits.timestampPeriod) : 0;
            o->q1 = nvn_timestamp_from_ns(back < base ? base - back : base); }
        c->ntq = 0; }
    for (int i = 0; i < c->npost; i++) post_apply(&c->post[i]);
    for (int i = 0; i < c->nstg; i++) stg_free(&c->stg[i]);
    if (c->ngml) gm_chunk_done(c);   // its raw stores' write logs, before its dead objects (a table they index may be one)
    sp_done(c);   // after the write-backs, which read its blocks
    for (int i = 0; i < c->ndead; i++) {
        uint64_t h = c->dead[i].h;
        switch (c->dead[i].kind) { case 1: vkDestroyImageView(g_vk.dev, (VkImageView)h, NULL); break; case 2: vkDestroyFramebuffer(g_vk.dev, (VkFramebuffer)h, NULL); break;
                                   case 3: vkDestroyImage(g_vk.dev, (VkImage)h, NULL); break; case 4: vkFreeMemory(g_vk.dev, (VkDeviceMemory)h, NULL); break;
                                   case 5: vkDestroyBuffer(g_vk.dev, (VkBuffer)h, NULL); break; case 7: spare_put((spare_mem *)(uintptr_t)h); break; }
    }
    draw_chunk_reset(c);
    vkResetCommandPool(g_vk.dev, c->pool, 0); vkResetFences(g_vk.dev, 1, &c->fence);
    c->nstg = c->npost = c->ndead = 0;
}
static _Atomic uint64_t g_fence, g_done, g_presents;
static pthread_mutex_t g_fmu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_fcv = PTHREAD_COND_INITIALIZER;
static void *chunk_thread(void *arg) {
    (void)arg; pthread_setname_np(pthread_self(), "vk-done");
    uint64_t last = 0;   // the GPU time the previous chunk ended (g_fgpu: each chunk counts its span after it)
    void dm_android_hint_thread(int32_t tid); dm_android_hint_thread(gettid());   // in the performance hint session (plat_android.c): the replay waits for its write-backs
    void dm_android_place_thread(int done); dm_android_place_thread(1);   // the placement experiment (DM_VK_PIN_DONE, DM_VK_UCLAMP_MIN), off by default
    for (;;) {
        pthread_mutex_lock(&g_cmu); while (!g_fifo_n) pthread_cond_wait(&g_ccv, &g_cmu); chunk *c = g_fifo[0]; pthread_mutex_unlock(&g_cmu);
        rec_wait_submitted(c->rq_ticket);   // a deferred submission reached the queue first (gfx_vk_rec.inc); before w0, so the recorder's lag is not taken for GPU time
        uint64_t w0 = clock_gettime_nsec_np(CLOCK_MONOTONIC);
        VkResult fr = c->sub_r != VK_SUCCESS ? c->sub_r : vkWaitForFences(g_vk.dev, 1, &c->fence, VK_TRUE, UINT64_MAX);   // a fence never on the queue never signals
        uint64_t waited = clock_gettime_nsec_np(CLOCK_MONOTONIC) - w0; int cut = 0;   // the chunk's time on the GPU (the one before had completed), at most (gpu_slow)
        int ts = c->ts, nq = c->nq, ntq = c->ntq, npq = c->npq;   // the queries it used, reset once read (qr_done): the counts are zeroed on the way
        c->t_end = 0; uint64_t busy = 0;
        if (c->ts) { uint64_t t[2]; c->ts = 0;
            VkResult tr = vkGetQueryPoolResults(g_vk.dev, g_tsq, 2 * (uint32_t)(c - g_ch), 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT); cut = tr == VK_NOT_READY && fr == VK_SUCCESS;
            if (tr == VK_SUCCESS && t[1] > t[0])
                { atomic_fetch_add(&g_gpu_ns, (uint64_t)((double)(t[1] - t[0]) * g_vk.props.limits.timestampPeriod)); atomic_fetch_add(&g_gpu_chunks, 1); c->t_end = t[1];
                  uint64_t s = t[1] > last && t[0] < last ? last : t[0];   // from the previous chunk's end when it started before it; an end at or before that one (the counter started again) counts the whole span
                  busy = (uint64_t)((double)(t[1] - s) * g_vk.props.limits.timestampPeriod); last = t[1]; } }
        if (waited >= GPU_SLOW_NS || fr != VK_SUCCESS) gpu_slow(c, fr, waited, cut);
        int had_wb = c->nwb != 0 || (c->gm_st && gm_store_wait()), had_cop = c->ncop != 0, had_fwd = c->nfwd != 0, had_ipw = c->nipw != 0;   // (gm_st: chunk_done clears it)
        chunk_done(c); c->nwb = c->ncop = c->nfwd = c->nipw = 0;
        if (c->nvn_fence) { pthread_mutex_lock(&g_fmu); if (atomic_load(&g_done) < c->nvn_fence) atomic_store(&g_done, c->nvn_fence); pthread_cond_broadcast(&g_fcv); pthread_mutex_unlock(&g_fmu);
                            nvn_queue_gpu_progress(); }   // the queue scheduler's waits on GPU progress (nvn_queue.inc), as nvn_mtl.m post_complete
        qr_done(c, fr == VK_SUCCESS, ts, nq, ntq, npq);   // after every read of them, after the NVN fence (the game waits on it), before busy = 0
        pthread_mutex_lock(&g_cmu);
        memmove(g_fifo, g_fifo + 1, --g_fifo_n * sizeof *g_fifo);
        if (had_wb) atomic_fetch_sub(&g_inflight_wb, 1);
        if (had_cop) atomic_fetch_sub(&g_inflight_cop, 1);
        if (had_fwd) atomic_fetch_sub(&g_inflight_fwd, 1);   // after its write-backs landed (chunk_done), as g_inflight_wb
        if (had_ipw) atomic_fetch_sub(&g_inflight_ipw, 1);
        g_seq_done = c->seq;
        if (c->gtag) { fgpu_done(c->gf, busy); c->gtag = 0; }
        if (c->hold > c->seq) c->held = 1; else { c->busy = 0; c->hold = 0; }
        for (int i = 0; i < g_nchunk; i++) if (g_ch[i].held && g_ch[i].hold <= g_seq_done) { g_ch[i].held = 0; g_ch[i].hold = 0; g_ch[i].busy = 0; }
        pthread_cond_broadcast(&g_ccv);
        pthread_mutex_unlock(&g_cmu);
        sp_trim();   // after the fence and g_seq_done: no sync waits for a trim's frees
    }
    return NULL;
}

// ---------------- Memory pools (GPU address = CPU address) ----------------
// Kept sorted by start, found by binary search, and remembered per thread (the last PC_RECENT pools found, valid while g_pool_gen is unchanged: every add and removal bumps it),
// as nvn_mtl.m's pool index does. Every uniform and storage buffer of every draw asks here whether it lies in a pool; a scan of every pool under the lock took 20% of Breath of
// the Wild's render thread on an Ayn Odin 2 (mtl_addr inlined into stage_set, about 3,300 draws a frame)
typedef struct { const uint8_t *lo, *hi; uint8_t how; int flags; } pool_rng;   // how: the GPU addresses the game took in it (mtl_pool_addressed): bit 0 a buffer's (the whole pool), bit 1 a texture's; flags: NVN's
static pool_rng *g_pools; static int g_npool, g_cappool; static pthread_mutex_t g_pool_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t g_pool_gen = 1;
enum { PC_RECENT = 8 };
static __thread struct { uint64_t gen; unsigned next; pool_rng r[PC_RECENT]; } t_pc;   // an empty entry has hi NULL
static void gm_place(const void *mem, uint64_t size, int live), gm_unplace(const void *mem);   // raw global memory (gfx_vk_gmem.inc): the pool's pages as placed Vulkan memory, with DM_VK_RAW_GMEM=1
// The pool's NVN flags, which hle_nvn.c gives just before mtl_pool_add on the same thread (a weak hook there): raw global memory leaves shader-code pools unplaced (gfx_vk_gmem.inc)
static __thread const void *t_pf_mem; static __thread int t_pf_flags;
void mtl_pool_flags(const void *mem, uint64_t size, int flags) { (void)size; t_pf_mem = mem; t_pf_flags = flags; }
static int gm_code_skip(int flags);   // gfx_vk_gmem.inc
static void fb_take(const void *mem, uint64_t size, int flags); static void fb_gone(const void *mem); static void fb_stat(void);   // gfx_vk_fileback.inc
void mtl_pool_add(const void *mem, uint64_t size) {
    int flags = t_pf_mem == mem ? t_pf_flags : 0;
    if (!gm_code_skip(flags)) gm_place(mem, size, 0);   // a pool made again where one had a buffer address: placed now, before the replay knows the range (gfx_vk_gmem.inc)
    fb_take(mem, size, flags);   // backed by a file (DM_VK_POOL_FILEBACK, gfx_vk_fileback.inc), before the dirty tracker protects it
    pthread_mutex_lock(&g_pool_mu);
    if (g_npool == g_cappool) { g_cappool = g_cappool ? 2 * g_cappool : 64; g_pools = realloc(g_pools, g_cappool * sizeof *g_pools); }
    int at = g_npool; while (at > 0 && g_pools[at - 1].lo > (const uint8_t *)mem) at--;   // pools come and go rarely: a move keeps the order
    memmove(g_pools + at + 1, g_pools + at, (size_t)(g_npool - at) * sizeof *g_pools); g_npool++;
    g_pools[at] = (pool_rng){mem, (const uint8_t *)mem + size, 0, flags};
    atomic_fetch_add_explicit(&g_pool_gen, 1, memory_order_release);
    pthread_mutex_unlock(&g_pool_mu);
}
void mtl_pool_del(const void *mem) {
    pthread_mutex_lock(&g_pool_mu);
    for (int i = 0; i < g_npool; i++) if (g_pools[i].lo == mem) { memmove(g_pools + i, g_pools + i + 1, (size_t)(--g_npool - i) * sizeof *g_pools); break; }
    atomic_fetch_add_explicit(&g_pool_gen, 1, memory_order_release);
    pthread_mutex_unlock(&g_pool_mu);
    gm_unplace(mem);
    fb_gone(mem);
}
static int pool_find(const uint8_t *a, uint64_t len) {   // under g_pool_mu: the pool holding [a, a + len), or -1
    int lo = 0, hi = g_npool - 1, hit = -1;   // the last pool whose start ≤ a
    while (lo <= hi) { int m = (lo + hi) / 2; if (g_pools[m].lo <= a) { hit = m; lo = m + 1; } else hi = m - 1; }
    if (hit >= 0 && a + len <= g_pools[hit].hi) return hit;
    for (int i = 0; i < g_npool; i++) if (a >= g_pools[i].lo && a + len <= g_pools[i].hi) return i;   // pools that overlap: the first that holds it, as before
    return -1;
}
int mtl_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off) {
    const uint8_t *a = (const uint8_t *)(uintptr_t)addr; pool_rng p = {NULL, NULL};
    if (t_pc.gen == atomic_load_explicit(&g_pool_gen, memory_order_acquire))
        for (unsigned k = 0; k < PC_RECENT; k++) if (a >= t_pc.r[k].lo && a + len <= t_pc.r[k].hi) { p = t_pc.r[k]; break; }
    if (!p.hi) {
        pthread_mutex_lock(&g_pool_mu);
        int i = pool_find(a, len);
        if (i >= 0) {
            p = g_pools[i]; uint64_t gen = atomic_load_explicit(&g_pool_gen, memory_order_relaxed);
            if (t_pc.gen != gen) { memset(t_pc.r, 0, sizeof t_pc.r); t_pc.gen = gen; }
            t_pc.r[t_pc.next++ % PC_RECENT] = p;
        }
        pthread_mutex_unlock(&g_pool_mu);
        if (!p.hi) return 0;
    }
    if (buf) *buf = (void *)p.lo; if (off) *off = addr - (uint64_t)(uintptr_t)p.lo;
    return 1;
}
void vt_drop_pool(const void *pool);   // below, with the textures
void mtl_arch_save(int wait_ms, int ending) { (void)wait_ms; (void)ending; }   // Metal's pipeline archive (nvn_mtl.m); the Vulkan pipeline cache is written at pause (front_android.c) and during play
void mtl_alias_drop_pool(const void *pool) { vt_drop_pool(pool); }

// ---------------- Formats ----------------
// MTLPixelFormat (the number nvn_fmt_to_mtl gives, see nvn_fmt_mtl.c) → VkFormat. Metal names packed formats from the least significant bits, Vulkan from the most
static VkFormat vk_of_mtl(uint32_t m) {
    switch (m) {
        case 10: return VK_FORMAT_R8_UNORM; case 12: return VK_FORMAT_R8_SNORM; case 13: return VK_FORMAT_R8_UINT; case 14: return VK_FORMAT_R8_SINT;
        case 20: return VK_FORMAT_R16_UNORM; case 22: return VK_FORMAT_R16_SNORM; case 23: return VK_FORMAT_R16_UINT; case 24: return VK_FORMAT_R16_SINT; case 25: return VK_FORMAT_R16_SFLOAT;
        case 30: return VK_FORMAT_R8G8_UNORM; case 32: return VK_FORMAT_R8G8_SNORM; case 33: return VK_FORMAT_R8G8_UINT; case 34: return VK_FORMAT_R8G8_SINT;
        case 40: return VK_FORMAT_R5G6B5_UNORM_PACK16; case 41: return VK_FORMAT_R5G5B5A1_UNORM_PACK16; case 42: return VK_FORMAT_R4G4B4A4_UNORM_PACK16; case 43: return VK_FORMAT_A1R5G5B5_UNORM_PACK16;
        case 53: return VK_FORMAT_R32_UINT; case 54: return VK_FORMAT_R32_SINT; case 55: return VK_FORMAT_R32_SFLOAT;
        case 60: return VK_FORMAT_R16G16_UNORM; case 62: return VK_FORMAT_R16G16_SNORM; case 63: return VK_FORMAT_R16G16_UINT; case 64: return VK_FORMAT_R16G16_SINT; case 65: return VK_FORMAT_R16G16_SFLOAT;
        case 70: return VK_FORMAT_R8G8B8A8_UNORM; case 71: return VK_FORMAT_R8G8B8A8_SRGB; case 72: return VK_FORMAT_R8G8B8A8_SNORM; case 73: return VK_FORMAT_R8G8B8A8_UINT; case 74: return VK_FORMAT_R8G8B8A8_SINT;
        case 80: return VK_FORMAT_B8G8R8A8_UNORM; case 81: return VK_FORMAT_B8G8R8A8_SRGB;
        case 90: return VK_FORMAT_A2B10G10R10_UNORM_PACK32; case 91: return VK_FORMAT_A2B10G10R10_UINT_PACK32; case 92: return VK_FORMAT_B10G11R11_UFLOAT_PACK32; case 93: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
        case 103: return VK_FORMAT_R32G32_UINT; case 104: return VK_FORMAT_R32G32_SINT; case 105: return VK_FORMAT_R32G32_SFLOAT;
        case 110: return VK_FORMAT_R16G16B16A16_UNORM; case 112: return VK_FORMAT_R16G16B16A16_SNORM; case 113: return VK_FORMAT_R16G16B16A16_UINT; case 114: return VK_FORMAT_R16G16B16A16_SINT; case 115: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case 123: return VK_FORMAT_R32G32B32A32_UINT; case 124: return VK_FORMAT_R32G32B32A32_SINT; case 125: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case 130: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK; case 131: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK; case 132: return VK_FORMAT_BC2_UNORM_BLOCK; case 133: return VK_FORMAT_BC2_SRGB_BLOCK;
        case 134: return VK_FORMAT_BC3_UNORM_BLOCK; case 135: return VK_FORMAT_BC3_SRGB_BLOCK; case 140: return VK_FORMAT_BC4_UNORM_BLOCK; case 141: return VK_FORMAT_BC4_SNORM_BLOCK;
        case 142: return VK_FORMAT_BC5_UNORM_BLOCK; case 143: return VK_FORMAT_BC5_SNORM_BLOCK; case 150: return VK_FORMAT_BC6H_SFLOAT_BLOCK; case 151: return VK_FORMAT_BC6H_UFLOAT_BLOCK;
        case 152: return VK_FORMAT_BC7_UNORM_BLOCK; case 153: return VK_FORMAT_BC7_SRGB_BLOCK;
        case 250: return VK_FORMAT_D16_UNORM; case 252: return VK_FORMAT_D32_SFLOAT; case 253: return VK_FORMAT_S8_UINT; case 260: return VK_FORMAT_D32_SFLOAT_S8_UINT;
        default: break;
    }
    static const uint8_t astc_idx[15] = {0, 1, 2, 3, 4, 255, 5, 6, 7, 8, 9, 10, 11, 12, 13};   // Metal's ASTC numbers skip one between 6x6 and 8x5 (nvn_fmt_mtl.c astc_off)
    if (m >= 204 && m <= 218 && astc_idx[m - 204] != 255) return (VkFormat)(VK_FORMAT_ASTC_4x4_UNORM_BLOCK + 2 * astc_idx[m - 204]);
    if (m >= 186 && m <= 200 && astc_idx[m - 186] != 255) return (VkFormat)(VK_FORMAT_ASTC_4x4_SRGB_BLOCK + 2 * astc_idx[m - 186]);
    return VK_FORMAT_UNDEFINED;
}
// BC decoded on the CPU when the GPU has no BC: the format the decoder writes
enum { CONV_NONE, CONV_BC1, CONV_BC2, CONV_BC3, CONV_BC4, CONV_BC4S, CONV_BC5, CONV_BC5S, CONV_BC6H, CONV_BC6HS, CONV_BC7, CONV_U16H, CONV_S16H };
// A 16-bit normalized colour format the GPU cannot filter linearly, made as the 16-bit float format of its channels where that one filters (and draws, and blends where
// the normalized one does): the Xclipse 950 filters none of R16_UNORM, R16G16_UNORM, R16G16B16A16_UNORM and their SNORM forms (no SAMPLED_IMAGE_FILTER_LINEAR), and
// the Adreno 830 and Mali-G715 filter them all. A Maxwell sampler filters them, and games count on it: Brilliant Diamond's water simulation steps its R16G16_UNORM
// height field by reading the neighbours 1.5 texels away through a linear sampler, between texels on purpose, so the filter is part of its stencil. Point sampled (the
// Xclipse's answer, and what Vulkan's NEAREST gives) the field diverged to 0 and 1 in a checkerboard, a grid of white dashes on Twinleaf's pond. The float format holds
// the texels to 11 significant bits instead of 16; uploads convert (bc_decode), and so do CPU reads (mtl_read_texels, n16_back). A texture-to-buffer copy of such an
// image is not made, as one of a BC image decoded on the CPU is not (copy_t2b, a red line).
// DM_VK_N16_HALF=0 (env.txt): the normalized formats, as before
static VkFormat n16_twin(VkFormat f, int *conv, int *px) {   // the float format of a 16-bit normalized one, its conversion and texel size (0: not one)
    switch (f) {
        case VK_FORMAT_R16_UNORM: *conv = CONV_U16H; *px = 2; return VK_FORMAT_R16_SFLOAT; case VK_FORMAT_R16_SNORM: *conv = CONV_S16H; *px = 2; return VK_FORMAT_R16_SFLOAT;
        case VK_FORMAT_R16G16_UNORM: *conv = CONV_U16H; *px = 4; return VK_FORMAT_R16G16_SFLOAT; case VK_FORMAT_R16G16_SNORM: *conv = CONV_S16H; *px = 4; return VK_FORMAT_R16G16_SFLOAT;
        case VK_FORMAT_R16G16B16A16_UNORM: *conv = CONV_U16H; *px = 8; return VK_FORMAT_R16G16B16A16_SFLOAT;
        case VK_FORMAT_R16G16B16A16_SNORM: *conv = CONV_S16H; *px = 8; return VK_FORMAT_R16G16B16A16_SFLOAT;
        default: return VK_FORMAT_UNDEFINED;
    }
}
static VkFormat n16_half(VkFormat f, int *conv, int *px) {   // n16_twin where the GPU needs it (above); 0: the format as it is
    static int on = -1; if (on < 0) { const char *e = getenv("DM_VK_N16_HALF"); on = !(e && *e == '0'); }
    int c = 0, p = 0; VkFormat h = on ? n16_twin(f, &c, &p) : VK_FORMAT_UNDEFINED; if (!h) return VK_FORMAT_UNDEFINED;
    VkFormatProperties a, b; vkGetPhysicalDeviceFormatProperties(g_vk.pd, f, &a); vkGetPhysicalDeviceFormatProperties(g_vk.pd, h, &b);
    VkFormatFeatureFlags fa = a.optimalTilingFeatures, fb = b.optimalTilingFeatures, need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
        (fa & (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT));
    if (!(fa & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) || (fa & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) || (fb & need) != need) return VK_FORMAT_UNDEFINED;
    static _Atomic uint32_t told; if (!(atomic_fetch_or(&told, 1u << (f & 31)) & (1u << (f & 31))))
        LOG("[vk] VkFormat %d is not filtered linearly on this GPU: its sampled images are made as %d, which is (DM_VK_N16_HALF=0: as they are)", (int)f, (int)h);
    *conv = c; *px = p; return h;
}
static float h2f(uint16_t h) {   // an IEEE half as a float
    uint32_t s = (uint32_t)(h & 0x8000) << 16, e = (h >> 10) & 0x1f, m = h & 0x3ff, u;
    if (!e) { if (!m) u = s; else { int k = -1; do { k++; m <<= 1; } while (!(m & 0x400)); u = s | (uint32_t)(112 - k) << 23 | (m & 0x3ff) << 13; } }
    else if (e == 31) u = s | 0x7f800000 | m << 13;
    else u = s | (e + 112) << 23 | m << 13;
    float f; memcpy(&f, &u, 4); return f;
}
static void n16_back(int conv, uint8_t *p, size_t n) {   // halves (n16_half) back to the 16-bit normalized texels the game reads, in place
    for (size_t i = 0; i + 2 <= n; i += 2) {
        uint16_t q; memcpy(&q, p + i, 2); float f = h2f(q);
        if (conv == CONV_U16H) { f = f != f || f < 0 ? 0 : f > 1 ? 1 : f; q = (uint16_t)(f * 65535.0f + 0.5f); }
        else { f = f != f ? 0 : f < -1 ? -1 : f > 1 ? 1 : f; q = (uint16_t)(int16_t)lrintf(f * 32767.0f); }
        memcpy(p + i, &q, 2);
    }
}
static uint16_t f2h(float x) {   // a float as IEEE half, rounded to nearest even (the values here are in [-1, 1])
    uint32_t u; memcpy(&u, &x, 4); uint32_t s = (u >> 16) & 0x8000, e = (u >> 23) & 0xff, m = u & 0x7fffff;
    if (e < 103) return (uint16_t)s;   // under half's smallest subnormal: signed zero
    if (e < 113) { m |= 0x800000; uint32_t sh = 126 - e, r = m >> sh, rem = m & ((1u << sh) - 1), half = 1u << (sh - 1); if (rem > half || (rem == half && (r & 1))) r++; return (uint16_t)(s | r); }
    uint32_t r = ((e - 112) << 10) | (m >> 13), rem = m & 0x1fff; if (rem > 0x1000 || (rem == 0x1000 && (r & 1))) r++;
    return (uint16_t)(s | (r > 0x7bff ? 0x7c00 : r));
}
static VkFormat bc_target(VkFormat f, int *conv, int *px) {
    switch (f) {
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: *conv = CONV_BC1; *px = 4; return VK_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: *conv = CONV_BC1; *px = 4; return VK_FORMAT_R8G8B8A8_SRGB;
        case VK_FORMAT_BC2_UNORM_BLOCK: *conv = CONV_BC2; *px = 4; return VK_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_BC2_SRGB_BLOCK: *conv = CONV_BC2; *px = 4; return VK_FORMAT_R8G8B8A8_SRGB;
        case VK_FORMAT_BC3_UNORM_BLOCK: *conv = CONV_BC3; *px = 4; return VK_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_BC3_SRGB_BLOCK: *conv = CONV_BC3; *px = 4; return VK_FORMAT_R8G8B8A8_SRGB;
        case VK_FORMAT_BC4_UNORM_BLOCK: *conv = CONV_BC4; *px = 1; return VK_FORMAT_R8_UNORM;
        case VK_FORMAT_BC4_SNORM_BLOCK: *conv = CONV_BC4S; *px = 1; return VK_FORMAT_R8_SNORM;
        case VK_FORMAT_BC5_UNORM_BLOCK: *conv = CONV_BC5; *px = 2; return VK_FORMAT_R8G8_UNORM;
        case VK_FORMAT_BC5_SNORM_BLOCK: *conv = CONV_BC5S; *px = 2; return VK_FORMAT_R8G8_SNORM;
        case VK_FORMAT_BC6H_UFLOAT_BLOCK: *conv = CONV_BC6H; *px = 8; return VK_FORMAT_R16G16B16A16_SFLOAT;
        case VK_FORMAT_BC6H_SFLOAT_BLOCK: *conv = CONV_BC6HS; *px = 8; return VK_FORMAT_R16G16B16A16_SFLOAT;
        case VK_FORMAT_BC7_UNORM_BLOCK: *conv = CONV_BC7; *px = 4; return VK_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_BC7_SRGB_BLOCK: *conv = CONV_BC7; *px = 4; return VK_FORMAT_R8G8B8A8_SRGB;
        default: *conv = CONV_NONE; *px = 0; return f;
    }
}
// Decode a w×h (texels) region of BC blocks (rows of src_pitch bytes) into px-byte texels (rows of dst_pitch bytes); edge blocks are clipped
static void bc_decode(int conv, const uint8_t *src, size_t src_pitch, uint32_t w, uint32_t h, uint8_t *dst, size_t dst_pitch, int px) {
    if (conv == CONV_U16H || conv == CONV_S16H) {   // 16-bit normalized texels as halves (n16_half): rows of w texels of px bytes
        for (uint32_t y = 0; y < h; y++) {
            const uint8_t *s = src + y * src_pitch; uint8_t *d = dst + y * dst_pitch;
            for (size_t i = 0; i < (size_t)w * px / 2; i++) {
                uint16_t q; memcpy(&q, s + 2 * i, 2);
                float f = conv == CONV_U16H ? q / 65535.0f : ((int16_t)q < -32767 ? -1.0f : (int16_t)q / 32767.0f);
                uint16_t hv = f2h(f); memcpy(d + 2 * i, &hv, 2);
            }
        }
        return;
    }
    size_t bsz = conv == CONV_BC1 || conv == CONV_BC4 || conv == CONV_BC4S ? 8 : 16;
    uint8_t blk[4 * 4 * 8];
    for (uint32_t by = 0; by < (h + 3) / 4; by++) for (uint32_t bx = 0; bx < (w + 3) / 4; bx++) {
        const uint8_t *b = src + by * src_pitch + bx * bsz; int bp = 4 * px;
        switch (conv) {
            case CONV_BC1: bcdec_bc1(b, blk, bp); break;
            case CONV_BC2: bcdec_bc2(b, blk, bp); break;
            case CONV_BC3: bcdec_bc3(b, blk, bp); break;
            case CONV_BC4: bcdec_bc4(b, blk, bp, 0); break;
            case CONV_BC4S: bcdec_bc4(b, blk, bp, 1); break;
            case CONV_BC5: bcdec_bc5(b, blk, bp, 0); break;
            case CONV_BC5S: bcdec_bc5(b, blk, bp, 1); break;
            case CONV_BC6H: case CONV_BC6HS: {   // RGB half → RGBA half (alpha 1.0)
                uint16_t rgb[16 * 3]; bcdec_bc6h_half(b, rgb, 4 * 3, conv == CONV_BC6HS);
                for (int i = 0; i < 16; i++) { uint16_t *o = (uint16_t *)(void *)(blk + i * 8); o[0] = rgb[3 * i]; o[1] = rgb[3 * i + 1]; o[2] = rgb[3 * i + 2]; o[3] = 0x3C00; }
                break; }
            case CONV_BC7: bcdec_bc7(b, blk, bp); break;
        }
        for (uint32_t y = 0; y < 4 && by * 4 + y < h; y++) {
            uint32_t n = w - bx * 4 < 4 ? w - bx * 4 : 4;
            memcpy(dst + (size_t)(by * 4 + y) * dst_pitch + (size_t)bx * 4 * px, blk + y * bp, (size_t)n * px);
        }
    }
}

// ---------------- Textures ----------------
typedef struct {
    uint64_t serial; VkImage img; VkDeviceMemory mem; VkFormat fmt, nvfmt; VkImageAspectFlags aspect; int conv, px;
    const void *pool; const uint8_t *store; nvn_texdesc desc;   // what it was made from: a serial met again with other storage or another description is made anew
    uint32_t w, h, d, layers, levels; VkImageType type; VkSampleCountFlagBits samples;
    int pkg; uint64_t stamp, seen; const void *span; uint8_t *edge[2]; uint32_t edge_n[2];
    uint64_t rt_last; uint32_t rt_runs;   // the last present (+1) that drew into it as a pass's target, and in how many presents in a row up to it (gfx_vk_draw.inc rt_note)
    int refs, shared, mutable_, fixed_fmt; uint64_t used;   // mutable_: views in another format of the same texel size (a texture view's format); fixed_fmt: only its format and its sRGB twin (vt_create)   // texture objects (serials) resolving to it; in the sharing table; g_fence at its last lookup
    uint64_t mem_bytes, last_present, last_w; uint8_t gpu_w, filled, pool_read, evict;   // eviction (gfx_vk_evict.inc): its memory's size, the presents of its last lookup and its last write by the GPU, written by the GPU since it was made, filled from its storage, read through the texture pool by ID, chosen
    uint8_t taken;   // an uncompressed 2D texture taken from its storage at its first use (storage_first_use): the GPU's first write into it makes it a GPU texture (vt_get_w)
    uint32_t mem_type;   // v->mem's memory type (gfx_vk_spare.inc: a freed image's memory kept as a spare)
    VkImageUsageFlags usage; int ext_store;   // its usage; ext_store: STORAGE for views of another format only (EXTENDED_USAGE, vt_storage_up), so each view takes the usage its format has (view_get)
    uint8_t rbx;   // NVN RGBA4 / RGB5 / RGB5A1 / RGB565: the image holds the Vulkan format's order and guest bytes are exchanged where they cross (rb16.h: red's bit, 4 for RGBA4); 0: as they are
    uint8_t ds_copied;   // a depth image a copy wrote since its last render pass (copy_t2t; gfx_vk_dscopy.inc copied_depth_pass)
} vtex;
static nvn_hmap g_tex = {.klen = 8}, g_cpuw = {.klen = 8};   // serial → vtex; serials the CPU wrote (WriteTexels). Keyed by the 8-byte serial
static uint64_t g_pkg_uploads, g_pkg_reuploads, g_pkg_edge_skips, g_up_bytes, g_draws_skipped;
static unsigned tex_layers(const nvn_texdesc *d) { return d->target == 4 || d->target == 9 || d->target == 6 ? (d->depth ? d->depth : 1) : d->target == 8 ? 6 : d->target == 3 ? (d->height ? d->height : 1) : 1; }
static VkImageAspectFlags aspect_of(VkFormat f) {
    switch (f) {
        case VK_FORMAT_D16_UNORM: case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_X8_D24_UNORM_PACK32: return VK_IMAGE_ASPECT_DEPTH_BIT;
        case VK_FORMAT_S8_UINT: return VK_IMAGE_ASPECT_STENCIL_BIT;
        case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT: case VK_FORMAT_D16_UNORM_S8_UINT: return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        default: return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}
// Linear textures (LINEAR / LINEAR_RENDER_TARGET) are the game's memory as it writes it from the CPU: Super Mario Bros. Wonder's glyph cache (2048×8192 R8, IMAGE |
// LINEAR, its font engine rasterizes glyphs into it on the CPU) and sky colour ramps (64×1 RGBA16F written every frame); as GPU-only images they stayed empty and every
// menu and dialog text was blank (nvn_mtl.m tex_linear makes them on the pool memory, zero copy). Here they go the packaged way: uploaded from guest memory, again when
// its pages were written (rows at the texture's own stride: vt_upload)
static int is_linear(const tex_t *t) { return (t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) != 0; }
static int is_packaged(const tex_t *t) { return t->mem && (t->has_layout || is_linear(t) || nvn_hm_get(&g_cpuw, &t->serial)); }
void vkb_frame_forget(uint64_t serial);
// g_vt_gen: bumped (under g_bk) whenever a vtex is freed or a serial stops resolving to the vtex it did (vt_destroy, the g_tex deletions): a draw's textures resolved in
// stage_textures are used again in stage_set only while it has not moved (gfx_vk_draw.inc tex_once)
static uint64_t g_vt_gen = 1;
static uint64_t g_im_live, g_im_n;   // the textures' image memory alive and their count (under g_bk; the periodic line reads them as they are)
static void im_in(vtex *v, uint64_t b) { v->mem_bytes = b; g_im_live += b; g_im_n++; }
static void im_out(vtex *v) { if (v->mem_bytes) { g_im_live -= v->mem_bytes; g_im_n--; v->mem_bytes = 0; } }
#include "gfx_vk_spare.inc"   // a freed image's memory kept as a spare (DM_VK_IMAGE_SPARE)
static void vt_destroy(vtex *v) {   // with the replay's lock held; the front end may be drawing the image: it is taken off the presented frame first (vkb_frame_forget waits for that)
    if (!v) return;
    g_vt_gen++;
    vkb_frame_forget(v->serial);
    if (v->img) views_drop(v->img);   // the image, its views and framebuffers go when the current chunk completes: recorded and submitted commands may use them
    dead_add(3, (uint64_t)v->img); vt_mem_drop(v);
    free(v->edge[0]); free(v->edge[1]); free(v);
}
static void vt_clear_new(vtex *v) {   // fresh Switch memory reads as 0
    VkCommandBuffer c = cb(); if (!c) return;
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = v->img,
        .subresourceRange = {v->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkImageSubresourceRange rg = {v->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    if (v->aspect == VK_IMAGE_ASPECT_COLOR_BIT) {
        VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(g_vk.pd, v->fmt, &fp);
        int compressed = v->fmt >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && v->fmt <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK;
        if (!compressed) { VkClearColorValue z = {{0}}; vkCmdClearColorImage(c, v->img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &rg); }
    } else { VkClearDepthStencilValue z = {0, 0}; vkCmdClearDepthStencilImage(c, v->img, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &rg); }
    cb_barrier();
}
static nvn_hmap g_img_refused = {.klen = 8};   // serials whose image was over DM_VK_IMAGE_MAX_MB (vt_create; g_bk held)
static int fmt_bytes(VkFormat f);   // gfx_vk_draw.inc
// The sRGB form of an 8-bit UNORM colour format and back (0: none), the one other format a colour image is viewed in under DM_VK_UBWC (vt_create)
static VkFormat srgb_twin(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_SRGB; case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_SRGB; case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
        case VK_FORMAT_A8B8G8R8_UNORM_PACK32: return VK_FORMAT_A8B8G8R8_SRGB_PACK32; case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
        case VK_FORMAT_R8_UNORM: return VK_FORMAT_R8_SRGB; case VK_FORMAT_R8_SRGB: return VK_FORMAT_R8_UNORM;
        case VK_FORMAT_R8G8_UNORM: return VK_FORMAT_R8G8_SRGB; case VK_FORMAT_R8G8_SRGB: return VK_FORMAT_R8G8_UNORM;
        default: return VK_FORMAT_UNDEFINED;
    }
}
// DM_VK_UBWC=0 (env.txt): colour images MUTABLE_FORMAT with no format list, as before (vt_create)
static int ubwc_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_UBWC"); v = !(e && *e == '0') && g_fmt_list; LOG("[vk] colour images %s", v ? "list their format and its sRGB twin, so Adreno keeps their framebuffer compression (DM_VK_UBWC=0: MUTABLE_FORMAT, any format of the texel size)" : g_fmt_list ? "MUTABLE_FORMAT with no format list (DM_VK_UBWC=0)" : "MUTABLE_FORMAT with no format list (no VK_KHR_image_format_list)"); } return v; }
static vtex *vt_create(const tex_t *t) {
    uint32_t m = 0; uint8_t swz[4];
    if (nvn_hm_get(&g_img_refused, &t->serial)) return NULL;
    if (!nvn_fmt_to_mtl(t->d.format, &m, swz) || !m) { RED_ONCE(10, "texture format %#x has no Vulkan counterpart yet (first case ser=%llu)", t->d.format, (unsigned long long)t->serial); return NULL; }
    // a buffer texture gets an image only as rows (tbuf_rows.h), asked for by a 2D slot that may read one so (vt_get_s): its other reads are texel buffers (gfx_vk_draw.inc tbuf_view)
    int rows = t->d.target == NVN_TEXTURE_TARGET_BUFFER; uint32_t rw = 0, rh = 0;
    if (rows) { int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
        if (bpb <= 0 || bw != 1 || bh != 1 || !tbr_shape(t->d.width, g_vk.props.limits.maxImageDimension2D, &rw, &rh)) {
            RED_ONCE(11, "buffer texture of %u elements of format %#x has no rows of %u texels on this GPU (largest 2D image %u): the slot reads 0", (unsigned)t->d.width, t->d.format, TBR_ROW, g_vk.props.limits.maxImageDimension2D); return NULL; } }
    VkFormat nf = vk_of_mtl(m), f = nf; int conv = CONV_NONE, px = 0;
    if (nf == VK_FORMAT_UNDEFINED) { RED_ONCE(12, "Metal format %u not mapped to Vulkan (NVN format %#x)", m, t->d.format); return NULL; }
    if (nf >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && nf <= VK_FORMAT_BC7_SRGB_BLOCK && !g_vk.has_bc) f = bc_target(nf, &conv, &px);
    else if (!rows && !is_linear(t) && !(t->d.flags & NVN_TEXTURE_FLAGS_IMAGE) && t->d.samples <= 1) { VkFormat h = n16_half(nf, &conv, &px); if (h) f = h; }   // not a storage image: shaders declare those formats (rows: fetched, their bits as they are)
    // rows of 32-bit integers are held as the float format of their size: the translator declares every texture float, and KTGL's light and decal lists (32-bit integer buffer
    // textures) are fetched through such a slot and their bits taken back from the result (nvn_mtl.m tbuf_view_format). A float image hands its bits over as they are; an
    // integer image read through a float declaration is undefined in Vulkan
    if (rows) switch (f) { case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: f = VK_FORMAT_R32_SFLOAT; break; case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT: f = VK_FORMAT_R32G32_SFLOAT; break;
                           case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT: f = VK_FORMAT_R32G32B32A32_SFLOAT; break; default: break; }
    if (nf >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && nf <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK && !g_vk.has_astc) { RED_ONCE(13, "ASTC texture on a GPU without ASTC: not decoded yet"); return NULL; }
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(g_vk.pd, f, &fp);
    VkFormatFeatureFlags ff = fp.optimalTilingFeatures;
    if (f == VK_FORMAT_D32_SFLOAT_S8_UINT && !(ff & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {   // Depth32F_S8 → D24S8 where the GPU has no D32S8 attachment
        VkFormatProperties f2; vkGetPhysicalDeviceFormatProperties(g_vk.pd, VK_FORMAT_D24_UNORM_S8_UINT, &f2);
        if (f2.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) { f = VK_FORMAT_D24_UNORM_S8_UINT; ff = f2.optimalTilingFeatures; }
    }
    if (!(ff & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) && !(ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) { RED_ONCE(14, "format %d not usable as an image on this GPU (NVN format %#x)", f, t->d.format); return NULL; }
    if (rows && (!(ff & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) || !(ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))) { RED_ONCE(197, "buffer texture format %d (NVN %#x) can't be filled and sampled as an image here: its rows read 0", f, t->d.format); return NULL; }
    vtex *v = calloc(1, sizeof *v); if (!v) return NULL;
    v->serial = t->serial; v->pool = t->pool; v->store = t->mem; v->desc = t->d; v->fmt = f; v->nvfmt = nf; v->conv = conv; v->px = px; v->aspect = aspect_of(f);
    v->rbx = (uint8_t)rb16_shift(m, swz);   // the swizzle's red / blue exchange: in the bits, so what the GPU draws into the image reads as what it uploads
    if (v->rbx && t->d.format >= 0x39 && t->d.format <= 0x3C) {   // once per format: which games take the exchange (a release sweep greps for it)
        static _Atomic uint32_t told; uint32_t bit = 1u << (t->d.format - 0x39); static const char *const nm[4] = {"RGBA4", "RGB5", "RGB5A1", "RGB565"};
        if (!(atomic_fetch_or(&told, bit) & bit)) LOG("[vk] first NVN %s texture (format %#x, %ux%u ser=%llu): held in VkFormat %d's order, its bits exchanged where they cross guest memory (rb16.h)",
                                                      nm[t->d.format - 0x39], (unsigned)t->d.format, (unsigned)t->d.width, (unsigned)t->d.height, (unsigned long long)t->serial, (int)f);
    }
    v->type = t->d.target == 0 || t->d.target == 3 ? VK_IMAGE_TYPE_1D : t->d.target == 2 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    v->w = t->d.width ? t->d.width : 1; v->h = v->type == VK_IMAGE_TYPE_1D ? 1 : (t->d.height ? t->d.height : 1); v->d = v->type == VK_IMAGE_TYPE_3D ? (t->d.depth ? t->d.depth : 1) : 1;
    v->layers = tex_layers(&t->d); v->levels = t->d.levels ? t->d.levels : 1;
    if (rows) { v->type = VK_IMAGE_TYPE_2D; v->w = rw; v->h = rh; v->d = 1; v->layers = 1; v->levels = 1;
        static int nl; if (nl++ < 4) LOG("[vk] buffer texture read through a 2D slot: an image of %ux%u rows of %u texels (ser=%llu, %u elements of format %#x; vt_get_s, tbuf_rows.h)", rw, rh, TBR_ROW,
                                         (unsigned long long)t->serial, (unsigned)t->d.width, t->d.format); }
    v->samples = t->d.samples > 1 ? (VkSampleCountFlagBits)t->d.samples : VK_SAMPLE_COUNT_1_BIT;
    VkImageUsageFlags u = 0;
    if (ff & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT) u |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (ff & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) u |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) u |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (ff & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) u |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (ff & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) u |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if ((t->d.flags & NVN_TEXTURE_FLAGS_IMAGE) && (ff & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) u |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (rows) u &= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;   // only read: written by its uploads alone (vt_get_s keeps it from targets and image slots)
    v->usage = u;   // STORAGE added later if the texture is bound to an image slot all the same (vt_storage_up)
    // an uncompressed colour image may be viewed in another format of its texel size (NVN texture views: sRGB over UNORM and the like): mutable. So may a BC image
    // decoded on the CPU, which holds uncompressed texels: a BC7_SRGB view of a BC7 texture is its RGBA8 image viewed as RGBA8_SRGB (gfx_vk_draw.inc view_format)
    int mut = !rows && v->aspect == VK_IMAGE_ASPECT_COLOR_BIT && !(f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK);
    // MUTABLE_FORMAT alone lets a view take any format of the texel size, and Adreno then leaves the image without UBWC, its framebuffer compression: every target
    // stored and loaded uncompressed. nx2android (github.com/shirai91/nx2android, a1b4f54) found that a format list keeps it: Super Mario Bros. Wonder's GPU time
    // 11.8 -> 8.8 ms a frame on an Adreno 830. A colour image that is not a storage image now lists its format and its sRGB twin (MUTABLE_FORMAT only with a twin);
    // a view in any other format reads the image's own (view_format, a red line). Storage images keep MUTABLE_FORMAT: a shader declares their format (storage_view)
    VkFormat fl[2] = {f, srgb_twin(f)};
    VkImageFormatListCreateInfoKHR fli = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO_KHR, .viewFormatCount = fl[1] ? 2 : 1, .pViewFormats = fl};
    if (mut && !(u & VK_IMAGE_USAGE_STORAGE_BIT) && ubwc_on()) { v->fixed_fmt = 1; mut = fl[1] != VK_FORMAT_UNDEFINED; }
    v->mutable_ = mut || (u & VK_IMAGE_USAGE_STORAGE_BIT);
    VkImageCreateInfo ic = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = v->type, .format = f, .extent = {v->w, v->h, v->d}, .mipLevels = v->levels,
        .arrayLayers = v->layers, .samples = v->samples, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = u, .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .flags = ((t->d.target == 8 || t->d.target == 9) && v->layers % 6 == 0 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0) |
                                                           (u & VK_IMAGE_USAGE_STORAGE_BIT || mut ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0) |   // storage: viewed in the format a shader declares (gfx_vk_draw.inc storage_view); colour: a texture view's format
                                                           (v->type == VK_IMAGE_TYPE_3D && (u & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) ? VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT : 0)};   // a 3D target is drawn a slice at a time through 2D views (gfx_vk_draw.inc rp_start)
    if (v->samples > 1) ic.mipLevels = 1;
    if (v->fixed_fmt && mut) ic.pNext = &fli;
    if (vkCreateImage(g_vk.dev, &ic, NULL, &v->img) != VK_SUCCESS) { RED_ONCE(15, "vkCreateImage %ux%ux%u ×%u levels %u format %d failed", v->w, v->h, v->d, v->layers, v->levels, f); free(v); return NULL; }
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(g_vk.dev, v->img, &mr);
    if ((u & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) && v->aspect == VK_IMAGE_ASPECT_COLOR_BIT && !conv && v->w >= 256 && v->h >= 256 && v->levels == 1 && v->layers == 1 && v->d == 1 && v->samples == 1) {
        // a compressed target needs room for its metadata beside the texels: the size says whether the driver kept UBWC (above)
        static int n; uint64_t raw = (uint64_t)v->w * v->h * (uint64_t)fmt_bytes(f);
        if (raw && n++ < 6) LOG("[vk] colour target %ux%u format %d%s: %.2f MB of memory for %.2f MB of texels", v->w, v->h, f, v->fixed_fmt ? " (format list)" : mut ? " (MUTABLE_FORMAT)" : "", mr.size / 1048576.0, raw / 1048576.0);
    }
    // A phone shares its RAM with the GPU, and the replay makes every texture whole: Mario Kart World's terrain arrays (8192x8192x68, 5.8 GB in BC6H, 1.45 GB in ASTC 8x8), sparse in a
    // virtual pool on the Switch 2 with a fraction of their pages mapped, took the Ayn Odin 2's 11.5 GB until the low-memory killer ended the game, as each failed lookup made the image again.
    // An image over DM_VK_IMAGE_MAX_MB (1024 by default; 0: no limit), or one in a virtual pool over DM_VK_SPARSE_MAX_MB (256 by default: the console maps only part of such a texture, and
    // Mario Kart World's 341 and 382 MB ones in virtual pools, made whole beside the arrays, still ended it after 75 s), is refused once and for all: the texture reads as missing and
    // the game goes on. Partially resident images (the driver has sparse residency for 2D images) would be the fix
    if (mr.size >= (256ull << 20)) {
        static int once; if (!once) { once = 1; VkPhysicalDeviceFeatures pf; vkGetPhysicalDeviceFeatures(g_vk.pd, &pf);
            LOG("[vk] sparse binding %u, residency buffer %u, image 2D %u, image 3D %u, aliased %u (what partially resident images would need)", pf.sparseBinding, pf.sparseResidencyBuffer, pf.sparseResidencyImage2D, pf.sparseResidencyImage3D, pf.sparseResidencyAliased); }
        static long max_mb = -1, sparse_mb = -1;
        if (max_mb < 0) { const char *e = getenv("DM_VK_IMAGE_MAX_MB"), *sp = getenv("DM_VK_SPARSE_MAX_MB"); max_mb = e ? atol(e) : 1024; sparse_mb = sp ? atol(sp) : 256; }
        long lim = t->pool && (t->pool->flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL) && sparse_mb > 0 && (sparse_mb < max_mb || max_mb <= 0) ? sparse_mb : max_mb;
        int refuse = lim > 0 && mr.size > (uint64_t)lim << 20;
        static int n; if (n++ < 32) LOG("%s[vk] large image ser=%llu %ux%ux%u x%u layers, %u levels, format %d (NVN %#x%s): %.0f MB%s", refuse ? "[RED] " : "", (unsigned long long)t->serial, v->w, v->h, v->d, v->layers, v->levels, f, t->d.format,
                                        t->pool && (t->pool->flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL) ? ", in a virtual pool" : "", mr.size / 1048576.0, refuse ? ": over the image limit (DM_VK_IMAGE_MAX_MB, DM_VK_SPARSE_MAX_MB in a virtual pool), not made (the texture reads as missing)" : "");
        if (refuse) { nvn_hm_put(&g_img_refused, &t->serial, (void *)1); vkDestroyImage(g_vk.dev, v->img, NULL); free(v); return NULL; }
    }
    uint64_t msz = 0;
    if (!(v->mem = img_mem(&mr, &msz, &v->mem_type))) { RED_ONCE(16, "image memory of %llu bytes failed", (unsigned long long)mr.size); vkDestroyImage(g_vk.dev, v->img, NULL); free(v); return NULL; }
    vkBindImageMemory(g_vk.dev, v->img, v->mem, 0); im_in(v, msz);
    vt_clear_new(v);
    return v;
}
// A texture bound to an image slot that was made without storage use, as nvn_mtl.m's image_prepare upgrades one (tex_upgrade): the game left NVN's image flag out of its
// builder and registered it with RegisterTexture, and a shader loads or stores through it anyway. Ocarina of Time's title does it with a 1920x1080 RGBA8 target (a storage
// view of an image without STORAGE, which is invalid) and a 2560x1440 RGBA8_SRGB one (its dispatch skipped: sRGB is no storage format here).
// - The image is made again with STORAGE and MUTABLE_FORMAT, its levels and layers copied over, and swapped in under the same vtex: every serial and the sharing table keep it.
// - The old image, its views and framebuffers go when the chunk completes, as vt_destroy's do, and the presented frame lets go of it first.
// - A format the GPU cannot store to (sRGB) takes STORAGE for views in its UNORM twin alone (EXTENDED_USAGE, ext_store), which storage_view then makes.
// Under g_bk; cb() ends an open render pass. 0: it cannot be (a decoded BC texture, multisample, depth or stencil, no format of it the GPU stores to)
static uint64_t g_st_up;
// An E5B9G9R9 texture bound to an image slot is stored through an R32_UINT view, the shader packing each texel into RGB9E5 (spv_e5.h), and the texture keeps its format
// and its bits: on a GPU that stores to E5B9G9R9 too (Samsung's Xclipse), since the shader's stores are packed words (gfx_vk_draw.inc storage_view). DM_VK_E5_STORE=0 (env.txt): skipped, as before
static int e5_store_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_E5_STORE"); v = !(e && *e == '0'); if (!v) LOG("[vk] DM_VK_E5_STORE=0: an E5B9G9R9 texture bound to an image slot is skipped"); } return v; }
static int vt_storage_up(vtex *v) {
    if (v->usage & VK_IMAGE_USAGE_STORAGE_BIT) return 1;
    VkFormat f = v->fmt, sf = f; VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(g_vk.pd, f, &fp);
    VkImageCreateFlags cf = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | ((v->desc.target == 8 || v->desc.target == 9) && v->layers % 6 == 0 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0) |
                            (v->type == VK_IMAGE_TYPE_3D && (v->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) ? VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT : 0);   // as vt_create makes them
    int ext = 0;
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)) {   // EXTENDED_USAGE is Vulkan 1.1's
        VkFormatProperties tp = {0}; sf = srgb_twin(f); if (!sf && f == VK_FORMAT_E5B9G9R9_UFLOAT_PACK32 && e5_store_on()) sf = VK_FORMAT_R32_UINT;   // its views: R32_UINT, packed by the shader
        if (sf) vkGetPhysicalDeviceFormatProperties(g_vk.pd, sf, &tp);
        if (!sf || !(tp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) || (VK_API_VERSION_MAJOR(g_vk.props.apiVersion) == 1 && VK_API_VERSION_MINOR(g_vk.props.apiVersion) < 1)) sf = VK_FORMAT_UNDEFINED;
        else { ext = 1; cf |= VK_IMAGE_CREATE_EXTENDED_USAGE_BIT; }
    }
    VkImageUsageFlags u = v->usage | VK_IMAGE_USAGE_STORAGE_BIT; VkImageFormatProperties ip;
    const char *why = v->conv ? "a BC texture decoded on the CPU" : v->samples > 1 ? "multisample" : v->aspect != VK_IMAGE_ASPECT_COLOR_BIT ? "depth or stencil" : !sf ? "no format of it the GPU stores to" : NULL;
    if (!why && (vkGetPhysicalDeviceImageFormatProperties(g_vk.pd, f, v->type, VK_IMAGE_TILING_OPTIMAL, u, cf, &ip) != VK_SUCCESS || ip.maxExtent.width < v->w || ip.maxExtent.height < v->h
                 || ip.maxExtent.depth < v->d || ip.maxMipLevels < v->levels || ip.maxArrayLayers < v->layers)) why = "the GPU takes no storage image of its size and format";
    VkImage img = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkMemoryRequirements mr; uint64_t msz = 0; uint32_t mty = 0;
    VkImageCreateInfo ic = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = cf, .imageType = v->type, .format = f, .extent = {v->w, v->h, v->d}, .mipLevels = v->levels, .arrayLayers = v->layers,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = u, .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (!why && vkCreateImage(g_vk.dev, &ic, NULL, &img) != VK_SUCCESS) why = "vkCreateImage failed";
    if (!why) {
        vkGetImageMemoryRequirements(g_vk.dev, img, &mr);
        if (!(mem = img_mem(&mr, &msz, &mty))) why = "no memory for it"; else vkBindImageMemory(g_vk.dev, img, mem, 0);
    }
    VkCommandBuffer c = why ? VK_NULL_HANDLE : cb(); if (!why && !c) why = "no command buffer";
    if (why) {
        if (img) vkDestroyImage(g_vk.dev, img, NULL);
        if (mem) vkFreeMemory(g_vk.dev, mem, NULL);
        RED_ONCE(123, "texture ser=%llu %ux%u format %d, made without storage use, is bound to an image slot and cannot take it (%s): skipped", (unsigned long long)v->serial, v->w, v->h, f, why); return 0;
    }
    cb_barrier();   // what drew it before the copy reads it
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkImageCopy r[16]; uint32_t nr = 0;
    for (uint32_t l = 0; l < v->levels && nr < 16; l++)
        r[nr++] = (VkImageCopy){{VK_IMAGE_ASPECT_COLOR_BIT, l, 0, v->layers}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, v->layers}, {0, 0, 0},
                                {v->w >> l ? v->w >> l : 1, v->h >> l ? v->h >> l : 1, v->type == VK_IMAGE_TYPE_3D ? (v->d >> l ? v->d >> l : 1) : 1}};
    vkCmdCopyImage(c, v->img, VK_IMAGE_LAYOUT_GENERAL, img, VK_IMAGE_LAYOUT_GENERAL, nr, r);
    cb_barrier();
    vkb_frame_forget(v->serial);
    views_drop(v->img); dead_add(3, (uint64_t)v->img); vt_mem_drop(v);
    v->img = img; v->mem = mem; v->mem_type = mty; v->usage = u; v->ext_store = ext; v->mutable_ = 1; v->fixed_fmt = 0; g_vt_gen++; im_in(v, msz);
    if (g_st_up++ < 8) LOG("[vk] texture ser=%llu %ux%u format %d bound to an image slot without storage use: made again with it%s (%llu so far)", (unsigned long long)v->serial, v->w, v->h, f,
                           ext ? (f == VK_FORMAT_E5B9G9R9_UFLOAT_PACK32 ? ", stored through an R32_UINT view, RGB9E5 packed by the shader" : ", stored through its UNORM twin") : "", (unsigned long long)g_st_up);
    return 1;
}
// Upload a texture's guest storage (deswizzled on the CPU; BC decoded when the GPU lacks it). The dirty-page stamp is taken and the pages protected before reading (nvn_mtl.m pkg_upload_)
// texture uploads' CPU time (deswizzle, BC decode); pipelines made and their creation time: the long-frame lines (hle_nvn.c)
static _Atomic uint64_t g_up_ns, g_pso_made, g_pso_ns;
static _Atomic uint64_t g_mod_made, g_mod_ns, g_cpso_made, g_cpso_ns;   // shader modules (variants too) and compute pipelines made, and their creation time (gfx_vk_draw.inc): the driver compiles in both
static int vt_upload_(vtex *v, const tex_t *t, int track);
// track 0: a one-time fill (tex_init_fill), with no write tracking: the pages are not protected and no dirty span is kept
static int vt_upload(vtex *v, const tex_t *t, int track) { uint64_t t0 = clock_gettime_nsec_np(CLOCK_MONOTONIC); int r = vt_upload_(v, t, track); atomic_fetch_add(&g_up_ns, clock_gettime_nsec_np(CLOCK_MONOTONIC) - t0); return r; }
// The upload's sync (DM_VK_UP_RANGE=1; off by default, see DM_VK_LINWB_SNAP: cpu_view). cpu_view waits for every pending write-back and queued copy wherever it writes, and with a linear render target
// written back in nearly every chunk Breath of the Wild's uploads drained the GPU 2.1 to 2.65 times a frame (Ayn Odin 2: 1.49 submitted + 0.64 in flight a frame in one dump, 1.51 + 1.14
// in another run). An upload reads only its own storage: it waits only for a pending write overlapping the bytes it reads (chunk_hits: write-backs' and queued copies' destinations),
// the current chunk submitted and waited for, or the newest overlapping chunk in flight waited for (they complete in order). The open render pass is ended first, as the cb() below
// would end it, so that its targets' write-backs (lin_writeback) and shadow copies are queued and seen: a dirty texture sampled while it is an attachment of the open pass was read
// before that pass's write-back existed. Counted at the upload site (submitted / in flight); g_up_free: the uploads that waited for nothing
static uint64_t g_up_free;
static int up_range_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_UP_RANGE"); v = e && !strcmp(e, "1"); if (v) LOG("[vk] DM_VK_UP_RANGE=1: a texture upload waits only for pending writes it reads (off by default: the grass flicker, see DM_VK_LINWB_SNAP)"); } return v; }
static void up_view(const vtex *v, const tex_t *t) {
    if (!up_range_on() || vk_safe()) { g_vs = 2; cpu_view(); return; }
    rp_end();
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh); size_t n = t->L.size;
    if (bpb && !v->conv && is_linear(t)) {   // the rows vt_upload_ reads, at the stride clamped up to the pitch: possibly past the storage size
        size_t pitch = (size_t)((v->w + bw - 1) / bw) * bpb, rows = (v->h + bh - 1) / bh, srow = t->L.layer_size / (rows ? rows : 1);
        size_t e = (size_t)(v->layers - 1) * t->L.layer_size + srow * (rows ? rows - 1 : 0) + pitch; if (srow >= pitch && e > n) n = e; }   // a shorter stride reads within the storage (vt_upload_)
    uintptr_t lo = (uintptr_t)t->mem, hi = lo + n; uint64_t seq = 0;
    if (C && chunk_hits(C, lo, hi, 0)) { g_vsite[2][0]++; submit_wait(); return; }
    if (atomic_load(&g_inflight_wb) || atomic_load(&g_inflight_cop)) {
        pthread_mutex_lock(&g_cmu); for (int f = 0; f < g_fifo_n; f++) if (chunk_hits(g_fifo[f], lo, hi, 0)) seq = g_fifo[f]->seq; pthread_mutex_unlock(&g_cmu); }
    if (seq) { g_vsite[2][1]++; seq_wait(seq); return; }
    g_up_free++;
}
static int vt_upload_(vtex *v, const tex_t *t, int track) {
    up_view(v, t);
    if (track) { v->stamp = nvn_dirty_now(); nvn_dirty_protect(t->mem, t->L.size); }
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (!bpb) { RED_ONCE(20, "packaged texture format %#x has no block size", t->d.format); return 0; }
    size_t total = 0;
    for (unsigned l = 0; l < v->levels; l++) {
        uint32_t w = v->w >> l ? v->w >> l : 1, h = v->h >> l ? v->h >> l : 1, z = v->type == VK_IMAGE_TYPE_3D ? (v->d >> l ? v->d >> l : 1) : 1;
        size_t pitch = v->conv ? (size_t)w * v->px : (size_t)((w + bw - 1) / bw) * bpb, rows = v->conv ? h : (h + bh - 1) / bh;
        total += pitch * rows * z * v->layers;
    }
    stg_sl s = stg_take(total, v->conv ? v->px : bpb); if (!s.p) return 0;
    VkCommandBuffer c = cb(); if (!c) return 0;   // may end an open pass: lin_writeback takes staging too (s stays valid: stg_take)
    uint8_t *tmp = NULL; size_t off = 0;
    for (unsigned l = 0; l < v->levels; l++) {
        uint32_t w = v->w >> l ? v->w >> l : 1, h = v->h >> l ? v->h >> l : 1, z = v->type == VK_IMAGE_TYPE_3D ? (v->d >> l ? v->d >> l : 1) : 1;
        size_t pitch = (size_t)((w + bw - 1) / bw) * bpb, rows = (h + bh - 1) / bh;
        for (unsigned y = 0; y < v->layers; y++) {
            size_t out = v->conv ? (size_t)w * v->px * h * z : pitch * rows * z;
            if (v->conv) {
                tmp = realloc(tmp, pitch * rows * z);
                if (nvn_deswizzle(&t->d, l, y, t->mem, tmp, pitch)) { RED_ONCE(21, "deswizzle failed format %#x target %d", t->d.format, t->d.target); free(tmp); return 0; }
                for (uint32_t k = 0; k < z; k++) bc_decode(v->conv, tmp + k * pitch * rows, pitch, w, h, s.p + off + (size_t)k * w * v->px * h, (size_t)w * v->px, v->px);
            } else if (t->d.target == NVN_TEXTURE_TARGET_BUFFER) tbr_fill(s.p + off, out, t->mem, t->L.size);   // rows of 16384 texels (vt_create): the buffer's bytes as they lie
            else if (is_linear(t)) {   // linear: block rows at the texture's stride (its layer's bytes over its block rows, as GetStorageSize gives them)
                if (l || z > 1) { RED_ONCE(22, "linear texture with levels or depth: only level 0 of a 2D one is read"); off += out; continue; }
                size_t srow = t->L.layer_size / (rows ? rows : 1); const uint8_t *src = t->mem + (size_t)y * t->L.layer_size;
                if (srow < pitch) {   // a stride shorter than the row (Skyrim 1.1.404's 1280x720 RGBA8 of 3840-byte rows): each row runs on into the next one's bytes, as the GPU reads it,
                                      // and what would lie past the storage's end reads as 0 (it was read at the row's length instead, sheared, and 0.9 MB past the storage)
                    size_t left = t->L.size > (size_t)y * t->L.layer_size ? t->L.size - (size_t)y * t->L.layer_size : 0;
                    for (size_t r = 0; r < rows; r++) { size_t at = r * srow, k = at < left ? (left - at < pitch ? left - at : pitch) : 0;
                        memcpy(s.p + off + r * pitch, src + at, k); memset(s.p + off + r * pitch + k, 0, pitch - k); }
                } else for (size_t r = 0; r < rows; r++) memcpy(s.p + off + r * pitch, src + r * srow, pitch);
            } else if (nvn_deswizzle(&t->d, l, y, t->mem, s.p + off, pitch)) { RED_ONCE(21, "deswizzle failed format %#x target %d", t->d.format, t->d.target); free(tmp); return 0; }
            if (v->rbx) rb16_rows(s.p + off, out, 1, out, v->rbx);   // the guest's order into the image's (rb16.h)
            VkBufferImageCopy r = {.bufferOffset = s.off + off, .imageSubresource = {v->aspect, l, y, 1}, .imageExtent = {w, h, z}};
            vkCmdCopyBufferToImage(c, s.b, v->img, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
            off += out;
        }
    }
    free(tmp); cb_barrier();
    if (vk_debug()) LOG("[vk] upload ser=%llu mem=%p size %#llx levels %u layers %u total %zu first bytes %02x %02x %02x %02x", (unsigned long long)v->serial, t->mem, (unsigned long long)t->L.size, v->levels, v->layers, total, s.p[0], s.p[1], s.p[2], s.p[3]);
    g_up_bytes += total;
    if (track) { v->span = nvn_dirty_span(t->mem, t->L.size); v->seen = v->stamp; }
    return 1;
}
// A new GPU texture's initial content (nvn_mtl.m tex_init_fill): until the GPU first uses a texture it has not written it, so what its storage holds is its content when the
// CPU cannot write that pool (CPU_NO_ACCESS: filled from files), cannot cache it (CPU_UNCACHED), or flushed part of it before this first use (nvn_flushed, hle_nvn.c).
// Breath of the Wild copies its sky scattering tables (System/KSys/sky.skybin, a 256x32x16 RGBA16F 3D texture and two 2D ones) into CPU_CACHED pools with memcpy and
// flushes them: as images of zeros the sky and the distant haze came out murky olive instead of blue on an Ayn Odin 2, as on the Mac before (seedds/nxnative issue #52)
int nvn_flushed(const void *p, uint64_t n);   // hle_nvn.c
static int mem_any(const uint8_t *p, uint64_t n) {   // whether any byte is non-zero (stops at the first)
    uint64_t i = 0, w;
    for (; i + 8 <= n; i += 8) { memcpy(&w, p + i, 8); if (w) return 1; }
    for (; i < n; i++) if (p[i]) return 1;
    return 0;
}
// DM_VK_FILL_CHECK (env.txt, diagnostic): an image read back right after its upload (level 0, every slice), counting what landed; the readback buffer starts as 0xAB
// so a copy that never ran shows as untouched bytes
static void fill_check(vtex *v, const tex_t *t, const char *what) {
    if (!getenv("DM_VK_FILL_CHECK") || v->levels != 1 || v->layers != 1 || v->conv) return;
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh); VkDeviceSize sz = (VkDeviceSize)((v->w + bw - 1) / bw) * ((v->h + bh - 1) / bh) * v->d * bpb; stg_t chk;
    if (!bpb || !stg_new(&chk, sz)) return;
    memset(chk.p, 0xAB, (size_t)sz);
    VkCommandBuffer c = cb();
    if (c) { VkBufferImageCopy r = {.imageSubresource = {v->aspect, 0, 0, 1}, .imageExtent = {v->w, v->h, v->d}}; vkCmdCopyImageToBuffer(c, v->img, VK_IMAGE_LAYOUT_GENERAL, chk.b, 1, &r);
        cb_barrier(); submit_wait();
        uint64_t got = 0, ab = 0; for (VkDeviceSize i = 0; i < sz; i++) { got += chk.p[i] != 0; ab += chk.p[i] == 0xAB; }
        LOG("[vk] fill check (%s): ser=%llu %ux%ux%u vk%d image %#llx read back at once: %llu of %llu bytes non-zero, %llu still 0xAB", what, (unsigned long long)t->serial, v->w, v->h, v->d, v->fmt,
            (unsigned long long)(uint64_t)v->img, (unsigned long long)got, (unsigned long long)sz, (unsigned long long)ab); }
    stg_free(&chk);
}
static int tex_init_fill(vtex *v, const tex_t *t) {
    if (!t->mem || !t->pool || (t->pool->flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL) || t->has_layout || !t->L.size) return 0;
    if (t->d.samples > 1 || v->aspect != VK_IMAGE_ASPECT_COLOR_BIT) return 0;   // depth and stencil: the GPU's own
    int no_cpu = (t->pool->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS) != 0, uncached = (t->pool->flags & NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED) != 0;
    if (no_cpu && (t->d.flags & NVN_TEXTURE_FLAGS_COMPRESSIBLE)) return 0;   // in a pool the CPU never writes, a compressible texture's memory may mean nothing to the CPU (nvn_mtl.m)
    static int flush_fill = -1; if (flush_fill < 0) { const char *e = getenv("DM_NVN_FLUSH_FILL"); flush_fill = !e || atoi(e) != 0; }
    if (!no_cpu && !uncached && (!flush_fill || !nvn_flushed(t->mem, t->L.size))) return 0;
    if (!mem_any(t->mem, t->L.size) || !vt_upload(v, t, 0)) return 0;
    int why = no_cpu ? 0 : uncached ? 1 : 2;
    static uint64_t n[3]; if (n[why]++ < 4) { uint64_t nz = 0; for (uint64_t i = 0; i < t->L.size; i++) nz += t->mem[i] != 0;
        LOG("[vk] initial content: ser=%llu %ux%ux%u format %#x filled from its storage (%llu of %llu bytes non-zero), %s (%llu so far)", (unsigned long long)t->serial, t->d.width, t->d.height, t->d.depth, t->d.format,
            (unsigned long long)nz, (unsigned long long)t->L.size, (const char *[]){"a CPU_NO_ACCESS pool's", "a CPU_UNCACHED pool's", "flushed by the CPU before its first use"}[why], (unsigned long long)n[why]); }
    fill_check(v, t, "fill");
    return 1;
}
// Writes beside a texture (nvn_mtl.m pkg_edge_ranges / pkg_edges_take / pkg_edges_clean): the texture's bytes in the host pages it shares with other data
static void edge_ranges(const tex_t *t, uintptr_t r[2][2], uintptr_t *ia, uintptr_t *ie) { edge_rng((uintptr_t)t->mem, (uintptr_t)t->mem + t->L.size, r, ia, ie); }
static void edges_take(vtex *v, const tex_t *t) {
    for (int i = 0; i < 2; i++) { free(v->edge[i]); v->edge[i] = NULL; v->edge_n[i] = 0; }
    if (!v->span) return;
    uintptr_t r[2][2], ia, ie; edge_ranges(t, r, &ia, &ie);
    for (int i = 0; i < 2; i++) if (r[i][1] > r[i][0]) { v->edge_n[i] = (uint32_t)(r[i][1] - r[i][0]); v->edge[i] = malloc(v->edge_n[i]); memcpy(v->edge[i], (const void *)r[i][0], v->edge_n[i]); }
    if (nvn_dirty_since_span(v->span, t->mem, t->L.size, v->stamp)) for (int i = 0; i < 2; i++) { free(v->edge[i]); v->edge[i] = NULL; v->edge_n[i] = 0; }
}
static int edges_clean(vtex *v, const tex_t *t) {
    if (!v->edge_n[0] && !v->edge_n[1]) return 0;
    uintptr_t r[2][2], ia, ie; edge_ranges(t, r, &ia, &ie);
    uint64_t st = nvn_dirty_now(); nvn_dirty_protect(t->mem, t->L.size);
    if (ie > ia && nvn_dirty_since_span(v->span, (const void *)ia, ie - ia, v->stamp)) return 0;
    for (int i = 0; i < 2; i++) if (v->edge_n[i] && memcmp(v->edge[i], (const void *)r[i][0], v->edge_n[i])) return 0;
    v->stamp = st; g_pkg_edge_skips++;
    return 1;
}
// the texture's own write-back noted by the completion thread (ab_note): 1 = adopted, the image is guest memory's content (with the replay's lock held)
static int ab_take(vtex *v, const tex_t *t) {
    if (!atomic_load_explicit(&g_nab_n, memory_order_relaxed)) return 0;
    pthread_mutex_lock(&g_ab_mu);
    int i = 0; while (i < g_nab && g_ab[i].ser != t->serial) i++;
    if (i == g_nab) { pthread_mutex_unlock(&g_ab_mu); return 0; }
    ab_rec e = g_ab[i]; g_ab[i] = g_ab[--g_nab]; atomic_store(&g_nab_n, g_nab);
    pthread_mutex_unlock(&g_ab_mu);
    uintptr_t r[2][2], ia, ie; edge_ranges(t, r, &ia, &ie);
    // still this image, not uploaded after the note (its stamp then is newer), no page wholly inside written since, the bytes in the edge pages as the note kept them
    int ok = e.img == (uint64_t)v->img && e.store == t->mem && e.gen >= v->stamp && v->span && v->levels == 1 && v->layers == 1
             && !(ie > ia && nvn_dirty_since_span(v->span, (const void *)ia, ie - ia, e.gen));
    for (int k = 0; k < 2 && ok; k++) ok = e.edge_n[k] == (r[k][1] > r[k][0] ? r[k][1] - r[k][0] : 0) && (!e.edge_n[k] || !memcmp(e.edge[k], (const void *)r[k][0], e.edge_n[k]));
    // seen too, not only the stamp (as upstream's take, #604): an edge page a neighbour's write unprotected after e.gen passed the compare but stays writable, so a later write to the
    // texture's bytes there neither faults nor moves the clock, and vt_get's now == seen shortcut (sound only while every page is protected) would keep a stale image. With seen = e.gen
    // the shortcut holds only while the clock is still e.gen (no page unprotected since ab_note's protect); otherwise the next lookup checks the pages, finds that one newer than the
    // stamp and goes to edges_clean, which protects before it compares: the protect is left to the lookup that meets such a page
    if (ok) { v->stamp = v->seen = e.gen; for (int k = 0; k < 2; k++) { free(v->edge[k]); v->edge[k] = e.edge[k]; v->edge_n[k] = e.edge_n[k]; e.edge[k] = NULL; } atomic_fetch_add(&g_ab_taken, 1); }   // a neighbour's write in an edge page is judged by them (edges_clean)
    else atomic_fetch_add(&g_ab_refused, 1);
    free(e.edge[0]); free(e.edge[1]);
    return ok;
}
// Textures shared by storage (nvn_mtl.m g_alias): an NVN texture object is only a descriptor, and an object made again over the same storage with the same
// description is the same texture. Games make their transient render targets on new objects every frame (Super Mario Bros. Wonder never finalizes them) and
// sample what was drawn through another object: one image per serial left those reads empty and leaked an image a frame. A GPU-made texture (not packaged) is
// keyed by pool, offset and description; each serial resolving to it holds a reference, and the image stays while its pool lives or until another description
// takes over its storage (share_take_over).
typedef struct { uint64_t pool; int64_t off; nvn_texdesc d; } share_key;
static nvn_hmap g_share = {.klen = sizeof(share_key)};
// NVN's image flag says how the texture is registered (mtl_reg_image adds it for RegisterImage), not what it is: the key leaves it out, as nvn_mtl.m's does, so a texture
// drawn into and then registered as an image is one image, not a second, empty one beside it. One registered without it and bound to an image slot all the same takes
// storage use then (vt_storage_up)
static share_key share_of(const tex_t *t) { share_key k; memset(&k, 0, sizeof k); k.pool = (uint64_t)(uintptr_t)t->pool; k.off = t->off; k.d = t->d; k.d.flags &= ~NVN_TEXTURE_FLAGS_IMAGE; return k; }
static int desc_same(const nvn_texdesc *a, const nvn_texdesc *b) { nvn_texdesc x = *a, y = *b; x.flags &= ~NVN_TEXTURE_FLAGS_IMAGE; y.flags &= ~NVN_TEXTURE_FLAGS_IMAGE; return !memcmp(&x, &y, sizeof x); }
static void vt_unref(vtex *v) { if (--v->refs <= 0 && !v->shared) vt_destroy(v); }
static void vt_ref(uint64_t serial, vtex *v) { nvn_hm_put(&g_tex, &serial, v); v->refs++; }
static uint64_t g_share_taken;
// A new shared texture takes over storage: entries of the same pool overlapping it under another description go, unless an object still resolves to them or
// a lookup found them in the last 16 submissions (targets sharing storage within a frame must not evict each other: nvn_mtl.m ALIAS_IDLE)
static void share_take_over(const share_key *nk) {
    nvn_texlayout L; nvn_tex_layout(&nk->d, &L); if (!L.size) return;
    int64_t lo = nk->off, hi = nk->off + (int64_t)L.size;
    for (uint32_t i = 0; i < g_share.cap; i++) {
        vtex *v = g_share.vals[i]; if (!v) continue;
        share_key k; memcpy(&k, g_share.keys + (size_t)i * g_share.klen, sizeof k);
        if (k.pool != nk->pool || !memcmp(&k, nk, sizeof k) || k.off >= hi || v->refs > 0 || atomic_load(&g_fence) - v->used < 16) continue;
        nvn_texlayout kl; nvn_tex_layout(&k.d, &kl);
        if (k.off + (int64_t)kl.size <= lo) continue;
        nvn_hm_del(&g_share, &k); v->shared = 0; vt_destroy(v); g_share_taken++;
        i = (uint32_t)-1;   // the table moved entries on delete: scan again
    }
}
// vkb()'s answer, once it was yes: a flag instead of pthread_once on every lookup (0.17% of Breath of the Wild's replay thread on the Odin 2); DM_VK_TEX_ONCE=0 asks vkb() each time
static int tex_once(void);   // gfx_vk_draw.inc
static _Atomic int g_vkb_up;
static int vkb_up(void) { if (atomic_load_explicit(&g_vkb_up, memory_order_acquire)) return 1; if (!vkb()) return 0; if (tex_once()) atomic_store_explicit(&g_vkb_up, 1, memory_order_release); return 1; }
// Whether a texture's storage can be where its content comes from, whatever it holds now (nvn_mtl.m tex_storage_ok): not a packaged, multisample or buffer texture, nor what only the GPU
// or the decoder fills (depth or stencil, display, video decode), not in a pool the CPU can't write or a sparse one, and none of the flags that make its memory the GPU's (compressible,
// linear, sparse; shader-writable for an uncompressed one, as shaders can't write a block-compressed one). *unc: the uncompressed 2D kind, which the GPU can render into
static int tex_storage_ok(const tex_t *t, int *unc) {
    int f = (int)t->d.format, comp = (f >= 0x41 && f <= 0x50) || (f >= 0x79 && f <= 0x94);   // nvn_mtl.m fmt_compressed
    *unc = !comp && t->d.target != NVN_TEXTURE_TARGET_3D;   // tex_unc
    return t->mem && t->pool && !t->has_layout && t->d.samples <= 1 && t->d.target != NVN_TEXTURE_TARGET_BUFFER
        && !(*unc && ((f >= 0x31 && f <= 0x36) || (t->d.flags & (NVN_TEXTURE_FLAGS_DISPLAY | NVN_TEXTURE_FLAGS_VIDEO_DECODE))))
        && !(t->pool->flags & (NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_VIRTUAL))
        && !(t->d.flags & (NVN_TEXTURE_FLAGS_COMPRESSIBLE | NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET | NVN_TEXTURE_FLAGS_SPARSE))
        && !((t->d.flags & NVN_TEXTURE_FLAGS_IMAGE) && !comp);
}
// Whether an image the GPU wrote lies over a whole page of the texture's storage, or over all of it (g_bk held). This replay keeps no record of GPU-written storage (nvn_mtl.m gws_owned);
// the sharing table's images are the GPU textures over their pools. Part of a page doesn't count: textures are 512-byte aligned, so a GPU-drawn neighbour may end in the page this one
// begins in (nvn_mtl.m gws_sliver: Ocarina of Time's RG8 normal tiles reached into the height and hole maps beside them)
static int gpu_wrote_over(const tex_t *t) {
    static uintptr_t pg; if (!pg) { long l = sysconf(_SC_PAGESIZE); pg = l > 0 ? (uintptr_t)l : 4096; }
    int64_t lo = t->off, hi = t->off + (int64_t)t->L.size;
    for (uint32_t i = 0; i < g_share.cap; i++) {
        vtex *v = g_share.vals[i]; if (!v || !v->gpu_w) continue;
        share_key k; memcpy(&k, g_share.keys + (size_t)i * g_share.klen, sizeof k);
        if (k.pool != (uint64_t)(uintptr_t)t->pool || k.off >= hi) continue;
        nvn_texlayout kl; nvn_tex_layout(&k.d, &kl); int64_t klo = k.off, khi = k.off + (int64_t)kl.size;
        if (khi <= lo) continue;
        if (klo <= lo && khi >= hi) return 1;
        uintptr_t a = (uintptr_t)t->pool->mem + (uintptr_t)(klo > lo ? klo : lo), e = (uintptr_t)t->pool->mem + (uintptr_t)(khi < hi ? khi : hi);
        a = (a + pg - 1) & ~(pg - 1); e &= ~(pg - 1);
        if (e > a) return 1;
    }
    return 0;
}
// Storage the CPU wrote before a texture's first use (nvn_mtl.m tex_cpu_content; DM_NVN_STORAGE_FIRST_USE=0 turns it off): a texture whose storage can hold its content and holds data
// when it is first resolved is made CPU-written, uploaded now and again when its pages change, flushed or not. tex_init_fill's one-time fill takes only a pool the CPU can't access or
// uncached, or flushed storage, and the poll (tex_dirty_upload) sees only writes after nvnTextureInitialize. Ocarina of Time writes its terrain tiles' height (260x260 R32F), blend and
// hole (RGBA8) and detail (BC) maps into a CPU_CACHED pool before it makes the textures over them, so they were GPU images of zeros: the riverbed was never drawn, and the water pass
// tinted the far depth it saw through it a flat pale teal. An uncompressed 2D texture can't be told from a render target by its flags (NVN has none), so it is taken only over storage no
// image the GPU wrote lies on. → 0: not taken; 1: taken; 2: taken, uncompressed 2D (g_bk held)
static uint64_t g_first_use_taken;
static int storage_first_use(const tex_t *t) {
    static int on = -1; if (on < 0) { const char *e = getenv("DM_NVN_STORAGE_FIRST_USE"); on = !e || atoi(e) != 0; }
    int unc; if (!on || !tex_storage_ok(t, &unc) || !mem_any(t->mem, t->L.size) || (unc && gpu_wrote_over(t))) return 0;
    if (!nvn_hm_get(&g_cpuw, &t->serial)) nvn_hm_put(&g_cpuw, &t->serial, (void *)1);
    if (g_first_use_taken++ < 4) LOG("[vk] texture written by the CPU before its first use: ser=%llu %ux%ux%u format %#x%s, pool flags %#x: uploaded from its storage, again when it changes (%llu so far)",
        (unsigned long long)t->serial, t->d.width, t->d.height, t->d.depth, t->d.format, unc ? " (uncompressed, over storage no GPU-written image lies on)" : "", t->pool->flags, (unsigned long long)g_first_use_taken);
    return unc ? 2 : 1;
}
// The image of a texture: made at first use; a texture whose content lives in guest memory is uploaded, and uploaded again when its pages were written
static void ev_made(vtex *v);   // gfx_vk_evict.inc
// DM_VK_TBUF_ROWS=0 (env.txt): a buffer texture in a 2D texture slot reads 0, as before it was read as rows (gfx_vk_draw.inc tex_rows_slot; the A/B of that alone)
static int tbuf_rows_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_TBUF_ROWS"); v = !(e && *e == '0'); if (!v) LOG("[vk] DM_VK_TBUF_ROWS=0: a buffer texture in a 2D texture slot reads 0"); } return v; }
// rows: the caller is a 2D texture slot that may read a buffer texture as rows (vt_get_s); any other lookup of a buffer texture has no image (its reads are texel buffers)
static vtex *vt_get_(const tex_t *ct, int rows) {
    tex_t *t = (tex_t *)ct;
    if (!t || !t->serial || !vkb_up()) return NULL;
    if (t->magic != MAGIC('T', 'E', 'X', 0)) { RED_ONCE(30, "texture object no longer valid (magic %#x)", t->magic); return NULL; }
    int tbuf = t->d.target == NVN_TEXTURE_TARGET_BUFFER;
    if (tbuf && !rows) { RED_ONCE(198, "a buffer texture where an image is needed (a texture slot of another type, an image slot, a target, a copy or a texture pool read): it reads 0, and is not drawn into"); return NULL; }
    // a buffer texture's ID can outlive its pool (Unreal Engine 5.4 keeps them, nvn_mtl.m tbuf_as_2d): its rows are read from guest memory, which must still be mapped
    if (tbuf && (!t->mem || !mtl_addr((uint64_t)(uintptr_t)t->mem, t->L.size, NULL, NULL))) return NULL;
    vtex *v = nvn_hm_get(&g_tex, &t->serial);
    if (v && (v->store != t->mem || !desc_same(&v->desc, &t->d))) {
        static int n; if (n++ < 8) LOG("[vk] texture ser=%llu met again with %s: made anew (storage %p, now %p)", (unsigned long long)t->serial, v->store != t->mem ? "other storage" : "another description", (const void *)v->store, (const void *)t->mem);
        nvn_hm_del(&g_tex, &t->serial); g_vt_gen++; vt_unref(v); v = NULL; }
    int pkg = is_packaged(t) || tbuf, took = 0;   // rows: from guest memory, again when its pages change, as a linear texture (vt_upload_)
    if (!v && !pkg && (took = storage_first_use(t))) pkg = 1;
    if (v && v->pkg) {
        uint64_t now = nvn_dirty_now();
        int clean = v->span && (now == v->seen || !nvn_dirty_since_span(v->span, t->mem, t->L.size, v->stamp));
        v->seen = now;
        if (clean) return v;
        if (wb_absorb_on() && ab_take(v, t)) return v;   // written only by its own write-back: the image holds it (ab_note)
        if (edges_clean(v, t)) return v;
        if (vt_upload(v, t, 1)) { g_pkg_reuploads++; edges_take(v, t); }
        return v;
    }
    if (v && !pkg) { v->used = atomic_load(&g_fence); return v; }
    if (v) { nvn_hm_del(&g_tex, &t->serial); g_vt_gen++; vt_unref(v); }   // written by the CPU since it was made as a GPU texture: made again from guest memory, its own
    share_key k = share_of(t);
    if (!pkg && t->pool) {   // another object over the same storage, the same description: the same texture
        vtex *s = nvn_hm_get(&g_share, &k);
        if (s) { vt_ref(t->serial, s); s->used = atomic_load(&g_fence); return s; }
    }
    if (!(v = vt_create(t))) return NULL;
    v->used = atomic_load(&g_fence); ev_made(v);
    if (pkg) { v->pkg = 1; v->taken = took == 2; if (vt_upload(v, t, 1)) { g_pkg_uploads++; edges_take(v, t); } }
    else if (t->pool) { v->filled = (uint8_t)tex_init_fill(v, t); share_take_over(&k); nvn_hm_put(&g_share, &k, v); v->shared = 1; }
    vt_ref(t->serial, v);
    return v;
}
static vtex *vt_get(const tex_t *ct) { vtex *v = vt_get_(ct, 0); if (v) v->last_present = atomic_load(&g_presents); return v; }   // its last lookup, for eviction (gfx_vk_evict.inc)
// A texture slot's lookup (gfx_vk_draw.inc stage_textures, stage_set): rows, the slot may read a buffer texture as rows of 16384 texels (tex_rows_slot), which it then gets
static vtex *vt_get_s(const tex_t *ct, int rows) { vtex *v = vt_get_(ct, rows); if (v) v->last_present = atomic_load(&g_presents); return v; }
// A lookup for what the GPU writes (a pass's target, a copy's or a clear's destination): such an image's content is the GPU's from then on, not its storage's, so it is never evicted
// A texture taken from its storage at its first use (storage_first_use) that the GPU then writes holds what the GPU drew, not what the storage holds: it becomes a GPU texture, so a CPU write
// beside it no longer uploads the storage over the drawing (nvn_mtl.m cpu_unc_drop)
static vtex *vt_get_w(const tex_t *ct) {
    vtex *v = vt_get(ct);
    if (v && v->taken) { v->taken = 0; v->pkg = 0; nvn_hm_del(&g_cpuw, &ct->serial); }
    if (v) { v->gpu_w = 1; v->last_w = v->last_present; }
    return v;
}
// A memory pool's Finalize (mtl_alias_drop_pool): the textures made on its storage go, as nvn_mtl.m evicts its shared textures; their serials may be met again on other storage
void vt_drop_pool(const void *pool) {
    void vkb_frame_forget(uint64_t serial);
    pthread_mutex_lock(&g_bk);
    for (uint32_t i = 0; i < g_tex.cap; i++) {   // the objects' references, then the sharing table's entries
        vtex *v = g_tex.vals[i];
        if (!v || v->pool != pool) continue;
        uint64_t k; memcpy(&k, g_tex.keys + (size_t)i * g_tex.klen, 8); nvn_hm_del(&g_tex, &k); g_vt_gen++; vt_unref(v);
        i = (uint32_t)-1;   // the table may have moved entries on delete: scan again
    }
    for (uint32_t i = 0; i < g_share.cap; i++) {
        vtex *v = g_share.vals[i];
        if (!v || v->pool != pool) continue;
        share_key k; memcpy(&k, g_share.keys + (size_t)i * g_share.klen, sizeof k); nvn_hm_del(&g_share, &k); v->shared = 0; if (v->refs <= 0) vt_destroy(v);
        i = (uint32_t)-1;
    }
    pthread_mutex_unlock(&g_bk);
}
#include "gfx_vk_evict.inc"   // images no draw has used for a while, evicted when their content is still in guest memory (vt_evict)
void mtl_tex_init(const tex_t *t) { (void)t; }
static void tex_drop(uint64_t serial) {   // g_bk held
    vtex *v = nvn_hm_del(&g_tex, &serial); nvn_hm_del(&g_cpuw, &serial);
    if (v) { g_vt_gen++; vt_unref(v); }   // a shared image stays for the next object over its storage
}
static int tex_pinned_release(uint64_t serial);   // with the queue scheduler's hooks
void mtl_tex_release(tex_t *t) {
    if (!t->serial || tex_pinned_release(t->serial)) return;   // a queued job or the bound targets still name it: dropped at its last unpin
    pthread_mutex_lock(&g_bk); tex_drop(t->serial); pthread_mutex_unlock(&g_bk);
}
// nvn_queue.inc polls the CPU's texture rewrites (nvn_ft_poll, tex_dirty_upload) between mtl_sub_begin and mtl_sub_commit, on the replay thread, which holds g_bk all that
// time: these answers take g_bk only when the calling thread does not hold it already. Taken again, it deadlocked the replay at the first rewrite the poll saw: Ocarina of
// Time's first submission never ran, and the game waited on its fence with a black screen
#define BK_LOCK() int bk_own_ = !t_bk_sub; if (bk_own_) pthread_mutex_lock(&g_bk)
#define BK_UNLOCK() do { if (bk_own_) pthread_mutex_unlock(&g_bk); } while (0)
void mtl_tex_cpu_written(const tex_t *t) { BK_LOCK(); if (!nvn_hm_get(&g_cpuw, &t->serial)) nvn_hm_put(&g_cpuw, &t->serial, (void *)1); BK_UNLOCK(); }
int mtl_tex_gpu_owned(uint64_t serial) { BK_LOCK(); vtex *v = nvn_hm_get(&g_tex, &serial); int r = v && !v->pkg; BK_UNLOCK(); return r; }
// nvn_ft_poll (tex_dirty_upload, hle_nvn.c): the CPU rewrote a registered texture's pages after its first use. As nvn_mtl.m's rule (nxnative PR #1558): a texture whose
// storage can be where its content comes from is uploaded from it, unless it is an uncompressed 2D one the GPU has drawn into, whose content lives in its Vulkan image (this
// replay writes no block-linear target back to its storage): that one stays the GPU's until the CPU rewrites it again. The rest: the GPU's if it is a GPU-only image, else
// uploaded. Ocarina of Time rewrites its terrain tiles' height (R32F), blend and hole (RGBA8) maps as tile slots are reused; the nxnative merge (b547f1e2) moved the poll
// to this question, which the replay did not answer yet, so a host with tex_dirty_upload did not link
int mtl_tex_cpu_rewritten(const tex_t *t) {
    int unc, ok = tex_storage_ok(t, &unc);
    if (ok) {
        if (unc) { BK_LOCK(); vtex *v = nvn_hm_get(&g_tex, &t->serial); int drawn = v && !v->pkg && v->rt_last; BK_UNLOCK(); if (drawn) return MTL_REWRITE_GPU_NOW; }
        mtl_tex_cpu_written(t); return MTL_REWRITE_UPLOADED;
    }
    if (mtl_tex_gpu_owned(t->serial)) return MTL_REWRITE_GPU;
    mtl_tex_cpu_written(t); return MTL_REWRITE_UPLOADED;
}
static void reg_tex(int id, const tex_t *t, const view_t *v); static void reg_samp(int id, const samp_par *p);   // gfx_vk_draw.inc
void mtl_reg_tex(int id, const tex_t *t, const view_t *vw) { reg_tex(id, t, vw); }
void mtl_reg_samp(int id, const samp_par *p) { reg_samp(id, p); }
// hle_nvn.c calls mtl_prog_release at a program's Finalize and SetShaders, before the program's key record (pk_get) is dropped or reset, and mtl_prog_keys_changed once
// it is dropped or filled: the replay's program-to-shader memo (gfx_vk_draw.inc prog_memo) holds what the key records said, so any of them changing empties it. Both move
// the generation, on the game's threads with no lock the replay takes: a draw in between can read the new serial with the old or reset keys and c1 ranges (SetShaders
// gives the serial first), and its entry carries a generation read before the second move, so it is stale once that lands; a draw that reads the second move's value
// (acquire) sees the complete record (release)
static _Atomic uint64_t g_prog_gen = 1;
void mtl_prog_release(prog_t *p) { (void)p; atomic_fetch_add_explicit(&g_prog_gen, 1, memory_order_release); }
void mtl_prog_keys_changed(const prog_t *p) { (void)p; atomic_fetch_add_explicit(&g_prog_gen, 1, memory_order_release); }
void mtl_set_tex_pool_n(int n) { g_pool_texn = n; }

// ---------------- Copies ----------------
static struct { int64_t row, img; } S;   // SetCopyRowStride / SetCopyImageStride
static int tex_level_layer(const vtex *v, const view_t *vw, int has, unsigned *level, unsigned *layer0) {
    *level = has && vw->set_levels ? vw->base_level : 0; *layer0 = has && vw->set_layers ? vw->min_layer : 0;
    return *level < v->levels;
}
// A readback of a depth / stencil image (ReadTexels, a texture → buffer copy) copies each aspect the image and the guest's format have into a plane of its own, tightly
// packed (Vulkan's layout for an aspect: depth 2 or 4 bytes a texel, stencil 1), and the completion thread rebuilds NVN's texels from them into guest memory (wb_apply,
// ds_texels.h). The depth aspect alone was copied before, at the guest's texel size: DEPTH32F_STENCIL8's rows are 8 bytes a texel, so every row after the first read
// another row's depths and the rest of the region was left as the slice held it; the stencil was never read; DEPTH24_STENCIL8 and DEPTH24, kept as D32_SFLOAT (S8), came
// back as float bits instead of 24-bit unorm. Colour images are copied as before. → 0: not a depth / stencil image of a format ds_texels knows (copied as before)
static int ds_dkind(const vtex *v) {
    if (!(v->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) return DS_NONE;
    switch (v->fmt) {
        case VK_FORMAT_D16_UNORM: return DS_D16;
        case VK_FORMAT_D32_SFLOAT: case VK_FORMAT_D32_SFLOAT_S8_UINT: return DS_D32F;
        case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_X8_D24_UNORM_PACK32: return DS_D24;
        default: return DS_NONE;
    }
}
static int ds_wb(vtex *v, uint32_t nvn_format, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) {
    if (!(v->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) || !ds_texel_bytes((int)nvn_format)) return 0;
    int dkind = nvn_format == 0x31 ? DS_NONE : ds_dkind(v), sten = (v->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) && (nvn_format == 0x31 || nvn_format == 0x35 || nvn_format == 0x36);
    if (nvn_format != 0x31 && !dkind) return 0;
    size_t n = (size_t)r->width * (size_t)r->height, soff = ds_sten_off(dkind, n), per = ds_slice_bytes(dkind, sten, n);
    int is3d = v->type == VK_IMAGE_TYPE_3D, nz = r->depth ? r->depth : 1;
    if (vk_debug()) LOG("[vk] depth / stencil readback ser=%llu pkg %d format %#x level %u layer %u region %d,%d %dx%dx%d row %zu img %zu -> %p", (unsigned long long)v->serial, v->pkg, nvn_format, level, layer0, r->xoffset, r->yoffset, r->width, r->height, nz, row, img, (void *)dst);
    stg_sl s = stg_take(per * nz, 4); VkCommandBuffer cbuf = cb(); if (!s.p || !cbuf) return 1;
    for (int z = 0; z < nz; z++) {
        for (int a = 0; a < 2; a++) {   // the depth plane, then the stencil plane
            if (a ? !sten : !dkind) continue;
            VkBufferImageCopy rc = {.bufferOffset = s.off + (size_t)z * per + (a ? soff : 0), .imageSubresource = {a ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT, level, is3d ? 0 : layer0 + (unsigned)r->zoffset + (unsigned)z, 1},
                                    .imageOffset = {r->xoffset, r->yoffset, is3d ? r->zoffset + z : 0}, .imageExtent = {(uint32_t)r->width, (uint32_t)r->height, 1}};
            vkCmdCopyImageToBuffer(cbuf, v->img, VK_IMAGE_LAYOUT_GENERAL, s.b, 1, &rc);
        }
        size_t rb = (size_t)r->width * ds_texel_bytes((int)nvn_format);
        wb_t *w = wb_add(); if (!w) continue;
        *w = (wb_t){s.st, (size_t)s.off + (size_t)z * per, rb, (size_t)r->height, rb, row, dst + (size_t)z * img, NULL};
        w->ds_n = (uint32_t)r->width; w->ds_fmt = nvn_format; w->ds_dkind = dkind; w->ds_sten = sten;
    }
    return 1;
}
// The other way (a buffer → texture copy into a depth / stencil image): NVN's texels are split into the same planes (ds_texels.h ds_slice_up) and each aspect the image
// and the guest's format have is copied in, so what ds_wb read out goes back in as it was. The guest's bytes were copied into the depth aspect as they were before:
// DEPTH24 and DEPTH24_STENCIL8, kept as D32_SFLOAT (S8), had their unorm bits taken as floats, DEPTH32F_STENCIL8's 8-byte texels were read as 4-byte ones, and no
// stencil was written. → 0: not a depth / stencil image of a format ds_texels knows (copied as before, red line 41)
static int ds_up(vtex *v, uint32_t nvn_format, unsigned level, unsigned layer0, const NVNcopyRegion *r, const uint8_t *src, size_t row, size_t img) {
    if (!(v->aspect & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) || !ds_texel_bytes((int)nvn_format)) return 0;
    int dkind = nvn_format == 0x31 ? DS_NONE : ds_dkind(v), sten = (v->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) && (nvn_format == 0x31 || nvn_format == 0x35 || nvn_format == 0x36);
    if (nvn_format != 0x31 && !dkind) return 0;
    if (!dkind && !sten) return 1;   // STENCIL8 texels for an image without stencil: nothing of them has a place
    size_t n = (size_t)r->width * (size_t)r->height, soff = ds_sten_off(dkind, n), per = ds_slice_bytes(dkind, sten, n);
    int is3d = v->type == VK_IMAGE_TYPE_3D, nz = r->depth ? r->depth : 1;
    stg_sl s = stg_take(per * nz, 4); VkCommandBuffer cbuf = cb(); if (!s.p || !cbuf) return 1;
    for (int z = 0; z < nz; z++) {
        ds_slice_up((int)nvn_format, dkind, sten, src + (size_t)z * img, row, (size_t)r->width, (size_t)r->height, s.p + (size_t)z * per);
        for (int a = 0; a < 2; a++) {   // the depth plane, then the stencil plane
            if (a ? !sten : !dkind) continue;
            VkBufferImageCopy rc = {.bufferOffset = s.off + (size_t)z * per + (a ? soff : 0), .imageSubresource = {a ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT, level, is3d ? 0 : layer0 + (unsigned)r->zoffset + (unsigned)z, 1},
                                    .imageOffset = {r->xoffset, r->yoffset, is3d ? r->zoffset + z : 0}, .imageExtent = {(uint32_t)r->width, (uint32_t)r->height, 1}};
            vkCmdCopyBufferToImage(cbuf, s.b, v->img, VK_IMAGE_LAYOUT_GENERAL, 1, &rc);
        }
    }
    return 1;
}
// The plain copy of a depth-stencil image that ds_wb does not take (a format it does not know) copies the depth aspect alone into a slice sized by the guest's texel, which
// leaves part of each row unwritten: a pool slice holds an earlier slice's bytes, so it is zeroed first and the guest gets what a buffer of its own gave (zeros)
static void stg_zero_ds(stg_sl s, VkDeviceSize n, const vtex *v) { if (s.p && s.st >= SP_ST && v->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) memset(s.p, 0, n); }
static void copy_b2t(const p_copy_b2t *c) {
    vtex *v = vt_get_w(c->tex); if (!v) return;
    unsigned level, layer0; if (!tex_level_layer(v, &c->view, c->has_view, &level, &layer0)) return;
    int bw, bh, bpb = nvn_fmt_bpb(c->tex->d.format, &bw, &bh); if (!bpb) return;
    size_t rb = (size_t)((c->r.width + bw - 1) / bw) * bpb, rows = (c->r.height + bh - 1) / bh;
    size_t row = S.row ? (size_t)S.row : rb, img = S.img ? (size_t)S.img : row * rows;
    int is3d = v->type == VK_IMAGE_TYPE_3D, nz = c->r.depth ? c->r.depth : 1;
    if (!mtl_addr(c->src, img * (nz - 1) + row * (rows - 1) + rb, NULL, NULL)) { RED_ONCE(40, "buffer→texture copy source is in no memory pool"); return; }
    g_vs = 3; cpu_view();
    if (!v->conv && ds_up(v, (uint32_t)c->tex->d.format, level, layer0, &c->r, (const uint8_t *)c->src, row, img)) { cb_barrier(); return; }   // depth / stencil: split into the aspects' planes
    size_t out_row = v->conv ? (size_t)c->r.width * v->px : rb, out_rows = v->conv ? (size_t)c->r.height : rows;
    stg_sl s = stg_take(out_row * out_rows * nz, v->conv ? v->px : bpb); VkCommandBuffer cbuf = cb(); if (!s.p || !cbuf) return;
    for (int z = 0; z < nz; z++) {
        const uint8_t *src = (const uint8_t *)c->src + (size_t)z * img; uint8_t *d = s.p + (size_t)z * out_row * out_rows;
        if (v->conv) bc_decode(v->conv, src, row, (uint32_t)c->r.width, (uint32_t)c->r.height, d, out_row, v->px);
        else for (size_t y = 0; y < rows; y++) memcpy(d + y * rb, src + y * row, rb);
        if (v->rbx) rb16_rows(d, rb, rows, rb, v->rbx);   // the guest's order into the image's (rb16.h)
        VkBufferImageCopy r = {.bufferOffset = s.off + (size_t)z * out_row * out_rows, .imageSubresource = {v->aspect & ~VK_IMAGE_ASPECT_STENCIL_BIT ? v->aspect & ~VK_IMAGE_ASPECT_STENCIL_BIT : v->aspect, level, is3d ? 0 : layer0 + c->r.zoffset + z, 1},
                               .imageOffset = {c->r.xoffset, c->r.yoffset, is3d ? c->r.zoffset + z : 0}, .imageExtent = {(uint32_t)c->r.width, (uint32_t)c->r.height, 1}};
        if (v->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) RED_ONCE(41, "buffer→texture copy into a depth-stencil texture: only the depth is copied, as is");
        vkCmdCopyBufferToImage(cbuf, s.b, v->img, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    }
    cb_barrier();
}
static void copy_t2b(const p_copy_b2t *c) {
    vtex *v = vt_get(c->tex); if (!v) return;
    unsigned level, layer0; if (!tex_level_layer(v, &c->view, c->has_view, &level, &layer0)) return;
    if (v->conv) { RED_ONCE(42, "texture→buffer copy of a texture converted on upload (BC decoded on the CPU, or 16-bit normalized made as halves: n16_half): not copied"); return; }
    int bw, bh, bpb = nvn_fmt_bpb(c->tex->d.format, &bw, &bh); if (!bpb) return;
    size_t rb = (size_t)((c->r.width + bw - 1) / bw) * bpb, rows = (c->r.height + bh - 1) / bh;
    size_t row = S.row ? (size_t)S.row : rb, img = S.img ? (size_t)S.img : row * rows;
    int is3d = v->type == VK_IMAGE_TYPE_3D, nz = c->r.depth ? c->r.depth : 1;
    if (!mtl_addr(c->src, img * (nz - 1) + row * (rows - 1) + rb, NULL, NULL)) { RED_ONCE(43, "texture→buffer copy destination is in no memory pool"); return; }
    if (ds_wb(v, (uint32_t)c->tex->d.format, level, layer0, &c->r, (uint8_t *)c->src, row, img)) { cb_barrier(); return; }   // depth / stencil: each aspect apart, rebuilt
    stg_sl s = stg_take(rb * rows * nz, bpb); stg_zero_ds(s, rb * rows * nz, v); VkCommandBuffer cbuf = cb(); if (!s.p || !cbuf) return;
    if (vk_debug()) LOG("[vk] t2b ser=%llu pkg %d level %u layer %u region %d,%d %dx%dx%d row %zu img %zu -> %#llx", (unsigned long long)v->serial, v->pkg, level, layer0, c->r.xoffset, c->r.yoffset, c->r.width, c->r.height, nz, row, img, (unsigned long long)c->src);
    for (int z = 0; z < nz; z++) {
        VkBufferImageCopy r = {.bufferOffset = s.off + (size_t)z * rb * rows, .imageSubresource = {v->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : v->aspect, level, is3d ? 0 : layer0 + c->r.zoffset + z, 1},
                               .imageOffset = {c->r.xoffset, c->r.yoffset, is3d ? c->r.zoffset + z : 0}, .imageExtent = {(uint32_t)c->r.width, (uint32_t)c->r.height, 1}};
        vkCmdCopyImageToBuffer(cbuf, v->img, VK_IMAGE_LAYOUT_GENERAL, s.b, 1, &r);
        wb_t *w = wb_add(); if (w) { *w = (wb_t){s.st, (size_t)s.off + (size_t)z * rb * rows, rb, rows, rb, row, (uint8_t *)c->src + (size_t)z * img, NULL}; w->rbx = v->rbx; }   // rbx: into the guest's order as it lands
    }
    cb_barrier();
}
static int fmt_int(VkFormat f) {   // an integer colour format (vk_of_mtl's): a blit cannot convert to or from one
    switch (f) {
        case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT: case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_SINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8_SINT:
        case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R8G8B8A8_SINT:
        case VK_FORMAT_A2B10G10R10_UINT_PACK32: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R16G16B16A16_SINT:
        case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT: return 1;
        default: return 0;
    }
}
static int ds_copy_draw(vtex *s, vtex *d, unsigned sl, unsigned sz, unsigned dl, unsigned dz, const p_copy_t2t *c);   // gfx_vk_dscopy.inc: one a blit cannot make, drawn
static void copy_t2t(const p_copy_t2t *c) {
    vtex *s = vt_get(c->src), *d = vt_get_w(c->dst); if (!s || !d) return;
    if (d->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) d->ds_copied = 1;   // its next render pass begins with gfx_vk_dscopy.inc copied_depth_pass's draw
    unsigned sl, sz, dl, dz;
    if (!tex_level_layer(s, &c->sv, c->has_sv, &sl, &sz) || !tex_level_layer(d, &c->dv, c->has_dv, &dl, &dz)) return;
    VkCommandBuffer cbuf = cb(); if (!cbuf) return;
    int ns = c->sr.depth ? c->sr.depth : 1, nd = c->dr.depth ? c->dr.depth : 1;
    VkImageAspectFlags as = s->aspect, ad = d->aspect;
    if ((c->flags & DM_COPY_RESOLVE) && s->samples > 1) {
        VkImageResolve r = {.srcSubresource = {as, sl, sz + c->sr.zoffset, 1}, .srcOffset = {c->sr.xoffset, c->sr.yoffset, 0},
                            .dstSubresource = {ad, dl, dz + c->dr.zoffset, 1}, .dstOffset = {c->dr.xoffset, c->dr.yoffset, 0}, .extent = {(uint32_t)c->dr.width, (uint32_t)c->dr.height, 1}};
        vkCmdResolveImage(cbuf, s->img, VK_IMAGE_LAYOUT_GENERAL, d->img, VK_IMAGE_LAYOUT_GENERAL, 1, &r); cb_barrier(); return;
    }
    // A colour copy between two float, normalized or sRGB formats converts, as NVN's copy and the Mac's draw do: it is a blit unless the formats are equal or both
    // decode on the CPU from one compressed family (the same blocks, decoded alike). Unconverted formats all have conv 0 and px 0, so without the conv test Link's
    // Awakening's R11G11B10F frame was copied into its RGBA8 window as raw bits, banding every colour. A blit cannot convert to or from an integer format, nor
    // between depth formats, so those keep the raw copy
    int blits = s->aspect == VK_IMAGE_ASPECT_COLOR_BIT && d->aspect == VK_IMAGE_ASPECT_COLOR_BIT && !fmt_int(s->fmt) && !fmt_int(d->fmt);
    int same = c->sr.width == c->dr.width && c->sr.height == c->dr.height && ns == nd && !(c->flags & (NVN_COPY_FLAGS_MIRROR_X_BIT | NVN_COPY_FLAGS_MIRROR_Y_BIT | NVN_COPY_FLAGS_MIRROR_Z_BIT))
               && (s->fmt == d->fmt || (s->conv == d->conv && aspect_of(s->fmt) == aspect_of(d->fmt) && s->px == d->px && (s->conv != CONV_NONE || !blits)));
    int s3 = s->type == VK_IMAGE_TYPE_3D, d3 = d->type == VK_IMAGE_TYPE_3D;
    if (same && s->samples == d->samples) {
        VkImageCopy r = {.srcSubresource = {as, sl, s3 ? 0 : sz + c->sr.zoffset, s3 ? 1 : (uint32_t)ns}, .srcOffset = {c->sr.xoffset, c->sr.yoffset, s3 ? c->sr.zoffset : 0},
                         .dstSubresource = {ad, dl, d3 ? 0 : dz + c->dr.zoffset, d3 ? 1 : (uint32_t)nd}, .dstOffset = {c->dr.xoffset, c->dr.yoffset, d3 ? c->dr.zoffset : 0},
                         .extent = {(uint32_t)c->sr.width, (uint32_t)c->sr.height, s3 && d3 ? (uint32_t)ns : 1}};
        vkCmdCopyImage(cbuf, s->img, VK_IMAGE_LAYOUT_GENERAL, d->img, VK_IMAGE_LAYOUT_GENERAL, 1, &r); cb_barrier(); return;
    }
    // a format change, a scaled or mirrored region: a blit, where both formats allow it
    VkFormatProperties fs, fd; vkGetPhysicalDeviceFormatProperties(g_vk.pd, s->fmt, &fs); vkGetPhysicalDeviceFormatProperties(g_vk.pd, d->fmt, &fd);
    if (!(fs.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) || !(fd.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) || as != ad) {
        if (as != VK_IMAGE_ASPECT_COLOR_BIT && ad != VK_IMAGE_ASPECT_COLOR_BIT && ds_copy_draw(s, d, sl, sz, dl, dz, c)) return;   // depth-stencil: drawn (no Adreno blits D32_SFLOAT_S8_UINT)
        RED_ONCE(44, "texture copy with a format change or scaling between formats %d and %d: not blittable on this GPU", s->fmt, d->fmt); return; }
    int x0 = c->dr.xoffset, x1 = c->dr.xoffset + c->dr.width, y0 = c->dr.yoffset, y1 = c->dr.yoffset + c->dr.height;
    if (c->flags & NVN_COPY_FLAGS_MIRROR_X_BIT) { int k = x0; x0 = x1; x1 = k; }
    if (c->flags & NVN_COPY_FLAGS_MIRROR_Y_BIT) { int k = y0; y0 = y1; y1 = k; }
    for (int z = 0; z < (s3 ? 1 : ns); z++) {
        VkImageBlit b = {.srcSubresource = {as, sl, s3 ? 0 : sz + c->sr.zoffset + z, 1}, .srcOffsets = {{c->sr.xoffset, c->sr.yoffset, s3 ? c->sr.zoffset : 0}, {c->sr.xoffset + c->sr.width, c->sr.yoffset + c->sr.height, s3 ? c->sr.zoffset + ns : 1}},
                         .dstSubresource = {ad, dl, d3 ? 0 : dz + c->dr.zoffset + z, 1}, .dstOffsets = {{x0, y0, d3 ? c->dr.zoffset : 0}, {x1, y1, d3 ? c->dr.zoffset + nd : 1}}};
        vkCmdBlitImage(cbuf, s->img, VK_IMAGE_LAYOUT_GENERAL, d->img, VK_IMAGE_LAYOUT_GENERAL, 1, &b,
                       (c->flags & NVN_COPY_FLAGS_LINEAR_FILTER_BIT) && as == VK_IMAGE_ASPECT_COLOR_BIT && (fs.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    }
    cb_barrier();
}
// CPU texture read: packaged textures from guest memory (the ground truth, nvn_mtl.m mtl_read_texels); the rest copied back from the image
void mtl_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) {
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh); if (!bpb) return;
    size_t wb = (size_t)((r->width + bw - 1) / bw) * bpb, hb = (size_t)(r->height + bh - 1) / bh, x0 = (size_t)(r->xoffset / bw) * bpb, y0 = (size_t)(r->yoffset / bh);
    int nz = r->depth ? r->depth : 1, is3d = t->d.target == 2;
    pthread_mutex_lock(&g_bk);
    if (is_packaged(t)) {
        uint32_t lw = t->d.width >> level ? t->d.width >> level : 1, lh = t->d.height >> level ? t->d.height >> level : 1, lz = is3d ? (t->d.depth >> level ? t->d.depth >> level : 1) : 1;
        size_t lp = (size_t)((lw + bw - 1) / bw) * bpb, lr = (lh + bh - 1) / bh;
        uint8_t *lin = malloc(lp * lr * lz);
        for (int z = 0; lin && z < nz; z++) {
            unsigned layer = is3d ? 0 : layer0 + (unsigned)r->zoffset + (unsigned)z;
            if ((is3d && z == 0) || !is3d) if (nvn_deswizzle(&t->d, level, layer, t->mem, lin, lp)) { RED_ONCE(50, "ReadTexels deswizzle failed"); break; }
            const uint8_t *src = lin + (is3d ? (size_t)(r->zoffset + z) * lp * lr : 0);
            for (size_t y = 0; y < hb; y++) memcpy(dst + (size_t)z * img + y * row, src + (y0 + y) * lp + x0, wb);
        }
        free(lin); pthread_mutex_unlock(&g_bk); return;
    }
    vtex *v = vt_get(t);
    int half = v && (v->conv == CONV_U16H || v->conv == CONV_S16H);   // read back and converted (n16_back)
    if (!v || (v->conv && !half)) { pthread_mutex_unlock(&g_bk); for (int z = 0; z < nz; z++) for (size_t y = 0; y < hb; y++) memset(dst + (size_t)z * img + y * row, 0, wb); return; }
    if (ds_wb(v, (uint32_t)t->d.format, level, layer0, r, dst, row, img)) { submit_wait(); pthread_mutex_unlock(&g_bk); return; }   // depth / stencil: each aspect apart, rebuilt
    stg_sl s = stg_take(wb * hb * nz, bpb); stg_zero_ds(s, wb * hb * nz, v); VkCommandBuffer cbuf = cb();
    if (s.p && cbuf) {
        for (int z = 0; z < nz; z++) {
            VkBufferImageCopy rc = {.bufferOffset = s.off + (size_t)z * wb * hb, .imageSubresource = {v->aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : v->aspect, level, is3d ? 0 : layer0 + (unsigned)r->zoffset + (unsigned)z, 1},
                                    .imageOffset = {r->xoffset, r->yoffset, is3d ? r->zoffset + z : 0}, .imageExtent = {(uint32_t)r->width, (uint32_t)r->height, 1}};
            vkCmdCopyImageToBuffer(cbuf, v->img, VK_IMAGE_LAYOUT_GENERAL, s.b, 1, &rc);
        }
        for (int z = 0; z < nz; z++) { wb_t *w = wb_add(); if (w) { *w = (wb_t){s.st, (size_t)s.off + (size_t)z * wb * hb, wb, hb, wb, row, dst + (size_t)z * img, NULL}; w->rbx = v->rbx; } }   // copied out by the completion thread (rbx: in the guest's order)
        submit_wait();
        if (half) for (int z = 0; z < nz; z++) for (size_t y = 0; y < hb; y++) n16_back(v->conv, dst + (size_t)z * img + y * row, wb);
    }
    pthread_mutex_unlock(&g_bk);
}

// ---------------- Clears ----------------
static void clear_color_pass(const p_clear_color *cc); static void clear_ds_pass(const p_clear_ds *cd, VkImageAspectFlags a);   // gfx_vk_draw.inc: partial and masked ones
static int clear_in_pass(const vtex *v, int slot, unsigned level, int depth);   // gfx_vk_draw.inc: whether a full clear of v (colour slot, -1 depth) can be made in the bound targets' pass
static struct { p_set_rt rt; int has_rt; p_scissor sc; int has_sc; } R;
static int full_scissor(const vtex *v, unsigned level) {
    if (!R.has_sc) return 1;
    uint32_t w = v->w >> level ? v->w >> level : 1, h = v->h >> level ? v->h >> level : 1;
    return R.sc.x <= 0 && R.sc.y <= 0 && (int64_t)R.sc.x + R.sc.w >= (int64_t)w && (int64_t)R.sc.y + R.sc.h >= (int64_t)h;
}
static VkImageSubresourceRange view_range(const vtex *v, const view_t *vw, int has, VkImageAspectFlags a) {
    VkImageSubresourceRange r = {a, has && vw->set_levels ? vw->base_level : 0, has && vw->set_levels && vw->nlevels ? vw->nlevels : 1,
                                 has && vw->set_layers ? vw->min_layer : 0, has && vw->set_layers && vw->nlayers ? vw->nlayers : v->layers};
    if (r.baseMipLevel >= v->levels) r.baseMipLevel = v->levels - 1;
    if (r.baseMipLevel + r.levelCount > v->levels) r.levelCount = v->levels - r.baseMipLevel;
    if (r.baseArrayLayer >= v->layers) r.baseArrayLayer = 0;
    if (r.baseArrayLayer + r.layerCount > v->layers) r.layerCount = v->layers - r.baseArrayLayer;
    return r;
}
static VkClearColorValue clear_value(VkFormat f, const float rgba[4]) {
    VkClearColorValue c;
    switch (f) {
        case VK_FORMAT_R8_UINT: case VK_FORMAT_R16_UINT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R32G32_UINT:
        case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_A2B10G10R10_UINT_PACK32:
            for (int i = 0; i < 4; i++) c.uint32[i] = (uint32_t)rgba[i]; break;
        case VK_FORMAT_R8_SINT: case VK_FORMAT_R16_SINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R32G32_SINT:
        case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R32G32B32A32_SINT:
            for (int i = 0; i < 4; i++) c.int32[i] = (int32_t)rgba[i]; break;
        default: memcpy(c.float32, rgba, sizeof c.float32);
    }
    return c;
}
static void clear_color(const p_clear_color *cc) {
    if (!R.has_rt || cc->index < 0 || cc->index >= R.rt.n || !R.rt.color[cc->index]) { RED_ONCE(60, "color clear of target %d, which is not set", cc->index); return; }
    vtex *v = vt_get_w(R.rt.color[cc->index]); if (!v) return;
    VkImageSubresourceRange rg = view_range(v, &R.rt.cview[cc->index], R.rt.has_cview[cc->index], VK_IMAGE_ASPECT_COLOR_BIT); rg.levelCount = 1;
    if ((cc->mask & 0xf) != 0xf || !full_scissor(v, rg.baseMipLevel)) { if (cc->mask & 0xf) clear_color_pass(cc); return; }
    // a full clear of a target the size of the bound targets' pass: in the pass (vkCmdClearAttachments), on a tiler a clear of the tile memory, instead of writing the
    // whole image to memory outside it, a full barrier, and the pass loading it back. Only one layer of one level is a pass attachment
    if (rg.layerCount == 1 && clear_in_pass(v, cc->index, rg.baseMipLevel, 0)) { clear_color_pass(cc); return; }
    VkCommandBuffer c = cb(); if (!c) return;
    VkClearColorValue cv = clear_value(v->fmt, cc->rgba);
    vkCmdClearColorImage(c, v->img, VK_IMAGE_LAYOUT_GENERAL, &cv, 1, &rg); cb_barrier();
}
static void clear_ds(const p_clear_ds *cd) {
    if (!R.has_rt || !R.rt.depth) { RED_ONCE(62, "depth-stencil clear with no depth target set"); return; }
    vtex *v = vt_get_w(R.rt.depth); if (!v) return;
    VkImageAspectFlags a = (cd->depth_mask ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | ((cd->stencil_mask & 0xff) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    a &= v->aspect; if (!a) return;
    if (((cd->stencil_mask & 0xff) && (cd->stencil_mask & 0xff) != 0xff) || !full_scissor(v, 0)) { clear_ds_pass(cd, a); return; }
    VkImageSubresourceRange rg = view_range(v, &R.rt.dview, R.rt.has_dview, a); rg.levelCount = 1;
    if (rg.layerCount == 1 && clear_in_pass(v, -1, rg.baseMipLevel, 1)) { clear_ds_pass(cd, a); return; }   // in the pass, as colour clears
    VkCommandBuffer c = cb(); if (!c) return;
    VkClearDepthStencilValue dv = {cd->depth, (uint32_t)cd->stencil};
    vkCmdClearDepthStencilImage(c, v->img, VK_IMAGE_LAYOUT_GENERAL, &dv, 1, &rg); cb_barrier();
}
static void clear_tex(const p_clear_tex *ct) {
    vtex *v = vt_get_w(ct->tex); if (!v) return;
    VkImageSubresourceRange rg = view_range(v, &ct->view, ct->has_view, VK_IMAGE_ASPECT_COLOR_BIT); rg.levelCount = 1;
    uint32_t w = v->w >> rg.baseMipLevel ? v->w >> rg.baseMipLevel : 1, h = v->h >> rg.baseMipLevel ? v->h >> rg.baseMipLevel : 1;
    if ((ct->mask & 0xf) != 0xf || ct->r.xoffset || ct->r.yoffset || (uint32_t)ct->r.width < w || (uint32_t)ct->r.height < h) { RED_ONCE(64, "texture clear of a region or with a channel mask: not done yet"); return; }
    if (v->aspect != VK_IMAGE_ASPECT_COLOR_BIT) { RED_ONCE(65, "ClearTexture of a depth texture: not done yet"); return; }
    VkCommandBuffer c = cb(); if (!c) return;
    VkClearColorValue cv; if (ct->is_uint) memcpy(cv.uint32, ct->c.u, sizeof cv.uint32); else cv = clear_value(v->fmt, ct->c.f);
    vkCmdClearColorImage(c, v->img, VK_IMAGE_LAYOUT_GENERAL, &cv, 1, &rg); cb_barrier();
}

static int png_texels(const char *path, VkFormat f, int bpp, const uint8_t *p, uint32_t w, uint32_t x, uint32_t y, uint32_t cw, uint32_t ch, int flip);   // below (the pass dump's PNGs)
#include "gfx_vk_posrows.inc"   // the vertex shaders whose position reads its constant buffer rows at run time (vk_pos_rows)
#include "gfx_vk_draw.inc"
#include "gfx_vk_mesh.inc"
#include "gfx_vk_meshemu.inc"   // the same draws in compute form, on a GPU without VK_EXT_mesh_shader

// The front end's first window failed on a DM_VK_DRIVER driver (front_android.c vk_window_first), which has destroyed what it made: the device and instance are destroyed
// too and made again on the system's driver. Only while nothing else holds objects of them, before the host starts: no texture, buffer, pipeline or cache yet. Here,
// after gfx_vk_draw.inc, for raw global memory's state (g_gm_on). 0: no DM_VK_DRIVER driver was running, or no device was made
int vkb_fallback(const char *why) {
    if (!vk_driver_name()) return 0;
    pthread_mutex_lock(&g_vk_mu);
    if (g_vk_ok) { vkDeviceWaitIdle(g_vk.dev); vkDestroyDevice(g_vk.dev, NULL); }
    if (g_vk.inst) vkDestroyInstance(g_vk.inst, NULL);
    g_vk_ok = 0; g_qreset = NULL; g_fault_info = NULL; g_smid = NULL; g_gm_on = 0; g_mesh = 0; g_draw_mesh = NULL;
    pthread_mutex_destroy(&g_vk.qmu); memset(&g_vk, 0, sizeof g_vk); pthread_mutex_init(&g_vk.qmu, NULL);
    if (vk_driver_fallback(why)) vk_make();
    pthread_mutex_unlock(&g_vk_mu);
    return g_vk_ok;
}
#include "gfx_vk_dscopy.inc"
#include "gfx_vk_bcdec.inc"   // a GPU block encoder's blocks decoded into a BC texture the CPU decoded (bcd_decode)
#include "gfx_vk_upsq.inc"   // a game's upscaler network replaced by a bilinear scale and a signed square (upsq_run)
#include "gfx_vk_readback.inc"   // arena write-backs read through a cached copy where the arena reads slowly (DM_VK_WB_READBACK)
#include "gfx_vk_shadow.inc"   // DM_VK_SKIP_SHADOWS: a game's shadow map passes left out (vk_shadow_rt)
#include "gfx_vk_vsync.inc"   // the pacing grid on the display's vsyncs, and each frame's time to be shown (DM_VK_PRESENT_TIMING)

// ---------------- Submission ----------------
static void post(post_op o) {   // done when the current chunk completes, after its write-backs
    if (!cur()) return;
    if (C->npost == C->cappost) { C->cappost = C->cappost ? 2 * C->cappost : 64; C->post = realloc(C->post, C->cappost * sizeof *C->post); }
    C->post[C->npost++] = o;
}
// ---------------- The frame's work, for the performance hint (plat_android.c dm_android_hint_frame) ----------------
// Present to present, from the end of the last present's pacing sleep to the start of this one's, less the acquires' pacing waits in between (hle_nvn.c dm_acquire_paced):
// the frame step's cost (fstep_tick: the time between frame starts less the acquire's wait) with mtl_present's own sleep left out as well. That sleep counted as work,
// so at hold 3 the cost was 3 vsyncs whatever the work (Breath of the Wild on an Ayn Odin 2: about 43 ms of work and about 7 ms asleep in present per 50 ms frame);
// reported to the hint, it would have asked for full boost at any load. Wall clock, as the hint's contract asks: the replay's waits for the GPU (about 5 ms a
// frame there) are part of the frame. The threads that acquired or replayed since the last present go with it, so the session follows the threads doing the work
static _Atomic uint64_t g_hint_paced, g_hint_gen = 1;   // the acquires' pacing since the last present; presents so far (+ 1), for hint_note
static pthread_mutex_t g_hint_mu = PTHREAD_MUTEX_INITIALIZER; static int32_t g_hint_tid[4]; static int g_hint_ntid;   // 4: the acquiring, submitting and replay threads, and the recorder (gfx_vk_rec.inc)
static void hint_note(void) {   // the calling thread works on this frame (once per thread and frame)
    static __thread uint64_t seen; uint64_t g = atomic_load_explicit(&g_hint_gen, memory_order_relaxed);
    if (seen == g) return;
    seen = g; int32_t t = gettid();
    pthread_mutex_lock(&g_hint_mu);
    int i = 0; while (i < g_hint_ntid && g_hint_tid[i] != t) i++;
    if (i == g_hint_ntid && i < 4) g_hint_tid[g_hint_ntid++] = t;
    pthread_mutex_unlock(&g_hint_mu);
}
void dm_acquire_paced(uint64_t ns) { atomic_fetch_add_explicit(&g_hint_paced, ns, memory_order_relaxed); hint_note(); }
// The frame's deadline, the target that work is held against, follows the frame step's hold (hle_nvn.c nvn_fstep_hold, read here after fstep_tick decided this frame's): the cost
// its fit trial needs to try the next hold down, FSTEP_FIT × (k − 1) vsyncs, and at kmin the same share of kmin, the margin that keeps it. The declared period (rt_period_ns) is
// kmin's whatever the hold: at Breath of the Wild's hold 3 on the Odin 2 it put 16.7 ms against about 43 ms of work, every frame about 2.6 times over, which holds a session at
// full boost for as long as the hold is raised, at any load and through thermal mitigation: its answer no longer depends on the work. Aimed at k − 1 it asks for what the next hold
// takes (28.3 ms at hold 3, 14.2 ms at holds 2 and 1 at a 60 fps cap; 28.3 ms at a 30 cap), so it eases off once the work fits it, and with the full k − 1 instead the work
// would settle just above what the fit trial accepts and stay boosted at hold k. The frame step sees that cost once its own cost leaves the present's sleep out (plan A1).
// Without the frame step (no cap, the game's own step) the target is the declared period. *thermal: the thermal cap holds 30 fps (plat_android.c closes the session then)
static void hint_frame(uint64_t now, uint64_t from) {   // the present thread, before its pacing sleep; from: when the last one ended (0 = no present yet)
    void dm_android_hint_frame(uint64_t work_ns, const int32_t *tids, int n, uint64_t hold_ns, int thermal);
    uint64_t nvn_fstep_hold(int *k, int *kmin, int *thermal);
    int32_t t[4]; pthread_mutex_lock(&g_hint_mu); int n = g_hint_ntid; memcpy(t, g_hint_tid, sizeof t); g_hint_ntid = 0; pthread_mutex_unlock(&g_hint_mu);
    atomic_fetch_add_explicit(&g_hint_gen, 1, memory_order_relaxed);
    uint64_t paced = atomic_exchange_explicit(&g_hint_paced, 0, memory_order_relaxed), span = from && now > from ? now - from : 0;
    int k, kmin, th; uint64_t vs = nvn_fstep_hold(&k, &kmin, &th), hold = vs ? (uint64_t)(FSTEP_FIT * (k - 1 > kmin ? k - 1 : kmin) * (double)vs) : 0;
    dm_android_hint_frame(span > paced && span < 1000000000ull ? span - paced : 0, t, n, hold, th);   // a second or more since the last present (loading, the app in the background) is no frame
}
// DM_VK_PLACE_LOG=1: the CPUs the replay ran on, sampled at each submit and logged every 300 presents (mtl_present); the placement experiment's switches: plat_android.c
static int place_log_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_PLACE_LOG"); v = e && atoi(e); } return v; }
static unsigned g_place_cpu[16], g_place_n;   // under g_bk
// ---------- Conditional rendering (profile render_cond; nvn_mtl.m OP_RENDER_COND, which this follows) ----------
// SetRenderEnableConditional(mode, rec) enables rendering if the 64-bit counter of the report at rec equals (RENDER_IF_EQUAL) or differs from (RENDER_IF_NOT_EQUAL) the one at
// rec + 16, until the next SetRenderEnable(true). Tears of the Kingdom and Animal Crossing: New Horizons wrap their clears this way: rec is set to 1 from the CPU when the target
// needs a clear, each clear is followed by ReportCounter(TIMESTAMP_TOP, rec), and rec + 16 is never written. A timestamp report puts 0 in the counter field (post_apply), so
// after one clear the two compare equal and later conditional clears of that target are skipped. Ignored, every clear ran: Animal Crossing's four clear-only passes wiped its
// composite before the window copy, and every frame was black on an Ayn Odin 2.
// Evaluated here at replay, on the CPU, from the records in memory, as on Metal (Vulkan's VK_EXT_conditional_rendering reads a 32-bit value, not two 64-bit counters). A report
// replayed earlier may not be written yet (the completion thread does it): a timestamp one counts with its known counter 0; a samples-passed result is unknown, and a condition
// on it keeps drawing (occlusion culling only saves work). Inside a block evaluated false, draws and clears are dropped. The state carries over from one of the queue's
// continuations to the next (hle_nvn.c nvn_queue.inc splits a submission at every event signal and wait), as nvn_mtl.m keeps it: reset per continuation, a block opened
// before a signal lost its condition
static int g_rc_skip;   // a render-enable block evaluated false is active
static uint64_t g_rc_skipped;   // clears and draws dropped by it (the first is logged)
#define RC_PEND 256
static struct { uint64_t addr; int kind; } g_rc_pend[RC_PEND]; static int g_rc_npend;   // reports replayed in this submission: kind 1 = timestamp (counter 0), 2 = another counter (unknown)
// Addresses ever written by a non-timestamp report, each with the NVN fence of the submission its last samples-passed report is in (RC_ANY: a counter kind the host does not
// implement, unknown for good). A samples-passed result is written when its chunk completes (post_apply, before the fence: chunk_thread), so a condition on it reads memory once
// that fence has completed, and keeps drawing while it is in flight, as the Mac does ([Occlusion conditions read once landed]): every such condition drew before, and Ocarina of
// Time reports its queries a submission ahead of the conditional draws (on the Mac, 42% of Kokiri Forest's 1.48 million conditions were false). An open-addressing table grown
// at half full. It was a fixed 4,096 slots, which Kokiri Forest filled (its counter reports go to ever new addresses), and once full every report and every condition probed all
// 4,096: 37% of the replay thread on a Galaxy Z Fold 8. Past RC_OCC_MAX slots it is emptied and filled again (logged): a condition on an address forgotten so reads its memory.
// DM_VK_RC_LANDED=0 (env.txt): every samples-passed condition keeps drawing, as before
enum { RC_OCC_MIN = 4096, RC_OCC_MAX = 1u << 21 };
#define RC_ANY UINT64_MAX
static uint64_t *g_rc_occ, *g_rc_occf; static uint8_t *g_rc_occr; static uint32_t g_rc_cap, g_rc_n;   // address, fence, read by a condition since (the stats line: how many reports a condition ever reads); the replay's thread alone (mtl_exec)
static uint64_t g_q_rep_read, g_q_rep_new;   // samples-passed reports to an address a condition has read before / to one none has
static uint64_t g_rc_landed, g_rc_flying;   // samples-passed conditions read from memory / still in flight (drawn)
static int rc_landed_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_RC_LANDED"); v = !(e && *e == '0') && occl_mode(); if (!v) LOG("[vk] %s: every samples-passed condition keeps drawing", occl_mode() ? "DM_VK_RC_LANDED=0" : "DM_VK_OCCLUSION=0"); } return v; }
static uint32_t rc_occ_slot(const uint64_t *t, uint32_t cap, uint64_t a) { uint32_t i = (uint32_t)((a >> 4) * 0x9E3779B1u) & (cap - 1); while (t[i] && t[i] != a) i = (i + 1) & (cap - 1); return i; }
static void rc_occ_add(uint64_t a, uint64_t fence) {
    if (!a) return;   // (0 is an empty slot)
    if (2 * (g_rc_n + 1) > g_rc_cap) {
        uint32_t nc = g_rc_cap ? 2 * g_rc_cap : RC_OCC_MIN; int clear = nc > RC_OCC_MAX; if (clear) nc = g_rc_cap;
        uint64_t *nt = calloc(nc, sizeof *nt), *nf = calloc(nc, sizeof *nf); uint8_t *nr = calloc(nc, 1); if (!nt || !nf || !nr) { free(nt); free(nf); free(nr); return; }
        if (clear) { static int nl; if (nl++ < 4) LOG("[vk] conditional rendering: %u counter report addresses kept, the most it keeps: emptied", g_rc_n); g_rc_n = 0; }
        else { for (uint32_t i = 0; i < g_rc_cap; i++) if (g_rc_occ[i]) { uint32_t k = rc_occ_slot(nt, nc, g_rc_occ[i]); nt[k] = g_rc_occ[i]; nf[k] = g_rc_occf[i]; nr[k] = g_rc_occr[i]; }
               if (nc >= 65536) LOG("[vk] conditional rendering: %u counter report addresses kept, room for %u now", g_rc_n, nc / 2); }
        free(g_rc_occ); free(g_rc_occf); free(g_rc_occr); g_rc_occ = nt; g_rc_occf = nf; g_rc_occr = nr; g_rc_cap = nc;
    }
    uint32_t i = rc_occ_slot(g_rc_occ, g_rc_cap, a);
    if (fence != RC_ANY) { if (g_rc_occ[i] && g_rc_occr[i]) g_q_rep_read++; else g_q_rep_new++; }
    if (!g_rc_occ[i]) { g_rc_occ[i] = a; g_rc_occf[i] = fence; g_rc_n++; }
    else if (g_rc_occf[i] != RC_ANY && (fence == RC_ANY || fence > g_rc_occf[i])) g_rc_occf[i] = fence;   // the latest report; unknown kinds for good
}
static int rc_occ_get(uint64_t a, uint64_t *fence) { if (!g_rc_cap || !a) return 0; uint32_t i = rc_occ_slot(g_rc_occ, g_rc_cap, a); if (g_rc_occ[i] != a) return 0; *fence = g_rc_occf[i]; g_rc_occr[i] = 1; return 1; }
static void rc_note(uint64_t a, int kind) {   // kind 1 a timestamp, 2 a counter kind the host does not implement, 3 samples passed
    if (kind >= 2) rc_occ_add(a, kind == 3 && rc_landed_on() ? atomic_load(&g_fence) + 1 : RC_ANY);   // (the fence this submission completes: mtl_sub_commit)
    if (kind == 3) kind = 2;   // (in this submission: not landed)
    for (int i = 0; i < g_rc_npend; i++) if (g_rc_pend[i].addr == a) { g_rc_pend[i].kind = kind; return; }
    if (g_rc_npend == RC_PEND) { memmove(g_rc_pend, g_rc_pend + RC_PEND / 2, sizeof *g_rc_pend * (RC_PEND / 2)); g_rc_npend = RC_PEND / 2; }   // the older half has landed by now
    g_rc_pend[g_rc_npend].addr = a; g_rc_pend[g_rc_npend++].kind = kind;
}
static int rc_value(uint64_t a, uint64_t *v) {   // 0 = unknown at replay
    for (int i = g_rc_npend - 1; i >= 0; i--) if (g_rc_pend[i].addr == a) { if (g_rc_pend[i].kind != 1) return 0; *v = 0; return 1; }   // a report of this submission first: not landed
    uint64_t f;
    if (rc_occ_get(a, &f)) {
        if (f == RC_ANY || atomic_load(&g_done) < f) { if (f != RC_ANY) g_rc_flying++; return 0; }
        g_rc_landed++;
    }
    *v = *(const volatile uint64_t *)a; return 1;
}
static int rc_skips(void) {   // the clear or draw at hand is dropped
    if (!g_rc_skip) return 0;
    if (!g_rc_skipped++) LOG("[vk] conditional rendering (render_cond): a clear or draw in a block evaluated false is dropped, as the GPU would");
    return 1;
}
void mtl_sub_begin(void) {
    pthread_mutex_lock(&g_bk); t_bk_sub = 1; hint_note();
    gm_lazy_tick();   // chunks of large pools placed between submissions (gfx_vk_gmem_lazy.inc)
    static __thread int placed; if (!placed) { placed = 1; void dm_android_place_thread(int done); dm_android_place_thread(0); }
    if (place_log_on()) { int c = sched_getcpu(); g_place_cpu[c >= 0 && c < 16 ? c : 15]++; g_place_n++; }
}
void mtl_sub_index(const uint64_t *h, int n) { (void)h; (void)n; }
void mtl_exec_rest(const uint64_t *handles, int n) { (void)handles; (void)n; }
// Buffer copies and fills: done on the CPU (GPU address = CPU address). A large one goes to the completion thread, in its place in the queue, so the submitting
// thread (the game's) does not stop for it, as it would not on the GPU; a small one now, after the writes before it
enum { COP_MIN = 1 << 20 };
static int cop_add(uint64_t src, uint64_t dst, uint64_t n, uint32_t value, int fill) {
    if (n < COP_MIN || !cur()) return 0;
    g_vs = 4; wb_view();   // the GPU's earlier writes land first; queued copies stay queued (done in order)
    if (!cur()) return 0;
    if (C->ncop == C->capcop) { C->capcop = C->capcop ? 2 * C->capcop : 8; C->cop = realloc(C->cop, C->capcop * sizeof *C->cop); }
    C->cop[C->ncop++] = (cop_t){src, dst, n, value, fill};
    return 1;
}
void mtl_sub_copy(uint64_t src, uint64_t dst, uint64_t n) { if (!n || cop_add(src, dst, n, 0, 0)) return; mem_view(src, n, dst, n); memmove((void *)dst, (const void *)src, n); }
void mtl_sub_fill(uint64_t dst, uint64_t n, uint32_t value) { if (cop_add(0, dst, n, value, 1)) return; mem_view(0, 0, dst, n); uint8_t *d = (uint8_t *)dst; for (uint64_t i = 0; i + 4 <= n; i += 4) memcpy(d + i, &value, 4); }
// q0: the fence of the submission it ends (nvn_queue.inc ends a continuation at each signal), for the queue scheduler's event ordering (nvn_event_publish_gpu)
void mtl_sub_event(volatile uint32_t *val, uint32_t mode, uint32_t v) { post((post_op){1, val, mode, v, 0, 0, atomic_load(&g_fence) + 1, 0}); }
void mtl_sub_counter(uint64_t addr, uint32_t type) { post((post_op){2, NULL, 0, 0, addr, type}); }
static uint64_t gpu_ticks(void) { uint64_t g = guest_ns(); return nvn_timestamp_from_ns(g ? g : clock_gettime_nsec_np(CLOCK_MONOTONIC)); }
enum { EARLY_DRAWS = 48, MID_DRAWS = 64 };   // MID_DRAWS: a chunk that long goes to the GPU before the next draw, in the middle of a pass (the pass is begun again with its targets
// loaded): Super Mario Bros. Wonder draws most of a frame in two passes of about 140 draws, which reached the GPU only when each ended, and the game then waited for the GPU
// (SyncWait 12.5 ms a wait) after recording 23 ms of draws
// g_mid_draws: MID_DRAWS, DM_VK_MID_DRAWS=<n> (env.txt, or a game's android_defaults.txt), or a file named vk_mid beside $DM_DUMP_DIR holding a number, read every 30 presents
// (a test of the split's cost: each piece loads and stores the pass's targets). A game whose passes are long and whose GPU is not waited for draw by draw is better
// with fewer cuts: Breath of the Wild on a OnePlus Pad 3 (about 3,600 draws a frame) at 512 instead of 64 took 1.5 ms less of the replay's CPU a frame (30.6 against
// 32.2: fewer submissions and command buffer ends) and 2.9 ms less GPU time (18.7 against 21.6: fewer passes ended and begun again with their targets loaded). Measured
// with descriptor set reuse on and one draw that hangs that GPU skipped by a local test switch (the device is lost without it), one run per value
static int g_mid_draws = MID_DRAWS;
static void mid_poll(uint64_t presents) {
    static int init; if (!init) { init = 1; const char *e = getenv("DM_VK_MID_DRAWS"); int v = e ? atoi(e) : 0;
        if (v > 0) { g_mid_draws = v; LOG("[vk] chunks sent in the middle of a pass after %d draws (DM_VK_MID_DRAWS)", v); } }
    const char *dir = getenv("DM_DUMP_DIR"); if (!dir || presents % 30) return;
    char p[700]; snprintf(p, sizeof p, "%s/../vk_mid", dir); FILE *f = fopen(p, "r"); if (!f) return;
    int v = 0; if (fscanf(f, "%d", &v) == 1 && v > 0 && v != g_mid_draws) { LOG("[vk] chunks sent in the middle of a pass after %d draws (was %d)", v, g_mid_draws); g_mid_draws = v; }
    fclose(f);
}
static uint64_t g_op_warned;   // ops this replay does not handle, logged once each
static uint64_t g_mesh_skipped;   // task / mesh draws skipped (OP_DRAW with DM_PRIM_MESH_TASKS)
void mtl_exec(const uint8_t *p) {
    for (;;) {
        op_hdr h; memcpy(&h, p, sizeof h);
        const uint8_t *a = p + 8;
        // a render pass stays open over what is done on the CPU (uniform buffer updates, buffer copies and fills, events, counters, fences: the draws took their copies of guest
        // memory when they were recorded): Super Mario Bros. Wonder updates a uniform buffer before nearly every draw, and ending the pass there made 400 passes a frame, each
        // loading and storing its targets (on a tiler GPU the whole frame's bandwidth again and again)
        int cpu_op = h.op == OP_UPDATE_UB || h.op == OP_SIGNAL_EVENT || h.op == OP_REPORT_COUNTER || h.op == OP_FENCE_SYNC || h.op == OP_COPY_STRIDE || h.op == OP_CALL
                     || h.op == OP_CLEAR_BUF || h.op == OP_COPY_B2B
                     || h.op == OP_CLEAR_COLOR || h.op == OP_CLEAR_DS || h.op == OP_RESET_COUNTER || h.op == OP_RENDER_COND;   // clears of the bound targets are made in their pass when they can be (clear_color); the others end it themselves (cb())
        if (P.active && h.op != OP_DRAW && h.op != OP_DRAW_IDX && h.op != OP_SCISSOR && h.op != OP_JUMP && h.op != OP_END && !cpu_op && !draw_state_is(h.op)) rp_end();
        switch (h.op) {
            case OP_END: return;
            case OP_JUMP: memcpy(&p, a, 8); continue;
            case OP_CALL: { uint64_t t; memcpy(&t, a, 8); mtl_exec((const uint8_t *)t); break; }
            case OP_SIGNAL_EVENT: { volatile uint32_t *val; uint32_t m, v; memcpy(&val, a, 8); memcpy(&m, a + 8, 4); memcpy(&v, a + 12, 4); mtl_sub_event(val, m, v); break; }
            case OP_REPORT_COUNTER: { uint64_t t, ad; memcpy(&t, a, 8); memcpy(&ad, a + 8, 8);
                if (DM_GAME_RENDER_COND) rc_note(ad, t == 0 || t == 0x10 ? 1 : t == NVN_COUNTER_TYPE_SAMPLES_PASSED ? 3 : 2);
                if (t == NVN_COUNTER_TYPE_SAMPLES_PASSED) { occl_end(); g_q_reports++; post((post_op){3, NULL, 0, 0, ad, (uint32_t)t, g_q_on ? g_q_win : g_qslot, g_qslot}); }
                else if (C && C->tq && C->ntq < 256 && (t == 0 || t == 0x10)) {   // a timestamp: written by the GPU at this point of the commands, so two of them time the GPU work between
                    // (Super Mario Bros. Wonder times its frame's GPU work for its dynamic resolution; the host used to write every report of a chunk with one time, its completion)
                    VkCommandBuffer cbuf = C->open ? C->cb : cb(); if (!cbuf) break;
                    g_ts_reports++; int q = C->ntq++; vkCmdWriteTimestamp(cbuf, t == 0x10 ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, C->tq, (uint32_t)q);
                    post((post_op){4, NULL, 0, 0, ad, (uint32_t)t, (uint64_t)q, 0}); }
                else mtl_sub_counter(ad, (uint32_t)t);
                break; }
            case OP_RENDER_COND: { const p_render_cond *rc = (const p_render_cond *)a;   // recorded only with the profile key render_cond (hle_nvn.c)
                if (rc->mode >= 2) g_rc_skip = rc->mode == 3;
                else { uint64_t x = 0, y = 0; int known = rc_value(rc->addr, &x) & rc_value(rc->addr + 16, &y); g_rc_skip = known && (rc->mode ? x == y : x != y); }
                break; }
            case OP_RESET_COUNTER: { uint64_t t; memcpy(&t, a, 8); if (t == NVN_COUNTER_TYPE_SAMPLES_PASSED) { occl_end(); g_q_on = 1; g_q_win = g_qslot; g_q_resets++; } break; }
            case OP_FENCE_SYNC: { sync_t *s; memcpy(&s, a, 8); s->fence = atomic_load(&g_fence) + 1; s->fenced = 1; break; }
            case OP_CLEAR_BUF: { const p_clear_buf *c = (const p_clear_buf *)a; mtl_sub_fill(c->addr, c->size, c->value); break; }
            case OP_COPY_B2B: { uint64_t s, d, n; memcpy(&s, a, 8); memcpy(&d, a + 8, 8); memcpy(&n, a + 16, 8); mtl_sub_copy(s, d, n); break; }
            case OP_UPDATE_UB: { uint64_t d, n; memcpy(&d, a, 8); memcpy(&n, a + 8, 8); mem_view(0, 0, d, n); if (n) memcpy((void *)d, a + 16, n); break; }
            case OP_COPY_STRIDE: { const p_copy_stride *c = (const p_copy_stride *)a; if (c->image) S.img = c->stride; else S.row = c->stride; break; }
            case OP_COPY_B2T: case OP_COPY_T2B: {   // the command buffer's copy strides travel in the record (nvn_obj.h copy_strides); none: tightly packed
                const p_copy_strides *cs = copy_strides(a, h.size); S.row = cs ? cs->row : 0; S.img = cs ? cs->image : 0;
                if (h.op == OP_COPY_B2T) copy_b2t((const p_copy_b2t *)a); else copy_t2b((const p_copy_b2t *)a);
                break; }
            case OP_COPY_T2T: copy_t2t((const p_copy_t2t *)a); break;
            case OP_SET_RT:   // new render targets: a chunk with enough work goes to the GPU now, so that it draws while the rest of the submission is recorded
                // (one command buffer per frame kept the GPU idle until the CPU had recorded everything: Super Mario Bros. Wonder's level took CPU 29 ms + GPU 41 ms
                // a frame one after the other). A dispatch's storage buffer read by later draws on the GPU is found in the chunks in flight too (fwd_find)
                if (C && C->ndraw >= EARLY_DRAWS && !vk_safe()) flush();
                R.rt = *(const p_set_rt *)a; R.has_rt = 1; g_sh_rt = shadow_rt(&R.rt); break;
            case OP_SCISSOR: R.sc = *(const p_scissor *)a; R.has_sc = 1; break;
            case OP_CLEAR_COLOR: if (!rc_skips()) clear_color((const p_clear_color *)a); break;
            case OP_CLEAR_DS: if (!rc_skips()) clear_ds((const p_clear_ds *)a); break;
            case OP_CLEAR_TEX: clear_tex((const p_clear_tex *)a); break;
            case OP_DRAW: if (rc_skips() || shadow_skips()) break;
                if (C && C->ndraw >= g_mid_draws && !vk_safe()) flush();
                { p_draw t; const p_draw *dw = draw_rec(a, h.size, &t);   // short records: draw_rec (nvn_obj.h)
                  if (dw->prim == DM_PRIM_MESH_TASKS) {   // NVN2's DrawMeshTasks: on VK_EXT_mesh_shader (gfx_vk_mesh.inc); the Adreno 7xx drivers and Turnip have none, and no lowering is ported
                      if (g_mesh || memu_on()) { if (!vk_mesh_draw(dw)) g_draws_dropped++; break; }   // VK_EXT_mesh_shader, else their compute form (gfx_vk_meshemu.inc)
                      RED_ONCE(131, "task / mesh shader draws (NVN2 DrawMeshTasks) are skipped: no VK_EXT_mesh_shader on this GPU (Mario Kart World's roadside bushes and grass tufts)"); g_mesh_skipped++; break; }
                  if (!vk_draw(dw, NULL)) g_draws_dropped++; break; }
            case OP_DRAW_IDX: if (rc_skips() || shadow_skips()) break;
                if (C && C->ndraw >= g_mid_draws && !vk_safe()) flush();
                { p_draw_idx t; if (!vk_draw(NULL, draw_idx_rec(a, h.size, &t))) g_draws_dropped++; break; }
            case OP_DISPATCH: if (!vk_dispatch((const p_dispatch *)a)) g_draws_dropped++; break;
            case OP_BARRIER: { int32_t bits; memcpy(&bits, a, 4); if (bits) g_cs_gbar = 1; break; }   // the replay's own syncs stand for barriers, apart from the order of dispatches (cs_game)
            case OP_VP_SWIZZLE: case OP_BEGIN: break;   // viewport swizzles are not replayed (as before the unhandled-op line)
            default: if (!draw_state_op(h.op, a) && h.op < 64 && !(g_op_warned & 1ull << h.op)) { g_op_warned |= 1ull << h.op; LOG("[RED] [vk] command op %u is not replayed on Vulkan: skipped", h.op); }
                break;   // state for draws (programs, bindings, render state)
        }
        if (!h.size) { LOG("[RED] [vk] command record of size 0 (op %u) at %p: the rest of the list is skipped", h.op, (const void *)p); return; }
        p += h.size;
    }
}
static void post_apply(const post_op *o) {   // the completion thread, when the GPU has done the commands before it
    if (o->kind == 1) { nvn_event_publish_gpu(o->val, o->mode, o->v, o->q0); return; }   // writes the word and releases the queues waiting on it (nvn_queue.inc)
    // render_cond: a timestamp report's counter field is 0, as on the GPU, which the conditions after it compare (rc_value); without the key it holds the time, as before
    int ts = (o->type == 0 || o->type == 0x10) && !DM_GAME_RENDER_COND;
    uint64_t now = gpu_ticks(), rec[2] = {o->type == NVN_COUNTER_TYPE_SAMPLES_PASSED ? 0 : ts ? now : 0, now};
    if (o->kind == 4) { rec[0] = ts ? o->q1 : 0; rec[1] = o->q1; }   // a timestamp the GPU wrote where the report was (gfx_vk.c OP_REPORT_COUNTER)
    if (o->kind == 3) { uint64_t sum = 0, a = o->q1 - o->q0 > QRES ? o->q1 - QRES : o->q0; for (uint64_t q = a; q < o->q1; q++) sum += g_qres[q % QRES]; rec[0] = sum; }
    dm_guest_write_prepare((void *)(uintptr_t)o->addr, sizeof rec); memcpy((void *)(uintptr_t)o->addr, rec, sizeof rec);
}
static void bound_preserve(void);   // below, with the queue scheduler's hooks
// A submission's end: its last chunk completes NVN fence f (the completion thread sets g_done); the GPU may still be at it on return
uint64_t mtl_sub_commit(void) {
    bound_preserve();
    uint64_t f = atomic_fetch_add(&g_fence, 1) + 1;
    if (!cur()) { t_bk_sub = 0; pthread_mutex_unlock(&g_bk); return f; }
    chunk_submit(f);
    t_bk_sub = 0; pthread_mutex_unlock(&g_bk);
    return f;
}
uint64_t mtl_last_fence(void) { return atomic_load(&g_fence); }
int mtl_wait(uint64_t fence, uint64_t timeout_ns) {
    if (atomic_load(&g_done) >= fence) return 1;
    if (!timeout_ns) return 0;
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t t = (uint64_t)ts.tv_nsec + (timeout_ns > 3600000000000ull ? 3600000000000ull : timeout_ns);
    ts.tv_sec += (time_t)(t / 1000000000ull); ts.tv_nsec = (long)(t % 1000000000ull);
    pthread_mutex_lock(&g_fmu);
    while (atomic_load(&g_done) < fence) if (pthread_cond_timedwait(&g_fcv, &g_fmu, &ts)) break;
    pthread_mutex_unlock(&g_fmu);
    return atomic_load(&g_done) >= fence;
}
int mtl_wait_acq(uint64_t fence, uint64_t timeout_ns) { return mtl_wait(fence, timeout_ns); }
void mtl_finish(void) { uint64_t f = atomic_load(&g_fence); while (!mtl_wait(f, 1000000000ull)) {} }   // everything submitted so far is done
void mtl_drain(void) { mtl_finish(); pthread_mutex_lock(&g_cmu); while (g_fifo_n) pthread_cond_wait(&g_ccv, &g_cmu); pthread_mutex_unlock(&g_cmu); }

// ---------------- Screenshots and PNG ----------------
// A minimal PNG writer (8-bit RGB, deflate's stored blocks: no compression library in the app) for screenshots and the pass dump's targets
static uint32_t png_crc(uint32_t c, const uint8_t *p, size_t n) {
    static uint32_t t[256];
    if (!t[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t x = i; for (int k = 0; k < 8; k++) x = x & 1 ? 0xedb88320u ^ (x >> 1) : x >> 1; t[i] = x; }
    c = ~c; while (n--) c = t[(c ^ *p++) & 0xff] ^ (c >> 8); return ~c;
}
static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void png_chunk(FILE *f, const char *type, const uint8_t *d, size_t n) {
    uint8_t h[8]; be32(h, (uint32_t)n); memcpy(h + 4, type, 4); fwrite(h, 1, 8, f); if (n) fwrite(d, 1, n, f);
    uint8_t c[4]; be32(c, png_crc(png_crc(0, (const uint8_t *)type, 4), d, n)); fwrite(c, 1, 4, f);
}
static int png_write(const char *path, const uint8_t *rgb, uint32_t w, uint32_t h) {   // rgb: w * h * 3 bytes, top row first
    size_t row = (size_t)w * 3 + 1, raw = row * h, nblk = (raw + 65534) / 65535, zn = 2 + raw + 5 * nblk + 4;
    uint8_t *z = malloc(zn); FILE *f = z ? fopen(path, "wb") : NULL;
    if (!f) { free(z); return 0; }
    uint8_t *o = z; *o++ = 0x78; *o++ = 0x01;
    uint32_t a = 1, b = 0; size_t left = raw, pos = 0;   // the stored blocks cut the filtered rows (filter byte 0 + pixels) at 65535 bytes; adler32 over the same bytes
    while (left) {
        uint16_t n = (uint16_t)(left < 65535 ? left : 65535); left -= n;
        *o++ = left ? 0 : 1; *o++ = (uint8_t)n; *o++ = (uint8_t)(n >> 8); *o++ = (uint8_t)~n; *o++ = (uint8_t)(~n >> 8);
        for (uint16_t i = 0; i < n; i++, pos++) { size_t y = pos / row, x = pos % row; uint8_t v = x ? rgb[y * (row - 1) + x - 1] : 0; *o++ = v; a = (a + v) % 65521; b = (b + a) % 65521; }
    }
    be32(o, b << 16 | a); o += 4;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'}; fwrite(sig, 1, 8, f);
    uint8_t ih[13]; be32(ih, w); be32(ih + 4, h); ih[8] = 8; ih[9] = 2; ih[10] = ih[11] = ih[12] = 0; png_chunk(f, "IHDR", ih, 13);
    png_chunk(f, "IDAT", z, (size_t)(o - z)); png_chunk(f, "IEND", NULL, 0);
    int ok = !ferror(f); fclose(f); free(z); return ok;
}
static float uf_f(uint32_t bits, int mb) { uint32_t e = bits >> mb, m = bits & ((1u << mb) - 1); return e == 31 ? 1.0f : e ? ldexpf((float)(m | 1u << mb), (int)e - 15 - mb) : ldexpf((float)m, -14 - mb); }   // B10G11R11's unsigned floats (5-bit exponent)
static uint8_t f_u8(float v) { return v <= 0 || v != v ? 0 : v >= 1 ? 255 : (uint8_t)(v * 255.0f + 0.5f); }
static int px_rgb(VkFormat f, const uint8_t *s, uint8_t *o) {   // one texel → 8-bit RGB (float formats clamped to [0, 1], no tone mapping); 0: format not converted
    uint32_t u = 0; uint16_t c16 = 0;   // read only as wide as the format: the last texel of a staging buffer has no bytes after it
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_A8B8G8R8_UNORM_PACK32: case VK_FORMAT_A8B8G8R8_SRGB_PACK32: o[0] = s[0]; o[1] = s[1]; o[2] = s[2]; return 1;
    case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: o[0] = s[2]; o[1] = s[1]; o[2] = s[0]; return 1;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: memcpy(&u, s, 4); o[0] = (uint8_t)((u & 1023) >> 2); o[1] = (uint8_t)((u >> 10 & 1023) >> 2); o[2] = (uint8_t)((u >> 20 & 1023) >> 2); return 1;
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: memcpy(&u, s, 4); o[0] = f_u8(uf_f(u & 2047, 6)); o[1] = f_u8(uf_f(u >> 11 & 2047, 6)); o[2] = f_u8(uf_f(u >> 22, 5)); return 1;
    case VK_FORMAT_R16G16B16A16_SFLOAT: { uint16_t c[3]; memcpy(c, s, 6); for (int i = 0; i < 3; i++) o[i] = f_u8(half_f(c[i])); return 1; }
    case VK_FORMAT_R32G32B32A32_SFLOAT: { float c[3]; memcpy(c, s, 12); for (int i = 0; i < 3; i++) o[i] = f_u8(c[i]); return 1; }
    case VK_FORMAT_R8_UNORM: o[0] = o[1] = o[2] = s[0]; return 1;
    case VK_FORMAT_R8G8_UNORM: o[0] = s[0]; o[1] = s[1]; o[2] = 0; return 1;
    case VK_FORMAT_R5G6B5_UNORM_PACK16: { uint16_t c; memcpy(&c, s, 2); o[0] = (uint8_t)((c >> 11) * 255 / 31); o[1] = (uint8_t)((c >> 5 & 63) * 255 / 63); o[2] = (uint8_t)((c & 31) * 255 / 31); return 1; }
    case VK_FORMAT_R16_SFLOAT: memcpy(&c16, s, 2); o[0] = o[1] = o[2] = f_u8(half_f(c16)); return 1;
    case VK_FORMAT_R32_SFLOAT: { float c; memcpy(&c, s, 4); o[0] = o[1] = o[2] = f_u8(c); return 1; }
    default: return 0;
    }
}
// texels (w * h, tightly packed, bpp bytes each) → PNG, the rectangle x, y, cw, ch of them; flip: bottom row first (a lower-left origin)
static int png_texels(const char *path, VkFormat f, int bpp, const uint8_t *p, uint32_t w, uint32_t x, uint32_t y, uint32_t cw, uint32_t ch, int flip) {
    uint8_t t[3], z[16] = {0}; if (!px_rgb(f, z, t)) return 0;
    uint8_t *rgb = malloc((size_t)cw * ch * 3); if (!rgb) return 0;
    for (uint32_t r = 0; r < ch; r++) { const uint8_t *s = p + ((size_t)(y + (flip ? ch - 1 - r : r)) * w + x) * bpp; for (uint32_t c = 0; c < cw; c++) px_rgb(f, s + (size_t)c * bpp, rgb + ((size_t)r * cw + c) * 3); }
    int ok = png_write(path, rgb, cw, ch); free(rgb); return ok;
}
// The presented image (its crop rectangle) → PNG, with the replay's lock held: copied to a staging buffer and waited for
static void shot_write(vtex *v, const int crop[4], const char *path) {
    int bpp = fmt_bytes(v->fmt); uint8_t t[3], z[16] = {0};
    if (!bpp || !px_rgb(v->fmt, z, t)) { LOG("[RED] screenshot %s: format %d not converted", path, v->fmt); return; }
    stg_t s; if (!stg_new(&s, (VkDeviceSize)v->w * v->h * bpp)) { LOG("[RED] screenshot %s: no staging buffer", path); return; }
    VkCommandBuffer c = cb(); if (!c) { stg_free(&s); return; }
    VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    VkBufferImageCopy r = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {v->w, v->h, 1}};
    vkCmdCopyImageToBuffer(c, v->img, VK_IMAGE_LAYOUT_GENERAL, s.b, 1, &r);
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    all_wait();
    uint32_t x = 0, y = 0, w = v->w, h = v->h;
    if (crop && crop[2] > 0 && crop[3] > 0 && crop[0] >= 0 && crop[1] >= 0 && (uint32_t)(crop[0] + crop[2]) <= v->w && (uint32_t)(crop[1] + crop[3]) <= v->h) { x = (uint32_t)crop[0]; y = (uint32_t)crop[1]; w = (uint32_t)crop[2]; h = (uint32_t)crop[3]; }
    uint64_t sum = 0; for (VkDeviceSize i = 0; i < s.n; i += 4 * 97) sum += s.p[i] + s.p[i + 1 < s.n ? i + 1 : i];   // a coarse "is anything there" figure for the log
    if (png_texels(path, v->fmt, bpp, s.p, v->w, x, y, w, h, g_origin_ll)) LOG("[shot] frame %llu → %s (%ux%u of %ux%u, vk%d, sampled byte sum %llu)", (unsigned long long)atomic_load(&g_presents) + 1, path, w, h, v->w, v->h, v->fmt, (unsigned long long)sum);
    else LOG("[RED] screenshot %s: cannot write", path);
    stg_free(&s);
}
// Requests: dm_gfx_shot_at (k presents from now), DM_NVN_SHOT=<frame>[,<frame>...]:<path> (%d = the frame), and a file named shot_now beside $DM_DUMP_DIR
// (checked every 30 presents; removed): the next present to <DM_DUMP_DIR>/shot_<frame>.png
#define SHOTQ_MAX 16
static struct { uint64_t frame; char path[700]; } g_shotq[SHOTQ_MAX]; static int g_nshotq;
static pthread_mutex_t g_shotq_mu = PTHREAD_MUTEX_INITIALIZER;
static void shot_queue(uint64_t frame, const char *path) {
    pthread_mutex_lock(&g_shotq_mu);
    if (g_nshotq < SHOTQ_MAX) { g_shotq[g_nshotq].frame = frame; snprintf(g_shotq[g_nshotq].path, sizeof g_shotq[0].path, "%s", path); g_nshotq++; }
    else LOG("[RED] more than %d screenshot requests: %s dropped", SHOTQ_MAX, path);
    pthread_mutex_unlock(&g_shotq_mu);
}
static void shot_poll(uint64_t presents, vtex *v, const int crop[4]) {   // presents: before this one; with the replay's lock held
    static long frames[256]; static int nf = -1; static const char *fpath;
    if (nf < 0) { nf = 0; const char *e = getenv("DM_NVN_SHOT"), *c = e ? strchr(e, ':') : NULL;
        if (c) { fpath = c + 1; for (const char *q = e; q < c && nf < 256; q = strchr(q, ',') && strchr(q, ',') < c ? strchr(q, ',') + 1 : c) frames[nf++] = atol(q); } }
    const char *dir = getenv("DM_DUMP_DIR"); char p[700];
    if (dir && !(presents % 30)) { snprintf(p, sizeof p, "%s/../shot_now", dir);
        if (!access(p, F_OK)) { unlink(p); mkdir(dir, 0777); snprintf(p, sizeof p, "%s/shot_%llu.png", dir, (unsigned long long)presents + 1); shot_queue(presents + 1, p); }
        snprintf(p, sizeof p, "%s/../threads_now", dir);   // a file named threads_now: every guest thread's state, what it waits on and its backtrace (a game that stopped moving on)
        if (!access(p, F_OK)) { unlink(p); void svc_thread_dump(void); LOG("[threads] threads_now seen at present %llu", (unsigned long long)presents); svc_thread_dump(); } }
    if (!v) return;
    for (int i = 0; i < nf; i++) if ((long)presents + 1 == frames[i]) { char path[700]; snprintf(path, sizeof path, fpath, (int)presents + 1); shot_write(v, crop, path); }
    pthread_mutex_lock(&g_shotq_mu);
    for (int i = 0; i < g_nshotq;) if (presents + 1 >= g_shotq[i].frame) { char path[700]; memcpy(path, g_shotq[i].path, sizeof path); g_shotq[i] = g_shotq[--g_nshotq];
        pthread_mutex_unlock(&g_shotq_mu); shot_write(v, crop, path); pthread_mutex_lock(&g_shotq_mu); } else i++;
    pthread_mutex_unlock(&g_shotq_mu);
}
void dm_gfx_shot_at(long k, const char *path) { shot_queue(atomic_load(&g_presents) + 1 + (uint64_t)(k > 0 ? k : 0), path); }

// ---------------- Presents ----------------
static pthread_mutex_t g_frame_mu = PTHREAD_MUTEX_INITIALIZER; static vkb_frame g_frame; static uint64_t g_frame_serial;
int vkb_frame_lock(vkb_frame *f) { pthread_mutex_lock(&g_frame_mu); if (!g_frame.img) { pthread_mutex_unlock(&g_frame_mu); return 0; } *f = g_frame; return 1; }
void vkb_frame_unlock(void) { pthread_mutex_unlock(&g_frame_mu); }
void vkb_frame_forget(uint64_t serial) { pthread_mutex_lock(&g_frame_mu); if (g_frame_serial == serial) { memset(&g_frame, 0, sizeof g_frame); g_frame_serial = 0; } pthread_mutex_unlock(&g_frame_mu); }
// The last present's sleep to its vsync grid (mtl_present, presenting thread): its length (0 = it did not sleep) and when it ended, on CLOCK_MONOTONIC (the host's now_ns)
static uint64_t g_psleep_ns, g_psleep_end;
// For the frame step (hle_nvn.c fstep_tick): that sleep is pacing, as the acquire's wait is, not the frame's work. Counted as work it made the cost the hold whatever the work, so the
// hold could never come down: Breath of the Wild on an Ayn Odin 2 held 3 vsyncs for good with about 43 ms of work a frame (p50) and 7 ms asleep here (NVN submit+present p50 49.7 ms).
// DM_VK_FSTEP_SLEEP=0 counts it as work again (the old accounting, for A/B)
uint64_t mtl_present_slept_ns(uint64_t *end_ns) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("DM_VK_FSTEP_SLEEP"); on = !(e && !strcmp(e, "0")); if (!on) LOG("[vk] DM_VK_FSTEP_SLEEP=0: the frame step counts the present's sleep to its vsync grid as the frame's work"); }
    if (end_ns) *end_ns = on ? g_psleep_end : 0;
    return on ? g_psleep_ns : 0;
}
static uint64_t g_pace_per; static int g_pace_due;   // the period of the present mtl_present_pace sleeps after (both on the replay thread)
uint64_t mtl_present(const tex_t *t, double period_s, const int crop[4]) {
    pthread_mutex_lock(&g_bk);
    pcache_made_n(0);   // the driver's cache file, when a request for it came inside the gap after the last write (gfx_vk_draw.inc)
    vtex *v = vt_get(t); flush(); rec_drain();   // the frame's chunks on the queue before the front end's blit of it (rec_submit)
    vt_evict();   // images no draw has used for a while (gfx_vk_evict.inc)
    spare_sweep(atomic_load(&g_presents));   // spare image memory unused for a while (gfx_vk_spare.inc)
    dump_poll(atomic_load(&g_presents)); prof_poll(atomic_load(&g_presents)); mid_poll(atomic_load(&g_presents)); fl_poll(atomic_load(&g_presents));
    if (prof_on()) { all_wait(); prof_write(); }
    if (atomic_load(&g_presents) == 30) prewarm_start();   // the pipelines of earlier sessions (gfx_vk_draw.inc), once the game draws
    if (dump_on()) { if (v) LOG("[dump] frame %ld presents ser=%llu %ux%u vk%d", dump_frame(), (unsigned long long)v->serial, v->w, v->h, v->fmt); all_wait(); dump_write();
        if (v) { char p[700]; snprintf(p, sizeof p, "%s/f%ld_present.png", getenv("DM_DUMP_DIR") ? getenv("DM_DUMP_DIR") : ".", dump_frame()); shot_write(v, crop, p); } }
    shot_poll(atomic_load(&g_presents), v, crop);
    if (place_log_on() && g_place_n && atomic_load(&g_presents) % 300 == 299) {   // the CPUs the replay's submits ran on since the last line, in percent
        char l[256]; size_t o = 0; for (int c = 0; c < 16; c++) if (g_place_cpu[c]) o += (size_t)snprintf(l + o, sizeof l - o, " cpu%d %.0f%%", c, 100.0 * g_place_cpu[c] / g_place_n);
        LOG("[place] replay submits up to present %llu:%s (%u submits)", (unsigned long long)atomic_load(&g_presents) + 1, l, g_place_n); memset(g_place_cpu, 0, sizeof g_place_cpu); g_place_n = 0; }
    if (v) {
        pthread_mutex_lock(&g_frame_mu);
        g_frame = (vkb_frame){v->img, v->fmt, v->w, v->h, {0, 0, (int)v->w, (int)v->h}, atomic_load(&g_presents) + 1, g_origin_ll}; g_frame_serial = v->serial;
        if (crop && crop[2] > 0 && crop[3] > 0) memcpy(g_frame.crop, crop, sizeof g_frame.crop);
        g_frame.show_at = vs_show_time(period_s > 0 ? (uint64_t)(period_s * 1e9) : 0);
        pthread_mutex_unlock(&g_frame_mu);
    }
    pthread_mutex_unlock(&g_bk);
    g_pace_per = period_s > 0 ? (uint64_t)(period_s * 1e9) : 16666667ull; g_pace_due = 1;
    void front_android_present(const tex_t *t, const int crop[4]); front_android_present(t, crop);
    // DM_HITCH_DUMP (hle_thread.c) times stalls from this; only nvn_mtl.m's present set it, so on Android the dump never fired
    extern _Atomic uint64_t dm_present_ns; atomic_store(&dm_present_ns, now_ns());
    return atomic_fetch_add(&g_presents, 1) + 1;
}
// The present's sleep to its vsync grid, after the replay has let g_sub_mu go (nvn_obj.h)
void mtl_present_pace(void) {
    static uint64_t next, slept;   // slept: when the last present's pacing sleep ended, where the next frame's work starts (hint_frame)
    if (!g_pace_due) return;
    g_pace_due = 0;
    uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC);
    hint_frame(now, slept);
    int locked = vs_snap(&next, g_pace_per);   // next on the display's vsyncs (gfx_vk_vsync.inc)
    if (next > now && next - now < 1000000000ull) { mach_wait_until(next); g_psleep_end = clock_gettime_nsec_np(CLOCK_MONOTONIC); g_psleep_ns = g_psleep_end - now; } else { next = now; g_psleep_ns = 0; }
    g_vs_start = locked ? next : 0;   // where the next frame's work starts: on the grid, or now when late
    next += g_pace_per; slept = clock_gettime_nsec_np(CLOCK_MONOTONIC);
}
void mtl_present_wait(uint64_t ticket) { (void)ticket; }
void mtl_present_flush(void) {}
double mtl_presented_time(uint64_t n) { (void)n; return 0; }
int mtl_display_hz(void) { return 60; }
int mtl_has_window(void) { int front_has_window(void); return front_has_window(); }
int mtl_fps_target(void) { return 60; }
int mtl_link_active(void) { return 0; }
int mtl_link_wait_tick(int vsyncs, uint64_t timeout_ns) { (void)vsyncs; (void)timeout_ns; return 0; }
int mtl_link_wait_ticks(int ticks, uint64_t timeout_ns) { (void)ticks; (void)timeout_ns; return 0; }
int mtl_link_rate(void) { return 60; }
int mtl_link_stalled(void) { return 0; }
int mtl_snap_on(void) { return 0; }
uint64_t mtl_snap_acquire_wait(void) { return 0; }
void mtl_xv_window_tex(uint64_t serial) { (void)serial; }
void mtl_set_depth_zero(int on) { g_depth_zero = on; }
void mtl_set_origin_lower_left(int on) { if (g_origin_ll != on) LOG("[vk] window origin → %s (presents so far %llu)", on ? "lower left" : "upper left", (unsigned long long)atomic_load(&g_presents)); g_origin_ll = on; }   // as nvn_mtl.m logs it
void mtl_set_queue_origin_lower_left(int on) { g_queue_ll = on != 0; }
void mtl_reg_image(int id, const tex_t *t, const view_t *v) {   // RegisterImage declares image use even when the builder left IMAGE out (as nvn_mtl.m)
    if (!t) { mtl_reg_tex(id, NULL, v); return; }
    tex_t image = *t; image.d.flags |= NVN_TEXTURE_FLAGS_IMAGE; mtl_reg_tex(id, &image, v);
}
unsigned mtl_cpu_tex_count(void) { return 0; }   // Metal's CPU-written texture set: the replay has none
int mtl_link_tick(uint64_t n, double *target, double *lead_ms, int *ticks, double *gap_ms) { (void)n; (void)target; (void)lead_ms; (void)ticks; (void)gap_ms; return 0; }   // no display link
void mtl_set_frame(uint64_t f) { atomic_store(&g_cur_frame, f); }   // the frame later chunks count toward (g_fgpu)
int mtl_pso_prebuild_tool(void) { return 0; }

// ---------------- Statistics the interface layer reads ----------------
void mtl_pkg_stats(uint64_t *uploads, uint64_t *reuploads) { *uploads = g_pkg_uploads; *reuploads = g_pkg_reuploads; }
uint64_t mtl_pkg_edge_skips(void) { return g_pkg_edge_skips; }
uint64_t mtl_pkg_copy_absorbs(void) { return 0; }   // Metal's packaged-texture copies taken as written (nvn_mtl.m pkg_copy_absorb): none here
uint64_t mtl_pkg_miss(void) { return 0; }
uint64_t mtl_upload_bytes(void) { return g_up_bytes; }
void mtl_vbuf_stats(uint64_t *a, uint64_t *b, uint64_t *c, uint64_t *d, uint64_t *e, uint64_t *f) { *a = *b = *c = *d = *e = *f = 0; }
void mtl_stats(uint64_t *passes, uint64_t *draws) { *passes = g_passes; *draws = g_draws; }
void mtl_args_stats(uint64_t *a, uint64_t *b) { *a = *b = 0; }
void mtl_use_stats(uint64_t *a, uint64_t *b) { *a = *b = 0; }
uint64_t mtl_ds_omit(void) { return 0; }
uint64_t mtl_drawable_ns_take(void) { return 0; }
void mtl_bw(uint64_t *a, uint64_t *b, uint64_t *c) { *a = *b = *c = 0; }
uint64_t mtl_udc_saved_bytes(void) { return 0; }
double mtl_gpu_ms(uint64_t f) {   // frame f's GPU busy time once all its chunks have completed; -1 until then, for a frame with no recorded chunk, or with DM_VK_FRAME_GPU=0
    unsigned k = f % FGPU;
    if (g_fgpu_on <= 0 || atomic_load(&g_fgpu[k].frame) != f + 1 || atomic_load(&g_fgpu[k].pending)) return -1;
    double ms = (double)atomic_load(&g_fgpu[k].ns) / 1e6;
    return atomic_load(&g_fgpu[k].frame) == f + 1 ? ms : -1;   // not taken by a later frame meanwhile
}
void mtl_ab_gpu(uint64_t f, double g, double fr) { (void)f; (void)g; (void)fr; }
int mtl_gpu_span(uint64_t f, uint64_t *t0, uint64_t *t1) { (void)f; (void)t0; (void)t1; return 0; }
void mtl_ub_present(double iv) { (void)iv; }
uint64_t mtl_upload_ns_take(void) { return atomic_exchange(&g_up_ns, 0); }
uint64_t mtl_upload_gpu_ns_take(void) { return 0; }
// the long-frame line's "new pipelines + shader libraries": graphics and compute pipelines, and shader modules (Vulkan's counterpart of Metal's libraries)
void mtl_create_stats(uint64_t *a, uint64_t *b) { *a = atomic_load(&g_mod_made); *b = atomic_load(&g_pso_made) + atomic_load(&g_cpso_made); }
void mtl_create_ns(uint64_t *a, uint64_t *b) { *a = atomic_load(&g_mod_ns); *b = atomic_load(&g_pso_ns) + atomic_load(&g_cpso_ns); }
void mtl_hybrid_stats(uint64_t *a, uint64_t *b, uint64_t *c) { *a = *b = *c = 0; }
void mtl_alloc_stats(uint64_t *a, uint64_t *b, int *c) { *a = *b = 0; *c = 0; }
void mtl_barrier_stats(uint64_t *a, uint64_t *b) { *a = *b = 0; }
uint64_t mtl_blit_splits(void) { return 0; }
uint64_t mtl_copy_cut_count(void) { return 0; }
uint64_t mtl_cut_count(void) { return g_flushes; }
void mtl_cshoist_stats(uint64_t *a, uint64_t *b, uint64_t *c) { *a = *b = *c = 0; }
void mtl_lc_stats(uint64_t *a, uint64_t *b) { *a = *b = 0; }
void mtl_sm_stats(uint64_t *a, uint64_t *b, uint64_t *c) { *a = *b = *c = 0; }
void mtl_ubd_stats(uint64_t *a, uint64_t *b) { *a = *b = 0; }
void mtl_ubp_stats(uint64_t *a, uint64_t *b, uint64_t *c) { *a = *b = *c = 0; }
void mtl_ubp_why(uint64_t w[5]) { memset(w, 0, 5 * sizeof *w); }
void mtl_mem_stat(unsigned long long frame, unsigned long long mb) { LOG("[vk] frame %llu: resident %llu MB, textures %u, uploads %llu (%llu again), %llu MB uploaded; linear render targets: snapshots %llu "
    "(%llu not taken: a write was pending), write-backs whole %llu / changed bytes only %llu (%llu KB) / nothing changed %llu (%.1f ms landing and noting them), own write-backs absorbed %llu (noted %llu, %llu of them compared in their edge rows alone, guest differed %llu, refused %llu) (cumulative)",
    frame, mb, g_tex.n, (unsigned long long)g_pkg_uploads, (unsigned long long)g_pkg_reuploads, (unsigned long long)(g_up_bytes >> 20), (unsigned long long)g_lsnap, (unsigned long long)g_lsnap_stale,
    (unsigned long long)atomic_load(&g_lwb_whole), (unsigned long long)atomic_load(&g_lwb_diff), (unsigned long long)(atomic_load(&g_lwb_bytes) >> 10), (unsigned long long)atomic_load(&g_lwb_same),
    atomic_load(&g_lwb_ns) / 1e6, (unsigned long long)atomic_load(&g_ab_taken), (unsigned long long)atomic_load(&g_ab_noted), (unsigned long long)atomic_load(&g_ab_clean), (unsigned long long)atomic_load(&g_ab_differ), (unsigned long long)atomic_load(&g_ab_refused));
    ev_stat(); spare_stat(); fb_stat();
    if (wb_revert_on()) rv_stat(frame);
    if (DM_GAME_RENDER_COND) LOG("[vk] conditional rendering: %llu clears and draws dropped, samples-passed conditions %llu read once landed / %llu in flight (drawn), %u counter report addresses kept "
                                 "(cumulative)", (unsigned long long)g_rc_skipped, (unsigned long long)g_rc_landed, (unsigned long long)g_rc_flying, g_rc_n);
    {   static unsigned long long f0; static uint64_t n0, b0, r0, t0;   // since the last line, a frame on average
        uint64_t n = atomic_load(&g_awb_n), b = atomic_load(&g_awb_bytes), r = atomic_load(&g_awb_rb), t = atomic_load(&g_wb_ns); double fr = frame > f0 ? (double)(frame - f0) : 1;
        LOG("[vk] arena write-backs (storage buffers, buffer images, transform feedback), frames %llu to %llu: %.1f a frame, %.0f KB a frame, %.0f%% read through the GPU's cached copy (%s); landing every write-back took the completion thread %.2f ms a frame",
            f0, frame, (double)(n - n0) / fr, (double)(b - b0) / 1024 / fr, n > n0 ? 100.0 * (double)(r - r0) / (double)(n - n0) : 0.0, rb_state(), (double)(t - t0) / 1e6 / fr);
        f0 = frame; n0 = n; b0 = b; r0 = r; t0 = t; }
    LOG("[vk] guest reads of draws: waited for the current submission %llu, for submissions in flight %llu, no wait %llu (%llu of them after a barrier that made a draw's write readable on the GPU: wb_promote); uniform updates and small copies: %llu / %llu / %llu; submissions %llu (cumulative)",
        (unsigned long long)g_view_cur, (unsigned long long)g_view_fly, (unsigned long long)g_view_free, (unsigned long long)g_wb_promote, (unsigned long long)g_mem_cur, (unsigned long long)g_mem_fly, (unsigned long long)g_mem_free,
        (unsigned long long)g_flushes);
    LOG("[vk] constant and storage buffer copies: %llu reused, %llu made, %llu MB copied to the arena and %llu MB to the shadows (cumulative; gfx_vk_draw.inc ucopy); constant buffer bindings of the shaders loaded: %llu bounded by res.json, %llu by the SPIR-V scan, %llu read whole, %llu res.json extents below the scan's (gfx_vk_draw.inc cb_ext_pick)%s",
        (unsigned long long)g_ucopy_hit, (unsigned long long)g_ucopy_miss, (unsigned long long)(g_ucopy_bytes >> 20), (unsigned long long)(g_ucopy_shadow >> 20), (unsigned long long)g_cbx[0], (unsigned long long)g_cbx[2], (unsigned long long)g_cbx[3], (unsigned long long)g_cbx[1],
        ext_check() ? "; DM_VK_EXT_CHECK on" : "");
    if (ext_check()) LOG("[vk] DM_VK_EXT_CHECK: %llu constant buffer bindings copied whole with NaN past their extents (cumulative)", (unsigned long long)g_ext_checked);
    sbc_stat();
    LOG("[vk] support buffer: %llu stages wrote it, %llu left it unwritten (no module reads binding 0) (cumulative; gfx_vk_draw.inc stage_set)", (unsigned long long)g_sup_written, (unsigned long long)g_sup_skipped);
    gm_stat();   // raw global memory (gfx_vk_gmem.inc), with DM_VK_RAW_GMEM=1
    if (g_rq) LOG("[vk] recorder: %llu calls queued (%llu of them descriptor set writes, %llu more written directly after a drain), %llu drains, %llu made directly; the replay waited %.1f ms for it (cumulative; gfx_vk_rec.inc)",
                  (unsigned long long)g_rq_records, (unsigned long long)g_rq_desc, (unsigned long long)g_rq_desc_direct, (unsigned long long)g_rq_drains, (unsigned long long)g_rq_direct, atomic_load(&g_rq_waits_ns) / 1e6);
    if (set_reuse()) { static unsigned long long f0; static uint64_t h0, m0;   // per frame since the last line
        if (frame > f0) { double nf = (double)(frame - f0), h = (double)(g_sr_hit - h0) / nf, m = (double)(g_sr_miss - m0) / nf;
            LOG("[vk] descriptor sets a frame over the last %llu frames: %.0f bound again with the same descriptors, %.0f written (%.1f%% bound again); %llu too large to keep "
                "(cumulative; DM_VK_SET_REUSE, gfx_vk_draw.inc sr_find)", frame - f0, h, m, h + m > 0 ? 100.0 * h / (h + m) : 0.0, (unsigned long long)g_sr_long); }
        f0 = frame; h0 = g_sr_hit; m0 = g_sr_miss; }
    if (g_ds_stats) { static unsigned long long f0; static uint64_t h0, r0, m0, k0; uint64_t h = g_ds_got, r = g_ds_reused, m = g_ds_made, k = g_ds_calls;   // DM_VK_SET_STATS=1
        if (frame > f0) { double nf = (double)(frame - f0); char sets[16 * NCHUNK_MAX], pools[16 * NCHUNK_MAX]; size_t ns = 0, np = 0; sets[0] = pools[0] = 0;
            for (int i = 0; i < g_nchunk; i++) { ns += (size_t)snprintf(sets + ns, sizeof sets - ns, i ? " %d" : "%d", g_ch[i].nds); np += (size_t)snprintf(pools + np, sizeof pools - np, i ? " %d" : "%d", g_ch[i].ndp); }
            LOG("[vk] descriptor sets a frame over the last %llu frames: %.0f handed out, %.0f of them recycled; %.0f made in %.0f vkAllocateDescriptorSets calls; held by the chunks %s in %s pools, "
                "%llu full pool resets, %llu of %llu stage_set calls did not write each descriptor of their layout, %llu draws, captures or dispatches taken again after a sync inside "
                "stage_set closed their sets' chunk and %llu / %llu / %llu dropped when it closed again (cumulative; DM_VK_SET_RECYCLE=%d, gfx_vk_draw.inc ds_get, ds_moved)", frame - f0,
                (double)(h - h0) / nf, (double)(r - r0) / nf, (double)(m - m0) / nf, (double)(k - k0) / nf, sets, pools,
                (unsigned long long)atomic_load(&g_ds_resets), (unsigned long long)g_ds_short, (unsigned long long)g_ds_checked, (unsigned long long)g_ds_again,
                (unsigned long long)g_ds_lost[0], (unsigned long long)g_ds_lost[1], (unsigned long long)g_ds_lost[2], g_ds_on > 0); }
        f0 = frame; h0 = h; r0 = r; m0 = m; k0 = k; }
    if (c2_check()) LOG("[vk] c2 check: %llu handle words compared with c2_word, %llu differed (cumulative; gfx_vk_draw.inc bank_word)", (unsigned long long)g_c2_checked, (unsigned long long)g_c2_bad);
    if (memo_log()) LOG("[vk] memo programs: %llu draws found their shaders, %llu resolved and kept, %llu with a program of serial 0 resolved; program changes %llu (cumulative; gfx_vk_draw.inc prog_memo, DM_VK_PROG_MEMO %d)",
        (unsigned long long)g_pmemo_hit, (unsigned long long)g_pmemo_miss, (unsigned long long)g_pmemo_ser0, (unsigned long long)((atomic_load(&g_prog_gen) - 1) / 2), prog_memo_on());   // two moves per Finalize or SetShaders
    if (memo_log()) LOG("[vk] memo pipelines: %llu draws had the draw before's, %llu built their key; state changes %llu; checked %llu, differed %llu (cumulative; gfx_vk_draw.inc pso_memo, DM_VK_PSO_MEMO %d, DM_VK_PSO_CHECK %d)",
        (unsigned long long)g_pso_hit, (unsigned long long)g_pso_miss, (unsigned long long)(g_pso_gen - 1), (unsigned long long)g_pso_checked, (unsigned long long)g_pso_bad, pso_memo_on(), pso_check());
    if (memo_log()) LOG("[vk] memo binds left out / recorded: pipeline %llu / %llu, dynamic state %llu / %llu, vertex buffers %llu / %llu, index buffer %llu / %llu (cumulative; gfx_vk_draw.inc bf_*, DM_VK_BIND_FILTER %d)",
        (unsigned long long)g_bf_skip[0], (unsigned long long)g_bf_made[0], (unsigned long long)g_bf_skip[1], (unsigned long long)g_bf_made[1], (unsigned long long)g_bf_skip[2], (unsigned long long)g_bf_made[2],
        (unsigned long long)g_bf_skip[3], (unsigned long long)g_bf_made[3], bind_filter());
    if (memo_log()) LOG("[vk] memo forwarding: %llu vertex streams answered with no forwardable write-back pending, %llu looked up (cumulative; gfx_vk_draw.inc stream_fwd, DM_VK_FWD_EARLY %d)",
        (unsigned long long)g_fwd_early, (unsigned long long)g_fwd_look, fwd_early());
    if (memo_log()) LOG("[vk] memo textures: %llu elements taken as stage_textures resolved them, %llu resolved again; samplers %llu found per ID, %llu made or looked up; image generation %llu (cumulative; gfx_vk_draw.inc tex_once, DM_VK_TEX_ONCE %d)",
        (unsigned long long)g_tc_hit, (unsigned long long)g_tc_again, (unsigned long long)g_sc_hit, (unsigned long long)g_sc_made, (unsigned long long)g_vt_gen, tex_once());
    LOG("[vk] geometry kept on the GPU: %llu index / vertex bindings and %llu read-only storage buffer bindings from kept copies, %llu copies kept (%llu MB now), %llu copied per draw; "
        "%llu walks for room let %llu copies go, %llu requests found no walk due (cumulative; gfx_vk_draw.inc gc_get, sb_keep, gc_evict)",
        (unsigned long long)(g_gc_hit - g_gc_sb), (unsigned long long)g_gc_sb, (unsigned long long)g_gc_made, (unsigned long long)(g_gc_bytes >> 20), (unsigned long long)g_gc_copy,
        (unsigned long long)g_gc_evicts, (unsigned long long)g_gc_evicted, (unsigned long long)g_gc_evict_skips);
    { static unsigned long long f0; static uint64_t g0, w0; uint64_t g = atomic_load(&g_gpu_ns), w = atomic_load(&g_chunk_wait_ns);
      if (frame > f0) LOG("[vk] GPU busy %.2f ms per frame over the last %llu frames (%llu command buffers in all); the replay waited %.2f ms per frame for a free one of %d", (double)(g - g0) / 1e6 / (double)(frame - f0), frame - f0,
                          (unsigned long long)atomic_load(&g_gpu_chunks), (double)(w - w0) / 1e6 / (double)(frame - f0), g_nchunk);
      f0 = frame; g0 = g; w0 = w; }
    LOG("[vk] occlusion queries %llu (%s; %llu stretches counted with none), samples-passed reports %llu (%llu to an address a condition had read, %llu to one none had), counter resets %llu, timestamp reports %llu, queries reset on the host %llu / in command buffers %llu, graphics pipelines made %llu (%.0f ms), compute pipelines %llu (%.0f ms), shader modules %llu (%.0f ms); pipelines the driver's cache gave for module identifiers %llu, that needed the modules after all %llu, modules known by identifier %llu; shaders with binding numbers made dense %llu (%llu bindings; spv_bind.h) (cumulative)",
        (unsigned long long)g_q_queries, occl_mode() == 2 ? "precise" : occl_mode() ? "any count" : "none", (unsigned long long)g_q_noq, (unsigned long long)g_q_reports, (unsigned long long)g_q_rep_read, (unsigned long long)g_q_rep_new, (unsigned long long)g_q_resets,
        (unsigned long long)g_ts_reports, (unsigned long long)atomic_load(&g_qr_host), (unsigned long long)g_qr_cmd,
        (unsigned long long)atomic_load(&g_pso_made), atomic_load(&g_pso_ns) / 1e6, (unsigned long long)atomic_load(&g_cpso_made), atomic_load(&g_cpso_ns) / 1e6, (unsigned long long)atomic_load(&g_mod_made), atomic_load(&g_mod_ns) / 1e6,
        (unsigned long long)atomic_load(&g_lm_hit), (unsigned long long)atomic_load(&g_lm_miss), (unsigned long long)atomic_load(&g_lm_ids), (unsigned long long)g_bind_shaders, (unsigned long long)g_bind_entries);
    fl_stats();
    LOG("[vk] indirect dispatches: arguments read on the GPU %llu (vkCmdDispatchIndirect on the pending write, %llu of them in place in a placed pool; DM_VK_IND_FWD), on the CPU %llu, of them %llu after a wait for the GPU (cumulative)", (unsigned long long)g_ind_gpu, (unsigned long long)g_ind_inpl, (unsigned long long)g_ind_cpu, (unsigned long long)g_ind_cpu_wait);
    LOG("[vk] draws not made: shader missing %llu, targets %llu, pipeline %llu, descriptors %llu, invalid binding %llu, pass %llu, vertex buffers %llu; dropped in all %llu, task / mesh draws skipped %llu; shaders whose descriptor set fits no pool %llu; under rasterizer discard: made as dispatches %llu, left out (writing nothing) %llu, their storage buffer writes in place on a home copy %llu / through a scratch copy %llu, home copies made %llu (%llu from a chunk in flight on the GPU), home ranges learned %llu (cumulative)", (unsigned long long)g_drop[0], (unsigned long long)g_drop[1], (unsigned long long)g_drop[2], (unsigned long long)g_drop[3], (unsigned long long)g_drop[4], (unsigned long long)g_drop[5], (unsigned long long)g_drop[6], (unsigned long long)g_draws_dropped, (unsigned long long)g_mesh_skipped, (unsigned long long)g_ds_toobig, (unsigned long long)g_rd_made, (unsigned long long)g_rd_none, (unsigned long long)g_rd_w[0], (unsigned long long)g_rd_w[1], (unsigned long long)g_rd_w[2], (unsigned long long)g_rd_w[3], (unsigned long long)g_rd_learned);
    LOG("[vk] pipelines met for the first time: %llu built in the background of %llu queued (%llu failed), %llu draws left out meanwhile, %llu built at once for draws that re-draw what an earlier pass laid the depth down for (cumulative; DM_VK_ASYNC_PIPE, apipe_rule.h)",
        (unsigned long long)g_apq_built, (unsigned long long)g_apq_queued, (unsigned long long)g_apq_failed, (unsigned long long)g_apq_left, (unsigned long long)g_apq_redraw);
    if (memu_on()) LOG("[vk] task / mesh draws in compute form: %llu drawn in %llu parts, %llu skipped, %llu left out while a kernel's pipeline was built; kernel pipelines made %llu (%llu from the driver's cache at once, %llu in the background, %.0f ms of compiles) (cumulative; gfx_vk_meshemu.inc)",
                       (unsigned long long)g_me_draws, (unsigned long long)g_me_parts, (unsigned long long)g_me_skipped, (unsigned long long)g_me_left, (unsigned long long)g_me_kpipes, (unsigned long long)g_me_kcached, (unsigned long long)g_me_kbg, atomic_load(&g_me_kpipe_ns) / 1e6);
    if (g_mesh_leftout) LOG("[vk] task / mesh draws left out: %llu (cumulative; DM_VK_MESH_LEAVE_OUT, gfx_vk_mesh.inc)", (unsigned long long)g_mesh_leftout);
    pool_stats();
    shadow_line();
    LOG("[vk] read on the GPU from a dispatch's pending write: vertex streams %llu (%llu looked up as far as the write-back's end), other bindings %llu, written in place by a later dispatch %llu; bound on another binding's copy of the same shader %llu; draws' in-place writes read by a dispatch %llu (cumulative; gfx_vk_draw.inc stream_fwd, buf_fwd, own_alias, fwd_hit)", (unsigned long long)g_fwd, (unsigned long long)g_stream_clip, (unsigned long long)g_buf_fwd[0], (unsigned long long)g_buf_fwd[1], (unsigned long long)g_alias, (unsigned long long)g_ip_read);
    LOG("[vk] render passes with every layer of their targets attached (a draw writes the layer): %llu (cumulative; gfx_vk_draw.inc rp_prepare_)", (unsigned long long)g_layered_passes);
    LOG("[vk] compute dispatches: %llu with no barrier before them, %llu after a barrier of the game's, %llu after a command of another kind (a full barrier), %llu runs closed by a later command (cumulative; %s)",
        (unsigned long long)g_cs_free, (unsigned long long)g_cs_gbarred, (unsigned long long)g_cs_first, (unsigned long long)g_cs_closed, cs_game() ? "ordered by the game's barriers, DM_VK_CS_BARRIER" : "DM_VK_CS_BARRIER=full");
    if (slot0_fill_on()) LOG("[vk] render passes given a scratch target in their empty colour slot 0: %llu, made with no colour slot as every one was empty: %llu (cumulative; gfx_vk_draw.inc slot0_fill_on)", (unsigned long long)g_slot0_fills, (unsigned long long)g_slot0_none);
    if (layer_pad_on()) LOG("[vk] layered render passes padded past 1 MB of texels with scratch colour slots: %llu (cumulative; gfx_vk_layerpad.inc layer_pad_on)", (unsigned long long)g_layer_pads);
    if (dup_slots_on()) LOG("[vk] render passes attaching an image once that two colour slots name: %llu, draws whose write to the slot left unused was lost: %llu (cumulative; gfx_vk_draw.inc dup_slots)", (unsigned long long)g_dup_passes, (unsigned long long)g_dup_lost);
    if (stg_stat()) { static unsigned long long f0; static uint64_t a0, r0, s0; uint64_t a = atomic_load(&g_stg_allocs), r = atomic_load(&g_stg_frees), sl = g_sp_slices; double nf = frame > f0 ? (double)(frame - f0) : 1;
      pthread_mutex_lock(&g_sp_mu); int nb = g_sp_nblk, fr = g_sp_nfree; pthread_mutex_unlock(&g_sp_mu);
      LOG("[vk] staging over the last %llu frames: vkAllocateMemory %.2f and vkFreeMemory %.2f a frame, %.1f slices a frame cut from the pool%s; pool blocks %d of %d now (%d free, %u MB each), %llu made, %llu trimmed; requests given a buffer of their own: over %u MB %llu, pool at its cap %llu (cumulative)",
          frame - f0, (double)(a - a0) / nf, (double)(r - r0) / nf, (double)(sl - s0) / nf, g_sp_on > 0 ? "" : " (DM_VK_STG_POOL=0)", nb, SP_NBLK, fr, SP_BLOCK >> 20, (unsigned long long)g_sp_made, (unsigned long long)atomic_load(&g_sp_trimmed),
          SP_MAXREQ >> 20, (unsigned long long)g_sp_big, (unsigned long long)g_sp_full);
      f0 = frame; a0 = a; r0 = r; s0 = sl; }
    LOG("[vk] syncs by site (submitted / in flight): other %llu/%llu, draw %llu/%llu, upload %llu/%llu (%llu waited for nothing), t2b %llu/%llu, large copy %llu/%llu, buffer texture %llu/%llu, buffer image %llu/%llu, dispatch %llu/%llu",
        (unsigned long long)g_vsite[0][0], (unsigned long long)g_vsite[0][1], (unsigned long long)g_vsite[1][0], (unsigned long long)g_vsite[1][1], (unsigned long long)g_vsite[2][0], (unsigned long long)g_vsite[2][1], (unsigned long long)g_up_free,
        (unsigned long long)g_vsite[3][0], (unsigned long long)g_vsite[3][1], (unsigned long long)g_vsite[4][0], (unsigned long long)g_vsite[4][1], (unsigned long long)g_vsite[5][0], (unsigned long long)g_vsite[5][1],
        (unsigned long long)g_vsite[6][0], (unsigned long long)g_vsite[6][1], (unsigned long long)g_vsite[7][0], (unsigned long long)g_vsite[7][1]); }
void mtl_pso_line(char *out, size_t n) { if (n) out[0] = 0; }
void mtl_res_stats(char *out, size_t n) { if (n) snprintf(out, n, "textures %u", g_tex.n); }
uint32_t mtl_present_qos(void) { return 0; }
void dm_gfx_passdump_next(long n) { (void)n; }
void dm_gfx_texdump_next(const char *path) { (void)path; }
void dm_gfx_texlist(int target) { (void)target; }
void dm_gfx_texraw(uint64_t serial, const char *path) { (void)serial; (void)path; }

// ---------------- The NVN queue scheduler's hooks (hle_nvn.c nvn_queue.inc) ----------------
// Submissions are copied when the game submits them and replayed in order on the scheduler's thread, "NVN queue replay", a continuation at a time (a submission is cut at each
// event signal and wait). The copies hold snapshots of the programs and textures their records name (nvn_capture.c cap_rec_fields), freed once the job is replayed.
uint64_t mtl_completed_fence(void) { return atomic_load(&g_done); }
// Called by the game's thread as it submits (nvn_queue_submit) and by the replay thread before each continuation: both join this frame's performance hint session
// (hint_note), as the game's render thread did when it replayed its own submissions. The prewarm starts at the 30th present (mtl_present)
void mtl_prepare(void) { hint_note(); }
// One replay state for every NVN queue, as before the scheduler (nvn_mtl.m keeps one per queue): a game's second queue (Fire Emblem: Three Houses) sees the first's bindings
void mtl_queue_activate(uint64_t serial) { static uint64_t last; if (last && serial != last) RED_ONCE(130, "a second NVN queue submits: the Vulkan replay shares one state between queues"); last = serial; }
void mtl_queue_forget(uint64_t serial) { (void)serial; }
// A program snapshot needs nothing kept alive here: shaders are found by the program's serial (hle_nvn.c prog_key, prog_c1) and stay in the shader cache
void mtl_prog_snapshot(prog_t *copy, const prog_t *source) { *copy = *source; }
void mtl_prog_unpin(prog_t *p) { (void)p; }
// The bound programs are snapshots (a job's or bound_preserve's copies), never the game's object, so a finalized program cannot be left bound here
void mtl_prog_forget(const prog_t *p) { (void)p; }
void mtl_prog_prefetch(const prog_t *p) { (void)p; }   // SPIR-V modules are made at first use and kept in the pipeline cache

// Texture pins: a queued job, a queued present or the bound render targets name a texture by serial, and the game may finalize it before the queue gets there. Its image stays
// in g_tex until the last pin goes, as nvn_mtl.m keeps the Metal texture. g_pin_mu is a leaf, so the game's threads pin without waiting for a replay holding g_bk
typedef struct { int refs, released; } tex_pin;
static pthread_mutex_t g_pin_mu = PTHREAD_MUTEX_INITIALIZER; static nvn_hmap g_pins = {.klen = 8};
void mtl_tex_pin(uint64_t serial) {
    if (!serial) return;
    pthread_mutex_lock(&g_pin_mu);
    tex_pin *p = nvn_hm_get(&g_pins, &serial);
    if (!p) { p = calloc(1, sizeof *p); if (!p) die("texture pin allocation failed"); nvn_hm_put(&g_pins, &serial, p); }
    p->refs++;
    pthread_mutex_unlock(&g_pin_mu);
}
static int tex_unpin_(uint64_t serial) {   // 1: the game finalized it meanwhile and this was its last pin, so the caller drops it (tex_drop, under g_bk)
    pthread_mutex_lock(&g_pin_mu);
    tex_pin *p = nvn_hm_get(&g_pins, &serial); int drop = 0;
    if (p && !--p->refs) { drop = p->released; nvn_hm_del(&g_pins, &serial); free(p); }
    pthread_mutex_unlock(&g_pin_mu);
    return drop;
}
void mtl_tex_unpin(uint64_t serial) { if (serial && tex_unpin_(serial)) { pthread_mutex_lock(&g_bk); tex_drop(serial); pthread_mutex_unlock(&g_bk); } }
static int tex_pinned_release(uint64_t serial) {
    pthread_mutex_lock(&g_pin_mu); tex_pin *p = nvn_hm_get(&g_pins, &serial); if (p) p->released = 1; pthread_mutex_unlock(&g_pin_mu);
    return p != NULL;
}
// NVN's bound state carries over to the next submission, but a job's snapshots go once it is replayed: at each commit the bound programs and render targets are copied into
// the replay's own, as nvn_mtl.m queue_program_preserve and rt_preserve_bound do, and the targets' serials stay pinned while bound. Read every source before writing: a stage
// may already point into the copies. g_bk held
static prog_t g_own_prog[6]; static tex_t g_own_rt[9]; static uint64_t g_rt_pin[9];
static void bound_preserve(void) {
    const prog_t *b[6] = {D.bst[0], D.bst[1], D.bst[2], D.bst[3], D.bst[4], D.cprog};
    int need = 0; for (int i = 0; i < 6; i++) need |= b[i] && (b[i] < g_own_prog || b[i] >= g_own_prog + 6);
    if (need) {
        prog_t next[6]; const prog_t *from[6]; int idx[6], n = 0;
        for (int i = 0; i < 6; i++) {
            idx[i] = -1; if (!b[i]) continue;
            int k = 0; while (k < n && from[k] != b[i]) k++;
            if (k == n) { from[n] = b[i]; next[n++] = *b[i]; }
            idx[i] = k;
        }
        memcpy(g_own_prog, next, (size_t)n * sizeof *next);
        for (int i = 0; i < 5; i++) D.bst[i] = idx[i] < 0 ? NULL : &g_own_prog[idx[i]];
        D.cprog = idx[5] < 0 ? NULL : &g_own_prog[idx[5]];
    }
    if (!R.has_rt) return;
    const tex_t *src[9]; tex_t next[9]; unsigned pend = 0;
    for (int k = 0; k < 9; k++) {
        src[k] = k == 8 ? R.rt.depth : k < R.rt.n ? R.rt.color[k] : NULL;
        uint64_t ser = src[k] ? src[k]->serial : 0;
        if (ser != g_rt_pin[k]) { mtl_tex_pin(ser); if (g_rt_pin[k] && tex_unpin_(g_rt_pin[k])) tex_drop(g_rt_pin[k]); g_rt_pin[k] = ser; }
        if (src[k] && src[k] != &g_own_rt[k]) { next[k] = *src[k]; pend |= 1u << k; }
    }
    for (int k = 0; k < 9; k++) if (pend >> k & 1) { g_own_rt[k] = next[k]; if (k == 8) R.rt.depth = &g_own_rt[k]; else R.rt.color[k] = &g_own_rt[k]; }
}

// The rest of the interface Metal gives and Android does not need yet
void mtl_tex_gpu_written(const tex_t *t) { (void)t; }   // Metal's record of GPU-written storage (tex_cpu_content); tex_init_fill here decides by the pool's flags
static int gm_game(void);   // gfx_vk_gmem.inc: the game turned raw global memory on (DM_VK_RAW_GMEM=1)
void mtl_pool_addressed(const void *mem, uint64_t size) {   // raw global memory: which pools the game took a GPU address in (counted in gfx_vk_gmem.inc gm_stat); nothing for a game without it
    if (!gm_game()) return;
    pthread_mutex_lock(&g_pool_mu);
    int i = pool_find((const uint8_t *)mem, size ? size : 1);
    int whole = i >= 0 && (const uint8_t *)mem == g_pools[i].lo && (const uint8_t *)mem + size >= g_pools[i].hi, first = whole && !(g_pools[i].how & 1) && !gm_code_skip(g_pools[i].flags);
    if (i >= 0) g_pools[i].how |= whole ? 1 : 2;
    pthread_mutex_unlock(&g_pool_mu);
    if (first) gm_place(mem, size, 1);   // a buffer's address: raw pointers may reach the pool from now on, so its pages become placed Vulkan memory before the game has it
}
void mtl_sp_phys_gone(const void *mem, uint64_t size) { (void)mem; (void)size; }   // sparse pools' physical pages: no sparse pools on Vulkan
int mtl_addr_known(uint64_t addr, uint64_t len) { return mtl_addr(addr, len, NULL, NULL); }
int mtl_probe_ramp_on(void) { return 0; }   // macOS's GPU clock ramp: the frame step's probes have their iPhone grace here
void mtl_present_layer(uint32_t w, uint32_t h) { (void)w; (void)h; }   // the picture's shape (iOS / Mac layout); the front end letterboxes the crop it is given
void mtl_present_screen(uint32_t w, uint32_t h) { (void)w; (void)h; }
void mtl_present_progress(uint64_t *queued, uint64_t *done) { *queued = *done = atomic_load(&g_presents); }   // presents are shown when made
void mtl_fb_stats(uint64_t *snaps, uint64_t *reused, uint64_t *tile, uint64_t *wait) { *snaps = *reused = *tile = *wait = 0; }
void mtl_pool_stats(uint64_t s[6]) { memset(s, 0, 6 * sizeof *s); }
void mtl_geohoist_stats(uint64_t *moved, uint64_t *runs, uint64_t *stops) { *moved = *runs = 0; memset(stops, 0, 6 * sizeof *stops); }
void mtl_geomesh_stats(uint64_t *n, uint64_t *wait, uint64_t *run_draws, uint64_t *runs, uint64_t ends[4]) { *n = *wait = *run_draws = *runs = 0; memset(ends, 0, 4 * sizeof *ends); }
void dm_gfx_restop(void) {}   // DM_CTL restop: Metal's residency set
void *objc_autoreleasePoolPush(void) { return NULL; }   // nvn_queue.inc brackets its Metal work with autorelease pools
void objc_autoreleasePoolPop(void *pool) { (void)pool; }
