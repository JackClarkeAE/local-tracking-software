#include "joint_recorder.h"
#include <iostream>
#include <chrono>
#include <algorithm>

JointRecorder::~JointRecorder() {
    stop();
}

bool JointRecorder::start(const std::string& filepath,
                          std::chrono::steady_clock::time_point epoch) {
    if (recording_ || file_) return false;
    epoch_ = epoch;
    epochOffsetUs_ = 0;

    file_ = fopen(filepath.c_str(), "w");
    if (!file_) {
        std::cerr << "Failed to open recording file: " << filepath << std::endl;
        return false;
    }

    fprintf(file_, "time_seconds,body_id,joint_index,joint_name,x,y,z,confidence\n");

    gotFirstTimestamp_ = false;
    firstTimestamp_ = 0;
    droppedFrames_.store(0);
    stopFlag_.store(false);
    recording_.store(true);
    writerThread_ = std::thread(&JointRecorder::writerLoop, this);
    return true;
}

void JointRecorder::recordFrame(const FrameData& frame) {
    if (!recording_) return;
    // Anchor the first frame to the shared epoch using its host arrival
    // time; later frames advance by device-timestamp deltas. Producer
    // thread only — the ring buffer push orders this for the writer.
    if (!gotFirstTimestamp_) {
        const auto sinceEpoch = std::chrono::steady_clock::now() - epoch_;
        epochOffsetUs_ = (uint64_t)std::max<int64_t>(0,
            std::chrono::duration_cast<std::chrono::microseconds>(sinceEpoch).count());
        firstTimestamp_ = frame.timestamp_us;
        gotFirstTimestamp_ = true;
    }
    // Copy only joints, not the image
    RecordingFrame rf;
    rf.timestamp_us = frame.timestamp_us;
    rf.bodies = frame.bodies;
    if (!buffer_.tryPush(std::move(rf))) {
        droppedFrames_.fetch_add(1, std::memory_order_relaxed);
    } else {
        cv_.notify_one();
    }
}

void JointRecorder::stop() {
    if (!recording_) return;
    stopFlag_.store(true, std::memory_order_release);
    cv_.notify_one();
    if (writerThread_.joinable())
        writerThread_.join();
    recording_.store(false);

    if (file_) {
        fclose(file_);
        file_ = nullptr;
    }

    uint64_t dropped = droppedFrames_.load();
    if (dropped > 0) {
        std::cerr << "Recording finished. Dropped frames: " << dropped << std::endl;
    }
}

void JointRecorder::writeFrame(const RecordingFrame& frame) {
    double relativeSeconds =
        (double)(epochOffsetUs_ + (frame.timestamp_us - firstTimestamp_)) / 1000000.0;

    for (auto& body : frame.bodies) {
        for (int j = 0; j < JOINT_COUNT; ++j) {
            fprintf(file_, "%.6f,%u,%d,%s,%.6f,%.6f,%.6f,%.3f\n",
                relativeSeconds,
                body.id, j, jointName(j),
                body.joints[j].x, body.joints[j].y, body.joints[j].z,
                body.joints[j].confidence);
        }
    }
}

void JointRecorder::writerLoop() {
    RecordingFrame frame;
    while (true) {
        bool gotData = buffer_.tryPop(frame);
        if (gotData) {
            writeFrame(frame);
            fflush(file_);
        } else {
            if (stopFlag_.load(std::memory_order_acquire))
                break;
            std::unique_lock<std::mutex> lock(cvMutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(50));
        }
    }

    // Drain remaining items
    while (buffer_.tryPop(frame)) {
        writeFrame(frame);
    }
}
