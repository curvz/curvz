#!/usr/bin/env bash
# Curvz build script
#
# Usage:
#   ./build.sh        — production build
#
# s219 m1: the --diagnostic flag and CURVZ_DIAGNOSTIC option are gone.
# The Scripter window and all scripting TUs compile unconditionally;
# whether the Scripter opens at runtime is the user preference
# AppPreferences::scripter_enabled, surfaced as Developer ▸ Scripting
# in the hamburger menu and Application ▸ Developer in the inspector.
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# ── Parse args ────────────────────────────────────────────────────────────────
for arg in "$@"; do
    case "$arg" in
        --help|-h)
            echo "Usage: $0"
            echo ""
            echo "No build flags — Scripter is a runtime preference now"
            echo "(toggle in Developer > Scripting from the hamburger menu, or"
            echo "in the Application > Developer subsection of the inspector)."
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
CXX=clang++ cmake ..
cmake --build . -- -j"$(nproc)"

echo ""
echo "Build complete: ./build/curvz"
