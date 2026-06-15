#include "camera_feed_window.h"
#include "../app_controller.h"
#include "../camera/camera_types.h"

#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QPixmap>
#include <QTimer>
#include <mutex>

CameraFeedWindow::CameraFeedWindow(AppController* ctrl, QWidget* parent)
    : QWidget(parent), ctrl_(ctrl) {
    setWindowTitle("Camera Feed");
    resize(1100, 480);

    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(10, 10, 10, 10);
    root->setSpacing(10);

    cam1Label_ = new QLabel("No camera 1 image");
    cam2Label_ = new QLabel("No camera 2 image");
    for (auto* label : {cam1Label_, cam2Label_}) {
        label->setAlignment(Qt::AlignCenter);
        label->setMinimumSize(360, 270);
        label->setStyleSheet("background:#101318; border:1px solid #2a2f3a; color:#9ba1a6;");
        root->addWidget(label, 1);
    }

    timer_ = new QTimer(this);
    timer_->setInterval(33);
    connect(timer_, &QTimer::timeout, this, &CameraFeedWindow::refreshFrames);
    timer_->start();
}

void CameraFeedWindow::refreshFrames() {
    if (!ctrl_) return;
    std::shared_ptr<FrameData> f0;
    std::shared_ptr<FrameData> f1;
    {
        auto& slot = ctrl_->slot(0);
        std::lock_guard<std::mutex> lock(slot.frameMutex);
        f0 = slot.sharedFrame;
    }
    {
        auto& slot = ctrl_->slot(1);
        std::lock_guard<std::mutex> lock(slot.frameMutex);
        f1 = slot.sharedFrame;
    }
    updateLabel(cam1Label_, f0, "No camera 1 image");
    updateLabel(cam2Label_, f1, "No camera 2 image");
}

void CameraFeedWindow::updateLabel(QLabel* label, const std::shared_ptr<FrameData>& frame,
                                   const QString& emptyText) {
    if (!label) return;
    if (!frame || frame->imageWidth <= 0 || frame->imageHeight <= 0 || frame->imageRGBA.empty()) {
        label->setText(emptyText);
        label->setPixmap(QPixmap());
        return;
    }

    QImage img(frame->imageRGBA.data(), frame->imageWidth, frame->imageHeight,
               frame->imageWidth * 4, QImage::Format_RGBA8888);
    QPixmap px = QPixmap::fromImage(img.copy());
    label->setPixmap(px.scaled(label->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
}
