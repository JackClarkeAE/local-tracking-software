#include "biofeedback_recorder.h"
#include <filesystem>

BiofeedbackRecorder::~BiofeedbackRecorder() {
    stop();
}

bool BiofeedbackRecorder::start(const std::string& avatarCsvPath, const std::string& anglesCsvPath) {
    stop();

    if (!avatarRecorder_.start(avatarCsvPath))
        return false;

    anglesFile_ = fopen(anglesCsvPath.c_str(), "w");
    if (!anglesFile_) {
        avatarRecorder_.stop();
        return false;
    }

    fprintf(anglesFile_, "time_seconds,body_id,angle_name,raw_degrees,modified_degrees,scale_factor\n");
    recording_ = true;
    return true;
}

void BiofeedbackRecorder::recordFrame(const FrameData& modifiedFrame,
                                       const std::vector<AngleMeasurement>& angles,
                                       double timeSeconds) {
    if (!recording_) return;

    // Record modified skeleton (same format as raw joint recording)
    avatarRecorder_.recordFrame(modifiedFrame);

    // Record angle measurements
    if (anglesFile_) {
        for (auto& body : modifiedFrame.bodies) {
            for (auto& m : angles) {
                const AngleDefinition& def = getAngleDefinition(m.angle);
                fprintf(anglesFile_, "%.6f,%u,%s,%.3f,%.3f,%.3f\n",
                        timeSeconds,
                        body.id,
                        def.name,
                        m.rawDegrees,
                        m.modifiedDegrees,
                        m.scaleFactor);
            }
        }
        fflush(anglesFile_);
    }
}

void BiofeedbackRecorder::stop() {
    if (!recording_) return;
    avatarRecorder_.stop();
    if (anglesFile_) {
        fclose(anglesFile_);
        anglesFile_ = nullptr;
    }
    recording_ = false;
}
