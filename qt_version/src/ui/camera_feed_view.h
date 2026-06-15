#pragma once
#include <QWidget>
#include <memory>

struct FrameData;

// Renders the camera image stream inline in the Live tab — used as the
// main view for RGB cameras, which have no skeleton to draw.
class CameraFeedView : public QWidget {
    Q_OBJECT
public:
    explicit CameraFeedView(QWidget* parent = nullptr);

    void setFrame(std::shared_ptr<FrameData> frame);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    std::shared_ptr<FrameData> frame_;
};
