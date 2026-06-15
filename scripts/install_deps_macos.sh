#!/bin/bash
# Install build dependencies for Local Tracking Software (Qt) on macOS.
# Camera capture is not available on macOS (no vendor SDKs) — macOS builds
# run in playback/analysis mode.
set -euo pipefail

if ! command -v brew >/dev/null; then
    echo "Homebrew is required: https://brew.sh"
    exit 1
fi

echo "==> Installing CMake, Ninja, Qt 6 and ONNX Runtime..."
brew install cmake ninja qt onnxruntime

echo
echo "Done. Build with:"
echo "  cmake -S qt_version -B qt_version/build -G Ninja -DCMAKE_BUILD_TYPE=Release"
echo "  cmake --build qt_version/build"
echo "Package a DMG with:"
echo "  (cd qt_version/build && cpack)"
echo
echo "NOTE: package from a native APFS volume (not an exFAT external drive),"
echo "otherwise AppleDouble files break code signing."
