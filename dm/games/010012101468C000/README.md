# Metroid Prime Remastered (`010012101468C000`)

The base game 1.0.0 (v0), SDK 12.3.1, Retro Studios' own engine (RFRM forms, the engine of [Donkey Kong Country: Tropical Freeze](../0100C1F0051B6000/README.md) in a little-endian form), NVN. Modules `main`, `subsdk0` (the SDK's movie library) and `sdk`. `main` exports its own C++ names (`CGraphicsDeviceContext`, `NMaterialArchive`, `CSceneProxy` …), so its functions can be named from its dynamic symbols. RomFS is 341 files, 6.7 GB: `MaterialArchive.arc`, `MiscData.pak`, `StartupUI.pak`, `KernelMPRT.pak`, the preload packs under `Preload/` (`DuplicateData.pak` alone is 1.5 GB), `UI/MPR1/UIMP1.pak` and a `.pak` per room under `Worlds/`.

Brought up on the Mac: boot, the title, the save slot and difficulty menus, a new game's opening (the briefing, the gunship's approach) and the first room of the Frigate Orpheon. Played for about a minute on the Frigate's landing deck: walking, turning and firing, with the HUD, the hint texts and the lock-on markers. 60 fps (frame time p50 16.67 ms; the long frames are loads and first uses of pipelines), no red line but one (below); the audio renderer produces sound (71,573 of the first 72,000 rendered frames not silent; not listened to); memory levels off at about 4.3 GB resident and 2.6 GB of Metal allocations once the Frigate is loaded and stays there (frames 14,700 to 32,700). Not yet tested on the iPhone.

## What it needs

