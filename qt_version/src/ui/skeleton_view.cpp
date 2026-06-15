#include "skeleton_view.h"
#include "../biofeedback/biofeedback_engine.h"

#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

SkeletonView::SkeletonView(QWidget* parent) : QOpenGLWidget(parent) {
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(200, 150);
}

SkeletonView::~SkeletonView() = default;

void SkeletonView::setFrame(std::shared_ptr<FrameData> frame) {
    {
        std::lock_guard<std::mutex> lock(frameMutex_);
        frame_ = frame;
        isOverlay_ = false;
    }
    update();
}

void SkeletonView::setOverlayFrames(std::shared_ptr<FrameData> f1, std::shared_ptr<FrameData> f2) {
    {
        std::lock_guard<std::mutex> lock(frameMutex_);
        frame_ = f1;
        overlayFrame2_ = f2;
        isOverlay_ = true;
    }
    update();
}

void SkeletonView::clearOverlay() {
    {
        std::lock_guard<std::mutex> lock(frameMutex_);
        overlayFrame2_.reset();
        isOverlay_ = false;
    }
    update();
}

void SkeletonView::initializeGL() {
    initializeOpenGLFunctions();
    glHint(GL_POINT_SMOOTH_HINT, GL_NICEST);
    glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);
}

void SkeletonView::resizeGL(int w, int h) {
    glViewport(0, 0, w, h);
}

