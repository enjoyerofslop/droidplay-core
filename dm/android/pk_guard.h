// The start-up list's guard against a pipeline whose compile never finishes. The list (<DM_PIPELINE_CACHE>.keys4, gfx_vk_draw.inc prewarm_thread) makes the pipelines of
// earlier sessions again at every start, so an entry the driver's shader compiler crashes on kills every start a few seconds in, and nothing a player can do clears it.
// Before each compile the prewarm writes the entry to a marker file (<cache>.making4) and empties the file after; a crash in that compile also flags it (pb_dump, the crash
// dump's graphics hook). An entry still in the marker at the next start goes to the skip list (<cache>.skip4), which the prewarm leaves out from then on.
// Clearing one entry per crash is slow when a list holds many bad ones: on a OnePlus Pad 3, Ocarina of Time's held 32, each a start's crash. So the starts in a row that crashed inside
// the list are counted (<cache>.strikes4), and at the second the whole list is set aside (<cache>.keys4.aside) and the session starts from an empty one.
// Plain POSIX file calls on whole records of the caller's size, for dm/tools/tests/test_pk_guard.py.
#pragma once
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { PKG_BUSY = '0', PKG_CRASHED = 'C' };   // the byte after the record: being compiled; the crash hook's flag

// The marker of this session, emptied: its descriptor, or -1
static int pkg_open(const char *path) { return open(path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600); }

// An entry about to be compiled: its record, then the busy byte (a record without that byte, written by a start that ended between the two, counts as busy)
static void pkg_begin(int fd, const uint8_t *rec, size_t n) {
    if (fd < 0) return;
    char b = PKG_BUSY; if (pwrite(fd, rec, n, 0) == (ssize_t)n) (void)!pwrite(fd, &b, 1, (off_t)n);
}

// Its compile returned
static void pkg_end(int fd) { if (fd >= 0) (void)!ftruncate(fd, 0); }

// From the crash handler, on the compiling thread: one write, which is async-signal-safe
static void pkg_crashed(int fd, size_t n) { if (fd >= 0) { char b = PKG_CRASHED; (void)!pwrite(fd, &b, 1, (off_t)n); } }

// What an earlier start left in the marker: 1 with the record in rec (and *crashed when the crash hook flagged it), 0 when it was empty or held less than a record.
// The marker is removed either way, so one entry is taken once
static int pkg_take(const char *path, uint8_t *rec, size_t n, int *crashed) {
    *crashed = 0; int fd = open(path, O_RDONLY | O_CLOEXEC); if (fd < 0) return 0;
    char b = 0; ssize_t got = pread(fd, rec, n, 0); int whole = got == (ssize_t)n;
    if (whole && pread(fd, &b, 1, (off_t)n) == 1) *crashed = b == PKG_CRASHED;
    close(fd); unlink(path);
    return whole;
}

// The skip list's records (whole ones only), in a buffer the caller frees: their count
static size_t pkg_skip_load(const char *path, size_t n, uint8_t **out) {
    *out = NULL; int fd = open(path, O_RDONLY | O_CLOEXEC); if (fd < 0) return 0;
    off_t len = lseek(fd, 0, SEEK_END); size_t cnt = len > 0 ? (size_t)len / n : 0;
    if (cnt) { *out = malloc(cnt * n); if (!*out || pread(fd, *out, cnt * n, 0) != (ssize_t)(cnt * n)) { free(*out); *out = NULL; cnt = 0; } }
    close(fd); return cnt;
}

// A record added to the skip list unless it is there already: 1 when added. It goes after the last whole record, so a part record a start left at the end is overwritten
static int pkg_skip_add(const char *path, const uint8_t *rec, size_t n) {
    uint8_t *all; size_t cnt = pkg_skip_load(path, n, &all);
    for (size_t i = 0; i < cnt; i++) if (!memcmp(all + i * n, rec, n)) { free(all); return 0; }
    free(all);
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600); if (fd < 0) return 0;
    off_t at = (off_t)(cnt * n); int ok = ftruncate(fd, at) == 0 && pwrite(fd, rec, n, at) == (ssize_t)n; close(fd); return ok;
}

