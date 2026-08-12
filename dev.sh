#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright the Kamiframe contributors.
#
# One command to remember instead of five to memorize.
#
# This wraps CMake, which is the real build system and always will be --
# nothing here replaces it, it just hides the incantations behind words a
# human says out loud. If this script and CMake ever disagree about what
# should happen, CMake is right; file a bug against this script, not your
# understanding of it.
#
# Usage (from the repository root):
#   bash dev.sh build     configure and build everything
#   bash dev.sh run       build if needed, then launch the simulator window
#   bash dev.sh stress    same, but the full-screen stress demo
#   bash dev.sh test      build if needed, then run the automated checks
#   bash dev.sh clean     delete the build output and start fresh
#
# `./dev.sh <command>` also works once you've run `chmod +x dev.sh` one time.
# `bash dev.sh <command>` always works, with no setup, which is why every
# example here uses it.

set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"

# --------------------------------------------------------------------------
# Where build output goes.
#
# WSL2 reaches a Windows drive (/mnt/c, /mnt/d, ...) over a network-style
# protocol, and a build creates thousands of small files -- see BUILDING.md's
# WSL2 section. If the repository itself lives on a Windows drive under
# WSL2, build output goes to a folder on the Linux side instead of ./build,
# which is the single biggest speed difference a WSL2 user can make and the
# one this script exists partly to stop anyone needing to remember by hand.
# Everywhere else (native Linux, macOS, native Windows, WSL2 with the repo
# already on the Linux filesystem), build output is just ./build, next to
# the source, same as BUILDING.md's plain instructions.
# --------------------------------------------------------------------------
BUILD_DIR="build"
if [ -f /proc/version ] && grep -qi microsoft /proc/version 2>/dev/null; then
    case "$(pwd)" in
        /mnt/*)
            BUILD_DIR="${HOME}/.cache/kamiframe-build"
            echo "==> WSL2 + a Windows drive detected: building into"
            echo "    $BUILD_DIR instead of ./build (much faster; see"
            echo "    BUILDING.md's WSL2 section for why)."
            ;;
    esac
fi

nproc_portable() {
    if command -v nproc >/dev/null 2>&1; then
        nproc
    elif command -v sysctl >/dev/null 2>&1; then
        sysctl -n hw.ncpu
    else
        echo 4
    fi
}

do_build() {
    # Always re-run configure, not just on the first build. It is fast when
    # nothing changed, and skipping it is exactly what caused a real stale
    # build here before: files can change without CMake noticing on its own
    # across the WSL2-to-Windows-drive bridge, so re-running it every time
    # costs a couple of seconds and removes an entire category of "I built
    # it but nothing changed" confusion.
    echo "==> Configuring ($BUILD_DIR)"
    cmake -B "$BUILD_DIR" -S . -DCMAKE_BUILD_TYPE=RelWithDebInfo
    echo "==> Building"
    cmake --build "$BUILD_DIR" -j"$(nproc_portable)"
}

bin_path() {
    local name="$1"
    # Single-config generators (the normal case: Makefiles, Ninja) put the
    # binary directly in the build directory. Multi-config generators (MSVC
    # via Visual Studio) put it in a config subfolder instead.
    if [ -x "$BUILD_DIR/$name" ]; then
        echo "$BUILD_DIR/$name"
    elif [ -x "$BUILD_DIR/RelWithDebInfo/$name" ]; then
        echo "$BUILD_DIR/RelWithDebInfo/$name"
    elif [ -x "$BUILD_DIR/RelWithDebInfo/${name}.exe" ]; then
        echo "$BUILD_DIR/RelWithDebInfo/${name}.exe"
    else
        echo ""
    fi
}

do_run() {
    do_build
    local bin
    bin="$(bin_path kamiframe-sim)"
    if [ -z "$bin" ]; then
        echo "error: kamiframe-sim not found under $BUILD_DIR after a build that reported success." >&2
        echo "       Something upstream failed silently; please report this." >&2
        exit 1
    fi
    echo "==> Launching the simulator"
    exec "$bin" "${1:-}"
}

do_test() {
    do_build
    echo "==> Running automated checks"
    ctest --test-dir "$BUILD_DIR" --output-on-failure
    echo "==> Checking core stays heap-free"
    python3 tools/check_no_heap.py
    echo "==> Checking core stays float-free"
    python3 tools/check_no_float.py
    echo "==> Running kf_debug.py's self-test (no hardware needed)"
    python3 tools/kf_debug_selftest.py
    # tools/kf_panel_layout_check.py needs tkinter AND a real display --
    # neither is guaranteed everywhere this script runs (WSL2 with no X
    # server, a minimal CI container, ...), and a missing display is an
    # environment gap, not a layout bug. Best-effort: run it if tkinter
    # imports, skip with a clear reason otherwise. NOT wired into CI
    # (.github/workflows/ci.yml) for the same reason -- those runners have
    # no display and forcing this on would fail on environment grounds
    # every time, not on a real regression.
    if python3 -c "import tkinter" >/dev/null 2>&1; then
        echo "==> Checking kf_panel.py's layout (needs tkinter + a display)"
        python3 tools/kf_panel_layout_check.py || {
            echo "    kf_panel_layout_check.py failed -- see output above." >&2
            echo "    If this is 'couldn't connect to display' rather than" >&2
            echo "    a real layout problem, that's this machine having no" >&2
            echo "    display, not a bug; run it locally with a screen to" >&2
            echo "    confirm before treating this as a real failure." >&2
            exit 1
        }
    else
        echo "==> Skipping kf_panel.py's layout check: this python3 has no" \
             "tkinter (see kf_panel_layout_check.py's own header for how to" \
             "get one)"
    fi
}

do_clean() {
    echo "==> Removing $BUILD_DIR"
    rm -rf "$BUILD_DIR"
}

case "${1:-}" in
    build)  do_build ;;
    run)    do_run ;;
    stress) do_run --stress ;;
    test)   do_test ;;
    clean)  do_clean ;;
    *)
        cat <<'USAGE'
Usage: bash dev.sh <command>

  build    configure and build everything
  run      build if needed, then launch the simulator window
  stress   same, but the full-screen stress demo
  test     build if needed, then run the automated checks
  clean    delete the build output and start fresh

First time on a new machine? Just run: bash dev.sh run
USAGE
        exit 1
        ;;
esac
