# kingpin-recomp

A static recompilation of **Kingpin: Life of Crime** (Xatrix, 1999) for **macOS, iOS/iPadOS
and Android** on arm64.

The game's original Windows x86 executables are translated to C++ by a recompiler
(`tools/x86recomp.py`) and run on a Win32 compatibility layer written for this project. No
third-party engine or source port is used: what runs is the original game code.

> **No game files are included.** You need your own copy of Kingpin: Life of Crime (the CD,
> as a BIN/CUE rip). The recompiled code is generated on your machine from your copy and must
> not be redistributed. This project is not affiliated with or endorsed by Interplay, Xatrix
> or the rights holders.

## Features

- Native resolution, widescreen (Hor+ field of view, HUD kept at 4:3), anisotropic filtering,
  4x MSAA, ambient occlusion, bloom, sharpening and a brightness control
- Up to 120 fps with display-synchronised frame pacing
- Game controllers (Xbox, PlayStation, Switch Pro, MFi; Android gamepads)
- Touch controls on phones and tablets
- The intro movie (the original MVE player, recompiled) and CD music from the disc's audio tracks
- Optional AI-upscaled textures, generated locally from your game files

## Requirements

- A Kingpin: Life of Crime CD rip as BIN/CUE (data track + audio tracks), e.g.
  `Kingpin - Life of Crime (USA).cue` with its `.bin` files