void SkeletonView::paintGL() {
    glClearColor(0.09f, 0.094f, 0.125f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    std::shared_ptr<FrameData> f1, f2;
    bool overlay = false;
    {
        std::lock_guard<std::mutex> lock(frameMutex_);
        f1 = frame_;
        f2 = overlayFrame2_;
        overlay = isOverlay_;
    }

    if (!avatarVisible_) {
        QPainter painter(this);
        drawPainterOverlay(painter, f1);
        return;
    }
    if (!f1) {
        QPainter painter(this);
        drawPainterOverlay(painter, nullptr);
        return;
    }

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    if (mode_ == Mode::Orbit3D) {
        setupPerspective();
        drawGrid();
        if (f1 && !f1->bodies.empty()) {
            if (overlay && f2) {
                drawSkeletonColored(*f1, 0.3f, 1.0f, 0.4f);
                if (!f2->bodies.empty()) drawSkeletonColored(*f2, 0.3f, 0.6f, 1.0f);
            } else {
                drawSkeleton(*f1);
            }
        }
    } else {
        // AutoFit2D
        if (overlay && f2 && (!f1->bodies.empty() || !f2->bodies.empty())) {
            setupOrthoAutoFitDual(*f1, *f2);
            drawSkeletonColored(*f1, 0.3f, 1.0f, 0.4f);
            drawSkeletonColored(*f2, 0.3f, 0.6f, 1.0f);
        } else if (f1 && !f1->bodies.empty()) {
            setupOrthoAutoFit(*f1);
            drawSkeleton(*f1);
        }
    }

    glDisable(GL_BLEND);

    QPainter painter(this);
    drawPainterOverlay(painter, f1);
}

void SkeletonView::drawPainterOverlay(QPainter& painter, const std::shared_ptr<FrameData>& frame) {
    painter.setRenderHint(QPainter::Antialiasing, true);

    if (showJointAngles_ && frame && !frame->bodies.empty()) {
        const auto angles = BiofeedbackEngine::measureAngles(*frame);
        const auto projected = projectJointsToScreen();
        painter.setFont(QFont("Segoe UI", 10, QFont::DemiBold));
        for (const auto& m : angles) {
            const AngleDefinition& def = getAngleDefinition(m.angle);
            for (const auto& sj : projected) {
                if (!sj.valid || sj.jointIdx != def.vertexJoint) continue;
                const QString text = QString::number(m.rawDegrees, 'f', 0);
                const QRect textRect = painter.fontMetrics().boundingRect(text).adjusted(-4, -2, 4, 2);
                QPoint pos((int)sj.sx + 10, (int)sj.sy - textRect.height() / 2);
                QRect bg(pos, textRect.size());
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(0, 0, 0, 170));
                painter.drawRoundedRect(bg, 3, 3);
                painter.setPen(QColor(255, 220, 100));
                painter.drawText(bg, Qt::AlignCenter, text);
                break;
            }
        }
    }

    const OverlayState state = overlayState_;
    if (!state.text.empty()) {
        QFont font("Segoe UI", std::max(12, (int)(22.0f * state.textScale)), QFont::Bold);
        painter.setFont(font);
        const QString text = QString::fromStdString(state.text);
        QRect rect = painter.fontMetrics().boundingRect(QRect(0, 0, width(), height()),
                                                        Qt::AlignCenter | Qt::TextWordWrap, text)
                         .adjusted(-18, -10, 18, 10);
        int y = 28;
        if (state.textPosition == TextPosition::Middle) y = (height() - rect.height()) / 2;
        else if (state.textPosition == TextPosition::Bottom) y = height() - rect.height() - 32;
        rect.moveTop(y);
        rect.moveLeft((width() - rect.width()) / 2);

        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 150));
        painter.drawRoundedRect(rect, 8, 8);
        painter.setPen(state.textColor);
        painter.drawText(rect, Qt::AlignCenter | Qt::TextWordWrap, text);
    }

    if (state.shape != OverlayShape::None) {
        const QPointF c(width() * 0.5, height() * 0.5);
        const qreal size = std::min(width(), height()) * 0.18;
        QColor color(255, 80, 80, 210);
        if (state.shape == OverlayShape::BlueSquare) color = QColor(80, 130, 255, 210);
        else if (state.shape == OverlayShape::GreenTriangle) color = QColor(80, 230, 80, 210);

        painter.setPen(QPen(color.lighter(130), 4));
        painter.setBrush(QColor(color.red(), color.green(), color.blue(), 70));
        if (state.shape == OverlayShape::RedCircle) {
            painter.drawEllipse(c, size * 0.5, size * 0.5);
        } else if (state.shape == OverlayShape::BlueSquare) {
            painter.drawRect(QRectF(c.x() - size * 0.5, c.y() - size * 0.5, size, size));
        } else if (state.shape == OverlayShape::GreenTriangle) {
            QPolygonF tri;
            tri << QPointF(c.x(), c.y() - size * 0.58)
                << QPointF(c.x() - size * 0.58, c.y() + size * 0.48)
                << QPointF(c.x() + size * 0.58, c.y() + size * 0.48);
            painter.drawPolygon(tri);
        }
    }
}

