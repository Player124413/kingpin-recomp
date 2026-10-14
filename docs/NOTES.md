# Technical notes

How the port works, what was verified, and the debugging knobs. Setup and building are in the
[README](../README.md).

The game's own x86 binaries are recompiled to native arm64 code; no third-party engine or source
port is used. `kingpin.exe`, `ref_gl.dll` and `main/gamex86.dll` (including their statically linked
MSVC C runtime) are translated to C++ by `tools/x86recomp.py`. A Win32 layer (`runtime/win32`)
provides the operating system they expect on top of a small platform layer (`runtime/platform`):
Cocoa on macOS, UIKit on iOS/iPadOS, NativeActivity on Android. The game's fixed-function OpenGL
1.x runs on an emulation over GL 3.2 core / GL ES 3.0 (`runtime/gfx/glemu.cpp`).

## Playing

```
./run.sh                       # starts the game (D:\ is read from track1.iso)
./run.sh +map sr1              # straight into the first level
./run.sh +set developer 1      # enables the console (` key); Kingpin gates it behind developer
```

- Keyboard: Kingpin's defaults. Arrows move and turn, Ctrl attacks, Shift runs, F activates, etc.
  Mouse look works while the window has focus (the pointer is captured and hidden).
- Cmd+Enter (or Cmd+F) toggles fullscreen. Cmd+= / Cmd+- change the brightness. Cmd+P toggles the
  post effects. Cmd+Q quits.
- The intro movie plays on start (Escape skips it; `KP_NO_INTRO=1` turns it off).

- Game controllers (Xbox, PlayStation, Switch Pro, MFi; GameController framework on Apple,
  Android input events) drive the game's own joystick code: left stick moves, right stick
  looks, A jump, B crouch (or puts away a note/notebook/inventory that is up), X activate, Y
  reload, LB/RB weapon prev/next, LT run, RT fire, L3 flashlight, R3 use the selected item,
  Back/View holster, D-pad up/down conversation replies, left/right select item, Start = menu
  (also puts notes away). In menus the pad
  navigates (stick/D-pad, A = Enter, B = Escape). The bindings live in `main/config.cfg`
  (buttons 5+ are AUX5.. in Quake 2's numbering). Verified with the scripted fake pad
  (`KP_PAD`) on the Mac only; a physical controller has not been tried on any platform yet.

### iOS / iPadOS

```
ios/build.sh sim       # simulator app: build-ios-sim/out/Kingpin.app
ios/build.sh device    # device app, signed (see the provisioning note in ios/build.sh)
xcrun devicectl device install app --device <udid> build-ios-device/out/Kingpin.app
```

- Game data goes in the app's Documents folder (Files app, or `xcrun devicectl device copy to
  --domain-type appDataContainer --domain-identifier com.blounty.kingpin`): `kingpin.exe`,
  `ref_gl.dll`, `kpintro.exe`, `main/`, `track1.iso` (the disc's data track: D:\ is read from it
  directly), and optionally `cd/<cue>` with the audio track .bin files for the music.
- Touch controls (`runtime/touch_controls.cpp`, shared with Android): floating move stick on the
  left, swipe to look on the right, FIRE JUMP DUCK USE RELOAD WPN-/WPN+ MENU buttons; in menus a
  D-pad with OK/BACK. Hidden while a controller is in use; a touch brings them back. A tap skips
  the intro.
- The guest needs a 4 GB virtual address reservation. An iPhone 17 Pro Max on iOS 27 grants it
  without the Extended Virtual Addressing entitlement (personal teams cannot get that one); the
  app shows a message if a device refuses.
- The simulator's OpenGL ES is a CPU renderer: expect 2-4 fps there (it defaults to a 0.4 render
  scale with effects off) and occasional black triangles that real GPUs do not show.
- Uses the UIScene lifecycle (required on iOS 27).

### Android

```
android/build.sh                                  # build-android/Kingpin.apk (no Gradle)
adb install -r build-android/Kingpin.apk
android/push_data.sh game track1.iso "<path>/Kingpin - Life of Crime (USA).cue"
```

- Data: `/sdcard/Android/data/com.blounty.kingpin/files` (same layout as iOS). push_data.sh
  also opens the directories adb creates to the app. Uninstalling the app deletes this folder.
- `kp_env.txt` there sets `KP_*` options (KEY=VALUE lines; `ARGS=+map sr1` adds game arguments).
  The game console log is mirrored to logcat (`adb logcat -s kingpin`).
- Immersive full screen (`android/java/.../KingpinActivity.java`), back key = Escape, keyboards
  and game controllers work, AAudio output. Tested on the arm64 emulator (Android 16) and on a
  phone (NX809J, Snapdragon SM8850 / Adreno 840, Android 16): steady 60 fps at 2688x1216 with
  all effects (that phone keeps third-party apps at 60 Hz; the app asks for 120). Save/load
  verified on the emulator.
- Vertex streaming on GL ES uses client-side arrays: Adreno flushed the (multisampled) render
  pass on every mapped/updated buffer range, which cost 220 ms a frame (`KP_GL_STREAM=map|
  subdata|client` to compare).
- Some phones silence app logs (`log.tag` = S); `adb shell setprop log.tag V` turns them on.

## Graphics improvements (host side; the game code is unchanged)

- **Fullscreen:** follows the game's own setting (`vid_fullscreen`, default on) and the intro's
  DirectDraw exclusive mode, using native macOS fullscreen. Cmd+Enter toggles it;
  `KP_WINDOWED=1` forces a window. Windows fit the visible screen at the game's aspect ratio.
- **Native resolution:** video mode 6 (the game's 1600x1200 entry) becomes the screen area a
  fullscreen window gets (below the camera housing on notched displays), or `--res WxH`. It is
  set by `extras/port.cfg`; the video menu can still pick the original modes.
- **Widescreen:**
  - The 3D view fills the screen with a Hor+ field of view (the 4:3 vertical FOV, widened
    horizontally, with frustum culling to match).
  - HUD, menus, console and full-screen pictures are laid out at 4:3, centred with black bars, so
    they are not stretched (`KP_STRETCH_2D=1` stretches them instead).
  - 2D is scaled by height/600 (`KP_HUD_SCALE`).
  - `KP_NO_HORPLUS=1` restores the original cropped view.
- **HD textures:**
  - All 3073 world textures, model skins and sprites were upscaled 4x with Real-ESRGAN
    (`tools/upscale_textures.py`, PyTorch on the Apple GPU).
  - World textures are upscaled with wrap-around padding so they tile seamlessly; cut-out
    alpha stays sharp.
  - A colour-correction pass keeps the originals' colours, so the AI only adds detail.
  - At runtime a hook on ref_gl `GL_LoadPic` swaps the upload for the HD image, keeping the
    original size for texture coordinates. The game's own gamma/intensity processing is learned
    from its upload of the original and applied to the replacement.
  - Stored in `game/main/hd/` (about 1.1 GB, generated locally from your own game files with
    `tools/upscale_textures.py`; never commit or share them). Without them the original
    textures are used; `KP_HD_TEXTURES=0` turns them off. Levels load about 1.5 s slower.
- **Post-processing** (`runtime/win32/gl_post.cpp`):
  - The game renders off-screen with 4x MSAA (`KP_MSAA`).
  - When it switches from the 3D view to 2D, the 3D image gets screen-space ambient occlusion
    from the depth buffer (`KP_SSAO`, default 0.8) and bloom on bright lights (`KP_BLOOM`, 0.35).
    The HUD is drawn on top, unaffected.
  - The final pass applies a contrast-adaptive sharpen (`KP_SHARPEN`, 0.25) and the brightness
    curve (default gamma 1.3; Cmd+= / Cmd+- adjust it live and the value is saved in
    `game/main/kpmac_gamma.txt`).
  - Cmd+P toggles AO, bloom and sharpening for comparison. `KP_POST=0` renders directly (no
    effects). `KP_POST_DEBUG=1` shows the AO buffer.
- **Texture filtering:** 16x anisotropic (`KP_ANISO`) and trilinear mipmaps.
- **Full-size textures:** `gl_maxtexsize 1024` (the game shrank textures to 256).
- **Frame rate:** 120 fps (`cl_maxfps 125`, paced by vsync).
- **Model shadows:** `gl_shadows` was tried and left off. Kingpin casts each character's shadow
  from its nearest light, so shadows point in different directions.
- All 52 maps were re-tested with everything on: no faults. Presentation was checked with real
  window captures (`KP_WINDOW_CAPTURE=file.png,seconds`), not just GL readbacks. A readback
  missed a black window once.

## Verified (automated, with frame dumps)

- Start-up logo and main menu; new game from the menu, then the opening cutscene (kpcut1).
- All 52 maps load and run with no traps or faults at 120 fps. Run `tools/smoke_maps.sh`
  (`KP_WINDOWED=1 KP_NO_INTRO=1 tools/smoke_maps.sh 15 4`).
- Gameplay input: movement, turning, mouse look, attacks, weapon switching. A 2-minute
  random-input soak test on sr1 ran without problems.
- Sound effects and voices through waveOut → CoreAudio (checked on the recorded output); CD
  music when enabled.
- Error recovery (`Com_Error` → longjmp), level reloads with game DLL reload, save and load.
- Instruction semantics: 3530 differential test cases against Unicorn (QEMU x86) pass
  (`tools/x86test.py`).

## Known gaps

- Networking is disabled: winsock sockets fail, so single player works over loopback but there
  is no multiplayer.
- Mobile: no on-screen keyboard (the console and chat need a hardware keyboard), and save
  games, config and screenshots stay in the app's data folder.
- The start-up menu runs at 10 fps. That is the original behaviour: Kingpin keeps the client
  "connected" while the logo picture shows. The game runs at full rate once a map loads.
- The game sees a "D:" CD drive backed by the data track image, `track1.iso`, read directly
  (`runtime/win32/iso9660.cpp`; `KP_DISC` can point at a directory instead). Its CD check also
  needs the disc's track lengths, hence the cue sheet in `cd/`.

## Intro movie (kpintro.exe)

The intro is a separate program: an Interplay MVE player using DirectDraw (8-bit palettized
640x480) and DirectSound. It is recompiled into its own host executable (`build/kpintro`).
`kingpin.exe` starts it with CreateProcess and waits, as on Windows. iOS and Android cannot start
processes, so there the player is compiled into the game binary as well (its generated code
wrapped in `namespace intro`, see `add_intro_in_process` in CMakeLists.txt) and runs first on the
guest main thread; its ExitProcess unwinds back to `platform/app.cpp`, which resets the Win32
state it used and loads the game (`-DKP_INTRO_IN_PROCESS=ON` does the same on the Mac).
- `runtime/win32/ddraw.cpp` and `dsound.cpp` implement the COM objects (vtables of HLE thunks,
  `com.h`).
- The MVE decoder modifies its own code: it patches register-selection bytes in its block
  templates.
  - `tools/smc_sites.py` finds the 1232 patched bytes and the values they can take (from the
    patch tables).
  - `x86recomp.py --smc tools/kpintro_smc.txt` compiles each such instruction as a runtime
    switch over its variants.
- The decoder's threaded dispatch (`push next; jmp handler`, handler ends in `ret`) is handled by
  ret-dispatch in functions that push code addresses.
- Regenerate:
  `.venv/bin/python tools/x86recomp.py gen_intro game/kpintro.exe --smc tools/kpintro_smc.txt`

## Layout and pipeline

- `track1.iso`, `cd/`, `game/`: your disc and installed game (`tools/install_game.sh`, git-ignored).
- `tools/disc.py`: data track → ISO conversion, file extraction from the ISO.
- `tools/x86recomp.py`: recompiler. It does function discovery (entry, exports, call targets,
  relocations, data references), jump tables (including end-relative ones), flag liveness
  (flags are carried across calls only where needed), x87 and string ops, and setjmp/longjmp
  overrides (`tools/overrides.txt`). Run:
  `.venv/bin/python tools/x86recomp.py gen game/kingpin.exe game/ref_gl.dll game/main/gamex86.dll --seeds tools/seeds.txt --overrides tools/overrides.txt`
  If the runtime traps on "call to code that was not recompiled", the address is appended to
  `game/kp_missing.txt`; add it to `tools/seeds.txt` and regenerate.
- `tools/gen_gl.py`: opengl32 thunks generated from the SDK `gl.h`.
- `tools/pak.py`: lists and extracts PAK files.
- `tools/smoke_maps.sh`: the all-maps test.
- Build: `cmake -S . -B build -G Ninja && ninja -C build` (about 1 minute).
- Runtime:
  - `runtime/x86`: CPU state, dispatcher, x87, string ops, setjmp.
  - `runtime/win32`: kernel32 (memory, files, threads, synchronization), user32/gdi32/wgl,
    winmm (waveOut, MCI CD audio), joystick (gamepad.cpp), DirectDraw/DirectSound, GL texture
    handling and post-processing.
  - `runtime/gfx/glemu.cpp`: fixed-function GL on GL 3.2 core / GL ES 3.0 (batched immediate
    mode, matrix stacks, an uber-shader for texenv/alpha test/fog).
  - `runtime/platform`: `platform.h` (window, GL context, present/pacing, audio, images,
    input entry points), `app.cpp` (portable start-up), `mac.mm`, `ios.mm`, `android.cpp`,
    `gc_gamepad.mm` (Apple controllers), `apple_images.inc`.
  - `runtime/touch_controls.cpp`: on-screen controls (iOS, Android; `KP_TOUCH=1` on the Mac
    makes the mouse a finger).

## Debug knobs

- `KP_FRAME_DUMP=dir[,N]`: PNG of every Nth frame.
- `KP_INPUT="t:key:dur,..."`: scripted input. Keys: a-z 0-9 enter escape space tab up down left
  right ctrl shift alt f1-f12 mouse1 `` ` ``; `lookN` turns by N mouse units; `type text` types a
  console line.
