// The Vulkan driver behind gfx_vk.h's vk* pointers.
//   By default the system's: libvulkan.so, opened when the replay makes its device (vkb), as linking it did before.
//   DM_VK_DRIVER=<file> (env.txt) runs another driver, such as a build of Mesa's Turnip for Adreno GPUs, as players of Switch emulators use: a .so the player puts on the
//   device, named relative to the folder env.txt is in (or by an absolute path). Android loads no library from shared storage, so it is copied into the app's internal files
//   (vk_driver/<name>), and opened through libadrenotools (dm/third_party/libadrenotools): a second copy of the system's libvulkan.so in a linker namespace of its own, where
//   a hook loads the player's driver in place of the vendor's. The loader in that copy still makes the window surface and the swapchain, which Turnip on Android lacks.
//   A driver that cannot be copied or opened, or that offers no GPU, logs one [RED] line, and the system's driver runs. So does one that opens and then fails to make the
//   instance, find the GPU's graphics queue or the window extensions, or make the device (gfx_vk.c vk_create, through vk_driver_fallback), or the first window's surface
//   or swapchain (front_android.c vk_window_first): a player's choice of driver must never leave a game without a picture. On a phone whose GPU is neither an Adreno nor a
//   Samsung Xclipse, DM_VK_DRIVER is not opened at all.
#include <dirent.h>
#include <dlfcn.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <drm/drm.h>
#include "dm.h"
#include "gfx_vk.h"
#ifdef __ANDROID__
#include "../third_party/libadrenotools/include/adrenotools/driver.h"   // the type of adrenotools_open_libvulkan, opened with dlsym: the host does not link it
#endif

#define VKB_DEF(f) PFN_##f f;
VKB_FNS_GLOBAL(VKB_DEF) VKB_FNS(VKB_DEF)
static PFN_vkGetInstanceProcAddr g_gipa;
static char g_ext[512], g_int[512], g_name[256];   // vk_driver_dirs; g_name: the DM_VK_DRIVER file running

void vk_driver_dirs(const char *ext, const char *internal) { snprintf(g_ext, sizeof g_ext, "%s", ext); snprintf(g_int, sizeof g_int, "%s", internal); }
const char *vk_driver_name(void) { return g_name[0] ? g_name : NULL; }
static char g_why[300];   // the last DRV_RED line's text up to "; the system's driver": why the DM_VK_DRIVER driver isn't running, for its records and the status screen
static void __attribute__((format(printf, 2, 3))) drv_red(const char *drv, const char *fmt, ...) {
    char b[1024]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    LOG("[RED] [vk] DM_VK_DRIVER %s: %s", drv, b);
    char *cut = strstr(b, "; the system's driver"); if (cut) *cut = 0;
    snprintf(g_why, sizeof g_why, "%s", b);
}
#define DRV_RED(drv, fmt, ...) drv_red(drv, fmt, ##__VA_ARGS__)

// src → <dir><name> (dir: <internal>/vk_driver/), unless the copy there has src's size and time already. Read-only once written, as Android 14 asks of code an app loads
// from its own files. → 0 with errno set
static int copy_in(const char *src, const char *dir, const char *name) {
    struct stat s, d; char dst[800], tmp[810];
    if (stat(src, &s)) return 0;
    if (!S_ISREG(s.st_mode)) { errno = EISDIR; return 0; }
    if (mkdir(dir, 0700) && errno != EEXIST) return 0;
    snprintf(dst, sizeof dst, "%s%s", dir, name); snprintf(tmp, sizeof tmp, "%s.part", dst);
    if (!stat(dst, &d) && d.st_size == s.st_size && d.st_mtime == s.st_mtime) return 1;
    int in = open(src, O_RDONLY | O_CLOEXEC); if (in < 0) return 0;
    unlink(tmp); int out = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (out < 0) { int e = errno; close(in); errno = e; return 0; }
    static char buf[1 << 16]; ssize_t k = 0; int ok = 1;
    while (ok && (k = read(in, buf, sizeof buf)) > 0) for (ssize_t w = 0, n; w < k; w += n) if ((n = write(out, buf + w, (size_t)(k - w))) <= 0) { ok = 0; break; }
    struct timespec ts[2] = {s.st_atim, s.st_mtim};
    ok = ok && k == 0 && !fchmod(out, 0500) && !futimens(out, ts);
    int e = errno; close(in); if (close(out)) ok = 0;
    if (ok) { unlink(dst); ok = !rename(tmp, dst); e = errno; }
    if (!ok) unlink(tmp);
    errno = e; return ok;
}

