# Building Kamiframe

You need a C++17 compiler, CMake 3.20 or newer, Git, and a Python 3
interpreter (`find_package(Python3 REQUIRED)` in `simulator/CMakeLists.txt` —
used by the asset pipeline and code-generation steps the build runs). SDL3 is
fetched and built automatically the first time, which takes a few minutes; every
build after that is seconds.

## Quick start

On Linux, macOS, or WSL2, the whole thing is one command:

```
bash dev.sh run
```

That configures, builds, and launches the simulator, in that order, every
time — there is nothing else to remember. `bash dev.sh stress` runs the
full-screen stress demo instead, `bash dev.sh test` runs the automated
checks, and `bash dev.sh clean` deletes the build output if something gets
into a state you'd rather just start over from. Run `bash dev.sh` with no
argument for the full list. (`./dev.sh run` works the same way after a
one-time `chmod +x dev.sh`, if you'd rather drop the `bash `.)

`dev.sh` is a thin wrapper — it runs the exact CMake commands below, plus,
on WSL2 specifically, it automatically points build output at your Linux
home directory instead of the Windows drive the repo lives on, which is the
single biggest speed difference a WSL2 user can make (see the WSL2 section)
and now happens without anyone needing to know to ask for it.

Native Windows (MSVC) is not yet wrapped by `dev.sh` — use the "Windows,
natively" section below for that.

## What `dev.sh` is actually running

```
cmake -B build
cmake --build build
```

Then run one of:

```
build/kamiframe-sim              # window, the demo creature's Home screen (care loop, animation, dirty-rectangle repaint)
build/kamiframe-sim --stress     # scrolling tilemap + 12 sprites, 100% redraw
build/kamiframe-headless         # no window at all, checks frames, used by CI
```

On Windows the binaries land in `build\RelWithDebInfo\` unless you say
otherwise.

---

## Windows, natively (recommended for day to day)

Install [Visual Studio Build Tools](https://visualstudio.microsoft.com/downloads/)
with the "Desktop development with C++" workload, and
[CMake](https://cmake.org/download/). Then, from the repository folder:

```
cmake -B build
cmake --build build --config RelWithDebInfo
build\RelWithDebInfo\kamiframe-sim.exe
```

No package manager, no display server, no path translation. The simulator is a
normal Windows window.

You do not need a GCC build locally: the GitHub Actions workflow builds with
GCC on every push, which is what catches the differences that matter for the
ESP32 (see below).

---

## WSL2

Works, and has one advantage worth knowing: WSL2 builds with GCC, which is the
same compiler family the ESP32 uses. GCC is stricter than Microsoft's compiler
and rejects things MSVC accepts, so a WSL2 build catches device problems
earlier. It is also where `#pragma GCC poison` (the mechanism that keeps the
heap out of core) actually has teeth.

### Two things that will bite you

**1. Building on `/mnt/d/...` is slow.** WSL2 reaches Windows drives over a
network-style protocol, and a build creates thousands of small files. Keep the
source where it is, next to your git repository, but put the *build output* on
the Linux side:

```
cmake -B ~/kf-build -S .
cmake --build ~/kf-build -j$(nproc)
~/kf-build/kamiframe-sim
```

Most of the I/O is build output, so this is usually a large improvement for a
one-line change.

If you use `dev.sh` (see Quick start, above), this happens for you: it
detects a WSL2 repo sitting on a Windows drive and redirects build output to
`~/.cache/kamiframe-build` automatically, printing a line saying so the
first time. This is the same fix as the manual commands above, just applied
without you needing to remember it exists.

**2. You need a display for the window.** Windows 11's WSL2 includes WSLg, so
a window just appears. On Windows 10 you may need an X server. If there is no
display, `kamiframe-sim` will exit with an SDL error about no video device.
`kamiframe-headless` needs no display at all and still runs the real firmware,
so it is always available as a fallback.

**3. Gamepads do not reach a WSL2 build.** SDL finds controllers on Linux by
reading evdev out of `/dev/input`, and WSL2 does not populate that directory —
there is usually no `/dev/input` at all. So `sdl_input.cpp` never receives an
`SDL_EVENT_GAMEPAD_ADDED`, never logs `gamepad connected: ...`, and the pad
appears dead no matter how it is plugged in. Nothing is wrong with the build
and there is no flag to enable: gamepad support is automatic wherever SDL can
see the device. **Use the native Windows build for controller work** — SDL
talks to XInput directly there and the same code picks the pad up. Keyboard
input works everywhere and is unaffected.

### If configure fails on a missing X11 package

`cmake/fetch_sdl.cmake` already switches off the X11 extensions this project
cannot use (`XSCRNSAVER`, `XTEST`, `XDBE`), which is what used to make a first
build a game of installing one package, re-running, and finding the next one.

If your distribution is missing the base X11 headers as well, this covers it:

```
sudo apt-get update
sudo apt-get install -y build-essential cmake git \
  libx11-dev libxext-dev libxrandr-dev libxi-dev \
  libxcursor-dev libxfixes-dev libxkbcommon-dev \
  libpulse-dev libasound2-dev
```

