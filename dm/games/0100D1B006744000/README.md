# Crash Bandicoot N. Sane Trilogy (`0100D1B006744000`)

Base game 1.0.0 (`v0`), SDK **3.5.3** (`NintendoSdk_nnSdk-3_5_3`, `NintendoSDK_NVN-3_5_3`). Vicarious Visions' Alchemy engine (IGA archives of IGZ files), with Havok and FMOD, on NVN. Modules `main`, `subsdk0` (GLSLC, NVN's runtime shader compiler) and `sdk`. The RomFS holds 171 files in 5.21 GB: 166 IGA archives under `archives/` (`.pak`), two Bink movies (`bumper_atvi.bk2`, `credits.bk2`), `build.xml`, `cachenonce.txt` and `memoryconfiguration.igz`.

- **On Android:** In-game on a Galaxy Z Fold 8 (Adreno 840, its cover screen): from a new game through the intros, the title, the game select and Crash Bandicoot's opening cinematic into N. Sanity Beach under the player's control, at 30 fps (as on the Switch), with sound; its save loads by Continue after a cold start. See [Android](#android).
- **On the Mac:** the boot, the intros and the title screen at 30 fps, in a headless run of the Mac host; not played further.

## What it needs

| Setting | Where | Why |
|---|---|---|
| `modules main subsdk0 sdk` | [`game.tsv`](game.tsv) | every module `main` imports from |
| `sys_resource 0x0`, `mem_reserve 0x100000000`, `mem_alias 0x10000000`, `mem_stack 0x10000000`, `app_pool 0xCD500000` | [`game.tsv`](game.tsv) | the NPDM declares no system resource, so no virtual address memory; `main` sets a heap of at most the pool − 64 MB |
| `mem_used 0x5c00000` | [`game.tsv`](game.tsv) | the rewritten modules take 92 MB, more than the default 64 MB |
| `romfs_marker archives/l300_hub.pak`, `zip_store .bk2:.pak:.metallib` | [`game.tsv`](game.tsv) | every game file is an IGA archive or a Bink movie, already compressed |
| `shader_groups /archives/=archives /glslc_capture/=runtime` | [`game.tsv`](game.tsv) | the archive's shaders and the run-time captures in packages of their own |

The survey's provisional profile is unchanged in its keys but for the shader groups. The rewriter needed one more form of switch table, now general ([Index copied right before the check, table base built after it](../../../docs/reference/rewriter.md#index-copied-right-before-the-check-table-base-built-after-it), droidplay-core #446). Nothing for SDK 3.5.3 needed changing: the host's NVN device reports API 53.313 for SDK 3, and the audio renderer takes its SDK 4 DeviceSink command (`audren: … taking it as the SDK 4 DeviceSink`).

## Shaders

- **Where:** 1,635 shaders, 1,267 pixel, 367 vertex and 1 geometry:
  - 1,623 from `archives/shaders_nx.pak`: the 83 `.igz` files under `shaders/` hold 2,249 whole GLSLC outputs, in the archive's LZMA pieces ([GLSLC outputs in Alchemy's IGA archives](../../../docs/reference/shaders.md#glslc-outputs-in-alchemys-iga-archives)). Inflating all 166 archives (9.9 GB) found no other member holding a GLSLC output or BNSH;
  - 10 GLSLC outputs in `main`;
  - 2 that the game compiles at run time (below).
  Every output holds one stage, so the vertex shaders get the pixel input cover (`071b3f825a0d03e6`). The whole RomFS splits in about 1 s.
- **Before the reader (resolved):** the split found only `main`'s 10, and the first boot on Android (2026-11-13) showed the copyright notice and then stayed black, with `no translated shader` for 16 keys (`96d397221a3f9209`, `76ac0c21fc88433d`, …). All 16 are vertex shaders of `shaders_nx.pak` (Skybox, Bloom, TiledLight, BloomApply, LutGenerator, the bilateral and tile-max passes, ScreenSpaceShadows, ReflectionBlend, AmbientOcclusion, DepthDownsample, FurBlur, ColorGenerator).
- **Run-time shaders:** `main` holds 5 GLSL sources (`#version 440 core`): the Bink movie's vertex shader and its YUV → RGB pixel shaders without and with alpha, and a textured vertex / pixel pair (`ig_texture_bind`). Two compile at boot, before the Activision bumper: `5bd75993ec1facc2.glslc` (vertex, key `501595e5351f0131`) and `88e276f8d34c2fbc.glslc` (pixel, `68cecab93682bb22`). The phone's capture and a Mac capture (`DM_GLSLC_CAPTURE`) are byte-identical. Nothing else compiled up to N. Sanity Beach; the alpha shader may come with the credits movie, untested. Since the shader map names these two, the NSP script builds the game for Android from a Mac build's captures, as for the other games that compile shaders at run time ([droidplay-core#441](https://github.com/enjoyerofslop/droidplay-core/issues/441)).
- **Android:** all 1,635 translate for Vulkan, none fail.
- **Mac:** all 1,635 build.

## Android

Tested 2026-11-14 on a Galaxy Z Fold 8 (SM-F971U1: Snapdragon SM8850, Adreno 840 with Qualcomm's driver 0x8034a013, Vulkan 1.4.295, Android 17), closed, on its cover screen, with the on-screen controls and `DM_CTL` presses. DroidPlay Core `main` at `4db3b789` with the IGA reader. Four launches, about 20 minutes, with saves in a folder of the test's own (the fourth launch took the third's, for Continue).

- **Boot:** a black screen for about 5 s, the copyright and autosave notice, the Activision bumper (`bumper_atvi.bk2`, a Bink movie), the Vicarious Visions intro (crates in real time), then the title, about 60 s after launch.
- **Menus:** A at the title opens the License & Privacy Policy screen (Accept / Decline; it was accepted once in the test's save folder), then the game select: Crash Bandicoot, Crash Bandicoot 2: Cortex Strikes Back, Crash Bandicoot: Warped and Stormy Ascent, with A Select, Y License, X Credits.
- **Crash Bandicoot, NEW GAME:** the opening cinematic (Cortex's lab, Tawna), then N. Sanity Beach under control: walking, a spin that breaks a crate, a jump, the B / Y / X hint, a crab that costs a life with the death animation and the restart at the start, the pause menu (progress, time trials, relics) and back.
- **Save:** the new game writes `MACK_0.sav` and `MACK_OPTIONS.sav` (153,600 bytes each). After a cold start the license screen doesn't come again, and the game select shows CONTINUE, which loads Crash Bandicoot's island map (N. Sanity Beach, "Stormy Ascent is now available!"); A there starts N. Sanity Beach again. That launch made four save commits.
- **30 fps throughout:** after the boot's first 30 s, every 30 s window of the frame times holds 29.7–30.0 fps (median frame 33.33 ms; 19 windows over two launches). GPU p50 21.5 ms at the title and the license screen, 24.7–25.1 ms in N. Sanity Beach; NVN submit + present 16–18 ms in the level.
- **Memory:** 2.15–2.19 GB PSS in N. Sanity Beach, 0.81 GB of it graphics.
- **Sound:** the AudioRenderer and an AudioOut on AAudio at 48 kHz stereo, no underrun; 83,930 of the renderer's first 84,000 frames hold sound. Not listened to.
- **No `[RED]` line** in any launch.
- **Against the Mac:** the intros and the title look the same. The Mac run wasn't taken past the license screen, so the game select, the cinematic and the level weren't compared.
- **Not shown:** a level played through (real-time platforming can't be played over adb), Crash Bandicoot 2 and Warped, the credits movie, the open phone's main screen, other devices.

## Found on this game

- [GLSLC outputs in Alchemy's IGA archives](../../../docs/reference/shaders.md#glslc-outputs-in-alchemys-iga-archives).
- [Index copied right before the check, table base built after it](../../../docs/reference/rewriter.md#index-copied-right-before-the-check-table-base-built-after-it).
