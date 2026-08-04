# ADR 0003: SDL3 for the desktop backend

**Status:** Accepted, 2026-08-04
**Reversal cost:** Low. Roughly 50 lines in two files.

## Decision

SDL3, pinned to `release-3.4.8`.

As of August 2026 SDL3 is the actively developed line, shipping regular
releases; SDL2 is in maintenance and `sdl2-compat` exists to run SDL2 apps on
SDL3. Starting a multi-year project on the outgoing version makes no sense.

Considered and rejected: SDL2; GLFW plus a hand-rolled texture upload
(smaller, but no audio, gamepad or touch, all of which are wanted later);
sokol_app plus sokol_gfx (genuinely tempting, tiny, excellent WASM story,
rejected because SDL supplies audio and gamepad in the same dependency and
has far more prior art to copy); raylib (opinionated about the game loop in
ways that fight ADR 0007); SFML; minifb.

## Two implementation rules that matter more than the version

**The texture is `SDL_PIXELFORMAT_RGB565`.** The exact bytes the ST7789 would
receive, uploaded with no CPU-side conversion. A conversion step would be
desktop-only code with no device counterpart, and would hide pixel-format
bugs that hardware would show immediately.

**Nearest-neighbour scaling at integer factors.** Linear filtering would make
sprites look better here than they can ever look on a 240x320 panel, which is
the same category of lie as running at 400fps.

## This does not constrain a WASM build

Emscripten has a working SDL3 port. The thing that *would* have constrained
WASM is not the graphics library at all, it is who owns the frame loop. See
ADR 0007.

## This decision is a test of ADR 0004

If replacing SDL is ever expensive, something has leaked across the HAL
boundary and needs pushing back. Keeping it cheap is the check.
