#include "popout_window.h"
#include "skeleton_view.h"

#include <QVBoxLayout>
#include <QKeyEvent>
#include <QGuiApplication>
#include <QScreen>

PopoutWindow::PopoutWindow(QWidget* parent) : QWidget(parent, Qt::Window) {
    setWindowTitle("Patient Screen");
    setStyleSheet("background-color: #000;");
    resize(800, 1200);  // portrait by default, patient TV

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    view_ = new SkeletonView(this);
    view_->setMode(SkeletonView::Mode::AutoFit2D);
    layout->addWidget(view_);

    // Move to secondary screen if available
    auto screens = QGuiApplication::screens();
    if (screens.size() > 1) {
        auto* screen = screens[1];
        QRect geom = screen->geometry();
        move(geom.x() + 50, geom.y() + 50);
    }
}

void PopoutWindow::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_F11) {
        if (isFullscreen_) showNormal();
        else showFullScreen();
        isFullscreen_ = !isFullscreen_;
    } else if (e->key() == Qt::Key_Escape && isFullscreen_) {
        showNormal();
        isFullscreen_ = false;
    }
    QWidget::keyPressEvent(e);
}
