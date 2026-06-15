#pragma once
#include "../camera/camera_types.h"
#include <string>
#include <vector>
#include <map>

// A single timestamped snapshot of all bodies
struct PlaybackFrame {
    double timeSeconds = 0.0;
    std::vector<TrackedBody> bodies;
};

struct PlaybackFlag {
    double timeSeconds = 0.0;
    std::string label;
};

struct PlaybackMetadata {
    std::string patientId;
    std::string operatorName;
    std::string notes;
    std::string date;
    std::string protocol;
    std::vector<std::string> cameraNames;
    bool biofeedbackActive = false;
    bool loaded = false;
};

class PlaybackManager {
public:
    bool loadCSV(const std::string& filepath);
    bool loadLegacyJSON(const std::string& filepath);
    bool loadResampledCSV(const std::string& filepath);
    void clear();
    bool isLoaded() const { return !frames_.empty(); }

    // Accessors
    double duration() const;
    int frameCount() const { return (int)frames_.size(); }
    const std::string& filePath() const { return filePath_; }
    const std::vector<PlaybackFlag>& flags() const { return flags_; }
    const PlaybackMetadata& metadata() const { return metadata_; }

    // Get the frame at or just before the given time
    const PlaybackFrame* getFrameAtTime(double timeSeconds) const;
    int getFrameIndexAtTime(double timeSeconds) const;
    const PlaybackFrame* getFrame(int index) const;

private:
    bool loadFlags(const std::string& basePath);
    bool loadMetadata(const std::string& basePath);

    std::vector<PlaybackFrame> frames_;
    std::vector<PlaybackFlag> flags_;
    PlaybackMetadata metadata_;
    std::string filePath_;
};
