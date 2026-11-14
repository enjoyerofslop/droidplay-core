#!/usr/bin/env bash
# DroidPlay's AppImage checked on one Linux distribution, inside its container (linux-package.yml runs it in each): the system libraries a desktop has (the
# README's list, plus Xvfb, weston and a font) and nothing else of DroidPlay's, then the AppImage started by an ordinary user as a player starts it. A container
# has no FUSE, so the AppImage extracts itself and runs (APPIMAGE_EXTRACT_AND_RUN, what a system without FUSE does too). It checks:
#   - the first start, under X11 (Xvfb): the version unpacked into ~/.local/share/DroidPlay/<version>/ behind its progress window, then the window open 10 s;
#   - a second start, offscreen: the unpacked version reused as it was (not copied again), the window open 10 s;
#   - --check's Python rows, and an HTTPS request with the bundled Python, which must find this distribution's CA certificates;
#   - the window under X11 (Xvfb), Wayland (weston's headless backend) and offscreen, with window_check.py: the platform plugin that drew it, where its Qt
#     and X11 libraries came from, Get started's checks answered, and a picture of it.
# No phone, game or keys. Logs and pictures go to <out folder>.
#   bash smoke_test.sh <AppImage> <out folder>          (as root in a fresh container: it installs packages and adds a user)
set -euo pipefail
# shellcheck source=/dev/null
. /etc/os-release
say() { printf 'smoke %s: %s\n' "$ID" "$*"; }
fail() { say "FAILED: $*"; exit 1; }

if [[ ${1:-} != --player ]]; then
    img=$(readlink -f "$1"); out=$(readlink -f "$2"); mkdir -p "$out"; chmod 777 "$out"
    say 'the libraries a desktop has'
    case $ID in
        ubuntu|debian)
            export DEBIAN_FRONTEND=noninteractive; apt-get update -qq
            apt-get install -y -qq --no-install-recommends libgl1 libegl1 libx11-6 libx11-xcb1 libxcb1 libfontconfig1 libfreetype6 libglib2.0-0 libdbus-1-3 \
                libwayland-client0 libwayland-cursor0 libwayland-egl1 ca-certificates fonts-dejavu-core xvfb weston procps > /dev/null ;;
        fedora)
            dnf install -y -q mesa-libGL mesa-libEGL libX11 libX11-xcb libxcb fontconfig freetype glib2 dbus-libs libwayland-client libwayland-cursor \
                libwayland-egl ca-certificates dejavu-sans-fonts xorg-x11-server-Xvfb weston procps-ng util-linux > /dev/null ;;
        arch)
            pacman -Syu --noconfirm --needed libglvnd mesa libx11 libxcb fontconfig freetype2 glib2 dbus wayland ca-certificates ttf-dejavu xorg-server-xvfb \
                weston procps-ng > /dev/null ;;
        opensuse-tumbleweed)
            # openSUSE packs GLib's libgthread on its own (other distributions keep it in GLib's package); its desktops have it
            zypper -n -q in Mesa-libGL1 Mesa-libEGL1 libX11-6 libX11-xcb1 libxcb1 fontconfig libfreetype6 libglib-2_0-0 libgthread-2_0-0 libdbus-1-3 \
                libwayland-client0 libwayland-cursor0 libwayland-egl1 ca-certificates dejavu-fonts xorg-x11-server-Xvfb weston procps > /dev/null ;;
        *) fail "no package list for $ID" ;;
    esac
    # what the AppImage brings itself and a fresh install may lack: shown as the distribution has it, absent or not
    say "the system's own xcb-util and xkbcommon-x11: $( (ldconfig -p 2> /dev/null || true) | grep -oE 'lib(xcb-cursor|xcb-icccm|xkbcommon-x11)\.so\.[0-9]+' | sort -u | tr '\n' ' ')"
    mkdir -p /tmp/.X11-unix; chmod 1777 /tmp/.X11-unix   # Xvfb, not run as root, can't make it (a desktop's display manager has)
    useradd -m player
    cp "$img" /home/player/DroidPlay.AppImage; chmod 755 /home/player/DroidPlay.AppImage; chown player: /home/player/DroidPlay.AppImage
    exec runuser -u player -- env -i HOME=/home/player USER=player PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 bash "$0" --player "$out" "$ID"
fi

out=$2; cd "$HOME"; app=./DroidPlay.AppImage
export APPIMAGE_EXTRACT_AND_RUN=1 XDG_RUNTIME_DIR=$HOME/.runtime; mkdir -p "$XDG_RUNTIME_DIR"; chmod 700 "$XDG_RUNTIME_DIR"
pids=()
cleanup() { for p in "${pids[@]}"; do kill -- -"$p" 2> /dev/null || true; done; }
trap cleanup EXIT
start() {   # name, then the environment and command: started in a session of its own, so its whole tree can be ended (stop)
    local name=$1; shift
    setsid env "$@" > "$out/$name.log" 2>&1 &
    last=$!; pids+=("$last")
}
stop() { kill -- -"$1" 2> /dev/null || true; sleep 1; kill -9 -- -"$1" 2> /dev/null || true; }
alive_for() {   # pid, seconds, what: still running after that long, else the log says why
    sleep "$2"; kill -0 "$1" 2> /dev/null || { cat "$out/$3.log"; fail "$3: DroidPlay closed by itself"; }
}

