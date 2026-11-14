# dm/android — the host on Android

The Switch game's own arm64 code runs natively on an Android phone, as it does on the iPhone: the same rewriter, host and NVN interface layer, built
for Android (bionic, arm64-v8a) with stand-ins for the Darwin interfaces they call. The Vulkan NVN replay draws (stage 2: shaders translated to
SPIR-V when the game is packed) and runs compute and transform feedback; tessellation is missing, as on the Mac and iOS (see Status). Super Mario Bros. Wonder plays; Bowser's Fury and Danganronpa V3 (from a provisional profile) run with
a correct picture (the [README](../../README.md#game-status) has the list).

Translating shaders on the phone, at the moment a game uses them (instead of before packing): [plan_on_device_shaders.md](plan_on_device_shaders.md).

This host on Windows on Arm (Snapdragon laptops and tablets), reusing the Vulkan replay: [plan_windows_arm.md](../../docs/plan_windows_arm.md).

## Status

| Part | State |
|---|---|
| Rewriter | `DM_TARGET=android dm/tools/patch_nso.py`: guest thread slots through x18 (below); the outputs carry `# TARGET android` and the loader refuses a module rewritten for the other target |
| Host | All of `dm/host` builds unchanged in logic: Darwin interfaces come from `compat/` (force-included `dm_android.h` and headers named as Apple's) and `plat_android.c` |
| Audio and movies | AudioToolbox's output AudioUnit on AAudio (`coreaudio_aaudio.c`). Movies' H.264 (`hle_movie_avc.h`) and AAC (AudioConverter) on MediaCodec (`media_android.c`): H.264 pictures through an AImageReader (YUV 4:2:0 with plane strides), out as NV12 or RGBA |
| Graphics | `gfx_vk.c`, the NVN replay on Vulkan, stage 1: textures (one image per serial; packaged and CPU-written textures deswizzled and uploaded, re-uploaded after writes through the dirty-page tracker; BC decoded on the CPU with `dm/third_party/bcdec` where the GPU lacks BC), buffer/texture copies, CPU texel reads, render-target and texture clears (full ones), presents to the window. Draws (`gfx_vk_draw.inc`, stage 2): the shaders translated when the game is packed (below), one descriptor set per stage, pipelines cached by their state, render passes on the bound targets, vertex / index / uniform / storage data copied from guest memory per draw, NVN's driver constant banks made from the bound state, textures and samplers resolved through the pools the game registers. Compute dispatches (direct and indirect; c0 + 0x200 the group counts, + 0x210 the uniform buffer table). Indirect and multi-draw-indirect draws (the arguments read on the CPU once earlier GPU writes have landed). Images (storage images in the format the shader declares, the textures made mutable for it; buffer images as storage texel buffers), buffer textures (uniform texel buffers over copies; in a 2D slot read by wrapped 1D fetches, or with its handle in a uniform buffer, an image of 16,384-texel rows uploaded from guest memory, as the Mac's row wrap: `tbuf_rows.h`), multisample textures, with stand-ins of the right kind for unbound slots (a draw whose binding cannot be valid is skipped, never bound wrong). Textures shared by storage as `nvn_mtl.m` shares them (pool, offset and description: a render target made anew on another object each frame, or read through another object, is the same image); texture views in their own format (the images of uncompressed colour formats are mutable); whole texture arrays; vertex streams stepping per N instances (`VK_EXT_vertex_attribute_divisor`, or the KHR one, from instance 0 where its driver cannot start past it); vertex attributes matched to the shader's input types (res.json `vin`; integer-only inputs translated again as uint, as `dmsh_build.py` does): a format of another kind, or one the GPU takes as no vertex format (all scaled ones on the Adreno 619: `tests/t_vkfmt.c`), becomes on the CPU the 32-bit registers Maxwell's fetch would give, in a binding of its own; CPU-written linear textures (LINEAR, LINEAR_RENDER_TARGET: a game's glyph cache) uploaded from guest memory and again after CPU writes. Clears limited by the scissor (`vkCmdClearAttachments`) and with a channel or stencil mask (a draw: `shaders/clear.*`). Submissions are asynchronous: chunks of commands with their own staging, arena, descriptor pools and write-backs, completed in order by a thread that writes back to guest memory only the bytes the GPU changed, then signals events and the NVN fence. Not done: tessellation (the Mac / iOS backend has none either: `SetPatchSize` is not even recorded, and the Adreno binds only 4 descriptor sets) Transform feedback without `VK_EXT_transform_feedback` (the Adreno 619 has none): a vertex shader with a feedback layout gets a `.tf.spv` variant that stores its captured outputs into storage buffers, drawn as points with rasterization off and copied back to the game's buffers (the limits of `nvn_mtl.m` tf_draw: not indexed, instanced, fans or quads); rasterizer discard draws nothing else. `-DDM_GFX=null` builds `gfx_null.c` instead (no GPU) |
| Shaders | `tools/spv_build.py`: the package `dm/gfx/tools/shader_split.py` makes (as for iOS) translated by the repository's `dmsh_xlate` to Vulkan SPIR-V, then `tools/spv_android.py` (pure Python, in one process per CPU, or `spv_build.py -j N`; each shader's files are those one process makes): a descriptor set per stage, fragment shader interlock removed, a `.z.spv` variant of each vertex shader for NVN's -1..1 depth. The app reads them from `game/spv` (`DM_SPV_DIR`). Vertex shaders whose inputs are only used as integers are translated again with those inputs uint (`int_inputs` on the SPIR-V). Each constant buffer's extent, the bytes the shader can read of it, goes into res.json (`cbufs[].ext`, `cb_extents`: computed indices too, a loop's counter run through its trip count). Then a vertex shader's position can read the constant buffer rows it is made of through a run-time index (`pos_rows`), behind a specialization constant the replay sets on Qualcomm's Adreno 7xx driver for the vertex shaders a game's profile names (`vk_pos_rows`, `gfx_vk_posrows.inc`): that driver's binning copy of FF's flame shader lost those rows. Outputs carry the Android chain version (`android` in res.json); older ones are translated again, except chain 3's, which only lacked the extents and get them from their SPIR-V. Two-part vertex shaders (VertexA + VertexB) are merged first, as `dmsh_build.py` does; compute shaders without facts take their thread group from the control section |
| Front end | `front_android.c`: `GameActivity` (a NativeActivity that keeps the system bars off the screen), Vulkan window with a status screen, a physical controller as player 1, lifecycle, log to logcat (tag `DroidPlay`, `nxnative` before 0.5.0) and `files/log.txt`. The Java side (`java/`, in the APK's `classes.dex`): the virtual gamepad of dm/ios's `DmGamepad.m` (2D / 3D / Tap layouts, kept per app; hidden while a controller is connected; touches no control takes go to the game's touch screen; shaking the phone shakes the controller), drawn in a PopupWindow over the activity, and the software keyboard's text box (an AlertDialog). Game vibration (`haptics_android.c`): player 1's motors on the controller's vibrator, else the phone's, as a game vibration (`USAGE_GAME`); `DM_RUMBLE` / `DM_RUMBLE_GAIN` as on Apple. The driver's pipeline cache is kept in the app's files (`DM_PIPELINE_CACHE`), written when the app pauses |
| Packing | `scripts/build_android_from_nsp.py`, and the desktop app `scripts/nxnative_android.py` over it. A game without a profile is surveyed first and packed from a provisional profile (`--no-push`), then built and installed like any other. `--game-apk` (or `--xapk`) packs one game as an app of its own ([A game app](#a-game-app)) |

Tested on a POCO X5 5G (Snapdragon 695, Adreno 619, Android 14, 4 KiB pages): the self test and the sandbox checks pass in the app, and the host
unit tests below pass over adb. Games run on it: Super Mario Bros. Wonder, Super Mario 3D World + Bowser's Fury, Danganronpa V3 and The Binding of Isaac: Afterbirth+ with Repentance; Pokémon Brilliant Diamond on an Ayn Odin 2 (Snapdragon 8 Gen 2, Adreno 740, Android 13)
(its shaders compiled at run time and captured on the phone: [Run-time GLSLC outputs captured on the phone](../../docs/reference/shaders.md#run-time-glslc-outputs-captured-on-the-phone-android)).

## Games on the phone: the launcher

One app carries every game packed on the computer. Its launcher (`java/org/nxnative/android/Launcher.java`) lists the games in `Android/data/org.nxnative.android/files/games/<title ID>/`
(icon, name and version from their `icon.jpg` and `control.nacp`); a tap opens the game's settings screen, and Play starts it. It has the desktop app's look: an ink
ground, one cream action (Play), the game's own colours washed behind its screen; the app's icon is the adaptive icon in `apk/res` (the mark: two halves of a controller,
cream on ink, with a monochrome layer), which `tools/build_apk.py` links unless `--icon` names another.

- **A host per game.** The host is built for one profile, so the APK holds one per game, `lib/arm64-v8a/libdmhost_<title ID>.so`. The NativeActivity's library is
  `libnxloader.so` (`loader.c`): it reads the intent extra `game` (the title ID) and forwards `ANativeActivity_onCreate` to that game's host. A game folder whose host the APK
  lacks is listed as needing an app update.
- **A process per game.** The game runs in `:game` (`apk/AndroidManifest.xml`): a host is loaded once per process, so starting another game ends the running one, after
  asking, and starts a fresh process.
- **Per-game paths** (`front_android.c`): the game's files in `files/games/<id>/`, its settings in `files/games/<id>/launcher.txt` (and `env.txt`, for every game, in `files/`);
  in internal storage its save in `saves/<id>` (`DM_SAVE`; a device save beside it as `saves/<id>_device`) and the driver's pipeline cache in `cache/<id>/pipeline.cache`.
- **Saves of the one-game installs.** The app before the launcher kept one game in `files/game/` and its save in internal `files/save`. The packing script moves that game
  folder to `files/games/<id>/` when it packs the same game (the same `control.nacp`), or with `--migrate --title <id>`, and leaves a `legacy_save` marker there. At its next start
  the launcher backs every internal save up to `files/backups/<time>/` and copies `files/save` (and `save_device`) into `saves/<id>`, through a temporary folder; `files/save`
  itself is left as it was. **Back up saves** in the game list makes the same backup on demand (xattrs, the guest names of case collisions, go to its `xattrs.tsv`: external
  storage does not keep them); a backup pushed to `files/backups/restore/` is copied back into internal storage at the next start, after a backup of what is there,
  a game at a time (a game the backup has no save of keeps its own).
- **A game's save in and out** (the settings screen's Save rows, `LauncherSaves.java`; the logic, plain Java that `dm/tools/tests/test_android_saves.py` runs on a JVM, in
  `Saves.java`). Export writes `saves/<id>/` (and `saves/<id>_device/`) as a ZIP laid out as a backup, through Android's file picker. Import takes a ZIP or a folder from the
  picker, staged in internal `files/import-staging/`, and finds the save in the layouts other tools write (a DroidPlay export or backup; yuzu and its successors,
  `nand/user/save/0000000000000000/<user>/<title ID>/`; Ryujinx, `bis/user/save/<save ID>/0/` with the title ID in `ExtraData0`; JKSV with `.nx_save_meta.bin`; EdiZon
  with `edizon_save_metadata.json`; Checkpoint's `0x<title ID> <name>/`), else takes the folder as the save itself, matched by file names against the game's save on the
  phone when there is one. It keeps the game's save as a backup (`files/backups/<time>-<id>/`) before swapping the new one in by renames through `<save>.swap`, the
  host's own convention (`plat_android.c`), and never while the game runs. Earlier saves lists the backups that hold the game's save, and puts one back the same way.
  A save from the computer (`build_android_from_nsp.py --import-save` / `--restore-backup`, the desktop app's Import a save and Restore a backup) is pushed to
  `files/import/<id>/`, opened to the app, and taken by the launcher when it comes to the front, through the same find, dialogs and swap (`LauncherSaves.incoming`).
  Only `user.*` extended attributes travel with a save: a backup's `security.selinux` rows name its own phone's labels.
- **Removing a game** (the settings screen's Remove game row, and a tap on a game the APK has no host for: `LauncherRemove.java`; the logic, plain Java that
  `dm/tools/tests/test_android_remove.py` runs on a JVM, in `Removal.java`). It deletes what the install put there: `files/games/<id>/`, the internal `cache/<id>/`, and the
  RomFS image of an install with `--romfs-dir` when the game's `defaults.txt` names it as `DM_ROMFS=<folder>/<id>/RomFS.img` (with a copy's `.partial` / `.piece`). Saves,
  backups and the game's settings stay. The prompt gives the size first (added up off the UI thread) and offers Export save; the game is closed first, after asking.
  The folder is renamed to `games/.removing-<id>` before it is deleted, so the list skips a removal cut short, and the launcher finishes one at its next start.
  Links are deleted, not followed. A game app is uninstalled instead: its files are in its own package's folders.
- **Updates keep the saves.** Every APK is signed with `~/.android/nxnative-debug.jks` and installed with `adb install -r`; the scripts never uninstall.

## Settings screen

A game's settings screen (the iOS launcher's counterpart) shows its icon and name and the settings the Android host reads: console mode (`DM_OPMODE`), game language (`DM_LANG`, the game's own languages), game timing
(`DM_AUDIO_PACE`), game rumble and its strength (`DM_RUMBLE`, `DM_RUMBLE_GAIN`). Defaults come from the game's `defaults.txt`: the packing script's `--language`, and the switches a game's profile lists in `dm/games/<id>/android_defaults.txt` (KEY=VALUE per line), which the host takes after `launcher.txt` without overriding it or `env.txt`. The frame-rate counter
(`DM_SHOW_FPS`, drawn by `Gamepad.java` from the frames presented) and the render scale (`DM_RENDER_SCALE`, offered when `main.fixups.tsv` marks a render-scale stub: Mario 1.2.1 has
one, `dm/games/010015100B514000/patches.py`) are there too, and Keep the GPU clock up (`DM_VK_GPU_FLOOR=100`, `gfx_vk_floor.inc`: a KGSL power constraint on the Vulkan
device's own contexts), offered for every game on a phone with `/dev/kgsl-3d0` (Qualcomm's kernel GPU driver, tested as `vk_driver.c` does before loading Turnip), off by default. Play writes the game's `launcher.txt` and starts `GameActivity`, which reads it after `env.txt` without overriding it; a key
`env.txt` sets is shown greyed out. The choices are kept per game. Not yet on Android: touch feedback.

Write watches (`DM_PROBE_WATCH`, `dm/host/probe.c`) work here too: `heap+<offset>` names a guest-heap address (the guest address space moves between runs on Android), the page is made
writable for each emulated store (no second mapping), and STXR / STLXR are carried out as plain stores that succeed. One run with that last change showed a black picture: use the
watch for finding a writer, not for playing.

## Thread slots through x18

On Apple the rewritten code finds its four thread slots (the guest's x18, tpidr_el0 and tpidrro_el0, and the host gate) at `tpidrro_el0 + 8 * key`,
the pthread TSD. Linux arm64 gives user code no such register, but it preserves x18, which Apple's kernel does not. So on Android each guest thread
gets a block (`plat_android.c` `dm_slots_thread_init`): a page whose last 32 bytes are the slots, then 16 KiB of shadow call stack, and x18 points at
the start of the shadow call stack. The rewriter emits `mov xN, x18` where it emitted `mrs xN, tpidrro_el0` (`dm/tools/nso_asm.py`), and the key page
holds each slot's offset from x18 (-32, -24, -16, -8). The guest never changes x18: every guest use of x18 is rewritten to slot K18. System code built
with ShadowCallStack pushes return addresses at [x18] and finds a stack there. The host is built with x18 reserved, and gives x18 back to bionic's
value before a thread returns into bionic or exits (`dm_slots_x18_restore`).

## Building

With the Android SDK (NDK, CMake 3.22, build-tools), a JDK (keytool) and Python with lz4, capstone and fonttools:

```sh
cmake -G Ninja -S dm/android -B <build> -DCMAKE_TOOLCHAIN_FILE=<ndk>/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a \
      -DANDROID_PLATFORM=android-29 -DGAME=<base game title ID> [-DDM_TESTS=ON]
ninja -C <build> dmhost_app nxloader dmhost_cli   # libdmhost_<id>.so (the game's host in the app), libnxloader.so and dmhost (a command-line host for adb shell)
python dm/android/tests/selftest_nso.py <ndk clang> <ndk llvm-objcopy> <dir>/main.nso
DM_TARGET=android DM_GAME=<id> python dm/tools/patch_nso.py <dir>/main.nso <dir>/patched main
python dm/android/tools/build_apk.py --sdk <sdk> --loader <build>/libnxloader.so --so <build>/libdmhost_<id>.so [--so <another game's host> ...]       --selftest <dir>/patched --keystore ~/.android/nxnative-debug.jks -o nxnative.apk
```

On Windows, Git Bash rewrites `/data/...` arguments: set `MSYS_NO_PATHCONV=1` for adb only (it breaks native tools' paths).

`scripts/build_android_from_nsp.py <NSP folder> -k <prod.keys>` does it all for a game you own: extraction, the Android rewrite, the host for the
profile (kept in the computer's host library, `%LOCALAPPDATA%\nxnative\android\hosts\<id>\` on Windows, `~/.local/share/nxnative/android` elsewhere), the APK with every host in
the library, then installs it and copies the modules, the RomFS, the DLC, the button-icon font and a `game.json` to `Android/data/org.nxnative.android/files/games/<id>/`.
`--push-only` installs and copies a game built before without building; `--apk-only` packs and installs the app alone; `--migrate --title <id>` moves a one-game install;
`--check` and `--info <NSP folder>` print the requirements and a game's details as JSON; `--progress json` prints the build's progress as JSON events.
adbd may not create folders there, so the folder tree is made first by a mkdir script run in the shell. The shell makes them `drwxrws---` for itself, which an Android 15 phone (a Galaxy S21 Ultra) kept the app out of (it found no game and ran the self test, logging `[RED] the game in … cannot be read`), so the script opens the copied tree to everyone at the end (`chmod 0777` folders, `0666` files; `Android/data/<package>` is reachable only by the app and the shell). Every build is signed with
`~/.android/nxnative-debug.jks`: an app signed with another key cannot be updated, and uninstalling it deletes its saves and game files.

A phone adb loses on the way (a Wi-Fi phone that slept, roamed or restarted wireless debugging, a cable pulled) stops the step it was on with `PhoneLost`: adb's
own words (`device '…' not found`, `device offline`, `error: closed`, exit status 255 with no output), a call past its wait (`adb_timeout`: 120 s for a shell step,
60 s plus the bytes at 0.5 MB/s for a copy) or a copy whose byte count stood still for 90 s (`counting`). The install, the folder copies, the RomFS copy and the
game's copy as a whole then wait up to `--reconnect-wait` seconds (180) for the phone by its ID (`reacquire`): its serial again, or another serial with its ID,
connecting over Wi-Fi to its old address and to the address `adb mdns services` gives for its serial number; every later call goes to the serial it came back
as. Each step can run again, and the large copies go on from what the phone holds: the RomFS image from `RomFS.img.partial` (256 MiB pieces appended, the last
MiB hashed on both sides), a DLC image over 256 MiB the same way, and a shader folder of more than 200 files in tar archives of about 256 MiB, each named in the
folder's `.nxnative_manifest` once the phone has unpacked it (one archive of every file was 8.4 GB for Final Fantasy Resonance, and needed twice that free on the
phone). More than five losses in a row with no piece or archive copied between them stop it. The desktop app passes `--phone-id <id>` with `--serial`, so a build whose serial is gone before it starts (a
new Wi-Fi port since the phone was picked) still knows whom to wait for, and its **Install again** runs on the serial the phone has now; a serial whose phone now
gives another ID (an address lent to another phone) is refused. The app's failure help says what to do over Wi-Fi for a Wi-Fi phone (`explain.py`), and reports
leave out a Wi-Fi phone's mDNS name, which carries its serial number.

### A game app

`--game-apk` builds one game as an app of its own, to install on any device without this computer or the launcher: package `org.nxnative.game.t<title ID>`
(`tools/build_apk.py game_package`), the game's name and icon, `apk/AndroidManifest.game.xml`. Several install side by side, apart from the DroidPlay app, each with its own
saves. Nothing goes into the host library, so the DroidPlay app is left as it was.

- **What the APK carries:** the game's host alone (the NativeActivity's library; no loader) and the game's files under `assets/game/`, laid out as `games/<id>/` with a
  `files.txt` list. Its entry is `java GameStart`, which unpacks everything but the RomFS to `Android/data/<package>/files/games/<id>/` at the first start and after an
  update, where `front_android.c` finds the game, then starts `GameActivity` in its own process. The translated shaders are one entry, `spv.zip` (a stored ZIP of
  the `spv` folder, which GameStart unpacks into `spv/`), because an APK without ZIP64 holds at most 65,535 entries and a game's shaders are files of their own
  (Mario Kart 8 Deluxe's passed that, issue #169); the other files are counted against that limit before packing.
- **The RomFS is read where it lies, never copied.** GameStart opens it and sets `DM_ROMFS=fd:<descriptor>:<offset>:<length>` in the process's environment, which
  `dm/host/romfs_image.h` (`romfs_image_spec`) reads as that part of an open file. It looks in order:
  1. a stored entry of the APK, `assets/game/RomFS.img`, packed when the APK stays under 2 GiB (Python's zipfile writes no bigger ZIP without ZIP64, which `apksigner` can't read);
  2. the app's OBB file, `Android/obb/<package>/main.1.<package>.obb`;
  3. a file the player picked once with Android's file picker, kept as a persistable grant: `<name>.romfs`, written beside a bigger game's APK (an APFS clone or a hard link
     of the extraction's `RomFS.img`), which can go anywhere on the device or its SD card.

  Each is checked for the image's mark and for the size `game.json` gives.
- **DLC** (`aoc/<index>.img`) goes into the APK while it fits. When it doesn't (Mario Kart 8 Deluxe's Booster Course Pass and its shaders pass 2 GiB with no RomFS,
  issue #169), it follows the RomFS in the same data file, the `.romfs` or the OBB, so the player still picks one file: the RomFS image from byte 0, then at `dlc_at`
  (the RomFS's end rounded up to 4 KiB) a pack, `DMAOCPK1`, the image count, a record per image (index, offset, size) and the images at 4 KiB offsets. `game.json` gives
  `dlc_at` and the file's whole size, `data_bytes`. GameStart copies the images out once into `games/<id>/aoc/`, where `front_android.c` points `DM_AOC_DIR` (the host
  mounts DLC by path; only the RomFS reaches it as a descriptor), and again after an update of the app. The RomFS leaves the APK first, and the DLC only with it.
  What the APK must still carry and can't (the code, the shaders and the font past 2 GiB) stops the build with each part's size: such a game installs through the
  library instead. `NXNATIVE_GAME_APP_LIMIT=<bytes>` lowers the limit, so a smaller game takes a big one's layout (a check of the DLC beside the APK).
- **`--xapk`** packs one `<name>.xapk` instead, an XAPK installer's format: `manifest.json`, `icon.jpg`, `<package>.apk` and the RomFS (with the DLC after it, as
  above, when the APK can't hold it) as its OBB file, stored.
  An XAPK installer installs the APK and puts the OBB in place (Android 11+ lets an app holding `REQUEST_INSTALL_PACKAGES` write other apps' OBB folders).
- `--out <folder>` puts them elsewhere than the NSP folder. The two kinds sit side by side; each replaces only its own files.

## Desktop app

`scripts/nxnative_android.py` (Python with PySide6: `pip install PySide6`) runs the packing script and adb for you:

- **Library**: the NSP folders you add, with each game's box art, name, version and status on the phone picked at the top; *Build and install*, *Build only*, *Install
  on phone* (no build) and *Play*. A game without a profile gets *Experimental build* (`--no-push`: the survey, a provisional profile in `<NSP folder>/<id>_build/profile`,
  found through `DM_GAMES_DIR`); once built it is marked *Experimental* and installs like the others.
- **Phones**: the adb devices over USB and Wi-Fi (*Pair over Wi-Fi*, *Connect*, *Discover*, *Disconnect*); for the phone picked at the top, the app version and the games
  on it, *Update the app*, *Move the old one-game install*, *Back up saves to this computer* (the app's `files/backups/`), *Restore a backup* and *Remove game from
  phone* (its save stays); a game's *Manage* menu has *Import a save to the phone* and *Restore its save from a backup*.
  A phone is known by its ID (`dm/android/tools/phone_id.py`: `ro.serialno`, or the shell's Android ID where that is empty or a placeholder), asked once per adb
  serial: the picker shows a phone on USB and Wi-Fi once, over USB; the picked phone, its backups (`<data>/backups/<id>`) and the first-start notice are kept by ID, as
  are the script's device lock and `logs/push_<id>`, so a Wi-Fi port that changes loses nothing and one phone's two serials can't run two installs at once.
- **Builds**: the running build's stages, progress bars and messages, from `--progress json` (or a console build's journal); *Stop* ends it and its tools.
- **Performance**: `adb logcat -s DroidPlay` with tiles for the figures the host prints (frame time, GPU busy, replay CPU and draws, game speed, the wait for the GPU) and a frame-time
  chart; *Profile a frame* asks for a frame's GPU times per pass (`files/prof_now`) and groups them; *Thermal* shows the phone's cooling state.
- **Settings**: the keys, and `--check` with how to get what is missing. It opens first when something is.

## Checks

- The app without a game also runs `nvn_demo.c`: the NVN calls a game makes to clear its window texture and present it, 600 frames of changing colour on screen,
  and with `--demo` given to `build_apk.py` (the output of `tests/synth_shaders.py` translated: `dmsh_xlate` on its `jobs.json`, then `tools/spv_android.py`, plus its
  `*.code` / `*.ctl`) a triangle drawn by synthetic Maxwell shaders over it. `dmhost` runs it after the self test with `DM_NVN_DEMO=<frames>` and `DM_SPV_DIR`, and logs
  pixels read back: on the Adreno 619 the centre is `ffccbbba` (red and green interpolated, blue and alpha from the uniform buffer) and the orientation is NVN's lower-left origin.
  The demo's last frame also draws through DrawArraysIndirect, clears two corners through the scissor (one red only), and dispatches a compute shader that writes
  a storage buffer (checked after, with a word the CPU wrote meanwhile beside it, which must stay); with `vstf.*` it draws the triangle once more with
  rasterization off and transform feedback on, and checks the captured outputs.
  The last frame reads the triangle's colour as RGBA16_UI2F from a stream of its own. The texture probe after the frames also clears a target through one texture object and reads it through another over the same storage.
  `DM_NVN_DEMO_MODE=llf` culls front faces (the triangle goes), `ul` sets what most games set (upper-left origin, depth 0..1) with back faces culled (the triangle
  stays, its rows flipped), `ulf` culls front faces there (it goes): all four pass on the Adreno 619.
- The app without a game: the self test (`tests/selftest_nso.py`: x18 slots, TLS, TLR, gates, an import, a second guest thread) and the sandbox checks
  (a write fault reaching the host's hook past ART, the save transaction's tree copy with xattrs and `renameat2(RENAME_EXCHANGE)`, an unmapped read).
- In the app without a game, with switches in `env.txt`: `DM_PAD_TEST=1` sends made-up touches to the virtual gamepad (not to the screen) and checks every
  key, the D-pad, both sticks, two fingers and the game's touch screen (`gamepad check: 44 checks, 0 failed`); `DM_TEXT_TEST=2` opens the text box after the demo and
  types and confirms "Zelda" and an emoji itself, under a minimum of 2 characters (`text box check: … OK with 1 character disabled`, then `text "Zelda😀" (9 bytes)`; `DM_TEXT_TEST=1` waits for a person); `DM_RUMBLE_TEST=1` vibrates half a second (`dumpsys vibrator_manager` shows it).
- `tests/t_audio`: the AAudio output at real time (a 440 Hz tone for two seconds: the frames rendered, the underruns), Stop and Start again.
- `tests/t_media <h264> <aac> [<h264 without B-frames>]`: the movie decoders on clips ffmpeg makes (`ffmpeg -f lavfi -i color=c=0x2060c0:size=320x240:rate=30 -t 1 -c:v libx264 -profile:v high -bf 2
  -pix_fmt yuv420p -x264-params aud=1 -f h264 blue.h264`; `ffmpeg -f lavfi -i sine=frequency=1000:sample_rate=48000 -t 1 -ac 2 -c:a aac -f adts tone.aac`; the same picture
  with `-profile:v main -bf 0` into `blue_p.h264`): every picture in NV12 and RGBA with its colour, the tone's length and pitch, and every picture of the clip without
  B-frames before the end of stream, as it is and with its SPS's `bitstream_restriction` taken out (10 of 10 on the Odin 2 on 2026-11-12).
- `-DDM_TESTS=ON`: `t_thread_leak`, `t_condvar`, `t_fs`, `t_fs_commit_list`, `t_svc_memory` (with and without the map limit), `t_nro`, `t_hid`,
  `t_preempt` (both modes) pass on the device. `t_nvn` passes 154 of 154 on `gfx_vk.c` (130 on `gfx_null.c`; two of its checks time the GPU right after a submit and can miss on a
  fast run); before asynchronous submissions three of them failed: they expect the GPU to be
  still busy right after a submit.

## Benchmarks

`scripts/bench_android.py` runs games on one or more devices over adb, the same way on each, so devices and builds compare. Each run uses the same save, the same scripted input and the same measuring window:

```
python3 scripts/bench_android.py <NSP folder> [<NSP folder> ...] [-s <serial> ...] [--route <name>] [--warmup] [--runs N] [--install]
```

- **Routes:** a game's routes are in `dm/games/<id>/bench_android.tsv`, one per line:
  - the `DM_KEYS` steps
  - a save folder or none
  - the measuring window
  - a log pattern that marks the game ready
  - a description
  - optionally, switches the route needs (`DM_FIXED_TIME` for a scene that follows the clock, `DM_SHARED_FONT`, `DM_KBD`)

  The first routes are Super Mario Odyssey's Start menu and opening cutscene, Brilliant Diamond's language menu and Eterna City, Animal Crossing's airport and island, Smash's opening movie and a battle, Link's Awakening's opening movie and overworld, and Skyward Sword HD's opening movie and Skyloft. Results are in [baselines.md](baselines.md).
- **Each run:**
  1. waits for the device to cool down
  2. copies the route's save into a folder of the run's own under the app's external files (`files/bench_runs/`), with `DM_SAVE` pointing there, so the device's own saves aren't touched
  3. writes `env.txt` (per-frame timing goes through `DM_NVN_TSV`)
  4. starts the game from the launcher
  5. takes screenshots at the window's start and end (on a phone with a cover screen too, of the screen that's on: the cover screen of a folded Galaxy Z Fold 8)
  6. puts `env.txt` back as it was
  7. removes its own folder under `files/bench_runs/`, and no other run's: on a shared test device the others may be another session's
- **Making a save:** `--keep-save <folder>` copies the save a run ended with back to the computer, for a route that starts from it. `--shot-every N` adds a screenshot every N seconds, for working a route out.
- **Keeping devices awake:** every 45 s each device gets `KEYCODE_WAKEUP`, and the lock screen is dismissed before each start. A PIN or pattern lock can't be dismissed over adb, so set test devices to None or Swipe.
- **Results:** per device, game and route:
  - time to the first frame and to the ready pattern
  - frames per second, frame time p50 / p95 / p99, and the share of frames over 17.5 and 34 ms
  - GPU time per frame, memory (PSS) and temperatures
  - how the game ended. A run ends early only on a confirmed death: an empty `pidof` alone isn't one, since it came back empty for a live game on a device out of memory (the Odin 2). The game's pid must also be gone from `/proc`, or the run's logcat must say the game's process died; otherwise the run carries on, and the miss is printed and kept in `result.json` (`pidof_misses`)

  They go in `results.md` and `results.json`, beside each run's log, timing file and screenshots.
- **Warm-up:** `--warmup` adds a first run, reported apart. The first launch after an install builds the driver's pipelines: Brilliant Diamond's first frame took 33 s there on a OnePlus Pad 3, against 10 s warm on an Odin 2.
- **Installs:** install before a run, never during one, because an install stops the app.

### Save sweeps

A sweep starts games from saves people downloaded, so a run lands in late-game places that a scripted route from a new game never reaches: a 100 % Ocarina of Time file, Super Mario Odyssey's Metro and Sand kingdoms, Breath of the Wild after the main story. It's for regressions, performance and coverage after a merge.

**A downloaded save never moves a game up a tier.** Playable needs hand play from a new game, or from a save made on Android.

```
python3 scripts/bench_android.py <NSP folder> [<NSP folder> ...] -s <serial> --sweep [--compare <earlier sweep>]
python3 scripts/bench_android.py <NSP folder> -s <serial> --routes 'dl_metro,dl_sand'
```

- **Scene routes:** a game's `dl_*` routes in its `bench_android.tsv`. Each one:
  - starts from a downloaded save in the game's `saves/dl-<name>/` folder;
  - uses the keys that reach player control from the title's Continue;
  - measures a 60–90 s window, standing or walking;
  - has a ready pattern where the scene's files show one;
  - has a description of where the save stands.

  A save only gets a route once it has loaded on the game version we pack. A save from a newer update may not load, and is noted and left out.
- **A save in its archive:** a downloaded save stays in the archive it came in (JKSV, Checkpoint and EdiZon folders, or its files bare). The route names the archive, then `//` and the folder inside it that holds the save's files:

  `saves/dl-100pct/<archive>.zip//<folder inside>`

  It's unpacked into the sweep's output folder (`dm/android/tools/bench_sweep.py`): zip by Python, or by Info-ZIP's `unzip` for Deflate64, which Python can't read (Breath of the Wild's save); rar and 7z by `bsdtar` (macOS's `tar`, and Windows 10's). Nothing is written beside the archive.
- **`--sweep`** runs every `dl_*` route of each game given, once each unless `--runs` says otherwise. **`--routes`** takes comma-separated shell patterns instead.
- **`sweep.md`** has one table per phone, and `sweep.json` has the same rows. The columns:
  - route
  - load to the scene: the ready pattern's time (– for a route without one)
  - fps, frame p50 / p95, GPU p50
  - PSS
  - red lines, with how many kinds (numbers and addresses made alike)
  - how it ended
  - the end screenshot's path
- **`--compare <earlier sweep folder or sweep.json>`** flags a route when:
  - its fps fell 5 % or more;
  - it has a red-line kind the earlier sweep didn't;
  - it crashed or was killed where the earlier sweep didn't.

  The flags go in a column of their own, with the count at the top of `sweep.md`.
- **On an idle phone:** `device-sweep queue <NSP folder> <APK | installed>` adds a `sweep` job to device-watch's queue, which `device-watch match` lists like any other job, and `device-sweep run <serial> <job id>` runs it. The runner:
  1. takes the device's claim as every job does, and refuses a held device;
  2. pulls the app's APK;
  3. installs the build (or keeps the installed app);
  4. sweeps, comparing against the last sweep of that game on that phone;
  5. puts the APK back, releases the claim, and marks the job done with the result line.

  device-watch and device-sweep are the orchestration tooling on the test Mac, outside this repository.

**The routes:** the 12 downloaded saves on the test Mac, each in its game's `saves/dl-<name>/` folder. Loads are measured from the launch, on a Galaxy Z Flip 7 (Xclipse 950) and a Retroid Pocket 5 (Adreno 650):

| Game | Route | Where it stands | Load to scene |
|---|---|---|---|
| Breath of the Wild | `dl_main_story` | the Great Plateau, after the main story | 62 s |
| Link's Awakening | `dl_eagles_tower` | Mabe Village, before Eagle's Tower | 27 s |
| | `dl_turtle_rock` | Tal Tal Mountain Range, before Turtle Rock | 27 s |
| Persona 3 Portable | `dl_final_day` | the Dark Hour at Tartarus, on 1/31 | (no ready pattern) |
| Pokémon Shield | `dl_wyndon` | a Wyndon street, with all eight badges | 25 s |
| | `dl_postgame` | a Pokémon Center, post-game | 26 s |
| Skyward Sword HD | `dl_sand_sea` | by a bird statue in the Lanayru Sand Sea | 30 s |
| Smash Bros. Ultimate | `dl_all_characters` | a battle against a random fighter, every fighter unlocked | (no ready pattern) |
| Super Mario Odyssey | `dl_metro`, `dl_sand`, `dl_bowsers` | New Donk City, outside Tostarena, Bowser's Kingdom | 51 s |
| Ocarina of Time | none yet | the 100 % save: killed for memory on the Flip before it loaded; it needs a phone with more memory | |

Breath of the Wild's keys clear the "Downloaded DLC" notice first, so they work only when The Champions' Ballad is packed.

## What the GPU backend needs from this device

The front end logs the Vulkan device's capabilities at start. The Adreno 619's driver (Vulkan 1.1.128) has no BC texture compression (every packaged
Switch texture is BC: they need decoding), no descriptor indexing (NVN's texture and sampler pools are bindless), no transform feedback, no custom
border colors and no extended dynamic state; it has geometry and tessellation shaders, buffer device addresses and push descriptors.

The replay has shaders of its own too, for what Vulkan has no command for: `dm/android/shaders/` (a depth-stencil copy that scales or changes format, drawn by
`gfx_vk_dscopy.inc`), compiled at build time by the NDK's `glslc` (`shader-tools/`). The copy writes the stencil from its shader where the device has
`VK_EXT_shader_stencil_export` (the Adreno 740 and 830), and a bit at a time where it has not (the Adreno 650).

## Another Vulkan driver: DM_VK_DRIVER

The host does not link `libvulkan.so`. Every `vk*` function the replay and the front end call is a pointer (`gfx_vk.h`, `VK_NO_PROTOTYPES` with the list in
`VKB_FNS`), filled by `vk_driver.c` when `vkb()` makes the device: from the system's `libvulkan.so`, opened then, by default; with `DM_VK_DRIVER=<file>`, from a
driver the player put beside `env.txt` (a Turnip build, as for Switch emulators, or RADV for Xclipse: [Another Vulkan driver](../../docs/android.md#another-vulkan-driver-turnip)).

- The file is copied into the internal files (`vk_driver/<name>`, read-only, again when its size or time changes): Android maps no code from shared storage.
- It is opened through libadrenotools ([dm/third_party/libadrenotools](../third_party/libadrenotools/README.md)): `adrenotools_open_libvulkan` loads a second copy of
  the system's `libvulkan.so` in a linker namespace of its own, whose hook (`libmain_hook.so`, `libhook_impl.so`) loads the player's driver where that loader would load
  the vendor's. The loader still makes the window surface and the swapchain (Turnip on Android has no WSI of its own). The hooks are found in the app's extracted native
  library folder (both manifests set `extractNativeLibs`), and `libadrenotools.so` is opened only when `DM_VK_DRIVER` is set: its liblinkernsbypass constructor changes
  the protection of the linker's `dlopen` page as it loads.
- A driver is used only once it offers a GPU on an instance made to ask, and once its file is mapped in the process (by device and inode in `/proc/self/maps`): when the
  hook fails, libadrenotools loads the vendor's driver in its place and says so only in logcat (`hook_impl`). Anything short of that is one `[RED] [vk] DM_VK_DRIVER`
  line, and the system's driver runs.
- Past that probe, the driver can still fail to make the instance, to offer a graphics queue or the window extensions (`VK_KHR_android_surface`, `VK_KHR_swapchain`),
  or to make the device. Before, the game then ran with no picture: the front end had no device. Now `vk_create` destroys what the try made and tries again on the
  system's driver (`vk_try`, `vk_driver_fallback`), with one line: `[RED] [vk] DM_VK_DRIVER <file>: no Vulkan device (vkCreateDevice: VK_ERROR_…); the system's
  driver instead`. The driver's library stays loaded beside the system's, each in its own linker namespace. `DM_VK_DRIVER_FAIL=<step>` (`instance`, `gpu`,
  `queue`, `extensions`, `device`, `swapchain`) fails that step on the player's driver, to try the fallback on a phone whose driver works.
- The first window's surface and swapchain (`front_android.c` `vk_window_first`) come before the host starts, so the front end's are the only Vulkan objects then. When
  one of them fails on the player's driver, the front end destroys what it made, `vkb_fallback` destroys the device and the instance, and both are made again on the
  system's driver, in the same process (`vkb()` is made under a mutex, not a `pthread_once`, for this). A later window (after the app comes back) can't: the game's
  textures and pipelines are on the device by then.
- Start records and the crash guard (`vk_driver.c`): a driver that crashes or hangs the game would do it at every start. Before the driver opens, the host writes
  `{"state": "trying", …}` to `<external files>/vk_driver_state/<driver file>.json`, and the same state to the game's `games/<title ID>/vk_driver_result.txt`
  (`driver=`, `result=`, `reason=`, `time=` lines, for the desktop app and the launcher). Then: `ok` after 120 presented frames or 30 s of the game (a first
  start's pipeline compile takes longer), or once the self test's picture check has drawn; `fell back` (any fallback above); `crashed` (crash.c's
  `crash_set_pc_hook`: the crashing thread's pc, lr or a return address in its first 16 frames lies in the driver's mapping, from `/proc/self/maps`); `crashed
  elsewhere` (a crash with the driver running, in the host's or the game's code, with the module and offset); `device lost` (`gpu_fault_log`). A start that finds
  `crashed`, `device lost` or `trying` (a start that ended before `ok`) runs the system's driver once and writes `skipped`, with a `[RED]` line and a line on the
  status screen, which also names the driver in use or why it isn't; the next start tries the driver again. `crashed elsewhere` doesn't skip (a game that crashes at
  boot in its own code would otherwise change drivers at every start); the next start says it in the log and on the status screen. A hang in the driver after `ok`
  isn't caught: the records can't tell a hang past 30 s from a long first compile, as the plan accepts. A start without `DM_VK_DRIVER` removes the game's result file; on a phone with neither GPU (below) the result is
  `not used`. The host's crash handler goes in as the host starts (`main.c`), after the device and the first window, so a crash in the driver before then is
  left as `trying`, which skips the driver once too. `DM_VK_DRIVER_FAIL=crash` crashes in the driver's code (a `vkCreateSampler` with no create info) at the
  replay's first use of the device, to try the guard; `crash_host` crashes in the host's code there, for `crashed elsewhere`.
- The start record (`vk_start.c`), whatever driver runs: `games/<title ID>/vk_start.txt`, with `driver=` (`system`, or the `DM_VK_DRIVER` file), `driver_id=`
  (`VkDriverId`: 8 Qualcomm's own, 18 Turnip), `gpu=`, `pipelines_failed=` (pipelines the game needed that the driver refused, graphics and compute, each once),
  `failed_shaders=` (the first 4 distinct shader sets: vertex+fragment keys, or the compute shader's), `device_lost=` and `time=`. The front end removes the last start's when the game's
  folder is known; the host writes it when the device is made, within 2 s of a change (`vk_start_tick`, from the front end's loop, not under the replay's lock), and at
  once when the GPU is lost. A refused pipeline drops its draws: Frostpunk's Generator on a Razr's Adreno 730 (54 pipelines on the way into its city, where the log
  names only the first 16 of each kind), Ocarina of Time's characters on an Odin 2's 740. DroidPlay for Windows reads the record to suggest a driver its catalog marks tested for that game on that GPU; it never switches one on by itself.
- `DM_VK_DRIVER` is opened only on a GPU another driver can run on, told by its kernel GPU driver: an Adreno (`/dev/kgsl-3d0`, Qualcomm's KGSL, which Turnip drives)
  or a Samsung Xclipse (a DRM render node whose driver name is `amdgpu`, as RADV tells its GPU: Samsung's sgpu, AMD's amdgpu adapted). libadrenotools' custom-driver
  load is the same for any vendor: the hook swaps the file the system's loader opens as `vulkan.<ro.hardware.vulkan>.so` (`vulkan.samsung.so` on an Xclipse,
  through `android_load_sphal_library`). Elsewhere (a Mali) it isn't opened, with one plain `[vk] DM_VK_DRIVER` line, and `libadrenotools.so` is not loaded, so its
  constructor doesn't patch the linker. Which GPU let it try is logged (`trying it on this Xclipse GPU`), and the driver line after it names the driver that ran.
- Each driver keeps its own pipeline cache: `DM_PIPELINE_CACHE` with `.<driver file name>` after it (`gfx_vk_draw.inc` `pcache`).
- `build_android_from_nsp.py` builds the three libraries with every host and packs them into both kinds of app (`build_apk.py --lib`).
- Tried with Mr. Purple's Turnip T30 (Mesa 26.3.0-devel, Vulkan 1.4.359) on an Odin 2 (Adreno 740, Android 13) and a Retroid Pocket 5 (Adreno 650, Android 13), with
  `scripts/bench_android.py`. It loads and draws on both. Super Mario Odyssey's opening: Odin 2 59.9 fps (GPU 3.5 ms against 5.7 on Qualcomm's driver), Retroid
  Pocket 5 40.2 fps (37.5 on Qualcomm's). Brilliant Diamond in Eterna City: Odin 2 60.0 fps on both drivers (GPU 5.9 ms against 8.2). It ran at 20.6 fps until the
  arena took a cached memory type: Turnip lists a write-combined one first, and the completion thread read storage-buffer write-backs from it ([rendering](../../docs/reference/rendering.md#storage-buffer-write-backs-read-from-cached-memory-on-vulkan)).
  On the Retroid Pocket 5 it ran at 25.1 fps (11.3 before), and at 57.7–58.2 fps on 2026-11-10, against 14.9 on Qualcomm's driver, whose compiler no
  longer crashes on it ([baselines](baselines.md), problems 2 and 13).
