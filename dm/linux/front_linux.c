// Linux front end (SDL3): the game's window, input, vibration and log around the host's main() (main.c), dm/android/front_android.c's work on a Linux desktop or handheld.
//   dmhost_<title ID> [<game folder>]
//   Game folder (default <data>/games/<title ID>): code/ (the modules rewritten with DM_TARGET=linux), RomFS/ or RomFS.img, spv/ (the shaders translated for Vulkan),
//   optionally aoc/ and font/, launcher.txt and defaults.txt (KEY=VALUE per line, as on Android). <data> is $DM_DATA, else $XDG_DATA_HOME/nxnative/linux, else
//   ~/.local/share/nxnative/linux: env.txt (switches for every game), log.txt, saves/<title ID>, cache/<title ID> (the driver's pipeline cache), temp, dump.
//   Log: the host's stderr goes to the terminal and to <data>/log.txt (the previous run's to log.prev.txt).
//   Window: SDL3 (Wayland), fullscreen unless DM_WINDOWED=1 (F11 switches). Vulkan on the NVN replay's device (dm/android/gfx_vk.c): the window texture the game presented
//   last is drawn scaled to fit with its aspect kept (black bars where the screen's shape differs), as front_android.c draws it. Before the first frame the window is dark.
//   Present mode: FIFO by default, as on Android (the frame step paces to the display); DM_VK_PRESENT=mailbox or immediate for lower latency.
//   Input: SDL gamepads (buttons by position, Nintendo's layout; DM_PAD_ABXY=label maps by label), the keyboard (below), both player 1. Vibration on the gamepad's motors.
//   Escape or closing the window ends the game (the pipeline cache is saved first).
#include "dm.h"
#include "prev_file.h"
#include "front.h"
#include "dm_input.h"
#include "nvn_obj.h"
#include "gfx_vk.h"
#include "game_profile.h"   // DM_GAME_ID, DM_GAME_NAME
#include "hle_haptics.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <stdatomic.h>
#include <sys/stat.h>

void dm_focus_set(int st);
void dm_audio_suspend(int on);

enum { ST_START, ST_GAME, ST_ENDED, ST_CRASHED };
static _Atomic int g_state = ST_START;
static _Atomic uint64_t g_frames;
static char g_data[512], g_game[700];
static SDL_Window *g_win;
static Uint32 g_wake;   // the SDL event a present pushes, so the main loop shows the frame at once

// ---------- Log: stderr → the terminal and log.txt (front_android.c log_reader) ----------
static int g_tty = -1;
static void *log_reader(void *p) {
    int fd = (int)(intptr_t)p; char path[600]; snprintf(path, sizeof path, "%s/log.txt", g_data);
    FILE *f = fopen(path, "w"); char buf[4096]; size_t n = 0;
    for (;;) {
        ssize_t k = read(fd, buf + n, sizeof buf - 1 - n);
        if (k <= 0) break;
        if (g_tty >= 0) { ssize_t w = write(g_tty, buf + n, (size_t)k); (void)w; }
        n += (size_t)k; buf[n] = 0;
        char *s = buf, *e;
        while ((e = memchr(s, '\n', buf + n - s))) {
            *e = 0;
            if (f) { fputs(s, f); fputc('\n', f); fflush(f); }
            if (strstr(s, "CRASH") || strstr(s, "FATAL")) atomic_store(&g_state, ST_CRASHED);
            s = e + 1;
        }
        n -= (size_t)(s - buf); memmove(buf, s, n);
        if (n == sizeof buf - 1) n = 0;   // a line longer than the buffer: dropped
    }
    if (f) fclose(f);
    return NULL;
}
static void log_start(void) {
    int pfd[2]; if (pipe(pfd)) return;
    g_tty = dup(2); dup2(pfd[1], 2); close(pfd[1]); setvbuf(stderr, NULL, _IOLBF, 0);
    pthread_t th; pthread_create(&th, NULL, log_reader, (void *)(intptr_t)pfd[0]); pthread_detach(th);
}

// ---------- Files (front_android.c's) ----------
static int exists(const char *p) { struct stat st; return !stat(p, &st); }
static void mkdirs(const char *p) { char b[700]; snprintf(b, sizeof b, "%s", p); for (char *s = b + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(b, 0755); *s = '/'; } mkdir(b, 0755); }
static void env_file(const char *dir, const char *name, int over) {
    char path[800]; snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "r"); if (!f) return;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0; char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq) continue;
        *eq = 0; if (!over && getenv(line)) continue;
        setenv(line, eq + 1, 1); LOG("%s: %s=%s", name, line, eq + 1);
    }
    fclose(f);
}
static void data_dir(void) {
    const char *d = getenv("DM_DATA"), *x = getenv("XDG_DATA_HOME"), *h = getenv("HOME");
    if (d && *d) snprintf(g_data, sizeof g_data, "%s", d);
    else if (x && *x) snprintf(g_data, sizeof g_data, "%s/nxnative/linux", x);
    else snprintf(g_data, sizeof g_data, "%s/.local/share/nxnative/linux", h && *h ? h : ".");
    mkdirs(g_data);
}

