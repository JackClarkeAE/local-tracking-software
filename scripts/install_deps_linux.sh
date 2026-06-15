#!/bin/bash
# Install build dependencies for Local Tracking Software (Qt) on Ubuntu 22.04+.
#
# Base toolchain + Qt are installed automatically. The camera SDKs are
# optional — the build auto-detects them and falls back to playback-only mode.
set -euo pipefail

QT_VERSION=6.8.3

echo "==> Installing toolchain..."
sudo apt-get update
sudo apt-get install -y \
    build-essential cmake ninja-build git python3-pip \
    libgl1-mesa-dev libglu1-mesa-dev libxkbcommon-dev \
    libxcb-cursor0 libxkbcommon-x11-0

# Distro Qt packages are too old (Ubuntu 22.04 ships Qt 6.2; camera footage
# recording needs 6.6+), so install official Qt via aqtinstall — the same
# version CI builds against.
echo "==> Installing official Qt ${QT_VERSION} via aqtinstall..."
python3 -m pip install --user --quiet aqtinstall 2>/dev/null \
    || python3 -m pip install --user --quiet --break-system-packages aqtinstall
if [ ! -d "$HOME/Qt/${QT_VERSION}/gcc_64" ]; then
    python3 -m aqt install-qt linux desktop "${QT_VERSION}" -m qtmultimedia --outputdir "$HOME/Qt"
fi

# ONNX Runtime — enables drop-in RGB tracking models
ORT_VERSION=1.20.1
if [ ! -d "$HOME/onnxruntime-linux-x64-${ORT_VERSION}" ]; then
    echo "==> Installing ONNX Runtime ${ORT_VERSION}..."
    wget -q "https://github.com/microsoft/onnxruntime/releases/download/v${ORT_VERSION}/onnxruntime-linux-x64-${ORT_VERSION}.tgz" -O /tmp/ort.tgz
    tar xf /tmp/ort.tgz -C "$HOME"
fi

cat <<EOF

Dependencies installed (Qt ${QT_VERSION} at \$HOME/Qt/${QT_VERSION}/gcc_64). Build with:
  cmake -S qt_version -B qt_version/build -G Ninja -DCMAKE_BUILD_TYPE=Release \\
        -DCMAKE_PREFIX_PATH="\$HOME/Qt/${QT_VERSION}/gcc_64" \\
        -DONNXRUNTIME_ROOT="\$HOME/onnxruntime-linux-x64-${ORT_VERSION}"
  cmake --build qt_version/build

-------------------------------------------------------------------------
OPTIONAL camera SDKs (capture support — requires an NVIDIA GPU):

ZED SDK (Stereolabs):
  1. Install CUDA:        sudo apt-get install -y nvidia-cuda-toolkit
  2. Download the ZED SDK .run installer for your Ubuntu/CUDA version:
       https://www.stereolabs.com/developers/release
  3. Run it silently:     bash ZED_SDK_*.run -- silent
  It installs to /usr/local/zed, which the build finds automatically.

Azure Kinect SDK (Microsoft, packages target Ubuntu 18.04/20.04 — on
newer Ubuntu install the .debs manually):
  1. Sensor SDK + Body Tracking SDK packages:
       https://learn.microsoft.com/azure/kinect-dk/sensor-sdk-download
     Packages needed: libk4a1.4, libk4a1.4-dev, libk4abt1.1, libk4abt1.1-dev, k4a-tools
  2. Body tracking on Linux requires CUDA (NVIDIA GPU mandatory).
  3. Allow device access without root:
       sudo cp scripts/99-k4a.rules /etc/udev/rules.d/ 2>/dev/null || true
-------------------------------------------------------------------------
EOF
