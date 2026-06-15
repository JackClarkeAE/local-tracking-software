#pragma once
#include <QObject>
#include <QtCore/qtconfigmacros.h>
#include <string>
#include <cstdint>
#include <chrono>

#include "../camera/camera_types.h"

// Video frame pushing via QVideoFrameInput needs Qt 6.6+
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
#define HAS_VIDEO_RECORDING 1
#else
#define HAS_VIDEO_RECORDING 0
#endif

class QMediaCaptureSession;
class QMediaRecorder;
class QVideoFrameInput;

// Records the camera image stream (RGB webcam or depth-camera colour feed)
// to an MP4 file alongside the joint CSV, so footage can be reused for
// testing other tracking models later.
//
// Frames are pushed from the GUI thread (AppController::onTick); the
// encoder runs on Qt Multimedia's own threads. Encoding is configured
// lazily on the first frame, when the resolution is known.
class VideoRecorder : public QObject {
    Q_OBJECT
public:
    explicit VideoRecorder(QObject* parent = nullptr);
    ~VideoRecorder() override;

signals:
    // Emitted when the encoder backend reports a failure (e.g. a Qt build
    // without the ffmpeg media backend, or disk full)
    void recordingError(QString message);

public:

    static bool supported() { return HAS_VIDEO_RECORDING != 0; }

    // epoch: shared session reference time (see JointRecorder::start) — the
    // MP4's frame timestamps then line up with the joint CSV's time_seconds.
    void start(const std::string& outputPath,
               std::chrono::steady_clock::time_point epoch = std::chrono::steady_clock::now());
    void stop();
    bool isRecording() const { return active_; }

    // GUI thread only. Drops the frame if the encoder queue is full.
    void pushFrame(const FrameData& frame);

private:
    bool active_ = false;
    bool configured_ = false;
    std::string path_;
    uint64_t firstTimestampUs_ = 0;
    std::chrono::steady_clock::time_point epoch_{};
    uint64_t epochOffsetUs_ = 0;

#if HAS_VIDEO_RECORDING
    QMediaCaptureSession* session_ = nullptr;
    QMediaRecorder* recorder_ = nullptr;
    QVideoFrameInput* input_ = nullptr;
    void teardown();
#endif
};
