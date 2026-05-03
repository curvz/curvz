#!/usr/bin/env bash
# Curvz build script
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

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
CXX=clang++ cmake ..
cmake --build . -- -j"$(nproc)"

echo ""
echo "Build complete: ./build/curvz"
