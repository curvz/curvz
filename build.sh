#!/usr/bin/env bash
# Curvz build script
#
# Usage:
#   ./build.sh                  — production build
#   ./build.sh --diagnostic     — diagnostic build (Scripter window opens at startup)
#
# s186 m2: --diagnostic adds -DCURVZ_DIAGNOSTIC=ON to the cmake configure,
# which compiles in the script-driven test harness (curvz::scripting::
# ScriptListener + ScripterWindow) and routes the Node-tool toolbar
# button through curvz::widgets::ToggleButton so scripts can address
# it as `tool.node`. Production builds drop all of this.
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# ── Parse args ────────────────────────────────────────────────────────────────
CMAKE_OPTS=()
for arg in "$@"; do
    case "$arg" in
        --diagnostic)
            CMAKE_OPTS+=(-DCURVZ_DIAGNOSTIC=ON)
            echo "Build mode: DIAGNOSTIC (Scripter window will open at startup)"
            ;;
        --help|-h)
            echo "Usage: $0 [--diagnostic]"
            echo "  --diagnostic    Build with CURVZ_DIAGNOSTIC=ON (Scripter window)"
            exit 0
            ;;
        *)
            echo "Unknown argument: $arg" >&2
            echo "Try: $0 --help" >&2
            exit 1
            ;;
    esac
done

# ── Install dependencies (Ubuntu/Debian) ──────────────────────────────────────
if command -v apt-get &>/dev/null; then
    sudo apt-get install -y \
        libgtkmm-4.0-dev \
        libcairo2-dev \
        libcairomm-1.16-dev \
        libpangomm-2.48-dev \
        nlohmann-json3-dev \
        libpng-dev \
        libspdlog-dev \
        pkg-config \
        clang \
        cmake
fi

# ── Clean build directory ─────────────────────────────────────────────────────
rm -rf build
mkdir build
cd build

# ── Configure + build ─────────────────────────────────────────────────────────
CXX=clang++ cmake .. "${CMAKE_OPTS[@]}"
cmake --build . -- -j"$(nproc)"

echo ""
echo "Build complete: ./build/curvz"
