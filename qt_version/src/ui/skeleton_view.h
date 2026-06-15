#pragma once
#include "../camera/camera_types.h"
#include "overlay_state.h"
#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <memory>
#include <mutex>

struct Camera3D {
    float rotX = 0.0f;
    float rotY = 0.0f;
    float panX = 0.0f;
    float panY = 0.0f;
    float zoom = 1.0f;
    void reset() { rotX = 0; rotY = 0; panX = 0; panY = 0; zoom = 1.0f; }
};

struct ScreenJoint {
    float sx, sy;
    int bodyIdx, jointIdx;
    bool valid = false;
};

// SkeletonView: a Qt OpenGL widget that renders body-tracking skeletons.
// Supports two rendering modes:
//   - Mode::AutoFit2D — orthographic, auto-fits the skeleton bounding box (live view)
//   - Mode::Orbit3D   — 3D perspective with camera controls (playback)
class SkeletonView : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT
public:
    enum class Mode { AutoFit2D, Orbit3D };

    explicit SkeletonView(QWidget* parent = nullptr);
    ~SkeletonView() override;

    void setMode(Mode m) { mode_ = m; }
    Mode mode() const { return mode_; }

    // Set frame data (thread-safe, will trigger update)
    void setFrame(std::shared_ptr<FrameData> frame);
    // Set both frames for dual-camera overlay mode
    void setOverlayFrames(std::shared_ptr<FrameData> f1, std::shared_ptr<FrameData> f2);
    void clearOverlay();

    // Camera3D for orbit mode (read-write reference)
    Camera3D& camera() { return camera_; }
    const Camera3D& camera() const { return camera_; }

    // Avatar visibility (hides skeleton but keeps overlays)
    void setAvatarVisible(bool v) { avatarVisible_ = v; update(); }
    void setOverlayState(const OverlayState& s) { overlayState_ = s; update(); }
    void setShowJointAngles(bool v) { showJointAngles_ = v; update(); }

    // Access smoothed bounds (for tooltips)
    std::vector<ScreenJoint> projectJointsToScreen() const;

signals:
    void cameraChanged();

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

    // Mouse interaction for Orbit3D mode
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;

private:
    void drawSkeleton(const FrameData& frame);
    void drawSkeletonColored(const FrameData& frame, float r, float g, float b);
    void drawPainterOverlay(class QPainter& painter, const std::shared_ptr<FrameData>& frame);
    void drawGrid();
    void setupOrthoAutoFit(const FrameData& frame);
    void setupOrthoAutoFitDual(const FrameData& a, const FrameData& b);
    void setupPerspective();

    Mode mode_ = Mode::AutoFit2D;
    std::shared_ptr<FrameData> frame_;
    std::shared_ptr<FrameData> overlayFrame2_;
    bool isOverlay_ = false;
    mutable std::mutex frameMutex_;

    Camera3D camera_;
    bool avatarVisible_ = true;
    OverlayState overlayState_;
    bool showJointAngles_ = false;

    // Smoothed ortho bounds
    float smoothCx_ = 0, smoothCy_ = 0;
    float smoothHalfW_ = 1.5f, smoothHalfH_ = 1.5f;
    bool boundsInitialized_ = false;

    // Mouse tracking
    QPoint lastMousePos_;
    Qt::MouseButton lastMouseButton_ = Qt::NoButton;
};
