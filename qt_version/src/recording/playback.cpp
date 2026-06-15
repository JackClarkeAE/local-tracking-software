#include "playback.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cstring>

bool PlaybackManager::loadCSV(const std::string& filepath) {
    clear();

    std::ifstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "Playback: failed to open " << filepath << std::endl;
        return false;
    }

    filePath_ = filepath;

    // Read header
    std::string header;
    std::getline(file, header);

    // Parse rows grouped by timestamp
    // Format: time_seconds,body_id,joint_index,joint_name,x,y,z,confidence
    std::map<double, std::map<uint32_t, TrackedBody>> frameMap;

    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;

        // Fast CSV parsing
        double timeSec;
        uint32_t bodyId;
        int jointIdx;
        char jointName[64];
        float x, y, z, conf;

        int parsed = sscanf(line.c_str(), "%lf,%u,%d,%[^,],%f,%f,%f,%f",
                           &timeSec, &bodyId, &jointIdx, jointName, &x, &y, &z, &conf);
        if (parsed < 8) continue;
        if (jointIdx < 0 || jointIdx >= JOINT_COUNT) continue;

        auto& bodies = frameMap[timeSec];
        auto& body = bodies[bodyId];
        body.id = bodyId;
        body.joints[jointIdx].x = x;
        body.joints[jointIdx].y = y;
        body.joints[jointIdx].z = z;
        body.joints[jointIdx].confidence = conf;
    }

    // Convert map to sorted vector
    frames_.reserve(frameMap.size());
    for (auto& [time, bodyMap] : frameMap) {
        PlaybackFrame pf;
        pf.timeSeconds = time;
        for (auto& [id, body] : bodyMap) {
            pf.bodies.push_back(body);
        }
        frames_.push_back(std::move(pf));
    }

    // Normalize timestamps to be relative to the first frame
    // This handles both old epoch-microsecond CSVs and new relative-seconds CSVs
    if (!frames_.empty() && frames_.front().timeSeconds != 0.0) {
        double offset = frames_.front().timeSeconds;
        // Detect old format: if first timestamp > 1e9, it's likely epoch microseconds
        bool isEpochMicroseconds = (offset > 1e9);
        for (auto& f : frames_) {
            f.timeSeconds -= offset;
            if (isEpochMicroseconds) {
                // Convert microseconds delta to seconds
                f.timeSeconds /= 1000000.0;
            }
        }
    }

    std::cout << "Playback: loaded " << frames_.size() << " frames, duration "
              << duration() << " s from " << filepath << std::endl;

    // Auto-detect and load companion flags file
    loadFlags(filepath);
    loadMetadata(filepath);

    return !frames_.empty();
}

static std::string companionStemForCsv(const std::string& basePath) {
    auto dotPos = basePath.rfind(".csv");
    if (dotPos == std::string::npos) return "";
    std::string stem = basePath.substr(0, dotPos);

    if (stem.size() >= 5 && stem.substr(stem.size() - 5, 4) == "_cam") {
        stem = stem.substr(0, stem.size() - 5);
    }
    return stem;
}

static std::string extractJsonStringLocal(const std::string& json, const std::string& key) {
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
            if (json[i + 1] == '"') { result += '"'; i++; }
            else if (json[i + 1] == 'n') { result += '\n'; i++; }
            else if (json[i + 1] == '\\') { result += '\\'; i++; }
            else result += json[i];
        } else if (json[i] == '"') {
            break;
        } else {
            result += json[i];
        }
    }
    return result;
}

static bool extractJsonBoolLocal(const std::string& json, const std::string& key) {
    auto pos = json.find("\"" + key + "\"");
    if (pos == std::string::npos) return false;
    pos = json.find(':', pos);
    if (pos == std::string::npos) return false;
    auto start = json.find_first_not_of(" \t\r\n", pos + 1);
    return start != std::string::npos && json.substr(start, 4) == "true";
}

