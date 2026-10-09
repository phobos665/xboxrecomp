# Android port: KONKR Pocket FIT and AYN Thor

Status, Oct 2026: **plan, nothing built.** Measured against `main` at #52.

This replaces the Android parts of `vulkan-backend.md` §6.5 and its phase 7.
Those were written before the macOS port, and two of their three Android
blockers (x87 width, lifted code on arm64) are now retired by it; see §2.

---

## 0. What is being built, and what is not

**A per-title APK, built on a desktop and installed with `adb`.** The title is
lifted and compiled exactly as it is for Windows or macOS; only the last step
(the host build and its packaging) changes. An app that takes an ISO and
recompiles on the device was costed separately and rejected for now: compiling
millions of lines of generated C on a phone is the dominant cost and has not
been measured, it needs clang, lld, CPython and Capstone shipped in the APK,
and "give it an ISO and it plays" only holds for titles with a per-title fix
pack anyway. Revisit once this port has numbers.

Three rules carry over unchanged from the macOS port:

- **The APK holds the title's code.** Like the `.app`, it is for the device of
  the person who built it. It never goes to CI artifacts or releases. Game data
  is not packaged at all; it is pushed to the device separately (§5).
- **No build step starts a title.** No `adb shell am start`, no Gradle
  `installDebug && run` task, no ctest that launches the APK.
- **Changes that alter Windows behaviour** (lifter output, shared HLE, shared
  HLSL) need the Windows regression of TS2 and BLiNX before `main`.

---

## 1. Target devices

| | AYN Thor (Base / Pro / Max) | KONKR Pocket FIT | KONKR Pocket FIT Elite |
|---|---|---|---|
| SoC | Snapdragon 8 Gen 2 | Snapdragon G3 Gen 3 | Snapdragon 8 Elite |
| CPU | 1 prime + 4 performance + 3 efficiency | 1 prime + 5 performance + 2 efficiency | 2 prime + 6 performance (Oryon) |
| GPU | Adreno 740 | Adreno A32 | Adreno 830 |
| RAM | 8 / 12 / 16 GB LPDDR5X | 8 / 12 / 16 GB | 8-24 GB |
| OS (as shipped) | Android 13 | Android 14 (reported) | Android 16 (reported) |
| Main display | 6" OLED 1080x1920, 120 Hz, touch | 6" LCD 1920x1080, 144 Hz | same |
| Second display | 3.92" OLED 1080x1240, 60 Hz, touch | - | - |
| Cooling | active | copper heatsink and fan | same |

**Thor Lite (Snapdragon 865, Adreno 650) is out of scope.** Its stock driver
is unlikely to meet the Vulkan 1.3 floor (§3.9), and Adreno 6xx is the family
least likely to expose BC textures. Revisit only once both primary devices run.

The Pocket FIT Elite is the fastest of the three, but it has been reported as
likely to be discontinued. **Plan around the Thor (8 Gen 2) as the reference
device and the Pocket FIT G3 Gen 3 as the floor.**

Both run Android 13 or later, so **minSdk 33**. That one number settles
several things below: `memfd_create` (API 30), ELF TLS in bionic (API 29),
`ANativeWindow_setFrameRate` (API 30) and the ADPF performance-hint API
(API 33) are all available unconditionally.

### 1.1 Measure first, on each device

Spec sheets do not answer what the renderer needs. Before any port work, run
`vulkaninfo` (push the NDK build to `/data/local/tmp`; `adb shell` can execute
there) on each device and record the answers in this document:

| Question | Why | Where it matters |
|---|---|---|
| `apiVersion` >= 1.3 | The RHI is written to 1.3 core | §3.9 |
| `dynamicRendering`, `extendedDynamicState`, `extendedDynamicState2` | Same | §3.9 |
| `VK_KHR_push_descriptor` | Same | §3.9 |
| `textureCompressionBC` | Xbox titles are DXT-heavy | §3.8 |
| `D24_UNORM_S8_UINT` as a depth attachment | The RHI already falls back to D32S8 (`rhi_vulkan.c:1353`) | informational |
| `VK_EXT_4444_formats`, blendable B4G4R4A4 | A MoltenVK gap the RHI already works around | informational |
| `samplerMipLodBias` | MoltenVK lacked it | informational |
| Present modes | §3.10 | pacing |
| `currentTransform` of the window surface | §3.2 | orientation |