void SkeletonView::setupOrthoAutoFit(const FrameData& frame) {
    // Find bounds
    float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
    for (auto& body : frame.bodies) {
        for (int j = 0; j < JOINT_COUNT; ++j) {
            auto& jt = body.joints[j];
            if (jt.confidence < 0.1f) continue;
            if (jt.x < minX) minX = jt.x;
            if (jt.x > maxX) maxX = jt.x;
            if (jt.y < minY) minY = jt.y;
            if (jt.y > maxY) maxY = jt.y;
        }
    }
    if (minX >= maxX || minY >= maxY) {
        minX = -1.5f; maxX = 1.5f; minY = -1.5f; maxY = 1.5f;
    }
    float rangeX = maxX - minX, rangeY = maxY - minY;
    float padX = std::max(rangeX * 0.2f, 0.2f);
    float padY = std::max(rangeY * 0.2f, 0.2f);
    minX -= padX; maxX += padX; minY -= padY; maxY += padY;

    float targetCx = (minX + maxX) * 0.5f;
    float targetCy = (minY + maxY) * 0.5f;
    float targetHalfW = (maxX - minX) * 0.5f;
    float targetHalfH = (maxY - minY) * 0.5f;
    float aspect = (float)width() / (float)height();
    if (targetHalfW / targetHalfH > aspect) targetHalfH = targetHalfW / aspect;
    else targetHalfW = targetHalfH * aspect;

    // Smooth
    float s = 0.85f;
    if (!boundsInitialized_) {
        smoothCx_ = targetCx; smoothCy_ = targetCy;
        smoothHalfW_ = targetHalfW; smoothHalfH_ = targetHalfH;
        boundsInitialized_ = true;
    } else {
        smoothCx_ = s * smoothCx_ + (1.0f - s) * targetCx;
        smoothCy_ = s * smoothCy_ + (1.0f - s) * targetCy;
        smoothHalfW_ = s * smoothHalfW_ + (1.0f - s) * targetHalfW;
        smoothHalfH_ = s * smoothHalfH_ + (1.0f - s) * targetHalfH;
    }

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(smoothCx_ - smoothHalfW_, smoothCx_ + smoothHalfW_,
            smoothCy_ - smoothHalfH_, smoothCy_ + smoothHalfH_, -10, 10);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

void SkeletonView::setupOrthoAutoFitDual(const FrameData& a, const FrameData& b) {
    float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
    for (const auto* fd : {&a, &b}) {
        for (auto& body : fd->bodies) {
            for (int j = 0; j < JOINT_COUNT; ++j) {
                auto& jt = body.joints[j];
                if (jt.confidence < 0.1f) continue;
                if (jt.x < minX) minX = jt.x;
                if (jt.x > maxX) maxX = jt.x;
                if (jt.y < minY) minY = jt.y;
                if (jt.y > maxY) maxY = jt.y;
            }
        }
    }
    if (minX >= maxX || minY >= maxY) {
        minX = -1.5f; maxX = 1.5f; minY = -1.5f; maxY = 1.5f;
    }
    float rangeX = maxX - minX, rangeY = maxY - minY;
    float padX = std::max(rangeX * 0.2f, 0.2f);
    float padY = std::max(rangeY * 0.2f, 0.2f);
    minX -= padX; maxX += padX; minY -= padY; maxY += padY;

    float cx = (minX + maxX) * 0.5f;
    float cy = (minY + maxY) * 0.5f;
    float hw = (maxX - minX) * 0.5f;
    float hh = (maxY - minY) * 0.5f;
    float aspect = (float)width() / (float)height();
    if (hw / hh > aspect) hh = hw / aspect;
    else hw = hh * aspect;

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(cx - hw, cx + hw, cy - hh, cy + hh, -10, 10);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
}

void SkeletonView::setupPerspective() {
    // Compute orbit center from current frame
    float centerX = 0, centerY = 0, centerZ = 0;
    std::shared_ptr<FrameData> f;
    { std::lock_guard<std::mutex> lock(frameMutex_); f = frame_; }
    if (f && !f->bodies.empty()) {
        int count = 0;
        for (auto& body : f->bodies) {
            for (int j = 0; j < JOINT_COUNT; ++j) {
                if (body.joints[j].confidence < 0.1f) continue;
                centerX += body.joints[j].x;
                centerY += body.joints[j].y;
                centerZ += body.joints[j].z;
                count++;
            }
        }
        if (count > 0) { centerX /= count; centerY /= count; centerZ /= count; }
    }

    double aspect = (double)width() / height();
    double fov = 45.0 / camera_.zoom;
    double tanFov = tan(fov * M_PI / 360.0);
    double nearP = 0.1, farP = 50.0;
    double top = nearP * tanFov;
    double right = top * aspect;

    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glFrustum(-right, right, -top, top, nearP, farP);

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(camera_.panX, camera_.panY, -5.0f);
    glRotatef(camera_.rotX, 1, 0, 0);
    glRotatef(camera_.rotY, 0, 1, 0);
    glTranslatef(-centerX, -centerY, -centerZ);
}

void SkeletonView::drawGrid() {
    glLineWidth(1.0f);
    glBegin(GL_LINES);
    glColor4f(0.3f, 0.3f, 0.3f, 0.5f);
    for (int i = -5; i <= 5; i++) {
        glVertex3f((float)i, -1.5f, -5.0f);
        glVertex3f((float)i, -1.5f,  5.0f);
        glVertex3f(-5.0f, -1.5f, (float)i);
        glVertex3f( 5.0f, -1.5f, (float)i);
    }
    glEnd();

    glLineWidth(2.0f);
    glBegin(GL_LINES);
    glColor3f(1, 0, 0); glVertex3f(0, -1.5f, 0); glVertex3f(0.3f, -1.5f, 0);
    glColor3f(0, 1, 0); glVertex3f(0, -1.5f, 0); glVertex3f(0, -1.2f, 0);
    glColor3f(0, 0, 1); glVertex3f(0, -1.5f, 0); glVertex3f(0, -1.5f, 0.3f);
    glEnd();
}

void SkeletonView::drawSkeleton(const FrameData& frame) {
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_POINT_SMOOTH);
    glEnable(GL_LINE_SMOOTH);
    glEnable(GL_DEPTH_TEST);
    const auto& bones = getBoneConnections();
    for (auto& body : frame.bodies) {
        glLineWidth(3.0f);
        glBegin(GL_LINES);
        for (auto& [a, b] : bones) {
            if (a >= JOINT_COUNT || b >= JOINT_COUNT) continue;
            auto& ja = body.joints[a];
            auto& jb = body.joints[b];
            if (ja.confidence < 0.1f || jb.confidence < 0.1f) continue;
            float avg = (ja.confidence + jb.confidence) * 0.5f;
            glColor4f(1.0f - avg, avg, 0.2f, 0.8f);
            glVertex3f(ja.x, ja.y, ja.z);
            glVertex3f(jb.x, jb.y, jb.z);
        }
        glEnd();

        glPointSize(8.0f);
        glBegin(GL_POINTS);
        for (int j = 0; j < JOINT_COUNT; ++j) {
            auto& joint = body.joints[j];
            if (joint.confidence < 0.1f) continue;
            glColor4f(1.0f - joint.confidence, joint.confidence, 0.3f, 1.0f);
            glVertex3f(joint.x, joint.y, joint.z);
        }
        glEnd();
    }
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LINE_SMOOTH);
    glDisable(GL_POINT_SMOOTH);
    glDisable(GL_BLEND);
}