bool PlaybackManager::loadFlags(const std::string& basePath) {
    flags_.clear();

    // Try to find flags file by stripping _camN suffix and adding _flags
    // e.g. "session_20260401_143052_cam0.csv" → "session_20260401_143052_flags.csv"
    const std::string stem = companionStemForCsv(basePath);
    if (stem.empty()) return false;

    std::string flagPath = stem + "_flags.csv";
    std::ifstream flagFile(flagPath);
    if (!flagFile.is_open()) return false;

    // Skip header
    std::string header;
    std::getline(flagFile, header);

    std::string line;
    while (std::getline(flagFile, line)) {
        if (line.empty()) continue;
        auto comma = line.find(',');
        if (comma == std::string::npos) continue;
        PlaybackFlag flag;
        try { flag.timeSeconds = std::stod(line.substr(0, comma)); } catch (...) { continue; }
        flag.label = line.substr(comma + 1);
        flags_.push_back(flag);
    }

    if (!flags_.empty()) {
        std::cout << "Playback: loaded " << flags_.size() << " flags from " << flagPath << std::endl;
    }
    return !flags_.empty();
}

// Legacy Azure JSON joint mapping → canonical joint indices
bool PlaybackManager::loadMetadata(const std::string& basePath) {
    metadata_ = PlaybackMetadata{};

    const std::string stem = companionStemForCsv(basePath);
    if (stem.empty()) return false;

    std::ifstream f(stem + "_metadata.json");
    if (!f.is_open()) return false;

    std::string json((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    metadata_.patientId = extractJsonStringLocal(json, "patient_id");
    metadata_.operatorName = extractJsonStringLocal(json, "operator");
    metadata_.notes = extractJsonStringLocal(json, "notes");
    metadata_.date = extractJsonStringLocal(json, "date");
    metadata_.protocol = extractJsonStringLocal(json, "protocol");
    metadata_.biofeedbackActive = extractJsonBoolLocal(json, "biofeedback_active");

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
                std::string name = extractJsonStringLocal(arr.substr(namePos), "name");
                if (!name.empty()) metadata_.cameraNames.push_back(name);
                pos = namePos + 6;
            }
        }
    }

    metadata_.loaded = true;
    return true;
}

static const int LEGACY_TO_CANONICAL[32] = {
    25, // 0  right_foot    → FOOT_RIGHT
    21, // 1  left_foot     → FOOT_LEFT
    24, // 2  right_ankle   → ANKLE_RIGHT
    20, // 3  left_ankle    → ANKLE_LEFT
    23, // 4  right_knee    → KNEE_RIGHT
    19, // 5  left_knee     → KNEE_LEFT
    22, // 6  right_hip     → HIP_RIGHT
    18, // 7  left_hip      → HIP_LEFT
     1, // 8  spine_navel   → SPINE_NAVEL
     2, // 9  spine_chest   → SPINE_CHEST
     3, // 10 neck          → NECK
    26, // 11 head          → HEAD
    12, // 12 right_shoulder→ SHOULDER_RIGHT
     5, // 13 left_shoulder → SHOULDER_LEFT
    13, // 14 right_elbow   → ELBOW_RIGHT
     6, // 15 left_elbow    → ELBOW_LEFT
    14, // 16 right_wrist   → WRIST_RIGHT
     7, // 17 left_wrist    → WRIST_LEFT
    15, // 18 right_hand    → HAND_RIGHT
     8, // 19 left_hand     → HAND_LEFT
     0, // 20 pelvis        → PELVIS
    27, // 21 nose          → NOSE
    30, // 22 right_eye     → EYE_RIGHT
    31, // 23 right_ear     → EAR_RIGHT
    28, // 24 left_eye      → EYE_LEFT
    29, // 25 left_ear      → EAR_LEFT
    11, // 26 right_clavicle→ CLAVICLE_RIGHT
     4, // 27 left_clavicle → CLAVICLE_LEFT
    17, // 28 right_thumb   → THUMB_RIGHT
    16, // 29 tip_right_hand→ HANDTIP_RIGHT
    10, // 30 left_thumb    → THUMB_LEFT
     9, // 31 tip_left_hand → HANDTIP_LEFT
};

static double parseLegacyTimestamp(const std::string& ts) {
    // Format: "HH:MM:SS.cs" e.g. "00:07:32.06"
    int h = 0, m = 0, s = 0, cs = 0;
    if (sscanf(ts.c_str(), "%d:%d:%d.%d", &h, &m, &s, &cs) >= 3) {
        return h * 3600.0 + m * 60.0 + s + cs * 0.01;
    }
    return 0.0;
}

