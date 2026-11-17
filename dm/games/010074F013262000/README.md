# Xenoblade Chronicles 3

Base title ID `010074F013262000`, update 2.2.1 (`v720896`), SDK 12.3.9,
Monolith's engine, NVN. Modules: `main`, `subsdk0` (NintendoSDK_movie), `sdk`.
The build IDs are in [game.tsv](game.tsv).

## Current status

Opening gameplay tested on an Apple-silicon Mac (2026-10-27). The compact layout
renders the title, terrain, characters, lighting and effects. New-game setup,
opening movies, the first enemy defeat, destination tutorials, walking and the
engagement-area collision boundary work. All 27,055 base/update shaders compile
for Mac and iOS. The unsigned IPA is built and its archive checks pass.

On Android, the cartridge's base game 1.0.0 is Playable – major issues: from a new game through the opening, four battles and the Everblight Plain to a save and Continue, at 30 fps in every window on the Ayn Odin 2 and nearly every one on the Galaxy Z Flip 7; the Moto Razr Plus 2023 plays the same route at 17–28 fps in battle ([Android](#android)).

iOS runtime testing, DLC gameplay and later-game allocation peaks remain open.
The supplied four DLC packs are included; their additional shaders have not been
integrated into the base/update shader census, so DLC rendering is unvalidated.

## Profile and memory

- The NPDM declares a 32 MB system resource. Memory is committed through address
  regions, without `SetHeapSize`. The profile uses a 5 GB guest block, including
  a 4.375 GB alias region, and the Switch application pool of `0xCD500000` bytes.
- `mem_map_limit on`: startup attempts 1 MB physical commits until allocation
  fails, then frees its probe. An unlimited host lets it map all 60 GB; enforcing
  the hardware pool makes that probe stop at the expected physical limit.
- **Address-space adaptation:** `nninitStartup` calls `main+0x128e58c`, reserving 60 GB.
  Its first arena reserves 4 GB + 128 KB (`main+0x1241860`), so reducing only the
  containing region cannot work. [patches.py](patches.py) changes that arena's
  bump increment, subtraction and capacity together, to 512 MB + 128 KB, and its
  containing region to 4 GB. All changes are gated by main build ID `82d187fe9ef9be921b8e1b1cddf7bd28`,
  also reported for 2.2.0 below.
  The SDK build takes the existing node-area and large-region alignment cuts.
- A 40-second diagnostic run with the original arenas had 2,092.4 MB committed:
  about 203 MB in the first arena, about 1.8 GB in subsequent windows, and a 64 MB
  allocation near the end of the reservation. Original-layout opening battle
  (`boot29wide`) commits 225.44 MB in the first arena. That leaves about 286 MB
  of capacity in the compact first arena; later-game peaks remain unvalidated.
- Compact runs stayed at the title for over ten minutes without an allocator
  failure. This validates the title allocation pattern, not later gameplay peaks.
- `pad_layout full` supplies both sticks and the D-pad.

### Update 2.2.0 and address-space evidence

