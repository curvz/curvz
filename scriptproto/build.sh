#!/usr/bin/env bash
# scriptproto build script
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# ── Install dependencies (Ubuntu/Debian) ──────────────────────────────────────
# Sandbox depends only on gtkmm-4.0 — no json, spdlog, cairo, pango, png.
# Standalone-by-design: cheap to thrash on, no main-app blast radius.
if command -v apt-get &>/dev/null; then
    sudo apt-get install -y \
        libgtkmm-4.0-dev \
        pkg-config \
        clang \
        cmake
fi

# ── Clean build directory ─────────────────────────────────────────────────────
rm -rf build
mkdir build
cd build

# ── Configure + build ─────────────────────────────────────────────────────────
CXX=clang++ cmake ..
cmake --build . -- -j"$(nproc)"

echo ""
echo "Build complete: ./build/scriptproto"
echo ""
echo "Run the m1 reference test:"
echo "  ./build/scriptproto --script scripts/01_toggle_click.curvzs"
