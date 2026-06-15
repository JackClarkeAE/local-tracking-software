#include "webcam_camera.h"

#include <QCamera>
#include <QCameraDevice>
#include <QMediaCaptureSession>
#include <QMediaDevices>
#include <QVideoSink>
#include <QVideoFrame>
#include <QImage>
#include <QObject>
#include <chrono>
#include <cstring>

WebcamCamera::~WebcamCamera() {
    close();
}

bool WebcamCamera::open(const CameraConfig& config) {
    // Camera permission is checked (and requested) by AppController before
    // this is called, so the device can be opened directly here.
    const auto devices = QMediaDevices::videoInputs();
    if (devices.isEmpty()) return false;

    int idx = config.rgbDeviceIndex;
    if (idx < 0 || idx >= devices.size()) idx = 0;
    const QCameraDevice& device = devices[idx];
    name_ = "RGB Camera (" + device.description().toStdString() + ")";

    camera_ = new QCamera(device);
    session_ = new QMediaCaptureSession;
    sink_ = new QVideoSink;
    session_->setCamera(camera_);
    session_->setVideoSink(sink_);

    QObject::connect(sink_, &QVideoSink::videoFrameChanged, sink_,
        [this](const QVideoFrame& frame) {
            if (!frame.isValid()) return;
            QImage img = frame.toImage();
            if (img.isNull()) return;
            if (img.format() != QImage::Format_RGBA8888)
                img = img.convertToFormat(QImage::Format_RGBA8888);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                width_ = img.width();
                height_ = img.height();
                const size_t rowBytes = (size_t)width_ * 4;
                rgba_.resize(rowBytes * height_);
                // copy row by row — QImage scanlines may be padded
                for (int y = 0; y < height_; ++y)
                    std::memcpy(rgba_.data() + (size_t)y * rowBytes,
                                img.constScanLine(y), rowBytes);
                frameCounter_++;
            }
            cv_.notify_all();
        });

    camera_->start();
    open_.store(true);
    return true;
}

void WebcamCamera::close() {
    open_.store(false);
    if (camera_) camera_->stop();
    if (session_) {
        session_->setCamera(nullptr);
        session_->setVideoSink(nullptr);
    }
    delete sink_;    sink_ = nullptr;
    delete session_; session_ = nullptr;
    delete camera_;  camera_ = nullptr;
    cv_.notify_all();
}

bool WebcamCamera::grabFrame(FrameData& out) {
    std::unique_lock<std::mutex> lock(mutex_);
    const bool gotFrame = cv_.wait_for(lock, std::chrono::milliseconds(500),
        [this] { return frameCounter_ != lastDelivered_ || !open_.load(); });
    if (!gotFrame || !open_.load() || frameCounter_ == lastDelivered_) return false;

    lastDelivered_ = frameCounter_;
    out.imageWidth = width_;
    out.imageHeight = height_;
    out.imageRGBA = rgba_;
    out.bodies.clear();
    out.timestamp_us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return true;
}
