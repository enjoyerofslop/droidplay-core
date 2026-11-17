# Xenoblade Chronicles 2

Base title ID `0100E95004038000`, update 2.1.0 (`v983040`), SDK 3.5.0,
Monolith's engine, NVN. Modules: `main`, `subsdk0` (NintendoSDK_movie), `sdk`.
The build IDs are in [game.tsv](game.tsv).

## Current status

Opening gameplay tested on an Apple-silicon Mac (2026-10-27). The game boots,
mounts all six supplied add-on content entries, renders its title and menus,
completes King Crustip and reaches Argentum in Chapter One. Field tutorials,
walking and the menu work. The general queue raster-origin fix restores the
lighting and geometry without a game-specific rule or rewriter cut.

All 15,572 shaders build for Mac and iOS. The unsigned IPA has been rebuilt
with this fix: 18,331,894,202 bytes (17.07 GiB), 78,482 ZIP entries, all ZIP
CRCs valid, with all shader libraries and six DLC mount entries. It has no
code signature or provisioning profile. iOS runtime testing, DLC gameplay
and later areas remain open.
System → Save and a restart into Argentum were exercised during bring-up;
the save was created with the isolated winding diagnostic and reloaded with
the production path. The final fixed host also loads it and walks in Argentum.

On Android, 2.1.0 is Playable – major issues on the Galaxy S25 Edge, from a new game through King Crustip's battle and Argentum to a save and Continue (2026-11-12): 30 fps through the opening and the battle, 10–30 fps in Argentum; the dark stipple pattern over the characters and much of the world it had then is fixed ([Android](#android)).

## Profile and memory

The NPDM declares no system resource. Startup sets a `0xC9400000`-byte heap
inside the profile's 4 GB guest reservation. The application pool remains
`0xCD500000`, with 256 MB each for alias memory and stacks. No game-specific
rewriter cuts are used. `pad_layout full` supplies both sticks and the D-pad.

## Files and shaders

The merged base/update RomFS contains 28,376 files (13.05 GB), including 154
WebM movies and `bf2.arh` / `bf2.ard`. The ARH1 index names 40,733 packed assets.
The existing Monolith archive reader decodes its XBC1-compressed HCPS programs.
The complete split, including the supplied DLC, finds 15,572 keys: 4,726 vertex, 10,830 pixel,
three geometry and 13 compute, with 12,028 variation pairs. Six stages come
from the standalone SLCT at `main+0x9ce0e0`; the other 15,566 come from assets.
Of those keys, 2,834 first appear in DLC assets, 12,732 in the base/update
assets and six in main. Full Mac and iOS compilation passes, including all
three geometry shaders. DLC gameplay remains unvalidated.

## Rendering