- `KP_TRACE_HLE`, `KP_TRACE_GPA`: log every Win32 / GetProcAddress call.
- `KP_GL_ERRORS`, `KP_NO_AUDIO`, `KP_AUDIO_DUMP=file` (raw float stereo 44.1 kHz), `KP_CUE`.
- `--seconds N`: quit after N seconds; `+set logfile 2` writes `game/main/qconsole.log`.
- `KP_FRAMETIMES=1`: frame pacing statistics every 2 s. `KP_PACING=0`: GL vsync instead of the
  display link.
- `KP_WINDOW_CAPTURE=path,secs`: saves what the Mac window actually shows.
- `KP_PAD="t:control:value:dur,..."`: fake gamepad. `KP_TOUCH=1`, `KP_TOUCH_SCRIPT=
  "t:x:y:x2:y2:dur,..."` (screen fractions), `KP_TOUCH_LOOK` (look speed).
- `KP_DISC=path` (directory or .iso), `KP_TRACE_FILES`, `KP_TRACE_DISC`, `KP_RENDER_SCALE` (mobile).

## Notable fixes along the way

- MSVC `memcpy` jump tables are bounded by `and reg, 3` (not `cmp`) and are sometimes addressed
  from their end with a negative index.
- The CD check reads the whole disc length in minutes via MCI (it must be between 75 and 99).
- Dynamic lightmaps: `glTexSubImage2D` stalled on the GPU (halving the frame rate). Guest texture
  names are now mapped to host textures, and updated textures are re-uploaded into fresh ones.
- MSVC CRT math helpers return results in CPU flags. The recompiler finds the functions whose
  flags are live at entry or after return and passes them through `Cpu::fl`.
