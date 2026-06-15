#pragma once
#include <string>

struct AppConfig {
    std::string recordingsDir;
    std::string protocolsDir;
    // Drop-in RGB tracking models (.onnx + .json pairs). Defaults to a
    // sibling of the recordings directory named RGB_Models.
    std::string rgbModelsDir;
};

// Default RGB_Models location for a given recordings directory
std::string defaultRgbModelsDir(const std::string& recordingsDir);

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
