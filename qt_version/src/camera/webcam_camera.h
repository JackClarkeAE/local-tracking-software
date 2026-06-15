#pragma once
#include "icamera.h"
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <vector>
#include <cstdint>

class QCamera;
class QMediaCaptureSession;
class QVideoSink;

// Standard RGB camera (webcam / USB camera) via Qt Multimedia.
// Provides a live video feed only — no skeleton tracking, so FrameData
// carries the image with an empty body list.
//
// Qt camera objects need a thread with an event loop, so open()/close()
// must be called from the main thread (which startCamera/stopCamera do).
// grabFrame() is called from the tracking thread and blocks until the
// next frame arrives.
class WebcamCamera : public ICamera {
public:
    ~WebcamCamera() override;
    bool open(const CameraConfig& config) override;
    void close() override;
    bool isOpen() const override { return open_.load(); }
    bool grabFrame(FrameData& out) override;
    std::string cameraName() const override { return name_; }

private:
    QCamera* camera_ = nullptr;
    QMediaCaptureSession* session_ = nullptr;
    QVideoSink* sink_ = nullptr;
    std::string name_ = "RGB Camera";
    std::atomic<bool> open_{false};

    // Latest frame, written by the video sink callback (main thread),
    // read by grabFrame (tracking thread)
    std::mutex mutex_;
    std::condition_variable cv_;
    uint64_t frameCounter_ = 0;
    uint64_t lastDelivered_ = 0;
    int width_ = 0, height_ = 0;
    std::vector<uint8_t> rgba_;
};
