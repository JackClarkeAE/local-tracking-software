#include "protocol.h"
#include "../biofeedback/biofeedback_engine.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <iostream>
#include <filesystem>
#include <cstring>

// ============================================================
// Event type helpers
// ============================================================

static const char* EVENT_TYPE_NAMES[] = {
    "Start Camera", "Countdown", "Start Recording", "Stop Recording",
    "Add Flag", "Display Text", "Display Shape", "Clear Overlay", "Stop Camera",
    "Hide Avatar", "Show Avatar",
    "[EXP] Begin Biofeedback", "[EXP] End Biofeedback",
    "[EXP] Set BF Transform", "[EXP] Clear BF Transform",
    "Play Sound"
};

static const char* EVENT_TYPE_JSON_NAMES[] = {
    "StartCamera", "Countdown", "StartRecording", "StopRecording",
    "AddFlag", "DisplayText", "DisplayShape", "ClearOverlay", "StopCamera",
    "HideAvatar", "ShowAvatar",
    "BeginBiofeedback", "EndBiofeedback",
    "SetBiofeedbackTransform", "ClearBiofeedbackTransform",
    "PlaySound"
};

const char* protocolEventTypeName(ProtocolEventType type) {
    return EVENT_TYPE_NAMES[(int)type];
}

int protocolEventTypeIndex(ProtocolEventType type) {
    return (int)type;
}

ProtocolEventType protocolEventTypeFromIndex(int index) {
    if (index < 0 || index >= protocolEventTypeCount())
        return ProtocolEventType::StartCamera;
    return (ProtocolEventType)index;
}

int protocolEventTypeCount() {
    return 16;
}

// ============================================================
// Protocol
// ============================================================

void Protocol::sortByTime() {
    std::sort(events.begin(), events.end(),
        [](const ProtocolEvent& a, const ProtocolEvent& b) {
            return a.timeOffsetSeconds < b.timeOffsetSeconds;
        });
}

std::string Protocol::validate() const {
    bool cameraStarted = false;
    bool recording = false;
    bool biofeedbackActive = false;

    for (auto& e : events) {
        switch (e.type) {
            case ProtocolEventType::StartCamera:
                if (cameraStarted) return "Duplicate 'Start Camera' event";
                cameraStarted = true;
                break;
            case ProtocolEventType::StartRecording:
                if (!cameraStarted) return "'Start Recording' before 'Start Camera'";
                if (recording) return "Duplicate 'Start Recording' without 'Stop Recording'";
                recording = true;
                break;
            case ProtocolEventType::StopRecording:
                if (!recording) return "'Stop Recording' without active recording";
                recording = false;
                break;
            case ProtocolEventType::StopCamera:
                if (recording) return "'Stop Camera' while still recording";
                if (biofeedbackActive) return "'Stop Camera' while biofeedback active";
                cameraStarted = false;
                break;
            case ProtocolEventType::AddFlag:
                if (!recording) return "'Add Flag' while not recording";
                break;
            case ProtocolEventType::Countdown:
                if (e.parameter.empty()) return "Countdown event needs seconds parameter";
                break;
            case ProtocolEventType::BeginBiofeedback:
                if (!cameraStarted) return "'Begin Biofeedback' before 'Start Camera'";
                if (biofeedbackActive) return "Duplicate 'Begin Biofeedback'";
                biofeedbackActive = true;
                break;
            case ProtocolEventType::EndBiofeedback:
                if (!biofeedbackActive) return "'End Biofeedback' without active biofeedback";
                biofeedbackActive = false;
                break;
            case ProtocolEventType::SetBiofeedbackTransform:
                if (!biofeedbackActive) return "'Set BF Transform' before 'Begin Biofeedback'";
                if (e.parameter.empty()) return "'Set BF Transform' needs parameters";
                break;
            case ProtocolEventType::ClearBiofeedbackTransform:
                if (!biofeedbackActive) return "'Clear BF Transform' before 'Begin Biofeedback'";
                break;
            case ProtocolEventType::PlaySound:
                if (e.parameter.empty()) return "'Play Sound' needs a sound name or WAV file path";
                break;
            default:
                break;
        }
    }

    // Validate biofeedback transform kinematic ordering:
    // At the same time point, proximal joints must be listed before distal joints.
    // Chain priority (lower = more proximal, must come first):
    //   Hip (2) > Knee (1) > Ankle (0)
    // Left and right sides are independent.
    auto chainPriority = [](const std::string& param, bool& isLeft) -> int {
        std::string angleName = param.substr(0, param.find(','));
        isLeft = (angleName.find("LEFT") != std::string::npos);
        if (angleName.find("HIP") != std::string::npos) return 2;
        if (angleName.find("KNEE") != std::string::npos) return 1;
        if (angleName.find("ANKLE") != std::string::npos) return 0;
        return -1; // unknown
    };

    for (size_t i = 0; i + 1 < events.size(); i++) {
        if (events[i].type != ProtocolEventType::SetBiofeedbackTransform) continue;
        for (size_t j = i + 1; j < events.size(); j++) {
            if (events[j].type != ProtocolEventType::SetBiofeedbackTransform) continue;
            // Only check events at the same time
            if (events[j].timeOffsetSeconds != events[i].timeOffsetSeconds) break;

            bool leftI, leftJ;
            int priI = chainPriority(events[i].parameter, leftI);
            int priJ = chainPriority(events[j].parameter, leftJ);
            if (priI < 0 || priJ < 0) continue;
            // Only compare same side
            if (leftI != leftJ) continue;
            // Proximal (higher priority) must come first
            if (priI < priJ) {
                std::string side = leftI ? "left" : "right";
                return "Kinematic order error: on the " + side + " side, proximal transforms (hip) "
                       "must be listed before distal transforms (knee/ankle) at time "
                       + std::to_string((int)events[i].timeOffsetSeconds) + "s";
            }
        }
    }

    return ""; // valid
}