Also record the CPU topology from
`/sys/devices/system/cpu/cpu*/cpufreq/cpuinfo_max_freq`, which §3.12 needs.

---

## 2. What the macOS port already did for Android

Android is Linux on arm64. The macOS port proved the arm64 half; most of the
Linux half exists in the tree but **has never executed**, because no title has
run on Linux.

| Piece | State | Where |
|---|---|---|
| Lifted code correct on arm64 | **Proven** (TS2 and BLiNX on Apple Silicon) | `templates/runtime/recomp_types.h` |
| x87 width | **Not an Android problem.** The x87 stack is double-backed, so AArch64's 128-bit `long double` never enters | `recomp_types.h:391` |
| Guest memory as a 4 GB arena at an offset | Every POSIX host, not only Apple | `xbox_memory_layout.c:3110` |
| 16 KB host pages under 4 KB guest pages | Proven on macOS; newer Android devices may use 16 KB pages | `docs/technical/memory-layout.md` |
| Shared memory for mirrors | `memfd_create` on Linux (Android API 30+) | `posix_memory.c:107` |
| Faults: AArch64 load/store emulator | Proven on macOS; the Linux `ucontext` glue exists, unexecuted | `mmio_decode_a64.h:506`, `fault_posix.c:81` |
| Guest lock and back-edge yields | On by default on any arm64 host | `xbox_memory_layout.c` (`GUEST_LOCK_DEFAULT`) |
| Atomics, `cvt*2si`, divide-by-zero | Explicit on every host | lifter |
| Window, audio, pads | SDL3, which supports Android | `src/host`, `audio_output_sdl.c`, `src/input` |
| Renderer | Vulkan RHI with per-device format selection | `src/d3d/rhi_vulkan.c` |
| F9 overlay text | Has a non-GDI 8x8 font path off Windows | `d3d8_overlay.c:80` |
| SPIR-V shader cache | Keyed by the HLSL, so device-independent | `rhi_shader_cache.c` |
| FMV | FFmpeg loaded at run time | `src/video/xmv_decode.c` |

---

## 3. Gaps, measured against the tree

Sizes are relative: **S** is a day or two, **M** about a week, **L** more.

### 3.1 No Vulkan window surface off Windows and macOS (M); Linux has it too

`surface_ext()` (`rhi_vulkan.c:1100`) returns a surface extension only for
Win32 and Metal, and `v_device_create()` (`rhi_vulkan.c:1719`) prints
"no window surface on this platform yet" everywhere else. **So nothing has
been shown on a Linux screen either.** The renderer runs headless in CI only.

Add `VK_KHR_android_surface`, with the `ANativeWindow*` taken from SDL3
(`SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER`), and X11/Wayland surfaces for
desktop Linux in the same change. Alternatively, use `SDL_Vulkan_CreateSurface`
for every SDL host. That needs the loader's `vkGetInstanceProcAddr` handed to
SDL, because volk loads the loader itself. Either way it is one function and
the instance extension list.

### 3.2 Surface rotation (S to M)

`create_swapchain` sets `preTransform = caps.currentTransform`
(`rhi_vulkan.c:830`) but draws unrotated. On a panel whose native orientation
is portrait, Android reports `ROTATE_90`/`ROTATE_270` and an extent in panel
orientation. The Thor's main panel is specified as 1080x1920, so expect this
there. The frame then appears sideways.

Rotate in the display resolve, the one pass that already maps the back buffer
to the swapchain. The alternative is to pass `IDENTITY` and let the compositor
rotate, which costs a composition pass every frame on a battery device. Read
§4.4 of `vulkan-backend.md` before touching anything near the viewport: this
rotation is not the Y-flip and must not be folded into it.

### 3.3 The file system is case-sensitive (M); Linux has it too

`CreateFileA` on POSIX (`win32_compat.c:1212`) normalises backslashes and
calls `open()` on the path as given. The Xbox's file systems are
case-insensitive, and titles are not consistent about case. macOS's default
APFS volume is case-insensitive, which is why this never showed. Android's
`/data` is not.