// ---------- The guest ----------
static int g_argc; static char *g_argv[4];
int dm_host_main(int argc, char **argv);   // main.c's main, built under this name for the windowed host (CMakeLists.txt): it runs on the guest thread, as android_main runs it
static void *guest_thread(void *p) {
    (void)p;
    int rc = dm_host_main(g_argc, g_argv);
    LOG("host main returned %d", rc);
    if (atomic_load(&g_state) == ST_GAME) atomic_store(&g_state, ST_ENDED);
    SDL_Event ev = {.type = SDL_EVENT_QUIT}; SDL_PushEvent(&ev);
    return NULL;
}
void front_run(void (*guest)(void)) { guest(); }
static int start_host(void) {
    char code[800], buf[800];
    snprintf(code, sizeof code, "%s/code", g_game);
    snprintf(buf, sizeof buf, "%s/main.fixups.tsv", code);
    if (!exists(buf)) { LOG("[RED] no game in %s: pack it with scripts/build_android_from_nsp.py --linux, or give the game folder as the argument", g_game); return 0; }
    snprintf(buf, sizeof buf, "%s/saves", g_data); mkdirs(buf);
    snprintf(buf, sizeof buf, "%s/saves/%s", g_data, DM_GAME_ID); setenv("DM_SAVE", buf, 0);
    snprintf(buf, sizeof buf, "%s/temp", g_data); mkdirs(buf); setenv("DM_TEMP", buf, 0);
    snprintf(buf, sizeof buf, "%s/cache/%s", g_data, DM_GAME_ID); mkdirs(buf);
    snprintf(buf, sizeof buf, "%s/cache/%s/pipeline.cache", g_data, DM_GAME_ID); setenv("DM_PIPELINE_CACHE", buf, 0);
    setenv("DM_AUDIO", "1", 0);
    setenv("DM_OPMODE", "0", 0);   // handheld mode (720p), as on Android; DM_OPMODE=1 in env.txt for docked
    snprintf(buf, sizeof buf, "%s/dump", g_data); setenv("DM_DUMP_DIR", buf, 0); mkdirs(buf);
    setenv("DM_ROOT", g_game, 0);
    snprintf(buf, sizeof buf, "%s/RomFS", g_game);
    if (!exists(buf)) snprintf(buf, sizeof buf, "%s/RomFS.img", g_game);
    if (exists(buf)) setenv("DM_ROMFS", buf, 0);
    snprintf(buf, sizeof buf, "%s/aoc", g_game); if (exists(buf)) setenv("DM_AOC_DIR", buf, 0);
    snprintf(buf, sizeof buf, "%s/spv", g_game); setenv("DM_SPV_DIR", buf, 0);
    snprintf(buf, sizeof buf, "%s/glslc_capture", g_game); setenv("DM_GLSLC_CAPTURE", buf, 0);
    snprintf(buf, sizeof buf, "%s/font", g_game); if (exists(buf)) setenv("DM_FONT_DIR", buf, 0);
    snprintf(buf, sizeof buf, "%s/font/standard.ttf", g_game);
    if (!exists(buf)) {   // a hinted TrueType font for a game packed without standard.ttf (front_android.c and build_android_from_nsp.py say why)
        static const char *const sys[] = {"/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/dejavu/DejaVuSans.ttf"};
        for (unsigned i = 0; i < sizeof sys / sizeof *sys; i++) if (exists(sys[i])) { setenv("DM_SHARED_FONT", sys[i], 0); break; }
    }
    g_argv[0] = "dmhost"; g_argv[1] = strdup(code); g_argc = 2;
    atomic_store(&g_state, ST_GAME); LOG("game in %s", code);
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 8 << 20);
    pthread_t th; if (pthread_create(&th, &a, guest_thread, NULL)) { LOG("[RED] cannot start the guest thread"); return 0; }
    pthread_detach(th);
    return 1;
}

