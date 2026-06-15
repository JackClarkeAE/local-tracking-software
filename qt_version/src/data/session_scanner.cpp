#include "session_scanner.h"
#include "../biofeedback/biofeedback_engine.h"
#include "../recording/playback.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <map>
#include <cstring>
#include <sstream>

namespace fs = std::filesystem;

// ============================================================
// Helpers
// ============================================================

// Extract session prefix by stripping known suffixes
static std::string extractPrefix(const std::string& filename) {
    std::string stem = filename;
    // Remove extension
    auto dot = stem.rfind('.');
    if (dot != std::string::npos) stem = stem.substr(0, dot);

    // Strip known suffixes
    static const char* suffixes[] = {
        "_flags", "_metadata",
        "_biofeedback_avatar", "_biofeedback_angles",
        "_cam1", "_cam2"
    };
    for (auto& suf : suffixes) {
        size_t slen = strlen(suf);
        if (stem.size() > slen && stem.substr(stem.size() - slen) == suf) {
            stem = stem.substr(0, stem.size() - slen);
            break;
        }
    }
    return stem;
}

// Extract display name (everything before the _YYYYMMDD_HHMMSS timestamp)
static std::string extractDisplayName(const std::string& prefix) {
    // Look for _YYYYMMDD_HHMMSS at the end (16 chars: _8digits_6digits)
    if (prefix.size() >= 16) {
        size_t tsStart = prefix.size() - 16;
        if (prefix[tsStart] == '_' && prefix[tsStart + 9] == '_') {
            std::string name = prefix.substr(0, tsStart);
            if (name.empty()) name = "session";
            return name;
        }
    }
    return prefix;
}

static std::string extractJsonString(const std::string& json, const std::string& key) {
    auto pos = json.find("\"" + key + "\"");
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos);
    if (pos == std::string::npos) return "";
    auto start = json.find('"', pos + 1);
    if (start == std::string::npos) return "";
    start++;
    std::string result;
    for (size_t i = start; i < json.size(); i++) {
        if (json[i] == '\\' && i + 1 < json.size()) {
            if (json[i+1] == '"') { result += '"'; i++; }
            else if (json[i+1] == 'n') { result += '\n'; i++; }
            else if (json[i+1] == '\\') { result += '\\'; i++; }
            else result += json[i];
        } else if (json[i] == '"') break;
        else result += json[i];
    }
    return result;
}

static bool extractJsonBool(const std::string& json, const std::string& key) {
    auto pos = json.find("\"" + key + "\"");
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos);
    if (pos == std::string::npos) return false;
    auto valStart = json.find_first_not_of(" \t", pos + 1);
    return json.substr(valStart, 4) == "true";
}

// ============================================================
// SessionScanner
// ============================================================

void SessionScanner::scan(const std::string& recordingsDir) {
    sessions_.clear();

    if (!fs::exists(recordingsDir)) return;

    // Group files by prefix
    std::map<std::string, SessionInfo> sessionMap;

    for (auto& entry : fs::directory_iterator(recordingsDir)) {
        if (!entry.is_regular_file()) continue;
        std::string filename = entry.path().filename().string();
        std::string fullPath = entry.path().string();
        std::string prefix = extractPrefix(filename);

        auto& session = sessionMap[prefix];
        session.prefix = prefix;
        session.displayName = extractDisplayName(prefix);
        session.totalFileSize += entry.file_size();

        // Classify file
        if (filename.find("_metadata.json") != std::string::npos) {
            session.metadataPath = fullPath;
        } else if (filename.find("_flags.csv") != std::string::npos) {
            session.flagsPath = fullPath;
        } else if (filename.find("_biofeedback_avatar.csv") != std::string::npos) {
            session.biofeedbackAvatarPath = fullPath;
        } else if (filename.find("_biofeedback_angles.csv") != std::string::npos) {
            session.biofeedbackAnglesPath = fullPath;
        } else if (filename.find(".csv") != std::string::npos) {
            session.csvPaths.push_back(fullPath);
        }
    }

    // Parse metadata and compute quick stats for each session
    for (auto& [prefix, session] : sessionMap) {
        if (!session.metadataPath.empty()) parseMetadata(session);
        computeQuickStats(session);
        sessions_.push_back(std::move(session));
    }

    // Sort by date (newest first)
    std::sort(sessions_.begin(), sessions_.end(), [](const SessionInfo& a, const SessionInfo& b) {
        return a.date > b.date;
    });

    std::cout << "SessionScanner: found " << sessions_.size() << " sessions" << std::endl;
}