| Setting | Where | Why |
|---|---|---|
| 4 GB layout: `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | [`game.tsv`](game.tsv) | no virtual address memory: `nninitStartup` binary-searches its heap from 4.5 GB down and settles on 3.11 GB in the 3.25 GB heap region, as Tropical Freeze does ([memory.md](../../../docs/reference/memory.md#a-heap-request-beyond-the-heap-region-returns-resultoutofmemory)) |
| `mem_used 0x6600000` | `game.tsv` | the rewritten modules are larger than the default 64 MB of used memory |
| `sys_resource 0x0` | `game.tsv` | the NPDM declares 0 MB of system resource |
| `modules main subsdk0 sdk` | `game.tsv` | every module `main` imports from |
| `shader_groups` (`/Worlds/`, `/Preload/`, `/UI/`) | `game.tsv` | the shader split's groups ([Shaders](#shaders)) |
| [`dual_want.tsv`](dual_want.tsv) | profile | `ab809a7456f59614` writes colour output 1 that a pipeline blends as the second source |

No rendering key: what the game needed beyond the profile is general ([Found on this game](#found-on-this-game)).

## Shaders

- **Resolved: an empty split** (only the startup archive's 8 shaders). Retro's forms are little-endian here, a pack's resource list sits in a `TOCC` form with 52-byte entries, and four vertex stages carry GLSLC data past their control section's own length: [SNVN program records in Retro Studios' RFRM forms](../../../docs/reference/shaders.md#snvn-program-records-in-retro-studios-rfrm-forms). 40,483 shaders: 20,233 vertex, 20,233 pixel, 12 geometry and 5 compute, from `MaterialArchive.arc` (1,141 stages), the 778 `MTRL` resources of the packs and the startup archive in `main` (`main+0x1d0e7a4`, 8 stages: the title logo's quads); 20,233 vertex / pixel pairs. The split takes about 6 minutes (most of it reading `DuplicateData.pak`), the Mac libraries about 17.
- **Resolved: `[RED] shader … reflection conversion failed: … c0 dynamic index c0+0x630`** and `Shader instruction Pixld is not implemented` in the build, `nvnProgramSetSampleShading` unimplemented. The game shades per sample: 438 of `MaterialArchive.arc`'s 562 pixel shaders read the sample index (`PIXLD.MYINDEX`), and 30 of them look up GLSLC's sample position table in the driver segment and pack the position into an integer G-buffer target: [Pixel `c0 + 0x620`: the sample index and GLSLC's sample position table](../../../docs/reference/shaders.md#pixel-c0--0x620-the-sample-index-and-glslcs-sample-position-table). Every render target met so far is single-sample (1600×900 docked).

## Rendering

- **Resolved: the world black, the HUD drawn over it** (the gunship invisible in the opening, the Frigate's hull and rooms black in play; menus, text and effects fine). The scene target held the whole room after the opaque passes (`DM_NVN_TEXSNAP`), and a full-screen pass (`1b17e5a457135e3b`) turned it black: the volumetric fog, which samples a 100×57×64 RGBA16F froxel volume at each pixel's depth and blends it over the scene. The volume is filled by `1cbc19f806a08166`, 5,700 points drawn with rasterization off whose vertex shader writes the 3D image; the host skipped that draw because only storage buffer writes counted, so the volume stayed zero (colour × 0 + 0): [Rasterizer discard preserves vertex storage writes](../../../docs/reference/rendering.md#rasterizer-discard-preserves-vertex-storage-writes). The profile keys `tex_flush_upload` / `tex_dirty_upload` changed nothing (no CPU writes it: its guest memory stays zero), so the profile does not set them.

## Current

- `vertex attribute format 0x27 (8 / 16-bit integer) feeds a float input` (RGBA8UI): converted on the CPU at replay, as for [Need for Speed: Hot Pursuit Remastered](../010029B0118E8000/README.md); no visible effect.
- **Open: the orange floor strips on the Frigate's deck are missing on an M6** ([GitHub issue #783](https://github.com/seedds/nxnative/issues/783); reported as orange noise on an M2 and in Ryujinx on a Mac). They are alpha-tested in the depth pre-pass (`4b87459ed2e526f3`, `bc7e3ae479c4bffa`: discard where mask² × instance alpha < 0.25) and coloured with depth `EQUAL` (`5bb21ce49ca5ba6b`, `df351b403930b318`). Every pixel is discarded, so the buffer keeps the floor's depth, the strip's colour draw is nearer than the buffer by more than 16 ulps, and `EQUAL` fails. Depth is not the cause: with the discard removed, plain `EQUAL` draws the whole strip, and lowering the threshold to 0.20 draws nothing. Unknown: whether the mask (ASTC 5×5) or the instance alpha (attribute 2's `w`) reads wrong, or the game means to hide them; not compared with the Switch. Loosening `EQUAL` to `LEQUAL` draws them, but only by bypassing the alpha test.
- **Open: Samus fully black in a Tallon IV room** (the same issue, on an iPad, with a build from before the lean geometry path kept the layer). She is lit on the Frigate's deck on main; there is no save past the Frigate to test Tallon IV.
- A translucent cyan panel with a grid stands between two red lights where Samus lands, and fills the left of the view as she walks past it: drawn in the 3D scene, apparently one of the Frigate's holograms; not compared with the Switch.

## Mac routes

The work folder is `<NSP folder>/010012101468C000-work`: `host.sh` copies the worktree's `dm/host/dmhost` to `build/mprhost`; `run/boot.sh <log> [secs] VAR=value…` runs it on the NSP script's build (`010012101468C000_build`: the RomFS image, the rewritten modules, the shader libraries in `shader/cache`); `run/ctl.sh press|snap` drives a run started with `DM_CTL=run/ctl.txt`, and `run/at.sh <log> <frame> <ctl lines>…` sends control lines once the run passes a frame (for `memdump`, `texlist`, `passdump`). `tools/sym.py main <offsets>` names `main` offsets by its dynamic symbols; `tools/a64.py` disassembles.

- From a fresh save: `DM_KEYS=f1600:A,f2200:A,f2800:A,f3600:A,f4200:A`: the title (press A), the start of the menu, save slot 1 (New Game), difficulty (Normal), the save message; the briefing and the gunship's approach follow, and the game takes control on the Frigate Orpheon at about frame 12,000 (Samus's visor up, energy 99). Skipping the opening was not tried, and there is no save yet past the new file.

## Android

First test on Android, 2026-11-12, from `main` c6920f36 with the guest block 32 MB-aligned. The cartridge's 1.0.0 was packed with this profile: 40,483 shaders translated, none failed.

- **Too big for a game app:** the APK would have to carry 2.66 GiB of shaders, and one holds 1.94 GiB. It was installed through a DroidPlay carrying only this game's host, then the phone's own DroidPlay was put back.
- **The boot crash:** before the change, the Pixel crashed on the main thread at the first `nnMain` steps (`addr=0x6aaac0 pc=sdk+0x53c48c`): [The guest block 32 MB-aligned (Android)](../../../docs/reference/memory.md#the-guest-block-32-mb-aligned-android).
- **Moto Razr Plus 2023** (Adreno 730, Qualcomm's driver 0x80267062, Vulkan 1.1.128): from a new game with the [Mac route](#mac-routes)'s frame-keyed presses, through the briefing and the gunship's approach, into control on the Frigate Orpheon's landing deck. Walked forward over adb to the lock-on tutorial.
  - **Frame rate:** 60.0 fps in every 30 s window, GPU about 10 ms a frame.
  - **Memory:** 2.0 GB PSS in the game process, 0.26 GB of it swapped out.
  - **Red lines:** none since q356 (before it, `virtual pool remap … failed: 5` 6 times: the light-probe volumes' pools, now one `[nvn] virtual pool … could not be aliased` line).
  - **Picture, against the Mac at the same points:**
    - **Resolved: the force fields drew as a jumble of blue triangles and blocks** instead of a translucent cyan panel (the landing deck's door, and the panel beside the lock-on tutorial), with the lights beside them pink or green and the floor strip a different colour each run. Qualcomm's driver linked only the first element of the fragment shaders' input array (`in vec4 _50[16]`, read at a run-time index to pick a UV set: 7,390 of the game's fragment shaders), which matches none of the vertex shader's separate outputs: [A shader's attribute array: its elements past the first not fetched or not linked (Android)](../../../docs/reference/rendering.md#a-shaders-attribute-array-its-elements-past-the-first-not-fetched-or-not-linked-android). The door field is vertex `2052c1dde364833f`, fragment `8a9e8ae4d53c4a82`, in the translucent pass after the scene's blurred copy (q335, 2026-11-13; q354, 2026-11-17 on `main` 2a376a35: the same jumble before, drawn as on the Mac after). `DM_VK_POS_ROWS=1`, `DM_VK_COPIED_DEPTH=0` and `DM_VK_ASYNC_PIPE=0` changed nothing; Turnip (T30) drew the fields right with main's host. 60.0 fps either way.
    - **Resolved: the arm cannon was a black silhouette,** on Turnip too. Two causes, both general: two of its pass's three draws (vertex `9b97258ddcf23736` and `099c3ff54bb28029`) read attributes 0, 1, 4, 5 and 6 through an input array and were given attribute 0 alone (the attribute-array entry above; its light strips drew after that, q354); and its body, like every model the game lights from its light probes, read a black ambient colour. Each frame a vertex shader under rasterizer discard (`f7800d296c68645b`) samples six 512x256x368 light-probe volumes (BC1 and BC6H 3D textures, a sparse virtual pool each) into an 80-byte record per object, which the materials' fragment shaders (`60840532d150e97a`, `a99a06e6c78d6a20`, `eb10d82ae5a756df`) read from a storage buffer at `c0 + 0x144`. On Android those volumes were images of zeros: [Textures of a sparse pool read through its page table on Android](../../../docs/reference/nvn-objects.md#textures-of-a-sparse-pool-read-through-its-page-table-on-android) (q356, 2026-11-17: green with yellow strips and the brown glove, as on the Mac; black again with `DM_VK_SPARSE_TEX=0`; 60 fps either way).
    - The deck's yellow floor strip shows, which an M4 Pro Mac doesn't draw either ([Open](#open)).
- **Pixel 9 Pro XL** (Mali-G715, Arm's driver 0xd803000):
  - The title is nearly black: only some of its particles are drawn.
  - Two shaders write storage from the vertex stage, which the Mali can't, and two pipelines fail (-3). A 368 MB image is over the image limit.
  - About 3 minutes in (frame 10,500), the driver crashed building a pipeline in the background (`libGLES_mali.so+0x1de07f4`, vertex `9f31f616f65b098b` .z, fragment `2ab0d59de3ce1cc8`).
- **Not tried:** sound, saves, past the Frigate's first room, the other devices.

## Found on this game

General host and tool changes first needed here; each applies to every game:

- **Little-endian RFRM forms, `TOCC` resource lists and GLSLC's extra control data** (`rfrm_order`, `rfrm_shaders`, `snvn_records`): [shaders.md](../../../docs/reference/shaders.md#snvn-program-records-in-retro-studios-rfrm-forms).
- **The sample index (`PIXLD.MYINDEX`) and the pixel stage's sample position rows of `c0`** (translator, `SAMPLE_ROWS`, `c0_sample_rows`, `nvnProgramSetSampleShading`): [shaders.md](../../../docs/reference/shaders.md#pixel-c0--0x620-the-sample-index-and-glslcs-sample-position-table).
- **Image stores of vertex shaders drawn with rasterizer discard** (`vs_writes`): [rendering.md](../../../docs/reference/rendering.md#rasterizer-discard-preserves-vertex-storage-writes).
- **Sparse 3D textures take the standard 3D tile** (`sparse_tile_h3`, `sparse_tile_d3`): [rendering.md](../../../docs/reference/rendering.md#sparse-3d-textures-take-the-standard-3d-tile).
- **Integer textures read through float slots** (`tex_float_fit`): [rendering.md](../../../docs/reference/rendering.md#integer-textures-read-through-float-slots).
- **Android:** the guest block 32 MB-aligned: [The guest block 32 MB-aligned (Android)](../../../docs/reference/memory.md#the-guest-block-32-mb-aligned-android).
- **Android:** attribute arrays, every location they cover in the replay's masks and split into an input per location (`spv_inarr.h`): [rendering.md](../../../docs/reference/rendering.md#a-shaders-attribute-array-its-elements-past-the-first-not-fetched-or-not-linked-android).
- **Android:** textures of sparse virtual pools read through the pool's page table (`sparse_tex`, `vpool_map`): [nvn-objects.md](../../../docs/reference/nvn-objects.md#textures-of-a-sparse-pool-read-through-its-page-table-on-android).

## Open

- The iPhone: no IPA built or tested yet, and the pipeline prewarm list `pso_manifest.tsv` is still empty.
- Only the opening and the Frigate's landing deck have been played; the Frigate's interior, Tallon IV, the visors, the Morph Ball, bosses and the movies (`FMV0` resources, the SDK movie library in `subsdk0`) are untested.
- Sample shading and the sample position table were met only on single-sample targets; the table's nibble order for several samples is the hardware's, not seen in a game.