[Issue #847](https://github.com/seedds/nxnative/issues/847) reports update 2.2.0
(`v655360`) with the same main build ID `82d187fe9ef9be921b8e1b1cddf7bd28`
and matching original bytes at all six allocator cut sites. Its running title
screen says 2.2.1. The existing build-ID gate therefore covers that main too;
the package version alone does not select these cuts. The report supplies no
SDK or movie-module build IDs, so the registered package and shader version in
`game.tsv` remain 2.2.1 pending a complete build survey.

The same reporter tested `mmap(PROT_NONE)` in a minimal app on an iPad mini
(A17 Pro, iPadOS 27.2), with no extended virtual-address entitlement: 60 GiB
failed with `ENOMEM`, while 5, 4 and 3 GiB reservations succeeded. Sizes between
5 and 60 GiB were not probed. This supports the address-space adaptation; the
full game's compact layout has not been tested on that iPad.

On an M2 Mac with 8 GB RAM and macOS 27.2, the reporter restored the original
six instructions and used `DM_ALIAS_SIZE=0x1000000000` (64 GiB). Title, opening,
first battle, character/camera movement and Expansion Pass menus ran for
1,173.6 seconds without an allocation failure (2,690 MB footprint, 2,734 MB peak).
This extends the evidence for the original layout on Mac. Later-game peaks,
iPad gameplay and DLC content beyond those menus remain unvalidated.

## Files and shaders

- The merged RomFS is 14.15 GB: 238 WebM movies, nine Wwise PCKs, and `bf3.arh` /
  `bf3.ard`. The ARH1 index has 76,445 files; the ARD is 9.05 GB. Archive entries
  and model/map streams use XBC1 (zlib and zstd).
- The full base/update split finds 27,055 shader keys: 8,201 vertex, 18,533 pixel,
  308 geometry and 13 compute, with 17,648 variation pairs. `main` supplies 204;
  archive assets supply 26,851. All compile for Mac and iOS with feedback entries
  (chain `7fbca6dd477d`, profile inputs `2342f43192c3`, after integration with main).
- **Resolved, extraction:** six boot shader stages live in one standalone SLCT
  at `main+0x14e01c0`, without an HCPS header. Control and code offsets are relative
  to the SLCT itself. Its control sections include assembly text and are aligned
  to eight bytes. The first pair's keys, `58e2e00d24c6f3ea` and
  `5d36d4144f49ec9a`, match runtime dumps exactly.
- Twelve more keys captured during startup match the extracted archive programs.
  Equal code can have different compiler identity bytes at control +0x7d0; all
  such keys are retained, with their original control bytes.
- **Resolved, extraction:** `/effect/cmn_assets.wipac` contains complete GLSLC
  outputs alongside HCPS programs. Reading them adds 914 base-game keys. Captured
  `6babc936d58fcd8f` (vertex), `7a7eb30dfb6187b5` (geometry) and
  `b540e3f8e28bd41e` (pixel) match these outputs exactly.
- **Resolved, translation:** vertex `4b98320a0f512efe` reads `SubgroupLtMask`
  through a component pointer; the general one-lane lowering now covers that form.
- **Resolved, title rendering:** the black foreground model had valid G-buffer
  colour and normals, but the separate IMAGE stencil descriptor read empty
  storage. The general sharing fix restores lighting (`boot17shared`, frame 1200).
- **Resolved, opening deadlock:** the output thread held the host decoder mutex
  while signalling the higher-priority input thread; the handoff waited for that
  input thread, which was blocked on the same native mutex. Annotating contended
  native waits restores playback (`boot18movie`, `mv01005100.webm`, 140 seconds).
- **Current, native audio setup:** a later real-audio run (`boot25full`) stopped
  presenting while `AKEventManager` joined the Apple audio-unit setup helper.
  A native join must release the guest core, just like a contended codec lock;
  `dm_host_thread_join` now preserves that scheduler accounting. Gated join,
  nesting and audio-cycle regressions pass. The sample does not establish that
  Apple audio-unit construction itself would remain blocked indefinitely.
  The corrected compact run continues for over ten minutes with real output
  `#0`, without the diagnostic watchdog or reported audio underruns. It does
  not reopen the output, so the earlier construction stall is not isolated.
- **Resolved, nested effect compression:** those four geometry/pixel keys, and
  vertex `d021f6a3763a9327` / pixel `1c6c6a015f6f3acf`, match GLSLC programs inside
  `/effect/fld/ma01a.wipac` after its additional `cmp\0zlib` layer is decoded.
  The expanded base/update census is 27,055 keys (8,201 vertex, 18,533 pixel,
  308 geometry, 13 compute), with 17,648 variation pairs.
- **Resolved, vertex work under discard:** `cf66cef403c57845` and
  `36710e69a07e849d` write bone palettes through storage buffers. Running their
  vertex work restores geometry (`boot21discard`), initially with large distorted
  surfaces. `boot22full` reproduced this with the complete shader set. A native
  post-vertex capture of the first `4f6b050575d4de0b` draw gives 2,080 finite
  vertices inside the clip volume. The remaining corruption was the primitive
  ordering failure below.
- **Resolved, feedback:** effect geometry `c01e3ade8e15a842` captures eleven
  interleaved generic outputs (172 bytes per vertex). The general feedback path
  now decodes these byte component counts, captures geometry output and saves
  GPU draw counts. Native regression checks pass; `boot23feedback` runs this path
  without missing feedback calls or capture-entry errors.
- **Resolved, exploding meshes:** the game orders bone-palette work with
  `ORDER_PRIMITIVES` barriers (`0x21`). The host only split render encoders for
  fragment barriers after storage writes. Enforcing primitive ordering removes
  the exploding model parts (`boot24barrier`).
- **Resolved, terrain allocation:** `boot27draw` frame 4000 replays every
  recorded program bind/draw, with no map shader or rejected draw. CPU probes
  (`boot28cpu`) find the map owner rejected before visibility because its core
  buffer at `+0xd18` is null. The 390 MB allocation (`boot30alloc`) needs a
  512 MB virtual window. `boot32region` at `main+0x12429a4` records bump
  `0xe46c0000` → `0x1046c0000`, beyond the compact 4 GB container, before any
  physical mapping. The heap's byte limit is disabled (`heap31.bin`). Reducing
  the first arena's reservation from 1 GB to 512 MB frees that window; this
  retains the Switch physical-memory pool and all map work. The original-layout
  battle's reservation sequence predicts 159.56 MB of remaining outer space;
  corrected compact gameplay (`boot33compact512`) confirms bump `0xf6070000`,
  a 512 MB + 128 KB first-arena capacity and about 225 MB committed there.

## Mac validation and remaining work

`boot33compact512` uses the complete shader set, the corrected compact modules
and real audio for over ten minutes. It defeats the first enemy, advances through
the destination and tips tutorials, then walks across Everblight Plain to the
engagement boundary.
`run/compact33_terrain.png`, `compact33_move.png` and `compact33_walk.png` retain
these milestones. Alias mappings settle near 2,653–2,655 MB during this sequence.
Later areas, saves/reloads and DLC have not been exercised.

The unsigned iOS app compiles with signing disabled. The IPA is 20,629,976,490
bytes (19.21 GiB), contains all 27,055 base/update shader libraries and four
mounted DLC directories, and passes every ZIP CRC. It has no code signature or
provisioning profile. No device installation or runtime test was performed.
The delivered IPA is the original validated `bab889ccd129` build; integration
with main rebuilt both platforms' libraries but did not replace that artifact.

After integrating main at `6f58a7d`, `boot34main` repeats the first enemy defeat
and destination tutorial with the combined host and shader chain. The full Metal
suite, all 48 Python test files, all 178 iOS source compile checks and the pinned
lint/type ratchet pass. A fresh launch after shader metadata generation accepts
the new library/table stamps. The all-local-game regression harness was started
but stopped during its first baseline compilation, before any comparison
completed; no cross-game comparison coverage is claimed for that sweep.

### Send-off particles (GitHub issue #1011, resolved)

A soldier's remains in Everblight Plain, A to send off. The steering field is a 10x10x10 RGBA8 3D texture
(format `0x25`, flags 0, pool flags `0x64`) whose 4,000 bytes of texels the CPU wrote before first use. It is
now taken from its storage ([entry](../../../docs/reference/nvn-objects.md#block-compressed-storage-the-cpu-wrote-before-first-use)).
A/B on two Mac hosts that differ only by that change, the same save, 120-frame screenshot bursts from the
press of A, spark pixels counted against the frame before it:
- before: no sparks on the remains before A (3 pixels in the box), and none rise after it; they sink out of
  sight at once;
- after: sparks sit on the remains before A (1,281 pixels), then rise up the rock. Over the second burst their
  centroid moves up by 153 px and their top edge from y 709 to 418, while the ground band empties
  (1,358 to 81 pixels).

The issue also names a 10x10x10 RGBA16F field; it did not appear in this scene.

## Mac development files

The supplied folder is `/Volumes/S790/nsp/Xenoblade Chronicles 3/`. Extraction,
the shader packages and isolated hosts are in `010074F013262000_build/`; analysis,
private keys, Python tools and isolated runs are in `010074F013262000-work/`.
`run/boot.py` accepts a log name, duration and environment assignments. Diagnostic
logs `boot01` / `boot02` show the original allocator failure; `boot03wide` through
`boot07wide` retain the wider-reservation experiments. `boot07wide` ends cleanly
after 40 seconds; it uses an empty shader map and is not a visual test.

Use `patched_512/` with `tools/dmhost_512` and `shader/fx_cache/` for the validated
Mac layout; the earlier `patched_compact/` has the insufficient 1 GB first-arena
reservation. `DM_NVN_SHMAP` points to this profile's `shader_map.tsv`.
On the title/new-game screens press A. The auto-attack tip advances with R and
closes with A; stand near an enemy to auto-attack and use the left stick to walk.
During a movie, Plus then a held X skips it. The diagnostic watchdog is off when
`DM_WATCH` is absent; setting it to `0` still enables its default stall threshold.

## Android

First run on Android, 2026-11-11. The cartridge's base game 1.0.0 (v0), packed as a game app (`build_android_from_nsp.py --game-apk`, a 422 MB APK with the 14.8 GB RomFS as its OBB) from DroidPlay Core `main` at `1c06a5c4`, without draft #203 (the cloud shaders' else-if). Two phones:
- a Galaxy S25 Edge: SM8750, Adreno 830 with Qualcomm's driver 0x80320001, Android 16;
- a Galaxy Z Flip 7: Exynos 2500, Xclipse 950 with Samsung's driver 0x6000215, Android 16, unfolded.

Both in handheld mode, driven over adb with `DM_CTL`.

- **The build:** a profile derived from 2.2.1's. The split found 26,907 shaders (2.2.1 has 27,055), all translated to SPIR-V, none failed.
  - 1.0.0's 60 GB allocator reservation fits Android's 64 GB alias region as it is (`[mem] address region 60.00 GB at alias +0.06 GB`), without 2.2.x's cuts.
- **No lost device:** the triage expected the Adreno to lose the device a few seconds into the title until #203 lands. On this `main` it didn't: the S25 Edge ran the title, the opening and the first battle for about 11 minutes without it.
- **What was played on both phones:** the auto-save notice, the title, New Game, Normal, the opening's cutscenes, the first battle against the Agnus Attacker (the auto-attack tutorial, won with auto-attacks and arts), the Destinations and Tips tutorials, and walking on the Everblight Plain.
- **The picture, both phones:**
  - **Resolved, characters drawn only in cutscenes:** in the opening's cutscenes Noah, Eunie, Lanz and the soldiers drew; in battle and in the field the party didn't, only the effects of his arts. Gameplay skins the characters with vertex-only draws under rasterizer discard outside transform feedback, which the replay left out; they draw since those run as dispatches ([Draws under rasterizer discard that write memory](../../../docs/reference/rendering.md#draws-under-rasterizer-discard-that-write-memory-android)).
    - Both phones log one `[RED] transform feedback with vertex shader 0b8bcbe86791ef65, which has no capture variant: nothing captured`. It wasn't the characters' skinning; it stays.
  - **Resolved, the world striped:** the titans on the title screen, the plain's ground, its rocks and the wreckage were drawn as black-and-white zebra stripes instead of their textures; the sky, the UI, the fire and the arts' effects looked right. The lighting reads a depth-stencil buffer in stencil mode, and the Vulkan replay read its depth there ([Stencil-mode texture slots read the stencil](../../../docs/reference/rendering.md#stencil-mode-texture-slots-read-the-stencil-android), found on Xenoblade Chronicles 2). With the fix (2026-11-12, the S25 Edge, this game app with only the host swapped), the title's titans and the plain are drawn with their textures.
  - The Mac's 2.2.1 runs drew the terrain and the characters; 1.0.0 wasn't run on the Mac.
  - On the S25 Edge, Qualcomm's driver also logged 842 "Failed to link shaders" lines with no `[RED]` from the host; the Z Flip 7's driver logged none.
- **Frame rate:** the target is 30 (the Switch's rate).
  - **S25 Edge:** 30 s windows from 24.5 to 30.1, median 30.0, GPU 18–21 ms.
  - **Z Flip 7:** 21.4 to 30.1, median 30.1, GPU 15–20 ms.
  - The dips come from loads and the first battle's first pipeline builds: 1,349 pipelines in 13.9 s on the S25 Edge, 1,463 in 37.7 s on the Z Flip 7.
- **Sound:** a stereo `AudioOut`, no underrun on either phone. The `DM_AUDIO_DUMP` has signal (RMS about 550–2,000). Not listened to by a person.
- **Memory:** 3.56 GB PSS on the S25 Edge (1.03 GB graphics) and 4.20 GB on the Z Flip 7 (1.92 GB graphics). Neither was killed.
- **Save:** no save commit within the stretch (the game saves automatically, but not before this point), so saving and Continue are untested.
- **Not tried:** past the first walk on the Everblight Plain, and the other devices.
- **2026-11-12, both fixes, on the Galaxy S25 Edge:** the sprint's game app with a host from the stencil-mode fix's head (which has the rasterizer-discard dispatches), a new game on Easy (so auto-attacks carry the battles over adb) in an empty private save.
  - **Played:** the title, the opening's cutscenes, the first battle on the Everblight Plain (the auto-attack tutorial, won), the Destinations and Tips boxes, the walk toward the next marker, and the second battle up to its arts tutorial. Arts presses reached the host but didn't register (the tutorial's count stayed 0/5) at that frame rate, so the run stopped there.
  - **Picture:** the titans, the plain's ground and rocks, the spires and the Levnis are drawn with their textures, and the party, the soldiers and the enemies are drawn in battle and on the field. The log names the stencil-mode slot once (`531d637463e53486`, format 130).
  - **Frame rate:** the title and the opening's cutscenes 27–30 fps (GPU 17–24 ms). The first battle 5.6–5.8 fps (frames of 179–193 ms, GPU 63–66 ms, about 1,430 draws), the plain 6.4–7.1, the second battle 5.9–6.9. The GPU's clock was at its lowest step, 160 MHz, in 78% of the samples: it is busy for about a third of each frame, and the frame is bound by the replay's CPU side. Per gameplay frame: 117 discarded-draw dispatches, 76 writes through scratch copies, 45 home copies made, 45 waits on the current submission, 45 write-backs (about 45 MB) that take the completion thread about 16 ms, and 4 ms waiting for a free command buffer. The rasterizer-discard fix's first XC3 run measured the same battle at about 5 fps without the stencil-mode fix, so the stencil-mode fix doesn't change the speed.
  - **Red lines, memory:** the transform-feedback line above, once; PSS 3.44–3.51 GB with about 0.55 GB swapped; no crash or kill in 27 minutes.
- **Gameplay speed (2026-11-12):** the 5.6–7 fps above were the replay waiting for the GPU.
  - 53 of its 55 waits a frame were character draws reading a skinned vertex buffer as a storage buffer. The buffer sat inside a home range two buffers wide, learned from overrunning vertex streams, and 48 bytes off the storage buffer alignment in its copy ([An indexed draw's vertex stream read only as far as the write-back it starts in](../../../docs/reference/rendering.md#an-indexed-draws-vertex-stream-read-only-as-far-as-the-write-back-it-starts-in-android)).
  - With that fixed, the first battle and the field run at about 12 fps at the same 160 MHz, and the replay thread's CPU time drops from 78 to 38 ms a frame.
  - The rest of the waits: a draw's storage buffer write read by a later draw ([A draw's storage buffer write read by a later draw after a barrier](../../../docs/reference/rendering.md#a-draws-storage-buffer-write-read-by-a-later-draw-after-a-barrier-android)), and the first draw after each submission ([Pending writes read on the GPU by the first draw of a submission](../../../docs/reference/rendering.md#pending-writes-read-on-the-gpu-by-the-first-draw-of-a-submission-android)). With all three, the S25 Edge plays the first battle and the plain at 30 fps (GPU 24.6–24.9 ms at 342–389 MHz). The skinning draws' writes past the alignment still go through scratch copies.
- **2026-11-12, by hand on the Galaxy Z Flip 7 and the Moto Razr Plus 2023 (q270):** a game app built from `main` at `021fc6b6` with #367, #368 and #369 (merged since), a new game on Normal, driven over adb with `DM_CTL`.
  - **Played on both:** the opening's cutscenes; four battles, all won: auto-attacks; the arts tutorial (5 arts with X/Y/B); arts; the Lv 4 Agnus machine with a Role Action (behind it, then Edge Thrust) and, on the Z Flip 7, the Talent Art. Then the 20-minute cutscene chain, the plain by day toward Colony 9, System → Save to a new file (Chapter 1, 0:43–0:47), a cold start, Continue back at the same spot, and walking on.
  - **Z Flip 7** (Xclipse 950): 30 fps in 280 of 294 ten-second windows (frame p50 33.2 ms, GPU 18.6 ms a frame at 240–509 MHz).
    - The slower windows are the first battle's pipeline builds, the arts' effects, loads, and the plain by day. There the GPU is 85–95% busy at 404–509 MHz, and some windows drop to 23–28 fps.
    - The first battle matches the Mac's gallery frame (2.2.1): the terrain, the characters and the effects. Faces and hair in close-ups are right.
    - PSS 3.4–4.9 GB, up to 0.96 GB swapped; no crash or kill; no underruns (not listened to).
  - **Razr Plus 2023** (Adreno 730, V@0615.98): the cutscenes run at 30 fps; the battles and the plain at 17–28 fps (frame p50 36–60 ms).
    - Its overnight update (Android 16, security patch 2026-09-01, build W1TZS36H.24-71-2-4) left the GPU driver as it was: 0x80267062, V@0615.98, Compiler EV031.36.08.33.
    - Its GPU is busy 37–41 ms a frame, with about one wait a frame left (a draw's write in a submission in flight), so the 730's GPU bounds it. Its clock isn't readable from the shell.
    - Menu presses need holding for 0.2 s there. PSS 2.5–3.1 GB, about 1–1.4 GB swapped; no crash; no underruns.
  - **Red lines:** one a session, `transform feedback with vertex shader <key>, which has no capture variant`, naming the first such shader (`0b8bcbe86791ef65` on the Razr, others on the Z Flip 7). What it leaves out isn't found.
  - **Saves:** the save slots' thumbnails are black.
- **2026-11-12, the same route on the Ayn Odin 2 (q270):** a game app from `main` at `3d9f479c`, which has #367–#369 and the later replay changes (compute dispatches ordered by the game's barriers, 687b7358). Adreno 740, driver V@0676.0 (0x802a4000, Compiler E031.41.03.36).
  - **Played:** the same: the opening, the four battles (arts, the Role Action and the Talent Art), the cutscene chain, the plain by day, System → Save to a new file (Chapter 1, 0:41), a cold start and Continue back at the same spot.
  - **Frame rate:** 30 fps in every one of 276 ten-second windows, the battles included. GPU median 11.5 ms a frame, the clock at 680 MHz throughout.
  - **Other:** one red line (`0b8bcbe86791ef65`, as above); PSS 2.9–3.3 GB, little swap; no crash; no underruns (not listened to).

- **2026-11-17, the Google Pixel 9 Pro XL (Mali-G715, Android 17; q358):** the game app from the branch that runs vertex-stage storage writes in the fragment stage ([Vertex-stage storage writes run in the fragment stage](../../../docs/reference/rendering.md#vertex-stage-storage-writes-run-in-the-fragment-stage-android)), a new game on Normal into the first battle.
  - **With `DM_VK_VS_FRAG=0`:** the opening's cutscenes draw their characters, but in the battle's intro and the battle Noah's party and the Agnian soldiers are missing (Mali has no vertex-stage stores, so the skinning draws under rasterizer discard are left out).
  - **By default:** all are drawn, in the battle too. 30 fps in the opening (GPU p50 24–25 ms); 24.5 fps over the battle intro's first minute, while the characters' pipelines were built.
  - **Red lines:** the transform-feedback line above, and `pipeline creation failed: VkResult -3` for two pipelines with a geometry shader (`b5f17d227ab8ecd0` / `9250108618485c07` and `f5d9900f9a887bad` / `abdad6e24b16e263`), both ways, so not from that change. Not investigated.
  - **Not tried:** past the auto-attack tutorial, save and Continue.

## Found on this game

- [Vertex-stage storage writes run in the fragment stage](../../../docs/reference/rendering.md#vertex-stage-storage-writes-run-in-the-fragment-stage-android): the battle's characters on Mali; see [Android](#android).
- [Same-pass feedback through a read-only image](../../../docs/reference/rendering.md#same-pass-feedback-read-through-a-snapshot): lighting shader `15816d836842bfb5` reads its R32Uint zone mask between barriers. Reading the previous frame’s mask made the Colony Sigma cutscene alternate in brightness. Resolved ([#1009](https://github.com/seedds/nxnative/issues/1009)): measured on an Apple M2 in Events Theater chapter 1's Colony Sigma battle, 8 of 10 bursts flipped brightness every frame before (sign-flip fraction 0.83–1.00), at most 0.28 of 11 after. The extra snapshots cost GPU time there: p50 50.4 → 53.0 ms, 18.3 → 17.2 fps. Open: if the shader loads the mask at its own pixel, a tile read (as sampled textures have) would avoid that cost; it needs the translator to record image loads' coordinates.
- [Monolith ARH1, XBC1 and HCPS/SLCT shader containers](../../../docs/reference/shaders.md#monolith-arh1-xbc1-and-hcpsslct-shader-containers).
- [Subgroup operations rewritten for a one-lane subgroup](../../../docs/reference/shaders.md#subgroup-operations-rewritten-for-a-one-lane-subgroup).
- [IMAGE usage does not split texture storage](../../../docs/reference/rendering.md#image-usage-does-not-split-texture-storage).
- [Rasterizer discard preserves vertex storage writes](../../../docs/reference/rendering.md#rasterizer-discard-preserves-vertex-storage-writes).
- [Stencil-mode texture slots read the stencil](../../../docs/reference/rendering.md#stencil-mode-texture-slots-read-the-stencil-android): the world's zebra stripes on Android; see [Android](#android).
- [An indexed draw's vertex stream read only as far as the write-back it starts in](../../../docs/reference/rendering.md#an-indexed-draws-vertex-stream-read-only-as-far-as-the-write-back-it-starts-in-android): the skinned buffers' storage buffer reads, 53 waits a frame; see [Android](#android).
- [Pending writes read on the GPU by the first draw of a submission](../../../docs/reference/rendering.md#pending-writes-read-on-the-gpu-by-the-first-draw-of-a-submission-android): about 5 drains of the GPU a frame on the S25 Edge; see [Android](#android).
- [A draw's storage buffer write read by a later draw after a barrier](../../../docs/reference/rendering.md#a-draws-storage-buffer-write-read-by-a-later-draw-after-a-barrier-android): two waits a frame; see [Android](#android).
- [Primitive barriers order vertex storage writes](../../../docs/reference/rendering.md#primitive-barriers-order-vertex-storage-writes).
- [Transform feedback captured by a vertex entry point](../../../docs/reference/rendering.md#transform-feedback-captured-by-a-vertex-entry-point): interleaved outputs and geometry capture.
- [A host lock held across a same-core handoff](../../../docs/reference/threads.md#a-host-lock-held-across-a-same-core-handoff): contended native decoder waits must release a handoff.
- [oe calls answered without an applet proxy](../../../docs/reference/services.md#oe-calls-answered-without-an-applet-proxy): enabling album-image notifications dereferenced the absent SelfController.
- [The SDK's address-region node area](../../../docs/reference/memory.md#the-sdks-address-region-node-area) and [1 GB alignment of regions of 1 GB and more](../../../docs/reference/memory.md#1-gb-alignment-of-regions-of-1-gb-and-more).
- [Block-compressed storage the CPU wrote before first use](../../../docs/reference/nvn-objects.md#block-compressed-storage-the-cpu-wrote-before-first-use): uncompressed 3D particle steering fields, taken from their storage too (GitHub issue #1011).
