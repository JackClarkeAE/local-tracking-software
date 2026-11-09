#include "biofeedback_dev_view.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSurfaceFormat>
#include <QVector4D>
#include <QWheelEvent>
#include <QtMath>
#include <algorithm>
#include <cmath>

namespace {

const QColor kBackground(15, 19, 26);
const QColor kLeftColor(232, 108, 108);    // gait-lab convention: left = red
const QColor kRightColor(96, 204, 142);    // right = green
const QColor kTrunkColor(188, 200, 214);

enum class BodySide { Trunk, Left, Right };

BodySide sideOfJoint(int j) {
    if ((j >= 4 && j <= 10) || (j >= 18 && j <= 21) || j == 28 || j == 29) return BodySide::Left;
    if ((j >= 11 && j <= 17) || (j >= 22 && j <= 25) || j == 30 || j == 31) return BodySide::Right;
    return BodySide::Trunk;
}

QColor boneColor(int a, int b) {
    const BodySide sa = sideOfJoint(a), sb = sideOfJoint(b);
    const BodySide s = (sa != BodySide::Trunk) ? sa : sb;
    // pelvis→hip and chest→clavicle bones belong to the trunk visually
    if ((a == 0 && (b == 18 || b == 22)) || (a == 2 && (b == 4 || b == 11))) return kTrunkColor;
    if (s == BodySide::Left) return kLeftColor;
    if (s == BodySide::Right) return kRightColor;
    return kTrunkColor;
}

void glColor(const QColor& c, float alpha) {
    glColor4f(c.redF(), c.greenF(), c.blueF(), alpha);
}

} // namespace

// Text/chips are painted on a transparent child widget with a raster QPainter,
// so they never depend on the GL state the skeleton rendering leaves behind
// (mixing fixed-function GL and QPainter on one surface fades or drops text
// on some drivers).
class BiofeedbackDevViewOverlay : public QWidget {
public:
    explicit BiofeedbackDevViewOverlay(BiofeedbackDevView* view) : QWidget(view), view_(view) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setAttribute(Qt::WA_TranslucentBackground);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        view_->drawOverlay(p);
    }

private:
    BiofeedbackDevView* view_;
};

BiofeedbackDevView::BiofeedbackDevView(QWidget* parent)
    : QOpenGLWidget(parent), camera_(std::make_shared<BfOrbitCamera>()) {
    QSurfaceFormat fmt = format();
    fmt.setDepthBufferSize(24);
    fmt.setSamples(4);
    setFormat(fmt);
    setMinimumSize(240, 180);
    setFocusPolicy(Qt::ClickFocus);
    setMouseTracking(false);
    overlay_ = new BiofeedbackDevViewOverlay(this);
    overlay_->setGeometry(rect());
}

void BiofeedbackDevView::refresh() {
    update();
    overlay_->update();
}

void BiofeedbackDevView::resizeEvent(QResizeEvent* e) {
    QOpenGLWidget::resizeEvent(e);
    overlay_->setGeometry(rect());
}

BiofeedbackDevView::~BiofeedbackDevView() = default;

void BiofeedbackDevView::setScene(Scene scene) {
    scene_ = std::move(scene);
    refresh();
}

void BiofeedbackDevView::setCamera(std::shared_ptr<BfOrbitCamera> camera) {
    camera_ = camera ? std::move(camera) : std::make_shared<BfOrbitCamera>();
    refresh();
}

void BiofeedbackDevView::setMirrorZ(bool mirror) {
    mirrorZ_ = mirror;
    refresh();
}

QVector3D BiofeedbackDevView::toDisplay(const bf::Vec3& v) const {
    return QVector3D(v.x, v.y, mirrorZ_ ? -v.z : v.z);
}

QMatrix4x4 BiofeedbackDevView::modelMatrix() const {
    QMatrix4x4 m;
    if (mirrorZ_) m.scale(1.0f, 1.0f, -1.0f);
    return m;
}

