#pragma once
#include <string>

struct AppConfig {
    std::string recordingsDir;
    std::string protocolsDir;
    // Drop-in RGB tracking models (.onnx + .json pairs). Defaults to a
    // sibling of the recordings directory named RGB_Models.
    std::string rgbModelsDir;
    // UI layer: "clinical" (reduced Record + Viewer UI, clinical theme) or
    // "research" (the full upstream tab set). Empty means clinical.
    std::string uiMode;

    // Clinical defaults ([defaults] in config.ini), applied to the Record tab
    // at start-up so a clinic can pre-select its usual setup. All optional.
    std::string defaultCameraType;   // "depth" or "rgb"
    std::string defaultDevice;       // "ZED 2i" / "Azure Kinect", or an RGB camera name
    std::string defaultRgbModel;     // pose model name for RGB cameras
    std::string defaultProtocol;     // protocol file name
    std::string defaultClinician;    // pre-filled Clinician field
};

// Default RGB_Models location for a given recordings directory
std::string defaultRgbModelsDir(const std::string& recordingsDir);

// Writable base directory for default recordings/protocols folders:
// next to the exe for a portable build, else the user's Documents folder.
std::string getDefaultDataRoot();

// Read-only directory of models shipped with the app (installed by CMake
// next to the exe, or in Resources inside a macOS .app bundle).
std::string getBundledModelsDir();

// Returns the directory containing the running executable
std::string getExeDir();

// Returns path to config.ini next to the executable
std::string getConfigPath();

// Load config from INI file. Returns true if file exists and was parsed.
bool loadConfig(const std::string& iniPath, AppConfig& out);

// Save config to INI file. Returns true on success.
bool saveConfig(const std::string& iniPath, const AppConfig& cfg);

// Validate that configured directories exist. Returns empty string if OK,
// or a warning message describing what's wrong.
std::string validateConfig(const AppConfig& cfg);