void SkeletonView::drawSkeletonColored(const FrameData& frame, float r, float g, float b) {
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_POINT_SMOOTH);
    glEnable(GL_LINE_SMOOTH);
    glEnable(GL_DEPTH_TEST);
    const auto& bones = getBoneConnections();
    for (auto& body : frame.bodies) {
        glLineWidth(3.0f);
        glBegin(GL_LINES);
        for (auto& [ai, bi] : bones) {
            if (ai >= JOINT_COUNT || bi >= JOINT_COUNT) continue;
            auto& ja = body.joints[ai];
            auto& jb = body.joints[bi];
            if (ja.confidence < 0.1f || jb.confidence < 0.1f) continue;
            glColor4f(r * 0.7f, g * 0.7f, b * 0.7f, 0.8f);
            glVertex3f(ja.x, ja.y, ja.z);
            glVertex3f(jb.x, jb.y, jb.z);
        }
        glEnd();

        glPointSize(8.0f);
        glBegin(GL_POINTS);
        for (int j = 0; j < JOINT_COUNT; ++j) {
            auto& joint = body.joints[j];
            if (joint.confidence < 0.1f) continue;
            glColor4f(r, g, b, 1.0f);
            glVertex3f(joint.x, joint.y, joint.z);
        }
        glEnd();
    }
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_LINE_SMOOTH);
    glDisable(GL_POINT_SMOOTH);
    glDisable(GL_BLEND);
}

void SkeletonView::mousePressEvent(QMouseEvent* e) {
    lastMousePos_ = e->pos();
    lastMouseButton_ = e->button();
}