// ============================================================
// Minimal JSON serialization (no external dependency)
// ============================================================

static std::string escapeJSON(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

bool Protocol::saveJSON(const std::string& filepath) const {
    std::filesystem::create_directories(std::filesystem::path(filepath).parent_path());

    std::ofstream f(filepath);
    if (!f.is_open()) return false;

    f << "{\n";
    f << "  \"name\": \"" << escapeJSON(name) << "\",\n";
    f << "  \"events\": [\n";

    for (size_t i = 0; i < events.size(); i++) {
        auto& e = events[i];
        f << "    { \"type\": \"" << EVENT_TYPE_JSON_NAMES[(int)e.type] << "\", "
          << "\"time\": " << e.timeOffsetSeconds << ", "
          << "\"param\": \"" << escapeJSON(e.parameter) << "\" }";
        if (i + 1 < events.size()) f << ",";
        f << "\n";
    }

    f << "  ]\n";
    f << "}\n";
    return true;
}

// Simple JSON parser — handles our specific format only
static std::string extractString(const std::string& line, const std::string& key) {
    auto pos = line.find("\"" + key + "\"");
    if (pos == std::string::npos) return "";
    pos = line.find(':', pos);
    if (pos == std::string::npos) return "";
    auto start = line.find('"', pos + 1);
    if (start == std::string::npos) return "";
    start++;
    std::string result;
    for (size_t i = start; i < line.size(); i++) {
        if (line[i] == '\\' && i + 1 < line.size()) {
            if (line[i + 1] == '"') { result += '"'; i++; }
            else if (line[i + 1] == 'n') { result += '\n'; i++; }
            else if (line[i + 1] == '\\') { result += '\\'; i++; }
            else result += line[i];
        } else if (line[i] == '"') {
            break;
        } else {
            result += line[i];
        }
    }
    return result;
}

static float extractFloat(const std::string& line, const std::string& key) {
    auto pos = line.find("\"" + key + "\"");
    if (pos == std::string::npos) return 0.0f;
    pos = line.find(':', pos);
    if (pos == std::string::npos) return 0.0f;
    return std::stof(line.substr(pos + 1));
}

bool Protocol::loadJSON(const std::string& filepath) {
    std::ifstream f(filepath);
    if (!f.is_open()) return false;

    events.clear();
    name = "Untitled";

    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());

    // Extract name
    name = extractString(content, "name");
    if (name.empty()) name = "Untitled";

    // Extract events — find each { } block inside "events" array
    auto eventsPos = content.find("\"events\"");
    if (eventsPos == std::string::npos) return false;

    auto arrStart = content.find('[', eventsPos);
    if (arrStart == std::string::npos) return false;

    size_t pos = arrStart;
    while (true) {
        auto objStart = content.find('{', pos);
        if (objStart == std::string::npos) break;
        auto objEnd = content.find('}', objStart);
        if (objEnd == std::string::npos) break;

        std::string obj = content.substr(objStart, objEnd - objStart + 1);

        ProtocolEvent e;
        std::string typeName = extractString(obj, "type");
        e.timeOffsetSeconds = extractFloat(obj, "time");
        e.parameter = extractString(obj, "param");

        // Map type name to enum
        e.type = ProtocolEventType::StartCamera;
        for (int i = 0; i < protocolEventTypeCount(); i++) {
            if (typeName == EVENT_TYPE_JSON_NAMES[i]) {
                e.type = (ProtocolEventType)i;
                break;
            }
        }

        events.push_back(e);
        pos = objEnd + 1;
    }

    return !events.empty();
}

// ============================================================
// ProtocolRunner
// ============================================================

void ProtocolRunner::load(const Protocol& protocol) {
    protocol_ = protocol;
    protocol_.sortByTime();
    reset();
}

void ProtocolRunner::start() {
    state_ = ProtocolRunnerState::Running;
    nextEventIdx_ = 0;
    currentTime_ = 0.0f;
}

void ProtocolRunner::abort() {
    state_ = ProtocolRunnerState::Aborted;
}

void ProtocolRunner::reset() {
    state_ = ProtocolRunnerState::Idle;
    nextEventIdx_ = 0;
    currentTime_ = 0.0f;
}

float ProtocolRunner::totalDuration() const {
    if (protocol_.events.empty()) return 0.0f;
    return protocol_.events.back().timeOffsetSeconds;
}

std::vector<ProtocolAction> ProtocolRunner::update(float elapsedSeconds) {
    std::vector<ProtocolAction> actions;
    if (state_ != ProtocolRunnerState::Running) return actions;

    currentTime_ = elapsedSeconds;

    // Fire all events whose time has passed
    while (nextEventIdx_ < (int)protocol_.events.size()) {
        auto& e = protocol_.events[nextEventIdx_];
        if (elapsedSeconds >= e.timeOffsetSeconds) {
            ProtocolAction action;
            action.type = e.type;
            action.parameter = e.parameter;
            actions.push_back(action);
            nextEventIdx_++;
        } else {
            break;
        }
    }

    // Check if protocol is finished
    if (nextEventIdx_ >= (int)protocol_.events.size()) {
        state_ = ProtocolRunnerState::Finished;
    }

    return actions;
}

// ============================================================
// List protocol files
// ============================================================

std::vector<std::string> listProtocolFiles(const std::string& dir) {
    std::vector<std::string> files;
    if (!std::filesystem::exists(dir)) return files;

    for (auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().extension() == ".json") {
            files.push_back(entry.path().filename().string());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}