// Whether the file at path is mapped in this process: when adrenotools' hook cannot put the player's driver in place, it opens the vendor's instead and says so only in
// logcat. By device and inode, as /proc/self/maps lists them (its path column may name the file through another mount, /data/data for /data/user/0). (Not
// dl_iterate_phdr: <link.h> clashes with dm.h's ELF types). Its addresses, from the lowest mapping's start to the highest's end, are kept for the crash guard (crash_hook)
static uint64_t g_lo, g_hi;
static int mapped(const char *path) {
    struct stat st; if (stat(path, &st)) return 0;
    FILE *f = fopen("/proc/self/maps", "re"); if (!f) return 0;
    char line[1024]; unsigned maj, mnr; unsigned long long lo, hi, ino; uint64_t l = 0, h = 0;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "%llx-%llx %*s %*s %x:%x %llu", &lo, &hi, &maj, &mnr, &ino) == 5 && ino == (unsigned long long)st.st_ino && maj == major(st.st_dev) && mnr == minor(st.st_dev)) {
            if (!l || lo < l) l = lo;
            if (hi > h) h = hi;
        }
    fclose(f);
    if (h) { g_lo = l; g_hi = h; }
    return h != 0;
}

// The GPU a driver offers, on an instance made for the question: a Turnip built for another Adreno generation opens and then has none
static int first_gpu(PFN_vkGetInstanceProcAddr g, VkPhysicalDeviceProperties *pr) {
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)g(NULL, "vkCreateInstance");
    VkApplicationInfo ai = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "nxnative", .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ic = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai};
    VkInstance in; if (!create || create(&ic, NULL, &in) != VK_SUCCESS) return 0;
    PFN_vkEnumeratePhysicalDevices gpus = (PFN_vkEnumeratePhysicalDevices)g(in, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties props = (PFN_vkGetPhysicalDeviceProperties)g(in, "vkGetPhysicalDeviceProperties");
    PFN_vkDestroyInstance destroy = (PFN_vkDestroyInstance)g(in, "vkDestroyInstance");
    uint32_t n = 1; VkPhysicalDevice pd; VkResult r = gpus ? gpus(in, &n, &pd) : VK_ERROR_INITIALIZATION_FAILED;
    int ok = (r == VK_SUCCESS || r == VK_INCOMPLETE) && n && props;
    if (ok) props(pd, pr);
    if (destroy) destroy(in, NULL);
    return ok;
}

