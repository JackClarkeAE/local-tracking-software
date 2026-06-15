# Install build dependencies for Local Tracking Software (Qt) on Windows.
# Run from an elevated PowerShell:  powershell -ExecutionPolicy Bypass -File scripts\install_deps_windows.ps1
#
# Installs: CMake, Ninja, Qt 6 (via aqtinstall). Visual Studio Build Tools and
# the optional camera SDKs print guidance if missing — the build auto-detects
# them and falls back to playback-only mode when absent.

$ErrorActionPreference = "Stop"

function Have($cmd) { return [bool](Get-Command $cmd -ErrorAction SilentlyContinue) }

Write-Host "==> Checking Visual Studio Build Tools..."
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere) -or -not (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)) {
    Write-Host "  MSVC not found. Install Visual Studio 2022 Build Tools (C++ workload):"
    Write-Host "    winget install Microsoft.VisualStudio.2022.BuildTools --override `"--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended`""
} else {
    Write-Host "  MSVC found."
}

Write-Host "==> Installing CMake + Ninja..."
if (-not (Have cmake)) { winget install --accept-source-agreements --accept-package-agreements Kitware.CMake }
if (-not (Have ninja)) { winget install --accept-source-agreements --accept-package-agreements Ninja-build.Ninja }

Write-Host "==> Installing Qt 6 via aqtinstall..."
if (-not (Have python)) { winget install --accept-source-agreements --accept-package-agreements Python.Python.3.12 }
python -m pip install --quiet aqtinstall
$qtRoot = "C:\Qt"
if (-not (Test-Path "$qtRoot\6.8.3\msvc2022_64")) {
    python -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64 --outputdir $qtRoot
}
Write-Host "  Qt installed at $qtRoot\6.8.3\msvc2022_64"
Write-Host "  Configure with: cmake -S qt_version -B qt_version\build -DCMAKE_PREFIX_PATH=$qtRoot\6.8.3\msvc2022_64"

Write-Host ""
Write-Host "-------------------------------------------------------------------"
Write-Host "OPTIONAL camera SDKs (capture support):"
Write-Host ""
Write-Host "Azure Kinect (default install paths are auto-detected):"
Write-Host "  Sensor SDK 1.4.x:        https://learn.microsoft.com/azure/kinect-dk/sensor-sdk-download"
Write-Host "  Body Tracking SDK 1.1.x: https://learn.microsoft.com/azure/kinect-dk/body-sdk-download"
Write-Host ""
Write-Host "ZED SDK (requires NVIDIA GPU + CUDA; installer can run silently with /S):"
Write-Host "  https://www.stereolabs.com/developers/release"
Write-Host ""
Write-Host "NSIS (only needed to build the installer with cpack):"
Write-Host "  winget install NSIS.NSIS"
Write-Host "-------------------------------------------------------------------"
