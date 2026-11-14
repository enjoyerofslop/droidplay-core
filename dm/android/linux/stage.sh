#!/usr/bin/env bash
# Builds DroidPlay for Linux as an AppImage: the recipe of DroidPlay for Windows (dm/android/windows/stage.ps1), on Linux. From a checkout, on x86_64 Linux
# with uv (astral.sh), git, curl, sha256sum and readelf:
#   dm/android/linux/stage.sh --out <folder> [--version <x.y.z>] [--python 3.14] [--cache <folder>] [--no-launch] [--no-pack]
# It makes, in <folder>:
#   AppDir/                   what the AppImage holds:
#     AppRun                    its entry: unpack.py on the AppImage's own Python copies payload/ into ~/.local/share/DroidPlay/<version>/ on a version's
#                               first start, then starts DroidPlay from there (unpack.py says why it doesn't run from the AppImage's mount)
#     payload/runtime           Python (python-build-standalone, the build uv installs, as on Windows) with the modules scripts/nxnative_android.py.lock pins,
#                               precompiled. A real Python, not a frozen app: the packing script starts Python children everywhere (sys.executable scripts,
#                               process pools, CMake's FindPython). PySide6 is cut to what the app uses, with the X11 libraries Qt's xcb plugin needs and a
#                               fresh desktop may lack (xcb_libs.tsv)
#     payload/core              the tree of HEAD without docs/ and .github/, as a release's source archives have it, with VERSION, and DejaVu Sans in
#                               dm/android/fonts for the build's fonts on a system without DejaVu or Liberation
#     payload/licenses          the licences of all of it, and NOTICES.md naming each part (also usr/share/doc/droidplay/licenses)
#     io.github.enjoyerofslop.droidplay.desktop, .png (drawn by the app's --write-icon), .DirIcon, usr/share/metainfo/…appdata.xml
#   DroidPlay-<version>-x86_64.AppImage, and its .sha256: packed by appimagetool with AppImage's static type-2 runtime, both pinned by digest. That runtime
#                               mounts with the kernel's FUSE and fusermount3 and needs no libfuse2, which Ubuntu 22.04 and later don't install
# and checks it: --check's Python rows, the bundled fonts, every ELF file's needs (glibc 2.34 and GCC 11's libstdc++ at most, the floor PySide6 6.12 sets
# and the README states; each library either in the AppImage or one DroidPlay takes from the system), and the window opened offscreen for 10 s, with its
# settings and data in a folder of this stage rather than this user's (--no-launch leaves it out). --no-pack stops at the AppDir.
set -euo pipefail
ID=io.github.enjoyerofslop.droidplay
# Pinned like Inno Setup in windows-package.yml: a release's file and the digest GitHub lists for it
APPIMAGETOOL=https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage
APPIMAGETOOL_SHA=ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0
RUNTIME=https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64
RUNTIME_SHA=2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d
DEJAVU=https://github.com/dejavu-fonts/dejavu-fonts/releases/download/version_2_37/dejavu-fonts-ttf-2.37.tar.bz2
DEJAVU_SHA=fa9ca4d13871dd122f61258a80d01751d603b4d3ee14095d65453b4e846e17d7
GLIBC_MAX=2.34 GLIBCXX_MAX=3.4.29

say() { printf 'stage: %s\n' "$*"; }
die() { printf 'stage: %s\n' "$*" >&2; exit 1; }
fetch() {   # url, file, sha256: the file, downloaded unless it is there with that digest
    if [[ -f $2 ]] && sha256sum "$2" | grep -q "^$3 "; then return; fi
    curl -fsSL --retry 3 -o "$2.part" "$1" || die "could not download $1"
    sha256sum "$2.part" | grep -q "^$3 " || die "$1 does not match its digest"
    mv "$2.part" "$2"
}

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd); repo=$(cd "$here/../../.." && pwd)
out='' version='' python=3.14 cache='' launch=1 pack=1
while (($#)); do
    case $1 in
        --out) out=$2; shift 2 ;; --version) version=$2; shift 2 ;; --python) python=$2; shift 2 ;; --cache) cache=$2; shift 2 ;;
        --no-launch) launch=0; shift ;; --no-pack) pack=0; shift ;;
        -h|--help) sed -n '2,25p' "$0"; exit 0 ;;
        *) die "unknown option $1 (--help lists them)" ;;
    esac
