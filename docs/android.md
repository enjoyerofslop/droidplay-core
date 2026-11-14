# DroidPlay on Android

The [README](../README.md#droidplay-core) has the short path: install DroidPlay for Windows, build a game, play it. This page has the rest: every page of
the desktop app, the app on the phone, game apps, another Vulkan driver, games that compile their shaders while they run, the command line and the files that
ask a running game for diagnostics.

> [!IMPORTANT]
> **Bring your own game.** Nothing from any game is in this repository, and no packed game is distributed: you dump the game and the keys from **your own**
> Switch, and the app you build is for you only. Do not share APKs packed with a game, keys or firmware.

- [DroidPlay for Windows](#droidplay-for-windows): [installing](#installing), [Get started](#get-started), [the pages](#the-pages), [from source](#from-source)
- [DroidPlay for Linux (preview)](#droidplay-for-linux-preview)
- [DroidPlay on a Mac](#droidplay-on-a-mac)
- [The Android app](#the-android-app)
- [Game apps](#game-apps)
- [Another Vulkan driver (Turnip)](#another-vulkan-driver-turnip)
- [Games that compile their shaders while they run](#games-that-compile-their-shaders-while-they-run)
- [Packing from the command line](#packing-from-the-command-line)
- [Seeing what a game draws](#seeing-what-a-game-draws)

## DroidPlay for Windows

DroidPlay for Windows is the desktop app in this repository, `scripts/nxnative_android.py`: it drives the packing script and adb for you. The same app runs on
Linux as an AppImage ([DroidPlay for Linux](#droidplay-for-linux-preview), in preview), and on Linux and a Mac [from source](#from-source). It has
DroidPlay's name and icon, but it is a separate app from [DroidPlay on a Mac](#droidplay-on-a-mac).

### Installing

Download **DroidPlay-Setup-&lt;version&gt;.exe** from the [latest release](https://github.com/enjoyerofslop/droidplay-core/releases/latest) and run it. Windows
warns that it comes from the internet and isn't signed: **More info**, then **Run anyway**.

- It installs DroidPlay for you only, in `%LOCALAPPDATA%\Programs\DroidPlay`, with no administrator prompt and nothing more to download: its own Python with
  the app's modules, and DroidPlay Core (Setup is about 80 MB, 600 MB installed). Each Setup has a `.sha256` beside it on the release page.
- DroidPlay is then in the Start menu, on the desktop, and in **Settings › Apps**, which uninstalls it. Your games and their builds, the Android SDK, the
  tools and your settings stay.
- DroidPlay says when a newer release is out, and **Update** installs it: DroidPlay downloads the new Setup, checks it against the release, runs it
  (a small Updating DroidPlay window shows its progress) and opens again. Or close DroidPlay and run the new Setup yourself.
- **Coming from Install DroidPlay.cmd?** Close DroidPlay and run Setup: it moves your DroidPlay over and removes the copy the .cmd installed, and keeps your
  games, builds and settings. `Install DroidPlay.cmd uninstall` still removes such a copy by hand (`dm/android/windows/install_droidplay.cmd`).

### Get started

DroidPlay opens on **Get started**: four steps, each saying what it needs and whether it's done. Setting up takes about half an hour the first time.

- **Your keys:** pick your `prod.keys`.
- **Tools for building:** **Install tools** downloads Microsoft's JDK 21 and the .NET SDK 10 into the app's own folder (`%LOCALAPPDATA%\nxnative\android\tools`),
  and Google's Android SDK with the NDK, CMake, build-tools, platform and platform-tools into Android Studio's place, `%LOCALAPPDATA%\Android\Sdk` (about 1.5 GB
  to download, 5 GB installed), with one bar and the time left. Installing the Android SDK means accepting its
  [license](https://developer.android.com/studio/terms), which the app asks first; its usage metrics stay off. What the computer already has is kept.
  From a console: `build_android_from_nsp.py --setup --accept-android-licenses` (Windows, and x86_64 Linux, where it installs into
  `~/.local/share/nxnative/android/tools`: [DroidPlay for Linux](#droidplay-for-linux-preview)).
- **Your phone:** plug it in with USB debugging on; the step says when the phone is waiting for you to tap **Allow**.
- **Your first game:** add the folder with its NSP or XCI files, or the folder that holds a Switch 2 game's `.nup` or dump (no keys needed), then **Build and install**. The build works beside the game's files, so their drive needs room:
  a lot of it for a game with many shaders ([Free space](#free-space)).

Until all four are done, a launch opens on the checklist and the top bar's **Setup 2 of 4** opens it; **Settings › Show the setup checklist** does any time.

<table>
<tr>
<td align="center" valign="top" width="36%"><img src="images/windows/phone-setup.png" alt="DroidPlay for Windows' Set up your phone guide for a Samsung Galaxy: Build number, Auto Blocker, USB debugging and Allow, with the phone waiting for Allow at its foot"><br><small><b>Set up my phone</b>: the steps for your phone's brand</small></td>
<td align="center" valign="top" width="64%"><img src="images/windows/build.png" alt="DroidPlay for Windows' Builds page part-way through building Pokémon Brilliant Diamond: four stages done, the shaders for Vulkan translating, with the time left"><br><small><b>Builds</b>: a first build part-way, its stages and the time left</small></td>
</tr>
</table>

### The pages

- **Library:** the games you added (**+ Build game** picks a folder with a game's NSP or XCI files, or a Switch 2 game's `.nup` or dump; Windows' folder picker shows no files, so pick the folder that holds them), each with its box art, version and status on the phone picked at the top.
  The big button does what comes next: **Build and install**, **Install on phone** (a game built before, without building again) or **Play**. **Manage** has the rest:
  **Options** has the game's version, language and **Game data** (Phone storage, or the SD card when the picked phone has one: [the game data on an SD card](#the-game-data-on-an-sd-card)),
  build only, update the app on the phone, remove the game, import a save to the phone, restore its save from a backup ([Saves](#saves)). A game with no profile gets **Experimental build**: the script surveys the game, writes a guessed
  provisional profile into the game's folder and packs it; once built it is marked *Experimental* and can be installed like any other.
- **Builds:** the running build's stages, progress and messages (the bar at the bottom of every page follows it); a build started from a console shows here too.
  When a build fails, a banner says what went wrong in plain words, with the button that fixes it where there is one (**Install again** once the phone has room,
  **Choose prod.keys** for keys older than the game). Otherwise it is likely a bug: **Copy report** copies a short report to paste in #help on the Discord, and
  **Save full report** saves the whole log; both leave out your user folder, the phone's serial and anything that looks like a key.
- **Phones:** adb devices over USB and Wi-Fi (pair, connect, discover), each one's battery and free space, the app version and the games on each phone, saves
  backed up to the computer and put back (**Restore a backup…**). **How to connect a phone** (also the checklist's **Set up my phone**) walks through turning on USB debugging for the phone's brand
  (Samsung's Auto Blocker, Xiaomi's Install via USB, a Pixel's driver) and what to try when the phone doesn't show up; a phone below 20% battery gets a warning,
  since a PC's USB port charges slowly and an install takes minutes. A phone on a cable and on Wi-Fi at once is one phone: the list at the top shows it once, over
  the cable. What DroidPlay keeps for a phone (the one you picked, its backups) follows the phone itself, not its Wi-Fi address, which changes each time wireless
  debugging restarts.
- **Performance:** the game's log from `adb logcat`, with the frame time, GPU and CPU figures the host prints, and a frame's GPU time per pass.
- **Settings:** your `prod.keys`, and the [requirements](#what-the-command-line-needs) with how to install what is missing (on Windows and Linux, **Install what's missing** does it).

A rebuild only does what changed: the extraction, the rewritten code and the translated shaders are reused when their inputs are the same, and the RomFS image is not
copied to the phone again when the phone already holds the same one.

If the phone drops off during an install (it slept, Wi-Fi roamed, wireless debugging restarted on a new port, or the cable came out), the install waits up to
three minutes for it to come back, finds it again by its serial number (over Wi-Fi it connects to it again by itself) and goes on: the game data, the shaders
and large DLC carry on from what the phone already holds, and the other steps run again. Wake the phone, or plug it back in, while the Builds page says it is waiting.

### Over Wi-Fi

DroidPlay can install and play over Wi-Fi instead of a cable, through Android's wireless debugging (Android 11 and later):

1. On the phone, turn on **Settings › Developer options › Wireless debugging**, allow it on your network, and tap **Pair device with pairing code**. Keep that
   screen open.
2. On the **Phones** page, click **Pair over Wi-Fi…**. It finds the phone on the network by itself and fills in its address: type the six-digit code the phone shows,
   then **Pair**. DroidPlay then connects to the phone (adb usually does it by itself within a few seconds), and the phone appears in the list at the top.
   If the phone isn't listed (some networks don't pass the multicast that finds it), type the address shown with the code; if the connection isn't found after
   pairing, it asks for the address on the Wireless debugging screen itself.
3. Install and play as over a cable. A phone already allowed over USB can skip pairing: **Connect…** with the address on its Wireless debugging screen.

The phone and this computer must be on the same network: a guest network keeps devices apart. A failed pairing says why (a wrong code, the pairing screen closed,
another network) in place of adb's own words.

**Discover** lists every phone offering wireless debugging on the network (a phone already connected, over Wi-Fi or its cable, is shown as such) and connects to
the ones you tick. **Disconnect** drops the phone picked in the list (or at the top): a phone connected by address is disconnected at once. A phone adb connected
by itself over the network would be connected again while Wireless debugging is on, so for that one DroidPlay offers to turn Wireless debugging off on the phone.
A phone on its cable is unplugged to disconnect it.

When the phone picked at the top drops off Wi-Fi (it slept, roamed, or Wireless debugging restarted on a new port), the list at the top says **Reconnecting to
<phone> over Wi-Fi…** and DroidPlay looks for it every 20 seconds for ten minutes, by its serial number on the network and at its last address, and connects to
it again. If the phone's Wi-Fi itself went off, Android turns Wireless debugging off with it: turn it on again on the phone. The Performance page's log carries
on once the phone is back.

**Use over Wi-Fi…** (Phones page) takes a phone connected by its cable onto Wi-Fi with no pairing, on Android 10 as well: DroidPlay restarts adb on the phone in
network mode (`adb tcpip 5555`) and connects to it at its Wi-Fi address, and then the cable can come out. That connection is **not encrypted** (the phone still
accepts only computers it has allowed) and lasts until the phone restarts; on Android 11 and later, Pair over Wi-Fi gives an encrypted one.

### From source

On Linux without the AppImage, on a Mac, or on Windows without Setup, `uv run scripts/nxnative_android.py` opens a window that drives the packing script and adb for you ([uv](https://docs.astral.sh/uv/) brings Python and the
modules the script names at its top; `python scripts/nxnative_android.py` works too, with them installed).

To get it on Windows, install [uv](https://docs.astral.sh/uv/) (`powershell -ExecutionPolicy ByPass -c "irm https://astral.sh/uv/install.ps1 | iex"`), get
DroidPlay Core (`git clone https://github.com/enjoyerofslop/droidplay-core`) and, in its folder, `uv run scripts\nxnative_android.py`.

The command line's requirements are in [What the command line needs](#what-the-command-line-needs); the app's **Settings** page checks them.

## DroidPlay for Linux (preview)

DroidPlay for Linux is the app of [DroidPlay for Windows](#droidplay-for-windows) packed as one AppImage, `DroidPlay-<version>-x86_64.AppImage` (about
130 MB), built the same way: its own Python with the app's modules and Qt, and DroidPlay Core. It's a preview for testers: it isn't on the release page yet.

- **Getting it:** the **DroidPlay-AppImage** artifact of the `linux-package` workflow's latest run (the AppImage and its `.sha256`, kept for 7 days), or
  build it from a checkout with `dm/android/linux/stage.sh --out <folder>` (x86_64 Linux with uv, git, curl and binutils' readelf).
- **What it runs on:** x86_64 Linux with glibc 2.34 or later: Ubuntu 22.04, Debian 12, Fedora 35, RHEL 9, Linux Mint 21, SteamOS 3 or newer, and current
  Arch, openSUSE Tumbleweed and their relatives. There is no arm64 build, since Google publishes the Android SDK for Linux on x86_64 alone. It draws its
  window with X11 or Wayland, whichever the desktop runs (`QT_QPA_PLATFORM=xcb` forces X11), and needs the libraries every desktop has: OpenGL/EGL,
  X11 and Wayland's client libraries, fontconfig, FreeType, GLib (on openSUSE also `libgthread-2_0-0`) and D-Bus. Qt's X11 helpers that a fresh install may lack (`libxcb-cursor` and the rest)
  come with it.
- **Starting it:** `chmod +x DroidPlay-<version>-x86_64.AppImage`, then open it. It mounts itself with the kernel's FUSE and `fusermount3`, and needs no
  libfuse2. Where FUSE isn't available (some containers, minimal systems), `./DroidPlay-<version>-x86_64.AppImage --appimage-extract-and-run` opens it, or
  set `APPIMAGE_EXTRACT_AND_RUN=1`.
- **The first start of a version** copies it into `~/.local/share/DroidPlay/<version>/` (`$XDG_DATA_HOME/DroidPlay` if set), about 720 MB, behind a
  "Preparing DroidPlay" window, and DroidPlay runs from there. That's what Windows' Setup does with its install folder: builds need a tree they can write
  to, at a path that stays the same from one start to the next. Later starts of that version are quick. A new version keeps the one before it and removes
  older ones, but never one that's still open.
- **The tools for building:** **Install tools** in Get started and **Install what's missing** in Settings work as on Windows: after you accept the
  Android SDK License, DroidPlay downloads Microsoft's JDK 21 and .NET SDK 10 and Google's Android SDK (command-line tools, platform-tools, platform 35,
  build-tools 36.0.0, the NDK 28.2 and CMake 3.22.1), about 1.5 GB, into `~/.local/share/nxnative/android/tools` (`jdk`, `dotnet`, `android-sdk`; about
  4 GB on disk). Everything goes into your own folders: no `sudo`, no package manager, nothing in the system. The Android SDK isn't inside the AppImage:
  Google's licence doesn't allow passing it on, so it always comes from Google. An SDK you have already is used and completed instead (`ANDROID_HOME`,
  `ANDROID_SDK_ROOT`, or Android Studio's `~/Android/Sdk`), and so are your distribution's adb, JDK or .NET SDK 10 when they're on your PATH (Bazzite and
  Fedora ship adb: DroidPlay uses it). Where Linux has no ICU library (minimal installs), .NET runs without it. The build's fonts need nothing: DejaVu Sans
  comes with the AppImage. On Linux on arm64 there's nothing to install: Google publishes the Android SDK for x86_64 Linux alone.
- **Your phone over USB** ([Phones on Linux](#phones-on-linux)): Fedora, Arch, Bazzite and other distributions with systemd 258 or later let the person
  at the screen use a phone's USB debugging with nothing to install. Elsewhere adb lists the phone as `no permissions`: DroidPlay keeps it in the phone
  list as **USB access needed** and **How to connect a phone** › **If it doesn't work** says what to do. Over Wi-Fi it works as on Windows
  ([Over Wi-Fi](#over-wi-fi)); if **Pair over Wi-Fi** doesn't list the phone, a firewall may be blocking mDNS: type the address shown with the code.
- **In the applications menu:** an AppImage has no menu entry of its own. **Settings** › **Add to the applications menu** writes one,
  `~/.local/share/applications/io.github.enjoyerofslop.droidplay.desktop`, and its icon in `~/.local/share/icons/hicolor/256x256/apps`, starting the AppImage
  file you opened (not the prepared copy, which a new version replaces). **Remove** takes both away. Moved or renamed the AppImage? Settings says the entry
  starts another copy, and **Start this copy** points it at this one. AppImageLauncher and Gear Lever make their own entries instead.
- **Where things go:** your games' builds beside their NSP files (`<title ID>_build`), as on Windows; the game hosts, the APK and the build journals in
  `~/.local/share/nxnative/android` (`$XDG_DATA_HOME/nxnative/android`), shared with the command line; settings in `~/.config/nxnative/android.conf`;
  the signing key in `~/.android/nxnative-debug.jks`.
- **Updating:** DroidPlay says when a newer release is out, as on Windows, and **Update** (or **Update to &lt;version&gt;** under Updates in Settings)
  downloads the release's AppImage into the folder of the one you opened, checks it against the SHA-256 GitHub lists for it, renames it into place, then
  closes and opens the new one, whose first start prepares the new version and keeps the previous one. Not during a build.
  - The file keeps saying its version: `DroidPlay-0.9.0-x86_64.AppImage` becomes `DroidPlay-0.9.1-x86_64.AppImage`, and the menu entry DroidPlay added
    follows it. A name of your own (`DroidPlay.AppImage`, or one an AppImage manager gave it) stays and the file is replaced in place, so a link or a
    launcher that starts it still does: rename the AppImage once if you start it from a link.
  - Before writing anything, DroidPlay checks that it can write to the AppImage's folder and that there's room for the new AppImage and its first start
    (about 1 GB). A download that is cut short or doesn't match the release is deleted. Either way the old AppImage stays as it was.
  - **Download** opens the release page instead, and Settings says why, where DroidPlay can't write to the AppImage's folder, for a test build from CI
    (its version isn't a release's) and for DroidPlay run from source; also while a release has no AppImage.
- **Uninstalling:** **Remove** the menu entry in Settings if you added one, then delete the AppImage and `~/.local/share/DroidPlay`; the tools Install
  tools brought are in `~/.local/share/nxnative/android/tools`, and the folders above keep your builds, settings and signing key until you delete them too.
- **Licences:** each part's licence and a list of what's inside are in `~/.local/share/DroidPlay/<version>/licenses` (`NOTICES.md`). Qt and Qt for
  Python are LGPL: their libraries are in `runtime/lib/python3.14/site-packages/PySide6/Qt/lib` there, and can be replaced. DroidPlay for Windows carries the same notices in
  `%LOCALAPPDATA%\Programs\DroidPlay\licenses`.

### Phones on Linux

Linux lets a program open a phone's USB debugging interface only where a udev rule gives it to the user, and an AppImage can't install one (or run
`sudo`): this is the one step you do yourself, once, and only where your distribution doesn't do it.

1. **Recent distributions need nothing.** systemd 258 and later mark Android's debugging interfaces (ADB and fastboot) for the person at the screen
   (`70-uaccess.rules`): Fedora, Arch, Bazzite and others with it. A phone that shows **Tap Allow on your phone** in DroidPlay's phone list is past this step.
2. **Elsewhere,** DroidPlay's phone list shows **Phone found: USB access needed** and Get started's phone step says so; adb itself says `no permissions`.
   Install your distribution's Android rules, then unplug the phone and plug it back in:
   - Ubuntu, Debian, Linux Mint: `sudo apt install android-sdk-platform-tools-common`
   - Arch, Manjaro: `sudo pacman -S android-udev`
3. **No package, or it still says `no permissions`:** add the rule yourself, in `/etc/udev/rules.d/70-android-adb.rules`:
   ```
   SUBSYSTEM=="usb", ENV{ID_USB_INTERFACES}=="*:ff4201:*", TAG+="uaccess"
   ```
   then `sudo udevadm control --reload` and `sudo udevadm trigger`, and plug the phone in again. It gives the logged-in user every phone's ADB interface
   (class ff, subclass 42, protocol 01), whatever its maker. If adb still says it, `adb kill-server` (DroidPlay starts adb again).
4. **Older guides** give phones to the `plugdev` group (`GROUP="plugdev"`): that works only once you're in it (`sudo usermod -aG plugdev $USER`, then log
   out and back in), and systemd 258 and later ignore such a rule when the group isn't a system group. The `uaccess` rule above needs no group.

Other things to check, as on any computer: a cable that carries data, the phone unlocked, **File transfer** picked in its USB notification on phones that
offer debugging only then, and **Allow** tapped on "Allow USB debugging?".

## DroidPlay on a Mac

[DroidPlay](https://github.com/enjoyerofslop/droidplay) is a Mac app that builds and plays Switch games on a Mac, an iPhone and an iPad, and builds
Android games with DroidPlay Core. For Android, add your games folder, then **Install on Android** (one launcher app holding every game, over adb)
or **Build Game App** (an APK, or an XAPK for an installer app). Its README has the requirements and the setup steps.

## The Android app

- **Launcher.** Opening **DroidPlay** (its package is still `org.nxnative.android`) shows the games on the phone with their icon, name and version (a game whose
  host the installed app lacks is marked as needing an app update). Picking a game opens its **settings screen**, grouped into Game and Controls: console mode (handheld is lighter), game language, game timing, render
  scale where the game supports it, the frame-rate counter, rumble and its strength, and the touch controls' size and inset ([settings](settings.md#touch-control-size)). **Play** starts the game. Lines like `DM_RENDER_SCALE=0.8` in
  `Android/data/org.nxnative.android/files/env.txt` win over the screen (they show greyed out).
- **The first start** of a game builds its pipelines for the phone's GPU, and stays on the loading screen meanwhile: 80 seconds for Brilliant Diamond on a
  Galaxy S25 Edge, 39 on a OnePlus Pad 3. Desktop DroidPlay says so the first time **Play** is pressed after an install, and the app says so when the game has no
  pipelines saved yet. Later starts take a few seconds.
- **In the game.** Touch controls are drawn over the picture in three layouts (2D, 3D and Tap, switched with the round button at the top); touches no control takes go
  to the game's touch screen. Their size, how far in from the sides they sit and how high from the bottom are the settings screen's choices; a game app, which has none, takes `DM_PAD_SCALE`,
  `DM_PAD_INSET` and `DM_PAD_RAISE` from its `env.txt`. A physical controller works as player 1 and hides the touch controls (its A/B/X/Y by position, or by label for a controller with Nintendo's layout: [settings](settings.md#controller-face-buttons)); rumble goes to the controller or the phone, and shaking the phone
  shakes the controller. The **≡** button at the top (left of −; with a controller connected it hides, and a tap on the screen shows it for a few seconds), or a controller's Mode or Menu key where Android passes it on, opens the pause menu: the game is held
  under it (its clock, sound and vibration stop, and no input reaches it) until **Resume**, ≡ again, B or the phone's Back. Its rows change the touch layout and turn the frame-rate
  counter on or off for this run (the settings screen's choice is the one that is kept), **Export or import the save…** opens the game's screen in the launcher,
  where the Save rows are (they ask before closing the game: a save is not swapped under a running game), and **Back to the launcher** goes there while the game
  keeps running behind it: **Play** on the same game resumes it, and picking another game asks before closing the running one. A game app has no launcher: it has
  no save row, and its last row is **Home screen**.
- **Saves** stay in the app's internal storage (`saves/<title ID>`), so updating the app keeps them; each game's screen exports, imports and puts back its save
  ([Saves](#saves)), and the desktop app backs them all up to the computer. The log is
  `Android/data/org.nxnative.android/files/log.txt` (also `adb logcat -s DroidPlay`). The start before's is kept as `log.prev.txt`, so a game the system closed
  for memory and that opened again still leaves its last log.
- **Removing a game:** **Remove game**, at the bottom of the game's screen, deletes the game from the phone after saying how much space that frees. It deletes the
  game's folder (`files/games/<title ID>`: code, shaders, game data), its pipeline cache, and its game data on the SD card when it was installed there
  ([the game data on an SD card](#the-game-data-on-an-sd-card)). Its save stays, and so do its settings, so installing the game again picks up where you left off.
  **Export save first** in the prompt makes a copy elsewhere. A running game is closed first, after asking. A game whose host the installed app lacks offers
  **Remove…** when you tap it. It can't be undone: to play the game again, install it again from your computer. A game app is removed by uninstalling it ([Game apps](#game-apps)).

## Saves

At the bottom of a game's screen in the app, under **Save**:

- **Export save** writes the game's save as a zip wherever you pick: Downloads, a cloud drive, an SD card. Inside, `saves/<title ID>/` is the save as the game
  sees it (with `saves/<title ID>_device/` beside it for a game that also keeps a device save). For an emulator, copy the files inside that folder into its save folder for the game.
- **Import save** takes a save from a zip or a folder you pick and puts it in place of the game's save. It recognises:
  - an export or backup from DroidPlay;
  - a yuzu-family emulator's save folder (yuzu, Citron, Eden, Sudachi…: `nand/user/save/0000000000000000/<user>/<title ID>/`) and Ryujinx's (`bis/user/save/<number>/`);
    pick the game's folder, or one higher up: when it holds several users' saves, you pick one;
  - JKSV, EdiZon and Checkpoint backups;
  - any folder that is the save itself, such as UltraTears' `userdata/save/<user ID>/`, or a save site's zip.

  It shows what it found and asks first. It warns when the files say they're another game's, or share no file names with the save the game has on the phone.
  RAR and 7z archives, and zips that Windows packed with Deflate64 (it does for big ones), can't be opened on a phone: unpack them with a file manager app or on a
  computer, then import the folder.
- **Earlier saves** puts back a save kept before an import, or one from **Back up saves** on the games list.

From a computer, **DroidPlay for Windows** does the same over adb, for a phone that's connected:
- **Library › Manage › Import a save to the phone…** sends a save folder or archive for that game. Unlike the phone, the computer unpacks RAR and 7z archives too
  (with Windows' own `tar`); a Deflate64 zip still needs unpacking first (Explorer's **Extract all** reads it).
- **Manage › Restore its save from a backup…**, and **Phones › Restore a backup…** for every game in one, send saves back from what **Back up saves to this computer**
  copied (pick its folder, or one of the dated folders inside it; the newest backup of each game is taken).
- The command line's `--import-save` and `--restore-backup` do the same ([Packing from the command line](#packing-from-the-command-line)).

The save goes to `files/import/<title ID>/` on the phone, which adb can write; the app's own save folder it can't. DroidPlay on the phone then shows what it found and
asks, as for an import from the phone: at once when the app is open or opens, or after the game, when it's being played.

Before an import or a restore, the game's save is kept in `Android/data/org.nxnative.android/files/backups/<time>-<title ID>/`, which DroidPlay for Windows' **Back up
saves to this computer** copies too. The game must not be running; the app offers to close it, and progress it hasn't saved is lost. Uninstalling the app deletes
those backups along with the saves, so export the saves you care about first.

- **Play the game once before importing**, until it saves. The import then checks the new files against that save, and finds the save's folder by its file names
  when a zip holds it deeper down or inside a folder of its own. Without a save to compare with, it goes down through folders that hold nothing but one folder, and takes the first that holds more.
- **A save from a later update of the game** than the one on the phone may not load (the game may say it's damaged, or start afresh). Your previous save is kept.
- **A game with several save slots** keeps them all in its one save, so an import replaces every slot at once.

## Game apps

A game can also be built as an app of its own, with its name, icon and saves, opening straight into the game. It installs on any arm64 device with Vulkan
without the DroidPlay app or a connection to your computer: copy the file over and open it. Several install side by side, apart from the DroidPlay app.

- **DroidPlay for Windows:** **Manage › Build a game app…**, then APK or XAPK. No phone needs to be connected; the file goes in the game's folder. For an APK, **Keep the game data for an SD card** is `--romfs-beside`.
- **DroidPlay on a Mac:** **Build Game App**.
- **The command line:** `--game-apk`, or `--game-apk --xapk` ([Packing from the command line](#packing-from-the-command-line)).

A game whose data is more than an APK holds (2 GB) gets it beside the APK, as `<name>.romfs`: copy that file anywhere on the device or its SD card, and pick it
once when the game first starts. On the command line, `--romfs-beside` does this for a smaller game too, so the APK stays small and the data can live on the SD card
(DLC that fits stays in the APK). An XAPK carries the data as the app's OBB file instead, for an XAPK installer app. A game app is for your own devices only: it
carries the game, so never share it. How it is packed: [A game app](../dm/android/README.md#a-game-app).

To remove a game app, uninstall it as you would any app (long-press its icon, then **Uninstall**, or **Settings › Apps**). That deletes its save too: a game app keeps
its save in its own storage, and has no screen to export it. A `<name>.romfs` file you copied over stays where you put it, for you to delete.

## Another Vulkan driver (Turnip)

On a phone with an Adreno GPU, a game can run on a Vulkan driver you put on the phone instead of the system's, as in Switch emulators: usually a build of Mesa's
open-source Turnip driver, such as [Mr. Purple's](https://github.com/MrPurple666/purple-turnip/releases). On a Samsung phone with an Xclipse GPU, the same goes for
a build of Mesa's RADV for Xclipse, such as [radv-xclipse](https://github.com/JimVulkan/radv-xclipse/releases) (its Android package, `vulkan.radeon.so`). The app
carries what loads it ([libadrenotools](../dm/third_party/libadrenotools/README.md)), so trying one needs no new build:

1. Unzip the driver package (`turnip_mrpurple_T30-toasted.adpkg.zip`, say). It holds a `.so` (`vulkan.purple.so`) and a `meta.json` that names it.
2. Copy the `.so` into `Android/data/org.nxnative.android/files/`, beside `env.txt`: `adb push vulkan.purple.so /sdcard/Android/data/org.nxnative.android/files/`.
   A game app reads its own folder, `Android/data/org.nxnative.game.t<title ID>/files/`.
3. Add `DM_VK_DRIVER=vulkan.purple.so` to `env.txt` there (a path relative to that folder, or an absolute one), then start the game.

The app copies the driver into its internal storage at start, since Android loads no code from shared storage, and copies it again when the file changes. The log
names the driver that runs: `[vk] driver: … (DM_VK_DRIVER, through libadrenotools), not the system's: PurpleVK-public Adreno (TM) 740 …`. A driver that can't run
(a wrong path, a build for another GPU, one that loads but can't make the Vulkan device or the window's swapchain) gets one `[RED] [vk] DM_VK_DRIVER …` line saying why, and the game runs on
the system's driver. On a phone with neither an Adreno nor an Xclipse GPU (a Mali) the line is ignored, and the log says so. When a start with the driver crashes in the driver's
code, loses the GPU, or ends in its first 30 seconds (a hang you closed, say), the next start runs on the system's driver once, and the start screen and the log say why;
the start after that tries your driver again. A crash outside the driver's code is noted but doesn't count against it, and a hang after the first 30 seconds isn't
caught. The app keeps each driver's last start in `files/vk_driver_state/<driver file>.json` and each game's in
`files/games/<title ID>/vk_driver_result.txt`, which DroidPlay for Windows can read. Every start, on any driver, also writes
`files/games/<title ID>/vk_start.txt`: the driver and GPU it ran on, how many pipelines the driver refused (their draws are missing from the picture) with the first
shaders' keys, and a lost GPU. Remove the line from `env.txt`
(or start it with `#`) to go back. Each driver keeps its own pipeline cache, so the first start on a new driver compiles the game's pipelines again (it can take a
minute to the first frame, and it stutters until they are built), and the system driver's cache is still there afterwards.

Whether a driver helps depends on the game and the device. With Mr. Purple's T30, Super Mario Odyssey's opening runs at 60 fps on an Odin 2 (as on its own driver)
and at 40 fps on a Retroid Pocket 5 (38 on its own); Pokémon Brilliant Diamond's Eterna City runs at 58 fps on a Retroid Pocket 5 with
Turnip (15 on its own driver), and at 60 fps on an Odin 2, as on its own driver.

## Packing from the command line

### What the command line needs

Python 3.12 or newer with `lz4`, `capstone`, `fonttools` and `backports.zstd` (not needed on Python 3.14), plus `PySide6` for the desktop app:
`pip install lz4 capstone fonttools backports.zstd PySide6`. The .NET SDK 10, the Android SDK with an NDK, CMake 3.22 and build-tools (set `ANDROID_HOME` or pass
`--sdk`), `adb`, and a JDK (for `keytool` and `javac`). The desktop app's **Settings** page checks all of this and says how to get what is missing; on Windows and x86_64 Linux, **Install what's missing** (or
`--setup --accept-android-licenses`) installs it for you, into your user folders.
On Windows, use Python 3.14 and set `PYTHONUTF8=1` for the command line (the desktop app sets it for what it runs): the scripts read and write text as UTF-8,
which Python on Windows does by default only from 3.15, and lz4 has no Windows build for 3.15 yet. On Linux, an Unreal game whose pak index is encrypted also
needs OpenSSL's libcrypto (`libcrypto.so.3`, which distributions install with OpenSSL); the Mac and Windows have their own AES.

```
python scripts/build_android_from_nsp.py "<folder with your NSP / XCI files>" -k "<your prod.keys>"    build, install, copy the game
                         (a Switch 2 game: the folder with its .nup or dump, or the .nup itself; no keys needed)
    --game-apk [--xapk]  the game as an app of its own (<name>.apk, plus <name>.romfs, with its DLC after the RomFS, for a game over 2 GB, or one .xapk); no device needed
    --romfs-beside       with --game-apk: <name>.romfs beside the APK whatever the game's size, so the APK stays small
    -p v0                pack the base game instead of the newest version in the folder (a version the profile does not register is packed from a derived profile)
    -l es-419            the default game language (a code or a name)
    --serial <device>    the adb device, when more than one is connected (over Wi-Fi: its ip:port, or the adb-…._adb-tls-connect._tcp name adb found it by)
    --no-push            build the APK only (a game without a profile is first built this way)
    --push-only          install the app and copy a game built before, without building
    --romfs-dir sd       put the game's data (its RomFS image) on the phone's SD card, not its own storage; or give a folder on the phone
    --no-romfs           with --romfs-dir: copy no data; you copy RomFS.img there yourself
python scripts/build_android_from_nsp.py --apk-only    pack the app with every game built on this computer and install it
python scripts/build_android_from_nsp.py --import-save "<save folder or archive>" --title <title ID>    send a save to the phone (Saves)
python scripts/build_android_from_nsp.py --restore-backup "<backup folder>" [--title <title ID>]       send a game's save, or every game's, from a backup
python scripts/build_android_from_nsp.py --check       what the build needs on this computer, as JSON
```

The first build of a game takes a while (the shader translation and, on a hard disk, the RomFS image); later builds reuse what did not change.

### The game data on an SD card

A game's data, its RomFS image (gigabytes), goes in the app's folder on the phone's own storage, `files/games/<title ID>/RomFS.img`. With `--romfs-dir sd`, it goes
in the app's own folder on the SD card instead: `/storage/<card ID>/Android/data/org.nxnative.android/files/romfs/<title ID>/RomFS.img`. The app reads that folder
with no storage permission. The rest of the game (code, shaders) stays on the phone's storage.

- **How the game finds it:** the game's `defaults.txt` gets `DM_ROMFS=<that path>`, and a copy left in the game's folder by an earlier install is deleted. An
  install with `--romfs-dir` again copies only a changed image, and goes on from where a copy cut short stopped, as before.
- **Another folder:** `--romfs-dir <folder>` takes any folder on the phone. On Android 11 and later the app reads only its own folders (`Android/data/org.nxnative.android`
  on the phone or the card), so the script warns about a folder outside them.
- **Copying it yourself:** `--romfs-dir sd --no-romfs` copies no data, sets the path, and prints it. Copy the extraction's image there, for example with
  `adb push "<NSP folder>/<title ID>_build/extract/<version>/RomFS.img" /storage/<card ID>/Android/data/org.nxnative.android/files/romfs/<title ID>/RomFS.img`, or with
  a card reader.
- **In DroidPlay for Windows:** a game's **Options** has **Game data**, Phone storage (the default) or SD card, saved per game. SD card is offered only when the picked
  phone has exactly one card (the script's own reading of `sm list-volumes`, else `/storage`); an install with it chosen passes `--romfs-dir sd`, and **Build a game app**
  passes `--romfs-beside` for an APK. If the card is out when you install, DroidPlay says so and doesn't start. Going back to Phone storage copies the data back, as above.
- **When the image isn't there** (the card taken out, or the image not copied yet), the game doesn't start, and the log says so: `[RED] the game's RomFS … is missing`.
- **Going back:** an install without `--romfs-dir` copies the image to the phone's storage again and leaves the card's copy, which you delete yourself. So does an
  install from DroidPlay on a Mac, which doesn't pass the option yet. **Remove game** on the phone deletes the card's copy with the rest of the game; removing
  the game from the computer (**Manage › Remove from the phone…**) leaves it, for you to delete. Uninstalling the app deletes it, with the app's other folders.

## Free space

A build keeps its files in a `<title ID>_build` folder beside the game's files: the extracted game (its RomFS image is as large as the game), the game's shaders as
one package (`shader/pkg`), and the shaders translated for Vulkan (`spv`). While the shaders translate, the translator's own copy of them sits in `<title ID>_build/tmp`
too, and goes when the translation ends, however it ends. So everything is on the drive with the game's folder: moving that folder to a bigger drive moves all of it.

The translated shaders take about 12 to 14 times the package's size, and the translation needs about 20 times it for a while:

| Game | Shaders | Package | Translated | Needed while translating |
|---|---|---|---|---|
| Super Mario Odyssey | 27,984 | 0.16 GB | 1.9 GB | about 3 GB |
| The Legend of Zelda: Ocarina of Time | 42,848 | 0.54 GB | 6.5 GB | about 12 GB |
| Final Fantasy Resonance | 73,496 | 0.68 GB | 9.4 GB | about 17 GB |
| [Pokémon Pokopia](../dm/games/0400A3A01EE4C000/README.md) | 131,426 | 4.4 GB | about 50 to 60 GB (estimated) | 90 GB or more |
| [Hyrule Warriors: Age of Imprisonment](../dm/games/04003F40232DE000/README.md) | 101,860 | not measured | likely 40 GB or more | likely 70 GB or more, besides its 46 GB RomFS |

Those are on top of the extracted game and the package. Yoshi and the Mysterious Book has over 100,000 shaders too (133,155), not measured.

While they translate, the Builds page counts the shaders done out of those left to translate (`progress: <done>/<total>` in `spv_build.log`, every 10 seconds). A game with over 100,000 shaders can take hours on a home PC. A translation stopped part-way keeps nothing of that run (its outputs are in `tmp` until it ends), so letting it finish is quickest.

- **The build checks first.** Before it translates, it works out what the shaders left to translate need, and stops when the drive can't hold it:
  `[RED] spv_build: not enough free space to translate … shaders`, with what the drive has and what it needs. Nothing has been translated then, and what the build
  did before (the extraction, the shader split) is kept.
- **If the disk fills anyway,** the build stops with `This computer ran out of disk space`, naming the drive. Free some space and build again: the shaders translated
  so far are kept, and only the rest is translated. A game's `<title ID>_build` folder can be deleted once you don't build that game any more.
- **Builds before DroidPlay Core 0.8.3** kept the translator's copy in the system's temp folder instead (`%TEMP%` on Windows, on C: whatever drive the game was on), and a
  translation that failed left it there: folders named `spv_` and some letters, tens of GB each for a big game. Nothing uses them, so delete them: type `%TEMP%` into File
  Explorer's address bar to get there.
- **The phone needs room too:** the translated shaders and the RomFS image. The install checks the phone before each copy, and a copy that doesn't fit stops before it
  starts.

## Games that compile their shaders while they run

Some games carry their shaders as GLSL source and compile them with Nintendo's GLSL compiler while they run. The Binding of Isaac compiles all of them that way; Crash
Bandicoot N. Sane Trilogy compiles some. No game file holds those shaders, so the first build doesn't have them: whatever they draw is black, and a game that compiles all
of them has no shaders at all (`[RED] the game is packed without shaders`). It runs but draws nothing.

The build says so before its long steps, once it has read the game's files (its `main` holds GLSL sources and calls `glslcCompile`):
`[RED] <game> compiles some of its shaders while it runs, so its first install may draw some things black. Play it to its first scene, then bring them in with --update-shaders`.
DroidPlay for Windows shows the same notice on the Builds page. On the game's page it then points at **Manage › Update shaders from the phone**, which is in bold in the
Manage menu. A game app can't take them in later, so build it after the steps below. The app captures them on the phone by itself:

1. Play the game for a while: each shader is saved the first time the game compiles it (in the game's folder on the phone, `glslc_capture/`).
2. On the computer, the game's card in the desktop app says how many new shaders the phone has; **Manage › Update shaders from the phone** copies them, translates
   the new ones and puts them on the phone (no app build: about a minute). From the command line: `build_android_from_nsp.py "<NSP folder>" -k <prod.keys> --update-shaders`.
   Every build and **Install on phone** also picks up the new ones first.
3. Open the game again.

A scene reached later (or a DLC) can compile more shaders: repeat until the log shows no `no translated shader`. This is done for games packed from a provisional
profile; a registered profile's shader tables name the captures they were made from. DLC in the NSP folder is extracted and copied to the phone with the game.

**Games only a Mac can build so far.** Six registered profiles' tables name shaders their games compile at boot: DRAGON QUEST BUILDERS 2, Fire Emblem: Three Houses,
Portal, Portal 2, Hyrule Warriors: Definitive Edition and Catherine: Full Body. Only a build on a Mac captures them (it boots the game in the Mac host), so on Windows and
Linux these games can't be built yet ([droidplay-core#441](https://github.com/enjoyerofslop/droidplay-core/issues/441)). The build stops at once, before it extracts
the game: `<game> compiles <N> of its shaders while it runs, with its own shader compiler (GLSLC), and game profile <id>'s shader tables need them. Only a build on a
Mac can capture them so far`. DroidPlay for Windows shows such a game as **Builds on a Mac only for now** and leaves its Play button off. On a Mac, build the game for
the Mac first: DroidPlay for Mac, or `build_ipa_from_nsp.py` on the same folder. The Android build then takes the captures from that build's
`<title ID>_build/extract/<version>/glslc_capture` ([why](reference/packing.md#an-android-build-of-a-game-that-compiles-shaders-while-it-runs-stops-at-the-split-off-a-mac)).

A game packed from a registered profile has its shaders in its tables, so a shader split that fails there stops the build rather than pack the game without them ([why](reference/packing.md#an-android-build-packs-a-registered-game-without-shaders-after-a-split-error-and-succeeds)). A shader translation that stops ends the build for every game, rather than pack it with part of its shaders ([why](reference/packing.md#an-android-build-packs-the-shaders-translated-so-far-after-the-translation-stops)): the shaders translated by then are kept, and the next build translates the rest. So does one that translates none of the shaders it has to (`[RED] spv_build: no shader was translated`), as happens when a file is missing from the translator's folder, `dm/gfx/tools/dmsh_xlate_bin` ([why](reference/packing.md#an-android-build-packs-a-game-without-shaders-when-the-translator-makes-nothing)).

## Seeing what a game draws

When a game shows a black or wrong picture, these files in `Android/data/org.nxnative.android/files/` ask the running game for diagnostics (each is checked every
30 frames and removed when taken; `adb shell touch <path>` creates one):

- `shot_now`: the next presented frame as a PNG in `dump/` (`DM_NVN_SHOT=<frame>[,<frame>...]:<path>` in `env.txt` does the same for given frames).
- `dump_now`: every render target of one frame, after each pass, as `.raw` and `.png` in `dump/` (a 3D texture's slices stacked, up to 1,024 images), with each draw's shaders, bound textures, constant buffers (their first values) and blend state in the log.
- `threads_now`: every game thread's state, what it waits on and its backtrace, in the log (for a game that stops moving on).