// The starts in a row that crashed inside the list, this one's outcome counted: a crash (the marker flagged) adds one, anything else starts the count again. One byte in path
static int pkg_strikes(const char *path, int crashed) {
    unsigned char n = 0; int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600); if (fd < 0) return crashed;
    if (crashed) { if (pread(fd, &n, 1, 0) != 1) n = 0; if (n < 255) n++; }
    (void)!pwrite(fd, &n, 1, 0); close(fd); return n;
}

// The list set aside under another name (an earlier one there is replaced), so the session starts from an empty list: 1 when it was moved
static int pkg_set_aside(const char *list, const char *aside) { return rename(list, aside) == 0; }

// ---- The play guard: pipelines built during play ----
// A pipeline a game needs during play goes through the same compiler, and one it crashes on ends every start at the same scene: on a Retroid Pocket G2 (Adreno 735, driver
// V@0762) Qualcomm's compiler crashed building Ocarina of Time's first compute pipeline (a039399e6336de1b) as the first 3D scene began, at every start (droidplay-core#512).
// Nothing is written while builds run: only the crash hook writes, on the crashing thread, one record of the build it was in (pgd_note: one pwrite, async-signal-safe) to the
// marker <cache>.pcrash1. The next start moves it to the skip list <cache>.pskip1, and pipelines of those shaders are not built from then on (their draws or dispatches are
// skipped, with a red line). A kill or a hang inside a build writes nothing: lmkd kills at a scene's load while several threads build, and none of those builds is to blame.
// A record keeps the driver's version and the host's build, and records of another are not used: an updated driver may compile the shader, and a new host may hand it other
// code (the SPIR-V rewrites made at load), so each update gives it one more try rather than leaving a fixed shader out for good.
typedef struct { char magic[4]; char kind; uint8_t z; uint8_t pad[2]; uint32_t drv, host; char sh[4][17]; uint8_t pad2[12]; } pgd_rec;   // kind: 'g' graphics, 'c' compute, 'k' a task / mesh kernel in compute form; z: the vertex .z variant (said, not matched)

// A host build's number in the records: FNV-1a of its commit line
static uint32_t pgd_host(const char *build) { uint32_t h = 2166136261u; for (; build && *build; build++) h = (h ^ (uint8_t)*build) * 16777619u; return h; }

// The record of a build of these shaders (keys of up to 16 characters; NULL for an empty slot). Plain stores only, as the crash hook makes it
static void pgd_make(pgd_rec *r, char kind, uint32_t drv, uint32_t host, const char *const sh[4], int z) {
    memset(r, 0, sizeof *r); memcpy(r->magic, "PGD1", 4); r->kind = kind; r->drv = drv; r->host = host; r->z = (uint8_t)(z != 0);
    for (int i = 0; i < 4; i++) if (sh[i]) memcpy(r->sh[i], sh[i], strnlen(sh[i], 16));
}

// From the crash handler, on the crashing thread
static void pgd_note(int fd, const pgd_rec *r) { if (fd >= 0) (void)!pwrite(fd, r, sizeof *r, 0); }

// What the last start's crash left in the marker: 1 with the record in r. The marker is removed either way, so a record is taken once
static int pgd_take(const char *path, pgd_rec *r) {
    int fd = open(path, O_RDONLY | O_CLOEXEC); if (fd < 0) return 0;
    ssize_t got = pread(fd, r, sizeof *r, 0); close(fd); unlink(path);
    return got == (ssize_t)sizeof *r && !memcmp(r->magic, "PGD1", 4);
}

// The skip list's records made under driver version drv and host build host, in a buffer the caller frees: their count. The file keeps only those
static size_t pgd_skip_load(const char *path, uint32_t drv, uint32_t host, pgd_rec **out) {
    uint8_t *all; size_t n = pkg_skip_load(path, sizeof(pgd_rec), &all), k = 0;
    *out = (pgd_rec *)all;
    for (size_t i = 0; i < n; i++) { pgd_rec r; memcpy(&r, all + i * sizeof r, sizeof r); if (!memcmp(r.magic, "PGD1", 4) && r.drv == drv && r.host == host) memcpy(all + k++ * sizeof r, &r, sizeof r); }
    if (k < n) {   // records of other drivers or host builds are dropped from the file, so it does not grow with every update
        int fd = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC); if (fd >= 0) { (void)!write(fd, all, k * sizeof(pgd_rec)); close(fd); } }
    if (!k) { free(all); *out = NULL; }
    return k;
}