// DM_VK_DRIVER's driver → its vkGetInstanceProcAddr, NULL (and one [RED] line) when it cannot run
static PFN_vkGetInstanceProcAddr open_custom(const char *drv) {
#ifdef DM_LINUX   // the Vulkan loader of a Linux system picks the driver itself: VK_DRIVER_FILES names another one's manifest
    DRV_RED(drv, "DM_VK_DRIVER is for Android; on Linux set VK_DRIVER_FILES to the driver's ICD manifest. The system's driver instead"); return NULL;
#else
    char src[700], dir[600]; const char *name;
    if (drv[0] == '/' || !g_ext[0]) snprintf(src, sizeof src, "%s", drv); else snprintf(src, sizeof src, "%s/%s", g_ext, drv);
    name = strrchr(src, '/') ? strrchr(src, '/') + 1 : src;
    if (!*name || strlen(name) >= sizeof g_name) { DRV_RED(drv, "not a file name; the system's driver instead"); return NULL; }
    if (g_int[0]) {   // the app: copied in
        snprintf(dir, sizeof dir, "%s/vk_driver/", g_int);
        if (!copy_in(src, dir, name)) { DRV_RED(drv, "%s: %s; the system's driver instead", src, strerror(errno)); return NULL; }
    } else {          // the command-line host (adb shell): loaded where it lies, which must be a folder that allows code (/data/local/tmp, not /sdcard)
        if (access(src, R_OK)) { DRV_RED(drv, "%s: %s; the system's driver instead", src, strerror(errno)); return NULL; }
        snprintf(dir, sizeof dir, "%.*s/", name > src ? (int)(name - src - 1) : 1, name > src ? src : ".");
    }
    // adrenotools' hooks (libhook_impl.so, libmain_hook.so) are looked for where the host lies: the app's native library folder, extracted (AndroidManifest extractNativeLibs)
    Dl_info di; char libs[600] = "";
    if (dladdr((void *)open_custom, &di) && di.dli_fname && strrchr(di.dli_fname, '/'))
        snprintf(libs, sizeof libs, "%.*s/", (int)(strrchr(di.dli_fname, '/') - di.dli_fname), di.dli_fname);
    void *at = dlopen("libadrenotools.so", RTLD_NOW | RTLD_LOCAL);
    __typeof__(adrenotools_open_libvulkan) *open_vk = at ? (__typeof__(adrenotools_open_libvulkan) *)dlsym(at, "adrenotools_open_libvulkan") : NULL;
    if (!open_vk) { DRV_RED(drv, "libadrenotools.so is not in this app (%s): pack the app again; the system's driver instead", dlerror()); return NULL; }
    void *lib = open_vk(RTLD_NOW | RTLD_LOCAL, ADRENOTOOLS_DRIVER_CUSTOM, NULL, libs, dir, name, NULL, NULL);
    PFN_vkGetInstanceProcAddr g = lib ? (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr") : NULL;
    if (!g) { DRV_RED(drv, "libadrenotools could not open it (adb logcat -s hook_impl says why); the system's driver instead"); return NULL; }
    // (a driver that opened and is not used stays loaded: drivers do not always unload cleanly)
    VkPhysicalDeviceProperties pr; char path[900]; snprintf(path, sizeof path, "%s%s", dir, name);
    if (!first_gpu(g, &pr)) { DRV_RED(drv, "it offers no GPU (a build for another GPU?); the system's driver instead"); return NULL; }
    if (!mapped(path)) {
        DRV_RED(drv, "libadrenotools opened the system's driver in its place (adb logcat -s hook_impl says why); the system's driver instead"); return NULL; }
    snprintf(g_name, sizeof g_name, "%s", name);
    LOG("[vk] driver: %s (DM_VK_DRIVER, through libadrenotools), not the system's: %s, Vulkan %u.%u.%u, driver version %#x", path, pr.deviceName,
        VK_API_VERSION_MAJOR(pr.apiVersion), VK_API_VERSION_MINOR(pr.apiVersion), VK_API_VERSION_PATCH(pr.apiVersion), pr.driverVersion);
    return g;
#endif
}

#define VKB_LOAD_GLOBAL(f) f = (PFN_##f)g_gipa(NULL, #f);
static int open_system(void) {
#ifdef DM_LINUX
    const char *so = "libvulkan.so.1";   // the Khronos loader's soname (libvulkan.so is only in development packages)
#else
    const char *so = "libvulkan.so";
#endif
    void *lib = dlopen(so, RTLD_NOW | RTLD_LOCAL);
    if (!lib || !(g_gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr"))) { LOG("[RED] [vk] no Vulkan: %s: %s", so, dlerror()); return 0; }
    VKB_FNS_GLOBAL(VKB_LOAD_GLOBAL)
    if (!vkCreateInstance || !vkEnumerateInstanceExtensionProperties) { LOG("[RED] [vk] no Vulkan: the driver has no vkCreateInstance"); return 0; }
    return 1;
}
// The GPUs a player's driver can run on. libadrenotools' custom-driver load is the same for any vendor (it swaps the file the system's loader opens as vulkan.<x>.so), so
// what decides is the kernel GPU driver the player's driver drives. Elsewhere (a Mali phone) no such driver has anything to run on, and libadrenotools.so's constructor
// (liblinkernsbypass) would patch the linker for nothing.
//   Adreno: Turnip drives Qualcomm's KGSL. Only a missing node counts: one SELinux keeps from a stat is still there
static int adreno(void) { struct stat st; return !stat("/dev/kgsl-3d0", &st) || errno != ENOENT; }
//   Xclipse: RADV built for it (radv-xclipse, as Eden loads it) drives Samsung's kernel GPU driver, sgpu, AMD's amdgpu adapted, through a DRM render node, which apps
//   may open (their Vulkan driver does). Told as RADV tells its GPU (radv_physical_device_try_create): the node's DRM driver name is "amdgpu", which sgpu keeps (sysfs
//   names its platform driver sgpu). Other render nodes have other names (the Flip 7 also has an exynos-drm one, for the display)
static int xclipse(void) {
    for (int i = 128; i < 136; i++) {
        char p[32], name[16] = ""; snprintf(p, sizeof p, "/dev/dri/renderD%d", i);
        int fd = open(p, O_RDWR | O_CLOEXEC); if (fd < 0) continue;
        struct drm_version v = {.name_len = sizeof name - 1, .name = name};
        int ok = !ioctl(fd, DRM_IOCTL_VERSION, &v) && !strcmp(name, "amdgpu");
        close(fd); if (ok) return 1;
    }
    return 0;
}

// ---------------- Start records and the crash guard ----------------
// A driver that crashes the game, hangs it or loses the GPU would do so again at every start. So each start with a DM_VK_DRIVER driver is recorded, and one that went wrong
// makes the next start run on the system's driver, once, saying why in the log and on the status screen (vk_driver_note); the start after that tries the driver again.
// The records are in the external files, where the desktop app can read and clear them over adb (the app isn't debuggable: its internal files are out of reach):
//   vk_driver_state/<driver file>.json      the driver's last start, whatever the game: {"state": …, "game": <title ID>, "time": <Unix time>, "reason": …}
//   games/<title ID>/vk_driver_result.txt   this game's last start: driver=, result= (a state), reason=, time= lines; removed at a start without DM_VK_DRIVER
// The states: trying (written before the driver opens), ok (120 frames presented, 30 s of the game, or the self test's picture check drawn: a first start compiles for longer
// than 30 s), fell back (it failed and the system's driver ran), crashed (in the driver's code), crashed elsewhere (in the host's or the game's code, the driver running),
// device lost, skipped and not used (neither an Adreno nor an Xclipse GPU: the result file only). A start that finds crashed, device lost or trying (a start that ended before ok: a crash the
// handler did not see, a hang the player ended, or a game closed in its first 30 s) skips the driver and writes skipped; crashed elsewhere is only said, since the driver
// may have no part in it (a game that crashed at boot would otherwise change drivers at every start). A hang in the driver after ok isn't caught: the records can't tell it
// from a long compile, and a game hung after its first 30 s is ended by the player, which leaves ok
static char g_game_dir[600], g_game_id[40], g_state[700], g_result[700], g_drv[256], g_note[300];
static int g_note_warn;
static _Atomic int g_bad, g_writing;   // g_bad: 1 crashed or device lost was written, 2 crashed elsewhere: ok must not replace either; g_writing: ok is being written (mark_bad)
static int g_last_elsewhere;   // skip_once: the last start crashed outside the driver's code, which the status line says
void vk_driver_game(const char *dir, const char *id) { snprintf(g_game_dir, sizeof g_game_dir, "%s", dir); snprintf(g_game_id, sizeof g_game_id, "%s", id); }
const char *vk_driver_note(int *warn) { if (warn) *warn = g_note_warn; return g_note[0] ? g_note : NULL; }

static void esc(char *o, size_t n, const char *s, int json) {   // control characters as spaces; for JSON, quotes and backslashes escaped too
    size_t k = 0;
    for (; *s && k + 2 < n; s++) { if (json && (*s == '"' || *s == '\\')) o[k++] = '\\'; o[k++] = (unsigned char)*s < 0x20 ? ' ' : *s; }
    o[k] = 0;
}
static void put(const char *path, const char *text) {   // whole or not at all (a rename); from the crash handler too, so no allocation and no lock
    char tmp[720]; snprintf(tmp, sizeof tmp, "%s.%d.new", path, (int)gettid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666); if (fd < 0) return;
    size_t n = strlen(text); int ok = write(fd, text, n) == (ssize_t)n;
    if (close(fd) || !ok || rename(tmp, path)) unlink(tmp);
}
static void record(const char *state, const char *reason) {
    char r[400], b[1200]; long long t = (long long)time(NULL);
    if (g_state[0]) { esc(r, sizeof r, reason, 1); snprintf(b, sizeof b, "{\"state\": \"%s\", \"game\": \"%s\", \"time\": %lld, \"reason\": \"%s\"}\n", state, g_game_id, t, r); put(g_state, b); }
    if (g_result[0]) { esc(r, sizeof r, reason, 0); snprintf(b, sizeof b, "driver=%s\nresult=%s\nreason=%s\ntime=%lld\n", g_drv, state, r, t); put(g_result, b); }
}
static void drop_parts(const char *dir, const char *prefix) {   // put's <name>.<tid>.new files that a start killed mid-write left
    DIR *dd = opendir(dir); struct dirent *e; char p[900]; size_t n = strlen(prefix);
    while (dd && (e = readdir(dd))) {
        size_t k = strlen(e->d_name);
        if (!strncmp(e->d_name, prefix, n) && k > 4 && !strcmp(e->d_name + k - 4, ".new")) { snprintf(p, sizeof p, "%s/%s", dir, e->d_name); unlink(p); }
    }
    if (dd) closedir(dd);
}
static void record_paths(const char *drv, int state) {   // drv NULL: no DM_VK_DRIVER, so this game's old result goes
    char d[600];
    if (g_game_dir[0]) drop_parts(g_game_dir, "vk_driver_result.txt.");
    if (!drv) { if (g_game_dir[0]) { snprintf(d, sizeof d, "%s/vk_driver_result.txt", g_game_dir); unlink(d); } return; }
    const char *b = strrchr(drv, '/') ? strrchr(drv, '/') + 1 : drv;
    snprintf(g_drv, sizeof g_drv, "%s", b);
    if (state && g_ext[0] && *b) { snprintf(d, sizeof d, "%s/vk_driver_state", g_ext); mkdir(d, 0777); drop_parts(d, ""); snprintf(g_state, sizeof g_state, "%s/%s.json", d, b); }
    if (g_game_dir[0]) snprintf(g_result, sizeof g_result, "%s/vk_driver_result.txt", g_game_dir);
}
static int jstr(const char *b, const char *key, char *o, size_t n) {   // a string field of a record, as record writes it
    char k[32]; snprintf(k, sizeof k, "\"%s\": \"", key);
    const char *s = strstr(b, k); size_t i = 0;
    if (s) for (s += strlen(k); *s && *s != '"' && i + 1 < n; s++) { if (*s == '\\' && s[1]) s++; o[i++] = *s; }
    o[i] = 0; return s != NULL;
}
// The driver's last start went wrong: 1 when this start skips it (and skipped is written, so the next one tries it again)
static int skip_once(void) {
    char b[2048], st[32] = "", why[300] = "", game[40] = ""; size_t n = 0;
    FILE *f = g_state[0] ? fopen(g_state, "re") : NULL;
    if (f) { n = fread(b, 1, sizeof b - 1, f); fclose(f); }
    b[n] = 0; jstr(b, "state", st, sizeof st); jstr(b, "reason", why, sizeof why); jstr(b, "game", game, sizeof game);
    static const char *const bad[][3] = {{"crashed", "crashed in the driver", ""}, {"device lost", "lost the GPU device", ""},
                                         {"trying", "ended in its first 30 s", ": a crash or hang the host could not catch, or the game was closed"}};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        if (strcmp(st, bad[i][0])) continue;
        char r[sizeof g_why]; int other = game[0] && strcmp(game, g_game_id);
        snprintf(r, sizeof r, "the last start with it%s%s%s %s%s%s%s%s", other ? " (title " : "", other ? game : "", other ? ")" : "", bad[i][1],
                 why[0] ? " (" : "", why, why[0] ? ")" : "", bad[i][2]);
        DRV_RED(g_drv, "%s; the system's driver this time, and the next start tries it again", r);
        record("skipped", r);
        snprintf(g_note, sizeof g_note, "Vulkan driver: the system's this time, as the last start with %s %s", g_drv, bad[i][1]); g_note_warn = 1;
        return 1;
    }
    if (!strcmp(st, "crashed elsewhere")) { LOG("[vk] DM_VK_DRIVER %s: the last start with it crashed outside the driver's code (%s), so it runs again", g_drv, why); g_last_elsewhere = 1; }
    return 0;
}
static void fell_back(const char *why) {
    record("fell back", why);
    snprintf(g_note, sizeof g_note, "Vulkan driver: the system's; %s: %.200s", g_drv, why); g_note_warn = 1;
}
static int in_drv(uint64_t a) { return a >= g_lo && a < g_hi; }
// crash.c, in the signal handler of the first thread that crashed: a crash in the driver's code is recorded. Its pc, its lr, or a return address in the first 16 frames of
// its frame chain (an abort or a libc fault the driver called) in the driver's mapping
// A bad state is written after an ok being written, never under it: ok checks g_bad with g_writing set, and this waits (at most 100 ms, in case the thread that crashed
// is the one writing ok) for g_writing to clear before its own record
static void mark_bad(int v) { atomic_store(&g_bad, v); for (int i = 0; i < 100 && atomic_load(&g_writing); i++) usleep(1000); }
static void crash_hook(int sig, uint64_t pc, uint64_t lr, uint64_t fp) {
    if (!g_name[0] || !g_hi || sig == SIGALRM) return;   // (SIGALRM: the run's time limit, wherever it finds the thread)
    uint64_t pc0 = pc;
    uint64_t at = in_drv(pc) ? pc : in_drv(lr) ? lr : 0;
    for (int i = 0; !at && i < 16 && fp && !(fp & 7); i++) {
        uint64_t w[2]; vm_size_t got = 0;
        if (vm_read_overwrite(mach_task_self(), (vm_address_t)fp, sizeof w, (vm_address_t)w, &got) != KERN_SUCCESS || got != sizeof w) break;
        if (in_drv(w[1])) at = w[1];
        if (w[0] <= fp) break;
        fp = w[0];
    }
    char why[160], line[400];
    if (!at) {   // a crash with the driver running, outside its code: said, but no reason to skip the driver (a device lost already written stays)
        if (atomic_load(&g_bad)) return;
        module_t *m = module_of((void *)pc0); Dl_info di;
        if (m) snprintf(why, sizeof why, "signal %d at %s+%#llx", sig, m->name, (unsigned long long)(pc0 - (uint64_t)m->base));
        else if (dladdr((void *)pc0, &di) && di.dli_fname) snprintf(why, sizeof why, "signal %d at %s+%#llx", sig, strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname,
                                                                    (unsigned long long)(pc0 - (uint64_t)di.dli_fbase));
        else snprintf(why, sizeof why, "signal %d at %#llx", sig, (unsigned long long)pc0);
        mark_bad(2); record("crashed elsewhere", why); return;
    }
    snprintf(why, sizeof why, "signal %d at %s+%#llx", sig, g_name, (unsigned long long)(at - g_lo));
    mark_bad(1); record("crashed", why);
    int n = snprintf(line, sizeof line, "[RED] [vk] DM_VK_DRIVER %s: the crash below is in its code (%s), so the next start runs the system's driver once\n", g_name, why);
    if (n > 0 && write(2, line, (size_t)n) < 0) {}
}
void vk_driver_ok(void) {
    static int done; if (!g_name[0] || done) return;
    done = 1; atomic_store(&g_writing, 1);
    if (!atomic_load(&g_bad)) record("ok", "");
    atomic_store(&g_writing, 0);
}
void vk_driver_lost(VkResult r) { if (g_name[0]) { mark_bad(1); record("device lost", vk_result_str(r)); } }

int vk_driver_load(void) {
    const char *drv = getenv("DM_VK_DRIVER");
    int on = drv && *drv;
    const char *gpu = !on ? NULL : adreno() ? "Adreno" : xclipse() ? "Xclipse" : NULL;   // the GPU a player's driver can run on, or NULL
    record_paths(on ? drv : NULL, gpu != NULL);
    if (on && !gpu) {
        LOG("[vk] DM_VK_DRIVER %s: not used, this GPU is neither an Adreno (no /dev/kgsl-3d0) nor a Samsung Xclipse (no amdgpu render node), the GPUs another driver (Turnip, "
            "RADV for Xclipse) runs on: the system's driver runs", drv);
        record("not used", "not an Adreno or Xclipse GPU (no /dev/kgsl-3d0, no amdgpu render node)");
        snprintf(g_note, sizeof g_note, "Vulkan driver: the system's (%s is for Adreno and Xclipse GPUs only)", g_drv); g_note_warn = 1;
    } else if (gpu && !skip_once()) {
        LOG("[vk] DM_VK_DRIVER %s: trying it on this %s GPU", drv, gpu);
        record("trying", "");
        if ((g_gipa = open_custom(drv))) {
            void crash_set_pc_hook(void (*)(int, uint64_t, uint64_t, uint64_t)); crash_set_pc_hook(crash_hook);
            snprintf(g_note, sizeof g_note, "Vulkan driver: %s (DM_VK_DRIVER)%s", g_name, g_last_elsewhere ? "; its last start crashed outside the driver" : ""); g_note_warn = 0;
        } else fell_back(g_why);
    }
    if (g_gipa) {
        VKB_FNS_GLOBAL(VKB_LOAD_GLOBAL)
        if (vkCreateInstance && vkEnumerateInstanceExtensionProperties) return 1;
        DRV_RED(g_name, "it has no vkCreateInstance; the system's driver instead"); g_name[0] = 0; fell_back(g_why);
    }
    return open_system();
}

// DM_VK_DRIVER_FAIL=<step> (env.txt, a test): that step fails while the DM_VK_DRIVER driver runs, as if the driver had failed it, so the fallback can be tried on a phone
// whose driver works. Steps: instance, gpu, queue, extensions, device (gfx_vk.c vk_try), swapchain (the first window's, front_android.c vk_window_up), crash (a crash in
// the driver's code at the replay's first use of the device, gfx_vk.c chunks_init, for the crash guard above) and crash_host (one in the host's code there: crashed elsewhere). The system's driver it falls back to is left alone
int vk_driver_fail(const char *step) {
    const char *e = getenv("DM_VK_DRIVER_FAIL");
    return g_name[0] && e && !strcmp(e, step);
}
// The DM_VK_DRIVER driver failed after the probe: the system's driver from now on, with one [RED] line saying why. 0: none was running, or the system's has no Vulkan
int vk_driver_fallback(const char *why) {
    if (!g_name[0]) return 0;
    DRV_RED(g_name, "%s; the system's driver instead", why);
    g_name[0] = 0; g_gipa = NULL;   // (its library stays loaded, as after a failed probe)
    fell_back(why);
    return open_system();
}

const char *vk_result_str(VkResult r) {
    switch (r) {
#define VKR(x) case x: return #x;
        VKR(VK_SUCCESS) VKR(VK_INCOMPLETE) VKR(VK_ERROR_OUT_OF_HOST_MEMORY) VKR(VK_ERROR_OUT_OF_DEVICE_MEMORY) VKR(VK_ERROR_INITIALIZATION_FAILED) VKR(VK_ERROR_DEVICE_LOST)
        VKR(VK_ERROR_LAYER_NOT_PRESENT) VKR(VK_ERROR_EXTENSION_NOT_PRESENT) VKR(VK_ERROR_FEATURE_NOT_PRESENT) VKR(VK_ERROR_INCOMPATIBLE_DRIVER) VKR(VK_ERROR_TOO_MANY_OBJECTS)
        VKR(VK_ERROR_FORMAT_NOT_SUPPORTED) VKR(VK_ERROR_UNKNOWN) VKR(VK_ERROR_SURFACE_LOST_KHR) VKR(VK_ERROR_NATIVE_WINDOW_IN_USE_KHR) VKR(VK_ERROR_OUT_OF_DATE_KHR)
#undef VKR
        default: { static _Thread_local char b[24]; snprintf(b, sizeof b, "VkResult %d", (int)r); return b; }
    }
}

// The floating-point environment of the driver's compiler. The replay and its pipeline threads run on threads the game's NVN calls started, or on the game's own, so
// they carry the game's FPCR: 0x1000000 (flush-to-zero) for Ocarina of Time. Qualcomm's compiler, run on the CPU inside vkCreate*Pipelines (Samsung's in
// vkCreateShaderModule), then folds constants with denormals flushed and makes different code: Swap2-Native measured Ocarina of Time's scene about 2^12 too bright
// at the bloom pass on driver 891.11, and BananaNative sets FPCR 0 around these calls (vka_fpenv.h). So the calls that compile (device, module, pipelines, and a
// submit, where a driver may compile what it deferred) run with FPCR 0 and give the caller's back after; the game's own threads keep theirs. The pipeline cache's
// file is named apart (gfx_vk_draw.inc pcache), so pipelines compiled under flush-to-zero are not taken from it. DM_VK_FP_HOST=0 (env.txt): the caller's FPCR, as before
#if defined(__aarch64__)
static int fp_host_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_VK_FP_HOST"); v = !(e && *e == '0'); if (!v) LOG("[vk] DM_VK_FP_HOST=0: the driver compiles with the calling thread's FPCR"); } return v; }
static inline uint64_t fp_enter(void) {
    uint64_t v; __asm__ volatile("mrs %0, fpcr" : "=r"(v));
    if (v && fp_host_on()) {
        static _Atomic int told; if (!atomic_exchange(&told, 1)) LOG("[vk] the calling thread's FPCR is %#llx: the driver compiles with FPCR 0 (DM_VK_FP_HOST=0: as it is)", (unsigned long long)v);
        __asm__ volatile("msr fpcr, %0" :: "r"((uint64_t)0)); return v;
    }
    return 0;
}
static inline void fp_leave(uint64_t v) { if (v) __asm__ volatile("msr fpcr, %0" :: "r"(v)); }
#else
static inline uint64_t fp_enter(void) { return 0; }
static inline void fp_leave(uint64_t v) { (void)v; }
#endif
int vk_fp_host(void) {   // 1: compiles run with FPCR 0 (the pipeline cache's file is named for it)
#if defined(__aarch64__)
    return fp_host_on();
#else
    return 0;
#endif
}
static PFN_vkCreateDevice d_create_device; static PFN_vkCreateShaderModule d_create_module; static PFN_vkCreateGraphicsPipelines d_create_gp;
static PFN_vkCreateComputePipelines d_create_cp; static PFN_vkQueueSubmit d_submit;
static VKAPI_ATTR VkResult VKAPI_CALL fp_create_device(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *a, VkDevice *d) {
    uint64_t f = fp_enter(); VkResult r = d_create_device(pd, ci, a, d); fp_leave(f); return r; }
static VKAPI_ATTR VkResult VKAPI_CALL fp_create_module(VkDevice d, const VkShaderModuleCreateInfo *ci, const VkAllocationCallbacks *a, VkShaderModule *m) {
    uint64_t f = fp_enter(); VkResult r = d_create_module(d, ci, a, m); fp_leave(f); return r; }
static VKAPI_ATTR VkResult VKAPI_CALL fp_create_gp(VkDevice d, VkPipelineCache c, uint32_t n, const VkGraphicsPipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *p) {
    uint64_t f = fp_enter(); VkResult r = d_create_gp(d, c, n, ci, a, p); fp_leave(f); return r; }
static VKAPI_ATTR VkResult VKAPI_CALL fp_create_cp(VkDevice d, VkPipelineCache c, uint32_t n, const VkComputePipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *p) {
    uint64_t f = fp_enter(); VkResult r = d_create_cp(d, c, n, ci, a, p); fp_leave(f); return r; }
static VKAPI_ATTR VkResult VKAPI_CALL fp_submit(VkQueue q, uint32_t n, const VkSubmitInfo *si, VkFence fe) {
    uint64_t f = fp_enter(); VkResult r = d_submit(q, n, si, fe); fp_leave(f); return r; }

void vk_driver_instance(VkInstance inst) {
#define VKB_LOAD(f) f = (PFN_##f)g_gipa(inst, #f);
    VKB_FNS(VKB_LOAD)
    if (vkCreateDevice) { d_create_device = vkCreateDevice; vkCreateDevice = fp_create_device; }   // through the FPCR guard (above)
    if (vkCreateShaderModule) { d_create_module = vkCreateShaderModule; vkCreateShaderModule = fp_create_module; }
    if (vkCreateGraphicsPipelines) { d_create_gp = vkCreateGraphicsPipelines; vkCreateGraphicsPipelines = fp_create_gp; }
    if (vkCreateComputePipelines) { d_create_cp = vkCreateComputePipelines; vkCreateComputePipelines = fp_create_cp; }
    if (vkQueueSubmit) { d_submit = vkQueueSubmit; vkQueueSubmit = fp_submit; }
}