Add a case-insensitive fallback on `ENOENT`: resolve the path component by
component with `readdir` and `strcasecmp`, and cache the directories. The same
goes for directory enumeration (`kernel_file.c:1320` already matches patterns
with `FNM_CASEFOLD`; it is only the open path that is missing). Android's
emulated shared storage happens to be case-insensitive, but do not rely on it:
desktop Linux needs this fix regardless.

### 3.4 Paths: per-user folders, the game folder, the log (S)

- `recomp_config.c:184` falls through to XDG and `$HOME` on anything
  non-Apple. Android sets no `HOME`, so every per-user directory comes back
  empty. Add an `__ANDROID__` branch **before** the Linux one, because
  `__ANDROID__` also defines `__linux__`. Use SDL's internal storage path for
  saves and settings, and the app's external files directory for the cache.
- `host_exe_path` (`host_main_posix.c`) reads `/proc/self/exe`, which on
  Android is `/system/bin/app_process64`. So the template's `find_game`
  (`templates/new-game/src/main.c:149`) and the `<executable>.log` fallback
  (`host_setup_output`) both look in the wrong place. Give both an Android
  branch: the game in `<external files dir>/game`, and stderr to logcat as well
  as a log file beside it. Change the template and regenerate with
  `scripts/regen_title_main.py`. Do not edit the titles' copies.

### 3.5 Entry point and linkage (M)

SDL3 on Android loads `libmain.so` and calls `SDL_main` on its own thread. The
title builds as an executable today (`add_executable` in each title's
`CMakeLists.txt`). Under the NDK toolchain, build it as a shared library named
`main`, with `SDL_main` exported. `recomp_host_loop_run` then runs on SDL's
thread, which is what `host_sdl.c` expects of "the main thread".

**Link one library, not two.** The guest registers (`g_eax` and the rest) are
`RECOMP_TLS` globals defined in the runtime (`xbox_memory_layout.c:2543`). If
the runtime were its own `.so`, every register access in lifted code would
become a cross-library TLS access through a TLS descriptor. Link the runtime's
static libraries and the lifted code into the single `libmain.so`, as the
desktop builds already do into one executable. Keep `ENABLE_EXPORTS`: the
fault report names functions with `dladdr`.

### 3.6 OpenSSL looks vestigial (S)

`src/kernel/CMakeLists.txt:55` requires `OpenSSL::Crypto` off Windows, but no
file under `src/` includes an `openssl/` header; the Xc* crypto ordinals are
implemented in the tree. Remove the link on Linux CI first. If it links, the
Android build needs no OpenSSL at all.

### 3.7 DXC on Android (M to L): **the main dependency risk**

Shaders are generated from the title's own data while it runs, so a compiler
has to be available. `rhi_vulkan_dxc.cpp:94` `dlopen`s `libdxcompiler.so`, and
there is no official Android build of DXC.

The way through is the shader cache that already exists. `rhi_shader_cache.c`
stores each compiled SPIR-V blob keyed by a hash of its HLSL and compile
settings, and SPIR-V does not depend on the device. So:

1. **First:** play the title through on desktop Vulkan (macOS or Linux), and
   bundle `<cache>/shadercache/<title id>/<backend>/` into the APK. On the
   device, log every miss and skip that draw. This proves everything else
   without DXC. Check that the backend tag (it names the compile settings)
   matches across hosts, or the cache will all miss.
2. **Then:** cross-compile DXC for `android-arm64` (it is LLVM-based and builds
   with CMake). Ship it (about 18 MB) for the misses. Expect a week of build
   fighting; budget it as a separate task.

Do not move the generators to GLSL or to HLSL 2021 to avoid this; see CLAUDE.md
and the `-HV 2018` note.

### 3.8 BC (DXT) textures (M if needed)

Unsupported formats are only reported today (`rhi_vulkan.c:443`); nothing
replaces them. Crowd-sourced WebGPU data shows BC exposed on Adreno 7xx
Android devices, which suggests the Thor's Adreno 740 has it. Adreno 830 is
likely too. The A32 in the G3 Gen 3 is unknown. Settle it with §1.1 rather
than guessing.