bool PlaybackManager::loadLegacyJSON(const std::string& filepath) {
    clear();
    filePath_ = filepath;

    std::ifstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "Legacy: failed to open " << filepath << std::endl;
        return false;
    }

    // Read entire file
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

    // Find all frame objects by searching for "Timestamp" markers
    double firstTime = -1.0;
    size_t searchPos = 0;

    while (true) {
        // Find next Timestamp
        auto tsPos = content.find("\"Timestamp\"", searchPos);
        if (tsPos == std::string::npos) break;

        // Extract timestamp string
        auto tsQuote1 = content.find('"', content.find(':', tsPos) + 1);
        auto tsQuote2 = content.find('"', tsQuote1 + 1);
        if (tsQuote1 == std::string::npos || tsQuote2 == std::string::npos) break;
        std::string tsStr = content.substr(tsQuote1 + 1, tsQuote2 - tsQuote1 - 1);
        double timeSec = parseLegacyTimestamp(tsStr);

        if (firstTime < 0) firstTime = timeSec;
        double relTime = timeSec - firstTime;

        // Find Joint_Positions array
        auto jpPos = content.find("\"Joint_Positions\"", tsPos);
        if (jpPos == std::string::npos) break;
        auto arrStart = content.find('[', jpPos);
        auto arrEnd = content.find(']', arrStart);
        if (arrStart == std::string::npos || arrEnd == std::string::npos) break;

        // Parse the 96 float values (may be quoted strings or raw numbers)
        std::string arrStr = content.substr(arrStart + 1, arrEnd - arrStart - 1);
        std::vector<float> values;
        values.reserve(96);
        size_t vPos = 0;
        while (vPos < arrStr.size() && values.size() < 96) {
            // Skip whitespace and commas
            while (vPos < arrStr.size() && (arrStr[vPos] == ' ' || arrStr[vPos] == ',' || arrStr[vPos] == '\n' || arrStr[vPos] == '\r'))
                vPos++;
            if (vPos >= arrStr.size()) break;

            // Skip opening quote if present
            bool quoted = (arrStr[vPos] == '"');
            if (quoted) vPos++;

            // Find end of number
            size_t numStart = vPos;
            while (vPos < arrStr.size() && arrStr[vPos] != ',' && arrStr[vPos] != '"' && arrStr[vPos] != ']')
                vPos++;
            std::string numStr = arrStr.substr(numStart, vPos - numStart);

            if (quoted && vPos < arrStr.size() && arrStr[vPos] == '"') vPos++;

            try { values.push_back(std::stof(numStr)); } catch (...) { values.push_back(0.0f); }
        }

        if (values.size() >= 96) {
            PlaybackFrame pf;
            pf.timeSeconds = relTime;

            TrackedBody body;
            body.id = 1;

            // Remap legacy joints to canonical order, convert mm → metres
            for (int legacyIdx = 0; legacyIdx < 32; legacyIdx++) {
                int canonIdx = LEGACY_TO_CANONICAL[legacyIdx];
                if (canonIdx >= 0 && canonIdx < JOINT_COUNT) {
                    body.joints[canonIdx].x = values[legacyIdx * 3 + 0] / 1000.0f;
                    body.joints[canonIdx].y = -values[legacyIdx * 3 + 1] / 1000.0f; // negate Y (Kinect Y-down)
                    body.joints[canonIdx].z = values[legacyIdx * 3 + 2] / 1000.0f;
                    body.joints[canonIdx].confidence = 1.0f;
                }
            }

            pf.bodies.push_back(body);
            frames_.push_back(std::move(pf));
        }

        searchPos = arrEnd + 1;
    }

    std::cout << "Legacy: loaded " << frames_.size() << " frames from " << filepath << std::endl;
    return !frames_.empty();
}

void PlaybackManager::clear() {
    frames_.clear();
    flags_.clear();
    metadata_ = PlaybackMetadata{};
    filePath_.clear();
}

double PlaybackManager::duration() const {
    if (frames_.empty()) return 0.0;
    return frames_.back().timeSeconds;
}

