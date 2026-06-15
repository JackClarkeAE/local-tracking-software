#include "video_recorder.h"

#if HAS_VIDEO_RECORDING
#include <QMediaCaptureSession>
#include <QMediaRecorder>
#include <QMediaFormat>
#include <QVideoFrameInput>
#include <QVideoFrame>
#include <QVideoFrameFormat>
#include <QUrl>
#include <QString>
#include <cstring>
#endif

VideoRecorder::VideoRecorder(QObject* parent) : QObject(parent) {}

VideoRecorder::~VideoRecorder() {
    stop();
}

void VideoRecorder::start(const std::string& outputPath,
                          std::chrono::steady_clock::time_point epoch) {
    if (active_) return;
    path_ = outputPath;
    epoch_ = epoch;
    epochOffsetUs_ = 0;
    active_ = true;
    configured_ = false;
    firstTimestampUs_ = 0;
}

void VideoRecorder::stop() {
    if (!active_) return;
    active_ = false;
#if HAS_VIDEO_RECORDING
    teardown();
#endif
}

#if HAS_VIDEO_RECORDING

void VideoRecorder::teardown() {
    if (recorder_) recorder_->stop();
    delete input_;    input_ = nullptr;
    delete recorder_; recorder_ = nullptr;
    delete session_;  session_ = nullptr;
    configured_ = false;
}

void VideoRecorder::pushFrame(const FrameData& frame) {
    if (!active_ || frame.imageWidth <= 0 || frame.imageHeight <= 0 || frame.imageRGBA.empty())
        return;

    if (!configured_) {
        session_ = new QMediaCaptureSession;
        recorder_ = new QMediaRecorder;
        input_ = new QVideoFrameInput;
        session_->setRecorder(recorder_);
        session_->setVideoFrameInput(input_);
        QObject::connect(recorder_, &QMediaRecorder::errorOccurred, this,
            [this](QMediaRecorder::Error, const QString& message) {
                emit recordingError(
                    "Camera footage recording failed: " + message +
                    " (requires a Qt build with the ffmpeg media backend)");
            });

        QMediaFormat format(QMediaFormat::MPEG4);
        format.setVideoCodec(QMediaFormat::VideoCodec::H264);
        recorder_->setMediaFormat(format);
        recorder_->setQuality(QMediaRecorder::HighQuality);
        recorder_->setOutputLocation(QUrl::fromLocalFile(QString::fromStdString(path_)));
        recorder_->record();
        configured_ = true;
        firstTimestampUs_ = frame.timestamp_us;
        const auto sinceEpoch = std::chrono::steady_clock::now() - epoch_;
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(sinceEpoch).count();
        epochOffsetUs_ = us > 0 ? (uint64_t)us : 0;
    }

    QVideoFrameFormat fmt(QSize(frame.imageWidth, frame.imageHeight),
                          QVideoFrameFormat::Format_RGBA8888);
    QVideoFrame videoFrame(fmt);
    if (!videoFrame.map(QVideoFrame::WriteOnly)) return;

    const size_t srcRow = (size_t)frame.imageWidth * 4;
    const int dstStride = videoFrame.bytesPerLine(0);
    uint8_t* dst = videoFrame.bits(0);
    for (int y = 0; y < frame.imageHeight; ++y)
        std::memcpy(dst + (size_t)y * dstStride,
                    frame.imageRGBA.data() + (size_t)y * srcRow, srcRow);
    videoFrame.unmap();

    // Same timeline as the joint CSV: shared-epoch offset + device-clock delta
    const qint64 relUs = (qint64)(epochOffsetUs_ + (frame.timestamp_us - firstTimestampUs_));
    videoFrame.setStartTime(relUs);
    videoFrame.setEndTime(relUs + 33000); // ~30 fps frame duration

    // sendVideoFrame returns false when the encoder queue is full — the
    // frame is simply dropped rather than blocking the GUI thread
    input_->sendVideoFrame(videoFrame);
}

#else // !HAS_VIDEO_RECORDING

void VideoRecorder::pushFrame(const FrameData&) {}

#endif
