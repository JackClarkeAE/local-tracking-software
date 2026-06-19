# Local Tracking Software

Clinical body-tracking suite for movement research using Azure Kinect
and ZED 2i depth cameras, or any standard RGB webcam via drop-in pose models.
Records skeletal joint data during protocol-driven sessions with optional
real-time biofeedback.

> **Licensing:** free for personal, non-commercial, and research use under
> [CC BY-NC-SA 4.0](LICENSE). Commercial use is assessed case-by-case — see
> [License](#license).

## Downloads

Prebuilt installers are published on the
[Releases page](https://github.com/JackClarkeAE/local-tracking-software/releases/latest).

| Download | Platform | Type |
|---|---|---|
| `LocalTrackingSoftware-*-win64.exe` | Windows 10/11 (x64) | Installer (recommended) |
| `LocalTrackingSoftware-*-win64.zip` | Windows 10/11 (x64) | Portable (unzip and run) |
| `LocalTrackingSoftware-*-Darwin.dmg` | macOS (Apple Silicon) | Disk image |
| `Local_Tracking_Software-*.AppImage` | Linux (x86-64) | Portable, runs on any distro |

**Every download works out of the box for RGB-camera tracking, playback,
protocols and analysis — no extra install needed** — and ships with a bundled
pose model (see [Camera support](#camera-support--requirements)). Azure Kinect
and ZED capture have additional requirements, detailed below.

## Camera support & requirements

| Mode | Works in the public downloads? | Requirements |
|---|---|---|
| **RGB camera** (webcam / USB) | Yes, out of the box | Any USB/web camera. Live body tracking via the bundled MoveNet model. |
| **Azure Kinect** | Needs a capture-enabled build | A build compiled with the Azure Kinect SDKs **+** the Azure Kinect Sensor SDK and Body Tracking SDK installed on the machine **+** a Kinect device. Body tracking runs via DirectML (most GPUs, or CPU). |
| **ZED 2i** | Needs a capture-enabled build | A build compiled with the ZED SDK **+** the ZED SDK and CUDA installed **+** an NVIDIA GPU **+** a ZED camera. |

The public release binaries are built on CI, which has no camera SDKs, so they
run in **RGB + playback** mode. Azure Kinect and ZED support must be compiled in
on a machine that has those SDKs (see [Building](#building-the-main-app-qt_version)) —
the vendor runtimes (ZED SDK + CUDA + NVIDIA GPU; or the Azure Kinect SDK) are
required on the end-user machine regardless, as neither vendor permits bundling
them. When the SDKs are absent the app simply runs without that camera.

### Bundled RGB model (included by default)

The downloads include **Google MoveNet (SinglePose Lightning)** — a fast,
single-person, 17-keypoint full-body pose model — so RGB tracking works
immediately with no setup. Select **RGB Camera → Model: MoveNet Lightning** in
the Live tab. The model is redistributed under the Apache License 2.0; see
[`RGB_Models/MoveNet_NOTICE.txt`](RGB_Models/MoveNet_NOTICE.txt) for attribution.
Additional models can be dropped into the `RGB_Models` folder as `.onnx` + `.json`
pairs (template in [`RGB_Models/TEMPLATE.json.example`](RGB_Models/TEMPLATE.json.example)).

## License

Copyright © 2026 Jack Clarke. Released under the **Creative Commons
Attribution-NonCommercial-ShareAlike 4.0 International** license
([CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/)):

- **Free** for personal, non-commercial, and academic/research use, with
  attribution and share-alike.
- **Commercial use** (commercial products, paid services, commercial or
  clinical deployment) is **not** covered by that license and is assessed on a
  **case-by-case basis** — contact the copyright holder to arrange terms.

Bundled third-party components keep their own licenses (Qt 6 — LGPL v3;
ONNX Runtime — MIT; MoveNet — Apache 2.0). Full terms in [`LICENSE`](LICENSE).

This is a research tool and is **not a certified medical device**.

## Repository layout

| Directory | Purpose |
|---|---|
| `qt_version/` | The capture application (Qt 6): live tracking, recording, playback, protocols, biofeedback |
| `RGB_Models/` | Drop-in ONNX pose models (MoveNet bundled) + manifest template |
| `protocols/` | Protocol definitions (JSON timed-event sequences) |
| `scripts/` | Per-platform dependency install scripts |
| `tools/` | OpenEMR FHIR integration probe (planning stage) |
| `packaging/` | Desktop entry + icon for Linux AppImage packaging |

Recorded session data (`recordings/`) is **not** tracked in git.

## Platform support

| | Windows | Linux (Ubuntu 22.04+) | macOS | Jetson (JetPack 6) |
|---|---|---|---|---|
| Build & run | Yes | Yes | Yes | Yes (aarch64) |
| Live capture | ZED / Kinect | ZED / Kinect (NVIDIA GPU) | none (no vendor SDKs) | ZED (Kinect is x86-only) |
| RGB camera capture (webcam/USB) | Yes | Yes | Yes | Yes (ONNX GPU-accelerated) |
| Playback / protocols / analysis | Yes | Yes | Yes | Yes |
| Camera footage recording (MP4) | Yes | Yes | Yes (needs ffmpeg backend) | Yes (Qt from source) |
| Package | NSIS installer + ZIP | AppImage (CI) | DMG | run from build tree |

Qt **6.6 or newer is required** (enforced at configure time). On desktop Linux
the install script sets up official Qt 6.8.3 via aqtinstall — distro packages
on Ubuntu 22.04/24.04 are too old; pass
`-DCMAKE_PREFIX_PATH=$HOME/Qt/6.8.3/gcc_64` when configuring.

**Jetson / JetPack 6 (Ubuntu 22.04, aarch64):** run
`scripts/install_deps_jetson.sh`, which builds Qt 6.8.3 from source (aqtinstall
has no aarch64 desktop Qt) with the ffmpeg media backend, and fetches the
aarch64 ONNX Runtime. Install the ZED SDK's JetPack 6 ARM64 build separately
(CUDA/cuDNN/TensorRT ship with JetPack). Azure Kinect is x86_64-only and is
not available on Jetson.

Cameras are organised into categories in the Live tab: **Markerless** (ZED,
Azure Kinect — skeleton tracking), **RGB Camera** (any connected webcam/USB
camera), and **Markered** (placeholder for future marker-based systems).
Protocols support audio cues (three built-in sounds or a custom WAV).
Recording can capture joint CSVs, camera footage MP4s, or both — all session
files (both cameras, footage, flags) share a single recording timeline.

### Drop-in RGB tracking models

RGB cameras can run pose-estimation models live, making a plain webcam behave
like a markerless tracking camera (skeleton display, joint recording, angles).
Models are **.onnx + .json pairs** dropped into the `RGB_Models` folder
(created next to the recordings directory; configurable via `rgb_models=` in
config.ini):

- `movenet_lightning.onnx` + `.json` — bundled reference model (Google
  MoveNet, 17 COCO keypoints)
- `TEMPLATE.json.example` — fully documented manifest template for adding
  other models; the app reads tensor shape/layout/dtype from the ONNX file
  itself, the manifest supplies normalization, output decoding and keypoint
  names

Requires a build with ONNX Runtime (`brew install onnxruntime`, or set
`ONNXRUNTIME_ROOT` to a prebuilt release). CI builds include it.

> Windows + Azure Kinect note: the K4A Body Tracking SDK deploys its own
> (older) `onnxruntime.dll` next to the exe. Running both Kinect body
> tracking and RGB models in one install needs DLL-collision testing on the
> lab machine before relying on it.

Camera SDKs are optional at both compile time and runtime: the build
auto-detects them, and the app falls back to playback-only mode when they are
absent.

## Building the main app (qt_version)

Install dependencies first — `scripts/install_deps_<platform>` — then:

```bash
cmake -S qt_version -B qt_version/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build qt_version/build
ctest --test-dir qt_version/build          # run the test suite
```

On Windows without Ninja, use the Visual Studio generator instead:

```powershell
cmake -S qt_version -B qt_version\build -G "Visual Studio 17 2022" -A x64
cmake --build qt_version\build --config Release
```

If Qt isn't found automatically, point CMake at it:
`-DCMAKE_PREFIX_PATH=C:\Qt\6.8.3\msvc2022_64` (or wherever Qt lives).

### Camera SDK discovery (Windows/Linux)

The build looks in the standard install locations, overridable via cache
variables or environment variables of the same name:

- `ZED_SDK_ROOT` — default `C:/Program Files (x86)/ZED SDK` (Windows), `/usr/local/zed` (Linux)
- `K4A_ROOT` / `K4ABT_ROOT` — default Azure Kinect install paths on Windows; system prefixes on Linux

## Packaging / installers

```bash
cd qt_version/build && cpack
```

Produces a self-contained NSIS installer + ZIP on Windows (requires NSIS)
and a DMG with bundled Qt frameworks on macOS. On Linux, CI builds an
**AppImage** (Qt and the ffmpeg media backend bundled — runs on any distro,
no install needed); `cpack` can still emit a DEB/tar.gz, but those link
Qt 6.8 which distro repositories don't provide, so prefer the AppImage for
distribution.

macOS note: build and package from a native APFS volume. Packaging from an
exFAT external drive litters AppleDouble (`._*`) files into the bundle and
breaks code signing.

## Continuous integration

`.github/workflows/build.yml` builds and packages all three platforms on
every push (artifacts: installer/ZIP, DEB/tar.gz, DMG). CI runners have no
camera SDKs, so CI binaries are playback-only; capture-enabled Windows builds
are produced on a machine with the SDKs installed.

## Data formats

Each recorded session produces three files in the recordings directory:

- `<ID>.csv` — joints: `time_seconds,body_id,joint_index,joint_name,x,y,z,confidence` (32 joints/frame, ~30 fps)
- `<ID>_flags.csv` — events: `time_seconds,label`
- `<ID>_metadata.json` — patient code, operator, protocol, cameras, biofeedback state, video flag
- `<ID>_video.mp4` — optional camera footage (when "Record Camera Footage" is enabled), reusable for testing other tracking models
