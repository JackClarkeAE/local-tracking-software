#!/bin/bash
# Build dependencies for Local Tracking Software (Qt) on an NVIDIA Jetson
# running JetPack 6 (Ubuntu 22.04, aarch64 / L4T).
#
# Why this is separate from install_deps_linux.sh:
#   - Jetson is aarch64; the desktop script's downloads are x86_64.
#   - aqtinstall has no aarch64 Linux desktop Qt, and Ubuntu 22.04's apt Qt
#     (6.4) predates QVideoFrameInput (6.6+) which the MP4 footage recorder
#     needs — so Qt is built from source here.
#   - JetPack already provides CUDA/cuDNN/TensorRT, which the ZED SDK and
#     (optionally) GPU ONNX Runtime use.
#
# Camera support on Jetson: ZED (first-class) and RGB webcams + ONNX models.
# Azure Kinect is x86_64-only and is not built here.
#
# NOTE: this script has been authored from the documented build steps but
# has NOT been run on a Jetson by the author — expect to debug device
# specifics (driver/L4T versions) on first run. It is idempotent: completed
# steps are skipped on re-run.
set -euo pipefail

QT_VERSION=6.8.3
QT_MINOR=6.8
ORT_VERSION=1.20.1
QT_PREFIX="$HOME/Qt/${QT_VERSION}/gcc_arm64"
JOBS=$(nproc)

arch=$(uname -m)
if [ "$arch" != "aarch64" ]; then
    echo "This script is for Jetson aarch64. Detected: $arch."
    echo "For a desktop x86_64 Linux build use scripts/install_deps_linux.sh."
    exit 1
fi

# NOTE on FFmpeg: Qt 6.8's media backend wants the FULL libav* -dev set
# (avcodec/avformat/avutil/swscale/swresample/avfilter/avdevice) — missing any
# makes Qt report "FFmpeg not found". Ubuntu 22.04 ships FFmpeg 4.4; if the
# qtmultimedia build then fails to COMPILE with AVChannelLayout/ch_layout
# errors, 4.4 is too old and you need FFmpeg 6.x (see the troubleshooting note
# at the end of this script).
echo "==> Installing toolchain + Qt build dependencies..."
sudo apt-get update
sudo apt-get install -y \
    build-essential cmake ninja-build git wget python3 \
    libgl1-mesa-dev libglu1-mesa-dev libegl1-mesa-dev \
    libxkbcommon-dev libxkbcommon-x11-dev libxcb-cursor0 \
    '^libxcb.*-dev' libx11-xcb-dev libxrender-dev libxi-dev \
    libfontconfig1-dev libfreetype6-dev \
    libavcodec-dev libavformat-dev libavutil-dev libswscale-dev \
    libswresample-dev libavfilter-dev libavdevice-dev libva-dev libpulse-dev

# ---------------------------------------------------------------------------
# Qt 6.8.3 from source — only the modules the app needs:
#   qtbase  ->  qtshadertools  ->  qtmultimedia (ffmpeg backend for MP4)
#
# Per-module idempotency: each module is (re)built only if its install marker
# is absent. So if a previous run got qtbase in but failed on qtmultimedia
# (e.g. missing FFmpeg dev libs), just re-running this script resumes at
# qtmultimedia rather than skipping everything because qmake exists.
# ---------------------------------------------------------------------------
SRC="$HOME/qt-src-${QT_VERSION}"
mkdir -p "$SRC" && cd "$SRC"
base="https://download.qt.io/official_releases/qt/${QT_MINOR}/${QT_VERSION}/submodules"
for mod in qtbase qtshadertools qtmultimedia; do
    if [ ! -d "$mod" ]; then
        echo "==> Fetching $mod source..."
        wget -q "${base}/${mod}-everywhere-src-${QT_VERSION}.tar.xz"
        tar xf "${mod}-everywhere-src-${QT_VERSION}.tar.xz"
        mv "${mod}-everywhere-src-${QT_VERSION}" "$mod"
    fi
done

# qtbase — provides qmake + qt-configure-module for the rest
if [ ! -x "${QT_PREFIX}/bin/qmake" ]; then
    echo "==> Building qtbase (this takes a while)..."
    cd "$SRC/qtbase"
    cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$QT_PREFIX" -DQT_BUILD_EXAMPLES=OFF -DQT_BUILD_TESTS=OFF
    cmake --build build --parallel "$JOBS"
    cmake --install build
else
    echo "==> qtbase already installed (skipping)"
fi

