#pragma once
#include "../camera/camera_types.h"
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <cstdint>

struct SessionInfo {
    std::string prefix;           // e.g. "session_20260401_143052"
    std::string displayName;      // name part before timestamp

    // File paths (empty if not present)
    std::vector<std::string> csvPaths;
    std::string flagsPath;
    std::string metadataPath;
    std::string biofeedbackAvatarPath;
    std::string biofeedbackAnglesPath;

    // From metadata.json
    std::string patientId;
    std::string operatorName;
    std::string notes;
    std::string date;
    std::string protocol;
    std::vector<std::string> cameraNames;
    bool biofeedbackActive = false;

    // Quick stats (computed during scan)
    int frameCount = 0;
    double durationSeconds = 0.0;
    int flagCount = 0;
    uint64_t totalFileSize = 0;
    int cameraCount = 0;

    // Detailed stats (computed on-demand)
    bool detailedStatsLoaded = false;
    float avgConfidence = 0.0f;
    float trackedFramePercent = 0.0f;

    struct AngleSummary {
        std::string name;
        float minDeg = 0.0f, maxDeg = 0.0f, meanDeg = 0.0f;
    };
    std::vector<AngleSummary> angleSummaries;

    // UI state
    bool selected = false;
};

class SessionScanner {
public:
    void scan(const std::string& recordingsDir);
    void computeDetailedStats(int sessionIndex);
    void cancelBackgroundWork();

    bool isComputingStats() const { return computing_.load(); }
    float computeProgress() const { return computeProgress_.load(); }

    std::vector<SessionInfo>& sessions() { return sessions_; }
    const std::vector<SessionInfo>& sessions() const { return sessions_; }

    std::vector<int> selectedIndices() const;
    void selectAll();
    void deselectAll();

    // Rename a session — changes all filenames and updates metadata.json
    std::string renameSession(int sessionIndex, const std::string& newName,
                               const std::string& recordingsDir);

    // Move selected sessions to internal recycling bin
    std::string recycleSessions(const std::vector<int>& indices,
                                 const std::string& recordingsDir);

    // Update session metadata (patient ID, operator, notes) in the metadata.json
    std::string updateMetadata(int sessionIndex, const std::string& patientId,
                                const std::string& operatorName, const std::string& notes);

private:
    void parseMetadata(SessionInfo& session);
    void computeQuickStats(SessionInfo& session);
    double getLastTimestamp(const std::string& csvPath);
    int countLines(const std::string& path);

    std::vector<SessionInfo> sessions_;
    std::thread computeThread_;
    std::atomic<bool> computing_{false};
    std::atomic<bool> cancelCompute_{false};
    std::atomic<float> computeProgress_{0.0f};
};
