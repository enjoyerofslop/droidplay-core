# Assassin's Creed: The Ezio Collection (`0100670014482000`)

Update 1.3 (v196608) over the base game, SDK **11.4.3** (`NintendoSDK_NVN-11_4_3`). **Assassin's Creed II** is the base game; **Brotherhood** (`0100670014483001`) and **Revelations** are DLC. It runs on Ubisoft's **AnvilNext** (Scimitar) with Havok and Wwise, and renders with **NVN**. Modules `main` and `sdk`. The RomFS is 114 files, 6.7 GB: 27 `.forge` archives under `AC2/` and `AC2_PATCH/`, two Wwise `.pck` banks and Bink movies. Brotherhood's DLC is 88 files, 10.4 GB: 29 `.forge` archives under `ACB/`, eight `.pck` banks and movies.

- **On the Mac:** In-game. Assassin's Creed II boots through the collection's launcher parameter to its title, menus and a new game, and into Desmond's escape from Abstergo under control at 30 fps. Everything the run asked for is in the shader map; see [Shaders](#shaders).
- **On Android:** In-game on a Galaxy S25 Edge: the same route at 30 fps, lit as on the Mac since buffer textures read through 2D slots got an image of their rows on Vulkan (#489). See [Android](#android).
- **Brotherhood** is program 1 and wasn't tried; Revelations isn't in this dump. See [Two programs](#two-programs).

## What it needs

| Setting | Where | Why |
|---|---|---|
| `shader_separate on` | [`game.tsv`](game.tsv) | every shader is one stage, a bare control section and code area in the `.forge` entries and in `main`, so every vertex shader is padded for any pixel shader. See [Shaders](#shaders) |
| `modules main sdk` | [`game.tsv`](game.tsv) | every module `main` imports from |
| `sys_resource 0x0`, `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | [`game.tsv`](game.tsv) | the NPDM declares no system resource; `main` sets a heap of at most the pool less 64 MB |
| `launch_param hex:…` | [`game.tsv`](game.tsv) | the launcher's parameter for II, handed over at the start, so the game skips its launcher. See [Two programs](#two-programs) |
| `shader_groups … /ACB/=acb` | [`game.tsv`](game.tsv) | Brotherhood's shaders, in the DLC, get a package of their own |

The profile is the provisional one `scripts/build_ipa_from_nsp.py` made (the survey's keys), with `launch_param`, `shader_separate`, Brotherhood's shader group and the comments added by hand. Its tables were split with the forge reader, the DLC included.

## Two programs

- **Program 0** (`0100670014482000`, this profile) is the program HOME starts: the collection's launcher, a title, then a choice of game.
- **II:** the launcher restarts program 0 with `oe::ExecuteProgram(0, "/launcher:off /gamesoundlanguage:en /gamemenulanguage:en /gamerewardsPurchased:0  /world:TitleScreen")`, 100 bytes with no terminating zero (two spaces before `/world`). Started with it, program 0 runs II.
- **Why the parameter is in the profile:** the host runs one program, so with `launch_param` it hands that parameter over once at the start (`hle_oe.c`), as for [the Rebel Collection](../010044700DEB0000/README.md#two-programs). Without it, choosing II ends the run (`[RED] oe::ExecuteProgram(0, 100 bytes of parameters: …): the host has only this program; it ends`). It sets English for sound and menus.
  - **How it was read:** a `DM_PROGRAMS` table with a row for program 0 whose `exe` is `/usr/bin/true`: the host writes the parameter to that row's `cwd` as `launch_param.bin`, then starts the row.
- **Brotherhood** is program 1: the launcher starts it with `oe::ExecuteProgram(1, …)`, 80 bytes, the same text without ` /world:TitleScreen` and its spaces. Its data is the DLC (`0100670014483001`, `ACB/` in its RomFS). It has no profile and wasn't tried.

## Shaders

- **Where they are:** 46,700 shaders in all: 3,488 vertex, 43,205 pixel and 7 compute. 20,417 are in II's 27 `.forge` archives (version 31), 26,123 in Brotherhood's 29 (version 25), and 160 in `main`. [`anvil_forge.py`](../../gfx/tools/anvil_forge.py) reads both versions ([AnvilNext .forge archives of versions 25 and 31](../../../docs/reference/shaders.md#anvilnext-forge-archives-of-versions-25-and-31-oodle-blocks-no-bnsh)).
  - **The format:** each entry's blocks are Oodle chunks of up to 32 KB, Leviathan in II's archives and Mermaid in Brotherhood's, which the game's own `OodleLZ_Decompress` decodes for the split (in the Mac host's tool mode). II's archives decode to 12.97 GB, Brotherhood's to 12.03 GB.
  - **No BNSH:** each shader is a 0x880-byte control section directly followed by its code area, at any byte offset, inside a resource of class `0xbcfb3c7a`. II has 539 such resources in 278 entries (1.87 GB decoded), such as `AC2_Tex2_PixelLit` (570 shaders) and the levels' own entries (`Firenze`, `Wetlands`). Most of them repeat: II's archives hold 224,124 shaders, 20,417 different.
  - **`main`** holds 224 shaders laid out the same way, 153 different, 8 of them compute shaders whose code areas have no header. None of them is in an archive.
- **Before the reader:** the split found none, in `main` or the RomFS. The Mac build stopped at the split, and an Android build of the provisional profile packed the game without shaders (the tester's report, 2026-11-13).
- **One stage per shader:** so the profile sets `shader_separate on`, as the Rebel Collection's does: the 3,488 vertex shaders are paired with a cover of two pixel shaders.

## Mac

- **Checked on Apple silicon, 2026-11-13,** headless, with the host's control file (`DM_CTL`): scripted presses and screenshots, no hand play. From a `--platform mac` build of the provisional profile (the same keys).
- **Route:** the parameter skips the launcher (without it the launcher's title and its game menu are drawn too). II's title ("press A") within 40 s, then A, Story Mode, Create New Game.
  - **The software keyboard:** the game asks for a name (its default text is `Desmond`); the run sets `DM_KBD=Ezio`.
  - Then the "previously on" movie (Bink; B skips it), the Animus load, Subject 16's writing on the wall ("Press any button") and Desmond's escape from Abstergo with Lucy, where the left stick walks him through the lab.
- **What draws:** the launcher, the titles and menus, the movie, the Animus loading screen, the lab, Desmond, Lucy and the light through the windows.
- **Frame rate:** 30.0 fps, the game's target on the Switch, in every 300-frame window from the title through the lab, but for three around the new game's movie and load (15.3, 26.8 and 28.5). The CPU used 19–30% of one core.
- **Red lines:** three, once each: `SetRenderEnableConditional: the host draws anyway for now` (the profile has no `render_cond`; nothing visibly wrong), `stage 0 cb7 has no uniform buffer bound`, and an image buffer texture at a pool offset below Metal's 16-byte alignment (`tbuf_shadow`). No shader was missing from the map.
- **Shader build:** 46,700 of 46,700 Mac shaders, none failed, in 40 minutes on the first build; the pipeline prebuild did 6,976 pairs.
- **Not checked:** sound, loading the save, Brotherhood, and play by hand.

## Android

### Samsung Galaxy S25 Edge

- **Tested 2026-11-13** (Adreno 830, Qualcomm's driver), from a private build of this branch with the provisional profile and its launch parameter in `env.txt` (`DM_LAUNCH_PARAM`). The build's own APK went on for the run, and the phone's own DroidPlay was put back afterwards. Scripted over adb with `DM_CTL` presses and `DM_KBD=Ezio`, from a new game in a private save folder. No hand play.
- **The build:** the split found the same 46,700 shaders with the Mac host as the game's Oodle decoder (1 min 16 s), and all 46,700 translated for Vulkan with none failed (3 min 2 s).
- **Route:** as on the Mac, through the movie, the load and the wall, into Abstergo's opening. The game wrote its first save there.
- **Resolved: the picture was black.** The titles, menus and loading screen drew their text and shapes on black, where the Mac draws them on a white backdrop, and nothing of the opening's 3D scene showed. The game reads a 48-element R32F buffer texture (texture ID 4610, slot 132 of compute shader `2405a4193559eaec` and others) through a slot the translator declares 2D, at `(x & 0x3FFF, x >> 14)` (res.json `fetch1d`). The Mac binds it as 2D rows (`tbuf_as_2d`); the Vulkan replay refused every buffer texture (`[RED] [vk] buffer textures are not on Vulkan yet`), so the slot read 0 and the lit passes multiplied by it. A Mac host with `tbuf_as_2d` switched off drew the S25's black picture. Fixed by #489 (`dm/android/tbuf_rows.h`; `DM_VK_TBUF_ROWS=0` turns it off): on 2026-11-14, with hosts from main a72a85c5 plus this profile and #489, the same route drew the title and menus on white, the Animus load's text, and the lab and corridor matching the Mac's frames after the same walk, at the same frame time (p50 33.4 ms, p95 41.4 ms) and GPU time (p50 14.9 ms), 2.89 GB PSS at most.
- **Current: at the spawn after the Animus load** the wall around the doorway in front of Desmond isn't drawn for the first moments, and the lab shows through; the Mac draws it. The white patch on the doorway's floor after about 30 s shows on the Mac too. `DM_VK_OCCLUSION=0` gives the same frame. Not traced yet.
- **Current: the compute pass that writes the buffer texture** is sometimes still pending when its reader comes, and that read waits for the submission (409 such waits over the route, 0 before #489); the frame rate is the same.
  - **Other red lines:** shaders `a8dcee323ede7253` and `97a3014bf67b9022` read uniform slot 0 with no buffer bound (8 lines), and `SetRenderEnableConditional`, as on the Mac.
- **Frame rate:** 30.0 fps in every 20 s window but one of the load (28.0): frame p50 33.4 ms, p95 about 41 ms in the opening.
- **Memory:** 2.9 GB PSS in the opening, 0.69 GB of it graphics.
- **Sound:** both audio outputs ran with no underrun; nobody listened.

## Open

- **Android, at the spawn:** the doorway's wall that isn't drawn for the first moments: a frame dump against the Mac's.
- **Brotherhood** (program 1): a profile, then a boot. Its 26,123 shaders are in this profile's tables now, as the split reads the DLC with the game; whether program 1's own build should be the one to carry them is open. Revelations wasn't dumped.
- **On Android the decode is the phone's on Windows and Linux:** the split decodes every forge entry, about 13 GB for II and 12 GB for Brotherhood, so a build with no Mac to decode on sends some 700,000 Oodle streams to the phone ([Oodle-compressed archives decoded on the phone](../../../docs/reference/shaders.md#oodle-compressed-archives-decoded-on-the-phone-for-an-android-build)). Every shader is in resources of one class (`0xbcfb3c7a`): decoding only the chunks that hold resources' first bytes, then that class's resources, would be 5.22 GB of II's 12.97 GB.
- **`render_cond`:** the game sets render conditions; whether it needs the key wasn't looked at.
- **`pso_manifest.tsv`** is empty.
