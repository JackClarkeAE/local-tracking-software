#include "flag_recorder.h"
#include <iostream>

FlagRecorder::~FlagRecorder() {
    stop();
}

bool FlagRecorder::start(const std::string& filepath) {
    if (file_) return false;

    file_ = fopen(filepath.c_str(), "w");
    if (!file_) {
        std::cerr << "Failed to open flag file: " << filepath << std::endl;
        return false;
    }

    fprintf(file_, "time_seconds,label\n");
    fflush(file_);
    return true;
}

void FlagRecorder::addFlag(double timeSeconds, const std::string& label) {
    if (!file_) return;
    fprintf(file_, "%.6f,%s\n", timeSeconds, label.c_str());
    fflush(file_);
}

void FlagRecorder::stop() {
    if (file_) {
        fclose(file_);
        file_ = nullptr;
    }
}