// Orthonormal display-space basis the orbit is defined in: `up` is the
// floor normal (camera Y unless the camera was tilted), fwd/right span the floor.
void BiofeedbackDevView::orbitBasis(QVector3D& up, QVector3D& fwd, QVector3D& right) const {
    up = toDisplay(bf::Vec3(camera_->up.x(), camera_->up.y(), camera_->up.z())).normalized();
    if (up.isNull()) up = QVector3D(0, 1, 0);
    fwd = QVector3D(0, 0, 1) - up * QVector3D::dotProduct(QVector3D(0, 0, 1), up);
    if (fwd.lengthSquared() < 1e-6f) fwd = QVector3D(1, 0, 0) - up * QVector3D::dotProduct(QVector3D(1, 0, 0), up);
    fwd.normalize();
    right = QVector3D::crossProduct(up, fwd);  // = +X when up = Y, fwd = Z
}

float BiofeedbackDevView::yawToSee(const bf::Vec3& dataDir) const {
    QVector3D up, fwd, right;
    orbitBasis(up, fwd, right);
    const QVector3D d = toDisplay(dataDir);
    return qRadiansToDegrees(std::atan2(QVector3D::dotProduct(d, right), QVector3D::dotProduct(d, fwd)));
}

QMatrix4x4 BiofeedbackDevView::viewMatrix() const {
    const BfOrbitCamera& c = *camera_;
    const QVector3D target = modelMatrix().map(c.target) + c.pan;
    const float yaw = qDegreesToRadians(c.yaw), pitch = qDegreesToRadians(c.pitch);
    QVector3D up, fwd, right;
    orbitBasis(up, fwd, right);
    const QVector3D dir = up * std::sin(pitch) + (fwd * std::cos(yaw) + right * std::sin(yaw)) * std::cos(pitch);
    QMatrix4x4 v;
    v.lookAt(target + dir * c.distance, target, up);
    return v;
}

QMatrix4x4 BiofeedbackDevView::projectionMatrix() const {
    QMatrix4x4 p;
    p.perspective(40.0f, std::max(0.1f, (float)width() / std::max(1, height())), 0.05f, 60.0f);
    return p;
}

bool BiofeedbackDevView::projectToScreen(const QVector3D& dataPos, QPointF& out) const {
    const QVector4D clip = projectionMatrix() * viewMatrix() * modelMatrix() * QVector4D(dataPos, 1.0f);
    if (clip.w() <= 1e-5f) return false;
    const float nx = clip.x() / clip.w(), ny = clip.y() / clip.w();
    out = QPointF((nx + 1.0f) * 0.5f * width(), (1.0f - ny) * 0.5f * height());
    return true;
}

void BiofeedbackDevView::initializeGL() {
    initializeOpenGLFunctions();
}

void BiofeedbackDevView::resizeGL(int, int) {
    // QOpenGLWidget sets the viewport (in device pixels) before paintGL
}

void BiofeedbackDevView::paintGL() {
    glClearColor(kBackground.redF(), kBackground.greenF(), kBackground.blueF(), 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (scene_.body) {
        const QMatrix4x4 proj = projectionMatrix();
        const QMatrix4x4 mv = viewMatrix() * modelMatrix();
        glMatrixMode(GL_PROJECTION);
        glLoadMatrixf(proj.constData());
        glMatrixMode(GL_MODELVIEW);
        glLoadMatrixf(mv.constData());

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glEnable(GL_LINE_SMOOTH);
        glEnable(GL_POINT_SMOOTH);
        glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);

        drawFloor(camera_->target);
        if (scene_.ghost) drawBody(*scene_.ghost, true);
        drawBody(*scene_.body, false);
        if (scene_.showAxes && scene_.axes.valid) drawAxes();

        glDisable(GL_DEPTH_TEST);
        glDisable(GL_LINE_SMOOTH);
        glDisable(GL_POINT_SMOOTH);
        glDisable(GL_BLEND);
        glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
        glLineWidth(1.0f);
        glPointSize(1.0f);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
    }
}