- macOS on Apple silicon, Xcode command line tools, CMake, Ninja, Python 3,
  [unshield](https://github.com/twogood/unshield) (`brew install cmake ninja unshield`)
- iOS: Xcode and an Apple developer account (a free one works)
- Android: the Android SDK with NDK 27+, build-tools and platform 34+, Java 17

## Setup

```sh
# 1. Python tools
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt

# 2. Install the game from your disc: creates track1.iso, cd/ and game/ (all git-ignored)
tools/install_game.sh "/path/to/Kingpin - Life of Crime (USA).cue"

# 3. Generate the recompiled code from your copy (gen/ and gen_intro/, git-ignored)
.venv/bin/python tools/x86recomp.py gen game/kingpin.exe game/ref_gl.dll game/main/gamex86.dll \
    --seeds tools/seeds.txt --overrides tools/overrides.txt
.venv/bin/python tools/x86recomp.py gen_intro game/kpintro.exe --smc tools/kpintro_smc.txt
```

## macOS

```sh
cmake -S . -B build -G Ninja && ninja -C build
./run.sh                     # or ./run.sh +map sr1
```

Cmd+Enter toggles fullscreen, Cmd+= / Cmd+- change the brightness, Cmd+P toggles the
post-processing effects. `KP_WINDOWED=1` starts in a window, `KP_NO_INTRO=1` skips the intro.

## iOS / iPadOS

```sh
ios/build.sh sim       # simulator (slow: the simulator renders OpenGL ES on the CPU)
ios/build.sh device    # a signed device build; see the provisioning steps at the top of ios/build.sh
xcrun devicectl device install app --device <device id> build-ios-device/out/Kingpin.app
```

Then copy `kingpin.exe`, `ref_gl.dll`, `kpintro.exe`, `main/` (from `game/`), `track1.iso`
and the `cd/` folder into the app's Documents folder (Files app, or `xcrun devicectl device
copy to --domain-type appDataContainer --domain-identifier <bundle id>`).

## Android

```sh
android/build.sh                                         # build-android/Kingpin.apk (no Gradle)
adb install -r build-android/Kingpin.apk
android/push_data.sh game track1.iso "/path/to/Kingpin - Life of Crime (USA).cue"
```

The bundle/package id is `com.blounty.kingpin`; change it in `ios/`, `android/` and
`android/java/...` if you publish your own builds.

## Controls

**Controller:** left stick move, right stick look, A jump, B crouch (or close a note /
notebook / inventory), X use/talk, Y reload, LB/RB previous/next weapon, LT run, RT fire, L3
flashlight, R3 use item, Back/View holster, D-pad up/down conversation replies, left/right
select item, Start menu. The layout is in `extras/gamepad.cfg`.

**Touch:** move stick on the left, swipe on the right to look, on-screen buttons for fire,
jump, crouch, use, reload, weapons and the menu; menus get a D-pad with OK/BACK.

## Optional: HD textures

```sh
tools/make_hd_textures.sh          # all textures (under an hour on an Apple M-series GPU)
```

This upscales your game's textures 4x with Real-ESRGAN into `game/main/hd/`. It installs
PyTorch into `.venv`, downloads the
[RealESRGAN_x4plus](https://github.com/xinntao/Real-ESRGAN) weights (BSD-3-Clause, checked by
SHA-256), and runs `tools/upscale_textures.py` on Apple silicon (MPS), NVIDIA (CUDA) or the CPU.
The game picks the HD textures up automatically; on iOS and Android copy them with `main/`.

The HD textures are derived from the game's art, so they are not distributed: everyone
generates them from their own copy. Keep them out of the repository and don't share them.

## How it works

See [docs/NOTES.md](docs/NOTES.md): the recompiler, the Win32 layer, the fixed-function OpenGL
emulation on GL 3.2 / GL ES 3, the platform layers, verification and debugging options.

## License

The code in this repository is MIT licensed (see `LICENSE`). Kingpin: Life of Crime, its
executables and data belong to their respective owners and are not part of this project.

### Android APK via GitHub Actions

The manual **Build Android APK** workflow builds on Ubuntu (no local Android SDK
needed). Use only a disc you own. The ZIP must contain exactly one `.cue` and all
of its `.bin` files, with their original names and relative paths preserved. A
single enclosing folder is fine. Installed-game ZIPs without BIN/CUE are not supported.

1. Upload the ZIP to Google Drive (allow downloads for anyone with the link), or
   preferably a **private Hugging Face dataset**. Do not publish copyrighted game files.
2. Paste the ZIP URL into **game_archive_url** in the **Run workflow** form.
   Alternatively, open **Settings → Secrets and variables → Actions** and add
   `GAME_ARCHIVE_URL` as a repository secret; leave the form field blank to use it.
   Form inputs are not secrets and may be visible in workflow run metadata, so
   use the secret for sensitive links. Supported URL formats:
   - Google Drive: `https://drive.google.com/file/d/FILE_ID/view`
   - Hugging Face: `https://huggingface.co/datasets/USER/DATASET/resolve/main/disc.zip`
     (use `resolve`, not `blob`). For a private dataset, also add the `HF_TOKEN`
     secret with read access to that dataset (never paste tokens into the URL field).
3. Open **Actions → Build Android APK → Run workflow** and select the branch
   containing the workflow. GitHub may require the workflow to be present on the
   default branch before showing the manual-run button.
4. When it succeeds, download the **Kingpin-Android-arm64** artifact and extract
   `Kingpin.apk`. Artifacts expire after 3 days. Drive download quotas can cause
   failures; use Hugging Face if that happens.

The APK targets arm64 devices running Android 11 or newer and includes touch
controls. CI now produces **two separate artifacts**:

- **Kingpin-Android-arm64** — contains `Kingpin.apk`, without embedded game data.
- **Kingpin-Game-Data** — contains the prepared game files directly (no nested ZIP).

Download both artifacts. Install the APK. Extract the game-data artifact into a
normal folder, e.g. `Download/KingpinData/`. In the launcher tap **Выбрать папку
игры**, select that folder (the one directly containing `kingpin.exe`, `main/`
and `track1.iso`), wait for copying, then tap **Играть**. No ZIP extraction runs
inside the app. Both artifacts expire after 3 days.

These artifacts contain copyrighted game material/code: keep them for personal
use, do not publish them as releases. No generated source code is uploaded.
Each clean CI run creates a new debug signing key, so upgrading an earlier APK
may require uninstalling it first (back up saves/data before uninstalling).

### Android launcher and folder import

The app now opens a launcher with **Choose game folder** and **Play** buttons
(localized in Russian). Select a folder using Android's system document picker;
no all-files storage permission is needed. Select the folder directly containing:

```text
kingpin.exe
ref_gl.dll
track1.iso
main/              # complete installed folder, including gamex86.dll and pak0.pak
kpintro.exe        # optional intro player
cd/                # optional CUE and audio BIN tracks for music
```

This is an **installed-data importer**, not a BIN/CUE installer or ZIP extractor.
CI prepares these files for you in **Kingpin-Game-Data**. Alternatively, prepare
the disc locally with `tools/install_game.sh` and assemble the files above in one
folder. Put it somewhere the Android picker can access (e.g. a subfolder of
Downloads).

Import runs off the UI thread, copies the entire `main/` and optional `cd/`, adds
the port/controller settings, validates required nonempty files, then publishes
an `imported-game/` directory inside the app's external files directory. Original
source files are not modified. Failed copies leave the current imported game
untouched; temporary copies need additional free space. Import again replaces
the previous imported game **including saves**, so back up saves first. Do not
re-import while a game session is running. Android may interrupt imports if you
close the app; reopen and retry. Legacy data copied directly with `push_data.sh`
is still used when no imported game exists. Uninstalling the app removes imported
data and saves.

For a local all-in-one build, run `KP_BUNDLE_DATA=1 android/build.sh` after disc
installation and recompilation. This creates a temporary `game-data.zip` under
`build-android/assets/` (git-ignored) and embeds it in the APK. Interrupted first-run
extraction can be retried by closing and reopening the app. Full APK assembly and
on-device extraction require testing with your own disc files.
