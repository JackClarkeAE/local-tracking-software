#pragma once

#include <QWidget>
#include <memory>

class AppController;
class QLabel;
class QTimer;
struct FrameData;

class CameraFeedWindow : public QWidget {
    Q_OBJECT
public:
    explicit CameraFeedWindow(AppController* ctrl, QWidget* parent = nullptr);

private slots:
    void refreshFrames();

private:
    void updateLabel(QLabel* label, const std::shared_ptr<FrameData>& frame,
                     const QString& emptyText);

    AppController* ctrl_ = nullptr;
    QLabel* cam1Label_ = nullptr;
    QLabel* cam2Label_ = nullptr;
    QTimer* timer_ = nullptr;
};