void BiofeedbackDevView::drawFloor(const QVector3D& target) {
    if (!scene_.hasFloor) return;
    // Grid on the fitted floor plane (tilted when the camera was pitched),
    // in data space; snapped to whole metres so it reads as fixed ground
    // while the camera follows the body.
    const bf::Vec3 up = bf::normalizeOr(scene_.floorUp, bf::Vec3(0, 1, 0));
    const bf::Vec3 e1 = bf::normalizeOr(bf::rejectFrom(bf::Vec3(1, 0, 0), up), bf::Vec3(0, 0, 1));
    const bf::Vec3 e2 = bf::cross(up, e1);
    const bf::Vec3 t(target.x(), target.y(), target.z());
    const float s1 = std::round(bf::dot(t, e1)), s2 = std::round(bf::dot(t, e2));
    const bf::Vec3 centre = up * scene_.floorHeight + e1 * s1 + e2 * s2;
    const float step = 0.25f, extent = 3.0f;
    const float dpr = (float)devicePixelRatioF();
    auto vtx = [](const bf::Vec3& p) { glVertex3f(p.x, p.y, p.z); };

    glDisable(GL_DEPTH_TEST);
    glLineWidth(1.0f * dpr);
    glBegin(GL_LINES);
    for (int k = -12; k <= 12; ++k) {
        const float o = k * step;
        const bool major = k % 4 == 0;
        glColor4f(0.62f, 0.70f, 0.80f, major ? 0.20f : 0.08f);
        vtx(centre + e1 * o - e2 * extent);
        vtx(centre + e1 * o + e2 * extent);
        vtx(centre - e1 * extent + e2 * o);
        vtx(centre + e1 * extent + e2 * o);
    }
    glEnd();
}

void BiofeedbackDevView::drawBody(const TrackedBody& body, bool ghost) {
    const auto& bones = getBoneConnections();
    const float dpr = (float)devicePixelRatioF();
    const float minConf = 0.1f;

    if (ghost) {
        glDisable(GL_DEPTH_TEST);
        glLineWidth(2.0f * dpr);
        glBegin(GL_LINES);
        for (const auto& [a, b] : bones) {
            const Joint& ja = body.joints[a];
            const Joint& jb = body.joints[b];
            if (ja.confidence < minConf || jb.confidence < minConf) continue;
            glColor4f(1.0f, 1.0f, 1.0f, 0.30f);
            glVertex3f(ja.x, ja.y, ja.z);
            glVertex3f(jb.x, jb.y, jb.z);
        }
        glEnd();
        return;
    }

    // Glow under the modified limb
    glDisable(GL_DEPTH_TEST);
    glLineWidth(10.0f * dpr);
    glBegin(GL_LINES);
    for (const auto& [a, b] : bones) {
        if (!scene_.highlight[a] || !scene_.highlight[b]) continue;
        const Joint& ja = body.joints[a];
        const Joint& jb = body.joints[b];
        if (ja.confidence < minConf || jb.confidence < minConf) continue;
        glColor(scene_.accent, 0.22f);
        glVertex3f(ja.x, ja.y, ja.z);
        glVertex3f(jb.x, jb.y, jb.z);
    }
    glEnd();

    glEnable(GL_DEPTH_TEST);
    for (int pass = 0; pass < 2; ++pass) {
        // pass 0: normal bones, pass 1: highlighted (thicker, on top)
        glLineWidth((pass == 0 ? 3.0f : 4.5f) * dpr);
        glBegin(GL_LINES);
        for (const auto& [a, b] : bones) {
            const bool hl = scene_.highlight[a] && scene_.highlight[b];
            if (hl != (pass == 1)) continue;
            const Joint& ja = body.joints[a];
            const Joint& jb = body.joints[b];
            if (ja.confidence < minConf || jb.confidence < minConf) continue;
            const float conf = std::min(ja.confidence, jb.confidence);
            QColor c = boneColor(a, b);
            if (hl) c = c.lighter(125);
            glColor(c, 0.45f + 0.55f * std::min(1.0f, conf));
            glVertex3f(ja.x, ja.y, ja.z);
            glVertex3f(jb.x, jb.y, jb.z);
        }
        glEnd();
    }

    glPointSize(7.0f * dpr);
    glBegin(GL_POINTS);
    for (int j = 0; j < JOINT_COUNT; ++j) {
        const Joint& jt = body.joints[j];
        if (jt.confidence < minConf) continue;
        if (j >= 9 && j <= 10) continue;    // hand tip / thumb clutter
        if (j >= 16 && j <= 17) continue;
        const QColor c = scene_.highlight[j] ? scene_.accent : QColor(236, 241, 247);
        glColor(c, 0.4f + 0.6f * std::min(1.0f, jt.confidence));
        glVertex3f(jt.x, jt.y, jt.z);
    }
    glEnd();
}

