#pragma once
#include "biofeedback_engine.h"
#include "../recording/joint_recorder.h"
#include <string>
#include <cstdio>

class BiofeedbackRecorder {
public:
    BiofeedbackRecorder() = default;
    ~BiofeedbackRecorder();

    bool start(const std::string& avatarCsvPath, const std::string& anglesCsvPath);
    void recordFrame(const FrameData& modifiedFrame,
                     const std::vector<AngleMeasurement>& angles,
                     double timeSeconds);
    void stop();
    bool isRecording() const { return recording_; }

private:
    JointRecorder avatarRecorder_;
    FILE* anglesFile_ = nullptr;
    bool recording_ = false;
};
