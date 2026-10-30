# Dead or Alive 4 — PC recomp

> **Playtest build.** This is the `playtest` branch of a fork of
> [parkerallan/doa4-recomp](https://github.com/parkerallan/doa4-recomp). It carries the launcher and
> the features of [pull request #2](https://github.com/parkerallan/doa4-recomp/pull/2) while that
> pull request is open, and its launcher builds this branch.
>
> **New to this? Start with the [Easy setup guide](#easy-setup-guide) just below.** It takes you
> from nothing to playing, one click at a time. The rest of this page is for people who want the
> technical details.

A native Windows build of **Dead or Alive 4** (Xbox 360), statically recompiled with the
[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk). The game's code is translated ahead of time
into C++ (`generated/`); the SDK supplies the Xbox 360 runtime (kernel, GPU, audio, input). This
repository contains **no game data**: you build it, then run it against files extracted from your
own copy of the game.

This port adds, on top of the original recomp: frame pacing fixes, an Esc menu with Video /
Controls / Photo / Extras / Game tabs, graphics options (anti-aliasing, anisotropic filtering,
render scale), photos (F12 and full-quality copies of in-game snapshots), unlockables and tweaks,
a diagnostics overlay (F6), a first-run setup that finds or extracts the game files, and a
one-click launcher. It builds against the ReXGlue SDK branch
[`doa4-recomp` of Dwreckgg/rexglue-sdk](https://github.com/Dwreckgg/rexglue-sdk/tree/doa4-recomp),
which carries the SDK changes the port needs (frame pacing, audio fixes, controller remap, a swap
counter) on top of rexglue-sdk `e8ce24f`, the SDK version the port's code was generated with.

## Easy setup guide

No programming knowledge needed. You click through a few windows and wait while your PC does the
work. Set aside about an hour the first time; most of it is waiting for downloads.

### What you need

- **A Windows 10 or Windows 11 PC (64-bit).** A gaming PC from roughly the last eight years is fine:
  the processor must support AVX2 (Intel from 2013 on, any AMD Ryzen) and the graphics card must
  support DirectX 12.
- **Your own copy of Dead or Alive 4 for Xbox 360, as a disc image** (a file ending in `.iso`) made
  from a disc you own. It must be the **USA / Europe "Rev 1"** version (see
  [The game version](#1-the-game-version)). This project does not include or provide the game.
- **About 20 GB of free disk space** (about 7 GB for the game files, the rest for the tools and the
  build).
- **An internet connection** for the first setup.
- **A controller** is recommended (an Xbox-style one works best). A keyboard also works: press
  **Esc** in the game, open **Controls** and tick **Use keyboard as controller**.

### Step 1: Download the launcher

1. At the top of this page, click the green **Code** button, then **Download ZIP**.
2. Open your **Downloads** folder. Right-click the ZIP file (`doa4-recomp-playtest.zip`), choose
   **Extract All...**, then click **Extract**. A folder with the same name opens.
3. Make a new, empty folder for the game, for example **`C:\Games\DOA4`**.
   - Use a short, simple location. Its name may only contain plain letters (A-Z), numbers and spaces.
     The game cannot start from a folder whose path has accented or non-English letters.
   - Do not use your Desktop, Documents or OneDrive folders. OneDrive would try to upload several GB
     of build files.
4. In the extracted folder, open the folder called **`launcher`**. Inside it you see a file called
   **`setup.bat`** and another folder also called **`launcher`**. Copy **both** of them into your new
   `C:\Games\DOA4` folder.

When you are done, `C:\Games\DOA4` contains exactly two things: `setup.bat` and a folder named
`launcher`. You can delete the downloaded ZIP and the extracted folder now.

### Step 2: Install

1. Double-click **`setup.bat`** in `C:\Games\DOA4`.
   - If a blue window says **"Windows protected your PC"**, click **More info**, then **Run anyway**.
     It appears because the file was downloaded from the internet.
2. The **DOA4 launcher** window opens. Click **Install**.
3. The launcher checks for three free tools the build needs (Git, LLVM and Microsoft's Visual Studio
   Build Tools) and lists the ones you are missing, with their size. Confirm, and it installs them.
   - The Visual Studio installer opens its own window and asks for administrator permission. Click
     **Yes**, then let it finish by itself. This is the longest wait.
4. The launcher then downloads the game's code (about 2 GB) and builds the game. You can follow each
   step in the window. Leave it running; do not close it.
5. When it is finished, a **Dead or Alive 4** shortcut appears on your desktop.

### Step 3: First start

1. Click **Play** in the launcher (or open the **Dead or Alive 4** shortcut on your desktop, then click
   **Play**).
2. The first time, the game asks for your game files. Choose **your disc image** (the `.iso` file).
   - It checks that the disc image is the right version.
   - It asks where to put the game files. The suggested place (a `DOA4` folder next to your disc
     image) is fine. It shows how much space is needed and how much you have.
   - It copies the game files out of the disc image (about 7 GB, a few minutes). You only do this
     once; afterwards the disc image is not needed.
3. The game starts. Enjoy!

### Playing

- **Esc** opens the menu: video and graphics options, controls, photos, extras and game settings.
- **F12** takes a photo. In the launcher, **Open photos** shows them.
- To get the newest version later, open the launcher and click **Update & rebuild**.
- **Open saves** and **Open logs** in the launcher open the folders with your saves and the log files.

### If something goes wrong

| What you see | What to do |
|---|---|
| "Windows protected your PC" | Click **More info**, then **Run anyway**. |
| The game says the disc image is the wrong version | You need the USA / Europe Rev 1 disc image. Other regions and versions do not work. |
| An error about the folder or its name | Move the game folder to a simple place such as `C:\Games\DOA4` (plain letters, numbers and spaces only) and run `setup.bat` from there. |
| "Not enough space" | Free up space (about 20 GB in total), then click **Install** again. |
| Anything else | In the launcher, click **Open logs**. Send the newest log file to the person helping you, or attach it to an issue on this page. |

### Taking part in an online test

If someone sent you a **netplay test package** (a ZIP with a file called `run_check.bat` in it), you do
**not** need any of the steps above. The package already contains the game program. All you need is
your own disc image; follow the `README.txt` inside the package.

## Quick start: the one-click launcher

The launcher installs what the build needs, downloads the sources, builds the game and makes a
desktop shortcut; afterwards it plays, updates and rebuilds. It is a small folder (`setup.bat` and a
`launcher` folder) made from this repository by `launcher\make_zip.ps1` (it writes
`DOA4-launcher.zip`, or `-Destination <folder>` writes the folder directly).

1. Put the `DOA4` folder somewhere whose path has only plain letters, digits and spaces (for
   example `C:\Games\DOA4`; the game cannot start from a path with accented or non-Latin letters).
   Everything the launcher downloads and builds stays inside it (about 4 GB).
2. Double-click `setup.bat` and press **Install**:
   - **Tools:** it checks for Git, LLVM 23.1.4 and Visual Studio 2026 Build Tools with the C++
     workload, lists any that are missing with their size, and installs them with winget after
     one confirmation (Visual Studio shows its own installer and asks for administrator rights).
   - **Code:** it downloads the game port and the ReXGlue SDK (about 2 GB the first time).
   - **Build:** it applies the Windows symlink fix from step 4 below and builds (a few minutes on
     a recent PC). Progress and every step's status show in the window.
   - **Shortcut:** a "Dead or Alive 4" shortcut on the desktop opens the launcher.
3. Press **Play**. On the first start the game asks for your disc image or extracted folder
   ([step 5](#5-the-game-files)).
4. Later: **Update & rebuild** fetches new commits and rebuilds ("nothing new" when there are none);
   **Change game files**, **Open photos**, **Open saves** and **Open logs** do what they say.

![The launcher before Install](docs/images/launcher.png)

- `setup.bat -DryRun` shows what Install would install and do, without doing it.
- Which sources it builds is set in `launcher\config.ps1`: this fork's `playtest` branch
  (`Dwreckgg/doa4-recomp`) with the SDK from `Dwreckgg/rexglue-sdk` (branch `doa4-recomp`) by
  default. To build another fork or branch, change `GameRepoUrl` / `GameRef` (and `SdkRepoUrl` /
  `SdkRef`) and install into a new `DOA4` folder.
- It never pushes, stashes or overwrites your changes: Update stops and lists any file you changed
  by hand in its checkouts (other than its own two fixes).

The rest of this README is the same process by hand.

## Contents

1. [The game version](#1-the-game-version)
2. [Tools](#2-tools)
3. [Folder layout](#3-folder-layout)
4. [Get the sources](#4-get-the-sources)
5. [The game files](#5-the-game-files)
6. [Build](#6-build)
7. [Run](#7-run)
8. [Regenerating the recompiled code](#8-regenerating-the-recompiled-code)
9. [Host tests](#9-host-tests)
10. [Troubleshooting](#10-troubleshooting)

## 1. The game version

The recompiled code matches one exact `default.xex`. Other regions or revisions will not work.

| | |
|---|---|
| Disc image used | `Dead or Alive 4 (USA, Europe) (En,Ja,Fr,De,Es,It,Zh,Ko) (Rev 1).iso` (Redump naming) |
| Title ID | `544307D1` |
| `default.xex` SHA-1 | `af8011945037b37c98fb8a78d5a4e844dd56c33f` |
| `default.xex` MD5 | `b2eb9c5df1f2060ae9c616945450289f` |
| Extracted size | about 6.8 GB |

Make the image from a disc you own. After extracting (step 5), check the hash:

```bat
certutil -hashfile xex\DOA4\default.xex SHA1
```

## 2. Tools

Versions are the ones this build was made with; nearby versions will probably work, but these are known
to.

| Tool | Version used | Where to get it |
|---|---|---|
| Windows 10/11, 64-bit, CPU with AVX2 (Intel Haswell / AMD Zen or newer) | Windows 11 | The build targets `x86-64-v3`. |
| Git | any recent | <https://git-scm.com> or `winget install Git.Git` |
| Visual Studio 2026 Build Tools (v18), workload **Desktop development with C++** | 18 | <https://visualstudio.microsoft.com/downloads/> — provides the Windows SDK, the MSVC libraries, **CMake 4.3.1** and **Ninja 1.13.2** (the build uses the bundled CMake and Ninja) |
| LLVM / Clang (the compiler; MSVC's compiler is not used) | 23.1.4 | `winget install LLVM.LLVM` (installs to `C:\Program Files\LLVM`) |
| Python 3 (for the ISO extractor and the symlink fix) | 3.14 | <https://www.python.org> or `winget install Python.Python.3.14` |

Optional: [extract-xiso](https://github.com/XboxDev/extract-xiso) as an alternative ISO extractor
(step 5).

## 3. Folder layout

Keep the three folders side by side. The code generator's manifest
(`dead_or_alive_4_manifest.toml`) looks for the game at `../xex/DOA4`, so this layout matters when
regenerating code (step 8); for building and running, any paths work.

```
doa4\
  doa4-recomp\     this repository
  rexglue-sdk\     the SDK, built from source together with the game
  xex\DOA4\        the extracted game files (default.xex, *.afs, movie\, SOUND\, dic\)
```

## 4. Get the sources

```bat
mkdir C:\doa4
cd /d C:\doa4
git clone --branch playtest https://github.com/Dwreckgg/doa4-recomp doa4-recomp
git clone --branch doa4-recomp https://github.com/Dwreckgg/rexglue-sdk rexglue-sdk
cd rexglue-sdk
git submodule update --init --recursive
cd ..
```

The SDK branch `doa4-recomp` is rexglue-sdk `e8ce24f`, the SDK commit that generated this port's
code (v0.8.x; the manifest names `0.8.1.4-dev.ge8ce24f`), plus the SDK changes this port calls; the
upstream SDK alone does not build it. CMake prints a harmless "no v* tag reachable from HEAD"
warning on a fresh clone (fetching the tags does not silence it).

### Fix the SDK's symlinks (Windows)

Two SDK submodules (`libmspack`, `o1heap`) contain git symlinks. Unless symlinks are enabled
(Windows Developer Mode plus `git config --global core.symlinks true` *before* cloning), git
checks them out as small text files holding a path, and the build fails on them (for example
errors in `thirdparty/libmspack/cabextract/mspack/lzxd.c`, whose content is just
`../../libmspack/mspack/lzxd.c`). Replace them with real copies:

```bat
py -3 doa4-recomp\tools\fix_git_symlinks.py rexglue-sdk
```

It reports `replaced 16 symlink stub(s)` the first time and nothing afterwards; it is safe to run
again. `git status` in the SDK then shows the two submodules as modified (` m`), which is expected.

## 5. The game files

The recomp reads the game's files from a folder, not from the disc image. Xbox 360 discs use the
XDVDFS file system, which ordinary ISO tools cannot open.

**Usually nothing to do here:** the first time `dead_or_alive_4.exe` starts without game files it
shows a setup screen (step 7). Choose your disc image and it checks the version, asks where to
extract (default: a `DOA4` folder beside the image; it shows the space needed and free) and
extracts with a progress bar; or choose a folder you extracted before. The location is saved, and
the Esc menu's **Game** tab can change it later.

![The setup screen: choose the disc image or the extracted folder](docs/images/setup-screen.png)

**By hand:** `tools/xdvdfs_extract.py` (a short Python script, no dependencies) does the same
extraction from a command line, and is what the first build used. It finds the game partition itself
(DOA4 is an XGD2 disc; its partition starts at byte `0xFD90000`).

```bat
cd /d C:\doa4
py -3 doa4-recomp\tools\xdvdfs_extract.py "Dead or Alive 4 (USA, Europe) (En,Ja,Fr,De,Es,It,Zh,Ko) (Rev 1).iso" xex\DOA4 --list
py -3 doa4-recomp\tools\xdvdfs_extract.py "Dead or Alive 4 (USA, Europe) (En,Ja,Fr,De,Es,It,Zh,Ko) (Rev 1).iso" xex\DOA4
```

The first command only lists the contents (a quick check that the image is readable); the second
extracts about 6.8 GB. Then check the `default.xex` hash (step 1). The ISO is not needed afterwards.

**Alternative:** [extract-xiso](https://github.com/XboxDev/extract-xiso) (download
`extract-xiso-Win64_Release.zip` from its releases) does the same job:
`extract-xiso -x -d xex\DOA4 "<image>.iso"`. It was not the tool used for this build, so check its
own help (`extract-xiso -h`) if that command line does not work.

## 6. Build

From a plain `cmd` window (not PowerShell), in `C:\doa4`:

```bat
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64
set "PATH=C:\Program Files\LLVM\bin;%PATH%"
cd doa4-recomp

rem Configure (once, or after deleting out\build):
cmake --preset win-amd64-release -DREXSDK_DIR=C:/doa4/rexglue-sdk "-DCMAKE_C_FLAGS=-march=x86-64-v3" "-DCMAKE_CXX_FLAGS=-march=x86-64-v3"

rem Build (every time):
cmake --build --preset win-amd64-release
```

- `VsDevCmd.bat` sets up the Windows SDK and puts Visual Studio's CMake and Ninja on `PATH`;
  LLVM goes first on `PATH` so `clang` / `clang++` are found (the presets name them).
- `REXSDK_DIR` builds the SDK from source alongside the game (no separate SDK install). Use forward
  slashes.
- The `-march=x86-64-v3` flags are required: the port's preset does not set a CPU level and the SDK
  uses SSSE3/AVX intrinsics, which fail to compile without it.
- The first build compiles the SDK and the large generated code and takes a while; later builds are
  incremental.

The result is in `out\build\win-amd64-release\`: `dead_or_alive_4.exe` with `rexruntime.dll` and
`TracyClient.dll` beside it.

## 7. Run

Start `out\build\win-amd64-release\dead_or_alive_4.exe`. On the first start (or when the saved
location has gone, e.g. an unplugged drive) the setup screen asks for your disc image or extracted
folder (step 5); after that the game starts straight away. The setup accepts only the supported
version (step 1) and explains why when it refuses one.

Optionally, a `run_doa4.bat` next to the exe can pass the folders on the command line. A
`--game_data_root` given this way overrides the saved location (and the Game tab's **Change…** is
then off); leave it out to use the setup's choice:

```bat
@echo off
rem GAME = the extracted folder holding default.xex. USERDATA = saves and the shader cache.
set GAME=C:\doa4\xex\DOA4
set USERDATA=C:\doa4\userdata
cd /d "%~dp0"
start "" dead_or_alive_4.exe --game_data_root="%GAME%" --user_data_root="%USERDATA%"
```

Saves and the shader cache go to `--user_data_root` if given, else to the folder in
`doa4_user_data` in `dead_or_alive_4.toml` (for example `doa4_user_data = "C:/doa4/userdata"`, with
forward slashes), else to `Documents\dead_or_alive_4`. Keep it outside OneDrive if you can (OneDrive
can lock files the game is writing). The Esc menu's Game tab shows the folder in use.

![The Esc menu's Game tab: the game files folder (with Change... and Open folder) and the saves folder](docs/images/game-tab.png)

- Settings live in `dead_or_alive_4.toml` next to the exe; the **Esc** menu edits them (tabs
  Video, Controls, Photo, Extras, Game).
- **F6** shows a diagnostics overlay.

### Photos

- **F12** saves a photo of the game's own frame at the render scale, without any overlay. The Esc
  menu's Photo tab picks the format (PNG at monitor size by default, or PNG / JPG at full render size)
  and the folder (default `Pictures\DOA4`; **Open folder** opens it).
- With "Also save a full-quality copy when you save a snapshot to the in-game Album" ticked, saving a
  snapshot in the game's own photo mode also writes that photo at full quality to the same folder
  (the game's Album keeps a small, compressed copy). Only snapshots you save to the Album are copied.

![A snapshot from the game's photo mode, saved as a full-quality copy (shown here reduced)](docs/images/photo-mode.jpg)
- Logs: `out\build\win-amd64-release\logs\dead_or_alive_4_NNN.log`.

## 8. Regenerating the recompiled code

`generated/` is checked in, so a normal build does not need this. Only after adding a function
address to `hints.toml` (for example after a log line
`Call to invalid or unregistered function at guest address 0x...`):

```bat
cmake --build --preset win-amd64-release --target dead_or_alive_4_codegen
cmake --preset win-amd64-release -DREXSDK_DIR=C:/doa4/rexglue-sdk "-DCMAKE_C_FLAGS=-march=x86-64-v3" "-DCMAKE_CXX_FLAGS=-march=x86-64-v3"
cmake --build --preset win-amd64-release
```

The generator reads `../xex/DOA4/default.xex` (the folder layout in step 3). Reconfiguring is needed
only if new `.cpp` files were generated. `commands.txt` has the upstream author's notes on this loop.

## 9. Host tests

The mod code (unlocks, photo, graphics options, …) has small tests that build with `clang-cl` without
CMake or the game:

```bat
tests\host\run_photo_test.bat
```

One `run_<name>_test.bat` per module in `tests\host\`; each prints `<name>: all checks passed`.

## 10. Troubleshooting

| Symptom | Cause and fix |
|---|---|
| Compile errors in `thirdparty/libmspack/...` or `thirdparty/o1heap/...` | Symlink stubs: run `tools/fix_git_symlinks.py` (step 4). |
| Errors about SSSE3 / AVX intrinsics (`_mm_...` functions) | Configure was run without the `-march=x86-64-v3` flags. Delete `out\build\win-amd64-release` and configure again. |
| `imgui.h` or `rex/version.h` not found | The two include directories after `rexglue_setup_target(dead_or_alive_4)` in `CMakeLists.txt` are missing (an older upstream checkout): see [Building from a fresh GitHub clone](#building-from-a-fresh-github-clone). |
| `clang` not found / CMake picks the wrong compiler | LLVM's `bin` folder is not first on `PATH` in that window. |
| CMake warning `no v* tag reachable from HEAD` | Harmless; the build is not affected. |
| `no XDVDFS volume found` from the extractor | The file is not an Xbox / Xbox 360 game image (or is damaged). |
| The game does not start | Check the newest log in `logs\` (lines tagged `[doa4][SETUP]` say which game folder was used and why). |
| The setup screen keeps coming back | The saved location could not be written to `dead_or_alive_4.toml` (the log says why), or the folder has moved. |
| "This path has characters that this build cannot use" | The game folder's path has characters outside the Windows code page (for example Japanese on an English Windows); the runtime cannot start from it. Use a folder whose path has plain letters and digits. |
| "This folder holds an unfinished extraction" | An earlier extraction was interrupted (the game was killed or the PC lost power). Delete that folder and extract again. |

## Building from a fresh GitHub clone

A fresh clone of this branch builds with steps 4 and 6 exactly as written: clone the port and the
SDK branch `doa4-recomp`, update the SDK's submodules, run the symlink fix, configure with
`REXSDK_DIR` and the `-march=x86-64-v3` flags, and build. Nothing has to be edited by hand.

Two things a fresh clone needs that are easy to miss:

- **The SDK branch, not upstream rexglue-sdk.** The port calls SDK additions (the swap counter,
  controller remap, raw pad state, input blocking for the menu) that upstream `e8ce24f` does not
  have, so it does not build against upstream (for example `rex/input/pad_remap.h` is not found).
- **The symlink fix** (step 4) on Windows without Developer Mode symlinks; without it the SDK's
  `libmspack` / `o1heap` sources fail to compile.

`CMakeLists.txt` on this branch already has the two include directories the Windows build needs
after `rexglue_setup_target(dead_or_alive_4)` (older upstream checkouts lack them; the launcher adds
them to such a checkout itself):

```cmake
if(TARGET imgui)
    target_include_directories(dead_or_alive_4 PRIVATE $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
if(REXSDK_DIR)
    target_include_directories(dead_or_alive_4 PRIVATE ${CMAKE_BINARY_DIR}/rexglue-sdk/include)
endif()
```

An upstream checkout from before this branch (`parkerallan/doa4-recomp` `1d38c03`) builds the same
way against rexglue-sdk `e8ce24f`, but has none of the features listed at the top (Esc menu, pacing
fixes, photos, setup screen, launcher).

## AI disclosure

Much of the code in this fork was written with the help of an AI coding assistant (a large language
model). That includes the one-click launcher, the first-run setup, the Esc menu and its options, the
frame pacing and audio changes, photo mode, the online-play work, and this README. The fork's
maintainer decided what to build, directed the work and tested the results in the game. The code
was also checked by automated tests and by separate review passes.

The original recompilation ([parkerallan/doa4-recomp](https://github.com/parkerallan/doa4-recomp))
and the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) are their authors' own work; this note
covers only what this fork adds. If you find a bug, please report it as an issue on this page.
