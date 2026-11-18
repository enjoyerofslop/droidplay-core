// The app's version beside this game's host's, for the line front_android.c logs at start. The launcher, the loader and the Java side come with every app update; a
// game's host comes only with an update that built it again (scripts/build_android_from_nsp.py refresh_hosts: Update games), so the log says whether the game runs the
// app's runtime. One build packs both from the same version: build_apk.py's --version-name (build_android_from_nsp.py core_version) and the host's build stamp
// (tools/build_stamp.py: "<commit>[+dirty] (v<version>)") both take the checkout's git describe, or a release's VERSION file. Plain libc, for
// dm/tools/tests/test_app_version.py.
#pragma once
#include <string.h>

// 1: the host's stamp names the app's version; 0: another version; -1: one of them names none (an app packed without a version keeps the manifest's 0.1, and a host
// built outside a checkout or a release says "not recorded")
static int host_matches_app(const char *stamp, const char *app) {
    if (!stamp || !app || !*app || !strcmp(app, "0.1")) return -1;
    const char *o = strrchr(stamp, '('), *c = o ? strchr(o, ')') : NULL;
    if (!o || !c) return -1;
    o++; if (*o == 'v') o++;   // a release's tag, v0.8.13; the app's version is the same without the v
    if (*app == 'v') app++;
    size_t n = (size_t)(c - o);
    return n == strlen(app) && !strncmp(o, app, n) ? 1 : 0;
}
