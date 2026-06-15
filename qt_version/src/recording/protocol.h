#pragma once
#include <string>
#include <vector>
#include <chrono>

enum class ProtocolEventType {
    StartCamera,
    Countdown,
    StartRecording,
    StopRecording,
    AddFlag,
    DisplayText,
    DisplayShape,
    ClearOverlay,
    StopCamera,
    // Avatar visibility
    HideAvatar,
    ShowAvatar,
    // EXPERIMENTAL — Biofeedback
    BeginBiofeedback,
    EndBiofeedback,
    SetBiofeedbackTransform,
    ClearBiofeedbackTransform,
    // Audio cue: parameter is a builtin sound name (beep/chime/alert)
    // or a path to a local WAV file
    PlaySound
};

struct ProtocolEvent {
    ProtocolEventType type = ProtocolEventType::StartCamera;
    float timeOffsetSeconds = 0.0f;
    std::string parameter;
};

struct Protocol {
    std::string name = "Untitled Protocol";
    std::vector<ProtocolEvent> events;

    void sortByTime();
    bool saveJSON(const std::string& filepath) const;
    bool loadJSON(const std::string& filepath);
    std::string validate() const; // returns empty string if valid, error message otherwise
};

// Helpers for display
const char* protocolEventTypeName(ProtocolEventType type);
int protocolEventTypeIndex(ProtocolEventType type);
ProtocolEventType protocolEventTypeFromIndex(int index);
int protocolEventTypeCount();

// ============================================================
// Protocol Runner — executes a protocol in real time
// ============================================================

enum class ProtocolRunnerState { Idle, Running, Finished, Aborted };

// Actions returned by the runner for the app to process
struct ProtocolAction {
    ProtocolEventType type;
    std::string parameter;
};

class ProtocolRunner {
public:
    void load(const Protocol& protocol);
    void start();
    void abort();
    void reset();

    // Call each frame with elapsed seconds since protocol start
    // Returns list of actions that fired this frame
    std::vector<ProtocolAction> update(float elapsedSeconds);

    ProtocolRunnerState state() const { return state_; }
    float totalDuration() const;
    float currentTime() const { return currentTime_; }
    int currentEventIndex() const { return nextEventIdx_ - 1; }
    int totalEvents() const { return (int)protocol_.events.size(); }
    const Protocol& protocol() const { return protocol_; }

private:
    Protocol protocol_;
    ProtocolRunnerState state_ = ProtocolRunnerState::Idle;
    int nextEventIdx_ = 0;
    float currentTime_ = 0.0f;
};

// List JSON files in protocols/ directory
std::vector<std::string> listProtocolFiles(const std::string& dir);