void BiofeedbackDevView::drawAxes() {
    const anat::BodyAxes& ax = scene_.axes;
    const float dpr = (float)devicePixelRatioF();
    const bf::Vec3 o = ax.origin;
    auto line = [](const bf::Vec3& a, const bf::Vec3& b) {
        glVertex3f(a.x, a.y, a.z);
        glVertex3f(b.x, b.y, b.z);
    };
    glDisable(GL_DEPTH_TEST);
    glLineWidth(2.5f * dpr);
    glBegin(GL_LINES);
    glColor4f(0.95f, 0.45f, 0.40f, 0.95f); line(o, o + ax.ml * 0.25f);   // subject's right
    glColor4f(0.45f, 0.90f, 0.55f, 0.95f); line(o, o + ax.up * 0.25f);   // up
    glColor4f(0.40f, 0.70f, 1.00f, 0.95f); line(o, o + ax.ant * 0.40f);  // anterior
    glEnd();
}

void BiofeedbackDevView::drawOverlay(QPainter& p) {
    p.setRenderHint(QPainter::Antialiasing, true);

    auto chip = [&](const QPointF& anchor, const QString& text, const QColor& color, bool bold) {
        QFont f = font();
        f.setPointSizeF(8.5);
        f.setBold(bold);
        p.setFont(f);
        const QFontMetrics fm(f);
        QRectF r(anchor, QSizeF(fm.horizontalAdvance(text) + 10, fm.height() + 4));
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(8, 11, 16, 200));
        p.drawRoundedRect(r, 4, 4);
        p.setPen(color);
        p.drawText(r, Qt::AlignCenter, text);
    };

    if (scene_.body) {
        for (const Label& l : scene_.labels) {
            if (l.joint < 0 || l.joint >= JOINT_COUNT) continue;
            const Joint& j = scene_.body->joints[l.joint];
            if (j.confidence < 0.1f) continue;
            QPointF s;
            if (!projectToScreen(QVector3D(j.x, j.y, j.z), s)) continue;
            chip(s + QPointF(10, -9), l.text, l.color, true);
        }
        if (scene_.showAxes && scene_.axes.valid) {
            const anat::BodyAxes& ax = scene_.axes;
            auto tag = [&](const bf::Vec3& v, const QString& t, const QColor& c) {
                QPointF s;
                const bf::Vec3 tip = ax.origin + v;
                if (projectToScreen(QVector3D(tip.x, tip.y, tip.z), s)) chip(s + QPointF(4, -8), t, c, false);
            };
            tag(ax.ant * 0.40f, "ANT", QColor(130, 185, 255));
            tag(ax.ml * 0.25f, "R", QColor(242, 130, 120));
        }
    }

    // Title block
    QFont tf = font();
    tf.setPointSizeF(9.0);
    tf.setBold(true);
    tf.setLetterSpacing(QFont::AbsoluteSpacing, 1.2);
    p.setFont(tf);
    const QFontMetrics tfm(tf);
    QFont sf = font();
    sf.setPointSizeF(8.0);
    const QFontMetrics sfm(sf);
    const int w = std::max(tfm.horizontalAdvance(scene_.title), sfm.horizontalAdvance(scene_.subtitle)) + 20;
    const int h = tfm.height() + (scene_.subtitle.isEmpty() ? 8 : sfm.height() + 10);
    const QRectF titleRect(10, 10, w, h);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(8, 11, 16, 200));
    p.drawRoundedRect(titleRect, 5, 5);
    p.setBrush(scene_.accent);
    p.drawRoundedRect(QRectF(titleRect.left(), titleRect.top() + 6, 3, titleRect.height() - 12), 1.5, 1.5);
    p.setPen(QColor(243, 247, 251));
    p.drawText(QRectF(titleRect.left() + 10, titleRect.top() + 4, w, tfm.height()), Qt::AlignLeft | Qt::AlignVCenter,
               scene_.title);
    if (!scene_.subtitle.isEmpty()) {
        p.setFont(sf);
        p.setPen(QColor(157, 168, 181));
        p.drawText(QRectF(titleRect.left() + 10, titleRect.top() + 6 + tfm.height(), w, sfm.height()),
                   Qt::AlignLeft | Qt::AlignVCenter, scene_.subtitle);
    }

    if (!scene_.body) {
        QFont ef = font();
        ef.setPointSizeF(10.0);
        p.setFont(ef);
        p.setPen(QColor(120, 132, 147));
        p.drawText(rect(), Qt::AlignCenter, scene_.emptyText.isEmpty() ? QString("No frame") : scene_.emptyText);
    } else {
        QFont hf = font();
        hf.setPointSizeF(7.5);
        p.setFont(hf);
        p.setPen(QColor(110, 122, 137));
        p.drawText(QRectF(10, height() - 22, width() - 20, 16), Qt::AlignLeft | Qt::AlignVCenter,
                   "Drag: orbit   Right-drag: pan   Wheel: zoom   Double-click: reset");
    }
}