**Current, iPadOS pipeline validation ([#702](https://github.com/seedds/nxnative/issues/702)):**
the reported opening-sequence abort is an invalid `Invalid / Depth32Float_Stencil8`
pipeline, not a confirmed memory kill. The general renderer correction keeps
[combined depth-stencil formats paired](../../../docs/reference/rendering.md#combined-depth-stencil-formats-stay-paired-in-passes-and-pipeline-keys)
in physical attachments and live, predicted and historical pipeline keys while
preserving the unused plane. The log uses the legacy pipeline compiler with
TBDR off; its original live pass remains unrecorded. The reporter confirms
`b925591522f0` reaches cutscenes and normal gameplay on iPad. The subsequent
smearing investigation below identifies another cause of malformed pipeline
formats; playing and skipping the opening with the complete correction remain
separate device checks.

**Current, angle-dependent character smearing (2026-10-30):** on an M4 iPad
(iPadOS 26.6.2), Rex's silhouette and body smear while stationary, change with
the viewing angle, and can clear when another character walks nearer. The
`b925591522f0` log uses legacy pipelines, threaded encoding and TBDR off. Frame
2610 contains 76 var122 skinning draws: 74 execute, but passes 874367 and 874378
skip the 319-vertex and 56-vertex updates after pipeline rejection. Their
passes begin with eight color targets and depth/stencil `260/260`, while the
failed keys contain depth `0x01010104` and stencil `260`.

A device write watchpoint identifies `udc_note_draw`. The nine-element `g_sd`
ends exactly where `g_pass` begins in this build. Its tenth, 56-byte stencil
record overlaps the pass cache: write flags at offsets 34/35 change depth
`0x00000104` to `0x01010104`. The general correction is
[ten attachment records for eight colors and depth-stencil](../../../docs/reference/rendering.md#ten-attachment-records-for-eight-colors-and-depth-stencil).
Disabling only the unwritten-store tracking (`g_udc_ab=0`, TBDR still off) in
the running app clears Rex at the same camera angle; this is a diagnostic
control, not the shipped fix. AddressSanitizer reproduces the old overflow
without game files. The corrected ledger and lookahead arrays pass the GPU
regression; the rebuilt permanent correction still needs iPad testing.

**Resolved, hard-edged dark boxes in the indirect lighting (GitHub issue #643):**
indoors and under decks a world-space box was much darker than the scene,
darker with the time of day (Goldmouth Return Port), and elsewhere a lighter
translucent rectangle. The indirect-lighting pass (`shd_lgt.wishp` var4.3,
`468bd9c28161136f` / `93d0e7a7c14cdfb0`) reads a per-pixel list of up to four
light probes from a 640×360 RG32Uint target (slot 7): the index bytes in x,
then the probes' boxes from `fp_c6` and their cubes from the cube-map array at
slot 5. The probe boxes build that list with logic operation OR (`lo=7`): the
writers (`shd_lgt.wishp` var3: `ef07fd3d5046164f`, `aa9a0923524b7e46`,
`2717a0aa47361bbd`, `d95874c161deb77d`, `f24b58784a33adea`) put their index into
the next free byte and set a bit in an R32Uint image, and every other fragment
writes 0. The host drew them with COPY, so the list was replaced fragment by
fragment. Hand-patching the five writers to OR into their target removed the
boxes; the general fix is [Logic operations through a library of its own](../../../docs/reference/rendering.md#logic-operations-through-a-library-of-its-own).
The reporter's lead,
3 of 21 64×64 cube-map arrays with few non-zero bytes, counts guest memory the
CPU wrote and does not bear on it.

**Resolved, stretched geometry streaks (GitHub issue #643):** for a frame, a
character's mesh stretched into a wedge or sheet reaching another character
(the Nopon by Bonbon in the Argentum Bazaar, the pink Nopon and the diving-suit
NPCs at Goldmouth Return Port), and other characters missing in that frame;
about one frame in a hundred near the crates at the spawn. Skinning runs as
vertex-only draws (`shd_post.wishp` var122: `fa456dcb6b05c2ce`,
`0cf988e8a8ea2aee`, `bce3419b07cb2f3d`, `3fb8ebc13eb37ad7`) that write blended
bone matrices, 48 bytes per slot at `gl_VertexIndex`, from slot 0 of one
384,000-byte storage buffer, which the character's draws read; each is preceded
by ORDER_PRIMITIVES (bits `0x1`) and followed by `0x21`. One frame had 91 such
draws, 43 in the same Metal pass as the previous character's draws. Fix:
[Ordering barriers before a draw that writes](../../../docs/reference/rendering.md#ordering-barriers-before-a-draw-that-writes);
with it, 104 a frame take a barrier inside the pass, none ends it, and no streak
showed in 288 frames at the spawn. The `cb_extent.tsv` lead of the report is
not it: the 256 shaders it counts are the `c1_full.tsv` rows, whose
`gl_VertexIndex` reads go to the shader's own constant table (a full 64 KiB
each), and the slot-1 extent it names is for reads at fixed indices.

**Resolved, dark and inside-out geometry:** the game creates its queue before
switching the device from the default lower-left origin to upper-left
(`boot31properties`). Mario Wonder sets upper-left before queue creation
(`wonder33order`). The native SDK's `DeviceSetWindowOriginMode` at
`sdk+0x2bfac0` only stores the mode. Its queue initializer at `sdk+0x2c54ec`
calls `0x2dba3c` at `0x2c594c`, which calls `0x2db3b0` at `0x2dc2ec`.
That helper emits `SET_WINDOW_ORIGIN` at `0x2db670`–`0x2db6a0`: the lower-left
device state selects `0x801004eb`, upper-left selects `0x800004eb`. This helper
has no caller outside queue initialization. Later viewport construction at
`0x2d5ca0` reads the device's current mode. These SDK offsets refer to build
`867b345248124ef3d70e3202e221e978`.

The [general queue raster-origin fix](../../../docs/reference/rendering.md#queue-initialization-captures-the-raster-origin)
preserves this distinction without a game rule or SDK-version gate.
`boot34queue` loads Chapter One and walks in Argentum with correct geometry
and lighting. The earlier global winding experiment was rejected because it
broke Mario Wonder; the queue-state fix passes Wonder's full smoke route.

Before the fix, indirect-lighting pair `468bd9c28161136f` / `93d0e7a7c14cdfb0`
writes nothing. Its depth test is off, stencil comparison is
ALWAYS, culling is BACK and front-face winding is CW. Native post-vertex
capture gives `(-1,-1,0.5,1)`, `(3,-1,0.5,1)`, `(-1,3,0.5,1)`; the byte
indices are `00 01 02`. Those vertices also occur unchanged at
`main+0x9d3c30` in build `f77f1559371c0ec6d50e78774ac59d95`.
The material buffers and reflection cubes contain valid images. Highlighting
the five direct-lighting material shaders reaches their expected geometry;
highlighting the indirect-lighting draw does not. A native D16 depth-equality
fixture passes tags 1 through 8, ruling out that earlier hypothesis.

The engine initializes all seven reusable polygon states to CW at
`main+0x6ff6a4` in that build. The SDK's original polygon binder and
window-origin command encoding were compared with Mario Wonder's SDK 16
driver: both encode the same front-face and upper/lower-left origin rules.
The difference is initialization order, rather than SDK-version semantics.

**Resolved, title logo:** the game deliberately hides `IMG_Logo` when its save
buffer does not begin with `0x0300a15b` (`main+0x489140` through `0x489180` in
the build above). A fresh save buffer begins with zero. Forcing visibility in
an isolated diagnostic draws the 800×312 BC3 logo correctly; no such patch is
retained. More decisively, `boot28save` creates `bf2savefile.sav` through
System → Save after reaching Argentum, and `boot29reload` uses unmodified game
modules and the production rendering path: its frame 600 shows the logo and
Chapter One's blue-sky title, and frame 1800 is back in Argentum. The absent
fresh-save logo is not a texture-loading or language-selection defect.
One startup layout shader,
`da21866ffe3a9d7a`, reads a 3D texture through a declared 2D slot; its relation
to a visible defect is not established.

## Validation

- Python tool suite: 292 tests pass, 16 skipped. The complete Metal graphics
  suite passes. The queue-origin fixture passes 389 checks in each encoding
  mode, including actual culling, fragment facing and attachment coverage.
  All 178 iOS source compile checks pass.
- `boot34queue` validates saved-game loading and walking with the fix.
  After refreshing the standard NSP Mac build and all 15,572 Mac libraries,
  `boot38packagedmac` repeats loading into Argentum with that build's host.
  `boot37play` starts fresh with real audio, completes the opening battle,
  plays Chapter One's cutscene, skips it through the game's confirmation,
  advances the field tutorials and walks in Argentum (`field37.png`). It
  runs for fifteen minutes without a guest crash. A separate twenty-minute
  windowed/audio run (`boot36play`) remains in the battle tutorial.
  The fifteen-minute run reports fifteen audio underruns during concurrent
  validation/build work; its timing is not used as a performance measurement.
- Full local regression report:
  `0100E95004038000-work/regress-harness/reports/20261027-161811-check/report.md`.
  No shaders or interfaces change. Mario Wonder and Hades smoke routes pass.
  Xenoblade 2's screenshots show the corrected lighting, but this sweep's
  timing and frame-deadline checks fail while other validation builds run.
  The isolated repeat,
  `0100E95004038000-work/regress-harness/reports/20261027-174408-check/report.md`,
  passes both games' frame deadlines without new red lines: Xenoblade 2
  stays at 30.1 fps (frame p99 33.46 → 33.42 ms), Wonder at 59.7 fps
  (19.21 → 17.31 ms). Xenoblade 2 is labelled REGRESSION only for the
  intended screenshot change; Wonder's image passes unchanged. Balatro is outside
  the harness's GL chain support. Bayonetta 2/3 and OCTOPATH TRAVELER I/II
  lack package files required by the baseline shader build; Yoshi's Crafted
  World has no profile in the baseline revision. These are unchecked.

- **Current, [#424](https://github.com/seedds/nxnative/issues/424) controls
  (2026-10-27):** the unchanged `1963fab7` host reproduces the startup texture
  warning described above and peak footprints of 1,522 and 2,070 MB, versus
  the first baseline's 564 MB and the patched run's 1,274 MB. A byte-identical
  HEAD copy also misses both scheduled captures. These flags are not specific
  to the analyzer fixes. The 65-second startup route reaches different
  workloads between runs, so its timing difference is inconclusive. An
  alternating fresh-save idle-title benchmark, two runs per host with the same
  shaders, also has unstable timing: p99 134.51/712.67 ms on HEAD and
  117.44/180.31 ms patched. This does not establish a patch-induced slowdown
  or a stable performance baseline. Reports are in the NSP work folder's
  `issue424-controls/` and `issue424-idle-benchmark/results-valid/`; the earlier
  idle benchmark with an incorrect shader-cache path is marked invalid.

### Controller motion review

**Current (PR #351, 2026-10-27):** the full regression comparison of `6d1e07b`
with `f978e28` reaches the tutorial on both hosts, with unchanged shaders and
no new red lines, but flags windowed frame timing. The candidate repeatedly
logs one-second display-link stalls; the baseline also stalls and misses both
requested screenshots. Its windowed visual comparison is unavailable.

Two cached-archive A/B repeats in reverse order reproduce the large spikes on
both hosts, with nearly identical median frame rates. A second two-run pair
using direct presentation (`DM_PRESENT_LINK=0`) has matching 33.24 ms median
frame times and overlapping p99 ranges. These controls do not reproduce a
consistent slowdown from the gyro change. Windowed performance remains open
for a controlled foreground run; see [silent display links](../../../docs/reference/frame-rate.md#present-thread-rescues-a-silent-display-link).
`DM_WINDOW=0` still creates a window: the Mac front end checks whether the
variable exists.

The original flagged report is `nx-harness/reports/pr351-review/` on the game
drive. The cached-archive controls are in this game's
`0100E95004038000-work/regress/pr351-windowed-ab/` and
`0100E95004038000-work/regress/pr351-direct-ab/`; their public tables are in
[PR #351](https://github.com/seedds/nxnative/pull/351).


## Integration with main

Rebased onto `75065df` (2026-10-27), whose shader chain is `157e1b96c8da`.
The complete Metal suite, all 389 queue-origin checks in both encoding modes,
347 Python tool tests (16 skipped) and all 178 iOS source compile checks pass
on the integrated code. The Python app-bundle fixture needs access to macOS's
icon tools outside the sandbox.

All 15,572 Mac shaders rebuild with that chain. `boot40main` uses the refreshed
host, libraries and merged `RomFS.img`, loads the saved game into Argentum
and exercises walking; its frame 1500 shows the correctly lit field.

The final rebase includes `349abd0`, with the same shader chain. Repeated
checks pass: 389 queue-origin checks in each encoding mode, 73 pipeline
archive checks, 79 prewarm checks on each Metal path, 108 affected Python
tests and all 178 iOS compile checks. `boot41latestmain` loads Argentum and
walks with that host, with no crash or new red lines. The impact report is
in `0100E95004038000-work/regress-harness/main-integration-impact/`.

The delivered IPA above retains the gameplay-tested `9bdef355` base plus this
queue-origin fix, with shader chain `7fbca6dd477d`; it was not replaced during
this integration. The regression reports above also refer to that base.

## Mac development files

The supplied folder is `/Volumes/S790/nsp/Xenoblade Chronicles 2/`. Extraction,
rewritten modules and shader builds live in `0100E95004038000_build/`; binary
analysis, bounded runs, saves and logs live in `0100E95004038000-work/`.
`run/boot.py` accepts a log name, duration and environment assignments.
`boot01.log` records the initial startup and clearing frames; it stops because
that diagnostic launch did not yet have a shader map configured.

## Android

First run on Android, 2026-11-11. The cartridge's 2.1.0 (v983040, the registered build), packed as a game app (`build_android_from_nsp.py --game-apk`, a 158 MB APK with the 14 GB RomFS as its OBB) from DroidPlay Core `main` at `1c06a5c4`. Two phones:
- a Galaxy S25 Edge: SM8750, Adreno 830 with Qualcomm's driver 0x80320001, Android 16;
- a Galaxy Z Flip 7: Exynos 2500, Xclipse 950 with Samsung's driver 0x6000215, Android 16, unfolded.

Both in handheld mode, driven over adb with `DM_CTL`.

- **The build:** the dump has no DLC, so the split's DLC-only path made a shader-only derived profile (the registered module fixes kept). It found 12,738 shaders (the registered tables, with the DLC's, have 15,572), all translated to SPIR-V, none failed.
- **What was played:**
  - **S25 Edge:** the title (no logo, the game's own fresh-save behaviour), New Game, Easy, the opening (the Cloud Sea, the salvage dive, Rex and Gramps), King Crustip's tutorial battle (won: auto-attacks, the EXP and SP shown), and the scenes after it.
  - **Z Flip 7:** the same into King Crustip's tutorial.
- **Resolved, no characters drawn on both GPUs:** Rex, Gramps and King Crustip never appeared (fixed: see the next item).
  - The cutscenes frame them over an empty deck, an empty cabin ("No worries, Gramps!") and empty sky. In battle, only the target marker, the red ring and the damage numbers show where the crab is.
  - The world, the ship, the Cloud Sea, the opening's titans, the UI and the effects draw.
  - No `[RED]` line on either phone, and the host's pipeline counters show only the draws a first build leaves out. On the S25 Edge, Qualcomm's driver logged 272 "Failed to link shaders" / "Shader compilation failed for shaderType: 4" lines; Samsung's logged none.
  - The Mac draws the characters (its Argentum runs).
- **Cause and fix (2026-11-11):** the game skins every character with a vertex-only draw under rasterizer discard, without transform feedback
  (`shd_post.wishp` var122, `fa456dcb6b05c2ce`, `0cf988e8a8ea2aee`, `bce3419b07cb2f3d`, `3fb8ebc13eb37ad7`: the bone matrices into one 384,000-byte
  storage buffer; and `bd4b7f58c16f180f`, which updates vertex buffers), and the Vulkan replay left every such draw out
  ([Draws under rasterizer discard that write memory](../../../docs/reference/rendering.md#draws-under-rasterizer-discard-that-write-memory-android)).
  The S25 Edge's "Failed to link shaders" lines are the Adreno's shader module identifiers' cache misses, not failed pipelines.
  - **Moto Razr Plus 2023** (Adreno 730, V@0615.98, Android 16), one host: with `DM_VK_RAST_DISCARD=0` the three are missing as above; with the fix,
    New Game on Easy through the opening (Rex diving in the salvage suit, Rex and Gramps on deck) into the tutorial battle (Rex auto-attacking King
    Crustip), all drawn, a steady 30 fps (battle frame p50 33.3 ms, p95 33.9 ms, GPU 27.1 ms), no red line.
  - **Galaxy S25 Edge**, with the fix: the same route, all three drawn, a steady 30 fps (battle frame p50 33.2 ms, GPU 22.1 ms), no red line.
  - The Z Flip 7 was not run with the fix.
- **Resolved, a dark stipple and stripes (2026-11-12):** with the characters drawn, their skin, hair and clothes showed dark, blocky speckles in close-ups,
  and much of the world (crates, walls, the rock spires on the deck) carried the same pattern; the Mac's frames are clean. The deferred lighting classifies
  pixels by stencil bits 3 and 4 (`3a65a75970c5ac32`, `68bd4ad78d01642b`), read through a stencil-mode view of the depth-stencil buffer, and the Vulkan replay
  read its depth there ([Stencil-mode texture slots read the stencil](../../../docs/reference/rendering.md#stencil-mode-texture-slots-read-the-stencil-android)).
  It wasn't Adreno-only: the Pixel 9 Pro XL's Mali-G715 drew the same. With the fix, the S25 Edge's battle frames and the Pixel's rock spires are clean, 30 fps. The Galaxy Z Flip 7 (Xclipse 950) too: the game app at v0.8.2, then with the fix's host, each from a new game to King Crustip, has the stipple, then is clean in the same frames, median 30.1 fps both (q255).
  ![Xenoblade Chronicles 2 on a Galaxy S25 Edge at v0.8.2, before the fix: Rex at Goldmouth Return Port, a stipple pattern over the walls and crates](../../../docs/images/android/xenoblade-chronicles-2-galaxy-s25-edge.jpg)
  ![Xenoblade Chronicles 2 on a Galaxy Z Flip 7 with the fix: King Crustip on the ship's deck, drawn clean](../../../docs/images/android/xenoblade-chronicles-2-galaxy-z-flip-7.jpg)
  The Pixel drew no characters either way then (no `vertexPipelineStoresAndAtomics` on the Mali, so the skinning draws were left out; resolved, see the next item).
- **Resolved, no characters on Mali (2026-11-17):** on the Pixel 9 Pro XL the skinning draws now run in the fragment stage ([Vertex-stage storage writes run in the fragment stage](../../../docs/reference/rendering.md#vertex-stage-storage-writes-run-in-the-fragment-stage-android)). One host, New Game on Normal into King Crustip's tutorial: with `DM_VK_VS_FRAG=0` the diver, Rex, Gramps and King Crustip are missing; by default all are drawn, matching the S25 Edge's frames of the route, 30 fps both, GPU p50 19.3–21.1 ms against 18.8–19.8 ms without the characters. The title's `13900f41cec4d4bc` (a full-screen pass storing four constants into a storage buffer) is rasterized and writes from the vertex stage: its writes are made first in the fragment stage and the draw then runs without them (67 such draws in 8 minutes, none lost, no `[RED]` line left).
- **Frame rate:** 30 fps (the target), every 60 s window 29.6–30.1 on the S25 Edge over 12.5 minutes, frame p50 33.2 ms; GPU 18–23 ms on the S25 Edge, 12.5–13 ms on the Z Flip 7.
- **Sound:** a stereo `AudioOut`, no underrun on either phone. The `DM_AUDIO_DUMP` has signal (RMS about 640–1,360). Not listened to by a person.
- **Memory:** 2.38 GB PSS on the S25 Edge (0.77 GB graphics) and 2.31 GB on the Z Flip 7. Neither was killed.
- **Save:** not tried in that run (System → Save opens in Argentum: see the run of 2026-11-12 below).
- **Not tried:** past King Crustip, and the other devices.
- **2026-11-12, Playable – major issues from a new game on the Galaxy S25 Edge, at v0.8.2.** The game app rebuilt at v0.8.2 (shaders as before, the host new), played by hand over adb with `DM_CTL` presses, each batch after a check that the game ran and held the focus.
  - **Played:** New Game on Normal; the opening (the Cloud Sea, the salvage dive, Rex and Gramps); King Crustip's tutorial battle (drawing the weapon, auto-attacks, the arts tutorial, Anchor Shot on X), won; the scenes after it; the arrival at the Argentum Trade Guild, its landmark and controls tutorials, and walking at Goldmouth Return Port; System → Save (Rex Lv 2, Chapter 1, 0:17:34, Argentum Trade Guild); a cold start, the title now with Continue, and Continue back at the same spot, walking on.
  - **Frame rate:** 30 fps (the target) through the opening, the battle and the scenes after it, 29.9 over 15.5 minutes with the worst 30 s window 28.9, GPU 21.8 ms. Argentum depends on the view: looking into the town (2,200–2,800 draws a frame) 10–16 fps with GPU 38–62 ms a frame; looking out over the docks (500–700 draws) 30 fps at GPU 20–24 ms; about 20 fps over its 6.5 minutes in the two sessions. There the replay makes about 54 skinning dispatches a frame, with 16 scratch copies, 13 home copies, 4 waits on submissions in flight and 19 write-backs (7 MB); which of these costs the GPU time isn't measured.
  - **Picture:** a dark stipple and stripe pattern over the characters' skin, hair and clothes in close-ups and over much of the world (crates, walls, Gramps's scales, the rocks on the deck); the Mac's frame of the same spot in Argentum has none. The xc2 session's S25 frames from before v0.8.2 show it too. 919 draws were skipped while their pipelines were first built; no `[RED]` line.
  - **Sound, memory:** sound running, no underrun (not listened to); PSS 1.8 GB at the title, 2.4 GB in the opening, up to 3.3 GB in Argentum; no crash, no kill.
  - **Not tried:** battles after the first, Argentum's shops and the rest of the chapter, the other devices.
  ![Xenoblade Chronicles 2 on a Galaxy S25 Edge: Rex sitting by the cabin after King Crustip's battle, the stipple pattern over his face, arms and clothes](../../../docs/images/android/xenoblade-chronicles-2-galaxy-s25-edge-close-up.jpg)

## Found on this game

- [Ten attachment records for eight colors and depth-stencil](../../../docs/reference/rendering.md#ten-attachment-records-for-eight-colors-and-depth-stencil): the iPad character smear and malformed skinning pipeline formats.
- [Combined depth-stencil formats stay paired in passes and pipeline keys](../../../docs/reference/rendering.md#combined-depth-stencil-formats-stay-paired-in-passes-and-pipeline-keys): combined storage on iPadOS; see [Rendering](#rendering).
- [Monolith ARH1, XBC1 and HCPS/SLCT shader containers](../../../docs/reference/shaders.md#monolith-arh1-xbc1-and-hcpsslct-shader-containers).
- [Queue initialization captures the raster origin](../../../docs/reference/rendering.md#queue-initialization-captures-the-raster-origin).
- [Logic operations through a library of its own](../../../docs/reference/rendering.md#logic-operations-through-a-library-of-its-own): the light probe lists; see [Rendering](#rendering).
- [Ordering barriers before a draw that writes](../../../docs/reference/rendering.md#ordering-barriers-before-a-draw-that-writes): the skinning draws; see [Rendering](#rendering).
- [Draws under rasterizer discard that write memory](../../../docs/reference/rendering.md#draws-under-rasterizer-discard-that-write-memory-android): the skinning draws on Android; see [Android](#android).
- [Stencil-mode texture slots read the stencil](../../../docs/reference/rendering.md#stencil-mode-texture-slots-read-the-stencil-android): the deferred lighting's stipple on Android; see [Android](#android).
- [Vertex-stage storage writes run in the fragment stage](../../../docs/reference/rendering.md#vertex-stage-storage-writes-run-in-the-fragment-stage-android): the skinning draws on Mali; see [Android](#android).
- [Present thread rescues a silent display link](../../../docs/reference/frame-rate.md#present-thread-rescues-a-silent-display-link): the motion review's windowed timing comparison is confounded by repeated stalls; see [Controller motion review](#controller-motion-review).
