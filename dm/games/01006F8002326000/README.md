# Animal Crossing: New Horizons (`01006F8002326000`)

Update 3.0.3 (v2228224) over the base game, with the Happy Home Paradise add-on mounted. SDK 20.5.4. Nintendo EPD's own engine (code name Park; sead / agl / gsys, NintendoWare G3d / Ui2d / Vfx2 / Atk / Font, Havok, Pia and NEX for local and online play) on NVN. Modules `main`, `subsdk0` (the SDK's movie library) and `sdk`. Plays on the iPhone; 30 fps, as on the Switch.

On the Mac: a new game through Tom Nook's counter (name entry on the game's own keyboard, birthday, look, hemisphere, island choice), the in-flight movie with its sound, the island's creation and the arrival (Timmy's welcome, the player at the dock under control); no red line but the ones below. 1.85 GB resident and 1.15 GB of Metal allocations at the dock.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `render_cond on` | [`game.tsv`](game.tsv) | the game wraps its clears in render conditions that skip a target nothing drew into since its last clear, as Tears of the Kingdom does ([0100F2C0115B6000](../0100F2C0115B6000/README.md)). Without the key, four clear-only passes wiped the composite before the window copy at frame 900, and every frame came out black (the island probe and the loading icon were drawn into render targets) |
| `sys_resource 0x0` | `game.tsv` | the NPDM declares no system resource (META +0x14 = 0): `nninitStartup` sets a heap (3.09 GB) |
| 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | `game.tsv` | 256 MB in front + 256 MB alias region + 256 MB stack region + 3.25 GB heap region |
| `mem_used 0x7a00000` (122 MB) | `game.tsv` | the rewritten modules are larger than the default 64 MB of used memory (`main` alone is 63 MB of text and 15 MB of bss); the NSP script's guess after the rewrite, which leaves a 3.09 GB heap |
| `pad_layout full` | `game.tsv` | the 3D layout: the D-pad holds the tool ring and the right stick turns the camera |
| `shader_groups` `/Shader/` `/System/` `/Effect/` `/Model/` `/Layout/` `/Swkbd/` | `game.tsv` | see [Files and shaders](#files-and-shaders) |

## Rewriter

- **Resolved: a `B()` assertion at a spin wait, a branch into the patch area beyond the ±128 MB a `b` reaches.** Its `main` was the first module to put branches out of range of the patch area: 63 MB of text and 51 halfword jump tables (`ldrh wE, [xT, xI, lsl #1]; …; adr x18, #L; add x18, x18, xE, lsl #2; br x18`). Each got a landing pad for every one of the 65,536 values an entry can hold (768 KB a table), whatever the switch's size: 40 MB, and the spin-wait stubs placed after them were out of reach. `half_count` now pads only up to a table's largest entry; 7 of the 51 cannot be read and keep all 65,536. The patch area went from 42 MB to 7.9 MB.
- The spin-wait search through stores finds 166 loops in `main` (162 before), and the movie library's thread loop is wider in `subsdk0` ([development.md, Power](../../../docs/development.md#power)).

## Threads

- **Resolved: a black screen with the plane icon for 77 s after the opening movie** (also about 40 s of the same at boot), the CPU nearly idle. `SaveProc` yielded about 2,600 times a second, the power line's top yielder with as many back-off sleeps. The thread was working, not waiting: its grid walks over the new island (`main+0x157d020`) call `nn::os::YieldThread` once per cell, and the back-off made each yield a 250 µs sleep. A yield whose caller's x19–x28 changed since the last one now counts as progress and returns at once (`thr_yield` in `hle_thread.c`). The island now shows about 10 s after the movie, as with `DM_YIELD_BACKOFF=0`. A temporary log of which of x19–x28 move at each yield, per thread, told the working loops from the polls.

## Services

- **Resolved: abort in `sm::Initialize` under `sf::hipc::InitializeHipcServiceResolution`, right after the first frames** (from `main+0x5b8198`). `main` links its own clients of `olsc:u` (cloud save data backup settings) and `npns` (push notifications), which talk HIPC through `sm` and `svcSendSyncRequest`. For each user the game asks whether backup is on and switches it off where it is (`main+0x2809c8c`); a failed connect aborts as well. `hle_sf.c` takes over the SDK's HIPC client entry points (`InitializeHipcServiceResolution`, `ConnectToHipcService`, `SendSyncRequest`, `CloseClientSessionHandle` and the control requests `QueryPointerBufferSize`, `ConvertSessionToDomain`, `DuplicateSession(Ex)`): `olsc:u` gets a host session, and every request on it succeeds with zeroed output, which reads as backup off for every user. Any other name is refused with a red line, as `sm` refuses a service nobody registered (`npns` was never connected). `DM_SF_TRACE=1` dumps each request.
- **Current: device save data.** 3.0.3's `main` mounts no account save data; the island is in device save data (`MountDeviceSaveData`, `GetDeviceSaveDataSize`, `ExtendDeviceSaveData`), which the host gives the account save's host directory and transaction (one player). A second mount name over a save already mounted shares that mount's transaction (`mount_ex`); it used to clone the committed directory again, over the first name's pending writes. Also `fs::Commit` with a `CommitOption` (its one flag clears the restore flag) and `GetSaveDataRestoreFlag` (never restored from a backup). A game whose `main` imports `MountSaveData` too gets a device save of its own (Mario Kart 8 Deluxe, [0100152000022000](../0100152000022000/README.md)); this game does not, so its islands stay where they are.
- **Resolved (PR #25): `fs::Commit(const char* const*, int)`, the mount-list overload.** The SDK's own code opened fsp-srv and aborted, leaving the save writes uncommitted. The host commits the list as one transaction when every name is the same host save; cache and temporary storage in the list are skipped, and a list of two separate saves is refused without committing either.
- Found in `main`'s imports before the first boot: the adjustable user clock (`time::AdjustableUserSystemClock::GetCurrentTime`, which the game's days follow) reads the time service's shared memory in the SDK (`MonotonicSystemClock::GetCurrentTime`, `sdk+0x4421cc`), which the host never maps, so it is the host's clock, as the user clock; `time::GetStandardSteadyClockCurrentTimePoint`, the free-function form of the steady clock; `oe::GetRegionCode` (from the language: Japan, the Americas, Europe or Hong Kong / Taiwan / Korea); the SDK 20 album calls `SaveAndShareScreenshot` with a share ID and `GetSharePostedLastUrl` (no album: they fail).

## Saves

- A save is not reached on the Mac: the game first saves when the player sleeps at the end of the first day (the `−` menu says "You can't save right now." before that). Until then it only probes for `main.dat`, `mainHeader.dat` and `Moved/personal.dat` in its device save data.
- On Android it is reached: in the second run by hand on the Ayn Odin 2 ([below](#android-through-the-first-night-on-the-odin-2)) the first save came the next morning, as island time began to follow the real clock, not when the player lay down. It wrote `main.dat` (10,161,808 bytes), `mainHeader.dat`, `landname.dat` and `Villager0/`'s files through the work directory, swapped in on `Commit`. The `−` menu's Save and end saved again ("Save complete."), and Continue loaded that save after a cold start.
- The first run by hand there stopped on the first evening, before the first night, with the same "You can't save right now."

## Movies

- The in-flight movie `Movie/OpeningMovie.webm` is VP9 1080p with Vorbis, demuxed by `movie::Extractor` and decoded on the host (`hle_movie.c`).
- **Resolved: the movie showed as a black screen with the loading icon, one picture every few seconds, while its sound decoder kept up.** The player holds every output it is offered (16 of 16 video, 59 of 64 audio), so how far its extractor can get ahead of the picture on screen is the video slots plus the 4 queued packets. With 8 video slots that was 0.4 s, and the audio behind the next video packet, whose clock the player follows, ran dry: 86 pictures in 25 s. With 16 or 32 slots all 1,960 play at 30 fps. A video decoder now has 16 (`MD_NOUT_VIDEO`); a slot is allocated when first used, so the 8 more cost 24 MB of NV12 at 1080p only in a player that holds them all.

## Files and shaders

- RomFS: 97,997 zstd files (`.zs`, most of them models), BWAV sound, a few Yaz0 SARC archives (`.szs`). The profile's `romfs_marker` is `Movie/OpeningMovie.webm`.
- **Current:** 9,766 BNSH programs: `Shader/Park_UBER_All` (3,385) and the other bfsha / sharcb in `Shader/`, agl's `System/Agl/agl_resource` (3,206), particle effects in `Effect/` (2,522), models' own bfsha in `Model/`, layouts (bgsh) in `Layout/`, the game's keyboard in `Swkbd/` and 40 in `main`.
- The keyboard's ocean background has 8 geometry shaders, which do not translate; the name entry draws without them.
- Prewarm list: recorded from a fresh save through the counter, the movie and the arrival to the dock (965 lines).

## Screen shape (`DM_SCREEN_ASPECT`, current)

With `DM_SCREEN_ASPECT=1.43` (launcher setting *Picture shape*, offered because the profile has `screen_aspect on`; the host side is Breath of the Wild's, [its note](../01007EF00011E000/README.md#screen-shape-dm_screen_aspect-current)) the game renders a 1920x1344 picture of the screen's own shape instead of a 16:9 one: 16:9's sides with more above and below, the layouts in their own shape, and their edge widgets at the taller screen's edges. The stubs in `patches.py` (3.0.3 `main` only) each take one bit of `DM_SCREEN_ASPECT_SITES`:

- **Sizes** (bits 0-4, 7, 8). The platform config (vtable `main+0x47fa0e0`, no RTTI) hands out the display size three ways: the graphics create arg (`main+0x5c9b44`: +0x60 the display size from the table at `main+0x4076728`, +0x20 1280x720), slot 7's static display size and slot 8's static 1280.0 x 720.0, and slot 3's arg +0x14. Each takes the screen's height (width x the output's height / 1920, in integers: 1920 → 1344). Every frame `main+0x1527ebc` reads the resolution table's entry into the display buffer's crop and the 3D views' render scale: without its two loads (`main+0x1527f00`, `main+0x1527f2c`) the 1920x1344 window was cropped back to its top 1920x1080. The layouts' DrawInfo takes the framebuffer's size at `main+0x31cee60` / `main+0x31cee9c` for nn::ui2d's scissors and captures.
- **The view** (bit 5). The island camera keeps its vertical angle in radians at +0x2cc (0.384, 22 degrees) and reads it twice (`main+0x7a895c`, `main+0x7a89a8`); both reads, and the other camera path's half angle (`main+0x386a7a0`), are widened to 2 atan(tan(fovy/2) k), k = (16/9) / ratio. The aspect already follows the taller viewport, so before this the picture kept the vertical angle and lost a fifth of each side.
- **Layouts' projection** (bit 6). `nn::ui2d::DrawInfo::SetProjectionMtx` (`main+0x265100`) gets the layouts' perspective projection of a 16:9 rectangle (m0 1.545, m5 2.747, fovy 40 degrees); its y row is divided by the vertical stretch (layout height / 720) before the return, so layouts keep their shape, centred. Only a projection within 5% of 16:9 is divided: the game copies a DrawInfo's projection into another, and without the check the title's layouts were divided twice.
- **Edges and backgrounds** (bit 9, `duo_anchor.c`, run after `nn::ui2d::Pane::Calculate` has computed a pane's global matrix, `main+0x27d570`). The game's panes are its own classes (no RTTI): what draws is told by its vtable's DrawSelf (slot 21) against `nn::ui2d::Pane::DrawSelf` (`main+0x27e2c8`, empty). A full-screen or full-height background leaf is stretched to the taller screen; a widget (the topmost pane of its chain that is not a null pane, alignment or layout-sized container) beyond 230 units from the middle towards a corner (|x| 400 or more), or 270 nearer the middle, moves out by the extra half-height with its children. Layouts are 1920x1080 (793 of 959) or 1280x720; the distances are in 720 units and scaled by the root pane's size. Layouts placed by their root (balloons over characters) are left alone. `_duo_anchor` (patches.py) is the code of `duo_anchor.c` built with the command in the comment above it.

Checked on a Mac (M1, macOS 27, headless, an island save) against 16:9: the 3D view keeps 16:9's sides and shows more above and below; the title's version, press-A and settings lines, the island's ZL icon, clock, minimap and button guides sit at the new edges; the pockets, the phone (its backdrop's light band stretched) and the save dialog keep their shape. On an iPhone Duo (iOS 27.1) an earlier build of this looked right to its player. Open: an NPC's dialog box stays at its 16:9 place (-320), not at the taller screen's bottom edge; the phone's blurred backdrop shows a hard edge at its top left.

## Mac routes

- From a fresh save, `DM_KEYS="40:A*51/2,145:B*12/0.4,151:Down,152:Right*4/0.4,154.5:A,155.5:Right*4/0.4,158:A,159:Down*2/0.4,160.5:Left*2/0.4,162:A,163.5:Plus,167:A*4/2,175:Plus,178:A*146/2,179:Plus*30/4"` names the player "tom" on the keyboard, takes the defaults, watches the movie and reaches the dock at about frame 14,100 (470 s).

## Android

Played by hand on an Ayn Odin 2 (Snapdragon 8 Gen 2, Adreno 740 with Qualcomm's driver V@0676.32, Android 13), 2026-11-10 23:03 to 2026-11-11 00:13, about 70 minutes from a new game in an empty save folder.
- **The build:** the host from `main` at `cfc2c254`, the game files from the build folder (shader chain 5).
- **Input:** adb gamepad keys and stick events.

- **The stretch:**
  - Nook Inc.'s counter: the name on the game's own keyboard, the birthday, the look, the hemisphere and the island choice;
  - the in-flight movie, and the arrival at the dock;
  - Tom Nook's welcome with Timmy, Tommy and the two villagers;
  - the tent, placed by the beach (a first spot refused as too close to the ledge);
  - helping both villagers place theirs;
  - the evening gathering at the plaza at sunset, and Tom Nook's request for 10 tree branches for the campfire.

  Nothing wrong was seen in the picture, and there was no crash and no hang.
- **Where it stopped:** at the branches, with one picked up.
  - A branch is picked up only with the player on or facing its tile, which a slow look-and-press loop over adb mostly misses, so the run ended there.
  - The game saves only after the first night (see Saves), so a save made on Android and Continue are not checked.
  - A person with a controller can finish the first night in about 15 minutes; nothing in it needs quick reactions.
- **30 fps throughout, as on the Switch:**
  - 127 windows of 30 s over 64 minutes, median 30.0 fps; 4 windows at 29.0–29.2, each with one long frame;
  - frame p50 33.3 ms, p95 34–46 ms;
  - GPU p50 about 4 ms at the counter, 7–15.5 ms on the island.
- **Sound:** `DM_AUDIO_DUMP` of a fresh launch (67 s): music and effects from about 15 s on, at −27.5 dBFS RMS on the front pair, peaks −3 dBFS. No underrun in 693 callback reports.
- **Seen: three GPU submissions cut short at the arrival.** Three `[RED]` lines `submission N took 412–998 ms on the GPU, its end timestamp never written (the driver's hang recovery cut it short)`.
  - **When:** 596, 608 and 618 s into the run, as the island's first frames were drawn.
  - **What each was:** a list of 65 draws, and it made one long frame (1031, 450 and 451 ms).
  - **After:** the game went on normally, with none in the next 54 minutes.
  - **Since then:** the same GPU hang, found and reset by the kernel's GPU driver, not at a load ([the GPU hang on the Odin 2](#android-the-gpu-hang-on-the-odin-2)).

## Android: through the first night on the Odin 2

Played by hand on the same Ayn Odin 2 (Adreno 740, Qualcomm's driver V@0676.32, Android 13) on 2026-11-11, 14:14 to 16:26, from a new game in an empty private save folder (`DM_SAVE`). The host was from `main` at `c03656cf` (`ANDROID_CHAIN` 6), swapped into the device's own APK, with chain 6's shaders; input was adb gamepad keys and stick events.

- **The stretch:**
  - the first run's way to the evening gathering at the plaza;
  - Tom Nook's 10 branches, and the 6 apples he asks for next (shaken from trees);
  - the welcome party at the campfire, with the island's name entered on the game's keyboard and the toast;
  - the night: Tom Nook's word before bed, the camping cot placed from the pockets in the tent, and Rest;
  - K.K.'s dream, then the next morning: Tom Nook at the tent, the NookPhone, the Nook Mileage Program and the 49,800 Bells for the getaway package;
  - "From now on, island time will match the flow of time in the real world", then free play.
- **Save and Continue:** the game's first save came as that morning's messages closed, then the `−` menu's Save and end ([Saves](#saves)). After a cold start the title showed the island's plaza, and A loaded the player at the tent, at the real time.
- **Frame rate** (target 30, as on the Switch), from the log's frame counter: 30.00 fps over the 2 h 12 min session (237,163 frames in 7,907 s); between the long-frame stamps 30 s or more apart, none under 29.89. Frame p50 33.3 ms in all 792 of the log's 300-frame windows, p95 at most 40.8 ms. 79 long frames, none over 368 ms; GPU p50 10.1 ms on the island.
- **Sound:** no underrun in the session's 1,415 callback reports. `DM_AUDIO_DUMP` of the cold start (97 s): silent through the boot, then the title music and the island's sound at −16 to −31 dBFS RMS.
- **Red lines:** one GPU submission cut short by the driver's hang recovery, 613 s in as the island's first frames drew (336 ms on the GPU, a 368 ms frame), the first run's fault again (three there): a GPU hang the kernel resets ([below](#android-the-gpu-hang-on-the-odin-2)). The game went on; no crash or hang of its own.
- **For a person playing on:** the tent takes the night only with the camping cot placed (pockets, Place Item), lain on, and Rest chosen. After the dream the player stays lying on the cot until a button gets them up. A tree's shaken item is picked up with the player standing on it (the item's name shows) and Y pressed.

## Android: the GPU hang on the Odin 2

Checked on 2026-11-13 (job q319) with the host from `main` at `50a256fe` (0.8.7-1) and its own build of the game files (shader chain 6; `main` rewritten to the same bytes as before). Every run started a new game in an empty private save folder, with its own pipeline cache file (`DM_PIPELINE_CACHE`: empty for the first run on each phone, kept for the later ones), and the [Mac route](#mac-routes)'s `DM_KEYS` with A pressed every 2 s until 698 s. Each run reached the dock, and the island's first frames, at frames 12,339–12,402 (about 414 s), as the Mac did at frame 12,354.

- **What the red line measures** (`gpu_slow` in `dm/android/gfx_vk.c`). The completion thread waits for each submission's fence once the one before it has completed. A wait of 200 ms or more is logged with the submission's last passes, draws and dispatches. "Its end timestamp never written" means the fence signalled but the timestamp the submission writes at its end can't be read (`VK_NOT_READY`): the GPU stopped working on it before its end.
- **It is a GPU hang, and the kernel's GPU driver finds it.** The Odin's `dmesg` and `/sys/class/kgsl/kgsl-3d0/snapshot/` can be read over adb; the OnePlus Pad 3's can't. At run o2's red line the kernel logged:
  - `adreno-gen7-gmu 3d68000.qcom,gmu: MISC: GPU hang detected`;
  - `kgsl kgsl-3d0: ve.android:game[27602]: ctx 17 ctx_type VK ts 251795 status 00E794E7 dispatch_queue=-1 rb 1af0/1d74 ib1 000000404935C8B4/0000 ib2 0000004049356070/0000`;
  - `GPU snapshot created at pa f1f00000++0x3e69ec`, then `Suspended GMU`: the GPU was reset.

  The snapshot's fault count went from 6 to 7. With the driver's fault-tolerance policy (`ft_policy` 0xC2: replay, skip the command, throttle), the rest of the hung submission is dropped and the game's context goes on, so the device isn't lost. The kernel keeps its first snapshot until something reads it. The one still there on 2026-11-13 was from 2026-11-10 23:13:57, about ten minutes into [the first run by hand](#android). It names the game's process, the same context (17) and the same size of command buffer (780 words), so that run's red lines were the same fault.
- **Runs** (the first island frame is the first one with over 6,000 draws):

  | Run | Device | Pipeline cache | First island frames | GPU hang |
  |---|---|---|---|---|
  | o1 | Ayn Odin 2 | empty | frame 12,339, 1,226 ms (60 pipelines made) | none in 730 s |
  | o2 | Ayn Odin 2 | o1's | frame 12,363, 88 ms | one, 572 s in: 995 ms on the GPU and a 1,029 ms frame (frame 17,091), 159 s after the first island frames |
  | o3 | Ayn Odin 2 | o1's and o2's | frame 12,361, 85 ms | none in 873 s |
  | o4 | Ayn Odin 2 | o1's to o3's | frame 12,368, 94 ms | none in 781 s |
  | p1 | OnePlus Pad 3 | empty | frame 12,347, 807 ms | none in 720 s |
  | p2 | OnePlus Pad 3 | p1's | frame 12,402, 69 ms | none in 880 s |

  Every run held 30 fps (median of 30 s windows 30.00; lowest window 27.5 in o1, from the first island frames' pipeline builds, and 29.0 in o2, from the hang).
- **Where it comes: not at a load.** o2's came in a steady scene: the player standing at the dock, 2,705 draws a frame, 9 ms of GPU a frame. The hung submission was a 1280 x 720 pass and 64 of the island's draws (vertex shaders `5930533f8cf31c82`, `438b0d58b8eeb2c0`, `8214035a40a89d07`, `f7114c72220a0fc0`, `83e5385e0e0d4e7a` and others; the last one `83e5385e0e0d4e7a` with fragment `0073e35f6fe87884`, 25,266 vertices). No pipeline or shader was new in that frame. All five hangs so far came 572–618 s after the launch: 596, 608 and 618 s in the first run, 613 s in the second and 572 s in o2. In the runs by hand that was when the island's first frames were drawn; in o2 it was 2.6 minutes after them.
- **The Mac at the same moment.** Run m1 used the same route from a new game and ran alongside o2. Frame 17,091 (572.8 s) took 33.3 ms, with 1.9 ms on the GPU. After the island's loads (431 s) and up to 784 s, no frame took over 117 ms, and the log has no GPU error. The Mac draws the docked 1920 x 1080 picture, the phones the handheld 1280 x 720 one. At frame 17,090, run m2's picture is the Odin's scene at the same second: the dock, the airport and the player, in the same light. Each new game makes its own island and look, so only the scene can be compared, not the pixels.
- **On screen:** the picture stops for about a second, then play goes on, and o2's picture 148 s later is right. The frame shown after the stall lacks whatever draws of that submission the reset dropped. No video caught it: o3 recorded 500–860 s and o4 540–720 s, and neither run hung.

A GPU hang that the kernel resets, in three of six sessions on the Adreno 740, isn't one of the cosmetic faults Playable – minor issues allows, and the Mac has nothing like it. It keeps the game at Playable – major issues.

## Found on this game

General host changes first needed here; each applies to every game:

- **Android: a late AAudio data callback takes what the pacer's ring lacks from the audio renderer's queue** (`coreaudio_aaudio.c`): on an Ayn Odin 2 on AAudio's legacy path with a data callback 30 ms late every 2 s, 35 callbacks were padded with zeros (135 ms in 80 s on the island) before, none after: [a late data callback or pacer tick](../../../docs/reference/audio.md#android-a-late-data-callback-or-pacer-tick-dropped-sound-the-game-had-queued).
- **The host's shaders compiled when the host is built** (`dm/gfx/msl`, `host_msl_lib`): the HUD's `Compiled 2 shaders … 102.70ms` in play on an iPhone 17 Pro Max came from clear-triangle libraries compiled from source when a new room first issued a scissored clear; [The host's own shaders compiled from source at run time](../../../docs/reference/shaders.md#the-hosts-own-shaders-compiled-from-source-at-run-time).
- **Halfword jump tables padded to their largest entry** (`half_count` in `patch_nso.py`): [Halfword jump table landing pads sized to the largest entry](../../../docs/reference/rewriter.md#halfword-jump-table-landing-pads-sized-to-the-largest-entry).
- **Yields that make progress return at once** (`thr_yield` in `hle_thread.c`): [Yields that make progress return at once (`thr_yield`)](../../../docs/reference/threads.md#yields-that-make-progress-return-at-once-thr_yield) and [development.md, Power](../../../docs/development.md#power) ("Yield loops").
- **The game's own HIPC clients answered by host sessions** (`hle_sf.c`, `DM_SF_TRACE`), **device save data and shared transactions** (`mount_ex`), **`fs::Commit` with a mount list or a `CommitOption`**, and the adjustable user clock, region code and SDK 20 album and steady-clock calls: [Game-linked HIPC clients of system services](../../../docs/reference/services.md#game-linked-hipc-clients-of-system-services), [Device save data on the account save's directory](../../../docs/reference/services.md#device-save-data-on-the-account-saves-directory), [`fs::Commit` with a list of mounts](../../../docs/reference/services.md#fscommit-with-a-list-of-mounts), [Steady clock, clock snapshots and user clocks](../../../docs/reference/services.md#steady-clock-clock-snapshots-and-user-clocks), [Region code from the language](../../../docs/reference/services.md#region-code-from-the-language), [Album calls fail with no album](../../../docs/reference/services.md#album-calls-fail-with-no-album).
- **16 output slots for video movie decoders** (`MD_NOUT_VIDEO` in `hle_movie.c`): [Video output slots: how far the extractor gets ahead](../../../docs/reference/movies.md#video-output-slots-how-far-the-extractor-gets-ahead).
- **Functions with function constants specialized by the pipeline** (`fn_spec`): the HUD's `Compiled 2 shaders … Consider adopting MTL4Compiler` during play on the iPhone with Metal 4 pipelines on: [Functions with function constants specialized by the pipeline, not the library](../../../docs/reference/shaders.md#functions-with-function-constants-specialized-by-the-pipeline-not-the-library).
- [The alpha test through a library of its own](../../../docs/reference/rendering.md#the-alpha-test-through-a-library-of-its-own): the island map's corruption (issue #73) improved with an alpha test in a downstream runtime (PR #430).
- **The game's view in the shape the game presents** (`front_picture_shape`; with `DM_SCREEN_ASPECT=1.43` the 1920x1344 picture was fitted into a 16:9 view on the phone): [The picture laid out in the shape the game presents](../../../docs/reference/rendering.md#the-picture-laid-out-in-the-shape-the-game-presents).

## Open

- Check the island map (issue #73) with the host's alpha test.
- `game.tsv` still describes the game as "In bring-up", while it plays on the iPhone.
- A save on the Mac (the next morning, after the first night) has not been reached; on Android it has ([Saves](#saves)).
- The GPU hang on the Ayn Odin 2 ([the GPU hang on the Odin 2](#android-the-gpu-hang-on-the-odin-2)): a GPU hang the kernel resets, in three of six sessions, always 572–618 s after the launch; not on the OnePlus Pad 3 or the Mac. This keeps the game at Playable – major issues. Next: find the draw. `DM_VK_MID_DRAWS=1` sends each draw in a submission of its own, so the red line names the hung one. Then find what happens about ten minutes in. The stretch already takes in the end of the opening (the next morning, when island time starts to follow the real time), which the tier asks of a game with no battle, race or level end.
- The keyboard's 8 geometry shaders do not translate.
