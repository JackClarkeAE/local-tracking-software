#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <vector>
#else
#include <unistd.h>
#include <limits.h>
#endif

#include "config.h"
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>

#include <QStandardPaths>
#include <QDir>

namespace fs = std::filesystem;

// True if we can actually create files in `dir` (portable ZIP next to a
// writable folder), false for read-only install locations such as
// C:\Program Files, a macOS .app bundle, or /usr/bin.
static bool isDirWritable(const std::string& dir) {
    if (dir.empty()) return false;
    std::error_code ec;
    if (!fs::exists(dir, ec)) return false;
    const fs::path probe = fs::path(dir) / ".lts_write_test.tmp";
    std::ofstream f(probe);
    if (!f.is_open()) return false;
    f.close();
    fs::remove(probe, ec);
    return true;
}

std::string getExeDir() {
#ifdef _WIN32
    char buf[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string path(buf);
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buf(size + 1, 0);
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return ".";
    std::string path;
    try {
        path = fs::canonical(buf.data()).string();
    } catch (const fs::filesystem_error&) {
        path = buf.data();
    }
#else
    char buf[PATH_MAX] = {0};
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0) return ".";
    std::string path(buf, static_cast<size_t>(len));
#endif
    auto pos = path.find_last_of("\\/");
    return (pos != std::string::npos) ? path.substr(0, pos) : ".";
}

std::string getConfigPath() {
    // Portable build (Windows/Linux): keep config next to the exe when that
    // folder is writable. macOS is always a .app bundle, never portable.
#ifndef __APPLE__
    if (isDirWritable(getExeDir()))
        return getExeDir() + "/config.ini";
#endif
    // Installed build (Program Files / .app / /usr/bin are read-only):
    // use the per-user config location.
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (dir.isEmpty()) return getExeDir() + "/config.ini";
    QDir().mkpath(dir);
    return QDir(dir).filePath("config.ini").toStdString();
}

std::string getDefaultDataRoot() {
    // Portable build (Windows/Linux): data sits next to the exe when writable.
#ifndef __APPLE__
    if (isDirWritable(getExeDir()))
        return getExeDir();
#endif
    // Installed build: the user's Documents folder.
    QString docs = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (docs.isEmpty()) docs = QDir::homePath();
    const QString root = QDir(docs).filePath("Local Tracking Software");
    QDir().mkpath(root);
    return root.toStdString();
}

std::string getBundledModelsDir() {
    // Read-only models shipped alongside the app (installed by CMake).
#ifdef __APPLE__
    return getExeDir() + "/../Resources/RGB_Models";
#else
    return getExeDir() + "/RGB_Models";
#endif
}

std::string defaultRgbModelsDir(const std::string& recordingsDir) {
    if (recordingsDir.empty()) return "";
    // Strip trailing separators so parent_path() gives the actual parent
    std::string trimmed = recordingsDir;
    while (trimmed.size() > 1 && (trimmed.back() == '/' || trimmed.back() == '\\'))
        trimmed.pop_back();
    const fs::path parent = fs::path(trimmed).parent_path();
    return (parent.empty() ? fs::path("RGB_Models") : parent / "RGB_Models").string();
}

static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

bool loadConfig(const std::string& iniPath, AppConfig& out) {
    std::ifstream f(iniPath);
    if (!f.is_open()) return false;

    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[')
            continue;

        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));

        if (key == "recordings") out.recordingsDir = val;
        else if (key == "protocols") out.protocolsDir = val;
        else if (key == "rgb_models") out.rgbModelsDir = val;
    }

    return !out.recordingsDir.empty() || !out.protocolsDir.empty();
}

bool saveConfig(const std::string& iniPath, const AppConfig& cfg) {
    std::ofstream f(iniPath);
    if (!f.is_open()) return false;

    f << "[paths]\n";
    f << "recordings=" << cfg.recordingsDir << "\n";
    f << "protocols=" << cfg.protocolsDir << "\n";
    if (!cfg.rgbModelsDir.empty())
        f << "rgb_models=" << cfg.rgbModelsDir << "\n";

    return f.good();
}

std::string validateConfig(const AppConfig& cfg) {
    std::string warning;

    if (cfg.recordingsDir.empty()) {
        warning += "Recordings directory is not set.\n";
    } else if (!fs::exists(cfg.recordingsDir)) {
        warning += "Recordings directory not found: " + cfg.recordingsDir + "\n";
    }

    if (cfg.protocolsDir.empty()) {
        warning += "Protocols directory is not set.\n";
    } else if (!fs::exists(cfg.protocolsDir)) {
        // Auto-create protocols dir if recordings dir is valid
        if (warning.empty()) {
            fs::create_directories(cfg.protocolsDir);
            if (!fs::exists(cfg.protocolsDir)) {
                warning += "Failed to create protocols directory: " + cfg.protocolsDir + "\n";
            }
        } else {
            warning += "Protocols directory not found: " + cfg.protocolsDir + "\n";
        }
    }

    return warning;
}