say 'X11 (Xvfb) and Wayland (weston, headless)'
Xvfb :99 -screen 0 1600x1000x24 -nolisten tcp > "$out/xvfb.log" 2>&1 & pids+=("$!")
weston_up() { for _ in $(seq 50); do [[ -S $XDG_RUNTIME_DIR/wayland-dp ]] && return 0; sleep 0.2; done; return 1; }
for backend in headless headless-backend.so; do   # weston 10 and later name it headless; 9 (Ubuntu 22.04) headless-backend.so
    setsid weston --backend=$backend --socket=wayland-dp --idle-time=0 --width=1600 --height=1000 > "$out/weston.log" 2>&1 & wpid=$!
    if weston_up; then pids+=("$wpid"); break; fi
    kill -- -$wpid 2> /dev/null || true
done
[[ -S $XDG_RUNTIME_DIR/wayland-dp ]] || { cat "$out/weston.log"; fail 'weston did not start'; }
for _ in $(seq 50); do [[ -S /tmp/.X11-unix/X99 ]] && break; sleep 0.2; done

say 'first start, under X11: the version unpacks, then the window'
t0=$(date +%s)
start first DISPLAY=:99 "$app"
for _ in $(seq 600); do compgen -G "$HOME/.local/share/DroidPlay/*/.complete" > /dev/null && break; sleep 0.5; done
vers=()   # a container may have no find (openSUSE's has none)
for d in "$HOME"/.local/share/DroidPlay/*/; do [[ $d == *.partial/ ]] || vers+=("${d%/}"); done
((${#vers[@]} == 1)) && [[ -f ${vers[0]}/.complete ]] || { cat "$out/first.log"; fail "no unpacked version after the first start (${vers[*]:-none})"; }
dir=${vers[0]}; say "  unpacked ${dir#"$HOME"/} ($(du -sm "$dir" | cut -f1) MB) in $(($(date +%s) - t0)) s"
alive_for "$last" 10 first; stop "$last"
grep -q 'preparing DroidPlay' "$out/first.log" || fail 'the first start said nothing of preparing the version'

say 'second start, offscreen: the unpacked version reused'
before=$(stat -c %Y "$dir/.complete"); touch "$dir/core/.smoke-left-here"
start second QT_QPA_PLATFORM=offscreen "$app"
alive_for "$last" 10 second; stop "$last"
[[ $(stat -c %Y "$dir/.complete") == "$before" && -f $dir/core/.smoke-left-here ]] || fail 'the second start copied the version again'
grep -q 'preparing DroidPlay' "$out/second.log" && fail 'the second start prepared the version again'
rm "$dir/core/.smoke-left-here"

py=$dir/runtime/bin/python3
say "--check's Python rows"
"$py" "$dir/core/scripts/build_android_from_nsp.py" --check > "$out/check.json" || fail '--check failed'
"$py" -I -c '
import json, sys
rows = json.load(open(sys.argv[1]))
py = [r for r in rows if r["name"].startswith("Python")]
if not py or not all(r["ok"] for r in py): sys.exit("missing: " + ", ".join(r["name"] for r in py if not r["ok"]))
print("  found: " + ", ".join(r["name"] for r in py)); print("  missing (installed by hand for now): " + ", ".join(r["name"] for r in rows if not r["ok"]))' "$out/check.json"

say "HTTPS with the bundled Python and this distribution's certificates"
"$py" -I -c '
import ssl, sys, urllib.error, urllib.request
try: urllib.request.urlopen(urllib.request.Request("https://api.github.com/repos/enjoyerofslop/droidplay-core/releases/latest", headers={"User-Agent": "DroidPlay smoke test"}), timeout=30)
except urllib.error.HTTPError as e: print(f"  HTTP {e.code} (the connection and its certificate were fine)")   # a rate limit still means TLS worked
except urllib.error.URLError as e: sys.exit(f"  {e.reason}  (default paths: {ssl.get_default_verify_paths()})")
else: print("  the releases API answered")'

for p in xcb wayland offscreen; do
    say "the window, $p"
    case $p in
        xcb) env=(DISPLAY=:99) ;;
        wayland) env=(WAYLAND_DISPLAY=wayland-dp) ;;
        offscreen) env=(QT_QPA_PLATFORM=offscreen) ;;
    esac
    env "${env[@]}" XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" "$py" "$dir/core/dm/android/linux/window_check.py" "$dir/core" "$out/$ID-$p.png" --expect $p \
        > "$out/window-$p.log" 2>&1 || { cat "$out/window-$p.log"; fail "the window under $p"; }
    grep -E '^window_check: (platform|--check)' "$out/window-$p.log" | sed 's/^/  /'
done
say 'passed'