int PlaybackManager::getFrameIndexAtTime(double timeSeconds) const {
    if (frames_.empty()) return -1;
    if (timeSeconds <= 0) return 0;
    if (timeSeconds >= frames_.back().timeSeconds) return (int)frames_.size() - 1;

    // Binary search for the last frame <= timeSeconds
    int lo = 0, hi = (int)frames_.size() - 1;
    while (lo < hi) {
        int mid = lo + (hi - lo + 1) / 2;
        if (frames_[mid].timeSeconds <= timeSeconds)
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo;
}

const PlaybackFrame* PlaybackManager::getFrameAtTime(double timeSeconds) const {
    int idx = getFrameIndexAtTime(timeSeconds);
    if (idx < 0) return nullptr;
    return &frames_[idx];
}

const PlaybackFrame* PlaybackManager::getFrame(int index) const {
    if (index < 0 || index >= (int)frames_.size()) return nullptr;
    return &frames_[index];
}

bool PlaybackManager::loadResampledCSV(const std::string& filepath) {
    clear();

    std::ifstream file(filepath);
    if (!file.is_open()) return false;

    // Map from CSV column prefix to canonical joint index
    static const std::pair<std::string, int> jointMap[] = {
        {"pelvis",          0},
        {"spine_navel",     1},
        {"spine_chest",     2},
        {"neck",            3},
        {"left_clavicle",   4},
        {"left_shoulder",   5},
        {"left_elbow",      6},
        {"left_wrist",      7},
        {"left_hand",       8},
        {"tip_left_hand",   9},
        {"left_thumb",     10},
        {"right_clavicle", 11},
        {"right_shoulder", 12},
        {"right_elbow",    13},
        {"right_wrist",    14},
        {"right_hand",     15},
        {"tip_right_hand", 16},
        {"right_thumb",    17},
        {"left_hip",       18},
        {"left_knee",      19},
        {"left_ankle",     20},
        {"left_foot",      21},
        {"right_hip",      22},
        {"right_knee",     23},
        {"right_ankle",    24},
        {"right_foot",     25},
        {"head",           26},
        {"nose",           27},
        {"left_eye",       28},
        {"left_ear",       29},
        {"right_eye",      30},
        {"right_ear",      31},
    };

    // Read header and map column indices to (joint_index, axis 0=x 1=y 2=z)
    std::string header;
    if (!std::getline(file, header)) return false;

    std::vector<std::string> colNames;
    {
        std::istringstream hs(header);
        std::string col;
        while (std::getline(hs, col, ',')) colNames.push_back(col);
    }

    // For each column, determine if it's a joint coordinate
    // Format: {joint_prefix}_{x|y|z}
    struct ColMapping { int jointIdx; int axis; }; // axis: 0=x, 1=y, 2=z
    std::vector<ColMapping> colMap(colNames.size(), {-1, -1});
    int timeCol = -1;

    for (int c = 0; c < (int)colNames.size(); c++) {
        const auto& name = colNames[c];
        if (name == "t" || name == "time" || name == "time_seconds") {
            timeCol = c;
            continue;
        }

        // Check if column ends with _x, _y, or _z
        if (name.size() < 3) continue;
        std::string suffix = name.substr(name.size() - 2);
        int axis = -1;
        if (suffix == "_x") axis = 0;
        else if (suffix == "_y") axis = 1;
        else if (suffix == "_z") axis = 2;
        else continue;

        std::string prefix = name.substr(0, name.size() - 2);

        for (auto& [jName, jIdx] : jointMap) {
            if (prefix == jName) {
                colMap[c] = {jIdx, axis};
                break;
            }
        }
    }

    if (timeCol < 0) return false;

    // Read data rows
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty()) continue;

        std::vector<float> vals;
        std::istringstream ls(line);
        std::string cell;
        while (std::getline(ls, cell, ',')) {
            try { vals.push_back(std::stof(cell)); }
            catch (...) { vals.push_back(0.0f); }
        }

        if ((int)vals.size() <= timeCol) continue;

        PlaybackFrame frame;
        frame.timeSeconds = (double)vals[timeCol];

        TrackedBody body;
        body.id = 1;
        // Set all joints to confidence 1.0 (resampled data has no confidence info)
        for (int j = 0; j < JOINT_COUNT; j++)
            body.joints[j].confidence = 1.0f;

        for (int c = 0; c < (int)vals.size() && c < (int)colMap.size(); c++) {
            auto& m = colMap[c];
            if (m.jointIdx < 0) continue;
            float v = vals[c];
            if (m.axis == 0) body.joints[m.jointIdx].x = v;
            else if (m.axis == 1) body.joints[m.jointIdx].y = v;
            else if (m.axis == 2) body.joints[m.jointIdx].z = v;
        }

        frame.bodies.push_back(body);
        frames_.push_back(std::move(frame));
    }

    filePath_ = filepath;
    return !frames_.empty();
}