// ---------- Front end interface (front.h) ----------
void *front_metal_layer(void) { return NULL; }
int front_has_window(void) { return 1; }
int front_screen_max_fps(void) {
    const SDL_DisplayMode *m = g_win ? SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(g_win)) : NULL;
    return m && m->refresh_rate > 0 ? (int)(m->refresh_rate + 0.5f) : 0;
}
int front_ctrl_support(int min, int max) { (void)min; (void)max; return -1; }   // no controller prompt yet: the applet closes as the player would (front_android.c's answer)
void front_screen_match(int fps) { (void)fps; }
void front_progress(const char *title, long done, long total) { if (title) LOG("progress: %s %ld / %ld", title, done, total); }
// The software keyboard (hle_swkbd.c): no input box yet. A stopgap so that a game which asks for a name goes on: the text the game proposes is confirmed, else
// DM_SWKBD_TEXT, else "Player"
int front_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap) {
    (void)sub; (void)guide; (void)max_chars;
    if (out_cap <= 0) return -1;
    const char *e = getenv("DM_SWKBD_TEXT"), *t = initial && *initial ? initial : e && *e ? e : "Player";
    snprintf(out, (size_t)out_cap, "%s", t);
    LOG("software keyboard (%s): no input box on Linux yet, \"%s\" confirmed (DM_SWKBD_TEXT sets the text)", title ? title : "", out);
    return 1;
}
void front_android_present(const tex_t *t, const int crop[4]) {
    (void)t; (void)crop; atomic_fetch_add(&g_frames, 1);
    if (g_wake) { SDL_Event ev = {.type = g_wake}; SDL_PushEvent(&ev); }
}
uint32_t front_vk_instance_extensions(const char *const **names) {
    Uint32 n = 0; *names = g_win ? SDL_Vulkan_GetInstanceExtensions(&n) : NULL;
    return *names ? n : 0;
}