If any target lacks BC, add decode-on-upload as a format-table fallback using
`d3d8_dxt_decode_texel` (already exported from `d3d8_resources.c`). It costs 4
to 8 times the texture memory, which these devices can afford at Xbox texture
sizes. Transcoding to ASTC is the later optimisation, not the first step.

### 3.9 Vulkan 1.3 floor (S, if the devices pass §1.1)

The RHI targets 1.3 core plus push descriptors (`vulkan-backend.md` §4.1).
Adreno 740 drivers report 1.3. If the A32's driver does not, the fallback
§4.1 describes (a render-pass cache and a fatter pipeline key) is several
hundred lines. Decide on the §1.1 results, not before.

### 3.10 Present mode and refresh rate (S)

Android's swapchain offers FIFO and usually MAILBOX, but not IMMEDIATE. The
mode picker (`choose_mode`, `rhi_vulkan.c:727`) already falls back in order, and logs what it
chose. The `vulkan-backend.md` §4.10 warning applies: if it lands on FIFO with
shadow rendering, that is the Burnout 2 throttling regression by another
route. Read the log line on each device.

Refresh rate matters more here than on desktop:

- **Thor, 120 Hz:** 60 divides evenly, and `RECOMP_FRAME_INTERP=2` gives
  exactly 120.
- **Pocket FIT, 144 Hz:** 60 does not divide, so 60 fps judders. Call
  `ANativeWindow_setFrameRate(window, 60, ...)` (or 120 with interpolation) so
  the display switches mode. Check that the panel offers 60/120 modes.

### 3.11 App lifecycle (M)

Android destroys the window surface when the app goes to the background, and
may kill the process there. The RHI creates its surface once, in
`v_device_create`.

- On SDL's `WILL_ENTER_BACKGROUND`, stop drawing, destroy the swapchain and
  surface, pause the audio device, and **park the guest**. The guest lock is
  the natural place to park it: stop handing it out, and every guest thread
  stops at its next back edge or blocking call. On `DID_ENTER_FOREGROUND`, do
  the reverse with the new `ANativeWindow`.
- `create_swapchain` already returns -1 for a zero extent
  (`rhi_vulkan.c:816`). `VK_ERROR_SURFACE_LOST_KHR` and `OUT_OF_DATE` must
  take the same path rather than `VKCHECK` failing.
