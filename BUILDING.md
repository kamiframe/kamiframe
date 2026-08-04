# Building Kamiframe

You need a C++17 compiler, CMake 3.20 or newer, and Git. Nothing else. SDL3 is
fetched and built automatically the first time, which takes a few minutes; every
build after that is seconds.

```
cmake -B build
cmake --build build
```

Then run one of:

```
build/kamiframe-sim              # window, one sprite, dirty-rectangle repaint
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

**2. You need a display for the window.** Windows 11's WSL2 includes WSLg, so
a window just appears. On Windows 10 you may need an X server. If there is no
display, `kamiframe-sim` will exit with an SDL error about no video device.
`kamiframe-headless` needs no display at all and still runs the real firmware,
so it is always available as a fallback.

### If configure fails on a missing X11 package

`cmake/fetch_sdl.cmake` already switches off the X11 extensions this project
cannot use (`XSCRNSAVER`, `XTEST`, `XDBE`), which is what used to make a first
build a game of installing one package, re-running, and finding the next one.

If your distribution is missing the base X11 headers as well, this covers it:

```
sudo apt-get update
sudo apt-get install -y build-essential cmake git \
  libx11-dev libxext-dev libxrandr-dev libxi-dev \
  libxcursor-dev libxfixes-dev libxkbcommon-dev
```

Audio libraries are deliberately not in that list. SDL only *warns* when they
are missing, and Kamiframe has no audio yet. Add `libasound2-dev` and
`libpulse-dev` when the audio HAL lands and you want to hear it.

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
ctest --test-dir build --output-on-failure
```

Three tests, all running the real firmware against the headless backend:

- **headless_determinism** hashes every rendered frame and compares against a
  known value. If rendering changed, this fails. When the change was
  deliberate, run `kamiframe-headless --frames 300 --seed 0x5EEDCAFE`, take the
  checksum it prints, and update `KAMIFRAME_GOLDEN_CHECKSUM` in
  `simulator/CMakeLists.txt` in the same commit.
- **headless_dirty_area** fails if the simple demo starts redrawing more of the
  screen than it needs to. That would look perfectly fine on your PC and halve
  the frame rate on hardware.
- **headless_fullscreen** does the same for the full-screen stress mode.

Plus one check that is not a ctest:

```
python3 tools/check_no_heap.py .
```

## The ESP32 build

Not buildable yet. See `ports/esp32/README.md`.