done
[[ $out ]] || die '--out <folder> is needed'
[[ $(uname -s) == Linux && $(uname -m) == x86_64 ]] || die 'x86_64 Linux only: Google publishes the Android SDK for Linux on x86_64 alone'
uv=$(command -v uv || echo "$HOME/.local/bin/uv"); [[ -x $uv ]] || die 'uv was not found: install it (https://docs.astral.sh/uv/) or put it on PATH'
for t in git curl sha256sum readelf; do command -v $t > /dev/null || die "$t is needed"; done

rm -rf "$out"; mkdir -p "$out"; out=$(cd "$out" && pwd)
app=$out/AppDir; work=$out/.work; pay=$app/payload; lic=$pay/licenses
cache=${cache:-$work/downloads}; mkdir -p "$app" "$work" "$pay" "$lic" "$cache"; cache=$(cd "$cache" && pwd)

# ---- core: the tree as a release ships it
say 'core: the tree of HEAD without docs and .github'
mkdir -p "$pay/core"
git -C "$repo" archive --format=tar HEAD ':!docs' ':!.github' | tar -x -C "$pay/core"
[[ $version ]] || version=$(git -C "$repo" describe --tags --always)
version=${version#v}   # a release's tag, v0.9.0, is version 0.9.0
[[ $version =~ ^[0-9A-Za-z][0-9A-Za-z._+-]*$ ]] || die "version $version can't name a folder"   # unpack.py makes ~/.local/share/DroidPlay/<version>
printf '%s\n' "$version" > "$pay/core/VERSION"   # build_android_from_nsp.py core_version reads it

# ---- runtime: uv's Python, installed into a folder of this stage (not the user's), then copied whole: it is relocatable
say "runtime: Python $python"
export UV_PYTHON_INSTALL_DIR=$work/pythons UV_PYTHON_BIN_DIR=$work/bin UV_PYTHON_PREFERENCE=only-managed
"$uv" python install --quiet "$python"
mapfile -t found < <(find "$work/pythons" -mindepth 1 -maxdepth 1 -type d -regex '.*/cpython-[0-9]+\.[0-9]+\.[0-9]+-linux-x86_64-gnu')
((${#found[@]} == 1)) || die "uv python install left ${#found[@]} Linux Pythons in $work/pythons"
rt=$pay/runtime
cp -a "${found[0]}" "$rt"
find "$rt" -name EXTERNALLY-MANAGED -delete   # a private Python: pip may add a module to it
py=$rt/bin/python3; [[ -x $py ]] || die 'the runtime has no bin/python3'
pyver=$("$py" -c 'import sys; print(*sys.version_info[:3], sep=".")'); site=$rt/lib/python${pyver%.*}/site-packages

say 'runtime: the modules the lock pins'
"$uv" export --quiet --script "$repo/scripts/nxnative_android.py" --format requirements.txt --no-header -o "$work/requirements.txt"
"$uv" pip install --quiet --python "$py" --break-system-packages --no-cache -r "$work/requirements.txt"

# ---- what DroidPlay doesn't use, out: about two thirds of PySide6-Essentials (Qt Quick and QML, Designer, Qt's tools, Network, SQL), pip, Tk and IDLE, the C
# headers, the launchers pip left in bin/ (they name this stage's Python), and libpython (33 MB), which only a program embedding Python loads: this build's
# python3 has the interpreter linked in, and extension modules on Linux don't link it. The app imports QtCore, QtGui, QtWidgets and QtSvg alone: keep this
# list in step with its imports and with stage.ps1's. The window checks (here offscreen; in linux-package.yml under X11 and Wayland too) fail on a piece that
# went and turns out to be needed
say 'runtime: what DroidPlay does not use, out'
find "$rt/bin" -mindepth 1 ! -name 'python' ! -name 'python3' ! -name "python${pyver%.*}" -exec rm -rf {} +
rm -rf "$rt/include" "$rt/share/man" "$rt/lib/pkgconfig" "$rt"/lib/libpython3*.so* "$rt"/lib/libtcl* "$rt"/lib/tcl[0-9]* "$rt"/lib/tk[0-9]* "$rt"/lib/itcl* "$rt"/lib/thread[0-9]* \
       "$rt/lib/python${pyver%.*}"/{idlelib,tkinter,turtledemo,ensurepip,turtle.py} "$rt/lib/python${pyver%.*}"/lib-dynload/_tkinter* "$site"/pip "$site"/pip-*
ps6=$site/PySide6
keep_top=(__init__.py _config.py _git_pyside_version.py py.typed PySide6_Essentials.json support __pycache__ Qt
          QtCore.abi3.so QtGui.abi3.so QtWidgets.abi3.so QtSvg.abi3.so 'libpyside6.abi3.so.*')
keep_lib=(libQt6Core.so.6 libQt6Gui.so.6 libQt6Widgets.so.6 libQt6Svg.so.6 libQt6DBus.so.6 'libicu*.so.*'
          libQt6XcbQpa.so.6 libQt6WaylandClient.so.6 libQt6OpenGL.so.6)   # the last three: the X11 and Wayland plugins and Wayland's EGL integration
# X11, Wayland (with the shell, title bar and EGL plugins: GNOME draws no title bars for apps) and offscreen; image formats (the games' art is JPEG, the
# icons SVG); the desktop's own file dialogs (portal, GTK); input methods and dead keys
keep_plugins=(platforms/libqxcb.so platforms/libqwayland.so platforms/libqoffscreen.so 'xcbglintegrations/*'
              wayland-shell-integration/libxdg-shell.so wayland-decoration-client/libadwaita.so wayland-decoration-client/libbradient.so
              wayland-graphics-integration-client/libqt-plugin-wayland-egl.so
              imageformats/libqjpeg.so imageformats/libqsvg.so imageformats/libqico.so imageformats/libqgif.so iconengines/libqsvgicon.so
              platformthemes/libqxdgdesktopportal.so platformthemes/libqgtk3.so
              platforminputcontexts/libcomposeplatforminputcontextplugin.so platforminputcontexts/libibusplatforminputcontextplugin.so)
# shellcheck disable=SC2053   # the patterns are unquoted on purpose: they are globs
kept() { local n=$1 k; shift; for k in "$@"; do [[ $n == $k ]] && return 0; done; return 1; }
[[ -d $ps6/Qt/lib && -d $ps6/Qt/plugins ]] || die "PySide6 isn't laid out as this script expects: $ps6"
for f in "$ps6"/*; do kept "${f##*/}" "${keep_top[@]}" || rm -rf "$f"; done
for f in "$ps6"/Qt/*; do kept "${f##*/}" lib plugins || rm -rf "$f"; done
for f in "$ps6"/Qt/lib/*; do kept "${f##*/}" "${keep_lib[@]}" || rm -rf "$f"; done
while IFS= read -r f; do kept "${f#"$ps6/Qt/plugins/"}" "${keep_plugins[@]}" || rm -f "$f"; done < <(find "$ps6/Qt/plugins" -type f)
find "$ps6/Qt/plugins" -mindepth 1 -type d -empty -delete

say "runtime: the X11 libraries Qt's xcb plugin needs (xcb_libs.tsv, from Ubuntu 22.04)"
"$py" -I "$here/stage_tools.py" debs "$here/xcb_libs.tsv" "$cache" "$ps6/Qt/lib" "$lic/xcb"
"$py" -m compileall -q -j 0 --invalidation-mode unchecked-hash "$rt/lib/python${pyver%.*}"   # no source check at each start
unset UV_PYTHON_INSTALL_DIR UV_PYTHON_BIN_DIR UV_PYTHON_PREFERENCE

# ---- DejaVu Sans, for the build's standard and button fonts where the system has neither DejaVu nor Liberation (build_android_from_nsp.py BUNDLED_FONTS)
say 'core: DejaVu Sans 2.37'
fetch "$DEJAVU" "$cache/dejavu-fonts-ttf-2.37.tar.bz2" "$DEJAVU_SHA"
mkdir -p "$pay/core/dm/android/fonts" "$work/dejavu"
tar -xjf "$cache/dejavu-fonts-ttf-2.37.tar.bz2" -C "$work/dejavu" dejavu-fonts-ttf-2.37/ttf/DejaVuSans.ttf dejavu-fonts-ttf-2.37/ttf/DejaVuSans-Bold.ttf dejavu-fonts-ttf-2.37/LICENSE
cp "$work"/dejavu/dejavu-fonts-ttf-2.37/ttf/DejaVuSans{,-Bold}.ttf "$pay/core/dm/android/fonts/"
cp "$work/dejavu/dejavu-fonts-ttf-2.37/LICENSE" "$lic/DejaVu-LICENSE.txt"

# ---- licences: each part's own files, and NOTICES.md naming the parts and their versions
say 'licences'
cp "$repo/LICENSE" "$lic/DroidPlay-Core-LICENSE.txt"
cp "$rt/lib/python${pyver%.*}/LICENSE.txt" "$lic/Python-LICENSE.txt"
cp "$repo"/dm/android/licenses/*.txt "$lic/"
"$py" -I "$here/stage_tools.py" pbs-licenses "$rt" "$cache" "$lic/python-build-standalone"
modules=''
for d in "$site"/*.dist-info; do
    name=$(sed -n 's/^Name: //p' "$d/METADATA" | head -1); ver=$(sed -n 's/^Version: //p' "$d/METADATA" | head -1)
    lf=$(sed -n 's/^License-Expression: //p;s/^License: //p' "$d/METADATA" | head -1)
    mkdir -p "$lic/python-modules/$name"
    if [[ -d $d/licenses ]]; then cp -r "$d/licenses/." "$lic/python-modules/$name/"; else find "$d" -maxdepth 1 -iname '*licen[cs]e*' -exec cp {} "$lic/python-modules/$name/" \;; fi
    modules+="- $name $ver${lf:+ ($lf)}"$'\n'
done
read -r openssl sqlite < <("$py" -c 'import ssl, sqlite3; print(ssl.OPENSSL_VERSION.split()[1], sqlite3.sqlite_version)')
pyside=$("$py" -c 'import importlib.metadata as m; print(m.version("PySide6-Essentials"))')
qt=$(QT_QPA_PLATFORM=offscreen "$py" -c 'from PySide6.QtCore import qVersion; print(qVersion())')
VERSION=$version PYTHON_VERSION=$pyver PBS_BUILD=$(cat "$rt/BUILD") OPENSSL_VERSION=$openssl SQLITE_VERSION=$sqlite QT_VERSION=$qt PYSIDE_VERSION=$pyside \
MODULES=$modules "$py" -I -c '
import os, re, sys
t = open(sys.argv[1], encoding="utf-8").read()
open(sys.argv[2], "w", encoding="utf-8").write(re.sub(r"@([A-Z_]+)@", lambda m: os.environ[m.group(1)].rstrip(), t))' "$here/NOTICES.md" "$lic/NOTICES.md"

# ---- the AppDir's top: the entry, the desktop entry, the icon (drawn offscreen by the app itself), AppStream metadata. The desktop entry and the icon live
# under usr/share as on an installed system, where AppStream's validator and AppImage managers look, with the links at the top that AppImage asks for
say 'AppDir: AppRun, desktop entry, icon, metadata'
install -m 755 "$here/AppRun" "$app/AppRun"
icons=usr/share/icons/hicolor/256x256/apps
mkdir -p "$app/usr/share/applications" "$app/$icons" "$app/usr/share/metainfo" "$app/usr/share/doc/droidplay"
cp "$here/$ID.desktop" "$app/usr/share/applications/"; ln -s "usr/share/applications/$ID.desktop" "$app/$ID.desktop"
"$py" -c 'import PySide6.QtGui, PySide6.QtSvg, PySide6.QtWidgets' || die "the runtime's PySide6 doesn't load (above: what this system lacks)"
QT_QPA_PLATFORM=offscreen "$py" "$pay/core/scripts/nxnative_android.py" --write-icon "$app/$icons/$ID.png"
[[ -s $app/$icons/$ID.png ]] || die 'the app wrote no icon'
ln -s "$icons/$ID.png" "$app/$ID.png"; ln -s "$icons/$ID.png" "$app/.DirIcon"
sed -e "s/@VERSION@/$version/" -e "s/@DATE@/$(date -u +%Y-%m-%d)/" "$here/$ID.appdata.xml" > "$app/usr/share/metainfo/$ID.appdata.xml"
ln -s ../../../../payload/licenses "$app/usr/share/doc/droidplay/licenses"

# ---- the checks
say "checks: --check's Python rows"
json=$("$py" "$pay/core/scripts/build_android_from_nsp.py" --check) || die "--check failed"
"$py" -I -c '
import json, sys
rows = [r for r in json.loads(sys.argv[1]) if r["name"].startswith("Python")]
bad = [r["name"] for r in rows if not r["ok"]]
if not rows or bad: sys.exit("the runtime lacks: " + (", ".join(bad) or "every Python row"))
print("stage:   " + ", ".join(r["name"].removeprefix("Python module ").removeprefix("Python ") for r in rows) + ": found")' "$json"

say 'checks: the bundled fonts, as the build finds them'
"$py" -c '
import sys; sys.path.insert(0, sys.argv[1] + "/scripts")
import build_android_from_nsp as ba
for f in ("DejaVuSans.ttf", "DejaVuSans-Bold.ttf"):
    p = ba.BUNDLED_FONTS / f
    if not p.is_file() or str(p) not in ba.STANDARD_FONTS + ba.LETTER_FONTS: sys.exit(f"{p} is not where the build looks")
if not ba.hinted(ba.BUNDLED_FONTS / "DejaVuSans.ttf"): sys.exit("DejaVu Sans has no TrueType hinting: it cannot be the standard font")' "$pay/core"

say "checks: ELF files need glibc $GLIBC_MAX and GLIBCXX $GLIBCXX_MAX at most, and only libraries in the AppImage or the system's"
"$py" -I "$here/stage_tools.py" elf-check "$pay" "$GLIBC_MAX" "$GLIBCXX_MAX"

if ((launch)); then
    say 'checks: the window, offscreen, for 10 s'
    mkdir -p "$work/home/config" "$work/home/data"
    # its settings and data in this stage's folder: the window reads and writes DroidPlay's settings, and --check lists the hosts library
    XDG_CONFIG_HOME=$work/home/config XDG_DATA_HOME=$work/home/data QT_QPA_PLATFORM=offscreen PYTHONNOUSERSITE=1 \
        "$py" "$pay/core/scripts/nxnative_android.py" > "$work/launch.log" 2>&1 &
    pid=$!; sleep 10
    kill -0 $pid 2> /dev/null || { cat "$work/launch.log"; die 'the window closed by itself'; }
    kill $pid; wait $pid 2> /dev/null || true
fi

size() { du -sm "$1" | cut -f1; }
say "runtime $(size "$rt") MB, core $(size "$pay/core") MB, AppDir $(size "$app") MB ($(find "$app" -type f | wc -l) files)"
if ((pack)); then
    say 'pack: appimagetool with the static type-2 runtime, zstd'
    fetch "$APPIMAGETOOL" "$cache/appimagetool-1.9.1-x86_64.AppImage" "$APPIMAGETOOL_SHA"
    fetch "$RUNTIME" "$cache/type2-runtime-20251108-x86_64" "$RUNTIME_SHA"
    chmod +x "$cache/appimagetool-1.9.1-x86_64.AppImage"
    image=$out/DroidPlay-$version-x86_64.AppImage
    # extract-and-run: appimagetool is an AppImage itself, and a build machine (a container, CI) may have no FUSE
    ARCH=x86_64 APPIMAGE_EXTRACT_AND_RUN=1 "$cache/appimagetool-1.9.1-x86_64.AppImage" --comp zstd --runtime-file "$cache/type2-runtime-20251108-x86_64" \
        "$app" "$image" > "$work/appimagetool.log" 2>&1 || { cat "$work/appimagetool.log"; die 'appimagetool failed'; }
    (cd "$out" && sha256sum "${image##*/}" > "${image##*/}.sha256")
    say "$(basename "$image"): $(( $(stat -c %s "$image") / 1048576 )) MB"
fi
rm -rf "$work"
say "DroidPlay $version staged in $out"
