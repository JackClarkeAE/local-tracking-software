#pragma once
#include <string>
#include <cstdio>

class FlagRecorder {
public:
    ~FlagRecorder();

    bool start(const std::string& filepath);
    void addFlag(double timeSeconds, const std::string& label);
    void stop();
    bool isActive() const { return file_ != nullptr; }

private:
    FILE* file_ = nullptr;
};
