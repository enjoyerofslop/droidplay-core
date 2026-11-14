<!-- The "droidnative" anchor keeps links made before the rename working: the project was droidnative until 2026-11-09. -->
# <a id="droidnative"></a>DroidPlay Core

[![Latest release](https://img.shields.io/github/v/release/enjoyerofslop/droidplay-core?label=latest%20release)](https://github.com/enjoyerofslop/droidplay-core/releases/latest)
[![License: MIT](https://img.shields.io/github/license/enjoyerofslop/droidplay-core)](LICENSE)
[![Discord](https://img.shields.io/badge/Discord-DroidPlay-5865F2?logo=discord&logoColor=white)](https://discord.gg/3r92f8wmh7)

**Play the Switch games you own on your Android phone or handheld, natively.** DroidPlay builds your own copy of a Switch game into an Android app: the
game's code runs directly on the phone's processor, not in an emulator, and its graphics are drawn by the phone's GPU through Vulkan.

It's for players with an Android phone or handheld and a Switch of their own to dump their games from. You build each game once on your computer, and
DroidPlay installs it on the phone. DroidPlay Core is the engine behind it: [seedds/nxnative](https://github.com/seedds/nxnative), which runs Switch games
natively on the iPhone, the iPad and the Mac, with an Android port that began as [RogelioLB/nxnative-android](https://github.com/RogelioLB/nxnative-android).

> [!IMPORTANT]
> **Bring your own game.** Nothing from any game is in this repository, and no packed game is distributed: you dump the game and the keys from **your own**
> Switch, and the app you build is for you only. Do not share APKs packed with a game (they carry its code and files), keys or firmware, and do not ask for them
> in issues. The project is not affiliated with Nintendo or any game publisher.

**Download [DroidPlay for Windows](https://github.com/enjoyerofslop/droidplay-core/releases/latest)**: **DroidPlay-Setup-&lt;version&gt;.exe** from the latest
release. Run it, and it walks you through the rest ([Get started](#get-started)). On a Mac, use [DroidPlay](https://github.com/enjoyerofslop/droidplay).
On Linux, [DroidPlay for Linux](#on-linux-preview) is an AppImage in preview, for testers.

- **Runs on** arm64 phones and handhelds with Android 10 or later and a Vulkan GPU. Tested on Qualcomm Adreno (the Ayn Odin 2, the Retroid Pocket 5, the
  Galaxy S25 Edge and others), Arm Mali (the Pixel 9 Pro XL) and Samsung Xclipse (the Galaxy Z Flip 7): [Phones and handhelds](#phones-and-handhelds).
- **What plays:** [Game Status](#game-status) lists every game run on Android, on which devices, and how far it got.
- **Help:** the [DroidPlay Discord](https://discord.gg/3r92f8wmh7), #help for a problem and #compatibility for how a game runs on your device.

<table>
<tr>
<td align="center" valign="top" width="50%"><img src="docs/images/windows/get-started.png" alt="DroidPlay for Windows on Get started: the keys picked, the tools for building downloading with the time left, and the phone waiting for Allow"><br><small><b>Get started</b>: your keys, the tools downloading, your phone and your first game</small></td>
<td align="center" valign="top" width="50%"><img src="docs/images/windows/library.png" alt="DroidPlay for Windows' Library: Pokémon Brilliant Diamond with its box art, ready on a Galaxy S25 Edge, with a Play button"><br><small><b>Library</b>: a game installed on the phone, ready to play</small></td>
</tr>
</table>

[Phones and handhelds](#phones-and-handhelds) · [Get started](#get-started) · [Updating](#updating) · [On your phone](#on-your-phone) ·
[Game Status](#game-status) · [Help and reports](#help-and-reports) · [Guides](#guides) · [How it works](#how-it-works) · [Contributing](#contributing)

## Phones and handhelds

DroidPlay runs on arm64 Android phones, handhelds and tablets with Android 10 or later and a GPU with Vulkan. How well a game runs depends on the GPU and the
game: [Game Status](#game-status) gives each game's frame rate on each device. Games have been run on these:

| Device | Chip | GPU |
|---|---|---|
| Samsung Galaxy Z Fold 8 | Snapdragon 8 Elite Gen 5 | Adreno 840 |
| Samsung Galaxy S25 Edge | Snapdragon 8 Elite for Galaxy | Adreno 830 |
| OnePlus Pad 3 | Snapdragon 8 Elite | Adreno 830 |
| Ayn Odin 2 | Snapdragon 8 Gen 2 | Adreno 740 |
| Moto Razr Plus 2023 | Snapdragon 8+ Gen 1 | Adreno 730 |
| Retroid Pocket 5 | Snapdragon 865 | Adreno 650 |
| POCO X5 5G | Snapdragon 695 | Adreno 619 |
| Google Pixel 9 Pro XL | Google Tensor G4 | Arm Mali-G715 |
| Samsung Galaxy Z Flip 7 | Samsung Exynos 2500 | Samsung Xclipse 950 |

Qualcomm's Adreno GPUs have had the most testing; Mali and Xclipse are newer to the project. The POCO X5 5G is the original port's device, last run before
2026-11-08. Other devices may work too, but haven't been tested by the project: say how a game runs on yours in #compatibility on the Discord.

### Another Vulkan driver (Turnip)

On an Adreno GPU, a game can run on a Vulkan driver you put on the phone instead of the system's, as in Switch emulators: usually a build of Mesa's
open-source Turnip driver (on a Samsung Xclipse GPU, a build of Mesa's RADV for Xclipse). Trying one needs no new build, and whether it helps depends on the game and the device. The steps:
[Another Vulkan driver](docs/android.md#another-vulkan-driver-turnip).

## Get started

### What you need

- **A computer to build on:** a Windows 10 (1809 or later) or Windows 11 PC, 64-bit, for DroidPlay for Windows; or a Mac with Apple silicon and macOS 15 or
  later for [DroidPlay on a Mac](https://github.com/enjoyerofslop/droidplay); or, in preview, an x86_64 Linux with glibc 2.34 or later (Ubuntu 22.04,
  Debian 12, Fedora 35, SteamOS 3 or newer) for [DroidPlay for Linux](#on-linux-preview). On Windows and Linux, DroidPlay and the tools it installs take about 6 GB, and each game's
  build needs room besides, on the drive with the game's files: about the game's size for its extracted files, plus what its shaders need while they translate,
  up to about 20 GB for most games and 90 GB or more for one with over 100,000 shaders, such as Pokémon Pokopia ([Free space](docs/android.md#free-space)).
- **An Android phone or handheld** with Android 10 or later ([the devices tested](#phones-and-handhelds)), a USB cable (or wireless debugging), and room for
  the game: its files are copied to the phone (6.7 GB for Danganronpa V3).
- **Your own Switch, and the game and keys dumped from it:** the game as NSP or XCI files in one folder (the base game and, if you have them, its update and
  DLC; for example with nxdumptool), and your Switch's `prod.keys` (Lockpick_RCM). A Switch 2 game is built from its `.nup` file or dump folder, which needs
  no keys; Switch 2 games are at an early stage on Android ([Game Status](#game-status)).

### Install and build your first game

On Windows:

1. **Install DroidPlay.** Download **DroidPlay-Setup-&lt;version&gt;.exe** from the [latest release](https://github.com/enjoyerofslop/droidplay-core/releases/latest)
   and run it. Windows warns that it comes from the internet and isn't signed: choose **More info**, then **Run anyway**. It installs for your Windows user
   only, with no administrator prompt.
2. **Follow Get started.** DroidPlay opens on a checklist of four steps. Setting up takes about half an hour the first time.
   - **Your keys:** pick your `prod.keys`.
   - **Tools for building:** **Install tools** downloads the JDK, the .NET SDK and Google's Android SDK (about 1.5 GB to download). DroidPlay asks
     before accepting the Android SDK's license.
   - **Your phone:** turn on USB debugging, plug the phone in and tap **Allow** on it. **Set up my phone** shows the steps for your phone's brand.
   - **Your first game:** add the folder with its NSP or XCI files (a Switch 2 game: the folder that holds its `.nup` or its dump), then **Build and install**. The first build of a game takes about 10 to 20 minutes;
     later builds reuse what didn't change.
3. **Play.** Press **Play** in DroidPlay's Library, or open the DroidPlay app on the phone and pick the game. The first start of a game takes a minute or
   more while the phone prepares its shaders; later starts take a few seconds.

Every page of the app, uninstalling, and running it from source: [DroidPlay for Windows](docs/android.md#droidplay-for-windows).

### On Linux (preview)

**DroidPlay for Linux** is the same app as an AppImage, `DroidPlay-<version>-x86_64.AppImage`, for x86_64 Linux with glibc 2.34 or later. It's in preview:
it isn't on the release page yet, and testers get it from the `linux-package` workflow's **DroidPlay-AppImage** artifact (with its `.sha256`), or build it
with `dm/android/linux/stage.sh`. Make it executable (`chmod +x`) and open it. The first start of a version prepares it in
`~/.local/share/DroidPlay/<version>` (about 720 MB, a few seconds); later starts are quick. Get started works as on Windows: **Install tools** downloads
Java, .NET and the Android SDK into `~/.local/share/nxnative/android/tools` (about 1.5 GB, 4 GB on disk), with no `sudo`; a phone that Linux won't let
DroidPlay use over USB shows as **USB access needed**, and **How to connect a phone** says what to install. **Settings** › **Add to the applications
menu** gives it a menu entry. Where FUSE is missing, `--appimage-extract-and-run` opens it. More:
[DroidPlay for Linux](docs/android.md#droidplay-for-linux-preview).

### On a Mac or from the command line

- **On a Mac:** [DroidPlay](https://github.com/enjoyerofslop/droidplay) builds and installs Android games too: add your games folder, then **Install on
  Android** (one app holding every game, over adb) or **Build Game App**. Its README has the requirements and the setup steps.
- **From the command line,** on Windows, macOS or Linux: `scripts/build_android_from_nsp.py`
  ([Packing from the command line](docs/android.md#packing-from-the-command-line)).

## Updating

- **DroidPlay for Windows** says when a new release is out: press **Update**, or **Update to &lt;version&gt;** under Updates in **Settings**. It downloads
  the new Setup, checks it against the release, installs it and opens again; your games, builds and settings stay. Or close DroidPlay and run the new Setup
  yourself.
- **DroidPlay for Linux** (preview) says so too, and once releases carry the AppImage, **Update** downloads it into the old one's folder, checks it against
  the release, puts it in the old one's place and opens it. Its first start prepares the new version and keeps the previous one; your games, builds and
  settings stay. An AppImage with the name it was downloaded with takes the new version's (the menu entry DroidPlay added follows it); one you renamed keeps
  your name. Where DroidPlay can't write to the AppImage's folder, **Download** opens the release page instead.
- **Games you've already built** keep what they were built with. Build them again (**Build and install**) to get a release's fixes; what didn't change isn't
  redone, so it's usually quick. Saves stay on the phone.
- **What changed** in each release is in the [release notes](https://github.com/enjoyerofslop/droidplay-core/releases).

## On your phone

- **The DroidPlay app** lists the games on the phone. Pick one for its settings (console mode, game language, render scale where the game supports it, the
  frame-rate counter, rumble), then **Play**.
- **Controls:** touch controls are drawn over the game in three layouts, 2D, 3D and Tap, switched with the round button at the top. A controller works as
  player 1 and hides them.
- **Pause and back to the list:** the **≡** button at the top pauses the game and opens a small menu: resume, the touch layout, the frame-rate counter, the way to
  export or import the save, and back to the list, where **Play** on the same game resumes it.
- **Saves** stay in the app's storage, so updating the app keeps them. DroidPlay for Windows backs them up to your computer from its **Phones** page.
- **Game apps:** **Manage › Build a game app…** in DroidPlay for Windows makes one game into an app of its own, with its own icon and saves, to copy to a
  device and install there. It's for your own devices only: it carries the game, so never share it.

More: [The Android app](docs/android.md#the-android-app) · [Game apps](docs/android.md#game-apps).

<!-- The "status" anchor keeps links made before the rename working: DroidPlay's README links to #status. -->
## <a id="status"></a>Game Status

Which games run on Android, on which devices, and how far each has been played. Every row comes from runs on real devices, by the project or a contributor.
A Playable game was played by hand through the stretch its row names, with saves and sound checked; the tiers below say exactly what each label asks for.

Playable now:

- **Playable – minor issues:** Pokémon Brilliant Diamond, Pokémon Shield, Pokémon: Let's Go, Pikachu!, Persona 3 Portable, Divinity: Original Sin 2 - Definitive Edition, Mario Kart 8 Deluxe, Paper Mario: The Thousand-Year Door, Advance Wars 1+2: Re-Boot Camp and Mario vs. Donkey Kong.
- **Playable – major issues:** Animal Crossing: New Horizons, Final Fantasy Resonance, Frostpunk, OCTOPATH TRAVELER, OCTOPATH TRAVELER II, Pokémon Legends: Arceus, Pokémon Legends: Z-A, The Legend of Zelda: Breath of the Wild, The Legend of Zelda: Link's Awakening, The Legend of Zelda: Skyward Sword HD, Xenoblade Chronicles 2 and Xenoblade Chronicles 3.

More games reach gameplay (In-game) or their menus; the table below has every game run on Android, and the issues seen.

Screen captures from the test runs, taken on the device, best tier first. The round button at the top is DroidPlay's menu, the line at the bottom Android's
gesture bar. On a phone, DroidPlay's on-screen controls are drawn over the game. The chip under each game's name is the tier of its row in the table below
(the tiers are defined under the captures).

<table>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/android/pokemon-brilliant-diamond-odin-2.jpg" width="256" alt="Pokémon Brilliant Diamond in Eterna City on an Ayn Odin 2"><br><small><b><a href="dm/games/0100000011D90000/README.md">Pokémon Brilliant Diamond</a></b><br><a href="#tiers"><img src="docs/badges/playable-minor.svg" height="20" alt="Playable – minor issues"></a><br>Ayn Odin 2: Eterna City, 60 fps</small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/android/pokemon-shield-galaxy-s25-edge.jpg" width="256" alt="Pokémon Shield on a Samsung Galaxy S25 Edge: the player at the front door of the house in Postwick, under the on-screen controls"><br><small><b><a href="dm/games/01008DB008C2C000/README.md">Pokémon Shield</a></b><br><a href="#tiers"><img src="docs/badges/playable-minor.svg" height="20" alt="Playable – minor issues"></a><br>Samsung Galaxy S25 Edge: outside the player's house in Postwick, 30 fps</small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/android/pokemon-lets-go-pikachu-pixel-9-pro-xl.jpg" width="256" alt="Pokémon: Let's Go, Pikachu! on a Google Pixel 9 Pro XL: the player outside his house in Pallet Town, under the on-screen controls"><br><small><b><a href="dm/games/010003F003A34000/README.md">Pokémon: Let's Go, Pikachu!</a></b><br><a href="#tiers"><img src="docs/badges/playable-minor.svg" height="20" alt="Playable – minor issues"></a><br>Google Pixel 9 Pro XL: Pallet Town, 30 fps</small></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/android/divinity-original-sin-2-odin-2.jpg" width="256" alt="Divinity: Original Sin 2 on an Ayn Odin 2: the player's character in the Merryweather's hold"><br><small><b><a href="dm/games/010027400CDC6000/README.md">Divinity: Original Sin 2 - Definitive Edition</a></b><br><a href="#tiers"><img src="docs/badges/playable-minor.svg" height="20" alt="Playable – minor issues"></a><br>Ayn Odin 2: the Merryweather's hold, 30 fps</small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/android/persona-3-portable-galaxy-z-flip-7.jpg" width="256" alt="Persona 3 Portable on a Samsung Galaxy Z Flip 7, from a downloaded save: the party in Tartarus's entrance hall on January 31, under the on-screen controls"><br><small><b><a href="dm/games/0100DCD01525A000/README.md">Persona 3 Portable</a></b><br><a href="#tiers"><img src="docs/badges/playable-minor.svg" height="20" alt="Playable – minor issues"></a><br>Samsung Galaxy Z Flip 7: Tartarus's entrance (a downloaded save), 60 fps</small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/android/mario-kart-8-deluxe-pixel-9-pro-xl.jpg" width="256" alt="Mario Kart 8 Deluxe on a Google Pixel 9 Pro XL: Mario in first place on the second lap of Mario Kart Stadium, under the on-screen controls"><br><small><b><a href="dm/games/0100152000022000/README.md">Mario Kart 8 Deluxe</a></b><br><a href="#tiers"><img src="docs/badges/playable-minor.svg" height="20" alt="Playable – minor issues"></a><br>Google Pixel 9 Pro XL: Mario Kart Stadium, 60 fps</small></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/android/paper-mario-the-thousand-year-door-galaxy-s25-edge.jpg" width="256" alt="Paper Mario: The Thousand-Year Door on a Samsung Galaxy S25 Edge: Mario's first battle, against Lord Crump on the theatre stage, under the on-screen controls"><br><small><b><a href="dm/games/0100ECD018EBE000/README.md">Paper Mario: The Thousand-Year Door</a></b><br><a href="#tiers"><img src="docs/badges/playable-minor.svg" height="20" alt="Playable – minor issues"></a><br>Samsung Galaxy S25 Edge: the first battle, against Lord Crump, 30 fps</small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/android/advance-wars-1-2-re-boot-camp-moto-razr-plus-2023.jpg" width="256" alt="Advance Wars 1+2: Re-Boot Camp on a Moto Razr Plus 2023: a battle scene in Field Training's first lesson, Orange Star's infantry against Blue Moon's, under the on-screen controls"><br><small><b><a href="dm/games/0100300012F2A000/README.md">Advance Wars 1+2: Re-Boot Camp</a></b><br><a href="#tiers"><img src="docs/badges/playable-minor.svg" height="20" alt="Playable – minor issues"></a><br>Moto Razr Plus 2023: Field Training's first battle, 30 fps</small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/android/mario-vs-donkey-kong-ayn-thor-pro.jpg" width="256" alt="Mario vs. Donkey Kong on an AYN Thor Pro, from a contributor: Mario beside a lava pool in world 3, with a key, a present and a locked door on stone blocks, the timer at 66, 60 fps in the corner"><br><small><b><a href="dm/games/0100B99019412000/README.md">Mario vs. Donkey Kong</a></b><br><a href="#tiers"><img src="docs/badges/playable-minor.svg" height="20" alt="Playable – minor issues"></a><br>AYN Thor Pro (contributor): a lava level in world 3, 60 fps</small></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/android/animal-crossing-new-horizons-odin-2.jpg" width="256" alt="Animal Crossing: New Horizons on an Ayn Odin 2: the first evening's welcome party, the player at the campfire with Tom Nook, Timmy, Tommy and two islanders"><br><small><b><a href="dm/games/01006F8002326000/README.md">Animal Crossing: New Horizons</a></b><br><a href="#tiers"><img src="docs/badges/playable-major.svg" height="20" alt="Playable – major issues"></a><br>Ayn Odin 2: the welcome party on the first evening, 30 fps</small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/android/zelda-skyward-sword-hd-odin-2.jpg" width="256" alt="The Legend of Zelda: Skyward Sword HD in Skyloft's plaza on an Ayn Odin 2, Link at the bird statue"><br><small><b><a href="dm/games/01002DA013484000/README.md">The Legend of Zelda: Skyward Sword HD</a></b><br><a href="#tiers"><img src="docs/badges/playable-major.svg" height="20" alt="Playable – major issues"></a><br>Ayn Odin 2: Skyloft at 60 fps</small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/android/final-fantasy-resonance-odin-2.jpg" width="256" alt="Final Fantasy Resonance's first field at night on an Ayn Odin 2, its flames drawn along the road"><br><small><b><a href="dm/games/0100606026F12000/README.md">Final Fantasy Resonance</a></b><br><a href="#tiers"><img src="docs/badges/playable-major.svg" height="20" alt="Playable – major issues"></a><br>Ayn Odin 2: the first field, its flames drawn, 52 fps</small></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/android/frostpunk-galaxy-s25-edge.jpg" width="256" alt="Frostpunk's city on a Samsung Galaxy S25 Edge: the Generator in the crater and the wreckage beside it, on the snow"><br><small><b><a href="dm/games/0100B1C0249B4000/README.md">Frostpunk</a></b><br><a href="#tiers"><img src="docs/badges/playable-major.svg" height="20" alt="Playable – major issues"></a><br>Samsung Galaxy S25 Edge: the city, 30 fps</small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/android/zelda-breath-of-the-wild-odin-2.jpg" width="256" alt="The Legend of Zelda: Breath of the Wild on an Ayn Odin 2: Link in the Shrine of Resurrection"><br><small><b><a href="dm/games/01007EF00011E000/README.md">The Legend of Zelda: Breath of the Wild</a></b><br><a href="#tiers"><img src="docs/badges/playable-major.svg" height="20" alt="Playable – major issues"></a><br>Ayn Odin 2: the Shrine of Resurrection, 30 fps</small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/android/octopath-traveler-pixel-9-pro-xl.jpg" width="256" alt="OCTOPATH TRAVELER on a Google Pixel 9 Pro XL: H'aanit in a Provoke battle against a fledgling hunter in S'warkii, her command menu open"><br><small><b><a href="dm/games/010057D006492000/README.md">OCTOPATH TRAVELER</a></b><br><a href="#tiers"><img src="docs/badges/playable-major.svg" height="20" alt="Playable – major issues"></a><br>Google Pixel 9 Pro XL: a Provoke battle in S'warkii, 57–60 fps</small></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/android/octopath-traveler-ii-galaxy-z-flip-7.jpg" width="256" alt="OCTOPATH TRAVELER II on a Samsung Galaxy Z Flip 7: Hikari's first battle on the battlefield, under the on-screen controls"><br><small><b><a href="dm/games/0100A3501946E000/README.md">OCTOPATH TRAVELER II</a></b><br><a href="#tiers"><img src="docs/badges/playable-major.svg" height="20" alt="Playable – major issues"></a><br>Samsung Galaxy Z Flip 7: Hikari's first battle, 56–60 fps</small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/android/pokemon-legends-arceus-oneplus-pad-3.jpg" width="256" alt="Pokémon Legends: Arceus on a OnePlus Pad 3, from a new game: the player throwing a Poké Ball at Cyndaquil beside a boulder near Prelude Beach, under the on-screen controls"><br><small><b><a href="dm/games/01001F5010DFA000/README.md">Pokémon Legends: Arceus</a></b><br><a href="#tiers"><img src="docs/badges/playable-major.svg" height="20" alt="Playable – major issues"></a><br>OnePlus Pad 3: the first catch, 30 fps</small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/android/xenoblade-chronicles-2-galaxy-z-flip-7.jpg" width="256" alt="Xenoblade Chronicles 2 on a Samsung Galaxy Z Flip 7: King Crustip on the ship's deck at the start of the tutorial battle, Rex in front, drawn clean, under the on-screen controls"><br><small><b><a href="dm/games/0100E95004038000/README.md">Xenoblade Chronicles 2</a></b><br><a href="#tiers"><img src="docs/badges/playable-major.svg" height="20" alt="Playable – major issues"></a><br>Samsung Galaxy Z Flip 7: King Crustip's battle, 30 fps</small></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/android/zelda-links-awakening-retroid-pocket-5.jpg" width="256" alt="The Legend of Zelda: Link's Awakening on a Retroid Pocket 5, from a new game: Link holding up his sword at the water's edge on Toronbo Shores"><br><small><b><a href="dm/games/01006BB00C6F0000/README.md">The Legend of Zelda: Link's Awakening</a></b><br><a href="#tiers"><img src="docs/badges/playable-major.svg" height="20" alt="Playable – major issues"></a><br>Retroid Pocket 5: the sword, 30 fps</small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/android/xenoblade-chronicles-3-galaxy-s25-edge.jpg" width="256" alt="Xenoblade Chronicles 3 on a Samsung Galaxy S25 Edge: Noah and the party fighting an Agnus soldier in the first battle on the Everblight Plain, the ground and the rocks drawn clean, under the on-screen controls"><br><small><b><a href="dm/games/010074F013262000/README.md">Xenoblade Chronicles 3</a></b><br><a href="#tiers"><img src="docs/badges/playable-major.svg" height="20" alt="Playable – major issues"></a><br>Samsung Galaxy S25 Edge: the first battle, 30 fps</small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/android/super-mario-odyssey-retroid-pocket-5.jpg" width="256" alt="Super Mario Odyssey's opening cutscene on a Retroid Pocket 5"><br><small><b><a href="dm/games/0100000000010000/README.md">Super Mario Odyssey</a></b><br><a href="#tiers"><img src="docs/badges/in-game.svg" height="20" alt="In-game"></a><br>Retroid Pocket 5: the opening cutscene, 59 fps</small></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/android/super-smash-bros-ultimate-odin-2.jpg" width="256" alt="A Super Smash Bros. Ultimate battle on an Ayn Odin 2"><br><small><b><a href="dm/games/01006A800016E000/README.md">Super Smash Bros. Ultimate</a></b><br><a href="#tiers"><img src="docs/badges/in-game.svg" height="20" alt="In-game"></a><br>Ayn Odin 2: a battle, 40–59 fps</small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/android/dark-souls-remastered-odin-2.jpg" width="256" alt="DARK SOULS: REMASTERED on an Ayn Odin 2: the player in the Undead Asylum's cell"><br><small><b><a href="dm/games/01004AB00A260000/README.md">DARK SOULS: REMASTERED</a></b><br><a href="#tiers"><img src="docs/badges/in-game.svg" height="20" alt="In-game"></a><br>Ayn Odin 2: the Undead Asylum cell, 30 fps</small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/android/blasphemous-2-odin-2.jpg" width="256" alt="Blasphemous 2 on an Ayn Odin 2: the Penitent One in the room of the three statues"><br><small><b><a href="dm/games/010040801A4C8000/README.md">Blasphemous 2</a></b><br><a href="#tiers"><img src="docs/badges/in-game.svg" height="20" alt="In-game"></a><br>Ayn Odin 2: the room of the three statues, 60 fps</small></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/android/donkey-kong-country-returns-hd-odin-2.jpg" width="256" alt="Donkey Kong Country Returns HD on an Ayn Odin 2: Donkey Kong on the beach in 1-1 Jungle Hijinxs"><br><small><b><a href="dm/games/01009D901BC56000/README.md">Donkey Kong Country Returns HD</a></b><br><a href="#tiers"><img src="docs/badges/in-game.svg" height="20" alt="In-game"></a><br>Ayn Odin 2: 1-1 Jungle Hijinxs, 60 fps</small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/android/assassins-creed-black-flag-moto-razr-plus-2023.jpg" width="256" alt="Assassin's Creed IV: Black Flag's opening storm on a Moto Razr Plus 2023: Edward on the ship's deck in the rain, under the on-screen controls"><br><small><b><a href="dm/games/010044700DEB0000/README.md">Assassin's Creed: The Rebel Collection</a></b><br><a href="#tiers"><img src="docs/badges/in-game.svg" height="20" alt="In-game"></a><br>Moto Razr Plus 2023: Black Flag's opening at sea, 30 fps</small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/android/bayonetta-2-oneplus-pad-3.jpg" width="256" alt="Bayonetta 2 on a OnePlus Pad 3: Bayonetta fighting angels on the jet in Verse 1, the combo points counting"><br><small><b><a href="dm/games/01007960049A0000/README.md">Bayonetta 2</a></b><br><a href="#tiers"><img src="docs/badges/in-game.svg" height="20" alt="In-game"></a><br>OnePlus Pad 3: Verse 1 on the jet, 60 fps</small></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/android/pokemon-legends-z-a-odin-2.jpg" width="256" alt="Pokémon Legends: Z-A on an Ayn Odin 2, from a new game: the player at the railing of the Gare de Lumiose platform, the tracks and the hills beyond"><br><small><b><a href="dm/games/0100F43008C44000/README.md">Pokémon Legends: Z-A</a></b><br><a href="#tiers"><img src="docs/badges/playable-major.svg" height="20" alt="Playable – major issues"></a><br>Ayn Odin 2: the Gare de Lumiose platform, 30 fps</small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/android/skyrim-galaxy-s25-edge.jpg" width="256" alt="The Elder Scrolls V: Skyrim on a Samsung Galaxy S25 Edge, from a new game: Helgen in flames, the keep's tower ahead, under the on-screen controls"><br><small><b><a href="dm/games/01000A10041EA000/README.md">The Elder Scrolls V: Skyrim</a></b><br><a href="#tiers"><img src="docs/badges/in-game.svg" height="20" alt="In-game"></a><br>Samsung Galaxy S25 Edge: Helgen in flames, 30 fps</small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/android/metroid-dread-pixel-9-pro-xl.jpg" width="256" alt="Metroid Dread on a Google Pixel 9 Pro XL, from a new game: Samus jumping over the rocks of a cavern, a crawler below her"><br><small><b><a href="dm/games/010093801237C000/README.md">Metroid Dread</a></b><br><a href="#tiers"><img src="docs/badges/in-game.svg" height="20" alt="In-game"></a><br>Google Pixel 9 Pro XL: the first caverns, 60 fps</small></td>
</tr>
<tr>
<td align="center" valign="top" width="33%"><img src="docs/images/android/metroid-prime-remastered-moto-razr-plus-2023.jpg" width="256" alt="Metroid Prime Remastered on a Moto Razr Plus 2023, from a new game: the Frigate Orpheon's landing deck, its force field drawn as a jumble of blue shapes and the arm cannon dark, under the on-screen controls"><br><small><b><a href="dm/games/010012101468C000/README.md">Metroid Prime Remastered</a></b><br><a href="#tiers"><img src="docs/badges/in-game.svg" height="20" alt="In-game"></a><br>Moto Razr Plus 2023: the Frigate Orpheon's landing deck, 60 fps</small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/android/mario-kart-world-oneplus-pad-3.jpg" width="256" alt="Mario Kart World on a OnePlus Pad 3: Mario in seventh place on the first lap at Mario Bros. Circuit, the track's surface untextured, under the on-screen controls"><br><small><b><a href="dm/games/0400C3F00006E000/README.md">Mario Kart World</a></b><br><a href="#tiers"><img src="docs/badges/in-game-blocked.svg" height="20" alt="In-game, blocked"></a><br>OnePlus Pad 3: a race at Mario Bros. Circuit, about 27 fps</small></td>
<td align="center" valign="top" width="33%"><img src="docs/images/android/zelda-ocarina-of-time-galaxy-z-fold-8.jpg" width="256" alt="The Legend of Zelda: Ocarina of Time on a Samsung Galaxy Z Fold 8: young Link on the path through Kokiri Forest with Navi, under the on-screen controls"><br><small><b><a href="dm/games/0400486024DEC000/README.md">The Legend of Zelda: Ocarina of Time</a></b><br><a href="#tiers"><img src="docs/badges/in-game-blocked.svg" height="20" alt="In-game, blocked"></a><br>Samsung Galaxy Z Fold 8: Kokiri Forest, about 20 fps</small></td>
</tr>
</table>

<a id="tiers"></a>
Each row's State starts with its tier: the highest one whose conditions have all been shown. Both Playable tiers say how far the game was played ("through …");
neither means the whole game was.

| Tier | What has been shown |
|---|---|
| **Playable – minor issues** through *a named stretch* | Everything Playable – major issues asks, and on at least one device, through the stretch: a median frame rate of at least 95% of the target, with no 30 s window under 90% of it (loads, scene changes and a first start's shader and pipeline builds excepted); only cosmetic faults (a one-frame artefact, a slightly wrong colour or shade, draws left out only while a first start builds its pipelines), each one compared with the Mac; no crash or hang; sound right; Continue loading the latest save; and a stretch that takes in the game's first battle, race or level end, or, in a game with none of these, the end of its opening, where it hands the player its everyday play (the row says where) |
| **Playable – major issues** through *a named stretch* | Played by hand (adb input counts; a scripted run alone doesn't) from a new game or a save made on Android, through the stretch named (a save made by hand on another Android device counts; a save made on the Mac doesn't); at least half the target frame rate (a median); the stretch finished, with at most one crash or hang, got past with a restart and Continue, and not back at the same point; a save made on Android and loaded again after a cold start; sound checked. The row names each issue that keeps it from Playable – minor issues |
| **In-game** | Gameplay under the player's control, not only menus or movies, with the picture usable and at least half the target frame rate, but not yet shown playable: a scripted run, someone else's save or a save made on the Mac, no stretch played by hand, or saves or sound not checked |
| **In-game, blocked** | Gameplay reached, but a named bug stops normal play: the world missing or black, under half the target frame rate, or a crash or hang that comes back at the same point |
| **Menus** | The title, menus or movies; no control of the player in gameplay |
| **Boots** | A first frame or a logo; nothing further shown |
| **Doesn't run** | Stops before its first frame, or the packing script refuses it |

The target frame rate is the rate the game runs at by default on Android: `DM_FPS_TARGET` in its profile's `android_defaults.txt`, else the profile's
`fps_target`, else the Switch's rate. A row's tier rests on the device that has shown the most, and the row says how the others did.

The rest of each row says on which devices, how far and how (by hand, or a scripted run from a fresh or a downloaded save), the frame rate, the issues seen,
and what has not been tried.

| Game | Device | State |
|---|---|---|
| Pokémon Brilliant Diamond 1.3.0 | Ayn Odin 2, OnePlus Pad 3, Retroid Pocket 5, Samsung Galaxy S25 Edge, Samsung Galaxy Z Flip 7, Google Pixel 9 Pro XL, Moto Razr Plus 2023 | Playable – minor issues through Lake Verity's first battle: 60 fps (the profile's rate; the Switch runs it at 30). On the Pixel 9 Pro XL (Mali), the Moto Razr Plus 2023 (Adreno 730) and the Samsung Galaxy Z Flip 7 (Xclipse), played by hand from a new game through Twinleaf, Route 201 and Lake Verity to Turtwig and the first battle, a wild Starly, won, with sound, a save made on the phone and Continue working; on the Z Flip 7, 59.8 fps over 32 minutes (no 30 s window under 57.9) and Twinleaf's pond drawn as water since #204. Issues: on the Pixel, one minute at the starter screen at 50–54 fps; none seen on the Razr or the Z Flip 7. On the other four, In-game: 59–60 fps in Eterna City's overworld, in a scripted run from a downloaded save with the player standing by the Pokémon Center; a new game was played by hand into the overworld on the Odin 2 on 2026-11-07, before the nxnative merge, and not since. The Retroid Pocket 5 no longer crashes at launch, and its own driver now runs Eterna City as fast as [Turnip](#another-vulkan-driver-turnip) does (15 fps before) |
| Animal Crossing: New Horizons 3.0.3 | Ayn Odin 2, OnePlus Pad 3, Retroid Pocket 5, Samsung Galaxy S25 Edge | Playable – major issues through the first day and night into the next morning: on the Ayn Odin 2, played by hand from a new game through character creation, the flight and the arrival, Tom Nook's welcome and the tents, the branches and apples for the campfire, the welcome party (where the island is named), the first night in the tent and the next morning's NookPhone, at 30 fps (as on the Switch; 30.00 over the 2 h 12 min session, no stretch under 29.8), with sound, and the game's first save, made that morning, loaded by Continue after a cold start ([its note](dm/games/01006F8002326000/README.md#android-through-the-first-night-on-the-odin-2)). Issue: on the Odin 2 (Adreno 740), a GPU hang that the kernel's GPU driver detects and resets, about ten minutes into a session: the picture stops for 0.4–1 s, then play goes on. It came in three of six sessions there, never on the OnePlus Pad 3 (two runs), and the Mac runs the same moment at 30 fps ([its note](dm/games/01006F8002326000/README.md#android-the-gpu-hang-on-the-odin-2)). The stretch takes in the end of the game's opening (it has no battle, race or level end): the next morning, when island time starts to follow the real time and free play begins. Not yet Playable – minor issues: that GPU hang isn't a cosmetic fault, and the Mac doesn't have it. Scripted runs: 30 fps on all four, at the new game's airport (a fresh save, no input) and outside the player's house from a downloaded save with the player standing; on 0.8.7 (`main` 50a256fe), from a new game through the arrival to the dock, 30 fps on the Odin 2 and the Pad 3 |
| Super Mario Odyssey 1.4.1 | Ayn Odin 2, OnePlus Pad 3, Retroid Pocket 5, Samsung Galaxy S25 Edge, Google Pixel 9 Pro XL | In-game: the Start menu and the opening cutscene at 59–60 fps on all five (the cutscene ran at 38 on the Retroid Pocket 5 on 2026-11-08), and play in the Cap Kingdom at 60 on the Odin 2 (one scripted run of 15 s), past the hint video that used to crash it (the host's AVC decoder, fixed; the video plays on the Pad 3 too). Not played by hand. The crash now and then while it loads is fixed. The Pixel 9 Pro XL's Mali-G715, the first GPU outside Adreno, has no BC textures, so they are decoded on the CPU (2.2 GB resident in the cutscene, against 1.64 GB) |
| Super Smash Bros. Ultimate 13.0.5 | Ayn Odin 2, OnePlus Pad 3, Retroid Pocket 5, Samsung Galaxy S25 Edge | In-game: boots to its opening movie at 60 fps on all four, with the hinted shared font the packer now ships (Android's Roboto made it crash); the crash in about one boot in five is fixed too. The route's battle (Mario against a random CPU fighter, the player idle; not played by hand) runs at 60 fps on the S25 Edge, 52–60 on the Pad 3 (2026-11-09) and 57–60 on the Retroid Pocket 5 (2026-11-10), by stage and fighter; 40–59 on the Odin 2 (2026-11-08, not measured since) |
| The Legend of Zelda: Link's Awakening 1.1.0 | Ayn Odin 2, OnePlus Pad 3, Retroid Pocket 5, Samsung Galaxy S25 Edge | Playable – major issues through Toronbo Shores, the sword and its first fights: on the Retroid Pocket 5 (2026-11-12), over adb from a new game through Marin's house, Tarin's shield, Mabe Village and the walled fields to Toronbo Shores and the sword, then two Sea Urchins and two Sand Crabs cut down with it. Saved into File 1 from the Game Over screen, and after a cold start Start brought Link back to the beach with the sword. Sound right (5 short underruns in 22 minutes), one red line per launch with nothing visible, no crash. Mabe Village at Marin's door matches the Mac's picture, sun shadows included ([its note](dm/games/01006BB00C6F0000/README.md#android)). What keeps it from minor issues: outside the houses the Retroid Pocket 5 drops to the game's 30 fps interval (median 31–32 fps against 60; 59 in Marin's house). The OnePlus Pad 3 played the same stretch by hand to the sword before the sun was fixed: 60 fps in the house and the village, the 30 fps interval in the fields and on the beach. Not tried yet: the Mysterious Forest. From a downloaded save at the Ancient Ruins (after the fifth dungeon), Link standing: 60 fps on the Odin 2, 38–45 on the S25 Edge, and 32–33 on the Pad 3 and the Retroid Pocket 5, where the game drops to its 30 fps interval. The boot crash it had on every launch on the Retroid Pocket 5 and now and then on the Pad 3 is fixed (checked on the Retroid Pocket 5) |
| The Legend of Zelda: Skyward Sword HD 1.0.1 | Ayn Odin 2, OnePlus Pad 3, Retroid Pocket 5, Samsung Galaxy S25 Edge | Playable – major issues through the opening into Skyloft (Instructor Horwell's first lessons): on the Retroid Pocket 5, played by hand from a new game with button controls, through the opening, Link's room, the Knight Academy and out into Skyloft, at 60 fps (a title card at 30), with sound, and the autosave loaded by Continue after a cold start. The parts of Skyloft that drew flat or not at all in the session that started the new game (Horwell's wooden crates, flat magenta in the area's opening shot) are fixed since 0.8.0 ([#270](https://github.com/enjoyerofslop/droidplay-core/pull/270); a new game to Skyloft's opening shot draws right). Not yet Playable – minor issues: the stretch doesn't take in the game's first battle, race or level end ([its note](dm/games/01002DA013484000/README.md#android-a-new-game-on-the-retroid-pocket-5)). From a downloaded 31-hour Hero Mode save, Skyloft's plaza with Link standing at the bird statue, its terrain in colour on all four: 59–60 fps on the Odin 2, the Pad 3 and the S25 Edge, and 43 on the Retroid Pocket 5 (2026-11-10), where the game then switched between its 60 and 30 fps intervals within a run. The opening movie at 55–60 fps (41 on the Retroid Pocket 5) |
| Final Fantasy Resonance 1.0.1 | Ayn Odin 2, OnePlus Pad 3, Samsung Galaxy S25 Edge, Google Pixel 9 Pro XL | Playable – major issues through a new game's opening, the first field and its first three battles: on the Ayn Odin 2, played by hand from a new game, the field's flames drawn since `ANDROID_CHAIN` 6, at a median 52 fps on chain 6 (43–60 in 30 s windows), with sound, and the autosave loaded by Continue after a cold start ([its note](dm/games/0100606026F12000/README.md#android-a-new-game-by-hand-on-the-odin-2)). Issues: the frame rate, under 95% of the 60 target; the flames nearest the camera drawn as coarse blocks, a herringbone stripe on rock textures and faint horizontal lines, not yet compared with the Mac. Scripted runs: about 42 fps in the field on the Odin 2 and the Pad 3, 30–33 on the S25 Edge (10–16 fps before 0.6.0), and the opening at 30 fps on the Pixel 9 Pro XL. Not tried on the Retroid Pocket 5 |
| Pokémon Shield 1.3.2 | Retroid Pocket 5, Samsung Galaxy S25 Edge, Google Pixel 9 Pro XL, Samsung Galaxy Z Flip 7 | Playable – minor issues through Route 2 into Professor Magnolia's house: 30 fps (as on the Switch) on all four. On the Z Flip 7 (Xclipse), played by hand from a new game through the opening, Route 1, Wedgehurst and the starters to Hop's battle, won, then into the Slumbering Weald with a wild battle there; on the Retroid Pocket 5, as far as Hop's battle, won, and from the Z Flip 7's Weald save through the Weald's event (the battle in the fog, Leon's rescue) back to Postwick and Mum, then from there through Wedgehurst (the Pokédex), Route 2 and its first trainers into Professor Magnolia's house; on the S25 Edge and the Pixel 9 Pro XL (Mali), into Postwick, where Hop meets the player outside the house. Sound, saves made on the phone and Continue work on all four. Issues: a first start's pipeline builds (one 6.4 s frame on the Z Flip 7 and one of 5.0 s on the Retroid Pocket 5; 118 and 178 ms at most on the next start), and frames of 1.4–1.7 s as the lab and Route 2 built theirs on a first visit; nothing else seen. Not played as far as the battle in Magnolia's garden or the Wild Area; not tried on the other devices |
| Frostpunk 1.0.1 | Samsung Galaxy S25 Edge, Samsung Galaxy Z Flip 7, Moto Razr Plus 2023 | Playable – major issues through day 1 into the first night: 30 fps (29.1–29.7 in every 30 s window outside loads) on the S25 Edge (Adreno) and the Galaxy Z Flip 7 (Xclipse); on the S25 Edge, played by hand over adb from a new game of A New Home: coal, wood and steel gathered, three tents built and a cookhouse placed, the Generator started, the first workday into the night, with sound (an audio dump: sound throughout), a save made on the phone and Resume story loading it after a cold start. The Generator, the buildings and the props draw through raw global memory, which its profile turns on. Issue: the S25's grabs of the opening view looked darker than the Mac's capture, not compared under the same capture. Not yet Playable – minor issues: that picture isn't compared with the Mac, and the stretch isn't shown to take in the end of the game's opening, which the tier asks of a game with no battle or race (and no level end short of a whole scenario's): where it hands the player its everyday play isn't named yet ([its note](dm/games/0100B1C0249B4000/README.md#open)). On the Galaxy Z Flip 7 (Xclipse), the same stretch by hand from a new game through the first night, saved and resumed after a cold start, at 29.0–29.7 fps in the city's 30 s windows, with sound (an audio dump). On the Moto Razr Plus 2023 (Adreno 730, Qualcomm's driver V@0615.98), from the title into the city by a scripted route at 29.5 fps, with the Generator and the wreckage drawn: the driver's compiler refuses the pointers the shaders read raw global memory through (`OpConvertUToPtr not supported`), so there the memory is reached through a storage-buffer array instead. Not played past the tutorial's first pop-up, and sound not checked again there |
| Pokémon: Let's Go, Pikachu! 1.0.0 | Samsung Galaxy Z Fold 8, Google Pixel 9 Pro XL, Samsung Galaxy Z Flip 7, OnePlus Pad 3 | Playable – minor issues through Viridian City and the rival's first battle (Oak's Parcel): 30 fps (as on the Switch) on all three. These runs predate the frame step stub that made 60 fps the game's Android default: they ran at its own 30, the target then, and the stretch hasn't been played by hand at 60, where Playable – minor issues asks a median of 57 fps with no 30 s window under 54. At 60, from Android-made saves (scripted checks, 2026-11-13), the Z Fold 8 held 60 fps in Pallet Town, Viridian Forest and a wild catch there, and the Pixel 9 Pro XL ran 58 in Pallet Town and fell back to 30 in the forest, with game time at real time on both ([its note](dm/games/010003F003A34000/README.md#60-fps)). On the Z Fold 8, played by hand from a save made on the phone in Pallet Town through Professor Oak's scene, getting Pikachu, his lab and the Pokédex, to two wild catches on Route 1 made with the buttons (no motion needed); the opening into Pallet Town was a scripted run there. On the Pixel 9 Pro XL (Mali), played by hand from a new game through the opening and the rival's visit into Pallet Town, and a wild Pidgey caught on Route 1 from the Z Fold 8's save. Then, from that save, by hand through Route 1 (more catches) to Viridian City, the Poké Mart clerk's Oak's Parcel back to Professor Oak, and the rival's first battle in the lab (his Eevee, won with Thunder Shock), at 29.6–30.0 fps in every minute of a 35-minute launch, with a save made after it that Continue loads after a cold start. On the Z Flip 7 (Xclipse), played by hand from a new game through the opening into the bedroom, then from the Pixel's save after the battle through Route 1 (a trainer battle won), Viridian City and Route 2 into Viridian Forest, with a save there that Continue loads after a cold start. Sound (confirmed by audio dumps), a save made on the phone and Continue work on all three. Issues: the party, bag and Catching Pocket sprites had red and blue exchanged on every Android GPU (a cyan Pikachu) until [#424](https://github.com/enjoyerofslop/droidplay-core/pull/424), in 0.8.5, checked on the Pixel 9 Pro XL, the Z Flip 7 and a OnePlus Pad 3; nothing else seen up to the rival's battle. On the Z Flip 7, the walk past Viridian Forest's first Bug Catcher ran at 21–28 fps for 40–90 s each time (the game's own frame work; not compared with the Mac), which keeps the forest out of the minor-issues stretch; a later run there showed a flat dark band over the lower third of the forest's picture, which the Pixel 9 Pro XL and the OnePlus Pad 3 show too from the same save, so it isn't Xclipse's alone; open and not yet compared with the Mac ([its note](dm/games/010003F003A34000/README.md#android)). Not tried: the Pokémon Center and Mart inside, Viridian Forest's trainers and Pewter City, aiming with motion, the other devices |
| Divinity: Original Sin 2 - Definitive Edition 1.0.3.1 | Ayn Odin 2, OnePlus Pad 3, Google Pixel 9 Pro XL | Playable – minor issues through Fort Joy's beach (the ship's escape by lifeboat): the game's 30 fps from a new game through character creation and the opening cinematic into the Merryweather's hold, then by hand to the cabin deck, and on the Odin 2 through the burning hold to the top deck's first fight (two Voidwoken, won), the lifeboat and its dialogue, and below deck a second fight (Void Hatchlings, won with Lohse and Beast); on the Pad 3 from the top deck by the lifeboat's release, the shipwreck, to Fort Joy's beach and its waypoint, at 30 fps (29.97 over 48 minutes, no 30 s span under 29.3); with sound, Quick Saves dated by the host's clock and Continue loading the latest after a cold start. Every window's median frame time 33.1–33.5 ms; four alpha-test red lines in the log, nothing wrong on screen. On the Pixel 9 Pro XL (Mali) the top deck's first fight, won, at 30 fps too, with sound and Continue after a cold start; its GPU takes about 26 ms of the 33 ms frame ([its note](dm/games/010027400CDC6000/README.md#on-the-pixel-9-pro-xl)). On the Pad 3 it runs at 40 or 60 fps with a setting, the game moving by real time at each, and held 60 for 25 minutes ([its note](dm/games/010027400CDC6000/README.md#frame-rate)). Not played past Fort Joy's beach |
| The Legend of Zelda: Breath of the Wild 1.9.0 | Ayn Odin 2, OnePlus Pad 3, Samsung Galaxy S25 Edge, Samsung Galaxy Z Fold 8 | Playable – major issues through the Shrine of Resurrection onto the Great Plateau (no combat yet): 30 fps (its Android target) on the Odin 2, played by hand from a new game: waking in the Shrine, taking the Sheikah Slate, opening its doors and climbing out, then the cutscene over Hyrule and the first minutes on the plateau, with sound, an autosave made on the phone and Continue after a cold start. The Old Man not reached ([its note](dm/games/01007EF00011E000/README.md#android)). Issues: the stretch ends before the first combat; the frame where Link died from a fall had red smears on nearby rocks, not compared with the Mac. The Shrine from a save made on the Mac: 30 fps on the Pad 3 and the S25 Edge with its Android defaults (descriptor sets bound again, the command recorder, a 30 fps target); 24 on the Galaxy Z Fold 8, whose GPU takes 30 ms a frame. On the Pad 3 it lost the GPU at its first frame in the field, and on both Adreno 830s it drew the Shrine dark: both are fixed, checked on the S25 Edge and, by hand from a new game with main's 0.7.5 build, on the Pad 3: the Shrine lit, about 5.5 minutes on the plateau without losing the GPU at 30 fps, and a save loaded by Continue after a cold start. 60 fps needs the replay's CPU time and the GPU's to come down |
| OCTOPATH TRAVELER 1.0.3 | Google Pixel 9 Pro XL, Retroid Pocket 5 | Playable – major issues through H'aanit's first battles: on the Pixel 9 Pro XL (Mali-G715, Android 17), played over adb from a new game through H'aanit's opening (the flashback and S'warkii), the headman, the Path Actions tutorial and two Provoke battles against the fledgling hunters, both won, then a save at the village's save monument, loaded by Continue after a cold start, and on to the Path to the Whisperwood's first monster battle (a Marmot, broken and beaten), with sound and no crash ([its note](dm/games/010057D006492000/README.md#android)). Built from the cartridge's 1.0.3 with a profile derived from 1.0.5's; the shader split found all 5,434 shaders. The target is 60 fps (the Switch runs it at 30): the battles and the Whisperwood hold 57–60, and the medians were 52.1 fps from the new game to the save and 58.7 from Continue to the Marmot. 3.4 GB at most (the process's memory cgroup), well under Android 17's 8 GiB cap; never killed. Issues: in S'warkii the Mali's GPU takes 17–19 ms a frame, so the village runs at 44–57 fps and its save screen at 38; on the Path to the Whisperwood, pale sharp-edged areas at the top and bottom of the screen where the blurred foreground meets the background, not compared with the Mac (not played that far there). Not yet Playable – minor issues: the median is under 57 fps and those areas aren't compared. On the Retroid Pocket 5 (Adreno 650) every start crashed in the driver until small layered render passes got scratch colour slots on Adreno 6xx; with that, a new game through H'aanit's flashback into S'warkii at about 49 fps, the grading as on the Pixel (not played further). Not tried on the other devices |
| OCTOPATH TRAVELER II 1.0.0 | Samsung Galaxy S25 Edge, Samsung Galaxy Z Flip 7 | Playable – major issues through Hikari's first battle: on the S25 Edge (Adreno 830), played over adb from a new game through the initial settings, Hikari's opening and the battlefield's first battle with its tutorials (won), then free roam on the battlefield and two more battles, with sound and no crash, and the game's autosave on the battlefield loaded by Continue after a cold start; the same on the Galaxy Z Flip 7 (Xclipse 950) ([its note](dm/games/0100A3501946E000/README.md#android)). Built from the cartridge's base game 1.0.0 with a profile derived from 1.1.1's; all 13,942 of its shaders translated. The target is 60 fps (the Switch runs it at 30): the battles, scenes and the camp hold 56–60 on both phones, and the S25 Edge's 30 s windows had a median of 59.1. 3.2–3.3 GB PSS, never killed. Issues: the battlefield's top-down field (1,700–2,050 draws a frame) runs at 43–50 fps, under 90% of the target; not compared with the Mac. The battlefield's ground, rocks and the camp's ground were drawn as horizontal streaks on both phones until texture slots in stencil mode read the stencil (the ground clean on the S25 Edge with that fix, no cost in its frame rate). The prologue's next fight, Rai Mei's battle with the mounted general, was lost by the agent's plain attacks; not played past it. Not tried on the other devices |
| Persona 3 Portable 1.0.2 | Samsung Galaxy S25 Edge, Samsung Galaxy Z Flip 7 | Playable – minor issues through the first battle (April 9's rooftop): 60 fps (as on the Switch) on both, played by hand from a new game through the opening, the dorm and the first school days with their point-and-click exploration, to the night of April 9 and its first battle, won with the buttons, with sound, a save made at the dorm's Sign-In Sheet and Continue working. On the S25 Edge in console mode, on the Z Flip 7 (Xclipse) in Android's default handheld mode, which works on both ([its note](dm/games/0100DCD01525A000/README.md#android)). Issues: none seen on the S25 Edge; on the Z Flip 7, one frame of the opening movie covered in a fine dot grid, not compared with the Mac. Not played past the first battle (Tartarus); not tried on the other devices |
| Mario Kart 8 Deluxe 4.0.1 | Google Pixel 9 Pro XL, Samsung Galaxy Z Fold 8, Moto Razr Plus 2023, OnePlus Pad 3 | Playable – minor issues through the first 50cc race (the Mushroom Cup at Mario Kart Stadium): on the Pixel 9 Pro XL, two 50cc Grand Prix races at Mario Kart Stadium, both won, at 60 fps (as on the Switch), from a new save: boot, the title's demo race, the Mii pick (the heads blank, as on the Mac), the menus with their movie previews, then the race; the save loads after a cold start, with sound. Those races were steered by the game's Smart Steering with accelerate held over adb; races were steered by hand on a Samsung Galaxy Z Fold 8 (the project's owner), with the steering controls working as expected. The Mali GPU has no BC textures, so they are decoded on the CPU: 4.3 GB resident against the Mac's 2.4. Issue: on the Moto Razr Plus 2023 (Adreno 730) a race runs at about 45 fps, the replay CPU-bound; there, since the course-load crash was fixed ([#243](https://github.com/enjoyerofslop/droidplay-core/pull/243)), the demo race, a 50cc race to its results and the save after a cold start ran, in 1.6 GB, with the title and menus at 60. On the OnePlus Pad 3 (Adreno 830), with the same fix: two demo races and the Mushroom Cup's first two races (Mario Kart Stadium and Water Park) to their results at 60 fps, GPU about 8.5 ms a frame, 2.6 GB, no red lines, and the save after a cold start ([its note](dm/games/0100152000022000/README.md#android)) |
| Paper Mario: The Thousand-Year Door 1.0.0 | Samsung Galaxy S25 Edge | Playable – minor issues through the first battle (Lord Crump in Rogueport, with Goombella's tutorial): on the S25 Edge, played by hand over adb from a new file through the storybook prologue, Peach in Rogueport, Mario's house and the boat into Rogueport's docks, the first battle won and Goombella joining in the plaza, at a median 29.9 fps over 37 minutes (27.8–30.0 in 30 s windows; the game's own 30 fps), with sound (the prologue movie's on its own output), no crash, and a Save Block save made in the plaza after the battle loaded by Continue after a cold start. Since [#337](https://github.com/enjoyerofslop/droidplay-core/pull/337) (in 0.8.7) the low-memory killer no longer ends the game at the prologue's sea scene, where its descriptor pools had reached 6.7 GB: with it the S25 Edge went on past the scene into Rogueport's docks with 34 pools (427 MB). The dump is the 1.0.0 cartridge base, packed from a profile derived from 1.0.1's, so 1.0.1's 60 fps patch doesn't apply ([its note](dm/games/0100ECD018EBE000/README.md#android)). Nothing looked wrong; the prologue's scenes match a Mac run of the same build at the same frames. Not tried on another phone |
| Advance Wars 1+2: Re-Boot Camp 1.0.0 | Moto Razr Plus 2023, Samsung Galaxy Z Flip 7 | Playable – minor issues through Field Training's first lesson (Troop Orders): on the Moto Razr Plus 2023 (Adreno 730, Qualcomm's own driver), played over adb from a new game through the Bink opening movie, Nell's introduction and a new Casual campaign to the first lesson's battle, won, at the game's own rates: 60 fps in the menus and 30 in battle by its own limit (every 30 s window of the battle 29.7–30.0, frame p95 about 38 ms), with sound (FMOD, no underrun), the game's save at the lesson's end and Continue after a cold start (the next lesson offered); the second lesson (Cities & Terrain) was then played into its second day. 2.35 GB PSS on the phone's 8 GB, never killed. Packed as a game app ([its note](dm/games/0100300012F2A000/README.md#android)). Issues: back in a menu after a battle, the GPU can stay at the clock the battle's light frames left it at, so the menu holds 30 fps (24 ms of GPU against 13) until a touch raises the clock: seen only with scripted input, which sends no touches. The first sight of each battle animation builds its pipelines (frames of up to 0.8 s once). Nothing wrong seen in the picture, so nothing was compared with the Mac. On the Samsung Galaxy Z Flip 7 (Xclipse 950, Samsung's driver), the same stretch from a new game: the troops, the map and the battle scenes all draw (another Android port draws neither troops nor map on Xclipse), battles at 29.7–30.0 fps in every 30 s window, menus at 60, including after a battle (GPU 5 ms), sound with no underrun, the save and Continue after a cold start, 2.9–3.2 GB PSS of 11 GB. Not tried on the other devices |
| Mario vs. Donkey Kong 1.0.1 | AYN Thor Pro | Playable – minor issues through world 2 into world 3 (contributor report): on an AYN Thor Pro (Snapdragon 8 Gen 2, Adreno 740, the PurpleVK driver), played by hand from the tester's own save, at 60 fps and the game's right speed (its level timer keeping real seconds) with `DM_PRESENT_INTERVAL=display`, this game's Android default since [#445](https://github.com/enjoyerofslop/droidplay-core/pull/445). Without it the game drops itself to 30 fps and half speed after one slow frame ([its note](dm/games/0100B99019412000/README.md#android)). Issues: a slight audio crackle now and then, with or without that setting; the logs show no underruns ([#435](https://github.com/enjoyerofslop/droidplay-core/issues/435)). |
| SHIN MEGAMI TENSEI V: Vengeance 1.0.3 | Samsung Galaxy S25 Ultra | In-game (contributor report): a save in Mita at 30 fps, and at 60 with `DM_UE_DYNRES_BUDGET=16.7` ([its note](dm/games/010069C01AB82000/README.md#frame-rate)). Not run on the project's devices |
| DARK SOULS: REMASTERED 1.0.0 | Samsung Galaxy Z Flip 7, Ayn Odin 2 | In-game: on the Galaxy Z Flip 7 (Xclipse 950), over adb from a new game through the first-run settings, character creation (the name typed through Android's keyboard) and the opening into the Undead Asylum cell, walking and turning the camera, at the game's own 30 fps (every 30 s window 28.0–30.1) and 1.8 GB PSS; the game's constant saves load by Continue after a cold start ([its note](dm/games/01004AB00A260000/README.md#android)). Built from the cartridge's base game 1.0.0 with a profile derived from 1.0.3's, whose 60 fps cuts are 1.0.3-only, so it runs at 30 as on the Switch. Issue: on the Z Flip 7 the opening movie's sound, on a second output, underruns about 9% of the time (it crackles); the game's own sound has no underrun, and on the Ayn Odin 2 (Adreno 740), which runs the same route into the cell at 30 fps, the movie's output underran in only 1 of 10 reports. Not shown playable: real-time action, so its stretch (the Asylum through the Asylum Demon) needs a person with a controller. The game app stays installed on the Odin 2 for a person to play. Not tried on the other devices |
| Super Mario Bros. Wonder 1.2.1 | POCO X5 5G (Adreno 619) | In-game: about 30 fps of the game's 60 at render scale 0.8, in levels and on the world map; below 60 fps Piranha Plants on Parade softlocks unless the stopgap `DM_AUDIO_PACE=game` is on ([audio](docs/reference/audio.md#music-timed-sequences-on-a-device-below-the-games-frame-rate)) † |
| Assassin's Creed: The Rebel Collection 1.0.0 | OnePlus Pad 3, Moto Razr Plus 2023, Ayn Odin 2, Galaxy Z Flip 7 | In-game: Black Flag's opening storm at sea under control at the Switch's 30 fps: 30.0 fps over 5 minutes on the Pad 3 (p95 41 ms, GPU 12 ms, 1.4 GB PSS), 29.7 and 28.9 on the Razr Plus (2–7% of its frames take 50–73 ms, each with about twice the draws and passes). From a new game, in scripted runs over adb: the title, the menus and the name prompt, then Edward walked along the deck to the ship's wheel with the camera turned, on both. The ship, the crew, the storm, the cannon fire and the HUD are drawn, with no red line. The save the opening makes loads after a restart on both. Sound plays: the game's PCM dump holds signal, but nobody listened. Taking the wheel and sailing weren't reached (the take-wheel prompt came and went as the deck swayed), nor was hand play. On the Ayn Odin 2 (Adreno 740) the same opening at 30.0 fps in every 30 s window, GPU 9.3 ms, 1.4 GB PSS, no red line, and the save loads after a cold start. On the Galaxy Z Flip 7 (Xclipse 950, BC textures decoded on the CPU) the same: 30.0 fps in every window, GPU 13 ms, 1.9 GB PSS, no red line or underrun, the save loads, and Edward reached the wheel's raised deck. Rogue can't run from this cartridge: program 1 there holds only its movies, and its game data is a download that wasn't dumped (packed as a test, it crashes before its first frame) ([its note](dm/games/010044700DEB0000/README.md#android)) |
| Assassin's Creed: The Ezio Collection 1.3 | Samsung Galaxy S25 Edge | In-game: Assassin's Creed II over adb from a new game, the collection's launcher skipped by its launch parameter: the title, the menus, the name, the opening movie, the Animus load and Desmond's escape from Abstergo, lit as on the Mac, at 30 fps (frame p50 33.4 ms, p95 about 41 ms, GPU 15 ms, 2.9 GB PSS), and the opening's save is written. For the first moments after the Animus load the wall around the doorway in front of Desmond isn't drawn. Scripted, not played by hand. Brotherhood (program 1) wasn't tried ([its note](dm/games/0100670014482000/README.md#android)) |
| Blasphemous 2 1.0.1 | Samsung Galaxy S25 Edge, Ayn Odin 2 | In-game: on the S25 Edge (Adreno 830), over adb from a new game through the opening cinematic, the awakening and its Bile Flask tutorial, to the room of the three statues and the sword taken there, at 60 fps (every 30 s window 58.8–60.0), with sound (no underruns) and 2.0 GB PSS ([its note](dm/games/010040801A4C8000/README.md#android)). Built from the cartridge's 1.0.1 with a profile derived from 1.0.8's. Not shown playable: the game saves only at a Prie Dieu, which wasn't reached, so saving and Continue are untested; a real-time platformer, its Playable stretch (through the Faceless One) needs a person with a controller. Nothing wrong seen in the picture; one red line (an advanced blend mode drawn as plain blending), not compared with the Mac. The same on the Ayn Odin 2 (Adreno 740) at 60 fps, where the game app stays installed for a person to play. Not tried on the other devices |
| Super Mario 3D World + Bowser's Fury 1.2.2 | POCO X5 5G | Menus: Bowser's Fury starts and is drawn with correct colours; how far it was played isn't recorded, and one launch stopped at a black loading screen † |
| Batman: Arkham Asylum 1.0.0 | Ayn Odin 2 | In-game: on the Ayn Odin 2 (Adreno 740), over adb from a new game (Normal) through the languages, the warnings, the title and the opening cinematic to the first walk behind the Joker's escort through Intensive Treatment, at 28.5–29.7 fps in play (the game's own 30), with sound (a 6-channel `AudioOut` and the `AudioRenderer`, one underrun) and 1.4 GB PSS; the autosave loads by Continue Story after a cold start. Packed from the Batman: Arkham Trilogy cartridge (which holds three games: pick one with `--title-id`, [#373](https://github.com/enjoyerofslop/droidplay-core/pull/373)) from a provisional profile, as the repository has none for it yet: Rocksteady's Unreal Engine 3 on NVN, its packages read with the game's own Oodle; 40,443 of its 41,809 shaders translated, 1,366 left out, and 12 red "no translated shader" lines in play with nothing seen missing. Not shown playable: real-time action, so its stretch needs a person. Arkham City and Arkham Knight on the same cartridge are stubs with no game data (their games come as downloads that weren't dumped). Not tried on the other devices |
| The Elder Scrolls V: Skyrim 1.1.404 | Samsung Galaxy S25 Edge, AYN Thor Pro | In-game: on the S25 Edge (Adreno 830), over adb from a new game through the title, the main menu, the opening cart ride into Helgen, character creation (named in DroidPlay's text box) and the dragon attack to the first steps under the player's control, at 30 fps (the game's own rate: a median frame time of 33.0–33.5 ms in 64 of 69 windows, up to 39.6 ms at boot and loads) with 3.2 GB PSS; both audio renderers produce output, not listened to; the game wrote its character-creation save and an autosave. Its 23,133 shaders are in its BSA archive and main, read since this profile. Issues: one red line, 8 times as the opening loads (a pixel shader reads an unbound uniform slot 11; nothing seen missing), and the main menu's "Start a new game?" box drawn as two lines the height of the screen (the Mac the same). Not shown playable: real-time action, so its stretch needs a person; saves not loaded back; not tried on the other devices. A contributor's AYN Thor Pro (Adreno 740, the phone's own driver) played on by hand into and through Helgen Keep: since [#448](https://github.com/enjoyerofslop/droidplay-core/pull/448) (in 0.8.7) the keep's lights no longer flicker. The crashes it met at a door, after a knockdown and near a cave came from the game's audio pool, used by two threads without a lock; [#453](https://github.com/enjoyerofslop/droidplay-core/pull/453) (in 0.8.7) adds the lock (on the Mac a forced-timing test went from 4 crashes in 4 to none; a phone recheck is pending). Also seen there: first-person hands and weapons sometimes drawn as flat grey or cyan shapes ([#444](https://github.com/enjoyerofslop/droidplay-core/issues/444)) |
| Danganronpa V3 Anniversary Edition 1.0.2 | POCO X5 5G | Menus: boots, plays its opening movie and draws its picture, from a provisional profile; not played through † |
| Donkey Kong Country Returns HD 1.0.0 | Samsung Galaxy S25 Edge, Ayn Odin 2, Samsung Galaxy Z Flip 7 | In-game: on the S25 Edge (Adreno 830), over adb from a new save through the title, the menus, the opening movie and the treehouse into 1-1 Jungle Hijinxs (running, jumping, damage), at 60 fps (every 30 s window 59.6–60.1), with sound (no underruns) and 2.5 GB PSS; the save loads by Continue after a cold start ([its note](dm/games/01009D901BC56000/README.md#android)). Built from the cartridge's base game 1.0.0 (Unity 2022.3) with a profile derived from 1.1.0's (Unity 6); all 16,718 of its shaders translated. The opening movie froze on its 3,566th of 3,570 pictures (118.8 s) on the S25 Edge and the Ayn Odin 2 until 2026-11-12; with the fix it plays to its end on the Odin 2 and on a Galaxy Z Flip 7, and the game goes on by itself, to the treehouse ([Movies](docs/reference/movies.md#android-an-h264-movie-stops-a-few-pictures-before-its-end); the S25 Edge has not run it). The Odin 2 (Adreno 740) runs 1-1 at 60 fps too, and the game app stays installed there for a person to play. Not shown playable: a real-time platformer, so its stretch (1-1 to its end) needs a person with a controller. Not tried on the other devices |
| Pokémon Legends: Arceus 1.1.1 | Samsung Galaxy Z Flip 7, OnePlus Pad 3, Samsung Galaxy Z Fold 8, Google Pixel 9 Pro XL | Playable – major issues through Prelude Beach's catching tutorial: at 60 fps, Android's target for this game since [#330](https://github.com/enjoyerofslop/droidplay-core/pull/330) (frame step stubs; the Switch runs it at 30). On the Samsung Galaxy Z Flip 7 (Xclipse 950), from a new game played by hand over adb at 47d67b7d: the language, the name, Arceus's opening and the fall to Prelude Beach, Professor Laventon and the three Pokémon, the Arc Phone and the view of Mount Coronet, and all three Pokémon caught (Oshawott, then Cyndaquil behind the rocks, then Rowlet under the pines); a save from the game's menu, a cold start and Continue back by the rocks. The opening runs at 60 fps; in the field the Flip holds 30 in dialogue and 38–56 fps while walking (GPU 12–17 ms a frame), a median of 34.2 fps over the first 32 minutes and 34.8 over the 47 minutes on the beach, with game time at real time (0.99–1.00 of it) as the frame step moves between 1 and 2 vsyncs; sound running (no underruns; not listened to), one red line (a depth-stencil clear with no depth target set), no crash or kill, 1.6 GB resident ([its note](dm/games/01001F5010DFA000/README.md)). Issues that keep it from Playable – minor issues: the field doesn't hold 60 on the Flip (one 30 s window of it over 57); Jubilife Village, reached after the catches and saved in, runs at 18–29 fps on the Flip (median 25, under half the target; GPU 20–24 ms a frame for 700–1,260 draws), and on Laventon's walk through it, held at 3 vsyncs a frame, game time ran at 0.96–0.97 of real time; the game's first battle comes the morning after Jubilife and wasn't reached; on the Flip, rocks near the camera show contour-like stripes that the Pad 3's and the Mac's pictures of the same rocks don't have. Before the 60 target, at v0.8.2: on the OnePlus Pad 3 (Adreno 830), the same stretch from a new game at 30 fps (median 29.9 over 32 minutes, the view of Mount Coronet at 21–26), with a save and Continue; at the 60 target its field runs at about 30 (2 vsyncs a frame, game time at real time, checked from a save for #330). On the Flip at 30, the same stretch at 29.9 and Jubilife at 24.4, and loaded from the Flip's saves on the Mac, the title's beach, the field facing Mount Coronet and the village at night look the same as on the Flip. From someone else's saves, which don't count toward the tier, on the Pad 3 and the Flip, played step by step over adb for about 12 minutes each at 30: walking and riding in the Coronet Highlands and the Obsidian Fieldlands, fast travel, a battle and a catch, the Satchel and Save menus, and a save made on Android loaded again after a cold start (Pad 3: median 30.1; Flip: median 29.9, worst 30 s window 21.8 while 248 pipelines were first built). Earlier, at 30: on the Galaxy Z Fold 8 (Adreno 840, its cover display), by hand from a new game through the opening to walking Prelude Beach (median 29.9 over 18 minutes), 1.6 GB resident and no kill by Android 17's memory limit; the picture was washed out and shadowless on Android until the vertex shaders' unwritten outputs were started as (0, 0, 0, 1) ([Unwritten components of a written output start as (0, 0, 0, 1) on Android too](docs/reference/shaders.md#unwritten-components-of-a-written-output-start-as-0-0-0-1-on-android-too)); with that, the beach matches the Mac's picture on the Fold 8. On the Pixel 9 Pro XL (Mali-G715) the same opening runs at 30 fps, with the characters and Pokémon drawn too: the game skins them with transform feedback, which on a GPU without vertex-stage storage writes is now [captured in the fragment stage](docs/reference/rendering.md#transform-feedback-captured-in-the-fragment-stage-android) (checked through the Prelude Beach dialogue). Not run at 60 on the Fold 8 or the Pixel. Built with the phone decode of the game's Oodle archives ([#320](https://github.com/enjoyerofslop/droidplay-core/pull/320)) |
| Pokémon Legends: Z-A 2.0.2 | Ayn Odin 2, Google Pixel 9 Pro XL | Playable – major issues through the opening onto the Gare de Lumiose platform: on the Ayn Odin 2 (Adreno 740), over adb from a new game through the language, character creation and the opening to play on the Gare de Lumiose platform, at 30 fps (27.2–30.4 in 30 s windows), with sound (no underruns) and 2.1 GB PSS, the picture matching a Mac run of the same build; a save made with R loads by Continue after a cold start ([its note](dm/games/0100F43008C44000/README.md#android)). First built from the cartridge's base game 1.0.0 (Switch 1 edition) with a profile derived from 2.0.2's (23,209 shaders); update 2.0.2, the registered build (28,603 shaders, all translated), then ran the same: the save made on 1.0.0 loads on it, 30 fps in every 30 s window, and a save made on 2.0.2 loads by Continue after a cold start. Installed through DroidPlay: its 2.49 GiB of shaders don't fit a game app. Not yet Playable – minor issues: the stretch ends before the station's ticket gate, so it doesn't take in the first battle, and the battles are real-time, so they likely need a person to play them. On the Google Pixel 9 Pro XL (Mali-G715) the platform was drawn nearly black at 20 fps on both versions: Arm's driver drops a layer of 8 or more from a vertex shader, so the colour-grading LUT was mostly unwritten. With the fix it is lit as on the Odin 2, at 20–21 fps, from a save made on the Pixel and loaded by Continue after a cold start (In-game there: not yet played by hand, sound not checked at that frame rate); its memory peaked at 1.9 GB, far under Android 17's limit. Not tried on the other devices |
| Xenoblade Chronicles 2 2.1.0 | Samsung Galaxy S25 Edge, Samsung Galaxy Z Flip 7, Moto Razr Plus 2023 | Playable – major issues through Argentum and the first save: on the Galaxy S25 Edge (Adreno 830), at v0.8.2, played by hand over adb from a new game (Normal): the opening (the Cloud Sea, the salvage dive, Rex and Gramps), King Crustip's tutorial battle won with auto-attacks and the Anchor Shot art, the scenes after it, the arrival at the Argentum Trade Guild and its field tutorials, and walking at Goldmouth Return Port; then System → Save, a cold start and Continue back at the same spot. 30 fps as on the Switch through the opening and the battle (29.9 over 15.5 minutes, worst 30 s window 28.9), with sound running (no underruns; not listened to), no red line, no crash or kill, up to 3.3 GB PSS ([its note](dm/games/0100E95004038000/README.md#android)). Issues that keep it from Playable – minor issues: Argentum runs at 10–16 fps looking into the town (2,200–2,800 draws, GPU 38–62 ms a frame, against about 20 elsewhere) and at 30 looking out over the docks, about 20 fps over its 6.5 minutes. A dark stipple and stripe pattern over the characters and much of the world (crates, walls, Gramps's scales), which the Mac's picture doesn't have, is gone since texture slots in stencil mode read the stencil (checked on the S25 Edge and the Galaxy Z Flip 7, 30 fps). Earlier, no character was drawn on any phone until the replay ran draws under rasterizer discard that write memory (its GPU skinning); with that fix the Moto Razr Plus 2023 (Adreno 730) played through King Crustip's battle at a steady 30 fps too, and before it the Galaxy Z Flip 7 (Xclipse 950) reached King Crustip at 30 fps. Built from the cartridge's 2.1.0 (registered) with a shader-only derived profile, as the dump has no DLC; all 12,738 shaders translated. Not tried: battles after the first, and the other devices |
| The Binding of Isaac: Afterbirth+ 1.7.9b | POCO X5 5G | Boots: runs from a provisional profile with its Repentance DLC, its shaders compiled at run time and captured on the phone; how far it got isn't recorded † |
| Crash Bandicoot N. Sane Trilogy 1.0.0 | Samsung Galaxy Z Fold 8 | Boots: the build gets through since the rewriter learned one more form of switch table ([#446](https://github.com/enjoyerofslop/droidplay-core/pull/446), in 0.8.7). On the Galaxy Z Fold 8 (Adreno 840, its cover screen) the game reaches its main at a steady 30 fps with sound and shows its copyright notice, then stays black: it compiles 16 of its shaders while it runs (its own GLSLC), and the first run captured 2, so their draws are skipped until **Update shaders from the phone** brings them in. Not tried further |
| Xenoblade Chronicles 3 1.0.0 | Ayn Odin 2, Samsung Galaxy S25 Edge, Samsung Galaxy Z Flip 7, Moto Razr Plus 2023 | Playable – major issues through the opening, its first four battles and the Everblight Plain to a save: on the Ayn Odin 2 (Adreno 740, driver V@0676.0), with a game app from `main` at 3d9f479c, played by hand over adb from a new game (Normal). It covers the opening's cutscenes, four battles all won (auto-attacks, arts, a Role Action and the Talent Art), and the plain by day toward Colony 9; then System → Save, a cold start and Continue back at the same spot. 30 fps as on the Switch in every one of 276 ten-second windows (GPU 11.5 ms at 680 MHz), with sound running (no underruns; not listened to), no crash or kill, 2.9–3.3 GB PSS. The Galaxy Z Flip 7 (Xclipse 950) and the Moto Razr Plus 2023 (Adreno 730) played the same route, save and Continue included, on `main` at 021fc6b6 with the replay-wait fixes #367–#369. The Z Flip 7 held 30 fps in 280 of 294 windows (GPU 18.6 ms); the Razr's cutscenes run at 30 fps but its battles and the plain at 17–28 fps, bound by its GPU. On the Galaxy S25 Edge (Adreno 830) the opening, the first battle and the plain run at 30 fps (GPU 24.6–24.9 ms); before #367–#369 its gameplay ran at 5.6–7 fps. Issues that keep it from Playable – minor issues: one red line a session (a transform-feedback vertex shader with no capture variant, whose effect isn't found); the save slots' thumbnails are black; the picture is compared with the Mac's only in the first battle; on the plain by day the Z Flip 7's GPU is 85–95% busy and some windows drop to 23–28 fps; the Razr's battles run at 17–28 fps ([its note](dm/games/010074F013262000/README.md#android)). Built from the cartridge's base game 1.0.0 with a profile derived from 2.2.1's; all 26,907 of its shaders translated. Not tried: past the plain, the other devices |
| Metroid Dread 1.0.0 | Google Pixel 9 Pro XL, Moto Razr Plus 2023, Retroid Pocket 5 | In-game: on the Pixel 9 Pro XL (Mali-G715), the Razr Plus 2023 (Adreno 730) and the Retroid Pocket 5 (Adreno 650), played over adb from a new game through the opening movie, the briefing and the gunship's approach into control in the first cavern, then walking, jumping and shooting through the next caverns and a fight with the crawlers there. 60 fps on the Pixel (GPU 9–10 ms, 2.6 GB at most), 52–60 on the Razr (52–53 in the caverns, 2.5 GB PSS), 53–58 on the Retroid Pocket 5 (2.6 GB PSS). The first room looks as on the Mac; one red line (a depth-stencil clear with no depth target); sound with 2 short underruns in 152 reports on the Pixel, none on the other two. Packed from a profile derived from 2.1.0 (the cartridge holds 1.0.0). Not tried: saves (no save station reached), past the first caverns, a person playing ([its note](dm/games/010093801237C000/README.md#android)) |
| Metroid Prime Remastered 1.0.0 | Moto Razr Plus 2023, Google Pixel 9 Pro XL | In-game: on the Razr Plus 2023 (Adreno 730), from a new game over adb (the menus by frame-keyed presses) through the briefing and the gunship's approach into control on the Frigate Orpheon's landing deck, walked to the lock-on tutorial, at 60 fps (GPU 10 ms), 2.0 GB PSS, with no red line but the virtual pool remaps. Issues: the force fields draw as a jumble of blue shapes instead of a translucent cyan panel, and the arm cannon is dark. Too big for a game app (2.66 GiB of shaders), so installed through DroidPlay. Before the guest block was 32 MB-aligned it crashed at start on Android. On the Pixel 9 Pro XL (Mali-G715) the title is nearly black and the driver crashes building a pipeline about 3 minutes in. Not tried: sound, saves, past the first room ([its note](dm/games/010012101468C000/README.md#android)) |
| Bayonetta 2 1.0 | OnePlus Pad 3 | In-game: 60 fps (the game's rate; 30 in its cutscenes), played by hand from a new game through the opening, the tutorial and Verse 1's first fights on the jet in Easy's Automatic Mode, with sound (an audio dump) and the auto-save, which Load Game lists after a restart ([the test](dm/android/baselines.md#a-first-test-bayonetta-2-on-a-oneplus-pad-3)). Built from the base game with a profile derived from 1.2's. Loading the save wasn't seen through (another run took the device), so not Playable yet; not tried past Verse 1's first fights, in Normal, or on the other devices |
| **Mario Kart World 1.8.0 (Switch 2)** | OnePlus Pad 3, Galaxy S25 Edge with the Turnip T30 driver, Google Pixel 9 Pro XL, Samsung Galaxy Z Flip 7, Ayn Odin 2 | **In-game, blocked** (2026-11-11, on the OnePlus Pad 3 with Qualcomm's own driver): the title, the menus by adb input, and a Mushroom Cup race at Mario Bros. Circuit, driven and steered, at 20–26 fps in the race (frames of 34–50 ms; the replay's CPU 23–38 ms, the GPU 18–29 ms) with raw global memory, which its profile turns on, and 15–22 fps without, the menus at about 30 fps. Since its profile also runs the indirect dispatches on the GPU's copy of their counts (2026-11-12, on 0.8.4's host), the race runs at 27.3 fps on the Pad 3 (24.8 without that) and 24.1 on a Samsung Galaxy Z Flip 7 (19.5); a Google Pixel 9 Pro XL ran it at 28.8–29.2 fps either way, but froze once in two launches with it and crashed in Mali's driver once in three without. The track untextured (sparse textures), no roadside bushes (no mesh shaders). The Galaxy S25 Edge reached the same race with Mr. Purple's Turnip T30 through `DM_VK_DRIVER`. 168 of its compute shaders declare 48 KB of shared memory, past the 32 KB that Qualcomm's driver takes: they now take it from a storage buffer there; without that the GPU is lost about 10 s after the title. The faults now and then in the menus and races (in Qualcomm's driver on the Pad 3, in the replay on the S25 Edge, in Mali's driver on a Google Pixel 9 Pro XL) came from holes that a refused virtual pool mapping left in the pool's reservation, fixed by [#298](https://github.com/enjoyerofslop/droidplay-core/pull/298): with it, the Pixel 9 Pro XL went through the menus and ran a race out in 4 runs of 4 (all 4 failed before), at about 10 fps in the race (frames of 97–99 ms, the GPU 45 ms), and the S25 Edge ran two more races without a fault. The Odin 2 (2026-11-08): the world map at about 55 fps, the attract drive at about 3.4 fps. Open: memory (0.5–0.7 GB left on the 12 GB Pad 3 in a race; faster runs with raw global memory were killed; on 2026-11-12, OnePlus's leak killer ended 4 of 7 launches on the Pad 3 at the race load, at about 8 GiB), sparse textures, virtual memory pools, no mesh shaders |
| **The Legend of Zelda: Ocarina of Time v131072 (Switch 2)** | Samsung Galaxy Z Fold 8, Samsung Galaxy S25 Edge, OnePlus Pad 3, Samsung Galaxy Z Flip 7, Google Pixel 9 Pro XL, Ayn Odin 2, Moto Razr Plus 2023 | **In-game, blocked:** under half the target frame rate (60 fps). From a Mac save, Kokiri Forest, Saria's scene and the Sacred Grove's Mad Scrub fight draw lit on the Adreno 830 and 840 with no kill (Galaxy S25 Edge 3 runs of 3, Galaxy Z Fold 8 5 launches), at 13–18 fps: on the Fold a median of 15–17 fps in Kokiri Forest and Saria's scene, about 20 in the Grove. Measured since with the replay's CPU and the GPU's work brought down ([its note](dm/games/0400486024DEC000/README.md#android)), Kokiri Forest runs at 29.1 fps on a OnePlus Pad 3 (Adreno 830 at 1,100 MHz; 30.0 with the launcher's Fewer plants setting, off by default, which leaves out its ferns, flowers and tree canopies), 19.0 on the Galaxy S25 Edge, whose GPU clock stays at 525 MHz, and 19.9–21 on the Galaxy Z Fold 8, by how warm it is. The Galaxy Z Flip 7 (Xclipse 950) draws it lit at about 12 fps, 8.25 GB at most; the Pixel 9 Pro XL (Mali) lit at about 9 fps. On older Qualcomm drivers for the Adreno 7xx (the Odin 2's 740 at V@0676, the Razr's 730 at V@0615) the characters are dark: the driver rejects the pointer conversion (`OpConvertUToPtr`) raw global memory uses; the 740 with Turnip T30 draws them lit at about 9 fps; raw global memory through a descriptor array builds the shaders there, but with them the game no longer fits the Odin 2's memory, so its profile keeps that off. The pale, flat water patches in Kokiri Forest were terrain tiles left undrawn, whose maps the game fills from the CPU before it makes their textures: they draw since [#315](https://github.com/enjoyerofslop/droidplay-core/pull/315) (checked on the Galaxy Z Fold 8). Open: the frame rate (the CPU's submission, about 20 µs a draw), the mesh-shaded foliage left out (no mesh stage on Vulkan yet), and a stall of about 10 s at the first start while the pipelines build |

† Tested before DroidPlay Core merged nxnative's `main` (2026-11-08); not run since. "Not run since" with a date marks a later run on code `main` has moved on
from, and "contributor report" a run on a contributor's own device.

The rows for the first six games, Final Fantasy Resonance and Breath of the Wild are from the [Android baselines](dm/android/baselines.md): the same scripted runs on
each device, with frame times, the devices' drivers, and the known problems with what is known of their causes. The first were measured on 2026-11-08 on the Odin
2, the Pad 3 and the Retroid Pocket 5. On 2026-11-10 the Galaxy S25 Edge's column and the rows for Final Fantasy Resonance and Breath of the Wild were added, and
the Retroid Pocket 5's rows were measured again; Ocarina of Time on the Galaxy Z Fold 8 and Super Mario Odyssey on the Pixel 9 Pro XL have sections of their own
there; cells measured again since are dated (Smash's battles, Link's Awakening's Ancient Ruins, Skyward Sword HD's Skyloft). The other rows are
from the games' own notes, which the [games table](dm/games/README.md#games) links, but for two: SHIN MEGAMI TENSEI V: Vengeance's device is named in its
contributor's pull request ([#101](https://github.com/enjoyerofslop/droidplay-core/pull/101)), and
the POCO X5 5G rows are from the original port's README.

Every other game in the [games table](dm/games/README.md#games) has a profile and can be packed the same way, but has not been run on Android. A game without a
profile can still be packed as an *experimental build*: most likely it does not boot, but it is the starting point for porting it.

The Android host replays NVN only. A game that draws with OpenGL or makes its own Vulkan calls (OpenGL or Vulkan in the games table's Graphics column) cannot
draw on Android, since the OpenGL and Vulkan layers are the Mac's, on Metal: Balatro, Digimon Story Cyber Sleuth, Street Fighter 30th Anniversary Collection and
Well Dweller (OpenGL), and Hades and both Super Mario Galaxy games (Vulkan). The packing script stops on such a game and says why, before the rewrite and the
shaders. Super Mario 3D All-Stars still builds, since its launcher draws with NVN, but Super Mario 64 and Sunshine inside it call Vulkan and cannot draw.
The games the script refuses are in the Doesn't run tier. Street Fighter 30th Anniversary Collection stopped at its first EGL call on a Retroid Pocket 5
before the script refused it ([its note](dm/games/0100024008310000/README.md#android)).

Games that keep their shaders in archives compressed with the game's own Oodle need the phone connected for their first build: Pokémon Legends: Arceus,
Pokémon Legends: Z-A, Bayonetta 3, Batman: Arkham Knight and EA SPORTS FC 27. The packing script decodes those archives with the game's own decoder on the
phone before it splits the shaders, which adds minutes and keeps the decoded streams (gigabytes) in the game's build folder for later builds; on a Mac with
DroidPlay for Mac's build of the game, its Mac host does it instead. Without either, the script stops at the start and says so
([#310](https://github.com/enjoyerofslop/droidplay-core/issues/310)). Arceus and Z-A, built this way, have rows in the table above; the others have not been run on Android yet.

## Help and reports

- **Ask in the [DroidPlay Discord](https://discord.gg/3r92f8wmh7):** #help for a problem, #compatibility for how a game runs on your device. Its rules are
  this README's: no games, keys, firmware or packed APKs.
- **When a build fails,** DroidPlay for Windows says why in plain words, with the button that fixes it where there is one. Otherwise **Copy report** on the
  Builds page copies a short report to paste in #help; it leaves out your user folder, the phone's serial and anything that looks like a key.
- **A useful report** names the device and its GPU (Settings, or `adb shell getprop ro.product.model`), Android version, the game, its title ID and version,
  how it was packed (DroidPlay, the command line, game app or launcher), and attaches the log: `adb logcat -s DroidPlay` (`-s nxnative` for an app older
  than 0.5.0), or `log.txt` in the app's folder (`Android/data/<package>/files/`). A screenshot and the first `[RED]` lines help most. Never attach game
  files or keys.
- **Testers and contributors** can also open an issue with the bug report form ([Reporting a problem](CONTRIBUTING.md#reporting-a-problem)).

## Guides

- [DroidPlay for Windows](docs/android.md#droidplay-for-windows): installing, Get started and every page of the app, in detail.
- [DroidPlay for Linux (preview)](docs/android.md#droidplay-for-linux-preview): the AppImage, what it needs, where it keeps its files.
- [The Android app](docs/android.md#the-android-app): the launcher, a game's settings, the first start, controls, saves, removing a game and the log.
- [Game apps](docs/android.md#game-apps): a game as an app of its own.
- [Another Vulkan driver (Turnip)](docs/android.md#another-vulkan-driver-turnip): on an Adreno or Xclipse GPU, a driver you put on the phone.
- [Games that compile their shaders while they run](docs/android.md#games-that-compile-their-shaders-while-they-run): The Binding of Isaac, for one.
- [Packing from the command line](docs/android.md#packing-from-the-command-line): what it needs, and its options.
- [Seeing what a game draws](docs/android.md#seeing-what-a-game-draws): screenshots, frame dumps and thread states from a running game.

## How it works

A Switch and an Android phone both run ARM64 code, so there is no CPU emulation. The game's code is rewritten only where the host needs it (system calls,
thread registers), a host answers the Switch's system calls and SDK services, and the game's NVN graphics are replayed on the GPU through Vulkan, with its
shaders translated to SPIR-V when the game is built.

### How it fits together

```
 Your computer (Windows, macOS or Linux)                         Your Android phone
 ───────────────────────────────────────                         ─────────────────────────────────────
 NSP / XCI files + prod.keys                                     DroidPlay app (org.nxnative.android)
          │                                                        ├─ Launcher: the games on the phone,
          ▼                                                        │    each game's settings, Play
 Desktop app: scripts/nxnative_android.py                          │
 (or the command line: scripts/build_android_from_nsp.py)          └─ Game screen, in its own process:
          │  1. extract the game (nx_extract)                           loads that game's host
          │  2. rewrite its code for Android (patch_nso.py)             (libdmhost_<title ID>.so), which runs
          │  3. split its shaders, translate them to SPIR-V             the game's code and replays its
          │  4. build the game's host with the Android NDK              graphics on Vulkan
          │  5. pack one APK carrying every game built so far
          └─ adb: install the APK, copy the game files ──────▶   Android/data/org.nxnative.android/files/games/<title ID>/
```

One app holds every game: each game you build adds its host to a library on your computer, and every APK is packed from the whole library, so building a second
game keeps the first one in the app. Game files (code, shaders, RomFS, icon) live in the app's folder on the phone; saves stay in the app's internal storage.

### What the Android port changed

The iOS / Mac host runs on Android with stand-ins for the Darwin interfaces it calls (`dm/android/compat`); the guest's thread slots go through the x18 register (the
rewriter emits `mov xN, x18` for `DM_TARGET=android`). NVN is replayed on Vulkan (`dm/android/gfx_vk.c`, `gfx_vk_draw.inc`): textures (BC decoded on the CPU where the
GPU lacks it), copies, clears, draws, compute, indirect draws, transform feedback, queries and timestamps, submitted in chunks so the GPU works while the CPU records;
shaders are translated with the project's translator and adjusted for Android drivers (`dm/android/tools/spv_android.py`). Audio goes through AAudio, movies through
MediaCodec. Details of the port: [dm/android/README.md](dm/android/README.md).

Problems found on the way, each with its cause and fix in the reference ([rendering](docs/reference/rendering.md#android-vulkan-replay), [memory](docs/reference/memory.md),
[NVN objects](docs/reference/nvn-objects.md)):

- **Wrong pictures:** the Adreno driver misreading dynamic vector components in uniform buffers (vertical stripes); render targets attached in their sRGB view format
  (washed-out colours); `char` unsigned on Android (noise in clouds); separate textures read as "no texture" because the texture and sampler banks are packed into one
  field (Danganronpa V3's black screen); a second NVN device resetting the window origin (an upside-down picture); a 3D colour-grading LUT drawn a slice at a time into
  one slice (Bowser's Fury's blue scene).
- **Crashes:** the thread stack reported with bionic's guard page, which Unity's garbage collector scanned (Danganronpa V3 at boot); `pthread_exit` run on a game
  thread's small stack (Bowser's Fury after a few seconds).
- **Speed:** syncs limited to what a draw reads, passes kept open, early and mid-pass submissions, constant buffers copied only as far as the shader reads, pipeline keys
  built from the state fields, a capped pipeline prebuild; *Game time* holds the audio to the game's frames for levels timed to the music.

## Contributing

DroidPlay Core is nxnative with an Android port. Android work belongs here; a fix for the Mac or the iPhone goes to
[seedds/nxnative](https://github.com/seedds/nxnative) first and reaches DroidPlay Core with the next merge. nxnative's `main` is merged in regularly, so
its games, fixes and Switch 2 work reach Android too; nxnative itself stays Apple-only by its maintainer's choice. Testers and contributors are welcome.

- [CONTRIBUTING.md](CONTRIBUTING.md#droidplay-core): how a change gets in, the checks to run, and how DroidPlay Core stays in step with nxnative.
- [dm/android/README.md](dm/android/README.md): the Android host, the Vulkan replay, packing, game apps and the desktop app.
- [Android baselines](dm/android/baselines.md): the same scripted runs on each device, with frame times, the drivers and the known problems.
- [The games table](dm/games/README.md#games) and each game's note: what its profile needs, what was found on it and what is open.
- [AGENTS.md](AGENTS.md) and [docs/agents](docs/agents/README.md): where to start on a crash, a hang or a wrong picture.
- [docs/development.md](docs/development.md): nxnative's internals, the packing script's options and building by hand.

## Credits and license

- [seedds/nxnative](https://github.com/seedds/nxnative) runs Switch games natively on the iPhone, the iPad and the Mac; DroidPlay Core is nxnative with an
  Android port. nxnative's [acknowledgements](#acknowledgements), below, name the work both stand on.
- [RogelioLB/nxnative-android](https://github.com/RogelioLB/nxnative-android) began the port: the Vulkan replay of NVN, the Android front end and the packing
  script.
- DroidPlay Core was called droidnative until 2026-11-09, and was renamed so as not to be confused with
  [DroidNative](https://github.com/DroidNative/DroidNativeApp), which runs Linux desktop apps on Android
  ([#40](https://github.com/enjoyerofslop/droidplay-core/issues/40)). Links to the old name lead here.
- MIT ([LICENSE](LICENSE)). Third-party code keeps its own license ([dm/third_party](dm/third_party/README.md)).

Below the line is nxnative's own README, merged in from nxnative with the rest of its changes: building games for the iPhone, the iPad and the Mac.

---

# nxnative

Play your own Nintendo Switch and Switch 2 games natively on Apple silicon Macs, iPhone and iPad. One script packs your game into a Mac app or an unsigned IPA that you sideload.

It is not an emulator: the game's own code runs directly on the Apple chip, with Switch graphics rebuilt on Metal. Nothing from any game is in this repository, and the project is not affiliated with Nintendo or any game publisher.

[Games](#games) · [Community front ends](#community-front-ends) · [Requirements](#requirements) · [How to play](#how-to-play) · [Discussions](#discussions) · [For developers](#for-developers)

## Games

The full [tested games table](dm/games/README.md#games) includes original Switch games, tested builds, engines and graphics APIs. Each game's note records its tested platforms, progress and known issues.

These Switch 2 games have passed Mac gameplay testing. Build them from a [`.nup` file or a dump folder](#switch-2-games):

- [Donkey Kong Bananza](dm/games/040074A01BF12000/README.md)
- [Fire Emblem Fortune's Weave](dm/games/04007EF01F8AE000/README.md) — up to 120 fps
- [Hyrule Warriors: Age of Imprisonment](dm/games/04003F40232DE000/README.md)
- [Kirby Air Riders](dm/games/040077201BE50000/README.md)
- [Mario Kart World](dm/games/0400C3F00006E000/README.md)
- [Mario Tennis Fever](dm/games/0400D4201C6EA000/README.md)
- [Pokémon Pokopia](dm/games/0400A3A01EE4C000/README.md)
- [Splatoon Raiders](dm/games/0400380020056000/README.md)
- [Star Fox](dm/games/0400E88025B1A000/README.md)
- [The Legend of Zelda: Ocarina of Time](dm/games/0400486024DEC000/README.md)
- [Yoshi and the Mysterious Book](dm/games/0400245021D2C000/README.md)

## Community front ends

Community projects build on nxnative with tools to turn your games into apps for your iPhone, iPad or Mac without the command line, and libraries to organize and launch them. They are separate projects: questions and bugs about them go to their own pages.

- [NX2Apple](https://github.com/LNPANDY/NX2Apple) — Mac builder with a shared iPhone/iPad game library.
- [NX Play](https://github.com/varyform/nx-native-wrapper) — Mac library with device installs and save sync.
- [NxNative Launcher](https://github.com/rennan101/nxnative_Launcher) — Mac launcher with guided builds and controller mapping.
- [LumaNX](https://github.com/Xc1Ym/LumaNX) — Mac game builder with IPA export.
- [GameCase](https://github.com/Tiaoge-tou/GameCase) — iOS game library based on LiveContainer.

## Requirements

- **Mac:** Apple silicon, macOS 26.6 or later. Building games requires Xcode 27.2.
- **iPhone:** A17 Pro chip or later, with iOS 26 or later.
- **iPad:** M1 chip or later, with iPadOS 26 or later.

The games' textures are BC-compressed, which older iPhones cannot read. Switch 2 builds are tested on the Mac; check each game's note for device-specific validation.

For a game without a profile, the script writes a survey and tries a provisional build with guessed settings. That build may not start; see [Port another game](docs/development.md#port-another-game).

## How to play

Switch 2 games are built from one `.nup` file or a folder of their files, not from NSPs, and need no `prod.keys`: see [Switch 2 games](#switch-2-games). On Android they are at an early stage ([Game Status](#game-status)); in DroidPlay for Windows, **+ Build game** picks the folder that holds the `.nup` or the dump.

### Build from the command line

You need a Mac with macOS 26.6 or later, [Xcode](https://developer.apple.com/download/all/?q=Xcode%2027.2) 27.2, [Homebrew](https://brew.sh) and Python 3.12 or newer (an arm64 one, not an Intel Homebrew's). An iPhone build also needs Xcode's iOS platform (Xcode → Settings → Components).

The shaders are built with Xcode 27.2's Metal compiler. Select it before step 1: `sudo xcode-select -s /Applications/Xcode.app`. With several Xcode versions installed, name this one `Xcode-27.2.app` and select it with `sudo xcode-select -s /Applications/Xcode-27.2.app`.

**1. Install the tools** (only once), in Terminal:

```
xcodebuild -downloadComponent MetalToolchain
git clone https://github.com/seedds/nxnative.git && cd nxnative
brew install xcodegen zstd glslang
brew install --cask dotnet-sdk
python3 -m pip install lz4 "capstone>=5,<6" fonttools nsz zbic "backports.zstd; python_version < '3.14'"
```

**2. Put your game in a folder**: the game, plus its update and DLC if you have them (`.nsp`, `.xci`, `.nsz` or `.xcz`):

```
~/Games/MarioWonder/
  Super Mario Bros. Wonder [010015100B514000][v0].nsp
  Super Mario Bros. Wonder [010015100B514800][v327680].nsp
```

**3. Build**, with your game folder and your `prod.keys`:

```
python3 scripts/build_ipa_from_nsp.py ~/Games/MarioWonder -k ~/keys/prod.keys
```

The IPA appears in the game's folder. The first build takes 30–60 minutes and needs free disk space of about twice the game's size plus 10 GB; building again is much faster.

To play on the Mac instead, add `--platform mac-app`: you get a Mac app of the game to open from Finder. All options: [The packing script in detail](docs/development.md#the-packing-script-in-detail).

**Android.** To play on an Android phone instead, see [the top of this page](#droidplay-core): a desktop app packs the game and installs it on the phone.

### Install

Install the IPA on your iPhone or iPad with a sideloading tool such as AltStore, Sideloadly or LiveContainer. If your tool offers **Increased Memory Limit**, turn it on: the bigger games need it.

Game too big to install? Build again with `--romfs separate`, and run the command the script prints once to copy the game files to your phone ([Quick reinstalls](docs/development.md#quick-reinstalls)). With AltServer, also add `--shader-files`: it cannot install a file of 2 GiB or more.

Controls, saves and settings: [Playing](docs/playing.md).

### Switch 2 games

A Switch 2 game is built from one [NUP](https://github.com/a55uka/nup) container (a `.nup` file) or a folder of its files, not from NSPs. Neither needs `prod.keys`. A game this project has no profile for is surveyed and gets a provisional profile, as NSPs do ([Port another game](docs/development.md#port-another-game)), which may not boot. You get a Mac app; iPhone and iPad are untested.

**From a `.nup` file**, which holds the base game, its updates and DLC in one file. Give the file, or a folder that holds it:

```
python3 scripts/build_ipa_from_nsp.py ~/Games/DonkeyKongBananza.nup --platform mac-app
```

[Hyrule Warriors: Age of Imprisonment](dm/games/04003F40232DE000/README.md) is played this way: its 47 GB NUP unpacks in under five minutes. A NUP that carries the game's control data (its NACP) and icon gives the app the game's name and icon; otherwise the name comes from the game's profile and the app has no icon unless you give it one with `--icon <image>` (`ICON` in Spyder; a JPEG or PNG of any size).

A NUP without the base game also works, when its base package is empty or it has none and its updates are marked `standalone_update`: the update is taken as the whole game, and the script says so. If the game then stops on missing files, that update holds only what changed: pack a NUP with the base game. The newest update is selected by default; `--package v<N>` selects another version in the container. Such a container has no `v0` (base game) to select.

**From a folder of files**. The script expects this layout (`updates` and `dlc` are optional):

```
~/Games/DonkeyKongBananza/
  base/exefs/                     main, main.npdm, sdk, rtld
  base/romfs/
  updates/458752/exefs/           the update's version, as a decimal number
  updates/458752/romfs/           the patched RomFS, not a patch
  dlc/040074a01bf13001/0/romfs/   the DLC's title ID
```

```
python3 scripts/build_ipa_from_nsp.py ~/Games/DonkeyKongBananza --platform mac-app
```

An update's folder alone will do (no `base/`), and so will `exefs/` and `romfs/` directly in the folder, as `nup extract` writes them, which count as `base/`. The app has no icon, because the folder holds none; `--icon <image>` (`ICON` in Spyder) gives it one. To turn such a folder into a NUP: `nup pack ~/Games/DonkeyKongBananza game.nup`.

## Discussions

Questions, how a game runs on your Mac or iPhone, and ideas go in [Discussions](https://github.com/seedds/nxnative/discussions); [Issues](https://github.com/seedds/nxnative/issues) are for bugs, with the log. Ocarina of Time's performance (frame rate, resolution, which Macs hold 60 fps) is followed in [Ocarina of Time: state on main](https://github.com/seedds/nxnative/discussions/1789).

## For developers

[How it works](docs/development.md) · [Game notes](dm/games/README.md#games) · [Bring-up and debugging](docs/agents/README.md) · [Front ends](docs/frontends.md) · [Contributing](CONTRIBUTING.md)

## Acknowledgements

This project is built on other people's work ([licenses](dm/third_party/README.md)):

- **[d2r-mobile](https://github.com/jhaizhou-ops/d2r-mobile)** (MIT) developed this native-execution approach, for Diablo II: Resurrected. nxnative started as a copy of its generic parts: the NSO rewriter, the host that stands in for Horizon, the NVN-on-Metal layer and the iOS app shell. Fixes go both ways.
- **Ryujinx** (MIT): its shader translator (`Ryujinx.Graphics.Shader`, `Ryujinx.Common` and `Spv.Generator` 1.3.2, in `dm/third_party/ryujinx` with a few fixes listed in [its README](dm/third_party/ryujinx/README.md)) turns the games' shaders into SPIR-V. Its emulation of the kernel and system services is also a reference when the host's behaviour is in doubt.
- **[Atmosphère](https://github.com/Atmosphere-NX/Atmosphere)**: its kernel, mesosphère, is the reference for Horizon's synchronization semantics (address arbiter, condition variables).
- **[LibHac](https://github.com/Thealexbarney/LibHac)**: reads NSPs and layers updates over the base game in `dm/tools/nx_extract`.
- **[SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross)**: SPIR-V to Metal Shading Language.
- **[glslang](https://github.com/KhronosGroup/glslang)** compiles the GLSL of OpenGL games to SPIR-V for it.
- **[MacNativeGames](https://github.com/ly05010419/MacNativeGames)** (MIT): the GPU widening of 8-bit indices and in-pass clears in `dm/gfx/nvn/nvn_mtl.m`, and the visibility-counter lowering in the shader translator.

## License

MIT ([LICENSE](LICENSE)). Third-party code keeps its own license ([dm/third_party](dm/third_party/README.md)).