// ---------- Vulkan (front_android.c's swapchain and frame drawing, on an SDL window's surface) ----------
static VkInstance g_inst; static VkPhysicalDevice g_pd; static VkDevice g_dev; static VkQueue g_q; static uint32_t g_qf;
static VkSurfaceKHR g_surf; static VkSwapchainKHR g_sc; static VkImage g_img[8]; static uint32_t g_nimg; static VkFormat g_fmt; static VkExtent2D g_ext2;
static int g_sc_srgb, g_sc_has_srgb;
static VkCommandPool g_cpool; static VkCommandBuffer g_cb; static VkFence g_fence; static VkSemaphore g_acq, g_done;
static int vk_bad(const char *call, VkResult r) { LOG("[RED] Vulkan: %s = %s", call, vk_result_str(r)); return 0; }
#define VKC(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) return vk_bad(#x, r_); } while (0)
static int vk_init(void) {
    if (g_inst) return 1;
    vkb_ctx *c = vkb();
    if (!c || !c->has_surface || !c->has_swapchain) { LOG("[RED] Vulkan: no device with a window surface"); return 0; }
    g_inst = c->inst; g_pd = c->pd; g_dev = c->dev; g_q = c->q; g_qf = c->qf;
    LOG("vulkan: %s, API %u.%u.%u", c->props.deviceName, VK_API_VERSION_MAJOR(c->props.apiVersion), VK_API_VERSION_MINOR(c->props.apiVersion), VK_API_VERSION_PATCH(c->props.apiVersion));
    VkCommandPoolCreateInfo pc = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = g_qf};
    VKC(vkCreateCommandPool(g_dev, &pc, NULL, &g_cpool));
    VkCommandBufferAllocateInfo ca = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = g_cpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VKC(vkAllocateCommandBuffers(g_dev, &ca, &g_cb));
    VkFenceCreateInfo fc = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT}; VKC(vkCreateFence(g_dev, &fc, NULL, &g_fence));
    VkSemaphoreCreateInfo sc = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; VKC(vkCreateSemaphore(g_dev, &sc, NULL, &g_acq)); VKC(vkCreateSemaphore(g_dev, &sc, NULL, &g_done));
    return 1;
}
static void sc_down(void) {
    if (!g_dev) return;
    pthread_mutex_lock(&vkb()->qmu); vkDeviceWaitIdle(g_dev); pthread_mutex_unlock(&vkb()->qmu);
    if (g_sc) { vkDestroySwapchainKHR(g_dev, g_sc, NULL); g_sc = VK_NULL_HANDLE; }
}
static VkPresentModeKHR present_mode(void) {
    const char *e = getenv("DM_VK_PRESENT"); VkPresentModeKHR want = VK_PRESENT_MODE_FIFO_KHR;
    if (e && !strcmp(e, "mailbox")) want = VK_PRESENT_MODE_MAILBOX_KHR; else if (e && !strcmp(e, "immediate")) want = VK_PRESENT_MODE_IMMEDIATE_KHR;
    uint32_t n = 8; VkPresentModeKHR m[8];
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)(void *)SDL_Vulkan_GetVkGetInstanceProcAddr();   // SDL's, from the same libvulkan.so.1 as vk_driver.c's
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR f = gipa ? (PFN_vkGetPhysicalDeviceSurfacePresentModesKHR)gipa(g_inst, "vkGetPhysicalDeviceSurfacePresentModesKHR") : NULL;
    if (want != VK_PRESENT_MODE_FIFO_KHR && f && f(g_pd, g_surf, &n, m) >= 0) for (uint32_t i = 0; i < n; i++) if (m[i] == want) return want;
    if (want != VK_PRESENT_MODE_FIFO_KHR) LOG("[RED] DM_VK_PRESENT=%s: the surface does not offer it, FIFO instead", e);
    return VK_PRESENT_MODE_FIFO_KHR;
}
static int sc_up(void) {
    if (!vk_init()) return 0;
    if (!g_surf && !SDL_Vulkan_CreateSurface(g_win, g_inst, NULL, &g_surf)) { LOG("[RED] Vulkan: no window surface: %s", SDL_GetError()); return 0; }
    VkBool32 ok = 0; vkGetPhysicalDeviceSurfaceSupportKHR(g_pd, g_qf, g_surf, &ok);
    if (!ok) { LOG("[RED] Vulkan: the queue cannot present to the window"); return 0; }
    VkSurfaceCapabilitiesKHR cap; VKC(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_pd, g_surf, &cap));
    uint32_t nf = 0; vkGetPhysicalDeviceSurfaceFormatsKHR(g_pd, g_surf, &nf, NULL);
    VkSurfaceFormatKHR fm[32]; if (nf > 32) nf = 32; vkGetPhysicalDeviceSurfaceFormatsKHR(g_pd, g_surf, &nf, fm);
    if (!nf) { LOG("[RED] Vulkan: the surface has no formats"); return 0; }
    g_fmt = fm[0].format; VkColorSpaceKHR cs = fm[0].colorSpace; g_sc_has_srgb = 0;
    for (uint32_t i = 0; i < nf; i++) if (fm[i].format == VK_FORMAT_R8G8B8A8_SRGB || fm[i].format == VK_FORMAT_B8G8R8A8_SRGB) g_sc_has_srgb = 1;
    for (uint32_t i = 0; i < nf; i++) {   // the frame's encoding (sc_encoding), in the surface's RGBA or BGRA order
        VkFormat f = fm[i].format;
        if (g_sc_srgb && g_sc_has_srgb ? f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_SRGB : f == VK_FORMAT_R8G8B8A8_UNORM || f == VK_FORMAT_B8G8R8A8_UNORM) { g_fmt = f; cs = fm[i].colorSpace; break; }
    }
    int pw = 0, ph = 0; SDL_GetWindowSizeInPixels(g_win, &pw, &ph);
    g_ext2 = cap.currentExtent.width != 0xFFFFFFFFu ? cap.currentExtent : (VkExtent2D){(uint32_t)pw, (uint32_t)ph};
    if (!g_ext2.width || !g_ext2.height) return 0;   // a minimized window: made when it comes back
    VkSwapchainCreateInfoKHR sc = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = g_surf, .minImageCount = cap.minImageCount < 3 && (!cap.maxImageCount || cap.maxImageCount >= 3) ? 3 : cap.minImageCount,
        .imageFormat = g_fmt, .imageColorSpace = cs, .imageExtent = g_ext2, .imageArrayLayers = 1, .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = cap.currentTransform, .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = present_mode(), .clipped = 1};
    if (!(cap.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)) sc.compositeAlpha = (VkCompositeAlphaFlagBitsKHR)(cap.supportedCompositeAlpha & -cap.supportedCompositeAlpha);
    VKC(vkCreateSwapchainKHR(g_dev, &sc, NULL, &g_sc));
    g_nimg = 8; VKC(vkGetSwapchainImagesKHR(g_dev, g_sc, &g_nimg, g_img));
    LOG("window %ux%u, swapchain %u images, format %d, present mode %d", g_ext2.width, g_ext2.height, g_nimg, g_fmt, (int)sc.presentMode);
    return 1;
}
static uint64_t g_shown; static int g_sc_stale;
// The swapchain in the presented frame's encoding (front_android.c sc_encoding says why); DM_VK_SC_SRGB=0 keeps UNORM
static int sc_encoding(VkFormat f) {
    static int on = -1; if (on < 0) { const char *e = getenv("DM_VK_SC_SRGB"); on = !(e && *e == '0'); }
    int srgb = on && (f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_A8B8G8R8_SRGB_PACK32);
    if (srgb == g_sc_srgb || (srgb && !g_sc_has_srgb)) return 0;
    LOG("[front] the frame is %s (format %d): swapchain made again", srgb ? "sRGB" : "not sRGB", (int)f);
    g_sc_srgb = srgb; g_sc_stale = 1; return 1;
}
static void sc_follow_window(void) {
    int pw = 0, ph = 0; SDL_GetWindowSizeInPixels(g_win, &pw, &ph);
    if (g_sc && !g_sc_stale && (pw <= 1 || ph <= 1 || ((uint32_t)pw == g_ext2.width && (uint32_t)ph == g_ext2.height))) return;
    if (!g_sc && (pw <= 1 || ph <= 1)) return;
    g_sc_stale = 0; sc_down(); sc_up(); g_shown = 0;
}
// The display's vsyncs, where the swapchain reports them (VK_GOOGLE_display_timing; front_android.c's)
static _Atomic uint64_t g_vs_at, g_vs_r;
static PFN_vkGetRefreshCycleDurationGOOGLE g_vs_refresh; static PFN_vkGetPastPresentationTimingGOOGLE g_vs_past;
static VkSwapchainKHR g_vs_sc; static uint32_t g_vs_id;
uint64_t front_vsync(uint64_t *refresh) { *refresh = atomic_load_explicit(&g_vs_r, memory_order_relaxed); return atomic_load_explicit(&g_vs_at, memory_order_relaxed); }
static void vs_read(void) {
    vkb_ctx *c = vkb(); if (!c || !c->has_display_timing) return;
    if (!g_vs_past) {
        g_vs_refresh = (PFN_vkGetRefreshCycleDurationGOOGLE)vkGetDeviceProcAddr(g_dev, "vkGetRefreshCycleDurationGOOGLE");
        g_vs_past = (PFN_vkGetPastPresentationTimingGOOGLE)vkGetDeviceProcAddr(g_dev, "vkGetPastPresentationTimingGOOGLE");
        if (!g_vs_refresh || !g_vs_past) { c->has_display_timing = 0; return; }
    }
    if (g_vs_sc != g_sc) {
        VkRefreshCycleDurationGOOGLE rc = {0}; g_vs_sc = g_sc; atomic_store(&g_vs_at, 0);
        atomic_store(&g_vs_r, g_vs_refresh(g_dev, g_sc, &rc) == VK_SUCCESS ? rc.refreshDuration : 0);
    }
    VkPastPresentationTimingGOOGLE past[8]; uint32_t n = 8;
    VkResult r = g_vs_past(g_dev, g_sc, &n, past);
    if ((r == VK_SUCCESS || r == VK_INCOMPLETE) && n && past[n - 1].actualPresentTime) atomic_store_explicit(&g_vs_at, past[n - 1].actualPresentTime, memory_order_relaxed);
}
static void frame_draw(void) {
    sc_follow_window();
    if (!g_sc || atomic_load(&g_state) != ST_GAME) return;
    vkb_frame fr; if (!vkb_frame_lock(&fr)) return;
    if (fr.n == g_shown || sc_encoding(fr.fmt)) { vkb_frame_unlock(); return; }
    vkWaitForFences(g_dev, 1, &g_fence, 1, UINT64_MAX);
    uint32_t idx; VkResult r = vkAcquireNextImageKHR(g_dev, g_sc, 100000000ull, g_acq, VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) g_sc_stale = 1;   // made again before the next frame (sc_follow_window)
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { vkb_frame_unlock(); return; }
    vkResetFences(g_dev, 1, &g_fence); vkResetCommandBuffer(g_cb, 0);
    VkCommandBufferBeginInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(g_cb, &cbi);
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = g_img[idx], .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(g_cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkClearColorValue black = {{0, 0, 0, 1}}; VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(g_cb, g_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
    VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT};
    vkCmdPipelineBarrier(g_cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    // the crop rectangle into the window, scaled to fit with its aspect kept (black bars, never stretched or cropped)
    int cw = fr.crop[2] > 0 ? fr.crop[2] : (int)fr.w, ch = fr.crop[3] > 0 ? fr.crop[3] : (int)fr.h;
    double sx = (double)g_ext2.width / cw, sy = (double)g_ext2.height / ch, sc = sx < sy ? sx : sy;
    int dw = (int)(cw * sc), dh = (int)(ch * sc), dx = ((int)g_ext2.width - dw) / 2, dy = ((int)g_ext2.height - dh) / 2;
    VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{fr.crop[0], fr.crop[1], 0}, {fr.crop[0] + cw, fr.crop[1] + ch, 1}},
                      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{dx, dy, 0}, {dx + dw, dy + dh, 1}}};
    if (fr.flip) { bl.srcOffsets[0].y = fr.crop[1] + ch; bl.srcOffsets[1].y = fr.crop[1]; }   // the window image is stored bottom-up (NVN's lower-left origin)
    vkCmdBlitImage(g_cb, fr.img, VK_IMAGE_LAYOUT_GENERAL, g_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = 0; b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(g_cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    vkEndCommandBuffer(g_cb);
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &g_acq, .pWaitDstStageMask = &ws, .commandBufferCount = 1, .pCommandBuffers = &g_cb,
                       .signalSemaphoreCount = 1, .pSignalSemaphores = &g_done};
    vs_read(); VkPresentTimeGOOGLE pt = {g_vs_id++, vkb()->has_display_timing ? fr.show_at : 0};
    VkPresentTimesInfoGOOGLE pti = {.sType = VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE, .swapchainCount = 1, .pTimes = &pt};
    pthread_mutex_lock(&vkb()->qmu); vkQueueSubmit(g_q, 1, &si, g_fence);
    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .pNext = vkb()->has_display_timing ? &pti : NULL, .waitSemaphoreCount = 1, .pWaitSemaphores = &g_done, .swapchainCount = 1,
                           .pSwapchains = &g_sc, .pImageIndices = &idx};
    r = vkQueuePresentKHR(g_q, &pi); pthread_mutex_unlock(&vkb()->qmu);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) g_sc_stale = 1;
    vkWaitForFences(g_dev, 1, &g_fence, 1, UINT64_MAX);   // the texture is held until the GPU has read it
    g_shown = fr.n; vkb_frame_unlock();
}

