#pragma once
#include "ring_buffer.h"
#include "../camera/camera_types.h"
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <cstdio>
#include <chrono>

// Lightweight frame for recording — no image data
struct RecordingFrame {
    uint64_t timestamp_us = 0;
    std::vector<TrackedBody> bodies;
};

class JointRecorder {
public:
    JointRecorder() = default;
    ~JointRecorder();

    // epoch: shared session reference time. All recorders started with the
    // same epoch (both cameras, flags, video) produce timestamps on one
    // common timeline, so multi-camera data aligns directly in analysis.
    bool start(const std::string& filepath,
               std::chrono::steady_clock::time_point epoch = std::chrono::steady_clock::now());
    void recordFrame(const FrameData& frame);
    void stop();
    bool isRecording() const { return recording_.load(std::memory_order_relaxed); }
    uint64_t droppedFrames() const { return droppedFrames_.load(std::memory_order_relaxed); }

private:
    void writerLoop();
    void writeFrame(const RecordingFrame& frame);

    SPSCRingBuffer<RecordingFrame, 2048> buffer_;
    std::atomic<bool> recording_{false};
    std::atomic<bool> stopFlag_{false};
    std::atomic<uint64_t> droppedFrames_{0};
    std::thread writerThread_;
    std::mutex cvMutex_;
    std::condition_variable cv_;
    FILE* file_ = nullptr;
    uint64_t firstTimestamp_ = 0;
    bool gotFirstTimestamp_ = false;

    // Shared-epoch anchoring: device timestamps remain the time source
    // (smooth, no OS jitter); the first frame is pinned to the host-clock
    // offset from the shared epoch.
    std::chrono::steady_clock::time_point epoch_{};
    uint64_t epochOffsetUs_ = 0;
};