void BiofeedbackDevView::mousePressEvent(QMouseEvent* e) {
    lastMouse_ = e->pos();
    dragButtons_ = e->buttons();
}

void BiofeedbackDevView::mouseMoveEvent(QMouseEvent* e) {
    const QPoint d = e->pos() - lastMouse_;
    lastMouse_ = e->pos();
    BfOrbitCamera& c = *camera_;
    if (e->buttons() & Qt::LeftButton) {
        c.yaw -= d.x() * 0.4f;
        c.pitch = std::clamp(c.pitch + d.y() * 0.4f, -89.0f, 89.0f);
    } else if (e->buttons() & (Qt::RightButton | Qt::MiddleButton)) {
        const QMatrix4x4 v = viewMatrix();
        const QVector3D right(v(0, 0), v(0, 1), v(0, 2));
        const QVector3D up(v(1, 0), v(1, 1), v(1, 2));
        const float k = c.distance * 0.0016f;
        c.pan += (-right * (float)d.x() + up * (float)d.y()) * k;
    } else {
        return;
    }
    refresh();
    emit cameraEdited();
}

void BiofeedbackDevView::mouseReleaseEvent(QMouseEvent*) {
    dragButtons_ = Qt::NoButton;
}

void BiofeedbackDevView::wheelEvent(QWheelEvent* e) {
    const float steps = e->angleDelta().y() / 120.0f;
    camera_->distance = std::clamp(camera_->distance * std::pow(0.9f, steps), 0.6f, 25.0f);
    refresh();
    emit cameraEdited();
    e->accept();
}

void BiofeedbackDevView::mouseDoubleClickEvent(QMouseEvent*) {
    emit resetRequested();
}