void SessionScanner::parseMetadata(SessionInfo& session) {
    std::ifstream f(session.metadataPath);
    if (!f.is_open()) return;
    std::string json((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    session.patientId = extractJsonString(json, "patient_id");
    session.operatorName = extractJsonString(json, "operator");
    session.notes = extractJsonString(json, "notes");
    session.date = extractJsonString(json, "date");
    session.protocol = extractJsonString(json, "protocol");
    session.biofeedbackActive = extractJsonBool(json, "biofeedback_active");

    // Parse cameras array — extract "name" fields
    auto camPos = json.find("\"cameras\"");
    if (camPos != std::string::npos) {
        auto arrStart = json.find('[', camPos);
        auto arrEnd = json.find(']', arrStart);
        if (arrStart != std::string::npos && arrEnd != std::string::npos) {
            std::string arr = json.substr(arrStart, arrEnd - arrStart + 1);
            size_t pos = 0;
            while (true) {
                auto namePos = arr.find("\"name\"", pos);
                if (namePos == std::string::npos) break;
                std::string name = extractJsonString(arr.substr(namePos), "name");
                if (!name.empty()) session.cameraNames.push_back(name);
                pos = namePos + 6;
            }
        }
    }
    session.cameraCount = (int)session.cameraNames.size();
    if (session.cameraCount == 0) session.cameraCount = (int)session.csvPaths.size();
}

void SessionScanner::computeQuickStats(SessionInfo& session) {
    // Duration from last line of primary CSV (fast — reads 4KB from end)
    if (!session.csvPaths.empty()) {
        session.durationSeconds = getLastTimestamp(session.csvPaths[0]);
        // Estimate frame count from file size (~60 bytes per CSV line, JOINT_COUNT lines per frame)
        auto fsize = fs::file_size(session.csvPaths[0]);
        int estimatedLines = (int)(fsize / 60);
        session.frameCount = estimatedLines / JOINT_COUNT;
    }

    // Flag count (flags files are tiny, safe to count lines)
    if (!session.flagsPath.empty()) {
        int lines = countLines(session.flagsPath);
        session.flagCount = (lines > 1) ? lines - 1 : 0;
    }

    if (session.cameraCount == 0 && !session.csvPaths.empty())
        session.cameraCount = (int)session.csvPaths.size();
}

double SessionScanner::getLastTimestamp(const std::string& csvPath) {
    std::ifstream f(csvPath, std::ios::ate);
    if (!f.is_open()) return 0.0;

    // Seek back from end to find last line
    auto fileSize = f.tellg();
    if (fileSize <= 0) return 0.0;
    int seekBack = 4096;
    if (seekBack > (int)fileSize) seekBack = (int)fileSize;
    f.seekg(-seekBack, std::ios::end);

    std::string chunk;
    chunk.resize(seekBack);
    f.read(&chunk[0], seekBack);

    // Find last newline, then parse the line before it
    auto lastNl = chunk.rfind('\n');
    if (lastNl == std::string::npos) return 0.0;
    // Find the newline before the last one
    auto prevNl = chunk.rfind('\n', lastNl - 1);
    std::string lastLine;
    if (prevNl != std::string::npos)
        lastLine = chunk.substr(prevNl + 1, lastNl - prevNl - 1);
    else
        lastLine = chunk.substr(0, lastNl);

    if (lastLine.empty()) return 0.0;
    auto comma = lastLine.find(',');
    if (comma == std::string::npos) return 0.0;
    try { return std::stod(lastLine.substr(0, comma)); } catch (...) { return 0.0; }
}

int SessionScanner::countLines(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return 0;
    int count = 0;
    std::string line;
    while (std::getline(f, line)) count++;
    return count;
}

void SessionScanner::computeDetailedStats(int sessionIndex) {
    if (computing_.load()) return;
    if (sessionIndex < 0 || sessionIndex >= (int)sessions_.size()) return;

    computing_.store(true);
    cancelCompute_.store(false);
    computeProgress_.store(0.0f);

    if (computeThread_.joinable()) computeThread_.join();

    computeThread_ = std::thread([this, sessionIndex]() {
        auto& session = sessions_[sessionIndex];
        if (session.csvPaths.empty()) {
            computing_.store(false);
            return;
        }

        PlaybackManager pm;
        pm.loadCSV(session.csvPaths[0]);
        if (!pm.isLoaded()) { computing_.store(false); return; }

        int totalFrames = pm.frameCount();
        float confSum = 0.0f;
        int confCount = 0;
        int trackedFrames = 0;

        // Angle tracking
        std::vector<float> angleSums(biomechAngleCount(), 0.0f);
        std::vector<float> angleMins(biomechAngleCount(), 999.0f);
        std::vector<float> angleMaxs(biomechAngleCount(), -999.0f);
        std::vector<int> angleCounts(biomechAngleCount(), 0);

        for (int i = 0; i < totalFrames; i++) {
            if (cancelCompute_.load()) break;
            if (i % 50 == 0) computeProgress_.store((float)i / totalFrames);

            const PlaybackFrame* pf = pm.getFrame(i);
            if (!pf) continue;

            if (!pf->bodies.empty()) trackedFrames++;

            for (auto& body : pf->bodies) {
                for (int j = 0; j < JOINT_COUNT; j++) {
                    if (body.joints[j].confidence > 0.0f) {
                        confSum += body.joints[j].confidence;
                        confCount++;
                    }
                }
            }

            // Compute angles
            FrameData fd;
            fd.bodies = pf->bodies;
            auto angles = BiofeedbackEngine::measureAngles(fd);
            for (auto& m : angles) {
                int idx = (int)m.angle;
                if (idx < 0 || idx >= biomechAngleCount()) continue;
                angleSums[idx] += m.rawDegrees;
                if (m.rawDegrees < angleMins[idx]) angleMins[idx] = m.rawDegrees;
                if (m.rawDegrees > angleMaxs[idx]) angleMaxs[idx] = m.rawDegrees;
                angleCounts[idx]++;
            }
        }

        session.avgConfidence = (confCount > 0) ? confSum / confCount : 0.0f;
        session.trackedFramePercent = (totalFrames > 0) ? 100.0f * trackedFrames / totalFrames : 0.0f;

        session.angleSummaries.clear();
        for (int i = 0; i < biomechAngleCount(); i++) {
            SessionInfo::AngleSummary as;
            as.name = biomechAngleName(i);
            if (angleCounts[i] > 0) {
                as.minDeg = angleMins[i];
                as.maxDeg = angleMaxs[i];
                as.meanDeg = angleSums[i] / angleCounts[i];
            }
            session.angleSummaries.push_back(as);
        }

        session.detailedStatsLoaded = true;
        computeProgress_.store(1.0f);
        computing_.store(false);
    });
}

void SessionScanner::cancelBackgroundWork() {
    cancelCompute_.store(true);
    if (computeThread_.joinable()) computeThread_.join();
    computing_.store(false);
}

std::vector<int> SessionScanner::selectedIndices() const {
    std::vector<int> result;
    for (int i = 0; i < (int)sessions_.size(); i++)
        if (sessions_[i].selected) result.push_back(i);
    return result;
}

void SessionScanner::selectAll() {
    for (auto& s : sessions_) s.selected = true;
}

void SessionScanner::deselectAll() {
    for (auto& s : sessions_) s.selected = false;
}

std::string SessionScanner::renameSession(int sessionIndex, const std::string& newName,
                                            const std::string& recordingsDir) {
    if (sessionIndex < 0 || sessionIndex >= (int)sessions_.size())
        return "Invalid session index.";
    if (newName.empty())
        return "New name cannot be empty.";

    // Validate name — no path separators or special chars
    for (char c : newName) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            return "Name contains invalid characters.";
    }

    auto& session = sessions_[sessionIndex];

    // Extract the timestamp portion from the prefix (last 16 chars: _YYYYMMDD_HHMMSS)
    std::string oldPrefix = session.prefix;
    std::string timestamp;
    if (oldPrefix.size() >= 16) {
        timestamp = oldPrefix.substr(oldPrefix.size() - 16);
    } else {
        return "Cannot determine timestamp from session prefix.";
    }

    std::string newPrefix = newName + timestamp;

    // Collect all files for this session
    std::vector<std::string> allFiles;
    for (auto& p : session.csvPaths) allFiles.push_back(p);
    if (!session.flagsPath.empty()) allFiles.push_back(session.flagsPath);
    if (!session.metadataPath.empty()) allFiles.push_back(session.metadataPath);
    if (!session.biofeedbackAvatarPath.empty()) allFiles.push_back(session.biofeedbackAvatarPath);
    if (!session.biofeedbackAnglesPath.empty()) allFiles.push_back(session.biofeedbackAnglesPath);

    if (allFiles.empty()) return "No files found for this session.";

    // Compute new paths — replace old prefix with new prefix in each filename
    std::vector<std::pair<fs::path, fs::path>> renames; // (old, new)
    for (auto& filePath : allFiles) {
        fs::path p(filePath);
        std::string filename = p.filename().string();

        // Find the old display name in the filename and replace with new name
        std::string oldDisplayName = session.displayName;
        auto pos = filename.find(oldDisplayName);
        if (pos == std::string::npos) {
            return "Could not find session name in filename: " + filename;
        }
        std::string newFilename = newName + filename.substr(pos + oldDisplayName.size());
        fs::path newPath = p.parent_path() / newFilename;
        renames.push_back({p, newPath});
    }

    // Pre-flight: check no destination files already exist
    // On Windows, fs::exists is case-insensitive, so allow case-only renames
    // by checking if the existing file is actually the same file (same path ignoring case)
    for (auto& [oldP, newP] : renames) {
        if (fs::exists(newP)) {
            // Check if it's the same file (case-only rename) by comparing canonical paths
            std::error_code ec1, ec2;
            auto canon1 = fs::canonical(oldP, ec1);
            auto canon2 = fs::canonical(newP, ec2);
            if (ec1 || ec2 || canon1 != canon2) {
                return "File already exists: " + newP.filename().string();
            }
            // Same file — case-only rename, allowed
        }
    }

    // Execute renames
    // On Windows, case-only renames may fail with direct rename.
    // Use two-step: old → temp, temp → new
    std::vector<std::pair<fs::path, fs::path>> completed;
    for (auto& [oldP, newP] : renames) {
        if (oldP == newP) continue;

        std::error_code ec;
        // Check if this is a case-only rename (same canonical path)
        auto canon1 = fs::canonical(oldP, ec);
        bool caseOnly = (!ec && fs::exists(newP));

        if (caseOnly) {
            // Two-step rename via temp file to handle case-only changes on Windows
            fs::path tempP = oldP;
            tempP += ".tmp_rename";
            fs::rename(oldP, tempP, ec);
            if (ec) {
                for (auto& [origNew, origOld] : completed) fs::rename(origNew, origOld);
                return "Rename failed (step 1) for " + oldP.filename().string() + ": " + ec.message();
            }
            fs::rename(tempP, newP, ec);
            if (ec) {
                fs::rename(tempP, oldP); // restore
                for (auto& [origNew, origOld] : completed) fs::rename(origNew, origOld);
                return "Rename failed (step 2) for " + oldP.filename().string() + ": " + ec.message();
            }
        } else {
            fs::rename(oldP, newP, ec);
            if (ec) {
                for (auto& [origNew, origOld] : completed) fs::rename(origNew, origOld);
                return "Rename failed for " + oldP.filename().string() + ": " + ec.message();
            }
        }
        completed.push_back({newP, oldP});
    }

    // Update metadata.json with new patient_id
    // Find the new metadata path
    for (auto& [oldP, newP] : renames) {
        if (oldP.string().find("_metadata.json") != std::string::npos) {
            // Read, update patient_id, write back
            std::ifstream fin(newP.string());
            if (fin.is_open()) {
                std::string json((std::istreambuf_iterator<char>(fin)), std::istreambuf_iterator<char>());
                fin.close();

                // Replace patient_id value
                auto pidPos = json.find("\"patient_id\"");
                if (pidPos != std::string::npos) {
                    auto colonPos = json.find(':', pidPos);
                    auto firstQuote = json.find('"', colonPos + 1);
                    auto secondQuote = json.find('"', firstQuote + 1);
                    if (firstQuote != std::string::npos && secondQuote != std::string::npos) {
                        json = json.substr(0, firstQuote + 1) + newName + json.substr(secondQuote);
                    }
                }

                std::ofstream fout(newP.string());
                if (fout.is_open()) {
                    fout << json;
                }
            }
            break;
        }
    }

    // Re-scan to refresh
    scan(recordingsDir);
    return ""; // success
}

std::string SessionScanner::updateMetadata(int sessionIndex, const std::string& patientId,
                                            const std::string& operatorName, const std::string& notes) {
    if (sessionIndex < 0 || sessionIndex >= (int)sessions_.size())
        return "Invalid session index.";

    auto& session = sessions_[sessionIndex];
    if (session.metadataPath.empty())
        return "No metadata file for this session.";

    // Read existing JSON
    std::ifstream fin(session.metadataPath);
    if (!fin.is_open()) return "Could not open metadata file.";
    std::string json((std::istreambuf_iterator<char>(fin)), std::istreambuf_iterator<char>());
    fin.close();

    // Helper to replace a JSON string value
    auto replaceField = [](std::string& json, const std::string& key, const std::string& newVal) {
        auto pos = json.find("\"" + key + "\"");
        if (pos == std::string::npos) return;
        auto colonPos = json.find(':', pos);
        if (colonPos == std::string::npos) return;
        auto firstQuote = json.find('"', colonPos + 1);
        auto secondQuote = json.find('"', firstQuote + 1);
        if (firstQuote != std::string::npos && secondQuote != std::string::npos) {
            // Escape the new value
            std::string escaped;
            for (char c : newVal) {
                if (c == '"') escaped += "\\\"";
                else if (c == '\\') escaped += "\\\\";
                else if (c == '\n') escaped += "\\n";
                else escaped += c;
            }
            json = json.substr(0, firstQuote + 1) + escaped + json.substr(secondQuote);
        }
    };

    replaceField(json, "patient_id", patientId);
    replaceField(json, "operator", operatorName);
    replaceField(json, "notes", notes);

    // Write back
    std::ofstream fout(session.metadataPath);
    if (!fout.is_open()) return "Could not write metadata file.";
    fout << json;
    fout.close();

    // Update in-memory state
    session.patientId = patientId;
    session.operatorName = operatorName;
    session.notes = notes;

    return ""; // success
}

std::string SessionScanner::recycleSessions(const std::vector<int>& indices,
                                              const std::string& recordingsDir) {
    if (indices.empty()) return "No sessions selected.";

    // Create recycling bin directory
    std::string binDir = recordingsDir + "/.recycled";
    fs::create_directories(binDir);

    int movedCount = 0;
    for (int idx : indices) {
        if (idx < 0 || idx >= (int)sessions_.size()) continue;
        auto& session = sessions_[idx];

        // Collect all files
        std::vector<std::string> allFiles;
        for (auto& p : session.csvPaths) allFiles.push_back(p);
        if (!session.flagsPath.empty()) allFiles.push_back(session.flagsPath);
        if (!session.metadataPath.empty()) allFiles.push_back(session.metadataPath);
        if (!session.biofeedbackAvatarPath.empty()) allFiles.push_back(session.biofeedbackAvatarPath);
        if (!session.biofeedbackAnglesPath.empty()) allFiles.push_back(session.biofeedbackAnglesPath);

        // Move each file to recycling bin
        for (auto& filePath : allFiles) {
            fs::path src(filePath);
            fs::path dst = fs::path(binDir) / src.filename();
            std::error_code ec;
            fs::rename(src, dst, ec);
            if (ec) {
                return "Failed to recycle " + src.filename().string() + ": " + ec.message();
            }
        }
        movedCount++;
    }

    // Re-scan
    scan(recordingsDir);
    return ""; // success
}
