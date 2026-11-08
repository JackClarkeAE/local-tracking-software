#pragma once
// EXPERIMENTAL — 3D skeleton viewport for the Biofeedback Development tab.
// Two of these sit side by side (original | modified). When linked they share
// one BfOrbitCamera, so orbiting/panning/zooming either moves both.

#include "../camera/camera_types.h"
#include "../biofeedback/anatomical_engine.h"

#include <QColor>
#include <QMatrix4x4>
#include <QOpenGLFunctions>
#include <QOpenGLWidget>
#include <QString>
#include <QVector3D>
#include <array>
#include <memory>
#include <vector>

struct BfOrbitCamera {
    float yaw = 0.0f;        // degrees, eye direction around world Y
    float pitch = 12.0f;     // degrees above the horizon
    float distance = 2.9f;   // metres from the target
    QVector3D pan;           // display-space offset added to the target
    QVector3D target;        // data-space point the camera orbits (pelvis when following)
};

class BiofeedbackDevViewOverlay;

class BiofeedbackDevView : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT
public:
    struct Label {
        int joint = -1;
        QString text;
        QColor color;
    };

    struct Scene {
        std::shared_ptr<const TrackedBody> body;    // drawn normally
        std::shared_ptr<const TrackedBody> ghost;   // drawn faint behind (optional)
        std::array<bool, JOINT_COUNT> highlight{};  // joints whose limb is being modified
        bool hasFloor = false;
        float floorY = 0.0f;
        bool showAxes = false;
        anat::BodyAxes axes;
        std::vector<Label> labels;
        QString title;
        QString subtitle;
        QString emptyText;
        QColor accent = QColor(242, 184, 75);
    };

    explicit BiofeedbackDevView(QWidget* parent = nullptr);
    ~BiofeedbackDevView() override;

    void setScene(Scene scene);
    void setCamera(std::shared_ptr<BfOrbitCamera> camera);
    std::shared_ptr<BfOrbitCamera> camera() const { return camera_; }

    // Kinect data is left-handed after its Y flip; mirroring Z for display
    // keeps the subject's left leg on their left.
    void setMirrorZ(bool mirror);
    bool mirrorZ() const { return mirrorZ_; }

    // Data-space direction → display-space (applies the mirror)
    QVector3D toDisplay(const bf::Vec3& v) const;

    // Repaint the 3D scene and its text overlay (e.g. after the shared camera moved)
    void refresh();

signals:
    void cameraEdited();
    void resetRequested();

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    friend class BiofeedbackDevViewOverlay;
    QMatrix4x4 viewMatrix() const;
    QMatrix4x4 projectionMatrix() const;
    QMatrix4x4 modelMatrix() const;
    bool projectToScreen(const QVector3D& dataPos, QPointF& out) const;

    void drawFloor(const QVector3D& displayTarget);
    void drawBody(const TrackedBody& body, bool ghost);
    void drawAxes();
    void drawOverlay(class QPainter& p);

    Scene scene_;
    std::shared_ptr<BfOrbitCamera> camera_;
    bool mirrorZ_ = false;
    QPoint lastMouse_;
    Qt::MouseButtons dragButtons_;
    QWidget* overlay_ = nullptr;
};
