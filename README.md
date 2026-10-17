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
CI requires a persistent signing key in `ANDROID_KEYSTORE_BASE64` so subsequent
builds can be installed over existing ones without deleting app data. See below.

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

### Android performance profile, ETC2 and FPS

Android now defaults to a **60 FPS target** and requests a 60 Hz display mode.
Render buffers are capped to a 720-pixel short edge (never upscaled). MSAA and
AO/bloom/sharpen effects are disabled, anisotropy is reduced to 2x, and HD PNG
replacements are disabled by default. These reduce GPU load, not CPU emulation
cost. They do **not** guarantee 60 FPS on all phones or in every scene. Thermal
throttling, device drivers and CPU performance still matter. No device benchmark
has been performed for this profile.

An on-screen FPS label in the upper-right updates twice a second from successful
native buffer presents, not the UI refresh rate. It measures presentation rate,
not GPU timing or simulation speed.

CI encodes static TGA world/model/sprite/sky textures from PAKs and loose files
into ETC2 RGBA8/EAC mip chains under `main/etc2/`. These ship in the separate
**Kingpin-Game-Data** artifact: import the new data folder to use them. Android
GLES 3 guarantees ETC2 support. Lightmaps, UI, video and unsupported source formats
remain uncompressed. Missing/invalid ETC2 cache entries fall back to the original
upload. Originals remain in the PAKs, so this reduces texture GPU memory/bandwidth,
not the size of the game-data download. Compression is lossy and uses original
texture colours rather than the guest's gamma/intensity lookup; brightness may
differ from the uncompressed path. Disable with `KP_ETC2=0` if needed.

For local preparation: `pip install etcpak==0.9.15 pillow==13.0.0`, then
`python tools/make_etc2.py` after installing your disc. Optional codec tests also
use `texture2ddecoder`. No game textures or generated caches are committed.

Advanced overrides in the active game folder's `kp_env.txt` include
`KP_RENDER_SCALE=0.5`, `KP_MSAA=0`, `KP_POST=0`, `KP_ANISO=2`,
`KP_HD_TEXTURES=0`, `KP_ETC2=0` and `KP_HZ=60`. Explicit render scale overrides the
720p cap. Adjust `cl_maxfps` in the game's config for a different simulation cap.

### Copying Android diagnostics

The launcher has **Показать логи** (view/select text) and **Скопировать логи**
(copy to clipboard) buttons. After a crash, reopen the launcher and copy logs
**before launching the game again**, then paste them into a bug report. No ADB,
storage permissions or network upload is required.

App-private `files/logs/` holds bounded Java logs (including uncaught Java
exceptions) and flushed native runtime messages (current/previous files, up to
about 1 MiB each). The report includes recent tails, device/OS/app version and up
to three Android historical exit records. Android may supply an ANR text trace;
full native tombstones/stacks are not guaranteed or exposed as text here. Crashes
before native logging starts may only appear in Java or OS exit records. Logging
is best-effort, not a substitute for `adb logcat` when a complete stack is needed.

Review copied text before sharing: it may contain device details and file paths.
Logs survive app/process restarts, but not uninstalling or clearing app storage.

### Launcher language

Use the **Язык: Русский / Language: English** button near the top of the launcher
to select Russian or English. The choice is saved across app restarts and applied
immediately. The initial default follows the device language (Russian for `ru`,
English otherwise). Buttons, help, import progress/errors and log dialogs are
translated; technical diagnostic records remain in their original language.
The selector is disabled while copying/extracting game data to avoid interrupting
the import. This preference does not translate the game or change the Android
system folder picker's language.


### Android updates without uninstalling

APK updates must use the same package ID, signing key and a version code no lower
than the installed app. The build workflow now requires a persistent signing key
instead of generating a different one on every run.

One-time setup (possible entirely in the GitHub browser UI):

1. Run **Actions → Create Android signing key → Run workflow** once.
2. Download **PRIVATE-Android-Signing-Key**, unzip it, and open
   `ANDROID_KEYSTORE_BASE64.txt` locally. **This is a private signing key**, not
   game data: never publish it or paste it into chat. Anyone who can download
   repository Actions artifacts may be able to access it; prefer a private repo.
3. Add the entire file contents as the repository Actions secret
   **ANDROID_KEYSTORE_BASE64**. The connected agent cannot set repository secrets
   with its current GitHub permissions, so this step must be done by the owner.
4. Back up this key securely, then delete the signing-key workflow run/artifact
   after saving it. The artifact also expires after one day. Do not regenerate
   the key for later builds: use the same secret every time.
5. Run **Build Android APK**. Install future APKs as updates (or use
   `adb install -r Kingpin.apk`). Do not uninstall or clear app storage.

The helper creates an RSA/JKS keystore with alias `androiddebugkey`, store/key
password `android`, matching the personal-build script. The passwords are not a
substitute for protecting the private keystore. A pre-existing matching keystore
can instead be base64-encoded and saved as the same secret; do not use the helper
if you already have the signing key you need to preserve.

**Previously built APKs used ephemeral runner keys.** If the signing key of an
already installed APK was not saved, Android cannot accept a newly signed update
over it; there is no safe bypass. Back up saves/data before the one-time uninstall
and reinstall. After that, keep the persistent key unchanged. The keystore is
never included in the normal APK/game-data artifacts. Increase `versionCode` in
`android/AndroidManifest.xml` for releases; never decrease it for an update.
