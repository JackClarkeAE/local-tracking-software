#include "camera_feed_view.h"
#include "../camera/camera_types.h"

#include <QPainter>
#include <QImage>

CameraFeedView::CameraFeedView(QWidget* parent) : QWidget(parent) {
    setMinimumSize(160, 120);
}

void CameraFeedView::setFrame(std::shared_ptr<FrameData> frame) {
    frame_ = std::move(frame);
    update();
}

void CameraFeedView::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(12, 14, 18));

    if (!frame_ || frame_->imageWidth <= 0 || frame_->imageHeight <= 0 ||
        frame_->imageRGBA.empty()) {
        p.setPen(QColor(120, 130, 140));
        p.drawText(rect(), Qt::AlignCenter, "No camera feed");
        return;
    }

    const QImage img(frame_->imageRGBA.data(), frame_->imageWidth, frame_->imageHeight,
                     frame_->imageWidth * 4, QImage::Format_RGBA8888);
    const QSize scaled = img.size().scaled(size(), Qt::KeepAspectRatio);
    const QRect target(QPoint((width() - scaled.width()) / 2,
                              (height() - scaled.height()) / 2), scaled);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawImage(target, img);
}