void SkeletonView::mouseMoveEvent(QMouseEvent* e) {
    if (mode_ == Mode::AutoFit2D && e->buttons() == Qt::NoButton) {
        const auto joints = projectJointsToScreen();
        const ScreenJoint* best = nullptr;
        double bestDist = 18.0;
        for (const auto& sj : joints) {
            if (!sj.valid) continue;
            const double dx = sj.sx - e->position().x();
            const double dy = sj.sy - e->position().y();
            const double dist = std::sqrt(dx * dx + dy * dy);
            if (dist < bestDist) {
                bestDist = dist;
                best = &sj;
            }
        }
        if (best) {
            std::shared_ptr<FrameData> f;
            { std::lock_guard<std::mutex> lock(frameMutex_); f = frame_; }
            if (f && best->bodyIdx >= 0 && best->bodyIdx < (int)f->bodies.size()) {
                const auto& j = f->bodies[best->bodyIdx].joints[best->jointIdx];
                QToolTip::showText(e->globalPosition().toPoint(),
                    QString("%1\nx %2  y %3  z %4\nconfidence %5")
                        .arg(jointName(best->jointIdx))
                        .arg(j.x, 0, 'f', 3)
                        .arg(j.y, 0, 'f', 3)
                        .arg(j.z, 0, 'f', 3)
                        .arg(j.confidence, 0, 'f', 2),
                    this);
            }
        } else {
            QToolTip::hideText();
        }
    }

    if (mode_ != Mode::Orbit3D) return;
    QPoint delta = e->pos() - lastMousePos_;
    if (e->buttons() & Qt::LeftButton) {
        camera_.rotY += delta.x() * 0.5f;
        camera_.rotX = std::clamp(camera_.rotX + delta.y() * 0.5f, -89.0f, 89.0f);
        emit cameraChanged();
        update();
    } else if (e->buttons() & Qt::RightButton) {
        camera_.panX += delta.x() * 0.005f;
        camera_.panY -= delta.y() * 0.005f;
        emit cameraChanged();
        update();
    }
    lastMousePos_ = e->pos();
}

void SkeletonView::wheelEvent(QWheelEvent* e) {
    if (mode_ != Mode::Orbit3D) return;
    float dy = e->angleDelta().y() / 120.0f;
    camera_.zoom = std::clamp(camera_.zoom * (1.0f + dy * 0.1f), 0.1f, 10.0f);
    emit cameraChanged();
    update();
}

void SkeletonView::mouseDoubleClickEvent(QMouseEvent* e) {
    if (e->button() == Qt::MiddleButton) {
        camera_.reset();
        emit cameraChanged();
        update();
    }
}

std::vector<ScreenJoint> SkeletonView::projectJointsToScreen() const {
    std::vector<ScreenJoint> result;
    std::shared_ptr<FrameData> f;
    { std::lock_guard<std::mutex> lock(frameMutex_); f = frame_; }
    if (!f || width() <= 0 || height() <= 0) return result;

    const float left = smoothCx_ - smoothHalfW_;
    const float right = smoothCx_ + smoothHalfW_;
    const float bottom = smoothCy_ - smoothHalfH_;
    const float top = smoothCy_ + smoothHalfH_;
    const float rw = std::max(0.0001f, right - left);
    const float rh = std::max(0.0001f, top - bottom);

    for (int bi = 0; bi < (int)f->bodies.size(); ++bi) {
        const auto& body = f->bodies[bi];
        for (int ji = 0; ji < JOINT_COUNT; ++ji) {
            const auto& j = body.joints[ji];
            ScreenJoint sj;
            sj.bodyIdx = bi;
            sj.jointIdx = ji;
            sj.valid = j.confidence >= 0.1f;
            if (sj.valid) {
                sj.sx = ((j.x - left) / rw) * width();
                sj.sy = height() - ((j.y - bottom) / rh) * height();
            }
            result.push_back(sj);
        }
    }
    return result;
}