// ---------- Input: SDL gamepads and the keyboard, merged into player 1 (front_android.c merges its touch controls and controller the same way) ----------
static SDL_Gamepad *g_pad;
static uint64_t g_pad_btn, g_key_btn; static int32_t g_axis[4];
static int abxy_label(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_PAD_ABXY"); v = e && !strcmp(e, "label"); } return v; }
// A, B, X and Y by where the button sits (SDL's south, east, west, north: the Switch's B, A, Y, X), or by the label the controller gives it (DM_PAD_ABXY=label)
static uint64_t face_button(SDL_GamepadButton b) {
    if (abxy_label() && g_pad) switch (SDL_GetGamepadButtonLabel(g_pad, b)) {
        case SDL_GAMEPAD_BUTTON_LABEL_A: return DM_BTN_A; case SDL_GAMEPAD_BUTTON_LABEL_B: return DM_BTN_B;
        case SDL_GAMEPAD_BUTTON_LABEL_X: return DM_BTN_X; case SDL_GAMEPAD_BUTTON_LABEL_Y: return DM_BTN_Y; default: break;
    }
    switch (b) {
        case SDL_GAMEPAD_BUTTON_SOUTH: return DM_BTN_B; case SDL_GAMEPAD_BUTTON_EAST: return DM_BTN_A;
        case SDL_GAMEPAD_BUTTON_WEST: return DM_BTN_Y; case SDL_GAMEPAD_BUTTON_NORTH: return DM_BTN_X; default: return 0;
    }
}
static uint64_t pad_button(SDL_GamepadButton b) {
    uint64_t f = face_button(b); if (f) return f;
    switch (b) {
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return DM_BTN_L; case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return DM_BTN_R;
        case SDL_GAMEPAD_BUTTON_LEFT_STICK: return DM_BTN_LS; case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return DM_BTN_RS;
        case SDL_GAMEPAD_BUTTON_START: return DM_BTN_PLUS; case SDL_GAMEPAD_BUTTON_BACK: return DM_BTN_MINUS;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return DM_BTN_LEFT; case SDL_GAMEPAD_BUTTON_DPAD_UP: return DM_BTN_UP;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return DM_BTN_RIGHT; case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return DM_BTN_DOWN;
        default: return 0;
    }
}
// The keyboard: arrows the D-pad, X / Z the Switch's A / B, S / A its X / Y, Q / W L / R, 1 / 2 ZL / ZR, Return plus, Backspace minus
static uint64_t key_button(SDL_Keycode k) {
    switch (k) {
        case SDLK_LEFT: return DM_BTN_LEFT; case SDLK_UP: return DM_BTN_UP; case SDLK_RIGHT: return DM_BTN_RIGHT; case SDLK_DOWN: return DM_BTN_DOWN;
        case SDLK_X: return DM_BTN_A; case SDLK_Z: return DM_BTN_B; case SDLK_S: return DM_BTN_X; case SDLK_A: return DM_BTN_Y;
        case SDLK_Q: return DM_BTN_L; case SDLK_W: return DM_BTN_R; case SDLK_1: return DM_BTN_ZL; case SDLK_2: return DM_BTN_ZR;
        case SDLK_RETURN: return DM_BTN_PLUS; case SDLK_BACKSPACE: return DM_BTN_MINUS; default: return 0;
    }
}
static int32_t ax(Sint16 v) { int32_t x = v < -DM_STICK_MAX ? -DM_STICK_MAX : v; return x > -3900 && x < 3900 ? 0 : x; }   // Android's 0.12 dead zone
static void publish(void) {
    uint64_t b = g_pad_btn | g_key_btn;
    if (g_pad) {
        if (SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16384) b |= DM_BTN_ZL;   // analog triggers past half
        if (SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16384) b |= DM_BTN_ZR;
    }
    dm_input_set(b, g_axis[0], g_axis[1], g_axis[2], g_axis[3]);
}
static void pad_open(SDL_JoystickID id) {
    if (g_pad) return;
    if (!(g_pad = SDL_OpenGamepad(id))) { LOG("[RED] controller %u: %s", (unsigned)id, SDL_GetError()); return; }
    LOG("controller: %s, player 1 (buttons by %s; DM_PAD_ABXY=%s maps by %s)", SDL_GetGamepadName(g_pad), abxy_label() ? "label" : "position",
        abxy_label() ? "position" : "label", abxy_label() ? "position" : "label");
}
static void pad_close(SDL_JoystickID id) {
    if (!g_pad || SDL_GetGamepadID(g_pad) != id) return;
    SDL_CloseGamepad(g_pad); g_pad = NULL; g_pad_btn = 0; memset(g_axis, 0, sizeof g_axis); publish();
    int n = 0; SDL_JoystickID *ids = SDL_GetGamepads(&n); if (ids && n) pad_open(ids[0]); SDL_free(ids);   // the next one takes over
}

// ---------- Vibration: player 1's motors on the gamepad's (haptics_android.c's sink; the main loop sends them) ----------
static pthread_mutex_t g_hmu = PTHREAD_MUTEX_INITIALIZER; static dm_haptic_state g_hstate; static int g_hnew; static float g_gain = 1;
static void haptic_sink(dm_haptic_state s) { if (s.player != 0) return; pthread_mutex_lock(&g_hmu); g_hstate = s; g_hnew = 1; pthread_mutex_unlock(&g_hmu); }
static void haptics_start(void) {
    const char *on = getenv("DM_RUMBLE"), *gain = getenv("DM_RUMBLE_GAIN");
    if (gain && atof(gain) > 0) g_gain = (float)atof(gain);
    dm_haptics_init(haptic_sink, !on || atoi(on) != 0);
    dm_haptics_route(0, 1);
}
static void haptics_tick(void) {   // the newest state, and again every 800 ms while it lasts (each rumble is given 1 s)
    static Uint16 lo, hi; static Uint64 at;
    pthread_mutex_lock(&g_hmu); int fresh = g_hnew; dm_haptic_state s = g_hstate; g_hnew = 0; pthread_mutex_unlock(&g_hmu);
    if (!g_pad) return;
    if (fresh) {
        float l = s.motor[0].amplitude_low > s.motor[1].amplitude_low ? s.motor[0].amplitude_low : s.motor[1].amplitude_low;
        float h = s.motor[0].amplitude_high > s.motor[1].amplitude_high ? s.motor[0].amplitude_high : s.motor[1].amplitude_high;
        l *= g_gain; h *= g_gain;
        lo = (Uint16)(l < 0.02f ? 0 : l > 1 ? 65535 : l * 65535); hi = (Uint16)(h < 0.02f ? 0 : h > 1 ? 65535 : h * 65535);
    } else if (!(lo || hi) || SDL_GetTicks() - at < 800) return;
    SDL_RumbleGamepad(g_pad, lo, hi, lo || hi ? 1000 : 0); at = SDL_GetTicks();
}

// ---------- Main ----------
static void quit(int code) {
    vkb_pipeline_cache_save();
    LOG("front end: quit");
    fflush(stderr); usleep(200000);   // the log reader writes the last lines
    _exit(code);
}
static void on_window(const SDL_WindowEvent *w) {
    int game = atomic_load(&g_state) == ST_GAME;
    switch (w->type) {
        case SDL_EVENT_WINDOW_MINIMIZED: case SDL_EVENT_WINDOW_HIDDEN: if (game) { dm_audio_suspend(1); dm_focus_set(3); } break;
        case SDL_EVENT_WINDOW_RESTORED: case SDL_EVENT_WINDOW_SHOWN: if (game) { dm_audio_suspend(0); dm_focus_set(1); } g_shown = 0; break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: g_sc_stale = 1; break;
        default: break;   // focus changes are left alone: a handheld's overlays (the Steam button) would pause the game
    }
}
int main(int argc, char **argv) {
    data_dir();
    if (argc > 1) snprintf(g_game, sizeof g_game, "%s", argv[1]); else snprintf(g_game, sizeof g_game, "%s/games/%s", g_data, DM_GAME_ID);
    char lp[600]; snprintf(lp, sizeof lp, "%s/log.txt", g_data); int kept = keep_prev(lp);   // the previous run's log (prev_file.h), before log_reader truncates it
    log_start();
    LOG("nxnative on Linux: %s (%s), game folder %s, data %s", DM_GAME_NAME, DM_GAME_ID, g_game, g_data);
    env_file(g_data, "env.txt", 1); env_file(g_game, "launcher.txt", 0); env_file(g_game, "defaults.txt", 0);
    {   const char *ad = getenv("DM_AUDIO_DUMP"); kept += keep_prev(getenv("DM_NVN_TSV"));
        if (ad && *ad) { kept += keep_prev(ad); for (int i = 1; i < 8; i++) { char p[1024]; snprintf(p, sizeof p, "%s.%d", ad, i); kept += keep_prev(p); } }
        if (kept) LOG("the previous run's log and dumps: %d file%s kept beside the new ones, with .prev before the extension (log.prev.txt)", kept, kept == 1 ? "" : "s"); }
    vk_driver_dirs(g_data, ""); vk_driver_game(g_game, DM_GAME_ID); vk_start_game(g_game);
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) { LOG("[RED] SDL: %s", SDL_GetError()); quit(1); }
    const char *wd = getenv("DM_WINDOWED"); int windowed = wd && atoi(wd);
    g_win = SDL_CreateWindow(DM_GAME_NAME, 1280, 720, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | (windowed ? 0 : SDL_WINDOW_FULLSCREEN));
    if (!g_win) { LOG("[RED] SDL: no window: %s", SDL_GetError()); quit(1); }
    LOG("window: SDL %d.%d.%d, video driver %s%s", SDL_VERSIONNUM_MAJOR(SDL_GetVersion()), SDL_VERSIONNUM_MINOR(SDL_GetVersion()), SDL_VERSIONNUM_MICRO(SDL_GetVersion()),
        SDL_GetCurrentVideoDriver(), windowed ? ", windowed (DM_WINDOWED)" : ", fullscreen (F11 switches)");
    if (!windowed) SDL_HideCursor();
    g_wake = SDL_RegisterEvents(1);
    if (!sc_up()) LOG("[RED] no picture: the window's swapchain could not be made (the lines above say why)");
    haptics_start();
    if (!start_host()) quit(1);
    for (;;) {
        SDL_Event ev;
        if (SDL_WaitEventTimeout(&ev, 100)) do {
            switch (ev.type) {
                case SDL_EVENT_QUIT: quit(atomic_load(&g_state) == ST_CRASHED ? 1 : 0); break;
                case SDL_EVENT_GAMEPAD_ADDED: pad_open(ev.gdevice.which); break;
                case SDL_EVENT_GAMEPAD_REMOVED: pad_close(ev.gdevice.which); break;
                case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP:
                    if (g_pad && ev.gbutton.which == SDL_GetGamepadID(g_pad)) {
                        uint64_t b = pad_button((SDL_GamepadButton)ev.gbutton.button);
                        if (ev.gbutton.down) g_pad_btn |= b; else g_pad_btn &= ~b;
                        publish();
                    }
                    break;
                case SDL_EVENT_GAMEPAD_AXIS_MOTION:
                    if (g_pad && ev.gaxis.which == SDL_GetGamepadID(g_pad)) {
                        switch (ev.gaxis.axis) {
                            case SDL_GAMEPAD_AXIS_LEFTX: g_axis[0] = ax(ev.gaxis.value); break; case SDL_GAMEPAD_AXIS_LEFTY: g_axis[1] = -ax(ev.gaxis.value); break;   // the Switch's y points up
                            case SDL_GAMEPAD_AXIS_RIGHTX: g_axis[2] = ax(ev.gaxis.value); break; case SDL_GAMEPAD_AXIS_RIGHTY: g_axis[3] = -ax(ev.gaxis.value); break;
                            default: break;
                        }
                        publish();
                    }
                    break;
                case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: {
                    SDL_Keycode k = ev.key.key;
                    if (ev.key.down && !ev.key.repeat && k == SDLK_ESCAPE) quit(0);
                    if (ev.key.down && !ev.key.repeat && k == SDLK_F11) { int fs = !(SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN); SDL_SetWindowFullscreen(g_win, fs); if (fs) SDL_HideCursor(); else SDL_ShowCursor(); }
                    uint64_t b = key_button(k); if (!b) break;
                    if (ev.key.down) g_key_btn |= b; else g_key_btn &= ~b;
                    publish(); break;
                }
                default:
                    if (ev.type >= SDL_EVENT_WINDOW_FIRST && ev.type <= SDL_EVENT_WINDOW_LAST) on_window(&ev.window);
                    break;
            }
        } while (SDL_PollEvent(&ev));
        frame_draw(); haptics_tick(); vk_start_tick();
    }
}