### If the simulator runs but is silent

The last two packages are the audio ones, and they have to be present **when
SDL is configured**, not merely when you run. SDL compiles one backend per set
of headers it finds; miss them and it builds with only its `dummy` driver,
which accepts every sample and plays none. Nothing errors — `kf_audio_init()`
opens the dummy device successfully and logs its usual `SDL3 audio: 48000 Hz
mono`, so a silent build looks exactly like a working one.

This bit Chris on 2026-08-16, because this page used to say audio libraries
were deliberately omitted and to add them "when the audio HAL lands". It
landed on 2026-08-12 (`f5c4714`) and the line was never updated.

Runtime libraries are not enough on their own: SDL's `_DYNAMIC` backends
`dlopen()` `libpulse.so.0` at runtime but still need its headers at build
time to be compiled at all. To check what a build actually got:

```
grep SDL_AUDIO_DRIVER_PULSEAUDIO \
  <build-dir>/_deps/sdl3-build/include-config-*/build_config/SDL_build_config.h
```

`#define ... 1` is what you want; `/* #undef ... */` means silence. If it is
undefined, install the packages and then **delete the build directory** —
SDL's probe results are cached in the top-level `CMakeCache.txt`, so
reconfiguring in place will not re-detect them. Re-pass any `-D` options you
were using, `KF_ASSET_PACK` included.

On WSL2 the rest of the path is already in place: WSLg exposes a PulseAudio
server at `/mnt/wslg/PulseServer` and sets `PULSE_SERVER` to match.

---

## Linux and macOS

Same as the first block. On macOS, Xcode command line tools plus CMake.

---

## Useful build options

| Option | Default | Why |
|---|---|---|
| `-DKAMIFRAME_WARNINGS_AS_ERRORS=ON` | OFF | What CI uses. Turn it on before pushing. |
| `-DKAMIFRAME_BUILD_SIMULATOR=OFF` | ON | Skips SDL entirely. Builds in seconds, gives you `kamiframe-headless` only. Good for a quick check. |
| `-DCMAKE_BUILD_TYPE=Debug` | RelWithDebInfo | Slower, better debugging. Note the arena and budget checks are always on in every configuration, on purpose. |
| `-DFETCHCONTENT_SOURCE_DIR_SDL3=/path/to/SDL` | unset | Use a local SDL checkout instead of downloading. Handy offline. |

## Tests

```
bash dev.sh test
```

runs both the `ctest` suite and `check_no_heap.py` together. The two
commands it wraps, if you want to run them separately:

```
ctest --test-dir build --output-on-failure
```

The suite has grown well past its original handful and keeps growing — run
`ctest --test-dir build -N` for today's exact count and list rather than
trusting a number here (44 with the default `KF_ENABLE_LVGL=OFF` as of
2026-08-11, 46 with it `ON`). A few of the oldest and most load-bearing:

- **headless_determinism** hashes every rendered frame and compares against a
  known value. If rendering changed, this fails. When the change was
  deliberate, run `kamiframe-headless --frames 300 --seed 0x5EEDCAFE`, take the
  checksum it prints, and update `KAMIFRAME_GOLDEN_CHECKSUM` in
  `simulator/CMakeLists.txt` in the same commit.
- **headless_dirty_area** fails if the simple demo starts redrawing more of the
  screen than it needs to. That would look perfectly fine on your PC and halve
  the frame rate on hardware.
- **headless_fullscreen** does the same for the full-screen stress mode.
- **storage_power_check** proves save state survives and offline pet ageing
  actually works, deterministically, without waiting real days for it. See
  `docs/architecture/adr-0012-storage-and-power.md`.
- **lvgl_determinism_check** proves the LVGL port glue renders
  deterministically, the same property headless_determinism proves for the
  custom engine. See `docs/architecture/adr-0013-lvgl-for-menus.md`. **Only
  registered when built with `-DKF_ENABLE_LVGL=ON`** — LVGL defaults OFF
  (ADR 0045), so this test does not run in a plain `bash dev.sh test`.

Plus one check that is not a ctest:

```
python3 tools/check_no_heap.py .
```

## Save data

`kamiframe-sim` creates a `kf_save/` folder next to wherever you run it from,
the first time it runs, with no configuration needed. That is where save
state (`kf/hal/storage.h`) lives on desktop. Add `kf_save/` to your
`.gitignore` if it is not already covered — it is per-machine data, not
something to commit.

## The ESP32 build

Buildable, and it boots on real hardware. `idf.py build` produces a real
`kamiframe-firmware.elf`/`.bin` for the esp32s3 target from `ports/esp32/` —
same `hakoniwaos/` sources, real ESP-IDF v6.0.2 toolchain, real display and
input HAL backends. See `ports/esp32/README.md` for setup, panel selection
(`-DKF_PANEL=st7789` or `ili9341`, defaulting to `ili9341`), and what has
been confirmed on the bench versus in Wokwi only.