- Lock the activity to landscape and handle config changes in the manifest
  (SDL's template does), so a rotation never recreates the activity.
- A killed process loses unsaved progress, as on the console when the power
  goes off. That is acceptable; do not try to snapshot guest state.

### 3.12 CPU placement and clocks (S)

`guest_cpu_pin` (`xbox_memory_layout.c:913`) ignores `RECOMP_GUEST_ONE_CPU`
off Apple. Android does not let an app request a performance level the way
macOS QoS classes do, but it does allow `sched_setaffinity` on the app's own
threads. With the guest lock on, only one guest thread runs at a time anyway,
like the console's single CPU. So pinning **all guest threads to the prime
core** costs nothing. Keep host threads (audio, the SDL thread) on the
performance cores. Find the cores by `cpuinfo_max_freq`, not by number.

Report the frame target to the ADPF performance-hint API (`APerformanceHint`,
API 33) from the flip gate, so the governor does not clock down between
frames. Both devices also have their own performance-mode switches; record
which mode each measurement used.

### 3.13 Input and hot keys (S to M)

The built-in controls appear to SDL3 as an ordinary gamepad, so
`src/input/input_bindings.c` works as on desktop. Check the default mapping
on both devices, especially the analog triggers, and that black and white
land somewhere sensible.

F9, F10 and F11 have no keyboard. Bind them to a pad chord in
`input_bindings.json` (a new action type, read in the same place the keys
are). Make Android's back gesture or button open the same thing, rather than
exiting. `tools.input_ui` stays on the desktop; push its JSON with `adb`. The
Thor's touch bottom screen is a natural home for the overlay and these
controls, but it is optional and comes last (§6, phase 7).

### 3.14 Signals under the Android runtime (S, must be tested)

ART installs its own SIGSEGV handler and chains app handlers through
libsigchain. The runtime's SIGSEGV/SIGBUS handlers (`fault_posix.c`) must
still see MMIO and watchpoint faults, at the rate a title causes them. The
existing tests cover this: `tests/guest_faults` and `tests/mmio_decode_a64`,
run on the device (§6, phase 2). Run them inside the APK as well, because the
shell has no ART.

### 3.15 FFmpeg (S)

`xmv_decode.c:58` `dlopen`s `libavcodec.so.<major>`. APK native libraries
must be named `lib*.so` with no version suffix, so add Android names. Build
FFmpeg's LGPL shared libraries for `arm64-v8a` with only the decoders XMV
needs. Without it, movies are skipped, as on any host without FFmpeg, so this
can come late.

### 3.16 Not problems

- **Address space:** the 4 GB arena (plus 64 KB guard, aligned to 4 GB) is a
  small fraction of a 39-bit user address space.
- **Physical memory:** mirrors are mappings of one object, so guest RAM costs
  64 or 128 MB once. Decoded DXT (§3.8) is the only large new cost.
- **Audio:** SDL3 uses AAudio. Both devices are natively 48 kHz, like the
  Xbox.

---

## 4. Do Linux first

§3.1 and §3.3 mean desktop Linux cannot show a title today. Every Linux
item is also an Android item, and Linux is far easier to debug. So the first
two phases are Linux.

GitHub's `ubuntu-24.04-arm` runners give **Linux arm64** in CI: the fault
emulator's Linux `ucontext` path, `memfd`, the guest lock and the arm64 memory
tests, all under the same kernel interfaces Android uses. Add that job beside
the existing `ubuntu-24.04` one. Lavapipe gives it a Vulkan device for
`tests/d3d8_replay`.

---

## 5. Packaging

One script, like `scripts/make_macos_app.py`:

```
python3 scripts/make_android_apk.py titles/<name> [--ndk <dir>] [--install]
```

- **Template:** `templates/android/`, a Gradle project holding SDL3's
  `SDLActivity` Java glue (from the `third_party/SDL` submodule), a manifest
  and a `build.gradle` that takes prebuilt `jniLibs`. There is no per-title
  Java.
- **Native build:** CMake with the NDK's `android.toolchain.cmake`,
  `ANDROID_ABI=arm64-v8a`, `ANDROID_PLATFORM=android-33`, Release. Output:
  `libmain.so` (§3.5), plus `libdxcompiler.so` and FFmpeg when present.
- **Package name** per title (`org.xboxrecomp.<title>`), so several titles can
  be installed side by side. Signed with the debug key; this is not a store
  build.
- **Output** in `titles/<name>/build/android/`, which is ignored. `--install`
  runs `adb install -r` and nothing more. It does not start the title (§0).
- **Shader cache** (§3.7): copied from the desktop cache directory into the
  APK's assets, and unpacked into the cache directory on first run.
- **Game data:** `adb push games/<title>/. /sdcard/Android/data/<package>/files/game/`,
  or copy over USB. App-specific external storage needs no permission.
- **Saves** go in internal storage, not beside the game. The FATX partition
  images are sparse, and a microSD card formatted exFAT has no sparse files,
  so they would take their full size there. `\Device\Harddisk0\Partition1\`
  still maps to the game folder (CLAUDE.md), so a title that saves to E: writes
  into the game folder on the device. That is fine on a player's device.
- **CI** builds the runtime, the tests and `src/replay` with the NDK (an NDK
  is preinstalled on GitHub's Ubuntu runners). It never builds or packages a
  title.

---

## 6. Phases

Each phase has a pass condition a log or a capture can answer.

**Phase 0: desktop Linux shows a title.** §3.1 (X11/Wayland surface), §3.3
(case-insensitive paths), §3.6 (OpenSSL). *Pass: TimeSplitters 2 plays
in-level on x86-64 Linux, with sound and a pad.*

**Phase 1: Linux arm64 in CI.** The `ubuntu-24.04-arm` job runs the full
ctest set: `guest_memory`, `guest_faults`, `mmio_decode_a64`,
`memory_layout`, `compat_threads` and `d3d8_replay` on lavapipe. *Pass: green
on arm64 Linux.*

**Phase 2: the runtime on the devices, no title.** NDK build of the runtime
and tests. Push them to `/data/local/tmp` and run them over `adb shell`. Run
`vulkaninfo` and fill in §1.1. *Pass: the tests pass on the Thor and the
Pocket FIT, and §1.1 is filled in for both.*

**Phase 3: a replayed frame on the devices.** Android's loader is not
expected to offer `VK_EXT_headless_surface`, so replay either draws into a
window (a minimal APK around `src/replay`) or into an offscreen image read
back (a small RHI addition, useful anywhere). Exercises §3.1, §3.2, §3.8 and
§3.9 with no game running. *Pass: a captured TS2 menu frame and in-level frame
replay on both devices and match the desktop Vulkan replay within a small
per-pixel tolerance. Different GPUs are not expected to match byte for byte;
the byte-identical claim in CLAUDE.md is same-machine.*

**Phase 4: TimeSplitters 2 in an APK.** §3.4, §3.5, §3.7 step 1, §3.10,
§3.13 (pad only), and the packaging script (§5). *Pass: TS2 reaches its front
end on the Thor with sound and the built-in pad, and the log names the present
mode, the paths and the shader cache hits and misses.*

**Phase 5: play, pace and survive.** §3.11 lifecycle, §3.12 placement and
hints, §3.14 in the APK. Measure frame time with `RECOMP_FPS_OVERLAY=1` and
`scripts/fps_report.py` on pulled logs. *Pass: TS2 in-level holds 60 fps for
30 minutes on the Thor in its default performance mode, and survives 10
background/foreground cycles with sound and pad intact.*

**Phase 6: the floor device and the second title.** Pocket FIT G3 Gen 3,
§3.8 if its BC answer is no, BLiNX, §3.7 step 2 (DXC) and §3.15 (FFmpeg).
*Pass: both titles play on both devices with movies, and with no shader-cache
misses skipped.*

**Phase 7: comfort.** Hot-key chord and back-button menu (§3.13), the Thor's
bottom screen for the overlay, internal resolution and widescreen per
`widescreen-and-resolution.md`.

---

## 7. What is unknown, and what would change the plan

- **Performance.** The macOS port says 60 fps on Apple Silicon. It says
  nothing about phone cores: both 8 Gen 2 and G3 Gen 3 are slower per core,
  and throttle. If phase 5 misses 60, measure where with `RECOMP_SAMPLE`
  before reaching for CLAUDE.md's planned change #2 (the register model). That
  change is the right fix for arm64 in principle, but it is a lifter rewrite
  and it bakes into every title's generated code.
- **Adreno A32's driver**: Vulkan version, BC, present modes. §1.1 answers it
  on day one.
- **DXC on Android**: §3.7 step 1 keeps it off the critical path, but a title
  that generates new shader variants deep into play will skip draws until
  step 2 lands.

---

## Sources for the device specs

- [AYN Thor variants and specs (Liliputing)](https://liliputing.com/ayn-thor-is-dual-screen-android-handheld-game-console-with-oled-displays-and-qualcomm-snapdragon-inside/)
- [AYN Thor release specs (Time Extension)](https://www.timeextension.com/news/2025/08/ayn-reveals-release-date-colours-and-specs-for-its-pocket-ds-rival)
- [KONKR Pocket FIT (DroiX)](https://droix.net/product/konkr-pocket-fit/) and [review](https://droix.net/blogs/en-gb/konkr-pocket-fit-review/)
- [KONKR Pocket FIT Elite (Retro Catalog)](https://retrocatalog.com/retro-handhelds/konkr-pocket-fit-elite)
- [Snapdragon G3 Gen 3 (ITC)](https://itc.ua/en/news/qualcomm-unveiled-snapdragon-g3-g2-and-g1-for-portable-consoles-the-first-devices-with-prices-starting-from-120-are-already-available/)
- [BC texture support on Android by GPU family (web3dsurvey)](https://web3dsurvey.com/webgpu/features/texture-compression-bc/platform/Android)