# qtshadertools — build dependency of qtmultimedia
if [ ! -e "${QT_PREFIX}/lib/libQt6ShaderTools.so" ] && \
   [ ! -e "${QT_PREFIX}/metatypes/qt6shadertools_relwithdebinfo_metatypes.json" ]; then
    echo "==> Building qtshadertools..."
    cd "$SRC/qtshadertools" && rm -f CMakeCache.txt
    "$QT_PREFIX/bin/qt-configure-module" . -- -DCMAKE_BUILD_TYPE=Release
    cmake --build . --parallel "$JOBS"
    cmake --install .
else
    echo "==> qtshadertools already installed (skipping)"
fi

# qtmultimedia — provides QMediaRecorder/QVideoFrameInput + the ffmpeg backend
if [ ! -e "${QT_PREFIX}/lib/libQt6Multimedia.so" ]; then
    echo "==> Building qtmultimedia..."
    cd "$SRC/qtmultimedia" && rm -f CMakeCache.txt
    "$QT_PREFIX/bin/qt-configure-module" . -- -DCMAKE_BUILD_TYPE=Release
    cmake --build . --parallel "$JOBS"
    cmake --install .
else
    echo "==> qtmultimedia already installed (skipping)"
fi
echo "==> Qt ready at $QT_PREFIX"

# Confirm the ffmpeg media backend was built (required for MP4 recording)
if ls "$QT_PREFIX"/plugins/multimedia/*ffmpeg* >/dev/null 2>&1; then
    echo "    ffmpeg media backend: present (MP4 recording enabled)"
else
    echo "    WARNING: ffmpeg media backend NOT built — MP4 recording will be"
    echo "             unavailable. See the TROUBLESHOOTING notes at the end."
fi

# ---------------------------------------------------------------------------
# ONNX Runtime (aarch64) — drop-in RGB tracking models
# CPU build is plenty for MoveNet at webcam resolution. For TensorRT/GPU
# acceleration, swap in NVIDIA's Jetson ONNX Runtime build (see Jetson Zoo).
# ---------------------------------------------------------------------------
ORT_DIR="$HOME/onnxruntime-linux-aarch64-${ORT_VERSION}"
if [ ! -d "$ORT_DIR" ]; then
    echo "==> Installing ONNX Runtime ${ORT_VERSION} (aarch64)..."
    wget -q "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/onnxruntime-linux-aarch64-${ORT_VERSION}.tgz" -O /tmp/ort.tgz
    tar xf /tmp/ort.tgz -C "$HOME"
fi

cat <<EOF

============================================================================
Toolchain + Qt ${QT_VERSION} + ONNX Runtime ready. Build with:

  cmake -S qt_version -B qt_version/build -G Ninja -DCMAKE_BUILD_TYPE=Release \\
        -DCMAKE_PREFIX_PATH="${QT_PREFIX}" \\
        -DONNXRUNTIME_ROOT="${ORT_DIR}"
  cmake --build qt_version/build

----------------------------------------------------------------------------
ZED SDK (required for ZED tracking — install separately, license-gated):
  Download the JetPack 6 ARM64 installer from
    https://www.stereolabs.com/developers/release
  then run it (it installs to /usr/local/zed, which the build auto-detects):
    chmod +x ZED_SDK_Tegra_L4T*.run && ./ZED_SDK_Tegra_L4T*.run

  CUDA/cuDNN/TensorRT come with JetPack — no separate install needed.
----------------------------------------------------------------------------
TROUBLESHOOTING the Qt build:

"FFmpeg not found" during qtmultimedia configure:
  Missing libav* -dev packages. This script now installs the full set; if you
  hit it on an older run, install them and rebuild just qtmultimedia:
    sudo apt-get install -y libavfilter-dev libavdevice-dev
    cd "\$HOME/qt-src-${QT_VERSION}/qtmultimedia" && rm -f CMakeCache.txt
    "${QT_PREFIX}/bin/qt-configure-module" . -- -DCMAKE_BUILD_TYPE=Release
    cmake --build . --parallel 2 && cmake --install .

qtmultimedia COMPILE errors mentioning AVChannelLayout / ch_layout:
  Ubuntu 22.04's FFmpeg 4.4 is too old for Qt 6.8. Options: build FFmpeg 6.x
  from source, or rebuild qtmultimedia against the GStreamer backend (native +
  HW-accelerated on Jetson): sudo apt-get install -y libgstreamer1.0-dev \\
    libgstreamer-plugins-base1.0-dev libgstreamer-plugins-bad1.0-dev

Build "Killed" with no error: out of RAM. Use --parallel 2 and add swap.
============================================================================
EOF
