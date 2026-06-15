#pragma once
#include <QWidget>

class SkeletonView;

class PopoutWindow : public QWidget {
    Q_OBJECT
public:
    explicit PopoutWindow(QWidget* parent = nullptr);

    SkeletonView* view() { return view_; }

protected:
    void keyPressEvent(QKeyEvent* e) override;

private:
    SkeletonView* view_;
    bool isFullscreen_ = false;
};
