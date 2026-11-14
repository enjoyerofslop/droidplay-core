# dm/linux: the host on Linux (aarch64)

The Switch game's own arm64 code runs natively on an arm64 Linux device (handhelds such as the AYN Odin and Thor running a desktop Linux, Snapdragon laptops, the Steam Frame), as it does on Android. It uses the same rewriter, the same host and the same Vulkan replay of NVN (`dm/android/gfx_vk.c`). The parts that differ are a Linux front end, Linux audio and Linux movie decoding.

## Status

| Part | State |
|---|---|
| Rewriter | `DM_TARGET=linux dm/tools/patch_nso.py`: the thread slots are read at `tpidr_el0` (glibc's thread pointer) plus each slot's offset in the host's initial-exec TLS (`plat_linux.c`). Android's x18 block is not used: desktop Linux libraries (glibc, Mesa, SDL) are not built with x18 reserved, so a host call into one may change it. The outputs carry `# TARGET linux`, and the loader refuses modules rewritten for another target |
| Host | `dm/host` and Android's platform layer (`dm/android/plat_android.c`, the Darwin stand-ins of `dm/android/compat`) built for glibc with `DM_LINUX`. Android's performance hint sessions and thermal manager are absent; `libvulkan.so.1` is opened as the system's driver (`VK_DRIVER_FILES` picks another one, `DM_VK_DRIVER` is Android's) |
| Window | `front_linux.c`, SDL3: a Wayland window (fullscreen unless `DM_WINDOWED=1`, F11 switches) with a Vulkan swapchain on the replay's device. The game's picture is scaled to fit with its aspect kept (black bars where the screen's shape differs). FIFO presents by default, as on Android; `DM_VK_PRESENT=mailbox` or `immediate` lowers latency |
| Input | SDL gamepads: buttons by position in Nintendo's layout (`DM_PAD_ABXY=label` maps by label), sticks, analog triggers as ZL / ZR. The keyboard: arrows for the D-pad, X / Z for A / B, S / A for X / Y, Q / W for L / R, 1 / 2 for ZL / ZR, Return for plus, Backspace for minus. Both are player 1. Escape ends the game. Not done: more players, motion, touch, the controller applet's prompt |
| Vibration | Player 1's motors on the gamepad's low- and high-frequency motors (`DM_RUMBLE=0` turns it off, `DM_RUMBLE_GAIN` scales it) |
| Audio | `audio_sdl.c`: AudioToolbox's output unit on an SDL3 audio stream (PipeWire, PulseAudio or ALSA), asked for 5 ms device buffers |
| Movies | `media_linux.c`: H.264, AAC and AV1 on FFmpeg's libavcodec (libdav1d for AV1 when FFmpeg has it) |
| Software keyboard | No input box yet: the game's proposed text is confirmed, else `DM_SWKBD_TEXT`, else "Player" |

## Building

On the aarch64 Linux device itself, with clang, CMake, Ninja, glslc (shaderc), Python 3, the Vulkan headers, and SDL3 and FFmpeg with their development files. On Arch Linux ARM:

```
sudo pacman -S --needed clang cmake ninja shaderc python sdl3 ffmpeg vulkan-headers vulkan-icd-loader
cmake -G Ninja -S dm/linux -B build-linux -DCMAKE_C_COMPILER=clang -DGAME=<title ID>
ninja -C build-linux dmhost
```

The host is built per game profile, as on the Mac and Android. `-DDM_TESTS=ON` adds the host unit tests (`ctest --test-dir build-linux`).

## Packing a game

`scripts/build_android_from_nsp.py <NSP folder> -k <prod.keys> --linux` extracts the game, rewrites its modules for Linux, translates its shaders and writes the game's folder to `<NSP folder>/<title ID>_build/linux/<title ID>` (`--out` puts it elsewhere). It needs no Android SDK and no phone. Run on the aarch64 Linux device, it also builds the host into that folder. A game whose shaders are Oodle-compressed cannot be packed this way yet.

Copy the folder to `~/.local/share/nxnative/linux/games/<title ID>` on the device and run `dmhost_<title ID>` (or give the game folder as its argument). Saves, the pipeline cache and `log.txt` are kept in `~/.local/share/nxnative/linux` (`$DM_DATA` or `$XDG_DATA_HOME/nxnative/linux` when set). `env.txt` there sets switches for every game, as on Android.

## Checks

`.github/workflows/linux.yml` builds the host on an arm64 runner, runs the unit tests and runs the self test module (`dm/android/tests/selftest_nso.py`, rewritten with `DM_TARGET=linux`) on the command-line host.
